//////////////////////////////////////////////////////////////////////
//
//  CSGNullGeometryLuminaireCrashTest.cpp - Regression test for the
//  2026-07-31 --agent-eval crash: a csg_object with an emissive
//  material bound null-derefs at render time.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-07-31
//  Tabs: 4
//  Comments:
//
//    ROOT CAUSE: RISE::Implementation::Object's default constructor
//    leaves `pGeometry` null; CSGObject (Object.h/CSGObject.h) NEVER
//    sets it -- a CSG object's shape is synthesized on the fly from
//    its two operand objects (CSGObject::IntersectRay /
//    getBoundingBox), so it overrides those two virtuals but not
//    GetArea() or UniformRandomPoint(), which fall through to the
//    base Object's `pGeometry->...` calls.
//
//    FIX ROUND 1 (this file's original scope): LuminaryManager::
//    AddToLuminaryList (src/Library/Rendering/LuminaryManager.cpp)
//    gates which emissive objects become NEE-sampled area lights.
//    Its guard used to read `if( pGeom && !pGeom->CanBeAreaLight() )`,
//    which short-circuits to FALSE -- i.e. "acceptable" -- for
//    exactly a null pGeom, so a csg_object with an emissive material
//    bound was pushed onto the luminaries list, and
//    LightSampler::Prepare (reached via RayCaster::AttachScene ->
//    RebuildLightSamplers) called GetArea() on it -> EXC_BAD_ACCESS.
//    Sibling sites with the identical short-circuit bug were also
//    fixed in round 1: SubSurfaceScatteringShaderOp.cpp,
//    DonnerJensenSkinSSSShaderOp.cpp (same pattern gating
//    UniformRandomPoint()/GetArea()), and ManifoldSolver.cpp's
//    SpecularCasterCollector (an unconditional UniformRandomPoint()
//    probe over EVERY material'd scene object, reached before its own
//    CanBeAreaLight() gate).
//
//    FIX ROUND 2 (reviewer-found gap this file's tests below close):
//    round 1 ONLY kept a null-geometry object OFF the NEE luminaries
//    list -- it did NOT audit the SEPARATE code path that reads an
//    already-hit emitter's OWN area/geometry when a ray actually
//    LANDS on it (direct camera view, or a BSDF-sampled indirect
//    bounce).  Four more unconditional/inverted-null-check GetArea()
//    call sites crashed on exactly that path:
//      - EmissionShaderOp.cpp (PerformOperation + PerformOperationNM):
//        `emitterNeeSampleable = ( !pEmitGeom || pEmitGeom->
//        CanBeAreaLight() )` was INVERTED for the null case (null ->
//        "sampleable" -> TRUE), entering a block that called
//        `ri.pObject->GetArea()` on ANY bsdfPdf>0 hit.
//      - PathTracingIntegrator.cpp (Pel PART 1 + the HWSS/NM loop):
//        identical inverted pattern.
//      - BDPTIntegrator.cpp (the s=0 "eye path naturally arrived at
//        an emitter" strategy): `eyeEnd.pObject->GetArea()` called
//        UNCONDITIONALLY -- reached on the PRIMARY camera ray
//        directly viewing the emitter (eyeVerts[0]=camera,
//        eyeVerts[1]=emitter -> t=2, the FIRST surface hit).
//      - VCMIntegrator.cpp (the analogous s=0 / path-length-1
//        strategy): GetArea() called before its own null-geometry
//        check.
//    Layer-1 fix: Object::GetArea() / Object::UniformRandomPoint()
//    (Object.cpp) now have a base-layer null-pGeometry guard
//    (return 0 / a defined fallback instead of dereferencing null) --
//    belt-and-suspenders under EVERY call site, known or future.
//    Layer-2 fix: each of the four sites above was corrected to treat
//    null geometry as NOT NEE-sampleable (matching the existing
//    CanBeAreaLight()==false precedent), so none of them reach
//    GetArea() on a null-geometry object at all in the audited call
//    graph.
//
//    THIS CRASHED TWICE during LIVE --agent-eval runs of the
//    cozy-study scenario (gemini-3.5-flash bound an emissive lamp-
//    shade material to a csg_object combining two standard_object
//    operands -- see evals/runs/bare_prompt_s0_anchor/
//    bare_prompt_build_cozy_study__gemini__gemini-3.5-flash__r2/
//    bare_prompt_build_cozy_study.trajectory.jsonl, tool call at
//    dotted_order ...00043, immediately followed by the fatal
//    `render` call).
//
//    TRUTH-DEFECT NOTE: earlier comments/diagnostics in this codebase
//    (LuminaryManager.cpp, AgentSession.cpp's LUMINAIRE_NULL_GEOMETRY
//    message, and this file's own original comments) asserted "the
//    object still glows when hit directly" as a description of
//    INTENDED behavior -- but round 1 alone did not make that TRUE in
//    general (round 1's own Test A/B below only ever exercised a
//    bsdfPdf==0 direct-camera-view PT hit, which happened to dodge
//    all four round-2 sites).  Every "still glows" claim in this
//    codebase is reworded to describe exactly what is VERIFIED by the
//    tests in this file: non-crashing, non-zero-radiance emission on
//    direct camera view AND on a BSDF-sampled indirect-bounce hit,
//    under PT, BDPT, AND VCM (TestCSGLuminaireGlowsUnderPathTracing /
//    TestCSGLuminaireGlowsUnderBDPT / TestCSGLuminaireGlowsUnderVCM
//    below) -- never "should" or "is designed to".
//
//    RED-PROOF METHOD used for every crash proved in this file: `git
//    stash` the specific fix commit's changed source files (keeping
//    this test file, which is untracked / independently staged),
//    rebuild, run (expect SIGSEGV / exit 139), `git stash pop`,
//    rebuild, run again (expect a clean pass).  See the fix round's
//    session report for the exact per-round file lists and observed
//    exit codes.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined( _WIN32 )
	#include <process.h>
	#include <io.h>
	#define RISE_TEST_GETPID _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>
	#define RISE_TEST_GETPID getpid
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Interfaces/IObjectManager.h"   // P1b: direct GetItem() lookup, no renderer
#include "../src/Library/Interfaces/IObjectPriv.h"       // P1b: GetArea()/UniformRandomPoint()/GetGeometry() direct calls
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"      // P1b: Point3/Vector3/Point2/Scalar
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentDiagnostic.h"

using namespace RISE;
using namespace RISE::Implementation;
using RISE::Agent::AgentSession;
using RISE::Agent::AgentDiagnostic;
namespace AgentDiagnosticCode = RISE::Agent::AgentDiagnosticCode;

static int g_failures = 0;
static std::vector<std::string> g_tmpScenes;  // removed at end of main

static void Check( bool cond, const char* what )
{
	if( cond ) {
		std::cout << "  [ok] " << what << "\n";
	} else {
		std::cout << "  [FAIL] " << what << "\n";
		++g_failures;
	}
}

//-----------------------------------------------------------------------------
// Captures rendered pixels so we can confirm the scene rendered SOMETHING
// (the emissive csg_object contributes non-zero radiance, even though it is
// refused as an NEE-sampled area light -- see the file header's
// TRUTH-DEFECT NOTE for exactly what this proves and does not prove).
//-----------------------------------------------------------------------------
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	unsigned int width;
	unsigned int height;
	double       maxLum;

	CapturingRasterizerOutput() : width( 0 ), height( 0 ), maxLum( 0.0 ) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage(
		const IRasterImage& pImage,
		const Rect*,
		const unsigned int ) override
	{
		width  = pImage.GetWidth();
		height = pImage.GetHeight();
		maxLum = 0.0;
		for( unsigned int y = 0; y < height; ++y ) {
			for( unsigned int x = 0; x < width; ++x ) {
				const RISEColor c = pImage.GetPEL( x, y );
				const double lum = c.base.r + c.base.g + c.base.b;
				if( lum > maxLum ) maxLum = lum;
			}
		}
	}
};

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	const char* tempDir = std::getenv( "TMPDIR" );
#if defined( _WIN32 )
	if( !tempDir || !*tempDir ) {
		tempDir = std::getenv( "TEMP" );
	}
#endif
	if( !tempDir || !*tempDir ) {
		tempDir = ".";
	}

	std::string path( tempDir );
	if( path.back() != '/' && path.back() != '\\' ) {
		path += '/';
	}
	char filename[256];
	std::snprintf( filename, sizeof(filename),
		"csg_null_geom_luminaire_%s_%d.RISEscene",
		tag, static_cast<int>( RISE_TEST_GETPID() ) );
	path += filename;

	std::ofstream ofs( path.c_str() );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	g_tmpScenes.push_back( path );
	return path;
}

// Render `sceneText` and return {rendered-without-crashing, maxLum}.  A
// crash inside pJob->Rasterize() terminates the WHOLE TEST PROCESS (that is
// the point -- see the file header's RED-PROOF METHOD), so "rendered==true"
// really does mean "reached the line after Rasterize() alive".
struct RenderProbeResult { bool rendered; double maxLum; };
static RenderProbeResult RenderScene( const std::string& sceneText, const char* tag )
{
	RenderProbeResult result{ false, 0.0 };

	const std::string scenePath = WriteSceneToTempFile( sceneText, tag );
	if( scenePath.empty() ) return result;

	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return result;

	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "csg null-geometry luminaire test capture" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	result.rendered = pJob->Rasterize();
	result.maxLum   = pCap->maxLum;

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

// Same as RenderScene, but ALSO captures everything written to stdout during
// the render (GlobalLog's default sink prints there -- see every `[ok]`-
// adjacent log line already visible in this test's own console output) so a
// test can assert a SPECIFIC diagnostic fired, not just "didn't crash".  Used
// by the P1a red-proof (ManifoldSolver's null-geometry reflection-fallback
// gate) per the reviewer's own stated fallback: "if constructing a
// behavioral assertion is impractical, a targeted... log assertion that the
// gate fires is acceptable."  Redirects the process's real stdout fd (via
// dup/dup2, not freopen) to a temp file for the duration of the call, then
// restores it -- robust regardless of what stdout originally pointed to
// (pipe, file, tty), unlike freopen-ing back to a guessed path.
struct RenderProbeResultWithLog { bool rendered; double maxLum; std::string capturedOutput; };
static RenderProbeResultWithLog RenderSceneCapturingStdout( const std::string& sceneText, const char* tag )
{
	RenderProbeResultWithLog result{ false, 0.0, std::string() };

	const std::string scenePath = WriteSceneToTempFile( sceneText, tag );
	if( scenePath.empty() ) return result;
	const std::string capturePath = scenePath + ".stdout.txt";

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capturePath.c_str(), "w" );
	if( capFile ) {
		RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );
	}

	{
		IJobPriv* pJob = 0;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			if( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
				pJob->RemoveRasterizerOutputs();
				CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
				GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "csg null-geometry luminaire stdout-capture test" );
				pJob->GetRasterizer()->AddRasterizerOutput( pCap );
				result.rendered = pJob->Rasterize();
				result.maxLum   = pCap->maxLum;
				safe_release( pCap );
			}
			safe_release( pJob );
		}
	}

	std::fflush( stdout );
	if( savedFd >= 0 ) {
		RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) );
		RISE_TEST_CLOSE( savedFd );
	}
	if( capFile ) {
		std::fclose( capFile );
	}

	std::ifstream ifs( capturePath );
	if( ifs.is_open() ) {
		std::ostringstream oss;
		oss << ifs.rdbuf();
		result.capturedOutput = oss.str();
	}
	std::remove( capturePath.c_str() );

	return result;
}

// Minimal distillation of the cozy-study trajectory's fatal document state:
// two standard_object operands (any material -- AssignObjects hides both,
// so they are never independently enumerated as luminaries) combined by a
// csg_object whose OWN material is emissive.  This is the smallest scene
// that reaches LuminaryManager::AddToLuminaryList with a null-geometry,
// emitter-bound object.
//
// SCOPE NOTE (fix round 2): this scene ONLY exercises a bsdfPdf==0 direct
// camera-view PT hit -- it does NOT reach any of the four round-2 sites
// (EmissionShaderOp / PathTracingIntegrator's bsdfPdf>0 blocks require an
// INDIRECT bounce; BDPT / VCM are entirely different rasterizers).  Kept as
// the minimal round-1 repro; see kThreeIntegratorScene* below for the
// scene that reaches all four round-2 sites under PT/BDPT/VCM.
static const char* kCSGLuminaireSceneText =
	"RISE ASCII SCENE 7\n"
	"\n"
	"film\n{\n\twidth 32\n\theight 32\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 -6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_glow\n\tcolor 3.0 2.5 1.5\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_glow\n\texitance pnt_glow\n\tscale 3.0\n}\n\n"
	"sphere_geometry\n{\n\tname sph_a\n\tradius 0.6\n}\n\n"
	"sphere_geometry\n{\n\tname sph_b\n\tradius 0.6\n}\n\n"
	"standard_object\n{\n\tname csg_opA\n\tgeometry sph_a\n\tposition 0 0 0\n\tmaterial matte\n}\n\n"
	"standard_object\n{\n\tname csg_opB\n\tgeometry sph_b\n\tposition 0.35 0 0\n\tmaterial matte\n}\n\n"
	"csg_object\n{\n\tname csg_glow\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tposition 0 0 0\n\tmaterial mat_glow\n}\n";

//-----------------------------------------------------------------------------
// Test A: a csg_object with an emissive material bound must not crash the
// render pipeline (Job::Rasterize -> RayCaster::AttachScene ->
// RebuildLightSamplers -> LightSampler::Prepare).  This is the exact call
// chain from the crash reports (rise-2026-07-31-220450.ips /
// rise-2026-07-31-221153.ips) and proves the ROUND-1 (LuminaryManager) fix.
//-----------------------------------------------------------------------------
static void TestCSGEmissiveObjectDoesNotCrash()
{
	std::cout << "Test A: csg_object with emissive material renders without crashing (round-1 scope)...\n";

	// THE CRASH: pre-round-1-fix, this call segfaults inside
	// RebuildLightSamplers (LightSampler::Prepare -> Object::GetArea() ->
	// pGeometry->GetArea() with pGeometry == 0 for the csg_object).
	// Reaching the Check() below at all is the proof of that fix.
	const RenderProbeResult r = RenderScene( kCSGLuminaireSceneText, "basic" );
	Check( r.rendered, "scene with csg_object luminary rendered without crashing" );

	// The csg_object is refused as an NEE-sampled area light (no crash,
	// just skipped), but it still contributes emission when directly hit by
	// a camera ray (bsdfPdf==0) -- the render should not be all-black.
	Check( r.maxLum > 0.0,
		( "csg_object contributes non-zero radiance on direct view (maxLum=" + std::to_string( r.maxLum ) + ")" ).c_str() );
}

//-----------------------------------------------------------------------------
// Test B: re-rendering the same document (RayCaster::AttachScene's
// same-pointer-generation-advanced rebuild path, RebuildLightSamplers'
// OTHER call site) must also not crash -- this is the path a live agent-
// edited document takes on every subsequent render after the first.
//-----------------------------------------------------------------------------
static void TestCSGEmissiveObjectSurvivesRerender()
{
	std::cout << "Test B: re-render of the same csg-luminaire document does not crash...\n";

	const std::string scenePath = WriteSceneToTempFile( kCSGLuminaireSceneText, "rerender" );
	if( scenePath.empty() ) { Check( false, "scene temp file written" ); return; }

	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check( false, "Job created" ); return; }
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) { Check( false, "scene loaded" ); safe_release( pJob ); return; }

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "csg null-geometry luminaire rerender capture" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	Check( pJob->Rasterize(), "first render did not crash" );
	Check( pJob->Rasterize(), "second render (re-attach) did not crash" );

	safe_release( pCap );
	safe_release( pJob );
}

//=============================================================================
// FIX ROUND 2: the three-integrator scene.
//
// A floor (infiniteplane_geometry, diffuse) directly below a large, close,
// effectively room-spanning "ceiling" (a csg_object union of two big boxes,
// bound to an emissive material with a REAL underlying scattering material
// -- lambertian_luminaire_material's `material matte`, not the "none"
// default round-1's kCSGLuminaireSceneText left implicit).  The ceiling
// covers nearly the entire upward hemisphere as seen from any floor point,
// so:
//   - the camera's PRIMARY ray can see the ceiling directly (exercises
//     BDPT's t=2 s=0 strategy / VCM's path-length-1 strategy -- both fire
//     deterministically on ANY primary-ray hit of an emitter, no bounce
//     needed);
//   - a floor hit's BSDF-SAMPLED bounce ray (bsdfPdf>0) is, for all
//     practical statistical purposes, guaranteed to also land on the
//     ceiling given the huge covered solid angle -- exercising
//     EmissionShaderOp / PathTracingIntegrator's bsdfPdf>0 blocks, which
//     kCSGLuminaireSceneText above (no floor, no second object to bounce
//     off) structurally cannot reach.
//=============================================================================
static const char* kThreeIntegratorScenePrefix =
	"RISE ASCII SCENE 7\n"
	"\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0.6 -4.0\n\tlookat 0 0.9 0\n\tup 0 1 0\n\tfov 70.0\n}\n\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

static const char* kThreeIntegratorSceneSuffix =
	"\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_glow\n\tcolor 3.0 2.5 1.5\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_glow\n\texitance pnt_glow\n\tmaterial matte\n\tscale 4.0\n}\n\n"
	"infiniteplane_geometry\n{\n\tname floor_plane\n\txtile 1.0\n\tytile 1.0\n}\n\n"
	"standard_object\n{\n\tname obj_floor\n\tgeometry floor_plane\n\tmaterial matte\n\tposition 0 0 0\n\torientation -90 0 0\n}\n\n"
	"box_geometry\n{\n\tname ceil_a\n\twidth 14.0\n\theight 0.6\n\tdepth 14.0\n}\n\n"
	"box_geometry\n{\n\tname ceil_b\n\twidth 14.0\n\theight 0.6\n\tdepth 14.0\n}\n\n"
	"standard_object\n{\n\tname csg_opA\n\tgeometry ceil_a\n\tmaterial matte\n\tposition 0 0 0\n}\n\n"
	"standard_object\n{\n\tname csg_opB\n\tgeometry ceil_b\n\tmaterial matte\n\tposition 0.05 0 0\n}\n\n"
	"csg_object\n{\n\tname csg_glow\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tposition 0 1.0 0\n\tmaterial mat_glow\n}\n";

static std::string BuildThreeIntegratorScene( const std::string& rasterizerChunk )
{
	return std::string( kThreeIntegratorScenePrefix ) + rasterizerChunk + kThreeIntegratorSceneSuffix;
}

//-----------------------------------------------------------------------------
// Test C: PathTracingIntegrator / EmissionShaderOp's bsdfPdf>0 (INDIRECT
// bounce) sites.  High sample count so a floor-bounce landing on the huge
// ceiling is, for practical purposes, guaranteed at least once across the
// render.  Pre-round-2-fix this SIGSEGVs (EmissionShaderOp.cpp's inverted
// null check entered the weighting block and called
// `ri.pObject->GetArea()` on the null-geometry csg_object).
//-----------------------------------------------------------------------------
static void TestCSGLuminaireGlowsUnderPathTracing()
{
	std::cout << "Test C: csg_object luminaire under pathtracing_pel_rasterizer (indirect bounce)...\n";

	const std::string scene = BuildThreeIntegratorScene(
		"pathtracing_pel_rasterizer\n{\n\tsamples 48\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" );
	const RenderProbeResult r = RenderScene( scene, "pt3" );
	Check( r.rendered, "PT: ceiling+floor scene with csg_object luminaire rendered without crashing" );
	Check( r.maxLum > 0.0,
		( "PT: csg_object contributes non-zero radiance (direct view + indirect bounce), maxLum=" +
		  std::to_string( r.maxLum ) + ")" ).c_str() );
}

//-----------------------------------------------------------------------------
// Test D: BDPTIntegrator's s=0 "eye path naturally arrived at an emitter"
// strategy.  Fires deterministically on the PRIMARY camera ray directly
// viewing the emitter (eyeVerts[0]=camera, eyeVerts[1]=csg_glow -> t=2) --
// no bounce needed, so even samples=1 reliably reaches it.  Pre-round-2-fix
// this SIGSEGVs (`eyeEnd.pObject->GetArea()` called unconditionally).
//
// ENERGY NOTE (2026-08-01).  This test asserts only maxLum > 0; the VALUE it
// prints tracked BDPT's phantom-NEE-strategy MIS defect and moved when that
// was fixed: maxLum was 9.26 while remap0 promoted the non-NEE-sampleable
// emitter's true pdfRev == 0 to 1, and is 42.00 now that the s=0 strategy
// carries full weight -- an exact match for the VCM twin below (Test E, also
// 42.00).  PT's 29.57 differs from both; a MAX statistic is single-sample and
// sampling-dependent, so that is plausibly just which sample each integrator
// happened to draw, but it was NOT investigated and nothing here rests on it.
// The energy claim itself is pinned quantitatively (mean
// luma vs PT, both flavours of non-NEE-sampleable emitter) by
// tests/BDPTPhantomStrategyWeightTest.cpp, not here.
//-----------------------------------------------------------------------------
static void TestCSGLuminaireGlowsUnderBDPT()
{
	std::cout << "Test D: csg_object luminaire under bdpt_pel_rasterizer (direct view)...\n";

	const std::string scene = BuildThreeIntegratorScene(
		"bdpt_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" );
	const RenderProbeResult r = RenderScene( scene, "bdpt3" );
	Check( r.rendered, "BDPT: ceiling+floor scene with csg_object luminaire rendered without crashing" );
	Check( r.maxLum > 0.0,
		( "BDPT: csg_object contributes non-zero radiance on direct view, maxLum=" +
		  std::to_string( r.maxLum ) + ")" ).c_str() );
}

//-----------------------------------------------------------------------------
// Test E: VCMIntegrator's analogous s=0 / path-length-1 strategy.  Same
// determinism argument as BDPT above (direct primary-ray view of the
// emitter).  Pre-round-2-fix this SIGSEGVs (`v.pObject->GetArea()` called
// before its own null-geometry check).
//-----------------------------------------------------------------------------
static void TestCSGLuminaireGlowsUnderVCM()
{
	std::cout << "Test E: csg_object luminaire under vcm_pel_rasterizer (direct view)...\n";

	const std::string scene = BuildThreeIntegratorScene(
		"vcm_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" );
	const RenderProbeResult r = RenderScene( scene, "vcm3" );
	Check( r.rendered, "VCM: ceiling+floor scene with csg_object luminaire rendered without crashing" );
	Check( r.maxLum > 0.0,
		( "VCM: csg_object contributes non-zero radiance on direct view, maxLum=" +
		  std::to_string( r.maxLum ) + ")" ).c_str() );
}

//-----------------------------------------------------------------------------
// Test F: AgentSession::ValidateText surfaces LUMINAIRE_NULL_GEOMETRY at
// Severity::Warning for a document whose sole luminary is a csg_object
// (closes 74-log open item 2 at the validate-seam level; the full
// CheckDiagnosticsKind red-proof lives in
// tests/AgentEvalCheckTest.cpp's TestDiagnosticsCheckpointCleanFailsOnLuminaireNullGeometryWarning).
//-----------------------------------------------------------------------------
static void TestValidateSurfacesLuminaireNullGeometryWarning()
{
	std::cout << "Test F: AgentSession::ValidateText fires LUMINAIRE_NULL_GEOMETRY (Warning)...\n";

	const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( kCSGLuminaireSceneText );
	const AgentDiagnostic* found = nullptr;
	for( const AgentDiagnostic& d : diags ) {
		if( d.code == AgentDiagnosticCode::LUMINAIRE_NULL_GEOMETRY ) { found = &d; break; }
	}
	Check( found != nullptr, "LUMINAIRE_NULL_GEOMETRY diagnostic fires on the csg-luminaire document" );
	if( found ) {
		Check( found->severity == AgentDiagnostic::Severity::Warning, "...at Severity::Warning (a real functional gap, not a style advisory)" );
	}
}

//=============================================================================
// P1b (fix round 3, Opus mutation review): mutation testing proved removing
// Object::GetArea()'s null guard is SILENT against every test above --
// every scenario is absorbed by a LOCAL gate upstream (LuminaryManager,
// EmissionShaderOp, the integrators, the SSS shader ops) before GetArea()
// is ever reached on a null-geometry object, so the literal
// original-crash LINE (Object::GetArea() -> pGeometry->GetArea()) was
// unpinned by any test -- layered defense working exactly as intended, but
// the base layer itself had no DIRECT proof.  This test calls
// Object::GetArea() / Object::UniformRandomPoint() DIRECTLY through the
// public IObject interface on a null-geometry object constructed via the
// NORMAL parse path (a csg_object) -- no renderer, no camera/film/
// rasterizer chunk, no Rasterize() call at all.
//=============================================================================
static const char* kNullGeometryUnitScene =
	"RISE ASCII SCENE 7\n"
	"\n"
	"sphere_geometry\n{\n\tname sph_a\n\tradius 0.6\n}\n\n"
	"sphere_geometry\n{\n\tname sph_b\n\tradius 0.6\n}\n\n"
	"standard_object\n{\n\tname unit_opA\n\tgeometry sph_a\n}\n\n"
	"standard_object\n{\n\tname unit_opB\n\tgeometry sph_b\n\tposition 0.35 0 0\n}\n\n"
	"csg_object\n{\n\tname csg_unit\n\tobja unit_opA\n\tobjb unit_opB\n\toperation union\n}\n";

static void TestObjectGetAreaAndUniformRandomPointDirectOnNullGeometry()
{
	std::cout << "Test K (P1b): Object::GetArea()/UniformRandomPoint() called DIRECTLY "
	             "on a null-geometry csg_object (no renderer)...\n";

	const std::string scenePath = WriteSceneToTempFile( kNullGeometryUnitScene, "null_geom_unit" );
	Check( !scenePath.empty(), "scene temp file written" );
	if( scenePath.empty() ) return;

	IJobPriv* pJob = 0;
	Check( RISE_CreateJobPriv( &pJob ) && pJob, "Job created" );
	if( !pJob ) return;

	// No camera / film / rasterizer chunk anywhere in kNullGeometryUnitScene --
	// this test never renders, only parses + derives + queries the object
	// directly, proving the base-layer guard in complete isolation from the
	// render pipeline.
	const bool loaded = pJob->LoadAsciiSceneViaCst( scenePath.c_str() );
	Check( loaded, "scene loaded (parse) -- geometry/object/csg_object chunks only" );
	if( !loaded ) { safe_release( pJob ); return; }

	IObjectManager* pObjMan = pJob->GetObjects();
	Check( pObjMan != nullptr, "job exposes an object manager" );
	if( !pObjMan ) { safe_release( pJob ); return; }

	IObjectPriv* pCsg = pObjMan->GetItem( "csg_unit" );
	Check( pCsg != nullptr, "csg_unit resolved via the normal parse path" );
	if( pCsg ) {
		Check( pCsg->GetGeometry() == nullptr,
			"csg_unit's GetGeometry() is null (CSGObject never sets pGeometry -- the root-cause condition)" );

		// THE LITERAL ORIGINAL-CRASH LINE, called directly:
		// Object::GetArea() -> (pre-fix) pGeometry->GetArea() with
		// pGeometry == 0.
		const Scalar area = pCsg->GetArea();
		Check( area == Scalar( 0 ), "Object::GetArea() on a null-geometry object returns exactly 0 (documented fallback)" );

		// Object::UniformRandomPoint's documented fallback: this object's own
		// local origin transformed to world space (identity transform here --
		// no position/orientation/scale set on csg_unit -- so world origin),
		// a canonical +Y normal, and a zero UV.  Pre-fill with sentinel
		// values so a no-op (uninitialized-read) accidentally matching by
		// chance is not mistaken for a real pass.
		Point3  p( 1, 2, 3 );
		Vector3 n( 9, 9, 9 );
		Point2  uv( 9, 9 );
		pCsg->UniformRandomPoint( &p, &n, &uv, Point3( 0.3, 0.5, 0.7 ) );
		Check( p.x == 0 && p.y == 0 && p.z == 0, "UniformRandomPoint's fallback point is the object's world-space origin" );
		Check( n.x == 0 && n.y == 1 && n.z == 0, "UniformRandomPoint's fallback normal is canonical +Y" );
		Check( uv.x == 0 && uv.y == 0, "UniformRandomPoint's fallback UV is (0,0)" );
	}

	safe_release( pJob );
}

//=============================================================================
// P2b(i) (fix round 3): EmissionShaderOp.cpp's two flipped-gate fixes
// (PerformOperation / PerformOperationNM) were exercised by NO test -- the
// three-integrator scene above routes through PathTracingIntegrator.cpp
// (the MODERN unified integrator), and the SSS scenes' csg_object is
// non-emissive.  EmissionShaderOp is a SEPARATE, legacy code path: it only
// fires when a standard_shader chunk explicitly chains the "DefaultEmission"
// shaderop (Job::InitializeContainers registers it, but does NOT include it
// in "DefaultDirectLighting" or "DefaultPathTracing" -- those never call
// GetEmitter()/emittedRadiance at all) ahead of a lighting shaderop, under
// the legacy pixelpel_rasterizer.  This test makes an emissive csg_object
// (dropping the SSS shaderop from the earlier BuildCSGSSSScene pattern)
// visible to the camera so EmissionShaderOp::PerformOperation's emitter
// branch actually runs.
//
// SCOPE (P2b(ii)): this is a DIRECT-VIEW test only (bsdfPdf==0, camera ray
// hits the emitter directly) -- it does NOT independently exercise
// EmissionShaderOp's bsdfPdf>0 (BSDF-sampled indirect hit) block.  That
// block is CODE-verified (identical `pEmitGeom && pEmitGeom->
// CanBeAreaLight()` fix, reviewed alongside PathTracingIntegrator.cpp's
// twin, which the indirect-bounce PT test above DOES exercise), not
// independently render-tested here.  See the file header / P2b comments in
// LuminaryManager.cpp and AgentSession.cpp for the exact scoped claim.
//=============================================================================
static std::string BuildCSGEmissionShaderOpScene()
{
	return
		"RISE ASCII SCENE 7\n"
		"\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultEmission\n\tshaderop DefaultDirectLighting\n}\n"
		"\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n"
		"pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n"
		"\n"
		"pixelpel_rasterizer\n{\n\tmax_recursion 4\n\tsamples 4\n\tlum_samples 1\n\toidn_denoise FALSE\n}\n"
		"\n"
		"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n"
		"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_glow\n\tcolor 3.0 2.5 1.5\n}\n"
		"lambertian_luminaire_material\n{\n\tname mat_glow\n\texitance pnt_glow\n\tmaterial matte\n\tscale 4.0\n}\n"
		"sphere_geometry\n{\n\tname sph_a\n\tradius 0.7\n}\n"
		"sphere_geometry\n{\n\tname sph_b\n\tradius 0.7\n}\n"
		"standard_object\n{\n\tname csg_opA\n\tgeometry sph_a\n\tmaterial matte\n\tposition -0.3 0 0\n}\n"
		"standard_object\n{\n\tname csg_opB\n\tgeometry sph_b\n\tmaterial matte\n\tposition 0.3 0 0\n}\n"
		"csg_object\n{\n\tname csg_glow\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tmaterial mat_glow\n}\n";
}

//-----------------------------------------------------------------------------
// Test L (P2b-i): emissive csg_object under the legacy DefaultEmission /
// pixelpel_rasterizer chain.  This is a DIRECT-VIEW test (bsdfPdf==0), so
// EmissionShaderOp's `rs.bsdfPdf > 0 && ri.pObject && emitterNeeSampleable`
// weighting block is skipped on this path regardless of the null-geometry
// fix (see the P2b(ii) scope note above the scene builder) -- what THIS
// test proves is that EmissionShaderOp's emitter branch (the
// `pEmitter->emittedRadiance(...)` call, reached for ANY pEmitter hit
// regardless of bsdfPdf) fires AT ALL under the legacy chain without
// crashing and produces visible emission, closing the "EmissionShaderOp
// has NO test coverage" gap for its direct-view path.  Its bsdfPdf>0
// branch remains code-verified-only, not independently render-tested.
//-----------------------------------------------------------------------------
static void TestCSGEmissiveUnderEmissionShaderOpChain()
{
	std::cout << "Test L (P2b-i): emissive csg_object under DefaultEmission/pixelpel_rasterizer chain...\n";

	const RenderProbeResult r = RenderScene( BuildCSGEmissionShaderOpScene(), "emission_shaderop" );
	Check( r.rendered, "EmissionShaderOp chain: csg_object luminaire rendered without crashing" );
	Check( r.maxLum > 0.0,
		( "EmissionShaderOp chain: csg_object contributes non-zero radiance on direct view, maxLum=" +
		  std::to_string( r.maxLum ) + ")" ).c_str() );
}

//=============================================================================
// SIBLING COVERAGE: SSS (round-1 fix, previously untested).
//
// SubSurfaceScatteringShaderOp.cpp / DonnerJensenSkinSSSShaderOp.cpp had the
// identical `pSSSGeom && !pSSSGeom->CanBeAreaLight()` short-circuit bug
// (fixed round 1) gating `ri.pObject->UniformRandomPoint(...)` /
// `ri.pObject->GetArea()` for the SSS irradiance point-set build.  A
// csg_object (union of two overlapping spheres, so the union has real
// volume/surface, unlike a degenerate case) shaded through either SSS
// shaderop reaches this on its first hit.  No emissive material needed --
// SSS applies to any real (non-emissive) material via the shaderop chain.
//=============================================================================
static std::string BuildCSGSSSScene( const char* sssShaderOpChunk )
{
	return
		std::string(
			"RISE ASCII SCENE 7\n"
			"\n"
			"standard_shader\n{\n\tname sss_irrad\n\tshaderop DefaultDirectLighting\n}\n" )
		+ sssShaderOpChunk +
		"standard_shader\n{\n\tname global\n\tshaderop sss_op\n}\n"
		"\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n"
		"pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n"
		"\n"
		"pixelpel_rasterizer\n{\n\tmax_recursion 4\n\tsamples 4\n\tlum_samples 1\n\toidn_denoise FALSE\n}\n"
		"\n"
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1.0 1.0 1.0\n}\n"
		"lambertian_material\n{\n\tname sss_mat\n\treflectance white\n}\n"
		"sphere_geometry\n{\n\tname sph_a\n\tradius 0.7\n}\n"
		"sphere_geometry\n{\n\tname sph_b\n\tradius 0.7\n}\n"
		"standard_object\n{\n\tname csg_opA\n\tgeometry sph_a\n\tmaterial sss_mat\n\tposition -0.3 0 0\n}\n"
		"standard_object\n{\n\tname csg_opB\n\tgeometry sph_b\n\tmaterial sss_mat\n\tposition 0.3 0 0\n}\n"
		"csg_object\n{\n\tname csg_sss\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tmaterial sss_mat\n}\n"
		"omni_light\n{\n\tname light\n\tpower 1200.0\n\tposition 0 4 4\n\tcolor 1.0 1.0 1.0\n}\n";
}

//-----------------------------------------------------------------------------
// Test G: simple_sss_shaderop on a csg_object.  Pre-round-1-fix this
// SIGSEGVs (`ri.pObject->UniformRandomPoint(...)` on the null-geometry
// csg_object during the irradiance point-set build).
//-----------------------------------------------------------------------------
static void TestCSGSimpleSSSDoesNotCrash()
{
	std::cout << "Test G: csg_object shaded via simple_sss_shaderop renders without crashing...\n";

	const std::string scene = BuildCSGSSSScene(
		"simple_sss_shaderop\n{\n\tname sss_op\n\tnumpoints 500\n\tirrad_scale 6.0\n\tgeometric_scale 1.0\n\tshader sss_irrad\n}\n" );
	const RenderProbeResult r = RenderScene( scene, "sss_simple" );
	Check( r.rendered, "simple_sss_shaderop csg_object rendered without crashing" );
}

//-----------------------------------------------------------------------------
// Test H: donner_jensen_skin_sss_shaderop on a csg_object.  Pre-round-1-fix
// this SIGSEGVs the same way (`ri.pObject->GetArea()` /
// `UniformRandomPoint(...)` on the null-geometry csg_object).
//-----------------------------------------------------------------------------
static void TestCSGDonnerJensenSSSDoesNotCrash()
{
	std::cout << "Test H: csg_object shaded via donner_jensen_skin_sss_shaderop renders without crashing...\n";

	const std::string scene = BuildCSGSSSScene(
		"donner_jensen_skin_sss_shaderop\n{\n\tname sss_op\n\tnumpoints 500\n\tirrad_scale 6.0\n\tshader sss_irrad\n}\n" );
	const RenderProbeResult r = RenderScene( scene, "sss_dj" );
	Check( r.rendered, "donner_jensen_skin_sss_shaderop csg_object rendered without crashing" );
}

//=============================================================================
// SIBLING COVERAGE: SMS (round-1 fix, previously untested).
//
// ManifoldSolver.cpp's SpecularCasterCollector (PreRenderSetup ->
// EnumerateSpecularCasters, run unconditionally whenever sms_enabled TRUE,
// regardless of seeding mode -- PathTracingPelRasterizer.cpp: "Cheap to do
// unconditionally -- runs once per render and only iterates objects that
// have a material attached") walked EVERY material'd scene object and
// called `obj.UniformRandomPoint(...)` UNCONDITIONALLY, before its own
// CanBeAreaLight() gate further down and before any specular
// classification.  A plain LAMBERTIAN (non-specular) csg_object placed
// OFF-SCREEN still gets probed -- classification happens BY probing, not
// by pre-filtering to already-known-specular materials.
//=============================================================================
static const char* kSMSOffscreenCSGScene =
	"RISE ASCII SCENE 7\n"
	"\n"
	"film\n{\n\twidth 16\n\theight 16\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tsms_enabled TRUE\n\toidn_denoise FALSE\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"directional_light\n{\n\tname sun\n\tpower 3.14159\n\tcolor 1 1 1\n\tdirection 0 0 1\n}\n\n"
	"sphere_geometry\n{\n\tname vis_sphere\n\tradius 1.0\n}\n\n"
	"standard_object\n{\n\tname obj_visible\n\tgeometry vis_sphere\n\tmaterial matte\n\tposition 0 0 0\n}\n\n"
	"sphere_geometry\n{\n\tname sph_a\n\tradius 0.6\n}\n\n"
	"sphere_geometry\n{\n\tname sph_b\n\tradius 0.6\n}\n\n"
	"standard_object\n{\n\tname csg_opA\n\tgeometry sph_a\n\tmaterial matte\n\tposition -5.0 0 0\n}\n\n"
	"standard_object\n{\n\tname csg_opB\n\tgeometry sph_b\n\tmaterial matte\n\tposition -4.65 0 0\n}\n\n"
	"csg_object\n{\n\tname csg_offscreen\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tposition 0 0 0\n\tmaterial matte\n}\n";

//-----------------------------------------------------------------------------
// Test I: an off-screen, non-specular, non-emissive csg_object with
// sms_enabled TRUE renders without crashing.  Pre-round-1-fix this
// SIGSEGVs during PreRenderSetup, BEFORE any pixel is even shaded
// (`obj.UniformRandomPoint(...)` in the kProbes classification loop).
//-----------------------------------------------------------------------------
static void TestSMSOffscreenCSGDoesNotCrash()
{
	std::cout << "Test I: off-screen lambertian csg_object with sms_enabled TRUE renders without crashing...\n";

	const RenderProbeResult r = RenderScene( kSMSOffscreenCSGScene, "sms_offscreen" );
	Check( r.rendered, "sms_enabled scene with an off-screen csg_object rendered without crashing" );
}

//=============================================================================
// P1a (fix round 3): ManifoldSolver.cpp's k=1 REFLECTION SURFACE-SAMPLE
// FALLBACK -- a DIFFERENT ManifoldSolver site from Test I above.  Test I's
// `mSpecularCasters` cache is pre-filtered for null geometry (Test I proves
// THAT gate).  This fallback's `pFirstCaster` instead comes from a LIVE ray
// hit (`baseSeedChain[0].pObject`, published by CSGObject::IntersectRay as
// `ri.pObject = this`), so it was NOT covered by Test I's fix and reaches
// `pFirstCaster->UniformRandomPoint(...)` independently whenever: SMS is
// enabled, the caster is REFLECTIVE (non-refracting, e.g.
// perfectreflector_material), and the chain is k=1 (single-bounce mirror
// caustic -- see ManifoldSolver.cpp's own comment on why k=1 reflection is
// structurally degenerate and needs this fallback at all).
//
// Modeled on scenes/Tests/SMS/sms_k1_mirror.RISEscene's proven "tilted
// mirror reflects an area light onto a floor" geometry, with the mirror's
// single clippedplane_geometry replaced by a csg_object (union of two thin
// overlapping boxes) so pFirstCaster->GetGeometry() is null when a shading
// ray's manifold walk hits it.  `sms_multi_trials` > 1 with no photon map
// forces trials past the base seed into the `photonCursor >=
// photonSeeds.size()` branch, which is exactly where
// surfaceSampleReflectionFallback is consulted.
//=============================================================================
static const char* kSMSMirrorCSGScene =
	"RISE ASCII SCENE 7\n"
	"\n"
	"film\n{\n\twidth 40\n\theight 30\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 2.0 3\n\tlookat 0 0.2 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tsms_enabled TRUE\n\tsms_multi_trials 8\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.8 0.75 0.65\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance pnt_floor\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_light\n\tcolor 1.0 0.95 0.85\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname light_mat\n\texitance pnt_light\n\tscale 2000.0\n\tmaterial none\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_mirror\n\tcolor 0.9 0.9 0.9\n}\n\n"
	"perfectreflector_material\n{\n\tname mirror_mat\n\treflectance pnt_mirror\n}\n\n"
	"clippedplane_geometry\n{\n\tname floor_geom\n\tpta -4.0 0.0 -4.0\n\tptb -4.0 0.0 4.0\n\tptc 4.0 0.0 4.0\n\tptd 4.0 0.0 -4.0\n}\n\n"
	"clippedplane_geometry\n{\n\tname light_geom\n\tpta -0.1 0.0 -0.1\n\tptb 0.1 0.0 -0.1\n\tptc 0.1 0.0 0.1\n\tptd -0.1 0.0 0.1\n}\n\n"
	"box_geometry\n{\n\tname mirror_box_a\n\twidth 1.2\n\theight 1.2\n\tdepth 0.05\n}\n\n"
	"box_geometry\n{\n\tname mirror_box_b\n\twidth 1.2\n\theight 1.2\n\tdepth 0.05\n}\n\n"
	"standard_object\n{\n\tname floor\n\tgeometry floor_geom\n\tmaterial floor_mat\n}\n\n"
	"standard_object\n{\n\tname area_light\n\tgeometry light_geom\n\tposition 0 2.3 -1.3\n\tscale 0.4 0.4 0.4\n\tmaterial light_mat\n}\n\n"
	"standard_object\n{\n\tname mirror_opA\n\tgeometry mirror_box_a\n\tmaterial mirror_mat\n}\n\n"
	"standard_object\n{\n\tname mirror_opB\n\tgeometry mirror_box_b\n\tposition 0 0 0.02\n\tmaterial mirror_mat\n}\n\n"
	"csg_object\n{\n\tname mirror_csg\n\tobja mirror_opA\n\tobjb mirror_opB\n\toperation union\n\torientation -45 0 0\n\tposition 0 0.7 -0.3\n\tmaterial mirror_mat\n}\n";

//-----------------------------------------------------------------------------
// Test J (P1a): an ON-SCREEN reflective csg_object mirror driving a k=1 SMS
// reflection chain must not silently feed ManifoldSolver::UniformRandomPoint's
// fabricated (wrong-position) fallback point into Newton as a manifold seed.
//
// ASSERTION STRATEGY: a full behavioral assertion (proving Newton converges
// to a WRONG root, or that the caustic energy is measurably off) would
// require either a reference-image comparison or deep solver
// instrumentation neither of which is proportionate to a lifetime/
// null-safety fix -- per the reviewer's own stated fallback, this is a
// TARGETED LOG assertion that the null-geometry gate fires: ManifoldSolver.cpp
// logs a distinctive, log-once diagnostic (containing the token
// "NULL_GEOM_REFLECTION_FALLBACK_REFUSED") exactly when every OTHER
// surfaceSampleReflectionFallback condition holds (k=1, reflective, a
// caster exists) but the object's GetGeometry() is null -- i.e. exactly the
// "on-screen reflective CSG mirror drove a k=1 chain" case this test
// constructs.  RenderSceneCapturingStdout captures the render's stdout and
// this test asserts that diagnostic string is present.
//
// RED-PROOF (see the fix round's session report for the stash sequence):
// stashing ONLY ManifoldSolver.cpp (keeping Object.cpp's base guard from
// fix round 2) reproduces the PRE-P1a-fix state -- the render still does
// NOT crash (Object::UniformRandomPoint's base guard silently absorbs the
// null pGeometry), but the new diagnostic is ABSENT (the gate + its log
// line are P1a's new code) -- i.e. exactly the "silently feeds the
// fabricated point, not zero-measure" defect the reviewer described.
// Post-fix, the diagnostic IS present.
//-----------------------------------------------------------------------------
static void TestSMSOnScreenReflectiveCSGMirrorFallbackGateFires()
{
	std::cout << "Test J: on-screen reflective csg_object mirror -- k=1 SMS fallback null-geometry gate fires...\n";

	const RenderProbeResultWithLog r = RenderSceneCapturingStdout( kSMSMirrorCSGScene, "sms_mirror_csg" );
	Check( r.rendered, "SMS mirror-csg scene rendered without crashing" );
	Check( r.capturedOutput.find( "NULL_GEOM_REFLECTION_FALLBACK_REFUSED" ) != std::string::npos,
		"ManifoldSolver's null-geometry reflection-fallback gate fired (diagnostic observed in captured stdout)" );
}

int main()
{
	TestCSGEmissiveObjectDoesNotCrash();
	TestCSGEmissiveObjectSurvivesRerender();
	TestCSGLuminaireGlowsUnderPathTracing();
	TestCSGLuminaireGlowsUnderBDPT();
	TestCSGLuminaireGlowsUnderVCM();
	TestValidateSurfacesLuminaireNullGeometryWarning();
	TestObjectGetAreaAndUniformRandomPointDirectOnNullGeometry();
	TestCSGEmissiveUnderEmissionShaderOpChain();
	TestCSGSimpleSSSDoesNotCrash();
	TestCSGDonnerJensenSSSDoesNotCrash();
	TestSMSOffscreenCSGDoesNotCrash();
	TestSMSOnScreenReflectiveCSGMirrorFallbackGateFires();

	for( const std::string& f : g_tmpScenes ) { std::remove( f.c_str() ); }

	if( g_failures == 0 ) {
		std::cout << "All CSGNullGeometryLuminaireCrash tests passed.\n";
		return 0;
	}
	std::cout << g_failures << " CSGNullGeometryLuminaireCrash check(s) FAILED.\n";
	return 1;
}
