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
#include "../Interfaces/IPixelFilter.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IEmitter.h"
#include "../Interfaces/ILight.h"
#include "../Interfaces/IObject.h"
#include "../Lights/LightSampler.h"
#include "../Rendering/LuminaryManager.h"
#include "../Utilities/PathVertexEval.h"
#include "../Utilities/PathValueOps.h"
#include "../Utilities/Color/SpectralValueTraits.h"
#include "../Utilities/BDPTUtilities.h"
#include "../Interfaces/ICamera.h"
#include "../Cameras/CameraUtilities.h"
#include "../Rendering/SplatFilm.h"

using namespace RISE;
using namespace RISE::Implementation;
using RISE::SpectralDispatch::PelTag;
using RISE::SpectralDispatch::NMTag;
using RISE::SpectralDispatch::SpectralValueTraits;

static inline Scalar AreaToSolidAngleFactor(
	const BDPTVertex& v,
	const Vector3& dirFromAdjacent
	);

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

	inline VCMMisQuantities ApplyBSSRDFEntryAreaUpdate(
		const BDPTVertex& v
		)
	{
		VCMMisQuantities r;
		if( v.pdfFwd > NEARZERO ) {
			// BSSRDF entry sampling gives an area-density directly; there is
			// no edge Jacobian to invert.  The VC MIS state at the entry
			// therefore carries the reciprocal area PDF, so NEE/interior
			// connections compete with the sampled BSSRDF transport.
			r.dVCM = Scalar( 1 ) / v.pdfFwd;
		}
		return r;
	}

	inline VCMMisQuantities ApplyBSSRDFEntryOnwardUpdate(
		const VCMMisQuantities& mis,
		const std::vector<BDPTVertex>& verts,
		const std::size_t i,
		const VCMNormalization& norm
		)
	{
		if( i + 1 >= verts.size() ) {
			return mis;
		}

		const BDPTVertex& v = verts[i];
		const BDPTVertex& next = verts[i + 1];
		if( next.pdfFwd <= 0 ) {
			return mis;
		}

		const Vector3 nextStep = Vector3Ops::mkVector3( next.position, v.position );
		const Scalar nextDistSq = Vector3Ops::SquaredModulus( nextStep );
		if( nextDistSq <= 0 ) {
			return mis;
		}
		const Scalar nextDist = std::sqrt( nextDistSq );
		const Vector3 wo = nextStep * ( Scalar( 1 ) / nextDist );
		const Scalar nextFactor = AreaToSolidAngleFactor( next, -wo );
		if( nextFactor <= 0 ) {
			return mis;
		}

		const Scalar cosThetaOut = fabs( Vector3Ops::Dot( v.normal, wo ) );
		const Scalar bsdfDirPdfW = next.pdfFwd * nextDistSq / nextFactor;
		return ApplyBsdfSamplingUpdate(
			mis, cosThetaOut, bsdfDirPdfW, Scalar( 0 ), false, norm );
	}

	// Luminance approximation for ILight sources that only expose a
	// Pel API.  For wavelength-accurate rendering the scene should
	// use mesh luminaries with spectral emitters.  This proxy is
	// applied in the NM path whenever we need a scalar radiance
	// from an ILight (point / spot / directional).
	inline Scalar RISEPelToNMProxy( const RISEPel& p )
	{
		return Scalar( 0.2126 ) * p.r + Scalar( 0.7152 ) * p.g + Scalar( 0.0722 ) * p.b;
	}

	//////////////////////////////////////////////////////////////////
	// Tag-dispatched helpers used by the templated VCM evaluators.
	// Each pair forwards to the existing Pel or NM implementation
	// based on the compile-time tag, producing zero-overhead dispatch
	// that lets the 5 evaluator methods share a single template body.
	//////////////////////////////////////////////////////////////////

	/// BDPTVertex throughput field picker.  Pel reads v.throughput,
	/// NM reads v.throughputNM.  Both fields coexist on every vertex.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type
	VertexThroughput( const BDPTVertex& v, const Tag& tag );

	template<>
	inline RISEPel VertexThroughput<PelTag>( const BDPTVertex& v, const PelTag& )
	{
		return v.throughput;
	}

	template<>
	inline Scalar VertexThroughput<NMTag>( const BDPTVertex& v, const NMTag& )
	{
		return v.throughputNM;
	}

	/// IEmitter::emittedRadiance dispatcher.  IEmitter provides both
	/// Pel and NM virtuals, so the dispatch is direct.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type
	EvalEmitterRadiance(
		const IEmitter& emitter,
		const RayIntersectionGeometric& ri,
		const Vector3& outDir,
		const Vector3& normal,
		const Tag& tag );

	template<>
	inline RISEPel EvalEmitterRadiance<PelTag>(
		const IEmitter& emitter,
		const RayIntersectionGeometric& ri,
		const Vector3& outDir,
		const Vector3& normal,
		const PelTag& )
	{
		return emitter.emittedRadiance( ri, outDir, normal );
	}

	template<>
	inline Scalar EvalEmitterRadiance<NMTag>(
		const IEmitter& emitter,
		const RayIntersectionGeometric& ri,
		const Vector3& outDir,
		const Vector3& normal,
		const NMTag& tag )
	{
		return emitter.emittedRadianceNM( ri, outDir, normal, tag.nm );
	}

	/// ILight::emittedRadiance dispatcher.  ILight has ONLY a Pel
	/// API; the NM path applies the RISEPelToNMProxy luminance
	/// projection to preserve v1 behavior.  See comment at the
	/// "SPECTRAL (NM) VARIANTS" block below for rationale.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type
	EvalLightRadiance( const ILight& light, const Vector3& dir, const Tag& tag );

	template<>
	inline RISEPel EvalLightRadiance<PelTag>(
		const ILight& light, const Vector3& dir, const PelTag& )
	{
		return light.emittedRadiance( dir );
	}

	template<>
	inline Scalar EvalLightRadiance<NMTag>(
		const ILight& light, const Vector3& dir, const NMTag& )
	{
		return RISEPelToNMProxy( light.emittedRadiance( dir ) );
	}

	/// Signed magnitude summary used by the "skip if contribution is
	/// non-positive" gates inside the templated evaluator bodies.
	/// Matches the pre-refactor gate semantics exactly:
	///   Pel: `ColorMath::MaxValue(v) <= 0` skips when the largest
	///        channel is <= 0 (RGB is treated as a signed triple).
	///   NM:  `v <= 0` skips negative or zero scalar contributions.
	///
	/// DO NOT change the NM specialization to fabs — negative
	/// spectral values are meaningful as "drop this sample" signals
	/// (a bug elsewhere would otherwise accumulate into the film);
	/// the pre-refactor code relied on the signed comparison as an
	/// implicit assertion.
	///
	/// For absolute-magnitude needs (Russian roulette, clamping) use
	/// a different helper — the two concerns must stay separate.
	inline Scalar PositiveMagnitude( const RISEPel& v ) { return ColorMath::MaxValue( v ); }
	inline Scalar PositiveMagnitude( const Scalar  v ) { return v; }

	/// LightVertex throughput field picker with v1 NM proxy.  The
	/// LightVertexStore holds Pel throughputs only (populated from
	/// the hero pass); the NM merge path uses RISEPelToNMProxy
	/// to project to scalar.  This preserves the v1 behavior
	/// documented at the EvaluateMergesNM comment block.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type
	LightVertexThroughput( const LightVertex& lv, const Tag& tag );

	template<>
	inline RISEPel LightVertexThroughput<PelTag>( const LightVertex& lv, const PelTag& )
	{
		return lv.throughput;
	}

	template<>
	inline Scalar LightVertexThroughput<NMTag>( const LightVertex& lv, const NMTag& )
	{
		return RISEPelToNMProxy( lv.throughput );
	}

	/// Convert a contribution to an RGB splat value for writing to
	/// SplatFilm.  Pel passes through; NM applies the CIE XYZ
	/// wavelength → tri-stimulus conversion.  Returns (valid, RISEPel).
	/// When conversion fails (e.g. NM is out of the visible range),
	/// returns (false, zero).
	template<class Tag>
	inline std::pair<bool, RISEPel> ToSplatRGB(
		const typename SpectralValueTraits<Tag>::value_type& v, const Tag& tag );

	template<>
	inline std::pair<bool, RISEPel> ToSplatRGB<PelTag>(
		const RISEPel& v, const PelTag& )
	{
		return std::make_pair( true, v );
	}

	template<>
	inline std::pair<bool, RISEPel> ToSplatRGB<NMTag>(
		const Scalar& v, const NMTag& tag )
	{
		XYZPel xyz( 0, 0, 0 );
		if( !ColorUtils::XYZFromNM( xyz, tag.nm ) ) {
			return std::make_pair( false, RISEPel( 0, 0, 0 ) );
		}
		xyz = xyz * v;
		return std::make_pair( true, RISEPel( xyz.X, xyz.Y, xyz.Z ) );
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
// BSSRDF re-entry vertices carry their spatial sampling density in
// area measure directly.  They skip the ordinary edge-Jacobian
// inversion, are excluded from the merge store, and still populate the
// parallel MIS array with dVCM = 1/pdfFwd so VC connections through SSS
// compete with the sampled BSSRDF transport.
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

		// BSSRDF re-entry vertices carry their spatial sampling PDF
		// directly in area measure.  They do not go through the ordinary
		// edge Jacobian path below, but VC MIS still needs the reciprocal
		// area PDF at the entry so connections through SSS compete with
		// the sampled BSSRDF transport instead of receiving weight ~1.
		if( v.isBSSRDFEntry ) {
			mis = ApplyBSSRDFEntryAreaUpdate( v );
			if( outMis ) (*outMis)[i] = mis;
			mis = ApplyBSSRDFEntryOnwardUpdate(
				mis, verts, i, norm );
			continue;
		}

		// Skip vertices that lack a valid area-measure conversion
		// factor.
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
		const bool skipRecurrence = ( v.type != BDPTVertex::SURFACE && !isMedium );

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
		// Standard SmallVCM stores at every connectible (non-delta)
		// surface vertex.  Filtering the store to post-specular
		// only (caustic-only PM) was tried and produced ~50% too-
		// dim indirect / caustic regions in glass-heavy scenes —
		// because the SmallVCM MIS formula assumes merge is a
		// valid strategy at every non-delta surface, and removing
		// deposits without a matching MIS correction leaves the
		// merge weight < 1 on SDS paths that only VM can sample.
		// Keep unfiltered deposits; if splotches become a problem,
		// tune merge radius or light-subpath count instead.
		if( v.isConnectible )
		{
			LightVertex lv;
			lv.ptPosition = v.position;
			lv.plane      = 0;
			lv.flags      = 0;
			if( v.isDelta          ) lv.flags |= kLVF_IsDelta;
			if( v.isConnectible    ) lv.flags |= kLVF_IsConnectible;
			if( v.isBSSRDFEntry    ) lv.flags |= kLVF_IsBSSRDFEntry;
			if( v.bHasVertexColor  ) lv.flags |= kLVF_HasVertexColor;
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
			lv.vColor     = v.vColor;
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
//////////////////////////////////////////////////////////////////////
// EvaluateS0Impl — templated body shared by EvaluateS0 and
// EvaluateS0NM.  Tag is PelTag for RGB, NMTag(nm) for spectral.
// See EvaluateS0 docstring above for the algorithm.
//////////////////////////////////////////////////////////////////////
namespace
{
	template<class Tag>
	typename SpectralValueTraits<Tag>::value_type EvaluateS0Impl(
		const IScene& scene,
		const IRayCaster& caster,
		const std::vector<BDPTVertex>& eyeVerts,
		const std::vector<VCMMisQuantities>& eyeMis,
		const Tag& tag
		)
	{
		using Traits = SpectralValueTraits<Tag>;
		typename Traits::value_type total = Traits::zero();

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
			const typename Traits::value_type Le =
				EvalEmitterRadiance<Tag>( *pEmitter, rig, woFromEmitter, v.normal, tag );
			if( PositiveMagnitude( Le ) <= 0 ) {
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

			// SmallVCM: path length 1 (eye ray directly hits emitter) has
			// no competing strategies.  NEE (s=1, t=1) cannot generate it
			// because there is no intermediate eye vertex to sample from;
			// the LIGHT-endpoint-to-camera splat (s=1, t=1) is skipped in
			// SplatLightSubpathToCamera for the same reason; and the
			// merging strategy has zero probability since photons emitted
			// from a light never land on the emitter surface.  Returning
			// full radiance with weight=1 is the unique unbiased estimator.
			//
			// Without this gate, naive balance-heuristic MIS downweights
			// s=0 by wCamera = directPdfA * dVCM > 0 even though no other
			// strategy exists, producing a systematically dim emitter.
			// Matches SmallVCM GetLightRadiance: `if (mPathLength == 1)
			// return radiance;` — see vertexcm.hxx.
			Scalar weight;
			if( i == 1 ) {
				weight = Scalar( 1 );
			} else {
				const Scalar wCamera = directPdfA * eyeMis[i].dVCM + emissionPdfW * eyeMis[i].dVC;
				weight = Scalar( 1 ) / ( VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );
			}

			total = total + ( VertexThroughput<Tag>( v, tag ) * Le * weight );
		}

		return total;
	}
}

RISEPel VCMIntegrator::EvaluateS0(
	const IScene& scene,
	const IRayCaster& caster,
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const VCMNormalization& /*norm*/
	) const
{
	return EvaluateS0Impl<PelTag>( scene, caster, eyeVerts, eyeMis, PelTag{} );
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
//////////////////////////////////////////////////////////////////////
// EvaluateNEEImpl — templated body shared by EvaluateNEE and
// EvaluateNEENM.  See EvaluateNEE documentation above for the
// algorithm.  The only Pel/NM differences resolved by Tag dispatch
// are (a) Le from ILight (Pel direct, NM via RISEPelToNMProxy),
// (b) Le from IEmitter (Pel/NM via virtuals), (c) BSDF evaluation at
// the eye vertex, and (d) throughput field selection.
//////////////////////////////////////////////////////////////////////
namespace
{
	template<class Tag>
	typename SpectralValueTraits<Tag>::value_type EvaluateNEEImpl(
		const IScene& scene,
		const IRayCaster& caster,
		ISampler& sampler,
		const std::vector<BDPTVertex>& eyeVerts,
		const std::vector<VCMMisQuantities>& eyeMis,
		const VCMNormalization& norm,
		const Tag& tag
		)
	{
		using Traits = SpectralValueTraits<Tag>;
		typename Traits::value_type total = Traits::zero();

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

			// Evaluate Le at the light toward the eye vertex.  ILight
			// (delta lights) use the Pel-only API with luminance
			// projection for the NM path; mesh luminaries route
			// through IEmitter which has a proper NM virtual.
			typename Traits::value_type Le = Traits::zero();
			if( ls.pLight ) {
				Le = EvalLightRadiance<Tag>( *ls.pLight, -dirToLight, tag );
			} else if( ls.pLuminary && ls.pLuminary->GetMaterial() ) {
				const IEmitter* pEmitter = ls.pLuminary->GetMaterial()->GetEmitter();
				if( pEmitter ) {
					RayIntersectionGeometric rig( Ray( ls.position, -dirToLight ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = ls.position;
					rig.vNormal = ls.normal;
					Le = EvalEmitterRadiance<Tag>( *pEmitter, rig, -dirToLight, ls.normal, tag );
				}
			}
			if( PositiveMagnitude( Le ) <= 0 ) {
				continue;
			}

			const typename Traits::value_type fEye =
				RISE::PathValueOps::EvalBSDFAtVertex<Tag>( v, dirToLight, woAtEye, tag );
			if( PositiveMagnitude( fEye ) <= 0 ) {
				continue;
			}

			const Scalar bsdfDirPdfW =
				RISE::PathValueOps::EvalPdfAtVertex<Tag>( v, woAtEye, dirToLight, tag );
			const Scalar bsdfRevPdfW =
				RISE::PathValueOps::EvalPdfAtVertex<Tag>( v, dirToLight, woAtEye, tag );

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

			const typename Traits::value_type contribution =
				VertexThroughput<Tag>( v, tag ) * fEye * Le * ( G / invLightPdfArea ) * weight;
			total = total + contribution;
		}

		return total;
	}
}

RISEPel VCMIntegrator::EvaluateNEE(
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const VCMNormalization& norm
	) const
{
	return EvaluateNEEImpl<PelTag>(
		scene, caster, sampler, eyeVerts, eyeMis, norm, PelTag{} );
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
//////////////////////////////////////////////////////////////////////
// SplatLightSubpathToCameraImpl — templated body shared by Pel and NM
// variants.  ToSplatRGB<Tag> converts the per-wavelength scalar
// contribution to XYZ tri-stimulus in the NM path; Pel is a no-op.
//////////////////////////////////////////////////////////////////////
namespace
{
	template<class Tag>
	void SplatLightSubpathToCameraImpl(
		const std::vector<BDPTVertex>& lightVerts,
		const std::vector<VCMMisQuantities>& lightMis,
		const IRayCaster& caster,
		const ICamera& camera,
		SplatFilm& splatFilm,
		const VCMNormalization& norm,
		const IPixelFilter* pixelFilter,
		const Tag& tag
		)
	{
		using Traits = SpectralValueTraits<Tag>;

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
			// SmallVCM: the LIGHT endpoint (i=0) is not splatted to the
			// camera.  Its sole possible contribution — "primary ray hits
			// emitter" — is already accounted for by EvaluateS0 with
			// full weight (see the i==1 gate in EvaluateS0Impl).
			// Splatting it here too would be pure double counting, and
			// MIS can't rescue that because with VM enabled the wLight
			// formula includes mMisVmWeightFactor which has no mirror
			// term in wCamera at eyeMis[1] (dVC=0 there), producing an
			// asymmetric weight split that dims the emitter.
			if( i == 0 || v.type == BDPTVertex::LIGHT ) {
				continue;
			}
			if( v.type != BDPTVertex::SURFACE ) {
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

			typename Traits::value_type contribution = Traits::zero();
			Scalar bsdfRevPdfW = 0;

			if( v.type == BDPTVertex::LIGHT )
			{
				typename Traits::value_type Le = Traits::zero();
				if( v.pLight ) {
					Le = EvalLightRadiance<Tag>( *v.pLight, dirToCam, tag );
				} else if( v.pLuminary && v.pLuminary->GetMaterial() ) {
					const IEmitter* pEmitter = v.pLuminary->GetMaterial()->GetEmitter();
					if( pEmitter ) {
						RayIntersectionGeometric rig(
							Ray( v.position, dirToCam ), nullRasterizerState );
						rig.bHit = true;
						rig.ptIntersection = v.position;
						rig.vNormal = v.normal;
						rig.onb = v.onb;
						Le = EvalEmitterRadiance<Tag>( *pEmitter, rig, dirToCam, v.normal, tag );
					}
				}
				if( PositiveMagnitude( Le ) <= 0 ) {
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

				const typename Traits::value_type fLight =
					RISE::PathValueOps::EvalBSDFAtVertex<Tag>( v, wiAtLight, dirToCam, tag );
				if( PositiveMagnitude( fLight ) <= 0 ) {
					continue;
				}

				bsdfRevPdfW =
					RISE::PathValueOps::EvalPdfAtVertex<Tag>( v, dirToCam, wiAtLight, tag );

				contribution = VertexThroughput<Tag>( v, tag ) * fLight * ( G * We );
			}

			const Scalar wLight =
				( cameraPdfA / norm.mLightSubPathCount ) *
				( norm.mMisVmWeightFactor + lightMis[i].dVCM + lightMis[i].dVC * bsdfRevPdfW );
			const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) );

			const typename Traits::value_type weighted = contribution * weight;

			// Defensive zero-skip: pre-refactor NM path dropped
			// non-positive contributions before the XYZ conversion.
			// Contributions are non-negative in practice (product of
			// throughput, BSDF, G, We, all >= 0), but preserving the
			// skip avoids splatting zeros to the film.
			if( PositiveMagnitude( weighted ) <= 0 ) {
				continue;
			}

			// Convert weighted contribution to RGB for splat write.
			// Pel passes through; NM applies ColorUtils::XYZFromNM.
			const std::pair<bool, RISEPel> rgb = ToSplatRGB<Tag>( weighted, tag );
			if( !rgb.first ) {
				continue;
			}

			const Scalar fx = rasterPos.x;
			const Scalar fy = static_cast<Scalar>( camera.GetHeight() ) - rasterPos.y;

			if( pixelFilter ) {
				splatFilm.SplatFiltered( fx, fy, rgb.second, *pixelFilter );
			} else {
				const Scalar rx = fx + Scalar( 0.5 );
				const Scalar ry = fy + Scalar( 0.5 );
				if( rx < 0 || ry < 0 ) continue;
				const int sx = static_cast<int>( rx );
				const int sy = static_cast<int>( ry );
				if( sx < 0 || sy < 0 ||
				    static_cast<unsigned int>( sx ) >= camera.GetWidth() ||
				    static_cast<unsigned int>( sy ) >= camera.GetHeight() ) continue;
				splatFilm.Splat( sx, sy, rgb.second );
			}
		}
	}
}

void VCMIntegrator::SplatLightSubpathToCamera(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<VCMMisQuantities>& lightMis,
	const IScene& /*scene*/,
	const IRayCaster& caster,
	const ICamera& camera,
	SplatFilm& splatFilm,
	const VCMNormalization& norm,
	const IPixelFilter* pixelFilter
	) const
{
	SplatLightSubpathToCameraImpl<PelTag>(
		lightVerts, lightMis, caster, camera, splatFilm, norm, pixelFilter, PelTag{} );
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
//////////////////////////////////////////////////////////////////////
// EvaluateInteriorConnectionsImpl — templated body shared by the Pel
// and NM variants.  See EvaluateInteriorConnections above for the
// algorithm.  Pel/NM dispatch resolves BSDF evaluations at the light
// and eye endpoints plus throughput-field selection.
//////////////////////////////////////////////////////////////////////
namespace
{
	template<class Tag>
	typename SpectralValueTraits<Tag>::value_type EvaluateInteriorConnectionsImpl(
		const IRayCaster& caster,
		const std::vector<BDPTVertex>& lightVerts,
		const std::vector<VCMMisQuantities>& lightMis,
		const std::vector<BDPTVertex>& eyeVerts,
		const std::vector<VCMMisQuantities>& eyeMis,
		const VCMNormalization& norm,
		const Tag& tag
		)
	{
		using Traits = SpectralValueTraits<Tag>;
		typename Traits::value_type total = Traits::zero();

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

				const typename Traits::value_type fLight =
					RISE::PathValueOps::EvalBSDFAtVertex<Tag>( lv, wiAtLight, lightToEye, tag );
				if( PositiveMagnitude( fLight ) <= 0 ) {
					continue;
				}
				const typename Traits::value_type fEye =
					RISE::PathValueOps::EvalBSDFAtVertex<Tag>( ev, -lightToEye, woAtEye, tag );
				if( PositiveMagnitude( fEye ) <= 0 ) {
					continue;
				}

				const Scalar cosAtLight = fabs( Vector3Ops::Dot( lv.normal, lightToEye ) );
				const Scalar cosAtEye   = fabs( Vector3Ops::Dot( ev.normal, -lightToEye ) );
				if( cosAtLight <= 0 || cosAtEye <= 0 ) {
					continue;
				}
				const Scalar G = ( cosAtLight * cosAtEye ) / distSq;

				const Scalar lightBsdfDirPdfW =
					RISE::PathValueOps::EvalPdfAtVertex<Tag>( lv, wiAtLight, lightToEye, tag );
				const Scalar cameraBsdfDirPdfW =
					RISE::PathValueOps::EvalPdfAtVertex<Tag>( ev, woAtEye, -lightToEye, tag );

				const Scalar lightBsdfRevPdfW =
					RISE::PathValueOps::EvalPdfAtVertex<Tag>( lv, lightToEye, wiAtLight, tag );
				const Scalar cameraBsdfRevPdfW =
					RISE::PathValueOps::EvalPdfAtVertex<Tag>( ev, -lightToEye, woAtEye, tag );

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

				const typename Traits::value_type contrib =
					VertexThroughput<Tag>( lv, tag ) * fLight * ( G * weight ) * fEye
					* VertexThroughput<Tag>( ev, tag );
				total = total + contrib;
			}
		}

		return total;
	}
}

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
	return EvaluateInteriorConnectionsImpl<PelTag>(
		caster, lightVerts, lightMis, eyeVerts, eyeMis, norm, PelTag{} );
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

		// Mirror of ConvertLightSubpath's BSSRDF handling: the entry
		// vertex supplies an area-density directly, so record its
		// reciprocal for VC MIS and skip the ordinary edge-Jacobian path.
		if( v.isBSSRDFEntry ) {
			mis = ApplyBSSRDFEntryAreaUpdate( v );
			outMis[i] = mis;
			// Eye side ungated — see comment at top of ConvertEyeSubpath.
			mis = ApplyBSSRDFEntryOnwardUpdate(
				mis, verts, i, norm );
			continue;
		}

		// MEDIUM vertices propagate both the geometric update (using
		// sigma_t_scalar) and the phase-function sampling update.
		//
		// IMPORTANT: we DO NOT skip on `pdfFwd <= 0`.  See the
		// matching comment in ConvertLightSubpath.
		const bool isMedium = ( v.type == BDPTVertex::MEDIUM );
		const bool skipRecurrence = ( v.type != BDPTVertex::SURFACE && !isMedium );

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

					// Eye side ungated — see top-of-function comment.
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

			// Eye side ungated — see top-of-function comment.
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
//////////////////////////////////////////////////////////////////////
// EvaluateMergesImpl — templated body shared by Pel and NM variants.
// For NM, LightVertexThroughput<NMTag> applies the RISEPelToNMProxy
// luminance projection since the store holds Pel only (v1 behavior).
//////////////////////////////////////////////////////////////////////
namespace
{
	template<class Tag>
	typename SpectralValueTraits<Tag>::value_type EvaluateMergesImpl(
		const std::vector<BDPTVertex>& eyeVerts,
		const std::vector<VCMMisQuantities>& eyeMis,
		const LightVertexStore& store,
		const VCMNormalization& norm,
		const Tag& tag
		)
	{
		using Traits = SpectralValueTraits<Tag>;
		typename Traits::value_type total = Traits::zero();

		if( !norm.mEnableVM || norm.mMergeRadiusSq <= 0 || norm.mVmNormalization <= 0 ) {
			return total;
		}
		if( eyeVerts.size() != eyeMis.size() || eyeVerts.size() < 2 ) {
			return total;
		}
		if( store.Size() == 0 || !store.IsBuilt() ) {
			return total;
		}

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

			typename Traits::value_type pixelMerge = Traits::zero();

			for( std::size_t k = 0; k < candidates.size(); k++ )
			{
				const LightVertex& lv = candidates[k];

				if( ( lv.flags & kLVF_IsConnectible ) == 0 ) {
					continue;
				}

				const Vector3 wiAtEye = -lv.wi;

				const typename Traits::value_type cameraBsdf =
					RISE::PathValueOps::EvalBSDFAtVertex<Tag>( v, wiAtEye, woAtEye, tag );
				if( PositiveMagnitude( cameraBsdf ) <= 0 ) {
					continue;
				}
				const Scalar cameraBsdfDirPdfW =
					RISE::PathValueOps::EvalPdfAtVertex<Tag>( v, woAtEye, wiAtEye, tag );
				const Scalar cameraBsdfRevPdfW =
					RISE::PathValueOps::EvalPdfAtVertex<Tag>( v, wiAtEye, woAtEye, tag );

				const Scalar wLight =
					lv.mis.dVCM * norm.mMisVcWeightFactor
					+ lv.mis.dVM * cameraBsdfDirPdfW;
				const Scalar wCamera =
					eyeMis[i].dVCM * norm.mMisVcWeightFactor
					+ eyeMis[i].dVM * cameraBsdfRevPdfW;
				const Scalar weight = Scalar( 1 ) / ( VCMMis( wLight ) + VCMMis( Scalar( 1 ) ) + VCMMis( wCamera ) );

				pixelMerge = pixelMerge + cameraBsdf * LightVertexThroughput<Tag>( lv, tag ) * weight;
			}

			total = total + VertexThroughput<Tag>( v, tag ) * pixelMerge * norm.mVmNormalization;
		}

		return total;
	}
}

RISEPel VCMIntegrator::EvaluateMerges(
	const std::vector<BDPTVertex>& eyeVerts,
	const std::vector<VCMMisQuantities>& eyeMis,
	const LightVertexStore& store,
	const VCMNormalization& norm
	) const
{
	return EvaluateMergesImpl<PelTag>( eyeVerts, eyeMis, store, norm, PelTag{} );
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

//////////////////////////////////////////////////////////////////////
// EvaluateS0NM — thin forwarder to the templated EvaluateS0Impl with
// an NMTag(nm) context.  See the Pel-side EvaluateS0 comment block
// above for the algorithm and the SPECTRAL VARIANTS block for
// wavelength handling.
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
	return EvaluateS0Impl<NMTag>( scene, caster, eyeVerts, eyeMis, NMTag( nm ) );
}

//////////////////////////////////////////////////////////////////////
// EvaluateNEENM — thin forwarder to the templated EvaluateNEEImpl.
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
	return EvaluateNEEImpl<NMTag>(
		scene, caster, sampler, eyeVerts, eyeMis, norm, NMTag( nm ) );
}

//////////////////////////////////////////////////////////////////////
// SplatLightSubpathToCameraNM — thin forwarder to the templated
// SplatLightSubpathToCameraImpl.  ToSplatRGB<NMTag> applies
// ColorUtils::XYZFromNM inside the template body.
//////////////////////////////////////////////////////////////////////
void VCMIntegrator::SplatLightSubpathToCameraNM(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<VCMMisQuantities>& lightMis,
	const IScene& /*scene*/,
	const IRayCaster& caster,
	const ICamera& camera,
	SplatFilm& splatFilm,
	const VCMNormalization& norm,
	const Scalar nm,
	const IPixelFilter* pixelFilter
	) const
{
	SplatLightSubpathToCameraImpl<NMTag>(
		lightVerts, lightMis, caster, camera, splatFilm, norm, pixelFilter, NMTag( nm ) );
}

//////////////////////////////////////////////////////////////////////
// EvaluateInteriorConnectionsNM — thin forwarder to
// EvaluateInteriorConnectionsImpl with an NMTag context.
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
	return EvaluateInteriorConnectionsImpl<NMTag>(
		caster, lightVerts, lightMis, eyeVerts, eyeMis, norm, NMTag( nm ) );
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
	return EvaluateMergesImpl<NMTag>( eyeVerts, eyeMis, store, norm, NMTag( nm ) );
}
