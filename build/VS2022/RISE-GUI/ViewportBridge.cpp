//////////////////////////////////////////////////////////////////////
//
//  ViewportBridge.cpp - Qt implementation that wraps the C++
//    SceneEditController via RISE_API_SceneEditController_* C-API.
//
//  Mirrors the macOS RISEViewportBridge.mm.
//
//////////////////////////////////////////////////////////////////////

#include "ViewportBridge.h"
#include "RenderEngine.h"

#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <atomic>
#include <vector>

#include "RISE_API.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/IRasterizerOutput.h"
#include "Interfaces/IRasterImage.h"
#include "Utilities/Reference.h"
#include "SceneEditor/SceneEditController.h"
#include "Rendering/InteractivePelRasterizer.h"

using namespace RISE;

// =====================================================================
// ViewportPreviewSink — IRasterizerOutput that converts the final frame
// of each render pass to a QImage and queues it onto the UI thread via
// the bridge's `imageUpdated` signal.
//
// Suppress-next: drops exactly one upcoming dispatch.  Used right
// after a production render returns so the production image stays
// on screen until the user actually starts interacting.  Beyond
// that we do NOT throttle: every frame the rasterizer produces
// reaches the screen, including partials from cancelled passes.
// =====================================================================
class ViewportPreviewSink : public IRasterizerOutput,
                            public Implementation::Reference
{
public:
    explicit ViewportPreviewSink(ViewportBridge* bridge) : m_bridge(bridge) {}
    ~ViewportPreviewSink() override = default;

    // Borrowed; the bridge keeps the controller alive for the sink's
    // lifetime.  Used to query IsCancelRequested at end-of-pass.
    void SetController(SceneEditController* c) { m_controller = c; }

    // Drop the very next OutputImage call.  Auto-clears after one
    // drop.  Atomic so the bridge can call this from the UI thread
    // while the render thread fires OutputImage from a worker thread.
    void SuppressNextFrame() { m_suppressNext.store(true); }

    // Per-tile callback fires many times per render pass — explicitly
    // ignore so the user doesn't see tile-by-tile fills.
    void OutputIntermediateImage(const IRasterImage& /*pImage*/,
                                 const RISE::Rect* /*pRegion*/) override {}

    // End-of-pass: blit the whole image and emit on the UI thread.
    //
    // Every dispatch reaches the screen.  We do NOT drop cancelled-
    // mid-pass frames: during fast manipulation the cancel flag
    // trips on every pointer move, and dropping the resulting
    // partial buffers makes the viewport feel throttled (the user
    // only sees post-pause refinement frames).  Center-out tile
    // order keeps partial buffers visually usable.  The one-shot
    // suppress is kept because it serves a distinct purpose
    // (preserving the production image until the user drags).
    void OutputImage(const IRasterImage& pImage,
                     const RISE::Rect* /*pRegion*/,
                     const unsigned int /*frame*/) override {
        if (!m_bridge) return;
        if (m_suppressNext.exchange(false)) {
            // One-shot suppression (post-production) — skip exactly
            // this dispatch.  The next render's frame goes through.
            return;
        }
        const unsigned int W = pImage.GetWidth();
        const unsigned int H = pImage.GetHeight();
        if (W == 0 || H == 0) return;

        QImage img(static_cast<int>(W), static_cast<int>(H), QImage::Format_RGBA8888);

        for (unsigned int y = 0; y < H; ++y) {
            uchar* row = img.scanLine(static_cast<int>(y));
            for (unsigned int x = 0; x < W; ++x) {
                RISEColor c = pImage.GetPEL(x, y);
                auto clamp8 = [](double v) -> uchar {
                    if (v <= 0.0) return 0;
                    if (v >= 1.0) return 255;
                    return static_cast<uchar>(v * 255.0 + 0.5);
                };
                *row++ = clamp8(c.base.r);
                *row++ = clamp8(c.base.g);
                *row++ = clamp8(c.base.b);
                *row++ = 255;
            }
        }

        // Queue a UI-thread emission.  Capture the bridge via QPointer
        // so the lambda no-ops if the QObject was destroyed between
        // the worker thread enqueueing this call and the UI thread
        // running it.  Without QPointer, dereferencing a raw pointer
        // to a destroyed QObject is undefined behaviour (Qt unhooks
        // queued events whose receiver dies, but our lambda dereferences
        // its captured pointer directly, bypassing that protection).
        QPointer<ViewportBridge> guard(m_bridge);
        QMetaObject::invokeMethod(m_bridge, [guard, img]() {
            if (ViewportBridge* b = guard.data()) {
                emit b->imageUpdated(img);
            }
        }, Qt::QueuedConnection);
    }

private:
    ViewportBridge*      m_bridge = nullptr;
    SceneEditController* m_controller = nullptr;   // borrowed
    std::atomic<bool>    m_suppressNext{false};
};

// =====================================================================
// ViewportBridge
// =====================================================================

ViewportBridge::ViewportBridge(RenderEngine* engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
{
    if (!engine) return;
    void* opaque = engine->opaqueJobHandle();
    if (!opaque) return;

    IJobPriv* pJob = static_cast<IJobPriv*>(opaque);
    buildLivePreview();

    if (!RISE_API_CreateSceneEditController(pJob, m_interactiveRasterizer, &m_controller)) {
        m_controller = nullptr;
        releaseLivePreview();
        return;
    }
    if (m_previewSink) {
        // The sink queries the controller's cancel state at end-of-pass
        // so it can drop a stale dispatch.  Wire the pointer before
        // installing the sink as a rasterizer output.
        m_previewSink->SetController(m_controller);
        RISE_API_SceneEditController_SetPreviewSink(m_controller, m_previewSink);
    }
}

ViewportBridge::~ViewportBridge()
{
    stop();
    if (m_controller) {
        RISE_API_DestroySceneEditController(m_controller);
        m_controller = nullptr;
    }
    releaseLivePreview();
}

void ViewportBridge::buildLivePreview()
{
    if (!m_engine) return;
    void* opaque = m_engine->opaqueJobHandle();
    if (!opaque) return;

    IRasterizer* interactive = nullptr;
    IRayCaster* pCaster = nullptr;
    IRayCaster* pPolishCaster = nullptr;
    if (!Implementation::CreateInteractiveMaterialPreviewPipeline(
            &interactive, &pCaster, &pPolishCaster)) {
        return;
    }

    m_caster = pCaster;
    m_polishCaster = pPolishCaster;
    m_interactiveRasterizer = interactive;

    m_previewSink = new ViewportPreviewSink(this);
    m_previewSink->addref();
}

void ViewportBridge::releaseLivePreview()
{
    if (m_previewSink) { m_previewSink->release(); m_previewSink = nullptr; }
    if (m_interactiveRasterizer) { m_interactiveRasterizer->release(); m_interactiveRasterizer = nullptr; }
    if (m_polishCaster) { m_polishCaster->release(); m_polishCaster = nullptr; }
    if (m_caster) { m_caster->release(); m_caster = nullptr; }
}

void ViewportBridge::start()
{
    if (!m_controller) return;
    RISE_API_SceneEditController_Start(m_controller);
    m_running = true;
}

void ViewportBridge::stop()
{
    if (!m_controller) return;
    RISE_API_SceneEditController_Stop(m_controller);
    m_running = false;
}

void ViewportBridge::suppressNextFrame()
{
    if (m_previewSink) m_previewSink->SuppressNextFrame();
}

void ViewportBridge::setTool(ViewportTool t)
{
    if (!m_controller) return;
    RISE_API_SceneEditController_SetTool(m_controller, static_cast<int>(t));
}

void ViewportBridge::pointerDown(double x, double y) { if (m_controller) RISE_API_SceneEditController_OnPointerDown(m_controller, x, y); }
void ViewportBridge::pointerMove(double x, double y) { if (m_controller) RISE_API_SceneEditController_OnPointerMove(m_controller, x, y); }
void ViewportBridge::pointerUp(double x, double y)   { if (m_controller) RISE_API_SceneEditController_OnPointerUp(m_controller, x, y); }

QSize ViewportBridge::cameraSurfaceDimensions() const
{
    if (!m_controller) return QSize();
    unsigned int w = 0, h = 0;
    if (!RISE_API_SceneEditController_GetCameraDimensions(m_controller, &w, &h)) {
        return QSize();
    }
    return QSize(static_cast<int>(w), static_cast<int>(h));
}

bool ViewportBridge::animationOptions(double& timeStart, double& timeEnd, unsigned int& numFrames) const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_GetAnimationOptions(m_controller, &timeStart, &timeEnd, &numFrames);
}

void ViewportBridge::scrubTimeBegin() { if (m_controller) RISE_API_SceneEditController_OnTimeScrubBegin(m_controller); }
void ViewportBridge::scrubTime(double t) { if (m_controller) RISE_API_SceneEditController_OnTimeScrub(m_controller, t); }
void ViewportBridge::scrubTimeEnd()   { if (m_controller) RISE_API_SceneEditController_OnTimeScrubEnd(m_controller); }

void ViewportBridge::beginPropertyScrub() { if (m_controller) RISE_API_SceneEditController_BeginPropertyScrub(m_controller); }
void ViewportBridge::endPropertyScrub()   { if (m_controller) RISE_API_SceneEditController_EndPropertyScrub(m_controller); }

void ViewportBridge::undo() { if (m_controller) RISE_API_SceneEditController_Undo(m_controller); }
void ViewportBridge::redo() { if (m_controller) RISE_API_SceneEditController_Redo(m_controller); }

double ViewportBridge::lastSceneTime() const
{
    if (!m_controller) return 0.0;
    double t = 0.0;
    RISE_API_SceneEditController_LastSceneTime(m_controller, &t);
    return t;
}

bool ViewportBridge::requestProductionRender()
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_RequestProductionRender(m_controller);
}

ViewportBridge::PanelMode ViewportBridge::panelMode() const
{
    if (!m_controller) return PanelMode::None;
    const int m = RISE_API_SceneEditController_PanelMode(m_controller);
    switch (m) {
        case 1: return PanelMode::Camera;
        case 2: return PanelMode::Object;
        default: return PanelMode::None;
    }
}

QString ViewportBridge::panelHeader() const
{
    if (!m_controller) return QString();
    char buf[256] = {0};
    RISE_API_SceneEditController_PanelHeader(m_controller, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

QVector<ViewportProperty> ViewportBridge::propertySnapshot()
{
    QVector<ViewportProperty> out;
    if (!m_controller) return out;
    RISE_API_SceneEditController_RefreshProperties(m_controller);
    const unsigned int n = RISE_API_SceneEditController_PropertyCount(m_controller);
    out.reserve(static_cast<int>(n));
    char nameBuf[128];
    char valBuf[256];
    char descBuf[512];
    for (unsigned int i = 0; i < n; ++i) {
        RISE_API_SceneEditController_PropertyName(m_controller, i, nameBuf, sizeof(nameBuf));
        RISE_API_SceneEditController_PropertyValue(m_controller, i, valBuf, sizeof(valBuf));
        RISE_API_SceneEditController_PropertyDescription(m_controller, i, descBuf, sizeof(descBuf));
        ViewportProperty p;
        p.name = QString::fromUtf8(nameBuf);
        p.value = QString::fromUtf8(valBuf);
        p.description = QString::fromUtf8(descBuf);
        p.kind = RISE_API_SceneEditController_PropertyKind(m_controller, i);
        p.editable = RISE_API_SceneEditController_PropertyEditable(m_controller, i);
        out.append(p);
    }
    return out;
}

bool ViewportBridge::setProperty(const QString& name, const QString& value)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetProperty(m_controller,
        name.toUtf8().constData(), value.toUtf8().constData());
}
