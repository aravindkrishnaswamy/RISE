//////////////////////////////////////////////////////////////////////
//
//  ClippedPlaneGeometry.cpp - Implementation of the
//  ClippedPlaneGeometry class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 16, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//  Surface model
//  -------------
//  Pre-2026 the geometry's IntersectRay traced two flat triangles
//  spanning the (vP[0], vP[2]) diagonal, while TessellateToMesh
//  emitted a bilinear surface across the four corners.  For planar
//  quads the two surfaces coincide; for non-planar quads they
//  diverge, and IntersectRay's per-triangle linear UV did not
//  roundtrip through TessellateToMesh's bilinear forward formula.
//
//  This implementation now traces the canonical bilinear surface
//  end-to-end (via RayBilinearPatchIntersection in IntersectRay,
//  GeometricUtilities::BilinearForward in UniformRandomPoint, and
//  GeometricUtilities::BilinearInverse in ComputeSurfaceDerivatives).
//  Corner UVs follow the row-major layout used by TessellateToMesh:
//
//      vP[0] -> (u=0, v=0)
//      vP[1] -> (u=1, v=0)
//      vP[2] -> (u=1, v=1)
//      vP[3] -> (u=0, v=1)
//
//  For planar parallelograms the visible behaviour is unchanged.
//  For non-planar / non-parallelogram quads the geometry now
//  faithfully renders the bilinear surface implied by the four
//  corners (instead of the two-flat-triangle approximation).
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClippedPlaneGeometry.h"
#include "GeometryUtilities.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Animation/KeyframableHelper.h"
#include <atomic>

using namespace RISE;
using namespace RISE::Implementation;

ClippedPlaneGeometry::ClippedPlaneGeometry(
	const Point3 (&vP_)[4],
	const bool bDoubleSided_
	) :
  bDoubleSided( bDoubleSided_ )
{
	vP[0] = vP_[0];
	vP[1] = vP_[1];
	vP[2] = vP_[2];
	vP[3] = vP_[3];

	RegenerateData();
}

ClippedPlaneGeometry::~ClippedPlaneGeometry( )
{
}

// Construct a BilinearPatch whose RISE-bilinear (u, v) — the
// convention used by RayBilinearPatchIntersection (pts[0]->(0,0),
// pts[1]->(0,1), pts[2]->(1,0), pts[3]->(1,1)) — coincides with our
// row-major (u, v) (vP[0]->(0,0), vP[1]->(1,0), vP[2]->(1,1),
// vP[3]->(0,1)).  After the remap, BILINEAR_HIT.u and .v are the
// row-major coordinates directly.
static inline BilinearPatch ToBilinearPatch( const Point3 vP[4] )
{
	BilinearPatch p;
	p.pts[0] = vP[0];  // (u=0, v=0)
	p.pts[1] = vP[3];  // (u=0, v=1)
	p.pts[2] = vP[1];  // (u=1, v=0)
	p.pts[3] = vP[2];  // (u=1, v=1)
	return p;
}

bool ClippedPlaneGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( detail < 1 ) {
		return false;
	}

	const unsigned int nU = detail;
	const unsigned int nV = detail;
	const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );
	const unsigned int rowStride = nU + 1;

	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v = Scalar(j) / Scalar(nV);
		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u = Scalar(i) / Scalar(nU);

			const Point3 pos = GeometricUtilities::BilinearForward(
				vP[0], vP[1], vP[2], vP[3], u, v );

			// Per-vertex analytical normal so non-planar quads tessellate
			// to a smoothly-shaded bilinear surface.  For planar quads
			// every vertex normal collapses to the constant plane normal.
			const Vector3 dpdu = GeometricUtilities::BilinearTangentU(
				vP[0], vP[1], vP[2], vP[3], v );
			const Vector3 dpdv = GeometricUtilities::BilinearTangentV(
				vP[0], vP[1], vP[2], vP[3], u );
			const Vector3 nrm = Vector3Ops::Normalize(
				Vector3Ops::Cross( dpdu, dpdv ) );

			vertices.push_back( pos );
			normals.push_back( nrm );
			coords.push_back( Point2( u, v ) );
		}
	}

	for( unsigned int j = 0; j < nV; j++ ) {
		for( unsigned int i = 0; i < nU; i++ ) {
			const unsigned int a = baseIdx + j     * rowStride + i;
			const unsigned int b = baseIdx + j     * rowStride + (i + 1);
			const unsigned int c = baseIdx + (j+1) * rowStride + i;
			const unsigned int d = baseIdx + (j+1) * rowStride + (i + 1);

			tris.push_back( MakeIndexedTriangleSameIdx( a, b, c ) );
			tris.push_back( MakeIndexedTriangleSameIdx( b, d, c ) );
		}
	}

	return true;
}

void ClippedPlaneGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	const BilinearPatch patch = ToBilinearPatch( vP );

	BILINEAR_HIT h;
	RayBilinearPatchIntersection( ri.ray, h, patch );
	if( !h.bHit ) {
		return;
	}
	RemapToPositiveSheet( h.u, h.v );	// DL-460: a folded dart's hit on its folded sheet

	// Analytic normal at (u, v) on the bilinear surface.
	const Vector3 dpdu = GeometricUtilities::BilinearTangentU(
		vP[0], vP[1], vP[2], vP[3], h.v );
	const Vector3 dpdv = GeometricUtilities::BilinearTangentV(
		vP[0], vP[1], vP[2], vP[3], h.u );
	Vector3 nrm = Vector3Ops::Normalize( Vector3Ops::Cross( dpdu, dpdv ) );

	// Front-face = ray going opposite to the surface normal (cosI < 0).
	// Apply the same culling semantics the legacy two-triangle path used:
	//   * bHitFrontFaces gates front-face hits.
	//   * bHitBackFaces gates back-face hits, and only on double-sided
	//     geometry (single-sided clipped planes still treat the back as
	//     invisible regardless of bHitBackFaces, matching prior behaviour).
	const Scalar cosI = Vector3Ops::Dot( nrm, ri.ray.Dir() );
	const bool   isBackFaceHit = cosI > 0.0;

	const bool allowed =
		isBackFaceHit ? (bHitBackFaces && bDoubleSided)
		              : bHitFrontFaces;
	if( !allowed ) {
		return;
	}

	// For back-face hits flip the normal so it faces the incoming ray,
	// matching the legacy two-triangle behaviour where vNormal was
	// implicitly oriented toward the camera for double-sided geometry.
	if( isBackFaceHit ) {
		nrm = -nrm;
	}

	ri.bHit = true;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;
	ri.ptIntersection = ri.ray.PointAtLength( ri.range );
	ri.vNormal = nrm;
	ri.vGeomNormal = nrm;	// flat plane: shading == geometric
	// The back-face flip above orients the geometric normal toward the
	// ray -- record it so consumers needing the TRUE surface facing
	// (RayCaster's x-ray self-hit test) can recover the unflipped sign.
	ri.bGeomNormalOrientedToRay = isBackFaceHit;
	// DL-96: a plane/patch never encloses a volume, so a double-sided
	// hit here is always an open sheet (both faces are legitimate
	// physical sides) -- see RayIntersectionGeometric::bOpenSheet's
	// doc comment.
	ri.bOpenSheet = isBackFaceHit;
	// DL-157 review round 2: a PLANE cannot enclose a volume, so this is a
	// real certification rather than "uncertified" -- see the flag's doc.
	// DL-345: stamped on EVERY hit (it used to be back-face only, which no
	// consumer needed beyond): the transmissive SPFs cross an open sheet
	// by its face, and a FRONT hit must be recognised as one too, or a
	// front hit with the sheet already on the stack reads as an exit.
	ri.bProvablyNoInterior = true;
	ri.ptCoord = Point2( h.u, h.v );

	// docs/CLOTH_FABRIC_DESIGN.md 9.1: `dpdu` above is the bilinear
	// surface's own analytic UV tangent -- a genuine parameterisation
	// with no per-triangle discontinuous fallback (unlike the mesh
	// sites) -- so hand it to the shading ONB unconditionally.  Object-
	// space, un-normalized; Object::IntersectRay normalizes after
	// promoting with the forward matrix and clears the flag if the
	// promoted result degenerates (e.g. a singular transform), so no
	// local degeneracy guard is needed here.  This is a NEW write site:
	// ClippedPlaneGeometry never populated `ri.derivatives` at all
	// before this fix, and still does not -- only the shading tangent
	// is written, not the full SurfaceDerivatives (mip-LOD / SMS
	// curvature) payload, which is out of this fix's scope.
	ri.bShadingTangentFromGeometry = true;
	ri.vShadingTangent             = dpdu;	// object space
	ri.bHasShadingTangent          = true;

	if( bComputeExitInfo ) {
		// The bilinear surface is single-sided — there is no genuine
		// "exit" intersection for an external ray.  Mirror the legacy
		// flat-triangle behaviour and report the entry point with a
		// flipped normal so callers expecting (entry, exit) pairs get
		// a consistent result.  RayBilinearPatchIntersection does not
		// expose a second-root range in BILINEAR_HIT, so dRange2 stays
		// at its initial value.
		ri.ptExit = ri.ptIntersection;
		ri.vNormal2 = -nrm;
		ri.vGeomNormal2 = ri.vNormal2;	// flat plane: shading == geometric
	}
}

bool ClippedPlaneGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	const BilinearPatch patch = ToBilinearPatch( vP );

	BILINEAR_HIT h;
	RayBilinearPatchIntersection( ray, h, patch );
	if( !h.bHit ) {
		return false;
	}

	// The self-intersection floor lives in RayBilinearPatchIntersection
	// (scale-relative tMin, 2026-09-03); a bare NEARZERO compare here would
	// be a second, absolute threshold on the same noisy quantity.
	if( h.dRange <= 0.0 || h.dRange > dHowFar ) {
		return false;
	}
	RemapToPositiveSheet( h.u, h.v );	// DL-460: same sheet IntersectRay reports

	// Cull the same way IntersectRay does so a shadow ray sees the
	// same surface as a primary ray.
	const Vector3 dpdu = GeometricUtilities::BilinearTangentU(
		vP[0], vP[1], vP[2], vP[3], h.v );
	const Vector3 dpdv = GeometricUtilities::BilinearTangentV(
		vP[0], vP[1], vP[2], vP[3], h.u );
	const Vector3 nrm = Vector3Ops::Normalize(
		Vector3Ops::Cross( dpdu, dpdv ) );

	const Scalar cosI = Vector3Ops::Dot( nrm, ray.Dir() );
	const bool   isBackFaceHit = cosI > 0.0;

	return isBackFaceHit ? (bHitBackFaces && bDoubleSided)
	                     : bHitFrontFaces;
}

void ClippedPlaneGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	int			i;

	Point3		ptMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3		ptMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );

	// The bilinear surface is bounded by the convex hull of its four
	// corners (the saddle term D·u·v lies inside the parallelepiped
	// spanned by the corner vectors), so the corner-AABB still
	// contains the surface.  No additional padding required.
	for( i=0; i<4; i++ )
	{
		if( vP[i].x < ptMin.x ) {
			ptMin.x = vP[i].x;
		}
		if( vP[i].y < ptMin.y ) {
			ptMin.y = vP[i].y;
		}
		if( vP[i].z < ptMin.z ) {
			ptMin.z = vP[i].z;
		}
		if( vP[i].x > ptMax.x ) {
			ptMax.x = vP[i].x;
		}
		if( vP[i].y > ptMax.y ) {
			ptMax.y = vP[i].y;
		}
		if( vP[i].z > ptMax.z ) {
			ptMax.z = vP[i].z;
		}
	}

	ptCenter = Point3Ops::WeightedAverage2( ptMax, ptMin, 0.5 );
	radius = 0;

	for( i=0; i<4; i++ )
	{
		Vector3			r = Vector3Ops::mkVector3( vP[i], ptCenter );
		const Scalar	d = Vector3Ops::Magnitude(r);

		if( d > radius ) {
			radius = d;
		}
	}
}

BoundingBox ClippedPlaneGeometry::GenerateBoundingBox() const
{
	// The bilinear surface lies in the convex hull of its corners
	// (see GenerateBoundingSphere comment), so the corner-AABB
	// contains it.
	Point3 ll = Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3 ur = Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );

	for( unsigned int i=0; i<4; i++ )
	{
		if( vP[i].x < ll.x ) {
			ll.x = vP[i].x;
		}
		if( vP[i].x > ur.x ) {
			ur.x = vP[i].x;
		}

		if( vP[i].y < ll.y ) {
			ll.y = vP[i].y;
		}
		if( vP[i].y > ur.y ) {
			ur.y = vP[i].y;
		}

		if( vP[i].z < ll.z ) {
			ll.z = vP[i].z;
		}
		if( vP[i].z > ur.z ) {
			ur.z = vP[i].z;
		}
	}

	// Add a little fudge to avoid grazing-ray miss at the bounding planes.
	return BoundingBox(
		Point3( ll.x + (-0.001), ll.y + (-0.001), ll.z + (-0.001) ),
		Point3( ur.x + (0.001), ur.y + (0.001), ur.z + (0.001) ) );
}

void ClippedPlaneGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// DL-460: AREA-UNIFORM on the traced bilinear surface, with density
	// exactly 1/GetArea().  A parallelogram (rectangles included) has a
	// constant area element, so uniform (u, v) is exact and this keeps the
	// pre-DL-460 arithmetic (bit-identical).  Any other quad draws a cell
	// from the bound-weighted CDF (prand.z), a point uniform in it
	// (prand.x, prand.y) and accepts with integrand / bound; the accept
	// variates and retries come from a stream seeded by prand's bits, so
	// the map stays a pure function of prand and an accepted first
	// candidate keeps prand's stratification.  See the header.
	Scalar u = prand.x;
	Scalar v = prand.y;
	const AreaMode mode = nAreaMode;
	const bool hasTable = bHasCellTable;
	if( mode == eAreaGeneral && hasTable ) {
		GeometricUtilities::PrandStream stream( prand, 0x434C50 /*'CLP'*/ );
		Scalar cz = prand.z, cx = prand.x, cy = prand.y;
		static const int kMaxCandidates = 4096;
		bool accepted = false;
		for( int attempt = 0; attempt < kMaxCandidates && !accepted; ++attempt ) {
			const auto it =
				std::upper_bound( vCellCdf.begin(), vCellCdf.end(), cz );
			const std::size_t cell = std::min( std::size_t( it - vCellCdf.begin() ), vCellCdf.size() - std::size_t( 1 ) );
			const unsigned int ci = static_cast<unsigned int>( cell % nAreaCells );
			const unsigned int cj = static_cast<unsigned int>( cell / nAreaCells );
			u = ( Scalar( ci ) + cx ) / Scalar( nAreaCells );
			v = ( Scalar( cj ) + cy ) / Scalar( nAreaCells );
			accepted = stream.Next() * vCellBound[cell] < AreaIntegrand( u, v );
			if( !accepted ) {
				cz = stream.Next(); cx = stream.Next(); cy = stream.Next();
			}
		}
		if( !accepted ) {
			static std::atomic<bool> warned{ false };
			bool expected = false;
			if( warned.compare_exchange_strong( expected, true ) ) {
				GlobalLog()->PrintEasyWarning( "ClippedPlaneGeometry:: area sampler exhausted its rejection candidates (DL-460); that sample is not area-uniform" );
			}
		}
	}

	Point3 pt = GeometricUtilities::BilinearForward(
		vP[0], vP[1], vP[2], vP[3], u, v );

	const Vector3 dpdu = GeometricUtilities::BilinearTangentU(
		vP[0], vP[1], vP[2], vP[3], v );
	const Vector3 dpdv = GeometricUtilities::BilinearTangentV(
		vP[0], vP[1], vP[2], vP[3], u );
	const Vector3 nrm = Vector3Ops::Normalize(
		Vector3Ops::Cross( dpdu, dpdv ) );

	if( point ) {
		// SINGLE-sided: pull the point out by a small epsilon along the
		// surface normal so when the clipped plane is a back-facing
		// luminary it still occludes anyone behind it.  Matches prior
		// behaviour.
		//
		// DOUBLE-sided (DL-320): no offset.  Both faces emit
		// (IGeometry::IsDoubleSided), so a receiver behind the plane is
		// lit by the back face, and a point pushed 1e-5 toward the FRONT
		// would sit on the far side of the plane from it: every
		// connection from behind would cross the emitter and read as
		// occluded under a connection epsilon smaller than the push
		// (BDPT/VCM use 2 * BDPT_RAY_EPSILON; PT's NEE shadow ray stops
		// 0.001 short and happened to clear it).  On the plane itself the
		// point is reachable from both faces.
		*point = bDoubleSided ? pt : Point3Ops::mkPoint3( pt, nrm * 0.00001 );
	}

	if( normal ) {
		*normal = nrm;
	}

	if( coord ) {
		*coord = Point2( u, v );
	}
}

SurfaceDerivatives ClippedPlaneGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;

	// Recover (u, v) on the bilinear surface from the object-space hit
	// point.  This is the *bilinear inverse* — solving
	//   pos(u, v) = c00·(1-u)(1-v) + c10·u(1-v) + c11·uv + c01·(1-u)v
	//             = objSpacePoint
	// for (u, v) ∈ [0, 1]².  See GeometricUtilities::BilinearInverse for
	// the algorithm (a 2x2 quadratic-in-v reduction with axis-pair
	// selection by patch-normal alignment).
	Scalar u = 0.0, v = 0.0;
	const bool ok = GeometricUtilities::BilinearInverse(
		vP[0], vP[1], vP[2], vP[3], objSpacePoint, u, v );

	if( !ok ) {
		// Off-surface input — fall back to a conservative (0, 0) UV so
		// downstream consumers (SMS, surface-derivative readers) get a
		// finite answer instead of NaNs.
		u = 0.0;
		v = 0.0;
	}

	// Analytical first-order derivatives at (u, v).
	sd.dpdu = GeometricUtilities::BilinearTangentU(
		vP[0], vP[1], vP[2], vP[3], v );
	sd.dpdv = GeometricUtilities::BilinearTangentV(
		vP[0], vP[1], vP[2], vP[3], u );

	// Normal derivatives via the unit-normal product rule:
	//   N(u, v) = unnormalised cross / |cross|, with
	//   cross(u, v) = dpdu(v) × dpdv(u).
	//   d(cross)/du = d(dpdu)/du × dpdv + dpdu × d(dpdv)/du
	// Only d(dpdv)/du is non-zero for bilinear (dpdu does not depend on
	// u) — and similarly d(dpdu)/dv = (c11 - c01) - (c10 - c00) is
	// independent of (u, v).
	const Vector3 d_dpdu_dv = Vector3(
		(vP[2].x - vP[3].x) - (vP[1].x - vP[0].x),
		(vP[2].y - vP[3].y) - (vP[1].y - vP[0].y),
		(vP[2].z - vP[3].z) - (vP[1].z - vP[0].z) );
	const Vector3 d_dpdv_du = Vector3(
		(vP[2].x - vP[1].x) - (vP[3].x - vP[0].x),
		(vP[2].y - vP[1].y) - (vP[3].y - vP[0].y),
		(vP[2].z - vP[1].z) - (vP[3].z - vP[0].z) );

	const Vector3 cross_uv = Vector3Ops::Cross( sd.dpdu, sd.dpdv );
	const Scalar  crossLen = Vector3Ops::Magnitude( cross_uv );
	if( crossLen > NEARZERO ) {
		const Scalar invLen = 1.0 / crossLen;
		const Vector3 N = cross_uv * invLen;

		const Vector3 dCross_du = Vector3Ops::Cross( sd.dpdu, d_dpdv_du );
		const Vector3 dCross_dv = Vector3Ops::Cross( d_dpdu_dv, sd.dpdv );

		sd.dndu = (dCross_du - N * Vector3Ops::Dot( N, dCross_du )) * invLen;
		sd.dndv = (dCross_dv - N * Vector3Ops::Dot( N, dCross_dv )) * invLen;
	} else {
		sd.dndu = Vector3( 0, 0, 0 );
		sd.dndv = Vector3( 0, 0, 0 );
	}

	sd.uv = Point2( u, v );
	sd.valid = true;
	return sd;
}

Scalar ClippedPlaneGeometry::GetArea( ) const
{
	// DL-460: the area of the traced surface, measured by the SAME
	// integrand UniformRandomPoint samples, so 1/GetArea() is its density.
	// A rectangle keeps the legacy |edgeA0| |edgeB1| product (bit-identical
	// for every rect_light and panel); a parallelogram is |e0 x e1|; any
	// other quad is the exact integral built in BuildAreaData.
	switch( nAreaMode ) {
	case eAreaRectangle:
		return (Vector3Ops::Magnitude(vEdgesA[0]) * Vector3Ops::Magnitude(vEdgesB[1]));
	case eAreaParallelogram:
		return Vector3Ops::Magnitude( vJacA );
	default:
		return dArea;
	}
}

Scalar ClippedPlaneGeometry::AreaIntegrand( const Scalar u, const Scalar v ) const
{
	const Vector3 w = vJacA + vJacB * u + vJacC * v;
	if( nJacPlanar == 0 ) {
		return Vector3Ops::Magnitude( w );
	}
	const Scalar s = dJacSign * Vector3Ops::Dot( vPlaneNormal, w );
	if( nJacPlanar == 2 ) {
		return std::fabs( s );
	}
	return s > Scalar( 0 ) ? s : Scalar( 0 );
}

namespace
{
	//! Exact integral of max(0, a + b u + c v) over [0,1]^2: clip the
	//! square by the half-plane and integrate the linear function over
	//! the clipped convex polygon by fan triangles (area x vertex mean is
	//! exact for a linear integrand).
	Scalar IntegratePositivePartOfLinear( const Scalar a, const Scalar b, const Scalar c )
	{
		const Scalar sq[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		Scalar poly[8][2];
		int n = 0;
		for( int i = 0; i < 4; ++i ) {
			const Scalar* p = sq[i];
			const Scalar* q = sq[( i + 1 ) & 3];
			const Scalar fp = a + b * p[0] + c * p[1];
			const Scalar fq = a + b * q[0] + c * q[1];
			if( fp >= 0 ) {
				poly[n][0] = p[0]; poly[n][1] = p[1]; ++n;
			}
			if( ( fp >= 0 ) != ( fq >= 0 ) ) {
				const Scalar t = fp / ( fp - fq );
				poly[n][0] = p[0] + t * ( q[0] - p[0] );
				poly[n][1] = p[1] + t * ( q[1] - p[1] );
				++n;
			}
		}
		Scalar total = 0;
		for( int i = 1; i + 1 < n; ++i ) {
			const Scalar* p0 = poly[0];
			const Scalar* p1 = poly[i];
			const Scalar* p2 = poly[i + 1];
			const Scalar area = Scalar( 0.5 ) * std::fabs(
				( p1[0] - p0[0] ) * ( p2[1] - p0[1] ) - ( p1[1] - p0[1] ) * ( p2[0] - p0[0] ) );
			const Scalar f0 = a + b * p0[0] + c * p0[1];
			const Scalar f1 = a + b * p1[0] + c * p1[1];
			const Scalar f2 = a + b * p2[0] + c * p2[1];
			total += area * ( f0 + f1 + f2 ) / Scalar( 3 );
		}
		return total;
	}
}

void ClippedPlaneGeometry::BuildAreaData()
{
	// w(u,v) = dpdu(v) x dpdv(u) with dpdu = (v1-v0) + v D, dpdv = (v3-v0) + u D,
	// D = v2 - v3 - v1 + v0:  w = A + u B + v C, the u v term D x D = 0.
	const Vector3 e1 = Vector3Ops::mkVector3( vP[1], vP[0] );
	const Vector3 e3 = Vector3Ops::mkVector3( vP[3], vP[0] );
	const Vector3 D(
		vP[2].x - vP[3].x - vP[1].x + vP[0].x,
		vP[2].y - vP[3].y - vP[1].y + vP[0].y,
		vP[2].z - vP[3].z - vP[1].z + vP[0].z );
	vJacA = Vector3Ops::Cross( e1, e3 );
	vJacB = Vector3Ops::Cross( e1, D );
	vJacC = Vector3Ops::Cross( D, e3 );
	nJacPlanar = 0;
	dJacSign = 1;
	dArea = 0;
	bHasCellTable = false;
	bFolded = false;

	const Scalar l1 = Vector3Ops::Magnitude( e1 );
	const Scalar l3 = Vector3Ops::Magnitude( e3 );
	const Scalar scale = std::max( l1, std::max( l3, Vector3Ops::Magnitude( Vector3Ops::mkVector3( vP[2], vP[0] ) ) ) );
	static const Scalar kTol = Scalar( 1e-9 );
	if( Vector3Ops::Magnitude( D ) <= kTol * scale ) {
		// Parallelogram: constant area element.  A RECTANGLE keeps the
		// legacy product so every pre-DL-460 quad light is bit-identical.
		nAreaMode = ( std::fabs( Vector3Ops::Dot( e1, e3 ) ) <= kTol * l1 * l3 )
			? eAreaRectangle : eAreaParallelogram;
		dArea = ( nAreaMode == eAreaRectangle ) ? l1 * Vector3Ops::Magnitude( vEdgesB[1] ) : Vector3Ops::Magnitude( vJacA );
		return;
	}
	nAreaMode = eAreaGeneral;

	if( bCornersCoplanar ) {
		// Polygon orientation = sign of the signed area integral n.(A + B/2 + C/2).
		const Scalar a = Vector3Ops::Dot( vPlaneNormal, vJacA );
		const Scalar b = Vector3Ops::Dot( vPlaneNormal, vJacB );
		const Scalar c = Vector3Ops::Dot( vPlaneNormal, vJacC );
		dJacSign = ( a + Scalar( 0.5 ) * ( b + c ) >= 0 ) ? Scalar( 1 ) : Scalar( -1 );
		// Bow-tie: the turns at the four corners split two and two.
		int npos = 0, nneg = 0;
		for( int i = 0; i < 4; ++i ) {
			const Vector3 ea = Vector3Ops::mkVector3( vP[( i + 1 ) & 3], vP[i] );
			const Vector3 eb = Vector3Ops::mkVector3( vP[( i + 2 ) & 3], vP[( i + 1 ) & 3] );
			const Scalar t = Vector3Ops::Dot( vPlaneNormal, Vector3Ops::Cross( ea, eb ) );
			if( t > 0 ) ++npos; else if( t < 0 ) ++nneg;
		}
		nJacPlanar = ( npos == 2 && nneg == 2 ) ? 2 : 1;
		// Folded: the (affine) area element is negative at some corner.
		if( nJacPlanar == 1 ) {
			const Scalar s00 = dJacSign * a, s10 = dJacSign * ( a + b ), s01 = dJacSign * ( a + c ), s11 = dJacSign * ( a + b + c );
			bFolded = std::min( std::min( s00, s10 ), std::min( s01, s11 ) ) < 0;
		}
		dArea = IntegratePositivePartOfLinear( dJacSign * a, dJacSign * b, dJacSign * c );
		if( nJacPlanar == 2 ) {
			dArea += IntegratePositivePartOfLinear( -dJacSign * a, -dJacSign * b, -dJacSign * c );
		}
	} else {
		// Non-coplanar: |w| is smooth (a twisted bilinear patch has no
		// fold), so tensor Gauss-Legendre per cell converges fast.
		static const Scalar xg[5] = { -0.9061798459386640, -0.5384693101056831, 0.0, 0.5384693101056831, 0.9061798459386640 };
		static const Scalar wg[5] = { 0.2369268850561891, 0.4786286704993665, 0.5688888888888889, 0.4786286704993665, 0.2369268850561891 };
		const Scalar h = Scalar( 1 ) / Scalar( nAreaCells );
		Scalar total = 0;
		for( unsigned int cj = 0; cj < nAreaCells; ++cj ) {
			for( unsigned int ci = 0; ci < nAreaCells; ++ci ) {
				Scalar cellSum = 0;
				for( int a = 0; a < 5; ++a ) {
					const Scalar u = h * ( Scalar( ci ) + Scalar( 0.5 ) * ( xg[a] + 1 ) );
					for( int b = 0; b < 5; ++b ) {
						const Scalar v = h * ( Scalar( cj ) + Scalar( 0.5 ) * ( xg[b] + 1 ) );
						cellSum += wg[a] * wg[b] * AreaIntegrand( u, v );
					}
				}
				total += cellSum * Scalar( 0.25 ) * h * h;
			}
		}
		dArea = total;
	}

	// Cell bounds: every integrand above is CONVEX in (u, v) (a norm or a
	// positive part of an affine map), so its maximum over a cell is at a
	// corner -- a rigorous bound.
	Scalar acc = 0;
	for( unsigned int cj = 0; cj < nAreaCells; ++cj ) {
		for( unsigned int ci = 0; ci < nAreaCells; ++ci ) {
			const Scalar u0 = Scalar( ci ) / Scalar( nAreaCells ), u1 = Scalar( ci + 1 ) / Scalar( nAreaCells );
			const Scalar v0 = Scalar( cj ) / Scalar( nAreaCells ), v1 = Scalar( cj + 1 ) / Scalar( nAreaCells );
			const Scalar bound = std::max( std::max( AreaIntegrand( u0, v0 ), AreaIntegrand( u1, v0 ) ),
			                               std::max( AreaIntegrand( u0, v1 ), AreaIntegrand( u1, v1 ) ) );
			vCellBound[cj * nAreaCells + ci] = bound;
			acc += bound;
			vCellCdf[cj * nAreaCells + ci] = acc;
		}
	}
	if( acc > 0 ) {
		for( Scalar& c : vCellCdf ) c /= acc;
		vCellCdf.back() = 1;
		bHasCellTable = true;
	}
}

void ClippedPlaneGeometry::RemapToPositiveSheet( Scalar& u, Scalar& v ) const
{
	// Only a folded coplanar quad, and only a hit on the negative sheet.
	// On a plane the two sheets meet the ray at the same distance, so which
	// root RayBilinearPatchIntersection returns is decided by rounding; the
	// luminary (and its sampler) is the positive sheet, so take the other
	// preimage of the same point when it exists.
	if( !bFolded || AreaIntegrand( u, v ) > Scalar( 0 ) ) {
		return;
	}
	const Point3 x = GeometricUtilities::BilinearForward( vP[0], vP[1], vP[2], vP[3], u, v );
	// 2-D bilinear inverse in the plane's basis: q = u e + v f + u v D, so
	// (e + v D) is parallel to (q - v f): cross(e + vD, q - vf) = 0, a
	// quadratic in v.
	const Vector3 dq = Vector3Ops::mkVector3( x, vP[0] );
	const Vector3 de = Vector3Ops::mkVector3( vP[1], vP[0] );
	const Vector3 df = Vector3Ops::mkVector3( vP[3], vP[0] );
	const Vector3 dD( vP[2].x - vP[3].x - vP[1].x + vP[0].x, vP[2].y - vP[3].y - vP[1].y + vP[0].y, vP[2].z - vP[3].z - vP[1].z + vP[0].z );
	const Scalar q[2] = { Vector3Ops::Dot( dq, vPlaneU ), Vector3Ops::Dot( dq, vPlaneV ) };
	const Scalar e[2] = { Vector3Ops::Dot( de, vPlaneU ), Vector3Ops::Dot( de, vPlaneV ) };
	const Scalar f[2] = { Vector3Ops::Dot( df, vPlaneU ), Vector3Ops::Dot( df, vPlaneV ) };
	const Scalar D[2] = { Vector3Ops::Dot( dD, vPlaneU ), Vector3Ops::Dot( dD, vPlaneV ) };
	auto cr = []( const Scalar* a, const Scalar* b ) { return a[0]*b[1] - a[1]*b[0]; };
	const Scalar A = -cr( D, f );
	const Scalar B = cr( D, q ) - cr( e, f );
	const Scalar C = cr( e, q );
	Scalar roots[2];
	int n = 0;
	const Scalar scale = std::fabs( A ) + std::fabs( B ) + std::fabs( C );
	if( std::fabs( A ) <= Scalar( 1e-12 ) * scale ) {
		if( std::fabs( B ) > 0 ) roots[n++] = -C / B;
	} else {
		const Scalar disc = B*B - 4*A*C;
		if( disc >= 0 ) {
			const Scalar sq = std::sqrt( disc );
			const Scalar t = -0.5 * ( B + ( B >= 0 ? sq : -sq ) );	// stable form
			if( t != 0 ) { roots[n++] = t / A; roots[n++] = C / t; }
			else { roots[n++] = 0; }
		}
	}
	static const Scalar kSlack = Scalar( 1e-9 );
	Scalar best = Scalar( 0 );
	for( int i = 0; i < n; ++i ) {
		const Scalar vv = roots[i];
		if( vv < -kSlack || vv > 1 + kSlack ) continue;
		const Scalar g[2] = { e[0] + vv * D[0], e[1] + vv * D[1] };
		const Scalar gg = g[0]*g[0] + g[1]*g[1];
		if( !( gg > 0 ) ) continue;
		const Scalar uu = ( ( q[0] - vv * f[0] ) * g[0] + ( q[1] - vv * f[1] ) * g[1] ) / gg;
		if( uu < -kSlack || uu > 1 + kSlack ) continue;
		const Scalar uc = std::min( Scalar( 1 ), std::max( Scalar( 0 ), uu ) );
		const Scalar vc = std::min( Scalar( 1 ), std::max( Scalar( 0 ), vv ) );
		const Scalar sPos = AreaIntegrand( uc, vc );
		if( sPos > best ) { best = sPos; u = uc; v = vc; }
	}
}

static const unsigned int PTA_ID = 100;
static const unsigned int PTB_ID = 101;
static const unsigned int PTC_ID = 102;
static const unsigned int PTD_ID = 103;

IKeyframeParameter* ClippedPlaneGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "pta" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, PTA_ID );
		}
	} else if( name == "ptb" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, PTB_ID );
		}
	} else if( name == "ptc" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, PTC_ID );
		}
	} else if( name == "ptd" ) {
		Point3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Point3Keyframe( v, PTD_ID );
		}
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void ClippedPlaneGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case PTA_ID:
	case PTB_ID:
	case PTC_ID:
	case PTD_ID:
		{
			vP[val.getID()-PTA_ID] = *(Point3*)val.getValue();
		}
		break;
	}
}

void ClippedPlaneGeometry::RegenerateData( )
{
	// vEdgesA / vEdgesB / vNormalA / vNormalB / vNormal are kept for
	// the legacy rectangle GetArea (DL-460) and any external readers
	// of the legacy "average plane normal" — IntersectRay,
	// IntersectRay_IntersectionOnly, UniformRandomPoint, and
	// ComputeSurfaceDerivatives all use the analytical bilinear
	// derivatives from GeometricUtilities directly.
	vEdgesA[0] = Vector3Ops::mkVector3( vP[1], vP[0] );
	vEdgesA[1] = Vector3Ops::mkVector3( vP[2], vP[0] );

	vEdgesB[0] = Vector3Ops::mkVector3( vP[2], vP[0] );
	vEdgesB[1] = Vector3Ops::mkVector3( vP[3], vP[0] );

	vNormalA = Vector3Ops::Normalize(Vector3Ops::Cross( vEdgesA[0], vEdgesA[1] ));
	vNormalB = Vector3Ops::Normalize(Vector3Ops::Cross( vEdgesB[0], vEdgesB[1] ));
	vNormal = Vector3Ops::Normalize(Vector3Ops::WeightedAverage2(vNormalA, vNormalB, 0.5, 0.5));

	// COPLANARITY, decided once -- see the field's doc comment in the
	// header for why this geometry is a bilinear patch and why a
	// non-coplanar one must refuse the proximity query rather than answer
	// with a flat approximation.
	//
	// The test is scale-relative, not an absolute epsilon: a quad whose
	// corners are metres apart and one whose corners are millimetres apart
	// must be judged by the same standard.  `scale` below is the largest
	// edge-from-v0 length, so `tol` is "one part in 1e-9 of the quad's own
	// size" -- the same relative discipline Object's sigma detection uses.
	bCornersCoplanar = false;
	bCornersConvex   = false;
	vPlaneNormal = Vector3( 0, 0, 1 );
	vPlaneU      = Vector3( 1, 0, 0 );
	vPlaneV      = Vector3( 0, 1, 0 );
	{
		const Vector3 e1 = Vector3Ops::mkVector3( vP[1], vP[0] );
		const Vector3 e3 = Vector3Ops::mkVector3( vP[3], vP[0] );
		const Vector3 e2 = Vector3Ops::mkVector3( vP[2], vP[0] );
		const Vector3 n  = Vector3Ops::Cross( e1, e3 );
		const Scalar nLen = Vector3Ops::Magnitude( n );
		const Scalar scale = std::max( Vector3Ops::Magnitude( e1 ),
		                     std::max( Vector3Ops::Magnitude( e2 ), Vector3Ops::Magnitude( e3 ) ) );
		if( nLen > Scalar( 0 ) && scale > Scalar( 0 ) ) {
			const Vector3 nHat = n * ( Scalar( 1 ) / nLen );
			// v2's out-of-plane offset is the whole test: v0, v1 and v3
			// span the candidate plane by construction, so a quad is planar
			// exactly when its fourth corner lies in it.
			const Scalar off = std::fabs( Vector3Ops::Dot( e2, nHat ) );
			if( off <= Scalar( 1e-9 ) * scale ) {
				bCornersCoplanar = true;
				vPlaneNormal = nHat;
				vPlaneU = Vector3Ops::Normalize( e1 );
				// Completed by cross product rather than by orthogonalising
				// e3, so the basis is orthonormal even for a sheared quad.
				vPlaneV = Vector3Ops::Cross( nHat, vPlaneU );

				// AND IS IT CONVEX?  Coplanarity alone is NOT enough for the
				// point-to-quad closed form to be exact, because the surface
				// this class traces is the BILINEAR PATCH through the four
				// corners, not the polygon they outline.  For a coplanar
				// CONVEX quad the two coincide -- the bilinear map is a
				// bijection from [0,1]^2 onto the quad -- and the closed form
				// is exact.  For a coplanar NON-convex one (a dart: one
				// corner inside the triangle of the other three) they do not:
				// the patch FOLDS, and its image is the polygon PLUS a folded
				// overshoot outside it (DL-460's degree argument, see
				// BuildAreaData), so over the overshoot the point-to-polygon
				// form reports the distance to the polygon's edge while the
				// real surface is nearer.  That error is an over-report, but
				// the form is no longer exact, so a dart REFUSES, exactly as
				// a non-coplanar quad does.
				//
				// The test is the standard one: the four cross products of
				// consecutive edges, taken in the plane's own basis, must all
				// share a sign.  Scale-relative against the largest of them,
				// so a collinear triple (a degenerate "triangle" quad, whose
				// bilinear image is again not the polygon) refuses too rather
				// than passing on a sign read out of rounding noise.
				Scalar cx[4], cy[4];
				for( int i = 0; i < 4; ++i ) {
					const Vector3 dd = Vector3Ops::mkVector3( vP[i], vP[0] );
					cx[i] = Vector3Ops::Dot( dd, vPlaneU );
					cy[i] = Vector3Ops::Dot( dd, vPlaneV );
				}
				Scalar cr[4];
				Scalar amax = Scalar( 0 );
				for( int i = 0; i < 4; ++i ) {
					const int j = ( i + 1 ) & 3;
					const int k = ( i + 2 ) & 3;
					const Scalar ax = cx[j] - cx[i], ay = cy[j] - cy[i];
					const Scalar bx = cx[k] - cx[j], by = cy[k] - cy[j];
					cr[i] = ax * by - ay * bx;
					const Scalar m = std::fabs( cr[i] );
					if( m > amax ) { amax = m; }
				}
				if( amax > Scalar( 0 ) ) {
					const Scalar crTol = Scalar( 1e-9 ) * amax;
					bool convex = true;
					for( int i = 0; i < 4; ++i ) {
						if( std::fabs( cr[i] ) <= crTol
						 || ( cr[i] > Scalar( 0 ) ) != ( cr[0] > Scalar( 0 ) ) ) {
							convex = false;
							break;
						}
					}
					bCornersConvex = convex;
				}
			}
		}
	}

	BuildAreaData();
}

bool ClippedPlaneGeometry::DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const
{
	(void)maxDistObject;	// no early-out worth having: the whole form is O(1)

	// EXACT ON A COPLANAR **CONVEX** QUAD, REFUSES OTHERWISE.  Both
	// conditions are decided once in RegenerateData; see the convexity
	// block there for why coplanarity alone is not enough (the traced
	// surface is the bilinear patch, and only for a convex planar quad is
	// its image the polygon below).
	if( !bCornersCoplanar || !bCornersConvex ) {
		return false;
	}

	// The plane's own frame, origin at vP[0].
	const Vector3 rel = Vector3Ops::mkVector3( ptObject, vP[0] );
	const Scalar h = Vector3Ops::Dot( rel, vPlaneNormal );	// signed height above the plane

	// The four corners in the plane's 2D basis, and the query point's
	// projection.
	Scalar cx[4], cy[4];
	for( int i = 0; i < 4; ++i ) {
		const Vector3 d = Vector3Ops::mkVector3( vP[i], vP[0] );
		cx[i] = Vector3Ops::Dot( d, vPlaneU );
		cy[i] = Vector3Ops::Dot( d, vPlaneV );
	}
	const Scalar px = Vector3Ops::Dot( rel, vPlaneU );
	const Scalar py = Vector3Ops::Dot( rel, vPlaneV );

	// POINT IN POLYGON by ray crossing, in the plane.  With non-convex
	// quads now refused above, a four half-plane test would agree with this
	// everywhere; the crossing test is kept because it needs no winding
	// convention and cannot be silently invalidated by a future corner
	// reordering.
	bool inside = false;
	for( int i = 0, j = 3; i < 4; j = i++ ) {
		if( ( cy[i] > py ) != ( cy[j] > py ) ) {
			const Scalar t = ( py - cy[i] ) / ( cy[j] - cy[i] );
			if( px < cx[i] + t * ( cx[j] - cx[i] ) ) {
				inside = !inside;
			}
		}
	}

	if( inside ) {
		// The closest point of the quad-as-a-region IS the projection, so
		// the perpendicular offset is the exact answer; no edge can be
		// nearer than the foot of the perpendicular.
		outDist = std::fabs( h );
		return true;
	}

	// Outside: the nearest point lies on the boundary, so take the minimum
	// over the four edge SEGMENTS in 3D (which already accounts for `h`).
	Scalar best = RISE_INFINITY;
	for( int i = 0, j = 3; i < 4; j = i++ ) {
		const Vector3 ab = Vector3Ops::mkVector3( vP[j], vP[i] );
		const Vector3 ap = Vector3Ops::mkVector3( ptObject, vP[i] );
		const Scalar abb = Vector3Ops::SquaredModulus( ab );
		Scalar t = ( abb > Scalar( 0 ) ) ? ( Vector3Ops::Dot( ap, ab ) / abb ) : Scalar( 0 );
		t = ( t < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( t > Scalar( 1 ) ) ? Scalar( 1 ) : t );
		const Vector3 closest = ab * t;
		const Scalar d = Vector3Ops::Magnitude( ap - closest );
		if( d < best ) { best = d; }
	}
	if( !RISE::IsFiniteDouble( (double)best ) ) {
		return false;
	}
	outDist = best;
	return true;
}
