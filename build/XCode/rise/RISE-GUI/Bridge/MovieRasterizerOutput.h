//////////////////////////////////////////////////////////////////////
//
//  MovieRasterizerOutput.h - An IRasterizerOutput that assembles
//  animation frames into a QuickTime .mov file using AVAssetWriter.
//  Mac-only (RISE-GUI).
//
//////////////////////////////////////////////////////////////////////

#ifndef MOVIE_RASTERIZER_OUTPUT_H
#define MOVIE_RASTERIZER_OUTPUT_H

#include "Interfaces/IRasterizerOutput.h"
#include "Utilities/Reference.h"
#include "Rendering/FileEncoderObserver.h"

#include <mutex>

#ifdef __OBJC__
@class AVAssetWriter;
@class AVAssetWriterInput;
@class AVAssetWriterInputPixelBufferAdaptor;
@class NSString;
#endif

class MovieRasterizerOutput :
    public virtual RISE::IRasterizerOutput,
    public virtual RISE::Implementation::Reference
{
public:
    /// Create a movie output that writes to the given file path.
    /// @param outputPath Full path for the .mov file (will be overwritten if it exists).
    /// @param fps Frames per second for the video timeline.
    MovieRasterizerOutput(NSString* outputPath, int fps = 30);
    virtual ~MovieRasterizerOutput();

    // IRasterizerOutput
    void OutputIntermediateImage(const RISE::IRasterImage& pImage,
                                const RISE::Rect* pRegion) override;
    void OutputImage(const RISE::IRasterImage& pImage,
                     const RISE::Rect* pRegion,
                     const unsigned int frame) override;
    void OnRasterizerFrameStoreChanged(
        RISE::Implementation::FrameStore* framestore) override;
    bool DeclaresFireArtifactRoute() const override { return true; }
    bool ProvidesFirePrimaryArtifactRoute() const override { return true; }
    bool IsFireArtifactRouteAvailable() const override { return _routeAvailable; }

    /// Flush remaining frames and close the movie file.
    bool finalize(bool publish = true);

private:
#ifdef __OBJC__
    AVAssetWriter* _writer;
    AVAssetWriterInput* _input;
    AVAssetWriterInputPixelBufferAdaptor* _adaptor;
    NSString* _outputPath;
    NSString* _writerPath;
    NSString* _primaryPattern;
#else
    void* _writer;
    void* _input;
    void* _adaptor;
    void* _outputPath;
    void* _writerPath;
    void* _primaryPattern;
#endif
    RISE::Implementation::FrameStore* _frameStore;
    mutable std::mutex _frameStoreMutex;
    RISE::IFrameEncoder* _primaryEncoder;
    RISE::FrameStoreOutput::Metadata _fireMetadata;
    std::vector<RISE::Implementation::FireFramePrimary> _framePrimaries;
    int _fps;
    bool _started;
    bool _finalized;
    bool _failed;
    bool _succeeded;
    bool _routeAvailable;
    bool _fireRender;
    bool _metadataCaptured;
    int _width;
    int _height;
    unsigned int _framesReceived;

    /// Lazily configure the AVAssetWriter on first frame (when we know the dimensions).
    bool setupWriter(int width, int height);
};

#endif
