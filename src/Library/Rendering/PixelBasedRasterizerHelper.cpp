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

#include "ScanlineRasterizeSequence.h"
#include "BlockRasterizeSequence.h"
#include "HilbertRasterizeSequence.h"
#include "MortonRasterizeSequence.h"
#include "ProgressiveFilm.h"
#include "../RISE_API.h"
#include "../Interfaces/IScenePriv.h"

#ifdef RISE_ENABLE_OIDN
#include "AOVBuffers.h"
#include "OIDNDenoiser.h"
#endif

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedRasterizerHelper::PixelBasedRasterizerHelper(
	IRayCaster* pCaster_
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
	if( !pScene.GetCamera() ) {
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

	// Acquire scene dimensions
	const unsigned int width = pScene.GetCamera()->GetWidth();
	const unsigned int height = pScene.GetCamera()->GetHeight();

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
		if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
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

	if( !skipBlockOutput ) {
		// Draw red toggles to show we are working on this tile
		DrawToggles( image, rect, RISEColor( 1,0,0,1 ), 0.20 );

		RasterizerOutputListType::const_iterator	r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( image, &rect );
		}
	}

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

	if( !skipBlockOutput && framedata.field == FIELD_BOTH ) {
		// Draw red toggles to show we are working on this tile
		DrawToggles( image, rect, RISEColor( 1,0,0,1 ), 0.20 );

		RasterizerOutputListType::const_iterator	r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( image, &rect );
		}
	}

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
	if( !pScene.GetCamera() ) {
		GlobalLog()->PrintSourceError( "PixelBasedRasterizerHelper::RasterizeScene:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

#ifdef RISE_ENABLE_OIDN
	// Stamp render-start wall clock so the OIDN auto-quality heuristic
	// can compute render_seconds / megapixels at denoise time.
	BeginRenderTimer();
#endif

	// Acquire scene dimensions
	const unsigned int width = pScene.GetCamera()->GetWidth();
	const unsigned int height = pScene.GetCamera()->GetHeight();

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
	const unsigned int tileEdge = ComputeTileSize(
		width, height,
		static_cast<unsigned int>( HowManyThreadsToSpawn() ),
		8, 8, 64 );

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
				progFilm.Resolve( *pImage );

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
#ifdef RISE_ENABLE_OIDN
		if( !bDenoisingEnabled ) {
			pFilteredFilm->Resolve( *pImage );
		}
#else
		pFilteredFilm->Resolve( *pImage );
#endif
	}

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
		mDenoiser->ApplyDenoise( *pImage, *pAOVBuffers, width, height,
			mDenoisingQuality, mDenoisingDevice, mDenoisingPrefilter,
			GetRenderElapsedSeconds() );
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

	// Exposure time can change from frame to frame
	const Scalar exposure = pScene.GetCamera()->GetExposureTime();
	const Scalar scanningRate = pScene.GetCamera()->GetScanningRate();
	const Scalar pixelRate = pScene.GetCamera()->GetPixelRate();

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
		const unsigned int tileEdgeAnim = ComputeTileSize(
			image.GetWidth(), image.GetHeight(),
			static_cast<unsigned int>( HowManyThreadsToSpawn() ),
			8, 8, 64 );
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

		const unsigned int tileEdgeAnim = ComputeTileSize(
			width, height,
			static_cast<unsigned int>( HowManyThreadsToSpawn() ),
			8, 8, 64 );

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
				progFilm.Resolve( image );

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
		progFilm.Resolve( image );

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
#ifdef RISE_ENABLE_OIDN
		if( !bDenoisingEnabled ) {
			pFilteredFilm->Resolve( image );
		}
#else
		pFilteredFilm->Resolve( image );
#endif
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
	// We need a camera
	if( !pScene.GetCamera() ) {
		GlobalLog()->PrintSourceError( "PixelBasedRasterizerHelper::RasterizeScene:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

	// Acquire scene dimensions
	const unsigned int width = pScene.GetCamera()->GetWidth();
	const unsigned int height = pScene.GetCamera()->GetHeight();
	const Scalar step_size = num_frames>1?(time_end-time_start)/Scalar(num_frames-1):0;

	// If there is no raster sequence, create a default one
	MortonRasterizeSequence* blocks = 0;
	if( !pRasterSequence ) {
		const unsigned int tileEdgeAnim = ComputeTileSize(
			width, height,
			static_cast<unsigned int>( HowManyThreadsToSpawn() ),
			8, 8, 64 );
		blocks = new MortonRasterizeSequence( tileEdgeAnim );
		pRasterSequence = blocks;
	}

	// Create the image we are going to render to
	IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	GlobalLog()->PrintNew( pImage, __FILE__, __LINE__, "image" );

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
	const unsigned int animTileEdge = ComputeTileSize(
		width, height,
		static_cast<unsigned int>( HowManyThreadsToSpawn() ),
		8, 8, 64 );
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
			mDenoiser->ApplyDenoise( *pImage, *pAOVBuffers, width, height,
				mDenoisingQuality, mDenoisingDevice, mDenoisingPrefilter,
				GetRenderElapsedSeconds() );
			FlushDenoisedToOutputs( *pImage, pRect, frameIdx );
		} else {
			FlushToOutputs( *pImage, pRect, frameIdx );
		}
#else
		FlushToOutputs( *pImage, pRect, frameIdx );
#endif
		pImage->Clear( RISEColor(0,0,0,0), pRect );
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

void PixelBasedRasterizerHelper::ReleaseRenderImage( IRasterImage* pImage ) const
{
	// Drop the addref AcquireRenderImage handed to the caller; the
	// persistent reference keeps the image alive across calls.
	safe_release( pImage );
}

// Default per-render entry: clear to a random pastel (debug visual —
// uncovered tiles stand out) and fire OutputIntermediateImage so any
// observer sees the "render started" signal.  Interactive subclasses
// override to skip both: the previous frame's pixels remain visible
// while new tiles render in place, eliminating cancel-restart flashes.
void PixelBasedRasterizerHelper::PrepareImageForNewRender( IRasterImage& img, const Rect* pRect ) const
{
	// GlobalRNG is fine here — single-threaded prelude.
	img.Clear( RISEColor(
		GlobalRNG().CanonicalRandom()*0.6+0.3,
		GlobalRNG().CanonicalRandom()*0.6+0.3,
		GlobalRNG().CanonicalRandom()*0.6+0.3,
		1.0 ), pRect );

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

void PixelBasedRasterizerHelper::FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	// Write to output objects
	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputImage( img, rcRegion, frame );
	}
}

void PixelBasedRasterizerHelper::FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputPreDenoisedImage( img, rcRegion, frame );
	}
}

void PixelBasedRasterizerHelper::FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputDenoisedImage( img, rcRegion, frame );
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
