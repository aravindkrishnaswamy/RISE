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
#include "../PhotonMapping/PendingPhotonShoots.h"

namespace RISE
{
	class IProgressCallback;

	class IScenePriv : public virtual IScene
	{
	protected:
		IScenePriv(){};
		virtual ~IScenePriv(){};

	public:
		//
		// Non-const accessors for privileged mutation
		//

		/// \return Non-const camera for setup
		virtual ICamera*				GetCameraMutable() = 0;

		/// \return Non-const caustic PEL photon map
		virtual IPhotonMap*				GetCausticPelMapMutable() = 0;

		/// \return Non-const global PEL photon map
		virtual IPhotonMap*				GetGlobalPelMapMutable() = 0;

		/// \return Non-const translucent PEL photon map
		virtual IPhotonMap*				GetTranslucentPelMapMutable() = 0;

		/// \return Non-const caustic spectral photon map
		virtual ISpectralPhotonMap*		GetCausticSpectralMapMutable() = 0;

		/// \return Non-const global spectral photon map
		virtual ISpectralPhotonMap*		GetGlobalSpectralMapMutable() = 0;

		/// \return Non-const shadow photon map
		virtual IShadowPhotonMap*		GetShadowMapMutable() = 0;

		/// \return Non-const irradiance cache
		virtual IIrradianceCache*		GetIrradianceCacheMutable() = 0;

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

		//! Sets the global participating medium
		virtual void SetGlobalMedium(
			const IMedium* pMedium								///< [in] Global medium to set
			) = 0;

		//
		// Deferred photon-map shoots.  The parser / Job API queues requests here
		// during scene setup; the rasterizer invokes BuildPendingPhotonMaps once
		// at the start of RasterizeScene (after PrepareForRendering, before any
		// worker thread is spawned) to actually trace the photons.
		//

		virtual void QueueCausticPelPhotonShoot(	const PendingCausticPelShoot& req )		= 0;
		virtual void QueueGlobalPelPhotonShoot(		const PendingGlobalPelShoot& req )		= 0;
		virtual void QueueTranslucentPelPhotonShoot(const PendingTranslucentPelShoot& req )	= 0;
		virtual void QueueCausticSpectralPhotonShoot(	const PendingCausticSpectralShoot& req )	= 0;
		virtual void QueueGlobalSpectralPhotonShoot(	const PendingGlobalSpectralShoot& req )		= 0;
		virtual void QueueShadowPhotonShoot(		const PendingShadowShoot& req )			= 0;

		virtual void QueueCausticPelGatherParams(	Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons ) = 0;
		virtual void QueueGlobalPelGatherParams(	Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons ) = 0;
		virtual void QueueTranslucentPelGatherParams(Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons ) = 0;
		virtual void QueueCausticSpectralGatherParams(	Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons, Scalar nmRange ) = 0;
		virtual void QueueGlobalSpectralGatherParams(	Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons, Scalar nmRange ) = 0;
		virtual void QueueShadowGatherParams(		Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons ) = 0;

		//! Builds any queued photon maps.  Safe to call when nothing is queued
		//! (returns true, no-op).  Single-threaded; called once per RasterizeScene.
		virtual bool BuildPendingPhotonMaps( IProgressCallback* pProgress ) = 0;

		//! Shutsdown the scene, forces the deletion and clearing of everything
		virtual void Shutdown() = 0;
	};
}

#endif
