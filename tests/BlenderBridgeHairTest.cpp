//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeHairTest.cpp - Contract test for the HAIR half of the
//    Blender native bridge (ABI v9): `rise_blender_hair_material` /
//    `rise_blender_hair_object` and the two translation functions that
//    consume them, in src/Blender/native/rise_blender_bridge.cpp.
//    Slice P2-D of the hair/fur arc, docs/HAIR_FUR_DESIGN.md; the
//    Blender-side mapping contract is docs/BLENDER_MATERIAL_TRANSLATION.md.
//
//  WHY THIS TEST INCLUDES A .cpp, WHICH IS NOT A THING DONE LIGHTLY.
//  The bridge is built as a standalone shared library by its own
//  Makefile (src/Blender/native/Makefile), and everything inside it
//  except the six `extern "C"` entry points lives in an anonymous
//  namespace.  There are exactly three ways to test the hair
//  translation, and the other two are worse:
//
//    (a) dlopen the built .dylib and call `rise_blender_render_scene`.
//        That needs the bridge built (it is not part of `make tests`),
//        and it can only exercise hair by driving a FULL RENDER -- a
//        camera, a film, a rasterizer and a frame's worth of wall clock
//        to assert that a material got registered.
//    (b) Export the hair helpers from the bridge with test-only
//        `extern "C"` symbols, i.e. widen a shipping ABI for a test.
//    (c) Compile the bridge into this test's translation unit, which
//        makes its anonymous-namespace functions ordinary file-scope
//        functions here and lets the test call the REAL, SHIPPING
//        `add_hair_material` / `add_hair_object` / `pack_warnings` with
//        no duplicated logic and no ABI widening.
//
//  (c) is what this file does.  The cost is honest and bounded: the
//  bridge compiles twice (once into its .dylib, once here) and this
//  test binary carries the bridge's `extern "C"` entry points it never
//  calls.  The benefit is that the thing under test is the thing that
//  ships -- a re-implementation of the tier mapping in a test would
//  agree with itself forever while the bridge drifted.  That "thing
//  that ships" claim is configuration-approximate, not exact: the
//  dylib's own Makefile (src/Blender/native/Makefile) builds this
//  translation unit with `-std=c++17` and, when an OpenVDB install is
//  found, `-DRISE_BLENDER_ENABLE_OPENVDB`, while this test TU builds it
//  with `-std=gnu++17` and no OpenVDB define -- every VDB-gated region
//  is media-only, so hair is unaffected either way.
//
//  The five groups:
//
//    1. THE ABI ITSELF.  The version constant and `rise_blender_api_
//       version()` agree and read 9; the v9 scene fields are APPENDED
//       (every v8 field keeps its offset), which is what lets a stale
//       add-on fail on the version check rather than on garbage.
//
//    2. EACH COLOUR TIER REACHES A REAL HairMaterial.  Melanin,
//       sigma_a and color each register a material whose GetBSDF() is a
//       real HairBRDF, and each produces a DIFFERENT reflectance -- so
//       the test would catch a tier mapping that silently bound the
//       wrong slot as well as one that bound nothing.
//
//    3. THE MELANIN PARITY RESCALE, ON AND OFF.  The rescale is the
//       decision this slice owns (docs/BLENDER_MATERIAL_TRANSLATION.md's
//       units disclosure), so it is checked against its own definition
//       rather than a golden number: a material at concentration c with
//       the rescale ON must be indistinguishable from one at c x the
//       published coefficient ratio with it OFF, and strictly darker
//       than one at c with it OFF.  Both pigments, plus the degenerate
//       c = 0 case where the rescale must be a no-op.
//
//    4. A GROOM END TO END.  A `.hair` file written by this test (an
//       independent writer -- see MakeHairFile) becomes a registered
//       HairGeometry under `<object>::hairgeom` with the strand count
//       the file declares, bound to a registered object carrying the
//       transform and visibility the struct asked for.
//
//    5. EVERY FAILURE IS NON-FATAL AND NAMED.  A missing file, a
//       corrupt file, a non-positive width multiplier, an unresolvable
//       material, an unknown tier, a non-finite parameter, a negative
//       concentration and a nameless struct each: return false, record
//       a warning that names the offending object/material, register
//       NOTHING, and leave the job usable.  Plus `pack_warnings`'s
//       joining and its truncation note.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
	#include <process.h>
	#include <io.h>
	#define getpid _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

// The unit under test.  See the banner for why this is a .cpp include.
#include "../src/Blender/native/rise_blender_bridge.cpp"

#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/HairBSDF.h"
#include "../src/Library/Materials/HairMaterial.h"

using RISE::Implementation::HairBRDF;
using RISE::Implementation::HairGeometry;

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

// ============================================================
//  Fixtures
// ============================================================

std::string TempPath( const std::string& tag )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) {
		dir += "/";
	}
	char pid[32];
	std::snprintf( pid, sizeof( pid ), "%d", static_cast<int>( ::getpid() ) );
	return dir + "rise_bridgehair_" + tag + "_" + pid;
}

void PutU32( std::vector<unsigned char>& b, const unsigned int v )
{
	for( int i = 0; i < 4; ++i ) {
		b.push_back( static_cast<unsigned char>( ( v >> ( 8 * i ) ) & 0xffu ) );
	}
}

void PutU16( std::vector<unsigned char>& b, const unsigned int v )
{
	b.push_back( static_cast<unsigned char>( v & 0xffu ) );
	b.push_back( static_cast<unsigned char>( ( v >> 8 ) & 0xffu ) );
}

void PutF32( std::vector<unsigned char>& b, const float f )
{
	unsigned int bits = 0;
	std::memcpy( &bits, &f, 4 );
	PutU32( b, bits );
}

//! An INDEPENDENT writer of the Cem Yuksel `.hair` format, packed
//! straight from the specification table in HairFileLoader.h's header.
//! Shares no code with the reader under test, so a groom that comes
//! back with the right strand count really did round-trip through the
//! byte layout.  `badMagic` produces the corrupt-file fixture.
//! Returns the path written (empty string on an I/O failure).
std::string MakeHairFile( const std::string& tag, const bool badMagic, const unsigned int numStrands )
{
	const unsigned int pointsPerStrand = 4;
	const unsigned int segments = pointsPerStrand - 1;
	const unsigned int numPoints = numStrands * pointsPerStrand;

	std::vector<unsigned char> b;
	b.push_back( 'H' ); b.push_back( 'A' ); b.push_back( 'I' );
	b.push_back( badMagic ? 'X' : 'R' );
	PutU32( b, numStrands );
	PutU32( b, numPoints );
	PutU32( b, ( 1u << 0 ) | ( 1u << 1 ) | ( 1u << 2 ) );	// segments + points + thickness
	PutU32( b, segments );
	PutF32( b, 0.01f );							// default thickness
	PutF32( b, 0.0f );							// default transparency
	PutF32( b, 1.0f ); PutF32( b, 1.0f ); PutF32( b, 1.0f );	// default colour
	{
		const char* info = "RISE BlenderBridgeHairTest fixture";
		const size_t infoLength = std::strlen( info );
		for( unsigned int i = 0; i < 88; ++i ) {
			b.push_back( static_cast<unsigned char>( i < infoLength ? info[i] : 0 ) );
		}
	}

	for( unsigned int s = 0; s < numStrands; ++s ) {
		PutU16( b, segments );
	}

	for( unsigned int s = 0; s < numStrands; ++s ) {
		for( unsigned int p = 0; p < pointsPerStrand; ++p ) {
			PutF32( b, static_cast<float>( s ) * 0.05f );
			PutF32( b, 0.0f );
			PutF32( b, static_cast<float>( p ) * 0.1f );	// grows along +Z, strictly positive length
		}
	}

	for( unsigned int i = 0; i < numPoints; ++i ) {
		PutF32( b, 0.01f );
	}

	const std::string path = TempPath( tag ) + ".hair";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	if( !f.is_open() ) {
		return std::string();
	}
	f.write( reinterpret_cast<const char*>( &b[0] ), static_cast<std::streamsize>( b.size() ) );
	f.close();
	return path;
}

//! A synthetic fibre hit, enough for HairBRDF::albedo() -- which reads
//! the bound painters and the ONB, nothing scene-side.  Same
//! construction HairBSDFTest uses.
RISE::RayIntersectionGeometric MakeFibreHit()
{
	const RISE::Vector3 wo( 0.0, 1.0, 0.0 );
	const RISE::Ray inRay( RISE::Point3( wo.x, wo.y, wo.z ), -wo );
	RISE::RasterizerState rs = { 0, 0 };
	RISE::RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = RISE::Point3( 0, 0, 0 );
	ri.vNormal     = RISE::Vector3( 0, 0, 1 );
	ri.vGeomNormal = RISE::Vector3( 0, 0, 1 );
	ri.onb.CreateFromWU( RISE::Vector3( 0, 0, 1 ), RISE::Vector3( 1, 0, 0 ) );
	ri.ptCoord  = RISE::Point2( 0.5, 0.5 );
	ri.ptCoord1 = ri.ptCoord;
	return ri;
}

rise_blender_hair_material MelaninMaterial(
	const char* name, const float eumelanin, const float pheomelanin, const int rescale )
{
	rise_blender_hair_material m;
	std::memset( &m, 0, sizeof( m ) );
	m.name = name;
	m.tier = RISE_BLENDER_HAIR_TIER_MELANIN;
	m.eumelanin = eumelanin;
	m.pheomelanin = pheomelanin;
	m.apply_melanin_parity_rescale = rescale;
	m.beta_m = 0.3f;
	m.beta_n = 0.3f;
	m.alpha_degrees = 2.0f;
	m.ior = 1.55f;
	return m;
}

rise_blender_hair_object HairObject(
	const char* name, const char* file, const char* material )
{
	rise_blender_hair_object o;
	std::memset( &o, 0, sizeof( o ) );
	o.name = name;
	o.file_path = file;
	o.material_name = material;
	// Identity, row major.
	for( int i = 0; i < 16; ++i ) {
		o.transform[i] = ( i % 5 == 0 ) ? 1.0f : 0.0f;
	}
	o.width_root_scale = 1.0f;
	o.width_tip_scale = 1.0f;
	o.casts_shadows = 1;
	o.receives_shadows = 1;
	o.visible = 1;
	return o;
}

const HairBRDF* BrdfOf( RISE::IJobPriv& job, const char* name )
{
	RISE::IMaterial* material = job.GetMaterials() ? job.GetMaterials()->GetItem( name ) : 0;
	if( !material ) {
		return 0;
	}
	return dynamic_cast<const HairBRDF*>( material->GetBSDF() );
}

//! True when any recorded warning mentions `needle`.
bool WarningsMention( const std::vector<std::string>& warnings, const std::string& needle )
{
	for( size_t i = 0; i < warnings.size(); ++i ) {
		if( warnings[i].find( needle ) != std::string::npos ) {
			return true;
		}
	}
	return false;
}

//! Runs `body` with stdout redirected to a temp file -- GlobalLog's
//! console sink carries eLog_Warning / eLog_Error, and the negative
//! cases below deliberately provoke those.  Same fd-dup technique as
//! HairMaterialChunkTest.  Returns whatever `body` returned.
template<class Body>
bool Quietly( Body body )
{
	const std::string capPath = TempPath( "stdout" ) + ".txt";

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capPath.c_str(), "w" );
	if( capFile ) {
		RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );
	}

	const bool result = body();

	std::fflush( stdout );
	if( savedFd >= 0 ) {
		RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) );
		RISE_TEST_CLOSE( savedFd );
	}
	if( capFile ) {
		std::fclose( capFile );
	}
	remove( capPath.c_str() );
	return result;
}

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

// ============================================================
//  1. The ABI itself
// ============================================================

void TestAbiVersionAndLayout()
{
	std::cout << "Test: ABI version 9 and an append-only v9 scene struct" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 9, "RISE_BLENDER_API_VERSION is 9" );
	Check( rise_blender_api_version() == RISE_BLENDER_API_VERSION,
		"rise_blender_api_version() reports the compiled-in constant" );

	// The v9 additions must sit AFTER every v8 field.  If they were
	// inserted mid-struct, a v8 add-on would not merely fail the version
	// check -- a caller that skipped the check would read shifted
	// garbage for fields it thinks it knows.
	Check( offsetof( rise_blender_scene, hair_materials ) >
	       offsetof( rise_blender_scene, world_radiance_is_background ),
		"scene.hair_materials is appended after the last v8 field" );
	Check( offsetof( rise_blender_scene, num_hair_objects ) >
	       offsetof( rise_blender_scene, hair_objects ),
		"scene hair counts follow their arrays" );
	Check( offsetof( rise_blender_render_result, warnings ) >
	       offsetof( rise_blender_render_result, resolve_reason ),
		"result.warnings is appended after the v8 auto-dispatcher fields" );

	// The tier tags the add-on maps its `tier` strings onto.
	Check( RISE_BLENDER_HAIR_TIER_MELANIN == 0 &&
	       RISE_BLENDER_HAIR_TIER_SIGMA_A == 1 &&
	       RISE_BLENDER_HAIR_TIER_COLOR   == 2,
		"hair tier tags are 0 / 1 / 2 (bridge.py's _HAIR_TIER_BY_NAME depends on these)" );
}

// ============================================================
//  2. Each colour tier reaches a real HairMaterial
// ============================================================

void TestEachTierRegisters()
{
	std::cout << "Test: each of the three colour tiers registers a real HairMaterial" << std::endl;

	JobHolder job;
	if( !job.Valid() ) {
		Check( false, "created a job" );
		return;
	}

	std::vector<std::string> warnings;

	rise_blender_hair_material melanin = MelaninMaterial( "hair_melanin", 1.3f, 0.0f, 0 );
	Check( add_hair_material( *job, melanin, warnings ), "melanin tier: add_hair_material succeeded" );

	rise_blender_hair_material sigma;
	std::memset( &sigma, 0, sizeof( sigma ) );
	sigma.name = "hair_sigma";
	sigma.tier = RISE_BLENDER_HAIR_TIER_SIGMA_A;
	sigma.sigma_a[0] = 0.245f; sigma.sigma_a[1] = 0.46f; sigma.sigma_a[2] = 1.6f;
	sigma.beta_m = 0.3f; sigma.beta_n = 0.3f; sigma.alpha_degrees = 2.0f; sigma.ior = 1.55f;
	Check( add_hair_material( *job, sigma, warnings ), "sigma_a tier: add_hair_material succeeded" );

	double blonde[3] = { 0.7, 0.6, 0.4 };
	Check( (*job).AddUniformColorPainter( "hair_tint", blonde, "Rec709RGB_Linear" ),
		"registered a colour painter for the colour tier" );

	rise_blender_hair_material colour;
	std::memset( &colour, 0, sizeof( colour ) );
	colour.name = "hair_color";
	colour.tier = RISE_BLENDER_HAIR_TIER_COLOR;
	colour.color_painter_name = "hair_tint";
	colour.beta_m = 0.3f; colour.beta_n = 0.3f; colour.alpha_degrees = 2.0f; colour.ior = 1.55f;
	Check( add_hair_material( *job, colour, warnings ), "color tier: add_hair_material succeeded" );

	Check( warnings.empty(), "three good materials produced no warnings" );

	const HairBRDF* bMelanin = BrdfOf( *job, "hair_melanin" );
	const HairBRDF* bSigma   = BrdfOf( *job, "hair_sigma" );
	const HairBRDF* bColour  = BrdfOf( *job, "hair_color" );

	Check( bMelanin != 0, "melanin tier: GetBSDF() is a real HairBRDF" );
	Check( bSigma   != 0, "sigma_a tier: GetBSDF() is a real HairBRDF" );
	Check( bColour  != 0, "color tier: GetBSDF() is a real HairBRDF" );

	if( bMelanin && bSigma && bColour ) {
		const RISE::RayIntersectionGeometric ri = MakeFibreHit();
		const RISE::RISEPel aMelanin = bMelanin->albedo( ri );
		const RISE::RISEPel aSigma   = bSigma->albedo( ri );
		const RISE::RISEPel aColour  = bColour->albedo( ri );

		// A tier mapping that bound the wrong slot (or fell through to
		// the "misconfigured" fallback) would collapse these onto each
		// other; three deliberately different colour models must not
		// agree by accident.
		Check( std::fabs( aMelanin[1] - aSigma[1] )  > 1e-4, "melanin and sigma_a tiers differ in reflectance" );
		Check( std::fabs( aMelanin[1] - aColour[1] ) > 1e-4, "melanin and color tiers differ in reflectance" );
		Check( std::fabs( aSigma[1]   - aColour[1] ) > 1e-4, "sigma_a and color tiers differ in reflectance" );

		for( int c = 0; c < 3; ++c ) {
			Check( aMelanin[(unsigned int)c] > 0.0 && aMelanin[(unsigned int)c] <= 1.0,
				"melanin tier reflectance is a physical [0,1] value" );
		}
	}
}

// ============================================================
//  3. The melanin parity rescale
// ============================================================

void TestMelaninParityRescale()
{
	std::cout << "Test: the Blender-parity melanin rescale, on and off" << std::endl;

	// Locked to the coefficient table in
	// docs/BLENDER_MATERIAL_TRANSLATION.md's hair section: Cycles' green
	// sigma_a coefficient over RISE's, per pigment.
	Check( std::fabs( kEumelaninBlenderParityScale   - ( 0.841 / 0.697 ) ) < 1e-12,
		"eumelanin parity scale is Cycles-G / RISE-G = 0.841 / 0.697" );
	Check( std::fabs( kPheomelaninBlenderParityScale - ( 0.733 / 0.400 ) ) < 1e-12,
		"pheomelanin parity scale is Cycles-G / RISE-G = 0.733 / 0.400" );

	JobHolder job;
	if( !job.Valid() ) {
		Check( false, "created a job" );
		return;
	}

	std::vector<std::string> warnings;
	const RISE::RayIntersectionGeometric ri = MakeFibreHit();

	// --- eumelanin -------------------------------------------------
	const float eu = 1.0f;
	const float euScaled = static_cast<float>( double( eu ) * kEumelaninBlenderParityScale );

	rise_blender_hair_material euOn       = MelaninMaterial( "eu_on",        eu,       0.0f, 1 );
	rise_blender_hair_material euOff      = MelaninMaterial( "eu_off",       eu,       0.0f, 0 );
	rise_blender_hair_material euOffEquiv = MelaninMaterial( "eu_off_equiv", euScaled, 0.0f, 0 );
	euOn.pheomelanin = 0.0f;
	euOff.pheomelanin = 0.0f;
	euOffEquiv.pheomelanin = 0.0f;

	Check( add_hair_material( *job, euOn, warnings ) &&
	       add_hair_material( *job, euOff, warnings ) &&
	       add_hair_material( *job, euOffEquiv, warnings ),
		"eumelanin A/B materials registered" );

	const HairBRDF* bOn       = BrdfOf( *job, "eu_on" );
	const HairBRDF* bOff      = BrdfOf( *job, "eu_off" );
	const HairBRDF* bOffEquiv = BrdfOf( *job, "eu_off_equiv" );

	if( bOn && bOff && bOffEquiv ) {
		const RISE::RISEPel aOn       = bOn->albedo( ri );
		const RISE::RISEPel aOff      = bOff->albedo( ri );
		const RISE::RISEPel aOffEquiv = bOffEquiv->albedo( ri );

		for( unsigned int c = 0; c < 3; ++c ) {
			// The rescale IS a multiplication of the concentration, so
			// "on at c" and "off at c x ratio" must be the same
			// material, not merely similar.
			Check( std::fabs( aOn[c] - aOffEquiv[c] ) < 1e-9,
				"eumelanin: rescale ON at c equals rescale OFF at c x 0.841/0.697" );
			// ... and it must actually do something: more absorption is
			// a darker fibre.
			Check( aOn[c] < aOff[c],
				"eumelanin: the rescale darkens the fibre relative to rescale OFF" );
		}
	} else {
		Check( false, "eumelanin A/B materials produced HairBRDFs" );
	}

	// --- pheomelanin -----------------------------------------------
	const float ph = 1.0f;
	const float phScaled = static_cast<float>( double( ph ) * kPheomelaninBlenderParityScale );

	rise_blender_hair_material phOn       = MelaninMaterial( "ph_on",        0.0f, ph,       1 );
	rise_blender_hair_material phOffEquiv = MelaninMaterial( "ph_off_equiv", 0.0f, phScaled, 0 );

	Check( add_hair_material( *job, phOn, warnings ) &&
	       add_hair_material( *job, phOffEquiv, warnings ),
		"pheomelanin A/B materials registered" );

	const HairBRDF* bPhOn       = BrdfOf( *job, "ph_on" );
	const HairBRDF* bPhOffEquiv = BrdfOf( *job, "ph_off_equiv" );
	if( bPhOn && bPhOffEquiv ) {
		const RISE::RISEPel aOn       = bPhOn->albedo( ri );
		const RISE::RISEPel aOffEquiv = bPhOffEquiv->albedo( ri );
		for( unsigned int c = 0; c < 3; ++c ) {
			Check( std::fabs( aOn[c] - aOffEquiv[c] ) < 1e-9,
				"pheomelanin: rescale ON at c equals rescale OFF at c x 0.733/0.400" );
		}
	} else {
		Check( false, "pheomelanin A/B materials produced HairBRDFs" );
	}

	// --- the degenerate case ---------------------------------------
	// Melanin 0 (an unpigmented white groom) must not move: scaling
	// zero is zero, and an artist who dialled the slider to 0 gets the
	// same fibre either way.
	rise_blender_hair_material zeroOn  = MelaninMaterial( "zero_on",  0.0f, 0.0f, 1 );
	rise_blender_hair_material zeroOff = MelaninMaterial( "zero_off", 0.0f, 0.0f, 0 );
	Check( add_hair_material( *job, zeroOn, warnings ) &&
	       add_hair_material( *job, zeroOff, warnings ),
		"zero-concentration A/B materials registered" );

	const HairBRDF* bZeroOn  = BrdfOf( *job, "zero_on" );
	const HairBRDF* bZeroOff = BrdfOf( *job, "zero_off" );
	if( bZeroOn && bZeroOff ) {
		const RISE::RISEPel aOn  = bZeroOn->albedo( ri );
		const RISE::RISEPel aOff = bZeroOff->albedo( ri );
		for( unsigned int c = 0; c < 3; ++c ) {
			Check( std::fabs( aOn[c] - aOff[c] ) < 1e-12,
				"zero concentration: the rescale is a no-op" );
		}
	} else {
		Check( false, "zero-concentration materials produced HairBRDFs" );
	}

	Check( warnings.empty(), "the rescale A/B materials produced no warnings" );
}

// ============================================================
//  4. A groom, end to end
// ============================================================

void TestHairObjectEndToEnd()
{
	std::cout << "Test: a .hair file becomes a registered geometry bound to an object" << std::endl;

	const unsigned int numStrands = 3;
	const std::string path = MakeHairFile( "good", false, numStrands );
	if( path.empty() ) {
		Check( false, "wrote the .hair fixture" );
		return;
	}

	JobHolder job;
	if( !job.Valid() ) {
		Check( false, "created a job" );
		remove( path.c_str() );
		return;
	}

	std::vector<std::string> warnings;
	rise_blender_hair_material material = MelaninMaterial( "groom_mat", 1.3f, 0.0f, 1 );
	Check( add_hair_material( *job, material, warnings ), "the groom's material registered" );

	rise_blender_hair_object object = HairObject( "groom", path.c_str(), "groom_mat" );
	// A non-identity placement: translate by (2, 3, 4).  Row major, so
	// the translation column is indices 3 / 7 / 11.
	object.transform[3]  = 2.0f;
	object.transform[7]  = 3.0f;
	object.transform[11] = 4.0f;
	object.width_root_scale = 2.0f;
	object.width_tip_scale = 0.5f;

	Check( add_hair_object( *job, object, warnings ), "add_hair_object succeeded" );
	Check( warnings.empty(), "a good groom produced no warnings" );

	RISE::IGeometry* geometry = (*job).GetGeometries()->GetItem( "groom::hairgeom" );
	Check( geometry != 0, "the geometry registered under <object>::hairgeom" );

	const HairGeometry* hair = dynamic_cast<const HairGeometry*>( geometry );
	Check( hair != 0, "the registered geometry is a HairGeometry" );
	if( hair ) {
		Check( hair->numStrands() == numStrands,
			"every strand in the file reached the groom" );
	}

	RISE::IObjectPriv* placed = (*job).GetObjects()->GetItem( "groom" );
	Check( placed != 0, "the hair object registered under its own name" );
	if( placed ) {
		// The bridge routes hair through the SAME add_object the mesh
		// path uses; confirm the transform actually landed by checking
		// the world-space bounding box moved with it.  The fixture's
		// strands live in x in [0, 0.1], z in [0, 0.3] around the
		// origin, so a +(2,3,4) translation must put the whole box in
		// the positive octant well away from zero.
		const RISE::BoundingBox box = placed->getBoundingBox();
		Check( box.ll.x > 1.0 && box.ll.y > 2.0 && box.ll.z > 3.0,
			"the object's world bounding box carries the struct's transform" );
		Check( placed->IsWorldVisible(), "the object honours the struct's visible flag" );
	}

	remove( path.c_str() );
}

// ============================================================
//  5. Every failure is non-fatal and named
// ============================================================

void TestFailuresAreNonFatalAndNamed()
{
	std::cout << "Test: hair failures skip one groom, warn by name, and register nothing" << std::endl;

	JobHolder job;
	if( !job.Valid() ) {
		Check( false, "created a job" );
		return;
	}

	std::vector<std::string> warnings;
	rise_blender_hair_material material = MelaninMaterial( "ok_mat", 1.3f, 0.0f, 1 );
	Check( add_hair_material( *job, material, warnings ), "the baseline material registered" );

	// --- a missing file --------------------------------------------
	{
		warnings.clear();
		const std::string missing = TempPath( "does_not_exist" ) + ".hair";
		rise_blender_hair_object object = HairObject( "missing_groom", missing.c_str(), "ok_mat" );
		const bool ok = Quietly( [&]() { return add_hair_object( *job, object, warnings ); } );
		Check( !ok, "a missing .hair file is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "missing_groom" ),
			"the missing-file warning names the object" );
		Check( (*job).GetGeometries()->GetItem( "missing_groom::hairgeom" ) == 0,
			"a missing file registers no geometry" );
		Check( (*job).GetObjects()->GetItem( "missing_groom" ) == 0,
			"a missing file registers no object" );
	}

	// --- a corrupt file --------------------------------------------
	{
		warnings.clear();
		const std::string corrupt = MakeHairFile( "corrupt", true, 3 );
		Check( !corrupt.empty(), "wrote the corrupt .hair fixture" );
		rise_blender_hair_object object = HairObject( "corrupt_groom", corrupt.c_str(), "ok_mat" );
		const bool ok = Quietly( [&]() { return add_hair_object( *job, object, warnings ); } );
		Check( !ok, "a corrupt .hair file is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "corrupt_groom" ),
			"the corrupt-file warning names the object" );
		Check( WarningsMention( warnings, corrupt ),
			"the corrupt-file warning names the path, so the artist can find it" );
		Check( (*job).GetObjects()->GetItem( "corrupt_groom" ) == 0,
			"a corrupt file registers no object" );
		remove( corrupt.c_str() );
	}

	// --- a non-positive width multiplier ---------------------------
	{
		warnings.clear();
		const std::string good = MakeHairFile( "widths", false, 2 );
		rise_blender_hair_object object = HairObject( "zero_width", good.c_str(), "ok_mat" );
		object.width_tip_scale = 0.0f;
		const bool ok = Quietly( [&]() { return add_hair_object( *job, object, warnings ); } );
		Check( !ok, "a zero width multiplier is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "zero_width" ),
			"the width warning names the object" );
		Check( (*job).GetGeometries()->GetItem( "zero_width::hairgeom" ) == 0,
			"a bad width multiplier does not even read the file" );
		remove( good.c_str() );
	}

	// --- a material that never got created -------------------------
	{
		warnings.clear();
		const std::string good = MakeHairFile( "orphan", false, 2 );
		rise_blender_hair_object object = HairObject( "orphan_groom", good.c_str(), "no_such_material" );
		const bool ok = Quietly( [&]() { return add_hair_object( *job, object, warnings ); } );
		Check( !ok, "a groom whose material failed is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "no_such_material" ),
			"the unresolved-material warning names the material" );
		Check( (*job).GetGeometries()->GetItem( "orphan_groom::hairgeom" ) == 0,
			"an unresolvable material leaves no orphaned geometry registered" );
		remove( good.c_str() );
	}

	// --- bad material payloads -------------------------------------
	{
		warnings.clear();
		rise_blender_hair_material unknownTier = MelaninMaterial( "bad_tier", 1.0f, 0.0f, 1 );
		unknownTier.tier = 99;	// what bridge.py sends for an unrecognised tier string
		const bool ok = Quietly( [&]() { return add_hair_material( *job, unknownTier, warnings ); } );
		Check( !ok, "an unrecognised colour tier is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "bad_tier" ),
			"the unknown-tier warning names the material" );
		Check( (*job).GetMaterials()->GetItem( "bad_tier" ) == 0, "no material is registered for it" );
	}

	{
		warnings.clear();
		rise_blender_hair_material nonFinite = MelaninMaterial( "bad_beta", 1.0f, 0.0f, 1 );
		nonFinite.beta_m = std::numeric_limits<float>::quiet_NaN();
		const bool ok = Quietly( [&]() { return add_hair_material( *job, nonFinite, warnings ); } );
		Check( !ok, "a non-finite beta_m is refused before it reaches the scalar parser" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "bad_beta" ),
			"the non-finite warning names the material" );
	}

	{
		warnings.clear();
		rise_blender_hair_material negative = MelaninMaterial( "bad_conc", -1.0f, 0.0f, 1 );
		const bool ok = Quietly( [&]() { return add_hair_material( *job, negative, warnings ); } );
		Check( !ok, "a negative melanin concentration is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "bad_conc" ),
			"the negative-concentration warning names the material" );
	}

	// --- a negative sigma_a channel (would amplify energy) ---------
	{
		warnings.clear();
		rise_blender_hair_material negativeSigma;
		std::memset( &negativeSigma, 0, sizeof( negativeSigma ) );
		negativeSigma.name = "bad_sigma_a";
		negativeSigma.tier = RISE_BLENDER_HAIR_TIER_SIGMA_A;
		negativeSigma.sigma_a[0] = 0.245f; negativeSigma.sigma_a[1] = -0.01f; negativeSigma.sigma_a[2] = 1.6f;
		negativeSigma.beta_m = 0.3f; negativeSigma.beta_n = 0.3f; negativeSigma.alpha_degrees = 2.0f; negativeSigma.ior = 1.55f;
		const bool ok = Quietly( [&]() { return add_hair_material( *job, negativeSigma, warnings ); } );
		Check( !ok, "a negative sigma_a channel is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "bad_sigma_a" ),
			"the negative-sigma_a warning names the material" );
		Check( (*job).GetMaterials()->GetItem( "bad_sigma_a" ) == 0,
			"no material is registered for a negative sigma_a channel" );
	}

	{
		warnings.clear();
		rise_blender_hair_material unbound;
		std::memset( &unbound, 0, sizeof( unbound ) );
		unbound.name = "bad_color";
		unbound.tier = RISE_BLENDER_HAIR_TIER_COLOR;
		unbound.color_painter_name = 0;
		unbound.beta_m = 0.3f; unbound.beta_n = 0.3f; unbound.alpha_degrees = 2.0f; unbound.ior = 1.55f;
		const bool ok = Quietly( [&]() { return add_hair_material( *job, unbound, warnings ); } );
		Check( !ok, "the colour tier with no painter bound is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "bad_color" ),
			"the unbound-colour warning names the material" );
	}

	{
		warnings.clear();
		rise_blender_hair_material nameless = MelaninMaterial( "", 1.0f, 0.0f, 1 );
		const bool ok = Quietly( [&]() { return add_hair_material( *job, nameless, warnings ); } );
		Check( !ok, "a nameless hair material is refused" );
		Check( warnings.size() == 1, "the nameless material still records a warning" );
	}

	// --- a colour painter that was never registered ----------------
	{
		warnings.clear();
		rise_blender_hair_material dangling;
		std::memset( &dangling, 0, sizeof( dangling ) );
		dangling.name = "dangling_color";
		dangling.tier = RISE_BLENDER_HAIR_TIER_COLOR;
		dangling.color_painter_name = "no_such_painter";
		dangling.beta_m = 0.3f; dangling.beta_n = 0.3f; dangling.alpha_degrees = 2.0f; dangling.ior = 1.55f;
		const bool ok = Quietly( [&]() { return add_hair_material( *job, dangling, warnings ); } );
		Check( !ok, "a colour painter name RISE cannot resolve is refused" );
		Check( warnings.size() == 1 && WarningsMention( warnings, "dangling_color" ),
			"the unresolved-painter warning names the material" );
	}

	// --- the job is still usable afterwards ------------------------
	{
		warnings.clear();
		rise_blender_hair_material fine = MelaninMaterial( "after_failures", 0.8f, 0.2f, 1 );
		Check( add_hair_material( *job, fine, warnings ),
			"a good material still registers after nine failed ones -- nothing was left half-built" );
		Check( warnings.empty(), "and it warns about nothing" );
	}
}

// ============================================================
//  5b. pack_warnings
// ============================================================

void TestPackWarnings()
{
	std::cout << "Test: pack_warnings joins, terminates and truncates" << std::endl;

	char buffer[64];

	{
		std::vector<std::string> none;
		std::memset( buffer, 'x', sizeof( buffer ) );
		pack_warnings( none, buffer, sizeof( buffer ) );
		Check( buffer[0] == '\0', "no warnings packs to an empty string" );
	}

	{
		std::vector<std::string> two;
		two.push_back( "first" );
		two.push_back( "second" );
		pack_warnings( two, buffer, sizeof( buffer ) );
		Check( std::string( buffer ) == "first\nsecond", "warnings are newline separated" );
	}

	{
		std::vector<std::string> many;
		for( int i = 0; i < 50; ++i ) {
			many.push_back( "a warning long enough to overflow a small buffer" );
		}
		std::memset( buffer, 'x', sizeof( buffer ) );
		pack_warnings( many, buffer, sizeof( buffer ) );
		const std::string packed( buffer );
		Check( packed.size() < sizeof( buffer ), "an overflowing pack stays inside the buffer" );
		Check( packed.find( "further warnings omitted" ) != std::string::npos,
			"an overflowing pack says so rather than cutting silently" );
	}

	{
		// Degenerate: a buffer too small even for the truncation note
		// must still produce a terminated string rather than scribble.
		char tiny[8];
		std::vector<std::string> many;
		many.push_back( "aaaaaaaaaaaaaaaaaaaaaaaaaaaa" );
		pack_warnings( many, tiny, sizeof( tiny ) );
		Check( std::strlen( tiny ) == sizeof( tiny ) - 1, "a tiny buffer is filled and terminated" );
	}
}

} // namespace

int main()
{
	std::cout << "=== Blender bridge hair test (ABI v9) ===" << std::endl;

	TestAbiVersionAndLayout();
	TestEachTierRegisters();
	TestMelaninParityRescale();
	TestHairObjectEndToEnd();
	TestFailuresAreNonFatalAndNamed();
	TestPackWarnings();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;

	if( g_failures == 0 ) {
		std::cout << "BlenderBridgeHairTest: PASSED" << std::endl;
		return 0;
	}

	std::cout << "BlenderBridgeHairTest: FAILED" << std::endl;
	return 1;
}
