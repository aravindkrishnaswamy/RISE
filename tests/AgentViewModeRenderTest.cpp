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
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
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

// Whole-frame mean luminance (moved up from its original P1-b/P2-e-local
// definition, review-p3 P1 fix, so scenario (d) below -- and any other
// test earlier in the file -- can use it too).
static double MeanLuminance( const Decoded& d )
{
	double sum = 0; unsigned int n = 0;
	for( std::size_t px = 0; px < d.px.size(); ++px )
	{
		const Px& q = d.px[px];
		if( q[3] == 0 ) continue;
		sum += ( (double)q[0] + (double)q[1] + (double)q[2] ) / 3.0;
		++n;
	}
	return n > 0 ? sum / n : -1.0;
}

// The full accepted-mode-name set every one of the three agent surfaces
// (AgentRpc.cpp's parser, AgentChatCodecs.cpp's hand-synced schema,
// AgentMcpAdapter.cpp's generated schema) must agree on: "beauty" +
// "objectmap" (hardcoded in all three) UNION the registry's casterFactory
// entries (Normals/Depth/Facets/Wireframe) UNION its BeautyVariant entries
// (GUI render modes P2a -- deep_reflect/direct today, discovered via
// IsBeautyVariantMode so a future variant row doesn't silently rot this
// test into a false pass, exactly like the casterFactory discovery already
// does for a future 5th data mode).
static std::set<std::string> ExpectedAcceptedModes()
{
	std::set<std::string> expected;
	expected.insert( "beauty" );
	expected.insert( "objectmap" );
	unsigned int count = 0;
	const Implementation::ViewportRenderModeInfo* modes = Implementation::GetViewportRenderModes( count );
	for( unsigned int i = 0; i < count; ++i )
		if( modes[i].casterFactory || Implementation::IsBeautyVariantMode( modes[i].mode ) )
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
// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): BeautyVariant
// (deep_reflect/direct) end-to-end.  Uses kSceneMeshAndSphere (same fixture
// as the P1 per-mode test above) -- both variant modes are REAL
// production-class renders, so a mesh+sphere+omni-light scene shades
// non-trivially even without glass/metal materials to specifically probe
// reflections; the "plausibly shaded" assertion is the honest "basic
// sanity" fallback the task calls for absent a cheap glass-region
// comparison.
//----------------------------------------------------------------------
static void RunBeautyVariantEndToEndTest()
{
	std::printf( "=== AgentViewModeRenderTest: BeautyVariant (deep_reflect/direct) end-to-end ===\n" );
	const std::string scenePath = WriteTemp( "rise_beautyvariant_e2e.RISEscene", kSceneMeshAndSphere );
	Check( !scenePath.empty(), "wrote the mesh+sphere scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "beauty-variant scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "beauty-variant session wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	struct VariantExpect
	{
		Implementation::ViewportRenderMode mode;
		const char* name;
		unsigned int divisor;
		int          spp;
	};
	const VariantExpect variants[2] = {
		{ Implementation::ViewportRenderMode::DeepReflect, "deep_reflect", 4, 16 },
		{ Implementation::ViewportRenderMode::Direct,      "direct",       2, 8  },
	};

	for( unsigned int i = 0; i < 2; ++i )
	{
		const VariantExpect& v = variants[i];
		std::printf( "  -- mode \"%s\" --\n", v.name );

		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = v.mode;
		// A positive samples request + explicit draft quality -- both MUST
		// be ignored (fixed config) and trip the honest note.
		p.samples = 999;

		AgentRenderResult r = session->Render( p );
		Check( r.ok, std::string( v.name ) + ": render succeeds" );
		Check( r.renderMode == v.name, std::string( v.name ) + ": renderMode echoes the registry name" );
		// No width/height override supplied -- effective dims are the
		// scene's authored 96x72, divided by the mode's fixed divisor.
		const unsigned int expectW = 96u / v.divisor;
		const unsigned int expectH = 72u / v.divisor;
		Check( r.width == expectW && r.height == expectH,
		       std::string( v.name ) + ": dims are the authored 96x72 divided by the mode's fixed divisor" );
		Check( !r.samplesOverridden, std::string( v.name ) + ": samplesOverridden is false (the override is honestly refused)" );
		Check( r.effectiveSamples == v.spp,
		       std::string( v.name ) + ": effectiveSamples reports the mode's REAL fixed spp, not the ShaderPipeline exactness invariant's 1" );
		const std::string expectedNote = std::string( "mode:" ) + v.name + " uses a FIXED production-quality config; quality/samples ignored";
		Check( r.message.find( expectedNote ) != std::string::npos,
		       std::string( v.name ) + ": message carries the honest quality/samples-ignored note" );
		const std::string expectedXrayNote = std::string( "xray is ignored under mode:" ) + v.name;
		Check( r.message.find( expectedXrayNote ) != std::string::npos,
		       std::string( v.name ) + ": message carries the honest xray-ignored note" );

		Decoded dec;
		Check( DecodePng( r.png, dec ), std::string( v.name ) + ": PNG decodes" );
		if( dec.w != expectW || dec.h != expectH ) continue;

		// Basic sanity: a REAL shaded render of a lit mesh+sphere scene is
		// not flat -- distinct pixel values across the frame (MONEY
		// ASSERTION per the task: "nonzero variance, not the flat preview
		// look").
		std::set<std::uint32_t> distinctColors;
		for( std::size_t px = 0; px < dec.px.size(); ++px )
		{
			const Px& q = dec.px[px];
			const std::uint32_t key = ( (std::uint32_t)q[0] << 16 ) | ( (std::uint32_t)q[1] << 8 ) | q[2];
			distinctColors.insert( key );
		}
		Check( distinctColors.size() > 5,
		       std::string( v.name ) + ": MONEY ASSERTION -- more than 5 distinct colours across the image (plausibly shaded, not flat)" );
	}

	// Film-dims restore: the SAME contract the P1 view-mode path has.
	const IScenePriv* scenePriv = pJob->GetScene();
	const IFilm* film = scenePriv ? scenePriv->GetFilm() : nullptr;
	Check( film && film->GetWidth() == 96 && film->GetHeight() == 72,
	       "MONEY ASSERTION: Film dims are RESTORED to the authored 96x72 after both variant renders" );

	// Width/height override composes: the divisor applies to the EFFECTIVE
	// (caller-supplied) dims, not the scene-authored ones.
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = Implementation::ViewportRenderMode::DeepReflect;
		p.width  = 64;
		p.height = 48;
		AgentRenderResult r = session->Render( p );
		Check( r.ok, "deep_reflect with a width/height override succeeds" );
		Check( r.width == 16 && r.height == 12,
		       "MONEY ASSERTION: the divisor (4) applies to the OVERRIDE dims (64x48 -> 16x12), not the scene-authored ones" );
	}

	pJob->release();
}

//----------------------------------------------------------------------
// P1 review fix (variantMaxBounces was inert -- "direct" rendered full GI):
// a Cornell-corner colour-bleed scene, white floor abutting a red wall, lit
// by a single white omni light that hits BOTH surfaces directly.  A floor
// patch immediately next to the wall base only picks up a RED tint via an
// INDIRECT bounce off the wall (classic radiosity colour bleed) -- under
// direct lighting alone the floor patch is lit by the (white) light only,
// so it stays neutral (R roughly == G roughly == B, modulated only by the
// floor's own grey reflectance).  This is the empirical proof the review
// demanded: "direct" (variantMaxBounces=1) must show (a) a MEANINGFULLY
// lower whole-frame mean than "deep_reflect" (missing multi-bounce indirect
// energy) and (b) no material red contamination in the near-wall floor
// patch, while "deep_reflect" (variantMaxBounces=24) shows both.  Before
// the fix (PathTracingIntegrator's main loop hardcoded 128 bounces
// regardless of variantMaxBounces), "direct" ran the SAME full-GI transport
// as "deep_reflect" and both assertions below fail.
//----------------------------------------------------------------------
static const char* const kSceneColorBleedCornellCorner =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 96\n\theight 96\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0.6 1.7 5.2\n\tlookat -1.0 1.0 -0.6\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname white_pnt\n\tcolor 0.85 0.85 0.85\n}\n\n"
	"uniformcolor_painter\n{\n\tname red_pnt\n\tcolor 0.85 0.05 0.05\n}\n\n"
	"lambertian_material\n{\n\tname white_mat\n\treflectance white_pnt\n}\n\n"
	"lambertian_material\n{\n\tname red_mat\n\treflectance red_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial white_mat\n\tposition 0 -0.1 0\n}\n\n"
	"box_geometry\n{\n\tname ceil_geo\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n"
	"standard_object\n{\n\tname ceil_obj\n\tgeometry ceil_geo\n\tmaterial white_mat\n\tposition 0 3.1 0\n}\n\n"
	"box_geometry\n{\n\tname back_geo\n\twidth 8\n\theight 3.2\n\tdepth 0.2\n}\n\n"
	"standard_object\n{\n\tname back_obj\n\tgeometry back_geo\n\tmaterial white_mat\n\tposition 0 1.5 -2.1\n}\n\n"
	"box_geometry\n{\n\tname wall_geo\n\twidth 0.2\n\theight 3.2\n\tdepth 8\n}\n\n"
	"standard_object\n{\n\tname wall_obj\n\tgeometry wall_geo\n\tmaterial red_mat\n\tposition -2.0 1.5 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 7.0\n\tcolor 1 1 1\n\tposition -0.3 2.7 -0.5\n}\n";

static void RunDirectModeMissingIndirectBleedTest()
{
	std::printf( "=== AgentViewModeRenderTest: P1 fix -- \"direct\" excludes indirect colour bleed \"deep_reflect\" shows ===\n" );

	const std::string scenePath = WriteTemp( "rise_colorbleed_corner.RISEscene", kSceneColorBleedCornellCorner );
	Check( !scenePath.empty(), "wrote the Cornell-corner colour-bleed scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "colour-bleed scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "colour-bleed session wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	// Reference objectmap render (full authored res, divisor 1) locates
	// wall_obj's and floor_obj's own screen-space silhouettes.
	AgentRenderParams objP;
	objP.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult objR = session->Render( objP );
	Check( objR.ok && objR.renderMode == "objectmap", "reference objectmap render succeeds" );
	Decoded objDec;
	Check( DecodePng( objR.png, objDec ), "reference objectmap PNG decodes" );
	const LegendEntry* wallLegend  = FindLegend( objR, "wall_obj" );
	const LegendEntry* floorLegend = FindLegend( objR, "floor_obj" );
	Check( wallLegend  != nullptr, "reference objectmap legend carries wall_obj" );
	Check( floorLegend != nullptr, "reference objectmap legend carries floor_obj" );
	BBox wallBBox{ 0, 0, 0, 0, 0 }, floorBBox{ 0, 0, 0, 0, 0 };
	if( wallLegend && floorLegend )
	{
		unsigned char wallCb[3], floorCb[3];
		Check( HexToBytes( wallLegend->colorHex, wallCb ), "wall_obj colorHex parses" );
		Check( HexToBytes( floorLegend->colorHex, floorCb ), "floor_obj colorHex parses" );
		wallBBox  = ScanBBoxForColor( objDec, wallCb );
		floorBBox = ScanBBoxForColor( objDec, floorCb );
		Check( wallBBox.found  > 0, "wall_obj occupies a nonzero pixel region" );
		Check( floorBBox.found > 0, "floor_obj occupies a nonzero pixel region" );
	}
	if( wallBBox.found == 0 || floorBBox.found == 0 || objDec.w == 0 ) { pJob->release(); return; }

	// The "near-wall floor patch" in FULL-RES (divisor-1) pixel space: a
	// strip just to the right of the wall's own silhouette (the wall
	// stands at world x=-2, floor extends toward +x on camera-right), 3px
	// wide, restricted to rows inside the floor's own bbox so the patch is
	// unambiguously floor pixels, never wall or background.
	// Start 4px off the wall silhouette (was 2): the variant pipeline
	// forces OIDN on, and at the /4 divisor the near-wall rows land 1-2
	// downsampled pixels from the directly-lit red wall -- close enough
	// for denoiser smear to bleed red into the DIRECT-mode patch and
	// erode the discriminator's headroom (round-2 review P3).  4px keeps
	// the patch clear of the smear radius while still inside the bleed
	// gradient deep_reflect must show.
	const unsigned int patchX0 = wallBBox.maxX + 4;
	const unsigned int patchX1 = wallBBox.maxX + 11;   // 7px wide (full-res) -- still maps to a few px after the /4 divisor
	const unsigned int patchY0 = std::max( floorBBox.minY, wallBBox.minY ) + 2;
	const unsigned int patchY1 = std::min( floorBBox.maxY, wallBBox.maxY ) - 2;
	const bool patchSane = patchX1 < objDec.w && patchY0 < patchY1 && patchY1 < objDec.h;
	Check( patchSane, "near-wall floor patch geometry is sane in full-res pixel space (camera framing produced a usable corner)" );
	if( !patchSane ) { pJob->release(); return; }

	// Average (R, G, B) and luminance over a pixel rectangle in an image of
	// width `w`/height `h` at resolution divisor `divisor` relative to the
	// full-res coordinates above -- both variant renders share the SAME
	// camera/FOV, so dividing the full-res patch rectangle by the mode's
	// own scale divisor lands on the spatially-corresponding patch in its
	// downsampled frame.  Skips background (alpha==0) pixels.  Returns
	// false if the mapped patch is empty/degenerate.
	struct PatchAvg { double r = 0, g = 0, b = 0; unsigned int n = 0; };
	auto averagePatch = [&]( const Decoded& d, unsigned int divisor ) -> PatchAvg
	{
		PatchAvg out;
		const unsigned int x0 = patchX0 / divisor, x1 = patchX1 / divisor;
		const unsigned int y0 = patchY0 / divisor, y1 = patchY1 / divisor;
		if( x1 >= d.w || y1 >= d.h || x0 > x1 || y0 > y1 ) return out;
		for( unsigned int y = y0; y <= y1; ++y ) {
			for( unsigned int x = x0; x <= x1; ++x ) {
				const Px& q = d.at( x, y );
				if( q[3] == 0 ) continue;   // background miss
				out.r += q[0]; out.g += q[1]; out.b += q[2];
				++out.n;
			}
		}
		if( out.n > 0 ) { out.r /= out.n; out.g /= out.n; out.b /= out.n; }
		return out;
	};

	struct VariantRun { Decoded dec; double meanLuminance = 0; PatchAvg patch; bool valid = false; };
	auto runVariant = [&]( Implementation::ViewportRenderMode mode, unsigned int divisor ) -> VariantRun
	{
		VariantRun out;
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) return out;
		if( !DecodePng( r.png, out.dec ) ) return out;
		double sum = 0; unsigned int n = 0;
		for( std::size_t px = 0; px < out.dec.px.size(); ++px )
		{
			const Px& q = out.dec.px[px];
			if( q[3] == 0 ) continue;
			sum += ( (double)q[0] + (double)q[1] + (double)q[2] ) / 3.0;
			++n;
		}
		if( n == 0 ) return out;
		out.meanLuminance = sum / n;
		out.patch = averagePatch( out.dec, divisor );
		out.valid = true;
		return out;
	};

	const VariantRun deepReflect = runVariant( Implementation::ViewportRenderMode::DeepReflect, 4 );
	const VariantRun direct      = runVariant( Implementation::ViewportRenderMode::Direct,       2 );
	Check( deepReflect.valid, "deep_reflect render decodes and has real (non-background) pixels" );
	Check( direct.valid,      "direct render decodes and has real (non-background) pixels" );
	if( !deepReflect.valid || !direct.valid ) { pJob->release(); return; }

	std::printf( "  deep_reflect: meanLuminance=%.2f patch(r=%.2f,g=%.2f,b=%.2f,n=%u)\n",
		deepReflect.meanLuminance, deepReflect.patch.r, deepReflect.patch.g, deepReflect.patch.b, deepReflect.patch.n );
	std::printf( "  direct:       meanLuminance=%.2f patch(r=%.2f,g=%.2f,b=%.2f,n=%u)\n",
		direct.meanLuminance, direct.patch.r, direct.patch.g, direct.patch.b, direct.patch.n );

	// (a) MONEY ASSERTION: direct-only transport is missing the wall's
	// multi-bounce indirect contribution across the WHOLE frame (the floor
	// and wall both lose their indirect self-illumination top-up), so its
	// mean luminance is meaningfully below deep_reflect's full-GI mean.
	Check( direct.meanLuminance < deepReflect.meanLuminance - 8.0,
	       "MONEY ASSERTION (a): \"direct\" mean luminance is MEANINGFULLY below \"deep_reflect\"'s -- "
	       "indirect energy is genuinely missing, not just noise" );

	// (b) MONEY ASSERTION: the near-wall floor patch. deep_reflect's red
	// wall bounces red light onto it (R clearly above G/B); direct's SAME
	// patch is lit by the white light alone, so R stays close to G/B.
	Check( deepReflect.patch.n > 0 && direct.patch.n > 0, "both variants' near-wall floor patches contain real (non-background) pixels" );
	if( deepReflect.patch.n > 0 && direct.patch.n > 0 )
	{
		const double deepReflectRedBias = deepReflect.patch.r - 0.5 * ( deepReflect.patch.g + deepReflect.patch.b );
		const double directRedBias      = direct.patch.r      - 0.5 * ( direct.patch.g      + direct.patch.b );
		std::printf( "  redBias: deep_reflect=%.2f direct=%.2f\n", deepReflectRedBias, directRedBias );
		Check( deepReflectRedBias > 12.0,
		       "MONEY ASSERTION (b1): deep_reflect's near-wall floor patch shows a MATERIAL red bias (R noticeably above G/B) -- "
		       "the indirect bounce off the red wall reaches the floor" );
		Check( directRedBias < 6.0,
		       "MONEY ASSERTION (b2): direct's near-wall floor patch shows NO material red bias -- "
		       "direct lighting alone is neutral (white light on grey/red surfaces), the red only ever arrives via a "
		       "bounce that \"direct\" must NOT trace" );
	}

	pJob->release();
}

//----------------------------------------------------------------------
// GUI render modes P2b `indirect` (docs/gui/RENDER_MODES.md §3 Lighting):
// discrimination tests for PathTracingIntegrator::SetIndirectOnly.
//
// Scenario (a): a single large diffuse plane fills the ENTIRE frame (no
// other geometry to bounce light onto, no environment map) -- under
// "indirect", the depth==0 direct (emission + NEE) contribution at every
// pixel is suppressed and there is no OTHER surface for a continuation ray
// to land on, so the whole image should read NEAR-BLACK.  Under a real
// transport render ("deep_reflect"), the SAME plane reads brightly lit.
//----------------------------------------------------------------------
//----------------------------------------------------------------------
// Scenario (c): directly-visible ENVIRONMENT background under "indirect".
// The camera-visible background (a primary ray that misses all geometry
// and sees the radiance map) is a DIRECT contribution -- same as a
// directly-visible emitter -- so "indirect" must render it BLACK, while
// "deep_reflect" shows it bright.  This is the P2b review P1 regression:
// the in-loop env gates were dead (never reached at depth 0); the real
// suppression lives at the top-level primary-miss return.
//----------------------------------------------------------------------
static const char* const kSceneEnvBackground =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"uniformcolor_painter\n{\n\tname env_pnt\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n\tradiance_map env_pnt\n\tradiance_scale 1.0\n\tradiance_background true\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.75 0.75 0.75\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname sph_geo\n\tradius 0.6\n}\n\n"
	"standard_object\n{\n\tname sph_obj\n\tgeometry sph_geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 6.0\n\tcolor 1 1 1\n\tposition 0 2 4\n}\n";

static void RunIndirectModeEnvBackgroundBlackTest()
{
	std::printf( "=== AgentViewModeRenderTest: \"indirect\" scenario (c) -- directly-visible env background reads BLACK ===\n" );
	const std::string scenePath = WriteTemp( "rise_indirect_envbg.RISEscene", kSceneEnvBackground );
	Check( !scenePath.empty(), "wrote the env-background scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "env-background scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "env-background session wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	// Mean luminance of a guaranteed-background corner block (top-left
	// 12x12) -- the central sphere (radius 0.6 at z=0, fov 45, cam z=5)
	// never reaches the frame corner, so every corner pixel is a primary
	// miss into the env map.
	auto cornerMean = [&]( Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		if( dec.w < 12 || dec.h < 12 ) { Check( false, "frame large enough to sample a corner" ); return -1.0; }
		double sum = 0; unsigned int n = 0;
		for( unsigned int y = 0; y < 12; ++y )
			for( unsigned int x = 0; x < 12; ++x ) {
				const Px& q = dec.at( x, y );
				sum += ( (double)q[0] + (double)q[1] + (double)q[2] ) / 3.0;
				++n;
			}
		return n > 0 ? sum / n : -1.0;
	};

	const double indirectCorner    = cornerMean( Implementation::ViewportRenderMode::Indirect );
	const double deepReflectCorner = cornerMean( Implementation::ViewportRenderMode::DeepReflect );
	std::printf( "  indirectCorner=%.2f deepReflectCorner=%.2f\n", indirectCorner, deepReflectCorner );

	Check( deepReflectCorner > 120.0,
	       "deep_reflect: the directly-visible env background reads brightly (sanity -- the fixture's env IS visible)" );
	Check( indirectCorner >= 0.0 && indirectCorner < 8.0,
	       "MONEY ASSERTION (c): \"indirect\" renders the directly-visible env background BLACK -- the "
	       "primary-miss env is a direct contribution and is suppressed (the review P1: in-loop gates were "
	       "dead; the fix is at the top-level primary-miss return)" );

	pJob->release();
}

static const char* const kSceneSinglePlaneNoIndirectPath =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.75 0.75 0.75\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"box_geometry\n{\n\tname plane_geo\n\twidth 12\n\theight 12\n\tdepth 0.2\n}\n\n"
	"standard_object\n{\n\tname plane_obj\n\tgeometry plane_geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 6.0\n\tcolor 1 1 1\n\tposition 0 2 4\n}\n";

static void RunIndirectModeNoIndirectPathTest()
{
	std::printf( "=== AgentViewModeRenderTest: \"indirect\" scenario (a) -- single plane, no indirect path -> near-black ===\n" );
	const std::string scenePath = WriteTemp( "rise_indirect_singleplane.RISEscene", kSceneSinglePlaneNoIndirectPath );
	Check( !scenePath.empty(), "wrote the single-plane scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "single-plane scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "single-plane session wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	auto meanLuminance = [&]( Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		double sum = 0; unsigned int n = 0;
		for( std::size_t px = 0; px < dec.px.size(); ++px )
		{
			const Px& q = dec.px[px];
			if( q[3] == 0 ) continue;
			sum += ( (double)q[0] + (double)q[1] + (double)q[2] ) / 3.0;
			++n;
		}
		return n > 0 ? sum / n : -1.0;
	};

	const double indirectMean    = meanLuminance( Implementation::ViewportRenderMode::Indirect );
	const double deepReflectMean = meanLuminance( Implementation::ViewportRenderMode::DeepReflect );
	std::printf( "  indirectMean=%.2f deepReflectMean=%.2f\n", indirectMean, deepReflectMean );

	Check( deepReflectMean > 40.0,
	       "deep_reflect: the directly-lit plane reads brightly (sanity -- the fixture is actually lit)" );
	Check( indirectMean >= 0.0 && indirectMean < 8.0,
	       "MONEY ASSERTION (a): \"indirect\" on a scene with NO possible indirect path reads NEAR-BLACK "
	       "(mean below a small threshold) -- the direct contribution was genuinely suppressed, not just dimmed" );
	Check( indirectMean < deepReflectMean - 30.0,
	       "MONEY ASSERTION (a2): \"indirect\" is MEANINGFULLY below \"deep_reflect\" on the same fixture" );

	pJob->release();
}

//----------------------------------------------------------------------
// Scenario (d), review-p3 P2-a fix: scenario (a)'s omni_light is a DELTA-
// POSITION light -- LightSampler::EvaluateDirectLighting's delta-position
// branch means the BSDF-sampled ray can never land exactly on a point
// light (w_bsdf approx 0 by construction), so scenario (a) cannot
// discriminate "suppress NEE only" from "suppress NEE AND its BSDF-
// sampled MIS partner": both give the same near-zero result on a point
// light regardless of which (buggy or fixed) gate is in effect.
//
// This fixture swaps the point light for a LARGE mesh area light
// centered exactly along the plane's normal direction and placed BEHIND
// the camera (z=8, camera at z=5 looking toward -z, plane at z=0) --
// invisible to primary camera rays (camera rays only ever reach z<5) but
// subtending a large solid angle from the plane's surface, so a
// Lambertian cosine-weighted BSDF sample has NON-NEGLIGIBLE probability
// of landing directly on it (the light is centered on the density's own
// peak direction).  Pre-fix (emission suppressed only at depth==0),
// "indirect" renders direct x w_bsdf: a large, easily-measured leak, not
// scenario (a)'s undetectable-by-construction zero.  Post-fix (emission
// suppressed at depth<=1), the leak is gone -- there is no OTHER
// geometry for a genuinely-indirect bounce to land on, so "indirect"
// reads near-black exactly like scenario (a).
//----------------------------------------------------------------------
static const char* const kSceneSinglePlaneLargeAreaLight =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 24\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.75 0.75 0.75\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"box_geometry\n{\n\tname plane_geo\n\twidth 12\n\theight 12\n\tdepth 0.2\n}\n\n"
	"standard_object\n{\n\tname plane_obj\n\tgeometry plane_geo\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname emit_pnt\n\tcolor 3 3 3\n}\n\n"
	// `material none` (the canonical pure-emitter idiom -- see
	// scenes/FeatureBased/BDPT/bdpt_cloister.RISEscene's luminaires): NO
	// underlying reflectance.  Discovered the hard way -- an earlier draft
	// reused a bright reflectance (matching the exitance colour) as the
	// luminaire's substrate material, which gave the light box its own
	// Lambertian BSDF lobe with albedo 3.0 (unphysical >1, but still a
	// real scatter event): a depth==1 BSDF-sampled ray landing on the
	// light (correctly emission-suppressed by the P2-a fix) would then
	// itself SCATTER a depth==2 continuation off that bright substrate
	// back toward the plane, which is genuinely-indirect two-bounce
	// transport, not a residual leak -- but the inflated albedo amplified
	// it into a large, confounding signal that masked whether the P2-a
	// fix actually worked.  `material none` removes that confound: the
	// light box ONLY emits, never scatters, so nothing but the intended
	// depth<=1 gate governs what reaches the plane.
	"lambertian_luminaire_material\n{\n\tname emitmat\n\texitance emit_pnt\n\tmaterial none\n\tscale 6.0\n}\n\n"
	"box_geometry\n{\n\tname lightgeo\n\twidth 8\n\theight 8\n\tdepth 0.2\n}\n\n"
	// Centered on the plane's normal, BEHIND the camera -- never a primary
	// hit, but a large solid angle as seen from the plane's surface.
	"standard_object\n{\n\tname big_light\n\tgeometry lightgeo\n\tmaterial emitmat\n\tposition 0 0 8\n}\n";

static void RunIndirectModeBsdfMisPartnerLeakTest()
{
	std::printf( "=== AgentViewModeRenderTest: \"indirect\" scenario (d), review-p3 P2-a -- BSDF-sampled MIS partner of NEE must ALSO be suppressed ===\n" );
	const std::string scenePath = WriteTemp( "rise_indirect_largearealight.RISEscene", kSceneSinglePlaneLargeAreaLight );
	Check( !scenePath.empty(), "wrote the single-plane large-area-light scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "large-area-light scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "large-area-light session wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	auto meanLuminance = [&]( Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		return MeanLuminance( dec );
	};

	const double indirectMean    = meanLuminance( Implementation::ViewportRenderMode::Indirect );
	const double deepReflectMean = meanLuminance( Implementation::ViewportRenderMode::DeepReflect );
	std::printf( "  indirectMean=%.2f deepReflectMean=%.2f\n", indirectMean, deepReflectMean );

	Check( deepReflectMean > 40.0,
	       "deep_reflect: the directly-lit plane reads brightly (sanity -- the fixture is actually lit)" );
	// MONEY ASSERTION (d): with the BSDF-sampled MIS partner of the
	// suppressed depth==0 NEE ALSO suppressed (depth<=1 emission gate),
	// "indirect" on a large-solid-angle area light reads NEAR-BLACK, not
	// direct x w_bsdf.  Pre-fix (emission gate only at depth==0), this
	// scene's w_bsdf is substantial (the light is centered on the
	// Lambertian cosine lobe's peak direction) and this assertion FAILS --
	// verified by temporarily reverting the depth<=1 gate back to
	// depth==0 and re-running: indirectMean read well above this
	// threshold (a large, easily-measured leak, unlike scenario (a)'s
	// undetectable-by-construction delta-light zero).
	Check( indirectMean >= 0.0 && indirectMean < 8.0,
	       "MONEY ASSERTION (d): \"indirect\" suppresses BOTH the depth==0 NEE and its depth==1 BSDF-sampled "
	       "MIS partner on a large area light -- reads NEAR-BLACK, not direct x w_bsdf (review-p3 P2-a)" );
	Check( indirectMean < deepReflectMean - 30.0,
	       "MONEY ASSERTION (d2): \"indirect\" is MEANINGFULLY below \"deep_reflect\" on the same fixture" );

	pJob->release();
}

//----------------------------------------------------------------------
// Scenario (b): reuse the Cornell-corner colour-bleed fixture
// (kSceneColorBleedCornellCorner, already defined above for the "direct"
// missing-indirect-bleed test) -- under "indirect", the whole-frame mean is
// a MEANINGFUL FRACTION below "deep_reflect"'s (the direct contribution at
// every camera-visible vertex is removed, leaving only bounce energy).
//----------------------------------------------------------------------
static void RunIndirectModeColorBleedSceneMeanTest()
{
	std::printf( "=== AgentViewModeRenderTest: \"indirect\" scenario (b) -- colour-bleed scene mean vs deep_reflect ===\n" );
	const std::string scenePath = WriteTemp( "rise_indirect_colorbleed.RISEscene", kSceneColorBleedCornellCorner );
	Check( !scenePath.empty(), "wrote the Cornell-corner colour-bleed scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "colour-bleed scene loads via the CST path (indirect test)" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "colour-bleed session wraps the locally-owned Job (indirect test)" );
	if( !session ) { pJob->release(); return; }

	auto meanLuminance = [&]( Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		double sum = 0; unsigned int n = 0;
		for( std::size_t px = 0; px < dec.px.size(); ++px )
		{
			const Px& q = dec.px[px];
			if( q[3] == 0 ) continue;
			sum += ( (double)q[0] + (double)q[1] + (double)q[2] ) / 3.0;
			++n;
		}
		return n > 0 ? sum / n : -1.0;
	};

	const double indirectMean    = meanLuminance( Implementation::ViewportRenderMode::Indirect );
	const double deepReflectMean = meanLuminance( Implementation::ViewportRenderMode::DeepReflect );
	std::printf( "  indirectMean=%.2f deepReflectMean=%.2f\n", indirectMean, deepReflectMean );

	Check( indirectMean >= 0.0 && deepReflectMean >= 0.0, "both renders produced real pixels" );
	Check( indirectMean < deepReflectMean - 8.0,
	       "MONEY ASSERTION (b): \"indirect\"'s whole-frame mean is a MEANINGFUL FRACTION below "
	       "\"deep_reflect\"'s on the same Cornell-corner scene -- the direct contribution is genuinely "
	       "missing, not just noise" );

	pJob->release();
}

//----------------------------------------------------------------------
// GUI render modes P2b `clay_lights` (docs/gui/RENDER_MODES.md §3
// Lighting): albedo-independence discrimination for PathTracingIntegrator::
// SetClayOverride.  Two scenes identical except ONE floor's reflectance
// colour (saturated red vs saturated blue) -- under "clay_lights" both
// floors are substituted for the SAME neutral clay, so the two renders'
// colour bias should MATCH within a tight tolerance; under a real transport
// render ("deep_reflect"), the two floors' authored colours dominate and
// the colour bias differs substantially between the two scenes.
//----------------------------------------------------------------------
static const char* const kSceneClayFloorRed =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.9 0.05 0.05\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -0.1 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 6.0\n\tcolor 1 1 1\n\tposition 0 3 2\n}\n";

static const char* const kSceneClayFloorBlue =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.05 0.05 0.9\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -0.1 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 6.0\n\tcolor 1 1 1\n\tposition 0 3 2\n}\n";

// Whole-frame (meanR - 0.5*(meanG+meanB)) colour-bias metric -- positive for
// a red-dominant image, negative for a blue-dominant one, near zero for a
// neutral-grey (clay) image.  Same idiom as RunDirectModeMissingIndirectBleedTest's
// per-patch redBias above, applied whole-frame here.
static double ColorBias( const Decoded& d )
{
	double r = 0, g = 0, b = 0; unsigned int n = 0;
	for( std::size_t px = 0; px < d.px.size(); ++px )
	{
		const Px& q = d.px[px];
		if( q[3] == 0 ) continue;
		r += q[0]; g += q[1]; b += q[2];
		++n;
	}
	if( n == 0 ) return 0.0;
	r /= n; g /= n; b /= n;
	return r - 0.5 * ( g + b );
}

static void RunClayLightsAlbedoIndependenceTest()
{
	std::printf( "=== AgentViewModeRenderTest: \"clay_lights\" albedo-independence discrimination ===\n" );

	const std::string redPath  = WriteTemp( "rise_clay_floor_red.RISEscene", kSceneClayFloorRed );
	const std::string bluePath = WriteTemp( "rise_clay_floor_blue.RISEscene", kSceneClayFloorBlue );
	Check( !redPath.empty() && !bluePath.empty(), "wrote both clay-floor scenes" );

	Job* pRedJob = new Job();
	Check( pRedJob->LoadAsciiSceneViaCst( redPath.c_str() ), "red-floor scene loads via the CST path" );
	std::unique_ptr<AgentSession> redSession = AgentSession::WrapJob( pRedJob );
	Check( redSession != nullptr, "red-floor session wraps" );

	Job* pBlueJob = new Job();
	Check( pBlueJob->LoadAsciiSceneViaCst( bluePath.c_str() ), "blue-floor scene loads via the CST path" );
	std::unique_ptr<AgentSession> blueSession = AgentSession::WrapJob( pBlueJob );
	Check( blueSession != nullptr, "blue-floor session wraps" );

	if( !redSession || !blueSession ) { pRedJob->release(); pBlueJob->release(); return; }

	auto renderBias = [&]( AgentSession& session, Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session.Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return 0.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return 0.0; }
		return ColorBias( dec );
	};

	// Under a REAL transport render, the two floors' authored colours
	// dominate: the red scene reads strongly R-biased, the blue scene
	// strongly negatively biased -- the two DIFFER substantially.
	const double beautyBiasRed  = renderBias( *redSession,  Implementation::ViewportRenderMode::DeepReflect );
	const double beautyBiasBlue = renderBias( *blueSession, Implementation::ViewportRenderMode::DeepReflect );
	std::printf( "  deep_reflect: biasRed=%.2f biasBlue=%.2f\n", beautyBiasRed, beautyBiasBlue );
	Check( beautyBiasRed > 10.0,
	       "deep_reflect: the red floor reads R-dominant (sanity -- authored colour is honored)" );
	Check( beautyBiasBlue < -10.0,
	       "deep_reflect: the blue floor reads B-dominant (sanity -- authored colour is honored)" );
	Check( ( beautyBiasRed - beautyBiasBlue ) > 20.0,
	       "deep_reflect: the two scenes' colour bias DIFFERS substantially -- materials genuinely drive the image" );

	// Under "clay_lights", BOTH floors are substituted for the same neutral
	// clay -- the two renders' colour bias should MATCH within a tight
	// tolerance, both close to zero (no residual material colour).
	const double clayBiasRed  = renderBias( *redSession,  Implementation::ViewportRenderMode::ClayLights );
	const double clayBiasBlue = renderBias( *blueSession, Implementation::ViewportRenderMode::ClayLights );
	std::printf( "  clay_lights: biasRed=%.2f biasBlue=%.2f\n", clayBiasRed, clayBiasBlue );
	Check( std::abs( clayBiasRed ) < 8.0,
	       "MONEY ASSERTION: clay_lights on the red-floor scene shows NO material colour bias (neutralized to clay)" );
	Check( std::abs( clayBiasBlue ) < 8.0,
	       "MONEY ASSERTION: clay_lights on the blue-floor scene shows NO material colour bias (neutralized to clay)" );
	Check( std::abs( clayBiasRed - clayBiasBlue ) < 6.0,
	       "MONEY ASSERTION: clay_lights's colour bias MATCHES within a tight tolerance between the red-floor and "
	       "blue-floor scenes -- materials are genuinely neutralized, independent of the authored albedo" );

	pRedJob->release();
	pBlueJob->release();
}

//----------------------------------------------------------------------
// GUI render modes P2a `render{view:}` surface (docs/gui/RENDER_MODES.md
// §8, deferred from P1): a headless (WrapJob, no controller) session falls
// back to resolving `view` against a SCENE CAMERA of that name.  Two
// cameras positioned so their renders visibly differ (front vs. side view
// of the same mesh+sphere pair); an unresolvable name fails honestly with
// the available-name list.
//----------------------------------------------------------------------
static const char* const kSceneTwoCameras =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tname camFront\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"pinhole_camera\n{\n\tname camSide\n\tlocation 6 0 0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"box_geometry\n{\n\tname boxbase\n\twidth 1.3\n\theight 1.3\n\tdepth 1.3\n}\n\n"
	"standard_object\n{\n\tname mesh_obj\n\tgeometry boxbase\n\tmaterial mat\n\tposition -1.3 0 0\n}\n\n"
	"sphere_geometry\n{\n\tname sph_geo\n\tradius 0.9\n}\n\n"
	"standard_object\n{\n\tname sph_obj\n\tgeometry sph_geo\n\tmaterial mat\n\tposition 1.3 0 0\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

static void RunViewArgEndToEndTest()
{
	std::printf( "=== AgentViewModeRenderTest: render{view:} end-to-end (headless scene-camera fallback) ===\n" );
	const std::string scenePath = WriteTemp( "rise_view_arg_e2e.RISEscene", kSceneTwoCameras );
	Check( !scenePath.empty(), "wrote the two-camera scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "two-camera scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "view-arg session wraps the locally-owned Job (headless, no controller)" );
	if( !session ) { pJob->release(); return; }
	Check( !session->HasController(), "view-arg session is genuinely headless -- exercises the scene-camera fallback, not the named-view store" );

	AgentRenderParams pFront;
	pFront.view = "camFront";
	AgentRenderResult rFront = session->Render( pFront );
	Check( rFront.ok, "view:\"camFront\" render succeeds" );
	Check( rFront.cameraOverridden, "view:\"camFront\" applied a camera override" );

	AgentRenderParams pSide;
	pSide.view = "camSide";
	AgentRenderResult rSide = session->Render( pSide );
	Check( rSide.ok, "view:\"camSide\" render succeeds" );
	Check( rSide.cameraOverridden, "view:\"camSide\" applied a camera override" );

	Decoded decFront, decSide;
	Check( DecodePng( rFront.png, decFront ), "camFront PNG decodes" );
	Check( DecodePng( rSide.png, decSide ), "camSide PNG decodes" );
	if( decFront.w == decSide.w && decFront.h == decSide.h && !decFront.px.empty() )
	{
		unsigned int differing = 0;
		for( std::size_t px = 0; px < decFront.px.size(); ++px )
		{
			if( decFront.px[px] != decSide.px[px] ) ++differing;
		}
		Check( differing > 0,
		       "MONEY ASSERTION: camFront and camSide renders DIFFER -- view: actually changed the vantage, "
		       "not a silent no-op onto the same (implicit) active camera" );
	}

	// After both view-overridden renders, the active camera + its properties
	// are unperturbed (the ephemeral-override capture/restore contract).
	const std::string activeAfter = pJob->GetActiveCameraName();
	Check( !activeAfter.empty(), "the job still has an active camera after two view-overridden renders" );

	// Invalid name: fails honestly, listing the available (scene camera)
	// names -- never silently falls back to the active camera.
	AgentRenderParams pBad;
	pBad.view = "definitely_not_a_camera_or_view";
	AgentRenderResult rBad = session->Render( pBad );
	Check( !rBad.ok, "an unresolvable view name fails the render" );
	Check( rBad.message.find( "definitely_not_a_camera_or_view" ) != std::string::npos,
	       "the failure message echoes the requested name" );
	Check( rBad.message.find( "camFront" ) != std::string::npos && rBad.message.find( "camSide" ) != std::string::npos,
	       "MONEY ASSERTION: the failure message lists the available scene-camera names" );

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
// OFF: facets and depth with the DEFAULT (first surface) vs an EXPLICIT
// xray:true over a transmissive glass sphere placed IN FRONT of
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

	// (a) facets at the DEFAULT (xray defaults FALSE) -- should show the
	// glass surface itself with NO extra param.
	AgentRenderParams facetsDefault;
	facetsDefault.renderTarget = AgentRenderTarget::ViewMode;
	facetsDefault.viewMode     = Implementation::ViewportRenderMode::Facets;
	AgentRenderResult rFacetsDefault = session->Render( facetsDefault );
	Check( rFacetsDefault.ok, "facets at the default (xray implicitly false) succeeds" );
	Check( rFacetsDefault.message.find( "xray" ) != std::string::npos,
	       "facets-at-default message notes xray is inactive" );
	Decoded decFacetsDefault;
	Check( DecodePng( rFacetsDefault.png, decFacetsDefault ), "facets-at-default PNG decodes" );

	// (b) facets with an EXPLICIT xray:true -- shows the mesh underneath.
	AgentRenderParams facetsXray = facetsDefault;
	facetsXray.xray = true;
	AgentRenderResult rFacetsXray = session->Render( facetsXray );
	Check( rFacetsXray.ok, "facets with explicit xray:true succeeds" );
	Decoded decFacetsXray;
	Check( DecodePng( rFacetsXray.png, decFacetsXray ), "facets-xray-true PNG decodes" );

	if( glassBBox.found > 0 &&
	    decFacetsXray.w == decFacetsDefault.w && decFacetsXray.h == decFacetsDefault.h ) {
		unsigned int differing = 0;
		for( unsigned int y = glassBBox.minY; y <= glassBBox.maxY; ++y ) {
			for( unsigned int x = glassBBox.minX; x <= glassBBox.maxX; ++x ) {
				const Px& a = decFacetsXray.at( x, y );
				const Px& b = decFacetsDefault.at( x, y );
				if( a[0] != b[0] || a[1] != b[1] || a[2] != b[2] ) ++differing;
			}
		}
		Check( differing > 0,
		       "MONEY ASSERTION (a/b): facets at the DEFAULT shows DIFFERENT pixels than an explicit "
		       "xray:true over the glass sphere's own screen region -- the default shows the glass "
		       "surface, and xray:true reveals the mesh underneath" );
	}

	// (c) depth at the DEFAULT vs an EXPLICIT xray:true -- should differ
	// (x-ray sees through to the mesh, which is farther away than the
	// default glass-sphere surface).
	AgentRenderParams depthDefault;
	depthDefault.renderTarget = AgentRenderTarget::ViewMode;
	depthDefault.viewMode     = Implementation::ViewportRenderMode::Depth;
	AgentRenderResult rDepthDefault = session->Render( depthDefault );
	Check( rDepthDefault.ok, "depth at the default (xray implicitly false) succeeds" );
	Decoded decDepthDefault;
	Check( DecodePng( rDepthDefault.png, decDepthDefault ), "depth-at-default PNG decodes" );

	AgentRenderParams depthXray = depthDefault;
	depthXray.xray = true;
	AgentRenderResult rDepthXray = session->Render( depthXray );
	Check( rDepthXray.ok, "depth with explicit xray:true succeeds" );
	Decoded decDepthXray;
	Check( DecodePng( rDepthXray.png, decDepthXray ), "depth-xray-true PNG decodes" );

	if( decDepthXray.w == decDepthDefault.w && decDepthXray.h == decDepthDefault.h ) {
		unsigned int differing = 0;
		for( std::size_t px = 0; px < decDepthXray.px.size(); ++px ) {
			if( decDepthXray.px[px][0] != decDepthDefault.px[px][0] ) ++differing;
		}
		Check( differing > 0,
		       "MONEY ASSERTION (c): depth at the DEFAULT differs from depth with explicit xray:true "
		       "somewhere in the frame" );
	}

	// (d) explicit xray:true with mode:"beauty" (the default
	// renderTarget) is ACCEPTED, not rejected, and carries the honest
	// ignored note.
	AgentRenderParams beautyXray;
	// renderTarget stays Beauty (the default); xray is explicitly enabled to
	// prove it is honestly ignored under mode:beauty.
	beautyXray.xray = true;
	AgentRenderResult rBeautyXray = session->Render( beautyXray );
	Check( rBeautyXray.ok, "explicit xray:true with mode:beauty is accepted (not rejected)" );
	Check( rBeautyXray.message.find( "ignored" ) != std::string::npos,
	       "MONEY ASSERTION (d): explicit xray:true under mode:beauty carries an honest 'ignored' note in the message" );

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
	depthP.xray         = true;
	// This regression specifically exercises the explicit see-through path.
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
	// This regression exercises the explicit see-through walk.  The product
	// default is intentionally opaque to transparent objects.
	depthP.xray         = true;
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

//----------------------------------------------------------------------
// (11) External review round 6, item 1 (P1) regression: RayCaster::
// ResolveXrayView_'s degenerate-self-hit predicate must NOT accept a
// genuine EXIT face of a shell/box THINNER than `selfHitWindow`
// (max(1e-6, 16*curEps)) as a self-hit just because it lands inside
// that range window.  Without the facing test, the walk retries with a
// DOUBLED eps re-anchored at the ENTRY face -- and because the doubling
// ladder grows geometrically (1e-9 * 2^n), the retry that finally
// crosses the glass's own 1e-7 depth threshold overshoots by an amount
// on the SAME order as that threshold, comfortably clearing a second,
// thin opaque layer sitting immediately behind the glass too (see the
// derivation in RayCaster.cpp's own comment at the fix site).  The FIX
// (facing test: only classify as self-hit when the re-hit faces the
// SAME way as the face being left) accepts the exit face on the very
// FIRST check -- zero retries -- and correctly walks onto the opaque
// backstop next.
//
// Scene: `glass_obj` is a box 1.2x1.2 wide with a 1e-7 DEPTH (thinner
// than the 1e-6 selfHitWindow floor by a full order of magnitude),
// centred at z=1.7 (entry z~1.70000005, exit z~1.69999995).
// `bs_obj` ("backstop") is a SMALLER (1.0x1.0, per the review round 4
// lesson: an undersized backstop keeps the boundary-straddle
// discriminator below honest) opaque box whose front face sits ~1e-8
// behind the glass's exit face (bs_obj spans z in
// [1.69999993, 1.69999994]), and `bg_obj` is a large opaque plane far
// behind everything (z=-2).  With the fix, the through-glass silhouette
// centre resolves the NEAR backstop; without it (facing test disabled),
// the walk's retry ladder leaps clean over both the 1e-7 glass and the
// 1e-8 gap in one doubling step and resolves the FAR background
// instead -- see the numeric derivation in RayCaster.cpp's comment.
//
// Discriminator: the same silhouette-boundary technique as
// RunLargeTransverseCoordinateXrayTest / RunScaledGlassStandoffTest
// (single-render, auto-window-safe): a reference objectmap locates
// glass_obj's own screen-space disk; the CENTRE of that disk (guaranteed
// to sit within the smaller, co-centred backstop's silhouette too) is
// compared against a pixel 2px OUTSIDE the disk (which hits `bg_obj`
// directly, unaffected by the x-ray resolve bug).  If the walk stops at
// the near backstop (fix), the centre is much BRIGHTER (nearer) than
// the outside sample; if the walk leaps to `bg_obj` (bug), both samples
// resolve to the same background plane and the delta collapses.
//
// Verified discriminating (manual step during development, not re-run
// by this test): with the facing test temporarily removed from
// RayCaster.cpp (degrading the predicate back to review round 5's
// range-only check), this test's MONEY ASSERTION fails -- the centre
// and outside samples both land on bg_obj and the delta collapses to a
// handful of bytes, exactly the "walk leaps both thin surfaces to bg"
// symptom the fix closes.  Restoring the facing test passes again.
//----------------------------------------------------------------------

static const char* const kSceneThinGlassNearBackstop =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname bg_pnt\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname bg_mat\n\treflectance bg_pnt\n}\n\n"
	"box_geometry\n{\n\tname bg_geo\n\twidth 2000\n\theight 2000\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname bg_obj\n\tgeometry bg_geo\n\tmaterial bg_mat\n\tposition 0 0 -2\n}\n\n"
	"uniformcolor_painter\n{\n\tname bs_pnt\n\tcolor 0.9 0.9 0.9\n}\n\n"
	"lambertian_material\n{\n\tname bs_mat\n\treflectance bs_pnt\n}\n\n"
	"box_geometry\n{\n\tname bs_geo\n\twidth 1.0\n\theight 1.0\n\tdepth 0.00000001\n}\n\n"
	"standard_object\n{\n\tname bs_obj\n\tgeometry bs_geo\n\tmaterial bs_mat\n\tposition 0 0 1.699999935\n}\n\n"
	"box_geometry\n{\n\tname glass_geo\n\twidth 1.2\n\theight 1.2\n\tdepth 0.0000001\n}\n\n"
	"dielectric_material\n{\n\tname glass_mat\n\ttau 1.0 1.0 1.0\n\tior 1.5\n}\n\n"
	"standard_object\n{\n\tname glass_obj\n\tgeometry glass_geo\n\tmaterial glass_mat\n\tposition 0 0 1.7\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

static void RunThinGlassNearOpaqueDiscriminationTest()
{
	std::printf( "=== AgentViewModeRenderTest: (11) sub-window-thin glass resolves the NEAR opaque backstop, not a leapt-over far bg (review r6, item 1) ===\n" );

	const std::string scenePath = WriteTemp( "rise_viewmode_thinglass.RISEscene", kSceneThinGlassNearBackstop );
	Check( !scenePath.empty(), "wrote the thin-glass+near-backstop+bg scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "thin-glass scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "thin-glass session wraps the Job" );
	if( !session ) { pJob->release(); return; }

	AgentRenderParams objP;
	objP.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult objR = session->Render( objP );
	Check( objR.ok, "thin-glass reference objectmap render succeeds" );
	Decoded objDec;
	Check( DecodePng( objR.png, objDec ), "thin-glass objectmap PNG decodes" );
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
	depthP.xray         = true;
	AgentRenderResult rDepth = session->Render( depthP );
	Check( rDepth.ok, "thin-glass depth render succeeds" );
	Decoded decDepth;
	Check( DecodePng( rDepth.png, decDepth ), "thin-glass depth PNG decodes" );

	if( glassBBox.found > 0 && decDepth.w == 64 && decDepth.h == 64 &&
	    glassBBox.maxX + 2 < decDepth.w && glassBBox.minX + glassBBox.maxX > 0 ) {
		const unsigned int row = ( glassBBox.minY + glassBBox.maxY ) / 2;
		const Px& center  = decDepth.at( ( glassBBox.minX + glassBBox.maxX ) / 2, row );
		const Px& outside = decDepth.at( glassBBox.maxX + 2, row );
		Check( center[3] != 0 && outside[3] != 0, "both thin-glass samples are real hits" );
		const int delta = static_cast<int>( center[0] ) - static_cast<int>( outside[0] );
		Check( delta > 40,
		       "MONEY ASSERTION: through the sub-window-thin glass, the silhouette centre pixel resolves the "
		       "NEAR opaque backstop (~4.3 world units away), far BRIGHTER than the pixel 2px outside the "
		       "silhouette which hits the far background directly (~8 world units away) -- the walk's facing "
		       "test accepted the glass's own exit face on the first check instead of retry-doubling from the "
		       "entry face and leaping over both the glass and the backstop onto the same far background the "
		       "outside pixel sees" );
	}

	pJob->release();
}

//----------------------------------------------------------------------
// (12) External review round 7, item 1 (P1) regression: double-sided
// triangle meshes flip `vGeomNormal` to oppose the incoming ray on EVERY
// face -- entry AND exit alike (TriangleMeshGeometry::IntersectRay /
// TriangleMeshGeometryIndexed::IntersectRay).  Round 6's facing test
// (previous test above) distinguishes a genuine exit from a self-hit by
// checking the sign of `Dot(vGeomNormal, dir)` relative to the entry
// face -- but under the double-sided flip, BOTH the entry and the exit
// face are oriented to face the ray, so the raw dot product is
// SAME-SIGNED on both, and round 6's fix alone can't tell them apart
// for THIS geometry (round 6's own "(a)" caveat: non-standard exit-
// normal orientation degrades to the round-5 range-only behavior).  The
// round 7 fix recovers the TRUE (pre-flip) facing via
// RayIntersectionGeometric::bGeomNormalOrientedToRay, which the mesh
// geometries now set at the exact site they flip `vGeomNormal` --
// `RayCaster::ResolveXrayView_`'s facing test un-flips before comparing
// signs, so it regains discrimination power on double-sided meshes too.
//
// Scene: identical shape/gap/backstop geometry to
// kSceneThinGlassNearBackstop above (box 1.2x1.2 wide, 1e-7 depth,
// centred at z=1.7; an undersized 1.0x1.0 opaque backstop ~1e-8 behind
// the exit face; a large opaque background plane far behind at z=-2) --
// but `glass_obj`'s geometry is a `displaced_geometry` wrapping that
// same thin box (`base_geometry`, no displacement painter -- pure
// tessellation, matches kSceneMeshAndSphere's own established idiom for
// "a genuine, self-contained ITriangleMeshGeometryIndexed") with
// `double_sided true`, so vGeomNormal is genuinely flipped-to-ray on
// both faces instead of naturally opposite-signed the way an untouched
// analytic/single-sided box's exit face is.
//
// Discriminator: identical silhouette-boundary technique to the round 6
// test -- the through-mesh silhouette centre pixel (depth view, explicit
// xray:true) must resolve the NEAR backstop (far brighter / less
// distant than a pixel 2px outside the silhouette, which hits the far
// background plane directly).  If the round 7 fix's flag-based facing
// recovery is removed (facing test falls back to the raw, flipped dot
// product), this MONEY ASSERTION fails: the double-sided flip makes
// entry and exit same-signed again, round 6's predicate misclassifies
// the exit as a self-hit, and the retry-doubling ladder leaps the walk
// clean over the thin mesh AND the undersized backstop onto the same
// far background the outside pixel sees -- collapsing the delta.
//----------------------------------------------------------------------

static const char* const kSceneThinDoubleSidedMeshNearBackstop =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname bg_pnt\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname bg_mat\n\treflectance bg_pnt\n}\n\n"
	"box_geometry\n{\n\tname bg_geo\n\twidth 2000\n\theight 2000\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname bg_obj\n\tgeometry bg_geo\n\tmaterial bg_mat\n\tposition 0 0 -2\n}\n\n"
	"uniformcolor_painter\n{\n\tname bs_pnt\n\tcolor 0.9 0.9 0.9\n}\n\n"
	"lambertian_material\n{\n\tname bs_mat\n\treflectance bs_pnt\n}\n\n"
	"box_geometry\n{\n\tname bs_geo\n\twidth 1.0\n\theight 1.0\n\tdepth 0.00000001\n}\n\n"
	"standard_object\n{\n\tname bs_obj\n\tgeometry bs_geo\n\tmaterial bs_mat\n\tposition 0 0 1.699999935\n}\n\n"
	"box_geometry\n{\n\tname glass_base_geo\n\twidth 1.2\n\theight 1.2\n\tdepth 0.0000001\n}\n\n"
	"displaced_geometry\n{\n\tname glass_geo\n\tbase_geometry glass_base_geo\n\tdetail 1\n\tdouble_sided true\n}\n\n"
	"dielectric_material\n{\n\tname glass_mat\n\ttau 1.0 1.0 1.0\n\tior 1.5\n}\n\n"
	"standard_object\n{\n\tname glass_obj\n\tgeometry glass_geo\n\tmaterial glass_mat\n\tposition 0 0 1.7\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

static void RunDoubleSidedThinMeshNearOpaqueDiscriminationTest()
{
	std::printf( "=== AgentViewModeRenderTest: (12) sub-window-thin DOUBLE-SIDED mesh resolves the NEAR opaque backstop, not a leapt-over far bg (review r7, item 1) ===\n" );

	const std::string scenePath = WriteTemp( "rise_viewmode_thindsmesh.RISEscene", kSceneThinDoubleSidedMeshNearBackstop );
	Check( !scenePath.empty(), "wrote the thin-double-sided-mesh+near-backstop+bg scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "thin-double-sided-mesh scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "thin-double-sided-mesh session wraps the Job" );
	if( !session ) { pJob->release(); return; }

	AgentRenderParams objP;
	objP.renderTarget = AgentRenderTarget::ObjectMap;
	AgentRenderResult objR = session->Render( objP );
	Check( objR.ok, "thin-double-sided-mesh reference objectmap render succeeds" );
	Decoded objDec;
	Check( DecodePng( objR.png, objDec ), "thin-double-sided-mesh objectmap PNG decodes" );
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
	depthP.xray         = true;
	AgentRenderResult rDepth = session->Render( depthP );
	Check( rDepth.ok, "thin-double-sided-mesh depth render succeeds" );
	Decoded decDepth;
	Check( DecodePng( rDepth.png, decDepth ), "thin-double-sided-mesh depth PNG decodes" );

	if( glassBBox.found > 0 && decDepth.w == 64 && decDepth.h == 64 &&
	    glassBBox.maxX + 2 < decDepth.w && glassBBox.minX + glassBBox.maxX > 0 ) {
		const unsigned int row = ( glassBBox.minY + glassBBox.maxY ) / 2;
		const Px& center  = decDepth.at( ( glassBBox.minX + glassBBox.maxX ) / 2, row );
		const Px& outside = decDepth.at( glassBBox.maxX + 2, row );
		Check( center[3] != 0 && outside[3] != 0, "both thin-double-sided-mesh samples are real hits" );
		const int delta = static_cast<int>( center[0] ) - static_cast<int>( outside[0] );
		Check( delta > 40,
		       "MONEY ASSERTION: through the sub-window-thin DOUBLE-SIDED mesh, the silhouette centre pixel "
		       "resolves the NEAR opaque backstop, far BRIGHTER than the pixel 2px outside the silhouette which "
		       "hits the far background directly -- the facing test correctly un-flipped the double-sided "
		       "orient-to-ray normal (bGeomNormalOrientedToRay) and accepted the mesh's own exit face on the "
		       "first check instead of retry-doubling from the entry face and leaping over both the mesh and "
		       "the backstop onto the same far background the outside pixel sees" );
	}

	pJob->release();
}

//----------------------------------------------------------------------
// review-p2b P1/P2 regression coverage (external review of the P2a/P2b
// BeautyVariant work): P1-b (all-black placeholder-default UAF-adjacent
// energy loss on the BSSRDF/RW-SSS continuation), P1-c (clay_lights MIS
// material-independence), P2-d (clay_lights bypasses authored SSS), and
// P2-e (indirect misses the primary-segment medium-scatter NEE).
//----------------------------------------------------------------------

// Render one pass DIRECTLY through CreateBeautyVariantPipeline (bypassing
// AgentSession -- this helper calls the 4-arg factory directly, exactly
// like ViewportRenderModeTest.cpp's structural R17 does and like
// AgentSession.cpp / SceneEditController.cpp now both do in production
// (review-p3 P2-c), but through a REAL RasterizeScene pass for pixel-
// level proof).
static bool RenderBeautyVariantPipeline(
	const IScenePriv& scene, Implementation::ViewportRenderMode mode,
	IShader* pDefaultShader, Decoded& out )
{
	IRasterizer* rast = nullptr;
	IRayCaster*  caster = nullptr;
	if( !Implementation::CreateBeautyVariantPipeline( mode, &rast, &caster, pDefaultShader ) || !rast ) {
		safe_release( rast );
		safe_release( caster );
		return false;
	}
	Agent::InMemoryRasterizerOutput* sink = new Agent::InMemoryRasterizerOutput();
	rast->AddRasterizerOutput( sink );
	rast->RasterizeScene( scene, 0, nullptr );
	const std::vector<unsigned char> png = sink->ToPng();
	const bool decoded = DecodePng( png, out );
	safe_release( sink );
	safe_release( caster );
	rast->release();
	return decoded;
}

// review-p3 P1 fix: mean luminance restricted to a screen-space bbox (the
// established objectmap-bbox idiom this file uses everywhere else -- see
// ScanBBoxForColor's callers).  The whole-frame MeanLuminance above is
// dominated by a directly-visible emissive backwall in the P1-b fixture
// (PT shades that primary hit itself -- the default shader under test is
// reached only via the BSSRDF continuation, a small fraction of the
// frame's energy), so a whole-frame assertion can't discriminate a
// blackened default shader from a working one.  Restricting to the SSS
// sphere's own pixel region makes the measurement actually about the
// thing under test.
//! Peak luminance inside a bbox.  For "did a SPECULAR path survive?"
//! questions the MEAN is the wrong metric: a mirror reflects the WHOLE
//! room, and under `indirect` the room's direct lighting legitimately goes
//! dark, so the mean collapses even when the reflected highlight is intact.
//! The reflected lamp is a small, very bright region -- peak luminance is
//! what actually tracks "the specular transport path still contributes".
//! `bbox` is in the REFERENCE image's pixel space (typically a full-res
//! objectmap render); `refW`/`refH` are that reference's dims.  BeautyVariant
//! modes render at their own REDUCED divisor (deep_reflect quarter-res,
//! indirect half-res), so the bbox must be RESCALED into `d`'s space --
//! applying full-res coordinates to a quarter-res image simply misses the
//! image entirely and silently reports "nothing found".
static double MaxLuminanceInBBox( const Decoded& d, const BBox& bbox,
                                  unsigned int refW, unsigned int refH )
{
	double best = -1.0;
	if( bbox.found == 0 || refW == 0 || refH == 0 || d.w == 0 || d.h == 0 ) return best;
	const double sx = (double)d.w / (double)refW;
	const double sy = (double)d.h / (double)refH;
	unsigned int y0 = (unsigned int)( bbox.minY * sy );
	unsigned int y1 = (unsigned int)( bbox.maxY * sy );
	unsigned int x0 = (unsigned int)( bbox.minX * sx );
	unsigned int x1 = (unsigned int)( bbox.maxX * sx );
	if( y1 >= d.h ) y1 = d.h - 1;
	if( x1 >= d.w ) x1 = d.w - 1;
	if( y0 > y1 || x0 > x1 ) return best;
	for( unsigned int y = y0; y <= y1 && y < d.h; ++y ) {
		for( unsigned int x = x0; x <= x1 && x < d.w; ++x ) {
			const Px& q = d.at( x, y );
			const double lum = ( (double)q[0] + (double)q[1] + (double)q[2] ) / 3.0;
			if( lum > best ) best = lum;
		}
	}
	return best;
}

static double MeanLuminanceInBBox( const Decoded& d, const BBox& bbox )
{
	if( bbox.found == 0 ) return -1.0;
	double sum = 0; unsigned int n = 0;
	for( unsigned int y = bbox.minY; y <= bbox.maxY; ++y )
	{
		for( unsigned int x = bbox.minX; x <= bbox.maxX; ++x )
		{
			const Px& q = d.at( x, y );
			if( q[3] == 0 ) continue;
			sum += ( (double)q[0] + (double)q[1] + (double)q[2] ) / 3.0;
			++n;
		}
	}
	return n > 0 ? sum / n : -1.0;
}

// review-p3 P1 fix: a deliberately-black, TEST-ONLY IShader.  The shipped
// P1-b fix means BOTH the null-default and the explicitly-supplied-shader
// paths through CreateBeautyVariantPipeline now build/use a REAL path-
// tracing shader -- there is no live all-black placeholder left in
// production to discriminate against, so a test that only compares
// "no shader supplied" vs "the real global shader supplied" can no longer
// FAIL if a future regression re-blackens the internal default (both
// sides would darken together).  This shader is the actual discriminator:
// passing it as pDefaultShader must measurably darken the SSS sphere's
// own pixel region relative to the real default, proving
// CreateBeautyVariantPipeline's `pDefaultShader` plumbing is genuinely
// wired through to RayCaster::SelectShader's fallback, not silently
// ignored.
namespace {
class TestBlackShader : public IShader, public Implementation::Reference
{
public:
	void Shade( const RuntimeContext&, const RayIntersection&, const IRayCaster&,
	            const IRayCaster::RAY_STATE&, RISEPel& c, const IORStack& ) const override
	{
		c = RISEPel( 0, 0, 0 );
	}
	Scalar ShadeNM( const RuntimeContext&, const RayIntersection&, const IRayCaster&,
	                const IRayCaster::RAY_STATE&, const Scalar, const IORStack& ) const override
	{
		return 0;
	}
	// ShadeHWSS deliberately NOT overridden: IShader's default impl loops
	// ShadeNM per wavelength, which is already all-zero above -- correct
	// for a black test shader with no separate HWSS transport to fake.
	void ResetRuntimeData() const override {}
};
}

//----------------------------------------------------------------------
// P1-b: an SSS sphere with NO explicit per-object shader (standard_object
// never sets `shader` -- the common authoring case), strongly backlit by
// a large emissive wall almost touching it so BSSRDF continuation rays
// reliably escape onto something bright.  RayCaster::CastRay/CastRayNM
// (called recursively by the BSSRDF disk-projection continuation in
// PathTracingIntegrator.cpp) resolves shading via RayCaster::SelectShader,
// which falls back to the caster's OWN default shader for this object.
// review-p3 P1 fix: the shipped P1-b fix means BOTH "no shader supplied"
// and "the scene's real global shader supplied" now build/use a REAL
// path-tracing default (BeautyVariantDefaultShader) -- there is no black
// placeholder left in production, so the MONEY ASSERTIONS below compare
// against a deliberately-black TEST shader (TestBlackShader, above)
// instead, restricted to the SSS sphere's own screen-space region (a
// small fraction of this backlit-backwall-dominated frame).
//----------------------------------------------------------------------
static const char* const kSceneSSSNoShaderBacklit =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 24\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname emit_pnt\n\tcolor 3 3 3\n}\n\n"
	"lambertian_material\n{\n\tname basemat\n\treflectance emit_pnt\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname emitmat\n\texitance emit_pnt\n\tmaterial basemat\n\tscale 4.0\n}\n\n"
	"box_geometry\n{\n\tname backwallgeo\n\twidth 6\n\theight 6\n\tdepth 0.2\n}\n\n"
	// standard_object deliberately has NO `shader` line -- the common
	// authoring shape RayCaster::SelectShader falls back for.
	"standard_object\n{\n\tname backwall\n\tgeometry backwallgeo\n\tposition 0 0 -1.6\n\tmaterial emitmat\n}\n\n"
	"subsurfacescattering_material\n{\n\tname wax\n\tior 1.3\n\tabsorption 0.01 0.3 1\n\tscattering 2\n\tg 0.0\n\troughness 0.0\n}\n\n"
	"sphere_geometry\n{\n\tname spheregeo\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname sss_sphere\n\tgeometry spheregeo\n\tposition 0 0 0\n\tmaterial wax\n}\n";

static void RunBeautyVariantSSSDefaultShaderTest()
{
	std::printf( "=== AgentViewModeRenderTest: P1-b BeautyVariant default-shader SSS energy ===\n" );
	const std::string scenePath = WriteTemp( "rise_beautyvariant_sss_defaultshader.RISEscene", kSceneSSSNoShaderBacklit );
	Check( !scenePath.empty(), "wrote the backlit-SSS-no-shader scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "backlit-SSS scene loads via the CST path" );
	const IScenePriv* scenePriv = pJob->GetScene();
	Check( scenePriv != nullptr, "backlit-SSS job has a Scene" );
	if( !scenePriv ) { pJob->release(); return; }

	IShader* pGlobal = pJob->GetShaders() ? pJob->GetShaders()->GetItem( "global" ) : nullptr;
	Check( pGlobal != nullptr, "the fixture's \"global\" shader resolves via the job's shader manager" );

	// review-p3 P1 fix: locate the SSS sphere's OWN screen-space pixel
	// region via a reference objectmap render (the established
	// objectmap-bbox idiom this file uses everywhere else) -- restricting
	// the mean-luminance measurement below to this bbox instead of the
	// whole frame, which is dominated by the directly-visible emissive
	// backwall (measured pre-fix: whole-frame meanNoShader=210.25,
	// meanWithShader=209.65 -- indistinguishable regardless of what the
	// SSS sphere's own default shader does).  Same film dims/camera as
	// the beauty-variant renders below (both come from the same scene's
	// own authored Film chunk, no override on either path).
	std::unique_ptr<AgentSession> objSession = AgentSession::WrapJob( pJob );
	Check( objSession != nullptr, "backlit-SSS job wraps for the reference objectmap render" );
	BBox sphereBBox{ 0, 0, 0, 0, 0 };
	if( objSession )
	{
		AgentRenderParams objP;
		objP.renderTarget = AgentRenderTarget::ObjectMap;
		AgentRenderResult objR = objSession->Render( objP );
		Check( objR.ok && objR.renderMode == "objectmap", "backlit-SSS reference objectmap render succeeds" );
		Decoded objDec;
		Check( DecodePng( objR.png, objDec ), "backlit-SSS reference objectmap PNG decodes" );
		const LegendEntry* sphereLegend = FindLegend( objR, "sss_sphere" );
		Check( sphereLegend != nullptr, "reference objectmap legend carries sss_sphere" );
		if( sphereLegend )
		{
			unsigned char cb[3];
			Check( HexToBytes( sphereLegend->colorHex, cb ), "sss_sphere colorHex parses" );
			sphereBBox = ScanBBoxForColor( objDec, cb );
			Check( sphereBBox.found > 0, "sss_sphere occupies a nonzero pixel region in the reference objectmap render" );
		}
	}

	// "No default shader supplied" -- the shipped P1-b fix builds a REAL
	// internal default here (BeautyVariantDefaultShader), not a black
	// placeholder.
	Decoded decNoShader;
	Check( RenderBeautyVariantPipeline( *scenePriv, Implementation::ViewportRenderMode::DeepReflect,
	                                     nullptr, decNoShader ),
	       "deep_reflect render with NO default shader decodes" );
	const double meanNoShaderSphere = MeanLuminanceInBBox( decNoShader, sphereBBox );

	// The scene's real production "global" shader supplied explicitly.
	Decoded decWithShader;
	Check( RenderBeautyVariantPipeline( *scenePriv, Implementation::ViewportRenderMode::DeepReflect,
	                                     pGlobal, decWithShader ),
	       "deep_reflect render WITH the real default shader decodes" );
	const double meanWithShaderSphere = MeanLuminanceInBBox( decWithShader, sphereBBox );

	// review-p3 P1 fix: the actual discriminator -- a deliberately-black
	// TEST shader (TestBlackShader, defined above) passed as
	// pDefaultShader.  If CreateBeautyVariantPipeline's `pDefaultShader`
	// plumbing were ever silently ignored (or its internal fallback
	// re-blackened), this render would read the SAME as the real-shader
	// renders above instead of measurably darker.
	IShader* pBlack = new TestBlackShader();
	Decoded decBlackShader;
	Check( RenderBeautyVariantPipeline( *scenePriv, Implementation::ViewportRenderMode::DeepReflect,
	                                     pBlack, decBlackShader ),
	       "deep_reflect render WITH a deliberately-black test default shader decodes" );
	const double meanBlackShaderSphere = MeanLuminanceInBBox( decBlackShader, sphereBBox );
	pBlack->release();

	std::printf( "  sss_sphere bbox=[%u..%u]x[%u..%u] (found=%u)\n",
		sphereBBox.minX, sphereBBox.maxX, sphereBBox.minY, sphereBBox.maxY, sphereBBox.found );
	std::printf( "  meanNoShaderSphere=%.2f meanWithShaderSphere=%.2f meanBlackShaderSphere=%.2f\n",
		meanNoShaderSphere, meanWithShaderSphere, meanBlackShaderSphere );
	Check( meanNoShaderSphere >= 0.0 && meanWithShaderSphere >= 0.0 && meanBlackShaderSphere >= 0.0,
	       "all three sphere-bbox renders produced real pixels" );

	// MONEY ASSERTION (a): the DEFAULT (no pDefaultShader) beauty-variant
	// pipeline carries real BSSRDF continuation energy on the SSS
	// sphere's own screen region -- the all-black placeholder default
	// shader that once silently zeroed it is gone (P1-b), and this is
	// now measured on the region that actually exercises the default
	// shader instead of the whole (backwall-dominated) frame.  Threshold
	// is 130.0, NOT a token >0-style floor: the SSS material's PRIMARY-hit
	// diffusion response alone (independent of any continuation shader --
	// see assertion (b)'s comment below) already puts a correctly-working
	// render around 168-174 on this fixture, while a default shader whose
	// CONTINUATION half is entirely blackened (verified by temporarily
	// zeroing BeautyVariantDefaultShader::Shade/ShadeNM, simulating the
	// pre-P1-b all-black placeholder) reads ~100 -- 130 sits with real
	// margin on both sides of that gap, so this assertion genuinely fails
	// under that regression instead of trivially passing at a near-zero
	// floor no real render would ever cross.
	Check( meanNoShaderSphere > 130.0,
	       "MONEY ASSERTION (a): the DEFAULT (no pDefaultShader) beauty-variant pipeline carries real BSSRDF "
	       "continuation energy on the SSS sphere's OWN pixel region" );
	// MONEY ASSERTION (b): the deliberately-black test shader measurably
	// DARKENS the sphere region relative to the real default -- proof
	// pDefaultShader is genuinely wired through RayCaster::SelectShader's
	// fallback, not silently ignored (the failure mode a same-object-
	// identity structural check like ViewportRenderModeTest.cpp's R17
	// cannot catch).  NOT asserted near-black: measured
	// meanNoShaderSphere=173.84 -> meanBlackShaderSphere=100.47 (a real
	// run, not a placeholder) -- the SSS material's PRIMARY-hit diffusion
	// response (BSSRDF NEE sampled locally at the entry point) is a
	// SEPARATE illumination pathway from the disk-projection/random-walk
	// CONTINUATION rays that recursively call caster.CastRay and resolve
	// through the blackened default -- blacking the continuation alone
	// cannot zero the whole sphere, only measurably darken it.  The
	// relative + absolute thresholds below have generous margin under
	// that real measurement (42% relative drop, 73.4 absolute) while
	// still failing outright if pDefaultShader were silently ignored
	// (which would leave meanBlackShaderSphere == meanNoShaderSphere,
	// zero drop).
	Check( meanBlackShaderSphere < meanNoShaderSphere * 0.8,
	       "MONEY ASSERTION (b): a deliberately-black default shader reads MEANINGFULLY (>=20%) darker than the "
	       "real default over the SSS sphere's own region" );
	Check( meanNoShaderSphere - meanBlackShaderSphere > 30.0,
	       "MONEY ASSERTION (b2): the black test shader is darker than the real default by a LARGE absolute "
	       "margin over the same sphere region -- pDefaultShader is actually reaching RayCaster::SelectShader's "
	       "fallback" );
	// Consistency check (not the money assertion): the built-in default
	// and an explicitly-supplied real production shader agree, since both
	// are genuine path-tracing transport post P1-b.
	Check( fabs( meanWithShaderSphere - meanNoShaderSphere ) < 15.0,
	       "the built-in default shader MATCHES an explicitly-supplied production shader over the sphere region "
	       "(both real path tracing) -- no energy gap between the two entry points" );

	pJob->release();
}

//----------------------------------------------------------------------
// P1-c: clay_lights MIS material-independence.  Two scenes identical
// except the floor's material: one a diffuse Lambertian, one a PERFECT
// MIRROR (delta reflector -- IMaterial::Pdf() delegates to GetSPF()->Pdf(),
// and a delta SPF's Pdf() is 0, the exact "near-zero/delta Pdf()" case the
// P1-c bug targets: LightSampler::EvaluateDirectLighting only applies its
// MIS weight when `p_bsdf > 0`, so a delta-material's NEE contribution
// against the CLAY BRDF's eval goes out UNWEIGHTED (full raw weight)
// while the diffuse-material scene's NEE gets properly power-heuristic
// weighted -- a material-dependent bias clay_lights must not have).
//
// MUST use a MESH area light (a luminaire sphere), NOT a delta omni/spot
// light: LightSampler::EvaluateDirectLighting's delta-position light
// branch never calls pMaterial->Pdf() at all ("delta-position light: w=1,
// no alternative sampling strategy" -- verified by reading the branch),
// so a scene lit only by an omni_light doesn't exercise the P1-c code
// path (confirmed empirically: an earlier omni_light-lit revision of
// this fixture showed NO discrimination between mirror/diffuse pre-fix).
// Only the MESH LUMINARY branch calls pMaterial->Pdf() for its MIS
// weight, so the light here must be an emissive object with a CanBeAreaLight
// geometry (a sphere) positioned so a cosine-sampled BSDF ray from the
// floor has a real chance of hitting it directly, giving p_bsdf a
// meaningful (non-negligible) magnitude to be mis-weighted incorrectly
// against.
//----------------------------------------------------------------------
static const char* const kSceneClayFloorDiffuse =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 32\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.9 0.9 0.9\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -0.1 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname emit_pnt\n\tcolor 6 6 6\n}\n\n"
	"lambertian_material\n{\n\tname lampbase\n\treflectance emit_pnt\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname lampmat\n\texitance emit_pnt\n\tmaterial lampbase\n\tscale 1.0\n}\n\n"
	"sphere_geometry\n{\n\tname lampgeo\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname lamp_obj\n\tgeometry lampgeo\n\tmaterial lampmat\n\tposition 0 3 0\n}\n";

static const char* const kSceneClayFloorMirror =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 32\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.9 0.9 0.9\n}\n\n"
	"perfectreflector_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 8\n\theight 0.2\n\tdepth 8\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -0.1 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname emit_pnt\n\tcolor 6 6 6\n}\n\n"
	"lambertian_material\n{\n\tname lampbase\n\treflectance emit_pnt\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname lampmat\n\texitance emit_pnt\n\tmaterial lampbase\n\tscale 1.0\n}\n\n"
	"sphere_geometry\n{\n\tname lampgeo\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname lamp_obj\n\tgeometry lampgeo\n\tmaterial lampmat\n\tposition 0 3 0\n}\n";

static void RunClayLightsMaterialIndependenceMirrorTest()
{
	std::printf( "=== AgentViewModeRenderTest: P1-c \"clay_lights\" MIS material-independence (mirror vs diffuse) ===\n" );

	const std::string diffusePath = WriteTemp( "rise_clay_floor_diffuse.RISEscene", kSceneClayFloorDiffuse );
	const std::string mirrorPath  = WriteTemp( "rise_clay_floor_mirror.RISEscene",  kSceneClayFloorMirror );
	Check( !diffusePath.empty() && !mirrorPath.empty(), "wrote both diffuse/mirror clay-floor scenes" );

	Job* pDiffuseJob = new Job();
	Check( pDiffuseJob->LoadAsciiSceneViaCst( diffusePath.c_str() ), "diffuse-floor scene loads via the CST path" );
	std::unique_ptr<AgentSession> diffuseSession = AgentSession::WrapJob( pDiffuseJob );
	Check( diffuseSession != nullptr, "diffuse-floor session wraps" );

	Job* pMirrorJob = new Job();
	Check( pMirrorJob->LoadAsciiSceneViaCst( mirrorPath.c_str() ), "mirror-floor scene loads via the CST path" );
	std::unique_ptr<AgentSession> mirrorSession = AgentSession::WrapJob( pMirrorJob );
	Check( mirrorSession != nullptr, "mirror-floor session wraps" );

	if( !diffuseSession || !mirrorSession ) { pDiffuseJob->release(); pMirrorJob->release(); return; }

	auto renderMean = [&]( AgentSession& session, Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session.Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		return MeanLuminance( dec );
	};

	// Sanity: under REAL transport, a mirror floor and a diffuse floor
	// look substantially different (the mirror mostly shows reflected
	// surroundings/near-black, not the flat lit diffuse look).
	const double beautyDiffuse = renderMean( *diffuseSession, Implementation::ViewportRenderMode::DeepReflect );
	const double beautyMirror  = renderMean( *mirrorSession,  Implementation::ViewportRenderMode::DeepReflect );
	std::printf( "  deep_reflect: diffuse=%.2f mirror=%.2f\n", beautyDiffuse, beautyMirror );
	Check( std::abs( beautyDiffuse - beautyMirror ) > 15.0,
	       "deep_reflect: the diffuse and mirror floors DIFFER substantially -- sanity, materials genuinely "
	       "drive the image absent clay_lights" );

	// Under clay_lights, both floors are substituted for the SAME clay
	// lobe: the two renders' mean luminance should MATCH within a tight
	// tolerance, independent of whether the hidden authored material was
	// diffuse or a delta mirror.
	const double clayDiffuse = renderMean( *diffuseSession, Implementation::ViewportRenderMode::ClayLights );
	const double clayMirror  = renderMean( *mirrorSession,  Implementation::ViewportRenderMode::ClayLights );
	std::printf( "  clay_lights: diffuse=%.2f mirror=%.2f\n", clayDiffuse, clayMirror );
	Check( clayDiffuse >= 0.0 && clayMirror >= 0.0, "both clay_lights renders produced real pixels" );
	// Tolerance 1.5: MC noise alone (post-fix, repeated runs) keeps the
	// diffuse/mirror gap under ~0.5; the pre-fix bug (verified by
	// temporarily reverting PathTracingIntegrator.{h,cpp} to HEAD and
	// re-running this exact test) consistently produces a ~3.0-3.5 gap on
	// this fixture -- 1.5 sits with clean margin on both sides of that
	// separation.
	Check( std::abs( clayDiffuse - clayMirror ) < 1.5,
	       "MONEY ASSERTION: clay_lights's mean luminance MATCHES within a tight tolerance whether the hidden "
	       "authored floor material is a diffuse Lambertian or a delta-Pdf() perfect mirror -- the NEE MIS "
	       "weight now agrees with the clay BRDF's own pdf (ClayNEEMaterial), so the mode's material-"
	       "independence contract holds even for materials whose OWN Pdf() is zero/delta (P1-c fix)" );

	pDiffuseJob->release();
	pMirrorJob->release();
}

//----------------------------------------------------------------------
// P2-d: clay_lights must bypass the authored material's subsurface
// transport too, not just its BSDF/SPF.  Two scenes identical in
// geometry/lighting except the sphere's material: one a diffusion-profile
// SSS material (translucent, backlit glow under real transport), one a
// plain opaque Lambertian of a SIMILAR base tone.  Under clay_lights BOTH
// must reduce to the SAME clay Lambertian look (mean luminance close);
// under deep_reflect they must differ substantially (the SSS sphere's
// characteristic glow vs a flat opaque diffuse look).
//----------------------------------------------------------------------
// No background/env and no visible emissive surface geometry -- the ONLY
// light sources are two DELTA omni lights (invisible in-frame), so every
// background pixel outside the sphere's own silhouette is alpha=0 (no
// hit) and MeanLuminance's alpha-skip means the whole-frame mean IS,
// effectively, the sphere's own mean brightness -- no bright backwall to
// dilute the signal.  One light is front-ish (so the opaque sphere is
// visibly lit at all, giving clay_lights real NEE to substitute over);
// the other is directly BEHIND the sphere (drives the SSS backlit-glow
// signature under real transport).
static const char* const kSceneClaySSSSphere =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 24\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"omni_light\n{\n\tname lgt_front\n\tpower 1.5\n\tcolor 1 1 1\n\tposition 1.5 1.5 3\n}\n\n"
	"omni_light\n{\n\tname lgt_back\n\tpower 24.0\n\tcolor 1 1 1\n\tposition 0 0 -3\n}\n\n"
	"subsurfacescattering_material\n{\n\tname wax\n\tior 1.3\n\tabsorption 0.01 0.3 1\n\tscattering 2\n\tg 0.0\n\troughness 0.0\n}\n\n"
	"sphere_geometry\n{\n\tname spheregeo\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname the_sphere\n\tgeometry spheregeo\n\tposition 0 0 0\n\tmaterial wax\n}\n";

static const char* const kSceneClayOpaqueSphere =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 24\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"omni_light\n{\n\tname lgt_front\n\tpower 1.5\n\tcolor 1 1 1\n\tposition 1.5 1.5 3\n}\n\n"
	"omni_light\n{\n\tname lgt_back\n\tpower 24.0\n\tcolor 1 1 1\n\tposition 0 0 -3\n}\n\n"
	"uniformcolor_painter\n{\n\tname sph_pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname opaquemat\n\treflectance sph_pnt\n}\n\n"
	"sphere_geometry\n{\n\tname spheregeo\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname the_sphere\n\tgeometry spheregeo\n\tposition 0 0 0\n\tmaterial opaquemat\n}\n";

static void RunClayLightsSSSBypassTest()
{
	std::printf( "=== AgentViewModeRenderTest: P2-d \"clay_lights\" bypasses authored SSS ===\n" );

	const std::string sssPath    = WriteTemp( "rise_clay_sss_sphere.RISEscene",    kSceneClaySSSSphere );
	const std::string opaquePath = WriteTemp( "rise_clay_opaque_sphere.RISEscene", kSceneClayOpaqueSphere );
	Check( !sssPath.empty() && !opaquePath.empty(), "wrote both SSS/opaque clay-sphere scenes" );

	Job* pSssJob = new Job();
	Check( pSssJob->LoadAsciiSceneViaCst( sssPath.c_str() ), "SSS-sphere scene loads via the CST path" );
	std::unique_ptr<AgentSession> sssSession = AgentSession::WrapJob( pSssJob );
	Check( sssSession != nullptr, "SSS-sphere session wraps" );

	Job* pOpaqueJob = new Job();
	Check( pOpaqueJob->LoadAsciiSceneViaCst( opaquePath.c_str() ), "opaque-sphere scene loads via the CST path" );
	std::unique_ptr<AgentSession> opaqueSession = AgentSession::WrapJob( pOpaqueJob );
	Check( opaqueSession != nullptr, "opaque-sphere session wraps" );

	if( !sssSession || !opaqueSession ) { pSssJob->release(); pOpaqueJob->release(); return; }

	auto renderDec = [&]( AgentSession& session, Implementation::ViewportRenderMode mode ) -> Decoded
	{
		Decoded dec;
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session.Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return dec; }
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); }
		return dec;
	};

	// The discriminating signal is COLOUR, not brightness: the wax SSS
	// material's absorption is strongly wavelength-selective (R 0.01 ~
	// transparent, G 0.3 moderate, B 1.0 heavily absorbed), so light that
	// has actually travelled THROUGH the material (the diffusion-profile
	// transport P2-d bypasses) comes out warm/red-shifted -- a genuine
	// SSS signature no amount of relighting reproduces from a neutral-
	// grey opaque surface.  Reuses ColorBias (defined above for the P1-c
	// albedo-independence test): positive = red-dominant, ~0 = neutral.
	const Decoded beautySssDec    = renderDec( *sssSession,    Implementation::ViewportRenderMode::DeepReflect );
	const Decoded beautyOpaqueDec = renderDec( *opaqueSession, Implementation::ViewportRenderMode::DeepReflect );
	const double beautySssBias    = ColorBias( beautySssDec );
	const double beautyOpaqueBias = ColorBias( beautyOpaqueDec );
	std::printf( "  deep_reflect colour bias: sss=%.2f opaque=%.2f\n", beautySssBias, beautyOpaqueBias );
	Check( !beautySssDec.px.empty() && !beautyOpaqueDec.px.empty(), "both deep_reflect renders produced real pixels" );
	Check( beautySssBias > beautyOpaqueBias + 5.0,
	       "deep_reflect: the SSS sphere reads a MEANINGFULLY warmer/red-shifted colour than the neutral-grey "
	       "opaque sphere -- sanity, the diffusion-profile transport's wavelength-selective absorption is "
	       "genuinely visible absent clay_lights" );

	// Under clay_lights, P2-d bypasses the authored SSS transport entirely
	// (not just the surface BSDF/SPF) -- both spheres reduce to the SAME
	// neutral clay Lambertian, so neither should carry ANY residual colour
	// cast, and the two renders' colour bias should match closely.
	const Decoded claySssDec    = renderDec( *sssSession,    Implementation::ViewportRenderMode::ClayLights );
	const Decoded clayOpaqueDec = renderDec( *opaqueSession, Implementation::ViewportRenderMode::ClayLights );
	const double claySssBias    = ColorBias( claySssDec );
	const double clayOpaqueBias = ColorBias( clayOpaqueDec );
	std::printf( "  clay_lights colour bias: sss=%.2f opaque=%.2f\n", claySssBias, clayOpaqueBias );
	Check( !claySssDec.px.empty() && !clayOpaqueDec.px.empty(), "both clay_lights renders produced real pixels" );
	Check( std::abs( claySssBias ) < 5.0,
	       "MONEY ASSERTION: clay_lights on the SSS sphere carries NO meaningful residual colour cast -- the "
	       "wavelength-selective diffusion-profile transport is genuinely bypassed (P2-d fix), not merely "
	       "relit with the same colour-shifting absorption still active underneath" );
	Check( std::abs( claySssBias - clayOpaqueBias ) < 5.0,
	       "MONEY ASSERTION: clay_lights's colour bias MATCHES within a tight tolerance whether the hidden "
	       "sphere material is diffusion-profile SSS or a plain opaque Lambertian -- both reduce to the exact "
	       "same clay look" );
	Check( std::abs( claySssBias - beautySssBias ) > 5.0,
	       "MONEY ASSERTION: on the SAME SSS-sphere scene, clay_lights's colour bias is SUBSTANTIALLY different "
	       "from deep_reflect's -- the SSS colour signature visible under real transport is genuinely gone "
	       "under clay_lights, not just diluted" );

	pSssJob->release();
	pOpaqueJob->release();
}

//----------------------------------------------------------------------
// P2-e: `indirect` must also suppress the in-scattering NEE at a medium
// scatter event on the PRIMARY camera segment (a camera looking straight
// into fog, scattering before ever hitting a surface), not just surface
// NEE at the camera-visible vertex.  A scene-wide `global_medium` fog with
// an omni light positioned inside it, camera at the origin looking
// straight at the light -- the primary ray scatters in the fog well
// before reaching anything else, so any visible glow here is EXCLUSIVELY
// the primary-segment single-scatter NEE the fix gates.
//----------------------------------------------------------------------
static const char* const kSceneFogPrimaryScatter =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 32\n\tpixel_filter box\n\toidn_denoise false\n\tmax_volume_bounce 8\n}\n\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 0\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"homogeneous_medium\n{\n\tname fog\n\tabsorption 0.02 0.02 0.02\n\tscattering 0.6 0.6 0.6\n\tphase isotropic\n}\n\n"
	"omni_light\n{\n\tname lamp\n\tpower 8.0\n\tcolor 1 1 1\n\tposition 0 0 2.5\n}\n\n"
	"global_medium\n{\n\tmedium fog\n}\n";

static void RunIndirectPrimaryMediumScatterTest()
{
	std::printf( "=== AgentViewModeRenderTest: P2-e \"indirect\" misses primary-segment medium-scatter NEE ===\n" );
	const std::string scenePath = WriteTemp( "rise_indirect_fog_primary.RISEscene", kSceneFogPrimaryScatter );
	Check( !scenePath.empty(), "wrote the fog-with-lamp scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "fog-with-lamp scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "fog-with-lamp session wraps" );
	if( !session ) { pJob->release(); return; }

	auto renderMean = [&]( Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		return MeanLuminance( dec );
	};

	const double deepReflectMean = renderMean( Implementation::ViewportRenderMode::DeepReflect );
	const double indirectMean    = renderMean( Implementation::ViewportRenderMode::Indirect );
	std::printf( "  deepReflectMean=%.2f indirectMean=%.2f\n", deepReflectMean, indirectMean );

	Check( deepReflectMean >= 0.0 && indirectMean >= 0.0, "both renders produced real pixels" );
	Check( deepReflectMean > 3.0,
	       "sanity: deep_reflect shows a real single-scatter glow from the lamp through the fog (not black)" );
	Check( indirectMean < deepReflectMean - 2.0,
	       "MONEY ASSERTION: \"indirect\"'s mean luminance is MEANINGFULLY below \"deep_reflect\"'s on this "
	       "camera-in-fog-with-a-lamp scene -- the primary-segment medium-scatter NEE (the ONLY light path "
	       "reachable before any surface hit here) is genuinely suppressed under indirect, not just surface "
	       "NEE (P2-e fix)" );

	pJob->release();
}

//----------------------------------------------------------------------
// Scenario (g), review-p3: the ENV-SIDE half of the MIS-partner rule.
//
// Scenario (f) exercises env suppression at a LAMBERTIAN depth-0 vertex
// (an env-NEE partner exists there and is suppressed, so the depth-1 env
// hit must be suppressed too).  But its depth-0 scatter is never delta, so
// `!bPassedThroughSpecular` is always true and removing the delta term
// from the ENV gate would NOT move it -- the surviving half of the rule
// was untested.
//
// Here the depth-0 scatter IS delta: a MIRROR floor under a bright uniform
// environment.  NEE cannot sample through a delta BSDF, so no env-NEE
// partner was suppressed at depth 0, and the depth-1 env hit is the SOLE
// estimator of the mirror's env reflection -- it must SURVIVE under
// `indirect`, exactly as the emitter case in scenario (e) does.
//
// Metric: PEAK luminance inside the mirror's own pixels, with the bbox
// rescaled into each render's resolution (BeautyVariant modes render at
// their own divisor -- a full-res objectmap bbox applied verbatim to a
// quarter-res render misses the image entirely).
//----------------------------------------------------------------------
static const char* const kSceneMirrorUnderEnv =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"uniformcolor_painter\n{\n\tname env_pnt\n\tcolor 0.9 0.9 0.9\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n\tradiance_map env_pnt\n\tradiance_scale 1.0\n\tradiance_background true\n}\n\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.95 0.95 0.95\n}\n\n"
	"perfectreflector_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 12\n\theight 0.2\n\tdepth 12\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -0.1 0\n}\n";

static void RunIndirectModeMirrorKeepsEnvReflectionTest()
{
	std::printf( "=== AgentViewModeRenderTest: \"indirect\" scenario (g), review-p3 -- a MIRROR under a bright env "
	             "KEEPS its env reflection (delta depth==0 => no suppressed env-NEE partner) ===\n" );
	const std::string scenePath = WriteTemp( "rise_indirect_mirror_env.RISEscene", kSceneMirrorUnderEnv );
	Check( !scenePath.empty(), "wrote the mirror-under-env scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "mirror-under-env scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "mirror-under-env session wraps" );
	if( !session ) { pJob->release(); return; }

	BBox floorBBox{ 0, 0, 0, 0, 0 };
	unsigned int objW = 0, objH = 0;
	{
		AgentRenderParams objP;
		objP.renderTarget = AgentRenderTarget::ObjectMap;
		AgentRenderResult objR = session->Render( objP );
		Check( objR.ok, "mirror-under-env reference objectmap render succeeds" );
		Decoded objDec;
		if( objR.ok && DecodePng( objR.png, objDec ) ) {
			objW = objDec.w; objH = objDec.h;
			const LegendEntry* floorLegend = FindLegend( objR, "floor_obj" );
			Check( floorLegend != nullptr, "objectmap legend carries floor_obj" );
			if( floorLegend ) {
				unsigned char cb[3];
				if( HexToBytes( floorLegend->colorHex, cb ) ) {
					floorBBox = ScanBBoxForColor( objDec, cb );
				}
			}
		}
	}
	Check( floorBBox.found > 0, "the mirror floor occupies a nonzero pixel region" );

	auto peakOnFloor = [&]( Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		return MaxLuminanceInBBox( dec, floorBBox, objW, objH );
	};

	const double deepReflectPeak = peakOnFloor( Implementation::ViewportRenderMode::DeepReflect );
	const double indirectPeak    = peakOnFloor( Implementation::ViewportRenderMode::Indirect );
	std::printf( "  FLOOR REGION PEAK (env): deep_reflect=%.2f indirect=%.2f\n", deepReflectPeak, indirectPeak );

	Check( deepReflectPeak > 15.0,
	       "deep_reflect: the mirror floor reflects the bright environment (sanity -- the fixture actually "
	       "shows an env reflection)" );
	Check( indirectPeak > deepReflectPeak * 0.5,
	       "MONEY ASSERTION (g): the mirror's ENV reflection SURVIVES under \"indirect\" -- a delta depth==0 "
	       "scatter suppresses no env-NEE partner, so the depth==1 env hit is the sole estimator and must be "
	       "kept.  This is the half of the MIS-partner rule scenario (f) cannot reach (its depth==0 vertex is "
	       "Lambertian, so the delta term never changes its outcome)." );

	pJob->release();
}

//----------------------------------------------------------------------
// review-p2c P2-b: MIS-partner-existence gating regressions for
// PathTracingIntegrator::SetIndirectOnly.
//
// Scenario (e): a mirror reflecting an area light.  The depth==0 scatter
// is DELTA (perfectreflector_material has no NEE-sampleable lobe -- Pdf()
// is zero/undefined for a delta BSDF, so NEE contributes nothing at that
// vertex either way).  With no suppressed NEE partner at depth==0, the
// depth==1 BSDF-sampled emission hit (the mirror showing the lamp) is the
// SOLE estimator of that specular-transport path and must survive under
// "indirect" -- suppressing it (the pre-fix raw `depth<=1` rule, with no
// delta check) deletes real energy.  Reuses kSceneClayFloorMirror (already
// validated for the P1-c clay/MIS material-independence test above): a
// perfect-mirror floor angled so the camera sees the floor reflecting a
// bright sphere lamp positioned directly above.
//----------------------------------------------------------------------
static void RunIndirectModeMirrorReflectsLightTest()
{
	std::printf( "=== AgentViewModeRenderTest: \"indirect\" scenario (e), review-p2c P2-b -- mirror reflecting an "
	             "area light STAYS LIT (delta depth==0 scatter has no suppressed NEE partner) ===\n" );
	const std::string scenePath = WriteTemp( "rise_indirect_mirror_light.RISEscene", kSceneClayFloorMirror );
	Check( !scenePath.empty(), "wrote the mirror-floor scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "mirror-floor scene loads via the CST path (indirect test)" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "mirror-floor session wraps (indirect test)" );
	if( !session ) { pJob->release(); return; }

	// Locate the MIRROR FLOOR's own pixels via a reference objectmap render.
	// Measuring the WHOLE FRAME here would repeat the "measure the thing,
	// not the frame" mistake the P1-b SSS test made: this fixture ALSO shows
	// the lamp sphere directly, and `indirect` correctly blacks that out (a
	// directly-visible emitter IS direct light), dragging a whole-frame mean
	// below any threshold calibrated as though the reflection were the only
	// content.  The claim under test is about the REFLECTION -- so measure
	// the reflecting surface.
	BBox floorBBox{ 0, 0, 0, 0, 0 };
	unsigned int objW = 0, objH = 0;
	{
		AgentRenderParams objP;
		objP.renderTarget = AgentRenderTarget::ObjectMap;
		AgentRenderResult objR = session->Render( objP );
		Check( objR.ok, "mirror-floor reference objectmap render succeeds" );
		Decoded objDec;
		if( objR.ok && DecodePng( objR.png, objDec ) ) {
			objW = objDec.w; objH = objDec.h;
			const LegendEntry* floorLegend = FindLegend( objR, "floor_obj" );
			Check( floorLegend != nullptr, "objectmap legend carries floor_obj" );
			if( floorLegend ) {
				unsigned char cb[3];
				if( HexToBytes( floorLegend->colorHex, cb ) ) {
					floorBBox = ScanBBoxForColor( objDec, cb );
				}
			}
		}
	}
	Check( floorBBox.found > 0, "the mirror floor occupies a nonzero pixel region" );

	auto peakLuminanceOnFloor = [&]( Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		return MaxLuminanceInBBox( dec, floorBBox, objW, objH );
	};

	const double deepReflectMean = peakLuminanceOnFloor( Implementation::ViewportRenderMode::DeepReflect );
	const double indirectMean    = peakLuminanceOnFloor( Implementation::ViewportRenderMode::Indirect );
	std::printf( "  FLOOR REGION PEAK: deep_reflect=%.2f indirect=%.2f\n", deepReflectMean, indirectMean );

	Check( deepReflectMean > 15.0,
	       "deep_reflect: the floor region PEAK is bright -- the mirror reflects the lamp (sanity: the fixture shows a "
	       "visible reflection)" );
	// MONEY ASSERTION (e): the mirror floor's brightness is ENTIRELY the
	// depth==1 BSDF-sampled emission hit reached through a DELTA depth==0
	// scatter -- there is no suppressed NEE partner at a delta vertex, so
	// this must survive under "indirect".  Pre-fix (raw depth<=1
	// suppression with no delta check), this reflection was unconditionally
	// zeroed under "indirect" regardless of the depth==0 scatter type --
	// verified by temporarily reverting the `!bPassedThroughSpecular` delta
	// check (i.e. restoring `depth <= 1` unconditional suppression) and
	// re-running: the floor-region PEAK drops 246.00 -> 55.33 (MEASURED;
	// an earlier note claimed "~0", which overstated it -- the assertion
	// still fails, since the threshold is 0.5 * 250.33 = 125).
	Check( indirectMean > deepReflectMean * 0.5,
	       "MONEY ASSERTION (e): the MIRROR FLOOR's PEAK stays bright under \"indirect\" -- the reflected-lamp highlight survives (the floor MEAN legitimately drops: the mirror also reflects a room whose direct lighting indirect removes). A delta depth==0 "
	       "scatter has no suppressed NEE partner, so its depth==1 BSDF-sampled emission hit is the sole "
	       "estimator and must survive (review-p2c P2-b)" );

	pJob->release();
}

//----------------------------------------------------------------------
// Scenario (f): a diffuse surface lit ONLY by a bright uniform environment
// (no other light source, no other geometry to generate genuine >=2-bounce
// GI).  The depth==0 scatter is NON-delta (a Lambertian lobe), so a real
// NEE partner exists -- BOTH halves of the direct-env estimator (env-NEE
// at depth==0 AND its depth==1 BSDF-sampled MIS partner, the continuation
// ray escaping to the env map) must be suppressed under "indirect".  This
// is the P2-b(ii) regression: the loop's own env-miss adds were reverted
// to UNCONDITIONAL in an earlier fix (P2b review), so pre-fix this scene
// still shows roughly half its direct-env brightness under "indirect" (the
// env-NEE half suppressed, the BSDF-sampled half leaking through).
//----------------------------------------------------------------------
static const char* const kSceneDiffuseUnderBrightEnv =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"uniformcolor_painter\n{\n\tname env_pnt\n\tcolor 3 3 3\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 24\n\tpixel_filter box\n\toidn_denoise false\n\tradiance_map env_pnt\n\tradiance_scale 1.0\n\tradiance_background false\n}\n\n"
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"box_geometry\n{\n\tname plane_geo\n\twidth 12\n\theight 12\n\tdepth 0.2\n}\n\n"
	"standard_object\n{\n\tname plane_obj\n\tgeometry plane_geo\n\tmaterial mat\n\tposition 0 0 0\n}\n";

static void RunIndirectModeDiffuseUnderEnvSuppressedTest()
{
	std::printf( "=== AgentViewModeRenderTest: \"indirect\" scenario (f), review-p2c P2-b -- diffuse surface under "
	             "a bright uniform env is NOT direct-env-lit (depth==1 env-hit MIS partner also suppressed) ===\n" );
	const std::string scenePath = WriteTemp( "rise_indirect_diffuse_env.RISEscene", kSceneDiffuseUnderBrightEnv );
	Check( !scenePath.empty(), "wrote the diffuse-under-env scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "diffuse-under-env scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "diffuse-under-env session wraps" );
	if( !session ) { pJob->release(); return; }

	auto meanLuminance = [&]( Implementation::ViewportRenderMode mode ) -> double
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = mode;
		AgentRenderResult r = session->Render( p );
		if( !r.ok ) { Check( false, "render succeeds" ); return -1.0; }
		Decoded dec;
		if( !DecodePng( r.png, dec ) ) { Check( false, "PNG decodes" ); return -1.0; }
		return MeanLuminance( dec );
	};

	const double deepReflectMean = meanLuminance( Implementation::ViewportRenderMode::DeepReflect );
	const double indirectMean    = meanLuminance( Implementation::ViewportRenderMode::Indirect );
	std::printf( "  deep_reflect=%.2f indirect=%.2f\n", deepReflectMean, indirectMean );

	Check( deepReflectMean > 40.0,
	       "deep_reflect: the plane reads brightly lit by the uniform env (sanity -- the fixture is actually lit)" );
	// MONEY ASSERTION (f): with no other light source and no other geometry
	// to generate genuine >=2-bounce GI, "indirect" must read NEAR-BLACK --
	// BOTH halves of the direct-env estimator are suppressed at a non-delta
	// depth==0 vertex.  Pre-fix, the env-miss add was UNCONDITIONAL --
	// verified by temporarily removing the P2-b `suppressIndirectEnv` gate
	// (restoring the unconditional add) and re-running: indirectMean reads
	// well above this threshold.
	Check( indirectMean >= 0.0 && indirectMean < 8.0,
	       "MONEY ASSERTION (f): \"indirect\" on a diffuse surface under a bright uniform env reads NEAR-BLACK -- "
	       "the depth==1 env-hit MIS partner of the suppressed depth==0 env-NEE is ALSO suppressed "
	       "(review-p2c P2-b)" );
	Check( indirectMean < deepReflectMean - 30.0,
	       "MONEY ASSERTION (f2): \"indirect\" is MEANINGFULLY below \"deep_reflect\" on the same fixture" );

	pJob->release();
}

int main()
{
	RunPerModeEndToEndTest();
	RunFilmRestoreTest();
	RunBeautyVariantEndToEndTest();
	RunDirectModeMissingIndirectBleedTest();
	RunIndirectModeNoIndirectPathTest();
	RunIndirectModeBsdfMisPartnerLeakTest();
	RunIndirectModeColorBleedSceneMeanTest();
	RunIndirectModeEnvBackgroundBlackTest();
	RunClayLightsAlbedoIndependenceTest();
	RunViewArgEndToEndTest();
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
	RunThinGlassNearOpaqueDiscriminationTest();
	RunDoubleSidedThinMeshNearOpaqueDiscriminationTest();
	RunBeautyVariantSSSDefaultShaderTest();
	RunClayLightsMaterialIndependenceMirrorTest();
	RunClayLightsSSSBypassTest();
	RunIndirectPrimaryMediumScatterTest();
	RunIndirectModeMirrorReflectsLightTest();
	RunIndirectModeMirrorKeepsEnvReflectionTest();
	RunIndirectModeDiffuseUnderEnvSuppressedTest();

	std::printf( "\nAgentViewModeRenderTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
