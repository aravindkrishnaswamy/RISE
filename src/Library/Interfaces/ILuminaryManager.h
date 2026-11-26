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
#include "../Utilities/ISampler.h"
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
	};
}

#include "IRayCaster.h"

#endif

