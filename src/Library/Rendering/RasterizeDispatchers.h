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
					if( !pProgressFunc->Progress( static_cast<double>(idx), static_cast<double>(numTiles-1) ) ) {
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
				IProgressCallback* pProgressFunc_
				) :
			  pass( pass_ ),
			  image( image_ ),
			  scene( scene_ ),
			  rasterizer( rasterizer_ ),
			  pProgressFunc( pProgressFunc_ ),
			  nextTile( 0 ),
			  cancelled( false )
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
			  RasterizeBlockDispatcher( pass_, image_, scene_, seq_, rasterizer_, pProgressFunc_ ),
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
			}
		};
	}
}

#endif
