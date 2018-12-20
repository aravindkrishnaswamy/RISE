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

			const IAtmosphere*			pGlobalAtmosphere;

			virtual ~Scene( );

		public:
			Scene( );

			const IObjectManager*		GetObjects( )	const	{ return pObjectManager; }
			const ILightManager*		GetLights( )	const	{ return pLightManager; }
			const ILuminaryManager*		GetLuminaries() const	{ return pLuminaryManager; }
			ICamera*					GetCamera( )	const	{ return pCamera; }

			inline const IRadianceMap*	GetGlobalRadianceMap() const { return pGlobalRadianceMap; }
			inline IPhotonMap*			GetCausticPelMap()	const	{ return pCausticMap; }
			inline IPhotonMap*			GetGlobalPelMap()	const	{ return pGlobalMap; }
			inline IPhotonMap*			GetTranslucentPelMap() const { return pTranslucentMap; }
			inline ISpectralPhotonMap*	GetCausticSpectralMap() const{ return pCausticSpectralMap; }
			inline ISpectralPhotonMap*	GetGlobalSpectralMap() const{ return pGlobalSpectralMap; }
			inline IShadowPhotonMap*	GetShadowMap() const{ return pShadowMap; }
			inline IIrradianceCache*	GetIrradianceCache() const  { return pIrradianceCache; }

			inline IAnimator*			GetAnimator() const { return pAnimator; }

			inline const IAtmosphere*	GetGlobalAtmosphere() const { return pGlobalAtmosphere; }

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
			void		SetGlobalAtmosphere( const IAtmosphere* pAtmosphere );

			void		SetSceneTime( const Scalar time ) const ;

			void		Shutdown();
		};
	}
}

#endif
