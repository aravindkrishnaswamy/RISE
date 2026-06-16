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
			//! Thin-film (eFresnelThinFilmConductor) FILM slots — the
			//! oxide layer of the air/oxide/metal stack.  Physical
			//! scalars (no JH uplift): film n, film k, film thickness
			//! (nm, may be spatially varying).  nullptr for every other
			//! Fresnel mode (the conductor/Schlick branches never read
			//! them); only dereferenced inside the thin-film branch,
			//! which the parser/factory guarantee supplies all three.
			//! The SUBSTRATE n,k reuse pIOR / pExtinction.
			const IScalarPainter*	pFilmIOR;
			const IScalarPainter*	pFilmExtinction;
			const IScalarPainter*	pFilmThickness;

		public:
			GGXBRDF(
				const IPainter& diffuse,
				const IPainter& specular,
				const IScalarPainter& alphaX,
				const IScalarPainter& alphaY,
				const IScalarPainter& ior,
				const IScalarPainter& ext,
				const FresnelMode fresnel_mode = eFresnelConductor,
				const IPainter* tangent_rotation = nullptr,
				const IScalarPainter* film_ior = nullptr,
				const IScalarPainter* film_extinction = nullptr,
				const IScalarPainter* film_thickness = nullptr
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
			//! Thin-film FILM slots — POINTER-returning because they are
			//! NULLABLE (only the thin-film Fresnel mode binds them; every
			//! other mode leaves them null).  Callers MUST null-check
			//! before dereferencing — unlike GetIOR which returns a
			//! required reference.  GetFresnelMode lets the introspection
			//! layer gate the film rows on the active Fresnel mode.
			inline const IScalarPainter* GetFilmIOR()        const { return pFilmIOR; }
			inline const IScalarPainter* GetFilmExtinction() const { return pFilmExtinction; }
			inline const IScalarPainter* GetFilmThickness()  const { return pFilmThickness; }
			inline FresnelMode           GetFresnelMode()    const { return fresnelMode; }
			void SetDiffuse( const IPainter& v );
			void SetSpecular( const IPainter& v );
			void SetAlphaX( const IScalarPainter& v );
			void SetAlphaY( const IScalarPainter& v );
			void SetIOR( const IScalarPainter& v );
			void SetExtinction( const IScalarPainter& v );
			//! Rebind a thin-film FILM slot.  Same release-old / addref-new
			//! discipline as SetIOR.  Material's forwarder hits BOTH the
			//! BRDF and the SPF in lockstep so the shaded value and the
			//! sampling distribution never drift.  Takes a reference (the
			//! editor only ever rebinds to a registered painter, never to
			//! null) — to CLEAR a film slot you would reconstruct the
			//! material, matching how the other nullable slots behave.
			void SetFilmIOR( const IScalarPainter& v );
			void SetFilmExtinction( const IScalarPainter& v );
			void SetFilmThickness( const IScalarPainter& v );
		};
	}
}

#endif
