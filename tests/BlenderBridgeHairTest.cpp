//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeHairTest.cpp - Contract test for the HAIR half of the
//    Blender native bridge (ABI v10): `rise_blender_hair_material` /
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
//  The seven groups:
//
//    1. THE ABI ITSELF.  The version constant and `rise_blender_api_
//       version()` agree and read 11; the v9 scene fields, the v10
//       hair-material fields and the v11 modifier `normalize` field are
//       APPENDED (every earlier field keeps its offset), which is what
//       lets a stale add-on fail on the version check rather than on
//       garbage.
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
//    4b. TEXTURE-DRIVEN SCALAR SLOTS (v10).  A `beta_m` driven by a
//       spatially-varying colour painter really does vary the shaded
//       result across UV, really does override the struct's numeric
//       `beta_m`, and really does register the scalar wrapper the
//       header promises -- checked against a numeric-only twin that must
//       NOT vary, and against the empty-field fallback, which must be
//       indistinguishable from pre-v10 behaviour.  Plus the non-fatal
//       missing-painter case: warn, fall back to the number for that ONE
//       slot, and still register the material.
//
//    5. EVERY FAILURE IS NON-FATAL AND NAMED.  A missing file, a
//       corrupt file, a non-positive width multiplier, an unresolvable
//       material, an unknown tier, a non-finite parameter, a negative
//       concentration and a nameless struct each: return false, record
//       a warning that names the offending object/material, register
//       NOTHING, and leave the job usable.  Plus `pack_warnings`'s
//       joining and its truncation note.
//
//    6. THE BUMP MODIFIER'S normalize FOLD (v11, P1 Phase B review,
//       docs/RELIEF_MODIFIER_DESIGN.md 7.5).  `add_modifier`'s
//       RISE_BLENDER_MODIFIER_BUMP case reaches
//       `RISE_API_CreateBumpMapModifierEx` directly (bypassing the
//       ABI-frozen, always-window-coupled `IJob::AddBumpMapModifier`
//       shim) with `normalizeGradient = modifier.normalize`.  Checked on
//       a linear-ramp height field, whose central difference is exact
//       for any window: normalize=TRUE delivers a tilt of exactly
//       strength*distance*slope, independent of `window`; normalize=FALSE
//       (the RED-PROOF) reproduces the pre-fix window-coupled fold
//       (scale*2*window), ~200x weaker at the exporter's window=0.005.
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
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Interfaces/IFunction2DManager.h"
#include "../src/Library/Interfaces/IModifierManager.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/HairBSDF.h"
#include "../src/Library/Materials/HairMaterial.h"
#include "../src/Library/Utilities/Reference.h"   // Implementation::Reference -- base for the linear-ramp IFunction2D below (P1 bump-normalize test)

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
RISE::RayIntersectionGeometric MakeFibreHitAt( const double u, const double v )
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
	ri.ptCoord  = RISE::Point2( u, v );
	ri.ptCoord1 = ri.ptCoord;
	return ri;
}

RISE::RayIntersectionGeometric MakeFibreHit()
{
	return MakeFibreHitAt( 0.5, 0.5 );
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
	std::cout << "Test: ABI version 11 and append-only v9 / v10 / v11 struct growth" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 11, "RISE_BLENDER_API_VERSION is 11" );
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

	// Same discipline for v10's three optional texture-scalar fields:
	// appended after `ior`, the last v9 field of the hair material, so
	// every v9 offset in that struct survives.
	Check( offsetof( rise_blender_hair_material, beta_m_texture_painter_name ) >
	       offsetof( rise_blender_hair_material, ior ),
		"hair_material's v10 texture fields are appended after the last v9 field" );
	Check( offsetof( rise_blender_hair_material, beta_n_texture_painter_name ) >
	       offsetof( rise_blender_hair_material, beta_m_texture_painter_name ) &&
	       offsetof( rise_blender_hair_material, ior_texture_painter_name ) >
	       offsetof( rise_blender_hair_material, beta_n_texture_painter_name ),
		"the three v10 texture fields are in the order bridge.py mirrors" );

	// Same discipline for v11's `normalize` field (P1, Phase B review):
	// appended after `window`, the last v10 field of the modifier struct.
	Check( offsetof( rise_blender_modifier, normalize ) >
	       offsetof( rise_blender_modifier, window ),
		"modifier's v11 `normalize` field is appended after the last v10 field" );

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
//  4b. Texture-driven scalar slots (v10)
// ============================================================

//! The wrapped painter is read PER HIT, so a spatially-varying source
//! must produce a spatially-varying shade.  `value()` is the
//! observable, not `albedo()`: `beta_m` is the LONGITUDINAL roughness,
//! which shapes M_p and hence the directional lobe, while the
//! closed-form multiple-scattering reflectance the OIDN AOV uses does
//! not read it at all.
void TestTextureDrivenScalarSlots()
{
	std::cout << "Test: a texture-driven beta_m varies the shade, overrides the number, and falls back safely" << std::endl;

	JobHolder job;
	if( !job.Valid() ) {
		Check( false, "created a job" );
		return;
	}

	// Two constants and a checker between them.  0.25 / 0.75 rather
	// than round decimals on purpose: both are exact in binary, so the
	// painter's double and the `%.9g` float literal the numeric
	// reference materials below travel as are the SAME number, and the
	// equality checks can be exact instead of approximate.  Registered
	// Rec709RGB_Linear, i.e. verbatim -- RISEPel IS Rec709RGBPel -- so
	// the R channel the bridge's wrapper reads is exactly 0.25 / 0.75.
	double lo[3] = { 0.25, 0.25, 0.25 };
	double hi[3] = { 0.75, 0.75, 0.75 };
	Check( (*job).AddUniformColorPainter( "rough_lo", lo, "Rec709RGB_Linear" ) &&
	       (*job).AddUniformColorPainter( "rough_hi", hi, "Rec709RGB_Linear" ) &&
	       (*job).AddCheckerPainter( "rough_checker", 0.25, "rough_lo", "rough_hi" ),
		"registered a spatially-varying roughness painter" );

	std::vector<std::string> warnings;

	// Textured.  The struct's numeric beta_m stays at MelaninMaterial's
	// 0.3 -- a value the checker never takes -- so a wiring that quietly
	// kept the number would show up rather than coincide.
	rise_blender_hair_material textured = MelaninMaterial( "tex_beta", 1.3f, 0.0f, 0 );
	textured.beta_m_texture_painter_name = "rough_checker";
	Check( add_hair_material( *job, textured, warnings ), "a texture-driven beta_m registers" );

	// The same material with no texture: the numeric beta_m, unchanged.
	rise_blender_hair_material numeric = MelaninMaterial( "num_beta", 1.3f, 0.0f, 0 );
	Check( add_hair_material( *job, numeric, warnings ), "the numeric-only twin registers" );

	// Numeric twins pinned to the two checker constants, so the check is
	// "the wrapper is TRANSPARENT", not merely "the wrapper changed
	// something".
	rise_blender_hair_material atLo = MelaninMaterial( "beta_lo", 1.3f, 0.0f, 0 );
	atLo.beta_m = 0.25f;
	rise_blender_hair_material atHi = MelaninMaterial( "beta_hi", 1.3f, 0.0f, 0 );
	atHi.beta_m = 0.75f;
	Check( add_hair_material( *job, atLo, warnings ) && add_hair_material( *job, atHi, warnings ),
		"the two pinned numeric reference materials register" );

	Check( warnings.empty(), "a resolvable texture painter produces no warnings" );

	// The wrapper the header promises, under the derived name.
	Check( (*job).GetScalarPainters() != 0 &&
	       (*job).GetScalarPainters()->GetItem( "rough_checker::hairscalar" ) != 0,
		"the colour painter is registered as a scalar painter under its derived name" );

	// Two hits differing ONLY in u.  v is pinned at 0.5 so the near-field
	// offset h = 2v - 1 is identical at both -- any difference in the
	// shade is the roughness, not the geometry.  Checker size 0.25:
	// ceil(0.1/0.25) = 1 (odd) and ceil(0.4/0.25) = 2 (even) against
	// ceil(0.5/0.25) = 2 (even), so the two cells pick opposite painters.
	const RISE::RayIntersectionGeometric riHi = MakeFibreHitAt( 0.1, 0.5 );
	const RISE::RayIntersectionGeometric riLo = MakeFibreHitAt( 0.4, 0.5 );

	// Off the fibre's normal plane (a non-zero x component in the ONB's
	// tangent direction): beta_m only shapes M_p, which is flat in theta
	// when both directions sit in that plane.
	const RISE::Vector3 wi = RISE::Vector3Ops::Normalize( RISE::Vector3( 0.4, 0.5, 0.5 ) );

	const HairBRDF* bTex = BrdfOf( *job, "tex_beta" );
	const HairBRDF* bNum = BrdfOf( *job, "num_beta" );
	const HairBRDF* bLo  = BrdfOf( *job, "beta_lo" );
	const HairBRDF* bHi  = BrdfOf( *job, "beta_hi" );

	if( bTex && bNum && bLo && bHi ) {
		const RISE::RISEPel texAtHi = bTex->value( wi, riHi );
		const RISE::RISEPel texAtLo = bTex->value( wi, riLo );
		const RISE::RISEPel numAtHi = bNum->value( wi, riHi );
		const RISE::RISEPel numAtLo = bNum->value( wi, riLo );

		Check( std::fabs( texAtHi[1] - texAtLo[1] ) > 1e-9,
			"a texture-driven beta_m shades differently at two UVs" );
		Check( std::fabs( numAtHi[1] - numAtLo[1] ) == 0.0,
			"the numeric-only twin does not vary across the same two UVs" );

		const RISE::RISEPel refHi = bHi->value( wi, riHi );
		const RISE::RISEPel refLo = bLo->value( wi, riLo );
		for( unsigned int c = 0; c < 3; ++c ) {
			Check( std::fabs( texAtHi[c] - refHi[c] ) == 0.0,
				"the textured material at the 0.75 cell equals a pinned beta_m = 0.75" );
			Check( std::fabs( texAtLo[c] - refLo[c] ) == 0.0,
				"the textured material at the 0.25 cell equals a pinned beta_m = 0.25" );
		}

		Check( std::fabs( texAtHi[1] - numAtHi[1] ) > 1e-9 &&
		       std::fabs( texAtLo[1] - numAtLo[1] ) > 1e-9,
			"the texture overrides the struct's numeric beta_m at both cells" );
	} else {
		Check( false, "the v10 A/B materials produced HairBRDFs" );
		return;
	}

	// --- the empty-field fallback: exactly the pre-v10 path ----------
	{
		// MelaninMaterial memsets, so the three v10 fields are already
		// NULL above; an explicitly EMPTY string must read the same way.
		warnings.clear();
		rise_blender_hair_material emptyStr = MelaninMaterial( "empty_str", 1.3f, 0.0f, 0 );
		emptyStr.beta_m_texture_painter_name = "";
		emptyStr.beta_n_texture_painter_name = "";
		emptyStr.ior_texture_painter_name    = "";
		Check( add_hair_material( *job, emptyStr, warnings ), "empty texture names register" );
		Check( warnings.empty(), "empty texture names warn about nothing" );

		const HairBRDF* bEmpty = BrdfOf( *job, "empty_str" );
		if( bEmpty ) {
			const RISE::RISEPel emptyAt = bEmpty->value( wi, riHi );
			const RISE::RISEPel numAt   = bNum->value( wi, riHi );
			for( unsigned int c = 0; c < 3; ++c ) {
				Check( std::fabs( emptyAt[c] - numAt[c] ) == 0.0,
					"an empty texture name is bit-for-bit the pre-v10 numeric path" );
			}
		} else {
			Check( false, "the empty-name material produced a HairBRDF" );
		}
	}

	// --- a texture name nothing is registered under ------------------
	{
		warnings.clear();
		rise_blender_hair_material dangling = MelaninMaterial( "dangling_rough", 1.3f, 0.0f, 0 );
		dangling.beta_m_texture_painter_name = "no_such_texture";
		const bool ok = Quietly( [&]() { return add_hair_material( *job, dangling, warnings ); } );
		Check( ok, "an unresolvable texture does NOT lose the material" );
		Check( warnings.size() == 1 &&
		       WarningsMention( warnings, "no_such_texture" ) &&
		       WarningsMention( warnings, "dangling_rough" ),
			"the unresolvable-texture warning names both the texture and the material" );
		Check( (*job).GetMaterials()->GetItem( "dangling_rough" ) != 0,
			"the material with the unresolvable texture is still registered" );

		const HairBRDF* bDangling = BrdfOf( *job, "dangling_rough" );
		if( bDangling ) {
			const RISE::RISEPel danglingAt = bDangling->value( wi, riHi );
			const RISE::RISEPel numAt      = bNum->value( wi, riHi );
			for( unsigned int c = 0; c < 3; ++c ) {
				Check( std::fabs( danglingAt[c] - numAt[c] ) == 0.0,
					"an unresolvable texture falls back to that slot's number" );
			}
		} else {
			Check( false, "the unresolvable-texture material produced a HairBRDF" );
		}
	}
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

// ============================================================
//  6. THE BUMP MODIFIER'S normalize FOLD (P1, Phase B review;
//     docs/RELIEF_MODIFIER_DESIGN.md 7.5)
// ============================================================

//! height(u, v) = k*u -- a LINEAR RAMP whose gradient is EXACTLY (k, 0)
//! everywhere, so ReliefModifier's central difference (any window,
//! including the exporter's 0.005) has no truncation error and the
//! delivered tilt has a closed form: with `normalize` TRUE the folded
//! amplitude is exactly `-strength*distance` (RISE_API.cpp's
//! `RISE_API_CreateBumpMapModifierEx`), so
//! `perturbed = N - (T*k)*(-strength*distance) = N + T*(k*strength*distance)`
//! and, since T and N are orthogonal unit vectors, normalizing preserves
//! the RATIO `perturbed.x / perturbed.z` exactly: it equals
//! `k*strength*distance` to floating-point precision, independent of the
//! window. That ratio is what the test below checks.
class LinearRampFunction2D :
	public virtual RISE::IFunction2D,
	public virtual RISE::Implementation::Reference
{
public:
	explicit LinearRampFunction2D( const RISE::Scalar k ) : m_k( k ) {}

	RISE::Scalar Evaluate( const RISE::Scalar x, const RISE::Scalar /*y*/ ) const override
	{
		return m_k * x;
	}

protected:
	virtual ~LinearRampFunction2D() {}

private:
	RISE::Scalar m_k;
};

void TestBumpModifierNormalizeFold()
{
	std::cout << "Test: the bridge's bump modifier reaches RISE_API_CreateBumpMapModifierEx "
	             "with normalize=TRUE (P1, Phase B review)" << std::endl;

	const RISE::Scalar k        = RISE::Scalar( 2.0 );   // height gradient, dH/du
	const RISE::Scalar strength = RISE::Scalar( 0.7 );
	const RISE::Scalar distance = RISE::Scalar( 1.5 );
	const RISE::Scalar window   = RISE::Scalar( 0.005 ); // matches exporter.py's _build_bump_modifier

	// `rise_blender_modifier.scale` / `.window` are `float` (the ABI
	// struct, not `RISE::Scalar` == double) -- exactly what
	// `_marshal_modifier` writes.  Round-trip through float BEFORE
	// deriving the expected ratio, or the ~1e-7 relative rounding this
	// truncation introduces (strength*distance = 1.05 has no exact
	// float32 representation) swamps a 1e-9 tolerance on a check that
	// has nothing to do with that truncation.  Past this point every
	// step (the fold, the central difference, the normalization) is
	// double-precision arithmetic on these ALREADY-ROUNDED inputs, so
	// 1e-9 is exactly the right bound for what the test actually checks.
	const float mScale  = static_cast<float>( strength * distance );
	const float mWindow = static_cast<float>( window );

	// A hit with onb.u() == +X, vNormal == +Z (MakeFibreHitAt above), so
	// T == (1,0,0) and N == (0,0,1) -- perturbed.x / perturbed.z reads
	// off the tilt directly.
	const RISE::RayIntersectionGeometric riBase = MakeFibreHitAt( 0.3, -0.4 );

	// --- (a) normalize TRUE: window-INDEPENDENT amplitude -- the fix. ---
	{
		JobHolder job;
		Check( job.Valid(), "6a: job created" );
		if( job.Valid() ) {
			LinearRampFunction2D* fn = new LinearRampFunction2D( k );
			Check( (*job).GetFunction2Ds()->AddItem( fn, "ramp_fn" ),
				"6a: the linear-ramp height function registers" );

			rise_blender_modifier m;
			m.name = "bump_a";
			m.kind = RISE_BLENDER_MODIFIER_BUMP;
			m.source_painter_name = "ramp_fn";
			m.scale = mScale;
			m.window = mWindow;
			m.normalize = 1;

			char err[256] = { 0 };
			Check( add_modifier( *job, m, err, sizeof( err ) ),
				"6a: add_modifier succeeds through the SAME code path the exporter's bump reaches" );

			RISE::IRayIntersectionModifier* mod = (*job).GetModifiers()->GetItem( "bump_a" );
			Check( mod != 0, "6a: the modifier is registered under its name" );
			if( mod ) {
				RISE::RayIntersectionGeometric ri = riBase;
				mod->Modify( ri );

				const RISE::Scalar wantRatio = k * static_cast<RISE::Scalar>( mScale );
				const RISE::Scalar gotRatio  = ri.vNormal.x / ri.vNormal.z;
				Check( std::fabs( gotRatio - wantRatio ) < 1e-9,
					"6a: tilt == strength*distance*slope (normalize=TRUE is window-independent)" );
			}

			fn->release();
		}
	}

	// --- (b) RED-PROOF: normalize FALSE reproduces the P1 bug -- the
	//     legacy window-COUPLED fold (`scale' = -scale*2*window`), which
	//     at window=0.005 delivers a tilt ~200x WEAKER than (a)'s. If this
	//     assertion is made to pass (or `add_modifier` is reverted to call
	//     `IJob::AddBumpMapModifier` unconditionally), test (a) above must
	//     fail -- this block exists to prove the fix is actually reached,
	//     not merely that the modifier registers. ---
	{
		JobHolder job;
		Check( job.Valid(), "6b: job created" );
		if( job.Valid() ) {
			LinearRampFunction2D* fn = new LinearRampFunction2D( k );
			Check( (*job).GetFunction2Ds()->AddItem( fn, "ramp_fn" ),
				"6b: the linear-ramp height function registers" );

			rise_blender_modifier m;
			m.name = "bump_b";
			m.kind = RISE_BLENDER_MODIFIER_BUMP;
			m.source_painter_name = "ramp_fn";
			m.scale = mScale;
			m.window = mWindow;
			m.normalize = 0;

			char err[256] = { 0 };
			Check( add_modifier( *job, m, err, sizeof( err ) ),
				"6b: add_modifier succeeds with normalize=FALSE too" );

			RISE::IRayIntersectionModifier* mod = (*job).GetModifiers()->GetItem( "bump_b" );
			Check( mod != 0, "6b: the modifier is registered under its name" );
			if( mod ) {
				RISE::RayIntersectionGeometric ri = riBase;
				mod->Modify( ri );

				const RISE::Scalar wantRatioIfFixed = k * static_cast<RISE::Scalar>( mScale );
				const RISE::Scalar gotRatio         = ri.vNormal.x / ri.vNormal.z;
				// The legacy fold's ratio is scaled by 2*window relative to
				// the fixed one -- at window=0.005 that is a ~100x shrink
				// (2*0.005 == 0.01), so the two can never be mistaken for
				// FP noise around each other.
				Check( std::fabs( gotRatio - wantRatioIfFixed ) > 1e-3,
					"6b: RED-PROOF -- normalize=FALSE does NOT reach strength*distance*slope "
					"(reproduces the P1 bug; the window-coupled fold is ~200x weaker at window=0.005)" );
				const RISE::Scalar wantRatioLegacy = k * static_cast<RISE::Scalar>( mScale ) * RISE::Scalar( 2 ) * static_cast<RISE::Scalar>( mWindow );
				Check( std::fabs( gotRatio - wantRatioLegacy ) < 1e-9,
					"6b: ...and IS exactly the legacy window-coupled fold (scale*2*window)" );
			}

			fn->release();
		}
	}
}

} // namespace

int main()
{
	std::cout << "=== Blender bridge hair test (ABI v11) ===" << std::endl;

	TestAbiVersionAndLayout();
	TestEachTierRegisters();
	TestMelaninParityRescale();
	TestHairObjectEndToEnd();
	TestTextureDrivenScalarSlots();
	TestFailuresAreNonFatalAndNamed();
	TestPackWarnings();
	TestBumpModifierNormalizeFold();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;

	if( g_failures == 0 ) {
		std::cout << "BlenderBridgeHairTest: PASSED" << std::endl;
		return 0;
	}

	std::cout << "BlenderBridgeHairTest: FAILED" << std::endl;
	return 1;
}
