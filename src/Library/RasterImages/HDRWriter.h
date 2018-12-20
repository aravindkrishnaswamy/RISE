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
				const unsigned char RGBEBase = 128;

				Scalar v = ColorMath::MaxValue(p);

				const unsigned int idx = y*bufW*4+x*4;

				if( v < 1e-32 ) {
					pBuffer[idx] = 0;
					pBuffer[idx+1] = 0;
					pBuffer[idx+2] = 0;
					pBuffer[idx+3] = 0;
				} else {
					int e;
					v = frexp(v,&e) * 255.9999/v;
					pBuffer[idx] = (unsigned char)(p.r<0?0:p.r*v);
					pBuffer[idx+1] = (unsigned char)(p.g<0?0:p.g*v);
					pBuffer[idx+2] = (unsigned char)(p.b<0?0:p.b*v);
					pBuffer[idx+3] = (e + RGBEBase);
				}
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
