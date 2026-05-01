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
#include "Interfaces/IPhotonTracer.h"
#include "Interfaces/IPhotonMap.h"
#include "Interfaces/IProgressCallback.h"

using namespace RISE;
using namespace RISE::Implementation;

Scene::Scene( ) :
  pObjectManager( 0 ),
  pLightManager( 0 ),
  pLuminaryManager( 0 ),
  pCameraManager( 0 ),
  activeCameraName(),
  pActiveCamera( 0 ),
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

bool Scene::BuildPendingPhotonMaps( IProgressCallback* pProgress )
{
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

