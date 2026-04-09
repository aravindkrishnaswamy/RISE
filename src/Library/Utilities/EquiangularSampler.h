//////////////////////////////////////////////////////////////////////
//
//  EquiangularSampler.h - Equiangular distance sampling for
//    point lights in participating media
//
//  Samples a distance along a ray proportional to the 1/r^2
//  geometry term toward a point light.  This concentrates samples
//  near the light, dramatically reducing variance for in-scattering
//  from point/spot lights in dense media.
//
//  The sampled PDF is:
//    p(t) = D / ((theta_b - theta_a) * (D^2 + (t - delta)^2))
//
//  where:
//    D     = perpendicular distance from light to ray
//    delta = projection of light onto ray
//    theta_a, theta_b = angle subtended by [tNear, tFar] from light
//
//  Combined with delta tracking via one-sample MIS (balance
//  heuristic) for unbiased, low-variance volume transport.
//
//  Reference:
//    - Kulla, Fajardo, "Importance Sampling Techniques for Path
//      Tracing in Participating Media", EGSR 2012
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 8, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EQUIANGULAR_SAMPLER_
#define EQUIANGULAR_SAMPLER_

#include "Math3D/Math3D.h"
#include "Ray.h"

namespace RISE
{
	namespace EquiangularSampling
	{
		/// Result of equiangular distance sampling
		struct Sample
		{
			Scalar t;			///< Sampled distance along the ray
			Scalar pdf;			///< PDF at the sampled distance
		};

		/// Sample a distance in [tNear, tFar] along the ray
		/// proportional to the 1/r^2 geometry term toward lightPos.
		///
		/// Uses the Kulla-Fajardo 2012 closed-form CDF inversion.
		inline Sample SampleDistance(
			const Ray& ray,
			const Point3& lightPos,
			Scalar tNear,
			Scalar tFar,
			Scalar xi
			)
		{
			Sample result;

			// Project light position onto the ray
			const Vector3 rayToLight = Vector3Ops::mkVector3( lightPos, ray.origin );
			const Scalar delta = Vector3Ops::Dot( rayToLight, ray.Dir() );

			// Perpendicular distance from light to ray
			const Scalar D_sq = Vector3Ops::Dot( rayToLight, rayToLight ) - delta * delta;
			const Scalar D = sqrt( fmax( D_sq, 1e-20 ) );

			// Angle bounds
			const Scalar thetaA = atan2( tNear - delta, D );
			const Scalar thetaB = atan2( tFar - delta, D );

			// Degenerate case: light is on the ray (D ~ 0)
			// Fall back to uniform sampling
			if( fabs( thetaB - thetaA ) < 1e-10 || D < 1e-10 )
			{
				result.t = tNear + xi * (tFar - tNear);
				result.pdf = 1.0 / fmax( tFar - tNear, 1e-20 );
				return result;
			}

			// CDF inversion: t = delta + D * tan( lerp(thetaA, thetaB, xi) )
			const Scalar theta = thetaA + xi * (thetaB - thetaA);
			result.t = delta + D * tan( theta );

			// Clamp to valid range (numerical safety)
			result.t = fmax( tNear, fmin( result.t, tFar ) );

			// PDF: D / ((thetaB - thetaA) * (D^2 + (t - delta)^2))
			const Scalar dt = result.t - delta;
			result.pdf = D / ((thetaB - thetaA) * (D_sq + dt * dt));
			if( result.pdf < 1e-30 ) result.pdf = 1e-30;

			return result;
		}

		/// Evaluate the equiangular PDF at distance t
		inline Scalar Pdf(
			const Ray& ray,
			const Point3& lightPos,
			Scalar tNear,
			Scalar tFar,
			Scalar t
			)
		{
			const Vector3 rayToLight = Vector3Ops::mkVector3( lightPos, ray.origin );
			const Scalar delta = Vector3Ops::Dot( rayToLight, ray.Dir() );
			const Scalar D_sq = Vector3Ops::Dot( rayToLight, rayToLight ) - delta * delta;
			const Scalar D = sqrt( fmax( D_sq, 1e-20 ) );

			if( D < 1e-10 )
				return 1.0 / fmax( tFar - tNear, 1e-20 );

			const Scalar thetaA = atan2( tNear - delta, D );
			const Scalar thetaB = atan2( tFar - delta, D );

			if( fabs( thetaB - thetaA ) < 1e-10 )
				return 1.0 / fmax( tFar - tNear, 1e-20 );

			const Scalar dt = t - delta;
			return D / ((thetaB - thetaA) * (D_sq + dt * dt));
		}
	}
}

#endif
