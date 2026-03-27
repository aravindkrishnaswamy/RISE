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
		c = pEmitter->emittedRadiance( ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal );

		// MIS weight for BSDF-sampled emission.
		// When bsdfPdf > 0, this ray was traced via BSDF importance sampling.
		// We weight the emission by w_bsdf = p_bsdf² / (p_bsdf² + p_light²).
		// When bsdfPdf == 0 (view ray or delta), full weight (no MIS).
		if( rs.bsdfPdf > 0 && ri.pObject )
		{
			const Scalar area = ri.pObject->GetArea();
			if( area > 0 )
			{
				// cos at light surface
				const Scalar cosLight = fabs( Vector3Ops::Dot( ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
				if( cosLight > 0 )
				{
					const Scalar dist = Vector3Ops::Magnitude(
						Vector3Ops::mkVector3( ri.geometric.ptIntersection, ri.geometric.ray.origin ) );
					const Scalar p_light = (dist * dist) / (area * cosLight);
					const Scalar p_bsdf = rs.bsdfPdf;
					const Scalar w_bsdf = (p_bsdf * p_bsdf) / (p_bsdf * p_bsdf + p_light * p_light);
					c = c * w_bsdf;
				}
			}
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
		c = pEmitter->emittedRadianceNM( ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal, nm );

		// MIS weight for BSDF-sampled emission (spectral)
		if( rs.bsdfPdf > 0 && ri.pObject )
		{
			const Scalar area = ri.pObject->GetArea();
			if( area > 0 )
			{
				const Scalar cosLight = fabs( Vector3Ops::Dot( ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
				if( cosLight > 0 )
				{
					const Scalar dist = Vector3Ops::Magnitude(
						Vector3Ops::mkVector3( ri.geometric.ptIntersection, ri.geometric.ray.origin ) );
					const Scalar p_light = (dist * dist) / (area * cosLight);
					const Scalar p_bsdf = rs.bsdfPdf;
					const Scalar w_bsdf = (p_bsdf * p_bsdf) / (p_bsdf * p_bsdf + p_light * p_light);
					c = c * w_bsdf;
				}
			}
		}
	}

	return c;
}
