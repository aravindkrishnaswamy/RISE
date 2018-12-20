//////////////////////////////////////////////////////////////////////
//
//  TIFFWriter.h - Definition of a class that can write raster images
//  to a TIFF file.
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 4, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TIFF_WRITER_
#define TIFF_WRITER_

#include "../Interfaces/IRasterImageWriter.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TIFFWriter : public virtual IRasterImageWriter, public virtual Reference
		{
		protected:
			IWriteBuffer&		pWriteBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			unsigned int		bufH;
			COLOR_SPACE			color_space;

		public:
			TIFFWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ );

			virtual ~TIFFWriter();

			void	BeginWrite( const unsigned int width, const unsigned int height );
			void	WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndWrite( );
		};
	}
}

#endif
