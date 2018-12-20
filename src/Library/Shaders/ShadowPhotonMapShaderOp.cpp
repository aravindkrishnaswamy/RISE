//////////////////////////////////////////////////////////////////////
//
//  ShadowPhotonMapShaderOp.cpp - Implementation of the 
//    ShadowPhotonMapShaderOp class
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
#include "ShadowPhotonMapShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

ShadowPhotonMapShaderOp::ShadowPhotonMapShaderOp(
	)
{
}

ShadowPhotonMapShaderOp::~ShadowPhotonMapShaderOp( )
{
}

//! Tells the shader to apply shade to the given intersection point
void ShadowPhotonMapShaderOp::PerformOperation(
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

	const IScene* pScene = caster.GetAttachedScene();
	const IShadowPhotonMap* pSh = pScene->GetShadowMap();

	if( pSh ) {
		char shadow = 2;
		pSh->ShadowEstimate( shadow, ri.geometric.ptIntersection );

		switch( shadow )
		{
		case 0:
			c = RISEPel(1,1,1);
			break;
		case 1:
			break;
		case 2:
			c = RISEPel(1,0.0,0.0);
			break;
		};
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar ShadowPhotonMapShaderOp::PerformOperationNM(
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
	Scalar c = 0;

	// Only do stuff on a normal pass or on final gather
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return 0;
	}

	const IScene* pScene = caster.GetAttachedScene();
	const IShadowPhotonMap* pSh = pScene->GetShadowMap();

	if( pSh ) {
		char shadow = 2;
		pSh->ShadowEstimate( shadow, ri.geometric.ptIntersection );

		switch( shadow )
		{
		case 0:
			c = 1.0;
			break;
		case 1:
			break;
		case 2:
			c = 0.5;
			break;
		};
	}

	return c;
}
