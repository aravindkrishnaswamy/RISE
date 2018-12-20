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
#include "../RasterImages/RasterImage.h"
#include "RasterizeDispatchers.h"

#include "ScanlineRasterizeSequence.h"
#include "BlockRasterizeSequence.h"
#include "HilbertRasterizeSequence.h"

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedRasterizerHelper::PixelBasedRasterizerHelper(
	IRayCaster* pCaster_
	) :
  pCaster( pCaster_ ),
  pSampling( 0 ),
  pPixelFilter( 0 )
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

	// Also iterate through outputs and get them to intermediate rasterize
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputIntermediateImage( image, &rect );
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

void PixelBasedRasterizerHelper::RasterizeScenePass(
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
	} else {

		// Otherwise call the SP code
		// Create a runtime context
		RuntimeContext rc( GlobalRNG(), pass, false );

		// Get all the parts of the scene we have to render in the order we have to
		// render them
		seq.Begin( startx, endx, starty, endy );

		const unsigned int numseq = seq.NumRegions();

		for( unsigned int i=0; i<numseq; i++ ) {
			const Rect rect = seq.GetNextRegion();

			if( pProgressFunc && i>0 )	{
				if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(numseq-1) ) ) {
					break;		// abort the render
				}
			}

			SPRasterizeSingleBlock( rc, image, scene, rect, height );
		}
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

	// If there is no raster sequence, create a default one
	BlockRasterizeSequence* blocks = 0;
	if( !pRasterSequence ) {
		blocks = new BlockRasterizeSequence( 32, 24, 2 );
		pRasterSequence = blocks;
	}

	// We should do the irradiance pass to populate the cache
	IIrradianceCache* pIrradianceCache = pScene.GetIrradianceCache();
	if( pIrradianceCache && !pIrradianceCache->Precomputed() ) {
		BlockRasterizeSequence* irrad_seq = new BlockRasterizeSequence( 32, 24, 6 );
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

	RasterizeScenePass( RuntimeContext::PASS_NORMAL, pScene, *pImage, pRect, *pRasterSequence );

	if( blocks ) {
		safe_release( blocks );
		blocks = 0;
	}

	FlushToOutputs( *pImage, pRect, 0 );

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

	IIrradianceCache* pIrradianceCache = pScene.GetIrradianceCache();
	if( pIrradianceCache && !pIrradianceCache->Precomputed() ) {
		BlockRasterizeSequence* irrad_seq = new BlockRasterizeSequence( 32, 24, 6 );
		pProgressFunc->SetTitle( "Irradiance Pass: " );
		RenderFrameOfAnimationPass( RuntimeContext::PASS_IRRADIANCE_CACHE, pScene, pRect, field, image, time, *irrad_seq, framedata );
		pIrradianceCache->FinishedPrecomputation();
		safe_release( irrad_seq );
		pProgressFunc->SetTitle( "Rasterizing Field/Frame: " );
	}

	// Do a pass
	RenderFrameOfAnimationPass( RuntimeContext::PASS_NORMAL, pScene, pRect, field, image, time, seq, framedata );
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
	BlockRasterizeSequence* blocks = 0;
	if( !pRasterSequence ) {
		blocks = new BlockRasterizeSequence( 32, 24, 2 );
		pRasterSequence = blocks;
	}

	// Create the image we are going to render to
	IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	GlobalLog()->PrintNew( pImage, __FILE__, __LINE__, "image" );

	// Get the ray caster ready to roll
	pCaster->AttachScene( &pScene );

	for( unsigned int i=0; i<(specificFrame?1:num_frames); i++ )
	{
		if( do_fields ) {
			// Render to fields
			Scalar curtime_upper = time_start + Scalar(specificFrame?(*specificFrame):i)*step_size;
			Scalar curtime_lower = curtime_upper + step_size/2;

			// Upper field
			pScene.GetAnimator()->EvaluateAtTime( curtime_upper );
			pScene.SetSceneTime( curtime_upper );
			GlobalLog()->PrintEx( eLog_Event, "Rasterizing field %u of %u", (specificFrame?*specificFrame:i)*2 +1, num_frames*2 );
			RenderFrameOfAnimation( pScene, pRect, do_fields?(invert_fields?FIELD_LOWER:FIELD_UPPER):FIELD_BOTH, *pImage, curtime_upper, *pRasterSequence );

			// Lower field
			pScene.GetAnimator()->EvaluateAtTime( curtime_lower );
			pScene.SetSceneTime( curtime_lower );
			GlobalLog()->PrintEx( eLog_Event, "Rasterizing field %u of %u", (specificFrame?*specificFrame:i)*2+1 +1, num_frames*2 );
			RenderFrameOfAnimation( pScene, pRect, do_fields?((invert_fields?FIELD_UPPER:FIELD_LOWER)):FIELD_BOTH, *pImage, curtime_lower, *pRasterSequence );
		} else {
			// Render to frames
			const Scalar curtime = time_start + Scalar(specificFrame?(*specificFrame):i)*step_size;
			pScene.GetAnimator()->EvaluateAtTime( curtime );
			pScene.SetSceneTime( curtime );
			GlobalLog()->PrintEx( eLog_Event, "Rasterizing frame %u of %u", (specificFrame?*specificFrame:i) +1, num_frames );

			RenderFrameOfAnimation( pScene, pRect, FIELD_BOTH, *pImage, curtime, *pRasterSequence );
		}
		// After every frame, flush to outputs
		FlushToOutputs( *pImage, pRect, specificFrame?*specificFrame:i );
		pImage->Clear( RISEColor(0,0,0,0), pRect );
	}

	if( blocks ) {
		safe_release( blocks );
		blocks = 0;
	}

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
