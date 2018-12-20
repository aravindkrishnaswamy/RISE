//////////////////////////////////////////////////////////////////////
//
//  RayBilinearPatchIntersection.cpp - Implements a ray-bilinear
//    patch intersection as described by Ramsey et al. in 
//    "Ray Bilinear Patch Intersections", JGT 04 and sample source
//    here: http://www.cs.utah.edu/~ramsey/bp/bilinear.cc
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 18, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"
#include "../Functions/Polynomial.h"
#include "../Utilities/GeometricUtilities.h"

namespace RISE
{

	// Choose between the best denominator to avoid singularities
	// and to get the most accurate root possible
	inline Scalar getu( 
		const Scalar v,
		const Scalar M1,
		const Scalar M2, 
		const Scalar J1,
		const Scalar J2,
		const Scalar K1, 
		const Scalar K2,
		const Scalar R1, 
		const Scalar R2
		)
	{
		const Scalar denom = (v*(M1-M2)+J1-J2);
		const Scalar d2 = (v*M1+J1);
		if(fabs(denom) > fabs(d2)) { // which denominator is bigger
			return (v*(K2-K1)+R2-R1)/denom;
		}

		return -(v*K1+R1)/d2;
	}

	// Compute t with the best accuracy by using the component
	// of the direction that is largest
	Scalar computet(
		const Ray& ray, 
		const Point3& srfpos
		)
	{
		if(fabs(ray.dir.x) >= fabs(ray.dir.y) && fabs(ray.dir.x) >= fabs(ray.dir.z)) {
			return (srfpos.x - ray.origin.x) / ray.dir.x;
		} else if(fabs(ray.dir.y) >= fabs(ray.dir.z)) {
			return (srfpos.y - ray.origin.y) / ray.dir.y;
		} else {
			return (srfpos.z - ray.origin.z) / ray.dir.z;
		}
	}


	void RayBilinearPatchIntersection( 
		const Ray& ray, 
		BILINEAR_HIT& hit,
		const BilinearPatch& patch
		)
	{
		hit.bHit = false;
		hit.dRange = INFINITY;
		hit.dRange2 = INFINITY;

		//
		// Equation of the patch
		// 
		// P(u,v) = (1-u)(1-v)*patch.pts[0] + (1-u)v*patch.pts[1] + u(1-v)*patch.pts[2] + uv*patch.pts[3]
		//

		// Variables for substitution
		// a = pts[3] - pts[2] - pts[1] + pts[0]
		// b = pts[2] - pts[0]
		// c = pts[1] - pts[0]
		// d = pts[0]

		// Find a w.r.t. x, y, z
		const Scalar ax = patch.pts[3].x - patch.pts[2].x - patch.pts[1].x + patch.pts[0].x;
		const Scalar ay = patch.pts[3].y - patch.pts[2].y - patch.pts[1].y + patch.pts[0].y;
		const Scalar az = patch.pts[3].z - patch.pts[2].z - patch.pts[1].z + patch.pts[0].z;


		// Find b w.r.t. x, y, z
		const Scalar bx = patch.pts[2].x - patch.pts[0].x;
		const Scalar by = patch.pts[2].y - patch.pts[0].y;
		const Scalar bz = patch.pts[2].z - patch.pts[0].z;

		// Find c w.r.t. x, y, z
		const Scalar cx = patch.pts[1].x - patch.pts[0].x;
		const Scalar cy = patch.pts[1].y - patch.pts[0].y;
		const Scalar cz = patch.pts[1].z - patch.pts[0].z;


		const Scalar rx = ray.origin.x;
		const Scalar ry = ray.origin.y;
		const Scalar rz = ray.origin.z;

		// Retrieve the xyz of the q part of ray
		const Scalar qx = ray.dir.x;
		const Scalar qy = ray.dir.y;
		const Scalar qz = ray.dir.z;

		// Find d w.r.t. x, y, z - subtracting r just after  
		const Scalar dx = patch.pts[0].x - rx;
		const Scalar dy = patch.pts[0].y - ry;
		const Scalar dz = patch.pts[0].z - rz;

		// Find A1 and A2
		const Scalar A1 = ax*qz - az*qx;
		const Scalar A2 = ay*qz - az*qy;

		// Find B1 and B2
		const Scalar B1 = bx*qz - bz*qx;
		const Scalar B2 = by*qz - bz*qy;

		// Find C1 and C2
		const Scalar C1 = cx*qz - cz*qx;
		const Scalar C2 = cy*qz - cz*qy;

		// Find D1 and D2
		const Scalar D1 = dx*qz - dz*qx;
		const Scalar D2 = dy*qz - dz*qy;

		Scalar coeff[3] = {0};
		coeff[0] = A2*C1 - A1*C2;
		coeff[1] = A2*D1 - A1*D2 + B2*C1 -B1*C2;
		coeff[2] = B2*D1 - B1*D2;

		hit.u = hit.v = hit.dRange = -2;
		
		Scalar sol[2] = {0};
		const int numSol = Polynomial::SolveQuadricWithinRange( coeff, sol, -NEARZERO, 1.0+NEARZERO ); 

		switch( numSol )
		{
		case 0:
			break;			 // no solutions found
		case 1:
			{
				hit.u = getu(sol[0],A2,A1,B2,B1,C2,C1,D2,D1);
				hit.v = sol[0];
				
				const Point3 pos1 = GeometricUtilities::EvaluateBilinearPatchAt( patch, hit.u, hit.v );
				hit.dRange = computet(ray,pos1);

				if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > 0 ) {
					hit.bHit = true;
				}
			}
			break;
		case 2: // two solutions found
			{
				hit.v = sol[0];
				hit.u = getu(sol[0],A2,A1,B2,B1,C2,C1,D2,D1);
				
				const Point3 pos1 = GeometricUtilities::EvaluateBilinearPatchAt( patch, hit.u, hit.v );
				hit.dRange = computet(ray,pos1); 

				if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > 0 ) {
					hit.bHit = true;

					const Scalar u = getu(sol[1],A2,A1,B2,B1,C2,C1,D2,D1);
					if( u < 1+NEARZERO && u > NEARZERO ) {
						const Point3 pos2 = GeometricUtilities::EvaluateBilinearPatchAt( patch, u, sol[1] );
						const Scalar t2 = computet(ray,pos2);
						if(t2 < 0 || hit.dRange < t2) { // t2 is bad or t1 is better
							return;
						}
						// other wise both t2 > 0 and t2 < t1
						hit.v = sol[1];
						hit.u = u;
						hit.dRange = t2;					
					}
				}
				else // doesn't fit in the root - try other one
				{
					hit.u = getu(sol[1],A2,A1,B2,B1,C2,C1,D2,D1);
					hit.v = sol[1];
					const Point3 pos1 = GeometricUtilities::EvaluateBilinearPatchAt( patch, hit.u, hit.v );
					hit.dRange = computet(ray,pos1);

					if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > 0 ) {
						hit.bHit = true;
					}
				}
			}
			break;
		};
	}
}
