//////////////////////////////////////////////////////////////////////
//
//  TranslucentPelPhotonMap.h - Definition of the translucent pel 
//    photon map class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 19, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRANSLUCENT_PEL_PHOTON_MAP_
#define TRANSLUCENT_PEL_PHOTON_MAP_

#include "PhotonMap.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentPelPhotonMap : 
			public Implementation::PhotonMapCore<TranslucentPhoton>
		{
		public:
			TranslucentPelPhotonMap(
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				);
			virtual ~TranslucentPelPhotonMap( );

			// Stores the given photon with no direction
			bool Store( 
				const RISEPel& power, 
				const Point3& pos 
				);

			void RadianceEstimate( 
				RISEPel&						rad,					// returned radiance
				const RayIntersectionGeometric&	ri,						// ray-surface intersection information
				const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
				) const;

			void Serialize( 
				IWriteBuffer&			buffer					///< [in] Buffer to serialize to
				) const;

			void Deserialize(
				IReadBuffer&			buffer					///< [in] Buffer to deserialize from
				);

			// scale = 1/number of emmitted photons
			void ScalePhotonPower( const Scalar scale );
		};
	}
}

#endif
