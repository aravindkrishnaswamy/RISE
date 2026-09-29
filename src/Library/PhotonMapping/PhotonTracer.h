//////////////////////////////////////////////////////////////////////
//
//  PhotonTracer.h - Helper class for photon tracers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 23, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PHOTON_TRACER_
#define PHOTON_TRACER_

#include "../Utilities/IndependentSampler.h"
#include "../Lights/LightSampler.h"
#include "../Interfaces/IPhotonTracer.h"
#include "../Utilities/Reference.h"
#include "../Utilities/IORStackSeeding.h"
#include "../Rendering/LuminaryManager.h"
#include "../Interfaces/IGeometry.h"
#include "../Interfaces/IEmitter.h"
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace RISE
{
	namespace Implementation
	{
        // Single-threaded emission estimator: fixed attempted budget, sources
        // sampled proportional to unmasked power, packets divided by source PDF.
        // Shared RNGs and animation mutation require serial shooting. Deposits
        // use a bounded uniform reservoir independently of path continuation.
		template< class PhotonMapType >
		class PhotonTracer :
			public virtual IPhotonTracer,
			public virtual Reference
		{
		protected:
			const bool					bShootFromNonMeshLights;///< Should we shoot from non mesh based lights?
			const bool					bShootFromMeshLights;	///< Should we shoot from mesh based lights (luminaries)?
			const Scalar				dPowerScale;			///< How much to scale shooting power by
			const unsigned int			nNumTemporalSamples;	///< Number of temporal samples to take when tracing at a particular time
			const bool					bRegenerateSpecificTime;///< Should the photon map regenerate when asked to for a specific time?
			mutable IScenePriv*			pScene;					///< Scene pointer, setup-only writes via AttachScene() (see class docstring for parallel-shoot constraints)
			LuminaryManager*			pLumManager;

			// Shared RNG state: MUST be made per-thread before the photon
			// shoot is parallelised. Scene animation and map storage are serial too.
			const RandomNumberGenerator	geomsampler;
			const RandomNumberGenerator random;

			PhotonTracer(
				const bool shootFromNonMeshLights,
				const Scalar power_scale,
				const unsigned int temporal_samples,
				const bool regenerate,
				const bool shootFromMeshLights = true
				) :
			  bShootFromNonMeshLights( shootFromNonMeshLights ),
			  bShootFromMeshLights( shootFromMeshLights ),
			  dPowerScale( power_scale ),
			  nNumTemporalSamples( temporal_samples ),
			  bRegenerateSpecificTime( regenerate ),
			  pScene( 0 ),
			  pLumManager( 0 )
			{
				pLumManager = new LuminaryManager();
				GlobalLog()->PrintNew( pLumManager, __FILE__, __LINE__, "luminary manager" );
			}

			virtual ~PhotonTracer()
			{
				safe_release( pScene );
				safe_release( pLumManager );
			}
			
			// Traces a single photon through the scene until it can't trace it any longer
			// This is what the specific instances must extend
			virtual void TraceSinglePhoton(
				const Ray& ray,
				const RISEPel& power,
				PhotonMapType& pPhotonMap,
				const IORStack& ior_stack								///< [in] Index of refraction stack
				) const = 0;

			// Tells the tracer to set the photon map specifically for the scene
			virtual void SetSpecificPhotonMapForScene( 
				PhotonMapType* pPhotonMap
				) const = 0;

            void TraceNPhotons(const unsigned int numPhotons, PhotonMapType* pPhotonMap,
                const Scalar /*legacyTotalExitance*/, uint64_t& numshot, Scalar batchWeight = 1) const
            {
                struct Source { const IObject* object; const ILightPriv* light; Scalar weight; };
                std::vector<Source> sources;
                Scalar total = 0;
                const auto add = [&](const IObject* object, const ILightPriv* light, Scalar weight) {
                    if (weight > 0 && std::isfinite(weight)) { sources.push_back({object,light,weight}); total += weight; }
                };
                if (bShootFromMeshLights)
                for (const auto& entry : pLumManager->getLuminaries()) {
                    const IObject* object = entry.pLum;
                    const bool twoSided = object->GetGeometry() && object->GetGeometry()->IsDoubleSided();
                    const Scalar area = object->GetArea()*EmitterSides::FaceCount(twoSided);
                    add(object, nullptr, ColorMath::MaxValue(object->GetMaterial()->GetEmitter()->averageRadiantExitance())*area);
                }
                if (bShootFromNonMeshLights && pScene->GetLights())
                    for (const auto* light : pScene->GetLights()->getLights())
                        if (light->CanGeneratePhotons()) add(nullptr, light, ColorMath::MaxValue(light->radiantExitance()));
                for (unsigned int attempt = 0; attempt < numPhotons; ++attempt) {
                    ++numshot; // includes rejected alpha, zero power, and zero deposits
                    if (!(total > 0)) continue;
                    Scalar selection = geomsampler.CanonicalRandom()*total;
                    const Source* selected = &sources.back();
                    for (const auto& source : sources) {
                        selection -= source.weight;
                        if (selection < 0) { selected = &source; break; }
                    }
                    const Scalar q = selected->weight/total;
                    if (selected->object) {
                        const IObject* object = selected->object;
                        const IEmitter* pEmitter = object->GetMaterial()->GetEmitter();
                        const bool twoSided = object->GetGeometry() && object->GetGeometry()->IsDoubleSided();
                        const Scalar area = object->GetArea()*EmitterSides::FaceCount(twoSided);
                        const RISEPel power = pEmitter->averageRadiantExitance()*area*(dPowerScale*batchWeight/q);
						// To find out where the photon starts off, ask the luminary for a uniform random point
						Ray	r;
						Vector3 normal;
						Point2 coord;
						object->UniformRandomPoint( &r.origin, &normal, &coord, Point3( geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom() ) );

						// DL-320: the emitting face.  The first direction
						// coordinate is remapped to pick it (no extra draw),
						// without an additional face-selection draw.
						Point2 dirRand( geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom() );
						if( twoSided ) {
							if( dirRand.x < 0.5 ) {
								dirRand.x = dirRand.x * 2.0;
							} else {
								dirRand.x = dirRand.x * 2.0 - 1.0;
								normal = -normal;
							}
						}

						RayIntersectionGeometric rig( r, nullRasterizerState );
						rig.vNormal = normal;
						// `UniformRandomPoint` returns the geometric face
						// normal on luminary meshes; mirror it so any
						// downstream consumer that reads vGeomNormal sees
						// a populated value rather than default-zero.
						rig.vGeomNormal = normal;
						rig.ptCoord = coord;
						rig.onb.CreateFromW( rig.vNormal );

                        rig.ptIntersection = r.origin;
                        rig.ptObjIntersec = LightSampler::EmitterObjectPoint(object, r.origin, r.origin);
                        EmitterSurfacePayload alphaSurface;
                        LightSampler::ProbeEmitterSurface(object, pScene->GetObjects(), r.origin, normal, alphaSurface);
                        LightSampler::ApplyEmitterSurface(rig, alphaSurface);
                        IndependentSampler alphaSampler(random);
                        if (!LightSampler::AcceptEmitterAlpha(object, pScene->GetObjects(), r.origin, rig, alphaSampler)) {
                            continue;
                        }
						r.SetDir(pEmitter->getEmmittedPhotonDir( rig, dirRand ));

						// Fresh per-photon stack seeded from THIS photon's
						// origin: a luminaire sealed inside nested
						// dielectrics (e.g. a light in a glass egg) needs
						// its emitted photon to see bFromInside==true at
						// the first boundary crossing, or DielectricSPF
						// misclassifies it and drops the transmission
						// lobe.  Also fixes a latent state-leak: the old
						// shared ior_stack could carry residual state
						// between unrelated photons.
						IORStack ior_stack( 1.0 );
						IORStackSeeding::SeedFromPoint( ior_stack, r.origin, *pScene );

						// Now shoot that ray as a photon
						TraceSinglePhoton( r, power, *pPhotonMap, ior_stack );

                    } else {
                        const ILightPriv* light = selected->light;
                        const Ray ray = light->generateRandomPhoton(Point3(geomsampler.CanonicalRandom(),geomsampler.CanonicalRandom(),geomsampler.CanonicalRandom()));
                        const Scalar pdf = light->pdfDirection(ray.Dir());
                        const RISEPel power = pdf > 0 ? light->emittedRadiance(ray.Dir())*(dPowerScale*batchWeight/(q*pdf)) : RISEPel(0,0,0);
                        IORStack stack(1.0);IORStackSeeding::SeedFromPoint(stack,ray.origin,*pScene);
                        TraceSinglePhoton(ray,power,*pPhotonMap,stack);
                    }
                }
            }

		public:
			// Attaches a scene
			void AttachScene( 
				IScenePriv* pScene_ 
				)
			{
				if( pScene == pScene_ ) {
					GlobalLog()->PrintSourceInfo( "PhotonTracer::AttachScene:: Attaching same scene", __FILE__, __LINE__ );
					return;
				}

				if( pScene_ ) {
					safe_release( pScene );

					pScene = pScene_;
					pScene->addref();
					pLumManager->AttachScene( pScene );
				}
			}

			//! Traces photons
			virtual bool TracePhotons( 
				const unsigned int numPhotons,
				const Scalar time,					///< [in] The time to trace these photons at
				const bool bAtTime,					///< [in] Should we be tracing photons at a particular time?
				IProgressCallback* pFunc			///< [in] Callback functor for reporting progress
				) const
			{
				if( bAtTime && !bRegenerateSpecificTime ) {
					return false;
				}
				
				GlobalLog()->PrintEx( eLog_Event, "TracePhotons:: Launching %u photon emission attempts", numPhotons );

				// Initialize the map in the scene
				PhotonMapType*	pPhotonMap = new PhotonMapType( numPhotons, this );
				if( pPhotonMap ) {
					GlobalLog()->PrintNew( pPhotonMap, __FILE__, __LINE__, "photon map" );
				} else {
					GlobalLog()->PrintEasyError( "TracePhotons:: Failed to create photon map" );
					return false;
				}

                if (!pPhotonMap->EnableReservoir()) { safe_release(pPhotonMap); return false; }
                if (pFunc) {
                    pFunc->SetTitle("Shooting photon emission attempts: ");
                    if (!pFunc->Progress(0, numPhotons)) { safe_release(pPhotonMap); return false; }
                }
                uint64_t numshot = 0;
                const Scalar exposure = pScene->GetCamera() ? pScene->GetCamera()->GetExposureTime() : 0;
                const bool temporal = bAtTime && exposure > 0 && nNumTemporalSamples > 1 && numPhotons > 0;
                const unsigned strata = temporal ? std::min(nNumTemporalSamples,numPhotons) : 1;
                for (unsigned stratum=0; stratum<strata; ++stratum) {
                    const unsigned count = numPhotons/strata + (stratum < numPhotons%strata ? 1 : 0);
                    if (temporal) {
                        const Scalar dt = exposure/strata;
                        pScene->GetAnimator()->EvaluateAtTime(time-exposure*.5+(stratum+random.CanonicalRandom())*dt);
                    }
                    // Each equal-width time stratum has equal integral weight,
                    // even when integer attempt counts differ by one.
                    const Scalar weight = temporal ? Scalar(numPhotons)/(Scalar(strata)*count) : 1;
                    const unsigned batches = pFunc ? std::min(100u,count) : 1;
                    for (unsigned batch=0; batch<batches; ++batch) {
                        const unsigned n = count/batches + (batch < count%batches ? 1 : 0);
                        TraceNPhotons(n,pPhotonMap,0,numshot,weight);
                        if (pFunc && !pFunc->Progress(Scalar(numshot),Scalar(numPhotons))) {
                            safe_release(pPhotonMap); return false; // never publish a partial estimator
                        }
                    }
                }
                GlobalLog()->PrintEx(eLog_Event,"TracePhotons:: Stored %u packets from %llu emission attempts",pPhotonMap->NumStored(),static_cast<unsigned long long>(numshot));
                // Uniform K-of-M deposit reservoir, then attempted emission average.
                // This is the sole capacity correction; ScalePhotonPower itself
                // retains ordinary/manual-map semantics.
                const Scalar storageWeight=pPhotonMap->StorageNormalization();
                pPhotonMap->EndReservoir();
                pPhotonMap->ScalePhotonPower(numshot ? storageWeight/Scalar(numshot) : 0);

				// Tell the photon map to balance itself!
				GlobalLog()->PrintEasyEvent( "TracePhotons:: Balancing KD-Tree" );
				pPhotonMap->Balance();

				SetSpecificPhotonMapForScene( pPhotonMap );
				safe_release( pPhotonMap );

				return true;
			}
		};
	}
}

#endif

