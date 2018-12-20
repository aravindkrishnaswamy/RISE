//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringShaderOp.h - 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 18, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SUBSURFACE_SCATTERING_SHADER_OP_
#define SUBSURFACE_SCATTERING_SHADER_OP_

#include "../../Interfaces/IShaderOp.h"
#include "../../Interfaces/IMaterial.h"
#include "../../Interfaces/ISubSurfaceExtinctionFunction.h"
#include "../../Utilities/Reference.h"
#include "../../Utilities/Threads/Threads.h"
#include "PointSetOctree.h"
#include <vector>
#include <map>

namespace RISE
{
	namespace Implementation
	{
		class SubSurfaceScatteringShaderOp : 
			public virtual IShaderOp, 
			public virtual Reference
		{
		protected:
			virtual ~SubSurfaceScatteringShaderOp();

			const unsigned int numPoints;
			const Scalar error;
			const unsigned int maxPointsPerNode;
			const unsigned char maxDepth;
			const Scalar irrad_scale;
			const bool multiplyBSDF;
			const bool regenerate;
			const IShader& shader;
			const ISubSurfaceExtinctionFunction& extinction;
			const bool cache;
			const bool low_discrepancy;

			typedef std::map<const IObject*,PointSetOctree*> PointSetMap;
			mutable PointSetMap	pointsets;

			const RMutex create_mutex;

		public:
			SubSurfaceScatteringShaderOp( 
				const unsigned int numPoints_,
				const Scalar error_,
				const unsigned int maxPointsPerNode_,
				const unsigned char maxDepth_,
				const Scalar irrad_scale_,
				const bool multiplyBSDF_,
				const bool regenerate_,
				const IShader& shader_,
				const ISubSurfaceExtinctionFunction& extinction_,
				const bool cache_,
				const bool low_discrepancy_
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

			//! Tells the ShaderOp to reset itself
			void ResetRuntimeData() const;

			//! Asks if the shader op needs SPF data
			bool RequireSPF() const { return false; };
		};
	}
}

#endif
