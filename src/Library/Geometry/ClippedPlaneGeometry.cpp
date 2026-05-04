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
	ri.ptCoord = Point2( h.u, h.v );

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

	if( h.dRange < NEARZERO || h.dRange > dHowFar ) {
		return false;
	}

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
	// Stratified (u, v) sampling on the bilinear surface.  Samples are
	// uniform in the (u, v) parameter square, which is uniform on the
	// surface for parallelogram quads and approximately uniform for
	// nearby shapes.  For genuinely twisted / non-parallelogram quads
	// the surface-area density varies with |dpdu × dpdv|; rendering
	// code that needs strict surface-uniform sampling should weight by
	// the local Jacobian (TODO: expose an area-density helper).  This
	// is consistent with the prior implementation, which also assumed
	// a parallelogram parametrisation.
	const Scalar u = prand.x;
	const Scalar v = prand.y;

	Point3 pt = GeometricUtilities::BilinearForward(
		vP[0], vP[1], vP[2], vP[3], u, v );

	const Vector3 dpdu = GeometricUtilities::BilinearTangentU(
		vP[0], vP[1], vP[2], vP[3], v );
	const Vector3 dpdv = GeometricUtilities::BilinearTangentV(
		vP[0], vP[1], vP[2], vP[3], u );
	const Vector3 nrm = Vector3Ops::Normalize(
		Vector3Ops::Cross( dpdu, dpdv ) );

	if( point ) {
		// Pull the point out by a small epsilon along the surface
		// normal so when the clipped plane is a back-facing luminary
		// it still occludes anyone behind it.  Matches prior behaviour.
		*point = Point3Ops::mkPoint3( pt, nrm * 0.00001 );
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
	// Parallelogram approximation: |edgeA[0]| * |edgeB[1]|.  Exact for
	// planar parallelograms (the dominant use case).  Non-parallelogram
	// quads compute a parallelogram area through edge magnitudes which
	// over-estimates for trapezoids and under-estimates for re-entrant
	// shapes.  TODO: integrate |dpdu × dpdv| over (u, v) for an exact
	// bilinear surface area when this matters for light sampling pdfs.
	return (Vector3Ops::Magnitude(vEdgesA[0]) * Vector3Ops::Magnitude(vEdgesB[1]));
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
	// GetArea (parallelogram approximation) and any external readers
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
}
