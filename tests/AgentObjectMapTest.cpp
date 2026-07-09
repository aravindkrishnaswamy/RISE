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
//  Self-contained: inline native-v7 scenes, OIDN off, no RISE_MEDIA_PATH.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
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
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
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

	std::printf( "\nAgentObjectMapTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
