//////////////////////////////////////////////////////////////////////
//
//  CausticPelPhotonMap.h - Definition of the caustic pel photon map class which is what
//                          which uses the standard photon map class functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 23, 2002
//  Tabs: 4
//  Comments:  The code here is an implementation from Henrik Wann
//             Jensen's book Realistic Image Synthesis Using 
//             Photon Mapping.  Much of the code is influeced or
//             taken from the sample code in the back of his book.
//			   I have however used STD data structures rather than
//			   reinventing the wheel as he does.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAUSTIC_PEL_PHOTON_MAP_
#define CAUSTIC_PEL_PHOTON_MAP_

#include "PhotonMap.h"

namespace RISE
{
	namespace Implementation
	{
		class CausticPelPhotonMap : 
			public PhotonMapDirectionalPelHelper<Photon>
		{
		protected:

		public:
			CausticPelPhotonMap( 
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				);
			virtual ~CausticPelPhotonMap( );

			void RadianceEstimate(
				RISEPel& rad,
				const RayIntersectionGeometric& ri,
				const IBSDF& brdf,
				const IORStack* pIorStack = 0
				) const;

			void Serialize( 
				IWriteBuffer&			buffer					///< [in] Buffer to serialize to
				) const;

			bool DeserializeChecked(IReadBuffer& buffer);

			void Deserialize(
				IReadBuffer&			buffer					///< [in] Buffer to deserialize from
				);
		};
	}
}

#endif
