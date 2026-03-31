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
	// Step 6: Cast probe rays in both +axis and -axis directions.
	// Trace the full intersection chain through the object and collect
	// all valid hits, then select one uniformly (PBRT convention).
	//
	struct ProbeHit {
		Point3 point;
		Vector3 normal;
		OrthonormalBasis3D onb;
	};
	std::vector<ProbeHit> hits;
	hits.reserve( 8 );
	// Limit probe distance to the profile's effective range — hits
	// beyond this contribute negligible energy and may cross voids.
	const Scalar probeMaxDist = pProfile->GetMaximumDistanceForError( 1e-4 );
	const int maxProbeHits = 64;  // safety cap

	// Trace all intersections along +axis and -axis
	for( int dir = 0; dir < 2; dir++ )
	{
		const Vector3 probeDir = (dir == 0) ? probeAxis : -probeAxis;
		Ray probeRay( probeCenter, probeDir );
		probeRay.Advance( BSSRDF_RAY_EPSILON );

		Scalar traveled = 0;
		for( int bounce = 0; bounce < maxProbeHits; bounce++ )
		{
			const Scalar remaining = probeMaxDist - traveled;
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
			h.onb = probeRI.geometric.onb;
			hits.push_back( h );

			// Advance ray past this hit
			traveled += probeRI.geometric.range;
			probeRay = Ray( probeRI.geometric.ptIntersection, probeDir );
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

	// cosProjection for each axis: |dot(entryNormal, axisDir)|
	// This is the Jacobian converting disk area to surface area.
	const Scalar cosN = fabs( Vector3Ops::Dot( entryNormal, exitNormal ) );
	const Scalar cosT = fabs( Vector3Ops::Dot( entryNormal, exitTangent ) );
	const Scalar cosB = fabs( Vector3Ops::Dot( entryNormal, exitBitangent ) );

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
	const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
	const Scalar SwNorm = (41.0 - 20.0 * F0) / 42.0;
	const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, ri );

	// Full BSSRDF weight (for continuation path):
	//   Rd(r) * Ft(exit) * Sw(cosine_dir) / pdfSurface
	// where Sw = Ft(entry) / c is the directional scattering factor
	// for the cosine-sampled continuation direction.
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

	result.entryPoint = entryPoint;
	result.entryNormal = entryNormal;
	result.entryONB = entryONB;
	result.scatteredRay = Ray( entryPoint, cosineDir );
	result.scatteredRay.Advance( BSSRDF_RAY_EPSILON );
	result.cosinePdf = cosTheta * INV_PI;
	result.pdfSurface = pdfSurface;
	result.valid = true;

	return result;
}
