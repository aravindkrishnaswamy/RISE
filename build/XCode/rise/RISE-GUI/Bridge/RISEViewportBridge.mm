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

#include <atomic>
#include <vector>
#include <cstring>

using namespace RISE;

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
    , mSuppressNext( false )
    {}

    virtual ~ViewportPreviewSink() {
        // _block is __strong; nilling on dealloc lets ARC release it.
        mBlock = nil;
    }

    void SetBlock( RISEViewportImageBlock block ) {
        mBlock = [block copy];
    }

    // Borrowed; the bridge keeps the controller alive for the sink's
    // lifetime.  Used to query IsCancelRequested at end-of-pass.
    void SetController( SceneEditController* c ) { mController = c; }

    // Set true to drop the very next OutputImage call.  Auto-clears
    // after one drop.  Atomic so the bridge can call this from the
    // UI thread while the render thread fires OutputImage from a
    // worker thread.
    void SuppressNextFrame() { mSuppressNext.store( true ); }

    // Per-tile callback fires many times per render pass — explicitly
    // ignore.  We don't want the user seeing tile-by-tile fills.
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
                      const RISE::Rect* /*pRegion*/,
                      const unsigned int /*frame*/ ) override {
        if( mSuppressNext.exchange( false ) ) {
            // One-shot suppression (post-production) — skip exactly
            // this dispatch.  The next render's frame goes through.
            return;
        }
        BlitWholeAndDispatch( pImage );
    }

private:
    __strong RISEViewportImageBlock mBlock;
    SceneEditController*            mController;     // borrowed
    std::atomic<bool>               mSuppressNext;

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
        RISE_API_DestroySceneEditController(_controller);
        _controller = nullptr;
    }
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

- (BOOL)isRunning {
    return _ownsRunning;
}

#pragma mark - Toolbar

- (RISEViewportTool)currentTool {
    return RISEViewportToolSelect;  // C-API doesn't surface CurrentTool yet
}

- (void)setCurrentTool:(RISEViewportTool)tool {
    if (!_controller) return;
    RISE_API_SceneEditController_SetTool(_controller, static_cast<int>(tool));
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

- (RISEViewportCategory)selectionCategory {
    if (!_controller) return RISEViewportCategoryNone;
    const int c = RISE_API_SceneEditController_GetSelectionCategory(_controller);
    switch (c) {
        case 1: return RISEViewportCategoryCamera;
        case 2: return RISEViewportCategoryRasterizer;
        case 3: return RISEViewportCategoryObject;
        case 4: return RISEViewportCategoryLight;
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
