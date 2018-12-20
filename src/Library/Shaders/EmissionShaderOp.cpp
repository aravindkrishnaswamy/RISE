//////////////////////////////////////////////////////////////////////
//
//  EmissionShaderOp.cpp - Implementation of the EmissionShaderOp class
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
#include "EmissionShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

EmissionShaderOp::EmissionShaderOp(
	)
{
}

EmissionShaderOp::~EmissionShaderOp( )
{
}

//! Tells the shader to apply shade to the given intersection point
void EmissionShaderOp::PerformOperation(
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

	IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;

	if( pEmitter && rs.considerEmission ) {
		c = pEmitter->emittedRadiance( ri.geometric, -ri.geometric.ray.dir, ri.geometric.vNormal );

		// For view rays, scale their values down, to help out the AA
		if( rs.type == rs.eRayView ) {
			ColorMath::Scale( c );
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar EmissionShaderOp::PerformOperationNM(
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

	IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;

	if( pEmitter && rs.considerEmission ) {
		c = pEmitter->emittedRadianceNM( ri.geometric, -ri.geometric.ray.dir, ri.geometric.vNormal, nm );
	}

	return c;
}
