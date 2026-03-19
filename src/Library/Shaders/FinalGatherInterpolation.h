//////////////////////////////////////////////////////////////////////
//
//  FinalGatherInterpolation.h - Shared interpolation helpers for
//    final gather irradiance cache lookups.
//
//  Author: Aravind Krishnaswamy
//  Date: March 1, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FINALGATHER_INTERPOLATION_
#define FINALGATHER_INTERPOLATION_

#include "../Interfaces/IIrradianceCache.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Math3D/Math3D.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		struct FinalGatherInterpolation
		{
			static inline Scalar ContributorWeightForCount(
				const Scalar weight,
				const Scalar minAcceptedWeight
				)
			{
				const Scalar dominanceClampMultiplier = 2.0;
				const Scalar contributorWeightCap = r_max( minAcceptedWeight * dominanceClampMultiplier, Scalar(NEARZERO) );
				return r_min( weight, contributorWeightCap );
			}

			static inline Scalar ComputeEffectiveContributors(
				const std::vector<IIrradianceCache::CacheElement>& results
				)
			{
				if( results.empty() ) {
					return 0.0;
				}

				Scalar minAcceptedWeight = 1e10;
				std::vector<IIrradianceCache::CacheElement>::const_iterator i;
				for( i=results.begin(); i!=results.end(); i++ ) {
					const Scalar w = r_min( 1e10, i->dWeight );
					minAcceptedWeight = r_min( minAcceptedWeight, w );
				}

				Scalar sumW2 = 0;
				Scalar sumWeights = 0;
				for( i=results.begin(); i!=results.end(); i++ ) {
					const Scalar w = ContributorWeightForCount( r_min( 1e10, i->dWeight ), minAcceptedWeight );
					sumWeights += w;
					sumW2 += w*w;
				}

				if( sumWeights <= NEARZERO ) {
					return 0.0;
				}

				return (sumWeights*sumWeights) / r_max( sumW2, Scalar(NEARZERO) );
			}

			static inline RISEPel EvaluateElement(
				const IIrradianceCache::CacheElement& elem,
				const Point3& ptPosition,
				const Vector3& vNormal,
				const bool bComputeCacheGradients,
				bool* pUsedFallback
				)
			{
				RISEPel temp = elem.cIRad;
				bool bUsedFallback = false;

				if( bComputeCacheGradients ) {
					temp = temp + (ptPosition.x-elem.ptPosition.x)*elem.translationalGradient[0];
					temp = temp + (ptPosition.y-elem.ptPosition.y)*elem.translationalGradient[1];
					temp = temp + (ptPosition.z-elem.ptPosition.z)*elem.translationalGradient[2];

					const Vector3 cp = Vector3Ops::Cross( vNormal, elem.vNormal );
					temp = temp + cp.x*elem.rotationalGradient[0];
					temp = temp + cp.y*elem.rotationalGradient[1];
					temp = temp + cp.z*elem.rotationalGradient[2];

					// Gradient extrapolation can produce negative values for individual
					// channels.  Rather than snapping back to the base irradiance
					// (which creates hard discontinuities / blotches), let the
					// per-channel clamp below (EnsurePositve) handle it smoothly.
					if( ColorMath::MinValue( temp ) < 0.0 ) {
						bUsedFallback = true;
					}
				}

				ColorMath::EnsurePositve( temp );

				if( pUsedFallback ) {
					*pUsedFallback = bUsedFallback;
				}

				return temp;
			}

			static inline bool TryInterpolate(
				const Point3& ptPosition,
				const Vector3& vNormal,
				const std::vector<IIrradianceCache::CacheElement>& results,
				const Scalar weights,
				const bool bComputeCacheGradients,
				const unsigned int minEffectiveContributors,
				RISEPel& c,
				unsigned int* pGradientFallbacks
				)
			{
				if( results.empty() || weights <= NEARZERO ) {
					c = RISEPel(0.0);
					if( pGradientFallbacks ) {
						*pGradientFallbacks = 0;
					}
					return false;
				}

				const Scalar effectiveContributors = ComputeEffectiveContributors( results );
				if( effectiveContributors < Scalar(minEffectiveContributors) ) {
					c = RISEPel(0.0);
					if( pGradientFallbacks ) {
						*pGradientFallbacks = 0;
					}
					return false;
				}

				c = RISEPel(0.0);
				unsigned int gradientFallbacks = 0;

				std::vector<IIrradianceCache::CacheElement>::const_iterator i;
				for( i=results.begin(); i!=results.end(); i++ ) {
					const IIrradianceCache::CacheElement& elem = *i;
					const Scalar w = r_min( 1e10, elem.dWeight );
					bool bUsedFallback = false;
					RISEPel temp = EvaluateElement(
						elem,
						ptPosition,
						vNormal,
						bComputeCacheGradients,
						&bUsedFallback
						);
					if( bUsedFallback ) {
						gradientFallbacks++;
					}
					c = c + temp * w;
				}

				c = c * (1.0/weights);
				ColorMath::EnsurePositve( c );

				if( pGradientFallbacks ) {
					*pGradientFallbacks = gradientFallbacks;
				}

				return true;
			}
		};
	}
}

#endif
