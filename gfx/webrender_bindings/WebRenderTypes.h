/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_WEBRENDERTYPES_H
#define GFX_WEBRENDERTYPES_H

#include "mozilla/webrender/webrender_ffi.h"
#include "mozilla/Maybe.h"
#include "mozilla/gfx/Types.h"
#include "mozilla/gfx/Tools.h"
#include "mozilla/Range.h"
#include "Units.h"
#include "nsStyleConsts.h"

typedef mozilla::Maybe<WrImageMask> MaybeImageMask;

namespace mozilla {
namespace wr {

typedef WrGradientExtendMode GradientExtendMode;
typedef WrMixBlendMode MixBlendMode;
typedef WrImageRendering ImageRendering;
typedef WrImageFormat ImageFormat;
typedef WrWindowId WindowId;
typedef WrPipelineId PipelineId;
typedef WrImageKey ImageKey;
typedef WrFontKey FontKey;
typedef WrEpoch Epoch;

inline WindowId NewWindowId(uint64_t aId) {
  WindowId id;
  id.mHandle = aId;
  return id;
}

inline Epoch NewEpoch(uint32_t aEpoch) {
  Epoch e;
  e.mHandle = aEpoch;
  return e;
}

inline Maybe<WrImageFormat>
SurfaceFormatToWrImageFormat(gfx::SurfaceFormat aFormat) {
  switch (aFormat) {
    case gfx::SurfaceFormat::B8G8R8X8:
      // TODO: WebRender will have a BGRA + opaque flag for this but does not
      // have it yet (cf. issue #732).
    case gfx::SurfaceFormat::B8G8R8A8:
      return Some(WrImageFormat::RGBA8);
    case gfx::SurfaceFormat::B8G8R8:
      return Some(WrImageFormat::RGB8);
    case gfx::SurfaceFormat::A8:
      return Some(WrImageFormat::A8);
    case gfx::SurfaceFormat::UNKNOWN:
      return Some(WrImageFormat::Invalid);
    default:
      return Nothing();
  }
}

inline gfx::SurfaceFormat
WrImageFormatToSurfaceFormat(ImageFormat aFormat) {
  switch (aFormat) {
    case ImageFormat::RGBA8:
      return gfx::SurfaceFormat::B8G8R8A8;
    case ImageFormat::A8:
      return gfx::SurfaceFormat::A8;
    case ImageFormat::RGB8:
      return gfx::SurfaceFormat::B8G8R8;
    default:
      return gfx::SurfaceFormat::UNKNOWN;
  }
}

struct ImageDescriptor: public WrImageDescriptor {
  ImageDescriptor(const gfx::IntSize& aSize, gfx::SurfaceFormat aFormat)
  {
    format = SurfaceFormatToWrImageFormat(aFormat).value();
    width = aSize.width;
    height = aSize.height;
    stride = 0;
    is_opaque = gfx::IsOpaqueFormat(aFormat);
  }

  ImageDescriptor(const gfx::IntSize& aSize, uint32_t aByteStride, gfx::SurfaceFormat aFormat)
  {
    format = SurfaceFormatToWrImageFormat(aFormat).value();
    width = aSize.width;
    height = aSize.height;
    stride = aByteStride;
    is_opaque = gfx::IsOpaqueFormat(aFormat);
  }
};

// Whenever possible, use wr::PipelineId instead of manipulating uint64_t.
inline uint64_t AsUint64(const PipelineId& aId) {
  return (static_cast<uint64_t>(aId.mNamespace) << 32)
        + static_cast<uint64_t>(aId.mHandle);
}

inline PipelineId AsPipelineId(const uint64_t& aId) {
  PipelineId pipeline;
  pipeline.mNamespace = aId >> 32;
  pipeline.mHandle = aId;
  return pipeline;
}

inline ImageRendering ToImageRendering(gfx::SamplingFilter aFilter)
{
  return aFilter == gfx::SamplingFilter::POINT ? ImageRendering::Pixelated
                                               : ImageRendering::Auto;
}

static inline MixBlendMode ToWrMixBlendMode(gfx::CompositionOp compositionOp)
{
  switch (compositionOp)
  {
      case gfx::CompositionOp::OP_MULTIPLY:
        return MixBlendMode::Multiply;
      case gfx::CompositionOp::OP_SCREEN:
        return MixBlendMode::Screen;
      case gfx::CompositionOp::OP_OVERLAY:
        return MixBlendMode::Overlay;
      case gfx::CompositionOp::OP_DARKEN:
        return MixBlendMode::Darken;
      case gfx::CompositionOp::OP_LIGHTEN:
        return MixBlendMode::Lighten;
      case gfx::CompositionOp::OP_COLOR_DODGE:
        return MixBlendMode::ColorDodge;
      case gfx::CompositionOp::OP_COLOR_BURN:
        return MixBlendMode::ColorBurn;
      case gfx::CompositionOp::OP_HARD_LIGHT:
        return MixBlendMode::HardLight;
      case gfx::CompositionOp::OP_SOFT_LIGHT:
        return MixBlendMode::SoftLight;
      case gfx::CompositionOp::OP_DIFFERENCE:
        return MixBlendMode::Difference;
      case gfx::CompositionOp::OP_EXCLUSION:
        return MixBlendMode::Exclusion;
      case gfx::CompositionOp::OP_HUE:
        return MixBlendMode::Hue;
      case gfx::CompositionOp::OP_SATURATION:
        return MixBlendMode::Saturation;
      case gfx::CompositionOp::OP_COLOR:
        return MixBlendMode::Color;
      case gfx::CompositionOp::OP_LUMINOSITY:
        return MixBlendMode::Luminosity;
      default:
        return MixBlendMode::Normal;
  }
}

static inline WrColor ToWrColor(const gfx::Color& color)
{
  WrColor c;
  c.r = color.r;
  c.g = color.g;
  c.b = color.b;
  c.a = color.a;
  return c;
}

template<class T>
static inline WrPoint ToWrPoint(const gfx::PointTyped<T>& point)
{
  WrPoint p;
  p.x = point.x;
  p.y = point.y;
  return p;
}

template<class T>
static inline WrPoint ToWrPoint(const gfx::IntPointTyped<T>& point)
{
  return ToWrPoint(IntPointToPoint(point));
}

static inline WrPoint ToWrPoint(const gfx::Point& point)
{
  WrPoint p;
  p.x = point.x;
  p.y = point.y;
  return p;
}

template<class T>
static inline WrRect ToWrRect(const gfx::RectTyped<T>& rect)
{
  WrRect r;
  r.x = rect.x;
  r.y = rect.y;
  r.width = rect.width;
  r.height = rect.height;
  return r;
}

template<class T>
static inline WrRect ToWrRect(const gfx::IntRectTyped<T>& rect)
{
  return ToWrRect(IntRectToRect(rect));
}

template<class T>
static inline WrSize ToWrSize(const gfx::SizeTyped<T>& size)
{
  WrSize ls;
  ls.width = size.width;
  ls.height = size.height;
  return ls;
}

template<class T>
static inline WrSize ToWrSize(const gfx::IntSizeTyped<T>& size)
{
  return ToWrSize(IntSizeToSize(size));
}

static inline WrBorderStyle ToWrBorderStyle(const uint8_t& style)
{
  switch (style) {
  case NS_STYLE_BORDER_STYLE_NONE:
    return WrBorderStyle::None;
  case NS_STYLE_BORDER_STYLE_SOLID:
    return WrBorderStyle::Solid;
  case NS_STYLE_BORDER_STYLE_DOUBLE:
    return WrBorderStyle::Double;
  case NS_STYLE_BORDER_STYLE_DOTTED:
    return WrBorderStyle::Dotted;
  case NS_STYLE_BORDER_STYLE_DASHED:
    return WrBorderStyle::Dashed;
  case NS_STYLE_BORDER_STYLE_HIDDEN:
    return WrBorderStyle::Hidden;
  case NS_STYLE_BORDER_STYLE_GROOVE:
    return WrBorderStyle::Groove;
  case NS_STYLE_BORDER_STYLE_RIDGE:
    return WrBorderStyle::Ridge;
  case NS_STYLE_BORDER_STYLE_INSET:
    return WrBorderStyle::Inset;
  case NS_STYLE_BORDER_STYLE_OUTSET:
    return WrBorderStyle::Outset;
  default:
    MOZ_ASSERT(false);
  }
  return WrBorderStyle::None;
}

static inline WrBorderSide ToWrBorderSide(const gfx::Color& color, const uint8_t& style)
{
  WrBorderSide bs;
  bs.color = ToWrColor(color);
  bs.style = ToWrBorderStyle(style);
  return bs;
}

static inline WrBorderRadius ToWrUniformBorderRadius(const LayerSize& aSize)
{
  WrBorderRadius br;
  br.top_left = ToWrSize(aSize);
  br.top_right = ToWrSize(aSize);
  br.bottom_left = ToWrSize(aSize);
  br.bottom_right = ToWrSize(aSize);
  return br;
}

static inline WrBorderRadius ToWrBorderRadius(const LayerSize& topLeft, const LayerSize& topRight,
                                              const LayerSize& bottomLeft, const LayerSize& bottomRight)
{
  WrBorderRadius br;
  br.top_left = ToWrSize(topLeft);
  br.top_right = ToWrSize(topRight);
  br.bottom_left = ToWrSize(bottomLeft);
  br.bottom_right = ToWrSize(bottomRight);
  return br;
}

static inline WrBorderWidths ToWrBorderWidths(float top, float right, float bottom, float left)
{
  WrBorderWidths bw;
  bw.top = top;
  bw.right = right;
  bw.bottom = bottom;
  bw.left = left;
  return bw;
}

static inline WrNinePatchDescriptor ToWrNinePatchDescriptor(uint32_t width, uint32_t height,
                                                            const WrSideOffsets2Du32& slice)
{
  WrNinePatchDescriptor patch;
  patch.width = width;
  patch.height = height;
  patch.slice = slice;
  return patch;
}

static inline WrSideOffsets2Du32 ToWrSideOffsets2Du32(uint32_t top, uint32_t right, uint32_t bottom, uint32_t left)
{
  WrSideOffsets2Du32 offset;
  offset.top = top;
  offset.right = right;
  offset.bottom = bottom;
  offset.left = left;
  return offset;
}

static inline WrSideOffsets2Df32 ToWrSideOffsets2Df32(float top, float right, float bottom, float left)
{
  WrSideOffsets2Df32 offset;
  offset.top = top;
  offset.right = right;
  offset.bottom = bottom;
  offset.left = left;
  return offset;
}

static inline WrRepeatMode ToWrRepeatMode(uint8_t repeatMode)
{
  switch (repeatMode) {
  case NS_STYLE_BORDER_IMAGE_REPEAT_STRETCH:
    return WrRepeatMode::Stretch;
  case NS_STYLE_BORDER_IMAGE_REPEAT_REPEAT:
    return WrRepeatMode::Repeat;
  case NS_STYLE_BORDER_IMAGE_REPEAT_ROUND:
    return WrRepeatMode::Round;
  case NS_STYLE_BORDER_IMAGE_REPEAT_SPACE:
    return WrRepeatMode::Space;
  default:
    MOZ_ASSERT(false);
  }

  return WrRepeatMode::Stretch;
}

template<class T>
static inline WrComplexClipRegion ToWrComplexClipRegion(const gfx::RectTyped<T>& rect,
                                                        const LayerSize& size)
{
  WrComplexClipRegion complex_clip;
  complex_clip.rect = wr::ToWrRect(rect);
  complex_clip.radii = wr::ToWrUniformBorderRadius(size);
  return complex_clip;
}

static inline WrExternalImageId ToWrExternalImageId(uint64_t aID)
{
  WrExternalImageId id;
  id.id = aID;
  return id;
}

struct VecU8 {
  WrVecU8 inner;
  VecU8() {
    inner.data = nullptr;
    inner.capacity = 0;
  }
  VecU8(VecU8&) = delete;
  VecU8(VecU8&& src) {
    inner = src.inner;
    src.inner.data = nullptr;
    src.inner.capacity = 0;
  }

  VecU8&
  operator=(VecU8&& src) {
    inner = src.inner;
    src.inner.data = nullptr;
    src.inner.capacity = 0;
    return *this;
  }

  WrVecU8
  Extract() {
    WrVecU8 ret = inner;
    inner.data = nullptr;
    inner.capacity = 0;
    return ret;
  }

  ~VecU8() {
    if (inner.data) {
      wr_vec_u8_free(inner);
    }
  }
};

struct ByteBuffer
{
  ByteBuffer(size_t aLength, uint8_t* aData)
    : mLength(aLength)
    , mData(aData)
    , mOwned(false)
  {}

  // XXX: this is a bit of hack that assumes
  // the allocators are the same
  explicit ByteBuffer(VecU8&& vec)
  {
    if (vec.inner.capacity) {
      mLength = vec.inner.length;
      mData = vec.inner.data;
      vec.inner.data = nullptr;
      vec.inner.capacity = 0;
      mOwned = true;
    } else {
      mOwned = false;
      mData = nullptr;
      mLength = 0;
    }
  }

  ByteBuffer()
    : mLength(0)
    , mData(nullptr)
    , mOwned(false)
  {}

  bool
  Allocate(size_t aLength)
  {
    MOZ_ASSERT(mData == nullptr);
    mData = (uint8_t*)malloc(aLength);
    if (!mData) {
      return false;
    }
    mLength = aLength;
    mOwned = true;
    return true;
  }

  ~ByteBuffer()
  {
    if (mData && mOwned) {
      free(mData);
    }
  }

  const Range<uint8_t> AsSlice() const { return Range<uint8_t>(mData, mLength); }

  Range<uint8_t> AsSlice() { return Range<uint8_t>(mData, mLength); }

  bool operator==(const ByteBuffer& other) const {
    return mLength == other.mLength &&
          !(memcmp(mData, other.mData, mLength));
  }

  size_t mLength;
  uint8_t* mData;
  bool mOwned;
};

inline WrByteSlice RangeToByteSlice(mozilla::Range<uint8_t> aRange) {
  return WrByteSlice { aRange.begin().get(), aRange.length() };
}

inline mozilla::Range<uint8_t> ByteSliceToRange(WrByteSlice aWrSlice) {
  return mozilla::Range<uint8_t>(aWrSlice.mBuffer, aWrSlice.mLength);
}

struct BuiltDisplayList {
  VecU8 dl;
  WrBuiltDisplayListDescriptor dl_desc;
  VecU8 aux;
  WrAuxiliaryListsDescriptor aux_desc;
};

} // namespace wr
} // namespace mozilla

#endif /* GFX_WEBRENDERTYPES_H */
