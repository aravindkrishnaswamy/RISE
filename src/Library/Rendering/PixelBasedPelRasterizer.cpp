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
	const PathGuidingConfig& guidingCfg,
	const AdaptiveSamplingConfig& adaptiveCfg,
	const StabilityConfig& stabilityCfg
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
#ifdef RISE_ENABLE_OPENPGL
  pGuidingField( 0 ),
  guidingAlphaScale( 1.0 ),
#endif
  guidingConfig( guidingCfg ),
  adaptiveConfig( adaptiveCfg ),
  stabilityConfig( stabilityCfg )
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
	rc.pStabilityConfig = &stabilityConfig;
#ifdef RISE_ENABLE_OPENPGL
	if( pGuidingField ) {
		rc.pGuidingField = pGuidingField;
		rc.guidingAlpha = guidingConfig.alpha * guidingAlphaScale;
		rc.maxGuidingDepth = guidingConfig.maxGuidingDepth;
		rc.guidingSamplingType = guidingConfig.samplingType;
		rc.guidingRISCandidates = guidingConfig.risCandidates;
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

			{
				BlockRasterizeSequence* pTrainBlocks = new BlockRasterizeSequence( 64, 64, 2 );
				pTrainBlocks->addref();
				RasterizeBlocksForPass(
					RuntimeContext::PASS_NORMAL,
					pScene,
					*pTrainImage,
					pRect,
					*pTrainBlocks );
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

			//
			// ADAPTIVE ALPHA SCALING — Variance-Aware (Rath 2020)
			//
			// The guiding alpha (probability of sampling from the learned
			// distribution) is scaled by two factors:
			//
			//   guidingAlphaScale = densityScale * energyScale
			//
			// 1. densityScale = positiveSampleDensity²
			//    Smooth ramp that approaches zero when few camera samples
			//    produce nonzero contributions.  This is the Cycles-style
			//    approach: no hard threshold that kills guiding globally.
			//    OpenPGL's InitDistribution() returns false for under-
			//    trained spatial cells, providing per-vertex safety.
			//
			// 2. energyScale = covScale * min(1, indirectFraction + covScale)
			//    where covScale = min(1, indirectCoV / kCoVThreshold).
			//    Inspired by Rath et al. 2020 ("Variance-Aware Path
			//    Guiding").  The coefficient of variation (CoV) of
			//    indirect sample energy measures how directionally
			//    concentrated the illumination is.  High CoV (>= 2.0)
			//    means the energy comes from a narrow set of directions
			//    where guiding is most valuable; low CoV means diffuse
			//    illumination where BSDF sampling already works well.
			//
			// ALTERNATIVE CONSIDERED: Cycles-style approach ("A+E") uses
			//   energyScale = sqrt(min(1, indirectEnergyFraction))
			// instead of CoV.  This is simpler but does not distinguish
			// between a scene with uniform indirect lighting (guiding
			// adds little) and one with concentrated caustic-like
			// indirect lighting (guiding very valuable).  Both methods
			// produce identical results when indirectFraction = 1.0 and
			// CoV >= 2.0.  To switch to A+E, replace the CoV block below
			// with: energyScale = sqrt(min(1, indirectEnergyFraction)).
			// To switch to A+E: replace the CoV block with energyScale = sqrt(min(1, indirectEnergyFraction)).
			//
			const Scalar indirectEnergySquaredSum =
				pGuidingField->GetLastAddedIndirectSurfaceSampleEnergySquaredSum();

			// Compute CoV of indirect energy.  We need at least 2 samples
			// for a meaningful variance estimate.
			Scalar indirectCoV = 0;
			if( indirectSampleEnergy > NEARZERO && positiveSamples > 1 )
			{
				const Scalar n = static_cast<Scalar>( positiveSamples );
				const Scalar indirectMean = indirectSampleEnergy / n;
				const Scalar indirectVar =
					indirectEnergySquaredSum / n - indirectMean * indirectMean;
				if( indirectVar > 0 ) {
					indirectCoV = std::sqrt( indirectVar ) / indirectMean;
				}
			}

			const Scalar kCoVThreshold = 2.0;
			const Scalar covScale = std::min( Scalar( 1.0 ),
				indirectCoV / kCoVThreshold );
			const Scalar energyScale =
				totalSampleEnergy > NEARZERO ?
					covScale * std::min( Scalar( 1.0 ), indirectEnergyFraction + covScale ) :
					0.0;
			if( positiveSampleDensity < 1.0 )
			{
				const Scalar densityScale =
					positiveSampleDensity * positiveSampleDensity;
				guidingAlphaScale = densityScale * energyScale;

				GlobalLog()->PrintEx( eLog_Event,
					"PathGuidingField:: PT alpha scaled to %.3f (density %.3f, indirect fraction %.3f, indirect CoV %.3f, covScale %.3f, energyScale %.3f)",
					guidingAlphaScale, positiveSampleDensity, indirectEnergyFraction, indirectCoV, covScale, energyScale );
			}
			else
			{
				guidingAlphaScale = energyScale;

				GlobalLog()->PrintEx( eLog_Event,
					"PathGuidingField:: PT alpha scaled to %.3f (density %.3f, indirect fraction %.3f, indirect CoV %.3f, covScale %.3f, energyScale %.3f)",
					guidingAlphaScale, positiveSampleDensity, indirectEnergyFraction, indirectCoV, covScale, energyScale );
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

	// Derive a per-pixel seed for Owen scrambling from pixel coordinates
	const uint32_t pixelSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x),
		static_cast<uint32_t>(y) );

	if( pSampling && pPixelFilter && rc.pass == RuntimeContext::PASS_NORMAL )
	{
		const bool adaptive = adaptiveConfig.maxSamples > 0;
		const unsigned int batchSize = pSampling->GetNumSamples();
		const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;

		RISEPel		colAccrued( 0, 0, 0 );
		Scalar		weights = 0;
		Scalar		alphas = 0;

		// Welford online variance state (luminance-based)
		Scalar		wMean = 0;
		Scalar		wM2 = 0;
		unsigned int wN = 0;

		uint32_t globalSampleIndex = 0;
		bool converged = false;

		while( globalSampleIndex < maxSamples && !converged )
		{
			ISampling2D::SamplesList2D samples;
			pSampling->GenerateSamplePoints( rc.random, samples );

			ISampling2D::SamplesList2D::const_iterator m, n;
			for( m=samples.begin(), n=samples.end(); m!=n && globalSampleIndex<maxSamples; m++, globalSampleIndex++ )
			{
				RISEPel		c;
				Point2		ptOnScreen;
				const Scalar weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
				weights += weight;

				if( temporal_samples ) {
					pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
				}

				// Install a Sobol sampler for this pixel sample so that
				// shader ops (PathTracingShaderOp) can use low-discrepancy
				// sampling across the full path recursion.
				SobolSampler sobolSampler( globalSampleIndex, pixelSeed );
				rc.pSampler = &sobolSampler;

				Ray ray;
				if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
					if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
						colAccrued = colAccrued + c*weight;
						alphas += weight;

						// Welford update on luminance
						if( adaptive ) {
							const Scalar lum = ColorMath::MaxValue(c);
							wN++;
							const Scalar delta = lum - wMean;
							wMean += delta / Scalar(wN);
							const Scalar delta2 = lum - wMean;
							wM2 += delta * delta2;
						}
					} else if( adaptive ) {
						// Ray missed — count as zero-luminance sample for Welford
						wN++;
						const Scalar delta = -wMean;
						wMean += delta / Scalar(wN);
						const Scalar delta2 = -wMean;
						wM2 += delta * delta2;
					}
				} else if( adaptive ) {
					wN++;
					const Scalar delta = -wMean;
					wMean += delta / Scalar(wN);
					const Scalar delta2 = -wMean;
					wM2 += delta * delta2;
				}

				rc.pSampler = 0;
			}

			// Check convergence after enough batches for reliable statistics.
			// Require at least 4 batches so the variance estimate has some
			// stability (n >= 16 with typical batch sizes).
			if( adaptive && globalSampleIndex >= batchSize * 4 && wN >= 16 )
			{
				const Scalar variance = wM2 / Scalar(wN - 1);
				const Scalar stdError = sqrt( variance / Scalar(wN) );
				const Scalar meanAbs = fabs( wMean );

				if( meanAbs > NEARZERO ) {
					// Apply a small-sample confidence correction: scale the
					// threshold down when n is low so we are conservative
					// about early stopping.  The factor approaches 1.0 as
					// n grows and equals ~0.5 at n=16.
					const Scalar confidence = 1.0 - 4.0 / Scalar(wN);
					if( stdError / meanAbs < adaptiveConfig.threshold * confidence ) {
						converged = true;
					}
				} else if( wM2 < NEARZERO && wN >= batchSize * 8 ) {
					// Near-zero pixel: require many more samples before
					// declaring convergence, since rare bright contributions
					// (e.g. caustics, indirect light) might not have appeared
					// yet in a small sample set.
					converged = true;
				}
			}
		}

		if( adaptive && adaptiveConfig.showMap ) {
			const Scalar t = Scalar(globalSampleIndex) / Scalar(maxSamples);
			cret = RISEColor( RISEPel(t, t, t), 1.0 );
		} else {
			// Divide out by the number of samples
			colAccrued = colAccrued * (alphas>0?(1.0/alphas):0);
			cret = RISEColor(colAccrued, alphas/weights);
		}
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
