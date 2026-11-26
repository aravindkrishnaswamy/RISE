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

		// ----- Methods appended below this line (vtable-stable) -----
		// New methods MUST be appended here so that any in-tree
		// consumer compiled against an older version of this header
		// keeps the same vtable slot indices for the methods it
		// knows about.  Mid-insertion shifts every subsequent slot
		// and miscompiles stale .obj files.

		//! Replaces the active Film with the supplied one.  The Job
		//! installs a default qHD film during InitializeContainers; a
		//! `film` chunk in the scene file or a CLI override calls
		//! SetFilm to swap it.  Passing null is a no-op (active film
		//! is preserved); a scene without a film cannot render, so
		//! there is no "clear the film" path.  Same concurrency
		//! contract as the camera mutators above — must not run
		//! concurrently with rendering.
		virtual void SetFilm(
			IFilm* pFilm_										///< [in] Film to install (replaces any prior film); null is a no-op
			) = 0;

		//! Mutate the existing active Film's dims in place (no
		//! allocation) and re-sync every camera's Frame to match.
		//! Hot-path alternative to SetFilm for the SceneEditController
		//! preview-scale pump — avoids the per-frame IFilm allocation
		//! that SetFilm would otherwise incur when toggling between
		//! rest and scaled dims at every interactive frame.  Active
		//! Film must already exist (the qHD default installed by
		//! Job::InitializeContainers ensures it).
		//!
		//! **Concurrency contract.**  Same as SetFilm and the camera
		//! mutators above — must NOT run concurrently with rendering.
		//! `Implementation::Film::Resize` writes width / height /
		//! pixelAR as plain non-atomic stores, so a concurrent reader
		//! on another thread would see a torn (width-new, height-old)
		//! triple.  Callers serialize externally: the editor's render
		//! thread is the sole driver of preview-scale Resize calls,
		//! and `MainWindow::onRender()` plus
		//! `SceneEditController::RequestProductionRender` both Stop()
		//! the editor before triggering a production render so the
		//! editor's render thread is joined before the production
		//! rasterizer reads Film.
		virtual void ResizeFilm(
			unsigned int width,									///< [in] New width in pixels
			unsigned int height,								///< [in] New height in pixels
			Scalar       pixelAR								///< [in] New pixel aspect ratio
			) = 0;

		//! Shutsdown the scene, forces the deletion and clearing of everything
		virtual void Shutdown() = 0;

		//! \return Non-const handle to the global radiance (environment)
		//! map, or NULL if none is set.  Mirrors the GetGlobalRadianceMap()
		//! const accessor and the GetGlobalPelMapMutable() family above.
		//! Backs Job::SetActiveRasterizerRadianceScale, which pushes a
		//! `> modify rasterizer radiance_scale` override into the map via
		//! IRadianceMap::SetScale so the background/miss radiance stays
		//! consistent with the environment importance sampler.  Mutated
		//! only before a render (single-threaded), preserving the
		//! render-time scene-immutability contract.  Appended at the END
		//! of the interface (vtable-stable).
		virtual IRadianceMap* GetGlobalRadianceMapMutable() = 0;
	};
}

#endif
