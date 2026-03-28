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
//       - Strategies with t==1 (needsSplat=true) are accumulated
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
#include "AOVBuffers.h"
#include "OIDNDenoiser.h"

using namespace RISE;
using namespace RISE::Implementation;

BDPTRasterizerBase::BDPTRasterizerBase(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth,
	const ManifoldSolverConfig& smsConfig
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  pIntegrator( 0 ),
  pManifoldSolver( 0 ),
  pSplatFilm( 0 ),
  pScratchImage( 0 ),
  mSplatTotalSamples( 1.0 )
#ifdef RISE_ENABLE_OIDN
  ,pAOVBuffers( 0 )
#endif
{
	pIntegrator = new BDPTIntegrator( maxEyeDepth, maxLightDepth );
	pIntegrator->addref();

	if( smsConfig.enabled )
	{
		pManifoldSolver = new ManifoldSolver( smsConfig );
		pManifoldSolver->addref();
		pIntegrator->SetManifoldSolver( pManifoldSolver );
	}
}

BDPTRasterizerBase::~BDPTRasterizerBase()
{
	safe_release( pIntegrator );
	safe_release( pManifoldSolver );
	safe_release( pSplatFilm );
	safe_release( pScratchImage );
#ifdef RISE_ENABLE_OIDN
	delete pAOVBuffers;
	pAOVBuffers = 0;
#endif
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

#ifdef RISE_ENABLE_OIDN
	// Allocate AOV buffers for denoiser auxiliary input
	delete pAOVBuffers;
	pAOVBuffers = 0;
	if( bDenoisingEnabled ) {
		pAOVBuffers = new AOVBuffers( width, height );
	}
#endif

	// Compute total sample count for splat film resolve/unresolve.
	// Must be set before any blocks render so the progressive hooks work.
	mSplatTotalSamples = 1.0;
	if( pSampling ) {
		mSplatTotalSamples = static_cast<Scalar>( pSampling->GetNumSamples() );
	}
	mSplatTotalSamples *= GetSplatSampleScale();

	// Create the primary image and a scratch copy for progressive output
	IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	GlobalLog()->PrintNew( pImage, __FILE__, __LINE__, "image" );

	safe_release( pScratchImage );
	pScratchImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	pScratchImage->addref();

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

	// Resolve splat film: add t==1 strategy contributions to the primary
	// image.  Each pixel sample may have contributed one splat per (s,t)
	// strategy with needsSplat=true, so we divide by the total number of
	// pixel samples to get the correct per-pixel average.
	pSplatFilm->Resolve( *pImage, mSplatTotalSamples );

	RISE_PROFILE_REPORT(GlobalLog());

#ifdef RISE_ENABLE_OIDN
	// Run OIDN denoiser as a post-process on the fully-resolved image
	if( pAOVBuffers && bDenoisingEnabled ) {
		OIDNDenoiser::ApplyDenoise( *pImage, *pAOVBuffers, width, height );
	}
#endif

	if( blocks ) {
		safe_release( blocks );
	}

	// Final output
	FlushToOutputs( *pImage, pRect, 0 );

	safe_release( pImage );
	safe_release( pSplatFilm );
	pSplatFilm = 0;
	safe_release( pScratchImage );
	pScratchImage = 0;
#ifdef RISE_ENABLE_OIDN
	delete pAOVBuffers;
	pAOVBuffers = 0;
#endif
}

IRasterImage& BDPTRasterizerBase::GetIntermediateOutputImage( IRasterImage& primary ) const
{
	if( !pSplatFilm || !pScratchImage ) {
		return primary;
	}

	// Copy the current primary image into the scratch buffer
	const unsigned int w = primary.GetWidth();
	const unsigned int h = primary.GetHeight();
	for( unsigned int y=0; y<h; y++ ) {
		for( unsigned int x=0; x<w; x++ ) {
			pScratchImage->SetPEL( x, y, primary.GetPEL( x, y ) );
		}
	}

	// Resolve splats into the scratch copy (primary is untouched)
	pSplatFilm->Resolve( *pScratchImage, mSplatTotalSamples );

	return *pScratchImage;
}
