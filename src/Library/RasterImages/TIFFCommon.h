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
		[[maybe_unused]] static tsize_t TIFFReadDummy( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			return 0;
		}

		[[maybe_unused]] static tsize_t TIFFRead( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			IReadBuffer* pBuffer = (IReadBuffer*)v;

			if( pBuffer && amount >= 0 &&
				static_cast<toff_t>(amount) <= std::numeric_limits<unsigned int>::max() &&
				pBuffer->getBytes( ptr, static_cast<unsigned int>(amount) ) ) {
				return amount;
			}

			return 0;
		}

		static inline toff_t TIFFSeek( IBuffer* buffer, toff_t offset, int whence, IWriteBuffer* writer = nullptr )
		{
			constexpr toff_t failure = static_cast<toff_t>(-1);
			if( !buffer ) return failure;
			const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
			std::uint64_t base = 0;
			switch( whence ) {
			case SEEK_CUR: base = BufferPosition(*buffer); break;
			case SEEK_END: base = BufferSize(*buffer); break;
			case SEEK_SET: if( offset > maximum ) return failure; break;
			default: return failure;
			}
			if( base > maximum ) return failure;
			if( offset > maximum ) {
				// libtiff passes negative relative offsets through unsigned toff_t.
				const auto distance = toff_t(0) - offset;
				if( distance > base ) return failure;
				offset = base - distance;
			} else {
				if( offset > maximum - base ) return failure;
				offset += base;
			}
			const bool wide = dynamic_cast<IBuffer64*>(buffer) != nullptr;
			if( !wide && offset > static_cast<toff_t>(std::numeric_limits<int>::max()) ) return failure;
			if( offset > BufferSize(*buffer) && writer && !wide ) {
				// libtiff may align a directory field beyond the current end.
				// Write the alignment gap so in-memory output can seek there too.
				const auto size = BufferSize(*buffer);
				if( size != BufferPosition(*buffer) && !BufferSeek(*buffer, IBuffer::START, static_cast<std::int64_t>(size)) )
					return failure;
				const unsigned char zeros[16]{};
				for( auto remaining = offset - size; remaining != 0; ) {
					const auto count = static_cast<unsigned int>(remaining > sizeof(zeros) ? sizeof(zeros) : remaining);
					if( !writer->setBytes( zeros, count ) ) return failure;
					remaining -= count;
				}
			}
			if( offset != BufferPosition(*buffer) && !BufferSeek(*buffer, IBuffer::START, static_cast<std::int64_t>(offset)) )
				return failure;
			return BufferPosition(*buffer);
		}

		[[maybe_unused]] static toff_t TIFFSeekReader( thandle_t handle, toff_t offset, int whence )
		{
			return TIFFSeek( static_cast<IReadBuffer*>(handle), offset, whence );
		}

		[[maybe_unused]] static toff_t TIFFSeekWriter( thandle_t handle, toff_t offset, int whence )
		{
			auto* writer = static_cast<IWriteBuffer*>(handle);
			return TIFFSeek( writer, offset, whence, writer );
		}

		[[maybe_unused]] static tsize_t TIFFWrite( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			IWriteBuffer* pBuffer = (IWriteBuffer*)v;

			if( pBuffer && amount >= 0 &&
				static_cast<toff_t>(amount) <= std::numeric_limits<unsigned int>::max() &&
				pBuffer->setBytes( ptr, static_cast<unsigned int>(amount) ) ) {
				return amount;
			}

			return 0;
		}

		[[maybe_unused]] static tsize_t TIFFWriteDummy( thandle_t v, tdata_t ptr, tsize_t amount )
		{
			return 0;
		}

		[[maybe_unused]] static int TIFFClose( thandle_t v )
		{
			return 0;
		}

		[[maybe_unused]] static toff_t TIFFSizeReader( thandle_t v )
		{
			IReadBuffer* pBuffer = (IReadBuffer*)v;

			if( pBuffer ) {
				return BufferSize(*pBuffer);
			}

			return 0;
		}

		[[maybe_unused]] static toff_t TIFFSizeWriter( thandle_t v )
		{
			IWriteBuffer* pBuffer = (IWriteBuffer*)v;

			if( pBuffer ) {
				return BufferSize(*pBuffer);
			}

			return 0;
		}

		[[maybe_unused]] static int TIFFMapFile( thandle_t, tdata_t*, toff_t* )
		{
			return 0;
		}

		[[maybe_unused]] static void TIFFUnmapFile( thandle_t, tdata_t, toff_t )
		{
		}
	}
}

#endif
#endif

