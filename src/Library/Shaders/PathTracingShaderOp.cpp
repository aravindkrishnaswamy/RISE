//////////////////////////////////////////////////////////////////////
//
//  PathTracingShaderOp.cpp - Implementation of the PathTracingShaderOp class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 30, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PathTracingShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

PathTracingShaderOp::PathTracingShaderOp(
	const bool bBranch_,
	const bool forcecheckemitters,
	const bool bFinalGather_,
	const bool reflections,
	const bool refractions,
	const bool diffuse,
	const bool translucents
	) : 
  bBranch( bBranch_ ),
  bForceCheckEmitters( forcecheckemitters ),
  bFinalGather( bFinalGather_ ),
  bTraceReflection( reflections ),
  bTraceRefraction( refractions ),
  bTraceDiffuse( diffuse ),
  bTraceTranslucent( translucents )
{
}

PathTracingShaderOp::~PathTracingShaderOp( )
{
}

bool PathTracingShaderOp::ShouldTraceRay( const ScatteredRay::ScatRayType type ) const
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
void PathTracingShaderOp::PerformOperation(
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
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return;
	}

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	if( rs.type == IRayCaster::RAY_STATE::eRayFinalGather && pBRDF )
	{
		const IScene* pScene = caster.GetAttachedScene();
		const IPhotonMap* pGM = pScene->GetGlobalPelMap();
		const ISpectralPhotonMap* pGSM = pScene->GetGlobalSpectralMap();

		if( pGM ) {
			pGM->RadianceEstimate( c, ri.geometric, *pBRDF );
		}

		if( pGSM ) {
			RISEPel cs;
			pGSM->RadianceEstimate( cs, ri.geometric, *pBRDF );
			c = c + cs;
		}
		return;
	}

	if( pScat ) {
		const ScatteredRayContainer scattered = *pScat;

		IRayCaster::RAY_STATE rs2;
		rs2.depth = rs.depth+1;
		if( bForceCheckEmitters ) {
			rs2.considerEmission = true;
		} else {
			rs2.considerEmission = ((pBRDF)||(caster.GetAttachedScene()->GetCausticSpectralMap()&&!rs.considerEmission))?false:true;
		}

		if( bFinalGather ) {
			if( rs.type == IRayCaster::RAY_STATE::eRayView ) {
				// If we are to shoot final gather rays and this is the view ray, then we
				// shoot final gather rays which will look at the photon map
				rs2.type = IRayCaster::RAY_STATE::eRayFinalGather;
			}

			if( rs.type == IRayCaster::RAY_STATE::eRayFinalGather ) {
				rs2.type = IRayCaster::RAY_STATE::eRayFinalGather;
			}
		}

		if( bBranch ) {
			for( unsigned int i=0; i<scattered.Count(); i++ ) {
				ScatteredRay& scat = scattered[i];

				if( ShouldTraceRay( scat.type ) ) {
					const Scalar scatmaxv = ColorMath::MaxValue(scat.kray);
					if( scatmaxv > 0.0 ) {
						RISEPel		cthis(0,0,0);
						rs2.importance = rs.importance * scatmaxv;
						if( !bFinalGather ) {
							rs2.type = scat.type==ScatteredRay::eRayDiffuse ? IRayCaster::RAY_STATE::eRayDiffuse : IRayCaster::RAY_STATE::eRaySpecular;
						}

						Ray ray = scat.ray;
						ray.Advance( 1e-8 );
						caster.CastRay( rc, ri.geometric.rast, ray, cthis, rs2, 0, ri.pRadianceMap, scat.ior_stack?scat.ior_stack:ior_stack );
						c = c + (scat.kray*cthis);
					}
				}
			}
		} else {
			const ScatteredRay* pScat = scattered.RandomlySelect( rc.random.CanonicalRandom(), false );;
			if( pScat && ShouldTraceRay( pScat->type ) ) {
				if( !bFinalGather ) {
					rs2.type = pScat->type==ScatteredRay::eRayDiffuse ? IRayCaster::RAY_STATE::eRayDiffuse : IRayCaster::RAY_STATE::eRaySpecular;
				}
				
				Ray ray = pScat->ray;
				ray.Advance( 1e-8 );
				rs2.importance = rs.importance * ColorMath::MaxValue(pScat->kray);
				caster.CastRay( rc, ri.geometric.rast, ray, c, rs2, 0, ri.pRadianceMap, pScat->ior_stack?pScat->ior_stack:ior_stack );
				c = c * pScat->kray;
			}
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar PathTracingShaderOp::PerformOperationNM(
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

	if( pScat ) {
		const ScatteredRayContainer& scattered = *pScat;

		IRayCaster::RAY_STATE rs2;
		rs2.depth = rs.depth+1;
		rs2.considerEmission = ((ri.pMaterial->GetBSDF())||(caster.GetAttachedScene()->GetCausticSpectralMap()&&!rs.considerEmission))?false:true;

		if( bBranch ) {
			for( unsigned int i=0; i<scattered.Count(); i++ ) {
				ScatteredRay& scat = scattered[i];

				if( ShouldTraceRay( scat.type ) ) {
					if( scat.krayNM > 0.0 ) {
						Scalar	cthis = 0;
						rs2.importance = rs.importance * scat.krayNM;
						rs2.type = scat.type==ScatteredRay::eRayDiffuse ? IRayCaster::RAY_STATE::eRayDiffuse : IRayCaster::RAY_STATE::eRaySpecular;

						Ray ray = scat.ray;
						ray.Advance( 1e-8 );
						caster.CastRayNM( rc, ri.geometric.rast, ray, cthis, rs2, nm, 0, ri.pRadianceMap, scat.ior_stack?scat.ior_stack:ior_stack );
						c += cthis * scat.krayNM;
					}
				}
			}
		} else {
			const ScatteredRay* pScat = scattered.RandomlySelect( rc.random.CanonicalRandom(), true );;
			if( pScat && ShouldTraceRay( pScat->type ) ) {
				Scalar	cthis = 0;
				Ray ray = pScat->ray;
				ray.Advance( 1e-8 );
				rs2.importance = rs.importance * ColorMath::MaxValue(pScat->kray);
				rs2.type = pScat->type==ScatteredRay::eRayDiffuse ? IRayCaster::RAY_STATE::eRayDiffuse : IRayCaster::RAY_STATE::eRaySpecular;
				caster.CastRayNM( rc, ri.geometric.rast, ray, cthis, rs2, nm, 0, ri.pRadianceMap, pScat->ior_stack?pScat->ior_stack:ior_stack );
				c += cthis * pScat->krayNM;
			}
		}
	}

	return c;
}
