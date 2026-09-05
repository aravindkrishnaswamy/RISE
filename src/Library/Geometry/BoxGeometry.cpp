//////////////////////////////////////////////////////////////////////
//
//  BoxGeometry.cpp - Implementation of the box class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 10, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BoxGeometry.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

BoxGeometry::BoxGeometry( Scalar dWidth_, Scalar dHeight_, Scalar dDepth_ ) : 
  dWidth( dWidth_ ), dHeight( dHeight_ ), dDepth( dDepth_ ), 
  dOVWidth( 0 ), dOVHeight( 0 ), dOVDepth( 0 ),
  dWidthOV2( dWidth_*0.5 ), dHeightOV2( dHeight_*0.5 ), dDepthOV2( dDepth_*0.5 )
{
	RegenerateData();
}

BoxGeometry::~BoxGeometry( )
{
}

bool BoxGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( detail < 1 ) {
		return false;
	}

	const Scalar hw = dWidthOV2;
	const Scalar hh = dHeightOV2;
	const Scalar hd = dDepthOV2;
	const Scalar w  = dWidth;
	const Scalar h  = dHeight;
	const Scalar d  = dDepth;

	// Per-face (origin, edgeU, edgeV, normal) matching the UV convention in IntersectRay.
	struct FaceDef {
		Point3  origin;
		Vector3 edgeU;
		Vector3 edgeV;
		Vector3 normal;
	};
	const FaceDef faces[6] = {
		// 0: -X face; u = (z+d/2)/d, v = 1-(y+h/2)/h
		{ Point3( -hw,  hh, -hd ), Vector3(  0.0,  0.0,  d   ), Vector3(  0.0, -h,    0.0 ), Vector3( -1.0,  0.0,  0.0 ) },
		// 1: +X face; u = 1-(z+d/2)/d, v = 1-(y+h/2)/h
		{ Point3(  hw,  hh,  hd ), Vector3(  0.0,  0.0, -d   ), Vector3(  0.0, -h,    0.0 ), Vector3(  1.0,  0.0,  0.0 ) },
		// 2: -Y face; u = (x+w/2)/w, v = 1-(z+d/2)/d
		{ Point3( -hw, -hh,  hd ), Vector3(  w,    0.0,  0.0 ), Vector3(  0.0,  0.0, -d   ), Vector3(  0.0, -1.0,  0.0 ) },
		// 3: +Y face; u = (x+w/2)/w, v = (z+d/2)/d
		{ Point3( -hw,  hh, -hd ), Vector3(  w,    0.0,  0.0 ), Vector3(  0.0,  0.0,  d   ), Vector3(  0.0,  1.0,  0.0 ) },
		// 4: -Z face; u = 1-(x+w/2)/w, v = 1-(y+h/2)/h
		{ Point3(  hw,  hh, -hd ), Vector3( -w,    0.0,  0.0 ), Vector3(  0.0, -h,    0.0 ), Vector3(  0.0,  0.0, -1.0 ) },
		// 5: +Z face; u = (x+w/2)/w, v = 1-(y+h/2)/h
		{ Point3( -hw,  hh,  hd ), Vector3(  w,    0.0,  0.0 ), Vector3(  0.0, -h,    0.0 ), Vector3(  0.0,  0.0,  1.0 ) },
	};

	const unsigned int nU = detail;
	const unsigned int nV = detail;
	const unsigned int rowStride = nU + 1;

	for( int f = 0; f < 6; f++ ) {
		const FaceDef& face = faces[f];
		const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );

		for( unsigned int j = 0; j <= nV; j++ ) {
			const Scalar v = Scalar(j) / Scalar(nV);
			for( unsigned int i = 0; i <= nU; i++ ) {
				const Scalar u = Scalar(i) / Scalar(nU);

				const Point3 pos(
					face.origin.x + u * face.edgeU.x + v * face.edgeV.x,
					face.origin.y + u * face.edgeU.y + v * face.edgeV.y,
					face.origin.z + u * face.edgeU.z + v * face.edgeV.z );

				vertices.push_back( pos );
				normals.push_back( face.normal );
				coords.push_back( Point2( u, v ) );
			}
		}

		for( unsigned int j = 0; j < nV; j++ ) {
			for( unsigned int i = 0; i < nU; i++ ) {
				const unsigned int a = baseIdx + j     * rowStride + i;
				const unsigned int b = baseIdx + j     * rowStride + (i + 1);
				const unsigned int c = baseIdx + (j+1) * rowStride + i;
				const unsigned int d_idx = baseIdx + (j+1) * rowStride + (i + 1);

				tris.push_back( MakeIndexedTriangleSameIdx( a, c, b ) );
				tris.push_back( MakeIndexedTriangleSameIdx( b, c, d_idx ) );
			}
		}
	}

	return true;
}

namespace {

// ================================================================
// Self-hit root suppression -- an origin ON one of the box's own faces.
//
// RayBoxIntersection is a slab test: it returns the entry root (tmin)
// as the primary hit whenever tmin > 0, however small.  A ray whose
// origin was PUBLISHED from a hit on this box -- Object::IntersectRay
// backs the hit point off along the incoming ray by
// SURFACE_INTERSEC_ERROR (1e-12, object-local) -- therefore sits
// ~1e-12 outside (entry face) or inside (exit face) the plane it just
// hit, and a continuation / shadow ray leaving that point re-crosses
// that plane at t = (1e-12 * |d_in.axis|) / |d_out.axis|: a tiny,
// DIRECTION-DEPENDENT root that straddles NEARZERO.  Downstream, every
// closed two-root primitive is consumed as "the FIRST root is the hit"
// (IntersectRay_IntersectionOnly rejects the whole primitive when that
// root is < NEARZERO), so the straddle produced BOTH failure modes on
// the same box:
//   * root < NEARZERO  -> the ENTIRE box was discarded, including the
//     genuine far face -- a shadow ray from the front face toward a
//     light behind the box saw no occluder and lit the front face as if
//     the back face did not exist (PT read a closed thin-transmissive
//     box 3.5x BRIGHTER than the same six faces as separate planes);
//   * root >= NEARZERO -> the box occluded its OWN origin face -- a
//     shadow ray toward a light INSIDE the box was self-shadowed (PT
//     read 0.44x).
// BDPT / VCM never saw either because they Advance() every shadow and
// continuation ray by 1e-6 first, which is why they agreed with each
// other, with PT-on-six-planes, and not with PT-on-the-box
// (docs/CLOTH_FABRIC_DESIGN.md section 15 debt 25).
//
// RaySphereIntersection already handles this class at the producer:
// it skips any root <= NEARZERO and returns the NEXT one.  A box can do
// better than a range threshold, because "the origin is on this face"
// is a PLANE-DISTANCE fact, independent of the ray's angle to the
// face: |origin.axis - bound| is at the back-off / round-off floor.
// So each root is tested against the face it belongs to, and a root
// on the origin's own face is dropped in favour of the other root --
// exactly what would have happened had the origin been published a
// hair further along the ray.  The tolerance is PER AXIS and carries no
// transverse or half-extent term (debt-25 review round 2, P1): the
// plane distance |origin.axis - bound| is an exact FP subtraction of
// two nearby numbers, so its only error is the error in origin.axis
// itself -- the 1e-12 back-off (always <= 1e-12 in these local units)
// plus the world<->local transform's rounding, which scales with THAT
// coordinate, not with the box's other extents or the origin's other
// components.  Hence
//   eps(axis) = 4 * NEARZERO + 64 * DBL_EPSILON * |origin.axis|
// (4x headroom over the back-off; the ulp term is RayCaster::
// ResolveXrayView_'s representability bound).  A coordinate-summed
// band (the first cut mirrored RayBilinearPatchIntersection's
// NEARZERO * (1 + L1 coordinate) floor and added the half-extents)
// grew with the box's TRANSVERSE size and swallowed the deliberate
// standoff CSGObject's exit-face probe uses -- see that function's
// margin derivation for the other half of this contract.
//
// Known limit, pre-existing for every primitive: a box translated far
// from the world origin (|T| >~ 1e4) with non-dyadic face coordinates
// round-trips its published point to within ~ulp(|T|) of the plane,
// which can exceed this band; the geometry only ever sees local
// coordinates and cannot widen for a translation it cannot observe.
//
// Returns true when the primary root was replaced by the far root,
// i.e. the surviving hit is an EXIT face reached from the origin's own
// entry face -- the caller treats that like an origin that began inside
// the box for the front/back-face flag rule.
//
// Known, accepted limits (debt-25 review round 1, P3-2 / P3-3): the test
// is on the PLANE, not on provenance, so (a) a ray within ~1e-9 rad of
// parallel to the origin's face whose genuine entry root lies far along
// the ray is also dropped (|dir.n| < eps / t -- measure-zero in a
// render), and (b) a hit point published by an UNRELATED object whose
// face is exactly coplanar with one of ours reads as our own face
// (exact-contact stacks: a box resting on another box reports the
// lower box's far face for a ray leaving the upper box's bottom -- a
// first-hit shading error, never a shadow leak).  A deliberate standoff
// that wants to re-hit the face it stands off from -- CSGObject's
// exit-face payload probe -- must therefore stand off by MORE than
// `eps` in THIS frame; it derives its margin from the same constants.
// ================================================================
inline Scalar FaceBound( const int side, const Point3& ll, const Point3& ur )
{
	// RayBoxIntersection side ids: SIDE_X0=0, SIDE_X1=1, SIDE_Y0=2, SIDE_Y1=3, SIDE_Z0=4, SIDE_Z1=5.
	switch( side ) {
	case 0: return ll.x;
	case 1: return ur.x;
	case 2: return ll.y;
	case 3: return ur.y;
	case 4: return ll.z;
	default: return ur.z;
	}
}

inline Scalar OriginAxis( const int side, const Point3& o )
{
	switch( side / 2 ) {
	case 0: return o.x;
	case 1: return o.y;
	default: return o.z;
	}
}

inline bool DropSelfHitRoot( const Ray& ray, BOX_HIT& h, const Point3& ll, const Point3& ur )
{
	if( !h.bHit ) {
		return false;
	}

	const Point3& o = ray.origin;
	constexpr Scalar kUlpFactor = 64.0 * 2.2204460492503131e-16;   // 64 * DBL_EPSILON
	auto onFace = [&]( const int side ) -> bool
	{
		const Scalar oAxis = OriginAxis( side, o );
		const Scalar eps = Scalar(4) * NEARZERO + kUlpFactor * std::fabs( oAxis );
		return std::fabs( oAxis - FaceBound( side, ll, ur ) ) <= eps;
	};

	if( !onFace( h.sideA ) ) {
		return false;
	}

	// The primary root is the origin's own face.  The other root is the
	// hit -- if it is ahead of the origin and not ALSO the origin's own
	// face (an edge / corner origin leaving the box).
	if( h.dRange2 <= Scalar(4) * NEARZERO || onFace( h.sideB ) ) {
		h.bHit = false;
		h.dRange = RISE_INFINITY;
		h.dRange2 = RISE_INFINITY;
		return false;
	}

	// Publish as RaySphereIntersection does when it skips a root at the
	// origin: primary = the exit root, dRange2 = 0 -- the "single root,
	// origin inside" sentinel CSGObject reads (`range2 == 0`) and that
	// Object::IntersectRay's exit-info block leaves alone.  (Carrying the
	// ~1e-12 self-root as dRange2 instead would publish an exit BEHIND the
	// entry, which CSG's range ordering misreads.)  sideB keeps the
	// origin's own face so bComputeExitInfo still names a face.
	const int sideSelf = h.sideA;
	h.dRange = h.dRange2;
	h.sideA = h.sideB;
	h.dRange2 = Scalar(0);
	h.sideB = sideSelf;
	return true;
}

} // anonymous namespace

void BoxGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	Point3		ptLowerLeft = Point3( -dWidthOV2, -dHeightOV2, -dDepthOV2 );
	Point3		ptUpperRight = Point3( dWidthOV2, dHeightOV2, dDepthOV2 );

	// If the point is inside the box and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	const bool RayBeginsInBox = GeometricUtilities::IsPointInsideBox( ri.ray.origin, ptLowerLeft, ptUpperRight );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInBox ) {
		return;
	}

	BOX_HIT	h;
	RayBoxIntersection( ri.ray, h, ptLowerLeft, ptUpperRight );

	// An origin on one of our own faces: drop that root (see
	// DropSelfHitRoot above).  The surviving hit is then an EXIT face,
	// exactly as for an origin that began strictly inside -- so the
	// front/back-face rule is decided on that combined predicate rather
	// than on the strict inside test alone (which calls an on-face origin
	// "outside" and used to reject the far face under back-faces-only).
	// Note (false, false) now yields no hit, where the removed early-out
	// let it through; no caller passes that combination.
	const bool bExitHit = DropSelfHitRoot( ri.ray, h, ptLowerLeft, ptUpperRight ) || RayBeginsInBox;
	if( h.bHit && ( bExitHit ? !bHitBackFaces : !bHitFrontFaces ) ) {
		h.bHit = false;
	}

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;
	
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );

		Point3&	it = ri.ptIntersection;

		// Compute normal and texture coords
		switch( h.sideA )
		{
		case 0:
			ri.vNormal = Vector3( -1.0, 0.0, 0.0 );
			// X coords are in +Z, Y coords are in +Y
			ri.ptCoord = Point2( (it.z+dDepthOV2)*dOVDepth, 1.0 - (it.y+dHeightOV2)*dOVHeight );
			break;
		case 1:
			ri.vNormal = Vector3( 1.0, 0.0, 0.0 );
			// X coords are in -Z, Y coords are in +Y
			ri.ptCoord = Point2( 1.0 - (it.z+dDepthOV2)*dOVDepth, 1.0 - (it.y+dHeightOV2)*dOVHeight );
			break;
		case 2:
			ri.vNormal = Vector3( 0.0, -1.0, 0.0 );
			// X coords are in +X, Y coords are in -Z
			ri.ptCoord = Point2( (it.x+dWidthOV2)*dOVWidth, 1.0 - (it.z+dDepthOV2)*dOVDepth );
			break;
		case 3:
			ri.vNormal = Vector3( 0.0, 1.0, 0.0 );
			// X coords are in -X, Y coords are in Z
			ri.ptCoord = Point2( (it.x+dWidthOV2)*dOVWidth, (it.z+dDepthOV2)*dOVDepth );
			break;
		case 4:
			ri.vNormal = Vector3( 0.0, 0.0, -1.0 );
			// X coords are in +Y, Y coords are in -X
			ri.ptCoord = Point2( 1.0 - (it.x+dWidthOV2)*dOVWidth, 1.0 - (it.y+dHeightOV2)*dOVHeight );
			break;
		case 5:
			ri.vNormal = Vector3( 0.0, 0.0, 1.0 );
			// X coords are in -Y, Y coords are in -X
			ri.ptCoord = Point2( (it.x+dWidthOV2)*dOVWidth, 1.0 - (it.y+dHeightOV2)*dOVHeight );
			break;
		};
		ri.vGeomNormal = ri.vNormal;	// box face is flat: shading == geometric

		if( bComputeExitInfo )
		{
			// Compute the normal at the point of exit
			switch( h.sideB )
			{
			case 0:
				ri.vNormal2 = Vector3( -1.0, 0.0, 0.0 );
				break;
			case 1:
				ri.vNormal2 = Vector3( 1.0, 0.0, 0.0 );
				break;
			case 2:
				ri.vNormal2 = Vector3( 0.0, -1.0, 0.0 );
				break;
			case 3:
				ri.vNormal2 = Vector3( 0.0, 1.0, 0.0 );
				break;
			case 4:
				ri.vNormal2 = Vector3( 0.0, 0.0, -1.0 );
				break;
			case 5:
				ri.vNormal2 = Vector3( 0.0, 0.0, 1.0 );
				break;
			};
			ri.vGeomNormal2 = ri.vNormal2;	// box face is flat: shading == geometric
		}
	}
}

bool BoxGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	Point3		ptLowerLeft = Point3( -dWidthOV2, -dHeightOV2, -dDepthOV2 );
	Point3		ptUpperRight = Point3( dWidthOV2, dHeightOV2, dDepthOV2 );

	// If the point is inside the box and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	const bool RayBeginsInBox = GeometricUtilities::IsPointInsideBox( ray.origin, ptLowerLeft, ptUpperRight );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInBox ) {
		return false;
	}

	BOX_HIT	h;
	RayBoxIntersection( ray, h, ptLowerLeft, ptUpperRight );

	// An origin on one of our own faces: drop that root rather than
	// letting a ~1e-12 self-root either occlude the origin's own face or
	// (when it falls under NEARZERO below) discard the genuine far face
	// along with it -- see DropSelfHitRoot above.  Same exit-vs-entry
	// flag rule as IntersectRay.
	const bool bExitHit = DropSelfHitRoot( ray, h, ptLowerLeft, ptUpperRight ) || RayBeginsInBox;
	if( h.bHit && ( bExitHit ? !bHitBackFaces : !bHitFrontFaces ) ) {
		h.bHit = false;
	}

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void BoxGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3( 0.0, 0.0, 0.0 );
	radius = Vector3Ops::Magnitude( Vector3( dWidth/2, dHeight/2, dDepth/2 ) );
}

BoundingBox BoxGeometry::GenerateBoundingBox() const
{
	return BoundingBox( Point3( -dWidthOV2, -dHeightOV2, -dDepthOV2 ),
						Point3( dWidthOV2, dHeightOV2, dDepthOV2 ) );
	
}

void BoxGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// The caller uses GetArea() as the reciprocal position PDF, so face
	// selection must be proportional to face area, not uniform by face.
	// Uniform face selection over-samples thin sides and under-samples broad
	// faces, which biases any area-light estimator that assumes 1/GetArea().
	const Scalar faceAreas[6] = {
		dHeight*dDepth, dHeight*dDepth,
		dWidth*dDepth, dWidth*dDepth,
		dWidth*dHeight, dWidth*dHeight
	};
	const Scalar totalArea = faceAreas[0] + faceAreas[1] + faceAreas[2] +
		faceAreas[3] + faceAreas[4] + faceAreas[5];
	const Scalar sample = prand.z * totalArea;
	Scalar cumulativeArea = 0;
	int idx = 5;
	for( int i=0; i<5; ++i ) {
		cumulativeArea += faceAreas[i];
		if( sample < cumulativeArea ) {
			idx = i;
			break;
		}
	}

	Point3 pt;
	switch( idx ) {
		default:
		case 0:
			// -X face
			pt = Point3( -dWidthOV2, prand.x*dHeight-dHeightOV2, prand.y*dDepth-dDepthOV2 );
			break;
		case 1:
			// +X face
			pt = Point3( dWidthOV2, prand.x*dHeight-dHeightOV2, prand.y*dDepth-dDepthOV2 );
			break;
		case 2:
			// -Y face
			pt = Point3( prand.x*dWidth-dWidthOV2, -dHeightOV2, prand.y*dDepth-dDepthOV2 );
			break;
		case 3:
			// +Y face
			pt = Point3( prand.x*dWidth-dWidthOV2, dHeightOV2, prand.y*dDepth-dDepthOV2 );
			break;
		case 4:
			// -Z face
			pt = Point3( prand.x*dWidth-dWidthOV2, prand.y*dHeight-dHeightOV2, -dDepthOV2 );
			break;
		case 5:
			// +Z face
			pt = Point3( prand.x*dWidth-dWidthOV2, prand.y*dHeight-dHeightOV2, dDepthOV2 );
			break;
	}

	if( point ) {
		*point = pt;
	}

	if( normal ) {
		switch( idx ) {
			default:
			case 0:
				*normal = Vector3( -1.0, 0.0, 0.0 );
				break;
			case 1:
				*normal = Vector3( 1.0, 0.0, 0.0 );
				break;
			case 2:
				*normal = Vector3( 0.0, -1.0, 0.0 );
				break;
			case 3:
				*normal = Vector3( 0.0, 1.0, 0.0 );
				break;
			case 4:
				*normal = Vector3( 0.0, 0.0, -1.0 );
				break;
			case 5:
				*normal = Vector3( 0.0, 0.0, 1.0 );
				break;
		}
	}

	if( coord ) {
		switch( idx ) {
			default:
			case 0:
				// -X face: same as IntersectRay case 0
				*coord = Point2( (pt.z+dDepthOV2)*dOVDepth, 1.0 - (pt.y+dHeightOV2)*dOVHeight );
				break;
			case 1:
				// +X face: same as IntersectRay case 1
				*coord = Point2( 1.0 - (pt.z+dDepthOV2)*dOVDepth, 1.0 - (pt.y+dHeightOV2)*dOVHeight );
				break;
			case 2:
				// -Y face: same as IntersectRay case 2
				*coord = Point2( (pt.x+dWidthOV2)*dOVWidth, 1.0 - (pt.z+dDepthOV2)*dOVDepth );
				break;
			case 3:
				// +Y face: same as IntersectRay case 3
				*coord = Point2( (pt.x+dWidthOV2)*dOVWidth, (pt.z+dDepthOV2)*dOVDepth );
				break;
			case 4:
				// -Z face: same as IntersectRay case 4
				*coord = Point2( 1.0 - (pt.x+dWidthOV2)*dOVWidth, 1.0 - (pt.y+dHeightOV2)*dOVHeight );
				break;
			case 5:
				// +Z face: same as IntersectRay case 5
				*coord = Point2( (pt.x+dWidthOV2)*dOVWidth, 1.0 - (pt.y+dHeightOV2)*dOVHeight );
				break;
		}
	}
}

SurfaceDerivatives BoxGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;
	sd.dndu = Vector3( 0, 0, 0 );
	sd.dndv = Vector3( 0, 0, 0 );
	sd.valid = true;

	// Determine which face from the normal direction
	const Scalar ax = fabs( objSpaceNormal.x );
	const Scalar ay = fabs( objSpaceNormal.y );
	const Scalar az = fabs( objSpaceNormal.z );

	if( ax > ay && ax > az )
	{
		// X face
		if( objSpaceNormal.x > 0 ) {
			// +X face
			sd.dpdu = Vector3( 0, 0, -1 );
			sd.dpdv = Vector3( 0, 1, 0 );
			sd.uv = Point2( 1.0 - (objSpacePoint.z + dDepthOV2) * dOVDepth, 1.0 - (objSpacePoint.y + dHeightOV2) * dOVHeight );
		} else {
			// -X face
			sd.dpdu = Vector3( 0, 0, 1 );
			sd.dpdv = Vector3( 0, 1, 0 );
			sd.uv = Point2( (objSpacePoint.z + dDepthOV2) * dOVDepth, 1.0 - (objSpacePoint.y + dHeightOV2) * dOVHeight );
		}
	}
	else if( ay > az )
	{
		// Y face — swap (u, v) so (dpdu × dpdv, n) is right-handed per
		// docs/GEOMETRY_DERIVATIVES.md.
		if( objSpaceNormal.y > 0 ) {
			// +Y face
			sd.dpdu = Vector3( 0, 0, 1 );
			sd.dpdv = Vector3( 1, 0, 0 );
			sd.uv = Point2( (objSpacePoint.z + dDepthOV2) * dOVDepth, (objSpacePoint.x + dWidthOV2) * dOVWidth );
		} else {
			// -Y face
			sd.dpdu = Vector3( 0, 0, -1 );
			sd.dpdv = Vector3( 1, 0, 0 );
			sd.uv = Point2( 1.0 - (objSpacePoint.z + dDepthOV2) * dOVDepth, (objSpacePoint.x + dWidthOV2) * dOVWidth );
		}
	}
	else
	{
		// Z face
		if( objSpaceNormal.z > 0 ) {
			// +Z face
			sd.dpdu = Vector3( 1, 0, 0 );
			sd.dpdv = Vector3( 0, 1, 0 );
			sd.uv = Point2( (objSpacePoint.x + dWidthOV2) * dOVWidth, 1.0 - (objSpacePoint.y + dHeightOV2) * dOVHeight );
		} else {
			// -Z face
			sd.dpdu = Vector3( -1, 0, 0 );
			sd.dpdv = Vector3( 0, 1, 0 );
			sd.uv = Point2( 1.0 - (objSpacePoint.x + dWidthOV2) * dOVWidth, 1.0 - (objSpacePoint.y + dHeightOV2) * dOVHeight );
		}
	}

	return sd;
}

Scalar BoxGeometry::GetArea( ) const
{
	const Scalar faceA = dWidth*dHeight;
	const Scalar faceB = dWidth*dDepth;
	const Scalar faceC = dHeight*dDepth;

	return faceA*2 + faceB*2 + faceC*2;
}

static const unsigned int WIDTH_ID = 100;
static const unsigned int HEIGHT_ID = 101;
static const unsigned int DEPTH_ID = 102;

IKeyframeParameter* BoxGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "width" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), WIDTH_ID );
	} else if( name == "height" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), HEIGHT_ID );
	} else if( name == "depth" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), DEPTH_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void BoxGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case WIDTH_ID:
		{
			dWidth = *(Scalar*)val.getValue();
			dWidthOV2 = dWidth * 0.5;
		}
		break;
	case HEIGHT_ID:
		{
			dHeight = *(Scalar*)val.getValue();
			dHeightOV2 = dHeight * 0.5;
		}
		break;
	case DEPTH_ID:
		{
			dDepth = *(Scalar*)val.getValue();
			dDepthOV2 = dDepth * 0.5;
		}
		break;
	}
}

void BoxGeometry::RegenerateData( )
{
	if( dWidth > 0 ) {
		dOVWidth = 1.0 / dWidth;
	} else {
		GlobalLog()->PrintSourceError( "BoxGeometry:: Width is 0", __FILE__, __LINE__ );
	}

	if( dHeight > 0 ) {
		dOVHeight = 1.0 / dHeight;
	} else {
		GlobalLog()->PrintSourceError( "BoxGeometry:: Height is 0", __FILE__, __LINE__ );
	}

	if( dDepth > 0 ) {
		dOVDepth = 1.0 / dDepth;
	} else {
		GlobalLog()->PrintSourceError( "BoxGeometry:: Depth is 0", __FILE__, __LINE__ );
	}
}
