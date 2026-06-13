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

			//! Original "red corner-marker" tile decoration.  Kept as a
			//! back-pocket debug viz; no production call site as of
			//! L8 round 19 (`DimTileBorder` replaced it everywhere
			//! that DrawToggles was previously invoked).  See
			//! `DimTileBorder` for the rationale.
			void DrawToggles(
				IRasterImage& image,
				const Rect& rc_region,
				const RISEColor& toggle_color,
				const double toggle_size
				) const;

			//! Tile-border dim — replaces `DrawToggles` as the per-
			//! block "this is being rendered" visual indicator.
			//!
			//! Multiplies each pixel's RGB by `dim_factor` for pixels
			//! within `border_w` of any edge of `rc_region`.  Alpha is
			//! untouched.  The block's interior is left as-is so the
			//! previous frame's content remains visible there — useful
			//! when the user is watching variance reduction across
			//! progressive passes.
			//!
			//! Why border instead of full-tile dim or corner markers:
			//!   * Corner markers (the old DrawToggles) flash in then
			//!     out unevenly as the per-pixel loop overwrites
			//!     corners at different times; user-reported as
			//!     visually noisy.
			//!   * Full-tile dim loses spatial context — user can't
			//!     see the previous frame's content while waiting for
			//!     this block to re-render.
			//!   * Border-only preserves the interior (variance-
			//!     reduction reference) while still signalling "this
			//!     block is being processed" via a ring around it.
			//!
			//! Cost: O(border_w × perimeter) pixel ops per block,
			//! typically ~1000-2000 ops/block on a 64x64 block with
			//! 5 px border, ~1 ms total over a multi-second render.
			//! Negligible.
			//!
			//! Edge case: if the block is too small to support two
			//! non-overlapping border strips (height or width <=
			//! 2 × border_w, only happens at image edges where the
			//! block is clipped), the whole region is dimmed.  No
			//! double-dim from overlapping strips.
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

			//! Persistent IRasterImage held across RasterizeScene calls.
			//! Allocated lazily in AcquireRenderImage on first use, kept
			//! alive until the rasterizer is destroyed or the camera
			//! dimensions change.  Two consumers benefit:
			//!
			//!   1. Interactive viewport — the cancel-restart loop
			//!      reuses the previous frame's pixels so cancelled
			//!      passes degrade gracefully (no buffer-clear flash
			//!      between motion frames).
			//!
			//!   2. Production "save image" — once a render completes,
			//!      the final pixel data is queryable via
			//!      GetLastRenderedImage() so the host UI can write it
			//!      to disk in any format without re-rendering.
			//!
			//! `mutable` because Acquire is `const` (called from the
			//! const RasterizeScene path).
			mutable IRasterImage*	mPersistentImage;
			mutable unsigned int	mPersistentW;
			mutable unsigned int	mPersistentH;

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

			//! Whether OIDN should be invoked at the end of a render
			//! (or render attempt — see below).  Default just gates
			//! on `bDenoisingEnabled`; the cancel state is INTENTIONALLY
			//! NOT consulted, so cancelling a render mid-flight still
			//! produces a denoised partial image.  Per-pixel coverage
			//! varies (some tiles may be un-rendered or under-sampled),
			//! but the resulting denoised partial is more useful for
			//! interactive workflows than the raw-noisy partial that
			//! would otherwise be flushed.  Interactive subclasses can
			//! add their own policy gates on top — see
			//! `InteractivePelRasterizer::ShouldDenoise` for the
			//! preview-mode toggle.  Documented in docs/OIDN.md
			//! (decision log: 2026-04-29 cancel-still-denoises).
			virtual bool ShouldDenoise() const;

			//! Number of primary-ray AOV samples to collect when the
			//! renderer did not accumulate AOVs during shading.  The
			//! production default keeps DOF/AA coverage; interactive
			//! preview overrides to 1 for latency.
			virtual unsigned int GetDenoiseAOVSamplesPerPixel() const;

			//! L7 — Propagate per-pixel albedo + normal data from the
			//! `AOVBuffers` (built by `CollectFirstHitAOVs` or
			//! per-block `Accumulate*`) into the canonical
			//! `mFrameStore`'s Albedo + Normal channels.  No-op when
			//! `mFrameStore` is null or doesn't have those channels
			//! requested in its Spec, or when dims mismatch the
			//! AOVBuffers.  Bracketed via `FrameStoreBulkBracket` so
			//! direct AOV-channel readers don't see torn writes.
			//!
			//! Called from `RasterizeScene` post-CollectFirstHitAOVs,
			//! before `ApplyDenoise` mutates the beauty channel.
			//! Pre-L7: AOV data was used by OIDN once and discarded;
			//! post-L7 it persists in the canonical FrameStore for
			//! downstream consumers (multichannel EXR, AOV-aware
			//! viewports, future compositing pipelines).
			void PropagateAOVsToFrameStore_( const AOVBuffers& aov ) const;
#endif

			//! L8 round 6 / 9 / 13 — Whether `SPRasterizeSingleBlock`
			//! / `SPRasterizeSingleBlockOfAnimation` should fire the
			//! "split bracket" pair that exposes the toggle-decorated
			//! image to FrameStore observers BEFORE the per-pixel
			//! writes overwrite it.
			//!
			//! **Default TRUE** as of L8 round 13 — restores the
			//! pre-regression "red tile-corner toggles appear
			//! immediately when a render starts" UX.
			//!
			//! Timeline:
			//!   Round 1 (original) — added the split-bracket
			//!     EndTile→BeginTile pair after DrawToggles so
			//!     FrameStore observers see the toggle-decorated
			//!     image briefly before the per-pixel loop overwrites
			//!     the red corners.
			//!   Round 7 — defaulted FALSE to dodge a
			//!     `bufferMutex_ ↔ tile-mutex` lock inversion that
			//!     was caused by the bridge's synchronous per-tile
			//!     observer dispatch holding `bufferMutex_` while
			//!     trying to acquire a tile shared_lock.
			//!   Round 9 — eliminated the inversion structurally:
			//!     the platform bridges no longer wire the per-tile
			//!     observer callback at all (production VFS's
			//!     `tileCb_` is left null; the 30 Hz polling timer
			//!     is the sole driver of progressive UI updates).
			//!     Observer dispatch from EndTile becomes a no-op at
			//!     the bridge layer.
			//!   Round 13 — re-enable by default.  The split-bracket
			//!     `EndTile` bumps `globalGeneration_`, which the
			//!     bridge's poll catches within ~33 ms — the user
			//!     sees the red toggles as soon as the block starts,
			//!     even for heavy scenes whose per-pixel loop takes
			//!     seconds.
			//!
			//! Cost: one extra mutex unlock/lock + generation bump
			//! per tile per block — ~100 ns each on Apple Silicon,
			//! ~1 µs total even for a 64x64 block with 4 FS tiles.
			//! Negligible vs the per-pixel cost of ANY rendering
			//! integrator.
			//!
			//! Subclasses can still override to `false` to opt out
			//! (e.g. for benchmarks where the toggle visualisation
			//! adds measurement noise).
			//!
			//! **Placement note**: this method intentionally lives
			//! OUTSIDE the `RISE_ENABLE_OIDN` guard above — it has
			//! nothing to do with denoising and is called from the
			//! tile bracket regardless of OIDN availability.
			//! Pre-round-9 it lived inside the guard, which broke
			//! the Android build (Android does not define
			//! `RISE_ENABLE_OIDN`).
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
			PixelBasedRasterizerHelper( IRayCaster* pCaster_ , RISE::Implementation::FrameStore* frameStore = nullptr);

			/// \return The ray caster this rasterizer drives (borrowed,
			/// not addref'd).  Used by Job::SetActiveRasterizerRadianceScale
			/// to reach the concrete RayCaster (via a further dynamic_cast)
			/// and push a `> modify rasterizer radiance_scale` override.
			/// Every in-tree pixel-based rasterizer (PT / BDPT / VCM / MLT
			/// and their spectral variants) routes through this base, so
			/// the cast from IRasterizer succeeds for all of them.
			IRayCaster* GetRayCaster() const { return pCaster; }

			/// Called after a RuntimeContext is created, before any rendering
			/// with it.  Subclasses can override to inject per-context state
			/// (e.g. path guiding field pointers).  Default installs shared
			/// progressive render state.
			virtual void PrepareRuntimeContext( RuntimeContext& rc ) const;

			/// Total sample budget for progressive rendering.  Rasterizers with
			/// adaptive sampling override this to use their adaptive max.
			virtual unsigned int GetProgressiveTotalSPP() const;

			/// Whether the adaptive-sample-map heatmap should be emitted in
			/// place of beauty when ProgressiveFilm::Resolve runs.  Default
			/// FALSE; rasterizers that carry an `adaptiveConfig` override to
			/// return `adaptiveConfig.showMap && adaptive-is-active`.
			///
			/// Why this exists: the per-pixel `cret` heatmap write in each
			/// rasterizer's IntegratePixel is silently overwritten in
			/// progressive mode by the ProgressiveFilm resolve step, so the
			/// resolve also needs to know to emit the heatmap.  See the
			/// audit "show_adaptive_map ineffective in progressive mode"
			/// fix landed 2026-05-24.
			virtual bool GetAdaptiveShowMap() const { return false; }

			/// Target sample count used as the denominator for the
			/// adaptive-sample-map heatmap.  Default 0; rasterizers with
			/// adaptive sampling return `adaptiveConfig.maxSamples`.
			virtual unsigned int GetAdaptiveTargetSamples() const { return 0; }

		/// Pointer to the IRasterImage produced by the most recent
		/// RasterizeScene call (or nullptr if no render has run yet).
		/// Refcount is held by this rasterizer; callers must NOT
		/// release.  The buffer is invalidated when:
		///   - the rasterizer is destroyed
		///   - a subsequent RasterizeScene at different dimensions
		///     reallocates it (interactive viewport scale step)
		///   - PrepareImageForNewRender clears it for the next pass
		///     (default impl; interactive subclass skips the clear)
		///
		/// Intended use: production rasterizers expose this so the
		/// host UI can save the final image to disk in any format
		/// after a completed render, without re-running the render.
		/// Thread-safe to read AFTER RasterizeScene returns; not safe
		/// to read concurrently with RasterizeScene.
		const IRasterImage* GetLastRenderedImage() const { return mPersistentImage; }

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

		/// Acquire the IRasterImage that this RasterizeScene call will
		/// fill.  Default impl allocates a fresh zero-initialized image
		/// per call (production behaviour).  Subclasses can override
		/// (e.g. InteractivePelRasterizer) to return a persistent
		/// buffer whose pixel content survives across calls — this
		/// lets the interactive viewport's cancel-restart loop start
		/// each new render on top of the previous frame's pixels
		/// instead of a freshly cleared buffer, eliminating black /
		/// debug-color flashes between cancelled passes.
		///
		/// The returned pointer must have its refcount incremented for
		/// the caller (RasterizeScene pairs this with ReleaseRenderImage).
		virtual IRasterImage* AcquireRenderImage( unsigned int width, unsigned int height ) const;

		/// Release the IRasterImage acquired by AcquireRenderImage.
		/// Default impl calls safe_release.  Subclasses caching the
		/// image (interactive viewport) decrement the caller's
		/// reference but keep the persistent reference alive.
		virtual void ReleaseRenderImage( IRasterImage* pImage ) const;

		/// Per-pass entry hook called once at the top of RasterizeScene
		/// after the image is acquired.  Default: clear the image to a
		/// random pastel and fire OutputIntermediateImage on every
		/// rasterizer output (the random clear is a debug visualization
		/// — un-rendered tiles end up in a distinctive colour that
		/// makes incomplete coverage easy to spot).  Interactive
		/// subclasses override to skip the clear entirely so the
		/// previous frame's pixels remain visible while new tiles
		/// render in place.
		virtual void PrepareImageForNewRender( IRasterImage& img, const Rect* pRect ) const;

		/// Factory for the default tile-dispatch sequence used when
		/// RasterizeScene is invoked with pRasterSequence == nullptr.
		/// Default returns a Morton (Z-curve) sequence — production
		/// rasterizers don't care about tile order because they
		/// always render to completion.  Interactive subclasses
		/// override to return a centre-out sequence so partial
		/// buffers from cancelled passes show useful image content
		/// in the middle of the frame rather than upper-left corner
		/// noise.  Returned object has refcount 1; caller releases.
		virtual IRasterizeSequence* CreateDefaultRasterSequence( unsigned int tileEdge ) const;

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
