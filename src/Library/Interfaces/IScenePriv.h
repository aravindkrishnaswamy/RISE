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

		/// \return Non-const handle to the currently active camera.
		virtual ICamera*				GetCameraMutable() = 0;

		/// \return Non-const handle to the camera manager.
		virtual ICameraManager*			GetCamerasMutable() = 0;

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

		//
		// Camera-set mutators.
		//
		// **Concurrency contract.**  These three methods (AddCamera,
		// RemoveCamera, SetActiveCamera) plus SetCameraManager are
		// structural mutations.  They MUST NOT run concurrently with
		// rendering — the render thread reads the active camera per
		// pixel, and a concurrent swap would invite torn reads or
		// use-after-free on the old camera.  The interactive editor
		// enforces this via SceneEditController's cancel-and-park
		// pattern (see SceneEditController::SetProperty for the
		// "active_camera" intercept and OnTimeScrub for the canonical
		// implementation).  Programmatic callers outside the editor
		// must serialize externally — there is no in-library lock.
		//

		//! Adds a camera to the scene's camera manager and marks it
		//! active.  Every successful AddCamera makes the new camera
		//! the active one — "last-added wins" matches the design.
		//! Returns false on null camera, null/empty name, or
		//! duplicate-name collision.
		virtual bool AddCamera(
			const char* szName,									///< [in] Name to register the camera under
			ICamera* pCamera_									///< [in] Camera to add
			) = 0;

		//! Removes the named camera from the manager.  If the removed
		//! camera was active, auto-promotes the lexicographically
		//! FIRST remaining camera (the order GenericManager iterates
		//! its std::map<String,...> in) and falls back to "no active
		//! camera" if the manager is empty.  Lex-first is a
		//! deliberate compromise: tracking insertion order would
		//! require a parallel vector and the design only requires the
		//! promotion to be deterministic.
		//! Returns false if the name isn't found.
		virtual bool RemoveCamera(
			const char* szName									///< [in] Name of camera to remove
			) = 0;

		//! Designates the named camera as active.  Returns false
		//! (leaving the active camera unchanged) if the name isn't
		//! registered with the manager.
		virtual bool SetActiveCamera(
			const char* szName									///< [in] Name of camera to make active
			) = 0;

		//! Sets the camera manager.  Used by Job during scene
		//! construction to wire the manager into the scene.  Caller
		//! retains ownership semantics through addref/release.
		virtual void SetCameraManager(
			ICameraManager* pCameraManager_						///< [in] Camera manager to install
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
