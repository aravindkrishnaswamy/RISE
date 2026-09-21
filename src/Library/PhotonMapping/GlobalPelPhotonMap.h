//////////////////////////////////////////////////////////////////////
//
//  GlobalPelPhotonMap.h - Definition of the global pel photon map class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 14, 2003
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

#ifndef GLOBAL_PEL_PHOTON_MAP_
#define GLOBAL_PEL_PHOTON_MAP_

#include "PhotonMap.h"

namespace RISE
{
	namespace Implementation
	{
		class GlobalPelPhotonMap : 
			public PhotonMapDirectionalPelHelper<IrradPhoton>
		{		
		protected:
			bool	bPrecomputedIrrad;							// Has irradiance been precomputed ?

		public:
			GlobalPelPhotonMap( 
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				);
			virtual ~GlobalPelPhotonMap( );	

			void PrecomputeIrradiance( 
				const unsigned int apart,						// How far apart should precomputed irradiances be?
				IProgressCallback* pFunc						// Progress callback
				);

			void RadianceEstimate( 
				RISEPel&						rad,					// returned radiance
				const RayIntersectionGeometric&	ri,						// ray-surface intersection information
				const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
				) const;

			bool Store( const RISEPel& power, const Point3& pos, const Vector3& N, const Vector3& dir );

			void SetGatherParams( const Scalar radius, const Scalar ellipse_ratio, const unsigned int nminphotons, const unsigned int nmaxphotons, IProgressCallback* pFunc )
			{
				PhotonMapCore<IrradPhoton>::SetGatherParams( radius, ellipse_ratio, nminphotons, nmaxphotons, pFunc );
				if( !bPrecomputedIrrad ) {
					PrecomputeIrradiance( 4, pFunc );
				}
			}

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
