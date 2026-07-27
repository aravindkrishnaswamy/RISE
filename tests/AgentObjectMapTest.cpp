//////////////////////////////////////////////////////////////////////
//
//  AgentObjectMapTest.cpp - Toolkit slice 3a (the OBJECT-MAP segmentation
//  render: render{"mode":"objectmap"} + per-object legend).
//
//  Proves the identity-render path end-to-end, both at the AgentSession
//  verb level (WrapJob -> Render(renderTarget=ObjectMap)) and at the
//  pipeline level (CreateInteractiveObjectMapPipeline directly, for the two
//  red-proves that need to force a state the AgentSession path structurally
//  forbids):
//
//    * pixel->object: a rendered objectmap PNG decodes (byte-exactly, via a
//      Rec709RGB_Linear PNGReader that recovers the raw stored byte) so the
//      center pixel's bytes EQUAL the hit object's legend colorHex, and an
//      empty (background/miss) pixel is (0,0,0).
//    * quantizer byte-exactness: for every legend colour, the linear
//      pre-image the shader emits round-trips through the sRGB encode back
//      to the exact byte (half-LSB-centered contract).
//    * legend sanity: one entry per visible object, sum(pixelCounts) +
//      background == w*h, every visible entry pixelCount > 0.
//    * determinism: two identical objectmap renders -> byte-identical PNG
//      AND identical legend (the id shader has no MC noise).
//    * isolation: an objectmap render leaves the production rasterizer's
//      FrameStore identity + Generation() untouched (mirrors the S2 draft-
//      isolation assertions).
//    * document byte-identity across an objectmap render.
//    * camera-override composition: aiming at one object makes its
//      pixelCount dominate.
//    * instance_array -> 4 distinct <gen>[i,j] legend entries; CSG -> a
//      single root entry.
//    * downscale guard (native-size rule): read_image maxEdge box-blends
//      the identity colours, so the downscaled image carries colours that
//      do NOT all match a legend entry.
//    * RED-PROVE (a) the EXACTNESS INVARIANT: forcing a >1-sample kernel +
//      pixel filter onto the objectmap instance blends boundary pixels into
//      colours no legend entry carries; a fresh (kernel-free) instance is
//      exact again (restore).
//    * RED-PROVE (b) the UNKNOWN-colour rule: skipping one object from the
//      identity registry makes its region decode to the reserved UNKNOWN
//      colour (magenta) with a zero legend pixelCount and a nonzero unknown
//      tally; registering it again restores an exact per-object map.
//
//  Also covers Toolkit slice 3b (query_object_at {x,y}, the cheap single-
//  pixel companion to render mode:"objectmap" -- IMPLEMENTATION CHOICE (a):
//  reuses the objectmap ephemeral pipeline WHOLESALE, one identity render +
//  a single-pixel legend match, rather than a bespoke single-ray probe):
//
//    * center-of-object query names all 3 objects of kScene3 correctly
//      (located via a reference objectmap render + pixel scan).
//    * an empty-region (background) query -> hit:false, a STRUCTURED
//      result, never an error.
//    * camera-override composition: aiming at sph_a makes the SAME center
//      pixel that resolves to sph_b under the default camera resolve to
//      sph_a instead.
//    * works on a head with NO active production rasterizer (mirrors the
//      quality:"draft"/mode:"objectmap" gate) -- a sibling production
//      render on the same head fails honestly, query_object_at does not.
//    * document byte-identity across a query (with and without a camera
//      override).
//    * the WIRE verb over the JSON-RPC dispatcher: an in-range query
//      returns the documented {hit,name,kind,pixelX,pixelY,width,height,
//      message} shape; an out-of-range x or y is a clean -32602
//      (kInvalidParams), never a structured hit:false.
//
//  Self-contained: inline native-v7 scenes, OIDN off, no RISE_MEDIA_PATH.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Agent/InMemoryRasterizerOutput.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IEnumCallback.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Rendering/Rasterizer.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/SceneEditor/SceneEditController.h"   // P3-a: async+objectmap through the coordinator harness
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

//----------------------------------------------------------------------
// Scenes.
//----------------------------------------------------------------------

// Three well-separated named spheres (sorted: sph_a, sph_b, sph_c -> ids
// 0,1,2) in a row across a 64x64 film; the center pixel hits sph_b.  A
// pathtracing rasterizer + shader are included ONLY so the head has an
// active production rasterizer with a canonical FrameStore for the
// isolation test -- the objectmap render never touches them.
static const char* const kScene3 =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
	"standard_object\n{\n\tname sph_a\n\tgeometry geo\n\tmaterial mat\n\tposition -1.7 0 0\n}\n\n"
	"standard_object\n{\n\tname sph_b\n\tgeometry geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"standard_object\n{\n\tname sph_c\n\tgeometry geo\n\tmaterial mat\n\tposition 1.7 0 0\n}\n";

// A 2x2 instance_array named `grid` -> grid[0,0], grid[1,0], grid[0,1],
// grid[1,1] (the `%s[%d,%d]` synthesized-name format from Cst.cpp).
static const char* const kSceneInstances =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 7\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 0.5\n}\n\n"
	"instance_array\n{\n\tname grid\n\ttemplate geo\n\tcount_u 2\n\tcount_v 2\n\tmaterial mat\n\tposition expr(u*3.0-1.5) expr(v*3.0-1.5) 0\n}\n";

// Two spheres that OVERLAP in screen space (one partly occludes the other),
// so a boundary pixel's sub-pixel samples straddle TWO distinct objects.  The
// well-separated kScene3 spheres never do this: every silhouette is
// object-vs-background, and IntegratePixel alpha-normalizes (colAccrued /=
// alphas) so a partially-covered pixel still resolves to the single hit
// object's EXACT identity colour.  Only an object-vs-object boundary produces
// a blended colour no identity carries -- which is exactly what the RED half
// of the exactness red-prove must exercise.  sph_front (id 0) sits closer in Z
// and occludes part of sph_back (id 1); their shared occlusion edge is the
// object-object boundary the kernel blends across.
static const char* const kSceneAdjacent =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
	"standard_object\n{\n\tname sph_front\n\tgeometry geo\n\tmaterial mat\n\tposition -0.35 0 0.6\n}\n\n"
	"standard_object\n{\n\tname sph_back\n\tgeometry geo\n\tmaterial mat\n\tposition 0.35 0 0\n}\n";

// A DIRECTLY-VISIBLE EMISSIVE object (a glowing sphere as an area light --
// the common RISE pattern) at screen center, plus one non-emissive sphere off
// to the side.  Sorted names: lamp (id 0, center, emissive), side (id 1).
// Exercises the P1 fix: the objectmap caster must SEE luminaires (an emitter
// is a real world-visible, selectable object) while the draft/material-preview
// caster must STILL hide them (studio-preview semantics unchanged).
static const char* const kSceneEmissive =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"uniformcolor_painter\n{\n\tname emit\n\tcolor 1 1 1\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname lampmat\n\texitance emit\n\tmaterial mat\n\tscale 1.0\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
	"standard_object\n{\n\tname lamp\n\tgeometry geo\n\tmaterial lampmat\n\tposition 0 0 0\n}\n\n"
	"standard_object\n{\n\tname side\n\tgeometry geo\n\tmaterial mat\n\tposition -1.7 0 0\n}\n";

// A single CSG union of two spheres -> a single composite root object.
static const char* const kSceneCsg =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
	"standard_object\n{\n\tname op_a\n\tgeometry geo\n\tmaterial mat\n\tposition -0.4 0 0\n}\n\n"
	"standard_object\n{\n\tname op_b\n\tgeometry geo\n\tmaterial mat\n\tposition 0.4 0 0\n}\n\n"
	"csg_object\n{\n\tname csg_root\n\tobja op_a\n\tobjb op_b\n\toperation union\n\tmaterial mat\n}\n";

// Toolkit slice 3b test (5): the SAME 3-sphere layout as kScene3, but with
// NO rasterizer chunk (and no standard_shader, which existed only to be
// resolved by the now-absent rasterizer's Finalize) -- Job::GetRasterizer()
// resolves nullptr on this head.  Proves query_object_at works on a
// rasterizer-less head (it reuses the objectmap ephemeral pipeline, which
// never dereferences the production rasterizer -- mirrors the quality:
// "draft" / mode:"objectmap" gate).
static const char* const kScene3NoRasterizer =
	"RISE ASCII SCENE 7\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
	"standard_object\n{\n\tname sph_a\n\tgeometry geo\n\tmaterial mat\n\tposition -1.7 0 0\n}\n\n"
	"standard_object\n{\n\tname sph_b\n\tgeometry geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"standard_object\n{\n\tname sph_c\n\tgeometry geo\n\tmaterial mat\n\tposition 1.7 0 0\n}\n";

//----------------------------------------------------------------------
// Helpers.
//----------------------------------------------------------------------

static std::string WriteTemp( const char* name, const std::string& text )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	std::string path = dir + name;
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return std::string();
	f.write( text.data(), (std::streamsize)text.size() );
	f.close();
	return path;
}

typedef std::array<unsigned char, 4> Px;   // r,g,b,a

struct Decoded
{
	unsigned int    w = 0, h = 0;
	std::vector<Px> px;   // row-major
	const Px& at( unsigned int x, unsigned int y ) const { return px[ (std::size_t)y * w + x ]; }
};

// Decode PNG bytes into raw stored bytes.  Reading with
// eColorSpace_Rec709RGB_Linear makes the PNGReader do a bare byte/255 with
// NO transfer-function conversion, so round(v*255) recovers the EXACT byte
// that was written -- no sRGB round-trip that could drift an LSB.
static bool DecodePng( const std::vector<unsigned char>& png, Decoded& out )
{
	if( png.empty() ) return false;
	Implementation::MemoryBuffer* buf = new Implementation::MemoryBuffer(
		const_cast<char*>( reinterpret_cast<const char*>( png.data() ) ),
		(unsigned int)png.size(), /*bTakeOwnership*/false );
	IRasterImageReader* reader = nullptr;
	if( !RISE_API_CreatePNGReader( &reader, *buf, eColorSpace_Rec709RGB_Linear ) || !reader ) {
		safe_release( buf );
		return false;
	}
	unsigned int w = 0, h = 0;
	if( !reader->BeginRead( w, h ) ) { safe_release( reader ); safe_release( buf ); return false; }
	out.w = w; out.h = h;
	out.px.resize( (std::size_t)w * h );
	auto toB = []( double v ) -> unsigned char {
		int i = (int)( v * 255.0 + 0.5 );
		if( i < 0 ) i = 0; if( i > 255 ) i = 255;
		return (unsigned char)i;
	};
	for( unsigned int y = 0; y < h; ++y ) {
		for( unsigned int x = 0; x < w; ++x ) {
			RISEColor c;
			reader->ReadColor( c, x, y );
			Px p = { toB( c.base.r ), toB( c.base.g ), toB( c.base.b ), toB( c.a ) };
			out.px[ (std::size_t)y * w + x ] = p;
		}
	}
	reader->EndRead();
	safe_release( reader );
	safe_release( buf );
	return true;
}

// Parse "#RRGGBB" -> byte triple.
static bool HexToBytes( const std::string& hex, unsigned char b[3] )
{
	if( hex.size() != 7 || hex[0] != '#' ) return false;
	for( int i = 0; i < 3; ++i ) {
		char buf[3] = { hex[1 + i * 2], hex[2 + i * 2], 0 };
		char* end = nullptr;
		long v = std::strtol( buf, &end, 16 );
		if( end != buf + 2 ) return false;
		b[i] = (unsigned char)v;
	}
	return true;
}

// Find a legend entry by name; returns nullptr if absent.
static const LegendEntry* FindLegend( const AgentRenderResult& r, const std::string& name )
{
	for( std::size_t i = 0; i < r.legend.size(); ++i )
		if( r.legend[i].name == name ) return &r.legend[i];
	return nullptr;
}

// Toolkit slice 3b: find ANY pixel whose decoded bytes exactly equal
// `rgb` -- since every hit-pixel of a solid-identity-colour object decodes
// to the SAME exact byte triple (the objectmap exactness invariant), any
// matching pixel is an equally valid query_object_at probe point for that
// object; returns false if none found.
static bool FindPixelForColor( const Decoded& d, const unsigned char rgb[3],
                                unsigned int& outX, unsigned int& outY )
{
	for( unsigned int y = 0; y < d.h; ++y ) {
		for( unsigned int x = 0; x < d.w; ++x ) {
			const Px& p = d.at( x, y );
			if( p[0] == rgb[0] && p[1] == rgb[1] && p[2] == rgb[2] ) {
				outX = x; outY = y;
				return true;
			}
		}
	}
	return false;
}

//----------------------------------------------------------------------
// (1)+(2)+(3)+(4)+(6): AgentSession-level core objectmap render.
//----------------------------------------------------------------------
static void RunCoreTests()
{
	std::printf( "=== AgentObjectMapTest: core (pixel->object / quantizer / legend / determinism) ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_3.RISEscene", kScene3 );
	Check( !scenePath.empty(), "wrote the 3-sphere scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "3-sphere scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	const std::string preDoc = session->ReadDocument();

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult r = session->Render( p );

	Check( r.ok, "objectmap render succeeds (ok)" );
	Check( r.renderMode == "objectmap", "renderMode reports \"objectmap\"" );
	Check( r.width == 64 && r.height == 64, "render dims are the authored 64x64" );
	Check( r.legend.size() == 3, "legend has exactly one entry per registered object (3)" );

	// (6) Document byte-identity across the render.
	Check( session->ReadDocument() == preDoc, "objectmap render is side-effect-free on the Document" );

	// Legend names are the 3 objects, in sorted id order.
	if( r.legend.size() == 3 ) {
		Check( r.legend[0].name == "sph_a" && r.legend[1].name == "sph_b" && r.legend[2].name == "sph_c",
		       "legend entries are in deterministic sorted-name order (sph_a, sph_b, sph_c)" );
	}

	// (2) Quantizer byte-exactness: for each legend colour, the linear
	// pre-image the shader emits (L = SRGBinv((B+0.5)/255)) re-encodes
	// (truncating SRGB*255) back to the exact byte.
	bool quantOk = true;
	for( std::size_t i = 0; i < r.legend.size(); ++i ) {
		unsigned char b[3];
		if( !HexToBytes( r.legend[i].colorHex, b ) ) { quantOk = false; continue; }
		for( int ch = 0; ch < 3; ++ch ) {
			const Scalar L = ColorUtils::SRGBTransferFunctionInverse( ( (Scalar)b[ch] + 0.5 ) / 255.0 );
			Scalar enc = ColorUtils::SRGBTransferFunction( L < 0 ? 0 : ( L > 1 ? 1 : L ) );
			if( enc > 1.0 ) enc = 1.0;
			const unsigned char back = (unsigned char)( enc * 255.0 );   // TRUNCATING, matching Integerize<sRGBPel>
			if( back != b[ch] ) quantOk = false;
		}
	}
	Check( quantOk, "quantizer contract holds: every legend colour's linear pre-image re-encodes to its exact byte" );

	// (1) pixel->object: decode the native PNG, check center + a corner.
	Decoded dec;
	Check( DecodePng( r.png, dec ), "objectmap PNG decodes" );
	Check( dec.w == 64 && dec.h == 64, "decoded dims are 64x64 (native, not downscaled)" );
	if( dec.w == 64 && dec.h == 64 && r.legend.size() == 3 ) {
		unsigned char cb[3];
		HexToBytes( r.legend[1].colorHex, cb );   // sph_b (center)
		const Px& center = dec.at( 32, 32 );
		Check( center[0] == cb[0] && center[1] == cb[1] && center[2] == cb[2],
		       "MONEY ASSERTION: center pixel bytes EQUAL sph_b's legend colorHex (byte-exact)" );
		const Px& corner = dec.at( 0, 0 );
		Check( corner[0] == 0 && corner[1] == 0 && corner[2] == 0,
		       "an empty (miss) corner pixel is (0,0,0)" );
		Check( corner[3] == 0, "an empty corner pixel is fully transparent (alpha 0)" );
	}

	// (3) legend sanity: sum(pixelCounts) + background == w*h; each > 0.
	std::uint64_t sum = 0;
	bool allPositive = true;
	for( std::size_t i = 0; i < r.legend.size(); ++i ) {
		sum += r.legend[i].pixelCount;
		if( r.legend[i].pixelCount == 0 ) allPositive = false;
	}
	Check( allPositive, "every visible object's pixelCount is > 0" );
	// background = pixels that decoded to (0,0,0) alpha 0.
	std::uint64_t bg = 0;
	for( std::size_t i = 0; i < dec.px.size(); ++i )
		if( dec.px[i][3] == 0 ) ++bg;
	Check( sum + bg == (std::uint64_t)dec.w * dec.h,
	       "sum(legend pixelCounts) + background == w*h" );

	// (4) determinism: a second identical objectmap render -> byte-identical
	// PNG AND identical legend (the id shader has no MC noise).
	AgentRenderResult r2 = session->Render( p );
	Check( r2.ok && r2.png == r.png, "two identical objectmap renders are byte-identical PNGs (deterministic)" );
	bool legendSame = ( r2.legend.size() == r.legend.size() );
	for( std::size_t i = 0; legendSame && i < r.legend.size(); ++i )
		legendSame = ( r2.legend[i].name == r.legend[i].name
		            && r2.legend[i].colorHex == r.legend[i].colorHex
		            && r2.legend[i].pixelCount == r.legend[i].pixelCount );
	Check( legendSame, "the legend is identical across two renders" );

	// samples / quality are honestly reported as ignored under objectmap.
	AgentRenderParams pq;
	pq.renderTarget = AgentRenderTarget::ObjectMap;
	pq.samples = 64;
	pq.quality = AgentRenderQuality::Draft;
	AgentRenderResult rq = session->Render( pq );
	Check( rq.ok && rq.renderMode == "objectmap", "objectmap render ignores quality:draft (still renderMode==objectmap)" );
	Check( !rq.samplesOverridden && rq.effectiveSamples == 1,
	       "objectmap reports 1 spp, samples override not applied" );
	Check( rq.message.find( "objectmap ignores the samples override" ) != std::string::npos,
	       "the message honestly states the samples override is ignored under objectmap" );

	pJob->release();
}

//----------------------------------------------------------------------
// (7) camera-override composition: aim at one object -> its count dominates.
//----------------------------------------------------------------------
static void RunCameraOverrideTest()
{
	std::printf( "=== AgentObjectMapTest: camera-override composition ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_cam.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "cam scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "cam session" ); return; }

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	p.camera.hasLocation = true;  p.camera.location = "-1.7 0 2";
	p.camera.hasLookAt   = true;  p.camera.lookAt   = "-1.7 0 0";
	AgentRenderResult r = session->Render( p );
	Check( r.ok && r.cameraOverridden, "objectmap composes with a camera override (cameraOverridden)" );
	Check( r.renderMode == "objectmap", "camera-override objectmap still renderMode==objectmap" );

	const LegendEntry* a = FindLegend( r, "sph_a" );
	const LegendEntry* b = FindLegend( r, "sph_b" );
	const LegendEntry* c = FindLegend( r, "sph_c" );
	Check( a && b && c, "all three objects still in the legend after a camera override" );
	if( a && b && c ) {
		Check( a->pixelCount > b->pixelCount && a->pixelCount > c->pixelCount,
		       "MONEY ASSERTION: aiming the camera at sph_a makes its pixelCount dominate" );
	}
	pJob->release();
}

//----------------------------------------------------------------------
// (8a) instance_array -> 4 distinct <gen>[i,j] entries.
//----------------------------------------------------------------------
static void RunInstanceArrayTest()
{
	std::printf( "=== AgentObjectMapTest: instance_array legend ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_inst.RISEscene", kSceneInstances );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "instance scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "instance session" ); return; }

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult r = session->Render( p );
	Check( r.ok && r.renderMode == "objectmap", "instance_array objectmap render succeeds" );
	Check( r.legend.size() == 4, "instance_array of 2x2 yields exactly 4 legend entries" );
	Check( FindLegend( r, "grid[0,0]" ) && FindLegend( r, "grid[1,0]" )
	    && FindLegend( r, "grid[0,1]" ) && FindLegend( r, "grid[1,1]" ),
	    "each instance carries its synthesized <gen>[i,j] name" );
	pJob->release();
}

//----------------------------------------------------------------------
// (8b) CSG -> a single composite-root entry.
//----------------------------------------------------------------------
static void RunCsgTest()
{
	std::printf( "=== AgentObjectMapTest: CSG single-root legend ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_csg.RISEscene", kSceneCsg );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "csg scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "csg session" ); return; }

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult r = session->Render( p );
	Check( r.ok && r.renderMode == "objectmap", "CSG objectmap render succeeds" );
	Check( r.legend.size() == 1, "a CSG composite is a SINGLE world-visible object -> one legend entry" );
	if( r.legend.size() == 1 ) {
		Check( r.legend[0].name == "csg_root", "the single entry is the CSG root object" );
		Check( r.legend[0].pixelCount > 0, "the CSG root covers pixels" );
	}
	pJob->release();
}

//----------------------------------------------------------------------
// (5) isolation: production FrameStore identity + Generation untouched.
//----------------------------------------------------------------------
static void RunIsolationTest()
{
	std::printf( "=== AgentObjectMapTest: production FrameStore isolation ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_iso.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "iso scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "iso session" ); return; }

	IRasterizer* rast = pJob->GetRasterizer();
	Implementation::Rasterizer* concreteRast = dynamic_cast<Implementation::Rasterizer*>( rast );
	Check( concreteRast != nullptr, "head has a concrete production rasterizer" );
	if( !concreteRast ) { pJob->release(); return; }
	Implementation::FrameStore* displayStore = concreteRast->GetFrameStore();
	Check( displayStore != nullptr, "production rasterizer has a canonical FrameStore before any render" );
	if( !displayStore ) { pJob->release(); return; }

	const uint64_t genBefore = displayStore->Generation();

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult r = session->Render( p );
	Check( r.ok, "objectmap render succeeds (isolation scene)" );
	Check( displayStore->Generation() == genBefore,
	       "MONEY ASSERTION: the production FrameStore's Generation() did NOT advance across an objectmap render" );
	Check( concreteRast->GetFrameStore() == displayStore,
	       "the production rasterizer's FrameStore identity is untouched by an objectmap render" );
	pJob->release();
}

//----------------------------------------------------------------------
// (9) downscale guard (native-size rule).
//----------------------------------------------------------------------
static void RunDownscaleGuardTest()
{
	std::printf( "=== AgentObjectMapTest: downscale guard (native-size rule) ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_ds.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "ds scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "ds session" ); return; }

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult r = session->Render( p );
	Check( r.ok, "objectmap render for downscale guard succeeds" );

	// Build the set of legal legend colours (+ background) for matching.
	std::vector<std::array<unsigned char, 3> > legal;
	std::array<unsigned char, 3> black = { { 0, 0, 0 } };
	legal.push_back( black );
	for( std::size_t i = 0; i < r.legend.size(); ++i ) {
		unsigned char b[3];
		if( HexToBytes( r.legend[i].colorHex, b ) ) {
			std::array<unsigned char, 3> a = { { b[0], b[1], b[2] } };
			legal.push_back( a );
		}
	}

	// Native read: EVERY pixel is a legal colour (exact).
	unsigned int nw = 0, nh = 0;
	std::vector<unsigned char> nativePng = session->ReadImage( 0, nw, nh );
	Decoded natDec;
	Check( DecodePng( nativePng, natDec ), "native read_image decodes" );
	bool nativeAllLegal = true;
	for( std::size_t i = 0; i < natDec.px.size(); ++i ) {
		bool ok = false;
		for( std::size_t j = 0; j < legal.size(); ++j )
			if( natDec.px[i][0] == legal[j][0] && natDec.px[i][1] == legal[j][1] && natDec.px[i][2] == legal[j][2] ) { ok = true; break; }
		if( !ok ) { nativeAllLegal = false; break; }
	}
	Check( nativeAllLegal, "at NATIVE size every objectmap pixel matches a legend colour (or background) exactly" );

	// Downscaled read: box-filter blending produces colours that are NOT
	// all legal -- documents why the objectmap must be read at native size.
	unsigned int dw = 0, dh = 0;
	std::vector<unsigned char> smallPng = session->ReadImage( 16, dw, dh );
	Decoded smallDec;
	Check( DecodePng( smallPng, smallDec ), "downscaled read_image decodes" );
	bool foundBlended = false;
	for( std::size_t i = 0; i < smallDec.px.size() && !foundBlended; ++i ) {
		bool legalPix = false;
		for( std::size_t j = 0; j < legal.size(); ++j )
			if( smallDec.px[i][0] == legal[j][0] && smallDec.px[i][1] == legal[j][1] && smallDec.px[i][2] == legal[j][2] ) { legalPix = true; break; }
		if( !legalPix ) foundBlended = true;
	}
	Check( foundBlended,
	       "MONEY ASSERTION: a box-downscaled objectmap carries BLENDED colours no legend entry matches (read it at native size!)" );
	pJob->release();
}

//----------------------------------------------------------------------
// Pipeline-level palette builder (test-local; the production builder is a
// file-static in AgentSession.cpp).  Assigns a fixed distinct base colour
// per object and their exact linear pre-images; `skipIndex` omits one
// object from the identity registry to exercise the UNKNOWN path.
//----------------------------------------------------------------------
static RISEPel LinFromBytes( unsigned char r, unsigned char g, unsigned char b )
{
	return RISEPel(
		ColorUtils::SRGBTransferFunctionInverse( ( (Scalar)r + 0.5 ) / 255.0 ),
		ColorUtils::SRGBTransferFunctionInverse( ( (Scalar)g + 0.5 ) / 255.0 ),
		ColorUtils::SRGBTransferFunctionInverse( ( (Scalar)b + 0.5 ) / 255.0 ) );
}

static void BuildTestPalette( IObjectManager* objMgr,
                              Implementation::ObjectMapPalette& pal,
                              int skipIndex )
{
	struct NC : public IEnumCallback<const char*>
	{
		std::vector<std::string> n;
		bool operator()( const char* const& s ) override { if( s ) n.push_back( std::string( s ) ); return true; }
	} nc;
	if( objMgr ) objMgr->EnumerateItemNames( nc );
	const std::size_t count = nc.n.size();

	static const unsigned char base[5][3] = {
		{ 230, 25, 75 }, { 60, 180, 75 }, { 0, 130, 200 }, { 245, 130, 48 }, { 145, 30, 180 }
	};

	pal.names = nc.n;
	pal.bytes.resize( count );
	pal.linearColors.resize( count );
	pal.registry.clear();
	for( std::size_t id = 0; id < count; ++id ) {
		const unsigned char* b = base[ id % 5 ];
		std::array<unsigned char, 3> a = { { b[0], b[1], b[2] } };
		pal.bytes[id] = a;
		pal.linearColors[id] = LinFromBytes( b[0], b[1], b[2] );
		if( (int)id != skipIndex ) {
			IObjectPriv* o = objMgr ? objMgr->GetItem( nc.n[id].c_str() ) : 0;
			if( o ) pal.registry[ static_cast<const IObject*>( o ) ] = (std::uint32_t)id;
		}
	}
	std::array<unsigned char, 3> mag = { { 255, 0, 255 } };
	pal.unknownBytes  = mag;
	pal.unknownLinear = LinFromBytes( 255, 0, 255 );
	pal.counts = std::vector<std::atomic<std::uint32_t> >( count + 1 );
	for( std::size_t i = 0; i < pal.counts.size(); ++i ) pal.counts[i].store( 0u, std::memory_order_relaxed );
}

// Render one objectmap pass through a fresh (kernel-free) pipeline; if
// `installKernel` is true, force a >1-sample MultiJittered kernel + box
// pixel filter onto the instance (the state the AgentSession path forbids).
// Returns the decoded image.
static bool RenderPipeline( const IScenePriv& scene,
                            const Implementation::ObjectMapPalette& pal,
                            bool installKernel,
                            Decoded& out )
{
	IRasterizer* rast = nullptr;
	IRayCaster*  caster = nullptr;
	if( !Implementation::CreateInteractiveObjectMapPipeline( &rast, &caster, pal ) || !rast )
		return false;

	if( installKernel ) {
		Implementation::PixelBasedRasterizerHelper* helper =
			dynamic_cast<Implementation::PixelBasedRasterizerHelper*>( rast );
		if( helper ) {
			ISampling2D* kernel = nullptr;
			RISE_API_CreateMultiJitteredSampling2D( &kernel, 1.0, 1.0 );
			IPixelFilter* filter = nullptr;
			RISE_API_CreateBoxPixelFilter( &filter, 1.0, 1.0 );
			if( kernel ) kernel->SetNumSamples( 16 );
			helper->SubSampleRays( kernel, filter );   // installs BOTH -> jittered multisampling
			safe_release( kernel );
			safe_release( filter );
		}
	}

	InMemoryRasterizerOutput* sink = new InMemoryRasterizerOutput();
	rast->AddRasterizerOutput( sink );
	rast->RasterizeScene( scene, 0, nullptr );
	const std::vector<unsigned char> png = sink->ToPng();
	const bool decoded = DecodePng( png, out );
	safe_release( sink );
	safe_release( caster );
	safe_release( rast );
	return decoded;
}

static bool AllPixelsLegal( const Decoded& d, const Implementation::ObjectMapPalette& pal )
{
	for( std::size_t i = 0; i < d.px.size(); ++i ) {
		if( d.px[i][3] == 0 ) continue;   // background
		bool ok = false;
		for( std::size_t id = 0; id < pal.bytes.size(); ++id )
			if( d.px[i][0] == pal.bytes[id][0] && d.px[i][1] == pal.bytes[id][1] && d.px[i][2] == pal.bytes[id][2] ) { ok = true; break; }
		if( !ok ) return false;
	}
	return true;
}

//----------------------------------------------------------------------
// RED-PROVE (a): the exactness invariant.
//----------------------------------------------------------------------
static void RunExactnessRedProve()
{
	std::printf( "=== AgentObjectMapTest: RED-PROVE (a) exactness invariant (no kernel) ===\n" );
	// Uses OVERLAPPING geometry (kSceneAdjacent) so the RED multisampled pass
	// has an object-vs-object boundary to blend across; see kSceneAdjacent.
	const std::string scenePath = WriteTemp( "rise_objmap_exact.RISEscene", kSceneAdjacent );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "exact scene loads" ); return; }
	const IScenePriv* scene = pJob->GetScene();
	if( !scene ) { pJob->release(); Check( false, "exact scene priv" ); return; }

	Implementation::ObjectMapPalette pal;
	BuildTestPalette( pJob->GetObjects(), pal, /*skipIndex*/-1 );

	// GREEN: kernel-free -> every non-background pixel is an EXACT identity
	// colour (single-ray, no jitter, no filter).
	Decoded green;
	Check( RenderPipeline( *scene, pal, /*installKernel*/false, green ), "kernel-free pipeline renders" );
	Check( AllPixelsLegal( green, pal ),
	       "kernel-free objectmap: every non-background pixel is an EXACT identity colour" );

	// RED: force a multisampling kernel + filter -> boundary pixels blend
	// into colours no identity colour carries (the invariant would break).
	Decoded red;
	Check( RenderPipeline( *scene, pal, /*installKernel*/true, red ), "kernel+filter pipeline renders" );
	Check( !AllPixelsLegal( red, pal ),
	       "RED-PROVE: a >1-sample kernel+filter BLENDS boundary pixels -> NOT all match an identity colour (why the factory installs neither)" );

	// RESTORE: a fresh kernel-free instance is exact again.
	Decoded restored;
	Check( RenderPipeline( *scene, pal, /*installKernel*/false, restored ), "restore pipeline renders" );
	Check( AllPixelsLegal( restored, pal ),
	       "RESTORE: a fresh kernel-free objectmap is exact again (invariant intact)" );

	pJob->release();
}

//----------------------------------------------------------------------
// RED-PROVE (b): the unknown-colour rule.
//----------------------------------------------------------------------
static void RunUnknownColorRedProve()
{
	std::printf( "=== AgentObjectMapTest: RED-PROVE (b) unknown-colour rule ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_unknown.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "unknown scene loads" ); return; }
	const IScenePriv* scene = pJob->GetScene();
	if( !scene ) { pJob->release(); Check( false, "unknown scene priv" ); return; }

	// RED: skip sph_b (id 1, the center) from the registry -> its region
	// decodes to the reserved UNKNOWN colour (magenta), its tally stays 0,
	// and the unknown tally is nonzero.
	{
		Implementation::ObjectMapPalette pal;
		BuildTestPalette( pJob->GetObjects(), pal, /*skipIndex*/1 );
		Decoded d;
		Check( RenderPipeline( *scene, pal, /*installKernel*/false, d ), "skip-one pipeline renders" );
		Check( d.w == 64 && d.h == 64, "skip-one image is 64x64" );
		if( d.w == 64 && d.h == 64 ) {
			const Px& center = d.at( 32, 32 );
			Check( center[0] == 255 && center[1] == 0 && center[2] == 255,
			       "RED-PROVE: an unregistered object's pixels decode to the reserved UNKNOWN colour (magenta)" );
		}
		Check( pal.counts[1].load() == 0u, "the skipped object's own tally stays 0 (it never matched)" );
		Check( pal.counts[ pal.names.size() ].load() > 0u, "the UNKNOWN tally caught the unmapped pixels" );
	}

	// RESTORE: register all objects -> the center is sph_b's colour again,
	// and nothing lands in the UNKNOWN tally.
	{
		Implementation::ObjectMapPalette pal;
		BuildTestPalette( pJob->GetObjects(), pal, /*skipIndex*/-1 );
		Decoded d;
		Check( RenderPipeline( *scene, pal, /*installKernel*/false, d ), "restore pipeline renders" );
		if( d.w == 64 && d.h == 64 ) {
			const Px& center = d.at( 32, 32 );
			Check( center[0] == pal.bytes[1][0] && center[1] == pal.bytes[1][1] && center[2] == pal.bytes[1][2],
			       "RESTORE: with sph_b registered, the center decodes to its identity colour" );
		}
		Check( pal.counts[ pal.names.size() ].load() == 0u,
		       "RESTORE: nothing lands in the UNKNOWN tally when every object is registered" );
	}

	pJob->release();
}

//----------------------------------------------------------------------
// P1: a directly-visible EMISSIVE object segments into the objectmap
// (the objectmap caster SEES luminaires), while a DRAFT render of the same
// scene STILL hides it (studio-preview semantics unchanged).
//----------------------------------------------------------------------
static void RunEmissiveVisibilityTest()
{
	std::printf( "=== AgentObjectMapTest: emissive object visibility (P1) ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_emit.RISEscene", kSceneEmissive );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "emissive scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "emissive session" ); return; }

	// --- objectmap: the emissive `lamp` IS a legend entry with pixelCount>0,
	// and the center pixel byte-matches its legend colour. ---
	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult r = session->Render( p );
	Check( r.ok && r.renderMode == "objectmap", "objectmap render of the emissive scene succeeds" );
	Check( r.legend.size() == 2, "the emissive scene has exactly two world-visible objects (lamp, side)" );

	const LegendEntry* lamp = FindLegend( r, "lamp" );
	Check( lamp != nullptr, "the emissive object `lamp` appears in the legend (NOT dropped as a luminaire)" );
	if( lamp ) {
		Check( lamp->pixelCount > 0,
		       "MONEY ASSERTION (P1): the directly-visible emissive object has pixelCount > 0 (it segments into the map, not background)" );
	}

	Decoded dec;
	Check( DecodePng( r.png, dec ), "emissive objectmap PNG decodes" );
	if( dec.w == 64 && dec.h == 64 && lamp ) {
		unsigned char cb[3];
		HexToBytes( lamp->colorHex, cb );
		const Px& center = dec.at( 32, 32 );
		Check( center[0] == cb[0] && center[1] == cb[1] && center[2] == cb[2],
		       "MONEY ASSERTION (P1): the center pixel (over the emissive lamp) byte-matches lamp's legend colorHex" );
		Check( center[3] != 0, "the emissive lamp's center pixel is opaque (a real hit, not background)" );
	}

	// --- draft: the SAME luminaire is STILL hidden (studio-preview default
	// showLuminaires=false is unchanged).  Its center pixel is background
	// (transparent), while the non-emissive `side` sphere still renders. ---
	AgentRenderParams pd;
	pd.quality = AgentRenderQuality::Draft;
	AgentRenderResult rd = session->Render( pd );
	Check( rd.ok && rd.renderMode == "draft", "draft render of the emissive scene succeeds" );

	Decoded draftDec;
	Check( DecodePng( rd.png, draftDec ), "draft PNG decodes" );
	if( draftDec.w == 64 && draftDec.h == 64 ) {
		const Px& center = draftDec.at( 32, 32 );
		Check( center[3] == 0,
		       "MONEY ASSERTION (P1 pin): a DRAFT render STILL hides the luminaire -- its center pixel is background/transparent (preview semantics unchanged)" );
		// Sanity: the draft render is not simply all-background -- the
		// non-emissive `side` sphere renders (some pixel is opaque).
		std::uint64_t opaque = 0;
		for( std::size_t i = 0; i < draftDec.px.size(); ++i )
			if( draftDec.px[i][3] != 0 ) ++opaque;
		Check( opaque > 0, "the draft render is not all-background: the non-emissive `side` sphere renders" );
	}

	pJob->release();
}

//----------------------------------------------------------------------
// P2-1: the objectmap identity-palette generator, at a large object count,
// degrades ONLY the min-L1 separation while keeping BYTE-UNIQUENESS and
// ROUNDTRIP unconditionally.  Unit-tests BuildObjectMapPaletteBytes directly
// (via the AgentSession::ForTest_ seam) so no 2000-object scene is needed.
//----------------------------------------------------------------------
static void RunPaletteExhaustionUnitTest()
{
	std::printf( "=== AgentObjectMapTest: palette exhaustion invariants (P2-1) ===\n" );
	const std::size_t N = 2000;
	std::vector<std::array<unsigned char, 3> > bytes;
	unsigned int minDist = 24;
	AgentSession::ForTest_BuildObjectMapPaletteBytes( N, bytes, minDist );
	Check( bytes.size() == N, "generator returns exactly N=2000 triples" );

	// (a) BYTE-UNIQUENESS: no two ids share a colour.
	std::set<std::uint32_t> keys;
	bool allUnique = true;
	for( std::size_t i = 0; i < bytes.size(); ++i ) {
		const std::uint32_t k = ( (std::uint32_t)bytes[i][0] << 16 )
		                      | ( (std::uint32_t)bytes[i][1] <<  8 )
		                      |   (std::uint32_t)bytes[i][2];
		if( !keys.insert( k ).second ) { allUnique = false; break; }
	}
	Check( allUnique, "MONEY ASSERTION (P2-1): all 2000 palette colours are BYTE-UNIQUE (no duplicates even under exhaustion)" );

	// (b) ROUNDTRIP: every colour's half-LSB-centered linear pre-image
	// re-encodes (truncating sRGB*255) back to the exact byte.
	bool allRoundtrip = true;
	for( std::size_t i = 0; i < bytes.size() && allRoundtrip; ++i ) {
		for( int ch = 0; ch < 3; ++ch ) {
			const unsigned char b = bytes[i][ch];
			const Scalar L = ColorUtils::SRGBTransferFunctionInverse( ( (Scalar)b + 0.5 ) / 255.0 );
			Scalar enc = ColorUtils::SRGBTransferFunction( L < 0 ? 0 : ( L > 1 ? 1 : L ) );
			if( enc > 1.0 ) enc = 1.0;
			const unsigned char back = (unsigned char)( enc * 255.0 );
			if( back != b ) { allRoundtrip = false; break; }
		}
	}
	Check( allRoundtrip, "every one of the 2000 palette colours ROUND-TRIPS through the sRGB encode (quantizer contract holds under exhaustion)" );

	// (c) the degrade flag TRIPS: at 2000 objects the default 24 separation is
	// exhausted, so the generator relaxed below it (but stayed positive).
	Check( minDist < 24,
	       "MONEY ASSERTION (P2-1): at 2000 objects the min-L1 separation DEGRADED below the default 24 (the honest-note trigger)" );
	Check( minDist >= 1, "the degraded separation is still positive (>= 1)" );
}

//----------------------------------------------------------------------
// Closing-review P1: the EXHAUSTIVE last-resort scan (the branch below
// every golden-walk relaxation) starts at rgb=0 -- which IS the reserved
// background byte (0,0,0), round-trippable and (pre-fix) never interned
// in takenKeys.  The first id to reach that branch was therefore painted
// as LITERAL BACKGROUND, indistinguishable from "no object" in the PNG.
// The 2000-entry test above never reaches the exhaustive branch (the
// golden walk at min-L1 1 places everything), so this test STARVES the
// walk via the seam's forTestGoldenTries=0: ids past the 24-entry base
// list then take the exhaustive scan directly.
//----------------------------------------------------------------------
static void RunExhaustiveBranchReservedTest()
{
	std::printf( "=== AgentObjectMapTest: exhaustive-branch reserved-colour exclusion (closing P1) ===\n" );
	const std::size_t N = 30;   // 24 base entries + 6 forced-exhaustive ids
	std::vector<std::array<unsigned char, 3> > bytes;
	unsigned int minDist = 24;
	AgentSession::ForTest_BuildObjectMapPaletteBytes( N, bytes, minDist, /*forTestGoldenTries*/0 );
	Check( bytes.size() == N, "generator returns exactly N=30 triples with a starved golden walk" );

	// The reserved set the production path uses.
	const std::array<unsigned char, 3> background = { { 0, 0, 0 } };
	const std::array<unsigned char, 3> unknown    = { { 255, 0, 255 } };

	bool noReserved = true;
	bool allUnique  = true;
	std::set<std::uint32_t> keys;
	for( std::size_t i = 0; i < bytes.size(); ++i ) {
		if( bytes[i] == background || bytes[i] == unknown ) { noReserved = false; }
		const std::uint32_t k = ( (std::uint32_t)bytes[i][0] << 16 )
		                      | ( (std::uint32_t)bytes[i][1] <<  8 )
		                      |   (std::uint32_t)bytes[i][2];
		if( !keys.insert( k ).second ) { allUnique = false; }
	}
	Check( noReserved,
	       "MONEY ASSERTION (closing P1): NO palette entry equals the reserved background (0,0,0) or UNKNOWN (255,0,255) -- "
	       "the exhaustive scan skips interned reserved bytes (pre-fix, the first exhaustive id painted as background)" );
	Check( allUnique, "the forced-exhaustive entries remain byte-unique" );
	Check( minDist == 1, "the exhaustive branch reports the collapsed min-L1 of 1" );
}

//----------------------------------------------------------------------
// P3-a: an ASYNC objectmap render through the coordinator produces the SAME
// result fields (INCLUDING the legend) as the equivalent SYNC render.
//----------------------------------------------------------------------
static void RunAsyncObjectMapTest()
{
	std::printf( "=== AgentObjectMapTest: async + objectmap through the coordinator (P3-a) ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_async.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "async-objmap scene loads" ); return; }

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { controller.Stop(); pJob->release(); Check( false, "async-objmap session" ); return; }
	session->AttachController( &controller );

	// SYNC reference objectmap render.
	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult syncR = session->Render( p );
	Check( syncR.ok && syncR.renderMode == "objectmap", "SYNC objectmap render succeeds (reference)" );

	// ASYNC objectmap render + wait, then fetch the cached result.
	const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
	Check( ar.accepted && ar.renderJobId != 0, "async objectmap submission is accepted with a nonzero id" );
	const bool completed = session->RenderWait( ar.renderJobId, 10000 );
	Check( completed, "the async objectmap render completes within the timeout" );

	const AgentSession::AgentLastAsyncRenderResult last = session->LastAsyncRenderResult( ar.renderJobId );
	Check( last.found, "the async objectmap render's cached result is retrievable" );

	if( last.found ) {
		const AgentRenderResult& asyncR = last.result;
		// Same result-field set INCLUDING the legend: an async objectmap must
		// not silently drop the segmentation the sync path produces.
		Check( asyncR.ok == syncR.ok, "async objectmap ok == sync ok" );
		Check( asyncR.renderMode == "objectmap", "async result renderMode is \"objectmap\" (legend-bearing mode)" );
		Check( asyncR.width == syncR.width && asyncR.height == syncR.height, "async objectmap dims == sync dims" );
		Check( asyncR.legend.size() == syncR.legend.size() && asyncR.legend.size() == 3,
		       "MONEY ASSERTION (P3-a): the async objectmap legend has the SAME entry count as the sync render (INCLUDING legend, not dropped)" );
		bool legendSame = ( asyncR.legend.size() == syncR.legend.size() );
		for( std::size_t i = 0; legendSame && i < syncR.legend.size(); ++i )
			legendSame = ( asyncR.legend[i].name == syncR.legend[i].name
			            && asyncR.legend[i].colorHex == syncR.legend[i].colorHex
			            && asyncR.legend[i].pixelCount == syncR.legend[i].pixelCount );
		Check( legendSame, "the async objectmap legend is entry-for-entry identical to the sync legend (name+colorHex+pixelCount)" );
	}

	session->AttachController( nullptr );
	controller.Stop();
	pJob->release();
}

//----------------------------------------------------------------------
// Toolkit slice 3b: query_object_at {x,y} -- the cheap single-pixel
// companion to render mode:"objectmap".  IMPLEMENTATION CHOICE (a): reuses
// the SAME ephemeral objectmap pipeline wholesale (one identity render at
// the effective dims, then decode ONE pixel against that render's legend
// by exact colorHex byte) rather than a bespoke single-ray probe -- see
// AgentSession::QueryObjectAt's doc.
//
// (1) center-of-object query returns the right name for all 3 objects of
//     kScene3 (located via a reference objectmap render + pixel scan, so
//     this does not depend on hand-derived screen-space coordinates).
// (2) an empty-region (background) query -> hit:false, not an error.
// (6) query_object_at is side-effect-free on the retained Document.
//----------------------------------------------------------------------
static void RunQueryObjectAtCoreTest()
{
	std::printf( "=== AgentObjectMapTest: query_object_at core (Toolkit slice 3b) ===\n" );
	const std::string scenePath = WriteTemp( "rise_qoa_core.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "qoa core scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "qoa core session" ); return; }

	const std::string preDoc = session->ReadDocument();

	// Reference objectmap render: locate each object's pixels + legend colours.
	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult r = session->Render( p );
	Check( r.ok && r.legend.size() == 3, "reference objectmap render for query_object_at succeeds with 3 objects" );
	Decoded dec;
	Check( DecodePng( r.png, dec ), "reference objectmap PNG decodes" );

	// (1) center-of-object query for ALL THREE objects returns the right name.
	static const char* const kNames[3] = { "sph_a", "sph_b", "sph_c" };
	for( int i = 0; i < 3; ++i ) {
		const LegendEntry* le = FindLegend( r, kNames[i] );
		Check( le != nullptr, std::string( "legend has " ) + kNames[i] );
		if( !le ) continue;
		unsigned char cb[3];
		Check( HexToBytes( le->colorHex, cb ), std::string( "legend colorHex parses for " ) + kNames[i] );
		unsigned int px = 0, py = 0;
		Check( FindPixelForColor( dec, cb, px, py ), std::string( "found a pixel for " ) + kNames[i] );

		AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( (int)px, (int)py );
		Check( !qr.outOfRange, std::string( "query_object_at on " ) + kNames[i] + "'s own pixel is NOT out of range" );
		Check( qr.hit, std::string( "query_object_at hits " ) + kNames[i] );
		Check( qr.name == kNames[i],
		       std::string( "MONEY ASSERTION (1): query_object_at names " ) + kNames[i] +
		       " correctly (got '" + qr.name + "')" );
		Check( qr.width == 64 && qr.height == 64, "query_object_at reports the effective 64x64 dims" );
		Check( qr.pixelX == px && qr.pixelY == py, "query_object_at echoes back the queried pixel coords" );
	}

	// (2) empty-region (background) query -> hit:false, a STRUCTURED
	// result, not an error/failure.  Corner (0,0) is background (see
	// RunCoreTests' identical corner assertion).
	{
		AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( 0, 0 );
		Check( !qr.outOfRange, "an in-range background pixel is NOT reported as outOfRange" );
		Check( !qr.hit, "MONEY ASSERTION (2): query_object_at on an empty/background pixel reports hit:false" );
		Check( qr.name.empty(), "a miss carries an empty name" );
	}

	// (6) Document byte-identity: query_object_at (like render) never
	// mutates the retained Document.
	Check( session->ReadDocument() == preDoc,
	       "MONEY ASSERTION (6): query_object_at is side-effect-free on the Document" );

	pJob->release();
}

//----------------------------------------------------------------------
// (4) camera-override composition: aiming the ephemeral camera override at
// sph_a makes the SAME center pixel (32,32) -- which resolves to sph_b
// with the DEFAULT camera (see RunCoreTests) -- resolve to sph_a instead.
// Also re-checks (6) Document byte-identity under a camera override.
//----------------------------------------------------------------------
static void RunQueryObjectAtCameraOverrideTest()
{
	std::printf( "=== AgentObjectMapTest: query_object_at camera-override composition (4) ===\n" );
	const std::string scenePath = WriteTemp( "rise_qoa_cam.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "qoa cam scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "qoa cam session" ); return; }

	const std::string preDoc = session->ReadDocument();

	AgentQueryObjectParams qp;
	qp.camera.hasLocation = true;  qp.camera.location = "-1.7 0 2";
	qp.camera.hasLookAt   = true;  qp.camera.lookAt   = "-1.7 0 0";
	AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( 32, 32, qp );
	Check( !qr.outOfRange, "camera-override query center pixel is in-range" );
	Check( qr.hit && qr.name == "sph_a",
	       "MONEY ASSERTION (4): aiming the camera at sph_a makes the CENTER pixel query return sph_a "
	       "(the default camera resolves that same pixel to sph_b -- see RunCoreTests)" );

	Check( session->ReadDocument() == preDoc,
	       "(6) query_object_at with a camera override is ALSO side-effect-free on the Document" );

	pJob->release();
}

//----------------------------------------------------------------------
// (5) query_object_at works on a head with NO active production
// rasterizer -- it reuses the objectmap ephemeral pipeline, which (like
// quality:"draft") never dereferences the production rasterizer.  Pins the
// CORRECT behaviour (works) against the alternative a naive implementation
// might have: a production render on the SAME head fails honestly with
// "no active rasterizer", so this test also proves query_object_at does
// NOT share that fate.
//----------------------------------------------------------------------
static void RunQueryObjectAtNoRasterizerTest()
{
	std::printf( "=== AgentObjectMapTest: query_object_at on a rasterizer-less head (5) ===\n" );
	const std::string scenePath = WriteTemp( "rise_qoa_norast.RISEscene", kScene3NoRasterizer );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "qoa no-rasterizer scene loads" ); return; }
	Check( pJob->GetRasterizer() == nullptr, "the head genuinely has no active rasterizer" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "qoa no-rasterizer session" ); return; }

	// Sanity: a PRODUCTION render on this head fails honestly (the
	// pre-existing contract) -- query_object_at must NOT share that fate.
	AgentRenderResult prodRes = session->Render( -1 );
	Check( !prodRes.ok && prodRes.message == "no active rasterizer",
	       "sanity: a production render on this rasterizer-less head fails honestly" );

	AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( 32, 32 );
	Check( !qr.outOfRange,
	       "MONEY ASSERTION (5): query_object_at on a rasterizer-less head is NOT rejected as out-of-range" );
	Check( qr.width == 64 && qr.height == 64,
	       "MONEY ASSERTION (5): query_object_at resolves the effective 64x64 dims on a rasterizer-less head" );
	Check( qr.message.find( "no active rasterizer" ) == std::string::npos,
	       "query_object_at does not fail with the production-only \"no active rasterizer\" message" );
	Check( qr.hit && qr.name == "sph_b",
	       "MONEY ASSERTION (5): query_object_at correctly identifies sph_b at center on a rasterizer-less head "
	       "(mirrors the quality:\"draft\"/mode:\"objectmap\" gate -- never needs the production rasterizer)" );

	pJob->release();
}

//----------------------------------------------------------------------
// (3)+(7): the query_object_at WIRE verb over the JSON-RPC dispatcher.
// (7) an in-range query returns the documented result shape; (3) an
// out-of-range x or y is a clean -32602 (kInvalidParams), NOT a
// structured hit:false result.
//----------------------------------------------------------------------
static std::string QoaReq( double id, int x, int y )
{
	JsonValue params = JsonValue::MakeObject();
	params.set( "x", JsonValue::MakeNumber( (double)x ) );
	params.set( "y", JsonValue::MakeNumber( (double)y ) );
	JsonValue r = JsonValue::MakeObject();
	r.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	r.set( "id", JsonValue::MakeNumber( id ) );
	r.set( "method", JsonValue::MakeString( "query_object_at" ) );
	r.set( "params", params );
	return JsonSerialize( r );
}

static void RunQueryObjectAtWireTest()
{
	std::printf( "=== AgentObjectMapTest: query_object_at wire dispatch (3)+(7) ===\n" );
	const std::string scenePath = WriteTemp( "rise_qoa_wire.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "qoa wire scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "qoa wire session" ); return; }
	AgentRpcDispatcher rpc( std::move( session ) );

	// (7) a normal in-range query returns the documented shape.
	{
		const std::string resp = rpc.HandleLine( QoaReq( 1, 32, 32 ) );
		JsonValue env; std::string err;
		Check( JsonParse( resp, env, err ), "query_object_at wire response parses as JSON" );
		Check( !env.has( "error" ), "an in-range query_object_at is a JSON-RPC success" );
		const JsonValue& result = env.get( "result" );
		Check( result.get( "hit" ).isBool(), "result carries a boolean 'hit'" );
		Check( result.get( "name" ).isString(), "result carries a string 'name'" );
		Check( result.get( "kind" ).isString(), "result carries a string 'kind' (empty today)" );
		Check( result.get( "pixelX" ).asNumber( -1 ) == 32 && result.get( "pixelY" ).asNumber( -1 ) == 32,
		       "result echoes back the queried pixel coords" );
		Check( result.get( "width" ).asNumber( 0 ) == 64 && result.get( "height" ).asNumber( 0 ) == 64,
		       "result reports the effective 64x64 dims" );
		Check( result.get( "hit" ).asBool() && result.get( "name" ).asString() == "sph_b",
		       "MONEY ASSERTION (7): the wire verb identifies sph_b at the center pixel" );
	}

	// (3) out-of-range x -> -32602 (kInvalidParams), NOT a structured hit:false.
	{
		const std::string resp = rpc.HandleLine( QoaReq( 2, 999, 32 ) );
		JsonValue env; std::string err;
		Check( JsonParse( resp, env, err ), "out-of-range-x response parses as JSON" );
		Check( env.has( "error" ),
		       "MONEY ASSERTION (3): an out-of-range x is a JSON-RPC ERROR, not a structured hit:false" );
		Check( env.get( "error" ).get( "code" ).asNumber( 0 ) == -32602,
		       "the out-of-range-x error carries code -32602 (kInvalidParams)" );
	}

	// (3) a negative y -> also -32602.
	{
		const std::string resp = rpc.HandleLine( QoaReq( 3, 10, -1 ) );
		JsonValue env; std::string err;
		Check( JsonParse( resp, env, err ), "negative-y response parses as JSON" );
		Check( env.has( "error" ), "a negative y is ALSO a JSON-RPC error" );
		Check( env.get( "error" ).get( "code" ).asNumber( 0 ) == -32602,
		       "negative y carries -32602 too" );
	}

	pJob->release();
}

//----------------------------------------------------------------------
// Review-round P2 (slice 3b): EVERY other scene in this file places its
// objects at world y=0, so a y-axis regression -- a flipped or off-by-one
// vertical pixel mapping in the objectmap render OR in query_object_at --
// would pass the entire suite undetected (empirically proven: an injected
// y-flip left all prior checks green while an adversarial probe caught it
// immediately).  This test is that probe, made permanent: ONE sphere
// clearly offset in world +Y.  With `up 0 1 0` and the file's standard
// camera, world +Y is image TOP, i.e. SMALL pixel-y rows in the decoded
// PNG (the render maps a pixel via Point2(x, height-y)).
//   (a) render-side lock: the sphere's identity-colour bbox centre lands
//       in the TOP half of the PNG (a render y-flip moves it to the
//       bottom half);
//   (b) query-side lock: query_object_at at the bbox centre HITS the
//       sphere, and at the vertically MIRRORED point (same x, h-1-y)
//       MISSES (a query-only y-flip swaps these two outcomes).
// Assertions are derived from the render itself (bbox scan), so no
// hand-tuned pixel coordinates can rot.
//----------------------------------------------------------------------
// Fix-round-2 P1-A: query_object_at MUST NOT clobber the read_image cache.
//
// query_object_at fires a full ephemeral mode:"objectmap" Render()
// internally, and Render() unconditionally caches every success into the
// session's last-render pair (mLastPng / mLastSink) -- the pair ReadImage()
// serves and the `read_image` verb returns.  Unguarded, that leaves a flat
// SEGMENTATION image sitting where the caller's beauty frame was, so the
// natural agent sequence
//
//     render -> query_object_at -> read_image
//
// hands the model the segmentation image and it "judges" the beauty render
// from it.  That is the same symptom class as the per-session-cache bug this
// branch fixes; before that fix the GUI's session split masked it, because
// render and query_object_at landed on DIFFERENT sessions.  Now they share
// one, so the clobber is live.
//
// CompareToReference's split-mask render has had a stash/restore guard for
// exactly this reason since it shipped (locked by AgentViewportReadTest's
// "split: cache-survival" block); query_object_at never got one.  Both now
// take the SAME file-local RAII guard (EphemeralRenderCacheGuard in
// AgentSession.cpp) so they cannot drift apart again.
//
// RED-PROOF SHAPE: the beauty frame is rendered at 48x24 while the internal
// objectmap runs at the Document's authored 64x64, so the two PNGs cannot
// coincidentally compare equal -- byte-for-byte equality after the query is
// only reachable if the cache was genuinely preserved.
//
// The second half exercises ReadImage(maxEdge), which re-encodes from
// mLastSink -- a SEPARATE cached pointer from mLastPng, so the byte
// comparison above cannot speak for it.  Its failure mode is NOT "goes
// empty": drop only the sink half of the restore and mLastSink is left
// pointing at the EPHEMERAL OBJECTMAP sink (non-null), so the downscaling
// overload cheerfully serves a downscaled SEGMENTATION image -- exactly the
// shipped bug's shape, one pointer over.  A "did it come back non-empty?"
// assertion therefore proves nothing.  The DIMS are the discriminator: the
// beauty frame is deliberately NON-SQUARE (48x24, 2:1) while the objectmap
// is square (64x64), and ToPngDownscaled preserves aspect, so at maxEdge=16
// the beauty sink answers 16x8 and the objectmap sink answers 16x16.
// Asserting dw==16 && dh==8 EXACTLY is the only form of this check that can
// tell the two sinks apart.
//----------------------------------------------------------------------
static void RunQueryObjectAtPreservesImageCacheTest()
{
	std::printf( "=== AgentObjectMapTest: query_object_at preserves the read_image cache (P1-A) ===\n" );
	const std::string scenePath = WriteTemp( "rise_qoa_cache.RISEscene", kScene3 );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "qoa cache scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "qoa cache session" ); return; }

	// A BEAUTY frame at dims that differ from the authored film dims the
	// internal objectmap render will use -- and deliberately NON-SQUARE, so
	// the aspect-preserving downscale below identifies WHICH sink answered
	// (see the header comment's RED-PROOF SHAPE note).
	AgentRenderParams bp;
	bp.width  = 48;
	bp.height = 24;
	const AgentRenderResult beauty = session->Render( bp );
	Check( beauty.ok, "beauty render (48x24) succeeds" );
	Check( !beauty.png.empty(), "beauty render produces PNG bytes" );

	const std::vector<unsigned char> beforeQuery = session->ReadImage();
	Check( !beforeQuery.empty() && beforeQuery == beauty.png,
	       "read_image returns the beauty frame BEFORE query_object_at (baseline)" );

	// The pixel choice is irrelevant to this invariant -- any query runs the
	// same internal objectmap render -- but use a real object centre so the
	// query is a genuine success path, not an early-out.
	AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( 32, 32 );
	Check( qr.hit, "query_object_at hits an object (a real success path ran the internal render)" );
	Check( qr.width == 64 && qr.height == 64,
	       "the internal objectmap ran at the authored 64x64 -- DIFFERENT dims (and a DIFFERENT aspect) from the 48x24 beauty frame" );

	const std::vector<unsigned char> afterQuery = session->ReadImage();
	Check( !afterQuery.empty(),
	       "MONEY (P1-A): read_image is NOT emptied by query_object_at" );
	Check( afterQuery == beauty.png,
	       "MONEY (P1-A): read_image STILL returns the BEAUTY frame byte-for-byte after query_object_at "
	       "-- not the flat objectmap the internal render produced" );

	// mLastSink half: ReadImage(maxEdge) re-encodes from the cached SINK, a
	// separate pointer from mLastPng, so nothing above speaks for it.  A
	// restore that forgets the sink leaves mLastSink pointing at the
	// EPHEMERAL 64x64 OBJECTMAP sink -- non-null, so the call still returns
	// bytes; only the DIMS give it away (16x8 from the 48x24 beauty frame
	// vs 16x16 from the square objectmap).
	{
		unsigned int dw = 0, dh = 0;
		const std::vector<unsigned char> scaled = session->ReadImage( 16, dw, dh );
		Check( !scaled.empty(),
		       "MONEY (P1-A): ReadImage(maxEdge) still works after query_object_at -- the cached SINK was restored too" );
		Check( dw == 16 && dh == 8,
		       "MONEY (P1-A): the restored sink downscales the 2:1 48x24 BEAUTY frame to EXACTLY 16x8 "
		       "-- the square 64x64 objectmap sink would have answered 16x16" );
	}

	// A SECOND query must be idempotent on the cache, not merely
	// "restores once" -- a guard that restored a stale stash would drift
	// here.
	(void)session->QueryObjectAt( 32, 32 );
	Check( session->ReadImage() == beauty.png,
	       "MONEY (P1-A): a SECOND query_object_at leaves the beauty frame intact as well" );

	pJob->release();
	std::remove( scenePath.c_str() );
}

//----------------------------------------------------------------------
static const char* const kSceneVerticalAsym =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 0.7\n}\n\n"
	"standard_object\n{\n\tname high_ball\n\tgeometry geo\n\tmaterial mat\n\tposition 0 1.6 0\n}\n";

static void RunVerticalAsymmetryTest()
{
	std::printf( "=== AgentObjectMapTest: vertical-asymmetry y-mapping lock (3b review P2) ===\n" );
	const std::string scenePath = WriteTemp( "rise_objmap_vertasym.RISEscene", kSceneVerticalAsym );
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { pJob->release(); Check( false, "vert-asym scene loads" ); return; }
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	if( !session ) { pJob->release(); Check( false, "vert-asym session" ); return; }

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult r = session->Render( p );
	Check( r.ok && r.legend.size() == 1, "vert-asym objectmap render succeeds with 1 object" );
	Decoded dec;
	Check( DecodePng( r.png, dec ), "vert-asym objectmap PNG decodes" );
	const LegendEntry* le = FindLegend( r, "high_ball" );
	Check( le != nullptr, "legend carries high_ball" );
	if( !le || !r.ok ) { pJob->release(); std::remove( scenePath.c_str() ); return; }

	unsigned char cb[3];
	Check( HexToBytes( le->colorHex, cb ), "high_ball colorHex parses" );

	// Scan the decoded PNG for the sphere's colour bbox.
	unsigned int minX = dec.w, maxX = 0, minY = dec.h, maxY = 0;
	unsigned int found = 0;
	for( unsigned int y = 0; y < dec.h; ++y ) {
		for( unsigned int x = 0; x < dec.w; ++x ) {
			const Px& q = dec.at( x, y );
			if( q[0] == cb[0] && q[1] == cb[1] && q[2] == cb[2] ) {
				if( x < minX ) minX = x;
				if( x > maxX ) maxX = x;
				if( y < minY ) minY = y;
				if( y > maxY ) maxY = y;
				++found;
			}
		}
	}
	Check( found > 0, "the sphere's identity colour appears in the PNG" );
	if( !found ) { pJob->release(); std::remove( scenePath.c_str() ); return; }

	const unsigned int cx = ( minX + maxX ) / 2;
	const unsigned int cy = ( minY + maxY ) / 2;

	// (a) render-side lock: world +Y == image TOP == small pixel-y.
	Check( cy < dec.h / 2,
	       "MONEY ASSERTION (render y-mapping): the +Y sphere's bbox centre is in the TOP half of the PNG" );

	// (b) query-side lock: hit at the bbox centre, miss at the mirrored y.
	{
		AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( (int)cx, (int)cy );
		Check( qr.hit && qr.name == "high_ball",
		       "MONEY ASSERTION (query y-mapping): query_object_at at the bbox centre hits high_ball" );
	}
	{
		const unsigned int my = dec.h - 1 - cy;   // vertical mirror -- bottom half, empty
		AgentSession::AgentQueryObjectResult qr = session->QueryObjectAt( (int)cx, (int)my );
		Check( !qr.hit,
		       "MONEY ASSERTION (query y-mapping): the vertically MIRRORED point misses (a y-flip would swap these)" );
	}

	pJob->release();
	std::remove( scenePath.c_str() );
}

int main()
{
	RunCoreTests();
	RunCameraOverrideTest();
	RunInstanceArrayTest();
	RunCsgTest();
	RunIsolationTest();
	RunDownscaleGuardTest();
	RunExactnessRedProve();
	RunUnknownColorRedProve();
	RunEmissiveVisibilityTest();
	RunPaletteExhaustionUnitTest();
	RunExhaustiveBranchReservedTest();
	RunAsyncObjectMapTest();
	RunQueryObjectAtCoreTest();
	RunQueryObjectAtCameraOverrideTest();
	RunQueryObjectAtNoRasterizerTest();
	RunQueryObjectAtWireTest();
	RunQueryObjectAtPreservesImageCacheTest();
	RunVerticalAsymmetryTest();

	std::printf( "\nAgentObjectMapTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
