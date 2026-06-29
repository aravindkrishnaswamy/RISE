//////////////////////////////////////////////////////////////////////
//
//  RISEBridge.mm - Objective-C++ bridge between RISE C++ engine
//  and the Swift/SwiftUI application.
//
//////////////////////////////////////////////////////////////////////

#import "RISEBridge.h"
#import "MovieRasterizerOutput.h"

// L5a — NSScreen.maximumExtendedDynamicRangeColorComponentValue
// for headroom detection.  AppKit pulls in CGColorSpace
// (kCGColorSpaceExtendedLinearSRGB is documented but consumed on
// the Swift / Metal side in MetalEDRView, not here).
#import <AppKit/AppKit.h>

// C++ RISE engine includes
#include "RISE_API.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/IProgressCallback.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/ILogPriv.h"
#include "Interfaces/IScene.h"
#include "Interfaces/ICamera.h"
#include "Interfaces/IFilm.h"
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

    // L5a — HDR (extended-linear-sRGB binary16) block.  See
    // RISEBridge.h documentation on RISEHDRImageOutputBlock.
    // SetHDRBlock + SetHDREnabled are the dual of SetBlock + the
    // implicit LDR mode.  When both blocks are set and HDR is ON,
    // only the HDR block fires.
    void SetHDRBlock(RISEHDRImageOutputBlock block) {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        hdrBlock_ = [block copy];
    }

    void SetHDREnabled(bool enabled) {
        // Atomic so the rasterizer-thread callback can read it
        // without locking; the Repaint() path re-runs
        // RenderToBuffer at the new format on the UI thread.
        // Buffer realloc on mode change is handled by
        // EnsureBuffer_locked picking up `bytesPerPixel` from the
        // current TargetFormatInfo.
        hdrEnabled_.store(enabled);
    }
    bool HDREnabled() const { return hdrEnabled_.load(); }

    // Live exposure-EV scrub.  Atomic so the rasterizer-thread
    // callback can read it without locking; the Repaint() path
    // re-runs RenderToBuffer with the new EV.
    void SetExposureEV(double ev) { exposureEV_.store(ev); }
    double ExposureEV() const     { return exposureEV_.load(); }

    // L5e — Live tone-curve scrub for the LDR display path.  Same
    // atomic-snapshot pattern as exposure: rasterizer-thread reads
    // it via `ToneCurve()` inside `SelectTargetFormatAndXform`, the
    // UI thread writes it via `SetToneCurve` then calls Repaint to
    // get a fresh emit.  Default ACES (the modern preview standard;
    // `_None` was the legacy bit-equivalent default kept while we
    // were chasing L4 regression coverage, no longer needed).
    void SetToneCurve(int curve) {
        toneCurve_.store(curve);
    }
    int ToneCurve() const { return toneCurve_.load(); }

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
        // L8 round 9 — sync the polling sentinel so a subsequent
        // Poll() call doesn't re-emit the same generation.
        lastSeenGeneration_ = vfs->Generation();
    }

    // L8 round 9 — Lockless progressive-update path.
    //
    // Replaces the synchronous per-tile `OnTileComplete` callback
    // that previously fired from rasterizer worker threads.  That
    // path acquired `bufferMutex_` from every worker for ~85 μs per
    // tile and serialised them all on a single mutex.  Combined
    // with the synchronous `vfs->RenderToBuffer` call inside the
    // lock (which itself takes per-tile `shared_lock` on the
    // FrameStore), it produced a `bufferMutex_ ↔ tile-mutex`
    // inversion when two worker blocks landed in the same FrameStore
    // tile (pre round-8 tile-alignment fix) and ~4% wall-clock
    // overhead from observer serialisation even when no inversion
    // fired.
    //
    // Post round-9: workers no longer call into the bridge from
    // `EndTile`.  Instead, the UI thread polls `vfs->Generation()`
    // (a `std::atomic<uint64_t>` already bumped on every EndTile)
    // at its own cadence — typically a 30 Hz `NSTimer` driven from
    // the Swift `RenderViewModel`.  When the generation advances,
    // `PollAndEmitIfDirty` does a single full-frame
    // `EmitFullImage_locked` on the UI thread.  Net properties:
    //   * Workers: zero `bufferMutex_` contention; render time is
    //     independent of bridge / Swift / Cocoa observer work.
    //   * UI thread: bounded ~15% utilisation during render
    //     (~5 ms × 30 Hz emit cost); idle when generation hasn't
    //     advanced.
    //   * Visual: smooth 30 fps update of the in-progress image.
    //     Per-pixel cadence is whatever the workers produce; UI
    //     reads at 30 Hz.
    //
    // No-op when `vfs` is null or its generation hasn't changed
    // since the last call.  Safe to call repeatedly (cheap when
    // nothing is dirty).
    void PollAndEmitIfDirty(Implementation::ViewportFrameStore* vfs) {
        if (!vfs) return;
        const uint64_t gen = vfs->Generation();
        if (gen == lastSeenGeneration_) return;  // no new pixels
        std::lock_guard<std::mutex> lock(bufferMutex_);
        // L8 round 14 — `nonBlocking=true`.  A slow per-pixel block
        // on a worker can hold a tile's exclusive lock for seconds;
        // a blocking RenderToBuffer would beachball the poll thread
        // (which is the Mac main thread, driving the UI run loop)
        // for that whole duration.  Non-blocking variant skips
        // contended tiles — their staging-buffer bytes keep the
        // previous poll's content, and the next poll within 100 ms
        // catches them when the worker's intra-block flush
        // (round 11) briefly releases the tile.
        EmitFullImage_locked(vfs, /*nonBlocking=*/true);
        lastSeenGeneration_ = gen;
    }

    // Tile-complete callback.  Renders ONLY the changed region into
    // the staging buffer at its image-space offset and fires the
    // block with that region's bounds.  The Swift consumer
    // (`RenderImageBuffer.handleOutput`) iterates only the supplied
    // bounds when downconverting RGBA16 → RGBA8, so passing the
    // tile region keeps per-tile work O(tile) rather than O(image).
    void OnTileComplete(Implementation::ViewportFrameStore* vfs,
                        const RISE::Rect& halfOpenRoi,
                        uint64_t          /*generation*/) {
        if (!vfs) return;
        std::lock_guard<std::mutex> lock(bufferMutex_);
        EmitRegion_locked(vfs, halfOpenRoi);
    }

    // L8 round 17 — synchronous per-tile observer dispatch with
    // try_lock on bufferMutex_.  Wired into the production VFS's
    // SetTileCompleteCallback so the worker thread that just fired
    // EndTile gets a chance to push the tile-decorated pixels to
    // the UI *immediately* — specifically to make the red toggle
    // markers (DrawToggles, painted before the per-pixel loop)
    // visible during the round-13 split-bracket's EndTile→BeginTile
    // window.
    //
    // The previous design (round 9 — bridge tileCb_ left unwired)
    // relied on a 30 Hz polling timer to catch toggle-decorated
    // FrameStore state, but the unlock window between split-EndTile
    // and split-BeginTile is microseconds — a 33 ms poll almost
    // never hits it.  Wiring this callback runs synchronously on
    // the worker thread, so it executes *while* the tile is still
    // unlocked.
    //
    // Why try_lock (not blocking lock) on bufferMutex_:
    //   * The background pollQueue (round 16) holds bufferMutex_
    //     during its full-image emits — ~5-20 ms per tick.  If the
    //     worker observer blocked on bufferMutex_ while the bg poll
    //     was running, every worker would wait ~20 ms per tile per
    //     block.  At 100 blocks × 4 tiles per render that's
    //     800 × 20 ms = 16 s of serialised wait — catastrophic.
    //   * try_lock + skip-on-busy means: if the bg poll is busy
    //     when this fires, the toggle for this tile is missed
    //     (the bg poll will catch the FINAL pixels at its next
    //     tick).  Probability of catching the toggle is roughly
    //     (33 - bg_poll_duration) / 33, typically 60-85%.
    //   * Workers don't block.  Per-tile observer cost is
    //     ~bufferMutex_ try_lock + EmitRegion_locked (~50-100 µs
    //     per tile) only when the lock is acquired; near-zero
    //     when skipped.
    //
    // Why this doesn't re-introduce the round-1 lock inversion:
    //   * The inversion required Worker A (in observer, holding
    //     bufferMutex_) to wait on `shared_lock(Worker B's tile)`
    //     while Worker B (holding its tile exclusive) waited on
    //     bufferMutex_.  A's observer only reads A's OWN tile (the
    //     one that just unlocked via EndTile); it never tries to
    //     read B's tile.  So A's `shared_lock(A's tile)` succeeds
    //     immediately (no writer holds it — A just released it),
    //     and the cycle doesn't form.
    //   * If A's emit path were to read multiple tiles (e.g., a
    //     full-image emit), inversion could re-form.
    //     `EmitRegion_locked` strictly reads only the tile passed
    //     in `halfOpenRoi`, so the assumption holds.
    void OnTileCompleteTry(Implementation::ViewportFrameStore* vfs,
                           const RISE::Rect& halfOpenRoi,
                           uint64_t          /*generation*/) {
        if (!vfs) return;
        std::unique_lock<std::mutex> lock(bufferMutex_, std::try_to_lock);
        if (!lock.owns_lock()) return;  // bg poll busy, skip
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
    // Buffer is sized in uint16_t units; both `RGBA16_sRGB` (LDR)
    // and `RGBA16F_ExtendedLinearSRGB` (HDR) are 8 bytes/pixel = 4
    // uint16 per pixel, so the same buffer fits both modes.
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

    // L5a — pick (TargetFormat, ViewTransform) for the supplied
    // mode snapshot.  HDR mode emits binary16 in extended-linear-
    // sRGB (no tone curve, no gamma, values can exceed 1.0); LDR
    // mode emits uint16 sRGB (tone-curve + sRGB transfer applied).
    //
    // L5a round-2 P2-A fix — `useHDR` is a snapshot taken ONCE per
    // emit (at the start of EmitRegion_locked / EmitFullImage_locked)
    // and used for both the encode (here) and the block dispatch
    // (`FireBlock_locked` below).  The previous code read
    // `hdrEnabled_.load()` independently in both places, so a UI
    // toggle landing between RenderToBuffer and FireBlock_locked
    // could deliver bytes encoded in one format through the
    // callback expecting the other — uint16 sRGB bytes
    // re-interpreted as half-floats by the Metal path, or vice
    // versa.  Snapshot-once eliminates the window without needing
    // setHDREnabled to participate in bufferMutex_.
    static void SelectTargetFormatAndXform(
        bool useHDR, float ev, int toneCurveInt,
        TargetFormat& outFmt, ViewTransform& outXform) {
        if (useHDR) {
            outFmt   = TargetFormat::RGBA16F_ExtendedLinearSRGB;
            // ForHDRDisplay: exposure applied, NO tone curve (HDR
            // highlights propagate to display unmolested).  The
            // toneCurve argument is intentionally ignored in this
            // branch; the OS compositor handles the display-side map.
            outXform = ViewTransform::ForHDRDisplay(ev);
        } else {
            outFmt   = TargetFormat::RGBA16_sRGB;
            // L5e — bake in the user-selected tone curve (default
            // ACES, see toneCurve_'s in-class initialiser below).
            // Was previously hardcoded to eDisplayTransform_None
            // (legacy clip-at-1.0 behaviour kept for L4 byte-
            // identical regression coverage); now drops through
            // to whatever the user picked from View > Tone Curve.
            DISPLAY_TRANSFORM tc = static_cast<DISPLAY_TRANSFORM>(toneCurveInt);
            outXform = ViewTransform::ForLDRDisplay(ev, tc);
        }
    }

    // Fire whichever block matches `useHDR` — snapshot taken at
    // the start of the emit alongside the format selection above.
    // Caller holds bufferMutex_, so block_ / hdrBlock_ are stable
    // through the call.
    void FireBlock_locked(bool useHDR,
                          unsigned int W, unsigned int H,
                          unsigned int top, unsigned int left,
                          unsigned int bottom, unsigned int right) {
        @autoreleasepool {
            if (useHDR) {
                if (hdrBlock_) {
                    hdrBlock_(buffer_.data(), W, H, top, left, bottom, right);
                }
            } else {
                if (block_) {
                    block_(buffer_.data(), W, H, top, left, bottom, right);
                }
            }
        }
    }

    // Render a sub-region of the FrameStore into the matching slice
    // of the staging buffer + fire the block with inclusive bounds.
    // L4 round-7 P1 perf fix: per-tile work is now O(tile-area) not
    // O(image-area), matching the legacy dispatch path.
    // L5a: format + view-transform now selected by SelectTargetFormat-
    // AndXform (LDR vs HDR-EDR).  Buffer fmt+stride are 8bpp either
    // way, so the same staging buffer fits both modes.
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
        // L5a round-2 P2-A: snapshot HDR mode + EV ONCE so encode
        // (RenderToBuffer) and dispatch (FireBlock_locked) agree
        // even if setHDREnabled lands in between.
        const bool useHDR    = hdrEnabled_.load();
        const float ev       = static_cast<float>(exposureEV_.load());
        const int   tcInt    = toneCurve_.load();
        TargetFormat fmt; ViewTransform xf;
        SelectTargetFormatAndXform(useHDR, ev, tcInt, fmt, xf);
        uint16_t* base = buffer_.data()
                         + (static_cast<size_t>(y0) * W + x0) * 4;
        const size_t dstStride = static_cast<size_t>(W) * 4 * sizeof(uint16_t);
        vfs->RenderToBuffer(base, dstStride, halfOpenRoi, fmt, xf);
        FireBlock_locked(useHDR, W, H,
                         /*top=*/y0, /*left=*/x0,
                         /*bottom=*/y1 - 1, /*right=*/x1 - 1);
    }

    // Render the full image — used by Repaint() (live exposure
    // scrub on the UI thread, no roi available) and OnFrameComplete
    // (per-frame final pass).  Format selected by HDR toggle:
    // - LDR: `RGBA16_sRGB` (uint16 fixed sRGB).  NOT bit-identical to
    //   legacy `Integerize<sRGBPel,unsigned short>(65535)`: VFS's
    //   `Q16` (FrameStore.cpp:653-658) rounds-to-nearest while
    //   legacy truncates — at most ±1 LSB after Swift's `>> 8` to
    //   RGBA8, imperceptible.  See L4 round-4 P1-B review.
    // - HDR: `RGBA16F_ExtendedLinearSRGB` (binary16 half-float).
    //   Linear sRGB primaries with no transfer applied; values
    //   can exceed 1.0 for highlights past SDR clip.  Consumed by
    //   a Swift CAMetalLayer with
    //   colorspace = kCGColorSpaceExtendedLinearSRGB.
    // L8 round 14 — `nonBlocking` opt-in for the polling path so a
    // slow worker block doesn't beachball the bridge thread.  When
    // true, RenderToBuffer uses `try_lock_shared` per tile and
    // skips contended tiles; the staging buffer retains its prior
    // contents for those tiles (a previous emit's bytes), and the
    // next poll within 100 ms catches them as workers flush.
    //
    // Default `false` for Repaint / OnFrameComplete callers — they
    // need a complete coherent snapshot (slider scrub, end-of-render
    // final frame).
    void EmitFullImage_locked(Implementation::ViewportFrameStore* vfs,
                              bool nonBlocking = false) {
        unsigned int W = 0, H = 0;
        if (!EnsureBuffer_locked(vfs, W, H)) return;
        // L5a round-2 P2-A: same mode-snapshot pattern as
        // EmitRegion_locked — snapshot once so encode + dispatch
        // agree across a concurrent setHDREnabled.
        const bool useHDR    = hdrEnabled_.load();
        const float ev       = static_cast<float>(exposureEV_.load());
        const int   tcInt    = toneCurve_.load();
        TargetFormat fmt; ViewTransform xf;
        SelectTargetFormatAndXform(useHDR, ev, tcInt, fmt, xf);
        vfs->RenderToBuffer(buffer_.data(),
                            static_cast<size_t>(W) * 4 * sizeof(uint16_t),
                            RISE::Rect(0, 0, H, W),
                            fmt, xf, nonBlocking);
        FireBlock_locked(useHDR, W, H,
                         /*top=*/0, /*left=*/0,
                         /*bottom=*/H - 1, /*right=*/W - 1);
    }

    RISEImageOutputBlock     block_;     // LDR (RGBA16 sRGB)
    RISEHDRImageOutputBlock  hdrBlock_;  // HDR (binary16 extended-linear-sRGB)
    std::mutex            bufferMutex_;
    std::vector<uint16_t> buffer_;
    unsigned int          bufW_ = 0;
    unsigned int          bufH_ = 0;
    std::atomic<double>   exposureEV_{0.0};
    std::atomic<bool>     hdrEnabled_{false};
    // L5e — LDR view tone curve, default ACES (matches the modern
    // preview-standard convergent across Blender / Karma / Maya
    // Arnold).  Stored as int so the UI can pass through the
    // bridge's @interface without leaking the DISPLAY_TRANSFORM
    // enum into the public bridge header.  Cast back to enum at
    // consumption inside SelectTargetFormatAndXform.
    std::atomic<int>      toneCurve_{static_cast<int>(eDisplayTransform_ACES)};

    // L8 round 9 — sentinel for the lockless polling path.
    // Read + written ONLY on the UI thread that drives Poll() — no
    // synchronisation needed.  Initialised to 0 so the first poll
    // unconditionally emits (matches the workers' generation, which
    // starts at 0 too and bumps on the first EndTile).  Reset
    // implicitly by Repaint(), which syncs to the current generation
    // after its full-image emit so the next poll doesn't redo the
    // same work.
    uint64_t lastSeenGeneration_ = 0;
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
    // L5a round-5 — TWO independent ViewportFrameStores, one per
    // path.  Production VFS is for the full-quality renderer
    // (Render / Render-Animation buttons), with per-tile updates
    // and a downstream-consumer-friendly observer chain.
    // Interactive VFS is for the live-preview rasterizer driven
    // by the SceneEditController, with frame-complete-only
    // observer fires (no per-tile DrawToggles flash) and an
    // independent resolution lifecycle (preview-scale thrash
    // reallocates only this VFS).
    //
    // Each VFS has its own callbacks helper bound to a separate
    // pair of blocks (production-LDR / production-HDR vs
    // interactive-LDR / interactive-HDR).  This separation:
    //   * Ensures cancelling a production render preserves
    //     production pixels (interactive output writes into a
    //     different FrameStore).
    //   * Prevents per-tile interactive output from triggering
    //     observer fires that drive the Metal layer.
    //   * Decouples resolution lifecycles so the interactive
    //     preview-scale state machine doesn't churn production's
    //     FrameStore allocations.
    Implementation::ViewportFrameStore* _productionVFS;
    Implementation::ViewportFrameStore* _interactiveVFS;
    std::unique_ptr<ViewportFrameStoreCallbacks> _productionVFSCallbacks;
    std::unique_ptr<ViewportFrameStoreCallbacks> _interactiveVFSCallbacks;
    BOOL _productionVFSAttachedToRasterizer;
    BlockLogPrinter* _logPrinter;
    RISEProgressBlock _progressBlock;
    // Production blocks (LDR + HDR).  Fire from the production
    // rasterizer's tile/frame events through `_productionVFS`.
    RISEImageOutputBlock _imageOutputBlock;
    RISEHDRImageOutputBlock _hdrImageOutputBlock;
    // Interactive blocks (LDR + HDR).  Fire ONLY from frame-complete
    // events on the interactive rasterizer through `_interactiveVFS`.
    RISEImageOutputBlock _interactiveImageOutputBlock;
    RISEHDRImageOutputBlock _interactiveHDRImageOutputBlock;
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

        // L5d — interactive GUI: drop file_rasterizeroutput chunks
        // at parse time so loading a scene doesn't auto-write
        // PNG/EXR/etc. on every Render click.  Users save via
        // File > Save Rendered Image (writes to a user-picked path
        // through the L2 IFrameEncoder pipeline).
        if (_job) {
            _job->SetSuppressFileRasterizerOutputs(true);
        }

        _progressCallback = nullptr;
        _productionVFS = nullptr;
        _interactiveVFS = nullptr;
        // L4 round-4 P2-C — eagerly construct both VFS callbacks
        // helpers here so block setters never race a lazy init on
        // the rasterize thread.  Cheap construction (just the
        // buffer-mutex + atomic ev + the to-be-set blocks).
        _productionVFSCallbacks = std::make_unique<ViewportFrameStoreCallbacks>(nullptr);
        _interactiveVFSCallbacks = std::make_unique<ViewportFrameStoreCallbacks>(nullptr);
        _productionVFSAttachedToRasterizer = NO;
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
    // returns no callbacks can be in flight against either VFS).
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
    // L4 round-4 P2-A defensive: nil the callbacks before release
    // so any path that re-enters them post-destruction gracefully
    // no-ops instead of dereferencing the freed helper / `self`.
    // By this point the rasterizer's worker pool is joined (via
    // `_job->release()` above), so observer dispatch is drained —
    // but defense in depth is cheap.  Round-5: same treatment for
    // both VFSes.
    auto teardownVFS = [](Implementation::ViewportFrameStore*& vfs) {
        if (vfs) {
            vfs->SetTileCompleteCallback(nullptr);
            vfs->SetFrameCompleteCallback(nullptr);
            vfs->SetPreDenoiseCompleteCallback(nullptr);
            vfs->SetDenoiseCompleteCallback(nullptr);
            vfs->release();
            vfs = nullptr;
        }
    };
    teardownVFS(_productionVFS);
    teardownVFS(_interactiveVFS);
    _productionVFSCallbacks.reset();
    _interactiveVFSCallbacks.reset();
    _productionVFSAttachedToRasterizer = NO;
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
    const char* path = [filePath UTF8String];
    // P5 (Model-B): RISE_LOAD_VIA_CST opts the GUI into the canonical CST path (retains the CST Document,
    // bakes scene_variant overrides) instead of the legacy streaming parser.  No silent fallback -- a
    // non-native-v7 / derive-error scene returns NO so the failure is visible.  Pending Slice-5 default-on.
    if (getenv("RISE_LOAD_VIA_CST")) {
        NSLog(@"[RISE_LOAD_VIA_CST] loading via the Model-B CST path");
        return _job->LoadAsciiSceneViaCst(path) ? YES : NO;
    }
    return _job->LoadAsciiScene(path) ? YES : NO;
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
    // L5a round-5 — production-LDR slot.  See header doc.
    _imageOutputBlock = [block copy];
    _productionVFSCallbacks->SetBlock(_imageOutputBlock);
}

- (void)setHDRImageOutputBlock:(RISEHDRImageOutputBlock)block {
    // L5a round-5 — production-HDR slot.  See header doc.
    _hdrImageOutputBlock = [block copy];
    _productionVFSCallbacks->SetHDRBlock(_hdrImageOutputBlock);
}

- (void)setInteractiveImageOutputBlock:(RISEImageOutputBlock)block {
    // L5a round-5 — interactive-LDR slot.  Fires only on
    // frame-complete events from the live-preview rasterizer.
    _interactiveImageOutputBlock = [block copy];
    _interactiveVFSCallbacks->SetBlock(_interactiveImageOutputBlock);
}

- (void)setInteractiveHDRImageOutputBlock:(RISEHDRImageOutputBlock)block {
    // L5a round-5 — interactive-HDR slot.  Same conditions as
    // above but binary16 / extended-linear-sRGB for EDR layers.
    _interactiveHDRImageOutputBlock = [block copy];
    _interactiveVFSCallbacks->SetHDRBlock(_interactiveHDRImageOutputBlock);
}

- (void)setHDREnabled:(BOOL)enabled {
    // Toggle the HDR pipeline on BOTH callback helpers — production
    // and interactive switch in lockstep.
    const bool e = enabled ? true : false;
    _productionVFSCallbacks->SetHDREnabled(e);
    _interactiveVFSCallbacks->SetHDREnabled(e);

    if (e) {
        // Toggling ON: Repaint each VFS so the freshly-attached
        // Metal layer picks up the latest cached pixels without
        // waiting for the next bridge fire — slider-style live
        // response.
        _productionVFSCallbacks->Repaint(_productionVFS);
        _interactiveVFSCallbacks->Repaint(_interactiveVFS);
    }
    // Toggling OFF (round-7 fix): do NOT Repaint.  Repaint would
    // fire each helper's LDR block at the current FrameStore
    // contents — including production's, which is wired to write
    // `renderedImage`.  That would clobber whatever the
    // interactive viewport's `BlitWholeAndDispatch` (legacy
    // NSImage path, fires unconditionally) had just set.  The
    // user's complaint: drag from HDR (interactive showing) → SDR
    // (NSImage takes over) was showing the production image
    // because production's Repaint ran last and overwrote
    // interactive.  Skipping the toggle-off Repaint preserves
    // the most-recent BlitWholeAndDispatch state in
    // `renderedImage`, which matches what the user was viewing
    // on the HDR display when interactive was the active
    // renderer.  Production's HDR block still drives its own
    // Metal layer when EDR is on; we just don't backfill an
    // LDR copy on EDR-off transitions.
}

- (float)displayMaxEDRHeadroom {
    // Query the active screen's EDR CAPABILITY (max potential
    // headroom).  Returns 1.0 (SDR) for screens that lack EDR or
    // when no screen is available (headless / pre-window-show);
    // >1.0 indicates EDR-capable (e.g. ~16.0 on a Pro Display
    // XDR; ~1.6 on a recent MacBook Pro mini-LED display in
    // SDR-Reference mode, ~5.5 in HDR mode).
    //
    // L5a round-3: switched from
    // `maximumExtendedDynamicRangeColorComponentValue` (CURRENT
    // headroom, gated on the OS having actually transitioned the
    // screen into EDR mode) to
    // `maximumPotentialExtendedDynamicRangeColorComponentValue`
    // (CAPABILITY — the maximum the screen could ever display).
    // The capability check is what we want for toggle availability:
    // setting `CAMetalLayer.wantsExtendedDynamicRangeContent = YES`
    // signals the OS to transition the screen when EDR content is
    // actually present, so the user shouldn't need to first enable
    // HDR in System Settings just to make the toggle clickable.
    NSScreen* screen = [NSScreen mainScreen];
    if (!screen) return 1.0f;
    return static_cast<float>(screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
}

- (void)setViewExposureEV:(double)ev {
    // Live exposure scrubbing — adjusts the ViewTransform applied
    // to the cached HDR FrameStores on the next emit, with no
    // rasterizer re-run.  Round-5: broadcast to BOTH helpers and
    // repaint both VFSes so the slider drives both the production
    // texture AND the interactive overlay layer.  No-op for either
    // VFS that hasn't been chain-allocated yet.
    _productionVFSCallbacks->SetExposureEV(ev);
    _interactiveVFSCallbacks->SetExposureEV(ev);
    _productionVFSCallbacks->Repaint(_productionVFS);
    _interactiveVFSCallbacks->Repaint(_interactiveVFS);
}

- (void)setViewToneCurve:(int)curve {
    // L5e — broadcast tone curve to both helpers and repaint.
    // Same lifecycle as setViewExposureEV: no rasterizer re-run,
    // no-op for VFSes that haven't allocated yet.  Tone curve is
    // ignored on the HDR display path (ForHDRDisplay always uses
    // _None internally) but Repaint is still safe — it just
    // re-emits the same HDR bytes.
    _productionVFSCallbacks->SetToneCurve(curve);
    _interactiveVFSCallbacks->SetToneCurve(curve);
    _productionVFSCallbacks->Repaint(_productionVFS);
    _interactiveVFSCallbacks->Repaint(_interactiveVFS);
}

// L8 round 9 — UI-thread polling entry points.  Driven by the Swift
// side's display Timer at ~30 Hz during a render.  Each call:
//   * Reads `vfs->Generation()` (atomic, lock-free).
//   * No-ops if the generation matches the last-emitted sentinel —
//     workers haven't produced new pixels since the previous poll.
//   * Otherwise acquires `bufferMutex_` (uncontended in this design
//     — workers no longer take it during their hot path), emits one
//     full-image RenderToBuffer + block dispatch, updates the
//     sentinel.
//
// The work happens on whatever thread Swift calls from — typically
// the main run loop driving the display Timer.  Workers never wait
// on the bridge for observer dispatch, so wall-clock render time is
// independent of UI / Cocoa work.  See `PollAndEmitIfDirty` header
// doc for the architecture rationale.
- (void)pollProductionVFS {
    _productionVFSCallbacks->PollAndEmitIfDirty(_productionVFS);
}
- (void)pollInteractiveVFS {
    _interactiveVFSCallbacks->PollAndEmitIfDirty(_interactiveVFS);
}

- (BOOL)saveAs:(NSString *)path
        format:(NSString *)formatName
    exposureEV:(double)ev {
    // Save-As targets the PRODUCTION VFS — that's where the
    // full-quality, multi-output rendered result lives.  Saving
    // from the interactive (live-preview) VFS would dump the
    // low-quality preview, not the production result.
    if (!_productionVFS || !path || !formatName) return NO;
    IFrameEncoder* enc =
        Implementation::FrameEncoderRegistry::Get().ByFormatName(
            [formatName UTF8String]);
    if (!enc) {
        NSLog(@"RISEBridge::saveAs: unknown format '%@'", formatName);
        return NO;
    }
    // L5a round-9 — format-aware EncodeOpts.  HDR archival formats
    // (EXR / .hdr / RGBEA) preserve scene-referred linear values
    // > 1.0; their encoders explicitly ignore `viewTransform` and
    // `bpp`, but pass an identity transform anyway so a future
    // encoder change can't accidentally clip a save.  LDR formats
    // (PNG / TIFF-8 / TGA / PPM) get the user's current display EV
    // baked in via `ForLDRDisplay(ev)`, matching the on-screen
    // viewport result the user is likely trying to preserve.
    EncodeOpts opts;
    opts.colorSpace = eColorSpace_sRGB;
    if (enc->SupportsHDR()) {
        opts.bpp           = 0;  // ignored by HDR encoders
        opts.viewTransform = ViewTransform::Identity();
    } else {
        // L5e — bake in the user's currently-active tone curve so
        // the saved LDR image matches the on-screen viewport (the
        // user is most often saving exactly what they're looking at).
        // Production VFS callbacks track the canonical state.
        const DISPLAY_TRANSFORM tc = static_cast<DISPLAY_TRANSFORM>(
            _productionVFSCallbacks->ToneCurve());
        opts.bpp           = 8;
        opts.viewTransform = ViewTransform::ForLDRDisplay(
            static_cast<float>(ev), tc);
    }
    return _productionVFS->SaveAs(
        std::string([path UTF8String]), enc, opts) ? YES : NO;
}

// Lazy-allocate the PRODUCTION VFS + bind its observer callbacks
// to the production helper.  Tile-complete + frame-complete +
// pre-denoise + denoise are all wired so production renders show
// per-tile progressive fills.  Called from the
// rasterize / rasterizeAnimation / rasterizeRegion paths via
// `-ensureVFSAttachedToRasterizer:`.
- (void)ensureProductionVFSCreated {
    if (_productionVFS) return;
    _productionVFS = new Implementation::ViewportFrameStore();
    ViewportFrameStoreCallbacks* helper = _productionVFSCallbacks.get();
    Implementation::ViewportFrameStore* vfs = _productionVFS;
    // L8 round 9 / 17 — Tile-complete callback wired to a
    // try_lock-protected synchronous emit (round 17), restoring
    // per-tile red-toggle visibility that round 9's unwired
    // tile-callback design + 30 Hz polling could not deliver
    // (the toggle "unlock window" between the round-13
    // split-bracket's EndTile-all and BeginTile-all is microseconds,
    // versus the 33 ms poll interval — almost never caught).
    //
    // The synchronous callback runs on the rasterizer worker thread,
    // immediately after the tile's exclusive lock has been released.
    // It TRY-locks the helper's `bufferMutex_` so a concurrent
    // background-queue poll can't make the worker block; on a
    // try-lock miss it simply returns and the bg poll will pick the
    // tile up at the next 33 ms tick (the toggle visualisation for
    // that specific tile is lost in that case, but per-block partial
    // updates and end-of-block final pixels are unaffected).  The
    // emit reads ONLY the just-released tile's region — no
    // cross-tile reads, so the round-1 `bufferMutex_ ↔ tile-mutex`
    // inversion can't re-form.  See `OnTileCompleteTry` doc for
    // the full rationale.
    vfs->SetTileCompleteCallback(
        [helper, vfs](const RISE::Rect& roi, uint64_t gen) {
            helper->OnTileCompleteTry(vfs, roi, gen);
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

// L5a round-5 — lazy-allocate the INTERACTIVE VFS + bind ONLY
// frame-complete observer slots (tile-complete deliberately left
// unbound).  This is what eliminates the per-tile DrawToggles red
// flash from the interactive preview: the interactive rasterizer
// still fires `OutputIntermediateImage` per tile, and VFS still
// processes those into the FrameStore (so the buffer accumulates
// pixels), but no observer fires → no Metal-layer present per
// tile.  Only end-of-pass `OutputImage` fires the frame-complete
// observer, driving an atomic full-frame update of the interactive
// Metal layer.  Called from `-opaqueInteractiveViewportFrameStore`
// when the interactive viewport bridge requests the handle.
//
// Note (L6e-2c): the INTERACTIVE VFS deliberately does NOT call
// `Attach(rasterizer)` — it's exposed as an opaque handle to the
// interactive viewport which feeds it via the
// `ViewportPreviewSink` fan-out (frame-complete only) per L5a's
// SceneEditController-driven preview-scale oscillation.  As a
// result the interactive VFS stays in INTERNAL-managed mode (the
// L5a dormant-cache codepath in EnsureChain is still load-bearing
// for resolution oscillation reuse).  Only the PRODUCTION VFS
// migrated to direct-bind in L6e-2b.  Migrating the interactive
// path requires routing the SceneEditController's per-pass
// rasterizer through Attach / OnRasterizerFrameStoreChanged
// instead of OutputImage; deferred until L6e-3.
- (void)ensureInteractiveVFSCreated {
    if (_interactiveVFS) return;
    _interactiveVFS = new Implementation::ViewportFrameStore();
    ViewportFrameStoreCallbacks* helper = _interactiveVFSCallbacks.get();
    Implementation::ViewportFrameStore* vfs = _interactiveVFS;
    // Tile-complete deliberately NOT bound — see method doc.
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

- (void)ensureVFSAttachedToRasterizer:(IRasterizer*)rasterizer {
    if (!rasterizer) return;

    [self ensureProductionVFSCreated];

    // (Re-)attach.  Each rasterize call begins with FreeRasterizerOutputs
    // which dropped the rasterizer's reference; we reattach here.  Attach
    // is also safe to call repeatedly without a Free in between (it'll
    // just stack a redundant reference) — the BOOL guard avoids that
    // case for clarity.
    //
    // L6e-2b/c — `Attach` (post-commit-f6b0bb4) automatically calls
    // `BindFrameStore(rasterizer->GetFrameStore())` if the rasterizer
    // already has a canonical FrameStore (typical: Job's
    // `PushJobFrameStoreToRasterizers` ran on scene-load before any
    // bridge attached a VFS).  Subsequent `Rasterizer::SetFrameStore`
    // swaps (camera-dim change, active-camera switch) automatically
    // re-bind via the new `OnRasterizerFrameStoreChanged` virtual
    // dispatched on every attached output.  Net effect: for PT / BDPT
    // / VCM / interactive rasterizers the VFS now observes the
    // rasterizer's mFrameStore directly — no VFS-internal FrameStore
    // allocation, no FrameSink cross-store copy.
    //
    // Caveat: MLT integrators (`mlt_rasterizer`, `mlt_spectral_rasterizer`)
    // override `Rasterizer::AcceptsFrameStorePush()` to false (per
    // L6e-1.1), so Job never pushes a FrameStore to them and
    // `rasterizer->GetFrameStore()` returns null at Attach time.  VFS
    // stays in legacy FrameSink-copy mode for those rasterizers
    // until L6d-2 migrates MLT to multi-round-aware FrameStore writes.
    //
    // Note: the EXTERNAL FrameStore bind survives `FreeRasterizerOutputs`
    // (rasterizer drops its ref to VFS but VFS still holds the addref
    // on the FrameStore + observer registration).  When the BOOL flips
    // and we re-Attach, `BindFrameStore` is idempotent on same-pointer
    // rebind — same observer, no thrash.
    //
    // Bridge code below is unchanged because the migration is additive
    // (BindFrameStore is opt-in; Attach now always opts in).
    if (!_productionVFSAttachedToRasterizer) {
        _productionVFS->Attach(rasterizer);
        _productionVFSAttachedToRasterizer = YES;
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

- (NSString *)autoResolvedIntegrator {
    // The active rasterizer's resolved concrete integrator, IF it is the
    // auto-dispatcher.  nil for a normal rasterizer (or before any render —
    // the dispatcher resolves lazily at the first render-time entry).
    if (!_job) return nil;
    IRasterizer* r = _job->GetRasterizer();
    if (!r || !r->IsAutoDispatcher()) return nil;
    const char* name = r->ResolvedIntegratorName();
    return (name && name[0]) ? [NSString stringWithUTF8String:name] : nil;
}

- (NSString *)autoResolveReason {
    if (!_job) return nil;
    IRasterizer* r = _job->GetRasterizer();
    if (!r || !r->IsAutoDispatcher()) return nil;
    const char* reason = r->ResolveReason();
    return (reason && reason[0]) ? [NSString stringWithUTF8String:reason] : nil;
}

- (uint32_t)cameraWidth {
    // Post-master-merge — camera dims live on the scene-level Film
    // (origin/master's camera-Film split, commit 45aa217).
    if (!_job) return 0;
    IScenePriv* scene = _job->GetScene();
    if (!scene) return 0;
    const IFilm* film = scene->GetFilm();
    if (!film) return 0;
    return film->GetWidth();
}

- (uint32_t)cameraHeight {
    if (!_job) return 0;
    IScenePriv* scene = _job->GetScene();
    if (!scene) return 0;
    const IFilm* film = scene->GetFilm();
    if (!film) return 0;
    return film->GetHeight();
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
        _productionVFSAttachedToRasterizer = NO;
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
    _productionVFSAttachedToRasterizer = NO;

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
        _productionVFSAttachedToRasterizer = NO;
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

- (void)attachViewportFrameStoreToOpaqueRasterizer:(void *)opaqueRasterizer {
    // L5a round-3 — wire a rasterizer into the production VFS.
    // Suitable for additional production-style rasterizers; not
    // useful for the SceneEditController-driven interactive
    // viewport (use `opaqueInteractiveViewportFrameStore` + the
    // sink fan-out for that path — see header doc).
    //
    // L6e-2b/c — `Attach` auto-binds the VFS to the rasterizer's
    // canonical FrameStore (when present) so subsequent reads /
    // writes go straight through `mFrameStore`.  See
    // `-ensureVFSAttachedToRasterizer:` for the migration commentary.
    if (!opaqueRasterizer) return;
    [self ensureProductionVFSCreated];
    IRasterizer* rasterizer = static_cast<IRasterizer*>(opaqueRasterizer);
    _productionVFS->Attach(rasterizer);
}

- (void *)opaqueInteractiveViewportFrameStore {
    // L5a round-5 — opaque INTERACTIVE VFS handle for the
    // interactive viewport's preview-sink fan-out.  Lazy-creates
    // if needed.  Independent of the production VFS — see the
    // header doc-comment for why this separation matters
    // (cancel-preserves-production, no per-tile flash, no
    // resolution-thrash interfering with production buffers).
    [self ensureInteractiveVFSCreated];
    return static_cast<void*>(_interactiveVFS);
}

@end
