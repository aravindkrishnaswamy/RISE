//////////////////////////////////////////////////////////////////////
//
//  BSSRDFSampling.cpp - Implementation of BSSRDF importance sampling
//
//  See BSSRDFSampling.h for algorithm overview and factorization.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 30, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BSSRDFSampling.h"

using namespace RISE;

BSSRDFSampling::SampleResult BSSRDFSampling::SampleEntryPoint(
	const RayIntersectionGeometric& ri,
	const IObject* pObject,
	const IMaterial* pMaterial,
	ISampler& sampler,
	const Scalar nm
	)
{
	SampleResult result;

	if( !pObject || !pMaterial ) {
		return result;
	}

	ISubSurfaceDiffusionProfile* pProfile = pMaterial->GetDiffusionProfile();
	if( !pProfile ) {
		return result;
	}

	// Exit point geometry
	const Point3& exitPoint = ri.ptIntersection;
	const Vector3& exitNormal = ri.onb.w();
	const Vector3& exitTangent = ri.onb.u();
	const Vector3& exitBitangent = ri.onb.v();

	// Fresnel transmission at exit point
	const Scalar cosExit = fabs( Vector3Ops::Dot( exitNormal,
		Vector3Ops::Normalize( -ri.ray.Dir() ) ) );
	const Scalar FtExit = pProfile->FresnelTransmission( cosExit, ri );

	if( FtExit < 1e-10 ) {
		return result;
	}

	//
	// Step 1: Choose a color channel uniformly
	//
	const int channel = static_cast<int>( sampler.Get1D() * 3.0 );
	const int ch = (channel >= 3) ? 2 : channel;  // clamp

	//
	// Step 2: Choose a projection axis
	//
	const Scalar axisSample = sampler.Get1D();
	Vector3 probeAxis;
	Vector3 perpU, perpV;

	if( axisSample < 0.5 )
	{
		probeAxis = exitNormal;
		perpU = exitTangent;
		perpV = exitBitangent;
	}
	else if( axisSample < 0.75 )
	{
		probeAxis = exitTangent;
		perpU = exitNormal;
		perpV = exitBitangent;
	}
	else
	{
		probeAxis = exitBitangent;
		perpU = exitNormal;
		perpV = exitTangent;
	}

	//
	// Step 3: Sample radius from profile CDF
	//
	const Scalar rSample = pProfile->SampleRadius( sampler.Get1D(), ch, ri );
	if( rSample <= 0 ) {
		return result;
	}

	//
	// Step 4: Sample angle uniformly
	//
	const Scalar phi = TWO_PI * sampler.Get1D();

	//
	// Step 5: Compute probe origin offset in the perpendicular plane
	//
	const Scalar offsetU = rSample * cos( phi );
	const Scalar offsetV = rSample * sin( phi );
	const Point3 probeCenter = Point3Ops::mkPoint3(
		exitPoint,
		perpU * offsetU + perpV * offsetV );

	//
	// Step 6: Cast a single finite CHORD along the probe axis, passing
	// THROUGH the projection plane (the perpU/perpV plane through
	// exitPoint that probeCenter lies in), and collect every
	// intersection the chord crosses -- the standard separable-BSSRDF
	// probe (Christensen & Burley 2015; PBRT's SeparableBSSRDF::Sample_Sp
	// traces the same start-before/travel-through chord).
	//
	// DL-52: the previous implementation started BOTH probe rays AT
	// probeCenter (a point IN the projection plane) and advanced
	// BSSRDF_RAY_EPSILON before the first intersection test, once per
	// +axis and once per -axis.  On a broad flat face, probeCenter is
	// itself coplanar with the local surface (a lateral tangent/
	// bitangent offset never leaves the plane of a flat surface), so
	// BOTH epsilon advances moved the ray origin to the WRONG side of
	// exactly the nearby surface the probe was centered on before the
	// intersection test ever ran.  The dominant near-coplanar entry
	// point -- the one the whole disk-projection scheme is built to
	// find -- was skipped by construction on every sample; only a
	// distant, unrelated surface crossed later along either half-line
	// (e.g. the far side of a slab) could restore any profile mass.
	//
	// Fix: trace ONE continuous ray per axis, starting well BEFORE the
	// projection plane (probeMaxDist back along -axis) and travelling
	// forward THROUGH it for a total chord length of 2*probeMaxDist.  A
	// surface coplanar with (or arbitrarily close to) the projection
	// plane is now crossed mid-chord like any other intersection, never
	// skipped by an epsilon offset anchored ON the plane.  The single
	// monotonic sweep (the bounce loop only ever advances `traveled`
	// forward) cannot hit the same physical point twice, and it
	// subsumes what the old two-direction trace covered on both sides
	// of probeCenter -- no distinct "+axis" and "-axis" loop is needed.
	//
	struct ProbeHit {
        Scalar coverage = 1;
		Point3 point;
		Vector3 normal;		///< Shading normal at probe-ray hit (post-modifier)
		Vector3 geomNormal;	///< Geometric normal — area Jacobian and entry front-face gate
		OrthonormalBasis3D onb;
		SurfaceDerivativesInfo derivatives;
		SurfaceSignalInfo signals;
		TextureFootprint txFootprint;
		Point2 ptCoord;
		Point2 ptCoord1;
		bool bHasTexCoord1;
		Point3 ptObjIntersec;
		RISEPel vColor;
		bool bHasVertexColor;
	};
	std::vector<ProbeHit> hits;
	hits.reserve( 8 );
	// Limit the chord's reach on each side of the projection plane to the
	// profile's effective range — hits beyond this contribute negligible
	// energy and may cross voids.
	const Scalar probeMaxDist = pProfile->GetMaximumDistanceForErrorAt( 1e-4, ri );
	const int maxProbeHits = 64;  // safety cap

	{
		const Point3 chordStart = Point3Ops::mkPoint3( probeCenter, -probeAxis * probeMaxDist );
		Ray probeRay( chordStart, probeAxis );
		// P3 bookkeeping note: this initial epsilon advance is NOT added to
		// `traveled` below, so the loop's actual reach is
		// `chordLength + BSSRDF_RAY_EPSILON` from `chordStart`, not exactly
		// `chordLength` -- negligible (epsilon-scale) but stated explicitly
		// here since the per-hit `remaining` budget is computed against
		// `chordLength` alone.
		probeRay.Advance( BSSRDF_RAY_EPSILON );

		const Scalar chordLength = 2.0 * probeMaxDist;
		Scalar traveled = 0;
		for( int bounce = 0; bounce < maxProbeHits; bounce++ )
		{
			const Scalar remaining = chordLength - traveled;
			if( remaining < BSSRDF_RAY_EPSILON ) break;

			RayIntersection probeRI( probeRay, nullRasterizerState );
			pObject->IntersectRay( probeRI, remaining, true, true, false );

			if( !probeRI.geometric.bHit ) break;

			// Entry material owns the optical proposal, but coverage belongs
            // to this actual endpoint (CSG may inherit a different material).
            Scalar coverage = 1;
            if( probeRI.pMaterial && probeRI.pMaterial->GetAlphaMode() != eAlphaOpaque ) {
                // Supply scene/raster context only to coverage. Preserve the raw
                // probe record and existing modifier/proposal inputs.
                RayIntersectionGeometric alphaRI(probeRI.geometric);
                alphaRI.rast = ri.rast;
                alphaRI.signals.pScene = ri.signals.pScene;
                alphaRI.signals.pSelf = pObject;
                alphaRI.signals.ptWorld = alphaRI.ptIntersection;
                coverage = probeRI.pMaterial->AlphaCoverage(alphaRI);
            }
            if( probeRI.pModifier ) {
				probeRI.pModifier->Modify( probeRI.geometric );
			}

			ProbeHit h;
            h.coverage = coverage;
			h.point = probeRI.geometric.ptIntersection;
			h.normal = probeRI.geometric.vNormal;
			h.geomNormal = probeRI.geometric.vGeomNormal;
			h.onb = probeRI.geometric.onb;
			h.derivatives = probeRI.geometric.derivatives;
			h.signals = probeRI.geometric.signals;
			h.txFootprint = probeRI.geometric.txFootprint;
			h.ptCoord = probeRI.geometric.ptCoord;
			h.ptCoord1 = probeRI.geometric.ptCoord1;
			h.bHasTexCoord1 = probeRI.geometric.bHasTexCoord1;
			h.ptObjIntersec = probeRI.geometric.ptObjIntersec;
			h.vColor = probeRI.geometric.vColor;
			h.bHasVertexColor = probeRI.geometric.bHasVertexColor;

			// Cross-object signals triple (DL-22):
			// Stamped with exactly the values ObjectManager::IntersectRay stamps:
			// pScene = ri.signals.pScene (forwarded from exit hit), pSelf = pObject,
			// ptWorld = h.point.
			h.signals.pScene = ri.signals.pScene;
			h.signals.pSelf = pObject;
			h.signals.ptWorld = h.point;

			// DL-71/DL-75 (P1): recover the TRUE, ray-independent winding
			// normal from a geometry that re-orients its reported normal
			// to face the incoming ray (RayIntersectionGeometric::
			// bGeomNormalOrientedToRay -- double-sided triangle meshes,
			// ClippedPlaneGeometry, BezierPatchGeometry, and HairGeometry).
			//
			// A PREVIOUS version of this fix (round 1, DL-71) applied the
			// correction only on "near-half" hits (`distFromChordStart <
			// probeMaxDist`), reasoning that only those hits were reached
			// from the "opposite" direction a pre-DL-52 `-probeAxis` probe
			// would have used.  That positional model is WRONG in general:
			// the flip predicate each setter evaluates (e.g.
			// TriangleMeshGeometry::IntersectRay's `bFlipGeomNormal =
			// Dot(vGeomNormal, ray.Dir()) > 0`) is a per-hit fact about
			// THIS ray direction and THIS surface winding -- it does not
			// depend on which half of the chord the hit falls in.  On a
			// concave double-sided mesh (an L-corner, an ear) a FAR-half
			// hit can just as easily be an EXITING crossing that gets
			// flipped, and the near/far boundary itself sits at the
			// initial `BSSRDF_RAY_EPSILON` advance from chordStart -- a
			// hit a few ulps past that boundary was silently inverted by
			// the old code for no physical reason.  The correct rule is
			// unconditional: `oriented ? -raw : raw` recovers the true
			// winding-order normal for EVERY hit where the flag is set,
			// regardless of position or approach direction (see the field's
			// own doc comment in RayIntersectionGeometric.h).
			//
			// EXCEPTION -- HairGeometry (DL-75): its
			// `bGeomNormalOrientedToRay` is unconditionally true, but the
			// reported normal is FABRICATED (ray-derived), not the
			// recovery of a genuine two-sided winding normal -- a hair
			// ribbon has no "outward side" to undo the flip back to.
			// Applying the correction there would just report the
			// ray-OPPOSITE direction, not a physically meaningful entry
			// normal, so `bGeomNormalRayDerived` gates the correction off
			// for hair (filed as DL-75: SSS on hair geometry has no
			// defined outward entry normal -- coverage/precision gap, not
			// fixed here).
			//
			// Geometry types that never set `bGeomNormalOrientedToRay`
			// (the vast majority -- any consistently-wound single-sided
			// mesh, and every analytical primitive) are untouched either
			// way, since the flag defaults false for them.
			//
			// DL-96 (checked, NOT changed here): the entry GATE at the
			// call site (PathTracingIntegrator.cpp / BDPTIntegrator.cpp)
			// now admits BSSRDF entry from EITHER face of an open sheet
			// (`RayIntersectionGeometric::bOpenSheet` / `BSSRDFEntryFacing`
			// -- see those doc comments), but this probe loop's own job
			// is different: given that an exit hit was admitted, find a
			// SELF-CONSISTENT nearby entry point on the same physical
			// surface.  For a thin/open sheet the profile's radiative
			// transfer is symmetric in the sheet's SINGLE true normal
			// regardless of which face was viewed (a backlit leaf glows
			// the same way, mirrored) -- so probe hits keep recovering
			// the TRUE winding-order normal unconditionally here, exactly
			// as DL-71/DL-75 already validate ("no legitimate reason for
			// the entry side to disagree", `BSSRDFPlanarProbeReachTest`'s
			// `outwardOriented`/`neePositive` checks).  Gating this
			// recovery on `bOpenSheet` too was tried and REVERTED: every
			// double-sided `TriangleMeshGeometry` hit sets `bOpenSheet`
			// unconditionally (that class has no watertightness
			// certification at all), so it collapsed `outwardOriented`
			// from 500/500 to 0/500 on that suite's real double-sided
			// mesh/clipped-plane fixtures -- confirmed by an isolated
			// rebuild+rerun before reverting.
			if( probeRI.geometric.bGeomNormalOrientedToRay &&
				!probeRI.geometric.bGeomNormalRayDerived )
			{
				h.geomNormal = -h.geomNormal;

				// P2-A: the SHADING normal's own flip predicate
				// (`Dot(vNormal, ray.Dir()) > 0`, e.g.
				// TriangleMeshGeometry::IntersectRay's `ri.vNormal =
				// -ri.vNormal` a few lines above its independent
				// `bFlipGeomNormal` test) is evaluated INDEPENDENTLY of
				// the geometric normal's -- at a grazing crossing the two
				// can disagree (the interpolated per-vertex shading
				// normal already opposes the chord while the flat face
				// normal does not, or vice versa).  Blindly negating
				// `h.normal` in lockstep with `h.geomNormal` (round-1
				// DL-71 behaviour) can therefore leave the pair in
				// OPPOSITE hemispheres.  Instead, orient the (unflipped)
				// shading normal into the SAME hemisphere as the just-
				// corrected geometric normal -- the pairing every
				// downstream BSSRDF/Fresnel/cosine consumer assumes.
				if( Vector3Ops::Dot( h.normal, h.geomNormal ) < 0 ) {
					h.normal = -h.normal;
				}

				// Rebuild the basis around the corrected shading normal,
				// keeping the existing tangent (u) as the seed so the
				// frame stays a genuine orthonormal triple rather than
				// just negating W in isolation (which downstream
				// consumers that overwrite vNormal/onb together -- e.g.
				// PathTracingIntegrator.cpp, BDPTIntegrator.cpp -- would
				// otherwise receive as a mismatched W-vs-U/V pair).
				h.onb.CreateFromWU( h.normal, h.onb.u() );
			}

			// If the shading normal was flipped relative to the raw probe hit normal,
			// re-pair derivatives, direct curvature, and signal provider normal
			// (CSGObject.cpp / TriangleMeshGeometryIndexed.cpp invariant):
			if( Vector3Ops::Dot( probeRI.geometric.vNormal, h.normal ) < Scalar( 0 ) ) {
				if( h.derivatives.valid ) {
					h.derivatives.dndu = -h.derivatives.dndu;
					h.derivatives.dndv = -h.derivatives.dndv;
				}
				if( h.derivatives.curvatureValid ) {
					h.derivatives.curvature = -h.derivatives.curvature;
				}
				if( h.signals.pProvider ) {
					h.signals.nObject = -h.signals.nObject;
					h.signals.bComplementedField = !h.signals.bComplementedField;
				}
			}

			hits.push_back( h );

			// Advance ray past this hit
			traveled += probeRI.geometric.range;
			probeRay = Ray( probeRI.geometric.ptIntersection, probeAxis );
			probeRay.Advance( BSSRDF_RAY_EPSILON );
			traveled += BSSRDF_RAY_EPSILON;
		}
	}

	const int numHits = static_cast<int>( hits.size() );
	if( numHits == 0 ) {
		return result;
	}

	// Select uniformly among all hits
	const int selected = static_cast<int>(
		sampler.Get1D() * numHits );
	const int sel = (selected >= numHits) ? numHits - 1 : selected;

    // Chord geometry defines the unchanged spatial proposal. Only the
    // selected physical endpoint receives a coverage decision.
    result.acceptedAlphaCoverage = hits[sel].coverage;
    if (result.acceptedAlphaCoverage <= 0 || (result.acceptedAlphaCoverage < 1 &&
        sampler.GetAlpha1D() >= result.acceptedAlphaCoverage)) return result;
	Point3 entryPoint = hits[sel].point;
	Vector3 entryNormal = hits[sel].normal;
	Vector3 entryGeomNormal = hits[sel].geomNormal;
	OrthonormalBasis3D entryONB = hits[sel].onb;

	// Skip if entry point is too close to exit point (self-intersection)
	const Vector3 offset = Vector3Ops::mkVector3( exitPoint, entryPoint );
	const Scalar rActual = Vector3Ops::Magnitude( offset );
	if( rActual < BSSRDF_RAY_EPSILON ) {
		return result;
	}

	// Skip entry points beyond the profile's effective range.
	// This prevents probe rays from finding distant entry points
	// across voids (e.g., mouth cavity between lips).
	const Scalar maxDist = pProfile->GetMaximumDistanceForErrorAt( 1e-4, ri );
	if( rActual > maxDist ) {
		return result;
	}

	//
	// Step 7: Evaluate profile and compute multi-axis PDF
	//
	// Evaluate Rd(r) at the actual 3D distance between exit and entry.
	const RISEPel Rd = pProfile->EvaluateProfile( rActual, ri );

	// Compute offset in exit-point local frame for projected radii
	const Scalar dN = Vector3Ops::Dot( offset, exitNormal );
	const Scalar dT = Vector3Ops::Dot( offset, exitTangent );
	const Scalar dB = Vector3Ops::Dot( offset, exitBitangent );

	// Projected radii for each axis:
	//   Normal axis:    project onto tangent-bitangent plane
	//   Tangent axis:   project onto normal-bitangent plane
	//   Bitangent axis: project onto normal-tangent plane
	const Scalar rProjN = sqrt( dT*dT + dB*dB );
	const Scalar rProjT = sqrt( dN*dN + dB*dB );
	const Scalar rProjB = sqrt( dN*dN + dT*dT );

	// The disk-to-surface area Jacobian uses the geometric normal.
	// Shading-normal modifiers change the angular frame, not probe-hit density.
	// DL-96: `fabs` makes these three projections invariant under a sign
	// flip of `entryGeomNormal`, so they need no `bOpenSheet` branch --
	// unlike the entry-gate and the probe-recovery loop above, an open
	// sheet's choice of "which face's normal" cancels here regardless.
	const Scalar cosN = fabs( Vector3Ops::Dot( entryGeomNormal, exitNormal ) );
	const Scalar cosT = fabs( Vector3Ops::Dot( entryGeomNormal, exitTangent ) );
	const Scalar cosB = fabs( Vector3Ops::Dot( entryGeomNormal, exitBitangent ) );

	// Sum PDF over all 3 axes x 3 channels (PBRT Pdf_Sp convention).
	// For each axis a with probability pdfAxis[a]:
	//   pdf_disk = PdfR(rProj[a], ch) / (2*pi*rProj[a])
	//   pdf_surface = pdf_disk * cosProj[a]
	// Average over channels and sum over axes.
	const Scalar axisProbs[3] = { 0.5, 0.25, 0.25 };
	const Scalar rProjs[3] = { rProjN, rProjT, rProjB };
	const Scalar cosProjs[3] = { cosN, cosT, cosB };

	Scalar pdfSurface = 0;
	for( int a = 0; a < 3; a++ )
	{
		if( rProjs[a] < 1e-10 || cosProjs[a] < 1e-6 ) {
			continue;
		}

		Scalar channelSum = 0;
		for( int c = 0; c < 3; c++ ) {
			channelSum += pProfile->PdfRadius( rProjs[a], c, ri );
		}
		channelSum /= 3.0;

		pdfSurface += axisProbs[a] * channelSum * cosProjs[a]
			/ (TWO_PI * rProjs[a]);
	}

	// Account for uniform selection among probe hits
	pdfSurface /= static_cast<Scalar>( numHits );

	if( pdfSurface < 1e-20 ) {
		return result;
	}

	//
	// Step 8: Generate cosine-weighted direction from entry point
	//
	OrthonormalBasis3D cosineONB;
	cosineONB.CreateFromW( entryNormal );

	const Scalar u1 = sampler.Get1D();
	const Scalar u2 = sampler.Get1D();
	const Scalar cosTheta = sqrt( u1 );
	const Scalar sinTheta = sqrt( 1.0 - u1 );
	const Scalar phiCosine = TWO_PI * u2;

	const Vector3 cosineDir = Vector3Ops::Normalize(
		cosineONB.u() * (sinTheta * cos(phiCosine)) +
		cosineONB.v() * (sinTheta * sin(phiCosine)) +
		cosineONB.w() * cosTheta );

	//
	// Step 9: Compute entry Fresnel and Sw normalization
	//
	// Reconstruct entry hit record so entry IOR and Fresnel are evaluated
	// at the sampled entry hit rather than the exit point (DL-22).
	RayIntersectionGeometric entryRig(
		Ray( entryPoint, -entryNormal ), nullRasterizerState );
	entryRig.bHit = true;
	entryRig.ptIntersection = entryPoint;
	entryRig.vNormal = entryNormal;
	entryRig.vGeomNormal = entryGeomNormal;
	entryRig.onb = entryONB;
	entryRig.derivatives = hits[sel].derivatives;
	entryRig.signals = hits[sel].signals;
	entryRig.txFootprint = hits[sel].txFootprint;
	entryRig.ptCoord = hits[sel].ptCoord;
	entryRig.ptCoord1 = hits[sel].ptCoord1;
	entryRig.bHasTexCoord1 = hits[sel].bHasTexCoord1;
	entryRig.ptObjIntersec = hits[sel].ptObjIntersec;
	entryRig.vColor = hits[sel].vColor;
	entryRig.bHasVertexColor = hits[sel].bHasVertexColor;
	// DL-49: the entry point is on the same object's boundary, bathed in
	// the same exterior medium the exit hit was reached through (the
	// continuation ray carries that same IOR stack), so the entry record
	// inherits the exit record's exterior index.  The probe hits above
	// are fresh intersections and never carried one.
	entryRig.ambientIOR = ExteriorIOR( ri );

	const Scalar eta = RelativeBoundaryIOR(
		pProfile->GetIOR( entryRig ), ExteriorIOR( entryRig ) );
	const Scalar SwNorm = BoundaryTransmissionNormalization( eta );
	const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, entryRig );

	// Full BSSRDF weight (for continuation path):
	//   Rd(r) * Ft(exit) * Ft(entry) / (c * pdfSurface)
	// Sw = Ft(entry)/(c*PI); multiplying by cosine and dividing by
	// the cosine-sampling PDF leaves the directional weight Ft(entry)/c.
	const Scalar SwFactor = (SwNorm > 1e-20) ? FtEntry / SwNorm : FtEntry;
	result.weight = Rd * (FtExit * SwFactor / pdfSurface);

	// Spatial-only weight (for NEE / connections):
	//   Rd(r) * Ft(exit) / pdfSurface
	// NEE and BDPT connections evaluate Sw independently for their
	// own direction, so the continuation Sw must NOT be baked in.
	result.weightSpatial = Rd * (FtExit / pdfSurface);

	// Scalar weight for NM path: use spectral profile evaluation when
	// a wavelength is provided, falling back to RGB luminance otherwise.
	if( nm > 0 ) {
		const Scalar RdNM = pProfile->EvaluateProfileNM( rActual, ri, nm );
		result.weightNM = RdNM * FtExit * SwFactor / pdfSurface;
		result.weightSpatialNM = RdNM * FtExit / pdfSurface;
	} else {
		const Scalar RdScalar = 0.2126 * Rd[0] + 0.7152 * Rd[1] + 0.0722 * Rd[2];
		result.weightNM = RdScalar * FtExit * SwFactor / pdfSurface;
		result.weightSpatialNM = RdScalar * FtExit / pdfSurface;
	}

	// Offset entry point along the surface normal so that shadow
	// rays (NEE) and connection rays (BDPT) clear the originating
	// surface and do not self-intersect.  This is critical for thin
	// geometry where the entry point lies on a face whose back side
	// is within floating-point epsilon.
	// Offset entry point along the surface normal so that shadow
	// rays (NEE), connection rays (BDPT), and the continuation ray
	// all start above the originating surface.  Using the normal
	// offset for the scattered ray (rather than advancing along the
	// ray direction) ensures adequate clearance even for near-grazing
	// directions, and keeps the ray origin consistent with the stored
	// vertex position used by BDPT geometric terms.
	result.entryPoint = Point3Ops::mkPoint3( entryPoint,
		entryNormal * BSSRDF_RAY_EPSILON );
	result.entryNormal = entryNormal;
	result.entryGeomNormal = entryGeomNormal;
	result.entryONB = entryONB;
	result.derivatives = hits[sel].derivatives;
	result.signals = hits[sel].signals;
	result.signals.ptWorld = result.entryPoint;
	result.txFootprint = hits[sel].txFootprint;
	result.ptCoord = hits[sel].ptCoord;
	result.ptCoord1 = hits[sel].ptCoord1;
	result.bHasTexCoord1 = hits[sel].bHasTexCoord1;
	result.ptObjIntersec = hits[sel].ptObjIntersec;
	result.vColor = hits[sel].vColor;
	result.bHasVertexColor = hits[sel].bHasVertexColor;
	result.scatteredRay = Ray( result.entryPoint, cosineDir );
	result.cosinePdf = cosTheta * INV_PI;
	result.pdfSurface = pdfSurface;
	result.valid = true;

	return result;
}
