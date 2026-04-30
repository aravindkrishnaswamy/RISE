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

#pragma mark - Toolbar

@property (nonatomic) RISEViewportTool currentTool;

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
/// Mirrors RISE::SceneEditController::PanelMode.
typedef NS_ENUM(NSInteger, RISEViewportPanelMode) {
    RISEViewportPanelModeNone   = 0,  ///< empty (Select tool, nothing picked)
    RISEViewportPanelModeCamera = 1,  ///< camera manipulator selected
    RISEViewportPanelModeObject = 2,  ///< Select tool with picked object
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
