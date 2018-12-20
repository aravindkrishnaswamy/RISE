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

namespace RISE
{

	void RayBoxIntersection( const Ray& ray, BOX_HIT& hit, const Point3& ll, const Point3& ur )
	{
		hit.bHit = false;
		hit.dRange = INFINITY;
		hit.dRange2 = INFINITY;

	#define SIDE_X0 0
	#define SIDE_X1 1
	#define SIDE_Y0 2
	#define SIDE_Y1 3
	#define SIDE_Z0 4
	#define SIDE_Z1 5

		Scalar		fTMin=0, fTMax=INFINITY;
		int			nSideMin=SIDE_X0, nSideMax=SIDE_X1;

		Scalar		t;

		//
		// Start with the X-Axis
		//
		if( ray.dir.x < -NEARZERO )
		{
			t = (ll.x - ray.origin.x) / ray.dir.x;

			if( t < fTMin ) {
				return;		// No chance
			}

			if( t <= fTMax ) {
				fTMax = t;
				nSideMax = SIDE_X0;
			}

			t = (ur.x - ray.origin.x) / ray.dir.x;

			if( t >= fTMin ) {
				if( t > fTMax ) {
					return;	// No chance
				}

				fTMin = t;
				nSideMin = SIDE_X1;
			}
		}
		else
		{
			if( ray.dir.x > NEARZERO )
			{
				t = (ur.x - ray.origin.x ) / ray.dir.x;

				if( t < fTMin ) {
					return;	// No chance
				}

				if( t <= fTMax ) {
					fTMax = t;
					nSideMax = SIDE_X1;
				}

				t = (ll.x - ray.origin.x ) / ray.dir.x;

				if( t >= fTMin ) {
					if( t > fTMax ) {
						return;// No chance
					}

					fTMin = t;
					nSideMin = SIDE_X0;
				}
			}
			else
			{
				if( (ray.origin.x < ll.x) || (ray.origin.x > ur.x) ) {
					return;		// No chance
				}
			}
		}


		//
		// And now the Y axis
		//
		
		if( ray.dir.y < -NEARZERO )
		{
			t = (ll.y - ray.origin.y) / ray.dir.y;

			if( t < fTMin ) {
				return;			// no Chance
			}

			if( t <= (fTMax-NEARZERO) ) {
				fTMax = t;
				nSideMax = SIDE_Y0;
			}

			t = (ur.y - ray.origin.y) / ray.dir.y;

			if( t >= (fTMin+NEARZERO) ) {
				if( t > fTMax ) {
					return;				// no chance
				}

				fTMin = t;
				nSideMin = SIDE_Y1;
			}
		}
		else
		{
			if( ray.dir.y > NEARZERO )
			{
				t = (ur.y - ray.origin.y) / ray.dir.y;

				if( t < fTMin ) {
					return;			// no chance
				}

				if( t <= fTMax - NEARZERO ) {
					fTMax = t;
					nSideMax = SIDE_Y1;
				}

				t = (ll.y - ray.origin.y) / ray.dir.y;

				if( t >= (fTMin + NEARZERO) ) {
					if( t > fTMax ) {
						return;			// no chance
					}

					fTMin = t;
					nSideMin = SIDE_Y0;
				}
			}
			else
			{
				if( (ray.origin.y < ll.y) || (ray.origin.y > ur.y) ) {
					return;			// no chance
				}
			}
		}


		//
		// And finally the Z axis
		//
		
		if( ray.dir.z < -NEARZERO )
		{
			t = (ll.z - ray.origin.z) / ray.dir.z;

			if( t < fTMin ) {
				return;		// no chance
			}

			if( t <= (fTMax-NEARZERO) ) {
				fTMax = t;
				nSideMax = SIDE_Z0;
			}

			t = (ur.z - ray.origin.z) / ray.dir.z;

			if( t >= (fTMin+NEARZERO) )
			{
				if( t > fTMax ) {
					return;				// no chance
				}

				fTMin = t;
				nSideMin = SIDE_Z1;
			}
		}
		else
		{
			if( ray.dir.z > NEARZERO )
			{
				t = (ur.z - ray.origin.z) / ray.dir.z;

				if( t < fTMin ) {
					return;		// no chance
				}

				if( t <= (fTMax - NEARZERO) ) {
					fTMax = t;
					nSideMax = SIDE_Z1;
				}

				t = (ll.z - ray.origin.z) / ray.dir.z;

				if( t >= (fTMin + NEARZERO) ) {
					if( t > fTMax ) {
						return;			// no chance
					}

					fTMin = t;
					nSideMin = SIDE_Z0;
				}
			}
			else
			{
				if( (ray.origin.z < ll.z) || (ray.origin.z > ur.z) )
					return;			// no chance
			}
		}


		// Whew!  and if we make it to here, then there was an intersection, take
		// the smallest positive distance

		// If both distances are negative, then the box is behind the ray, and no intersection
		if( fTMin < 0 && fTMax < 0 ) {
			return;
		}

		hit.bHit = true;

		if( fTMin > 0 )
		{
			hit.dRange = fTMin;
			hit.dRange2 = fTMax;
			hit.sideA = nSideMin;					// side when entering
			hit.sideB = nSideMax;					// side when exiting
		}
		else
		{
			hit.dRange = fTMax;
			hit.dRange2 = fTMin;
			hit.sideA = nSideMax;					// side when entering
			hit.sideB = nSideMin;					// side when exiting
		}
	}

}
