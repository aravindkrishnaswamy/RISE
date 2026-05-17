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
#include "../Interfaces/IScalarPainter.h"
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

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pDiffuse;
			const IPainter*			pSpecular;
			const IScalarPainter*	pMasking;		// roughness (physical scalar)
			const IScalarPainter*	pIOR;
			const IScalarPainter*	pExtinction;

		public:
			CookTorranceBRDF(
				const IPainter& diffuse,
				const IPainter& specular,
				const IScalarPainter& masking,
				const IScalarPainter& ior,
				const IScalarPainter& ext
				);

			template< class T >
			static T ComputeFactor( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& m );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetDiffuse()    const { return *pDiffuse; }
			inline const IPainter&       GetSpecular()   const { return *pSpecular; }
			inline const IScalarPainter& GetMasking()    const { return *pMasking; }
			inline const IScalarPainter& GetIOR()        const { return *pIOR; }
			inline const IScalarPainter& GetExtinction() const { return *pExtinction; }
			void SetDiffuse( const IPainter& v );
			void SetSpecular( const IPainter& v );
			void SetMasking( const IScalarPainter& v );
			void SetIOR( const IScalarPainter& v );
			void SetExtinction( const IScalarPainter& v );
		};
	}
}

#endif

