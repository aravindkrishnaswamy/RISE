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
		if(fabs(ray.Dir().x) >= fabs(ray.Dir().y) && fabs(ray.Dir().x) >= fabs(ray.Dir().z)) {
			return (srfpos.x - ray.origin.x) / ray.Dir().x;
		} else if(fabs(ray.Dir().y) >= fabs(ray.Dir().z)) {
			return (srfpos.y - ray.origin.y) / ray.Dir().y;
		} else {
			return (srfpos.z - ray.origin.z) / ray.Dir().z;
		}
	}


	void RayBilinearPatchIntersection( 
		const Ray& ray, 
		BILINEAR_HIT& hit,
		const BilinearPatch& patch
		)
	{
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;
		hit.dRange2 = RISE_INFINITY;

		// Self-intersection floor for `dRange`, scale-relative to the
		// coordinates actually involved rather than the fixed absolute
		// `NEARZERO` (1e-12).  A shadow or continuation ray whose origin
		// sits exactly on this patch (the common case: it just came from
		// a hit on this same quad) has a MATHEMATICALLY zero root at
		// t = 0, but `computet`'s (srfpos - ray.origin) numerator and the
		// (u, v) that produced `srfpos` both carry FP round-off that
		// scales with the MAGNITUDE of the patch/ray coordinates, not
		// with machine epsilon in absolute terms -- at world-scale
		// coordinates of a few units, that noise floor is ~1e-12 to
		// 1e-11, straddling `NEARZERO` and letting roughly half of all
		// self-intersections through as spurious tiny-positive hits
		// (measured: ~94% spurious self-shadow rate on a flat curtain's
		// own NEE shadow ray toward a light behind it -- see
		// docs/CLOTH_FABRIC_DESIGN.md section 15 debt 21).  Same pattern
		// as the torus shadow-ray speckle in
		// docs/skills/precision-fix-the-formulation.md; fixed once here
		// (the shared producer) rather than in each of `IntersectRay` /
		// `IntersectRay_IntersectionOnly` (the anti-pattern that skill
		// calls out) so every caller benefits.
		Scalar coordScale = fabs(ray.origin.x) + fabs(ray.origin.y) + fabs(ray.origin.z);
		for( int ci = 0; ci < 4; ci++ ) {
			coordScale = r_max( coordScale,
				fabs(patch.pts[ci].x) + fabs(patch.pts[ci].y) + fabs(patch.pts[ci].z) );
		}
		const Scalar tMin = NEARZERO * ( Scalar(1) + coordScale );

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

		// Find a, b, c per axis; index 0/1/2 == x/y/z so the elimination
		// below can address them by a permuted axis index.
		const Scalar a[3] = {
			patch.pts[3].x - patch.pts[2].x - patch.pts[1].x + patch.pts[0].x,
			patch.pts[3].y - patch.pts[2].y - patch.pts[1].y + patch.pts[0].y,
			patch.pts[3].z - patch.pts[2].z - patch.pts[1].z + patch.pts[0].z
		};

		const Scalar b[3] = {
			patch.pts[2].x - patch.pts[0].x,
			patch.pts[2].y - patch.pts[0].y,
			patch.pts[2].z - patch.pts[0].z
		};

		const Scalar c[3] = {
			patch.pts[1].x - patch.pts[0].x,
			patch.pts[1].y - patch.pts[0].y,
			patch.pts[1].z - patch.pts[0].z
		};

		// Retrieve the xyz of the q part of ray
		const Scalar q[3] = { ray.Dir().x, ray.Dir().y, ray.Dir().z };

		// Find d w.r.t. x, y, z - subtracting the ray origin just after
		const Scalar d[3] = {
			patch.pts[0].x - ray.origin.x,
			patch.pts[0].y - ray.origin.y,
			patch.pts[0].z - ray.origin.z
		};

		// ---- Choose the elimination axis -------------------------------
		//
		// The three component equations of `P(u,v) = ray.origin + t*q` are
		//
		//     a[k]*u*v + b[k]*u + c[k]*v + d[k] = t*q[k]     (k = x, y, z)
		//
		// Ramsey-Potter-Hansen removes `t` by scaling the k = i equation by
		// q[w] and the k = w equation by q[i] and subtracting: the right
		// sides become t*q[i]*q[w] on both, so they cancel EXACTLY and what
		// is left is
		//
		//     A*u*v + B*u + C*v + D = 0,   A = a[i]*q[w] - a[w]*q[i], ...
		//
		// Doing that for both axes i, j other than w yields the 2x2 system
		// the quadratic in v below is built from.
		//
		// `w` is the axis DIVIDED OUT, and it must be one where q[w] is not
		// small.  When q[w] == 0 the two eliminated equations collapse to
		//
		//     -q[i]*( a[w]uv + b[w]u + c[w]v + d[w] ) = 0   and
		//     -q[j]*( a[w]uv + b[w]u + c[w]v + d[w] ) = 0
		//
		// -- the SAME equation twice, up to scale.  The 2x2 system is then
		// rank-1, every coefficient of the quadratic in v cancels to
		// identically zero, `SolveQuadricWithinRange` reports no roots and
		// the patch is missed.  The published reference implementation hard-
		// codes w = z, so ANY ray travelling in the XY plane was missed:
		// notably a `clipped_plane` / `bilinear_patch` viewed dead-on from
		// above by a camera looking along -Y was invisible (measured: a ray
		// from (0,10,0) along (0,-1,0) missed a 10x10 quad in y = 0, while
		// tilting the direction by 0.01 hit it at t = 10; recorded in
		// docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md section 10.6).
		//
		// The degeneracy is in the FORMULATION, not in a threshold: no
		// epsilon on q[z] can rescue a rank-1 system.  Picking w as the
		// LARGEST |q| component -- the same choice `computet` already makes
		// for the t recovery, with the same tie-break order -- makes q[w] as
		// far from zero as the direction allows (|q[w]| >= |q|/sqrt(3) for
		// any non-zero q), so the system is never rank-deficient.  With
		// (i, j, w) taken cyclically, w = z gives (i, j) = (x, y) and the
		// algebra below is TEXTUALLY the reference one, so nothing changes
		// for the rays that already worked.
		int w = 2;
		if( fabs(q[0]) >= fabs(q[1]) && fabs(q[0]) >= fabs(q[2]) ) {
			w = 0;
		} else if( fabs(q[1]) >= fabs(q[2]) ) {
			w = 1;
		}
		const int i = (w + 1) % 3;
		const int j = (w + 2) % 3;

		// Find A1 and A2
		const Scalar A1 = a[i]*q[w] - a[w]*q[i];
		const Scalar A2 = a[j]*q[w] - a[w]*q[j];

		// Find B1 and B2
		const Scalar B1 = b[i]*q[w] - b[w]*q[i];
		const Scalar B2 = b[j]*q[w] - b[w]*q[j];

		// Find C1 and C2
		const Scalar C1 = c[i]*q[w] - c[w]*q[i];
		const Scalar C2 = c[j]*q[w] - c[w]*q[j];

		// Find D1 and D2
		const Scalar D1 = d[i]*q[w] - d[w]*q[i];
		const Scalar D2 = d[j]*q[w] - d[w]*q[j];

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

				if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > tMin ) {
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

				if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > tMin ) {
					hit.bHit = true;

					const Scalar u = getu(sol[1],A2,A1,B2,B1,C2,C1,D2,D1);
					if( u < 1+NEARZERO && u > NEARZERO ) {
						const Point3 pos2 = GeometricUtilities::EvaluateBilinearPatchAt( patch, u, sol[1] );
						const Scalar t2 = computet(ray,pos2);
						if(t2 < tMin || hit.dRange < t2) { // t2 is bad or t1 is better
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

					if( hit.u < 1+NEARZERO && hit.u > -NEARZERO && hit.dRange > tMin ) {
						hit.bHit = true;
					}
				}
			}
			break;
		};
	}
}
