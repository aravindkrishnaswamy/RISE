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

class RenderEngine;

namespace RISE {
    class SceneEditController;
    class IRayCaster;
    class IRasterizer;
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
    /// Stop the render thread and join.  Idempotent.
    void stop();
    bool isRunning() const { return m_running; }

    /// Drop exactly one upcoming preview frame.  Call right before
    /// `start` after a production render so the production image
    /// stays on screen until the user actually starts dragging.
    void suppressNextFrame();

    /// Shrink the scene Film so the interactive preview renders at a
    /// screen-appropriate resolution rather than blindly inheriting
    /// whatever the .RISEscene file declared.  Wraps
    /// `IJobPriv::ScaleFilmToFit` — never upscales, preserves the
    /// scene's authored aspect ratio + pixelAR.  Caller passes the
    /// available rendering-surface dims in window pixels; the long
    /// edge is also capped at `maxLongEdge`.  Call once after the
    /// bridge is constructed and before `start()`.
    void scaleFilmToFit(int surfaceW, int surfaceH, int maxLongEdge);

    void setTool(ViewportTool t);

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

    /// Canonical scene time owned by the underlying SceneEditController.
    /// Updated by every time-scrub AND by Undo / Redo of a SetSceneTime
    /// edit; that's why MainWindow::onRender queries this just before
    /// kicking the production rasterizer instead of trusting
    /// ViewportTimeline::currentTime, which goes stale across
    /// undo/redo.  Returns 0 when no controller is attached.
    double lastSceneTime() const;

    bool requestProductionRender();

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
        Medium     = 7    ///< Participating media
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

private:
    void buildLivePreview();
    void releaseLivePreview();

    RenderEngine*             m_engine = nullptr;
    RISE::SceneEditController* m_controller = nullptr;
    RISE::IRayCaster*          m_caster = nullptr;        // preview caster, max-recursion 1
    RISE::IRayCaster*          m_polishCaster = nullptr;  // polish caster, max-recursion 2 (one bounce of glossy / refl / refr)
    RISE::IRasterizer*         m_interactiveRasterizer = nullptr;
    ViewportPreviewSink*       m_previewSink = nullptr;
    bool                       m_running = false;
};

#endif // VIEWPORTBRIDGE_H
