//////////////////////////////////////////////////////////////////////
//
//  AdaptiveTileSizer.h - Compute a tile size that keeps
//    tilesPerThread ≥ target so work-stealing always has slack.
//    Small tiles hurt cache locality; large tiles hurt load balance.
//    This picks the middle ground given image dimensions + thread
//    count.
//
//    Used by every caller of MortonRasterizeSequence(…) plus the
//    VCM light-pass dispatcher so the same heuristic applies
//    end-to-end.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 16, 2026
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_ADAPTIVE_TILE_SIZER_
#define RISE_ADAPTIVE_TILE_SIZER_

namespace RISE
{
	namespace Implementation
	{
		/// Compute a tile edge size (pixels) such that the image
		/// decomposes into at least `targetTilesPerThread × numThreads`
		/// tiles, clamped to [minTileSize, maxTileSize] and rounded to
		/// a multiple of 8 for SIMD friendliness.
		///
		/// \param width                  Image width in pixels
		/// \param height                 Image height in pixels
		/// \param numThreads             Active worker count
		/// \param targetTilesPerThread   Slack factor (default 8)
		/// \param minTileSize            Lower clamp (default 8)
		/// \param maxTileSize            Upper clamp (default 64)
		unsigned int ComputeTileSize(
			unsigned int width,
			unsigned int height,
			unsigned int numThreads,
			unsigned int targetTilesPerThread,
			unsigned int minTileSize,
			unsigned int maxTileSize
			);
	}
}

#endif
