//////////////////////////////////////////////////////////////////////
//
//  AgentProposeRenderTest.cpp - Facet 5 (agentic surface) slice 0b.
//
//  Proves the TWO new AgentSession capabilities end-to-end, both
//  headless + single-threaded (docs/agentic-redesign/50-agentic-surface.md
//  §2.2.3 propose_patch, §2.2.5 render / read_image):
//
//    * ProposePatch (STRUCTURED set) — a param-value edit routed through
//      Job::ApplyCstParamEdit (the SAME call the GUI property panel makes)
//      that MUTATES the retained CST Document and re-derives the live Job.
//      Asserted: applied==true, ReadDocument() now CONTAINS the new value
//      AND round-trips (RED-PROVEN: the value differs from the pre-edit
//      head); a BOGUS target is rejected (applied==false) WITHOUT
//      corrupting the head (ReadDocument still round-trips + unchanged).
//
//    * Render + ReadImage — render the current head into an in-memory
//      sRGB PNG.  Asserted: non-empty PNG bytes, valid \x89PNG signature,
//      IHDR dims match the film; a render is SIDE-EFFECT-FREE on the
//      Document (ReadDocument() byte-identical across the Render call); and
//      a NOISE FLOOR is measured by re-rendering the SAME head — RISE's PT
//      sampler is not bit-deterministic (per-worker RNG state depends on
//      thread order, so the DEFLATE stream diverges wholesale), but the
//      LINEAR channel means agree within a tiny MC noise floor.  The
//      edit-changes-render "money assertion" below is measured AGAINST that
//      floor so it cannot ride on render nondeterminism.
//
//    * Edit-then-render coherence (the money assertion): a VISIBLE edit
//      (recolour the sphere's albedo painter) then a second Render yields
//      DIFFERENT PNG bytes — proving the edit flowed
//      Document -> derive -> render, i.e. the agent is a genuine client of
//      the GUI's edit pathway (L2).
//
//  Self-contained: an inline native-v7 scene (a lit sphere), OIDN off,
//  no RISE_MEDIA_PATH.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/Json.h"   // pre-S2 hardening: red-prove the session-local id's exact wire round-trip
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Interfaces/IRasterizer.h"   // Model-B F2 slice S3: SetSampleCountOverride/GetSampleCountOverride red-prove
#include "../src/Library/SceneEditor/CameraIntrospection.h"
#include "../src/Library/SceneEditor/SceneEditController.h"   // Model-B F2 slice S1: RenderJobId coordinator bookkeeping

#include <atomic>      // Model-B F2 slice S1: TestController's simulated-render slice counter
#include <chrono>      // Model-B F2 slice S1: TestController's simulated-render sleep
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>   // P1-A red-prove: ThrowingRasterizeJob throws std::runtime_error
#include <string>
#include <thread>      // Model-B F2 slice S1: drive a Render(AgentRenderParams) from a background thread
#include <vector>

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

// A small, self-contained native-v7 scene: a lit diffuse sphere with an
// area emitter, path-traced at a low sample count with OIDN off.  The
// shader chunk precedes the rasterizer (Job::Set*Rasterizer resolves
// `defaultshader` during the rasterizer chunk's Finalize).  Mirrors the
// known-good AutoRasterizerTest body so it renders non-black.
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// Write `text` to a temp file and return its path (or "" on failure).
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

// The 8-byte PNG signature.
static bool HasPngSignature( const std::vector<unsigned char>& b )
{
	static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	if( b.size() < 8 ) return false;
	for( int i = 0; i < 8; ++i ) if( b[i] != sig[i] ) return false;
	return true;
}

// Read the big-endian width/height out of the PNG IHDR chunk (bytes
// 16..23 for a well-formed PNG: 8 sig + 4 len + 4 "IHDR" + 4 width +
// 4 height).  Returns false if the buffer is too short or IHDR is absent.
static bool ReadPngDims( const std::vector<unsigned char>& b, unsigned int& w, unsigned int& h )
{
	if( b.size() < 24 ) return false;
	if( std::memcmp( &b[12], "IHDR", 4 ) != 0 ) return false;
	w = ( (unsigned)b[16] << 24 ) | ( (unsigned)b[17] << 16 ) | ( (unsigned)b[18] << 8 ) | (unsigned)b[19];
	h = ( (unsigned)b[20] << 24 ) | ( (unsigned)b[21] << 16 ) | ( (unsigned)b[22] << 8 ) | (unsigned)b[23];
	return true;
}

static void RunCameraOverrideTests();          // fwd decl -- defined below, called from main()
static void RunRestoreOnThrowTest();           // P1-A red-prove -- defined below, called from main()
static void RunMalformedCameraOverrideTests(); // P1-B -- defined below, called from main()
static void RunRenderJobIdTests();             // Model-B F2 slice S1 -- defined below, called from main()
static void RunPreS2HardeningTests();          // Model-B F2 slice S1 pre-S2 hardening -- defined below, called from main()
static void RunSampleCountOverrideTests();     // Model-B F2 slice S3 (EffectiveRenderConfig) -- defined below, called from main()

static void RunCoreTests()
{
	std::printf( "=== AgentProposeRenderTest (Facet 5 slice 0b: propose_patch + render/read_image) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_slice0b.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the scene to a temp file" );

	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "AgentSession::LoadFromFile loads the native-v7 scene" );
	if( !session ) { std::printf( "cannot continue without a session\n" ); return; }

	//----------------------------------------------------------------------
	// propose_patch — a structured set on a real param.
	//----------------------------------------------------------------------
	std::printf( "[propose_patch] set a real param (albedo colour)\n" );
	const std::string preEdit = session->ReadDocument();
	Check( preEdit.find( "0.5 0.5 0.5" ) != std::string::npos,
	       "pre-edit head carries the original albedo colour (0.5 0.5 0.5)" );

	{
		AgentSetPatch sp;
		sp.target = "pnt_albedo";
		sp.param  = "color";
		sp.value  = "0.9 0.1 0.1";
		AgentPatchResult r = session->ProposePatch( sp );
		Check( r.applied, "ProposePatch(pnt_albedo.color) applied" );
		Check( r.status == "applied", "clean apply -> status==\"applied\"" );

		const std::string postEdit = session->ReadDocument();
		// The Document was mutated: it now carries the new value ...
		Check( postEdit.find( "0.9 0.1 0.1" ) != std::string::npos,
		       "ReadDocument() now CONTAINS the new albedo value (Document mutated)" );
		// ... RED-PROVE: and it DIFFERS from the pre-edit head.
		Check( postEdit != preEdit, "post-edit head DIFFERS from pre-edit head (red-prove)" );
		// ... and still round-trips through the CST parser.
		Cst::Document rt = Cst::ParseToCst( postEdit );
		Check( Cst::SerializeCst( rt ) == postEdit,
		       "ParseToCst(ReadDocument()) round-trips byte-identically after the edit" );
	}

	//----------------------------------------------------------------------
	// propose_patch — a bogus target is rejected without corrupting the head.
	//----------------------------------------------------------------------
	std::printf( "[propose_patch] a bogus target is rejected, head intact\n" );
	{
		const std::string before = session->ReadDocument();
		AgentSetPatch bad;
		bad.target = "no_such_entity";
		bad.param  = "color";
		bad.value  = "0 0 0";
		AgentPatchResult r = session->ProposePatch( bad );
		Check( !r.applied, "ProposePatch(bogus target) is NOT applied" );
		Check( r.rawCode == 0, "bogus target maps to rawCode 0 (reject)" );
		Check( r.status == "rejected", "bogus target -> status==\"rejected\" (head byte-identical)" );
		const std::string after = session->ReadDocument();
		Check( after == before, "head is unchanged after a rejected edit" );
		Cst::Document rt = Cst::ParseToCst( after );
		Check( Cst::SerializeCst( rt ) == after, "head still round-trips after a rejected edit" );
	}

	// A missing-Document guard is exercised implicitly via a bogus param on
	// a real target too (defensive) — but keep the head-integrity focus above.
	//
	// P1 #2 code-3 mapping (mutated-but-diagnosed -> applied=false/"diagnosed"):
	// NOT synthesizable here.  rawCode 3 fires only when Job.cpp's
	// DeriveEditedCstDocument_ full re-derive emits diagnostics on a document
	// whose validate-before-destroy dry-run ALREADY passed the identical
	// derive -- the source calls this the "should-not-happen" branch.  Every
	// authorable bad edit is caught earlier and returns 0 (reject, head
	// intact), so a real code-3 cannot be reliably provoked from a scene edit
	// without a fault-injection seam that does not exist in slice 0.  The
	// mapping (rawCode 3 -> applied=false, status=="diagnosed", message states
	// the Document WAS mutated) is therefore covered BY INSPECTION of the
	// switch in AgentSession::ProposePatch, not by a flaky synthesized case.

	//----------------------------------------------------------------------
	// render — the current head to an in-memory sRGB PNG.
	//----------------------------------------------------------------------
	std::printf( "[render] head -> in-memory sRGB PNG\n" );
	std::vector<unsigned char> firstPng;
	double firstMeanR = 0.0, firstMeanG = 0.0, firstMeanB = 0.0;
	double noiseFloor = 0.0;
	{
		// A render must NOT mutate the retained Document -- capture the head
		// before rendering so we can prove it is byte-identical afterward.
		const std::string docBeforeRender = session->ReadDocument();

		AgentRenderResult rr = session->Render( /*samplesOverride=*/-1 );
		Check( rr.ok, "Render() succeeded" );
		Check( rr.width == 24 && rr.height == 24, "render dims match the film (24x24)" );
		Check( !rr.png.empty(), "render produced non-empty PNG bytes" );
		Check( HasPngSignature( rr.png ), "PNG bytes carry the \\x89PNG signature" );

		unsigned int w = 0, h = 0;
		Check( ReadPngDims( rr.png, w, h ), "PNG IHDR is present" );
		Check( w == 24 && h == 24, "PNG IHDR dims match the film (24x24)" );

		// A render is side-effect-free on the Document: ReadDocument() is
		// byte-identical across the Render call (proves samplesOverride and
		// the sink swap never touch the retained CST).
		Check( session->ReadDocument() == docBeforeRender,
		       "ReadDocument() is byte-identical after a Render (render does not mutate the Document)" );

		// ReadImage() returns the cached bytes of the last render.
		std::vector<unsigned char> cached = session->ReadImage();
		Check( cached == rr.png, "ReadImage() returns the last render's PNG bytes" );

		firstPng     = rr.png;
		firstMeanR   = rr.meanR;
		firstMeanG   = rr.meanG;
		firstMeanB   = rr.meanB;
	}

	//----------------------------------------------------------------------
	// render noise floor — re-render the SAME head (no edit between) to
	// MEASURE the run-to-run difference.  RISE's PT sampler draws from a
	// per-worker RNG whose state depends on thread scheduling, so two
	// renders are NOT byte-identical (the DEFLATE stream diverges wholesale
	// on any sub-LSB pixel change).  We therefore establish determinism in
	// the STABLE, order-independent LINEAR channel-mean signature: the same
	// head re-renders to means within a tiny MC noise floor.  This bounds
	// the noise BEFORE the edit-changes-render assertion below, so a mean
	// shift under an edit cannot be attributed to render nondeterminism.
	//----------------------------------------------------------------------
	std::printf( "[noise floor] re-render the same head: channel means stable within MC noise\n" );
	{
		AgentRenderResult rr = session->Render( -1 );
		Check( rr.ok && !rr.png.empty(), "second Render() of the unchanged head produced PNG bytes" );
		const double dR = std::fabs( rr.meanR - firstMeanR );
		const double dG = std::fabs( rr.meanG - firstMeanG );
		const double dB = std::fabs( rr.meanB - firstMeanB );
		noiseFloor = dR + dG + dB;
		std::printf( "  same-head mean drift: dR=%.5f dG=%.5f dB=%.5f (sum=%.5f)\n", dR, dG, dB, noiseFloor );
		// Same head, same sample count: the linear means agree within a small
		// MC noise floor (empirically << 0.02 at 8 spp on this scene).
		Check( noiseFloor < 0.05,
		       "re-rendering the SAME head yields channel means within the MC noise floor (deterministic in mean)" );
	}

	//----------------------------------------------------------------------
	// edit-then-render coherence — the money assertion.  A VISIBLE recolour
	// (grey -> saturated red sphere) then a re-render must shift the rendered
	// image FAR BEYOND the run-to-run noise floor measured above.  We compare
	// in the stable LINEAR channel-mean signature (not raw PNG bytes, which
	// diverge on every render): the red channel mean jumps and the green/blue
	// means drop, a change orders of magnitude larger than `noiseFloor`.  That
	// the shift exceeds the noise floor is what proves the EDIT (not render
	// nondeterminism) drove it — the edit flowed Document -> derive -> render.
	//----------------------------------------------------------------------
	std::printf( "[coherence] visible edit changes the rendered image (beyond the noise floor)\n" );
	{
		AgentSetPatch sp;
		sp.target = "pnt_albedo";
		sp.param  = "color";
		sp.value  = "0.9 0.05 0.05";   // grey -> saturated red: a large, obvious shift
		AgentPatchResult r = session->ProposePatch( sp );
		Check( r.applied, "ProposePatch(recolour sphere red) applied" );

		AgentRenderResult rr = session->Render( -1 );
		Check( rr.ok && !rr.png.empty(), "post-edit Render() produced PNG bytes" );

		const double editShift =
			std::fabs( rr.meanR - firstMeanR ) +
			std::fabs( rr.meanG - firstMeanG ) +
			std::fabs( rr.meanB - firstMeanB );
		std::printf( "  post-edit mean shift=%.5f  (noise floor=%.5f)\n", editShift, noiseFloor );
		// The visible recolour must move the image by MUCH more than the
		// same-head noise floor (guard against a false positive from MC noise).
		// Empirically the edit shifts the mean ~0.011 vs a ~0.0001 noise floor
		// (~100x); require a comfortable margin over BOTH the measured floor
		// (10x) and a small absolute threshold that still sits well under the
		// edit shift (most of the 24x24 frame is background/emitter, so the
		// lit-sphere recolour moves the whole-frame mean only modestly).
		Check( editShift > 10.0 * noiseFloor + 0.002,
		       "post-edit channel means shift FAR beyond the render noise floor (edit flowed Document->derive->render)" );
		// The PNG bytes also differ (necessary but not sufficient on its own,
		// since two renders of the SAME head already differ); kept as a cheap
		// sanity check alongside the mean-shift money assertion.
		Check( rr.png != firstPng, "post-edit PNG bytes differ from the pre-edit render" );
	}

	//----------------------------------------------------------------------
	// Preview-render: legacy Render(int) stays EXACTLY byte-compatible --
	// AgentRenderParams with every field absent must reproduce the same
	// result shape as before (previewWidth/previewHeight echo the actual
	// (unoverridden) film dims; cameraOverridden is false).
	//----------------------------------------------------------------------
	std::printf( "[preview-render] all-absent params == legacy Render(-1) behaviour\n" );
	{
		AgentRenderParams legacy;   // every field at its default (absent)
		AgentRenderResult rr = session->Render( legacy );
		Check( rr.ok, "Render(all-absent AgentRenderParams) succeeded" );
		Check( rr.width == 24 && rr.height == 24, "dims are the Document's authored 24x24 (no override applied)" );
		Check( rr.previewWidth == 24 && rr.previewHeight == 24, "previewWidth/previewHeight echo the dims actually used" );
		Check( !rr.cameraOverridden, "cameraOverridden is false when no camera override was requested" );
	}

	//----------------------------------------------------------------------
	// Preview-render: transient film-dims override.  Render at a SMALL
	// preview size; assert the result echoes the requested dims (PNG dims
	// too), then assert a FOLLOW-UP unoverridden Render is back at the
	// Document's authored 24x24 -- proving the override is TRANSIENT (never
	// wrote back into the Scene/Document permanently).
	//----------------------------------------------------------------------
	std::printf( "[preview-render] transient film-dims override (16x16 preview)\n" );
	{
		const std::string docBeforePreview = session->ReadDocument();

		AgentRenderParams p;
		p.width  = 16;
		p.height = 16;
		AgentRenderResult rr = session->Render( p );
		Check( rr.ok, "Render(width=16,height=16) succeeded" );
		Check( rr.width == 16 && rr.height == 16, "render dims honour the 16x16 preview override" );
		Check( rr.previewWidth == 16 && rr.previewHeight == 16, "previewWidth/previewHeight echo 16x16" );
		unsigned int pw = 0, ph = 0;
		Check( ReadPngDims( rr.png, pw, ph ), "preview PNG IHDR is present" );
		Check( pw == 16 && ph == 16, "preview PNG IHDR dims are 16x16" );

		// THE HARD CONTRACT: a previewed render must not mutate the retained
		// Document AT ALL (RED-PROVEN below by checking this catches a
		// regression: if the override leaked into the Document, this would
		// differ by a width/height param on the film chunk).
		Check( session->ReadDocument() == docBeforePreview,
		       "ReadDocument() is BYTE-IDENTICAL across a dims-overridden preview render" );

		// Restoration: a follow-up render with NO override is back at the
		// Document's authored 24x24 (the Job-level Film dims were restored,
		// not just the Document).
		AgentRenderResult rr2 = session->Render( -1 );
		Check( rr2.ok, "follow-up unoverridden Render() succeeded" );
		Check( rr2.width == 24 && rr2.height == 24,
		       "follow-up render is back at the Document's authored 24x24 (film-dims override was TRANSIENT)" );
	}

	//----------------------------------------------------------------------
	// Preview-render: width/height clamping.  Out-of-range requests are
	// CLAMPED (never rejected) to [16,512] -- AgentRpc.cpp does the
	// clamping before it reaches AgentSession, so drive that layer here via
	// a second AgentSession sharing the same head is unnecessary; the
	// clamp is exercised directly by requesting an in-range-but-boundary
	// value (16, the floor) and confirming it round-trips exactly (the
	// clamp logic itself is unit-level AgentRpc.cpp behaviour, exercised
	// end-to-end by the wire test below).
	//----------------------------------------------------------------------
	std::printf( "[preview-render] boundary dims (16x16 floor) round-trip exactly\n" );
	{
		AgentRenderParams p;
		p.width  = 16;
		p.height = 16;
		AgentRenderResult rr = session->Render( p );
		Check( rr.ok && rr.width == 16 && rr.height == 16, "16x16 (the clamp floor) renders at exactly 16x16" );
	}

	//----------------------------------------------------------------------
	// read_image maxEdge: downscale on read (no re-render).  Render at the
	// Document's authored 24x24, then ReadImage with maxEdge=8: the long
	// edge must be clamped to 8, aspect preserved (source is square), PNG
	// signature valid, and the byte count should shrink vs the un-clamped
	// read.
	//----------------------------------------------------------------------
	std::printf( "[read_image] maxEdge downscales the cached image (no re-render)\n" );
	{
		AgentRenderResult rr = session->Render( -1 );
		Check( rr.ok && rr.width == 24 && rr.height == 24, "full-res render (24x24) for the maxEdge test" );

		unsigned int fullW = 0, fullH = 0;
		std::vector<unsigned char> fullBytes = session->ReadImage( /*maxEdge=*/0, fullW, fullH );
		Check( HasPngSignature( fullBytes ), "ReadImage(maxEdge=0) PNG carries a valid signature" );
		Check( fullW == 24 && fullH == 24, "ReadImage(maxEdge=0) reports the native 24x24 dims (back-compat)" );
		Check( fullBytes == rr.png, "ReadImage(maxEdge=0) returns the SAME bytes as the render (legacy behaviour)" );

		unsigned int smallW = 0, smallH = 0;
		std::vector<unsigned char> smallBytes = session->ReadImage( /*maxEdge=*/8, smallW, smallH );
		Check( HasPngSignature( smallBytes ), "ReadImage(maxEdge=8) PNG carries a valid signature" );
		Check( smallW == 8 && smallH == 8, "ReadImage(maxEdge=8) downscales the square 24x24 source to 8x8" );
		Check( smallBytes.size() < fullBytes.size(),
		       "the downscaled PNG is smaller than the full-resolution PNG" );

		// Never-upscale: requesting a maxEdge LARGER than the native image
		// returns the SAME dims (and bytes) as the unbounded read.
		unsigned int bigW = 0, bigH = 0;
		std::vector<unsigned char> bigBytes = session->ReadImage( /*maxEdge=*/1024, bigW, bigH );
		Check( bigW == 24 && bigH == 24, "maxEdge larger than the source never upscales (stays at 24x24)" );
		Check( bigBytes == fullBytes, "a maxEdge above the native size returns the SAME bytes as the unbounded read" );
	}

	std::printf( "=== AgentProposeRenderTest (core): %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// A separate main-body helper: the camera-override tests need to
// introspect the ACTIVE camera's properties directly (before/after) to
// prove restoration, which requires holding our OWN Job* (AgentSession
// owns its Job privately with no accessor) -- so this second scene uses
// AgentSession::WrapJob over a locally-owned Job, mirroring the
// AgentLiveCommitTest pattern.
//////////////////////////////////////////////////////////////////////
static void RunCameraOverrideTests()
{
	std::printf( "=== AgentProposeRenderTest: camera-pose override (preview-render) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_camera_override.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the camera-override scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene directly (for camera introspection)" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	// Resolve the active camera directly so we can introspect its
	// properties before/after a camera-overridden render.
	ICameraManager* cams = pJob->GetCameras();
	const std::string activeName = pJob->GetActiveCameraName();
	ICamera* cam = ( cams && !activeName.empty() ) ? cams->GetItem( activeName.c_str() ) : nullptr;
	Check( cam != nullptr, "the active camera resolves for introspection" );
	if( !cam ) { pJob->release(); return; }

	const String origLocation = CameraIntrospection::GetPropertyValue( *cam, String( "location" ) );
	const String origLookAt   = CameraIntrospection::GetPropertyValue( *cam, String( "lookat" ) );
	const String origUp       = CameraIntrospection::GetPropertyValue( *cam, String( "up" ) );
	const String origFov      = CameraIntrospection::GetPropertyValue( *cam, String( "fov" ) );
	std::printf( "  original camera: location=\"%s\" lookat=\"%s\" up=\"%s\" fov=\"%s\"\n",
	             origLocation.c_str(), origLookAt.c_str(), origUp.c_str(), origFov.c_str() );

	//----------------------------------------------------------------------
	// Baseline render from the AUTHORED camera angle.
	//----------------------------------------------------------------------
	AgentRenderResult baseline = session->Render( -1 );
	Check( baseline.ok, "baseline (unoverridden) render succeeded" );

	//----------------------------------------------------------------------
	// Render from a markedly DIFFERENT angle (side-on instead of head-on) --
	// the money assertion: the two renders' channel means must differ by
	// more than the same-head MC noise floor established in the main test
	// above (this scene is identical, so we re-establish a local floor via
	// two overridden renders from the SAME new angle).
	//----------------------------------------------------------------------
	std::printf( "[preview-render] camera override renders from a different angle\n" );
	AgentRenderParams sideParams;
	sideParams.camera.hasLocation = true;
	sideParams.camera.location    = "3.5 0 0";   // 90 degrees around from the authored +Z view
	sideParams.camera.hasLookAt   = true;
	sideParams.camera.lookAt      = "0 0 0";
	AgentRenderResult side1 = session->Render( sideParams );
	Check( side1.ok, "side-angle render (camera override) succeeded" );
	Check( side1.cameraOverridden, "cameraOverridden==true when a camera override was applied" );

	AgentRenderResult side2 = session->Render( sideParams );
	Check( side2.ok, "second side-angle render (same override) succeeded" );
	const double sideNoiseFloor =
		std::fabs( side2.meanR - side1.meanR ) +
		std::fabs( side2.meanG - side1.meanG ) +
		std::fabs( side2.meanB - side1.meanB );

	const double angleShift =
		std::fabs( side1.meanR - baseline.meanR ) +
		std::fabs( side1.meanG - baseline.meanG ) +
		std::fabs( side1.meanB - baseline.meanB );
	std::printf( "  angleShift=%.5f  (same-angle noise floor=%.5f)\n", angleShift, sideNoiseFloor );
	Check( angleShift > 10.0 * sideNoiseFloor + 0.002,
	       "a different camera angle shifts the channel means FAR beyond the same-angle noise floor" );

	//----------------------------------------------------------------------
	// Restoration: the active camera's properties must be IDENTICAL to
	// their pre-render values after EVERY Render call, overridden or not.
	//----------------------------------------------------------------------
	std::printf( "[preview-render] camera properties are RESTORED after an overridden render\n" );
	const String afterLocation = CameraIntrospection::GetPropertyValue( *cam, String( "location" ) );
	const String afterLookAt   = CameraIntrospection::GetPropertyValue( *cam, String( "lookat" ) );
	const String afterUp       = CameraIntrospection::GetPropertyValue( *cam, String( "up" ) );
	const String afterFov      = CameraIntrospection::GetPropertyValue( *cam, String( "fov" ) );
	Check( std::string( afterLocation.c_str() ) == std::string( origLocation.c_str() ),
	       "camera 'location' restored to its pre-render value after a camera-overridden render" );
	Check( std::string( afterLookAt.c_str() ) == std::string( origLookAt.c_str() ),
	       "camera 'lookat' restored to its pre-render value after a camera-overridden render" );
	Check( std::string( afterUp.c_str() ) == std::string( origUp.c_str() ),
	       "camera 'up' restored to its pre-render value (was not part of the override -- must be untouched too)" );
	Check( std::string( afterFov.c_str() ) == std::string( origFov.c_str() ),
	       "camera 'fov' restored to its pre-render value (was not part of the override)" );

	// A follow-up UNOVERRIDDEN render is back to the baseline angle (channel
	// means agree with the ORIGINAL baseline within noise, not the side angle).
	AgentRenderResult after = session->Render( -1 );
	Check( after.ok, "follow-up unoverridden render succeeded" );
	Check( !after.cameraOverridden, "follow-up render reports cameraOverridden==false (no override requested)" );
	const double backToBaseline =
		std::fabs( after.meanR - baseline.meanR ) +
		std::fabs( after.meanG - baseline.meanG ) +
		std::fabs( after.meanB - baseline.meanB );
	std::printf( "  backToBaseline drift=%.5f (side angle shift was %.5f)\n", backToBaseline, angleShift );
	Check( backToBaseline < angleShift,
	       "the follow-up unoverridden render is back near the ORIGINAL baseline angle, not the side angle" );

	//----------------------------------------------------------------------
	// RED-PROVE the restoration test: verify Render(-1) (no override
	// requested at all) leaves the camera untouched too -- confirming the
	// assertions above are actually discriminating (they would catch a
	// "forgot to restore" regression because they compare against a KNOWN
	// original value captured before ANY override ran).
	//----------------------------------------------------------------------
	Check( std::string( CameraIntrospection::GetPropertyValue( *cam, String( "location" ) ).c_str() )
	           == std::string( origLocation.c_str() ),
	       "RED-PROVE: camera location still matches the ORIGINAL captured value (not just the last override's target)" );

	std::printf( "=== camera-pose override tests: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// P1-A red-prove: a Job subclass whose Rasterize() throws AFTER the base
// Job::Rasterize() has genuinely run (so the sink really got a completed
// image and the camera/film overrides were really applied) -- mirrors the
// CodeThreeJob technique in AgentLiveCommitTest.cpp (override a single
// virtual, call the base for real, then corrupt/interrupt the outcome) so
// the exception fires from the EXACT call site AgentSession::RenderCore_
// invokes (mJob->Rasterize()), proving the RAII guard's restore runs on a
// real unwind through that real call, not a synthetic short-circuit.
//////////////////////////////////////////////////////////////////////
class ThrowingRasterizeJob : public Job
{
public:
	ThrowingRasterizeJob() : Job(), mThrowOnRasterize( true ) {}
	void SetThrowOnRasterize( bool on ) { mThrowOnRasterize = on; }
	bool Rasterize() override
	{
		const bool base = Job::Rasterize();   // the real render actually runs
		if( mThrowOnRasterize ) {
			throw std::runtime_error( "ThrowingRasterizeJob: simulated OIDN-class throw from Rasterize()" );
		}
		return base;
	}
private:
	bool mThrowOnRasterize;
};

static void RunRestoreOnThrowTest()
{
	std::printf( "=== AgentProposeRenderTest: P1-A RAII restore-on-throw (Rasterize() throws) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_throw_restore.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the throw-restore scene to a temp file" );

	ThrowingRasterizeJob* pJob = new ThrowingRasterizeJob();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ThrowingRasterizeJob loads the native-v7 scene via the CST path" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the throwing Job" );
	if( !session ) { pJob->release(); return; }

	ICameraManager* cams = pJob->GetCameras();
	const std::string activeName = pJob->GetActiveCameraName();
	ICamera* cam = ( cams && !activeName.empty() ) ? cams->GetItem( activeName.c_str() ) : nullptr;
	Check( cam != nullptr, "the active camera resolves for introspection" );
	if( !cam ) { pJob->release(); return; }

	const String origLocation = CameraIntrospection::GetPropertyValue( *cam, String( "location" ) );
	const IScenePriv* scenePriv = pJob->GetScene();
	const IFilm* curFilm = scenePriv ? scenePriv->GetFilm() : nullptr;
	Check( curFilm != nullptr, "the scene has a Film to capture pre-override dims from" );
	const unsigned int origW = curFilm ? curFilm->GetWidth()  : 0;
	const unsigned int origH = curFilm ? curFilm->GetHeight() : 0;
	Check( origW == 24 && origH == 24, "pre-override Film dims are the Document's authored 24x24" );

	//----------------------------------------------------------------------
	// Render WITH both a film-dims override and a camera-pose override so
	// BOTH restore paths (camera fields + film dims) are exercised by the
	// SAME throwing call.  mJob->Rasterize() throws; AgentRpc's caller-side
	// dispatch (not exercised directly here -- this is the AgentSession
	// layer) would turn an escaping exception into a clean -32603, but the
	// money assertion is what happens to the CAMERA/FILM STATE: it must be
	// restored EVEN THOUGH the exception unwound past the old straight-line
	// restore code that used to sit after Rasterize().
	//----------------------------------------------------------------------
	AgentRenderParams p;
	p.width  = 16;
	p.height = 16;
	p.camera.hasLocation = true;
	p.camera.location    = "3.5 0 0";
	p.camera.hasLookAt   = true;
	p.camera.lookAt      = "0 0 0";

	// Model-B F2 slice S2a doc-truth fix: Render() no longer lets a thrown
	// exception escape as a raw C++ exception -- RenderCore_ now catches it
	// (at every call shape that can run doRenderWork(), including this
	// headless direct-call path) and reports it as an ordinary ok=false
	// result CARRYING the renderJobId the failed render was assigned (see
	// AgentSession.cpp's S1-delta doc-truth-fix comment).  RED-PROVE the
	// seam actually fires via the result instead of a caught exception.
	const AgentRenderResult thrownResult = session->Render( p );
	Check( !thrownResult.ok, "Render() with a throwing Rasterize() reports ok=false (RED-PROVE the seam actually fires)" );
	Check( thrownResult.renderJobId != 0, "S1-delta doc-truth fix: a FAILED (thrown) render still carries a real, nonzero renderJobId" );
	Check( thrownResult.message.find( "ThrowingRasterizeJob" ) != std::string::npos,
	       "the failure message names the thrown exception's text" );

	//----------------------------------------------------------------------
	// THE MONEY ASSERTION: despite the exception, both overrides are
	// restored -- the RAII guard's destructor ran during unwinding.
	//----------------------------------------------------------------------
	const String afterLocation = CameraIntrospection::GetPropertyValue( *cam, String( "location" ) );
	Check( std::string( afterLocation.c_str() ) == std::string( origLocation.c_str() ),
	       "P1-A: camera 'location' is RESTORED after Rasterize() throws (RAII guard ran on unwind)" );

	const IScenePriv* scenePrivAfter = pJob->GetScene();
	const IFilm* filmAfter = scenePrivAfter ? scenePrivAfter->GetFilm() : nullptr;
	Check( filmAfter != nullptr, "the scene still has a Film after the throwing render" );
	Check( filmAfter && filmAfter->GetWidth() == origW && filmAfter->GetHeight() == origH,
	       "P1-A: Film dims are RESTORED to the pre-override 24x24 after Rasterize() throws (RAII guard ran on unwind)" );

	//----------------------------------------------------------------------
	// RED-PROVE the red-prove: confirm the session is still USABLE after the
	// exception (a subsequent non-throwing render succeeds cleanly) -- the
	// guard's restore did not itself leave the Job in a broken state.
	//----------------------------------------------------------------------
	pJob->SetThrowOnRasterize( false );
	AgentRenderResult clean = session->Render( -1 );
	Check( clean.ok, "a follow-up non-throwing Render() succeeds after the earlier throw+restore (session still usable)" );
	Check( clean.width == 24 && clean.height == 24, "follow-up render is at the authored 24x24 (no lingering override)" );

	std::printf( "=== P1-A RAII restore-on-throw: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// P1-B: malformed camera vectors must be rejected CLEANLY, not silently
// no-op while cameraOverridden reports true.  Exercises BOTH layers:
//
//   * AgentSession's belt-and-braces (this file, via WrapJob + a HOSTILE
//     string passed directly through the C++ API, bypassing AgentRpc's
//     validation entirely) -- proves fail-loud holds even for a caller
//     that skips the wire layer.
//   * The RPC-layer shape validation (AgentRpc.cpp ParseCameraOverrideParam)
//     is exercised end-to-end by AgentChatLoopTest.cpp / the JSON-RPC wire
//     tests, not duplicated here -- this file is the AgentSession-level
//     (C++ API) surface.
//////////////////////////////////////////////////////////////////////
static void RunMalformedCameraOverrideTests()
{
	std::printf( "=== AgentProposeRenderTest: P1-B fail-loud camera-override validation (AgentSession belt-and-braces) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_malformed_camera.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the malformed-camera scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene directly" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	ICameraManager* cams = pJob->GetCameras();
	const std::string activeName = pJob->GetActiveCameraName();
	ICamera* cam = ( cams && !activeName.empty() ) ? cams->GetItem( activeName.c_str() ) : nullptr;
	Check( cam != nullptr, "the active camera resolves for introspection" );
	if( !cam ) { pJob->release(); return; }

	const String origLocation = CameraIntrospection::GetPropertyValue( *cam, String( "location" ) );
	const String origLookAt   = CameraIntrospection::GetPropertyValue( *cam, String( "lookat" ) );
	const String origUp       = CameraIntrospection::GetPropertyValue( *cam, String( "up" ) );
	const String origFov      = CameraIntrospection::GetPropertyValue( *cam, String( "fov" ) );

	// RED-PROVE THE BUG THIS FIXES: before the fix, CameraIntrospection::
	// SetProperty's bool return was discarded (captureAndSet ignored it) and
	// overrodeCamera = !capturedCam.empty() counted ATTEMPTS, not successes
	// -- so a hostile string like "5 5" (2 tokens, ParseVec3's sscanf wants
	// 3) would sail through as ok:true, cameraOverridden:true, with the
	// camera actually UNCHANGED (SetProperty's ParseVec3 failed and
	// returned early before calling SetLocation). That is precisely the
	// false observation the feature exists to prevent. The fixed contract:
	// ok must be FALSE and the camera must be untouched.
	auto CheckMalformed = [&]( const std::string& label,
	                            bool hasLoc, const std::string& loc,
	                            bool hasLookAt, const std::string& lookAt,
	                            bool hasUp, const std::string& up,
	                            bool hasFov, const std::string& fov )
	{
		AgentRenderParams p;
		p.camera.hasLocation = hasLoc;   p.camera.location = loc;
		p.camera.hasLookAt   = hasLookAt; p.camera.lookAt   = lookAt;
		p.camera.hasUp       = hasUp;     p.camera.up       = up;
		p.camera.hasFov      = hasFov;    p.camera.fov      = fov;

		AgentRenderResult rr = session->Render( p );
		Check( !rr.ok, ( "[" + label + "] malformed camera override -> ok==false (fail-loud)" ).c_str() );
		Check( !rr.cameraOverridden, ( "[" + label + "] malformed camera override -> cameraOverridden==false (no false-positive)" ).c_str() );
		Check( !rr.message.empty(), ( "[" + label + "] malformed camera override -> a non-empty message explains the failure" ).c_str() );

		Check( std::string( CameraIntrospection::GetPropertyValue( *cam, String( "location" ) ).c_str() ) == std::string( origLocation.c_str() ),
		       ( "[" + label + "] camera 'location' is UNCHANGED after a rejected override" ).c_str() );
		Check( std::string( CameraIntrospection::GetPropertyValue( *cam, String( "lookat" ) ).c_str() ) == std::string( origLookAt.c_str() ),
		       ( "[" + label + "] camera 'lookat' is UNCHANGED after a rejected override" ).c_str() );
		Check( std::string( CameraIntrospection::GetPropertyValue( *cam, String( "up" ) ).c_str() ) == std::string( origUp.c_str() ),
		       ( "[" + label + "] camera 'up' is UNCHANGED after a rejected override" ).c_str() );
		Check( std::string( CameraIntrospection::GetPropertyValue( *cam, String( "fov" ) ).c_str() ) == std::string( origFov.c_str() ),
		       ( "[" + label + "] camera 'fov' is UNCHANGED after a rejected override" ).c_str() );
	};

	std::printf( "[fail-loud] location \"5 5\" (2 tokens -- the exact reported false-positive)\n" );
	CheckMalformed( "location 2-token", true, "5 5", true, "0 0 0", false, "", false, "" );

	std::printf( "[fail-loud] location \"abc def ghi\" (non-numeric)\n" );
	CheckMalformed( "location non-numeric", true, "abc def ghi", true, "0 0 0", false, "", false, "" );

	// NOTE (honest gap, by design): "1 2 3 4" (trailing-token garbage) is
	// NOT a case this AgentSession-level belt-and-braces layer can catch on
	// its own -- CameraIntrospection::SetProperty's ParseVec3 is a plain
	// sscanf( "%lf %lf %lf" ), which reads the first 3 tokens and silently
	// ignores trailing content, so SetProperty returns TRUE (a real,
	// successful apply) for that string. The belt-and-braces guard here
	// only fires when SetProperty itself reports failure; it cannot
	// retroactively invent a rejection SetProperty didn't produce. The
	// EXACT-3-token shape check that catches "1 2 3 4" lives at the RPC
	// layer (AgentRpc.cpp ParseCameraOverrideParam / ValidateVec3Shape),
	// which validates the wire string BEFORE it ever reaches AgentSession
	// -- see AgentFirstSliceTest.cpp's "camera vector SHAPE validation
	// (P1-B)" block for that coverage. A direct C++ caller that bypasses
	// the RPC layer (as this test does) and hands SetProperty a
	// trailing-garbage string gets exactly what SetProperty gives it: a
	// clean apply of the first 3 components. That is SetProperty's
	// documented contract, not a bug this layer is responsible for.

	std::printf( "[fail-loud] location \"\" (empty)\n" );
	CheckMalformed( "location empty", true, "", true, "0 0 0", false, "", false, "" );

	std::printf( "[fail-loud] up malformed while location/lookat are VALID (must not silently keep original up)\n" );
	CheckMalformed( "up malformed", true, "3.5 0 0", true, "0 0 0", true, "9 9", false, "" );

	//------------------------------------------------------------------
	// Sanity: after all those rejections, a CLEAN override still works --
	// proves the session was never left in a broken state by a rejected
	// override.
	//------------------------------------------------------------------
	std::printf( "[fail-loud] a clean override still works after several rejections\n" );
	{
		AgentRenderParams good;
		good.camera.hasLocation = true;  good.camera.location = "3.5 0 0";
		good.camera.hasLookAt   = true;  good.camera.lookAt   = "0 0 0";
		AgentRenderResult rr = session->Render( good );
		Check( rr.ok, "a well-formed camera override renders successfully after prior rejections" );
		Check( rr.cameraOverridden, "a well-formed camera override reports cameraOverridden==true" );
	}
	Check( std::string( CameraIntrospection::GetPropertyValue( *cam, String( "location" ) ).c_str() ) == std::string( origLocation.c_str() ),
	       "camera 'location' restored to ORIGINAL after the clean override's render too" );

	std::printf( "=== P1-B fail-loud camera-override validation: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// Model-B F2 slice S1: render IDENTITY (RenderJobId + coordinator
// bookkeeping), ZERO behavior change.  Four sub-tests:
//
//   (a) Headless, no controller -- two sequential Render() calls yield
//       DISTINCT, MONOTONICALLY INCREASING renderJobId (the session-local
//       counter documented on AgentRenderResult::renderJobId).
//
//   (b) part 1, LIVE / RenderClass::Interactive -- the controller's OWN
//       interactive RenderLoop pass is genuinely observable mid-flight
//       from another thread (DoOneRenderPass runs OUTSIDE mMutex, between
//       the two lock_guard bookkeeping blocks): a background poll catches
//       CurrentRenderJob() reporting {active=true, RenderClass::
//       Interactive} while a slowed simulated pass is in flight, then
//       {active=false} after.  A RenderJobIdTestController subclass
//       (mirroring AgentLiveCommitTest.cpp's TestController) overrides
//       DoOneRenderPass with a cancel-checked simulated render so this is
//       a REAL render thread, not a mock.
//
//   (b) part 2, LIVE / RenderClass::AgentPreview -- a render routed
//       through RunPreviewRenderParked (forced via a camera-pose
//       override, matching AgentSession::RenderCore_'s documented
//       routing rule).  RunPreviewRenderParked holds mMutex across its
//       WHOLE call BY DESIGN (the record lives under the SAME lock that
//       critical section already took -- this slice's zero-new-
//       synchronization mandate), so its active window is NOT observable
//       from another thread without that thread blocking on the same
//       mutex until the render finishes.  What IS safely checked: the
//       result's renderJobId is nonzero and shares the SAME monotonic
//       counter as part 1 (greater than whatever id the controller had
//       already assigned), and the post-render snapshot correctly
//       reports {inactive, that id, RenderClass::AgentPreview}.
//
//   (c) RED-PROVE: a deliberately "unwired" read of renderJobId (as if
//       the id were never threaded through) would show 0 for both
//       renders in (a) -- assert the SECOND call's id is nonzero AND
//       different from the first, so a regression that drops the
//       threading (leaves renderJobId at its 0 default) fails this test.
//////////////////////////////////////////////////////////////////////

// Mirrors AgentLiveCommitTest.cpp's TestController: a real render thread
// whose DoOneRenderPass simulates work in cancel-checked slices so the
// interactive loop cycles without needing a live IRasterizer, and so a
// RunPreviewRenderParked call from another thread can be observed as
// "active" via CurrentRenderJob() while it holds the park.
class RenderJobIdTestController : public SceneEditController
{
public:
	RenderJobIdTestController( IJobPriv& job, unsigned int simulatedRenderMs = 20 )
	: SceneEditController( job, /*interactiveRasterizer*/0 )
	, mSimulatedRenderMs( simulatedRenderMs )
	{}

protected:
	void DoOneRenderPass() override
	{
		const unsigned int sliceMs = 2;
		const unsigned int slices  = ( mSimulatedRenderMs + sliceMs - 1 ) / sliceMs;
		for( unsigned int i = 0; i < slices; ++i )
		{
			if( IsCancelRequested() ) return;
			std::this_thread::sleep_for( std::chrono::milliseconds( sliceMs ) );
		}
	}

private:
	unsigned int mSimulatedRenderMs;
};

static void RunRenderJobIdTests()
{
	std::printf( "=== AgentProposeRenderTest: Model-B F2 slice S1 (RenderJobId + coordinator bookkeeping) ===\n" );

	//----------------------------------------------------------------------
	// (a) + (c): headless, no controller -- distinct, monotonically
	// increasing ids across sequential renders; RED-PROVE that both are
	// nonzero and differ (catches "id never threaded, stays at 0 default").
	//----------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_renderjobid_headless.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the headless render-id scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene directly (headless render-id test)" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the headless Job (render-id test)" );
		if( session )
		{
			Check( !session->HasController(), "headless session has no attached controller (sets up the no-coordinator id path)" );

			const AgentRenderResult r1 = session->Render( -1 );
			Check( r1.ok, "headless render #1 succeeds" );
			const AgentRenderResult r2 = session->Render( -1 );
			Check( r2.ok, "headless render #2 succeeds" );

			// RED-PROVE (c): both must be nonzero (a dropped-threading
			// regression leaves renderJobId at its 0 default on BOTH
			// calls, which would pass a weaker "r2 != r1" check alone if
			// both were 0 -- so assert nonzero explicitly too).
			Check( r1.renderJobId != 0, "headless render #1 reports a NONZERO renderJobId (RED-PROVE: catches an unthreaded id stuck at the 0 default)" );
			Check( r2.renderJobId != 0, "headless render #2 reports a NONZERO renderJobId" );
			Check( r2.renderJobId != r1.renderJobId, "sequential headless renders yield DISTINCT renderJobId values" );
			Check( r2.renderJobId > r1.renderJobId, "sequential headless renders yield MONOTONICALLY INCREASING renderJobId values" );

			// A third call keeps the sequence going (not just a 1->2 fluke).
			const AgentRenderResult r3 = session->Render( -1 );
			Check( r3.ok, "headless render #3 succeeds" );
			Check( r3.renderJobId > r2.renderJobId, "a third headless render continues the monotonic sequence" );
		}

		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//----------------------------------------------------------------------
	// (b) part 1: the INTERACTIVE render class's active window IS
	// genuinely observable from another thread -- DoOneRenderPass runs
	// OUTSIDE mMutex (between the two lock_guard bookkeeping blocks in
	// RenderLoop; see SceneEditController.cpp), unlike
	// RunPreviewRenderParked's agent-preview window, which holds mMutex
	// for its ENTIRE duration (by design -- the record lives under the
	// SAME lock that critical section already took, per this slice's
	// zero-new-synchronization mandate). RenderJobIdTestController's
	// slowed DoOneRenderPass (mSimulatedRenderMs) gives a wide,
	// deterministic window for the poll below to land in.
	//----------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_renderjobid_interactive.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the interactive render-id scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene directly (interactive render-id test)" );

		RenderJobIdTestController controller( *pJob, /*simulatedRenderMs*/300 );
		controller.Start();

		// Poll CurrentRenderJob() for the window where the controller's
		// OWN initial render pass (RenderLoop's Start-up "show something"
		// pass) is active with RenderClass::Interactive -- the 300ms
		// simulated pass gives a wide margin for the poll (started
		// immediately after Start(), before waiting for completion) to
		// land inside it.
		bool observedInteractiveActive = false;
		SceneEditController::RenderJobStatus observedInteractiveStatus;
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 5000 );
			while( controller.ForTest_GetRenderCount() == 0 && std::chrono::steady_clock::now() < deadline )
			{
				const SceneEditController::RenderJobStatus st = controller.CurrentRenderJob();
				if( st.active && st.renderClass == SceneEditController::RenderClass::Interactive )
				{
					observedInteractiveActive = true;
					observedInteractiveStatus = st;
					break;
				}
				std::this_thread::yield();
			}
		}
		Check( controller.ForTest_WaitForRenders( 1, 3000 ), "the controller's initial interactive render completes" );

		Check( observedInteractiveActive, "CurrentRenderJob() caught the INTERACTIVE render ACTIVE mid-flight (RenderClass::Interactive)" );
		if( observedInteractiveActive )
		{
			Check( observedInteractiveStatus.id != SceneEditController::kInvalidRenderJobId,
			       "the observed interactive job has a NONZERO id" );
		}

		const SceneEditController::RenderJobStatus afterInteractive = controller.CurrentRenderJob();
		Check( !afterInteractive.active, "CurrentRenderJob() reports the job INACTIVE after the interactive pass completes" );
		if( observedInteractiveActive )
		{
			Check( afterInteractive.id == observedInteractiveStatus.id,
			       "the post-pass snapshot still names the SAME renderJobId observed mid-flight (stale-but-informational)" );
		}

		controller.Stop();
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//----------------------------------------------------------------------
	// (b) part 2: the AGENT-PREVIEW class -- a controller is attached, and
	// the render actually routes through RunPreviewRenderParked (forced
	// via a camera-pose override, matching AgentSession::RenderCore_'s
	// documented routing rule).  Because RunPreviewRenderParked holds
	// mMutex across the WHOLE call (by design -- see part 1's doc), its
	// "active" window is NOT observable from another thread without that
	// thread itself blocking on the same mutex until the render finishes
	// -- so the assertions here are the ones that ARE meaningful: the
	// result's renderJobId is nonzero, DISTINCT from (and greater than)
	// every id assigned so far on this controller (proving it shares the
	// SAME monotonic counter as the interactive pass above -- one counter
	// per controller, not a separate one per class), and the post-render
	// snapshot correctly reports {inactive, that id, RenderClass::
	// AgentPreview}.
	//----------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_renderjobid_live.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the live render-id scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene directly (live render-id test)" );

		RenderJobIdTestController controller( *pJob, /*simulatedRenderMs*/5 );
		controller.Start();
		Check( controller.ForTest_WaitForRenders( 1, 2000 ), "controller's initial interactive render fires" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the live Job" );
		if( session )
		{
			session->AttachController( &controller );
			Check( session->HasController(), "live session reports an attached controller" );

			const SceneEditController::RenderJobStatus before = controller.CurrentRenderJob();

			AgentRenderParams p;
			p.camera.hasLocation = true;
			p.camera.location    = "3.5 0 0";
			p.camera.hasLookAt   = true;
			p.camera.lookAt      = "0 0 0";
			const AgentRenderResult liveResult = session->Render( p );

			Check( liveResult.ok, "live camera-override render succeeds" );
			Check( liveResult.renderJobId != 0, "live render reports a NONZERO renderJobId" );
			Check( liveResult.renderJobId > before.id,
			       "the agent-preview render's id is GREATER than whatever the controller's counter last assigned (ONE shared monotonic counter across BOTH render classes)" );

			// AFTER the render completes, CurrentRenderJob() reports the
			// job inactive, naming the SAME id/class the result reported --
			// this snapshot IS safely observable (no race: the render
			// thread released mMutex before Render() returned).
			const SceneEditController::RenderJobStatus after = controller.CurrentRenderJob();
			Check( !after.active, "CurrentRenderJob() reports the job INACTIVE after the render completes" );
			Check( after.id == liveResult.renderJobId,
			       "CurrentRenderJob()'s post-render snapshot names the SAME renderJobId the result reported" );
			Check( after.renderClass == SceneEditController::RenderClass::AgentPreview,
			       "CurrentRenderJob()'s post-render snapshot reports RenderClass::AgentPreview for the agent-routed render" );

			session->AttachController( nullptr );
		}

		controller.Stop();
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	std::printf( "=== Model-B F2 slice S1: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// Pre-S2 hardening (two review-flagged items that must land BEFORE S2
// builds Status(jobId) on this bookkeeping):
//
//   ITEM 1 (P2): a throw out of RunPreviewRenderParked's `fn` must NOT
//     leave CurrentRenderJob().active stuck at true forever.  Reuses the
//     ThrowingRasterizeJob seam (a real mJob->Rasterize() throw site, the
//     same one the P1-A restore-on-throw test exercises) but THIS time
//     with a controller ATTACHED and a camera override, so the render
//     actually routes through RunPreviewRenderParked (matching
//     AgentSession::RenderCore_'s documented routing rule) rather than
//     running headless.
//
//   ITEM 2 (P3, S2-blocking): the controller's coordinator-tracked ids
//     and AgentSession's session-local ids must be PROVABLY DISJOINT (by
//     parity: coordinator EVEN, session-local ODD), and a session-local
//     id must round-trip through the JSON wire serializer EXACTLY (not
//     just "some number came out the other end").
//////////////////////////////////////////////////////////////////////
static void RunPreS2HardeningTests()
{
	std::printf( "=== AgentProposeRenderTest: pre-S2 hardening (exception-safe active flip + disjoint render-job ids) ===\n" );

	//----------------------------------------------------------------------
	// ITEM 1: throw out of a CONTROLLER-ATTACHED, RunPreviewRenderParked-
	// routed render must leave CurrentRenderJob().active == false.
	//----------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_throw_active_flip.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the throw-active-flip scene to a temp file" );

		ThrowingRasterizeJob* pJob = new ThrowingRasterizeJob();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ThrowingRasterizeJob loads the native-v7 scene via the CST path (active-flip test)" );

		RenderJobIdTestController controller( *pJob, /*simulatedRenderMs*/5 );
		controller.Start();
		Check( controller.ForTest_WaitForRenders( 1, 2000 ), "controller's initial interactive render fires (active-flip test)" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the throwing Job (active-flip test)" );
		if( session )
		{
			session->AttachController( &controller );
			Check( session->HasController(), "active-flip session reports an attached controller" );

			// Force the RunPreviewRenderParked route (matches
			// AgentSession::RenderCore_'s documented routing rule: a
			// controller attached + a requested override).
			AgentRenderParams p;
			p.camera.hasLocation = true;
			p.camera.location    = "3.5 0 0";
			p.camera.hasLookAt   = true;
			p.camera.lookAt      = "0 0 0";

			// Model-B F2 slice S2a doc-truth fix: the thrown exception is now
			// caught by RenderCore_ and reported as ok=false + a nonzero
			// renderJobId (see AgentSession.cpp's S1-delta comment) rather
			// than escaping as a raw C++ exception -- RED-PROVE the seam via
			// the result.
			const AgentRenderResult throwResult = session->Render( p );
			Check( !throwResult.ok, "controller-routed Render() with a throwing Rasterize() reports ok=false (RED-PROVE the seam fires)" );
			Check( throwResult.renderJobId != 0, "S1-delta doc-truth fix: the failed controller-routed render still carries a real, nonzero renderJobId" );

			// THE MONEY ASSERTION: unfixed code leaves `active` stuck true
			// forever after this throw (mCurrentRenderJob.active = false
			// sat AFTER fn() in plain sequential code, so the throw skips
			// it) -- this assertion FAILS against that code and PASSES
			// once the RAII guard flips `active` false on every exit.
			const SceneEditController::RenderJobStatus afterThrow = controller.CurrentRenderJob();
			Check( !afterThrow.active,
			       "ITEM 1 MONEY ASSERTION: CurrentRenderJob().active is FALSE after fn() throws out of RunPreviewRenderParked (RAII guard ran on unwind)" );
			Check( afterThrow.id == throwResult.renderJobId,
			       "the controller's post-throw job record names the SAME id the failed result reported" );

			// RED-PROVE THE RED-PROVE: the session/controller are still
			// usable after the throw -- a follow-up non-throwing render
			// succeeds AND correctly reports active again while it should,
			// then inactive again after.
			pJob->SetThrowOnRasterize( false );
			AgentRenderParams good;
			good.camera.hasLocation = true;  good.camera.location = "3.5 0 0";
			good.camera.hasLookAt   = true;  good.camera.lookAt   = "0 0 0";
			AgentRenderResult clean = session->Render( good );
			Check( clean.ok, "a follow-up non-throwing controller-routed Render() succeeds after the earlier throw" );
			const SceneEditController::RenderJobStatus afterClean = controller.CurrentRenderJob();
			Check( !afterClean.active, "CurrentRenderJob().active is false after the follow-up render completes normally too" );
			Check( afterClean.id != afterThrow.id, "the follow-up render was assigned a FRESH id (the counter kept advancing through the throw)" );

			session->AttachController( nullptr );
		}

		controller.Stop();
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//----------------------------------------------------------------------
	// ITEM 2a (Model-B F2 slice S2a REBASE): pre-S2a, a controller-attached
	// no-override render never reached RunPreviewRenderParked at all (the
	// documented pre-existing race -- see AgentSession.h's LIVE-MODE
	// SAFETY note) and so fell through to the session-local (ODD) counter.
	// S2a closes that race by routing EVERY controller-attached render --
	// override or not -- through the controller's coordinator (either
	// RunPreviewRenderParked for an override, or SubmitAgentRenderSync for
	// no override).  The updated, deliberate contract: BOTH now yield EVEN
	// (coordinator) ids, from the SAME shared monotonic counter, and the
	// session-local (ODD) counter is now reachable ONLY when no controller
	// is attached at all (headless) -- covered separately below.
	//----------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_disjoint_ids.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the disjoint-ids scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene directly (disjoint-ids test)" );

		RenderJobIdTestController controller( *pJob, /*simulatedRenderMs*/5 );
		controller.Start();
		Check( controller.ForTest_WaitForRenders( 1, 2000 ), "controller's initial interactive render fires (disjoint-ids test)" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the Job (disjoint-ids test)" );
		if( session )
		{
			session->AttachController( &controller );

			// Model-B F2 slice S2a: no override requested, but a controller
			// IS attached -- now routes through SubmitAgentRenderSync (the
			// no-override race closure), so this ALSO yields an EVEN
			// (coordinator) id, not the pre-S2a session-local ODD one.
			const AgentRenderResult noOverride = session->Render( -1 );
			Check( noOverride.ok, "controller-attached, no-override render succeeds" );
			Check( ( noOverride.renderJobId % 2 ) == 0,
			       "ITEM 2a (S2a rebase): a controller-attached render with NO override now yields an EVEN (coordinator) renderJobId -- the no-override race closure" );

			// A camera override IS requested -> routes through
			// RunPreviewRenderParked -> coordinator (EVEN) id.
			AgentRenderParams p;
			p.camera.hasLocation = true;  p.camera.location = "3.5 0 0";
			p.camera.hasLookAt   = true;  p.camera.lookAt   = "0 0 0";
			const AgentRenderResult withOverride = session->Render( p );
			Check( withOverride.ok, "controller-attached, WITH-override render succeeds" );
			Check( ( withOverride.renderJobId % 2 ) == 0,
			       "ITEM 2a: a controller-attached render WITH an override yields an EVEN (coordinator) renderJobId" );

			Check( withOverride.renderJobId != noOverride.renderJobId,
			       "ITEM 2a: the two ids are numerically distinct" );
			Check( withOverride.renderJobId > noOverride.renderJobId,
			       "ITEM 2a (S2a rebase): both draw from the SAME shared monotonic coordinator counter (strictly increasing across classes)" );

			// A second no-override render continues the SAME coordinator
			// sequence (proves this is the stable shared-counter behavior,
			// not a fluke of one call).
			const AgentRenderResult noOverride2 = session->Render( -1 );
			Check( noOverride2.ok, "second controller-attached, no-override render succeeds" );
			Check( ( noOverride2.renderJobId % 2 ) == 0, "a second no-override render also yields an EVEN (coordinator) id" );
			Check( noOverride2.renderJobId > withOverride.renderJobId, "coordinator ids keep increasing across calls, across BOTH override and no-override renders" );

			session->AttachController( nullptr );
		}

		controller.Stop();
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//----------------------------------------------------------------------
	// ITEM 2b: a session-local renderJobId round-trips through the JSON
	// wire serializer BYTE-EXACTLY.  This is the concrete red-prove for
	// the rejected high-bit-tag scheme: id | (1ULL<<63) (~9.22e18) fails
	// Json.cpp SerializeNumber's `fabs(d) < 9.0e15` exact-integer fast
	// path, falls through to %.17g (scientific notation), AND cannot even
	// round-trip through a double bit-exactly (2^63 exceeds the 53-bit
	// mantissa) -- so this test would FAIL under that scheme. The chosen
	// parity scheme keeps every realistic id comfortably inside the
	// exact-double-integer range, so this test PASSES.
	//----------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_wire_roundtrip.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the wire-roundtrip scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene directly (wire-roundtrip test)" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the Job (wire-roundtrip test)" );
		if( session )
		{
			const AgentRenderResult r = session->Render( -1 );
			Check( r.ok, "headless render for the wire-roundtrip test succeeds" );
			Check( ( r.renderJobId % 2 ) == 1, "headless session-local renderJobId is ODD" );

			// Mirror AgentRpc.cpp's exact wire encoding: MakeNumber(double).
			const RISE::Agent::JsonValue wire = RISE::Agent::JsonValue::MakeNumber( static_cast<double>( r.renderJobId ) );
			const std::string serialized = RISE::Agent::JsonSerialize( wire );

			// Must be an exact base-10 integer literal -- NOT scientific
			// notation, NOT a trailing ".0" -- so a wire client parses it
			// back as the exact same integer.
			Check( serialized.find( 'e' ) == std::string::npos && serialized.find( 'E' ) == std::string::npos,
			       "ITEM 2b: the serialized renderJobId is NOT in scientific notation" );
			Check( serialized.find( '.' ) == std::string::npos,
			       "ITEM 2b: the serialized renderJobId has no trailing fractional part" );
			Check( serialized == std::to_string( r.renderJobId ),
			       "ITEM 2b: the serialized renderJobId is the EXACT decimal string of the uint64 id" );

			// Full round-trip: parse it back and compare as a uint64,
			// bit-for-bit.
			RISE::Agent::JsonValue parsedBack;
			std::string parseErr;
			Check( RISE::Agent::JsonParse( serialized, parsedBack, parseErr ), "the serialized renderJobId re-parses as valid JSON" );
			Check( parsedBack.isNumber(), "the re-parsed value is a JSON number" );
			const std::uint64_t roundTripped = static_cast<std::uint64_t>( parsedBack.asNumber() );
			Check( roundTripped == r.renderJobId,
			       "ITEM 2b MONEY ASSERTION: the renderJobId round-trips through JsonSerialize -> JsonParse EXACTLY (byte-exact wire identity)" );
		}

		pJob->release();
		std::remove( scenePath.c_str() );
	}

	std::printf( "=== pre-S2 hardening: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// Model-B F2 slice S3 (EffectiveRenderConfig): AgentRenderParams::samples
// is no longer a documented no-op -- it is HONORED via
// IRasterizer::SetSampleCountOverride/GetSampleCountOverride (implemented
// on PixelBasedRasterizerHelper, covering the pixel-based rasterizer
// family: PT, spectral PT, BDPT, VCM) with a capture/apply/restore window
// around Rasterize() that NEVER touches the retained CST Document.  kScene
// authors `pathtracing_pel_rasterizer { samples 8 ... }`, so the pre-
// override scene-authored count is 8 throughout.
//////////////////////////////////////////////////////////////////////
static void RunSampleCountOverrideTests()
{
	std::printf( "=== AgentProposeRenderTest: Model-B F2 slice S3 sample-count override ===\n" );

	//------------------------------------------------------------------
	// (a) RED-PROVE byte-identity: ReadDocument() before/after a
	//     samples-overridden render is IDENTICAL -- the whole point of
	//     the design is that this is a LIVE rasterizer-state mutation,
	//     never a CST edit.  Without the design's non-mutating contract
	//     (the old slice-0b draft that routed samplesOverride through
	//     ProposePatch), the Document would gain a rewritten `samples`
	//     line here.
	//------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_samples_byteidentity.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the byte-identity scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (samples byte-identity test)" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the Job (samples byte-identity test)" );
		if( session )
		{
			const std::string beforeDoc = session->ReadDocument();
			Check( beforeDoc.find( "samples 8" ) != std::string::npos, "the pre-render Document authors 'samples 8'" );

			AgentRenderParams p;
			p.samples = 64;   // a big departure from the authored 8 -- if this leaked into the CST, the doc would visibly change
			const AgentRenderResult r = session->Render( p );
			Check( r.ok, "the samples-overridden render succeeds" );
			Check( r.samplesOverridden, "PT (a pixel-based rasterizer) accepts the sample-count override" );
			Check( r.effectiveSamples == 64, "effectiveSamples echoes the requested override (64)" );

			const std::string afterDoc = session->ReadDocument();
			Check( afterDoc == beforeDoc,
			       "MONEY ASSERTION (byte-identity): ReadDocument() is BYTE-IDENTICAL before and after a samples-overridden render -- "
			       "the override never touches the retained CST Document" );
			Check( afterDoc.find( "samples 8" ) != std::string::npos, "the Document STILL authors 'samples 8' after a samples=64 override render" );
		}

		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//------------------------------------------------------------------
	// (b) RED-PROVE restore: after an overridden render, the rasterizer's
	//     OWN sample count reads back to the scene-authored 8 (not left
	//     stuck at the override) -- both via GetSampleCountOverride()
	//     directly and via a SECOND, un-overridden render's
	//     effectiveSamples echo.
	//------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_samples_restore.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the restore scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (samples restore test)" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the Job (samples restore test)" );
		if( session )
		{
			IRasterizer* rast = pJob->GetRasterizer();
			Check( rast != nullptr, "the scene has an active rasterizer to query" );
			if( rast ) {
				Check( rast->GetSampleCountOverride() == 8, "GetSampleCountOverride() reads the scene-authored count (8) before any override" );
			}

			AgentRenderParams p1;
			p1.samples = 1;   // a big departure DOWN from 8, so a failed restore is easy to observe
			const AgentRenderResult r1 = session->Render( p1 );
			Check( r1.ok && r1.samplesOverridden, "the samples=1 override render succeeds and is accepted" );
			Check( r1.effectiveSamples == 1, "effectiveSamples echoes 1 for the first render" );

			if( rast ) {
				Check( rast->GetSampleCountOverride() == 8,
				       "RESTORE MONEY ASSERTION: immediately after the samples=1 override render, GetSampleCountOverride() reads back the "
				       "scene-authored 8 -- NOT left stuck at the override value" );
			}

			// A SECOND render with NO override requested renders at the
			// restored (scene-authored) count -- corroborating evidence
			// via the result's own echo, independent of the direct
			// rasterizer query above.
			AgentRenderParams p2;   // samples left at -1 (no override)
			const AgentRenderResult r2 = session->Render( p2 );
			Check( r2.ok, "the follow-up un-overridden render succeeds" );
			Check( !r2.samplesOverridden, "the follow-up render did not request an override" );
			Check( r2.effectiveSamples == 8, "the follow-up render's effectiveSamples echoes the RESTORED scene-authored count (8), not the prior override (1)" );
		}

		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//------------------------------------------------------------------
	// (c) RED-PROVE the existing-kernel in-place mutation path: kScene's
	//     rasterizer already authors a kernel (samples 8), so overriding
	//     to another value > 1 must MUTATE that kernel's count in place
	//     (SetNumSamples) rather than silently doing nothing -- observed
	//     via the same GetSampleCountOverride() readback.
	//------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_samples_inplace.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the in-place-mutation scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (samples in-place test)" );

		IRasterizer* rast = pJob->GetRasterizer();
		Check( rast != nullptr, "the scene has an active rasterizer (samples in-place test)" );
		if( rast ) {
			Check( rast->SetSampleCountOverride( 32 ), "SetSampleCountOverride(32) is accepted against the authored 8-sample kernel" );
			Check( rast->GetSampleCountOverride() == 32, "the existing kernel's count is MUTATED IN PLACE to 32" );
			Check( rast->SetSampleCountOverride( 8 ), "restoring back to 8 is accepted" );
			Check( rast->GetSampleCountOverride() == 8, "the kernel's count is restored to 8" );
		}

		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//------------------------------------------------------------------
	// (d) RED-PROVE the unsupported-rasterizer honesty contract: MLT
	//     (mlt_rasterizer) does NOT derive from PixelBasedRasterizerHelper
	//     and therefore inherits IRasterizer's safe default -- ALWAYS
	//     false/-1, never silently "succeeding" at nothing.  Queried
	//     directly (no render needed -- MLT is comparatively expensive
	//     and this is purely an interface-contract check).
	//------------------------------------------------------------------
	{
		const std::string mltScene =
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"mlt_rasterizer\n{\n\toidn_denoise false\n}\n\n"
			"film\n{\n\twidth 8\n\theight 8\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
			"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
			"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
			"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
			"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
			"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";
		const std::string scenePath = WriteTemp( "rise_agent_samples_unsupported.RISEscene", mltScene );
		Check( !scenePath.empty(), "wrote the MLT (unsupported-rasterizer) scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the MLT native-v7 scene via the CST path" );

		IRasterizer* rast = pJob->GetRasterizer();
		Check( rast != nullptr, "the MLT scene has an active rasterizer" );
		if( rast ) {
			Check( rast->GetSampleCountOverride() == -1,
			       "HONESTY MONEY ASSERTION: MLT (not a PixelBasedRasterizerHelper subclass) reports GetSampleCountOverride()==-1 -- unsupported, never guessed" );
			Check( !rast->SetSampleCountOverride( 16 ),
			       "HONESTY MONEY ASSERTION: MLT's SetSampleCountOverride(16) returns false -- honestly unsupported, no silent no-op success" );
			Check( rast->GetSampleCountOverride() == -1, "MLT's state is unchanged by the refused override attempt" );
		}

		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//------------------------------------------------------------------
	// (e) RED-PROVE throw-path restore: ThrowingRasterizeJob + a samples
	//     override -- the rasterizer's sample count is restored via the
	//     SAME SampleCountRestoreGuard RAII pattern as the film/camera
	//     overrides, even though mJob->Rasterize() throws mid-render.
	//------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_samples_throwrestore.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the samples throw-restore scene to a temp file" );

		ThrowingRasterizeJob* pJob = new ThrowingRasterizeJob();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ThrowingRasterizeJob loads the native-v7 scene via the CST path (samples throw-restore test)" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the throwing Job (samples throw-restore test)" );
		if( session )
		{
			IRasterizer* rast = pJob->GetRasterizer();
			Check( rast != nullptr, "the throwing Job has an active rasterizer" );

			AgentRenderParams p;
			p.samples = 40;   // a big departure from the authored 8

			const AgentRenderResult thrownResult = session->Render( p );
			Check( !thrownResult.ok, "the samples-overridden render on a throwing Rasterize() reports ok=false" );
			Check( thrownResult.message.find( "ThrowingRasterizeJob" ) != std::string::npos,
			       "the failure message names the thrown exception's text (samples throw-restore test)" );

			if( rast ) {
				Check( rast->GetSampleCountOverride() == 8,
				       "THROW-PATH RESTORE MONEY ASSERTION: despite the exception, the rasterizer's sample count is restored to the "
				       "scene-authored 8 -- the SampleCountRestoreGuard's destructor ran during unwinding, exactly like the film/camera guard" );
			}
		}

		pJob->release();
		std::remove( scenePath.c_str() );
	}

	std::printf( "=== sample-count override: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

int main()
{
	RunCoreTests();
	RunCameraOverrideTests();
	RunRestoreOnThrowTest();
	RunMalformedCameraOverrideTests();
	RunRenderJobIdTests();
	RunPreS2HardeningTests();
	RunSampleCountOverrideTests();

	std::printf( "=== AgentProposeRenderTest TOTAL: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
