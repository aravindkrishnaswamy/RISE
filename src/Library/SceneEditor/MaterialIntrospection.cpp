//////////////////////////////////////////////////////////////////////
//
//  MaterialIntrospection.cpp - Read-only material introspection.
//
//    Each row is created via `MakeReadOnlyRow` — Phase 2 ships the
//    Materials category visible-but-read-only.  The path to making
//    rows editable is documented in MaterialIntrospection.h; it
//    needs per-material setter API additions across IBSDF / ISPF
//    that don't exist today.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MaterialIntrospection.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IPainterManager.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Materials/Material.h"   // NullMaterial — Job's default-registered "none" material
#include "../Materials/LambertianMaterial.h"
#include "../Materials/PolishedMaterial.h"
#include "../Materials/DielectricMaterial.h"
#include "../Materials/PerfectReflectorMaterial.h"
#include "../Materials/PerfectRefractorMaterial.h"
#include "../Materials/IsotropicPhongMaterial.h"
#include "../Materials/AshikminShirleyAnisotropicPhongMaterial.h"
#include "../Materials/OrenNayarMaterial.h"
#include "../Materials/SchlickMaterial.h"
#include "../Materials/CookTorranceMaterial.h"
#include "../Materials/GGXMaterial.h"
#include "../Materials/WardIsotropicGaussianMaterial.h"
#include "../Materials/WardAnisotropicEllipticalGaussianMaterial.h"
#include "../Materials/DataDrivenMaterial.h"
#include "../Materials/LambertianLuminaireMaterial.h"
#include "../Materials/PhongLuminaireMaterial.h"
#include "../Materials/BioSpecSkinMaterial.h"
#include "../Materials/SubSurfaceScatteringMaterial.h"
#include "../Materials/RandomWalkSSSMaterial.h"
#include "../Materials/CompositeMaterial.h"
#include "../Materials/TranslucentMaterial.h"
#include "../Materials/SheenMaterial.h"
#include "../Materials/DonnerJensenSkinBSSRDFMaterial.h"
#include "../Materials/GenericHumanTissueMaterial.h"
#include "../Utilities/RString.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace {

CameraProperty MakeReadOnlyRow( const char* name, const String& value, const char* description )
{
	CameraProperty p;
	p.name        = String( name );
	p.value       = value;
	p.description = String( description );
	p.kind        = ValueKind::String;
	p.editable    = false;
	return p;
}

// Reverse-lookup an IPainter pointer to its manager-registered name.
// Returns empty if not registered.  Cheap at panel-edit cadence;
// the painter manager iteration is bounded by registered painter
// count (typically small).
String FindPainterName( const IPainterManager* mgr, const IPainter* target )
{
	if( !mgr || !target ) return String();
	struct Cb : public IEnumCallback<const char*> {
		const IPainterManager* mgr;
		const IPainter*        target;
		String                 found;
		bool operator()( const char* const& name ) override {
			if( const_cast<IPainterManager*>(mgr)->GetItem( name ) == target ) {
				found = String( name );
				return false;
			}
			return true;
		}
	};
	Cb cb;
	cb.mgr    = mgr;
	cb.target = target;
	const_cast<IPainterManager*>( mgr )->EnumerateItemNames( cb );
	return cb.found;
}

}  // namespace

String MaterialIntrospection::GetTypeName( const IMaterial& material )
{
	// Order is widest-first so derived materials match before their
	// base types.  None of the listed materials inherit from each
	// other today (every one declares `public virtual IMaterial`),
	// but the dynamic_cast cascade pattern keeps the code future-
	// proof against a future MaterialBase refactor.
	//
	// Type-name ambiguity caveat: a few `Job::Add*Material` paths
	// don't construct a dedicated material class — instead they
	// compose an existing material with a painter graph.  Those
	// materials surface here with the name of the composed type:
	//   - `pbr_metallic_roughness` scene chunk → composes a
	//     GGXMaterial; surfaces as "GGX".
	//   - `ggx_emissive_material` → composes a GGXMaterial with an
	//     emission painter; surfaces as "GGX".
	// Distinguishing these from a hand-authored GGX requires reading
	// the painter graph's naming convention (out of scope for this
	// read-only Phase-2 panel).  Phase 4 can add the disambiguator
	// alongside the editable surface.
	// NullMaterial is the default `"none"` material `Job` registers
	// at construction.  It has no BSDF / SPF / emitter and is what
	// objects fall back to when their `material` chunk arg was
	// omitted.  Surface it explicitly so the user sees a meaningful
	// type instead of "(unknown type)".
	if( dynamic_cast<const NullMaterial*>( &material ) )                                return String( "None (default)" );
	if( dynamic_cast<const LambertianMaterial*>( &material ) )                          return String( "Lambertian" );
	if( dynamic_cast<const PolishedMaterial*>( &material ) )                            return String( "Polished" );
	if( dynamic_cast<const DielectricMaterial*>( &material ) )                          return String( "Dielectric" );
	if( dynamic_cast<const PerfectReflectorMaterial*>( &material ) )                    return String( "Perfect Reflector" );
	if( dynamic_cast<const PerfectRefractorMaterial*>( &material ) )                    return String( "Perfect Refractor" );
	if( dynamic_cast<const IsotropicPhongMaterial*>( &material ) )                      return String( "Isotropic Phong" );
	if( dynamic_cast<const AshikminShirleyAnisotropicPhongMaterial*>( &material ) )     return String( "Ashikmin-Shirley Anisotropic Phong" );
	if( dynamic_cast<const OrenNayarMaterial*>( &material ) )                           return String( "Oren-Nayar" );
	if( dynamic_cast<const SchlickMaterial*>( &material ) )                             return String( "Schlick" );
	if( dynamic_cast<const CookTorranceMaterial*>( &material ) )                        return String( "Cook-Torrance" );
	if( dynamic_cast<const GGXMaterial*>( &material ) )                                 return String( "GGX" );
	if( dynamic_cast<const WardIsotropicGaussianMaterial*>( &material ) )               return String( "Ward Isotropic" );
	if( dynamic_cast<const WardAnisotropicEllipticalGaussianMaterial*>( &material ) )   return String( "Ward Anisotropic" );
	if( dynamic_cast<const DataDrivenMaterial*>( &material ) )                          return String( "Data-Driven (MERL/IES)" );
	if( dynamic_cast<const LambertianLuminaireMaterial*>( &material ) )                 return String( "Lambertian Luminaire (emissive)" );
	if( dynamic_cast<const PhongLuminaireMaterial*>( &material ) )                      return String( "Phong Luminaire (emissive)" );
	if( dynamic_cast<const BioSpecSkinMaterial*>( &material ) )                         return String( "BioSpec Skin" );
	if( dynamic_cast<const SubSurfaceScatteringMaterial*>( &material ) )                return String( "SSS (Diffusion Profile)" );
	if( dynamic_cast<const RandomWalkSSSMaterial*>( &material ) )                       return String( "SSS (Random Walk)" );
	if( dynamic_cast<const CompositeMaterial*>( &material ) )                           return String( "Composite (layered)" );
	if( dynamic_cast<const TranslucentMaterial*>( &material ) )                         return String( "Translucent" );
	if( dynamic_cast<const SheenMaterial*>( &material ) )                               return String( "Sheen" );
	if( dynamic_cast<const DonnerJensenSkinBSSRDFMaterial*>( &material ) )              return String( "Donner-Jensen Skin BSSRDF" );
	if( dynamic_cast<const GenericHumanTissueMaterial*>( &material ) )                  return String( "Generic Human Tissue" );
	return String( "(unknown type)" );
}

std::vector<CameraProperty> MaterialIntrospection::Inspect(
	const String& name, const IMaterial& material, const IPainterManager* painters )
{
	std::vector<CameraProperty> rows;

	rows.push_back( MakeReadOnlyRow(
		"Name", name,
		"The material's manager-registered name (matches the chunk's first arg in the .RISEscene file)." ) );

	rows.push_back( MakeReadOnlyRow(
		"Type", GetTypeName( material ),
		"Concrete material kind.  Each kind has its own set of painter / scalar slots — full per-type editing arrives in a future Phase 4 PR." ) );

	// Per-type field surfacing.  Today only Lambertian exposes a
	// reverse-readable painter binding; other materials follow the
	// same pattern once their BRDF/SPF gain runtime reverse-getters
	// (and ideally setters for editable round-trip).
	if( const LambertianMaterial* lam = dynamic_cast<const LambertianMaterial*>( &material ) ) {
		const IPainter& refl = lam->GetReflectance();
		const String refName = FindPainterName( painters, &refl );
		rows.push_back( MakeReadOnlyRow(
			"reflectance", refName.size() > 1 ? refName : String( "(unregistered painter)" ),
			"Painter that drives the Lambertian reflectance (BRDF + SPF use the same instance).  "
			"Read-only in this Phase 2 surface; editing this binding requires per-material runtime "
			"setters that don't exist yet — see MaterialIntrospection.h for the Phase 4 roadmap." ) );
	}

	// Read-only volumetric / SSS indicators — useful diagnostic info
	// that costs nothing extra to surface.
	if( material.IsVolumetric() ) {
		rows.push_back( MakeReadOnlyRow(
			"volumetric", String( "yes" ),
			"This material has volumetric transport (SSS or similar).  BDPT uses kray-based throughput accordingly." ) );
	}
	if( material.GetDiffusionProfile() ) {
		rows.push_back( MakeReadOnlyRow(
			"diffusion_profile", String( "present" ),
			"This material has a BSSRDF diffusion profile.  Integrators perform importance-sampled probe-ray casting." ) );
	}
	if( const RandomWalkSSSParams* rw = material.GetRandomWalkSSSParams() ) {
		(void)rw;
		rows.push_back( MakeReadOnlyRow(
			"random_walk_sss", String( "present" ),
			"This material uses random-walk subsurface scattering instead of disk-projection sampling." ) );
	}
	if( material.CouldLightPassThrough() ) {
		rows.push_back( MakeReadOnlyRow(
			"transmissive", String( "yes" ),
			"Light can pass through this material (glass or other transmissive medium)." ) );
	}

	return rows;
}
