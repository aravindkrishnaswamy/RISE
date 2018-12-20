//////////////////////////////////////////////////////////////////////
//
//  IShader.h - Interface for shaders.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 15, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISHADER_
#define ISHADER_

#include "IReference.h"
#include "IRayCaster.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "ISampling2D.h"

namespace RISE
{
	class RayIntersection;

	//! A shader evaluates the shade (a combination of color and lighting)
	//! for a point on some surface somewhere
	class IShader : public virtual IReference
	{
	protected:
		IShader(){};
		virtual ~IShader(){};

	public:
		//! Tells the shader to apply shade to the given intersection point
		virtual void Shade(
			const RuntimeContext& rc,					///< [in] The runtime context
			const RayIntersection& ri,					///< [in] Intersection information 
			const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
			const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
			RISEPel& c,									///< [out] IFXPel value at the point
			const IORStack* const ior_stack				///< [in/out] Index of refraction stack
			) const = 0;

		//! Tells the shader to apply shade to the given intersection point for the given wavelength
		/// \return Amplitude of spectral function 
		virtual Scalar ShadeNM(
			const RuntimeContext& rc,					///< [in] The runtime context
			const RayIntersection& ri,					///< [in] Intersection information 
			const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
			const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
			const Scalar nm,							///< [in] Wavelength to shade
			const IORStack* const ior_stack				///< [in/out] Index of refraction stack
			) const = 0;

		//! Tells the shader to reset itself
		virtual void ResetRuntimeData() const = 0;
	};
}

#include "../Intersection/RayIntersectionGeometric.h"

#endif

