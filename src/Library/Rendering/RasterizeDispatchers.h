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
#include <cstdint>
#include <cstdlib>
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
			// Monotonic high-water mark for the reported numerator.  Worker
			// threads take tile indices via fetch_add but report progress
			// under progressMut in scheduler order, so a low-idx report can
			// arrive after a higher one; we clamp so progress never recedes.
			double lastReportedNum;

			RMutex progressMut;		// Only for progress callback serialization

			// Tile RNG seeds (DL-625 review).  Each worker used to
			// default-construct its RandomNumberGenerator, whose seed is
			// `rand()`: N workers calling libc rand() at once is a data race
			// on its hidden state (undefined behaviour; in practice workers
			// could receive equal or reordered seeds), and even without the
			// race which seed drew which tile depended on thread scheduling,
			// so one std::srand gave different multi-threaded renders.  The
			// libc stream is now read ONCE, here, on the thread that builds
			// the dispatcher, and each worker RESEEDS its generator at the
			// start of every tile from (that value, the tile's corner): the
			// random stream a tile consumes is a pure function of std::srand's
			// state and the tile, whichever worker renders it and in whatever
			// order the (shuffled) sequence hands the tiles out, and no worker touches
			// libc state.  The generator stays per worker (one MT state, the
			// RuntimeContext keeps a reference to it); a reseed is ~600 word
			// operations per tile.  Scope of the claim: a render with the
			// same std::srand and the SAME worker count reproduces; a one-
			// thread render takes PixelBasedRasterizerHelper's SP path
			// (GlobalRNG(), no tiles seeded here) and differs from an
			// N-thread one, and tile size (hence tile corners) depends on
			// the worker count.  Worker-side generators must not read libc
			// either (ThreadLocalSeed), or the stream position after a pass
			// would depend on how many workers ran.
			const unsigned int tileSeedBase;

			unsigned int TileSeed( const Rect& tile ) const
			{
				// Keyed on the tile's PIXEL position, not its place in the
				// sequence: the default block sequence is shuffled from
				// std::random_device, so a sequence index names a different
				// tile every render.
				return DeriveSeed( tileSeedBase,
					( static_cast<unsigned long long>( tile.top ) << 32 ) | static_cast<unsigned long long>( tile.left ) );
			}

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
					// Never let the reported numerator go backward (out-of-order
					// multi-thread reports): a receding bar is jarring and a
					// >resetThreshold backward jump would restart the ETA.
					if( num < lastReportedNum ) {
						num = lastReportedNum;
					} else {
						lastReportedNum = num;
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
			  progressTotal( progressTotal_ ),
			  lastReportedNum( 0 ),
			  tileSeedBase( LockedLibcRand() )
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
				RandomNumberGenerator random( tileSeedBase );
				RuntimeContext rc( random, pass, true );
				rasterizer.PrepareRuntimeContext( rc );

				// This thread will execute until we ask for another block and we're told there are no more
				Rect rect(0,0,0,0);
				while( GetNextBlock( rect ) ) {
					random.Reseed( TileSeed( rect ) );
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
			const TimeIndexedFrame* pTimeFrame;	//!< DL-465: per-thread time-indexed state, or null

		public:

			RasterizeBlockAnimationDispatcher(
				const RuntimeContext::PASS pass_,
				IRasterImage& image_,
				const IScene& scene_,
				IRasterizeSequence& seq_,
				const PixelBasedRasterizerHelper& rasterizer_,
				IProgressCallback* pProgressFunc_,
				const PixelBasedRasterizerHelper::AnimFrameData& animData_,
				const double progressBase_,
				const double progressWeight_,
				const double progressTotal_,
				const TimeIndexedFrame* pTimeFrame_ = 0
				) :
			  RasterizeBlockDispatcher( pass_, image_, scene_, seq_, rasterizer_, pProgressFunc_, progressBase_, progressWeight_, progressTotal_ ),
			  animData( animData_ ),
			  pTimeFrame( pTimeFrame_ )
			{
			}

			void DoAnimWork()
			{
				const unsigned int height = image.GetHeight();

				// DL-465: this worker's clones of the moving elements, posed
				// per sample by AnimateSceneToSampleTime.  A no-op when the
				// frame is not motion-blurred (null frame).
				TimeIndexedThread timeView( pTimeFrame );

				// Create a runtime context for this thread
				RandomNumberGenerator random( tileSeedBase );
				RuntimeContext rc( random, pass, true );
				rasterizer.PrepareRuntimeContext( rc );

				// This thread will execute until we ask for another block and we're told there are no more
				Rect rect(0,0,0,0);
				while( GetNextBlock( rect ) ) {
					random.Reseed( TileSeed( rect ) );
					// Operate on this block
					rasterizer.SPRasterizeSingleBlockOfAnimation( rc, image, scene, rect, height, animData );
				}

				FlushCallingThreadSplatBuffer();
			}
		};
	}
}

#endif
