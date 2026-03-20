//////////////////////////////////////////////////////////////////////
//
//  RayBoxIntersection.cpp - Implements a ray box intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"
#include "../Utilities/Profiling.h"

namespace RISE
{

	//
	// Branchless slab method (Williams et al. 2004) using precomputed
	// inverse direction and sign arrays stored in the Ray.
	//
	// This replaces the original 18-branch implementation with one that
	// uses 6 multiplies, 0 divisions, and far fewer branches.
	// The precomputed invDir handles the dir~=0 case naturally via
	// IEEE 754 infinities.
	//

#define SIDE_X0 0
#define SIDE_X1 1
#define SIDE_Y0 2
#define SIDE_Y1 3
#define SIDE_Z0 4
#define SIDE_Z1 5

	void RayBoxIntersection( const Ray& ray, BOX_HIT& hit, const Point3& ll, const Point3& ur )
	{
		RISE_PROFILE_INC(nBoxIntersectionTests);
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;
		hit.dRange2 = RISE_INFINITY;

		// Store bounds as an array so we can index by sign
		const Scalar boundsX[2] = { ll.x, ur.x };
		const Scalar boundsY[2] = { ll.y, ur.y };
		const Scalar boundsZ[2] = { ll.z, ur.z };

		// X slab
		Scalar tmin  = (boundsX[    ray.sign[0]] - ray.origin.x) * ray.invDir.x;
		Scalar tmax  = (boundsX[1 - ray.sign[0]] - ray.origin.x) * ray.invDir.x;
		int sideMin  = ray.sign[0] ? SIDE_X1 : SIDE_X0;
		int sideMax  = ray.sign[0] ? SIDE_X0 : SIDE_X1;

		// Y slab
		const Scalar tymin = (boundsY[    ray.sign[1]] - ray.origin.y) * ray.invDir.y;
		const Scalar tymax = (boundsY[1 - ray.sign[1]] - ray.origin.y) * ray.invDir.y;

		if( tmin > tymax || tymin > tmax ) {
			return;
		}

		if( tymin > tmin ) {
			tmin = tymin;
			sideMin = ray.sign[1] ? SIDE_Y1 : SIDE_Y0;
		}
		if( tymax < tmax ) {
			tmax = tymax;
			sideMax = ray.sign[1] ? SIDE_Y0 : SIDE_Y1;
		}

		// Z slab
		const Scalar tzmin = (boundsZ[    ray.sign[2]] - ray.origin.z) * ray.invDir.z;
		const Scalar tzmax = (boundsZ[1 - ray.sign[2]] - ray.origin.z) * ray.invDir.z;

		if( tmin > tzmax || tzmin > tmax ) {
			return;
		}

		if( tzmin > tmin ) {
			tmin = tzmin;
			sideMin = ray.sign[2] ? SIDE_Z1 : SIDE_Z0;
		}
		if( tzmax < tmax ) {
			tmax = tzmax;
			sideMax = ray.sign[2] ? SIDE_Z0 : SIDE_Z1;
		}

		// If both distances are negative, the box is behind the ray
		if( tmin < 0 && tmax < 0 ) {
			return;
		}

		hit.bHit = true;
		RISE_PROFILE_INC(nBoxIntersectionHits);

		if( tmin > 0 )
		{
			hit.dRange = tmin;
			hit.dRange2 = tmax;
			hit.sideA = sideMin;
			hit.sideB = sideMax;
		}
		else
		{
			hit.dRange = tmax;
			hit.dRange2 = tmin;
			hit.sideA = sideMax;
			hit.sideB = sideMin;
		}
	}

}
