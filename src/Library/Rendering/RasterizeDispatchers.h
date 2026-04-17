//////////////////////////////////////////////////////////////////////
//
//  RasterizeDispatchers.h - Rasterize dispatchers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 23, 2006
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RASTERIZE_DISPATCHERS_
#define RASTERIZE_DISPATCHERS_

#include "ThreadLocalSplatBuffer.h"

#include <atomic>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class RasterizeBlockDispatcher
		{
		protected:
			RuntimeContext::PASS pass;
			IRasterImage& image;
			const IScene& scene;
			const PixelBasedRasterizerHelper& rasterizer;
			IProgressCallback* pProgressFunc;

			std::vector<Rect> tiles;
			std::atomic<unsigned int> nextTile;
			std::atomic<bool> cancelled;
			unsigned int numTiles;

			// Progress-weighting parameters so the progress callback
			// reports a single 0..1 fraction across the WHOLE render
			// rather than resetting each progressive pass.
			//   progressBase      — work units completed before this pass
			//   progressWeight    — work units per tile within this pass
			//                       (= passSPP, so a pass with 32 spp/tile
			//                        contributes 32× per tile)
			//   progressTotal     — total work units across all passes
			// When progressTotal is 0, the legacy per-pass behavior
			// is preserved (callers pass 0,0,0).
			double progressBase;
			double progressWeight;
			double progressTotal;

			RMutex progressMut;		// Only for progress callback serialization

			bool GetNextBlock( Rect& rc )
			{
				if( cancelled.load( std::memory_order_relaxed ) ) {
					return false;
				}

				const unsigned int idx = nextTile.fetch_add( 1, std::memory_order_relaxed );
				if( idx >= numTiles ) {
					return false;
				}

				rc = tiles[idx];

				// Progress callback needs serialization since it may update UI
				if( pProgressFunc && idx > 0 ) {
					progressMut.lock();
					if( cancelled.load( std::memory_order_relaxed ) ) {
						progressMut.unlock();
						return false;
					}
					double num, denom;
					if( progressTotal > 0 ) {
						// Weighted mode — single 0..1 bar across all passes.
						num   = progressBase + static_cast<double>(idx) * progressWeight;
						denom = progressTotal;
					} else {
						// Legacy per-pass mode.
						num   = static_cast<double>(idx);
						denom = static_cast<double>(numTiles - 1);
					}
					if( !pProgressFunc->Progress( num, denom ) ) {
						cancelled.store( true, std::memory_order_relaxed );
						progressMut.unlock();
						return false;		// abort the render
					}
					progressMut.unlock();
				}

				return true;
			}

		public:

			RasterizeBlockDispatcher(
				const RuntimeContext::PASS pass_,
				IRasterImage& image_,
				const IScene& scene_,
				IRasterizeSequence& seq_,
				const PixelBasedRasterizerHelper& rasterizer_,
				IProgressCallback* pProgressFunc_,
				const double progressBase_,
				const double progressWeight_,
				const double progressTotal_
				) :
			  pass( pass_ ),
			  image( image_ ),
			  scene( scene_ ),
			  rasterizer( rasterizer_ ),
			  pProgressFunc( pProgressFunc_ ),
			  nextTile( 0 ),
			  cancelled( false ),
			  progressBase( progressBase_ ),
			  progressWeight( progressWeight_ ),
			  progressTotal( progressTotal_ )
			{
				numTiles = seq_.NumRegions();
				tiles.reserve( numTiles );
				for( unsigned int i = 0; i < numTiles; i++ ) {
					tiles.push_back( seq_.GetNextRegion() );
				}
			}

			void DoWork()
			{
				const unsigned int height = image.GetHeight();

				// Create a runtime context for this thread
				RandomNumberGenerator random;
				RuntimeContext rc( random, pass, true );
				rasterizer.PrepareRuntimeContext( rc );

				// This thread will execute until we ask for another block and we're told there are no more
				Rect rect(0,0,0,0);
				while( GetNextBlock(rect) ) {
					// Operate on this block
					rasterizer.SPRasterizeSingleBlock( rc, image, scene, rect, height );
				}

				// Flush any splats this worker collected in its
				// thread-local buffer into the shared SplatFilm
				// before returning.  Without this, splats for the
				// last tile (or beyond the last 64k auto-flush
				// threshold) would be orphaned when the worker exits.
				FlushCallingThreadSplatBuffer();
			}

			bool WasCancelled() const
			{
				return cancelled.load( std::memory_order_relaxed );
			}
		};

		class RasterizeBlockAnimationDispatcher : public RasterizeBlockDispatcher
		{
		protected:
			const PixelBasedRasterizerHelper::AnimFrameData& animData;

		public:

			RasterizeBlockAnimationDispatcher(
				const RuntimeContext::PASS pass_,
				IRasterImage& image_,
				const IScene& scene_,
				IRasterizeSequence& seq_,
				const PixelBasedRasterizerHelper& rasterizer_,
				IProgressCallback* pProgressFunc_,
				const PixelBasedRasterizerHelper::AnimFrameData& animData_
				) :
			  RasterizeBlockDispatcher( pass_, image_, scene_, seq_, rasterizer_, pProgressFunc_, 0, 0, 0 ),
			  animData( animData_ )
			{
			}

			void DoAnimWork()
			{
				const unsigned int height = image.GetHeight();

				// Create a runtime context for this thread
				RandomNumberGenerator random;
				RuntimeContext rc( random, pass, true );
				rasterizer.PrepareRuntimeContext( rc );

				// This thread will execute until we ask for another block and we're told there are no more
				Rect rect(0,0,0,0);
				while( GetNextBlock(rect) ) {
					// Operate on this block
					rasterizer.SPRasterizeSingleBlockOfAnimation( rc, image, scene, rect, height, animData );
				}

				FlushCallingThreadSplatBuffer();
			}
		};
	}
}

#endif
