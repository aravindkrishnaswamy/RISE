//////////////////////////////////////////////////////////////////////
//
//  SchlickSPF.h - Defines a Oren-Nayar SPF, which just
//    does uniform sampling but adjusts the weights according
//    to the Cook-Torrance BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCHLICK_SPF_
#define SCHLICK_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SchlickSPF : 
			public virtual ISPF, 
			public virtual Reference
		{
		protected:
			virtual ~SchlickSPF( );

			const IPainter& pDiffuse;
			const IPainter& pSpecular;
			const IPainter& pRoughness;
			const IPainter& pIsotropy;

		public:
			SchlickSPF( 
				const IPainter& diffuse, 
				const IPainter& specular, 
				const IPainter& roughness,
				const IPainter& isotropy
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
