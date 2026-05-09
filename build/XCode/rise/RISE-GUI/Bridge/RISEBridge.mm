//////////////////////////////////////////////////////////////////////
//
//  RISEBridge.mm - Objective-C++ bridge between RISE C++ engine
//  and the Swift/SwiftUI application.
//
//////////////////////////////////////////////////////////////////////

#import "RISEBridge.h"
#import "MovieRasterizerOutput.h"

// C++ RISE engine includes
#include "RISE_API.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/IProgressCallback.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/ILogPriv.h"
#include "Interfaces/IScene.h"
#include "Interfaces/ICamera.h"
#include "Utilities/RTime.h"
#include "Utilities/MediaPathLocator.h"
#include "Utilities/RenderETAEstimator.h"
#include "Utilities/Reference.h"

// L4b — ViewportFrameStore-driven viewport pipeline. Replaces the
// legacy IJobRasterizerOutput adapter (BlockRasterizerOutput) with a
// canonical-HDR-buffer flow: the rasterizer feeds pixels into a
// ViewportFrameStore (which owns an HDR FrameStore + per-tile
// shared_mutex + observer chain).  On each tile/frame callback we
// RenderToBuffer(RGBA16_sRGB) into a bridge-owned uint16 staging
// buffer and fire the existing RISEImageOutputBlock — preserving the
// Swift contract bit-for-bit while adding live exposure scrubbing
// (SetCameraExposureCompensationEV → Repaint without re-render) and
// multi-format Save-As (vfs->SaveAs(path, encoder, opts)) under the
// hood.  See docs/FRAMESTORE_DESIGN.md §11 L4b.
#include "Rendering/ViewportFrameStore.h"
#include "Rendering/FrameStore.h"
#include "Rendering/FrameEncoders.h"
#include "Rendering/TargetFormat.h"
#include "Rendering/ViewTransform.h"
#include "Interfaces/IFrameEncoder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::FrameStoreOutput;

// ============================================================
// C++ callback adapter: IProgressCallback -> ObjC block
// ============================================================
class BlockProgressCallback : public IProgressCallback {
public:
    RISEProgressBlock _block;
    std::string _currentTitle;

    BlockProgressCallback(RISEProgressBlock block) : _block(block) {}

    bool Progress(const double progress, const double total) override {
        if (_block) {
            @autoreleasepool {
                NSString *title = [NSString stringWithUTF8String:_currentTitle.c_str()];
                return (bool)_block(progress, total, title);
            }
        }
        return true;
    }

    void SetTitle(const char* title) override {
        _currentTitle = title ? title : "";
    }
};

// ============================================================
// VFS callback bridge: ViewportFrameStore -> ObjC block
// ============================================================
//
// Replaces the legacy `BlockRasterizerOutput : IJobRasterizerOutput`
// adapter.  The rasterizer now feeds the canonical HDR FrameStore
// inside a `ViewportFrameStore`; on each tile/frame observer
// callback we `RenderToBuffer(RGBA16_sRGB, ForLDRDisplay(currentEV))`
// into a bridge-owned uint16 staging buffer and fire the user's
// `RISEImageOutputBlock` — preserving the legacy producer contract
// (RGBA16 sRGB, then Swift `>> 8` to RGBA8) byte-for-byte at EV=0,
// while opening up live exposure scrubbing and multi-format Save-As
// without re-rendering.
//
// Threading: VFS observer callbacks fire from rasterizer worker
// threads (multiple may concurrently land tile callbacks).  The
// helper serialises buffer / block access on `bufferMutex_` so two
// workers can't tear each other's `RenderToBuffer` writes; the
// block call runs synchronously inside the lock so the Swift
// receiver completes its read before the next tile fires.
//
// Lifetime: bridge owns this helper; lambda captures pass a raw
// pointer.  The bridge tears down by (a) releasing its
// ViewportFrameStore reference (the rasterizer's reference is
// already gone via `FreeRasterizerOutputs()` or the rasterizer's
// dtor, joining its workers en route), then (b) deleting this
// helper.  Step (a) drains in-flight observer callbacks via the
// L1 in-flight-counter / cv-wait machinery, so by the time (b)
// runs no lambda capture is in flight.
class ViewportFrameStoreCallbacks {
public:
    explicit ViewportFrameStoreCallbacks(RISEImageOutputBlock block)
      : block_([block copy]) {}

    void SetBlock(RISEImageOutputBlock block) {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        block_ = [block copy];
    }

    // Live exposure-EV scrub.  Atomic so the rasterizer-thread
    // callback can read it without locking; the Repaint() path
    // re-runs RenderToBuffer with the new EV.
    void SetExposureEV(double ev) { exposureEV_.store(ev); }
    double ExposureEV() const     { return exposureEV_.load(); }

    // Force a repaint with the current EV.  Used by the bridge's
    // exposure-scrub path: Swift slider drag → bridge calls
    // SetExposureEV(newEV) + Repaint() → block fires with the
    // re-rendered uint16 buffer → Swift updates NSImage.  No
    // rasterizer re-run needed.  No-op until the chain is allocated
    // (i.e. at least one OutputImage has fired).
    void Repaint(Implementation::ViewportFrameStore* vfs) {
        if (!vfs) return;
        std::lock_guard<std::mutex> lock(bufferMutex_);
        EmitFullImage_locked(vfs);
    }

    // Tile-complete callback.  Renders ONLY the changed region into
    // the staging buffer at its image-space offset and fires the
    // block with that region's bounds.  The Swift consumer
    // (`RenderImageBuffer.handleOutput`) iterates only the supplied
    // bounds when downconverting RGBA16 → RGBA8, so passing the
    // tile region keeps per-tile work O(tile) rather than O(image).
    // Round-7 P1 fix for the 4× regression vs the legacy
    // CallbackRasterizerOutputDispatch path: the legacy dispatch
    // also wrote ONLY the per-tile rect into its pBuffer + fired
    // the block region-bounded; the L4b initial drop was firing
    // RenderToBuffer(full-image-roi) on every tile callback.
    void OnTileComplete(Implementation::ViewportFrameStore* vfs,
                        const RISE::Rect& halfOpenRoi,
                        uint64_t          /*generation*/) {
        if (!vfs) return;
        std::lock_guard<std::mutex> lock(bufferMutex_);
        EmitRegion_locked(vfs, halfOpenRoi);
    }

    void OnFrameComplete(Implementation::ViewportFrameStore* vfs,
                         unsigned int /*frame*/,
                         uint64_t     /*generation*/) {
        // Frame-complete fires once per frame (not per tile) so the
        // full-image cost is amortised; emit the full image to
        // catch any post-denoise / post-resolve pixels and to
        // guarantee the final image bytes are coherent end-to-end.
        if (!vfs) return;
        std::lock_guard<std::mutex> lock(bufferMutex_);
        EmitFullImage_locked(vfs);
    }

private:
    // Ensure the staging buffer matches the current FrameStore dims.
    // Returns false if the chain hasn't been allocated yet.
    bool EnsureBuffer_locked(Implementation::ViewportFrameStore* vfs,
                             unsigned int& W, unsigned int& H) {
        // GetDimensions takes chainMutex_ shared internally — safe
        // against a concurrent resolution-change reallocation in the
        // rasterizer thread (L4 round-4 P2-D).
        vfs->GetDimensions(W, H);
        if (W == 0 || H == 0) return false;
        const size_t need = static_cast<size_t>(W) * H * 4;
        if (bufW_ != W || bufH_ != H || buffer_.size() != need) {
            buffer_.assign(need, 0);
            bufW_ = W;
            bufH_ = H;
        }
        return true;
    }

    // Render a sub-region of the FrameStore into the matching slice
    // of the staging buffer + fire the block with inclusive bounds.
    // L4 round-7 P1 perf fix: per-tile work is now O(tile-area) not
    // O(image-area), matching the legacy dispatch path.
    void EmitRegion_locked(Implementation::ViewportFrameStore* vfs,
                           const RISE::Rect& halfOpenRoi) {
        unsigned int W = 0, H = 0;
        if (!EnsureBuffer_locked(vfs, W, H)) return;

        // Clip the half-open roi to image bounds defensively.
        const unsigned int y0 = halfOpenRoi.top;
        const unsigned int x0 = halfOpenRoi.left;
        const unsigned int y1 = std::min<unsigned int>(halfOpenRoi.bottom, H);
        const unsigned int x1 = std::min<unsigned int>(halfOpenRoi.right,  W);
        if (y1 <= y0 || x1 <= x0) return;

        // RenderToBuffer writes pixels at offset
        //   dst[(y - y0) * dstStride + (x - x0) * bpp]
        // (FrameStore.cpp:748-750), so to land them at their actual
        // (y, x) image coordinates we point `dst` at the (y0, x0)
        // pixel of the full-image buffer and pass the FULL row stride.
        const ViewTransform xf = ViewTransform::ForLDRDisplay(
            static_cast<float>(exposureEV_.load()),
            eDisplayTransform_None);
        uint16_t* base = buffer_.data()
                         + (static_cast<size_t>(y0) * W + x0) * 4;
        const size_t dstStride = static_cast<size_t>(W) * 4 * sizeof(uint16_t);
        vfs->RenderToBuffer(base, dstStride, halfOpenRoi,
                            TargetFormat::RGBA16_sRGB, xf);

        if (block_) {
            @autoreleasepool {
                // Inclusive bounds expected by Swift's `top...bottom`,
                // `left...right` iteration.
                block_(buffer_.data(), W, H,
                       /*top=*/y0, /*left=*/x0,
                       /*bottom=*/y1 - 1, /*right=*/x1 - 1);
            }
        }
    }

    // Render the full image — used by Repaint() (live exposure
    // scrub on the UI thread, no roi available) and OnFrameComplete
    // (per-frame final pass).  RGBA16_sRGB is L0's "Today's
    // IJobRasterizerOutput RGBA16 format".  NOT bit-identical to
    // legacy `Integerize<sRGBPel,unsigned short>(65535)`: VFS's `Q16`
    // (FrameStore.cpp:653-658) rounds-to-nearest while legacy
    // truncates — at most ±1 LSB difference, imperceptible after
    // the Swift `>> 8` to RGBA8.  See L4 round-4 P1-B review.
    void EmitFullImage_locked(Implementation::ViewportFrameStore* vfs) {
        unsigned int W = 0, H = 0;
        if (!EnsureBuffer_locked(vfs, W, H)) return;
        const ViewTransform xf = ViewTransform::ForLDRDisplay(
            static_cast<float>(exposureEV_.load()),
            eDisplayTransform_None);
        vfs->RenderToBuffer(buffer_.data(),
                            static_cast<size_t>(W) * 4 * sizeof(uint16_t),
                            RISE::Rect(0, 0, H, W),
                            TargetFormat::RGBA16_sRGB,
                            xf);
        if (block_) {
            @autoreleasepool {
                block_(buffer_.data(), W, H,
                       /*top=*/0, /*left=*/0,
                       /*bottom=*/H - 1, /*right=*/W - 1);
            }
        }
    }

    RISEImageOutputBlock  block_;
    std::mutex            bufferMutex_;
    std::vector<uint16_t> buffer_;
    unsigned int          bufW_ = 0;
    unsigned int          bufH_ = 0;
    std::atomic<double>   exposureEV_{0.0};
};

// ============================================================
// C++ callback adapter: ILogPrinter -> ObjC block
// ============================================================
class BlockLogPrinter : public ILogPrinter, public Implementation::Reference {
public:
    RISELogBlock _block;

    BlockLogPrinter(RISELogBlock block) : _block(block) {}
    virtual ~BlockLogPrinter() {}

    void Print(const LogEvent& event) override {
        if (_block && (event.eType & (eLog_Warning | eLog_Error | eLog_Fatal | eLog_Event))) {
            @autoreleasepool {
                NSString *msg = [NSString stringWithUTF8String:event.szMessage];
                RISELogLevel level = static_cast<RISELogLevel>(event.eType);
                _block(level, msg);
            }
        }
    }

    void Flush() override {}
};

// ============================================================
// RISEBridge implementation
// ============================================================
@implementation RISEBridge {
    IJobPriv* _job;
    BlockProgressCallback* _progressCallback;
    // L4b: ViewportFrameStore pipeline replaces BlockRasterizerOutput.
    // The VFS *is* the rasterizer's IRasterizerOutput; the bridge owns
    // the C++ Reference (intrusive refcount) and the rasterizer addrefs
    // it on Attach().  The callbacks helper is a plain C++ object owned
    // by the bridge — it captures the user's RISEImageOutputBlock and
    // bridges VFS observer events back to it via RenderToBuffer.
    Implementation::ViewportFrameStore* _viewportFrameStore;
    std::unique_ptr<ViewportFrameStoreCallbacks> _vfsCallbacks;
    BOOL _vfsAttachedToRasterizer;
    BlockLogPrinter* _logPrinter;
    RISEProgressBlock _progressBlock;
    RISEImageOutputBlock _imageOutputBlock;
    RISELogBlock _logBlock;
    NSString* _videoOutputPath;
    RenderETAEstimator _eta;  // fed from worker thread, sampled from UI thread
    std::mutex _etaMutex;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        srand(GetMilliseconds());

        // Write log file to user's home directory since GUI app working directory may not be writable
        NSString *logPath = [NSHomeDirectory() stringByAppendingPathComponent:@"RISE_GUI_Log.txt"];
        SetGlobalLogFileName([logPath UTF8String]);

        const char* mediaPath = getenv("RISE_MEDIA_PATH");
        if (mediaPath) {
            GlobalMediaPathLocator().AddPath(mediaPath);
        }

        _job = nullptr;
        RISE_CreateJobPriv(&_job);

        _progressCallback = nullptr;
        _viewportFrameStore = nullptr;
        // L4 round-4 P2-C — eagerly construct the VFS callbacks
        // helper here.  Previously this was lazy inside
        // -ensureVFSAttachedToRasterizer:, which made
        // -setImageOutputBlock: races dependent on init-vs-rasterize
        // ordering: a Swift caller that sets the block while a render
        // is in flight could race the unique_ptr load against the
        // lazy `_vfsCallbacks = make_unique<...>` write on the
        // rasterize-spawning thread.  Eager construction is cheap
        // (helper just owns a buffer that's allocated on first emit
        // and an atomic EV) and resolves the race entirely.
        _vfsCallbacks = std::make_unique<ViewportFrameStoreCallbacks>(nullptr);
        _vfsAttachedToRasterizer = NO;
        _logPrinter = nullptr;
    }
    return self;
}

- (void)dealloc {
    if (_job) {
        _job->SetProgress(nullptr);
    }
    if (_progressCallback) {
        delete _progressCallback;
        _progressCallback = nullptr;
    }
    // Tear-down ordering: drop the Job (which destroys the rasterizer,
    // joining its worker threads en route — by the time _job->release()
    // returns no callbacks can be in flight against `_viewportFrameStore`).
    // Then release the bridge's VFS reference (the rasterizer's reference
    // is already gone via the rasterizer's dtor) which frees the VFS,
    // its FrameStore, and its observer chain.  Only after VFS is freed
    // is it safe to destroy the callbacks helper that lambda captures
    // pointed at — the L1 in-flight-counter / cv-wait machinery
    // guarantees no lambda is mid-fire here.
    if (_logPrinter) {
        _logPrinter->release();
        _logPrinter = nullptr;
    }
    if (_job) {
        _job->release();
        _job = nullptr;
    }
    if (_viewportFrameStore) {
        // L4 round-4 P2-A defensive: nil the callbacks before
        // release so any path that re-enters them post-destruction
        // gracefully no-ops instead of dereferencing the freed
        // helper / `self`.  By this point the rasterizer's worker
        // pool is joined (it was joined inside the rasterizer's
        // dtor running from `_job->release()` above), so observer
        // dispatch is drained — but defense in depth is cheap.
        _viewportFrameStore->SetTileCompleteCallback(nullptr);
        _viewportFrameStore->SetFrameCompleteCallback(nullptr);
        _viewportFrameStore->SetPreDenoiseCompleteCallback(nullptr);
        _viewportFrameStore->SetDenoiseCompleteCallback(nullptr);
        _viewportFrameStore->release();
        _viewportFrameStore = nullptr;
    }
    _vfsCallbacks.reset();
    _vfsAttachedToRasterizer = NO;
}

+ (NSString *)versionString {
    int major = 0, minor = 0, revision = 0, build = 0;
    bool debug = false;
    RISE_API_GetVersion(&major, &minor, &revision, &build, &debug);
    return [NSString stringWithFormat:@"%d.%d.%d build %d%@",
            major, minor, revision, build, debug ? @" (DEBUG)" : @""];
}

- (void)addMediaPath:(NSString *)path {
    GlobalMediaPathLocator().AddPath([path UTF8String]);
}

- (void)setProjectRoot:(NSString *)path {
    GlobalMediaPathLocator().AddPath([path UTF8String]);

    // Append trailing slash so RISE_MEDIA_PATH concatenation works correctly
    // (FileRasterizerOutput does: mediapath + pattern, e.g. "/root/" + "rendered/file")
    NSString *rootPath = path;
    if (![rootPath hasSuffix:@"/"]) {
        rootPath = [rootPath stringByAppendingString:@"/"];
    }
    setenv("RISE_MEDIA_PATH", [rootPath UTF8String], 1);
}

- (BOOL)loadAsciiScene:(NSString *)filePath {
    if (!_job) return NO;
    return _job->LoadAsciiScene([filePath UTF8String]) ? YES : NO;
}

- (BOOL)clearAll {
    if (!_job) return NO;
    return _job->ClearAll() ? YES : NO;
}

- (void)setProgressBlock:(RISEProgressBlock)block {
    _progressBlock = [block copy];

    if (_progressCallback) {
        delete _progressCallback;
        _progressCallback = nullptr;
    }

    if (_progressBlock) {
        _progressCallback = new BlockProgressCallback(_progressBlock);
        if (_job) {
            _job->SetProgress(_progressCallback);
        }
    } else {
        if (_job) {
            _job->SetProgress(nullptr);
        }
    }
}

- (void)setImageOutputBlock:(RISEImageOutputBlock)block {
    _imageOutputBlock = [block copy];

    // The callbacks helper is created eagerly in -init (P2-C);
    // `_vfsCallbacks` is always non-null.  SetBlock locks
    // bufferMutex_ internally, so this is safe to call concurrent
    // with a rasterizer worker firing tile callbacks — the worker
    // sees either the old or the new block (atomically), never a
    // half-swapped state.  Setting nil suppresses block fires
    // (the helper still owns the buffer + VFS attachment so a later
    // setImageOutputBlock: can re-engage).
    _vfsCallbacks->SetBlock(_imageOutputBlock);
}

- (void)setViewExposureEV:(double)ev {
    // Live exposure scrubbing — adjusts the ViewTransform applied to
    // the cached HDR FrameStore on the next emit, with no rasterizer
    // re-run.  The current value is read atomically inside the
    // callbacks helper's RenderToBuffer call.  Calling Repaint() here
    // is what makes the slider feel "live" — without it the new EV
    // wouldn't take effect until the next tile/frame callback fires.
    // Returns immediately if the chain hasn't been allocated yet
    // (i.e. no render has produced output yet).
    if (!_vfsCallbacks) return;
    _vfsCallbacks->SetExposureEV(ev);
    _vfsCallbacks->Repaint(_viewportFrameStore);
}

- (BOOL)saveAs:(NSString *)path
        format:(NSString *)formatName
    exposureEV:(double)ev {
    if (!_viewportFrameStore || !path || !formatName) return NO;
    IFrameEncoder* enc =
        Implementation::FrameEncoderRegistry::Get().ByFormatName(
            [formatName UTF8String]);
    if (!enc) {
        NSLog(@"RISEBridge::saveAs: unknown format '%@'", formatName);
        return NO;
    }
    EncodeOpts opts;
    opts.colorSpace    = eColorSpace_sRGB;
    opts.bpp           = 8;
    opts.viewTransform = ViewTransform::ForLDRDisplay(
        static_cast<float>(ev), eDisplayTransform_None);
    return _viewportFrameStore->SaveAs(
        std::string([path UTF8String]), enc, opts) ? YES : NO;
}

// Lazy-create the VFS + callbacks helper, and attach to the
// current rasterizer.  Idempotent across multiple rasterize calls.
// Called from the rasterize / rasterizeAnimation / rasterizeRegion
// paths after `FreeRasterizerOutputs()` so the rasterizer's outputs
// list starts clean.  See L4 docs §11 L4b.
- (void)ensureVFSAttachedToRasterizer:(IRasterizer*)rasterizer {
    if (!rasterizer) return;

    if (!_viewportFrameStore) {
        _viewportFrameStore = new Implementation::ViewportFrameStore();
        // Reference starts at refcount=1 (Reference ctor).  Bridge
        // owns this initial reference; the rasterizer's Attach()
        // bumps it to 2.  On bridge teardown we release once,
        // matching the rasterizer-side release that comes from
        // FreeRasterizerOutputs() / rasterizer dtor.

        // The VFS callback helper is constructed eagerly in -init
        // (P2-C); `_vfsCallbacks` is guaranteed non-null.  Bind the
        // helper to the VFS observer slots once per VFS lifetime.
        // VFS's setters are not thread-safe per
        // ViewportFrameStore.h:118-122, so binding once at VFS
        // construction time (before any Attach) is the safe pattern.
        // Lambda captures: helper is owned by the bridge and outlives
        // any rasterizer-side callback (the rasterizer's dtor joins
        // its worker pool before our dealloc reaches the helper.reset()
        // step — see -dealloc ordering rationale).
        ViewportFrameStoreCallbacks* helper = _vfsCallbacks.get();
        Implementation::ViewportFrameStore* vfs = _viewportFrameStore;
        vfs->SetTileCompleteCallback(
            [helper, vfs](const RISE::Rect& roi, uint64_t gen) {
                helper->OnTileComplete(vfs, roi, gen);
            });
        vfs->SetFrameCompleteCallback(
            [helper, vfs](unsigned int frame, uint64_t gen) {
                helper->OnFrameComplete(vfs, frame, gen);
            });
        vfs->SetPreDenoiseCompleteCallback(
            [helper, vfs](unsigned int frame, uint64_t gen) {
                helper->OnFrameComplete(vfs, frame, gen);
            });
        vfs->SetDenoiseCompleteCallback(
            [helper, vfs](unsigned int frame, uint64_t gen) {
                helper->OnFrameComplete(vfs, frame, gen);
            });
    }

    // (Re-)attach.  Each rasterize call begins with FreeRasterizerOutputs
    // which dropped the rasterizer's reference; we reattach here.  Attach
    // is also safe to call repeatedly without a Free in between (it'll
    // just stack a redundant reference) — the BOOL guard avoids that
    // case for clarity.
    if (!_vfsAttachedToRasterizer) {
        _viewportFrameStore->Attach(rasterizer);
        _vfsAttachedToRasterizer = YES;
    }
}

- (void)setLogBlock:(RISELogBlock)block {
    _logBlock = [block copy];

    // Remove old printer from the global log
    if (_logPrinter) {
        // We can't selectively remove, so we just release our reference.
        // The printer will stop forwarding once the block is nil.
        _logPrinter->_block = nil;
        _logPrinter->release();
        _logPrinter = nullptr;
    }

    if (_logBlock) {
        _logPrinter = new BlockLogPrinter(_logBlock);
        _logPrinter->addref();
        GlobalLogPriv()->AddPrinter(_logPrinter);
    }
}

- (BOOL)hasAnimatedObjects {
    if (!_job) return NO;
    return _job->AreThereAnyKeyframedObjects() ? YES : NO;
}

- (uint32_t)cameraWidth {
    if (!_job) return 0;
    IScenePriv* scene = _job->GetScene();
    if (!scene) return 0;
    const ICamera* camera = scene->GetCamera();
    if (!camera) return 0;
    return camera->GetWidth();
}

- (uint32_t)cameraHeight {
    if (!_job) return 0;
    IScenePriv* scene = _job->GetScene();
    if (!scene) return 0;
    const ICamera* camera = scene->GetCamera();
    if (!camera) return 0;
    return camera->GetHeight();
}

- (void)setSceneTime:(double)t {
    if (!_job) return;
    IScenePriv* scene = _job->GetScene();
    if (!scene) return;
    // Full SetSceneTime: advances the animator AND regenerates every
    // populated photon map at time `t`.  The interactive scrub path
    // calls SetSceneTimeForPreview (animator-only, no photon regen)
    // for responsiveness; this method runs the expensive photon
    // regen so the next production render gets caustics consistent
    // with the scrubbed scene state.  Production rasterizers may
    // wait minutes here on photon-heavy scenes — the caller should
    // already be in a "rendering" UI state.
    scene->SetSceneTime(t);
}

- (BOOL)rasterize {
    if (!_job) return NO;

    // Clear previous rasterizer outputs before reattaching VFS.
    // Without this, every Render click stacks another rasterizer-side
    // reference (and on the legacy CallbackRasterizerOutputDispatch
    // path also stacked dispatcher objects) onto the production
    // rasterizer's outs list, producing N callback fires per pass.
    // FreeRasterizerOutputs() drops the rasterizer's VFS reference;
    // the bridge's reference keeps VFS alive across Free → Attach.
    IRasterizer* rasterizer = _job->GetRasterizer();
    if (rasterizer) {
        rasterizer->FreeRasterizerOutputs();
        _vfsAttachedToRasterizer = NO;
    }

    [self ensureVFSAttachedToRasterizer:rasterizer];

    BOOL result = _job->Rasterize() ? YES : NO;
    return result;
}

- (void)setAnimationVideoOutputPath:(NSString *)path {
    _videoOutputPath = [path copy];
}

- (BOOL)rasterizeAnimation {
    if (!_job) return NO;

    IRasterizer* rasterizer = _job->GetRasterizer();
    if (!rasterizer) return NO;

    // Clear outputs from previous renders to prevent accumulation of
    // callback dispatchers and old movie outputs.
    rasterizer->FreeRasterizerOutputs();
    _vfsAttachedToRasterizer = NO;

    [self ensureVFSAttachedToRasterizer:rasterizer];

    // Create and attach video output if a path was configured.
    // Reference starts at refcount=1 (from Reference ctor).
    // AddRasterizerOutput calls addref(), bringing it to 2.
    // We release our creation reference immediately, leaving the
    // rasterizer as sole owner (refcount=1).
    MovieRasterizerOutput* movieOutput = nullptr;
    if (_videoOutputPath) {
        movieOutput = new MovieRasterizerOutput(_videoOutputPath);
        rasterizer->AddRasterizerOutput(movieOutput);
        movieOutput->release();  // rasterizer now owns it (refcount=1)
    }

    BOOL result = _job->RasterizeAnimationUsingOptions() ? YES : NO;

    // Finalize the video file after rendering completes.
    // The movieOutput pointer is still valid because the rasterizer holds a reference.
    if (movieOutput) {
        movieOutput->finalize();
    }

    // Now free all outputs, which will release the rasterizer's reference
    // and destroy the movie output (refcount drops to 0).
    rasterizer->FreeRasterizerOutputs();

    return result;
}

- (BOOL)rasterizeRegionLeft:(uint32_t)left
                        top:(uint32_t)top
                      right:(uint32_t)right
                     bottom:(uint32_t)bottom {
    if (!_job) return NO;

    // Same accumulation hygiene as `rasterize:` — clear outputs so
    // the rasterizer's outs list doesn't grow unbounded across
    // repeat renders.
    IRasterizer* rasterizer = _job->GetRasterizer();
    if (rasterizer) {
        rasterizer->FreeRasterizerOutputs();
        _vfsAttachedToRasterizer = NO;
    }

    [self ensureVFSAttachedToRasterizer:rasterizer];

    return _job->RasterizeRegion(left, top, right, bottom) ? YES : NO;
}

#pragma mark - Render-time ETA estimator

- (void)etaBegin {
    std::lock_guard<std::mutex> lock(_etaMutex);
    _eta.Begin();
}

- (void)etaUpdateProgress:(double)progress total:(double)total {
    std::lock_guard<std::mutex> lock(_etaMutex);
    _eta.Update(progress, total);
}

- (double)etaElapsedSeconds {
    std::lock_guard<std::mutex> lock(_etaMutex);
    return _eta.ElapsedSeconds();
}

- (nullable NSNumber *)etaRemainingSeconds {
    std::lock_guard<std::mutex> lock(_etaMutex);
    double s = 0.0;
    if (!_eta.RemainingSeconds(s)) return nil;
    return [NSNumber numberWithDouble:s];
}

+ (NSString *)formatDuration:(double)seconds {
    const std::string s = RenderETAEstimator::FormatDuration(seconds);
    return [NSString stringWithUTF8String:s.c_str()];
}

- (void *)opaqueJobHandle {
    // Returns the IJobPriv* as an opaque pointer.  The viewport bridge
    // casts this back to IJobPriv* in its .mm.  Lifetime is tied to
    // this RISEBridge — the viewport must not outlive us.
    return static_cast<void *>(_job);
}

@end
