//////////////////////////////////////////////////////////////////////
//
//  RISEBridge.mm - Objective-C++ bridge between RISE C++ engine
//  and the Swift/SwiftUI application.
//
//////////////////////////////////////////////////////////////////////

#import "RISEBridge.h"

// C++ RISE engine includes
#include "RISE_API.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/IProgressCallback.h"
#include "Interfaces/IJobRasterizerOutput.h"
#include "Interfaces/ILogPriv.h"
#include "Utilities/RTime.h"
#include "Utilities/MediaPathLocator.h"

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
            NSString *title = [NSString stringWithUTF8String:_currentTitle.c_str()];
            return (bool)_block(progress, total, title);
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
            _block(pImageData, width, height, rc_top, rc_left, rc_bottom, rc_right);
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
            NSString *msg = [NSString stringWithUTF8String:event.szMessage];
            RISELogLevel level = static_cast<RISELogLevel>(event.eType);
            _block(level, msg);
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

@end
