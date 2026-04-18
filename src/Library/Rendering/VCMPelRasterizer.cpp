//////////////////////////////////////////////////////////////////////
//
//  VCMPelRasterizer.cpp - Pel (RGB) VCM rasterizer implementation.
//
//    Step 0 ships a solid-color IntegratePixel so the end-to-end
//    pipeline (parser -> Job::SetVCMPelRasterizer -> factory ->
//    scene attach -> block dispatch -> IntegratePixel -> output)
//    can be verified before any algorithmic code lands.
//
//    Steps 7-9 replace the IntegratePixel body with the real VCM
//    evaluation (s=0/s=1 + interior VC + VM merging).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "VCMPelRasterizer.h"
#include "ProgressiveFilm.h"
#include "AOVBuffers.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/ICamera.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Shaders/BDPTVertex.h"

#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

VCMPelRasterizer::VCMPelRasterizer(
	IRayCaster* pCaster_,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const Scalar mergeRadius,
	const bool enableVC,
	const bool enableVM,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig_,
	const bool useZSobol
	) :
	PixelBasedRasterizerHelper( pCaster_ ),
	VCMRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, mergeRadius, enableVC, enableVM, stabilityConfig_ ),
	PixelBasedPelRasterizer( pCaster_, guidingConfig, adaptiveConfig, stabilityConfig_, useZSobol )
{
}

VCMPelRasterizer::~VCMPelRasterizer()
{
}

void VCMPelRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	// Delegate to PixelBasedPelRasterizer for guiding setup, then
	// override the stability config pointer with the authoritative
	// VCMRasterizerBase copy (the PixelBasedPelRasterizer copy is
	// default-constructed due to diamond inheritance).
	PixelBasedPelRasterizer::PrepareRuntimeContext( rc );
	const StabilityConfig& sc = VCMRasterizerBase::stabilityConfig;
	rc.pStabilityConfig = &sc;
}

unsigned int VCMPelRasterizer::GetProgressiveTotalSPP() const
{
	if( adaptiveConfig.maxSamples > 0 ) {
		return adaptiveConfig.maxSamples;
	}
	return PixelBasedRasterizerHelper::GetProgressiveTotalSPP();
}

void VCMPelRasterizer::PreRenderSetup( const IScene& pScene, const Rect* pRect ) const
{
	// Both intermediate bases override the virtual from
	// PixelBasedRasterizerHelper.  Explicitly call both so:
	//   1. PixelBasedPelRasterizer handles its own bookkeeping
	//      (path-guiding training is a no-op in the v1 VCM config
	//      but we want a faithful base-class contract).
	//   2. VCMRasterizerBase runs the VCM light pass that fills
	//      the LightVertexStore for the subsequent eye pass.
	//
	// Order matters: run the Pel-base setup first so any state it
	// mutates is in place before the light pass reads it.
	PixelBasedPelRasterizer::PreRenderSetup( pScene, pRect );
	VCMRasterizerBase::PreRenderSetup( pScene, pRect );
}

void VCMPelRasterizer::FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	VCMRasterizerBase::FlushToOutputs( img, rcRegion, frame );
}

void VCMPelRasterizer::FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	VCMRasterizerBase::FlushPreDenoisedToOutputs( img, rcRegion, frame );
}

void VCMPelRasterizer::FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	VCMRasterizerBase::FlushDenoisedToOutputs( img, rcRegion, frame );
}

IRasterImage& VCMPelRasterizer::GetIntermediateOutputImage( IRasterImage& primary ) const
{
	return VCMRasterizerBase::GetIntermediateOutputImage( primary );
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel — Step 7 (VC-only s=0 path for now)
//
// Simpler than BDPTPelRasterizer::IntegratePixel: no progressive
// film, no adaptive sampling, no ZSobol.  Just a straight per-sample
// loop that generates an eye subpath, converts it through
// ConvertEyeSubpath, calls VCMIntegrator::EvaluateS0, and
// accumulates.  Steps 7c/7d/8/9 add NEE, t=1 light splat, interior
// connections, and merging.
//////////////////////////////////////////////////////////////////////
void VCMPelRasterizer::IntegratePixel(
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

	// Mirrors BDPTPelRasterizer::IntegratePixel so that progressive
	// rendering, per-sample t=1 splats, and the splat film normalization
	// all accumulate across passes identically to BDPT.

	// Progressive rendering: if this pixel has already converged, return
	// the stored value unchanged.
	ProgressiveFilm* pProgFilm = rc.pProgressiveFilm;
	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		if( px.converged ) {
			if( px.alphaSum > 0 ) {
				cret = RISEColor(
					px.colorSum * ( Scalar( 1 ) / px.alphaSum ),
					px.alphaSum / px.weightSum );
			}
			return;
		}
	}

	const bool bMultiSample = pSampling && pPixelFilter && rc.UsesPixelSampling();
	const unsigned int batchSize = bMultiSample ? pSampling->GetNumSamples() : 1;

	// Mirror BDPT's adaptive + ZSobol seed computation.
	const bool adaptive = adaptiveConfig.maxSamples > 0 && bMultiSample && rc.AllowsAdaptiveSampling();
	const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;

	uint32_t pixelSeed;
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	const unsigned int zSobolSPP = rc.totalProgressiveSPP > 0 ? rc.totalProgressiveSPP : maxSamples;

	if( useZSobol &&
	    MortonCode::CanEncode2D( static_cast<uint32_t>( x ), static_cast<uint32_t>( y ) ) )
	{
		const uint32_t mi = MortonCode::Morton2D(
			static_cast<uint32_t>( x ), static_cast<uint32_t>( y ) );
		const uint32_t l2 = MortonCode::Log2Int( MortonCode::RoundUpPow2( zSobolSPP ) );
		if( l2 < 32 &&
		    ( uint64_t( mi ) << l2 ) < ( uint64_t( 1 ) << 32 ) )
		{
			mortonIndex = mi;
			log2SPP = l2;
			pixelSeed = SobolSequence::HashCombine( mortonIndex, 0u );
		} else {
			pixelSeed = SobolSequence::HashCombine(
				static_cast<uint32_t>( x ), static_cast<uint32_t>( y ) );
		}
	} else {
		pixelSeed = SobolSequence::HashCombine(
			static_cast<uint32_t>( x ), static_cast<uint32_t>( y ) );
	}

	BDPTIntegrator* pGen = pIntegrator ? pIntegrator->GetGenerator() : 0;
	if( !pGen ) {
		return;
	}

	// Load persistent state from the progressive film (zero on first pass).
	RISEPel colAccrued( 0, 0, 0 );
	Scalar weightsAccrued = 0;
	Scalar alphasAccrued = 0;
	Scalar wMean = 0;
	Scalar wM2 = 0;
	unsigned int wN = 0;
	uint32_t globalSampleIndex = 0;
	bool converged = false;

	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		colAccrued = px.colorSum;
		weightsAccrued = px.weightSum;
		alphasAccrued = px.alphaSum;
		wMean = px.wMean;
		wM2 = px.wM2;
		wN = px.wN;
		globalSampleIndex = px.sampleIndex;
	}

	const uint32_t passStartSampleIndex = globalSampleIndex;
	const uint32_t targetSamples = pProgFilm && rc.totalProgressiveSPP > 0
		? rc.totalProgressiveSPP
		: maxSamples;
	uint32_t passEndIndex = targetSamples;
	if( pProgFilm ) {
		const uint64_t desiredEnd = static_cast<uint64_t>( globalSampleIndex ) +
			static_cast<uint64_t>( batchSize );
		passEndIndex = desiredEnd < targetSamples
			? static_cast<uint32_t>( desiredEnd )
			: targetSamples;
	}

	// Per-thread scratch.  thread_local keeps capacity across pixels,
	// samples, and entire passes — libmalloc never sees these
	// allocations again after the first sample per thread.
	static thread_local std::vector<BDPTVertex>         eyeVerts;
	static thread_local std::vector<VCMMisQuantities>   eyeMis;
	static thread_local std::vector<BDPTVertex>         localLightVerts;
	static thread_local std::vector<LightVertex>        localLightVertsStore;
	static thread_local std::vector<VCMMisQuantities>   localLightMis;
	eyeVerts.clear();
	eyeMis.clear();
	localLightVerts.clear();
	localLightVertsStore.clear();
	localLightMis.clear();
	if( eyeVerts.capacity() < pIntegrator->GetMaxEyeDepth() + 1 ) {
		eyeVerts.reserve( pIntegrator->GetMaxEyeDepth() + 1 );
	}
	if( eyeMis.capacity() < pIntegrator->GetMaxEyeDepth() + 1 ) {
		eyeMis.reserve( pIntegrator->GetMaxEyeDepth() + 1 );
	}
	if( localLightVerts.capacity() < pIntegrator->GetMaxLightDepth() + 1 ) {
		localLightVerts.reserve( pIntegrator->GetMaxLightDepth() + 1 );
	}
	if( localLightMis.capacity() < pIntegrator->GetMaxLightDepth() + 1 ) {
		localLightMis.reserve( pIntegrator->GetMaxLightDepth() + 1 );
	}

	while( globalSampleIndex < passEndIndex && !converged )
	{
		ISampling2D::SamplesList2D samples;
		if( bMultiSample ) {
			pSampling->GenerateSamplePoints( rc.random, samples );
		} else {
			samples.push_back( Point2( 0, 0 ) );
		}

		ISampling2D::SamplesList2D::const_iterator m, n;
		for( m = samples.begin(), n = samples.end();
		     m != n && globalSampleIndex < passEndIndex;
		     m++, globalSampleIndex++ )
		{
			Point2 ptOnScreen;
			Scalar weight = 1.0;
			if( bMultiSample ) {
				weight = pPixelFilter->warpOnScreen(
					rc.random, *m, ptOnScreen, x, height - y );
			} else {
				ptOnScreen = Point2( x, height - y );
			}
			weightsAccrued += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime(
					temporal_start + ( rc.random.CanonicalRandom() * temporal_exposure ) );
			}

			Ray cameraRay;
			if( !pCamera->GenerateRay( rc, cameraRay, ptOnScreen ) ) {
				continue;
			}

			// For ZSobol, remap the sample index via Morton code so
			// that the SobolSampler produces a blue-noise pixel
			// distribution.
			const uint32_t effectiveIndex = useZSobol
				? ( ( mortonIndex << log2SPP ) | globalSampleIndex )
				: globalSampleIndex;

			SobolSampler sampler( effectiveIndex, pixelSeed );

			// Mirror BDPT: generate the LIGHT subpath first, then the
			// EYE subpath, sharing the same SobolSampler so the two
			// subpaths are jointly stratified across the Sobol
			// dimensions.  When VC is disabled the local light
			// subpath is unused (VM reads from the prebuilt store),
			// so skip the generation cost.
			localLightVerts.clear();
			localLightMis.clear();
			localLightVertsStore.clear();
			if( mVCMNormalization.mEnableVC ) {
				pGen->GenerateLightSubpath( pScene, *pCaster, sampler, localLightVerts, rc.random );
				if( !localLightVerts.empty() ) {
					VCMIntegrator::ConvertLightSubpath(
						localLightVerts, mVCMNormalization, localLightVertsStore, &localLightMis );
				}
			}

			eyeVerts.clear();
			pGen->GenerateEyeSubpath( rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts );
			if( eyeVerts.empty() ) {
				continue;
			}
			VCMIntegrator::ConvertEyeSubpath( eyeVerts, mVCMNormalization, eyeMis );

#ifdef RISE_ENABLE_OIDN
			// Extract first-hit AOV data for the denoiser from the
			// first eye-subpath surface vertex.  Mirrors
			// BDPTPelRasterizer::IntegratePixelRGB.
			if( pAOVBuffers && eyeVerts.size() > 1 ) {
				const BDPTVertex& v = eyeVerts[1];
				if( v.type == BDPTVertex::SURFACE && v.pMaterial ) {
					PixelAOV aov;
					aov.normal = v.normal;
					if( v.pMaterial->GetBSDF() ) {
						Ray aovRay( Point3Ops::mkPoint3( v.position, v.normal ), -v.normal );
						RayIntersectionGeometric rig( aovRay, nullRasterizerState );
						rig.ptIntersection = v.position;
						rig.vNormal = v.normal;
						rig.onb = v.onb;
						aov.albedo = v.pMaterial->GetBSDF()->value( v.normal, rig ) * PI;
					} else {
						aov.albedo = RISEPel( 1, 1, 1 );
					}
					aov.valid = true;
					pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
					pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
				}
			}
#endif

			RISEPel sampleColor( 0, 0, 0 );

			// VC strategies: t=1 splat, s=0, NEE, interior connections.
			if( mVCMNormalization.mEnableVC ) {
				// Strategy (t=1): splat every connectible light-subpath
				// vertex to the camera.  Mirrors BDPT's per-sample
				// splat so the splat film accumulates across all
				// progressive passes and the final resolve divides by
				// the adaptive total sample count.
				if( pSplatFilm && !localLightVerts.empty() && !localLightMis.empty() ) {
					pIntegrator->SplatLightSubpathToCamera(
						localLightVerts, localLightMis,
						pScene, *pCaster, *pCamera, *pSplatFilm,
						mVCMNormalization, pPixelFilter );
				}

				// Non-splat strategies accumulated into sampleColor.
				sampleColor = pIntegrator->EvaluateS0(
					pScene, *pCaster, eyeVerts, eyeMis, mVCMNormalization );

				sampleColor = sampleColor + pIntegrator->EvaluateNEE(
					pScene, *pCaster, sampler, eyeVerts, eyeMis, mVCMNormalization );

				if( !localLightVerts.empty() && !localLightMis.empty() ) {
					sampleColor = sampleColor + pIntegrator->EvaluateInteriorConnections(
						pScene, *pCaster,
						localLightVerts, localLightMis,
						eyeVerts, eyeMis,
						mVCMNormalization );
				}
			}

			// VM strategy: vertex merging from the light vertex store.
			if( pLightVertexStore && mVCMNormalization.mEnableVM ) {
				sampleColor = sampleColor + pIntegrator->EvaluateMerges(
					eyeVerts, eyeMis, *pLightVertexStore, mVCMNormalization );
			}

			// Clamp per-SmallVCM convention.
			{
				const StabilityConfig& sc = VCMRasterizerBase::stabilityConfig;
				const Scalar clampVal = sc.directClamp;
				if( clampVal > 0 ) {
					const Scalar maxVal = ColorMath::MaxValue( sampleColor );
					if( maxVal > clampVal ) {
						sampleColor = sampleColor * ( clampVal / maxVal );
					}
				}
			}

			colAccrued = colAccrued + sampleColor * weight;
			alphasAccrued += weight;

			// Welford update on luminance of non-splat contribution.
			if( adaptive || pProgFilm ) {
				const Scalar lum = ColorMath::MaxValue( sampleColor );
				wN++;
				const Scalar delta = lum - wMean;
				wMean += delta / Scalar( wN );
				const Scalar delta2 = lum - wMean;
				wM2 += delta * delta2;
			}
		}

		// Adaptive/progressive convergence check (mirrors BDPT).
		if( ( adaptive || pProgFilm ) && wN >= 32 )
		{
			const Scalar variance = wM2 / Scalar( wN - 1 );
			const Scalar stdError = sqrt( variance / Scalar( wN ) );
			const Scalar meanAbs = fabs( wMean );

			if( meanAbs > NEARZERO ) {
				const Scalar confidence = 1.0 - 4.0 / Scalar( wN );
				const Scalar threshold = adaptiveConfig.maxSamples > 0
					? adaptiveConfig.threshold
					: Scalar( 0.01 );
				if( stdError / meanAbs < threshold * confidence ) {
					converged = true;
				}
			} else if( wM2 < NEARZERO && wN >= 64 ) {
				converged = true;
			}
		}

		// Single-pass non-progressive: one batch covers everything.
		if( !bMultiSample && !pProgFilm ) {
			break;
		}
	}

	// Write back persistent state for progressive rendering.
	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		px.colorSum = colAccrued;
		px.weightSum = weightsAccrued;
		px.alphaSum = alphasAccrued;
		px.wMean = wMean;
		px.wM2 = wM2;
		px.wN = wN;
		px.sampleIndex = globalSampleIndex;
		px.converged = converged;
	}

	// Track total samples for the splat film normalization.  Add only
	// the delta (samples rendered THIS pass) to avoid double counting
	// across progressive passes.
	if( adaptive || pProgFilm ) {
		AddAdaptiveSamples( globalSampleIndex - passStartSampleIndex );
	} else {
		AddAdaptiveSamples( batchSize );
	}

#ifdef RISE_ENABLE_OIDN
	if( pAOVBuffers && alphasAccrued > 0 && !pProgFilm ) {
		pAOVBuffers->Normalize( x, y, 1.0 / alphasAccrued );
	}
#endif

	if( adaptive && adaptiveConfig.showMap ) {
		const Scalar t = Scalar( globalSampleIndex ) / Scalar( targetSamples );
		cret = RISEColor( RISEPel( t, t, t ), 1.0 );
	} else if( alphasAccrued > 0 ) {
		colAccrued = colAccrued * ( Scalar( 1 ) / alphasAccrued );
		cret = RISEColor( colAccrued, alphasAccrued / weightsAccrued );
	} else {
		cret = RISEColor( 0, 0, 0, 0 );
	}
}
