//////////////////////////////////////////////////////////////////////
//
//  PixelBasedRasterizerHelper.h - A class that pixel based rasterizers
//    can extent.  This base class implements a few of the common
//    things that we need
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 19, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXEL_BASED_RASTERIZER_HELPER_
#define PIXEL_BASED_RASTERIZER_HELPER_

#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IPixelFilter.h"
#include "Rasterizer.h"
#include "../Utilities/RuntimeContext.h"

namespace RISE
{
	namespace Implementation
	{
		class PixelBasedRasterizerHelper : public virtual Rasterizer
		{
		public:
			enum FIELD
			{
				FIELD_UPPER = 0,
				FIELD_LOWER = 1,
				FIELD_BOTH  = 2
			};

			// For the dispatcher
			struct AnimFrameData
			{
				FIELD field;
				Scalar exposure;
				Scalar scanningRate;
				Scalar pixelRate;
				Scalar base_cur_time;
			};

		private:

			// Used only by the RasterizeAnimation
			void RenderFrameOfAnimation( 
				const IScene& pScene,
				const Rect* pRect,
				const FIELD field,
				IRasterImage& image,
				const Scalar time,
				IRasterizeSequence& seq
				) const;

			void DrawToggles( 
				IRasterImage& image, 
				const Rect& rc_region, 
				const RISEColor& toggle_color, 
				const double toggle_size 
				) const;

			void RasterizeScenePass( 
				const RuntimeContext::PASS pass,
				const IScene& scene, 
				IRasterImage& image, 
				const Rect* pRect, 
				IRasterizeSequence& seq 
				) const;

			void RenderFrameOfAnimationPass( 
				const RuntimeContext::PASS pass,
				const IScene& pScene,
				const Rect* pRect,
				const FIELD field,
				IRasterImage& image,
				const Scalar time,
				IRasterizeSequence& seq,
				const AnimFrameData& framedata
				) const;


		protected:
			IRayCaster*			pCaster;
			ISampling2D*		pSampling;
			IPixelFilter*		pPixelFilter;

			virtual ~PixelBasedRasterizerHelper( );

			//! TakeSingleSample is for taking a single image sample, which is used by the predictor
			virtual bool TakeSingleSample( 
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& ray,
				RISEPel& c
				) const = 0;

			//! IntegratePixel is used when we aren't rendering motion blurred frames, in which case
			//! we just render each of the raster sequence segments in order and display
			virtual void IntegratePixel(
				const RuntimeContext& rc,
				const unsigned int x,
				const unsigned int y, 
				const unsigned int height,
				const IScene& pScene,
				RISEColor& cret,
				const bool temporal_samples = false,
				const Scalar temporal_start = 0,
				const Scalar temporal_exposure = 0
				) const = 0;

			// Helper functions for people who extend this class

			//! Generates rendering bounds from a given RECT (which can be null)
			inline void BoundsFromRect( unsigned int& startx, unsigned int& starty, unsigned int& endx, unsigned int& endy, 
				const Rect* pRect, const unsigned int width, const unsigned int height ) const
			{
				startx = 0;
				starty = 0;
				endx = width-1;
				endy = height-1;

				if( pRect )
				{
					startx = pRect->left;
					starty = pRect->top;
					endx = pRect->right;
					endy = pRect->bottom;

					// Sanity check
					startx = startx < width ? startx : width-2;
					endx = endx < width-1 ? endx : width-1;
					starty = starty < height ? starty : height-2;
					endy = endy < height-1 ? endy : height-1;
				}
			}

			inline void DoAnimationFrameProgress( const Scalar step, const Scalar total, IRasterImage* pImage=0, const Rect* rc=0 ) const
			{
				if( pProgressFunc ) {
					// Also iterate through outputs and get them to intermediate rasterize
					
					if( rc && pImage ) {
						RasterizerOutputListType::const_iterator	r, s;
						for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
							(*r)->OutputIntermediateImage( *pImage, rc );
						}
					}
					
					pProgressFunc->Progress( step, total );
				}
			}

			// Our own functions
			virtual void FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;

		public:
			PixelBasedRasterizerHelper( IRayCaster* pCaster_ );

			// Rasterizer interface implementations
			virtual void AttachToScene( const IScene* ){};		// We don't need to do anything to attach
			virtual void DetachFromScene( const IScene* ){};	// We don't need to do anything to detach

			void SPRasterizeSingleBlock( const RuntimeContext& rc, IRasterImage& image, const IScene& scene, const Rect& rect, const unsigned int height ) const;
			void SPRasterizeSingleBlockOfAnimation( const RuntimeContext& rc, IRasterImage& image, const IScene& scene, const Rect& rect, const unsigned int height, const AnimFrameData& framedata ) const;

			virtual unsigned int PredictTimeToRasterizeScene( const IScene& pScene, const ISampling2D& pSampling, unsigned int* pActualTime ) const;
			virtual void RasterizeScene( const IScene& pScene, const Rect* pRect, IRasterizeSequence* pRasterSequence ) const;
			virtual void RasterizeSceneAnimation( const IScene& pScene, const Scalar time_start, const Scalar time_end, const unsigned int num_frames, const bool do_fields, const bool invert_fields, const Rect* pRect, const unsigned int* specificFrame, IRasterizeSequence* pRasterSequence ) const;

			virtual void SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ );
		};
	}
}

#endif
