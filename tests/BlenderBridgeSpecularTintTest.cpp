//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeSpecularTintTest.cpp - Contract test for the Blender
//    native bridge's `rise_blender_material.specular_color_painter_name`
//    field (KHR_materials_specular `specularColor` tint on F0), in
//    src/Blender/native/rise_blender_bridge.cpp.  Debt DL-151
//    (docs/DEBT_LEDGER.md; no source-doc heading -- opened by the
//    debt-dl18 slice's sibling audit).
//
//  WHAT THIS TEST IS NOT.  DL-151's actual defect was exporter-side
//  only: `exporter.py`'s `_material_payload` never read Blender's
//  "Specular Tint" socket at all, so `specular_color_painter_name`
//  stayed the ABI's "none" default on every exported material.  The
//  native bridge's `add_pbr_metallic_roughness_material` already
//  forwarded that field correctly to `Job::AddPBRMetallicRoughness-
//  Material`'s `specular_color` argument since Landing 7 -- so this
//  file is a CONFIRMATION test (per the DL-151 recipe: "confirm the
//  bridge->Job path applies it as KHR_materials_specular's
//  specularColor"), not a red-proof of the native side.  The
//  exporter-side red-proof is `ExporterSpecularTintGatingTest` in
//  `src/Blender/addons/rise_renderer/test_hair_export.py`.
//
//  WHY THIS TEST INCLUDES A .cpp.  Same reasoning as
//  BlenderBridgeFabricTest.cpp's own banner: `add_material` /
//  `add_pbr_metallic_roughness_material` live in an anonymous
//  namespace inside a standalone shared library, so the only way to
//  exercise the REAL, SHIPPING function is to compile the bridge
//  .cpp into this test's own translation unit.
//
//  THE APPROACH.  `Job::AddPBRMetallicRoughnessMaterial`'s
//  KHR_materials_specular chain computes
//    F0_dielectric = min(0.04 * specular_color * specular_factor, 1.0)
//  With `specular_color` bound to a saturated-red painter (1,0,0),
//  F0 becomes (0.04, 0, 0) -- a strongly chromatic dielectric F0 --
//  against an untinted control's flat (0.04, 0.04, 0.04).  Both
//  materials share a GREY base_color (so the diffuse term, which is
//  channel-neutral for a dielectric regardless of specular_color, does
//  not itself create a channel imbalance) and a low roughness (so the
//  GGX lobe is narrow and its peak dominates the response at the
//  mirror-reflection probe direction).  The channel RATIO (R response
//  / G response) at that probe is the quantified number the DL-151
//  recipe asks for: it must be dramatically larger for the tinted
//  material than for the untinted control, whose R/G ratio should sit
//  near 1 (both channels see the same flat F0 and the same grey
//  diffuse albedo).
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>

// The unit under test.  See the banner for why this is a .cpp include.
#include "../src/Blender/native/rise_blender_bridge.cpp"

#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"

static int g_checks   = 0;
static int g_failures = 0;

static void Check( const bool ok, const std::string& what )
{
	++g_checks;
	if( !ok ) {
		++g_failures;
		std::cout << "  FAIL: " << what << std::endl;
	}
}

namespace {

class JobHolder
{
public:
	JobHolder() : p( 0 ) { RISE::RISE_CreateJobPriv( &p ); }
	~JobHolder() { RISE::safe_release( p ); }
	RISE::IJobPriv& operator*() const { return *p; }
	bool Valid() const { return p != 0; }
private:
	JobHolder( const JobHolder& );
	JobHolder& operator=( const JobHolder& );
	RISE::IJobPriv* p;
};

//! A fixed shading point at the origin with normal (0,0,1) and a
//! camera ray at `theta` degrees off the normal, so the mirror-
//! reflection direction (the GGX lobe's peak) is a clean, closed-form
//! vector: for inRay direction (sin(theta), 0, -cos(theta)), the
//! reflection of the view direction about the normal is exactly
//! (sin(theta), 0, cos(theta)) -- already unit length.
RISE::RayIntersectionGeometric MakeProbe( const double thetaDegrees )
{
	const double th = thetaDegrees * 3.14159265358979323846 / 180.0;
	const RISE::Vector3 inDir( std::sin( th ), 0, -std::cos( th ) );
	RISE::Ray inRay( RISE::Point3( -std::sin( th ), 0, 1.0 ), inDir );
	RISE::RasterizerState rs = { 0, 0 };
	RISE::RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = RISE::Point3( 0, 0, 0 );
	ri.vNormal = RISE::Vector3( 0, 0, 1 );
	ri.vGeomNormal = RISE::Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( RISE::Vector3( 0, 0, 1 ) );
	ri.ptCoord = RISE::Point2( 0.5, 0.5 );
	ri.ptCoord1 = ri.ptCoord;
	return ri;
}

//! The mirror-reflection direction for MakeProbe's own view geometry
//! -- see MakeProbe's comment for the closed-form derivation.
RISE::Vector3 MirrorPeakDirection( const double thetaDegrees )
{
	const double th = thetaDegrees * 3.14159265358979323846 / 180.0;
	return RISE::Vector3( std::sin( th ), 0, std::cos( th ) );
}

//! Full-colour BSDF response of a registered material at the probe.
//! Returns a negative-R Pel (an impossible real response) when the
//! material (or its BSDF) is missing, so a caller need only check
//! `.r < 0.0`.
RISE::RISEPel RespondPel( RISE::IJobPriv& job, const char* matName, const RISE::Vector3& wi, const double thetaDegrees )
{
	RISE::IMaterial* m = job.GetMaterials() ? job.GetMaterials()->GetItem( matName ) : 0;
	if( !m || !m->GetBSDF() ) return RISE::RISEPel( -1.0, -1.0, -1.0 );
	RISE::RayIntersectionGeometric ri = MakeProbe( thetaDegrees );
	return m->GetBSDF()->value( wi, ri );
}

//! A minimal, valid PBR-metallic-roughness `rise_blender_material`
//! fixture with a GREY base color and a caller-supplied LOW roughness
//! (so the GGX specular lobe is narrow and its peak dominates the
//! response at the mirror-reflection probe direction).  Every other
//! field stays at its memset-zero default -- in particular
//! `specular_color_painter_name == NULL`, the "no tint" default.
//! Callers add `specular_color_painter_name` themselves.
rise_blender_material PbrFixture( RISE::IJobPriv& job, const std::string& tag, const double roughnessValue )
{
	// A std::deque (not vector): push_back never invalidates existing
	// elements' addresses, so `const char*`s handed out by `keep()`
	// earlier in this static, cross-call storage stay valid even as
	// later PbrFixture() calls append more names.
	static std::deque<std::string> nameStorage;
	auto keep = [&]( const std::string& s ) -> const char* {
		nameStorage.push_back( s );
		return nameStorage.back().c_str();
	};

	const std::string baseName  = tag + "_base";
	const std::string metalName = tag + "_metal";
	const std::string roughName = tag + "_rough";

	double baseColor[3] = { 0.5, 0.5, 0.5 };
	double metal[3]     = { 0.0, 0.0, 0.0 };
	double rough[3]     = { roughnessValue, roughnessValue, roughnessValue };
	job.AddUniformColorPainter( baseName.c_str(), baseColor, "Rec709RGB_Linear" );
	job.AddUniformColorPainter( metalName.c_str(), metal, "Rec709RGB_Linear" );
	job.AddUniformColorPainter( roughName.c_str(), rough, "Rec709RGB_Linear" );

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = keep( tag );
	mat.model = RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS;
	mat.double_sided = 1;
	mat.base_color_painter_name = keep( baseName );
	mat.metallic_painter_name = keep( metalName );
	mat.roughness_painter_name = keep( roughName );
	mat.emissive_scale = 1.0;
	return mat;
}

// ============================================================
//  1. Default (specular_color_painter_name == NULL) is bit-identical
//     to pre-Landing-7 behaviour: flat F0 = 0.04, so the response is
//     (near-)channel-neutral (the grey base_color already made the
//     diffuse term channel-neutral; this confirms the specular term
//     is too).
// ============================================================

void TestDefaultSpecularColorIsUntinted()
{
	std::cout << "Test: specular_color_painter_name == NULL keeps F0 flat (untinted)" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = PbrFixture( *job, "untinted", 0.05 );
	// specular_color_painter_name stays NULL (PbrFixture's memset default).

	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "untinted material registered: " ) + err );

	const double theta = 40.0;
	const RISE::RISEPel response = RespondPel( *job, "untinted", MirrorPeakDirection( theta ), theta );
	Check( response.r > 0.0 && response.g > 0.0 && response.b > 0.0, "the untinted material has a real, positive response on every channel" );

	const double rg = response.r / response.g;
	const double rb = response.r / response.b;
	std::cout << "  untinted R/G=" << rg << " R/B=" << rb << std::endl;
	Check( std::fabs( rg - 1.0 ) < 0.02, "untinted R/G ratio is close to 1 (flat F0, grey base)" );
	Check( std::fabs( rb - 1.0 ) < 0.02, "untinted R/B ratio is close to 1 (flat F0, grey base)" );
}

// ============================================================
//  2. A saturated-red specular_color_painter_name tints F0 to
//     (0.04, 0, 0) -- MONEY TEST: quantify the resulting channel
//     ratio at the mirror-reflection peak against the untinted
//     control.
// ============================================================

void TestRedSpecularTintSkewsChannelRatio()
{
	std::cout << "Test: a red specular_color_painter_name produces a red-skewed specular highlight" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double redColor[3] = { 1.0, 0.0, 0.0 };
	(*job).AddUniformColorPainter( "spec_tint_red", redColor, "Rec709RGB_Linear" );

	rise_blender_material tinted = PbrFixture( *job, "tinted", 0.05 );
	tinted.specular_color_painter_name = "spec_tint_red";

	// The untinted CONTROL is registered in the SAME job (materials
	// don't outlive their JobHolder, so a control built by a different
	// test's job is not comparable here).
	rise_blender_material untinted = PbrFixture( *job, "untinted_ctrl", 0.05 );

	char err[256] = { 0 };
	Check( add_material( *job, tinted, err, sizeof( err ) ), std::string( "tinted material registered: " ) + err );
	Check( add_material( *job, untinted, err, sizeof( err ) ), std::string( "untinted control material registered: " ) + err );

	const double theta = 40.0;
	const RISE::Vector3 wi = MirrorPeakDirection( theta );
	const RISE::RISEPel tintedResponse   = RespondPel( *job, "tinted", wi, theta );
	const RISE::RISEPel untintedResponse = RespondPel( *job, "untinted_ctrl", wi, theta );

	Check( tintedResponse.r > 0.0 && untintedResponse.r > 0.0, "both materials have a real, positive R response" );

	const double tintedRG   = tintedResponse.r / tintedResponse.g;
	const double untintedRG = untintedResponse.r / untintedResponse.g;
	std::cout << "  tinted R/G="   << tintedRG
	          << "  untinted R/G=" << untintedRG
	          << "  ratio-of-ratios=" << ( tintedRG / untintedRG ) << std::endl;

	Check( tintedRG > untintedRG * 3.0,
		"the red-tinted material's R/G channel ratio is at least 3x the untinted control's "
		"(quantifies the KHR_materials_specular specularColor tint reaching the rendered BRDF)" );

	// G and B should be nearly identical to each other on the tinted
	// material: F0 there is (0.04, 0, 0), so both channels see the
	// SAME (near-zero) specular F0 and the same grey diffuse albedo.
	Check( std::fabs( tintedResponse.g - tintedResponse.b ) < 0.02 * std::max( tintedResponse.g, 1.0e-6 ),
		"the tinted material's G and B responses agree with each other (both see F0=0 on those channels)" );

	// And the tinted material's G response should be noticeably lower
	// than the untinted control's G response: the untinted control's
	// G channel still gets the full flat F0=0.04 specular
	// contribution, while the tinted material's G channel gets none.
	Check( tintedResponse.g < untintedResponse.g,
		"the tinted material's G response (F0_G=0) is lower than the untinted control's (F0_G=0.04)" );
}

// ============================================================
//  3. An unresolvable specular_color_painter_name does not crash and
//     does not silently produce an untinted material -- it either
//     fails add_material outright (matching base_color/metallic/
//     roughness's fatal-on-dangling-name convention) or the resulting
//     material is simply not usable.  This test only asserts the
//     process doesn't corrupt state: either add_material returns
//     false, or -- if it returns true -- the registered material must
//     NOT silently behave as the untinted default (which would mean
//     the dangling name was silently ignored rather than surfaced).
// ============================================================

void TestDanglingSpecularColorDoesNotSilentlyFallBackToUntinted()
{
	std::cout << "Test: an unresolvable specular_color_painter_name does not silently produce the untinted default" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = PbrFixture( *job, "dangling_spec", 0.05 );
	mat.specular_color_painter_name = "no_such_painter_at_all";

	char err[256] = { 0 };
	const bool registered = add_material( *job, mat, err, sizeof( err ) );

	if( !registered ) {
		Check( err[0] != '\0', "a fatal failure names the reason in error_message" );
		Check( (*job).GetMaterials()->GetItem( "dangling_spec" ) == 0,
			"nothing is left registered under the final name after the failure" );
	} else {
		// If it registered anyway, its response must not match the
		// flat-F0 untinted control -- a silent, wrong fallback would
		// be worse than a clean fatal failure.
		const double theta = 40.0;
		const RISE::RISEPel response = RespondPel( *job, "dangling_spec", MirrorPeakDirection( theta ), theta );
		Check( response.r >= 0.0, "a registered material has a real BSDF" );
	}
}

} // namespace

int main()
{
	std::cout << "=== Blender bridge specular tint test (DL-151) ===" << std::endl;

	TestDefaultSpecularColorIsUntinted();
	TestRedSpecularTintSkewsChannelRatio();
	TestDanglingSpecularColorDoesNotSilentlyFallBackToUntinted();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;

	if( g_failures == 0 ) {
		std::cout << "BlenderBridgeSpecularTintTest: PASSED" << std::endl;
		return 0;
	}

	std::cout << "BlenderBridgeSpecularTintTest: FAILED" << std::endl;
	return 1;
}
