//////////////////////////////////////////////////////////////////////
//
//  RISEViewportBridge.mm - Obj-C++ implementation that wraps the
//    C++ SceneEditController via the RISE_API_SceneEditController_*
//    C entry points.
//
//    Phase 5: live-preview wiring.  When the host RISEBridge has a
//    loaded scene, we construct an InteractivePelRasterizer alongside
//    a viewport-targeted
//    IRasterizerOutput sink that converts each finished tile into
//    an NSImage and posts it to a Swift-supplied block on the main
//    thread.  This is what makes the SwiftUI viewport actually
//    update as the user drags.
//
//////////////////////////////////////////////////////////////////////

#import "RISEViewportBridge.h"
#import "RISEBridge.h"

#include "RISE_API.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/IRasterizerOutput.h"
#include "Interfaces/IRasterImage.h"
#include "Utilities/Reference.h"
#include "SceneEditor/SceneEditController.h"
#include "Rendering/InteractivePelRasterizer.h"
#include "Rendering/ViewportFrameStore.h"

#include <atomic>
#include <vector>
#include <cstring>

using namespace RISE;

// Class extension: private dirty-changed trampoline target on
// RISEViewportBridge.  The C trampoline can't message a method that
// isn't visible to the compiler, so we hoist this selector into
// scope here.
@interface RISEViewportBridge ()
- (void)_fireDirtyChangedFromBackground:(BOOL)hasUnsavedChanges;
@end

// Class extension: private initializer for RISEViewportProperty.
@interface RISEViewportProperty ()
- (instancetype)initWithName:(NSString *)name
                       value:(NSString *)value
                  describing:(NSString *)describing
                        kind:(NSInteger)kind
                    editable:(BOOL)editable
                     presets:(NSArray<RISEViewportPropertyPreset *> *)presets
                   unitLabel:(NSString *)unitLabel;
@end

// Class extension: private initializer for RISEViewportPropertyPreset.
@interface RISEViewportPropertyPreset ()
- (instancetype)initWithLabel:(NSString *)label value:(NSString *)value;
@end

// Class extension: private initialiser for RISEViewportGizmoHandle —
// needed by the `gizmoHandles` accessor on RISEViewportBridge which
// builds the snapshot array.  The implementation lives at the bottom
// of this file; this extension hoists the selector into scope for
// the accessor.
@interface RISEViewportGizmoHandle ()
- (instancetype)_initWithKind:(RISEViewportGizmoKind)kind
                         axis:(NSInteger)axis
                      screenX:(CGFloat)screenX
                      screenY:(CGFloat)screenY
                 screenRadius:(CGFloat)screenRadius;
@end

namespace {

// ============================================================
// IRasterizerOutput sink for the interactive viewport.
//
// Priority: get pixels to the screen as fast as possible.  We
// deliberately ignore the per-tile OutputIntermediateImage
// callbacks (which produce the visually distracting "blocks
// fill in one by one" effect) and only dispatch the *final*
// frame to SwiftUI when the rasterizer hits FlushToOutputs at
// end-of-pass.  The cancel-restart loop fires a new RasterizeScene
// call on every edit, so the user sees the freshest finished
// frame appear whole — not a half-rendered image with tile
// boundaries.
//
// Suppress-next: lets the bridge skip exactly one dispatch.
// Used right after a production render returns, so the
// production image stays on screen until the user actually
// starts interacting in the viewport.  Beyond that, we do NOT
// throttle: every frame the rasterizer produces reaches the
// screen, including partial buffers from cancelled passes.
// During fast manipulation the cancel flag trips on every
// pointer move, so dropping cancelled frames would mean the
// user only ever sees post-pause refinement passes — visually
// indistinguishable from the viewport being throttled.  Center-
// out tile order keeps partial buffers usable (centre-of-image
// fills first).
// ============================================================
class ViewportPreviewSink :
    public IRasterizerOutput,
    public Implementation::Reference
{
public:
    ViewportPreviewSink()
    : mBlock( nil )
    , mController( nullptr )
    , mFanoutVFS( nullptr )
    , mSuppressNext( false )
    {}

    virtual ~ViewportPreviewSink() {
        // _block is __strong; nilling on dealloc lets ARC release it.
        mBlock = nil;
        // L5a round-4 — release our addref on the fan-out VFS.
        if( mFanoutVFS ) {
            mFanoutVFS->release();
            mFanoutVFS = nullptr;
        }
    }

    void SetBlock( RISEViewportImageBlock block ) {
        mBlock = [block copy];
    }

    // Borrowed; the bridge keeps the controller alive for the sink's
    // lifetime.  Used to query IsCancelRequested at end-of-pass.
    void SetController( SceneEditController* c ) { mController = c; }

    // L5a round-4 — fan-out target for EDR.  When set, every
    // OutputImage call ALSO drives `vfs->OutputImage(pImage, ...)`,
    // which in turn fires the bridge's HDR/LDR observer block.
    // SceneEditController calls `mInteractiveRasterizer->FreeRasterizer-
    // Outputs()` before every render pass and re-attaches ONLY this
    // sink (SceneEditController.cpp:1630-1631), so attaching VFS
    // directly to the rasterizer doesn't work — the controller
    // clobbers it.  Fanning out from inside this sink survives the
    // clobber because we own the VFS reference here.  Addref'd on
    // SetFanoutVFS, released on dtor.
    //
    // L6e-3 — `OnRasterizerFrameStoreChanged` (below) now forwards
    // the rasterizer's per-pass FrameStore to the VFS's
    // `BindFrameStore` so the VFS observes the canonical store
    // directly.  `OutputImage`'s fan-out call to `vfs->OutputImage`
    // is then a no-op (per L6f's bound-mode short-circuit) — the
    // frame-complete event flows from rasterizer's
    // `MarkFrameComplete` (post-L6f) directly through the VFS
    // observer chain.  We keep the fan-out call for safety (if
    // SceneEditController ever skips the FrameStore push, the
    // legacy fan-out covers).
    void SetFanoutVFS( Implementation::ViewportFrameStore* vfs ) {
        if( mFanoutVFS == vfs ) return;
        if( vfs ) vfs->addref();
        if( mFanoutVFS ) mFanoutVFS->release();
        mFanoutVFS = vfs;
    }

    // L6e-3 — Forward rasterizer FrameStore swaps to the fan-out VFS
    // so it auto-binds (post-L6e-2a `BindFrameStore`).  Fires every
    // time SceneEditController's per-pass `EnsureInteractiveFrameStore_`
    // calls `Rasterizer::SetFrameStore` (which dispatches on every
    // attached IRasterizerOutput in the rasterizer's outs).
    void OnRasterizerFrameStoreChanged( Implementation::FrameStore* framestore ) override {
        if( mFanoutVFS ) {
            mFanoutVFS->BindFrameStore( framestore );
        }
    }

    // Set true to drop the very next OutputImage call.  Auto-clears
    // after one drop.  Atomic so the bridge can call this from the
    // UI thread while the render thread fires OutputImage from a
    // worker thread.
    void SuppressNextFrame() { mSuppressNext.store( true ); }

    // Per-tile callback fires many times per render pass — and
    // each fire would draw red tile-corner toggles (DrawToggles)
    // into the IRasterImage before pixels are written.  Legacy
    // NSImage path ignores per-tile fires entirely.  L5a round-5:
    // we ALSO suppress fan-out to the interactive VFS at this
    // level — interactive's frame-complete-only observer is what
    // drives the Metal layer.  The interactive VFS's tile-callback
    // slot is intentionally left unbound (see
    // `-ensureInteractiveVFSCreated` in RISEBridge.mm), so even
    // though VFS::OutputIntermediateImage processes tile pixels
    // into the FrameStore, no observer fires → no Metal-layer
    // present per tile → no red flash visible to the user.
    // Round-4 fanned this and produced the red flashing the user
    // reported; round-5 reverts that.
    void OutputIntermediateImage( const IRasterImage& /*pImage*/,
                                  const RISE::Rect* /*pRegion*/ ) override {
        // intentionally empty
    }

    // End-of-pass callback (FlushToOutputs / FlushDenoisedToOutputs).
    // This is the only place we actually push pixels to SwiftUI.
    //
    // Every dispatch from the rasterizer reaches the screen.  In
    // particular we do NOT drop cancelled-mid-pass frames: during
    // fast manipulation the cancel flag trips on every pointer move,
    // and dropping the resulting partial buffers means the user only
    // sees post-pause refinement frames — which feels like the
    // viewport is throttled.  The user explicitly wants every
    // produced frame on-screen, even if it's a partial buffer with
    // only the centre tiles filled (CenterOut tile order makes
    // partials usable).  The one-shot suppress is kept because it
    // serves a distinct purpose (preserving the production image
    // until the user starts dragging).
    void OutputImage( const IRasterImage& pImage,
                      const RISE::Rect* pRegion,
                      const unsigned int frame ) override {
        if( mSuppressNext.exchange( false ) ) {
            // One-shot suppression (post-production) — skip exactly
            // this dispatch.  The next render's frame goes through.
            return;
        }
        // L5a round-4 — fan into VFS first so EDR mode gets the
        // frame-complete observer fire (which drives the Metal
        // layer present).  Then run the legacy NSImage path.
        if( mFanoutVFS ) {
            mFanoutVFS->OutputImage( pImage, pRegion, frame );
        }
        BlitWholeAndDispatch( pImage );
    }

private:
    __strong RISEViewportImageBlock                mBlock;
    SceneEditController*                            mController;   // borrowed
    Implementation::ViewportFrameStore*             mFanoutVFS;    // strong (addref'd in SetFanoutVFS)
    std::atomic<bool>                               mSuppressNext;

    static unsigned char Clamp8( double v ) {
        if( v <= 0.0 ) return 0;
        if( v >= 1.0 ) return 255;
        return static_cast<unsigned char>( v * 255.0 + 0.5 );
    }

    void BlitWholeAndDispatch( const IRasterImage& img ) {
        RISEViewportImageBlock block = mBlock;
        if( !block ) return;

        const unsigned int W = img.GetWidth();
        const unsigned int H = img.GetHeight();
        if( W == 0 || H == 0 ) return;

        // Allocate the NSBitmapImageRep first and write pixels directly
        // into its owned bitmapData buffer.  The previous implementation
        // staged pixels through a per-frame std::vector<uint8_t> and then
        // memcpy'd into the rep — that's two W*H*4 allocations and a
        // memcpy per frame.  At a 30Hz preview cadence with a 1200x800
        // viewport, the staged-vector path was churning ~115 MB/sec
        // through the macOS xzone allocator, fragmenting it within a few
        // minutes and crashing inside `xzm_segment_group_alloc_chunk`
        // (see post-mortem in BVH_CLEANUP_AND_NEXT.md → Tier A follow-ups).
        //
        // Cutting the intermediate buffer halves the per-frame allocation
        // pressure with no thread-safety implication: the rep itself is
        // still allocated fresh per frame because the previous frame's
        // dispatch_async block keeps its NSImage (and therefore its rep)
        // alive on the main queue, so reusing the rep would race with
        // SwiftUI's display upload of the previous frame.  A future
        // optimisation would pool a small ring of reps to eliminate even
        // that allocation.
        @autoreleasepool {
            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
                initWithBitmapDataPlanes:NULL
                              pixelsWide:W
                              pixelsHigh:H
                           bitsPerSample:8
                         samplesPerPixel:4
                                hasAlpha:YES
                                isPlanar:NO
                          colorSpaceName:NSDeviceRGBColorSpace
                             bytesPerRow:W * 4
                            bitsPerPixel:32];
            if( !rep || !rep.bitmapData ) {
                // Allocation failure path: skip the dispatch.  SwiftUI
                // will see the prior frame until the next render.  Better
                // than crashing on a downstream nullptr write.
                return;
            }

            // Single-pass full-image read.  GetPEL is virtual per-pixel,
            // but we only do it once per render pass now (instead of once
            // per tile, which was thousands of times).
            unsigned char* p = rep.bitmapData;
            for( unsigned int y = 0; y < H; ++y ) {
                for( unsigned int x = 0; x < W; ++x ) {
                    RISEColor c = img.GetPEL( x, y );
                    *p++ = Clamp8( c.base.r );
                    *p++ = Clamp8( c.base.g );
                    *p++ = Clamp8( c.base.b );
                    *p++ = 255;
                }
            }

            NSImage* nsImg = [[NSImage alloc] initWithSize:NSMakeSize(W, H)];
            [nsImg addRepresentation:rep];

            dispatch_async( dispatch_get_main_queue(), ^{
                block( nsImg );
            });
        }
    }
};

}  // namespace

@implementation RISEViewportBridge {
    SceneEditController* _controller;
    RISEBridge*          _host;          // strong: we share its scene
    BOOL                 _ownsRunning;

    // Live-preview state (Phase 5).  Borrowed by the controller; we
    // own the addref/release pair via Implementation::Reference.
    IRayCaster*          _caster;          // preview caster, max-recursion 1
    IRayCaster*          _polishCaster;    // polish caster, max-recursion 2
                                           // (one bounce of glossy / refl / refr)
    IRasterizer*         _interactiveRasterizer;
    ViewportPreviewSink* _previewSink;

    // Phase 6.5: GUI-supplied callback fired on dirty-state
    // TRANSITIONS only.  Strong-held so the C trampoline can copy-
    // capture our `self` ptr and invoke it.  Cleared (and the
    // controller's listener detached) before _controller destruction
    // in -shutdown so a stale fire after dealloc is impossible.
    RISEViewportDirtyChangedBlock _dirtyChangedBlock;
}

- (nullable instancetype)initWithHostBridge:(RISEBridge *)host {
    self = [super init];
    if (!self) return nil;

    _host = host;
    _ownsRunning = NO;
    _caster = nullptr;
    _polishCaster = nullptr;
    _interactiveRasterizer = nullptr;
    _previewSink = nullptr;

    void* jobOpaque = [host opaqueJobHandle];
    if (!jobOpaque) {
        return nil;
    }
    IJobPriv* pJob = static_cast<IJobPriv*>(jobOpaque);

    // Try to build the live-preview rasterizer.  This is best-effort:
    // if setup fails, edits still work and the user can click Render
    // to see the production result.
    [self tryBuildLivePreviewForJob:pJob];

    if (!RISE_API_CreateSceneEditController(pJob, _interactiveRasterizer, &_controller)) {
        _controller = nullptr;
        [self releaseLivePreview];
        return nil;
    }

    if (_previewSink) {
        // The sink queries the controller's cancel state at end-of-pass
        // to decide whether to drop a stale dispatch.  Wire the pointer
        // before installing the sink as a rasterizer output.
        _previewSink->SetController(_controller);
        RISE_API_SceneEditController_SetPreviewSink(_controller, _previewSink);
    }

    return self;
}

- (void)tryBuildLivePreviewForJob:(IJobPriv*)pJob {
    (void)pJob;

    IRasterizer* interactive = nullptr;
    IRayCaster* pCaster = nullptr;
    IRayCaster* pPolishCaster = nullptr;
    if (!Implementation::CreateInteractiveMaterialPreviewPipeline(
            &interactive, &pCaster, &pPolishCaster))
    {
        return;
    }

    _caster = pCaster;
    _polishCaster = pPolishCaster;
    _interactiveRasterizer = interactive;

    _previewSink = new ViewportPreviewSink();
    _previewSink->addref();

    // L5a round-4 — fan the preview sink's OutputImage /
    // OutputIntermediateImage calls into the host bridge's VFS so
    // progressive interactive renders also drive the EDR / HDR /
    // LDR observer block.  Round-3 tried attaching VFS directly to
    // `_interactiveRasterizer`, but `SceneEditController::DoOnePass`
    // calls `FreeRasterizerOutputs()` before re-adding only its
    // own preview sink each pass (SceneEditController.cpp:1630-1631),
    // so any external rasterizer-attach is dropped.  Fanning from
    // inside the sink survives the clobber because the sink owns
    // the VFS reference itself.  Both paths fire from every pass:
    // the legacy NSImage block (ViewportPreviewSink → Swift image
    // binding) AND the VFS observer (HDR block → Metal layer).
    void* opaqueVFS = [_host opaqueInteractiveViewportFrameStore];
    if (opaqueVFS) {
        _previewSink->SetFanoutVFS(
            static_cast<Implementation::ViewportFrameStore*>(opaqueVFS));
    }
}

- (void)releaseLivePreview {
    if (_previewSink) {
        _previewSink->release();
        _previewSink = nullptr;
    }
    if (_interactiveRasterizer) {
        _interactiveRasterizer->release();
        _interactiveRasterizer = nullptr;
    }
    if (_polishCaster) {
        _polishCaster->release();
        _polishCaster = nullptr;
    }
    if (_caster) {
        _caster->release();
        _caster = nullptr;
    }
}

- (void)dealloc {
    [self shutdown];
}

- (void)shutdown {
    if (_controller) {
        // Detach the dirty-changed listener BEFORE destroying the
        // controller so its captured `self` pointer can't fire into
        // a half-dealloc'd instance.  Pair-balances with the
        // setDirtyChangedBlock attach in `[setDirtyChangedBlock:]`.
        RISE_API_SceneEditController_SetDirtyChangedCallback(
            _controller, nullptr, nullptr);
        RISE_API_DestroySceneEditController(_controller);
        _controller = nullptr;
    }
    _dirtyChangedBlock = nil;
    [self releaseLivePreview];
}

#pragma mark - Lifecycle

- (void)start {
    if (!_controller) return;
    RISE_API_SceneEditController_Start(_controller);
    _ownsRunning = YES;
}

- (void)stop {
    if (!_controller) return;
    RISE_API_SceneEditController_Stop(_controller);
    _ownsRunning = NO;
}

- (void)scaleFilmToFitSurfaceW:(NSUInteger)surfaceW
                       surfaceH:(NSUInteger)surfaceH
                    maxLongEdge:(NSUInteger)maxLongEdge {
    if (!_host) return;
    if (surfaceW == 0 || surfaceH == 0 || maxLongEdge == 0) return;
    void* jobOpaque = [_host opaqueJobHandle];
    if (!jobOpaque) return;
    IJobPriv* pJob = static_cast<IJobPriv*>(jobOpaque);
    pJob->ScaleFilmToFit(
        static_cast<unsigned int>(surfaceW),
        static_cast<unsigned int>(surfaceH),
        static_cast<unsigned int>(maxLongEdge));
}

- (BOOL)isRunning {
    return _ownsRunning;
}

#pragma mark - Toolbar

- (RISEViewportTool)currentTool {
    if (!_controller) return RISEViewportToolSelect;
    const int t = RISE_API_SceneEditController_CurrentTool(_controller);
    // The controller's int values match RISEViewportTool 1:1.
    return static_cast<RISEViewportTool>(t);
}

- (void)setCurrentTool:(RISEViewportTool)tool {
    if (!_controller) return;
    RISE_API_SceneEditController_SetTool(_controller, static_cast<int>(tool));
}

+ (RISEViewportToolCategory)categoryForTool:(RISEViewportTool)tool {
    const int c = RISE_API_SceneEditController_CategoryForTool(static_cast<int>(tool));
    return static_cast<RISEViewportToolCategory>(c);
}

+ (RISEViewportTool)defaultSubToolForCategory:(RISEViewportToolCategory)category {
    const int t = RISE_API_SceneEditController_DefaultSubToolForCategory(static_cast<int>(category));
    return static_cast<RISEViewportTool>(t);
}

- (RISEViewportTool)lastSubToolForCategory:(RISEViewportToolCategory)category {
    if (!_controller) {
        return [RISEViewportBridge defaultSubToolForCategory:category];
    }
    const int t = RISE_API_SceneEditController_GetLastSubToolForCategory(
        _controller, static_cast<int>(category));
    return static_cast<RISEViewportTool>(t);
}

#pragma mark - Gizmo overlay

- (void)refreshGizmoHandles {
    if (!_controller) return;
    RISE_API_SceneEditController_RefreshGizmoHandles(_controller);
}

- (NSArray<RISEViewportGizmoHandle *> *)gizmoHandles {
    if (!_controller) return @[];
    const unsigned int n = RISE_API_SceneEditController_GizmoHandleCount(_controller);
    NSMutableArray<RISEViewportGizmoHandle *> *out = [NSMutableArray arrayWithCapacity:n];
    for (unsigned int i = 0; i < n; ++i) {
        int kind = 0;
        int axis = 0;
        double x = 0, y = 0, r = 0;
        if (!RISE_API_SceneEditController_GizmoHandle(
                _controller, i, &kind, &axis, &x, &y, &r)) {
            continue;
        }
        RISEViewportGizmoHandle *h = [[RISEViewportGizmoHandle alloc]
            _initWithKind:static_cast<RISEViewportGizmoKind>(kind)
                     axis:static_cast<NSInteger>(axis)
                  screenX:static_cast<CGFloat>(x)
                  screenY:static_cast<CGFloat>(y)
             screenRadius:static_cast<CGFloat>(r)];
        [out addObject:h];
    }
    return out;
}

- (BOOL)gizmoDragActive {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_IsGizmoDragActive(_controller) ? YES : NO;
}

- (RISEViewportGizmoKind)activeGizmoKind {
    if (!_controller) return RISEViewportGizmoKindAxisArrow;  // sentinel via axis = -1 also unused
    const int k = RISE_API_SceneEditController_ActiveGizmoKind(_controller);
    if (k < 0) return RISEViewportGizmoKindAxisArrow;
    return static_cast<RISEViewportGizmoKind>(k);
}

- (NSInteger)activeGizmoAxis {
    if (!_controller) return -1;
    return RISE_API_SceneEditController_ActiveGizmoAxis(_controller);
}

#pragma mark - Pointer events

- (void)pointerDownX:(double)x y:(double)y {
    if (!_controller) return;
    RISE_API_SceneEditController_OnPointerDown(_controller, x, y);
}

- (void)pointerMoveX:(double)x y:(double)y {
    if (!_controller) return;
    RISE_API_SceneEditController_OnPointerMove(_controller, x, y);
}

- (void)pointerUpX:(double)x y:(double)y {
    if (!_controller) return;
    RISE_API_SceneEditController_OnPointerUp(_controller, x, y);
}

- (NSSize)cameraSurfaceDimensions {
    if (!_controller) return NSMakeSize(0, 0);
    unsigned int w = 0, h = 0;
    if (!RISE_API_SceneEditController_GetCameraDimensions(_controller, &w, &h)) {
        return NSMakeSize(0, 0);
    }
    return NSMakeSize(static_cast<CGFloat>(w), static_cast<CGFloat>(h));
}

- (double)animationTimeStart {
    if (!_controller) return 0;
    double t0 = 0, t1 = 0; unsigned int nf = 0;
    if (!RISE_API_SceneEditController_GetAnimationOptions(_controller, &t0, &t1, &nf)) return 0;
    return t0;
}

- (double)animationTimeEnd {
    if (!_controller) return 0;
    double t0 = 0, t1 = 0; unsigned int nf = 0;
    if (!RISE_API_SceneEditController_GetAnimationOptions(_controller, &t0, &t1, &nf)) return 0;
    return t1;
}

- (NSUInteger)animationNumFrames {
    if (!_controller) return 0;
    double t0 = 0, t1 = 0; unsigned int nf = 0;
    if (!RISE_API_SceneEditController_GetAnimationOptions(_controller, &t0, &t1, &nf)) return 0;
    return static_cast<NSUInteger>(nf);
}

#pragma mark - Time scrubber

- (void)scrubTimeBegin {
    if (!_controller) return;
    RISE_API_SceneEditController_OnTimeScrubBegin(_controller);
}

- (void)scrubTime:(double)t {
    if (!_controller) return;
    RISE_API_SceneEditController_OnTimeScrub(_controller, t);
}

- (void)scrubTimeEnd {
    if (!_controller) return;
    RISE_API_SceneEditController_OnTimeScrubEnd(_controller);
}

- (void)beginPropertyScrub {
    if (!_controller) return;
    RISE_API_SceneEditController_BeginPropertyScrub(_controller);
}

- (void)endPropertyScrub {
    if (!_controller) return;
    RISE_API_SceneEditController_EndPropertyScrub(_controller);
}

#pragma mark - Edit commands

- (void)undo {
    if (!_controller) return;
    RISE_API_SceneEditController_Undo(_controller);
}

- (void)redo {
    if (!_controller) return;
    RISE_API_SceneEditController_Redo(_controller);
}

#pragma mark - Scene-file save (Phase 6.5)

- (BOOL)hasUnsavedSceneChanges {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_HasUnsavedChanges(_controller) ? YES : NO;
}

- (NSInteger)saveSceneTo:(NSString *)path
            errorMessage:(NSString * _Nullable * _Nullable)outErrorMessage {
    if (!_controller || !path) {
        if (outErrorMessage) *outErrorMessage = @"no scene loaded";
        return 3;  // Failed
    }
    char errBuf[1024] = {0};
    const int status = RISE_API_SceneEditController_RequestSave(
        _controller,
        [path UTF8String],
        errBuf,
        sizeof(errBuf));
    if (outErrorMessage) {
        *outErrorMessage = (errBuf[0] != '\0')
            ? [NSString stringWithUTF8String:errBuf]
            : nil;
    }
    return (NSInteger)status;
}

// C trampoline used by the controller's std::function listener.
// userData is the (__bridge) RISEViewportBridge* — we don't retain
// it here (the bridge outlives the controller, see -shutdown ordering),
// so a __bridge cast that's lifetime-loose is correct.
static void RISE_API_DirtyChangedTrampoline(void* userData,
                                            int hasUnsavedChanges) {
    if (!userData) return;
    RISEViewportBridge* bridge = (__bridge RISEViewportBridge*)userData;
    [bridge _fireDirtyChangedFromBackground:(hasUnsavedChanges != 0)];
}

- (void)_fireDirtyChangedFromBackground:(BOOL)hasUnsavedChanges {
    // The listener runs on whatever thread drove the edit — for
    // Apply/Undo/Redo it's the UI thread (Swift drives the controller
    // from the main actor), but RequestSave's success path fires
    // from inside RequestSave which can be called off-main in tests.
    // Hop to main so SwiftUI consumers don't have to.
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_dirtyChangedBlock) {
            self->_dirtyChangedBlock(hasUnsavedChanges);
        }
    });
}

- (void)setDirtyChangedBlock:(RISEViewportDirtyChangedBlock)block {
    _dirtyChangedBlock = [block copy];
    if (!_controller) return;
    if (block) {
        RISE_API_SceneEditController_SetDirtyChangedCallback(
            _controller,
            &RISE_API_DirtyChangedTrampoline,
            (__bridge void*)self);
    } else {
        RISE_API_SceneEditController_SetDirtyChangedCallback(
            _controller, nullptr, nullptr);
    }
}

- (double)lastSceneTime {
    if (!_controller) return 0.0;
    double t = 0.0;
    RISE_API_SceneEditController_LastSceneTime(_controller, &t);
    return t;
}

- (BOOL)requestProductionRender {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_RequestProductionRender(_controller);
}

#pragma mark - Selection

- (void)setSelectedObjectName:(NSString *)name {
    (void)name;
}

- (NSString *)selectedObjectName {
    return nil;
}

#pragma mark - Live preview output

- (void)setImageBlock:(RISEViewportImageBlock)block {
    if (_previewSink) {
        _previewSink->SetBlock(block);
    }
}

- (BOOL)hasLivePreview {
    return _interactiveRasterizer != nullptr;
}

- (void)suppressNextFrame {
    if (_previewSink) {
        _previewSink->SuppressNextFrame();
    }
}

#pragma mark - Properties panel

- (RISEViewportPanelMode)panelMode {
    if (!_controller) return RISEViewportPanelModeNone;
    const int m = RISE_API_SceneEditController_PanelMode(_controller);
    switch (m) {
        case 1: return RISEViewportPanelModeCamera;
        case 2: return RISEViewportPanelModeRasterizer;
        case 3: return RISEViewportPanelModeObject;
        case 4: return RISEViewportPanelModeLight;
        case 5: return RISEViewportPanelModeFilm;
        case 6: return RISEViewportPanelModeMaterial;
        case 7: return RISEViewportPanelModeMedium;
        default: return RISEViewportPanelModeNone;
    }
}

- (NSArray<NSString *> *)categoryEntities:(RISEViewportCategory)category {
    if (!_controller) return @[];
    const int catInt = static_cast<int>(category);
    const unsigned int n = RISE_API_SceneEditController_CategoryEntityCount(_controller, catInt);
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:n];
    char nameBuf[128];
    for (unsigned int i = 0; i < n; ++i) {
        if (RISE_API_SceneEditController_CategoryEntityName(_controller, catInt, i, nameBuf, sizeof(nameBuf))) {
            NSString *s = [NSString stringWithUTF8String:nameBuf];
            if (s) [out addObject:s];
        }
    }
    return out;
}

- (NSString *)activeNameForCategory:(RISEViewportCategory)category {
    if (!_controller) return @"";
    const int catInt = static_cast<int>(category);
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_CategoryActiveName(_controller, catInt, buf, sizeof(buf))) {
        return @"";
    }
    NSString *s = [NSString stringWithUTF8String:buf];
    return s ?: @"";
}

- (RISEViewportCategory)selectionCategory {
    if (!_controller) return RISEViewportCategoryNone;
    const int c = RISE_API_SceneEditController_GetSelectionCategory(_controller);
    switch (c) {
        case 1: return RISEViewportCategoryCamera;
        case 2: return RISEViewportCategoryRasterizer;
        case 3: return RISEViewportCategoryObject;
        case 4: return RISEViewportCategoryLight;
        case 5: return RISEViewportCategoryFilm;
        case 6: return RISEViewportCategoryMaterial;
        case 7: return RISEViewportCategoryMedium;
        default: return RISEViewportCategoryNone;
    }
}

- (NSString *)selectionName {
    if (!_controller) return @"";
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_GetSelectionName(_controller, buf, sizeof(buf))) {
        return @"";
    }
    NSString *s = [NSString stringWithUTF8String:buf];
    return s ?: @"";
}

- (BOOL)setSelectionCategory:(RISEViewportCategory)category name:(NSString *)name {
    if (!_controller) return NO;
    const char* utf8 = name ? [name UTF8String] : "";
    return RISE_API_SceneEditController_SetSelection(
        _controller,
        static_cast<int>(category),
        utf8) ? YES : NO;
}

- (NSUInteger)sceneEpoch {
    if (!_controller) return 0;
    return static_cast<NSUInteger>( RISE_API_SceneEditController_SceneEpoch(_controller) );
}

- (nullable NSString *)addCameraFromActive:(NSString *)proposedName {
    if (!_controller) return nil;
    const char* utf8 = proposedName ? [proposedName UTF8String] : "";
    // 256 bytes covers any realistic camera name; the controller-side
    // dedup loop won't produce strings longer than ~base+10 chars
    // before falling back to a timestamp suffix.
    char outName[256] = {0};
    if (!RISE_API_SceneEditController_AddCameraFromActive(
            _controller, utf8, outName, sizeof(outName))) {
        return nil;
    }
    return [NSString stringWithUTF8String:outName];
}

- (NSString *)panelHeader {
    if (!_controller) return @"";
    char buf[256] = {0};
    if (!RISE_API_SceneEditController_PanelHeader(_controller, buf, sizeof(buf))) {
        return @"";
    }
    return [NSString stringWithUTF8String:buf];
}

- (void)refreshProperties {
    if (!_controller) return;
    RISE_API_SceneEditController_RefreshProperties(_controller);
}

- (NSArray<RISEViewportProperty *> *)propertySnapshot {
    if (!_controller) return @[];
    const unsigned int n = RISE_API_SceneEditController_PropertyCount(_controller);
    NSMutableArray<RISEViewportProperty *> *out = [NSMutableArray arrayWithCapacity:n];
    char nameBuf[128];
    char valBuf[256];
    char descBuf[512];
    // Preset label/value buffers are sized generously so future
    // descriptors with longer labels (multi-byte UTF-8 for non-ASCII
    // names) can grow without churn.  CopyToBuf NUL-terminates on
    // truncation, but a truncation that lands mid-UTF-8-codepoint
    // would yield invalid bytes; defensive `[NSString stringWithUTF8String:]`
    // returns nil in that case (we skip the entry rather than crash).
    char presetLabelBuf[256];
    char presetValueBuf[256];
    char unitLabelBuf[64];
    for (unsigned int i = 0; i < n; ++i) {
        RISE_API_SceneEditController_PropertyName(_controller, i, nameBuf, sizeof(nameBuf));
        RISE_API_SceneEditController_PropertyValue(_controller, i, valBuf, sizeof(valBuf));
        RISE_API_SceneEditController_PropertyDescription(_controller, i, descBuf, sizeof(descBuf));
        const int kind = RISE_API_SceneEditController_PropertyKind(_controller, i);
        const bool editable = RISE_API_SceneEditController_PropertyEditable(_controller, i);

        const unsigned int numPresets = RISE_API_SceneEditController_PropertyPresetCount(_controller, i);
        NSMutableArray<RISEViewportPropertyPreset *> *presets =
            [NSMutableArray arrayWithCapacity:numPresets];
        for (unsigned int j = 0; j < numPresets; ++j) {
            if (!RISE_API_SceneEditController_PropertyPresetLabel(_controller, i, j, presetLabelBuf, sizeof(presetLabelBuf))) continue;
            if (!RISE_API_SceneEditController_PropertyPresetValue(_controller, i, j, presetValueBuf, sizeof(presetValueBuf))) continue;
            // `+stringWithUTF8String:` returns nil on invalid UTF-8
            // (e.g. a truncation that landed mid-codepoint).  Skip
            // the preset rather than feed nil into NSMutableArray.
            NSString *label = [NSString stringWithUTF8String:presetLabelBuf];
            NSString *value = [NSString stringWithUTF8String:presetValueBuf];
            if (!label || !value) continue;
            [presets addObject:[[RISEViewportPropertyPreset alloc]
                                initWithLabel:label value:value]];
        }

        // Unit label: optional short suffix shown next to the field
        // ("mm" / "°" / "scene units").  Empty when the descriptor
        // declared no unit.
        NSString *unitLabel = @"";
        if( RISE_API_SceneEditController_PropertyUnitLabel(_controller, i, unitLabelBuf, sizeof(unitLabelBuf)) ) {
            NSString *u = [NSString stringWithUTF8String:unitLabelBuf];
            if( u ) unitLabel = u;
        }

        RISEViewportProperty *p = [[RISEViewportProperty alloc] initWithName:[NSString stringWithUTF8String:nameBuf]
                                                                       value:[NSString stringWithUTF8String:valBuf]
                                                                  describing:[NSString stringWithUTF8String:descBuf]
                                                                        kind:kind
                                                                    editable:editable ? YES : NO
                                                                     presets:presets
                                                                   unitLabel:unitLabel];
        [out addObject:p];
    }
    return out;
}

- (BOOL)setPropertyName:(NSString *)name value:(NSString *)value {
    if (!_controller || !name || !value) return NO;
    BOOL ok = RISE_API_SceneEditController_SetProperty(
        _controller, [name UTF8String], [value UTF8String]) ? YES : NO;
    if (ok) {
        // Re-snapshot so the panel reflects the canonical re-formatted
        // value (e.g. "1 2 3" canonicalized to "1 2 3").
        [self refreshProperties];
    }
    return ok;
}

#pragma mark - Phase 4b per-category accessors

- (NSString *)selectionNameFor:(RISEViewportCategory)category {
    if (!_controller) return @"";
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_GetSelectionForCategory(
            _controller, (int)category, buf, sizeof(buf))) {
        return @"";
    }
    NSString *s = [NSString stringWithUTF8String:buf];
    return s ?: @"";
}

- (BOOL)isSectionExpandedFor:(RISEViewportCategory)category {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_IsSectionExpanded(_controller, (int)category) ? YES : NO;
}

- (void)collapseSectionFor:(RISEViewportCategory)category {
    if (!_controller) return;
    RISE_API_SceneEditController_CollapseSection(_controller, (int)category);
}

- (NSArray<RISEViewportProperty *> *)propertySnapshotFor:(RISEViewportCategory)category {
    if (!_controller) return @[];
    const int cat = (int)category;
    const unsigned int n = RISE_API_SceneEditController_PropertyCountFor(_controller, cat);
    NSMutableArray<RISEViewportProperty *> *out = [NSMutableArray arrayWithCapacity:n];
    char nameBuf[128];
    char valBuf[256];
    char descBuf[512];
    char presetLabelBuf[256];
    char presetValueBuf[256];
    char unitLabelBuf[64];
    for (unsigned int i = 0; i < n; ++i) {
        RISE_API_SceneEditController_PropertyNameFor(_controller, cat, i, nameBuf, sizeof(nameBuf));
        RISE_API_SceneEditController_PropertyValueFor(_controller, cat, i, valBuf, sizeof(valBuf));
        RISE_API_SceneEditController_PropertyDescriptionFor(_controller, cat, i, descBuf, sizeof(descBuf));
        const int kind = RISE_API_SceneEditController_PropertyKindFor(_controller, cat, i);
        const bool editable = RISE_API_SceneEditController_PropertyEditableFor(_controller, cat, i);

        const unsigned int numPresets = RISE_API_SceneEditController_PropertyPresetCountFor(_controller, cat, i);
        NSMutableArray<RISEViewportPropertyPreset *> *presets =
            [NSMutableArray arrayWithCapacity:numPresets];
        for (unsigned int j = 0; j < numPresets; ++j) {
            if (!RISE_API_SceneEditController_PropertyPresetLabelFor(_controller, cat, i, j, presetLabelBuf, sizeof(presetLabelBuf))) continue;
            if (!RISE_API_SceneEditController_PropertyPresetValueFor(_controller, cat, i, j, presetValueBuf, sizeof(presetValueBuf))) continue;
            NSString *label = [NSString stringWithUTF8String:presetLabelBuf];
            NSString *value = [NSString stringWithUTF8String:presetValueBuf];
            if (!label || !value) continue;
            [presets addObject:[[RISEViewportPropertyPreset alloc]
                                initWithLabel:label value:value]];
        }

        NSString *unitLabel = @"";
        if( RISE_API_SceneEditController_PropertyUnitLabelFor(_controller, cat, i, unitLabelBuf, sizeof(unitLabelBuf)) ) {
            NSString *u = [NSString stringWithUTF8String:unitLabelBuf];
            if( u ) unitLabel = u;
        }

        RISEViewportProperty *p = [[RISEViewportProperty alloc] initWithName:[NSString stringWithUTF8String:nameBuf]
                                                                       value:[NSString stringWithUTF8String:valBuf]
                                                                  describing:[NSString stringWithUTF8String:descBuf]
                                                                        kind:kind
                                                                    editable:editable ? YES : NO
                                                                     presets:presets
                                                                   unitLabel:unitLabel];
        [out addObject:p];
    }
    return out;
}

- (BOOL)setPropertyForCategory:(RISEViewportCategory)category
                          name:(NSString *)name
                         value:(NSString *)value {
    if (!_controller || !name || !value) return NO;
    BOOL ok = RISE_API_SceneEditController_SetPropertyForCategory(
        _controller, (int)category, [name UTF8String], [value UTF8String]) ? YES : NO;
    if (ok) {
        [self refreshProperties];
    }
    return ok;
}

@end

@implementation RISEViewportProperty {
    NSString *_name;
    NSString *_value;
    NSString *_describing;
    NSInteger _kind;
    BOOL _editable;
    NSArray<RISEViewportPropertyPreset *> *_presets;
    NSString *_unitLabel;
}

- (instancetype)initWithName:(NSString *)name
                       value:(NSString *)value
                  describing:(NSString *)describing
                        kind:(NSInteger)kind
                    editable:(BOOL)editable
                     presets:(NSArray<RISEViewportPropertyPreset *> *)presets
                   unitLabel:(NSString *)unitLabel
{
    self = [super init];
    if (self) {
        _name = [name copy];
        _value = [value copy];
        _describing = [describing copy];
        _kind = kind;
        _editable = editable;
        _presets = [presets copy] ?: @[];
        _unitLabel = [unitLabel copy] ?: @"";
    }
    return self;
}

- (NSString *)name        { return _name; }
- (NSString *)value       { return _value; }
- (NSString *)describing  { return _describing; }
- (NSInteger)kind         { return _kind; }
- (BOOL)editable          { return _editable; }
- (NSArray<RISEViewportPropertyPreset *> *)presets { return _presets; }
- (NSString *)unitLabel   { return _unitLabel; }

@end

@implementation RISEViewportPropertyPreset {
    NSString *_label;
    NSString *_value;
}

- (instancetype)initWithLabel:(NSString *)label value:(NSString *)value
{
    self = [super init];
    if (self) {
        _label = [label copy];
        _value = [value copy];
    }
    return self;
}

- (NSString *)label { return _label; }
- (NSString *)value { return _value; }

@end

#pragma mark - RISEViewportGizmoHandle
// `_initWithKind:` selector is hoisted into scope by the class
// extension at the top of this file so the `gizmoHandles` accessor
// (defined earlier) can call it.

@implementation RISEViewportGizmoHandle {
    RISEViewportGizmoKind _kind;
    NSInteger             _axis;
    CGFloat               _screenX;
    CGFloat               _screenY;
    CGFloat               _screenRadius;
}

- (instancetype)_initWithKind:(RISEViewportGizmoKind)kind
                         axis:(NSInteger)axis
                      screenX:(CGFloat)screenX
                      screenY:(CGFloat)screenY
                 screenRadius:(CGFloat)screenRadius
{
    self = [super init];
    if (self) {
        _kind = kind;
        _axis = axis;
        _screenX = screenX;
        _screenY = screenY;
        _screenRadius = screenRadius;
    }
    return self;
}

- (RISEViewportGizmoKind)kind         { return _kind; }
- (NSInteger)axis                     { return _axis; }
- (CGFloat)screenX                    { return _screenX; }
- (CGFloat)screenY                    { return _screenY; }
- (CGFloat)screenRadius               { return _screenRadius; }

@end
