//////////////////////////////////////////////////////////////////////
//
//  AdaptiveTileSizer.cpp
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AdaptiveTileSizer.h"
#include <cmath>
#include <algorithm>

namespace RISE
{
	namespace Implementation
	{
		unsigned int ComputeTileSize(
			unsigned int width,
			unsigned int height,
			unsigned int numThreads,
			unsigned int targetTilesPerThread,
			unsigned int minTileSize,
			unsigned int maxTileSize
			)
		{
			if( numThreads < 1 ) numThreads = 1;
			if( targetTilesPerThread < 1 ) targetTilesPerThread = 1;
			if( minTileSize < 1 ) minTileSize = 1;
			if( maxTileSize < minTileSize ) maxTileSize = minTileSize;

			const unsigned long long totalPixels =
				static_cast<unsigned long long>( width ) *
				static_cast<unsigned long long>( height );
			const unsigned long long desiredTiles =
				static_cast<unsigned long long>( numThreads ) *
				static_cast<unsigned long long>( targetTilesPerThread );
			if( desiredTiles == 0 ) {
				return maxTileSize;
			}

			// Pixels per tile → tile edge (square tiles).
			const double pixelsPerTile =
				static_cast<double>( totalPixels ) /
				static_cast<double>( desiredTiles );
			unsigned int tileSize = static_cast<unsigned int>(
				std::round( std::sqrt( pixelsPerTile ) ) );

			// Round to a multiple of 8 (SIMD friendly) before clamping.
			tileSize = ( tileSize / 8 ) * 8;
			if( tileSize == 0 ) tileSize = minTileSize;

			if( tileSize < minTileSize ) tileSize = minTileSize;
			if( tileSize > maxTileSize ) tileSize = maxTileSize;

			return tileSize;
		}
	}
}
