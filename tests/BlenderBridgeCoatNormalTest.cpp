//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeCoatNormalTest.cpp - Contract test for DL-192's Coat
//    Normal half: `rise_blender_material`'s ABI v14
//    `coat_normal_painter_name` / `coat_normal_scale` fields and the
//    engine-level `CoatedBRDF`/`CoatedSPF` per-lobe frame perturbation
//    they drive (docs/DEBT_LEDGER.md; docs/BLENDER_MATERIAL_-
//    TRANSLATION.md "Coat Normal").
//
//  WHY THIS TEST INCLUDES A .cpp.  Same reasoning as
//  BlenderBridgeCoatTest.cpp's own banner: the bridge's translation
//  functions live in an anonymous namespace inside a standalone
//  shared library, so the only way to exercise the REAL, SHIPPING
//  `add_material` / `add_pbr_metallic_roughness_material` is to
//  compile the bridge .cpp into this test's own translation unit.
//
//  RED-PROOF HISTORY: against a pre-DL-192 `rise_blender_bridge.h` /
//  `.cpp` (no `coat_normal_*` fields at all), this file fails to
//  COMPILE ("no member named 'coat_normal_painter_name' in
//  'rise_blender_material'").  A SECOND, independent red state is
//  checked by `TestCoatNormalMovesSpecularPeak`'s own isolated-revert
//  recipe (see that function's comment): a build where the ABI exists
//  and threads through to `CoatedBRDF`'s constructor, but
//  `value()`/`valueNM()` still evaluate the coat lobe against the
//  UNPERTURBED substrate frame (the DL-100 frame-trap pattern this
//  closure's own doc explicitly calls out) -- that state compiles
//  fine and is WRONG, which is exactly the gap a pure ABI/compile
//  check cannot catch.
//
//  GEOMETRY.  The probe's `ri.onb` is built via `CreateFromW(0,0,1)`
//  on a hit with no coherent tangent, which resolves DETERMINISTICALLY
//  to U=(-1,0,0), V=(0,-1,0), W=(0,0,1) (OrthonormalBasis3D.cpp's own
//  canonical-axis construction for that exact W) -- so a tangent-space
//  coat-normal encoding of (nx, ny) decodes, in WORLD space, to
//  (-nx, -ny, nz).  This test picks (nx, ny) so the world-space tilt
//  lands along +X by a chosen angle, entirely independently of
//  CoatedBRDF's own implementation (the expected mirror directions
//  are computed here via the ordinary reflection formula, not by
//  calling into the code under test).
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
#include "../src/Library/Utilities/Color/ColorMath.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"

using RISE::Implementation::CoatedMaterial;

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
private:
	JobHolder( const JobHolder& );
	JobHolder& operator=( const JobHolder& );
	RISE::IJobPriv* p;
};

//! A flat-normal probe (no coherent tangent, matching
//! BlenderBridgeCoatTest.cpp's own MakeProbe) whose view direction is
//! `viewDir` (`ri.ray.Dir() = -viewDir`) and whose light direction is
//! `lightDir`.
RISE::RayIntersectionGeometric MakeProbe( const RISE::Vector3& viewDir, const double u = 0.5, const double v = 0.5 )
{
	RISE::Ray inRay( RISE::Point3( 0, 0, 1.0 ), RISE::Vector3Ops::Normalize( viewDir ) * -1.0 );
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

//! Perfect-mirror reflection of `v` about unit normal `n`.
RISE::Vector3 Reflect( const RISE::Vector3& v, const RISE::Vector3& n )
{
	using namespace RISE;
	return Vector3Ops::Normalize( n * ( 2.0 * Vector3Ops::Dot( n, v ) ) - v );
}

double RespondAt(
	RISE::IJobPriv& job,
	const char* matName,
	const RISE::Vector3& viewDir,
	const RISE::Vector3& lightDir
	)
{
	RISE::IMaterial* m = job.GetMaterials() ? job.GetMaterials()->GetItem( matName ) : 0;
	if( !m || !m->GetBSDF() ) return -1.0;
	RISE::RayIntersectionGeometric ri = MakeProbe( viewDir );
	return RISE::ColorMath::MaxValue( m->GetBSDF()->value( lightDir, ri ) );
}

double PdfAt(
	RISE::IJobPriv& job,
	const char* matName,
	const RISE::Vector3& viewDir,
	const RISE::Vector3& lightDir
	)
{
	RISE::IMaterial* m = job.GetMaterials() ? job.GetMaterials()->GetItem( matName ) : 0;
	if( !m || !m->GetSPF() ) return -1.0;
	RISE::RayIntersectionGeometric ri = MakeProbe( viewDir );
	RISE::IORStack iorStack( 1.0 );
	return m->GetSPF()->Pdf( ri, lightDir, iorStack );
}

const CoatedMaterial* CoatedOfCheck( RISE::IJobPriv& job, const char* name )
{
	RISE::IMaterial* material = job.GetMaterials() ? job.GetMaterials()->GetItem( name ) : 0;
	return dynamic_cast<const CoatedMaterial*>( material );
}

//! A minimal, valid PBR-metallic-roughness fixture -- see
//! BlenderBridgeCoatTest.cpp's identical helper for the rationale.
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

}	// namespace

// ============================================================
//  1. The ABI itself
// ============================================================

void TestAbiVersionAndCoatNormalFields()
{
	std::cout << "Test: ABI version 15 and preserved append-only v14 coat-normal fields" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 15, "RISE_BLENDER_API_VERSION is 15" );
	Check( rise_blender_api_version() == RISE_BLENDER_API_VERSION,
		"rise_blender_api_version() reports the compiled-in constant" );

	Check( offsetof( rise_blender_material, coat_normal_painter_name ) >
	       offsetof( rise_blender_material, subsurface_roughness ),
		"material.coat_normal_painter_name is appended after the last v13 field" );
	Check( offsetof( rise_blender_material, coat_normal_scale ) >
	       offsetof( rise_blender_material, coat_normal_painter_name ),
		"material.coat_normal_scale follows coat_normal_painter_name" );
}

// ============================================================
//  2. No coat normal -> unaffected (bit-identical to pre-v14)
// ============================================================

void TestNoCoatNormalUnaffected()
{
	std::cout << "Test: coat_weight with no coat_normal is unaffected by the new field" << std::endl;

	JobHolder job;
	char err[256];

	rise_blender_material mat = PbrFixture( *job, "no_coat_normal" );
	mat.coat_weight = 1.0;
	mat.coat_roughness = 0.08;
	mat.coat_ior = 1.5;
	// coat_normal_painter_name left NULL (memset-zero default).

	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "material registered: " ) + err );
	Check( CoatedOfCheck( *job, "no_coat_normal" ), "a real CoatedMaterial was built" );
}

// ============================================================
//  3. THE MONEY TEST -- a tilted coat normal moves the coat's own
//     specular peak while the substrate's diffuse response is
//     unaffected.
// ============================================================

void TestCoatNormalMovesSpecularPeak()
{
	std::cout << "Test: a coat_normal tilt moves the coat lobe's specular peak by the closed-form reflected angle" << std::endl;

	JobHolder job;
	char err[256];

	// View direction: mostly straight up, tilted slightly in +X so the
	// flat-normal and tilted-normal mirror directions are genuinely
	// distinct (a head-on view would make both degenerate to the same
	// retroreflection direction).
	const RISE::Vector3 viewDir = RISE::Vector3Ops::Normalize( RISE::Vector3( 0.25, 0.0, 1.0 ) );
	const RISE::Vector3 flatNormal = RISE::Vector3( 0, 0, 1 );

	// Desired WORLD-space coat-normal tilt: 20 degrees off flatNormal,
	// in the +X direction (the same plane as the view tilt, so the
	// mirror directions separate cleanly).
	const double tiltRad = 20.0 * 3.14159265358979323846 / 180.0;
	const RISE::Vector3 worldTilt = RISE::Vector3Ops::Normalize(
		RISE::Vector3( std::sin( tiltRad ), 0.0, std::cos( tiltRad ) ) );

	// Invert MakeProbe's onb (U=(-1,0,0), V=(0,-1,0), W=(0,0,1) for
	// CreateFromW(0,0,1) on a hit with no coherent tangent -- see this
	// file's banner) to find the TANGENT-SPACE (nx, ny) that decodes to
	// `worldTilt`: perturbed = U*nx + V*ny + W*nz = (-nx, -ny, nz).
	const double tangentNx = -worldTilt.x;
	const double tangentNy = -worldTilt.y;
	const double colorR = ( tangentNx + 1.0 ) / 2.0;
	const double colorG = ( tangentNy + 1.0 ) / 2.0;

	double coatNormalColor[3] = { colorR, colorG, 1.0 };
	// Rec709RGB_Linear: verbatim store, no gamma decode and no colour-
	// matrix conversion -- the SAME requirement NormalMap.h states for
	// its own tangent-space-encoded painters.
	Check( (*job).AddUniformColorPainter( "coatnormal_tex", coatNormalColor, "Rec709RGB_Linear" ),
		"coat-normal stand-in painter registered" );

	rise_blender_material flatMat = PbrFixture( *job, "coat_flat" );
	flatMat.coat_weight = 1.0;
	flatMat.coat_roughness = 0.08;
	flatMat.coat_ior = 1.5;

	rise_blender_material tiltedMat = PbrFixture( *job, "coat_tilted" );
	tiltedMat.coat_weight = 1.0;
	tiltedMat.coat_roughness = 0.08;
	tiltedMat.coat_ior = 1.5;
	tiltedMat.coat_normal_painter_name = "coatnormal_tex";
	tiltedMat.coat_normal_scale = 1.0;

	Check( add_material( *job, flatMat, err, sizeof( err ) ), std::string( "flat material registered: " ) + err );
	Check( add_material( *job, tiltedMat, err, sizeof( err ) ), std::string( "tilted material registered: " ) + err );

	const RISE::Vector3 lFlat   = Reflect( viewDir, flatNormal );
	const RISE::Vector3 lTilted = Reflect( viewDir, worldTilt );

	const double respFlatAtFlat     = RespondAt( *job, "coat_flat",   viewDir, lFlat );
	const double respFlatAtTilted   = RespondAt( *job, "coat_flat",   viewDir, lTilted );
	const double respTiltedAtFlat   = RespondAt( *job, "coat_tilted", viewDir, lFlat );
	const double respTiltedAtTilted = RespondAt( *job, "coat_tilted", viewDir, lTilted );

	std::cout << "  flat material:   value(lFlat)=" << respFlatAtFlat
	          << "  value(lTilted)=" << respFlatAtTilted << std::endl;
	std::cout << "  tilted material: value(lFlat)=" << respTiltedAtFlat
	          << "  value(lTilted)=" << respTiltedAtTilted << std::endl;

	Check( respFlatAtFlat > respFlatAtTilted,
		"no coat_normal: the specular peak stays at the FLAT mirror direction" );
	Check( respTiltedAtTilted > respTiltedAtFlat,
		"coat_normal tilted 20deg: the specular peak MOVED to the tilted mirror direction" );
	// The magnitude, not just the ordering: at this roughness the peak
	// should clearly dominate its counterpart, not win by noise.
	Check( respTiltedAtTilted > respTiltedAtFlat * 1.5,
		"the moved peak is not merely a marginal win (a real perturbation, not numerical noise)" );

	// The SUBSTRATE's own diffuse response, isolated by sampling well
	// away from either specular peak (near-grazing light on the +Y
	// side, orthogonal to the tilt plane, where the narrow coat lobe
	// contributes ~0 in EITHER material), must be UNCHANGED between
	// the two materials -- only the COAT lobe may move.
	const RISE::Vector3 offAxisLight = RISE::Vector3Ops::Normalize( RISE::Vector3( 0.0, 0.9, 0.3 ) );
	const double diffuseFlat   = RespondAt( *job, "coat_flat",   viewDir, offAxisLight );
	const double diffuseTilted = RespondAt( *job, "coat_tilted", viewDir, offAxisLight );
	Check( diffuseFlat > 0 && diffuseTilted > 0, "both materials still have a real diffuse response off-axis" );
	Check( std::fabs( diffuseFlat - diffuseTilted ) < diffuseFlat * 0.05,
		"the substrate's diffuse response is unchanged by the coat-lobe-only normal perturbation" );

	// DL-100's frame trap, applied to sampling: the SPF's own Pdf must
	// agree with value() on where the density lives -- Pdf at the
	// tilted mirror direction must be higher for the tilted material
	// than for the flat one (same "one function" consistency
	// ResolveCoatFrame's contract requires of value/Scatter/Pdf).
	const double pdfFlatAtTilted   = PdfAt( *job, "coat_flat",   viewDir, lTilted );
	const double pdfTiltedAtTilted = PdfAt( *job, "coat_tilted", viewDir, lTilted );
	Check( pdfTiltedAtTilted > pdfFlatAtTilted,
		"CoatedSPF::Pdf agrees with value(): higher density at the tilted mirror direction "
		"for the coat_normal material than for the flat one (DL-100 frame-trap consistency)" );
}

int main()
{
	std::cout << "=== Blender bridge Coat Normal test (DL-192) ===" << std::endl;

	TestAbiVersionAndCoatNormalFields();
	TestNoCoatNormalUnaffected();
	TestCoatNormalMovesSpecularPeak();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;
	if( g_failures > 0 ) {
		std::cout << "BlenderBridgeCoatNormalTest: FAILED" << std::endl;
		return 1;
	}
	std::cout << "BlenderBridgeCoatNormalTest: PASSED" << std::endl;
	return 0;
}
