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
#include <cmath>
#include <limits>

namespace RISE
{
	namespace EquiangularSampling
	{
		namespace Detail
		{
			inline Scalar Norm( const Vector3& v )
			{
				return std::hypot( std::hypot(v.x,v.y), v.z );
			}

			inline Scalar ScaledProduct( const Scalar a, const Scalar b )
			{
				if( a == 0.0 || b == 0.0 ) return 0.0;
				int exponentA = 0;
				int exponentB = 0;
				const Scalar mantissaA = std::frexp(a,&exponentA);
				const Scalar mantissaB = std::frexp(b,&exponentB);
				return std::scalbn(mantissaA*mantissaB,exponentA+exponentB);
			}

			inline Scalar MaterializeScaledFraction(
				const Scalar mantissa, const int exponent, const Scalar fraction )
			{
				if( fraction == 0.0 ) return 0.0;
				int fractionExponent = 0;
				const Scalar fractionMantissa =
					std::frexp(fraction,&fractionExponent);
				return std::scalbn(
					mantissa*fractionMantissa,exponent+fractionExponent);
			}

			struct Geometry
			{
				Scalar delta;
				Scalar distance;
				Scalar scaledDistance;
				Scalar scaledOffsetNear;
				Scalar scaledOffsetFar;
				int coordinateExponent;
				Scalar thetaSpanMantissa;
				int thetaSpanExponent;
				Scalar length;
				bool uniform;
				bool valid;

				Geometry() : delta(0), distance(0), scaledDistance(0),
					scaledOffsetNear(0), scaledOffsetFar(0), coordinateExponent(0),
					thetaSpanMantissa(0), thetaSpanExponent(0), length(0),
					uniform(false), valid(false) {}
			};

			inline Geometry BuildGeometry(
				const Ray& ray,
				const Point3& pivot,
				const Scalar tNear,
				const Scalar tFar,
				const bool segmentBounded )
			{
				Geometry g;
				if( !segmentBounded || !std::isfinite(tNear) ||
					!std::isfinite(tFar) || tFar <= tNear ) return g;
				g.length = tFar-tNear;
				if( !std::isfinite(g.length) || g.length <= 0.0 ) return g;
				const Vector3 toPivot = Vector3Ops::mkVector3( pivot, ray.origin );
				const Scalar pivotDistance = Norm(toPivot);
				if( !std::isfinite(pivotDistance) ) return g;
				if( pivotDistance > 0.0 ) {
					const int pivotExponent = std::ilogb(pivotDistance);
					const Vector3 scaledPivot(
						std::scalbn(toPivot.x,-pivotExponent),
						std::scalbn(toPivot.y,-pivotExponent),
						std::scalbn(toPivot.z,-pivotExponent) );
					g.delta = std::scalbn(
						Vector3Ops::Dot(scaledPivot,ray.Dir()),pivotExponent);
				}
				const Vector3 perpendicular = toPivot - ray.Dir()*g.delta;
				// Scaled hypot avoids overflow/underflow in the historical
				// dot(v,v)-delta^2 formulation while remaining nonnegative.
				g.distance = Norm(perpendicular);
				if( !std::isfinite(g.delta) || !std::isfinite(g.distance) ) return g;
				const Scalar scale = fmax(g.length,pivotDistance);
				const Scalar collinearThreshold = Scalar(32.0) *
					sqrt(std::numeric_limits<Scalar>::epsilon()) * scale;
				g.uniform = g.distance <= collinearThreshold;
				if( !g.uniform ) {
					const Scalar coordinateScale = fmax(g.distance,
						fmax(fabs(g.delta),fmax(fabs(tNear),fabs(tFar))));
					g.coordinateExponent = std::ilogb(coordinateScale);
					g.scaledDistance = std::scalbn(
						g.distance,-g.coordinateExponent);
					const Scalar scaledDelta = std::scalbn(
						g.delta,-g.coordinateExponent);
					g.scaledOffsetNear =
						std::scalbn(tNear,-g.coordinateExponent)-scaledDelta;
					g.scaledOffsetFar =
						std::scalbn(tFar,-g.coordinateExponent)-scaledDelta;
					const Scalar scaledDenominator =
						g.scaledDistance*g.scaledDistance+
						g.scaledOffsetNear*g.scaledOffsetFar;
					Scalar thetaSpan = 0.0;
					if( scaledDenominator == 0.0 ) {
						thetaSpan = PI_OV_TWO;
					} else {
						int distanceExponent = 0;
						int lengthExponent = 0;
						int denominatorExponent = 0;
						const Scalar distanceMantissa =
							std::frexp(g.distance,&distanceExponent);
						const Scalar lengthMantissa =
							std::frexp(g.length,&lengthExponent);
						const Scalar denominatorMantissa =
							std::frexp(fabs(scaledDenominator),&denominatorExponent);
						Scalar ratioMantissa =
							(distanceMantissa*lengthMantissa)/denominatorMantissa;
						int ratioAdjustment = 0;
						ratioMantissa = std::frexp(ratioMantissa,&ratioAdjustment);
						const int ratioExponent = distanceExponent+lengthExponent-
							2*g.coordinateExponent-denominatorExponent+ratioAdjustment;
						const int minimumNormalExponent =
							std::numeric_limits<Scalar>::min_exponent;
						if( ratioExponent < minimumNormalExponent ) {
							if( scaledDenominator > 0.0 ) {
								// atan(r)=r to binary64 precision this far below the
								// representable range.  Preserve r as mantissa/exponent.
								g.thetaSpanMantissa = ratioMantissa;
								g.thetaSpanExponent = ratioExponent;
							} else {
								thetaSpan = PI;
							}
						} else if( ratioExponent >
							std::numeric_limits<Scalar>::max_exponent ) {
							thetaSpan = PI_OV_TWO;
						} else {
							const Scalar ratio = std::scalbn(ratioMantissa,ratioExponent);
							thetaSpan = scaledDenominator > 0.0 ?
								atan(ratio) : PI-atan(ratio);
						}
					}
					if( thetaSpan > 0.0 ) {
						g.thetaSpanMantissa = std::frexp(
							thetaSpan,&g.thetaSpanExponent);
					}
					if( !std::isfinite(g.thetaSpanMantissa) ||
						g.thetaSpanMantissa <= 0.0 ) return Geometry();
				}
				g.valid = std::isfinite(g.delta) && std::isfinite(g.distance) &&
					std::isfinite(g.length) && g.length > 0.0;
				return g;
			}

			inline Scalar AngularPdf( const Geometry& g, const Scalar t )
			{
				const Scalar scaledDt =
					std::scalbn(t,-g.coordinateExponent)-
					std::scalbn(g.delta,-g.coordinateExponent);
				const Scalar normalizedSquared =
					g.scaledDistance*g.scaledDistance+scaledDt*scaledDt;
				if( normalizedSquared <= 0.0 ) return 0.0;
				// D/[thetaSpan*(D^2+dt^2)], factored so neither square nor
				// the denominator can overflow or underflow at finite scene scales.
				const Scalar pdf = std::scalbn(
					(g.scaledDistance/normalizedSquared)/g.thetaSpanMantissa,
					-g.coordinateExponent-g.thetaSpanExponent );
				return std::isfinite(pdf) && pdf >= 0.0 ? pdf : 0.0;
			}

			inline Scalar InvertAngularCdf(
				const Geometry& g, const Scalar tNear, const Scalar tFar,
				const Scalar xi )
			{
				if( xi <= 0.0 ) return tNear;
				if( xi >= 1.0 ) return tFar;
				if( xi <= 0.5 ) {
					const Scalar u = g.scaledOffsetNear/g.scaledDistance;
					const Scalar fractionalSpan = MaterializeScaledFraction(
						g.thetaSpanMantissa,g.thetaSpanExponent,xi);
					if( g.thetaSpanExponent <
						std::numeric_limits<Scalar>::min_exponent ||
						fractionalSpan < std::numeric_limits<Scalar>::min() ) {
						int distanceExponent = 0;
						const Scalar distanceMantissa =
							std::frexp(g.distance,&distanceExponent);
						const Scalar offset = std::scalbn(
							distanceMantissa*g.thetaSpanMantissa*xi*(1.0+u*u),
							distanceExponent+g.thetaSpanExponent );
						return fmax(tNear,fmin(tNear+offset,tFar));
					}
					const Scalar tangent = tan(fractionalSpan);
					const Scalar denominator = 1.0-u*tangent;
					const Scalar offset = ScaledProduct(
						g.distance,(tangent/denominator)*(1.0+u*u) );
					return fmax(tNear,fmin(tNear+offset,tFar));
				}
				const Scalar u = g.scaledOffsetFar/g.scaledDistance;
				const Scalar farFraction = 1.0-xi;
				const Scalar fractionalSpan = MaterializeScaledFraction(
					g.thetaSpanMantissa,g.thetaSpanExponent,farFraction);
				if( g.thetaSpanExponent <
					std::numeric_limits<Scalar>::min_exponent ||
					fractionalSpan < std::numeric_limits<Scalar>::min() ) {
					int distanceExponent = 0;
					const Scalar distanceMantissa =
						std::frexp(g.distance,&distanceExponent);
					const Scalar offset = -std::scalbn(
						distanceMantissa*g.thetaSpanMantissa*farFraction*(1.0+u*u),
						distanceExponent+g.thetaSpanExponent );
					return fmax(tNear,fmin(tFar+offset,tFar));
				}
				const Scalar tangent = tan(fractionalSpan);
				const Scalar denominator = 1.0+u*tangent;
				const Scalar offset = -ScaledProduct(
					g.distance,(tangent/denominator)*(1.0+u*u) );
				return fmax(tNear,fmin(tFar+offset,tFar));
			}
		}

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
			const bool segmentBounded,
			Scalar xi
			)
		{
			Sample result = { tNear, 0.0 };
			const Detail::Geometry g = Detail::BuildGeometry(
				ray,lightPos,tNear,tFar,segmentBounded );
			if( !g.valid || !std::isfinite(xi) || xi < 0.0 || xi > 1.0 ) return result;
			if( g.uniform ) {
				result.t = tNear+xi*g.length;
				result.pdf = 1.0/g.length;
				return result;
			}
			result.t = Detail::InvertAngularCdf(g,tNear,tFar,xi);
			result.pdf = Detail::AngularPdf(g,result.t);
			if( !std::isfinite(result.t) || !std::isfinite(result.pdf) ||
				result.pdf < 0.0 ) return Sample{tNear,0.0};
			return result;
		}

		/// Evaluate the equiangular PDF at distance t
		inline Scalar Pdf(
			const Ray& ray,
			const Point3& lightPos,
			Scalar tNear,
			Scalar tFar,
			const bool segmentBounded,
			Scalar t
			)
		{
			const Detail::Geometry g = Detail::BuildGeometry(
				ray,lightPos,tNear,tFar,segmentBounded );
			if( !g.valid || !std::isfinite(t) || t < tNear || t > tFar ) return 0.0;
			if( g.uniform ) return 1.0/g.length;
			return Detail::AngularPdf(g,t);
		}
	}
}

#endif
