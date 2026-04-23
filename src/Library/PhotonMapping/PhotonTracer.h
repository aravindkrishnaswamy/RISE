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

#include "../Interfaces/IPhotonTracer.h"
#include "../Utilities/Reference.h"
#include "../Rendering/LuminaryManager.h"

namespace RISE
{
	namespace Implementation
	{
		//
		// THREAD-SAFETY STATUS: SINGLE-THREADED ONLY.
		//
		// `TracePhotons()` runs on one thread and iterates luminaires +
		// photons serially.  Everything below is sized for that contract.
		// Parallelising the photon shoot — which is the obvious win for
		// big scenes — has to address each of these points explicitly:
		//
		//   1. `pScene` (mutable IScenePriv*) is written by AttachScene()
		//      during setup and read inside TraceNPhotons().  Fine today
		//      because AttachScene() always completes before trace begins
		//      and there is no concurrent AttachScene.  Parallel shoots
		//      can keep this as setup-only read-shared state — DO NOT
		//      call AttachScene from any worker.
		//
		//   2. `geomsampler` and `random` are RandomNumberGenerator
		//      instances whose MersenneTwister is `mutable`.  Concurrent
		//      CanonicalRandom() calls across threads race on the MT
		//      state.  Parallelisation needs per-thread RNGs seeded from
		//      a master stream (see RuntimeContext::random for the
		//      pattern the rasterizers use).
		//
		//   3. TraceNPhotons' inner loop is bounded by
		//      `pPhotonMap->NumStored() < thislummax`, and each
		//      TraceSinglePhoton deposits into the same PhotonMapType.
		//      Going parallel requires: (a) PhotonMapType::Store() to be
		//      thread-safe (atomic append or per-thread bucket then
		//      merge), (b) the loop-bound check tolerant of NumStored()
		//      drifting (use an atomic counter + compare-and-fetch), and
		//      (c) `numshot` converted to std::atomic<unsigned int> or
		//      accumulated per-thread and reduced after the pool joins.
		//
		//   4. `pScene->GetAnimator()->EvaluateAtTime()` inside the
		//      temporal-sampling branch mutates scene transforms and is
		//      absolutely NOT safe to run concurrently with shooting.
		//      Keep the temporal loop serial and only parallelise
		//      photon-shooting within a single time step.
		//
		//   5. LuminaryManager's luminaire list iteration is read-only,
		//      but `(*i).pLum` exposes methods that internally cache
		//      precomputed area integrals — verify those are frozen
		//      after scene prep before allowing workers to hit them.
		//
		// Parallel design sketch: make TraceNPhotons launch a thread
		// pool, each worker shoots a share of the target count for its
		// assigned luminaire, emits photons into a thread-local batch,
		// and the batches are merged into the shared PhotonMapType
		// under a single lock (or via a lock-free append buffer) once
		// the pool barrier is reached.  Temporal-sampling loop stays
		// serial.
		//
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
			// shoot is parallelised.  See class docstring point (2).
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
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const = 0;

			// Tells the tracer to set the photon map specifically for the scene
			virtual void SetSpecificPhotonMapForScene( 
				PhotonMapType* pPhotonMap
				) const = 0;

			void TraceNPhotons(
				const unsigned int numPhotons,
				PhotonMapType*	pPhotonMap, 
				const Scalar total_exitance,
				int& numshot
				) const
			{
				const LuminaryManager::LuminariesList& lum = pLumManager->getLuminaries();
				LuminaryManager::LuminariesList::const_iterator	i, e;

				IORStack ior_stack( 1.0 );

				// Then from now on each luminaire will only shoot photons proportional to its relative power
				if( bShootFromMeshLights )
				for( i=lum.begin(), e=lum.end(); i!=e; i++ )
				{
					const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
					const Scalar area = (*i).pLum->GetArea();
					const RISEPel totalpower = (*i).pLum->GetMaterial()->GetEmitter()->averageRadiantExitance() * area;
					const RISEPel power = pEmitter->averageRadiantExitance() * area * dPowerScale;

					unsigned int numshot_thislum = 0;
					const unsigned int numstored_sofar = pPhotonMap->NumStored();

					// Trace their photons
					unsigned int thislummax = (unsigned int)(ColorMath::MaxValue(totalpower)/total_exitance * numPhotons) + pPhotonMap->NumStored();
					thislummax = thislummax > pPhotonMap->MaxPhotons() ? pPhotonMap->MaxPhotons() : thislummax;
					while( pPhotonMap->NumStored() < thislummax )
					{
						//! This is a slightly dangerous fix, since it is entirely possible to only have a very small amount of photos
						//  be deposited for a luminary and since we are dealing with a stochastic process, there is a chance (albeit
						//  a trivially small chance) of having this introduce a bug.  However in any realworld use, this won't be a
						//  problem, hence the fix is here.
						if( (unsigned int)numshot_thislum > thislummax*100 ) {
							// bugfix for infinite loop when no photon is stored
							// e.g. when no suitable material is in the scene
							// if there is not at least one percent of the shot photons
							// gathered then break
							if( numstored_sofar == pPhotonMap->NumStored() ) {
								break;
							}
						}

						// To find out where the photon starts off, ask the luminary for a uniform random point
						Ray	r;
						Vector3 normal;
						Point2 coord;
						i->pLum->UniformRandomPoint( &r.origin, &normal, &coord, Point3( geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom() ) );

						RayIntersectionGeometric rig( r, nullRasterizerState );
						rig.vNormal = normal;
						rig.ptCoord = coord;
						rig.onb.CreateFromW( rig.vNormal );

						r.SetDir(pEmitter->getEmmittedPhotonDir( rig, Point2( geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom() ) ));

						// Now shoot that ray as a photon
						TraceSinglePhoton( r, power, *pPhotonMap, ior_stack );

						numshot_thislum++;
					}

					numshot += numshot_thislum;
				}

				// Do the non-mesh based lights
				if( bShootFromNonMeshLights ) {
					const ILightManager::LightsList& lights = pScene->GetLights()->getLights();
					ILightManager::LightsList::const_iterator	m, n;

					for( m=lights.begin(), n=lights.end(); m!=n; m++ ) {
						const ILightPriv* l = *m;
						if( l->CanGeneratePhotons() ) {
							const RISEPel totalpower = l->radiantExitance();
							unsigned int thislummax = (unsigned int)(ColorMath::MaxValue(totalpower)/total_exitance * numPhotons) + pPhotonMap->NumStored();
							thislummax = thislummax > pPhotonMap->MaxPhotons() ? pPhotonMap->MaxPhotons() : thislummax;

							unsigned int numshot_thislum = 0;
							const unsigned int numstored_sofar = pPhotonMap->NumStored();

							while( pPhotonMap->NumStored() < thislummax )
							{
								//! This is a slightly dangerous fix, since it is entirely possible to only have a very small amount of photos
								//  be deposited for a luminary and since we are dealing with a stochastic process, there is a chance (albeit
								//  a trivially small chance) of having this introduce a bug.  However in any realworld use, this won't be a
								//  problem, hence the fix is here.
								if( (unsigned int)numshot_thislum > thislummax*100 ) {
									// bugfix for infinite loop when no photon is stored
									// e.g. when no suitable material is in the scene
									// if there is not at least one percent of the shot photons
									// gathered then break
									if( numstored_sofar == pPhotonMap->NumStored() ) {
										break;
									}
								}

								Ray r;
								r = l->generateRandomPhoton( Point3(geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom()) );

								// Weight each photon by emittedRadiance/pdfDirection so that
								// lights with non-uniform emission profiles (e.g. spot light
								// inner/outer cone falloff) produce correctly weighted photons.
								const Scalar pdf = l->pdfDirection( r.Dir() );
								const RISEPel power = (pdf > 0) ?
									l->emittedRadiance( r.Dir() ) * (dPowerScale / pdf) :
									RISEPel(0,0,0);

								TraceSinglePhoton( r, power, *pPhotonMap, ior_stack );

								numshot_thislum++;
							}

							numshot += numshot_thislum;
						}
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
				
				// Find out how many luminaries there are
				const LuminaryManager::LuminariesList& lum = pLumManager->getLuminaries();

				GlobalLog()->PrintEx( eLog_Event, "TracePhotons:: Trying to capture %d photons", numPhotons );

				// Initialize the map in the scene
				PhotonMapType*	pPhotonMap = new PhotonMapType( numPhotons, this );
				if( pPhotonMap ) {
					GlobalLog()->PrintNew( pPhotonMap, __FILE__, __LINE__, "photon map" );
				} else {
					GlobalLog()->PrintEasyError( "TracePhotons:: Failed to create photon map" );
					return false;
				}

				if( pFunc ) {
					pFunc->SetTitle( "Shooting Photons: " );
					pFunc->Progress(0.0,1.0);
				}

				// Now for each luminaire....
				LuminaryManager::LuminariesList::const_iterator	i, e;

				Scalar total_exitance=0;
				// Compute the total power of all luminaires
				if( bShootFromMeshLights ) {
					for( i=lum.begin(), e=lum.end(); i!=e; i++ ) {
						const Scalar area = (*i).pLum->GetArea();
						const RISEPel power = (*i).pLum->GetMaterial()->GetEmitter()->averageRadiantExitance() * area;
						total_exitance += ColorMath::MaxValue(power);
					}
				}

				// Include the non-physically based luminaires as well
				const ILightManager::LightsList& lights = pScene->GetLights()->getLights();
				ILightManager::LightsList::const_iterator	m, n;

				if( bShootFromNonMeshLights ) {
					for( m=lights.begin(), n=lights.end(); m!=n; m++ ) {
						const ILightPriv* l = *m;
						if( l->CanGeneratePhotons() ) {
							total_exitance = total_exitance + ColorMath::MaxValue(l->radiantExitance());
						}
					}
				}

				int numshot = 0;

				const Scalar exposure = pScene->GetCamera()->GetExposureTime();

				if( bAtTime && exposure > 0 && nNumTemporalSamples>1 ) {
					const Scalar time_step = exposure/Scalar(nNumTemporalSamples);
					Scalar base_cur_time = (time-(exposure*0.5));

					for( unsigned int i=0; i<nNumTemporalSamples; i++, base_cur_time+=time_step ) {
						// Set a new time
						if( exposure > 0 ) {
							pScene->GetAnimator()->EvaluateAtTime( base_cur_time + (random.CanonicalRandom()*time_step) );
						}

						TraceNPhotons( numPhotons/nNumTemporalSamples, pPhotonMap, total_exitance, numshot );

						if( pFunc ) {
							const unsigned int cnt = pPhotonMap->NumStored();
							if( !pFunc->Progress(static_cast<Scalar>(cnt), static_cast<Scalar>(numPhotons)) ) {
								break;		// abort
							}
						}
					}
				} else {
					if( pFunc && numPhotons > 100 ) {
						for( int i=0; i<100; i++ ) {
							TraceNPhotons( numPhotons/100, pPhotonMap, total_exitance, numshot );
							const unsigned int cnt = pPhotonMap->NumStored();
							if( !pFunc->Progress(static_cast<Scalar>(cnt), static_cast<Scalar>(numPhotons)) ) {
								break;		// abort
							}
						}
					} else {
						TraceNPhotons( numPhotons, pPhotonMap, total_exitance, numshot );
					}
				}

				if( pFunc && pPhotonMap->NumStored() != numPhotons ) {
					pFunc->Progress(0,0);
				}

				// After shooting, scale the values in the photon map
				GlobalLog()->PrintEx( eLog_Event, "TracePhotons:: Stored %d of %d requested photons (%d shot)", pPhotonMap->NumStored(), numPhotons, numshot );
				pPhotonMap->ScalePhotonPower( 1.0/Scalar(numshot) );

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

