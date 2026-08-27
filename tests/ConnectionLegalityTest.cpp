//////////////////////////////////////////////////////////////////////
//
//  ConnectionLegalityTest.cpp - doc-88 Phase 3 S17:
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S17 + docs/gui/MATERIAL_-
//    EDITOR.md sect. 6.4 + docs/GUI_ROADMAP.md:361.
//
//    Five parts:
//
//      PART 1 -- descriptor self-consistency spot checks: a handful of
//        `ParameterSemantics.pipe` values read straight off the live
//        descriptor registry, pinning the audit table
//        ChunkParserRegistry.cpp's `p.semantics.pipe = ...` assignments
//        encode (the source of truth this whole slice is built on).
//
//      PART 2 -- THE CORPUS SWEEP, the slice's heart. For every
//        Reference-kind parameter audited on a Painter/Material
//        descriptor (124 of them), and for each of 8 representative
//        candidate archetypes spanning every pipe/category this slice
//        models, drives the REAL parser (Cst::ParseToCst +
//        Cst::DeriveToJob on a tiny generated scene) and asserts
//        ConnectionLegality::CheckConnectionByKeyword's verdict agrees.
//        A target keyword whose BASELINE binding (the archetype
//        matching its own audited pipe) fails to derive at all (a
//        chunk needing more setup than "name + one param" -- e.g. a
//        raster-image `texture` binding with no image on disk) is
//        SKIPPED and reported by name, never silently miscounted as a
//        pass. `gen` on voronoi{2,3}d_painter is excluded from the
//        derive cross-check for a documented reason (see
//        kExcludeFromDerive below) but still spot-checked statically.
//
//      PART 3 -- the NAMED special cases from the S17 brief, each
//        explicit: scalar-into-colour, colour-into-scalar (with EXACT
//        diagnostic-text equality against Job.cpp's own
//        ResolveOrDiagnoseScalar message constants), pbr's
//        colour-manager roughness, tangent_rotation's oddball,
//        function2d-slot vs expression_painter.
//
//      PART 4 -- WouldCycle goldens: direct cycle, transitive cycle,
//        self, no-cycle diamond. Pure Cst::Document structure (no
//        derive) -- same "self-reference is resolvable regardless of
//        declaration order" precedent ReferenceGraphTest PART 1.E
//        already established.
//
//////////////////////////////////////////////////////////////////////

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/ConnectionLegality.h"
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"
#include "../src/Library/SceneEditor/ReferenceGraph.h"
#include "../src/Library/Job.h"

using namespace RISE;

static int passCount = 0, failCount = 0, skipCount = 0;
static void Check( bool c, const std::string& n )
{
	if( c ) { ++passCount; }
	else    { ++failCount; std::cout << "  FAIL: " << n << std::endl; }
}
static void CheckEq( const std::string& got, const std::string& want, const std::string& n )
{
	if( got == want ) { ++passCount; return; }
	++failCount;
	std::cout << "  FAIL: " << n << "\n    got  : " << got << "\n    want : " << want << std::endl;
}

// sprintf into a std::string, against the SAME `inline constexpr` format
// strings (ChunkDescriptor.h's "shared parser diagnostic strings") that
// Job.cpp / ChunkParserRegistry.cpp emit from and ConnectionLegality.cpp
// consumes -- so a 3b/3f/3g expected literal below can never independently
// drift from the real parser's wording; both sides format the one symbol.
static std::string FmtDiag( const char* fmt, ... )
{
	char buf[1024];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	return std::string( buf );
}

namespace
{
	// ---- one row per audited Reference parameter (124 total) ----------
	struct Row
	{
		const char* keyword;
		const char* param;
		const char* pipe;             // "Color" / "Scalar" / "Material" / "Function1D" / "Function2D"
		bool        requireSingle;
		bool        excludeFromDerive;
		const char* valueTemplate;    // "%s" unless the param needs companion tokens
	};

	// gen on voronoi{2,3}d_painter is excluded from the DERIVE cross-check:
	// Job::AddVoronoi{2,3}DPainter[WithSpace] never null-checks a per-
	// generator painter lookup (`ptrs.push_back(pPntManager->GetItem(...))`,
	// no `if(!ptrs.back())` guard) -- unlike `border`, which DOES null-check
	// (`if(!pBorder) return false;`). So the real parser can silently ACCEPT
	// an illegal `gen` candidate (construct a Voronoi painter with a null
	// per-generator IPainter*) rather than reject it, which the derive-based
	// oracle cannot distinguish from "legal". Audited and asserted via
	// CheckConnectionByKeyword directly (PART 2's static-only branch) --
	// just not cross-checked against a live derive for this one param.
	const Row kRows[] = {
		{ "checker_painter", "colora", "Color", false, false, "%s" },
		{ "checker_painter", "colorb", "Color", false, false, "%s" },
		{ "lines_painter", "colora", "Color", false, false, "%s" },
		{ "lines_painter", "colorb", "Color", false, false, "%s" },
		{ "mandelbrot_painter", "colora", "Color", false, false, "%s" },
		{ "mandelbrot_painter", "colorb", "Color", false, false, "%s" },
		{ "controlled_smoothness2d_painter", "colora", "Color", false, false, "%s" },
		{ "controlled_smoothness2d_painter", "colorb", "Color", false, false, "%s" },
		{ "gerstnerwave_painter", "colora", "Color", false, false, "%s" },
		{ "gerstnerwave_painter", "colorb", "Color", false, false, "%s" },
		{ "polynomial_function2d_painter", "colora", "Color", false, false, "%s" },
		{ "polynomial_function2d_painter", "colorb", "Color", false, false, "%s" },
		{ "composite_function2d_painter", "colora", "Color", false, false, "%s" },
		{ "composite_function2d_painter", "colorb", "Color", false, false, "%s" },
		{ "composite_function2d_painter", "child_a", "Function2D", false, false, "%s" },
		{ "composite_function2d_painter", "child_b", "Function2D", false, false, "%s" },
		{ "reactiondiffusion3d_painter", "colora", "Color", false, false, "%s" },
		{ "reactiondiffusion3d_painter", "colorb", "Color", false, false, "%s" },
		{ "gabor3d_painter", "colora", "Color", false, false, "%s" },
		{ "gabor3d_painter", "colorb", "Color", false, false, "%s" },
		{ "sdf3d_painter", "colora", "Color", false, false, "%s" },
		{ "sdf3d_painter", "colorb", "Color", false, false, "%s" },
		{ "worley3d_painter", "colora", "Color", false, false, "%s" },
		{ "worley3d_painter", "colorb", "Color", false, false, "%s" },
		{ "iridescent_painter", "colora", "Color", false, false, "%s" },
		{ "iridescent_painter", "colorb", "Color", false, false, "%s" },
		{ "blend_painter", "colora", "Color", false, false, "%s" },
		{ "blend_painter", "colorb", "Color", false, false, "%s" },
		{ "blend_painter", "mask", "Color", false, false, "%s" },
		{ "channel_painter", "source", "Color", false, false, "%s" },
		{ "lambertian_material", "reflectance", "Color", false, false, "%s" },
		{ "perfectreflector_material", "reflectance", "Color", false, false, "%s" },
		{ "perfectrefractor_material", "refractance", "Color", false, false, "%s" },
		{ "perfectrefractor_material", "ior", "Scalar", false, false, "%s" },
		{ "polished_material", "reflectance", "Color", false, false, "%s" },
		{ "polished_material", "tau", "Scalar", false, false, "%s" },
		{ "polished_material", "ior", "Scalar", false, false, "%s" },
		{ "polished_material", "scattering", "Scalar", false, false, "%s" },
		{ "dielectric_material", "tau", "Scalar", false, false, "%s" },
		{ "dielectric_material", "ior", "Scalar", false, false, "%s" },
		{ "dielectric_material", "scattering", "Scalar", false, false, "%s" },
		{ "subsurfacescattering_material", "ior", "Scalar", false, false, "%s" },
		{ "subsurfacescattering_material", "absorption", "Scalar", false, false, "%s" },
		{ "subsurfacescattering_material", "scattering", "Scalar", false, false, "%s" },
		{ "randomwalk_sss_material", "ior", "Scalar", false, false, "%s" },
		{ "randomwalk_sss_material", "absorption", "Scalar", false, false, "%s" },
		{ "randomwalk_sss_material", "scattering", "Scalar", false, false, "%s" },
		{ "lambertian_luminaire_material", "exitance", "Color", false, false, "%s" },
		{ "lambertian_luminaire_material", "material", "Material", false, false, "%s" },
		{ "phong_luminaire_material", "exitance", "Color", false, false, "%s" },
		{ "phong_luminaire_material", "material", "Material", false, false, "%s" },
		{ "phong_luminaire_material", "N", "Scalar", false, false, "%s" },
		{ "ashikminshirley_anisotropicphong_material", "rd", "Color", false, false, "%s" },
		{ "ashikminshirley_anisotropicphong_material", "rs", "Color", false, false, "%s" },
		{ "ashikminshirley_anisotropicphong_material", "nu", "Scalar", false, false, "%s" },
		{ "ashikminshirley_anisotropicphong_material", "nv", "Scalar", false, false, "%s" },
		{ "isotropic_phong_material", "rd", "Color", false, false, "%s" },
		{ "isotropic_phong_material", "rs", "Color", false, false, "%s" },
		{ "isotropic_phong_material", "N", "Scalar", false, false, "%s" },
		{ "translucent_material", "ref", "Color", false, false, "%s" },
		{ "translucent_material", "tau", "Color", false, false, "%s" },
		{ "translucent_material", "ext", "Scalar", false, false, "%s" },
		{ "translucent_material", "N", "Scalar", false, false, "%s" },
		{ "translucent_material", "scattering", "Scalar", false, false, "%s" },
		{ "generic_human_tissue_material", "sca", "Scalar", false, false, "%s" },
		{ "generic_human_tissue_material", "g", "Scalar", false, false, "%s" },
		{ "composite_material", "top", "Material", false, false, "%s" },
		{ "composite_material", "bottom", "Material", false, false, "%s" },
		{ "composite_material", "extinction", "Color", false, false, "%s" },
		{ "ward_isotropic_material", "rd", "Color", false, false, "%s" },
		{ "ward_isotropic_material", "rs", "Color", false, false, "%s" },
		{ "ward_isotropic_material", "alpha", "Scalar", false, false, "%s" },
		{ "ward_anisotropic_material", "rd", "Color", false, false, "%s" },
		{ "ward_anisotropic_material", "rs", "Color", false, false, "%s" },
		{ "ward_anisotropic_material", "alphax", "Scalar", false, false, "%s" },
		{ "ward_anisotropic_material", "alphay", "Scalar", false, false, "%s" },
		{ "ggx_material", "rd", "Color", false, false, "%s" },
		{ "ggx_material", "rs", "Color", false, false, "%s" },
		{ "ggx_material", "alphax", "Scalar", false, false, "%s" },
		{ "ggx_material", "alphay", "Scalar", false, false, "%s" },
		{ "ggx_material", "ior", "Scalar", false, false, "%s" },
		{ "ggx_material", "extinction", "Scalar", false, false, "%s" },
		{ "ggx_material", "emissive", "Color", false, false, "%s" },
		{ "ggx_material", "film_ior", "Scalar", false, false, "%s" },
		{ "ggx_material", "film_extinction", "Scalar", false, false, "%s" },
		{ "ggx_material", "film_thickness", "Scalar", false, false, "%s" },
		{ "ggx_material", "tangent_rotation", "Color", false, false, "%s" },
		{ "pbr_metallic_roughness_material", "base_color", "Color", false, false, "%s" },
		{ "pbr_metallic_roughness_material", "metallic", "Color", false, false, "%s" },
		{ "pbr_metallic_roughness_material", "roughness", "Color", false, false, "%s" },
		{ "pbr_metallic_roughness_material", "emissive", "Color", false, false, "%s" },
		{ "pbr_metallic_roughness_material", "specular_factor", "Color", false, false, "%s" },
		{ "pbr_metallic_roughness_material", "specular_color", "Color", false, false, "%s" },
		{ "pbr_metallic_roughness_material", "anisotropy_factor", "Color", false, false, "%s" },
		{ "pbr_metallic_roughness_material", "anisotropy_rotation", "Color", false, false, "%s" },
		{ "cooktorrance_material", "rd", "Color", false, false, "%s" },
		{ "cooktorrance_material", "rs", "Color", false, false, "%s" },
		{ "cooktorrance_material", "facets", "Scalar", false, false, "%s" },
		{ "cooktorrance_material", "ior", "Scalar", false, false, "%s" },
		{ "cooktorrance_material", "extinction", "Scalar", false, false, "%s" },
		{ "orennayar_material", "reflectance", "Color", false, false, "%s" },
		{ "orennayar_material", "roughness", "Scalar", false, false, "%s" },
		{ "sheen_material", "sheen_color", "Color", false, false, "%s" },
		{ "sheen_material", "sheen_roughness", "Scalar", true, false, "%s" },
		{ "schlick_material", "rd", "Color", false, false, "%s" },
		{ "schlick_material", "rs", "Color", false, false, "%s" },
		{ "schlick_material", "roughness", "Scalar", false, false, "%s" },
		{ "schlick_material", "isotropy", "Scalar", false, false, "%s" },
		{ "ramp_painter", "input", "Color", false, false, "%s" },
		{ "mapping_painter", "source", "Color", false, false, "%s" },
		{ "stochastic_tile_painter", "source", "Color", false, false, "%s" },
		{ "scatter_painter", "source", "Color", false, false, "%s" },
		{ "scatter_painter", "background", "Color", false, false, "%s" },
		{ "function2d_painter", "function2d", "Function2D", false, false, "%s" },
		{ "scalar_painter", "function1d", "Function1D", false, false, "%s" },
		{ "scalar_painter", "function2d", "Function2D", false, false, "%s" },
		{ "scalar_painter", "base", "Scalar", false, false, "%s" },
		{ "scalar_painter", "multiply", "Scalar", false, false, "%s aux_scalar" },
		{ "scalar_painter", "texture", "Color", false, false, "%s" },
		{ "scalar_painter", "painter", "Color", false, false, "%s" },
		{ "voronoi2d_painter", "gen", "Color", false, true, "%s" },
		{ "voronoi2d_painter", "border", "Color", false, false, "%s" },
		{ "voronoi3d_painter", "gen", "Color", false, true, "%s" },
		{ "voronoi3d_painter", "border", "Color", false, false, "%s" },
	};
	const size_t kNumRows = sizeof( kRows ) / sizeof( kRows[0] );

	// ---- candidate archetypes: one per pipe/category this slice models ----
	struct Candidate { const char* archName; const char* keyword; ChunkCategory category; };
	const Candidate kCandidates[] = {
		{ "arch_color",       "uniformcolor_painter",       ChunkCategory::Painter },
		{ "arch_scalar",      "scalar_painter",              ChunkCategory::Painter },
		{ "arch_material",    "lambertian_material",         ChunkCategory::Material },
		{ "arch_func1d",      "piecewise_linear_function",   ChunkCategory::Function },
		{ "arch_func2d",      "piecewise_linear_function2d", ChunkCategory::Function },
		{ "arch_exprpainter", "expression_painter",          ChunkCategory::Painter },
		{ "arch_png",         "png_painter",                 ChunkCategory::Painter },
		{ "arch_cam",         "pinhole_camera",               ChunkCategory::Camera },
	};
	const size_t kNumCandidates = sizeof( kCandidates ) / sizeof( kCandidates[0] );

	// Every archetype needed by ANY row, declared once so every generated
	// mini-scene sees the same universe of legal referents. `arch_png`
	// requires a real on-disk image (tests run with CWD == repo root, the
	// same RISE_MEDIA_PATH-relative convention every other test here uses).
	const char* kArchetypeBlock =
		"uniformcolor_painter\n{\nname arch_color\ncolor 0.5 0.5 0.5\n}\n"
		"scalar_painter\n{\nname arch_scalar\nvalue 1.0\n}\n"
		"scalar_painter\n{\nname aux_scalar\nvalue 2.0\n}\n"
		"scalar_painter\n{\nname arch_scalar_pc\nvalues 0.2 0.3 0.4\n}\n"
		"lambertian_material\n{\nname arch_material\n}\n"
		"piecewise_linear_function\n{\nname arch_func1d\ncp 0.0 0.0\ncp 1.0 1.0\n}\n"
		"piecewise_linear_function2d\n{\nname arch_func2d\n}\n"
		"expression_painter\n{\nname arch_exprpainter\nexpr 0.5\n}\n"
		"png_painter\n{\nname arch_png\nfile textures/wood.png\n}\n"
		"pinhole_camera\n{\nname arch_cam\n}\n"
		"sphere_geometry\n{\nname arch_base\n}\n"
		"hair_guides\n{\nname arch_guides\nguide 0 0 0 0 0 1\n}\n";

	std::string FormatValue( const std::string& tmpl, const std::string& candidate )
	{
		const std::size_t pos = tmpl.find( "%s" );
		if( pos == std::string::npos ) return tmpl;
		return tmpl.substr( 0, pos ) + candidate + tmpl.substr( pos + 2 );
	}

	std::string BuildScene( const std::string& targetKeyword, const std::string& param, const std::string& value )
	{
		std::ostringstream ss;
		ss << "RISE ASCII SCENE 7\n" << kArchetypeBlock;
		ss << targetKeyword << "\n{\nname t\n" << param << " " << value << "\n}\n";
		return ss.str();
	}

	//! Same as BuildScene but with an arbitrary set of `param value` lines --
	//! for the handful of PART 3 spot checks where a SIBLING field's own
	//! Finalize default is itself unresolvable (e.g. dielectric_material's
	//! `tau` defaults to the literal string "none", which fails
	//! ResolveOrDiagnoseScalar on ANY derive that doesn't set `tau`
	//! explicitly -- a genuine pre-existing quirk this slice did not
	//! introduce and is out of scope to fix) and must be pinned to a KNOWN-
	//! GOOD value so only the field under test varies.
	std::string BuildSceneMulti( const std::string& targetKeyword,
	                              const std::vector<std::pair<std::string, std::string> >& params )
	{
		std::ostringstream ss;
		ss << "RISE ASCII SCENE 7\n" << kArchetypeBlock;
		ss << targetKeyword << "\n{\nname t\n";
		for( size_t i = 0; i < params.size(); ++i ) ss << params[i].first << " " << params[i].second << "\n";
		ss << "}\n";
		return ss.str();
	}

	//! True iff chunk "t" of `targetKeyword`/`targetCategory` is registered
	//! in the manager its OWN pipe resolves against, after deriving
	//! `sceneText` into a scratch Job. NOTE: `Cst::DeriveToJob`'s refuse-all
	//! is a PARSE-TIME (PASS 1: descriptor/parameter validation) discipline
	//! -- a PASS-2 Finalize returning `false` for one chunk (an unresolved
	//! reference) does NOT block the OTHER chunks in the same document from
	//! deriving, so `DeriveToJob`'s return (a count of successfully-applied
	//! chunks) is never the right success signal when the scene also
	//! contains the always-valid archetype block. Checking the SPECIFIC
	//! target chunk's presence in its own manager is the precise oracle.
	bool TargetExists( IJobPriv& job, const std::string& targetKeyword, ChunkCategory targetCategory )
	{
		if( targetCategory == ChunkCategory::Material ) return job.GetMaterials()->GetItem( "t" ) != nullptr;
		if( targetCategory == ChunkCategory::Geometry )  return job.GetGeometries()->GetItem( "t" ) != nullptr;
		if( targetKeyword == "scalar_painter" )          return job.GetScalarPainters()->GetItem( "t" ) != nullptr;
		return job.GetPainters()->GetItem( "t" ) != nullptr;
	}

	bool DeriveTargetOK( const std::string& sceneText, const std::string& targetKeyword, ChunkCategory targetCategory )
	{
		Job* job = new Job();
		job->addref();
		Cst::Document doc = Cst::ParseToCst( sceneText );
		std::vector<std::string> diags;
		Cst::DeriveToJob( doc, *job, &diags );
		const bool ok = TargetExists( *job, targetKeyword, targetCategory );
		job->release();
		return ok;
	}

	//! PRE-FLIGHT-only helper: does the archetype block alone derive
	//! cleanly (no target chunk involved at all)?
	bool DeriveOK( const std::string& sceneText )
	{
		Job* job = new Job();
		job->addref();
		Cst::Document doc = Cst::ParseToCst( sceneText );
		std::vector<std::string> diags;
		const int count = Cst::DeriveToJob( doc, *job, &diags );
		job->release();
		return count > 0;
	}

	std::string PositiveCandidateArch( const std::string& keyword, const std::string& param, const std::string& pipe )
	{
		if( keyword == "scalar_painter" && param == "texture" ) return "arch_png";
		if( pipe == "Color" )      return "arch_color";
		if( pipe == "Scalar" )     return "arch_scalar";
		if( pipe == "Material" )   return "arch_material";
		if( pipe == "Function1D" ) return "arch_func1d";
		if( pipe == "Function2D" ) return "arch_color";   // dual-registered, Function2D-capable
		return "arch_color";
	}
}

int main()
{
	std::cout << "ConnectionLegalityTest" << std::endl;

#ifdef _WIN32
	if( std::getenv( "RISE_MEDIA_PATH" ) == nullptr ) _putenv_s( "RISE_MEDIA_PATH", "./" );
#else
	setenv( "RISE_MEDIA_PATH", "./", 0 );
#endif

	// =================================================================
	// PRE-FLIGHT -- the archetype block alone must derive cleanly, or
	// every corpus row would report a spurious skip for an environment
	// reason (missing textures/wood.png, CWD not repo root, ...) rather
	// than a real per-row finding.
	// =================================================================
	{
		std::ostringstream ss;
		ss << "RISE ASCII SCENE 7\n" << kArchetypeBlock;
		const bool ok = DeriveOK( ss.str() );
		Check( ok, "PRE-FLIGHT: the shared archetype block derives cleanly on its own" );
		if( !ok ) {
			std::cout << "  Archetype block failed to derive -- aborting (every corpus row would "
			             "spuriously skip). Check CWD == repo root and textures/wood.png exists.\n";
			std::cout << "ConnectionLegalityTest: " << passCount << " passed, " << failCount << " failed\n";
			return 1;
		}
	}

	// =================================================================
	// PART 1 -- descriptor self-consistency spot checks
	// =================================================================
	{
		const ChunkDescriptor* ggx = DescriptorForKeyword( String( "ggx_material" ) );
		Check( ggx != nullptr, "PART1: ggx_material has a registered descriptor" );
		if( ggx ) {
			bool found = false;
			for( size_t i = 0; i < ggx->parameters.size(); ++i ) {
				if( ggx->parameters[i].name == "tangent_rotation" ) {
					found = true;
					Check( ggx->parameters[i].semantics.pipe == ParameterPipe::Color,
						"PART1: ggx_material.tangent_rotation audited as Color pipe (the oddball)" );
					Check( !ggx->parameters[i].semantics.note.empty(),
						"PART1: tangent_rotation carries a non-empty oddball note" );
				}
			}
			Check( found, "PART1: ggx_material descriptor declares tangent_rotation" );
		}

		const ChunkDescriptor* sp = DescriptorForKeyword( String( "scalar_painter" ) );
		Check( sp != nullptr, "PART1: scalar_painter has a registered descriptor" );
		if( sp ) {
			for( size_t i = 0; i < sp->parameters.size(); ++i ) {
				if( sp->parameters[i].name == "base" ) {
					Check( sp->parameters[i].semantics.pipe == ParameterPipe::Scalar,
						"PART1: scalar_painter.base audited as Scalar pipe (referenceCategories={Painter} is category, not pipe)" );
				}
				if( sp->parameters[i].name == "texture" ) {
					Check( sp->parameters[i].semantics.pipe == ParameterPipe::Color,
						"PART1: scalar_painter.texture audited as Color pipe" );
					Check( sp->parameters[i].semantics.keywordAllowlist.size() == 5,
						"PART1: scalar_painter.texture carries the 5-keyword raster-image allowlist" );
				}
			}
		}

		const ChunkDescriptor* pbr = DescriptorForKeyword( String( "pbr_metallic_roughness_material" ) );
		Check( pbr != nullptr, "PART1: pbr_metallic_roughness_material has a registered descriptor" );
		if( pbr ) {
			for( size_t i = 0; i < pbr->parameters.size(); ++i ) {
				if( pbr->parameters[i].name == "roughness" ) {
					Check( pbr->parameters[i].semantics.pipe == ParameterPipe::Color,
						"PART1: pbr_metallic_roughness_material.roughness audited as Color pipe (the colour-manager-roughness oddball)" );
				}
			}
		}
	}

	// =================================================================
	// PART 2 -- the corpus sweep
	// =================================================================
	{
		int tuples = 0, mismatches = 0, skippedTargets = 0;
		std::set<std::string> skippedKeywordParam;
		for( size_t r = 0; r < kNumRows; ++r ) {
			const Row& row = kRows[r];
			const std::string kw( row.keyword ), pn( row.param ), pipe( row.pipe );

			// Self-consistency: the descriptor's audited pipe matches this row's.
			const ChunkDescriptor* d = DescriptorForKeyword( String( kw.c_str() ) );
			Check( d != nullptr, "PART2: descriptor exists for `" + kw + "`" );
			if( !d ) continue;
			const ParameterDescriptor* pd = nullptr;
			for( size_t i = 0; i < d->parameters.size(); ++i ) if( d->parameters[i].name == pn ) pd = &d->parameters[i];
			Check( pd != nullptr, "PART2: `" + kw + "." + pn + "` is declared on its descriptor" );
			if( !pd ) continue;

			// Baseline gate: does this keyword construct at all with the ONE
			// param under test bound to a KNOWN-LEGAL candidate for its own
			// audited pipe, and every other field left at Finalize's own
			// default? Skip (report, don't fail) if not.
			const std::string posArch = PositiveCandidateArch( kw, pn, pipe );
			const std::string posVal  = FormatValue( row.valueTemplate, posArch );
			if( !DeriveTargetOK( BuildScene( kw, pn, posVal ), kw, d->category ) ) {
				++skippedTargets;
				skippedKeywordParam.insert( kw + "." + pn );
				continue;
			}

			for( size_t c = 0; c < kNumCandidates; ++c ) {
				const Candidate& cand = kCandidates[c];
				const bool expected = ConnectionLegality::CheckConnectionByKeyword(
					kw, pn, cand.keyword, cand.category, /*candidateIsPerChannelValues=*/false ).legal;

				if( row.excludeFromDerive ) {
					// Static-only: just confirm the call doesn't crash / returns
					// a verdict -- no live-derive cross-check (see kRows' header
					// comment on why `gen` is excluded).
					++tuples;
					continue;
				}

				const std::string val = FormatValue( row.valueTemplate, cand.archName );
				const bool actual = DeriveTargetOK( BuildScene( kw, pn, val ), kw, d->category );
				++tuples;
				if( actual != expected ) {
					++mismatches;
					std::cout << "  MISMATCH: " << kw << "." << pn << " <- " << cand.keyword
					          << " (" << cand.archName << "): CheckConnectionByKeyword says "
					          << ( expected ? "LEGAL" : "ILLEGAL" ) << ", real derive says "
					          << ( actual ? "ACCEPTED" : "REJECTED" ) << std::endl;
				}
			}
		}
		std::cout << "PART2 corpus sweep: " << tuples << " (param, candidate) tuples checked across "
		          << ( kNumRows - skippedTargets ) << "/" << kNumRows << " audited rows ("
		          << skippedTargets << " rows skipped -- baseline could not be constructed with only "
		          << "`name` + the param under test; see the printed list below), " << mismatches
		          << " mismatches." << std::endl;
		if( !skippedKeywordParam.empty() ) {
			std::cout << "  Skipped rows: ";
			for( std::set<std::string>::const_iterator it = skippedKeywordParam.begin(); it != skippedKeywordParam.end(); ++it )
				std::cout << *it << " ";
			std::cout << std::endl;
		}
		Check( mismatches == 0, "PART2: zero corpus mismatches between CheckConnectionByKeyword and the real parser" );
	}

	// =================================================================
	// PART 3 -- named special cases, explicit
	// =================================================================
	{
		// 3a. scalar-into-colour: binding a `scalar_painter` name to a
		// Color-pipe slot. The real parser has NO shared diagnostic for
		// this direction (many Color resolvers just `return false`) --
		// ConnectionLegality still gives a clear, honestly-labelled
		// message (see ConnectionLegality.cpp's CheckColorPipe comment).
		{
			const ConnectionVerdict v = ConnectionLegality::CheckConnectionByKeyword(
				"lambertian_material", "reflectance", "scalar_painter", ChunkCategory::Painter );
			Check( !v.legal, "3a: scalar_painter into a Color-pipe slot (reflectance) is illegal" );
			Check( v.diagnostic.find( "scalar_painter" ) != std::string::npos, "3a: diagnostic names the offending scalar_painter" );
			// Cross-check against the real parser too.
			Check( !DeriveTargetOK( BuildScene( "lambertian_material", "reflectance", "arch_scalar" ),
					"lambertian_material", ChunkCategory::Material ),
				"3a: the real parser also rejects a scalar_painter name in `reflectance`" );
		}

		// 3b. colour-into-scalar: EXACT diagnostic-text equality against
		// Job.cpp's ResolveOrDiagnoseScalar branch (b) message.
		{
			const ConnectionVerdict v = ConnectionLegality::CheckConnectionByKeyword(
				"dielectric_material", "ior", "uniformcolor_painter", ChunkCategory::Painter );
			Check( !v.legal, "3b: uniformcolor_painter into a Scalar-pipe slot (ior) is illegal" );
			CheckEq( v.diagnostic,
				FmtDiag( kScalarBoundToIPainterFmt, "dielectric_material", "<name>", "ior", "uniformcolor_painter" ),
				"3b: diagnostic matches Job.cpp ResolveOrDiagnoseScalar branch (b) verbatim -- via the shared "
				"kScalarBoundToIPainterFmt constant (modulo the <name> placeholder)" );
			// `tau` must be pinned explicitly -- its OWN Finalize default is
			// the literal string "none" (a pre-existing quirk, out of scope
			// here; see BuildSceneMulti's own comment) which fails
			// ResolveOrDiagnoseScalar regardless of `ior`, so leaving it
			// defaulted would make EVERY dielectric_material derive fail and
			// this cross-check would pass for the wrong reason.
			std::vector<std::pair<std::string, std::string> > params;
			params.push_back( std::make_pair( std::string( "tau" ), std::string( "1.0" ) ) );
			params.push_back( std::make_pair( std::string( "ior" ), std::string( "arch_color" ) ) );
			Check( !DeriveTargetOK( BuildSceneMulti( "dielectric_material", params ), "dielectric_material", ChunkCategory::Material ),
				"3b: the real parser also rejects a colour painter name in `ior`" );
		}

		// 3c. pbr's colour-manager roughness: a scalar_painter name in
		// `roughness` is Color-pipe-illegal (the slot resolves via
		// pPntManager, not pScalarPntManager) even though the SLOT is
		// semantically a scalar -- and a plain colour painter IS legal.
		{
			Check( !ConnectionLegality::CheckConnectionByKeyword(
				"pbr_metallic_roughness_material", "roughness", "scalar_painter", ChunkCategory::Painter ).legal,
				"3c: pbr roughness rejects a scalar_painter name (wrong manager)" );
			Check( ConnectionLegality::CheckConnectionByKeyword(
				"pbr_metallic_roughness_material", "roughness", "uniformcolor_painter", ChunkCategory::Painter ).legal,
				"3c: pbr roughness ACCEPTS a colour painter name (the documented oddball)" );
			Check( DeriveTargetOK( BuildScene( "pbr_metallic_roughness_material", "roughness", "arch_color" ),
					"pbr_metallic_roughness_material", ChunkCategory::Material ),
				"3c: the real parser also accepts a colour painter in `roughness`" );
		}

		// 3d. tangent_rotation's oddball: Color pipe, angle by meaning.
		// A colour painter is legal; a scalar_painter is not.
		{
			Check( ConnectionLegality::CheckConnectionByKeyword(
				"ggx_material", "tangent_rotation", "uniformcolor_painter", ChunkCategory::Painter ).legal,
				"3d: tangent_rotation accepts a colour painter" );
			Check( !ConnectionLegality::CheckConnectionByKeyword(
				"ggx_material", "tangent_rotation", "scalar_painter", ChunkCategory::Painter ).legal,
				"3d: tangent_rotation rejects a scalar_painter (the documented oddball, not a scalar slot)" );
		}

		// 3e. function2d-slot vs expression_painter: composite_function2d_-
		// painter's child_a/child_b (Function2D pipe) accept a plain colour
		// painter (dual-registered) and `piecewise_linear_function2d`, but
		// NOT `expression_painter` (single-registered) or `scalar_painter`
		// (never registers into IFunction2DManager at all).
		{
			Check( ConnectionLegality::CheckConnectionByKeyword(
				"composite_function2d_painter", "child_a", "uniformcolor_painter", ChunkCategory::Painter ).legal,
				"3e: child_a accepts a plain colour painter (dual-registered)" );
			Check( ConnectionLegality::CheckConnectionByKeyword(
				"composite_function2d_painter", "child_a", "piecewise_linear_function2d", ChunkCategory::Function ).legal,
				"3e: child_a accepts piecewise_linear_function2d" );
			Check( !ConnectionLegality::CheckConnectionByKeyword(
				"composite_function2d_painter", "child_a", "expression_painter", ChunkCategory::Painter ).legal,
				"3e: child_a REJECTS expression_painter (single-registered, the named exception)" );
			Check( !ConnectionLegality::CheckConnectionByKeyword(
				"composite_function2d_painter", "child_a", "scalar_painter", ChunkCategory::Painter ).legal,
				"3e: child_a REJECTS scalar_painter (never IFunction2DManager-registered)" );
			Check( !ConnectionLegality::CheckConnectionByKeyword(
				"composite_function2d_painter", "child_a", "piecewise_linear_function", ChunkCategory::Function ).legal,
				"3e: child_a REJECTS piecewise_linear_function (1D, not 2D)" );

			// Cross-check the expression_painter exclusion against the real
			// parser. `child_b` must be pinned to a valid Function2D-capable
			// candidate -- its OWN Finalize default is "none", which is not
			// Function2D-capable and would fail the derive regardless of
			// `child_a` (same "pin the sibling field" discipline as 3b).
			std::vector<std::pair<std::string, std::string> > badParams, goodParams;
			badParams.push_back( std::make_pair( std::string( "child_a" ), std::string( "arch_exprpainter" ) ) );
			badParams.push_back( std::make_pair( std::string( "child_b" ), std::string( "arch_color" ) ) );
			Check( !DeriveTargetOK( BuildSceneMulti( "composite_function2d_painter", badParams ),
					"composite_function2d_painter", ChunkCategory::Painter ),
				"3e: the real parser also rejects expression_painter in a function2d slot" );
			goodParams.push_back( std::make_pair( std::string( "child_a" ), std::string( "arch_color" ) ) );
			goodParams.push_back( std::make_pair( std::string( "child_b" ), std::string( "arch_color" ) ) );
			Check( DeriveTargetOK( BuildSceneMulti( "composite_function2d_painter", goodParams ),
					"composite_function2d_painter", ChunkCategory::Painter ),
				"3e: the real parser accepts a plain colour painter in a function2d slot" );
		}

		// 3f. requireSingle: sheen_material.sheen_roughness rejects a
		// per-channel (`values` form) scalar_painter, statically detected
		// via the candidate's authored `values` line.
		{
			Check( ConnectionLegality::CheckConnectionByKeyword(
				"sheen_material", "sheen_roughness", "scalar_painter", ChunkCategory::Painter,
				/*candidateIsPerChannelValues=*/false ).legal,
				"3f: sheen_roughness accepts a single-valued scalar_painter" );
			const ConnectionVerdict v = ConnectionLegality::CheckConnectionByKeyword(
				"sheen_material", "sheen_roughness", "scalar_painter", ChunkCategory::Painter,
				/*candidateIsPerChannelValues=*/true );
			Check( !v.legal, "3f: sheen_roughness rejects a per-channel (`values`-form) scalar_painter" );
			CheckEq( v.diagnostic,
				FmtDiag( kScalarBoundToPerChannelFmt, "sheen_material", "<name>", "sheen_roughness", "scalar_painter" ),
				"3f: diagnostic matches Job.cpp ResolveOrDiagnoseScalar branch (a) verbatim -- via the shared "
				"kScalarBoundToPerChannelFmt constant (modulo placeholders)" );

			Check( DeriveTargetOK( BuildScene( "sheen_material", "sheen_roughness", "arch_scalar" ),
					"sheen_material", ChunkCategory::Material ),
				"3f: the real parser accepts a single-valued scalar_painter in sheen_roughness" );
			Check( !DeriveTargetOK( BuildScene( "sheen_material", "sheen_roughness", "arch_scalar_pc" ),
					"sheen_material", ChunkCategory::Material ),
				"3f: the real parser rejects a per-channel scalar_painter in sheen_roughness" );
		}

		// 3g. undeclared parameter: mirrors DispatchChunkParameters's own
		// message verbatim.
		{
			const ConnectionVerdict v = ConnectionLegality::CheckConnectionByKeyword(
				"lambertian_material", "not_a_real_param", "uniformcolor_painter", ChunkCategory::Painter );
			Check( !v.legal, "3g: an undeclared parameter name is illegal" );
			CheckEq( v.diagnostic,
				FmtDiag( kUndeclaredParameterFmt, "not_a_real_param", "lambertian_material" ),
				"3g: diagnostic matches DispatchChunkParameters's own message verbatim -- via the shared "
				"kUndeclaredParameterFmt constant" );
		}

		// 3h. hair_geometry.guides: a `ParameterPipe::Other` parameter
		// carrying a `keywordAllowlist` ({"hair_guides"}).  Not folded
		// into the PART 2 corpus (kRows) because `guides` needs a whole
		// companion trio (`base_geometry` + `count` + `length`) to derive
		// at all -- same reasoning as the `gen` exclusion documented on
		// kRows above (see the comment there, :104-113): a param whose
		// legality can't be exercised through the single-param BuildScene
		// harness gets its own explicit spot check instead.
		//
		// THIS IS THE ALLOWLIST-HOIST REGRESSION TARGET.  Before the hoist
		// (ConnectionLegality.cpp's CheckConnectionByKeyword, the comment
		// starting "A `keywordAllowlist` is a per-PARAMETER special
		// case"), `keywordAllowlist` was consulted only inside
		// CheckColorPipe -- so an Other-pipe parameter like `guides` fell
		// straight through to the generic CategoryAllowed check, which
		// only compares ChunkCategory.  `sphere_geometry` and
		// `hair_guides` are BOTH ChunkCategory::Geometry, so a
		// sphere_geometry name would have been accepted into `guides`:
		// same category, entirely wrong keyword.  The hoisted check
		// catches it because it runs ahead of, and independently of, the
		// per-pipe switch.
		{
			Check( ConnectionLegality::CheckConnectionByKeyword(
				"hair_geometry", "guides", "hair_guides", ChunkCategory::Geometry ).legal,
				"3h: hair_geometry.guides accepts a hair_guides name" );
			Check( !ConnectionLegality::CheckConnectionByKeyword(
				"hair_geometry", "guides", "uniformcolor_painter", ChunkCategory::Painter ).legal,
				"3h: hair_geometry.guides rejects a Painter (wrong category entirely)" );
			Check( !ConnectionLegality::CheckConnectionByKeyword(
				"hair_geometry", "guides", "sphere_geometry", ChunkCategory::Geometry ).legal,
				"3h MONEY: hair_geometry.guides rejects a sphere_geometry -- SAME category as "
				"hair_guides, wrong keyword; exactly the case the allowlist hoist exists to catch" );

			// Cross-check against the real parser.  `guides` needs a legal
			// grow-mode hair_geometry around it (base_geometry + count +
			// length), so those three are pinned to known-good archetypes
			// and only the candidate bound to `guides` varies -- same "pin
			// the sibling fields" discipline as 3b / 3e.
			std::vector<std::pair<std::string, std::string> > params;
			params.push_back( std::make_pair( std::string( "base_geometry" ), std::string( "arch_base" ) ) );
			params.push_back( std::make_pair( std::string( "count" ),         std::string( "10" ) ) );
			params.push_back( std::make_pair( std::string( "length" ),        std::string( "0.05" ) ) );
			params.push_back( std::make_pair( std::string( "guides" ),        std::string( "arch_guides" ) ) );
			Check( DeriveTargetOK( BuildSceneMulti( "hair_geometry", params ), "hair_geometry", ChunkCategory::Geometry ),
				"3h: the real parser accepts a hair_guides name in `guides`" );

			params.back().second = "arch_color";
			Check( !DeriveTargetOK( BuildSceneMulti( "hair_geometry", params ), "hair_geometry", ChunkCategory::Geometry ),
				"3h: the real parser rejects a colour painter name in `guides`" );

			params.back().second = "arch_base";	// sphere_geometry: same category as hair_guides, wrong keyword
			Check( !DeriveTargetOK( BuildSceneMulti( "hair_geometry", params ), "hair_geometry", ChunkCategory::Geometry ),
				"3h MONEY: the real parser also rejects a sphere_geometry name in `guides` (Job::AddHairGeometry "
				"resolves `guides` against the Job-side hair_guides table, which a sphere_geometry name is never in)" );
		}
	}

	// =================================================================
	// PART 4 -- WouldCycle goldens (pure Cst::Document structure)
	// =================================================================
	{
		// Direct cycle: A already references B (blend_painter.colora).
		// Committing a new B->A edge would close the cycle.
		{
			const char* scene =
				"RISE ASCII SCENE 7\n"
				"uniformcolor_painter\n{\nname base\ncolor 0.1 0.1 0.1\n}\n"
				"blend_painter\n{\nname A\ncolora B\ncolorb base\nmask base\n}\n"
				"blend_painter\n{\nname B\ncolora base\ncolorb base\nmask base\n}\n";
			const Cst::Document doc = Cst::ParseToCst( scene );
			const Cst::NodeId a = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Painter, "A" );
			const Cst::NodeId b = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Painter, "B" );
			Check( a != 0 && b != 0, "PART4: A and B both resolve" );
			Check( ConnectionLegality::WouldCycle( doc, b, a ),
				"PART4 direct: B->A would close the cycle (A already references B)" );
			Check( !ConnectionLegality::WouldCycle( doc, a, b ),
				"PART4 direct: A->B is a NO-OP re-assertion of an edge that already exists structurally in "
				"the SAME direction, not a cycle (B does not reach A)" );
		}

		// Transitive: A->B->C exists. Committing C->A would close it.
		{
			const char* scene =
				"RISE ASCII SCENE 7\n"
				"uniformcolor_painter\n{\nname base\ncolor 0.1 0.1 0.1\n}\n"
				"blend_painter\n{\nname A\ncolora B\ncolorb base\nmask base\n}\n"
				"blend_painter\n{\nname B\ncolora C\ncolorb base\nmask base\n}\n"
				"blend_painter\n{\nname C\ncolora base\ncolorb base\nmask base\n}\n";
			const Cst::Document doc = Cst::ParseToCst( scene );
			const Cst::NodeId a = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Painter, "A" );
			const Cst::NodeId c = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Painter, "C" );
			Check( ConnectionLegality::WouldCycle( doc, c, a ),
				"PART4 transitive: C->A would close the A->B->C->A cycle" );
		}

		// Self: WouldCycle(X,X) is trivially true.
		{
			const char* scene =
				"RISE ASCII SCENE 7\n"
				"uniformcolor_painter\n{\nname solo\ncolor 0.2 0.2 0.2\n}\n";
			const Cst::Document doc = Cst::ParseToCst( scene );
			const Cst::NodeId solo = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Painter, "solo" );
			Check( solo != 0, "PART4 self: solo resolves" );
			Check( ConnectionLegality::WouldCycle( doc, solo, solo ), "PART4 self: WouldCycle(X,X) is true" );
		}

		// No-cycle diamond: A->B, A->C, B->D, C->D. Committing A->D is NOT
		// a cycle (D cannot reach A).
		{
			const char* scene =
				"RISE ASCII SCENE 7\n"
				"uniformcolor_painter\n{\nname baseD\ncolor 0.1 0.1 0.1\n}\n"
				"blend_painter\n{\nname D\ncolora baseD\ncolorb baseD\nmask baseD\n}\n"
				"blend_painter\n{\nname B\ncolora D\ncolorb baseD\nmask baseD\n}\n"
				"blend_painter\n{\nname C\ncolora D\ncolorb baseD\nmask baseD\n}\n"
				"blend_painter\n{\nname A\ncolora B\ncolorb C\nmask baseD\n}\n";
			const Cst::Document doc = Cst::ParseToCst( scene );
			const Cst::NodeId a = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Painter, "A" );
			const Cst::NodeId d = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Painter, "D" );
			Check( a != 0 && d != 0, "PART4 diamond: A and D both resolve" );
			Check( !ConnectionLegality::WouldCycle( doc, a, d ),
				"PART4 diamond: A->D is not a cycle (D has no path back to A)" );
		}
	}

	std::cout << "ConnectionLegalityTest: " << passCount << " passed, " << failCount
	          << " failed, " << skipCount << " skipped" << std::endl;
	return failCount == 0 ? 0 : 1;
}
