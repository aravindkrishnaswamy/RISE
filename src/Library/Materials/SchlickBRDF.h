//////////////////////////////////////////////////////////////////////
//
//  SchlickBRDF.h - Defines the Cook-Torrance BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef SCHLICK_BRDF_
#define SCHLICK_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SchlickBRDF : 
			public virtual IBSDF, 
			public virtual Reference
		{
		protected:
			virtual ~SchlickBRDF();

			const IPainter& pDiffuse;
			const IPainter& pSpecular;
			const IPainter& pRoughness;
			const IPainter& pIsotropy;

		public:
			SchlickBRDF( 
				const IPainter& diffuse, 
				const IPainter& specular,
				const IPainter& roughness,
				const IPainter& isotropy
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

