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
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

FinalGatherShaderOp::FinalGatherShaderOp(
	const unsigned int numTheta_,
	const unsigned int numPhi_,
	const bool bComputeCacheGradients_,
	const bool cache_
	) :
  numTheta( numTheta_ ),
  numPhi( numPhi_ ),
  bComputeCacheGradients( bComputeCacheGradients_ ),
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

				if( results.size() > 0 ) {
					// There were some results, so accrue
					std::vector<IIrradianceCache::CacheElement>::const_iterator i;
					for( i=results.begin(); i!=results.end(); i++ ) {
						const IIrradianceCache::CacheElement& elem = *i;
						RISEPel temp = (elem.cIRad * r_min(1e10,elem.dWeight));

						if( bComputeCacheGradients ) {
							// Apply translational gradient
							temp = temp + (ri.geometric.ptIntersection.x-elem.ptPosition.x)*elem.translationalGradient[0];
							temp = temp + (ri.geometric.ptIntersection.y-elem.ptPosition.y)*elem.translationalGradient[1];
							temp = temp + (ri.geometric.ptIntersection.z-elem.ptPosition.z)*elem.translationalGradient[2];

							// Apply the rotational gradient
							const Vector3 cp = Vector3Ops::Cross( ri.geometric.vNormal, elem.vNormal );

							temp = temp + cp.x*elem.rotationalGradient[0];
							temp = temp + cp.y*elem.rotationalGradient[1];
							temp = temp + cp.z*elem.rotationalGradient[2];
						}

						c = c + temp;
					}

					c = c * (1.0/weights);

					bComputeIrradiance = false;
				}
			}

			// We are in compute irradiance pass...
			if( rc.pass == RuntimeContext::PASS_IRRADIANCE_CACHE && pCache && pCache->GetTolerance() > 0 ) {
				// Ask the cache if we should generate a sample
				bComputeIrradiance = pCache->IsSampleNeeded( ri.geometric.ptIntersection, ri.geometric.vNormal );
			}

			if( bComputeIrradiance && pSPF ) {
				// We need to initiate a final gather
				const unsigned int irrM = numTheta;
				const unsigned int irrN = numPhi;
				const unsigned int final_gather_count = irrM*irrN;

				IRayCaster::RAY_STATE rs2;
				rs2.depth = rs.depth + 1;
				rs2.importance = 1.0;
				rs2.considerEmission = false;
				rs2.type = IRayCaster::RAY_STATE::eRayFinalGather;

				Scalar rsum = 0;
				unsigned int hits = 0;

				// Computing the final gather over the hemisphere using rotational and
				// translational gradients
				// This code is taken almost directly from the SunFlow renderer, LightServer.java
				//  available here: http://sunflow.sourceforge.net

				RISEPel	rotGradient[3];						// The rotational gradient as we go along sampling
				RISEPel transGradient1[3];					// The first part of the translational gradient

				if( pCache && bComputeCacheGradients )
				{
					RISEPel transGradient2[3];				// The second part of the translational gradient

					const Scalar fN = Scalar(irrN);
					const Scalar fM = Scalar(irrM);

					// Irradiance gradients temporary variables
					Vector3 vim;
					RISEPel lijm;							// L_{i,j-1}

					RISEPel* lim = new RISEPel[irrM];		// L_{i-1,j}
					RISEPel* l0 = new RISEPel[irrM];		// L_{0,j}

					Scalar rijm = 0;						// R_{i,j-1}
					Scalar* rim = new Scalar[irrM];			// R_{i-1,j}
					Scalar* r0 = new Scalar[irrM];			// R_{0,j}

					for( unsigned int i=0; i<irrN; i++ ) {
						const Scalar xi = (Scalar(i) + rc.random.CanonicalRandom()) / fN;
						const Scalar phi = TWO_PI * xi;
						const Scalar cosPhi = cos(phi);
						const Scalar sinPhi = sin(phi);
						const Vector3 vi = ri.geometric.onb.Transform( Vector3(-sinPhi,cosPhi,0.0) );

						RISEPel rotGradientTemp(0,0,0);

						const Vector3 ui = ri.geometric.onb.Transform( Vector3(cosPhi,sinPhi,0.0) );
						const Scalar phim = (TWO_PI * Scalar(i)) / fN;
						vim = ri.geometric.onb.Transform(
							Vector3(cos( phim + PI_OV_TWO ), sin( phim + PI_OV_TWO ), 0.0)
							);

						RISEPel transGradient1Temp = RISEPel(0,0,0);
						RISEPel transGradient2Temp = RISEPel(0,0,0);

						for( unsigned int j=0; j<irrM; j++ ) {
							const Scalar xj = (Scalar(j) + rc.random.CanonicalRandom()) / fM;
							const Scalar sinTheta = sqrt( xj );
							const Scalar cosTheta = sqrt( 1.0 - xj );

							const Vector3 w = ri.geometric.onb.Transform(
								Vector3(cosPhi * sinTheta,sinPhi * sinTheta,cosTheta)
								);

							RISEPel lij;

							Scalar t = 0;
							if( caster.CastRay( rc, ri.geometric.rast, Ray(ri.geometric.ptIntersection,w), lij, rs2, &t, ri.pRadianceMap, ior_stack ) ) {
								if (t>0) {
									rsum += 1.0/t;
									hits++;
									c = c + lij * ri.pMaterial->GetBSDF()->value( w, ri.geometric );

									// Increment the rotational gradient
									rotGradientTemp = rotGradientTemp + ((-sinTheta/cosTheta) * lij);
								}
							}

							// Increment translational gradient
							const Scalar rij = t;
							const Scalar sinThetam = sqrt( Scalar(j) / fM );

							if( j > 0 ) {
								const Scalar k = (sinThetam * (1.0 - (Scalar(j)/fM))) / (r_min(rij,rijm));
								transGradient1Temp = (transGradient1Temp + (lij - lijm)) * k;
							}

							if( i > 0 ) {
								const Scalar sinThetap = sqrt( Scalar(j+1) / fM );
								const Scalar k = (sinThetap - sinThetam) / (r_min(rij,rim[j]));
								transGradient2Temp = (transGradient2Temp + (lij - lim[j])) * k;
							} else {
								r0[j] = rij;
								l0[j] = lij;
							}

							// Set previous
							rijm = rij;
							lijm = lij;
							rim[j] = rij;
							lim[j] = lij;
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
						const Scalar sinThetam = sqrt( Scalar(j) / fM );
						const Scalar sinThetap = sqrt( Scalar(j+1) / fM );
						const Scalar k = (sinThetap - sinThetam) / r_min(r0[j],rim[j]);
						transGradient2Temp = transGradient2Temp + (l0[j] - lim[j]) * k;
					}

					transGradient2[0] = transGradient2[0] + (vim.x * transGradient2Temp);
					transGradient2[1] = transGradient2[1] + (vim.y * transGradient2Temp);
					transGradient2[2] = transGradient2[2] + (vim.z * transGradient2Temp);

					// Scale the first part of the translational gradient
					Scalar scale = TWO_PI / fM;

					transGradient2[0] = transGradient2[0] * scale;
					transGradient2[1] = transGradient2[1] * scale;
					transGradient2[2] = transGradient2[2] * scale;

					// Sum the two pieces together
					transGradient1[0] = transGradient1[0] + transGradient2[0];
					transGradient1[1] = transGradient1[1] + transGradient2[1];
					transGradient1[2] = transGradient1[2] + transGradient2[2];

					scale = PI / Scalar(final_gather_count);
					c = c * scale;

					rotGradient[0] = rotGradient[0] * scale;
					rotGradient[1] = rotGradient[1] * scale;
					rotGradient[2] = rotGradient[2] * scale;

					delete [] rim;
					delete [] r0;

					delete [] lim;
					delete [] l0;
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

						if( scat ) {
							RISEPel cthis;
							Scalar t = 0;
							scat->ray.Advance( 1e-8 );

							if( caster.CastRay( rc, ri.geometric.rast, scat->ray, cthis, rs2, &t, ri.pRadianceMap, scat->ior_stack?scat->ior_stack:ior_stack ) ) {
								if (t>0) {
									rsum += 1.0/t;
									hits++;
									c = c + (cthis * scat->kray);
								}
							}
						}
					}

					c = c * (1.0/Scalar(final_gather_count));
				}

				// Insert into cache if we are in irradiance cache pass
				if( pCache && pCache->GetTolerance() > 0 && rc.pass == RuntimeContext::PASS_IRRADIANCE_CACHE ) {
					if( rsum ) {
						// Add the indirect value to the cache
						rsum = (rsum!=0 ? Scalar(hits)/rsum : 0);
						pCache->InsertElement( ri.geometric.ptIntersection, ri.geometric.vNormal, c, rsum, rotGradient, transGradient1 );

						c = RISEPel(0.7,0.7,0); // shows yellow when a new cache value is computed
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
