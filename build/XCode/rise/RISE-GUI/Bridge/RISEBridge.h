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
/// pImageData is RGBA16 (4 channels, 16 bits each = 8 bytes/pixel).
/// The region defines which portion of the full image was updated.
typedef void (^RISEImageOutputBlock)(const uint16_t *pImageData,
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

/// Sets the image output callback block for progressive render updates. Pass nil to clear.
- (void)setImageOutputBlock:(nullable RISEImageOutputBlock)block;

/// Sets the log callback block for receiving engine log messages. Pass nil to clear.
/// The block may be called from any thread.
- (void)setLogBlock:(nullable RISELogBlock)block;

/// Returns YES if the loaded scene has any keyframed (animated) objects.
- (BOOL)hasAnimatedObjects;

/// Returns the width of the loaded scene's camera, or 0 if no scene is loaded.
- (uint32_t)cameraWidth;

/// Returns the height of the loaded scene's camera, or 0 if no scene is loaded.
- (uint32_t)cameraHeight;

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

@end

NS_ASSUME_NONNULL_END
