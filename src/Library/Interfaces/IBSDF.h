//////////////////////////////////////////////////////////////////////
//
//  IBSDF.h - Defines the interface to a BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IBSDF_
#define IBSDF_

#include "../Utilities/Color/Color.h"
#include "IReference.h"
#include "IRayCaster.h"

namespace RISE
{
	class RayIntersectionGeometric;

	//! Represents the Bi-Directional Scattering Distribution Function 
	//! The BSDF describes how light is reflected/transmitted from a surface.
	//! The following equation describes the BRDF, which is one component of the BSDF.  
	//! The BTDF (which is also a part of the BSDF) is described similarily
	//! \f[
	//!    f_r( x, \Theta_i \rightarrow \Theta_r ) = \frac{dL(x \rightarrow \Theta_r)}{dE( x \leftarrow \Theta_i)} = \frac{dL(x \rightarrow \Theta_r)}{L(x \leftarrow \Theta_i)cos(\Theta_i)dw_{\Theta_i}}
	//! \f]
	//! The BRDF can also be wavelength dependant which simply means that \f$ \Theta_i \f$ and \f$ \Theta_r \f$ can become
	//! wavelength dependant making them \f$ \Theta_{i\lambda} \f$ and \f$ \Theta_{r\lambda} \f$.
	//! The BRDF is dimensionless but it is expressed as \f$ \frac{1}{sr} \f$.
	//! One of the fundamental things that makes a BRDF work is reciprocity which means that the energy
	//! flow in each direction is the same, highlighted by the following equation:
	//! \f[
	//!   f_r( x, \Theta_i \rightarrow \Theta_r ) = f_r( x, \Theta_r \rightarrow \Theta_i ) = f_r(x, \Theta_i \leftrightarrow \Theta_r )
	//! \f]
	//! In order to be physical, a BRDF must also obey the law of enegy conservation, meaning that
	//! integrating over the hemisphere must be less than or equal to 1, shown in the equation below
	//! \f[
	//!   \forall\Theta: \int_{\Omega_x}fr(x, \Theta \leftrightarrow \Psi) cos( n_x, \Psi) dw_\Psi \leq 1
	//! \f]
	class IBSDF : public virtual IReference
	{
	protected:
		IBSDF(){}
		virtual ~IBSDF(){}

	public:

		/// \return Actual BRDF value given the following parameters, this is returned
		///         as an RISEPel
		virtual RISEPel	value( 
			const Vector3& vLightIn,						///< [in] Incoming vector from the light source
			const RayIntersectionGeometric& ri				///< [in] Geometric intersection information
			) const = 0;		

		/// \return Actual BRDF value given the following parameters as a scalar (double)
		virtual Scalar	valueNM(
			const Vector3& vLightIn,						///< [in] Incoming vector from the light source
			const RayIntersectionGeometric& ri,				///< [in] Geometric intersection information
			const Scalar nm									///< [in] Wavelength of spectral packet we are processing
			) const = 0;

		/// Approximate directional-hemispherical reflectance at the
		/// outgoing direction implied by `ri.ray` — i.e., the fraction
		/// of incident energy reflected back toward the camera under
		/// uniform white illumination.  Intended for the OIDN albedo
		/// AOV: must be in [0, 1] per channel and noise-free per pixel
		/// so OIDN can run with cleanAux=true.  Default returns white,
		/// which is a safe conservative AOV (no chromatic info → OIDN
		/// treats the surface as illumination-only).  Concrete BSDFs
		/// override with closed-form reflectance estimates.
		virtual RISEPel albedo(
			const RayIntersectionGeometric& /*ri*/			///< [in] Geometric intersection information (provides view direction via ri.ray)
			) const
		{
			return RISEPel( 1.0, 1.0, 1.0 );
		}
	};
}

#include "../Intersection/RayIntersectionGeometric.h"

#endif

