//////////////////////////////////////////////////////////////////////
//
//  VCMSpectralRasterizer.cpp - Spectral VCM rasterizer.
//
//    Mirrors BDPTSpectralRasterizer's HWSS pattern:
//      * generate BDPT light/eye subpaths at a hero wavelength
//      * ConvertLightSubpath / ConvertEyeSubpath produces
//        wavelength-independent VCMMisQuantities
//      * call the VCMIntegrator::Evaluate*NM strategies at the hero
//        wavelength, converting each scalar NM-value to XYZ and
//        accumulating
//      * for HWSS: re-evaluate companion wavelengths by copying the
//        vertex arrays, calling BDPTIntegrator::RecomputeSubpathThroughputNM
//        to update `throughputNM` at the new wavelength, then running
//        the strategies again against the same geometric path
//
//    Progressive state, adaptive convergence, ZSobol blue-noise seed,
//    OIDN AOV capture, splat film flush all mirror BDPT and the Pel
//    VCM IntegratePixel so that everything accumulates correctly
//    across progressive passes.
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
#include "VCMSpectralRasterizer.h"
#include "ProgressiveFilm.h"
#include "AOVBuffers.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/ICamera.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Shaders/BDPTVertex.h"

#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

VCMSpectralRasterizer::VCMSpectralRasterizer(
	IRayCaster* pCaster_,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const Scalar lambda_begin,
	const Scalar lambda_end,
	const unsigned int num_wavelengths,
	const unsigned int spectralSamples,
	const Scalar mergeRadius,
	const bool enableVC,
	const bool enableVM,
	const PathGuidingConfig& /*guidingConfig*/,
	const StabilityConfig& stabilityConfig_,
	const bool useZSobol,
	const bool useHWSS
	) :
	PixelBasedRasterizerHelper( pCaster_ ),
	VCMRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, mergeRadius, enableVC, enableVM, stabilityConfig_ ),
	PixelBasedSpectralIntegratingRasterizer(
		pCaster_,
		lambda_begin,
		lambda_end,
		num_wavelengths,
		spectralSamples,
		stabilityConfig_,
		useZSobol,
		useHWSS )
{
}

VCMSpectralRasterizer::~VCMSpectralRasterizer()
{
}

void VCMSpectralRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	PixelBasedSpectralIntegratingRasterizer::PrepareRuntimeContext( rc );
	const StabilityConfig& sc = VCMRasterizerBase::stabilityConfig;
	rc.pStabilityConfig = &sc;
}

void VCMSpectralRasterizer::PreRenderSetup( const IScene& pScene, const Rect* pRect ) const
{
	// PixelBasedSpectralIntegratingRasterizer does not override
	// PreRenderSetup, so we invoke VCMRasterizerBase's light pass
	// directly.
	VCMRasterizerBase::PreRenderSetup( pScene, pRect );
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel — spectral VCM pixel evaluation.
//
// Structure mirrors BDPTSpectralRasterizer::IntegratePixel for
// progressive / adaptive / ZSobol / OIDN / denoise parity.  Per pixel
// sample, we evaluate nSpectralSamples wavelength bundles.  Each
// bundle runs one subpath generation at a hero wavelength and — when
// HWSS is on — re-evaluates companion wavelengths by copying the
// subpath arrays and calling RecomputeSubpathThroughputNM.
//////////////////////////////////////////////////////////////////////
void VCMSpectralRasterizer::IntegratePixel(
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
	const unsigned int maxSamples = batchSize;

	BDPTIntegrator* pGen = pIntegrator ? pIntegrator->GetGenerator() : 0;
	if( !pGen ) {
		return;
	}

	// Owen-scrambling per-pixel seed.  Morton-indexed when ZSobol is
	// enabled so neighbours share low-order dimensions for blue-noise
	// error distribution.  Mirrors BDPTSpectralRasterizer exactly.
	uint32_t pixelSeed;
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;
	if( useZSobol &&
		MortonCode::CanEncode2D( static_cast<uint32_t>( x ), static_cast<uint32_t>( y ) ) )
	{
		const uint32_t mi = MortonCode::Morton2D(
			static_cast<uint32_t>( x ), static_cast<uint32_t>( y ) );
		const uint32_t zSobolPixelSPP = rc.totalProgressiveSPP > 0
			? rc.totalProgressiveSPP
			: maxSamples;
		const uint32_t l2 = MortonCode::Log2Int(
			MortonCode::RoundUpPow2( zSobolPixelSPP * nSpectralSamples ) );
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

	// Load persistent state from the progressive film.
	RISEPel colAccrued( 0, 0, 0 );
	Scalar weightsAccrued = 0;
	Scalar alphasAccrued = 0;
	Scalar wMean = 0;
	Scalar wM2 = 0;
	unsigned int wN = 0;
	uint32_t pixelSampleIndex = 0;
	bool converged = false;

	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		colAccrued = px.colorSum;
		weightsAccrued = px.weightSum;
		alphasAccrued = px.alphaSum;
		wMean = px.wMean;
		wM2 = px.wM2;
		wN = px.wN;
		pixelSampleIndex = px.sampleIndex;
	}

	const uint32_t passStartSampleIndex = pixelSampleIndex;
	const uint32_t targetSamples = pProgFilm && rc.totalProgressiveSPP > 0
		? rc.totalProgressiveSPP
		: maxSamples;
	uint32_t passEndIndex = targetSamples;
	if( pProgFilm ) {
		const uint64_t desiredEnd =
			static_cast<uint64_t>( pixelSampleIndex ) +
			static_cast<uint64_t>( batchSize );
		passEndIndex = desiredEnd < targetSamples
			? static_cast<uint32_t>( desiredEnd )
			: targetSamples;
	}

	std::vector<BDPTVertex> eyeVerts;
	std::vector<VCMMisQuantities> eyeMis;
	std::vector<BDPTVertex> localLightVerts;
	std::vector<LightVertex> localLightVertsStore;		// unused
	std::vector<VCMMisQuantities> localLightMis;
	eyeVerts.reserve( pIntegrator->GetMaxEyeDepth() + 1 );
	eyeMis.reserve( pIntegrator->GetMaxEyeDepth() + 1 );
	localLightVerts.reserve( pIntegrator->GetMaxLightDepth() + 1 );
	localLightMis.reserve( pIntegrator->GetMaxLightDepth() + 1 );

	// HWSS companion buffers (allocated once, reused each bundle).
	std::vector<BDPTVertex> compLight;
	std::vector<BDPTVertex> compEye;

	const Scalar lambda_diff = lambda_end - lambda_begin;

	while( pixelSampleIndex < passEndIndex && !converged )
	{
		ISampling2D::SamplesList2D samples;
		if( bMultiSample ) {
			pSampling->GenerateSamplePoints( rc.random, samples );
		} else {
			samples.push_back( Point2( 0, 0 ) );
		}

		ISampling2D::SamplesList2D::const_iterator m, n;
		for( m = samples.begin(), n = samples.end();
		     m != n && pixelSampleIndex < passEndIndex;
		     m++, pixelSampleIndex++ )
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

			// Spectral loop: nSpectralSamples wavelength bundles,
			// each either HWSS (hero + companions sharing the path)
			// or a single independent-wavelength evaluation.
			XYZPel spectralSum( 0, 0, 0 );
			unsigned int totalActive = 0;

			for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
			{
				// Camera ray.
				Ray cameraRay;
				if( !pCamera->GenerateRay( rc, cameraRay, ptOnScreen ) ) {
					continue;
				}

				// Hero wavelength selection + Sobol index.  Each
				// (pixel sample, spectral sample) pair gets a unique
				// index, Morton-remapped when ZSobol is active.
				Scalar heroNM;
				SampledWavelengths swl;
				if( bUseHWSS ) {
					const Scalar u = rc.random.CanonicalRandom();
					swl = SampledWavelengths::SampleEquidistant( u, lambda_begin, lambda_end );
					heroNM = swl.HeroLambda();
				} else {
					heroNM = ( num_wavelengths < 10000 )
						? ( lambda_begin + int( rc.random.CanonicalRandom() * Scalar( num_wavelengths ) ) * wavelength_steps )
						: ( lambda_begin + rc.random.CanonicalRandom() * lambda_diff );
				}

				const uint32_t combinedIndex = bUseHWSS
					? ( pixelSampleIndex * nSpectralSamples + ss ) * SampledWavelengths::N
					: ( pixelSampleIndex * nSpectralSamples + ss );
				const uint32_t effectiveIndex = useZSobol
					? ( ( mortonIndex << log2SPP ) | combinedIndex )
					: combinedIndex;
				SobolSampler sampler( effectiveIndex, pixelSeed );

				// Generate light subpath first (so it shares the
				// leading sampler dimensions — matches BDPT).
				// Skip when VC is disabled: VM reads from the
				// prebuilt store, not these per-sample subpaths.
				localLightVerts.clear();
				localLightMis.clear();
				localLightVertsStore.clear();
				if( mVCMNormalization.mEnableVC ) {
					pGen->GenerateLightSubpathNM(
						pScene, *pCaster, sampler, localLightVerts, heroNM, rc.random );
					if( !localLightVerts.empty() ) {
						VCMIntegrator::ConvertLightSubpath(
							localLightVerts, mVCMNormalization,
							localLightVertsStore, &localLightMis );
					}
				}

				eyeVerts.clear();
				pGen->GenerateEyeSubpathNM(
					rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts, heroNM );
				if( eyeVerts.empty() ) {
					continue;
				}
				VCMIntegrator::ConvertEyeSubpath(
					eyeVerts, mVCMNormalization, eyeMis );

#ifdef RISE_ENABLE_OIDN
				// First-hit AOV capture (hero wavelength, first bundle).
				if( ss == 0 && pAOVBuffers && eyeVerts.size() > 1 ) {
					const BDPTVertex& v1 = eyeVerts[1];
					if( v1.type == BDPTVertex::SURFACE && !v1.isDelta && v1.pMaterial ) {
						PixelAOV aov;
						aov.normal = v1.normal;
						if( v1.pMaterial->GetBSDF() ) {
							Ray aovRay( Point3Ops::mkPoint3( v1.position, v1.normal ), -v1.normal );
							RayIntersectionGeometric rig( aovRay, nullRasterizerState );
							rig.ptIntersection = v1.position;
							rig.vNormal = v1.normal;
							rig.onb = v1.onb;
							aov.albedo = v1.pMaterial->GetBSDF()->value( v1.normal, rig ) * PI;
						} else {
							aov.albedo = RISEPel( 1, 1, 1 );
						}
						aov.valid = true;
						pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
						pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
					}
				}
#endif

				Scalar heroValue = 0;

				// VC strategies at hero wavelength.
				if( mVCMNormalization.mEnableVC ) {
					if( pSplatFilm && !localLightVerts.empty() && !localLightMis.empty() ) {
						pIntegrator->SplatLightSubpathToCameraNM(
							localLightVerts, localLightMis,
							pScene, *pCaster, *pCamera, *pSplatFilm,
							mVCMNormalization, heroNM, pPixelFilter );
					}
					heroValue += pIntegrator->EvaluateS0NM(
						pScene, *pCaster, eyeVerts, eyeMis, mVCMNormalization, heroNM );
					heroValue += pIntegrator->EvaluateNEENM(
						pScene, *pCaster, sampler, eyeVerts, eyeMis, mVCMNormalization, heroNM );
					if( !localLightVerts.empty() && !localLightMis.empty() ) {
						heroValue += pIntegrator->EvaluateInteriorConnectionsNM(
							pScene, *pCaster,
							localLightVerts, localLightMis,
							eyeVerts, eyeMis,
							mVCMNormalization, heroNM );
					}
				}

				// VM strategy at hero wavelength.
				if( pLightVertexStore && mVCMNormalization.mEnableVM ) {
					heroValue += pIntegrator->EvaluateMergesNM(
						eyeVerts, eyeMis, *pLightVertexStore, mVCMNormalization, heroNM );
				}

				{
					const StabilityConfig& sc = VCMRasterizerBase::stabilityConfig;
					const Scalar clampVal = sc.directClamp;
					if( clampVal > 0 && fabs( heroValue ) > clampVal ) {
						heroValue = ( heroValue > 0 ) ? clampVal : -clampVal;
					}
				}

				XYZPel heroXYZ( 0, 0, 0 );
				if( ColorUtils::XYZFromNM( heroXYZ, heroNM ) ) {
					spectralSum = spectralSum + heroXYZ * heroValue;
					totalActive++;
				}

				if( !bUseHWSS ) {
					continue;
				}

				// HWSS: re-evaluate companion wavelengths on the same
				// geometric path.  If any vertex along the hero path is
				// dispersive (wavelength-dependent IOR) the companions
				// cannot share the geometry and are terminated.
				for( unsigned int w = 1; w < SampledWavelengths::N && !swl.SecondaryTerminated(); w++ )
				{
					if( BDPTIntegrator::HasDispersiveDeltaVertex( localLightVerts, heroNM, swl.lambda[w] ) ||
					    BDPTIntegrator::HasDispersiveDeltaVertex( eyeVerts,        heroNM, swl.lambda[w] ) )
					{
						swl.TerminateSecondary();
						break;
					}
				}

				for( unsigned int w = 1; w < SampledWavelengths::N; w++ )
				{
					if( swl.terminated[w] ) {
						// Still count terminated wavelengths with zero contribution.
						XYZPel compXYZ( 0, 0, 0 );
						if( ColorUtils::XYZFromNM( compXYZ, swl.lambda[w] ) ) {
							totalActive++;
						}
						continue;
					}

					const Scalar companionNM = swl.lambda[w];

					compLight = localLightVerts;
					compEye = eyeVerts;
					pGen->RecomputeSubpathThroughputNM(
						compLight, true, heroNM, companionNM, pScene, *pCaster );
					pGen->RecomputeSubpathThroughputNM(
						compEye, false, heroNM, companionNM, pScene, *pCaster );

					Scalar compValue = 0;

					// VC strategies at companion wavelength.
					if( mVCMNormalization.mEnableVC ) {
						if( pSplatFilm && !compLight.empty() && !localLightMis.empty() ) {
							pIntegrator->SplatLightSubpathToCameraNM(
								compLight, localLightMis,
								pScene, *pCaster, *pCamera, *pSplatFilm,
								mVCMNormalization, companionNM, pPixelFilter );
						}
						compValue += pIntegrator->EvaluateS0NM(
							pScene, *pCaster, compEye, eyeMis, mVCMNormalization, companionNM );
						compValue += pIntegrator->EvaluateNEENM(
							pScene, *pCaster, sampler, compEye, eyeMis, mVCMNormalization, companionNM );
						if( !compLight.empty() && !localLightMis.empty() ) {
							compValue += pIntegrator->EvaluateInteriorConnectionsNM(
								pScene, *pCaster,
								compLight, localLightMis,
								compEye, eyeMis,
								mVCMNormalization, companionNM );
						}
					}

					// VM strategy at companion wavelength.
					if( pLightVertexStore && mVCMNormalization.mEnableVM ) {
						compValue += pIntegrator->EvaluateMergesNM(
							compEye, eyeMis, *pLightVertexStore, mVCMNormalization, companionNM );
					}

					{
						const StabilityConfig& sc = VCMRasterizerBase::stabilityConfig;
						const Scalar clampVal = sc.directClamp;
						if( clampVal > 0 && fabs( compValue ) > clampVal ) {
							compValue = ( compValue > 0 ) ? clampVal : -clampVal;
						}
					}

					XYZPel compXYZ( 0, 0, 0 );
					if( ColorUtils::XYZFromNM( compXYZ, companionNM ) ) {
						spectralSum = spectralSum + compXYZ * compValue;
						totalActive++;
					}
				}
			}

			if( totalActive > 0 ) {
				spectralSum = spectralSum * ( Scalar( 1 ) / Scalar( totalActive ) );
			}

			const RISEPel samplePel( spectralSum.X, spectralSum.Y, spectralSum.Z );
			colAccrued = colAccrued + samplePel * weight;
			alphasAccrued += weight;

			if( pProgFilm ) {
				const Scalar lum = ColorMath::MaxValue( samplePel );
				wN++;
				const Scalar delta = lum - wMean;
				wMean += delta / Scalar( wN );
				const Scalar delta2 = lum - wMean;
				wM2 += delta * delta2;
			}
		}

		// Adaptive / progressive convergence check (mirrors Pel).
		if( pProgFilm && wN >= 32 )
		{
			const Scalar variance = wM2 / Scalar( wN - 1 );
			const Scalar stdError = sqrt( variance / Scalar( wN ) );
			const Scalar meanAbs = fabs( wMean );

			if( meanAbs > NEARZERO ) {
				const Scalar confidence = 1.0 - 4.0 / Scalar( wN );
				if( stdError / meanAbs < Scalar( 0.01 ) * confidence ) {
					converged = true;
				}
			} else if( wM2 < NEARZERO && wN >= 64 ) {
				converged = true;
			}
		}

		if( !bMultiSample && !pProgFilm ) {
			break;
		}
	}

	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		px.colorSum = colAccrued;
		px.weightSum = weightsAccrued;
		px.alphaSum = alphasAccrued;
		px.wMean = wMean;
		px.wM2 = wM2;
		px.wN = wN;
		px.sampleIndex = pixelSampleIndex;
		px.converged = converged;
	}

	if( pProgFilm ) {
		AddAdaptiveSamples( pixelSampleIndex - passStartSampleIndex );
	} else {
		AddAdaptiveSamples( batchSize );
	}

#ifdef RISE_ENABLE_OIDN
	if( pAOVBuffers && alphasAccrued > 0 && !pProgFilm ) {
		pAOVBuffers->Normalize( x, y, 1.0 / alphasAccrued );
	}
#endif

	if( alphasAccrued > 0 ) {
		colAccrued = colAccrued * ( Scalar( 1 ) / alphasAccrued );
		cret = RISEColor( colAccrued, alphasAccrued / weightsAccrued );
	} else {
		cret = RISEColor( 0, 0, 0, 0 );
	}
}
