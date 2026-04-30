//////////////////////////////////////////////////////////////////////
//
//  JPEGReader.h - Definition of a class that can read raster images
//  from a JPEG file contained in a buffer.
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 29, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef JPEG_READER_
#define JPEG_READER_

#include "../Interfaces/IRasterImageReader.h"
#include "../Interfaces/IReadBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class JPEGReader : public virtual IRasterImageReader, public virtual Reference
		{
		protected:
			IReadBuffer&		pReadBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			unsigned int		bufH;
			unsigned char		nChannels;

			COLOR_SPACE			color_space;

		public:
			JPEGReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ );
			virtual ~JPEGReader( );

			bool	BeginRead( unsigned int& width, unsigned int& height );
			void	ReadColor( RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndRead( );
		};
	}
}

#endif
