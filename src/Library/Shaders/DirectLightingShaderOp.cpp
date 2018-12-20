//////////////////////////////////////////////////////////////////////
//
//  DirectLightingShaderOp.cpp - Implementation of the DirectLightingShaderOp class
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
#include "DirectLightingShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

DirectLightingShaderOp::DirectLightingShaderOp(
	const IMaterial* pBSDF_,
	const bool nonmeshlights_,
	const bool meshlights_,
	const bool cache_
	) : 
  pBSDF( pBSDF_ ),
  nonmeshlights( nonmeshlights_ ),
  meshlights( meshlights_ ),
  cache( cache_ )
{
	if( pBSDF ) {
		pBSDF->addref();
	}
}

DirectLightingShaderOp::~DirectLightingShaderOp( )
{
	if( pBSDF ) {
		pBSDF->release();
	}
}

//! Tells the shader to apply shade to the given intersection point
void DirectLightingShaderOp::PerformOperation(
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
	const IBSDF* pBRDF = pBSDF ? pBSDF->GetBSDF() : (ri.pMaterial ? ri.pMaterial->GetBSDF() : 0);

	if( pScene && pBRDF ) {

		// Lets check our rasterizer state to see if we even need to do work!
		if( cache ) {
			if( !rc.StateCache_HasStateChanged( this, c, ri.pObject, ri.geometric.rast ) ) {
				// State hasn't changed, use the value already there
				return;
			}
		}

		const ILightManager* pLM = pScene->GetLights();
		if( pLM && nonmeshlights ) {
			pLM->ComputeDirectLighting( ri.geometric, caster, *pBRDF, ri.pObject->DoesReceiveShadows(), c );
		}

		// Account for lights from luminaries
		const ILuminaryManager* pLumManager = caster.GetLuminaries();
		if( pLumManager && meshlights) {
			c = c + pLumManager->ComputeDirectLighting( ri, *pBRDF, rc.random, caster, pScene->GetShadowMap() );			
		}

		// Add the result to the rasterizer state cache
		if( cache ) {
			rc.StateCache_SetState( this, c, ri.pObject, ri.geometric.rast );
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar DirectLightingShaderOp::PerformOperationNM(
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
	const IBSDF* pBRDF = pBSDF ? pBSDF->GetBSDF() : (ri.pMaterial ? ri.pMaterial->GetBSDF() : 0);

	if( pScene && pBRDF ) {
		// Account for lights from luminaries
		const ILuminaryManager* pLumManager = caster.GetLuminaries();
		if( pLumManager && meshlights ) {
			c = pLumManager->ComputeDirectLightingNM( ri, *pBRDF, nm, rc.random, caster, pScene->GetShadowMap() );			
		}
	}

	return c;
}
