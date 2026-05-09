//////////////////////////////////////////////////////////////////////
//
//  FileRasterizerOutput.h - A rasterizer output object that writes to a file
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments: Region updates are not supported by the file rasterizer output
//            the entire image is dumped 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FILE_RASTERIZEROUTPUT_
#define FILE_RASTERIZEROUTPUT_

#include "../Interfaces/IRasterizerOutput.h"
#include "../Utilities/Reference.h"
#include "../RasterImages/EXRCompression.h"
#include "DisplayTransform.h"

namespace RISE
{
	namespace Implementation
	{
		static const char extensions[7][6] = { "tga", "ppm", "png", "hdr", "tiff", "rgbea", "exr" };

		// Forward declarations for the L3 lazy-allocated chain.
		class FrameStore;
		class FrameSink;
		class FileEncoderObserver;

		class FileRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
		{
		public:
			enum FRO_TYPE
			{
				TGA		= 0,
				PPM		= 1,
				PNG		= 2,
				HDR		= 3,
				TIFF	= 4,
				RGBEA	= 5,
				EXR		= 6
			};

			//! HDR formats are radiometric: tone-mapping them
			//! corrupts the archival radiance.  Used by
			//! WriteImageToFile to decide whether to wrap the writer
			//! in a DisplayTransformWriter.
			static bool IsHDRFormat( const FRO_TYPE t )
			{
				return t == HDR || t == RGBEA || t == EXR;
			}

		protected:
			char				szPattern[1024];
			bool				bMultiple;
			FRO_TYPE			type;
			unsigned char		bpp;
			COLOR_SPACE			color_space;

			// New (Landing 1) — display pipeline
			Scalar				exposureEV;			///< Exposure offset in EV stops (0 = no scaling)
			DISPLAY_TRANSFORM	display_transform;	///< Tone curve (None for HDR types)

			// Landing 5 — photographic exposure compensation supplied
			// by the scene's camera (via SetCameraExposureCompensationEV
			// at frame start).  Stacks ADDITIVELY with `exposureEV` on
			// LDR outputs; ignored on HDR archival outputs.  Defaults
			// to 0 so non-physical cameras (and any output that the
			// rasterizer never calls SetCameraExposureCompensationEV
			// on) keep pre-L5 behaviour bit-identically.
			Scalar				cameraExposureEV;

			// New (Landing 1) — EXR-specific knobs (ignored for non-EXR types)
			EXR_COMPRESSION		exr_compression;
			bool				exr_with_alpha;

			// L3 — lazy-allocated FrameStore + sink + observer chain.
			// Allocated on first OutputImage / OutputPreDenoisedImage /
			// OutputDenoisedImage call (we need the rasterizer's image
			// dims, which aren't known at construction time).  The
			// trio is reused across subsequent frames in the
			// bMultiple animation case: only the per-frame OnFrameComplete
			// callback advances; the FrameStore buffer is overwritten
			// in place by FrameSink::CopyImageIntoStore.
			FrameStore*           framestore_       = nullptr;
			FrameSink*            framesink_        = nullptr;
			FileEncoderObserver*  encoderObserver_  = nullptr;

			virtual ~FileRasterizerOutput( );

			//! Lazy-allocate the FrameStore + FrameSink +
			//! FileEncoderObserver chain on first Output* call.
			//! After this returns, framestore_/framesink_/observer_
			//! are non-null and the observer is registered on the
			//! store.  Idempotent: subsequent calls with the same
			//! dims are no-ops.  Asserts in debug if dims change
			//! across calls (would require reallocating the chain).
			void EnsureChain( unsigned int width, unsigned int height );

		public:
			FileRasterizerOutput(
				const char* szPattern_,
				const bool bMultiple_,
				const FRO_TYPE type_,
				const unsigned char bpp_,
				const COLOR_SPACE color_space_,
				const Scalar exposureEV_,
				const DISPLAY_TRANSFORM display_transform_,
				const EXR_COMPRESSION exr_compression_,
				const bool exr_with_alpha_
				);

			void	OutputIntermediateImage( const IRasterImage& pImage, const Rect* pRegion ) override;
			void	OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame ) override;
			void	OutputPreDenoisedImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame ) override;
			void	OutputDenoisedImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame ) override;

			//! Landing 5: receive the scene camera's photographic exposure
			//! compensation in EV stops.  Called by the rasterizer at frame
			//! start; stacked with the static `exposureEV` parameter at
			//! WriteImageToFile time.  HDR formats clear it to 0 (consistent
			//! with their treatment of `exposureEV` from Landing 1).
			void			SetCameraExposureCompensationEV( Scalar ev ) override;
		};
	}
}

#endif
