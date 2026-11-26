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

		/// L8 round 8 — Align a rasterizer's adaptive block size to
		/// the FrameStore's tile grid so workers can NEVER compete
		/// for the same FrameStore tile's exclusive lock.
		///
		/// Background: `SPRasterizeSingleBlock` brackets its per-pixel
		/// writes with `mFrameStore->BeginTile(tx,ty)` / `EndTile(tx,ty)`
		/// for every FrameStore tile the block overlaps.  If two
		/// rasterizer blocks (run by different workers) overlap the
		/// same FrameStore tile, both call `BeginTile(tx,ty)` and one
		/// blocks waiting on the other's exclusive lock.  Combined
		/// with the synchronous `OnTileComplete` observer dispatch
		/// (which itself takes a per-VFS `bufferMutex_` then reads the
		/// tile via `shared_lock`), this produces a `bufferMutex_ ↔
		/// tile mutex` lock inversion that hangs the entire render
		/// after a few blocks.
		///
		/// Root cause: rasterizer `ComputeTileSize` returns values in
		/// [8, 64] rounded to multiples of 8, while the FrameStore is
		/// fixed at `tileEdge = 32`.  When the rasterizer picks 8, 16,
		/// or 24, multiple rasterizer blocks fit in one FrameStore
		/// tile and the inversion fires.
		///
		/// Fix: round the rasterizer tile size UP to the nearest
		/// multiple of `frameStoreTileEdge` (so a rasterizer block
		/// strictly contains an integer number of FrameStore tiles,
		/// and adjacent rasterizer blocks never share a FrameStore
		/// tile).  Pass `frameStoreTileEdge = 0` (or call only when
		/// the rasterizer is NOT writing to a FrameStore) to skip the
		/// alignment.
		///
		/// Pre-condition: `rasterizerTileEdge > 0`,
		/// `frameStoreTileEdge >= 0`.
		/// Post-condition: returned value is a multiple of
		/// `frameStoreTileEdge` (or equal to `rasterizerTileEdge` when
		/// `frameStoreTileEdge == 0`).
		inline unsigned int AlignTileSizeToFrameStore(
			unsigned int rasterizerTileEdge,
			unsigned int frameStoreTileEdge )
		{
			if( frameStoreTileEdge == 0 ) return rasterizerTileEdge;
			if( rasterizerTileEdge == 0 ) return frameStoreTileEdge;
			// Round UP to nearest multiple of frameStoreTileEdge.
			const unsigned int aligned =
				( ( rasterizerTileEdge + frameStoreTileEdge - 1 ) /
				  frameStoreTileEdge ) * frameStoreTileEdge;
			return aligned;
		}
	}
}

#endif
