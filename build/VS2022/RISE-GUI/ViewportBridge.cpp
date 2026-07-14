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
#include "Agent/AgentSession.h"
#include "Agent/AgentRpc.h"

using namespace RISE;

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
    explicit ViewportPreviewSink(ViewportBridge* bridge) : m_bridge(bridge) {}
    ~ViewportPreviewSink() override = default;

    // Borrowed; the bridge keeps the controller alive for the sink's
    // lifetime.  Used to query IsCancelRequested at end-of-pass.
    void SetController(SceneEditController* c) { m_controller = c; }

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
                     const unsigned int /*frame*/) override {
        if (!m_bridge) return;
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

    {
        std::unique_ptr<Agent::AgentSession> session =
            Agent::AgentSession::WrapJob(pJob);
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
            Agent::AgentSession::WrapJob(pJob);
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
            Agent::AgentSession::WrapJob(pJob, Agent::AgentAuthority::External);
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
    // Model-B F2 slice S4 fix round 4: StopInteractive, NOT the
    // monolithic Stop() -- see this method's header doc in
    // ViewportBridge.h.  The destructor above still gets the FULL
    // teardown: it calls this stop() first, then
    // RISE_API_DestroySceneEditController (a few lines down), whose
    // destructor call to the real Stop() retires the agent worker.
    RISE_API_SceneEditController_StopInteractive(m_controller);
    m_running = false;
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

    // Phase 6.5: on Saved with a Save-As target (path != current
    // loadedFilePath), re-anchor the engine's record so subsequent
    // in-place saves target the file we just wrote.  Matches the
    // library's FileIdentity re-anchor inside SaveEngine.
    if (rs == SaveStatus::Saved && m_engine) {
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

void ViewportBridge::setAgentAutonomyLevel(AgentAutonomyLevel level)
{
    if (level != AgentAutonomyLevel::Read && level != AgentAutonomyLevel::Propose
        && level != AgentAutonomyLevel::Apply) {
        return;   // out-of-range: no-op, keep the previous level (see the .h doc)
    }
    m_agentAutonomyLevel = level;
    // Only the OWNER tool dispatcher's autonomy ever changes at runtime
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
    static const char* const kNoDispatcher =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
        "{\"code\":-32603,\"message\":\"internal error: agent dispatcher unavailable\"}}";

    Agent::AgentRpcDispatcher* dispatcher =
        (m_agentAutonomyLevel == AgentAutonomyLevel::Propose)
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
    char nameBuf[128];
    for (unsigned int i = 0; i < n; ++i) {
        if (RISE_API_SceneEditController_CategoryEntityName(m_controller, catInt, i, nameBuf, sizeof(nameBuf))) {
            out.append(QString::fromUtf8(nameBuf));
        }
    }
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

        out.append(p);
    }
    return out;
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
