//////////////////////////////////////////////////////////////////////
//
//  AgentViewModeRenderTest.cpp - closes a review P1: the agent
//    render{mode} widening (docs/gui/RENDER_MODES.md §8, commit
//    b13c5bc8 "feat(agent): render{mode} widened to the view-mode
//    registry") shipped with ZERO test coverage of its own.  This file
//    is that coverage.
//
//    Structural template: tests/AgentObjectMapTest.cpp (inline scene
//    construction, WriteTemp/Decoded/DecodePng/HexToBytes/FindLegend
//    harness idioms, AgentSession::Render at the "core" level rather
//    than through the RPC dispatcher for the pixel-level assertions).
//    Assert/report conventions: tests/ViewportRenderModeTest.cpp (which
//    covers the REGISTRY + caster-factory + SceneEditController surface
//    -- this file covers the AGENT surface: AgentSession::Render(
//    renderTarget=ViewMode), AgentRpc.cpp's `mode` string parser, and
//    the two hand-authored/generated tool-schema surfaces that must
//    stay in lockstep with the registry, per docs/gui/RENDER_MODES.md
//    §4: "Parity is tested: a unit test asserts the agent tool's
//    accepted mode-name set == the registry's, so adding a mode
//    without agent parity fails CI." -- this file (specifically
//    RunRpcModeParityTest / RunChatCodecModeParityTest /
//    RunMcpAdapterModeParityTest below) is that unit test.
//
//    Covers:
//    (1) END-TO-END per casterFactory registry mode (Normals/Depth/
//        Facets/Wireframe, discovered dynamically from
//        Implementation::GetViewportRenderModes -- never hardcoded, so
//        a future 5th mode is automatically exercised): a render on a
//        scene with BOTH a triangle mesh (displaced_geometry over a
//        box_geometry base -- a genuine, self-contained, no-external-
//        file ITriangleMeshGeometryIndexed under the hood, so
//        wireframe's edge shader actually has edges to find) AND an
//        analytic sphere succeeds; dims match; renderMode echoes the
//        registry name; the honest quality/samples-ignored note
//        appears in the message; per-mode pixel sanity (normals:
//        colour variety; depth: exact per-pixel grayscale with >1
//        distinct value; facets: grayscale-ish tones; wireframe: both
//        bright line pixels AND dim base pixels within the mesh's own
//        screen-space region, located via a reference objectmap
//        render).
//    (2) Film-dims restore: a view-mode render with a width/height
//        override renders AT the override, then the Document's Film
//        dims are restored afterward (mirrors AgentProposeRenderTest's
//        RunRestoreOnThrowTest pattern, read via
//        Job::GetScene()->GetFilm()).
//    (3) THREE-WAY PARITY (the review's core ask):
//        (a) AgentRpc.cpp: an unrecognized mode string is a clean
//            -32602 whose message enumerates EXACTLY {"beauty",
//            "objectmap"} UNION the registry's casterFactory names --
//            no more, no fewer.
//        (b) AgentChatCodecs.cpp: the HAND-SYNCED `render` tool schema
//            (KEEP IN SYNC BY HAND, per that file's own comment) that
//            ships to Anthropic/Gemini/OpenAI carries the SAME enum,
//            checked via AnthropicChatCodec::BuildRequest's actual wire
//            body (structural JSON navigation to
//            tools[render].input_schema.properties.mode.enum, not
//            substring guessing) -- exact set equality.
//        (c) AgentMcpAdapter.cpp: the MCP tools/list `render` tool's
//            `mode` property description is GENERATED from the
//            registry (DescribeViewModes()) -- every accepted name is a
//            substring, and the one OTHER registered-but-non-
//            casterFactory mode name ("preview") is confirmed ABSENT
//            (a non-parity-guard implementation could plausibly leak
//            the whole registry, not just the casterFactory subset).
//    (4) An invalid mode is refused BEFORE any render runs: a fresh
//        session's cached image stays empty (ReadImage returns 0
//        bytes) across the refused call.
//
//    Self-contained: inline native-v7 scene (a self-tessellating
//    displaced_geometry mesh -- no external mesh file, no CST golden
//    regen needed), OIDN off, no RISE_MEDIA_PATH.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/InMemoryRasterizerOutput.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
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
// Scene: a self-tessellating triangle mesh (displaced_geometry with NO
// displacement painter over a box_geometry base -- Job::AddDisplacedGeometry
// / RISE_API_CreateDisplacedGeometry builds a genuine
// ITriangleMeshGeometryIndexed under the hood via TessellateToMesh, so the
// wireframe view mode's edge shader -- which only stamps line pixels for
// geometries that carry RayIntersectionGeometric::bHasWireEdgeInfo, i.e.
// real triangle meshes, never analytic primitives -- has actual edges to
// find) on the LEFT, plus an analytic sphere on the RIGHT (non-overlapping
// in screen space, so a reference objectmap render can cleanly locate the
// mesh's own pixel region for the wireframe assertion below).
//----------------------------------------------------------------------
static const char* const kSceneMeshAndSphere =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 96\n\theight 72\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"box_geometry\n{\n\tname boxbase\n\twidth 1.3\n\theight 1.3\n\tdepth 1.3\n}\n\n"
	"displaced_geometry\n{\n\tname mesh_geo\n\tbase_geometry boxbase\n\tdetail 6\n}\n\n"
	"standard_object\n{\n\tname mesh_obj\n\tgeometry mesh_geo\n\tmaterial mat\n\tposition -1.3 0 0\n}\n\n"
	"sphere_geometry\n{\n\tname sph_geo\n\tradius 0.9\n}\n\n"
	"standard_object\n{\n\tname sph_obj\n\tgeometry sph_geo\n\tmaterial mat\n\tposition 1.3 0 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

//----------------------------------------------------------------------
// X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis") coverage scene:
// kSceneMeshAndSphere PLUS a transmissive glass sphere (dielectric_material,
// CouldLightPassThrough() == true) positioned IN FRONT of mesh_obj (same x,
// closer z to the camera at (0,0,6)) -- big enough on screen to fully
// occlude mesh_obj's own silhouette, small enough and far enough away not to
// touch mesh_obj or bleed into sph_obj's region.  A SEPARATE scene from
// kSceneMeshAndSphere (rather than extending it in place) so the existing
// per-mode end-to-end test's reference-objectmap mesh bbox lookup above is
// untouched by the new occluder.
//----------------------------------------------------------------------
static const std::string kSceneMeshSphereGlass =
	std::string( kSceneMeshAndSphere ) +
	"sphere_geometry\n{\n\tname glass_geo\n\tradius 0.6\n}\n\n"
	"dielectric_material\n{\n\tname glass_mat\n\ttau 1.0 1.0 1.0\n\tior 1.5\n}\n\n"
	"standard_object\n{\n\tname glass_obj\n\tgeometry glass_geo\n\tmaterial glass_mat\n\tposition -1.3 0 1.7\n}\n";

//----------------------------------------------------------------------
// Helpers (mirrors tests/AgentObjectMapTest.cpp's idioms).
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

// Decode PNG bytes into raw stored bytes -- reading with
// eColorSpace_Rec709RGB_Linear does a bare byte/255 with NO transfer-
// function conversion, so round(v*255) recovers the exact stored byte.
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

static const LegendEntry* FindLegend( const AgentRenderResult& r, const std::string& name )
{
	for( std::size_t i = 0; i < r.legend.size(); ++i )
		if( r.legend[i].name == name ) return &r.legend[i];
	return nullptr;
}

// Bounding box (in decoded-PNG pixel space) of every pixel whose bytes
// exactly equal `rgb`.  Returns false (found == 0) if no pixel matches.
struct BBox { unsigned int minX, maxX, minY, maxY; unsigned int found; };
static BBox ScanBBoxForColor( const Decoded& d, const unsigned char rgb[3] )
{
	BBox b{ d.w, 0, d.h, 0, 0 };
	for( unsigned int y = 0; y < d.h; ++y ) {
		for( unsigned int x = 0; x < d.w; ++x ) {
			const Px& q = d.at( x, y );
			if( q[0] == rgb[0] && q[1] == rgb[1] && q[2] == rgb[2] ) {
				if( x < b.minX ) b.minX = x;
				if( x > b.maxX ) b.maxX = x;
				if( y < b.minY ) b.minY = y;
				if( y > b.maxY ) b.maxY = y;
				++b.found;
			}
		}
	}
	return b;
}

// The full accepted-mode-name set every one of the three agent surfaces
// (AgentRpc.cpp's parser, AgentChatCodecs.cpp's hand-synced schema,
// AgentMcpAdapter.cpp's generated schema) must agree on: "beauty" +
// "objectmap" (hardcoded in all three) UNION the registry's casterFactory
// entries (Normals/Depth/Facets/Wireframe today, discovered dynamically so
// a future 5th mode doesn't silently rot this test into a false pass).
static std::set<std::string> ExpectedAcceptedModes()
{
	std::set<std::string> expected;
	expected.insert( "beauty" );
	expected.insert( "objectmap" );
	unsigned int count = 0;
	const Implementation::ViewportRenderModeInfo* modes = Implementation::GetViewportRenderModes( count );
	for( unsigned int i = 0; i < count; ++i )
		if( modes[i].casterFactory )
			expected.insert( modes[i].name );
	return expected;
}

// Every double-quoted substring in `s`, in order -- used to pull the
// accepted-name list out of AgentRpc.cpp's dynamically-built error message
// ("...must be one of \"beauty\", \"objectmap\", \"normals\", ...").
static std::vector<std::string> ExtractQuotedTokens( const std::string& s )
{
	std::vector<std::string> out;
	std::size_t i = 0;
	while( i < s.size() ) {
		if( s[i] == '"' ) {
			const std::size_t j = s.find( '"', i + 1 );
			if( j == std::string::npos ) break;
			out.push_back( s.substr( i + 1, j - i - 1 ) );
			i = j + 1;
		} else {
			++i;
		}
	}
	return out;
}

//----------------------------------------------------------------------
// (1) END-TO-END per casterFactory registry mode.
//----------------------------------------------------------------------
static void RunPerModeEndToEndTest()
{
	std::printf( "=== AgentViewModeRenderTest: per-mode end-to-end (Normals/Depth/Facets/Wireframe) ===\n" );
	const std::string scenePath = WriteTemp( "rise_viewmode_e2e.RISEscene", kSceneMeshAndSphere );
	Check( !scenePath.empty(), "wrote the mesh+sphere scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "mesh+sphere scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	// Reference objectmap render (no overrides -- SAME default 96x72 film +
	// camera every per-mode render below uses) to locate mesh_obj's own
	// pixel bounding box, for the wireframe bright/dim assertion.
	AgentRenderParams objP;
	objP.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult objR = session->Render( objP );
	Check( objR.ok && objR.renderMode == "objectmap", "reference objectmap render succeeds" );
	Decoded objDec;
	Check( DecodePng( objR.png, objDec ), "reference objectmap PNG decodes" );
	const LegendEntry* meshLegend = FindLegend( objR, "mesh_obj" );
	Check( meshLegend != nullptr, "reference objectmap legend carries mesh_obj" );
	BBox meshBBox{ 0, 0, 0, 0, 0 };
	if( meshLegend ) {
		unsigned char cb[3];
		Check( HexToBytes( meshLegend->colorHex, cb ), "mesh_obj colorHex parses" );
		meshBBox = ScanBBoxForColor( objDec, cb );
		Check( meshBBox.found > 0, "mesh_obj occupies a nonzero pixel region in the reference objectmap render" );
	}

	unsigned int modeCount = 0;
	const Implementation::ViewportRenderModeInfo* modes = Implementation::GetViewportRenderModes( modeCount );
	unsigned int casterFactoryModesTested = 0;

	for( unsigned int i = 0; i < modeCount; ++i ) {
		if( !modes[i].casterFactory ) continue;
		++casterFactoryModesTested;
		const std::string modeName = modes[i].name;
		std::printf( "  -- mode \"%s\" --\n", modeName.c_str() );

		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = modes[i].mode;
		// A positive samples request that MUST be ignored -- this is what
		// trips the honest quality/samples-ignored note in the message
		// (RenderCore_'s isViewMode tail only appends it when
		// wantSamplesOverride || quality==Draft).
		p.samples = 8;

		AgentRenderResult r = session->Render( p );
		Check( r.ok, modeName + ": render succeeds" );
		Check( r.renderMode == modeName, modeName + ": renderMode echoes the registry name" );
		Check( r.width == 96 && r.height == 72, modeName + ": render dims are the authored 96x72" );
		Check( !r.samplesOverridden, modeName + ": samplesOverridden is false (the override is honestly refused)" );
		Check( r.effectiveSamples == 1, modeName + ": effectiveSamples reports the single-ray 1 spp exactness invariant" );
		const std::string expectedNote =
			" (mode:" + modeName + " is a single-pass diagnostic render; quality/samples ignored)";
		Check( r.message.find( expectedNote ) != std::string::npos,
		       modeName + ": message carries the honest quality/samples-ignored note" );

		Decoded dec;
		Check( DecodePng( r.png, dec ), modeName + ": PNG decodes" );
		if( dec.w != 96 || dec.h != 72 ) continue;

		if( modeName == "normals" ) {
			std::set<std::uint32_t> distinctColors;
			unsigned char rmin = 255, rmax = 0;
			for( std::size_t px = 0; px < dec.px.size(); ++px ) {
				const Px& q = dec.px[px];
				if( q[3] == 0 ) continue;   // background miss
				const std::uint32_t key = ( (std::uint32_t)q[0] << 16 ) | ( (std::uint32_t)q[1] << 8 ) | q[2];
				distinctColors.insert( key );
				if( q[0] < rmin ) rmin = q[0];
				if( q[0] > rmax ) rmax = q[0];
			}
			Check( distinctColors.size() > 5,
			       "normals: MONEY ASSERTION -- more than 5 distinct colours across the image (not all-equal)" );
			Check( (unsigned int)( rmax - rmin ) > 10,
			       "normals: the R channel spreads over a nontrivial range (surfaces genuinely face different ways)" );
		} else if( modeName == "depth" ) {
			bool allGray = true;
			std::set<unsigned char> distinctGray;
			unsigned char grayMin = 255, grayMax = 0;
			for( std::size_t px = 0; px < dec.px.size(); ++px ) {
				const Px& q = dec.px[px];
				if( q[0] != q[1] || q[1] != q[2] ) { allGray = false; }
				distinctGray.insert( q[0] );
				if( q[0] < grayMin ) grayMin = q[0];
				if( q[0] > grayMax ) grayMax = q[0];
			}
			Check( allGray, "depth: MONEY ASSERTION -- every pixel is exactly grayscale (r==g==b)" );
			// Depth auto-windowing (docs/gui/RENDER_MODES.md "Depth auto-
			// windowing"): the active window is calibrated to the VISIBLE
			// hit-distance range in THIS frame, so a scene whose visible depth
			// span is a small fraction of the whole-scene extent (like this
			// mesh+sphere fixture) should show STRONG contrast, not the ~1%
			// band the old fixed-scene-diagonal normalization produced.
			const bool strongContrast =
				distinctGray.size() >= 30 ||
				( (unsigned int)( grayMax - grayMin ) > (unsigned int)( 0.3 * 255 ) );
			Check( strongContrast,
			       "depth: MONEY ASSERTION -- auto-windowed depth render shows strong contrast "
			       "(>=30 distinct gray levels, or a min/max spread > 0.3 in byte terms), not a "
			       "near-flat band" );
		} else if( modeName == "facets" ) {
			bool allGrayish = true;
			unsigned int hitCount = 0;
			for( std::size_t px = 0; px < dec.px.size(); ++px ) {
				const Px& q = dec.px[px];
				if( q[3] == 0 ) continue;   // background miss
				++hitCount;
				const int dRG = (int)q[0] - (int)q[1];
				const int dBG = (int)q[2] - (int)q[1];
				if( std::abs( dRG ) > 15 || std::abs( dBG ) > 15 ) allGrayish = false;
			}
			Check( hitCount > 0, "facets: at least one hit pixel to evaluate" );
			Check( allGrayish, "facets: MONEY ASSERTION -- every hit pixel is grayscale-ish (headlamp-shaded geometric normal, no colour)" );
		} else if( modeName == "wireframe" ) {
			Check( meshBBox.found > 0, "wireframe: have a mesh bbox to scan from the reference objectmap render" );
			if( meshBBox.found > 0 ) {
				unsigned int bright = 0, dim = 0;
				for( unsigned int y = meshBBox.minY; y <= meshBBox.maxY; ++y ) {
					for( unsigned int x = meshBBox.minX; x <= meshBBox.maxX; ++x ) {
						const Px& q = dec.at( x, y );
						if( q[3] == 0 ) continue;   // background miss inside the bbox (a non-rectangular silhouette)
						const int avg = ( (int)q[0] + (int)q[1] + (int)q[2] ) / 3;
						if( avg > 200 ) ++bright;
						else if( avg < 180 ) ++dim;
					}
				}
				Check( bright >= 3,
				       "wireframe: MONEY ASSERTION -- at least 3 bright LINE pixels within the mesh's own region (near-white edge colour 0.92/0.96/1.0)" );
				Check( dim > 0,
				       "wireframe: MONEY ASSERTION -- at least one dim BASE pixel within the mesh's own region (the image is not uniformly bright)" );
			}
		}
	}
	Check( casterFactoryModesTested >= 4, "at least the 4 P1 casterFactory modes (normals/depth/facets/wireframe) were exercised" );

	pJob->release();
}

//----------------------------------------------------------------------
// (2) Film-dims restore after a view-mode render with a width/height
// override.
//----------------------------------------------------------------------
static void RunFilmRestoreTest()
{
	std::printf( "=== AgentViewModeRenderTest: film-dims restore after a width/height override ===\n" );
	const std::string scenePath = WriteTemp( "rise_viewmode_restore.RISEscene", kSceneMeshAndSphere );
	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "restore-test scene loads" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "restore-test session wraps" );
	if( !session ) { pJob->release(); return; }

	const IScenePriv* scenePriv = pJob->GetScene();
	const IFilm* filmBefore = scenePriv ? scenePriv->GetFilm() : nullptr;
	Check( filmBefore != nullptr, "the scene has a Film to capture pre-override dims from" );
	const unsigned int origW = filmBefore ? filmBefore->GetWidth()  : 0;
	const unsigned int origH = filmBefore ? filmBefore->GetHeight() : 0;
	Check( origW == 96 && origH == 72, "pre-override Film dims are the Document's authored 96x72" );

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ViewMode;
	p.viewMode     = Implementation::ViewportRenderMode::Depth;
	p.width  = 40;
	p.height = 30;
	AgentRenderResult r = session->Render( p );
	Check( r.ok, "view-mode render with a width/height override succeeds" );
	Check( r.width == 40 && r.height == 30, "the render itself used the OVERRIDE dims 40x30" );

	const IScenePriv* scenePrivAfter = pJob->GetScene();
	const IFilm* filmAfter = scenePrivAfter ? scenePrivAfter->GetFilm() : nullptr;
	Check( filmAfter != nullptr, "the scene still has a Film after the view-mode render" );
	Check( filmAfter && filmAfter->GetWidth() == origW && filmAfter->GetHeight() == origH,
	       "MONEY ASSERTION: Film dims are RESTORED to the pre-override 96x72 after the view-mode render" );

	pJob->release();
}

//----------------------------------------------------------------------
// (3a) AgentRpc.cpp: an invalid mode's -32602 error message enumerates
// EXACTLY the accepted-name set.
//----------------------------------------------------------------------
static std::string RenderModeReq( double id, const std::string& mode )
{
	JsonValue params = JsonValue::MakeObject();
	params.set( "mode", JsonValue::MakeString( mode ) );
	JsonValue r = JsonValue::MakeObject();
	r.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	r.set( "id", JsonValue::MakeNumber( id ) );
	r.set( "method", JsonValue::MakeString( "render" ) );
	r.set( "params", params );
	return JsonSerialize( r );
}

static void RunRpcModeParityTest()
{
	std::printf( "=== AgentViewModeRenderTest: (3a) AgentRpc.cpp accepted-mode-name parity ===\n" );
	const std::string scenePath = WriteTemp( "rise_viewmode_rpc.RISEscene", kSceneMeshAndSphere );
	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "rpc-parity scene loads" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "rpc-parity session wraps" );
	if( !session ) { pJob->release(); return; }

	AgentRpcDispatcher rpc( std::move( session ) );
	const std::string resp = rpc.HandleLine( RenderModeReq( 1, "definitely_not_a_mode" ) );

	JsonValue env; std::string perr;
	Check( JsonParse( resp, env, perr ), "invalid-mode response parses as JSON" );
	Check( env.has( "error" ), "an unrecognized mode is a JSON-RPC error" );
	Check( env.get( "error" ).get( "code" ).asNumber( 0 ) == -32602,
	       "MONEY ASSERTION (4): the invalid-mode error carries code -32602 (kInvalidParams)" );

	const std::string message = env.get( "error" ).get( "message" ).asString();
	const std::vector<std::string> tokens = ExtractQuotedTokens( message );
	std::set<std::string> actual( tokens.begin(), tokens.end() );
	const std::set<std::string> expected = ExpectedAcceptedModes();

	Check( actual == expected,
	       "MONEY ASSERTION (3a): the -32602 message's quoted name set EQUALS EXACTLY "
	       "{\"beauty\",\"objectmap\"} UNION the registry's casterFactory names -- no more, no fewer" );

	pJob->release();
}

//----------------------------------------------------------------------
// (4) An invalid mode never runs a render -- the session's cached image
// stays empty across the refused call.
//----------------------------------------------------------------------
static void RunNoRenderOnInvalidModeTest()
{
	std::printf( "=== AgentViewModeRenderTest: (4) invalid mode does not run a render ===\n" );
	const std::string scenePath = WriteTemp( "rise_viewmode_norender.RISEscene", kSceneMeshAndSphere );
	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "no-render-test scene loads" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "no-render-test session wraps" );
	if( !session ) { pJob->release(); return; }

	AgentRpcDispatcher rpc( std::move( session ) );
	Check( rpc.Session() != nullptr, "the dispatcher exposes the wrapped session" );
	if( !rpc.Session() ) { return; }

	unsigned int w0 = 0, h0 = 0;
	const std::vector<unsigned char> before = rpc.Session()->ReadImage( 0, w0, h0 );
	Check( before.empty(), "a fresh session's cached image is empty before any render" );

	const std::string resp = rpc.HandleLine( RenderModeReq( 1, "definitely_not_a_mode" ) );
	JsonValue env; std::string perr;
	Check( JsonParse( resp, env, perr ), "invalid-mode response parses as JSON" );
	Check( env.has( "error" ), "the invalid-mode call is refused as a JSON-RPC error" );

	unsigned int w1 = 0, h1 = 0;
	const std::vector<unsigned char> after = rpc.Session()->ReadImage( 0, w1, h1 );
	Check( after.empty(),
	       "MONEY ASSERTION (4): the cached image is STILL empty after the refused call -- no render ran" );
}

//----------------------------------------------------------------------
// (3b) AgentChatCodecs.cpp: the HAND-SYNCED `render` tool schema's `mode`
// enum, read structurally off AnthropicChatCodec::BuildRequest's actual
// wire body -- exact set equality against the registry.
//----------------------------------------------------------------------
static void RunChatCodecModeParityTest()
{
	std::printf( "=== AgentViewModeRenderTest: (3b) AgentChatCodecs.cpp hand-synced schema parity ===\n" );

	AnthropicChatCodec codec;
	const ChatHttpRequest req = codec.BuildRequest(
		"claude-test-model", /*apiKey*/std::string(), "system prompt",
		std::vector<std::string>() );
	Check( !req.body.empty(), "BuildRequest produces a non-empty body" );

	JsonValue root; std::string perr;
	Check( JsonParse( req.body, root, perr ), "the request body parses as JSON" );

	const JsonValue& tools = root.get( "tools" );
	Check( tools.isArray() && tools.size() > 0, "the body carries a non-empty tools array" );

	JsonValue renderTool = JsonValue::MakeNull();
	for( std::size_t i = 0; i < tools.size(); ++i ) {
		if( tools.at( i ).get( "name" ).asString() == "render" ) { renderTool = tools.at( i ); break; }
	}
	Check( renderTool.isObject(), "found the \"render\" tool in the tools array" );
	if( !renderTool.isObject() ) return;

	const JsonValue& modeEnum = renderTool.get( "input_schema" ).get( "properties" ).get( "mode" ).get( "enum" );
	Check( modeEnum.isArray() && modeEnum.size() > 0, "render tool's input_schema.properties.mode.enum is a non-empty array" );

	std::set<std::string> actual;
	for( std::size_t i = 0; i < modeEnum.size(); ++i ) actual.insert( modeEnum.at( i ).asString() );
	const std::set<std::string> expected = ExpectedAcceptedModes();

	Check( actual == expected,
	       "MONEY ASSERTION (3b): the chat-codec schema's mode enum EQUALS EXACTLY "
	       "{\"beauty\",\"objectmap\"} UNION the registry's casterFactory names -- the hand-synced "
	       "literal has not drifted from the registry" );
}

//----------------------------------------------------------------------
// (3c) AgentMcpAdapter.cpp: the GENERATED `render` tool's `mode` property
// description carries every accepted name, and does NOT leak the one
// OTHER registered-but-non-casterFactory mode name ("preview").
//----------------------------------------------------------------------
static void RunMcpAdapterModeParityTest()
{
	std::printf( "=== AgentViewModeRenderTest: (3c) AgentMcpAdapter.cpp generated schema parity ===\n" );
	const std::string scenePath = WriteTemp( "rise_viewmode_mcp.RISEscene", kSceneMeshAndSphere );
	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "mcp-parity scene loads" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "mcp-parity session wraps" );
	if( !session ) { pJob->release(); return; }

	AgentMcpAdapter mcp( std::move( session ) );

	JsonValue req = JsonValue::MakeObject();
	req.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	req.set( "id", JsonValue::MakeNumber( 1 ) );
	req.set( "method", JsonValue::MakeString( "tools/list" ) );
	req.set( "params", JsonValue::MakeObject() );
	const std::string resp = mcp.HandleLine( JsonSerialize( req ) );

	JsonValue env; std::string perr;
	Check( JsonParse( resp, env, perr ), "tools/list response parses as JSON" );
	Check( !env.has( "error" ), "tools/list is a JSON-RPC success" );

	const JsonValue& toolsList = env.get( "result" ).get( "tools" );
	Check( toolsList.isArray() && toolsList.size() > 0, "tools/list result.tools is a non-empty array" );

	JsonValue renderTool = JsonValue::MakeNull();
	for( std::size_t i = 0; i < toolsList.size(); ++i ) {
		if( toolsList.at( i ).get( "name" ).asString() == "render" ) { renderTool = toolsList.at( i ); break; }
	}
	Check( renderTool.isObject(), "found the \"render\" tool in tools/list" );
	if( !renderTool.isObject() ) return;

	// Read the ACTUAL (unescaped) description text via .asString() rather
	// than substring-searching the re-serialized JSON: JsonSerialize
	// re-escapes the description's embedded literal quote characters
	// (`"beauty"` inside the prose) back to `\"beauty\"`, so a raw
	// "\"beauty\"" search over the serialized text never matches.
	const std::string modeDesc =
		renderTool.get( "inputSchema" ).get( "properties" ).get( "mode" ).get( "description" ).asString();
	Check( !modeDesc.empty(), "the render tool's mode property carries a non-empty description" );

	const std::set<std::string> expected = ExpectedAcceptedModes();
	for( const std::string& name : expected ) {
		Check( modeDesc.find( "\"" + name + "\"" ) != std::string::npos,
		       "MCP render tool mode description mentions accepted mode name \"" + name + "\"" );
	}
	// The registry's ONE other registered name that is NOT casterFactory
	// (Preview -- see ViewportRenderModeTest.cpp R1/R3) must NOT leak into
	// the generated description: DescribeViewModes() filters on
	// `casterFactory`, not "every registered name".
	Check( modeDesc.find( "\"preview\"" ) == std::string::npos,
	       "MONEY ASSERTION (3c): the non-casterFactory registry entry \"preview\" is NOT among the "
	       "MCP render tool's mode names (DescribeViewModes filters correctly, doesn't leak the whole registry)" );
}

//----------------------------------------------------------------------
// (5) X-ray axis coverage (docs/gui/RENDER_MODES.md "X-ray axis"), DEFAULT
// ON (2026-07-17): facets and depth with the DEFAULT (see-through) vs an
// EXPLICIT xray:false over a transmissive glass sphere placed IN FRONT of
// the mesh, plus the honest-ignored-under-beauty note.
//----------------------------------------------------------------------
static void RunXrayCoverageTest()
{
	std::printf( "=== AgentViewModeRenderTest: (5) X-ray axis coverage ===\n" );
	const std::string scenePath = WriteTemp( "rise_viewmode_xray.RISEscene", kSceneMeshSphereGlass );
	Check( !scenePath.empty(), "wrote the mesh+sphere+glass scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "xray scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "xray session wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	// Reference objectmap render (no xray -- objectmap doesn't support it)
	// to locate glass_obj's own pixel bounding box: since glass_obj sits IN
	// FRONT of mesh_obj, it should fully occlude mesh_obj's silhouette here.
	AgentRenderParams objP;
	objP.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult objR = session->Render( objP );
	Check( objR.ok && objR.renderMode == "objectmap", "xray-scene reference objectmap render succeeds" );
	Decoded objDec;
	Check( DecodePng( objR.png, objDec ), "xray-scene reference objectmap PNG decodes" );
	const LegendEntry* glassLegend = FindLegend( objR, "glass_obj" );
	Check( glassLegend != nullptr, "reference objectmap legend carries glass_obj" );
	BBox glassBBox{ 0, 0, 0, 0, 0 };
	if( glassLegend ) {
		unsigned char cb[3];
		Check( HexToBytes( glassLegend->colorHex, cb ), "glass_obj colorHex parses" );
		glassBBox = ScanBBoxForColor( objDec, cb );
		Check( glassBBox.found > 0, "glass_obj occupies a nonzero pixel region (occludes the mesh from the camera)" );
	}

	// (a) facets at the DEFAULT (xray defaults TRUE -- 2026-07-17) --
	// should show the mesh through the glass with NO extra param.
	AgentRenderParams facetsDefault;
	facetsDefault.renderTarget = AgentRenderTarget::ViewMode;
	facetsDefault.viewMode     = Implementation::ViewportRenderMode::Facets;
	AgentRenderResult rFacetsDefault = session->Render( facetsDefault );
	Check( rFacetsDefault.ok, "facets at the default (xray implicitly true) succeeds" );
	Check( rFacetsDefault.message.find( "xray" ) != std::string::npos,
	       "facets-at-default message notes xray is active" );
	Decoded decFacetsDefault;
	Check( DecodePng( rFacetsDefault.png, decFacetsDefault ), "facets-at-default PNG decodes" );

	// (b) facets with an EXPLICIT xray:false -- shows the glass sphere's
	// own surface instead of the mesh underneath it.
	AgentRenderParams facetsNoXray = facetsDefault;
	facetsNoXray.xray = false;
	AgentRenderResult rFacetsNo = session->Render( facetsNoXray );
	Check( rFacetsNo.ok, "facets with explicit xray:false succeeds" );
	Decoded decFacetsNo;
	Check( DecodePng( rFacetsNo.png, decFacetsNo ), "facets-xray-false PNG decodes" );

	if( glassBBox.found > 0 &&
	    decFacetsNo.w == decFacetsDefault.w && decFacetsNo.h == decFacetsDefault.h ) {
		unsigned int differing = 0;
		for( unsigned int y = glassBBox.minY; y <= glassBBox.maxY; ++y ) {
			for( unsigned int x = glassBBox.minX; x <= glassBBox.maxX; ++x ) {
				const Px& a = decFacetsNo.at( x, y );
				const Px& b = decFacetsDefault.at( x, y );
				if( a[0] != b[0] || a[1] != b[1] || a[2] != b[2] ) ++differing;
			}
		}
		Check( differing > 0,
		       "MONEY ASSERTION (a/b): facets at the DEFAULT shows DIFFERENT pixels than an explicit "
		       "xray:false, over the glass sphere's own screen region -- the mesh shows through the glass "
		       "by default, and xray:false switches back to the glass surface itself" );
	}

	// (c) depth at the DEFAULT vs an EXPLICIT xray:false -- should differ
	// (the default sees through to the mesh, which is farther away than
	// the glass sphere's own surface).
	AgentRenderParams depthDefault;
	depthDefault.renderTarget = AgentRenderTarget::ViewMode;
	depthDefault.viewMode     = Implementation::ViewportRenderMode::Depth;
	AgentRenderResult rDepthDefault = session->Render( depthDefault );
	Check( rDepthDefault.ok, "depth at the default (xray implicitly true) succeeds" );
	Decoded decDepthDefault;
	Check( DecodePng( rDepthDefault.png, decDepthDefault ), "depth-at-default PNG decodes" );

	AgentRenderParams depthNoXray = depthDefault;
	depthNoXray.xray = false;
	AgentRenderResult rDepthNo = session->Render( depthNoXray );
	Check( rDepthNo.ok, "depth with explicit xray:false succeeds" );
	Decoded decDepthNo;
	Check( DecodePng( rDepthNo.png, decDepthNo ), "depth-xray-false PNG decodes" );

	if( decDepthNo.w == decDepthDefault.w && decDepthNo.h == decDepthDefault.h ) {
		unsigned int differing = 0;
		for( std::size_t px = 0; px < decDepthNo.px.size(); ++px ) {
			if( decDepthNo.px[px][0] != decDepthDefault.px[px][0] ) ++differing;
		}
		Check( differing > 0,
		       "MONEY ASSERTION (c): depth at the DEFAULT differs from depth with explicit xray:false "
		       "somewhere in the frame" );
	}

	// (d) xray at the default (true) with mode:"beauty" (the default
	// renderTarget) is ACCEPTED, not rejected, and carries the honest
	// ignored note.
	AgentRenderParams beautyXray;
	// renderTarget stays Beauty (the default); xray stays at its own
	// default (true) -- deliberately NOT set here, to prove the default
	// itself (not just an explicit xray:true) is honestly ignored under
	// mode:beauty.
	AgentRenderResult rBeautyXray = session->Render( beautyXray );
	Check( rBeautyXray.ok, "xray (default true) with mode:beauty is accepted (not rejected)" );
	Check( rBeautyXray.message.find( "ignored" ) != std::string::npos,
	       "MONEY ASSERTION (d): xray (default true) under mode:beauty carries an honest 'ignored' note in the message" );

	pJob->release();
}

//----------------------------------------------------------------------
// (6) Review P2 coverage: RayCaster::CastRay{,NM,HWSS}'s eRayView
// luminaire-suppression check (`bShowLuminaires`) must re-apply to the
// RESOLVED hit after ResolveXrayView_, not just the ORIGINAL hit -- a
// resolved emitter behind glass must be suppressed exactly like a
// directly-visible one on a showLuminaires=false caster.  Every x-ray
// render exercised elsewhere in this file goes through AgentSession's
// ViewMode path, which ALWAYS uses a showLuminaires=TRUE caster
// (InteractiveViewModeRayCaster -- see its class doc); AgentSession
// never wires x-ray onto the showLuminaires=false studio-preview/draft
// pipeline (CreateInteractiveMaterialPreviewPipeline) at all -- the
// `xray` param's C-ABI/agent surface only reaches
// CreateInteractiveViewModeCaster.  So the fix is exercised one level
// BELOW AgentSession here, directly against the showLuminaires=false
// preview caster, reusing two idioms already proven elsewhere in this
// suite: the RayCaster downcast + direct SetXrayViewResolve call
// (ViewportRenderModeTest.cpp's R9), and the raw
// InMemoryRasterizerOutput + IRasterizer::RasterizeScene render path
// (AgentObjectMapTest.cpp's RenderPipeline helper).
//----------------------------------------------------------------------

// An emissive "lamp" sphere at screen centre, occluded by a smaller
// transmissive glass sphere positioned IN FRONT of it (closer to the
// camera, same optical axis) -- big enough on screen that the exact
// centre pixel (32,32 on this 64x64 frame) always lands on the glass
// sphere's own surface before any x-ray resolve.
static const char* const kSceneLampBehindGlass =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"uniformcolor_painter\n{\n\tname emit\n\tcolor 1 1 1\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname lampmat\n\texitance emit\n\tmaterial mat\n\tscale 1.0\n}\n\n"
	"sphere_geometry\n{\n\tname lamp_geo\n\tradius 0.7\n}\n\n"
	"standard_object\n{\n\tname lamp\n\tgeometry lamp_geo\n\tmaterial lampmat\n\tposition 0 0 0\n}\n\n"
	"sphere_geometry\n{\n\tname glass_geo\n\tradius 0.5\n}\n\n"
	"dielectric_material\n{\n\tname glass_mat\n\ttau 1.0 1.0 1.0\n\tior 1.5\n}\n\n"
	"standard_object\n{\n\tname glass_obj\n\tgeometry glass_geo\n\tmaterial glass_mat\n\tposition 0 0 1.7\n}\n";

// Render one pass through the studio-preview (showLuminaires=false)
// pipeline, with x-ray resolve stamped directly onto the returned
// RayCaster via the downcast idiom (no SceneEditController needed for a
// single one-shot render).  Returns false on any setup/decode failure.
static bool RenderPreviewPipeline( const IScenePriv& scene, bool xray, Decoded& out )
{
	IRasterizer* rast = nullptr;
	IRayCaster*  preview = nullptr;
	IRayCaster*  polish = nullptr;
	if( !Implementation::CreateInteractiveMaterialPreviewPipeline( &rast, &preview, &polish ) ||
	    !rast || !preview ) {
		safe_release( rast );
		safe_release( preview );
		safe_release( polish );
		return false;
	}

	Implementation::RayCaster* rc = dynamic_cast<Implementation::RayCaster*>( preview );
	if( !rc ) {
		rast->release();
		preview->release();
		safe_release( polish );
		return false;
	}
	rc->SetXrayViewResolve( xray );

	Agent::InMemoryRasterizerOutput* sink = new Agent::InMemoryRasterizerOutput();
	rast->AddRasterizerOutput( sink );
	rast->RasterizeScene( scene, 0, nullptr );
	const std::vector<unsigned char> png = sink->ToPng();
	const bool decoded = DecodePng( png, out );
	safe_release( sink );
	preview->release();
	safe_release( polish );
	rast->release();
	return decoded;
}

static void RunPreviewResolvedLuminaireSuppressionTest()
{
	std::printf( "=== AgentViewModeRenderTest: (6) resolved-luminaire suppression under x-ray ===\n" );
	const std::string scenePath = WriteTemp( "rise_viewmode_lampglass.RISEscene", kSceneLampBehindGlass );
	Check( !scenePath.empty(), "wrote the lamp-behind-glass scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "lamp-behind-glass scene loads via the CST path" );
	const IScenePriv* scenePriv = pJob->GetScene();
	Check( scenePriv != nullptr, "lamp-behind-glass job has a Scene" );
	if( !scenePriv ) { pJob->release(); return; }

	// xray:false -- the preview caster shades the glass sphere's OWN
	// surface (its own studio-preview AO/albedo look): the centre pixel
	// is an opaque real hit, not background.  This is the control: it
	// proves the render pipeline itself produces a real (non-background)
	// hit here absent any resolve.
	Decoded decNoXray;
	Check( RenderPreviewPipeline( *scenePriv, /*xray*/false, decNoXray ), "preview render (xray off) decodes" );
	bool noXrayCenterOpaque = false;
	if( decNoXray.w == 64 && decNoXray.h == 64 ) {
		noXrayCenterOpaque = decNoXray.at( 32, 32 )[3] != 0;
	}
	Check( noXrayCenterOpaque, "xray off: the glass sphere's own surface renders opaque at screen centre" );

	// xray:true -- resolves THROUGH the glass to the emissive lamp behind
	// it.  Fix under test: bShowLuminaires (false on this caster) must be
	// re-checked against the RESOLVED hit, so the centre pixel is
	// background -- exactly as a directly-visible lamp would be suppressed
	// on this same showLuminaires=false caster (see AgentObjectMapTest.cpp
	// RunEmissiveVisibilityTest's analogous draft-render assertion).
	// BEFORE the fix, the resolved lamp hit escaped the check entirely
	// (it only ever ran against the original glass hit) and rendered as
	// an opaque shaded lamp surface instead.
	Decoded decXray;
	Check( RenderPreviewPipeline( *scenePriv, /*xray*/true, decXray ), "preview render (xray on) decodes" );
	if( decXray.w == 64 && decXray.h == 64 ) {
		const Px& center = decXray.at( 32, 32 );
		Check( center[3] == 0,
		       "MONEY ASSERTION: x-ray resolved through the glass to the emissive lamp is STILL SUPPRESSED "
		       "(background/transparent centre pixel) on the showLuminaires=false preview caster -- the "
		       "suppression check re-runs on the RESOLVED hit, not just the original glass hit" );
	}

	pJob->release();
}

//----------------------------------------------------------------------
// (7) Review P2 coverage: InteractivePelRasterizer's depth auto-window
// self-correction must fire not only on the FIRST pass EVER (the
// original "depth renders nothing" fix, DepthViewShader's old
// WindowPending -- "never armed yet") but also when an ALREADY-VALID
// window (armed by an EARLIER render through the SAME persistent
// caster/DepthViewShader) has gone STALE because the view changed, e.g.
// a camera jump to a wildly different depth range (WindowStale's new
// case (2)).
//
// This case is architecturally UNREACHABLE through AgentSession's
// ViewMode path: EVERY AgentSession::Render(ViewMode) call builds a
// brand-new EPHEMERAL InteractivePelRasterizer + InteractiveViewModeRayCaster
// (see AgentSession.cpp's doViewModeRenderWork / CreateInteractiveViewModePipeline),
// so no window ever survives across two separate agent render calls --
// each call independently starts from mWinValid=false and only ever
// exercises the "never armed yet" trigger, regardless of any
// AgentCameraOverride passed in.  The GUI's ACTUAL interactive viewport,
// by contrast, uses ONE PERSISTENT InteractivePelRasterizer across many
// RasterizeScene calls (SceneEditController::SetViewportRenderMode
// installs a data-mode caster ONCE; repeated camera drags/jumps all
// render through that SAME instance) -- so this test reproduces THAT
// shape directly: ViewportRenderModeTest.cpp R5's persistent-pipeline
// idiom (CreateInteractiveMaterialPreviewPipeline + SetViewModeCaster)
// plus a direct RasterizeScene/InMemoryRasterizerOutput render (this
// file's RenderPreviewPipeline / AgentObjectMapTest.cpp's RenderPipeline),
// called TWICE on the SAME rasterizer against two scenes whose visible
// depth ranges do not overlap at all -- the "camera jump" stand-in.
//----------------------------------------------------------------------

// A sphere close to the camera: visible depth range ~[4, 5.7].
static const char* const kSceneDepthNear =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 2.0\n}\n\n"
	"standard_object\n{\n\tname obj\n\tgeometry geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

// SAME sphere, SAME dims/fov -- camera moved FAR away: visible depth
// range ~[58, 60], entirely non-overlapping with kSceneDepthNear's.
static const char* const kSceneDepthFar =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 60\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 2.0\n}\n\n"
	"standard_object\n{\n\tname obj\n\tgeometry geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

// Render one pass on an ALREADY-CONSTRUCTED, persistent rasterizer --
// unlike RenderPreviewPipeline above, this does not build/tear down a
// pipeline per call, so caster-owned state (the depth window) survives
// across successive calls against the SAME `rast`.
static bool RenderOnPersistentCaster( IRasterizer& rast, const IScenePriv& scene, Decoded& out )
{
	Agent::InMemoryRasterizerOutput* sink = new Agent::InMemoryRasterizerOutput();
	rast.AddRasterizerOutput( sink );
	rast.RasterizeScene( scene, 0, nullptr );
	const std::vector<unsigned char> png = sink->ToPng();
	const bool decoded = DecodePng( png, out );
	// Detach before the next call reuses `rast` -- avoids accumulating a
	// growing outs list across repeated calls on the same instance.
	rast.FreeRasterizerOutputs();
	safe_release( sink );
	return decoded;
}

// Same "strongContrast" shape as RunPerModeEndToEndTest's depth
// assertion (>= distinct gray levels, or a wide min/max spread) --
// evaluated over hit pixels only.
static bool DepthStrongContrast( const Decoded& d )
{
	if( d.px.empty() ) return false;
	std::set<unsigned char> distinctGray;
	unsigned char grayMin = 255, grayMax = 0;
	bool anyHit = false;
	for( std::size_t px = 0; px < d.px.size(); ++px ) {
		const Px& q = d.px[px];
		if( q[3] == 0 ) continue;   // background miss
		anyHit = true;
		distinctGray.insert( q[0] );
		if( q[0] < grayMin ) grayMin = q[0];
		if( q[0] > grayMax ) grayMax = q[0];
	}
	if( !anyHit ) return false;
	return distinctGray.size() >= 10 ||
	       (unsigned int)( grayMax - grayMin ) > (unsigned int)( 0.3 * 255 );
}

static void RunDepthWindowStaleSelfCorrectTest()
{
	std::printf( "=== AgentViewModeRenderTest: (7) depth stale-window self-correct across a camera jump ===\n" );

	const std::string nearPath = WriteTemp( "rise_viewmode_depthnear.RISEscene", kSceneDepthNear );
	const std::string farPath  = WriteTemp( "rise_viewmode_depthfar.RISEscene",  kSceneDepthFar );
	Check( !nearPath.empty() && !farPath.empty(), "wrote the near/far depth-jump scenes" );

	Job* pJobNear = new Job();
	Check( pJobNear->LoadAsciiSceneViaCst( nearPath.c_str() ), "near-camera scene loads" );
	Job* pJobFar = new Job();
	Check( pJobFar->LoadAsciiSceneViaCst( farPath.c_str() ), "far-camera scene loads" );
	const IScenePriv* sceneNear = pJobNear->GetScene();
	const IScenePriv* sceneFar  = pJobFar->GetScene();
	Check( sceneNear != nullptr && sceneFar != nullptr, "both jobs expose a Scene" );
	if( !sceneNear || !sceneFar ) { pJobNear->release(); pJobFar->release(); return; }

	IRasterizer* rast = nullptr;
	IRayCaster*  previewCaster = nullptr;
	IRayCaster*  polishCaster  = nullptr;
	Check( Implementation::CreateInteractiveMaterialPreviewPipeline( &rast, &previewCaster, &polishCaster ),
	       "persistent preview pipeline builds" );
	if( !rast ) { pJobNear->release(); pJobFar->release(); return; }

	Implementation::InteractivePelRasterizer* impl = dynamic_cast<Implementation::InteractivePelRasterizer*>( rast );
	Check( impl != nullptr, "rasterizer downcasts to InteractivePelRasterizer" );

	IRayCaster* depthCaster = nullptr;
	Check( Implementation::CreateInteractiveViewModeCaster(
	           Implementation::ViewportRenderMode::Depth, &depthCaster ), "depth caster builds" );

	if( impl && depthCaster )
	{
		impl->SetViewModeCaster( depthCaster );

		// Pass 1: near camera.  Arms a valid window from this call's own
		// internal self-correction (case (1), "never armed yet" -- not
		// what's under test here, but exercising it is a harmless and
		// realistic warm-up matching how the GUI viewport actually starts).
		Decoded decNear;
		Check( RenderOnPersistentCaster( *rast, *sceneNear, decNear ), "near-camera depth render decodes" );

		// Pass 2: SAME rasterizer/caster, camera jumps to a FAR, entirely
		// non-overlapping visible distance range.  Fix under test: the
		// base pass starts by shading against the now-STALE near window
		// (armed by pass 1) -- every far pixel clamps to the SAME extreme
		// (near-window's far edge), which is what "depth renders nothing"
		// for the new view looks like.  WindowStale() must detect that
		// deviation and trigger the self-correcting extra pass, so THIS
		// SAME call still returns a properly windowed (high-contrast)
		// image for the far view.
		Decoded decFar;
		Check( RenderOnPersistentCaster( *rast, *sceneFar, decFar ), "far-camera depth render decodes" );

		Check( DepthStrongContrast( decNear ), "near-camera pass shows windowed (high-contrast) depth" );
		Check( DepthStrongContrast( decFar ),
		       "MONEY ASSERTION: far-camera pass, on the SAME persistent caster right after the near-camera "
		       "pass, STILL shows windowed (high-contrast) depth -- WindowStale() detected the camera jump "
		       "and re-armed the window for the new view within this single RasterizeScene call, instead of "
		       "shading the far view against the stale near-camera window (which would clamp every far hit "
		       "pixel to the SAME extreme value -- zero contrast)" );

		impl->SetViewModeCaster( nullptr );   // restore, mirrors R5's teardown order
	}

	safe_release( depthCaster );
	safe_release( rast );
	safe_release( previewCaster );
	safe_release( polishCaster );
	pJobNear->release();
	pJobFar->release();
}

//----------------------------------------------------------------------
// (8) External review P2 coverage: InteractivePelRasterizer's depth
// auto-window must treat a DEGENERATE (min==max) visible depth range as
// a first-class "Flat" window, not refuse to arm at all.  Two bugs
// this closes (see InteractivePelRasterizer.cpp's DepthViewShader class
// doc, "External review P2 fix"):
//
//   (a) a view that NEVER arms (every pass is degenerate) used to stay
//       Unarmed forever, and WindowStale()'s "still on the fallback"
//       case is unconditionally true for Unarmed -- so a permanently-
//       flat scene re-ran the self-correcting extra pass on EVERY call,
//       forever, instead of settling.
//   (b) a jump from a genuinely-ranged view to a flat one used to leave
//       the OLD ranged window active (PreparePass's old refusal doesn't
//       touch it) -- WindowStale()'s span-ratio check was explicitly
//       SKIPPED for a degenerate new span, so the stale ranged window
//       could survive indefinitely, clamping the whole (now-flat) frame
//       through a window that no longer describes the scene.
//
// A TRULY degenerate (bit-identical, not just near-flat) depth field
// needs a camera whose primary rays are PARALLEL (a perspective camera
// facing a flat wall still has a small but genuine per-pixel depth
// gradient off-axis) -- so these scenes use an `orthographic_camera`
// facing a large, FINITE (deliberately NOT `infiniteplane_geometry`,
// whose bounding box is a genuine infinity -- see CLAUDE.md's
// "-ffast-math: no infinity" note; this codebase's fast-math build
// miscompiles arithmetic seeded from a true infinity) flat box face.
// Every ortho ray's origin offset from the central ray lies entirely
// WITHIN the box face's plane (perpendicular to the shared ray
// direction), so every pixel's hit distance is the identical floating-
// point value -- a genuinely zero-variance depth field, not an
// approximation of one.
//
// The Flat-window constant (DepthViewShader::DepthValue, 0.6) is
// cross-checked via the SAME sRGB transfer function PNGWriter's
// Integerize path applies (RISE::ColorUtils::SRGBTransferFunction),
// rather than a hand-computed/guessed byte target -- this stays correct
// if the transfer curve is ever retuned, and (more importantly) is
// numerically FAR from what either buggy alternative would produce for
// this scene (the Unarmed scene-diagonal fallback, or a stale Ranged-
// window clamp to its near/far edge), so an exact-byte (+/-1 rounding)
// match is a tight, non-coincidental discriminator between "the Flat
// branch actually ran" and either bug.
//----------------------------------------------------------------------

// A large, flat, FINITE box face under an orthographic camera -- every
// visible pixel is the SAME distance from the camera (see the block
// comment above).  `film` intentionally matches kSceneDepthNear/Far's
// 64x64 so RenderOnPersistentCaster's decoded images are directly
// comparable in size.
static const char* const kSceneDepthFlat =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"orthographic_camera\n{\n\tname cam\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 3 3\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"box_geometry\n{\n\tname geo\n\twidth 100\n\theight 100\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj\n\tgeometry geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

// The exact 8-bit sRGB byte DepthViewShader's Flat-window constant (0.6
// linear) encodes to, via the SAME transfer function PNGWriter's
// Integerize path uses -- see the block comment above.
static unsigned char ExpectedFlatDepthByte()
{
	double encoded = RISE::ColorUtils::SRGBTransferFunction( 0.6 );
	double byte = encoded * 255.0 + 0.5;
	if( byte < 0.0 )   byte = 0.0;
	if( byte > 255.0 ) byte = 255.0;
	return static_cast<unsigned char>( byte );
}

// True iff every HIT pixel's red channel is within `tol` of `expected`
// (8-bit rounding slack -- a couple of ULPs across the sRGB encode).
// Requires at least one hit pixel.
static bool AllHitPixelsNear( const Decoded& d, unsigned char expected, int tol = 1 )
{
	if( d.px.empty() ) return false;
	bool anyHit = false;
	for( std::size_t px = 0; px < d.px.size(); ++px ) {
		const Px& q = d.px[px];
		if( q[3] == 0 ) continue;   // background miss
		anyHit = true;
		const int diff = static_cast<int>( q[0] ) - static_cast<int>( expected );
		if( diff < -tol || diff > tol ) return false;
	}
	return anyHit;
}

static void RunDepthFlatWindowSettlesTest()
{
	std::printf( "=== AgentViewModeRenderTest: (8a) flat scene settles to the constant mid-gray, no perpetual re-run ===\n" );

	const std::string flatPath = WriteTemp( "rise_viewmode_depthflat.RISEscene", kSceneDepthFlat );
	Check( !flatPath.empty(), "wrote the flat-wall scene" );

	Job* pJobFlat = new Job();
	Check( pJobFlat->LoadAsciiSceneViaCst( flatPath.c_str() ), "flat-wall scene loads" );
	const IScenePriv* sceneFlat = pJobFlat->GetScene();
	Check( sceneFlat != nullptr, "job exposes a Scene" );
	if( !sceneFlat ) { pJobFlat->release(); return; }

	IRasterizer* rast = nullptr;
	IRayCaster*  previewCaster = nullptr;
	IRayCaster*  polishCaster  = nullptr;
	Check( Implementation::CreateInteractiveMaterialPreviewPipeline( &rast, &previewCaster, &polishCaster ),
	       "persistent preview pipeline builds" );
	if( !rast ) { pJobFlat->release(); return; }

	Implementation::InteractivePelRasterizer* impl = dynamic_cast<Implementation::InteractivePelRasterizer*>( rast );
	Check( impl != nullptr, "rasterizer downcasts to InteractivePelRasterizer" );

	IRayCaster* depthCaster = nullptr;
	Check( Implementation::CreateInteractiveViewModeCaster(
	           Implementation::ViewportRenderMode::Depth, &depthCaster ), "depth caster builds" );

	if( impl && depthCaster )
	{
		impl->SetViewModeCaster( depthCaster );

		const unsigned char expected = ExpectedFlatDepthByte();

		// Call 1: NEVER armed yet (bug (a)'s first-ever-pass case).  The
		// override's own internal self-correction (WindowStale() case
		// "Unarmed -> *: always stale") must fire WITHIN this single call
		// -- the fix makes the second internal pass arm a Flat window and
		// shade the constant, so even THIS FIRST call already returns the
		// settled result.
		Decoded decCall1;
		Check( RenderOnPersistentCaster( *rast, *sceneFlat, decCall1 ), "flat-wall pass 1 decodes" );
		Check( AllHitPixelsNear( decCall1, expected ),
		       "MONEY ASSERTION: pass 1 already shows the constant Flat mid-gray (0.6, self-corrected within "
		       "this call) -- NOT the Unarmed scene-diagonal fallback a still-refusing PreparePass would leave it at" );

		// Call 2: SAME persistent caster, SAME (unchanged) scene.  Bug (a)
		// was "re-runs the extra pass on EVERY call forever" -- the
		// observable symptom of THAT bug is indistinguishable from the fix
		// by output alone (both reach the same fallback value every time,
		// since a perpetually-refusing PreparePass is deterministic too),
		// so the real assertion is the SAME exact-byte check again: only
		// an ARMED Flat window produces the constant 0.6, and Flat -> Flat
		// is classified NOT stale, so this call also settles in exactly
		// one internal pass.
		Decoded decCall2;
		Check( RenderOnPersistentCaster( *rast, *sceneFlat, decCall2 ), "flat-wall pass 2 decodes" );
		Check( AllHitPixelsNear( decCall2, expected ),
		       "pass 2 (Flat -> Flat, not stale) still shows the constant Flat mid-gray" );

		// Stability: repeated renders of an unchanged flat scene produce
		// pixel-identical output -- no oscillation between the fallback
		// and the Flat constant across calls.
		Check( decCall1.w == decCall2.w && decCall1.h == decCall2.h && decCall1.px == decCall2.px,
		       "pass 1 and pass 2 are pixel-identical" );

		impl->SetViewModeCaster( nullptr );   // restore, mirrors R5's teardown order
	}

	safe_release( depthCaster );
	safe_release( rast );
	safe_release( previewCaster );
	safe_release( polishCaster );
	pJobFlat->release();
}

static void RunDepthRangedToFlatSelfCorrectTest()
{
	std::printf( "=== AgentViewModeRenderTest: (8b) ranged -> flat self-corrects within one call ===\n" );

	const std::string nearPath = WriteTemp( "rise_viewmode_depthnear2.RISEscene", kSceneDepthNear );
	const std::string flatPath = WriteTemp( "rise_viewmode_depthflat2.RISEscene", kSceneDepthFlat );
	Check( !nearPath.empty() && !flatPath.empty(), "wrote the ranged/flat scene pair" );

	Job* pJobNear = new Job();
	Check( pJobNear->LoadAsciiSceneViaCst( nearPath.c_str() ), "near-camera (ranged) scene loads" );
	Job* pJobFlat = new Job();
	Check( pJobFlat->LoadAsciiSceneViaCst( flatPath.c_str() ), "flat-wall scene loads" );
	const IScenePriv* sceneNear = pJobNear->GetScene();
	const IScenePriv* sceneFlat = pJobFlat->GetScene();
	Check( sceneNear != nullptr && sceneFlat != nullptr, "both jobs expose a Scene" );
	if( !sceneNear || !sceneFlat ) { pJobNear->release(); pJobFlat->release(); return; }

	IRasterizer* rast = nullptr;
	IRayCaster*  previewCaster = nullptr;
	IRayCaster*  polishCaster  = nullptr;
	Check( Implementation::CreateInteractiveMaterialPreviewPipeline( &rast, &previewCaster, &polishCaster ),
	       "persistent preview pipeline builds" );
	if( !rast ) { pJobNear->release(); pJobFlat->release(); return; }

	Implementation::InteractivePelRasterizer* impl = dynamic_cast<Implementation::InteractivePelRasterizer*>( rast );
	Check( impl != nullptr, "rasterizer downcasts to InteractivePelRasterizer" );

	IRayCaster* depthCaster = nullptr;
	Check( Implementation::CreateInteractiveViewModeCaster(
	           Implementation::ViewportRenderMode::Depth, &depthCaster ), "depth caster builds" );

	if( impl && depthCaster )
	{
		impl->SetViewModeCaster( depthCaster );

		// Pass 1: near camera (ranged) -- arms a Ranged window (via its
		// own "never armed yet" self-correction), same warm-up idiom as
		// RunDepthWindowStaleSelfCorrectTest above.
		Decoded decNear;
		Check( RenderOnPersistentCaster( *rast, *sceneNear, decNear ), "near-camera (ranged) pass decodes" );
		Check( DepthStrongContrast( decNear ), "near-camera pass shows windowed (high-contrast) depth" );

		// Pass 2: SAME rasterizer/caster, scene jumps to the flat wall.
		// External review P2 bug (b): a stale Ranged window would clamp
		// this pass to whichever extreme the flat depth happens to land
		// nearest, and WindowStale()'s old span-ratio check explicitly
		// SKIPPED reacting to a degenerate new span -- so the stale
		// window could survive indefinitely.  The fix's Ranged->Flat
		// transition is UNCONDITIONALLY stale, so THIS SAME call must
		// self-correct to the Flat constant before returning.
		Decoded decFlat;
		Check( RenderOnPersistentCaster( *rast, *sceneFlat, decFlat ), "ranged-to-flat pass decodes" );
		Check( AllHitPixelsNear( decFlat, ExpectedFlatDepthByte() ),
		       "MONEY ASSERTION: the flat-wall pass, on the SAME persistent caster right after the ranged "
		       "near-camera pass, ALREADY shows the constant Flat mid-gray within this single call -- not a "
		       "stale ranged-window clamp to the near or far edge" );

		impl->SetViewModeCaster( nullptr );   // restore, mirrors R5's teardown order
	}

	safe_release( depthCaster );
	safe_release( rast );
	safe_release( previewCaster );
	safe_release( polishCaster );
	pJobNear->release();
	pJobFlat->release();
}

//----------------------------------------------------------------------
// (9) External review round 4, item 1 (P1) regression coverage:
// RayCaster::ResolveXrayView_'s adaptive skip epsilon must NOT be
// inflated by a large TRANSVERSE world coordinate (one the ray barely
// travels along).  This scene sits at world X = 1e12 (verified to parse
// -- RISE's ascii tokenizer already accepts scientific-notation literals
// elsewhere in the corpus, e.g. scenes/Tests/Materials/
// dielectric_dispersion.RISEscene) while the camera looks straight down
// -Z: for the exact optical-axis ray, dir.x == dir.y == 0, so ANY
// contribution the huge X coordinate makes to a max-abs-COMPONENT
// epsilon is pure transverse coupling, not genuine representability
// need along the ray -- exactly the review's own numeric example (hit
// point (1e12,0,0), dir (0,1,0)), just with the huge axis and the travel
// axis swapped.  `backstop1` is an opaque layer sitting 1e-3 behind
// the glass BOX's exit face; `bg` is a much farther opaque plane
// filling the rest of the frame.  The glass is deliberately a BOX, not
// a sphere: a sphere's quadratic does (o-c) differencing at 1e12 scale
// (catastrophic cancellation, t-noise ~ulp(1e12) ~ 1e-4), so a sub-1e-3
// gap is unresolvable by ANY epsilon scheme there -- while a box's
// per-axis slab test never mixes the huge X into the Z intersection, so
// the geometry itself stays exact.  The gap (1e-3) sits well above that
// noise floor and well below the OLD epsilon (~1.4e-2 at this X
// magnitude), which skipped clean over backstop1's entire 0.011 extent
// onto `bg`; the NEW direction-weighted epsilon (~1e-12, since the
// ray's own Z-axis travel is small) correctly stops at backstop1.
//
// Discriminator: NOT a raw depth-byte comparison (the per-pass
// auto-window in InteractivePelRasterizer.cpp's DepthViewShader
// renormalizes every render's own [min,max] independently, so an
// absolute-value comparison across renders is not reliable -- the
// on-axis centre pixel is always that render's OWN brightest value
// regardless of which surface it actually resolved to).  Instead this
// exploits the SILHOUETTE BOUNDARY within a SINGLE render: a reference
// objectmap render (same idiom as RunXrayCoverageTest above) locates
// glass_obj's own screen-space disk, and the test reads a pixel pair
// straddling that boundary (2px inside vs 2px outside).  If the walk
// stops at backstop1 (fix), the inside pixel is backstop1's own near
// depth (~4.9 world units) and the outside pixel is bg's much farther
// depth (~10.5+) -- a huge fraction of the frame's own [min,max] window,
// however it happens to be normalized THIS render.  If the walk skips to
// bg (bug), both pixels resolve to the SAME bg plane, only ~1.6 degrees
// apart in viewing angle -- a negligible smooth gradient, regardless of
// how tightly the (now bg-only) window is normalized.
//
// Verified discriminating (manual step during development, not re-run
// by this test): with the old max-abs-component epsilon temporarily
// reintroduced in RayCaster.cpp, this test's boundary-jump assertion
// fails (delta collapses to single-digit bytes) -- see the review round
// 4 handoff notes for the exact before/after byte deltas observed.
//----------------------------------------------------------------------

static const char* const kSceneLargeXGlassBackstop =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 1e12 0 6\n\tlookat 1e12 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname bg_pnt\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname bg_mat\n\treflectance bg_pnt\n}\n\n"
	"box_geometry\n{\n\tname bg_geo\n\twidth 2000\n\theight 2000\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname bg_obj\n\tgeometry bg_geo\n\tmaterial bg_mat\n\tposition 1e12 0 -5\n}\n\n"
	"box_geometry\n{\n\tname glass_geo\n\twidth 1.2\n\theight 1.2\n\tdepth 1.2\n}\n\n"
	"dielectric_material\n{\n\tname glass_mat\n\ttau 1.0 1.0 1.0\n\tior 1.5\n}\n\n"
	"standard_object\n{\n\tname glass_obj\n\tgeometry glass_geo\n\tmaterial glass_mat\n\tposition 1e12 0 1.7\n}\n\n"
	"uniformcolor_painter\n{\n\tname bs_pnt\n\tcolor 0.9 0.9 0.9\n}\n\n"
	"lambertian_material\n{\n\tname bs_mat\n\treflectance bs_pnt\n}\n\n"
	"box_geometry\n{\n\tname bs_geo\n\twidth 1.0\n\theight 1.0\n\tdepth 0.01\n}\n\n"
	"standard_object\n{\n\tname bs_obj\n\tgeometry bs_geo\n\tmaterial bs_mat\n\tposition 1e12 0 1.094\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 1e12 3 4\n}\n";

static void RunLargeTransverseCoordinateXrayTest()
{
	std::printf( "=== AgentViewModeRenderTest: (9) large transverse coordinate does not inflate the x-ray nudge (review r4, item 1) ===\n" );

	const std::string scenePath = WriteTemp( "rise_viewmode_largex.RISEscene", kSceneLargeXGlassBackstop );
	Check( !scenePath.empty(), "wrote the large-X glass+backstop+bg scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "large-X scene loads via the CST path (verifies 1e12 literals parse)" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "large-X session wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	// Reference objectmap render locates glass_obj's OWN screen-space
	// silhouette (objectmap always reports the first-hit object's ID
	// colour -- unaffected by x-ray resolve, same idiom as
	// RunXrayCoverageTest above).
	AgentRenderParams objP;
	objP.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult objR = session->Render( objP );
	Check( objR.ok && objR.renderMode == "objectmap", "large-X reference objectmap render succeeds" );
	Decoded objDec;
	Check( DecodePng( objR.png, objDec ), "large-X reference objectmap PNG decodes" );
	const LegendEntry* glassLegend = FindLegend( objR, "glass_obj" );
	Check( glassLegend != nullptr, "reference objectmap legend carries glass_obj" );
	BBox glassBBox{ 0, 0, 0, 0, 0 };
	if( glassLegend ) {
		unsigned char cb[3];
		Check( HexToBytes( glassLegend->colorHex, cb ), "glass_obj colorHex parses" );
		glassBBox = ScanBBoxForColor( objDec, cb );
		Check( glassBBox.found > 0, "glass_obj occupies a nonzero pixel region" );
	}

	AgentRenderParams depthP;
	depthP.renderTarget = AgentRenderTarget::ViewMode;
	depthP.viewMode     = Implementation::ViewportRenderMode::Depth;
	// xray stays at its own default (true) -- deliberately not set, same
	// as RunXrayCoverageTest's (d) case, proving the DEFAULT (not just an
	// explicit override) is what resolves through the glass.
	AgentRenderResult rDepth = session->Render( depthP );
	Check( rDepth.ok, "large-X depth render succeeds" );
	Decoded decDepth;
	Check( DecodePng( rDepth.png, decDepth ), "large-X depth PNG decodes" );

	if( glassBBox.found > 0 && decDepth.w == 64 && decDepth.h == 64 &&
	    glassBBox.maxX + 2 < decDepth.w && glassBBox.maxX >= 2 ) {
		const unsigned int row = ( glassBBox.minY + glassBBox.maxY ) / 2;
		// INSIDE sample: the silhouette CENTER, not the edge.  The
		// backstop (1.0 wide) is deliberately SMALLER than the glass box
		// (1.2 wide): only central through-glass rays are guaranteed
		// glass-then-backstop, while edge-adjacent ones would pass the
		// backstop onto bg.  OUTSIDE sample: 2px past the silhouette,
		// where the undersized backstop cannot be seen directly, so the
		// ray lands on bg (the original 2x2 backstop ringed the glass and
		// put the outside sample at backstop depth too -- a self-
		// defeating comparison).
		const Px& insideEdge  = decDepth.at( ( glassBBox.minX + glassBBox.maxX ) / 2, row );
		const Px& outsideEdge = decDepth.at( glassBBox.maxX + 2, row );
		Check( insideEdge[3] != 0 && outsideEdge[3] != 0,
		       "both boundary-straddle pixels are real hits (not background misses)" );
		const int delta = static_cast<int>( insideEdge[0] ) - static_cast<int>( outsideEdge[0] );
		Check( delta > 40,
		       "MONEY ASSERTION: the depth byte 2px INSIDE the glass silhouette boundary is far BRIGHTER "
		       "(nearer) than 2px OUTSIDE it -- the x-ray walk stopped at backstop1 (~4.9 world units away) "
		       "instead of skipping clean over its 1e-4 gap to bg (~10.5+ world units away, which is what the "
		       "transverse-coordinate-coupled epsilon used to do at this X magnitude, collapsing this same "
		       "delta to a handful of bytes from bg's own smooth background gradient alone)" );
	}

	pJob->release();
}


//----------------------------------------------------------------------
// (10) External review round 5 regression: an object-local SCALE along
// the ray amplifies Object::IntersectRay's published-point standoff
// (SURFACE_INTERSEC_ERROR in OBJECT-LOCAL units -> ~1e-11 world at 10x),
// which used to exceed both the 1e-12 nudge floor AND the `< curEps`
// degenerate-re-hit window -- the self-hit was accepted as a "real"
// skip, looping to the cap, and the x-ray walk never reached the opaque
// backstop.  Scene: glass box scaled 10x along the view axis (Z) with a
// large opaque backstop plane behind it.  If the walk works, the
// through-glass center pixel and a beside-the-glass pixel both resolve
// the SAME backstop plane (similar depth bytes); if the walk sticks at
// the glass, the center pixel reads the much NEARER glass surface
// (far brighter).
//----------------------------------------------------------------------
static const char* const kSceneScaledGlassBackstop =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname bg_pnt\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname bg_mat\n\treflectance bg_pnt\n}\n\n"
	"box_geometry\n{\n\tname bg_geo\n\twidth 2000\n\theight 2000\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname bg_obj\n\tgeometry bg_geo\n\tmaterial bg_mat\n\tposition 0 0 -2\n}\n\n"
	"box_geometry\n{\n\tname glass_geo\n\twidth 1.2\n\theight 1.2\n\tdepth 0.2\n}\n\n"
	"dielectric_material\n{\n\tname glass_mat\n\ttau 1.0 1.0 1.0\n\tior 1.5\n}\n\n"
	"standard_object\n{\n\tname glass_obj\n\tgeometry glass_geo\n\tmaterial glass_mat\n\tposition 0 0 1.7\n\tscale 1 1 10\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

static void RunScaledGlassStandoffTest()
{
	std::printf( "=== AgentViewModeRenderTest: (10) local-scale-amplified published-point standoff does not strand the x-ray walk (review r5) ===\n" );

	const std::string scenePath = WriteTemp( "rise_viewmode_scaledglass.RISEscene", kSceneScaledGlassBackstop );
	Check( !scenePath.empty(), "wrote the scaled-glass scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "scaled-glass scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "scaled-glass session wraps the Job" );
	if( !session ) { pJob->release(); return; }

	AgentRenderParams objP;
	objP.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult objR = session->Render( objP );
	Check( objR.ok, "scaled-glass reference objectmap render succeeds" );
	Decoded objDec;
	Check( DecodePng( objR.png, objDec ), "scaled-glass objectmap PNG decodes" );
	const LegendEntry* glassLegend = FindLegend( objR, "glass_obj" );
	Check( glassLegend != nullptr, "objectmap legend carries glass_obj" );
	BBox glassBBox{ 0, 0, 0, 0, 0 };
	if( glassLegend ) {
		unsigned char cb[3];
		Check( HexToBytes( glassLegend->colorHex, cb ), "glass_obj colorHex parses" );
		glassBBox = ScanBBoxForColor( objDec, cb );
		Check( glassBBox.found > 0, "glass_obj occupies a nonzero pixel region" );
	}

	AgentRenderParams depthP;
	depthP.renderTarget = AgentRenderTarget::ViewMode;
	depthP.viewMode     = Implementation::ViewportRenderMode::Depth;
	AgentRenderResult rDepth = session->Render( depthP );
	Check( rDepth.ok, "scaled-glass depth render succeeds" );
	Decoded decDepth;
	Check( DecodePng( rDepth.png, decDepth ), "scaled-glass depth PNG decodes" );

	if( glassBBox.found > 0 && decDepth.w == 64 && decDepth.h == 64 &&
	    glassBBox.maxX + 2 < decDepth.w && glassBBox.minX + glassBBox.maxX > 0 ) {
		const unsigned int row = ( glassBBox.minY + glassBBox.maxY ) / 2;
		const Px& center  = decDepth.at( ( glassBBox.minX + glassBBox.maxX ) / 2, row );
		const Px& outside = decDepth.at( glassBBox.maxX + 2, row );
		Check( center[3] != 0 && outside[3] != 0, "both scaled-glass samples are real hits" );
		const int delta = static_cast<int>( center[0] ) - static_cast<int>( outside[0] );
		Check( delta < 30 && delta > -30,
		       "MONEY ASSERTION: through the 10x-stretched glass, the center pixel resolves the SAME "
		       "backstop plane as the beside-the-glass pixel (similar depth bytes) -- the walk cleared the "
		       "local-backoff-amplified standoff instead of self-hitting to the skip cap and reporting the "
		       "much nearer glass surface" );
	}

	pJob->release();
}

int main()
{
	RunPerModeEndToEndTest();
	RunFilmRestoreTest();
	RunRpcModeParityTest();
	RunNoRenderOnInvalidModeTest();
	RunChatCodecModeParityTest();
	RunMcpAdapterModeParityTest();
	RunXrayCoverageTest();
	RunPreviewResolvedLuminaireSuppressionTest();
	RunDepthWindowStaleSelfCorrectTest();
	RunDepthFlatWindowSettlesTest();
	RunDepthRangedToFlatSelfCorrectTest();
	RunLargeTransverseCoordinateXrayTest();
	RunScaledGlassStandoffTest();

	std::printf( "\nAgentViewModeRenderTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
