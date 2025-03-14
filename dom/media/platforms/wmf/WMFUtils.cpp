/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WMFUtils.h"
#include "VideoUtils.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/Logging.h"
#include "mozilla/RefPtr.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"
#include "nsWindowsHelpers.h"
#include <initguid.h>
#include <stdint.h>

#ifdef WMF_MUST_DEFINE_AAC_MFT_CLSID
// Some SDK versions don't define the AAC decoder CLSID.
// {32D186A7-218F-4C75-8876-DD77273A8999}
DEFINE_GUID(CLSID_CMSAACDecMFT, 0x32D186A7, 0x218F, 0x4C75, 0x88, 0x76, 0xDD, 0x77, 0x27, 0x3A, 0x89, 0x99);
#endif

namespace mozilla {

HRESULT
HNsToFrames(int64_t aHNs, uint32_t aRate, int64_t* aOutFrames)
{
  MOZ_ASSERT(aOutFrames);
  const int64_t HNS_PER_S = USECS_PER_S * 10;
  CheckedInt<int64_t> i = aHNs;
  i *= aRate;
  i /= HNS_PER_S;
  NS_ENSURE_TRUE(i.isValid(), E_FAIL);
  *aOutFrames = i.value();
  return S_OK;
}

HRESULT
GetDefaultStride(IMFMediaType *aType, uint32_t aWidth, uint32_t* aOutStride)
{
  // Try to get the default stride from the media type.
  HRESULT hr = aType->GetUINT32(MF_MT_DEFAULT_STRIDE, aOutStride);
  if (SUCCEEDED(hr)) {
    return S_OK;
  }

  // Stride attribute not set, calculate it.
  GUID subtype = GUID_NULL;

  hr = aType->GetGUID(MF_MT_SUBTYPE, &subtype);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = wmf::MFGetStrideForBitmapInfoHeader(
    subtype.Data1, aWidth, (LONG*)(aOutStride));
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  return hr;
}

int32_t
MFOffsetToInt32(const MFOffset& aOffset)
{
  return int32_t(aOffset.value + (aOffset.fract / 65536.0f));
}

media::TimeUnit
GetSampleDuration(IMFSample* aSample)
{
  NS_ENSURE_TRUE(aSample, media::TimeUnit::Invalid());
  int64_t duration = 0;
  aSample->GetSampleDuration(&duration);
  return media::TimeUnit::FromMicroseconds(HNsToUsecs(duration));
}

media::TimeUnit
GetSampleTime(IMFSample* aSample)
{
  NS_ENSURE_TRUE(aSample, media::TimeUnit::Invalid());
  LONGLONG timestampHns = 0;
  HRESULT hr = aSample->GetSampleTime(&timestampHns);
  NS_ENSURE_TRUE(SUCCEEDED(hr), media::TimeUnit::Invalid());
  return media::TimeUnit::FromMicroseconds(HNsToUsecs(timestampHns));
}

// Gets the sub-region of the video frame that should be displayed.
// See: http://msdn.microsoft.com/en-us/library/windows/desktop/bb530115(v=vs.85).aspx
HRESULT
GetPictureRegion(IMFMediaType* aMediaType, nsIntRect& aOutPictureRegion)
{
  // Determine if "pan and scan" is enabled for this media. If it is, we
  // only display a region of the video frame, not the entire frame.
  BOOL panScan =
    MFGetAttributeUINT32(aMediaType, MF_MT_PAN_SCAN_ENABLED, FALSE);

  // If pan and scan mode is enabled. Try to get the display region.
  HRESULT hr = E_FAIL;
  MFVideoArea videoArea;
  memset(&videoArea, 0, sizeof(MFVideoArea));
  if (panScan) {
    hr = aMediaType->GetBlob(MF_MT_PAN_SCAN_APERTURE,
                             (UINT8*)&videoArea,
                             sizeof(MFVideoArea),
                             nullptr);
  }

  // If we're not in pan-and-scan mode, or the pan-and-scan region is not set,
  // check for a minimimum display aperture.
  if (!panScan || hr == MF_E_ATTRIBUTENOTFOUND) {
    hr = aMediaType->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                             (UINT8*)&videoArea,
                             sizeof(MFVideoArea),
                             nullptr);
  }

  if (hr == MF_E_ATTRIBUTENOTFOUND) {
    // Minimum display aperture is not set, for "backward compatibility with
    // some components", check for a geometric aperture.
    hr = aMediaType->GetBlob(MF_MT_GEOMETRIC_APERTURE,
                             (UINT8*)&videoArea,
                             sizeof(MFVideoArea),
                             nullptr);
  }

  if (SUCCEEDED(hr)) {
    // The media specified a picture region, return it.
    aOutPictureRegion = nsIntRect(MFOffsetToInt32(videoArea.OffsetX),
                                  MFOffsetToInt32(videoArea.OffsetY),
                                  videoArea.Area.cx,
                                  videoArea.Area.cy);
    return S_OK;
  }

  // No picture region defined, fall back to using the entire video area.
  UINT32 width = 0, height = 0;
  hr = MFGetAttributeSize(aMediaType, MF_MT_FRAME_SIZE, &width, &height);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);
  aOutPictureRegion = nsIntRect(0, 0, width, height);
  return S_OK;
}

namespace wmf {

static const wchar_t* sDLLs[] = {
  L"mfplat.dll",
  L"mf.dll",
  L"dxva2.dll",
  L"evr.dll",
};

HRESULT
LoadDLLs()
{
  static bool sDLLsLoaded = false;
  static bool sFailedToLoadDlls = false;

  if (sDLLsLoaded) {
    return S_OK;
  }
  if (sFailedToLoadDlls) {
    return E_FAIL;
  }

  // Try to load all the required DLLs. If we fail to load any dll,
  // unload the dlls we succeeded in loading.
  nsTArray<const wchar_t*> loadedDlls;
  for (const wchar_t* dll : sDLLs) {
    if (!LoadLibrarySystem32(dll)) {
      NS_WARNING("Failed to load WMF DLLs");
      for (const wchar_t* loadedDll : loadedDlls) {
        FreeLibrary(GetModuleHandleW(loadedDll));
      }
      sFailedToLoadDlls = true;
      return E_FAIL;
    }
    loadedDlls.AppendElement(dll);
  }
  sDLLsLoaded = true;

  return S_OK;
}

#define ENSURE_FUNCTION_PTR_HELPER(FunctionType, FunctionName, DLL)            \
  static FunctionType FunctionName##Ptr = nullptr;                             \
  if (!FunctionName##Ptr) {                                                    \
    FunctionName##Ptr =                                                        \
      (FunctionType)GetProcAddress(GetModuleHandleW(L## #DLL), #FunctionName); \
    if (!FunctionName##Ptr) {                                                  \
      NS_WARNING("Failed to get GetProcAddress of " #FunctionName              \
                 " from " #DLL);                                               \
      return E_FAIL;                                                           \
    }                                                                          \
  }

#define ENSURE_FUNCTION_PTR(FunctionName, DLL)                                 \
  ENSURE_FUNCTION_PTR_HELPER(decltype(::FunctionName)*, FunctionName, DLL)

#define ENSURE_FUNCTION_PTR_(FunctionName, DLL)                                \
  ENSURE_FUNCTION_PTR_HELPER(FunctionName##Ptr_t, FunctionName, DLL)

#define DECL_FUNCTION_PTR(FunctionName, ...)                                   \
  typedef HRESULT(STDMETHODCALLTYPE* FunctionName##Ptr_t)(__VA_ARGS__)

HRESULT
MFStartup()
{
  HRESULT hr = LoadDLLs();
  if (FAILED(hr)) {
    return hr;
  }

  const int MF_WIN7_VERSION = (0x0002 << 16 | MF_API_VERSION);

  // decltype is unusable for functions having default parameters
  DECL_FUNCTION_PTR(MFStartup, ULONG, DWORD);
  ENSURE_FUNCTION_PTR_(MFStartup, Mfplat.dll)
  return MFStartupPtr(MF_WIN7_VERSION, MFSTARTUP_FULL);
}

HRESULT
MFShutdown()
{
  ENSURE_FUNCTION_PTR(MFShutdown, Mfplat.dll)
  return (MFShutdownPtr)();
}

HRESULT
MFCreateMediaType(IMFMediaType **aOutMFType)
{
  ENSURE_FUNCTION_PTR(MFCreateMediaType, Mfplat.dll)
  return (MFCreateMediaTypePtr)(aOutMFType);
}


HRESULT
MFGetStrideForBitmapInfoHeader(DWORD aFormat,
                               DWORD aWidth,
                               LONG *aOutStride)
{
  ENSURE_FUNCTION_PTR(MFGetStrideForBitmapInfoHeader, evr.dll)
  return (MFGetStrideForBitmapInfoHeaderPtr)(aFormat, aWidth, aOutStride);
}

HRESULT MFGetService(IUnknown *punkObject,
                     REFGUID guidService,
                     REFIID riid,
                     LPVOID *ppvObject)
{
  ENSURE_FUNCTION_PTR(MFGetService, mf.dll)
  return (MFGetServicePtr)(punkObject, guidService, riid, ppvObject);
}

HRESULT
DXVA2CreateDirect3DDeviceManager9(UINT *pResetToken,
                                  IDirect3DDeviceManager9 **ppDXVAManager)
{
  ENSURE_FUNCTION_PTR(DXVA2CreateDirect3DDeviceManager9, dxva2.dll)
  return (DXVA2CreateDirect3DDeviceManager9Ptr)(pResetToken, ppDXVAManager);
}

HRESULT
MFCreateSample(IMFSample **ppIMFSample)
{
  ENSURE_FUNCTION_PTR(MFCreateSample, mfplat.dll)
  return (MFCreateSamplePtr)(ppIMFSample);
}

HRESULT
MFCreateAlignedMemoryBuffer(DWORD cbMaxLength,
                            DWORD fAlignmentFlags,
                            IMFMediaBuffer **ppBuffer)
{
  ENSURE_FUNCTION_PTR(MFCreateAlignedMemoryBuffer, mfplat.dll)
  return (MFCreateAlignedMemoryBufferPtr)(
    cbMaxLength, fAlignmentFlags, ppBuffer);
}

HRESULT
MFCreateDXGIDeviceManager(UINT* pResetToken,
                          IMFDXGIDeviceManager** ppDXVAManager)
{
  ENSURE_FUNCTION_PTR(MFCreateDXGIDeviceManager, mfplat.dll)
  return (MFCreateDXGIDeviceManagerPtr)(pResetToken, ppDXVAManager);
}

HRESULT
MFCreateDXGISurfaceBuffer(REFIID riid,
                          IUnknown *punkSurface,
                          UINT uSubresourceIndex,
                          BOOL fButtomUpWhenLinear,
                          IMFMediaBuffer **ppBuffer)
{
  ENSURE_FUNCTION_PTR(MFCreateDXGISurfaceBuffer, mfplat.dll)
  return (MFCreateDXGISurfaceBufferPtr)(
    riid, punkSurface, uSubresourceIndex, fButtomUpWhenLinear, ppBuffer);
}

} // end namespace wmf
} // end namespace mozilla
