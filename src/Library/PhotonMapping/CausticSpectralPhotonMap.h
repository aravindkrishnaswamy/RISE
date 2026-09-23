//////////////////////////////////////////////////////////////////////
//
//  CausticSpectralPhotonMap.h - Definition of the caustic spectral photon
//								 map class which is a standard caustic
//                               photon map but stores spectra
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 8, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAUSTIC_SPECTRAL_PHOTON_MAP_
#define CAUSTIC_SPECTRAL_PHOTON_MAP_

#include "PhotonMap.h"

namespace RISE
{
	namespace Implementation
	{
		class CausticSpectralPhotonMap : 
			public PhotonMapDirectionalSpectralHelper
		{
		protected:
			Scalar			nm_range;							// range of wavelengths to search for a NM irradiance estimate

		public:
			CausticSpectralPhotonMap( 
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				);

			virtual ~CausticSpectralPhotonMap( );

			bool Store( const Scalar power, const Scalar nm, const Point3& pos, const Vector3& dir );

			void SetGatherParamsNM( const Scalar radius, const Scalar ellipse_ratio, const unsigned int nminphotons, const unsigned int nmaxphotons, const Scalar nm_range_, IProgressCallback* pFunc )
			{
				PhotonMapCore<SpectralPhoton>::SetGatherParams( radius, ellipse_ratio, nminphotons, nmaxphotons, pFunc );
				nm_range = nm_range_;
			}

			void GetGatherParamsNM( Scalar& radius, Scalar& ellipse_ratio, unsigned int& nminphotons, unsigned int& nmaxphotons, Scalar& nm_range_ )
			{
				PhotonMapCore<SpectralPhoton>::GetGatherParams( radius, ellipse_ratio, nminphotons, nmaxphotons );
				nm_range_ = nm_range;
			}

			void RadianceEstimate(
				RISEPel& rad,
				const RayIntersectionGeometric& ri,
				const IBSDF& brdf,
				const IORStack* pIorStack = 0
				) const;

			void RadianceEstimateNM(
				const Scalar nm,
				Scalar& rad,
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
