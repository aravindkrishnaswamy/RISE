//////////////////////////////////////////////////////////////////////
//
//  ShadowPhotonMap.h - Definition of the shadow photon map class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHADOW_PHOTON_MAP_
#define SHADOW_PHOTON_MAP_

#include "PhotonMap.h"

namespace RISE
{
	namespace Implementation
	{
		class ShadowPhotonMap : 
			public IShadowPhotonMap,
			public PhotonMapCore<ShadowPhoton>
		{
		public:
			ShadowPhotonMap( 
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				);
			virtual ~ShadowPhotonMap( );

			// Stores the given photon with no direction
			bool Store( 
				const Point3& pos,
				const bool shadow
				);

			void LocateShadowPhotons( 
				const Point3&			loc,								// the location from which to search for photons
				const Scalar			maxDist,							// the maximum radius to look for photons
				const int				from,								// index to search from
				const int				to,									// index to search to
				bool&					lit_found,							// has a fully lit photon been found ?
				bool&					shadow_found						// has a fully shadowed photon been found ?
			) const;

			void RadianceEstimate( 
				RISEPel&						rad,					// returned radiance
				const RayIntersectionGeometric&	ri,						// ray-surface intersection information
				const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
				) const;

			//! Estimates the shadowing amount
			void ShadowEstimate( 
				char&					shadow,					///< [in] 0 = totally lit, 1 = totally shadowed, 2 = pieces of both
				const Point3&			pos						///< [in] Surface position
				) const;

			void Serialize( 
				IWriteBuffer&			buffer					///< [in] Buffer to serialize to
				) const;

			void Deserialize(
				IReadBuffer&			buffer					///< [in] Buffer to deserialize from
				);
		};
	}
}

#endif
