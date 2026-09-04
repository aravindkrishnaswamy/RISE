//////////////////////////////////////////////////////////////////////
//
//  FabricMaterial.h - `fabric_material`: an energy-compensated
//    Charlie sheen lobe over a RESTRICTED substrate, with the weave
//    direction delivered as a rotation of the frame the SUBSTRATE is
//    evaluated in.
//
//    docs/CLOTH_FABRIC_DESIGN.md Phase 1 (9.2 - 9.6).
//
//  The triad is FabricMaterial + FabricBRDF + FabricSPF; the preset
//  table lives in FabricPresets.h.  Read FabricBRDF.h for the model and
//  the roughness floor, FabricSPF.h for the sampling.
//
//  WHY THIS EXISTS WHEN `sheen_material` ALREADY DOES.  `sheen_material`
//  is the bare lobe, intended to be stacked with `composite_material`.
//  That stack has two defects this material closes: `composite_material`
//  forwards ONE sub-material's BSDF, so NEE / BDPT / VCM connections see
//  a bare substrate; and nothing anywhere subtracts the sheen's energy
//  from the base, so a sheen-over-diffuse composite returns more light
//  than it receives at grazing.  `fabric_material` answers with the
//  COMBINED closed form and applies glTF's albedo-scaling law using a
//  baked directional-albedo table.
//
//  THE SUBSTRATE ALLOWLIST (9.2).
//
//  `base` is NOT "any IMaterial".  It is exactly FOUR C++ types --
//
//    LambertianMaterial | OrenNayarMaterial | GGXMaterial |
//    WeaveMaterial
//
//  -- and FIVE user-facing scene keywords, because
//  `pbr_metallic_roughness_material` is NOT its own material class: it
//  is resolved at scene-build time in
//  `Job::AddPBRMetallicRoughnessMaterial` into a painter graph plus a
//  single `ggx_material` in eFresnelSchlickF0 mode
//  (docs/MATERIALS.md 8), so by the time `IsSupportedSubstrate` runs it
//  IS a GGXMaterial and passes TRANSITIVELY.  Do not add another cast
//  target for it; there is no such class.  That transitive route is
//  what lets a glTF `KHR_materials_sheen` asset land on a PBR base.
//
//  `weave_material` JOINED THE LIST IN PHASE 2, and the composition it
//  enables -- an isotropic fuzz layer over a structured weave -- is the
//  PHYSICAL stack rather than a convenience: surface fuzz is loose
//  fibre ends standing off the woven cloth underneath.  It qualifies on
//  both of the criteria below without an exception: `WeaveBRDF`
//  implements `hemisphericalAlbedo` (an upper-bound closed form, whose
//  exactness class that file states), and `WeaveSPF` emits at most one
//  ray per `Scatter` call.  This is also what makes 9.3's split table
//  finally close for silk, satin and denim: the "verb-supplied" column
//  can now name a substrate that actually carries the weave.
//
//  Two independent reasons for the restriction, both inherited from
//  `coated_material`:
//
//    Correctness -- `hemisphericalAlbedo` (FabricBRDF.cpp, route 1)
//    consumes the substrate's OWN `IBSDF::hemisphericalAlbedo`.  A
//    luminaire, a BSSRDF or a volumetric random walk cannot supply one.
//
//    Sampling -- FabricSPF's single-sample estimator is well-posed
//    because every allowlisted substrate emits AT MOST ONE ray per
//    Scatter call.
//
//  An emissive substrate is refused even when its scattering class is
//  on the list: the sheen lobe would attenuate the substrate's
//  scattering while leaving its emission untouched, which is not a
//  modelled configuration.
//
//  ERROR vs WARNING -- TWO DIFFERENT DIAGNOSTICS, DO NOT CONFLATE.
//
//    * A base outside the allowlist is an ERROR (refused, here and at
//      the API), because the BSDF cannot evaluate it.
//    * A base that is allowlisted but is NOT the class the chosen
//      `fabric` preset was calibrated for is a WARNING, emitted by
//      `Job::AddFabricMaterial` where the runtime class is known.  The
//      composition is LEGAL and may be deliberate -- `fabric satin`
//      over a Lambertian is chalk with a faint sheen, which is a real
//      thing an author might want -- it just is not satin.  9.3 states
//      the split; `make_fabric` (9.7) is what closes the gap by minting
//      a matching substrate.
//
//  NO GetSpecularInfo OVERRIDE, DELIBERATELY -- AND NOTHING TO FORWARD.
//  The Charlie lobe is cosine-sampled with a full-hemisphere density,
//  and no allowlisted substrate OVERRIDES `ISPF::GetSpecularInfo`
//  either: `WeaveSPF` explicitly declines to report its `transmission
//  thin` gap lobe there (WeaveBRDF.h section 2a, "WHAT P2-B DOES NOT
//  DO") because an undeviated pass-through is not a refractive boundary
//  a specular-manifold chain would bend through.  So the ISPF default is
//  the substrate's answer AND the wrapper's, and adding an override here
//  could only invent information neither layer has.  What DOES have to
//  survive the wrapper -- and does, see `FabricSPF::ScatterImpl` -- is
//  the per-ray `ScatteredRay::isDelta` flag on that gap sample, which is
//  what PT / BDPT / VCM actually read to keep a delta lobe out of the
//  MIS density.  SMS therefore ignores fabric entirely, which is correct
//  and matches hair's precedent.  `SpecularInfo` also carries
//  `canRefract` + `ior`, which the SMS solver and IOR-stack seeding read
//  as "rays cross a refractive boundary here" -- nothing crosses a
//  boundary in this material, so reporting one would be a lie to those
//  subsystems.
//
//  THE TWO FULL-SPHERE FLAGS ARE FORWARDED FROM THE SUBSTRATE (R8 P1.1,
//  docs/CLOTH_FABRIC_DESIGN.md 15 debt 22).  `ScattersFullSphere()` and
//  `CouldLightPassThrough()` both return the BASE's answer, because this
//  material's transmission IS the base's transmission -- modulated by
//  the fuzz layer's two-crossing attenuation (FabricBRDF.h's
//  transmission section), never created or destroyed by it.  For every
//  Phase-1 substrate and for a `transmission none` weave the base
//  answers false, so this is the committed Phase-1 behaviour unchanged;
//  for a `transmission thin` weave it is what stops the wrapper from
//  silently extinguishing a sheer curtain.
//
//  Both flags are honest rather than merely permissive.
//  `ScattersFullSphere()` is a promise that `value()` is genuinely
//  non-zero below the horizon -- claiming it wrongly would have
//  `LightSampler` light back faces at full weight (IMaterial.h says so)
//  -- and with the forwarding in place `FabricBRDF::value()` returns
//  `f_base * scale` there, which is nonzero exactly when the base's is.
//  `CouldLightPassThrough()` reaches `AutoRasterizer`'s Tier-1
//  transmissive-material signal and the GUI's x-ray view; both should
//  see a sheen-wrapped sheer curtain the same way they see the bare one.
//
//  `IsVolumetric` DELIBERATELY STAYS FALSE AND IS *NOT* FORWARDED.  It
//  means something the other two do not: "BDPT must use `kray` for
//  throughput instead of BSDF*cos/pdf, because the SPF's kray carries
//  weighting the BSDF cannot reproduce" (IMaterial.h).  `FabricSPF`
//  OVERWRITES every emitted ray's kray with `f_fabric * cos / q` -- the
//  BSDF-derived quantity -- so fabric's kray is precisely what BDPT
//  would compute itself, and claiming volumetric transport would be a
//  false statement about this material's estimator.  No allowlisted
//  substrate reports it either, so forwarding would be a no-op today
//  AND wrong the moment one did.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FABRIC_MATERIAL_
#define FABRIC_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "FabricBRDF.h"
#include "FabricSPF.h"
#include "FabricPresets.h"
#include "LambertianMaterial.h"
#include "OrenNayarMaterial.h"
#include "GGXMaterial.h"
#include "WeaveMaterial.h"

namespace RISE
{
	namespace Implementation
	{
		class FabricMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			const IMaterial*	pBase;
			FabricBRDF*			pBRDF;
			FabricSPF*			pSPF;

			virtual ~FabricMaterial()
			{
				safe_release( pBRDF );
				safe_release( pSPF );
				safe_release( pBase );
			}

		public:
			//! Human-readable allowlist, for diagnostics.  Single source
			//! of truth so the parser message, the API refusal and this
			//! header can never drift -- CoatedMaterial's idiom.
			static const char* SubstrateAllowlistText()
			{
				return "lambertian_material, orennayar_material, ggx_material, "
				       "pbr_metallic_roughness_material, weave_material";
			}

			//! Allowlist predicate (9.2).  On refusal, `reason` (when
			//! non-null) receives a short phrase naming WHY, so callers
			//! can build a message that distinguishes "wrong scattering
			//! class" from "emits".
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
				    dynamic_cast<const WeaveMaterial*>( &base ) ) {
					return true;
				}
				if( reason ) *reason = "the substrate is not one of the supported scattering classes";
				return false;
			}

			//! The SCENE-LANGUAGE keyword naming `base`'s runtime class,
			//! for diagnostics.  Returns the keyword an author would
			//! have typed -- not the C++ class name -- and "an
			//! unsupported material" for anything off the allowlist.
			//!
			//! `pbr_metallic_roughness_material` is INDISTINGUISHABLE
			//! here: Job::AddPBRMetallicRoughnessMaterial resolves it
			//! into a plain GGXMaterial at scene-build time, so a PBR
			//! base honestly reports `ggx_material`.  Saying so in the
			//! message is better than pretending otherwise, since the
			//! two are the same object by then.
			static const char* SubstrateClassText( const IMaterial& base )
			{
				if( dynamic_cast<const LambertianMaterial*>( &base ) ) return "lambertian_material";
				if( dynamic_cast<const OrenNayarMaterial*>( &base ) )  return "orennayar_material";
				if( dynamic_cast<const GGXMaterial*>( &base ) )        return "ggx_material (or a pbr_metallic_roughness_material, which resolves to one)";
				if( dynamic_cast<const WeaveMaterial*>( &base ) )      return "weave_material";
				return "an unsupported material";
			}

			//! Does `base`'s runtime class match what `preset`
			//! recommends?  TRUE for `custom` (which recommends
			//! nothing) and for any preset whose class matches.  Drives
			//! the WARN-level diagnostic only -- never a refusal.
			//!
			//! `pbr_metallic_roughness_material` matches the GGX
			//! recommendation transitively, for the same reason the
			//! allowlist admits it.
			static bool MatchesPresetSubstrate( const IMaterial& base, const FabricPreset& preset )
			{
				switch( preset.substrate ) {
					case eFabricSubstrateLambertian:
						return dynamic_cast<const LambertianMaterial*>( &base ) != 0;
					case eFabricSubstrateOrenNayar:
						return dynamic_cast<const OrenNayarMaterial*>( &base ) != 0;
					case eFabricSubstrateGGX:
						return dynamic_cast<const GGXMaterial*>( &base ) != 0;
					case eFabricSubstrateWeave:
						// A `weave_material` base satisfies a weave
						// recommendation and a GGX one does NOT -- even
						// though GGX is what Phase 1 recommended for
						// silk / satin / denim and remains a legal,
						// shipping composition.  The warning is the whole
						// point: `fabric silk` over a bare anisotropic
						// GGX is exactly the configuration 9.9 gate 9b
						// measured and found wanting (95-99 % of the
						// substrate's anisotropy survives, and it still
						// reads as brushed metal), so an author who still
						// has one should be told what the preset now
						// wants.  Nothing is refused.
						return dynamic_cast<const WeaveMaterial*>( &base ) != 0;
					case eFabricSubstrateAny:
					default:
						return true;
				}
			}

			FabricMaterial(
				const IMaterial& base,
				const IPainter& sheenColor,
				const IScalarPainter& sheenRoughness,
				const IScalarPainter& weaveRotation
				) :
			  pBase( &base )
			{
				pBase->addref();

				// The substrate's full-sphere capability is captured here
				// and handed to the BRDF, because `IBSDF` has no such
				// flag -- it lives on `IMaterial`, and this constructor
				// is the one place both are in hand.  Safe to capture
				// once: `base` is not rebindable on this material, and
				// the only substrate that can answer TRUE is a
				// `weave_material`, whose `transmission` enum is
				// explicitly not rebindable either (WeaveMaterial.h).
				pBRDF = new FabricBRDF( *base.GetBSDF(), sheenColor, sheenRoughness, weaveRotation,
				                        base.ScattersFullSphere() );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new FabricSPF( *pBRDF, *base.GetSPF() );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  Never NULL -- the
			///         combined closed form is the point of the material.
			inline IBSDF* GetBSDF() const { return pBRDF; }

			/// \return The SPF for this material.  Never NULL.
			inline ISPF* GetSPF() const { return pSPF; }

			/// \return NULL: fabric never emits (the allowlist refuses
			///         emissive substrates).
			inline IEmitter* GetEmitter() const { return 0; }

			//! R8 P1.1: the SUBSTRATE's answer, verbatim.  See the file
			//! header ("THE TWO FULL-SPHERE FLAGS ARE FORWARDED") for why
			//! this is honest rather than merely permissive, and why
			//! `IsVolumetric` is deliberately NOT forwarded alongside.
			//! False for every Phase-1 substrate and for a `transmission
			//! none` weave, so this is the committed behaviour unchanged
			//! on everything that shipped before 2026-09-04.
			inline bool ScattersFullSphere() const { return pBase->ScattersFullSphere(); }

			//! Same forwarding, same reason: a sheen layer neither opens
			//! nor closes an aperture the substrate does not have.
			inline bool CouldLightPassThrough() const { return pBase->CouldLightPassThrough(); }

			//! Read-back for the interactive editor / snapshot clone.
			inline const IMaterial&      GetBase()           const { return *pBase; }
			inline const IPainter&       GetSheenColor()     const { return pBRDF->GetSheenColor(); }
			inline const IScalarPainter& GetSheenRoughness() const { return pBRDF->GetSheenRoughness(); }
			inline const IScalarPainter& GetWeaveRotation()  const { return pBRDF->GetWeaveRotation(); }

			//! Rebind for the interactive editor.  Only the BRDF is
			//! touched -- FabricSPF reads every fabric parameter back
			//! through it (FabricSPF.h), so there is no second copy to
			//! keep in lockstep (contrast SheenMaterial / GGXMaterial,
			//! which must forward to BOTH their BRDF and their SPF).
			//!
			//! `base` is deliberately NOT rebindable: it is a MATERIAL,
			//! not a painter, so it has no MaterialSlotRef kind, and
			//! swapping it would have to re-run the substrate allowlist
			//! and rebuild both the BRDF and the SPF.  Re-author the
			//! chunk to change the substrate.  Same call as
			//! `coated_material`.
			inline void SetSheenColor( const IPainter& v )           { pBRDF->SetSheenColor( v ); }
			inline void SetSheenRoughness( const IScalarPainter& v ) { pBRDF->SetSheenRoughness( v ); }
			inline void SetWeaveRotation( const IScalarPainter& v )  { pBRDF->SetWeaveRotation( v ); }
		};
	}
}

#endif
