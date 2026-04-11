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

#include "ScanlineRasterizeSequence.h"
#include "BlockRasterizeSequence.h"
#include "HilbertRasterizeSequence.h"
#include "MortonRasterizeSequence.h"
#include "ProgressiveFilm.h"
#include "../RISE_API.h"

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
  mProgressiveFilm( 0 ),
  mTotalProgressiveSPP( 0 )
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
#ifdef RISE_ENABLE_OIDN
	delete pAOVBuffers;
#endif
}

void PixelBasedRasterizerHelper::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	rc.pProgressiveFilm = mProgressiveFilm;
	rc.totalProgressiveSPP = mTotalProgressiveSPP;
}

unsigned int PixelBasedRasterizerHelper::GetProgressiveTotalSPP() const
{
	return pSampling ? pSampling->GetNumSamples() : 0;
}

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

	// Draw red toggles to show we are working on this tile
	DrawToggles( image, rect, RISEColor( 1,0,0,1 ), 0.20 );

	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputIntermediateImage( image, &rect );
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

	// Get the image for intermediate output.  BDPT returns a scratch
	// copy with resolved splats; other rasterizers return the primary.
	IRasterImage& outputImage = GetIntermediateOutputImage( image );

	// Also iterate through outputs and get them to intermediate rasterize
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputIntermediateImage( outputImage, &rect );
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
	if( framedata.field == FIELD_BOTH ) {
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

	// After every sequence block, report progress
	// Also iterate through outputs and get them to intermediate rasterize
	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputIntermediateImage( image, &rect );
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

		// Our MP solution is to spawn a thread for each processor and it will come back to us and request tiles
		// as it completes them

		RasterizeBlockDispatcher dispatcher( pass, image, scene, seq, *this, pProgressFunc );

		RISETHREADID thread_ids[MAX_THREADS];
		for( int i=0; i<threads; i++ ) {
			Threading::riseCreateThread( RasterizeBlock_ThreadProc, (void*)&dispatcher, 0, 0, &thread_ids[i] );
		}

		// Then wait for all the threads to complete
		for( int i=0; i<threads; i++ ) {
			Threading::riseWaitUntilThreadFinishes( thread_ids[i], 0 );
		}

		return !dispatcher.WasCancelled();
	} else {

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

	// Acquire scene dimensions
	const unsigned int width = pScene.GetCamera()->GetWidth();
	const unsigned int height = pScene.GetCamera()->GetHeight();

	IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	GlobalLog()->PrintNew( pImage, __FILE__, __LINE__, "image" );

	// Allocate film buffer for wide-support pixel filter reconstruction
	safe_release( pFilteredFilm );
	safe_release( pFilteredScratch );
	if( UseFilteredFilm() ) {
		pFilteredFilm = new FilteredFilm( width, height );
		pFilteredScratch = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	}

	{
		// GlobalRNG is ok here since this part will always be single threaded
		pImage->Clear( RISEColor( GlobalRNG().CanonicalRandom()*0.6+0.3, GlobalRNG().CanonicalRandom()*0.6+0.3, GlobalRNG().CanonicalRandom()*0.6+0.3, 1.0 ), pRect );

		// Empty the intermediate output image
		RasterizerOutputListType::const_iterator	r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( *pImage, pRect );
		}
	}

	pCaster->AttachScene( &pScene );

	// Eagerly build spatial acceleration structures (BSP/octree) from current
	// world-space bounding boxes before any multi-threaded rendering begins.
	pScene.GetObjects()->PrepareForRendering();

	// Pre-render hook (e.g. path guiding training)
	PreRenderSetup( pScene, pRect );

	// If there is no raster sequence, create a default one
	MortonRasterizeSequence* blocks = 0;
	if( !pRasterSequence ) {
		blocks = new MortonRasterizeSequence( 32 );
		pRasterSequence = blocks;
	}

	// We should do the irradiance pass to populate the cache
	const IIrradianceCache* pIrradianceCache = pScene.GetIrradianceCache();
	if( pIrradianceCache && !pIrradianceCache->Precomputed() ) {
		MortonRasterizeSequence* irrad_seq = new MortonRasterizeSequence( 32 );
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
	delete pAOVBuffers;
	pAOVBuffers = 0;
	if( bDenoisingEnabled ) {
		pAOVBuffers = new AOVBuffers( width, height );
	}
#endif

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

		for( unsigned int passIdx = 0; passIdx < numPasses; passIdx++ )
		{
			const unsigned int passSPP = r_min( spp, totalSPP - passIdx * spp );

			ISampling2D* pPassSampling = pSavedSampling->Clone();
			pPassSampling->SetNumSamples( passSPP );
			const_cast<PixelBasedRasterizerHelper*>(this)->pSampling = pPassSampling;

			if( pProgressFunc ) {
				char title[128];
				snprintf( title, sizeof(title), "Progressive Pass [%u/%u]: ", passIdx+1, numPasses );
				pProgressFunc->SetTitle( title );
			}

			MortonRasterizeSequence* pPassSeq = new MortonRasterizeSequence( 32 );
			const bool passCompleted = RasterizeScenePass( RuntimeContext::PASS_NORMAL, pScene, *pImage, pRect, *pPassSeq );
			safe_release( pPassSeq );

			const_cast<PixelBasedRasterizerHelper*>(this)->pSampling = pSavedSampling;
			safe_release( pPassSampling );

			// Intermediate preview: rebuild the displayed image from the
			// accumulated progressive state so every update is cumulative.
			progFilm.Resolve( *pImage );

			IRasterImage& outputImage = GetIntermediateOutputImage( *pImage );
			RasterizerOutputListType::const_iterator r, s;
			for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
				(*r)->OutputIntermediateImage( outputImage, pRect );
			}

			if( !passCompleted ) {
				GlobalLog()->PrintEx( eLog_Event,
					"Progressive:: Cancelled after pass %u/%u",
					passIdx+1, numPasses );
				break;
			}

			// Check whether every pixel has converged or reached the sample cap.
			const unsigned int doneCount = progFilm.CountDone( totalSPP );
			if( doneCount >= width * height ) {
				GlobalLog()->PrintEx( eLog_Event,
					"Progressive:: All pixels complete after pass %u/%u",
					passIdx+1, numPasses );
				break;
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
	}
	else
	{
		RasterizeScenePass( RuntimeContext::PASS_NORMAL, pScene, *pImage, pRect, *pRasterSequence );
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

#ifdef RISE_ENABLE_OIDN
	if( bDenoisingEnabled && pAOVBuffers ) {
		if( !pAOVBuffers->HasData() ) {
			OIDNDenoiser::CollectFirstHitAOVs( pScene, *pCaster, *pAOVBuffers );
		}
		OIDNDenoiser::ApplyDenoise( *pImage, *pAOVBuffers, width, height );
		delete pAOVBuffers;
		pAOVBuffers = 0;
	}
#endif

	if( blocks ) {
		safe_release( blocks );
		blocks = 0;
	}

	FlushToOutputs( *pImage, pRect, 0 );

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

		// Our MP solution is to spawn a thread for each processor and it will come back to us and request tiles
		// as it completes them

		RasterizeBlockAnimationDispatcher dispatcher( pass, image, pScene, seq, *this, pProgressFunc, framedata );

		RISETHREADID thread_ids[256];
		for( int i=0; i<threads; i++ ) {
			Threading::riseCreateThread( RasterizeBlockAnimation_ThreadProc, (void*)&dispatcher, 0, 0, &thread_ids[i] );
		}

		// Then wait for all the threads to complete
		for( int i=0; i<threads; i++ ) {
			Threading::riseWaitUntilThreadFinishes( thread_ids[i], 0 );
		}

	} else {

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

	const IIrradianceCache* pIrradianceCache = pScene.GetIrradianceCache();
	if( pIrradianceCache && !pIrradianceCache->Precomputed() ) {
		MortonRasterizeSequence* irrad_seq = new MortonRasterizeSequence( 32 );
		pProgressFunc->SetTitle( "Irradiance Pass: " );
		RenderFrameOfAnimationPass( RuntimeContext::PASS_IRRADIANCE_CACHE, pScene, pRect, field, image, time, *irrad_seq, framedata );
		pIrradianceCache->FinishedPrecomputation();
		safe_release( irrad_seq );
		pProgressFunc->SetTitle( "Rasterizing Field/Frame: " );
	}

	// Do a pass
	RenderFrameOfAnimationPass( RuntimeContext::PASS_NORMAL, pScene, pRect, field, image, time, seq, framedata );

	// Resolve filtered film for this frame
	if( pFilteredFilm ) {
		pFilteredFilm->Resolve( image );
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
		blocks = new MortonRasterizeSequence( 32 );
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

	const bool bHasKeyframedObjects = pScene.GetAnimator()->AreThereAnyKeyframedObjects();

	for( unsigned int i=0; i<(specificFrame?1:num_frames); i++ )
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
			RenderFrameOfAnimation( pScene, pRect, do_fields?(invert_fields?FIELD_LOWER:FIELD_UPPER):FIELD_BOTH, *pImage, curtime_upper, *pRasterSequence );

			// Lower field
			pScene.GetAnimator()->EvaluateAtTime( curtime_lower );
			if( bHasKeyframedObjects ) {
				pScene.GetObjects()->InvalidateSpatialStructure();
			}
			pScene.GetObjects()->PrepareForRendering();
			pScene.SetSceneTime( curtime_lower );
			GlobalLog()->PrintEx( eLog_Event, "Rasterizing field %u of %u", (specificFrame?*specificFrame:i)*2+1 +1, num_frames*2 );
			RenderFrameOfAnimation( pScene, pRect, do_fields?((invert_fields?FIELD_UPPER:FIELD_LOWER)):FIELD_BOTH, *pImage, curtime_lower, *pRasterSequence );
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

			RenderFrameOfAnimation( pScene, pRect, FIELD_BOTH, *pImage, curtime, *pRasterSequence );
		}
		// After every frame, flush to outputs
		FlushToOutputs( *pImage, pRect, specificFrame?*specificFrame:i );
		pImage->Clear( RISEColor(0,0,0,0), pRect );
	}

	RISE_PROFILE_REPORT(GlobalLog());

	if( blocks ) {
		safe_release( blocks );
		blocks = 0;
	}

	safe_release( pFilteredFilm );
	pFilteredFilm = 0;
	safe_release( pFilteredScratch );
	pFilteredScratch = 0;
	safe_release( pImage );
}

void PixelBasedRasterizerHelper::FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	// Write to output objects
	RasterizerOutputListType::const_iterator	r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputImage( img, rcRegion, frame );
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
