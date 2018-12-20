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
		hit.dRange = INFINITY;

		const Vector3& rayDirNorm = ray.dir;

		/* Not sure why yet, but this causes a major major bug!
		Scalar yin  = ray.origin.x + minorRadius * rayDirNorm.x;
		Scalar yout = ray.origin.x + majorRadius * rayDirNorm.x;

		if( ( (yin >  majorRadius && yout >  majorRadius) ||
			(yin < -majorRadius && yout < -majorRadius) )) {
				return;
			}
		*/
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

	#if 0
		std::vector<Scalar> coeffs;
		coeffs.push_back( u*u - q*t );
		coeffs.push_back( 2.0*m*u - q*l );
		coeffs.push_back( m*m + 2.0*u - q*f );
		coeffs.push_back( 2.0 * m );
		coeffs.push_back( 1.0 );

	//	Scalar solution;
	//	if( Polynomial::SolvePolynomialWithinRange( coeffs, 0.0, 10000, solution, 0 ) ) {
	//		hit.bHit = true;
	//		hit.dRange = solution;
	//	}

		std::vector<Scalar> solutions;
		if( Polynomial::SolvePolynomialWithinRange( coeffs, 0.0, 10000, solutions, 0 ) > 0 ) {
			hit.bHit = true;
			hit.dRange = solutions[0];
			if( solutions.size() > 1 ) {
				hit.dRange2 = solutions[1];
			}
		}

	#else
		// Form a quartic with the torus co-efficients
		Scalar C[5];

		C[0] = 1.0;
		C[1] = 2.0 * m;
		C[2] = m*m + 2.0*u - q*f;
		C[3] = 2.0*m*u - q*l;
		C[4] = u*u - q*t;

		Scalar s[4];
		int n = Polynomial::SolveQuartic( C, s );

		if( n > 0 )
		{
			// We have some solutions
			// Find the closest solution
			for( int i=0; i<n; i++ ) {
				if( s[i] < hit.dRange && s[i] > NEARZERO ) {
					hit.dRange2 = hit.dRange + 1e-3;
					hit.dRange = s[i] - 1e-6;// / dirnorm;			
					hit.bHit = true;
				}
			}
		}/* else {
			std::vector<Scalar> coeffs;
			coeffs.push_back( u*u - q*t );
			coeffs.push_back( 2.0*m*u - q*l );
			coeffs.push_back( m*m + 2.0*u - q*f );
			coeffs.push_back( 2.0 * m );
			coeffs.push_back( 1.0 );

			Scalar solution;
			if( Polynomial::SolvePolynomialWithinRange( coeffs, 0.0, 10000, solution, 1e-8 ) ) {
				_asm int 3h;
			}		
		} Used for testing the faster Quartic solver */
	#endif
	}
}

