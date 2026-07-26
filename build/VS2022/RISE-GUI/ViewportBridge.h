//////////////////////////////////////////////////////////////////////
//
//  ViewportBridge.h - Qt wrapper around the C++ SceneEditController
//    for the Windows interactive 3D viewport.  Mirrors the macOS
//    RISEViewportBridge class.
//
//  Lifetime: borrows an existing RenderEngine's IJobPriv; the engine
//  must outlive the bridge.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTBRIDGE_H
#define VIEWPORTBRIDGE_H

#include <QObject>
#include <QImage>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <memory>

class RenderEngine;

namespace RISE {
    class SceneEditController;
    class IRayCaster;
    class IRasterizer;
    namespace Agent {
        class AgentRpcDispatcher;
    }
}
class ViewportPreviewSink;

/// Single quick-pick preset; mirrors ParameterPreset in
/// src/Library/Parsers/ChunkDescriptor.h.
struct ViewportPropertyPreset {
    QString label;          // human-readable, shown in the combo box
    QString value;          // parser-acceptable literal written through SetProperty
};

/// One row of the properties panel — descriptor-driven, mirrors
/// CameraProperty in src/Library/SceneEditor/CameraIntrospection.h.
struct ViewportProperty {
    QString name;
    QString value;
    QString description;
    int     kind = 0;       // ValueKind cast to int
    bool    editable = false;
    QVector<ViewportPropertyPreset> presets;   // empty when descriptor declared no presets
    QString unitLabel;                         // short suffix shown next to the field — "mm", "°", "scene units", or empty
    int     index = -1;                        // snapshot position (the C-ABI property index) — jump-to-definition queries by index
};

/// Tool enum mirroring SceneEditController::Tool and the C-API
/// SceneEditTool_* constants.  Kept as an int-backed enum so the
/// MOC-generated metadata stays simple.
enum class ViewportTool {
    Select          = 0,
    TranslateObject = 1,
    RotateObject    = 2,
    ScaleObject     = 3,
    OrbitCamera     = 4,
    PanCamera       = 5,
    ZoomCamera      = 6,
    ScrubTimeline   = 7,
    RollCamera      = 8
};

/// Snapshot of the scene's IBL environment (a scene-level singleton: an
/// hdr/exr painter bound via `radiance_*` on the active rasterizer, NOT an
/// ILight).  Mirrors SceneEditController::EnvironmentInfo and the macOS
/// RISEEnvironmentInfo.  Filled by ViewportBridge::environmentInfo().
struct EnvironmentInfo {
    bool    hasEnvironment = false;   ///< a radiance_map painter is bound
    bool    proceduralSky  = false;   ///< a procedural sky / non-painter map is installed (read-only)
    bool    editable       = false;   ///< false when the active rasterizer takes no radiance map (MLT / pixel*)
    QString painterName;              ///< bound painter name (empty if none)
    QString file;                     ///< resolved HDRI path (empty if unresolved / procedural)
    double  scale          = 1.0;     ///< intensity multiplier
    double  orientX        = 0.0;     ///< Euler rotation X in DEGREES
    double  orientY        = 0.0;     ///< Euler rotation Y in DEGREES
    double  orientZ        = 0.0;     ///< Euler rotation Z in DEGREES
    bool    background     = true;    ///< map visible behind geometry
};

/// One viewport render mode entry (P1, docs/gui/RENDER_MODES.md §4/§5) --
/// ONLY the viewportSelectable subset of the registry (today: "preview",
/// "normals", "depth", "facets", "wireframe" -- NOT "objectmap", which has
/// its own palette-lifecycle pipeline).  Mirrors the UI-facing fields of
/// RISE::Implementation::ViewportRenderModeInfo; `name` is the wire
/// identifier (C-ABI / agent tool / persistence), `title` is the combo
/// box label, `question` is the tooltip ("the question this mode
/// answers" -- see RENDER_MODES.md §3).
struct ViewportRenderModeInfo {
    QString name;
    QString title;
    QString question;
    /// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): the registry's
    /// `wantsDenoise` flag, read via the additive
    /// RISE_API_GetViewportRenderModeWantsDenoise alongside name/title/
    /// question -- the DENOISED-label formatter (TopBar's
    /// ComputeRefinementStatus) keys off this instead of a hardcoded
    /// `mode == "preview"` comparison now that BeautyVariant modes
    /// (deep_reflect/direct) genuinely denoise too.
    bool wantsDenoise = false;
    /// P2a review fix: the registry's `IsBeautyVariantMode` flag, read via
    /// the additive RISE_API_GetViewportRenderModeIsVariant.  True for
    /// `deep_reflect`/`direct` -- those modes drive a wholly separate
    /// ephemeral PT pipeline (`mVariantRasterizer`) that never reads the
    /// x-ray flag, so TopBar disables `m_xrayBtn` while the active mode has
    /// this set (see TopBar::refreshRenderModeCombo).
    bool isVariant = false;
};

class ViewportBridge : public QObject
{
    Q_OBJECT

public:
    explicit ViewportBridge(RenderEngine* engine, QObject* parent = nullptr);
    ~ViewportBridge() override;

    /// True if the live-preview rasterizer was successfully constructed
    /// against the loaded scene (false = skeleton mode; edits still
    /// apply but the user has to click Render to see them).
    bool hasLivePreview() const { return m_interactiveRasterizer != nullptr; }

    /// Spawn the C++ render thread.  Idempotent.
    void start();
    /// Spawn the C++ render thread WITHOUT its one-shot initial render
    /// pass.  Call this (instead of `start`) when restarting the
    /// viewport right after a production render: the finished render is
    /// already on screen and the render thread stays parked until the
    /// user interacts, so the production image survives.  This avoids
    /// the "render flashes then flips back to the live preview" bug —
    /// the interactive rasterizer never produces the overwriting frame
    /// in the first place, so no display-layer frame-suppression (which
    /// only ever covered the LDR sink path, not the HDR observer path)
    /// is needed.
    void startSuppressingInitialRender();
    /// Stop the interactive render thread and join.  Idempotent.  Does
    /// NOT touch the attached SceneEditController's agent-render worker
    /// -- a production render submitted immediately afterward (via
    /// RunProductionRenderThroughController, which
    /// RenderEngine::startRender/startAnimationRender call) is still
    /// served normally.
    ///
    /// Model-B F2 slice S4 fix round 4: this used to call the C++
    /// controller's monolithic Stop(), which ALSO permanently retired
    /// the agent-render worker (mAgentRenderStop is a one-shot flag; the
    /// worker thread is spawned only once, in the constructor, and
    /// nothing ever respawns it) -- so MainWindow::onRender/
    /// onRenderAnimation calling this immediately before
    /// m_engine->startRender()/startAnimationRender() poisoned the
    /// controller for the rest of its lifetime, and the production
    /// render (and every later one) was refused with "controller
    /// stopped".  Wired to RISE_API_SceneEditController_StopInteractive
    /// instead; see that function's doc.
    void stop();
    /// Persist and close any controller-owned gesture before the shell drops
    /// its matching release event. Safe when no interaction is active.
    bool finalizeOpenInteractions();
    bool isRunning() const { return m_running; }

    /// Shrink the scene Film so the interactive preview renders at a
    /// screen-appropriate resolution rather than blindly inheriting
    /// whatever the .RISEscene file declared.  Wraps
    /// `IJobPriv::SetViewportFit`, which caches the fit params and
    /// applies the fit immediately; the cache lets a subsequent D2
    /// full re-derive (variant switch / CST edit) re-apply the SAME
    /// fit so the preview stays screen-sized instead of jumping to
    /// authored full-res.  Never upscales, preserves the scene's
    /// authored aspect ratio + pixelAR.  Caller passes the available
    /// rendering-surface dims in window pixels; the long edge is also
    /// capped at `maxLongEdge`.  Call once after the bridge is
    /// constructed and before `start()`.
    void scaleFilmToFit(int surfaceW, int surfaceH, int maxLongEdge);

    void setTool(ViewportTool t);
    ViewportTool currentTool() const;

    /// Photoshop-style toolbar category — the "slot" a tool sits in.
    /// Mirrors `RISE::SceneEditController::ToolCategory`.  Numeric
    /// values are part of the C-API contract.
    enum class ToolCategory : int {
        Select          = 0,
        Camera          = 1,
        ObjectTransform = 2
    };

    /// Map a tool to its category.  Pure-function — no bridge state.
    /// Qt UI uses this to compute the active slot for the current
    /// `currentTool()`.
    static ToolCategory categoryForTool(ViewportTool t);

    /// Default sub-tool the category's slot shows before the user
    /// picks anything from the flyout.  Pure-function.
    static ViewportTool defaultSubToolForCategory(ToolCategory cat);

    /// Photoshop "last-used" memory: returns the sub-tool the user
    /// most recently picked from this category's flyout, or the
    /// category default if nothing's been picked yet.
    ViewportTool lastSubToolForCategory(ToolCategory cat) const;

    // Gizmo overlay -------------------------------------------------

    /// Kind of gizmo handle — what UI gesture the platform overlay
    /// binds to it.  Mirrors `RISE::SceneEditController::GizmoHandle::Kind`.
    enum class GizmoKind : int {
        AxisArrow        = 0,
        AxisPlane        = 1,
        ScreenCenter     = 2,
        AxisRing         = 3,
        ScreenRing       = 4,
        AxisScaleHandle  = 5,
        UniformScaleCube = 6
    };

    /// One gizmo handle.  Positions are in the camera's CURRENT
    /// image-pixel space — the QPainter overlay maps to widget
    /// coords using the same `cameraSurfaceDimensions()` ratio
    /// pointer events use.
    struct GizmoHandle {
        GizmoKind kind        = GizmoKind::AxisArrow;
        int       axis        = -1;     ///< 0=X, 1=Y, 2=Z; -1 for screen-aligned
        double    screenX     = 0.0;
        double    screenY     = 0.0;
        double    screenRadius = 0.0;
    };

    /// Recompute the gizmo handle array.  Caller invokes this once
    /// per preview frame before reading `gizmoHandles()`.  No-op
    /// when the active tool isn't in ObjectTransform, no Object is
    /// selected, or the camera is degenerate.
    void refreshGizmoHandles();

    /// Snapshot of the current gizmo handle array (empty when no
    /// gizmo is currently shown).  Returns a fresh copy so values
    /// stay valid even if the controller refreshes internally.
    QVector<GizmoHandle> gizmoHandles() const;

    /// True iff a gizmo handle was hit on the most recent pointer-
    /// down and the drag is still active.  Drives the overlay's
    /// active-handle highlight on/off.
    bool gizmoDragActive() const;

    /// Active drag handle kind / axis, or sentinel values when no
    /// drag is in progress: kind defaults to AxisArrow, axis to -1.
    /// Use together with `gizmoDragActive()` for unambiguous state.
    GizmoKind activeGizmoKind() const;
    int       activeGizmoAxis() const;

    // -------- Navigation axis-ball gizmo (Tier 2 §4) --------

    /// One nav-gizmo nub.  Positions are in the widget space the overlay
    /// passed to refreshNavGizmo() (and the same space it feeds navGizmoNubAt).
    struct NavNub {
        int    axis         = 0;      ///< 0=X, 1=Y, 2=Z
        bool   negative     = false;  ///< false=+axis, true=−axis
        double screenX      = 0.0;
        double screenY      = 0.0;
        double screenRadius = 0.0;
        bool   facing       = true;   ///< true=toward viewer (bright)
    };

    /// Recompute the six ±X/±Y/±Z nubs for a ball centered at (centerX,
    /// centerY) with `ballRadius`, each nub `nubRadius`, all in widget space.
    /// Returns false (empty array) when there's no supported (pinhole)
    /// interactive camera.  Call once per paint before reading navGizmoNubs().
    bool refreshNavGizmo(double centerX, double centerY,
                         double ballRadius, double nubRadius);

    /// Snapshot of the current nub array (empty when not shown).
    QVector<NavNub> navGizmoNubs() const;

    /// Hit-test a widget-space point against the nubs (front-facing win ties).
    /// Returns the nub index or -1.
    int navGizmoNubAt(double x, double y) const;

    // View navigation (Tier 2 §4-5): non-destructive — drive the transient
    // free-fly ViewportPose (the interactive pass renders through it) and
    // NEVER mutate a scene camera.  Each returns true on success.
    bool snapViewToAxis(int axis, bool negative);
    bool enterFreeFly();
    bool exitFreeFly();
    bool isFreeFlyActive() const;
    bool setHomeView();
    bool goToHomeView();
    bool hasHomeView() const;

    // N-up navigation targets the primary pane, rather than the legacy
    // pane-0 aliases above.  The nav overlay is the only caller.
    bool snapPaneViewToAxis(unsigned int pane, int axis, bool negative);
    bool isPaneFreeFlyActive(unsigned int pane) const;
    bool paneSetHomeView(unsigned int pane);
    bool paneGoToHomeView(unsigned int pane);

    /// B3 fly-then-stamp: promote the current free-fly view into a NEW named
    /// scene camera (named from a CST-safe canonicalization of `proposedName`,
    /// then dedup-suffixed; the new camera becomes active). Returns the
    /// created camera's name, or an empty
    /// QString when there's no free-fly pose to stamp / the edit was refused.
    QString stampViewToNewCamera(const QString& proposedName);
    QString stampPaneViewToNewCamera(unsigned int pane, const QString& proposedName);

    // -------- Named Views (Tier 2 §3) --------
    /// Capture the current view (free-fly pose if active, else the active
    /// camera) as a new named view.  False when there's no capturable camera.
    bool captureNamedView(const QString& name);
    /// The named views' names, in order.
    QStringList namedViewNames() const;
    /// Restore view `idx` into the transient ViewportPose (non-destructive).
    bool restoreNamedView(int idx);
    /// Re-capture the current view into slot `idx`.
    bool updateNamedView(int idx);
    /// Remove view `idx`.
    bool deleteNamedView(int idx);
    /// Promote view `idx` into a NEW scene camera (named from a CST-safe
    /// canonicalization of `proposedName`; becomes active). Returns the
    /// created camera's name, or empty on refusal.
    QString promoteNamedView(int idx, const QString& proposedName);

    // -------- Viewport render modes (P1, docs/gui/RENDER_MODES.md §5) --
    // Mirrors the RISE_API_SceneEditController_{Set,Get}ViewportRenderMode /
    // RISE_API_GetViewportRenderMode{Count,Info} C exports.  The registry
    // itself is controller-independent (static strings, no allocation) --
    // `viewportRenderModes()` is a pure function, like `categoryForTool`
    // above, safe to call before any scene loads.  The current-mode
    // getter/setter DO need a live controller and mirror that C-ABI's own
    // null-controller fallback ("preview").

    /// All viewportSelectable modes, in registry order.  Pure function --
    /// no bridge state, callable with no scene loaded.
    static QVector<ViewportRenderModeInfo> viewportRenderModes();

    /// The registry wire name of the CURRENTLY active viewport render
    /// mode ("preview" when no controller is attached, or after every
    /// scene load/reload -- SceneEditController resets to Preview on
    /// every whole-scene rebind; this bridge never assumes that stays
    /// true and always re-reads).
    QString viewportRenderMode() const;

    /// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): the CURRENTLY
    /// active mode's `wantsDenoise` flag, looked up from
    /// `viewportRenderModes()` by the current `viewportRenderMode()` name.
    /// Defaults to true (matching "preview"'s own flag and the pre-fix
    /// behaviour) when the current mode isn't found in the registry list
    /// (e.g. no controller attached).
    bool viewportRenderModeWantsDenoise() const;

    /// Switch the interactive viewport to render-mode `name` (a wire
    /// name from `viewportRenderModes()`).  Returns false on a null
    /// controller, an unknown/non-selectable name, or a controller-level
    /// refusal (a production/agent render owns the scene, or skeleton
    /// mode with no interactive rasterizer) -- the set CAN fail, so
    /// callers must re-read `viewportRenderMode()` afterward rather than
    /// assume the requested mode took effect.
    bool setViewportRenderMode(const QString& name);

    // -------- X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis") -------
    // Mirrors the RISE_API_SceneEditController_{Set,Get}ViewportXray C
    // exports.  An orthogonal boolean that applies to EVERY viewport
    // render mode, INCLUDING "preview" (not just the four data modes:
    // normals/depth/facets/wireframe) -- resolution lives in the caster
    // layer, so it composes with whichever mode is active rather than
    // being scoped to the data modes.  DEFAULT ON: the viewport starts
    // see-through, and SceneEditController resets the flag back to ON
    // on every whole-scene rebind (RebindEditorToJob).

    /// The CURRENT x-ray flag ("false" when no controller is attached --
    /// same null-controller fallback convention as `viewportRenderMode()`
    /// -- or while a render owns the scene, per the C-ABI's documented
    /// never-blocks contract).  Otherwise true by default and after every
    /// scene rebind.
    bool viewportXray() const;

    /// Set the x-ray flag.  Applies immediately regardless of which mode
    /// is active (including preview).  Returns false on a null controller
    /// or a controller-level refusal (a production/agent render owns the
    /// scene, or skeleton mode) -- the set CAN fail, so callers must
    /// re-read `viewportXray()` afterward rather than assume the
    /// requested value took effect.
    bool setViewportXray(bool on);

    // -------- N-up multi-viewport pane model (docs/gui/RENDER_MODES.md §7) -
    // Mirrors the RISE_API_SceneEditController_{Set,Get}ViewportLayout /
    // {Set,Get}PrimaryPane / {Set,Get}PaneRenderMode / SetPaneSurfaceDims /
    // SetPaneVantage{SceneCamera,NamedView,SceneCameraNamed} / GetPaneVantage /
    // OnPanePointer{Down,Move,Up} / Pane{Enter,Exit}FreeFly /
    // GetPaneRefinementStatus C exports.  Four ALWAYS-PRESENT pane slots
    // (kViewportPaneCount); the layout selects the visible subset.  Pane 0
    // is an ALIAS VIEW of the existing single-viewport state -- every
    // un-indexed call above (setViewportRenderMode, pointerDown/Move/Up,
    // enterFreeFly/exitFreeFly, refinementPhase, ...) keeps operating on
    // pane 0 unchanged, so Single-layout behaviour is byte-identical to
    // before this section existed.  Every setter here is fail-closed like
    // every other viewport setter: unknown pane / hidden pane / render owns
    // scene => false, nothing mutated -- callers must re-read afterward
    // rather than assume the requested value took effect (same discipline
    // as setViewportRenderMode / setViewportXray above).

    /// Mirrors RISE::SceneEditController::kViewportPaneCount.
    static constexpr unsigned int kViewportPaneCount = 4;

    /// Mirrors RISE::SceneEditController::ViewportLayout.  Numeric values
    /// are part of the C-API contract.
    enum class ViewportLayout : int {
        Single     = 0,   ///< pane 0 only (today's viewport)
        TwoH       = 1,   ///< panes 0 | 1 side-by-side
        OnePlusTwo = 2,   ///< pane 0 big + panes 1,2 stacked right
        Quad       = 3    ///< panes 0-3 in a 2x2 grid
    };

    /// Mirrors RISE::SceneEditController::PaneVantageKind.
    enum class PaneVantageKind : int {
        SceneCamera      = 0,  ///< track the live active scene camera
        FreeFly          = 1,  ///< per-pane free-fly pose (never mutates the scene camera)
        NamedView        = 2,  ///< re-resolved by name each pass
        SceneCameraNamed = 3   ///< track one manager-registered scene camera by name
    };

    enum class PaneContentSource : int {
        Interactive = 0,
        LastRender  = 1
    };

    /// Panes a layout makes visible: Single=1, TwoH=2, OnePlusTwo=3, Quad=4.
    /// Pure function -- no bridge state, mirrors the core's
    /// PaneCountForLayout table (docs/gui/RENDER_MODES.md §7.2); not itself
    /// exposed via the C-ABI, so re-derived here rather than round-tripped.
    static unsigned int paneCountForLayout(ViewportLayout layout);

    bool setViewportLayout(ViewportLayout layout);
    ViewportLayout viewportLayout() const;

    /// Make `pane` primary (owner of editing/picking/gizmos).  False on an
    /// invalid or hidden pane, or while a render owns the scene.
    bool setPrimaryPane(unsigned int pane);
    unsigned int primaryPane() const;

    /// Pane 0 forwards to setViewportRenderMode (the alias contract above).
    bool setPaneRenderMode(unsigned int pane, const QString& name);
    /// "preview" on a null controller / invalid pane (mirrors
    /// viewportRenderMode()'s fail-closed default).
    QString paneRenderMode(unsigned int pane) const;
    bool setPaneContentSource(unsigned int pane, PaneContentSource source);
    PaneContentSource paneContentSource(unsigned int pane) const;

    /// Pane render-surface pixel dims (the GUI's pane rect -- caller passes
    /// actual DEVICE pixels, i.e. widget points times devicePixelRatioF()).
    /// 0/0 resets to "use the film's rest dims".  Applies to visible panes
    /// only.
    bool setPaneSurfaceDims(unsigned int pane, unsigned int w, unsigned int h);

    bool setPaneVantageSceneCamera(unsigned int pane);
    bool setPaneVantageSceneCameraNamed(unsigned int pane, const QString& name);
    bool setPaneVantageNamedView(unsigned int pane, const QString& name);
    /// Introspection: current vantage kind (+ referenced name for
    /// NamedView/SceneCameraNamed, "" otherwise). Returns false on null controller,
    /// invalid pane, or an unrecognized kind from the C-ABI.
    bool paneVantage(unsigned int pane, PaneVantageKind* outKind, QString* outNamedView) const;

    /// Per-pane free-fly twins (§7.4).  Pane 0 forwards to
    /// enterFreeFly()/exitFreeFly().
    bool paneEnterFreeFly(unsigned int pane);
    bool paneExitFreeFly(unsigned int pane);

    /// Per-pane refinement status: phase (0 Idle / 1 Rendering / 2
    /// Refining / 3 Polishing / 4 Paused -- same contract as
    /// refinementPhase()) + scale divisor.  Returns false on null
    /// controller / invalid pane.
    bool getPaneRefinementStatus(unsigned int pane, int* outPhase,
                                  unsigned int* outScaleDivisor) const;

    /// Pane-indexed pointer input -- coordinates are in THAT PANE's local
    /// surface pixel space (see ViewportWidget's per-pane coordinate
    /// mapping).  Down returns false when the gesture must be dropped
    /// (hidden pane / render owns scene) -- callers must NOT forward the
    /// rest of that physical gesture's Move/Up when Down refused it.
    bool onPanePointerDown(unsigned int pane, double x, double y);
    bool onPanePointerMove(unsigned int pane, double x, double y);
    bool onPanePointerUp(unsigned int pane, double x, double y);

    // Pointer events — coordinates are in viewport surface pixel space.
    void pointerDown(double x, double y);
    void pointerMove(double x, double y);
    void pointerUp(double x, double y);

    /// Stable full-resolution camera dimensions for pointer-event
    /// coord conversion in the ViewportWidget.  The rasterized
    /// QImage's size shrinks during a fast drag (preview-scale
    /// subsampling); using QImage::size() as the conversion target
    /// makes mLastPx (captured at one scale level) and the next
    /// pointer event (in another) live in mismatched coord spaces,
    /// producing 4×–32× pan/orbit jumps when the scale state machine
    /// steps.  This getter returns the camera's canonical full-res
    /// dims directly from the controller, so the widget's
    /// surface-point math stays stable across subsampling.  Returns
    /// QSize() when no camera is attached.
    QSize cameraSurfaceDimensions() const;

    /// Scene's animation options for sizing the timeline scrubber.
    /// Returns the values from the scene's `animation_options` chunk
    /// (defaults to time=[0,1], 30 frames if not declared).  Returns
    /// false on null controller / no job attached.
    bool animationOptions(double& timeStart, double& timeEnd, unsigned int& numFrames) const;

    // Named animations are surfaced as a first-class accordion Category
    // (Category::Animation) — the generic categoryEntities() /
    // activeNameForCategory() / setSelection() surface lists + activate
    // them, so there are no bespoke animation accessors here.
    // animationOptions() above already reflects the active animation's
    // options.

    void scrubTimeBegin();
    void scrubTime(double t);
    void scrubTimeEnd();

    /// Bracket a click-and-drag scrub on a property's chevron handle.
    /// The controller bumps the preview-scale divisor between Begin
    /// and End so the rapid-fire SetProperty stream doesn't cancel
    /// every in-flight render before the outer tiles get a chance.
    void beginPropertyScrub();
    void endPropertyScrub();

    void undo();
    void redo();

    /// Human-readable label of the next Undo/Redo step ("Translate",
    /// "Agent Edit", ...) for the Edit menu's dynamic item titles.
    /// Empty string when the corresponding stack is empty (or no
    /// controller is attached).
    QString undoActionLabel() const;
    QString redoActionLabel() const;

    // ---- Refinement pause + status (UI redesign, design brief A2) --
    // Mirrors the macOS RISEViewportBridge pauseRefinement /
    // resumeRefinement / isRefinementPaused / refinementPhase.

    /// Freeze the interactive refinement ladder at its current rung.
    /// No-op when no controller is attached or already paused.
    void pauseRefinement();
    /// Resume after pauseRefinement().  No-op when not paused.
    void resumeRefinement();
    bool isRefinementPaused() const;

    /// Returns the RefinementPhase as int (0 Idle, 1 Rendering,
    /// 2 Refining, 3 Polishing, 4 Paused); -1 when no controller is
    /// attached.  `*outScaleDivisor` receives the preview-scale
    /// divisor (1..32, 1 = full resolution) when non-null.
    int refinementPhase(unsigned int* outScaleDivisor) const;

    // ---- Interactive region-of-interest (UI redesign, A4) -----------
    // Full-resolution film-pixel coordinates, INCLUSIVE.  Cleared
    // automatically before production renders.

    void setInteractiveRegion(unsigned int left, unsigned int top,
                               unsigned int right, unsigned int bottom);
    void clearInteractiveRegion();
    bool getInteractiveRegion(unsigned int* left, unsigned int* top,
                               unsigned int* right, unsigned int* bottom) const;
    /// True iff the active interactive rasterizer actually honors a
    /// set region (some rasterizers ignore it entirely).  Drives the
    /// viewport toolbar's REGION chip disable/tooltip state.
    bool interactiveRasterizerHonorsRegion() const;

    // ---- Editor live-sync (UI refinement item 1) --------------------
    // Mirrors the macOS RISEViewportBridge `serializedSceneText` /
    // `getSceneTextVersionUuid:revision:`.  CAUTION: both take the
    // controller's commit mutex, the SAME mutex a production render (or
    // an outstanding chat-driven agent render) holds for its duration --
    // callers MUST NOT poll these while the scene isn't editable (see
    // MainWindow::canUseSceneTransport / ChatPanel::refreshProposals's
    // gate comment for the "would wedge the GUI thread against the
    // render" hazard this guards against).

    /// Serialized text of the live CST document, or an empty string
    /// when no controller is attached / no document is retained.
    QString serializedSceneText() const;

    /// Retained CST head version (uuid, revision).  uuid is fresh per
    /// load; revision bumps iff content changed.  Both outputs are
    /// zeroed and the return is false when no controller is attached.
    bool getSceneTextVersion(quint64* outUuid, quint64* outRevision) const;

    // ---- Phase 6.5 scene-file save ----------------------------------
    // Round-trip-save bindings.  Mirrors the macOS RISEViewportBridge
    // `hasUnsavedSceneChanges` / `saveSceneTo:errorMessage:` /
    // `setDirtyChangedBlock:` triplet.

    /// True iff there's at least one in-memory edit since the last
    /// load / save that the SaveEngine would write to disk.  Drives
    /// the ViewportProperties panel's "Save Scene" button enable
    /// state.  Cheap O(1).
    bool hasUnsavedSceneChanges() const;

    /// Outcome of `saveSceneTo`.  Mirrors RISE::SaveResult::Status.
    enum class SaveStatus : int {
        Saved   = 0,
        NoOp    = 1,
        Refused = 2,
        Failed  = 3,
        Error   = -1   ///< null controller or null path; caller mistake
    };

    /// Save the in-memory edits to `path`.  Routes through
    /// SceneEditController::RequestSave (cancel-and-park dance
    /// handled controller-side).  On Refused / Failed, `outError`
    /// receives the engine's diagnostic; empty otherwise.
    SaveStatus saveSceneTo(const QString& path, QString& outError);

    /// The path of the currently-loaded .RISEscene file, or an
    /// empty string when no scene has been loaded.  Delegates to
    /// the owning RenderEngine so the ViewportProperties header
    /// can resolve the default Save target without learning about
    /// the engine directly.
    QString loadedFilePath() const;

    /// Canonical scene time owned by the underlying SceneEditController.
    /// Updated by every time-scrub AND by Undo / Redo of a SetSceneTime
    /// edit; that's why MainWindow::onRender queries this just before
    /// kicking the production rasterizer instead of trusting
    /// ViewportTimeline::currentTime, which goes stale across
    /// undo/redo.  Returns 0 when no controller is attached.
    double lastSceneTime() const;

    bool requestProductionRender();

    /// Execute one Agent JSON-RPC request against the live scene.
    /// Mirrors macOS RISEViewportBridge.agentHandleLine.  This ALWAYS
    /// runs at Owner authority + Commit autonomy, regardless of
    /// `agentAutonomyLevel()` below -- it's the "administrative" path
    /// (list_proposals / resolve_proposal / render submit+poll / the
    /// one-time read_skill index fetch all go through it) and MUST
    /// stay that way: resolve_proposal is refused outright under
    /// Propose/Read autonomy (see AgentRpc.h), so if this method
    /// tracked the composer's level, setting the composer to
    /// Read/Propose would silently disable the Owner's own "Approve"/
    /// "Reject" buttons on already-staged proposals, which has nothing
    /// to do with what the CHAT AGENT is permitted to do.  The chat
    /// driver's own model-requested tool calls go through
    /// `agentHandleToolCall()` instead — see that method's doc.
    QString agentHandleLine(const QString& jsonRpcRequest);

    // ---- Agent autonomy selector (2026-07 GUI composer chips) -------
    // Mirrors the macOS RISEViewportBridge's three-dispatcher design
    // (RISEAgentAutonomyLevel / -agentHandleToolCall:).

    /// Mirrors RISE::Agent::AgentAutonomy plus the routing choice THIS
    /// bridge makes for Propose (see `agentHandleToolCall()` below) --
    /// NOT a 1:1 re-export, since the C++ enum alone cannot express
    /// "and which AgentSession authority backs it."
    enum class AgentAutonomyLevel : int {
        Read    = 0,   ///< read-safe verbs only; render allowed; every edit verb refused (kAutonomyRefused).
        Propose = 1,   ///< edit verbs STAGE a proposal (External authority) instead of committing; the owner reviews via the existing proposals panel (list_proposals/resolve_proposal, both still through agentHandleLine()).
        Apply   = 2    ///< today's unrestricted behaviour: edit verbs commit directly (Owner authority, Commit autonomy). The default.
    };

    /// The chat composer's current autonomy level for its OWN tool
    /// calls (the LLM-issued propose_patch/insert_chunk/remove_chunk/
    /// etc. driven by `agentHandleToolCall()`, NOT the administrative
    /// calls `agentHandleLine()` makes on its own — see that method's
    /// note).  Defaults to Apply at construction time, matching every
    /// pre-existing call site's behaviour byte-for-byte until the Qt
    /// composer explicitly sets a persisted choice (see ChatPanel's
    /// QSettings-backed "agentAutonomyLevel" key — this accessor does
    /// NOT read QSettings itself; the Qt layer applies the persisted
    /// value on attach).  Setting an out-of-range value is a no-op
    /// (keeps the previous level) rather than undefined behaviour.
    AgentAutonomyLevel agentAutonomyLevel() const { return m_agentAutonomyLevel; }
    void setAgentAutonomyLevel(AgentAutonomyLevel level);

    /// Hand one JSON-RPC 2.0 request line to whichever internal
    /// dispatcher matches `agentAutonomyLevel()` right now, and return
    /// the response line.  This is the entry point ChatPanel's OWN
    /// tool-call execution uses (processNextToolCall, for every
    /// non-"render" tool call) — the verb-by-verb behaviour per level:
    ///   * Read    -> the read-safe allowlist dispatches;
    ///                propose_patch/insert_chunk/remove_chunk (and any
    ///                other verb) are REFUSED (kAutonomyRefused,
    ///                -32011) — this is Owner authority under Read
    ///                autonomy, so even the refusal path never reaches
    ///                ProposePatch/InsertChunk/RemoveChunk.
    ///   * Propose -> the read-safe allowlist dispatches AS BEFORE, but
    ///                this level runs over a SEPARATE, External-
    ///                authority AgentSession sharing the SAME live
    ///                SceneEditController `agentHandleLine()`'s Owner
    ///                session is attached to — so propose_patch/
    ///                insert_chunk/remove_chunk STAGE a real proposal
    ///                onto that controller's ONE queue (the exact queue
    ///                the existing proposals panel already reads via
    ///                `agentHandleLine()`'s list_proposals/
    ///                resolve_proposal), rather than committing.
    ///                resolve_proposal itself is refused at THIS level
    ///                (by design); the chat driver never calls it, only
    ///                the Owner-authority proposals-panel code path
    ///                does, via `agentHandleLine()`.
    ///   * Apply   -> byte-for-byte today's behaviour: routes to the
    ///                SAME Owner-authority, Commit-autonomy posture
    ///                `agentHandleLine()` uses (a separate dispatcher
    ///                INSTANCE over a separate AgentSession, but
    ///                identical authority+autonomy, so observably
    ///                indistinguishable).
    ///
    /// A "render" tool call is deliberately NOT routed through this
    /// level selector — ChatPanel's async submit/poll/cancel sequence
    /// spans multiple agentHandleLine-shaped calls against ONE
    /// session's per-job result cache, and the user can change
    /// `agentAutonomyLevel()` mid-poll; routing those calls through
    /// whichever dispatcher happens to be current at each poll tick
    /// would risk polling a DIFFERENT session than the one that
    /// submitted the job, missing its cached result.  `render` is
    /// read-safe at every level anyway (it never mutates the CST
    /// document), so there is no honesty cost to always running it
    /// over the stable Owner/Commit session `agentHandleLine()`
    /// already uses — ChatPanel's startAsyncRenderToolCall /
    /// pollOutstandingRender keep calling `agentHandleLine()` directly.
    ///
    /// Same nil-safety contract as `agentHandleLine()`: never returns
    /// an empty string, always well-formed JSON-RPC.
    QString agentHandleToolCall(const QString& jsonRpcRequest);

    // Properties panel ------------------------------------------------

    /// Mirrors RISE::SceneEditController::PanelMode.  Drives which
    /// accordion section is expanded and what the property panel
    /// underneath shows.  Values match SceneEditCategory_*.
    enum class PanelMode : int {
        None       = 0,
        Camera     = 1,
        Rasterizer = 2,
        Object     = 3,
        Light      = 4,
        Film       = 5,   ///< Output Settings (single Film per scene)
        Material   = 6,   ///< Materials
        Medium     = 7    ///< Participating media
    };

    /// Mirrors RISE::SceneEditController::Category — identical numeric
    /// values to PanelMode.  Used by the accordion's list view to pull
    /// per-section entity names and route selection clicks back into
    /// the controller.
    enum class Category : int {
        None       = 0,
        Camera     = 1,
        Rasterizer = 2,
        Object     = 3,
        Light      = 4,
        Film       = 5,   ///< Output Settings (single Film per scene)
        Material   = 6,   ///< Materials
        Medium     = 7,   ///< Participating media
        Animation  = 8,   ///< Named animation paths (pick to activate; no editable properties)
        SceneVariant = 9, ///< scene_variant overlays (pick to re-derive that variant active)
        Painter    = 10,  ///< Painters (union of the IPainter + IScalarPainter managers)
        Geometry   = 11   ///< Geometry (every "*_geometry" chunk -- GUI redesign 2026-07-22)
    };

    PanelMode panelMode() const;
    QString   panelHeader() const;
    QVector<ViewportProperty> propertySnapshot();
    bool setProperty(const QString& name, const QString& value);

    /// Phase 4b: per-category property snapshot + per-category
    /// SetProperty.  Each section in the multi-section panel reads
    /// its own snapshot from `propertySnapshotFor` and routes edits
    /// through `setPropertyForCategory` so a Material-section row
    /// edits the right material even when Object is the primary
    /// selection (auto-synced state).
    QVector<ViewportProperty> propertySnapshotFor(Category cat);
    /// Jump-to-definition (GUI redesign 2026-07-22): for `cat`'s
    /// snapshot row at `index` (a Reference-kind row), resolve which
    /// category the value names.  False for non-Reference rows or
    /// dangling references — the context-menu item stays hidden.
    bool propertyJumpTargetFor(Category cat, int index,
                               Category* outCat, QString* outName);
    bool setPropertyForCategory(Category cat, const QString& name, const QString& value);
    /// Per-category selection accessor.  Returns the entity name
    /// picked in `cat`'s section, or empty when nothing is picked.
    /// An Object pick auto-fills the Material section's selection
    /// (controller-side); both sections then report non-empty.
    QString selectionNameForCategory(Category cat) const;

    /// Is `cat`'s accordion section expanded?  Tracked separately
    /// from the per-category selection so a section-header click
    /// (empty-name SetSelection) still expands the section.
    bool isSectionExpanded(Category cat) const;

    /// Collapse `cat`'s section: clears expanded flag + per-
    /// category selection.  Does NOT affect other sections.
    void collapseSection(Category cat);

    /// Accordion list entries for `category`.  Each entry is the
    /// display name (manager-registered name for cameras / objects /
    /// lights, chunk-name for rasterizers).  Empty list when the
    /// scene has nothing in that category.
    QStringList categoryEntities(Category cat) const;

    /// Scene-level active entity name for `category`, independent of
    /// the UI selection.  Camera → active camera; Rasterizer →
    /// active rasterizer chunk name; Film → "default" (a scene has
    /// exactly one Film by construction); Object/Light/None → empty.
    /// Used to populate the dropdown on first scene load with the
    /// scene's current active entity rather than blank.
    QString activeNameForCategory(Category cat) const;

    /// Current selection (the accordion's expanded section + picked
    /// row).  Empty name means "section open, no row picked".
    Category selectionCategory() const;
    QString  selectionName() const;

    /// Apply a selection.  Empty `name` opens the section without
    /// picking a row.  Camera / Rasterizer selections also activate
    /// the named entity (calls SetActiveCamera / SetActiveRasterizer
    /// respectively); Object / Light / Film selections are UI state only.
    bool setSelection(Category cat, const QString& name);

    /// Monotonic counter — bumped on any structural mutation.  The
    /// properties panel watches it and re-pulls entity lists when it
    /// advances.
    unsigned int sceneEpoch() const;

    /// "Reveal in scene file" (design comp ⌗ affordance): resolve
    /// entity (category, name) to its byte offset + 1-based line
    /// number inside `serializedSceneText()`.  `category` uses the
    /// SAME numbering as `Category` above (identical to the C-API's
    /// SceneEditCategory_*).  Returns false (outputs left untouched)
    /// on a null controller, no retained CST document, an
    /// unresolvable/ambiguous name, or a category with no chunk-name
    /// addressing scheme (Rasterizer/Film/None — see
    /// SceneEditController::EntitySourceLocation's doc comment).  Same
    /// do-not-call-during-renders caveat as `serializedSceneText()` /
    /// `getSceneTextVersion()` — both take the controller's commit
    /// mutex.
    bool getEntitySourceLocation(Category category, const QString& name,
                                  quint64* outByteOffset, quint32* outLine) const;

    // ---- Source traceability (any UI element <-> scene-file span) ----
    // Mirrors the macOS RISEViewportBridge's identically-named section and
    // the RISE_API_SceneEditController_ResolveSourceSpan /
    // SourceRefAtByteOffset C exports.  Same do-not-poll-during-renders
    // caveat as getEntitySourceLocation (both take the controller's commit
    // mutex) -- gate on MainWindow::canUseSceneTransport().

    /// Resolve a UI element's scene-file span (generalizes
    /// getEntitySourceLocation to param granularity + the Film / Rasterizer
    /// singletons).  `param` empty = the whole chunk (outLength 0); non-empty
    /// = the `occ`-th matching param's tight `role value` run.  Fills
    /// byteOffset/byteLength (UTF-8 bytes into serializedSceneText()) + 1-based
    /// line/column.  Returns false (outputs left untouched) on a null
    /// controller, no retained CST document, or an unresolvable ref.
    bool resolveSourceSpan(Category cat, const QString& name, const QString& param,
                            int occ, quint64* outOffset, quint64* outLength,
                            quint32* outLine, quint32* outColumn) const;

    /// Reverse (a text-editor byte offset -> the UI element it backs).  On
    /// success fills `outCat` (a Category), `outName` (the entity name, empty
    /// for an unnamed singleton), `outParam` (empty when the offset is on a
    /// chunk header rather than a specific param), and `outOccurrence`.
    /// Returns false when there's no retained CST or the offset isn't inside
    /// an addressable entity/singleton chunk.  Exposed now for a later
    /// text-cursor -> UI-select slice; the forward (resolveSourceSpan) is the
    /// one wired in this slice.
    bool sourceRefAtByteOffset(quint64 offset, Category* outCat, QString* outName,
                                QString* outParam, int* outOccurrence) const;

    // ---- Entity creation + painter CRUD (entity-creation slice) -----
    // Mirrors RISE_API_SceneEditController_{EntityTemplateCount,
    // EntityTemplateLabel,InstantiateEntityTemplate,DuplicateEntity,
    // RemoveEntity} / the macOS RISEViewportBridge's identically-named
    // section.  The three mutating calls (instantiate/duplicate/remove)
    // take the controller's commit mutex -- same do-not-call-during-
    // renders caveat as `serializedSceneText()` / `saveSceneTo()` (gate
    // on `MainWindow::canUseSceneTransport()` before calling).

    /// Number of "Add Entity" templates registered for `category` (0
    /// for categories with none -- Camera/Rasterizer/Film/Animation/
    /// SceneVariant/None).
    unsigned int entityTemplateCount(Category category) const;

    /// Display label for the template at `idx` within `category`
    /// (e.g. "Sphere", "Omni Light").  Empty string for a null
    /// controller or an out-of-range idx.
    QString entityTemplateLabel(Category category, unsigned int idx) const;

    /// Instantiate the template at `idx` within `category`.  Returns
    /// the AgentCommitResult's `applied` flag; `outName` (optional,
    /// may be null) receives the deduped instance name on success,
    /// `outMessage` (optional, may be null) receives a human-readable
    /// message on failure (left untouched on success).  A multi-chunk
    /// template undoes as several separate steps -- see the C++
    /// method's header doc.
    bool instantiateEntityTemplate(Category category, unsigned int idx,
                                    QString* outName = nullptr,
                                    QString* outMessage = nullptr);

    /// Duplicate the named entity in `category` under a freshly-
    /// deduped name.  Returns `applied`; `outName` / `outMessage` as
    /// above (each optional, may be null).
    bool duplicateEntity(Category category, const QString& name,
                          QString* outName = nullptr,
                          QString* outMessage = nullptr);

    /// Remove the named entity in `category` -- refused with a non-
    /// empty `outMessage` if it is still referenced (e.g. a material a
    /// standard_object still binds) or not found.  Returns `applied`;
    /// `outMessage` as above (optional, may be null).
    bool removeEntity(Category category, const QString& name,
                       QString* outMessage = nullptr);

    /// Clone the currently-active camera under a new name and
    /// promote the clone to active. `proposedName` is canonicalized to a
    /// CST-safe identifier, then deduplicated with a numeric suffix.
    /// Returns the actual name registered, or an
    /// empty QString on no-active-camera / unclonable type.
    ///
    /// Persistence caveat: the clone lives only in the in-memory
    /// Scene/Job.  Reloading the .RISEscene file from the editor
    /// drops it (scene-text round-trip is the pending Phase 6
    /// work).  Caller should surface a one-shot warning the first
    /// time per session.
    QString addCameraFromActive(const QString& proposedName);

    // ---- Environment / IBL section ----------------------------------
    // Mirrors the macOS RISEViewportBridge's identically-named section and
    // the RISE_API_SceneEditController_*Environment* C exports.  The
    // mutating calls take the controller's commit mutex -- gate on
    // MainWindow::canUseSceneTransport() before calling.

    /// Read the current environment binding into `out`.  Returns false only
    /// when there is no scene / no active rasterizer; otherwise fills `out`
    /// (with hasEnvironment == false when unbound).
    bool environmentInfo(EnvironmentInfo* out) const;

    /// Set the environment intensity / background-visibility / rotation
    /// (degrees).  Each applies live (viewport re-renders) AND persists (CST
    /// mirror).  Returns false when no editable bound environment exists.
    bool setEnvironmentScale(double scale);
    bool setEnvironmentBackground(bool background);
    bool setEnvironmentOrient(double xDeg, double yDeg, double zDeg);

    /// Swap the bound environment painter's HDRI file (an existing path; the
    /// file picker is the guard).  Returns false when none is bound.
    bool setEnvironmentFile(const QString& absPath);

    /// Create an environment from an HDRI file when none exists (inserts an
    /// hdr/exr painter chosen from the extension + binds radiance_map).
    /// Returns `applied`; `outName` / `outMessage` optional (may be null).
    bool addEnvironment(const QString& hdriPath,
                         QString* outName = nullptr,
                         QString* outMessage = nullptr);

    /// Remove the environment (unbinds radiance_map, live + CST).  Returns
    /// false when no editable environment exists.
    bool removeEnvironment();

signals:
    /// Emitted on the UI thread with each completed preview frame.
    /// The QImage owns its own data (deep copy from C++ buffer).
    void imageUpdated(const QImage& image);

    /// N-up multi-viewport (docs/gui/RENDER_MODES.md §7): emitted on the UI
    /// thread with each completed frame for a SECONDARY pane (1..
    /// kViewportPaneCount-1).  Pane 0 keeps riding `imageUpdated` above --
    /// the render loop's documented fallback treats the legacy preview
    /// sink as pane 0's sink whenever a pane has none, so this signal never
    /// fires for pane 0.  The QImage owns its own data (deep copy).
    void paneImageUpdated(unsigned int pane, const QImage& image);

    /// Phase 6.5: emitted on each `hasUnsavedSceneChanges()` TRANSITION
    /// (clean→dirty or dirty→clean).  Edits that leave the scene
    /// already-dirty do NOT re-fire it.  Connected by the
    /// ViewportProperties header to gate the Save button's enable
    /// state.  Emitted via QueuedConnection (see ctor) so we don't
    /// re-enter Qt from the C trampoline's thread.
    void dirtyChanged(bool hasUnsavedChanges);

private:
    void buildLivePreview();
    void releaseLivePreview();

    RenderEngine*             m_engine = nullptr;
    RISE::SceneEditController* m_controller = nullptr;
    RISE::IRayCaster*          m_caster = nullptr;        // preview caster, max-recursion 1
    RISE::IRayCaster*          m_polishCaster = nullptr;  // polish caster, max-recursion 2 (one bounce of glossy / refl / refr)
    RISE::IRasterizer*         m_interactiveRasterizer = nullptr;
    ViewportPreviewSink*       m_previewSink = nullptr;
    // N-up multi-viewport (§7): one bridge-owned sink per SECONDARY pane
    // (index 0 unused -- pane 0 keeps using m_previewSink/imageUpdated
    // above), registered via RISE_API_SceneEditController_SetPaneSink in
    // the constructor alongside m_previewSink's _SetPreviewSink call, and
    // released alongside it in releaseLivePreview().
    class ViewportPaneSink*   m_paneSinks[kViewportPaneCount] = {};
    std::unique_ptr<RISE::Agent::AgentRpcDispatcher> m_agentDispatcher;
    // Agent autonomy selector (2026-07): TWO more in-process
    // dispatchers, sibling to `m_agentDispatcher` above, that exist for
    // the whole bridge lifetime so `agentHandleToolCall()` never has to
    // construct one mid-turn.  `m_agentToolDispatcherOwner` borrows an
    // Owner-authority AgentSession (its own instance, separate from
    // `m_agentDispatcher`'s — `m_agentDispatcher` must stay permanently
    // Commit-capable for resolve_proposal, so ONLY this separate
    // instance's autonomy is ever toggled between Read/Commit via
    // AgentRpcDispatcher::SetAutonomy as `agentAutonomyLevel()`
    // changes).  `m_agentToolDispatcherPropose` borrows a SEPARATE
    // External-authority AgentSession, fixed at Propose autonomy for
    // its whole life.  Both AttachController'd to the SAME
    // `m_controller` as `m_agentDispatcher`'s session, so a staged
    // proposal lands on the SAME queue list_proposals/resolve_proposal
    // already read.  Torn down alongside `m_agentDispatcher` in the
    // destructor (same borrows-the-controller lifetime rule).  All
    // three dispatchers are called only from the main/UI thread
    // (matching AgentRpcDispatcher's single-caller contract), so there
    // is no cross-instance locking concern despite three coexisting
    // instances.
    std::unique_ptr<RISE::Agent::AgentRpcDispatcher> m_agentToolDispatcherOwner;
    std::unique_ptr<RISE::Agent::AgentRpcDispatcher> m_agentToolDispatcherPropose;
    AgentAutonomyLevel         m_agentAutonomyLevel = AgentAutonomyLevel::Apply;
    bool                       m_running = false;
};

#endif // VIEWPORTBRIDGE_H
