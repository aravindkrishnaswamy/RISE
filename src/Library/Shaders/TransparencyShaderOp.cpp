//////////////////////////////////////////////////////////////////////
//
//  TransparencyShaderOp.cpp - Implementation of the TransparencyShaderOp class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 8, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TransparencyShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

TransparencyShaderOp::TransparencyShaderOp(
	const IPainter& transparency_,
	const bool bOneSided_
	) : 
  transparency( transparency_ ),
  bOneSided( bOneSided_ )
{
	transparency.addref();
}

TransparencyShaderOp::~TransparencyShaderOp( )
{
	transparency.release();
}

//! Tells the shader to apply shade to the given intersection point
void TransparencyShaderOp::PerformOperation(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [in/out] Resultant color from op
	const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	// What we do is continue the ray through this intersection and composite with the value behind
	Ray ray = ri.geometric.ray;
	ray.Advance( ri.geometric.range+2e-8 );

	RISEPel cthis;
	if( caster.CastRay( rc, ri.geometric.rast, ray, cthis, rs, 0, ri.pRadianceMap, ior_stack ) ) {
		// Blend by painter color
		// But if we one sided only and the ray is coming from behind, then don't
		if( bOneSided ) {
			if( Vector3Ops::Dot( ri.geometric.ray.dir, ri.geometric.vNormal ) > 0 ) {
				c = cthis;
			}
		}

		// Blend
		const RISEPel factor = transparency.GetColor(ri.geometric);
		c = cthis*factor  + c*(1.0-factor);
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar TransparencyShaderOp::PerformOperationNM(
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

	// What we do is continue the ray through this intersection and composite with the value behind
	Ray ray = ri.geometric.ray;
	ray.Advance( ri.geometric.range+2e-8 );

	if( caster.CastRayNM( rc, ri.geometric.rast, ray, c, rs, nm, 0, ri.pRadianceMap, ior_stack ) ) {
		// Blend by painter color
		// But if we one sided only and the ray is coming from behind, then don't
		if( bOneSided ) {
			if( Vector3Ops::Dot( ri.geometric.ray.dir, ri.geometric.vNormal ) > 0 ) {
				return c;
			}
		}

		// Blend
		const Scalar trans = transparency.GetColorNM(ri.geometric,nm);
		c = caccum*(1.0-trans) + c*trans;
	}
	
	return c;
}
