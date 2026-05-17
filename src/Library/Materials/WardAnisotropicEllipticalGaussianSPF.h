//////////////////////////////////////////////////////////////////////
//
//  WardAnisotropicEllipticalGaussianSPF.h - Defines the SPF for Greg Ward's
//    anisotropic gaussian BRDF as described in his paper "Measuring
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

#ifndef WARD_ANISOTROPIC_ELLIPTICAL_GAUSSIAN_SPF_
#define WARD_ANISOTROPIC_ELLIPTICAL_GAUSSIAN_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class WardAnisotropicEllipticalGaussianSPF :
			public virtual ISPF,
			public virtual Reference
		{
		protected:
			virtual ~WardAnisotropicEllipticalGaussianSPF( );

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pDiffuse;			// diffuse reflectance
			const IPainter*			pSpecular;			// specular reflectance
			const IScalarPainter*	pAlphaX;			// surface slope RMS (physical scalar)
			const IScalarPainter*	pAlphaY;

		public:
			WardAnisotropicEllipticalGaussianSPF(
				const IPainter& diffuse_,
				const IPainter& specular_,
				const IScalarPainter& alphax_,
				const IScalarPainter& alphay_
				);

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetDiffuse()  const { return *pDiffuse; }
			inline const IPainter&       GetSpecular() const { return *pSpecular; }
			inline const IScalarPainter& GetAlphaX()   const { return *pAlphaX; }
			inline const IScalarPainter& GetAlphaY()   const { return *pAlphaY; }
			void SetDiffuse( const IPainter& v );
			void SetSpecular( const IPainter& v );
			void SetAlphaX( const IScalarPainter& v );
			void SetAlphaY( const IScalarPainter& v );

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

			Scalar	Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const;

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
