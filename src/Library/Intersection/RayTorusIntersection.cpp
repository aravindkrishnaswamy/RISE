//////////////////////////////////////////////////////////////////////
//
//  RayTorusIntersection.cpp - Implements a ray torus intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 12, 2001
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

	void RayTorusIntersection( const Ray& ray, HIT& hit, const Scalar majorRadius, const Scalar minorRadius, const Scalar sqrP0 )
	{
		hit.bHit = false;
		hit.dRange = RISE_INFINITY;
		hit.dRange2 = RISE_INFINITY;

		const Vector3& rayDirNorm = ray.Dir();

		// Compute torus constants
		Scalar a0  = 4.0 * sqrP0;
		Scalar b0  = sqrP0 - minorRadius*minorRadius;

		// Compute some terms for building the co-efficients
		Scalar	sqr_rayDirY = rayDirNorm.y*rayDirNorm.y;

		Scalar f = 1.0 - sqr_rayDirY;
		Scalar l = 2.0 * (ray.origin.x*rayDirNorm.x + ray.origin.z*rayDirNorm.z);
		Scalar t = ray.origin.x*ray.origin.x + ray.origin.z*ray.origin.z;
		Scalar g = f + sqr_rayDirY;
		Scalar q = a0 / (g*g);
		Scalar m = (l + 2.0*rayDirNorm.y*ray.origin.y) / g;
		Scalar u = (t +  ray.origin.y*ray.origin.y + b0) / g;

		// Form a quartic with the torus co-efficients
		Scalar C[5];

		C[0] = 1.0;
		C[1] = 2.0 * m;
		C[2] = m*m + 2.0*u - q*f;
		C[3] = 2.0*m*u - q*l;
		C[4] = u*u - q*t;

		// Self-intersection deflation.
		//
		// When the ray origin sits on (or essentially on) the torus
		// surface — typical for shadow / continuation rays — the
		// implicit torus equation
		//   F(O) = (|O|² + R² − r²)² − 4 R² (Ox² + Oz²)
		// is mathematically zero, which drives C[4] = u² − q·t to zero
		// too.  In floating point, C[4] lands at noise level (FP
		// cancellation in u² − q·t plus the SURFACE_INTERSEC_ERROR
		// nudge applied by Object::IntersectRay when it reports the
		// surface point).  The OQS solver then returns a "self" root
		// near t = 0 that is positive and above NEARZERO — passing the
		// downstream gate as a spurious self-shadow.
		//
		// Mathematically the correct fix is to factor out the known
		// t = 0 root and solve the deflated cubic.  Detect "origin on
		// surface" by comparing |C[4]| against a relative scale derived
		// from u² and q·t: surface-resident origins have |C[4]| many
		// orders of magnitude smaller than max(|u²|, |q·t|), whereas
		// off-surface origins have |C[4]| at the same order as both.
		// A relative threshold of 1e-10 leaves FP noise in deflation
		// territory while keeping every off-surface origin (including
		// rays that pass close to the surface) on the quartic path.
		Scalar s[4];
		int n;
		const Scalar quartScale = fabs(u*u) + fabs(q*t);
		if( fabs(C[4]) <= quartScale * 1e-10 ) {
			// Root at t = 0 is the self-intersection — drop it and solve
			// the deflated cubic for the remaining real roots.
			Scalar cubicC[4] = { C[0], C[1], C[2], C[3] };
			Scalar cubicSol[3];
			n = Polynomial::SolveCubic( cubicC, cubicSol );
			for( int i = 0; i < n; i++ ) s[i] = cubicSol[i];
		} else {
			n = Polynomial::SolveQuartic( C, s );
		}

		if( n > 0 )
		{
			// Sort solutions (simple insertion sort, at most 4 elements)
			for( int i = 1; i < n; i++ ) {
				Scalar key = s[i];
				int j = i - 1;
				while( j >= 0 && s[j] > key ) {
					s[j+1] = s[j];
					j--;
				}
				s[j+1] = key;
			}

			// Collect positive roots and count negative roots
			Scalar pos[4];
			int npos = 0;
			int nneg = 0;
			for( int i = 0; i < n; i++ ) {
				if( s[i] > NEARZERO ) {
					pos[npos++] = s[i];
				} else {
					nneg++;
				}
			}

			if( npos > 0 )
			{
				hit.bHit = true;
				hit.dRange = pos[0];

				// For a closed surface, an odd number of negative roots
				// means the ray originates inside the torus tube.
				if( nneg % 2 == 1 ) {
					// Inside the torus — signal with range2 = 0
					hit.dRange2 = 0;
				} else {
					// Outside the torus — range2 is the exit point
					if( npos >= 2 ) {
						hit.dRange2 = pos[1];
					} else {
						// Only one positive root but even negative count;
						// tangent hit — treat as a thin intersection
						hit.dRange2 = pos[0];
					}
				}
			}
		}
	}
}

