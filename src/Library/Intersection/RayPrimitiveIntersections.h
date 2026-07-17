//////////////////////////////////////////////////////////////////////
//
//  RayPrimitiveIntersections.h - Defines a bunch of ray/primitive
//  intersections.  These are all implemented in their respective
//  cpp files
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  These intersections return after the FIRST intersection
//			   This is done as an optimization
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAY_PRIMITIVE_INTERSECTIONS_
#define RAY_PRIMITIVE_INTERSECTIONS_

#include "../Utilities/Ray.h"
#include "../Polygon.h"
#include "../Interfaces/IFunction2D.h"

namespace RISE
{
	struct HIT
	{
		bool		bHit;
		Scalar		dRange;
		Scalar		dRange2;

		HIT( ) : bHit( false ), dRange( RISE_INFINITY ), dRange2( RISE_INFINITY ) {}
	};

	struct BOX_HIT : public HIT
	{
		int			sideA;
		int			sideB;

		BOX_HIT( ) : sideA( 0 ), sideB( 0 ) {}
	};

	struct TRIANGLE_HIT : public HIT
	{
		Scalar		alpha;
		Scalar		beta;

		TRIANGLE_HIT( ) : alpha( 0 ), beta( 0 ) {}
	};

	//! GUI render modes P1 (wireframe view mode): closest point on a
	//! segment, then on the nearest of a triangle's three edges to a
	//! point on the triangle.  Object-space; the mesh intersectors store
	//! the result on the intersection record and Object::IntersectRay
	//! transforms it to world space alongside ptIntersection (see
	//! RayIntersectionGeometric::bWantsWireEdgeInfo).
	//!
	//! KNOWN LIMITATION (documented, deliberately unfixed): the nearest-of-
	//! three-edges choice happens in OBJECT space; under a non-uniform
	//! object scale the object-space-nearest edge need not be the world-
	//! space-nearest, so wire lines near triangle corners can attach to the
	//! "wrong" (second-nearest) edge on non-uniformly-scaled meshes.  The
	//! chosen point itself still world-transforms exactly, so lines are
	//! merely mis-attributed near corners, never geometrically wrong on an
	//! edge.  A true fix needs all three candidate points on the (hot) hit
	//! record or transform access here -- both judged disproportionate for
	//! a diagnostic view mode.
	inline Point3 WireClosestPointOnSegment(
		const Point3& p,
		const Point3& a,
		const Point3& b )
	{
		const Vector3 ab = Vector3Ops::mkVector3( b, a );
		const Scalar len2 = Vector3Ops::SquaredModulus( ab );
		if( len2 <= 0.0 ) {
			return a;
		}
		Scalar t = Vector3Ops::Dot( Vector3Ops::mkVector3( p, a ), ab ) / len2;
		t = t < 0.0 ? 0.0 : ( t > 1.0 ? 1.0 : t );
		return Point3Ops::mkPoint3( a, ab * t );
	}

	inline Point3 WireClosestPointOnTriangleEdges(
		const Point3& p,
		const Point3& v0,
		const Point3& v1,
		const Point3& v2 )
	{
		const Point3 c0 = WireClosestPointOnSegment( p, v0, v1 );
		const Point3 c1 = WireClosestPointOnSegment( p, v1, v2 );
		const Point3 c2 = WireClosestPointOnSegment( p, v2, v0 );
		const Scalar d0 = Vector3Ops::SquaredModulus( Vector3Ops::mkVector3( p, c0 ) );
		const Scalar d1 = Vector3Ops::SquaredModulus( Vector3Ops::mkVector3( p, c1 ) );
		const Scalar d2 = Vector3Ops::SquaredModulus( Vector3Ops::mkVector3( p, c2 ) );
		// Seed the reduction from the first candidate, never from an
		// infinity sentinel (-ffast-math; see the repo threading/FP notes).
		Point3 best = c0;
		Scalar bestD2 = d0;
		if( d1 < bestD2 ) {
			best = c1;
			bestD2 = d1;
		}
		if( d2 < bestD2 ) {
			best = c2;
		}
		return best;
	}

	struct BEZIER_HIT : public HIT
	{
		Scalar		u;
		Scalar		v;

		BEZIER_HIT( ) : u( 0 ), v( 0 ) {}
	};

	struct BILINEAR_HIT : public HIT
	{
		Scalar		u;
		Scalar		v;

		BILINEAR_HIT( ) : u( 0 ), v( 0 ) {}
	};

	extern void RayTriangleIntersection(
		const Ray& ray,
		TRIANGLE_HIT& hit,
		const Point3& vPt1, 
		const Vector3& vEdgeA, 
		const Vector3& vEdgeB
		);

	extern void RayTriangleIntersectionWithDisplacement(
		const Ray& ray,
		TRIANGLE_HIT& hit,
		const Point3 (&coeff)[3],						// Three points of the triangle
		const Vector3 (&normals)[3],					// Three normals of the triangle
		const IFunction2D& displace,					// Displacement function
		const Scalar N,									// Subdivision amount
		const Scalar m,									// Maximum displacement below
		const Scalar M									// Maximum displacement above
		);

	extern void RaySphereIntersection( 
		const Ray& ray,
		HIT& hit,
		const Scalar radius,
		const Point3& center
		);

	extern void RaySphereIntersection( 
		const Ray& ray, 
		HIT& hit, 
		const Scalar radius
		);

	extern void RayQuadricIntersection( 
		const Ray& ray, 
		HIT& hit, 
		const Point3& center, 
		const Matrix4& Q 
		);

	extern void RayBoxIntersection( 
		const Ray& ray,
		BOX_HIT& hit, 
		const Point3& ll,
		const Point3& ur
		);

	extern void RayPlaneIntersection(
		const Ray& ray, 
		HIT& hit, 
		const Vector3& vPlaneNormal
		); 

	extern int RayPlaneIntersectionSimple(
		const Vector3& point, 
		const Vector3& vPlaneNormal,
		const Scalar planeD
		); 

	extern void RayTorusIntersection( 
		const Ray& ray, 
		HIT& hit, 
		const Scalar majorRadius, 
		const Scalar minorRadius, 
		const Scalar sqrP0
		);

	extern void RayXCylinderIntersection(
		const Ray& ray, 
		HIT& hit,
		const Scalar xMin,
		const Scalar xMax, 
		const Scalar yCenter, 
		const Scalar zCenter,
		const Scalar radius, 
		bool& bHitFarSide 
		);

	extern void RayYCylinderIntersection(
		const Ray& ray, 
		HIT& hit, 
		const Scalar yMin,
		const Scalar yMax,
		const Scalar xCenter, 
		const Scalar zCenter,
		const Scalar radius, 
		bool& bHitFarSide 
		);

	extern void RayZCylinderIntersection( 
		const Ray& ray, 
		HIT& hit,
		const Scalar zMin,
		const Scalar zMax, 
		const Scalar xCenter, 
		const Scalar yCenter,
		const Scalar radius, 
		bool& bHitFarSide 
		);

	extern void RayBezierPatchIntersection( 
		const Ray& ray, 
		BEZIER_HIT& hit,
		const BezierPatch& patch
		);

	extern void RayBilinearPatchIntersection( 
		const Ray& ray, 
		BILINEAR_HIT& hit,
		const BilinearPatch& patch
		);
}

#endif
