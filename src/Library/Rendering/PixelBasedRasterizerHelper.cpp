//////////////////////////////////////////////////////////////////////
//
//  PixelBasedRasterizerHelper.cpp - Implements the basic
//    PixelBasedRasterizerHelper helper class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 19, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PixelBasedRasterizerHelper.h"
#include "../Utilities/RTime.h"
#include "../Utilities/Profiling.h"
#include "../RasterImages/RasterImage.h"
#include "RasterizeDispatchers.h"
#include "ThreadLocalSplatBuffer.h"
#include "../Utilities/ThreadPool.h"
#include "AdaptiveTileSizer.h"
#include "PreviewScheduler.h"
#include <chrono>

#include "ScanlineRasterizeSequence.h"
#include "BlockRasterizeSequence.h"
#include "HilbertRasterizeSequence.h"
#include "MortonRasterizeSequence.h"
#include "ProgressiveFilm.h"
#include "../RISE_API.h"
#include "../Interfaces/IScenePriv.h"

#include "FrameStore.h"  // L6c — needed unconditionally by AcquireRenderImage
#ifdef RISE_ENABLE_OIDN
#include "AOVBuffers.h"
#include "OIDNDenoiser.h"
#endif

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedRasterizerHelper::PixelBasedRasterizerHelper(
	IRayCaster* pCaster_,
	RISE::Implementation::FrameStore* frameStore
	) :
  pCaster( pCaster_ ),
  pSampling( 0 ),
  pPixelFilter( 0 ),
  useZSobol( false ),
  pFilteredFilm( 0 ),
  pFilteredScratch( 0 ),
  mPersistentImage( 0 ),
  mPersistentW( 0 ),
  mPersistentH( 0 ),
  mProgressiveFilm( 0 ),
  mTotalProgressiveSPP( 0 ),
  mProgressBase( 0 ),
  mProgressWeight( 0 ),
  mProgressTotal( 0 )
#ifdef RISE_ENABLE_OIDN
  ,pAOVBuffers( 0 )
#endif
{
	if( pCaster ) {
		pCaster->addref();
	} else {
		GlobalLog()->PrintSourceError( "PixelBasedRasterizerHelper:: No RayCaster specified", __FILE__, __LINE__ );
	}
}

PixelBasedRasterizerHelper::~PixelBasedRasterizerHelper( )
{
	safe_release( pSampling );
	safe_release( pPixelFilter );
	safe_release( pCaster );
	safe_release( pFilteredFilm );
	safe_release( pFilteredScratch );
	safe_release( mPersistentImage );
#ifdef RISE_ENABLE_OIDN
	delete pAOVBuffers;
#endif
}

void PixelBasedRasterizerHelper::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	rc.pProgressiveFilm = mProgressiveFilm;
	rc.totalProgressiveSPP = mTotalProgressiveSPP;
#ifdef RISE_ENABLE_OIDN
	// Forward the rasterizer's prefilter mode into the per-thread
	// RuntimeContext so integrators that inline-accumulate AOVs know
	// whether to record at first hit (Fast) or first non-delta scatter
	// (Accurate).  Field is harmless on integrators that don't read it.
	rc.aovPrefilterMode = mDenoisingPrefilter;
#endif
}

unsigned int PixelBasedRasterizerHelper::GetProgressiveTotalSPP() const
{
	return pSampling ? pSampling->GetNumSamples() : 0;
}

#ifdef RISE_ENABLE_OIDN
bool PixelBasedRasterizerHelper::ShouldDenoise() const
{
	// Cancellation state is intentionally NOT consulted here — see the
	// header doc for the rationale (cancelled renders still get
	// denoised, producing a "useful partial" rather than the raw
	// noisy partial).  The only base-class gate is the user's
	// `oidn_denoise` toggle.
	return bDenoisingEnabled;
}

unsigned int PixelBasedRasterizerHelper::GetDenoiseAOVSamplesPerPixel() const
{
	return 4;
}
#endif

bool PixelBasedRasterizerHelper::UseFilteredFilm() const
{
	if( !pPixelFilter ) return false;
	Scalar hw, hh;
	pPixelFilter->GetFilterSupport( hw, hh );
	return hw > 0.501 || hh > 0.501;
}

IRasterImage& PixelBasedRasterizerHelper::GetIntermediateOutputImage( IRasterImage& primary ) const
{
	if( !pFilteredFilm || !pFilteredScratch ) {
		return primary;
	}

	const unsigned int w = primary.GetWidth();
	const unsigned int h = primary.GetHeight();

	// Copy the current primary image into the scratch buffer
	for( unsigned int y=0; y<h; y++ ) {
		for( unsigned int x=0; x<w; x++ ) {
			pFilteredScratch->SetPEL( x, y, primary.GetPEL( x, y ) );
		}
	}

	// Resolve the film into the scratch copy
	pFilteredFilm->Resolve( *pFilteredScratch );

	return *pFilteredScratch;
}

unsigned int PixelBasedRasterizerHelper::PredictTimeToRasterizeScene( const IScene& pScene, const ISampling2D& pSampling, unsigned int* pActualTime ) const
{
	// Snapshot the active camera once at function entry — keeps
	// all inner uses consistent and matches the per-pass contract
	// (structural camera changes must serialize against rendering;
	// see IScenePriv.h).
	const ICamera* pCam = pScene.GetCamera();
	if( !pCam ) {
		GlobalLog()->PrintSourceError( "PixelBasedRasterizerHelper::PredictTimeToRasterizeScene:: Scene contains no camera!", __FILE__, __LINE__ );
		return 0xFFFFFFFF;
	}

	if( pScene.GetIrradianceCache() ) {
		GlobalLog()->PrintEasyError( "PixelBasedRasterizerHelper::PredictTimeToRasterizeScene:: Irradiance caching is turned on, I can't accurate predict rendering time for this scene." );
		return 0xFFFFFFFF;
	}

	// The prediction is based on how long it takes to rasterize the set of random
	// rays given by the sampling kernel pSampling
	Timer	t;

	// Create a runtime context
	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_IRRADIANCE_CACHE, false );

	// Acquire scene dimensions from the active Film.
	const IFilm* pFilm = pScene.GetFilm();
	const unsigned int width = pFilm->GetWidth();
	const unsigned int height = pFilm->GetHeight();

	pCaster->AttachScene( &pScene );
	pScene.GetObjects()->PrepareForRendering();

	ISampling2D::SamplesList2D samples;
	pSampling.GenerateSamplePoints(rc.random, samples);

	ISampling2D::SamplesList2D::const_iterator		m, n;

	t.start();

	for( m=samples.begin(), n=samples.end(); m!=n; m++ ) {
		RISEPel			c;
		Point2			ptOnScreen( width*(*m).x, height*(*m).y );

		Ray ray;
		if( pCam->GenerateRay( rc, ray, ptOnScreen ) ) {
			RasterizerState rast;
			rast.x = (unsigned int)ptOnScreen.x;
			rast.y = (unsigned int)ptOnScreen.y;
			TakeSingleSample( rc, rast, ray, c );
		}
	}

	t.stop();

	Scalar		num_subsamples = 1;

	if( this->pSampling ) {
		num_subsamples = Scalar(this->pSampling->GetNumSamples());
	}

	if( pActualTime ) {
		*pActualTime = t.getInterval();
	}

	int threads = HowManyThreadsToSpawn();
	return (unsigned int)((Scalar(t.getInterval())/Scalar(samples.size())) * width*height*num_subsamples / threads);
}

void PixelBasedRasterizerHelper::DrawToggles( IRasterImage& image, const Rect& rc_region, const RISEColor& toggle_color, const double toggle_size ) const
{
	// The toggle size is in % of overall width/height
	const unsigned int toggle_width = static_cast<unsigned int>(double(rc_region.right-rc_region.left)*toggle_size);
	const unsigned int toggle_height = static_cast<unsigned int>(double(rc_region.bottom-rc_region.top)*toggle_size);

	// Do 8 clear ops to draw the toggle

	// Top Left
	Rect rcCurrentToggle(rc_region.top, rc_region.left, rc_region.top+toggle_height/4, rc_region.left+toggle_width);

	image.Clear( toggle_color, &rcCurrentToggle );

	rcCurrentToggle.right = rcCurrentToggle.left+toggle_width/4;
	rcCurrentToggle.bottom = rc_region.top+toggle_height;

	image.Clear( toggle_color, &rcCurrentToggle );

	// Top right
	rcCurrentToggle.left = rc_region.right-toggle_width;
	rcCurrentToggle.right = rc_region.right;
	rcCurrentToggle.top = rc_region.top;
	rcCurrentToggle.bottom = rc_region.top+toggle_height/4;

	image.Clear( toggle_color, &rcCurrentToggle );

	rcCurrentToggle.left = rc_region.right-toggle_width/4;
	rcCurrentToggle.bottom = rc_region.top+toggle_height;

	image.Clear( toggle_color, &rcCurrentToggle );

	// Bottom left
	rcCurrentToggle.left = rc_region.left;
	rcCurrentToggle.right = rcCurrentToggle.left+toggle_width/4;
	rcCurrentToggle.top = rc_region.bottom - toggle_height;
	rcCurrentToggle.bottom = rc_region.bottom;

	image.Clear( toggle_color, &rcCurrentToggle );

	rcCurrentToggle.top = rc_region.bottom - toggle_height/4;
	rcCurrentToggle.right = rcCurrentToggle.left+toggle_width;

	image.Clear( toggle_color, &rcCurrentToggle );

	// Bottom right
	rcCurrentToggle.left = rc_region.right-toggle_width;
	rcCurrentToggle.right = rc_region.right;
	rcCurrentToggle.top = rc_region.bottom-toggle_height/4;
	rcCurrentToggle.bottom = rc_region.bottom;

	image.Clear( toggle_color, &rcCurrentToggle );

	rcCurrentToggle.left = rc_region.right-toggle_width/4;
	rcCurrentToggle.top = rc_region.bottom-toggle_height;

	image.Clear( toggle_color, &rcCurrentToggle );
}

// L8 round 19 — tile-border dim.  See header doc for the design
// rationale.  Replaces `DrawToggles` at both call sites in
// `SPRasterizeSingleBlock` / `SPRasterizeSingleBlockOfAnimation`.
//
// Implementation: four non-overlapping rectangular strips covering
// the border ring (top, bottom, left, right — left/right exclude
// the corners which the top/bottom strips already covered).
// Inside each strip we read the pixel, multiply RGB by dim_factor,
// write it back.  Alpha is preserved.
//
// Hot-path note: we use `IRasterImage::GetPEL` / `SetPEL` rather
// than a contiguous-buffer memcpy because the underlying pixel
// storage differs across rasterizer implementations (RGB beauty,
// spectral wavelength bins, FilteredFilm, FrameStore view).  The
// per-pixel virtual dispatch is irrelevant on this code path —
// total work is ~1000 ops/block per render, which is well below
// noise even for a multi-thousand-block render.
void PixelBasedRasterizerHelper::DimTileBorder(
	IRasterImage& image,
	const Rect& rc_region,
	unsigned int border_w,
	double dim_factor ) const
{
	const unsigned int t = rc_region.top;
	const unsigned int b = rc_region.bottom;
	const unsigned int l = rc_region.left;
	const unsigned int r = rc_region.right;
	if( b <= t || r <= l ) return;

	const unsigned int H = b - t;
	const unsigned int W = r - l;

	// Per-pixel dim helper — captures `image` + `dim_factor` and
	// applies the multiply.  Tight inner loop; compiler will inline.
	auto dim_one = [&]( unsigned int x, unsigned int y ) {
		RISEColor c = image.GetPEL( x, y );
		c.base.r *= dim_factor;
		c.base.g *= dim_factor;
		c.base.b *= dim_factor;
		image.SetPEL( x, y, c );
	};

	// Edge case: block too small for two non-overlapping border
	// strips.  Happens at image edges where a block is clipped.
	// Dim the whole clipped region — no risk of double-dimming
	// from overlapping strips.
	if( H <= 2 * border_w || W <= 2 * border_w ) {
		for( unsigned int y = t; y < b; ++y ) {
			for( unsigned int x = l; x < r; ++x ) {
				dim_one( x, y );
			}
		}
		return;
	}

	// Top strip: full width, top `border_w` rows.
	for( unsigned int y = t; y < t + border_w; ++y ) {
		for( unsigned int x = l; x < r; ++x ) {
			dim_one( x, y );
		}
	}
	// Bottom strip: full width, bottom `border_w` rows.
	for( unsigned int y = b - border_w; y < b; ++y ) {
		for( unsigned int x = l; x < r; ++x ) {
			dim_one( x, y );
		}
	}
	// Left strip: excluding top/bottom strips already done.
	for( unsigned int y = t + border_w; y < b - border_w; ++y ) {
		for( unsigned int x = l; x < l + border_w; ++x ) {
			dim_one( x, y );
		}
	}
	// Right strip: excluding top/bottom strips already done.
	for( unsigned int y = t + border_w; y < b - border_w; ++y ) {
		for( unsigned int x = r - border_w; x < r; ++x ) {
			dim_one( x, y );
		}
	}
}

void* RasterizeBlock_ThreadProc( void* ptr )
{
	RasterizeBlockDispatcher* pDispatcher = (RasterizeBlockDispatcher*)ptr;
	pDispatcher->DoWork();

	return 0;
}

void* RasterizeBlockAnimation_ThreadProc( void* ptr )
{
	RasterizeBlockAnimationDispatcher* pDispatcher = (RasterizeBlockAnimationDispatcher*)ptr;
	pDispatcher->DoAnimWork();

	return 0;
}

void PixelBasedRasterizerHelper::SPRasterizeSingleBlock( const RuntimeContext& rc, IRasterImage& image, const IScene& scene, const Rect& rect, const unsigned int height ) const
{
	// Progressive tile-level early-out: skip if no pixel in the tile needs more samples.
	if( rc.pProgressiveFilm && rc.pProgressiveFilm->IsTileDone( rect, rc.totalProgressiveSPP ) ) {
		return;
	}

	const bool skipBlockOutput = SkipPerBlockIntermediateOutput();

	// L6e-1.1 — DrawToggles + the pre-block OutputIntermediateImage
	// dispatch BOTH moved inside the fsBracket window (below).  Why:
	//   * DrawToggles writes pixels into `image`; if `image` is the
	//     FrameStore beauty view, those writes need per-tile lock
	//     protection so concurrent direct-FrameStore readers don't
	//     observe a torn toggle-decoration mid-flight.
	//   * The OutputIntermediateImage call follows DrawToggles in
	//     the original code so that legacy IRasterizerOutput
	//     consumers see `image` WITH the toggle decoration drawn —
	//     this is the in-progress visual feedback.  Splitting the
	//     two would silently regress that visualisation (observers
	//     would see image without toggles).
	// Both happen inside the bracket below.

	// L6e-1 — When the rasterizer's per-pixel writes target the
	// canonical FrameStore (image IS mFrameStore->AsBeautyRasterImage()
	// per L6c-1's AcquireRenderImage), bracket the writes with
	// BeginTile / EndTile so concurrent readers (UI viewports,
	// encoders mid-render) see post-bracket state instead of torn
	// per-pixel writes.  The bracketing also bumps each tile's
	// generation counter and fires the FrameStore's observer chain
	// — which L6e-2 will use to drive direct-from-rasterizer-FrameStore
	// repaints (replacing the bridge VFS-internal FrameStore + the
	// FrameSink cross-store copy).
	//
	// Multi-tile coverage: the rasterizer's adaptive block size (8..64,
	// see AdaptiveTileSizer.cpp) may differ from the FrameStore's
	// fixed 32-pixel tile edge.  Cases:
	//   * block ≥ 32: one block covers 1 or more FrameStore tiles.
	//     We BeginTile / EndTile each tile that overlaps the block's
	//     pixel range.
	//   * block < 32: multiple blocks share a FrameStore tile; the
	//     per-tile exclusive lock serializes them.  Acceptable
	//     correctness + light perf cost for small dispatch sizes.
	//
	// The pixel-write loop below is intentionally unchanged.  Reordering
	// it (e.g. iterate tile-by-tile within the block) would change the
	// per-pixel RNG sample stream → byte-identity break against the
	// legacy `RISERasterImage` path.  Holding all overlapping tile
	// locks across the original row-major loop preserves the order.
	// L6e-1.1 — identity gate (was dim-only).  A private buffer
	// (e.g. BDPT path-guiding training image) sized at camera dims
	// would otherwise spuriously fire BeginTile/EndTile on every
	// FrameStore tile during the training pass, bumping generation
	// counters and firing OnTileComplete observers for tiles that
	// were never actually modified in `mFrameStore`.
	const bool fsBracket =
		( mFrameStore &&
		  &image == &mFrameStore->AsBeautyRasterImage() );
	size_t fsTx0 = 0, fsTy0 = 0, fsTx1 = 0, fsTy1 = 0;
	if( fsBracket ) {
		const size_t te = mFrameStore->TileEdge();
		fsTx0 = static_cast<size_t>( rect.left )   / te;
		fsTy0 = static_cast<size_t>( rect.top )    / te;
		fsTx1 = std::min( mFrameStore->TileCountX(),
		                  ( static_cast<size_t>( rect.right )  / te ) + 1 );
		fsTy1 = std::min( mFrameStore->TileCountY(),
		                  ( static_cast<size_t>( rect.bottom ) / te ) + 1 );
		for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
			for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
				mFrameStore->BeginTile( tx, ty );
			}
		}
	}

	// L6e-1.1 — Draw toggles + dispatch in-progress observer fire
	// INSIDE the bracket so toggle writes are lock-protected, and
	// observers still see the same toggle-decorated image they did
	// pre-fix.  Legacy IRasterizerOutput consumers run synchronously
	// on this thread; they don't recurse back into the FrameStore
	// for THIS rasterizer's mFrameStore, so holding all overlapping
	// tile locks across this dispatch is safe.
	//
	// CONTRACT (load-bearing for L6e-2 onwards): IRasterizerOutput
	// implementations MUST NOT call back into `mFrameStore`
	// (BeginTile/EndTile, Render, AsBeautyRasterImage()->GetPEL
	// without external sync) from inside `OutputIntermediateImage`.
	// We currently hold an exclusive lock on every overlapping tile;
	// re-entry would self-deadlock on the non-recursive
	// std::shared_mutex.  Audit of in-tree IRasterizerOutput impls
	// (FrameSink, FileRasterizerOutput, Win32WindowRasterizerOutput,
	// CallbackRasterizerOutputDispatch, ViewportFrameStore) confirms
	// none re-enter `mFrameStore` today.  L6e-2 direct-FrameStore
	// observers will use the IRenderObserver chain, NOT
	// IRasterizerOutput, and are a different code path.
	if( !skipBlockOutput ) {
		// L8 round 19 — dim a 5-pixel border around the block at
		// 40% brightness to signal "this block is being rendered".
		// Replaces the previous red corner-markers (`DrawToggles`):
		//   * Border dim preserves the BLOCK INTERIOR so the user
		//     can see previous-frame content fade away as variance
		//     reduces across progressive passes.
		//   * The 5-pixel ring is wide enough to be visible at
		//     typical viewport zoom levels without occluding much
		//     of the interior; the 0.4 dim factor gives strong
		//     visual contrast against full-brightness rendered
		//     pixels without crushing the previous-frame content
		//     to black.
		//   * Per-block cost: O(border_w × perimeter) ≈ ~1000
		//     pixel ops on a 64x64 block, well below noise on a
		//     multi-second render.
		DimTileBorder( image, rect, 5, 0.4 );

		RasterizerOutputListType::const_iterator	r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( image, &rect );
		}
	}

	// L6e-3 follow-up — toggle visibility restoration.  Pre-L6e-2
	// the bridge received per-block intermediate updates via
	// `VFS::OutputIntermediateImage` (legacy IRasterizerOutput
	// chain) which copied the toggle-marked image to its internal
	// FrameStore + fired `OnTileComplete` on observers.  Bound-mode
	// VFS short-circuits OutputIntermediateImage entirely — the
	// only `OnTileComplete` events come from `BeginTile/EndTile` on
	// the canonical store, and the single `EndTile` at the end of
	// this block fires AFTER the per-pixel loop has overwritten
	// the toggles, so the user never sees them.
	//
	// Restoration: split the bracket.  Fire an EndTile pass here
	// (toggle-marked content visible to direct-FrameStore readers
	// like the bound VFS), then re-acquire the locks before the
	// per-pixel loop.  Net: 2 OnTileComplete events per block per
	// tile (was 1 post-L6e-1.1, was 2 pre-L6e-2 via the legacy
	// path — back to parity).  Bridges' tile-complete callbacks
	// see toggles briefly between events, then the final pixel
	// content.
	//
	// `skipBlockOutput`-true case (VCM): no toggles drawn → no
	// in-progress event needed → keep the single EndTile at the
	// end of the block.
	//
	// L8 round 6 — gated on `ShouldFireToggleObserverEvents()` so
	// `InteractivePelRasterizer` (~30Hz preview) can opt out: the
	// extra OnTileComplete fires drove a measurable per-frame
	// perf cost during fast manipulation (each fire goes through
	// the bridge's bufferMutex → vfs->RenderToBuffer → block
	// dispatch).  Interactive renders refresh the whole frame at
	// frame-complete cadence; per-tile toggles are noise rather
	// than useful progress.
	if( fsBracket && !skipBlockOutput && ShouldFireToggleObserverEvents() ) {
		for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
			for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
				mFrameStore->EndTile( tx, ty );
			}
		}
		for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
			for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
				mFrameStore->BeginTile( tx, ty );
			}
		}
	}

	// L8 round 11 — time-based intra-block tile-bracket flush.
	//
	// Without this, a single rasterizer block on a heavy scene (deep
	// PT, complex BSDFs) can take many seconds.  During that time
	// the worker holds exclusive locks on all the block's FrameStore
	// tiles + the per-pixel loop never bumps `globalGeneration_`.
	// The platform bridge's 30 Hz polling timer (round 9) sees no
	// generation change and emits nothing — net visible effect: the
	// app looks hung between the start and end of a block, even
	// though workers are making real progress.
	//
	// Fix: every `kFlushInterval` of wall-clock time spent in the
	// per-pixel loop, EndTile + BeginTile every tile in the worker's
	// block.  EndTile releases the per-tile mutex, bumps the global
	// generation, fires observers (no-op for bound-mode VFS post
	// round-9), and BeginTile re-acquires the mutex.  The poll now
	// catches the generation advance and emits a partial-block
	// snapshot of the FrameStore.  100 ms gives ~10 visible updates
	// per second on heavy scenes; on fast scenes (block < 100 ms)
	// the flush is skipped and overhead is exactly zero.
	//
	// Order: each flush walks the same (tx, ty) order as the end-of-
	// block EndTile loop below, so per-tile observer fires are
	// consistent in cadence.  Total observer fires per block =
	// (1 + ceil(blockTime / kFlushInterval)) × tilesInBlock, vs the
	// previous 1 × tilesInBlock — for fast blocks this is one fire
	// per tile (unchanged); for slow blocks it grows linearly with
	// block time.
	//
	// Why not row-based: a row-based flush (e.g., every 8 rows) at
	// 8 µs/pixel = 2 ms / row gives a 16 ms flush cadence — too
	// frequent for fast scenes, and the per-flush mutex overhead
	// would dominate.  Time-based scales naturally with scene cost.
	using FlushClock = std::chrono::steady_clock;
	const auto kFlushInterval = std::chrono::milliseconds( 100 );
	auto lastFlush = FlushClock::now();
	bool earlyAbort = false;

	for( unsigned int y=rect.top; y<=rect.bottom; y++ )
	{
		for( unsigned int x=rect.left; x<=rect.right; x++ )
		{
			RISEColor	c;
			IntegratePixel( rc, x, y, height, scene, c );
			ColorMath::EnsurePositve(c.base);
			if( c.a < 0.0 ) c.a = 0.0;
			if( c.a > 1.0 ) c.a = 1.0;
			image.SetPEL( x, y, c );
		}

		// Outer-loop time check so the cost is one steady_clock::now()
		// per row, not per pixel.  Skip on the final row — the
		// end-of-block EndTile loop runs immediately after.
		if( fsBracket && !skipBlockOutput && y < rect.bottom ) {
			const auto now = FlushClock::now();
			if( now - lastFlush >= kFlushInterval ) {
				for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
					for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
						mFrameStore->EndTile( tx, ty );
					}
				}
				for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
					for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
						mFrameStore->BeginTile( tx, ty );
					}
				}
				lastFlush = now;

				// L8 round 12+15 — intra-block cancellation check.
				// Heavy scenes with multi-second per-pixel blocks
				// otherwise can't see a pointer-move cancellation
				// until the dispatcher's per-block Progress check
				// fires at end-of-block — way too late for an
				// interactive UX (the preview-scale ramp can't react,
				// so it never bumps the scale up to "drop down to
				// lower resolution").
				//
				// Round 15 — query `IsCancelled()` directly instead
				// of calling `Progress(prog, tot)`.  The latter
				// doubles as a progress publisher; passing
				// `mProgressBase` (the dispatcher's per-pass starting
				// value) re-reports a STALE reading that is LOWER
				// than the dispatcher's most recent
				// `Progress(mProgressBase + idx*weight, ...)` per-block
				// publish.  The Swift / Qt / Kotlin progress-bar
				// observer just sees raw `progress/total`, so the bar
				// visibly bounced backward every 100 ms of block work
				// — user-reported.  `IsCancelled()` is a pure query
				// with no publish side effect.
				if( pProgressFunc && pProgressFunc->IsCancelled() ) {
					earlyAbort = true;
					break;
				}
			}
		}
	}

	(void)earlyAbort;  // The end-of-block EndTile loop below still runs
	                   // regardless — releases the tile mutexes that
	                   // were re-acquired by the last flush's BeginTile
	                   // loop.  The outer `RasterizeBlockDispatcher`
	                   // sees the cancellation through its own
	                   // per-block Progress check on the next
	                   // GetNextBlock call.

	if( fsBracket ) {
		// EndTile fires per-tile observers (OnTileComplete) and bumps
		// the global generation.  Order doesn't matter — each tile's
		// observer fire is independent.
		for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
			for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
				mFrameStore->EndTile( tx, ty );
			}
		}
	}

	if( !skipBlockOutput ) {
		// Get the image for intermediate output.  BDPT returns a scratch
		// copy with resolved splats; other rasterizers return the primary.
		IRasterImage& outputImage = GetIntermediateOutputImage( image );

		// Also iterate through outputs and get them to intermediate rasterize
		RasterizerOutputListType::const_iterator	r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( outputImage, &rect );
		}
	}
}

void PixelBasedRasterizerHelper::SPRasterizeSingleBlockOfAnimation(
	const RuntimeContext& rc,
	IRasterImage& image,
	const IScene& scene,
	const Rect& rect,
	const unsigned int height,
	const AnimFrameData& framedata
	) const
{
	// Progressive tile-level early-out: skip if no pixel in the tile
	// needs more samples.  Mirrors SPRasterizeSingleBlock so VCM's
	// per-iteration progressive film properly short-circuits converged
	// tiles during animation.
	if( rc.pProgressiveFilm && rc.pProgressiveFilm->IsTileDone( rect, rc.totalProgressiveSPP ) ) {
		return;
	}

	// Mirror SPRasterizeSingleBlock: when the rasterizer opts out of
	// per-block intermediate output (VCM does, because each pass is 1
	// SPP and flushing after every 32×32 block both wastes I/O and
	// paints visible "rasterizer blocks" over the whole-image preview
	// that the per-pass Resolve writes).
	const bool skipBlockOutput = SkipPerBlockIntermediateOutput();

	// L6e-1.1 — DrawToggles + the in-progress observer dispatch
	// moved INSIDE the fsBracket window (below); see comment in
	// SPRasterizeSingleBlock for rationale.

	// L6e-1 — same FrameStore tile bracketing as SPRasterizeSingleBlock
	// (see comment block in that method).  Iteration order preserved.
	// L6e-1.1 — identity gate (was dim-only); see comment in
	// SPRasterizeSingleBlock.
	const bool fsBracket =
		( mFrameStore &&
		  &image == &mFrameStore->AsBeautyRasterImage() );
	size_t fsTx0 = 0, fsTy0 = 0, fsTx1 = 0, fsTy1 = 0;
	if( fsBracket ) {
		const size_t te = mFrameStore->TileEdge();
		fsTx0 = static_cast<size_t>( rect.left )   / te;
		fsTy0 = static_cast<size_t>( rect.top )    / te;
		fsTx1 = std::min( mFrameStore->TileCountX(),
		                  ( static_cast<size_t>( rect.right )  / te ) + 1 );
		fsTy1 = std::min( mFrameStore->TileCountY(),
		                  ( static_cast<size_t>( rect.bottom ) / te ) + 1 );
		for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
			for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
				mFrameStore->BeginTile( tx, ty );
			}
		}
	}

	// L6e-1.1 — block-border dim + in-progress observer dispatch
	// inside the bracket; see SPRasterizeSingleBlock for rationale.
	// L8 round 19 — `DrawToggles` replaced with `DimTileBorder`;
	// preserves the variable name `drewToggles` for downstream gate
	// reuse but the actual visual is now a 5-pixel border dim.
	const bool drewToggles =
		( !skipBlockOutput && framedata.field == FIELD_BOTH );
	if( drewToggles ) {
		DimTileBorder( image, rect, 5, 0.4 );

		RasterizerOutputListType::const_iterator	r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( image, &rect );
		}
	}

	// L6e-3 follow-up — split bracket so direct-FrameStore observers
	// (post-L6e-2 bound VFS) see the border-dimmed content briefly
	// before per-pixel writes overwrite it.  See SPRasterizeSingleBlock
	// for the rationale; this is the animation-path twin.
	// L8 round 6 — same per-rasterizer gate as the static path.
	if( fsBracket && drewToggles && ShouldFireToggleObserverEvents() ) {
		for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
			for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
				mFrameStore->EndTile( tx, ty );
			}
		}
		for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
			for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
				mFrameStore->BeginTile( tx, ty );
			}
		}
	}

	// L8 round 11 — time-based intra-block flush; see
	// SPRasterizeSingleBlock for the rationale.  Same shape for the
	// animation path so heavy animated scenes get the same partial-
	// block visibility.
	using FlushClockAnim = std::chrono::steady_clock;
	const auto kFlushIntervalAnim = std::chrono::milliseconds( 100 );
	auto lastFlushAnim = FlushClockAnim::now();
	bool earlyAbortAnim = false;

	for( unsigned int y=rect.top; y<=rect.bottom; y++ ) {
		if( framedata.field == FIELD_BOTH || y%2 == (unsigned int)framedata.field ) {
			const Scalar base_scanline_time = framedata.base_cur_time + framedata.scanningRate*y;

			for( unsigned int x=rect.left; x<=rect.right; x++ ) {
				const Scalar base_pixel_time = base_scanline_time + framedata.pixelRate*x;

				Scalar start = 0;
				if( framedata.exposure > 0 ) {
					if( framedata.pixelRate ) {
						start = base_pixel_time;
					} else if( framedata.scanningRate ) {
						start = base_scanline_time;
					} else {
						start = framedata.base_cur_time;
					}
				}

				RISEColor	c;
				IntegratePixel( rc, x, y, height, scene, c, framedata.exposure>0, start, framedata.exposure );
				ColorMath::EnsurePositve(c.base);
				if( c.a < 0.0 ) c.a = 0.0;
				if( c.a > 1.0 ) c.a = 1.0;
				image.SetPEL( x, y, c );
			}
		}

		if( fsBracket && !skipBlockOutput && y < rect.bottom ) {
			const auto now = FlushClockAnim::now();
			if( now - lastFlushAnim >= kFlushIntervalAnim ) {
				for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
					for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
						mFrameStore->EndTile( tx, ty );
					}
				}
				for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
					for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
						mFrameStore->BeginTile( tx, ty );
					}
				}
				lastFlushAnim = now;

				// L8 round 12+15 — intra-block cancellation check
				// (anim path twin of the static path's check).  See
				// SPRasterizeSingleBlock comment for rationale.
				if( pProgressFunc && pProgressFunc->IsCancelled() ) {
					earlyAbortAnim = true;
					break;
				}
			}
		}
	}
	(void)earlyAbortAnim;

	if( fsBracket ) {
		for( size_t ty = fsTy0; ty < fsTy1; ++ty ) {
			for( size_t tx = fsTx0; tx < fsTx1; ++tx ) {
				mFrameStore->EndTile( tx, ty );
			}
		}
	}

	if( !skipBlockOutput ) {
		// After every sequence block, iterate through outputs and get
		// them to intermediate-rasterize.  Skipped for VCM etc.
		RasterizerOutputListType::const_iterator	r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( image, &rect );
		}
	}
}

bool PixelBasedRasterizerHelper::RasterizeScenePass(
	const RuntimeContext::PASS pass,
	const IScene& scene,
	IRasterImage& image,
	const Rect* pRect,
	IRasterizeSequence& seq
	) const
{
	unsigned int startx, starty, endx, endy;
	unsigned int width = image.GetWidth();
	unsigned int height = image.GetHeight();
	BoundsFromRect( startx, starty, endx, endy, pRect, width, height );

	int threads = HowManyThreadsToSpawn();

	static const int MAX_THREADS = 10000;

	if( threads > MAX_THREADS ) {
		threads = MAX_THREADS;
	}

	if( threads > 1 ) {

		// Start the raster sequence
		seq.Begin( startx, endx, starty, endy );

		// Reuse the global thread pool instead of spawning fresh
		// pthreads per pass.  The dispatcher's atomic tile counter
		// does the work distribution; the pool just provides the
		// persistent workers.

		RasterizeBlockDispatcher dispatcher(
			pass, image, scene, seq, *this, pProgressFunc,
			mProgressBase, mProgressWeight, mProgressTotal );

		ThreadPool& pool = GlobalThreadPool();
		const unsigned int numWorkers = static_cast<unsigned int>( threads );
		pool.ParallelFor( numWorkers, [&dispatcher]( unsigned int /*workerIdx*/ ) {
			dispatcher.DoWork();
		} );

		return !dispatcher.WasCancelled();
	} else {

		// Legacy "render in the background" mode: the SP branch runs
		// the entire render on THIS thread (typically the CLI main
		// thread), bypassing the pool whose workers are already at
		// reduced priority.  Lower this thread too so the "every
		// render participant at reduced priority" contract holds.
		// QoS on macOS can only be lowered, not raised — that's fine
		// because legacy mode is explicit opt-in for the whole render
		// lifetime; we don't restore.
		if( GlobalOptions().ReadBool( "force_all_threads_low_priority", false ) ) {
			Threading::riseSetThreadLowPriority( 0 );
		}

		// Otherwise call the SP code
		// Create a runtime context
		RuntimeContext rc( GlobalRNG(), pass, false );
		PrepareRuntimeContext( rc );

		// Get all the parts of the scene we have to render in the order we have to
		// render them
		seq.Begin( startx, endx, starty, endy );

		const unsigned int numseq = seq.NumRegions();
		bool completed = true;

		for( unsigned int i=0; i<numseq; i++ ) {
			const Rect rect = seq.GetNextRegion();

			if( pProgressFunc && i>0 )	{
				if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(numseq-1) ) ) {
					completed = false;
					break;		// abort the render
				}
			}

			SPRasterizeSingleBlock( rc, image, scene, rect, height );
		}

		// MP-path dispatcher workers flush via DoWork's exit hook.
		// The SP path has no such hook — flush explicitly so any t=1
		// splats this thread collected reach the SplatFilm before
		// the caller (BDPT/VCM/MLT) resolves it.  Also unbinds the
		// TLS buffer so a later render's new SplatFilm can't trip
		// over a dangling pointer.
		FlushCallingThreadSplatBuffer();

		return completed;
	}
}

void PixelBasedRasterizerHelper::RasterizeScene(
	const IScene& pScene,
	const Rect* pRect,
	IRasterizeSequence* pRasterSequence
	) const
{
	// Snapshot once at entry — see PredictTimeToRasterizeScene.
	const ICamera* pCam = pScene.GetCamera();
	if( !pCam ) {
		GlobalLog()->PrintSourceError( "PixelBasedRasterizerHelper::RasterizeScene:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

	// Landing 5: propagate the camera's photographic exposure
	// compensation to every output once at render entry.  Default
	// 0 (= no compensation) preserves pre-L5 behaviour for all
	// non-physical cameras.  Both the still-render path
	// (RasterizeScene) and the animation path (RenderFrameOfAnimation)
	// must run this — pixelpel_rasterizer for a still goes through
	// RasterizeScene, so the animation-path-only propagation we
	// originally wired was a missed call site.
	{
		const Scalar camEV = pCam->GetExposureCompensationEV();
		for( RasterizerOutputListType::const_iterator r = outs.begin(), s = outs.end(); r != s; ++r ) {
			(*r)->SetCameraExposureCompensationEV( camEV );
		}
	}

	// Profiling: reset all counters/phases at render entry so successive
	// renders within one CLI session don't accumulate.  Manual start/end
	// for the Render phase (rather than RAII) so we can call
	// PrintProfilingReport() while the Render bucket is already populated.
	RISE_PROFILE_RESET();
#ifdef RISE_ENABLE_PROFILING
	const auto renderProfilingStart = std::chrono::steady_clock::now();
#endif

#ifdef RISE_ENABLE_OIDN
	// Stamp render-start wall clock so the OIDN auto-quality heuristic
	// can compute render_seconds / megapixels at denoise time.
	BeginRenderTimer();
#endif

	// Acquire scene dimensions from the active Film.
	const IFilm* pFilm = pScene.GetFilm();
	const unsigned int width = pFilm->GetWidth();
	const unsigned int height = pFilm->GetHeight();

	IRasterImage* pImage = AcquireRenderImage( width, height );

	// Allocate film buffer for wide-support pixel filter reconstruction
	safe_release( pFilteredFilm );
	safe_release( pFilteredScratch );
	if( UseFilteredFilm() ) {
		pFilteredFilm = new FilteredFilm( width, height );
		pFilteredScratch = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	}

	PrepareImageForNewRender( *pImage, pRect );

	pCaster->AttachScene( &pScene );

	// Eagerly build spatial acceleration structures (BSP/octree) from current
	// world-space bounding boxes before any multi-threaded rendering begins.
	pScene.GetObjects()->PrepareForRendering();

	// Build any pending photon maps (deferred from scene parse).  Safe window:
	// after PrepareForRendering but before any worker thread spawns — matches
	// the irradiance-cache pre-pass precedent below.  Idempotent: consumed
	// requests leave pending=false so repeated RasterizeScene calls are no-ops.
	if( IScenePriv* pScenePriv = dynamic_cast<IScenePriv*>( &const_cast<IScene&>( pScene ) ) ) {
		pScenePriv->BuildPendingPhotonMaps( pProgressFunc );
	}

	// Pre-render hook (e.g. path guiding training)
	PreRenderSetup( pScene, pRect );

	// Compute tile size once — keeps tilesPerThread ≥ 8 across all
	// image dimensions and thread counts.
	unsigned int tileEdge = ComputeTileSize(
		width, height,
		static_cast<unsigned int>( HowManyThreadsToSpawn() ),
		8, 8, 64 );

	// L8 round 8 — align to the FrameStore's tile grid when the
	// rasterizer is writing through `mFrameStore->AsBeautyRasterImage()`
	// (the typical L6c+ path).  Without alignment, adaptive tile sizes
	// smaller than `mFrameStore->TileEdge()` cause multiple worker
	// blocks to compete for the same FrameStore tile's exclusive
	// lock, producing a `bufferMutex_ ↔ tile` lock inversion with the
	// synchronous `OnTileComplete` observer dispatch.  See
	// `AlignTileSizeToFrameStore` doc for the full root cause.
	if( mFrameStore ) {
		tileEdge = AlignTileSizeToFrameStore(
			tileEdge, static_cast<unsigned int>( mFrameStore->TileEdge() ) );
	}

	// If there is no raster sequence, create a default one
	IRasterizeSequence* blocks = 0;
	if( !pRasterSequence ) {
		blocks = CreateDefaultRasterSequence( tileEdge );
		pRasterSequence = blocks;
	}

	// We should do the irradiance pass to populate the cache
	const IIrradianceCache* pIrradianceCache = pScene.GetIrradianceCache();
	if( pIrradianceCache && !pIrradianceCache->Precomputed() ) {
		MortonRasterizeSequence* irrad_seq = new MortonRasterizeSequence( tileEdge );
		if( pProgressFunc ) {
			pProgressFunc->SetTitle( "Irradiance Pass: " );
		}
		RasterizeScenePass( RuntimeContext::PASS_IRRADIANCE_CACHE, pScene, *pImage, pRect, *irrad_seq );
		pIrradianceCache->FinishedPrecomputation();
		safe_release( irrad_seq );

        FlushToOutputs( *pImage, pRect, 0 );

		if( pProgressFunc ) {
			pProgressFunc->SetTitle( "Rasterizing Scene: " );
		}
	}

#ifdef RISE_ENABLE_OIDN
	if( bDenoisingEnabled ) {
		if( !pAOVBuffers ) {
			pAOVBuffers = new AOVBuffers( width, height );
		} else {
			pAOVBuffers->Reset( width, height );
		}
	}
#endif

	bool mainPassCompleted = true;

	if( progressiveConfig.enabled && pSampling )
	{
		// Progressive multi-pass rendering: split the total SPP budget
		// into passes of samplesPerPass each.  Between passes, flush
		// intermediate images to output callbacks for live preview.
		// Converged pixels (via Welford variance) are skipped in later
		// passes; when all pixels converge, break early.

		const unsigned int totalSPP = GetProgressiveTotalSPP();
		const unsigned int spp = progressiveConfig.samplesPerPass > 0 ? progressiveConfig.samplesPerPass : 1;
		const unsigned int numPasses = (totalSPP + spp - 1) / spp;

		ProgressiveFilm progFilm( width, height );
		mProgressiveFilm = &progFilm;
		mTotalProgressiveSPP = totalSPP;

		ISampling2D* pSavedSampling = pSampling;

		// Compute total work units across all passes: tiles × totalSPP.
		// Used by the block dispatcher to report a single 0..1 progress
		// bar across the entire render instead of resetting each pass.
		// Tile divisor MUST match the adaptive `tileEdge` used below,
		// otherwise the dispatcher's actual numTiles and our computed
		// numTilesPerPass diverge and the progress bar overshoots /
		// undershoots 100 %.
		unsigned int renderStartX, renderStartY, renderEndX, renderEndY;
		BoundsFromRect( renderStartX, renderStartY, renderEndX, renderEndY, pRect, width, height );
		const unsigned int renderPixelsX = renderEndX - renderStartX + 1;
		const unsigned int renderPixelsY = renderEndY - renderStartY + 1;
		const unsigned int tilesX = ( renderPixelsX + tileEdge - 1 ) / tileEdge;
		const unsigned int tilesY = ( renderPixelsY + tileEdge - 1 ) / tileEdge;
		const unsigned int numTilesPerPass = tilesX * tilesY;
		const double totalProgressUnits =
			static_cast<double>( numTilesPerPass ) *
			static_cast<double>( totalSPP );

		double accumulatedProgress = 0;

		// Preview cadence — decouple from iteration cadence so we
		// don't resolve + write a PNG every pass on small-scene VCM
		// (where iterations can fire 25×/sec).  Target 7.5 s by
		// default; user can override via scene option in future.
		PreviewScheduler previewScheduler( 7.5 );

		for( unsigned int passIdx = 0; passIdx < numPasses; passIdx++ )
		{
			const unsigned int passSPP = r_min( spp, totalSPP - passIdx * spp );

			ISampling2D* pPassSampling = pSavedSampling->Clone();
			pPassSampling->SetNumSamples( passSPP );
			const_cast<PixelBasedRasterizerHelper*>(this)->pSampling = pPassSampling;

			OnProgressivePassBegin( pScene, passIdx );

			if( pProgressFunc ) {
				pProgressFunc->SetTitle( "Rasterizing Scene: " );
			}

			// Thread the weighted progress params through to the
			// block dispatcher.  Each tile in this pass contributes
			// passSPP work units; we start at accumulatedProgress.
			mProgressBase   = accumulatedProgress;
			mProgressWeight = static_cast<double>( passSPP );
			mProgressTotal  = totalProgressUnits;

			MortonRasterizeSequence* pPassSeq = new MortonRasterizeSequence( tileEdge );
			const bool passCompleted = RasterizeScenePass( RuntimeContext::PASS_NORMAL, pScene, *pImage, pRect, *pPassSeq );
			safe_release( pPassSeq );

			accumulatedProgress += static_cast<double>( numTilesPerPass ) *
			                       static_cast<double>( passSPP );

			const_cast<PixelBasedRasterizerHelper*>(this)->pSampling = pSavedSampling;
			safe_release( pPassSampling );

			if( !passCompleted ) {
				GlobalLog()->PrintEx( eLog_Event,
					"Progressive:: Cancelled after pass %u/%u",
					passIdx+1, numPasses );
				mainPassCompleted = false;
				break;
			}

			const bool isFinalPass = ( passIdx == numPasses - 1 );
			const bool runPreview  = isFinalPass || previewScheduler.ShouldRunPreview();

			if( runPreview ) {
				// Intermediate preview: rebuild the displayed image from the
				// accumulated progressive state so every update is cumulative.
				{
					// L6e-1.1 — bracket the full-image Resolve via
					// RAII so concurrent UI readers see the
					// post-Resolve image, never a torn half-resolved
					// state.  Exception-safe: if Resolve throws, the
					// destructor still releases every tile lock.
					FrameStoreBulkBracket bracket( mFrameStore, *pImage );
					progFilm.Resolve( *pImage );
				}

				IRasterImage& outputImage = GetIntermediateOutputImage( *pImage );
				RasterizerOutputListType::const_iterator r, s;
				for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
					(*r)->OutputIntermediateImage( outputImage, pRect );
				}
				previewScheduler.MarkPreviewRan();

				// Convergence check runs alongside preview — user gets
				// early exit no later than the next preview interval.
				const unsigned int doneCount = progFilm.CountDone( totalSPP );
				if( doneCount >= width * height ) {
					GlobalLog()->PrintEx( eLog_Event,
						"Progressive:: All pixels complete after pass %u/%u",
						passIdx+1, numPasses );
					break;
				}
			}
		}

#ifdef RISE_ENABLE_OIDN
		if( pAOVBuffers ) {
			for( unsigned int y=0; y<height; y++ ) {
				for( unsigned int x=0; x<width; x++ ) {
					const ProgressivePixel& px = progFilm.Get( x, y );
					if( px.alphaSum > 0 ) {
						pAOVBuffers->Normalize( x, y, 1.0 / px.alphaSum );
					}
				}
			}
		}
#endif

		mProgressiveFilm = 0;
		mTotalProgressiveSPP = 0;
		mProgressBase = mProgressWeight = mProgressTotal = 0;
	}
	else
	{
		mainPassCompleted = RasterizeScenePass( RuntimeContext::PASS_NORMAL, pScene, *pImage, pRect, *pRasterSequence );
	}

	// Resolve filtered film: overwrites per-pixel inline estimates with
	// properly filter-reconstructed values.
	// When OIDN denoising is active, skip the film resolve: OIDN is
	// trained on raw MC noise and works poorly on filter-reconstructed
	// images (negative lobes / ringing confuse the denoiser).  The
	// inline box-filtered estimate provides the clean input OIDN needs.
	if( pFilteredFilm ) {
		// L6e-1.1 — bracket the full-image filter resolve via RAII.
		FrameStoreBulkBracket bracket( mFrameStore, *pImage );
#ifdef RISE_ENABLE_OIDN
		if( !bDenoisingEnabled ) {
			pFilteredFilm->Resolve( *pImage );
		}
#else
		pFilteredFilm->Resolve( *pImage );
#endif
	}

#ifdef RISE_ENABLE_PROFILING
	{
		const auto renderProfilingEnd = std::chrono::steady_clock::now();
		const auto ns = (unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				renderProfilingEnd - renderProfilingStart ).count();
		RISE::AddPhaseNanos( RISE::kPhase_Render, ns );
	}
#endif
	RISE_PROFILE_REPORT(GlobalLog());

	if( blocks ) {
		safe_release( blocks );
		blocks = 0;
	}

#ifdef RISE_ENABLE_OIDN
	// `mainPassCompleted` is intentionally NOT passed: cancelled
	// renders still get OIDN'd on whatever was accumulated up to the
	// cancel point.  See ShouldDenoise() and docs/OIDN.md decision
	// log (2026-04-29).
	(void)mainPassCompleted;
	const bool bWillDenoise = ( pAOVBuffers && ShouldDenoise() );
#else
	const bool bWillDenoise = false;
#endif

	if( bWillDenoise ) {
		// Write the pre-denoised (but fully splatted) image to file-based
		// outputs under the normal filename.  Non-file outputs no-op and
		// wait for the denoised final via FlushDenoisedToOutputs.
		FlushPreDenoisedToOutputs( *pImage, pRect, 0 );

#ifdef RISE_ENABLE_OIDN
		if( !pAOVBuffers->HasData() ) {
			OIDNDenoiser::CollectFirstHitAOVs(
				pScene,
				*pCaster,
				*pAOVBuffers,
				GetDenoiseAOVSamplesPerPixel() );
		}
		// L7 — Persist AOV data into the canonical FrameStore so
		// downstream consumers (multichannel EXR, AOV-aware
		// viewports) can read it.  Pre-L7 the AOV data was
		// consumed by `ApplyDenoise` below and discarded; post-L7
		// it lives in the FrameStore Albedo+Normal channels for
		// the FrameStore's lifetime.  Cheap (one full-image copy
		// at end of render); bracketed for concurrent-reader
		// correctness.
		PropagateAOVsToFrameStore_( *pAOVBuffers );
		{
			// L6e-1.1 — bracket the full-image OIDN denoise via RAII.
			// ApplyDenoise reads `*pImage` row-by-row and overwrites
			// every pixel with the denoised output; a concurrent
			// reader would otherwise observe a torn read/write
			// mid-frame.  Critically, OIDN is one of the few
			// bracketed sites that REALISTICALLY can throw (CUDA /
			// Metal device errors, OOM in scratch buffers); the
			// RAII guard's destructor unwinds correctly in that case
			// where the previous Begin/End-pair pattern would have
			// leaked all tile locks → process-wide deadlock.
			FrameStoreBulkBracket bracket( mFrameStore, *pImage );
			mDenoiser->ApplyDenoise( *pImage, *pAOVBuffers, width, height,
				mDenoisingQuality, mDenoisingDevice, mDenoisingPrefilter,
				GetRenderElapsedSeconds() );
		}
#endif

		// File outputs write the denoised image with a "_denoised" suffix;
		// non-file outputs forward to OutputImage so they still see the
		// denoised final (matching pre-change behavior).
		FlushDenoisedToOutputs( *pImage, pRect, 0 );
	} else {
		FlushToOutputs( *pImage, pRect, 0 );
	}

	// Post-render hook (e.g. path guiding cleanup)
	PostRenderCleanup();

	safe_release( pFilteredFilm );
	pFilteredFilm = 0;
	safe_release( pFilteredScratch );
	pFilteredScratch = 0;
	safe_release( pImage );
}

void PixelBasedRasterizerHelper::RenderFrameOfAnimationPass(
	const RuntimeContext::PASS pass,
	const IScene& pScene,
	const Rect* pRect,
	const FIELD field,
	IRasterImage& image,
	const Scalar time,
	IRasterizeSequence& seq,
	const AnimFrameData& framedata
	) const
{
	// Compute the bounds of the region we are going to render
	unsigned int startx, starty, endx, endy;
	unsigned int width = image.GetWidth();
	unsigned int height = image.GetHeight();
	BoundsFromRect( startx, starty, endx, endy, pRect, width, height );

	int threads = HowManyThreadsToSpawn();

	// We can support multiprocessors if exposure is turned on, due to the way we do
	// exposures.  For each pixel sample, the scene time is reset, this is bad for MP

	if( threads>1 && framedata.exposure==0 ) {

		// Start the raster sequence
		seq.Begin( startx, endx, starty, endy );

		// Reuse the global thread pool (see RasterizeScenePass above).

		RasterizeBlockAnimationDispatcher dispatcher(
			pass, image, pScene, seq, *this, pProgressFunc, framedata,
			mProgressBase, mProgressWeight, mProgressTotal );

		ThreadPool& pool = GlobalThreadPool();
		const unsigned int numWorkers = static_cast<unsigned int>( threads );
		pool.ParallelFor( numWorkers, [&dispatcher]( unsigned int /*workerIdx*/ ) {
			dispatcher.DoAnimWork();
		} );

	} else {

		// Legacy low-priority: lower this caller thread too so it
		// joins the rest of the render at reduced priority.  See
		// RasterizeScenePass for the full rationale.
		if( GlobalOptions().ReadBool( "force_all_threads_low_priority", false ) ) {
			Threading::riseSetThreadLowPriority( 0 );
		}

		// Call the SP code

		// Create a runtime context
		RuntimeContext rc( GlobalRNG(), pass, false );
		PrepareRuntimeContext( rc );

		seq.Begin( startx, endx, starty, endy );
		const unsigned int numseq = seq.NumRegions();

		for( unsigned int i=0; i<numseq; i++ ) {
			const Rect rect = seq.GetNextRegion();

			if( pProgressFunc && i>0 )	{
				if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(numseq-1) ) ) {
					break;		// abort the render
				}
			}

			SPRasterizeSingleBlockOfAnimation( rc, image, pScene, rect, height, framedata );
		}

		// Mirrors the non-animation SP path: explicitly flush the
		// TLS splat buffer now that the workers won't do it for us.
		FlushCallingThreadSplatBuffer();
	}
}

void PixelBasedRasterizerHelper::RenderFrameOfAnimation(
	const IScene& pScene,
	const Rect* pRect,
	const FIELD field,
	IRasterImage& image,
	const Scalar time,
	IRasterizeSequence& seq
	) const
{
#ifdef RISE_ENABLE_OIDN
	// Per-frame timer reset so OidnQuality::Auto's render-seconds-per-
	// megapixel heuristic decides each frame independently rather than
	// inflating with cumulative animation time.  See docs/OIDN.md
	// (OIDN-P0-1) for the heuristic.
	BeginRenderTimer();

	// Allocate / reset AOV buffers per frame.  Mirrors RasterizeScene:
	// each frame is its own render, so the AOV state must be fresh.
	// Without the reset, multi-frame animation accumulates AOVs across
	// frames and the denoiser sees a smeared aux signal.
	if( bDenoisingEnabled ) {
		const unsigned int width = image.GetWidth();
		const unsigned int height = image.GetHeight();
		if( !pAOVBuffers ) {
			pAOVBuffers = new AOVBuffers( width, height );
		} else {
			pAOVBuffers->Reset( width, height );
		}
	}
#endif

	// Snapshot once at entry — see PredictTimeToRasterizeScene.
	const ICamera* pCam = pScene.GetCamera();
	if( !pCam ) {
		GlobalLog()->PrintSourceError( "PixelBasedRasterizerHelper::RenderFrameOfAnimation:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

	// Exposure time can change from frame to frame
	const Scalar exposure = pCam->GetExposureTime();
	const Scalar scanningRate = pCam->GetScanningRate();
	const Scalar pixelRate = pCam->GetPixelRate();

	// Landing 5: propagate the camera's photographic exposure
	// compensation to every output once per frame.  ICamera default
	// is 0 (= no compensation), so cameras that don't opt in to
	// physical units leave the outputs unchanged and existing
	// scenes render bit-identically.
	{
		const Scalar camEV = pCam->GetExposureCompensationEV();
		for( RasterizerOutputListType::const_iterator r = outs.begin(), s = outs.end(); r != s; ++r ) {
			(*r)->SetCameraExposureCompensationEV( camEV );
		}
	}

	// To start the progress counting at 0
	DoAnimationFrameProgress( 0, 1 );

	Scalar base_cur_time = (time-(exposure*0.5));

	// When we have scanning rates or pixel rates, then 'exposure' takes a different meaning
	// 'exposure' normally means the amount of time the entire image is exposed to the scene
	// However if the scanningRate is non zero, then 'exposure' is the amount of time that
	// scanline is exposed to the scene.
	// If pixelRate is set, then 'exposure' is amount of time each pixel is exposed to the scene
	// This representation isn't necessarily physically-based, but if the user places their own
	// contraints on how they use this, it is possible to do the physically correct thing
	if( pixelRate ) {
		base_cur_time = time - (image.GetHeight()/2*scanningRate) - (image.GetWidth()/2*pixelRate);
	} else if( scanningRate ) {
		base_cur_time = time - (image.GetHeight()/2*scanningRate);
	}

	AnimFrameData framedata;
	framedata.base_cur_time = base_cur_time;
	framedata.exposure = exposure;
	framedata.field = field;
	framedata.pixelRate = pixelRate;
	framedata.scanningRate = scanningRate;

	// Pre-render hook: e.g. VCM traces light subpaths and populates its
	// light vertex store here.  RasterizeScene calls this before the
	// main render; the animation path historically skipped it, so the
	// eye pass queried an empty store and users saw the block
	// dispatcher paint tile-by-tile instead of VCM's normal whole-
	// image progressive preview.  PreRenderSetup is idempotent across
	// frames — VCM clears and rebuilds its store each call.  It may
	// also flip progressiveConfig.enabled / samplesPerPass on for VM;
	// the per-iteration loop below honors that just like RasterizeScene.
	PreRenderSetup( pScene, pRect );

	// Capture the progress base the animation caller set for this
	// frame — the per-pass loop below extends it; the caller advances
	// it once we return.
	const double frameStartBase = mProgressBase;

	const IIrradianceCache* pIrradianceCache = pScene.GetIrradianceCache();
	if( pIrradianceCache && !pIrradianceCache->Precomputed() ) {
		unsigned int tileEdgeAnim = ComputeTileSize(
			image.GetWidth(), image.GetHeight(),
			static_cast<unsigned int>( HowManyThreadsToSpawn() ),
			8, 8, 64 );
		// L8 round 8 — align to FrameStore tile grid.  See
		// `AlignTileSizeToFrameStore` doc.
		if( mFrameStore && &image == &mFrameStore->AsBeautyRasterImage() ) {
			tileEdgeAnim = AlignTileSizeToFrameStore(
				tileEdgeAnim, static_cast<unsigned int>( mFrameStore->TileEdge() ) );
		}
		MortonRasterizeSequence* irrad_seq = new MortonRasterizeSequence( tileEdgeAnim );
		pProgressFunc->SetTitle( "Irradiance Pass: " );
		// Use legacy per-pass progress for the irradiance pre-pass so
		// it doesn't pollute the movie-wide progress accounting.
		const double savedBase   = mProgressBase;
		const double savedWeight = mProgressWeight;
		const double savedTotal  = mProgressTotal;
		mProgressBase = mProgressWeight = mProgressTotal = 0;
		RenderFrameOfAnimationPass( RuntimeContext::PASS_IRRADIANCE_CACHE, pScene, pRect, field, image, time, *irrad_seq, framedata );
		mProgressBase   = savedBase;
		mProgressWeight = savedWeight;
		mProgressTotal  = savedTotal;
		pIrradianceCache->FinishedPrecomputation();
		safe_release( irrad_seq );
		pProgressFunc->SetTitle( "Rasterizing Animation: " );
	}

	// Main render — progressive loop (VCM SPPM-style) or single pass.
	// Mirrors RasterizeScene's progressive path: split total SPP into
	// passes of samplesPerPass each, rebuild per-iteration state via
	// OnProgressivePassBegin, and resolve ProgressiveFilm into `image`
	// between passes so preview refreshes the whole frame rather than
	// appearing block-by-block.
	if( progressiveConfig.enabled && pSampling )
	{
		const unsigned int width = image.GetWidth();
		const unsigned int height = image.GetHeight();

		const unsigned int totalSPP = GetProgressiveTotalSPP();
		const unsigned int spp = progressiveConfig.samplesPerPass > 0 ? progressiveConfig.samplesPerPass : 1;
		const unsigned int numPasses = (totalSPP + spp - 1) / spp;

		ProgressiveFilm progFilm( width, height );
		mProgressiveFilm = &progFilm;
		mTotalProgressiveSPP = totalSPP;

		ISampling2D* pSavedSampling = pSampling;

		unsigned int tileEdgeAnim = ComputeTileSize(
			width, height,
			static_cast<unsigned int>( HowManyThreadsToSpawn() ),
			8, 8, 64 );
		// L8 round 8 — align to FrameStore tile grid (animation
		// progressive path).  See `AlignTileSizeToFrameStore` doc.
		if( mFrameStore && &image == &mFrameStore->AsBeautyRasterImage() ) {
			tileEdgeAnim = AlignTileSizeToFrameStore(
				tileEdgeAnim, static_cast<unsigned int>( mFrameStore->TileEdge() ) );
		}

		// Number of tiles we'll dispatch per pass — matches what
		// MortonRasterizeSequence will produce below.  Used to extend
		// mProgressBase by (tiles × passSPP) per pass so the single
		// movie-wide progress bar advances smoothly across passes.
		unsigned int animStartX, animStartY, animEndX, animEndY;
		BoundsFromRect( animStartX, animStartY, animEndX, animEndY, pRect, width, height );
		const unsigned int animTilesX = ( ( animEndX - animStartX + 1 ) + tileEdgeAnim - 1 ) / tileEdgeAnim;
		const unsigned int animTilesY = ( ( animEndY - animStartY + 1 ) + tileEdgeAnim - 1 ) / tileEdgeAnim;
		const unsigned int animNumTiles = animTilesX * animTilesY;

		PreviewScheduler previewScheduler( 7.5 );

		for( unsigned int passIdx = 0; passIdx < numPasses; passIdx++ )
		{
			const unsigned int passSPP = r_min( spp, totalSPP - passIdx * spp );

			ISampling2D* pPassSampling = pSavedSampling->Clone();
			pPassSampling->SetNumSamples( passSPP );
			const_cast<PixelBasedRasterizerHelper*>(this)->pSampling = pPassSampling;

			OnProgressivePassBegin( pScene, passIdx );

			if( pProgressFunc ) {
				pProgressFunc->SetTitle( mProgressTotal > 0 ? "Rasterizing Animation: " : "Rasterizing Field/Frame: " );
			}

			// Extend mProgressBase by the cumulative tile-sample units
			// consumed by earlier passes in THIS frame.  With weighted
			// mode (mProgressTotal>0) the block dispatcher reports a
			// single 0..1 across all frames × passes; without it (legacy)
			// the adjustment is harmless because progressTotal=0 still
			// triggers the per-pass 0..1 fallback in GetNextBlock.
			mProgressBase   = frameStartBase
			                + static_cast<double>( passIdx )
			                * static_cast<double>( animNumTiles )
			                * static_cast<double>( passSPP );
			mProgressWeight = static_cast<double>( passSPP );

			MortonRasterizeSequence* pPassSeq = new MortonRasterizeSequence( tileEdgeAnim );
			RenderFrameOfAnimationPass( RuntimeContext::PASS_NORMAL, pScene, pRect, field, image, time, *pPassSeq, framedata );
			safe_release( pPassSeq );

			const_cast<PixelBasedRasterizerHelper*>(this)->pSampling = pSavedSampling;
			safe_release( pPassSampling );

			// Cancellation between passes — break before the remaining
			// iterations so the outer animation loop can flush this
			// frame's partial image (or decide to skip it).  Use the
			// movie-wide progress fraction when in weighted mode so the
			// bar reports one continuous 0..1 across the whole render.
			if( pProgressFunc ) {
				double num, denom;
				if( mProgressTotal > 0 ) {
					num   = mProgressBase + static_cast<double>(animNumTiles) * static_cast<double>(passSPP);
					denom = mProgressTotal;
				} else {
					num   = static_cast<double>(passIdx+1);
					denom = static_cast<double>(numPasses);
				}
				if( !pProgressFunc->Progress( num, denom ) ) {
					GlobalLog()->PrintEx( eLog_Event,
						"RenderFrameOfAnimation:: cancelled after pass %u/%u",
						passIdx+1, numPasses );
					break;
				}
			}

			const bool isFinalPass = ( passIdx == numPasses - 1 );
			const bool runPreview  = isFinalPass || previewScheduler.ShouldRunPreview();

			if( runPreview ) {
				// Rebuild `image` from the accumulated progressive
				// state so each preview is cumulative across passes.
				{
					// L6e-1.1 — bracket via RAII.
					FrameStoreBulkBracket bracket( mFrameStore, image );
					progFilm.Resolve( image );
				}

				IRasterImage& outputImage = GetIntermediateOutputImage( image );
				RasterizerOutputListType::const_iterator r, s;
				for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
					(*r)->OutputIntermediateImage( outputImage, pRect );
				}
				previewScheduler.MarkPreviewRan();
			}
		}

		// Ensure `image` carries the final progressive state even when
		// the loop exited early (cancellation) without a final preview.
		{
			// L6e-1.1 — bracket via RAII.
			FrameStoreBulkBracket bracket( mFrameStore, image );
			progFilm.Resolve( image );
		}

#ifdef RISE_ENABLE_OIDN
		// Normalize per-pixel AOV by progressive-film alpha sum so the
		// denoiser sees averaged aux samples regardless of how many
		// passes accumulated into each pixel.  Mirrors RasterizeScene's
		// progressive branch.  Pixels with zero alphaSum (no samples
		// yet — pre-cancel) are left at zero; the denoiser handles
		// these via cleanAux fallback.
		if( pAOVBuffers ) {
			for( unsigned int y=0; y<height; y++ ) {
				for( unsigned int x=0; x<width; x++ ) {
					const ProgressivePixel& px = progFilm.Get( x, y );
					if( px.alphaSum > 0 ) {
						pAOVBuffers->Normalize( x, y, 1.0 / px.alphaSum );
					}
				}
			}
		}
#endif

		mProgressiveFilm = 0;
		mTotalProgressiveSPP = 0;

		// Leave mProgressBase at frameStartBase — caller advances by a
		// full frame's units after we return.
		mProgressBase = frameStartBase;
	}
	else
	{
		// Single-pass: PT/BDPT/MLT without explicit progressive config.
		// mProgressBase/Weight/Total were set by the caller (they may be
		// zero for legacy per-frame progress).
		RenderFrameOfAnimationPass( RuntimeContext::PASS_NORMAL, pScene, pRect, field, image, time, seq, framedata );
	}

	// Post-render hook (symmetric with RasterizeScene).
	PostRenderCleanup();

	// Resolve filtered film for this frame.
	// When OIDN denoising is active, skip the resolve: OIDN is trained
	// on raw MC noise and the inline box-filtered estimate provides
	// the clean input it expects.  Mirrors RasterizeScene's gate.
	// The Clear() still runs unconditionally so the per-frame film
	// state doesn't carry stale data into the next animation frame.
	if( pFilteredFilm ) {
		// L6e-1.1 — bracket the full-image filter resolve via RAII.
		// pFilteredFilm->Clear() resets the FILM, not `image`, so it
		// stays outside the bracket scope.
		{
			FrameStoreBulkBracket bracket( mFrameStore, image );
#ifdef RISE_ENABLE_OIDN
			if( !bDenoisingEnabled ) {
				pFilteredFilm->Resolve( image );
			}
#else
			pFilteredFilm->Resolve( image );
#endif
		}
		pFilteredFilm->Clear();
	}
}

void PixelBasedRasterizerHelper::RasterizeSceneAnimation(
	const IScene& pScene,
	const Scalar time_start,
	const Scalar time_end,
	const unsigned int num_frames,
	const bool do_fields,
	const bool invert_fields,
	const Rect* pRect,
	const unsigned int* specificFrame,
	IRasterizeSequence* pRasterSequence
	) const
{
	// Snapshot once at entry — see PredictTimeToRasterizeScene.
	const ICamera* pCam = pScene.GetCamera();
	if( !pCam ) {
		GlobalLog()->PrintSourceError( "PixelBasedRasterizerHelper::RasterizeSceneAnimation:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

	// Acquire scene dimensions from the active Film.
	const IFilm* pFilm = pScene.GetFilm();
	const unsigned int width = pFilm->GetWidth();
	const unsigned int height = pFilm->GetHeight();
	const Scalar step_size = num_frames>1?(time_end-time_start)/Scalar(num_frames-1):0;

	// If there is no raster sequence, create a default one
	MortonRasterizeSequence* blocks = 0;
	if( !pRasterSequence ) {
		unsigned int tileEdgeAnim = ComputeTileSize(
			width, height,
			static_cast<unsigned int>( HowManyThreadsToSpawn() ),
			8, 8, 64 );
		// L8 round 8 — align to FrameStore tile grid when the
		// FrameStore matches the render dims (workers will write
		// through `AsBeautyRasterImage`).  See
		// `AlignTileSizeToFrameStore` doc.
		if( mFrameStore &&
		    static_cast<unsigned int>( mFrameStore->Width()  ) == width &&
		    static_cast<unsigned int>( mFrameStore->Height() ) == height )
		{
			tileEdgeAnim = AlignTileSizeToFrameStore(
				tileEdgeAnim, static_cast<unsigned int>( mFrameStore->TileEdge() ) );
		}
		blocks = new MortonRasterizeSequence( tileEdgeAnim );
		pRasterSequence = blocks;
	}

	// L6e-1.1 — Acquire the render image via the helper so the
	// animation render path uses the canonical FrameStore beauty
	// view when one is available (same as the static path post-
	// L6c-1).  Pre-fix this allocated a fresh `RISERasterImage`
	// directly; the per-block bracketing in
	// `SPRasterizeSingleBlockOfAnimation` would then erroneously
	// fire `BeginTile/EndTile` on `mFrameStore` because its dim-
	// check (`image.GetWidth() == mFrameStore->Width()`) was true
	// (animation renders at the same dims as static), even though
	// the per-pixel writes landed in the local RISERasterImage,
	// NOT the FrameStore.  Direct L6e-2 observers would receive
	// `OnTileComplete` for tiles that were never modified.
	// Routing through `AcquireRenderImage` makes the image
	// genuinely the FrameStore view (when dims match) so the
	// bracketing fires for actual writes.
	IRasterImage* pImage = AcquireRenderImage( width, height );

	// Allocate film buffer for wide-support pixel filter reconstruction
	safe_release( pFilteredFilm );
	safe_release( pFilteredScratch );
	if( UseFilteredFilm() ) {
		pFilteredFilm = new FilteredFilm( width, height );
		pFilteredScratch = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	}

	// Get the ray caster ready to roll
	pCaster->AttachScene( &pScene );

	// Build any pending photon maps (deferred from scene parse).
	// Matches RasterizeScene: runs once before the render loop,
	// idempotent on re-entry.  The per-frame PrepareForRendering /
	// SetSceneTime calls below handle transform updates; photon maps
	// themselves aren't rebuilt per frame (that's a separate concern).
	if( IScenePriv* pScenePriv = dynamic_cast<IScenePriv*>( &const_cast<IScene&>( pScene ) ) ) {
		pScenePriv->BuildPendingPhotonMaps( pProgressFunc );
	}

	const bool bHasKeyframedObjects = pScene.GetAnimator()->AreThereAnyKeyframedObjects();

	// Movie-wide progress accounting.  One RenderFrameOfAnimation
	// call processes numTilesPerPass tiles at `totalSPPPerFrame`
	// samples each; we call it once per frame (or twice per frame
	// in fields mode).  Sum across all calls gives a single 0..1
	// progress across the whole render instead of per-pass or per-
	// frame 0..100 cycles.
	const unsigned int total_frames = specificFrame?1:num_frames;
	unsigned int animRenderStartX, animRenderStartY, animRenderEndX, animRenderEndY;
	BoundsFromRect( animRenderStartX, animRenderStartY, animRenderEndX, animRenderEndY, pRect, width, height );
	const unsigned int animRenderPixelsX = animRenderEndX - animRenderStartX + 1;
	const unsigned int animRenderPixelsY = animRenderEndY - animRenderStartY + 1;
	unsigned int animTileEdge = ComputeTileSize(
		width, height,
		static_cast<unsigned int>( HowManyThreadsToSpawn() ),
		8, 8, 64 );
	// L8 round 8 — align to FrameStore tile grid (animation
	// driver loop's tile-count accounting).  Must match the
	// tile size used in `RenderFrameOfAnimation` for the per-pass
	// MortonRasterizeSequence — otherwise progress accounting
	// drifts vs the actual dispatch count.  See
	// `AlignTileSizeToFrameStore` doc.
	if( mFrameStore &&
	    static_cast<unsigned int>( mFrameStore->Width()  ) == width &&
	    static_cast<unsigned int>( mFrameStore->Height() ) == height )
	{
		animTileEdge = AlignTileSizeToFrameStore(
			animTileEdge, static_cast<unsigned int>( mFrameStore->TileEdge() ) );
	}
	const unsigned int animTilesX = ( animRenderPixelsX + animTileEdge - 1 ) / animTileEdge;
	const unsigned int animTilesY = ( animRenderPixelsY + animTileEdge - 1 ) / animTileEdge;
	const unsigned int animNumTilesPerPass = animTilesX * animTilesY;
	const unsigned int totalSPPPerFrame = pSampling ? pSampling->GetNumSamples() : 1;
	const unsigned int animCalls = do_fields ? ( total_frames * 2u ) : total_frames;
	const double unitsPerCall = static_cast<double>( animNumTilesPerPass )
	                          * static_cast<double>( totalSPPPerFrame );
	const double totalProgressUnits = static_cast<double>( animCalls ) * unitsPerCall;
	double accumulatedProgress = 0;
	mProgressTotal  = totalProgressUnits;
	mProgressWeight = static_cast<double>( totalSPPPerFrame );

	// Cancellation note: the inner tile dispatcher breaks out of a
	// single frame when Progress() returns false, but it leaves a
	// partial image behind.  We poll Progress() here between frames
	// (and between fields) so we can (a) skip flushing that partial
	// image to outputs — important for the MOV writer, which would
	// otherwise get a corrupt tail frame — and (b) break out of the
	// animation loop entirely instead of churning through the
	// remaining frames.  The caller (e.g. RISEBridge::rasterizeAnimation)
	// still runs finalize() on the MOV writer after we return.
	bool cancelled = false;
	for( unsigned int i=0; i<total_frames && !cancelled; i++ )
	{
		if( do_fields ) {
			// Render to fields
			Scalar curtime_upper = time_start + Scalar(specificFrame?(*specificFrame):i)*step_size;
			Scalar curtime_lower = curtime_upper + step_size/2;

			// Upper field
			pScene.GetAnimator()->EvaluateAtTime( curtime_upper );
			// Rebuild spatial structure after transforms update but before
			// SetSceneTime, which regenerates photon maps via ray tracing.
			if( bHasKeyframedObjects ) {
				pScene.GetObjects()->InvalidateSpatialStructure();
			}
			pScene.GetObjects()->PrepareForRendering();
			pScene.SetSceneTime( curtime_upper );
			GlobalLog()->PrintEx( eLog_Event, "Rasterizing field %u of %u", (specificFrame?*specificFrame:i)*2 +1, num_frames*2 );
			mProgressBase = accumulatedProgress;
			RenderFrameOfAnimation( pScene, pRect, do_fields?(invert_fields?FIELD_LOWER:FIELD_UPPER):FIELD_BOTH, *pImage, curtime_upper, *pRasterSequence );
			accumulatedProgress += unitsPerCall;

			// If the user cancelled during the upper field, don't
			// render the lower field — the partial image here never
			// gets flushed.
			if( pProgressFunc && !pProgressFunc->Progress( accumulatedProgress, totalProgressUnits ) ) {
				GlobalLog()->PrintEx( eLog_Event, "Animation cancelled during frame %u of %u; skipping remaining frames", (specificFrame?*specificFrame:i)+1, num_frames );
				cancelled = true;
				break;
			}

			// Lower field
			pScene.GetAnimator()->EvaluateAtTime( curtime_lower );
			if( bHasKeyframedObjects ) {
				pScene.GetObjects()->InvalidateSpatialStructure();
			}
			pScene.GetObjects()->PrepareForRendering();
			pScene.SetSceneTime( curtime_lower );
			GlobalLog()->PrintEx( eLog_Event, "Rasterizing field %u of %u", (specificFrame?*specificFrame:i)*2+1 +1, num_frames*2 );
			mProgressBase = accumulatedProgress;
			RenderFrameOfAnimation( pScene, pRect, do_fields?((invert_fields?FIELD_UPPER:FIELD_LOWER)):FIELD_BOTH, *pImage, curtime_lower, *pRasterSequence );
			accumulatedProgress += unitsPerCall;
		} else {
			// Render to frames
			const Scalar curtime = time_start + Scalar(specificFrame?(*specificFrame):i)*step_size;
			pScene.GetAnimator()->EvaluateAtTime( curtime );
			// Rebuild spatial structure after transforms update but before
			// SetSceneTime, which regenerates photon maps via ray tracing.
			if( bHasKeyframedObjects ) {
				pScene.GetObjects()->InvalidateSpatialStructure();
			}
			pScene.GetObjects()->PrepareForRendering();
			pScene.SetSceneTime( curtime );
			GlobalLog()->PrintEx( eLog_Event, "Rasterizing frame %u of %u", (specificFrame?*specificFrame:i) +1, num_frames );

			mProgressBase = accumulatedProgress;
			RenderFrameOfAnimation( pScene, pRect, FIELD_BOTH, *pImage, curtime, *pRasterSequence );
			accumulatedProgress += unitsPerCall;
		}

		// If the user cancelled during this frame, skip the flush so
		// the MOV writer doesn't receive a partial tail frame, and
		// break out of the loop.  This is intentionally STRICTER than
		// RasterizeScene's cancel-still-denoises behaviour: an
		// animation has multi-frame coordination concerns (consistent
		// quality across frames, MOV writer expecting complete tail
		// frames) where a half-rendered + half-denoised frame is worse
		// than no frame at all.  The caller (e.g. RISEBridge) finalizes
		// the MOV writer after we return so the prior completed frames
		// play back correctly.
		if( pProgressFunc && !pProgressFunc->Progress( accumulatedProgress, totalProgressUnits ) ) {
			GlobalLog()->PrintEx( eLog_Event, "Animation cancelled during frame %u of %u; skipping remaining frames", (specificFrame?*specificFrame:i)+1, num_frames );
			cancelled = true;
			break;
		}

		// After every completed frame, flush to outputs.  When
		// denoising is enabled (and AOVs were accumulated), run OIDN
		// per frame and emit both the raw and denoised variants so
		// file outputs that distinguish them (via _denoised suffix)
		// match the still-image behaviour.  See docs/OIDN.md decision
		// log (2026-04-29 animation denoise).
		const unsigned int frameIdx = specificFrame?*specificFrame:i;
#ifdef RISE_ENABLE_OIDN
		if( bDenoisingEnabled && pAOVBuffers && ShouldDenoise() ) {
			FlushPreDenoisedToOutputs( *pImage, pRect, frameIdx );
			if( !pAOVBuffers->HasData() ) {
				OIDNDenoiser::CollectFirstHitAOVs(
					pScene,
					*pCaster,
					*pAOVBuffers,
					GetDenoiseAOVSamplesPerPixel() );
			}
			// L7 — propagate AOVs into the canonical FrameStore for
			// this frame.  See PropagateAOVsToFrameStore_ for the
			// contract.  Animation per-frame: the FrameStore's AOV
			// channels get the LATEST frame's data on each call;
			// observers reading after frameIdx's MarkFrameComplete
			// see frameIdx's AOVs.
			PropagateAOVsToFrameStore_( *pAOVBuffers );
			{
				// L6e-1.1 — bracket the full-image OIDN denoise via RAII.
				FrameStoreBulkBracket bracket( mFrameStore, *pImage );
				mDenoiser->ApplyDenoise( *pImage, *pAOVBuffers, width, height,
					mDenoisingQuality, mDenoisingDevice, mDenoisingPrefilter,
					GetRenderElapsedSeconds() );
			}
			FlushDenoisedToOutputs( *pImage, pRect, frameIdx );
		} else {
			FlushToOutputs( *pImage, pRect, frameIdx );
		}
#else
		FlushToOutputs( *pImage, pRect, frameIdx );
#endif
		{
			// L6e-1.1 — bracket the inter-frame Clear via RAII.
			//
			// L6f known-limitation — `FlushToOutputs` (above) fired
			// `MarkFrameComplete(frameIdx)` synchronously on
			// `mFrameStore`.  Synchronous observers
			// (FileEncoderObserver) finished writing before this
			// Clear executes.  ASYNC observers (e.g. UI repaint
			// patterns that signal a UI thread and return) may
			// wake AFTER the Clear and read black for a beat.
			// Pre-L6f (legacy VFS-internal FrameStore mode) had the
			// same race window: VFS-internal store also got cleared
			// between frames.  No regression vs pre-L6f, just
			// surfaced by the rasterizer-driven Mark* rendering
			// the per-frame timing more obvious.  L6e-3 (interactive
			// VFS migration) will need to address this for
			// animation-in-GUI workflows.
			FrameStoreBulkBracket bracket( mFrameStore, *pImage );
			pImage->Clear( RISEColor(0,0,0,0), pRect );
		}
	}

	// Reset progress-weighting state.
	mProgressBase = mProgressWeight = mProgressTotal = 0;

	RISE_PROFILE_REPORT(GlobalLog());

	if( blocks ) {
		safe_release( blocks );
		blocks = 0;
	}

	safe_release( pFilteredFilm );
	pFilteredFilm = 0;
	safe_release( pFilteredScratch );
	pFilteredScratch = 0;
	ReleaseRenderImage( pImage );
}

// Default IRasterImage acquisition: persistent buffer reused across
// RasterizeScene calls.  All rasterizers benefit:
//   - Interactive viewport: cancel-restart loop reuses previous
//     pixels so cancelled passes degrade gracefully.
//   - Production: GetLastRenderedImage() can serve the buffer to a
//     "save image" feature without re-rendering.
//
// On dimension change (rare for production, common for interactive
// preview-scale stepping) the buffer is reallocated at the new size;
// the previous content is discarded.  On first call the buffer is
// allocated fresh, zero-initialized.
//
// PrepareImageForNewRender (production) clears the buffer to a
// random pastel before each pass so the persistent state is wiped
// before the new render fills it.  Interactive subclasses override
// to skip the clear so previous content shows through.
IRasterImage* PixelBasedRasterizerHelper::AcquireRenderImage( unsigned int width, unsigned int height ) const
{
	// L6c — prefer the canonical FrameStore beauty channel (via the
	// `BeautyRasterImageView` shim) when one has been provided AND
	// its dims match the requested render dims.  Effect: per-pixel
	// `image.SetPEL(x, y, c)` calls in `SPRasterizeSingleBlock` write
	// DIRECTLY into the FrameStore's row-major beauty storage (a
	// `Channel<RISEPel>` indexed `data_[y*width + x]` per
	// Channel.h:130) instead of allocating a separate
	// `RISERasterImage` per render.
	//
	// The legacy `OutputImage → FrameSink → CopyTileFromRasterImage`
	// path stays active.  At L6c-1 the rasterizer's `mFrameStore`
	// and the bridge VFS / FileRasterizerOutput's per-output
	// FrameStore are DIFFERENT instances, so FrameSink performs a
	// real cross-store copy (rasterizer.beauty → output.beauty)
	// that fires the per-tile `BeginTile/EndTile` observer dispatch
	// on the OUTPUT-side store — that's what GUI viewports and
	// `FileEncoderObserver` consume for repaint / disk-write
	// notification.  L6e collapses the bridge VFS into the
	// rasterizer's mFrameStore (single store, observer pinned to
	// the canonical), and L6f retires the redundant FrameSink copy
	// once observers move to direct FrameStore subscription.
	//
	// L6c-1 does NOT yet bracket the rasterizer's per-pixel writes
	// with `BeginTile/EndTile`.  Concurrent-reader correctness on
	// `mFrameStore` is therefore NOT guaranteed in L6c-1 — but no
	// such reader exists today (bridges read their own
	// VFS-internal FrameStore which IS bracketed via FrameSink's
	// CopyTileFromRasterImage).  L6e MUST add the bracketing in
	// `SPRasterizeSingleBlock` before the bridges migrate, or
	// concurrent UI repaints will see torn writes.  TODO L6e.
	//
	// The FrameStore view's `addref` / `release` forward to the
	// owning FrameStore (see FrameStore.cpp:78-80), so the existing
	// AcquireRenderImage / ReleaseRenderImage refcount accounting
	// is preserved.
	//
	// Fallback to mPersistentImage when:
	//   * `mFrameStore` is null (Job didn't allocate one — typically
	//     because no active camera at construction time AND no
	//     post-load push has happened yet, e.g. test rasterizers
	//     constructed via `RISE_API_Create*Rasterizer` directly
	//     without a Job); OR
	//   * FrameStore dims differ from the requested render dims
	//     (e.g. `InteractivePelRasterizer` rendering at a preview-
	//     scale subset of the camera's full resolution).
	//
	// **NOTE:** BDPT and VCM rasterizers do NOT call
	// `AcquireRenderImage` — they allocate fresh `RISERasterImage`
	// instances inside their own `RasterizeScene` overrides
	// (BDPTRasterizerBase.cpp:803, VCMRasterizerBase similar).
	// L6c-1 therefore only impacts the PT / pixelpel /
	// pixelintegratingspectral / interactive / MLT path that flows
	// through `PixelBasedRasterizerHelper::RasterizeScene`.  L6d
	// folds the others in.
	if( mFrameStore ) {
		const unsigned int fsW =
			static_cast<unsigned int>( mFrameStore->Width() );
		const unsigned int fsH =
			static_cast<unsigned int>( mFrameStore->Height() );
		if( fsW == width && fsH == height ) {
			IRasterImage& view = mFrameStore->AsBeautyRasterImage();
			view.addref();  // forwards to FrameStore::addref
			return &view;
		}
	}

	// Phase 1 fallback (mPersistentImage path).
	if( !mPersistentImage || mPersistentW != width || mPersistentH != height ) {
		safe_release( mPersistentImage );
		mPersistentImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
		GlobalLog()->PrintNew( mPersistentImage, __FILE__, __LINE__, "persistent-image" );
		mPersistentW = width;
		mPersistentH = height;
	}
	// Hand a counted reference to the caller; the matching
	// ReleaseRenderImage drops it.  Our own persistent reference
	// (refcount 1 from `new`) keeps the image alive for the full
	// rasterizer lifetime.
	mPersistentImage->addref();
	return mPersistentImage;
}

#ifdef RISE_ENABLE_OIDN
// L7 — Thin wrapper delegating to the free utility in
// FrameStore.cpp.  Kept as a member for source compatibility with
// the existing call sites in this file (RasterizeScene + animation
// + BDPT subclass via inheritance).  L7 follow-up extracted the
// real implementation so MLTRasterizer (which doesn't inherit from
// PixelBasedRasterizerHelper) can use the same logic.  See
// `RISE::Implementation::PropagateAOVsToFrameStore` in FrameStore.h
// for the contract.
void PixelBasedRasterizerHelper::PropagateAOVsToFrameStore_( const AOVBuffers& aov ) const
{
	RISE::Implementation::PropagateAOVsToFrameStore( mFrameStore, aov );
}
#endif  // RISE_ENABLE_OIDN

void PixelBasedRasterizerHelper::ReleaseRenderImage( IRasterImage* pImage ) const
{
	// Drop the addref AcquireRenderImage handed to the caller; the
	// persistent reference keeps the image alive across calls.
	safe_release( pImage );
}

// L6e-1.1 — Full-image bulk-write bracketing has been hoisted to the
// `Implementation::FrameStoreBulkBracket` RAII guard in FrameStore.h.
// Use it at every full-image write site (Resolve / Clear / Denoise /
// SplatFilm composition).  See its header doc for the contract.

// Default per-render entry: clear to a random pastel (debug visual —
// uncovered tiles stand out) and fire OutputIntermediateImage so any
// observer sees the "render started" signal.  Interactive subclasses
// override to skip both: the previous frame's pixels remain visible
// while new tiles render in place, eliminating cancel-restart flashes.
void PixelBasedRasterizerHelper::PrepareImageForNewRender( IRasterImage& img, const Rect* pRect ) const
{
	{
		// L6e-1.1 — bracket the bulk Clear via RAII so an exception
		// thrown by `Clear` (extremely unlikely for a primitive
		// memset-shaped op, but principled) still releases all tile
		// locks.  No-op when `img` isn't the FrameStore beauty view.
		FrameStoreBulkBracket bracket( mFrameStore, img );
		// GlobalRNG is fine here — single-threaded prelude.
		img.Clear( RISEColor(
			GlobalRNG().CanonicalRandom()*0.6+0.3,
			GlobalRNG().CanonicalRandom()*0.6+0.3,
			GlobalRNG().CanonicalRandom()*0.6+0.3,
			1.0 ), pRect );
	}

	for( RasterizerOutputListType::const_iterator r = outs.begin(), s = outs.end(); r != s; ++r ) {
		(*r)->OutputIntermediateImage( img, pRect );
	}
}

// Default tile-dispatch order: Morton (Z-curve), starting upper-left.
// Production rasterizers don't care because they always render to
// completion — the order only affects which tiles populate the
// framebuffer first.  Interactive subclasses override to return
// centre-out so partial buffers from cancelled passes have useful
// content in the middle of the frame.
IRasterizeSequence* PixelBasedRasterizerHelper::CreateDefaultRasterSequence( unsigned int tileEdge ) const
{
	return new MortonRasterizeSequence( tileEdge );
}

// L6f — Frame-complete signaling moves into the rasterizer.
//
// Pre-L6f, `MarkFrameComplete` / `MarkPreDenoiseComplete` /
// `MarkDenoiseComplete` on the FrameStore was driven by
// `FrameSink::Output*Image` (legacy IRasterizerOutput → FrameStore
// adapter).  That worked when the VFS owned a separate internal
// FrameStore (FrameSink fed it), but post-L6e-2 the VFS observes
// the rasterizer's CANONICAL `mFrameStore` directly — and the
// legacy IRasterizerOutput chain becomes redundant for that
// signaling.
//
// Post-L6f, the rasterizer fires the Mark* events on its own
// `mFrameStore` AFTER the legacy IRasterizerOutput dispatch so:
//
//   * Legacy IRasterizerOutput consumers (FileRasterizerOutput,
//     callback sinks, FrameSink-backed VFS instances in
//     internal-managed mode) see `OutputImage` first, do their work
//     (file write, internal-FrameStore copy + own-MarkFrameComplete).
//
//   * THEN the rasterizer's `MarkFrameComplete` fires on the
//     CANONICAL FrameStore.  Direct FrameStore observers (post-L6e-2
//     bound VFS, FileEncoderObserver) receive it from the rasterizer
//     side, NOT from the IRasterizerOutput chain.
//
//   * VFS in bound mode is now a no-op for `OutputImage` — the
//     rasterizer-side Mark* is the sole frame-complete signal.
//     See `ViewportFrameStore::OutputImage`'s post-L6f short-
//     circuit.
//
// `mFrameStore` is null for MLT (which opted out of the FrameStore
// push via `AcceptsFrameStorePush()`); the null-check below is the
// degenerate-case guard.

void PixelBasedRasterizerHelper::FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	// Write to output objects (legacy IRasterizerOutput chain).
	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputImage( img, rcRegion, frame );
	}
	// L6f — fire `OnFrameComplete` on canonical-FrameStore observers.
	if( mFrameStore ) {
		mFrameStore->MarkFrameComplete( frame );
	}
}

void PixelBasedRasterizerHelper::FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputPreDenoisedImage( img, rcRegion, frame );
	}
	// L6f — fire `OnPreDenoiseComplete` on canonical-FrameStore observers.
	if( mFrameStore ) {
		mFrameStore->MarkPreDenoiseComplete( frame );
	}
}

void PixelBasedRasterizerHelper::FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputDenoisedImage( img, rcRegion, frame );
	}
	// L6f — fire `OnDenoiseComplete` on canonical-FrameStore observers.
	if( mFrameStore ) {
		mFrameStore->MarkDenoiseComplete( frame );
	}
}

// Our own functions
void PixelBasedRasterizerHelper::SetProgressiveConfig( const ProgressiveConfig& config )
{
	progressiveConfig = config;
	if( progressiveConfig.samplesPerPass == 0 ) {
		progressiveConfig.samplesPerPass = 1;
	}
}

void PixelBasedRasterizerHelper::SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ )
{
	if( pSampling_ )
	{
		safe_release( pSampling );

		pSampling = pSampling_;
		pSampling->addref();
	}

	if( pPixelFilter_ )
	{
		safe_release( pPixelFilter );

		if( pSampling ) {
			pPixelFilter = pPixelFilter_;
			pPixelFilter->addref();
		}
	}
}
