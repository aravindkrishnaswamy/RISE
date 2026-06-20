//////////////////////////////////////////////////////////////////////
//
//  Scene.cpp - Implementation of the scene class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 16, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Scene.h"
#include "Animation/Animator.h"
#include "RISE_API.h"
#include "Interfaces/ICameraManager.h"
#include "Interfaces/IEnumCallback.h"
#include "Interfaces/IPhotonTracer.h"
#include "Interfaces/IPhotonMap.h"
#include "Interfaces/IProgressCallback.h"
#include "Cameras/CameraCommon.h"		// for SetFilm camera-resync dynamic_cast
#include "Objects/Object.h"				// Object::CloneSnapshot
#include "Interfaces/IObjectManager.h"	// EnumerateObjects
#include "Interfaces/ILightManager.h"	// snapshot light capture
#include "Interfaces/ILight.h"			// snapshot light accessors
#include "Interfaces/IFilm.h"			// snapshot film capture
#include "Interfaces/IMedium.h"			// snapshot medium capture
#include "Interfaces/IRadianceMap.h"	// snapshot environment capture
#include "Objects/SnapshotLeafClone.h"	// CloneLight/Medium/CameraForSnapshot
#include <cstdio>						// snprintf for synthesized restore names

using namespace RISE;
using namespace RISE::Implementation;

Scene::Scene( ) :
  pObjectManager( 0 ),
  pLightManager( 0 ),
  pLuminaryManager( 0 ),
  pCameraManager( 0 ),
  activeCameraName(),
  pActiveCamera( 0 ),
  pFilm( 0 ),
  pGlobalRadianceMap( 0 ),
  pCausticMap( 0 ),
  pGlobalMap( 0 ),
  pTranslucentMap( 0 ),
  pCausticSpectralMap( 0 ),
  pGlobalSpectralMap( 0 ),
  pShadowMap( 0 ),
  pIrradianceCache( 0 ),
  pAnimator( 0 ),
  pGlobalMedium( 0 ),
  mCausticPelPending(),
  mGlobalPelPending(),
  mTranslucentPelPending(),
  mCausticSpectralPending(),
  mGlobalSpectralPending(),
  mShadowPending(),
  mCausticPelGather(),
  mGlobalPelGather(),
  mTranslucentPelGather(),
  mCausticSpectralGather(),
  mGlobalSpectralGather(),
  mShadowGather()
{
	// POD structs: zero-initialize all `pending` / `set` flags.
	mCausticPelPending.pending = false;
	mGlobalPelPending.pending = false;
	mTranslucentPelPending.pending = false;
	mCausticSpectralPending.pending = false;
	mGlobalSpectralPending.pending = false;
	mShadowPending.pending = false;
	mCausticPelGather.set = false;
	mGlobalPelGather.set = false;
	mTranslucentPelGather.set = false;
	mCausticSpectralGather.set = false;
	mGlobalSpectralGather.set = false;
	mShadowGather.set = false;

	pAnimator = new Animator();
	GlobalLog()->PrintNew( pAnimator, __FILE__, __LINE__, "animator" );
}

Scene::~Scene( )
{
	Shutdown();
}

void Scene::Shutdown()
{
	// Clear the publish pointer + drop Scene's own retain before
	// shutting down the manager.  The manager's Shutdown releases
	// its own per-camera refs; ours is independent.  Order doesn't
	// affect correctness — the contract is that no render is in
	// flight when Shutdown runs (callers serialize externally).
	ICamera* prevActive = pActiveCamera;
	pActiveCamera = 0;
	safe_release( prevActive );
	safe_shutdown_and_release( pCameraManager );
	activeCameraName = String();
	// pFilm released AFTER pCameraManager intentionally — Phase B
	// will plumb cameras to hold a strong ref on the active film,
	// so cameras must drop their refs (via the manager's Shutdown)
	// before Scene's own ref is released.  Phase B reviewers: if
	// you change cameras to cache a raw IFilm* without addref,
	// move this release UP above the camera teardown.
	safe_release( pFilm );
	safe_release( pObjectManager );
	safe_release( pLightManager );
	safe_release( pLuminaryManager );
	safe_release( pGlobalRadianceMap );
	safe_shutdown_and_release( pCausticMap );
	safe_shutdown_and_release( pGlobalMap );
	safe_shutdown_and_release( pTranslucentMap );
	safe_shutdown_and_release( pCausticSpectralMap );
	safe_shutdown_and_release( pGlobalSpectralMap );
	safe_shutdown_and_release( pShadowMap );
	safe_release( pIrradianceCache );
	safe_release( pGlobalMedium );
	safe_release( pAnimator );
}

void Scene::SetCameraManager( ICameraManager* pCameraManager_ )
{
	if( pCameraManager_ )
	{
		// Reset the active-camera state along with the manager swap.
		// Without this, a Job::InitializeContainers re-init (called
		// on scene reset) would leave the OLD activeCameraName
		// pointing at a name that the new (empty) manager doesn't
		// have, and pActiveCamera dangling at a freed object.
		ICamera* prevActive = pActiveCamera;
		pActiveCamera = 0;
		activeCameraName = String();
		safe_release( prevActive );
		safe_release( pCameraManager );
		pCameraManager_->addref();
		pCameraManager = pCameraManager_;
	}
}

// Camera-resync helper used by both SetFilm (pointer swap) and
// ResizeFilm (in-place dim mutation).  Without this, a CLI override
// (`--width 480 --height 270`) or a `film` chunk authored after the
// camera chunk, or the SceneEditController preview-scale pump, would
// change Film while the cameras' projection matrices still bake the
// old dims — rasterizer enumerates Film-many pixels but GenerateRay
// projects through the old dims, producing a re-FRAMED image instead
// of a re-RESOLVED one.  Walk the manager and update every camera so
// SetActiveCamera switches are also safe.  Cameras not derived from
// CameraCommon (none in tree today, but possible) are silently
// skipped.
void Scene::ResyncCamerasToFilmDims(
	unsigned int width,
	unsigned int height,
	Scalar       pixelAR
	)
{
	if( !pCameraManager ) {
		return;
	}
	struct ResyncCb : public IEnumCallback<const char*> {
		ICameraManager*    mgr;
		unsigned int       w, h;
		Scalar             pAR;
		bool operator()( const char* const& name ) override {
			ICamera* cam = mgr->GetItem( name );
			if( cam ) {
				if( Implementation::CameraCommon* cc =
				    dynamic_cast<Implementation::CameraCommon*>( cam ) )
				{
					cc->SetDimensionsAndPixelAR( w, h, pAR );
				}
			}
			return true;
		}
	};
	ResyncCb cb;
	cb.mgr = pCameraManager;
	cb.w   = width;
	cb.h   = height;
	cb.pAR = pixelAR;
	pCameraManager->EnumerateItemNames( cb );
}

void Scene::SetFilm( IFilm* pFilm_ )
{
	// Last-write-wins.  Same concurrency contract as the camera
	// mutators above (no concurrent rendering).  Null is treated as
	// a no-op rather than clearing — there is no rasterizer codepath
	// that handles a null film, and accidentally nulling it would
	// just defer the crash.
	//
	// Addref-then-release order (not release-then-addref).  Self-
	// assignment SetFilm(pFilm) with refcount==1 would destroy the
	// object on release before addref runs — UAF.  Addref first is
	// the standard refcounted-swap idiom; the temporary `prev` keeps
	// the old strong ref alive until the swap is committed.
	if( !pFilm_ ) {
		return;
	}
	pFilm_->addref();
	IFilm* prev = pFilm;
	pFilm = pFilm_;
	safe_release( prev );

	ResyncCamerasToFilmDims( pFilm->GetWidth(), pFilm->GetHeight(), pFilm->GetPixelAR() );
}

void Scene::ResizeFilm(
	unsigned int width,
	unsigned int height,
	Scalar       pixelAR
	)
{
	// Hot path: mutate the existing Film's dims in place + re-sync
	// cameras.  Zero allocation per call, so the SceneEditController
	// preview-scale pump can toggle dims at every interactive frame
	// without churning the heap.  Caller is responsible for ensuring
	// pFilm exists (Job::InitializeContainers installs the qHD
	// default unconditionally).
	if( !pFilm ) {
		return;
	}
	pFilm->Resize( width, height, pixelAR );
	ResyncCamerasToFilmDims( width, height, pixelAR );
}

// Lifetime model for the active camera.
//
// Two parties retain the camera:
//   - The camera manager (pCameraManager) holds one ref via AddItem;
//     dropped on RemoveItem (which now safe_releases — see
//     GenericManager.h) or on Shutdown.
//   - Scene holds an INDEPENDENT ref on whichever camera is currently
//     active.  Acquired in AddCamera / SetActiveCamera, dropped when
//     active changes (or on Shutdown).
//
// Why Scene keeps its own ref rather than relying on the manager's:
// it lets RemoveCamera complete its swap (manager release → Scene
// release after auto-promote) without ever leaving pActiveCamera
// pointing at half-destroyed memory.  Each transition is atomic
// from the user's perspective.
//
// **Concurrency contract.**  Structural camera changes (AddCamera,
// RemoveCamera, SetActiveCamera, SetCameraManager) MUST NOT run
// concurrently with rendering.  The editor enforces this via its
// cancel-and-park machinery (see SceneEditController::SetProperty
// for "active_camera" and OnTimeScrub for the canonical pattern):
//   1. Trip the rasterizer's cancel flag.
//   2. cv.wait until the in-flight pass has returned.
//   3. Mutate Scene with the lock held.
//   4. Drop the lock + KickRender for a fresh pass.
// Programmatic callers outside the editor must implement the same
// contract — there is no in-library lock here, by design (matches
// the existing camera-property-edit contract).

bool Scene::AddCamera( const char* szName, ICamera* pCamera_ )
{
	// Empty / null names violate the IJob contract (camera names are
	// the manager's primary key + the active-camera identifier).
	// Catch them here so the manager doesn't end up with a "" entry
	// that GetActiveCameraName().size() <= 1 mistakes for "no active".
	if( !pCameraManager || !szName || !*szName || !pCamera_ ) {
		return false;
	}
	if( !pCameraManager->AddItem( pCamera_, szName ) ) {
		return false;
	}
	// "Last added wins" — every successful AddCamera makes the new
	// camera active.  Take Scene's independent retain on it before
	// dropping the prior active's retain.
	pCamera_->addref();
	ICamera* prevActive = pActiveCamera;
	activeCameraName = String( szName );
	pActiveCamera = pCamera_;
	safe_release( prevActive );
	return true;
}

bool Scene::RemoveCamera( const char* szName )
{
	if( !pCameraManager || !szName || !*szName ) {
		return false;
	}
	const bool wasActive = ( activeCameraName.size() > 1 ) &&
	                       ( strcmp( activeCameraName.c_str(), szName ) == 0 );

	// Snapshot Scene's retain before it's potentially overwritten by
	// auto-promote, then clear the publish pointer so Scene's
	// invariants are kept consistent across the manager release.
	ICamera* prevActive = nullptr;
	if( wasActive ) {
		prevActive = pActiveCamera;
		pActiveCamera = 0;
		activeCameraName = String();
	}

	if( !pCameraManager->RemoveItem( szName ) ) {
		// Removal failed — restore the active state we just cleared
		// so the scene isn't left in a dead-active limbo.
		if( wasActive && prevActive ) {
			activeCameraName = String( szName );
			pActiveCamera = prevActive;
		}
		return false;
	}

	if( wasActive ) {
		// Auto-promote: pick whatever name is first in the manager's
		// iteration order.  GenericManager backs items in a
		// std::map<String,...>, so iteration is lexicographic —
		// stable and deterministic, which is what the design's
		// "auto-promote" rule requires.  If the manager is now
		// empty, the callback runs zero times and activeCameraName
		// stays empty — the renderer's null-camera guard
		// (PixelBasedRasterizerHelper.cpp line 150 etc.) handles
		// that.
		struct PickFirst : public IEnumCallback<const char*> {
			String pick;
			bool operator()( const char* const& name ) override {
				if( pick.size() <= 1 ) pick = String( name );
				return true;
			}
		};
		PickFirst cb;
		pCameraManager->EnumerateItemNames( cb );
		if( cb.pick.size() > 1 ) {
			ICamera* newActive = pCameraManager->GetItem( cb.pick.c_str() );
			if( newActive ) {
				newActive->addref();
				activeCameraName = cb.pick;
				pActiveCamera = newActive;
			}
		}

		// Drop Scene's retain on the removed camera.  Combined with
		// the manager's RemoveItem release above, this is what
		// destroys the camera object (count was 2, both refs now
		// gone).  Per the concurrency contract at the top of this
		// file, no render is in flight here.
		safe_release( prevActive );
	}
	return true;
}

bool Scene::SetActiveCamera( const char* szName )
{
	// SetFilm re-syncs EVERY camera's frame to the active Film, so
	// switching the active camera is always safe — whichever camera
	// becomes active already has its frame matching Film.  Multi-
	// camera scenes that authored different per-camera dims will see
	// every camera resync to the most-recent Film; per-camera dims
	// are not preserved across SetFilm.  See Scene::SetFilm.
	if( !pCameraManager || !szName || !*szName ) {
		return false;
	}
	ICamera* cam = pCameraManager->GetItem( szName );
	if( !cam ) {
		return false;
	}
	// Take Scene's retain on the new active before dropping the
	// prior retain — same idiom as AddCamera.
	cam->addref();
	ICamera* prevActive = pActiveCamera;
	activeCameraName = String( szName );
	pActiveCamera = cam;
	safe_release( prevActive );
	return true;
}

void Scene::SetObjectManager( const IObjectManager* pObjectManager_ )
{
	if( pObjectManager_ )
	{
		// Free the old one
		safe_release( pObjectManager );

		pObjectManager_->addref();
		pObjectManager = pObjectManager_;
	}
}

void Scene::SetLightManager( const ILightManager* pLightManager_ )
{
	if( pLightManager_ )
	{
		safe_release( pLightManager );

		pLightManager_->addref();
		pLightManager = pLightManager_;
	}
}
void Scene::SetGlobalRadianceMap( const IRadianceMap* pRadianceMap )
{
	if( pRadianceMap ) {
		safe_release( pGlobalRadianceMap );

		pGlobalRadianceMap = pRadianceMap;
		pGlobalRadianceMap->addref();
	}
}

void Scene::SetCausticPelMap( IPhotonMap* pPhotonMap )
{
	bool bAlreadyExisted = false;
	Scalar oldrad = 1.0;
	Scalar oldratio = 0.05;
	unsigned int oldminphot = 20;
	unsigned int oldmaxphot = 150;

	if( pCausticMap ) {
		bAlreadyExisted = true;
		pCausticMap->GetGatherParams( oldrad, oldratio, oldminphot, oldmaxphot );
		safe_release( pCausticMap );
	}

	pCausticMap = pPhotonMap;
	pPhotonMap->addref();

	if( bAlreadyExisted ) {
		pCausticMap->SetGatherParams( oldrad, oldratio, oldminphot, oldmaxphot, 0 );
	}
}

void Scene::SetGlobalPelMap( IPhotonMap* pPhotonMap )
{
	bool bAlreadyExisted = false;
	Scalar oldrad = 1.0;
	Scalar oldratio = 0.05;
	unsigned int oldminphot = 20;
	unsigned int oldmaxphot = 150;

	if( pGlobalMap ) {
		bAlreadyExisted = true;
		pGlobalMap->GetGatherParams( oldrad, oldratio, oldminphot, oldmaxphot );
		safe_release( pGlobalMap );
	}

	pGlobalMap = pPhotonMap;
	pPhotonMap->addref();

	if( bAlreadyExisted ) {
		pGlobalMap->SetGatherParams( oldrad, oldratio, oldminphot, oldmaxphot, 0 );
	}
}

void Scene::SetTranslucentPelMap( IPhotonMap* pPhotonMap )
{
	bool bAlreadyExisted = false;
	Scalar oldrad = 1.0;
	Scalar oldratio = 0.05;
	unsigned int oldminphot = 20;
	unsigned int oldmaxphot = 150;

	if( pTranslucentMap ) {
		bAlreadyExisted = true;
		pTranslucentMap->GetGatherParams( oldrad, oldratio, oldminphot, oldmaxphot );
		safe_release( pTranslucentMap );
	}

	pTranslucentMap = pPhotonMap;
	pPhotonMap->addref();

	if( bAlreadyExisted ) {
		pTranslucentMap->SetGatherParams( oldrad, oldratio, oldminphot, oldmaxphot, 0 );
	}
}

void Scene::SetCausticSpectralMap( ISpectralPhotonMap* pPhotonMap )
{
	bool bAlreadyExisted = false;
	Scalar oldrad = 1.0;
	Scalar oldratio = 0.05;
	unsigned int oldminphot = 20;
	unsigned int oldmaxphot = 150;
	Scalar oldrange = 1.0;

	if( pCausticSpectralMap ) {
		bAlreadyExisted = true;
		pCausticSpectralMap->GetGatherParamsNM( oldrad, oldratio, oldminphot, oldmaxphot, oldrange );
		safe_release( pCausticSpectralMap );
	}

	pCausticSpectralMap = pPhotonMap;
	pPhotonMap->addref();

	if( bAlreadyExisted ) {
		pCausticSpectralMap->SetGatherParamsNM( oldrad, oldratio, oldminphot, oldmaxphot, oldrange, 0 );
	}
}

void Scene::SetGlobalSpectralMap( ISpectralPhotonMap* pPhotonMap )
{
	bool bAlreadyExisted = false;
	Scalar oldrad = 1.0;
	Scalar oldratio = 0.05;
	unsigned int oldminphot = 20;
	unsigned int oldmaxphot = 150;
	Scalar oldrange = 1.0;

	if( pGlobalSpectralMap ) {
		bAlreadyExisted = true;
		pGlobalSpectralMap->GetGatherParamsNM( oldrad, oldratio, oldminphot, oldmaxphot, oldrange );
		safe_release( pGlobalSpectralMap );
	}

	pGlobalSpectralMap = pPhotonMap;
	pPhotonMap->addref();

	if( bAlreadyExisted ) {
		pGlobalSpectralMap->SetGatherParamsNM( oldrad, oldratio, oldminphot, oldmaxphot, oldrange, 0 );
	}
}

void Scene::SetShadowMap( IShadowPhotonMap* pPhotonMap )
{
	bool bAlreadyExisted = false;
	Scalar oldrad = 1.0;
	Scalar oldratio = 0.05;
	unsigned int oldminphot = 20;
	unsigned int oldmaxphot = 150;

	if( pShadowMap ) {
		bAlreadyExisted = true;
		pShadowMap->GetGatherParams( oldrad, oldratio, oldminphot, oldmaxphot );
		safe_release( pShadowMap );
	}

	pShadowMap = pPhotonMap;
	pPhotonMap->addref();

	if( bAlreadyExisted ) {
		pShadowMap->SetGatherParams( oldrad, oldratio, oldminphot, oldmaxphot, 0 );
	}
}

void Scene::SetIrradianceCache( IIrradianceCache* pCache )
{
	if( pCache ) {
		safe_release( pIrradianceCache );

		pIrradianceCache = pCache;
		pCache->addref();
	}
}

void Scene::SetGlobalMedium( const IMedium* pMedium )
{
	if( pMedium ) {
		safe_release( pGlobalMedium );

		pGlobalMedium = pMedium;
		pGlobalMedium->addref();
	}
}

void Scene::SetSceneTime( const Scalar time ) const
{
	pObjectManager->ResetRuntimeData();

	// A new time is being set, so we should tell the photon maps to re-generate themselves

	if( pCausticMap ) {
		pCausticMap->Regenerate( time );
	}

	if( pGlobalMap ) {
		if( pGlobalMap->Regenerate( time ) ) {
			// Clear the irradiance cache
			if( pIrradianceCache ) {
				pIrradianceCache->Clear();
			}
		}
	} else {
		// Clear the irradiance cache
		// Just in case 
		if( pIrradianceCache ) {
			pIrradianceCache->Clear();
		}
	}

	if( pTranslucentMap ) {
		pTranslucentMap->Regenerate( time );
	}

	if( pCausticSpectralMap ) {
		pCausticSpectralMap->Regenerate( time );
	}

	if( pGlobalSpectralMap ) {
		pGlobalSpectralMap->Regenerate( time );
	}

	if( pShadowMap ) {
		pShadowMap->Regenerate( time );
	}
}

void Scene::SetSceneTimeForPreview( const Scalar time ) const
{
	// Advance all animators to `time` — without this, scrubbing the
	// timeline slider had no visible effect because the keyframed
	// transforms / camera / parameters never updated.  The animator
	// drives every keyframe-bound parameter (object positions,
	// orientations, camera pose, material values, etc.) by walking
	// each timeline and calling SetIntermediateValue on the bound
	// parameter.  Production rendering does this from the rasterizer
	// loop (e.g. PixelBasedRasterizerHelper.cpp:1180), but the
	// preview path bypasses that loop and must drive the animator
	// directly.
	if( pAnimator ) {
		pAnimator->EvaluateAtTime( time );
	}

	// Animated transforms move objects between BSP nodes, so the
	// spatial structure must be rebuilt for the next render.  This
	// matches what the production animation loop does at line 1148
	// of PixelBasedRasterizerHelper.cpp.  We invalidate
	// unconditionally rather than gating on bHasKeyframedObjects —
	// the rasterizer's PrepareForRendering already short-circuits
	// when the structure is up-to-date, so the cost is bounded.
	pObjectManager->InvalidateSpatialStructure();

	// Reset per-object runtime state (intersection caches etc.) so
	// freshly-mutated transforms are honored on the next render.
	pObjectManager->ResetRuntimeData();

	// Photon-map regeneration is intentionally SKIPPED: it is the
	// expensive part of SetSceneTime (often seconds), and the
	// interactive preview rasterizer (InteractivePelRasterizer,
	// pixelpel-class) does not consult photon maps.  See the
	// IScene contract — production renders that DO consult them
	// must invoke the full SetSceneTime() before dispatch.
	//
	// The irradiance-cache clear that SetSceneTime performs as a
	// side-effect of pGlobalMap->Regenerate is also skipped; the
	// preview rasterizer does not consult the irradiance cache,
	// and the next production-mode SetSceneTime() will clear it.
}

//
// Deferred photon-shoot queueing
//

void Scene::QueueCausticPelPhotonShoot( const PendingCausticPelShoot& req )
{
	mCausticPelPending = req;
	mCausticPelPending.pending = true;
}

void Scene::QueueGlobalPelPhotonShoot( const PendingGlobalPelShoot& req )
{
	mGlobalPelPending = req;
	mGlobalPelPending.pending = true;
}

void Scene::QueueTranslucentPelPhotonShoot( const PendingTranslucentPelShoot& req )
{
	mTranslucentPelPending = req;
	mTranslucentPelPending.pending = true;
}

void Scene::QueueCausticSpectralPhotonShoot( const PendingCausticSpectralShoot& req )
{
	mCausticSpectralPending = req;
	mCausticSpectralPending.pending = true;
}

void Scene::QueueGlobalSpectralPhotonShoot( const PendingGlobalSpectralShoot& req )
{
	mGlobalSpectralPending = req;
	mGlobalSpectralPending.pending = true;
}

void Scene::QueueShadowPhotonShoot( const PendingShadowShoot& req )
{
	mShadowPending = req;
	mShadowPending.pending = true;
}

void Scene::QueueCausticPelGatherParams( Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons )
{
	mCausticPelGather.set = true;
	mCausticPelGather.radius = radius;
	mCausticPelGather.ellipseRatio = ellipseRatio;
	mCausticPelGather.minPhotons = minPhotons;
	mCausticPelGather.maxPhotons = maxPhotons;
}

void Scene::QueueGlobalPelGatherParams( Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons )
{
	mGlobalPelGather.set = true;
	mGlobalPelGather.radius = radius;
	mGlobalPelGather.ellipseRatio = ellipseRatio;
	mGlobalPelGather.minPhotons = minPhotons;
	mGlobalPelGather.maxPhotons = maxPhotons;
}

void Scene::QueueTranslucentPelGatherParams( Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons )
{
	mTranslucentPelGather.set = true;
	mTranslucentPelGather.radius = radius;
	mTranslucentPelGather.ellipseRatio = ellipseRatio;
	mTranslucentPelGather.minPhotons = minPhotons;
	mTranslucentPelGather.maxPhotons = maxPhotons;
}

void Scene::QueueCausticSpectralGatherParams( Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons, Scalar nmRange )
{
	mCausticSpectralGather.set = true;
	mCausticSpectralGather.radius = radius;
	mCausticSpectralGather.ellipseRatio = ellipseRatio;
	mCausticSpectralGather.minPhotons = minPhotons;
	mCausticSpectralGather.maxPhotons = maxPhotons;
	mCausticSpectralGather.nmRange = nmRange;
}

void Scene::QueueGlobalSpectralGatherParams( Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons, Scalar nmRange )
{
	mGlobalSpectralGather.set = true;
	mGlobalSpectralGather.radius = radius;
	mGlobalSpectralGather.ellipseRatio = ellipseRatio;
	mGlobalSpectralGather.minPhotons = minPhotons;
	mGlobalSpectralGather.maxPhotons = maxPhotons;
	mGlobalSpectralGather.nmRange = nmRange;
}

void Scene::QueueShadowGatherParams( Scalar radius, Scalar ellipseRatio, unsigned int minPhotons, unsigned int maxPhotons )
{
	mShadowGather.set = true;
	mShadowGather.radius = radius;
	mShadowGather.ellipseRatio = ellipseRatio;
	mShadowGather.minPhotons = minPhotons;
	mShadowGather.maxPhotons = maxPhotons;
}

namespace {
	// Diagnostic counter; see Scene::GetPhotonShootCount in Scene.h.  Single-
	// threaded (BuildPendingPhotonMaps runs at the pre-parallel prepare seam).
	unsigned int s_photonShootCount = 0;
}

unsigned int Scene::GetPhotonShootCount() { return s_photonShootCount; }
void         Scene::ResetPhotonShootCount() { s_photonShootCount = 0; }

bool Scene::BuildPendingPhotonMaps( IProgressCallback* pProgress )
{
	// Count this as a real shoot pass iff something is actually pending (the
	// caller gates the invocation on the rasterizer consuming photon maps).
	if( mGlobalPelPending.pending || mCausticPelPending.pending || mTranslucentPelPending.pending ||
	    mCausticSpectralPending.pending || mGlobalSpectralPending.pending || mShadowPending.pending ) {
		++s_photonShootCount;
	}

	// Global pel
	if( mGlobalPelPending.pending ) {
		const PendingGlobalPelShoot& r = mGlobalPelPending;
		IPhotonTracer* pTracer = 0;
		RISE_API_CreateGlobalPelPhotonTracer( &pTracer, r.maxRecur, r.minImportance,
			r.branch, r.shootFromNonMeshLights, r.powerScale, r.temporalSamples,
			r.regenerate, r.shootFromMeshLights );
		pTracer->AttachScene( this );
		pTracer->TracePhotons( r.num, 1.0, false, pProgress );
		safe_release( pTracer );
		if( mGlobalPelGather.set && pGlobalMap ) {
			pGlobalMap->SetGatherParams( mGlobalPelGather.radius, mGlobalPelGather.ellipseRatio,
				mGlobalPelGather.minPhotons, mGlobalPelGather.maxPhotons, pProgress );
		}
		mGlobalPelPending.pending = false;
	}

	// Caustic pel
	if( mCausticPelPending.pending ) {
		const PendingCausticPelShoot& r = mCausticPelPending;
		IPhotonTracer* pTracer = 0;
		RISE_API_CreateCausticPelPhotonTracer( &pTracer, r.maxRecur, r.minImportance,
			r.branch, r.reflect, r.refract, r.shootFromNonMeshLights, r.powerScale,
			r.temporalSamples, r.regenerate, r.shootFromMeshLights );
		pTracer->AttachScene( this );
		pTracer->TracePhotons( r.num, 1.0, false, pProgress );
		safe_release( pTracer );
		if( mCausticPelGather.set && pCausticMap ) {
			pCausticMap->SetGatherParams( mCausticPelGather.radius, mCausticPelGather.ellipseRatio,
				mCausticPelGather.minPhotons, mCausticPelGather.maxPhotons, pProgress );
		}
		mCausticPelPending.pending = false;
	}

	// Translucent pel
	if( mTranslucentPelPending.pending ) {
		const PendingTranslucentPelShoot& r = mTranslucentPelPending;
		IPhotonTracer* pTracer = 0;
		RISE_API_CreateTranslucentPelPhotonTracer( &pTracer, r.maxRecur, r.minImportance,
			r.reflect, r.refract, r.directTranslucent, r.shootFromNonMeshLights,
			r.powerScale, r.temporalSamples, r.regenerate, r.shootFromMeshLights );
		pTracer->AttachScene( this );
		pTracer->TracePhotons( r.num, 1.0, false, pProgress );
		safe_release( pTracer );
		if( mTranslucentPelGather.set && pTranslucentMap ) {
			pTranslucentMap->SetGatherParams( mTranslucentPelGather.radius, mTranslucentPelGather.ellipseRatio,
				mTranslucentPelGather.minPhotons, mTranslucentPelGather.maxPhotons, pProgress );
		}
		mTranslucentPelPending.pending = false;
	}

	// Caustic spectral
	if( mCausticSpectralPending.pending ) {
		const PendingCausticSpectralShoot& r = mCausticSpectralPending;
		IPhotonTracer* pTracer = 0;
		RISE_API_CreateCausticSpectralPhotonTracer( &pTracer, r.maxRecur, r.minImportance,
			r.nmBegin, r.nmEnd, r.numWavelengths, r.branch, r.reflect, r.refract,
			r.powerScale, r.temporalSamples, r.regenerate );
		pTracer->AttachScene( this );
		pTracer->TracePhotons( r.num, 1.0, false, pProgress );
		safe_release( pTracer );
		if( mCausticSpectralGather.set && pCausticSpectralMap ) {
			pCausticSpectralMap->SetGatherParamsNM( mCausticSpectralGather.radius, mCausticSpectralGather.ellipseRatio,
				mCausticSpectralGather.minPhotons, mCausticSpectralGather.maxPhotons,
				mCausticSpectralGather.nmRange, pProgress );
		}
		mCausticSpectralPending.pending = false;
	}

	// Global spectral
	if( mGlobalSpectralPending.pending ) {
		const PendingGlobalSpectralShoot& r = mGlobalSpectralPending;
		IPhotonTracer* pTracer = 0;
		RISE_API_CreateGlobalSpectralPhotonTracer( &pTracer, r.maxRecur, r.minImportance,
			r.nmBegin, r.nmEnd, r.numWavelengths, r.branch, r.powerScale,
			r.temporalSamples, r.regenerate );
		pTracer->AttachScene( this );
		pTracer->TracePhotons( r.num, 1.0, false, pProgress );
		safe_release( pTracer );
		if( mGlobalSpectralGather.set && pGlobalSpectralMap ) {
			pGlobalSpectralMap->SetGatherParamsNM( mGlobalSpectralGather.radius, mGlobalSpectralGather.ellipseRatio,
				mGlobalSpectralGather.minPhotons, mGlobalSpectralGather.maxPhotons,
				mGlobalSpectralGather.nmRange, pProgress );
		}
		mGlobalSpectralPending.pending = false;
	}

	// Shadow
	if( mShadowPending.pending ) {
		const PendingShadowShoot& r = mShadowPending;
		IPhotonTracer* pTracer = 0;
		RISE_API_CreateShadowPhotonTracer( &pTracer, r.temporalSamples, r.regenerate );
		pTracer->AttachScene( this );
		pTracer->TracePhotons( r.num, 1.0, false, pProgress );
		safe_release( pTracer );
		if( mShadowGather.set && pShadowMap ) {
			pShadowMap->SetGatherParams( mShadowGather.radius, mShadowGather.ellipseRatio,
				mShadowGather.minPhotons, mShadowGather.maxPhotons, pProgress );
		}
		mShadowPending.pending = false;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
//
// feature/gui-snapshot-prototype: immutable, render-faithful scene
// snapshot.  See Scene.h for the per-state clone-vs-addref rationale.
//
//////////////////////////////////////////////////////////////////////

SceneSnapshot::SceneSnapshot() :
  clonedCamera( 0 ),
  cameraPose( 0 ),
  activeCameraName(),
  clonedFilm( 0 ),
  globalRadianceMap( 0 ),
  globalMedium( 0 )
{
}

SceneSnapshot::~SceneSnapshot()
{
	// Drop our retain on every cloned object (CloneSnapshot returned them
	// with refcount 1; AddClonedObject took ownership without an extra
	// addref, so one release per clone destroys them).
	for( size_t i = 0; i < clonedObjects.size(); ++i ) {
		safe_release( clonedObjects[i] );
	}
	clonedObjects.clear();

	// Lights: each entry holds exactly one reference (the clone's own ref,
	// or the addref taken in the unknown-type fallback).
	for( size_t i = 0; i < clonedLights.size(); ++i ) {
		safe_release( clonedLights[i] );
	}
	clonedLights.clear();

	safe_release( clonedCamera );
	safe_delete( cameraPose );
	safe_release( clonedFilm );
	safe_release( globalRadianceMap );
	safe_release( globalMedium );
}

void SceneSnapshot::AddClonedObject( Object* obj, const String& name )
{
	// Takes ownership of `obj`'s single reference (no extra addref).
	clonedObjects.push_back( obj );
	objectNames.push_back( name );
}

void SceneSnapshot::AddClonedLight( const ILight* light, const String& name )
{
	// Takes ownership of `light`'s single reference (CloneLightForSnapshot
	// returns it with one ref — either the clone's own, or an addref in the
	// unknown-type fallback — and we do not addref again).
	clonedLights.push_back( light );
	lightNames.push_back( name );
}

void SceneSnapshot::SetClonedCamera( ICamera* cam, const CameraPoseSnapshot& pose, const String& name )
{
	// `cam` may be null (ONB / unknown camera that CloneCameraForSnapshot
	// refused) — the pose is captured regardless so pose-only callers still
	// work.  Takes ownership of `cam`'s single reference when non-null.
	safe_release( clonedCamera );
	clonedCamera = cam;

	safe_delete( cameraPose );
	cameraPose = new CameraPoseSnapshot( pose );
	GlobalLog()->PrintNew( cameraPose, __FILE__, __LINE__, "snapshot camera pose" );
	activeCameraName = name;
}

void SceneSnapshot::SetFilm( IFilm* film )
{
	// Takes ownership of `film`'s single reference (the clone's own).
	safe_release( clonedFilm );
	clonedFilm = film;
}

void SceneSnapshot::SetGlobalRadianceMap( const IRadianceMap* rmap )
{
	// ADDREF-shared: the environment map is not editor-mutated in place, so
	// we hold a shared reference (the builder addref'd before calling us).
	safe_release( globalRadianceMap );
	globalRadianceMap = rmap;
}

void SceneSnapshot::SetGlobalMedium( const IMedium* medium )
{
	// Takes ownership of `medium`'s single reference (CloneMediumForSnapshot
	// returns it with one ref — the homogeneous clone's own, or an addref in
	// the baked / unknown fallback).
	safe_release( globalMedium );
	globalMedium = medium;
}

Matrix4 SceneSnapshot::GetObjectFinalTransform( size_t index ) const
{
	if( index >= clonedObjects.size() || !clonedObjects[index] ) {
		return Matrix4Ops::Identity();
	}
	return clonedObjects[index]->GetFinalTransformMatrix();
}

String SceneSnapshot::GetObjectName( size_t index ) const
{
	if( index >= objectNames.size() ) {
		return String();
	}
	return objectNames[index];
}

const Object* SceneSnapshot::GetClonedObject( size_t index ) const
{
	if( index >= clonedObjects.size() ) {
		return 0;
	}
	return clonedObjects[index];
}

const IMaterial* SceneSnapshot::GetObjectMaterial( size_t index ) const
{
	if( index >= clonedObjects.size() || !clonedObjects[index] ) {
		return 0;
	}
	return clonedObjects[index]->GetMaterial();
}

Point3 SceneSnapshot::GetCameraPosition() const
{
	if( !cameraPose ) {
		return Point3( 0, 0, 0 );
	}
	return cameraPose->position;
}

Point3 SceneSnapshot::GetCameraLookAt() const
{
	if( !cameraPose ) {
		return Point3( 0, 0, 0 );
	}
	return cameraPose->lookAt;
}

const ILight* SceneSnapshot::GetLight( size_t index ) const
{
	if( index >= clonedLights.size() ) {
		return 0;
	}
	return clonedLights[index];
}

String SceneSnapshot::GetLightName( size_t index ) const
{
	if( index >= lightNames.size() ) {
		return String();
	}
	return lightNames[index];
}

unsigned int SceneSnapshot::GetFilmWidth() const
{
	return clonedFilm ? clonedFilm->GetWidth() : 0u;
}

unsigned int SceneSnapshot::GetFilmHeight() const
{
	return clonedFilm ? clonedFilm->GetHeight() : 0u;
}

Scalar SceneSnapshot::GetFilmPixelAR() const
{
	return clonedFilm ? clonedFilm->GetPixelAR() : Scalar( 1 );
}

SceneSnapshot* Scene::CreateSnapshot() const
{
	SceneSnapshot* pSnap = new SceneSnapshot();
	GlobalLog()->PrintNew( pSnap, __FILE__, __LINE__, "scene snapshot" );

	// --- Clone every object's mutable wrapper state ---
	// Enumerate by NAME so we can both look the object up via the manager
	// (which hands back the live IObjectPriv) AND record a stable name for
	// the snapshot's read accessors.  The live object is downcast to the
	// concrete Object so we can call CloneSnapshot() (a concrete method,
	// not an interface virtual) — same dynamic_cast pattern Scene uses for
	// CameraCommon in ResyncCamerasToFilmDims.
	if( pObjectManager ) {
		struct CloneCb : public IEnumCallback<const char*> {
			const IObjectManager* mgr;
			SceneSnapshot*        snap;
			bool operator()( const char* const& name ) override {
				IObjectPriv* live = mgr->GetItem( name );
				if( live ) {
					if( Implementation::Object* concrete =
					    dynamic_cast<Implementation::Object*>( live ) )
					{
						Object* clone = concrete->CloneSnapshot();
						snap->AddClonedObject( clone, String( name ) );
					}
				}
				return true;
			}
		};
		CloneCb cb;
		cb.mgr  = pObjectManager;
		cb.snap = pSnap;
		pObjectManager->EnumerateItemNames( cb );
	}

	// --- Clone every light (CLONE, not addref) ---
	// Lights are edited IN PLACE by the editor (SceneEditor::SetLightProperty
	// -> SetIntermediateValue rebinds color/energy/dir/cone), so addref-
	// sharing would bleed live edits.  CloneLightForSnapshot rebuilds each
	// from its public emission accessors (transform-derived position
	// restored) and falls back to addref for unknown out-of-tree types.
	if( pLightManager ) {
		const ILightManager::LightsList& lights = pLightManager->getLights();
		for( size_t i = 0; i < lights.size(); ++i ) {
			const ILightPriv* live = lights[i];
			if( !live ) { continue; }
			const ILight* clone = Implementation::CloneLightForSnapshot( live );
			if( clone ) {
				pSnap->AddClonedLight( clone, String() );
			}
		}
	}
	// NOTE on luminaries: the luminary list (ILuminaryManager) is DERIVED
	// from emissive objects at RayCaster::AttachScene time, not separately
	// authored state — so cloning the objects (above) is what makes mesh
	// luminaires faithful; there is no extra per-luminary mutable state to
	// capture here.

	// --- Render-faithful active-camera clone (CLONE, full params) ---
	// Replaces the pose-only capture: the editor edits camera params in
	// place (CameraIntrospection::SetProperty), so pose alone is not
	// faithful.  CloneCameraForSnapshot rebuilds the concrete camera with
	// ALL parameters; it returns null for ONB-constructed / unknown cameras
	// (honest refusal — see header), in which case the pose summary is the
	// best we can do.  The pose is captured in BOTH cases.
	if( pActiveCamera ) {
		ICamera* camClone = Implementation::CloneCameraForSnapshot( pActiveCamera );
		if( Implementation::CameraCommon* cc =
		    dynamic_cast<Implementation::CameraCommon*>( pActiveCamera ) )
		{
			pSnap->SetClonedCamera( camClone, cc->CaptureSnapshot(), activeCameraName );
		}
		else if( camClone ) {
			// No CameraCommon (out-of-tree camera) but a clone somehow
			// succeeded — store it with a default pose summary.
			pSnap->SetClonedCamera( camClone, CameraPoseSnapshot(), activeCameraName );
		}
	}

	// --- Film (CLONE — IFilm::Resize mutates in place during preview) ---
	if( pFilm ) {
		IFilm* filmClone = 0;
		RISE_API_CreateFilm( &filmClone, pFilm->GetWidth(), pFilm->GetHeight(),
			pFilm->GetPixelAR() );
		if( filmClone ) {
			pSnap->SetFilm( filmClone );   // takes the single ref
		}
	}

	// --- Environment / global radiance map (ADDREF — not editor-mutated) ---
	if( pGlobalRadianceMap ) {
		pGlobalRadianceMap->addref();
		pSnap->SetGlobalRadianceMap( pGlobalRadianceMap );
	}

	// --- Global medium (CLONE homogeneous / ADDREF baked) ---
	if( pGlobalMedium ) {
		const IMedium* medClone = Implementation::CloneMediumForSnapshot( pGlobalMedium );
		if( medClone ) {
			pSnap->SetGlobalMedium( medClone );   // takes the single ref
		}
	}

	return pSnap;
}

//////////////////////////////////////////////////////////////////////
//
// feature/gui-snapshot-prototype, increment 2a: restore / publish.
//
// Swap a snapshot's render-read state back INTO the live scene and
// leave it render-valid.  CLONE-ON-RESTORE — the snapshot is reusable
// for repeated rollback.  See Scene.h for the full contract +
// concurrency note.
//
//////////////////////////////////////////////////////////////////////

namespace {
	// Collect every name a manager currently holds.  Used to clear the
	// live object / light / camera managers before re-installing the
	// snapshot's clones (we can't RemoveItem while EnumerateItemNames
	// iterates the underlying std::map, so snapshot the names first).
	struct CollectNamesCb : public RISE::IEnumCallback<const char*> {
		std::vector<RISE::String> names;
		bool operator()( const char* const& name ) override {
			names.push_back( RISE::String( name ) );
			return true;
		}
	};
}

void Scene::RestoreFromSnapshot( const SceneSnapshot& snap )
{
	// ----------------------------------------------------------------
	// (1) OBJECTS — clear the live object manager, then install a FRESH
	//     clone of every snapshot object.  Object::CloneSnapshot() is
	//     virtual (CSGObject overrides it), so polymorphic types survive
	//     the round-trip.  Each clone is born with refcount 1; AddItem
	//     addrefs, so we release our build reference afterward.
	// ----------------------------------------------------------------
	if( pObjectManager ) {
		// IObjectManager is held const in Scene; AddItem / RemoveItem /
		// EnumerateItemNames are non-const on IManager.  The cast is
		// contained here, mirroring GetGlobalRadianceMapMutable()'s
		// documented pattern — the manager is mutated only between
		// renders under the caller's cancel-and-park.
		IObjectManager* objMgr = const_cast<IObjectManager*>( pObjectManager );

		CollectNamesCb live;
		objMgr->EnumerateItemNames( live );
		for( size_t i = 0; i < live.names.size(); ++i ) {
			objMgr->RemoveItem( live.names[i].c_str() );
		}

		for( size_t i = 0; i < snap.GetObjectCount(); ++i ) {
			const Object* src = snap.GetClonedObject( i );
			if( !src ) { continue; }
			// CloneSnapshot is const + virtual; the const source yields a
			// fresh, independent, non-const clone.
			Object* clone = src->CloneSnapshot();
			String  name  = snap.GetObjectName( i );
			if( name.size() <= 1 ) {
				// Degenerate / unnamed (shouldn't happen for captured
				// objects, but keep the manager key unique).
				char buf[40];
				std::snprintf( buf, sizeof(buf), "__restored_obj_%zu", i );
				name = String( buf );
			}
			objMgr->AddItem( clone, name.c_str() );
			safe_release( clone );
		}
	}

	// ----------------------------------------------------------------
	// (2) LIGHTS — clear the live light manager, then install a fresh
	//     clone of every snapshot light.  RemoveItem / AddItem are
	//     VIRTUAL: LightManager overrides them to keep its cachedLights
	//     list (returned by getLights()) in sync, so clearing name-by-
	//     name + re-adding leaves the cache correct (a plain Shutdown()
	//     would NOT — it clears items but not cachedLights).
	//
	//     The snapshot does not capture per-light names (capture stored
	//     String()), so synthesize unique manager keys; light identity
	//     for rendering is the instance + its emission, not the name.
	// ----------------------------------------------------------------
	if( pLightManager ) {
		ILightManager* lightMgr = const_cast<ILightManager*>( pLightManager );

		CollectNamesCb live;
		lightMgr->EnumerateItemNames( live );
		for( size_t i = 0; i < live.names.size(); ++i ) {
			lightMgr->RemoveItem( live.names[i].c_str() );
		}

		for( size_t i = 0; i < snap.GetLightCount(); ++i ) {
			const ILight* src = snap.GetLight( i );
			if( !src ) { continue; }
			// Re-clone through the same dispatcher the snapshot used at
			// capture (dynamic_cast on the concrete type) — yields a
			// fresh, independent clone, so the snapshot is not consumed.
			const ILight* cloneL = Implementation::CloneLightForSnapshot( src );
			if( !cloneL ) { continue; }
			// The built-in clones are concrete ILightPriv subtypes; the
			// manager stores ILightPriv*.  An unknown out-of-tree type
			// that fell back to addref MIGHT not be ILightPriv — skip it
			// rather than crash (documented residual).
			ILightPriv* clonePriv =
				dynamic_cast<ILightPriv*>( const_cast<ILight*>( cloneL ) );
			if( clonePriv ) {
				char buf[40];
				std::snprintf( buf, sizeof(buf), "__restored_light_%zu", i );
				lightMgr->AddItem( clonePriv, buf );
			} else {
				GlobalLog()->PrintEx( eLog_Warning,
					"Scene::RestoreFromSnapshot:: light %zu is not an ILightPriv (unknown out-of-tree type); skipped", i );
			}
			safe_release( cloneL );
		}
	}

	// ----------------------------------------------------------------
	// (3) ACTIVE CAMERA — clear the camera manager and install a fresh
	//     clone of the snapshot's active camera under its original name.
	//     CloneCameraForSnapshot rebuilds the concrete camera with ALL
	//     params (re-cloning the snapshot's clone, so the snapshot is
	//     not consumed).
	//
	//     HONEST: an ONB / unknown active camera the snapshot could not
	//     clone (clonedCamera == NULL) is NOT restored — the live active
	//     camera is left untouched and we warn.  There is no faithful
	//     pose-only rebuild path (the non-ONB factories would degrade an
	//     ONB basis — see CloneCameraForSnapshot).
	// ----------------------------------------------------------------
	{
		const ICamera* snapCam = snap.GetClonedCamera();
		if( snapCam && pCameraManager ) {
			ICamera* camClone = Implementation::CloneCameraForSnapshot( snapCam );
			if( camClone ) {
				// Clear existing cameras.  RemoveCamera handles the
				// active-camera bookkeeping (auto-promote / clear) as we
				// drain, then AddCamera re-establishes the active one.
				CollectNamesCb live;
				pCameraManager->EnumerateItemNames( live );
				for( size_t i = 0; i < live.names.size(); ++i ) {
					RemoveCamera( live.names[i].c_str() );
				}

				String camName = snap.GetActiveCameraName();
				if( camName.size() <= 1 ) {
					camName = String( "default" );
				}
				// AddCamera addrefs + makes it active; release our build
				// reference.  It also re-syncs the camera frame to Film,
				// which we set just below — order is fine because SetFilm
				// re-syncs every camera again.
				AddCamera( camName.c_str(), camClone );
				safe_release( camClone );
			}
		} else if( snap.HasCamera() && !snapCam ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"Scene::RestoreFromSnapshot:: snapshot camera was ONB-constructed / unclonable; live active camera left unchanged" );
		}
	}

	// ----------------------------------------------------------------
	// (4) FILM — install a fresh film matching the snapshot's dims.
	//     SetFilm re-syncs every camera's projection to the new dims.
	// ----------------------------------------------------------------
	if( snap.HasFilm() ) {
		IFilm* filmClone = 0;
		RISE_API_CreateFilm( &filmClone, snap.GetFilmWidth(), snap.GetFilmHeight(),
			snap.GetFilmPixelAR() );
		if( filmClone ) {
			SetFilm( filmClone );           // addrefs
			safe_release( filmClone );
		}
	}

	// ----------------------------------------------------------------
	// (5) ENVIRONMENT — addref-share the snapshot's radiance map, which
	//     is itself addref-shared from the original live map (the env is
	//     not editor-mutated in place; matches the capture policy).
	//     SetGlobalRadianceMap addrefs.
	// ----------------------------------------------------------------
	if( snap.HasEnvironment() ) {
		SetGlobalRadianceMap( snap.GetGlobalRadianceMap() );
	}

	// ----------------------------------------------------------------
	// (6) GLOBAL MEDIUM — install a fresh clone (homogeneous) / addref
	//     (baked) via the same dispatcher the snapshot used.
	// ----------------------------------------------------------------
	if( snap.GetGlobalMedium() ) {
		const IMedium* medClone =
			Implementation::CloneMediumForSnapshot( snap.GetGlobalMedium() );
		if( medClone ) {
			SetGlobalMedium( medClone );    // addrefs
			safe_release( medClone );
		}
	}

	// ----------------------------------------------------------------
	// (7) REBUILD DERIVED STRUCTURES.
	//
	//     TLAS: the object manager's top-level BVH was built from the
	//     OLD objects' world AABBs.  We swapped the objects out, so the
	//     structure is stale (dangling element pointers).  Drop it; the
	//     next PrepareForRendering() / RayCaster::AttachScene() rebuilds
	//     it over the restored objects (the `!pBVH` guard triggers a
	//     fresh CreateBVH).  This is exactly what SetSceneTimeForPreview
	//     does after animated transforms move objects between nodes
	//     (Scene.cpp InvalidateSpatialStructure call).
	//
	//     LightSampler: Scene does NOT own one — the LuminaryManager +
	//     LightSampler + EnvironmentSampler are built inside
	//     RayCaster::AttachScene and cached on the RayCaster.  Scene has
	//     no handle to them, so it cannot re-prepare them directly.
	//
	//     IMPORTANT CAVEAT (honest): RayCaster::AttachScene early-returns
	//     when re-attached to the SAME IScene pointer (RayCaster.cpp
	//     `if (pScene == pScene_) return;`), so a render that REUSES the
	//     same rasterizer/caster against this (unchanged) Scene pointer
	//     will NOT rebuild the LightSampler — its alias table / luminaries
	//     list / scene-bounds cache would still reflect the PRE-restore
	//     lights and emissive objects.  (The realize pass + the caller's
	//     own PrepareForRendering DO run before that early-return, which is
	//     why the TLAS above rebuilds reliably; the light-sampler setup is
	//     AFTER it.)  This is a pre-existing engine property — the same is
	//     true for any in-place light/topology edit on a reused caster,
	//     and rasterizers are cached in Job::rasterizerRegistry.  To get a
	//     render-faithful LightSampler after a restore that changed lights
	//     or emissive geometry, the caller MUST force a fresh attach —
	//     e.g. recreate the active rasterizer (drop it from the registry)
	//     so a new RayCaster runs the full AttachScene path.  Wiring that
	//     into the editor's restore-then-render flow is increment 2b's job;
	//     #2a deliberately does not reach into the rasterizer layer.
	//
	//     We additionally ResetRuntimeData() so any per-object
	//     intersection caches from the prior contents are cleared.
	// ----------------------------------------------------------------
	if( pObjectManager ) {
		pObjectManager->InvalidateSpatialStructure();
		pObjectManager->ResetRuntimeData();
	}
}

