//////////////////////////////////////////////////////////////////////
//
//  TIFFCommon.cpp - Common functionf or the TIFF reader/writer
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

#ifndef TIFF_COMMON_
#define TIFF_COMMON_

#ifndef NO_TIFF_SUPPORT

namespace RISE
{
	namespace Implementation
	{
		static tsize_t TIFFReadDummy( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			return 0;
		}

		static tsize_t TIFFRead( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			IReadBuffer* pBuffer = (IReadBuffer*)v;

			if( pBuffer && pBuffer->getBytes( ptr, amount ) ) {
				return amount;
			}

			return 0;
		}

		static toff_t TIFFSeekReader( thandle_t v, toff_t off, int whence )
		{
			// libtiff uses this as a special code, so avoid accepting it
			if( off == 0xFFFFFFFF ) {
				return 0xFFFFFFFF;
			}

			IReadBuffer* pBuffer = (IReadBuffer*)v;
			IBuffer::eSeek sk;

			switch(whence)
			{
			case 0:			// SEEK_SET
				sk = IBuffer::START;
				break;
			case 1:			// SEEK_CUR
				sk = IBuffer::CUR;
				break;
			case 2:			/// SEEK_END
				sk = IBuffer::END;
				break;
			default:
				sk = IBuffer::START;
				break;
			}

			if( pBuffer && pBuffer->seek( sk, off ) ) {
				return off;
			}

			return 0;
		}

		static toff_t TIFFSeekWriter( thandle_t v, toff_t off, int whence )
		{
			// libtiff uses this as a special code, so avoid accepting it
			if( off == 0xFFFFFFFF ) {
				return 0xFFFFFFFF;
			}

			IWriteBuffer* pBuffer = (IWriteBuffer*)v;
			IBuffer::eSeek sk;

			switch(whence)
			{
			case 0:			// SEEK_SET
				sk = IBuffer::START;
				break;
			case 1:			// SEEK_CUR
				sk = IBuffer::CUR;
				break;
			case 2:			/// SEEK_END
				sk = IBuffer::END;
				break;
			default:
				sk = IBuffer::START;
				break;
			}

			if( pBuffer && pBuffer->seek( sk, off ) ) {
				return off;
			}

			return 0;
		}

		static tsize_t TIFFWrite( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			IWriteBuffer* pBuffer = (IWriteBuffer*)v;

			if( pBuffer && pBuffer->setBytes( ptr, amount ) ) {
				return amount;
			}

			return 0;
		}

		static tsize_t TIFFWriteDummy( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			return 0;
		}

		static int TIFFClose( thandle_t v )
		{
			return 0;
		}

		static toff_t TIFFSizeReader( thandle_t v )
		{
			IReadBuffer* pBuffer = (IReadBuffer*)v;

			if( pBuffer ) {
				return pBuffer->Size();
			} 

			return 0;
		}

		static toff_t TIFFSizeWriter( thandle_t v )
		{
			IWriteBuffer* pBuffer = (IWriteBuffer*)v;

			if( pBuffer ) {
				return pBuffer->Size();
			} 

			return 0;
		}

		static int TIFFMapFile( thandle_t, tdata_t*, toff_t* )
		{
			return 0;
		}

		static void TIFFUnmapFile( thandle_t, tdata_t, toff_t )
		{
		}
	}
}

#endif
#endif

