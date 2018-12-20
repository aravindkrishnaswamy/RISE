//////////////////////////////////////////////////////////////////////
//
//  StandardShader.h - The standard shader is the common shader
//    which is just something built out of a series of 
//    shader ops.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 30, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef STANDARD_SHADER_
#define STANDARD_SHADER_

#include "../Interfaces/IShader.h"
#include "../Interfaces/IShaderOp.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class StandardShader : 
			public virtual IShader, 
			public virtual Reference
		{
		protected:
			virtual ~StandardShader();

			std::vector<IShaderOp*> shaderops;

			bool bComputeSPF;

		public:
			StandardShader( const std::vector<IShaderOp*>& shaderops_ );

			//! Tells the shader to apply shade to the given intersection point
			void Shade(
				const RuntimeContext& rc,					///< [in] The runtime context
				const RayIntersection& ri,					///< [in] Intersection information 
				const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
				const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
				RISEPel& c,									///< [out] RISEPel value at the point
				const IORStack* const ior_stack				///< [in/out] Index of refraction stack
				) const;

			//! Tells the shader to apply shade to the given intersection point for the given wavelength
			/// \return Amplitude of spectral function 
			Scalar ShadeNM(
				const RuntimeContext& rc,					///< [in] The runtime context
				const RayIntersection& ri,					///< [in] Intersection information 
				const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
				const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
				const Scalar nm,							///< [in] Wavelength to shade
				const IORStack* const ior_stack				///< [in/out] Index of refraction stack
				) const;

			//! Tells the shader to reset itself
			void ResetRuntimeData() const;
		};
	}
}

#endif
