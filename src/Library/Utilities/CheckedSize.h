//////////////////////////////////////////////////////////////////////
// CheckedSize.h - Representability checks for owned pixel containers.
// License: see the attached LICENSE.TXT file.
//////////////////////////////////////////////////////////////////////
#ifndef RISE_CHECKED_SIZE_H
#define RISE_CHECKED_SIZE_H

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		// Validate before multiplication/allocation. An optional index-domain
		// limit can be tighter than the native byte and vector limits.
		template< class T >
		std::size_t CheckedPixelCount(
			std::size_t width,
			std::size_t height,
			std::size_t maxCount = std::numeric_limits<std::size_t>::max() )
		{
			if( !width || !height ) return 0;
			const std::size_t limit = std::min( {
				maxCount, std::numeric_limits<std::size_t>::max() / sizeof(T),
				std::vector<T>().max_size() } );
			if( width > limit / height )
				throw std::length_error( "Pixel dimensions exceed addressable container storage" );
			return width * height;
		}
	}
}
#endif
