//////////////////////////////////////////////////////////////////////
//
//  IEmitter.h - Defines an interface to an emitter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IEMITTER_
#define IEMITTER_

#include "IReference.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/Color/Color.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	//! This is the interface for luminaries
	/// \sa IMaterial
	class IEmitter : public virtual IReference
	{
	protected:
		IEmitter(){};
		virtual ~IEmitter(){};

	public:
		//! Flux per solid angle per unit projected area (Watt/m^2sr)
		//! So this function would, given the outgoing vector out and the normal N
		/// \return The emitted radiance for each color component as an IFXPel
		virtual RISEPel emittedRadiance(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection information
			const Vector3& out,											///< [in] Outgoing vector from the surface of the luminary
			const Vector3& N											///< [in] Normal of the luminary
			) const = 0;

		//! Returns the radiance for a particular wavelength
		/// \return The emitted radiance for the parituclar wavelength as a scalar
		virtual Scalar emittedRadianceNM( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection information
			const Vector3& out,											///< [in] Outgoing vector from the surface of the luminary
			const Vector3& N,											///< [in] Normal of the luminary
			const Scalar nm												///< [in] Wavelength to process
			) const = 0;

		//! Average amount of radiative flux leaving any point on the surface into all
		//! directions above the surface.  Flux is the radiant energy flowing through a surface per unit time
		//! ie Watt = Joules / sec )
		/// \return The flux for each component as an IFXPel
		virtual RISEPel averageRadiantExitance(
			) const	= 0;

		//! Same as above except returns the radiant exitance for a particular wavelength
		/// \return The flux for the wavelength as a scalar
		virtual Scalar averageRadiantExitanceNM(
			const Scalar nm												///< [in] Wavelength to process
			) const = 0;

		//! Returns a random emmited photon direction for this material, assuming
		//! the material has emmisive properties
		/// \return Vector which represents the direction of photon emmision
		virtual Vector3 getEmmittedPhotonDir( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection information
			const Point2& random										///< [in] Two random variables which determine the perturbation of the photon emmision vector
			) const	= 0;
	};
}

#endif
