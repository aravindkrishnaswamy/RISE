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

#ifndef NO_EXR_SUPPORT
	// OpenEXR includes
	#include <ImfRgbaFile.h>
	#include <ImfIO.h>
	#include <ImfArray.h>
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

			Imf::Int64 tellp()
			{
				return pBuffer.getCurPos();
			}

			void seekp( Imf::Int64 pos )
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
			Imf::RgbaOutputFile*	exrout;
			Imf::Array2D<Imf::Rgba>	exrbuffer;
		#endif

			IWriteBuffer&			buffer;
			COLOR_SPACE				color_space;
			unsigned int			horzpixels;
			unsigned int			scanlines;

		#ifndef NO_EXR_SUPPORT
			template< typename T >
			void WriteColorToEXRBuffer( const T& c, const unsigned int x, const unsigned int y )
			{
				// Convert from our scalar format to the half format
				// Note that OpenEXR is defined as a premultiplied alpha for transparency
				exrbuffer[y][x].r = half( static_cast<float>( c.base.r*c.a ) );
				exrbuffer[y][x].g = half( static_cast<float>( c.base.g*c.a ) );
				exrbuffer[y][x].b = half( static_cast<float>( c.base.b*c.a ) );
				exrbuffer[y][x].a = half( static_cast<float>( c.a ) );
			}
		#endif

		public:
			EXRWriter( IWriteBuffer& buffer, const COLOR_SPACE color_space_ );

			virtual ~EXRWriter();

			void	BeginWrite( const unsigned int width, const unsigned int height );
			void	WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y );
			void	EndWrite( );
		};
	}
}

#endif
