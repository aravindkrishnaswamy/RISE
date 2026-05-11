//////////////////////////////////////////////////////////////////////
//
//  RISEBridge.h - Objective-C interface to the RISE C++ rendering engine.
//  This file must be pure Objective-C (no C++) so it can be imported
//  via the Swift bridging header.
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Block type for progress updates.
/// Return YES to continue rendering, NO to cancel.
typedef BOOL (^RISEProgressBlock)(double progress, double total, NSString *title);

/// Block type for receiving rendered image data (progressive updates).
/// pImageData is RGBA16 sRGB (4 channels, 16 bits each = 8 bytes/pixel,
/// uint16 fixed-point with sRGB primaries + sRGB transfer applied).
/// The region defines which portion of the full image was updated.
/// Use this block for the legacy LDR display path (NSImage / CGImage).
typedef void (^RISEImageOutputBlock)(const uint16_t *pImageData,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t regionTop,
                                     uint32_t regionLeft,
                                     uint32_t regionBottom,
                                     uint32_t regionRight);

/// L5a — HDR display block.  pImageData is `RGBA16F_ExtendedLinearSRGB`
/// (4 channels, IEEE 754 binary16 half-floats, linear sRGB primaries
/// with a Linear transfer — i.e. no gamma applied).  Values can
/// legitimately exceed 1.0 (HDR highlights); the OS / CAMetalLayer
/// path tone-maps to the display's actual EDR headroom.  Bytewise
/// the same 8-bytes-per-pixel layout as RISEImageOutputBlock, but
/// the bits encode binary16 not uint16 — Swift consumers MUST
/// interpret accordingly (e.g. via `withMemoryRebound(to: Float16.self, ...)`).
/// Fired only while `setHDREnabled:YES` is in effect on a screen
/// with `maximumExtendedDynamicRangeColorComponentValue > 1.0`.
typedef void (^RISEHDRImageOutputBlock)(const uint16_t *pImageData,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t regionTop,
                                        uint32_t regionLeft,
                                        uint32_t regionBottom,
                                        uint32_t regionRight);

/// Log severity levels matching RISE's LOG_ENUM.
typedef NS_ENUM(NSInteger, RISELogLevel) {
    RISELogLevelEvent   = 1,
    RISELogLevelInfo    = 2,
    RISELogLevelWarning = 4,
    RISELogLevelError   = 8,
    RISELogLevelFatal   = 16
};

/// Block type for receiving log messages from the RISE engine.
typedef void (^RISELogBlock)(RISELogLevel level, NSString *message);

@interface RISEBridge : NSObject

/// Returns the RISE library version as a string.
+ (NSString *)versionString;

/// Adds a directory to the media search path.
- (void)addMediaPath:(NSString *)path;

/// Sets the RISE project root directory. This adds it to the media search path
/// and sets the RISE_MEDIA_PATH environment variable (needed by FileRasterizerOutput).
- (void)setProjectRoot:(NSString *)path;

/// Loads a scene from an ASCII scene description file.
- (BOOL)loadAsciiScene:(NSString *)filePath;

/// Clears the entire scene, resetting everything.
- (BOOL)clearAll;

/// Sets the progress callback block. Pass nil to clear.
- (void)setProgressBlock:(nullable RISEProgressBlock)block;

/// PRODUCTION-render LDR callback.  Fires on every tile/frame event
/// from the production rasterizer (the one driven by Render +
/// Render-Animation buttons), when HDR is OFF.  Pass nil to clear.
/// L5a round-5 split: this is one of FOUR independent block slots
/// — production-LDR, production-HDR, interactive-LDR, interactive-
/// HDR — each driving its own ViewportFrameStore.  Production
/// renders are full-quality, multi-output (display + file +
/// denoise + ...), per-tile updates; the production VFS is the
/// one downstream consumers like FileRasterizerOutput attach to.
- (void)setImageOutputBlock:(nullable RISEImageOutputBlock)block;

/// PRODUCTION-render HDR callback (extended-linear-sRGB binary16
/// for CAMetalLayer EDR composition).  Same firing conditions as
/// `-setImageOutputBlock:` but for HDR-enabled mode.  Wire from a
/// Swift Metal renderer configured with
/// `wantsExtendedDynamicRangeContent = YES`.
- (void)setHDRImageOutputBlock:(nullable RISEHDRImageOutputBlock)block;

/// L5a round-5 — INTERACTIVE-render LDR callback.  Fires only on
/// frame-complete events from the interactive (live-preview)
/// rasterizer, when HDR is OFF.  Per-tile fires are deliberately
/// suppressed for the interactive path (no DrawToggles red
/// flashes, no per-tile preview-scale-thrash buffer clears).  The
/// interactive VFS is independent of the production VFS — its
/// pixels never overwrite production output, so cancelling a
/// production render preserves the partial production image.
- (void)setInteractiveImageOutputBlock:(nullable RISEImageOutputBlock)block;

/// L5a round-5 — INTERACTIVE-render HDR callback.  HDR variant of
/// `-setInteractiveImageOutputBlock:`.  Wire from a separate
/// Swift Metal renderer/layer that overlays the production EDR
/// surface so partial production results stay visible underneath.
- (void)setInteractiveHDRImageOutputBlock:(nullable RISEHDRImageOutputBlock)block;

/// L5a — toggle HDR pipeline.  When YES, the bridge emits the
/// HDR block with binary16 half-float pixels in extendedLinearSRGB;
/// the LDR block is suppressed.  When NO (default), the legacy LDR
/// block fires with RGBA16-sRGB.  Toggle from Swift after detecting
/// EDR headroom via -displayMaxEDRHeadroom; mid-render toggle is
/// supported and triggers an immediate re-emit at the new format.
- (void)setHDREnabled:(BOOL)enabled;

/// L5a — current EDR headroom of the active screen, expressed as
/// the maximum allowed component value (1.0 = SDR, no EDR; >1.0 =
/// EDR-capable).  Wraps
/// `[NSScreen.mainScreen maximumExtendedDynamicRangeColorComponentValue]`;
/// Swift code can poll this on `didChangeScreen` notifications and
/// flip -setHDREnabled: accordingly.  Returns 1.0 if no screen is
/// available (e.g. headless / pre-window-show).
- (float)displayMaxEDRHeadroom;

/// Sets the log callback block for receiving engine log messages. Pass nil to clear.
/// The block may be called from any thread.
- (void)setLogBlock:(nullable RISELogBlock)block;

/// Returns YES if the loaded scene has any keyframed (animated) objects.
- (BOOL)hasAnimatedObjects;

/// Returns the width of the loaded scene's camera, or 0 if no scene is loaded.
- (uint32_t)cameraWidth;

/// Returns the height of the loaded scene's camera, or 0 if no scene is loaded.
- (uint32_t)cameraHeight;

/// Sets the scene time the next `rasterize` call should render at.
/// Internally invokes `IScene::SetSceneTime(t)` immediately, which:
///   - Advances the animator (camera, transforms, materials at time t).
///   - Regenerates every populated photon map at time t — the
///     expensive step the interactive scrub deliberately skips.
/// Call this AFTER stopping the viewport bridge's interactive thread
/// (the viewport's preview path uses `SetSceneTimeForPreview` which
/// skips photon regen — without an explicit full SetSceneTime here,
/// hitting Render after scrubbing produces the right transforms but
/// caustics from the pre-scrub time).
- (void)setSceneTime:(double)t;

/// Rasterizes the entire scene. This is BLOCKING -- call from a background thread.
- (BOOL)rasterize;

/// Sets the output path for animation video (.mov). Pass nil to disable video output.
- (void)setAnimationVideoOutputPath:(nullable NSString *)path;

/// Rasterizes the full animation using options from the scene file.
/// This is BLOCKING -- call from a background thread.
- (BOOL)rasterizeAnimation;

/// Rasterizes a specific region. This is BLOCKING -- call from a background thread.
- (BOOL)rasterizeRegionLeft:(uint32_t)left
                        top:(uint32_t)top
                      right:(uint32_t)right
                     bottom:(uint32_t)bottom;

#pragma mark - Live exposure scrubbing & multi-format Save-As (L4b)

/// Set the view exposure compensation in EV stops.  Updates the
/// ViewTransform applied when reading the canonical HDR FrameStore
/// for live display — does NOT trigger a rasterizer re-run.  Calling
/// this triggers an immediate re-emit of the cached image through
/// the existing imageOutputBlock so the slider feels live.  No-op
/// until at least one render has produced output.  See
/// docs/FRAMESTORE_DESIGN.md §11 L4b.
- (void)setViewExposureEV:(double)ev;

/// L5e — Set the LDR view tone curve.  `curve` is a value of the
/// `DISPLAY_TRANSFORM` enum cast to `int`:
///   0 = None      (legacy clip-at-1.0 behaviour)
///   1 = Reinhard  (x / (1 + x))
///   2 = ACES      (Narkowicz fit; library default)
///   3 = AgX       (Sobotka scalar)
///   4 = Hable     (Uncharted 2 filmic)
/// The default is ACES.  Like `setViewExposureEV`, applies at
/// display read-back time only — no rasterizer re-run.  Triggers an
/// immediate Repaint so the on-screen image swaps curves live.
/// Ignored when the EDR (HDR) preview is on (HDR display path is
/// by-construction tone-curve-free; the OS compositor handles the
/// display-side mapping).
- (void)setViewToneCurve:(int)curve;

/// L8 round 9 — UI-thread polling entry points for the lockless
/// progressive-update path.  Call from a display Timer (~30 Hz
/// recommended) during an active render so the on-screen image
/// refreshes as workers produce new pixels.
///
/// Each call atomically reads the underlying FrameStore's
/// generation counter (which workers bump on every EndTile) and:
///   * No-ops if the counter hasn't changed since the last poll.
///   * Otherwise emits a full-image `RenderToBuffer` and fires the
///     LDR / HDR block(s) for the bound layer.
///
/// Cost when nothing has changed: ~10 ns (one atomic load + compare).
/// Cost when dirty: one full-image emit (~5 ms at 800x600).
/// Safe to call at any rate; the no-change short-circuit means
/// over-polling is cheap.
///
/// Workers fire NO synchronous bridge callbacks per tile in this
/// design — `setImageOutputBlock` / `setInteractiveImageOutputBlock`
/// (and their HDR siblings) ONLY fire from polling here and from
/// the end-of-render `OnFrameComplete` event.  See round-9 commit
/// message + `ViewportFrameStoreCallbacks::PollAndEmitIfDirty`
/// rationale in RISEBridge.mm for the full architecture.
- (void)pollProductionVFS;
- (void)pollInteractiveVFS;

/// Save the current FrameStore contents to disk via the L2
/// IFrameEncoder pipeline.  `formatName` is one of
/// "PNG", "EXR", "TIFF", "HDR", "RGBEA", "TGA", "PPM" (matched
/// case-insensitively by the FrameEncoderRegistry).  `ev` applies
/// to the LDR-fixed encoders (PNG/TIFF/TGA/PPM) where the tone curve
/// + sRGB transfer is applied; HDR encoders ignore it (scene-referred
/// linear).  Returns YES on success.  Safe to call mid-render — the
/// encoder walks the FrameStore under the per-tile shared_mutex so
/// concurrent writes are correctly synchronised (L4 round-1 fix).
- (BOOL)saveAs:(NSString *)path
        format:(NSString *)formatName
    exposureEV:(double)ev;

#pragma mark - Render-time ETA estimator
/// Starts a new ETA session. Call at the beginning of every render.
- (void)etaBegin;

/// Feed the estimator with the same (progress, total) pair received from
/// the progress block. Safe to call from any thread.
- (void)etaUpdateProgress:(double)progress total:(double)total;

/// Total seconds since etaBegin.
- (double)etaElapsedSeconds;

/// Returns the current remaining-time estimate in seconds, or nil while
/// the estimator is still warming up.
- (nullable NSNumber *)etaRemainingSeconds;

/// Formats a duration as "M:SS" below one hour, "HH:MM:SS" at or above.
+ (NSString *)formatDuration:(double)seconds;

#pragma mark - Internal handles for sibling bridges

/// Returns an opaque pointer to the underlying C++ IJobPriv*.  Used by
/// RISEViewportBridge so the interactive editor and the loader bridge
/// share the same in-memory scene.  The handle is owned by this
/// RISEBridge — callers must not retain or release it.
- (void *)opaqueJobHandle;

/// L5a round-3 — attach this bridge's ViewportFrameStore as an
/// IRasterizerOutput on the supplied IRasterizer.  Suitable for
/// rasterizers whose outputs list is NOT clobbered each render
/// pass (e.g. the production rasterizer).  NOT suitable for the
/// interactive viewport — `SceneEditController::DoOnePass`
/// explicitly calls `FreeRasterizerOutputs()` before re-installing
/// only its preview sink (SceneEditController.cpp:1630-1631), so
/// any external attach is silently dropped.  For that case use
/// `-opaqueViewportFrameStore` and fan VFS into the sink directly.
///
/// Lazily creates the VFS if it doesn't yet exist.  The bridge
/// keeps a strong VFS reference for the bridge's lifetime; the
/// rasterizer addrefs on Attach.
- (void)attachViewportFrameStoreToOpaqueRasterizer:(void *)opaqueRasterizer;

/// L5a round-5 — returns the INTERACTIVE ViewportFrameStore as an
/// opaque pointer (cast to `RISE::Implementation::ViewportFrameStore*`
/// inside `.mm` files).  Used by RISEViewportBridge's preview
/// sink to fan the interactive rasterizer's OutputImage into VFS.
/// The interactive VFS is INDEPENDENT of the production VFS:
///   - It only fires frame-complete observers (not per-tile), so
///     no DrawToggles red flashing while editing.
///   - Its pixels never overwrite production output — cancelling
///     a production render preserves the partial production image
///     in the production VFS.
///   - Resolution thrash from the interactive preview-scale state
///     machine reallocates only THIS VFS, not production.
/// Lifetime: borrowed pointer; lives for the lifetime of this
/// RISEBridge.  Callers MUST NOT release it.
- (void *)opaqueInteractiveViewportFrameStore;

@end

NS_ASSUME_NONNULL_END
