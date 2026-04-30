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
				const IPainter& alphaX,
				const IPainter& alphaY,
				const IPainter& ior,
				const IPainter& ext,
				const FresnelMode fresnel_mode = eFresnelConductor
				) : pEmitter( 0 )
			{
				pBRDF = new GGXBRDF( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new GGXSPF( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode );
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
				const IPainter& alphaX,
				const IPainter& alphaY,
				const IPainter& ior,
				const IPainter& ext,
				const IPainter* emissive,
				const Scalar    emissiveScale,
				const FresnelMode fresnel_mode = eFresnelConductor
				) : pEmitter( 0 )
			{
				pBRDF = new GGXBRDF( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new GGXSPF( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode );
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
		};
	}
}

#endif
