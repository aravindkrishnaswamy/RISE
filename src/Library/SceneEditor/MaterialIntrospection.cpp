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
	else if( const IsotropicPhongMaterial* iph = dynamic_cast<const IsotropicPhongMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "Rd", iph->GetRd(),
			painters, composed,
			"Diffuse reflectance painter — IPainter colour pipe (BRDF + SPF share the same instance)." ) );
		rows.push_back( BuildPainterSlot( "Rs", iph->GetRs(),
			painters, composed,
			"Specular reflectance painter for the Phong lobe — IPainter colour pipe." ) );
		rows.push_back( BuildScalarPainterSlot( "exponent", iph->GetExponent(),
			scalarPainters, composed,
			"Phong exponent — physical scalar.  Higher = sharper specular lobe." ) );
	}
	else if( const OrenNayarMaterial* on = dynamic_cast<const OrenNayarMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "reflectance", on->GetReflectance(),
			painters, composed,
			"Diffuse reflectance painter for the rough-surface Lambertian extension." ) );
		rows.push_back( BuildScalarPainterSlot( "roughness", on->GetRoughness(),
			scalarPainters, composed,
			"Surface roughness σ (radians) — physical scalar.  σ=0 collapses to Lambertian." ) );
	}
	else if( const SchlickMaterial* sch = dynamic_cast<const SchlickMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "diffuse", sch->GetDiffuse(),
			painters, composed,
			"Diffuse reflectance painter — IPainter colour pipe." ) );
		rows.push_back( BuildPainterSlot( "specular", sch->GetSpecular(),
			painters, composed,
			"Specular reflectance painter (F0 for Schlick Fresnel) — IPainter colour pipe." ) );
		rows.push_back( BuildScalarPainterSlot( "roughness", sch->GetRoughness(),
			scalarPainters, composed,
			"Surface roughness — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "isotropy", sch->GetIsotropy(),
			scalarPainters, composed,
			"Anisotropy parameter — physical scalar.  1.0 = isotropic." ) );
	}
	else if( const SheenMaterial* sh = dynamic_cast<const SheenMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "color", sh->GetColor(),
			painters, composed,
			"Sheen tint painter (Charlie distribution) — IPainter colour pipe." ) );
		rows.push_back( BuildScalarPainterSlot( "roughness", sh->GetRoughness(),
			scalarPainters, composed,
			"Sheen roughness α — physical scalar, clamped to [1e-3, 1]." ) );
	}
	else if( const AshikminShirleyAnisotropicPhongMaterial* as = dynamic_cast<const AshikminShirleyAnisotropicPhongMaterial*>( &material ) ) {
		rows.push_back( BuildScalarPainterSlot( "Nu", as->GetNu(),
			scalarPainters, composed,
			"Phong exponent along tangent u — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "Nv", as->GetNv(),
			scalarPainters, composed,
			"Phong exponent along bitangent v — physical scalar.  Set equal to Nu for isotropic Phong." ) );
		rows.push_back( BuildPainterSlot( "Rd", as->GetRd(),
			painters, composed,
			"Diffuse reflectance painter — IPainter colour pipe." ) );
		rows.push_back( BuildPainterSlot( "Rs", as->GetRs(),
			painters, composed,
			"Specular reflectance painter (Schlick F0) — IPainter colour pipe." ) );
	}
	else if( const CookTorranceMaterial* ct = dynamic_cast<const CookTorranceMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "diffuse", ct->GetDiffuse(),
			painters, composed,
			"Diffuse reflectance painter — IPainter colour pipe." ) );
		rows.push_back( BuildPainterSlot( "specular", ct->GetSpecular(),
			painters, composed,
			"Specular tint painter — IPainter colour pipe." ) );
		rows.push_back( BuildScalarPainterSlot( "masking", ct->GetMasking(),
			scalarPainters, composed,
			"Surface roughness for the GGX microfacet distribution — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "ior", ct->GetIOR(),
			scalarPainters, composed,
			"Index of refraction for the conductor Fresnel — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "ext", ct->GetExtinction(),
			scalarPainters, composed,
			"Extinction coefficient for the conductor Fresnel — physical scalar." ) );
	}
	else if( const WardIsotropicGaussianMaterial* wi = dynamic_cast<const WardIsotropicGaussianMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "diffuse", wi->GetDiffuse(),
			painters, composed,
			"Diffuse reflectance painter — IPainter colour pipe." ) );
		rows.push_back( BuildPainterSlot( "specular", wi->GetSpecular(),
			painters, composed,
			"Specular reflectance painter — IPainter colour pipe." ) );
		rows.push_back( BuildScalarPainterSlot( "alpha", wi->GetAlpha(),
			scalarPainters, composed,
			"Surface slope RMS for Ward's isotropic Gaussian — physical scalar." ) );
	}
	else if( const WardAnisotropicEllipticalGaussianMaterial* wa = dynamic_cast<const WardAnisotropicEllipticalGaussianMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "diffuse", wa->GetDiffuse(),
			painters, composed,
			"Diffuse reflectance painter — IPainter colour pipe." ) );
		rows.push_back( BuildPainterSlot( "specular", wa->GetSpecular(),
			painters, composed,
			"Specular reflectance painter — IPainter colour pipe." ) );
		rows.push_back( BuildScalarPainterSlot( "alphax", wa->GetAlphaX(),
			scalarPainters, composed,
			"Surface slope RMS along tangent u — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "alphay", wa->GetAlphaY(),
			scalarPainters, composed,
			"Surface slope RMS along bitangent v — physical scalar." ) );
	}
	else if( const TranslucentMaterial* tl = dynamic_cast<const TranslucentMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "ref", tl->GetRefFront(),
			painters, composed,
			"Front-face diffuse reflectance painter — IPainter colour pipe." ) );
		rows.push_back( BuildPainterSlot( "tau", tl->GetTrans(),
			painters, composed,
			"Transmittance painter for the primary layer — IPainter colour pipe." ) );
		rows.push_back( BuildScalarPainterSlot( "ext", tl->GetExtinction(),
			scalarPainters, composed,
			"Extinction factor (Beer-Lambert per unit distance) — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "N", tl->GetN(),
			scalarPainters, composed,
			"Phong exponent for the back-scatter lobe — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "scat", tl->GetScat(),
			scalarPainters, composed,
			"Multiple-scattering fraction (0..1) — physical scalar." ) );
	}
	else if( const GenericHumanTissueMaterial* ght = dynamic_cast<const GenericHumanTissueMaterial*>( &material ) ) {
		rows.push_back( BuildScalarPainterSlot( "sca", ght->GetSca(),
			scalarPainters, composed,
			"Scattering coefficient — physical scalar." ) );
		rows.push_back( BuildScalarPainterSlot( "g", ght->GetG(),
			scalarPainters, composed,
			"Henyey-Greenstein phase-function asymmetry factor — physical scalar." ) );
		rows.push_back( MakeReadOnlyRow(
			"params", String( "baked at construction" ),
			"whole_blood, hb_ratio, bilirubin, and beta-carotene concentrations are "
			"Scalar parameters captured at construction time — not editable through this interface." ) );
	}
	else if( const SubSurfaceScatteringMaterial* sss = dynamic_cast<const SubSurfaceScatteringMaterial*>( &material ) ) {
		rows.push_back( BuildScalarPainterSlot( "ior", sss->GetIOR(),
			scalarPainters, composed,
			"Index of refraction at the surface boundary — physical scalar." ) );
		rows.push_back( MakeReadOnlyRow(
			"params", String( "baked at construction" ),
			"absorption + scattering painters were consumed by the diffusion profile "
			"at construction time; g and roughness are Scalars baked into the BSDF/SPF. "
			"Editing requires reconstruction." ) );
	}
	else if( const RandomWalkSSSMaterial* rwm = dynamic_cast<const RandomWalkSSSMaterial*>( &material ) ) {
		rows.push_back( BuildScalarPainterSlot( "ior", rwm->GetIOR(),
			scalarPainters, composed,
			"Index of refraction at the surface boundary — physical scalar." ) );
		rows.push_back( MakeReadOnlyRow(
			"params", String( "baked at construction" ),
			"absorption + scattering painters were sampled once into the random-walk "
			"parameter snapshot at construction; g, roughness, and max_bounces are "
			"Scalars baked into the BSDF/SPF/walk.  Editing requires reconstruction." ) );
	}
	else if( const LambertianLuminaireMaterial* llm = dynamic_cast<const LambertianLuminaireMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "radEx", llm->GetRadEx(),
			painters, composed,
			"Radiant exitance painter for the Lambertian emission lobe — IPainter colour pipe.  "
			"Rebinding triggers a 100-sample refresh of the cached light-importance average." ) );
		rows.push_back( MakeReadOnlyRow(
			"base_material", String( "edit separately" ),
			"This luminaire wraps a base material.  Pick that material in the panel to edit its slots." ) );
	}
	else if( const PhongLuminaireMaterial* plm = dynamic_cast<const PhongLuminaireMaterial*>( &material ) ) {
		rows.push_back( BuildPainterSlot( "radEx", plm->GetRadEx(),
			painters, composed,
			"Radiant exitance painter for the Phong emission lobe — IPainter colour pipe.  "
			"Rebinding triggers a 100-sample refresh of the cached light-importance average." ) );
		rows.push_back( BuildScalarPainterSlot( "N", plm->GetN(),
			scalarPainters, composed,
			"Phong exponent for the emission lobe — physical scalar.  Higher = narrower cone." ) );
		rows.push_back( MakeReadOnlyRow(
			"base_material", String( "edit separately" ),
			"This luminaire wraps a base material.  Pick that material in the panel to edit its slots." ) );
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
	else if( const IsotropicPhongMaterial* iph = dynamic_cast<const IsotropicPhongMaterial*>( &material ) ) {
		if( slotName == String( "Rd" ) )       { out.kind = MaterialSlotRef::Painter;       out.painter       = &iph->GetRd();       return out; }
		if( slotName == String( "Rs" ) )       { out.kind = MaterialSlotRef::Painter;       out.painter       = &iph->GetRs();       return out; }
		if( slotName == String( "exponent" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &iph->GetExponent(); return out; }
	}
	else if( const OrenNayarMaterial* on = dynamic_cast<const OrenNayarMaterial*>( &material ) ) {
		if( slotName == String( "reflectance" ) ) { out.kind = MaterialSlotRef::Painter;       out.painter       = &on->GetReflectance(); return out; }
		if( slotName == String( "roughness" ) )   { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &on->GetRoughness();   return out; }
	}
	else if( const SchlickMaterial* sch = dynamic_cast<const SchlickMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )   { out.kind = MaterialSlotRef::Painter;       out.painter       = &sch->GetDiffuse();   return out; }
		if( slotName == String( "specular" ) )  { out.kind = MaterialSlotRef::Painter;       out.painter       = &sch->GetSpecular();  return out; }
		if( slotName == String( "roughness" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &sch->GetRoughness(); return out; }
		if( slotName == String( "isotropy" ) )  { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &sch->GetIsotropy();  return out; }
	}
	else if( const SheenMaterial* sh = dynamic_cast<const SheenMaterial*>( &material ) ) {
		if( slotName == String( "color" ) )     { out.kind = MaterialSlotRef::Painter;       out.painter       = &sh->GetColor();     return out; }
		if( slotName == String( "roughness" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &sh->GetRoughness(); return out; }
	}
	else if( const AshikminShirleyAnisotropicPhongMaterial* as = dynamic_cast<const AshikminShirleyAnisotropicPhongMaterial*>( &material ) ) {
		if( slotName == String( "Nu" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &as->GetNu(); return out; }
		if( slotName == String( "Nv" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &as->GetNv(); return out; }
		if( slotName == String( "Rd" ) ) { out.kind = MaterialSlotRef::Painter;       out.painter       = &as->GetRd(); return out; }
		if( slotName == String( "Rs" ) ) { out.kind = MaterialSlotRef::Painter;       out.painter       = &as->GetRs(); return out; }
	}
	else if( const CookTorranceMaterial* ct = dynamic_cast<const CookTorranceMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )  { out.kind = MaterialSlotRef::Painter;       out.painter       = &ct->GetDiffuse();    return out; }
		if( slotName == String( "specular" ) ) { out.kind = MaterialSlotRef::Painter;       out.painter       = &ct->GetSpecular();   return out; }
		if( slotName == String( "masking" ) )  { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ct->GetMasking();    return out; }
		if( slotName == String( "ior" ) )      { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ct->GetIOR();        return out; }
		if( slotName == String( "ext" ) )      { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ct->GetExtinction(); return out; }
	}
	else if( const WardIsotropicGaussianMaterial* wi = dynamic_cast<const WardIsotropicGaussianMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )  { out.kind = MaterialSlotRef::Painter;       out.painter       = &wi->GetDiffuse();  return out; }
		if( slotName == String( "specular" ) ) { out.kind = MaterialSlotRef::Painter;       out.painter       = &wi->GetSpecular(); return out; }
		if( slotName == String( "alpha" ) )    { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &wi->GetAlpha();    return out; }
	}
	else if( const WardAnisotropicEllipticalGaussianMaterial* wa = dynamic_cast<const WardAnisotropicEllipticalGaussianMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )  { out.kind = MaterialSlotRef::Painter;       out.painter       = &wa->GetDiffuse();  return out; }
		if( slotName == String( "specular" ) ) { out.kind = MaterialSlotRef::Painter;       out.painter       = &wa->GetSpecular(); return out; }
		if( slotName == String( "alphax" ) )   { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &wa->GetAlphaX();   return out; }
		if( slotName == String( "alphay" ) )   { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &wa->GetAlphaY();   return out; }
	}
	else if( const TranslucentMaterial* tl = dynamic_cast<const TranslucentMaterial*>( &material ) ) {
		if( slotName == String( "ref" ) )  { out.kind = MaterialSlotRef::Painter;       out.painter       = &tl->GetRefFront();   return out; }
		if( slotName == String( "tau" ) )  { out.kind = MaterialSlotRef::Painter;       out.painter       = &tl->GetTrans();      return out; }
		if( slotName == String( "ext" ) )  { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &tl->GetExtinction(); return out; }
		if( slotName == String( "N" ) )    { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &tl->GetN();          return out; }
		if( slotName == String( "scat" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &tl->GetScat();       return out; }
	}
	else if( const GenericHumanTissueMaterial* ght = dynamic_cast<const GenericHumanTissueMaterial*>( &material ) ) {
		if( slotName == String( "sca" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ght->GetSca(); return out; }
		if( slotName == String( "g" ) )   { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &ght->GetG();   return out; }
	}
	else if( const SubSurfaceScatteringMaterial* sss = dynamic_cast<const SubSurfaceScatteringMaterial*>( &material ) ) {
		if( slotName == String( "ior" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &sss->GetIOR(); return out; }
	}
	else if( const RandomWalkSSSMaterial* rwm = dynamic_cast<const RandomWalkSSSMaterial*>( &material ) ) {
		if( slotName == String( "ior" ) ) { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &rwm->GetIOR(); return out; }
	}
	else if( const LambertianLuminaireMaterial* llm = dynamic_cast<const LambertianLuminaireMaterial*>( &material ) ) {
		if( slotName == String( "radEx" ) ) { out.kind = MaterialSlotRef::Painter; out.painter = &llm->GetRadEx(); return out; }
	}
	else if( const PhongLuminaireMaterial* plm = dynamic_cast<const PhongLuminaireMaterial*>( &material ) ) {
		if( slotName == String( "radEx" ) ) { out.kind = MaterialSlotRef::Painter;       out.painter       = &plm->GetRadEx(); return out; }
		if( slotName == String( "N" ) )     { out.kind = MaterialSlotRef::ScalarPainter; out.scalarPainter = &plm->GetN();     return out; }
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
	if( IsotropicPhongMaterial* iph = dynamic_cast<IsotropicPhongMaterial*>( &material ) ) {
		if( slotName == String( "Rd" ) )       { if( !painter ) return false; iph->SetRd( *painter ); return true; }
		if( slotName == String( "Rs" ) )       { if( !painter ) return false; iph->SetRs( *painter ); return true; }
		if( slotName == String( "exponent" ) ) { if( !scalarPainter ) return false; iph->SetExponent( *scalarPainter ); return true; }
		return false;
	}
	if( OrenNayarMaterial* on = dynamic_cast<OrenNayarMaterial*>( &material ) ) {
		if( slotName == String( "reflectance" ) ) { if( !painter ) return false; on->SetReflectance( *painter ); return true; }
		if( slotName == String( "roughness" ) )   { if( !scalarPainter ) return false; on->SetRoughness( *scalarPainter ); return true; }
		return false;
	}
	if( SchlickMaterial* sch = dynamic_cast<SchlickMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )   { if( !painter ) return false; sch->SetDiffuse( *painter );  return true; }
		if( slotName == String( "specular" ) )  { if( !painter ) return false; sch->SetSpecular( *painter ); return true; }
		if( slotName == String( "roughness" ) ) { if( !scalarPainter ) return false; sch->SetRoughness( *scalarPainter ); return true; }
		if( slotName == String( "isotropy" ) )  { if( !scalarPainter ) return false; sch->SetIsotropy( *scalarPainter );  return true; }
		return false;
	}
	if( SheenMaterial* sh = dynamic_cast<SheenMaterial*>( &material ) ) {
		if( slotName == String( "color" ) )     { if( !painter ) return false; sh->SetColor( *painter ); return true; }
		if( slotName == String( "roughness" ) ) { if( !scalarPainter ) return false; sh->SetRoughness( *scalarPainter ); return true; }
		return false;
	}
	if( AshikminShirleyAnisotropicPhongMaterial* as = dynamic_cast<AshikminShirleyAnisotropicPhongMaterial*>( &material ) ) {
		if( slotName == String( "Nu" ) ) { if( !scalarPainter ) return false; as->SetNu( *scalarPainter ); return true; }
		if( slotName == String( "Nv" ) ) { if( !scalarPainter ) return false; as->SetNv( *scalarPainter ); return true; }
		if( slotName == String( "Rd" ) ) { if( !painter ) return false; as->SetRd( *painter ); return true; }
		if( slotName == String( "Rs" ) ) { if( !painter ) return false; as->SetRs( *painter ); return true; }
		return false;
	}
	if( CookTorranceMaterial* ct = dynamic_cast<CookTorranceMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )  { if( !painter ) return false; ct->SetDiffuse( *painter );  return true; }
		if( slotName == String( "specular" ) ) { if( !painter ) return false; ct->SetSpecular( *painter ); return true; }
		if( slotName == String( "masking" ) )  { if( !scalarPainter ) return false; ct->SetMasking( *scalarPainter );    return true; }
		if( slotName == String( "ior" ) )      { if( !scalarPainter ) return false; ct->SetIOR( *scalarPainter );        return true; }
		if( slotName == String( "ext" ) )      { if( !scalarPainter ) return false; ct->SetExtinction( *scalarPainter ); return true; }
		return false;
	}
	if( WardIsotropicGaussianMaterial* wi = dynamic_cast<WardIsotropicGaussianMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )  { if( !painter ) return false; wi->SetDiffuse( *painter );  return true; }
		if( slotName == String( "specular" ) ) { if( !painter ) return false; wi->SetSpecular( *painter ); return true; }
		if( slotName == String( "alpha" ) )    { if( !scalarPainter ) return false; wi->SetAlpha( *scalarPainter ); return true; }
		return false;
	}
	if( WardAnisotropicEllipticalGaussianMaterial* wa = dynamic_cast<WardAnisotropicEllipticalGaussianMaterial*>( &material ) ) {
		if( slotName == String( "diffuse" ) )  { if( !painter ) return false; wa->SetDiffuse( *painter );  return true; }
		if( slotName == String( "specular" ) ) { if( !painter ) return false; wa->SetSpecular( *painter ); return true; }
		if( slotName == String( "alphax" ) )   { if( !scalarPainter ) return false; wa->SetAlphaX( *scalarPainter ); return true; }
		if( slotName == String( "alphay" ) )   { if( !scalarPainter ) return false; wa->SetAlphaY( *scalarPainter ); return true; }
		return false;
	}
	if( TranslucentMaterial* tl = dynamic_cast<TranslucentMaterial*>( &material ) ) {
		if( slotName == String( "ref" ) )  { if( !painter ) return false; tl->SetRefFront( *painter ); return true; }
		if( slotName == String( "tau" ) )  { if( !painter ) return false; tl->SetTrans( *painter );    return true; }
		if( slotName == String( "ext" ) )  { if( !scalarPainter ) return false; tl->SetExtinction( *scalarPainter ); return true; }
		if( slotName == String( "N" ) )    { if( !scalarPainter ) return false; tl->SetN( *scalarPainter );          return true; }
		if( slotName == String( "scat" ) ) { if( !scalarPainter ) return false; tl->SetScat( *scalarPainter );       return true; }
		return false;
	}
	if( GenericHumanTissueMaterial* ght = dynamic_cast<GenericHumanTissueMaterial*>( &material ) ) {
		if( slotName == String( "sca" ) ) { if( !scalarPainter ) return false; ght->SetSca( *scalarPainter ); return true; }
		if( slotName == String( "g" ) )   { if( !scalarPainter ) return false; ght->SetG( *scalarPainter );   return true; }
		return false;
	}
	if( SubSurfaceScatteringMaterial* sss = dynamic_cast<SubSurfaceScatteringMaterial*>( &material ) ) {
		if( slotName == String( "ior" ) ) { if( !scalarPainter ) return false; sss->SetIOR( *scalarPainter ); return true; }
		return false;
	}
	if( RandomWalkSSSMaterial* rwm = dynamic_cast<RandomWalkSSSMaterial*>( &material ) ) {
		if( slotName == String( "ior" ) ) { if( !scalarPainter ) return false; rwm->SetIOR( *scalarPainter ); return true; }
		return false;
	}
	if( LambertianLuminaireMaterial* llm = dynamic_cast<LambertianLuminaireMaterial*>( &material ) ) {
		if( slotName == String( "radEx" ) ) { if( !painter ) return false; llm->SetRadEx( *painter ); return true; }
		return false;
	}
	if( PhongLuminaireMaterial* plm = dynamic_cast<PhongLuminaireMaterial*>( &material ) ) {
		if( slotName == String( "radEx" ) ) { if( !painter ) return false; plm->SetRadEx( *painter ); return true; }
		if( slotName == String( "N" ) )     { if( !scalarPainter ) return false; plm->SetN( *scalarPainter ); return true; }
		return false;
	}

	return false;
}
