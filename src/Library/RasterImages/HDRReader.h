//////////////////////////////////////////////////////////////////////
//
//  HDRReader.h - Definition of a class that can read raster images
//  from a the Radiance RGBE HDR file
//                
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 17, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HDR_READER_
#define HDR_READER_

#include "../Interfaces/IRasterImageReader.h"
#include "../Interfaces/IReadBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class HDRReader : public virtual IRasterImageReader, public virtual Reference
		{
		protected:
			IReadBuffer&		pReadBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			COLOR_SPACE			color_space;

		public:
			HDRReader( IReadBuffer& buffer );
			virtual ~HDRReader( );

			bool	BeginRead( unsigned int& width, unsigned int& height );
			void	ReadColor( RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndRead( );
		};
	}
}

#endif
