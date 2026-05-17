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
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AshikminShirleyAnisotropicPhongBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~AshikminShirleyAnisotropicPhongBRDF( );

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IScalarPainter*	pNu;		// Phong exponent (physical scalar)
			const IScalarPainter*	pNv;

			const IPainter*			pRd;		// diffuse reflectance
			const IPainter*			pRs;		// specular reflectance

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

			AshikminShirleyAnisotropicPhongBRDF( const IScalarPainter& Nu_, const IScalarPainter& Nv_, const IPainter& Rd_, const IPainter& Rs_ );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.
			inline const IScalarPainter& GetNu() const { return *pNu; }
			inline const IScalarPainter& GetNv() const { return *pNv; }
			inline const IPainter&       GetRd() const { return *pRd; }
			inline const IPainter&       GetRs() const { return *pRs; }
			void SetNu( const IScalarPainter& v );
			void SetNv( const IScalarPainter& v );
			void SetRd( const IPainter& v );
			void SetRs( const IPainter& v );
		};
	}
}

#endif

