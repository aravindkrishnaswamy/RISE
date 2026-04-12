//////////////////////////////////////////////////////////////////////
//
//  BSSRDFSampling.h - Shared BSSRDF importance sampling utility
//
//  Implements the disk projection method (Christensen & Burley 2015)
//  for sampling entry points on translucent surfaces.  The algorithm
//  is used by both the bidirectional path tracer (BDPTIntegrator) and
//  the unidirectional path tracer (PathTracingShaderOp).
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
//    6. Cast a probe ray along +-axis through the object
//    7. If hit: evaluate Rd(r_actual), compute multi-axis PDF
//    8. Generate cosine-weighted scattered ray from entry normal
//    9. Compute Fresnel transmission and Sw normalization
//
//  FACTORIZATION:
//    The BSSRDF is factored as (Christensen & Burley 2015):
//      S(wo, xo, wi, xi) = C * Ft(wo) * Rd(||xo - xi||) * Ft(wi)
//    where C = 1 / (c * PI), c = (41 - 20*F0) / 42, and
//    F0 = ((eta-1)/(eta+1))^2.  SampleEntryPoint returns two weights:
//      weight        = Rd * Ft(exit) * Sw(cosine_dir) / pdfSurface
//                      (for continuation along the cosine-sampled ray)
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
			Vector3				entryNormal;	///< Surface normal at entry point
			OrthonormalBasis3D	entryONB;		///< ONB at entry point
			Ray					scatteredRay;	///< Cosine-weighted ray from entry point
			RISEPel				weight;			///< Full BSSRDF weight: Rd * Ft(exit) * Ft(entry) / (c * pdfSurface)
			RISEPel				weightSpatial;	///< Spatial-only weight: Rd * Ft(exit) / pdfSurface (no entry Sw)
			Scalar				weightNM;		///< Scalar weight for spectral path (full)
			Scalar				weightSpatialNM;///< Scalar spatial-only weight for spectral path
			Scalar				cosinePdf;		///< PDF of the cosine-weighted direction
			Scalar				pdfSurface;		///< Spatial sampling PDF in area measure
			bool				valid;			///< True if sampling succeeded

			SampleResult() :
			weight( RISEPel(0,0,0) ), weightSpatial( RISEPel(0,0,0) ),
			weightNM(0), weightSpatialNM(0),
			cosinePdf(0), pdfSurface(0), valid(false) {}
		};

		/// Computes the Sw directional scattering factor at a BSSRDF
		/// entry point, given the Fresnel transmission at that point.
		///
		///   Sw(wi) = Ft(cos_theta_i) / (c * PI)
		///
		/// Used by BDPTIntegrator::EvalBSDFAtVertex for BSSRDF entry
		/// connections and by BSSRDFEntryBSDF in the unidirectional PT.
		///
		/// \return Sw value (scalar, achromatic)
		inline Scalar EvaluateSwWithFresnel(
			const Scalar FtEntry,					///< [in] Fresnel transmission at entry point
			const Scalar eta						///< [in] Index of refraction (eta_t / eta_i)
			)
		{
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar c = (41.0 - 20.0 * F0) / 42.0;

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
