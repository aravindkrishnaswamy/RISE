//////////////////////////////////////////////////////////////////////
//
//  OrenNayarBRDF.h - Defines the Oren-Nayar BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef ORENNAYAR_BRDF_
#define ORENNAYAR_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class OrenNayarBRDF : 
			public virtual IBSDF, 
			public virtual Reference
		{
		protected:
			virtual ~OrenNayarBRDF();

			const IPainter&	pReflectance;
			const IPainter& pRoughness;

		public:
			OrenNayarBRDF( 
				const IPainter& reflectance, 
				const IPainter& roughness
				);

			template< class T >
			static void ComputeFactor( T& L1, T& L2, const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& roughness );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

