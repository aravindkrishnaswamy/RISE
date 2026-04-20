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
#include "Interfaces/IPhotonTracer.h"
#include "Interfaces/IPhotonMap.h"
#include "Interfaces/IProgressCallback.h"

using namespace RISE;
using namespace RISE::Implementation;

Scene::Scene( ) :
  pObjectManager( 0 ),
  pLightManager( 0 ),
  pLuminaryManager( 0 ),
  pCamera( 0 ),
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
	safe_release( pCamera );
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

void Scene::SetCamera( ICamera*	pCamera_ )
{
	if( pCamera_ )
	{
		// Free the old one
		safe_release( pCamera );

		pCamera_->addref();
		pCamera = pCamera_;
	}
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

