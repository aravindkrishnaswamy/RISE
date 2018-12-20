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
	};
}

#endif
