#include "RiseCallbacks.h"
#include "RiseBridge.h"

#include "Interfaces/IProgressCallback.h"
#include "Interfaces/IJobRasterizerOutput.h"

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

// -----------------------------------------------------------------------------
// Rasterizer output adapter
// -----------------------------------------------------------------------------
// Receives the full RGBA16 image every call with a dirty rectangle. See
// src/Library/Interfaces/IJobRasterizerOutput.h for the exact signature.
// We request sRGB colour space (matches the Mac bridge) so the library
// applies gamma and a simple uint16 >> 8 downconvert is visually correct
// on the Kotlin side.
class RasterizerOutputAdapter : public RISE::IJobRasterizerOutput {
public:
    explicit RasterizerOutputAdapter(RiseBridge* bridge) : m_bridge(bridge) {}
    ~RasterizerOutputAdapter() override = default;

    bool PremultipliedAlpha() override { return false; }
    int  GetColorSpace()      override { return 1; } // sRGB

    void OutputImageRGBA16(
        const unsigned short* pImageData,
        const unsigned int    width,
        const unsigned int    height,
        const unsigned int    rc_top,
        const unsigned int    rc_left,
        const unsigned int    rc_bottom,
        const unsigned int    rc_right) override
    {
        if (!m_bridge || !pImageData) return;
        m_bridge->writeDirtyRegion(pImageData, width, height,
                                   rc_top, rc_left, rc_bottom, rc_right);
    }

private:
    RiseBridge* m_bridge;
};

} // namespace

RISE::IProgressCallback* newProgressCallback(RiseBridge* bridge) {
    return new ProgressAdapter(bridge);
}

RISE::IJobRasterizerOutput* newRasterizerOutput(RiseBridge* bridge) {
    return new RasterizerOutputAdapter(bridge);
}

} // namespace rise_jni
