//////////////////////////////////////////////////////////////////////
//
//  FinalGatherShaderOp.h - The final gather shader op performs
//    the final gather which shoots a distribution of final gather
//    rays.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 30, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FINALGATHER_SHADER_OP_
#define FINALGATHER_SHADER_OP_

#include "../Interfaces/IShaderOp.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class FinalGatherShaderOp : 
			public virtual IShaderOp, 
			public virtual Reference
		{
		protected:
			virtual ~FinalGatherShaderOp();

			const unsigned int numTheta;
			const unsigned int numPhi;
			const bool bComputeCacheGradients;
			const bool cache;

		public:
			FinalGatherShaderOp( 
				const unsigned int numTheta_,
				const unsigned int numPhi_,
				const bool bComputeCacheGradients_,
				const bool cache_
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
