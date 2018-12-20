//////////////////////////////////////////////////////////////////////
//
//  IPhotonMap.h - Interface to a photon map
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPHOTON_MAP_
#define IPHOTON_MAP_

#include "IReference.h"
#include "ISerializable.h"
#include "IBSDF.h"
#include "IProgressCallback.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/BoundingBox.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	//! The photon map stores a bunch of photons, this is a generic interface
	//! to various kinds of photon maps
	class IPhotonMap : public virtual IReference, public ISerializable
	{
	protected:
		IPhotonMap(){};
		virtual ~IPhotonMap(){};

	public:
		//! Sets gather parameters
		virtual void SetGatherParams( 
			const Scalar radius,							///< [in] Gather radius
			const Scalar ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int nminphotons,					///< [in] Minimum number of photons to count as proper gather
			const unsigned int nmaxphotons,					///< [in] Maximum number of photons that should be gathered
			IProgressCallback* pFunc						///< [in] Callback functor for reporting progress
			) = 0;

		//! Gets the gather parameters
		virtual void GetGatherParams( 
			Scalar& radius,									///< [out] Gather radius
			Scalar& ellipse_ratio,							///< [out] Ratio of the gather ellipse (percentage of gather radius)
			unsigned int& nminphotons,						///< [out] Minimum number of photons to count as proper gather
			unsigned int& nmaxphotons						///< [out] Maximum number of photons that should be gathered
			) = 0;
		
		/// \return Number of stored photons
		virtual unsigned int NumStored() = 0;

		/// \return Maximum number of photons that can be stored
		virtual unsigned int MaxPhotons() = 0;

		//! Balances the KD-Tree in the photon map
		virtual void Balance() = 0;

		//! Returns the bounding box of this photon map
		virtual BoundingBox GetBoundingBox() = 0;

		//! Estimates the radiance at the given point
		virtual void RadianceEstimate( 
			RISEPel&						rad,						///< [out] Radiance at the surface
			const RayIntersectionGeometric&	ri,							///< [in] Ray-surface intersection information
			const IBSDF&					brdf						///< [in] BRDF of the surface to estimate irradiance from
			) const = 0;

		//! Counts the number of photons at the particular location
		virtual void CountPhotonsAt(
			const Point3&			loc,								// the location from which to search for photons
			const Scalar			maxDist,							// the maximum radius to look for photons
			const unsigned int		max,								// maximum number of photons to look for
			unsigned int&			cnt									// count so far
			) const = 0;
		
		//! Tells the photon map to re-generate itself 
		/// \return TRUE if the photonmap is regenerated, FALSE otherwise
		virtual bool Regenerate( const Scalar time ) const = 0;

		//! Tells the photon map to shutdown, deleting all references and prepare for deletion
		virtual void Shutdown() = 0;
	};

	//! The spectral photon map stores a bunch of spectral photons and extends the generic IPhoton
	//! interface to provide a function for doing wavelength dependent estimates
	class ISpectralPhotonMap : public virtual IPhotonMap
	{
	protected:
		ISpectralPhotonMap(){};
		virtual ~ISpectralPhotonMap(){};

	public:
		//! Sets gather parameters
		virtual void SetGatherParamsNM( 
			const Scalar radius,									///< [in] Gather radius
			const Scalar ellipse_ratio,								///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int nminphotons,							///< [in] Minimum number of photons to count as proper gather
			const unsigned int nmaxphotons,							///< [in] Maximum number of photons that should be gathered
			const Scalar nm_range,									///< [in] Range of wavelengths to search for a NM irradiance estimate
			IProgressCallback* pFunc								///< [in] Callback functor for reporting progress
			) = 0;

		//! Gets the gather parameters
		virtual void GetGatherParamsNM( 
			Scalar& radius,											///< [out] Gather radius
			Scalar& ellipse_ratio,									///< [out] Ratio of the gather ellipse (percentage of gather radius)
			unsigned int& nminphotons,								///< [out] Minimum number of photons to count as proper gather
			unsigned int& nmaxphotons,								///< [out] Maximum number of photons that should be gathered
			Scalar& nm_range										///< [out] Range of wavelengths to search for a NM irradiance estimate
			) = 0;

		//! Estimates the irradiance at the given point
		virtual void RadianceEstimateNM( 
			const Scalar					nm,						///< [in] Wavelength for the estimate
			Scalar&							rad,					///< [out] Returned radiance for the particular wavelength
			const RayIntersectionGeometric&	ri,						///< [in] Ray-surface intersection information
			const IBSDF&					brdf					///< [in] BRDF of the surface to estimate irradiance from
			) const = 0;		

	};

	//! The shadow photon map stores a bunch of shadow and lit photons for fast
	//! shadows
	class IShadowPhotonMap : public virtual IPhotonMap
	{
	protected:
		IShadowPhotonMap(){};
		virtual ~IShadowPhotonMap(){};

	public:
		//! Estimates the shadowing amount
		virtual void ShadowEstimate( 
			char&					shadow,					///< [in] 0 = totally lit, 1 = totally shadowed, 2 = pieces of both
			const Point3&			pos						///< [in] Surface position
			) const = 0;		

	};
}

#endif
