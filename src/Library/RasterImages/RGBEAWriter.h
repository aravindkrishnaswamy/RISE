//////////////////////////////////////////////////////////////////////
//
//  RGBEAWriter.h - Definition of a class that can write raster images
//  to a custom RGBEA file format
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 4, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RGBEA_WRITER_
#define RGBEA_WRITER_

#include "../Interfaces/IRasterImageWriter.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class RGBEAWriter : public virtual IRasterImageWriter, public virtual Reference
		{
		protected:
			IWriteBuffer&		pWriteBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			unsigned int		bufH;

		public:
			RGBEAWriter( IWriteBuffer& buffer );

			virtual ~RGBEAWriter();

			void	BeginWrite( const unsigned int width, const unsigned int height );
			void	WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndWrite( );
		};
	}
}

#endif
