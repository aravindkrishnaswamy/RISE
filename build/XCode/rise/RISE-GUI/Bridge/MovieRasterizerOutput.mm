//////////////////////////////////////////////////////////////////////
//
//  MovieRasterizerOutput.mm - Implementation of MovieRasterizerOutput.
//  Assembles animation frames into an HDR-preserving ProRes 4444
//  QuickTime .mov file using AVFoundation's AVAssetWriter.
//
//////////////////////////////////////////////////////////////////////

#import "MovieRasterizerOutput.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include "Interfaces/IRasterImage.h"
#include "Interfaces/ILog.h"

// Note: do NOT use "using namespace RISE;" here — RISE::Rect conflicts
// with the macOS Carbon Rect type pulled in by AVFoundation headers.

// ProRes 4444 is a 12-bit INTEGER codec — it has no above-white headroom,
// so scene-linear half-float values >1.0 would clip to white at encode and
// HDR would be lost. We therefore master this as an HDR10-style signal:
// convert the renderer's Rec.709-primary scene-linear RGB to Rec.2020
// primaries, then apply the SMPTE ST.2084 (PQ) OETF so the full luminance
// range maps non-linearly into the codec's [0,1] integer range.

// Rec.709 (D65) -> Rec.2020 (D65) linear-RGB primary conversion.
// Input must already be floored at 0; returns the converted channel value.
static inline double rec709ToRec2020R(double r, double g, double b)
{
    return 0.6274039 * r + 0.3292830 * g + 0.0433136 * b;
}
static inline double rec709ToRec2020G(double r, double g, double b)
{
    return 0.0690973 * r + 0.9195404 * g + 0.0113623 * b;
}
static inline double rec709ToRec2020B(double r, double g, double b)
{
    return 0.0163914 * r + 0.0880133 * g + 0.8955857 * b;
}

// SMPTE ST.2084 (PQ) OETF. Reference: linear 1.0 = 100 nits (SDR diffuse
// white), PQ peak = 10000 nits. Returns a PQ code value in [0,1].
static inline float linearToPQ(double lin)
{
    double Lp = fmin(fmax(lin, 0.0) * (100.0 / 10000.0), 1.0);
    double m1 = 0.1593017578125, m2 = 78.84375;
    double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    double Lm = pow(Lp, m1);
    return (float)pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);
}

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
    , _framesReceived(0)
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
    // Round up to next multiple of 2 (harmless even-dimension safeguard)
    _width = (width + 1) & ~1;
    _height = (height + 1) & ~1;

    if (_width != width || _height != height) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Event,
            "MovieRasterizerOutput:: Rounding dimensions from %dx%d to %dx%d",
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

    // Video settings: Apple ProRes 4444 (constant-quality intra codec).
    // No compression-properties sub-dict — ProRes has no bitrate/profile
    // knobs (those are H.264-only and would be rejected).
    //
    // Tag the track as an HDR10-style PQ master. The fill loop converts the
    // renderer's Rec.709-primary scene-linear RGB to Rec.2020 primaries and
    // PQ-encodes it, so the carried code values are genuine Rec.2020 + PQ:
    //   primaries        Rec.2020 (data is matrix-converted into this gamut)
    //   transfer fn      SMPTE ST.2084 / PQ (data is PQ-encoded in the loop)
    //   Y'CbCr matrix    Rec.2020 (paired with the Rec.2020 primaries)
    // The source buffers are tagged with a matching Rec.2020 PQ color space
    // below, so AVFoundation tags the output without an implicit conversion.
    NSDictionary* videoSettings = @{
        AVVideoCodecKey: AVVideoCodecTypeAppleProRes4444,
        AVVideoWidthKey: @(_width),
        AVVideoHeightKey: @(_height),
        AVVideoColorPropertiesKey: @{
            AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_2020,
            AVVideoTransferFunctionKey: AVVideoTransferFunction_SMPTE_ST_2084_PQ,
            AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_2020,
        }
    };

    _input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                outputSettings:videoSettings];
    _input.expectsMediaDataInRealTime = NO;

    // Pixel buffer adaptor for efficient pixel buffer pool access.
    // 64-bit RGBA, 16-bit IEEE half per channel (8 bytes/pixel) so values
    // outside [0,1] survive into the encoder.
    NSDictionary* bufferAttributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_64RGBAHalf),
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
        "MovieRasterizerOutput:: Writing %dx%d ProRes 4444 (HDR10, PQ, Rec.2020) video to '%s'",
        width, height, [_outputPath UTF8String]);

    return true;
}

void MovieRasterizerOutput::OutputImage(
    const RISE::IRasterImage& pImage,
    const RISE::Rect* /*pRegion*/,
    const unsigned int frame)
{
    if (_finalized) return;

    @autoreleasepool {

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

    // Create a pixel buffer directly (don't rely on pool which can become nil).
    // 64-bit RGBA half-float so HDR radiance (>1.0) is preserved.
    CVPixelBufferRef pixelBuffer = NULL;
    NSDictionary* pbAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_64RGBAHalf),
        (NSString*)kCVPixelBufferWidthKey: @(_width),
        (NSString*)kCVPixelBufferHeightKey: @(_height),
        (NSString*)kCVPixelBufferCGImageCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferCGBitmapContextCompatibilityKey: @YES,
    };

    CVReturn status = CVPixelBufferCreate(
        kCFAllocatorDefault, _width, _height,
        kCVPixelFormatType_64RGBAHalf,
        (__bridge CFDictionaryRef)pbAttrs,
        &pixelBuffer);
    if (status != kCVReturnSuccess || !pixelBuffer) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Error,
            "MovieRasterizerOutput:: Failed to create pixel buffer at frame %u (status=%d)",
            frame, (int)status);
        return;
    }

    // Attach a Rec.2020 PQ color space matching the now-PQ-encoded buffer
    // contents and the Rec.2020 / ST.2084 track tagging in setupWriter, so
    // AVFoundation does not apply an implicit color conversion.
    //
    // NB: the SDK exposes kCGColorSpaceITUR_2020_PQ but marks it
    // API_DEPRECATED("No longer supported", macos(10.15.4,11.0)) and points
    // callers at kCGColorSpaceITUR_2100_PQ. Since warnings are treated as
    // build errors here, we use the non-deprecated Rec.2100 PQ successor —
    // identical Rec.2020 primaries + SMPTE ST.2084 PQ transfer, available
    // since macOS 11 (well under our macOS 26 deployment target).
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
    if (cs) {
        CVBufferSetAttachment(pixelBuffer, kCVImageBufferCGColorSpaceKey, cs,
                              kCVAttachmentMode_ShouldPropagate);
        CGColorSpaceRelease(cs);
    }

    // Lock the buffer and fill it with pixel data
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    uint8_t* baseAddress = (uint8_t*)CVPixelBufferGetBaseAddress(pixelBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);

    // Clear the entire buffer first (handles padding rows/columns for even dimensions)
    memset(baseAddress, 0, bytesPerRow * _height);

    for (int y = 0; y < imgH; y++) {
        // 16-bit IEEE half, RGBA order. The renderer emits Rec.709-primary
        // scene-linear RGB; floor each channel at 0, convert to Rec.2020
        // primaries, then PQ-encode so HDR radiance survives the 12-bit
        // integer ProRes codec as code values in [0,1]. Alpha is a coverage
        // value, not radiance — keep it LINEAR, clamped to [0,1] (no PQ).
        // c.base.* and c.a are double, so fmax/pow resolve to the double
        // overloads; the (__fp16) casts are explicit.
        __fp16* row = (__fp16*)(baseAddress + y * bytesPerRow);
        for (int x = 0; x < imgW; x++) {
            RISE::RISEColor c = pImage.GetPEL(x, y);

            double r = fmax(c.base.r, 0.0);
            double g = fmax(c.base.g, 0.0);
            double b = fmax(c.base.b, 0.0);
            double R = rec709ToRec2020R(r, g, b);
            double G = rec709ToRec2020G(r, g, b);
            double B = rec709ToRec2020B(r, g, b);

            row[x * 4 + 0] = (__fp16)linearToPQ(R);                // R
            row[x * 4 + 1] = (__fp16)linearToPQ(G);                // G
            row[x * 4 + 2] = (__fp16)linearToPQ(B);                // B
            row[x * 4 + 3] = (__fp16)fmin(fmax(c.a, 0.0), 1.0);    // A
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
    _framesReceived++;

    } // @autoreleasepool
}

void MovieRasterizerOutput::finalize()
{
    if (_finalized) return;
    _finalized = true;

    if (!_started || !_writer) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Warning,
            "MovieRasterizerOutput:: finalize called but no frames were written (received %u frames)",
            _framesReceived);
        return;
    }

    RISE::GlobalLog()->PrintEx(RISE::eLog_Event,
        "MovieRasterizerOutput:: Finalizing video with %u frames...", _framesReceived);

    [_input markAsFinished];

    // Use a semaphore to wait for async finishWriting
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [_writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (_writer.status == AVAssetWriterStatusCompleted) {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Event,
            "MovieRasterizerOutput:: Video written successfully (%u frames) to '%s'",
            _framesReceived, [_outputPath UTF8String]);
    } else {
        RISE::GlobalLog()->PrintEx(RISE::eLog_Error,
            "MovieRasterizerOutput:: Video writing failed after %u frames: %s",
            _framesReceived,
            [[_writer.error localizedDescription] UTF8String]);
    }

    _writer = nil;
    _input = nil;
    _adaptor = nil;
}
