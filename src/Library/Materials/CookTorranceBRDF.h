//////////////////////////////////////////////////////////////////////
//
//  CookTorranceBRDF.h - Defines the Cook-Torrance BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef COOKTORRANCE_BRDF_
#define COOKTORRANCE_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class CookTorranceBRDF : 
			public virtual IBSDF, 
			public virtual Reference
		{
		protected:
			virtual ~CookTorranceBRDF();

			const IPainter& pDiffuse;
			const IPainter& pSpecular;
			const IPainter& pMasking;
			const IPainter& pIOR;
			const IPainter& pExtinction;

		public:
			CookTorranceBRDF( 
				const IPainter& diffuse, 
				const IPainter& specular, 
				const IPainter& masking,
				const IPainter& ior,
				const IPainter& ext
				);

			template< class T >
			static T ComputeFactor( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& m );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

