//////////////////////////////////////////////////////////////////////
//
//  IRasterizerOutput.h - Defines an interface to output raster images from
//  rasterizers.  This is where the results of an image from a 
//  IRasterizer get stored
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRASTERIZEROUTPUT_
#define IRASTERIZEROUTPUT_

#include "IReference.h"
#include "IRasterImage.h"

namespace RISE
{
	class IRasterizerOutput : public virtual IReference
	{
	protected:
		IRasterizerOutput( ){};
		virtual ~IRasterizerOutput( ){};

	public:
		//! Outputs an intermediate scanline of rasterized data
		virtual void OutputIntermediateImage( 
			const IRasterImage& pImage,					///< [in] Rasterized image
			const Rect* pRegion							///< [in] Rasterized region, if its NULL then the entire image should be output
			) = 0;

		//! A full rasterization was complete, and the full image should be output
		virtual void OutputImage(
			const IRasterImage& pImage,					///< [in] Rasterized image
			const Rect* pRegion,						///< [in] Rasterized region, if its NULL then the entire image should be output
			const unsigned int frame					///< [in] The frame we are outputting
			) = 0;

		//! Pre-denoised (but fully splatted) image, emitted by rasterizers
		//! only when OIDN denoising is enabled.  File-based outputs should
		//! write this using the normal filename; other outputs default to a
		//! no-op so they only observe the denoised final via OutputImage.
		virtual void OutputPreDenoisedImage(
			const IRasterImage& /*pImage*/,
			const Rect* /*pRegion*/,
			const unsigned int /*frame*/
			)
		{
		}

		//! Denoised final image, emitted by rasterizers only when OIDN
		//! denoising is enabled.  File-based outputs should write this with
		//! "_denoised" appended to the filename stem.  The default
		//! implementation forwards to OutputImage so non-file outputs
		//! continue to observe the denoised final (preserving existing
		//! window/store/callback behavior).
		virtual void OutputDenoisedImage(
			const IRasterImage& pImage,
			const Rect* pRegion,
			const unsigned int frame
			)
		{
			OutputImage( pImage, pRegion, frame );
		}
	};
}

#endif
