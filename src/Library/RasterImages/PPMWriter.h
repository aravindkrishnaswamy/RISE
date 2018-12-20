//////////////////////////////////////////////////////////////////////
//
//  PPMWriter.h - Definition of a class that can write raster images
//  to a PPM file.
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

#ifndef PPM_WRITER_
#define PPM_WRITER_

#include "../Interfaces/IRasterImageWriter.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PPMWriter : public virtual IRasterImageWriter, public virtual Reference
		{
		protected:
			IWriteBuffer&		pWriteBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			unsigned int		bufH;
			COLOR_SPACE			color_space;

		public:
			PPMWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ );

			virtual ~PPMWriter();

			void	BeginWrite( const unsigned int width, const unsigned int height );
			void	WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndWrite( );
		};
	}
}

#endif
