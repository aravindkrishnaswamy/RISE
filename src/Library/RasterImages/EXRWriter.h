//////////////////////////////////////////////////////////////////////
//
//  EXRWriter.h - Definition of a class that can write raster images
//  to an OpenEXR file.
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 10, 2006
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXR_WRITER_
#define EXR_WRITER_

#include "../Interfaces/IRasterImageWriter.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Utilities/Reference.h"
#include "EXRCompression.h"

#ifndef NO_EXR_SUPPORT
	// OpenEXR includes
	#include <ImfRgbaFile.h>
	#include <ImfOutputFile.h>
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
		// Wrapper around our IWriteBuffer to an EXR OStream
		class OStreamWrapper : public Imf::OStream
		{
		protected:
			IWriteBuffer& pBuffer;

		public:
			OStreamWrapper( IWriteBuffer& buffer ) : Imf::OStream( "none" ), pBuffer( buffer )
			{}

			void write( const char c[/*n*/], int n )
			{
				pBuffer.ResizeForMore(n);
				pBuffer.setBytes( c, n );
			}

			uint64_t tellp()
			{
				return pBuffer.getCurPos();
			}

			void seekp( uint64_t pos )
			{
				pBuffer.seek( IBuffer::START, static_cast<int>(pos) );
			}
		};
	#endif

		class EXRWriter : public virtual IRasterImageWriter, public virtual Reference
		{
		protected:

		#ifndef NO_EXR_SUPPORT
			OStreamWrapper			out;
			Imf::RgbaOutputFile*	exrout;			///< half (FP16) path; NULL when write_float
			Imf::OutputFile*		exrout_float;	///< 32-bit FLOAT path; NULL unless write_float
			Imf::Array2D<Imf::Rgba>	exrbuffer;		///< half scanline buffer
			std::vector<float>		floatbuffer;	///< interleaved R,G,B,A float scanline buffer (write_float)
		#endif

			IWriteBuffer&			buffer;
			COLOR_SPACE				color_space;
			EXR_COMPRESSION			compression;
			bool					with_alpha;
			bool					write_float;	///< true => 32-bit FLOAT channels (no FP16 65504 clamp); false => half
			unsigned int			horzpixels;
			unsigned int			scanlines;

		#ifndef NO_EXR_SUPPORT
			template< typename T >
			void WriteColorToEXRBuffer( const T& c, const unsigned int x, const unsigned int y )
			{
				// OpenEXR is defined as premultiplied alpha for transparency.
				if( write_float ) {
					// 32-bit FLOAT path: stores the full linear-radiance range.
					// half (FP16) caps at 65504, which silently turns legitimate
					// bright HDR pixels (caustic / specular fireflies) into +Inf.
					const std::size_t idx =
						( static_cast<std::size_t>( y ) * horzpixels + x ) * 4u;
					floatbuffer[idx + 0u] = static_cast<float>( c.base.r * c.a );
					floatbuffer[idx + 1u] = static_cast<float>( c.base.g * c.a );
					floatbuffer[idx + 2u] = static_cast<float>( c.base.b * c.a );
					floatbuffer[idx + 3u] = static_cast<float>( c.a );
				} else {
					exrbuffer[y][x].r = half( static_cast<float>( c.base.r*c.a ) );
					exrbuffer[y][x].g = half( static_cast<float>( c.base.g*c.a ) );
					exrbuffer[y][x].b = half( static_cast<float>( c.base.b*c.a ) );
					exrbuffer[y][x].a = half( static_cast<float>( c.a ) );
				}
			}
		#endif

		public:
			EXRWriter(
				IWriteBuffer&         buffer,
				const COLOR_SPACE     color_space_,
				const EXR_COMPRESSION compression_ = eExrCompression_Piz,
				const bool            with_alpha_  = true,
				const bool            write_float_ = false );

			virtual ~EXRWriter();

			void	BeginWrite( const unsigned int width, const unsigned int height );
			void	WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndWrite( );
		};
	}
}

#endif
