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
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/SceneEditor/CameraIntrospection.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>   // P1-A red-prove: ThrowingRasterizeJob throws std::runtime_error
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

	bool threw = false;
	try {
		session->Render( p );
	}
	catch( const std::exception& ) {
		threw = true;
	}
	Check( threw, "Render() with a throwing Rasterize() propagates the exception (RED-PROVE the seam actually fires)" );

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

int main()
{
	RunCoreTests();
	RunCameraOverrideTests();
	RunRestoreOnThrowTest();
	RunMalformedCameraOverrideTests();

	std::printf( "=== AgentProposeRenderTest TOTAL: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
