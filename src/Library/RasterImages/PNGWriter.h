//////////////////////////////////////////////////////////////////////
//
//  PNGWriter.h - Definition of a class that can write raster images
//  to a PNG MemoryBuffer.
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

#ifndef PNG_WRITER_
#define PNG_WRITER_

#include "../Interfaces/IRasterImageWriter.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		// Thread-local fault hook for codec error propagation regressions.
		// Returns the previous setting so tests can restore it with RAII.
		bool ForTest_SetPNGWriteFailureAfterHeader( bool enabled );
		bool ForTest_PNGWriteFailureAfterHeader();

		class PNGWriter : public virtual IRasterImageWriter, public virtual Reference
		{
		protected:
			IWriteBuffer&		pWriteBuffer;
			unsigned char*		pBuffer;
			unsigned int		bufW;
			unsigned int		bufH;
			unsigned char		bpp;

			COLOR_SPACE			color_space;

		public:
			PNGWriter( IWriteBuffer& buffer, const unsigned char bpp, const COLOR_SPACE color_space_ );

			virtual ~PNGWriter();

			void	BeginWrite( const unsigned int width, const unsigned int height );
			void	WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndWrite( );
		};
	}
}

#endif
