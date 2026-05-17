//////////////////////////////////////////////////////////////////////
//
//  WardAnisotropicEllipticalGaussianBRDF.h - Implements Gred Ward's isotropic
//    gaussian BRDF as described in his paper "Measuring and Modelling
//    Anisotropic Reflection" available here:
//    http://radsite.lbl.gov/radiance/papers/sg92/paper.html
//    
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef WARD_ANISOTROPIC_ELLIPTICAL_GAUSSIAN_BRDF_
#define WARD_ANISOTROPIC_ELLIPTICAL_GAUSSIAN_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class WardAnisotropicEllipticalGaussianBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		protected:
			virtual ~WardAnisotropicEllipticalGaussianBRDF();

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pDiffuse;
			const IPainter*			pSpecular;
			const IScalarPainter*	pAlphaX;		// surface slope RMS (physical scalar)
			const IScalarPainter*	pAlphaY;

		public:
			WardAnisotropicEllipticalGaussianBRDF(
				const IPainter& diffuse_,
				const IPainter& specular_,
				const IScalarPainter& alphax_,
				const IScalarPainter& alphay_
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetDiffuse()  const { return *pDiffuse; }
			inline const IPainter&       GetSpecular() const { return *pSpecular; }
			inline const IScalarPainter& GetAlphaX()   const { return *pAlphaX; }
			inline const IScalarPainter& GetAlphaY()   const { return *pAlphaY; }
			void SetDiffuse( const IPainter& v );
			void SetSpecular( const IPainter& v );
			void SetAlphaX( const IScalarPainter& v );
			void SetAlphaY( const IScalarPainter& v );
		};
	}
}

#endif

