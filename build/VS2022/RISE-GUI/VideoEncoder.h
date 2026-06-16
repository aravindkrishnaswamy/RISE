//////////////////////////////////////////////////////////////////////
//
//  VideoEncoder.h - FFmpeg-based ProRes 4444 QuickTime encoder.
//
//  Implements IRasterizerOutput for animation video export.
//  Windows equivalent of the Mac app's MovieRasterizerOutput
//  (which uses AVFoundation).
//
//  HDR-preserving: scene-linear pels are PQ-encoded (SMPTE ST.2084)
//  into a 10-bit 4:4:4 + alpha ProRes 4444 stream so values above the
//  display-referred [0,1] range survive instead of being clamped.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIDEOENCODER_H
#define VIDEOENCODER_H

#include "Interfaces/IRasterizerOutput.h"
#include "Utilities/Reference.h"

#include <cstdint>
#include <string>

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class VideoEncoder
    : public virtual RISE::IRasterizerOutput
    , public virtual RISE::Implementation::Reference
{
public:
    // Which HDR master/delivery format this instance encodes.
    enum class Codec {
        ProRes4444,   // QuickTime .mov, YUVA444P10LE, full-range PQ/BT.2020
                      // — the high-quality HDR master.
        HevcHdr10,    // MP4 .mp4, YUV420P10LE Main10, limited-range PQ/BT.2020
                      // (libx265, HDR10 static metadata) — Windows-playable
                      // HDR delivery copy.
    };

    VideoEncoder(const std::string& outputPath,
                 Codec codec = Codec::ProRes4444,
                 int fps = 30);
    virtual ~VideoEncoder();

    // IRasterizerOutput
    void OutputIntermediateImage(
        const RISE::IRasterImage& pImage,
        const RISE::Rect* pRegion) override;

    void OutputImage(
        const RISE::IRasterImage& pImage,
        const RISE::Rect* pRegion,
        const unsigned int frame) override;

    void finalize();

    // Post-render reporting — valid after finalize() and before the
    // rasterizer frees this output.  `outputPath` is the final container
    // path (with the codec's forced extension); `wroteOutput` is true only
    // if the encoder initialised and at least one frame was encoded (e.g.
    // false for the HEVC path on an ffmpeg build without libx265).
    const std::string& outputPath() const { return m_outputPath; }
    bool wroteOutput() const { return m_started && m_framesReceived > 0; }

private:
    bool setupEncoder(int width, int height);
    bool encodeFrame(const uint16_t* rgbaData, int width, int height, unsigned int frameNum);
    void flushEncoder();

    std::string m_outputPath;
    Codec m_codec;
    int m_fps;
    int m_width = 0;
    int m_height = 0;
    bool m_started = false;
    bool m_setupFailed = false;
    bool m_finalized = false;
    unsigned int m_framesReceived = 0;

    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVStream* m_stream = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;
    SwsContext* m_swsCtx = nullptr;
};

#endif // VIDEOENCODER_H
