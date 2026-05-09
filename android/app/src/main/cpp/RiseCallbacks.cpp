#include "RiseCallbacks.h"
#include "RiseBridge.h"

#include "Interfaces/IProgressCallback.h"

namespace rise_jni {
namespace {

// -----------------------------------------------------------------------------
// Progress adapter
// -----------------------------------------------------------------------------
// Invoked by the library's rasterizer — always from a worker thread, always
// serialized via the dispatcher's internal mutex (see
// src/Library/Rendering/RasterizeDispatchers.h). Forwards to the bridge which
// reads the cancel flag and posts the value to Kotlin via JNI.
class ProgressAdapter : public RISE::IProgressCallback {
public:
    explicit ProgressAdapter(RiseBridge* bridge) : m_bridge(bridge) {}
    ~ProgressAdapter() override = default;

    bool Progress(const double progress, const double total) override {
        if (!m_bridge) return true;
        return m_bridge->onProgressTick(progress, total);
    }

    void SetTitle(const char* /*title*/) override {
        // No-op for Android — the UI doesn't need this and it arrives from
        // a worker thread anyway.
    }

private:
    RiseBridge* m_bridge;
};

// (Legacy RasterizerOutputAdapter / newRasterizerOutput removed by
// L4d.  ViewportFrameStore now plays the IRasterizerOutput role
// directly; see RiseBridge::ensureViewportFrameStoreAttached.)

} // namespace

RISE::IProgressCallback* newProgressCallback(RiseBridge* bridge) {
    return new ProgressAdapter(bridge);
}

} // namespace rise_jni
