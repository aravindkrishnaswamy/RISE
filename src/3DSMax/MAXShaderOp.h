//////////////////////////////////////////////////////////////////////
//
//  MAXShaderOp.h - 3D Studio MAX shaderop, that calls gets MAX
//    to do the shading for us
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 13, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MAX_SHADEROP_
#define MAX_SHADEROP_

#include "maxincl.h"
#include "RISEIncludes.h"
#include "MAX2RISE_Helpers.h"
#include "scontext.h"

class RiseRenderer;
class RiseRendererParams;

class MAXShaderOp : 
	public virtual RISE::IShaderOp, 
	public virtual RISE::Implementation::Reference
{
protected:
	RiseRenderer* renderer;
	RiseRendererParams* rparams;
	mutable BGContext	bc;
	mutable SContext	sc;

public:
	MAXShaderOp(
		RiseRenderer* renderer_,
		RiseRendererParams* rparams_
		);

	~MAXShaderOp();

	//! Tells the shader to apply shade to the given intersection point
	void PerformOperation( 
		const RISE::RayIntersection& ri,					///< [in] Intersection information 
		const RISE::IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
		const RISE::IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
		RISE::RISEPel& c,									///< [in/out] Resultant color from op
		const RISE::IORStack* const ior_stack,			///< [in/out] Index of refraction stack
		const RISE::ScatteredRayContainer* pScat,			///< [in] Scattering information
		const RISE::RandomNumberGenerator& random			///< [in] Random number generator
		) const;

	//! Tells the shader to apply shade to the given intersection point for the given wavelength
	/// \return Amplitude of spectral function 
	RISE::Scalar PerformOperationNM( 
		const RISE::RayIntersection& ri,					///< [in] Intersection information 
		const RISE::IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
		const RISE::IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
		const RISE::Scalar caccum,						///< [in] Current value for wavelength
		const RISE::Scalar nm,							///< [in] Wavelength to shade
		const RISE::IORStack* const ior_stack,			///< [in/out] Index of refraction stack
		const RISE::ScatteredRayContainer* pScat,			///< [in] Scattering information
		const RISE::RandomNumberGenerator& random			///< [in] Random number generator
		) const;
	//! Asks if the shader op needs SPF data
	bool RequireSPF() const { return false; };
};

#include "rise3dsmax.h"

#endif

