//////////////////////////////////////////////////////////////////////
//
//  AmbientOcclusionShaderOp.cpp - Implementation of the AmbientOcclusionShaderOp class
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
#include "AmbientOcclusionShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

AmbientOcclusionShaderOp::AmbientOcclusionShaderOp(
	const unsigned int numThetaSamples_,
	const unsigned int numPhiSamples_,
	const bool bMultiplyBRDF_,
	const bool bUseIrradianceCache_
	) :
  numThetaSamples( numThetaSamples_ ),
  numPhiSamples( numPhiSamples_ ),
  bMultiplyBRDF( bMultiplyBRDF_ ),
  bUseIrradianceCache( bUseIrradianceCache_ )
{
}
AmbientOcclusionShaderOp::~AmbientOcclusionShaderOp( )
{
}

//! Tells the shader to apply shade to the given intersection point
void AmbientOcclusionShaderOp::PerformOperation(
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

	// Only do stuff on a normal pass or on final gather
//	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
//		return;
//	}

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	bool bComputeAmbOcc = true;
	IIrradianceCache* pCache = caster.GetAttachedScene()->GetIrradianceCache();

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
			bComputeAmbOcc = false;
		}
	}

	// If we are using irradiance cache pass
	if( rc.pass == RuntimeContext::PASS_IRRADIANCE_CACHE ) {
		// And we are using irradiance caching
		if( (bUseIrradianceCache && pCache) || (rs.type == IRayCaster::RAY_STATE::eRayFinalGather) ) {
			// Check the cache to see if we should generate a sample here
			bComputeAmbOcc = pCache->IsSampleNeeded( ri.geometric.ptIntersection, ri.geometric.vNormal );
		} else {
			bComputeAmbOcc = false;
		}
	}

	if( bComputeAmbOcc ) {

		// For the irradiance cache stuff
		Scalar rsum = 0;
		unsigned int hits = 0;

		// Ambient occlusion, should if its required to be used with other effects such as
		// photon maps be used as a single pass, then composited later on.  This is why
		// the ambient occlusion shader is so trivially simple

		if( (pBRDF&&bMultiplyBRDF) || !bMultiplyBRDF ) {
			const IRadianceMap* pRadianceMap = ri.pRadianceMap ? ri.pRadianceMap : caster.GetAttachedScene()->GetGlobalRadianceMap();

			RISEPel accum;

			const Scalar fN = Scalar(numPhiSamples);
			const Scalar fM = Scalar(numThetaSamples);

			for( unsigned int i=0; i<numPhiSamples; i++ ) {
				const Scalar xi = (Scalar(i) + rc.random.CanonicalRandom()) / fN;
				const Scalar phi = TWO_PI * xi;
				const Scalar cosPhi = cos(phi);
				const Scalar sinPhi = sin(phi);
//				const Scalar phim = (TWO_PI * Scalar(i)) / fN;
/*
				const Vector3 vim = ri.geometric.onb.Transform(
					Vector3(cos( phim + PI_OV_TWO ), sin( phim + PI_OV_TWO ), 0.0)
					);
*/
				for( unsigned int j=0; j<numThetaSamples; j++ ) {
					const Scalar xj = (Scalar(j) + rc.random.CanonicalRandom()) / fM;
					const Scalar sinTheta = sqrt( xj );
					const Scalar cosTheta = sqrt( 1.0 - xj );

					const Vector3 dir = ri.geometric.onb.Transform(
						Vector3(cosPhi * sinTheta,sinPhi * sinTheta,cosTheta)
						);

					Ray const ray(ri.geometric.ptIntersection, dir);

					if( bUseIrradianceCache && pCache ) {
						RayIntersection	newri( ray, ri.geometric.rast );
						caster.GetAttachedScene()->GetObjects()->IntersectRay( newri, true, true, false );
						if( !newri.geometric.bHit ) {
							// Accumulate
							if( pBRDF && bMultiplyBRDF ) {
								accum = accum + pBRDF->value( dir, ri.geometric ) * (pRadianceMap?pRadianceMap->GetRadiance(ray,ri.geometric.rast) : RISEPel(1,1,1));
							} else if( !bMultiplyBRDF ) {
								accum = accum + (pRadianceMap?pRadianceMap->GetRadiance(ray,ri.geometric.rast) : RISEPel(1,1,1));
							}
						} else {
							rsum += 1.0/ri.geometric.range;
							hits++;
						}
					} else {
						if( !caster.CastShadowRay( ray, INFINITY ) ) {
							// Accumulate
							if( pBRDF && bMultiplyBRDF ) {
								accum = accum + pBRDF->value( dir, ri.geometric ) * (pRadianceMap?pRadianceMap->GetRadiance(ray,ri.geometric.rast) : RISEPel(1,1,1));
							} else if( !bMultiplyBRDF ) {
								accum = accum + (pRadianceMap?pRadianceMap->GetRadiance(ray,ri.geometric.rast) : RISEPel(1,1,1));
							}
						}
					}
				}

				// Divide out the values
				c = accum * (1.0/Scalar(numPhiSamples*numThetaSamples));

				if( pBRDF && bMultiplyBRDF ) {
					c = c * PI;
				}
			}

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
Scalar AmbientOcclusionShaderOp::PerformOperationNM(
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

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	// Ambient occlusion, should if its required to be used with other effects such as
	// photon maps be used as a single pass, then composited later on.  This is why
	// the ambient occlusion shader is so trivially simple

	if( (pBRDF&&bMultiplyBRDF) || !bMultiplyBRDF ) {
		const IRadianceMap* pRadianceMap = ri.pRadianceMap ? ri.pRadianceMap : caster.GetAttachedScene()->GetGlobalRadianceMap();

		Scalar accum = 0;

		const Scalar fN = Scalar(numPhiSamples);
		const Scalar fM = Scalar(numThetaSamples);

		for( unsigned int i=0; i<numPhiSamples; i++ ) {
			const Scalar xi = (Scalar(i) + rc.random.CanonicalRandom()) / fN;
			const Scalar phi = TWO_PI * xi;
			const Scalar cosPhi = cos(phi);
			const Scalar sinPhi = sin(phi);
//			const Scalar phim = (TWO_PI * Scalar(i)) / fN;
/*
			const Vector3 vim = ri.geometric.onb.Transform(
				Vector3(cos( phim + PI_OV_TWO ), sin( phim + PI_OV_TWO ), 0.0)
				);
*/
			for( unsigned int j=0; j<numThetaSamples; j++ ) {
				const Scalar xj = (Scalar(j) + rc.random.CanonicalRandom()) / fM;
				const Scalar sinTheta = sqrt( xj );
				const Scalar cosTheta = sqrt( 1.0 - xj );

				const Vector3 dir = ri.geometric.onb.Transform(
					Vector3(cosPhi * sinTheta,sinPhi * sinTheta,cosTheta)
					);

				Ray const ray(ri.geometric.ptIntersection, dir);
				if( !caster.CastShadowRay( ray, INFINITY ) ) {
					// Accumulate
					if( pBRDF && bMultiplyBRDF ) {
						accum += pBRDF->valueNM( dir, ri.geometric, nm ) * (pRadianceMap?pRadianceMap->GetRadianceNM(ray,ri.geometric.rast,nm) : 1.0);
					} else if( !bMultiplyBRDF ) {
						accum += (pRadianceMap?pRadianceMap->GetRadianceNM(ray,ri.geometric.rast,nm) : 1.0);
					}
				}
			}
		}

		// Divide out the values
		c = accum * (1.0/Scalar(numPhiSamples*numThetaSamples)) * ((pBRDF && bMultiplyBRDF)?PI:1);
	}

	return c;
}
