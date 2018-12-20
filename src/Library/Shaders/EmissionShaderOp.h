//////////////////////////////////////////////////////////////////////
//
//  EmissionShaderOp.h - The emission shader op evaluates the emissive
//    properties of a material if it exists and if the raystate
//    calls for it
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 28, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EMISSION_SHADER_OP_
#define EMISSION_SHADER_OP_

#include "../Interfaces/IShaderOp.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class EmissionShaderOp : 
			public virtual IShaderOp, 
			public virtual Reference
		{
		protected:
			virtual ~EmissionShaderOp();

		public:
			EmissionShaderOp( 
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
