//////////////////////////////////////////////////////////////////////
//
//  VideoEncoder.h - FFmpeg-based H.264 MP4 encoder.
//
//  Implements IRasterizerOutput for animation video export.
//  Windows equivalent of the Mac app's MovieRasterizerOutput
//  (which uses AVFoundation).
//
//////////////////////////////////////////////////////////////////////

#ifndef VIDEOENCODER_H
#define VIDEOENCODER_H

#include "Interfaces/IRasterizerOutput.h"
#include "Utilities/Reference.h"

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
    VideoEncoder(const std::string& outputPath, int fps = 30);
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

private:
    bool setupEncoder(int width, int height);
    bool encodeFrame(const uint8_t* rgbaData, int width, int height, unsigned int frameNum);
    void flushEncoder();

    std::string m_outputPath;
    int m_fps;
    int m_width = 0;
    int m_height = 0;
    bool m_started = false;
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
