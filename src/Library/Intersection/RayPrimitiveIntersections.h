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

		HIT( ) : bHit( false ), dRange( INFINITY ), dRange2( INFINITY ) {}
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
