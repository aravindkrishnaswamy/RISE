// RiseCallbacks.h — factory function for the progress adapter.
//
// Implementation of RISE::IProgressCallback lives in RiseCallbacks.cpp.
// Created by the bridge and forwards every call to the bridge itself,
// which is what actually talks to Kotlin.  The factory-function
// indirection lets RiseBridge.h stay ignorant of the RISE interface
// types.
//
// L4d: the legacy RasterizerOutputAdapter (IJobRasterizerOutput
// implementation) was removed in favor of ViewportFrameStore, which
// plays the IRasterizerOutput role directly.  See
// RiseBridge::ensureViewportFrameStoreAttached.

#ifndef RISE_CALLBACKS_H_
#define RISE_CALLBACKS_H_

namespace RISE {
    class IProgressCallback;
}

namespace rise_jni {

class RiseBridge;

// Ownership transferred to the caller (unique_ptr in the bridge).
RISE::IProgressCallback* newProgressCallback(RiseBridge* bridge);

} // namespace rise_jni

#endif // RISE_CALLBACKS_H_
