//////////////////////////////////////////////////////////////////////
//
//  GGXBRDF.h - GGX microfacet BRDF with anisotropic roughness,
//    Smith height-correlated masking-shadowing, and Kulla-Conty
//    multiscattering energy compensation.
//
//  References:
//    - Walter et al., EGSR 2007 (GGX NDF)
//    - Heitz, JCGT 3(2), 2014 (height-correlated Smith G2)
//    - Kulla & Conty, SIGGRAPH 2017 (energy compensation)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GGX_BRDF_
#define GGX_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class GGXBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		protected:
			virtual ~GGXBRDF();

			const IPainter& pDiffuse;
			const IPainter& pSpecular;
			const IPainter& pAlphaX;
			const IPainter& pAlphaY;
			const IPainter& pIOR;
			const IPainter& pExtinction;

		public:
			GGXBRDF(
				const IPainter& diffuse,
				const IPainter& specular,
				const IPainter& alphaX,
				const IPainter& alphaY,
				const IPainter& ior,
				const IPainter& ext
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;
		};
	}
}

#endif
