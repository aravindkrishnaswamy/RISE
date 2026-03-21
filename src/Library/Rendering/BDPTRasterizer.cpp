//////////////////////////////////////////////////////////////////////
//
//  BDPTRasterizer.cpp - Implementation of the BDPT rasterizer.
//    Supports both RGB and spectral (per-wavelength) rendering.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BDPTRasterizer.h"
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../RasterImages/RasterImage.h"
#include "BlockRasterizeSequence.h"
#include "RasterizeDispatchers.h"
#include "../Utilities/Profiling.h"

using namespace RISE;
using namespace RISE::Implementation;

BDPTRasterizer::BDPTRasterizer(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  PixelBasedPelRasterizer( pCaster_ ),
  pIntegrator( 0 ),
  pSplatFilm( 0 ),
  bSpectralMode( false ),
  lambda_begin( 380.0 ),
  lambda_end( 780.0 ),
  num_wavelengths( 10 ),
  nSpectralSamples( 1 )
{
	pIntegrator = new BDPTIntegrator( maxEyeDepth, maxLightDepth );
	pIntegrator->addref();
}

BDPTRasterizer::~BDPTRasterizer()
{
	safe_release( pIntegrator );
	safe_release( pSplatFilm );
}

void BDPTRasterizer::SetSpectralMode(
	const Scalar lambdaBegin,
	const Scalar lambdaEnd,
	const unsigned int numWavelengths,
	const unsigned int spectralSamples
	)
{
	bSpectralMode = true;
	lambda_begin = lambdaBegin;
	lambda_end = lambdaEnd;
	num_wavelengths = numWavelengths;
	nSpectralSamples = spectralSamples;
}

//////////////////////////////////////////////////////////////////////
// IntegratePixelNM - evaluates BDPT at a specific wavelength for
// a single camera ray.  Returns the scalar radiance estimate.
//////////////////////////////////////////////////////////////////////

Scalar BDPTRasterizer::IntegratePixelNM(
	const RuntimeContext& rc,
	const Point2& ptOnScreen,
	const IScene& pScene,
	const ICamera& camera,
	const Scalar nm
	) const
{
	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return 0;
	}

	IndependentSampler* pSampler = new IndependentSampler( rc.random );

	std::vector<BDPTVertex> lightVerts;
	std::vector<BDPTVertex> eyeVerts;

	pIntegrator->GenerateLightSubpathNM( pScene, *pCaster, *pSampler, lightVerts, nm );
	pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, ptOnScreen, pScene, *pCaster, *pSampler, eyeVerts, nm );

	std::vector<BDPTIntegrator::ConnectionResultNM> results =
		pIntegrator->EvaluateAllStrategiesNM( lightVerts, eyeVerts, pScene, *pCaster, camera, nm );

	safe_release( pSampler );

	Scalar sampleValue = 0;

	for( unsigned int r = 0; r < results.size(); r++ )
	{
		const BDPTIntegrator::ConnectionResultNM& cr = results[r];
		if( !cr.valid ) {
			continue;
		}

		const Scalar weighted = cr.contribution * cr.misWeight;

		if( cr.needsSplat && pSplatFilm )
		{
			// For spectral splatting, convert the scalar at this wavelength
			// to XYZ before splatting
			XYZPel thisXYZ( 0, 0, 0 );
			if( weighted > 0 && ColorUtils::XYZFromNM( thisXYZ, nm ) ) {
				thisXYZ = thisXYZ * weighted;
				const int sx = static_cast<int>( cr.rasterPos.x );
				const int sy = static_cast<int>( cr.rasterPos.y );

				if( sx >= 0 && sy >= 0 &&
					static_cast<unsigned int>(sx) < camera.GetWidth() &&
					static_cast<unsigned int>(sy) < camera.GetHeight() )
				{
					pSplatFilm->Splat( sx, sy, RISEPel( thisXYZ.X, thisXYZ.Y, thisXYZ.Z ) );
				}
			}
		}
		else
		{
			sampleValue += weighted;
		}
	}

	return sampleValue;
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel - main pixel integration routine.
// Handles both RGB and spectral modes.
//////////////////////////////////////////////////////////////////////

void BDPTRasterizer::IntegratePixel(
	const RuntimeContext& rc,
	const unsigned int x,
	const unsigned int y,
	const unsigned int height,
	const IScene& pScene,
	RISEColor& cret,
	const bool temporal_samples,
	const Scalar temporal_start,
	const Scalar temporal_exposure
	) const
{
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		return;
	}

	// Determine how many samples to take
	ISampling2D::SamplesList2D samples;
	bool bMultiSample = false;

	if( pSampling && pPixelFilter && rc.pass == RuntimeContext::PASS_NORMAL )
	{
		pSampling->GenerateSamplePoints( rc.random, samples );
		bMultiSample = true;
	}
	else
	{
		samples.push_back( Point2( 0, 0 ) );
	}

	if( bSpectralMode )
	{
		//
		// SPECTRAL MODE: sample wavelengths, accumulate XYZ
		//
		const Scalar lambda_diff = lambda_end - lambda_begin;
		const Scalar wavelength_steps = lambda_diff / Scalar(num_wavelengths);

		XYZPel colAccrued( 0, 0, 0 );
		Scalar weights = 0;

		ISampling2D::SamplesList2D::const_iterator m, n;
		for( m=samples.begin(), n=samples.end(); m!=n; m++ )
		{
			Point2 ptOnScreen;
			Scalar weight = 1.0;

			if( bMultiSample ) {
				weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
			} else {
				ptOnScreen = Point2( x, height-y );
			}
			weights += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			// For each pixel sample, take nSpectralSamples wavelength samples
			XYZPel spectralSum( 0, 0, 0 );

			for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
			{
				// Sample a random wavelength
				const Scalar nm = num_wavelengths < 10000 ?
					(lambda_begin + int(rc.random.CanonicalRandom() * Scalar(num_wavelengths)) * wavelength_steps) :
					(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);

				const Scalar nmvalue = IntegratePixelNM( rc, ptOnScreen, pScene, *pCamera, nm );

				if( nmvalue > 0 ) {
					XYZPel thisNM( 0, 0, 0 );
					if( ColorUtils::XYZFromNM( thisNM, nm ) ) {
						thisNM = thisNM * nmvalue;
						spectralSum = spectralSum + thisNM;
					}
				}
			}

			// Average over spectral samples
			spectralSum = spectralSum * (1.0 / Scalar(nSpectralSamples));
			colAccrued = colAccrued + spectralSum * weight;
		}

		if( weights > 0 ) {
			colAccrued = colAccrued * (1.0 / weights);
			cret = RISEColor( RISEPel( colAccrued.X, colAccrued.Y, colAccrued.Z ), 1.0 );
		}
	}
	else
	{
		//
		// RGB MODE: standard BDPT
		//
		RISEPel colAccrued( 0, 0, 0 );
		Scalar weights = 0;
		Scalar alphas = 0;

		ISampling2D::SamplesList2D::const_iterator m, n;
		for( m=samples.begin(), n=samples.end(); m!=n; m++ )
		{
			Point2 ptOnScreen;
			Scalar weight = 1.0;

			if( bMultiSample ) {
				weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
			} else {
				ptOnScreen = Point2( x, height-y );
			}
			weights += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			Ray cameraRay;
			if( !pCamera->GenerateRay( rc, cameraRay, ptOnScreen ) ) {
				continue;
			}

			IndependentSampler* pSampler = new IndependentSampler( rc.random );

			std::vector<BDPTVertex> lightVerts;
			std::vector<BDPTVertex> eyeVerts;

			pIntegrator->GenerateLightSubpath( pScene, *pCaster, *pSampler, lightVerts );
			pIntegrator->GenerateEyeSubpath( rc, cameraRay, ptOnScreen, pScene, *pCaster, *pSampler, eyeVerts );

			std::vector<BDPTIntegrator::ConnectionResult> results =
				pIntegrator->EvaluateAllStrategies( lightVerts, eyeVerts, pScene, *pCaster, *pCamera );

			safe_release( pSampler );

			RISEPel sampleColor( 0, 0, 0 );

			for( unsigned int r=0; r<results.size(); r++ )
			{
				const BDPTIntegrator::ConnectionResult& cr = results[r];
				if( !cr.valid ) {
					continue;
				}

				const RISEPel weighted = cr.contribution * cr.misWeight;

				if( cr.needsSplat && pSplatFilm )
				{
					const int sx = static_cast<int>( cr.rasterPos.x );
					const int sy = static_cast<int>( cr.rasterPos.y );

					if( sx >= 0 && sy >= 0 &&
						static_cast<unsigned int>(sx) < pCamera->GetWidth() &&
						static_cast<unsigned int>(sy) < pCamera->GetHeight() )
					{
						pSplatFilm->Splat( sx, sy, weighted );
					}
				}
				else
				{
					sampleColor = sampleColor + weighted;
				}
			}

			colAccrued = colAccrued + sampleColor * weight;
			alphas += weight;
		}

		if( alphas > 0 ) {
			colAccrued = colAccrued * (1.0 / alphas);
			cret = RISEColor( colAccrued, alphas / weights );
		}
	}
}

// Thread procedure for BDPT block rendering - mirrors RasterizeBlock_ThreadProc
static void* BDPTRasterizeBlock_ThreadProc( void* lpParameter )
{
	RasterizeBlockDispatcher* pDispatcher = (RasterizeBlockDispatcher*)lpParameter;
	pDispatcher->DoWork();
	return 0;
}

void BDPTRasterizer::RasterizeScene(
	const IScene& pScene,
	const Rect* pRect,
	IRasterizeSequence* pRasterSequence
	) const
{
	if( !pScene.GetCamera() ) {
		GlobalLog()->PrintSourceError( "BDPTRasterizer::RasterizeScene:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

	// Initialize the light sampler from the scene
	LightSampler* pLS = new LightSampler();
	pIntegrator->SetLightSampler( pLS );
	safe_release( pLS );

	// Acquire scene dimensions
	const unsigned int width = pScene.GetCamera()->GetWidth();
	const unsigned int height = pScene.GetCamera()->GetHeight();

	// Create the splat film for s<=1 strategies
	safe_release( pSplatFilm );
	pSplatFilm = new SplatFilm( width, height );
	pSplatFilm->addref();

	// Create the primary image
	IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	GlobalLog()->PrintNew( pImage, __FILE__, __LINE__, "image" );

	{
		pImage->Clear( RISEColor( GlobalRNG().CanonicalRandom()*0.6+0.3, GlobalRNG().CanonicalRandom()*0.6+0.3, GlobalRNG().CanonicalRandom()*0.6+0.3, 1.0 ), pRect );

		RasterizerOutputListType::const_iterator r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( *pImage, pRect );
		}
	}

	pCaster->AttachScene( &pScene );

	// If there is no raster sequence, create a default one
	BlockRasterizeSequence* blocks = 0;
	if( !pRasterSequence ) {
		blocks = new BlockRasterizeSequence( 64, 64, 2 );
		pRasterSequence = blocks;
	}

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( bSpectralMode ? "BDPT Spectral Rasterizing: " : "BDPT Rasterizing: " );
	}

	// Render pass: dispatch blocks to threads
	{
		unsigned int startx, starty, endx, endy;
		BoundsFromRect( startx, starty, endx, endy, pRect, width, height );

		int threads = HowManyThreadsToSpawn();
		static const int MAX_THREADS = 10000;
		if( threads > MAX_THREADS ) {
			threads = MAX_THREADS;
		}

		if( threads > 1 )
		{
			pRasterSequence->Begin( startx, endx, starty, endy );

			RasterizeBlockDispatcher dispatcher( RuntimeContext::PASS_NORMAL, *pImage, pScene, *pRasterSequence, *this, pProgressFunc );

			RISETHREADID thread_ids[MAX_THREADS];
			for( int i=0; i<threads; i++ ) {
				Threading::riseCreateThread( BDPTRasterizeBlock_ThreadProc, (void*)&dispatcher, 0, 0, &thread_ids[i] );
			}

			for( int i=0; i<threads; i++ ) {
				Threading::riseWaitUntilThreadFinishes( thread_ids[i], 0 );
			}
		}
		else
		{
			RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );

			pRasterSequence->Begin( startx, endx, starty, endy );

			const unsigned int numseq = pRasterSequence->NumRegions();

			for( unsigned int i=0; i<numseq; i++ ) {
				const Rect rect = pRasterSequence->GetNextRegion();

				if( pProgressFunc && i>0 ) {
					if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(numseq-1) ) ) {
						break;
					}
				}

				SPRasterizeSingleBlock( rc, *pImage, pScene, rect, height );
			}
		}
	}

	// Resolve splat film: add s<=1 contributions to the primary image
	Scalar totalSamples = 1.0;
	if( pSampling ) {
		totalSamples = static_cast<Scalar>( pSampling->GetNumSamples() );
	}
	if( bSpectralMode ) {
		// In spectral mode, each pixel sample takes nSpectralSamples wavelength samples
		// The splatted values are already XYZ-converted and accumulated
		totalSamples *= static_cast<Scalar>( nSpectralSamples );
	}
	pSplatFilm->Resolve( *pImage, totalSamples );

	RISE_PROFILE_REPORT(GlobalLog());

	if( blocks ) {
		safe_release( blocks );
	}

	// Final output
	FlushToOutputs( *pImage, pRect, 0 );

	safe_release( pImage );
	safe_release( pSplatFilm );
	pSplatFilm = 0;
}
