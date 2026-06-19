//////////////////////////////////////////////////////////////////////
//
//  RISEViewportBridge.h - Pure-Objective-C interface to the C++
//    SceneEditController for the macOS interactive 3D viewport.
//
//  Distinct from RISESceneEditorBridge (which is the text-editor
//  syntax-highlight / completion bridge).  See
//  docs/INTERACTIVE_EDITOR_PLAN.md §11.
//
//  Lifetime: this bridge borrows an existing RISEBridge's IJobPriv;
//  the RISEBridge must outlive the viewport bridge.
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

@class RISEBridge;
@class RISEViewportProperty;
@class RISEViewportGizmoHandle;

NS_ASSUME_NONNULL_BEGIN

/// Mirrors RISE::SceneEditController::Tool / RISE::SceneEditTool.
/// Kept as a Swift-friendly NS_ENUM; values must match the C++ enum
/// SceneEditTool_* in RISE_API.h.
typedef NS_ENUM(NSInteger, RISEViewportTool) {
    RISEViewportToolSelect           = 0,
    RISEViewportToolTranslateObject  = 1,
    RISEViewportToolRotateObject     = 2,
    RISEViewportToolScaleObject      = 3,
    RISEViewportToolOrbitCamera      = 4,
    RISEViewportToolPanCamera        = 5,
    RISEViewportToolZoomCamera       = 6,
    RISEViewportToolScrubTimeline    = 7,
    RISEViewportToolRollCamera       = 8
};

@interface RISEViewportBridge : NSObject

/// Construct over an existing RISEBridge.  The RISEBridge must have
/// successfully loaded a scene (-loadAsciiScene:) before the
/// viewport bridge can do useful work.  Returns nil if the host
/// bridge has no scene.
- (nullable instancetype)initWithHostBridge:(RISEBridge *)host;

/// Release the underlying SceneEditController.  Implicit on dealloc.
- (void)shutdown;

#pragma mark - Lifecycle

/// Spawn the C++ render thread.  Idempotent.
- (void)start;

/// Stop the render thread and join.  Idempotent.
- (void)stop;

@property (nonatomic, readonly) BOOL isRunning;

/// Shrink the scene Film so the interactive preview renders at a
/// screen-appropriate resolution rather than blindly inheriting
/// whatever the .RISEscene file declared.  Wraps the C++
/// `IJobPriv::ScaleFilmToFit` — never upscales, preserves the
/// scene's authored aspect ratio + pixelAR.  Caller passes the
/// available rendering-surface dims in pixels; the long edge is
/// also capped at `maxLongEdge`.  Call once after init, before
/// `start`.
- (void)scaleFilmToFitSurfaceW:(NSUInteger)surfaceW
                       surfaceH:(NSUInteger)surfaceH
                    maxLongEdge:(NSUInteger)maxLongEdge;

#pragma mark - Toolbar

@property (nonatomic) RISEViewportTool currentTool;

/// Photoshop-style toolbar category — the "slot" a tool sits in.
/// Mirrors `RISE::SceneEditController::ToolCategory`; numeric values
/// are part of the C-API contract.  `select` has a single tool;
/// `camera` covers Orbit/Pan/Zoom/Roll; `objectTransform` covers
/// Translate/Rotate/Scale and is the one that surfaces a gizmo
/// overlay when an object is selected.
typedef NS_ENUM(NSInteger, RISEViewportToolCategory) {
    RISEViewportToolCategorySelect          = 0,
    RISEViewportToolCategoryCamera          = 1,
    RISEViewportToolCategoryObjectTransform = 2
};

/// Map a tool to its category.  Pure-function (no bridge state); the
/// Swift toolbar uses this to compute slot membership for the current
/// `currentTool`.
+ (RISEViewportToolCategory)categoryForTool:(RISEViewportTool)tool;

/// Default sub-tool the category's slot shows before the user picks
/// anything from the flyout.  Pure-function.
+ (RISEViewportTool)defaultSubToolForCategory:(RISEViewportToolCategory)category;

/// Photoshop "last-used" memory: returns the sub-tool the user most
/// recently picked from this category's flyout, or the category
/// default if nothing's been picked yet.
- (RISEViewportTool)lastSubToolForCategory:(RISEViewportToolCategory)category
    NS_SWIFT_NAME(lastSubTool(for:));

#pragma mark - Gizmo overlay

/// Kind of gizmo handle — what UI gesture the platform overlay binds
/// to it.  Mirrors `RISE::SceneEditController::GizmoHandle::Kind`.
typedef NS_ENUM(NSInteger, RISEViewportGizmoKind) {
    RISEViewportGizmoKindAxisArrow        = 0,  ///< Translate: drag along world axis
    RISEViewportGizmoKindAxisPlane        = 1,  ///< Translate: drag in plane perpendicular to axis
    RISEViewportGizmoKindScreenCenter     = 2,  ///< Translate: drag in screen plane (axis = -1)
    RISEViewportGizmoKindAxisRing         = 3,  ///< Rotate: drag tangent to ring around world axis
    RISEViewportGizmoKindScreenRing       = 4,  ///< Rotate: view-axis spin (axis = -1)
    RISEViewportGizmoKindAxisScaleHandle  = 5,  ///< Scale: drag along world axis (cube glyph at tip)
    RISEViewportGizmoKindUniformScaleCube = 6   ///< Scale: uniform (axis = -1)
};

/// Recompute the gizmo handle array for the current Object selection
/// + tool + camera projection.  No-op when:
///   - the active tool isn't in the ObjectTransform category
///   - no Object is selected
///   - the camera projection is degenerate
/// Caller invokes this once per preview frame before reading
/// `gizmoHandles`.
- (void)refreshGizmoHandles;

/// Snapshot of the current gizmo handle array (empty if no gizmo
/// is currently shown).  Read AFTER `refreshGizmoHandles`.  The
/// returned NSArray is a fresh copy; values stay valid even if the
/// controller refreshes its internal array between calls.
@property (nonatomic, readonly) NSArray<RISEViewportGizmoHandle *> *gizmoHandles;

/// True iff a gizmo handle was hit on the most recent pointer-down
/// and the drag is still active (no pointer-up yet).  The overlay
/// uses this to switch the active-handle highlight on / off.
@property (nonatomic, readonly) BOOL gizmoDragActive;

/// Currently-active drag handle kind / axis, or -1 when no drag is
/// in progress.  Together with `gizmoDragActive` these drive the
/// overlay's active-handle styling.
@property (nonatomic, readonly) RISEViewportGizmoKind activeGizmoKind;
@property (nonatomic, readonly) NSInteger             activeGizmoAxis;

#pragma mark - Pointer events
//
// Coordinates are in the viewport surface's pixel space.  Bridge
// callers (Swift) are responsible for HiDPI / window-point ↔ pixel
// conversion.

- (void)pointerDownX:(double)x y:(double)y NS_SWIFT_NAME(pointerDown(x:y:));
- (void)pointerMoveX:(double)x y:(double)y NS_SWIFT_NAME(pointerMove(x:y:));
- (void)pointerUpX:(double)x y:(double)y   NS_SWIFT_NAME(pointerUp(x:y:));

//! Stable full-resolution camera dimensions for pointer-event coord
//! conversion in the Swift viewport view.  The rendered image's size
//! shrinks during a fast drag (preview-scale subsampling); using
//! image.size as the conversion target makes mLastPx (captured at
//! one scale level) and the next pointer event (in another) live in
//! mismatched coord spaces, producing 4×–32× pan/orbit jumps when
//! the scale state machine steps.  This getter returns the camera's
//! canonical full-res dims directly from the controller, so the
//! bridge's surface-point math stays stable across subsampling.
//! Returns (0,0) when no camera is attached.
@property (nonatomic, readonly) NSSize cameraSurfaceDimensions;

//! Scene's animation duration in scene-time units, derived from the
//! `animation_options` chunk's `time_end - time_start`.  Used by the
//! timeline scrubber to size its slider range.  Returns 0 when the
//! scene declared no animation options or no controller is attached;
//! the UI layer treats 0 as "no timeline" (slider hidden).
@property (nonatomic, readonly) double animationTimeStart;
@property (nonatomic, readonly) double animationTimeEnd;
@property (nonatomic, readonly) NSUInteger animationNumFrames;

//! Named animation paths for the side-panel dropdown.  `animationNames`
//! lists the scene's named animations in scene order; selecting one with
//! setSelectedAnimation: makes it active -- after which the animationTime*
//! properties above reflect the SELECTED animation's options and the caller
//! should re-scrub the preview to its start.  selectedAnimationIndex is -1
//! when the scene declares no animations.
@property (nonatomic, readonly, copy) NSArray<NSString *> *animationNames;
@property (nonatomic, readonly) NSInteger selectedAnimationIndex;
- (BOOL)setSelectedAnimation:(NSInteger)index;

#pragma mark - Time scrubber

- (void)scrubTimeBegin;
- (void)scrubTime:(double)t;
- (void)scrubTimeEnd;

#pragma mark - Properties-panel scrub gesture
//
// Bracket a click-and-drag scrub on a property's chevron handle.
// The controller bumps the preview-scale divisor between Begin and
// End so the rapid-fire SetProperty stream doesn't cancel every
// in-flight render before the outer tiles get a chance to update.

- (void)beginPropertyScrub;
- (void)endPropertyScrub;

#pragma mark - Edit commands

- (void)undo;
- (void)redo;

#pragma mark - Scene-file save (Phase 6.5)
//
// Persist transform edits + retained overrides back to a `.RISEscene`
// file via the SaveEngine round-trip pipeline.  The button driving
// these is gated on `hasUnsavedSceneChanges`; the dirty-changed
// block lets SwiftUI track the bool from the C++ edit pipeline
// without polling.

/// True iff at least one edit since the last load / save would
/// produce a non-NoOp save.  Drives the Save-button enable state.
/// Cheap O(1) (asks the controller's dirty trackers).
- (BOOL)hasUnsavedSceneChanges;

/// Save the in-memory edits to `path`.  Routes through
/// `SceneEditController::RequestSave` — caller is freed of the
/// cancel-and-park dance.  Returns status code:
///   0 = Saved          (bytes written)
///   1 = NoOp           (no edits to write; file untouched)
///   2 = Refused        (engine declined; original file untouched)
///   3 = Failed         (IO error; original file untouched)
/// `outErrorMessage` is populated on Refused or Failed with the
/// engine's diagnostic; empty otherwise.  Numeric mirror of
/// `SaveResult::Status`.
- (NSInteger)saveSceneTo:(NSString *)path
              errorMessage:(NSString * _Nullable * _Nullable)outErrorMessage
    NS_SWIFT_NAME(saveScene(to:errorMessage:));

typedef void (^RISEViewportDirtyChangedBlock)(BOOL hasUnsavedChanges);

/// Install a callback fired once per `hasUnsavedSceneChanges`
/// TRANSITION (clean→dirty or dirty→clean).  Edits that leave the
/// scene already-dirty do NOT fire it (steady-state edits are
/// folded).  Callback runs on the thread that drove the edit
/// (typically the main thread, which is where Swift drives the
/// controller from); SwiftUI listeners should still `Task { @MainActor }`
/// inside if they need to guarantee it.  Pass nil to detach.
- (void)setDirtyChangedBlock:(nullable RISEViewportDirtyChangedBlock)block;

/// Canonical scene time owned by the underlying SceneEditController.
/// Updated by every time-scrub AND by Undo / Redo of a SetSceneTime
/// edit; that's why callers should query this just before kicking
/// the production rasterizer instead of trusting the SwiftUI
/// timeline-slider state, which goes stale across undo/redo.
/// Returns 0 when no controller is attached.
- (double)lastSceneTime;

- (BOOL)requestProductionRender;

#pragma mark - Selection

/// Phase 3 picking is not yet implemented (waits on Phase 5 ray-cast
/// integration).  This is a test hook for setting selection by name
/// from the UI when picking lands.
- (void)setSelectedObjectName:(nullable NSString *)name;
@property (nonatomic, readonly, nullable) NSString *selectedObjectName;

#pragma mark - Live preview output

/// Block invoked on the main thread with a freshly-rendered preview
/// image (an NSImage snapshot of the interactive rasterizer's output).
/// Pass nil to clear.  Throttled to ~30Hz to keep the UI thread free.
typedef void (^RISEViewportImageBlock)(NSImage *image);

- (void)setImageBlock:(nullable RISEViewportImageBlock)block;

/// True if the live-preview rasterizer was successfully constructed
/// against the loaded scene.  False means the controller is still in
/// skeleton mode (the loaded scene didn't declare a usable shader, or
/// some other resource was missing).  Edits still mutate the in-memory
/// scene; the user can click "Render" to see the production result.
@property (nonatomic, readonly) BOOL hasLivePreview;

/// Drop exactly one upcoming preview frame.  Call right before
/// `start` after a production render so the production image stays
/// on screen until the user actually starts dragging.  Auto-clears
/// after one drop.
- (void)suppressNextFrame;

#pragma mark - Properties panel (descriptor-driven)

/// Discriminator for what the right-side panel should display.
/// Mirrors RISE::SceneEditController::PanelMode.  Numeric values are
/// kept in lockstep with RISEViewportCategory so the ints round-trip
/// through the C-API surface without translation.
typedef NS_ENUM(NSInteger, RISEViewportPanelMode) {
    RISEViewportPanelModeNone       = 0,  ///< nothing selected
    RISEViewportPanelModeCamera     = 1,
    RISEViewportPanelModeRasterizer = 2,
    RISEViewportPanelModeObject     = 3,
    RISEViewportPanelModeLight      = 4,
    RISEViewportPanelModeFilm       = 5,  ///< Output Settings (single Film per scene)
    RISEViewportPanelModeMaterial   = 6,  ///< Materials
    RISEViewportPanelModeMedium     = 7,  ///< Participating media
};

/// Mirrors RISE::SceneEditController::Category — drives the
/// accordion's section IDs and the selection round-trip.
typedef NS_ENUM(NSInteger, RISEViewportCategory) {
    RISEViewportCategoryNone       = 0,
    RISEViewportCategoryCamera     = 1,
    RISEViewportCategoryRasterizer = 2,
    RISEViewportCategoryObject     = 3,
    RISEViewportCategoryLight      = 4,
    RISEViewportCategoryFilm       = 5,   ///< Output Settings (single Film per scene)
    RISEViewportCategoryMaterial   = 6,   ///< Materials
    RISEViewportCategoryMedium     = 7,   ///< Participating media
};

/// Current panel mode — lets the SwiftUI parent decide whether to
/// show the panel at all and what header to draw.  Recompute after
/// any tool change or pointer-down event (picking).
@property (nonatomic, readonly) RISEViewportPanelMode panelMode;

/// Header string ("Camera", "Object: <name>", or empty) the platform
/// can render above the property list.
@property (nonatomic, readonly, copy) NSString *panelHeader;

/// Force a refresh of the panel snapshot from the live entity.
/// Called by the Swift panel before it re-reads `propertySnapshot`.
- (void)refreshProperties;

/// Snapshot of the current entity's properties.  Updated after
/// `refreshProperties` and after any successful setProperty: call.
- (NSArray<RISEViewportProperty *> *)propertySnapshot;

/// Apply an edit to a named property.  Returns YES on success.
- (BOOL)setPropertyName:(NSString *)name value:(NSString *)value;

/// Phase 4b: per-category property snapshot.  Returns the rows
/// for `category`'s expanded section.  Empty array if the section
/// has no selection (collapsed).  After `refreshProperties`, every
/// category with a non-empty selection has a populated snapshot.
- (NSArray<RISEViewportProperty *> *)propertySnapshotFor:(RISEViewportCategory)category
    NS_SWIFT_NAME(propertySnapshot(for:));

/// Phase 4b: per-category SetProperty.  Routes the edit through
/// the per-section selection so the Material section's edits go
/// to the right material even when Object is the primary
/// selection (auto-synced multi-section state).
- (BOOL)setPropertyForCategory:(RISEViewportCategory)category
                          name:(NSString *)name
                         value:(NSString *)value
    NS_SWIFT_NAME(setProperty(for:name:value:));

#pragma mark - Accordion list entries

/// Display names of the entries in `category`.  Pulled by the
/// accordion's list view; the platform UI caches by sceneEpoch.
- (NSArray<NSString *> *)categoryEntities:(RISEViewportCategory)category;

/// Phase 4b: per-category panel selection.  Returns the entity
/// name picked in `category`'s section, or empty when nothing is
/// picked (section collapsed).  Distinct from `selectionName`
/// which returns only the PRIMARY (most-recently-set) selection:
/// after an Object pick, both `selectionName(for: .object)` and
/// `selectionName(for: .material)` return non-empty (the latter
/// is the Object's bound material name, auto-filled).
- (NSString *)selectionNameFor:(RISEViewportCategory)category
    NS_SWIFT_NAME(selectionName(for:));

/// Phase 4b: is `category`'s accordion section expanded?
/// Tracked SEPARATELY from the per-category selection so a
/// click on a section header (which sends empty-name SetSelection)
/// still expands the section in the panel.  Without consulting
/// this flag, the panel would gate expansion on a non-empty
/// selection — collapsing every header-only click.
- (BOOL)isSectionExpandedFor:(RISEViewportCategory)category
    NS_SWIFT_NAME(isSectionExpanded(for:));

/// Phase 4b: collapse `category`'s section.  Clears the
/// expanded flag AND the per-category selection.  Does NOT
/// touch other sections — use `setSelection(.none, name:"")`
/// for the panel-wide collapse.
- (void)collapseSectionFor:(RISEViewportCategory)category
    NS_SWIFT_NAME(collapseSection(for:));

/// Scene-level active entity name for `category`, independent of
/// the UI selection.  Camera → active camera; Rasterizer → active
/// rasterizer chunk name; Film → "default" (a scene has exactly one
/// Film by construction); Object/Light/None → empty.  Used to
/// populate the dropdown on first scene load with the scene's
/// current active entity rather than "(pick one)".
- (NSString *)activeNameForCategory:(RISEViewportCategory)category
    NS_SWIFT_NAME(activeName(for:));

/// Current selection's category.  Drives which accordion section is
/// expanded.  Returns RISEViewportCategoryNone when nothing is
/// selected.
@property (nonatomic, readonly) RISEViewportCategory selectionCategory;

/// Current selection's entity name (manager name or rasterizer
/// chunk-name).  Empty when the section is open with no row picked.
@property (nonatomic, readonly, copy) NSString *selectionName;

/// Apply a (category, name) selection.  Empty `name` opens the
/// section without picking a row.  Camera / Rasterizer selections
/// also activate the named entity.  Returns YES on success.
- (BOOL)setSelectionCategory:(RISEViewportCategory)category name:(nullable NSString *)name
    NS_SWIFT_NAME(setSelection(_:name:));

/// Monotonic counter — bumped on any structural mutation that could
/// change a category's entity list.  Bridge callers cache
/// (epoch, category) → list and re-pull when this advances.
@property (nonatomic, readonly) NSUInteger sceneEpoch;

#pragma mark - Multi-camera

/// Clone the currently-active camera under a new name and switch
/// the scene to it.  `proposedName` is the user's choice; on
/// duplicate the controller appends a numeric dedup suffix.
/// Returns the actual name registered (the proposal verbatim if
/// available, or a deduplicated variant), or `nil` on no-active-
/// camera / unclonable type.  Caller passes a non-empty NSString
/// (an empty string falls back to "camera_copy").
///
/// Persistence caveat: the clone lives only in the in-memory
/// Scene/Job.  Reloading the .RISEscene file via the editor
/// drops it (scene-text round-trip is still pending Phase 6
/// work).  The Swift caller should surface a one-shot warning
/// the first time per session.
- (nullable NSString *)addCameraFromActive:(NSString *)proposedName
    NS_SWIFT_NAME(addCameraFromActive(proposedName:));

@end

/// One gizmo handle from the controller's screen-space layout.
/// Positions are in the camera's CURRENT image-pixel space — the same
/// space `cameraSurfaceDimensions` describes; the overlay applies the
/// view's `fullW`/`fullH` normalisation to map to widget points.
@interface RISEViewportGizmoHandle : NSObject
@property (nonatomic, readonly) RISEViewportGizmoKind kind;
@property (nonatomic, readonly) NSInteger axis;          ///< 0=X, 1=Y, 2=Z; -1 for screen-aligned
@property (nonatomic, readonly) CGFloat screenX;
@property (nonatomic, readonly) CGFloat screenY;
@property (nonatomic, readonly) CGFloat screenRadius;
@end

/// Single quick-pick preset, surfaced from the descriptor's
/// ParameterPreset list.  `value` is the parser-acceptable literal
/// the panel writes back when the user picks; `label` is the
/// human-readable name shown in the combo box.
@interface RISEViewportPropertyPreset : NSObject
@property (nonatomic, readonly) NSString *label;
@property (nonatomic, readonly) NSString *value;
@end

/// One row of the properties panel.  Field names mirror the C++
/// CameraProperty struct.  `kind` is the parser's ValueKind enum
/// cast to int.  `presets` is empty when the parameter has no
/// quick-pick combo entries; the panel falls back to a plain line
/// edit in that case.
@interface RISEViewportProperty : NSObject
@property (nonatomic, readonly) NSString *name;
@property (nonatomic, readonly) NSString *value;
@property (nonatomic, readonly) NSString *describing;
@property (nonatomic, readonly) NSInteger kind;
@property (nonatomic, readonly) BOOL editable;
@property (nonatomic, readonly) NSArray<RISEViewportPropertyPreset *> *presets;
/// Short unit suffix shown next to the editor field — "mm" for
/// camera sensor / focal / shift, "°" for angles, "scene units" for
/// focus_distance.  Empty when the descriptor declared no unit.
@property (nonatomic, readonly) NSString *unitLabel;
@end

NS_ASSUME_NONNULL_END
