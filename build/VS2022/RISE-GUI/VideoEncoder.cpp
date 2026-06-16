//////////////////////////////////////////////////////////////////////
//
//  VideoEncoder.cpp - FFmpeg HDR video encoder (ProRes 4444 / HEVC HDR10).
//
//  Ported from the Mac app's MovieRasterizerOutput.mm which uses
//  AVFoundation. This version uses FFmpeg libavcodec/libavformat and
//  encodes one of two formats, selected by the Codec ctor argument:
//
//  Codec::ProRes4444 — the HDR master:
//  - Apple ProRes 4444 (prores_ks, profile "4444")
//  - YUVA444P10LE (10-bit 4:4:4 + alpha), full ("JPEG") range
//  - Container: QuickTime (.mov)
//
//  Codec::HevcHdr10 — the Windows-playable HDR delivery copy:
//  - HEVC Main10 (libx265), HDR10 static metadata (ST.2086 master-display)
//  - YUV420P10LE (10-bit 4:2:0), limited ("MPEG"/video) range, 'hvc1' tag
//  - Container: MP4 (.mp4)
//  - Requires the vcpkg ffmpeg "x265" + "gpl" features.
//
//  Both are HDR-preserving via the same front end:
//  - Transfer: SMPTE ST.2084 (PQ) so scene-linear values above 1.0
//    survive instead of being clamped to white. linear 1.0 maps to
//    the 100-nit reference; the PQ curve carries up to 10000 nits.
//  - Colour tags: BT.2020 primaries, PQ transfer, BT.2020-NCL matrix.
//  - FPS: 30 (default); dimensions rounded to even.
//
//  The renderer's working space is Rec.709-primary scene-linear RGB
//  (RISEPel == Rec709RGBPel), so each pel is converted Rec.709 -> Rec.2020
//  (linear, D65) BEFORE the PQ OETF — otherwise the 2020-tagged output
//  would be over-saturated.
//
//  Alpha is encoded LINEARLY (coverage is not a light quantity, so it
//  must not run through the PQ OETF).  The HEVC 4:2:0 path has no alpha
//  plane, so its alpha is simply dropped by swscale.
//
//////////////////////////////////////////////////////////////////////

#include "VideoEncoder.h"

#include "Interfaces/IRasterImage.h"
#include "Interfaces/ILog.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

using namespace RISE;

// Route FFmpeg's internal diagnostics (including the libx265 / muxer
// messages that explain *why* avcodec_open2 / write_header fail) into
// RISE's GlobalLog.  Without this, a codec-open failure surfaces only as
// the bare "Could not open codec" — the actionable detail (e.g. an
// unsupported pixel format on an 8-bit x265, or a bad x265-param) is lost.
static void riseFFmpegLogCallback(void* avcl, int level, const char* fmt, va_list vl)
{
    // Only surface warnings and errors; INFO/VERBOSE/DEBUG are too noisy.
    if (level > AV_LOG_WARNING) {
        return;
    }
    char line[1024];
    int printPrefix = 1;
    av_log_format_line(avcl, level, fmt, vl, line, sizeof(line), &printPrefix);
    // Trim trailing newline(s) so each FFmpeg line is one log entry.
    size_t n = std::strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;
    }
    GlobalLog()->PrintEx((level <= AV_LOG_ERROR) ? eLog_Error : eLog_Warning,
        "VideoEncoder:: FFmpeg: %s", line);
}

// Install the FFmpeg->GlobalLog bridge once.  Called from the ctor; the
// encoders are constructed on the GUI thread, so a plain guard suffices.
static void ensureFFmpegLogRouted()
{
    static bool installed = false;
    if (!installed) {
        av_log_set_callback(riseFFmpegLogCallback);
        installed = true;
    }
}

// PQ-encode one scene-linear channel into a 16-bit code value.
//
// Maps scene-linear radiance onto the SMPTE ST.2084 (PQ) OETF so that
// values above the display-referred [0,1] range are preserved rather
// than clamped.  Convention: linear 1.0 == the 100-nit PQ reference
// level; the curve carries headroom up to the 10000-nit PQ peak.
//
// The result fills the full 16-bit range; the swscale stage scales it
// down to the encoder's 10-bit working precision.
static uint16_t linearToPQ16(double linear)
{
    // Normalise scene-linear onto the PQ [0,1] domain.  100-nit
    // reference white at linear 1.0, 10000-nit PQ peak.
    double Lp = fmin(fmax(linear, 0.0), 1.0e9) * (100.0 / 10000.0);
    Lp = fmin(Lp, 1.0);

    // SMPTE ST.2084 forward (linear -> PQ code) OETF.
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    double Lm = pow(Lp, m1);
    double E = pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);

    return static_cast<uint16_t>(lround(fmin(fmax(E, 0.0), 1.0) * 65535.0));
}

// Convert one scene-linear RGB triple from Rec.709 (BT.709) primaries to
// Rec.2020 (BT.2020) primaries, both D65.
//
// The renderer's working space is Rec.709-primary scene-linear RGB
// (RISEPel == Rec709RGBPel), but the encoder tags and PQ-encodes the
// stream as BT.2020.  Without this primaries conversion the same RGB
// triples would be reinterpreted against the wider 2020 gamut and come
// out over-saturated.  This is the linear RGB-to-RGB matrix (the product
// of Rec.709 RGB->XYZ and XYZ->Rec.2020, D65 on both ends), applied to
// linear light BEFORE the PQ OETF.  In/out are double (Chel) so there is
// no intermediate narrowing.
static void rec709ToRec2020(double r, double g, double b,
                            double& outR, double& outG, double& outB)
{
    outR = 0.6274039 * r + 0.3292830 * g + 0.0433136 * b;
    outG = 0.0690973 * r + 0.9195404 * g + 0.0113623 * b;
    outB = 0.0163914 * r + 0.0880133 * g + 0.8955857 * b;
}

// The file extension that matches a codec's container.  ProRes must live
// in QuickTime (.mov); HEVC HDR10 is delivered in MP4 (.mp4).  The
// container muxer (selected in setupEncoder) and the file extension must
// always agree, so the ctor forces the path to the right extension.
static const char* extensionForCodec(VideoEncoder::Codec codec)
{
    return (codec == VideoEncoder::Codec::ProRes4444) ? ".mov" : ".mp4";
}

// True if `path` ends in `ext` (e.g. ".mov"), case-insensitive.
static bool endsWithExtension(const std::string& path, const char* ext)
{
    const size_t extLen = std::char_traits<char>::length(ext);
    if (path.size() < extLen) {
        return false;
    }
    const std::string tail = path.substr(path.size() - extLen);
    for (size_t i = 0; i < extLen; ++i) {
        if (std::tolower(static_cast<unsigned char>(tail[i])) !=
            std::tolower(static_cast<unsigned char>(ext[i]))) {
            return false;
        }
    }
    return true;
}

// Force `path` to end in `ext`.  Writing a stream into a mismatched
// container (e.g. ProRes-in-MP4, or HEVC-in-MOV) is non-standard and many
// players reject it, so the extension must match the muxer.  If the path
// already has the right extension we keep it; if it has a different
// extension we replace it; otherwise we append.  Only the final path
// component's '.' is treated as a separator so a '.' in a directory name
// is not mistaken for an extension.
static std::string forceExtension(const std::string& path, const char* ext)
{
    if (endsWithExtension(path, ext)) {
        return path;
    }
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash)) {
        return path.substr(0, dot) + ext;
    }
    return path + ext;
}

VideoEncoder::VideoEncoder(const std::string& outputPath, Codec codec, int fps)
    : m_outputPath(forceExtension(outputPath, extensionForCodec(codec)))
    , m_codec(codec)
    , m_fps(fps)
{
    ensureFFmpegLogRouted();

    const char* codecName =
        (codec == Codec::ProRes4444) ? "ProRes 4444" : "HEVC HDR10";

    if (m_outputPath != outputPath) {
        GlobalLog()->PrintEx(eLog_Event,
            "VideoEncoder:: %s requires a %s container; rewrote output path %s -> %s",
            codecName,
            (codec == Codec::ProRes4444) ? "QuickTime (.mov)" : "MP4 (.mp4)",
            outputPath.c_str(), m_outputPath.c_str());
    }

    GlobalLog()->PrintEx(eLog_Event,
        "VideoEncoder:: Created for output: %s (codec=%s, fps=%d)",
        m_outputPath.c_str(), codecName, fps);
}

VideoEncoder::~VideoEncoder()
{
    if (!m_finalized) {
        finalize();
    }

    if (m_swsCtx) sws_freeContext(m_swsCtx);
    if (m_frame) av_frame_free(&m_frame);
    if (m_packet) av_packet_free(&m_packet);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    if (m_formatCtx) {
        if (m_formatCtx->pb) avio_closep(&m_formatCtx->pb);
        avformat_free_context(m_formatCtx);
    }
}

void VideoEncoder::OutputIntermediateImage(
    const IRasterImage& /*pImage*/, const Rect* /*pRegion*/)
{
    // No-op for video output, matching Mac app behavior
}

void VideoEncoder::OutputImage(
    const IRasterImage& pImage, const Rect* /*pRegion*/,
    const unsigned int frame)
{
    int w = pImage.GetWidth();
    int h = pImage.GetHeight();

    // A prior frame already failed to initialise the encoder (e.g. the
    // HEVC path on an ffmpeg build without libx265).  Don't retry per
    // frame — that would re-allocate (and leak) a format context and spam
    // the log on every frame.
    if (m_setupFailed) {
        return;
    }

    // Initialize on first frame
    if (!m_started) {
        if (!setupEncoder(w, h)) {
            m_setupFailed = true;
            GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Failed to initialize encoder");
            return;
        }
        m_started = true;
    }

    // Convert IRasterImage to a 16-bit RGBA buffer (matches swscale's
    // AV_PIX_FMT_RGBA64LE source).  RGB is PQ-encoded so scene-linear
    // values above 1.0 survive; alpha (coverage) is encoded linearly.
    std::vector<uint16_t> rgbaData(static_cast<size_t>(m_width) * m_height * 4);
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            RISEColor c = pImage.GetPEL(x, y);
            size_t idx = (static_cast<size_t>(y) * m_width + x) * 4;

            // Clamp scene-linear RGB at 0 (negative radiance is
            // unphysical), convert Rec.709 -> Rec.2020 primaries, THEN
            // PQ-encode in the 2020 gamut the stream is tagged with.
            double r = std::max(c.base.r, 0.0);
            double g = std::max(c.base.g, 0.0);
            double b = std::max(c.base.b, 0.0);
            double R, G, B;
            rec709ToRec2020(r, g, b, R, G, B);

            rgbaData[idx + 0] = linearToPQ16(R);
            rgbaData[idx + 1] = linearToPQ16(G);
            rgbaData[idx + 2] = linearToPQ16(B);
            // Alpha is coverage, not light: encode linearly, no gamut/OETF.
            rgbaData[idx + 3] = static_cast<uint16_t>(
                lround(std::min(std::max(c.a, 0.0), 1.0) * 65535.0));
        }
    }

    encodeFrame(rgbaData.data(), m_width, m_height, frame);
    m_framesReceived++;

    GlobalLog()->PrintEx(eLog_Event,
        "VideoEncoder:: Encoded frame %u (%u total)", frame, m_framesReceived);
}

bool VideoEncoder::setupEncoder(int width, int height)
{
    // Round to even dimensions (matching Mac app)
    m_width = (width + 1) & ~1;
    m_height = (height + 1) & ~1;

    const bool isProRes = (m_codec == Codec::ProRes4444);

    // ProRes -> QuickTime (.mov); HEVC HDR10 -> MP4 (.mp4).  Force the
    // muxer explicitly rather than guessing from the path extension (the
    // ctor has already forced the path to match the chosen muxer).
    const char* muxerName = isProRes ? "mov" : "mp4";

    GlobalLog()->PrintEx(eLog_Event,
        "VideoEncoder:: Setting up %s encoder: %dx%d -> %dx%d",
        isProRes ? "ProRes 4444" : "HEVC HDR10 (Main10)",
        width, height, m_width, m_height);

    int ret = avformat_alloc_output_context2(&m_formatCtx, nullptr, muxerName, m_outputPath.c_str());
    if (ret < 0 || !m_formatCtx) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not create output context");
        return false;
    }

    // Find the encoder.
    const AVCodec* codec = nullptr;
    if (isProRes) {
        // Prefer prores_ks: it supports the 4444 profile, an alpha plane,
        // 10-bit output, and the "profile" option.  Fall back to the
        // generic ProRes encoder if prores_ks is absent.
        codec = avcodec_find_encoder_by_name("prores_ks");
        if (codec) {
            GlobalLog()->PrintEx(eLog_Event, "VideoEncoder:: Using prores_ks encoder");
        } else {
            codec = avcodec_find_encoder(AV_CODEC_ID_PRORES);
            if (codec) {
                // The generic prores / prores_aw encoder may ignore the
                // "profile" / "qscale" priv_data options and may not emit the
                // alpha plane for a 4444 / YUVA target, so the output can
                // differ from the intended 10-bit 4:4:4+alpha ProRes 4444.
                GlobalLog()->PrintEx(eLog_Warning,
                    "VideoEncoder:: prores_ks unavailable; using generic ProRes encoder "
                    "- alpha and the 4444 profile/qscale may not be honored");
            }
        }
    } else {
        // libx265 is the only encoder in our FFmpeg build that emits 10-bit
        // HEVC Main10 with HDR10 static metadata.  It requires the vcpkg
        // ffmpeg "x265" + "gpl" features (see build/VS2022/vcpkg.json); if
        // ffmpeg has not yet been rebuilt with them, the lookup returns null
        // and only the ProRes master is written.
        codec = avcodec_find_encoder_by_name("libx265");
        if (codec) {
            GlobalLog()->PrintEx(eLog_Event, "VideoEncoder:: Using libx265 encoder");
        }
    }
    if (!codec) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: %s encoder not found",
            isProRes ? "ProRes" : "libx265 (HEVC) - rebuild ffmpeg with the x265 feature");
        return false;
    }

    // Create stream
    m_stream = avformat_new_stream(m_formatCtx, nullptr);
    if (!m_stream) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not create stream");
        return false;
    }

    // Create codec context
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not allocate codec context");
        return false;
    }

    m_codecCtx->width = m_width;
    m_codecCtx->height = m_height;
    m_codecCtx->time_base = {1, m_fps};
    m_codecCtx->framerate = {m_fps, 1};
    m_codecCtx->max_b_frames = 0;   // no B-frames (keeps PTS handling simple)
    // ProRes is all-intra (every frame a keyframe).  HEVC is a delivery
    // codec — allow inter-frame compression with a ~2 s keyframe interval
    // so the .mp4 stays small; forcing all-intra HEVC would bloat it.
    m_codecCtx->gop_size = isProRes ? 1 : (2 * m_fps);
    m_codecCtx->pix_fmt  = isProRes ? AV_PIX_FMT_YUVA444P10LE   // 10-bit 4:4:4 + alpha
                                    : AV_PIX_FMT_YUV420P10LE;   // HDR10 = 10-bit 4:2:0

    // HDR colour tags: BT.2020 primaries, PQ (ST.2084) transfer, BT.2020
    // non-constant-luminance matrix.  ProRes master uses full ("JPEG")
    // range; HDR10 delivery uses the standard limited ("MPEG"/video) range
    // consumer HDR players expect.  (libx265 forwards these avctx color
    // fields into the HEVC VUI automatically.)
    m_codecCtx->color_primaries = AVCOL_PRI_BT2020;
    m_codecCtx->color_trc       = AVCOL_TRC_SMPTE2084;
    m_codecCtx->colorspace      = AVCOL_SPC_BT2020_NCL;
    m_codecCtx->color_range     = isProRes ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

    if (isProRes) {
        // ProRes 4444 profile.  Set both the option string (prores_ks reads
        // "4444" from priv_data) and the numeric profile field.  FFmpeg 7.x
        // exposes only the AV_PROFILE_* spelling (the legacy
        // FF_PROFILE_PRORES_4444 macro was removed in 7.0).
        av_opt_set(m_codecCtx->priv_data, "profile", "4444", 0);
        m_codecCtx->profile = AV_PROFILE_PRORES_4444;
        // Quality-based rate control (prores_ks "qscale", lower = better).
        av_opt_set(m_codecCtx->priv_data, "qscale", "5", 0);
    } else {
        // HEVC Main10: libx265 auto-selects the Main10 profile from the
        // 10-bit pixel format, so we don't pin avctx->profile (which could
        // conflict with that).  All HDR10 signalling goes through x265-params:
        // CRF 20 (high-quality delivery), HDR10 4:2:0 optimisation, and the
        // SMPTE ST.2086 mastering-display SEI (BT.2020 primaries + D65 white,
        // 0.0001..10000 nit range matching our PQ encode where linear 1.0 =
        // 100 nits).  x265 units: CIE xy * 50000; luminance in 0.0001 cd/m^2.
        // MaxCLL/MaxFALL left 0,0 ("unknown") — we don't pre-scan for peak.
        av_opt_set(m_codecCtx->priv_data, "preset", "medium", 0);
        av_opt_set(m_codecCtx->priv_data, "x265-params",
            "crf=20:hdr10-opt=1:"
            "master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(100000000,1):"
            "max-cll=0,0",
            0);
    }

    if (m_formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open codec
    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        GlobalLog()->PrintEx(eLog_Error,
            "VideoEncoder:: Could not open %s codec: %s (the FFmpeg:: lines above carry the specific reason)",
            isProRes ? "ProRes" : "libx265", errbuf);
        return false;
    }

    // Copy codec params to stream
    ret = avcodec_parameters_from_context(m_stream->codecpar, m_codecCtx);
    if (ret < 0) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not copy codec params");
        return false;
    }

    // HEVC-in-MP4 must use the 'hvc1' sample entry (parameter sets carried
    // in the sample description via the global header above) — Windows'
    // built-in player and QuickTime reject the muxer-default 'hev1'.
    if (!isProRes) {
        m_stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    }

    m_stream->time_base = m_codecCtx->time_base;

    // Open output file
    if (!(m_formatCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_formatCtx->pb, m_outputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not open output file");
            return false;
        }
    }

    // Write header.  For the MP4 delivery, relocate the moov atom to the
    // front (+faststart) for progressive playback; no-op for the .mov.
    AVDictionary* muxOpts = nullptr;
    if (!isProRes) {
        av_dict_set(&muxOpts, "movflags", "+faststart", 0);
    }
    ret = avformat_write_header(m_formatCtx, &muxOpts);
    av_dict_free(&muxOpts);
    if (ret < 0) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not write header");
        return false;
    }

    // Allocate frame and packet
    m_frame = av_frame_alloc();
    m_frame->format = m_codecCtx->pix_fmt;
    m_frame->width = m_width;
    m_frame->height = m_height;

    // Mirror the HDR colour tags onto the frame so the muxer records
    // the correct PQ / BT.2020 metadata for every encoded picture.
    m_frame->color_primaries = m_codecCtx->color_primaries;
    m_frame->color_trc       = m_codecCtx->color_trc;
    m_frame->colorspace      = m_codecCtx->colorspace;
    m_frame->color_range     = m_codecCtx->color_range;

    av_frame_get_buffer(m_frame, 0);

    m_packet = av_packet_alloc();

    // Create SWS context: 16-bit RGBA (our PQ-encoded RGBA64LE buffer) ->
    // the encoder's pixel format (4:4:4+alpha for ProRes, 4:2:0 for HEVC).
    m_swsCtx = sws_getContext(
        m_width, m_height, AV_PIX_FMT_RGBA64LE,
        m_width, m_height, m_codecCtx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!m_swsCtx) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not create SWS context");
        return false;
    }

    // Drive the RGB<->YUV matrix with BT.2020 coefficients.  Our RGBA64
    // input is always full range; the destination range matches the encoder
    // tag (full for ProRes, limited for HDR10).
    const int* bt2020 = sws_getCoefficients(SWS_CS_BT2020);
    const int dstRange = isProRes ? 1 : 0;
    if (sws_setColorspaceDetails(m_swsCtx,
            bt2020, 1 /*srcRange full*/,
            bt2020, dstRange,
            0 /*brightness*/, 1 << 16 /*contrast*/, 1 << 16 /*saturation*/) < 0) {
        GlobalLog()->PrintEx(eLog_Error,
            "VideoEncoder:: Could not set BT.2020 colorspace details");
        return false;
    }

    GlobalLog()->PrintEx(eLog_Event,
        "VideoEncoder:: Encoder initialized. %s, FPS=%d",
        isProRes ? "ProRes 4444 (YUVA444P10LE, PQ/BT.2020, full range)"
                 : "HEVC HDR10 (YUV420P10LE Main10, PQ/BT.2020, limited range)",
        m_fps);

    return true;
}

bool VideoEncoder::encodeFrame(const uint16_t* rgbaData, int width, int height, unsigned int frameNum)
{
    if (!m_codecCtx || !m_frame || !m_swsCtx) return false;

    av_frame_make_writable(m_frame);

    // Convert 16-bit RGBA (RGBA64LE) to 10-bit YUVA 4:4:4.  swscale
    // works in bytes, so the stride is the 16-bit row width in bytes
    // (4 channels * 2 bytes) and the slice pointer is reinterpreted as
    // raw bytes.
    const uint8_t* srcSlice[1] = { reinterpret_cast<const uint8_t*>(rgbaData) };
    int srcStride[1] = { width * 4 * static_cast<int>(sizeof(uint16_t)) };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, height,
              m_frame->data, m_frame->linesize);

    m_frame->pts = frameNum;

    // Send frame to encoder
    int ret = avcodec_send_frame(m_codecCtx, m_frame);
    if (ret < 0) return false;

    // Receive encoded packets
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;

        av_packet_rescale_ts(m_packet, m_codecCtx->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;

        av_interleaved_write_frame(m_formatCtx, m_packet);
        av_packet_unref(m_packet);
    }

    return true;
}

void VideoEncoder::flushEncoder()
{
    if (!m_codecCtx) return;

    avcodec_send_frame(m_codecCtx, nullptr);

    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        av_packet_rescale_ts(m_packet, m_codecCtx->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;

        av_interleaved_write_frame(m_formatCtx, m_packet);
        av_packet_unref(m_packet);
    }
}

void VideoEncoder::finalize()
{
    if (m_finalized) return;
    m_finalized = true;

    if (m_started && m_formatCtx) {
        flushEncoder();
        av_write_trailer(m_formatCtx);

        GlobalLog()->PrintEx(eLog_Event,
            "VideoEncoder:: Finalized. Total frames: %u, Output: %s",
            m_framesReceived, m_outputPath.c_str());
    }
}
