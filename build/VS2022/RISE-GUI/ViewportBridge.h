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

#include <QByteArray>
#include <QObject>
#include <QImage>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <cstdint>
#include <memory>
#include <string>

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
    // doc 88 S4b (Tier-1 param sliders): the row's authored numeric range,
    // mirroring RISEViewportProperty on the macOS side and CameraProperty in
    // the core.  `hasRange` is true only when BOTH bounds are known.
    // `rangeStep` is 0 for "continuous".  Populated today only for an
    // expression painter's `param[i]` rows, from the `min`/`max`/`step`
    // metadata on the scene text's `param` line.
    //
    // Rendered by ViewportProperties::buildPropertyRow's slider block (the
    // Qt mirror of the Mac ParamSliderCell); still owed a compile-verify on
    // an actual MSVC build (this checkout is macOS-only).
    bool    hasRange = false;
    double  rangeMin = 0.0;
    double  rangeMax = 0.0;
    double  rangeStep = 0.0;
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

/// 87 section 5 step 4a: one node of the AUTHORED-graph tree, as read
/// out of SceneEditController's snapshot in a single pass.
///
/// `parent` and every entry of `children` index into the SAME
/// SceneTree::nodes this node came from -- which is why the whole tree is
/// handed over as one value by `categoryTree()` rather than
/// walked node by node: the controller's own snapshot may be republished
/// between calls, and a handle minted before a republish is REFUSED
/// afterwards (it carries the snapshot generation, so it fails rather
/// than silently naming whichever node now sits at that index).  One
/// locked pass, one consistent tree, and a QAbstractItemModel can hold
/// it as its backing store -- including in QModelIndex::internalId(),
/// which is exactly the place a controller handle must never go.
///
/// `parent` is -1 for a root.  `name` is the entity name -- what the row
/// displays and what `setSelection()` takes.
struct SceneTreeNode {
    QString      name;
    int          parent = -1;
    QVector<int> children;
};

/// 87 section 5 step 4: one category's whole authored tree -- the node
/// table PLUS the root list.
///
/// THE ROOTS ARE CARRIED EXPLICITLY, and step 4b (the macOS twin) flagged
/// their absence here as the one thing 4a got wrong.  Re-deriving them by
/// scanning `nodes` for `parent == -1` yields the right answer TODAY only
/// because `SceneEditController::BuildAuthoredTree` happens to emit its
/// node table in presentation order -- an internal detail of the
/// assembler, not a contract, and sibling ORDER is part of the contract
/// (87 section 2: "child order for display comes from declaration
/// order").  `AuthoredTree::roots` states that order directly, so it is
/// copied across rather than reconstructed.  RISESceneTree on macOS
/// carries the same two arrays for the same reason.
struct SceneTree {
    QVector<SceneTreeNode> nodes;
    /// Indices into `nodes`, in display order.
    QVector<int>           roots;
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
    // being scoped to the data modes. DEFAULT OFF: the viewport shows the
    // first transmissive surface, and SceneEditController resets the flag to OFF
    // on every whole-scene rebind (RebindEditorToJob).

    /// The CURRENT x-ray flag ("false" when no controller is attached --
    /// same null-controller fallback convention as `viewportRenderMode()`
    /// -- or while a render owns the scene, per the C-ABI's documented
    /// never-blocks contract). Otherwise false by default and after every
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
    /// (defaults to time=[0,1], 30 frames if not declared).  Returns false
    /// without touching the outputs for a null controller, render ownership,
    /// or editor-mutex contention; polling UI retains its last tuple.
    bool animationOptions(double& timeStart, double& timeEnd, unsigned int& numFrames) const;

    /// Tri-state live animation-presence snapshot: 1 when the scene
    /// currently has keyframed elements, 0 when it does not, and -1 when
    /// the controller is temporarily unavailable/contended.  Polling UI
    /// retains its last successful value on -1.
    int animationPresence() const;

    /// doc 88 S10 (Tier-2 panel affordances): a small RGBA8 preview patch
    /// for a named painter, or (defIndex >= 0) one of its expression
    /// `def` stages -- see src/Library/SceneEditor/PainterPreview.h for
    /// the domain / display-encode / scalar-normalization contract this
    /// call does not repeat.  CARRY ONLY on this platform today (the Qt
    /// paint-into-a-swatch-widget half is owed, needs an MSVC build to
    /// verify -- MATERIAL_EDITOR.md §2.1's standing Mac-renders/
    /// Windows-carries split): this fills `outRGBA` with exactly
    /// `w*h*4` bytes (row-major top-to-bottom RGBA8, trivially wrapped
    /// as `QImage(outRGBA.constData(), w, h, QImage::Format_RGBA8888)`
    /// by a future widget) and reports the auto-range normalization via
    /// `outWasScalar`/`outRangeMin`/`outRangeMax` when the previewed
    /// stage is scalar-typed.  Returns false (buffer cleared) on an
    /// unresolved name, an out-of-range/inapplicable defIndex, bad
    /// dimensions, or the render-owns-scene / contended-lock refusal
    /// every other getter in this section documents.
    bool painterPreview( const QString& painterName, int defIndex,
                          unsigned int w, unsigned int h, QByteArray& outRGBA,
                          bool* outWasScalar = nullptr,
                          double* outRangeMin = nullptr,
                          double* outRangeMax = nullptr ) const;

    /// doc 88 S10: a ramp_painter's own colour interpolation over its
    /// authored stop domain, as a horizontal gradient strip.  Same
    /// carry-only contract as painterPreview() above; additionally
    /// refuses when `painterName` does not resolve to a ramp_painter.
    bool rampStripPreview( const QString& painterName,
                            unsigned int w, unsigned int h,
                            QByteArray& outRGBA ) const;

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
    // Full-resolution film-pixel coordinates, INCLUSIVE. Preserved across
    // production renders; full-frame and region-only production are explicit
    // sibling commands and neither silently changes this interactive choice.

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
    /// (list_proposals / resolve_proposal / the one-time read_skill
    /// index fetch all go through it; the chat panel's render
    /// submit/poll/cancel does NOT -- see `agentHandleToolCall()`) and MUST
    /// stay that way: resolve_proposal is refused outright under
    /// Propose/Read autonomy (see AgentRpc.h), so if this method
    /// tracked the composer's level, setting the composer to
    /// Read/Propose would silently disable the Owner's own "Approve"/
    /// "Reject" buttons on already-staged proposals, which has nothing
    /// to do with what the CHAT AGENT is permitted to do.  The chat
    /// driver's own model-requested tool calls go through
    /// `agentHandleToolCall()` instead — see that method's doc.
    ///
    /// CONSEQUENCE FOR `read_image` TYPED BY HAND HERE (2026-07): the chat
    /// panel's `render` used to run on THIS session, so a hand-typed
    /// `read_image` in the raw JSON-RPC debug surface would return the chat
    /// agent's last frame.  It no longer does -- chat renders now run on
    /// the autonomy-selected tool-call session (see
    /// `agentHandleToolCall()`), and AgentSession's last-render PNG cache
    /// is PER-SESSION.  So after a chat render, a `read_image` sent through
    /// THIS method reports `byteLength` 0 until something is rendered
    /// through this session too.  Expected, not a bug: send a `render` here
    /// first if you want pixels back here.
    QString agentHandleLine(const QString& jsonRpcRequest);

    /// Durable document snapshots (see
    /// RISE::Agent::AgentChatLoop::SetDocumentSnapshotProvider): the
    /// retained CST head's REVISION alone, via the administrative
    /// dispatcher's session (m_agentDispatcher -- Owner authority,
    /// WrapJob's the SAME Job and AttachController's the SAME
    /// m_controller as every other in-app session, so any of the three
    /// would answer identically).  -1 when the dispatcher/session is
    /// unavailable, mirroring TrajectorySessionRecord::sceneHeadVersion's
    /// documented "-1 = unknown" sentinel.  CHEAP -- no document copy --
    /// safe to call at trajectory-start time, though it takes the
    /// controller mutex like every coherent read (same lock used by
    /// commits / interactive render coordination), so it can briefly
    /// block if the render thread currently holds it.  Mirrors macOS
    /// RISEViewportBridge.agentHeadVersionRevision.
    qint64 agentHeadVersionRevision() const;

    /// Durable document snapshots: ONE coherent read of the full CST
    /// document text plus the head-version identity those exact bytes
    /// ARE, for ChatPanel's document-snapshot provider to wrap.
    /// Delegates to AgentSession::ReadDocumentSnapshot on the SAME
    /// administrative session `agentHeadVersionRevision()` reads.
    /// CONTROLLER-MEDIATED: this call can BLOCK while a render owns the
    /// scene -- call only from the agent RPC thread, matching
    /// AgentChatLoop::SetDocumentSnapshotProvider's documented
    /// discipline.  Returns false (outText/outUuid/outRevision
    /// untouched) when there is no retained document (never CST-loaded,
    /// or after ClearAll) or the dispatcher/session is unavailable.
    /// Mirrors macOS RISEViewportBridge.agentReadDocumentSnapshotTextWithUuid:revision:.
    bool agentReadDocumentSnapshot(std::string& outText, uint64_t& outUuid, uint64_t& outRevision) const;

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
    /// calls (the LLM-issued mutating verbs and reads driven by
    /// `agentHandleToolCall()`, NOT the administrative
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
    ///   * Read    -> the read-safe allowlist dispatches (IsReadSafeVerb
    ///                in AgentRpc.cpp IS the membership list -- no copy
    ///                or count of it here);
    ///                the 6 mutating verbs (and any
    ///                other verb) are REFUSED (kAutonomyRefused,
    ///                -32011) — this is Owner authority under Read
    ///                autonomy, so even the refusal path never reaches
    ///                ProposePatch/InsertChunk/RemoveChunk.
    ///   * Propose -> the read-safe allowlist dispatches AS BEFORE, but
    ///                this level runs over a SEPARATE, External-
    ///                authority AgentSession sharing the SAME live
    ///                SceneEditController `agentHandleLine()`'s
    ///                administrative session is attached to — so
    ///                the 6 mutating verbs
    ///                (IsProposeSafeVerb) STAGE a real proposal
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
    /// A "render" tool call GOES THROUGH THIS SELECTOR TOO (2026-07
    /// per-session image-cache fix; mirrors the macOS bridge).  It used
    /// to be routed to `agentHandleLine()`'s administrative session
    /// instead, on the reasoning that ChatPanel's async submit/poll/
    /// cancel sequence spans several calls against ONE session's per-job
    /// state and the user can change `agentAutonomyLevel()` mid-poll.
    /// That reasoning was right about the job-id problem and wrong about
    /// the fix: it split "render" away from "read_image", and
    /// AgentSession's LAST-RENDER PNG cache (mLastPng / mLastSink,
    /// populated in AgentSession.cpp's RenderCore_) is PER-SESSION.  With
    /// the two verbs on different sessions the agent's read_image read a
    /// cache its own render never wrote -- returning zero bytes, or the
    /// stale objectmap PNG left behind by query_object_at's internal
    /// render.  Both verbs now run on the SAME session.  (The stale-
    /// objectmap half has since been fixed at its own source as well --
    /// query_object_at's internal render is stash/restore-guarded and no
    /// longer clobbers the cache; see AgentSession.cpp's
    /// EphemeralRenderCacheGuard.)
    ///
    /// The mid-render level flip is handled by PINNING instead: ChatPanel
    /// captures the level ONCE at submit time and passes it to the
    /// two-argument overload below for every subsequent poll / cancel /
    /// final-result call for that job, so a whole render job's calls stay
    /// on ONE session.  What the pin actually fixes -- and what it
    /// deliberately does NOT freeze -- is spelled out on that overload;
    /// read it before building on this.  Routing "render" here does not
    /// change what is PERMITTED: render / render_status / render_wait /
    /// render_cancel / read_image are all on IsReadSafeVerb's allowlist
    /// (AgentRpc.cpp), so each dispatches under Read, Propose, and Apply
    /// alike.
    ///
    /// Same nil-safety contract as `agentHandleLine()`: never returns
    /// an empty string, always well-formed JSON-RPC.
    QString agentHandleToolCall(const QString& jsonRpcRequest);

    /// Level-EXPLICIT overload: dispatch to the session `level` selects,
    /// IGNORING the live `agentAutonomyLevel()`.  The one-argument form
    /// above is exactly this called with the current level.
    ///
    /// This exists for ONE reason: a chat-driven async render is a
    /// MULTI-CALL job (submit -> render_wait poll xN -> possibly
    /// render_cancel), and PARTS OF THAT JOB'S STATE LIVE ON THE SESSION
    /// THAT RAN IT.  So ChatPanel captures the level once at submit time
    /// and pins it here for that job's whole lifecycle.
    ///
    /// WHICH PARTS -- be precise here, because an earlier version of this
    /// doc was NOT.  The renderJobId itself is *not* session-scoped: ids
    /// are minted by the CONTROLLER
    /// (SceneEditController::SubmitAgentRenderAsync), and
    /// AgentSession::RenderStatus / RenderWait / CancelAsyncRender each
    /// delegate straight through to that controller -- so ANY session
    /// attached to the same controller resolves ANY of its job ids.  Do
    /// not build on "another session cannot see this job"; it can.  What
    /// IS session-scoped, and is the real reason for the pin:
    ///
    ///   1. render_wait's OPTIONAL `result` payload.  AgentRpc.cpp's
    ///      render_wait handler attaches `result` only when
    ///      AgentSession::LastAsyncRenderResult finds a cache entry whose
    ///      mLastAsyncRenderResultJobId matches -- and only the session
    ///      that RAN the render ever writes that cell.  Poll a sibling
    ///      session mid-render and the reply is completed:true with NO
    ///      `result`, at which point the driver can only degrade to
    ///      "render completed but no cached result was found" (see
    ///      ChatPanel::pollOutstandingRender).  The render really did
    ///      succeed; the agent just never sees its stats.
    ///   2. (RETIRED 2026-07.)  The last-render PNG cache that ReadImage()
    ///      and the `read_image` verb serve used to be per-session too --
    ///      the original reason "render" was moved onto this selector.  It
    ///      is now an AgentImageCache SHARED by all three in-app sessions
    ///      (see ViewportBridge's constructor), so it no longer scopes
    ///      anything and no longer needs the pin.  Reason 1 above is the
    ///      only remaining one; the pin stays for it.
    ///
    /// WHAT IS PINNED IS THE **SESSION SELECTION**, NOT THE AUTONOMY
    /// POSTURE.  `level` chooses WHICH dispatcher/session handles the
    /// call; it does not freeze what that session is allowed to do.
    /// setAgentAutonomyLevel() mutates the TOOL-CALL OWNER session's
    /// autonomy IN PLACE (m_agentToolDispatcherOwner),
    /// so a poll issued with a pinned level of Apply *after* the
    /// user has dropped the chip to Read genuinely executes under Read.
    /// That is the CORRECT safety behaviour and is deliberately kept --
    /// the pin does not defeat a mid-render drop to Read.  (Nothing in a
    /// render job's poll/cancel sequence is an edit verb, so the live
    /// posture never changes the outcome here anyway; the property
    /// matters because it is what makes the pin safe to have at all.)
    ///
    /// The pin is likewise SCOPED TO ONE RENDER JOB, never to a chat
    /// turn.  Autonomy is a SAFETY control: a user who drops to Read
    /// mid-turn to stop the agent editing must have that take effect on
    /// the agent's very NEXT tool call.  Pinning a whole turn would defer
    /// a safety decision to the turn boundary; pinning a render job only
    /// works around the two mechanical session-scoping constraints listed
    /// above.
    ///
    /// THE AUTONOMY-FLIP RESIDUAL THIS DOC USED TO CARRY IS CLOSED.  It
    /// read: a chip flip TO or FROM Propose between a completed "render"
    /// and the "read_image" that follows lands that read on the other
    /// session, which returns ITS empty or stale PNG cache.  The three
    /// in-app sessions now share one AgentImageCache, so the read finds
    /// the render whichever session ran it.  (A Read<->Apply flip never
    /// changed session in the first place -- both select the SAME
    /// m_agentToolDispatcherOwner, differing only in the autonomy set on
    /// it.)  Covered at the library level by AgentRenderAsyncTest's
    /// "(shared-img-cache)" case.
    ///
    /// WHAT A FLIP STILL COSTS: a poll or wait issued on the other session
    /// gets completed:true with no `result` payload, per reason 1 above --
    /// which is why the render JOB is still pinned.  Sharing the cache
    /// bought back the pixels, not the per-session job bookkeeping, and
    /// that bookkeeping is deliberately NOT shared.
    QString agentHandleToolCall(const QString& jsonRpcRequest, AgentAutonomyLevel level);

    // Agent image generation (Arc 77 Phase 2: imagine_scene GUI wiring) --

    /// Windows mirror of macOS RISEViewportBridge's
    /// `-agentSetImageGeneratorProvider:apiKey:`. Installs (or replaces)
    /// the HOST half of `imagine_scene` on every in-app tool-call session
    /// this bridge owns (m_agentDispatcher, m_agentToolDispatcherOwner,
    /// m_agentToolDispatcherPropose -- see
    /// RISE::Agent::AgentSession::SetImageGenerator's doc for why this is
    /// host-installed-only). `providerName` is the SAME lowercase
    /// spelling ChatPanel's Provider enum already maps to for
    /// OpenAIChatCodec::Config ("anthropic"/"gemini"/"openai"/"xai"/
    /// "local") -- capability is per-provider (gemini/openai only).
    /// `apiKey` rides in the SAME per-provider auth header the chat
    /// codec for that provider uses; pass an empty string for a
    /// keyless/no-key-yet posture. Builds the actual generator via
    /// RISE::Agent::MakeChatImageGenerator (AgentChatCodecs.h) over the
    /// platform system transport (RISE::Agent::CreateSystemChatHttpTransport)
    /// -- `imagine_scene` dispatches synchronously on the calling thread,
    /// so it cannot await Qt's async QNetworkAccessManager reply the chat
    /// panel's own HTTP path uses. CALL AGAIN on every provider or key
    /// change; ChatPanel does so once per round, at the same point it
    /// already reads the applied provider's key for BuildRequest (see
    /// ChatPanel::runNextStep).
    void agentSetImageGenerator(const QString& providerName, const QString& apiKey);

    // Agent session mode (Arc 83 sec 4.1: the build/refine transition) ---

    /// Windows mirror of macOS RISEViewportBridge's
    /// `-agentNoteFinalAnswer`.  Tells EVERY in-app session this bridge
    /// owns (m_agentDispatcher, m_agentToolDispatcherOwner,
    /// m_agentToolDispatcherPropose) that the agent's turn just ended with
    /// its own prose rather than a tool call -- the ONE structural signal
    /// that ends the staged build protocol's COMPULSION (see
    /// RISE::Agent::AgentSession::NoteFinalAnswer and the block above
    /// AgentSession::SessionMode).  After it no construction gate fires
    /// again on these sessions: no phase refusals, no compose-phase delete
    /// ban, no forced clean rooms, no populate-before-render, no build-plan
    /// gate.  Every verb stays available; nothing is compelled.
    ///
    /// Called from ChatPanel's FinalText arm, unconditionally and on every
    /// final turn: NoteFinalAnswer is idempotent and one-way, so only the
    /// first one on a session that started from an EMPTY scene changes
    /// anything.  Deliberately does NOT reach the hosted loopback server's
    /// External session -- there the external MCP client owns the model
    /// loop and no turn-end signal crosses the wire.
    void agentNoteFinalAnswer();

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

    /// The AUTHORED-graph tree for `category` (87 section 5 step 4a) --
    /// the surface the outliner's QAbstractItemModel is built over,
    /// replacing the flat `categoryEntities()` list above.
    ///
    /// `nodes` is the controller's node TABLE and `roots` is the
    /// controller's own root list, both in presentation order.  START AT
    /// `roots` and descend through each node's `children` list -- do NOT
    /// re-derive the roots by scanning for `parent == -1` (see
    /// SceneTree's doc for why that only happens to work).
    ///
    /// A category with no hierarchy (Camera, Material, Painter, ...)
    /// comes back as N nodes all with `parent == -1` and no children,
    /// all of them in `roots`, so the model needs no per-category branch.
    ///
    /// For Category::Object the tree is the AUTHORED graph, not the flat
    /// render list: 87 step 3's synthesized instancing entries (`I.X`,
    /// `I[i,j]`) are folded into the chunk that produced them, so an 8x8
    /// instanced grid is one row rather than 64+.
    ///
    /// TRANSACTIONAL: built from SceneEditController::ReadTree, which
    /// copies the whole published tree under one lock hold, so the
    /// result is never a mixture of two trees.  Do NOT reimplement this
    /// as a walk over the per-node getters -- each of those takes the
    /// snapshot lock separately, and another thread's count call can
    /// republish between two of them.
    ///
    /// Re-read this whenever the scene epoch advances.  NOTE for the
    /// drag-to-reparent work: a re-parent changes the TREE without
    /// changing any category's entity LIST, so that path must bump the
    /// scene epoch itself or the outliner keeps drawing the old shape.
    ///
    /// Empty (both arrays) on a null controller or an empty category.
    SceneTree categoryTree(Category cat) const;

    // ---- doc-88 Phase 3 S14/S16/S22 Painter/Material graph value types ---
    // NESTED as public members of ViewportBridge (review-round P1 fix):
    // these FOUR structs were previously declared as siblings of this
    // class, at global namespace scope, while NodeGraphCanvas.h/.cpp (and
    // this file's own out-of-line method definitions) referenced them as
    // `ViewportBridge::PainterGraph` / `ViewportBridge::PainterGraphNode` /
    // `ViewportBridge::PainterGraphPort` / `ViewportBridge::
    // AppearanceClosureEntry` -- qualified-name lookup only searches the
    // NAMED class, so a global-scope sibling is simply not found through
    // that spelling ("no type named 'PainterGraph' in 'ViewportBridge'"
    // under Clang; MSVC C2039 identically). This predates the
    // AppearanceClosureEntry addition -- PainterGraph/PainterGraphNode/
    // PainterGraphPort shipped broken in the S14/S16/S22 slices, never
    // caught because this file has never been built by a real MSVC
    // toolchain (see the MSVC-verification checklist in
    // NodeGraphCanvas.cpp). Nesting here, rather than qualifying every
    // call site with `::` to force global lookup, is the minimal-churn fix:
    // every existing `ViewportBridge::X` spelling becomes valid AS WRITTEN.
    //
    // Plain data structs (QString/QVector/int/double/quint64 members only)
    // passed by value or by const-ref/QVector -- none crosses a
    // Q_DECLARE_METATYPE, a signal parameter, or any other context that
    // would need them registered with Qt's meta-object system, so nesting
    // introduces no such hazard.
    // =====================================================================

    /// doc-88 Phase 3 S14 (docs/gui/NODE_GRAPH_CANVAS.md sect. 6): one
    /// reference-param slot on a `PainterGraphNode`.  Mirrors
    /// `SceneEditController::GraphPort`.
    ///
    /// `otherNodeIndex` is -1 for a NODE-LESS port (a dangling reference, or
    /// one pointing outside this graph's modeled categories) -- `otherName`
    /// still carries the target's name either way, never blank for a real
    /// reference.  Otherwise it is an INDEX into the SAME
    /// `PainterGraph::nodes` array this port's owning node came from -- like
    /// `SceneTreeNode`'s own `parent`/`children` indices, NOT a controller
    /// handle: valid only within the ONE `painterMaterialGraph()` call that
    /// produced it (see `SceneTreeNode`'s own header comment for why -- the
    /// same rule applies here for the same reason).
    struct PainterGraphPort {
        QString paramName;
        int     occurrence     = 0;
        int     otherNodeIndex = -1;
        QString otherName;
    };

    /// One node of the Painter/Material graph, WITH its laid-out position --
    /// mirrors `SceneEditController::GraphNode` plus the parallel
    /// `SceneEditController::GraphNodePosition`
    /// `ReadPainterMaterialGraphLaidOut` hands back for it (doc-88 Phase 3
    /// S11/S12/S13/S14).
    ///
    /// `handle` is a real `SceneEditController::GraphNodeHandle` (unlike this
    /// struct's own port INDICES) -- it survives being held across a
    /// re-fetch of the SAME published generation (`PainterGraph::
    /// generation`), the same "the way back" role `ResolveGraphNodeHandle`
    /// plays on the C++ side.  A caller that wants to re-find "the node the
    /// user last selected" after a re-fetch should hold THIS, never an
    /// `outEdges[i].otherNodeIndex`.
    struct PainterGraphNode {
        quint64 handle   = 0;
        QString name;
        QString chunkKeyword;   ///< e.g. "ramp_painter" -- always non-blank for a real node
        /// `RISE::ChunkCategory` cast to int -- the SAME "cast the parser's
        /// enum, no bespoke per-value mirror" convention `ViewportProperty::
        /// kind` already uses for `ValueKind` (see `SceneEditController::
        /// PainterGraphNodeCategory`'s own comment). `chunkKeyword` above is
        /// almost always the more useful discriminator for a UI label/icon.
        int     category = -1;
        int     defCount = 0;   ///< expression-family def-stage count; 0 otherwise
        /// Object Graph slice (S3): a `standard_object`'s `count_u *
        /// count_v` repeat sugar when this chunk carries counts, 0
        /// otherwise -- see `SceneEditController::GraphNode::repeatCount`'s
        /// own header comment for the full contract, including the
        /// accepted `count_u 0` fold. Always 0 for a Painter/Material/
        /// Function node (`painterMaterialGraph()`).
        int     repeatCount = 0;
        double  x = 0.0;
        double  y = 0.0;
        QVector<PainterGraphPort> outEdges;
        QVector<PainterGraphPort> inEdges;   ///< non-empty here == this node is SHARED (fan-out badge)
    };

    /// The whole Painter/Material graph, positioned -- one
    /// `painterMaterialGraph()` call, the node table plus its generation.  No
    /// separate edge array: an edge is reachable through either endpoint's
    /// own `outEdges`/`inEdges` (`PainterGraphPort` already carries both
    /// endpoints + the param label, everything needed to draw a wire).
    struct PainterGraph {
        QVector<PainterGraphNode> nodes;
        quint64                   generation = 0;
    };

    /// One entry of an `appearanceClosureForObject()` result -- a node
    /// IDENTITY, not just a display string.  Mirrors
    /// `SceneEditController::AppearanceClosureEntry` field-for-field: a bare
    /// name is NOT enough to address a node in `PainterGraph::nodes` (a
    /// Painter and a Material chunk may legally share a name), so a caller
    /// matching a returned entry against an already-fetched node list MUST
    /// compare BOTH `category` and `name`, never `name` alone (review-round P1
    /// fix -- an earlier draft of this bridge method returned a bare
    /// `QStringList` and the Qt canvas matched by name only, which could
    /// silently spotlight the wrong node on a cross-category name collision).
    struct AppearanceClosureEntry {
        /// `RISE::ChunkCategory` cast to int -- the SAME ordinal
        /// `PainterGraphNode::category` already uses (Painter 0, Function 1,
        /// Material 2), so this compares directly against a node's own
        /// `category` field with no remapping.
        int     category = -1;
        QString name;
    };

    /// doc-88 Phase 3 S14: the Painter/Material node graph, positioned --
    /// {nodes, edges, positions}.  Same ONE-TRANSACTIONAL-READ discipline
    /// categoryTree() documents just above: built from
    /// SceneEditController::ReadPainterMaterialGraphLaidOut, which
    /// composes S11's graph + S13's saved sidecar positions + S12's
    /// auto-layout fill-in under one snapshot-lock hold (plus one small
    /// sidecar file read -- see that method's own header comment) rather
    /// than a per-node ABI walk.  Empty (no nodes) on a null controller.
    PainterGraph painterMaterialGraph() const;

    /// Node-graph "spotlight" query: for `objectName` (an Object-category
    /// chunk name -- pass `selectionRowName()`, NOT `selectionName()`, for
    /// a viewport/outliner pick, since a synthesized per-repetition/
    /// subtree-member name like `I[1,0]`/`I.child` is never itself an
    /// addressable chunk -- see `selectionRowName()`'s own comment),
    /// returns the chunk (category, name) IDENTITIES to highlight on the
    /// node-graph canvas: the object's bound material first, then the full
    /// transitive Painter/Function/Material closure reachable from it in
    /// the SAME published PainterGraph `painterMaterialGraph()` reads from,
    /// in BFS discovery order -- see `AppearanceClosureEntry`'s own comment
    /// on why a bare name is not enough to match against
    /// `PainterGraph::nodes`.  Empty when the object is unknown, has no
    /// material bound, or the resolved material name is AMBIGUOUS in the
    /// current graph (more than one same-category chunk shares it --
    /// refused rather than guessed, see
    /// SceneEditController::AppearanceClosureForObject's own comment).
    ///
    /// `degraded` (later external review round -- an empty result used to
    /// be overloaded, and the Qt consumer polling this could not tell a
    /// genuine empty answer apart from lock contention, which caused a
    /// material-less object to pay a full deep resolve on every single
    /// preview frame forever, since the caller had no way to know it was
    /// safe to cache "empty" the way it caches a real answer): when
    /// non-null, `*degraded` is set to `true` ONLY when the controller
    /// could not get a non-blocking hold of the commit lock right now (a
    /// render owns the scene) and so could not even ATTEMPT a real answer
    /// -- this is a POLLED query (the Qt canvas calls it from every
    /// performReload() pass), so `degraded == true` means "retry next
    /// pass," not "the object has nothing to show." Set to `false` on
    /// every other outcome, INCLUDING a successful walk that finds nothing
    /// reachable -- that is a genuine, resolved answer a caller should
    /// accept and cache, not retry. Defaults to `nullptr`. Called directly
    /// on the C++ controller (SceneEditController::AppearanceClosureForObject),
    /// the same "this file already calls SceneEditController natively"
    /// reasoning painterMaterialGraph() documents above.
    QVector<AppearanceClosureEntry> appearanceClosureForObject(const QString& objectName, bool* degraded = nullptr) const;

    /// User-requested slice: the node-graph canvas's "All" vs "Focused"
    /// view-scope toggle. Focused variant of painterMaterialGraph() --
    /// the returned PainterGraph's `nodes` contains ONLY the subgraph
    /// rooted at `(category, name)`, laid out fresh -- see
    /// SceneEditController::ReadPainterMaterialGraphLaidOutFocused's own
    /// header comment for the exact subgraph definition:
    ///   - `category == 8` (RISE::ChunkCategory::Object -- the SAME "cast
    ///     the parser's enum" ordinal PainterGraphNode::category already
    ///     uses for Painter(0)/Function(1)/Material(2), extended here to
    ///     also accept Object): the object's APPEARANCE CLOSURE, identical
    ///     to appearanceClosureForObject()'s own resolution + walk.
    ///   - `category` Painter(0)/Function(1)/Material(2) (a canvas node
    ///     itself): that node plus its own transitive closure in the SAME
    ///     direction (its "inputs") -- DOWNSTREAM REFERRERS of the
    ///     selected node are EXCLUDED, a narrower view than the Object
    ///     case would ever produce for the same node, by design.
    /// Empty `nodes` for an unknown `name`, or (Object case) an
    /// unresolved/ambiguous bound material, or (non-Object case) an
    /// ambiguous `(category, name)` -- same refusal convention as
    /// ResolveUniqueGraphNodeIndex.
    ///
    /// `degraded` -- the SAME contract appearanceClosureForObject()
    /// documents just above: set true ONLY when the Object-category
    /// case's live object->material resolution could not get a
    /// non-blocking hold of the commit lock (a render owns the scene);
    /// false on every other outcome, including a genuinely empty result.
    /// The non-Object case never degrades this way (a canvas-node lookup
    /// never touches the live object/material manager). Defaults to
    /// nullptr.
    ///
    /// LAYOUT IS TRANSIENT: this NEVER reads or writes the
    /// .risegraph.json sidecar -- see the C++ method's own comment.
    /// Toggling back to the all-view (painterMaterialGraph()) is
    /// completely unaffected by any number of prior focused reads.
    PainterGraph painterMaterialGraphFocused(int category, const QString& name, bool* degraded = nullptr) const;

    /// S3 Qt carry (Object Graph canvas, sibling of the Painter/Material
    /// canvas above): the object hierarchy graph, positioned -- {nodes,
    /// edges, positions}. Reuses `PainterGraph`/`PainterGraphNode`
    /// verbatim (same shape: a node table + generation, nodes carrying
    /// category/keyword/ports/position/repeatCount) rather than a second
    /// wrapper type -- the SAME "reuse the shared model, don't duplicate a
    /// structurally identical twin" call `SceneEditController::
    /// SceneGraphModel`'s own comment makes on the C++ side, ported
    /// verbatim from the Mac bridge's identical choice
    /// (`-[RISEViewportBridge objectGraph]` reuses `RISEPainterMaterialGraph`
    /// the same way). ONE TRANSACTIONAL READ, built from
    /// `SceneEditController::ReadObjectGraphLaidOut` -- see that method's
    /// own header comment for the node set (`ChunkCategory::Object` +
    /// `ChunkCategory::Geometry`, plus `rect_light`/`shape_light` by
    /// keyword), the edge taxonomy, and why this NEVER reads or writes the
    /// `.risegraph.json` sidecar (layout is always transient here, even
    /// for the all-view -- unlike `painterMaterialGraph()`). Empty (no
    /// nodes) on a null controller.
    PainterGraph objectGraph() const;

    /// Object Graph twin of `painterMaterialGraphFocused()` -- "all
    /// parents and children of the clicked object" (the user's own
    /// framing). See `SceneEditController::ReadObjectGraphLaidOutFocused`'s
    /// own header comment for the exact four-component subgraph
    /// definition (UP/DOWN/GEO/SOURCE). Unlike the Painter/Material
    /// focused read, there is no `category` parameter -- the object graph
    /// only ever focuses on an Object-category name (which already
    /// includes `rect_light`/`shape_light` nodes, per `objectGraph()`'s
    /// own comment).
    ///
    /// `degraded` -- the SAME contract `painterMaterialGraphFocused()`
    /// documents: set true ONLY when `mRenderOwnsScene` was observed true
    /// at the top of the C++ call (a render owns the scene right now);
    /// false on every other outcome, including a genuinely empty result.
    /// Defaults to nullptr.
    ///
    /// LAYOUT IS TRANSIENT -- same as `objectGraph()` itself (this graph
    /// has no sidecar to read OR write at all, focused or not).
    PainterGraph objectGraphFocused(const QString& name, bool* degraded = nullptr) const;

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

    /// The OUTLINER ROW the current selection should highlight -- what the
    /// outliner model compares against its row names.  Identical to
    /// `selectionName` for every ordinary entity; differs only when the
    /// selection names a SYNTHESIZED instancing entry (`I[1,0]`, `I.X`),
    /// which is what a viewport pick necessarily produces, where the row is
    /// the instancing CHUNK those entries were expanded from.
    ///
    /// ROW HIGHLIGHTING ONLY.  `selectionName` stays the selected ENTITY --
    /// what the property panel inspects, what the gizmo moves, what the
    /// viewport chrome names.  Falls back to `selectionName` when nothing
    /// resolves.
    QString  selectionRowName() const;

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

    // ---- Node-graph canvas: create node (S18) -----------------------
    // Mirrors RISE_API_SceneEditController_{ChunkNodeRequiredArgCount,
    // ChunkNodeRequiredArg,CreateChunkNode} / the macOS
    // RISEViewportBridge's identically-named section.  Unlike the fixed
    // template picker above, this creates a node for ANY painter/
    // material KEYWORD -- the open set the S21/S22 canvas search
    // palette offers.  `createChunkNode` takes the controller's commit
    // mutex (same do-not-call-during-renders caveat as the entity CRUD
    // above); `chunkNodeRequirements` is a pure descriptor read.
    //
    // STANDING CAVEAT (same posture as S4b's Qt range-slider and S10's
    // Qt swatch widget, docs/gui/MATERIAL_EDITOR.md:69,205): this half
    // is CARRIED, not MSVC-verified -- it is written to match the
    // macOS bridge line for line and is owed a Windows build.

    /// One argument the caller must supply to create a node of a given
    /// keyword.  `isReference` = the value must name another chunk in
    /// this scene (resolve it from the drag context / a candidate
    /// picker filtered by the S17 connection-legality check), rather
    /// than being free text or a file path.
    struct ChunkNodeRequirement
    {
        QString param;
        QString description;
        bool    isReference = false;
    };

    /// The creation-argument contract for `keyword`.  Empty for a
    /// keyword that needs nothing, for a non-painter/material keyword,
    /// and for an unknown keyword.
    QVector<ChunkNodeRequirement> chunkNodeRequirements(const QString& keyword) const;

    /// Create one painter/material node of type `keyword`, named from
    /// `baseName` (deduped on collision -- READ `outName`, do not
    /// assume the base was granted; an empty base falls back to the
    /// keyword).  `argParams` / `argValues` are ORDERED PARALLEL lists
    /// (must be the same length; a mismatched pair is treated as no
    /// args) mirroring the C ABI beneath -- NOT a QMap: a keyed map can
    /// neither repeat a param (e.g. voronoi's repeatable `gen`) nor
    /// guarantee the composed chunk body's line order matches what the
    /// caller wrote (QMap iterates in KEY-SORTED order, not insertion
    /// order).  Every `argParams[i]` must cover a requirement above.
    /// Returns `applied`; `outName` / `outMessage` as the entity CRUD
    /// calls above (each optional, may be null).
    ///
    /// On a REJECTED refusal the scene is left byte-identical.  A
    /// DIAGNOSED refusal is different: the node WAS created and the
    /// live managers WERE rebuilt, but the full re-derive also emitted
    /// diagnostics -- `applied` is still false, but the mutation is
    /// real (and undoable) and `outName` is filled with the name that
    /// landed.
    bool createChunkNode(const QString& keyword, const QString& baseName,
                          const QStringList& argParams,
                          const QStringList& argValues,
                          QString* outName = nullptr,
                          QString* outMessage = nullptr);

    // ---- Node-graph canvas: rewire a connection (S19) ---------------
    // Mirrors RISE_API_SceneEditController_RewireConnection / the macOS
    // RISEViewportBridge's identically-named section -- the
    // ownership-closure REWIRE verb (docs/gui/NODE_GRAPH_CANVAS.md
    // sect. 6 S19, implementing docs/gui/MATERIAL_EDITOR.md sect. 3.7a).
    // Takes the controller's commit mutex -- gate on
    // MainWindow::canUseSceneTransport() before calling.
    //
    // STANDING CAVEAT (same posture as the S18 block above): this half
    // is CARRIED, not MSVC-verified -- written to match the macOS bridge
    // line for line and owed a Windows build.

    /// Why a rewire was refused.  Mirrors `RISE::ClosureClassification`'s
    /// ordinals 1:1 (the C ABI passes it as a plain `int`).  This IS a real
    /// mirror that can drift (P2-3, S19 review round 1 -- an earlier
    /// RISE_API.h comment wrongly claimed neither bridge re-declares this
    /// enum) -- see the ordinal-pinning `static_assert`s at the top of
    /// tests/RewireConnectionTest.cpp, which check the C++ side; keep this
    /// enum's five values in step with them by hand.
    enum class RewireClosure
    {
        Clean = 0,
        UnresolvedTarget = 1,
        AmbiguousTargetName = 2,
        ExpressionDrivenTarget = 3,
        SharedTarget = 4
    };

    /// The refusal / success detail a canvas needs to explain a rewire.
    struct RewireOutcome
    {
        bool          applied = false;   ///< true only on a clean commit
        QString       status;            ///< "applied"/"rejected"/"diagnosed"/"conflict"
        /// On a LEGALITY refusal this is the real parser's own diagnostic
        /// verbatim, so a canvas-rejected wire reads exactly like a
        /// hand-edited scene's failure (MATERIAL_EDITOR.md:145).
        QString       message;
        RewireClosure closure = RewireClosure::Clean;
        bool          legalityRefused = false;
        bool          cycleRefused = false;
        QStringList   sharedChunks;             ///< sect. 3.7a (a)
        QStringList   outOfClosureReferrers;    ///< sect. 3.7a (b)
        QStringList   owners;                   ///< the owning roots (1 when clean)
        /// Chunks left with no reference -- BADGE them; this slice does
        /// not delete them (that is S20's reference-safe delete).  Empty
        /// whenever `applied` is false -- the controller clears it on
        /// every refusal path, including one that lands after the
        /// closure step already computed a non-empty report, so this
        /// never names an orphan from an edit that never committed.
        QStringList   nowUnreferenced;
    };

    /// Re-point `targetName`.`param` (occurrence `occurrence`, 0 for the
    /// only occurrence) at `newRefName`, as ONE undoable commit.  The
    /// category arguments are `RISE::ChunkCategory` ordinals -- the SAME
    /// convention the graph-snapshot node category uses, NOT
    /// ViewportBridge::Category.
    ///
    /// On ANY refusal the scene is left byte-identical; read
    /// `outcome.closure` to decide what to offer (a `SharedTarget`
    /// refusal is the one the Duplicate-node escape hatch unblocks).
    /// Returns `applied`; `outOutcome` (optional) receives the detail.
    bool rewireConnection(int targetCategory, const QString& targetName,
                           const QString& param, int occurrence,
                           int newRefCategory, const QString& newRefName,
                           RewireOutcome* outOutcome = nullptr);

    // ---- Node-graph canvas: reference-safe delete + duplicate (S20) --
    // Mirrors RISE_API_SceneEditController_DeleteGraphNode /
    // _DuplicateGraphNode and the macOS RISEViewportBridge's
    // identically-named section (docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S20,
    // implementing docs/gui/ENTITY_CREATION.md sect. 5's block-or-cascade
    // policy and MATERIAL_EDITOR.md sect. 3.7a's Duplicate escape hatch).
    // Both take the controller's commit mutex -- gate on
    // MainWindow::canUseSceneTransport() before calling.
    //
    // STANDING CAVEAT (same posture as the S18/S19 blocks above): this half
    // is CARRIED, not MSVC-verified -- written to match the macOS bridge
    // line for line and owed a Windows build.

    /// What a delete should do about the chunks BELOW the target that
    /// nothing else uses.  Mirrors
    /// `SceneEditController::GraphDeleteMode`'s ordinals (the C ABI passes
    /// it as a plain int: 0 = TargetOnly, 1 = Cascade).
    enum class GraphDeleteMode
    {
        TargetOnly = 0,
        Cascade    = 1
    };

    /// The reference-safety detail a canvas needs to explain a refused
    /// delete or show what a cascade took.
    struct DeleteOutcome
    {
        bool          applied = false;   ///< true only on a clean commit
        QString       status;            ///< "applied"/"rejected"/"diagnosed"/"conflict"
        QString       message;
        /// Only Clean / UnresolvedTarget / AmbiguousTargetName are
        /// reachable through this verb; the ordinals are shared with the
        /// rewire verb's mirror above (and pinned by the same
        /// static_asserts).
        RewireClosure closure = RewireClosure::Clean;
        bool          referenceRefused = false;   ///< offer "rewire those away first"
        bool          cascadeRefused = false;     ///< offer "delete without cascade"
        QStringList   referrers;                  ///< `chunk`.`param` of each blocker
        /// The chunks removed, in DOCUMENT ORDER.  EMPTY ON EVERY REFUSAL,
        /// including a cascade refusal (corrected -- S20 review round 1
        /// P2-2: an earlier draft of this comment claimed a cascade
        /// refusal leaves a sweep PREVIEW here; it cannot -- see
        /// `SceneEditController::DeleteResult::removed`'s own corrected
        /// comment for why). Read it as a mutation record only when
        /// `applied` is true.
        QStringList   removed;
    };

    /// The Duplicate-node fork's outcome.
    struct DuplicateOutcome
    {
        bool          applied = false;
        QString       status;
        QString       message;
        RewireClosure closure = RewireClosure::Clean;
        /// The DEDUPED name the copy actually landed under -- use THIS to
        /// select or rewire to the new node, never the requested name.
        /// Empty when nothing landed.
        QString       newName;
        /// The original's top-level document index at the moment of the
        /// fork, for placing the new node beside it; -1 when nothing landed.
        int           originalIndex = -1;
    };

    /// Delete the graph node `(category, name)` REFERENCE-SAFELY: a node
    /// anything still references is REFUSED in BOTH modes, with every
    /// referrer named.  `category` is a `RISE::ChunkCategory` ordinal (NOT
    /// ViewportBridge::Category).  `Cascade` also removes the chunks below
    /// the target that nothing else uses, as ONE undoable composite; a
    /// chunk shared with another graph is never swept, and a cascade that
    /// would reach one is refused whole.  On ANY refusal the scene is left
    /// byte-identical.  Returns `applied`; `outOutcome` receives the detail.
    bool deleteGraphNode(int category, const QString& name,
                          GraphDeleteMode mode,
                          DeleteOutcome* outOutcome = nullptr);

    /// Fork `(category, name)` into an owned copy placed IMMEDIATELY AFTER
    /// the original in declaration order, so every consumer the original
    /// had can legally be re-pointed at the copy -- MATERIAL_EDITOR.md
    /// sect. 3.7a's escape hatch out of a SharedTarget rewire refusal.
    /// SHALLOW: the copy shares everything the original referenced (the
    /// fork unshares exactly ONE level).  Returns `applied`; `outOutcome`
    /// receives the deduped `newName` to rewire at.
    bool duplicateGraphNode(int category, const QString& name,
                             DuplicateOutcome* outOutcome = nullptr);

    // ---- Node-graph canvas: drag-to-wire pre-checks + drag-to-reposition (S16/S21/S22) --
    // Mirrors RISE_API_SceneEditController_{CheckConnection,WouldCycle,
    // WriteGraphNodeLayoutPosition} / RISE_API_ConnectionLegality_
    // CheckConnectionByKeyword and the macOS RISEViewportBridge's
    // identically-named section (docs/gui/NODE_GRAPH_CANVAS.md sect. 6
    // S16/S21/S22). ADDED IN THIS SLICE -- the S14 bridge carry stopped at
    // painterMaterialGraph()/categoryTree() (the read-only S16 surface);
    // this half is the S21/S22 EDIT surface the Windows canvas widget
    // needs and had no carry yet. `checkConnection`/`wouldCycle` are pure
    // reads (safe at any time); `writeGraphNodeLayoutPosition` touches
    // only the layout sidecar file (never the CST document) but still
    // refuses while a render owns the scene -- gate on
    // MainWindow::canUseSceneTransport() before calling, same as every
    // other mutating call in this section.
    //
    // STANDING CAVEAT (same posture as the S18/S19/S20 blocks above): this
    // whole section is CARRIED, not MSVC-verified -- written to match the
    // macOS bridge line for line and owed a Windows build.

    /// Connection-legality pre-check (S17 passthrough) for the canvas's
    /// live drag-to-wire preview: may `candidateName` legally be bound at
    /// `targetName`.`param`? Categories are `RISE::ChunkCategory` ordinals,
    /// the SAME convention `rewireConnection`/`duplicateGraphNode` use, NOT
    /// ViewportBridge::Category. `outDiagnostic` (optional) receives the
    /// real parser's own diagnostic text on a refusal (left untouched on a
    /// legal verdict) -- show it as the drag's status line / tooltip.
    /// Returns false on a missing controller or empty argument, with no
    /// diagnostic written.
    bool checkConnection(int targetCategory, const QString& targetName,
                          const QString& param,
                          int candidateCategory, const QString& candidateName,
                          QString* outDiagnostic = nullptr) const;

    /// Forward-reachability cycle check (S17 `WouldCycle` passthrough):
    /// would wiring `fromName` to reference `toName` create a cycle?
    /// Categories as above. Returns false (never a crash) on a missing
    /// controller or either name failing to resolve -- treat an unresolved
    /// name as "cannot commit this wire" via `checkConnection` first, not
    /// as proof of safety.
    bool wouldCycle(int fromCategory, const QString& fromName,
                     int toCategory, const QString& toName) const;

    /// Pure-descriptor connection-legality check (`ConnectionLegality::
    /// CheckConnectionByKeyword` passthrough): may a chunk of
    /// `candidateKeyword`/`candidateCategory` legally be bound at
    /// `targetKeyword`.`param` -- for the "add node" palette's required-
    /// reference candidate picker, BEFORE the new node exists to address by
    /// name (unlike `checkConnection` above, which needs a real target
    /// chunk in the document). No document, no locking, safe at any time
    /// including before any scene is loaded -- does NOT require
    /// `m_controller` to be non-null (STATIC, unlike every other call in
    /// this section). `outDiagnostic` (optional) receives the diagnostic
    /// text on a refusal (left untouched on a legal verdict).
    static bool checkConnectionByKeyword(const QString& targetKeyword, const QString& param,
                                          const QString& candidateKeyword, int candidateCategory,
                                          QString* outDiagnostic = nullptr);

    /// Persist ONE node's canvas position into the layout sidecar -- the
    /// drag-to-reposition commit path (`WriteGraphLayoutPositions` with a
    /// single-element update, the shape a canvas drag commits: call ONCE
    /// on drag release, never mid-drag). `x`/`y` are the node's TOP-LEFT
    /// anchor in the same graph-space units `painterMaterialGraph()`'s
    /// `PainterGraphNode::x`/`.y` report. Returns true on success,
    /// INCLUDING the two documented no-op cases (position unchanged on
    /// disk; scene never saved yet) -- see the C++ method's own comment.
    /// Returns false on a null controller / empty name / an actual I/O
    /// failure / a render owning the scene; `outError` (optional)
    /// receives a message on false.
    bool writeGraphNodeLayoutPosition(const QString& name, double x, double y,
                                       QString* outError = nullptr);

    // ---- Node-graph canvas: add-node search palette (S16/S21/S22) ------

    /// Every registered Painter/Function/Material keyword whose descriptor
    /// category is `category` (a `RISE::ChunkCategory` ordinal), sorted
    /// lexicographically -- exactly the open keyword set `createChunkNode`
    /// can create. Pure descriptor read: no scene state, safe at any time.
    /// Empty for an unmodeled category or a null controller.
    QStringList paletteKeywords(int category) const;

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

    // QPointer, not a raw pointer (2026-07-24 shutdown-crash fix):
    // the lifetime contract says the engine must outlive the bridge,
    // and MainWindow::~MainWindow now enforces that ordering -- but if
    // it ever regresses (both are QObject children of MainWindow, and
    // creation-order teardown destroys the engine FIRST), every
    // `if (m_engine)` guard in this class degrades to a safe no-op
    // instead of writing into freed memory.
    QPointer<RenderEngine>    m_engine;
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
    // The ADMINISTRATIVE dispatcher -- the one behind agentHandleLine()
    // (Owner authority, permanently Commit autonomy; NOT one of the two
    // tool-call dispatchers below).
    std::unique_ptr<RISE::Agent::AgentRpcDispatcher> m_agentDispatcher;
    // Agent autonomy selector (2026-07): TWO more in-process
    // dispatchers -- the TOOL-CALL sessions, sibling to the
    // administrative `m_agentDispatcher` above -- that exist for
    // the whole bridge lifetime so `agentHandleToolCall()` never has to
    // construct one mid-turn.  The tool-call Owner session
    // `m_agentToolDispatcherOwner` borrows an
    // Owner-authority AgentSession (its own instance, separate from
    // `m_agentDispatcher`'s — `m_agentDispatcher` must stay permanently
    // Commit-capable for resolve_proposal, so ONLY this separate
    // instance's autonomy is ever toggled between Read/Commit via
    // AgentRpcDispatcher::SetAutonomy as `agentAutonomyLevel()`
    // changes).  The tool-call Propose session
    // `m_agentToolDispatcherPropose` borrows a SEPARATE
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
