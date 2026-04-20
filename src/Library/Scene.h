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
	namespace Implementation
	{
		class Scene : 
			public virtual IScenePriv, 
			public virtual Reference
		{
		protected:
			// For now a scene only has a list of objects and some kind of camera
			const IObjectManager*		pObjectManager;
			const ILightManager*		pLightManager;
			const ILuminaryManager*		pLuminaryManager;
			ICamera*					pCamera;

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
			const ICamera*				GetCamera( )	const	{ return pCamera; }

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
			ICamera*				GetCameraMutable()				{ return pCamera; }
			IPhotonMap*				GetCausticPelMapMutable()		{ return pCausticMap; }
			IPhotonMap*				GetGlobalPelMapMutable()		{ return pGlobalMap; }
			IPhotonMap*				GetTranslucentPelMapMutable()	{ return pTranslucentMap; }
			ISpectralPhotonMap*		GetCausticSpectralMapMutable()	{ return pCausticSpectralMap; }
			ISpectralPhotonMap*		GetGlobalSpectralMapMutable()	{ return pGlobalSpectralMap; }
			IShadowPhotonMap*		GetShadowMapMutable()			{ return pShadowMap; }
			IIrradianceCache*		GetIrradianceCacheMutable()		{ return pIrradianceCache; }
			void		SetCamera( ICamera* pCamera_ );
			void		SetObjectManager( const IObjectManager* pObjectManager_ );
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

			void		Shutdown();
		};
	}
}

#endif
