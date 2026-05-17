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
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IScalarPainterManager.h"
#include "../Interfaces/IJob.h"
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

// Same as `FindPainterName` but for IScalarPainter — the physical-
// scalar pipe described in `docs/ISCALARPAINTER_REFACTOR.md`.  The
// two managers are distinct (IPainter vs IScalarPainter) so the
// reverse-lookup needs its own helper rather than a templatized
// generic — IManager<T>::GetItem is not a static cast point.
//
// `[[maybe_unused]]` because Stage 4A only surfaces the Lambertian
// painter binding (IPainter); the IScalarPainter-using callers
// land in Stage 4B per-material rollout.  Marking explicit so a
// clean Stage-4A build doesn't trip `-Wunused-function`.
[[maybe_unused]]
String FindScalarPainterName( const IScalarPainterManager* mgr, const IScalarPainter* target )
{
	if( !mgr || !target ) return String();
	struct Cb : public IEnumCallback<const char*> {
		const IScalarPainterManager* mgr;
		const IScalarPainter*        target;
		String                       found;
		bool operator()( const char* const& name ) override {
			if( const_cast<IScalarPainterManager*>(mgr)->GetItem( name ) == target ) {
				found = String( name );
				return false;
			}
			return true;
		}
	};
	Cb cb;
	cb.mgr    = mgr;
	cb.target = target;
	const_cast<IScalarPainterManager*>( mgr )->EnumerateItemNames( cb );
	return cb.found;
}

// Build the {label, value} preset list for a painter slot from the
// passed-in manager.  Returns an empty vector when the manager is
// null (caller already gated on availability).  Caller adds a
// synthetic "(unbound)" entry if/where applicable.
template <class MgrT>
std::vector<ParameterPreset> CollectPainterPresets( const MgrT* mgr )
{
	std::vector<ParameterPreset> out;
	if( !mgr ) return out;
	struct Cb : public IEnumCallback<const char*> {
		std::vector<ParameterPreset>* out;
		bool operator()( const char* const& name ) override {
			if( name ) {
				ParameterPreset p;
				p.label = std::string( name );
				p.value = std::string( name );
				out->push_back( p );
			}
			return true;
		}
	};
	Cb cb;
	cb.out = &out;
	const_cast<MgrT*>( mgr )->EnumerateItemNames( cb );
	return out;
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

namespace {

// Build one painter-slot row for the panel.  Caller has already
// reverse-looked-up the binding's name; `nameResolved` is true iff
// that lookup found a registered name.
//
// Important: a slot whose current binding is NOT registered with
// the matching manager is surfaced as READ-ONLY even when
// `panelEditable` would otherwise allow the edit.  An unregistered
// painter has no recoverable name, so the SceneEdit's
// `prevPropertyValue` capture would be empty, and an Undo would
// silently no-op.  Better to refuse the edit up-front than to let
// the user rebind and then discover undo doesn't restore.
CameraProperty MakePainterSlotRow(
	const char* slotName, const String& binding, bool nameResolved,
	const std::vector<ParameterPreset>& presets,
	bool panelEditable, const char* description )
{
	CameraProperty p;
	p.name        = String( slotName );
	p.value       = binding;
	p.description = String( description );
	p.kind        = ValueKind::Reference;   // panel renders as text + presets dropdown
	p.editable    = panelEditable && nameResolved;
	p.presets     = presets;
	return p;
}

// Reverse-lookup + row-build for an IPainter slot.  Returns a
// read-only row when `painter` is unregistered with `painters`
// (no recoverable name = no undoable rebind).
CameraProperty BuildPainterSlot(
	const char* slot, const IPainter& painter,
	const IPainterManager* painters, bool composed,
	const char* desc )
{
	const String n = FindPainterName( painters, &painter );
	const bool nameResolved = n.size() > 1;
	return MakePainterSlotRow(
		slot,
		nameResolved ? n : String( "(unregistered painter)" ),
		nameResolved,
		CollectPainterPresets( painters ),
		!composed && painters != 0,
		desc );
}

// Same shape for an IScalarPainter slot.
CameraProperty BuildScalarPainterSlot(
	const char* slot, const IScalarPainter& painter,
	const IScalarPainterManager* scalarPainters, bool composed,
	const char* desc )
{
	const String n = FindScalarPainterName( scalarPainters, &painter );
	const bool nameResolved = n.size() > 1;
	return MakePainterSlotRow(
		slot,
		nameResolved ? n : String( "(unregistered scalar_painter)" ),
		nameResolved,
		CollectPainterPresets( scalarPainters ),
		!composed && scalarPainters != 0,
		desc );
}

}  // namespace

std::vector<CameraProperty> MaterialIntrospection::Inspect(
	const String& name, const IMaterial& material,
	const IPainterManager* painters,
	const IScalarPainterManager* scalarPainters,
	const IJob* job )
{
	std::vector<CameraProperty> rows;

	rows.push_back( MakeReadOnlyRow(
		"Name", name,
		"The material's manager-registered name (matches the chunk's first arg in the .RISEscene file)." ) );

	rows.push_back( MakeReadOnlyRow(
		"Type", GetTypeName( material ),
		"Concrete material kind.  Each kind has its own set of painter / scalar slots." ) );

	// Composed-material gate: PBR-MR and GGX-Emissive built a painter
	// graph internally; rebinding any slot would break the graph's
	// downstream computations.  Surface a clear note and mark every
	// slot row read-only.  Per-slot rows still display so the user
	// can SEE the bindings — they just can't edit.
	const bool composed = job && job->IsMaterialComposed( name.c_str() );
	if( composed ) {
		rows.push_back( MakeReadOnlyRow(
			"composed", String( "yes" ),
			"This material was registered via a composing factory "
			"(`pbr_metallic_roughness_material` or `ggx_emissive_material`) "
			"that built an internal painter graph.  Slot rows below are "
			"read-only — rebinding one would break the graph.  To change "
			"this material's appearance, edit the upstream painters directly." ) );
	}

	// Per-type field surfacing.  The pattern: for each known
	// material type, walk its painter slots, reverse-lookup each
	// binding's name, build a Reference-kind row with the matching
	// preset list, mark editable based on the composed gate.
	if( const LambertianMaterial* lam = dynamic_cast<const LambertianMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "reflectance", lam->GetReflectance(),
			painters, composed,
			"Painter that drives the Lambertian reflectance (BRDF + SPF share the same instance).  "
			"Picking a different painter from the dropdown rebinds the slot live." ) );
	}
	else if( const PerfectReflectorMaterial* pr = dynamic_cast<const PerfectReflectorMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "reflectance", pr->GetReflectance(),
			painters, composed,
			"Painter for the mirror's reflectance tint.  Multiplies SMS chain throughput "
			"so a coloured mirror passes its tint through caustic chains." ) );
	}
	else if( const PerfectRefractorMaterial* pf = dynamic_cast<const PerfectRefractorMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "refractivity", pf->GetRefractivity(),
			painters, composed,
			"Painter for per-wavelength refractive colour attenuation (Beer-Lambert-like tint applied to the transmitted ray)." ) );
		rows.push_back( BuildScalarPainterSlot( "ior", pf->GetIOR(),
			scalarPainters, composed,
			"Scalar painter for the index of refraction.  Use a spectral scalar painter for dispersion." ) );
	}
	else if( const PolishedMaterial* pol = dynamic_cast<const PolishedMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "diffuse_reflectance", pol->GetDiffuseReflectance(),
			painters, composed,
			"Painter for the substrate's diffuse reflectance (the Lambertian layer under the dielectric coat)." ) );
		rows.push_back( BuildScalarPainterSlot( "transmittance", pol->GetTransmittance(),
			scalarPainters, composed,
			"Scalar painter for the dielectric coat's transmittance — physical scalar, NOT a colour." ) );
		rows.push_back( BuildScalarPainterSlot( "ior", pol->GetIOR(),
			scalarPainters, composed,
			"Scalar painter for the dielectric coat's IOR." ) );
		rows.push_back( BuildScalarPainterSlot( "scattering", pol->GetScattering(),
			scalarPainters, composed,
			"Scalar painter for the coat's scattering function (Phong cone width or HG asymmetry depending on the material's `hg` flag)." ) );
	}
	else if( const DielectricMaterial* die = dynamic_cast<const DielectricMaterial*>( &material ) ) {
		rows.push_back( BuildScalarPainterSlot( "transmittance", die->GetTransmittance(),
			scalarPainters, composed,
			"Scalar painter for the dielectric's transmittance per channel — physical scalar pipe." ) );
		rows.push_back( BuildScalarPainterSlot( "ior", die->GetIOR(),
			scalarPainters, composed,
			"Scalar painter for the index of refraction.  Use a spectral scalar painter for dispersion." ) );
		rows.push_back( BuildScalarPainterSlot( "scattering", die->GetScattering(),
			scalarPainters, composed,
			"Scalar painter for the scattering function (Phong cone width or HG asymmetry per the material's `hg` flag)." ) );
	}
	else if( const GGXMaterial* ggx = dynamic_cast<const GGXMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "diffuse",  ggx->GetDiffuse(),
			painters, composed,
			"Painter for the diffuse (Lambertian) base lobe — IPainter colour pipe." ) );
		rows.push_back( BuildPainterSlot( "specular", ggx->GetSpecular(),
			painters, composed,
			"Painter for the specular tint OR Schlick F0 input depending on `fresnel_mode` "
			"(conductor mode = tint multiplier; schlick_f0 mode = F0 directly per glTF spec)." ) );
		rows.push_back( BuildScalarPainterSlot( "alphax",   ggx->GetAlphaX(),
			scalarPainters, composed,
			"Anisotropic roughness in the tangent direction — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "alphay",   ggx->GetAlphaY(),
			scalarPainters, composed,
			"Anisotropic roughness in the bitangent direction — physical scalar.  "
			"Set equal to alphax for isotropic GGX." ) );
		rows.push_back( BuildScalarPainterSlot( "ior",      ggx->GetIOR(),
			scalarPainters, composed,
			"Index of refraction (Fresnel conductor mode only; ignored in schlick_f0)." ) );
		rows.push_back( BuildScalarPainterSlot( "ext",      ggx->GetExtinction(),
			scalarPainters, composed,
			"Extinction coefficient (Fresnel conductor mode only)." ) );
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

	// Suppress the unused-parameter warning for scalarPainters until
	// scalar-painter slot surfacing lands per material in Stage 4B.
	(void)scalarPainters;

	return rows;
}

// -------------------------------------------------------------------
// Per-material slot get/set dispatch.  Each material type added in
// Stage 4B fills in its case here.  Lambertian is the working
// prototype.
// -------------------------------------------------------------------

MaterialSlotRef MaterialIntrospection::GetSlot(
	const IMaterial& material, const String& slotName )
{
	MaterialSlotRef out;

	if( const LambertianMaterial* lam = dynamic_cast<const LambertianMaterial*>( &material ) ) {
		if( slotName == String( "reflectance" ) ) {
			out.kind    = MaterialSlotRef::Painter;
			out.painter = &lam->GetReflectance();
			return out;
		}
	}
	else if( const PerfectReflectorMaterial* pr = dynamic_cast<const PerfectReflectorMaterial*>( &material ) ) {
		if( slotName == String( "reflectance" ) ) {
			out.kind    = MaterialSlotRef::Painter;
			out.painter = &pr->GetReflectance();
			return out;
		}
	}
	else if( const PerfectRefractorMaterial* pf = dynamic_cast<const PerfectRefractorMaterial*>( &material ) ) {
		if( slotName == String( "refractivity" ) ) {
			out.kind    = MaterialSlotRef::Painter;
			out.painter = &pf->GetRefractivity();
			return out;
		}
		if( slotName == String( "ior" ) ) {
			out.kind          = MaterialSlotRef::ScalarPainter;
			out.scalarPainter = &pf->GetIOR();
			return out;
		}
	}
	else if( const PolishedMaterial* pol = dynamic_cast<const PolishedMaterial*>( &material ) ) {
		if( slotName == String( "diffuse_reflectance" ) ) {
			out.kind = MaterialSlotRef::Painter; out.painter = &pol->GetDiffuseReflectance(); return out;
		}
		if( slotName == String( "transmittance" ) ) {
			out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &pol->GetTransmittance(); return out;
		}
		if( slotName == String( "ior" ) ) {
			out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &pol->GetIOR(); return out;
		}
		if( slotName == String( "scattering" ) ) {
			out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &pol->GetScattering(); return out;
		}
	}
	else if( const DielectricMaterial* die = dynamic_cast<const DielectricMaterial*>( &material ) ) {
		if( slotName == String( "transmittance" ) ) {
			out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &die->GetTransmittance(); return out;
		}
		if( slotName == String( "ior" ) ) {
			out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &die->GetIOR(); return out;
		}
		if( slotName == String( "scattering" ) ) {
			out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &die->GetScattering(); return out;
		}
	}
	else if( const GGXMaterial* ggx = dynamic_cast<const GGXMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )  { out.kind = MaterialSlotRef::Painter;       out.painter       = &ggx->GetDiffuse();    return out; }
		if( slotName == String( "specular" ) ) { out.kind = MaterialSlotRef::Painter;       out.painter       = &ggx->GetSpecular();   return out; }
		if( slotName == String( "alphax" ) )   { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ggx->GetAlphaX();     return out; }
		if( slotName == String( "alphay" ) )   { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ggx->GetAlphaY();     return out; }
		if( slotName == String( "ior" ) )      { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ggx->GetIOR();        return out; }
		if( slotName == String( "ext" ) )      { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ggx->GetExtinction(); return out; }
	}

	// Unknown slot for this material type — out.kind stays None.
	return out;
}

bool MaterialIntrospection::SetSlot(
	IMaterial& material, const String& slotName,
	const IPainter* painter, const IScalarPainter* scalarPainter )
{
	if( LambertianMaterial* lam = dynamic_cast<LambertianMaterial*>( &material ) ) {
		if( slotName == String( "reflectance" ) ) {
			if( !painter ) return false;
			lam->SetReflectance( *painter );
			return true;
		}
		return false;
	}
	if( PerfectReflectorMaterial* pr = dynamic_cast<PerfectReflectorMaterial*>( &material ) ) {
		if( slotName == String( "reflectance" ) ) {
			if( !painter ) return false;
			pr->SetReflectance( *painter );
			return true;
		}
		return false;
	}
	if( PerfectRefractorMaterial* pf = dynamic_cast<PerfectRefractorMaterial*>( &material ) ) {
		if( slotName == String( "refractivity" ) ) {
			if( !painter ) return false;
			pf->SetRefractivity( *painter );
			return true;
		}
		if( slotName == String( "ior" ) ) {
			if( !scalarPainter ) return false;
			pf->SetIOR( *scalarPainter );
			return true;
		}
		return false;
	}
	if( PolishedMaterial* pol = dynamic_cast<PolishedMaterial*>( &material ) ) {
		if( slotName == String( "diffuse_reflectance" ) ) {
			if( !painter ) return false;
			pol->SetDiffuseReflectance( *painter );
			return true;
		}
		if( slotName == String( "transmittance" ) ) {
			if( !scalarPainter ) return false;
			pol->SetTransmittance( *scalarPainter );
			return true;
		}
		if( slotName == String( "ior" ) ) {
			if( !scalarPainter ) return false;
			pol->SetIOR( *scalarPainter );
			return true;
		}
		if( slotName == String( "scattering" ) ) {
			if( !scalarPainter ) return false;
			pol->SetScattering( *scalarPainter );
			return true;
		}
		return false;
	}
	if( DielectricMaterial* die = dynamic_cast<DielectricMaterial*>( &material ) ) {
		if( slotName == String( "transmittance" ) ) {
			if( !scalarPainter ) return false;
			die->SetTransmittance( *scalarPainter );
			return true;
		}
		if( slotName == String( "ior" ) ) {
			if( !scalarPainter ) return false;
			die->SetIOR( *scalarPainter );
			return true;
		}
		if( slotName == String( "scattering" ) ) {
			if( !scalarPainter ) return false;
			die->SetScattering( *scalarPainter );
			return true;
		}
		return false;
	}
	if( GGXMaterial* ggx = dynamic_cast<GGXMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )  { if( !painter ) return false; ggx->SetDiffuse( *painter );  return true; }
		if( slotName == String( "specular" ) ) { if( !painter ) return false; ggx->SetSpecular( *painter ); return true; }
		if( slotName == String( "alphax" ) )   { if( !scalarPainter ) return false; ggx->SetAlphaX( *scalarPainter );     return true; }
		if( slotName == String( "alphay" ) )   { if( !scalarPainter ) return false; ggx->SetAlphaY( *scalarPainter );     return true; }
		if( slotName == String( "ior" ) )      { if( !scalarPainter ) return false; ggx->SetIOR( *scalarPainter );        return true; }
		if( slotName == String( "ext" ) )      { if( !scalarPainter ) return false; ggx->SetExtinction( *scalarPainter ); return true; }
		return false;
	}

	return false;
}
