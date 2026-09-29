//////////////////////////////////////////////////////////////////////
//
//  SpectralPhotonTracer.h - Helper class for spectral
//    photon tracers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 23, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPECTRAL_PHOTON_TRACER_
#define SPECTRAL_PHOTON_TRACER_

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
		template< class PhotonMapType >
		class SpectralPhotonTracer : 
			public virtual IPhotonTracer, 
			public virtual Reference
		{
		protected:
			const Scalar				nm_begin;				///< Wavelength to start shooting photons at
			const Scalar				nm_end;					///< Wavelength to end shooting photons at
			const Scalar				num_wavelengths;		///< Number of wavelengths to shoot photons at
			const Scalar				dPowerScale;			///< How much to scale shooting power by
			const unsigned int			nNumTemporalSamples;	///< Number of temporal samples to take when tracing at a particular time
			const bool					bRegenerateSpecificTime;///< Should the photon map regenerate when asked to for a specific time?
			IScenePriv*					pScene;
			LuminaryManager*			pLumManager;

			const RandomNumberGenerator	geomsampler;
			const RandomNumberGenerator	random;

			SpectralPhotonTracer(
				const Scalar nm_begin_,						///< [in] Wavelength to start shooting photons at
				const Scalar nm_end_,						///< [in] Wavelength to end shooting photons at
				const unsigned int num_wavelengths_,		///< [in] Number of wavelengths to shoot photons at
				const Scalar power_scale,
				const unsigned int temporal_samples,
				const bool regenerate
				) : 
			nm_begin( nm_begin_ ),
			nm_end( nm_end_ ),
			num_wavelengths( num_wavelengths_ ),
			dPowerScale( power_scale ),
			nNumTemporalSamples( temporal_samples ),
			bRegenerateSpecificTime( regenerate ),
			pScene( 0 ),
			pLumManager( 0 )
			{
				pLumManager = new LuminaryManager();
				GlobalLog()->PrintNew( pLumManager, __FILE__, __LINE__, "luminary manager" );
			}

			virtual ~SpectralPhotonTracer()
			{
				safe_release( pScene );
				safe_release( pLumManager );
			}
			
			// Traces a single photon through the scene until it can't trace it any longer
			// This is what the specific instances must extend
			virtual void TraceSinglePhoton(
				const Ray& ray,
				const Scalar power,
				const Scalar nm,
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
                for (const auto& entry : pLumManager->getLuminaries()) {
                    const IObject* object = entry.pLum;
                    const bool twoSided = object->GetGeometry() && object->GetGeometry()->IsDoubleSided();
                    const Scalar area = object->GetArea()*EmitterSides::FaceCount(twoSided);
                    add(object, nullptr, ColorMath::MaxValue(object->GetMaterial()->GetEmitter()->averageRadiantExitance())*area);
                }
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
						// To find out where the photon starts off, ask the luminary for a uniform random point
						Ray	r;
						Vector3 normal;
						Point2 coord;
						object->UniformRandomPoint( &r.origin, &normal, &coord, Point3( geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom() ) );
						
						// DL-320: the emitting face, chosen by remapping the
						// first direction coordinate (no extra draw), so a
						// face selection does not require another draw.
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
						rig.ray = r;
						rig.vNormal = normal;
						// `UniformRandomPoint` returns the geometric face
						// normal on luminary meshes; mirror so vGeomNormal
						// is not default-zero for any downstream reader.
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

						// Each photon gets a different wavelength...
						const Scalar nm = pPhotonMap->SampleWavelength(random.CanonicalRandom());
						const Scalar power = pEmitter->averageRadiantExitanceNM(nm) * area * dPowerScale * batchWeight / q;

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
						TraceSinglePhoton( r, power, nm, *pPhotonMap, ior_stack );

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
					GlobalLog()->PrintSourceInfo( "SpectralPhotonTracer::AttachScene:: Attaching same scene", __FILE__, __LINE__ );
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

				// Initialize the translucent map in the scene
				PhotonMapType*	pPhotonMap = new PhotonMapType( numPhotons, this );
				if( pPhotonMap ) {
					GlobalLog()->PrintNew( pPhotonMap, __FILE__, __LINE__, "photon map" );
				} else {
					GlobalLog()->PrintEasyError( "TracePhotons:: Failed to create photon map" );
					return false;
				}

                if(!pPhotonMap->ConfigureWavelengthSampling(nm_begin,nm_end,num_wavelengths)){
                    GlobalLog()->PrintEasyError("TracePhotons:: invalid wavelength sampling law");safe_release(pPhotonMap);return false;
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

