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

					RayIntersectionGeometric rig(
						Ray( vertex.position, -wi ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = vertex.position;
					rig.vNormal = vertex.normal;
					rig.onb = vertex.onb;

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
			ri.bHit = true;
			ri.ptIntersection = vertex.position;
			ri.vNormal = vertex.normal;
			ri.onb = vertex.onb;

			return pBSDF->value( wi, ri );
		}

		//////////////////////////////////////////////////////////////////////
		// RGB PDF Evaluation
		//////////////////////////////////////////////////////////////////////

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
			// Medium scatter vertex: phase function sampling PDF.
			if( vertex.type == BDPTVertex::MEDIUM ) {
				if( !vertex.pPhaseFunc ) {
					return 0;
				}
				return vertex.pPhaseFunc->Pdf( -wi, wo );
			}

			if( !vertex.pMaterial ) {
				return 0;
			}

			// BSSRDF entry vertex: cosine-weighted hemisphere PDF.
			if( vertex.isBSSRDFEntry ) {
				const Scalar cosTheta = fabs( Vector3Ops::Dot( wo, vertex.normal ) );
				return cosTheta * INV_PI;
			}

			const ISPF* pSPF = vertex.pMaterial->GetSPF();
			if( !pSPF ) {
				return 0;
			}

			// Negate wi to get toward-surface direction for ri.ray.Dir()
			Ray evalRay( vertex.position, -wi );
			RayIntersectionGeometric ri( evalRay, nullRasterizerState );
			ri.bHit = true;
			ri.ptIntersection = vertex.position;
			ri.vNormal = vertex.normal;
			ri.onb = vertex.onb;

			IORStack stack( 1.0 );
			BuildVertexIORStack( vertex, stack );
			return pSPF->Pdf( ri, wo, stack );
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

					RayIntersectionGeometric rig(
						Ray( vertex.position, -wi ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = vertex.position;
					rig.vNormal = vertex.normal;
					rig.onb = vertex.onb;

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
			ri.bHit = true;
			ri.ptIntersection = vertex.position;
			ri.vNormal = vertex.normal;
			ri.onb = vertex.onb;

			return pBSDF->valueNM( wi, ri, nm );
		}

		//////////////////////////////////////////////////////////////////////
		// Spectral (NM) PDF Evaluation
		//////////////////////////////////////////////////////////////////////

		/// Evaluates the SPF sampling PDF at a vertex for a single wavelength.
		inline Scalar EvalPdfAtVertexNM(
			const BDPTVertex& vertex,
			const Vector3& wi,
			const Vector3& wo,
			const Scalar nm
			)
		{
			// Medium scatter vertex: phase function PDF
			if( vertex.type == BDPTVertex::MEDIUM ) {
				if( !vertex.pPhaseFunc ) {
					return 0;
				}
				return vertex.pPhaseFunc->Pdf( -wi, wo );
			}

			if( !vertex.pMaterial ) {
				return 0;
			}

			// BSSRDF entry vertex: cosine hemisphere PDF
			if( vertex.isBSSRDFEntry ) {
				const Scalar cosTheta = fabs( Vector3Ops::Dot( wo, vertex.normal ) );
				return cosTheta * INV_PI;
			}

			const ISPF* pSPF = vertex.pMaterial->GetSPF();
			if( !pSPF ) {
				return 0;
			}

			Ray evalRay( vertex.position, -wi );
			RayIntersectionGeometric ri( evalRay, nullRasterizerState );
			ri.bHit = true;
			ri.ptIntersection = vertex.position;
			ri.vNormal = vertex.normal;
			ri.onb = vertex.onb;

			IORStack stack( 1.0 );
			BuildVertexIORStack( vertex, stack );
			return pSPF->PdfNM( ri, wo, nm, stack );
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
