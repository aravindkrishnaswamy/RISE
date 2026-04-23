//////////////////////////////////////////////////////////////////////
//
//  BDPTPelRasterizer.cpp - Implementation of the Pel (RGB) BDPT
//    rasterizer.
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
#include "BDPTPelRasterizer.h"
#include "AOVBuffers.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/ZSobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Sampling/SobolSequence.h"
#include "ProgressiveFilm.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

// Default safety clamp for per-strategy contribution.  Used when no
// user-specified stability clamp is active.  Prevents fireflies from
// imperfect MIS weights at volumetric vertices (SSS).  High enough
// to preserve energy for caustic paths through glass where individual
// contributions can legitimately reach several hundred.


BDPTPelRasterizer::BDPTPelRasterizer(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth,
	const ManifoldSolverConfig& smsConfig,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveCfg,
	const StabilityConfig& stabilityCfg,
	bool useZSobol_
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  BDPTRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, smsConfig, guidingConfig, stabilityCfg ),
  PixelBasedPelRasterizer( pCaster_, PathGuidingConfig(), AdaptiveSamplingConfig(), StabilityConfig(), false ),
  adaptiveConfig( adaptiveCfg )
{
	useZSobol = useZSobol_;
}

BDPTPelRasterizer::~BDPTPelRasterizer()
{
}

unsigned int BDPTPelRasterizer::GetProgressiveTotalSPP() const
{
	if( adaptiveConfig.maxSamples > 0 ) {
		return adaptiveConfig.maxSamples;
	}

	return PixelBasedRasterizerHelper::GetProgressiveTotalSPP();
}

void BDPTPelRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	// Delegate to PixelBasedPelRasterizer for guiding setup, then
	// override the stability config pointer with the authoritative
	// BDPTRasterizerBase copy (the PixelBasedPelRasterizer copy is
	// default-constructed due to diamond inheritance).
	PixelBasedPelRasterizer::PrepareRuntimeContext( rc );
	const StabilityConfig& sc = BDPTRasterizerBase::stabilityConfig;
	rc.pStabilityConfig = &sc;
}

//////////////////////////////////////////////////////////////////////
// IntegratePixelRGB - RGB integration for a single pixel sample.
// Generates subpaths, evaluates all (s,t) strategies, and returns
// the non-splat contribution.  Splats are written to pSplatFilm.
//////////////////////////////////////////////////////////////////////

RISEPel BDPTPelRasterizer::IntegratePixelRGB(
	const RuntimeContext& rc,
	const Point2& ptOnScreen,
	const IScene& pScene,
	const ICamera& camera,
	uint32_t sampleIndex,
	uint32_t pixelSeed,
	PixelAOV* pAOV
	) const
{
	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return RISEPel( 0, 0, 0 );
	}

	SobolSampler sampler( sampleIndex, pixelSeed );

	// Per-thread scratch — eliminates per-sample allocator traffic
	// in the BDPT hot path.
	static thread_local std::vector<BDPTVertex> lightVerts;
	static thread_local std::vector<BDPTVertex> eyeVerts;
	static thread_local std::vector<uint32_t> lightSubpathStarts;
	static thread_local std::vector<uint32_t> eyeSubpathStarts;
	lightVerts.clear();
	eyeVerts.clear();

	// Allow branching on both sides (pass -1 to use the configured
	// threshold).  The per-(eye-branch × light-branch) loop below
	// picks each sub-range out of the flat vertex vectors and runs
	// EvaluateAllStrategies independently per pair.  In the common
	// case of no split firing (single branch each), the outer loop
	// runs exactly once and behaviour matches the pre-refactor path
	// bit-exactly.
	pIntegrator->GenerateLightSubpath( pScene, *pCaster, sampler, lightVerts, lightSubpathStarts, rc.random, Scalar( -1 ) );
	pIntegrator->GenerateEyeSubpath( rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts, eyeSubpathStarts, Scalar( -1 ) );

	const size_t numEyeBranches = eyeSubpathStarts.size() >= 2 ?
		( eyeSubpathStarts.size() - 1 ) : 0;
	const size_t numLightBranches = lightSubpathStarts.size() >= 2 ?
		( lightSubpathStarts.size() - 1 ) : 0;

	// Extract first-hit AOV data for the denoiser.
	// Use the actual first eye-subpath surface vertex (even if delta)
	// so the albedo/normal buffers match what the unidirectional PT
	// produces.  For delta (specular) surfaces like glass, GetBSDF()
	// returns NULL; per OIDN documentation, transparent surfaces
	// should use white albedo (1,1,1) rather than zero, since the
	// beauty signal at those pixels is pure illumination with no
	// surface modulation.  Zero albedo causes OIDN to misinterpret
	// the energy balance, especially for BDPT where light subpath
	// connections can deposit significant energy through glass.
	if( pAOV && eyeVerts.size() > 1 ) {
		const BDPTVertex& v = eyeVerts[1];
		if( v.type == BDPTVertex::SURFACE && v.pMaterial ) {
			pAOV->normal = v.normal;
			if( v.pMaterial->GetBSDF() ) {
				Ray aovRay( Point3Ops::mkPoint3( v.position, v.normal ), -v.normal );
				RayIntersectionGeometric rig( aovRay, nullRasterizerState );
				rig.ptIntersection = v.position;
				rig.vNormal = v.normal;
				rig.onb = v.onb;
				pAOV->albedo = v.pMaterial->GetBSDF()->value( v.normal, rig ) * PI;
			} else {
				// Delta/transparent surface: white albedo per OIDN spec
				pAOV->albedo = RISEPel( 1, 1, 1 );
			}
			pAOV->valid = true;
		}
	}

	RISEPel sampleColor( 0, 0, 0 );

	// Per-(eye-branch × light-branch) EvaluateAllStrategies loop.
	// Each branch pair is an independent realization of the path;
	// its internal MIS weights sum to 1 over the (s,t) strategies
	// for that pair.  Deterministic kray weighting at the split
	// vertex (applied at subpath generation time) ensures the total
	// energy across branches is identical in expectation to the
	// pre-refactor random-select estimator.
	static thread_local std::vector<BDPTVertex> branchLightVerts;
	static thread_local std::vector<BDPTVertex> branchEyeVerts;

	for( size_t eb = 0; eb < numEyeBranches; eb++ ) {
		const uint32_t ebeg = eyeSubpathStarts[eb];
		const uint32_t eend = eyeSubpathStarts[eb + 1];
		if( ebeg >= eend ) continue;
		branchEyeVerts.assign(
			eyeVerts.begin() + ebeg, eyeVerts.begin() + eend );

		// Light side may be empty (Le==0, no emission): still evaluate
		// the eye-only strategies (s==0 hits emitter directly) by
		// passing an empty light-subpath through EvaluateAllStrategies.
		const size_t lbIters = numLightBranches > 0 ? numLightBranches : 1;
		for( size_t lb = 0; lb < lbIters; lb++ ) {
			if( numLightBranches > 0 ) {
				const uint32_t lbeg = lightSubpathStarts[lb];
				const uint32_t lend = lightSubpathStarts[lb + 1];
				if( lbeg >= lend ) continue;
				branchLightVerts.assign(
					lightVerts.begin() + lbeg, lightVerts.begin() + lend );
			} else {
				branchLightVerts.clear();
			}

			std::vector<BDPTIntegrator::ConnectionResult> results =
				pIntegrator->EvaluateAllStrategies(
					branchLightVerts,
					branchEyeVerts,
					pScene,
					*pCaster,
					camera,
					&sampler );

			for( unsigned int r=0; r<results.size(); r++ )
			{
				const BDPTIntegrator::ConnectionResult& cr = results[r];
				if( !cr.valid ) {
					continue;
				}

				// Dedupe strategies that are light-branch-independent.
				// Two cases read nothing beyond lightVerts[0] in either
				// contribution or MIS weight:
				//   s==0: eye path hits emitter directly — no light
				//         subpath vertices read at all.
				//   s==1: NEE/direct-lighting — reads only the LIGHT
				//         vertex at lightVerts[0], which is shared
				//         across all light branches (the split can
				//         only fire at lightVerts[1+] where the first
				//         scatter happens).
				// Gate both to lb==0 so each accumulates exactly once
				// per (eye-branch, spectral sample), matching pre-
				// branching behaviour.  Symmetric to the t==1 splat
				// gate below (eye-branch-independent light→camera).
				if( (cr.s == 0 || cr.s == 1) && lb != 0 ) continue;

				RISEPel weighted = cr.contribution * cr.misWeight;

				// Clamp per-strategy contribution.  Use directClamp for s==1
				// (direct lighting connections) and indirectClamp for all
				// other strategies.  A value of 0 means disabled.
				{
					const StabilityConfig& sc = BDPTRasterizerBase::stabilityConfig;
					const Scalar clampVal = (cr.s == 1)
						? sc.directClamp
						: sc.indirectClamp;
					if( clampVal > 0 ) {
						const Scalar maxVal = ColorMath::MaxValue( weighted );
						if( maxVal > clampVal ) {
							weighted = weighted * (clampVal / maxVal);
						}
					}
				}

				if( cr.needsSplat && pSplatFilm )
				{
					// Splat-to-camera (t=1) contributions are eye-
					// subpath-independent — the path goes light → camera
					// without traversing the eye walk beyond the camera
					// vertex.  When eye branching fires, the identical
					// splat would otherwise fire N_E times (once per
					// eye branch).  Gate on eb==0 so each pixel sample
					// produces one splat per light branch, matching
					// the pre-branching behaviour.  MIS weights across
					// eye branches differ at deep s values but the
					// PATH is identical — deduplicating at eb==0
					// avoids the N_E× energy blow-up.
					if( eb == 0 ) {
						const Scalar fx = cr.rasterPos.x;
						const Scalar fy = static_cast<Scalar>( camera.GetHeight() ) - cr.rasterPos.y;
						SplatContributionToFilm( fx, fy, weighted,
							camera.GetWidth(), camera.GetHeight() );
					}
				}
				else
				{
					sampleColor = sampleColor + weighted;
				}
			}
		}
	}

	// SMS contributions for specular caustic chains.
	//
	// SMS results are added with SMS's internal misWeight only (no
	// cross-strategy BDPT MIS).  The two estimators DO sample
	// overlapping path spaces — BDPT's (s==0) strategy generates
	// delta-through caustic paths whenever an eye subpath naturally
	// BSDF-samples a delta lobe and terminates at an emitter.
	// Double-counting is prevented upstream in
	// BDPTIntegrator::ConnectAndEvaluate's (s==0) branch, which
	// suppresses emission when the eye subpath has the SMS-reachable
	// topology (non-delta shading point followed by a delta chain
	// to the emitter).  That mirrors PT's
	// `bPassedThroughSpecular && bHadNonSpecularShading` rule.
	//
	// Per eye-branch: each branch has its own first non-specular
	// eye vertex, which is where SMS anchors the manifold chain.
	// StartStream(31) is issued once before the loop; subsequent
	// calls advance through the stream naturally so branches don't
	// consume correlated Sobol dimensions.
	if( pIntegrator ) {
		sampler.StartStream( 31 );
		for( size_t eb = 0; eb < numEyeBranches; eb++ ) {
			const uint32_t ebeg = eyeSubpathStarts[eb];
			const uint32_t eend = eyeSubpathStarts[eb + 1];
			if( ebeg >= eend ) continue;
			branchEyeVerts.assign(
				eyeVerts.begin() + ebeg, eyeVerts.begin() + eend );

			std::vector<BDPTIntegrator::ConnectionResult> smsResults =
				pIntegrator->EvaluateSMSStrategies(
					branchEyeVerts, pScene, *pCaster, camera, sampler );

			for( unsigned int r=0; r<smsResults.size(); r++ ) {
				const BDPTIntegrator::ConnectionResult& cr = smsResults[r];
				if( !cr.valid ) continue;

				RISEPel weighted = cr.contribution * cr.misWeight;
				{
					const StabilityConfig& sc = BDPTRasterizerBase::stabilityConfig;
					const Scalar clampVal = sc.directClamp;
					if( clampVal > 0 ) {
						const Scalar maxVal = ColorMath::MaxValue( weighted );
						if( maxVal > clampVal ) {
							weighted = weighted * (clampVal / maxVal);
						}
					}
				}
				sampleColor = sampleColor + weighted;
			}
		}
	}

	return sampleColor;
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel - Pel (RGB) pixel integration.
// Sample loop with pixel filtering, calls IntegratePixelRGB per sample.
//////////////////////////////////////////////////////////////////////

void BDPTPelRasterizer::IntegratePixel(
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

	// Progressive rendering: check if this pixel has already converged
	ProgressiveFilm* pProgFilm = rc.pProgressiveFilm;
	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		if( px.converged ) {
			if( px.alphaSum > 0 ) {
				cret = RISEColor( px.colorSum * (1.0/px.alphaSum), px.alphaSum / px.weightSum );
			}
			return;
		}
	}

	const bool bMultiSample = pSampling && rc.UsesPixelSampling();

	// Derive a per-pixel seed for Owen scrambling.
	// ZSobol (blue-noise): seed from Morton index for spatially
	// correlated scrambles across neighboring pixels.
	uint32_t pixelSeed;
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	const bool adaptive = adaptiveConfig.maxSamples > 0 && bMultiSample && rc.AllowsAdaptiveSampling();
	const unsigned int batchSize = bMultiSample ? pSampling->GetNumSamples() : 1;
	const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;

	// For ZSobol, compute log2SPP from the full progressive SPP budget
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
	} else {
		pixelSeed = SobolSequence::HashCombine(
			static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
	}

	// In progressive mode, restore persistent state from the film.
	RISEPel colAccrued( 0, 0, 0 );
	Scalar weights = 0;
	Scalar alphas = 0;
	Scalar wMean = 0;
	Scalar wM2 = 0;
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

	const uint32_t passStartSampleIndex = globalSampleIndex;
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
		if( bMultiSample ) {
			pSampling->GenerateSamplePoints( rc.random, samples );
		} else {
			samples.push_back( Point2( 0, 0 ) );
		}

		ISampling2D::SamplesList2D::const_iterator m, n;
		for( m=samples.begin(), n=samples.end(); m!=n && globalSampleIndex<passEndIndex; m++, globalSampleIndex++ )
		{
			Point2 ptOnScreen;
			Scalar weight = 1.0;

			// Uniform sub-pixel jitter for eye-subpath samples.  We
			// deliberately do NOT call pPixelFilter->warpOnScreen here:
			// that was the pre-existing behaviour but it aims rays at
			// screen positions dictated by the filter's importance
			// density (e.g. GaussianPixelFilter::warp lands samples
			// outside the pixel), which then get accumulated into this
			// pixel's bucket — a double-filtering that produces severe
			// blur with wide-support kernels.  Uniform jitter gives the
			// correct Monte-Carlo estimate of the per-pixel integral;
			// light-subpath splats go through SplatContributionToFilm
			// → SplatFilm::SplatFiltered which still uses the filter's
			// EvaluateFilter for cross-pixel reconstruction.
			if( bMultiSample ) {
				ptOnScreen = Point2(
					static_cast<Scalar>(x) + (*m).x - 0.5,
					static_cast<Scalar>(height-y) + (*m).y - 0.5 );
			} else {
				ptOnScreen = Point2( x, height-y );
			}
			weights += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			// For ZSobol, remap the sample index via Morton code so that
			// IntegratePixelRGB (which constructs the SobolSampler from
			// these parameters) gets the blue-noise-distributed index.
			const uint32_t effectiveIndex = useZSobol
				? ((mortonIndex << log2SPP) | globalSampleIndex)
				: globalSampleIndex;

#ifdef RISE_ENABLE_OIDN
			PixelAOV aov;
			const RISEPel sampleColor = IntegratePixelRGB( rc, ptOnScreen, pScene, *pCamera,
				effectiveIndex, pixelSeed, pAOVBuffers ? &aov : 0 );
			if( pAOVBuffers && aov.valid ) {
				pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
				pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
			}
#else
			const RISEPel sampleColor = IntegratePixelRGB( rc, ptOnScreen, pScene, *pCamera,
				effectiveIndex, pixelSeed, 0 );
#endif

			// Approach C: cross-pixel filter-weighted splat.  With a
			// wide-support filter, this spreads every eye-subpath
			// sample across the neighbouring pixels using the filter's
			// EvaluateFilter weights — mirroring PathTracingPelRasterizer.
			// When pFilteredFilm is null (narrow filter or filter off)
			// we fall through to per-pixel box accumulation below.
			if( pFilteredFilm ) {
				pFilteredFilm->Splat(
					ptOnScreen.x,
					static_cast<Scalar>(height) - ptOnScreen.y,
					sampleColor,
					*pPixelFilter );
			}

			colAccrued = colAccrued + sampleColor * weight;
			alphas += weight;

			// Welford update on luminance of non-splat contribution
			if( adaptive || pProgFilm ) {
				const Scalar lum = ColorMath::MaxValue(sampleColor);
				wN++;
				const Scalar delta = lum - wMean;
				wMean += delta / Scalar(wN);
				const Scalar delta2 = lum - wMean;
				wM2 += delta * delta2;
			}
		}

		// Check convergence after enough cumulative samples
		if( (adaptive || pProgFilm) && wN >= 32 )
		{
			const Scalar variance = wM2 / Scalar(wN - 1);
			const Scalar stdError = sqrt( variance / Scalar(wN) );
			const Scalar meanAbs = fabs( wMean );

			if( meanAbs > NEARZERO ) {
				const Scalar confidence = 1.0 - 4.0 / Scalar(wN);
				const Scalar threshold = adaptiveConfig.maxSamples > 0
					? adaptiveConfig.threshold
					: 0.01;
				if( stdError / meanAbs < threshold * confidence ) {
					converged = true;
				}
			} else if( wM2 < NEARZERO && wN >= 64 ) {
				converged = true;
			}
		}

		// For non-adaptive single-pass, the single batch covers everything
		if( !bMultiSample && !pProgFilm ) {
			break;
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

	// Track total adaptive samples for splat film normalization.
	// Use the delta (samples rendered THIS pass) to avoid double-counting
	// across progressive passes.
	if( adaptive || pProgFilm ) {
		AddAdaptiveSamples( globalSampleIndex - passStartSampleIndex );
	}

#ifdef RISE_ENABLE_OIDN
	if( pAOVBuffers && alphas > 0 && !pProgFilm ) {
		pAOVBuffers->Normalize( x, y, 1.0 / alphas );
	}
#endif

	if( adaptive && adaptiveConfig.showMap ) {
		const Scalar t = Scalar(globalSampleIndex) / Scalar(targetSamples);
		cret = RISEColor( RISEPel(t, t, t), 1.0 );
	} else if( alphas > 0 ) {
		colAccrued = colAccrued * (1.0 / alphas);
		cret = RISEColor( colAccrued, alphas / weights );
	}
}
