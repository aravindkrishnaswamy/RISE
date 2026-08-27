//////////////////////////////////////////////////////////////////////
//
//  HairMaterial.h - Aggregate for the Chiang et al. 2016 near-field
//    hair / fur BCSDF.  Owns BOTH a HairBRDF (IBSDF) and a HairSPF
//    (ISPF) built from the same painter set, following the
//    LambertianMaterial / GGXMaterial pattern.
//
//  It is deliberately NOT the SPF-only DielectricMaterial pattern
//  (`GetBSDF()` returning 0): a null BSDF makes a material invisible
//  to every BDPT / VCM connection strategy, which is right for delta
//  glass and wrong for a rough multi-lobe fibre BCSDF.
//
//  Every other IMaterial hook stays at its default:
//    * GetEmitter()      -> 0     (no emissive fur; the geometry slice
//                                  reports CanBeAreaLight() == false)
//    * GetSpecularInfo() -> default non-specular.  Hair lobes are never
//                           delta (the beta floors in HairBSDF.h see to
//                           that), so SMS correctly never sees hair.
//    * IsVolumetric()    -> false.  Fibre-interior absorption lives
//                           inside the lobe attenuation terms A_p, not
//                           in a medium the integrator marches.
//    * CouldLightPassThrough() -> false.  The TT lobe is ordinary
//                           BSDF-sampled scattering, not the
//                           straight-through transparency this flag
//                           gates.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HAIR_MATERIAL_
#define HAIR_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "HairBSDF.h"

namespace RISE
{
	namespace Implementation
	{
		class HairMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			HairBRDF*		pBRDF;
			HairSPF*		pSPF;

			virtual ~HairMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			//! Both halves are built from the SAME HairPainters, so the
			//! evaluated BSDF and the sampled SPF can never drift.
			explicit HairMaterial( const HairPainters& painters )
			{
				pBRDF = new HairBRDF( painters );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new HairSPF( painters );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  Never NULL -- see the
			///         file header for why hair must be evaluable.
			inline IBSDF* GetBSDF() const {			return pBRDF; };

			/// \return The SPF for this material.
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return NULL -- hair does not emit.
			inline IEmitter* GetEmitter() const {	return 0; };
		};
	}
}

#endif
