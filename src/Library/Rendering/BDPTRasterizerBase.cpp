//////////////////////////////////////////////////////////////////////
//
//  BDPTRasterizerBase.cpp - Implementation of the BDPT rasterizer
//    base class.
//
//  RENDERING PIPELINE:
//    1. Create a SplatFilm the same size as the output image.
//    2. Run the standard pixel-based render loop (multi-threaded,
//       block-based).  Each pixel calls IntegratePixel(), which
//       generates subpaths and evaluates all (s,t) strategies.
//       - Strategies with t>=2 contribute directly to the pixel.
//       - Strategies with t<=1 (needsSplat=true) are accumulated
//         into the SplatFilm at the projected raster position.
//    3. After all pixels are rendered, resolve the SplatFilm:
//       divide accumulated splats by the total sample count
//       and add them to the primary image.
//    4. Output the final composited image.
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
#include "BDPTRasterizerBase.h"
#include "../Lights/LightSampler.h"
#include "../RasterImages/RasterImage.h"
#include "BlockRasterizeSequence.h"
#include "RasterizeDispatchers.h"
#include "../Utilities/Profiling.h"

using namespace RISE;
using namespace RISE::Implementation;

BDPTRasterizerBase::BDPTRasterizerBase(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  pIntegrator( 0 ),
  pSplatFilm( 0 )
{
	pIntegrator = new BDPTIntegrator( maxEyeDepth, maxLightDepth );
	pIntegrator->addref();
}

BDPTRasterizerBase::~BDPTRasterizerBase()
{
	safe_release( pIntegrator );
	safe_release( pSplatFilm );
}

// Thread procedure for BDPT block rendering - mirrors RasterizeBlock_ThreadProc
static void* BDPTRasterizeBlock_ThreadProc( void* lpParameter )
{
	RasterizeBlockDispatcher* pDispatcher = (RasterizeBlockDispatcher*)lpParameter;
	pDispatcher->DoWork();
	return 0;
}

void BDPTRasterizerBase::RasterizeScene(
	const IScene& pScene,
	const Rect* pRect,
	IRasterizeSequence* pRasterSequence
	) const
{
	if( !pScene.GetCamera() ) {
		GlobalLog()->PrintSourceError( "BDPTRasterizerBase::RasterizeScene:: Scene contains no camera!", __FILE__, __LINE__ );
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
		pProgressFunc->SetTitle( GetProgressTitle() );
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

	// Resolve splat film: add t<=1 strategy contributions to the primary
	// image.  Each pixel sample may have contributed one splat per (s,t)
	// strategy with needsSplat=true, so we divide by the total number of
	// pixel samples to get the correct per-pixel average.
	Scalar totalSamples = 1.0;
	if( pSampling ) {
		totalSamples = static_cast<Scalar>( pSampling->GetNumSamples() );
	}
	totalSamples *= GetSplatSampleScale();
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
