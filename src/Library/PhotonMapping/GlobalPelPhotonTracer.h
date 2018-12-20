//////////////////////////////////////////////////////////////////////
//
//  GlobalPelPhotonTracer.h - A global photon tracer, that stores
//  an RISEPel
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 14, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GLOBAL_PEL_PHOTON_TRACER_
#define GLOBAL_PEL_PHOTON_TRACER_

#include "../Interfaces/IPhotonTracer.h"
#include "PhotonTracer.h"
#include "GlobalPelPhotonMap.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class GlobalPelPhotonTracer : 
			public virtual IPhotonTracer, 
			public virtual PhotonTracer<GlobalPelPhotonMap>
		{
		protected:
			unsigned int				nMaxRecursions;
			Scalar						dExtinction;
			bool						bBranch;			///< Should the tracer branch or only follow one path?

			virtual ~GlobalPelPhotonTracer();

			// Traces a single photon through the scene until it can't trace it any longer
			void TracePhoton( 
				const Ray& ray,
				const RISEPel& power,
				GlobalPelPhotonMap& pPhotonMap,
				const bool bStorePhoton,
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			// Traces a single photon through the scene until it can't trace it any longer
			// This is what the specific instances must extend
			inline void TraceSinglePhoton( 
				const Ray& ray,
				const RISEPel& power, 
				GlobalPelPhotonMap& pPhotonMap, 
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const
			{
				TracePhoton( ray, power, pPhotonMap, true, ior_stack );
			}

			// Tells the tracer to set the photon map specifically for the scene
			inline void SetSpecificPhotonMapForScene( 
				GlobalPelPhotonMap* pPhotonMap
				) const
			{
				pScene->SetGlobalPelMap( pPhotonMap );
			}

		public:
			GlobalPelPhotonTracer( 
				const unsigned int maxR, 
				const Scalar ext, 
				const bool branch, 
				const bool dononmeshlights,
				const bool useiorstack,						///< [in] Should we use an ior stack ?
				const Scalar powerscale,
				const unsigned int temporal_samples,
				const bool regenerate
				);
		};
	}
}

#endif

