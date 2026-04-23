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

	namespace Implementation { class LightSampler; }

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
			LightSampler*				pLightSampler;
			ISampling2D*				pLumSampling;

			bool						bConsiderRMapAsBackground;

			const unsigned int			nMaxRecursions;

			const bool					bShowLuminaires;

			Scalar						dPendingLightRRThreshold;
			bool						bPendingUseLightBVH;

			virtual ~RayCaster();

		public:
			RayCaster(
				const bool seeRadianceMap,
				const unsigned int maxR,
				const IShader& pDefaultShader_,
				const bool showLuminaires
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
				const IORStack& ior_stack							///< [in/out] Index of refraction stack
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
				const IORStack& ior_stack							///< [in/out] Index of refraction stack
				) const;

			//! Casts a ray for a bundle of HWSS wavelengths with shared
			//! scene intersection.  The hero wavelength drives medium
			//! transport; the shader op evaluates all wavelengths at
			//! the shared geometric intersection.
			/// \return TRUE if any wavelength produced a hit
			bool CastRayHWSS(
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				Scalar c[SampledWavelengths::N],					///< [out] Per-wavelength amplitudes
				const RAY_STATE& rs,								///< [in] The ray state
				SampledWavelengths& swl,							///< [in/out] Wavelength bundle
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap,					///< [in] Radiance map for misses
				const IORStack& ior_stack							///< [in/out] Index of refraction stack
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

			/// \return The unified light sampler for the current scene
			const LightSampler* GetLightSampler() const { return pLightSampler; };

			/// Sets the number of RIS candidates for spatially-aware
			/// light selection.  Must be called after AttachScene().
			void SetRISCandidates( const unsigned int M );

			/// Sets the threshold for light-sample Russian roulette.
			/// Must be called after AttachScene().
			void SetLightSampleRRThreshold( const Scalar threshold );

			/// Enables or disables the light BVH.
			void SetUseLightBVH( const bool enable );

		};
	}
}

#include "../Interfaces/IScene.h"

#endif
