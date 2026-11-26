//////////////////////////////////////////////////////////////////////
//
//  RayCylinderIntersection.cpp - Implements a ray cylinder intersections
//  These are taken from ggLibrary
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 13, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"
#include "../Functions/Polynomial.h"

#include "math.h"			// for sqrt

namespace RISE
{

	void RayXCylinderIntersection( const Ray& ray, HIT& hit, const Scalar xMin,
										const Scalar xMax, const Scalar yCenter, const Scalar zCenter, const Scalar radius, bool& bHitFarSide )
	{
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;

		const Vector3& v = ray.Dir();
		Scalar a = v.y * v.y + v.z * v.z;

		// Ray parallel to the cylinder axis (a==0): it holds a constant distance
		// from the axis and never crosses the side wall -> miss.  Mirrors
		// IntersectCappedSolid's `A > NEARZERO`; without it inva=1/a=inf makes
		// t=NaN, and the NaN ranges slip past every comparison as a false hit.
		if( a < NEARZERO ) {
			return;
		}

		const Point3&  o = ray.origin;
		// Scale-relative self-intersection floor -- same floor as
		// RaySphereIntersection (see its note) and the capped path's
		// IntersectCappedSolid: a ray published from a hit on this wall
		// that crosses it has a ~1e-12 self root the fixed NEARZERO gate
		// let through.  Measured 2026-09-05 (debt 25 sibling audit): an
		// open unit tube seen side-on, carrying a full-sphere transmissive
		// weave lit from behind, read PT 0.36x of BDPT.
		const Scalar tMin = NEARZERO * ( Scalar(1) + fabs( o.x ) + fabs( o.y ) + fabs( o.z ) + fabs( radius ) );
		Scalar oc1 = o.y - yCenter;
		Scalar oc2 = o.z - zCenter;
		Scalar b = v.y * oc1 + v.z * oc2;
		Scalar c = oc1 * oc1 + oc2 * oc2 - radius*radius;
		Scalar rad = b*b - a*c;

		if (rad < 0.0) {
			return;
		}

		Scalar inva = 1/a;
		Scalar sqrad = sqrt(rad);
		Scalar t = (-b - sqrad) * inva;
		Scalar t2 = t;
		Scalar x = ray.origin.x + t * ray.Dir().x;

		// Check if the first hit is ok... 
		bool	bFirstHit = true;
		if ( x < xMin || x > xMax || t < tMin)
		{
			// First hit no, good, that means we are inside the cylinder now
			// for there to be a hit
			t += 2 * sqrad * inva;
			x = ray.origin.x + t * ray.Dir().x;
			bFirstHit = false;
		}

		// Check if the ray hits the cylinder, no dice here either, there's no hit
		if ( x < xMin || x > xMax || t < tMin) {
			return;
		}
	    
		hit.bHit = true;
		hit.dRange = t;
		hit.dRange2 = bFirstHit ? t2+2*sqrad*inva : 0.0;

		bHitFarSide = !bFirstHit;
	}

	void RayYCylinderIntersection( const Ray& ray, HIT& hit, const Scalar yMin,
										const Scalar yMax, const Scalar xCenter, const Scalar zCenter,
										const Scalar radius, bool& bHitFarSide )
	{
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;

		const Vector3& v = ray.Dir();
		Scalar a = v.z * v.z + v.x * v.x;

		// Ray parallel to the cylinder axis (a==0): it holds a constant distance
		// from the axis and never crosses the side wall -> miss.  Mirrors
		// IntersectCappedSolid's `A > NEARZERO`; without it inva=1/a=inf makes
		// t=NaN, and the NaN ranges slip past every comparison as a false hit.
		if( a < NEARZERO ) {
			return;
		}

		const Point3&  o = ray.origin;
		// Scale-relative self-intersection floor -- same floor as
		// RaySphereIntersection (see its note) and the capped path's
		// IntersectCappedSolid: a ray published from a hit on this wall
		// that crosses it has a ~1e-12 self root the fixed NEARZERO gate
		// let through.  Measured 2026-09-05 (debt 25 sibling audit): an
		// open unit tube seen side-on, carrying a full-sphere transmissive
		// weave lit from behind, read PT 0.36x of BDPT.
		const Scalar tMin = NEARZERO * ( Scalar(1) + fabs( o.x ) + fabs( o.y ) + fabs( o.z ) + fabs( radius ) );
		Scalar oc1 = o.z - zCenter;
		Scalar oc2 = o.x - xCenter;
		Scalar b = v.z * oc1 + v.x * oc2;
		Scalar c = oc1 * oc1 + oc2 * oc2 - radius*radius;

		Scalar rad = b*b - a*c;

		if (rad < 0.0) {
			return;
		}

		Scalar inva = 1/a;
		Scalar sqrad = sqrt(rad);
		Scalar t = (-b - sqrad) * inva;
		Scalar t2 = t;
		Scalar y = ray.origin.y + t * ray.Dir().y;

		// Check if the first hit is ok... 
		bool	bFirstHit = true;
		if ( y < yMin || y > yMax || t < tMin)
		{
			// First hit no, good, that means we are inside the cylinder now
			// for there to be a hit
			t += 2 * sqrad * inva;
			y = ray.origin.y + t * ray.Dir().y;
			bFirstHit = false;
		}

		// Check if the ray hits the cylinder, no dice here either, there's no hit
		if ( y < yMin || y > yMax || t < tMin) {
			return;
		}
	    
		hit.bHit = true;
		hit.dRange = t;
		hit.dRange2 = bFirstHit ? t2+2*sqrad*inva : 0.0;

		bHitFarSide = !bFirstHit;
	}

	void RayZCylinderIntersection( const Ray& ray, HIT& hit, const Scalar zMin,
										const Scalar zMax, const Scalar xCenter, const Scalar yCenter,
										const Scalar radius, bool& bHitFarSide )
	{
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;

		const Vector3& v = ray.Dir();
		Scalar a = v.x * v.x + v.y * v.y;

		// Ray parallel to the cylinder axis (a==0): it holds a constant distance
		// from the axis and never crosses the side wall -> miss.  Mirrors
		// IntersectCappedSolid's `A > NEARZERO`; without it inva=1/a=inf makes
		// t=NaN, and the NaN ranges slip past every comparison as a false hit.
		if( a < NEARZERO ) {
			return;
		}

		const Point3&  o = ray.origin;
		// Scale-relative self-intersection floor -- same floor as
		// RaySphereIntersection (see its note) and the capped path's
		// IntersectCappedSolid: a ray published from a hit on this wall
		// that crosses it has a ~1e-12 self root the fixed NEARZERO gate
		// let through.  Measured 2026-09-05 (debt 25 sibling audit): an
		// open unit tube seen side-on, carrying a full-sphere transmissive
		// weave lit from behind, read PT 0.36x of BDPT.
		const Scalar tMin = NEARZERO * ( Scalar(1) + fabs( o.x ) + fabs( o.y ) + fabs( o.z ) + fabs( radius ) );
		Scalar oc1 = o.x - xCenter;
		Scalar oc2 = o.y - yCenter;
		Scalar b = v.x * oc1 + v.y * oc2;
		Scalar c = oc1 * oc1 + oc2 * oc2 - radius*radius;

		Scalar rad = b*b - a*c;

		if (rad < 0.0) {
			return;
		}

		Scalar inva = 1/a;
		Scalar sqrad = sqrt(rad);
		Scalar t = (-b - sqrad) * inva;
		Scalar t2 = t;
		Scalar z = ray.origin.z + t * ray.Dir().z;

		// Check if the first hit is ok... 
		bool	bFirstHit = true;
		if ( z < zMin || z > zMax || t < tMin)
		{
			// First hit no, good, that means we are inside the cylinder now
			// for there to be a hit
			t += 2 * sqrad * inva;
			z = ray.origin.z + t * ray.Dir().z;
			bFirstHit = false;
		}

		// Check if the ray hits the cylinder, no dice here either, there's no hit
		if ( z < zMin || z > zMax || t < tMin) {
			return;
		}
	    
		hit.bHit = true;
		hit.dRange = t;
		hit.dRange2 = bFirstHit ? t2+2*sqrad*inva : 0.0;

		bHitFarSide = !bFirstHit;
	}
}
