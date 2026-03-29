//////////////////////////////////////////////////////////////////////
//
//  PathTracingShaderOp.cpp - Unified MIS path tracer with SMS
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
#include "../Rendering/LuminaryManager.h"
#include "../Lights/LightSampler.h"
#include "../Utilities/IndependentSampler.h"
#ifdef RISE_ENABLE_OPENPGL
#include "../Utilities/PathGuidingField.h"
#endif

using namespace RISE;
using namespace RISE::Implementation;

PathTracingShaderOp::PathTracingShaderOp(
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

PathTracingShaderOp::~PathTracingShaderOp()
{
	safe_release( pSolver );
}

/// Power heuristic weight: w = pa² / (pa² + pb²)
static inline Scalar PowerHeuristic( const Scalar pa, const Scalar pb )
{
	const Scalar pa2 = pa * pa;
	return pa2 / (pa2 + pb * pb);
}

void PathTracingShaderOp::PerformOperation(
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
				const Scalar xi = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
				const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
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
		IndependentSampler fallbackLumSampler( rc.random );
		ISampler& lumSampler = rc.pSampler ? *rc.pSampler : fallbackLumSampler;

		RISEPel directMesh( 0, 0, 0 );
		directMesh = pLumMgr->ComputeDirectLighting(
			ri, *pBRDF, lumSampler, caster, pScene->GetShadowMap() );
		c = c + directMesh;
	}

	// --- 2c: SMS for caustics through specular surfaces ---
	// Attempt a single SMS evaluation using a randomly sampled light.
	// EvaluateAtShadingPoint handles: light sampling, seed chain,
	// Newton solve, BSDF evaluation, geometric terms.
	if( pSolver )
	{
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
					Scalar hitDistBranch = 0;
					caster.CastRay( rc, ri.geometric.rast, ray, cthis,
						rs2, &hitDistBranch, ri.pRadianceMap,
						scat.ior_stack ? scat.ior_stack : ior_stack );
					c = c + (scat.kray * cthis);

#ifdef RISE_ENABLE_OPENPGL
					// Collect training samples in branching mode
					if( rc.pGuidingField && !rc.pGuidingField->IsTrained() &&
						!scat.isDelta && scat.pdf > NEARZERO )
					{
						const Scalar lum = ColorMath::MaxValue( cthis );
						if( lum > 0 )
						{
							rc.pGuidingField->AddSample(
								ri.geometric.ptIntersection,
								ray.Dir(),
								hitDistBranch > 0 ? hitDistBranch : 1.0,
								scat.pdf,
								lum,
								false );
						}
					}
#endif
				}
			}
		}
		else
		{
			const Scalar xi = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
			const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
			if( pS )
			{
				Ray traceRay = pS->ray;
				RISEPel throughput = pS->kray;
				Scalar effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf;

#ifdef RISE_ENABLE_OPENPGL
				// --------------------------------------------------------
				// Path guiding: one-sample MIS for non-delta surfaces.
				// Blends the BSDF-sampled direction with a direction from
				// the learned incident radiance distribution.
				//
				// One-sample MIS: with probability alpha, sample from the
				// guiding distribution; with probability (1-alpha), keep
				// the BSDF direction.  In either case divide by the
				// combined PDF = alpha*guidePdf + (1-alpha)*bsdfPdf.
				// --------------------------------------------------------
				static thread_local GuidingDistributionHandle guideDist;

				if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
					rs.depth <= rc.maxGuidingDepth && !pS->isDelta )
				{
					if( rc.pGuidingField->InitDistribution( guideDist,
						ri.geometric.ptIntersection,
						rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() ) )
					{
						rc.pGuidingField->ApplyCosineProduct( guideDist, ri.geometric.vNormal );

						const Scalar alpha = rc.guidingAlpha;
						const Scalar xiG = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();

						if( xiG < alpha )
						{
							// Sample from the guiding distribution.
							Scalar guidePdf = 0;
							const Point2 xi2d(
								rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom(),
								rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() );
							const Vector3 guidedDir = rc.pGuidingField->Sample( guideDist, xi2d, guidePdf );

							if( guidePdf > NEARZERO )
							{
								const RISEPel fGuided = pBRDF->value( guidedDir, ri.geometric );
								const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
								const Scalar bsdfPdfGuided = pSPF ?
									pSPF->Pdf( ri.geometric, guidedDir, ior_stack ) : 0;
								const Scalar combinedPdf = alpha * guidePdf + (1.0 - alpha) * bsdfPdfGuided;

								if( combinedPdf > NEARZERO )
								{
									const Scalar cosTheta = fabs(
										Vector3Ops::Dot( guidedDir, ri.geometric.vNormal ) );
									throughput = fGuided * (cosTheta / combinedPdf);
									traceRay = Ray( pS->ray.origin, guidedDir );
									effectiveBsdfPdf = combinedPdf;
								}
								else
								{
									throughput = RISEPel( 0, 0, 0 );
								}
							}
							else
							{
								throughput = RISEPel( 0, 0, 0 );
							}
						}
						else
						{
							// Keep BSDF direction but adjust throughput for
							// the combined PDF.
							const Scalar guidePdfForBsdf = rc.pGuidingField->Pdf( guideDist, pS->ray.Dir() );
							const Scalar combinedPdf = alpha * guidePdfForBsdf + (1.0 - alpha) * pS->pdf;

							if( combinedPdf > NEARZERO )
							{
								throughput = pS->kray * (pS->pdf / combinedPdf);
								effectiveBsdfPdf = combinedPdf;
							}
						}
					}
				}

				// Training sample collection: record incident radiance
				// samples for the guiding field during training passes.
				const bool collectTrainingSample =
					rc.pGuidingField && !rc.pGuidingField->IsTrained() &&
					!pS->isDelta && pS->pdf > NEARZERO;
#endif // RISE_ENABLE_OPENPGL

				rs2.importance = rs.importance * ColorMath::MaxValue( throughput );
				rs2.bsdfPdf = effectiveBsdfPdf;

				if( pS->isDelta && bSMSEnabled ) {
					rs2.considerEmission = false;
				} else {
					rs2.considerEmission = true;
				}

				RISEPel cthis( 0, 0, 0 );
				Scalar hitDist = 0;
				traceRay.Advance( 1e-8 );
				caster.CastRay( rc, ri.geometric.rast, traceRay, cthis,
					rs2, &hitDist, ri.pRadianceMap,
					pS->ior_stack ? pS->ior_stack : ior_stack );
				c = c + (throughput * cthis);

#ifdef RISE_ENABLE_OPENPGL
				if( collectTrainingSample )
				{
					const Scalar lum = ColorMath::MaxValue( cthis );
					if( lum > 0 )
					{
						rc.pGuidingField->AddSample(
							ri.geometric.ptIntersection,
							traceRay.Dir(),
							hitDist > 0 ? hitDist : 1.0,
							pS->pdf,
							lum,
							false );
					}
				}
#endif
			}
		}
	}
}

Scalar PathTracingShaderOp::PerformOperationNM(
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
				const Scalar xi = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
				const ScatteredRay* pS = scattered.RandomlySelect( xi, true );
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
		IndependentSampler fallbackLumSamplerNM( rc.random );
		ISampler& lumSamplerNM = rc.pSampler ? *rc.pSampler : fallbackLumSamplerNM;

		c += pLumMgr->ComputeDirectLightingNM(
			ri, *pBRDF, nm, lumSamplerNM, caster, pScene->GetShadowMap() );
	}

	// 2c: SMS (spectral — per-wavelength IOR for dispersion)
	if( pSolver && rs.depth <= 1 )
	{
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
			const Scalar xi = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();
			const ScatteredRay* pS = scattered.RandomlySelect( xi, true );
			if( pS )
			{
				Ray traceRay = pS->ray;
				Scalar throughputNM = pS->krayNM;
				Scalar effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf;

#ifdef RISE_ENABLE_OPENPGL
				// Path guiding (spectral): same one-sample MIS logic
				static thread_local GuidingDistributionHandle guideDistNM;

				if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
					rs.depth <= rc.maxGuidingDepth && !pS->isDelta )
				{
					if( rc.pGuidingField->InitDistribution( guideDistNM,
						ri.geometric.ptIntersection,
						rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() ) )
					{
						rc.pGuidingField->ApplyCosineProduct( guideDistNM, ri.geometric.vNormal );

						const Scalar alpha = rc.guidingAlpha;
						const Scalar xiG = rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom();

						if( xiG < alpha )
						{
							Scalar guidePdf = 0;
							const Point2 xi2d(
								rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom(),
								rc.pSampler ? rc.pSampler->Get1D() : rc.random.CanonicalRandom() );
							const Vector3 guidedDir = rc.pGuidingField->Sample( guideDistNM, xi2d, guidePdf );

							if( guidePdf > NEARZERO )
							{
								const Scalar fGuided = pBRDF->valueNM( guidedDir, ri.geometric, nm );
								const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
								const Scalar bsdfPdfGuided = pSPF ?
									pSPF->Pdf( ri.geometric, guidedDir, ior_stack ) : 0;
								const Scalar combinedPdf = alpha * guidePdf + (1.0 - alpha) * bsdfPdfGuided;

								if( combinedPdf > NEARZERO )
								{
									const Scalar cosTheta = fabs(
										Vector3Ops::Dot( guidedDir, ri.geometric.vNormal ) );
									throughputNM = fGuided * cosTheta / combinedPdf;
									traceRay = Ray( pS->ray.origin, guidedDir );
									effectiveBsdfPdf = combinedPdf;
								}
								else
								{
									throughputNM = 0;
								}
							}
							else
							{
								throughputNM = 0;
							}
						}
						else
						{
							const Scalar guidePdfForBsdf = rc.pGuidingField->Pdf( guideDistNM, pS->ray.Dir() );
							const Scalar combinedPdf = alpha * guidePdfForBsdf + (1.0 - alpha) * pS->pdf;

							if( combinedPdf > NEARZERO )
							{
								throughputNM = pS->krayNM * (pS->pdf / combinedPdf);
								effectiveBsdfPdf = combinedPdf;
							}
						}
					}
				}

				const bool collectTrainingSampleNM =
					rc.pGuidingField && !rc.pGuidingField->IsTrained() &&
					!pS->isDelta && pS->pdf > NEARZERO;
#endif

				rs2.importance = rs.importance * fabs( throughputNM );
				rs2.bsdfPdf = effectiveBsdfPdf;

				if( pS->isDelta && bSMSEnabled ) {
					rs2.considerEmission = false;
				} else {
					rs2.considerEmission = true;
				}

				Scalar cthis = 0;
				Scalar hitDist = 0;
				traceRay.Advance( 1e-8 );
				caster.CastRayNM( rc, ri.geometric.rast, traceRay, cthis,
					rs2, nm, &hitDist, ri.pRadianceMap,
					pS->ior_stack ? pS->ior_stack : ior_stack );
				c += cthis * throughputNM;

#ifdef RISE_ENABLE_OPENPGL
				if( collectTrainingSampleNM )
				{
					if( fabs( cthis ) > 0 )
					{
						rc.pGuidingField->AddSample(
							ri.geometric.ptIntersection,
							traceRay.Dir(),
							hitDist > 0 ? hitDist : 1.0,
							pS->pdf,
							fabs( cthis ),
							false );
					}
				}
#endif
			}
		}
	}

	return c;
}
