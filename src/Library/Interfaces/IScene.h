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
#include "IMedium.h"

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
		virtual const ICamera*				GetCamera( )	const = 0;

		/// \return The global radiance map
		virtual const IRadianceMap*			GetGlobalRadianceMap() const = 0;

		/// \return The caustic PEL photon map
		virtual const IPhotonMap*			GetCausticPelMap()	const = 0;

		/// \return The global PEL photon map
		virtual const IPhotonMap*			GetGlobalPelMap()	const = 0;

		/// \return The translucent PEL photon map
		virtual const IPhotonMap*			GetTranslucentPelMap()	const = 0;

		/// \return The caustic spectral photon map
		virtual const ISpectralPhotonMap*	GetCausticSpectralMap() const = 0;

		/// \return The caustic global photon map
		virtual const ISpectralPhotonMap*	GetGlobalSpectralMap() const = 0;

		/// \return The shadow photon map
		virtual const IShadowPhotonMap*		GetShadowMap()	const = 0;

		/// \return The irradiance cache
		virtual const IIrradianceCache*		GetIrradianceCache() const = 0;

		/// \return The animator
		/// NOTE: Returns non-const because EvaluateAtTime() mutates keyframed
		/// elements. This is a known scene-immutability exception — the animation
		/// system predates the const-correctness model and a proper fix would
		/// require per-thread interpolated state for temporal sampling.
		virtual IAnimator*					GetAnimator() const = 0;

		/// \return The global participating medium (NULL if vacuum)
		virtual const IMedium*				GetGlobalMedium() const = 0;

		/// Tells the scene a new time is set
		virtual void						SetSceneTime( const Scalar time ) const = 0;

		/// Like SetSceneTime, but skips photon-map regeneration
		/// (and the irradiance-cache clear that's tied to global-map
		/// regen).  Valid only for interactive preview rendering with
		/// non-photon rasterizers (typically the InteractivePelRasterizer).
		///
		/// The caller MUST invoke the full SetSceneTime() exactly once
		/// before any production render of a scene that uses photon
		/// maps, otherwise photons will be stale at the scrubbed time.
		///
		/// See docs/INTERACTIVE_EDITOR_PLAN.md §7 (Timeline Scrubbing).
		virtual void						SetSceneTimeForPreview( const Scalar time ) const = 0;
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

