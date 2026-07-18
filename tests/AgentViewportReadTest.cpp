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
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
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
		}

		controller.Stop();   // retire the (never-started-interactive) agent worker + threads
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
			session->ForTest_ResolveBeautyDisplayTransform( exposureEV, dt );
			Check( dt == kDT_None,
				"HDR-first: resolves to the LATER LDR output's \"none\" transform, "
				"not the ACES default the FIRST (HDR) output would wrongly trigger "
				"(got " + std::to_string( dt ) + ", want " + std::to_string( kDT_None ) + ")" );
			Check( exposureEV == 0.0, "HDR-first: exposureEV is 0 (no camera EV, no declared exposure)" );
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
			session->ForTest_ResolveBeautyDisplayTransform( exposureEV, dt );
			Check( dt == kDT_None, "LDR-first: resolves to \"none\" (the LDR output's own declaration)" );
			Check( exposureEV == 0.0, "LDR-first: exposureEV is 0" );
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
			session->ForTest_ResolveBeautyDisplayTransform( exposureEV, dt );
			Check( dt == kDT_ACES, "HDR-only: no LDR output anywhere -> falls back to the ACES default" );
			Check( exposureEV == 0.0, "HDR-only: exposureEV falls back to 0" );
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

int main()
{
	std::printf( "=== AgentViewportReadTest ===\n" );

	RunPositiveAndIsolation();
	RunNoController();
	RunNoFrameYet();
	RunDisplayTransformOrdering();
	RunCompareToReference();

	std::printf( "=== AgentViewportReadTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
