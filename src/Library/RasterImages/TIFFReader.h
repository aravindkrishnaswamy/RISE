//////////////////////////////////////////////////////////////////////
//
//  TIFFReader.h - Definition of a class that can read raster images
//  from a TIFF file.
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 3, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TIFF_READER_
#define TIFF_READER_

#include "../Interfaces/IRasterImageReader.h"
#include "../Interfaces/IReadBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TIFFReader : public virtual IRasterImageReader, public virtual Reference
		{
		protected:
			IReadBuffer&		pReadBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			COLOR_SPACE			color_space;

		public:
			TIFFReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ );
			virtual ~TIFFReader( );

			bool	BeginRead( unsigned int& width, unsigned int& height );
			void	ReadColor( RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndRead( );
		};
	}
}

#endif
