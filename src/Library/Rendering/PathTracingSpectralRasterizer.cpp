//////////////////////////////////////////////////////////////////////
//
//  PathTracingSpectralRasterizer.cpp - Pure spectral path tracing
//    rasterizer.
//
//    Bypasses the shader op pipeline entirely.  Uses
//    PathTracingIntegrator for direct iterative path tracing.
//    Each pixel sample takes nSpectralSamples random wavelengths
//    (or HWSS bundles), converts to XYZ, and accumulates.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PathTracingSpectralRasterizer.h"
#include "../Shaders/PathTracingIntegrator.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/ZSobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Sampling/SobolSequence.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/Color/SampledWavelengths.h"
#include "../Interfaces/IScene.h"
#include "../Managers/ObjectManager.h"
#include "ProgressiveFilm.h"

using namespace RISE;
using namespace RISE::Implementation;

PathTracingSpectralRasterizer::PathTracingSpectralRasterizer(
	IRayCaster* pCaster_,
	const Scalar lambda_begin_,
	const Scalar lambda_end_,
	const unsigned int num_wavelengths_,
	const unsigned int spectralSamples,
	const ManifoldSolverConfig& smsConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityCfg,
	bool useZSobol_,
	bool useHWSS_,
	RISE::Implementation::FrameStore* frameStore
	) :
  Rasterizer( frameStore ),
  PixelBasedRasterizerHelper( pCaster_ , frameStore),
  PixelBasedSpectralIntegratingRasterizer( pCaster_, lambda_begin_, lambda_end_, num_wavelengths_, spectralSamples, stabilityCfg, useZSobol_, useHWSS_ , frameStore),
  pIntegrator( 0 ),
  adaptiveConfig( adaptiveConfig ),
  pSMSPhotonMap( 0 ),
  mSMSPhotonCount( smsConfig.enabled ? smsConfig.photonCount : 0 )
{
	pIntegrator = new PathTracingIntegrator(
		smsConfig,
		stabilityCfg
		);
}

void PathTracingSpectralRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
    PixelBasedSpectralIntegratingRasterizer::PrepareRuntimeContext(rc);
    rc.pNativePTSubsurfaceBounceCap = rc.pStabilityConfig ?
        &rc.pStabilityConfig->maxTranslucentBounce : nullptr;
}

PathTracingSpectralRasterizer::~PathTracingSpectralRasterizer()
{
	safe_release( pIntegrator );
	if( pSMSPhotonMap ) {
		delete pSMSPhotonMap;
		pSMSPhotonMap = 0;
	}
}

// Delegates to PixelBasedSpectralIntegratingRasterizer::PreRenderSetup
// for the path guiding training phase before (optionally) building
// the SMS photon map.  See PathTracingPelRasterizer::PreRenderSetup
// for the full rationale — missing the super-class call silently
// disables path guiding.
void PathTracingSpectralRasterizer::PreRenderSetup(
	const IScene& pScene,
	const Rect* pRect
	) const
{
	PixelBasedSpectralIntegratingRasterizer::PreRenderSetup( pScene, pRect );

	if( !pIntegrator ) {
		return;
	}
	ManifoldSolver* pSolver = pIntegrator->GetSolver();
	if( !pSolver ) {
		return;
	}

	// See PathTracingPelRasterizer::PreRenderSetup for rationale.
	std::vector<const IObject*> casters;
	ManifoldSolver::EnumerateSpecularCasters( pScene, casters );
	pSolver->SetSpecularCasters( std::move( casters ) );

	if( mSMSPhotonCount == 0 ) {
		return;
	}

	if( !pSMSPhotonMap ) {
		pSMSPhotonMap = new SMSPhotonMap();
	}
	const unsigned int stored = pSMSPhotonMap->Build( pScene, mSMSPhotonCount );
	pSolver->SetPhotonMap( stored > 0 ? pSMSPhotonMap : 0 );
}

bool PathTracingSpectralRasterizer::ValidateSceneForRasterizer( const IScene& pScene ) const
{
	if( const ObjectManager* pObjMgr = dynamic_cast<const ObjectManager*>( pScene.GetObjects() ) ) {
		std::string offender;
		if( !pObjMgr->SceneCoversHyLIoSWavelengths( lambda_begin, lambda_end, &offender ) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"PathTracingSpectralRasterizer:: Refusing scene: HyLIoS material on object '%s' "
				"does not cover the requested spectral range [%.2f, %.2f] nm.",
				offender.c_str(), lambda_begin, lambda_end );
			return false;
		}
	}
	return true;
}

unsigned int PathTracingSpectralRasterizer::GetProgressiveTotalSPP() const
{
	if( adaptiveConfig.maxSamples > 0 ) {
		return adaptiveConfig.maxSamples;
	}

	return PixelBasedRasterizerHelper::GetProgressiveTotalSPP();
}

//////////////////////////////////////////////////////////////////////
// IntegratePixelSpectral - spectral integration for a single pixel
// sample.  Samples nSpectralSamples wavelengths (or HWSS bundles),
// converts each to XYZ, and returns the averaged result.
//////////////////////////////////////////////////////////////////////

XYZPel PathTracingSpectralRasterizer::IntegratePixelSpectral(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Point2& ptOnScreen,
	const IScene& pScene,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	PixelAOV* pAOV,
	bool* pPrimaryHit
	) const
{
	if( pPrimaryHit ) *pPrimaryHit = false;
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera || nSpectralSamples == 0 ) {
		return XYZPel( 0, 0, 0 );
	}

	Ray cameraRay;
	if( !pCamera->GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return XYZPel( 0, 0, 0 );
	}

	XYZPel spectralSum( 0, 0, 0 );
	bool anyHit = false;

	if( bUseHWSS )
	{

		for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
		{
			const Scalar u = rc.random.CanonicalRandom();
			SampledWavelengths swl = SampledWavelengths::SampleEquidistant( u, lambda_begin, lambda_end, num_wavelengths );

			XYZPel bundleSum(0,0,0);
			Scalar result[SampledWavelengths::N] = {0};
			bool sampleHit = false;
			pIntegrator->IntegrateRayHWSS(
				rc, rast, cameraRay, swl, pScene, *pCaster, sampler, pRadianceMap, result, pAOV,
				pPrimaryHit ? &sampleHit : nullptr );
			if( sampleHit ) anyHit = true;

			for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
				if( !swl.terminated[i] ) {
					XYZPel thisNM( 0, 0, 0 );
					if( ColorUtils::XYZFromNM( thisNM, swl.lambda[i] ) ) {
						bundleSum = bundleSum + thisNM * result[i];
					}
				}
			}
			spectralSum = spectralSum + swl.BundleAverage(bundleSum);
		}

		if( pPrimaryHit ) *pPrimaryHit = anyHit;
		// Average independently sampled bundles, not their pooled active lanes.
		return spectralSum * (mYNormalization / Scalar(nSpectralSamples));
	}

	// Standard (non-HWSS) path: independent random wavelengths
	for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
	{
		const Scalar nm = num_wavelengths < 10000 ?
			(lambda_begin + int(rc.random.CanonicalRandom() * Scalar(num_wavelengths)) * wavelength_steps) :
			(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);

		bool sampleHit = false;
		const Scalar nmvalue = pIntegrator->IntegrateRayNM(
			rc, rast, cameraRay, nm, pScene, *pCaster, sampler, pRadianceMap, pAOV,
			pPrimaryHit ? &sampleHit : nullptr );
		if( sampleHit ) anyHit = true;

		if( nmvalue > 0 ) {
			XYZPel thisNM( 0, 0, 0 );
			if( ColorUtils::XYZFromNM( thisNM, nm ) ) {
				spectralSum = spectralSum + thisNM * nmvalue;
			}
		}
	}

	if( pPrimaryHit ) *pPrimaryHit = anyHit;
	// MC normalization: (1/N) × (b-a)/k_y.  See HWSS branch above.
	return spectralSum * ( mYNormalization / Scalar(nSpectralSamples) );
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel - Spectral pixel integration.
// Sample loop with pixel filtering, calls IntegratePixelSpectral
// per sample.
//////////////////////////////////////////////////////////////////////

void PathTracingSpectralRasterizer::IntegratePixel(
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
	ISampling2D* const sampling = ActiveSampling();
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		return;
	}

	RasterizerState rast = {x,y};
	const IRadianceMap* pRadianceMap = pScene.GetGlobalRadianceMap();

	const bool bMultiSample = sampling && rc.UsesPixelSampling();

	// Derive a per-pixel seed for Owen scrambling.
	uint32_t pixelSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	if( bMultiSample )
	{
		const bool adaptive = adaptiveConfig.maxSamples > 0 && rc.AllowsAdaptiveSampling();
		const unsigned int batchSize = sampling->GetNumSamples();
		const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;

		ProgressiveFilm* pProgFilm = rc.pProgressiveFilm;
		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			if( px.converged ) {
				if( adaptive && adaptiveConfig.showMap && adaptiveConfig.maxSamples > 0 ) {
					const Scalar t = Scalar( px.sampleIndex ) / Scalar( adaptiveConfig.maxSamples );
					cret = RISEColor( RISEPel( t, t, t ), 1.0 );
				} else if( px.alphaSum > 0 ) {
					// px.colorSum is XYZPel; defer XYZ->ROMM RGB to
					// here via implicit RISEPel(XYZPel) constructor.
					const XYZPel avgXYZ = px.colorSum * (1.0/px.alphaSum);
					cret = RISEColor( RISEPel( avgXYZ ), px.alphaSum / px.weightSum );
				} else {
					cret = RISEColor( RISEPel( 0, 0, 0 ), 0.0 );
				}
				return;
			}
		}

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

		// PBRT-v4 RGBFilm-style accumulator: XYZ throughout, deferred
		// XYZ->ROMM RGB conversion at the per-pixel resolve below.
		// Removes per-sample chromaticity gamut clip bias.
		XYZPel colAccrued( 0, 0, 0 );
		Scalar weights = 0;
		Scalar alphas = 0;
		BeautyMoments moments;

		uint32_t sampleIndex = 0;
		bool converged = false;

		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			colAccrued = px.colorSum;
			weights = px.weightSum;
			alphas = px.alphaSum;
			moments = px.beautyMoments;
			sampleIndex = px.sampleIndex;
		}

		const uint32_t targetSamples = pProgFilm && rc.totalProgressiveSPP > 0
			? rc.totalProgressiveSPP
			: maxSamples;
		uint32_t passEndIndex = targetSamples;
		if( pProgFilm ) {
			const uint64_t desiredEnd = static_cast<uint64_t>( sampleIndex ) + static_cast<uint64_t>( batchSize );
			passEndIndex = desiredEnd < targetSamples ? static_cast<uint32_t>( desiredEnd ) : targetSamples;
		}

		while( sampleIndex < passEndIndex && !converged )
		{
			ISampling2D::SamplesList2D samples;
			sampling->GenerateSamplePoints( rc.random, samples );

			ISampling2D::SamplesList2D::const_iterator m, n;
			for( m=samples.begin(), n=samples.end(); m!=n && sampleIndex<passEndIndex; m++, sampleIndex++ )
			{
				Point2 ptOnScreen;

				const bool filmMode = (pFilteredFilm != 0);
				Scalar weight;
				if( filmMode ) {
					ptOnScreen = RasterConvention::PixelToScreen( x, y, height, (*m).x, (*m).y );
					weight = 1.0;
				} else if( pPixelFilter ) {
					weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-1-y );
				} else {
					ptOnScreen = RasterConvention::PixelCentreToScreen( x, y, height );
					weight = 1.0;
				}
				weights += weight;

				if( temporal_samples && !pScene.FireTemporalHold() ) {
					AnimateSceneToSampleTime( pScene, temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
				}

				const uint32_t effectiveIndex = useZSobol
					? ((mortonIndex << log2SPP) | sampleIndex)
					: sampleIndex;

				SobolSampler stdSampler( effectiveIndex, pixelSeed );
				ZSobolSampler zSampler( effectiveIndex, mortonIndex, log2SPP, pixelSeed );
				ISampler& sampler = useZSobol
					? static_cast<ISampler&>(zSampler)
					: static_cast<ISampler&>(stdSampler);

				rc.pSampler = &sampler;

				PixelAOV aov;
				bool primaryHit = false;
				const bool transparentBg = GetTransparentBackground();
				XYZPel sampleXYZ = IntegratePixelSpectral(
					rc, rast, ptOnScreen, pScene, sampler, pRadianceMap,
					pAOVBuffers ? &aov : 0, transparentBg ? &primaryHit : nullptr );
				const Scalar sampleAlpha = ( transparentBg && !primaryHit ) ? Scalar( 0.0 ) : Scalar( 1.0 );
				if( sampleAlpha == Scalar( 0.0 ) ) {
					sampleXYZ = XYZPel( 0, 0, 0 );
				}
				if( pAOVBuffers ) {
					if( aov.valid ) {
						pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
						pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
					}
					if( aov.primaryDepthCaptured ) {
						pAOVBuffers->AccumulateDepth( x, y, aov.depth, weight );
						pAOVBuffers->MarkGuidesExamined();
					}
				}
				// Defer XYZ -> ROMM RGB to per-pixel resolve.  FilteredFilm
				// now accumulates XYZ; no per-sample chromaticity clip.

				if( filmMode ) {
					pFilteredFilm->Splat( ptOnScreen.x, static_cast<Scalar>(height) - ptOnScreen.y, sampleXYZ, *pPixelFilter, sampleAlpha );
					colAccrued = colAccrued + sampleXYZ;
					alphas += sampleAlpha;
				} else {
					colAccrued = colAccrued + sampleXYZ * weight;
					alphas += sampleAlpha * weight;
				}

				// Compensated moment update gated on `adaptive` only (see
				// BDPTPelRasterizer for the multi-pass selection-bias
				// rationale).  XYZ.Y is the CIE photometric luminance.
				if( adaptive ) {
					const Scalar lum = sampleXYZ.Y;
					moments.Add( lum );
				}

				rc.pSampler = 0;
			}

			if( adaptive && moments.IsConverged( (adaptiveConfig.maxSamples > 0 ? adaptiveConfig.threshold : 0.01), NEARZERO ) ) {
				converged = true;
			}
		}

		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			px.colorSum = colAccrued;
			px.weightSum = weights;
			px.alphaSum = alphas;
			px.beautyMoments = moments;
			px.sampleIndex = sampleIndex;
			px.converged = converged;
		}

		// Normalize accumulated AOVs by total weight (non-progressive only;
		// the progressive path normalizes in PixelBasedRasterizerHelper after
		// the final pass).  Mirrors PathTracingPelRasterizer.
		if( pAOVBuffers && alphas > 0 && !pProgFilm ) {
			pAOVBuffers->Normalize( x, y, 1.0 / alphas );
		}

		if( adaptive && adaptiveConfig.showMap ) {
			const Scalar t = Scalar(sampleIndex) / Scalar(targetSamples);
			cret = RISEColor( RISEPel(t, t, t), 1.0 );
		} else if( alphas > 0 ) {
			// Single XYZ -> ROMM RGB conversion at resolve.
			const XYZPel avgXYZ = colAccrued * (1.0 / alphas);
			cret = RISEColor( RISEPel( avgXYZ ), alphas / weights );
		} else {
			cret = RISEColor( RISEPel( 0, 0, 0 ), 0.0 );
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
		}

		SobolSampler stdSampler( 0, pixelSeed );
		ZSobolSampler zSampler( 0, mortonIndex, 0, pixelSeed );
		ISampler& sampler = useZSobol
			? static_cast<ISampler&>(zSampler)
			: static_cast<ISampler&>(stdSampler);

		rc.pSampler = &sampler;

		PixelAOV aov;
		bool primaryHit = false;
		const bool transparentBg = GetTransparentBackground();
		const XYZPel sampleXYZ = IntegratePixelSpectral(
			rc, rast, RasterConvention::PixelCentreToScreen( x, y, height ), pScene, sampler, pRadianceMap,
			pAOVBuffers ? &aov : 0, transparentBg ? &primaryHit : nullptr );
		if( pAOVBuffers ) {
			if( aov.valid ) {
				pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, 1.0 );
				pAOVBuffers->AccumulateNormal( x, y, aov.normal, 1.0 );
			}
			if( aov.primaryDepthCaptured ) {
				pAOVBuffers->AccumulateDepth( x, y, aov.depth, 1.0 );
				pAOVBuffers->MarkGuidesExamined();
			}
			pAOVBuffers->Normalize( x, y, 1.0 );
		}

		// Proper XYZ -> ROMM RGB via implicit RISEPel(XYZPel).
		const Scalar sampleAlpha = ( transparentBg && !primaryHit ) ? Scalar( 0.0 ) : Scalar( 1.0 );
		cret = ( sampleAlpha == Scalar( 0.0 ) )
			? RISEColor( RISEPel( 0, 0, 0 ), 0.0 )
			: RISEColor( RISEPel( sampleXYZ ), sampleAlpha );

		rc.pSampler = 0;
	}
}
