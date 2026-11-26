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

			// Retained legacy overload: directionless deposits are unsupported.
			// Reports a diagnostic and returns false without modifying this map.
			bool Store( 
				const RISEPel& power, 
				const Point3& pos 
				);

			// wi is a unit direction away from the receiving surface toward
			// the previous photon vertex. diffuseExit identifies a packet
			// already weighted by the translucent exit lobe's Beer*(1-s).
			bool Store( const RISEPel& power, const Point3& pos,
				const Vector3& wi, const bool diffuseExit );
			bool DeserializeChecked( IReadBuffer& buffer );

			void RadianceEstimate(
				RISEPel& rad,
				const RayIntersectionGeometric& ri,
				const IBSDF& brdf,
				const IORStack* pIorStack = 0
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
