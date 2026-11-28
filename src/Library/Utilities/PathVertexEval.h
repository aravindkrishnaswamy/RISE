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
#include "../Interfaces/IObject.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Interfaces/IPhaseFunction.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Intersection/TextureFootprintCompute.h"
#include "../Cameras/CameraUtilities.h"
#include "../Utilities/BSSRDFSampling.h"
#include "../Utilities/IORStack.h"
#include "Math3D/Math3D.h"

namespace RISE
{
	namespace PathVertexEval
	{
		//! Stamp the pixel footprint (`dpdx`, `dpdy` from
		//! `BDPTCameraUtilities::PixelFootprintThrough`) onto a light-subpath
		//! surface vertex before t == 1 camera-connection shading so
		//! footprint-filtered painters match eye-path strategies. Computes
		//! world/object widths and calls `SolveFootprintUV` from the stored
		//! post-modifier vertex (DL-14 / DL-402).
		inline void StampPixelFootprint( BDPTVertex& vertex, const Vector3& dpdx, const Vector3& dpdy )
		{
			TextureFootprint fp;
			fp.dpdx = dpdx;
			fp.dpdy = dpdy;
			fp.worldWidth = Scalar( 0.5 ) * ( Vector3Ops::Magnitude( dpdx ) + Vector3Ops::Magnitude( dpdy ) );
			fp.objectWidth = fp.worldWidth;
			if( vertex.pObject ) {
				const Matrix4 toObject = vertex.pObject->GetFinalInverseTransformMatrix();
				fp.objectWidth = Scalar( 0.5 ) * (
					Vector3Ops::Magnitude( Vector3Ops::Transform( toObject, dpdx ) ) +
					Vector3Ops::Magnitude( Vector3Ops::Transform( toObject, dpdy ) ) );
			}
			fp.widthValid = true;
			RayIntersectionGeometric rig( Ray( vertex.position, vertex.normal ), nullRasterizerState );
			rig.vNormal = vertex.normal;
			rig.derivatives = vertex.derivatives;
			rig.txFootprint = fp;
			SolveFootprintUV( rig );
			vertex.txFootprint = rig.txFootprint;
		}

		//! Returns a copy of `vertex` in `scratch` stamped with its t == 1
		//! pixel footprint through `ap` (or `vertex` itself if not a surface or
		//! footprint is undefined). Used for BSDF value queries only; MIS
		//! densities still read the unfiltered stored vertex.
		inline const BDPTVertex& WithPixelFootprint(
			const ICamera& camera,
			const BDPTCameraUtilities::ApertureSample& ap,
			const BDPTVertex& vertex,
			BDPTVertex& scratch )
		{
			Vector3 dpdx, dpdy;
			if( vertex.type != BDPTVertex::SURFACE ||
				!BDPTCameraUtilities::PixelFootprintThrough( camera, ap, vertex.position, vertex.normal, dpdx, dpdy ) ) {
				return vertex;
			}
			scratch = vertex;
			StampPixelFootprint( scratch, dpdx, dpdy );
			return scratch;
		}

		// Radiance is integrated against the shading-frame projected solid
		// angle: f_s(wi,wo) |Ns.wi| dwi. Connections and photon density
		// estimates instead carry geometric-area measure, so their response
		// is f_A = f_s |Ns.wi|/|Ng.wi|. Keep the geometric Jacobians intact.
		// A zero geometric cosine is a zero-measure direction, not a place
		// to clamp a finite ratio. Medium vertices have no surface measure.
		inline Scalar RadianceShadingNormalFactor(
			const Vector3& shadingNormal, const Vector3& geometricNormal,
			const Vector3& wi )
		{
			const Scalar ng = fabs( Vector3Ops::Dot( geometricNormal, wi ) );
			return ng > Scalar(0)
				? fabs( Vector3Ops::Dot( shadingNormal, wi ) ) / ng : Scalar(0);
		}

		inline Scalar RadianceShadingNormalFactor(
			const BDPTVertex& vertex, const Vector3& wi )
		{
			return vertex.type == BDPTVertex::SURFACE
				? RadianceShadingNormalFactor( vertex.normal, vertex.geomNormal, wi )
				: Scalar(1);
		}

		// A light walk samples wo, whereas the radiance kernel's cosine is
		// on wi. Its SPF kray already contains |Ns.wo|/pdf(wo); the adjoint
		// weight therefore needs |Ns.wi||Ng.wo|/(|Ng.wi||Ns.wo|).
		// This is wavelength-independent and belongs to importance walks
		// only. PDFs remain densities of the unmodified sampling procedure.
		inline Scalar ImportanceShadingNormalFactor(
			const Vector3& shadingNormal, const Vector3& geometricNormal,
			const Vector3& wi, const Vector3& wo )
		{
			const Scalar ngIn = fabs( Vector3Ops::Dot( geometricNormal, wi ) );
			const Scalar nsOut = fabs( Vector3Ops::Dot( shadingNormal, wo ) );
			if( ngIn == Scalar(0) || nsOut == Scalar(0) ) return Scalar(0);
			return ( fabs( Vector3Ops::Dot( shadingNormal, wi ) ) *
				fabs( Vector3Ops::Dot( geometricNormal, wo ) ) ) / ( ngIn * nsOut );
		}

		inline Scalar ImportanceShadingNormalFactor(
			const BDPTVertex& vertex, const Vector3& wi, const Vector3& wo )
		{
			return vertex.type == BDPTVertex::SURFACE
				? ImportanceShadingNormalFactor( vertex.normal, vertex.geomNormal, wi, wo )
				: Scalar(1);
		}

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
		// Single source of truth for rebuilding RayIntersectionGeometric from
		// a stored BDPTVertex across BDPT/VCM/MLT.
		//
		// CONTRACT: when adding any field to RayIntersectionGeometric read by
		// IBSDF / IPainter / IMaterial:
		//   1. Add it to BDPTVertex (surface state block).
		//   2. Populate it in eye/light subpath generation (RGB/NM/VCM).
		//   3. Copy it below.
		//   4. Add per-member sentinel checks to tests/BDPTVertexRIGRebuildTest.cpp.
		//
		// Forwards `derivatives`, `signals`, and `txFootprint` verbatim from
		// ObjectManager::IntersectRay (and probed `LightSample::surface` on
		// mesh luminary roots; docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md).
		// Safe under SourceHygieneTest's writer-set invariant because it
		// forwards stamped data rather than overwriting with defaults.
		// Remaining honest defaults: BSSRDF entry vertices (no probe hit) and
		// non-surface / delta / env vertices.
		//////////////////////////////////////////////////////////////////////
		inline void PopulateRIGFromVertex(
			const BDPTVertex& vertex,
			RayIntersectionGeometric& ri
			)
		{
			ri.bHit            = true;
			ri.ray.origin      = Point3Ops::mkPoint3( vertex.position, -(ri.ray.Dir() * vertex.scatterIncomingDistance) );
			ri.ptIntersection  = vertex.position;
			ri.vNormal         = vertex.normal;
			ri.vGeomNormal     = vertex.geomNormal;
			// Surface-identity flags and live arrival facing (DL-341).
			ri.bGeomNormalOrientedToRay = vertex.bGeomNormalOrientedToRay;
			ri.bGeomNormalRayDerived    = vertex.bGeomNormalRayDerived;
			ri.bOpenSheet               = vertex.bOpenSheet;
			ri.bProvablyNoInterior      = vertex.bProvablyNoInterior;
			ri.arrivalGeomFacing        = vertex.bGeomNormalOpposesArrival ? -1 : 1;
			ri.onb             = vertex.onb;
			// Coat normal decode frame (DL-302).
			ri.bHasCoatDecodeOnb = vertex.bHasCoatDecodeOnb;
			if( vertex.bHasCoatDecodeOnb ) ri.coatDecodeOnb = vertex.coatDecodeOnb;
			ri.ptCoord         = vertex.ptCoord;
			ri.ptCoord1        = vertex.ptCoord1;
			ri.bHasTexCoord1   = vertex.bHasTexCoord1;
			ri.ptObjIntersec   = vertex.ptObjIntersec;
			ri.vColor          = vertex.vColor;
			ri.bHasVertexColor = vertex.bHasVertexColor;
			// Forward stamped painter inputs (see contract above).
			ri.derivatives     = vertex.derivatives;
			ri.signals         = vertex.signals;
			ri.txFootprint     = vertex.txFootprint;
			ri.pmxWorldToObject = vertex.pmxWorldToObject;
			ri.pmxObjectToWorld = vertex.pmxObjectToWorld;
			ri.pCamera          = vertex.pCamera;
			ri.pObjectInfo      = vertex.pObjectInfo;
			ri.bIsCameraRay     = vertex.bIsCameraRay;
			// Replay enclosing medium IOR captured at trace time (fallback to air).
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

					// DL-49: `rig.ambientIOR` is the vertex's exterior
					// index (`BDPTVertex::mediumIOR`, replayed by
					// PopulateRIGFromVertex); Fresnel and its Sw
					// normalization use the RELATIVE index against it.
					const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, rig );
					const Scalar eta = BSSRDFSampling::RelativeBoundaryIOR(
						pProfile->GetIOR( rig ), BSSRDFSampling::ExteriorIOR( rig ) );
					const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, eta );
					return RISEPel( Sw, Sw, Sw );
				}

				// Random-walk SSS: Sw with the exact dielectric transmission (DL-306)
				const RandomWalkSSSParams* pRW = vertex.pMaterial->GetRandomWalkSSSParams();
				if( pRW ) {
					const Scalar cosTheta = Vector3Ops::Dot( wi, vertex.normal );
					if( cosTheta <= NEARZERO ) {
						return RISEPel( 0, 0, 0 );
					}
					// DL-49: relative to the vertex's exterior index
					// (`mediumIOR`, the IOR-stack top at the exit hit).
					const Scalar eta = BSSRDFSampling::RelativeBoundaryIOR(
						pRW->ior, ( vertex.mediumIOR > 0.0 ) ? vertex.mediumIOR : 1.0 );
					const Scalar FtEntry = BSSRDFSampling::BoundaryTransmission( cosTheta, pRW->ior,
						( vertex.mediumIOR > 0.0 ) ? vertex.mediumIOR : 1.0 );
					const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, eta );
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
			Ray evalRay( vertex.position, vertex.isLightSubpathVertex ? -wi : -wo );
			RayIntersectionGeometric ri( evalRay, nullRasterizerState );
			PopulateRIGFromVertex( vertex, ri );

			// Stateful BSDFs (e.g. translucent_material) query side via the
			// reconstructed IORStack (critical on light subpaths where evalRay is
			// reversed); other BSDFs ignore it via IBSDF::valueStateful default.
			IORStack vertexStack( 1.0 );
			BuildVertexIORStack( vertex, vertexStack );

			const Vector3& evalDir = vertex.isLightSubpathVertex ? wo : wi;
			// DL-472 (1): a light-subpath vertex is an IMPORTANCE query.
			const BSDFImportanceScope transportMode( vertex.isLightSubpathVertex );
			return pBSDF->valueStateful( evalDir, ri, &vertexStack );
		}

		/// DL-481: `EvalBSDFAtVertex`'s surface value split by the lobe
		/// label the material's SPF stamps (IBSDF::valueByScatterType),
		/// for a connection / merge endpoint under a per-type bounce cap.
		/// Same record, stack and transport mode as `EvalBSDFAtVertex`.
		/// False (out all zero) unless the vertex is a plain surface vertex
		/// whose material declares the split (IMaterial::HasConnectionTypeSplit).
		inline bool EvalBSDFByTypeAtVertex(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			RISEPel out[5]
			)
		{
			for( int k = 0; k < 5; k++ ) out[k] = RISEPel( 0, 0, 0 );
			if( vertex.type != BDPTVertex::SURFACE || vertex.isBSSRDFEntry || !vertex.pMaterial ||
				!vertex.pMaterial->HasConnectionTypeSplit() ) {
				return false;
			}
			const IBSDF* pBSDF = vertex.pMaterial->GetBSDF();
			if( !pBSDF ) {
				return false;
			}
			Ray evalRay( vertex.position, vertex.isLightSubpathVertex ? -wi : -wo );
			RayIntersectionGeometric ri( evalRay, nullRasterizerState );
			PopulateRIGFromVertex( vertex, ri );
			IORStack vertexStack( 1.0 );
			BuildVertexIORStack( vertex, vertexStack );
			const Vector3& evalDir = vertex.isLightSubpathVertex ? wo : wi;
			const BSDFImportanceScope transportMode( vertex.isLightSubpathVertex );
			return pBSDF->valueByScatterType( evalDir, ri, &vertexStack, out );
		}

		/// Spectral twin of `EvalBSDFByTypeAtVertex`.
		inline bool EvalBSDFByTypeAtVertexNM(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const Scalar nm,
			Scalar out[5]
			)
		{
			for( int k = 0; k < 5; k++ ) out[k] = 0;
			if( vertex.type != BDPTVertex::SURFACE || vertex.isBSSRDFEntry || !vertex.pMaterial ||
				!vertex.pMaterial->HasConnectionTypeSplit() ) {
				return false;
			}
			const IBSDF* pBSDF = vertex.pMaterial->GetBSDF();
			if( !pBSDF ) {
				return false;
			}
			Ray evalRay( vertex.position, vertex.isLightSubpathVertex ? -wi : -wo );
			RayIntersectionGeometric ri( evalRay, nullRasterizerState );
			PopulateRIGFromVertex( vertex, ri );
			IORStack vertexStack( 1.0 );
			BuildVertexIORStack( vertex, vertexStack );
			const Vector3& evalDir = vertex.isLightSubpathVertex ? wo : wi;
			const BSDFImportanceScope transportMode( vertex.isLightSubpathVertex );
			return pBSDF->valueByScatterTypeNM( evalDir, ri, nm, &vertexStack, out );
		}

		//////////////////////////////////////////////////////////////////////
		// VertexPdfContext
		//
		// Lazy per-vertex cache of RayIntersectionGeometric + IORStack shared
		// across forward/reverse EvalPdfAtVertex{,NM} queries on the same
		// vertex. `RecordFor(wi)` re-aims `m_ri.ray` per query. Holds a
		// reference to `vertex`; must not outlive vector reallocation.
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

			inline RayIntersectionGeometric& RecordFor( const Vector3& wi )
			{
				const Vector3 inDir = -wi;
				m_ri.ray = Ray( Point3Ops::mkPoint3( m_vertex.position, -(inDir * m_vertex.scatterIncomingDistance) ), inDir );
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

		/// Wavelength-independent short-circuit PDFs (medium, null material,
		/// BSSRDF entry) that need no reconstructed record.
		/// \return true if `outPdf` was set; false to proceed to SPF query.
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

					// DL-49: relative index, as in the RGB twin above.
					const Scalar FtEntry = pProfile->FresnelTransmissionNM( cosTheta, rig, nm );
					const Scalar eta = BSSRDFSampling::RelativeBoundaryIOR(
						pProfile->GetIORNM( rig, nm ), BSSRDFSampling::ExteriorIOR( rig ) );
					return BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, eta );
				}

				// Random-walk SSS: Sw with the exact dielectric transmission (DL-306)
				const RandomWalkSSSParams* pRW = vertex.pMaterial->GetRandomWalkSSSParams();
				RandomWalkSSSParams rwParamsNM;
				if( vertex.pMaterial->GetRandomWalkSSSParamsNM( nm, rwParamsNM ) ) {
					pRW = &rwParamsNM;
				}
				if( pRW ) {
					const Scalar cosTheta = Vector3Ops::Dot( wi, vertex.normal );
					if( cosTheta <= NEARZERO ) {
						return 0;
					}
					// DL-49: relative index, as in the RGB twin above.
					const Scalar eta = BSSRDFSampling::RelativeBoundaryIOR(
						pRW->ior, ( vertex.mediumIOR > 0.0 ) ? vertex.mediumIOR : 1.0 );
					const Scalar FtEntry = BSSRDFSampling::BoundaryTransmission( cosTheta, pRW->ior,
						( vertex.mediumIOR > 0.0 ) ? vertex.mediumIOR : 1.0 );
					return BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, eta );
				}

				return 0;
			}

			const IBSDF* pBSDF = vertex.pMaterial->GetBSDF();
			if( !pBSDF ) {
				return 0;
			}

			Ray evalRay( vertex.position, vertex.isLightSubpathVertex ? -wi : -wo );
			RayIntersectionGeometric ri( evalRay, nullRasterizerState );
			PopulateRIGFromVertex( vertex, ri );

			// DL-157, spectral twin of the RGB path above.
			IORStack vertexStack( 1.0 );
			BuildVertexIORStack( vertex, vertexStack );

			const Vector3& evalDir = vertex.isLightSubpathVertex ? wo : wi;
			const BSDFImportanceScope transportMode( vertex.isLightSubpathVertex );
			return pBSDF->valueStatefulNM( evalDir, ri, nm, &vertexStack );
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
			const RayIntersectionGeometric& ri,
			//! Live IORStack for stateful BSDFs (null forwards to `value`).
			const IORStack* pIORStack = 0
			)
		{
			return pBRDF->valueStateful( wi, ri, pIORStack );
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
			const Scalar nm,
			//! DL-157 P1 -- see the RGB twin.
			const IORStack* pIORStack = 0
			)
		{
			return pBRDF->valueStatefulNM( wi, ri, nm, pIORStack );
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
