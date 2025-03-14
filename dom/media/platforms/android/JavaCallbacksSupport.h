/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef JavaCallbacksSupport_h_
#define JavaCallbacksSupport_h_

#include "FennecJNINatives.h"
#include "MediaResult.h"
#include "MediaCodec.h"

namespace mozilla {

class JavaCallbacksSupport
  : public java::CodecProxy::NativeCallbacks::Natives<JavaCallbacksSupport>
{
public:
  typedef java::CodecProxy::NativeCallbacks::Natives<JavaCallbacksSupport> Base;
  using Base::AttachNative;
  using Base::DisposeNative;

  JavaCallbacksSupport() : mCanceled(false) { }

  virtual ~JavaCallbacksSupport() { }

  virtual void HandleInputExhausted() = 0;

  void OnInputExhausted()
  {
    if (!mCanceled) {
      HandleInputExhausted();
    }
  }

  virtual void HandleOutput(java::Sample::Param aSample) = 0;

  void OnOutput(jni::Object::Param aSample)
  {
    if (!mCanceled) {
      HandleOutput(java::Sample::Ref::From(aSample));
    }
  }

  virtual void HandleOutputFormatChanged(java::sdk::MediaFormat::Param aFormat) { };

  void OnOutputFormatChanged(jni::Object::Param aFormat)
  {
    if (!mCanceled) {
      HandleOutputFormatChanged(java::sdk::MediaFormat::Ref::From(aFormat));
    }
  }

  virtual void HandleError(const MediaResult& aError) = 0;

  void OnError(bool aIsFatal)
  {
    if (!mCanceled) {
      HandleError(
        aIsFatal
        ? MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR, __func__)
        : MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR, __func__));
    }
  }

  void Cancel()
  {
    mCanceled = true;
  }

private:
  Atomic<bool> mCanceled;
};

} // namespace mozilla

#endif
