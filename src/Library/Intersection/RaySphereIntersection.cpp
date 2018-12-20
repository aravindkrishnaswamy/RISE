//////////////////////////////////////////////////////////////////////
//
//  RaySphereIntersection.cpp - Implements a ray sphere intersection
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
#include "../Functions/Polynomial.h"

#include "math.h"			// for sqrt

namespace RISE
{

	void RaySphereIntersection( const Ray& ray, HIT& hit, const Scalar radius, const Point3& center )
	{
		hit.bHit = false;
		hit.dRange = INFINITY;

		Vector3 vTemp = Vector3Ops::mkVector3( ray.origin, center );

		Scalar		fSqrRadius = radius*radius;
		Scalar		coeffs[3] = {0};

		coeffs[0] = Vector3Ops::SquaredModulus( ray.dir );
		coeffs[1] = 2 * (ray.dir.x * vTemp.x + ray.dir.y * vTemp.y + ray.dir.z * vTemp.z);
		coeffs[2] = Vector3Ops::SquaredModulus( vTemp ) - (fSqrRadius);

		Scalar		solutions[2] = {0};

		int			numSolutions = Polynomial::SolveQuadric( coeffs, solutions );

		switch( numSolutions )
		{
		default:
		case 0:
			return;

		case 1:
			if( solutions[0] > NEARZERO )
			{
				hit.bHit = true;
				hit.dRange = hit.dRange2 = solutions[0];
			}
			break;
		case 2:
			if( solutions[0] > NEARZERO && solutions[1] > NEARZERO)
			{
				if( solutions[0] < solutions[1] )
				{
					hit.bHit = true;
					hit.dRange = solutions[0];
					hit.dRange2 = solutions[1];
				}

				if( solutions[1] < solutions[0] )
				{
					hit.bHit = true;
					hit.dRange = solutions[1];
					hit.dRange2 = solutions[0];
				}	
			}
			else if( solutions[0] > NEARZERO )
			{
				hit.bHit = true;
				hit.dRange = solutions[0];
				hit.dRange2 = 0.0; //solutions[0];
			}
			else if( solutions[1] > NEARZERO )
			{
				hit.bHit = true;
				hit.dRange = solutions[1];
				hit.dRange2 = 0.0; //solutions[1];
			}
			break;
		}
	}


	extern void RaySphereIntersection( const Ray& ray, HIT& hit, const Scalar radius )
	{
		hit.bHit = false;
		hit.dRange = INFINITY;

		Scalar		fSqrRadius = radius*radius;
		Scalar		coeffs[3] = {0};

		coeffs[0] = Vector3Ops::SquaredModulus( ray.dir );
		coeffs[1] = 2 * (ray.dir.x * ray.origin.x + ray.dir.y * ray.origin.y + ray.dir.z * ray.origin.z);
		Vector3 d = Vector3Ops::mkVector3( ray.origin, Point3(0, 0, 0) );
		coeffs[2] = Vector3Ops::SquaredModulus( d ) - (fSqrRadius);

		Scalar		solutions[2] = {0};

		int			numSolutions = Polynomial::SolveQuadric( coeffs, solutions );

		switch( numSolutions )
		{
		default:
		case 0:
			return;

		case 1:
			if( solutions[0] > NEARZERO )
			{
				hit.bHit = true;
				hit.dRange = hit.dRange2 = solutions[0];
			}
			break;
		case 2:
			if( solutions[0] > NEARZERO && solutions[1] > NEARZERO)
			{
				if( solutions[0] < solutions[1] )
				{
					hit.bHit = true;
					hit.dRange = solutions[0];
					hit.dRange2 = solutions[1];
				}

				if( solutions[1] < solutions[0] )
				{
					hit.bHit = true;
					hit.dRange = solutions[1];
					hit.dRange2 = solutions[0];
				}	
			}
			else if( solutions[0] > NEARZERO )
			{
				hit.bHit = true;
				hit.dRange = solutions[0];
				hit.dRange2 = 0.0; //solutions[0];
			}
			else if( solutions[1] > NEARZERO )
			{
				hit.bHit = true;
				hit.dRange = solutions[1];
				hit.dRange2 = 0.0; //solutions[1];
			}
			break;
		}
	}

}


