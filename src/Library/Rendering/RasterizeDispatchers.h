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
			IRasterizeSequence& seq;
			const PixelBasedRasterizerHelper& rasterizer;
			IProgressCallback* pProgressFunc;

			unsigned int sofar;
			unsigned int numseq;

			RMutex mut;

			bool GetNextBlock( Rect& rc )
			{
				mut.lock();
				{
					// Return the next available block
					if( sofar < numseq ) {

						if( pProgressFunc && sofar>0 )	{
							if( !pProgressFunc->Progress( static_cast<double>(sofar), static_cast<double>(numseq-1) ) ) {
								mut.unlock();
								return false;		// abort the render
							}
						}

						rc = seq.GetNextRegion();
						sofar++;
						mut.unlock();

						return true;
					}

					mut.unlock();
					return false;
				}
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
			  seq( seq_ ),
			  rasterizer( rasterizer_ ), 
			  pProgressFunc( pProgressFunc_ ),
			  sofar( 0 )
			{
				numseq = seq.NumRegions();
			}

			void DoWork()
			{
				const unsigned int height = image.GetHeight();

				// Create a runtime context for this thread
				RandomNumberGenerator random;
				RuntimeContext rc( random, pass, true );

				// This thread will execute until we ask for another block and we're told there are no more
				Rect rect(0,0,0,0);
				while( GetNextBlock(rect) ) {
					// Operate on this block
					rasterizer.SPRasterizeSingleBlock( rc, image, scene, rect, height );			
				}
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