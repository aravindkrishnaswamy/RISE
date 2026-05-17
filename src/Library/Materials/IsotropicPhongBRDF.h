//////////////////////////////////////////////////////////////////////
//
//  IsotropicPhongBRDF.h - Defines a phong BRDF 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef ISOTROPICPHONG_BRDF_
#define ISOTROPICPHONG_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class IsotropicPhongBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~IsotropicPhongBRDF();

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pRd;
			const IPainter*			pRs;
			const IScalarPainter*	pExponent;

		public:
			IsotropicPhongBRDF( const IPainter& rd, const IPainter& rs, const IScalarPainter& exp );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetRd()       const { return *pRd; }
			inline const IPainter&       GetRs()       const { return *pRs; }
			inline const IScalarPainter& GetExponent() const { return *pExponent; }
			void SetRd( const IPainter& v );
			void SetRs( const IPainter& v );
			void SetExponent( const IScalarPainter& v );
		};
	}
}

#endif

