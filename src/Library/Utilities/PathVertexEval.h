//////////////////////////////////////////////////////////////////////
//
//  PathVertexEval.h - Shared BSDF and PDF evaluation at path vertices
//
//  Free functions that evaluate material responses at path vertices.
//  The BDPTVertex overloads were extracted from BDPTIntegrator.
//  The PT-compatible overloads accept IBSDF/ISPF pointers and
//  RayIntersectionGeometric directly, matching the PT's native state.
//  Both sets can be reused by future transport consumers (RIS-based
//  guiding, connection strategies) without depending on either
//  integrator class.
//
//  The functions handle three vertex types uniformly:
//    - Surface vertices: delegate to IBSDF::value / ISPF::Pdf
//    - Medium vertices:  delegate to IPhaseFunction
//    - BSSRDF entry vertices: evaluate Sw(direction) = Ft(cos) / (c*PI)
//
//  DIRECTION CONVENTION:
//    Both wi and wo are "away from vertex" directions.
//    RISE's IBSDF::value(vLightIn, ri) expects:
//      vLightIn (wi): direction away from surface toward the light
//      ri.ray.Dir(): direction toward the surface (incoming viewer ray)
//    These functions negate wo to build ri.ray.Dir() for surface
//    evaluations, and negate wi for phase function calls (which expect
//    wi toward the scatter point).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 2, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATH_VERTEX_EVAL_
#define PATH_VERTEX_EVAL_

#include "../Shaders/BDPTVertex.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Interfaces/IPhaseFunction.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/BSSRDFSampling.h"
#include "../Utilities/IORStack.h"
#include "Math3D/Math3D.h"

namespace RISE
{
	namespace PathVertexEval
	{
		//////////////////////////////////////////////////////////////////////
		// IOR Stack Reconstruction
		//////////////////////////////////////////////////////////////////////

		/// Reconstructs an IOR stack from the vertex's stored medium IOR
		/// and object membership state.  Used by EvalPdfAtVertex to
		/// provide the correct IOR context for SPF::Pdf evaluation.
		inline void BuildVertexIORStack(
			const BDPTVertex& vertex,
			IORStack& stack
			)
		{
			if( !vertex.pObject ) {
				return;
			}

			if( vertex.insideObject ) {
				stack = IORStack( 1.0 );
				stack.SetCurrentObject( vertex.pObject );
				stack.push( vertex.mediumIOR );
			} else {
				stack = IORStack( vertex.mediumIOR );
				stack.SetCurrentObject( vertex.pObject );
			}
		}

		//////////////////////////////////////////////////////////////////////
		// PopulateRIGFromVertex
		//
		// Single source of truth for reconstructing a
		// RayIntersectionGeometric from a stored BDPTVertex's surface state.
		// Used by every BDPT/VCM site that hands a manually-built `ri` to a
		// BSDF, painter, or material query.  Centralising the copy keeps
		// the BDPTVertex / RayIntersectionGeometric surface-state field
		// lists from drifting out of sync — a class of bug that previously
		// produced silent BDPT firefly regressions when a new field
		// (vertex color, surface derivatives, ...) was added to the
		// intersection struct but missed at one of the manual reconstruction
		// sites.
		//
		// CONTRACT — when adding any new field to RayIntersectionGeometric
		// that is consumed by IBSDF::value / IPainter / IMaterial::Get*
		// paths, you MUST:
		//   1. Add the corresponding field to BDPTVertex (surface state
		//      block).
		//   2. Populate it during eye/light subpath generation in
		//      BDPTIntegrator::GenerateEyeSubpath / GenerateLightSubpath
		//      (and the spectral / VCM equivalents).
		//   3. Add the copy below.
		//   4. Extend tests/BDPTVertexRIGRebuildTest.cpp with a sentinel
		//      assertion for the new field -- one per SCALAR, POINTER and
		//      FLAG, not one per struct: a nested struct copied by
		//      assignment still has to be checked member by member, or a
		//      hand-rolled field-by-field copy could drop one silently.
		// The cross-reference comment in BDPTVertex.h points the next
		// developer at this contract.
		//
		// THE PAINTER-INPUT FIELDS ARE CARRIED (2026-09-11,
		// docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §3).  This block
		// used to say the opposite -- `derivatives`, `signals` and
		// `txFootprint` were DECLINED, so an expression evaluated at a
		// BDPT / VCM / MLT vertex read the documented neutral for `curv`,
		// `curvR`, `occlusion`, `thickness`, `convexity`, `proximity` and
		// `interior`.  That was a BIAS, not a flat mask: the forward walk
		// SAMPLES its continuation direction against the live record the
		// object manager stamped and then PRICES that same sample -- and
		// every (s,t) connection, every MIS reverse pdf, every VCM merge
		// at the eye vertex -- against the record rebuilt here.  One
		// material, one vertex, one path, sampled with the true roughness
		// and weighted with the neutral one (design §1).  All three are
		// now copied below.
		//
		// WHY THE COPY IS SAFE UNDER THE HYGIENE INVARIANT (§3.1).
		// SourceHygieneTest pins the set of files that assign `signals`,
		// and the invariant it protects is "nothing puts a
		// DEFAULT-CONSTRUCTED channel over `ObjectManager::IntersectRay`'s
		// stamp".  This copy does the reverse: it FORWARDS that stamp.
		// The chain is `ObjectManager::IntersectRay` stamps
		// `ri.geometric.signals` -> the subpath generator copies it onto
		// the vertex -> this function copies it back into the rebuilt
		// record.  The value a painter finally reads is the object
		// manager's own, two hops later, never a default over a stamp.
		// `PathVertexEval.h` and `BDPTIntegrator.cpp` are in the allowed
		// writer set for exactly that reason.
		//
		// TWO HONEST DEFAULTS REMAIN, and they are defaults because no
		// stamp exists to forward -- not because the copy declines one:
		//
		//   * BSSRDF ENTRY VERTICES.  Their record is built from
		//     `BSSRDFSampling::SampleResult` (entry point, normals, ONB),
		//     which is a sampled point on a surface, not an intersection
		//     the object manager resolved.  PT builds its entry record by
		//     hand the same way, so an IOR or Fresnel painter keyed on a
		//     signal at a subsurface entry reads neutral under EVERY
		//     integrator alike -- integrator-consistent, and disclosed as
		//     a residual in docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md
		//     §10.  Closing it means a probe record in
		//     `BSSRDFSampling::SampleResult`; out of scope here.
		//   * NON-SURFACE VERTICES -- camera, env and medium.  There is no
		//     surface to publish signals for, so the defaults are the
		//     whole truth.  (A rebuild is only ever handed to a
		//     BSDF / painter at a SURFACE vertex; medium vertices go to
		//     the phase function, which reads none of these.)
		//   * A `type == LIGHT` ROOT VERTEX is NOT in that list any more.
		//     It was until 2026-09-11: a mesh luminary's root IS a
		//     surface, and `LuminaryRadiance` (BDPT's t=1 light-to-camera
		//     splat and s=1 connections; MLT shares the generator) prices
		//     it through THIS function -- NOT VCM: `SplatLightSubpathToCameraImpl`
		//     skips the LIGHT root and `EvaluateS0Impl` prices a real ray
		//     hit (DL-44 review correction, 2026-09-14) -- so leaving it
		//     default made an emissive material keyed on a signal read
		//     neutral there.  Slice S3
		//     (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5) has
		//     `GenerateLightSubpathImpl` copy a PROBED payload onto it,
		//     from `LightSample::surface`, so this function replays that
		//     too.  A DELTA light's root, and an env root, still carry the
		//     defaults -- correctly: neither has a surface.
		//
		// `txFootprint` is carried although it is all-zero under today's
		// bidirectional rasterizers -- BDPT / VCM / MLT emit no ray
		// differentials, so nothing stamps it there.  The contract is
		// "every field a painter consumer reads", and carrying it now
		// means a future bidirectional ray-differential landing cannot
		// silently reopen the gap.
		//////////////////////////////////////////////////////////////////////
		inline void PopulateRIGFromVertex(
			const BDPTVertex& vertex,
			RayIntersectionGeometric& ri
			)
		{
			ri.bHit            = true;
			ri.ptIntersection  = vertex.position;
			ri.vNormal         = vertex.normal;
			ri.vGeomNormal     = vertex.geomNormal;
			ri.onb             = vertex.onb;
			ri.ptCoord         = vertex.ptCoord;
			ri.ptCoord1        = vertex.ptCoord1;
			ri.bHasTexCoord1   = vertex.bHasTexCoord1;
			ri.ptObjIntersec   = vertex.ptObjIntersec;
			ri.vColor          = vertex.vColor;
			ri.bHasVertexColor = vertex.bHasVertexColor;
			// The painter-input triple.  `signals` here is the object
			// manager's own stamp arriving two hops later (stamped on the
			// live record, copied onto the vertex by the subpath
			// generator, copied back out here) -- never a
			// default-constructed channel placed over a stamp, which is
			// the invariant SourceHygieneTest's writer-set check protects.
			// See the contract block above.
			ri.derivatives     = vertex.derivatives;
			ri.signals         = vertex.signals;
			ri.txFootprint     = vertex.txFootprint;
			// G6: replay the enclosing (ambient) medium IOR captured at trace
			// time (BDPTVertex::mediumIOR = IORStack::top() at hit production;
			// SetCurrentObject does NOT push, so this is the medium the ray was
			// travelling through — the ambient index for the conductor Fresnel).
			// This is the shared BDPT + VCM connection-time BSDF-eval path, so a
			// single stamp here covers value()/valueNM() for both integrators.
			// Guard a non-positive stored IOR to air (1.0).
			ri.ambientIOR      = ( vertex.mediumIOR > 0.0 ) ? vertex.mediumIOR : 1.0;
		}

		//////////////////////////////////////////////////////////////////////
		// RGB BSDF Evaluation
		//////////////////////////////////////////////////////////////////////

		/// Evaluates the BSDF value f(wi, wo) at a path vertex.
		///
		/// Handles surface, medium, and BSSRDF entry vertices uniformly.
		/// Both wi and wo are "away from vertex" directions.
		///
		/// \param vertex  The path vertex to evaluate at
		/// \param wi      Incoming light direction (away from surface)
		/// \param wo      Outgoing view direction (away from surface)
		/// \return RGB BSDF value (or phase function value for media)
		inline RISEPel EvalBSDFAtVertex(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo
			)
		{
			// Medium scatter vertex: evaluate the phase function.
			// Phase functions are symmetric (p(wi,wo) = p(wo,wi)) and
			// isotropic w.r.t. surface normal.
			// IPhaseFunction::Evaluate expects wi toward the scatter point,
			// so negate wi to convert from "away" to "toward" convention.
			if( vertex.type == BDPTVertex::MEDIUM ) {
				if( !vertex.pPhaseFunc ) {
					return RISEPel( 0, 0, 0 );
				}
				const Scalar p = vertex.pPhaseFunc->Evaluate( -wi, wo );
				return RISEPel( p, p, p );
			}

			if( !vertex.pMaterial ) {
				return RISEPel( 0, 0, 0 );
			}

			// BSSRDF entry vertex: evaluate Sw(direction) = Ft(cos) / (c*PI).
			// This is the directional component of the separable BSSRDF at
			// the re-emission point.
			if( vertex.isBSSRDFEntry ) {
				ISubSurfaceDiffusionProfile* pProfile = vertex.pMaterial->GetDiffusionProfile();
				if( pProfile ) {
					// wi is the direction into the surface (from outside).
					// No fabs: back-face connections (cosTheta < 0) return zero.
					const Scalar cosTheta = Vector3Ops::Dot( wi, vertex.normal );
					if( cosTheta <= NEARZERO ) {
						return RISEPel( 0, 0, 0 );
					}

					// Use the canonical helper so a future field added to
					// RayIntersectionGeometric reaches the BSSRDF profile's
					// FresnelTransmission / GetIOR without silently
					// defaulting (mirrors the contract block above).
					RayIntersectionGeometric rig(
						Ray( vertex.position, -wi ), nullRasterizerState );
					PopulateRIGFromVertex( vertex, rig );

					const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, rig );
					const Scalar eta = pProfile->GetIOR( rig );
					const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, eta );
					return RISEPel( Sw, Sw, Sw );
				}

				// Random-walk SSS: Sw with Schlick Fresnel
				const RandomWalkSSSParams* pRW = vertex.pMaterial->GetRandomWalkSSSParams();
				if( pRW ) {
					const Scalar cosTheta = Vector3Ops::Dot( wi, vertex.normal );
					if( cosTheta <= NEARZERO ) {
						return RISEPel( 0, 0, 0 );
					}
					const Scalar F0 = ((pRW->ior - 1.0) / (pRW->ior + 1.0)) *
						((pRW->ior - 1.0) / (pRW->ior + 1.0));
					const Scalar FSchlick = F0 + (1.0 - F0) * pow( 1.0 - cosTheta, 5.0 );
					const Scalar FtEntry = 1.0 - FSchlick;
					const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, pRW->ior );
					return RISEPel( Sw, Sw, Sw );
				}

				return RISEPel( 0, 0, 0 );
			}

			const IBSDF* pBSDF = vertex.pMaterial->GetBSDF();
			if( !pBSDF ) {
				return RISEPel( 0, 0, 0 );
			}

			// Build a RayIntersectionGeometric for the BSDF evaluation.
			// Negate wo to get ri.ray.Dir() toward the surface.
			Ray evalRay( vertex.position, -wo );
			RayIntersectionGeometric ri( evalRay, nullRasterizerState );
			PopulateRIGFromVertex( vertex, ri );

			return pBSDF->value( wi, ri );
		}

		//////////////////////////////////////////////////////////////////////
		// VertexPdfContext
		//
		// The reconstructed evaluation state of ONE vertex, shared across
		// the several `EvalPdfAtVertex` / `EvalPdfAtVertexNM` queries a
		// caller makes at that same vertex.
		//
		// WHY.  `EvalPdfAtVertex` has to rebuild a
		// `RayIntersectionGeometric` (via `PopulateRIGFromVertex`, which
		// copies the whole surface-state block including `onb`,
		// `derivatives`, `signals` and `txFootprint`) and an `IORStack`
		// from the stored vertex before it can ask the SPF anything.
		// Since DL-69 both subpath generators query the SAME vertex TWICE
		// per non-delta scatter -- once for this vertex's own `pdfFwd`
		// (wi = -incoming, wo = scattered) and once for the predecessor's
		// `pdfRev` (the same two directions with their roles swapped) --
		// so that rebuild ran twice per vertex per subpath.  Measured on
		// an all-`schlick_material` 256x256 / 256-spp BDPT render, the
		// duplicated rebuild is worth a few percent of whole-render CPU.
		//
		// WHAT IS SHARED AND WHAT IS NOT.  Everything
		// `PopulateRIGFromVertex` writes, plus the IOR stack, is a
		// function of the VERTEX alone and is built once.  The record's
		// `ray` is NOT: it encodes `wi` (the SPFs read `ri.ray.Dir()` to
		// anchor their geometric frame), so it is re-aimed per query by
		// `RecordFor`.  A context is therefore only valid while the
		// vertex it was built from is alive and unmodified -- it stores a
		// reference, so it must not outlive a `push_back` that could
		// reallocate the vertex array.
		//
		// The state is built LAZILY, on the first query that actually
		// needs it, so a vertex that short-circuits (MEDIUM, no material,
		// BSSRDF entry, or a material with no SPF) pays nothing -- the
		// same early-outs the standalone functions have always had.
		//////////////////////////////////////////////////////////////////////
		class VertexPdfContext
		{
		public:
			explicit VertexPdfContext( const BDPTVertex& vertex ) :
			  m_vertex( vertex ),
			  m_ri( Ray( vertex.position, Vector3( 0, 0, 1 ) ), nullRasterizerState ),
			  m_stack( 1.0 ),
			  m_pSPF( 0 ),
			  m_built( false )
			{}

			//! The vertex this context was built from.
			inline const BDPTVertex& Vertex() const { return m_vertex; }

			//! The vertex's SPF, or null when it has no material or the
			//! material has no SPF.  Builds the shared record and IOR
			//! stack on the first call that returns non-null.
			inline const ISPF* PrepareSPF()
			{
				if( !m_built ) {
					m_built = true;
					if( m_vertex.pMaterial ) {
						m_pSPF = m_vertex.pMaterial->GetSPF();
					}
					if( m_pSPF ) {
						PopulateRIGFromVertex( m_vertex, m_ri );
						BuildVertexIORStack( m_vertex, m_stack );
					}
				}
				return m_pSPF;
			}

			//! The shared record, re-aimed for this query's `wi`.  Only
			//! call after `PrepareSPF()` returned non-null.
			inline RayIntersectionGeometric& RecordFor( const Vector3& wi )
			{
				m_ri.ray = Ray( m_vertex.position, -wi );
				return m_ri;
			}

			inline const IORStack& Stack() const { return m_stack; }

		private:
			// Non-copyable: it holds a reference and a heavy record, and
			// every use is a short-lived local.
			VertexPdfContext( const VertexPdfContext& );
			VertexPdfContext& operator=( const VertexPdfContext& );

			const BDPTVertex&			m_vertex;
			RayIntersectionGeometric	m_ri;
			IORStack					m_stack;
			const ISPF*					m_pSPF;
			bool						m_built;
		};

		//////////////////////////////////////////////////////////////////////
		// RGB PDF Evaluation
		//////////////////////////////////////////////////////////////////////

		/// The vertex kinds whose sampling PDF needs no reconstructed
		/// record at all.  Shared by the Pel and NM evaluators and by both
		/// their context-carrying and standalone forms, so the standalone
		/// form never builds a `VertexPdfContext` (and therefore never a
		/// `RayIntersectionGeometric`) for a vertex that would not use it
		/// -- which is what the pre-context code did.  The three cases are
		/// wavelength-independent, which is why one helper serves both.
		///
		/// \return true when `outPdf` is the answer; false when the caller
		///         must go on to query the SPF.
		inline bool EvalPdfAtVertexShortCircuit(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			Scalar& outPdf
			)
		{
			// Medium scatter vertex: phase function sampling PDF.
			if( vertex.type == BDPTVertex::MEDIUM ) {
				outPdf = vertex.pPhaseFunc ? vertex.pPhaseFunc->Pdf( -wi, wo ) : Scalar( 0 );
				return true;
			}

			if( !vertex.pMaterial ) {
				outPdf = 0;
				return true;
			}

			// BSSRDF entry vertex: cosine-weighted hemisphere PDF.
			if( vertex.isBSSRDFEntry ) {
				outPdf = fabs( Vector3Ops::Dot( wo, vertex.normal ) ) * INV_PI;
				return true;
			}

			return false;
		}

		/// Evaluates the SPF sampling PDF at a path vertex, reusing a
		/// context's already-reconstructed record and IOR stack.
		///
		/// Semantically identical to the standalone overload below.
		inline Scalar EvalPdfAtVertex(
			VertexPdfContext& ctx,
			const Vector3& wi,
			const Vector3& wo
			)
		{
			Scalar shortCircuit = 0;
			if( EvalPdfAtVertexShortCircuit( ctx.Vertex(), wi, wo, shortCircuit ) ) {
				return shortCircuit;
			}

			const ISPF* pSPF = ctx.PrepareSPF();
			if( !pSPF ) {
				return 0;
			}

			// Negate wi to get toward-surface direction for ri.ray.Dir()
			return pSPF->Pdf( ctx.RecordFor( wi ), wo, ctx.Stack() );
		}

		/// Evaluates the SPF sampling PDF at a path vertex.
		///
		/// \param vertex  The path vertex to evaluate at
		/// \param wi      Incoming direction (away from surface)
		/// \param wo      Outgoing direction (away from surface)
		/// \return Solid-angle PDF for sampling wo given wi
		inline Scalar EvalPdfAtVertex(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo
			)
		{
			Scalar shortCircuit = 0;
			if( EvalPdfAtVertexShortCircuit( vertex, wi, wo, shortCircuit ) ) {
				return shortCircuit;
			}

			VertexPdfContext ctx( vertex );
			return EvalPdfAtVertex( ctx, wi, wo );
		}

		//////////////////////////////////////////////////////////////////////
		// Spectral (NM) BSDF Evaluation
		//////////////////////////////////////////////////////////////////////

		/// Evaluates the BSDF value at a vertex for a single wavelength.
		inline Scalar EvalBSDFAtVertexNM(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const Scalar nm
			)
		{
			// Medium scatter vertex: phase function (wavelength-independent)
			if( vertex.type == BDPTVertex::MEDIUM ) {
				if( !vertex.pPhaseFunc ) {
					return 0;
				}
				return vertex.pPhaseFunc->Evaluate( -wi, wo );
			}

			if( !vertex.pMaterial ) {
				return 0;
			}

			// BSSRDF entry vertex: Sw(direction)
			if( vertex.isBSSRDFEntry ) {
				ISubSurfaceDiffusionProfile* pProfile = vertex.pMaterial->GetDiffusionProfile();
				if( pProfile ) {
					const Scalar cosTheta = Vector3Ops::Dot( wi, vertex.normal );
					if( cosTheta <= NEARZERO ) {
						return 0;
					}

					// Spectral counterpart of the BSSRDF entry path above —
					// same helper-based rebuild for the same drift-prevention
					// rationale.
					RayIntersectionGeometric rig(
						Ray( vertex.position, -wi ), nullRasterizerState );
					PopulateRIGFromVertex( vertex, rig );

					const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, rig );
					const Scalar eta = pProfile->GetIOR( rig );
					return BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, eta );
				}

				// Random-walk SSS: Sw with Schlick Fresnel
				const RandomWalkSSSParams* pRW = vertex.pMaterial->GetRandomWalkSSSParams();
				RandomWalkSSSParams rwParamsNM;
				if( !pRW && vertex.pMaterial->GetRandomWalkSSSParamsNM( nm, rwParamsNM ) ) {
					pRW = &rwParamsNM;
				}
				if( pRW ) {
					const Scalar cosTheta = Vector3Ops::Dot( wi, vertex.normal );
					if( cosTheta <= NEARZERO ) {
						return 0;
					}
					const Scalar F0 = ((pRW->ior - 1.0) / (pRW->ior + 1.0)) *
						((pRW->ior - 1.0) / (pRW->ior + 1.0));
					const Scalar FSchlick = F0 + (1.0 - F0) * pow( 1.0 - cosTheta, 5.0 );
					const Scalar FtEntry = 1.0 - FSchlick;
					return BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, pRW->ior );
				}

				return 0;
			}

			const IBSDF* pBSDF = vertex.pMaterial->GetBSDF();
			if( !pBSDF ) {
				return 0;
			}

			Ray evalRay( vertex.position, -wo );
			RayIntersectionGeometric ri( evalRay, nullRasterizerState );
			PopulateRIGFromVertex( vertex, ri );

			return pBSDF->valueNM( wi, ri, nm );
		}

		//////////////////////////////////////////////////////////////////////
		// Spectral (NM) PDF Evaluation
		//////////////////////////////////////////////////////////////////////

		/// Evaluates the SPF sampling PDF at a vertex for a single
		/// wavelength, reusing a context's reconstructed record and IOR
		/// stack.  Semantically identical to the standalone overload below.
		inline Scalar EvalPdfAtVertexNM(
			VertexPdfContext& ctx,
			const Vector3& wi,
			const Vector3& wo,
			const Scalar nm
			)
		{
			Scalar shortCircuit = 0;
			if( EvalPdfAtVertexShortCircuit( ctx.Vertex(), wi, wo, shortCircuit ) ) {
				return shortCircuit;
			}

			const ISPF* pSPF = ctx.PrepareSPF();
			if( !pSPF ) {
				return 0;
			}

			return pSPF->PdfNM( ctx.RecordFor( wi ), wo, nm, ctx.Stack() );
		}

		/// Evaluates the SPF sampling PDF at a vertex for a single wavelength.
		inline Scalar EvalPdfAtVertexNM(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const Scalar nm
			)
		{
			Scalar shortCircuit = 0;
			if( EvalPdfAtVertexShortCircuit( vertex, wi, wo, shortCircuit ) ) {
				return shortCircuit;
			}

			VertexPdfContext ctx( vertex );
			return EvalPdfAtVertexNM( ctx, wi, wo, nm );
		}
		//////////////////////////////////////////////////////////////////////
		// PT-compatible overloads
		//
		// The unidirectional path tracer does not use BDPTVertex.  These
		// overloads accept the PT's native state (IBSDF + ISPF pointers
		// and RayIntersectionGeometric) so that PT can share the same
		// evaluation entry point as BDPT.
		//////////////////////////////////////////////////////////////////////

		/// Evaluate BSDF at a PT surface point.
		///
		/// \param pBRDF   Material BSDF (non-null)
		/// \param wi      Scattered direction (away from surface)
		/// \param ri      Geometric intersection containing the surface state
		/// \return BSDF value (RGB)
		inline RISEPel EvalBSDFAtSurface(
			const IBSDF* pBRDF,
			const Vector3& wi,
			const RayIntersectionGeometric& ri
			)
		{
			return pBRDF->value( wi, ri );
		}

		/// Evaluate PDF at a PT surface point.
		///
		/// \param pSPF     Scattering probability function (may be null)
		/// \param ri       Geometric intersection containing the surface state
		/// \param wi       Scattered direction (away from surface)
		/// \param ior_stack Current IOR stack for dielectric tracking
		/// \return PDF in solid angle measure, or 0 if pSPF is null
		inline Scalar EvalPdfAtSurface(
			const ISPF* pSPF,
			const RayIntersectionGeometric& ri,
			const Vector3& wi,
			const IORStack& ior_stack
			)
		{
			return pSPF ? pSPF->Pdf( ri, wi, ior_stack ) : 0;
		}

		/// Evaluate BSDF at a PT surface point (spectral).
		///
		/// \param pBRDF   Material BSDF (non-null)
		/// \param wi      Scattered direction (away from surface)
		/// \param ri      Geometric intersection containing the surface state
		/// \param nm      Wavelength in nanometers
		/// \return Spectral BSDF value
		inline Scalar EvalBSDFAtSurfaceNM(
			const IBSDF* pBRDF,
			const Vector3& wi,
			const RayIntersectionGeometric& ri,
			const Scalar nm
			)
		{
			return pBRDF->valueNM( wi, ri, nm );
		}

		/// Evaluate PDF at a PT surface point (spectral).
		///
		/// \param pSPF     Scattering probability function (may be null)
		/// \param ri       Geometric intersection containing the surface state
		/// \param wi       Scattered direction (away from surface)
		/// \param nm       Wavelength in nanometers
		/// \param ior_stack Current IOR stack for dielectric tracking (may be null)
		/// \return PDF in solid angle measure, or 0 if pSPF is null
		inline Scalar EvalPdfAtSurfaceNM(
			const ISPF* pSPF,
			const RayIntersectionGeometric& ri,
			const Vector3& wi,
			const Scalar nm,
			const IORStack& ior_stack
			)
		{
			return pSPF ? pSPF->PdfNM( ri, wi, nm, ior_stack ) : 0;
		}
	}
}

#endif
