//////////////////////////////////////////////////////////////////////
//
//  RayCaster.h - Definition of a class which an attached scene
//    capable of tracing rays through that scene. 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 16, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAYCASTER_
#define RAYCASTER_

#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IRadianceMap.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class Ray;
	class IScene;

	namespace Implementation
	{
		class RayCaster : 
			public virtual IRayCaster, 
			public virtual Reference
		{
		protected:
			const IScene*				pScene;
			const IShader&				pDefaultShader;
			ILuminaryManager*			pLuminaryManager;
			ISampling2D*				pLumSampling;

			bool						bConsiderRMapAsBackground;

			const unsigned int			nMaxRecursions;
			const Scalar				dMinImportance;

			const bool					bShowLuminaires;
			const bool					bIORStack;

			const bool					bChooseOnlyOneLuminaire;

			virtual ~RayCaster();

		public:
			RayCaster( 
				const bool seeRadianceMap,
				const unsigned int maxR, 
				const Scalar minI, 
				const IShader& pDefaultShader_,
				const bool showLuminaires,
				const bool useiorstack,
				const bool chooseonlyoneluminaire
				);

			void AttachScene( const IScene* pScene_ );

			//! Tells the ray caster to cast the specified ray into the scene
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastRay( 
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				RISEPel& c,											///< [out] RISEColor for the ray
				const RAY_STATE& rs,								///< [in] The ray state
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
				) const;

			//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastRayNM( 
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
				const RAY_STATE& rs,								///< [in] The ray state
				const Scalar nm,									///< [in] Wavelength to cast
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
				) const;

			//! Tells the ray caster to cast the specified ray into the scene
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastRay( 
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				RISEPel& c,											///< [out] RISEColor for the ray
				const RAY_STATE& rs,								///< [in] The ray state
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
				const IORStack* const ior_stack						///< [in/out] Index of refraction stack
				) const;

			//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastRayNM( 
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
				const RAY_STATE& rs,								///< [in] The ray state
				const Scalar nm,									///< [in] Wavelength to cast
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
				const IORStack* const ior_stack						///< [in/out] Index of refraction stack
				) const;

			//! This function casts a ray into the scene and only checks to see if it intersects something.
			//! Very useful for shadow checks
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastShadowRay( 
				const Ray& ray,										///< [in] Ray to cast
				const Scalar dHowFar								///< [in] How far to follow the ray, optimization
				) const;

			//! To retreive the current scene
			/// \return Pointer to currently attached scene, NULL if no scene is currently attached
			const IScene* GetAttachedScene() const { return pScene; };

			//! Sets the luminaire sampler
			void SetLuminaireSampling(
				ISampling2D* pLumSam								///< [in] Kernel to use for luminaire sampling
				);

			/// \return The luminary manager for the current scene
			const ILuminaryManager* GetLuminaries() const { return pLuminaryManager; };
		};
	}
}

#include "../Interfaces/IScene.h"

#endif
