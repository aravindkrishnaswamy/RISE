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
#include "../Interfaces/IObjectManager.h"

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

	// DL-370: the walk's medium can change mid-walk -- a walk that reaches
	// a face shared with a touching random-walk neighbour crosses into it
	// (see the exit event below) -- so the coefficients, the phase
	// asymmetry, the index and the object being walked are walk STATE,
	// loaded here from the arguments and reloaded at every crossing.
	const IObject* pCur = pObject;
	const IMaterial* pCurMaterial = 0;	// 0 = the caller's (entry) material
	RISEPel cur_sigma_a = sigma_a;
	RISEPel cur_sigma_s = sigma_s;
	RISEPel cur_sigma_t = sigma_t;
	Scalar curG = g;
	Scalar curIOR = ior;

	// Preserve the actual RGB proposal rates for both sampling and density
	// evaluation. Physical extinction stays separate in each numerator.
	Scalar sigma_t_max = 0;
	RISEPel proposalRates;
	bool rgbUsesFallback = false;

	// Scalar coefficients for the NM (single-wavelength) path.  NM callers
	// pass the material's coefficients AT THE TRACED WAVELENGTH, broadcast
	// to all three channels (IMaterial::GetRandomWalkSSSParamsNM; DL-374),
	// and a broadcast triple is read exactly.  A non-broadcast triple is an
	// RGB caller's colour; it is collapsed to its Rec. 709 luminance as a
	// last resort, which makes the walk wavelength-independent (grey) --
	// before DL-374 the material handed this path its RGB snapshot, so every
	// spectral render of random-walk SSS was achromatic.  These define
	// physical collision and survival transmittance. A fallback proposal may
	// use another rate, which event weights must divide out.
	auto nmScalar = []( const RISEPel& c ) -> Scalar {
		return ( c[0] == c[1] && c[1] == c[2] ) ? c[0]
			: 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
	};
	Scalar sigma_a_nm = 0;
	Scalar sigma_s_nm = 0;
	Scalar sigma_t_nm = 0;

	auto loadCoefficients = [&]() {
		sigma_t_max = ColorMath::MaxValue( cur_sigma_t );
		proposalRates = cur_sigma_t;
		rgbUsesFallback = false;
		for( int c = 0; c < 3; ++c ) {
			if( proposalRates[c] < 1e-20 ) {
				proposalRates[c] = sigma_t_max;
				rgbUsesFallback = true;
			}
		}
		sigma_a_nm = nmScalar( cur_sigma_a );
		sigma_s_nm = nmScalar( cur_sigma_s );
		sigma_t_nm = sigma_a_nm + sigma_s_nm;
	};
	loadCoefficients();

	// Ensure valid coefficients
	if( sigma_t_max < 1e-20 ) {
		return result;
	}

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
	// leaves into the SAME exterior: the walk stays inside the exterior's
	// SSS bodies (one object, or touching ones -- DL-370), and the caller
	// continues the exit ray with the unchanged IOR stack.  In air every
	// call below is the pre-DL-49 call bit-for-bit.
	const Scalar nExterior = BSSRDFSampling::ExteriorIOR( ri );

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
		pCur->IntersectRay( exitRI, RISE_INFINITY, false, true, false );

		if( !exitRI.geometric.bHit )
		{
			// Ray escaped without hitting a back face.
			// This can happen with open meshes or numerical issues.
			// Try front faces as fallback — the walk may have crossed
			// a thin shell and needs to find the other side.
			RayIntersection fallbackRI( walkRay, nullRasterizerState );
			pCur->IntersectRay( fallbackRI, RISE_INFINITY, true, false, false );

			if( !fallbackRI.geometric.bHit ) {
				// Truly escaped — terminate walk
				return result;
			}

			// Use the front-face hit as exit
			exitRI = fallbackRI;
		}

        // Capture the effective physical endpoint before shading modification.
        // This also covers the front-face fallback above. Internal Fresnel
        // boundaries still define the opaque-domain proposal: no alpha draw
        // occurs until this boundary is selected as the final exit below.
        Scalar exitCoverage = 1;
        if( exitRI.pMaterial && exitRI.pMaterial->GetAlphaMode() != eAlphaOpaque ) {
            // Supply scene/raster context only to coverage. Preserve the raw
            // probe record and existing modifier/proposal inputs.
            RayIntersectionGeometric alphaRI(exitRI.geometric);
            alphaRI.rast = ri.rast;
            alphaRI.signals.pScene = ri.signals.pScene;
            alphaRI.signals.pSelf = pCur;
            alphaRI.signals.ptWorld = alphaRI.ptIntersection;
            exitCoverage = exitRI.pMaterial->AlphaCoverage(alphaRI);
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
			// Spectral mode: single channel, the traced wavelength's
			// extinction (sigma_t_nm, DL-374).
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
				const Scalar Tr0 = exp( -cur_sigma_t[0] * t );
				const Scalar Tr1 = exp( -cur_sigma_t[1] * t );
				const Scalar Tr2 = exp( -cur_sigma_t[2] * t );

				// Use the actual rate mixture. Reuse physical transmittance
				// in the ordinary case, where proposal and physical rates match.
				const Scalar pdfMixture = rgbUsesFallback
					? (proposalRates[0] * exp( -proposalRates[0] * t )
						+ proposalRates[1] * exp( -proposalRates[1] * t )
						+ proposalRates[2] * exp( -proposalRates[2] * t )) / 3.0
					: (cur_sigma_t[0] * Tr0 + cur_sigma_t[1] * Tr1 + cur_sigma_t[2] * Tr2) / 3.0;

				// A density carries inverse-distance units: its magnitude cannot
				// decide whether the normalized collision weight is significant.
				// Reject only invalid densities before dividing physical density.
				if( !IsFiniteDouble( pdfMixture ) || pdfMixture <= 0.0 ) {
					return result;
				}

				// Weight: sigma_s[c] * Tr[c] / pdfMixture
				throughput[0] *= cur_sigma_s[0] * Tr0 / pdfMixture;
				throughput[1] *= cur_sigma_s[1] * Tr1 / pdfMixture;
				throughput[2] *= cur_sigma_s[2] * Tr2 / pdfMixture;
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
			dir = HenyeyGreensteinPhaseFunction::SampleWithG( dir, sampler, curG );
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
				const Scalar Tr0 = exp( -cur_sigma_t[0] * exitDist );
				const Scalar Tr1 = exp( -cur_sigma_t[1] * exitDist );
				const Scalar Tr2 = exp( -cur_sigma_t[2] * exitDist );
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
			// DL-370: an exit whose continuation would start INSIDE another
			// random-walk body is not an exit to the exterior.  The caller
			// starts the continuation (and NEE) at the exit point offset
			// BSSRDF_RAY_EPSILON along the outward normal (`entryPoint`
			// below).  When a touching neighbour's face coincides with this
			// one -- or lies within that offset -- the offset point is
			// inside the neighbour, the continuation meets the neighbour's
			// far face from behind with a stack that never entered it, and
			// the path dies there (the random-walk entry gate rejects a back
			// face; the SSS SPF absorbs it): a closed room of touching
			// conservative walls read 0.70 in a white furnace.  So the walk
			// CROSSES: the interface between the two bodies is a dielectric
			// boundary of relative index n_neighbour / n_current (none at
			// all when they match), and a transmitted walk continues inside
			// the neighbour with its coefficients until it leaves the
			// assembly.
			//
			// CONTAINMENT of that offset point is asked of the scene
			// (`IObjectManager::RandomWalkObjectContaining`), one candidate
			// object at a time, and decided winding-independently wherever
			// the candidate can sign a distance (an analytic solid, a
			// certified-watertight mesh, a CSG composite), and otherwise by
			// the parity of the candidate's own crossings along the outward
			// normal AND its reverse (both odd), which does not read the
			// winding either (see that interface's comment).  Asking each body ALONE is what
			// makes the answer immune to coincident faces: a probe along the
			// walk ray over the whole scene tied between the walker's own
			// exit face and the neighbour's (both at the exit,
			// indistinguishable by facing on a double-sided mesh), and a
			// whole-scene ray from the offset point ties again wherever the
			// neighbour's far face touches a third body (the corners of a
			// closed room of touching walls).  The point is past the walker's
			// own exit face, so a body coincident with that face on the
			// WALKER's side (an inset part flush with the surface, a sheet
			// lying on it) does not contain it and is never reported.  Only
			// random-walk neighbours are crossed into; any other body the
			// offset point lies inside (a diffusion body, an opaque surface)
			// keeps the old behaviour (DL-408).  Residuals: shells of the
			// walker's OWN object (the neighbour must be another object), a
			// point between two disjoint open sheets of one object reads
			// inside, the walk keeps the entry body's boundaryFilter / maxBounces /
			// maxDepth, and the neighbour's alpha coverage (DL-214) is not
			// consulted.
			//
			// COST: every exit event -- including the ones that then reflect
			// at the boundary -- whose offset point lies in ANOTHER
			// random-walk object's world bounding box pays the containment
			// query (a closed-form signed distance for an analytic solid; a
			// closest-point query plus a parity ray walk for a certified
			// mesh; one ray per crossing each way for an uncertified one),
			// touching or not.  An exit in no such box pays one
			// box-tree walk.  Nothing draws from the sampler, so a walk that
			// never meets a neighbour is unchanged bit for bit.  A record
			// without a scene (a unit test's synthetic hit) skips it.
			const Point3 offsetPoint = Point3Ops::mkPoint3( exitPoint,
				exitNormal * BSSRDFSampling::BSSRDF_RAY_EPSILON );
			if( ri.signals.pScene )
			{
				const IObject* pNeighbour = ri.signals.pScene->RandomWalkObjectContaining(
					offsetPoint, exitNormal, pCur );
				const IMaterial* pNbMaterial = pNeighbour ? pNeighbour->GetMaterial() : 0;
				RandomWalkSSSParams nbParams;
				bool nbWalkable = false;
				if( pNbMaterial )
				{
					if( nm > 0 && pNbMaterial->GetRandomWalkSSSParamsNM( nm, nbParams ) ) {
						nbWalkable = true;
					} else if( const RandomWalkSSSParams* pNb = pNbMaterial->GetRandomWalkSSSParams() ) {
						nbParams = *pNb;
						nbWalkable = true;
					}
					nbWalkable = nbWalkable && ColorMath::MaxValue( nbParams.sigma_t ) >= 1e-20;
				}
				if( nbWalkable )
				{
					Vector3 intoNeighbour = dir;
					if( nbParams.ior != curIOR )
					{
						if( !Optics::CalculateRefractedRay( -exitNormal, curIOR, nbParams.ior, intoNeighbour ) )
						{
							dir = Optics::CalculateReflectedRay( dir, exitNormal );
							pos = Point3Ops::mkPoint3( exitPoint, dir * BSSRDFSampling::BSSRDF_RAY_EPSILON );
							continue;
						}
						const Scalar F_interface = Optics::CalculateDielectricReflectance(
							-dir, intoNeighbour, -exitNormal, curIOR, nbParams.ior );
						if( sampler.Get1D() < F_interface )
						{
							dir = Optics::CalculateReflectedRay( dir, exitNormal );
							pos = Point3Ops::mkPoint3( exitPoint, dir * BSSRDFSampling::BSSRDF_RAY_EPSILON );
							continue;
						}
					}
					// Transmitted into the neighbour (stochastic Fresnel: the
					// 1/(1-F) of the coin cancels the physical (1-F)).  The walk
					// resumes at the offset point, which the probe just showed
					// is inside the neighbour.
					pCur = pNeighbour;
					pCurMaterial = pNbMaterial;
					cur_sigma_a = nbParams.sigma_a;
					cur_sigma_s = nbParams.sigma_s;
					cur_sigma_t = nbParams.sigma_t;
					curG = nbParams.g;
					curIOR = nbParams.ior;
					loadCoefficients();
					dir = intoNeighbour;
					pos = offsetPoint;
					continue;
				}
			}

			// Compute Fresnel reflectance at the exit boundary.
			// We are going from medium (curIOR) to the exterior (nExterior;
			// DL-49 -- was hardcoded air).
			Vector3 refractedOut = dir;
			if( !Optics::CalculateRefractedRay( -exitNormal, curIOR, nExterior, refractedOut ) )
			{
				// Total internal reflection — reflect and continue walk
				dir = Optics::CalculateReflectedRay( dir, exitNormal );
				pos = Point3Ops::mkPoint3( exitPoint,
					dir * BSSRDFSampling::BSSRDF_RAY_EPSILON );
				continue;
			}

			// Dielectric Fresnel reflectance at exit
			const Scalar F_exit = Optics::CalculateDielectricReflectance(
				-dir, refractedOut, -exitNormal, curIOR, nExterior );

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
			const Scalar c_norm = BSSRDFSampling::BoundaryTransmissionNormalization(
				BSSRDFSampling::RelativeBoundaryIOR( curIOR, nExterior ) );
			const Scalar FtEntry = BSSRDFSampling::BoundaryTransmission( cosTheta, curIOR, nExterior );
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
			result.signals.pSelf = pCur;
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
			// DL-370: the body the walk left (the entry object unless it
			// crossed into a touching neighbour) and its index.
			result.pExitObject = ( pCur != pObject ) ? pCur : 0;
			result.pExitMaterial = ( pCur != pObject ) ? pCurMaterial : 0;
			result.exitIOR = curIOR;
            // Interior boundary queries define the random-walk domain.
            // Apply coverage once to the physical surface endpoint.
            result.acceptedAlphaCoverage = exitCoverage;
            if (result.acceptedAlphaCoverage <= 0 || (result.acceptedAlphaCoverage < 1 &&
                sampler.GetAlpha1D() >= result.acceptedAlphaCoverage)) return result;
			result.valid = true;

			return result;
		}
	}

	// Walk exceeded maxBounces without exiting — absorbed
	return result;
}
