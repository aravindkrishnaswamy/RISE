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
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"

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
// (5) X-ray axis coverage (docs/gui/RENDER_MODES.md "X-ray axis"): facets
// and depth with vs without xray over a transmissive glass sphere placed IN
// FRONT of the mesh, plus the honest-ignored-under-beauty note.
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

	// (a) facets WITHOUT xray -- shows the glass sphere's own surface.
	AgentRenderParams facetsNoXray;
	facetsNoXray.renderTarget = AgentRenderTarget::ViewMode;
	facetsNoXray.viewMode     = Implementation::ViewportRenderMode::Facets;
	AgentRenderResult rFacetsNo = session->Render( facetsNoXray );
	Check( rFacetsNo.ok, "facets without xray succeeds" );
	Decoded decFacetsNo;
	Check( DecodePng( rFacetsNo.png, decFacetsNo ), "facets-without-xray PNG decodes" );

	// (b) facets WITH xray:true -- should show the mesh through the glass.
	AgentRenderParams facetsXray = facetsNoXray;
	facetsXray.xray = true;
	AgentRenderResult rFacetsXray = session->Render( facetsXray );
	Check( rFacetsXray.ok, "facets with xray succeeds" );
	Check( rFacetsXray.message.find( "xray" ) != std::string::npos,
	       "facets-with-xray message notes xray is active" );
	Decoded decFacetsXray;
	Check( DecodePng( rFacetsXray.png, decFacetsXray ), "facets-with-xray PNG decodes" );

	if( glassBBox.found > 0 &&
	    decFacetsNo.w == decFacetsXray.w && decFacetsNo.h == decFacetsXray.h ) {
		unsigned int differing = 0;
		for( unsigned int y = glassBBox.minY; y <= glassBBox.maxY; ++y ) {
			for( unsigned int x = glassBBox.minX; x <= glassBBox.maxX; ++x ) {
				const Px& a = decFacetsNo.at( x, y );
				const Px& b = decFacetsXray.at( x, y );
				if( a[0] != b[0] || a[1] != b[1] || a[2] != b[2] ) ++differing;
			}
		}
		Check( differing > 0,
		       "MONEY ASSERTION (a/b): facets WITH xray shows DIFFERENT pixels than WITHOUT, over "
		       "the glass sphere's own screen region -- the mesh shows through the glass" );
	}

	// (c) depth WITH vs WITHOUT xray -- should differ (xray sees through to
	// the mesh, which is farther away than the glass sphere's own surface).
	AgentRenderParams depthNoXray;
	depthNoXray.renderTarget = AgentRenderTarget::ViewMode;
	depthNoXray.viewMode     = Implementation::ViewportRenderMode::Depth;
	AgentRenderResult rDepthNo = session->Render( depthNoXray );
	Check( rDepthNo.ok, "depth without xray succeeds" );
	Decoded decDepthNo;
	Check( DecodePng( rDepthNo.png, decDepthNo ), "depth-without-xray PNG decodes" );

	AgentRenderParams depthXray = depthNoXray;
	depthXray.xray = true;
	AgentRenderResult rDepthXray = session->Render( depthXray );
	Check( rDepthXray.ok, "depth with xray succeeds" );
	Decoded decDepthXray;
	Check( DecodePng( rDepthXray.png, decDepthXray ), "depth-with-xray PNG decodes" );

	if( decDepthNo.w == decDepthXray.w && decDepthNo.h == decDepthXray.h ) {
		unsigned int differing = 0;
		for( std::size_t px = 0; px < decDepthNo.px.size(); ++px ) {
			if( decDepthNo.px[px][0] != decDepthXray.px[px][0] ) ++differing;
		}
		Check( differing > 0,
		       "MONEY ASSERTION (c): depth WITH xray differs from depth WITHOUT xray somewhere in the frame" );
	}

	// (d) xray:true with mode:"beauty" (the default renderTarget) is
	// ACCEPTED, not rejected, and carries the honest ignored note.
	AgentRenderParams beautyXray;
	beautyXray.xray = true;   // renderTarget stays Beauty (the default)
	AgentRenderResult rBeautyXray = session->Render( beautyXray );
	Check( rBeautyXray.ok, "xray:true with mode:beauty is accepted (not rejected)" );
	Check( rBeautyXray.message.find( "ignored" ) != std::string::npos,
	       "MONEY ASSERTION (d): xray:true under mode:beauty carries an honest 'ignored' note in the message" );

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

	std::printf( "\nAgentViewModeRenderTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
