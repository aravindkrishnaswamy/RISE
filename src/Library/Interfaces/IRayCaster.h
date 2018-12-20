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

			RAY_STATE() : depth( 1 ), importance( 1.0 ), considerEmission( true ), type( eRayView ) {}
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
	};
}

#include "ILuminaryManager.h"
#include "IScene.h"
#include "../Utilities/IORStack.h"
#include "../Utilities/RuntimeContext.h"

#endif
