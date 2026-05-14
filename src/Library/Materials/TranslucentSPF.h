//////////////////////////////////////////////////////////////////////
//
//  TranslucentSPF.h - Defines a SPF that is partially
//  transparent (like a lampshade)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRANSLUCENT_SPF_
#define TRANSLUCENT_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			virtual ~TranslucentSPF( );

			const IPainter&					pRefFront;			// Reflectance (color)
			const IPainter&					pTrans;				// Transmittance of the primary layer (color)
			const IScalarPainter&			pExtinction;		// Extinction factor (physical scalar)
			const IScalarPainter&			N;					// Phong exponent (physical scalar)
			const IScalarPainter&			pScat;				// Multiple scattering factor (physical scalar)


		public:
			TranslucentSPF( const IPainter& rF, const IPainter& T, const IScalarPainter& ext, const IScalarPainter& N_, const IScalarPainter& scat );

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.  
			void	Scatter( 
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,									///< [in] Sampler
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
					) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into 
			//! account spectral affects.  
			void	ScatterNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,									///< [in] Sampler
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Evaluates the PDF for scattering into direction wo
			Scalar	Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const;

			//! Spectral version of Pdf
			Scalar	PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;
		};
	}
}

#endif
