//////////////////////////////////////////////////////////////////////
//
//  AdvancedShader.h - The advanced shader offers more features
//    than the standard shader such as depth control on a per
//    shaderop basis and different blending and combination
//    options.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 1, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ADVANCED_SHADER_H
#define ADVANCED_SHADER_H

#include "../Interfaces/IShader.h"
#include "../Interfaces/IShaderOp.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AdvancedShader : 
			public virtual IShader, 
			public virtual Reference
		{
		public:
			struct SHADE_OP
			{
				IShaderOp*		pShaderOp;					// The shaderop
				unsigned int	nMinDepth;					// The minimum depth for this op to run (inclusive)
				unsigned int	nMaxDepth;					// The maximum depth for this op to run (inclusive)
				char			operation;					// Operation to perform when combined the results of this shaderop with the others
			};

			typedef std::vector<SHADE_OP> ShadeOpListType;

		protected:
			virtual ~AdvancedShader();

			ShadeOpListType shaderops;

			bool bComputeSPF;

		public:
			AdvancedShader( const ShadeOpListType& shaderops_ );

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
