//////////////////////////////////////////////////////////////////////
//
//  CookTorranceSPF.h - Defines a Cook-Torrance SPF, which just
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

#ifndef COOKTORRANCE_SPF_
#define COOKTORRANCE_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"
#include "CookTorranceBRDF.h"

namespace RISE
{
	namespace Implementation
	{
		class CookTorranceSPF : 
			public virtual ISPF, 
			public virtual Reference
		{
		protected:
			virtual ~CookTorranceSPF( );

			const IPainter& pDiffuse;
			const IPainter& pSpecular;
			const IPainter& pMasking;
			const IPainter& pIOR;
			const IPainter& pExtinction;

		public:
			CookTorranceSPF( 
				const IPainter& diffuse, 
				const IPainter& specular, 
				const IPainter& masking,
				const IPainter& ior,
				const IPainter& ext
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
