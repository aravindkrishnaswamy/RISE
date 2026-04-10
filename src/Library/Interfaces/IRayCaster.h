//////////////////////////////////////////////////////////////////////
//
//  IRayCaster.h - Interface to a ray caster
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRAYCASTER_
#define IRAYCASTER_

#include "IReference.h"
#include "ISampling2D.h"
#include "IRadianceMap.h"
#include "../Utilities/Ray.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	class ILuminaryManager;
	class IScene;
	class IORStack;
	struct RuntimeContext;

	namespace Implementation { class LightSampler; }

	//! A ray caster traces a ray generated on the virtual screen and traces it through the 
	//! scene
	/// \sa IPixelTracer
	class IRayCaster : public virtual IReference
	{
	protected:
		IRayCaster(){};
		virtual ~IRayCaster(){};

	public:

		// RAY_STATE describes the state of a particular ray we are casting
		struct RAY_STATE
		{
			enum RayType
			{
				eRayView			= 0,		///< Viewer ray
				eRayDiffuse			= 1,		///< Diffuse ray
				eRaySpecular		= 2,		///< Some kind of specular ray
				eRayFinalGather		= 3			///< A ray used for the final gather process
			};

			unsigned int depth;					///< Number of times the ray has bounced
			Scalar importance;					///< Importance of this ray
			bool considerEmission;				///< Should shader consider direct emission
			RayType type;						///< The type of ray
			Scalar bsdfPdf;						///< BSDF sampling PDF for MIS weighting (0 = not set / delta)
			RISEPel bsdfTimesCos;				///< BSDF * cos at scatter point (RGB), for optimal MIS full-integrand training

			// Per-type bounce counters for StabilityConfig bounce limits
			unsigned int diffuseBounces;		///< Accumulated diffuse bounces
			unsigned int glossyBounces;			///< Accumulated glossy/reflection bounces
			unsigned int transmissionBounces;	///< Accumulated refraction/transmission bounces
			unsigned int translucentBounces;	///< Accumulated translucent bounces

			Scalar glossyFilterWidth;			///< Accumulated glossy filter roughness increase (0 = no filtering)

			unsigned int volumeBounces;			///< Accumulated volume scattering bounces

			RAY_STATE() : depth( 1 ), importance( 1.0 ), considerEmission( true ), type( eRayView ), bsdfPdf( 0 ),
				diffuseBounces( 0 ), glossyBounces( 0 ), transmissionBounces( 0 ), translucentBounces( 0 ),
				glossyFilterWidth( 0 ), volumeBounces( 0 ) {}
		};

		//! Tells the ray caster to cast the specified ray into the scene
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastRay( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
			) const = 0;

		//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastRayNM( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
			const RAY_STATE& rs,								///< [in] The ray state
			const Scalar nm,									///< [in] Wavelength to cast
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
			) const = 0;

		//! Tells the ray caster to cast the specified ray into the scene
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastRay( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
			const IORStack* const ior_stack						///< [in/out] Index of refraction stack
			) const = 0;

		//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastRayNM( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
			const RAY_STATE& rs,								///< [in] The ray state
			const Scalar nm,									///< [in] Wavelength to cast
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
			const IORStack* const ior_stack						///< [in/out] Index of refraction stack
			) const = 0;

		//! This function casts a ray into the scene and only checks to see if it intersects something.
		//! Very useful for shadow checks
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastShadowRay( 
			const Ray& ray,										///< [in] Ray to cast
			const Scalar dHowFar								///< [in] How far to follow the ray, optimization
			) const = 0;

		//! This function attaches this ray caster to the given scene
		virtual void AttachScene( 
			const IScene* pScene_								///< [in] Scene to attach
			) = 0;

		//! To retreive the current scene
		/// \return Pointer to currently attached scene, NULL if no scene is currently attached
		virtual const IScene* GetAttachedScene() const = 0;

		//! Sets the luminaire sampler
		virtual void SetLuminaireSampling(
			ISampling2D* pLumSam								///< [in] Kernel to use for luminaire sampling
			) = 0;

		/// \return The luminary manager for the current scene
		virtual const ILuminaryManager* GetLuminaries() const = 0;

		/// \return The unified light sampler for the current scene, or NULL if not available
		virtual const Implementation::LightSampler* GetLightSampler() const = 0;

		/// Sets the number of RIS candidates for spatially-aware light
		/// selection.  Must be called after AttachScene().
		virtual void SetRISCandidates(
			const unsigned int M								///< [in] Number of RIS candidates (0=disabled)
			) = 0;

		/// Sets the threshold for light-sample Russian roulette.
		/// Must be called after AttachScene().
		virtual void SetLightSampleRRThreshold(
			const Scalar threshold								///< [in] RR threshold (0=disabled)
			) = 0;

		/// Enables or disables the light BVH for importance-weighted
		/// many-light selection.  Must be called before AttachScene().
		virtual void SetUseLightBVH(
			const bool enable									///< [in] True to enable light BVH
			) = 0;
	};
}

#include "ILuminaryManager.h"
#include "IScene.h"
#include "../Utilities/IORStack.h"
#include "../Utilities/RuntimeContext.h"

#endif
