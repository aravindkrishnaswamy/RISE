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
    /// Mirrors macOS RISEViewportBridge.agentHandleLine and is the
    /// bridge the Windows chat panel uses for model-requested tools.
    QString agentHandleLine(const QString& jsonRpcRequest);

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
        SceneVariant = 9  ///< scene_variant overlays (pick to re-derive that variant active)
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

    /// Clone the currently-active camera under a new name and
    /// promote the clone to active.  `proposedName` is the user's
    /// choice; on duplicate the controller appends a numeric dedup
    /// suffix.  Returns the actual name registered (the proposal
    /// verbatim if available, or a deduplicated variant), or an
    /// empty QString on no-active-camera / unclonable type.
    ///
    /// Persistence caveat: the clone lives only in the in-memory
    /// Scene/Job.  Reloading the .RISEscene file from the editor
    /// drops it (scene-text round-trip is the pending Phase 6
    /// work).  Caller should surface a one-shot warning the first
    /// time per session.
    QString addCameraFromActive(const QString& proposedName);

signals:
    /// Emitted on the UI thread with each completed preview frame.
    /// The QImage owns its own data (deep copy from C++ buffer).
    void imageUpdated(const QImage& image);

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
    std::unique_ptr<RISE::Agent::AgentRpcDispatcher> m_agentDispatcher;
    bool                       m_running = false;
};

#endif // VIEWPORTBRIDGE_H
