//////////////////////////////////////////////////////////////////////
//
//  PNGReader.h - Definition of a class that can read raster images
//  from a PNG file contained in a buffer.
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 13, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PNG_READER_
#define PNG_READER_

#include "../Interfaces/IRasterImageReader.h"
#include "../Interfaces/IReadBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PNGReader : public virtual IRasterImageReader, public virtual Reference
		{
		protected:
			IReadBuffer&		pReadBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			bool				bAlphaInfo;
			unsigned char		bpp;

			COLOR_SPACE			color_space;

		public:
			PNGReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ );
			virtual ~PNGReader( );

			bool	BeginRead( unsigned int& width, unsigned int& height );
			void	ReadColor( RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndRead( );
		};
	}
}

#endif
