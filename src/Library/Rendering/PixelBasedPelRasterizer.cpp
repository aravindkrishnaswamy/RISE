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

#ifdef RISE_ENABLE_OPENPGL
namespace
{
	inline bool IsFiniteBoundingBox( const BoundingBox& bbox )
	{
		return
			std::isfinite( bbox.ll.x ) && std::isfinite( bbox.ll.y ) && std::isfinite( bbox.ll.z ) &&
			std::isfinite( bbox.ur.x ) && std::isfinite( bbox.ur.y ) && std::isfinite( bbox.ur.z ) &&
			bbox.ll.x <= bbox.ur.x &&
			bbox.ll.y <= bbox.ur.y &&
			bbox.ll.z <= bbox.ur.z;
	}

	class SceneBoundsEnumerator :
		public IEnumCallback<IObject>
	{
	public:
		BoundingBox bounds;
		bool hasBounds;

		SceneBoundsEnumerator() :
			bounds(
				Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
				Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) ),
			hasBounds( false )
		{
		}

		virtual bool operator() ( const IObject& object )
		{
			const BoundingBox bbox = object.getBoundingBox();
			if( IsFiniteBoundingBox( bbox ) ) {
				if( hasBounds ) {
					bounds.Include( bbox );
				} else {
					bounds = bbox;
					hasBounds = true;
				}
			}
			return true;
		}
	};

	inline void ComputeGuidingSceneBounds(
		const IScene& scene,
		Point3& boundsMin,
		Point3& boundsMax
		)
	{
		SceneBoundsEnumerator enumerator;
		if( scene.GetObjects() ) {
			scene.GetObjects()->EnumerateObjects( enumerator );
		}

		if( enumerator.hasBounds ) {
			enumerator.bounds.SanityCheck();
			enumerator.bounds.EnsureBoxHasVolume();
			boundsMin = enumerator.bounds.ll;
			boundsMax = enumerator.bounds.ur;
		} else {
			boundsMin = Point3( -1e4, -1e4, -1e4 );
			boundsMax = Point3( 1e4, 1e4, 1e4 );
		}
	}
}
#endif

PixelBasedPelRasterizer::PixelBasedPelRasterizer(
	IRayCaster* pCaster_,
	const PathGuidingConfig& guidingCfg
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
#ifdef RISE_ENABLE_OPENPGL
  pGuidingField( 0 ),
  guidingAlphaScale( 1.0 ),
#endif
  guidingConfig( guidingCfg )
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
		rc.guidingAlpha = guidingConfig.alpha * guidingAlphaScale;
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
	guidingAlphaScale = 1.0;

	if( guidingConfig.enabled )
	{
		Point3 boundsMin;
		Point3 boundsMax;
		ComputeGuidingSceneBounds( pScene, boundsMin, boundsMax );

		pGuidingField = new PathGuidingField( guidingConfig, boundsMin, boundsMax );
		pGuidingField->addref();

		const unsigned int width = pScene.GetCamera()->GetWidth();
		const unsigned int height = pScene.GetCamera()->GetHeight();
		const unsigned int bootstrapTrainingSPP = 1;
		unsigned int currentTrainingSPP = bootstrapTrainingSPP;
		Scalar previousPositiveSampleDensity = 0;
		Scalar previousIndirectEnergyDensity = 0;
		unsigned int lowGainPasses = 0;

		// Run training iterations
		for( unsigned int trainIter = 0; trainIter < guidingConfig.trainingIterations; trainIter++ )
		{
			pGuidingField->BeginTrainingIteration();

			// Use a temporary low-spp sampling object for training
			ISampling2D* pTrainSampling = 0;
			RISE_API_CreateUniformSampling2D( &pTrainSampling, 1.0, 1.0 );
			if( pTrainSampling ) {
				pTrainSampling->SetNumSamples( currentTrainingSPP );
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

			const Scalar cameraSamples =
				static_cast<Scalar>( width ) *
				static_cast<Scalar>( height ) *
				static_cast<Scalar>( currentTrainingSPP );
			const size_t positiveSamples = pGuidingField->GetLastAddedSurfaceSampleCount();
			const Scalar totalSampleEnergy = pGuidingField->GetLastAddedSurfaceSampleEnergy();
			const Scalar indirectSampleEnergy =
				pGuidingField->GetLastAddedIndirectSurfaceSampleEnergy();
			const Scalar positiveSampleDensity =
				cameraSamples > NEARZERO ?
					static_cast<Scalar>( positiveSamples ) / cameraSamples :
					0.0;
			const Scalar indirectEnergyDensity =
				cameraSamples > NEARZERO ?
					indirectSampleEnergy / cameraSamples :
					0.0;
			const Scalar indirectEnergyFraction =
				totalSampleEnergy > NEARZERO ?
					indirectSampleEnergy / totalSampleEnergy :
					0.0;
			const Scalar energyScale =
				totalSampleEnergy > NEARZERO ?
					std::sqrt( std::min( Scalar( 1.0 ), indirectEnergyFraction ) ) :
					0.0;

			if( positiveSampleDensity < 1.0 )
			{
				const Scalar densityScale =
					positiveSampleDensity < 0.5 ?
						0.0 :
						positiveSampleDensity * positiveSampleDensity;
				guidingAlphaScale = densityScale * energyScale;

				GlobalLog()->PrintEx( eLog_Event,
					"PathGuidingField:: PT alpha scaled to %.3f (positive sample density %.3f, indirect energy density %.6f, indirect fraction %.3f)",
					guidingAlphaScale, positiveSampleDensity, indirectEnergyDensity, indirectEnergyFraction );
			}
			else
			{
				guidingAlphaScale = energyScale;
			}

			if( positiveSampleDensity < 0.75 )
			{
				GlobalLog()->PrintEx( eLog_Event,
					"PathGuidingField:: Stopping PT training after iteration %u (%zu positive samples, density %.3f)",
					trainIter + 1,
					positiveSamples,
					positiveSampleDensity );
				break;
			}

			if( previousPositiveSampleDensity > NEARZERO ||
				previousIndirectEnergyDensity > NEARZERO )
			{
				const Scalar relativeGain =
					previousPositiveSampleDensity > NEARZERO ?
						positiveSampleDensity / previousPositiveSampleDensity - 1.0 :
						0.0;
				const Scalar relativeEnergyGain =
					previousIndirectEnergyDensity > NEARZERO ?
						indirectEnergyDensity / previousIndirectEnergyDensity - 1.0 :
						0.0;

				if( relativeGain < 0.08 && relativeEnergyGain < 0.02 ) {
					lowGainPasses++;
				} else {
					lowGainPasses = 0;
				}

				if( trainIter >= 3 && lowGainPasses >= 2 )
				{
					GlobalLog()->PrintEx( eLog_Event,
						"PathGuidingField:: Stopping PT training after iteration %u (relative gain %.3f, energy gain %.3f, %u low-gain passes)",
						trainIter + 1,
						relativeGain,
						relativeEnergyGain,
						lowGainPasses );
					break;
				}
			}

			previousPositiveSampleDensity = positiveSampleDensity;
			previousIndirectEnergyDensity = indirectEnergyDensity;

			if( positiveSampleDensity >= 1.5 )
			{
				currentTrainingSPP = guidingConfig.trainingSPP;
			}
			else if( positiveSampleDensity >= 1.0 )
			{
				currentTrainingSPP = std::min( guidingConfig.trainingSPP, bootstrapTrainingSPP + 1 );
			}
			else
			{
				currentTrainingSPP = bootstrapTrainingSPP;
			}
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
