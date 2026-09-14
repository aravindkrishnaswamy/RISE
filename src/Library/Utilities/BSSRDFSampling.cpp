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
		Point3 point;
		Vector3 normal;		///< Shading normal at probe-ray hit (post-modifier)
		Vector3 geomNormal;	///< Geometric normal — area Jacobian and entry front-face gate
		OrthonormalBasis3D onb;
	};
	std::vector<ProbeHit> hits;
	hits.reserve( 8 );
	// Limit the chord's reach on each side of the projection plane to the
	// profile's effective range — hits beyond this contribute negligible
	// energy and may cross voids.
	const Scalar probeMaxDist = pProfile->GetMaximumDistanceForError( 1e-4 );
	const int maxProbeHits = 64;  // safety cap

	{
		const Point3 chordStart = Point3Ops::mkPoint3( probeCenter, -probeAxis * probeMaxDist );
		Ray probeRay( chordStart, probeAxis );
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

			if( probeRI.pModifier ) {
				probeRI.pModifier->Modify( probeRI.geometric );
			}

			ProbeHit h;
			h.point = probeRI.geometric.ptIntersection;
			h.normal = probeRI.geometric.vNormal;
			h.geomNormal = probeRI.geometric.vGeomNormal;
			h.onb = probeRI.geometric.onb;

			// DL-71 (P1): the chord travels in ONE fixed direction
			// (+probeAxis) for its entire length.  A hit on the near
			// (-axis) side of probeCenter -- i.e. before the chord has
			// travelled probeMaxDist from chordStart -- is the analogue
			// of what the pre-DL-52 code's SEPARATE -probeAxis probe
			// would have found, approaching the surface from the
			// OPPOSITE physical direction; here it was actually reached
			// with ray.Dir()==+probeAxis.  A geometry that re-orients
			// its reported normal to face the incoming ray
			// (RayIntersectionGeometric::bGeomNormalOrientedToRay --
			// currently double-sided triangle meshes, ClippedPlaneGeometry,
			// and BezierPatchGeometry) therefore reports, on every such
			// near-half hit, a normal that faces INTO the solid instead
			// of out of it: "outward" for the entry point must agree
			// with the approach direction a -probeAxis-directed probe
			// would have used, not the chord's actual +probeAxis travel
			// direction.  Far-half hits (beyond probeCenter) were
			// already reached with ray.Dir()==+probeAxis, exactly
			// matching the pre-DL-52 "+axis" probe, so they need no
			// correction.  Recover the ray-independent winding normal
			// (the same recovery RayCaster.cpp's env-escape helper uses,
			// `oriented ? -raw : raw`) by negating; the geometry types
			// that never set the flag (the vast majority -- any
			// consistently-wound single-sided mesh, and every
			// analytical primitive) are untouched either way, since
			// `bGeomNormalOrientedToRay` defaults false for them.
			const Scalar distFromChordStart = traveled + probeRI.geometric.range;
			if( probeRI.geometric.bGeomNormalOrientedToRay && distFromChordStart < probeMaxDist )
			{
				h.normal = -h.normal;
				h.geomNormal = -h.geomNormal;
				// Rebuild the basis around the corrected normal, keeping
				// the existing tangent (u) as the seed so the frame
				// stays a genuine orthonormal triple rather than just
				// negating W in isolation (which downstream consumers
				// that overwrite vNormal/onb together -- e.g.
				// PathTracingIntegrator.cpp, BDPTIntegrator.cpp -- would
				// otherwise receive as a mismatched W-vs-U/V pair).
				h.onb.CreateFromWU( h.normal, h.onb.u() );
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
	const Scalar maxDist = pProfile->GetMaximumDistanceForError( 1e-4 );
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
	const Scalar eta = pProfile->GetIOR( ri );
	const Scalar SwNorm = SchlickTransmissionNormalization( eta );
	const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, ri );

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
	result.scatteredRay = Ray( result.entryPoint, cosineDir );
	result.cosinePdf = cosTheta * INV_PI;
	result.pdfSurface = pdfSurface;
	result.valid = true;

	return result;
}
