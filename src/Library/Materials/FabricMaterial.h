//////////////////////////////////////////////////////////////////////
//
//  FabricMaterial.h - `fabric_material`: energy-compensated Charlie
//  sheen over a restricted substrate, with `weave_rotation` rotating
//  the substrate frame (docs/CLOTH_FABRIC_DESIGN.md §9.2-9.6).
//
//  Triad: FabricMaterial + FabricBRDF + FabricSPF (presets in
//  FabricPresets.h). Unlike `sheen_material` + `composite_material`,
//  exposes the combined energy-conserving BSDF to NEE/BDPT/VCM.
//
//  Load-bearing invariants:
//  - Substrate allowlist: LambertianMaterial, OrenNayarMaterial,
//    GGXMaterial (including pbr_metallic_roughness_material, which
//    resolves to GGXMaterial at scene-build time), and WeaveMaterial.
//    Emissive substrates are refused. Substrates must supply
//    IBSDF::hemisphericalAlbedo and emit <= 1 ray per Scatter.
//    Unsupported base is an ERROR; preset/substrate mismatch is a
//    WARNING only (Job::AddFabricMaterial).
//  - Capability forwarding: ScattersFullSphere(), CouldLightPassThrough(),
//    TracksInteriorContainment(), and HasDeltaPassThrough() forward to
//    pBase (safe to capture once in FabricBRDF since `base` and weave
//    `transmission` are non-rebindable). IsVolumetric stays FALSE (not
//    forwarded: FabricSPF computes BSDF-consistent kray).
//  - No GetSpecularInfo override: neither Charlie sheen nor any allowlisted
//    substrate reports refractive specular manifolds; SMS ignores fabric.
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

			//! Scene-language keyword for `base`'s runtime class (for diagnostics).
			//! `pbr_metallic_roughness_material` resolves to `GGXMaterial` at
			//! scene-build time, so it reports `ggx_material`.
			static const char* SubstrateClassText( const IMaterial& base )
			{
				if( dynamic_cast<const LambertianMaterial*>( &base ) ) return "lambertian_material";
				if( dynamic_cast<const OrenNayarMaterial*>( &base ) )  return "orennayar_material";
				if( dynamic_cast<const GGXMaterial*>( &base ) )        return "ggx_material (or a pbr_metallic_roughness_material, which resolves to one)";
				if( dynamic_cast<const WeaveMaterial*>( &base ) )      return "weave_material";
				return "an unsupported material";
			}

			//! Returns true if `base`'s runtime class matches `preset`'s
			//! recommended substrate (drives warning-only diagnostics; `custom`
			//! matches any, and PBR bases match GGX transitively).
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
						// Weave preset expects WeaveMaterial (not bare GGX); warns only.
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

				// Capture ScattersFullSphere() once here (IBSDF lacks this flag;
				// neither `base` nor a weave's `transmission` enum is rebindable).
				pBRDF = new FabricBRDF( *base.GetBSDF(), sheenColor, sheenRoughness, weaveRotation,
				                        base.ScattersFullSphere(),
				                        base.ConnectionScatterTypes() == (1u << ScatteredRay::eRayDiffuse) );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new FabricSPF( *pBRDF, *base.GetSPF() );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  Never NULL -- the
			///         combined closed form is the point of the material.
			inline IBSDF* GetBSDF() const { return pBRDF; }

			/// \return The SPF for this material.  Never NULL.
			inline ISPF* GetSPF() const { return pSPF; }

            unsigned int ConnectionScatterTypes() const {
                return pBRDF->SingleDiffuseLabel() ? (1u << ScatteredRay::eRayDiffuse)
                    : IMaterial::ConnectionScatterTypes();
            }
            bool HasConnectionTypeSplit() const { return pBRDF->SingleDiffuseLabel(); }

			/// \return NULL: fabric never emits (the allowlist refuses
			///         emissive substrates).
			inline IEmitter* GetEmitter() const { return 0; }

			//! Forwarded from pBase (true only for a `transmission thin` weave);
			//! IsVolumetric stays false because FabricSPF emits BSDF-consistent kray.
			inline bool ScattersFullSphere() const { return pBase->ScattersFullSphere(); }

			//! Same forwarding, same reason: a sheen layer neither opens
			//! nor closes an aperture the substrate does not have.
			inline bool CouldLightPassThrough() const { return pBase->CouldLightPassThrough(); }
			inline bool TracksInteriorContainment() const { return pBase->TracksInteriorContainment(); }

			//! DL-05: FabricSPF re-prices (never removes) the substrate's
			//! delta pass-through, so the capability is the substrate's;
			//! FabricSPF::DeltaPassThroughTransmittance supplies the
			//! re-priced value.
			inline bool HasDeltaPassThrough() const { return pBase->HasDeltaPassThrough(); }

			//! Read-back for the interactive editor / snapshot clone.
			inline const IMaterial&      GetBase()           const { return *pBase; }
			inline const IPainter&       GetSheenColor()     const { return pBRDF->GetSheenColor(); }
			inline const IScalarPainter& GetSheenRoughness() const { return pBRDF->GetSheenRoughness(); }
			inline const IScalarPainter& GetWeaveRotation()  const { return pBRDF->GetWeaveRotation(); }

			//! Rebinds painter slots on pBRDF only (FabricSPF reads shared state
			//! from pBRDF). `base` is not rebindable; re-author the chunk instead.
			inline void SetSheenColor( const IPainter& v )           { pBRDF->SetSheenColor( v ); }
			inline void SetSheenRoughness( const IScalarPainter& v ) { pBRDF->SetSheenRoughness( v ); }
			inline void SetWeaveRotation( const IScalarPainter& v )  { pBRDF->SetWeaveRotation( v ); }

			unsigned int GetPathDependence() const
			{
				return IMaterial::GetPathDependence() |
					( pBase ? pBase->GetPathDependence() : 0u ) |
					pBRDF->GetSheenColor().GetPathDependence() |
					pBRDF->GetSheenRoughness().GetPathDependence() |
					pBRDF->GetWeaveRotation().GetPathDependence();
			}
		};
	}
}

#endif
