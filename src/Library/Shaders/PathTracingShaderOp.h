//////////////////////////////////////////////////////////////////////
//
//  PathTracingShaderOp.h - The path tracing shader op uses 
//    Kajiya's path tracing algorithm to compute shading
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 30, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATHTRACING_SHADER_OP_
#define PATHTRACING_SHADER_OP_

#include "../Interfaces/IShaderOp.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PathTracingShaderOp : 
			public virtual IShaderOp, 
			public virtual Reference
		{
		protected:
			virtual ~PathTracingShaderOp();

			const bool bBranch;
			const bool bForceCheckEmitters;
			const bool bFinalGather;
			const bool bTraceReflection;
			const bool bTraceRefraction;
			const bool bTraceDiffuse;
			const bool bTraceTranslucent;

			bool ShouldTraceRay( const ScatteredRay::ScatRayType type ) const;

		public:
			PathTracingShaderOp( 
				const bool bBranch_,
				const bool forcecheckemitters,
				const bool bFinalGather_,
				const bool reflections,
				const bool refractions,
				const bool diffuse,
				const bool translucents
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
			bool RequireSPF() const { return true; };
		};
	}
}

#endif
