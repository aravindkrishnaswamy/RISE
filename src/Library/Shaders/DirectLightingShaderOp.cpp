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
	const IORStack& ior_stack,			///< [in] Index of refraction stack
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
		0,		// pMediumObject
		// DL-74 P2: no guiding hook on the legacy chain, but the
		// MIS-partner aggregate pdf must still be evaluated under the LIVE
		// IOR stack -- this arm's partner is EmissionShaderOp's
		// `rs.MisPartnerPdf()`, a density the previous vertex's SPF
		// produced under exactly this stack.
		/*pGuidedBlend*/ 0,
		&ior_stack,
		// DL-185: `RayCaster::CastRay{,NM,HWSS}` stamps its own local
		// cast-level RR compensation onto `rs` before dispatching to this
		// shader op (see `RAY_STATE::castRRCompensation`) -- fold it into
		// the training integrand so this NEE arm's trained optimal-MIS
		// moment agrees with the BSDF-escape arm's (DL-148).  Default 1
		// for every producer that predates the field.
		rs.castRRCompensation,
		// DL-171/DL-209: does a competing BSDF-sampled strategy actually
		// exist in THIS shader's own op list?  Stamped by the owning
		// StandardShader/AdvancedShader (see RAY_STATE::
		// chainHasBsdfContinuationOp's doc) -- default TRUE for every
		// caller that predates the field, so this is a no-op everywhere
		// but the legacy shader-op chain.
		rs.chainHasBsdfContinuationOp,
		// DL-292: price the NEE segment's graded-index factor (DL-09) from
		// this hit's stack.  RayCaster::CastRay{,NM,HWSS} Advances that
		// stack to the hit before shading it (and scales the whole shade by
		// the factor of the segment that reached it), so its top is n(hit)
		// and every BSDF-sampled sibling -- a distribution-tracing /
		// reflection / refraction continuation, or a PathTracingShaderOp --
		// prices its arrival at an emitter from the same value.  Before
		// DL-292 this passed nothing and the legacy chain kept the
		// pre-DL-09 accounting.
		&ior_stack );
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
	const IORStack& ior_stack,			///< [in] Index of refraction stack
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
		0,		// pMediumObject
		// DL-74 P2 -- see the RGB twin above.
		/*pGuidedBlend*/ 0,
		&ior_stack,
		// DL-185 -- see the RGB twin above.
		rs.castRRCompensation,
		// DL-171/DL-209 -- see the RGB twin above.
		rs.chainHasBsdfContinuationOp,
		// DL-292 -- see the RGB twin above.
		&ior_stack );
}
