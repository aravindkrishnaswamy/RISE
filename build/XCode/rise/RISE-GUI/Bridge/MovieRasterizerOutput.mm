//////////////////////////////////////////////////////////////////////
//
//  MovieRasterizerOutput.mm - Implementation of MovieRasterizerOutput.
//  Assembles animation frames into an H.264 QuickTime .mov file
//  using AVFoundation's AVAssetWriter.
//
//////////////////////////////////////////////////////////////////////

#import "MovieRasterizerOutput.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include "Interfaces/IRasterImage.h"
#include "Interfaces/ILog.h"

// Note: do NOT use "using namespace RISE;" here — RISE::Rect conflicts
// with the macOS Carbon Rect type pulled in by AVFoundation headers.

MovieRasterizerOutput::MovieRasterizerOutput(NSString* outputPath, int fps)
    : _writer(nil)
    , _input(nil)
    , _adaptor(nil)
    , _outputPath([outputPath copy])
    , _fps(fps)
    , _started(false)
    , _finalized(false)
    , _width(0)
    , _height(0)
{
}

MovieRasterizerOutput::~MovieRasterizerOutput()
{
    if (!_finalized) {
        finalize();
    }
}

void MovieRasterizerOutput::OutputIntermediateImage(
    const RISE::IRasterImage& /*pImage*/, const RISE::Rect* /*pRegion*/)
{
    // No-op: we only want complete frames.
}

bool MovieRasterizerOutput::setupWriter(int width, int height)
{
    // H.264 requires even dimensions — round up to next multiple of 2
    _width = (width + 1) & ~1;
    _height = (height + 1) & ~1;

    if (_width != width || _height != height) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Event,
            "MovieRasterizerOutput:: Rounding dimensions from %dx%d to %dx%d for H.264",
            width, height, _width, _height);
    }

    // Remove existing file if present
    NSURL* fileURL = [NSURL fileURLWithPath:_outputPath];
    [[NSFileManager defaultManager] removeItemAtURL:fileURL error:nil];

    NSError* error = nil;
    _writer = [AVAssetWriter assetWriterWithURL:fileURL
                                       fileType:AVFileTypeQuickTimeMovie
                                          error:&error];
    if (!_writer || error) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Error,
            "MovieRasterizerOutput:: Failed to create AVAssetWriter: %s",
            [[error localizedDescription] UTF8String]);
        return false;
    }

    // Video settings: H.264 codec
    NSDictionary* videoSettings = @{
        AVVideoCodecKey: AVVideoCodecTypeH264,
        AVVideoWidthKey: @(_width),
        AVVideoHeightKey: @(_height),
        AVVideoCompressionPropertiesKey: @{
            AVVideoAverageBitRateKey: @(_width * _height * 4),
            AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
        }
    };

    _input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                outputSettings:videoSettings];
    _input.expectsMediaDataInRealTime = NO;

    // Pixel buffer adaptor for efficient pixel buffer pool access
    NSDictionary* bufferAttributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferWidthKey: @(_width),
        (NSString*)kCVPixelBufferHeightKey: @(_height),
    };

    _adaptor = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:_input
                                  sourcePixelBufferAttributes:bufferAttributes];

    if (![_writer canAddInput:_input]) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Error,
            "MovieRasterizerOutput:: Cannot add video input to writer");
        _writer = nil;
        _input = nil;
        _adaptor = nil;
        return false;
    }

    [_writer addInput:_input];
    [_writer startWriting];
    [_writer startSessionAtSourceTime:kCMTimeZero];

    RISE::GlobalLog()->PrintEx(RISE::eLog_Event,
        "MovieRasterizerOutput:: Writing %dx%d H.264 video to '%s'",
        width, height, [_outputPath UTF8String]);

    return true;
}

void MovieRasterizerOutput::OutputImage(
    const RISE::IRasterImage& pImage,
    const RISE::Rect* /*pRegion*/,
    const unsigned int frame)
{
    if (_finalized) return;

    int imgW = (int)pImage.GetWidth();
    int imgH = (int)pImage.GetHeight();

    // Lazy initialization on first frame
    if (!_started) {
        if (!setupWriter(imgW, imgH)) {
            _finalized = true;
            return;
        }
        _started = true;
    }

    // Check if writer is still in a good state
    if (_writer.status != AVAssetWriterStatusWriting) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Error,
            "MovieRasterizerOutput:: Writer is in failed state at frame %u: %s",
            frame,
            [[_writer.error localizedDescription] UTF8String]);
        _finalized = true;
        return;
    }

    // Wait until the input is ready for more data
    while (!_input.isReadyForMoreMediaData) {
        [NSThread sleepForTimeInterval:0.01];
    }

    // Create a pixel buffer directly (don't rely on pool which can become nil)
    CVPixelBufferRef pixelBuffer = NULL;
    NSDictionary* pbAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferWidthKey: @(_width),
        (NSString*)kCVPixelBufferHeightKey: @(_height),
        (NSString*)kCVPixelBufferCGImageCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferCGBitmapContextCompatibilityKey: @YES,
    };

    CVReturn status = CVPixelBufferCreate(
        kCFAllocatorDefault, _width, _height,
        kCVPixelFormatType_32BGRA,
        (__bridge CFDictionaryRef)pbAttrs,
        &pixelBuffer);
    if (status != kCVReturnSuccess || !pixelBuffer) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Error,
            "MovieRasterizerOutput:: Failed to create pixel buffer at frame %u (status=%d)",
            frame, (int)status);
        return;
    }

    // Lock the buffer and fill it with pixel data
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    uint8_t* baseAddress = (uint8_t*)CVPixelBufferGetBaseAddress(pixelBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);

    // Clear the entire buffer first (handles padding rows/columns for even dimensions)
    memset(baseAddress, 0, bytesPerRow * _height);

    for (int y = 0; y < imgH; y++) {
        uint8_t* row = baseAddress + y * bytesPerRow;
        for (int x = 0; x < imgW; x++) {
            RISE::RISEColor c = pImage.GetPEL(x, y);

            // Clamp and convert float [0,1] to uint8 [0,255], BGRA order
            uint8_t r = (uint8_t)(fmin(fmax(c.base.r, 0.0), 1.0) * 255.0 + 0.5);
            uint8_t g = (uint8_t)(fmin(fmax(c.base.g, 0.0), 1.0) * 255.0 + 0.5);
            uint8_t b = (uint8_t)(fmin(fmax(c.base.b, 0.0), 1.0) * 255.0 + 0.5);
            uint8_t a = (uint8_t)(fmin(fmax(c.a, 0.0), 1.0) * 255.0 + 0.5);

            row[x * 4 + 0] = b;  // B
            row[x * 4 + 1] = g;  // G
            row[x * 4 + 2] = r;  // R
            row[x * 4 + 3] = a;  // A
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    // Append the pixel buffer with the correct presentation time
    CMTime presentationTime = CMTimeMake(frame, _fps);
    if (![_adaptor appendPixelBuffer:pixelBuffer
              withPresentationTime:presentationTime]) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Error,
            "MovieRasterizerOutput:: Failed to append frame %u: %s",
            frame,
            [[_writer.error localizedDescription] UTF8String]);
    }

    CVPixelBufferRelease(pixelBuffer);
}

void MovieRasterizerOutput::finalize()
{
    if (_finalized) return;
    _finalized = true;

    if (!_started || !_writer) return;

    [_input markAsFinished];

    // Use a semaphore to wait for async finishWriting
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [_writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (_writer.status == AVAssetWriterStatusCompleted) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Event,
            "MovieRasterizerOutput:: Video written successfully to '%s'",
            [_outputPath UTF8String]);
    } else {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Error,
            "MovieRasterizerOutput:: Video writing failed: %s",
            [[_writer.error localizedDescription] UTF8String]);
    }

    _writer = nil;
    _input = nil;
    _adaptor = nil;
}
