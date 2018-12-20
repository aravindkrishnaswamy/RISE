//////////////////////////////////////////////////////////////////////
//
//  CausticPelPhotonTracer.h - A caustic photon tracer, that stores
//  an RISEPel
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 8, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAUSTIC_PEL_PHOTON_TRACER_
#define CAUSTIC_PEL_PHOTON_TRACER_

#include "../Interfaces/IPhotonTracer.h"
#include "PhotonTracer.h"
#include "CausticPelPhotonMap.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class CausticPelPhotonTracer : 
			public virtual IPhotonTracer, 
			public virtual PhotonTracer<CausticPelPhotonMap>
		{
		protected:
			const unsigned int			nMaxRecursions;
			const Scalar				dExtinction;
			const bool					bBranch;			///< Should the tracer branch or only follow one path?
			const bool					bTraceReflections;	///< Should we trace reflected rays?
			const bool					bTraceRefractions;	///< Should we trace refracted rays?

			virtual ~CausticPelPhotonTracer();

			// Traces a single photon through the scene until it can't trace it any longer
			void TracePhoton( 
				const Ray& ray, 
				const RISEPel& power, 
				bool bFromSpecular, 
				CausticPelPhotonMap& pPhotonMap,
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			// Traces a single photon through the scene until it can't trace it any longer
			// This is what the specific instances must extend
			inline void TraceSinglePhoton( 
				const Ray& ray,
				const RISEPel& power, 
				CausticPelPhotonMap& pPhotonMap,
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const
			{
				TracePhoton( ray, power, false, pPhotonMap, ior_stack);
			}

			// Tells the tracer to set the photon map specifically for the scene
			inline void SetSpecificPhotonMapForScene( 
				CausticPelPhotonMap* pPhotonMap
				) const
			{
				pScene->SetCausticPelMap( pPhotonMap );
			}

		public:
			CausticPelPhotonTracer(
				const unsigned int maxR, 
				const Scalar ext, 
				const bool branch, 
				const bool reflect, 
				const bool refract, 
				const bool dononmeshlights,
				const bool useiorstack,
				const Scalar powerscale,
				const unsigned int temporal_samples,
				const bool regenerate
				);
		};
	}
}

#endif

