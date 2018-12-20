//////////////////////////////////////////////////////////////////////
//
//  TranslucentPelPhotonTracer.h - A translucent photon tracer, that 
//    translucent photons as RISEPels
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 19, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRANSLUCENT_PEL_PHOTON_TRACER_
#define TRANSLUCENT_PEL_PHOTON_TRACER_

#include "../Interfaces/IPhotonTracer.h"
#include "PhotonTracer.h"
#include "TranslucentPelPhotonMap.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentPelPhotonTracer :
			public virtual IPhotonTracer, 
			public virtual PhotonTracer<TranslucentPelPhotonMap>
		{
		protected:
			const unsigned int			nMaxRecursions;
			const Scalar				dExtinction;	
			const bool					bTraceReflections;	///< Should we trace reflected rays?
			const bool					bTraceRefractions;	///< Should we trace refracted rays?
			const bool					bTraceDirectTranslucent;	///< Should we trace translucent primary interactions?

			virtual ~TranslucentPelPhotonTracer();

			// Traces a single photon through the scene until it can't trace it any longer
			void TracePhoton( 
				const Ray& ray,
				const RISEPel& power, 
				const bool bFromTranslucent,
				TranslucentPelPhotonMap& pPhotonMap,
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			// Traces a single photon through the scene until it can't trace it any longer
			// This is what the specific instances must extend
			inline void TraceSinglePhoton( 
				const Ray& ray,
				const RISEPel& power, 
				TranslucentPelPhotonMap& pPhotonMap, 
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const
			{
				TracePhoton( ray, power, false, pPhotonMap, ior_stack);
			}

			// Tells the tracer to set the photon map specifically for the scene
			inline void SetSpecificPhotonMapForScene( 
				TranslucentPelPhotonMap* pPhotonMap
				) const
			{
				pScene->SetTranslucentPelMap( pPhotonMap );
			}

		public:
			TranslucentPelPhotonTracer( 
				const unsigned int maxR, 
				const Scalar ext,
				const bool reflect, 
				const bool refract, 
				const bool direct_translucent,
				const bool dononmeshlights,			///< Should we trace non-mesh lights?
				const bool useiorstack,				///< [in] Should we use an ior stack ?
				const Scalar powerscale,
				const unsigned int temporal_samples,
				const bool regenerate
				);
		};
	}
}

#endif

