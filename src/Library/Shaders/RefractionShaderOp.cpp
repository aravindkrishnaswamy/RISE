//////////////////////////////////////////////////////////////////////
//
//  RefractionShaderOp.cpp - Implementation of the RefractionShaderOp class
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
#include "RefractionShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

RefractionShaderOp::RefractionShaderOp(
	)
{
}

RefractionShaderOp::~RefractionShaderOp( )
{
}

//! Tells the shader to apply shade to the given intersection point
void RefractionShaderOp::PerformOperation(
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

	// Only do stuff on a normal pass
	if( rc.pass != RuntimeContext::PASS_NORMAL ) {
		return;
	}

	if( pScat ) {
		const ScatteredRayContainer& scattered = *pScat;
		for( unsigned int i=0; i<scattered.Count(); i++ ) {
			const ScatteredRay& scat = scattered[i];
			if( scat.type==ScatteredRay::eRayRefraction )
			{
				// Cast and add!
				RISEPel	refractedPixel( 0.0 );
				Ray ray( scat.ray );
				ray.Advance( 1e-8 );

				IRayCaster::RAY_STATE rs2;

				rs2.depth = rs.depth+1;
				rs2.importance = rs.importance * ColorMath::MaxValue(scat.kray);
				rs2.considerEmission = true;
				rs2.type = IRayCaster::RAY_STATE::eRaySpecular;

				caster.CastRay( rc, ri.geometric.rast, ray, refractedPixel, rs2, 0, ri.pRadianceMap, scat.ior_stack?scat.ior_stack:ior_stack );
				c = c + (refractedPixel * scat.kray);
			}
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar RefractionShaderOp::PerformOperationNM(
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

	// Only do stuff on a normal pass
	if( rc.pass != RuntimeContext::PASS_NORMAL ) {
		return 0;
	}

	if( pScat ) {
		const ScatteredRayContainer& scattered = *pScat;
		for( unsigned int i=0; i<pScat->Count(); i++ ) {
			const ScatteredRay& scat = scattered[i];
			if( scat.type==ScatteredRay::eRayRefraction )
			{
				// Cast and add!
				Scalar	refracted = 0.0;
				Ray ray( scat.ray );
				ray.Advance( 1e-8 );

				IRayCaster::RAY_STATE rs2;

				rs2.depth = rs.depth+1;
				rs2.importance = rs.importance * scat.krayNM;
				rs2.considerEmission = true;
				rs2.type = IRayCaster::RAY_STATE::eRaySpecular;

				caster.CastRayNM( rc, ri.geometric.rast, ray, refracted, rs2, nm, 0, ri.pRadianceMap, scat.ior_stack?scat.ior_stack:ior_stack );
				c = c + (refracted * scat.krayNM);
			}
		}
	}

	return c;
}
