//////////////////////////////////////////////////////////////////////
//
//  IJobRasterizerOutput.h - This the interface that applications must
//    implement if they want their own custom render output call back
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IJOB_RASTERIZER_OUTPUT_
#define IJOB_RASTERIZER_OUTPUT_

namespace RISE
{
	//! Very basic interface that applications must implement if they want
	//! to get access to the rasterized output
	class IJobRasterizerOutput
	{
	public:
		virtual ~IJobRasterizerOutput(){}
	
		//! Asks if you want premultiplied alpha
		virtual bool PremultipliedAlpha() = 0;

		//! Asks what colorspace is requested
		virtual int GetColorSpace() = 0;

		//! Outputs an intermediate region of rasterized data
		//! The rasterized data is a 16 bpp image
		virtual void OutputImageRGBA16( 
			const unsigned short* pImageData,			///< [in] Data pointer to rasterized image
			const unsigned int width,					///< [in] Width of the image
			const unsigned int height,					///< [in] Height of the image
			const unsigned int rc_top,					///< [in] Defines the precise rectangular area to update
			const unsigned int rc_left,					///< [in] Defines the precise rectangular area to update
			const unsigned int rc_bottom,				///< [in] Defines the precise rectangular area to update
			const unsigned int rc_right					///< [in] Defines the precise rectangular area to update
			) = 0;
	};
}

#endif

