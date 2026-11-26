//////////////////////////////////////////////////////////////////////
//
//  SheenBRDF.h - "Charlie" sheen BRDF for fabric / cloth-like
//    surfaces (the high-grazing-angle scatter that's characteristic
//    of velvet, suede, satin, brushed metal under glancing light).
//
//  Implements the Estevez & Kulla 2017 "Production Friendly
//  Microfacet Sheen BRDF" (Imageworks course notes): a Charlie
//  microfacet distribution with the full Λ-POLYNOMIAL Charlie
//  visibility.  Designed as an additive lobe on top of a base
//  material, layered via CompositeMaterial in glTF
//  KHR_materials_sheen scenes.
//
//  NOT "Charlie / Neubelt", despite what this banner and the
//  `sheen_material` chunk description both said until 2026-09-02.
//  The cheap Ashikhmin / Neubelt closed form
//  `V = 1 / (4·(n·l + n·v − n·l·n·v))` was REPLACED precisely
//  because it blew up to ρ ≈ 8.7 at grazing in the white-furnace
//  audit (tests/LayeredWhiteFurnaceTest.cpp config 2's note);
//  CharlieSheen::V has been the Λ-based Estevez & Kulla form since
//  that fix landed in 2026-05.  Corrected per
//  docs/CLOTH_FABRIC_DESIGN.md §2 debt 4.
//
//  Math (using α = roughness clamped to [1e-3, 1]):
//
//    sin²θ_h = 1 - (n·h)²
//    D_charlie(α, n·h) = (2 + 1/α) / (2π) · sin(θ_h)^(1/α)
//    Λ_charlie(α, x)   = a/(1 + b·x^c) + d·x + e   (Tab. 1 fit)
//    V_charlie(α, n·l, n·v)
//        = 1 / ((1 + Λ(α, n·l) + Λ(α, n·v)) · 4·n·l·n·v)
//    f_sheen(l, v) = sheenColor · D · V
//
//  See CharlieSheen.h for the coefficients and the C¹ mid-point
//  reflection at x = 0.5; it is the single source of truth shared
//  with SheenSPF and with FabricBRDF.
//
//  No diffuse lobe and no Fresnel on the sheen layer itself --
//  glTF's spec treats sheen as a colour-tinted grazing addition.
//
//  ENERGY: bounded above by 1 but NOT compensated -- this lobe
//  dissipates, and nothing here subtracts its energy from a base
//  layer underneath.  `fabric_material` (FabricBRDF.h) is the
//  compensated form, using the baked directional-albedo table in
//  SheenDirectionalAlbedo.h.  Note also that this material's own
//  roughness floor is 1e-3, where fabric_material's is 0.04: below
//  ~0.035 the lobe's directional albedo EXCEEDS 1 near grazing, so
//  1e-3 is safe for a standalone lobe (which compensates nothing)
//  and would not be safe for a compensator.  Left as-is
//  deliberately -- raising it would move every existing sheen
//  render.
//
//  References:
//    - Estevez & Kulla, "Production Friendly Microfacet Sheen BRDF",
//      Imageworks SIGGRAPH 2017.
//    - Khronos KHR_materials_sheen extension spec.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHEEN_BRDF_
#define SHEEN_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SheenBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		protected:
			virtual ~SheenBRDF();

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pColor;
			const IScalarPainter*	pRoughness;		// physical scalar

		public:
			SheenBRDF(
				const IPainter& sheenColor,
				const IScalarPainter& sheenRoughness
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetColor()     const { return *pColor; }
			inline const IScalarPainter& GetRoughness() const { return *pRoughness; }
			void SetColor( const IPainter& v );
			void SetRoughness( const IScalarPainter& v );
		};
	}
}

#endif
