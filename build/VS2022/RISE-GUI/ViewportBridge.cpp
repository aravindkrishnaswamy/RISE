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
#include <QCoreApplication>
#include <QThreadPool>
#include <atomic>
#include <limits>
#include <memory>
#include <vector>

#include "RISE_API.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/IRasterizerOutput.h"
#include "Interfaces/IRasterImage.h"
#include "Utilities/Reference.h"
#include "SceneEditor/SceneEditController.h"
#include "Rendering/InteractivePelRasterizer.h"
#include "Agent/AgentSession.h"
#include "Agent/AgentRpc.h"
#include "Agent/AgentChatCodecs.h"    // Arc 77 Phase 2 GUI wiring: MakeChatImageGenerator (imagine_scene)
#include "Agent/ChatHttpTransport.h"  // Arc 77 Phase 2 GUI wiring: CreateSystemChatHttpTransport

using namespace RISE;

namespace {

template<typename IsCurrent>
QImage RasterImageToQImage(const IRasterImage& image, IsCurrent&& isCurrent)
{
    const unsigned int width = image.GetWidth();
    const unsigned int height = image.GetHeight();
    if (width == 0 || height == 0
        || width > static_cast<unsigned int>(std::numeric_limits<int>::max())
        || height > static_cast<unsigned int>(std::numeric_limits<int>::max()))
        return QImage();
    QImage result(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGBA8888);
    if (result.isNull()) return QImage();
    for (unsigned int y = 0; y < height; ++y) {
        if (!isCurrent()) return QImage();
        uchar* row = result.scanLine(static_cast<int>(y));
        for (unsigned int x = 0; x < width; ++x) {
            const RISEColor c = image.GetPEL(x, y);
            auto clamp8 = [](double v) -> uchar {
                if (v <= 0.0) return 0;
                if (v >= 1.0) return 255;
                return static_cast<uchar>(v * 255.0 + 0.5);
            };
            *row++ = clamp8(c.base.r);
            *row++ = clamp8(c.base.g);
            *row++ = clamp8(c.base.b);
            *row++ = ViewportShellDisplayAlpha8(c);
        }
    }
    return result;
}

}  // namespace

// =====================================================================
// ViewportPreviewSink — IRasterizerOutput that converts the final frame
// of each render pass to a QImage and queues it onto the UI thread via
// the bridge's `imageUpdated` signal.
//
// Keeping the production image on screen after a production render is
// NOT handled here.  It used to be a one-shot "drop the next dispatch"
// flag on this sink, but that only covered this LDR QImage path — when
// the HDR/EDR display path is active the interactive frame reaches the
// screen through the interactive ViewportFrameStore's frame-complete
// observer (bound to the rasterizer's FrameStore), bypassing this sink
// entirely, so the suppression was a no-op and the production image
// flipped back to the live preview.  The fix lives one layer down: the
// SceneEditController is restarted via startSuppressingInitialRender(),
// so it simply doesn't produce the overwriting frame on any path until
// the user interacts.  We do NOT throttle here: every frame the
// rasterizer produces reaches the screen, including partials from
// cancelled passes.
// =====================================================================
class ViewportPreviewSink : public IRasterizerOutput,
                            public Implementation::Reference
{
public:
    explicit ViewportPreviewSink(ViewportBridge* bridge)
        : m_bridge(bridge)
        , m_presentGeneration(std::make_shared<std::atomic<unsigned long long>>(0))
        , m_lastRenderGeneration(std::make_shared<std::atomic<unsigned long long>>(0)) {}
    ~ViewportPreviewSink() override {
        InvalidatePresentation();
    }

    // Borrowed; the bridge keeps the controller alive for the sink's
    // lifetime.  Used to query IsCancelRequested at end-of-pass.
    void SetController(SceneEditController* c) { m_controller = c; }
    void InvalidatePresentation() {
        m_presentGeneration->fetch_add(1, std::memory_order_acq_rel);
        m_lastRenderGeneration->fetch_add(1, std::memory_order_acq_rel);
    }

    template<typename Fn>
    bool RunPresentationTransition(Fn&& fn, bool invalidateLastOnSuccess) {
        const bool ok = fn();
        if (ok && invalidateLastOnSuccess)
            m_lastRenderGeneration->fetch_add(1, std::memory_order_acq_rel);
        return ok;
    }

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
    // order keeps partial buffers visually usable.
    void OutputImage(const IRasterImage& pImage,
                     const RISE::Rect* /*pRegion*/,
                     const unsigned int frame) override {
        if (!m_bridge) return;
        const unsigned int W = pImage.GetWidth();
        const unsigned int H = pImage.GetHeight();
        if (W == 0 || H == 0) return;
        QPointer<ViewportBridge> guard(m_bridge);
        const auto generationState = m_presentGeneration;
        const unsigned long long generation =
            generationState->load(std::memory_order_acquire);
        const auto lastRenderState = m_lastRenderGeneration;
        auto queuePresent = [guard, generationState, generation,
                             lastRenderState](QImage img,
                                              unsigned long long lastRenderGeneration) {
            if (img.isNull()) return;
            QCoreApplication* app = QCoreApplication::instance();
            if (!app) return;
            QMetaObject::invokeMethod(app,
                [guard, generationState, generation, lastRenderState,
                 lastRenderGeneration, img]() {
                    if (generationState->load(std::memory_order_acquire) != generation) return;
                    if (lastRenderGeneration != 0
                        && lastRenderState->load(std::memory_order_acquire)
                           != lastRenderGeneration) return;
                    if (ViewportBridge* b = guard.data()) emit b->imageUpdated(img);
                }, Qt::QueuedConnection);
        };
        if (frame == SceneEditController::kLastRenderSinkFrame) {
            const unsigned long long lastRenderGeneration =
                lastRenderState->fetch_add(1, std::memory_order_acq_rel) + 1;
            pImage.addref();
            const IRasterImage* image = &pImage;
            QThreadPool::globalInstance()->start(
                [image, generationState, generation, lastRenderState,
                 lastRenderGeneration, queuePresent]() {
                    if (generationState->load(std::memory_order_acquire) != generation
                        || lastRenderState->load(std::memory_order_acquire)
                           != lastRenderGeneration) {
                        image->release();
                        return;
                    }
                    QImage converted = RasterImageToQImage(*image, [&]() {
                        return generationState->load(std::memory_order_acquire) == generation
                            && lastRenderState->load(std::memory_order_acquire)
                               == lastRenderGeneration;
                    });
                    image->release();
                    if (generationState->load(std::memory_order_acquire) == generation
                        && lastRenderState->load(std::memory_order_acquire)
                           == lastRenderGeneration)
                        queuePresent(converted, lastRenderGeneration);
                });
            return;
        }
        lastRenderState->fetch_add(1, std::memory_order_acq_rel);
        queuePresent(RasterImageToQImage(pImage, [] { return true; }), 0);
    }

private:
    ViewportBridge*      m_bridge = nullptr;
    SceneEditController* m_controller = nullptr;   // borrowed
    std::shared_ptr<std::atomic<unsigned long long>> m_presentGeneration;
    std::shared_ptr<std::atomic<unsigned long long>> m_lastRenderGeneration;
};

// =====================================================================
// ViewportPaneSink — N-up multi-viewport (docs/gui/RENDER_MODES.md §7):
// like ViewportPreviewSink above, but for a SECONDARY pane (1..
// ViewportBridge::kViewportPaneCount-1).  Registered per-pane via
// RISE_API_SceneEditController_SetPaneSink at bridge construction time.
// Pane 0 keeps using the legacy ViewportPreviewSink/_SetPreviewSink path
// above -- the render loop's documented fallback ("falling back to the
// legacy single sink ... when a pane has none") makes that pane 0's sink,
// so Single-layout behaviour never touches this class at all.
// =====================================================================
class ViewportPaneSink : public IRasterizerOutput,
                          public Implementation::Reference
{
public:
    ViewportPaneSink(ViewportBridge* bridge, unsigned int pane)
        : m_bridge(bridge)
        , m_pane(pane)
        , m_presentGeneration(std::make_shared<std::atomic<unsigned long long>>(0))
        , m_lastRenderGeneration(std::make_shared<std::atomic<unsigned long long>>(0)) {}
    ~ViewportPaneSink() override {
        InvalidatePresentation();
    }

    void InvalidatePresentation() {
        m_presentGeneration->fetch_add(1, std::memory_order_acq_rel);
        m_lastRenderGeneration->fetch_add(1, std::memory_order_acq_rel);
    }

    template<typename Fn>
    bool RunPresentationTransition(Fn&& fn, bool invalidateLastOnSuccess) {
        const bool ok = fn();
        if (ok && invalidateLastOnSuccess)
            m_lastRenderGeneration->fetch_add(1, std::memory_order_acq_rel);
        return ok;
    }

    void OutputIntermediateImage(const IRasterImage& /*pImage*/,
                                 const RISE::Rect* /*pRegion*/) override {}

    void OutputImage(const IRasterImage& pImage,
                     const RISE::Rect* /*pRegion*/,
                     const unsigned int frame) override {
        if (!m_bridge) return;
        const unsigned int W = pImage.GetWidth();
        const unsigned int H = pImage.GetHeight();
        if (W == 0 || H == 0) return;

        const unsigned int pane = m_pane;
        QPointer<ViewportBridge> guard(m_bridge);
        const auto generationState = m_presentGeneration;
        const unsigned long long generation =
            generationState->load(std::memory_order_acquire);
        const auto lastRenderState = m_lastRenderGeneration;
        auto queuePresent = [guard, pane, generationState, generation,
                             lastRenderState](QImage img,
                                              unsigned long long lastRenderGeneration) {
            if (img.isNull()) return;
            QCoreApplication* app = QCoreApplication::instance();
            if (!app) return;
            QMetaObject::invokeMethod(app,
                [guard, pane, generationState, generation, lastRenderState,
                 lastRenderGeneration, img]() {
                    if (generationState->load(std::memory_order_acquire) != generation) return;
                    if (lastRenderGeneration != 0
                        && lastRenderState->load(std::memory_order_acquire)
                           != lastRenderGeneration) return;
                    if (ViewportBridge* b = guard.data()) emit b->paneImageUpdated(pane, img);
                }, Qt::QueuedConnection);
        };
        if (frame == SceneEditController::kLastRenderSinkFrame) {
            const unsigned long long lastRenderGeneration =
                lastRenderState->fetch_add(1, std::memory_order_acq_rel) + 1;
            pImage.addref();
            const IRasterImage* image = &pImage;
            QThreadPool::globalInstance()->start(
                [image, generationState, generation, lastRenderState,
                 lastRenderGeneration, queuePresent]() {
                    if (generationState->load(std::memory_order_acquire) != generation
                        || lastRenderState->load(std::memory_order_acquire)
                           != lastRenderGeneration) {
                        image->release();
                        return;
                    }
                    QImage converted = RasterImageToQImage(*image, [&]() {
                        return generationState->load(std::memory_order_acquire) == generation
                            && lastRenderState->load(std::memory_order_acquire)
                               == lastRenderGeneration;
                    });
                    image->release();
                    if (generationState->load(std::memory_order_acquire) == generation
                        && lastRenderState->load(std::memory_order_acquire)
                           == lastRenderGeneration)
                        queuePresent(converted, lastRenderGeneration);
                });
            return;
        }
        lastRenderState->fetch_add(1, std::memory_order_acq_rel);
        queuePresent(RasterImageToQImage(pImage, [] { return true; }), 0);
    }

private:
    ViewportBridge* m_bridge = nullptr;
    unsigned int    m_pane = 0;
    std::shared_ptr<std::atomic<unsigned long long>> m_presentGeneration;
    std::shared_ptr<std::atomic<unsigned long long>> m_lastRenderGeneration;
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

    // Model-B F2 slice S4: register this controller on the engine so its
    // production-render entry points (startRender / startAnimationRender)
    // route through the SAME single-slot coordinator as the interactive
    // loop and agent renders, instead of calling Job::Rasterize() directly.
    // Cleared back to nullptr at the START of the destructor, before
    // `m_controller` is destroyed -- see RenderEngine::attachSceneEditController's
    // header doc for the full contract.  Mirrors macOS
    // RISEViewportBridge's -attachSceneEditController: wiring exactly.
    if (m_engine) {
        m_engine->attachSceneEditController(static_cast<void*>(m_controller));
    }

    if (m_previewSink) {
        // The sink queries the controller's cancel state at end-of-pass
        // so it can drop a stale dispatch.  Wire the pointer before
        // installing the sink as a rasterizer output.
        m_previewSink->SetController(m_controller);
        RISE_API_SceneEditController_SetPreviewSink(m_controller, m_previewSink);
    }

    // N-up multi-viewport (§7): register one sink per SECONDARY pane
    // (index 0 stays null -- pane 0 keeps the legacy m_previewSink above).
    // Built here rather than in buildLivePreview() so construction only
    // ever happens once the controller itself exists to register against;
    // torn down in releaseLivePreview() alongside m_previewSink.
    for (unsigned int pane = 1; pane < kViewportPaneCount; ++pane) {
        m_paneSinks[pane] = new ViewportPaneSink(this, pane);
        m_paneSinks[pane]->addref();
        RISE_API_SceneEditController_SetPaneSink(m_controller, pane, m_paneSinks[pane]);
    }

    // SHARED LAST-RENDER CACHE (2026-07).  All THREE in-app sessions below
    // are handed the SAME AgentImageCache, so a `render` performed through
    // any one of them is readable by `read_image` through the others.  Before
    // this, the cache was per-session and flipping the composer's autonomy
    // chip to or from Propose between a render and the read that followed it
    // moved the read onto a session whose cache was empty or stale -- the
    // model then re-rendered for several turns trying to get pixels back.
    // Windows stands up no hosted loopback server, so all three sessions here
    // are inside the app's trust boundary; see the macOS bridge's hosted
    // External session for the case that must NOT get this handle.
    std::shared_ptr<Agent::AgentImageCache> inAppImageCache =
        Agent::AgentSession::MakeSharedImageCache();
    {
        std::unique_ptr<Agent::AgentSession> session =
            Agent::AgentSession::WrapJob(pJob, Agent::AgentAuthority::Owner,
                                         inAppImageCache);
        if (session) {
            session->AttachController(m_controller);
        }
        m_agentDispatcher.reset(new Agent::AgentRpcDispatcher(std::move(session)));
    }

    // Agent autonomy selector (2026-07): stand up the two sibling
    // tool-call dispatchers alongside `m_agentDispatcher` above -- see
    // the header's ivar-block doc for why these are separate instances
    // rather than one dispatcher whose autonomy gets mutated in place.
    // Both borrow the SAME `m_controller` as `m_agentDispatcher`'s own
    // session is attached to.
    {
        std::unique_ptr<Agent::AgentSession> ownerSession =
            Agent::AgentSession::WrapJob(pJob, Agent::AgentAuthority::Owner,
                                         inAppImageCache);
        if (ownerSession) {
            ownerSession->AttachController(m_controller);
        }
        // Starts at Commit -- matches m_agentAutonomyLevel's Apply
        // default, so this dispatcher's observable behaviour is
        // identical to m_agentDispatcher's until the Qt composer
        // explicitly picks a different level.
        m_agentToolDispatcherOwner.reset(new Agent::AgentRpcDispatcher(
            std::move(ownerSession), Agent::AgentAutonomy::Commit));

        std::unique_ptr<Agent::AgentSession> proposeSession =
            Agent::AgentSession::WrapJob(pJob, Agent::AgentAuthority::External,
                                         inAppImageCache);
        if (proposeSession) {
            proposeSession->AttachController(m_controller);
            // Diagnostic only (SceneEditController::AgentProposal::sessionLabel)
            // -- distinguishes an in-app "Propose" chip proposal from an
            // external-MCP-client one in the proposals panel.
            proposeSession->SetSessionLabel("in-app-propose");
        }
        // Fixed for this dispatcher's whole life -- Propose autonomy is
        // the posture that PAIRS with External authority (AgentRpc.h's
        // file header); there is no level under which this instance
        // should ever run as anything else.
        m_agentToolDispatcherPropose.reset(new Agent::AgentRpcDispatcher(
            std::move(proposeSession), Agent::AgentAutonomy::Propose));
    }

    // Phase 6.5: hook up the C dirty-changed callback.  userData
    // is a __raw pointer to this; the controller's listener
    // outlives the trampoline (we detach in the destructor before
    // releasing the controller), so a stale-fire window is closed.
    // We marshal onto Qt's UI thread via QueuedConnection on the
    // `dirtyChanged` signal — Qt's metacall dispatches into the
    // QObject's thread, which is always the GUI thread for the
    // ViewportBridge constructed in MainWindow.
    RISE_API_SceneEditController_SetDirtyChangedCallback(
        m_controller,
        +[](void* userData, int hasUnsavedChanges) {
            auto* self = static_cast<ViewportBridge*>(userData);
            if (!self) return;
            // QMetaObject::invokeMethod with QueuedConnection lets
            // the trampoline fire from any thread without violating
            // Qt's "signal emission stays on the owning thread"
            // contract.  Receivers can connect with AutoConnection.
            QMetaObject::invokeMethod(
                self,
                "dirtyChanged",
                Qt::QueuedConnection,
                Q_ARG(bool, hasUnsavedChanges != 0));
        },
        this);
}

void ViewportBridge::scaleFilmToFit(int surfaceW, int surfaceH, int maxLongEdge)
{
    if (surfaceW <= 0 || surfaceH <= 0 || maxLongEdge <= 0) return;
    if (!m_engine) return;
    void* opaque = m_engine->opaqueJobHandle();
    if (!opaque) return;
    IJobPriv* pJob = static_cast<IJobPriv*>(opaque);
    // Route through SetViewportFit (NOT ScaleFilmToFit directly) so the Job caches the CURRENT viewport size
    // (this wrapper is the single chokepoint for both load-time and resize-time fits) -- a subsequent D2 full
    // re-derive then re-applies the same fit instead of reverting the preview to the authored full-res dims.
    pJob->SetViewportFit(
        static_cast<unsigned int>(surfaceW),
        static_cast<unsigned int>(surfaceH),
        static_cast<unsigned int>(maxLongEdge));
}

ViewportBridge::~ViewportBridge()
{
    stop();
    // Model-B F2 slice S4: deregister FIRST, before anything else here --
    // see the constructor's comment.  Safe even if a stale render is still
    // draining inside the coordinator: RISE_API_DestroySceneEditController
    // below is what actually calls Stop()/joins the worker; clearing the
    // engine's pointer here just stops any NEW production render from
    // being submitted to a controller that is about to disappear.
    if (m_engine) {
        m_engine->attachSceneEditController(nullptr);
    }
    if (m_agentDispatcher && m_agentDispatcher->Session()) {
        m_agentDispatcher->Session()->AttachController(nullptr);
    }
    m_agentDispatcher.reset();
    // Agent autonomy selector (2026-07): same detach-then-reset
    // discipline for the two sibling tool-call dispatchers -- they
    // borrow the SAME m_controller and must not outlive it either.
    if (m_agentToolDispatcherOwner && m_agentToolDispatcherOwner->Session()) {
        m_agentToolDispatcherOwner->Session()->AttachController(nullptr);
    }
    m_agentToolDispatcherOwner.reset();
    if (m_agentToolDispatcherPropose && m_agentToolDispatcherPropose->Session()) {
        m_agentToolDispatcherPropose->Session()->AttachController(nullptr);
    }
    m_agentToolDispatcherPropose.reset();
    if (m_controller) {
        // Phase 6.5: detach the dirty-changed C callback BEFORE the
        // controller (and its std::function listener) goes away so
        // the trampoline's captured `this` can't fire into a
        // half-deconstructed QObject.  Pair-balances the attach in
        // the constructor.
        RISE_API_SceneEditController_SetDirtyChangedCallback(
            m_controller, nullptr, nullptr);
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
    for (unsigned int pane = 1; pane < kViewportPaneCount; ++pane) {
        if (m_paneSinks[pane]) { m_paneSinks[pane]->release(); m_paneSinks[pane] = nullptr; }
    }
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

void ViewportBridge::startSuppressingInitialRender()
{
    if (!m_controller) return;
    RISE_API_SceneEditController_StartSuppressingInitialRender(m_controller);
    m_running = true;
}

void ViewportBridge::stop()
{
    if (!m_controller) return;
    // Retire every presentation queued by the interactive loop before
    // handing the scene to production. StopInteractive joins the controller
    // thread, but a sink may still be converting or have a UI delivery queued.
    // Without this generation bump, that stale preview can overwrite a fast
    // regional final after production reaches Completed.
    if (m_previewSink) m_previewSink->InvalidatePresentation();
    for (unsigned int pane = 1; pane < kViewportPaneCount; ++pane) {
        if (m_paneSinks[pane]) m_paneSinks[pane]->InvalidatePresentation();
    }
    // Model-B F2 slice S4 fix round 4: StopInteractive, NOT the
    // monolithic Stop() -- see this method's header doc in
    // ViewportBridge.h.  The destructor above still gets the FULL
    // teardown: it calls this stop() first, then
    // RISE_API_DestroySceneEditController (a few lines down), whose
    // destructor call to the real Stop() retires the agent worker.
    RISE_API_SceneEditController_StopInteractive(m_controller);
    // The pre-stop invalidation retires work already queued, while this
    // post-join bump closes the drain-frame race: no producer remains that
    // can capture the new generation after this point.
    if (m_previewSink) m_previewSink->InvalidatePresentation();
    for (unsigned int pane = 1; pane < kViewportPaneCount; ++pane) {
        if (m_paneSinks[pane]) m_paneSinks[pane]->InvalidatePresentation();
    }
    m_running = false;
}

bool ViewportBridge::finalizeOpenInteractions()
{
    return m_controller
        && RISE_API_SceneEditController_FinalizeOpenInteractions(m_controller);
}

void ViewportBridge::setTool(ViewportTool t)
{
    if (!m_controller) return;
    RISE_API_SceneEditController_SetTool(m_controller, static_cast<int>(t));
}

ViewportTool ViewportBridge::currentTool() const
{
    if (!m_controller) return ViewportTool::Select;
    return static_cast<ViewportTool>(
        RISE_API_SceneEditController_CurrentTool(m_controller));
}

ViewportBridge::ToolCategory ViewportBridge::categoryForTool(ViewportTool t)
{
    return static_cast<ToolCategory>(
        RISE_API_SceneEditController_CategoryForTool(static_cast<int>(t)));
}

ViewportTool ViewportBridge::defaultSubToolForCategory(ToolCategory cat)
{
    return static_cast<ViewportTool>(
        RISE_API_SceneEditController_DefaultSubToolForCategory(static_cast<int>(cat)));
}

ViewportTool ViewportBridge::lastSubToolForCategory(ToolCategory cat) const
{
    if (!m_controller) return defaultSubToolForCategory(cat);
    return static_cast<ViewportTool>(
        RISE_API_SceneEditController_GetLastSubToolForCategory(
            m_controller, static_cast<int>(cat)));
}

void ViewportBridge::refreshGizmoHandles()
{
    if (!m_controller) return;
    RISE_API_SceneEditController_RefreshGizmoHandles(m_controller);
}

QVector<ViewportBridge::GizmoHandle> ViewportBridge::gizmoHandles() const
{
    QVector<GizmoHandle> out;
    if (!m_controller) return out;
    const unsigned int n = RISE_API_SceneEditController_GizmoHandleCount(m_controller);
    out.reserve(static_cast<int>(n));
    for (unsigned int i = 0; i < n; ++i) {
        int kind = 0;
        int axis = 0;
        double x = 0, y = 0, r = 0;
        if (!RISE_API_SceneEditController_GizmoHandle(
                m_controller, i, &kind, &axis, &x, &y, &r)) {
            continue;
        }
        GizmoHandle h;
        h.kind         = static_cast<GizmoKind>(kind);
        h.axis         = axis;
        h.screenX      = x;
        h.screenY      = y;
        h.screenRadius = r;
        out.push_back(h);
    }
    return out;
}

bool ViewportBridge::gizmoDragActive() const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_IsGizmoDragActive(m_controller);
}

ViewportBridge::GizmoKind ViewportBridge::activeGizmoKind() const
{
    if (!m_controller) return GizmoKind::AxisArrow;
    const int k = RISE_API_SceneEditController_ActiveGizmoKind(m_controller);
    if (k < 0) return GizmoKind::AxisArrow;
    return static_cast<GizmoKind>(k);
}

int ViewportBridge::activeGizmoAxis() const
{
    if (!m_controller) return -1;
    return RISE_API_SceneEditController_ActiveGizmoAxis(m_controller);
}

// -------- Navigation axis-ball gizmo (Tier 2 §4) --------

bool ViewportBridge::refreshNavGizmo(double centerX, double centerY,
                                     double ballRadius, double nubRadius)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_RefreshNavGizmo(
        m_controller, centerX, centerY, ballRadius, nubRadius);
}

QVector<ViewportBridge::NavNub> ViewportBridge::navGizmoNubs() const
{
    QVector<NavNub> out;
    if (!m_controller) return out;
    const unsigned int n = RISE_API_SceneEditController_NavGizmoNubCount(m_controller);
    out.reserve(static_cast<int>(n));
    for (unsigned int i = 0; i < n; ++i) {
        int axis = 0, negative = 0, facing = 0;
        double x = 0, y = 0, r = 0;
        if (!RISE_API_SceneEditController_NavGizmoNub(
                m_controller, i, &axis, &negative, &x, &y, &r, &facing)) {
            continue;
        }
        NavNub nub;
        nub.axis         = axis;
        nub.negative     = (negative != 0);
        nub.screenX      = x;
        nub.screenY      = y;
        nub.screenRadius = r;
        nub.facing       = (facing != 0);
        out.push_back(nub);
    }
    return out;
}

int ViewportBridge::navGizmoNubAt(double x, double y) const
{
    if (!m_controller) return -1;
    return RISE_API_SceneEditController_NavGizmoNubAt(m_controller, x, y);
}

bool ViewportBridge::snapViewToAxis(int axis, bool negative)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SnapViewToAxis(m_controller, axis, negative ? 1 : 0);
}

bool ViewportBridge::enterFreeFly()
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_EnterFreeFly(m_controller);
}

bool ViewportBridge::exitFreeFly()
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_ExitFreeFly(m_controller);
}

bool ViewportBridge::isFreeFlyActive() const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_IsFreeFlyActive(m_controller);
}

bool ViewportBridge::setHomeView()
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetHomeView(m_controller);
}

bool ViewportBridge::goToHomeView()
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_GoToHomeView(m_controller);
}

bool ViewportBridge::hasHomeView() const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_HasHomeView(m_controller);
}

bool ViewportBridge::snapPaneViewToAxis(unsigned int pane, int axis, bool negative)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SnapPaneViewToAxis(
        m_controller, pane, axis, negative ? 1 : 0);
}

bool ViewportBridge::isPaneFreeFlyActive(unsigned int pane) const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_IsPaneFreeFlyActive(m_controller, pane);
}

bool ViewportBridge::paneSetHomeView(unsigned int pane)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_PaneSetHomeView(m_controller, pane);
}

bool ViewportBridge::paneGoToHomeView(unsigned int pane)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_PaneGoToHomeView(m_controller, pane);
}

QString ViewportBridge::stampViewToNewCamera(const QString& proposedName)
{
    if (!m_controller) return QString();
    char name[256] = { 0 };
    const QByteArray prop = proposedName.toUtf8();
    if (!RISE_API_SceneEditController_StampViewToNewCamera(
            m_controller, prop.constData(), name, sizeof(name))) {
        return QString();
    }
    return QString::fromUtf8(name);
}

QString ViewportBridge::stampPaneViewToNewCamera(unsigned int pane, const QString& proposedName)
{
    if (!m_controller) return QString();
    char name[256] = { 0 };
    const QByteArray prop = proposedName.toUtf8();
    if (!RISE_API_SceneEditController_PaneStampViewToNewCamera(
            m_controller, pane, prop.constData(), name, sizeof(name))) {
        return QString();
    }
    return QString::fromUtf8(name);
}

// -------- Named Views (Tier 2 §3) --------

bool ViewportBridge::captureNamedView(const QString& name)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_CaptureNamedView(
        m_controller, name.toUtf8().constData());
}

QStringList ViewportBridge::namedViewNames() const
{
    QStringList out;
    if (!m_controller) return out;
    const unsigned int n = RISE_API_SceneEditController_NamedViewCount(m_controller);
    for (unsigned int i = 0; i < n; ++i) {
        char nm[256] = { 0 };
        if (RISE_API_SceneEditController_NamedViewName(m_controller, i, nm, sizeof(nm))) {
            out.push_back(QString::fromUtf8(nm));
        }
    }
    return out;
}

bool ViewportBridge::restoreNamedView(int idx)
{
    if (!m_controller || idx < 0) return false;
    return RISE_API_SceneEditController_RestoreNamedView(
        m_controller, static_cast<unsigned int>(idx));
}

bool ViewportBridge::updateNamedView(int idx)
{
    if (!m_controller || idx < 0) return false;
    return RISE_API_SceneEditController_UpdateNamedView(
        m_controller, static_cast<unsigned int>(idx));
}

bool ViewportBridge::deleteNamedView(int idx)
{
    if (!m_controller || idx < 0) return false;
    return RISE_API_SceneEditController_DeleteNamedView(
        m_controller, static_cast<unsigned int>(idx));
}

QString ViewportBridge::promoteNamedView(int idx, const QString& proposedName)
{
    if (!m_controller || idx < 0) return QString();
    char name[256] = { 0 };
    const QByteArray prop = proposedName.toUtf8();
    if (!RISE_API_SceneEditController_PromoteNamedViewToCamera(
            m_controller, static_cast<unsigned int>(idx), prop.constData(), name, sizeof(name))) {
        return QString();
    }
    return QString::fromUtf8(name);
}

// -------- Viewport render modes (P1, docs/gui/RENDER_MODES.md §5) ------

QVector<ViewportRenderModeInfo> ViewportBridge::viewportRenderModes()
{
    QVector<ViewportRenderModeInfo> out;
    const unsigned int n = RISE_API_GetViewportRenderModeCount();
    for (unsigned int i = 0; i < n; ++i) {
        const char* name = nullptr;
        const char* title = nullptr;
        const char* question = nullptr;
        bool selectable = false;
        if (!RISE_API_GetViewportRenderModeInfo(i, &name, &title, &question, &selectable)) {
            continue;
        }
        if (!selectable) {
            continue;   // e.g. "objectmap" -- own palette-lifecycle pipeline, not a combo entry
        }
        ViewportRenderModeInfo info;
        info.name     = QString::fromUtf8(name);
        info.title    = QString::fromUtf8(title);
        info.question = QString::fromUtf8(question);
        bool wantsDenoise = false;
        RISE_API_GetViewportRenderModeWantsDenoise(i, &wantsDenoise);   // additive C-ABI; false on failure is a safe default
        info.wantsDenoise = wantsDenoise;
        bool isVariant = false;
        RISE_API_GetViewportRenderModeIsVariant(i, &isVariant);   // additive C-ABI; false on failure is a safe default
        info.isVariant = isVariant;
        out.push_back(info);
    }
    return out;
}

QString ViewportBridge::viewportRenderMode() const
{
    if (!m_controller) return QStringLiteral("preview");
    return QString::fromUtf8(RISE_API_SceneEditController_GetViewportRenderMode(m_controller));
}

bool ViewportBridge::viewportRenderModeWantsDenoise() const
{
    const QString current = viewportRenderMode();
    for (const ViewportRenderModeInfo& info : viewportRenderModes()) {
        if (info.name == current) return info.wantsDenoise;
    }
    return true;   // not found (e.g. no controller) -- default matches "preview"'s own flag
}

bool ViewportBridge::setViewportRenderMode(const QString& name)
{
    if (!m_controller) return false;
    const auto apply = [&]() {
        return RISE_API_SceneEditController_SetViewportRenderMode(
            m_controller, name.toUtf8().constData());
    };
    return m_previewSink
        ? m_previewSink->RunPresentationTransition(apply, true) : apply();
}

// -------- X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis") -----------

bool ViewportBridge::viewportXray() const
{
    if (!m_controller) return false;
    bool out = false;
    RISE_API_SceneEditController_GetViewportXray(m_controller, &out);
    return out;
}

bool ViewportBridge::setViewportXray(bool on)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetViewportXray(m_controller, on);
}

// -------- N-up multi-viewport pane model (docs/gui/RENDER_MODES.md §7) --

unsigned int ViewportBridge::paneCountForLayout(ViewportLayout layout)
{
    // Mirrors RISE::SceneEditController::PaneCountForLayout (§7.2's table);
    // not itself exposed via the C-ABI.
    switch (layout) {
    case ViewportLayout::Single:     return 1;
    case ViewportLayout::TwoH:       return 2;
    case ViewportLayout::OnePlusTwo: return 3;
    case ViewportLayout::Quad:       return 4;
    }
    return 1;
}

bool ViewportBridge::setViewportLayout(ViewportLayout layout)
{
    if (!m_controller) return false;
    const auto apply = [&]() {
        return RISE_API_SceneEditController_SetViewportLayout(
            m_controller, static_cast<int>(layout));
    };
    return layout == ViewportLayout::Single && m_previewSink
        ? m_previewSink->RunPresentationTransition(apply, true) : apply();
}

ViewportBridge::ViewportLayout ViewportBridge::viewportLayout() const
{
    if (!m_controller) return ViewportLayout::Single;
    int out = 0;
    if (!RISE_API_SceneEditController_GetViewportLayout(m_controller, &out)) {
        return ViewportLayout::Single;
    }
    return static_cast<ViewportLayout>(out);
}

bool ViewportBridge::setPrimaryPane(unsigned int pane)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetPrimaryPane(m_controller, pane);
}

unsigned int ViewportBridge::primaryPane() const
{
    if (!m_controller) return 0;
    unsigned int out = 0;
    if (!RISE_API_SceneEditController_GetPrimaryPane(m_controller, &out)) return 0;
    return out;
}

bool ViewportBridge::setPaneRenderMode(unsigned int pane, const QString& name)
{
    if (!m_controller) return false;
    const auto apply = [&]() {
        return RISE_API_SceneEditController_SetPaneRenderMode(
            m_controller, pane, name.toUtf8().constData());
    };
    if (pane == 0 && m_previewSink)
        return m_previewSink->RunPresentationTransition(apply, true);
    if (pane < kViewportPaneCount && m_paneSinks[pane])
        return m_paneSinks[pane]->RunPresentationTransition(apply, true);
    return apply();
}

QString ViewportBridge::paneRenderMode(unsigned int pane) const
{
    if (!m_controller) return QStringLiteral("preview");
    return QString::fromUtf8(RISE_API_SceneEditController_GetPaneRenderMode(m_controller, pane));
}

bool ViewportBridge::setPaneContentSource(
    unsigned int pane, PaneContentSource source)
{
    if (!m_controller) return false;
    const auto apply = [&]() {
        return RISE_API_SceneEditController_SetPaneContentSource(
            m_controller, pane, static_cast<int>(source));
    };
    const bool invalidateLast = source == PaneContentSource::Interactive;
    if (pane == 0 && m_previewSink)
        return m_previewSink->RunPresentationTransition(apply, invalidateLast);
    if (pane < kViewportPaneCount && m_paneSinks[pane])
        return m_paneSinks[pane]->RunPresentationTransition(apply, invalidateLast);
    return apply();
}

ViewportBridge::PaneContentSource
ViewportBridge::paneContentSource(unsigned int pane) const
{
    if (!m_controller) return PaneContentSource::Interactive;
    int source = 0;
    if (!RISE_API_SceneEditController_GetPaneContentSource(
            m_controller, pane, &source)) {
        return PaneContentSource::Interactive;
    }
    return source == static_cast<int>(PaneContentSource::LastRender)
        ? PaneContentSource::LastRender : PaneContentSource::Interactive;
}

bool ViewportBridge::setPaneSurfaceDims(unsigned int pane, unsigned int w, unsigned int h)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetPaneSurfaceDims(m_controller, pane, w, h);
}

bool ViewportBridge::setPaneVantageSceneCamera(unsigned int pane)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetPaneVantageSceneCamera(m_controller, pane);
}

bool ViewportBridge::setPaneVantageSceneCameraNamed(unsigned int pane, const QString& name)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetPaneVantageSceneCameraNamed(
        m_controller, pane, name.toUtf8().constData());
}

bool ViewportBridge::setPaneVantageNamedView(unsigned int pane, const QString& name)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetPaneVantageNamedView(
        m_controller, pane, name.toUtf8().constData());
}

bool ViewportBridge::paneVantage(unsigned int pane, PaneVantageKind* outKind, QString* outNamedView) const
{
    if (!m_controller) return false;
    SceneEditController::PaneVantageKind kind;
    String referencedName;
    if (!m_controller->GetPaneVantage(pane, kind, referencedName)) {
        return false;
    }
    const int wireKind = static_cast<int>(kind);
    if (wireKind < static_cast<int>(PaneVantageKind::SceneCamera)
        || wireKind > static_cast<int>(PaneVantageKind::SceneCameraNamed)) {
        return false;
    }
    if (outKind) *outKind = static_cast<PaneVantageKind>(wireKind);
    if (outNamedView) *outNamedView = QString::fromUtf8(referencedName.c_str());
    return true;
}

bool ViewportBridge::paneEnterFreeFly(unsigned int pane)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_PaneEnterFreeFly(m_controller, pane);
}

bool ViewportBridge::paneExitFreeFly(unsigned int pane)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_PaneExitFreeFly(m_controller, pane);
}

bool ViewportBridge::getPaneRefinementStatus(unsigned int pane, int* outPhase,
                                             unsigned int* outScaleDivisor) const
{
    if (!m_controller) return false;
    int phase = -1;
    unsigned int sd = 1;
    if (!RISE_API_SceneEditController_GetPaneRefinementStatus(m_controller, pane, &phase, &sd)) {
        return false;
    }
    if (outPhase) *outPhase = phase;
    if (outScaleDivisor) *outScaleDivisor = sd;
    return true;
}

bool ViewportBridge::onPanePointerDown(unsigned int pane, double x, double y)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_OnPanePointerDown(m_controller, pane, x, y);
}

bool ViewportBridge::onPanePointerMove(unsigned int pane, double x, double y)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_OnPanePointerMove(m_controller, pane, x, y);
}

bool ViewportBridge::onPanePointerUp(unsigned int pane, double x, double y)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_OnPanePointerUp(m_controller, pane, x, y);
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

int ViewportBridge::animationPresence() const
{
    if (!m_controller) return -1;
    bool hasAnimation = false;
    if (!RISE_API_SceneEditController_GetHasAnimation(m_controller, &hasAnimation)) return -1;
    return hasAnimation ? 1 : 0;
}

bool ViewportBridge::painterPreview( const QString& painterName, int defIndex,
                                      unsigned int w, unsigned int h, QByteArray& outRGBA,
                                      bool* outWasScalar, double* outRangeMin, double* outRangeMax ) const
{
    outRGBA.clear();
    if (!m_controller || painterName.isEmpty() || w == 0 || h == 0) return false;
    QByteArray buf( static_cast<int>( static_cast<qint64>( w ) * h * 4 ), Qt::Uninitialized );
    const QByteArray nameUtf8 = painterName.toUtf8();
    bool wasScalar = false;
    double rangeMin = 0, rangeMax = 0;
    if (!RISE_API_SceneEditController_PainterPreview(
            m_controller, nameUtf8.constData(), defIndex, w, h,
            reinterpret_cast<unsigned char*>( buf.data() ),
            &wasScalar, &rangeMin, &rangeMax)) {
        return false;
    }
    outRGBA = buf;
    if (outWasScalar) *outWasScalar = wasScalar;
    if (outRangeMin)  *outRangeMin  = rangeMin;
    if (outRangeMax)  *outRangeMax  = rangeMax;
    return true;
}

bool ViewportBridge::rampStripPreview( const QString& painterName,
                                        unsigned int w, unsigned int h,
                                        QByteArray& outRGBA ) const
{
    outRGBA.clear();
    if (!m_controller || painterName.isEmpty() || w == 0 || h == 0) return false;
    QByteArray buf( static_cast<int>( static_cast<qint64>( w ) * h * 4 ), Qt::Uninitialized );
    const QByteArray nameUtf8 = painterName.toUtf8();
    if (!RISE_API_SceneEditController_RampStripPreview(
            m_controller, nameUtf8.constData(), w, h,
            reinterpret_cast<unsigned char*>( buf.data() ))) {
        return false;
    }
    outRGBA = buf;
    return true;
}

// Named animations are now a first-class accordion Category
// (Category::Animation) — surfaced via the generic categoryEntities() /
// activeNameForCategory() / setSelection() methods, which pass the raw
// category int (8) straight to the C-API.  No bespoke per-feature
// accessors are needed here.

void ViewportBridge::scrubTimeBegin() { if (m_controller) RISE_API_SceneEditController_OnTimeScrubBegin(m_controller); }
void ViewportBridge::scrubTime(double t) { if (m_controller) RISE_API_SceneEditController_OnTimeScrub(m_controller, t); }
void ViewportBridge::scrubTimeEnd()   { if (m_controller) RISE_API_SceneEditController_OnTimeScrubEnd(m_controller); }

void ViewportBridge::beginPropertyScrub() { if (m_controller) RISE_API_SceneEditController_BeginPropertyScrub(m_controller); }
void ViewportBridge::endPropertyScrub()   { if (m_controller) RISE_API_SceneEditController_EndPropertyScrub(m_controller); }

void ViewportBridge::undo() { if (m_controller) RISE_API_SceneEditController_Undo(m_controller); }
void ViewportBridge::redo() { if (m_controller) RISE_API_SceneEditController_Redo(m_controller); }

QString ViewportBridge::undoActionLabel() const
{
    if (!m_controller) return QString();
    char buf[256] = {0};
    if (!RISE_API_SceneEditController_UndoLabel(m_controller, buf, sizeof(buf))) return QString();
    return QString::fromUtf8(buf);
}

QString ViewportBridge::redoActionLabel() const
{
    if (!m_controller) return QString();
    char buf[256] = {0};
    if (!RISE_API_SceneEditController_RedoLabel(m_controller, buf, sizeof(buf))) return QString();
    return QString::fromUtf8(buf);
}

// ---- Refinement pause + status (UI redesign, design brief A2) ------

void ViewportBridge::pauseRefinement()
{
    if (!m_controller) return;
    RISE_API_SceneEditController_PauseRefinement(m_controller);
}

void ViewportBridge::resumeRefinement()
{
    if (!m_controller) return;
    RISE_API_SceneEditController_ResumeRefinement(m_controller);
}

bool ViewportBridge::isRefinementPaused() const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_IsRefinementPaused(m_controller);
}

int ViewportBridge::refinementPhase(unsigned int* outScaleDivisor) const
{
    if (!m_controller) {
        if (outScaleDivisor) *outScaleDivisor = 1;
        return -1;
    }
    return RISE_API_SceneEditController_GetRefinementStatus(m_controller, outScaleDivisor);
}

// ---- Interactive region-of-interest (UI redesign, A4) ---------------

void ViewportBridge::setInteractiveRegion(unsigned int left, unsigned int top,
                                           unsigned int right, unsigned int bottom)
{
    if (!m_controller) return;
    RISE_API_SceneEditController_SetInteractiveRegion(m_controller, left, top, right, bottom);
}

void ViewportBridge::clearInteractiveRegion()
{
    if (!m_controller) return;
    RISE_API_SceneEditController_ClearInteractiveRegion(m_controller);
}

bool ViewportBridge::getInteractiveRegion(unsigned int* left, unsigned int* top,
                                           unsigned int* right, unsigned int* bottom) const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_GetInteractiveRegion(m_controller, left, top, right, bottom);
}

bool ViewportBridge::interactiveRasterizerHonorsRegion() const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_InteractiveRasterizerHonorsRegion(m_controller);
}

// ---- Editor live-sync (UI refinement item 1) ------------------------

QString ViewportBridge::serializedSceneText() const
{
    if (!m_controller) return QString();
    char* text = RISE_API_SceneEditController_SerializedSceneTextAlloc(m_controller);
    if (!text) return QString();
    QString out = QString::fromUtf8(text);
    RISE_API_FreeString(text);
    return out;
}

bool ViewportBridge::getSceneTextVersion(quint64* outUuid, quint64* outRevision) const
{
    if (outUuid)     *outUuid = 0;
    if (outRevision) *outRevision = 0;
    if (!m_controller) return false;
    return RISE_API_SceneEditController_GetSceneTextVersion(m_controller, outUuid, outRevision);
}

// ---- Phase 6.5 scene-file save -------------------------------------

bool ViewportBridge::hasUnsavedSceneChanges() const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_HasUnsavedChanges(m_controller);
}

QString ViewportBridge::loadedFilePath() const
{
    return m_engine ? m_engine->loadedFilePath() : QString();
}

ViewportBridge::SaveStatus ViewportBridge::saveSceneTo(
    const QString& path,
    QString& outError)
{
    outError.clear();
    if (!m_controller || path.isEmpty()) {
        outError = QStringLiteral("no scene loaded");
        return SaveStatus::Error;
    }
    // QByteArray keeps the UTF-8 alive across the C-call.
    const QByteArray utf8 = path.toUtf8();
    char errBuf[1024] = {0};
    const int status = RISE_API_SceneEditController_RequestSave(
        m_controller,
        utf8.constData(),
        errBuf,
        sizeof(errBuf));
    if (errBuf[0] != '\0') {
        outError = QString::fromUtf8(errBuf);
    }
    const SaveStatus rs = static_cast<SaveStatus>(status);

    // Phase 6.5: on either successful outcome with a Save-As target
    // (path != current
    // loadedFilePath), re-anchor the engine's record so subsequent
    // in-place saves target the file we just wrote.  Matches the
    // library's FileIdentity re-anchor inside SaveEngine.  NoOp still
    // re-anchors: the chosen target already contains byte-identical CST.
    if ((rs == SaveStatus::Saved || rs == SaveStatus::NoOp) && m_engine) {
        if (m_engine->loadedFilePath() != path) {
            m_engine->setLoadedFilePath(path);
        }
    }
    return rs;
}

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

QString ViewportBridge::agentHandleLine(const QString& jsonRpcRequest)
{
    static const char* const kNoDispatcher =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
        "{\"code\":-32603,\"message\":\"internal error: agent dispatcher unavailable\"}}";
    if (!m_agentDispatcher) {
        return QString::fromUtf8(kNoDispatcher);
    }

    const QByteArray utf8 = jsonRpcRequest.toUtf8();
    const std::string response =
        m_agentDispatcher->HandleLine(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    return QString::fromUtf8(response.c_str());
}

qint64 ViewportBridge::agentHeadVersionRevision() const
{
    if (!m_agentDispatcher || !m_agentDispatcher->Session()) return -1;
    return static_cast<qint64>(m_agentDispatcher->Session()->ReadHeadVersion().revision);
}

bool ViewportBridge::agentReadDocumentSnapshot(std::string& outText, uint64_t& outUuid,
                                                uint64_t& outRevision) const
{
    if (!m_agentDispatcher || !m_agentDispatcher->Session()) return false;
    const RISE::Agent::AgentSession::AgentDocumentSnapshot snap =
        m_agentDispatcher->Session()->ReadDocumentSnapshot();
    if (!snap.hasDocument) return false;
    outText = snap.document;
    outUuid = snap.headVersion.uuid;
    outRevision = snap.headVersion.revision;
    return true;
}

void ViewportBridge::setAgentAutonomyLevel(AgentAutonomyLevel level)
{
    if (level != AgentAutonomyLevel::Read && level != AgentAutonomyLevel::Propose
        && level != AgentAutonomyLevel::Apply) {
        return;   // out-of-range: no-op, keep the previous level (see the .h doc)
    }
    m_agentAutonomyLevel = level;
    // Only the tool-call OWNER session's autonomy ever changes at runtime
    // -- m_agentToolDispatcherPropose stays fixed at Propose for its
    // whole life, and m_agentDispatcher (the administrative path) is
    // never touched here at all.
    if (m_agentToolDispatcherOwner) {
        m_agentToolDispatcherOwner->SetAutonomy(
            level == AgentAutonomyLevel::Read ? Agent::AgentAutonomy::Read
                                               : Agent::AgentAutonomy::Commit);
    }
}

QString ViewportBridge::agentHandleToolCall(const QString& jsonRpcRequest)
{
    // The plain form is the level-explicit form applied to whatever the
    // composer chip currently says -- so a tool call issued WITHOUT a pin
    // always reflects the user's newest safety choice (see the .h doc for
    // why only a render JOB pins, never a whole turn).
    return agentHandleToolCall(jsonRpcRequest, m_agentAutonomyLevel);
}

QString ViewportBridge::agentHandleToolCall(const QString& jsonRpcRequest,
                                            AgentAutonomyLevel level)
{
    static const char* const kNoDispatcher =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
        "{\"code\":-32603,\"message\":\"internal error: agent dispatcher unavailable\"}}";

    // An out-of-range `level` falls to the tool-call Owner session
    // (m_agentToolDispatcherOwner), matching
    // setAgentAutonomyLevel()'s "keep a valid posture" no-op policy rather
    // than dispatching to nothing.
    Agent::AgentRpcDispatcher* dispatcher =
        (level == AgentAutonomyLevel::Propose)
            ? m_agentToolDispatcherPropose.get()
            : m_agentToolDispatcherOwner.get();
    if (!dispatcher) {
        return QString::fromUtf8(kNoDispatcher);
    }

    const QByteArray utf8 = jsonRpcRequest.toUtf8();
    const std::string response =
        dispatcher->HandleLine(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    return QString::fromUtf8(response.c_str());
}

void ViewportBridge::agentSetImageGenerator(const QString& providerName, const QString& apiKey)
{
    const QByteArray providerUtf8 = providerName.toUtf8();
    const QByteArray keyUtf8      = apiKey.toUtf8();
    const std::string provider(providerUtf8.constData(), static_cast<std::size_t>(providerUtf8.size()));
    const std::string key(keyUtf8.constData(), static_cast<std::size_t>(keyUtf8.size()));

    // ONE system transport, shared by all three sessions' generators
    // below -- safe because every in-app dispatcher is called only from
    // the UI thread (mirrors macOS RISEViewportBridge's identical note).
    // CreateSystemChatHttpTransport never returns null.
    const std::shared_ptr<Agent::IChatHttpTransport> transport(
        Agent::CreateSystemChatHttpTransport().release());

    const Agent::ChatImageGenerator wire =
        Agent::MakeChatImageGenerator(provider, key, transport);

    Agent::AgentSession::AgentImageGenerator gen;
    gen.providerName = wire.providerName;
    gen.supported     = wire.supported;
    gen.modelId       = wire.modelId;
    if (wire.supported) {
        const std::function<Agent::ChatImageGenOutcome(const std::string&)> rawGenerate = wire.generate;
        gen.generate =
            [rawGenerate](const std::string& description) -> Agent::AgentSession::AgentImageGenOutcome {
            Agent::AgentSession::AgentImageGenOutcome out;
            const Agent::ChatImageGenOutcome r = rawGenerate(description);
            out.ok       = r.ok;
            out.bytes    = r.bytes;
            out.mimeType = r.mimeType;
            out.error    = r.error;
            return out;
        };
    }

    // S2 (2026-08-11): the TEXT sibling -- `build_element`'s builder
    // completion, from the SAME provider name, key and shared transport,
    // installed on the SAME three sessions in the SAME loop below, so a
    // credential change reinstalls both capabilities or neither.  Mirrors
    // macOS RISEViewportBridge's identical block.
    const Agent::ChatTextCompleter wireText =
        Agent::MakeChatTextCompleter(provider, key, transport);

    Agent::AgentSession::AgentTextCompleter comp;
    comp.providerName = wireText.providerName;
    comp.supported     = wireText.supported;
    comp.modelId       = wireText.modelId;
    if (wireText.supported) {
        const std::function<Agent::ChatTextCompletionOutcome(const std::string&)> rawComplete =
            wireText.complete;
        comp.complete =
            [rawComplete](const std::string& prompt) -> Agent::AgentSession::AgentTextCompletionOutcome {
            Agent::AgentSession::AgentTextCompletionOutcome out;
            const Agent::ChatTextCompletionOutcome r = rawComplete(prompt);
            out.ok    = r.ok;
            out.text  = r.text;
            out.error = r.error;
            return out;
        };
    }

    // Every in-app tool-call-reachable session, mirroring macOS
    // RISEViewportBridge's identical three-dispatcher loop.
    Agent::AgentRpcDispatcher* dispatchers[] = {
        m_agentDispatcher.get(), m_agentToolDispatcherOwner.get(), m_agentToolDispatcherPropose.get()};
    for (Agent::AgentRpcDispatcher* d : dispatchers) {
        if (d && d->Session()) {
            d->Session()->SetImageGenerator(gen);
            d->Session()->SetTextCompleter(comp);
        }
    }
}

void ViewportBridge::agentNoteFinalAnswer()
{
    // The SAME three in-app sessions the generator install above touches,
    // and for the same reason: they serve one document, so the turn that
    // just ended is the same turn for all of them.  Idempotent and one-way
    // (AgentSession.h's block above SessionMode), so calling it on every
    // final turn is correct and costs nothing.
    Agent::AgentRpcDispatcher* dispatchers[] = {
        m_agentDispatcher.get(), m_agentToolDispatcherOwner.get(), m_agentToolDispatcherPropose.get()};
    for (Agent::AgentRpcDispatcher* d : dispatchers) {
        if (d && d->Session()) d->Session()->NoteFinalAnswer();
    }
}

ViewportBridge::PanelMode ViewportBridge::panelMode() const
{
    if (!m_controller) return PanelMode::None;
    const int m = RISE_API_SceneEditController_PanelMode(m_controller);
    switch (m) {
        case 1: return PanelMode::Camera;
        case 2: return PanelMode::Rasterizer;
        case 3: return PanelMode::Object;
        case 4: return PanelMode::Light;
        case 5: return PanelMode::Film;
        case 6: return PanelMode::Material;
        case 7: return PanelMode::Medium;
        default: return PanelMode::None;
    }
}

QStringList ViewportBridge::categoryEntities(Category cat) const
{
    QStringList out;
    if (!m_controller) return out;
    const int catInt = static_cast<int>(cat);
    const unsigned int n = RISE_API_SceneEditController_CategoryEntityCount(m_controller, catInt);
    out.reserve(static_cast<int>(n));
    for (unsigned int i = 0; i < n; ++i) {
        const String name = m_controller->CategoryEntityName(
            static_cast<SceneEditController::Category>(catInt), i);
        out.append(QString::fromUtf8(name.c_str()));
    }
    return out;
}

SceneTree ViewportBridge::categoryTree(Category cat) const
{
    SceneTree out;
    if (!m_controller) return out;

    // ONE TRANSACTIONAL READ.  SceneEditController::ReadTree refreshes once
    // and then copies the whole published tree out under a single hold of the
    // snapshot lock, so what lands here cannot be a mixture of two trees --
    // which a walk built out of the per-node getters CAN be, because each of
    // those takes the lock on its own and another thread's count call may
    // republish between two of them.
    //
    // That is now an API PROPERTY of ReadTree, not a property of how this
    // function happens to be written: the Swift shell (4b) reads the same
    // guarantee off the declaration instead of having to rediscover it.
    //
    // The indices in the returned tree are RAW indices into its own node
    // table -- not the generation-tagged handles the per-node getters hand
    // out -- because this copy is ours and nothing can republish underneath
    // it.  They map one-to-one onto SceneTreeNode positions below.
    SceneEditController::AuthoredTree t;
    m_controller->ReadTree(static_cast<SceneEditController::Category>(cat), t);

    const int n = static_cast<int>(t.nodes.size());
    if (n == 0) return out;
    out.nodes.resize(n);

    for (int i = 0; i < n; ++i) {
        const SceneEditController::TreeNodeRow& row = t.nodes[static_cast<std::size_t>(i)];
        SceneTreeNode& node = out.nodes[i];
        node.name = QString::fromUtf8(row.name.c_str());
        // kInvalidNodeIndex means ROOT, which the model draws as a top-level
        // row -- that is what the -1 default already says.
        if (row.parent != SceneEditController::kInvalidNodeIndex
         && row.parent < static_cast<unsigned int>(n)) {
            node.parent = static_cast<int>(row.parent);
        }
        node.children.reserve(static_cast<int>(row.childCount));
        for (unsigned int k = 0; k < row.childCount; ++k) {
            const std::size_t slot = static_cast<std::size_t>(row.firstChild) + k;
            if (slot >= t.childIndices.size()) break;
            const unsigned int child = t.childIndices[slot];
            if (child < static_cast<unsigned int>(n)) node.children.append(static_cast<int>(child));
        }
    }

    // THE ROOT LIST IS COPIED, NOT RE-DERIVED.  Scanning `out.nodes` for
    // `parent == -1` would agree today, but only because BuildAuthoredTree
    // emits its table in presentation order -- see SceneTree's doc.  Out-of-
    // range entries are dropped for the same belt-and-braces reason the
    // parent/child indices above are range-tested: a shell must never be able
    // to subscript past the node table.
    out.roots.reserve(static_cast<int>(t.roots.size()));
    for (std::size_t r = 0; r < t.roots.size(); ++r) {
        if (t.roots[r] < static_cast<unsigned int>(n)) {
            out.roots.append(static_cast<int>(t.roots[r]));
        }
    }
    return out;
}

// doc-88 Phase 3 S14 (docs/gui/NODE_GRAPH_CANVAS.md sect. 6) -----------

namespace {
    // Shared body for the outEdges/inEdges conversion loop below -- the
    // only difference between the two call sites is which
    // SceneEditController::GraphPort vector is being walked.
    QVector<PainterGraphPort> ConvertGraphPorts(
        const std::vector<SceneEditController::GraphPort>& ports)
    {
        QVector<PainterGraphPort> out;
        out.reserve(static_cast<int>(ports.size()));
        for (const SceneEditController::GraphPort& p : ports) {
            PainterGraphPort pp;
            pp.paramName = QString::fromUtf8(p.paramName.c_str());
            pp.occurrence = p.occurrence;
            pp.otherNodeIndex = (p.otherNode == SceneEditController::kInvalidNodeIndex)
                ? -1 : static_cast<int>(p.otherNode);
            pp.otherName = QString::fromUtf8(p.otherName.c_str());
            out.append(pp);
        }
        return out;
    }
}

PainterGraph ViewportBridge::painterMaterialGraph() const
{
    PainterGraph out;
    if (!m_controller) return out;

    // ONE TRANSACTIONAL READ, the SAME discipline categoryTree() documents
    // above: SceneEditController::ReadPainterMaterialGraphLaidOut composes
    // S11's graph + S13's saved sidecar positions + S12's auto-layout
    // fill-in under one snapshot-lock hold (plus one small sidecar file
    // read -- see that method's own header comment), so what lands here
    // cannot straddle two different published generations. Called
    // directly on the C++ controller, not through RISE_API_*: this file
    // already calls SceneEditController natively (see categoryTree()
    // above) -- the C ABI's PainterGraph* surface exists for a caller
    // that cannot see C++ at all, and is per-node -- exactly the
    // non-transactional shape this method exists to avoid.
    SceneEditController::PainterMaterialGraphLaidOut g;
    m_controller->ReadPainterMaterialGraphLaidOut(g);

    const std::size_t n = g.graph.nodes.size();
    out.nodes.resize(static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const SceneEditController::GraphNode& gn = g.graph.nodes[i];
        const SceneEditController::GraphNodePosition& pos = g.positions[i];
        PainterGraphNode& node = out.nodes[static_cast<int>(i)];
        node.handle       = gn.handle;
        node.name         = QString::fromUtf8(gn.name.c_str());
        node.chunkKeyword = QString::fromUtf8(gn.chunkKeyword.c_str());
        node.category     = static_cast<int>(gn.category);
        node.defCount     = gn.defCount;
        node.x = pos.x;
        node.y = pos.y;
        node.outEdges = ConvertGraphPorts(gn.outEdges);
        node.inEdges  = ConvertGraphPorts(gn.inEdges);
    }
    out.generation = g.graph.generation;
    return out;
}

QString ViewportBridge::activeNameForCategory(Category cat) const
{
    if (!m_controller) return QString();
    const int catInt = static_cast<int>(cat);
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_CategoryActiveName(m_controller, catInt, buf, sizeof(buf))) {
        return QString();
    }
    return QString::fromUtf8(buf);
}

ViewportBridge::Category ViewportBridge::selectionCategory() const
{
    if (!m_controller) return Category::None;
    const int c = RISE_API_SceneEditController_GetSelectionCategory(m_controller);
    switch (c) {
        case 1: return Category::Camera;
        case 2: return Category::Rasterizer;
        case 3: return Category::Object;
        case 4: return Category::Light;
        case 5: return Category::Film;
        case 6: return Category::Material;
        case 7: return Category::Medium;
        case 8: return Category::Animation;
        case 9: return Category::SceneVariant;
        case 10: return Category::Painter;
        case 11: return Category::Geometry;   // GUI redesign 2026-07-22
        default: return Category::None;
    }
}

QString ViewportBridge::selectionName() const
{
    if (!m_controller) return QString();
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_GetSelectionName(m_controller, buf, sizeof(buf))) {
        return QString();
    }
    return QString::fromUtf8(buf);
}

QString ViewportBridge::selectionRowName() const
{
    if (!m_controller) return QString();
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_GetSelectionRowName(m_controller, buf, sizeof(buf))) {
        return QString();
    }
    return QString::fromUtf8(buf);
}

bool ViewportBridge::setSelection(Category cat, const QString& name)
{
    if (!m_controller) return false;
    const QByteArray utf8 = name.toUtf8();
    return RISE_API_SceneEditController_SetSelection(
        m_controller,
        static_cast<int>(cat),
        utf8.constData());
}

unsigned int ViewportBridge::sceneEpoch() const
{
    if (!m_controller) return 0;
    return RISE_API_SceneEditController_SceneEpoch(m_controller);
}

bool ViewportBridge::getEntitySourceLocation(Category category, const QString& name,
                                              quint64* outByteOffset, quint32* outLine) const
{
    if (outByteOffset) *outByteOffset = 0;
    if (outLine)        *outLine       = 0;
    if (!m_controller || name.isEmpty()) return false;
    const QByteArray utf8 = name.toUtf8();
    return RISE_API_SceneEditController_GetEntitySourceLocation(
        m_controller, static_cast<int>(category), utf8.constData(),
        outByteOffset, outLine);
}

// ---- Source traceability (any UI element <-> scene-file span) ------

bool ViewportBridge::resolveSourceSpan(Category cat, const QString& name, const QString& param,
                                        int occ, quint64* outOffset, quint64* outLength,
                                        quint32* outLine, quint32* outColumn) const
{
    if (outOffset) *outOffset = 0;
    if (outLength) *outLength = 0;
    if (outLine)   *outLine   = 0;
    if (outColumn) *outColumn = 0;
    if (!m_controller) return false;
    const QByteArray nameUtf8  = name.toUtf8();
    const QByteArray paramUtf8 = param.toUtf8();
    return RISE_API_SceneEditController_ResolveSourceSpan(
        m_controller, static_cast<int>(cat),
        nameUtf8.constData(), paramUtf8.constData(), occ,
        outOffset, outLength, outLine, outColumn);
}

bool ViewportBridge::sourceRefAtByteOffset(quint64 offset, Category* outCat, QString* outName,
                                            QString* outParam, int* outOccurrence) const
{
    if (outName)       *outName       = QString();
    if (outParam)      *outParam      = QString();
    if (outOccurrence) *outOccurrence = 0;
    if (!m_controller) return false;
    int catInt = 0;
    int occ = 0;
    char nameBuf[256] = {0};
    char paramBuf[128] = {0};
    if (!RISE_API_SceneEditController_SourceRefAtByteOffset(
            m_controller, offset, &catInt,
            nameBuf, sizeof(nameBuf), paramBuf, sizeof(paramBuf), &occ)) {
        return false;
    }
    if (outCat)        *outCat        = static_cast<Category>(catInt);
    if (outName)       *outName       = QString::fromUtf8(nameBuf);
    if (outParam)      *outParam      = QString::fromUtf8(paramBuf);
    if (outOccurrence) *outOccurrence = occ;
    return true;
}

// ---- Entity creation + painter CRUD (entity-creation slice) --------

unsigned int ViewportBridge::entityTemplateCount(Category category) const
{
    if (!m_controller) return 0;
    return RISE_API_SceneEditController_EntityTemplateCount(
        m_controller, static_cast<int>(category));
}

QString ViewportBridge::entityTemplateLabel(Category category, unsigned int idx) const
{
    if (!m_controller) return QString();
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_EntityTemplateLabel(
            m_controller, static_cast<int>(category), idx, buf, sizeof(buf))) {
        return QString();
    }
    return QString::fromUtf8(buf);
}

bool ViewportBridge::instantiateEntityTemplate(Category category, unsigned int idx,
                                                QString* outName, QString* outMessage)
{
    if (!m_controller) return false;
    char nameBuf[256] = {0};
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const bool applied = RISE_API_SceneEditController_InstantiateEntityTemplate(
        m_controller, static_cast<int>(category), idx,
        nameBuf, sizeof(nameBuf),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf));
    if (outName && nameBuf[0] != '\0') *outName = QString::fromUtf8(nameBuf);
    if (outMessage && messageBuf[0] != '\0') *outMessage = QString::fromUtf8(messageBuf);
    return applied;
}

bool ViewportBridge::duplicateEntity(Category category, const QString& name,
                                      QString* outName, QString* outMessage)
{
    if (!m_controller || name.isEmpty()) return false;
    const QByteArray utf8 = name.toUtf8();
    char nameBuf[256] = {0};
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const bool applied = RISE_API_SceneEditController_DuplicateEntity(
        m_controller, static_cast<int>(category), utf8.constData(),
        nameBuf, sizeof(nameBuf),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf));
    if (outName && nameBuf[0] != '\0') *outName = QString::fromUtf8(nameBuf);
    if (outMessage && messageBuf[0] != '\0') *outMessage = QString::fromUtf8(messageBuf);
    return applied;
}

bool ViewportBridge::removeEntity(Category category, const QString& name, QString* outMessage)
{
    if (!m_controller || name.isEmpty()) return false;
    const QByteArray utf8 = name.toUtf8();
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const bool applied = RISE_API_SceneEditController_RemoveEntity(
        m_controller, static_cast<int>(category), utf8.constData(),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf));
    if (outMessage && messageBuf[0] != '\0') *outMessage = QString::fromUtf8(messageBuf);
    return applied;
}

// ---- Node-graph canvas: create node (S18) ---------------------------
//
// Carried mirror of the macOS bridge's identically-named section; see
// the header's STANDING CAVEAT (this half is owed an MSVC build).

QVector<ViewportBridge::ChunkNodeRequirement>
ViewportBridge::chunkNodeRequirements(const QString& keyword) const
{
    QVector<ChunkNodeRequirement> out;
    if (!m_controller || keyword.isEmpty()) return out;
    const QByteArray kw = keyword.toUtf8();
    const unsigned int n = RISE_API_SceneEditController_ChunkNodeRequiredArgCount(
        m_controller, kw.constData());
    out.reserve(static_cast<int>(n));
    for (unsigned int i = 0; i < n; ++i) {
        char paramBuf[128] = {0};
        char descBuf[1024] = {0};
        int isRef = 0;
        if (!RISE_API_SceneEditController_ChunkNodeRequiredArg(
                m_controller, kw.constData(), i,
                paramBuf, sizeof(paramBuf),
                descBuf, sizeof(descBuf), &isRef)) {
            continue;
        }
        ChunkNodeRequirement r;
        r.param       = QString::fromUtf8(paramBuf);
        r.description = QString::fromUtf8(descBuf);
        r.isReference = (isRef != 0);
        out.push_back(r);
    }
    return out;
}

bool ViewportBridge::createChunkNode(const QString& keyword, const QString& baseName,
                                      const QStringList& argParams,
                                      const QStringList& argValues,
                                      QString* outName, QString* outMessage)
{
    if (!m_controller || keyword.isEmpty()) return false;
    const QByteArray kw   = keyword.toUtf8();
    const QByteArray base = baseName.toUtf8();

    // round-1 P2-c: ORDERED parallel lists, not a QMap -- a keyed map can
    // neither repeat a param (voronoi's repeatable `gen`) nor preserve
    // caller order (QMap iterates in key-sorted order, not insertion
    // order), and the C ABI beneath is itself two parallel arrays.  A
    // count mismatch is a caller bug; clip to the shorter length rather
    // than reading past either list's end.  The QByteArray temporaries
    // must outlive the call -- hold them in `keep` and only then take
    // .constData() pointers into the two parallel arrays the ABI expects.
    const int n = qMin(argParams.size(), argValues.size());
    QVector<QByteArray> keep;
    keep.reserve(n * 2);
    for (int i = 0; i < n; ++i) {
        keep.push_back(argParams[i].toUtf8());
        keep.push_back(argValues[i].toUtf8());
    }
    std::vector<const char*> params, values;
    params.reserve(static_cast<size_t>(n));
    values.reserve(static_cast<size_t>(n));
    for (int i = 0; i + 1 < keep.size(); i += 2) {
        params.push_back(keep[i].constData());
        values.push_back(keep[i + 1].constData());
    }

    char nameBuf[256] = {0};
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const bool applied = RISE_API_SceneEditController_CreateChunkNode(
        m_controller, kw.constData(), base.constData(),
        params.empty() ? nullptr : params.data(),
        values.empty() ? nullptr : values.data(),
        static_cast<unsigned int>(params.size()),
        nameBuf, sizeof(nameBuf),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf));
    if (outName && nameBuf[0] != '\0')       *outName    = QString::fromUtf8(nameBuf);
    if (outMessage && messageBuf[0] != '\0') *outMessage = QString::fromUtf8(messageBuf);
    return applied;
}

// ---- Node-graph canvas: rewire a connection (S19) --------------------
//
// Carried mirror of the macOS bridge's identically-named section; see
// the header's STANDING CAVEAT (this half is owed an MSVC build).

namespace {

// Split a '\n'-JOINED name list (the C ABI's list convention -- see
// RISE_API.h's RewireConnection doc) back into a QStringList.  An empty
// buffer means an EMPTY list, not a list containing one empty string,
// which a naive QString::split would produce.
QStringList splitJoinedNames(const char* buf)
{
    if (!buf || buf[0] == '\0') return QStringList();
    return QString::fromUtf8(buf).split(QChar('\n'));
}

}  // namespace

bool ViewportBridge::rewireConnection(int targetCategory, const QString& targetName,
                                       const QString& param, int occurrence,
                                       int newRefCategory, const QString& newRefName,
                                       RewireOutcome* outOutcome)
{
    if (outOutcome) *outOutcome = RewireOutcome();
    if (!m_controller || targetName.isEmpty() || param.isEmpty()) return false;

    const QByteArray tgt = targetName.toUtf8();
    const QByteArray prm = param.toUtf8();
    const QByteArray ref = newRefName.toUtf8();

    int closure = 0, legality = 0, cycle = 0;
    char statusBuf[64] = {0};
    char messageBuf[2048] = {0};
    char sharedBuf[1024] = {0};
    char referrersBuf[2048] = {0};
    char ownersBuf[1024] = {0};
    char orphanBuf[1024] = {0};

    const bool applied = RISE_API_SceneEditController_RewireConnection(
        m_controller,
        targetCategory, tgt.constData(),
        prm.constData(), occurrence,
        newRefCategory, ref.constData(),
        &closure, &legality, &cycle,
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf),
        sharedBuf, sizeof(sharedBuf),
        referrersBuf, sizeof(referrersBuf),
        ownersBuf, sizeof(ownersBuf),
        orphanBuf, sizeof(orphanBuf));

    if (outOutcome) {
        outOutcome->applied               = applied;
        outOutcome->status                = QString::fromUtf8(statusBuf);
        outOutcome->message               = QString::fromUtf8(messageBuf);
        outOutcome->closure               = static_cast<RewireClosure>(closure);
        outOutcome->legalityRefused       = (legality != 0);
        outOutcome->cycleRefused          = (cycle != 0);
        outOutcome->sharedChunks          = splitJoinedNames(sharedBuf);
        outOutcome->outOfClosureReferrers = splitJoinedNames(referrersBuf);
        outOutcome->owners                = splitJoinedNames(ownersBuf);
        outOutcome->nowUnreferenced       = splitJoinedNames(orphanBuf);
    }
    return applied;
}

// ---- Node-graph canvas: reference-safe delete + duplicate (S20) ------

bool ViewportBridge::deleteGraphNode(int category, const QString& name,
                                      GraphDeleteMode mode,
                                      DeleteOutcome* outOutcome)
{
    if (outOutcome) *outOutcome = DeleteOutcome();
    if (!m_controller || name.isEmpty()) return false;

    const QByteArray nm = name.toUtf8();

    int closure = 0, refRefused = 0, cascadeRefused = 0;
    char statusBuf[64] = {0};
    char messageBuf[2048] = {0};
    char referrersBuf[2048] = {0};
    char removedBuf[2048] = {0};

    const bool applied = RISE_API_SceneEditController_DeleteGraphNode(
        m_controller, category, nm.constData(),
        (mode == GraphDeleteMode::Cascade) ? 1 : 0,
        &closure, &refRefused, &cascadeRefused,
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf),
        referrersBuf, sizeof(referrersBuf),
        removedBuf, sizeof(removedBuf));

    if (outOutcome) {
        outOutcome->applied          = applied;
        outOutcome->status           = QString::fromUtf8(statusBuf);
        outOutcome->message          = QString::fromUtf8(messageBuf);
        outOutcome->closure          = static_cast<RewireClosure>(closure);
        outOutcome->referenceRefused = (refRefused != 0);
        outOutcome->cascadeRefused   = (cascadeRefused != 0);
        outOutcome->referrers        = splitJoinedNames(referrersBuf);
        outOutcome->removed          = splitJoinedNames(removedBuf);
    }
    return applied;
}

bool ViewportBridge::duplicateGraphNode(int category, const QString& name,
                                         DuplicateOutcome* outOutcome)
{
    if (outOutcome) *outOutcome = DuplicateOutcome();
    if (!m_controller || name.isEmpty()) return false;

    const QByteArray nm = name.toUtf8();

    int closure = 0, originalIndex = -1;
    char newNameBuf[256] = {0};
    char statusBuf[64] = {0};
    char messageBuf[2048] = {0};

    const bool applied = RISE_API_SceneEditController_DuplicateGraphNode(
        m_controller, category, nm.constData(),
        &closure, &originalIndex,
        newNameBuf, sizeof(newNameBuf),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf));

    if (outOutcome) {
        outOutcome->applied       = applied;
        outOutcome->status        = QString::fromUtf8(statusBuf);
        outOutcome->message       = QString::fromUtf8(messageBuf);
        outOutcome->closure       = static_cast<RewireClosure>(closure);
        outOutcome->newName       = QString::fromUtf8(newNameBuf);
        outOutcome->originalIndex = originalIndex;
    }
    return applied;
}

// ---- Environment / IBL section --------------------------------------

bool ViewportBridge::environmentInfo(EnvironmentInfo* out) const
{
    if (!m_controller || !out) return false;
    int hasEnv = 0, procSky = 0, editable = 0, background = 0;
    char nameBuf[256] = {0};
    char fileBuf[1024] = {0};
    double scale = 1.0, ox = 0.0, oy = 0.0, oz = 0.0;
    if (!RISE_API_SceneEditController_GetEnvironment(
            m_controller, &hasEnv, &procSky, &editable,
            nameBuf, sizeof(nameBuf), fileBuf, sizeof(fileBuf),
            &scale, &ox, &oy, &oz, &background)) {
        return false;
    }
    out->hasEnvironment = (hasEnv != 0);
    out->proceduralSky  = (procSky != 0);
    out->editable       = (editable != 0);
    out->painterName    = QString::fromUtf8(nameBuf);
    out->file           = QString::fromUtf8(fileBuf);
    out->scale          = scale;
    out->orientX        = ox;
    out->orientY        = oy;
    out->orientZ        = oz;
    out->background     = (background != 0);
    return true;
}

bool ViewportBridge::setEnvironmentScale(double scale)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetEnvironmentScale(m_controller, scale);
}

bool ViewportBridge::setEnvironmentBackground(bool background)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetEnvironmentBackground(m_controller, background ? 1 : 0);
}

bool ViewportBridge::setEnvironmentOrient(double xDeg, double yDeg, double zDeg)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetEnvironmentOrient(m_controller, xDeg, yDeg, zDeg);
}

bool ViewportBridge::setEnvironmentFile(const QString& absPath)
{
    if (!m_controller || absPath.isEmpty()) return false;
    const QByteArray utf8 = absPath.toUtf8();
    return RISE_API_SceneEditController_SetEnvironmentFile(m_controller, utf8.constData());
}

bool ViewportBridge::addEnvironment(const QString& hdriPath, QString* outName, QString* outMessage)
{
    if (!m_controller || hdriPath.isEmpty()) return false;
    const QByteArray utf8 = hdriPath.toUtf8();
    char nameBuf[256] = {0};
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const bool applied = RISE_API_SceneEditController_AddEnvironment(
        m_controller, utf8.constData(),
        nameBuf, sizeof(nameBuf),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf));
    if (outName && nameBuf[0] != '\0') *outName = QString::fromUtf8(nameBuf);
    if (outMessage && messageBuf[0] != '\0') *outMessage = QString::fromUtf8(messageBuf);
    return applied;
}

bool ViewportBridge::removeEnvironment()
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_RemoveEnvironment(m_controller);
}

QString ViewportBridge::addCameraFromActive(const QString& proposedName)
{
    if (!m_controller) return QString();
    const QByteArray utf8 = proposedName.toUtf8();
    // 256 bytes covers any realistic camera name; the controller-side
    // dedup loop won't produce names longer than ~base+10 chars before
    // its timestamp-suffix fallback.
    char outName[256] = {0};
    if (!RISE_API_SceneEditController_AddCameraFromActive(
            m_controller, utf8.constData(), outName, sizeof(outName))) {
        return QString();
    }
    return QString::fromUtf8(outName);
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
    // Generously sized so future descriptors with longer labels
    // (multi-byte UTF-8 for non-ASCII names) can grow without churn.
    // QString::fromUtf8 silently substitutes replacement chars on
    // invalid sequences, so a truncation mid-codepoint is at worst
    // a cosmetic glitch rather than a crash.
    char presetLabelBuf[256];
    char presetValueBuf[256];
    char unitLabelBuf[64];
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
        p.index = static_cast<int>(i);   // jump-to-definition queries by snapshot index

        // Forward the descriptor's quick-pick presets — empty for
        // parameters that declared none, in which case the panel
        // falls through to a plain line edit.
        const unsigned int numPresets = RISE_API_SceneEditController_PropertyPresetCount(m_controller, i);
        p.presets.reserve(static_cast<int>(numPresets));
        for (unsigned int j = 0; j < numPresets; ++j) {
            if (!RISE_API_SceneEditController_PropertyPresetLabel(m_controller, i, j, presetLabelBuf, sizeof(presetLabelBuf))) continue;
            if (!RISE_API_SceneEditController_PropertyPresetValue(m_controller, i, j, presetValueBuf, sizeof(presetValueBuf))) continue;
            ViewportPropertyPreset preset;
            preset.label = QString::fromUtf8(presetLabelBuf);
            preset.value = QString::fromUtf8(presetValueBuf);
            p.presets.append(preset);
        }

        // Unit label — small suffix the panel renders next to the
        // value field ("mm" / "°" / "scene units" / empty).
        if (RISE_API_SceneEditController_PropertyUnitLabel(m_controller, i, unitLabelBuf, sizeof(unitLabelBuf))) {
            p.unitLabel = QString::fromUtf8(unitLabelBuf);
        }

        // doc 88 S4b — the row's authored numeric range (see ViewportProperty's
        // own doc for the carried-not-yet-rendered caveat).
        p.hasRange  = RISE_API_SceneEditController_PropertyHasRange(m_controller, i);
        p.rangeMin  = RISE_API_SceneEditController_PropertyRangeMin(m_controller, i);
        p.rangeMax  = RISE_API_SceneEditController_PropertyRangeMax(m_controller, i);
        p.rangeStep = RISE_API_SceneEditController_PropertyRangeStep(m_controller, i);

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

QVector<ViewportProperty> ViewportBridge::propertySnapshotFor(Category cat)
{
    QVector<ViewportProperty> out;
    if (!m_controller) return out;
    // `RefreshProperties` (called by `propertySnapshot()` when the
    // panel re-paints) populates per-category snapshots in one pass;
    // we don't need to refresh again here.  Callers should ensure
    // `propertySnapshot()` was called first (the panel's refresh path
    // already does this; multi-section panels read multiple
    // `propertySnapshotFor` after one refresh).
    const int catInt = static_cast<int>(cat);
    const unsigned int n = RISE_API_SceneEditController_PropertyCountFor(m_controller, catInt);
    out.reserve(static_cast<int>(n));
    char nameBuf[128];
    char valBuf[256];
    char descBuf[512];
    char presetLabelBuf[256];
    char presetValueBuf[256];
    char unitLabelBuf[64];
    for (unsigned int i = 0; i < n; ++i) {
        RISE_API_SceneEditController_PropertyNameFor(m_controller, catInt, i, nameBuf, sizeof(nameBuf));
        RISE_API_SceneEditController_PropertyValueFor(m_controller, catInt, i, valBuf, sizeof(valBuf));
        RISE_API_SceneEditController_PropertyDescriptionFor(m_controller, catInt, i, descBuf, sizeof(descBuf));
        ViewportProperty p;
        p.name = QString::fromUtf8(nameBuf);
        p.value = QString::fromUtf8(valBuf);
        p.description = QString::fromUtf8(descBuf);
        p.kind = RISE_API_SceneEditController_PropertyKindFor(m_controller, catInt, i);
        p.index = static_cast<int>(i);   // jump-to-definition queries by snapshot index
        p.editable = RISE_API_SceneEditController_PropertyEditableFor(m_controller, catInt, i);

        const unsigned int numPresets = RISE_API_SceneEditController_PropertyPresetCountFor(m_controller, catInt, i);
        p.presets.reserve(static_cast<int>(numPresets));
        for (unsigned int j = 0; j < numPresets; ++j) {
            if (!RISE_API_SceneEditController_PropertyPresetLabelFor(m_controller, catInt, i, j, presetLabelBuf, sizeof(presetLabelBuf))) continue;
            if (!RISE_API_SceneEditController_PropertyPresetValueFor(m_controller, catInt, i, j, presetValueBuf, sizeof(presetValueBuf))) continue;
            ViewportPropertyPreset preset;
            preset.label = QString::fromUtf8(presetLabelBuf);
            preset.value = QString::fromUtf8(presetValueBuf);
            p.presets.append(preset);
        }

        if (RISE_API_SceneEditController_PropertyUnitLabelFor(m_controller, catInt, i, unitLabelBuf, sizeof(unitLabelBuf))) {
            p.unitLabel = QString::fromUtf8(unitLabelBuf);
        }

        // doc 88 S4b — same as the selection-scoped snapshot above; this is the
        // path the properties panel actually reads.
        p.hasRange  = RISE_API_SceneEditController_PropertyHasRangeFor(m_controller, catInt, i);
        p.rangeMin  = RISE_API_SceneEditController_PropertyRangeMinFor(m_controller, catInt, i);
        p.rangeMax  = RISE_API_SceneEditController_PropertyRangeMaxFor(m_controller, catInt, i);
        p.rangeStep = RISE_API_SceneEditController_PropertyRangeStepFor(m_controller, catInt, i);

        out.append(p);
    }
    return out;
}

bool ViewportBridge::propertyJumpTargetFor(Category cat, int index,
                                           Category* outCat, QString* outName)
{
    if (!m_controller || !outCat || !outName || index < 0) return false;
    int rawCat = 0;
    char nameBuf[128] = { 0 };
    if (!RISE_API_SceneEditController_PropertyJumpTargetForCategory(
            m_controller, static_cast<int>(cat), static_cast<unsigned int>(index),
            &rawCat, nameBuf, sizeof(nameBuf))) {
        return false;
    }
    // Same int->enum translation discipline as selectionCategory() (the
    // bridge-enum-translation audit): unknown ints fail closed.
    switch (rawCat) {
        case 1:  *outCat = Category::Camera;   break;
        case 3:  *outCat = Category::Object;   break;
        case 4:  *outCat = Category::Light;    break;
        case 6:  *outCat = Category::Material; break;
        case 7:  *outCat = Category::Medium;   break;
        case 10: *outCat = Category::Painter;  break;
        case 11: *outCat = Category::Geometry; break;
        default: return false;
    }
    *outName = QString::fromUtf8(nameBuf);
    return !outName->isEmpty();
}

bool ViewportBridge::setPropertyForCategory(Category cat, const QString& name, const QString& value)
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_SetPropertyForCategory(
        m_controller, static_cast<int>(cat),
        name.toUtf8().constData(), value.toUtf8().constData());
}

QString ViewportBridge::selectionNameForCategory(Category cat) const
{
    if (!m_controller) return QString();
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_GetSelectionForCategory(
            m_controller, static_cast<int>(cat), buf, sizeof(buf))) {
        return QString();
    }
    return QString::fromUtf8(buf);
}

bool ViewportBridge::isSectionExpanded(Category cat) const
{
    if (!m_controller) return false;
    return RISE_API_SceneEditController_IsSectionExpanded(
        m_controller, static_cast<int>(cat));
}

void ViewportBridge::collapseSection(Category cat)
{
    if (!m_controller) return;
    RISE_API_SceneEditController_CollapseSection(
        m_controller, static_cast<int>(cat));
}
