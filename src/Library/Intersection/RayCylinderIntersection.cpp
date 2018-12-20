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
		hit.dRange = INFINITY;

		const Vector3& v = ray.dir;
		Scalar a = v.y * v.y + v.z * v.z;

		const Point3&  o = ray.origin;
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
		Scalar x = ray.origin.x + t * ray.dir.x;

		// Check if the first hit is ok... 
		bool	bFirstHit = true;
		if ( x < xMin || x > xMax || t < NEARZERO)
		{
			// First hit no, good, that means we are inside the cylinder now
			// for there to be a hit
			t += 2 * sqrad * inva;
			x = ray.origin.x + t * ray.dir.x;
			bFirstHit = false;
		}

		// Check if the ray hits the cylinder, no dice here either, there's no hit
		if ( x < xMin || x > xMax || t < NEARZERO) {
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
		hit.dRange = INFINITY;

		const Vector3& v = ray.dir;
		Scalar a = v.z * v.z + v.x * v.x;

		const Point3&  o = ray.origin;
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
		Scalar y = ray.origin.y + t * ray.dir.y;

		// Check if the first hit is ok... 
		bool	bFirstHit = true;
		if ( y < yMin || y > yMax || t < NEARZERO)
		{
			// First hit no, good, that means we are inside the cylinder now
			// for there to be a hit
			t += 2 * sqrad * inva;
			y = ray.origin.y + t * ray.dir.y;
			bFirstHit = false;
		}

		// Check if the ray hits the cylinder, no dice here either, there's no hit
		if ( y < yMin || y > yMax || t < NEARZERO) {
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
		hit.dRange = INFINITY;

		const Vector3& v = ray.dir;
		Scalar a = v.x * v.x + v.y * v.y;

		const Point3&  o = ray.origin;
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
		Scalar z = ray.origin.z + t * ray.dir.z;

		// Check if the first hit is ok... 
		bool	bFirstHit = true;
		if ( z < zMin || z > zMax || t < NEARZERO)
		{
			// First hit no, good, that means we are inside the cylinder now
			// for there to be a hit
			t += 2 * sqrad * inva;
			z = ray.origin.z + t * ray.dir.z;
			bFirstHit = false;
		}

		// Check if the ray hits the cylinder, no dice here either, there's no hit
		if ( z < zMin || z > zMax || t < NEARZERO) {
			return;
		}
	    
		hit.bHit = true;
		hit.dRange = t;
		hit.dRange2 = bFirstHit ? t2+2*sqrad*inva : 0.0;

		bHitFarSide = !bFirstHit;
	}
}
