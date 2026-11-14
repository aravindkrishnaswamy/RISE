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
	#include <limits>
	#include <stdexcept>
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
			bool failed = false;

		public:
			IStreamWrapper( IReadBuffer& buffer ) : Imf::IStream( "none" ), pBuffer( buffer )
			{}

			bool read( char c[/*n*/], int n )
			{
				if( n < 0 || !pBuffer.getBytes( c, static_cast<unsigned int>(n) ) ) {
					failed = true;
					throw std::runtime_error("EXR input read failed");
				}
				return BufferBytesAvailable(pBuffer) != 0;
			}

			// OpenEXR can catch a final uncompressed-chunk read exception and
			// still return normally. Keep that failure visible to the decoder.
			bool Failed() const { return failed; }

			uint64_t tellg()
			{
				return BufferPosition(pBuffer);
			}

			void seekg( uint64_t pos )
			{
				if( pos > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
					!BufferSeek(pBuffer, IBuffer::START, static_cast<std::int64_t>(pos)) ) {
					failed = true;
					throw std::runtime_error("EXR input seek failed");
				}
			}
		};
	#endif

		class EXRReader : public virtual IRasterImageReader, public virtual Reference
		{
		protected:

		#ifndef NO_EXR_SUPPORT
			// Interleaved R,G,B,A float scanline buffer.  Read through
			// Imf::InputFile with FLOAT framebuffer slices for RGB and
			// plain Y/YA, so 32-bit FLOAT channels retain their full range.
			// YC/YCA uses the SDK's historical half-precision RgbaInputFile
			// reconstruction, including its chroma filters/chromaticities.
			// In the direct FLOAT path, half-stored EXRs are
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
			std::uint64_t		buffer_start_pos;

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
