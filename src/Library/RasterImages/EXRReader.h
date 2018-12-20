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
	#include <ImfIO.h>
	#include <ImfArray.h>
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

			Imf::Int64 tellg()
			{
				return pBuffer.getCurPos();
			}

			void seekg( Imf::Int64 pos )
			{
				pBuffer.seek( IBuffer::START, static_cast<int>(pos) );
			}
		};
	#endif

		class EXRReader : public virtual IRasterImageReader, public virtual Reference
		{
		protected:

		#ifndef NO_EXR_SUPPORT
			Imf::Array2D<Imf::Rgba>	exrbuffer;
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
