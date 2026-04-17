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
#include "FilteredFilm.h"
#include "AOVBuffers.h"
#include "../Utilities/RuntimeContext.h"
#include "../Utilities/ProgressiveConfig.h"

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

			/// Renders one pass. Returns false if the progress callback
			/// requested cancellation during the pass.
			bool RasterizeScenePass(
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
			bool				useZSobol;		///< Use Morton-indexed Sobol (blue-noise error distribution)

			mutable FilteredFilm*	pFilteredFilm;		///< Film buffer for wide-support filter reconstruction
			mutable IRasterImage*	pFilteredScratch;	///< Scratch image for progressive display with film
			ProgressiveConfig		progressiveConfig;	///< Multi-pass progressive rendering configuration

			mutable ProgressiveFilm*	mProgressiveFilm;	///< Per-pixel state for progressive multi-pass rendering
			mutable unsigned int		mTotalProgressiveSPP;	///< Total SPP budget across all progressive passes

			// Weighted progress state.  The progressive loop fills
			// these before each RasterizeScenePass call so the
			// dispatcher can report a single 0..1 progress bar
			// across the whole render (weighted by SPP-per-tile for
			// the current pass).  When mProgressTotal is 0,
			// RasterizeScenePass falls back to per-pass 0..1 mode.
			mutable double				mProgressBase;		///< Work units done before this pass
			mutable double				mProgressWeight;	///< Work units per tile in this pass (= passSPP)
			mutable double				mProgressTotal;		///< Total work units across all passes

#ifdef RISE_ENABLE_OIDN
			mutable AOVBuffers*		pAOVBuffers;		///< First-hit albedo + normal buffers for OIDN
#endif

			/// Returns true when the pixel filter's support extends beyond
			/// a single pixel, requiring film-based reconstruction.
			bool UseFilteredFilm() const;

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

			/// Returns a reference to the image that should be sent to
			/// OutputIntermediateImage.  The default just returns the
			/// primary image.  BDPT overrides this to return a scratch
			/// copy with resolved splats composited in, avoiding any
			/// mutation of the primary accumulation buffer.
			virtual IRasterImage& GetIntermediateOutputImage( IRasterImage& primary ) const;

			// Our own functions
			virtual void FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;

			// Dispatch the pre-denoised splatted image to rasterizer outputs.
			// File outputs write to the normal filename; non-file outputs
			// no-op (they will receive the denoised final via
			// FlushDenoisedToOutputs).
			virtual void FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;

			// Dispatch the denoised final image to rasterizer outputs.  File
			// outputs write with a "_denoised" filename suffix; non-file
			// outputs forward to OutputImage so they still observe the
			// denoised final (existing behavior).
			virtual void FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;

			/// Reuses the normal block dispatcher for internal passes such as
			/// path-guiding training, so derived rasterizers can run those
			/// passes multithreaded without duplicating dispatch logic.
			/// Returns false if the progress callback requested cancellation.
			bool RasterizeBlocksForPass(
				const RuntimeContext::PASS pass,
				const IScene& scene,
				IRasterImage& image,
				const Rect* pRect,
				IRasterizeSequence& seq
				) const
			{
				return RasterizeScenePass( pass, scene, image, pRect, seq );
			}

		public:
			PixelBasedRasterizerHelper( IRayCaster* pCaster_ );

			/// Called after a RuntimeContext is created, before any rendering
			/// with it.  Subclasses can override to inject per-context state
			/// (e.g. path guiding field pointers).  Default installs shared
			/// progressive render state.
			virtual void PrepareRuntimeContext( RuntimeContext& rc ) const;

			/// Total sample budget for progressive rendering.  Rasterizers with
			/// adaptive sampling override this to use their adaptive max.
			virtual unsigned int GetProgressiveTotalSPP() const;

		/// Called at the beginning of RasterizeScene, before the main
		/// render pass.  Subclasses can override to perform setup such
		/// as path guiding training.  Default does nothing.
		virtual void PreRenderSetup( const IScene& pScene, const Rect* pRect ) const {}

		/// Called at the beginning of each progressive pass, before
		/// the per-pixel dispatch.  Subclasses can override to
		/// refresh per-iteration state (e.g. VCM rebuilds its
		/// light vertex store).  Default does nothing.
		/// \param pScene  Scene being rendered
		/// \param passIdx Zero-based progressive pass index
		virtual void OnProgressivePassBegin(
			const IScene& pScene,
			const unsigned int passIdx ) const {}

		/// When true, per-block intermediate output is skipped during
		/// the block dispatch.  The end-of-pass flush still runs.
		/// VCM overrides this to true because each pass is a single
		/// SPP — flushing after every 32×32 block is wasted I/O.
		virtual bool SkipPerBlockIntermediateOutput() const { return false; }

		/// Called at the end of RasterizeScene, after the main render
		/// pass and output flush.  Subclasses can override to perform
		/// cleanup.  Default does nothing.
		virtual void PostRenderCleanup() const {}

			// Rasterizer interface implementations
			virtual void AttachToScene( const IScene* ){};		// We don't need to do anything to attach
			virtual void DetachFromScene( const IScene* ){};	// We don't need to do anything to detach

			void SPRasterizeSingleBlock( const RuntimeContext& rc, IRasterImage& image, const IScene& scene, const Rect& rect, const unsigned int height ) const;
			void SPRasterizeSingleBlockOfAnimation( const RuntimeContext& rc, IRasterImage& image, const IScene& scene, const Rect& rect, const unsigned int height, const AnimFrameData& framedata ) const;

			virtual unsigned int PredictTimeToRasterizeScene( const IScene& pScene, const ISampling2D& pSampling, unsigned int* pActualTime ) const;
			virtual void RasterizeScene( const IScene& pScene, const Rect* pRect, IRasterizeSequence* pRasterSequence ) const;
			virtual void RasterizeSceneAnimation( const IScene& pScene, const Scalar time_start, const Scalar time_end, const unsigned int num_frames, const bool do_fields, const bool invert_fields, const Rect* pRect, const unsigned int* specificFrame, IRasterizeSequence* pRasterSequence ) const;

			virtual void SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ );
			void SetProgressiveConfig( const ProgressiveConfig& config );

		};
	}
}

#endif
