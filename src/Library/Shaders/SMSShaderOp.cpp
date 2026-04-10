//////////////////////////////////////////////////////////////////////
//
//  SMSShaderOp.cpp - Implementation of the SMS shader operation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SMSShaderOp.h"
#include "../Utilities/IndependentSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

SMSShaderOp::SMSShaderOp( const ManifoldSolverConfig& config ) :
  pSolver( 0 )
{
	pSolver = new ManifoldSolver( config );
}

SMSShaderOp::~SMSShaderOp()
{
	safe_release( pSolver );
}

void SMSShaderOp::PerformOperation(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	RISEPel& c,
	const IORStack* const ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	// Only run on normal passes
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return;
	}

	// Only attempt SMS on first bounce (direct caustics)
	if( rs.depth > 1 ) {
		return;
	}

	// Only attempt SMS at non-specular surfaces that have a BSDF
	if( !ri.pMaterial || !ri.pMaterial->GetBSDF() ) {
		return;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) {
		return;
	}

	const Vector3 woOutgoing = Vector3(
		-ri.geometric.ray.Dir().x,
		-ri.geometric.ray.Dir().y,
		-ri.geometric.ray.Dir().z );

	IndependentSampler fallbackSampler( rc.random );
	ISampler& smsSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

	ManifoldSolver::SMSContribution sms = pSolver->EvaluateAtShadingPoint(
		ri.geometric.ptIntersection,
		ri.geometric.vNormal,
		ri.geometric.onb,
		ri.pMaterial,
		woOutgoing,
		*pScene,
		caster,
		smsSampler );

	if( sms.valid )
	{
		c = c + sms.contribution * sms.misWeight;
	}
}

Scalar SMSShaderOp::PerformOperationNM(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	const Scalar caccum,
	const Scalar nm,
	const IORStack* const ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	// SMS spectral variant: uses per-wavelength IOR for dispersion
	// and scalar (single-wavelength) evaluation throughout.
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return 0;
	}

	if( !ri.pMaterial || !ri.pMaterial->GetBSDF() ) {
		return 0;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) {
		return 0;
	}

	const Vector3 woOutgoing = Vector3(
		-ri.geometric.ray.Dir().x,
		-ri.geometric.ray.Dir().y,
		-ri.geometric.ray.Dir().z );

	IndependentSampler fallbackSamplerNM( rc.random );
	ISampler& smsSamplerNM = rc.pSampler ? *rc.pSampler : fallbackSamplerNM;

	ManifoldSolver::SMSContributionNM sms = pSolver->EvaluateAtShadingPointNM(
		ri.geometric.ptIntersection,
		ri.geometric.vNormal,
		ri.geometric.onb,
		ri.pMaterial,
		woOutgoing,
		*pScene,
		caster,
		smsSamplerNM,
		nm );

	if( sms.valid )
	{
		return sms.contribution * sms.misWeight;
	}

	return 0;
}
