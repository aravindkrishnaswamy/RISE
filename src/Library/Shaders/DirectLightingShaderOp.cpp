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
#include "../Utilities/IndependentSampler.h"
#include "../Lights/LightSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

DirectLightingShaderOp::DirectLightingShaderOp(
	const IMaterial* pBSDF_
	) :
  pBSDF( pBSDF_ )
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
	const IORStack& ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	c = RISEPel(0.0);

	// Only do stuff on a normal pass or on final gather
	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return;
	}

	const IScene* pScene = caster.GetAttachedScene();
	const IBSDF* pBRDF = pBSDF ? pBSDF->GetBSDF() : (ri.pMaterial ? ri.pMaterial->GetBSDF() : 0);

	if( !pScene || !pBRDF ) {
		return;
	}

	// Route through the unified LightSampler.  Handles analytic
	// lights (ambient, directional, point, spot) and mesh luminaires
	// through a single MIS-weighted estimator that honours the Light
	// BVH / RIS / alias-table selection configured on the sampler.
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) {
		return;
	}

	IndependentSampler fallbackSampler( rc.random );
	ISampler& sampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
	c = pLS->EvaluateDirectLighting(
		ri.geometric,
		*pBRDF,
		ri.pMaterial,
		caster,
		sampler,
		ri.pObject,
		0,		// pMedium: shader op runs at surface scatter, vacuum along shadow ray
		false,	// isVolumeScatter
		0 );	// pMediumObject
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
	const IORStack& ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	// Only do stuff on a normal pass or on final gather
	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return 0;
	}

	const IScene* pScene = caster.GetAttachedScene();
	const IBSDF* pBRDF = pBSDF ? pBSDF->GetBSDF() : (ri.pMaterial ? ri.pMaterial->GetBSDF() : 0);

	if( !pScene || !pBRDF ) {
		return 0;
	}

	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) {
		return 0;
	}

	IndependentSampler fallbackSampler( rc.random );
	ISampler& sampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
	return pLS->EvaluateDirectLightingNM(
		ri.geometric,
		*pBRDF,
		ri.pMaterial,
		nm,
		caster,
		sampler,
		ri.pObject,
		0,		// pMedium
		false,	// isVolumeScatter
		0 );	// pMediumObject
}
