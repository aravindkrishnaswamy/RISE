//////////////////////////////////////////////////////////////////////
//
//  VCMIntegrator.cpp - Vertex Connection and Merging integrator.
//
//    Owns a BDPTIntegrator as its subpath generator and provides
//    post-pass conversion routines that derive the SmallVCM running
//    quantities (dVCM / dVC / dVM) from a BDPTVertex array produced
//    by BDPT's Generate{Light,Eye}Subpath.
//
//    Step 4 lands ConvertLightSubpath.  Later steps add
//    ConvertEyeSubpath and the full-strategy evaluators.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "VCMIntegrator.h"
#include "BDPTIntegrator.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IEmitter.h"
#include "../Interfaces/ILight.h"
#include "../Interfaces/IObject.h"
#include "../Lights/LightSampler.h"
#include "../Rendering/LuminaryManager.h"
#include "../Utilities/PathVertexEval.h"
#include "../Utilities/BDPTUtilities.h"
#include "../Interfaces/ICamera.h"
#include "../Cameras/CameraUtilities.h"
#include "../Rendering/SplatFilm.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Local copy of BDPT's internal ray epsilon so VCM doesn't
	// touch BDPTIntegrator's private constants.
	static const Scalar VCM_RAY_EPSILON = 1e-6;

	// Local helper: unoccluded segment test.  Mirrors
	// BDPTIntegrator::IsVisible so VCM doesn't touch BDPT.
	inline bool VCMIsVisible( const IRayCaster& caster, const Point3& p1, const Point3& p2 )
	{
		Vector3 d = Vector3Ops::mkVector3( p2, p1 );
		const Scalar dist = Vector3Ops::Magnitude( d );
		if( dist < VCM_RAY_EPSILON ) {
			return true;
		}
		d = d * ( Scalar( 1 ) / dist );
		Ray shadowRay( p1, d );
		shadowRay.Advance( VCM_RAY_EPSILON );
		return !caster.CastShadowRay( shadowRay, dist - 2.0 * VCM_RAY_EPSILON );
	}
}

VCMIntegrator::VCMIntegrator(
	const unsigned int maxEyeDepth_,
	const unsigned int maxLightDepth_,
	const Scalar requestedMergeRadius_,
	const bool enableVC_,
	const bool enableVM_,
	const StabilityConfig& stabilityConfig_
	) :
	pGenerator( 0 ),
	maxEyeDepth( maxEyeDepth_ ),
	maxLightDepth( maxLightDepth_ ),
	stabilityConfig( stabilityConfig_ ),
	requestedMergeRadius( requestedMergeRadius_ ),
	enableVC( enableVC_ ),
	enableVM( enableVM_ )
{
	pGenerator = new BDPTIntegrator( maxEyeDepth_, maxLightDepth_, stabilityConfig_ );
}

VCMIntegrator::~VCMIntegrator()
{
	safe_release( pGenerator );
}

void VCMIntegrator::SetLightSampler( const LightSampler* pLS )
{
	if( pGenerator ) {
		pGenerator->SetLightSampler( pLS );
	}
}

//////////////////////////////////////////////////////////////////////
// AreaToSolidAngleFactor
//
// Returns the receiving-side Jacobian factor for inverting an
// area-measure PDF back to solid angle at vertex 'v'.
//
//   SURFACE / LIGHT: |cos(theta)| between the surface normal
//     and the direction from the adjacent vertex.
//   MEDIUM:  sigma_t_scalar (replaces |cos| in the area-measure
//     conversion; see Veach thesis Ch. 11).
//   CAMERA:  1.0 (BDPT's pdfRev on the camera vertex uses the
//     generator-side form that cancels to unity).
//
// 'dirFromAdjacent' is the unit direction FROM the adjacent vertex
// TO 'v'.  It is only used for SURFACE/LIGHT cosine computation.
//////////////////////////////////////////////////////////////////////
static inline Scalar AreaToSolidAngleFactor(
	const BDPTVertex& v,
	const Vector3& dirFromAdjacent
	)
{
	if( v.type == BDPTVertex::MEDIUM ) {
		return v.sigma_t_scalar;
	}
	if( v.type == BDPTVertex::CAMERA ) {
		return Scalar( 1 );
	}
	// SURFACE, LIGHT, or anything else with a normal
	return fabs( Vector3Ops::Dot( v.normal, dirFromAdjacent ) );
}

//////////////////////////////////////////////////////////////////////
// ConvertLightSubpath
//
// The reuse lynchpin.  BDPT's GenerateLightSubpath already produces
// a fully-decorated std::vector<BDPTVertex> that handles media,
// dispersion, BSSRDF entry, delta propagation, and Russian-roulette
// termination.  VCM doesn't re-implement any of that — it just walks
// the array once, inverts the area-measure pdfFwd back to solid
// angle using the cached cosAtGen field, and feeds the SmallVCM
// recurrence in lock-step.
//
// The critical identities:
//
//   bsdfDirPdfW_at_vertex_i = v[i].pdfFwd * distSq / cosAtGen
//     (inverse of BDPTUtilities::SolidAngleToArea applied at
//      generation time to v[i-1]'s outbound solid-angle pdf)
//
//   bsdfRevPdfW_at_vertex_i = v[i].pdfRev * distSq / cosAtGen
//     (pdfRev is populated by BDPT on the previous call when the
//      NEXT vertex is generated — the "retroactive" fill)
//
// One special case: the first bounce from a light.  SmallVCM's
// geometric update gates on "pathLength > 1 || isFiniteLight", i.e.
// the distance^2 factor is SKIPPED when the light is infinite
// (environment / directional).  RISE doesn't currently distinguish
// finite vs infinite at the BDPTVertex level, so we approximate:
// LIGHT vertices with finite pdfPosition are finite; delta-direction
// lights (sun / spot) are technically both finite and infinite at
// once.  We treat any BDPTVertex marked type==LIGHT as finite; the
// infinite case is covered when VCM adds environment-map support in
// a later step.
//
// Vertex types handled:
//
//   v[0] = LIGHT      -> InitLight(...)
//   v[i] = SURFACE    -> geometric + bsdf-sampling update
//   v[i] = MEDIUM     -> medium vertices have cosAtGen == 0 as a
//                        sentinel; we do NOT invert their PDF nor
//                        push them to the store (surface-only
//                        merging).  The recurrence quantities are
//                        left unchanged across the medium vertex —
//                        this is an approximation documented in the
//                        plan's "medium support deferred" note.
//   v[i] = CAMERA     -> never appears on a light subpath
//
// Special-flagged vertices (BSSRDF re-entry, random-walk SSS exits)
// have non-analytic pdfFwd values that don't correspond to a
// solid-angle BSDF pdf.  Their cosAtGen field is left at 0 by the
// generator, which triggers the "no inversion" path and excludes
// them from the store.
//////////////////////////////////////////////////////////////////////
void VCMIntegrator::ConvertLightSubpath(
	const std::vector<BDPTVertex>& verts,
	const VCMNormalization& norm,
	std::vector<LightVertex>& out,
	std::vector<VCMMisQuantities>* outMis
	)
{
	const std::size_t n = verts.size();
	if( n == 0 ) {
		return;
	}
	if( outMis ) {
		outMis->assign( n, VCMMisQuantities() );
	}

	VCMMisQuantities mis;

	for( std::size_t i = 0; i < n; i++ )
	{
		const BDPTVertex& v = verts[i];

		//
		// Initialization at the light endpoint.
		//
		if( i == 0 )
		{
			if( v.type != BDPTVertex::LIGHT ) {
				// Malformed subpath — abandon.
				if( outMis ) outMis->clear();
				return;
			}

			// directPdfA = v[0].pdfFwd (BDPT stores
			// pdfSelect * pdfPosition here).  emissionPdfW
			// is the combined area*solid-angle product.
			const bool isFinite = true;	// conservative: see comment block above
			mis = InitLight(
				v.pdfFwd,
				v.emissionPdfW,
				v.cosAtGen,
				isFinite,
				v.isDelta,
				norm );
			if( outMis ) (*outMis)[0] = mis;
			continue;
		}

		//
		// Non-endpoint vertices: geometric update, then, if this
		// isn't the tail vertex, a BSDF-sampling update.
		//

		// Skip vertices that lack a valid area-measure conversion
		// factor.  BSSRDF re-entry and random-walk SSS exit are
		// always skipped.  Their dVCM/dVC/dVM are frozen from the
		// previous vertex (propagated via outMis[i] = mis).
		//
		// MEDIUM vertices ARE propagated through both the geometric
		// update (using sigma_t_scalar in place of |cos|) and the
		// phase-function sampling update (using AreaToSolidAngleFactor
		// on adjacent vertices for pdf inversion).  They are NOT
		// stored in the light vertex store (surface-only merging).
		//
		// IMPORTANT: we DO NOT skip on `pdfFwd <= 0`.  BDPT sets
		// `pdfFwd = 0` as the Veach "delta transparency" marker on
		// any vertex that was arrived at via a specular scatter —
		// typical for the diffuse receiver of a glass/mirror
		// caustic chain.  Skipping those vertices here would drop
		// every post-specular light vertex from the store, making
		// VM unable to catch caustics at all.
		const bool isMedium = ( v.type == BDPTVertex::MEDIUM );
		const bool skipRecurrence = ( v.type != BDPTVertex::SURFACE && !isMedium ) ||
		                            ( v.isBSSRDFEntry );

		// Surface vertices use cosAtGen; medium vertices use
		// sigma_t_scalar (the area-measure Jacobian factor).
		// Both must be positive for the geometric update.
		const Scalar cosFix = isMedium ? v.sigma_t_scalar : v.cosAtGen;
		if( skipRecurrence || cosFix <= 0 ) {
			if( outMis ) (*outMis)[i] = mis;
			continue;
		}

		// Reconstruct the solid-angle bsdf pdfs from the stored
		// area-measure fields using the cached receiving-side
		// cosine at this vertex.  Inverts
		// BDPTUtilities::SolidAngleToArea.
		//
		// Note: Vector3Ops::mkVector3(a, b) returns a - b (yes,
		// really — the arg names in the prototype are misleading).
		// So to get "direction FROM prev TO current" we pass the
		// current position first.
		const BDPTVertex& prev = verts[i - 1];
		const Vector3 step = Vector3Ops::mkVector3( v.position, prev.position );
		const Scalar distSq = Vector3Ops::SquaredModulus( step );
		if( distSq <= 0 ) {
			if( outMis ) (*outMis)[i] = mis;
			continue;
		}

		// Geometric update.  For the first bounce from the light
		// (i==1) the distance-squared gate should be skipped for
		// infinite lights.  All RISE lights currently routed
		// through GenerateLightSubpath vertex 0 are treated as
		// finite; keep the gate on for every bounce.
		const bool applyDistSqToDVCM = true;
		mis = ApplyGeometricUpdate( mis, distSq, cosFix, applyDistSqToDVCM );

		if( outMis ) (*outMis)[i] = mis;

		// Medium vertices: geometric update applied above.
		// Skip store append (surface-only merging) but DO apply
		// the phase-function sampling update so dVCM/dVC/dVM are
		// correct at the next vertex.  Phase functions are never
		// delta, so we always use the non-specular branch.
		if( isMedium ) {
			if( i + 1 < n )
			{
				const BDPTVertex& next = verts[i + 1];
				const Vector3 nextStep = Vector3Ops::mkVector3( next.position, v.position );
				const Scalar nextDistSq = Vector3Ops::SquaredModulus( nextStep );
				if( nextDistSq > 0 && next.pdfFwd > 0 )
				{
					const Scalar nextDist = std::sqrt( nextDistSq );
					const Vector3 wo = nextStep * ( Scalar( 1 ) / nextDist );
					const Scalar cosThetaOutMed = v.sigma_t_scalar;

					const Scalar nextFactor = AreaToSolidAngleFactor( next, -wo );
					const Scalar bsdfDirPdfW = ( nextFactor > 0 )
						? next.pdfFwd * nextDistSq / nextFactor : Scalar( 0 );

					const Scalar invDist = ( distSq > 0 ) ? ( Scalar( 1 ) / std::sqrt( distSq ) ) : Scalar( 0 );
					const Vector3 dirPrevToCur = step * invDist;
					const Scalar prevFactor = AreaToSolidAngleFactor( prev, dirPrevToCur );
					const Scalar bsdfRevPdfW = ( prev.pdfRev > 0 && prevFactor > 0 )
						? prev.pdfRev * distSq / prevFactor : Scalar( 0 );

					mis = ApplyBsdfSamplingUpdate(
						mis, cosThetaOutMed, bsdfDirPdfW, bsdfRevPdfW,
						false, norm );
				}
			}
			continue;
		}

		//
		// Emit a LightVertex record into 'out' BEFORE the
		// BSDF-sampling update, because the store is supposed to
		// hold the running quantities at the vertex being merged
		// — the SmallVCM merge formula reads the same "after
		// geometric update" state.  The post-bsdf update
		// prepares dVCM/dVC/dVM for the NEXT vertex, not for
		// this one.
		//
		if( v.isConnectible )
		{
			LightVertex lv;
			lv.ptPosition = v.position;
			lv.plane      = 0;
			lv.flags      = 0;
			if( v.isDelta        ) lv.flags |= kLVF_IsDelta;
			if( v.isConnectible  ) lv.flags |= kLVF_IsConnectible;
			if( v.isBSSRDFEntry  ) lv.flags |= kLVF_IsBSSRDFEntry;
			lv.pathLength = static_cast<unsigned short>( i );
			lv.normal     = v.normal;
			// Direction FROM the previous vertex TO this one.
			// At merge time the BSDF at the eye-side vertex is
			// evaluated with wi = -lv.wi so we store the
			// arrival direction here.
			if( distSq > 0 ) {
				const Scalar d = std::sqrt( distSq );
				lv.wi = step * ( Scalar( 1 ) / d );
			} else {
				lv.wi = Vector3( 0, 0, 0 );
			}
			lv.pMaterial  = v.pMaterial;
			lv.pObject    = v.pObject;
			lv.throughput = v.throughput;
			lv.mis        = mis;
			out.push_back( lv );
		}

		// BSDF-sampling update: prepare (dVCM, dVC, dVM) for the
		// NEXT vertex.  Requires the onward direction, which is
		// only available if a next vertex exists in the array.
		if( i + 1 < n )
		{
			const BDPTVertex& next = verts[i + 1];
			// Direction FROM v TO next: mkVector3(next, v).
			const Vector3 nextStep = Vector3Ops::mkVector3( next.position, v.position );
			const Scalar nextDistSq = Vector3Ops::SquaredModulus( nextStep );
			if( nextDistSq <= 0 ) {
				continue;
			}
			const Scalar nextDist = std::sqrt( nextDistSq );
			const Vector3 wo = nextStep * ( Scalar( 1 ) / nextDist );
			const Scalar cosThetaOut = fabs( Vector3Ops::Dot( v.normal, wo ) );

			if( v.isDelta ) {
				// Specular scatter at this vertex.  The SmallVCM
				// specular branch of ApplyBsdfSamplingUpdate needs
				// ONLY cosThetaOut — it ignores bsdfDirPdfW /
				// bsdfRevPdfW because those are Dirac distributions
				// that don't contribute finite solid-angle pdfs.
				// Crucially, we MUST still apply this update so that
				// dVCM is zeroed (the SmallVCM specular transparency
				// convention), otherwise the downstream merge/
				// connection weights at post-specular vertices are
				// wildly overestimated.
				mis = ApplyBsdfSamplingUpdate(
					mis,
					cosThetaOut,
					Scalar( 0 ),	// bsdfDirPdfW unused in specular branch
					Scalar( 0 ),	// bsdfRevPdfW unused in specular branch
					true,
					norm );
				continue;
			}

			// Non-specular BSDF update.  Requires the area-measure
			// forward pdf stored on the NEXT vertex, which is only
			// valid if THIS vertex was sampled non-specularly
			// (otherwise BDPT stores next.pdfFwd = 0 as the Veach
			// delta-transparency marker).
			if( next.pdfFwd <= 0 ) {
				continue;
			}
			// AreaToSolidAngleFactor handles SURFACE (cosAtGen),
			// MEDIUM (sigma_t_scalar), and CAMERA (1.0).
			const Vector3 dirCurToNext = wo;  // already unit from above
			const Scalar nextFactor = AreaToSolidAngleFactor( next, -dirCurToNext );
			if( nextFactor <= 0 ) {
				continue;
			}
			const Scalar bsdfDirPdfW_out = next.pdfFwd * nextDistSq / nextFactor;

			// Reverse bsdf pdf at THIS vertex for the direction
			// back toward 'prev'.  prev.pdfRev is in area measure
			// at 'prev'; invert with the outgoing-side Jacobian
			// factor at 'prev' for the direction prev→v.
			const Scalar invDist = ( distSq > 0 ) ? ( Scalar( 1 ) / std::sqrt( distSq ) ) : Scalar( 0 );
			const Vector3 dirPrevToCur = step * invDist;
			const Scalar prevFactor = AreaToSolidAngleFactor( prev, dirPrevToCur );

			Scalar bsdfRevPdfW_out = 0;
			if( prev.pdfRev > 0 && prevFactor > 0 ) {
				bsdfRevPdfW_out = prev.pdfRev * distSq / prevFactor;
			}

			mis = ApplyBsdfSamplingUpdate(
				mis,
				cosThetaOut,
				bsdfDirPdfW_out,
				bsdfRevPdfW_out,
				false,
				norm );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateS0 — eye path hits emitter directly
//
// Iterates every eye vertex at index i >= 1 (t = i+1 >= 2 in
// SmallVCM notation) that is a non-delta SURFACE carrying an
// IEmitter material.  For each such vertex, evaluates the
// SmallVCM MIS weight (balance heuristic via VCMMis) using the running quantities
// eyeMis[i] computed in ConvertEyeSubpath:
//
//   directPdfA      = pdfSelect * pdfPosition
//   emissionPdfW    = directPdfA * emissionDirPdfSA
//                     where emissionDirPdfSA is the solid-angle
//                     emission pdf at the emitter toward the
//                     previous eye vertex (matches what BDPT
//                     would compute at the light endpoint)
//   wCamera         = directPdfA * eyeMis[i].dVCM
//                   + emissionPdfW * eyeMis[i].dVC
//   w               = 1 / (1 + wCamera)
//   contribution    += v[i].throughput * Le * w
//
// Delta eye vertices are skipped because the pre-hit BSDF sample
// picked a specular direction and cannot coincide with any explicit
// connection strategy; in SmallVCM the specular branch of the
// recurrence zeros dVCM, so the weight already collapses to 1.
// We still apply the formula uniformly — the specular vertex
// contribution is unchanged by the wCamera=0 collapse.
//////////////////////////////////////////////////////////////////////
RISEPel VCMIntegrator::EvaluateS0(
	const IScene& scene,
	const IRayCaster& caster,
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const VCMNormalization& /*norm*/
	) const
{
	RISEPel total( 0, 0, 0 );

	if( eyeVerts.size() != eyeMis.size() || eyeVerts.size() < 2 ) {
		return total;
	}

	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) {
		return total;
	}

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager::LuminariesList& luminaries = pLumManager
		? const_cast<LuminaryManager*>( pLumManager )->getLuminaries()
		: emptyList;

	for( std::size_t i = 1; i < eyeVerts.size(); i++ )
	{
		const BDPTVertex& v = eyeVerts[i];
		if( v.type != BDPTVertex::SURFACE || !v.pMaterial || !v.pObject ) {
			continue;
		}

		const IEmitter* pEmitter = v.pMaterial->GetEmitter();
		if( !pEmitter ) {
			continue;
		}

		// Direction from the emitter toward the previous eye
		// vertex — this is the direction in which we evaluate Le.
		const BDPTVertex& prev = eyeVerts[i - 1];
		Vector3 woFromEmitter = Vector3Ops::mkVector3( prev.position, v.position );
		const Scalar d = Vector3Ops::Magnitude( woFromEmitter );
		if( d <= 0 ) {
			continue;
		}
		woFromEmitter = woFromEmitter * ( Scalar( 1 ) / d );

		// Le at the emitter in that direction.
		RayIntersectionGeometric rig( Ray( v.position, woFromEmitter ), nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = v.position;
		rig.vNormal = v.normal;
		rig.onb = v.onb;
		const RISEPel Le = pEmitter->emittedRadiance( rig, woFromEmitter, v.normal );
		if( ColorMath::MaxValue( Le ) <= 0 ) {
			continue;
		}

		// Area-measure NEE pdf: pdfSelect * pdfPosition.
		const Scalar pdfSelect = pLS->PdfSelectLuminary(
			scene, luminaries, *v.pObject, prev.position, prev.normal );
		const Scalar area = v.pObject->GetArea();
		if( area <= 0 ) {
			continue;
		}
		const Scalar pdfPosition = Scalar( 1 ) / area;
		const Scalar directPdfA = pdfSelect * pdfPosition;

		// Emission solid-angle pdf at the emitter toward prev.
		// Mesh luminaries use cosine-weighted hemisphere emission.
		const Scalar cosAtEmitter = Vector3Ops::Dot( v.normal, woFromEmitter );
		const Scalar emissionDirPdfSA = ( cosAtEmitter > 0 )
			? ( cosAtEmitter * INV_PI )
			: Scalar( 0 );

		// SmallVCM "emissionPdfW" is the combined area * solid-angle product.
		const Scalar emissionPdfW = directPdfA * emissionDirPdfSA;

		// SmallVCM MIS weight (balance heuristic via VCMMis).
		const Scalar wCamera = directPdfA * eyeMis[i].dVCM + emissionPdfW * eyeMis[i].dVC;
		const Scalar weight = Scalar( 1 ) / ( VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

		total = total + ( v.throughput * Le * weight );
	}

	return total;
}

//////////////////////////////////////////////////////////////////////
// EvaluateNEE — per-vertex next event estimation (s=1)
//
// Mirrors BDPT's s=1 strategy but samples a FRESH light per eye
// vertex (SmallVCM convention), not reusing the one already sampled
// at the start of the light subpath.  The SmallVCM MIS weights
// accumulate over the shared dVCM/dVC values stored in eyeMis.
//
// Per eye vertex at index i (t = i+1 >= 2):
//   if eye vertex is non-delta SURFACE with a valid BSDF:
//     sample a light via pLightSampler->SampleLight
//     shadow-test the connection
//     evaluate Le, BSDF, geometric term
//     compute forward / reverse bsdf pdfs via PathVertexEval
//     build SmallVCM weight  (wLight + 1 + wCamera)^{-1}
//     accumulate v.throughput * fEye * Le * Tr * (G / pdfLightArea) * w
//
// For v1 we leave connection transmittance Tr at 1 (the eye vertex's
// medium is assumed not to occlude the NEE segment); Step 5's
// integration of null-scattering volumes is deferred per the plan.
//////////////////////////////////////////////////////////////////////
RISEPel VCMIntegrator::EvaluateNEE(
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const VCMNormalization& norm
	) const
{
	RISEPel total( 0, 0, 0 );

	if( eyeVerts.size() != eyeMis.size() || eyeVerts.size() < 2 ) {
		return total;
	}

	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) {
		return total;
	}

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager::LuminariesList& luminaries = pLumManager
		? const_cast<LuminaryManager*>( pLumManager )->getLuminaries()
		: emptyList;

	for( std::size_t i = 1; i < eyeVerts.size(); i++ )
	{
		const BDPTVertex& v = eyeVerts[i];
		if( v.type != BDPTVertex::SURFACE || !v.pMaterial ) {
			continue;
		}
		if( !v.isConnectible ) {
			continue;
		}

		// Start a dedicated sampler stream per eye vertex so the
		// light-selection dimensions don't collide with the subpath
		// generator's.  Stream indices beyond 31 roll over; VCM
		// NEE uses streams 48 + depth for clear separation.
		sampler.StartStream( 48 + static_cast<unsigned int>( i ) );

		LightSample ls;
		if( !pLS->SampleLight( scene, luminaries, sampler, ls ) ) {
			continue;
		}
		if( ls.pdfSelect <= 0 || ls.pdfPosition <= 0 ) {
			continue;
		}

		// Direction from the eye vertex to the light position.
		// Note mkVector3 returns a-b, so mkVector3(light, eye) = light-eye.
		Vector3 dirToLight = Vector3Ops::mkVector3( ls.position, v.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToLight );
		if( dist < VCM_RAY_EPSILON ) {
			continue;
		}
		dirToLight = dirToLight * ( Scalar( 1 ) / dist );
		const Scalar distSq = dist * dist;

		// Visibility.
		if( !VCMIsVisible( caster, v.position, ls.position ) ) {
			continue;
		}

		// Direction at the eye vertex back toward the previous
		// eye vertex (the woAtEye the BSDF uses).
		const BDPTVertex& prev = eyeVerts[i - 1];
		Vector3 woAtEye = Vector3Ops::mkVector3( prev.position, v.position );
		const Scalar woDist = Vector3Ops::Magnitude( woAtEye );
		if( woDist < VCM_RAY_EPSILON ) {
			continue;
		}
		woAtEye = woAtEye * ( Scalar( 1 ) / woDist );

		// Evaluate Le at the light toward the eye vertex (direction -dirToLight).
		RISEPel Le( 0, 0, 0 );
		if( ls.pLight ) {
			Le = ls.pLight->emittedRadiance( -dirToLight );
		} else if( ls.pLuminary && ls.pLuminary->GetMaterial() ) {
			const IEmitter* pEmitter = ls.pLuminary->GetMaterial()->GetEmitter();
			if( pEmitter ) {
				RayIntersectionGeometric rig( Ray( ls.position, -dirToLight ), nullRasterizerState );
				rig.bHit = true;
				rig.ptIntersection = ls.position;
				rig.vNormal = ls.normal;
				Le = pEmitter->emittedRadiance( rig, -dirToLight, ls.normal );
			}
		}
		if( ColorMath::MaxValue( Le ) <= 0 ) {
			continue;
		}

		// BSDF at the eye vertex for (wi=dirToLight, wo=woAtEye).
		const RISEPel fEye = PathVertexEval::EvalBSDFAtVertex( v, dirToLight, woAtEye );
		if( ColorMath::MaxValue( fEye ) <= 0 ) {
			continue;
		}

		// Forward/reverse solid-angle BSDF pdfs at eye vertex.
		const Scalar bsdfDirPdfW = PathVertexEval::EvalPdfAtVertex( v, woAtEye, dirToLight );
		const Scalar bsdfRevPdfW = PathVertexEval::EvalPdfAtVertex( v, dirToLight, woAtEye );

		// Geometric term.  Delta lights have no surface area, so
		// their light-side cosine drops out.
		const Scalar cosAtEye = fabs( Vector3Ops::Dot( v.normal, dirToLight ) );
		Scalar cosAtLight = 0;
		Scalar G = 0;
		if( ls.isDelta ) {
			cosAtLight = 1;
			G = cosAtEye / distSq;
		} else {
			cosAtLight = fabs( Vector3Ops::Dot( ls.normal, -dirToLight ) );
			G = ( cosAtEye * cosAtLight ) / distSq;
		}
		if( cosAtEye <= 0 || G <= 0 ) {
			continue;
		}

		// SmallVCM directPdfW: solid-angle pdf at the eye vertex of
		// sampling this light position.  For delta lights the
		// "direct" pdf is degenerate; we treat it as 1 for the
		// weight math since delta lights have no competing NEE.
		const Scalar directPdfW = ls.isDelta
			? Scalar( 1 )
			: ( ls.pdfPosition * distSq / cosAtLight );

		// SmallVCM emissionPdfW: combined area+solid-angle product
		// pdf matching what would be computed at the light endpoint
		// if this path had been sampled from the light side.
		Scalar emissionDirPdfSA = 0;
		if( ls.pLuminary ) {
			// Cosine-weighted hemisphere emission on mesh lights.
			const Scalar c = Vector3Ops::Dot( ls.normal, -dirToLight );
			emissionDirPdfSA = ( c > 0 ) ? ( c * INV_PI ) : Scalar( 0 );
		} else if( ls.pLight ) {
			emissionDirPdfSA = ls.pLight->pdfDirection( -dirToLight );
		}
		const Scalar emissionPdfW = ls.pdfSelect * ls.pdfPosition * emissionDirPdfSA;

		const Scalar lightPickProb = ls.pdfSelect;

		// SmallVCM MIS weight (balance heuristic via VCMMis).
		const Scalar wLight = bsdfDirPdfW / ( lightPickProb * directPdfW );
		Scalar wCamera = 0;
		if( directPdfW > 0 && cosAtLight > 0 ) {
			const Scalar camFactor =
				( emissionPdfW * cosAtEye ) / ( directPdfW * cosAtLight );
			wCamera = camFactor * (
				norm.mMisVmWeightFactor
				+ eyeMis[i].dVCM
				+ eyeMis[i].dVC * bsdfRevPdfW );
		}
		const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

		// Contribution.  BDPT's formulation:
		//   contrib = fEye * Le * G / (pdfSelect * pdfPosition)
		// which is mathematically equivalent to
		// fEye * Le * cosAtEye / (lightPickProb * directPdfW)
		// when directPdfW is in solid angle.
		const Scalar invLightPdfArea = ls.pdfSelect * ls.pdfPosition;
		if( invLightPdfArea <= 0 ) {
			continue;
		}

		const RISEPel contribution = v.throughput * fEye * Le * ( G / invLightPdfArea ) * weight;
		total = total + contribution;
	}

	return total;
}

//////////////////////////////////////////////////////////////////////
// SplatLightSubpathToCamera — per-vertex t=1 strategy
//
// For each non-delta light-subpath vertex (surface OR the light
// itself), projects onto the camera sensor, shadow-tests the
// connection, evaluates the BSDF/emitter toward the camera, and
// splats the MIS-weighted contribution to the splat film.
//
// SmallVCM weight formula for t=1:
//   wLight  = (cameraPdfA / mLightSubPathCount)
//           * (mMisVmWeightFactor
//              + L.dVCM
//              + L.dVC * bsdfRevPdfW)
//   w       = 1 / (wLight + 1)
//
// where:
//   cameraPdfA   = solid-angle camera pdf * cosAtLight / dist^2
//                  (area measure at the light vertex)
//   bsdfRevPdfW  = BSDF pdf at the light vertex for the reverse
//                  direction (from light to previous light vertex),
//                  zero for the light endpoint itself (s=1)
//
// For s=1 (the LIGHT-type vertex splatting to camera) we skip the
// BSDF evaluation and use the emitter radiance directly.  The
// contribution then divides by pdfLight = v[0].pdfFwd in area
// measure.
//////////////////////////////////////////////////////////////////////
void VCMIntegrator::SplatLightSubpathToCamera(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<VCMMisQuantities>& lightMis,
	const IScene& /*scene*/,
	const IRayCaster& caster,
	const ICamera& camera,
	SplatFilm& splatFilm,
	const VCMNormalization& norm
	) const
{
	if( lightVerts.size() != lightMis.size() || lightVerts.empty() ) {
		return;
	}
	if( norm.mLightSubPathCount <= 0 ) {
		return;
	}

	const Point3 camPos = camera.GetLocation();

	for( std::size_t i = 0; i < lightVerts.size(); i++ )
	{
		const BDPTVertex& v = lightVerts[i];

		// Skip non-connectible vertices: they have only delta BxDF
		// components, so the BSDF evaluated toward the camera
		// direction will be zero.  Vertices that are isConnectible
		// but happened to sample a delta lobe (isDelta=true) still
		// have a non-delta component that can scatter toward the
		// camera, so we let them through.
		if( !v.isConnectible ) {
			continue;
		}

		// Skip unsupported types (MEDIUM merging is out of scope
		// for v1).  LIGHT and SURFACE are handled below.
		if( v.type != BDPTVertex::LIGHT && v.type != BDPTVertex::SURFACE ) {
			continue;
		}

		// Project onto the camera sensor.
		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, v.position, rasterPos ) ) {
			continue;
		}

		// Visibility test between light vertex and the camera position.
		if( !VCMIsVisible( caster, v.position, camPos ) ) {
			continue;
		}

		// Direction from light vertex to camera.
		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, v.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < VCM_RAY_EPSILON ) {
			continue;
		}
		dirToCam = dirToCam * ( Scalar( 1 ) / dist );
		const Scalar distSq = dist * dist;

		// Camera importance We for this direction.
		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );
		if( We <= 0 ) {
			continue;
		}

		// Geometric term: surface cos / dist^2.  For the LIGHT
		// endpoint we still have a normal (mesh luminaries emit
		// cosine-weighted from their surface) and for delta-position
		// lights we'd skip here via isDelta above.
		const Scalar cosAtLight = fabs( Vector3Ops::Dot( v.normal, dirToCam ) );
		if( cosAtLight <= 0 ) {
			continue;
		}
		const Scalar G = cosAtLight / distSq;

		// Camera directional pdf at this light vertex.
		const Scalar camPdfDirSA = BDPTCameraUtilities::PdfDirection( camera, camRay );
		if( camPdfDirSA <= 0 ) {
			continue;
		}
		const Scalar cameraPdfA = camPdfDirSA * cosAtLight / distSq;

		// Two cases: LIGHT endpoint (s=1) or SURFACE (s>=2).
		RISEPel contribution( 0, 0, 0 );
		Scalar bsdfRevPdfW = 0;

		if( v.type == BDPTVertex::LIGHT )
		{
			// s=1: emitter directly visible.  Re-evaluate Le in
			// the direction toward the camera.
			RISEPel Le( 0, 0, 0 );
			if( v.pLight ) {
				Le = v.pLight->emittedRadiance( dirToCam );
			} else if( v.pLuminary && v.pLuminary->GetMaterial() ) {
				const IEmitter* pEmitter = v.pLuminary->GetMaterial()->GetEmitter();
				if( pEmitter ) {
					RayIntersectionGeometric rig(
						Ray( v.position, dirToCam ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = v.position;
					rig.vNormal = v.normal;
					rig.onb = v.onb;
					Le = pEmitter->emittedRadiance( rig, dirToCam, v.normal );
				}
			}
			if( ColorMath::MaxValue( Le ) <= 0 ) {
				continue;
			}

			// Contribution = Le * G * We / pdfLightPosition
			const Scalar pdfLightArea = v.pdfFwd;	// pdfSelect * pdfPosition
			if( pdfLightArea <= 0 ) {
				continue;
			}
			contribution = Le * ( G * We / pdfLightArea );
			bsdfRevPdfW = 0;	// no previous direction at the light
		}
		else	// SURFACE, s>=2
		{
			if( !v.pMaterial ) {
				continue;
			}
			if( i < 1 ) {
				continue;
			}
			const BDPTVertex& prev = lightVerts[i - 1];
			// wi convention: "away from surface TOWARD light source".
			// For a light subpath vertex, the source-side direction
			// points back to the previous light vertex, i.e. prev-v.
			Vector3 wiAtLight = Vector3Ops::mkVector3( prev.position, v.position );
			const Scalar wiDist = Vector3Ops::Magnitude( wiAtLight );
			if( wiDist < VCM_RAY_EPSILON ) {
				continue;
			}
			wiAtLight = wiAtLight * ( Scalar( 1 ) / wiDist );

			// BSDF at the light vertex for (wi=wiAtLight, wo=dirToCam).
			const RISEPel fLight = PathVertexEval::EvalBSDFAtVertex( v, wiAtLight, dirToCam );
			if( ColorMath::MaxValue( fLight ) <= 0 ) {
				continue;
			}

			// Reverse bsdf pdf at this vertex (from camera
			// direction back to the previous light direction).
			bsdfRevPdfW = PathVertexEval::EvalPdfAtVertex( v, dirToCam, wiAtLight );

			contribution = v.throughput * fLight * ( G * We );
		}

		// SmallVCM MIS weight (balance heuristic via VCMMis) for t=1.
		const Scalar wLight =
			( cameraPdfA / norm.mLightSubPathCount ) *
			( norm.mMisVmWeightFactor + lightMis[i].dVCM + lightMis[i].dVC * bsdfRevPdfW );
		const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) );

		const RISEPel weighted = contribution * weight;

		// Rasterize returns screen coordinates where y=0 is the
		// bottom of the image; SplatFilm uses y=0 at top.
		const int sx = static_cast<int>( rasterPos.x );
		const int sy = static_cast<int>( camera.GetHeight() - rasterPos.y );
		if( sx < 0 || sy < 0 ||
			static_cast<unsigned int>( sx ) >= camera.GetWidth() ||
			static_cast<unsigned int>( sy ) >= camera.GetHeight() ) {
			continue;
		}

		splatFilm.Splat( sx, sy, weighted );
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateInteriorConnections — strategy (s>=2, t>=2)
//
// For every pair of non-delta SURFACE vertices on the light and eye
// subpaths, attempt an explicit connection and accumulate the
// MIS-weighted contribution.  SmallVCM interior weight (both sides
// are balanced factors of dVCM/dVC plus the mMisVmWeightFactor term
// that accounts for merging at either endpoint):
//
//   wLight  = cameraBsdfDirPdfA
//           * (mMisVmWeightFactor
//              + L.dVCM
//              + L.dVC * lightBsdfRevPdfW)
//   wCamera = lightBsdfDirPdfA
//           * (mMisVmWeightFactor
//              + C.dVCM
//              + C.dVC * cameraBsdfRevPdfW)
//   w       = 1 / (wLight + 1 + wCamera)
//
// where:
//   cameraBsdfDirPdfA = area-measure pdf of sampling the light
//                        vertex from the eye vertex via the eye's
//                        BSDF (at the eye vertex)
//   lightBsdfDirPdfA  = same, but at the light vertex sampling the
//                        eye vertex
//
// The contribution uses BDPT's formula:
//   contrib = lightEnd.throughput * fLight * G * fEye * eyeEnd.throughput * w
//
// Interior connections never produce splats; they always land in
// the current pixel's sampleColor.
//////////////////////////////////////////////////////////////////////
RISEPel VCMIntegrator::EvaluateInteriorConnections(
	const IScene& /*scene*/,
	const IRayCaster& caster,
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<VCMMisQuantities>& lightMis,
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const VCMNormalization& norm
	) const
{
	RISEPel total( 0, 0, 0 );

	if( lightVerts.size() != lightMis.size() || eyeVerts.size() != eyeMis.size() ) {
		return total;
	}

	// Iterate s >= 2 and t >= 2.  s is the number of light subpath
	// vertices including the light itself; the light endpoint is
	// at index s-1.  SmallVCM's interior formula requires the
	// light endpoint to have a valid BSDF, which excludes the
	// LIGHT-type vertex 0.  So our loop starts at i=1 (s>=2).
	for( std::size_t i = 1; i < lightVerts.size(); i++ )
	{
		const BDPTVertex& lv = lightVerts[i];
		if( lv.type != BDPTVertex::SURFACE || !lv.pMaterial ) {
			continue;
		}
		if( !lv.isConnectible ) {
			continue;
		}

		// Direction from lv to its own previous vertex (for
		// evaluating the BSDF at lv).  lightVerts[i-1] exists by
		// the i>=1 constraint.
		const BDPTVertex& lvPrev = lightVerts[i - 1];
		Vector3 wiAtLight = Vector3Ops::mkVector3( lvPrev.position, lv.position );
		const Scalar wiLightDist = Vector3Ops::Magnitude( wiAtLight );
		if( wiLightDist < VCM_RAY_EPSILON ) {
			continue;
		}
		wiAtLight = wiAtLight * ( Scalar( 1 ) / wiLightDist );

		for( std::size_t j = 1; j < eyeVerts.size(); j++ )
		{
			const BDPTVertex& ev = eyeVerts[j];
			if( ev.type != BDPTVertex::SURFACE || !ev.pMaterial ) {
				continue;
			}
			if( !ev.isConnectible ) {
				continue;
			}

			// Direction from ev back to its own previous vertex.
			const BDPTVertex& evPrev = eyeVerts[j - 1];
			Vector3 woAtEye = Vector3Ops::mkVector3( evPrev.position, ev.position );
			const Scalar woEyeDist = Vector3Ops::Magnitude( woAtEye );
			if( woEyeDist < VCM_RAY_EPSILON ) {
				continue;
			}
			woAtEye = woAtEye * ( Scalar( 1 ) / woEyeDist );

			// Connection vector between the two endpoints.
			Vector3 lightToEye = Vector3Ops::mkVector3( ev.position, lv.position );
			const Scalar dist = Vector3Ops::Magnitude( lightToEye );
			if( dist < VCM_RAY_EPSILON ) {
				continue;
			}
			lightToEye = lightToEye * ( Scalar( 1 ) / dist );
			const Scalar distSq = dist * dist;

			// Visibility test.
			if( !VCMIsVisible( caster, lv.position, ev.position ) ) {
				continue;
			}

			// BSDF evaluations.
			// At the LIGHT endpoint: incoming = wiAtLight (from
			// its own previous vertex), outgoing = lightToEye.
			// At the EYE endpoint: incoming = -lightToEye (from
			// the light direction), outgoing = woAtEye (toward
			// previous eye vertex).
			const RISEPel fLight = PathVertexEval::EvalBSDFAtVertex( lv, wiAtLight, lightToEye );
			if( ColorMath::MaxValue( fLight ) <= 0 ) {
				continue;
			}
			const RISEPel fEye = PathVertexEval::EvalBSDFAtVertex( ev, -lightToEye, woAtEye );
			if( ColorMath::MaxValue( fEye ) <= 0 ) {
				continue;
			}

			// Geometric term (both sides are surface vertices
			// in v1; medium coupling is deferred).
			const Scalar cosAtLight = fabs( Vector3Ops::Dot( lv.normal, lightToEye ) );
			const Scalar cosAtEye   = fabs( Vector3Ops::Dot( ev.normal, -lightToEye ) );
			if( cosAtLight <= 0 || cosAtEye <= 0 ) {
				continue;
			}
			const Scalar G = ( cosAtLight * cosAtEye ) / distSq;

			// Forward direction bsdf pdfs in solid angle.
			// lightBsdfDirPdfW: at the light endpoint, for direction
			// toward the eye endpoint.
			// cameraBsdfDirPdfW: at the eye endpoint, for direction
			// toward the light endpoint.
			const Scalar lightBsdfDirPdfW =
				PathVertexEval::EvalPdfAtVertex( lv, wiAtLight, lightToEye );
			const Scalar cameraBsdfDirPdfW =
				PathVertexEval::EvalPdfAtVertex( ev, woAtEye, -lightToEye );

			// Reverse-direction pdfs: evaluating each BSDF for
			// the "opposite" direction pair.
			const Scalar lightBsdfRevPdfW =
				PathVertexEval::EvalPdfAtVertex( lv, lightToEye, wiAtLight );
			const Scalar cameraBsdfRevPdfW =
				PathVertexEval::EvalPdfAtVertex( ev, -lightToEye, woAtEye );

			// Convert both forward bsdf pdfs to AREA measure:
			// lightBsdfDirPdfA at the eye endpoint.
			const Scalar cameraBsdfDirPdfA =
				BDPTUtilities::SolidAngleToArea( cameraBsdfDirPdfW, cosAtLight, distSq );
			const Scalar lightBsdfDirPdfA =
				BDPTUtilities::SolidAngleToArea( lightBsdfDirPdfW, cosAtEye, distSq );

			// SmallVCM interior MIS weight.
			const Scalar wLight = cameraBsdfDirPdfA
				* ( norm.mMisVmWeightFactor
				    + lightMis[i].dVCM
				    + lightMis[i].dVC * lightBsdfRevPdfW );
			const Scalar wCamera = lightBsdfDirPdfA
				* ( norm.mMisVmWeightFactor
				    + eyeMis[j].dVCM
				    + eyeMis[j].dVC * cameraBsdfRevPdfW );
			const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

			// Contribution.  BDPT pattern:
			//   lightEnd.throughput * fLight * G * fEye * eyeEnd.throughput
			const RISEPel contrib = lv.throughput * fLight * ( G * weight ) * fEye * ev.throughput;
			total = total + contrib;
		}
	}

	return total;
}

//////////////////////////////////////////////////////////////////////
// ConvertEyeSubpath
//
// Symmetric to ConvertLightSubpath.  Differences:
//
//   - v[0] is CAMERA, not LIGHT.  Init uses InitCamera with
//     cameraPdfW = v[0].emissionPdfW (BDPT stores pdfCamDir here
//     via the Step 1 field population).
//   - The first bounce always applies the distance-squared term
//     to dVCM (applyDistSqToDVCM = true at i=1).  Cameras are
//     always at a finite point, so there's no infinite-light gate.
//   - No LightVertex is emitted; we write one VCMMisQuantities
//     per input BDPTVertex into outMis (parallel array).
//
// The output at outMis[i] is the state AFTER the geometric update
// at v[i] and BEFORE the BSDF-sampling update that prepares for
// v[i+1] — the same "at vertex i" state SmallVCM reads at
// connection / merge time.
//////////////////////////////////////////////////////////////////////
void VCMIntegrator::ConvertEyeSubpath(
	const std::vector<BDPTVertex>& verts,
	const VCMNormalization& norm,
	std::vector<VCMMisQuantities>& outMis
	)
{
	outMis.clear();
	const std::size_t n = verts.size();
	if( n == 0 ) {
		return;
	}
	outMis.resize( n );

	VCMMisQuantities mis;

	for( std::size_t i = 0; i < n; i++ )
	{
		const BDPTVertex& v = verts[i];

		//
		// Initialization at the camera endpoint.
		//
		if( i == 0 )
		{
			if( v.type != BDPTVertex::CAMERA ) {
				// Malformed subpath — abandon.
				outMis.clear();
				return;
			}
			mis = InitCamera( v.emissionPdfW, norm );
			outMis[0] = mis;
			continue;
		}

		//
		// Non-endpoint vertices: geometric update, then, if this
		// isn't the tail vertex, a BSDF-sampling update.
		//

		// Mirror of ConvertLightSubpath's skip logic.  MEDIUM
		// vertices propagate both the geometric update (using
		// sigma_t_scalar) and the phase-function sampling update.
		// BSSRDF re-entry is always skipped.
		//
		// IMPORTANT: we DO NOT skip on `pdfFwd <= 0`.  See the
		// matching comment in ConvertLightSubpath.
		const bool isMedium = ( v.type == BDPTVertex::MEDIUM );
		const bool skipRecurrence = ( v.type != BDPTVertex::SURFACE && !isMedium ) ||
		                            ( v.isBSSRDFEntry );

		const Scalar cosFix = isMedium ? v.sigma_t_scalar : v.cosAtGen;
		if( skipRecurrence || cosFix <= 0 ) {
			outMis[i] = mis;
			continue;
		}

		const BDPTVertex& prev = verts[i - 1];
		const Vector3 step = Vector3Ops::mkVector3( v.position, prev.position );
		const Scalar distSq = Vector3Ops::SquaredModulus( step );
		if( distSq <= 0 ) {
			outMis[i] = mis;
			continue;
		}

		// Eye-path geometric update always applies the
		// distance-squared term to dVCM (the camera is finite).
		mis = ApplyGeometricUpdate( mis, distSq, cosFix, /*applyDistSqToDVCM*/ true );

		outMis[i] = mis;

		// Medium vertices: geometric update applied above.
		// Apply phase-function sampling update (mirrors light side).
		if( isMedium ) {
			if( i + 1 < n )
			{
				const BDPTVertex& next = verts[i + 1];
				const Vector3 nextStep = Vector3Ops::mkVector3( next.position, v.position );
				const Scalar nextDistSq = Vector3Ops::SquaredModulus( nextStep );
				if( nextDistSq > 0 && next.pdfFwd > 0 )
				{
					const Scalar nextDist = std::sqrt( nextDistSq );
					const Vector3 wo = nextStep * ( Scalar( 1 ) / nextDist );
					const Scalar cosThetaOutMed = v.sigma_t_scalar;

					const Scalar nextFactor = AreaToSolidAngleFactor( next, -wo );
					const Scalar bsdfDirPdfW = ( nextFactor > 0 )
						? next.pdfFwd * nextDistSq / nextFactor : Scalar( 0 );

					const Scalar invDist = ( distSq > 0 ) ? ( Scalar( 1 ) / std::sqrt( distSq ) ) : Scalar( 0 );
					const Vector3 dirPrevToCur = step * invDist;
					const Scalar prevFactor = AreaToSolidAngleFactor( prev, dirPrevToCur );
					const Scalar bsdfRevPdfW = ( prev.pdfRev > 0 && prevFactor > 0 )
						? prev.pdfRev * distSq / prevFactor : Scalar( 0 );

					mis = ApplyBsdfSamplingUpdate(
						mis, cosThetaOutMed, bsdfDirPdfW, bsdfRevPdfW,
						false, norm );
				}
			}
			continue;
		}

		// BSDF-sampling update: prepare for v[i+1] if it exists.
		if( i + 1 < n )
		{
			const BDPTVertex& next = verts[i + 1];
			const Vector3 nextStep = Vector3Ops::mkVector3( next.position, v.position );
			const Scalar nextDistSq = Vector3Ops::SquaredModulus( nextStep );
			if( nextDistSq <= 0 ) {
				continue;
			}
			const Scalar nextDist = std::sqrt( nextDistSq );
			const Vector3 wo = nextStep * ( Scalar( 1 ) / nextDist );
			const Scalar cosThetaOut = fabs( Vector3Ops::Dot( v.normal, wo ) );

			if( v.isDelta ) {
				mis = ApplyBsdfSamplingUpdate(
					mis, cosThetaOut,
					Scalar( 0 ), Scalar( 0 ),
					true, norm );
				continue;
			}

			// Non-specular BSDF update.
			if( next.pdfFwd <= 0 ) {
				continue;
			}
			const Scalar nextFactor = AreaToSolidAngleFactor( next, -wo );
			if( nextFactor <= 0 ) {
				continue;
			}
			const Scalar bsdfDirPdfW_out = next.pdfFwd * nextDistSq / nextFactor;

			const Scalar invDist = ( distSq > 0 ) ? ( Scalar( 1 ) / std::sqrt( distSq ) ) : Scalar( 0 );
			const Vector3 dirPrevToCur = step * invDist;
			const Scalar prevFactor = AreaToSolidAngleFactor( prev, dirPrevToCur );

			Scalar bsdfRevPdfW_out = 0;
			if( prev.pdfRev > 0 && prevFactor > 0 ) {
				bsdfRevPdfW_out = prev.pdfRev * distSq / prevFactor;
			}

			mis = ApplyBsdfSamplingUpdate(
				mis, cosThetaOut,
				bsdfDirPdfW_out, bsdfRevPdfW_out,
				false, norm );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateMerges — strategy (vertex merging, density estimation)
//
// For each non-delta SURFACE eye vertex at index t >= 1, query the
// light vertex store within mMergeRadius.  For each candidate, evaluate
// the eye vertex's BSDF in the direction of the candidate's stored
// arrival direction (lv.wi) and accumulate the SmallVCM-weighted merge
// contribution.  After the inner loop, scale by mVmNormalization (the
// per-iteration density-estimation constant) and by the eye vertex's
// throughput.
//
// This strategy is a no-op when VM is disabled or when the merge radius
// is zero (both reduce mVmNormalization to zero).
//////////////////////////////////////////////////////////////////////
RISEPel VCMIntegrator::EvaluateMerges(
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const LightVertexStore& store,
	const VCMNormalization& norm
	) const
{
	RISEPel total( 0, 0, 0 );

	if( !norm.mEnableVM || norm.mMergeRadiusSq <= 0 || norm.mVmNormalization <= 0 ) {
		return total;
	}
	if( eyeVerts.size() != eyeMis.size() || eyeVerts.size() < 2 ) {
		return total;
	}
	if( store.Size() == 0 || !store.IsBuilt() ) {
		return total;
	}

	// Per-thread scratch — reused across pixels to eliminate per-sample
	// libmalloc arena contention in the hot merge-evaluation path.
	static thread_local std::vector<LightVertex> candidates;
	if( candidates.capacity() < 256 ) {
		candidates.reserve( 256 );
	}

	for( std::size_t i = 1; i < eyeVerts.size(); i++ )
	{
		const BDPTVertex& v = eyeVerts[i];
		if( v.type != BDPTVertex::SURFACE || !v.pMaterial ) {
			continue;
		}
		if( !v.isConnectible ) {
			continue;
		}

		const BDPTVertex& prev = eyeVerts[i - 1];
		Vector3 woAtEye = Vector3Ops::mkVector3( prev.position, v.position );
		const Scalar woDist = Vector3Ops::Magnitude( woAtEye );
		if( woDist < VCM_RAY_EPSILON ) {
			continue;
		}
		woAtEye = woAtEye * ( Scalar( 1 ) / woDist );

		candidates.clear();
		store.Query( v.position, norm.mMergeRadiusSq, candidates );
		if( candidates.empty() ) {
			continue;
		}

		RISEPel pixelMerge( 0, 0, 0 );

		for( std::size_t k = 0; k < candidates.size(); k++ )
		{
			const LightVertex& lv = candidates[k];

			// Gate on isConnectible only — a vertex that sampled
			// a delta lobe (kLVF_IsDelta) but has a non-delta BSDF
			// component (kLVF_IsConnectible) is a valid merge
			// target because the merge evaluates the full non-delta
			// BSDF, independent of the sampled continuation lobe.
			if( ( lv.flags & kLVF_IsConnectible ) == 0 ) {
				continue;
			}

			// lv.wi stores the direction FROM the previous light
			// vertex TO this one (arrival).  The eye BSDF expects
			// "away from surface toward source", so negate it.
			const Vector3 wiAtEye = -lv.wi;

			const RISEPel cameraBsdf =
				PathVertexEval::EvalBSDFAtVertex( v, wiAtEye, woAtEye );
			if( ColorMath::MaxValue( cameraBsdf ) <= 0 ) {
				continue;
			}
			const Scalar cameraBsdfDirPdfW =
				PathVertexEval::EvalPdfAtVertex( v, woAtEye, wiAtEye );
			const Scalar cameraBsdfRevPdfW =
				PathVertexEval::EvalPdfAtVertex( v, wiAtEye, woAtEye );

			const Scalar wLight =
				lv.mis.dVCM * norm.mMisVcWeightFactor
				+ lv.mis.dVM * cameraBsdfDirPdfW;
			const Scalar wCamera =
				eyeMis[i].dVCM * norm.mMisVcWeightFactor
				+ eyeMis[i].dVM * cameraBsdfRevPdfW;
			const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

			pixelMerge = pixelMerge + cameraBsdf * lv.throughput * weight;
		}

		total = total + v.throughput * pixelMerge * norm.mVmNormalization;
	}

	return total;
}

//////////////////////////////////////////////////////////////////////
// SPECTRAL (NM) VARIANTS
//
// These mirror the Pel evaluators but operate on a single wavelength.
// The recurrence (VCMMisQuantities, ConvertLightSubpath,
// ConvertEyeSubpath) is wavelength-independent and is reused from the
// Pel side — the NM variants read the SAME VCMMisQuantities arrays.
//
// Implementation note: the light sources (ILight) only have a Pel
// emittedRadiance API, so when a vertex has v.pLight we evaluate the
// Pel radiance and convert to XYZ.Y as the NM-channel proxy.  For
// mesh luminaries (v.pLuminary->GetMaterial()->GetEmitter()) we call
// IEmitter::emittedRadianceNM directly, which IS wavelength-aware.
//////////////////////////////////////////////////////////////////////

static inline Scalar RISEPelToNMProxy( const RISEPel& p )
{
	// Luminance approximation for light sources that only expose a
	// Pel API.  For wavelength-accurate rendering the scene should
	// use mesh luminaries with spectral emitters.
	return Scalar( 0.2126 ) * p.r + Scalar( 0.7152 ) * p.g + Scalar( 0.0722 ) * p.b;
}

//////////////////////////////////////////////////////////////////////
// EvaluateS0NM
//////////////////////////////////////////////////////////////////////
Scalar VCMIntegrator::EvaluateS0NM(
	const IScene& scene,
	const IRayCaster& caster,
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const VCMNormalization& /*norm*/,
	const Scalar nm
	) const
{
	Scalar total = 0;

	if( eyeVerts.size() != eyeMis.size() || eyeVerts.size() < 2 ) {
		return total;
	}

	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) {
		return total;
	}

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager::LuminariesList& luminaries = pLumManager
		? const_cast<LuminaryManager*>( pLumManager )->getLuminaries()
		: emptyList;

	for( std::size_t i = 1; i < eyeVerts.size(); i++ )
	{
		const BDPTVertex& v = eyeVerts[i];
		if( v.type != BDPTVertex::SURFACE || !v.pMaterial || !v.pObject ) {
			continue;
		}

		const IEmitter* pEmitter = v.pMaterial->GetEmitter();
		if( !pEmitter ) {
			continue;
		}

		const BDPTVertex& prev = eyeVerts[i - 1];
		Vector3 woFromEmitter = Vector3Ops::mkVector3( prev.position, v.position );
		const Scalar d = Vector3Ops::Magnitude( woFromEmitter );
		if( d <= 0 ) {
			continue;
		}
		woFromEmitter = woFromEmitter * ( Scalar( 1 ) / d );

		RayIntersectionGeometric rig( Ray( v.position, woFromEmitter ), nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = v.position;
		rig.vNormal = v.normal;
		rig.onb = v.onb;
		const Scalar Le = pEmitter->emittedRadianceNM( rig, woFromEmitter, v.normal, nm );
		if( Le <= 0 ) {
			continue;
		}

		const Scalar pdfSelect = pLS->PdfSelectLuminary(
			scene, luminaries, *v.pObject, prev.position, prev.normal );
		const Scalar area = v.pObject->GetArea();
		if( area <= 0 ) {
			continue;
		}
		const Scalar pdfPosition = Scalar( 1 ) / area;
		const Scalar directPdfA = pdfSelect * pdfPosition;

		const Scalar cosAtEmitter = Vector3Ops::Dot( v.normal, woFromEmitter );
		const Scalar emissionDirPdfSA = ( cosAtEmitter > 0 )
			? ( cosAtEmitter * INV_PI )
			: Scalar( 0 );

		const Scalar emissionPdfW = directPdfA * emissionDirPdfSA;

		const Scalar wCamera = directPdfA * eyeMis[i].dVCM + emissionPdfW * eyeMis[i].dVC;
		const Scalar weight = Scalar( 1 ) / ( VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

		total = total + ( v.throughputNM * Le * weight );
	}

	return total;
}

//////////////////////////////////////////////////////////////////////
// EvaluateNEENM
//////////////////////////////////////////////////////////////////////
Scalar VCMIntegrator::EvaluateNEENM(
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const VCMNormalization& norm,
	const Scalar nm
	) const
{
	Scalar total = 0;

	if( eyeVerts.size() != eyeMis.size() || eyeVerts.size() < 2 ) {
		return total;
	}

	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) {
		return total;
	}

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager::LuminariesList& luminaries = pLumManager
		? const_cast<LuminaryManager*>( pLumManager )->getLuminaries()
		: emptyList;

	for( std::size_t i = 1; i < eyeVerts.size(); i++ )
	{
		const BDPTVertex& v = eyeVerts[i];
		if( v.type != BDPTVertex::SURFACE || !v.pMaterial ) {
			continue;
		}
		if( !v.isConnectible ) {
			continue;
		}

		sampler.StartStream( 48 + static_cast<unsigned int>( i ) );

		LightSample ls;
		if( !pLS->SampleLight( scene, luminaries, sampler, ls ) ) {
			continue;
		}
		if( ls.pdfSelect <= 0 || ls.pdfPosition <= 0 ) {
			continue;
		}

		Vector3 dirToLight = Vector3Ops::mkVector3( ls.position, v.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToLight );
		if( dist < VCM_RAY_EPSILON ) {
			continue;
		}
		dirToLight = dirToLight * ( Scalar( 1 ) / dist );
		const Scalar distSq = dist * dist;

		if( !VCMIsVisible( caster, v.position, ls.position ) ) {
			continue;
		}

		const BDPTVertex& prev = eyeVerts[i - 1];
		Vector3 woAtEye = Vector3Ops::mkVector3( prev.position, v.position );
		const Scalar woDist = Vector3Ops::Magnitude( woAtEye );
		if( woDist < VCM_RAY_EPSILON ) {
			continue;
		}
		woAtEye = woAtEye * ( Scalar( 1 ) / woDist );

		Scalar Le = 0;
		if( ls.pLight ) {
			Le = RISEPelToNMProxy( ls.pLight->emittedRadiance( -dirToLight ) );
		} else if( ls.pLuminary && ls.pLuminary->GetMaterial() ) {
			const IEmitter* pEmitter = ls.pLuminary->GetMaterial()->GetEmitter();
			if( pEmitter ) {
				RayIntersectionGeometric rig( Ray( ls.position, -dirToLight ), nullRasterizerState );
				rig.bHit = true;
				rig.ptIntersection = ls.position;
				rig.vNormal = ls.normal;
				Le = pEmitter->emittedRadianceNM( rig, -dirToLight, ls.normal, nm );
			}
		}
		if( Le <= 0 ) {
			continue;
		}

		const Scalar fEye = PathVertexEval::EvalBSDFAtVertexNM( v, dirToLight, woAtEye, nm );
		if( fEye <= 0 ) {
			continue;
		}

		const Scalar bsdfDirPdfW = PathVertexEval::EvalPdfAtVertexNM( v, woAtEye, dirToLight, nm );
		const Scalar bsdfRevPdfW = PathVertexEval::EvalPdfAtVertexNM( v, dirToLight, woAtEye, nm );

		const Scalar cosAtEye = fabs( Vector3Ops::Dot( v.normal, dirToLight ) );
		Scalar cosAtLight = 0;
		Scalar G = 0;
		if( ls.isDelta ) {
			cosAtLight = 1;
			G = cosAtEye / distSq;
		} else {
			cosAtLight = fabs( Vector3Ops::Dot( ls.normal, -dirToLight ) );
			G = ( cosAtEye * cosAtLight ) / distSq;
		}
		if( cosAtEye <= 0 || G <= 0 ) {
			continue;
		}

		const Scalar directPdfW = ls.isDelta
			? Scalar( 1 )
			: ( ls.pdfPosition * distSq / cosAtLight );

		Scalar emissionDirPdfSA = 0;
		if( ls.pLuminary ) {
			const Scalar c = Vector3Ops::Dot( ls.normal, -dirToLight );
			emissionDirPdfSA = ( c > 0 ) ? ( c * INV_PI ) : Scalar( 0 );
		} else if( ls.pLight ) {
			emissionDirPdfSA = ls.pLight->pdfDirection( -dirToLight );
		}
		const Scalar emissionPdfW = ls.pdfSelect * ls.pdfPosition * emissionDirPdfSA;

		const Scalar lightPickProb = ls.pdfSelect;

		const Scalar wLight = bsdfDirPdfW / ( lightPickProb * directPdfW );
		Scalar wCamera = 0;
		if( directPdfW > 0 && cosAtLight > 0 ) {
			const Scalar camFactor =
				( emissionPdfW * cosAtEye ) / ( directPdfW * cosAtLight );
			wCamera = camFactor * (
				norm.mMisVmWeightFactor
				+ eyeMis[i].dVCM
				+ eyeMis[i].dVC * bsdfRevPdfW );
		}
		const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

		const Scalar invLightPdfArea = ls.pdfSelect * ls.pdfPosition;
		if( invLightPdfArea <= 0 ) {
			continue;
		}

		const Scalar contribution = v.throughputNM * fEye * Le * ( G / invLightPdfArea ) * weight;
		total = total + contribution;
	}

	return total;
}

//////////////////////////////////////////////////////////////////////
// SplatLightSubpathToCameraNM — mirrors Pel version, writes XYZ
// splats derived from the single-wavelength contribution via
// ColorUtils::XYZFromNM.
//////////////////////////////////////////////////////////////////////
void VCMIntegrator::SplatLightSubpathToCameraNM(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<VCMMisQuantities>& lightMis,
	const IScene& /*scene*/,
	const IRayCaster& caster,
	const ICamera& camera,
	SplatFilm& splatFilm,
	const VCMNormalization& norm,
	const Scalar nm
	) const
{
	if( lightVerts.size() != lightMis.size() || lightVerts.empty() ) {
		return;
	}
	if( norm.mLightSubPathCount <= 0 ) {
		return;
	}

	const Point3 camPos = camera.GetLocation();

	for( std::size_t i = 0; i < lightVerts.size(); i++ )
	{
		const BDPTVertex& v = lightVerts[i];

		if( !v.isConnectible ) {
			continue;
		}
		if( v.type != BDPTVertex::LIGHT && v.type != BDPTVertex::SURFACE ) {
			continue;
		}

		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, v.position, rasterPos ) ) {
			continue;
		}

		if( !VCMIsVisible( caster, v.position, camPos ) ) {
			continue;
		}

		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, v.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < VCM_RAY_EPSILON ) {
			continue;
		}
		dirToCam = dirToCam * ( Scalar( 1 ) / dist );
		const Scalar distSq = dist * dist;

		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );
		if( We <= 0 ) {
			continue;
		}

		const Scalar cosAtLight = fabs( Vector3Ops::Dot( v.normal, dirToCam ) );
		if( cosAtLight <= 0 ) {
			continue;
		}
		const Scalar G = cosAtLight / distSq;

		const Scalar camPdfDirSA = BDPTCameraUtilities::PdfDirection( camera, camRay );
		if( camPdfDirSA <= 0 ) {
			continue;
		}
		const Scalar cameraPdfA = camPdfDirSA * cosAtLight / distSq;

		Scalar contribution = 0;
		Scalar bsdfRevPdfW = 0;

		if( v.type == BDPTVertex::LIGHT )
		{
			Scalar Le = 0;
			if( v.pLight ) {
				Le = RISEPelToNMProxy( v.pLight->emittedRadiance( dirToCam ) );
			} else if( v.pLuminary && v.pLuminary->GetMaterial() ) {
				const IEmitter* pEmitter = v.pLuminary->GetMaterial()->GetEmitter();
				if( pEmitter ) {
					RayIntersectionGeometric rig( Ray( v.position, dirToCam ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = v.position;
					rig.vNormal = v.normal;
					rig.onb = v.onb;
					Le = pEmitter->emittedRadianceNM( rig, dirToCam, v.normal, nm );
				}
			}
			if( Le <= 0 ) {
				continue;
			}

			const Scalar pdfLightArea = v.pdfFwd;
			if( pdfLightArea <= 0 ) {
				continue;
			}
			contribution = Le * ( G * We / pdfLightArea );
			bsdfRevPdfW = 0;
		}
		else
		{
			if( !v.pMaterial ) {
				continue;
			}
			if( i < 1 ) {
				continue;
			}
			const BDPTVertex& prev = lightVerts[i - 1];
			Vector3 wiAtLight = Vector3Ops::mkVector3( prev.position, v.position );
			const Scalar wiDist = Vector3Ops::Magnitude( wiAtLight );
			if( wiDist < VCM_RAY_EPSILON ) {
				continue;
			}
			wiAtLight = wiAtLight * ( Scalar( 1 ) / wiDist );

			const Scalar fLight = PathVertexEval::EvalBSDFAtVertexNM( v, wiAtLight, dirToCam, nm );
			if( fLight <= 0 ) {
				continue;
			}

			bsdfRevPdfW = PathVertexEval::EvalPdfAtVertexNM( v, dirToCam, wiAtLight, nm );

			contribution = v.throughputNM * fLight * ( G * We );
		}

		const Scalar wLight =
			( cameraPdfA / norm.mLightSubPathCount ) *
			( norm.mMisVmWeightFactor + lightMis[i].dVCM + lightMis[i].dVC * bsdfRevPdfW );
		const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) );

		const Scalar weighted = contribution * weight;
		if( weighted <= 0 ) {
			continue;
		}

		// Convert scalar NM contribution to XYZ for the splat film.
		XYZPel xyz( 0, 0, 0 );
		if( !ColorUtils::XYZFromNM( xyz, nm ) ) {
			continue;
		}
		xyz = xyz * weighted;

		const int sx = static_cast<int>( rasterPos.x );
		const int sy = static_cast<int>( camera.GetHeight() - rasterPos.y );
		if( sx < 0 || sy < 0 ||
			static_cast<unsigned int>( sx ) >= camera.GetWidth() ||
			static_cast<unsigned int>( sy ) >= camera.GetHeight() ) {
			continue;
		}

		splatFilm.Splat( sx, sy, RISEPel( xyz.X, xyz.Y, xyz.Z ) );
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateInteriorConnectionsNM
//////////////////////////////////////////////////////////////////////
Scalar VCMIntegrator::EvaluateInteriorConnectionsNM(
	const IScene& /*scene*/,
	const IRayCaster& caster,
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<VCMMisQuantities>& lightMis,
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const VCMNormalization& norm,
	const Scalar nm
	) const
{
	Scalar total = 0;

	if( lightVerts.size() != lightMis.size() || eyeVerts.size() != eyeMis.size() ) {
		return total;
	}

	for( std::size_t i = 1; i < lightVerts.size(); i++ )
	{
		const BDPTVertex& lv = lightVerts[i];
		if( lv.type != BDPTVertex::SURFACE || !lv.pMaterial ) {
			continue;
		}
		if( !lv.isConnectible ) {
			continue;
		}

		const BDPTVertex& lvPrev = lightVerts[i - 1];
		Vector3 wiAtLight = Vector3Ops::mkVector3( lvPrev.position, lv.position );
		const Scalar wiLightDist = Vector3Ops::Magnitude( wiAtLight );
		if( wiLightDist < VCM_RAY_EPSILON ) {
			continue;
		}
		wiAtLight = wiAtLight * ( Scalar( 1 ) / wiLightDist );

		for( std::size_t j = 1; j < eyeVerts.size(); j++ )
		{
			const BDPTVertex& ev = eyeVerts[j];
			if( ev.type != BDPTVertex::SURFACE || !ev.pMaterial ) {
				continue;
			}
			if( !ev.isConnectible ) {
				continue;
			}

			const BDPTVertex& evPrev = eyeVerts[j - 1];
			Vector3 woAtEye = Vector3Ops::mkVector3( evPrev.position, ev.position );
			const Scalar woEyeDist = Vector3Ops::Magnitude( woAtEye );
			if( woEyeDist < VCM_RAY_EPSILON ) {
				continue;
			}
			woAtEye = woAtEye * ( Scalar( 1 ) / woEyeDist );

			Vector3 lightToEye = Vector3Ops::mkVector3( ev.position, lv.position );
			const Scalar dist = Vector3Ops::Magnitude( lightToEye );
			if( dist < VCM_RAY_EPSILON ) {
				continue;
			}
			lightToEye = lightToEye * ( Scalar( 1 ) / dist );
			const Scalar distSq = dist * dist;

			if( !VCMIsVisible( caster, lv.position, ev.position ) ) {
				continue;
			}

			const Scalar fLight = PathVertexEval::EvalBSDFAtVertexNM( lv, wiAtLight, lightToEye, nm );
			if( fLight <= 0 ) {
				continue;
			}
			const Scalar fEye = PathVertexEval::EvalBSDFAtVertexNM( ev, -lightToEye, woAtEye, nm );
			if( fEye <= 0 ) {
				continue;
			}

			const Scalar cosAtLight = fabs( Vector3Ops::Dot( lv.normal, lightToEye ) );
			const Scalar cosAtEye   = fabs( Vector3Ops::Dot( ev.normal, -lightToEye ) );
			if( cosAtLight <= 0 || cosAtEye <= 0 ) {
				continue;
			}
			const Scalar G = ( cosAtLight * cosAtEye ) / distSq;

			const Scalar lightBsdfDirPdfW =
				PathVertexEval::EvalPdfAtVertexNM( lv, wiAtLight, lightToEye, nm );
			const Scalar cameraBsdfDirPdfW =
				PathVertexEval::EvalPdfAtVertexNM( ev, woAtEye, -lightToEye, nm );

			const Scalar lightBsdfRevPdfW =
				PathVertexEval::EvalPdfAtVertexNM( lv, lightToEye, wiAtLight, nm );
			const Scalar cameraBsdfRevPdfW =
				PathVertexEval::EvalPdfAtVertexNM( ev, -lightToEye, woAtEye, nm );

			const Scalar cameraBsdfDirPdfA =
				BDPTUtilities::SolidAngleToArea( cameraBsdfDirPdfW, cosAtLight, distSq );
			const Scalar lightBsdfDirPdfA =
				BDPTUtilities::SolidAngleToArea( lightBsdfDirPdfW, cosAtEye, distSq );

			const Scalar wLight = cameraBsdfDirPdfA
				* ( norm.mMisVmWeightFactor
				    + lightMis[i].dVCM
				    + lightMis[i].dVC * lightBsdfRevPdfW );
			const Scalar wCamera = lightBsdfDirPdfA
				* ( norm.mMisVmWeightFactor
				    + eyeMis[j].dVCM
				    + eyeMis[j].dVC * cameraBsdfRevPdfW );
			const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

			const Scalar contrib = lv.throughputNM * fLight * ( G * weight ) * fEye * ev.throughputNM;
			total = total + contrib;
		}
	}

	return total;
}

//////////////////////////////////////////////////////////////////////
// EvaluateMergesNM
//
// Note: the LightVertexStore stores Pel throughputs (from the hero
// subpath the store was populated with).  For HWSS to work correctly,
// callers SHOULD repopulate the store at each wavelength — but for
// v1 we take the pragmatic shortcut of using the stored Pel
// throughput's luminance as the companion-wavelength proxy.  Mesh
// luminaries with strong spectral emission will still produce
// reasonable caustic merging from the hero pass; scenes that
// critically depend on wavelength-accurate merging should use a
// custom workflow or extend the store to hold per-wavelength
// throughputs.
//////////////////////////////////////////////////////////////////////
Scalar VCMIntegrator::EvaluateMergesNM(
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const LightVertexStore& store,
	const VCMNormalization& norm,
	const Scalar nm
	) const
{
	Scalar total = 0;

	if( !norm.mEnableVM || norm.mMergeRadiusSq <= 0 || norm.mVmNormalization <= 0 ) {
		return total;
	}
	if( eyeVerts.size() != eyeMis.size() || eyeVerts.size() < 2 ) {
		return total;
	}
	if( store.Size() == 0 || !store.IsBuilt() ) {
		return total;
	}

	// Per-thread scratch — reused across pixels to eliminate per-sample
	// libmalloc arena contention in the hot merge-evaluation path.
	static thread_local std::vector<LightVertex> candidates;
	if( candidates.capacity() < 256 ) {
		candidates.reserve( 256 );
	}

	for( std::size_t i = 1; i < eyeVerts.size(); i++ )
	{
		const BDPTVertex& v = eyeVerts[i];
		if( v.type != BDPTVertex::SURFACE || !v.pMaterial ) {
			continue;
		}
		if( !v.isConnectible ) {
			continue;
		}

		const BDPTVertex& prev = eyeVerts[i - 1];
		Vector3 woAtEye = Vector3Ops::mkVector3( prev.position, v.position );
		const Scalar woDist = Vector3Ops::Magnitude( woAtEye );
		if( woDist < VCM_RAY_EPSILON ) {
			continue;
		}
		woAtEye = woAtEye * ( Scalar( 1 ) / woDist );

		candidates.clear();
		store.Query( v.position, norm.mMergeRadiusSq, candidates );
		if( candidates.empty() ) {
			continue;
		}

		Scalar pixelMerge = 0;

		for( std::size_t k = 0; k < candidates.size(); k++ )
		{
			const LightVertex& lv = candidates[k];

			if( ( lv.flags & kLVF_IsConnectible ) == 0 ) {
				continue;
			}

			const Vector3 wiAtEye = -lv.wi;

			const Scalar cameraBsdf =
				PathVertexEval::EvalBSDFAtVertexNM( v, wiAtEye, woAtEye, nm );
			if( cameraBsdf <= 0 ) {
				continue;
			}
			const Scalar cameraBsdfDirPdfW =
				PathVertexEval::EvalPdfAtVertexNM( v, woAtEye, wiAtEye, nm );
			const Scalar cameraBsdfRevPdfW =
				PathVertexEval::EvalPdfAtVertexNM( v, wiAtEye, woAtEye, nm );

			const Scalar wLight =
				lv.mis.dVCM * norm.mMisVcWeightFactor
				+ lv.mis.dVM * cameraBsdfDirPdfW;
			const Scalar wCamera =
				eyeMis[i].dVCM * norm.mMisVcWeightFactor
				+ eyeMis[i].dVM * cameraBsdfRevPdfW;
			const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

			// Use the stored Pel throughput's luminance as the
			// companion-wavelength proxy.  See comment above.
			const Scalar lvThroughputNM = RISEPelToNMProxy( lv.throughput );
			pixelMerge = pixelMerge + cameraBsdf * lvThroughputNM * weight;
		}

		total = total + v.throughputNM * pixelMerge * norm.mVmNormalization;
	}

	return total;
}
