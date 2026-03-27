//////////////////////////////////////////////////////////////////////
//
//  MISPathTracingShaderOp.cpp - Unified MIS path tracer with SMS
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MISPathTracingShaderOp.h"
#include "../Rendering/LuminaryManager.h"
#include "../Lights/LightSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

MISPathTracingShaderOp::MISPathTracingShaderOp(
	const bool branch,
	const ManifoldSolverConfig& smsConfig
	) :
  pSolver( 0 ),
  bBranch( branch ),
  bSMSEnabled( smsConfig.enabled )
{
	if( smsConfig.enabled )
	{
		pSolver = new ManifoldSolver( smsConfig );
		pSolver->addref();
	}
}

MISPathTracingShaderOp::~MISPathTracingShaderOp()
{
	safe_release( pSolver );
}

/// Power heuristic weight: w = pa² / (pa² + pb²)
static inline Scalar PowerHeuristic( const Scalar pa, const Scalar pb )
{
	const Scalar pa2 = pa * pa;
	return pa2 / (pa2 + pb * pb);
}

void MISPathTracingShaderOp::PerformOperation(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	RISEPel& c,
	const IORStack* const ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	c = RISEPel( 0.0 );

	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) return;

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	// ================================================================
	// PART 1: Emission
	// ================================================================
	// Check emission at ANY surface (including those with BSDFs like
	// lambertian_luminaire_material which has both a BSDF and emitter).
	{
		IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;
		if( pEmitter && rs.considerEmission )
		{
			RISEPel emission = pEmitter->emittedRadiance(
				ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal );

			// MIS weight for BSDF-sampled emission hitting a mesh light
			if( rs.bsdfPdf > 0 && ri.pObject )
			{
				const Scalar area = ri.pObject->GetArea();
				if( area > 0 )
				{
					const Scalar cosLight = fabs( Vector3Ops::Dot(
						ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
					if( cosLight > 0 )
					{
						const Scalar dist = Vector3Ops::Magnitude(
							Vector3Ops::mkVector3(
								ri.geometric.ptIntersection,
								ri.geometric.ray.origin ) );
						const Scalar p_nee = (dist * dist) / (area * cosLight);
						const Scalar w_bsdf = PowerHeuristic( rs.bsdfPdf, p_nee );
						emission = emission * w_bsdf;
					}
				}
			}

			if( rs.type == rs.eRayView ) {
				ColorMath::Scale( emission );
			}
			c = c + emission;
		}
	}

	// Specular surfaces (no BSDF) — trace scattered rays and return
	if( !pBRDF ) {
		// Still need to trace scattered rays for specular surfaces
		if( pScat ) {
			const ScatteredRayContainer& scattered = *pScat;
			IRayCaster::RAY_STATE rs2;
			rs2.depth = rs.depth + 1;
			// Through specular surfaces, keep emission enabled.
			// The double-counting prevention happens at DIFFUSE surfaces
			// (Part 3) where SMS is the alternative to BSDF-sampled
			// emission through glass.  At specular surfaces we must
			// propagate emission visibility so that non-mesh lights
			// (spot, point) can illuminate surfaces seen through glass.
			rs2.considerEmission = true;

			if( bBranch ) {
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					ScatteredRay& scat = scattered[i];
					const Scalar scatmaxv = ColorMath::MaxValue( scat.kray );
					if( scatmaxv > 0 ) {
						RISEPel cthis( 0, 0, 0 );
						rs2.importance = rs.importance * scatmaxv;
						rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
						Ray ray = scat.ray;
						ray.Advance( 1e-8 );
						caster.CastRay( rc, ri.geometric.rast, ray, cthis,
							rs2, 0, ri.pRadianceMap,
							scat.ior_stack ? scat.ior_stack : ior_stack );
						c = c + (scat.kray * cthis);
					}
				}
			} else {
				const ScatteredRay* pS = scattered.RandomlySelect(
					rc.random.CanonicalRandom(), false );
				if( pS ) {
					RISEPel cthis( 0, 0, 0 );
					rs2.importance = rs.importance * ColorMath::MaxValue( pS->kray );
					rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
					Ray ray = pS->ray;
					ray.Advance( 1e-8 );
					caster.CastRay( rc, ri.geometric.rast, ray, cthis,
						rs2, 0, ri.pRadianceMap,
						pS->ior_stack ? pS->ior_stack : ior_stack );
					c = c + (pS->kray * cthis);
				}
			}
		}
		return;
	}

	// ================================================================
	// PART 2: NEE + SMS at diffuse/glossy surfaces
	// ================================================================

	// --- 2a: NEE for non-mesh lights (point, spot, directional) ---
	// These have delta position, so MIS weight = 1.0 always.
	const ILightManager* pLM = pScene->GetLights();
	if( pLM ) {
		RISEPel directNonMesh( 0, 0, 0 );
		pLM->ComputeDirectLighting( ri.geometric, caster, *pBRDF,
			ri.pObject->DoesReceiveShadows(), directNonMesh );
		c = c + directNonMesh;
	}

	// --- 2b: NEE for mesh luminaries (with MIS weight) ---
	// Use the existing LuminaryManager which iterates all luminaries,
	// handles shadow testing, and applies MIS weights.
	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	if( pLumMgr )
	{
		RISEPel directMesh( 0, 0, 0 );
		directMesh = pLumMgr->ComputeDirectLighting(
			ri, *pBRDF, rc.random, caster, pScene->GetShadowMap() );
		c = c + directMesh;
	}

	// --- 2c: SMS for caustics through specular surfaces ---
	// Attempt a single SMS evaluation using a randomly sampled light.
	// This only fires at first-bounce diffuse surfaces.
	// EvaluateAtShadingPoint handles: light sampling, seed chain,
	// Newton solve, BSDF evaluation, geometric terms.
	if( pSolver && rs.depth <= 1 )
	{
		const Vector3 woOutgoing = Vector3(
			-ri.geometric.ray.Dir().x,
			-ri.geometric.ray.Dir().y,
			-ri.geometric.ray.Dir().z );

		ManifoldSolver::SMSContribution sms = pSolver->EvaluateAtShadingPoint(
			ri.geometric.ptIntersection,
			ri.geometric.vNormal,
			ri.geometric.onb,
			ri.pMaterial,
			woOutgoing,
			*pScene,
			caster,
			rc.random );

		if( sms.valid )
		{
			c = c + sms.contribution * sms.misWeight;
		}
	}

	// ================================================================
	// PART 3: BSDF sampling (continue path)
	// ================================================================
	if( pScat )
	{
		const ScatteredRayContainer& scattered = *pScat;

		IRayCaster::RAY_STATE rs2;
		rs2.depth = rs.depth + 1;
		// For BSDF-sampled rays at diffuse surfaces, enable emission
		// so that direct light hits (non-specular paths) are counted
		// with MIS weight.  The MIS weight is computed in PART 1 above
		// when the next hit is an emitter.
		rs2.considerEmission = true;

		if( bBranch )
		{
			for( unsigned int i = 0; i < scattered.Count(); i++ )
			{
				ScatteredRay& scat = scattered[i];
				const Scalar scatmaxv = ColorMath::MaxValue( scat.kray );
				if( scatmaxv > 0 )
				{
					RISEPel cthis( 0, 0, 0 );
					rs2.importance = rs.importance * scatmaxv;
					rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;

					// For specular bounces, disable emission to
					// prevent double-counting with SMS
					if( scat.isDelta && bSMSEnabled ) {
						rs2.considerEmission = false;
					} else {
						rs2.considerEmission = true;
					}

					Ray ray = scat.ray;
					ray.Advance( 1e-8 );
					caster.CastRay( rc, ri.geometric.rast, ray, cthis,
						rs2, 0, ri.pRadianceMap,
						scat.ior_stack ? scat.ior_stack : ior_stack );
					c = c + (scat.kray * cthis);
				}
			}
		}
		else
		{
			const ScatteredRay* pS = scattered.RandomlySelect(
				rc.random.CanonicalRandom(), false );
			if( pS )
			{
				RISEPel cthis( 0, 0, 0 );
				rs2.importance = rs.importance * ColorMath::MaxValue( pS->kray );
				rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;

				if( pS->isDelta && bSMSEnabled ) {
					rs2.considerEmission = false;
				} else {
					rs2.considerEmission = true;
				}

				Ray ray = pS->ray;
				ray.Advance( 1e-8 );
				caster.CastRay( rc, ri.geometric.rast, ray, cthis,
					rs2, 0, ri.pRadianceMap,
					pS->ior_stack ? pS->ior_stack : ior_stack );
				c = c + (pS->kray * cthis);
			}
		}
	}
}

Scalar MISPathTracingShaderOp::PerformOperationNM(
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
	Scalar c = 0;

	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return 0;
	}

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) return 0;

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	// ================================================================
	// PART 1: Emission (spectral)
	// ================================================================
	{
		IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;
		if( pEmitter && rs.considerEmission )
		{
			Scalar emission = pEmitter->emittedRadianceNM(
				ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal, nm );

			if( rs.bsdfPdf > 0 && ri.pObject )
			{
				const Scalar area = ri.pObject->GetArea();
				if( area > 0 )
				{
					const Scalar cosLight = fabs( Vector3Ops::Dot(
						ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
					if( cosLight > 0 )
					{
						const Scalar dist = Vector3Ops::Magnitude(
							Vector3Ops::mkVector3(
								ri.geometric.ptIntersection,
								ri.geometric.ray.origin ) );
						const Scalar p_nee = (dist * dist) / (area * cosLight);
						const Scalar w_bsdf = PowerHeuristic( rs.bsdfPdf, p_nee );
						emission = emission * w_bsdf;
					}
				}
			}

			c += emission;
		}
	}

	// Specular surfaces — trace scattered rays
	if( !pBRDF ) {
		if( pScat ) {
			const ScatteredRayContainer& scattered = *pScat;
			IRayCaster::RAY_STATE rs2;
			rs2.depth = rs.depth + 1;
			rs2.considerEmission = true;

			if( bBranch ) {
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					ScatteredRay& scat = scattered[i];
					if( scat.krayNM > 0 ) {
						Scalar cthis = 0;
						rs2.importance = rs.importance * scat.krayNM;
						rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
						Ray ray = scat.ray;
						ray.Advance( 1e-8 );
						caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
							rs2, nm, 0, ri.pRadianceMap,
							scat.ior_stack ? scat.ior_stack : ior_stack );
						c += cthis * scat.krayNM;
					}
				}
			} else {
				const ScatteredRay* pS = scattered.RandomlySelect(
					rc.random.CanonicalRandom(), true );
				if( pS ) {
					Scalar cthis = 0;
					rs2.importance = rs.importance * pS->krayNM;
					rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
					Ray ray = pS->ray;
					ray.Advance( 1e-8 );
					caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
						rs2, nm, 0, ri.pRadianceMap,
						pS->ior_stack ? pS->ior_stack : ior_stack );
					c += cthis * pS->krayNM;
				}
			}
		}
		return c;
	}

	// ================================================================
	// PART 2: NEE (spectral)
	// ================================================================

	// 2a: Non-mesh lights — no spectral variant exists in ILightManager,
	// skip for spectral rendering (matching DirectLightingShaderOp behavior)

	// 2b: Mesh luminaries with MIS
	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	if( pLumMgr )
	{
		c += pLumMgr->ComputeDirectLightingNM(
			ri, *pBRDF, nm, rc.random, caster, pScene->GetShadowMap() );
	}

	// 2c: SMS (spectral — per-wavelength IOR for dispersion)
	if( pSolver && rs.depth <= 1 )
	{
		const Vector3 woOutgoing = Vector3(
			-ri.geometric.ray.Dir().x,
			-ri.geometric.ray.Dir().y,
			-ri.geometric.ray.Dir().z );

		ManifoldSolver::SMSContributionNM sms = pSolver->EvaluateAtShadingPointNM(
			ri.geometric.ptIntersection,
			ri.geometric.vNormal,
			ri.geometric.onb,
			ri.pMaterial,
			woOutgoing,
			*pScene,
			caster,
			rc.random,
			nm );

		if( sms.valid )
		{
			c += sms.contribution * sms.misWeight;
		}
	}

	// ================================================================
	// PART 3: BSDF sampling (spectral)
	// ================================================================
	if( pScat )
	{
		const ScatteredRayContainer& scattered = *pScat;

		IRayCaster::RAY_STATE rs2;
		rs2.depth = rs.depth + 1;
		rs2.considerEmission = true;

		if( bBranch )
		{
			for( unsigned int i = 0; i < scattered.Count(); i++ )
			{
				ScatteredRay& scat = scattered[i];
				if( scat.krayNM > 0 )
				{
					Scalar cthis = 0;
					rs2.importance = rs.importance * scat.krayNM;
					rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;

					if( scat.isDelta && bSMSEnabled ) {
						rs2.considerEmission = false;
					} else {
						rs2.considerEmission = true;
					}

					Ray ray = scat.ray;
					ray.Advance( 1e-8 );
					caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
						rs2, nm, 0, ri.pRadianceMap,
						scat.ior_stack ? scat.ior_stack : ior_stack );
					c += cthis * scat.krayNM;
				}
			}
		}
		else
		{
			const ScatteredRay* pS = scattered.RandomlySelect(
				rc.random.CanonicalRandom(), true );
			if( pS )
			{
				Scalar cthis = 0;
				rs2.importance = rs.importance * pS->krayNM;
				rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;

				if( pS->isDelta && bSMSEnabled ) {
					rs2.considerEmission = false;
				} else {
					rs2.considerEmission = true;
				}

				Ray ray = pS->ray;
				ray.Advance( 1e-8 );
				caster.CastRayNM( rc, ri.geometric.rast, ray, cthis,
					rs2, nm, 0, ri.pRadianceMap,
					pS->ior_stack ? pS->ior_stack : ior_stack );
				c += cthis * pS->krayNM;
			}
		}
	}

	return c;
}
