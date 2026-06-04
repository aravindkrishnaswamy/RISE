//////////////////////////////////////////////////////////////////////
//
//  EXRReader.h - Definition of a class that can read raster images
//  from an OpenEXR file.
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 11, 2006
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXR_READER_
#define EXR_READER_

#include "../Interfaces/IRasterImageReader.h"
#include "../Interfaces/IReadBuffer.h"
#include "../Utilities/Reference.h"

#ifndef NO_EXR_SUPPORT
	// OpenEXR includes
	#include <ImfRgbaFile.h>
	#include <ImfInputFile.h>
	#include <ImfChannelList.h>
	#include <ImfFrameBuffer.h>
	#include <ImfIO.h>
	#include <ImfArray.h>
	#include <vector>
#endif

namespace RISE
{
	namespace Implementation
	{
	#ifndef NO_EXR_SUPPORT
		// Wrapper around our IReadBuffer to an EXR IStream
		class IStreamWrapper : public Imf::IStream
		{
		protected:
			IReadBuffer& pBuffer;

		public:
			IStreamWrapper( IReadBuffer& buffer ) : Imf::IStream( "none" ), pBuffer( buffer )
			{}

			bool read( char c[/*n*/], int n )
			{
				return pBuffer.getBytes( c, n );
			}

			uint64_t tellg()
			{
				return pBuffer.getCurPos();
			}

			void seekg( uint64_t pos )
			{
				pBuffer.seek( IBuffer::START, static_cast<int>(pos) );
			}
		};
	#endif

		class EXRReader : public virtual IRasterImageReader, public virtual Reference
		{
		protected:

		#ifndef NO_EXR_SUPPORT
			// Interleaved R,G,B,A float scanline buffer.  Read through
			// Imf::InputFile with FLOAT framebuffer slices so 32-bit
			// FLOAT EXRs round-trip at full range; half-stored EXRs are
			// converted half->float losslessly by OpenEXR.  (The former
			// Imf::RgbaInputFile path stored Imf::Rgba (FP16), turning
			// any channel > 65504 into +Inf on read — read-side twin of
			// the EXRWriter 32-bit FLOAT fix.)
			std::vector<float>		floatbuffer;
			unsigned int			img_width;
			unsigned int			img_height;
		#endif

			IReadBuffer&		pReadBuffer;
			COLOR_SPACE			color_space;
			unsigned int		buffer_start_pos;

		public:
			EXRReader( IReadBuffer& buffer, const COLOR_SPACE color_space_ );
			virtual ~EXRReader( );

			bool	BeginRead( unsigned int& width, unsigned int& height );
			void	ReadColor( RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndRead( );
		};
	}
}

#endif
