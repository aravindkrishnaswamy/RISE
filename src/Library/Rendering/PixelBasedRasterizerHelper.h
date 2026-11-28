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
#include "TimeIndexedMotionBlur.h"
#include "../Utilities/Transformable.h"
#include <atomic>
#include <utility>
#include <typeinfo>	// Model-B F2 S3 fix round: ForTest_SamplingKernelName's typeid

namespace RISE
{
	namespace Implementation
	{
		// Internal render cleanup. Explicit Run propagates normal-path errors;
		// unwind cleanup preserves the active callback/observer exception.
		template< class F >
		class RasterizerScopeExit
		{
			F cleanup_;
			bool armed_ = true;

		public:
			explicit RasterizerScopeExit( F cleanup ) :
				cleanup_( std::move(cleanup) )
			{}

			RasterizerScopeExit( const RasterizerScopeExit& ) = delete;
			RasterizerScopeExit& operator=( const RasterizerScopeExit& ) = delete;

			~RasterizerScopeExit() noexcept
			{
				if( armed_ ) {
					try {
						cleanup_();
					} catch( ... ) {
						// Cleanup must preserve the original render failure.
					}
				}
			}

			void Run()
			{
				if( armed_ ) {
					armed_ = false;
					cleanup_();
				}
			}
		};

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

			//! Viewport-private camera override for interactive still-frame
			//! RasterizeScene (docs/gui/CAMERAS_AND_VIEWS.md §5.5).  Non-owning;
			//! ignored by animation and prediction paths; set/cleared on render
			//! thread under cancel-and-park.
			void SetViewportCameraOverride( const ICamera* pCam ) { m_pViewportCameraOverride = pCam; }

		private:
			static bool AppendEvaluatedCameraState(
				FrameStore* store,
				const ICamera& camera,
				unsigned int frame,
				Scalar time,
				const char* field );

			const ICamera* m_pViewportCameraOverride = nullptr;
			// A regional irradiance-cache pre-pass must cover the full film
			// because the cache is scene-wide. It renders into a private image;
			// suppress tile callbacks so that hidden full-frame work is never
			// published over the user's preserved outside-region pixels.
			mutable bool mSuppressIntermediateOutput = false;
			mutable bool mHasActiveOutputRegion = false;
			mutable Rect mActiveOutputRegion = Rect( 0, 0, 0, 0 );
			void ConfigureOutputRegion(
				const Rect* region,
				unsigned int width,
				unsigned int height
				) const;

			// Used only by the RasterizeAnimation
			bool RenderFrameOfAnimation(
				const IScene& pScene,
				const Rect* pRect,
				const FIELD field,
				IRasterImage& image,
				const Scalar time,
				IRasterizeSequence& seq,
				const bool resetAOVs
				) const;

			//! Legacy red corner-marker tile decoration (debug only; replaced by DimTileBorder).
			void DrawToggles(
				IRasterImage& image,
				const Rect& rc_region,
				const RISEColor& toggle_color,
				const double toggle_size
				) const;

			//! Dims RGB by `dim_factor` within `border_w` of `rc_region`'s edges
			//! (alpha and interior untouched; dims whole block if <= 2*border_w
			//! to avoid double-dimming).
			void DimTileBorder(
				IRasterImage& image,
				const Rect& rc_region,
				unsigned int border_w,
				double dim_factor
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

			//! Returns false when the progress callback cancels before all blocks finish.
			bool RenderFrameOfAnimationPass(
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

			// Per-invocation execution state, separate from the authored owning
			// kernel above. Installed only between joined worker phases; workers
			// read it while the scoped owner is alive. Concurrent render calls
			// on one rasterizer and callback re-entry are not supported.
			mutable ISampling2D* mActiveSampling = nullptr;
			mutable bool mHasActiveSampling = false;
			ISampling2D* ActiveSampling() const
			{
				return mHasActiveSampling ? mActiveSampling : pSampling;
			}

			// Borrows the caller-owned kernel. Declare its owner first so this
			// override restores the prior execution state before releasing it.
			class ScopedActiveSampling
			{
				const PixelBasedRasterizerHelper& owner_;
				ISampling2D* previous_;
				bool hadPrevious_;
				bool armed_ = true;

			public:
				ScopedActiveSampling( const PixelBasedRasterizerHelper& owner, ISampling2D* sampling ) :
					owner_( owner ),
					previous_( owner.mActiveSampling ),
					hadPrevious_( owner.mHasActiveSampling )
				{
					owner_.mActiveSampling = sampling;
					owner_.mHasActiveSampling = true;
				}

				ScopedActiveSampling( const ScopedActiveSampling& ) = delete;
				ScopedActiveSampling& operator=( const ScopedActiveSampling& ) = delete;

				~ScopedActiveSampling() noexcept
				{
					Restore();
				}

				void Restore() noexcept
				{
					if( armed_ ) {
						owner_.mActiveSampling = previous_;
						owner_.mHasActiveSampling = hadPrevious_;
						armed_ = false;
					}
				}
			};
			IPixelFilter*		pPixelFilter;
			bool				useZSobol;		///< Use Morton-indexed Sobol (blue-noise error distribution)

			//! Stashes the scene-authored sampling kernel while SetSampleCountOverride(1)
			//! forces pSampling == nullptr (implicit 1 SPP) so restoring >1 preserves
			//! the original kernel type.  Owns a ref; released in destructor.
			ISampling2D*		pSamplingStashedByOverride;

			mutable FilteredFilm*	pFilteredFilm;		///< Film buffer for wide-support filter reconstruction
			mutable IRasterImage*	pFilteredScratch;	///< Scratch image for progressive display with film
			ProgressiveConfig		progressiveConfig;	///< Multi-pass progressive rendering configuration

			//! Persistent IRasterImage kept across RasterizeScene calls until
			//! destruction or dimension change (supports interactive cancel-restart
			//! and GetLastRenderedImage()).
			mutable IRasterImage*	mPersistentImage;
			mutable unsigned int	mPersistentW;
			mutable unsigned int	mPersistentH;

			mutable ProgressiveFilm*	mProgressiveFilm;	///< Per-pixel state for progressive multi-pass rendering
			mutable unsigned int		mTotalProgressiveSPP;	///< Total SPP budget across all progressive passes

			// Weighted multi-pass progress state (0 total = per-pass 0..1 fallback).
			mutable double				mProgressBase;		///< Work units done before this pass
			mutable double				mProgressWeight;	///< Work units per tile in this pass (= passSPP)
			mutable double				mProgressTotal;		///< Total work units across all passes

			//! True during PreRenderSetup for the second field of an interlaced
			//! frame: per-frame accumulators survive; per-field state rebuilds.
			mutable bool				mContinuingInterlacedFrame;

			//! True while progressive passes render at one shutter time per pass
			//! (WantsPerPassShutterTime): per-pass state must rebuild on pass 0 too.
			mutable bool				mPerPassShutterTime = false;
			//! True during PreRenderSetup when a frame may use per-pass shutter time.
			mutable bool				mPerPassShutterTimeCandidate = false;
			mutable bool				mTimeIndexedFallbackLogged = false;

			mutable AOVBuffers*		pAOVBuffers;		///< Planned first-hit AOV sidecar (OIDN and/or FrameStore consumers)
			mutable std::atomic<bool>	mLastRenderCompleted;

			//! Allocate/reset only the float planes required by the current
			//! FrameStore, unioned with OIDN's albedo+normal pair when denoising.
			void PrepareAOVBuffers_( unsigned int width, unsigned int height ) const;

			//! Persist every allocated plane into the canonical FrameStore.
			void PropagateAOVsToFrameStore_(
				const AOVBuffers& aov,
				const Rect* region = 0
				) const;

#ifdef RISE_ENABLE_OIDN
			//! Whether OIDN runs at end of render (docs/OIDN.md).  Intentionally
			//! ignores cancel state so cancelled renders still denoise partials.
			virtual bool ShouldDenoise() const;

			//! Primary-ray AOV sample count when shading did not accumulate AOVs.
			virtual unsigned int GetDenoiseAOVSamplesPerPixel() const;

			virtual double DenoiseWorkPerMegaSample() const { return 0.1; }
		public:
			double EstimateDenoiseWorkPerMegapixel() const;
		protected:

#endif

			//! Whether tile blocks emit a split EndTile/BeginTile pair after tile
			//! decoration so polled FrameStore observers see active tiles promptly.
			//! Kept outside `#ifdef RISE_ENABLE_OIDN` (independent of OIDN/Android).
			virtual bool ShouldFireToggleObserverEvents() const { return true; }

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
				// A zero-sized film has no valid inclusive bounds. Callers normally
				// reject it before rasterization, but keep this helper arithmetic-safe
				// instead of underflowing width-1 / height-1.
				if( width == 0 || height == 0 ) {
					startx = starty = endx = endy = 0;
					return;
				}

				startx = 0;
				starty = 0;
				endx = width-1;
				endy = height-1;

				// Invalid or wholly out-of-film viewport regions are ignored for
				// interactive passes (full-frame fallback by contract). Valid partial
				// overlaps are intersected with the current film. This also makes a
				// preserved region safe after a film resize.
				if( pRect && pRect->left <= pRect->right && pRect->top <= pRect->bottom
					&& pRect->left < width && pRect->top < height )
				{
					startx = pRect->left;
					starty = pRect->top;
					endx = r_min( pRect->right, width-1 );
					endy = r_min( pRect->bottom, height-1 );
				}
			}

			/// Returns the image to hand to OutputIntermediateImage for @a region
			/// without mutating @a primary (composes into a pre-allocated scratch
			/// buffer when a FilteredFilm/SplatFilm is active, else returns @a primary).
			/// Contract: @a region is inclusive (null = full frame, single-threaded);
			/// concurrent calls are allowed only for pairwise-disjoint regions;
			/// scratch is never allocated/resized here; callers must not retain the ref.
			virtual IRasterImage& GetIntermediateOutputImage( IRasterImage& primary, const Rect* region ) const;

			static bool ClampRegionToImage( const Rect* region, unsigned int w, unsigned int h, Rect& out );
			static bool IntersectRegion( const Rect& a, const Rect* b, Rect& out );
			static void CopyRegion( const IRasterImage& src, IRasterImage& dst, const Rect& r );

			/// Clamped output crop for per-pass preview composition (null = full frame).
			const Rect* PreviewCompositionRegion() const {
				return mHasActiveOutputRegion ? &mActiveOutputRegion : 0;
			}

			/// Idempotent cleanup of render-scoped preview buffers (filtered film +
			/// scratch); called via scope guard on the dispatching thread after workers join.
			virtual void ReleaseRenderScopedPreviewResources() const;

			/// False if a subclass composes into its own scratch rather than pFilteredScratch.
			virtual bool UsesFilteredPreviewScratch() const { return true; }

			virtual void FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
			/// Pre-denoised flush: file outputs write normal filename; non-file outputs no-op.
			virtual void FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
			/// Denoised flush: file outputs write "_denoised" suffix; non-file outputs forward to OutputImage.
			virtual void FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;

			/// Reuses block dispatch for internal passes (e.g. guiding training). Returns false on cancel.
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
			PixelBasedRasterizerHelper( IRayCaster* pCaster_ , RISE::Implementation::FrameStore* frameStore = nullptr);

			bool ForTest_HasActiveOutputRegion() const {
				return mHasActiveOutputRegion;
			}
			Rect ForTest_ActiveOutputRegion() const {
				return mActiveOutputRegion;
			}

			/// Borrowed ray caster pointer (not addref'd).
			IRayCaster* GetRayCaster() const { return pCaster; }

			virtual void PrepareRuntimeContext( RuntimeContext& rc ) const;
			virtual unsigned int GetProgressiveTotalSPP() const;

			/// True when ProgressiveFilm::Resolve should emit the adaptive sample-count heatmap.
			virtual bool GetAdaptiveShowMap() const { return false; }
			virtual unsigned int GetAdaptiveTargetSamples() const { return 0; }

		/// Most recent RasterizeScene image (borrowed; safe to read only after RasterizeScene returns).
		const IRasterImage* GetLastRenderedImage() const { return mPersistentImage; }

		virtual void PreRenderSetup( const IScene& pScene, const Rect* pRect ) const {}

		/// Called after PrepareForRendering() and before PreRenderSetup(); return false to refuse unsupported scenes.
		virtual bool ValidateSceneForRasterizer( const IScene& /*pScene*/ ) const { return true; }

		/// Poses the scene (or current TimeIndexedThread's clones) to motion-blur sample time `t`.
		/// Enters Transformable::BeginPreparedInternalMutation so the prepared mutation freeze permits posing.
		static void AnimateSceneToSampleTime( const IScene& pScene, const Scalar t )
		{
			struct InternalMutation {
				InternalMutation() { Transformable::BeginPreparedInternalMutation(); }
				~InternalMutation() { Transformable::EndPreparedInternalMutation(); }
			} internalMutation;
			if( TimeIndexedThread* pThread = TimeIndexedThread::Current() ) {
				pThread->PoseAt( t );
				return;
			}
			pScene.GetAnimator()->EvaluateAtTime( t );
			pScene.GetObjects()->RecomposeAnimatedHierarchy();
		}

		/// Shutter interval [t0, t1] for the frame at `time` (false if camera has no exposure).
		static bool FrameShutterInterval( const ICamera& cam, const Scalar time, const unsigned int width, const unsigned int height, Scalar& t0, Scalar& t1 );

		/// Poses scene to `time` and builds acceleration/light structures (sweeping shutter when `bSweepShutter`).
		void PrepareSceneForFrame( const IScene& pScene, const Scalar time, const bool bHasKeyframedObjects, const unsigned int width, const unsigned int height, const bool bSweepShutter = true ) const;

		virtual void OnProgressivePassBegin(
			const IScene& pScene,
			const unsigned int passIdx ) const {}

		/// True if progressive passes share cross-pixel time-dependent state (e.g. VCM light
		/// vertex store), rendering one shutter stratum per pass instead of per-sample times.
		virtual bool WantsPerPassShutterTime() const { return false; }

		/// True for rasterizers safe to run multi-threaded motion blur via TimeIndexedMotionBlur.h.
		virtual bool SupportsTimeIndexedMotionBlur() const { return false; }

		/// Skip per-block intermediate output (end-of-pass flush still runs; used by 1-SPP/pass VCM).
		virtual bool SkipPerBlockIntermediateOutput() const { return false; }

#ifdef RISE_ENABLE_OIDN
		virtual void OnBeforeDenoise( double /*renderElapsedSeconds*/ ) const {}
#endif

		virtual void PostRenderCleanup() const {}

		/// Acquire/release target image for RasterizeScene (returns addref'd pointer).
		virtual IRasterImage* AcquireRenderImage( unsigned int width, unsigned int height ) const;
		virtual void ReleaseRenderImage( IRasterImage* pImage ) const;

		/// Per-pass entry hook (default: random pastel clear; interactive subclasses skip clear).
		virtual void PrepareImageForNewRender( IRasterImage& img, const Rect* pRect ) const;

		/// Default tile sequence when pRasterSequence == nullptr (default: Morton; caller releases ref).
		virtual IRasterizeSequence* CreateDefaultRasterSequence( unsigned int tileEdge ) const;

			// Rasterizer interface implementations
			virtual void AttachToScene( const IScene* ) override {};
			virtual void DetachFromScene( const IScene* ) override {};

			void SPRasterizeSingleBlock( const RuntimeContext& rc, IRasterImage& image, const IScene& scene, const Rect& rect, const unsigned int height ) const;
			void SPRasterizeSingleBlockOfAnimation( const RuntimeContext& rc, IRasterImage& image, const IScene& scene, const Rect& rect, const unsigned int height, const AnimFrameData& framedata ) const;

			virtual unsigned int PredictTimeToRasterizeScene( const IScene& pScene, const ISampling2D& pSampling, unsigned int* pActualTime ) const override;
			virtual void RasterizeScene( const IScene& pScene, const Rect* pRect, IRasterizeSequence* pRasterSequence ) const override;
			virtual void RasterizeSceneAnimation( const IScene& pScene, const Scalar time_start, const Scalar time_end, const unsigned int num_frames, const bool do_fields, const bool invert_fields, const Rect* pRect, const unsigned int* specificFrame, IRasterizeSequence* pRasterSequence ) const override;
			unsigned int PredictTimeToRasterizeScenePrepared(
				const IScene&, const ISampling2D&, unsigned int*, Scalar,
				IRenderPreparationController& ) const override;
			void RasterizeScenePrepared(
				const IScene&, Scalar, IRenderPreparationController&,
				const Rect*, IRasterizeSequence* ) const override;
			void RasterizeSceneAnimationPrepared(
				const IScene&, Scalar, Scalar, unsigned int, bool, bool,
				IRenderPreparationController&, const Rect*, const unsigned int*,
				IRasterizeSequence* ) const override;
			virtual bool LastRenderCompleted() const override
				{ return mLastRenderCompleted.load(std::memory_order_acquire); }

			virtual void SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ );
			void SetProgressiveConfig( const ProgressiveConfig& config );

			//! Overrides sample count for the pixel-based family while preserving
			//! the scene-authored kernel type across `orig -> 1 -> orig` via
			//! `pSamplingStashedByOverride`:
			//!   - samples < 1: no-op, returns false
			//!   - samples == 1: transfers non-null pSampling to stash and nulls pSampling
			//!   - samples > 1: mutates existing pSampling in place, or restores stash,
			//!     or constructs a MultiJittered kernel if neither exists.
			virtual bool SetSampleCountOverride( int samples ) override;

#ifdef RISE_ENABLE_OIDN
			//! Test-only parked-render oracle (call only when no RasterizeScene is in flight).
			bool ForTest_GetAOVBufferDimensions(
				unsigned int& width, unsigned int& height ) const
			{
				if( !pAOVBuffers ) return false;
				width = pAOVBuffers->GetWidth();
				height = pAOVBuffers->GetHeight();
				return true;
			}
#endif

			//! Live SPP for next RasterizeScene (`pSampling->GetNumSamples()`, else 1; never reads stash).
			virtual int GetSampleCountOverride() const override;

			//! Test-only RTTI name of `pSampling` ("" when null).
			const char* ForTest_SamplingKernelName() const
			{
				return pSampling ? typeid( *pSampling ).name() : "";
			}

		};
	}
}

#endif
