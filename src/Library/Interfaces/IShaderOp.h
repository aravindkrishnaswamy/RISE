//////////////////////////////////////////////////////////////////////
//
//  IShaderOp.h - Interface for a ShaderOp which is a shader
//    operation.  Shader operations are smaller bite size
//    pieces of shaders, combining several shader ops is what
//    gives us interesting shaders.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 28, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISHADEROP_
#define ISHADEROP_

#include "IReference.h"
#include "IRayCaster.h"
#include "ISPF.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Utilities/RandomNumbers.h"

namespace RISE
{
	class RayIntersection;

	//! A shader evaluates the shade (a combination of color and lighting)
	//! for a point on some surface somewhere
	class IShaderOp : public virtual IReference
	{
	protected:
		IShaderOp(){};
		virtual ~IShaderOp(){};

	public:
		//! Tells the shader to apply shade to the given intersection point
		virtual void PerformOperation( 
			const RuntimeContext& rc,					///< [in] Runtime context
			const RayIntersection& ri,					///< [in] Intersection information 
			const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
			const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
			RISEPel& c,									///< [in/out] Resultant color from op
			const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
			const ScatteredRayContainer* pScat			///< [in] Scattering information
			) const = 0;

		//! Tells the shader to apply shade to the given intersection point for the given wavelength
		/// \return Amplitude of spectral function 
		virtual Scalar PerformOperationNM( 
			const RuntimeContext& rc,					///< [in] Runtime context
			const RayIntersection& ri,					///< [in] Intersection information 
			const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
			const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
			const Scalar c,								///< [in] Current value for wavelength
			const Scalar nm,							///< [in] Wavelength to shade
			const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
			const ScatteredRayContainer* pScat			///< [in] Scattering information
			) const = 0;

		//! Tells the ShaderOp to reset itself
		virtual void ResetRuntimeData() const {};

		//! Asks if the shader op needs SPF data
		virtual bool RequireSPF() const = 0;
	};
}

#include "../Intersection/RayIntersection.h"

#endif

