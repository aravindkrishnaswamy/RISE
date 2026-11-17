//////////////////////////////////////////////////////////////////////
//
//  DistributionTracingShaderOp.cpp - Implementation of the DistributionTracingShaderOp class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 28, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DistributionTracingShaderOp.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/IndependentSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

DistributionTracingShaderOp::DistributionTracingShaderOp(
	const unsigned int numSamples_,
	const bool irradiancecaching,
	const bool forcecheckemitters,
	const bool reflections,
	const bool refractions,
	const bool diffuse,
	const bool translucents
	) :
  numSamples( numSamples_ ),
  dOVNumSamples( 1 ),
  bUseIrradianceCache( irradiancecaching ),
  bForceCheckEmitters( forcecheckemitters ),
  bTraceReflection( reflections ),
  bTraceRefraction( refractions ),
  bTraceDiffuse( diffuse ),
  bTraceTranslucent( translucents )
{
	if( numSamples > 1 ) {
		dOVNumSamples = 1.0 / Scalar(numSamples);
	}
}

DistributionTracingShaderOp::~DistributionTracingShaderOp( )
{
}

//////////////////////////////////////////////////////////////////////
// DL-171: the legacy shader-op chain's own MIS partner.
//
// `EmissionShaderOp` weighs a BSDF-sampled emitter hit against
// `RAY_STATE::MisPartnerPdf()`; `DirectLightingShaderOp` ->
// `LightSampler::EvaluateDirectLighting{,NM}` weighs its NEE sample
// against the material's AGGREGATE `Pdf()`/`PdfNM()` -- the SAME
// function this op's own continuation must stamp onto `rs2.bsdfPdf`/
// `bsdfMisPdf`, or the pair does not partition to one (DL-171: UNDER
// where `considerEmission` suppresses the emitter-hit strategy, OVER
// where it doesn't).  A delta lobe has no distribution for that
// aggregate to describe, so its partner stays 0 (DL-74's rule) --
// `EmissionShaderOp` already takes weight 1 there and `LightSampler`'s
// own arms see `Pdf()==0` at a delta-only material and do the same;
// see `ReflectionShaderOp`/`RefractionShaderOp`'s identical guard for
// the sibling materials (e.g. GGX's rough reflection lobe) that are
// NOT delta despite carrying `eRayReflection`/`eRayRefraction`.
static Scalar LegacyChainMisPartner(
	const ISPF& spf,
	const RayIntersectionGeometric& ri,
	const ScatteredRay& scat,
	const IORStack& ior_stack
	)
{
	if( scat.isDelta ) {
		return 0;
	}
	const Scalar aggregatePdf = spf.Pdf( ri, scat.ray.Dir(), ior_stack );
	// DL-41 guard (see DL-103/DL-69's fuller derivation): a zero
	// aggregate at a direction this lobe really did generate means "no
	// partner" on BOTH sides, which is a full double count -- fall back
	// to the selected lobe's own density instead, reproducing the
	// pre-DL-171 (no-partner) behaviour only at those SPFs.
	// (DL-41 itself closed 2026-09-18 -- `TranslucentSPF`, the one
	// documented inhabitant, now covers all of its lobes -- so this is a
	// guard against a future SPF rather than a live workaround.)
	return aggregatePdf > 0 ? aggregatePdf : scat.pdf;
}

static Scalar LegacyChainMisPartnerNM(
	const ISPF& spf,
	const RayIntersectionGeometric& ri,
	const ScatteredRay& scat,
	const Scalar nm,
	const IORStack& ior_stack
	)
{
	if( scat.isDelta ) {
		return 0;
	}
	const Scalar aggregatePdf = spf.PdfNM( ri, scat.ray.Dir(), nm, ior_stack );
	return aggregatePdf > 0 ? aggregatePdf : scat.pdf;
}

bool DistributionTracingShaderOp::ShouldTraceRay( const ScatteredRay::ScatRayType type ) const
{
	if( (type == ScatteredRay::eRayReflection && !bTraceReflection) ||
		(type == ScatteredRay::eRayRefraction && !bTraceRefraction) ||
		(type == ScatteredRay::eRayDiffuse && !bTraceDiffuse) ||
		(type == ScatteredRay::eRayTranslucent && !bTraceTranslucent)
		)
	{
		return false;
	}

	return true;
}

//! Tells the shader to apply shade to the given intersection point
void DistributionTracingShaderOp::PerformOperation(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [in/out] Resultant color from op
	const IORStack& ior_stack,			///< [in] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	c = RISEPel(0.0);

	const IScene* pScene = caster.GetAttachedScene();
	const ISPF* pSPF = ri.pMaterial->GetSPF();

	if( pScene && pSPF ) {

		bool bComputeIrradiance = true;
		const IIrradianceCache* pCache = pScene->GetIrradianceCache();

		// If we are to use irradiance caching and we are in a normal pass, look it up
		if( bUseIrradianceCache && pCache && pCache->GetTolerance() > 0 && rc.IsNormalShadingPass() ) {
			// Look it up
			std::vector<IIrradianceCache::CacheElement> results;
            // Cache key uses GEOMETRIC normal — see FinalGatherShaderOp
            // for rationale.
            const Scalar weights = pCache->Query(ri.geometric.ptIntersection, ri.geometric.vGeomNormal, results);

			if( results.size() > 0 ) {
				// There were some results, so accrue
				std::vector<IIrradianceCache::CacheElement>::const_iterator i;
				for( i=results.begin(); i!=results.end(); i++ ) {
					const IIrradianceCache::CacheElement& elem = *i;
					c = c + (elem.cIRad * r_min(1e10,elem.dWeight));
				}

				c = c * ((1.0/weights));
				bComputeIrradiance = false;
			}
		}

		// If we are using irradiance cache pass
		if( rc.pass == RuntimeContext::PASS_IRRADIANCE_CACHE ) {
			// And we are using irradiance caching
			if( (bUseIrradianceCache && pCache) || (rs.type == IRayCaster::RAY_STATE::eRayFinalGather) ) {
				// Check the cache to see if we should generate a sample here
				bComputeIrradiance = pCache->IsSampleNeeded( ri.geometric.ptIntersection, ri.geometric.vGeomNormal );
			} else {
				bComputeIrradiance = false;
			}
		}

		if( bComputeIrradiance ) {
			RISEPel	accruedIndirect(0,0,0);

			Scalar rsum = 0;
			unsigned int hits = 0;

			for( unsigned int i=0; i<numSamples; i++ )
			{
				IRayCaster::RAY_STATE rs2;
				rs2.type = rs.eRayFinalGather;
				rs2.depth = rs.depth+1;
				// DL-171 ruling: the pre-MIS "suppress emission whenever
				// the source material has a BSDF" hack is RETIRED -- now
				// that this continuation stamps a real MIS partner
				// (below), the BSDF-hit strategy and NEE partition
				// properly instead of needing one of them silenced.
				// `bForceCheckEmitters` stays wired (a scene can still set
				// it) but is now a NO-OP for that removed clause; it
				// remains meaningful only for the orthogonal caustic-map
				// suppression below (a scene relying on a caustic photon
				// map for this transport, not on the BSDF-vs-NEE MIS
				// pair, still wants that suppressed).
				if( bForceCheckEmitters ) {
					rs2.considerEmission = true;
				} else {
					rs2.considerEmission = (caster.GetAttachedScene()->GetCausticSpectralMap() && !rs.considerEmission) ? false : true;
				}

				Scalar t = 0;

				ScatteredRayContainer scattered;
				{
					IndependentSampler fallbackSampler( rc.random );
					ISampler& scatterSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
					pSPF->Scatter( ri.geometric, scatterSampler, scattered, ior_stack );
				}

				if( scattered.Count() > 1 ) {
					// Multiple rays: trace all to avoid high-variance correction
					for( unsigned int i=0; i<scattered.Count(); i++ ) {
						ScatteredRay& scat = scattered[i];
						if( ShouldTraceRay( scat.type ) ) {
							scat.ray.Advance( 1e-8 );
							// eta^2 basic-radiance factor (debt 30): this op
							// gathers RADIANCE from the eye side, so a lobe
							// that changes medium scales by
							// (eta_before/eta_after)^2.  Identically 1 for
							// every lobe that does not.
							const Scalar etaScale = RadianceEtaScale( ior_stack, scat.ior_stack );
							rs2.importance = rs.importance * ColorMath::MaxValue(scat.kray) * etaScale;
							// DL-171: this continuation's own MIS partner --
							// see `LegacyChainMisPartner`'s doc above.
							// DL-209: 0 (no partner -> full weight) when
							// THIS shader has no DirectLightingShaderOp
							// sibling to compete against -- see
							// RAY_STATE::chainHasNEEOp's doc.
							rs2.bsdfPdf = rs.chainHasNEEOp ?
								LegacyChainMisPartner( *pSPF, ri.geometric, scat, ior_stack ) : Scalar( 0 );
							rs2.bsdfMisPdf = rs2.bsdfPdf;
							RISEPel	cThisIndirectSample(0,0,0);
							if( caster.CastRay( rc, ri.geometric.rast, scat.ray, cThisIndirectSample, rs2, &t, ri.pRadianceMap, scat.ior_stack ? *scat.ior_stack : ior_stack ) ) {
								rsum += 1.0/t;
								hits++;
							}
							accruedIndirect = accruedIndirect + cThisIndirectSample * scat.kray * etaScale;
						}
					}
				} else {
					ScatteredRay* pScatRay = scattered.RandomlySelect( rc.random.CanonicalRandom(), false );
					if( pScatRay && ShouldTraceRay( pScatRay->type ) ) {
						pScatRay->ray.Advance( 1e-8 );
						const Scalar etaScale = RadianceEtaScale( ior_stack, pScatRay->ior_stack );
						rs2.importance = rs.importance * ColorMath::MaxValue(pScatRay->kray) * etaScale;
						// DL-171/DL-209: see the multi-ray branch above.
						rs2.bsdfPdf = rs.chainHasNEEOp ?
							LegacyChainMisPartner( *pSPF, ri.geometric, *pScatRay, ior_stack ) : Scalar( 0 );
						rs2.bsdfMisPdf = rs2.bsdfPdf;
						RISEPel	cThisIndirectSample(0,0,0);
						if( caster.CastRay( rc, ri.geometric.rast, pScatRay->ray, cThisIndirectSample, rs2, &t, ri.pRadianceMap, pScatRay->ior_stack ? *pScatRay->ior_stack : ior_stack ) ) {
							rsum += 1.0/t;
							hits++;
						}
						accruedIndirect = accruedIndirect + cThisIndirectSample * pScatRay->kray * etaScale;
					}
				}
			}

			// Divide it out
			c = accruedIndirect * dOVNumSamples;

			// Store it in the irradiance cache if it exists
			if( bUseIrradianceCache && pCache && pCache->GetTolerance() > 0 && rc.pass == RuntimeContext::PASS_IRRADIANCE_CACHE ) {
				if( rsum ) {
					rsum = Scalar(hits)/rsum;
				} else {
					rsum = 0;
				}

				// Add the indirect value to the cache
				// Cache record uses GEOMETRIC normal — paired with Query
				// and IsSampleNeeded above.
				pCache->InsertElement( ri.geometric.ptIntersection, ri.geometric.vGeomNormal, c, rsum, 0, 0 );

				c = RISEPel(0.7,0.7,0); // shows yellow when a new cache value is computed
			}
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function
Scalar DistributionTracingShaderOp::PerformOperationNM(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar caccum,						///< [in] Current value for wavelength
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack& ior_stack,			///< [in] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	Scalar c=0;

	// Only do stuff on a normal pass or on final gather
	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return 0;
	}

	const IScene* pScene = caster.GetAttachedScene();
	const ISPF* pSPF = ri.pMaterial->GetSPF();

	if( pScene && pSPF ) {
		for( unsigned int i=0; i<numSamples; i++ )
		{
			IRayCaster::RAY_STATE rs2;
			rs2.depth = rs.depth+1;
			rs2.considerEmission = (pScene->GetCausticSpectralMap())?false:true;

			ScatteredRayContainer scattered;
			{
				IndependentSampler fallbackSampler( rc.random );
				ISampler& scatterSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
				pSPF->ScatterNM( ri.geometric, scatterSampler, nm, scattered, ior_stack );
			}

			if( scattered.Count() > 1 ) {
				for( unsigned int i=0; i<scattered.Count(); i++ ) {
					ScatteredRay& scat = scattered[i];
					if( ShouldTraceRay( scat.type ) ) {
						scat.ray.Advance( 1e-8 );
						// Same eta^2 factor as the Pel twin above (debt 30).
						const Scalar etaScale = RadianceEtaScale( ior_stack, scat.ior_stack );
						rs2.importance = rs.importance * scat.krayNM * etaScale;
						// DL-171: this continuation's own MIS partner AT
						// THIS WAVELENGTH -- see `LegacyChainMisPartnerNM`'s
						// doc above `LegacyChainMisPartner`.
						// DL-209 -- see the RGB twin's identical gate.
						rs2.bsdfPdf = rs.chainHasNEEOp ?
							LegacyChainMisPartnerNM( *pSPF, ri.geometric, scat, nm, ior_stack ) : Scalar( 0 );
						rs2.bsdfMisPdf = rs2.bsdfPdf;
						Scalar	cThisIndirectSample = 0;
						caster.CastRayNM( rc, ri.geometric.rast, scat.ray, cThisIndirectSample, rs2, nm, 0, ri.pRadianceMap, scat.ior_stack ? *scat.ior_stack : ior_stack );
						c = c + cThisIndirectSample * scat.krayNM * etaScale;
					}
				}
			} else {
				ScatteredRay* pScatRay = scattered.RandomlySelect( rc.random.CanonicalRandom(), true );
				if( pScatRay && ShouldTraceRay(pScatRay->type) ) {
					pScatRay->ray.Advance( 1e-8 );
					const Scalar etaScale = RadianceEtaScale( ior_stack, pScatRay->ior_stack );
					rs2.importance = rs.importance * pScatRay->krayNM * etaScale;
					// DL-171/DL-209 -- see the multi-ray branch above.
					rs2.bsdfPdf = rs.chainHasNEEOp ?
						LegacyChainMisPartnerNM( *pSPF, ri.geometric, *pScatRay, nm, ior_stack ) : Scalar( 0 );
					rs2.bsdfMisPdf = rs2.bsdfPdf;
					Scalar	cThisIndirectSample = 0;
					caster.CastRayNM( rc, ri.geometric.rast, pScatRay->ray, cThisIndirectSample, rs2, nm, 0, ri.pRadianceMap, pScatRay->ior_stack ? *pScatRay->ior_stack : ior_stack );
					c = c + cThisIndirectSample * pScatRay->krayNM * etaScale;
				}
			}
		}

		// Divide it out
		c = c * dOVNumSamples;
	}

	return c;
}
