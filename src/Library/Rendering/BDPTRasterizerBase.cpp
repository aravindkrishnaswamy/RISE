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
#include "MortonRasterizeSequence.h"
#include "RasterizeDispatchers.h"
#include "../Utilities/Profiling.h"
#include "AOVBuffers.h"
#include "OIDNDenoiser.h"
#include "ProgressiveFilm.h"
#include "../RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

#ifdef RISE_ENABLE_OPENPGL
namespace
{
	inline void CopyRasterImage(
		IRasterImage& dest,
		const IRasterImage& src
		)
	{
		const unsigned int w = src.GetWidth();
		const unsigned int h = src.GetHeight();
		for( unsigned int y = 0; y < h; y++ ) {
			for( unsigned int x = 0; x < w; x++ ) {
				dest.SetPEL( x, y, src.GetPEL( x, y ) );
			}
		}
	}

	inline Scalar ComputeCoarseImageDeltaRMSE(
		const IRasterImage& a,
		const IRasterImage& b,
		const unsigned int coarseResolution
		)
	{
		const unsigned int w = a.GetWidth();
		const unsigned int h = a.GetHeight();
		if( w == 0 || h == 0 ) {
			return 0;
		}

		const unsigned int binsX = r_min( coarseResolution, w );
		const unsigned int binsY = r_min( coarseResolution, h );

		Scalar error = 0;
		size_t samples = 0;
		for( unsigned int by = 0; by < binsY; by++ )
		{
			const unsigned int y0 = (by * h) / binsY;
			const unsigned int y1 = ((by + 1) * h) / binsY;

			for( unsigned int bx = 0; bx < binsX; bx++ )
			{
				const unsigned int x0 = (bx * w) / binsX;
				const unsigned int x1 = ((bx + 1) * w) / binsX;

				Scalar accumA[3] = { 0, 0, 0 };
				Scalar accumB[3] = { 0, 0, 0 };
				size_t pixelCount = 0;

				for( unsigned int y = y0; y < y1; y++ ) {
					for( unsigned int x = x0; x < x1; x++ ) {
						const RISEColor colorA = a.GetPEL( x, y );
						const RISEColor colorB = b.GetPEL( x, y );
						accumA[0] += colorA.base[0];
						accumA[1] += colorA.base[1];
						accumA[2] += colorA.base[2];
						accumB[0] += colorB.base[0];
						accumB[1] += colorB.base[1];
						accumB[2] += colorB.base[2];
						pixelCount++;
					}
				}

				if( pixelCount == 0 ) {
					continue;
				}

				for( unsigned int c = 0; c < 3; c++ )
				{
					const Scalar avgA = accumA[c] / static_cast<Scalar>( pixelCount );
					const Scalar avgB = accumB[c] / static_cast<Scalar>( pixelCount );
					const Scalar diff = avgA - avgB;
					error += diff * diff;
					samples++;
				}
			}
		}

		return samples > 0 ?
			std::sqrt( error / static_cast<Scalar>( samples ) ) :
			0;
	}

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

BDPTRasterizerBase::BDPTRasterizerBase(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth,
	const ManifoldSolverConfig& smsConfig,
	const PathGuidingConfig& guidingCfg,
	const StabilityConfig& stabilityCfg
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  pIntegrator( 0 ),
  pManifoldSolver( 0 ),
  pSplatFilm( 0 ),
  pScratchImage( 0 ),
  mSplatTotalSamples( 1.0 ),
  mTotalAdaptiveSamples( 0 )
#ifdef RISE_ENABLE_OPENPGL
  ,pGuidingField( 0 )
  ,pLightGuidingField( 0 )
  ,pCompletePathGuide( 0 )
  ,guidingAlphaScale( 1.0 )
#endif
  ,guidingConfig( guidingCfg )
  ,stabilityConfig( stabilityCfg )
{
	pIntegrator = new BDPTIntegrator( maxEyeDepth, maxLightDepth, stabilityCfg );

	if( smsConfig.enabled )
	{
		pManifoldSolver = new ManifoldSolver( smsConfig );
		pIntegrator->SetManifoldSolver( pManifoldSolver );
	}
}

BDPTRasterizerBase::~BDPTRasterizerBase()
{
	safe_release( pIntegrator );
	safe_release( pManifoldSolver );
	safe_release( pSplatFilm );
	safe_release( pScratchImage );
#ifdef RISE_ENABLE_OPENPGL
	safe_release( pGuidingField );
	safe_release( pLightGuidingField );
	safe_release( pCompletePathGuide );
#endif
}

void BDPTRasterizerBase::AddAdaptiveSamples( uint64_t count ) const
{
	mTotalAdaptiveSamples.fetch_add( count, std::memory_order_relaxed );
}

Scalar BDPTRasterizerBase::GetEffectiveSplatSPP( unsigned int width, unsigned int height ) const
{
	const uint64_t totalSamples = mTotalAdaptiveSamples.load( std::memory_order_relaxed );
	if( totalSamples > 0 && width > 0 && height > 0 ) {
		const Scalar avgSPP = static_cast<Scalar>(totalSamples) / static_cast<Scalar>(width * height);
		return avgSPP * GetSplatSampleScale();
	}
	return mSplatTotalSamples;
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

	// Acquire scene dimensions
	const unsigned int width = pScene.GetCamera()->GetWidth();
	const unsigned int height = pScene.GetCamera()->GetHeight();

	// Training can cast probe rays to estimate incident radiance,
	// so the ray caster needs the scene attached before training starts.
	// AttachScene also creates and Prepare()s the unified LightSampler.
	pCaster->AttachScene( &pScene );
	pScene.GetObjects()->PrepareForRendering();

	// Share the RayCaster's prepared LightSampler with the integrator
	const LightSampler* pLS = pCaster->GetLightSampler();
	pIntegrator->SetLightSampler( pLS );

	// Create the splat film for s<=1 strategies
	safe_release( pSplatFilm );
	pSplatFilm = new SplatFilm( width, height );

	// Reset adaptive sample counter for this render
	mTotalAdaptiveSamples.store( 0, std::memory_order_relaxed );

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

#ifdef RISE_ENABLE_OPENPGL
	// Path guiding: training phase
	safe_release( pGuidingField );
	safe_release( pLightGuidingField );
	safe_release( pCompletePathGuide );
	pIntegrator->SetCompletePathGuide( 0, false, 0 );
	guidingAlphaScale = 1.0;
	if( guidingConfig.enabled )
	{
		Point3 boundsMin;
		Point3 boundsMax;
		ComputeGuidingSceneBounds( pScene, boundsMin, boundsMax );

		pGuidingField = new PathGuidingField( guidingConfig, boundsMin, boundsMax );

		// Separate field for light subpath guiding (Option B).
		// Avoids conflicting eye/light distributions in the same
		// spatial cells (e.g. floor positions where eye training
		// learns "look up toward ceiling light" but light training
		// learns "scatter through glass sphere").
		if( guidingConfig.maxLightGuidingDepth > 0 )
		{
			pLightGuidingField = new PathGuidingField( guidingConfig, boundsMin, boundsMax );
		}

		if( guidingConfig.completePathGuiding )
		{
			pCompletePathGuide = new CompletePathGuide(
				boundsMin,
				boundsMax,
				pIntegrator->GetMaxLightDepth(),
				pIntegrator->GetMaxEyeDepth() );

			GlobalLog()->PrintEx( eLog_Event,
				"CompletePathGuide:: Enabled for BDPT training (maxLightDepth=%u, maxEyeDepth=%u)",
				pIntegrator->GetMaxLightDepth(),
				pIntegrator->GetMaxEyeDepth() );
		}

		// Set the guiding field on the integrator (training mode: collect samples, no guiding)
		pIntegrator->SetGuidingField(
			pGuidingField,
			pLightGuidingField,
			guidingConfig.alpha * guidingAlphaScale,
			guidingConfig.maxGuidingDepth,
			guidingConfig.maxLightGuidingDepth,
			guidingConfig.samplingType,
			guidingConfig.risCandidates );
		pIntegrator->SetCompletePathGuide( pCompletePathGuide, false, 0 );

				const unsigned int bootstrapTrainingSPP = 1;
				unsigned int currentTrainingSPP = bootstrapTrainingSPP;
				Scalar previousPositiveSampleDensity = 0;
				Scalar previousIndirectEnergyDensity = 0;
				unsigned int lowGainPasses = 0;
				IRasterImage* pPreviousTrainingImage = 0;

				static const unsigned int kTrainingConvergenceGridResolution = 32;
				static const Scalar kTrainingConvergenceDeltaThreshold = 0.01;
				static const Scalar kTrainingConvergenceSampleGainThreshold = 0.05;
				static const Scalar kStrategySelectionMinTopBucketShare = 0.05;
				static const Scalar kStrategySelectionMinTopTechniqueShare = 0.85;

		// Run training iterations
			for( unsigned int trainIter = 0; trainIter < guidingConfig.trainingIterations; trainIter++ )
			{
				pIntegrator->ResetGuidingTrainingStats();
				pGuidingField->BeginTrainingIteration();
				if( pLightGuidingField ) {
					pLightGuidingField->BeginTrainingIteration();
				}
				if( pCompletePathGuide ) {
					pCompletePathGuide->BeginIteration();
				}

			// Use a temporary low-spp sampling object for training
			ISampling2D* pTrainSampling = 0;
			RISE_API_CreateUniformSampling2D( &pTrainSampling, 1.0, 1.0 );
			if( pTrainSampling ) {
				pTrainSampling->SetNumSamples( currentTrainingSPP );
			}

			ISampling2D* pSavedSampling = pSampling;
			const_cast<BDPTRasterizerBase*>(this)->pSampling = pTrainSampling;

			// Create a temporary image for training (results are discarded)
			IRasterImage* pTrainImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );

			// Create a temporary splat film for training
			SplatFilm* pTrainSplat = new SplatFilm( width, height );
			SplatFilm* pSavedSplat = pSplatFilm;
			pSplatFilm = pTrainSplat;
			mSplatTotalSamples = static_cast<Scalar>( currentTrainingSPP ) * GetSplatSampleScale();

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

				pTrainSplat->Resolve( *pTrainImage, mSplatTotalSamples );

				Scalar coarseImageDelta = 0;
				if( pPreviousTrainingImage )
				{
					coarseImageDelta = ComputeCoarseImageDeltaRMSE(
						*pPreviousTrainingImage,
						*pTrainImage,
						kTrainingConvergenceGridResolution );

					GlobalLog()->PrintEx( eLog_Event,
						"PathGuidingField:: BDPT training iteration %u coarse image delta %.6f (%ux%u RMSE)",
						trainIter + 1,
						coarseImageDelta,
						kTrainingConvergenceGridResolution,
						kTrainingConvergenceGridResolution );
				}
				else
				{
					pPreviousTrainingImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
				}

				if( pPreviousTrainingImage ) {
					CopyRasterImage( *pPreviousTrainingImage, *pTrainImage );
				}

			// Restore state
			pSplatFilm = pSavedSplat;
			safe_release( pTrainSplat );
			safe_release( pTrainImage );
			const_cast<BDPTRasterizerBase*>(this)->pSampling = pSavedSampling;
			safe_release( pTrainSampling );

			pGuidingField->EndTrainingIteration();
			if( pLightGuidingField ) {
				pLightGuidingField->EndTrainingIteration();
			}
			if( pCompletePathGuide ) {
				pCompletePathGuide->EndIteration();
			}

			const Scalar cameraSamples =
				static_cast<Scalar>( width ) *
				static_cast<Scalar>( height ) *
				static_cast<Scalar>( currentTrainingSPP ) *
				GetSplatSampleScale();
				const size_t positiveSamples = pGuidingField->GetLastAddedSurfaceSampleCount();
				const Scalar totalSampleEnergy = pGuidingField->GetLastAddedSurfaceSampleEnergy();
				const Scalar indirectSampleEnergy =
					pGuidingField->GetLastAddedIndirectSurfaceSampleEnergy();
				const Scalar positiveSampleDensity =
					cameraSamples > NEARZERO ?
						static_cast<Scalar>( positiveSamples ) / cameraSamples :
						0.0;
				const BDPTIntegrator::GuidingTrainingStats& guidingStats =
					pIntegrator->GetGuidingTrainingStats();
				const Scalar indirectEnergyDensity =
					cameraSamples > NEARZERO ?
						indirectSampleEnergy / cameraSamples :
						0.0;
				const Scalar deepEyeEnergyFraction =
					guidingStats.totalEnergy > NEARZERO ?
						guidingStats.deepEyeConnectionEnergy / guidingStats.totalEnergy :
						0.0;
				const Scalar indirectEnergyFraction =
					totalSampleEnergy > NEARZERO ?
						indirectSampleEnergy / totalSampleEnergy :
						0.0;

				//
				// ADAPTIVE ALPHA SCALING — Variance-Aware (Rath 2020)
				//
				// Same approach as the PT rasterizer (see
				// PixelBasedPelRasterizer.cpp for the full rationale).
				// BDPT additionally factors in deepEyeEnergyFraction
				// because guiding only affects eye subpath bounces,
				// so it is most valuable when deep eye connections
				// (t >= 3) carry significant energy.
				//
				// To switch to the simpler Cycles-style approach,
				// replace the CoV block with:
				//   strategyScale = sqrt(min(1, deepEyeFrac * indirectFrac))
				// See inline comment in PixelBasedPelRasterizer.cpp for the A+E alternative.
				//
				const Scalar indirectEnergySquaredSum =
					pGuidingField->GetLastAddedIndirectSurfaceSampleEnergySquaredSum();
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
				// For BDPT, include deep-eye energy fraction as well:
				// guiding only helps eye subpath bounces, so it's
				// most valuable when deep connections carry energy.
				const Scalar strategyScale =
					totalSampleEnergy > NEARZERO ?
						covScale * std::min( Scalar( 1.0 ),
							deepEyeEnergyFraction * indirectEnergyFraction + covScale ) :
						0.0;
				if( positiveSampleDensity < 1.0 )
				{
					const Scalar densityScale =
						positiveSampleDensity * positiveSampleDensity;
					guidingAlphaScale = densityScale * strategyScale;

					GlobalLog()->PrintEx( eLog_Event,
						"PathGuidingField:: BDPT alpha scaled to %.3f (density %.3f, indirect fraction %.3f, indirect CoV %.3f, covScale %.3f, deep fraction %.3f, strategyScale %.3f)",
						guidingAlphaScale,
						positiveSampleDensity,
						indirectEnergyFraction,
						indirectCoV,
						covScale,
						deepEyeEnergyFraction,
						strategyScale );
				}
				else
				{
					guidingAlphaScale = strategyScale;

					GlobalLog()->PrintEx( eLog_Event,
						"PathGuidingField:: BDPT alpha scaled to %.3f (density %.3f, indirect fraction %.3f, indirect CoV %.3f, covScale %.3f, deep fraction %.3f, strategyScale %.3f)",
						guidingAlphaScale,
						positiveSampleDensity,
						indirectEnergyFraction,
						indirectCoV,
						covScale,
						deepEyeEnergyFraction,
						strategyScale );
				}

			pIntegrator->SetGuidingField(
				pGuidingField,
				pLightGuidingField,
				guidingConfig.alpha * guidingAlphaScale,
				guidingConfig.maxGuidingDepth,
				guidingConfig.maxLightGuidingDepth,
				guidingConfig.samplingType,
				guidingConfig.risCandidates );

				if( trainIter == 0 && positiveSampleDensity >= 0.85 &&
					indirectEnergyFraction < 0.35 )
				{
					GlobalLog()->PrintEx( eLog_Event,
						"PathGuidingField:: Stopping BDPT training after bootstrap (density %.3f is already high enough for the coarse field and indirect fraction %.3f is low)",
						positiveSampleDensity,
						indirectEnergyFraction );
					break;
				}

			// Cycles-style: do NOT abort training for low density.
			// Let all configured iterations run; OpenPGL accumulates
			// data across passes and InitDistribution() handles
			// per-vertex safety for under-trained cells.

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
							"PathGuidingField:: Stopping BDPT training after iteration %u (relative gain %.3f, energy gain %.3f, %u low-gain passes)",
							trainIter + 1,
							relativeGain,
							relativeEnergyGain,
							lowGainPasses );
						break;
					}

					if( coarseImageDelta > NEARZERO &&
						trainIter >= 2 &&
						coarseImageDelta < kTrainingConvergenceDeltaThreshold &&
						relativeGain < kTrainingConvergenceSampleGainThreshold )
					{
						GlobalLog()->PrintEx( eLog_Event,
							"PathGuidingField:: Stopping BDPT training after iteration %u (coarse image delta %.6f, relative gain %.3f)",
							trainIter + 1,
							coarseImageDelta,
							relativeGain );
						break;
					}
				}

				previousPositiveSampleDensity = positiveSampleDensity;
				previousIndirectEnergyDensity = indirectEnergyDensity;

			if( positiveSampleDensity >= 1.0 )
			{
				currentTrainingSPP = guidingConfig.trainingSPP;
			}
			else if( positiveSampleDensity >= 0.6 )
			{
				currentTrainingSPP = std::min( guidingConfig.trainingSPP, bootstrapTrainingSPP + 1 );
			}
			else
			{
				currentTrainingSPP = bootstrapTrainingSPP;
			}
		}

		safe_release( pPreviousTrainingImage );

		pIntegrator->SetGuidingField(
			pGuidingField,
			pLightGuidingField,
			guidingConfig.alpha * guidingAlphaScale,
			guidingConfig.maxGuidingDepth,
			guidingConfig.maxLightGuidingDepth,
			guidingConfig.samplingType,
			guidingConfig.risCandidates );
		bool enableCompletePathStrategySelection =
			pCompletePathGuide &&
			guidingConfig.completePathStrategySelection;
		if( enableCompletePathStrategySelection )
		{
			const CompletePathGuide::IterationSummary& summary =
				pCompletePathGuide->GetLastSummary();
			const Scalar topTechniqueEnergyShare =
				summary.totalEnergy > NEARZERO ?
					summary.topTechniqueEnergy / summary.totalEnergy :
					0;

			if( summary.topBucketEnergyShare < kStrategySelectionMinTopBucketShare &&
				topTechniqueEnergyShare < kStrategySelectionMinTopTechniqueShare )
			{
				enableCompletePathStrategySelection = false;
				GlobalLog()->PrintEx( eLog_Event,
					"CompletePathGuide:: Final render strategy selection disabled (top bucket share %.3f, top technique share %.3f indicate a broad learned distribution)",
					summary.topBucketEnergyShare,
					topTechniqueEnergyShare );
			}
		}
		if( enableCompletePathStrategySelection )
		{
			pIntegrator->SetCompletePathGuide(
				pCompletePathGuide,
				true,
				r_max( static_cast<unsigned int>( 1 ),
					guidingConfig.completePathStrategySamples ) );

			GlobalLog()->PrintEx( eLog_Event,
				"CompletePathGuide:: Final render strategy selection enabled (%u techniques per path, Sobol stream 47)",
				r_max( static_cast<unsigned int>( 1 ),
					guidingConfig.completePathStrategySamples ) );
		}
		else
		{
			pIntegrator->SetCompletePathGuide( 0, false, 0 );
		}

		// Restore splat total samples for main render
		mSplatTotalSamples = 1.0;
		if( pSampling ) {
			mSplatTotalSamples = static_cast<Scalar>( pSampling->GetNumSamples() );
		}
		mSplatTotalSamples *= GetSplatSampleScale();

		GlobalLog()->PrintEx( eLog_Event,
			"PathGuidingField:: Training phase complete" );
	}
#endif

	// Create the primary image and a scratch copy for progressive output
	IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );
	GlobalLog()->PrintNew( pImage, __FILE__, __LINE__, "image" );

	safe_release( pScratchImage );
	pScratchImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 0 ) );

	{
		pImage->Clear( RISEColor( GlobalRNG().CanonicalRandom()*0.6+0.3, GlobalRNG().CanonicalRandom()*0.6+0.3, GlobalRNG().CanonicalRandom()*0.6+0.3, 1.0 ), pRect );

		RasterizerOutputListType::const_iterator r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputIntermediateImage( *pImage, pRect );
		}
	}

	// If there is no raster sequence, create a default one
	MortonRasterizeSequence* blocks = 0;
	if( !pRasterSequence ) {
		blocks = new MortonRasterizeSequence( 32 );
		pRasterSequence = blocks;
	}

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( GetProgressTitle() );
	}

#ifdef RISE_ENABLE_OPENPGL
	pIntegrator->ResetStrategySelectionStats();
#endif

	if( progressiveConfig.enabled && pSampling )
	{
		// Progressive multi-pass BDPT rendering.  SplatFilm accumulates
		// across all passes; intermediate preview composites splats via
		// GetIntermediateOutputImage.

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
			const_cast<BDPTRasterizerBase*>(this)->pSampling = pPassSampling;

			if( pProgressFunc ) {
				char title[128];
				snprintf( title, sizeof(title), "BDPT Progressive Pass [%u/%u]: ", passIdx+1, numPasses );
				pProgressFunc->SetTitle( title );
			}

			MortonRasterizeSequence* pPassSeq = new MortonRasterizeSequence( 32 );
			const bool passCompleted = RasterizeBlocksForPass( RuntimeContext::PASS_NORMAL, pScene, *pImage, pRect, *pPassSeq );
			safe_release( pPassSeq );

			const_cast<BDPTRasterizerBase*>(this)->pSampling = pSavedSampling;
			safe_release( pPassSampling );

			// Intermediate preview: rebuild the primary image from the
			// accumulated progressive state, then composite splats into a
			// scratch copy for display.
			progFilm.Resolve( *pImage );

			IRasterImage& outputImage = GetIntermediateOutputImage( *pImage );
			RasterizerOutputListType::const_iterator r, s;
			for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
				(*r)->OutputIntermediateImage( outputImage, pRect );
			}

			if( !passCompleted ) {
				GlobalLog()->PrintEx( eLog_Event,
					"BDPT Progressive:: Cancelled after pass %u/%u",
					passIdx+1, numPasses );
				break;
			}

			// Check whether every pixel has converged or reached the sample cap.
			const unsigned int doneCount = progFilm.CountDone( totalSPP );
			if( doneCount >= width * height ) {
				GlobalLog()->PrintEx( eLog_Event,
					"BDPT Progressive:: All pixels complete after pass %u/%u",
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
		// Single-pass render: dispatch blocks to threads
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
			PrepareRuntimeContext( rc );

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
	// When adaptive sampling is active, use the average SPP across all
	// pixels rather than the fixed per-pixel count.
	pSplatFilm->Resolve( *pImage, GetEffectiveSplatSPP( width, height ) );

	RISE_PROFILE_REPORT(GlobalLog());

#ifdef RISE_ENABLE_OIDN
	// Run OIDN denoiser as a post-process on the fully-resolved image
	if( pAOVBuffers && bDenoisingEnabled ) {
		OIDNDenoiser::ApplyDenoise( *pImage, *pAOVBuffers, width, height );
	}
#endif

#ifdef RISE_ENABLE_OPENPGL
	{
		unsigned long long pathCount = 0;
		unsigned long long candidateCount = 0;
		unsigned long long evaluatedCount = 0;
		pIntegrator->GetStrategySelectionStats(
			pathCount,
			candidateCount,
			evaluatedCount );

		if( pathCount > 0 )
		{
			GlobalLog()->PrintEx( eLog_Event,
				"CompletePathGuide:: Strategy selection evaluated %.3f / %.3f average BDPT techniques per path over %llu paths",
				static_cast<double>( evaluatedCount ) / static_cast<double>( pathCount ),
				static_cast<double>( candidateCount ) / static_cast<double>( pathCount ),
				pathCount );
		}
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
#ifdef RISE_ENABLE_OPENPGL
	pIntegrator->SetGuidingField( 0, 0, 0, 0, 0, eGuidingOneSampleMIS, 2 );
	pIntegrator->SetCompletePathGuide( 0, false, 0 );
	safe_release( pGuidingField );
	safe_release( pLightGuidingField );
	safe_release( pCompletePathGuide );
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
	pSplatFilm->Resolve( *pScratchImage, GetEffectiveSplatSPP( w, h ) );

	return *pScratchImage;
}
