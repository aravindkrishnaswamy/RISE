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

namespace RISE
{
	namespace Implementation
	{
		static const char extensions[7][6] = { "tga", "ppm", "png", "hdr", "tiff", "rgbea", "exr" };

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

		protected:
			char				szPattern[1024];
			bool				bMultiple;
			FRO_TYPE			type;
			unsigned char		bpp;
			COLOR_SPACE			color_space;

			virtual ~FileRasterizerOutput( );

		public:
			FileRasterizerOutput( 
				const char* szPattern_,
				const bool bMultiple_,
				const FRO_TYPE type_,
				const unsigned char bpp_,
				const COLOR_SPACE color_space_
				);

			virtual void	OutputIntermediateImage( const IRasterImage& pImage, const Rect* pRegion );
			virtual void	OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame );
		};
	}
}

#endif
