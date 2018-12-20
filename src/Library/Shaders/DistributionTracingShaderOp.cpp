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

using namespace RISE;
using namespace RISE::Implementation;

DistributionTracingShaderOp::DistributionTracingShaderOp(
	const unsigned int numSamples_,
	const bool irradiancecaching,
	const bool forcecheckemitters,
	const bool branch,
	const bool reflections,
	const bool refractions,
	const bool diffuse,
	const bool translucents
	) :
  numSamples( numSamples_ ),
  dOVNumSamples( 1 ),
  bUseIrradianceCache( irradiancecaching ),
  bForceCheckEmitters( forcecheckemitters ),
  bBranch( branch ),
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
	const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	c = RISEPel(0.0);

	const IScene* pScene = caster.GetAttachedScene();
	const ISPF* pSPF = ri.pMaterial->GetSPF();

	if( pScene && pSPF ) {

		bool bComputeIrradiance = true;
		IIrradianceCache* pCache = pScene->GetIrradianceCache();

		// If we are to use irradiance caching and we are in a normal pass, look it up
		if( bUseIrradianceCache && pCache && pCache->GetTolerance() > 0 && rc.pass == RuntimeContext::PASS_NORMAL ) {
			// Look it up
			std::vector<IIrradianceCache::CacheElement> results;
            const Scalar weights = pCache->Query(ri.geometric.ptIntersection, ri.geometric.vNormal, results);

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
				bComputeIrradiance = pCache->IsSampleNeeded( ri.geometric.ptIntersection, ri.geometric.vNormal );
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
				if( bForceCheckEmitters ) {
					rs2.considerEmission = true;
				} else {
					rs2.considerEmission = ((ri.pMaterial->GetBSDF())||(caster.GetAttachedScene()->GetCausticSpectralMap()&&!rs.considerEmission))?false:true;
				}

				Scalar t = 0;

				ScatteredRayContainer scattered;
				pSPF->Scatter( ri.geometric, rc.random, scattered, ior_stack );

				if( bBranch ) {
					// Branch
					int numshot=0;
					for( unsigned int i=0; i<scattered.Count(); i++ ) {
						ScatteredRay& scat = scattered[i];

						if( ShouldTraceRay( scat.type ) ) {
							scat.ray.Advance( 1e-8 );
							rs2.importance = rs.importance * ColorMath::MaxValue(scat.kray);

							RISEPel	cThisIndirectSample(0,0,0);
							if( caster.CastRay( rc, ri.geometric.rast, scat.ray, cThisIndirectSample, rs2, &t, ri.pRadianceMap, scat.ior_stack?scat.ior_stack:ior_stack ) ) {
								rsum += 1.0/t;
								hits++;
							}

							numshot++;
							accruedIndirect = accruedIndirect + cThisIndirectSample * scat.kray;
						}
					}
					c = c * (1.0/numshot);
				} else {
					// No branching, process all the rays
					ScatteredRay* pScatRay = scattered.RandomlySelect( rc.random.CanonicalRandom(), false );

					if( pScatRay && ShouldTraceRay( pScatRay->type ) ) {
						pScatRay->ray.Advance( 1e-8 );
						rs2.importance = rs.importance * ColorMath::MaxValue(pScatRay->kray);

						RISEPel	cThisIndirectSample(0,0,0);
						if( caster.CastRay( rc, ri.geometric.rast, pScatRay->ray, cThisIndirectSample, rs2, &t, ri.pRadianceMap, pScatRay->ior_stack?pScatRay->ior_stack:ior_stack ) ) {
							rsum += 1.0/t;
							hits++;
						}

						accruedIndirect = accruedIndirect + cThisIndirectSample * pScatRay->kray;
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
				pCache->InsertElement( ri.geometric.ptIntersection, ri.geometric.vNormal, c, rsum, 0, 0 );

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
	const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	Scalar c=0;

	// Only do stuff on a normal pass or on final gather
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
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
			pSPF->Scatter( ri.geometric, rc.random, scattered, ior_stack );

			if( bBranch ) {
				// Branch
				int numshot=0;
				for( unsigned int i=0; i<scattered.Count(); i++ ) {
					ScatteredRay& scat = scattered[i];

					if( ShouldTraceRay( scat.type ) ) {
						scat.ray.Advance( 1e-8 );
						rs2.importance = rs.importance * scat.krayNM;

						Scalar	cThisIndirectSample = 0;
						caster.CastRayNM( rc, ri.geometric.rast, scat.ray, cThisIndirectSample, rs2, nm, 0, ri.pRadianceMap, scat.ior_stack?scat.ior_stack:ior_stack );

						c = c + cThisIndirectSample * scat.krayNM;
						numshot++;
					}
				}
				c /= Scalar(numshot);
			} else {
				// No branching, randomly select a ray
				ScatteredRay* pScatRay = scattered.RandomlySelect( rc.random.CanonicalRandom(), true );
				if( pScatRay && ShouldTraceRay(pScatRay->type) ) {
					pScatRay->ray.Advance( 1e-8 );
					rs2.importance = rs.importance * pScatRay->krayNM;

					Scalar	cThisIndirectSample = 0;
					caster.CastRayNM( rc, ri.geometric.rast, pScatRay->ray, cThisIndirectSample, rs2, nm, 0, ri.pRadianceMap, pScatRay->ior_stack?pScatRay->ior_stack:ior_stack );

					c = c + cThisIndirectSample * pScatRay->krayNM;
				}
			}
		}

		// Divide it out
		c = c * dOVNumSamples;
	}

	return c;
}
