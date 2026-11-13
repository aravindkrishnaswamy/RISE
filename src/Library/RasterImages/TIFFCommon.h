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

#include <limits>

namespace RISE
{
	namespace Implementation
	{
		__attribute__((unused))
		static tsize_t TIFFReadDummy( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			return 0;
		}

		__attribute__((unused))
		static tsize_t TIFFRead( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			IReadBuffer* pBuffer = (IReadBuffer*)v;

			if( pBuffer && amount >= 0 &&
				static_cast<toff_t>(amount) <= std::numeric_limits<unsigned int>::max() &&
				pBuffer->getBytes( ptr, static_cast<unsigned int>(amount) ) ) {
				return amount;
			}

			return 0;
		}

		static inline toff_t TIFFSeek( IBuffer* buffer, toff_t offset, int whence )
		{
			constexpr toff_t failure = static_cast<toff_t>(-1);
			if( !buffer || offset > static_cast<toff_t>(std::numeric_limits<int>::max()) )
				return failure;
			switch( whence ) {
			case SEEK_CUR: offset += buffer->getCurPos(); break;
			case SEEK_END: offset += buffer->Size(); break;
			case SEEK_SET: break;
			default: return failure;
			}
			if( offset > static_cast<toff_t>(std::numeric_limits<int>::max()) )
				return failure;
			if( offset != buffer->getCurPos() &&
				!buffer->seek( IBuffer::START, static_cast<int>(offset) ) )
				return failure;
			return buffer->getCurPos();
		}

		[[maybe_unused]] static toff_t TIFFSeekReader( thandle_t handle, toff_t offset, int whence )
		{
			return TIFFSeek( static_cast<IReadBuffer*>(handle), offset, whence );
		}

		[[maybe_unused]] static toff_t TIFFSeekWriter( thandle_t handle, toff_t offset, int whence )
		{
			return TIFFSeek( static_cast<IWriteBuffer*>(handle), offset, whence );
		}

		__attribute__((unused))
		static tsize_t TIFFWrite( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			IWriteBuffer* pBuffer = (IWriteBuffer*)v;

			if( pBuffer && amount >= 0 &&
				static_cast<toff_t>(amount) <= std::numeric_limits<unsigned int>::max() &&
				pBuffer->setBytes( ptr, static_cast<unsigned int>(amount) ) ) {
				return amount;
			}

			return 0;
		}

		__attribute__((unused))
		static tsize_t TIFFWriteDummy( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			return 0;
		}

		__attribute__((unused))
		static int TIFFClose( thandle_t v )
		{
			return 0;
		}

		__attribute__((unused))
		static toff_t TIFFSizeReader( thandle_t v )
		{
			IReadBuffer* pBuffer = (IReadBuffer*)v;

			if( pBuffer ) {
				return pBuffer->Size();
			}

			return 0;
		}

		__attribute__((unused))
		static toff_t TIFFSizeWriter( thandle_t v )
		{
			IWriteBuffer* pBuffer = (IWriteBuffer*)v;

			if( pBuffer ) {
				return pBuffer->Size();
			}

			return 0;
		}

		__attribute__((unused))
		static int TIFFMapFile( thandle_t, tdata_t*, toff_t* )
		{
			return 0;
		}

		__attribute__((unused))
		static void TIFFUnmapFile( thandle_t, tdata_t, toff_t )
		{
		}
	}
}

#endif
#endif

