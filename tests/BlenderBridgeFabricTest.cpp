//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeFabricTest.cpp - Contract test for the SHEEN half of
//    the Blender native bridge (ABI v12): `rise_blender_material`'s
//    `sheen_color_painter_name` / `sheen_roughness` /
//    `sheen_roughness_texture_painter_name` fields and the
//    `add_pbr_metallic_roughness_material` wrap that consumes them, in
//    src/Blender/native/rise_blender_bridge.cpp.  Debt DL-18
//    (docs/DEBT_LEDGER.md), source heading
//    docs/CLOTH_FABRIC_DESIGN.md §15 item 13; the mapping contract is
//    docs/BLENDER_MATERIAL_TRANSLATION.md.
//
//  WHY THIS TEST INCLUDES A .cpp.  Same reasoning as
//  BlenderBridgeHairTest.cpp's own banner: the bridge's translation
//  functions live in an anonymous namespace inside a standalone shared
//  library, so the only way to exercise the REAL, SHIPPING
//  `add_material` / `add_pbr_metallic_roughness_material` (rather than
//  a re-implementation that could silently drift from the bridge) is
//  to compile the bridge .cpp into this test's own translation unit.
//
//  RED-PROOF HISTORY (see the fix commit message for the actual
//  captured output): this file was first written against a
//  `rise_blender_bridge.h` / `.cpp` that had NO `sheen_*` fields at
//  all -- `Sheen*Fixture()` below referencing
//  `material.sheen_color_painter_name` failed to COMPILE
//  ("no member named 'sheen_color_painter_name' in
//  'rise_blender_material'"), which is the genuine, unfixed-code
//  failure for a brand-new ABI field (there is no *runtime* behaviour
//  to be wrong yet -- the slot doesn't exist).  Once the header/.cpp
//  gained the three fields and the fabric-wrap branch, this file
//  compiles and the checks below exercise the real behaviour.
//
//  The seven groups:
//
//    1. THE ABI ITSELF.  Version 12; the three new fields are APPENDED
//       after `emissive_scale` (every v11 offset survives).
//
//    2. NO SHEEN -> NO CHANGE.  A PBR material with
//       `sheen_color_painter_name == NULL` (the common case, and what
//       every payload built before this debt closed looks like)
//       registers directly under its own name, is NOT a FabricMaterial,
//       and its BRDF response is unaffected -- bit-identical to pre-v12.
//
//    3. SHEEN -> A REAL FabricMaterial WRAPPING THE REAL PBR BASE.  When
//       `sheen_color_painter_name` is set, the material registered under
//       the requested name IS a `FabricMaterial`; the PBR base it wraps
//       is ALSO registered (under `<name>::pbrbase`) and is NOT itself a
//       FabricMaterial; and the two respond differently at the probe
//       (the sheen lobe adds energy the bare base doesn't have).
//
//    4. NUMERIC SHEEN ROUGHNESS CHANGES THE RESPONSE.  Two otherwise-
//       identical sheen materials that differ only in the numeric
//       `sheen_roughness` field produce different BRDF responses.
//
//    5. TEXTURE-DRIVEN SHEEN ROUGHNESS (v12's one texture exception,
//       mirroring hair's v10 beta_m/beta_n/ior pattern).  A
//       `sheen_roughness_texture_painter_name` bound to a spatially
//       varying colour painter really does vary the rendered roughness
//       across UV -- checked against a numeric-only twin that must NOT
//       vary -- and the missing-painter-name case falls back to the
//       numeric field non-fatally... except sheen has no such thing:
//       unlike hair's optional groom, a material with an UNRESOLVABLE
//       required painter (`sheen_color_painter_name` naming nothing
//       registered) is the material's OWN base_color/metallic/roughness
//       failure mode -- fatal, matching every other required PBR slot
//       in this same function.
//
//    6. SHEEN + EMISSION KEEP BOTH (post-DL-18 review P1 fix,
//       2026-09-17).  RED-PROOF HISTORY: before this fix,
//       `add_pbr_metallic_roughness_material` baked `emission_-
//       painter_name` into the PBR base UNCONDITIONALLY, then handed
//       that same base to `AddFabricMaterial` as the sheen substrate
//       whenever `sheen_color_painter_name` was also set --
//       `FabricMaterial::IsSupportedSubstrate` (FabricMaterial.h)
//       refuses any substrate with a non-null `GetEmitter()`, so
//       `add_material` FAILED OUTRIGHT for Emission Strength > 0 +
//       Sheen Weight > 0 on the SAME Principled node, and because
//       `rise_blender_scene_to_job`'s material loop aborts the WHOLE
//       job on one material's failure (rise_blender_bridge.cpp,
//       `rise_blender_render_scene`), this single combination failed
//       an entire render.  Fixed by building the PBR base WITHOUT
//       emission when sheen contributes, wrapping THAT in
//       `fabric_material`, then re-attaching the emission at the OUTER
//       layer via `AddLambertianLuminaireMaterial` -- a fully generic
//       wrapper that forwards `GetBSDF`/`GetSPF` from any `IMaterial`
//       and builds its own `LambertianEmitter`, the IDENTICAL class
//       `GGXMaterial`'s emissive constructor uses, so keeping both
//       sheen and emission is physically equivalent to baking emission
//       straight into the GGX base.  This group proves the material
//       registers (where it used to fail outright), that the final
//       name carries a real emitter, that the fabric (sheen) layer is
//       still genuinely composed in underneath it, and that the PBR
//       base at the bottom of the stack does NOT itself carry the
//       emitter (so it stays a legal fabric substrate).
//
//    7. An unresolvable `sheen_color_painter_name` is a fatal failure,
//       matching every other required PBR slot in this function.
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
#include "../src/Library/Materials/FabricMaterial.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Utilities/Color/ColorMath.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"

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

//! A fixed shading point + a fixed light direction, so materials can be
//! compared by BEHAVIOUR.  Same construction FabricMaterialChunkTest.cpp
//! uses (`MakeProbe` / `ProbeLight`).
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

//! Max-channel BRDF response of a registered material at the probe, at
//! a given UV.  -1 when the material (or its BSDF) is missing.
double Respond( RISE::IJobPriv& job, const char* matName, const double u = 0.5, const double v = 0.5 )
{
	RISE::IMaterial* m = job.GetMaterials() ? job.GetMaterials()->GetItem( matName ) : 0;
	if( !m || !m->GetBSDF() ) return -1.0;
	RISE::RayIntersectionGeometric ri = MakeProbe( u, v );
	return RISE::ColorMath::MaxValue( m->GetBSDF()->value( ProbeLight(), ri ) );
}

const FabricMaterial* FabricOf( RISE::IJobPriv& job, const char* name )
{
	RISE::IMaterial* material = job.GetMaterials() ? job.GetMaterials()->GetItem( name ) : 0;
	return dynamic_cast<const FabricMaterial*>( material );
}

//! A minimal, valid PBR-metallic-roughness `rise_blender_material`
//! fixture: registers the three painters it needs under
//! `<tag>_base` / `<tag>_metal` / `<tag>_rough` and returns the struct
//! with `model = RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS` and
//! every other field zeroed (in particular `sheen_color_painter_name
//! == NULL`, the "no sheen" default).  Callers add sheen fields
//! themselves.
rise_blender_material PbrFixture( RISE::IJobPriv& job, const std::string& tag )
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
	// sheen_color_painter_name / sheen_roughness / sheen_roughness_texture_painter_name
	// all stay at their memset-zero default: NULL / 0.0 / NULL.
	return mat;
}

// ============================================================
//  1. The ABI itself
// ============================================================

void TestAbiVersionAndLayout()
{
	std::cout << "Test: ABI version 12 and append-only v12 sheen fields" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 12, "RISE_BLENDER_API_VERSION is 12" );
	Check( rise_blender_api_version() == RISE_BLENDER_API_VERSION,
		"rise_blender_api_version() reports the compiled-in constant" );

	Check( offsetof( rise_blender_material, sheen_color_painter_name ) >
	       offsetof( rise_blender_material, emissive_scale ),
		"material.sheen_color_painter_name is appended after the last v11 field (emissive_scale)" );
	Check( offsetof( rise_blender_material, sheen_roughness ) >
	       offsetof( rise_blender_material, sheen_color_painter_name ),
		"material.sheen_roughness follows sheen_color_painter_name" );
	Check( offsetof( rise_blender_material, sheen_roughness_texture_painter_name ) >
	       offsetof( rise_blender_material, sheen_roughness ),
		"material.sheen_roughness_texture_painter_name is the last v12 field" );
}

// ============================================================
//  2. No sheen -> no change
// ============================================================

void TestNoSheenIsUnaffected()
{
	std::cout << "Test: a PBR material with no sheen fields is unaffected" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = PbrFixture( *job, "plain" );
	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), "plain PBR material registered" );

	Check( FabricOf( *job, "plain" ) == 0, "a sheen-less PBR material is NOT a FabricMaterial" );
	Check( (*job).GetMaterials()->GetItem( "plain::pbrbase" ) == 0,
		"no intermediate `::pbrbase` material is registered when there is no sheen" );

	const double r = Respond( *job, "plain" );
	Check( r > 0.0, "the plain material has a real, positive BRDF response" );
}

// ============================================================
//  3. Sheen wraps a real FabricMaterial around the real PBR base
// ============================================================

void TestSheenWrapsFabricMaterial()
{
	std::cout << "Test: sheen fields wrap the PBR base in a real FabricMaterial" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double sheenColor[3] = { 1.0, 1.0, 1.0 };
	(*job).AddUniformColorPainter( "sheen_white", sheenColor, "Rec709RGB_Linear" );

	rise_blender_material mat = PbrFixture( *job, "sheeny" );
	mat.sheen_color_painter_name = "sheen_white";
	mat.sheen_roughness = 0.3;

	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "sheeny material registered: " ) + err );

	Check( FabricOf( *job, "sheeny" ) != 0, "the final name IS a FabricMaterial" );
	Check( FabricOf( *job, "sheeny::pbrbase" ) == 0, "the intermediate PBR base is NOT itself a FabricMaterial" );
	Check( (*job).GetMaterials()->GetItem( "sheeny::pbrbase" ) != 0, "the intermediate PBR base IS registered" );

	const double bare  = Respond( *job, "sheeny::pbrbase" );
	const double fabric = Respond( *job, "sheeny" );
	Check( bare > 0.0 && fabric > 0.0, "both the bare base and the fabric wrap have a real response" );
	Check( std::fabs( fabric - bare ) > 1.0e-6,
		"the fabric-wrapped response differs from the bare PBR base's (the sheen lobe is really composed in)" );
}

// ============================================================
//  4. Numeric sheen roughness changes the response
// ============================================================

void TestNumericSheenRoughnessVaries()
{
	std::cout << "Test: sheen_roughness (numeric) changes the BRDF response" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double sheenColor[3] = { 1.0, 1.0, 1.0 };
	(*job).AddUniformColorPainter( "sheen_white2", sheenColor, "Rec709RGB_Linear" );

	rise_blender_material lo = PbrFixture( *job, "sheen_lo" );
	lo.sheen_color_painter_name = "sheen_white2";
	lo.sheen_roughness = 0.08;

	rise_blender_material hi = PbrFixture( *job, "sheen_hi" );
	hi.sheen_color_painter_name = "sheen_white2";
	hi.sheen_roughness = 0.9;

	char err[256] = { 0 };
	Check( add_material( *job, lo, err, sizeof( err ) ), "low-roughness sheen material registered" );
	Check( add_material( *job, hi, err, sizeof( err ) ), "high-roughness sheen material registered" );

	const double rLo = Respond( *job, "sheen_lo" );
	const double rHi = Respond( *job, "sheen_hi" );
	Check( rLo > 0.0 && rHi > 0.0, "both roughness variants have a real response" );
	Check( std::fabs( rLo - rHi ) > 1.0e-6, "different sheen_roughness values give different responses" );
}

// ============================================================
//  5. Texture-driven sheen roughness (v12 exception)
// ============================================================

void TestTextureDrivenSheenRoughness()
{
	std::cout << "Test: sheen_roughness_texture_painter_name varies roughness across UV" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double sheenColor[3] = { 1.0, 1.0, 1.0 };
	(*job).AddUniformColorPainter( "sheen_white3", sheenColor, "Rec709RGB_Linear" );

	// A per-channel painter standing in for a texture: RISE's uniform
	// colour painter is spatially CONSTANT, so this test needs an
	// actual spatially-varying source.  `AddChannelPainter` over a
	// checker painter gives one cheaply, matching the spirit of the
	// hair test's own texture fixture (a real image is unnecessary --
	// what matters is that the wrapper reads whatever the bound
	// painter says at each shading point).
	double checkerA[3] = { 0.05, 0.05, 0.05 };	// -> low Charlie alpha
	double checkerB[3] = { 0.95, 0.95, 0.95 };	// -> high Charlie alpha
	(*job).AddUniformColorPainter( "rough_cell_a", checkerA, "Rec709RGB_Linear" );
	(*job).AddUniformColorPainter( "rough_cell_b", checkerB, "Rec709RGB_Linear" );
	(*job).AddCheckerPainter( "sheen_rough_tex", 0.5, "rough_cell_a", "rough_cell_b" );

	rise_blender_material tex = PbrFixture( *job, "sheen_tex" );
	tex.sheen_color_painter_name = "sheen_white3";
	tex.sheen_roughness = 0.5;	// fallback only -- overridden by the texture below
	tex.sheen_roughness_texture_painter_name = "sheen_rough_tex";

	rise_blender_material num = PbrFixture( *job, "sheen_num" );
	num.sheen_color_painter_name = "sheen_white3";
	num.sheen_roughness = 0.5;	// same numeric value, no texture

	char err[256] = { 0 };
	Check( add_material( *job, tex, err, sizeof( err ) ), std::string( "textured-roughness sheen material registered: " ) + err );
	Check( add_material( *job, num, err, sizeof( err ) ), "numeric-roughness sheen material registered" );

	// Two UVs differing by exactly ONE checker cell in u alone (v fixed):
	// with a checker `size` of 0.5, cell(u) = floor(u/0.5), so u=0.1 is
	// cell 0 and u=0.6 is cell 1 -- a probe pair ALONG THE u==v DIAGONAL
	// would be a trap here (cell(u)+cell(v) changes by an even amount
	// on the diagonal, so its checker parity -- and therefore its
	// colour -- never flips no matter how fine the checker is).
	const double texA = Respond( *job, "sheen_tex", 0.1, 0.5 );
	const double texB = Respond( *job, "sheen_tex", 0.6, 0.5 );
	const double numA = Respond( *job, "sheen_num", 0.1, 0.5 );
	const double numB = Respond( *job, "sheen_num", 0.6, 0.5 );

	Check( texA > 0.0 && texB > 0.0 && numA > 0.0 && numB > 0.0, "all four probes have a real response" );
	Check( std::fabs( texA - texB ) > 1.0e-6,
		"the texture-driven sheen material's response VARIES across UV (the roughness map is really consulted)" );
	Check( std::fabs( numA - numB ) < 1.0e-9,
		"the numeric-only twin does NOT vary across UV (control: same material, same response everywhere)" );
}

// ============================================================
//  6. Sheen + emission keep BOTH (post-DL-18 review P1 fix)
// ============================================================

void TestSheenWithEmissionKeepsBoth()
{
	std::cout << "Test: Emission Strength + Sheen Weight on one material keep BOTH (P1 fix)" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double sheenColor[3] = { 1.0, 1.0, 1.0 };
	(*job).AddUniformColorPainter( "sheen_white_emit", sheenColor, "Rec709RGB_Linear" );
	double emitColor[3] = { 2.0, 1.5, 0.5 };
	(*job).AddUniformColorPainter( "emit_color", emitColor, "Rec709RGB_Linear" );

	rise_blender_material mat = PbrFixture( *job, "sheeny_emit" );
	mat.sheen_color_painter_name = "sheen_white_emit";
	mat.sheen_roughness = 0.3;
	mat.emission_painter_name = "emit_color";
	mat.emissive_scale = 2.0;

	char err[256] = { 0 };
	// MONEY: pre-fix this returned false ("the substrate is a
	// luminaire") because the PBR base handed to AddFabricMaterial
	// carried the baked-in emitter.
	Check( add_material( *job, mat, err, sizeof( err ) ),
		std::string( "MONEY: sheen+emission material registered (pre-fix this failed): " ) + err );

	RISE::IMaterial* finalMat = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "sheeny_emit" ) : 0;
	Check( finalMat != 0, "the final material is registered" );
	if( finalMat ) {
		Check( finalMat->GetEmitter() != 0,
			"MONEY: the final material has a real emitter -- emission was NOT dropped" );
		Check( dynamic_cast<LambertianLuminaireMaterial*>( finalMat ) != 0,
			"the final material is a LambertianLuminaireMaterial (the outer emissive wrapper)" );
		Check( dynamic_cast<FabricMaterial*>( finalMat ) == 0,
			"the final material is NOT itself a FabricMaterial -- that lives one layer down" );
	}

	Check( FabricOf( *job, "sheeny_emit::sheenbase" ) != 0,
		"the fabric (sheen) layer is registered under the `::sheenbase` intermediate name" );
	RISE::IMaterial* sheenBase = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "sheeny_emit::sheenbase" ) : 0;
	Check( sheenBase != 0 && sheenBase->GetEmitter() == 0,
		"the fabric (sheen) layer itself carries NO emitter -- it's a legal fabric result, "
		"not itself the luminaire" );

	RISE::IMaterial* pbrBase = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "sheeny_emit::pbrbase" ) : 0;
	Check( pbrBase != 0, "the intermediate PBR base is registered" );
	Check( pbrBase != 0 && pbrBase->GetEmitter() == 0,
		"MONEY: the PBR base does NOT carry the emitter -- it stays a legal fabric_material "
		"substrate (FabricMaterial::IsSupportedSubstrate refuses GetEmitter() != 0)" );

	// The sheen lobe is really composed in: the fabric layer's BSDF
	// response differs from the bare PBR base's, same comparison
	// TestSheenWrapsFabricMaterial makes.
	const double bare = Respond( *job, "sheeny_emit::pbrbase" );
	const double sheenResponse = Respond( *job, "sheeny_emit::sheenbase" );
	Check( bare > 0.0 && sheenResponse > 0.0, "both the bare base and the fabric wrap have a real response" );
	Check( std::fabs( sheenResponse - bare ) > 1.0e-6,
		"the fabric-wrapped response still differs from the bare PBR base's -- sheen is really "
		"composed in even with emission also set" );
}

//! Control: sheen with NO emission is completely unaffected by the
//! fix above -- the fabric layer registers directly under the final
//! name (no `::sheenbase` indirection), matching
//! TestSheenWrapsFabricMaterial's existing coverage. Also checks the
//! symmetric case: emission with NO sheen still bakes straight into
//! the PBR base (no wrapper layer at all), matching pre-v12 behaviour.
void TestEmissionWithoutSheenStillBakesDirectly()
{
	std::cout << "Test: emission with no sheen is unaffected (bakes directly into the PBR base, as before)" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	double emitColor[3] = { 1.0, 1.0, 1.0 };
	(*job).AddUniformColorPainter( "emit_only_color", emitColor, "Rec709RGB_Linear" );

	rise_blender_material mat = PbrFixture( *job, "emit_only" );
	mat.emission_painter_name = "emit_only_color";
	mat.emissive_scale = 1.0;

	char err[256] = { 0 };
	Check( add_material( *job, mat, err, sizeof( err ) ), "emission-only material registered" );

	RISE::IMaterial* finalMat = (*job).GetMaterials() ? (*job).GetMaterials()->GetItem( "emit_only" ) : 0;
	Check( finalMat != 0 && finalMat->GetEmitter() != 0,
		"the final material carries the emitter directly (baked into the GGXMaterial, no wrapper)" );
	Check( dynamic_cast<LambertianLuminaireMaterial*>( finalMat ) == 0,
		"no LambertianLuminaireMaterial wrapper was introduced -- emission-only is unchanged" );
	Check( (*job).GetMaterials()->GetItem( "emit_only::pbrbase" ) == 0,
		"no `::pbrbase` intermediate exists when there is no sheen" );
	Check( (*job).GetMaterials()->GetItem( "emit_only::sheenbase" ) == 0,
		"no `::sheenbase` intermediate exists when there is no sheen" );
}

// ============================================================
//  7. An unresolvable sheen_color_painter_name is a fatal failure,
//     matching every other required PBR slot in this function.
// ============================================================

void TestDanglingSheenColorIsFatal()
{
	std::cout << "Test: an unresolvable sheen_color_painter_name fails the material (fatal, matches base_color/metallic/roughness)" << std::endl;

	JobHolder job;
	if( !job.Valid() ) { Check( false, "created a job" ); return; }

	rise_blender_material mat = PbrFixture( *job, "dangling_sheen" );
	mat.sheen_color_painter_name = "no_such_painter_at_all";

	char err[256] = { 0 };
	Check( !add_material( *job, mat, err, sizeof( err ) ), "a dangling sheen_color_painter_name fails add_material" );
	Check( err[0] != '\0', "a fatal failure names the reason in error_message" );
	Check( (*job).GetMaterials()->GetItem( "dangling_sheen" ) == 0,
		"nothing is left registered under the final name after the failure" );
}

} // namespace

int main()
{
	std::cout << "=== Blender bridge fabric (sheen) test (ABI v12) ===" << std::endl;

	TestAbiVersionAndLayout();
	TestNoSheenIsUnaffected();
	TestSheenWrapsFabricMaterial();
	TestNumericSheenRoughnessVaries();
	TestTextureDrivenSheenRoughness();
	TestSheenWithEmissionKeepsBoth();
	TestEmissionWithoutSheenStillBakesDirectly();
	TestDanglingSheenColorIsFatal();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;

	if( g_failures == 0 ) {
		std::cout << "BlenderBridgeFabricTest: PASSED" << std::endl;
		return 0;
	}

	std::cout << "BlenderBridgeFabricTest: FAILED" << std::endl;
	return 1;
}
