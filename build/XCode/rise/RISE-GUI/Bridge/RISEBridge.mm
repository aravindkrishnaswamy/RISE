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
#include "Interfaces/IJobRasterizerOutput.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/ILogPriv.h"
#include "Interfaces/IScene.h"
#include "Interfaces/ICamera.h"
#include "Utilities/RTime.h"
#include "Utilities/MediaPathLocator.h"
#include "Utilities/RenderETAEstimator.h"

#include "Utilities/Reference.h"

#include <string>
#include <mutex>

using namespace RISE;

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
// C++ callback adapter: IJobRasterizerOutput -> ObjC block
// ============================================================
class BlockRasterizerOutput : public IJobRasterizerOutput {
public:
    RISEImageOutputBlock _block;

    BlockRasterizerOutput(RISEImageOutputBlock block) : _block(block) {}

    bool PremultipliedAlpha() override { return false; }
    int GetColorSpace() override { return 1; } // sRGB

    void OutputImageRGBA16(
        const unsigned short* pImageData,
        const unsigned int width,
        const unsigned int height,
        const unsigned int rc_top,
        const unsigned int rc_left,
        const unsigned int rc_bottom,
        const unsigned int rc_right) override
    {
        if (_block) {
            @autoreleasepool {
                _block(pImageData, width, height, rc_top, rc_left, rc_bottom, rc_right);
            }
        }
    }
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
    BlockRasterizerOutput* _rasterizerOutput;
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
        _rasterizerOutput = nullptr;
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
    if (_rasterizerOutput) {
        delete _rasterizerOutput;
        _rasterizerOutput = nullptr;
    }
    if (_logPrinter) {
        _logPrinter->release();
        _logPrinter = nullptr;
    }
    if (_job) {
        _job->release();
        _job = nullptr;
    }
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

    if (_rasterizerOutput) {
        delete _rasterizerOutput;
        _rasterizerOutput = nullptr;
    }

    if (_imageOutputBlock) {
        _rasterizerOutput = new BlockRasterizerOutput(_imageOutputBlock);
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

- (BOOL)rasterize {
    if (!_job) return NO;

    // Register callback rasterizer output right before rendering.
    // The rasterizer must exist (set up by LoadAsciiScene).
    if (_rasterizerOutput) {
        _job->AddCallbackRasterizerOutput(_rasterizerOutput);
    }

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

    if (_rasterizerOutput) {
        _job->AddCallbackRasterizerOutput(_rasterizerOutput);
    }

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

    if (_rasterizerOutput) {
        _job->AddCallbackRasterizerOutput(_rasterizerOutput);
    }

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

@end
