//////////////////////////////////////////////////////////////////////
//
//  IScene.h - Interface to a scene
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISCENE_
#define ISCENE_

#include "IReference.h"
#include "IRadianceMap.h"
#include "IAtmosphere.h"

namespace RISE
{
	class ILightManager;
	class ILuminaryManager;
	class IObjectManager;
	class IPhotonMap;
	class ISpectralPhotonMap;
	class IShadowPhotonMap;
	class IAnimator;
	class ICamera;
	class IIrradianceCache;

	class IScene : public virtual IReference
	{
	protected:
		IScene(){};
		virtual ~IScene(){};

	public:
		/// \return The object manager
		virtual const IObjectManager*		GetObjects( )	const = 0;

		/// \return The light manager
		virtual const ILightManager*		GetLights( )	const = 0;

		/// \return The currently assigned camera
		virtual ICamera*					GetCamera( )	const = 0;

		/// \return The global radiance map
		virtual const IRadianceMap*			GetGlobalRadianceMap() const = 0;

		/// \return The caustic PEL photon map
		virtual IPhotonMap*					GetCausticPelMap()	const = 0;

		/// \return The global PEL photon map
		virtual IPhotonMap*					GetGlobalPelMap()	const = 0;

		/// \return The translucent PEL photon map
		virtual IPhotonMap*					GetTranslucentPelMap()	const = 0;

		/// \return The caustic spectral photon map
		virtual ISpectralPhotonMap*			GetCausticSpectralMap() const = 0;

		/// \return The caustic global photon map
		virtual ISpectralPhotonMap*			GetGlobalSpectralMap() const = 0;

		/// \return The shadow photon map
		virtual IShadowPhotonMap*			GetShadowMap()	const = 0;

		/// \return The irradiance cache
		virtual IIrradianceCache*			GetIrradianceCache() const = 0;

		/// \return The animator
		virtual IAnimator*					GetAnimator() const = 0;

		/// \return The atmospherics effect
		virtual const IAtmosphere*			GetGlobalAtmosphere() const = 0;

		/// Tells the scene a new time is set 
		virtual void						SetSceneTime( const Scalar time ) const = 0;
	};
}

#include "ILightManager.h"
#include "ILuminaryManager.h"
#include "IObjectManager.h"
#include "IPhotonMap.h"
#include "IAnimator.h"
#include "ICamera.h"
#include "IIrradianceCache.h"

#endif

