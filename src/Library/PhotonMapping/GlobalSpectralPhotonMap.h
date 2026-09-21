//////////////////////////////////////////////////////////////////////
//
//  GlobalSpectralPhotonMap.h - Definition of the global spectral photon
//    map class 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 6, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GLOBAL_SPECTRAL_PHOTON_MAP_
#define GLOBAL_SPECTRAL_PHOTON_MAP_

#include "PhotonMap.h"

namespace RISE
{
	namespace Implementation
	{
		class GlobalSpectralPhotonMap : 
			public ISpectralPhotonMap,
			public PhotonMapDirectionalHelper<SpectralPhoton>
		{
		protected:
			Scalar			nm_range;							// range of wavelengths to search for a NM irradiance estimate

		public:
			GlobalSpectralPhotonMap( 
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				);

			virtual ~GlobalSpectralPhotonMap( );

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
				RISEPel&						rad,					// returned radiance
				const RayIntersectionGeometric&	ri,						// ray-surface intersection information
				const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
				) const;

			void RadianceEstimateNM( 
				const Scalar					nm,						// wavelength for the estimate
				Scalar&							rad,					// returned irradiance for the particular wavelength
				const RayIntersectionGeometric&	ri,						// ray-surface intersection information
				const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
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
