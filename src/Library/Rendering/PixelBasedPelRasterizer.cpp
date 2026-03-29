//////////////////////////////////////////////////////////////////////
//
//  PixelBasedPelRasterizer.cpp - Implements the basic pixel based
//  rasterizer which rasterizers RISEColor PELs
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 23, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PixelBasedPelRasterizer.h"
#include "../Utilities/SobolSampler.h"
#include "../Sampling/SobolSequence.h"
#include "BlockRasterizeSequence.h"
#include "../RasterImages/RasterImage.h"

#ifdef RISE_ENABLE_OPENPGL
#include "../RISE_API.h"
#endif

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedPelRasterizer::PixelBasedPelRasterizer(
	IRayCaster* pCaster_,
	const PathGuidingConfig& guidingCfg
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  guidingConfig( guidingCfg )
#ifdef RISE_ENABLE_OPENPGL
  ,pGuidingField( 0 )
#endif
{
}

PixelBasedPelRasterizer::~PixelBasedPelRasterizer( )
{
#ifdef RISE_ENABLE_OPENPGL
	safe_release( pGuidingField );
#endif
}

void PixelBasedPelRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
#ifdef RISE_ENABLE_OPENPGL
	if( pGuidingField ) {
		rc.pGuidingField = pGuidingField;
		rc.guidingAlpha = guidingConfig.alpha;
		rc.maxGuidingDepth = guidingConfig.maxGuidingDepth;
	}
#endif
}

void PixelBasedPelRasterizer::PreRenderSetup(
	const IScene& pScene,
	const Rect* pRect
	) const
{
#ifdef RISE_ENABLE_OPENPGL
	safe_release( pGuidingField );

	if( guidingConfig.enabled )
	{
		// Scene bounds for the guiding field's spatial structure.
		// OpenPGL's KD-tree adapts based on actual sample positions,
		// so a generous default bounding box works fine.
		Point3 boundsMin( -1e4, -1e4, -1e4 );
		Point3 boundsMax( 1e4, 1e4, 1e4 );

		pGuidingField = new PathGuidingField( guidingConfig, boundsMin, boundsMax );
		pGuidingField->addref();

		const unsigned int width = pScene.GetCamera()->GetWidth();
		const unsigned int height = pScene.GetCamera()->GetHeight();

		// Run training iterations
		for( unsigned int trainIter = 0; trainIter < guidingConfig.trainingIterations; trainIter++ )
		{
			pGuidingField->BeginTrainingIteration();

			// Use a temporary low-spp sampling object for training
			ISampling2D* pTrainSampling = 0;
			RISE_API_CreateUniformSampling2D( &pTrainSampling, 1.0, 1.0 );
			if( pTrainSampling ) {
				pTrainSampling->SetNumSamples( guidingConfig.trainingSPP );
			}

			ISampling2D* pSavedSampling = pSampling;
			const_cast<PixelBasedPelRasterizer*>(this)->pSampling = pTrainSampling;

			// Create a temporary image for training (results are discarded)
			IRasterImage* pTrainImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );

			if( pProgressFunc ) {
				char title[128];
				snprintf( title, sizeof(title), "Path Guiding Training [%u/%u]: ", trainIter+1, guidingConfig.trainingIterations );
				pProgressFunc->SetTitle( title );
			}

			// Single-threaded training to avoid thread-safety issues with AddSample
			{
				RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );
				PrepareRuntimeContext( rc );

				BlockRasterizeSequence* pTrainBlocks = new BlockRasterizeSequence( 64, 64, 2 );
				pTrainBlocks->addref();
				unsigned int startx, starty, endx, endy;
				BoundsFromRect( startx, starty, endx, endy, pRect, width, height );
				pTrainBlocks->Begin( startx, endx, starty, endy );

				const unsigned int numseq = pTrainBlocks->NumRegions();
				for( unsigned int i=0; i<numseq; i++ ) {
					const Rect rect = pTrainBlocks->GetNextRegion();
					if( pProgressFunc && i>0 ) {
						pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(numseq-1) );
					}
					SPRasterizeSingleBlock( rc, *pTrainImage, pScene, rect, height );
				}
				safe_release( pTrainBlocks );
			}

			// Restore state
			const_cast<PixelBasedPelRasterizer*>(this)->pSampling = pSavedSampling;
			safe_release( pTrainSampling );
			safe_release( pTrainImage );

			pGuidingField->EndTrainingIteration();
		}

		if( pProgressFunc ) {
			pProgressFunc->SetTitle( "Rasterizing Scene: " );
		}

		GlobalLog()->PrintEx( eLog_Event,
			"PathGuidingField:: Training phase complete" );
	}
#endif
}

void PixelBasedPelRasterizer::PostRenderCleanup() const
{
#ifdef RISE_ENABLE_OPENPGL
	safe_release( pGuidingField );
#endif
}

void PixelBasedPelRasterizer::IntegratePixel(
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
	RasterizerState rast = {x,y};

	// If we have a sampling object, then we want to sub-sample each pixel, so
	// do that
	// Derive a per-pixel seed for Owen scrambling from pixel coordinates
	const uint32_t pixelSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x),
		static_cast<uint32_t>(y) );

	if( pSampling && pPixelFilter && rc.pass == RuntimeContext::PASS_NORMAL )
	{
		RISEPel			colAccrued( 0, 0, 0 );

		ISampling2D::SamplesList2D samples;
		pSampling->GenerateSamplePoints(rc.random, samples);

		Scalar weights = 0;
		Scalar alphas = 0;

		uint32_t sampleIndex = 0;
		ISampling2D::SamplesList2D::const_iterator		m, n;
		for( m=samples.begin(), n=samples.end(); m!=n; m++, sampleIndex++ )
		{
			RISEPel			c;
			Point2		ptOnScreen;
			const Scalar weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
			weights += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			// Install a Sobol sampler for this pixel sample so that
			// shader ops (PathTracingShaderOp) can use low-discrepancy
			// sampling across the full path recursion.
			SobolSampler sobolSampler( sampleIndex, pixelSeed );
			rc.pSampler = &sobolSampler;

			Ray ray;
			if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
				if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
					colAccrued = colAccrued + c*weight;
					alphas += weight;
				}
			}

			rc.pSampler = 0;
		}

		// Divide out by the number of samples
		colAccrued = colAccrued * (alphas>0?(1.0/alphas):0);
		cret = RISEColor(colAccrued, alphas/weights);
	}
	else
	{
		SobolSampler sobolSampler( 0, pixelSeed );
		rc.pSampler = &sobolSampler;

		RISEPel	c;
		Ray ray;
		if( pScene.GetCamera()->GenerateRay( rc, ray, Point2(x, height-y) ) ) {
			if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
				cret = RISEColor( c, 1.0 );
			}
		}

		rc.pSampler = 0;
	}
}
