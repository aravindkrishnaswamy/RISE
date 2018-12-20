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
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class IsotropicPhongBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~IsotropicPhongBRDF();

			const IPainter&				refdiffuse;
			const IPainter&				refspecular;
			const IPainter&				exponent;

		public:
			IsotropicPhongBRDF( const IPainter& rd, const IPainter& rs, const IPainter& exp );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

