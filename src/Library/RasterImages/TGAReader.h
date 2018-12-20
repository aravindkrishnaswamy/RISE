//////////////////////////////////////////////////////////////////////
//
//  TGAReader.h - Definition of a class that can read raster images
//  from a TGA file.
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 24, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TGA_READER_
#define TGA_READER_

#include "../Interfaces/IRasterImageReader.h"
#include "../Interfaces/IReadBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TGAReader : public virtual IRasterImageReader, public virtual Reference
		{
		protected:
			IReadBuffer&		pReadBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			COLOR_SPACE			color_space;

		public:
			TGAReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ );
			virtual ~TGAReader( );

			bool	BeginRead( unsigned int& width, unsigned int& height );
			void	ReadColor( RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndRead( );
		};
	}
}

#endif
