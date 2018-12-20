//////////////////////////////////////////////////////////////////////
//
//  WardIsotropicGaussianSPF.h - Defines the SPF for Greg Ward's 
//    isotropic gaussian BRDF as described in his paper "Measuring
//    and Modelling Anisotropic Reflection" available here:
//    http://radsite.lbl.gov/radiance/papers/sg92/paper.html
//  
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WARD_ISOTROPIC_GAUSSIAN_SPF_
#define WARD_ISOTROPIC_GAUSSIAN_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class WardIsotropicGaussianSPF :
			public virtual ISPF, 
			public virtual Reference
		{
		protected:
			virtual ~WardIsotropicGaussianSPF( );

			const IPainter&	diffuse;			// diffuse reflectance
			const IPainter&	specular;			// specular reflectance
			const IPainter&	alpha;				// phong exponent

		public:
			WardIsotropicGaussianSPF(
				const IPainter& diffuse_,
				const IPainter& specular_,
				const IPainter& alpha_
				);

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.  
			void	Scatter( 
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,				///< [in] Random number generator
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into 
			//! account spectral affects.  
			void	ScatterNM( 
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,				///< [in] Random number generator
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;
		};
	}
}

#endif
