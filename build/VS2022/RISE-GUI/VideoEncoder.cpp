//////////////////////////////////////////////////////////////////////
//
//  VideoEncoder.cpp - FFmpeg H.264 MP4 encoder implementation.
//
//  Ported from the Mac app's MovieRasterizerOutput.mm which uses
//  AVFoundation. This version uses FFmpeg libavcodec/libavformat.
//
//  Encoding parameters match the Mac app:
//  - Codec: H.264 High profile
//  - Container: MP4
//  - FPS: 30 (default)
//  - Bitrate: width * height * 4
//  - Dimensions rounded to even (H.264 requirement)
//
//////////////////////////////////////////////////////////////////////

#include "VideoEncoder.h"

#include "Interfaces/IRasterImage.h"
#include "Interfaces/ILog.h"

#include <vector>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

using namespace RISE;

VideoEncoder::VideoEncoder(const std::string& outputPath, int fps)
    : m_outputPath(outputPath)
    , m_fps(fps)
{
    GlobalLog()->PrintEx(eLog_Event,
        "VideoEncoder:: Created for output: %s (fps=%d)", outputPath.c_str(), fps);
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

    // Initialize on first frame
    if (!m_started) {
        if (!setupEncoder(w, h)) {
            GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Failed to initialize encoder");
            return;
        }
        m_started = true;
    }

    // Convert IRasterImage to RGBA8 buffer
    std::vector<uint8_t> rgbaData(m_width * m_height * 4);
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            RISEColor c = pImage.GetPEL(x, y);
            size_t idx = (y * m_width + x) * 4;
            rgbaData[idx + 0] = static_cast<uint8_t>(std::min(std::max(c.base.r, 0.0), 1.0) * 255.0 + 0.5);
            rgbaData[idx + 1] = static_cast<uint8_t>(std::min(std::max(c.base.g, 0.0), 1.0) * 255.0 + 0.5);
            rgbaData[idx + 2] = static_cast<uint8_t>(std::min(std::max(c.base.b, 0.0), 1.0) * 255.0 + 0.5);
            rgbaData[idx + 3] = static_cast<uint8_t>(std::min(std::max(c.a, 0.0), 1.0) * 255.0 + 0.5);
        }
    }

    encodeFrame(rgbaData.data(), m_width, m_height, frame);
    m_framesReceived++;

    GlobalLog()->PrintEx(eLog_Event,
        "VideoEncoder:: Encoded frame %u (%u total)", frame, m_framesReceived);
}

bool VideoEncoder::setupEncoder(int width, int height)
{
    // Round to even dimensions (H.264 requirement, matching Mac app)
    m_width = (width + 1) & ~1;
    m_height = (height + 1) & ~1;

    GlobalLog()->PrintEx(eLog_Event,
        "VideoEncoder:: Setting up H.264 encoder: %dx%d -> %dx%d",
        width, height, m_width, m_height);

    // Create output format context
    int ret = avformat_alloc_output_context2(&m_formatCtx, nullptr, nullptr, m_outputPath.c_str());
    if (ret < 0 || !m_formatCtx) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not create output context");
        return false;
    }

    // Find H.264 encoder
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: H.264 codec not found");
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

    // Set encoding parameters (matching Mac app)
    m_codecCtx->bit_rate = static_cast<int64_t>(m_width) * m_height * 4;
    m_codecCtx->width = m_width;
    m_codecCtx->height = m_height;
    m_codecCtx->time_base = {1, m_fps};
    m_codecCtx->framerate = {m_fps, 1};
    m_codecCtx->gop_size = 12;
    m_codecCtx->max_b_frames = 2;
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    // H.264 High profile (matching Mac app's AVVideoProfileLevelH264HighAutoLevel)
    av_opt_set(m_codecCtx->priv_data, "profile", "high", 0);
    av_opt_set(m_codecCtx->priv_data, "preset", "medium", 0);

    if (m_formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open codec
    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not open codec");
        return false;
    }

    // Copy codec params to stream
    ret = avcodec_parameters_from_context(m_stream->codecpar, m_codecCtx);
    if (ret < 0) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not copy codec params");
        return false;
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

    // Write header
    ret = avformat_write_header(m_formatCtx, nullptr);
    if (ret < 0) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not write header");
        return false;
    }

    // Allocate frame and packet
    m_frame = av_frame_alloc();
    m_frame->format = m_codecCtx->pix_fmt;
    m_frame->width = m_width;
    m_frame->height = m_height;
    av_frame_get_buffer(m_frame, 0);

    m_packet = av_packet_alloc();

    // Create SWS context for RGBA -> YUV420P conversion
    m_swsCtx = sws_getContext(
        m_width, m_height, AV_PIX_FMT_RGBA,
        m_width, m_height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!m_swsCtx) {
        GlobalLog()->PrintEx(eLog_Error, "VideoEncoder:: Could not create SWS context");
        return false;
    }

    GlobalLog()->PrintEx(eLog_Event,
        "VideoEncoder:: Encoder initialized. Bitrate=%lld, FPS=%d",
        m_codecCtx->bit_rate, m_fps);

    return true;
}

bool VideoEncoder::encodeFrame(const uint8_t* rgbaData, int width, int height, unsigned int frameNum)
{
    if (!m_codecCtx || !m_frame || !m_swsCtx) return false;

    av_frame_make_writable(m_frame);

    // Convert RGBA to YUV420P
    const uint8_t* srcSlice[1] = { rgbaData };
    int srcStride[1] = { width * 4 };
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
