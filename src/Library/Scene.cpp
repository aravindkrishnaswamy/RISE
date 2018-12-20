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
  pGlobalAtmosphere( 0 )
{
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

void Scene::SetGlobalAtmosphere( const IAtmosphere* pAtmosphere )
{
	if( pAtmosphere ) {
		safe_release( pGlobalAtmosphere );

		pGlobalAtmosphere = pAtmosphere;
		pGlobalAtmosphere->addref();
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

