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
#include "Agent/AgentSession.h"   // Facet 5 slice 1c-1: live in-app agent injection
#include "Agent/AgentRpc.h"
#include "Agent/AgentMcpAdapter.h"          // Secure-MCP slice 5c: GUI-hosted external MCP endpoint
#include "Agent/AgentLoopbackHttpServer.h"  // Secure-MCP slice 5c

#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <cstring>
#include <limits>

using namespace RISE;

namespace {

bool NamedViewIndexFitsCAbi( const NSInteger idx )
{
    return idx >= 0 &&
        static_cast<unsigned long long>(idx) <=
        static_cast<unsigned long long>(std::numeric_limits<unsigned int>::max());
}

NSString* NamedViewDisplayName( const char* bytes )
{
    if( !bytes ) return nil;
    NSString* decoded = [[NSString alloc] initWithBytes:bytes
                                                   length:std::strlen(bytes)
                                                 encoding:NSUTF8StringEncoding];
    // The C ABI carries bytes, not an NSString.  Preserve every byte in a
    // displayable fallback instead of inserting nil into NSMutableArray.
    if( !decoded ) {
        decoded = [[NSString alloc] initWithBytes:bytes
                                            length:std::strlen(bytes)
                                          encoding:NSISOLatin1StringEncoding];
    }
    return decoded;
}

}  // namespace

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

// Class extension: private initialiser for RISEEnvironmentInfo — hoists the
// selector into scope for the `environmentInfo` accessor (the implementation
// lives near the bottom of this file, after RISEViewportProperty's).
@interface RISEEnvironmentInfo ()
- (instancetype)initWithHasEnvironment:(BOOL)hasEnvironment
                         proceduralSky:(BOOL)proceduralSky
                              editable:(BOOL)editable
                           painterName:(NSString *)painterName
                                  file:(NSString *)file
                                 scale:(double)scale
                               orientX:(double)orientX
                               orientY:(double)orientY
                               orientZ:(double)orientZ
                            background:(BOOL)background;
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

// Same hoist for the nav-gizmo nub value object (impl at the bottom).
@interface RISEViewportNavNub ()
- (instancetype)_initWithAxis:(NSInteger)axis
                     negative:(BOOL)negative
                      screenX:(CGFloat)screenX
                      screenY:(CGFloat)screenY
                 screenRadius:(CGFloat)screenRadius
                       facing:(BOOL)facing;
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
// Keeping the production image on screen after a production
// render is NOT handled here anymore.  It used to be a one-shot
// "drop the next dispatch" flag on this sink, but that only
// covered the legacy LDR NSImage path — in EDR mode the
// interactive frame reaches the shared Metal layer through the
// interactive ViewportFrameStore's frame-complete observer
// (bound to the rasterizer's FrameStore via
// OnRasterizerFrameStoreChanged), which never passes through
// this sink, so the suppression was bypassed and the production
// image flipped back to the live preview.  The fix lives one
// layer down: the SceneEditController is restarted via
// `startSuppressingInitialRender`, so it simply doesn't produce
// the overwriting frame on any path until the user interacts.
//
// We do NOT throttle here: every frame the rasterizer produces
// reaches the screen, including partial buffers from cancelled
// passes.  During fast manipulation the cancel flag trips on
// every pointer move, so dropping cancelled frames would mean
// the user only ever sees post-pause refinement passes —
// visually indistinguishable from the viewport being throttled.
// Center-out tile order keeps partial buffers usable
// (centre-of-image fills first).
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
    // partials usable).
    void OutputImage( const IRasterImage& pImage,
                      const RISE::Rect* pRegion,
                      const unsigned int frame ) override {
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

// Secure-MCP slice 5c: value-object result of -startAgentHostedServerWithLabel:.
// Declared/implemented standalone (NOT nested in RISEViewportBridge's
// @implementation) -- Objective-C does not support nested @implementation
// blocks; @interface RISEAgentHostedServerInfo lives in the .h, right
// before RISEViewportBridge's own @interface.
@implementation RISEAgentHostedServerInfo {
    BOOL       _ok;
    NSUInteger _port;
    NSString*  _bearerToken;
    NSString*  _message;
}
- (instancetype)initOk:(BOOL)ok port:(NSUInteger)port
            bearerToken:(NSString*)bearerToken message:(NSString*)message {
    self = [super init];
    if (self) {
        _ok = ok;
        _port = port;
        _bearerToken = [bearerToken copy] ?: @"";
        _message = [message copy] ?: @"";
    }
    return self;
}
- (BOOL)ok { return _ok; }
- (NSUInteger)port { return _port; }
- (NSString *)bearerToken { return _bearerToken; }
- (NSString *)message { return _message; }
@end

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

    // Facet 5 slice 1c-1: the live agent JSON-RPC dispatcher.  Owns a
    // non-owning AgentSession that WrapJob's the SAME IJobPriv the
    // controller wraps and is AttachController'd to `_controller`, so a
    // dispatched propose_patch routes through the controller's render-
    // safe cancel-and-park edit path.  Raw new/delete, mirroring
    // `_controller`.  LIFETIME ORDER: destroyed in -shutdown BEFORE
    // `_controller` (and before the host Job) because the session it
    // holds BORROWS both — deleting the dispatcher first guarantees it
    // never references a dead controller / job.
    RISE::Agent::AgentRpcDispatcher* _agentDispatcher;

    // Agent autonomy selector (2026-07 GUI composer chips): TWO more
    // in-process dispatchers, sibling to `_agentDispatcher` above, that
    // exist for the WHOLE bridge lifetime (not opt-in like the hosted
    // server below) so `-agentHandleToolCall:` never has to construct one
    // mid-turn. `_agentToolDispatcherOwner` borrows an Owner-authority
    // AgentSession (its own instance, separate from `_agentDispatcher`'s —
    // see the .h's routing doc for why they must NOT be the same instance:
    // `_agentDispatcher` must stay permanently Commit-capable for
    // `resolve_proposal`, so ONLY this separate instance's autonomy is
    // ever toggled between Read/Commit via AgentRpcDispatcher::SetAutonomy
    // as `agentAutonomyLevel` changes). `_agentToolDispatcherPropose`
    // borrows a SEPARATE External-authority AgentSession, fixed at Propose
    // autonomy for its whole life (mirrors -startAgentHostedServerWithLabel:'s
    // External+Propose construction, minus the HTTP server — see that
    // method's doc for the authority/autonomy pairing rationale). Both
    // AttachController'd to the SAME `_controller` as `_agentDispatcher`'s
    // session, so a staged proposal lands on the SAME queue
    // list_proposals/resolve_proposal already read. Torn down alongside
    // `_agentDispatcher` in -shutdown (same BORROWS-the-controller lifetime
    // rule). All three dispatchers are called only from the main/UI thread
    // (matching AgentRpcDispatcher's single-caller contract), so there is
    // no cross-instance locking concern despite three coexisting instances.
    RISE::Agent::AgentRpcDispatcher* _agentToolDispatcherOwner;
    RISE::Agent::AgentRpcDispatcher* _agentToolDispatcherPropose;
    RISEAgentAutonomyLevel           _agentAutonomyLevel;

    // Secure-MCP slice 5c: the GUI-hosted EXTERNAL loopback MCP server --
    // a SEPARATE AgentSession (External authority) + AgentMcpAdapter +
    // AgentLoopbackHttpServer from `_agentDispatcher` above (which is the
    // Owner-authority, in-process dispatcher -agentHandleLine drives).
    // Null/empty when not hosting (the default -- opt-in only via
    // -startAgentHostedServerWithLabel:). The External AgentSession
    // (WrapJob, non-owning, AttachController'd to `_controller` -- see
    // -startAgentHostedServerWithLabel:'s body) is owned INTERNALLY by
    // `_agentHttpAdapter`'s AgentRpcDispatcher (mirrors how
    // AgentMcpAdapter always owns its wrapped session -- see
    // AgentMcpAdapter.h); this class does not hold a separate pointer to
    // it. Two independent sessions (this one and `_agentDispatcher`'s)
    // borrow the SAME controller, serialized by the controller's own
    // mMutex (see the header doc's "two-dispatcher-one-controller" note).
    // `_agentHttpServerThread` runs the server's serial accept-handle
    // loop; joined by -stopAgentHostedServer / -shutdown before the
    // session/controller/job it touches goes away.
    std::unique_ptr<RISE::Agent::AgentMcpAdapter>       _agentHttpAdapter;
    std::unique_ptr<RISE::Agent::AgentLoopbackHttpServer> _agentHttpServer;
    std::thread                                          _agentHttpServerThread;
    BOOL                                                  _agentHttpServerRunning;
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
    _agentHttpServerRunning = NO;
    _agentDispatcher = nullptr;
    _agentToolDispatcherOwner = nullptr;
    _agentToolDispatcherPropose = nullptr;
    _agentAutonomyLevel = RISEAgentAutonomyApply;

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

    // Model-B F2 slice S4: register this controller on the host bridge so
    // its production-render entry points (-rasterize / -rasterizeAnimation /
    // -rasterizeRegionLeft:top:right:bottom:) route through the SAME
    // single-slot coordinator as the interactive loop and agent renders,
    // instead of calling Job::Rasterize() directly.  Cleared back to NULL
    // at the START of -shutdown, before `_controller` is destroyed -- see
    // -attachSceneEditController:'s header doc for the full contract.
    [_host attachSceneEditController:static_cast<void*>(_controller)];

    if (_previewSink) {
        // The sink queries the controller's cancel state at end-of-pass
        // to decide whether to drop a stale dispatch.  Wire the pointer
        // before installing the sink as a rasterizer output.
        _previewSink->SetController(_controller);
        RISE_API_SceneEditController_SetPreviewSink(_controller, _previewSink);
    }

    // Facet 5 slice 1c-1: stand up the live agent dispatcher over the SAME
    // Job the controller wraps, and attach the controller so a dispatched
    // propose_patch routes through its render-safe cancel-and-park edit
    // path (the same path GUI SetProperty uses).  Constructed AFTER
    // `_controller` (whose lifetime it borrows) and torn down BEFORE it
    // (see -shutdown).  WrapJob may return null (defensive; pJob is
    // non-null here), in which case the dispatcher speaks valid JSON-RPC
    // errors for the session-backed verbs — still safe.
    {
        std::unique_ptr<RISE::Agent::AgentSession> session =
            RISE::Agent::AgentSession::WrapJob(pJob);
        if (session) {
            session->AttachController(_controller);
        }
        _agentDispatcher =
            new RISE::Agent::AgentRpcDispatcher(std::move(session));
    }

    // Agent autonomy selector (2026-07): stand up the two sibling tool-call
    // dispatchers alongside `_agentDispatcher` above -- see the ivar
    // block's doc for why these are separate instances rather than one
    // dispatcher whose autonomy gets mutated in place. Both borrow the SAME
    // `_controller` `_agentDispatcher`'s own session is attached to.
    {
        std::unique_ptr<RISE::Agent::AgentSession> ownerSession =
            RISE::Agent::AgentSession::WrapJob(pJob);
        if (ownerSession) {
            ownerSession->AttachController(_controller);
        }
        // Starts at Commit -- matches `_agentAutonomyLevel`'s
        // RISEAgentAutonomyApply default above, so this dispatcher's
        // observable behaviour is identical to `_agentDispatcher`'s until
        // the Swift composer explicitly picks a different level.
        _agentToolDispatcherOwner = new RISE::Agent::AgentRpcDispatcher(
            std::move(ownerSession), RISE::Agent::AgentAutonomy::Commit);

        std::unique_ptr<RISE::Agent::AgentSession> proposeSession =
            RISE::Agent::AgentSession::WrapJob(pJob, RISE::Agent::AgentAuthority::External);
        if (proposeSession) {
            proposeSession->AttachController(_controller);
            // Diagnostic only (SceneEditController::AgentProposal::sessionLabel) --
            // distinguishes an in-app "Propose" chip proposal from an
            // external-MCP-client one in the proposals panel.
            proposeSession->SetSessionLabel("in-app-propose");
        }
        // Fixed for this dispatcher's whole life -- Propose autonomy is the
        // posture that PAIRS with External authority (AgentRpc.h's file
        // header); there is no level under which this instance should ever
        // run as anything else.
        _agentToolDispatcherPropose = new RISE::Agent::AgentRpcDispatcher(
            std::move(proposeSession), RISE::Agent::AgentAutonomy::Propose);
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
    // Model-B F2 slice S4: deregister FIRST, before anything else in this
    // method — the host bridge's production-render entry points
    // (-rasterize etc.) read `_viewportController` from whatever thread the
    // platform UI happens to call them on, so this pointer must go back to
    // NULL before `_controller` itself starts coming apart below.  Safe to
    // call even if a stale render is still draining inside the coordinator:
    // RISE_API_DestroySceneEditController (further down) is what actually
    // calls Stop()/joins the worker; clearing the host's pointer here just
    // stops any NEW production render from being submitted to a controller
    // that is about to disappear.
    [_host attachSceneEditController:nullptr];

    // Secure-MCP slice 5c: stop the EXTERNAL hosted server BEFORE the
    // in-process agent dispatcher / controller below.  -stopAgentHostedServer
    // signals Stop() and JOINS the server thread, so by the time this call
    // returns, no external HandleLine call is in flight against the
    // External AgentSession `_agentHttpAdapter` owns internally (which,
    // like `_agentDispatcher`'s own session, BORROWS `_controller` and the
    // host Job) — the same "stale server over a torn-down controller = UAF"
    // class the S4/agent-dispatcher teardown discipline above already
    // guards against. Idempotent (a no-op when no server is running), so
    // calling it unconditionally here is safe whether or not
    // -startAgentHostedServerWithLabel: was ever called for this bridge
    // instance's lifetime.
    [self stopAgentHostedServer];

    // Facet 5 slice 1c-1: destroy the agent dispatcher FIRST — the
    // AgentSession it owns BORROWS both `_controller` and the host Job.
    // Deleting it here, before the controller is torn down below,
    // guarantees the session never references a dead controller / job
    // (the lifetime-order invariant noted at the ivar declaration).
    // Idempotent: -shutdown may be called explicitly and again from
    // -dealloc; the nil guard makes the second call a no-op.
    //
    // Fix-round-1 P1-A belt-and-braces: explicitly detach the session's
    // controller BEFORE deleting the dispatcher.  AgentSession's C++
    // destructor (reached via the `delete` below, through
    // ~AgentRpcDispatcher's implicit unique_ptr<AgentSession> teardown)
    // now unconditionally drains any outstanding async render on its own —
    // this call is not required for correctness — but calling
    // AttachController(nullptr) here drains (cancel + bound-wait) while
    // `_controller` is STILL VALID and this ordering is still explicit at
    // the call site, rather than relying solely on the destructor reaching
    // the same drain moments later against a controller that is ALSO
    // mid-teardown.  Cheap (a null-check + a possible no-op wait) and
    // removes any doubt about ordering for a future maintainer reading
    // this method top-to-bottom.
    if (_agentDispatcher && _agentDispatcher->Session()) {
        _agentDispatcher->Session()->AttachController(nullptr);
    }
    delete _agentDispatcher;
    _agentDispatcher = nullptr;

    // Agent autonomy selector (2026-07): same detach-then-delete discipline
    // for the two sibling tool-call dispatchers -- they borrow the SAME
    // `_controller` and must not outlive it either.
    if (_agentToolDispatcherOwner && _agentToolDispatcherOwner->Session()) {
        _agentToolDispatcherOwner->Session()->AttachController(nullptr);
    }
    delete _agentToolDispatcherOwner;
    _agentToolDispatcherOwner = nullptr;

    if (_agentToolDispatcherPropose && _agentToolDispatcherPropose->Session()) {
        _agentToolDispatcherPropose->Session()->AttachController(nullptr);
    }
    delete _agentToolDispatcherPropose;
    _agentToolDispatcherPropose = nullptr;

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

- (void)startSuppressingInitialRender {
    if (!_controller) return;
    RISE_API_SceneEditController_StartSuppressingInitialRender(_controller);
    _ownsRunning = YES;
}

- (void)stop {
    if (!_controller) return;
    // Model-B F2 slice S4 fix round 4: StopInteractive, NOT the
    // monolithic Stop() -- this method exists so RenderViewModel can
    // pause the interactive viewport ahead of a production render
    // (startRender/startAnimationRender) WITHOUT permanently retiring
    // this controller's agent-render worker.  See this method's header
    // doc in RISEViewportBridge.h and
    // RISE_API_SceneEditController_StopInteractive's doc for the full
    // rationale -- the monolithic Stop() call here used to poison every
    // later production/agent submission on this controller with
    // "controller stopped".  Full teardown still happens in -shutdown,
    // via RISE_API_DestroySceneEditController's destructor call to the
    // real Stop().
    RISE_API_SceneEditController_StopInteractive(_controller);
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
    // Route through SetViewportFit (NOT ScaleFilmToFit directly) so the Job caches the CURRENT viewport size
    // (this wrapper is the single chokepoint for both load-time and resize-time fits) -- a subsequent D2 full
    // re-derive then re-applies the same fit instead of reverting the preview to the authored full-res dims.
    pJob->SetViewportFit(
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

#pragma mark - Nav gizmo (axis-ball) + view navigation

- (BOOL)refreshNavGizmoWithCenterX:(CGFloat)centerX centerY:(CGFloat)centerY
                        ballRadius:(CGFloat)ballRadius nubRadius:(CGFloat)nubRadius {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_RefreshNavGizmo(
        _controller, centerX, centerY, ballRadius, nubRadius) ? YES : NO;
}

- (NSArray<RISEViewportNavNub *> *)navGizmoNubs {
    if (!_controller) return @[];
    const unsigned int n = RISE_API_SceneEditController_NavGizmoNubCount(_controller);
    NSMutableArray<RISEViewportNavNub *> *out = [NSMutableArray arrayWithCapacity:n];
    for (unsigned int i = 0; i < n; ++i) {
        int axis = 0, negative = 0, facing = 0;
        double x = 0, y = 0, r = 0;
        if (!RISE_API_SceneEditController_NavGizmoNub(
                _controller, i, &axis, &negative, &x, &y, &r, &facing)) {
            continue;
        }
        RISEViewportNavNub *nub = [[RISEViewportNavNub alloc]
            _initWithAxis:static_cast<NSInteger>(axis)
                 negative:(negative != 0)
                  screenX:static_cast<CGFloat>(x)
                  screenY:static_cast<CGFloat>(y)
             screenRadius:static_cast<CGFloat>(r)
                   facing:(facing != 0)];
        [out addObject:nub];
    }
    return out;
}

- (NSInteger)navGizmoNubAtX:(CGFloat)x y:(CGFloat)y {
    if (!_controller) return -1;
    return RISE_API_SceneEditController_NavGizmoNubAt(_controller, x, y);
}

- (BOOL)snapViewToAxis:(NSInteger)axis negative:(BOOL)negative {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_SnapViewToAxis(
        _controller, static_cast<int>(axis), negative ? 1 : 0) ? YES : NO;
}

- (BOOL)enterFreeFly {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_EnterFreeFly(_controller) ? YES : NO;
}

- (BOOL)exitFreeFly {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_ExitFreeFly(_controller) ? YES : NO;
}

- (BOOL)freeFlyActive {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_IsFreeFlyActive(_controller) ? YES : NO;
}

- (BOOL)setHomeView {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_SetHomeView(_controller) ? YES : NO;
}

- (BOOL)goToHomeView {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_GoToHomeView(_controller) ? YES : NO;
}

- (BOOL)hasHomeView {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_HasHomeView(_controller) ? YES : NO;
}

- (NSString *)stampViewToNewCamera:(NSString *)proposedName {
    if (!_controller) return nil;
    char name[256] = {0};
    const char* prop = proposedName ? [proposedName UTF8String] : "";
    if (!RISE_API_SceneEditController_StampViewToNewCamera(
            _controller, prop, name, sizeof(name))) {
        return nil;
    }
    return [NSString stringWithUTF8String:name];
}

#pragma mark - Named Views (B1)

- (BOOL)captureNamedView:(NSString *)name {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_CaptureNamedView(
        _controller, name ? [name UTF8String] : "") ? YES : NO;
}

- (NSArray<NSString *> *)namedViewNames {
    if (!_controller) return @[];
    const unsigned int n = RISE_API_SceneEditController_NamedViewCount(_controller);
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:n];
    for (unsigned int i = 0; i < n; ++i) {
        char nm[256] = {0};
        if (RISE_API_SceneEditController_NamedViewName(_controller, i, nm, sizeof(nm))) {
            if (NSString* displayName = NamedViewDisplayName(nm)) {
                [out addObject:displayName];
            }
        }
    }
    return out;
}

- (BOOL)restoreNamedView:(NSInteger)idx {
    if (!_controller || !NamedViewIndexFitsCAbi(idx)) return NO;
    return RISE_API_SceneEditController_RestoreNamedView(
        _controller, static_cast<unsigned int>(idx)) ? YES : NO;
}

- (BOOL)updateNamedView:(NSInteger)idx {
    if (!_controller || !NamedViewIndexFitsCAbi(idx)) return NO;
    return RISE_API_SceneEditController_UpdateNamedView(
        _controller, static_cast<unsigned int>(idx)) ? YES : NO;
}

- (BOOL)deleteNamedView:(NSInteger)idx {
    if (!_controller || !NamedViewIndexFitsCAbi(idx)) return NO;
    return RISE_API_SceneEditController_DeleteNamedView(
        _controller, static_cast<unsigned int>(idx)) ? YES : NO;
}

- (NSString *)promoteNamedView:(NSInteger)idx name:(NSString *)proposedName {
    if (!_controller || !NamedViewIndexFitsCAbi(idx)) return nil;
    char name[256] = {0};
    const char* prop = proposedName ? [proposedName UTF8String] : "";
    if (!RISE_API_SceneEditController_PromoteNamedViewToCamera(
            _controller, static_cast<unsigned int>(idx), prop, name, sizeof(name))) {
        return nil;
    }
    return [NSString stringWithUTF8String:name];
}

#pragma mark - Viewport render modes (P1, docs/gui/RENDER_MODES.md §5)

- (NSArray<NSDictionary<NSString *, NSString *> *> *)viewportRenderModes {
    if (!_controller) return @[];
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *out = [NSMutableArray array];
    const unsigned int n = RISE_API_GetViewportRenderModeCount();
    for (unsigned int i = 0; i < n; ++i) {
        const char* name = nullptr;
        const char* title = nullptr;
        const char* question = nullptr;
        bool selectable = false;
        if (!RISE_API_GetViewportRenderModeInfo(i, &name, &title, &question, &selectable)) {
            continue;
        }
        if (!selectable) continue;   // e.g. "objectmap" -- its own palette-lifecycle pipeline
        [out addObject:@{
            @"name":     name     ? [NSString stringWithUTF8String:name]     : @"",
            @"title":    title    ? [NSString stringWithUTF8String:title]    : @"",
            @"question": question ? [NSString stringWithUTF8String:question] : @"",
        }];
    }
    return out;
}

- (NSString *)viewportRenderMode {
    if (!_controller) return @"preview";
    const char* name = RISE_API_SceneEditController_GetViewportRenderMode(_controller);
    return name ? [NSString stringWithUTF8String:name] : @"preview";
}

- (BOOL)setViewportRenderMode:(NSString *)name {
    if (!_controller || !name) return NO;
    return RISE_API_SceneEditController_SetViewportRenderMode(
        _controller, [name UTF8String]) ? YES : NO;
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

- (NSString *)undoActionLabel {
    if (!_controller) return @"";
    char buf[256] = {0};
    if (!RISE_API_SceneEditController_UndoLabel(_controller, buf, sizeof(buf))) return @"";
    return [NSString stringWithUTF8String:buf] ?: @"";
}

- (NSString *)redoActionLabel {
    if (!_controller) return @"";
    char buf[256] = {0};
    if (!RISE_API_SceneEditController_RedoLabel(_controller, buf, sizeof(buf))) return @"";
    return [NSString stringWithUTF8String:buf] ?: @"";
}

#pragma mark - Editor live-sync (UI refinement item 1)

- (NSString *)serializedSceneText {
    if (!_controller) return @"";
    char* text = RISE_API_SceneEditController_SerializedSceneTextAlloc(_controller);
    if (!text) return @"";
    NSString* out = [NSString stringWithUTF8String:text] ?: @"";
    RISE_API_FreeString(text);
    return out;
}

- (BOOL)getSceneTextVersionUuid:(unsigned long long *)outUuid
                       revision:(unsigned long long *)outRevision {
    if (outUuid)     *outUuid = 0;
    if (outRevision) *outRevision = 0;
    if (!_controller) return NO;
    return RISE_API_SceneEditController_GetSceneTextVersion(
               _controller, outUuid, outRevision) ? YES : NO;
}

#pragma mark - Refinement pause + status (UI redesign, design brief A2)

- (void)pauseRefinement {
    if (!_controller) return;
    RISE_API_SceneEditController_PauseRefinement(_controller);
}

- (void)resumeRefinement {
    if (!_controller) return;
    RISE_API_SceneEditController_ResumeRefinement(_controller);
}

- (BOOL)isRefinementPaused {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_IsRefinementPaused(_controller) ? YES : NO;
}

- (int)refinementPhaseWithScaleDivisor:(unsigned int *)scaleDivisor {
    if (!_controller) {
        if (scaleDivisor) *scaleDivisor = 1;
        return -1;
    }
    return RISE_API_SceneEditController_GetRefinementStatus(_controller, scaleDivisor);
}

#pragma mark - Interactive region-of-interest (UI redesign, design brief A4)

- (void)setInteractiveRegionLeft:(unsigned int)left
                             top:(unsigned int)top
                           right:(unsigned int)right
                          bottom:(unsigned int)bottom {
    if (!_controller) return;
    RISE_API_SceneEditController_SetInteractiveRegion(_controller, left, top, right, bottom);
}

- (void)clearInteractiveRegion {
    if (!_controller) return;
    RISE_API_SceneEditController_ClearInteractiveRegion(_controller);
}

- (BOOL)getInteractiveRegionLeft:(unsigned int *)left
                             top:(unsigned int *)top
                           right:(unsigned int *)right
                          bottom:(unsigned int *)bottom {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_GetInteractiveRegion(
               _controller, left, top, right, bottom) ? YES : NO;
}

- (BOOL)interactiveRasterizerHonorsRegion {
    if (!_controller) return NO;
    return RISE_API_SceneEditController_InteractiveRasterizerHonorsRegion(_controller) ? YES : NO;
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
        case 8: return RISEViewportCategoryAnimation;
        case 9: return RISEViewportCategorySceneVariant;
        case 10: return RISEViewportCategoryPainter;
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

- (BOOL)getEntitySourceLocationForCategory:(RISEViewportCategory)category
                                       name:(NSString *)name
                                 byteOffset:(unsigned long long *)outOffset
                                       line:(unsigned int *)outLine {
    if (outOffset) *outOffset = 0;
    if (outLine)   *outLine   = 0;
    if (!_controller || !name) return NO;
    const int cat = (int)category;
    return RISE_API_SceneEditController_GetEntitySourceLocation(
               _controller, cat, name.UTF8String, outOffset, outLine) ? YES : NO;
}

#pragma mark - Source traceability

- (BOOL)resolveSourceSpanForCategory:(RISEViewportCategory)category
                                name:(NSString *)name
                               param:(NSString *)param
                          occurrence:(int)occ
                          byteOffset:(unsigned long long *)outOffset
                          byteLength:(unsigned long long *)outLength
                                line:(unsigned int *)outLine
                              column:(unsigned int *)outColumn {
    if (outOffset) *outOffset = 0;
    if (outLength) *outLength = 0;
    if (outLine)   *outLine   = 0;
    if (outColumn) *outColumn = 0;
    if (!_controller) return NO;
    return RISE_API_SceneEditController_ResolveSourceSpan(
               _controller, (int)category,
               name  ? name.UTF8String  : "",
               param ? param.UTF8String : "",
               occ, outOffset, outLength, outLine, outColumn) ? YES : NO;
}

- (BOOL)sourceRefAtByteOffset:(unsigned long long)offset
                     category:(RISEViewportCategory *)outCategory
                         name:(NSString **)outName
                        param:(NSString **)outParam
                   occurrence:(int *)outOccurrence {
    if (outName)  *outName  = nil;
    if (outParam) *outParam = nil;
    if (outOccurrence) *outOccurrence = 0;
    if (!_controller) return NO;
    int catInt = 0, occ = 0;
    char nameBuf[256] = {0};
    char paramBuf[128] = {0};
    const BOOL ok = RISE_API_SceneEditController_SourceRefAtByteOffset(
        _controller, offset, &catInt,
        nameBuf, sizeof(nameBuf), paramBuf, sizeof(paramBuf), &occ) ? YES : NO;
    if (!ok) return NO;
    if (outCategory)   *outCategory   = (RISEViewportCategory)catInt;
    if (outName)       *outName       = [NSString stringWithUTF8String:nameBuf] ?: @"";
    if (outParam)      *outParam      = [NSString stringWithUTF8String:paramBuf] ?: @"";
    if (outOccurrence) *outOccurrence = occ;
    return YES;
}

#pragma mark - Entity creation + painter CRUD (entity-creation slice)

- (NSUInteger)entityTemplateCountForCategory:(RISEViewportCategory)category {
    if (!_controller) return 0;
    return static_cast<NSUInteger>(
        RISE_API_SceneEditController_EntityTemplateCount(_controller, (int)category) );
}

- (NSString *)entityTemplateLabelForCategory:(RISEViewportCategory)category
                                        index:(NSUInteger)idx {
    if (!_controller) return @"";
    char buf[128] = {0};
    if (!RISE_API_SceneEditController_EntityTemplateLabel(
            _controller, (int)category, (unsigned int)idx, buf, sizeof(buf))) {
        return @"";
    }
    NSString *s = [NSString stringWithUTF8String:buf];
    return s ?: @"";
}

- (BOOL)instantiateEntityTemplateForCategory:(RISEViewportCategory)category
                                        index:(NSUInteger)idx
                                      outName:(NSString **)outName
                                   outMessage:(NSString **)outMessage {
    if (outName)    *outName    = nil;
    if (outMessage) *outMessage = nil;
    if (!_controller) return NO;
    char nameBuf[256] = {0};
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const BOOL applied = RISE_API_SceneEditController_InstantiateEntityTemplate(
        _controller, (int)category, (unsigned int)idx,
        nameBuf, sizeof(nameBuf),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf)) ? YES : NO;
    if (outName && nameBuf[0] != '\0') {
        *outName = [NSString stringWithUTF8String:nameBuf];
    }
    if (outMessage && messageBuf[0] != '\0') {
        *outMessage = [NSString stringWithUTF8String:messageBuf];
    }
    if (applied) {
        // Structural mutation: bump epoch-driven refresh paths so the
        // outliner re-enumerates and the properties panel reflects the
        // new entity once the caller selects it.
        [self refreshProperties];
    }
    return applied;
}

- (BOOL)duplicateEntityForCategory:(RISEViewportCategory)category
                               name:(NSString *)name
                            outName:(NSString **)outName
                         outMessage:(NSString **)outMessage {
    if (outName)    *outName    = nil;
    if (outMessage) *outMessage = nil;
    if (!_controller || !name) return NO;
    char nameBuf[256] = {0};
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const BOOL applied = RISE_API_SceneEditController_DuplicateEntity(
        _controller, (int)category, [name UTF8String],
        nameBuf, sizeof(nameBuf),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf)) ? YES : NO;
    if (outName && nameBuf[0] != '\0') {
        *outName = [NSString stringWithUTF8String:nameBuf];
    }
    if (outMessage && messageBuf[0] != '\0') {
        *outMessage = [NSString stringWithUTF8String:messageBuf];
    }
    if (applied) {
        [self refreshProperties];
    }
    return applied;
}

- (BOOL)removeEntityForCategory:(RISEViewportCategory)category
                            name:(NSString *)name
                      outMessage:(NSString **)outMessage {
    if (outMessage) *outMessage = nil;
    if (!_controller || !name) return NO;
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const BOOL applied = RISE_API_SceneEditController_RemoveEntity(
        _controller, (int)category, [name UTF8String],
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf)) ? YES : NO;
    if (outMessage && messageBuf[0] != '\0') {
        *outMessage = [NSString stringWithUTF8String:messageBuf];
    }
    if (applied) {
        [self refreshProperties];
    }
    return applied;
}

#pragma mark - Environment / IBL section

- (nullable RISEEnvironmentInfo *)environmentInfo {
    if (!_controller) return nil;
    int hasEnv = 0, procSky = 0, editable = 0, background = 0;
    char nameBuf[256] = {0};
    char fileBuf[1024] = {0};
    double scale = 1.0, ox = 0.0, oy = 0.0, oz = 0.0;
    if (!RISE_API_SceneEditController_GetEnvironment(
            _controller, &hasEnv, &procSky, &editable,
            nameBuf, sizeof(nameBuf), fileBuf, sizeof(fileBuf),
            &scale, &ox, &oy, &oz, &background)) {
        return nil;
    }
    NSString *nm = [NSString stringWithUTF8String:nameBuf] ?: @"";
    NSString *fl = [NSString stringWithUTF8String:fileBuf] ?: @"";
    return [[RISEEnvironmentInfo alloc] initWithHasEnvironment:(hasEnv != 0)
                                                 proceduralSky:(procSky != 0)
                                                      editable:(editable != 0)
                                                   painterName:nm
                                                          file:fl
                                                         scale:scale
                                                       orientX:ox
                                                       orientY:oy
                                                       orientZ:oz
                                                    background:(background != 0)];
}

- (BOOL)setEnvironmentScale:(double)scale {
    if (!_controller) return NO;
    const BOOL ok = RISE_API_SceneEditController_SetEnvironmentScale(_controller, scale) ? YES : NO;
    if (ok) [self refreshProperties];
    return ok;
}

- (BOOL)setEnvironmentBackground:(BOOL)background {
    if (!_controller) return NO;
    const BOOL ok = RISE_API_SceneEditController_SetEnvironmentBackground(_controller, background ? 1 : 0) ? YES : NO;
    if (ok) [self refreshProperties];
    return ok;
}

- (BOOL)setEnvironmentOrientX:(double)x y:(double)y z:(double)z {
    if (!_controller) return NO;
    const BOOL ok = RISE_API_SceneEditController_SetEnvironmentOrient(_controller, x, y, z) ? YES : NO;
    if (ok) [self refreshProperties];
    return ok;
}

- (BOOL)setEnvironmentFile:(NSString *)absPath {
    if (!_controller || !absPath) return NO;
    const BOOL ok = RISE_API_SceneEditController_SetEnvironmentFile(_controller, absPath.UTF8String) ? YES : NO;
    if (ok) [self refreshProperties];
    return ok;
}

- (BOOL)addEnvironment:(NSString *)hdriPath
               outName:(NSString **)outName
            outMessage:(NSString **)outMessage {
    if (outName)    *outName    = nil;
    if (outMessage) *outMessage = nil;
    if (!_controller || !hdriPath) return NO;
    char nameBuf[256] = {0};
    char statusBuf[64] = {0};
    char messageBuf[1024] = {0};
    const BOOL applied = RISE_API_SceneEditController_AddEnvironment(
        _controller, hdriPath.UTF8String,
        nameBuf, sizeof(nameBuf),
        statusBuf, sizeof(statusBuf),
        messageBuf, sizeof(messageBuf)) ? YES : NO;
    if (outName && nameBuf[0] != '\0') {
        *outName = [NSString stringWithUTF8String:nameBuf];
    }
    if (outMessage && messageBuf[0] != '\0') {
        *outMessage = [NSString stringWithUTF8String:messageBuf];
    }
    if (applied) [self refreshProperties];
    return applied;
}

- (BOOL)removeEnvironment {
    if (!_controller) return NO;
    const BOOL ok = RISE_API_SceneEditController_RemoveEnvironment(_controller) ? YES : NO;
    if (ok) [self refreshProperties];
    return ok;
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

#pragma mark - Agent surface (Facet 5 slice 1c-1)

- (NSString *)agentHandleLine:(NSString *)jsonRpcRequest {
    // Guard the no-dispatcher / nil-input cases with a well-formed
    // JSON-RPC -32603 error so the Swift caller always parses a valid
    // response line (never nil).  id=null because we can't reliably
    // attribute the request without running the dispatcher's parse.
    static NSString* const kNoDispatcher =
        @"{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
        @"{\"code\":-32603,\"message\":\"internal error: agent dispatcher unavailable\"}}";
    if (!_agentDispatcher) {
        return kNoDispatcher;
    }
    // A nil request string becomes an empty line -> the dispatcher's own
    // -32700 parse-error path (it is TOTAL and never throws).  `?: ""`
    // keeps the std::string ctor from a null UTF8 pointer.
    const char* utf8 = jsonRpcRequest ? [jsonRpcRequest UTF8String] : "";
    // SYNCHRONOUS on the calling (main / @MainActor) thread — mirrors how
    // GUI SetProperty drives the controller's cancel-and-park from main.
    const std::string response =
        _agentDispatcher->HandleLine(std::string(utf8 ? utf8 : ""));
    NSString* out = [NSString stringWithUTF8String:response.c_str()];
    return out ?: kNoDispatcher;
}

#pragma mark - Agent autonomy selector (2026-07 GUI composer chips)

- (RISEAgentAutonomyLevel)agentAutonomyLevel {
    return _agentAutonomyLevel;
}

- (void)setAgentAutonomyLevel:(RISEAgentAutonomyLevel)level {
    if (level != RISEAgentAutonomyRead && level != RISEAgentAutonomyPropose
        && level != RISEAgentAutonomyApply) {
        return;  // out-of-range: no-op, keep the previous level (see the .h doc)
    }
    _agentAutonomyLevel = level;
    // Only the OWNER tool dispatcher's autonomy ever changes at runtime —
    // `_agentToolDispatcherPropose` stays fixed at Propose for its whole
    // life (see the ivar block's doc), and `_agentDispatcher` (the
    // administrative path) is never touched here at all.
    if (_agentToolDispatcherOwner) {
        _agentToolDispatcherOwner->SetAutonomy(
            level == RISEAgentAutonomyRead ? RISE::Agent::AgentAutonomy::Read
                                            : RISE::Agent::AgentAutonomy::Commit);
    }
}

- (NSString *)agentHandleToolCall:(NSString *)jsonRpcRequest {
    static NSString* const kNoDispatcher =
        @"{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
        @"{\"code\":-32603,\"message\":\"internal error: agent dispatcher unavailable\"}}";

    RISE::Agent::AgentRpcDispatcher* dispatcher =
        (_agentAutonomyLevel == RISEAgentAutonomyPropose) ? _agentToolDispatcherPropose
                                                            : _agentToolDispatcherOwner;
    if (!dispatcher) {
        return kNoDispatcher;
    }
    const char* utf8 = jsonRpcRequest ? [jsonRpcRequest UTF8String] : "";
    const std::string response = dispatcher->HandleLine(std::string(utf8 ? utf8 : ""));
    NSString* out = [NSString stringWithUTF8String:response.c_str()];
    return out ?: kNoDispatcher;
}

#pragma mark - Secure-MCP slice 5c: GUI-hosted external MCP endpoint

// THREADING MODEL (two-dispatcher-one-controller), read this before
// touching anything in this section:
//
//   `_agentDispatcher` (constructed in -initWithHostBridge:, Facet 5
//   slice 1c-1) is the OWNER-authority, Commit-autonomy dispatcher the
//   Swift ChatViewModel / the raw JSON-RPC debug panel drive via
//   -agentHandleLine, always synchronously on the MAIN THREAD (Swift's
//   @MainActor).
//
//   `_agentHttpAdapter` (constructed below by
//   -startAgentHostedServerWithLabel:) wraps a SEPARATE, SECOND
//   AgentRpcDispatcher (owned internally by AgentMcpAdapter) built over
//   a SEPARATE, SECOND AgentSession (External authority, Propose
//   autonomy, owned internally by that dispatcher -- this class holds no
//   raw pointer to it). `_agentHttpServer`'s Serve() loop runs on
//   `_agentHttpServerThread` -- a background thread this class owns --
//   and calls HandleLine on THIS SECOND dispatcher for every accepted
//   HTTP connection, ONE AT A TIME (AgentLoopbackHttpServer's own serial
//   accept-handle-close loop; see that class's header -- there is never
//   more than one HandleLine call against `_agentHttpAdapter` in flight
//   at once).
//
//   So there are exactly TWO caller threads that can be inside a
//   dispatcher's HandleLine at any moment: the main thread (via
//   -agentHandleLine -> `_agentDispatcher`) and the hosted-server thread
//   (via `_agentHttpServer`'s loop -> `_agentHttpAdapter`). Each
//   dispatcher/session pair is touched by exactly ONE of those threads,
//   never both -- `_agentDispatcher`/its session are main-thread-only
//   (same as every other -agentHandleLine caller before this slice);
//   `_agentHttpAdapter`/its session are hosted-server-thread-only
//   (nothing else ever calls into them). Neither dispatcher instance is
//   EVER called from more than one thread, so AgentRpcDispatcher's own
//   "single caller" contract (AgentRpc.h) holds for BOTH instances
//   independently.
//
//   What the two threads DO share is `_controller` (SceneEditController)
//   and its wrapped Job/Scene. Every mutating path either dispatcher can
//   reach -- StageProposal (external, stages only), ApplyAgentParamEdit /
//   ApplyAgentInsertChunk / ApplyAgentRemoveChunk (owner, commits
//   directly), ListProposals, ResolveProposal -- takes the controller's
//   OWN `mMutex` internally (SceneEditController.cpp; StageProposal's doc
//   spells out its own locked-capture discipline). So an external stage
//   (hosted-server thread) and an owner commit / resolve (main thread) —
//   or a concurrent GUI SetProperty edit, which routes through the SAME
//   mMutex-guarded cancel-and-park path — correctly serialize on that one
//   lock exactly as 5a/5b's TestExternalStageBaseVersionCoherentUnderConcurrency
//   already proved for the two-AgentSession-one-controller topology; this
//   slice adds a REAL second thread (a background std::thread) driving
//   that same topology instead of a same-thread test harness, but
//   introduces no new shared mutable state outside what mMutex already
//   guards. Neither AgentSession instance itself holds any lock of its
//   own beyond the per-instance mAsyncCacheMutex (render-result caching,
//   irrelevant to proposal staging) -- there is no session-level state
//   the two sessions could race on.

- (RISEAgentHostedServerInfo *)startAgentHostedServerWithLabel:(NSString *)sessionLabel {
    if (_agentHttpServerRunning) {
        return [[RISEAgentHostedServerInfo alloc]
            initOk:NO port:0 bearerToken:@""
            message:@"a hosted server is already running -- stop it first"];
    }
    if (!_controller) {
        return [[RISEAgentHostedServerInfo alloc]
            initOk:NO port:0 bearerToken:@""
            message:@"internal error: no live controller to host against"];
    }

    // Construct a SECOND, SEPARATE AgentSession over the SAME Job the
    // controller wraps -- WrapJob is non-owning (the host Job outlives
    // this session; released below in -stopAgentHostedServer, well
    // before the Job itself could go away). External authority: mutating
    // verbs STAGE rather than commit (see AgentSession::ProposePatch's
    // authority-gate doc). AttachController shares the SAME controller
    // `_agentDispatcher`'s own session is attached to, so a staged
    // proposal lands on that controller's ONE proposal queue -- the
    // queue the Owner dispatcher's list_proposals/resolve_proposal verbs
    // read/resolve.
    void* jobOpaque = [_host opaqueJobHandle];
    if (!jobOpaque) {
        return [[RISEAgentHostedServerInfo alloc]
            initOk:NO port:0 bearerToken:@""
            message:@"internal error: no live Job to host against"];
    }
    IJobPriv* pJob = static_cast<IJobPriv*>(jobOpaque);

    std::unique_ptr<RISE::Agent::AgentSession> session =
        RISE::Agent::AgentSession::WrapJob(pJob, RISE::Agent::AgentAuthority::External);
    if (!session) {
        return [[RISEAgentHostedServerInfo alloc]
            initOk:NO port:0 bearerToken:@""
            message:@"internal error: could not construct the external agent session"];
    }
    session->AttachController(_controller);

    // Secure-MCP slice 5c: label this session so every proposal it stages
    // carries a human-readable "who proposed this" string in the
    // proposals panel (see SceneEditController::AgentProposal::sessionLabel's
    // doc). "" from the caller falls back to a fixed generic label rather
    // than staging silently-unlabeled proposals -- there is exactly ONE
    // hosted-server session per bridge instance (not per-connection), so
    // a single fixed label is the honest granularity available here (see
    // the .h's doc for why a per-connection id isn't threaded further).
    const char* labelUtf8 = sessionLabel ? [sessionLabel UTF8String] : "";
    const std::string label = (labelUtf8 && labelUtf8[0]) ? std::string(labelUtf8)
                                                           : std::string("external-mcp");
    session->SetSessionLabel(label);

    // Propose autonomy is the wire-layer posture that PAIRS with External
    // authority (see AgentRpc.h's file header, "Secure-MCP slice 5b" —
    // the mutating verbs reach AgentSession, which stages; resolve_proposal
    // stays refused at the dispatcher layer for this session regardless).
    auto adapter = std::make_unique<RISE::Agent::AgentMcpAdapter>(
        std::move(session), RISE::Agent::AgentAutonomy::Propose);

    auto server = std::make_unique<RISE::Agent::AgentLoopbackHttpServer>(adapter.get());
    if (!server->Bind(/*port=*/0)) {
        return [[RISEAgentHostedServerInfo alloc]
            initOk:NO port:0 bearerToken:@""
            message:@"failed to bind a loopback port for the hosted MCP server"];
    }

    const unsigned short boundPort = server->BoundPort();
    NSString* token = [NSString stringWithUTF8String:server->ForTest_Token().c_str()];

    // The adapter now owns the session (via its internal dispatcher) --
    // this class retains only the ADAPTER, exactly like `_agentDispatcher`
    // above retains its dispatcher directly; every subsequent access goes
    // through -HandleLine, never a raw session pointer.
    _agentHttpAdapter = std::move(adapter);
    _agentHttpServer = std::move(server);
    _agentHttpServerRunning = YES;

    // Serve() blocks in a serial accept-handle loop until Stop() is
    // called -- run it on a dedicated background thread, joined by
    // -stopAgentHostedServer (and -shutdown, which calls that first).
    RISE::Agent::AgentLoopbackHttpServer* serverPtr = _agentHttpServer.get();
    _agentHttpServerThread = std::thread([serverPtr]() { serverPtr->Serve(); });

    return [[RISEAgentHostedServerInfo alloc]
        initOk:YES port:boundPort bearerToken:token message:@"hosted MCP server started"];
}

- (void)stopAgentHostedServer {
    if (!_agentHttpServerRunning) return;

    // Stop() closes the listen socket / signals the accept loop; safe to
    // call from this (the caller's) thread per AgentLoopbackHttpServer's
    // documented cross-thread contract. Join BEFORE releasing the
    // adapter/session below -- Serve()'s loop may be mid-HandleLine when
    // Stop() is called (Stop() only guarantees the loop exits promptly
    // AFTER the connection currently being served finishes; it does not
    // abort an in-flight HandleLine), so joining first guarantees no
    // external HandleLine call can still be touching the External
    // AgentSession `_agentHttpAdapter` owns / `_controller` once this
    // method returns.
    if (_agentHttpServer) {
        _agentHttpServer->Stop();
    }
    if (_agentHttpServerThread.joinable()) {
        _agentHttpServerThread.join();
    }
    _agentHttpServer.reset();
    _agentHttpAdapter.reset();
    _agentHttpServerRunning = NO;
}

- (BOOL)isAgentHostedServerRunning {
    return _agentHttpServerRunning;
}

- (NSUInteger)agentHostedServerPort {
    return (_agentHttpServerRunning && _agentHttpServer) ? _agentHttpServer->BoundPort() : 0;
}

- (NSString *)agentHostedServerToken {
    if (!_agentHttpServerRunning || !_agentHttpServer) return @"";
    return [NSString stringWithUTF8String:_agentHttpServer->ForTest_Token().c_str()];
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

@implementation RISEEnvironmentInfo {
    BOOL _hasEnvironment;
    BOOL _proceduralSky;
    BOOL _editable;
    NSString *_painterName;
    NSString *_file;
    double _scale;
    double _orientX;
    double _orientY;
    double _orientZ;
    BOOL _background;
}

- (instancetype)initWithHasEnvironment:(BOOL)hasEnvironment
                         proceduralSky:(BOOL)proceduralSky
                              editable:(BOOL)editable
                           painterName:(NSString *)painterName
                                  file:(NSString *)file
                                 scale:(double)scale
                               orientX:(double)orientX
                               orientY:(double)orientY
                               orientZ:(double)orientZ
                            background:(BOOL)background
{
    self = [super init];
    if (self) {
        _hasEnvironment = hasEnvironment;
        _proceduralSky = proceduralSky;
        _editable = editable;
        _painterName = [painterName copy] ?: @"";
        _file = [file copy] ?: @"";
        _scale = scale;
        _orientX = orientX;
        _orientY = orientY;
        _orientZ = orientZ;
        _background = background;
    }
    return self;
}

- (BOOL)hasEnvironment { return _hasEnvironment; }
- (BOOL)proceduralSky  { return _proceduralSky; }
- (BOOL)editable       { return _editable; }
- (NSString *)painterName { return _painterName; }
- (NSString *)file     { return _file; }
- (double)scale        { return _scale; }
- (double)orientX      { return _orientX; }
- (double)orientY      { return _orientY; }
- (double)orientZ      { return _orientZ; }
- (BOOL)background     { return _background; }

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

@implementation RISEViewportNavNub {
    NSInteger _axis;
    BOOL      _negative;
    CGFloat   _screenX;
    CGFloat   _screenY;
    CGFloat   _screenRadius;
    BOOL      _facing;
}

- (instancetype)_initWithAxis:(NSInteger)axis
                     negative:(BOOL)negative
                      screenX:(CGFloat)screenX
                      screenY:(CGFloat)screenY
                 screenRadius:(CGFloat)screenRadius
                       facing:(BOOL)facing
{
    self = [super init];
    if (self) {
        _axis = axis;
        _negative = negative;
        _screenX = screenX;
        _screenY = screenY;
        _screenRadius = screenRadius;
        _facing = facing;
    }
    return self;
}

- (NSInteger)axis        { return _axis; }
- (BOOL)negative         { return _negative; }
- (CGFloat)screenX       { return _screenX; }
- (CGFloat)screenY       { return _screenY; }
- (CGFloat)screenRadius  { return _screenRadius; }
- (BOOL)facing           { return _facing; }

@end
