//////////////////////////////////////////////////////////////////////
//
//  FinalGatherShaderOp.cpp - Implementation of the FinalGatherShaderOp class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 30, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FinalGatherShaderOp.h"
#include "FinalGatherInterpolation.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	const Scalar kRayBias = Scalar(1e-4);
	const Scalar kMinHitDistance = Scalar(1e-6);
	const Scalar kMinTranslationalGradientDistance = kRayBias * Scalar(10.0);
	const Scalar kMinTranslationalGradientNormalDot = Scalar(0.95);
	const Scalar kVariationBrightRecordLuminance = Scalar(0.2);
	const Scalar kVariationCoeffThreshold = Scalar(1.5);
	const Scalar kVariationPeakThreshold = Scalar(6.0);

	inline Scalar ComputePelLuminance( const RISEPel& pel )
	{
		return 0.212671 * pel[0] + 0.715160 * pel[1] + 0.072169 * pel[2];
	}

	struct IrradianceVariationStats
	{
		unsigned int sampleCount;
		Scalar sumLuminance;
		Scalar sumSquaredLuminance;
		Scalar maxLuminance;

		IrradianceVariationStats() :
			sampleCount( 0 ),
			sumLuminance( 0 ),
			sumSquaredLuminance( 0 ),
			maxLuminance( 0 )
		{}

		inline void RecordSample( const RISEPel& sampleIrradiance )
		{
			const Scalar luminance = r_max( Scalar(0), ComputePelLuminance( sampleIrradiance ) );
			sampleCount++;
			sumLuminance += luminance;
			sumSquaredLuminance += luminance * luminance;
			maxLuminance = r_max( maxLuminance, luminance );
		}

		inline Scalar ComputeReuseScale( const RISEPel& cachedIrradiance, const Scalar minimumReuseScale ) const
		{
			if( sampleCount == 0 ) {
				return 1.0;
			}

			const Scalar cachedLuminance = r_max( Scalar(0), ComputePelLuminance( cachedIrradiance ) );
			if( cachedLuminance <= kVariationBrightRecordLuminance ) {
				return 1.0;
			}

			const Scalar meanLuminance = sumLuminance / Scalar(sampleCount);
			if( meanLuminance <= NEARZERO ) {
				return 1.0;
			}

			const Scalar variance = r_max(
				Scalar(0),
				(sumSquaredLuminance / Scalar(sampleCount)) - meanLuminance * meanLuminance
				);
			const Scalar coefficientOfVariation = sqrt( variance ) / meanLuminance;
			const Scalar peakToMeanRatio = maxLuminance / meanLuminance;

			// Bright cache records with highly variable hemisphere samples are not
			// describing a locally smooth irradiance field. Reusing them broadly
			// causes exactly the kind of hot-centered circular footprints seen in
			// gi_spheres, so make those records much more local.
			Scalar reuseScale = 1.0;
			if( coefficientOfVariation > kVariationCoeffThreshold ) {
				reuseScale = r_min( reuseScale, kVariationCoeffThreshold / coefficientOfVariation );
			}
			if( peakToMeanRatio > kVariationPeakThreshold ) {
				reuseScale = r_min( reuseScale, kVariationPeakThreshold / peakToMeanRatio );
			}

				return r_max( minimumReuseScale, reuseScale );
		}
	};

	struct GradientEstimatorSample
	{
		bool bHit;
		bool bUsableForTranslation;
		const IObject* pObject;
		Vector3 vNormal;
		Scalar dDistance;
		RISEPel cIrradiance;

		GradientEstimatorSample() :
			bHit( false ),
			bUsableForTranslation( false ),
			pObject( 0 ),
			vNormal( 0, 0, 0 ),
			dDistance( 0 ),
			cIrradiance( 0, 0, 0 )
		{}
	};

	struct GradientEstimatorStats
	{
		unsigned int acceptedPairs;
		unsigned int rejectedMisses;
		unsigned int rejectedNearHits;
		unsigned int rejectedDiscontinuities;

		GradientEstimatorStats() :
			acceptedPairs( 0 ),
			rejectedMisses( 0 ),
			rejectedNearHits( 0 ),
			rejectedDiscontinuities( 0 )
		{}

		inline unsigned int RejectedPairs() const
		{
			return rejectedMisses + rejectedNearHits + rejectedDiscontinuities;
		}

		inline bool RejectWholeRecord() const
		{
			if( acceptedPairs == 0 ) {
				return true;
			}

			// Translational gradients are only valid when neighboring bins see a
			// locally smooth irradiance field. If more pair comparisons are rejected
			// than accepted, or if miss/discontinuity pairs alone outnumber the valid
			// ones, this record is sitting on an edge/corner/visibility boundary and
			// should not be allowed to extrapolate a translational gradient.
			const unsigned int rejectedPairs = RejectedPairs();
			const unsigned int discontinuousPairs = rejectedMisses + rejectedDiscontinuities;
			return (rejectedPairs > acceptedPairs) || (discontinuousPairs > acceptedPairs);
		}
	};

	enum GradientPairClassification
	{
		eGradientPairAccepted = 0,
		eGradientPairRejectedMiss,
		eGradientPairRejectedNearHit,
		eGradientPairRejectedDiscontinuity
	};

	inline GradientEstimatorSample TraceGradientEstimatorSample(
		const IScene* pScene,
		const RuntimeContext& rc,
		const RasterizerState& rast,
		const IRayCaster& caster,
		const IRayCaster::RAY_STATE& rs,
		const Point3& origin,
		const Vector3& wGrad,
		const IBSDF& brdf,
		const RayIntersectionGeometric& shading,
		const IRadianceMap* pRadianceMap,
		const IORStack* const ior_stack
		)
	{
		GradientEstimatorSample sample;

		Ray fgGradRay( origin, wGrad );
		fgGradRay.Advance( kRayBias );

		RISEPel lijGrad( 0, 0, 0 );
		Scalar tGrad = 0;
		if( caster.CastRay( rc, rast, fgGradRay, lijGrad, rs, &tGrad, pRadianceMap, ior_stack ) ) {
			if( tGrad > kMinHitDistance ) {
				sample.bHit = true;
				sample.dDistance = tGrad;
				sample.cIrradiance = lijGrad * brdf.value( wGrad, shading );

				if( tGrad > kMinTranslationalGradientDistance && pScene && pScene->GetObjects() ) {
					RayIntersection gradRI( fgGradRay, rast );
					pScene->GetObjects()->IntersectRay( gradRI, true, true, false );

					if( gradRI.geometric.bHit && gradRI.pObject ) {
						sample.bUsableForTranslation = true;
						sample.pObject = gradRI.pObject;
						sample.vNormal = gradRI.geometric.vNormal;
					}
				}
			}
		}

		return sample;
	}

	inline GradientPairClassification ClassifyGradientPair(
		const GradientEstimatorSample& lhs,
		const GradientEstimatorSample& rhs
		)
	{
		if( !lhs.bHit || !rhs.bHit ) {
			return eGradientPairRejectedMiss;
		}

		if( !lhs.bUsableForTranslation || !rhs.bUsableForTranslation ) {
			// Translational gradients divide by the hit distance. Extremely small
			// distances usually mean the sample is grazing an adjacent wall/corner,
			// which makes the finite-difference estimate numerically unstable.
			return eGradientPairRejectedNearHit;
		}

		if( lhs.pObject != rhs.pObject ) {
			return eGradientPairRejectedDiscontinuity;
		}

		if( Vector3Ops::Dot( lhs.vNormal, rhs.vNormal ) < kMinTranslationalGradientNormalDot ) {
			return eGradientPairRejectedDiscontinuity;
		}

		return eGradientPairAccepted;
	}

	inline void RecordGradientPairClassification(
		GradientEstimatorStats& stats,
		const GradientPairClassification classification
		)
	{
		switch( classification ) {
		case eGradientPairAccepted:
			stats.acceptedPairs++;
			break;
		case eGradientPairRejectedMiss:
			stats.rejectedMisses++;
			break;
		case eGradientPairRejectedNearHit:
			stats.rejectedNearHits++;
			break;
		case eGradientPairRejectedDiscontinuity:
			stats.rejectedDiscontinuities++;
			break;
		default:
			break;
		}
	}
}

FinalGatherShaderOp::FinalGatherShaderOp(
	const unsigned int numTheta_,
	const unsigned int numPhi_,
	const bool bComputeCacheGradients_,
	const unsigned int min_effective_contributors_,
	const Scalar high_variation_reuse_scale_,
	const bool cache_
	) :
	  numTheta( numTheta_ ),
	  numPhi( numPhi_ ),
	  bComputeCacheGradients( bComputeCacheGradients_ ),
	  min_effective_contributors( min_effective_contributors_ ),
	  high_variation_reuse_scale( r_max( Scalar(0), r_min( Scalar(1), high_variation_reuse_scale_ ) ) ),
	  cache( cache_ )
{
}

FinalGatherShaderOp::~FinalGatherShaderOp( )
{
}

//! Tells the shader to apply shade to the given intersection point
void FinalGatherShaderOp::PerformOperation(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [in/out] Resultant color from op
	const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	c = RISEPel(0.0);

	const IScene* pScene = caster.GetAttachedScene();
	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;
	const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
	const IPhotonMap* pGM = pScene->GetGlobalPelMap();
	const ISpectralPhotonMap* pGSM = pScene->GetGlobalSpectralMap();

	// In final gather, only global map values count, do nothing else
	if( rs.type == IRayCaster::RAY_STATE::eRayFinalGather ) {
		if( pBRDF ) {
			if( pGM ) {
				pGM->RadianceEstimate( c, ri.geometric, *pBRDF );
			}

			if( pGSM ) {
				RISEPel cs;
				pGSM->RadianceEstimate( cs, ri.geometric, *pBRDF );
				c = c + cs;
			}
		} else if( pScat ) {
			// Do the scattering and continue the final gather ray until we hit somewhere where
			//   it is likely to have an irradiance value
			const ScatteredRayContainer& scattered = *pScat;
			for( unsigned int i=0; i<scattered.Count(); i++ ) {
				ScatteredRay& scat = scattered[i];
				// Cast and add!
				RISEPel	reflectedPixel;
				scat.ray.Advance( 1e-8 );

				IRayCaster::RAY_STATE rs2;

				rs2.depth = rs.depth+1;
				rs2.importance = 1.0;
				rs2.considerEmission = false;
				rs2.type = IRayCaster::RAY_STATE::eRayFinalGather;

				caster.CastRay( rc, ri.geometric.rast, scat.ray, reflectedPixel, rs2, 0, ri.pRadianceMap, scat.ior_stack?scat.ior_stack:ior_stack );
				c = c + (reflectedPixel * scat.kray);
			}
		}
	}
	else
	{
		// Lets check our rasterizer state to see if we even need to do work!
		if( cache ) {
			if( !rc.StateCache_HasStateChanged( this, c, ri.pObject, ri.geometric.rast ) ) {
				// State hasn't changed, use the value already there
				return;
			}
		}

		if( (pGM||pGSM) && pBRDF )
		{
			bool bComputeIrradiance = true;

				// If we are in normal rendering pass, look it up in the cache
				IIrradianceCache* pCache = pScene->GetIrradianceCache();
				if( pCache && pCache->GetTolerance() > 0 && rc.pass == RuntimeContext::PASS_NORMAL ) {
					// Look it up
					std::vector<IIrradianceCache::CacheElement> results;
					const Scalar weights = pCache->Query(ri.geometric.ptIntersection, ri.geometric.vNormal, results);

					// Avoid trusting isolated cache records. Requiring a few
					// effective contributors reduces visible record footprints.
					const bool bInterpolated = FinalGatherInterpolation::TryInterpolate(
						ri.geometric.ptIntersection,
						ri.geometric.vNormal,
						results,
						weights,
						bComputeCacheGradients,
						min_effective_contributors,
						c,
						0
						);

					if( bInterpolated ) {
						bComputeIrradiance = false;
					}
				}

				// We are in compute irradiance pass...
				if( rc.pass == RuntimeContext::PASS_IRRADIANCE_CACHE && pCache && pCache->GetTolerance() > 0 ) {
					// Ask the cache whether a normal-pass interpolation would already
					// succeed here. If not, populate a denser cache record now.
					bComputeIrradiance = !pCache->WouldInterpolate(
						ri.geometric.ptIntersection,
						ri.geometric.vNormal,
						min_effective_contributors
						);
				}

				if( bComputeIrradiance && pSPF ) {
					// We need to initiate a final gather
					const unsigned int irrM = numTheta;
					const unsigned int irrN = numPhi;
					const unsigned int final_gather_count = irrM*irrN;
					if( final_gather_count == 0 ) {
						return;
					}

				IRayCaster::RAY_STATE rs2;
				rs2.depth = rs.depth + 1;
				rs2.importance = 1.0;
				rs2.considerEmission = false;
				rs2.type = IRayCaster::RAY_STATE::eRayFinalGather;

					Scalar rsum = 0;
					unsigned int hits = 0;
					IrradianceVariationStats irradianceVariationStats;

				// Computing the final gather over the hemisphere using rotational and
				// translational gradients
				// This code is taken almost directly from the SunFlow renderer, LightServer.java
				//  available here: http://sunflow.sourceforge.net

					RISEPel	rotGradient[3];						// The rotational gradient as we go along sampling
					RISEPel transGradient1[3];					// The first part of the translational gradient
					rotGradient[0] = rotGradient[1] = rotGradient[2] = RISEPel(0,0,0);
					transGradient1[0] = transGradient1[1] = transGradient1[2] = RISEPel(0,0,0);

					if( pCache && bComputeCacheGradients )
					{
						RISEPel transGradient2[3];				// The second part of the translational gradient
						transGradient2[0] = transGradient2[1] = transGradient2[2] = RISEPel(0,0,0);

					const Scalar fN = Scalar(irrN);
					const Scalar fM = Scalar(irrM);

					// Irradiance gradients temporary variables. Keep a separate deterministic
					// gradient estimator so the finite differences are taken between stratum
					// centers instead of jittered Monte Carlo samples.
					GradientEstimatorStats gradientStats;
					Vector3 vim;
					GradientEstimatorSample prevThetaSample;
					GradientEstimatorSample* prevPhiSamples = new GradientEstimatorSample[irrM];
					GradientEstimatorSample* firstPhiSamples = new GradientEstimatorSample[irrM];

					for( unsigned int i=0; i<irrN; i++ ) {
						const Scalar xi = (Scalar(i) + rc.random.CanonicalRandom()) / fN;
						const Scalar phi = TWO_PI * xi;
						const Scalar cosPhi = cos(phi);
						const Scalar sinPhi = sin(phi);

						const Scalar xiCenter = (Scalar(i) + Scalar(0.5)) / fN;
						const Scalar phiCenter = TWO_PI * xiCenter;
						const Scalar cosPhiCenter = cos(phiCenter);
						const Scalar sinPhiCenter = sin(phiCenter);
						const Vector3 vi = ri.geometric.onb.Transform( Vector3(-sinPhiCenter,cosPhiCenter,0.0) );

						RISEPel rotGradientTemp(0,0,0);

						const Vector3 ui = ri.geometric.onb.Transform( Vector3(cosPhiCenter,sinPhiCenter,0.0) );
						const Scalar phim = (TWO_PI * Scalar(i)) / fN;
						vim = ri.geometric.onb.Transform(
							Vector3(cos( phim + PI_OV_TWO ), sin( phim + PI_OV_TWO ), 0.0)
							);

							RISEPel transGradient1Temp = RISEPel(0,0,0);
							RISEPel transGradient2Temp = RISEPel(0,0,0);
							prevThetaSample = GradientEstimatorSample();

							for( unsigned int j=0; j<irrM; j++ ) {
							const Scalar xj = (Scalar(j) + rc.random.CanonicalRandom()) / fM;
							const Scalar sinTheta = sqrt( xj );
							const Scalar cosTheta = sqrt( 1.0 - xj );

								const Vector3 w = ri.geometric.onb.Transform(
									Vector3(cosPhi * sinTheta,sinPhi * sinTheta,cosTheta)
								);

								RISEPel lij(0,0,0);
								RISEPel sampleIrradiance(0,0,0);

								Scalar t = 0;
								Ray fgRay(ri.geometric.ptIntersection, w);
								fgRay.Advance( kRayBias );
								if( caster.CastRay( rc, ri.geometric.rast, fgRay, lij, rs2, &t, ri.pRadianceMap, ior_stack ) ) {
								if (t > kMinHitDistance) {
									rsum += 1.0/t;
									hits++;
									sampleIrradiance = lij * pBRDF->value( w, ri.geometric );
									c = c + sampleIrradiance;
								}
							}
							irradianceVariationStats.RecordSample( sampleIrradiance );

							// Deterministic center-of-stratum sample for gradient estimation.
							const Scalar xjCenter = (Scalar(j) + Scalar(0.5)) / fM;
							const Scalar sinThetaCenter = sqrt( xjCenter );
							const Scalar cosThetaCenter = sqrt( 1.0 - xjCenter );
							const Vector3 wGrad = ri.geometric.onb.Transform(
								Vector3(cosPhiCenter * sinThetaCenter,sinPhiCenter * sinThetaCenter,cosThetaCenter)
								);

							const GradientEstimatorSample gradientSample = TraceGradientEstimatorSample(
								pScene,
								rc,
								ri.geometric.rast,
								caster,
								rs2,
								ri.geometric.ptIntersection,
								wGrad,
								*pBRDF,
								ri.geometric,
								ri.pRadianceMap,
								ior_stack
								);

							if( gradientSample.bHit ) {
								// Keep the rotational gradient in the same units as the cached
								// irradiance. Only the translational estimate gets the extra
								// discontinuity checks below.
								rotGradientTemp = rotGradientTemp + ((-sinThetaCenter/cosThetaCenter) * gradientSample.cIrradiance);
							}

							// Translational gradients are much more fragile than rotational ones.
							// Only accumulate pairwise finite differences when both neighboring
							// bins are far enough away and agree on the local surface. That
							// keeps corners, misses, and cross-object visibility jumps from
							// overwhelming the cache record.
							const Scalar sinThetam = sqrt( Scalar(j) / fM );

							if( j > 0 ) {
								const GradientPairClassification classification = ClassifyGradientPair( gradientSample, prevThetaSample );
								if( classification == eGradientPairAccepted ) {
									const Scalar denomTheta = r_min( gradientSample.dDistance, prevThetaSample.dDistance );
									const Scalar kTheta = (sinThetam * (1.0 - (Scalar(j)/fM))) / denomTheta;
									transGradient1Temp = transGradient1Temp + (gradientSample.cIrradiance - prevThetaSample.cIrradiance) * kTheta;
								}
								RecordGradientPairClassification( gradientStats, classification );
							}

							if( i > 0 ) {
								const GradientPairClassification classification = ClassifyGradientPair( gradientSample, prevPhiSamples[j] );
								if( classification == eGradientPairAccepted ) {
									const Scalar sinThetap = sqrt( Scalar(j+1) / fM );
									const Scalar denomPhi = r_min( gradientSample.dDistance, prevPhiSamples[j].dDistance );
									const Scalar kPhi = (sinThetap - sinThetam) / denomPhi;
									transGradient2Temp = transGradient2Temp + (gradientSample.cIrradiance - prevPhiSamples[j].cIrradiance) * kPhi;
								}
								RecordGradientPairClassification( gradientStats, classification );
							} else {
								firstPhiSamples[j] = gradientSample;
							}

							// Set previous
							prevThetaSample = gradientSample;
							prevPhiSamples[j] = gradientSample;
						}

						// Increment rotational gradient vector
						rotGradient[0] = rotGradient[0] + (vi.x * rotGradientTemp);
						rotGradient[1] = rotGradient[1] + (vi.y * rotGradientTemp);
						rotGradient[2] = rotGradient[2] + (vi.z * rotGradientTemp);

						// Increment translational gradient vectors
						transGradient1[0] = transGradient1[0] + (ui.x * transGradient1Temp);
						transGradient1[1] = transGradient1[1] + (ui.y * transGradient1Temp);
						transGradient1[2] = transGradient1[2] + (ui.z * transGradient1Temp);

						transGradient2[0] = transGradient2[0] + (vim.x * transGradient2Temp);
						transGradient2[1] = transGradient2[1] + (vim.y * transGradient2Temp);
						transGradient2[2] = transGradient2[2] + (vim.z * transGradient2Temp);
					}

					// Compute the second part of the translational gradient
					vim = ri.geometric.onb.Transform( Vector3( 0, 1.0, 0 ) );

					RISEPel transGradient2Temp = RISEPel(0,0,0);

					for( unsigned int j=0; j<irrM; j++ ) {
						const GradientPairClassification classification = ClassifyGradientPair( firstPhiSamples[j], prevPhiSamples[j] );
						if( classification == eGradientPairAccepted ) {
							const Scalar sinThetam = sqrt( Scalar(j) / fM );
							const Scalar sinThetap = sqrt( Scalar(j+1) / fM );
							const Scalar denom = r_min( firstPhiSamples[j].dDistance, prevPhiSamples[j].dDistance );
							const Scalar k = (sinThetap - sinThetam) / denom;
							transGradient2Temp = transGradient2Temp + (firstPhiSamples[j].cIrradiance - prevPhiSamples[j].cIrradiance) * k;
						}
						RecordGradientPairClassification( gradientStats, classification );
					}

					transGradient2[0] = transGradient2[0] + (vim.x * transGradient2Temp);
					transGradient2[1] = transGradient2[1] + (vim.y * transGradient2Temp);
					transGradient2[2] = transGradient2[2] + (vim.z * transGradient2Temp);

					// Scale the azimuthal part of the translational gradient by the
					// angular spacing between phi rings (2*pi / N_phi).  Previously
					// this used fM (N_theta) by mistake, over-amplifying the gradient
					// by N_phi/N_theta when the two counts differ.
					Scalar scale = TWO_PI / fN;

					transGradient2[0] = transGradient2[0] * scale;
					transGradient2[1] = transGradient2[1] * scale;
					transGradient2[2] = transGradient2[2] * scale;

					// Sum the two pieces together
					transGradient1[0] = transGradient1[0] + transGradient2[0];
					transGradient1[1] = transGradient1[1] + transGradient2[1];
					transGradient1[2] = transGradient1[2] + transGradient2[2];

					if( gradientStats.RejectWholeRecord() ) {
						transGradient1[0] = transGradient1[1] = transGradient1[2] = RISEPel( 0, 0, 0 );
					}

					scale = PI / Scalar(final_gather_count);
					c = c * scale;

					rotGradient[0] = rotGradient[0] * scale;
					rotGradient[1] = rotGradient[1] * scale;
					rotGradient[2] = rotGradient[2] * scale;

					// Translational gradient must be normalized with the same Monte Carlo
					// factor as irradiance/rotational gradients. Without this, corrections are
					// vastly over-amplified and create dark/bright rings around cache records.
					transGradient1[0] = transGradient1[0] * scale;
					transGradient1[1] = transGradient1[1] * scale;
					transGradient1[2] = transGradient1[2] * scale;

					delete [] prevPhiSamples;
					delete [] firstPhiSamples;
				}
				else
				{
					// No cache gradients
					// This is the old way we were doing it... not sure if this is a good idea or not...
					for( unsigned int i=0; i<final_gather_count; i++ )
					{
						ScatteredRayContainer scattered;
						pSPF->Scatter( ri.geometric, rc.random, scattered, ior_stack );

						ScatteredRay* scat = scattered.RandomlySelectDiffuse( rc.random.CanonicalRandom(), false );
						RISEPel sampleIrradiance( 0, 0, 0 );

						if( scat ) {
							RISEPel cthis;
							Scalar t = 0;
							scat->ray.Advance( kRayBias );

							if( caster.CastRay( rc, ri.geometric.rast, scat->ray, cthis, rs2, &t, ri.pRadianceMap, scat->ior_stack?scat->ior_stack:ior_stack ) ) {
								if (t > kMinHitDistance) {
									rsum += 1.0/t;
									hits++;
									sampleIrradiance = cthis * scat->kray;
									c = c + sampleIrradiance;
								}
							}

						}

						irradianceVariationStats.RecordSample( sampleIrradiance );
					}

						c = c * (1.0/Scalar(final_gather_count));
					}

					// Irradiance is physically non-negative.
					ColorMath::EnsurePositve( c );

					// Insert into cache if we are in irradiance cache pass
					if( pCache && pCache->GetTolerance() > 0 && rc.pass == RuntimeContext::PASS_IRRADIANCE_CACHE ) {
					if( rsum ) {
						// Add the indirect value to the cache
						rsum = (rsum!=0 ? Scalar(hits)/rsum : 0);
							rsum *= irradianceVariationStats.ComputeReuseScale( c, high_variation_reuse_scale );
							pCache->InsertElement( ri.geometric.ptIntersection, ri.geometric.vNormal, c, rsum, rotGradient, transGradient1 );
						}
					}
			}
		}

		// Add the result to the rasterizer state cache
		if( cache ) {
			rc.StateCache_SetState( this, c, ri.pObject, ri.geometric.rast );
		}
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function
Scalar FinalGatherShaderOp::PerformOperationNM(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar caccum,						///< [in] Current value for wavelength
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	Scalar c=0;

	// We do nothing for spectral rendering, yet
	//! @@ TODO: To be implemented

	return c;
}
