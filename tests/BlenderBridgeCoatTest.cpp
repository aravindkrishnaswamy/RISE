//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeCoatTest.cpp - Contract test for the COAT half of
//    the Blender native bridge (ABI v13): `rise_blender_material`'s
//    `coat_weight` / `coat_weight_texture_painter_name` /
//    `coat_tint_painter_name` / `coat_roughness` /
//    `coat_roughness_texture_painter_name` / `coat_ior` fields and the
//    `add_pbr_metallic_roughness_material` wrap that consumes them, in
//    src/Blender/native/rise_blender_bridge.cpp.  Debt DL-186
//    (docs/DEBT_LEDGER.md); the mapping contract is
//    docs/BLENDER_MATERIAL_TRANSLATION.md "Coat and Subsurface".
//
//  WHY THIS TEST INCLUDES A .cpp.  Same reasoning as
//  BlenderBridgeFabricTest.cpp's own banner: the bridge's translation
//  functions live in an anonymous namespace inside a standalone shared
//  library, so the only way to exercise the REAL, SHIPPING
//  `add_material` / `add_pbr_metallic_roughness_material` is to
//  compile the bridge .cpp into this test's own translation unit.
//
//  RED-PROOF HISTORY: this file was first written against a
//  `rise_blender_bridge.h` / `.cpp` that had NO `coat_*` fields and no
//  coat-wrap branch at all -- `CoatFixture()`'s callers referencing
//  `material.coat_weight` failed to COMPILE ("no member named
//  'coat_weight' in 'rise_blender_material'").  Once the header/.cpp
//  gained the six fields and the coated_material wrap branch, this
//  file compiles and the checks below exercise the real behaviour.
//
//  The eight groups mirror BlenderBridgeFabricTest.cpp's own sheen
//  groups, one layer over (`coated_material` instead of
//  `fabric_material`):
//
//    1. ABI VERSION 13; the coat + subsurface fields are APPENDED
//       after `sheen_roughness_texture_painter_name` (every v12 offset
//       survives).
//    2. NO COAT -> NO CHANGE.
//    3. COAT -> A REAL CoatedMaterial WRAPPING THE REAL PBR BASE.
//    4. NUMERIC coat_roughness CHANGES THE RESPONSE.
//    5. TEXTURE-DRIVEN coat_weight (the v13 texture exception,
//       mirroring sheen_roughness_texture_painter_name's own v12
//       precedent) really varies coverage across UV.
//    6. COAT + EMISSION KEEP BOTH.
//    7. COAT + SHEEN: sheen wins (documented layering decision,
//       mirrors GLTFSceneImporter.cpp's identical clearcoat+sheen
//       call) -- the coat layer is skipped, no `::coatbase`
//       intermediate is registered, and the final material's response
//       matches a sheen-only control (coat contributes nothing).
//    8. An unresolvable `coat_tint_painter_name` is a fatal failure,
//       matching sheen_color_painter_name's own required-slot
//       convention.
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
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Utilities/Color/ColorMath.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"

using RISE::Implementation::CoatedMaterial;
using RISE::Implementation::FabricMaterial;
using RISE::Implementation::LambertianLuminaireMaterial;

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

//! Same fixed shading point + light direction BlenderBridgeFabricTest.cpp
//! uses, so the two suites' numbers stay comparable if anyone reads them
//! side by side.
RISE::RayIntersectionGeometric MakeProbe( const double u = 0.5, const double v = 0.5 )
{
	const double th = 40.0 * 3.14159265358979323846 / 180.0;
	const RISE::Vector3 inDir( std::sin( th ), 0, -std::cos( th ) );
	RISE::Ray inRay( RISE::Point3( std::sin( th ), 0, 1.0 ), inDir );
	RISE::RasterizerState rs = { 0, 0 };
	RISE::RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = RISE::Point3( 0, 0, 0 );
	ri.vNormal = RISE::Vector3( 0, 0, 1 );
	ri.vGeomNormal = RISE::Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( RISE::Vector3( 0, 0, 1 ) );
	ri.ptCoord = RISE::Point2( u, v );
	ri.ptCoord1 = ri.ptCoord;
	return ri;
}

RISE::Vector3 ProbeLight()
{
	return RISE::Vector3Ops::Normalize( RISE::Vector3( -0.3, 0.5, 0.8 ) );
}

double Respond( RISE::IJobPriv& job, const char* matName, const double u = 0.5, const double v = 0.5 )
{
	RISE::IMaterial* m = job.GetMaterials() ? job.GetMaterials()->GetItem( matName ) : 0;
	if( !m || !m->GetBSDF() ) return -1.0;
	RISE::RayIntersectionGeometric ri = MakeProbe( u, v );
	return RISE::ColorMath::MaxValue( m->GetBSDF()->value( ProbeLight(), ri ) );
}

const CoatedMaterial* CoatedOf( RISE::IJobPriv& job, const char* name )
{
	RISE::IMaterial* material = job.GetMaterials() ? job.GetMaterials()->GetItem( name ) : 0;
	return dynamic_cast<const CoatedMaterial*>( material );
}

const FabricMaterial* FabricOf( RISE::IJobPriv& job, const char* name )
{
	RISE::IMaterial* material = job.GetMaterials() ? job.GetMaterials()->GetItem( name ) : 0;
	return dynamic_cast<const FabricMaterial*>( material );
}

//! A minimal, valid PBR-metallic-roughness fixture -- see
//! BlenderBridgeFabricTest.cpp's identical helper for the rationale.
//! Every coat/sheen/subsurface field stays at its memset-zero default
//! (NULL / 0.0, "no coat / no sheen" for the fields this file cares
//! about).
rise_blender_material PbrFixture( RISE::IJobPriv& job, const std::string& tag )
{
	static std::deque<std::string> nameStorage;
	auto keep = [&]( const std::string& s ) -> const char* {
		nameStorage.push_back( s );
		return nameStorage.back().c_str();
	};

	const std::string baseName  = tag + "_base";
	const std::string metalName = tag + "_metal";
	const std::string roughName = tag + "_rough";

	double baseColor[3] = { 0.6, 0.2, 0.2 };
	double metal[3]     = { 0.0, 0.0, 0.0 };
	double rough[3]     = { 0.4, 0.4, 0.4 };
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
//  1. The ABI itself
// ============================================================

void TestAbiVersionAndLayout()
{
	std::cout << "Test: ABI version 13 and append-only v13 coat/subsurface fields" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 13, "RISE_BLENDER_API_VERSION is 13" );
	Check( rise_blender_api_version() == RISE_BLENDER_API_VERSION,
		"rise_blender_api_version() reports the compiled-in constant" );

	Check( offsetof( rise_blender_material, coat_weight_texture_painter_name ) >
	       offsetof( rise_blender_material, sheen_roughness_texture_painter_name ),
		"material.coat_weight_texture_painter_name is appended after the last v12 field" );
	Check( offsetof( rise_blender_material, coat_weight ) >
	       offsetof( rise_blender_material, coat_weight_texture_painter_name ),
		"material.coat_weight follows coat_weight_texture_painter_name" );
	Check( offsetof( rise_blender_material, coat_tint_painter_name ) >
	       offsetof( rise_blender_material, coat_weight ),
		"material.coat_tint_painter_name follows coat_weight" );
	Check( offsetof( rise_blender_material, coat_roughness_texture_painter_name ) >
	       offsetof( rise_blender_material, coat_tint_painter_name ),
		"material.coat_roughness_texture_painter_name follows coat_tint_painter_name" );
	Check( offsetof( rise_blender_material, coat_roughness ) >
	       offsetof( rise_blender_material, coat_roughness_texture_painter_name ),
		"material.coat_roughness follows coat_roughness_texture_painter_name" );
	Check( offsetof( rise_blender_material, coat_ior ) >
	       offsetof( rise_blender_material, coat_roughness ),
		"material.coat_ior follows coat_roughness" );
	Check( offsetof( rise_blender_material, subsurface_absorption ) >
	       offsetof( rise_blender_material, coat_ior ),
		"material.subsurface_absorption follows coat_ior (subsurface fields trail coat fields)" );
}

// ============================================================
//  2. No coat -> no change
// ============================================================

void TestNoCoatIsUnaffected()
{
	std::cout << "Test: a PBR material with no coat fields is unaffected" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = PbrFixture( *job, "plain_coat" );
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), "plain PBR material registered" );

	Check( CoatedOf( *job, "plain_coat" ) == 0, "a coat-less PBR material is NOT a CoatedMaterial" );
	Check( (*job).GetMaterials()->GetItem( "plain_coat::pbrbase" ) == 0,
		"no intermediate `::pbrbase` material is registered when there is no coat" );

	const double r = Respond( *job, "plain_coat" );
	Check( r > 0.0, "the plain material has a real, positive BRDF response" );
}

// ============================================================
//  3. Coat wraps a real CoatedMaterial around the real PBR base
// ============================================================

void TestCoatWrapsCoatedMaterial()
{
	std::cout << "Test: coat fields wrap the PBR base in a real CoatedMaterial" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = PbrFixture( *job, "coaty" );
	mat.coat_weight = 1.0;
	mat.coat_roughness = 0.1;
	mat.coat_ior = 1.5;

	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "coaty material registered: " ) + err );

	Check( CoatedOf( *job, "coaty" ) != 0, "the final name IS a CoatedMaterial" );
	Check( CoatedOf( *job, "coaty::pbrbase" ) == 0, "the intermediate PBR base is NOT itself a CoatedMaterial" );
	Check( (*job).GetMaterials()->GetItem( "coaty::pbrbase" ) != 0, "the intermediate PBR base IS registered" );

	const double bare  = Respond( *job, "coaty::pbrbase" );
	const double coated = Respond( *job, "coaty" );
	Check( bare > 0.0 && coated > 0.0, "both the bare base and the coated wrap have a real response" );
	Check( std::fabs( coated - bare ) > 1.0e-6,
		"the coated-wrapped response differs from the bare PBR base's (the coat lobe is really composed in)" );
}

// ============================================================
//  4. Numeric coat roughness changes the response
// ============================================================

void TestNumericCoatRoughnessVaries()
{
	std::cout << "Test: coat_roughness (numeric) changes the BRDF response" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material lo = PbrFixture( *job, "coat_lo" );
	lo.coat_weight = 1.0;
	lo.coat_roughness = 0.02;
	lo.coat_ior = 1.5;

	rise_blender_material hi = PbrFixture( *job, "coat_hi" );
	hi.coat_weight = 1.0;
	hi.coat_roughness = 0.9;
	hi.coat_ior = 1.5;

	char err[256] = { 0 };
	Check( add_material( *job, lo, err, sizeof( err ) ), "low-roughness coat material registered" );
	Check( add_material( *job, hi, err, sizeof( err ) ), "high-roughness coat material registered" );

	const double rLo = Respond( *job, "coat_lo" );
	const double rHi = Respond( *job, "coat_hi" );
	Check( rLo > 0.0 && rHi > 0.0, "both roughness variants have a real response" );
	Check( std::fabs( rLo - rHi ) > 1.0e-6, "different coat_roughness values give different responses" );
}

// ============================================================
//  5. Texture-driven coat weight (v13 exception)
// ============================================================

void TestTextureDrivenCoatWeight()
{
	std::cout << "Test: coat_weight_texture_painter_name varies coverage across UV" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double checkerA[3] = { 0.0, 0.0, 0.0 };	// -> no coat
	double checkerB[3] = { 1.0, 1.0, 1.0 };	// -> full coat
	(*job).AddUniformColorPainter( "coat_cell_a", checkerA, "Rec709RGB_Linear" );
	(*job).AddUniformColorPainter( "coat_cell_b", checkerB, "Rec709RGB_Linear" );
	(*job).AddCheckerPainter( "coat_weight_tex", 0.5, "coat_cell_a", "coat_cell_b" );

	rise_blender_material tex = PbrFixture( *job, "coat_tex" );
	tex.coat_weight = 0.5;	// fallback only -- overridden by the texture below
	tex.coat_roughness = 0.1;
	tex.coat_ior = 1.5;
	tex.coat_weight_texture_painter_name = "coat_weight_tex";

	rise_blender_material num = PbrFixture( *job, "coat_num" );
	num.coat_weight = 0.5;	// same numeric value, no texture
	num.coat_roughness = 0.1;
	num.coat_ior = 1.5;

	char err[256] = { 0 };
	Check( add_material( *job, tex, err, sizeof( err ) ), std::string( "textured-weight coat material registered: " ) + err );
	Check( add_material( *job, num, err, sizeof( err ) ), "numeric-weight coat material registered" );

	// Same u/v pair BlenderBridgeFabricTest.cpp's own texture group
	// uses (off the u==v diagonal, where a 0.5-size checker's parity
	// never flips).
	const double texA = Respond( *job, "coat_tex", 0.1, 0.5 );
	const double texB = Respond( *job, "coat_tex", 0.6, 0.5 );
	const double numA = Respond( *job, "coat_num", 0.1, 0.5 );
	const double numB = Respond( *job, "coat_num", 0.6, 0.5 );

	Check( texA > 0.0 && texB > 0.0 && numA > 0.0 && numB > 0.0, "all four probes have a real response" );
	Check( std::fabs( texA - texB ) > 1.0e-6,
		"the texture-driven coat material's response VARIES across UV (the weight map is really consulted)" );
	Check( std::fabs( numA - numB ) < 1.0e-9,
		"the numeric-only twin does NOT vary across UV (control: same material, same response everywhere)" );
}

// ============================================================
//  6. Coat + emission keep BOTH
// ============================================================

void TestCoatWithEmissionKeepsBoth()
{
	std::cout << "Test: Emission Strength + Coat Weight on one material keep BOTH" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double emitColor[3] = { 2.0, 1.5, 0.5 };
	(*job).AddUniformColorPainter( "emit_color_coat", emitColor, "Rec709RGB_Linear" );

	rise_blender_material mat = PbrFixture( *job, "coaty_emit" );
	mat.coat_weight = 1.0;
	mat.coat_roughness = 0.1;
	mat.coat_ior = 1.5;
	mat.emission_painter_name = "emit_color_coat";
	mat.emissive_scale = 2.0;

	char err[256] = { 0 };
	// MONEY: mirrors DL-18's own P1 -- AddCoatedMaterial refuses an
	// emissive substrate exactly like AddFabricMaterial does
	// (CoatedMaterial::IsSupportedSubstrate); pre-fix (no coat-wrap
	// branch at all) this path did not exist, so this exercises the
	// same emission-before-wrap ordering the sheen branch established.
	Check( add_material( *job, mat, err, sizeof( err ) ),
		std::string( "MONEY: coat+emission material registered: " ) + err );

	RISE::IMaterial* finalMat = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "coaty_emit" ) : 0;
	Check( finalMat != 0, "the final material is registered" );
	if( finalMat ) {
		Check( finalMat->GetEmitter() != 0,
			"MONEY: the final material has a real emitter -- emission was NOT dropped" );
		Check( dynamic_cast<LambertianLuminaireMaterial*>( finalMat ) != 0,
			"the final material is a LambertianLuminaireMaterial (the outer emissive wrapper)" );
		Check( dynamic_cast<CoatedMaterial*>( finalMat ) == 0,
			"the final material is NOT itself a CoatedMaterial -- that lives one layer down" );
	}

	Check( CoatedOf( *job, "coaty_emit::coatbase" ) != 0,
		"the coated layer is registered under the `::coatbase` intermediate name" );
	RISE::IMaterial* coatBase = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "coaty_emit::coatbase" ) : 0;
	Check( coatBase != 0 && coatBase->GetEmitter() == 0,
		"the coated layer itself carries NO emitter -- it's a legal coated_material result" );

	RISE::IMaterial* pbrBase = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "coaty_emit::pbrbase" ) : 0;
	Check( pbrBase != 0, "the intermediate PBR base is registered" );
	Check( pbrBase != 0 && pbrBase->GetEmitter() == 0,
		"MONEY: the PBR base does NOT carry the emitter -- it stays a legal coated_material substrate" );

	const double bare = Respond( *job, "coaty_emit::pbrbase" );
	const double coatResponse = Respond( *job, "coaty_emit::coatbase" );
	Check( bare > 0.0 && coatResponse > 0.0, "both the bare base and the coated wrap have a real response" );
	Check( std::fabs( coatResponse - bare ) > 1.0e-6,
		"the coated-wrapped response still differs from the bare PBR base's -- the coat is really "
		"composed in even with emission also set" );
}

// ============================================================
//  7. Coat + sheen: sheen wins, coat skipped (documented layering)
// ============================================================

void TestCoatAndSheenTogetherKeepsSheenOnly()
{
	std::cout << "Test: Coat Weight + Sheen together keep SHEEN ONLY (documented layering decision)" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double sheenColor[3] = { 1.0, 1.0, 1.0 };
	(*job).AddUniformColorPainter( "sheen_white_vs_coat", sheenColor, "Rec709RGB_Linear" );

	rise_blender_material both = PbrFixture( *job, "coat_and_sheen" );
	both.coat_weight = 1.0;
	both.coat_roughness = 0.1;
	both.coat_ior = 1.5;
	both.sheen_color_painter_name = "sheen_white_vs_coat";
	both.sheen_roughness = 0.3;

	rise_blender_material sheenOnly = PbrFixture( *job, "sheen_only_ctrl" );
	sheenOnly.sheen_color_painter_name = "sheen_white_vs_coat";
	sheenOnly.sheen_roughness = 0.3;

	char err[256] = { 0 };
	Check( add_material( *job, both, err, sizeof( err ) ), std::string( "coat+sheen material registered: " ) + err );
	Check( add_material( *job, sheenOnly, err, sizeof( err ) ), "sheen-only control registered" );

	Check( FabricOf( *job, "coat_and_sheen" ) != 0,
		"the final name IS a FabricMaterial (sheen is the outer/only layer)" );
	Check( CoatedOf( *job, "coat_and_sheen" ) == 0,
		"the final name is NOT a CoatedMaterial -- coat did not wrap on top" );
	Check( (*job).GetMaterials()->GetItem( "coat_and_sheen::coatbase" ) == 0,
		"MONEY: no `::coatbase` intermediate is registered -- the coat layer is genuinely skipped, "
		"not just hidden" );

	const double bothResponse = Respond( *job, "coat_and_sheen" );
	const double sheenOnlyResponse = Respond( *job, "sheen_only_ctrl" );
	Check( bothResponse > 0.0 && sheenOnlyResponse > 0.0, "both materials have a real response" );
	Check( std::fabs( bothResponse - sheenOnlyResponse ) < 1.0e-9,
		"MONEY: the coat+sheen material's response is IDENTICAL to the sheen-only control's -- "
		"the coat genuinely contributes nothing when sheen is also present" );
}

// ============================================================
//  8. An unresolvable coat_tint_painter_name is a fatal failure
// ============================================================

void TestDanglingCoatTintIsFatal()
{
	std::cout << "Test: an unresolvable coat_tint_painter_name fails the material" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = PbrFixture( *job, "dangling_coat_tint" );
	mat.coat_weight = 1.0;
	mat.coat_roughness = 0.1;
	mat.coat_ior = 1.5;
	mat.coat_tint_painter_name = "no_such_painter_at_all";

	char err[256] = { 0 };
	Check( !add_material( *job, mat, err, sizeof( err ) ), "a dangling coat_tint_painter_name fails add_material" );
	Check( err[0] != '\0', "a fatal failure names the reason in error_message" );
	Check( (*job).GetMaterials()->GetItem( "dangling_coat_tint" ) == 0,
		"nothing is left registered under the final name after the failure" );
}

} // namespace

int main()
{
	std::cout << "=== Blender bridge coat test (DL-186) ===" << std::endl;

	TestAbiVersionAndLayout();
	TestNoCoatIsUnaffected();
	TestCoatWrapsCoatedMaterial();
	TestNumericCoatRoughnessVaries();
	TestTextureDrivenCoatWeight();
	TestCoatWithEmissionKeepsBoth();
	TestCoatAndSheenTogetherKeepsSheenOnly();
	TestDanglingCoatTintIsFatal();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;

	if( g_failures == 0 ) {
		std::cout << "BlenderBridgeCoatTest: PASSED" << std::endl;
		return 0;
	}

	std::cout << "BlenderBridgeCoatTest: FAILED" << std::endl;
	return 1;
}
