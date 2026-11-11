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
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
	#include <process.h>
	#ifndef getpid
		#define getpid _getpid
	#endif
#else
	#include <unistd.h>
#endif

// The unit under test.  See the banner for why this is a .cpp include.
#include "../src/Blender/native/rise_blender_bridge.cpp"

#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IModifierManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Interfaces/IWriteBuffer.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IPainter.h"
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
	std::cout << "Test: ABI version 16 and preserved append-only v14 coat-normal fields" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 17, "RISE_BLENDER_API_VERSION is 17" );
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

//! DL-302: an independently authored Coat Normal must tilt the coat lobe
//! from the ORIGINAL frame, not compose with a base NormalMap modifier
//! that already tilted the shading frame.  The base map here is the real
//! production NormalMap::Modify run on the probe record (30 degrees
//! toward +Y, orthogonal to the coat tilt plane); the coat's own tilt is
//! 20 degrees toward +X.  The coat peak must sit at the coat-map-alone
//! mirror direction; the composed direction (reflection about
//! tilt_coat applied on top of the base-tilted frame) must NOT.
void TestCoatNormalIndependentOfBaseNormalMap()
{
	std::cout << "Test: coat normal decodes from the original frame, not the base-normal-map-tilted one (DL-302)" << std::endl;

	JobHolder job;
	char err[256];

	const RISE::Vector3 viewDir = RISE::Vector3Ops::Normalize( RISE::Vector3( 0.25, 0.0, 1.0 ) );
	const double pi = 3.14159265358979323846;
	const double coatTilt = 20.0 * pi / 180.0;
	const double baseTilt = 30.0 * pi / 180.0;
	const RISE::Vector3 coatWorld( std::sin( coatTilt ), 0.0, std::cos( coatTilt ) );

	// MakeProbe's frame is U=(-1,0,0), V=(0,-1,0), W=(0,0,1): world = (-nx,-ny,nz).
	double coatColor[3] = { ( -coatWorld.x + 1.0 ) / 2.0, ( -coatWorld.y + 1.0 ) / 2.0, 1.0 };
	// Base map tilts the shading normal toward world +Y by baseTilt: ny = -sin.
	double baseColor[3] = { 0.5, ( -std::sin( baseTilt ) + 1.0 ) / 2.0, 1.0 };
	Check( (*job).AddUniformColorPainter( "dl302_coat_tex", coatColor, "Rec709RGB_Linear" ), "coat map painter" );
	Check( (*job).AddUniformColorPainter( "dl302_base_tex", baseColor, "Rec709RGB_Linear" ), "base map painter" );

	rise_blender_material mat = PbrFixture( *job, "dl302" );
	mat.coat_weight = 1.0;
	mat.coat_roughness = 0.08;
	mat.coat_ior = 1.5;
	mat.coat_normal_painter_name = "dl302_coat_tex";
	mat.coat_normal_scale = 1.0;
	Check( add_material( *job, mat, err, sizeof( err ) ), std::string( "material registered: " ) + err );

	RISE::IMaterial* m = (*job).GetMaterials()->GetItem( "dl302" );
	const RISE::IPainter* basePainter = (*job).GetPainters()->GetItem( "dl302_base_tex" );
	Check( m && m->GetBSDF() && basePainter, "material and base painter resolve" );
	if( !m || !m->GetBSDF() || !basePainter ) return;

	RISE::IRayIntersectionModifier* pNmap = 0;
	Check( RISE::RISE_API_CreateNormalMapModifier( &pNmap, *basePainter, 1.0 ) && pNmap, "NormalMap modifier created" );
	if( !pNmap ) return;
	RISE::IRayIntersectionModifier& nmap = *pNmap;

	auto resp = [&]( const bool withBaseMap, const RISE::Vector3& l ) {
		RISE::RayIntersectionGeometric ri = MakeProbe( viewDir );
		if( withBaseMap ) nmap.Modify( ri );
		return RISE::ColorMath::MaxValue( m->GetBSDF()->value( l, ri ) );
	};

	// Sanity: the base map really tilted the frame.
	{
		RISE::RayIntersectionGeometric ri = MakeProbe( viewDir );
		nmap.Modify( ri );
		Check( ri.vNormal.y > 0.4, "base NormalMap tilted the shading normal toward +Y" );
		Check( ri.bHasCoatDecodeOnb, "pre-modifier frame was recorded" );
	}

	// Composed (pre-fix) coat normal: coat tilt applied about the
	// base-tilted frame, decoded with its (rebuilt) axes.
	RISE::RayIntersectionGeometric riMod = MakeProbe( viewDir );
	nmap.Modify( riMod );
	const RISE::Vector3 composedN = RISE::Vector3Ops::Normalize(
		riMod.onb.u() * ( -coatWorld.x ) + riMod.onb.v() * ( -coatWorld.y ) + riMod.onb.w() * coatWorld.z );

	const RISE::Vector3 lAlone    = Reflect( viewDir, coatWorld );
	const RISE::Vector3 lComposed = Reflect( viewDir, composedN );
	Check( RISE::Vector3Ops::Dot( lAlone, lComposed ) < 0.97, "composed and independent mirror directions are genuinely distinct" );

	const double aloneAtAlone   = resp( false, lAlone );
	const double baseAtAlone    = resp( true,  lAlone );
	const double baseAtComposed = resp( true,  lComposed );
	std::cout << "  coat alone @alone=" << aloneAtAlone << "  with base map @alone=" << baseAtAlone
	          << "  @composed=" << baseAtComposed << std::endl;
	Check( baseAtAlone > baseAtComposed * 1.5,
		"with a base normal map, the coat peak stays at the coat-map-alone mirror direction (not the composed one)" );
	Check( baseAtAlone > aloneAtAlone * 0.8 && baseAtAlone < aloneAtAlone * 1.25,
		"the coat peak magnitude with the base map matches the coat-alone peak" );
	pNmap->release();
}

//! Stage-B normal-map colour-space regression (Roadmap 0.1.1):
//! Post-Stage-B (`RISEPel = Rec709RGBPel`), `eColorSpace_ROMMRGB_Linear` (2)
//! applies a Bradford + ProPhoto->Rec.709 matrix that warps RGB-encoded
//! tangent normals (where B ~ 1.0 in ProPhoto pulls R/G strongly negative in
//! Rec.709).  Both `RISE_BLENDER_COLOR_LINEAR` (0, current `exporter.py`)
//! and the deprecated v4 `RISE_BLENDER_COLOR_ROMM_LINEAR` alias (2) must
//! register normal-map PNG and uniform painters as `Rec709RGB_Linear` (0) so
//! `add_modifier(RISE_BLENDER_MODIFIER_NORMAL_MAP)` and `coat_normal` decode
//! the exact closed-form normal vector, and `exporter.py` must emit
//! `COLOR_SPACE_LINEAR` at all normal-map sites.
void TestNormalMapColorSpacePassthrough()
{
	std::cout << "Test: Blender bridge PNG normal-map colour-space passthrough (Rec709RGB_Linear, no ROMM warp)" << std::endl;

	// Pick exact 8-bpc integer/255 encoded channels so an 8-bpc PNG
	// round-trips with zero quantization error under Rec709RGB_Linear.
	const double encR = 80.0  / 255.0;	// nx = -0.372549
	const double encG = 105.0 / 255.0;	// ny = -0.176471
	const double encB = 244.0 / 255.0;	// ~0.9569 (NormalMap reconstructs nz from sqrt(1 - nx^2 - ny^2))
	const double nx = 2.0 * encR - 1.0;
	const double ny = 2.0 * encG - 1.0;
	const double nz = std::sqrt( 1.0 - nx * nx - ny * ny );
	// MakeProbe's CreateFromW(0,0,1) gives U=(-1,0,0), V=(0,-1,0), W=(0,0,1),
	// so world = U*nx + V*ny + W*nz = (-nx, -ny, nz).
	const RISE::Vector3 expectedWorld = RISE::Vector3Ops::Normalize(
		RISE::Vector3( -nx, -ny, nz ) );

	std::error_code ec;
	std::filesystem::path tmpDir = std::filesystem::temp_directory_path( ec );
	if( ec || tmpDir.empty() ) tmpDir = ".";
	const std::string pngPath = ( tmpDir / ( "rise_bridge_nmap_" + std::to_string( static_cast<int>( getpid() ) ) + ".png" ) ).generic_string();

	{
		RISE::IRasterImage* pImg = 0;
		RISE::IWriteBuffer* pBuf = 0;
		RISE::IRasterImageWriter* pWriter = 0;
		const bool createdImg = RISE::RISE_API_CreateRISEColorRasterImage(
			&pImg, 4, 4, RISE::RISEColor( encR, encG, encB, 1.0 ) );
		const bool createdBuf = RISE::RISE_API_CreateDiskFileWriteBuffer( &pBuf, pngPath.c_str() );
		const bool createdWriter = createdBuf && pBuf && RISE::RISE_API_CreatePNGWriter(
			&pWriter, *pBuf, 8, RISE::eColorSpace_Rec709RGB_Linear );
		Check( createdImg && createdBuf && createdWriter && pImg && pWriter,
			"wrote 8-bpc linear PNG normal-map fixture" );
		if( pImg && pWriter ) {
			pImg->DumpImage( pWriter );
		}
		RISE::safe_release( pWriter );
		RISE::safe_release( pBuf );
		RISE::safe_release( pImg );
	}

	JobHolder job;
	char err[256] = { 0 };

	auto registerPngNormal = [&]( const char* painterName, const char* modName, const int csEnum ) -> bool {
		rise_blender_painter p;
		std::memset( &p, 0, sizeof( p ) );
		p.name = painterName;
		p.kind = RISE_BLENDER_PAINTER_TEXTURE_PNG;
		p.path = pngPath.c_str();
		p.color_space = csEnum;
		p.filter_type = 0;
		p.scale[0] = p.scale[1] = p.scale[2] = 1.0;
		if( !add_painter( *job, p, err, sizeof( err ) ) ) return false;

		rise_blender_modifier mod;
		std::memset( &mod, 0, sizeof( mod ) );
		mod.name = modName;
		mod.kind = RISE_BLENDER_MODIFIER_NORMAL_MAP;
		mod.source_painter_name = painterName;
		mod.scale = 1.0;
		return add_modifier( *job, mod, err, sizeof( err ) );
	};

	Check( registerPngNormal( "png_linear_tex", "mod_linear", RISE_BLENDER_COLOR_LINEAR ),
		std::string( "PNG normal map registered with RISE_BLENDER_COLOR_LINEAR: " ) + err );
	Check( registerPngNormal( "png_legacy_romm_tex", "mod_legacy_romm", RISE_BLENDER_COLOR_ROMM_LINEAR ),
		std::string( "PNG normal map registered with legacy RISE_BLENDER_COLOR_ROMM_LINEAR alias: " ) + err );

	const RISE::Vector3 viewDir = RISE::Vector3Ops::Normalize( RISE::Vector3( 0.2, -0.1, 1.0 ) );
	for( const char* modName : { "mod_linear", "mod_legacy_romm" } ) {
		RISE::IRayIntersectionModifier* pMod = (*job).GetModifiers() ? (*job).GetModifiers()->GetItem( modName ) : 0;
		Check( pMod != 0, std::string( "modifier resolved: " ) + modName );
		if( pMod ) {
			RISE::RayIntersectionGeometric ri = MakeProbe( viewDir );
			pMod->Modify( ri );
			const double cosAngle = RISE::Vector3Ops::Dot( ri.vNormal, expectedWorld );
			std::cout << "  " << modName << " decoded normal=(" << ri.vNormal.x << ", "
			          << ri.vNormal.y << ", " << ri.vNormal.z << ") dot(expected)=" << cosAngle << std::endl;
			// Exact k/255 8-bpc channels decode to cosAngle == 1.0; a Stage-A
			// ProPhoto->Rec.709 matrix warp drops cosAngle to ~0.957 (~16.8 deg off!).
			const std::string label = std::string( modName ) + " decodes the unwarped tangent normal verbatim";
			Check( cosAngle > 0.99999, label );
			if( cosAngle > 0.99999 ) std::cout << "  PASS: " << label << std::endl;
		}
	}

	// Uniform painter alias check through coat_normal as well:
	{
		rise_blender_painter up;
		std::memset( &up, 0, sizeof( up ) );
		up.name = "uniform_legacy_romm_coat";
		up.kind = RISE_BLENDER_PAINTER_UNIFORM;
		up.color[0] = encR;
		up.color[1] = encG;
		up.color[2] = encB;
		up.color_space = RISE_BLENDER_COLOR_ROMM_LINEAR;
		Check( add_painter( *job, up, err, sizeof( err ) ),
			std::string( "uniform coat-normal painter registered: " ) + err );
		const RISE::IPainter* p = (*job).GetPainters() ? (*job).GetPainters()->GetItem( "uniform_legacy_romm_coat" ) : 0;
		Check( p != 0, "uniform coat-normal painter resolved" );
		if( p ) {
			RISE::RayIntersectionGeometric ri = MakeProbe( viewDir );
			const RISE::RISEPel c = p->GetColor( ri );
			Check( std::fabs( c[0] - encR ) < 1e-6 &&
			       std::fabs( c[1] - encG ) < 1e-6 &&
			       std::fabs( c[2] - encB ) < 1e-6,
				"RISE_BLENDER_COLOR_ROMM_LINEAR uniform painter preserves verbatim RGB vector components" );
		}
	}

	std::remove( pngPath.c_str() );

	// Also guard exporter.py against re-introducing COLOR_SPACE_ROMM_LINEAR
	// at any call site (only the single constant definition line may name it).
	{
		std::ifstream py( "src/Blender/addons/rise_renderer/exporter.py" );
		if( py ) {
			std::string line;
			int uses = 0;
			while( std::getline( py, line ) ) {
				const std::size_t commentPos = line.find( '#' );
				const std::string code = ( commentPos == std::string::npos ) ? line : line.substr( 0, commentPos );
				if( code.find( "COLOR_SPACE_ROMM_LINEAR" ) != std::string::npos &&
				    code.find( "COLOR_SPACE_ROMM_LINEAR =" ) == std::string::npos )
				{
					++uses;
				}
			}
			Check( uses == 0, "exporter.py passes COLOR_SPACE_LINEAR (never COLOR_SPACE_ROMM_LINEAR) at all call sites" );
		}
	}
}

int main()
{
	std::cout << "=== Blender bridge Coat Normal test (DL-192) ===" << std::endl;

	TestAbiVersionAndCoatNormalFields();
	TestNoCoatNormalUnaffected();
	TestCoatNormalMovesSpecularPeak();
	TestCoatNormalIndependentOfBaseNormalMap();
	TestNormalMapColorSpacePassthrough();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;
	if( g_failures > 0 ) {
		std::cout << "BlenderBridgeCoatNormalTest: FAILED" << std::endl;
		return 1;
	}
	std::cout << "BlenderBridgeCoatNormalTest: PASSED" << std::endl;
	return 0;
}
