//////////////////////////////////////////////////////////////////////
//
//  AshikminShirleyAnisotropicPhongBRDF.h - Defines the diffuse component of 
//  the Shirley / Ashikhmin Anisotropic Phong BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 10, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ASHIKMINSHIRLEY_ANISOTROPIC_PHONG_BRDF_
#define ASHIKMINSHIRLEY_ANISOTROPIC_PHONG_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AshikminShirleyAnisotropicPhongBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~AshikminShirleyAnisotropicPhongBRDF( );

			const IPainter&			Nu;			// Phong exponents
			const IPainter&			Nv;

			const IPainter&			Rd;			// diffuse reflectance
			const IPainter&			Rs;			// specular reflectance

		public:
			template< class T >
			static void ComputeDiffuseSpecularFactors( 
				T& diffuse, 
				T& specular,
				const Vector3& vLightIn, 
				const RayIntersectionGeometric& ri,
				const T& NU, 
				const T& NV, 
				const T& Rs 
				);

			AshikminShirleyAnisotropicPhongBRDF( const IPainter& Nu_, const IPainter& Nv_, const IPainter& Rd_, const IPainter& Rs_ );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

