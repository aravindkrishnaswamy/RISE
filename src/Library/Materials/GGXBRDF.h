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
#include "../Interfaces/IMaterial.h"		// For FresnelMode
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
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

			//! Pointer storage so the interactive editor can rebind
			//! via Set*.  See LambertianBRDF for pattern.  The names
			//! already start with `p` (a pre-Phase-4 naming choice
			//! that happens to match the new pointer reality, so no
			//! site rename was needed when changing `&` to `*`).
			const IPainter*			pDiffuse;
			const IPainter*			pSpecular;
			const IScalarPainter*	pAlphaX;		// roughness (physical scalar)
			const IScalarPainter*	pAlphaY;
			const IScalarPainter*	pIOR;
			const IScalarPainter*	pExtinction;
			const FresnelMode fresnelMode;
			//! Landing 8: optional tangent-frame rotation (radians,
			//! applied around w to the (u, v) basis) per
			//! KHR_materials_anisotropy.  nullptr = no rotation
			//! (every existing GGX site falls back here, matching
			//! pre-L8 behaviour bit-identically).  When non-null,
			//! the painter is sampled per-shading-point so a
			//! texture or procedural can drive the rotation.
			const IPainter* pTangentRotation;

		public:
			GGXBRDF(
				const IPainter& diffuse,
				const IPainter& specular,
				const IScalarPainter& alphaX,
				const IScalarPainter& alphaY,
				const IScalarPainter& ior,
				const IScalarPainter& ext,
				const FresnelMode fresnel_mode = eFresnelConductor,
				const IPainter* tangent_rotation = nullptr
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.  Material
			//! forwards to BOTH the BRDF and SPF in lockstep so they
			//! never drift.  Tangent rotation is already pointer-typed
			//! (it's optional/nullable) so SetTangentRotation accepts
			//! a pointer; pass null to clear.
			inline const IPainter&       GetDiffuse()    const { return *pDiffuse; }
			inline const IPainter&       GetSpecular()   const { return *pSpecular; }
			inline const IScalarPainter& GetAlphaX()     const { return *pAlphaX; }
			inline const IScalarPainter& GetAlphaY()     const { return *pAlphaY; }
			inline const IScalarPainter& GetIOR()        const { return *pIOR; }
			inline const IScalarPainter& GetExtinction() const { return *pExtinction; }
			void SetDiffuse( const IPainter& v );
			void SetSpecular( const IPainter& v );
			void SetAlphaX( const IScalarPainter& v );
			void SetAlphaY( const IScalarPainter& v );
			void SetIOR( const IScalarPainter& v );
			void SetExtinction( const IScalarPainter& v );
		};
	}
}

#endif
