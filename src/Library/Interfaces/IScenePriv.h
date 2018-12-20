//////////////////////////////////////////////////////////////////////
//
//  IScenePriv.h - Priviledged interface to a scene, 
//    allowed to change stuff
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISCENEPRIV_
#define ISCENEPRIV_

#include "IScene.h"

namespace RISE
{
	class IScenePriv : public virtual IScene
	{
	protected:
		IScenePriv(){};
		virtual ~IScenePriv(){};

	public:
		//
		// Added interface functions
		//

		//! Sets the current camera
		virtual void SetCamera(
			ICamera* pCamera_									///< [in] Camera to set
			) = 0;

		//! Sets the object manager, which contains all the objects
		virtual void SetObjectManager( 
			const IObjectManager* pObjectManager_				///< [in] Object manager to set
			) = 0;

		//! Sets the light manager, which contains all the lights
		virtual void SetLightManager( 
			const ILightManager* pLightManager_					///< [in] Light manager to set
			) = 0;

		//! Sets the global radiance (environment) map
		virtual void SetGlobalRadianceMap( 
			const IRadianceMap* pRadianceMap					///< [in] Global radiance map to set
			) = 0;

		//! Sets the caustic PEL photon map
		virtual void SetCausticPelMap( 
			IPhotonMap* pPhotonMap								///< [in] Photon map to set
			) = 0;

		//! Sets the global PEL photon map
		virtual void SetGlobalPelMap( 
			IPhotonMap* pPhotonMap								///< [in] Photon map to set
			) = 0;

		//! Sets the translucent PEL photon map
		virtual void SetTranslucentPelMap( 
			IPhotonMap* pPhotonMap								///< [in] Photon map to set
			) = 0;

		//! Sets the caustic spectral photon map
		virtual void SetCausticSpectralMap( 
			ISpectralPhotonMap* pPhotonMap						///< [in] Photon map to set
			) = 0;

		//! Sets the global spectral photon map
		virtual void SetGlobalSpectralMap( 
			ISpectralPhotonMap* pPhotonMap						///< [in] Photon map to set
			) = 0;

		//! Sets the shadow photon map
		virtual void SetShadowMap( 
			IShadowPhotonMap* pPhotonMap						///< [in] Photon map to set
			) = 0;

		//! Sets an irradiance cache
		virtual void SetIrradianceCache( 
			IIrradianceCache* pCache							///< [in] Cache to set
			) = 0;

		//! Sets the global atmopsherics processor
		virtual void SetGlobalAtmosphere( 
			const IAtmosphere* pAtmosphere						///< [in] Global atmopshere to set
			) = 0;

		//! Shutsdown the scene, forces the deletion and clearing of everything
		virtual void Shutdown() = 0;
	};
}

#endif
