//////////////////////////////////////////////////////////////////////
//
//  CausticSpectralPhotonTracer.h - A caustic photon tracer, that stores
//  spectra
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 8, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAUSTIC_SPECTRAL_PHOTON_TRACER_
#define CAUSTIC_SPECTRAL_PHOTON_TRACER_

#include "../Interfaces/IPhotonTracer.h"
#include "SpectralPhotonTracer.h"
#include "CausticSpectralPhotonMap.h"

namespace RISE
{
	namespace Implementation
	{
		class CausticSpectralPhotonTracer : 
			public virtual IPhotonTracer,
			public virtual SpectralPhotonTracer<CausticSpectralPhotonMap>
		{
		protected:
			const unsigned int			nMaxRecursions;
			const Scalar				dExtinction;
			const bool					bBranch;				///< Should the tracer branch or only follow one path?
			const bool					bTraceReflections;		///< Should we trace reflected rays?
			const bool					bTraceRefractions;		///< Should we trace refracted rays?

			virtual ~CausticSpectralPhotonTracer();

			// Traces a single photon through the scene until it can't trace it any longer
			void TracePhoton( 
				const Ray& ray, 
				const Scalar power, 
				const Scalar nm, 
				bool bFromSpecular, 
				CausticSpectralPhotonMap& pPhotonMap,
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			// Traces a single photon through the scene until it can't trace it any longer
			// This is what the specific instances must extend
			inline void TraceSinglePhoton( 
				const Ray& ray,
				const Scalar power, 
				const Scalar nm,
				CausticSpectralPhotonMap& pPhotonMap, 
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const
			{
				TracePhoton( ray, power, nm, false, pPhotonMap, ior_stack);
			}

			// Tells the tracer to set the photon map specifically for the scene
			inline void SetSpecificPhotonMapForScene( 
				CausticSpectralPhotonMap* pPhotonMap
				) const
			{
				pScene->SetCausticSpectralMap( pPhotonMap );
			}

		public:
			CausticSpectralPhotonTracer( 
				const unsigned int maxR,					///< [in] Maximum recursion depth
				const Scalar ext,							///< [in] Minimum importance for a photon before extinction
				const Scalar nm_begin,						///< [in] Wavelength to start shooting photons at
				const Scalar nm_end,						///< [in] Wavelength to end shooting photons at
				const unsigned int num_wavelengths,			///< [in] Number of wavelengths to shoot photons at
				const bool useiorstack,						///< [in] Should we use an ior stack ?
				const bool branch,							///< [in] Should the tracer branch or only follow one path?
				const bool reflect,							///< Should we trace reflected rays?
				const bool refract,							///< Should we trace refracted rays?
				const Scalar powerscale,
				const unsigned int temporal_samples,
				const bool regenerate
				);
		};
	}
}

#endif

