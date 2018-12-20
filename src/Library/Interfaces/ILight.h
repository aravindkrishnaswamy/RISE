//////////////////////////////////////////////////////////////////////
//
//  ILight.h - Interface of the Light class.  This class represents
//  lights that are specific to computer graphics (ie. hack lights)
//  Some examples of these kinds of lights are infinite point lights, 
//  infinite point, spot lights, and directional lights.  
//
//  Note that Ambient light is also a subclass
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 23, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ILIGHT_
#define ILIGHT_

#include "IReference.h"
#include "IKeyframable.h"
#include "IBSDF.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	class IRayCaster;

	//! This is a 'hacky' light that are not physically based, such as point lights
	//! spot lights or directional lights
	class ILight :
		public virtual IReference,
		public virtual IKeyframable
	{
	protected:
		virtual ~ILight(){};
		ILight(){};

	public:

		//! Asks if the light can generate photons for the purpose of photon mapping
		virtual bool CanGeneratePhotons() const = 0;

		//! Asks the light for its radiant exitance
		virtual RISEPel radiantExitance() const = 0;

		//! Asks the light for its emitted radiance in a particular direction
		virtual RISEPel emittedRadiance( const Vector3& vLightOut ) const = 0;

		//! Asks the light to generate a random emitted photon
		virtual Ray generateRandomPhoton( const Point3& ptrand ) const = 0;

		//! Computes direct lighting
		virtual void ComputeDirectLighting( 
			const RayIntersectionGeometric& ri,				///< [in] Geometric intersection details at point to compute lighting information
			const IRayCaster& pCaster,						///< [in] The ray caster to use for occlusion testing
			const IBSDF& brdf,								///< [in] BRDF of the object 
			const bool bReceivesShadows,					///< [in] Should shadow checking be performed?
			RISEPel& amount									///< [out] Amount of lighting
			) const = 0;
	};
}

#include "IRayCaster.h"

#endif
