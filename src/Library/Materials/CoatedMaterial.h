//////////////////////////////////////////////////////////////////////
//
//  CoatedMaterial.h - `coated_material`: a transparent dielectric
//    film over a RESTRICTED substrate, with the film's coverage as a
//    first-class spatially varying slot.
//
//    docs/WETNESS_COAT_DESIGN.md Phase 2 (7.1 - 7.6).
//
//  The triad is CoatedMaterial + CoatedBRDF + CoatedSPF; the layer
//  algebra lives in CoatedLayer.h.  Read CoatedBRDF.h for the model
//  and CoatedSPF.h for the sampling.
//
//  THE SUBSTRATE ALLOWLIST (7.2 / Phase 2 item 4a).
//
//  `base` is NOT "any IMaterial", for two independent reasons the
//  design doc states:
//
//    Correctness -- the layered model runs a recycling series driven
//    by the substrate's DIRECTIONAL ALBEDO.  A diffuse or microfacet
//    base can supply that (`IBSDF::hemisphericalAlbedo{,NM}`); a
//    luminaire,
//    a BSSRDF, or a volumetric random walk cannot, and would silently
//    render as something physically meaningless.
//
//    Budget -- 7.5's lobe arithmetic only closes because the
//    substrate's lobe count is known.
//
//  v1 allowlist, exactly as 7.2 recommends:
//
//    lambertian_material
//    orennayar_material
//    ggx_material
//    pbr_metallic_roughness_material
//
//  The last one needs no separate case: `Job::AddPBRMetallicRoughness-
//  Material` resolves a glTF metallic-roughness material into a
//  painter graph plus a single `ggx_material` in eFresnelSchlickF0
//  mode at scene-build time (docs/MATERIALS.md 8), so by the time it
//  reaches here it IS a GGXMaterial and is admitted by that case.
//
//  `fabric_material` and `weave_material` JOINED THE LIST via DL-23
//  (docs/DEBT_LEDGER.md, docs/CLOTH_FABRIC_DESIGN.md 15 debt 22's own
//  "sibling wrapper" note) -- a coated fabric (waxed canvas; a
//  varnished basket weave) is a real, physical composition, and both
//  satisfy the two criteria above: `FabricBRDF`/`WeaveBRDF` implement
//  `hemisphericalAlbedo`, and `FabricSPF`/`WeaveSPF` emit at most one
//  ray per `Scatter` call.  `weave_material`'s `transmission thin` mode
//  additionally carries BELOW-HORIZON transport (a delta gap
//  pass-through plus a Lambertian back-face lobe), which
//  `CoatedBRDF`/`CoatedSPF` now forward and modulate exactly as
//  `FabricBRDF`/`FabricSPF` do for the identical case -- see
//  CoatedBRDF.h's "TRANSMISSION THROUGH THE COAT" section.
//
//  Anything else is REFUSED at parse time with a message naming the
//  allowlist, rather than rendering something quietly wrong.  A
//  material that emits is refused even when its scattering class is on
//  the list (an emissive GGX, a lambertian_luminaire): coating a
//  luminaire is not a modelled configuration, and the coat would
//  attenuate its scattering while leaving its emission untouched.
//
//  NO GetSpecularInfo OVERRIDE, DELIBERATELY.
//
//  7.1 asks for `GetSpecularInfo` / `GetSpecularInfoNM` "if the coat
//  lobe can go delta".  It cannot: CoatedBRDF floors the coat's GGX
//  alpha at CoatedLayer::kMinCoatAlpha, so every lobe this material
//  produces has a real sampling density.  That is a design decision,
//  not an omission, and it is aligned with Phase 2's whole purpose --
//  the material exists so NEE / BDPT / VCM connections can SEE the
//  coated surface (7.1's "single most important requirement"), and a
//  delta lobe is invisible to all three.  7.2's parameter ranges
//  (0.01-0.05 for water, 0.03-0.1 for clearcoat) never ask for one.
//
//  A second reason to leave the default in place: `SpecularInfo` also
//  carries `canRefract` + `ior`, which the SMS solver and the
//  IOR-stack seeding read as "rays cross a refractive boundary here".
//  Nothing crosses a boundary in this material -- the coat's
//  transmission is folded into the substrate lobe's throughput
//  analytically (7.5) and no ray ever enters the coat medium.
//  Reporting an interface IOR would be a lie to those subsystems.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COATED_MATERIAL_
#define COATED_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "CoatedBRDF.h"
#include "CoatedSPF.h"
#include "LambertianMaterial.h"
#include "OrenNayarMaterial.h"
#include "GGXMaterial.h"
#include "FabricMaterial.h"
#include "WeaveMaterial.h"

namespace RISE
{
	namespace Implementation
	{
		class CoatedMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			const IMaterial*	pBase;
			CoatedBRDF*			pBRDF;
			CoatedSPF*			pSPF;

			virtual ~CoatedMaterial()
			{
				safe_release( pBRDF );
				safe_release( pSPF );
				safe_release( pBase );
			}

		public:
			//! Human-readable allowlist, for diagnostics.  Single
			//! source of truth so the parser message, the API refusal
			//! and this header can never drift.
			//!
			//! DL-23 (docs/DEBT_LEDGER.md) added `fabric_material` and
			//! `weave_material` -- the same two criteria this header's
			//! banner states (a substrate `hemisphericalAlbedo` the
			//! recycling series can consume; a known per-Scatter-call
			//! lobe budget) are exactly the ones `FabricMaterial.h`
			//! states for its OWN allowlist admitting `weave_material`,
			//! and `FabricBRDF`/`FabricSPF` are single-sample estimators
			//! for the identical reason `CoatedBRDF`/`CoatedSPF` are.
			//! `coated_material over fabric_material` is the design's
			//! own named example (waxed canvas); `coated_material over
			//! weave_material` reaches a `transmission thin` weave
			//! directly (a waxed sheer curtain).
			static const char* SubstrateAllowlistText()
			{
				return "lambertian_material, orennayar_material, ggx_material, "
				       "pbr_metallic_roughness_material, fabric_material, weave_material";
			}

			//! Allowlist predicate (7.2 / Phase 2 item 4a).  On
			//! refusal, `reason` (when non-null) receives a short
			//! phrase naming WHY, so callers can build a message that
			//! distinguishes "wrong scattering class" from "emits".
			static bool IsSupportedSubstrate( const IMaterial& base, const char** reason = 0 )
			{
				if( base.GetEmitter() != 0 ) {
					if( reason ) *reason = "the substrate is a luminaire (it has an emitter)";
					return false;
				}
				if( base.GetBSDF() == 0 || base.GetSPF() == 0 ) {
					if( reason ) *reason = "the substrate has no BSDF and/or no SPF";
					return false;
				}
				if( dynamic_cast<const LambertianMaterial*>( &base ) ||
				    dynamic_cast<const OrenNayarMaterial*>( &base ) ||
				    dynamic_cast<const GGXMaterial*>( &base ) ||
				    dynamic_cast<const FabricMaterial*>( &base ) ||
				    dynamic_cast<const WeaveMaterial*>( &base ) ) {
					return true;
				}
				if( reason ) *reason = "the substrate is not one of the supported scattering classes";
				return false;
			}

			//! @param recyclingCompensation  ALWAYS true in production.
			//!        FALSE strips docs/WETNESS_COAT_DESIGN.md 7.4's
			//!        required interreflection term, reducing the layer
			//!        to plain Weidlich-Wilkie; it exists solely so
			//!        tests/LayeredWhiteFurnaceTest.cpp can measure the
			//!        ~45 % high-albedo loss that omitting it causes.
			//!        Not reachable from the scene language or the API.
			CoatedMaterial(
				const IMaterial& base,
				const IScalarPainter& coatWeight,
				const IScalarPainter& coatIOR,
				const IScalarPainter& coatRoughness,
				const IScalarPainter& coatThickness,
				const IScalarPainter& coatAbsorption,
				const IPainter& coatTint,
				const bool recyclingCompensation = true,
				//! [in] DL-192: optional coat-lobe-only tangent-space
				//! normal map (Blender Principled BSDF's "Coat Normal"
				//! socket).  NULL (default) is bit-identical to a
				//! pre-DL-192 CoatedMaterial -- see CoatedBRDF.h's
				//! matching ctor parameter for the full contract.
				const IPainter* coatNormal = 0,
				const Scalar coatNormalScale = Scalar(1)
				) :
			  pBase( &base )
			{
				pBase->addref();

				// DL-23: `base.ScattersFullSphere()` is captured here,
				// mirroring FabricMaterial's identical constructor-time
				// capture -- `IBSDF` carries no such flag (it lives on
				// `IMaterial`), and this constructor is the one place
				// both are in hand.  Safe to capture once: `base` is not
				// rebindable on this material, and the only substrate
				// that can answer TRUE is a `weave_material`, whose
				// `transmission` enum is itself not rebindable.
				pBRDF = new CoatedBRDF(
					*base.GetBSDF(), coatWeight, coatIOR, coatRoughness,
					coatThickness, coatAbsorption, coatTint, recyclingCompensation,
					base.ScattersFullSphere(), coatNormal, coatNormalScale );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new CoatedSPF( *pBRDF, *base.GetSPF() );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  Never NULL -- the
			///         whole point of the material (7.1).
			inline IBSDF* GetBSDF() const { return pBRDF; }

			/// \return The SPF for this material.  Never NULL.
			inline ISPF* GetSPF() const { return pSPF; }

			//! DL-23: the SUBSTRATE's answer, verbatim -- mirrors
			//! FabricMaterial's identical forwarding (docs/CLOTH_
			//! FABRIC_DESIGN.md 15 debt 22/R8 P1.1).  A coat layer
			//! neither opens nor closes an aperture the substrate does
			//! not have; it only attenuates whatever passes through it.
			//! False for every pre-DL-23 substrate and for a
			//! `transmission none` weave, so this is the committed
			//! behaviour unchanged on everything that shipped before.
			inline bool ScattersFullSphere() const { return pBase->ScattersFullSphere(); }

			//! Same forwarding, same reason.
			inline bool CouldLightPassThrough() const { return pBase->CouldLightPassThrough(); }

			//! DL-05: CoatedSPF re-prices the substrate's delta
			//! pass-through by the coat's two crossings; the capability is
			//! the substrate's (CoatedSPF::DeltaPassThroughTransmittance).
			inline bool HasDeltaPassThrough() const { return pBase->HasDeltaPassThrough(); }

			/// \return NULL: a coated surface never emits (the
			///         allowlist refuses emissive substrates).
			inline IEmitter* GetEmitter() const { return 0; }

			//! Read-back for the interactive editor / snapshot clone.
			inline const IMaterial&      GetBase()           const { return *pBase; }
			inline const IScalarPainter& GetCoatWeight()     const { return pBRDF->GetCoatWeight(); }
			inline const IScalarPainter& GetCoatIOR()        const { return pBRDF->GetCoatIOR(); }
			inline const IScalarPainter& GetCoatRoughness()  const { return pBRDF->GetCoatRoughness(); }
			inline const IScalarPainter& GetCoatThickness()  const { return pBRDF->GetCoatThickness(); }
			inline const IScalarPainter& GetCoatAbsorption() const { return pBRDF->GetCoatAbsorption(); }
			inline const IPainter&       GetCoatTint()       const { return pBRDF->GetCoatTint(); }
			//! DL-192.  NULL = no coat-normal perturbation bound.
			inline const IPainter*       GetCoatNormal()      const { return pBRDF->GetCoatNormal(); }
			inline Scalar                GetCoatNormalScale() const { return pBRDF->GetCoatNormalScale(); }

			//! Rebind for the interactive editor.  Only the BRDF is
			//! touched -- CoatedSPF reads every coat parameter back
			//! through it, so there is no second copy to keep in
			//! lockstep (contrast GGXMaterial, which must forward to BOTH
			//! its BRDF and its SPF; PolishedMaterial shares one painter
			//! set the same way since DL-285).
			//!
			//! `base` is deliberately NOT rebindable: it is a MATERIAL,
			//! not a painter, so it has no MaterialSlotRef kind, and
			//! swapping it would have to re-run the substrate allowlist
			//! and rebuild both the BRDF and the SPF.  Re-author the
			//! chunk to change the substrate.
			inline void SetCoatWeight( const IScalarPainter& v )     { pBRDF->SetCoatWeight( v ); }
			inline void SetCoatIOR( const IScalarPainter& v )        { pBRDF->SetCoatIOR( v ); }
			inline void SetCoatRoughness( const IScalarPainter& v )  { pBRDF->SetCoatRoughness( v ); }
			inline void SetCoatThickness( const IScalarPainter& v )  { pBRDF->SetCoatThickness( v ); }
			inline void SetCoatAbsorption( const IScalarPainter& v ) { pBRDF->SetCoatAbsorption( v ); }
			inline void SetCoatTint( const IPainter& v )             { pBRDF->SetCoatTint( v ); }
		};
	}
}

#endif
