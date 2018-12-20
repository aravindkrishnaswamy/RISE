//////////////////////////////////////////////////////////////////////
//
//  AreaLightShaderOp.h - The area light shaderop applies lighting
//    account to an area light source.  Note that the net effect
//    would be same if you created a cliiped plane and 
//    applied a lambertian luminaire light source to it, this
//    is just another way of doing the same thing, but in a slightly
//    easier way.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 28, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef AREALIGHT_SHADER_OP_
#define AREALIGHT_SHADER_OP_

#include "../Interfaces/IShaderOp.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/ISampling2D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AreaLightShaderOp : 
			public virtual IShaderOp, 
			public virtual Reference
		{
		protected:
			virtual ~AreaLightShaderOp();

			const Scalar width;		// width of the light source
			const Scalar height;	// height of the light source

			const Point3 location;	// where is the light source located
			const Vector3 dir;		// what is the light source focussed on

			const IPainter& emm;	// The emission of this area light
			const Scalar power;		// Power scale for emission

			const IPainter& N;		// Phong factor for the light, to focus
			const Scalar hotSpot;	// Angle in radians of the light's hotspot

			const bool cache;

			ISampling2D* pSampler;	// Sampler for the light source

			Matrix4 mxtransform;	// Transformation from canonical to us

			const Scalar area;		// Area of this light

		public:
			AreaLightShaderOp( 
				const Scalar width_,			///< [in] Width of the light source
				const Scalar height_,			///< [in] Height of the light source
				const Point3 location_,			///< [in] Where is the light source located
				const Vector3 dir_,				///< [in] What is the light source focussed on
				const unsigned int samples,		///< [in] Number of samples to take
				const IPainter& emm_,			///< [in] Emission of this light
				const Scalar power_,			///< [in] Power scale
				const IPainter& N_,				///< [in] Phong factor for focussing the light on something
				const Scalar hotSpot_,			///< [in] Angle in radians of the light's hot spot
				const bool cache_				///< [in] Should we use the rasterizer state cache?
				);

			//! Tells the shader to apply shade to the given intersection point
			void PerformOperation( 
				const RuntimeContext& rc,					///< [in] Runtime context
				const RayIntersection& ri,					///< [in] Intersection information 
				const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
				const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
				RISEPel& c,									///< [in/out] Resultant color from op
				const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
				const ScatteredRayContainer* pScat			///< [in] Scattering information
				) const;

			//! Tells the shader to apply shade to the given intersection point for the given wavelength
			/// \return Amplitude of spectral function 
			Scalar PerformOperationNM( 
				const RuntimeContext& rc,					///< [in] Runtime context
				const RayIntersection& ri,					///< [in] Intersection information 
				const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
				const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
				const Scalar caccum,						///< [in] Current value for wavelength
				const Scalar nm,							///< [in] Wavelength to shade
				const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
				const ScatteredRayContainer* pScat			///< [in] Scattering information
				) const;

			//! Asks if the shader op needs SPF data
			bool RequireSPF() const { return false; };
		};
	}
}

#endif
