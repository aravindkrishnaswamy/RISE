//////////////////////////////////////////////////////////////////////
//
//  ILuminaryManager.h - Interface to the luminary manager.
//    The luminary manager is what manages all those physically
//    based lights (objects with have luminescent material)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ILUMINARY_MANAGER_
#define ILUMINARY_MANAGER_

#include "IBSDF.h"
#include "ISampling2D.h"
#include "IObject.h"
#include "IScene.h"
#include "IProbabilityDensityFunction.h"
#include "../Intersection/RayIntersection.h"
#include "IPhotonMap.h"
#include "../Utilities/RandomNumbers.h"
#include <vector>

namespace RISE
{
	class IRayCaster;

	class ILuminaryManager : public virtual IReference
	{
	protected:
		ILuminaryManager(){};
		virtual ~ILuminaryManager(){};

	public:
		//! Binds the luminary manager to a particular scene
		virtual void AttachScene(
			const IScene* pScene											///< [in] Scene to bind to
			) = 0;

		//! Adds the object to the list of luminaries
		virtual void AddToLuminaryList(
			const IObject& pObject											///< [in] Object to add
			) = 0;

		//! Sets up luminaire sampling
		virtual void SetLuminaireSampling( 
			ISampling2D* pLumSam											///< [in] Sampling kernel to use when the luminaire needs to be sampled
			) = 0;

		//! Computes direct lighting for all luminaires
		/// \return Direct lighting value as an RISEPel
		virtual RISEPel ComputeDirectLighting( 
			const RayIntersection& ri,										///< [in] Intersection information at point we computing lighting for
			const IBSDF& pBRDF,												///< [in] BRDF of the material
			const RandomNumberGenerator& random,							///< [in] Random number generator
			const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
			const IShadowPhotonMap* pShadowMap								///< [in] Shadow photon map for speeding up shadow checks
			) const = 0;

		//! Computes direct lighting for a single wavelength
		/// \return Direct lighting value for the particular wavelength as a scalar
		virtual	Scalar ComputeDirectLightingNM( 
			const RayIntersection& ri,										///< [in] Intersection information at point we computing lighting for
			const IBSDF& pBRDF,												///< [in] BRDF of the material
			const Scalar nm,												///< [in] Wavelength
			const RandomNumberGenerator& random,							///< [in] Random number generator
			const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
			const IShadowPhotonMap* pShadowMap								///< [in] Shadow photon map for speeding up shadow checks
			) const = 0;
	};
}

#include "IRayCaster.h"

#endif

