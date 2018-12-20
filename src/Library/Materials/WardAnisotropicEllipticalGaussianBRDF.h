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

			const IPainter&	diffuse;
			const IPainter& specular;
			const IPainter& alphax;			// standard deviation (RMS) of the surface slope in x
			const IPainter& alphay;			// standard deviation (RMS) of the surface slope in y

		public:
			WardAnisotropicEllipticalGaussianBRDF( 
				const IPainter& diffuse_,
				const IPainter& specular_,
				const IPainter& alphax_,
				const IPainter& alphay_
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

