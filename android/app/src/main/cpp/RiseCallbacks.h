// RiseCallbacks.h — factory functions for the progress and rasterizer
// output adapters.
//
// Implementations of RISE::IProgressCallback and RISE::IJobRasterizerOutput
// live in RiseCallbacks.cpp. They are created by the bridge and forward
// every call to the bridge itself, which is what actually talks to Kotlin.
// The factory-function indirection lets RiseBridge.h stay ignorant of the
// RISE interface types.

#ifndef RISE_CALLBACKS_H_
#define RISE_CALLBACKS_H_

namespace RISE {
    class IProgressCallback;
    class IJobRasterizerOutput;
}

namespace rise_jni {

class RiseBridge;

// Ownership transferred to the caller (unique_ptr in the bridge).
RISE::IProgressCallback*    newProgressCallback(RiseBridge* bridge);
RISE::IJobRasterizerOutput* newRasterizerOutput(RiseBridge* bridge);

} // namespace rise_jni

#endif // RISE_CALLBACKS_H_
