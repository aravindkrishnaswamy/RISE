//////////////////////////////////////////////////////////////////////
//
//  RayQuadricIntersection.cpp - Implements a ray ellipsoid intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 7, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"

#include "math.h"			// for sqrt

namespace RISE
{

	void RayQuadricIntersection( const Ray& ray, HIT& hit, const Point3& center, const Matrix4& Q )
	{
		hit.bHit = false;
		hit.dRange = INFINITY;

		const Scalar qa = Q._00;
		const Scalar qb = Q._01 + Q._10;
		const Scalar qc = Q._02 + Q._20;
		const Scalar qd = Q._03 + Q._30;
		const Scalar qe = Q._11;
		const Scalar qf = Q._12 + Q._21;
		const Scalar qg = Q._13 + Q._31;
		const Scalar qh = Q._22;
		const Scalar qi = Q._23 + Q._32;
		const Scalar qj = Q._33;

		const Scalar k2 =	ray.dir.x * (ray.dir.x*qa + ray.dir.y*qb) + 
							ray.dir.y * (ray.dir.y*qe + ray.dir.z*qf) + 
							ray.dir.z * (ray.dir.z*qh + ray.dir.z*qc);

		const Scalar k1 =	ray.dir.x * ((ray.origin.x*2.0*qa + ray.origin.y*qb + ray.origin.z*qc) + qd) + 
							ray.dir.y * ((ray.origin.x*qb + ray.origin.y*2.0*qe + ray.origin.z*qf) + qg) + 
							ray.dir.z * ((ray.origin.x*qc + ray.origin.y*qg + ray.origin.z*2.0*qh) + qi);

		const Scalar k0 =	ray.origin.x * (ray.origin.x*qa + ray.origin.y*qb + ray.origin.z*qc + qd) + 
							ray.origin.y * (                  ray.origin.y*qe + ray.origin.z*qf + qg) + 
							ray.origin.z * (                                    ray.origin.z*qh + qi) + 
							qj;

		Scalar discriminant = k1*k1 - 4.0*k2*k0;

		hit.bHit = (discriminant >= 0.0);

		if( hit.bHit ) {
			discriminant = sqrt( discriminant );
			hit.dRange = (-k1 - discriminant) / (2.0*k2);
			hit.dRange2 = (-k1 + discriminant) / (2.0*k2);
		}
	}

}
