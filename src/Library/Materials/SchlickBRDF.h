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
#include "../Interfaces/IScalarPainter.h"
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

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pDiffuse;
			const IPainter*			pSpecular;
			const IScalarPainter*	pRoughness;		// physical scalar
			const IScalarPainter*	pIsotropy;

		public:
			SchlickBRDF(
				const IPainter& diffuse,
				const IPainter& specular,
				const IScalarPainter& roughness,
				const IScalarPainter& isotropy
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetDiffuse()   const { return *pDiffuse; }
			inline const IPainter&       GetSpecular()  const { return *pSpecular; }
			inline const IScalarPainter& GetRoughness() const { return *pRoughness; }
			inline const IScalarPainter& GetIsotropy()  const { return *pIsotropy; }
			void SetDiffuse( const IPainter& v );
			void SetSpecular( const IPainter& v );
			void SetRoughness( const IScalarPainter& v );
			void SetIsotropy( const IScalarPainter& v );
		};
	}
}

#endif

