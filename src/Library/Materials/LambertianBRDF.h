//////////////////////////////////////////////////////////////////////
//
//  LambertianBRDF.h - Defines a lambertian BRDF, which just 
//  reflects light equally in all directions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef LAMBERTIAN_BRDF_
#define LAMBERTIAN_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class LambertianBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~LambertianBRDF();

			const IPainter&	pReflectance;

		public:
			LambertianBRDF( const IPainter& reflectance );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back accessor for the interactive editor's
			//! `MaterialIntrospection` — reverse-lookup the painter's
			//! registered name via the IPainterManager.  Stays a
			//! const-ref because the BRDF stores the painter as a
			//! reference; full edit-rebind support is future work.
			inline const IPainter& GetReflectance() const { return pReflectance; }
		};
	}
}

#endif

