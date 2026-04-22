//////////////////////////////////////////////////////////////////////
//
//  PathTracingShaderOp.cpp - Thin wrapper around PathTracingIntegrator
//
//  Delegates all path tracing logic to PathTracingIntegrator:
//  - PerformOperation   → IntegrateFromHit   (RGB)
//  - PerformOperationNM → IntegrateFromHitNM (spectral)
//  - PerformOperationHWSS → IntegrateFromHitHWSS (hero wavelength)
//
//  The ShaderOp entry points bridge the RayCaster shader dispatch
//  interface to the integrator: they construct the required sampler
//  and state, then call the appropriate IntegrateFromHit variant
//  with the pre-computed RayIntersection.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PathTracingShaderOp.h"
#include "PathTracingIntegrator.h"
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/Color/Color.h"

using namespace RISE;
using namespace RISE::Implementation;


//////////////////////////////////////////////////////////////////////
// Construction / destruction
//////////////////////////////////////////////////////////////////////

PathTracingShaderOp::PathTracingShaderOp(
	const ManifoldSolverConfig& smsConfig,
	const StabilityConfig& stabilityCfg
	) :
  pIntegrator( 0 ),
  bSMSEnabled( smsConfig.enabled )
{
	pIntegrator = new PathTracingIntegrator( smsConfig, stabilityCfg );
	pIntegrator->addref();
}

PathTracingShaderOp::~PathTracingShaderOp()
{
	safe_release( pIntegrator );
}


//////////////////////////////////////////////////////////////////////
// PerformOperation — RGB path tracing via IntegrateFromHit
//////////////////////////////////////////////////////////////////////

void PathTracingShaderOp::PerformOperation(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	RISEPel& c,
	const IORStack& ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	c = RISEPel( 0.0 );

	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) return;

	IndependentSampler sampler( rc.random );
	IORStack localIorStack( ior_stack );

	c = pIntegrator->IntegrateFromHit(
		rc, ri.geometric.rast, ri, *pScene, caster, sampler,
		ri.pRadianceMap,
		rs.depth, localIorStack,
		rs.bsdfPdf, rs.bsdfTimesCos, rs.considerEmission,
		rs.importance, rs.type,
		rs.diffuseBounces, rs.glossyBounces,
		rs.transmissionBounces, rs.translucentBounces,
		0, rs.glossyFilterWidth,
		rs.smsPassedThroughSpecular, rs.smsHadNonSpecularShading, false );
}


//////////////////////////////////////////////////////////////////////
// PerformOperationNM — Spectral single-wavelength via IntegrateFromHitNM
//////////////////////////////////////////////////////////////////////

Scalar PathTracingShaderOp::PerformOperationNM(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	const Scalar caccum,
	const Scalar nm,
	const IORStack& ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return 0;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) return 0;

	IndependentSampler sampler( rc.random );
	IORStack localIorStack( ior_stack );

	// bsdfTimesCos is stored as RISEPel; extract the scalar NM value
	const Scalar bsdfTimesCosNM = rs.bsdfTimesCos.r;

	return pIntegrator->IntegrateFromHitNM(
		rc, ri.geometric.rast, ri, nm, *pScene, caster, sampler,
		ri.pRadianceMap,
		rs.depth, localIorStack,
		rs.bsdfPdf, bsdfTimesCosNM, rs.considerEmission,
		rs.importance, rs.type,
		rs.diffuseBounces, rs.glossyBounces,
		rs.transmissionBounces, rs.translucentBounces,
		0, rs.glossyFilterWidth );
}


//////////////////////////////////////////////////////////////////////
// PerformOperationHWSS — Hero wavelength via IntegrateFromHitHWSS
//////////////////////////////////////////////////////////////////////

void PathTracingShaderOp::PerformOperationHWSS(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	const Scalar caccum[SampledWavelengths::N],
	SampledWavelengths& swl,
	const IORStack& ior_stack,
	const ScatteredRayContainer* pScat,
	Scalar result[SampledWavelengths::N]
	) const
{
	for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
		result[i] = 0;
	}

	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) return;

	IndependentSampler sampler( rc.random );
	IORStack localIorStack( ior_stack );

	pIntegrator->IntegrateFromHitHWSS(
		rc, ri.geometric.rast, ri, swl, *pScene, caster, sampler,
		ri.pRadianceMap,
		rs.depth, localIorStack,
		rs.bsdfPdf, rs.considerEmission,
		rs.importance, rs.type,
		rs.diffuseBounces, rs.glossyBounces,
		rs.transmissionBounces, rs.translucentBounces,
		0, rs.glossyFilterWidth, result );
}
