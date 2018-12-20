//////////////////////////////////////////////////////////////////////
//
//  ShadowPhotonTracer.h - A shadow photon tracer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 15, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHADOW_PHOTON_TRACER_
#define SHADOW_PHOTON_TRACER_

#include "../Interfaces/IPhotonTracer.h"
#include "PhotonTracer.h"
#include "ShadowPhotonMap.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class ShadowPhotonTracer :
			public virtual IPhotonTracer, 
			public virtual PhotonTracer<ShadowPhotonMap>
		{
		protected:
			virtual ~ShadowPhotonTracer();

			// Traces a single photon through the scene until it can't trace it any longer
			void TracePhoton( 
				const Ray& ray,
				const bool bOccluded,
				ShadowPhotonMap& pPhotonMap
				) const;

			// Traces a single photon through the scene until it can't trace it any longer
			// This is what the specific instances must extend
			inline void TraceSinglePhoton( 
				const Ray& ray,
				const RISEPel& power, 
				ShadowPhotonMap& pPhotonMap, 
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const
			{
				TracePhoton( ray, false, pPhotonMap );
			}

			// Tells the tracer to set the photon map specifically for the scene
			inline void SetSpecificPhotonMapForScene( 
				ShadowPhotonMap* pPhotonMap
				) const
			{
				pScene->SetShadowMap( pPhotonMap );
			}

		public:
			ShadowPhotonTracer( 
				const unsigned int temporal_samples,
				const bool regenerate
				);
		};
	}
}

#endif

