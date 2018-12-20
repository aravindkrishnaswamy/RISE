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

#include "../Interfaces/IPhotonTracer.h"
#include "../Utilities/Reference.h"
#include "../Rendering/LuminaryManager.h"

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
			const bool					bUseIORStack;			///< Should we use an ior stack ?
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
				const bool useiorstack,
				const Scalar power_scale,
				const unsigned int temporal_samples,
				const bool regenerate
				) : 
			nm_begin( nm_begin_ ),
			nm_end( nm_end_ ),
			num_wavelengths( num_wavelengths_ ),
			bUseIORStack( useiorstack ),
			dPowerScale( power_scale ),
			nNumTemporalSamples( temporal_samples ),
			bRegenerateSpecificTime( regenerate ),
			pScene( 0 ),
			pLumManager( 0 )
			{
				pLumManager = new LuminaryManager(false);
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
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
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

				const Scalar wavelength_steps = (nm_end-nm_begin)/Scalar(num_wavelengths);
				for( i=lum.begin(), e=lum.end(); i!=e; i++ )
				{
					const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
					const Scalar area = (*i).pLum->GetArea();
					const RISEPel pelpower = (*i).pLum->GetMaterial()->GetEmitter()->averageRadiantExitance() * (area*INV_PI) * dPowerScale;
					const Scalar area_premul = area * dPowerScale;

					unsigned int numshot_thislum = 0;
					const unsigned int numstored_sofar = pPhotonMap->NumStored();

					// Trace their photons	
					unsigned int thislummax = (unsigned int)(ColorMath::MaxValue(pelpower)/total_exitance * numPhotons) + pPhotonMap->NumStored();
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
						rig.ray = r;
						rig.vNormal = normal;
						rig.ptCoord = coord;
						rig.onb.CreateFromW( rig.vNormal );

						r.dir = pEmitter->getEmmittedPhotonDir( rig, Point2( geomsampler.CanonicalRandom(), geomsampler.CanonicalRandom() ) );

						// Each photon gets a different wavelength...
						const Scalar nm = num_wavelengths < 10000 ? 
							nm_begin + int(random.CanonicalRandom()*Scalar(num_wavelengths)) * wavelength_steps : 
							nm_begin + random.CanonicalRandom() * (nm_end-nm_begin);
						const Scalar power = pEmitter->averageRadiantExitanceNM(nm) * area_premul;

						// Now shoot that ray as a photon
						TraceSinglePhoton( r, power, nm, *pPhotonMap, bUseIORStack?&ior_stack:0 );

						numshot++;
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

				// Find out how many luminaries there are
				const LuminaryManager::LuminariesList& lum = pLumManager->getLuminaries();

				GlobalLog()->PrintEx( eLog_Event, "TracePhotons:: Trying to capture %d photons", numPhotons );

				// Initialize the translucent map in the scene
				PhotonMapType*	pPhotonMap = new PhotonMapType( numPhotons, this );
				if( pPhotonMap ) {
					GlobalLog()->PrintNew( pPhotonMap, __FILE__, __LINE__, "photon map" );
				} else {
					GlobalLog()->PrintEasyError( "TracePhotons:: Failed to create photon map" );
					return false;
				}

				if( pFunc ) {
					pFunc->Progress(0.0,1.0);
				}

				// Now for each luminaire....
				LuminaryManager::LuminariesList::const_iterator	i, e;

				Scalar total_exitance=0;
				// Compute the total power of all luminaires
				for( i=lum.begin(), e=lum.end(); i!=e; i++ ) {
					//! \todo Something to consider.  Get the averageRadiantExitanceSpecrum, then use the peak value in the
					//! spectrum.  It is arguable which way is better.  If anyone has any suggestions, I'm willing to 
					//! listen
					const Scalar area = (*i).pLum->GetArea();
					const RISEPel power = (*i).pLum->GetMaterial()->GetEmitter()->averageRadiantExitance() * (area*INV_PI) * dPowerScale;
					total_exitance += ColorMath::MaxValue(power);
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
							if( !pFunc->Progress(static_cast<double>(cnt), static_cast<double>(numPhotons)) ) {
								break;		// abort
							}
						}
					}
				} else {
					if( pFunc && numPhotons > 100 ) {
						for( int i=0; i<100; i++ ) {
							TraceNPhotons( numPhotons/100, pPhotonMap, total_exitance, numshot );
							const unsigned int cnt = pPhotonMap->NumStored();
							if( !pFunc->Progress(static_cast<double>(cnt), static_cast<double>(numPhotons)) ) {
								break;		// abort
							}
						}
					} else {
						TraceNPhotons( numPhotons, pPhotonMap, total_exitance, numshot );
					}
				}
				if( pFunc && pPhotonMap->NumStored() != numPhotons ) {
					pFunc->Progress(1.0,1.0);
				}

				// After shooting, scale the values in the photon map
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

