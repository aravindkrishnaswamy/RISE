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
#include "../Utilities/ZSobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Utilities/OptimalMISAccumulator.h"
#include "../Sampling/SobolSequence.h"
#include "../Lights/LightSampler.h"
#include "BlockRasterizeSequence.h"
#include "MortonRasterizeSequence.h"
#include "ProgressiveFilm.h"
#include "../RasterImages/RasterImage.h"
#include "../RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

#ifdef RISE_ENABLE_OPENPGL
#include "../Utilities/PathGuidingField.h"
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
	const StabilityConfig& stabilityCfg,
	bool useZSobol_
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
#ifdef RISE_ENABLE_OPENPGL
  pGuidingField( 0 ),
  guidingAlphaScale( 1.0 ),
#endif
  pOptimalMISAccumulator( 0 ),
  guidingConfig( guidingCfg ),
  adaptiveConfig( adaptiveCfg ),
  stabilityConfig( stabilityCfg )
{
	useZSobol = useZSobol_;
}

PixelBasedPelRasterizer::~PixelBasedPelRasterizer( )
{
#ifdef RISE_ENABLE_OPENPGL
	safe_release( pGuidingField );
#endif
	safe_release( pOptimalMISAccumulator );
}

unsigned int PixelBasedPelRasterizer::GetProgressiveTotalSPP() const
{
	if( adaptiveConfig.maxSamples > 0 ) {
		return adaptiveConfig.maxSamples;
	}

	return PixelBasedRasterizerHelper::GetProgressiveTotalSPP();
}

void PixelBasedPelRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	PixelBasedRasterizerHelper::PrepareRuntimeContext( rc );
	rc.pStabilityConfig = &stabilityConfig;
	rc.pOptimalMIS = pOptimalMISAccumulator;
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
				MortonRasterizeSequence* pTrainBlocks = new MortonRasterizeSequence( 32 );
				RasterizeBlocksForPass(
					RuntimeContext::PASS_PATHGUIDING,
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

				GlobalLog()->PrintEx( eLog_Info,
					"PathGuidingField:: PT alpha scaled to %.3f (density %.3f, indirect fraction %.3f, indirect CoV %.3f, covScale %.3f, energyScale %.3f)",
					guidingAlphaScale, positiveSampleDensity, indirectEnergyFraction, indirectCoV, covScale, energyScale );
			}
			else
			{
				guidingAlphaScale = energyScale;

				GlobalLog()->PrintEx( eLog_Info,
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
					GlobalLog()->PrintEx( eLog_Info,
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

	//
	// OPTIMAL MIS TRAINING (Kondapaneni et al. 2019)
	//
	// When enabled, run low-SPP training iterations to estimate
	// second-moment statistics for NEE and BSDF sampling techniques.
	// The PathTracingShaderOp accumulates f^2/p_nee and f^2/p_bsdf
	// into the accumulator via RuntimeContext::pOptimalMIS during
	// training (when the accumulator is not yet solved).
	// After training, Solve() computes per-tile optimal alpha and
	// all subsequent rendering uses variance-minimizing MIS weights.
	//
	safe_release( pOptimalMISAccumulator );

	if( stabilityConfig.optimalMIS )
	{
		const unsigned int width = pScene.GetCamera()->GetWidth();
		const unsigned int height = pScene.GetCamera()->GetHeight();

		OptimalMISAccumulator::Config accConfig;
		accConfig.tileSize = stabilityConfig.optimalMISTileSize;
		accConfig.minSamplesPerTile = 32;
		accConfig.alphaClampMin = 0.05;
		accConfig.alphaClampMax = 0.95;

		pOptimalMISAccumulator = new OptimalMISAccumulator();
		pOptimalMISAccumulator->Initialize( width, height, accConfig );

		// Make the unsolved accumulator visible to the shading pipeline
		// so that PathTracingShaderOp can accumulate second-moment data
		// during the training render passes.
		const LightSampler* pLS = pCaster->GetLightSampler();
		if( pLS ) {
			pLS->SetOptimalMIS( pOptimalMISAccumulator );
		}

		GlobalLog()->PrintEx( eLog_Event,
			"OptimalMIS:: Starting training (%u iterations, tile size %u)",
			stabilityConfig.optimalMISTrainingIterations,
			stabilityConfig.optimalMISTileSize );

		// Reset once before training — accumulate across all iterations
		// so that every pass adds to the statistics.
		pOptimalMISAccumulator->Reset();

		for( unsigned int trainIter = 0;
			 trainIter < stabilityConfig.optimalMISTrainingIterations;
			 trainIter++ )
		{

			// Use 1 SPP for training — enough to populate tiles
			ISampling2D* pTrainSampling = 0;
			RISE_API_CreateUniformSampling2D( &pTrainSampling, 1.0, 1.0 );
			if( pTrainSampling ) {
				pTrainSampling->SetNumSamples( 1 );
			}

			ISampling2D* pSavedSampling = pSampling;
			const_cast<PixelBasedPelRasterizer*>(this)->pSampling = pTrainSampling;

			IRasterImage* pTrainImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );

			if( pProgressFunc ) {
				char title[128];
				snprintf( title, sizeof(title), "Optimal MIS Training [%u/%u]: ",
					trainIter+1, stabilityConfig.optimalMISTrainingIterations );
				pProgressFunc->SetTitle( title );
			}

			{
				MortonRasterizeSequence* pTrainBlocks = new MortonRasterizeSequence( 32 );
				RasterizeBlocksForPass(
					RuntimeContext::PASS_NORMAL,
					pScene,
					*pTrainImage,
					pRect,
					*pTrainBlocks );
				safe_release( pTrainBlocks );
			}

			const_cast<PixelBasedPelRasterizer*>(this)->pSampling = pSavedSampling;
			safe_release( pTrainSampling );
			safe_release( pTrainImage );
		}

		pOptimalMISAccumulator->Solve();

		if( pProgressFunc ) {
			pProgressFunc->SetTitle( "Rasterizing Scene: " );
		}

		GlobalLog()->PrintEx( eLog_Event,
			"OptimalMIS:: Training complete, accumulator solved" );
	}
}

void PixelBasedPelRasterizer::PostRenderCleanup() const
{
#ifdef RISE_ENABLE_OPENPGL
	safe_release( pGuidingField );
#endif

	// Clear the optimal MIS accumulator from the light sampler
	if( pOptimalMISAccumulator ) {
		const LightSampler* pLS = pCaster->GetLightSampler();
		if( pLS ) {
			pLS->SetOptimalMIS( 0 );
		}
		safe_release( pOptimalMISAccumulator );
	}
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

	// Progressive rendering: check if this pixel has already converged
	ProgressiveFilm* pProgFilm = rc.pProgressiveFilm;
	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		if( px.converged ) {
			// Return cached result
			if( px.alphaSum > 0 ) {
				cret = RISEColor( px.colorSum * (1.0/px.alphaSum), px.alphaSum / px.weightSum );
			}
			return;
		}
	}

	// Derive a per-pixel seed for Owen scrambling.
	// Standard Sobol: seed from pixel coordinates.
	// ZSobol (blue-noise): seed from Morton index for spatially
	// correlated scrambles across neighboring pixels.
	uint32_t pixelSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	if( pSampling && pPixelFilter && rc.UsesPixelSampling() )
	{
		const bool adaptive = adaptiveConfig.maxSamples > 0 && rc.AllowsAdaptiveSampling();
		const unsigned int batchSize = pSampling->GetNumSamples();
		const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;

		// For ZSobol, compute log2SPP from the full progressive SPP budget
		// so the blue-noise distribution spans the entire render.
		const unsigned int zSobolSPP = rc.totalProgressiveSPP > 0 ? rc.totalProgressiveSPP : maxSamples;

		if( useZSobol &&
			MortonCode::CanEncode2D( static_cast<uint32_t>(x), static_cast<uint32_t>(y) ) )
		{
			const uint32_t mi = MortonCode::Morton2D(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			const uint32_t l2 = MortonCode::Log2Int( MortonCode::RoundUpPow2( zSobolSPP ) );
			if( l2 < 32 &&
				(uint64_t(mi) << l2) < (uint64_t(1) << 32) )
			{
				mortonIndex = mi;
				log2SPP = l2;
				pixelSeed = SobolSequence::HashCombine( mortonIndex, 0u );
			} else {
				pixelSeed = SobolSequence::HashCombine(
					static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			}
		}

		// In progressive mode, restore persistent state from the film.
		// In single-pass mode, start fresh.
		RISEPel		colAccrued( 0, 0, 0 );
		Scalar		weights = 0;
		Scalar		alphas = 0;
		Scalar		wMean = 0;
		Scalar		wM2 = 0;
		unsigned int wN = 0;
		uint32_t globalSampleIndex = 0;
		bool converged = false;

		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			colAccrued = px.colorSum;
			weights = px.weightSum;
			alphas = px.alphaSum;
			wMean = px.wMean;
			wM2 = px.wM2;
			wN = px.wN;
			globalSampleIndex = px.sampleIndex;
		}

		const uint32_t targetSamples = pProgFilm && rc.totalProgressiveSPP > 0
			? rc.totalProgressiveSPP
			: maxSamples;
		uint32_t passEndIndex = targetSamples;
		if( pProgFilm ) {
			const uint64_t desiredEnd = static_cast<uint64_t>( globalSampleIndex ) + static_cast<uint64_t>( batchSize );
			passEndIndex = desiredEnd < targetSamples ? static_cast<uint32_t>( desiredEnd ) : targetSamples;
		}

		while( globalSampleIndex < passEndIndex && !converged )
		{
			ISampling2D::SamplesList2D samples;
			pSampling->GenerateSamplePoints( rc.random, samples );

			ISampling2D::SamplesList2D::const_iterator m, n;
			for( m=samples.begin(), n=samples.end(); m!=n && globalSampleIndex<passEndIndex; m++, globalSampleIndex++ )
			{
				RISEPel		c( 0, 0, 0 );
				Point2		ptOnScreen;

				// In film mode, use box sampling and splat to the film.
				// Otherwise, use the pixel filter's warp for inline accumulation.
				const bool filmMode = (pFilteredFilm != 0);
				Scalar weight;
				if( filmMode ) {
					ptOnScreen = Point2(
						static_cast<Scalar>(x) + (*m).x - 0.5,
						static_cast<Scalar>(height-y) + (*m).y - 0.5 );
					weight = 1.0;
					weights += 1.0;
				} else {
					weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
					weights += weight;
				}

				if( temporal_samples ) {
					pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
				}

				// Install a Sobol sampler for this pixel sample so that
				// shader ops (PathTracingShaderOp) can use low-discrepancy
				// sampling across the full path recursion.
				// ZSobol remaps the sample index via the pixel's Morton code
				// to produce blue-noise error distribution across screen space.
				SobolSampler stdSampler( globalSampleIndex, pixelSeed );
				ZSobolSampler zSampler( globalSampleIndex, mortonIndex, log2SPP, pixelSeed );
				rc.pSampler = useZSobol
					? static_cast<ISampler*>(&zSampler)
					: static_cast<ISampler*>(&stdSampler);

				Ray ray;
				if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
					bool bHit = pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 );

					if( filmMode ) {
						// Always splat to the film, even on miss (c stays zero).
						// Miss-samples must contribute zero radiance with proper
						// filter weight so that edge pixels blend correctly.
						pFilteredFilm->Splat( ptOnScreen.x, static_cast<Scalar>(height) - ptOnScreen.y, c, *pPixelFilter );
						if( bHit ) {
							colAccrued = colAccrued + c;
							alphas += 1.0;
						}
					} else if( bHit ) {
						colAccrued = colAccrued + c*weight;
						alphas += weight;
					}

					// Welford update on luminance (always in progressive mode,
					// or when adaptive is enabled in single-pass mode)
					if( adaptive || pProgFilm ) {
						const Scalar lum = bHit ? ColorMath::MaxValue(c) : 0;
						wN++;
						const Scalar delta = lum - wMean;
						wMean += delta / Scalar(wN);
						const Scalar delta2 = lum - wMean;
						wM2 += delta * delta2;
					}
				} else if( adaptive || pProgFilm ) {
					wN++;
					const Scalar delta = -wMean;
					wMean += delta / Scalar(wN);
					const Scalar delta2 = -wMean;
					wM2 += delta * delta2;
				}

				rc.pSampler = 0;
			}

			// Check convergence after enough cumulative samples for reliable
			// statistics.  In progressive mode, wN accumulates across all
			// passes, so convergence detection kicks in once enough total
			// samples have been gathered (typically pass 4+ at 8 SPP/pass).
			if( (adaptive || pProgFilm) && wN >= 32 )
			{
				const Scalar variance = wM2 / Scalar(wN - 1);
				const Scalar stdError = sqrt( variance / Scalar(wN) );
				const Scalar meanAbs = fabs( wMean );

				if( meanAbs > NEARZERO ) {
					const Scalar confidence = 1.0 - 4.0 / Scalar(wN);
					const Scalar threshold = adaptiveConfig.maxSamples > 0
						? adaptiveConfig.threshold
						: 0.01;		// Default threshold for progressive-only mode
					if( stdError / meanAbs < threshold * confidence ) {
						converged = true;
					}
				} else if( wM2 < NEARZERO && wN >= 64 ) {
					converged = true;
				}
			}
		}

		// Write back persistent state for progressive rendering
		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			px.colorSum = colAccrued;
			px.weightSum = weights;
			px.alphaSum = alphas;
			px.wMean = wMean;
			px.wM2 = wM2;
			px.wN = wN;
			px.sampleIndex = globalSampleIndex;
			px.converged = converged;
		}

		if( adaptive && adaptiveConfig.showMap ) {
			const Scalar t = Scalar(globalSampleIndex) / Scalar(targetSamples);
			cret = RISEColor( RISEPel(t, t, t), 1.0 );
		} else {
			// Divide out by the number of samples
			colAccrued = colAccrued * (alphas>0?(1.0/alphas):0);
			cret = RISEColor(colAccrued, alphas/weights);
		}
	}
	else
	{
		if( useZSobol &&
			MortonCode::CanEncode2D( static_cast<uint32_t>(x), static_cast<uint32_t>(y) ) )
		{
			mortonIndex = MortonCode::Morton2D(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			pixelSeed = SobolSequence::HashCombine( mortonIndex, 0u );
			// log2SPP stays 0 for single sample
		}
		SobolSampler stdSampler( 0, pixelSeed );
		ZSobolSampler zSampler( 0, mortonIndex, 0, pixelSeed );
		rc.pSampler = useZSobol
			? static_cast<ISampler*>(&zSampler)
			: static_cast<ISampler*>(&stdSampler);

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
