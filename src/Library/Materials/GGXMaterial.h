//////////////////////////////////////////////////////////////////////
//
//  GGXMaterial.h - Material wrapper combining GGXBRDF and GGXSPF.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GGX_MATERIAL_
#define GGX_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "GGXBRDF.h"
#include "GGXSPF.h"
#include "LambertianEmitter.h"

namespace RISE
{
	namespace Implementation
	{
		class GGXMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			GGXBRDF*			pBRDF;
			GGXSPF*				pSPF;
			LambertianEmitter*	pEmitter;	///< Optional, NULL when no emissive painter was supplied

			virtual ~GGXMaterial()
			{
				safe_release( pBRDF );
				safe_release( pSPF );
				safe_release( pEmitter );
			}

		public:
			GGXMaterial(
				const IPainter& diffuse,
				const IPainter& specular,
				const IScalarPainter& alphaX,
				const IScalarPainter& alphaY,
				const IScalarPainter& ior,
				const IScalarPainter& ext,
				const FresnelMode fresnel_mode = eFresnelConductor,
				const IPainter* tangent_rotation = nullptr,	///< Landing 8 / KHR_materials_anisotropy.  See GGXBRDF.h for semantics.
				const IScalarPainter* film_ior = nullptr,		///< Thin-film FILM slots (eFresnelThinFilmConductor).  See GGXBRDF.h.
				const IScalarPainter* film_extinction = nullptr,
				const IScalarPainter* film_thickness = nullptr
				) : pEmitter( 0 )
			{
				pBRDF = new GGXBRDF( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode, tangent_rotation, film_ior, film_extinction, film_thickness );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new GGXSPF( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode, tangent_rotation, film_ior, film_extinction, film_thickness );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			//! Overload for the emissive case (glTF baseColor + emissive workflow).
			//! `emissive` is a painter sampling the per-hit radiant exitance
			//! (typically baseColor * emissiveTexture * emissiveFactor for glTF
			//! pbrMetallicRoughness materials), `emissiveScale` multiplies it
			//! before the emitter samples (lets users push intensity past the
			//! [0,1] sampled-painter range).  Pass `nullptr` for `emissive` to
			//! get the same behaviour as the no-emissive constructor.
			GGXMaterial(
				const IPainter& diffuse,
				const IPainter& specular,
				const IScalarPainter& alphaX,
				const IScalarPainter& alphaY,
				const IScalarPainter& ior,
				const IScalarPainter& ext,
				const IPainter* emissive,
				const Scalar    emissiveScale,
				const FresnelMode fresnel_mode = eFresnelConductor,
				const IPainter* tangent_rotation = nullptr,	///< Landing 8 / KHR_materials_anisotropy.
				const IScalarPainter* film_ior = nullptr,		///< Thin-film FILM slots (eFresnelThinFilmConductor).  See GGXBRDF.h.
				const IScalarPainter* film_extinction = nullptr,
				const IScalarPainter* film_thickness = nullptr
				) : pEmitter( 0 )
			{
				pBRDF = new GGXBRDF( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode, tangent_rotation, film_ior, film_extinction, film_thickness );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new GGXSPF( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode, tangent_rotation, film_ior, film_extinction, film_thickness );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );

				if( emissive ) {
					pEmitter = new LambertianEmitter( *emissive, emissiveScale );
					GlobalLog()->PrintNew( pEmitter, __FILE__, __LINE__, "GGX emitter" );
				}
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return pEmitter; };

			//! Rescales emissive output (backs `> modify material <name> scale`).
			//! Non-emissive GGX (pEmitter NULL) rejects; an emissive GGX rebuilds
			//! its LambertianEmitter at the new scale, mirroring
			//! LambertianLuminaireMaterial::SetEmissionScale (hold a bridging ref on
			//! the radiance painter across the old emitter's release).
			bool SetEmissionScale( const Scalar scale )
			{
				if( !pEmitter ) {
					return false;
				}
				const IPainter& radEx = pEmitter->GetRadEx();
				radEx.addref();
				safe_release( pEmitter );
				pEmitter = new LambertianEmitter( radEx, scale );
				GlobalLog()->PrintNew( pEmitter, __FILE__, __LINE__, "GGX emitter" );
				radEx.release();
				return true;
			}

			//! Read-back + rebind for the interactive editor.  Each
			//! `Set*` hits BOTH the BRDF and the SPF in lockstep so
			//! the shaded value and scattering distribution stay in
			//! agreement.  Composed materials (PBR-MR, GGX-Emissive)
			//! are gated upstream via IJob::IsMaterialComposed — the
			//! editor refuses to call SetSlot on them so this Material
			//! doesn't need to gate per-slot itself.
			inline const IPainter&       GetDiffuse()    const { return pBRDF->GetDiffuse(); }
			inline const IPainter&       GetSpecular()   const { return pBRDF->GetSpecular(); }
			inline const IScalarPainter& GetAlphaX()     const { return pBRDF->GetAlphaX(); }
			inline const IScalarPainter& GetAlphaY()     const { return pBRDF->GetAlphaY(); }
			inline const IScalarPainter& GetIOR()        const { return pBRDF->GetIOR(); }
			inline const IScalarPainter& GetExtinction() const { return pBRDF->GetExtinction(); }
			//! Thin-film FILM slots + the active Fresnel mode.  The film
			//! getters delegate to the BRDF and return POINTERS because the
			//! slots are NULLABLE (bound only in eFresnelThinFilmConductor);
			//! callers MUST null-check.  GetFresnelMode lets the property
			//! panel surface the film rows only for a thin-film material.
			inline const IScalarPainter* GetFilmIOR()        const { return pBRDF->GetFilmIOR(); }
			inline const IScalarPainter* GetFilmExtinction() const { return pBRDF->GetFilmExtinction(); }
			inline const IScalarPainter* GetFilmThickness()  const { return pBRDF->GetFilmThickness(); }
			inline FresnelMode           GetFresnelMode()    const { return pBRDF->GetFresnelMode(); }
			inline void SetDiffuse( const IPainter& v )         { pBRDF->SetDiffuse( v );    pSPF->SetDiffuse( v ); }
			inline void SetSpecular( const IPainter& v )        { pBRDF->SetSpecular( v );   pSPF->SetSpecular( v ); }
			inline void SetAlphaX( const IScalarPainter& v )    { pBRDF->SetAlphaX( v );     pSPF->SetAlphaX( v ); }
			inline void SetAlphaY( const IScalarPainter& v )    { pBRDF->SetAlphaY( v );     pSPF->SetAlphaY( v ); }
			inline void SetIOR( const IScalarPainter& v )       { pBRDF->SetIOR( v );        pSPF->SetIOR( v ); }
			inline void SetExtinction( const IScalarPainter& v ){ pBRDF->SetExtinction( v ); pSPF->SetExtinction( v ); }
			//! Thin-film FILM-slot rebind — hits BOTH the BRDF and the SPF
			//! in lockstep (exactly like SetIOR) so the shaded value and the
			//! sampled distribution never diverge.
			inline void SetFilmIOR( const IScalarPainter& v )        { pBRDF->SetFilmIOR( v );        pSPF->SetFilmIOR( v ); }
			inline void SetFilmExtinction( const IScalarPainter& v ) { pBRDF->SetFilmExtinction( v ); pSPF->SetFilmExtinction( v ); }
			inline void SetFilmThickness( const IScalarPainter& v )  { pBRDF->SetFilmThickness( v );  pSPF->SetFilmThickness( v ); }
		};
	}
}

#endif
