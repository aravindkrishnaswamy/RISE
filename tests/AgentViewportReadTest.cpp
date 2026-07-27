//////////////////////////////////////////////////////////////////////
//
//  AgentViewportReadTest.cpp - Toolkit slice 1: the `read_viewport`
//    verb (fetch the user's CURRENT live GUI viewport pixels with NO
//    re-render).
//
//    read_viewport is DISTINCT from read_image: read_image returns the
//    agent's OWN last headless render (its private, offscreen
//    FrameStore); read_viewport returns the USER's LIVE interactive
//    viewport -- the exact frame the interactive render loop most
//    recently produced into SceneEditController::mInteractiveFrameStore.
//    It never triggers a render (the cheapest observe).
//
//    This file is the FIRST test in the tree to construct a
//    SceneEditController over a REAL interactive rasterizer
//    (pJob->GetRasterizer()); every prior SceneEditController test
//    passed 0 for the rasterizer (the TestController skeleton path).  So
//    it also exercises the real DoOneRenderPass -> EnsureInteractiveFrameStore_
//    -> RasterizeScene path end-to-end for the first time.
//
//    Cases:
//      1. POSITIVE -- a real-rasterizer controller renders one pass;
//         CopyInteractiveFrame returns the frame directly (dims == film),
//         AND the read_viewport RPC verb returns available:true with a
//         non-empty PNG that decodes to \x89PNG at the film dims; a
//         maxEdge=16 downscale returns dims <= 16.
//      2. NO-CONTROLLER -- a session with no attached controller reports
//         available:false, reason:"no_controller".
//      3. NO-FRAME-YET -- a real-rasterizer controller that was never
//         Start()ed (so the interactive loop produced no frame) reports
//         available:false, reason:"no_frame_yet".
//      4. ISOLATION CROSS-CHECK (regression lock tying to Slice 0) --
//         after freezing the interactive viewport, an agent `render`
//         (which uses the PRIVATE offscreen FrameStore) leaves the
//         viewport pixels byte-for-byte UNCHANGED -- proving the agent
//         render never touched the interactive store.
//
//    Self-contained: an inline native-v7 scene (a lit sphere), OIDN off,
//    written to a scratch temp file -- no RISE_MEDIA_PATH, no scenes/
//    tree file touched.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Agent/Base64.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Utilities/Color/Color_Template.h"   // RISEColor
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IMemoryBuffer.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"

#include <map>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
// Round-8 review P2: per-process temp filenames -- see WriteTemp below.
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

static std::string WriteTemp( const char* name, const std::string& text )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	// Round-8 review P2: per-process filename.  run_all_tests.sh runs the
	// suite in PARALLEL, and a developer may run one binary by hand while
	// the suite runs, so a FIXED temp name lets two processes clobber each
	// other's scene file mid-load.  That already cost a reviewer hours and
	// produced a false "the test is flaky / there is a race" P1.  The pid
	// prefix makes the path unique per process.
	std::string path = dir + std::to_string( (long)getpid() ) + "_" + name;
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return std::string();
	f.write( text.data(), (std::streamsize)text.size() );
	f.close();
	return path;
}

// A small lit diffuse sphere at a tiny NON-SQUARE 32x24 film so a full
// render pass is fast AND the width/height assertions (and the aspect-
// preserving downscale) are meaningful -- a square film would hide a
// width<->height transposition bug.  PT, box filter, OIDN off, low
// sample count.  Mirrors the canonical shape used by
// AgentFrameStoreIsolationTest.
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 32\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// compare_to_reference split:true discriminating scenes -- the SAME sphere
// + area-light rig as kScene above, PLUS a hosek_wilkie_skylight (visible
// as the BACKGROUND on every camera ray that misses all geometry -- see
// RayCaster.cpp's GetGlobalRadianceMap() miss path -- but NOT a registered
// scene OBJECT, so it never appears in an objectmap legend).  A wider fov
// than kScene so the sphere leaves a meaningful amount of sky visible
// around it (a sane, non-0/non-1 objectPixelFraction ~= 0.12).
//
// The exact photometric values below are TUNED and load-bearing -- they
// are what make the BACKGROUND-only perturbation (kSplitSceneBgRecolor)
// actually read as a background-dominant change.  A skylight is a LIGHT,
// so any sky change also relights the sphere; whether that relighting
// out-measures the directly-viewed sky change is decided by where each
// region sits on the ACES tone curve, NOT by which is "bigger" in linear
// radiance.  Measured while building this test:
//   * DIM sphere (albedo 0.15/0.05/0.05) sits on the curve's steep TOE,
//     so sky spill swings its bytes hard -- objectRmse 0.26 vs
//     backgroundRmse 0.05, i.e. exactly backwards.
//   * BRIGHT sphere (albedo 0.85, area light scale 100) sits on the
//     compressed SHOULDER, so the same sky spill barely moves its bytes.
//   * DIM sky (sky_intensity_scale 0.05) keeps the background itself on
//     the steep toe, so its own change registers at full strength.
// Net: bright-object + dim-sky yields objectRmse 0.33 / backgroundRmse
// 0.02 for an object-only change and 0.07 / 0.16 for a background-only
// change -- clean dominance in BOTH directions.  If you retune these,
// re-measure both directions; raising the sphere's albedo/light or
// lowering the sky intensity is the axis that preserves the property.
static const char* const kSplitSceneBase =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 40\n\theight 30\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"hosek_wilkie_skylight\n{\n\tsolar_elevation 45.0\n\tsolar_azimuth 0.0\n\tturbidity 2.0\n\tsky_intensity_scale 0.05\n\tcreate_sun false\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.85 0.85 0.85\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 100.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// Identical to kSplitSceneBase EXCEPT for ONE parameter -- the sky's
// sky_intensity_scale (0.05 -> 0.35, a 7x brightening entirely inside the
// tone curve's steep toe).  Turbidity and the solar angles are held FIXED
// so the sky's angular structure is unchanged and the perturbation is a
// pure background-brightness change; the sphere's own material is
// UNTOUCHED.  Used to prove a BACKGROUND-only change is picked up by
// `backgroundRmse` and mostly not by `objectRmse`.  See kSplitSceneBase's
// comment for why the tone-curve placement (not the linear magnitude) is
// what makes this direction come out background-dominant.
static const char* const kSplitSceneBgRecolor =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 40\n\theight 30\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"hosek_wilkie_skylight\n{\n\tsolar_elevation 45.0\n\tsolar_azimuth 0.0\n\tturbidity 2.0\n\tsky_intensity_scale 0.35\n\tcreate_sun false\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.85 0.85 0.85\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 100.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// A TWO-VISIBLE-OBJECT variant used to prove the object mask unions EVERY
// registered legend entry, not just the first one.  Two equal spheres side
// by side (obj_a at x=-0.75, obj_b at x=+0.75; legend order is sorted by
// name, so obj_a is entry 0).  The discriminating test recolours ONLY
// obj_b's painter: if the mask were built from a single legend entry --
// legend[0], or the largest, or the first hit -- obj_b's pixels would fall
// into the BACKGROUND bucket and the change would show up as a
// backgroundRmse rise instead of an objectRmse rise.  Same bright-object /
// dim-sky tone-curve tuning as kSplitSceneBase (see its comment).
static const char* const kSplitSceneTwoObjects =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 40\n\theight 30\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"hosek_wilkie_skylight\n{\n\tsolar_elevation 45.0\n\tsolar_azimuth 0.0\n\tturbidity 2.0\n\tsky_intensity_scale 0.05\n\tcreate_sun false\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo_a\n\tcolor 0.85 0.85 0.85\n}\n\n"
	"lambertian_material\n{\n\tname mat_a\n\treflectance pnt_albedo_a\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo_b\n\tcolor 0.85 0.85 0.85\n}\n\n"
	"lambertian_material\n{\n\tname mat_b\n\treflectance pnt_albedo_b\n}\n\n"
	"sphere_geometry\n{\n\tname sph_small\n\tradius 0.5\n}\n\n"
	"standard_object\n{\n\tname obj_a\n\tgeometry sph_small\n\tmaterial mat_a\n\tposition -0.75 0 0\n}\n\n"
	"standard_object\n{\n\tname obj_b\n\tgeometry sph_small\n\tmaterial mat_b\n\tposition 0.75 0 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 100.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

static JsonValue ParseResponse( const std::string& line, double expectId )
{
	JsonValue env; std::string err;
	const bool ok = JsonParse( line, env, err );
	Check( ok, "response line parses as JSON (" + ( ok ? std::string("ok") : err ) + ")" );
	if( !ok ) return JsonValue::MakeNull();
	Check( env.get( "id" ).asNumber( -999 ) == expectId, "response echoes the request id" );
	return env;
}

static std::string Req( double id, const std::string& method, const JsonValue& params )
{
	JsonValue r = JsonValue::MakeObject();
	r.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
	r.set( "id", JsonValue::MakeNumber( id ) );
	r.set( "method", JsonValue::MakeString( method ) );
	r.set( "params", params );
	return JsonSerialize( r );
}

// Decoded PNG bytes start with the 8-byte PNG signature 89 50 4E 47.
static bool StartsWithPngSignature( const std::vector<unsigned char>& b )
{
	return b.size() >= 4 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
}

//////////////////////////////////////////////////////////////////////
// Case 1 (POSITIVE) + Case 4 (ISOLATION): share one Started controller.
//////////////////////////////////////////////////////////////////////
static void RunPositiveAndIsolation()
{
	std::printf( "=== read_viewport: POSITIVE + ISOLATION (real interactive rasterizer) ===\n" );

	const std::string scenePath = WriteTemp( "agent_viewport_positive.RISEscene", kScene );
	Check( !scenePath.empty(), "positive: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "positive: scene loads via the CST path" );

	const IScenePriv* scenePriv = pJob->GetScene();
	const IFilm* film = scenePriv ? scenePriv->GetFilm() : nullptr;
	const unsigned int filmW = film ? film->GetWidth()  : 0;
	const unsigned int filmH = film ? film->GetHeight() : 0;
	Check( filmW == 32 && filmH == 24, "positive: authored film is 32x24 (non-square)" );

	{
		IRasterizer* rast = pJob->GetRasterizer();
		Check( rast != nullptr, "positive: Job has an active rasterizer" );

		// FIRST test in the tree to drive a real interactive rasterizer
		// (every prior SceneEditController test passed 0).  If
		// EnsureInteractiveFrameStore_ / DoOneRenderPass has a latent bug on
		// the real path, ForTest_WaitForRenders below would time out.
		SceneEditController controller( *pJob, rast );
		controller.Start();
		Check( controller.ForTest_WaitForRenders( 1, 5000 ),
			"positive: interactive render loop completed at least one pass (real-rasterizer path works)" );

		// ---- Direct CopyInteractiveFrame ---------------------------------
		{
			std::vector<RISEColor> pixels;
			unsigned int w = 0, h = 0;
			const bool got = controller.CopyInteractiveFrame( pixels, w, h );
			Check( got, "positive: CopyInteractiveFrame returns true after a render" );
			Check( w == filmW && h == filmH, "positive: CopyInteractiveFrame dims match the film (32x24)" );
			Check( pixels.size() == static_cast<std::size_t>( w ) * h && !pixels.empty(),
				"positive: CopyInteractiveFrame filled width*height pixels" );
		}

		// ---- Through the read_viewport RPC verb --------------------------
		std::vector<unsigned char> viewportPngNative;   // captured for the isolation cross-check
		{
			std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
			Check( session != nullptr, "positive: AgentSession::WrapJob wraps the Job" );
			session->AttachController( &controller );
			Check( session->HasController(), "positive: controller attached" );

			AgentRpcDispatcher rpc( std::move( session ) );

			// Native-resolution read_viewport.
			{
				const std::string resp = rpc.HandleLine( Req( 1, "read_viewport", JsonValue::MakeObject() ) );
				JsonValue env = ParseResponse( resp, 1 );
				Check( env.has( "result" ), "positive: read_viewport is a structured success (no JSON-RPC error)" );
				const JsonValue& r = env.get( "result" );
				Check( r.get( "available" ).asBool() == true, "positive: read_viewport available == true" );
				Check( r.get( "reason" ).asString().empty(), "positive: read_viewport reason is empty on success" );
				Check( static_cast<unsigned int>( r.get( "width"  ).asNumber() ) == filmW &&
				       static_cast<unsigned int>( r.get( "height" ).asNumber() ) == filmH,
					"positive: read_viewport reports the native 32x24 dims" );
				const std::string b64 = r.get( "png_base64" ).asString();
				Check( !b64.empty(), "positive: read_viewport png_base64 is non-empty" );
				Check( static_cast<std::size_t>( r.get( "byteLength" ).asNumber() ) > 0,
					"positive: read_viewport byteLength > 0" );
				std::vector<unsigned char> decoded;
				Check( Base64Decode( b64, decoded ), "positive: read_viewport png_base64 decodes cleanly" );
				Check( StartsWithPngSignature( decoded ), "positive: decoded bytes start with the \\x89PNG signature" );
				viewportPngNative = decoded;

				// P3c (§7.8 ratified decision 3): the pane-set introspection.
				// Single layout, controller attached: layout 0, primary 0,
				// sourcePane 0, pane 0 visible in "preview" tracking the
				// scene camera, panes 1-3 present but not visible.  The
				// pane set is READ-ONLY by ratified decision -- there is no
				// setter to test on this surface, deliberately.
				Check( r.has( "paneSet" ), "P3c: read_viewport carries paneSet when a controller is attached" );
				if( r.has( "paneSet" ) )
				{
					const JsonValue& ps = r.get( "paneSet" );
					Check( static_cast<int>( ps.get( "layout" ).asNumber() ) == 0, "P3c: layout is Single (0)" );
					Check( static_cast<unsigned int>( ps.get( "primary" ).asNumber() ) == 0, "P3c: primary is pane 0" );
					Check( static_cast<unsigned int>( ps.get( "sourcePane" ).asNumber() ) == 0,
					       "P3c: sourcePane is 0 -- the PNG is attributably pane 0's content in Single layout" );
					const JsonValue& arr = ps.get( "panes" );
					Check( arr.size() == 4, "P3c: all four pane slots are reported" );
					if( arr.size() == 4 )
					{
						Check( arr.at( 0 ).get( "visible" ).asBool() == true,  "P3c: pane 0 visible" );
						Check( arr.at( 0 ).get( "mode" ).asString() == "preview", "P3c: pane 0 mode is preview" );
						Check( arr.at( 0 ).get( "vantageKind" ).asNumber() == 0, "P3c: pane 0 tracks the scene camera" );
						Check( arr.at( 1 ).get( "visible" ).asBool() == false, "P3c: pane 1 hidden in Single" );
						Check( arr.at( 3 ).get( "visible" ).asBool() == false, "P3c: pane 3 hidden in Single" );
					}
				}
			}

			// T2: pane introspection carries additive kind 3 plus the exact
			// manager-camera reference.  This is desired-state metadata and is
			// available atomically even before the scheduled pane pass lands.
			Check( controller.SetViewportLayout(
					SceneEditController::ViewportLayout::TwoH ),
				"T2 paneSet: layout TwoH" );
			Check( controller.SetPaneVantageSceneCameraNamed( 1, "cam" ),
				"T2 paneSet: pane 1 binds scene camera cam" );
			Check( controller.SetPaneContentSource(
					1, SceneEditController::PaneContentSource::LastRender ),
				"T4 paneSet: pane 1 selects Last Render" );
			{
				const std::string resp = rpc.HandleLine(
					Req( 6, "read_viewport", JsonValue::MakeObject() ) );
				JsonValue env = ParseResponse( resp, 6 );
				const JsonValue& paneSet = env.get( "result" ).get( "paneSet" );
				const JsonValue& panes = paneSet.get( "panes" );
				Check( panes.size() == 4
				    && panes.at( 1 ).get( "visible" ).asBool()
				    && panes.at( 1 ).get( "contentSource" ).asNumber() == 1
				    && panes.at( 1 ).get( "vantageKind" ).asNumber() == 3
				    && panes.at( 1 ).get( "namedView" ).asString() == "cam",
					"MONEY ASSERTION T2/T4: read_viewport serializes LastRender "
					"orthogonally to kind 3 and the exact named scene-camera reference" );
			}

			// Downscaled read_viewport (maxEdge = 16 -> dims <= 16).
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "maxEdge", JsonValue::MakeNumber( 16.0 ) );
				const std::string resp = rpc.HandleLine( Req( 2, "read_viewport", params ) );
				JsonValue env = ParseResponse( resp, 2 );
				const JsonValue& r = env.get( "result" );
				Check( r.get( "available" ).asBool() == true, "positive(maxEdge=16): available == true" );
				const unsigned int dw = static_cast<unsigned int>( r.get( "width"  ).asNumber() );
				const unsigned int dh = static_cast<unsigned int>( r.get( "height" ).asNumber() );
				Check( dw <= 16 && dh <= 16 && dw > 0 && dh > 0,
					"positive(maxEdge=16): downscaled dims are within [1,16]" );
				const std::string b64 = r.get( "png_base64" ).asString();
				std::vector<unsigned char> decoded;
				Check( !b64.empty() && Base64Decode( b64, decoded ) && StartsWithPngSignature( decoded ),
					"positive(maxEdge=16): downscaled png decodes to a \\x89PNG" );
			}

			// ---- Case 4: ISOLATION CROSS-CHECK ---------------------------
			// Freeze the interactive viewport (stop only the interactive
			// loop; the agent-render worker stays live) so the viewport
			// content cannot drift between the two reads for reasons OTHER
			// than the agent render.  Then run an agent `render` (private
			// offscreen FrameStore) and confirm the viewport is byte-for-
			// byte identical -- the agent render never touched the
			// interactive store (Slice-0 offscreen-isolation regression
			// lock).
			controller.StopInteractive();

			std::vector<unsigned char> before;
			{
				const std::string resp = rpc.HandleLine( Req( 3, "read_viewport", JsonValue::MakeObject() ) );
				JsonValue env = ParseResponse( resp, 3 );
				const JsonValue& r = env.get( "result" );
				Check( r.get( "available" ).asBool() == true, "isolation: viewport still available after StopInteractive" );
				const std::string b64 = r.get( "png_base64" ).asString();
				Check( Base64Decode( b64, before ) && !before.empty(),
					"isolation: captured the frozen viewport PNG (before the agent render)" );
			}

			// Agent render -- routes through the controller's agent-render
			// worker into a PRIVATE offscreen FrameStore.
			{
				const std::string resp = rpc.HandleLine( Req( 4, "render", JsonValue::MakeObject() ) );
				JsonValue env = ParseResponse( resp, 4 );
				Check( env.get( "result" ).get( "ok" ).asBool() == true,
					"isolation: the intervening agent render succeeded" );
			}

			std::vector<unsigned char> after;
			{
				const std::string resp = rpc.HandleLine( Req( 5, "read_viewport", JsonValue::MakeObject() ) );
				JsonValue env = ParseResponse( resp, 5 );
				const std::string b64 = env.get( "result" ).get( "png_base64" ).asString();
				Check( Base64Decode( b64, after ) && !after.empty(),
					"isolation: captured the viewport PNG (after the agent render)" );
			}

			Check( before == after,
				"isolation: viewport pixels are byte-for-byte UNCHANGED across the agent render (agent used the private store)" );

			// rpc + session destruct here (controller still alive).
		}

		controller.Stop();   // idempotent w.r.t. the earlier StopInteractive
		// controller destructs at end of this scope (Stop again = no-op).
	}

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// Case 2: NO-CONTROLLER -> available:false, reason:"no_controller".
//////////////////////////////////////////////////////////////////////
static void RunNoController()
{
	std::printf( "=== read_viewport: NO-CONTROLLER ===\n" );

	const std::string scenePath = WriteTemp( "agent_viewport_nocontroller.RISEscene", kScene );
	Check( !scenePath.empty(), "no-controller: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "no-controller: scene loads via the CST path" );

	{
		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "no-controller: AgentSession::WrapJob wraps the Job" );
		Check( !session->HasController(), "no-controller: session has NO controller attached" );
		AgentRpcDispatcher rpc( std::move( session ) );

		const std::string resp = rpc.HandleLine( Req( 1, "read_viewport", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 1 );
		Check( env.has( "result" ), "no-controller: read_viewport is a structured success (NOT a JSON-RPC error)" );
		const JsonValue& r = env.get( "result" );
		Check( r.get( "available" ).asBool() == false, "no-controller: available == false" );
		Check( r.get( "reason" ).asString() == "no_controller", "no-controller: reason == \"no_controller\"" );
		Check( r.get( "png_base64" ).asString().empty(), "no-controller: png_base64 is empty" );
		Check( static_cast<int>( r.get( "byteLength" ).asNumber() ) == 0 &&
		       static_cast<int>( r.get( "width" ).asNumber() ) == 0 &&
		       static_cast<int>( r.get( "height" ).asNumber() ) == 0,
			"no-controller: byteLength/width/height are all 0" );
	}

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// Case 3: NO-FRAME-YET -> available:false, reason:"no_frame_yet".
//   A real-rasterizer controller that was NEVER Start()ed: the
//   interactive render loop never ran, so mInteractiveFrameStore is
//   still null.
//////////////////////////////////////////////////////////////////////
static void RunNoFrameYet()
{
	std::printf( "=== read_viewport: NO-FRAME-YET ===\n" );

	const std::string scenePath = WriteTemp( "agent_viewport_noframe.RISEscene", kScene );
	Check( !scenePath.empty(), "no-frame: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "no-frame: scene loads via the CST path" );

	{
		IRasterizer* rast = pJob->GetRasterizer();
		SceneEditController controller( *pJob, rast );
		// Deliberately do NOT Start() -- no interactive frame is ever
		// produced, so CopyInteractiveFrame must return false.

		// Direct: CopyInteractiveFrame returns false with empty outputs.
		{
			std::vector<RISEColor> pixels;
			unsigned int w = 99, h = 99;
			const bool got = controller.CopyInteractiveFrame( pixels, w, h );
			Check( !got, "no-frame: CopyInteractiveFrame returns false when no render has run" );
			Check( w == 0 && h == 0 && pixels.empty(), "no-frame: CopyInteractiveFrame leaves outputs empty/zero" );
		}

		// Through the verb: available:false, reason:"no_frame_yet".
		{
			std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
			session->AttachController( &controller );
			AgentRpcDispatcher rpc( std::move( session ) );

			const std::string resp = rpc.HandleLine( Req( 1, "read_viewport", JsonValue::MakeObject() ) );
			JsonValue env = ParseResponse( resp, 1 );
			Check( env.has( "result" ), "no-frame: read_viewport is a structured success" );
			const JsonValue& r = env.get( "result" );
			Check( r.get( "available" ).asBool() == false, "no-frame: available == false" );
			Check( r.get( "reason" ).asString() == "no_frame_yet", "no-frame: reason == \"no_frame_yet\"" );
			Check( r.get( "png_base64" ).asString().empty(), "no-frame: png_base64 is empty" );
			// The pane set is controller state, not image state.  It must remain
			// available while the first interactive pass is pending; otherwise an
			// agent cannot reason about the user's N-up layout before any pixels
			// have arrived.
			Check( r.has( "paneSet" ), "no-frame: paneSet remains available before the first frame" );
			if( r.has( "paneSet" ) ) {
				const JsonValue& ps = r.get( "paneSet" );
				Check( static_cast<int>( ps.get( "layout" ).asNumber() ) == 0,
				       "no-frame: paneSet reports Single layout" );
				Check( static_cast<unsigned int>( ps.get( "primary" ).asNumber() ) == 0,
				       "no-frame: paneSet reports pane 0 as primary" );
				const JsonValue& panes = ps.get( "panes" );
				Check( panes.size() == 4 && panes.at( 0 ).get( "visible" ).asBool()
				       && !panes.at( 1 ).get( "visible" ).asBool(),
				       "no-frame: paneSet preserves Single visibility" );
			}
		}

		controller.Stop();   // retire the (never-started-interactive) agent worker + threads
	}

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// P3c atomicity regression: read_viewport must describe the SAME pane set
// captured with its copied frame, even when the live layout changes after the
// parked copy releases.  The hook deliberately creates that otherwise-tiny
// post-park window deterministically; it made the old getter-based response
// report Single while returning pane 1's TwoH frame.
//////////////////////////////////////////////////////////////////////
static void RunPaneSetSnapshotAtomicity()
{
	std::printf( "=== read_viewport: pane-set snapshot is atomic with frame ===\n" );

	const std::string scenePath = WriteTemp( "agent_viewport_paneset_atomic.RISEscene", kScene );
	Check( !scenePath.empty(), "pane-set atomicity: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "pane-set atomicity: scene loads via CST" );
	{
		IRasterizer* rast = pJob->GetRasterizer();
		Check( rast != nullptr, "pane-set atomicity: Job has an active rasterizer" );
		SceneEditController controller( *pJob, rast );
		Check( controller.SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		       "pane-set atomicity: selects TwoH before rendering" );
		controller.Start();
		Check( controller.ForTest_WaitForRenders( 2, 5000 ),
		       "pane-set atomicity: both initial TwoH panes render" );
		const unsigned int beforeModeChange = controller.ForTest_GetRenderCount();
		Check( controller.SetPaneRenderMode( 1, "wireframe" ),
		       "pane-set atomicity: pane 1 switches to wireframe" );
		Check( controller.ForTest_WaitForRenders( beforeModeChange + 1, 5000 ),
		       "pane-set atomicity: pane 1's wireframe frame arrives" );

		// user-review P2 (test TOCTOU): STOP the interactive loop FIRST, then
		// verify the frozen frame -- otherwise a scheduler pass could complete
		// between the CopyInteractiveFrame check and StopInteractive, flipping
		// mInteractiveFrameStorePane to pane 0 and spuriously failing the read.
		controller.StopInteractive();   // freeze the exact frame for the RPC read
		std::vector<RISEColor> currentPixels;
		unsigned int currentW = 0, currentH = 0, currentPane = 99;
		Check( controller.CopyInteractiveFrame( currentPixels, currentW, currentH, &currentPane )
		       && currentPane == 1,
		       "pane-set atomicity: the frozen live frame is pane 1 before the read" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "pane-set atomicity: AgentSession wraps the Job" );
		session->AttachController( &controller );
		bool hookRan = false;
		session->ForTest_SetReadViewportAfterParkHook( [&]() {
			hookRan = controller.SetViewportLayout( SceneEditController::ViewportLayout::Single );
		} );
		AgentRpcDispatcher rpc( std::move( session ) );

		const std::string resp = rpc.HandleLine( Req( 1, "read_viewport", JsonValue::MakeObject() ) );
		JsonValue env = ParseResponse( resp, 1 );
		Check( hookRan && controller.GetViewportLayout() == SceneEditController::ViewportLayout::Single,
		       "pane-set atomicity: hook changed the live controller to Single after the parked copy" );
		Check( env.has( "result" ), "pane-set atomicity: read_viewport remains a structured success" );
		const JsonValue& r = env.get( "result" );
		Check( r.get( "available" ).asBool(), "pane-set atomicity: copied frame remains available" );
		Check( r.has( "paneSet" ), "pane-set atomicity: response includes the parked pane set" );
		if( r.has( "paneSet" ) ) {
			const JsonValue& ps = r.get( "paneSet" );
			Check( static_cast<int>( ps.get( "layout" ).asNumber() )
			           == static_cast<int>( SceneEditController::ViewportLayout::TwoH ),
			       "MONEY: response retains the frame's TwoH layout, not post-park Single" );
			Check( static_cast<unsigned int>( ps.get( "sourcePane" ).asNumber() ) == 1,
			       "MONEY: response attributes the PNG to the parked pane-1 frame" );
			const JsonValue& panes = ps.get( "panes" );
			Check( panes.size() == 4 && panes.at( 1 ).get( "visible" ).asBool()
			       && panes.at( 1 ).get( "mode" ).asString() == "wireframe",
			       "MONEY: pane 1 remains visible and wireframe in the response snapshot" );
		}

		controller.Stop();
	}

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// P2: ResolveBeautyDisplayTransform_ must scan ALL declared
//   file_rasterizeroutput chunks and adopt the FIRST **LDR** one's
//   declared curve+exposure, not just the first output of any kind.
//   Before the fix, a scene declaring an HDR output FIRST and an LDR
//   output with `display_transform none` SECOND wrongly fell back to
//   the ACES default (the HDR chunk's unconditional `break`) instead of
//   honouring the LDR chunk it never looked past the HDR one to find.
//   Exercised directly via the ForTest_ResolveBeautyDisplayTransform
//   hook (added alongside this fix) rather than round-tripping through
//   a full render + PNG decode -- the RESOLVED (exposureEV,
//   displayTransform) pair is exactly what the bug and the fix are
//   about.
//////////////////////////////////////////////////////////////////////

// DISPLAY_TRANSFORM enum values, mirrored here (kept as plain ints in
// ResolveBeautyDisplayTransform_'s signature to keep AgentSession.h off
// the DisplayTransform.h dependency -- see its doc).
static const int kDT_None  = 0;
static const int kDT_ACES  = 2;

static void RunDisplayTransformOrdering()
{
	std::printf( "=== ResolveBeautyDisplayTransform_: HDR-first / LDR-second ordering (P2) ===\n" );

	// (1) THE BUG CASE: an HDR (EXR) output declared FIRST, an LDR (PNG)
	// output with an EXPLICIT `display_transform none` declared SECOND.
	// Pre-fix this resolved to the ACES default (kDT_ACES); post-fix it
	// must resolve to the LDR output's own declared "none" (kDT_None).
	{
		const std::string scene =
			std::string( kScene ) +
			"\nfile_rasterizeroutput\n{\n\tpattern out.exr\n\ttype EXR\n\tcolor_space Rec709RGB_Linear\n}\n\n"
			"file_rasterizeroutput\n{\n\tpattern out.png\n\ttype PNG\n\tdisplay_transform none\n}\n";
		const std::string scenePath = WriteTemp( "agent_viewport_dt_hdr_first.RISEscene", scene );
		Check( !scenePath.empty(), "HDR-first: scratch scene file written" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "HDR-first: scene loads via the CST path" );
		{
			std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
			Check( session != nullptr, "HDR-first: AgentSession::WrapJob wraps the Job" );

			double exposureEV = -999.0;
			int    dt         = -999;
			int    cs         = -999;
			session->ForTest_ResolveBeautyDisplayTransform( exposureEV, dt, cs );
			Check( dt == kDT_None,
				"HDR-first: resolves to the LATER LDR output's \"none\" transform, "
				"not the ACES default the FIRST (HDR) output would wrongly trigger "
				"(got " + std::to_string( dt ) + ", want " + std::to_string( kDT_None ) + ")" );
			Check( exposureEV == 0.0, "HDR-first: exposureEV is 0 (no camera EV, no declared exposure)" );
			// External review P2 fix: the LDR PNG output declares no
			// `color_space`, so the resolved colour space must be the
			// descriptor's default (sRGB == 0), not the HDR output's
			// Rec709RGB_Linear (== 1) it should have skipped over.
			Check( cs == 0, "HDR-first: color_space defaults to sRGB (got " + std::to_string( cs ) + ")" );
		}
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	// (2) CONTROL, reversed order: the SAME two chunks, LDR declared
	// FIRST this time.  Must resolve identically to (1) -- proving the
	// fix is genuinely "first LDR wins", not merely "last chunk wins" or
	// some other accidental reordering.
	{
		const std::string scene =
			std::string( kScene ) +
			"\nfile_rasterizeroutput\n{\n\tpattern out.png\n\ttype PNG\n\tdisplay_transform none\n}\n\n"
			"file_rasterizeroutput\n{\n\tpattern out.exr\n\ttype EXR\n\tcolor_space Rec709RGB_Linear\n}\n";
		const std::string scenePath = WriteTemp( "agent_viewport_dt_ldr_first.RISEscene", scene );
		Check( !scenePath.empty(), "LDR-first: scratch scene file written" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "LDR-first: scene loads via the CST path" );
		{
			std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
			double exposureEV = -999.0;
			int    dt         = -999;
			int    cs         = -999;
			session->ForTest_ResolveBeautyDisplayTransform( exposureEV, dt, cs );
			Check( dt == kDT_None, "LDR-first: resolves to \"none\" (the LDR output's own declaration)" );
			Check( exposureEV == 0.0, "LDR-first: exposureEV is 0" );
			Check( cs == 0, "LDR-first: color_space defaults to sRGB (got " + std::to_string( cs ) + ")" );
		}
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	// (3) CONTROL, HDR-only: no LDR output exists anywhere -- must fall
	// back to the ACES/0EV default (the pre-loop initial values), never
	// crash or silently adopt the HDR chunk's own (ignored) settings.
	{
		const std::string scene =
			std::string( kScene ) +
			"\nfile_rasterizeroutput\n{\n\tpattern out.exr\n\ttype EXR\n\tcolor_space Rec709RGB_Linear\n}\n";
		const std::string scenePath = WriteTemp( "agent_viewport_dt_hdr_only.RISEscene", scene );
		Check( !scenePath.empty(), "HDR-only: scratch scene file written" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "HDR-only: scene loads via the CST path" );
		{
			std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
			double exposureEV = -999.0;
			int    dt         = -999;
			int    cs         = -999;
			session->ForTest_ResolveBeautyDisplayTransform( exposureEV, dt, cs );
			Check( dt == kDT_ACES, "HDR-only: no LDR output anywhere -> falls back to the ACES default" );
			Check( exposureEV == 0.0, "HDR-only: exposureEV falls back to 0" );
			Check( cs == 0, "HDR-only: color_space falls back to sRGB (got " + std::to_string( cs ) + ")" );
		}
		pJob->release();
		std::remove( scenePath.c_str() );
	}
}

//////////////////////////////////////////////////////////////////////
// compare_to_reference: the reconstruction feedback instrument.
//
//   1. NOISE FLOOR (draft): register a draft-rendered PNG as "view1",
//      then compare_to_reference (default -- draft) against the SAME,
//      unchanged scene -> rmse near 0 (measures the draft-vs-draft MC
//      noise floor; draft's fixed studio-preview shading is far more
//      deterministic than a stochastic PT render, so this floor is
//      expected to be tiny).
//   2. UNKNOWN REFERENCE -> a JSON-RPC error naming the registered set.
//   3. CAMERA OVERRIDE aimed away from the sphere raises the rmse well
//      above the noise floor.
//   4. VISUAL: visual:true (the default) returns a composite PNG 3x the
//      reference's width; visual:false returns none.
//   5. PRODUCTION + RECOLOR: register a PRODUCTION reference, confirm a
//      same-scene production self-compare rmse near ITS OWN (noisier)
//      floor, then recolor the sphere's material and confirm the rmse
//      rises far beyond that floor with channelDelta signs pointing in
//      the recolor's direction (production quality is REQUIRED for this
//      -- draft ignores materials/lighting entirely, so a recolor is
//      invisible to a draft-mode comparison by design).
//////////////////////////////////////////////////////////////////////
static void RunCompareToReference()
{
	std::printf( "=== compare_to_reference: the reconstruction feedback instrument ===\n" );

	const std::string scenePath = WriteTemp( "agent_compare_to_reference.RISEscene", kScene );
	Check( !scenePath.empty(), "compare_to_reference: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "compare_to_reference: scene loads via the CST path" );

	{
		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "compare_to_reference: AgentSession::WrapJob wraps the Job" );

		AgentRpcDispatcher rpc( std::move( session ) );
		AgentSession* s = rpc.Session();
		Check( s != nullptr, "compare_to_reference: dispatcher exposes the wrapped session" );

		// ---- (1) register a DRAFT reference + same-scene draft self-compare (noise floor) ----
		double draftNoiseFloor = 0.0;
		{
			AgentRenderParams rp;
			rp.quality = AgentRenderQuality::Draft;
			const AgentRenderResult rr = s->Render( rp );
			Check( rr.ok && !rr.png.empty(), "compare_to_reference: draft reference render succeeded" );

			std::vector<AgentReferenceImage> refs( 1 );
			refs[0].name = "view1";
			refs[0].pngBytes.assign( rr.png.begin(), rr.png.end() );
			s->SetReferenceImages( std::move( refs ) );
		}

		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "view1" ) );
			const std::string resp = rpc.HandleLine( Req( 1, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 1 );
			Check( env.has( "result" ), "compare_to_reference(view1, draft): structured success" );
			const JsonValue& r = env.get( "result" );
			draftNoiseFloor = r.get( "rmse" ).asNumber();
			std::printf( "  measured draft-vs-draft self-compare rmse = %.6f\n", draftNoiseFloor );
			// Measured empirically at 0.000000 on this scene (the draft
			// preview pipeline's fixed studio shading has no stochastic
			// sampling to disagree with itself run-to-run, unlike a real PT
			// render) -- 0.01 is a generous margin above that measured
			// floor, honest rather than a round guess.
			Check( draftNoiseFloor < 0.01,
				"compare_to_reference: same-scene draft self-compare rmse is near 0 (< 0.01 draft-vs-draft MC noise; measured 0.000000)" );
			Check( static_cast<unsigned int>( r.get( "width"  ).asNumber() ) == 32 &&
			       static_cast<unsigned int>( r.get( "height" ).asNumber() ) == 24,
				"compare_to_reference: reports the reference's 32x24 dims" );
			Check( r.get( "reference" ).asString() == "view1", "compare_to_reference: echoes the reference name" );
			Check( r.get( "grid" ).isArray() && r.get( "grid" ).size() == 9,
				"compare_to_reference: grid has 9 cells" );
			Check( !r.get( "worstCell" ).asString().empty(), "compare_to_reference: worstCell is non-empty" );
			Check( !r.get( "summary" ).asString().empty(), "compare_to_reference: summary is non-empty" );
		}

		// ---- (2) UNKNOWN REFERENCE ----
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "no_such_view" ) );
			const std::string resp = rpc.HandleLine( Req( 2, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 2 );
			Check( env.has( "error" ), "compare_to_reference(unknown name): a JSON-RPC error" );
			Check( env.get( "error" ).get( "code" ).asNumber() == -32602.0,
				"compare_to_reference(unknown name): -32602 (bad reference is a usage error)" );
			const std::string msg = env.get( "error" ).get( "message" ).asString();
			Check( msg.find( "view1" ) != std::string::npos,
				"compare_to_reference(unknown name): error message names the registered reference(s)" );
		}

		// ---- (3) CAMERA OVERRIDE raises the rmse ----
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "view1" ) );
			JsonValue camera = JsonValue::MakeObject();
			camera.set( "location", JsonValue::MakeString( "0 0 -3.5" ) );
			camera.set( "lookat",   JsonValue::MakeString( "0 0 -10" ) );
			camera.set( "up",       JsonValue::MakeString( "0 1 0" ) );
			params.set( "camera", camera );
			const std::string resp = rpc.HandleLine( Req( 3, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 3 );
			Check( env.has( "result" ), "compare_to_reference(camera override): structured success" );
			const double rmseAway = env.get( "result" ).get( "rmse" ).asNumber();
			std::printf( "  measured rmse looking away from the sphere = %.6f\n", rmseAway );
			Check( rmseAway > draftNoiseFloor + 0.02,
				"compare_to_reference: an aimed-away camera override raises the rmse well above the noise floor" );
		}

		// ---- (4) VISUAL true (default) / false ----
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "view1" ) );
			const std::string resp = rpc.HandleLine( Req( 4, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 4 );
			const JsonValue& r = env.get( "result" );
			const std::string b64 = r.get( "png_base64" ).asString();
			Check( !b64.empty(), "compare_to_reference(visual default true): png_base64 is non-empty" );
			std::vector<unsigned char> decoded;
			Check( Base64Decode( b64, decoded ) && StartsWithPngSignature( decoded ),
				"compare_to_reference(visual default true): decodes to a \\x89PNG" );
			Check( static_cast<unsigned int>( r.get( "compositeWidth" ).asNumber() ) == 32u * 3,
				"compare_to_reference(visual default true): compositeWidth == 3x the reference width" );
			Check( static_cast<unsigned int>( r.get( "compositeHeight" ).asNumber() ) == 24u,
				"compare_to_reference(visual default true): compositeHeight == the reference height" );
		}
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "view1" ) );
			params.set( "visual", JsonValue::MakeBool( false ) );
			const std::string resp = rpc.HandleLine( Req( 5, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 5 );
			const JsonValue& r = env.get( "result" );
			Check( r.get( "png_base64" ).asString().empty(),
				"compare_to_reference(visual:false): no composite image returned" );
		}

		// ---- (5) PRODUCTION self-compare noise floor + recolor ----
		double productionNoiseFloor = 0.0;
		{
			AgentRenderParams rp;
			rp.samples = 16;
			const AgentRenderResult rr = s->Render( rp );
			Check( rr.ok && !rr.png.empty(), "compare_to_reference: production reference render succeeded" );

			std::vector<AgentReferenceImage> refs( 1 );
			refs[0].name = "view2";
			refs[0].pngBytes.assign( rr.png.begin(), rr.png.end() );
			s->SetReferenceImages( std::move( refs ) );   // REPLACES the set -- "view1" no longer registered
		}
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "view2" ) );
			params.set( "samples", JsonValue::MakeNumber( 16.0 ) );
			const std::string resp = rpc.HandleLine( Req( 6, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 6 );
			Check( env.has( "result" ), "compare_to_reference(view2, production): structured success" );
			productionNoiseFloor = env.get( "result" ).get( "rmse" ).asNumber();
			// Measured empirically ~0.0053 at samples=16 on this scene
			// (real PT sampling noise, unlike draft's 0.0 floor above).
			std::printf( "  measured production-vs-production self-compare rmse = %.6f\n", productionNoiseFloor );

			// "view1" was superseded by the SetReferenceImages replace above.
			JsonValue oldRef = JsonValue::MakeObject();
			oldRef.set( "reference", JsonValue::MakeString( "view1" ) );
			const std::string oldResp = rpc.HandleLine( Req( 60, "compare_to_reference", oldRef ) );
			JsonValue oldEnv = ParseResponse( oldResp, 60 );
			Check( oldEnv.has( "error" ),
				"compare_to_reference: SetReferenceImages REPLACES the set -- the superseded \"view1\" name is now unknown" );
		}

		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "target", JsonValue::MakeString( "pnt_albedo" ) );
			params.set( "param",  JsonValue::MakeString( "color" ) );
			params.set( "value",  JsonValue::MakeString( "0.9 0.05 0.05" ) );
			const std::string resp = rpc.HandleLine( Req( 7, "propose_patch", params ) );
			JsonValue env = ParseResponse( resp, 7 );
			Check( env.get( "result" ).get( "applied" ).asBool(), "compare_to_reference: recolor propose_patch applied" );
		}
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "view2" ) );
			params.set( "samples", JsonValue::MakeNumber( 16.0 ) );
			const std::string resp = rpc.HandleLine( Req( 8, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 8 );
			Check( env.has( "result" ), "compare_to_reference(view2, post-recolor): structured success" );
			const JsonValue& r = env.get( "result" );
			const double rmseAfter = r.get( "rmse" ).asNumber();
			std::printf( "  measured rmse after recolor = %.6f\n", rmseAfter );
			Check( rmseAfter > productionNoiseFloor + 0.05,
				"compare_to_reference: recolor raises rmse well beyond the production self-compare noise floor" );
			const double dR = r.get( "channelDelta" ).get( "r" ).asNumber();
			const double dG = r.get( "channelDelta" ).get( "g" ).asNumber();
			const double dB = r.get( "channelDelta" ).get( "b" ).asNumber();
			std::printf( "  channelDelta after recolor: dR=%.4f dG=%.4f dB=%.4f\n", dR, dG, dB );
			Check( dR > 0.0,
				"compare_to_reference: channelDelta.r is POSITIVE after recolor toward red (render redder than the gray reference)" );
			Check( dG < 0.0,
				"compare_to_reference: channelDelta.g is NEGATIVE after recolor away from the gray reference's green" );
			Check( dB < 0.0,
				"compare_to_reference: channelDelta.b is NEGATIVE after recolor away from the gray reference's blue" );
		}
	}

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// compare_to_reference split:true -- the object-vs-background RMSE
// breakdown (see AgentSession::AgentCompareSplitResult's doc).
//
//   1. FLOOR: register a PRODUCTION render of kSplitSceneBase as the
//      reference, then self-compare split:true against the SAME
//      unchanged scene -> both objectRmse and backgroundRmse are near
//      that scene's own production noise floor; objectPixelFraction is
//      sane (neither 0 nor 1 -- the sphere covers a real chunk of the
//      wide-fov frame, not all of it).
//   2. EMPTY OBJECT: an ephemeral camera override aimed away from the
//      sphere -> split.ok stays true (the objectmap render itself
//      succeeds), but objectRmse is -1 (no object pixels), note is
//      non-empty, objectPixelFraction is 0 -- no crash.
//   3. split:false (the default, omitted) -> the response carries no
//      "split" key at all (back-compat).
//   4. THE DISCRIMINATING PROOF (object edit): propose_patch recolors
//      ONLY the sphere's material (pnt_albedo) -> objectRmse rises far
//      beyond the floor while backgroundRmse stays close to it (the sky
//      never changed).
//   5. THE DISCRIMINATING PROOF (background edit): a SEPARATE session
//      loads kSplitSceneBgRecolor -- same sphere colour as the
//      reference, only the sky (turbidity/intensity) changed ->
//      backgroundRmse rises far beyond the floor while objectRmse stays
//      much closer to it (the sphere's material never changed; only a
//      modest indirect-GI shift from the sky is expected).
//////////////////////////////////////////////////////////////////////
static void RunCompareToReferenceSplit()
{
	std::printf( "=== compare_to_reference split:true: object-vs-background RMSE ===\n" );

	const std::string basePath = WriteTemp( "agent_compare_split_base.RISEscene", kSplitSceneBase );
	Check( !basePath.empty(), "split: base scratch scene file written" );

	std::vector<unsigned char> refPng;

	Job* pBaseJob = new Job();
	Check( pBaseJob->LoadAsciiSceneViaCst( basePath.c_str() ), "split: base scene loads via the CST path" );
	{
		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pBaseJob );
		Check( session != nullptr, "split: AgentSession::WrapJob wraps the base Job" );

		AgentRpcDispatcher rpc( std::move( session ) );
		AgentSession* s = rpc.Session();
		Check( s != nullptr, "split: dispatcher exposes the wrapped base session" );

		// ---- (1) FLOOR: production reference render + self-compare split:true ----
		double objFloor = -1.0, bgFloor = -1.0;
		{
			AgentRenderParams rp;
			rp.samples = 64;
			const AgentRenderResult rr = s->Render( rp );
			Check( rr.ok && !rr.png.empty(), "split: base production reference render succeeded" );
			refPng.assign( rr.png.begin(), rr.png.end() );

			std::vector<AgentReferenceImage> refs( 1 );
			refs[0].name = "viewSplit";
			refs[0].pngBytes.assign( refPng.begin(), refPng.end() );
			s->SetReferenceImages( std::move( refs ) );
		}
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "viewSplit" ) );
			params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
			params.set( "split",     JsonValue::MakeBool( true ) );
			params.set( "visual",    JsonValue::MakeBool( false ) );
			const std::string resp = rpc.HandleLine( Req( 1, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 1 );
			Check( env.has( "result" ), "split: floor self-compare: structured success" );
			const JsonValue& r = env.get( "result" );
			Check( r.has( "split" ), "split: floor self-compare: response carries a \"split\" key when split:true" );
			const JsonValue& sp = r.get( "split" );
			Check( sp.get( "ok" ).asBool(), "split: floor self-compare: split.ok is true" );
			objFloor = sp.get( "objectRmse" ).asNumber();
			bgFloor  = sp.get( "backgroundRmse" ).asNumber();
			const double objFrac = sp.get( "objectPixelFraction" ).asNumber();
			std::printf( "  floor: objectRmse=%.6f backgroundRmse=%.6f objectPixelFraction=%.4f\n",
				objFloor, bgFloor, objFrac );
			Check( objFloor >= 0.0, "split: floor self-compare: objectRmse is a real measurement (object IS visible)" );
			Check( objFloor < 0.05, "split: floor self-compare: objectRmse is near its own production noise floor" );
			Check( bgFloor  < 0.05, "split: floor self-compare: backgroundRmse is near its own production noise floor" );
			Check( objFrac > 0.02 && objFrac < 0.85,
				"split: floor self-compare: objectPixelFraction is sane (sphere covers SOME but not ALL of the wide-fov frame)" );
			Check( r.get( "summary" ).asString().find( "split:" ) != std::string::npos,
				"split: floor self-compare: summary mentions the split" );
		}

		// ---- (2) EMPTY OBJECT: camera aimed away ----
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "viewSplit" ) );
			params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
			params.set( "split",     JsonValue::MakeBool( true ) );
			params.set( "visual",    JsonValue::MakeBool( false ) );
			JsonValue camera = JsonValue::MakeObject();
			camera.set( "location", JsonValue::MakeString( "0 0 -3.5" ) );
			camera.set( "lookat",   JsonValue::MakeString( "0 0 -10" ) );
			camera.set( "up",       JsonValue::MakeString( "0 1 0" ) );
			params.set( "camera", camera );
			const std::string resp = rpc.HandleLine( Req( 2, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 2 );
			Check( env.has( "result" ), "split: empty-object (camera away): structured success, no crash" );
			const JsonValue& r = env.get( "result" );
			Check( r.has( "split" ), "split: empty-object: response carries a \"split\" key" );
			const JsonValue& sp = r.get( "split" );
			Check( sp.get( "ok" ).asBool(), "split: empty-object: split.ok stays true (the objectmap render itself succeeded)" );
			Check( sp.get( "objectRmse" ).asNumber() == -1.0, "split: empty-object: objectRmse is -1 (no object pixels)" );
			Check( sp.get( "objectPixelFraction" ).asNumber() == 0.0, "split: empty-object: objectPixelFraction is 0" );
			Check( !sp.get( "note" ).asString().empty(), "split: empty-object: note explains the empty mask" );
		}

		// ---- (2b) EMPTY BACKGROUND: the MIRROR of (2) -- the camera pulled
		// in so close that the sphere fills the ENTIRE frame, leaving zero
		// background pixels.  Guards the sentinel SYMMETRY: an empty
		// background bucket must report -1 ("not measured"), NOT 0.0, which
		// a model would read as "the background matches perfectly".  The
		// camera sits at z=1.0, i.e. 0.2 OUTSIDE the r=0.8 sphere, so the
		// sphere subtends ~106 degrees against a 55-degree fov.
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "viewSplit" ) );
			params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
			params.set( "split",     JsonValue::MakeBool( true ) );
			params.set( "visual",    JsonValue::MakeBool( false ) );
			JsonValue camera = JsonValue::MakeObject();
			camera.set( "location", JsonValue::MakeString( "0 0 1.0" ) );
			camera.set( "lookat",   JsonValue::MakeString( "0 0 0" ) );
			camera.set( "up",       JsonValue::MakeString( "0 1 0" ) );
			params.set( "camera", camera );
			const std::string resp = rpc.HandleLine( Req( 22, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 22 );
			Check( env.has( "result" ), "split: empty-background: structured success, no crash" );
			const JsonValue& sp = env.get( "result" ).get( "split" );
			Check( sp.get( "ok" ).asBool(), "split: empty-background: split.ok stays true" );
			const double bgRmse = sp.get( "backgroundRmse" ).asNumber();
			const double objFrac = sp.get( "objectPixelFraction" ).asNumber();
			std::printf( "  empty-background: backgroundRmse=%.6f objectPixelFraction=%.4f\n", bgRmse, objFrac );
			Check( objFrac == 1.0, "split: empty-background: objectPixelFraction is exactly 1 (object fills the frame)" );
			Check( bgRmse == -1.0,
				"split: empty-background: backgroundRmse is the -1 SENTINEL, not a misleading 0.0" );
			Check( sp.get( "objectRmse" ).asNumber() >= 0.0,
				"split: empty-background: objectRmse is still a real measurement" );
			Check( !sp.get( "note" ).asString().empty(),
				"split: empty-background: note explains the empty background bucket" );
		}

		// ---- (2c) READ_IMAGE CACHE SURVIVES THE SPLIT ----
		// CompareToReference documents that read_image afterwards returns the
		// frame the comparison GRADED, and the modeling-from-image-captures
		// skill tells models to do exactly that.  split:true runs a second,
		// ephemeral objectmap render internally, and Render() caches every
		// success for read_image -- so without an explicit stash/restore the
		// model would get handed the flat segmentation image instead of its
		// beauty render.  Discriminator: an objectmap of this scene has only
		// a handful of flat identity colours, while the graded beauty frame
		// (lit sphere + sky gradient) has many.
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "viewSplit" ) );
			params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
			params.set( "split",     JsonValue::MakeBool( true ) );
			params.set( "visual",    JsonValue::MakeBool( false ) );
			const std::string resp = rpc.HandleLine( Req( 23, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 23 );
			Check( env.has( "result" ), "split: cache-survival: the split compare itself succeeded" );

			const std::vector<unsigned char> after = s->ReadImage();
			Check( !after.empty(), "split: cache-survival: read_image after a split compare returns an image" );

			int distinct = 0;
			{
				IMemoryBuffer* buf = nullptr;
				RISE_API_CreateCompatibleMemoryBuffer( &buf,
					const_cast<char*>( reinterpret_cast<const char*>( after.data() ) ),
					(unsigned int)after.size(), false );
				IRasterImageReader* rd = nullptr;
				if( buf && RISE_API_CreatePNGReader( &rd, *buf, eColorSpace_Rec709RGB_Linear ) && rd ) {
					unsigned int w = 0, h = 0;
					if( rd->BeginRead( w, h ) ) {
						std::map<std::string,int> hist;
						for( unsigned int y = 0; y < h; ++y ) {
							for( unsigned int x = 0; x < w; ++x ) {
								RISEColor c; rd->ReadColor( c, x, y );
								char hb[16];
								std::snprintf( hb, sizeof( hb ), "%d,%d,%d",
									(int)( c.base.r * 255.0 + 0.5 ),
									(int)( c.base.g * 255.0 + 0.5 ),
									(int)( c.base.b * 255.0 + 0.5 ) );
								hist[hb]++;
							}
						}
						distinct = (int)hist.size();
						rd->EndRead();
					}
					rd->release();
				}
				if( buf ) buf->release();
			}
			std::printf( "  cache-survival: read_image distinct colours after split compare = %d\n", distinct );
			Check( distinct > 10,
				"split: cache-survival: read_image returns the GRADED BEAUTY frame, not the flat objectmap" );

			// The check above only exercises mLastPng.  mLastSink is a
			// SEPARATE cached pointer, read only by the downscaling
			// ReadImage(maxEdge) overload and ReadPerception, so nothing
			// above speaks for it.  Its failure mode is NOT "goes empty":
			// drop the sink half of EphemeralRenderCacheGuard's restore and
			// mLastSink is left pointing at the EPHEMERAL OBJECTMAP sink
			// (non-null), so both readers happily serve a SEGMENTATION
			// image -- exactly the shipped bug's shape, one pointer over.
			// A "did it come back non-empty?" assertion therefore proves
			// nothing, and DIMS cannot discriminate here either:
			// CompareToReference forces the objectmap render to the SAME
			// refW/refH as the graded candidate, so both sinks are 40x30.
			//
			// CONTENT is the discriminator.  ToPngDownscaled with maxEdge >=
			// the long edge never downscales and emits bytes IDENTICAL to
			// ToPng() (see its "already within bounds" early-out) -- and
			// ToPng() is exactly what produced mLastPng at the render's
			// cache tail.  So byte-equality with `after` is reachable ONLY
			// from the beauty sink; the objectmap sink's own re-encode
			// differs wholesale.  The 16-bound call after it additionally
			// locks the aspect-preserving downscale shape.
			// (query_object_at decodes its OWN render's returned PNG, never
			// this cache -- it is a WRITER of the pair, guarded by the same
			// RAII helper; see AgentObjectMapTest's "preserves the
			// read_image cache" test.)
			{
				// 40 == kSplitSceneBase's authored film long edge, which the
				// compare forces both the candidate and the objectmap render to.
				unsigned int nw = 0, nh = 0;
				const std::vector<unsigned char> native = s->ReadImage( 40, nw, nh );
				Check( nw == 40 && nh == 30,
					"split: cache-survival: the restored sink reports the graded frame's 40x30 dims" );
				Check( !native.empty() && native == after,
					"split: cache-survival: MONEY -- a no-downscale ReadImage(maxEdge) re-encodes the "
					"BEAUTY sink BYTE-FOR-BYTE, so mLastSink was restored and is NOT the ephemeral objectmap sink" );

				unsigned int dw = 0, dh = 0;
				const std::vector<unsigned char> scaled = s->ReadImage( 16, dw, dh );
				Check( !scaled.empty() && dw == 16 && dh == 12,
					"split: cache-survival: the restored 40x30 sink downscales to EXACTLY 16x12 (aspect preserved)" );
			}
		}

		// ---- (3) split:false (default, omitted) -> no "split" key ----
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "viewSplit" ) );
			params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
			params.set( "visual",    JsonValue::MakeBool( false ) );
			const std::string resp = rpc.HandleLine( Req( 3, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 3 );
			Check( env.has( "result" ), "split: split:false (default): structured success" );
			Check( !env.get( "result" ).has( "split" ),
				"split: split:false (default): no \"split\" key at all -- back-compat" );
		}

		// ---- (4) DISCRIMINATING PROOF: recolor the OBJECT only ----
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "target", JsonValue::MakeString( "pnt_albedo" ) );
			params.set( "param",  JsonValue::MakeString( "color" ) );
			params.set( "value",  JsonValue::MakeString( "0.1 0.7 0.1" ) );
			const std::string resp = rpc.HandleLine( Req( 4, "propose_patch", params ) );
			JsonValue env = ParseResponse( resp, 4 );
			Check( env.get( "result" ).get( "applied" ).asBool(), "split: object-recolor propose_patch applied" );
		}
		{
			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "viewSplit" ) );
			params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
			params.set( "split",     JsonValue::MakeBool( true ) );
			params.set( "visual",    JsonValue::MakeBool( false ) );
			const std::string resp = rpc.HandleLine( Req( 5, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 5 );
			Check( env.has( "result" ), "split: post-object-recolor: structured success" );
			const JsonValue& sp = env.get( "result" ).get( "split" );
			const double objAfter = sp.get( "objectRmse" ).asNumber();
			const double bgAfter  = sp.get( "backgroundRmse" ).asNumber();
			std::printf( "  after OBJECT recolor: objectRmse=%.6f (floor %.6f) backgroundRmse=%.6f (floor %.6f)\n",
				objAfter, objFloor, bgAfter, bgFloor );
			Check( objAfter > objFloor + 0.1,
				"split: object recolor: objectRmse rises sharply above the floor" );
			Check( bgAfter < bgFloor + 0.03,
				"split: object recolor: backgroundRmse stays close to its floor (the sky never changed)" );
			Check( objAfter > bgAfter,
				"split: object recolor: objectRmse is now the dominant error, well above backgroundRmse" );
		}
	}
	pBaseJob->release();
	std::remove( basePath.c_str() );

	// ---- (5) DISCRIMINATING PROOF: recolor the BACKGROUND only (a SEPARATE session/scene) ----
	{
		const std::string bgPath = WriteTemp( "agent_compare_split_bg.RISEscene", kSplitSceneBgRecolor );
		Check( !bgPath.empty(), "split: background scratch scene file written" );

		Job* pBgJob = new Job();
		Check( pBgJob->LoadAsciiSceneViaCst( bgPath.c_str() ), "split: background scene loads via the CST path" );
		{
			std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pBgJob );
			Check( session != nullptr, "split: AgentSession::WrapJob wraps the background Job" );

			AgentRpcDispatcher rpc( std::move( session ) );
			AgentSession* s = rpc.Session();
			Check( s != nullptr, "split: dispatcher exposes the wrapped background session" );

			// Re-register the ORIGINAL (unchanged-sky) baseline PNG as the
			// reference -- this session's LIVE scene is the one with the
			// changed sky.
			std::vector<AgentReferenceImage> refs( 1 );
			refs[0].name = "viewSplit";
			refs[0].pngBytes.assign( refPng.begin(), refPng.end() );
			s->SetReferenceImages( std::move( refs ) );

			JsonValue params = JsonValue::MakeObject();
			params.set( "reference", JsonValue::MakeString( "viewSplit" ) );
			params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
			params.set( "split",     JsonValue::MakeBool( true ) );
			params.set( "visual",    JsonValue::MakeBool( false ) );
			const std::string resp = rpc.HandleLine( Req( 6, "compare_to_reference", params ) );
			JsonValue env = ParseResponse( resp, 6 );
			Check( env.has( "result" ), "split: background-recolor: structured success" );
			const JsonValue& sp = env.get( "result" ).get( "split" );
			const double objAfter = sp.get( "objectRmse" ).asNumber();
			const double bgAfter  = sp.get( "backgroundRmse" ).asNumber();
			std::printf( "  after BACKGROUND recolor: objectRmse=%.6f backgroundRmse=%.6f\n", objAfter, bgAfter );
			Check( bgAfter > 0.05,
				"split: background recolor: backgroundRmse rises sharply above the baseline floor" );
			Check( bgAfter > objAfter,
				"split: background recolor: backgroundRmse is now the dominant error, well above objectRmse" );
		}
		pBgJob->release();
		std::remove( bgPath.c_str() );
	}

	// ---- (6) LEGEND AGGREGATION: the mask must union EVERY registered
	// object, not just one.  Two equal spheres are visible; only the SECOND
	// (obj_b, legend entry 1) is recoloured.  A mask built from a single
	// legend entry would classify obj_b's pixels as BACKGROUND, so the
	// change would land in backgroundRmse instead of objectRmse -- this
	// test fails loudly in that case.
	{
		const std::string twoPath = WriteTemp( "agent_compare_split_two.RISEscene", kSplitSceneTwoObjects );
		Check( !twoPath.empty(), "split: two-object scratch scene file written" );

		Job* pTwoJob = new Job();
		Check( pTwoJob->LoadAsciiSceneViaCst( twoPath.c_str() ), "split: two-object scene loads via the CST path" );
		{
			std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pTwoJob );
			Check( session != nullptr, "split: AgentSession::WrapJob wraps the two-object Job" );

			AgentRpcDispatcher rpc( std::move( session ) );
			AgentSession* s = rpc.Session();
			Check( s != nullptr, "split: dispatcher exposes the wrapped two-object session" );

			// Reference = a production render of the UNCHANGED two-sphere scene.
			AgentRenderParams rp;
			rp.samples = 64;
			const AgentRenderResult rr = s->Render( rp );
			Check( rr.ok && !rr.png.empty(), "split: two-object reference render succeeded" );

			std::vector<AgentReferenceImage> refs( 1 );
			refs[0].name = "viewTwo";
			refs[0].pngBytes.assign( rr.png.begin(), rr.png.end() );
			s->SetReferenceImages( std::move( refs ) );

			// Floor first, so the post-recolor rise is measured against this
			// scene's OWN production noise -- and confirm BOTH spheres are in
			// the mask (a single-entry mask would report roughly half this).
			double objFloor = -1.0, bgFloor = -1.0, twoObjFrac = -1.0;
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "reference", JsonValue::MakeString( "viewTwo" ) );
				params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
				params.set( "split",     JsonValue::MakeBool( true ) );
				params.set( "visual",    JsonValue::MakeBool( false ) );
				const std::string resp = rpc.HandleLine( Req( 7, "compare_to_reference", params ) );
				JsonValue env = ParseResponse( resp, 7 );
				Check( env.has( "result" ), "split: two-object floor: structured success" );
				const JsonValue& sp = env.get( "result" ).get( "split" );
				objFloor   = sp.get( "objectRmse" ).asNumber();
				bgFloor    = sp.get( "backgroundRmse" ).asNumber();
				twoObjFrac = sp.get( "objectPixelFraction" ).asNumber();
				std::printf( "  two-object floor: objectRmse=%.6f backgroundRmse=%.6f objectPixelFraction=%.4f\n",
					objFloor, bgFloor, twoObjFrac );
				Check( objFloor >= 0.0 && bgFloor >= 0.0, "split: two-object floor: both buckets are real measurements" );
			}

			// Recolour ONLY obj_b's painter.
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "target", JsonValue::MakeString( "pnt_albedo_b" ) );
				params.set( "param",  JsonValue::MakeString( "color" ) );
				params.set( "value",  JsonValue::MakeString( "0.1 0.7 0.1" ) );
				const std::string resp = rpc.HandleLine( Req( 8, "propose_patch", params ) );
				JsonValue env = ParseResponse( resp, 8 );
				Check( env.get( "result" ).get( "applied" ).asBool(), "split: second-object recolor propose_patch applied" );
			}
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "reference", JsonValue::MakeString( "viewTwo" ) );
				params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
				params.set( "split",     JsonValue::MakeBool( true ) );
				params.set( "visual",    JsonValue::MakeBool( false ) );
				const std::string resp = rpc.HandleLine( Req( 9, "compare_to_reference", params ) );
				JsonValue env = ParseResponse( resp, 9 );
				Check( env.has( "result" ), "split: post-second-object-recolor: structured success" );
				const JsonValue& sp = env.get( "result" ).get( "split" );
				const double objAfter = sp.get( "objectRmse" ).asNumber();
				const double bgAfter  = sp.get( "backgroundRmse" ).asNumber();
				std::printf( "  after SECOND-OBJECT recolor: objectRmse=%.6f (floor %.6f) backgroundRmse=%.6f (floor %.6f)\n",
					objAfter, objFloor, bgAfter, bgFloor );
				Check( objAfter > objFloor + 0.05,
					"split: second-object recolor: objectRmse rises -- the mask covers legend entry 1, not just entry 0" );
				Check( bgAfter < bgFloor + 0.03,
					"split: second-object recolor: backgroundRmse stays near its floor (nothing in the sky changed)" );
				Check( objAfter > bgAfter,
					"split: second-object recolor: the error is attributed to the OBJECT bucket" );
			}
		}
		pTwoJob->release();
		std::remove( twoPath.c_str() );
	}

	// ---- (7) SCOPED SPLIT: `splitObjects` restricts the OBJECT bucket to
	// caller-named legend entries -- the fix for the "stage lands in the
	// OBJECT bucket" problem found in a live eval run.  Reuses the SAME
	// two-sphere scene as (6): scoping to ["obj_a"] must (a) roughly halve
	// objectPixelFraction vs the unscoped split (both spheres are equal
	// size), and (b) the money test -- recolour obj_b (the object EXCLUDED
	// from the scope) and confirm the error now lands in BACKGROUND, not
	// OBJECT -- the inverse of test (6), where the exact same recolour
	// raised objectRmse under the unscoped mask.  Also covers the
	// unknown-name and partial-match note wording.
	{
		const std::string scopedPath = WriteTemp( "agent_compare_split_scoped.RISEscene", kSplitSceneTwoObjects );
		Check( !scopedPath.empty(), "splitObjects: scoped scratch scene file written" );

		Job* pScopedJob = new Job();
		Check( pScopedJob->LoadAsciiSceneViaCst( scopedPath.c_str() ), "splitObjects: scoped scene loads via the CST path" );
		{
			std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pScopedJob );
			Check( session != nullptr, "splitObjects: AgentSession::WrapJob wraps the scoped session" );

			AgentRpcDispatcher rpc( std::move( session ) );
			AgentSession* s = rpc.Session();
			Check( s != nullptr, "splitObjects: dispatcher exposes the wrapped scoped session" );

			AgentRenderParams rp;
			rp.samples = 64;
			const AgentRenderResult rr = s->Render( rp );
			Check( rr.ok && !rr.png.empty(), "splitObjects: scoped reference render succeeded" );

			std::vector<AgentReferenceImage> refs( 1 );
			refs[0].name = "viewScoped";
			refs[0].pngBytes.assign( rr.png.begin(), rr.png.end() );
			s->SetReferenceImages( std::move( refs ) );

			// (e) BACK-COMPAT: split:true with splitObjects omitted behaves
			// exactly as the unscoped tests above -- both spheres in the
			// OBJECT bucket, no note.
			double unscopedFrac = -1.0;
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "reference", JsonValue::MakeString( "viewScoped" ) );
				params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
				params.set( "split",     JsonValue::MakeBool( true ) );
				params.set( "visual",    JsonValue::MakeBool( false ) );
				const std::string resp = rpc.HandleLine( Req( 10, "compare_to_reference", params ) );
				JsonValue env = ParseResponse( resp, 10 );
				Check( env.has( "result" ), "splitObjects: back-compat unscoped: structured success" );
				const JsonValue& sp = env.get( "result" ).get( "split" );
				unscopedFrac = sp.get( "objectPixelFraction" ).asNumber();
				std::printf( "  splitObjects back-compat (omitted): objectPixelFraction=%.4f note=\"%s\"\n",
					unscopedFrac, sp.get( "note" ).asString().c_str() );
				Check( sp.get( "ok" ).asBool(), "splitObjects: back-compat unscoped: split ok" );
				Check( unscopedFrac > 0.0, "splitObjects: back-compat unscoped: mask is non-empty" );
				Check( sp.get( "note" ).asString().empty(),
					"splitObjects: back-compat unscoped: no note (both spheres visible, nothing scoped)" );
			}

			// (a) SCOPED TO obj_a: objectPixelFraction should be roughly
			// HALF the unscoped fraction (equal-size spheres) -- meaningfully
			// less, and still > 0.
			double scopedObjFloor = -1.0, scopedBgFloor = -1.0;
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "reference", JsonValue::MakeString( "viewScoped" ) );
				params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
				params.set( "split",     JsonValue::MakeBool( true ) );
				params.set( "visual",    JsonValue::MakeBool( false ) );
				JsonValue splitObjects = JsonValue::MakeArray();
				splitObjects.push_back( JsonValue::MakeString( "obj_a" ) );
				params.set( "splitObjects", splitObjects );
				const std::string resp = rpc.HandleLine( Req( 11, "compare_to_reference", params ) );
				JsonValue env = ParseResponse( resp, 11 );
				Check( env.has( "result" ), "splitObjects: scoped-to-obj_a floor: structured success" );
				const JsonValue& sp = env.get( "result" ).get( "split" );
				scopedObjFloor = sp.get( "objectRmse" ).asNumber();
				scopedBgFloor  = sp.get( "backgroundRmse" ).asNumber();
				const double scopedFrac = sp.get( "objectPixelFraction" ).asNumber();
				std::printf( "  splitObjects=[obj_a]: objectPixelFraction=%.4f (unscoped %.4f) objectRmse=%.6f backgroundRmse=%.6f\n",
					scopedFrac, unscopedFrac, scopedObjFloor, scopedBgFloor );
				Check( scopedFrac > 0.0, "splitObjects: scoped-to-obj_a: mask is non-empty" );
				Check( scopedFrac < unscopedFrac * 0.65,
					"splitObjects: scoped-to-obj_a: objectPixelFraction is meaningfully less than the unscoped fraction" );
				Check( scopedFrac > unscopedFrac * 0.35,
					"splitObjects: scoped-to-obj_a: objectPixelFraction is roughly half the unscoped fraction (both spheres are equal size)" );
				Check( scopedObjFloor >= 0.0 && scopedBgFloor >= 0.0,
					"splitObjects: scoped-to-obj_a floor: both buckets are real measurements" );
			}

			// (b) THE MONEY TEST: recolour obj_b -- the object EXCLUDED from
			// the ["obj_a"] scope -- and confirm the error now lands in
			// BACKGROUND, the inverse of the unscoped legend-aggregation
			// test (6) where the exact same recolour raised objectRmse.
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "target", JsonValue::MakeString( "pnt_albedo_b" ) );
				params.set( "param",  JsonValue::MakeString( "color" ) );
				params.set( "value",  JsonValue::MakeString( "0.1 0.7 0.1" ) );
				const std::string resp = rpc.HandleLine( Req( 12, "propose_patch", params ) );
				JsonValue env = ParseResponse( resp, 12 );
				Check( env.get( "result" ).get( "applied" ).asBool(), "splitObjects: money-test obj_b recolor propose_patch applied" );
			}
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "reference", JsonValue::MakeString( "viewScoped" ) );
				params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
				params.set( "split",     JsonValue::MakeBool( true ) );
				params.set( "visual",    JsonValue::MakeBool( false ) );
				JsonValue splitObjects = JsonValue::MakeArray();
				splitObjects.push_back( JsonValue::MakeString( "obj_a" ) );
				params.set( "splitObjects", splitObjects );
				const std::string resp = rpc.HandleLine( Req( 13, "compare_to_reference", params ) );
				JsonValue env = ParseResponse( resp, 13 );
				Check( env.has( "result" ), "splitObjects: money-test post-recolor: structured success" );
				const JsonValue& sp = env.get( "result" ).get( "split" );
				const double objAfter = sp.get( "objectRmse" ).asNumber();
				const double bgAfter  = sp.get( "backgroundRmse" ).asNumber();
				std::printf( "  splitObjects MONEY TEST: BEFORE objectRmse=%.6f backgroundRmse=%.6f -- AFTER obj_b recolor objectRmse=%.6f (floor %.6f) backgroundRmse=%.6f (floor %.6f)\n",
					scopedObjFloor, scopedBgFloor, objAfter, scopedObjFloor, bgAfter, scopedBgFloor );
				Check( objAfter < scopedObjFloor + 0.03,
					"splitObjects: money test: objectRmse (scoped to obj_a) stays near its floor -- obj_b is OUT of scope" );
				Check( bgAfter > scopedBgFloor + 0.05,
					"splitObjects: money test: backgroundRmse rises sharply -- obj_b's recolor now lands in BACKGROUND" );
				Check( bgAfter > objAfter,
					"splitObjects: money test: the error is now attributed to the BACKGROUND bucket, the inverse of the unscoped case" );
			}

			// (c) UNKNOWN NAME: objectRmse == -1, note names "nope" and
			// lists the available legend names, and does NOT claim the
			// object is off-frame/occluded (that wording is reserved for
			// the genuinely-empty-mask case, which this is not).
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "reference", JsonValue::MakeString( "viewScoped" ) );
				params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
				params.set( "split",     JsonValue::MakeBool( true ) );
				params.set( "visual",    JsonValue::MakeBool( false ) );
				JsonValue splitObjects = JsonValue::MakeArray();
				splitObjects.push_back( JsonValue::MakeString( "nope" ) );
				params.set( "splitObjects", splitObjects );
				const std::string resp = rpc.HandleLine( Req( 14, "compare_to_reference", params ) );
				JsonValue env = ParseResponse( resp, 14 );
				Check( env.has( "result" ), "splitObjects: unknown-name: structured success" );
				const JsonValue& sp = env.get( "result" ).get( "split" );
				const double objRmse = sp.get( "objectRmse" ).asNumber();
				const std::string note = sp.get( "note" ).asString();
				std::printf( "  splitObjects=[nope]: objectRmse=%.6f note=\"%s\"\n", objRmse, note.c_str() );
				Check( objRmse == -1.0, "splitObjects: unknown-name: objectRmse sentinels to -1" );
				Check( note.find( "nope" ) != std::string::npos,
					"splitObjects: unknown-name: note names the unknown requested object" );
				Check( note.find( "obj_a" ) != std::string::npos,
					"splitObjects: unknown-name: note lists the available legend name(s)" );
				Check( note.find( "off-frame" ) == std::string::npos && note.find( "occluded" ) == std::string::npos,
					"splitObjects: unknown-name: note does NOT use the off-frame/occluded wording -- the cause is an unknown name, not a missing object" );
			}

			// (d) PARTIAL MATCH: ["obj_a","nope"] computes normally off the
			// matched name(s) and the note still names the unknown one, so
			// a typo can't silently shrink the mask unnoticed.
			{
				JsonValue params = JsonValue::MakeObject();
				params.set( "reference", JsonValue::MakeString( "viewScoped" ) );
				params.set( "samples",   JsonValue::MakeNumber( 64.0 ) );
				params.set( "split",     JsonValue::MakeBool( true ) );
				params.set( "visual",    JsonValue::MakeBool( false ) );
				JsonValue splitObjects = JsonValue::MakeArray();
				splitObjects.push_back( JsonValue::MakeString( "obj_a" ) );
				splitObjects.push_back( JsonValue::MakeString( "nope" ) );
				params.set( "splitObjects", splitObjects );
				const std::string resp = rpc.HandleLine( Req( 15, "compare_to_reference", params ) );
				JsonValue env = ParseResponse( resp, 15 );
				Check( env.has( "result" ), "splitObjects: partial-match: structured success" );
				const JsonValue& sp = env.get( "result" ).get( "split" );
				const double objRmse = sp.get( "objectRmse" ).asNumber();
				const std::string note = sp.get( "note" ).asString();
				std::printf( "  splitObjects=[obj_a,nope]: objectRmse=%.6f note=\"%s\"\n", objRmse, note.c_str() );
				Check( objRmse >= 0.0, "splitObjects: partial-match: objectRmse computes normally off the matched name" );
				Check( note.find( "nope" ) != std::string::npos,
					"splitObjects: partial-match: note names the unknown requested object even though obj_a matched" );

				// The summary is what a MODEL actually reads -- a typo that
				// silently shrank the mask must surface THERE too, not only
				// in the structured `note` field.  This branch computes both
				// buckets successfully, so it is the one branch that used to
				// drop the note on the floor.
				const std::string summary = env.get( "result" ).get( "summary" ).asString();
				Check( summary.find( "nope" ) != std::string::npos,
					"splitObjects: partial-match: the human-readable summary ALSO carries the unknown-name warning" );
			}
		}
		pScopedJob->release();
		std::remove( scopedPath.c_str() );
	}
}

int main()
{
	std::printf( "=== AgentViewportReadTest ===\n" );

	RunPositiveAndIsolation();
	RunNoController();
	RunNoFrameYet();
	RunPaneSetSnapshotAtomicity();
	RunDisplayTransformOrdering();
	RunCompareToReference();
	RunCompareToReferenceSplit();

	std::printf( "=== AgentViewportReadTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
