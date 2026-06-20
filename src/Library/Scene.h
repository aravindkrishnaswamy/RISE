//////////////////////////////////////////////////////////////////////
//
//  Scene.h - Defines a scene.  A scene is made up of a bunch of
//  objects and a bunch of materials.  
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCENE_
#define SCENE_

#include "Interfaces/IRadianceMap.h"
#include "Interfaces/IScenePriv.h"
#include "Interfaces/IAnimator.h"
#include "PhotonMapping/PendingPhotonShoots.h"
#include "Utilities/Reference.h"

namespace RISE
{
	class IMaterial;           // SceneSnapshot::GetObjectMaterial return type
	class ILight;              // SceneSnapshot::GetLight return type
	class IFilm;               // SceneSnapshot::GetFilm
	class IRadianceMap;        // SceneSnapshot::GetGlobalRadianceMap
	class IMedium;             // SceneSnapshot::GetGlobalMedium
	class ICamera;             // SceneSnapshot::GetClonedCamera

	namespace Implementation
	{
		class Object;          // cloned into SceneSnapshot
		class CameraCommon;    // active-camera pose capture
		struct CameraPoseSnapshot;

		//! feature/gui-snapshot-prototype: a genuinely immutable,
		//! RENDER-FAITHFUL snapshot of the scene's render-read state,
		//! taken while the live scene keeps being mutated in place by the
		//! interactive editor.
		//!
		//! DESIGN (vs the rejected "just addref the live instances"):
		//! CLONE every piece of state the editor mutates IN PLACE, ADDREF
		//! only the genuinely-immutable leaves.  Addref alone preserves
		//! lifetime, NOT state — the editor mutates the very instances it
		//! would have addref'd.  Per-state decisions (increment B):
		//!   - objects       CLONE  (Object::CloneSnapshot; transforms +
		//!                           material slots are editor-mutable)
		//!   - lights        CLONE  (SceneEditor::SetLightProperty edits
		//!                           color/energy/dir/cone in place)
		//!   - active camera CLONE  (full per-type params, not just pose —
		//!                           CameraIntrospection::SetProperty edits
		//!                           fstop/focal/... in place)
		//!   - film          CLONE  (IFilm::Resize mutates in place during
		//!                           interactive preview-scale)
		//!   - global medium CLONE  for HomogeneousMedium (MediaIntrospection
		//!                           SetAbsorption/Scattering/Emission edit
		//!                           in place); ADDREF for HeterogeneousMedium
		//!                           (baked, editor refuses to edit it)
		//!   - environment   ADDREF (global radiance map: no editor panel;
		//!                           only a pre-render CLI radiance_scale
		//!                           override touches it, outside snapshot life)
		//!
		//! SCOPE (increment B = capture/render-faithfulness only): this
		//! class CAPTURES the render-read state and exposes read accessors.
		//! It does NOT implement restore/publish (swap-back + TLAS /
		//! LightSampler rebuild) — that is the next increment.  It also does
		//! NOT deep-clone SSS-shader / interior-medium leaves on objects
		//! (a larger effort documented as the increment-A residual in
		//! SnapshotLeafClone.h); those stay addref-shared.
		class SceneSnapshot : public virtual Reference
		{
			// FREEZE: only the builder (Scene::CreateSnapshot) may populate
			// a snapshot.  The mutators below are PRIVATE so the snapshot is
			// immutable to every other caller post-construction — there is no
			// public surface to add/replace a cloned object or rebind the
			// camera pose after the builder hands the snapshot back.  This is
			// increment-A defect-fix #3 ("freeze the snapshot").
			friend class Scene;

		protected:
			std::vector<Object*>      clonedObjects;   //!< owned (each addref'd on clone)
			std::vector<String>       objectNames;     //!< parallel to clonedObjects

			std::vector<const ILight*> clonedLights;   //!< owned (each holds one ref)
			std::vector<String>        lightNames;     //!< parallel to clonedLights

			//! Render-faithful active-camera clone (full per-type params).
			//! Owned (one ref).  Null if no active camera, or if the active
			//! camera was ONB-constructed and so cannot be rebuilt through
			//! the non-ONB factory without silently degrading its basis —
			//! see CameraCommon::IsFromONB.  `cameraPose` is retained as a
			//! cheap pose summary AND as the honest fallback the read
			//! accessors fall back to when `clonedCamera` is null.
			ICamera*                  clonedCamera;
			CameraPoseSnapshot*       cameraPose;      //!< owned; null if no active camera
			String                    activeCameraName;

			IFilm*                    clonedFilm;       //!< owned (one ref); null if no film
			const IRadianceMap*       globalRadianceMap;//!< ADDREF-shared; null if none
			const IMedium*            globalMedium;     //!< CLONE for homogeneous / ADDREF for baked; null if none

			virtual ~SceneSnapshot();

			// Construction-only mutators (private; reachable solely via the
			// `friend class Scene` builder).  Populated by Scene::CreateSnapshot.
			void   AddClonedObject( Object* obj, const String& name );
			void   AddClonedLight( const ILight* light, const String& name );
			void   SetClonedCamera( ICamera* cam, const CameraPoseSnapshot& pose, const String& name );
			void   SetFilm( IFilm* film );
			void   SetGlobalRadianceMap( const IRadianceMap* rmap );
			void   SetGlobalMedium( const IMedium* medium );

		public:
			SceneSnapshot();

			// --- Objects ---
			size_t   GetObjectCount() const { return clonedObjects.size(); }
			Matrix4  GetObjectFinalTransform( size_t index ) const;
			String   GetObjectName( size_t index ) const;

			//! The snapshot's cloned object at `index` (const, read-only).
			//! Returns null for an out-of-range index.  Lets a caller
			//! inspect the cloned entity's polymorphic type (e.g. confirm a
			//! CSGObject clone is still a CSGObject, not a sliced Object)
			//! and its bindings without mutating the snapshot.
			const Object* GetClonedObject( size_t index ) const;

			//! The material bound to the snapshot's cloned object at `index`,
			//! or null if the index is out of range or that object has no
			//! material.  Used to prove the snapshot's material binding is
			//! INDEPENDENT of a later in-place rebind on the live material.
			const IMaterial* GetObjectMaterial( size_t index ) const;

			// --- Lights (CLONE — editor edits them in place) ---
			size_t           GetLightCount() const { return clonedLights.size(); }
			const ILight*    GetLight( size_t index ) const;
			String           GetLightName( size_t index ) const;

			// --- Active camera (CLONE — full params, render-faithful) ---
			bool     HasCamera() const { return cameraPose != 0; }
			Point3   GetCameraPosition() const;   //!< stored (rest) location
			Point3   GetCameraLookAt() const;
			//! The snapshot's cloned active camera (const, read-only) — a
			//! full per-type clone reproducing ALL parameters (thin-lens
			//! photographic params, fisheye scale, ortho viewport, ...),
			//! not merely the pose.  Null if there was no active camera, or
			//! if it was ONB-constructed (see `clonedCamera`).  Callers that
			//! only need pose can fall back to GetCameraPosition/LookAt.
			const ICamera*  GetClonedCamera() const { return clonedCamera; }

			// --- Film (CLONE — IFilm::Resize mutates in place) ---
			bool         HasFilm() const { return clonedFilm != 0; }
			const IFilm* GetFilm() const { return clonedFilm; }
			unsigned int GetFilmWidth() const;
			unsigned int GetFilmHeight() const;
			Scalar       GetFilmPixelAR() const;

			// --- Environment (ADDREF — no editor panel mutates it) ---
			bool                HasEnvironment() const { return globalRadianceMap != 0; }
			const IRadianceMap* GetGlobalRadianceMap() const { return globalRadianceMap; }

			// --- Global medium (CLONE homogeneous / ADDREF baked) ---
			const IMedium* GetGlobalMedium() const { return globalMedium; }
		};

		class Scene :
			public virtual IScenePriv,
			public virtual Reference
		{
		protected:
			// For now a scene only has a list of objects and some kind of camera
			const IObjectManager*		pObjectManager;
			const ILightManager*		pLightManager;
			const ILuminaryManager*		pLuminaryManager;
			ICameraManager*				pCameraManager;
			String						activeCameraName;
			// Cached pointer to the camera registered under
			// `activeCameraName` — kept in sync on every successful
			// Add/Set/Remove that mutates the active camera.  Reason
			// for the cache: GetCamera() is called per-pixel by every
			// rasterizer; resolving the name through the manager's
			// std::map<String,...>::find on every call would replace
			// a one-word pointer load with an O(log N) string-key
			// search.  Cache restores the pre-multi-camera cost.
			//
			// **Concurrency contract.**  Structural camera changes
			// (Add/Remove/SetActive/SetCameraManager) must serialize
			// against rendering — see IScenePriv.h.  The editor
			// enforces this via cancel-and-park; programmatic users
			// are responsible for their own serialization.  A render
			// pass that has begun is therefore guaranteed to see a
			// stable pActiveCamera until it returns.
			ICamera*					pActiveCamera;

			//! Active Film (pixel-grid descriptor — width / height /
			//! pixelAR).  Always non-null after Job::InitializeContainers
			//! installs the qHD default.  Replaced by SetFilm (called
			//! from the `film` chunk parser, the CLI overrides, or a
			//! programmatic IJob::SetFilm).
			IFilm*						pFilm;

			const IRadianceMap*			pGlobalRadianceMap;
			IPhotonMap*					pCausticMap;
			IPhotonMap*					pGlobalMap;
			IPhotonMap*					pTranslucentMap;
			ISpectralPhotonMap*			pCausticSpectralMap;
			ISpectralPhotonMap*			pGlobalSpectralMap;
			IShadowPhotonMap*			pShadowMap;
			IIrradianceCache*			pIrradianceCache;

			IAnimator*					pAnimator;

			const IMedium*				pGlobalMedium;

			// Deferred photon-map shoots: parser enqueues, first RasterizeScene flushes.
			PendingCausticPelShoot			mCausticPelPending;
			PendingGlobalPelShoot			mGlobalPelPending;
			PendingTranslucentPelShoot		mTranslucentPelPending;
			PendingCausticSpectralShoot		mCausticSpectralPending;
			PendingGlobalSpectralShoot		mGlobalSpectralPending;
			PendingShadowShoot				mShadowPending;

			// Gather parameters applied after the corresponding shoot completes.
			PendingPelGatherParams			mCausticPelGather;
			PendingPelGatherParams			mGlobalPelGather;
			PendingPelGatherParams			mTranslucentPelGather;
			PendingSpectralGatherParams		mCausticSpectralGather;
			PendingSpectralGatherParams		mGlobalSpectralGather;
			PendingPelGatherParams			mShadowGather;

			virtual ~Scene( );

		public:
			Scene( );

			const IObjectManager*		GetObjects( )	const	{ return pObjectManager; }
			const ILightManager*		GetLights( )	const	{ return pLightManager; }
			const ILuminaryManager*		GetLuminaries() const	{ return pLuminaryManager; }
			const ICamera*				GetCamera( )	const	{ return pActiveCamera; }
			const ICameraManager*		GetCameras( )	const	{ return pCameraManager; }
			String						GetActiveCameraName( ) const { return activeCameraName; }
			const IFilm*				GetFilm( )		const	{ return pFilm; }

			inline const IRadianceMap*			GetGlobalRadianceMap() const { return pGlobalRadianceMap; }
			inline const IPhotonMap*			GetCausticPelMap()	const	{ return pCausticMap; }
			inline const IPhotonMap*			GetGlobalPelMap()	const	{ return pGlobalMap; }
			inline const IPhotonMap*			GetTranslucentPelMap() const { return pTranslucentMap; }
			inline const ISpectralPhotonMap*	GetCausticSpectralMap() const{ return pCausticSpectralMap; }
			inline const ISpectralPhotonMap*	GetGlobalSpectralMap() const{ return pGlobalSpectralMap; }
			inline const IShadowPhotonMap*		GetShadowMap() const{ return pShadowMap; }
			inline const IIrradianceCache*		GetIrradianceCache() const  { return pIrradianceCache; }

			inline IAnimator*					GetAnimator() const { return pAnimator; }

			inline const IMedium*		GetGlobalMedium() const { return pGlobalMedium; }

			// Non-const accessors from IScenePriv
			ICamera*				GetCameraMutable()				{ return pActiveCamera; }
			ICameraManager*			GetCamerasMutable()				{ return pCameraManager; }
			IPhotonMap*				GetCausticPelMapMutable()		{ return pCausticMap; }
			IPhotonMap*				GetGlobalPelMapMutable()		{ return pGlobalMap; }
			IPhotonMap*				GetTranslucentPelMapMutable()	{ return pTranslucentMap; }
			ISpectralPhotonMap*		GetCausticSpectralMapMutable()	{ return pCausticSpectralMap; }
			ISpectralPhotonMap*		GetGlobalSpectralMapMutable()	{ return pGlobalSpectralMap; }
			IShadowPhotonMap*		GetShadowMapMutable()			{ return pShadowMap; }
			IIrradianceCache*		GetIrradianceCacheMutable()		{ return pIrradianceCache; }

			// Non-const handle to the global radiance map for the
			// `> modify rasterizer radiance_scale` override (see
			// IScenePriv).  `pGlobalRadianceMap` is stored `const` only
			// out of historical conservatism — the map is constructed
			// non-const (RISE_API_CreateRadianceMap) and is mutated here
			// solely before a render, in the single-threaded scene-setup
			// phase, so the const is over-broad storage typing rather
			// than a true immutability invariant.  Stripping it at this
			// one accessor is contained and avoids rippling a non-const
			// signature through IScene / IScenePriv::SetGlobalRadianceMap
			// / the out-of-tree 3DSMax caller.  All sibling scene objects
			// (photon maps, caches) are already exposed mutably this way.
			IRadianceMap*			GetGlobalRadianceMapMutable()	{ return const_cast<IRadianceMap*>( pGlobalRadianceMap ); }
			bool		AddCamera( const char* szName, ICamera* pCamera_ );
			bool		RemoveCamera( const char* szName );
			bool		SetActiveCamera( const char* szName );
			void		SetCameraManager( ICameraManager* pCameraManager_ );
			void		SetFilm( IFilm* pFilm_ );
			void		ResizeFilm( unsigned int width, unsigned int height, Scalar pixelAR );
			void		SetObjectManager( const IObjectManager* pObjectManager_ );

			//! SPIKE (feature/gui-snapshot-prototype): take a genuinely
			//! immutable snapshot of the scene's mutable wrapper state.
			//! Caller owns the returned SceneSnapshot (release() it).
			//! See SceneSnapshot above for the design rationale.
			SceneSnapshot* CreateSnapshot() const;

		private:
			// Walk the camera manager and call SetDimensionsAndPixelAR
			// on every CameraCommon-derived camera.  Shared between
			// SetFilm (pointer swap) and ResizeFilm (in-place mutation).
			void		ResyncCamerasToFilmDims( unsigned int width, unsigned int height, Scalar pixelAR );

		public:
			void		SetLightManager( const ILightManager* pLightManager_ );

			void		SetGlobalRadianceMap( const IRadianceMap* pRadianceMap );
			void		SetCausticPelMap( IPhotonMap* pPhotonMap );
			void		SetGlobalPelMap( IPhotonMap* pPhotonMap );
			void		SetTranslucentPelMap( IPhotonMap* pPhotonMap );
			void		SetCausticSpectralMap( ISpectralPhotonMap* pPhotonMap );
			void		SetGlobalSpectralMap( ISpectralPhotonMap* pPhotonMap );
			void		SetShadowMap( IShadowPhotonMap* pPhotonMap );
			void		SetIrradianceCache( IIrradianceCache* pCache );
			void		SetGlobalMedium( const IMedium* pMedium );

			void		SetSceneTime( const Scalar time ) const ;
			void		SetSceneTimeForPreview( const Scalar time ) const ;

			// Deferred photon-shoot queueing (called by Job during scene parse).
			void		QueueCausticPelPhotonShoot(		const PendingCausticPelShoot& req );
			void		QueueGlobalPelPhotonShoot(		const PendingGlobalPelShoot& req );
			void		QueueTranslucentPelPhotonShoot(	const PendingTranslucentPelShoot& req );
			void		QueueCausticSpectralPhotonShoot(const PendingCausticSpectralShoot& req );
			void		QueueGlobalSpectralPhotonShoot(	const PendingGlobalSpectralShoot& req );
			void		QueueShadowPhotonShoot(			const PendingShadowShoot& req );

			void		QueueCausticPelGatherParams(	Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons );
			void		QueueGlobalPelGatherParams(		Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons );
			void		QueueTranslucentPelGatherParams(Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons );
			void		QueueCausticSpectralGatherParams(Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons, Scalar nmRange );
			void		QueueGlobalSpectralGatherParams(Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons, Scalar nmRange );
			void		QueueShadowGatherParams(		Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons );

			bool		BuildPendingPhotonMaps( IProgressCallback* pProgress );

			// Diagnostic: process-wide count of photon-map shoot passes that did real
			// work (BuildPendingPhotonMaps invoked with >=1 pending shoot).  Proves the
			// deferred shoot is gated on the active rasterizer consuming photon maps:
			// a BDPT render leaves it 0; switching to a photon-mapping rasterizer makes
			// it >0.  Same static-counter pattern as DisplacedGeometry::GetBuildMeshCount.
			static unsigned int GetPhotonShootCount();
			static void         ResetPhotonShootCount();

			void		Shutdown();
		};
	}
}

#endif
