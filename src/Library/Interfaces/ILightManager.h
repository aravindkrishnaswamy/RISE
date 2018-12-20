//////////////////////////////////////////////////////////////////////
//
//  ILightManager.h - Interface to the light manager, which manages
//    all non-physical lights in a scene
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ILIGHTMANAGER_
#define ILIGHTMANAGER_

#include "IManager.h"
#include "../Intersection/RayIntersection.h"

namespace RISE
{
	class ILightPriv;
	class IRayCaster;

	class ILightManager : public virtual IManager<ILightPriv>
	{
	public:
		typedef std::vector<ILightPriv*> LightsList;

	protected:
		ILightManager(){};
		virtual ~ILightManager(){};

	public:
		//
		// Light specific
		//

		//! Computes direct lighting for all lights currently being managed
		virtual void ComputeDirectLighting( 
			const RayIntersectionGeometric& ri,					///< [in] Geometric intersection details at point to compute lighting information
			const IRayCaster& pCaster,							///< [in] The ray caster to use for occlusion testing
			const IBSDF& brdf,									///< [in] The BRDF of the object
			const bool bReceivesShadows,						///< [in] Should shadow checking be performed?
			RISEPel& amount										///< [out] Amount of lighting
			) const = 0;

		//! Returns the list of all the lights
		virtual const LightsList& getLights() const = 0;
	};
}

#include "IRayCaster.h"
#include "ILightPriv.h"

#endif
