//////////////////////////////////////////////////////////////////////
//
//  RayCaster.cpp - Implementation of the RayCaster class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 20, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayCaster.h"
#include "LuminaryManager.h"
#include "EnvironmentSampler.h"
#include "../Lights/LightSampler.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/MediumTracking.h"
#include "../Utilities/MediumTransport.h"
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/PathGuidingField.h"
#include "../Utilities/PathTransportUtilities.h"

#define ENABLE_MAX_RECURSION

//#define ENABLE_TERMINATION_MESSAGES

//
// Unbiased Russian Roulette in the RayCaster.  When rs.importance
// drops below this threshold, a proportional survival test fires
// before the intersection/shading work.  Survivors have the
// returned radiance scaled by 1/pSurvive to compensate for the
// killed paths, maintaining an unbiased estimator.
//
// This is a secondary safety net; the primary RR lives in
// PathTracingShaderOp.  The shader-level RR handles the common
// case (throughput < 1 after a few bounces).  This catches
// extreme low-importance rays that survive the shader RR.
//
#define ENABLE_RAYCASTER_RR
static const RISE::Scalar RC_RR_THRESHOLD = 0.01;

using namespace RISE;
using namespace RISE::Implementation;

RayCaster::RayCaster( 
	const bool seeRadianceMap,
	const unsigned int maxR,
	const Scalar minI,
	const IShader& pDefaultShader_,
	const bool showLuminaires,
	const bool useiorstack,
	const bool chooseonlyoneluminaire
	) : 
  pScene( 0 ),
  pDefaultShader( pDefaultShader_ ),
  pLuminaryManager( 0 ),
  pLightSampler( 0 ),
  pLumSampling( 0 ),
  bConsiderRMapAsBackground( seeRadianceMap ),
  nMaxRecursions( maxR ),
  dMinImportance( minI ),
  bShowLuminaires( showLuminaires ),
  bIORStack( useiorstack ),
  bChooseOnlyOneLuminaire( chooseonlyoneluminaire ),
  dPendingLightRRThreshold( 0 )
{
	pDefaultShader.addref();
}

RayCaster::~RayCaster( )
{
	safe_release( pScene );

	pDefaultShader.release();

	safe_release( pLumSampling );
	safe_release( pLightSampler );
	safe_release( pLuminaryManager );
}

void RayCaster::AttachScene( const IScene* pScene_ )
{
	if( pScene == pScene_ ) {
		return;
	}

	if( pScene_ ) {
		safe_release( pScene );

		pScene = pScene_;
		pScene->addref();

		safe_release( pLuminaryManager );

		LuminaryManager* pConcreteLumMgr = new LuminaryManager( bChooseOnlyOneLuminaire );
		pLuminaryManager = pConcreteLumMgr;
		GlobalLog()->PrintNew( pLuminaryManager, __FILE__, __LINE__, "luminary manager" );
		pLuminaryManager->AttachScene( pScene );

		if( pLumSampling ) {
			pLuminaryManager->SetLuminaireSampling( pLumSampling );
		}

		// Create and prepare the unified light sampler
		safe_release( pLightSampler );
		pLightSampler = new LightSampler();
		GlobalLog()->PrintNew( pLightSampler, __FILE__, __LINE__, "light sampler" );
		pLightSampler->Prepare( *pScene, pConcreteLumMgr->getLuminaries() );

		// Apply any pending light-sample RR threshold
		if( dPendingLightRRThreshold > 0 )
		{
			pLightSampler->SetLightSampleRRThreshold( dPendingLightRRThreshold );
		}

		// Build environment importance sampler if a global radiance map exists
		const IRadianceMap* pEnvMap = pScene->GetGlobalRadianceMap();
		if( pEnvMap )
		{
			EnvironmentSampler* pEnvSampler = new EnvironmentSampler(
				pEnvMap->GetPainter(),
				pEnvMap->GetScale(),
				pEnvMap->GetTransform(),
				64
				);
			GlobalLog()->PrintNew( pEnvSampler, __FILE__, __LINE__, "environment sampler" );
			pEnvSampler->Build();

			if( pEnvSampler->IsValid() )
			{
				pLightSampler->SetEnvironmentSampler( pEnvMap, pEnvSampler );
				GlobalLog()->PrintEasyEvent( "Environment importance sampler built successfully" );
			}
			else
			{
				GlobalLog()->PrintEasyWarning( "Environment map is black, importance sampling disabled" );
			}

			// LightSampler::SetEnvironmentSampler addrefs if valid, so
			// release our local reference.
			safe_release( pEnvSampler );
		}
	}
}


bool RayCaster::CastRay( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
			) const
{
	if( bIORStack ) {
		IORStack ior_stack( 1.0 );
		return CastRay( rc, rast, ray, c, rs, distance, pRadianceMap, &ior_stack );
	}

	return CastRay( rc, rast, ray, c, rs, distance, pRadianceMap, 0 );
}

bool RayCaster::CastRay( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
			const IORStack* const ior_stack						///< [in/out] Index of refraction stack
			) const
{
#ifdef ENABLE_MAX_RECURSION
	if( rs.depth > nMaxRecursions )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "FORCED RECURSION TERMINATION" );
#endif

		return false;
	}
#endif

	// Unbiased Russian roulette: decide before the expensive
	// intersection work, compensate the returned radiance after.
	Scalar rrCompensation = 1.0;
#ifdef ENABLE_RAYCASTER_RR
	if( rs.importance < RC_RR_THRESHOLD && rs.importance > 0 )
	{
		const Scalar pSurvive = rs.importance / RC_RR_THRESHOLD;
		if( rc.random.CanonicalRandom() >= pSurvive ) {
			return false;
		}
		rrCompensation = 1.0 / pSurvive;
	}
#endif

	bool bReturn = false;

	// Cast the ray into the scene
	RayIntersection	ri( ray, rast );
	ri.geometric.glossyFilterWidth = rs.glossyFilterWidth;
	pScene->GetObjects()->IntersectRay( ri, true, true, false );

	bool bHit = ri.geometric.bHit;

	if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
		if( ri.pMaterial && ri.pMaterial->GetEmitter() ) {
			bHit = bShowLuminaires;
		}
	}

	// ----------------------------------------------------------------
	// Medium transport: determine if the ray is traveling through a
	// participating medium and handle absorption/scattering.
	//
	// Resolution order (matching Cycles volume stack):
	//   1. Check innermost enclosing object (IOR stack top) for
	//      interior medium
	//   2. Fall back to scene's global medium
	//   3. No medium (vacuum) — skip medium transport entirely
	//
	// When a medium is present:
	//   a. Sample free-flight distance from the medium
	//   b. If scatter event occurs before surface hit:
	//      - NEE at scatter point (in-scattering)
	//      - Sample phase function for continuation direction
	//      - Recursively CastRay from scatter point
	//      - Return (skip surface shading)
	//   c. If surface hit through medium:
	//      - Apply transmittance to surface shading result
	// ----------------------------------------------------------------
	const IObject* pMediumObject = 0;
	const IMedium* pMedium = MediumTracking::GetCurrentMediumWithObject( ior_stack, pScene, pMediumObject );

	if( pMedium )
	{
		const Scalar maxDist = bHit ? ri.geometric.range : RISE_INFINITY;

		IndependentSampler mediumSampler( rc.random );
		bool scattered = false;
		const Scalar t_m = pMedium->SampleDistance( ray, maxDist, mediumSampler, scattered );

		if( scattered )
		{
			// Medium scatter event before surface hit.
			// Compute scatter point and evaluate in-scattering + continuation.
			//
			// Phase function convention: 'wo' is the travel direction of
			// the arriving photon (= ray.Dir()), NOT the toward-viewer
			// direction.  This matches Cycles' convention (which negates
			// sd->wi before passing to HG) and RISE's own BioSpec usage
			// (GenericHumanTissueSPF passes ri.ray.Dir() to SampleWithG).
			// For forward scattering (g > 0), Sample(wo) returns
			// directions close to wo — i.e., the photon continues
			// roughly in its original travel direction.
			const Point3 scatterPt = ray.PointAtLength( t_m );
			const Vector3 wo = ray.Dir();

			// Throughput: transmittance * sigma_s / sampling PDF.
			// For exponential sampling on sigma_t_max:
			//   PDF = sigma_t_max * exp(-sigma_t_max * t)
			//   Transmittance = exp(-sigma_t * t) per channel
			//   Net weight per channel = sigma_s / sigma_t_max
			//     (after canceling the exp terms)
			// This simplifies to the single-scattering albedo
			// times a channel correction.
			const MediumCoefficients coeff = pMedium->GetCoefficients( scatterPt );
			const RISEPel Tr = pMedium->EvalTransmittance( ray, t_m );
			const Scalar sigma_t_max = ColorMath::MaxValue( coeff.sigma_t );
			RISEPel throughput( 0, 0, 0 );
			if( sigma_t_max > 0 ) {
				// Per-channel weight: Tr[ch] * sigma_s[ch] / (sigma_t_max * T_scalar)
				// where T_scalar is the scalar tracking transmittance used by the
				// distance sampling PDF.  For delta tracking with majorant
				// sigma_t_majorant = MaxValue(max_sigma_t):
				//   T_scalar = exp(-sigma_t_majorant * integral_density)
				// Since Tr[ch] = exp(-max_sigma_t[ch] * integral_density), the
				// channel with the highest max_sigma_t gives the lowest Tr.
				// Therefore T_scalar = MinValue(Tr), which works correctly for
				// both homogeneous and heterogeneous media.
				const Scalar Tr_scalar = ColorMath::MinValue( Tr );
				if( Tr_scalar > 0 ) {
					throughput = Tr * coeff.sigma_s * (1.0 / (sigma_t_max * Tr_scalar));
				}
			}

			// 1. NEE at scatter point (in-scattering from lights)
			RISEPel Ld = MediumTransport::EvaluateInScattering(
				scatterPt, wo, pMedium, *this, pLightSampler,
				mediumSampler, rast, pMediumObject );

			// 2. Phase-function continuation (indirect in-scattering)
			// Volume bounces are bounded independently of the general
			// depth limit to prevent excessive scattering in dense media.
			static const unsigned int nMaxVolumeBounces = 64;
			const IPhaseFunction* pPhase = pMedium->GetPhaseFunction();
			RISEPel Li( 0, 0, 0 );
			Scalar phasePdf = 0;
			Vector3 wi( 0, 0, 0 );
			if( pPhase && rs.depth < nMaxRecursions &&
				rs.volumeBounces < nMaxVolumeBounces )
			{
				// Sample the continuation direction — optionally guided
				Scalar guidingMISWeight = 1.0;
				Scalar effectivePdf = 0;
				wi = pPhase->Sample( wo, mediumSampler );
				phasePdf = pPhase->Pdf( wo, wi );
				effectivePdf = phasePdf;

#ifdef RISE_ENABLE_OPENPGL
				// Volume guiding: one-sample MIS between guiding
				// distribution and phase function.
				if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
					rc.guidingAlpha > 0 &&
					rs.depth < rc.maxGuidingDepth )
				{
					static thread_local Implementation::GuidingVolumeDistributionHandle volGuideHandle;

					const Scalar alpha = rc.guidingAlpha;
					if( rc.pGuidingField->InitVolumeDistribution(
						volGuideHandle, scatterPt, mediumSampler.Get1D() ) )
					{
						// Apply HG product if the phase function is anisotropic
						const Scalar meanCosine = pPhase->GetMeanCosine();
						if( fabs( meanCosine ) > 1e-6 )
						{
							rc.pGuidingField->ApplyHGProduct(
								volGuideHandle, wo, meanCosine );
						}

						const Scalar xiG = mediumSampler.Get1D();
						if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
						{
							// Sample from guiding distribution.
							// Save phase-sampled state in case guide fails.
							const Vector3 wi_phase = wi;
							const Scalar phasePdf_phase = phasePdf;

							Scalar guidePdf = 0;
							const Point2 xi2D( mediumSampler.Get1D(), mediumSampler.Get1D() );
							wi = rc.pGuidingField->SampleVolume( volGuideHandle, xi2D, guidePdf );

							if( guidePdf > 0 )
							{
								phasePdf = pPhase->Pdf( wo, wi );
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, phasePdf );
								guidingMISWeight = phasePdf / combinedPdf;
								effectivePdf = combinedPdf;
							}
							else
							{
								// Degenerate guide sample — restore phase direction
								wi = wi_phase;
								phasePdf = phasePdf_phase;
							}
						}
						else
						{
							// Keep phase-sampled direction, but reweight for combined PDF
							const Scalar guidePdf = rc.pGuidingField->PdfVolume( volGuideHandle, wi );
							if( guidePdf > 0 && phasePdf > 0 )
							{
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, phasePdf );
								guidingMISWeight = phasePdf / combinedPdf;
								effectivePdf = combinedPdf;
							}
						}
					}
				}
#endif // RISE_ENABLE_OPENPGL

				const Ray scatterRay( scatterPt, wi );

				RAY_STATE rs2;
				rs2.depth = rs.depth + 1;
				rs2.importance = rs.importance * ColorMath::MaxValue( throughput ) * guidingMISWeight;
				rs2.considerEmission = true;
				rs2.type = rs.type;
				rs2.volumeBounces = rs.volumeBounces + 1;
				rs2.bsdfPdf = phasePdf;

				Scalar hitDist = 0;
				CastRay( rc, rast, scatterRay, Li, rs2, &hitDist,
					pRadianceMap, ior_stack );

#ifdef RISE_ENABLE_OPENPGL
				// Record volume training sample for the guiding field.
				// Use effectivePdf (= combinedPdf when guiding was applied)
				// so that weight = luminance / pdf matches the actual
				// sampling distribution used to generate the direction.
				if( rc.pGuidingField &&
					rc.pGuidingField->IsCollectingTrainingSamples() &&
					effectivePdf > NEARZERO )
				{
					const Scalar lum = ColorMath::MaxValue( Li );
					if( lum > 0 )
					{
						rc.pGuidingField->AddVolumeSample(
							scatterPt, wi,
							hitDist > 0 ? hitDist : 1.0,
							effectivePdf,
							lum,
							false );
					}
					else
					{
						rc.pGuidingField->AddZeroValueVolumeSample(
							scatterPt, wi );
					}
				}
#endif // RISE_ENABLE_OPENPGL

				Li = Li * guidingMISWeight;
			}

			// Combine: throughput * (Ld + Li) + emission
			c = throughput * (Ld + Li);

			// Volumetric emission contribution along segment [0, t_m].
			// The integral is: Le * integral_0^t Tr(0->s) ds
			//
			// For homogeneous media (constant sigma_t):
			//   = emission * (1 - exp(-sigma_t * t)) / sigma_t
			//
			// For heterogeneous media, sigma_t varies spatially.
			// We use the effective optical depth tau = -ln(Tr) from
			// the ray-marched transmittance and compute:
			//   emission * (1 - Tr) * t / tau
			// This is exact when sigma_t is constant along the segment
			// and a reasonable approximation otherwise.
			if( ColorMath::MaxValue( coeff.emission ) > 0 )
			{
				RISEPel emissionContrib( 0, 0, 0 );
				for( int ch = 0; ch < 3; ch++ )
				{
					if( Tr[ch] < 1.0 - 1e-10 )
					{
						// Use effective optical depth from ray-marched Tr
						const Scalar tau = -log( fmax( Tr[ch], 1e-30 ) );
						emissionContrib[ch] = coeff.emission[ch] *
							(1.0 - Tr[ch]) * t_m / tau;
					}
					else
					{
						// Nearly transparent: emission accumulates linearly
						emissionContrib[ch] = coeff.emission[ch] * t_m;
					}
				}
				c = c + emissionContrib;
			}

			if( distance ) {
				*distance = t_m;
			}

			// Apply RR compensation
			if( rrCompensation != 1.0 ) {
				c = c * rrCompensation;
			}

			return true;
		}
		// else: no scatter — ray passes through to surface or background.
		// Apply transmittance after shading below.
		//
		// Accumulate volumetric emission along the non-scatter segment.
		// Without this, purely absorptive emissive media (sigma_s = 0)
		// would never contribute emission since scatter events never occur.
		if( !scattered )
		{
			// Use midpoint of the segment for coefficient evaluation,
			// which is a better approximation than ray origin for
			// heterogeneous media where density varies spatially.
			const Scalar segDist = bHit ? ri.geometric.range : Scalar(1000.0);
			const Point3 midPt = ray.PointAtLength( segDist * 0.5 );
			const MediumCoefficients coeff = pMedium->GetCoefficients( midPt );
			if( ColorMath::MaxValue( coeff.emission ) > 0 )
			{
				// Use ray-marched transmittance for the emission integral
				// to handle heterogeneous extinction correctly.
				const RISEPel Tr_seg = pMedium->EvalTransmittance( ray, segDist );
				RISEPel emissionContrib( 0, 0, 0 );
				for( int ch = 0; ch < 3; ch++ )
				{
					if( Tr_seg[ch] < 1.0 - 1e-10 )
					{
						// Effective optical depth from ray-marched Tr
						const Scalar tau = -log( fmax( Tr_seg[ch], 1e-30 ) );
						emissionContrib[ch] = coeff.emission[ch] *
							(1.0 - Tr_seg[ch]) * segDist / tau;
					}
					else
					{
						emissionContrib[ch] = coeff.emission[ch] * segDist;
					}
				}
				c = c + emissionContrib;
			}
		}
	}

	if( bHit )
	{
		// If there is an intersection modifier, then get it to modify
		// the intersection information
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Set the current object on the IOR stack
		if( ior_stack ) {
			ior_stack->SetCurrentObject( ri.pObject );
		}

		// Apply shade by calling the appropriate shader
		if( ri.pShader ) {
			ri.pShader->Shade( rc, ri, *this, rs, c, ior_stack );
		} else {
			pDefaultShader.Shade( rc, ri, *this, rs, c, ior_stack );
		}

		// Apply medium transmittance to surface shading result
		if( pMedium ) {
			c = c * pMedium->EvalTransmittance( ray, ri.geometric.range );
		}

		if( distance ) {
			*distance = ri.geometric.range;
		}

		bReturn = true;
	} else if( pRadianceMap ) {
		c = pRadianceMap->GetRadiance( ray, rast );

		// Apply medium transmittance for background
		if( pMedium ) {
			c = c * pMedium->EvalTransmittance( ray, RISE_INFINITY );
		}
	} else if( pScene->GetGlobalRadianceMap() ) {
		c = pScene->GetGlobalRadianceMap()->GetRadiance( ray, rast );

		// Apply MIS weight for BSDF-sampled environment hit vs env NEE
		if( pLightSampler && rs.bsdfPdf > 0 )
		{
			const EnvironmentSampler* pES = pLightSampler->GetEnvironmentSampler();
			if( pES )
			{
				const Scalar envPdf = pES->Pdf( ray.Dir() );
				if( envPdf > 0 )
				{
					const Scalar bsdfPdf2 = rs.bsdfPdf * rs.bsdfPdf;
					const Scalar w_bsdf = bsdfPdf2 / (bsdfPdf2 + envPdf * envPdf);
					c = c * w_bsdf;
				}
			}
		}

		// Apply medium transmittance for environment
		if( pMedium ) {
			c = c * pMedium->EvalTransmittance( ray, RISE_INFINITY );
		}

		if( distance && bConsiderRMapAsBackground ) {
			*distance = RISE_INFINITY;
		}

		bReturn = bConsiderRMapAsBackground;
	}

	// Apply RR compensation to the returned radiance so the caller's
	// estimator (throughput * c) remains unbiased.
	if( rrCompensation != 1.0 ) {
		c = c * rrCompensation;
	}

	return bReturn;
}

//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
bool RayCaster::CastRayNM( 
	const RuntimeContext& rc,							///< [in] The runtime context
	const RasterizerState& rast,						///< [in] Current state of the rasterizer
	const Ray& ray,										///< [in] Ray to cast
	Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
	const RAY_STATE& rs,								///< [in] The ray state
	const Scalar nm,									///< [in] Wavelength to cast
	Scalar* distance,									///< [in] If there was a hit, how far?
	const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
	) const
{
	if( bIORStack ) {
		IORStack ior_stack( 1.0 );
		return CastRayNM( rc, rast, ray, c, rs, nm, distance, pRadianceMap, &ior_stack );
	}

	return CastRayNM( rc, rast, ray, c, rs, nm, distance, pRadianceMap, 0 );
}

//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
bool RayCaster::CastRayNM( 
    const RuntimeContext& rc,							///< [in] The runtime context
	const RasterizerState& rast,						///< [in] Current state of the rasterizer
	const Ray& ray,										///< [in] Ray to cast
	Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
	const RAY_STATE& rs,								///< [in] The ray state
	const Scalar nm,									///< [in] Wavelength to cast
	Scalar* distance,									///< [in] If there was a hit, how far?
	const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
	const IORStack* const ior_stack						///< [in/out] Index of refraction stack
	) const
{
#ifdef ENABLE_MAX_RECURSION
	if( rs.depth > nMaxRecursions )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "FORCED RECURSION TERMINATION" );
#endif
		return false;
	}
#endif

	// Unbiased Russian roulette: decide before the expensive
	// intersection work, compensate the returned radiance after.
	Scalar rrCompensation = 1.0;
#ifdef ENABLE_RAYCASTER_RR
	if( rs.importance < RC_RR_THRESHOLD && rs.importance > 0 )
	{
		const Scalar pSurvive = rs.importance / RC_RR_THRESHOLD;
		if( rc.random.CanonicalRandom() >= pSurvive ) {
			return false;
		}
		rrCompensation = 1.0 / pSurvive;
	}
#endif

	// Cast the ray into the scene
	RayIntersection	ri( ray, rast );
	ri.geometric.glossyFilterWidth = rs.glossyFilterWidth;
	pScene->GetObjects()->IntersectRay( ri, true, true, false );

	bool bHit = ri.geometric.bHit;

	if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
		if( ri.pMaterial && ri.pMaterial->GetEmitter() ) {
			bHit = bShowLuminaires;
		}
	}

	bool bReturn = false;

	// Medium transport (spectral variant)
	const IObject* pMediumObject = 0;
	const IMedium* pMedium = MediumTracking::GetCurrentMediumWithObject( ior_stack, pScene, pMediumObject );

	if( pMedium )
	{
		const Scalar maxDist = bHit ? ri.geometric.range : RISE_INFINITY;

		IndependentSampler mediumSampler( rc.random );
		bool scattered = false;
		const Scalar t_m = pMedium->SampleDistanceNM( ray, maxDist, nm, mediumSampler, scattered );

		if( scattered )
		{
			// Phase function convention: wo = travel direction (see RGB path comment)
			const Point3 scatterPt = ray.PointAtLength( t_m );
			const Vector3 wo = ray.Dir();

			const MediumCoefficientsNM coeff = pMedium->GetCoefficientsNM( scatterPt, nm );
			const Scalar Tr = pMedium->EvalTransmittanceNM( ray, t_m, nm );
			Scalar throughput = 0;
			if( coeff.sigma_t > 0 ) {
				// For single-channel exponential sampling:
				//   PDF = sigma_t * exp(-sigma_t * t)
				//   Transmittance = exp(-sigma_t * t)
				//   Net weight = sigma_s / sigma_t (single-scattering albedo)
				throughput = coeff.sigma_s / coeff.sigma_t;
			}

			// NEE at scatter point
			Scalar Ld = MediumTransport::EvaluateInScatteringNM(
				scatterPt, wo, pMedium, nm, *this, pLightSampler,
				mediumSampler, rast, pMediumObject );

			// Phase-function continuation
			static const unsigned int nMaxVolumeBounces = 64;
			const IPhaseFunction* pPhase = pMedium->GetPhaseFunction();
			Scalar Li = 0;
			Scalar phasePdf = 0;
			Vector3 wi( 0, 0, 0 );
			if( pPhase && rs.depth < nMaxRecursions &&
				rs.volumeBounces < nMaxVolumeBounces )
			{
				Scalar guidingMISWeight = 1.0;
				Scalar effectivePdf = 0;
				wi = pPhase->Sample( wo, mediumSampler );
				phasePdf = pPhase->Pdf( wo, wi );
				effectivePdf = phasePdf;

#ifdef RISE_ENABLE_OPENPGL
				// Volume guiding (spectral): one-sample MIS
				if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
					rc.guidingAlpha > 0 &&
					rs.depth < rc.maxGuidingDepth )
				{
					static thread_local Implementation::GuidingVolumeDistributionHandle volGuideHandleNM;

					const Scalar alpha = rc.guidingAlpha;
					if( rc.pGuidingField->InitVolumeDistribution(
						volGuideHandleNM, scatterPt, mediumSampler.Get1D() ) )
					{
						// Apply HG product if the phase function is anisotropic
						const Scalar meanCosine = pPhase->GetMeanCosine();
						if( fabs( meanCosine ) > 1e-6 )
						{
							rc.pGuidingField->ApplyHGProduct(
								volGuideHandleNM, wo, meanCosine );
						}

						const Scalar xiG = mediumSampler.Get1D();
						if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
						{
							// Save phase-sampled state in case guide fails
							const Vector3 wi_phase = wi;
							const Scalar phasePdf_phase = phasePdf;

							Scalar guidePdf = 0;
							const Point2 xi2D( mediumSampler.Get1D(), mediumSampler.Get1D() );
							wi = rc.pGuidingField->SampleVolume( volGuideHandleNM, xi2D, guidePdf );

							if( guidePdf > 0 )
							{
								phasePdf = pPhase->Pdf( wo, wi );
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, phasePdf );
								guidingMISWeight = phasePdf / combinedPdf;
								effectivePdf = combinedPdf;
							}
							else
							{
								// Degenerate guide sample — restore phase direction
								wi = wi_phase;
								phasePdf = phasePdf_phase;
							}
						}
						else
						{
							const Scalar guidePdf = rc.pGuidingField->PdfVolume( volGuideHandleNM, wi );
							if( guidePdf > 0 && phasePdf > 0 )
							{
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, phasePdf );
								guidingMISWeight = phasePdf / combinedPdf;
								effectivePdf = combinedPdf;
							}
						}
					}
				}
#endif // RISE_ENABLE_OPENPGL

				const Ray scatterRay( scatterPt, wi );

				RAY_STATE rs2;
				rs2.depth = rs.depth + 1;
				rs2.importance = rs.importance * throughput * guidingMISWeight;
				rs2.considerEmission = true;
				rs2.type = rs.type;
				rs2.volumeBounces = rs.volumeBounces + 1;
				rs2.bsdfPdf = phasePdf;

				Scalar hitDist = 0;
				CastRayNM( rc, rast, scatterRay, Li, rs2, nm, &hitDist,
					pRadianceMap, ior_stack );

#ifdef RISE_ENABLE_OPENPGL
				// Record volume training sample (spectral path).
				// Use effectivePdf (= combinedPdf when guiding was applied)
				// so that weight = luminance / pdf matches the actual
				// sampling distribution.
				if( rc.pGuidingField &&
					rc.pGuidingField->IsCollectingTrainingSamples() &&
					effectivePdf > NEARZERO )
				{
					if( Li > 0 )
					{
						rc.pGuidingField->AddVolumeSample(
							scatterPt, wi,
							hitDist > 0 ? hitDist : 1.0,
							effectivePdf,
							Li,
							false );
					}
					else
					{
						rc.pGuidingField->AddZeroValueVolumeSample(
							scatterPt, wi );
					}
				}
#endif // RISE_ENABLE_OPENPGL

				Li = Li * guidingMISWeight;
			}

			c = throughput * (Ld + Li);

			// Volumetric emission: use effective optical depth
			if( coeff.emission > 0 )
			{
				if( Tr < 1.0 - 1e-10 )
				{
					const Scalar tau = -log( fmax( Tr, 1e-30 ) );
					c += coeff.emission * (1.0 - Tr) * t_m / tau;
				}
				else
				{
					c += coeff.emission * t_m;
				}
			}

			if( distance ) {
				*distance = t_m;
			}

			if( rrCompensation != 1.0 ) {
				c = c * rrCompensation;
			}

			return true;
		}
		// Non-scatter path: accumulate volumetric emission along segment
		if( !scattered )
		{
			const Scalar segDist = bHit ? ri.geometric.range : Scalar(1000.0);
			const Point3 midPt = ray.PointAtLength( segDist * 0.5 );
			const MediumCoefficientsNM coeff = pMedium->GetCoefficientsNM( midPt, nm );
			if( coeff.emission > 0 )
			{
				const Scalar Tr_seg = pMedium->EvalTransmittanceNM( ray, segDist, nm );
				if( Tr_seg < 1.0 - 1e-10 )
				{
					const Scalar tau = -log( fmax( Tr_seg, 1e-30 ) );
					c += coeff.emission * (1.0 - Tr_seg) * segDist / tau;
				}
				else
				{
					c += coeff.emission * segDist;
				}
			}
		}
	}

	if( bHit ) {
		// If there is an intersection modifier, then get it to modify
		// the intersection information
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Set the current object on the IOR stack
		if( ior_stack ) {
			ior_stack->SetCurrentObject( ri.pObject );
		}

		// Apply shade by calling the appropriate shader
		if( ri.pShader ) {
			c = ri.pShader->ShadeNM( rc, ri, *this, rs, nm, ior_stack );
		} else {
			c = pDefaultShader.ShadeNM( rc, ri, *this, rs, nm, ior_stack );
		}

		// Apply medium transmittance to surface shading result
		if( pMedium ) {
			c *= pMedium->EvalTransmittanceNM( ray, ri.geometric.range, nm );
		}

		if( distance ) {
			*distance = ri.geometric.range;
		}

		bReturn = true;
	} else if( pRadianceMap ) {
		c = pRadianceMap->GetRadianceNM( ray, rast, nm );

		if( pMedium ) {
			c *= pMedium->EvalTransmittanceNM( ray, RISE_INFINITY, nm );
		}
	} else if( pScene->GetGlobalRadianceMap() ) {
		c = pScene->GetGlobalRadianceMap()->GetRadianceNM( ray, rast, nm );

		// Apply MIS weight for BSDF-sampled environment hit vs env NEE
		if( pLightSampler && rs.bsdfPdf > 0 )
		{
			const EnvironmentSampler* pES = pLightSampler->GetEnvironmentSampler();
			if( pES )
			{
				const Scalar envPdf = pES->Pdf( ray.Dir() );
				if( envPdf > 0 )
				{
					const Scalar bsdfPdf2 = rs.bsdfPdf * rs.bsdfPdf;
					const Scalar w_bsdf = bsdfPdf2 / (bsdfPdf2 + envPdf * envPdf);
					c = c * w_bsdf;
				}
			}
		}

		if( pMedium ) {
			c *= pMedium->EvalTransmittanceNM( ray, RISE_INFINITY, nm );
		}

		if( distance && bConsiderRMapAsBackground ) {
			*distance = RISE_INFINITY;
		}

		bReturn = bConsiderRMapAsBackground;
	}

	// Apply RR compensation to the returned radiance so the caller's
	// estimator (throughput * c) remains unbiased.
	if( rrCompensation != 1.0 ) {
		c = c * rrCompensation;
	}

	return bReturn;
}

bool RayCaster::CastShadowRay( const Ray& ray, const Scalar dHowFar ) const
{
	if( !pScene ) {
		GlobalLog()->PrintSourceError( "RayCaster::CastRay_IntersectionOnly:: No scene", __FILE__, __LINE__ );
		return false;
	}
	
	return pScene->GetObjects()->IntersectShadowRay( ray, dHowFar, true, true );
}

void RayCaster::SetRISCandidates( const unsigned int M )
{
	if( pLightSampler )
	{
		pLightSampler->SetRISCandidates( M );
	}
}

void RayCaster::SetLightSampleRRThreshold( const Scalar threshold )
{
	dPendingLightRRThreshold = threshold;
	if( pLightSampler )
	{
		pLightSampler->SetLightSampleRRThreshold( threshold );
	}
}

//! Sets the luminaire sampler
void RayCaster::SetLuminaireSampling(
	ISampling2D* pLumSam							///< [in] Kernel to use for luminaire sampling
	)
{
	safe_release( pLumSampling );

	if( pLumSam ) {
		pLumSampling = pLumSam;
		pLumSampling->addref();
	}
}

