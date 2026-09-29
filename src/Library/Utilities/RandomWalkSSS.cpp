//////////////////////////////////////////////////////////////////////
//
//  RandomWalkSSS.cpp - Implementation of random-walk subsurface
//    scattering sampler.
//
//  See RandomWalkSSS.h for algorithm overview.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 7, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RandomWalkSSS.h"
#include "FiniteMath.h"
#include "Optics.h"
#include "GeometricUtilities.h"
#include "../Materials/HenyeyGreensteinPhaseFunction.h"
#include "../Intersection/RayIntersection.h"
#include "../Intersection/RayIntersectionGeometric.h"

using namespace RISE;

BSSRDFSampling::SampleResult RandomWalkSSS::SampleExit(
	const RayIntersectionGeometric& ri,
	const IObject* pObject,
	const RISEPel& sigma_a,
	const RISEPel& sigma_s,
	const RISEPel& sigma_t,
	const Scalar g,
	const Scalar ior,
	const unsigned int maxBounces,
	ISampler& sampler,
	const Scalar nm,
	const Scalar maxDepth
	)
{
	BSSRDFSampling::SampleResult result;

	if( !pObject ) {
		return result;
	}

	// Ensure valid coefficients
	const Scalar sigma_t_max = ColorMath::MaxValue( sigma_t );
	if( sigma_t_max < 1e-20 ) {
		return result;
	}

	// Preserve the actual RGB proposal rates for both sampling and density
	// evaluation. Physical extinction stays separate in each numerator.
	RISEPel proposalRates = sigma_t;
	bool rgbUsesFallback = false;
	for( int c = 0; c < 3; ++c ) {
		if( proposalRates[c] < 1e-20 ) {
			proposalRates[c] = sigma_t_max;
			rgbUsesFallback = true;
		}
	}

	// Luminance-derived scalar coefficients for the NM (spectral) path.
	// The material stores RGB coefficients; for single-wavelength tracing
	// we collapse to a scalar using Rec. 709 luminance weights.  This
	// defines physical collision and survival transmittance. A fallback
	// proposal may use another rate, which event weights must divide out.
	const Scalar sigma_a_nm = 0.2126 * sigma_a[0] + 0.7152 * sigma_a[1] + 0.0722 * sigma_a[2];
	const Scalar sigma_s_nm = 0.2126 * sigma_s[0] + 0.7152 * sigma_s[1] + 0.0722 * sigma_s[2];
	const Scalar sigma_t_nm = sigma_a_nm + sigma_s_nm;

	//
	// Step 1: Refract into the surface
	//
	const Vector3& surfNormal = ri.vNormal;
	Vector3 dir = Vector3Ops::Normalize( ri.ray.Dir() );

	// Ensure normal points outward (toward the incoming ray).  Side-of-
	// surface decision uses the GEOMETRIC normal — entering vs exiting
	// a real medium boundary is a topology question (PBRT 4e §9.5).
	// Bump-perturbed shading normals can flip the cosIncoming sign and
	// drive the wrong refract/TIR branch on bumpy SSS skin.
	const Scalar cosIncomingGeom = Vector3Ops::Dot( ri.vGeomNormal, -dir );
	Vector3 outwardNormal = ( cosIncomingGeom > 0 ) ? surfNormal : -surfNormal;

	// DL-49: the boundary is an interface between the medium the ray
	// arrived through and this material -- NOT air and this material.
	// `ri.ambientIOR` is that exterior index (stamped from the IOR stack
	// by every integrator; 1.0 for a stackless record).  The exit below
	// leaves into the SAME exterior: the walk stays on one object, and the
	// caller continues the exit ray with the unchanged IOR stack.  In air
	// every call below is the pre-DL-49 call bit-for-bit.
	const Scalar nExterior = BSSRDFSampling::ExteriorIOR( ri );
	const Scalar etaRel = BSSRDFSampling::RelativeBoundaryIOR( ior, nExterior );

	// Snell's law refraction: exterior (nExterior) -> medium (ior).
	// Direction generation uses the SHADING-frame outward normal
	// (BSDF-coupled).
	Vector3 refractedDir = dir;
	if( !Optics::CalculateRefractedRay( outwardNormal, nExterior, ior, refractedDir ) )
	{
		// Total internal reflection at entry — no walk possible
		return result;
	}

	//
	// Step 2: Initialize walk position inside the mesh
	//
	Point3 pos = ri.ptIntersection;
	dir = refractedDir;

	// Offset inward to avoid self-intersection
	pos = Point3Ops::mkPoint3( pos, dir * BSSRDFSampling::BSSRDF_RAY_EPSILON );

	// For depth-limited walks (e.g. BioSpec skin thickness),
	// track depth below the entry surface.  Depth is measured
	// as the projection onto the inward normal from the entry
	// point.  Walks that exceed maxDepth are terminated.
	const Vector3 inwardNormal = -outwardNormal;
	const Point3 entryPos = ri.ptIntersection;

	//
	// Step 3: Random walk loop
	//
	RISEPel throughput( 1.0, 1.0, 1.0 );
	Scalar throughputNM = 1.0;

	for( unsigned int bounce = 0; bounce < maxBounces; bounce++ )
	{
		//
		// 3a. Trace ray to find exit distance (back-face hit)
		//
		Ray walkRay( pos, dir );
		RayIntersection exitRI( walkRay, nullRasterizerState );
		pObject->IntersectRay( exitRI, RISE_INFINITY, false, true, false );

		if( !exitRI.geometric.bHit )
		{
			// Ray escaped without hitting a back face.
			// This can happen with open meshes or numerical issues.
			// Try front faces as fallback — the walk may have crossed
			// a thin shell and needs to find the other side.
			RayIntersection fallbackRI( walkRay, nullRasterizerState );
			pObject->IntersectRay( fallbackRI, RISE_INFINITY, true, false, false );

			if( !fallbackRI.geometric.bHit ) {
				// Truly escaped — terminate walk
				return result;
			}

			// Use the front-face hit as exit
			exitRI = fallbackRI;
		}

		if( exitRI.pModifier ) {
			exitRI.pModifier->Modify( exitRI.geometric );
		}

		const Scalar exitDist = exitRI.geometric.range;

		//
		// 3b. Sample spectral channel for distance sampling
		//
		// Channel selection: uniform over RGB channels (or single
		// channel for spectral mode).  The distance is sampled from
		// the selected channel's exponential distribution.
		//
		int ch;
		Scalar sigma_t_ch;

		if( nm > 0 )
		{
			// Spectral mode: single channel, use luminance-derived
			// extinction for consistent distance sampling.
			ch = 0;
			sigma_t_ch = (sigma_t_nm > 1e-20) ? sigma_t_nm : sigma_t_max;
		}
		else
		{
			// RGB mode: uniform channel selection
			ch = static_cast<int>( sampler.Get1D() * 3.0 );
			if( ch >= 3 ) ch = 2;
			sigma_t_ch = proposalRates[ch];
		}

		//
		// 3c. Sample free-flight distance from Beer-Lambert
		//
		const Scalar xi = sampler.Get1D();
		const Scalar t = -log( fmax( 1e-20, 1.0 - xi ) ) / sigma_t_ch;

		//
		// 3d. Scatter inside or exit?
		//
		if( t < exitDist )
		{
			// --- Scatter event inside the mesh ---

			// Advance position
			pos = Point3Ops::mkPoint3( pos, dir * t );

			// Depth limit: terminate if the walk has gone
			// deeper than maxDepth below the entry surface.
			if( maxDepth > 0 )
			{
				const Vector3 offset = Vector3Ops::mkVector3(
					entryPos, pos );
				const Scalar depth = Vector3Ops::Dot( offset, inwardNormal );
				if( depth > maxDepth ) {
					return result;  // absorbed
				}
			}

			// Update throughput using mixture PDF formulation.
			//
			// The distance t was sampled from q[ch] * exp(-q[ch]*t),
			// where q is the effective proposal rate after fallback.
			// The mixture PDF over all channels (uniform 1/3 selection) is:
			//   p(t) = (1/3) * sum_c q[c] * exp(-q[c] * t)
			//
			// The scatter contribution for channel c at distance t is:
			//   f[c] = sigma_s[c] * exp(-sigma_t[c] * t)
			//
			// The IS weight: f[c] / p(t) = sigma_s[c] * Tr[c] / pdfMixture
			//
			// For isotropic media (all channels equal), this simplifies to:
			//   sigma_s / sigma_t = albedo (per step)
			//
			if( nm > 0 )
			{
				// Physical collision density divided by the sampled exponential
				// density. This is albedo when the rates match. The initial
				// maximum-extinction gate guarantees a positive proposal rate,
				// including the exact fallback threshold.
				throughputNM *= (sigma_s_nm / sigma_t_ch) *
					exp( (sigma_t_ch - sigma_t_nm) * t );
			}
			else
			{
				// Compute per-channel transmittance at distance t
				const Scalar Tr0 = exp( -sigma_t[0] * t );
				const Scalar Tr1 = exp( -sigma_t[1] * t );
				const Scalar Tr2 = exp( -sigma_t[2] * t );

				// Use the actual rate mixture. Reuse physical transmittance
				// in the ordinary case, where proposal and physical rates match.
				const Scalar pdfMixture = rgbUsesFallback
					? (proposalRates[0] * exp( -proposalRates[0] * t )
						+ proposalRates[1] * exp( -proposalRates[1] * t )
						+ proposalRates[2] * exp( -proposalRates[2] * t )) / 3.0
					: (sigma_t[0] * Tr0 + sigma_t[1] * Tr1 + sigma_t[2] * Tr2) / 3.0;

				// A density carries inverse-distance units: its magnitude cannot
				// decide whether the normalized collision weight is significant.
				// Reject only invalid densities before dividing physical density.
				if( !IsFiniteDouble( pdfMixture ) || pdfMixture <= 0.0 ) {
					return result;
				}

				// Weight: sigma_s[c] * Tr[c] / pdfMixture
				throughput[0] *= sigma_s[0] * Tr0 / pdfMixture;
				throughput[1] *= sigma_s[1] * Tr1 / pdfMixture;
				throughput[2] *= sigma_s[2] * Tr2 / pdfMixture;
			}

			// Check for degenerate throughput
			if( nm > 0 )
			{
				if( throughputNM < 1e-20 ) {
					return result;
				}
			}
			else
			{
				if( ColorMath::MaxValue( throughput ) < 1e-20 ) {
					return result;
				}
			}

			// Sample new direction from HG phase function
			dir = HenyeyGreensteinPhaseFunction::SampleWithG( dir, sampler, g );
		}
		else
		{
			// --- Exit event: walk reaches the surface ---

			// Divide physical transmittance by proposal survival probability.
			// For RGB this is the mean of exp(-q[c]*exitDist) over the
			// effective channel rates, including any fallback substitutions.
			if( nm > 0 )
			{
				// Divide physical transmittance by the probability of this
				// sampled survival event. Normally sigma_t_ch == sigma_t_nm,
				// so the conditional weight is one. Retain the ratio when
				// the tiny-extinction fallback uses a different proposal.
				throughputNM *= exp( (sigma_t_ch - sigma_t_nm) * exitDist );
			}
			else
			{
				const Scalar Tr0 = exp( -sigma_t[0] * exitDist );
				const Scalar Tr1 = exp( -sigma_t[1] * exitDist );
				const Scalar Tr2 = exp( -sigma_t[2] * exitDist );
				const Scalar pdfExit = rgbUsesFallback
					? (exp( -proposalRates[0] * exitDist )
						+ exp( -proposalRates[1] * exitDist )
						+ exp( -proposalRates[2] * exitDist )) / 3.0
					: (Tr0 + Tr1 + Tr2) / 3.0;

				if( pdfExit < 1e-20 ) {
					return result;
				}

				throughput[0] *= Tr0 / pdfExit;
				throughput[1] *= Tr1 / pdfExit;
				throughput[2] *= Tr2 / pdfExit;
			}

			// Move to the exit point
			const Point3 exitPoint = exitRI.geometric.ptIntersection;
			Vector3 exitNormal = exitRI.geometric.vNormal;
			Vector3 exitGeomNormal = exitRI.geometric.vGeomNormal;
			const Vector3 rawNormal = exitNormal;

			// Ensure exit normal points outward (away from interior).
			// Side-of-surface decision uses the GEOMETRIC normal — the
			// "is this hit's normal pointing inward or outward" test is
			// a topology question (PBRT 4e §9.5 / §10.1.1).  Flip both
			// shading and geometric in lock-step so downstream consumers
			// see a consistent outward-facing pair.
			if( Vector3Ops::Dot( exitGeomNormal, dir ) < 0 ) {
				exitNormal = -exitNormal;
				exitGeomNormal = -exitGeomNormal;
			}

			//
			// 3e. Fresnel at exit boundary
			//
			// Compute Fresnel reflectance at the exit boundary.
			// We are going from medium (ior) to the exterior (nExterior;
			// DL-49 -- was hardcoded air).
			Vector3 refractedOut = dir;
			if( !Optics::CalculateRefractedRay( -exitNormal, ior, nExterior, refractedOut ) )
			{
				// Total internal reflection — reflect and continue walk
				dir = Optics::CalculateReflectedRay( dir, exitNormal );
				pos = Point3Ops::mkPoint3( exitPoint,
					dir * BSSRDFSampling::BSSRDF_RAY_EPSILON );
				continue;
			}

			// Dielectric Fresnel reflectance at exit
			const Scalar F_exit = Optics::CalculateDielectricReflectance(
				-dir, refractedOut, -exitNormal, ior, nExterior );

			// Stochastic Fresnel: reflect with probability F, transmit with (1-F)
			if( sampler.Get1D() < F_exit )
			{
				// Fresnel reflection — bounce back inside
				dir = Optics::CalculateReflectedRay( dir, exitNormal );
				pos = Point3Ops::mkPoint3( exitPoint,
					dir * BSSRDFSampling::BSSRDF_RAY_EPSILON );
				continue;
			}

			// Transmitted through the exit boundary.
			// Weight includes transmission factor from stochastic
			// Fresnel: the 1/(1-F) from the coin flip cancels with
			// the (1-F) transmission factor, so no explicit Fresnel
			// weight is applied here.

			//
			// 3f. Generate cosine-weighted direction from exit normal
			//
			// NOTE: This always produces a diffuse (cosine-weighted)
			// exit direction, which is the standard approach for
			// random-walk SSS following Chiang & Burley 2016.  The
			// walk models MULTIPLE SCATTERING only — the ballistic /
			// single-scatter / refractive-transmission lobe is NOT
			// included here and should be modeled by a separate
			// dielectric BSDF layer at the material level.
			//
			// When sigma_s is very small or zero, the walk still
			// produces cosine-weighted exits (with Beer-Lambert
			// attenuation), which is nonphysical for that regime.
			// This is by design: low-scatter media should use a
			// glass/dielectric material, not the SSS walk.
			//
			OrthonormalBasis3D exitONB;
			exitONB.CreateFromW( exitNormal );

			const Scalar u1 = sampler.Get1D();
			const Scalar u2 = sampler.Get1D();
			const Scalar cosTheta = sqrt( u1 );
			const Scalar sinTheta = sqrt( 1.0 - u1 );
			const Scalar phiCosine = TWO_PI * u2;

			const Vector3 cosineDir = Vector3Ops::Normalize(
				exitONB.u() * (sinTheta * cos(phiCosine)) +
				exitONB.v() * (sinTheta * sin(phiCosine)) +
				exitONB.w() * cosTheta );

			//
			// 3g. Compute Sw and fill result
			//
			// Sw(wi) = Ft(cos_theta_i) / (c * PI)
			// For the cosine-weighted direction, compute entry Fresnel --
			// for the RELATIVE boundary index (DL-49), with the exact
			// dielectric law the SPF's surface reflection uses (DL-306).
			const Scalar c_norm = BSSRDFSampling::BoundaryTransmissionNormalization( etaRel );
			const Scalar FtEntry = BSSRDFSampling::BoundaryTransmission( cosTheta, etaRel );
			const Scalar SwFactor = (c_norm > 1e-20) ? FtEntry / c_norm : FtEntry;

			// IS weight for the cosine-sampled continuation direction.
			//
			// The true Sw is Ft / (c * PI).  The continuation direction
			// is sampled from cosine-weighted PDF = cos(theta) / PI.
			// In the rendering equation the IS weight is:
			//
			//   Sw * cos / PDF = [Ft / (c*PI)] * cos / [cos/PI]
			//                  = Ft / c
			//
			// The PI in Sw and the PI in the cosine PDF cancel, leaving
			// SwFactor = Ft / c as the correct IS weight per step.
			result.weight = throughput * SwFactor;
			result.weightSpatial = throughput;

			if( nm > 0 ) {
				result.weightNM = throughputNM * SwFactor;
				result.weightSpatialNM = throughputNM;
			} else {
				const Scalar tpScalar = 0.2126 * throughput[0] +
					0.7152 * throughput[1] + 0.0722 * throughput[2];
				result.weightNM = tpScalar * SwFactor;
				result.weightSpatialNM = tpScalar;
			}

			// Offset exit point along the surface normal so that
			// shadow rays, connection rays, and the continuation
			// ray all start above the originating surface.  Using
			// the normal offset (rather than advancing along the
			// ray direction) ensures clearance even for near-
			// grazing directions, and keeps the ray origin
			// consistent with the stored BDPT vertex position.
			result.entryPoint = Point3Ops::mkPoint3( exitPoint,
				exitNormal * BSSRDFSampling::BSSRDF_RAY_EPSILON );
			result.entryNormal = exitNormal;
			result.entryGeomNormal = exitGeomNormal;
			result.entryONB = exitONB;
			result.derivatives = exitRI.geometric.derivatives;
			result.signals = exitRI.geometric.signals;
			result.txFootprint = exitRI.geometric.txFootprint;
			result.ptCoord = exitRI.geometric.ptCoord;
			result.ptCoord1 = exitRI.geometric.ptCoord1;
			result.bHasTexCoord1 = exitRI.geometric.bHasTexCoord1;
			result.ptObjIntersec = exitRI.geometric.ptObjIntersec;
			result.vColor = exitRI.geometric.vColor;
			result.bHasVertexColor = exitRI.geometric.bHasVertexColor;

			// Cross-object signals triple (DL-22):
			result.signals.pScene = ri.signals.pScene;
			result.signals.pSelf = pObject;
			result.signals.ptWorld = result.entryPoint;

			// Re-pairing if normal was flipped:
			if( Vector3Ops::Dot( rawNormal, exitNormal ) < Scalar( 0 ) ) {
				if( result.derivatives.valid ) {
					result.derivatives.dndu = -result.derivatives.dndu;
					result.derivatives.dndv = -result.derivatives.dndv;
				}
				if( result.derivatives.curvatureValid ) {
					result.derivatives.curvature = -result.derivatives.curvature;
				}
				if( result.signals.pProvider ) {
					result.signals.nObject = -result.signals.nObject;
					result.signals.bComplementedField = !result.signals.bComplementedField;
				}
			}

			result.scatteredRay = Ray( result.entryPoint, cosineDir );
			result.cosinePdf = cosTheta * INV_PI;

			// The random walk has no analytical area PDF for the exit
			// point — it is implicitly encoded in the throughput weight.
			// In BDPT the entry vertex is marked isDelta=true with
			// isConnectible=false and pdfFwd=0.  This makes the MIS
			// ratio chain pass through cleanly via remap0(0)/remap0(0)=1
			// and prevents connection strategies from targeting a vertex
			// whose spatial PDF is unknown.
			result.pdfSurface = 0;
            // Interior boundary queries define the random-walk domain.
            // Apply coverage once to the physical surface endpoint.
            const IMaterial* material = pObject->GetMaterial();
            result.acceptedAlphaCoverage = material ? material->AlphaCoverage(exitRI.geometric) : 1;
            if (result.acceptedAlphaCoverage <= 0 || (result.acceptedAlphaCoverage < 1 &&
                sampler.GetAlpha1D() >= result.acceptedAlphaCoverage)) return result;
			result.valid = true;

			return result;
		}
	}

	// Walk exceeded maxBounces without exiting — absorbed
	return result;
}
