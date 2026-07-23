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
@class RISEViewportNavNub;

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

/// Secure-MCP slice 5c: the result of -startAgentHostedServerWithLabel:.
/// `ok` false means the server did not start (bind failure, or a server
/// was already running -- see that method's doc for the idempotent-
/// refusal contract); `port`/`bearerToken` are meaningful only when `ok`
/// is true.
@interface RISEAgentHostedServerInfo : NSObject
@property (nonatomic, readonly) BOOL ok;
@property (nonatomic, readonly) NSUInteger port;
/// The per-launch bearer token an external MCP client must present via
/// `Authorization: Bearer <token>`.  NEVER LOG THIS — the Swift caller
/// must only ever put it in a copyable text field, never in an
/// NSLog/print/os_log call.
@property (nonatomic, readonly, copy) NSString *bearerToken;
@property (nonatomic, readonly, copy) NSString *message;
@end

/// Snapshot of the scene's IBL environment (a scene-level singleton: an
/// hdr/exr painter bound via `radiance_*` on the active rasterizer, NOT
/// an ILight).  See SceneEditController::EnvironmentInfo and
/// docs/gui/ENVIRONMENT_SECTION.md.  Returned by -[RISEViewportBridge environmentInfo].
@interface RISEEnvironmentInfo : NSObject
@property (nonatomic, readonly) BOOL hasEnvironment;   ///< a radiance_map painter is bound
@property (nonatomic, readonly) BOOL proceduralSky;    ///< a procedural sky / non-painter map is installed (read-only)
@property (nonatomic, readonly) BOOL editable;         ///< false when the active rasterizer takes no radiance map (MLT / pixel*)
@property (nonatomic, readonly) NSString *painterName; ///< bound painter name ("" if none)
@property (nonatomic, readonly) NSString *file;        ///< resolved HDRI path ("" if unresolved / procedural)
@property (nonatomic, readonly) double scale;          ///< intensity multiplier
@property (nonatomic, readonly) double orientX;        ///< Euler rotation X in DEGREES
@property (nonatomic, readonly) double orientY;        ///< Euler rotation Y in DEGREES
@property (nonatomic, readonly) double orientZ;        ///< Euler rotation Z in DEGREES
@property (nonatomic, readonly) BOOL background;        ///< map visible behind geometry
@end

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

/// Spawn the C++ render thread WITHOUT its one-shot initial render
/// pass.  Call this (instead of `start`) when restarting the viewport
/// right after a production render: the finished render is already on
/// screen and the render thread stays parked until the user interacts,
/// so the production image survives.  Using this avoids the
/// "render flashes then flips back to the live preview" bug — the
/// interactive rasterizer never produces the overwriting frame in the
/// first place, so no display-layer frame-suppression is needed.
- (void)startSuppressingInitialRender;

/// Stop the interactive render thread and join.  Idempotent.  Does NOT
/// touch the attached SceneEditController's agent-render worker -- a
/// production render submitted immediately afterward (via
/// -[RISEBridge rasterize] et al, which route through
/// RunProductionRenderComposed) is still served normally.
///
/// Model-B F2 slice S4 fix round 4: this used to call the C++
/// controller's monolithic Stop(), which ALSO permanently retired the
/// agent-render worker (mAgentRenderStop is a one-shot flag; the worker
/// thread is spawned only once, in the constructor, and nothing ever
/// respawns it) -- so RenderViewModel's startRender/startAnimationRender
/// calling this immediately before a production submit poisoned the
/// controller for the rest of its lifetime, and the production render
/// (and every later one, interactive-viewport restart notwithstanding)
/// was refused with "controller stopped".  Wired to
/// RISE_API_SceneEditController_StopInteractive instead; see that
/// function's doc and SceneEditController::StopInteractive's header doc.
- (void)stop;

@property (nonatomic, readonly) BOOL isRunning;

/// Shrink the scene Film so the interactive preview renders at a
/// screen-appropriate resolution rather than blindly inheriting
/// whatever the .RISEscene file declared.  Wraps the C++
/// `IJobPriv::SetViewportFit`, which caches the fit params and
/// applies the fit immediately; the cache lets a subsequent D2 full
/// re-derive (variant switch / CST edit) re-apply the SAME fit so
/// the preview stays screen-sized instead of jumping to authored
/// full-res.  Never upscales, preserves the scene's authored aspect
/// ratio + pixelAR.  Caller passes the available rendering-surface
/// dims in pixels; the long edge is also capped at `maxLongEdge`.
/// Call once after init, before `start`.
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

#pragma mark - Nav gizmo (axis-ball) + view navigation

/// Recompute the six axis-ball nubs (±X/±Y/±Z) for a ball centered at
/// (centerX, centerY) with `ballRadius`, each nub `nubRadius`, all in widget
/// points.  Returns NO (empty nub array) when there's no supported (pinhole)
/// interactive camera.  Call once per preview frame before reading
/// `navGizmoNubs` — see docs/gui/CAMERAS_AND_VIEWS.md §4.
- (BOOL)refreshNavGizmoWithCenterX:(CGFloat)centerX centerY:(CGFloat)centerY
                        ballRadius:(CGFloat)ballRadius nubRadius:(CGFloat)nubRadius
    NS_SWIFT_NAME(refreshNavGizmo(centerX:centerY:ballRadius:nubRadius:));

/// Snapshot of the current nub array (empty if the gizmo isn't shown).
/// Read AFTER `refreshNavGizmo…`.
@property (nonatomic, readonly) NSArray<RISEViewportNavNub *> *navGizmoNubs;

/// Hit-test a widget-space point against the nubs (front-facing win ties).
/// Returns the nub index or -1.
- (NSInteger)navGizmoNubAtX:(CGFloat)x y:(CGFloat)y
    NS_SWIFT_NAME(navGizmoNubAt(x:y:));

/// Non-destructive view navigation: these drive the transient free-fly
/// ViewportPose (the interactive pass renders through it) and NEVER mutate a
/// scene camera.  Each returns YES on success / the documented refusal cases.
- (BOOL)snapViewToAxis:(NSInteger)axis negative:(BOOL)negative
    NS_SWIFT_NAME(snapView(toAxis:negative:));
- (BOOL)enterFreeFly;
- (BOOL)exitFreeFly;
@property (nonatomic, readonly) BOOL freeFlyActive;
- (BOOL)setHomeView;
- (BOOL)goToHomeView;
@property (nonatomic, readonly) BOOL hasHomeView;

/// B3 fly-then-stamp: promote the current free-fly view into a NEW named
/// scene camera (named from a CST-safe canonicalization of `proposedName`,
/// then dedup-suffixed; the new camera becomes active). Returns the created
/// camera's name, or nil when
/// there's no free-fly pose to stamp or the edit was refused.
- (nullable NSString *)stampViewToNewCamera:(NSString *)proposedName
    NS_SWIFT_NAME(stampViewToNewCamera(_:));

/// user-review P1-1: pane-indexed navigation twins.  The methods above alias
/// pane 0 (the §7.4 C-ABI contract); the N-up nav overlay is drawn on the
/// primary pane and calls THESE with its pane index, so it moves the pane it's
/// actually over.  (paneEnterFreeFly / paneExitFreeFly already exist below.)
- (BOOL)snapPaneView:(NSUInteger)pane toAxis:(NSInteger)axis negative:(BOOL)negative
    NS_SWIFT_NAME(snapPaneView(_:toAxis:negative:));
- (BOOL)isPaneFreeFlyActive:(NSUInteger)pane
    NS_SWIFT_NAME(isPaneFreeFlyActive(_:));
- (BOOL)paneSetHomeView:(NSUInteger)pane
    NS_SWIFT_NAME(paneSetHomeView(_:));
- (BOOL)paneGoToHomeView:(NSUInteger)pane
    NS_SWIFT_NAME(paneGoToHomeView(_:));
- (nullable NSString *)stampPaneViewToNewCamera:(NSUInteger)pane proposedName:(NSString *)proposedName
    NS_SWIFT_NAME(stampPaneViewToNewCamera(_:proposedName:));

#pragma mark - Named Views (B1)

/// Capture the current view (free-fly pose if active, else the active camera)
/// as a new named view.  Returns NO when there's no capturable camera.
- (BOOL)captureNamedView:(NSString *)name NS_SWIFT_NAME(captureNamedView(_:));
/// The named views, in order (an NSArray of their names).
@property (nonatomic, readonly) NSArray<NSString *> *namedViewNames;
/// Restore view `idx` into the transient ViewportPose (non-destructive).
- (BOOL)restoreNamedView:(NSInteger)idx NS_SWIFT_NAME(restoreNamedView(_:));
/// Re-capture the current view into slot `idx`.
- (BOOL)updateNamedView:(NSInteger)idx NS_SWIFT_NAME(updateNamedView(_:));
/// Remove view `idx`.
- (BOOL)deleteNamedView:(NSInteger)idx NS_SWIFT_NAME(deleteNamedView(_:));
/// Promote view `idx` into a NEW scene camera (named from a CST-safe
/// canonicalization of `proposedName`; becomes active). Returns the created
/// camera's name, or nil on refusal.
- (nullable NSString *)promoteNamedView:(NSInteger)idx name:(NSString *)proposedName
    NS_SWIFT_NAME(promoteNamedView(_:name:));

#pragma mark - Viewport render modes (P1, docs/gui/RENDER_MODES.md §5)

/// Registry entries with `viewportSelectable == true`, in registry
/// (UI) order — the set the mode dropdown / View menu offers.  Each
/// dictionary has string values for keys "name" ("preview", "normals",
/// ...), "title" ("Shaded Preview", ...), "question" (the tooltip
/// text), "wantsDenoise" ("1"/"0" — GUI render modes P2a: the registry's
/// `wantsDenoise` flag, added so the DENOISED-label formatter can key off
/// it instead of a hardcoded `mode == "preview"` check now that
/// BeautyVariant modes genuinely denoise too), and "isVariant" ("1"/"0" —
/// P2a review fix: `RISE::Implementation::IsBeautyVariantMode`, so the
/// x-ray toggle can disable itself while the active mode is a
/// BeautyVariant row — see RISE_API_GetViewportRenderModeIsVariant's doc).
/// Registry-level (not controller-scoped) but still guarded on
/// `_controller` for consistency with every other bridge accessor — empty
/// array when no controller is attached.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)viewportRenderModes;

/// The registry wire name of the CURRENTLY active viewport render mode
/// ("preview" when no controller is attached, matching the C-ABI's own
/// null-controller default).
@property (nonatomic, readonly, copy) NSString *viewportRenderMode;

/// Switch the interactive viewport to render-mode `name` (a wire name
/// from -viewportRenderModes).  Returns NO on a null controller/name,
/// an unknown or non-selectable name, or the documented controller-
/// level refusals (render-owns-scene, skeleton mode) — see
/// SceneEditController::SetViewportRenderMode.  On success the
/// controller kicks its own repaint; callers should re-read
/// -viewportRenderMode afterward rather than assuming `name` stuck.
- (BOOL)setViewportRenderMode:(NSString *)name
    NS_SWIFT_NAME(setViewportRenderMode(_:));

#pragma mark - Viewport X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis")

/// Whether the interactive viewport currently resolves the primary hit
/// THROUGH transmissive surfaces to the first opaque hit.  Applies to
/// EVERY viewport render mode, INCLUDING the shaded "preview" pipeline
/// (not just the four data modes) -- resolution lives in the caster
/// layer, so it is orthogonal to which mode is active.  DEFAULT ON: the
/// viewport starts see-through, and the controller resets the flag back
/// to ON on every scene rebind (RebindEditorToJob).  NO on a null
/// controller (matching the C-ABI's own null-controller default), and
/// also NO while a production/agent render owns the scene
/// (RISE_API_SceneEditController_GetViewportXray's documented
/// transient).
- (BOOL)viewportXray;

/// Set the x-ray flag.  Applies immediately regardless of which mode is
/// active (including preview).  Returns NO on a null controller or the
/// documented controller-level refusals (render-owns-scene, skeleton
/// mode) -- no caster rebuild happens either way, just a flag stamp.
/// Callers should re-read -viewportXray afterward rather than assuming
/// `on` stuck.
- (BOOL)setViewportXray:(BOOL)on
    NS_SWIFT_NAME(setViewportXray(_:));

#pragma mark - N-up multi-viewport (P3, docs/gui/RENDER_MODES.md §7)
//
// Up to four pane slots; the layout selects the visible subset (§7.2).
// Every setter is fail-closed per the controller contract (§7.4):
// unknown pane / hidden pane / render-owns-scene => NO, nothing
// mutated.  Pane 0's DISPLAY keeps using the legacy -setImageBlock: /
// -pointerDown(x:y:) family unchanged -- see RISEViewportBridge.mm's
// "N-up pane-0 sink" doc comment for why that's the correct choice
// rather than migrating pane 0 onto -setPaneImageBlock:forPane:.  The
// entry points below cover the layout/primary/chrome plumbing that
// spans all four panes, plus display + input routing for panes 1-3.

/// Mirrors SceneEditController::ViewportLayout.  §7.2: Single = pane 0
/// only; TwoH = 0|1 side by side; OnePlusTwo = 0 big + 1,2 stacked;
/// Quad = 0-3 in a 2x2 grid.
typedef NS_ENUM(NSInteger, RISEViewportLayout) {
    RISEViewportLayoutSingle     = 0,
    RISEViewportLayoutTwoH       = 1,
    RISEViewportLayoutOnePlusTwo = 2,
    RISEViewportLayoutQuad       = 3,
};

/// Mirrors SceneEditController::PaneVantageKind.
typedef NS_ENUM(NSInteger, RISEViewportVantageKind) {
    RISEViewportVantageSceneCamera = 0,
    RISEViewportVantageFreeFly     = 1,
    RISEViewportVantageNamedView   = 2,
};

/// The active N-up layout.  Defaults to Single (matches the
/// controller's construction default) when no controller is attached.
@property (nonatomic) RISEViewportLayout viewportLayout;

/// The primary pane index (0-3; §7.8 decision 1: a non-navigation
/// click in any pane promotes it).  0 when no controller is attached.
@property (nonatomic) NSUInteger primaryPane;

/// Per-pane render mode -- the SAME registry wire names
/// -viewportRenderModes lists.  Pane 0 forwards to
/// -setViewportRenderMode: / -viewportRenderMode controller-side
/// (alias contract, §7.4): calling either surface for pane 0 is
/// equivalent, so callers may safely treat all four panes uniformly.
- (NSString *)paneRenderMode:(NSUInteger)pane
    NS_SWIFT_NAME(paneRenderMode(_:));
- (BOOL)setPaneRenderMode:(NSUInteger)pane name:(NSString *)name
    NS_SWIFT_NAME(setPaneRenderMode(_:name:));

/// Per-pane vantage.  Pane 0's free-fly twins alias -enterFreeFly /
/// -exitFreeFly (§7.4); pane 0's Scene-camera / NamedView setters have
/// no pre-existing un-indexed equivalent, so calling them on pane 0 is
/// a new, valid operation.
- (BOOL)setPaneVantageSceneCamera:(NSUInteger)pane
    NS_SWIFT_NAME(setPaneVantageSceneCamera(_:));
- (BOOL)setPaneVantageNamedView:(NSUInteger)pane name:(NSString *)name
    NS_SWIFT_NAME(setPaneVantageNamedView(_:name:));
- (BOOL)paneEnterFreeFly:(NSUInteger)pane
    NS_SWIFT_NAME(paneEnterFreeFly(_:));
- (BOOL)paneExitFreeFly:(NSUInteger)pane
    NS_SWIFT_NAME(paneExitFreeFly(_:));

/// `outKind` receives the vantage kind; `outNamedView` (optional, may
/// be NULL) receives the NamedView name when kind == NamedView (""
/// otherwise).  Returns NO on a null controller / invalid pane
/// (outputs left untouched in that case).
- (BOOL)getPaneVantage:(NSUInteger)pane
                   kind:(RISEViewportVantageKind *)outKind
              namedView:(NSString * _Nullable * _Nullable)outNamedView
    NS_SWIFT_NAME(getPaneVantage(_:kind:namedView:));

/// Pane render-surface pixel dims -- the GUI's pane rect, in PIXELS
/// (backing-scale aware).  0/0 resets to film dims.  Call for every
/// VISIBLE pane on layout switch and on window/pane resize.
- (BOOL)setPaneSurfaceDims:(NSUInteger)pane width:(NSUInteger)w height:(NSUInteger)h
    NS_SWIFT_NAME(setPaneSurfaceDims(_:width:height:));

/// Pane-indexed pointer routing.  Coordinates are in the SAME
/// full-resolution film-pixel space -cameraSurfaceDimensions
/// describes: RISE's Film dims are a scene-level singleton (imaging
/// dims live on the `film` chunk, not per-camera -- see
/// docs/SCENE_CONVENTIONS.md) shared by every pane regardless of which
/// camera/vantage that pane shows, so the one existing dims accessor
/// is correct for all four cells (see RISEViewportBridge.mm's N-up doc
/// comment for the full reasoning and the C-ABI gap this works
/// around).  -onPanePointerDown returns NO to mean "drop the gesture"
/// (hidden pane / render-owns-scene) -- callers MUST swallow the
/// matching Move/Up sequence for that physical gesture when Down
/// returns NO, exactly like the single-viewport region-drag's
/// `suppressPointerUntilUp` pattern.
- (BOOL)onPanePointerDown:(NSUInteger)pane x:(double)x y:(double)y
    NS_SWIFT_NAME(onPanePointerDown(_:x:y:));
- (void)onPanePointerMove:(NSUInteger)pane x:(double)x y:(double)y
    NS_SWIFT_NAME(onPanePointerMove(_:x:y:));
- (void)onPanePointerUp:(NSUInteger)pane x:(double)x y:(double)y
    NS_SWIFT_NAME(onPanePointerUp(_:x:y:));

/// Per-pane refinement status -- same phase/scale contract as
/// -refinementPhaseWithScaleDivisor:, scoped to `pane`.
- (int)paneRefinementPhase:(NSUInteger)pane scaleDivisor:(unsigned int *)scaleDivisor
    NS_SWIFT_NAME(paneRefinementPhase(_:scaleDivisor:));

// -setPaneImageBlock:forPane: (panes 1-3 image stream) is declared
// below, next to -setImageBlock:, because it takes the
// RISEViewportImageBlock type typedef'd in that section.

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

// Named animations are surfaced as a first-class accordion Category
// (RISEViewportCategoryAnimation) — the generic categoryEntities: /
// activeName: / setSelection:name: surface lists + activates them, so there
// are no bespoke animation accessors here.  animationTime* above already
// reflect the active animation's options.

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

/// Human-readable label of the next undo/redo step ("Translate",
/// "Agent Edit", ...) for the Edit-menu items.  Empty string when the
/// corresponding stack is empty.
- (NSString *)undoActionLabel;
- (NSString *)redoActionLabel;

#pragma mark - Editor live-sync (UI refinement item 1)

/// The retained CST Document's serialization — the exact bytes a save
/// would write.  Empty string when no document is retained.  Takes the
/// controller's commit mutex: DO NOT call while a render owns the scene
/// (gate on the scene-editable predicate, like the proposals poll).
- (NSString *)serializedSceneText;

/// Retained CST head version — uuid is fresh per load, revision bumps
/// iff the document content changed.  Returns NO (and zeros) with no
/// controller.  Same do-not-call-during-renders caveat as above.
- (BOOL)getSceneTextVersionUuid:(unsigned long long *)outUuid
                       revision:(unsigned long long *)outRevision;

#pragma mark - Refinement pause + status (UI redesign, design brief A2)

/// Pause progressive refinement: the interactive render thread is
/// joined, the on-screen image survives, CPU goes quiet.  Edits made
/// while paused mutate the scene normally and appear on resume.
- (void)pauseRefinement;
/// Resume after pauseRefinement.  No-op when not paused.  Note any
/// (re)start of the interactive loop — e.g. the post-production-render
/// restart — also clears the paused state.
- (void)resumeRefinement;
- (BOOL)isRefinementPaused;

/// Honest interactive-refinement readout: phase is
/// 0 Idle / 1 Rendering / 2 Refining / 3 Polishing / 4 Paused
/// (-1 when no controller).  scaleDivisor (when non-NULL) receives the
/// preview-resolution divisor, 1..32 powers of two, 1 = full res.
/// There is NO "pass N of M" in the interactive loop — it refines by a
/// 6-level resolution ladder + a denoised polish pass; this reports
/// exactly that.
- (int)refinementPhaseWithScaleDivisor:(unsigned int *)scaleDivisor;

#pragma mark - Interactive region-of-interest (UI redesign, design brief A4)

/// Restrict full-resolution interactive passes to an INCLUSIVE box in
/// full-res film pixel coordinates.  Coarse ladder passes still render
/// full-frame; the region auto-clears before any production render.
- (void)setInteractiveRegionLeft:(unsigned int)left
                             top:(unsigned int)top
                           right:(unsigned int)right
                          bottom:(unsigned int)bottom;
- (void)clearInteractiveRegion;
/// TRUE (+ coords) when a region is active.
- (BOOL)getInteractiveRegionLeft:(unsigned int *)left
                             top:(unsigned int *)top
                           right:(unsigned int *)right
                          bottom:(unsigned int *)bottom;
/// TRUE when the interactive rasterizer honors regions (honesty query;
/// the stock interactive rasterizer always does).
- (BOOL)interactiveRasterizerHonorsRegion;

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

/// N-up (§7): live-preview image stream for panes 1-3 ONLY (pane 0
/// keeps using -setImageBlock: above -- see RISEViewportBridge.mm's
/// "N-up pane-0 sink" doc comment for why).  `pane` must be 1, 2, or 3;
/// a call with pane==0 or an out-of-range pane is a no-op.  Same
/// ~30Hz-throttled NSImage contract as -setImageBlock:.
- (void)setPaneImageBlock:(nullable RISEViewportImageBlock)block forPane:(NSUInteger)pane
    NS_SWIFT_NAME(setPaneImageBlock(_:forPane:));

/// True if the live-preview rasterizer was successfully constructed
/// against the loaded scene.  False means the controller is still in
/// skeleton mode (the loaded scene didn't declare a usable shader, or
/// some other resource was missing).  Edits still mutate the in-memory
/// scene; the user can click "Render" to see the production result.
@property (nonatomic, readonly) BOOL hasLivePreview;

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
    RISEViewportCategoryAnimation  = 8,   ///< Named animation paths (pick to activate)
    RISEViewportCategorySceneVariant = 9, ///< scene_variant overlays (pick to re-derive that variant active)
    RISEViewportCategoryPainter    = 10,  ///< Painters (union of the IPainter + IScalarPainter managers)
    RISEViewportCategoryGeometry   = 11,  ///< Geometry (every "*_geometry" chunk -- GUI redesign 2026-07-22)
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

/// Jump-to-definition (GUI redesign, 2026-07-22): for the PRIMARY
/// snapshot's Reference-kind row at `idx`, resolve which category the
/// row's value names (probing the descriptor's declared target
/// categories against the live managers, first-wins).  Returns NO for
/// non-Reference rows / dangling references -- the context-menu item
/// stays hidden.  On YES the caller navigates via -setSelection:name:.
- (BOOL)propertyJumpTargetAtIndex:(NSUInteger)idx
                      outCategory:(RISEViewportCategory *)outCategory
                          outName:(NSString * _Nullable * _Nonnull)outName
    NS_SWIFT_NAME(propertyJumpTarget(atIndex:outCategory:outName:));

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

/// "Reveal in scene file" (design comp ⌗ affordance): resolve entity
/// (category, name) to its byte offset + 1-based line number inside
/// -serializedSceneText.  Returns NO (outputs left untouched) on a null
/// controller, no retained CST document, an unresolvable/ambiguous name,
/// or a category with no chunk-name addressing scheme (Rasterizer/Film/
/// None — see SceneEditController::EntitySourceLocation's doc comment).
/// Same do-not-call-during-renders caveat as -serializedSceneText /
/// -getSceneTextVersionUuid:.
- (BOOL)getEntitySourceLocationForCategory:(RISEViewportCategory)category
                                       name:(NSString *)name
                                 byteOffset:(unsigned long long *)outOffset
                                       line:(unsigned int *)outLine
    NS_SWIFT_NAME(getEntitySourceLocation(for:name:byteOffset:line:));

#pragma mark - Source traceability (any UI element <-> scene-file span)

/// Resolve a UI element's scene-file span (SceneEditController::ResolveSourceSpan).
/// `param` empty = the whole chunk (byteLength 0); non-empty = the `occ`-th matching
/// param's tight `role value` run.  Fills byteOffset/byteLength (UTF-8 bytes into
/// -serializedSceneText) + 1-based line/column.  Returns NO (outputs untouched) on a
/// null controller, no retained CST, or an unresolvable ref.  Same
/// do-not-call-during-renders caveat as -getEntitySourceLocation.
- (BOOL)resolveSourceSpanForCategory:(RISEViewportCategory)category
                                name:(NSString *)name
                               param:(NSString *)param
                          occurrence:(int)occ
                          byteOffset:(unsigned long long *)outOffset
                          byteLength:(unsigned long long *)outLength
                                line:(unsigned int *)outLine
                              column:(unsigned int *)outColumn
    NS_SWIFT_NAME(resolveSourceSpan(for:name:param:occurrence:byteOffset:byteLength:line:column:));

/// Reverse: the UI element whose scene-file source contains UTF-8 byte `offset`
/// (SceneEditController::SourceRefAtByteOffset).  On success fills `outCategory`
/// (a RISEViewportCategory raw value), `outName`, `outParam` (empty if not on a
/// specific param), and `outOccurrence`.  Returns NO when the offset isn't inside an
/// addressable entity/singleton chunk.
- (BOOL)sourceRefAtByteOffset:(unsigned long long)offset
                     category:(RISEViewportCategory *)outCategory
                         name:(NSString * _Nullable * _Nullable)outName
                        param:(NSString * _Nullable * _Nullable)outParam
                   occurrence:(int *)outOccurrence
    NS_SWIFT_NAME(sourceRef(atByteOffset:category:name:param:occurrence:));

#pragma mark - Entity creation + painter CRUD (entity-creation slice)
//
// Mirrors RISE_API_SceneEditController_{EntityTemplateCount,
// EntityTemplateLabel,InstantiateEntityTemplate,DuplicateEntity,
// RemoveEntity}.  The three mutating calls (instantiate/duplicate/
// remove) take the controller's commit mutex -- same do-not-call-
// during-renders caveat as -serializedSceneText / -saveSceneTo:
// errorMessage: (gate on the scene-editable predicate before calling).

/// Number of "Add Entity" templates registered for `category` (0 for
/// categories with none -- Camera/Rasterizer/Film/Animation/
/// SceneVariant/None).
- (NSUInteger)entityTemplateCountForCategory:(RISEViewportCategory)category
    NS_SWIFT_NAME(entityTemplateCount(for:));

/// Display label for the template at `idx` within `category` (e.g.
/// "Sphere", "Omni Light").  Empty string for a null controller or an
/// out-of-range idx.
- (NSString *)entityTemplateLabelForCategory:(RISEViewportCategory)category
                                        index:(NSUInteger)idx
    NS_SWIFT_NAME(entityTemplateLabel(for:index:));

/// Instantiate the template at `idx` within `category`.  Returns the
/// AgentCommitResult's `applied` flag; `outName` (optional, may be
/// NULL) receives the deduped instance name on success (nil on
/// failure), `outMessage` (optional, may be NULL) receives a human-
/// readable message on failure (nil on success with nothing to say).
/// A multi-chunk template undoes as several separate steps -- see the
/// C++ method's header doc.
- (BOOL)instantiateEntityTemplateForCategory:(RISEViewportCategory)category
                                        index:(NSUInteger)idx
                                      outName:(NSString * _Nullable * _Nullable)outName
                                   outMessage:(NSString * _Nullable * _Nullable)outMessage
    NS_SWIFT_NAME(instantiateEntityTemplate(for:index:outName:outMessage:));

/// Duplicate the named entity in `category` under a freshly-deduped
/// name.  Returns `applied`; `outName` / `outMessage` as above (each
/// optional, may be NULL).
- (BOOL)duplicateEntityForCategory:(RISEViewportCategory)category
                               name:(NSString *)name
                            outName:(NSString * _Nullable * _Nullable)outName
                         outMessage:(NSString * _Nullable * _Nullable)outMessage
    NS_SWIFT_NAME(duplicateEntity(for:name:outName:outMessage:));

/// Remove the named entity in `category` -- refused with a non-nil
/// `outMessage` if it is still referenced (e.g. a material a
/// standard_object still binds) or not found.  Returns `applied`;
/// `outMessage` as above (optional, may be NULL).
- (BOOL)removeEntityForCategory:(RISEViewportCategory)category
                            name:(NSString *)name
                      outMessage:(NSString * _Nullable * _Nullable)outMessage
    NS_SWIFT_NAME(removeEntity(for:name:outMessage:));

#pragma mark - Environment / IBL section

/// Read the current environment binding.  Returns nil only when there is
/// no scene / no active rasterizer; otherwise a fully-populated snapshot
/// (with `hasEnvironment == NO` when unbound).
- (nullable RISEEnvironmentInfo *)environmentInfo;

/// Set the environment intensity / background-visibility / rotation
/// (degrees).  Each applies live (viewport re-renders) AND persists (CST
/// mirror).  Returns NO when no editable bound environment exists.
- (BOOL)setEnvironmentScale:(double)scale;
- (BOOL)setEnvironmentBackground:(BOOL)background;
- (BOOL)setEnvironmentOrientX:(double)x y:(double)y z:(double)z
    NS_SWIFT_NAME(setEnvironmentOrient(x:y:z:));

/// Swap the bound environment painter's HDRI file (an existing path; the
/// file picker is the guard).  Returns NO when none is bound.
- (BOOL)setEnvironmentFile:(NSString *)absPath;

/// Create an environment from an HDRI file when none exists (inserts an
/// hdr/exr painter chosen from the extension + binds radiance_map).
/// Returns `applied`; `outName` / `outMessage` optional (may be NULL).
- (BOOL)addEnvironment:(NSString *)hdriPath
               outName:(NSString * _Nullable * _Nullable)outName
            outMessage:(NSString * _Nullable * _Nullable)outMessage
    NS_SWIFT_NAME(addEnvironment(_:outName:outMessage:));

/// Remove the environment (unbinds radiance_map, live + CST).  Returns
/// NO when no editable environment exists.
- (BOOL)removeEnvironment;

#pragma mark - Multi-camera

/// Clone the currently-active camera under a new name and switch
/// the scene to it. `proposedName` is canonicalized to a CST-safe identifier,
/// then deduplicated with a numeric suffix.
/// Returns the actual name registered, or `nil` on no-active-
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

#pragma mark - Agent surface (Facet 5 slice 1c-1: live in-app injection)

/// Facet 5 slice 1c-1: hand one JSON-RPC 2.0 request LINE to the live
/// agent dispatcher and return the JSON-RPC response line.  This is the
/// "agent + user co-edit" entry point: the same
/// `AgentRpcDispatcher::HandleLine` the CLI's `--agent-stdio` loop drives,
/// but here it runs IN-PROCESS over the SAME live Job + SceneEditController
/// this viewport bridge owns — so a typed `propose_patch` edits the running
/// scene and the viewport reflects it.
///
/// Runs SYNCHRONOUSLY on the calling thread.  The caller is Swift's
/// @MainActor, i.e. the main thread — exactly where the GUI's own
/// SetProperty drives its cancel-and-park edit, so no extra marshalling is
/// needed for this slice (a background thread / socket is 1c-2+).  The
/// dispatcher's commit routes through `SceneEditController::ApplyAgentParamEdit`
/// (attached at init), which cancel-and-parks the render thread and marks the
/// editor dirty, so the viewport re-render + the Save-button enable happen
/// automatically via the controller's existing kick + dirty-changed block —
/// the caller need NOT refresh the viewport or the Save state by hand.
///
/// Never returns nil: if the dispatcher is unavailable (init failed / the
/// bridge was shut down), a well-formed JSON-RPC -32603 error string is
/// returned so the Swift caller always parses a valid response.
///
/// Agent-autonomy-selector note (2026-07): this entry point ALWAYS runs at
/// Owner authority + Commit autonomy, regardless of `agentAutonomyLevel`
/// below.  It is the "administrative" path — `list_proposals`,
/// `resolve_proposal`, and the one-time `read_skill` index fetch all go
/// through it — and MUST stay that way: `resolve_proposal` is refused
/// outright under Propose/Read autonomy (see AgentRpc.h), so if this method
/// tracked the composer's level, setting the composer to Read/Propose would
/// silently disable the Owner's own "Approve"/"Reject" buttons on already-
/// staged proposals, which has nothing to do with what the CHAT AGENT is
/// permitted to do.  The chat driver's own tool calls go through
/// `-agentHandleToolCall:` instead — see that method's doc.
- (NSString *)agentHandleLine:(NSString *)jsonRpcRequest
    NS_SWIFT_NAME(agentHandleLine(_:));

#pragma mark - Agent autonomy selector (2026-07 GUI composer chips)

/// Mirrors RISE::Agent::AgentAutonomy (AgentRpc.h) plus the routing choice
/// RISEViewportBridge itself makes for `Propose` (see `agentAutonomyLevel`
/// below) — NOT a 1:1 re-export, since the C++ enum alone cannot express
/// "and which AgentSession authority backs it."
typedef NS_ENUM(NSInteger, RISEAgentAutonomyLevel) {
    RISEAgentAutonomyRead    = 0,   ///< read-safe verbs only; render allowed; every edit verb refused (kAutonomyRefused).
    RISEAgentAutonomyPropose = 1,   ///< edit verbs STAGE a proposal (External authority) instead of committing; the Owner reviews via the existing proposals panel (list_proposals/resolve_proposal, both still through `agentHandleLine:`).
    RISEAgentAutonomyApply   = 2,   ///< today's unrestricted behaviour: edit verbs commit directly (Owner authority, Commit autonomy). The default.
};

/// The chat composer's current autonomy level for its OWN tool calls (the
/// LLM-issued `propose_patch`/`insert_chunk`/`remove_chunk`/etc. driven by
/// `-agentHandleToolCall:`, NOT the administrative calls `-agentHandleLine:`
/// makes on its own — see that method's note).  Defaults to
/// `RISEAgentAutonomyApply` at bridge-attach time, matching every
/// pre-existing construction site's behaviour byte-for-byte until the
/// Swift composer explicitly sets a persisted choice (see ChatViewModel's
/// UserDefaults-backed `agentAutonomyLevel` — this property does NOT read
/// UserDefaults itself; the Swift layer applies the persisted value on
/// attach).  Setting an out-of-range value is a no-op (keeps the previous
/// level) rather than undefined behaviour.
@property (nonatomic) RISEAgentAutonomyLevel agentAutonomyLevel;

/// Hand one JSON-RPC 2.0 request line to whichever internal dispatcher
/// matches `agentAutonomyLevel` right now, and return the response line.
/// This is the entry point the chat driver's OWN tool-call execution uses
/// (ChatViewModel's `driveTurn`, for every non-`render` tool call) — the
/// verb-by-verb behaviour per level:
///   * Read    -> the 10-verb read-safe allowlist (IsReadSafeVerb in
///                AgentRpc.cpp) dispatches; `propose_patch`/`insert_chunk`/
///                `remove_chunk` (and any other verb) are REFUSED
///                (kAutonomyRefused, -32011) — this is Owner authority
///                under Read autonomy, so even the refusal path never
///                reaches ProposePatch/InsertChunk/RemoveChunk.
///   * Propose -> the read-safe allowlist dispatches AS BEFORE, but this
///                level runs over a SEPARATE, External-authority
///                AgentSession sharing the SAME live SceneEditController
///                `-agentHandleLine:`'s Owner session is attached to — so
///                `propose_patch`/`insert_chunk`/`remove_chunk` STAGE a
///                real proposal onto that controller's ONE queue (the
///                exact queue the existing proposals panel already reads
///                via `-agentHandleLine:`'s `list_proposals`/
///                `resolve_proposal`), rather than committing.
///                `resolve_proposal` itself is refused at THIS level (by
///                design — see AgentRpc.h's IsProposeSafeVerb doc); the
///                chat driver never calls it, only the Owner-authority
///                proposals-panel code path does, via `-agentHandleLine:`.
///   * Apply   -> byte-for-byte today's behaviour: routes to the SAME
///                Owner-authority, Commit-autonomy posture `-agentHandleLine:`
///                uses (a separate dispatcher INSTANCE over a separate
///                AgentSession, but identical authority+autonomy, so
///                observably indistinguishable).
///
/// A `render` tool call is deliberately NOT routed through this level
/// selector — `executeRenderToolCallAsync`'s submit/poll/cancel sequence
/// spans multiple `agentHandleLine`-shaped calls against ONE session's
/// per-job result cache (`AgentSession::mLastAsyncRenderResult`), and the
/// user can change `agentAutonomyLevel` mid-poll; routing those calls
/// through whichever dispatcher happens to be current at each poll tick
/// would risk polling a DIFFERENT session than the one that submitted the
/// job, missing its cached result.  `render` is read-safe at every level
/// anyway (see AgentRpc.h — it never mutates the CST document), so there is
/// no HONESTY cost to always running it over the stable Owner/Commit
/// session `-agentHandleLine:` already uses.
///
/// Same nil-safety contract as `-agentHandleLine:`: never returns nil.
- (NSString *)agentHandleToolCall:(NSString *)jsonRpcRequest
    NS_SWIFT_NAME(agentHandleToolCall(_:));

#pragma mark - Secure-MCP slice 5c: GUI-hosted external MCP endpoint

/// Start a LOOPBACK-ONLY MCP HTTP server (the same
/// RISE::Agent::AgentLoopbackHttpServer the headless `rise --agent-http`
/// CLI transport hosts) bound to THIS bridge's LIVE Job + live
/// SceneEditController, so a real external MCP client (Claude Code, or
/// any other MCP host) can connect and PROPOSE edits into the scene
/// that's actually open in this window -- the counterpart to the
/// in-process Owner dispatcher `agentHandleLine` already drives.
///
/// AUTHORITY / AUTONOMY: constructs a NEW, SEPARATE AgentSession over
/// the SAME Job this bridge wraps (WrapJob), AttachController's it to
/// this bridge's live `_controller` (so a staged proposal lands on the
/// SAME queue the Owner dispatcher's ListProposals/ResolveProposal
/// verbs read), sets its authority to External (mutating verbs STAGE,
/// never commit), labels it via SetSessionLabel (see `sessionLabel`
/// below), and wraps it in an AgentMcpAdapter constructed with
/// AgentAutonomy::Propose (the wire-layer posture that pairs with
/// External authority -- see AgentRpc.h's file header). This is a
/// SEPARATE AgentRpcDispatcher instance from the one -agentHandleLine
/// drives (that one is Owner-authority + Commit-autonomy, constructed
/// at init time over its OWN AgentSession) -- see the .mm's
/// "two-dispatcher-one-controller" doc comment for the full threading
/// argument for why two independent dispatcher instances sharing one
/// controller is safe.
///
/// THREADING: the server's serial accept-handle loop runs on a
/// dedicated background thread this method spawns (std::thread,
/// detached-by-ownership -- joined by -stopAgentHostedServer or
/// -shutdown, never detached-and-abandoned). Every request that
/// thread handles calls HandleLine on the EXTERNAL dispatcher, which
/// (for a mutating verb) calls SceneEditController::StageProposal --
/// guarded by the controller's OWN mMutex, the SAME lock the render
/// thread and every OTHER controller entry point (SetProperty,
/// ApplyAgentParamEdit, the Owner dispatcher's own calls) already
/// serialize on. list_proposals / resolve_proposal called from the
/// GUI's Owner dispatcher (via -agentHandleLine, always the main
/// thread) and a concurrent external stage are therefore safe by the
/// SAME pre-existing mutex discipline 5a/5b already proved -- nothing
/// new is introduced here beyond a second caller thread.
///
/// IDEMPOTENT: calling this while a server is already running returns
/// `ok=false` with an explanatory message and does NOT start a second
/// server (a fresh call after -stopAgentHostedServer starts a new one
/// with a FRESH per-launch token, per AgentLoopbackHttpServer's own
/// per-construction-token contract).
///
/// `sessionLabel` is stamped onto the External AgentSession via
/// SetSessionLabel BEFORE Bind() -- so every proposal any external
/// client stages through this server carries it (see
/// SceneEditController::AgentProposal::sessionLabel's doc). Pass ""
/// for the generic default; the Swift caller passes a fixed
/// "external-mcp" label today (see StartAgentHostedServer's Swift-side
/// caller for why a per-connection id isn't threaded further: the
/// server is single-adapter-instance, not per-connection-session).
///
/// Returns immediately (Bind() is synchronous and fast; Serve() runs
/// on the spawned thread) -- never blocks for a client connection.
- (RISEAgentHostedServerInfo *)startAgentHostedServerWithLabel:(NSString *)sessionLabel
    NS_SWIFT_NAME(startAgentHostedServer(sessionLabel:));

/// Stop the hosted server started by -startAgentHostedServerWithLabel:
/// (a no-op, not an error, if none is running). Signals the server's
/// Stop() (unblocks the accept-loop thread's accept() call) and JOINS
/// that thread before returning -- so by the time this method returns,
/// no external-dispatcher HandleLine call can still be in flight and
/// it is safe to proceed to tear down the controller/Job (see -shutdown,
/// which calls this FIRST, before the agent dispatcher / controller
/// teardown, for exactly that reason).
- (void)stopAgentHostedServer;

/// True iff a hosted server is currently running (bound + serving).
@property (nonatomic, readonly) BOOL isAgentHostedServerRunning;

/// The bound port / bearer token of the CURRENTLY RUNNING hosted
/// server (0 / "" when not running) -- lets the Swift settings panel
/// re-display these after e.g. a view re-render without re-starting
/// the server.
@property (nonatomic, readonly) NSUInteger agentHostedServerPort;
@property (nonatomic, readonly, copy) NSString *agentHostedServerToken;

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

/// One navigation axis-ball nub from the controller's layout.  Positions are
/// in the widget space the overlay passed to `refreshNavGizmo…` (and the same
/// space it feeds `navGizmoNubAt…`).
@interface RISEViewportNavNub : NSObject
@property (nonatomic, readonly) NSInteger axis;       ///< 0=X, 1=Y, 2=Z
@property (nonatomic, readonly) BOOL negative;        ///< NO=+axis, YES=−axis
@property (nonatomic, readonly) CGFloat screenX;
@property (nonatomic, readonly) CGFloat screenY;
@property (nonatomic, readonly) CGFloat screenRadius;
@property (nonatomic, readonly) BOOL facing;          ///< YES=toward viewer (bright)
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
