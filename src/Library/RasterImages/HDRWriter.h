//////////////////////////////////////////////////////////////////////
//
//  HDRWriter.h - Definition of a class that can write raster images
//  to a the Radiance HDR file format
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 17, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HDR_WRITER_
#define HDR_WRITER_

#include "../Interfaces/IRasterImageWriter.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Color/ColorUtils.h"
#include <cstddef>

namespace RISE
{
	namespace Implementation
	{
		class HDRWriter : public virtual IRasterImageWriter, public virtual Reference
		{
		protected:
			IWriteBuffer&		pWriteBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			unsigned int		bufH;

			COLOR_SPACE			color_space;

			template< typename T >
			void WriteHDRPixelToBuffer( const unsigned int x, const unsigned int y, const T& p )
			{
				unsigned int samples[3];unsigned char exponent;
				ColorUtils::PackSharedExponentRGB<8>(p,samples,exponent);
				const std::size_t idx=(static_cast<std::size_t>(y)*bufW+x)*4;
				for(unsigned int i=0;i<3;++i)pBuffer[idx+i]=static_cast<unsigned char>(samples[i]);
				pBuffer[idx+3]=exponent;
			}

		public:
			HDRWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ );

			virtual ~HDRWriter();

			void	BeginWrite( const unsigned int width, const unsigned int height );
			void	WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndWrite( );
		};
	}
}

#endif
