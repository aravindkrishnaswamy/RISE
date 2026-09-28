//////////////////////////////////////////////////////////////////////
//
//  BSSRDFSampling.h - Shared BSSRDF importance sampling utility
//
//  Implements the disk projection method (Christensen & Burley 2015)
//  for sampling entry points on translucent surfaces.  The algorithm
//  is used by both the bidirectional path tracer (BDPTIntegrator) and
//  the unidirectional path tracer (PathTracingIntegrator).
//
//  ALGORITHM OVERVIEW:
//    Given a ray exit point on a material with a diffusion profile,
//    the sampler finds a nearby entry point on the same surface and
//    computes the BSSRDF importance sampling weight.
//
//    Steps:
//    1. Choose a spectral channel uniformly (R, G, B)
//    2. Choose a projection axis:
//       normal (50%), tangent (25%), bitangent (25%)
//    3. Sample radius r from the profile CDF for the channel
//    4. Sample angle phi uniformly on [0, 2pi)
//    5. Compute probe origin offset in the perpendicular plane
//    6. Cast a single finite chord through the object along the axis,
//       starting before and passing through the projection plane
//       (DL-52: NOT two half-lines starting AT the plane -- that
//       skips a coplanar near surface, see BSSRDFSampling.cpp).
//       DL-71/DL-75: any hit -- near OR far side of the projection
//       plane, position is irrelevant -- whose underlying geometry
//       orients its reported normal to face the incoming ray
//       (RayIntersectionGeometric::bGeomNormalOrientedToRay) has that
//       normal (and its shading-normal partner, oriented into the
//       same hemisphere -- P2-A) unconditionally recovered via
//       `oriented ? -raw : raw`, EXCEPT when the orientation is
//       fabricated rather than a recovered winding normal
//       (HairGeometry; RayIntersectionGeometric::bGeomNormalRayDerived)
//       -- see the probe loop in BSSRDFSampling.cpp for the full rule.
//       DL-96 (checked, not changed here): the entry ADMISSION GATE at
//       the call site treats an open sheet's two faces symmetrically
//       (RayIntersectionGeometric::bOpenSheet), but this probe's own
//       per-hit recovery stays unconditional -- see the long comment
//       at its call site in BSSRDFSampling.cpp for why.
//    7. If hit: evaluate Rd(r_actual), compute multi-axis PDF
//    8. Generate cosine-weighted scattered ray from entry normal
//    9. Compute Fresnel transmission and Sw normalization
//
//  FACTORIZATION:
//    The BSSRDF is factored as (Christensen & Burley 2015):
//      S(wo, xo, wi, xi) = C * Ft(wo) * Rd(||xo - xi||) * Ft(wi)
//    where C = 1 / (c * PI), c = 20*(1-F0) / 21, and
//    F0 = ((eta-1)/(eta+1))^2, with eta the RELATIVE boundary index
//    n_interior / n_exterior (DL-49; see RelativeBoundaryIOR below --
//    and SchlickTransmissionNormalization for the eta < 1 form).  SampleEntryPoint returns two weights:
//      weight        = Rd * Ft(exit) * Ft(cosine_dir) / (c * pdfSurface)
//                      (Sw * cosine / cosinePdf = Ft/c for this continuation)
//      weightSpatial = Rd * Ft(exit) / pdfSurface
//                      (for NEE/connections, which evaluate Sw independently)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 30, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BSSRDF_SAMPLING_
#define BSSRDF_SAMPLING_

#include "Math3D/Math3D.h"
#include "Math3D/Constants.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Intersection/RayIntersection.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "ISampler.h"
#include "Optics.h"

namespace RISE
{
	namespace BSSRDFSampling
	{
		/// Small epsilon for ray offsets to avoid self-intersection.
		constexpr Scalar BSSRDF_RAY_EPSILON = 1e-6;

		/// Result of BSSRDF importance sampling at a surface vertex
		struct SampleResult
		{
			Point3				entryPoint;		///< Entry point on the surface
			Vector3				entryNormal;	///< Shading normal at entry point — drives the cosine-sampled
												///< continuation frame and Sw Fresnel angular dependence.
			Vector3				entryGeomNormal;///< Geometric normal at entry point — drives the disk-to-surface
												///< area Jacobian and the rebuilt entry front/back-face gate.
												///< Independent of smooth interpolation and shading modifiers,
												///< including normal maps on analytical primitives.
			OrthonormalBasis3D	entryONB;		///< ONB at entry point
			SurfaceDerivativesInfo derivatives;	///< Surface derivatives at entry point (DL-22)
			SurfaceSignalInfo	signals;		///< Surface signals at entry point (DL-22)
			TextureFootprint	txFootprint;	///< Texture footprint at entry point (DL-22)
			Point2				ptCoord;		///< Texture coordinates at entry point (DL-22)
			Point2				ptCoord1;		///< Secondary texture coordinates (DL-22)
			bool				bHasTexCoord1;	///< True if secondary texture coordinates exist (DL-22)
			Point3				ptObjIntersec;	///< Object-space intersection point (DL-22)
			RISEPel				vColor;			///< Vertex color at entry point (DL-22)
			bool				bHasVertexColor;///< True if vertex color exists (DL-22)
			Ray					scatteredRay;	///< Cosine-weighted ray from entry point
			RISEPel				weight;			///< Full BSSRDF weight: Rd * Ft(exit) * Ft(entry) / (c * pdfSurface)
			RISEPel				weightSpatial;	///< Spatial-only weight: Rd * Ft(exit) / pdfSurface (no entry Sw)
			Scalar				weightNM;		///< Scalar weight for spectral path (full)
			Scalar				weightSpatialNM;///< Scalar spatial-only weight for spectral path
			Scalar				cosinePdf;		///< PDF of the cosine-weighted direction
			Scalar				pdfSurface;		///< Spatial sampling PDF in area measure
			bool				valid;			///< True if sampling succeeded

			SampleResult() :
			bHasTexCoord1( false ),
			vColor( RISEPel(0,0,0) ),
			bHasVertexColor( false ),
			weight( RISEPel(0,0,0) ), weightSpatial( RISEPel(0,0,0) ),
			weightNM(0), weightSpatialNM(0),
			cosinePdf(0), pdfSurface(0), valid(false) {}
		};

		//////////////////////////////////////////////////////////////
		// DL-49: the SSS boundary is an INTERFACE between two media.
		//
		// Every boundary quantity below (Fresnel transmission, its Sw
		// normalization, the random walk's Snell refraction) is a
		// function of the RELATIVE index eta = n_interior / n_exterior,
		// never of the material's absolute index against air.  The
		// exterior index is the medium the ray arrived through --
		// `RayIntersectionGeometric::ambientIOR`, which PT, BDPT, VCM,
		// MLT and the ray caster stamp from `IORStack::top()` at every
		// hit (G6) and which `PathVertexEval::PopulateRIGFromVertex`
		// replays from `BDPTVertex::mediumIOR`.  It is the SAME source
		// `SubSurfaceScatteringSPF`'s surface reflection has always
		// read (`ior_stack.top()`), so the two halves of one boundary
		// event now describe one interface.  A stackless caller (a
		// freshly constructed record, a photon tracer without a stack)
		// sees the field's default 1.0: air, exactly as before.
		//
		// In air (exterior 1.0) `interior / 1.0 == interior` exactly and
		// every helper below takes its pre-DL-49 branch, so an in-air
		// SSS evaluation is bit-identical to the pre-DL-49 code.
		//
		// DL-04's complete-event convention is untouched: Sw is still
		// normalized to a unit cosine-hemisphere integral on the
		// exterior side, so no unmatched eta^2 enters at any exterior
		// index (the two basic-radiance factors still telescope to 1).
		//////////////////////////////////////////////////////////////

		/// The exterior (incident-medium) index carried by a hit record.
		/// Non-positive or non-finite values fall back to air.
		inline Scalar ExteriorIOR( const RayIntersectionGeometric& ri )
		{
			const Scalar n = ri.ambientIOR;
			return ( n > 0 && n < RISE_INFINITY ) ? n : Scalar( 1.0 );
		}

		/// Relative boundary index n_interior / n_exterior.
		inline Scalar RelativeBoundaryIOR( const Scalar interiorIOR, const Scalar exteriorIOR )
		{
			return ( exteriorIOR > 0 && exteriorIOR < RISE_INFINITY )
				? interiorIOR / exteriorIOR : interiorIOR;
		}

		/// The cosine the Schlick law is evaluated at, for an EXTERIOR-side
		/// direction with cosine mu (in [0,1]) against the surface normal.
		///
		/// eta >= 1 (exterior no denser than the interior, including every
		/// in-air material): the exterior cosine itself -- the pre-DL-49
		/// law, bit-for-bit.
		///
		/// eta < 1 (a denser exterior, e.g. a 1.33 SSS body inside 1.5
		/// glass): Schlick's approximation must be evaluated at the cosine
		/// on the RARER side (the transmitted cosine), and directions past
		/// the critical angle are totally reflected.  Returns false for
		/// TIR (Ft == 0).  Only reachable once DL-49 made eta relative:
		/// pre-DL-49 eta was always an absolute index against air.
		inline bool SchlickBoundaryCosine( const Scalar mu, const Scalar eta, Scalar& cosSchlick )
		{
			if( eta >= 1.0 ) {
				cosSchlick = mu;
				return true;
			}
			return Optics::CalculateRefractedCosine( mu, 1.0, eta, cosSchlick );
		}

		/// Schlick transmission Ft = 1 - F at an exterior-side cosine mu,
		/// in the random-walk / entry-adapter arithmetic form
		/// (`pow(1-mu, 5)`), for the RELATIVE index eta.
		inline Scalar RandomWalkSchlickTransmission( const Scalar mu, const Scalar eta )
		{
			Scalar c;
			if( !SchlickBoundaryCosine( mu, eta, c ) ) {
				return 0;
			}
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - c, 5.0 );
			return 1.0 - F;
		}

		/// Normalization for the in-tree profiles' Schlick transmission law,
		/// for the RELATIVE index eta (DL-49).
		///
		/// eta >= 1: Ft(mu) = (1-F0) * (1-(1-mu)^5), so
		///   c = 2 * integral_0^1 Ft(mu)*mu dmu = 20*(1-F0)/21.
		/// eta < 1: Ft(mu) = (1-F0) * (1-(1-t)^5) with t the transmitted
		///   cosine and Ft = 0 past the critical angle.  Snell gives
		///   mu^2 = 1 - eta^2 (1 - t^2), so mu dmu = eta^2 t dt over
		///   t in [0,1], and c = eta^2 * 20*(1-F0)/21 exactly (the two
		///   branches agree at eta = 1).
		/// Consequently Sw = Ft/(c*PI) has unit exterior cosine-hemisphere
		/// integral at every relative index.
		inline Scalar SchlickTransmissionNormalization( const Scalar eta )
		{
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar c = (20.0 / 21.0) * (1.0 - F0);
			return ( eta >= 1.0 ) ? c : eta * eta * c;
		}

		/// Computes the Sw directional scattering factor at a BSSRDF
		/// entry point, given the Fresnel transmission at that point.
		///
		///   Sw(wi) = Ft(cos_theta_i) / (c * PI)
		///
		/// Used by PathVertexEval::EvalBSDFAtVertex for BSSRDF entry
		/// connections and by BSSRDFEntryBSDF in the unidirectional PT.
		///
		/// \return Sw value (scalar, achromatic)
		inline Scalar EvaluateSwWithFresnel(
			const Scalar FtEntry,					///< [in] Fresnel transmission at entry point
			const Scalar eta						///< [in] RELATIVE index n_interior / n_exterior (DL-49)
			)
		{
			const Scalar c = SchlickTransmissionNormalization( eta );

			if( c > 1e-20 ) {
				return FtEntry / (c * PI);
			}
			return 0;
		}

		/// Attempts BSSRDF importance sampling at a front-face hit
		/// on a material with a diffusion profile.
		///
		/// Uses the disk projection method (Christensen & Burley 2015).
		/// The probe ray is cast against the specific object (pObject),
		/// not the whole scene, to ensure the entry point is on the
		/// same translucent surface.
		///
		/// \return A SampleResult with valid=true on success
		SampleResult SampleEntryPoint(
			const RayIntersectionGeometric& ri,		///< [in] Exit point intersection
			const IObject* pObject,					///< [in] Object to cast probe rays against
			const IMaterial* pMaterial,				///< [in] Material with diffusion profile
			ISampler& sampler,						///< [in] Sampler for stochastic decisions
			const Scalar nm							///< [in] Wavelength for NM path (0 = RGB)
			);
	}
}

#endif
