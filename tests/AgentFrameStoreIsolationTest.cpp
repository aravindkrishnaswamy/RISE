//////////////////////////////////////////////////////////////////////
//
//  AgentFrameStoreIsolationTest.cpp - offscreen isolation for
//    AGENT/LLM renders (2026-07).
//
//    ROOT CAUSE this proves fixed: AgentSession's `render` verb used
//    to run mJob->Rasterize() directly on the PRODUCTION rasterizer,
//    which writes into its canonical Implementation::FrameStore beauty
//    buffer -- the SAME FrameStore a GUI's ViewportFrameStore binds to
//    via FrameStore::AddObserver, independent of the `outs` sink list
//    AgentSession already swapped (RemoveRasterizerOutputs +
//    AddRasterizerOutput(InMemoryRasterizerOutput)).  An agent render's
//    pixels therefore landed in the display buffer the interactive
//    viewport reads and re-composites on a window resize.
//
//    The fix (AgentSession.cpp's doRenderWork) installs a PRIVATE,
//    unobserved FrameStore for the duration of an agent render -- a
//    freshly allocated one sized to the current film dims when no
//    film-dims override was requested, or (for real) the fresh store
//    Job::SetFilm's PushJobFrameStoreToRasterizers allocates when an
//    override WAS requested -- and restores the rasterizer's FrameStore
//    POINTER IDENTITY back to the exact object a caller's observer is
//    bound to once the render completes, even across a throw.
//
//    This file proves, for BOTH a no-override render and a film-dims-
//    override render:
//      * the render still succeeds (ok, non-empty PNG);
//      * the CANONICAL (pre-render) FrameStore's Generation() does NOT
//        advance across the agent render (BeginTile/EndTile/
//        MarkFrameComplete never touched it);
//      * a probe IRenderObserver attached to that canonical FrameStore
//        BEFORE the render receives ZERO OnTileComplete/OnFrameComplete
//        callbacks during the render (the strongest proof -- an
//        observer watching for exactly the symptom a real
//        ViewportFrameStore would react to);
//      * the rasterizer's GetFrameStore() is IDENTICALLY the captured
//        canonical FrameStore pointer again after the call (identity
//        restored, not just "some non-null store");
//      * for the override case specifically, that restored FrameStore's
//        Width()/Height() are back to the scene's ORIGINAL film dims
//        (proving the "override restore reallocates a DIFFERENT object"
//        trap is closed, not just papered over).
//
//    Uses the scene's DEFAULT rasterizer (PT, path-traced) for the main
//    assertions, then re-runs the same probe against bdpt_pel_rasterizer,
//    vcm_pel_rasterizer, AND mlt_rasterizer scenes to confirm the fix
//    generalizes across the pixel-based integrator family (all four
//    write through the same canonical FrameStore -- PT/BDPT/VCM via
//    AcquireRenderImage's dims-exact-match gate, per
//    PixelBasedRasterizerHelper.cpp / BDPTRasterizerBase.cpp /
//    VCMRasterizerBase.cpp; MLT via its own per-round Flush* path since
//    it opted back INTO the FrameStore push in commit 36809dcf
//    "L6d-2b" -- see MLTRasterizer.h's AcceptsFrameStorePush doc).
//
//    Fix-round P1-1 / P1-A additions (offscreen-isolation fix round on
//    commit 81cdbadd):
//      * RunIsolationProbe's Case 3 renders with an override whose
//        width/height EQUAL the scene's current film dims -- the
//        regression lock for P1-1 (the private-store install used to be
//        gated on `!wantFilmOverride`, so a same-dims override silently
//        relied on Job::SetFilm's same-dims short-circuit, which never
//        pushes a fresh FrameStore -- an agent override-render at the
//        scene's current resolution used to paint straight into the
//        shared display store).
//      * RunThrowDuringOverrideTest (below) uses
//        AgentSession::ForTest_SetThrowBeforeRasterize to force a throw
//        from inside doRenderWork, immediately before Rasterize(), during
//        an OVERRIDE render -- the regression lock for P1-A (the
//        FrameStoreIsolationGuard / RenderOverrideRestoreGuard
//        construction-order bug: constructing the FrameStore guard LAST
//        made it destruct FIRST on unwind, so the film-dims restore's
//        SetFilm-driven FrameStore reallocation ran AFTER the identity
//        restore had already dropped its ref -- a reproduced SIGABRT
//        use-after-free).
//
//    Self-contained: inline native-v7 scenes (a lit sphere), OIDN off,
//    written to scratch temp files (LoadAsciiSceneViaCst takes a
//    filename) -- no RISE_MEDIA_PATH, no scenes/ tree file touched.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRenderObserver.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/Rasterizer.h"
#include "../src/Library/Rendering/AutoRasterizer.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
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

// A small lit diffuse sphere, mirroring AgentRenderAsyncTest.cpp's
// kScene / AgentProposeRenderTest.cpp's canonical test scene shape.
// `%s` is substituted with the rasterizer chunk text so the same scene
// shape drives PT / BDPT / VCM.
static std::string BuildScene( const std::string& rasterizerChunk )
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ rasterizerChunk + "\n\n"
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
}

// Full-frame glass slab at z=[1.4,1.6], opaque backstop at z=[-0.1,0.1],
// camera at z=3.5.  The first camera hit is therefore about 1.9 units away;
// Accurate albedo/normal may legitimately select the backstop, but depth must
// remain the primary glass hit for every integrator and wavelength strategy.
static std::string BuildDepthContractScene( const std::string& rasterizerChunk )
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ rasterizerChunk + "\n\n"
		"film\n{\n\twidth 12\n\theight 12\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 12.0\n}\n\n"
		"dielectric_material\n{\n\tname glass_mat\n\ttau 1 1 1\n\tior 1.5\n}\n\n"
		"box_geometry\n{\n\tname glass_geo\n\twidth 10\n\theight 10\n\tdepth 0.2\n}\n\n"
		"standard_object\n{\n\tname glass_obj\n\tgeometry glass_geo\n\tmaterial glass_mat\n\tposition 0 0 1.5\n}\n\n"
		"uniformcolor_painter\n{\n\tname back_albedo\n\tcolor 0.3 0.6 0.2\n}\n\n"
		"lambertian_material\n{\n\tname back_mat\n\treflectance back_albedo\n}\n\n"
		"box_geometry\n{\n\tname back_geo\n\twidth 10\n\theight 10\n\tdepth 0.2\n}\n\n"
		"standard_object\n{\n\tname back_obj\n\tgeometry back_geo\n\tmaterial back_mat\n}\n";
}

// The same camera geometry inside dense global fog. Nearly every beauty path
// scatters before reaching the slab, but depth must still describe the raw
// camera intersection rather than the redirected post-medium path.
static std::string BuildMediumDepthContractScene( const std::string& rasterizerChunk )
{
	return BuildDepthContractScene( rasterizerChunk ) +
		"\nhomogeneous_medium\n{\n\tname dense_fog\n\tabsorption 0.01 0.01 0.01\n"
		"\tscattering 5 5 5\n\tphase isotropic\n}\n\n"
		"global_medium\n{\n\tmedium dense_fog\n}\n";
}

// A sphere against empty background forces partially covered silhouette
// pixels. Depth must average only hit samples: misses are a coverage mask,
// not zero-distance measurements that pull depthMin toward the camera.
static std::string BuildSilhouetteDepthScene( const std::string& rasterizerChunk )
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ rasterizerChunk + "\n\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30\n}\n\n"
		"uniformcolor_painter\n{\n\tname gray\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname matte\n\treflectance gray\n}\n\n"
		"sphere_geometry\n{\n\tname sphere\n\tradius 1\n}\n\n"
		"standard_object\n{\n\tname sphere_object\n\tgeometry sphere\n\tmaterial matte\n}\n";
}

// Fully transparent shader-dispatch front slab. The transparency shader casts
// a continuation ray through the same RuntimeContext; recursive path-tracing
// shader invocations must not replace the root slab's camera depth.
static std::string BuildTransparencyDepthContractScene( const std::string& rasterizerChunk )
{
	return
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
		"transparency_shaderop\n{\n\tname transmit\n\ttransparency white\n\tone_sided false\n}\n\n"
		"advanced_shader\n{\n\tname transparent_path\n"
		"\tshaderop DefaultPathTracing 0 100 +\n\tshaderop transmit 0 100 =\n}\n\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		+ rasterizerChunk + "\n\n"
		"film\n{\n\twidth 12\n\theight 12\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 12.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname front_albedo\n\tcolor 0.7 0.2 0.2\n}\n\n"
		"lambertian_material\n{\n\tname front_mat\n\treflectance front_albedo\n}\n\n"
		"box_geometry\n{\n\tname front_geo\n\twidth 10\n\theight 10\n\tdepth 0.2\n}\n\n"
		"standard_object\n{\n\tname front_obj\n\tgeometry front_geo\n\tmaterial front_mat\n"
		"\tshader transparent_path\n\tposition 0 0 1.5\n}\n\n"
		"uniformcolor_painter\n{\n\tname back_albedo\n\tcolor 0.3 0.6 0.2\n}\n\n"
		"lambertian_material\n{\n\tname back_mat\n\treflectance back_albedo\n}\n\n"
		"box_geometry\n{\n\tname back_geo\n\twidth 10\n\theight 10\n\tdepth 0.2\n}\n\n"
		"standard_object\n{\n\tname back_obj\n\tgeometry back_geo\n\tmaterial back_mat\n}\n";
}

static const char* const kPtRasterizer =
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}";
static const char* const kBdptRasterizer =
	"bdpt_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}";
static const char* const kVcmRasterizer =
	"vcm_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}";
// P2: MLT opted BACK INTO the FrameStore push in commit 36809dcf
// ("L6d-2b") -- an ancestor of 81cdbadd -- so AcceptsFrameStorePush() is
// TRUE for MLT and it IS subject to the private-store swap this file
// proves.  Deliberately tiny bootstrap/chains/mutations so the probe
// stays fast on the 24x24 test scene; the RGB (non-spectral)
// `mlt_rasterizer` is used (not `mlt_spectral_rasterizer`) since it
// drives the exact same native-v7 RGB scene shape as the other three
// parametrizations with no changes.
static const char* const kMltRasterizer =
	"mlt_rasterizer\n{\n\tbootstrap_samples 200\n\tchains 4\n\tmutations_per_pixel 4\n\tpixel_filter box\n\toidn_denoise false\n}";
static const char* const kAutoRasterizer =
	"auto_rasterizer\n{\n\tintegrator pt\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n\toidn_prefilter accurate\n}";

// Depth is a geometry/camera fact, independent of the beauty integrator and
// of the surface selected for the albedo/normal prefilter.  Keep this matrix
// deliberately tiny: it crosses shader-dispatch, pure PT, BDPT, and VCM with
// RGB, scalar-wavelength, and HWSS spectral execution paths.
static const char* const kDepthShaderPel =
	"pixelpel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthShaderSpectralNM =
	"pixelintegratingspectral_rasterizer\n{\n\tsamples 2\n\tlum_samples 1\n\tnmbegin 450\n\tnmend 650\n\tnum_wavelengths 3\n\tspectral_samples 1\n\thwss false\n\tmax_recursion 8\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthShaderSpectralHWSS =
	"pixelintegratingspectral_rasterizer\n{\n\tsamples 2\n\tlum_samples 1\n\tnmbegin 450\n\tnmend 650\n\tnum_wavelengths 3\n\tspectral_samples 1\n\thwss true\n\tmax_recursion 8\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthPtPel =
	"pathtracing_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthPtSpectralNM =
	"pathtracing_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\tnmbegin 450\n\tnmend 650\n\tnum_wavelengths 3\n\tspectral_samples 1\n\thwss false\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthPtSpectralHWSS =
	"pathtracing_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\tnmbegin 450\n\tnmend 650\n\tnum_wavelengths 3\n\tspectral_samples 1\n\thwss true\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthBdptPel =
	"bdpt_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthBdptSpectralHWSS =
	"bdpt_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\tnmbegin 450\n\tnmend 650\n\tnum_wavelengths 3\n\tspectral_samples 1\n\thwss true\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthBdptSpectralNM =
	"bdpt_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\tnmbegin 450\n\tnmend 650\n\tnum_wavelengths 3\n\tspectral_samples 1\n\thwss false\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthVcmPel =
	"vcm_pel_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthVcmSpectralHWSS =
	"vcm_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\tnmbegin 450\n\tnmend 650\n\tnum_wavelengths 3\n\tspectral_samples 1\n\thwss true\n\toidn_denoise false\n\toidn_prefilter accurate\n}";
static const char* const kDepthVcmSpectralNM =
	"vcm_spectral_rasterizer\n{\n\tsamples 2\n\tpixel_filter box\n\tnmbegin 450\n\tnmend 650\n\tnum_wavelengths 3\n\tspectral_samples 1\n\thwss false\n\toidn_denoise false\n\toidn_prefilter accurate\n}";

static const char* const kSilhouetteShaderPel =
	"pixelpel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}";
static const char* const kSilhouettePtPel =
	"pathtracing_pel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}";
static const char* const kSilhouetteBdptPel =
	"bdpt_pel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}";
static const char* const kSilhouetteVcmPel =
	"vcm_pel_rasterizer\n{\n\tsamples 16\n\tpixel_filter box\n\toidn_denoise false\n}";

enum DepthSceneKind { kGlassDepth, kMediumDepth, kTransparencyDepth };

static void RunDepthContractProbe( const char* label, const char* rasterizerChunk,
	DepthSceneKind sceneKind = kGlassDepth )
{
	const std::string sceneText = sceneKind == kMediumDepth
		? BuildMediumDepthContractScene( rasterizerChunk )
		: ( sceneKind == kTransparencyDepth
			? BuildTransparencyDepthContractScene( rasterizerChunk )
			: BuildDepthContractScene( rasterizerChunk ) );
	const std::string scenePath = WriteTemp(
		( std::string( "agent_perception_depth_" ) + label + ".RISEscene" ).c_str(),
		sceneText );
	Check( !scenePath.empty(), std::string( label ) + ": depth-contract scene written" );
	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ),
		std::string( label ) + ": depth-contract scene loads" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	AgentRenderParams params;
	params.perception = true;
	const AgentRenderResult render = session ? session->Render( params ) : AgentRenderResult();
	unsigned int atlasW = 0, atlasH = 0;
	AgentPerceptionInfo info;
	const std::vector<unsigned char> atlas = session
		? session->ReadPerception( 24, atlasW, atlasH, info )
		: std::vector<unsigned char>();
	Check( render.ok && !atlas.empty() && info.available,
		std::string( label ) + ": perception render and atlas succeed" );
	Check( info.validDepthPixels == 12u * 12u && info.depthMin > 1.75 && info.depthMax < 2.25,
		std::string( label ) + ": depth stays on the ~1.9-unit primary glass hit" );
	unsigned int invalidW = 99, invalidH = 99;
	AgentPerceptionInfo invalidInfo;
	const std::vector<unsigned char> invalidAtlas = session
		? session->ReadPerception( 1, invalidW, invalidH, invalidInfo )
		: std::vector<unsigned char>();
	Check( invalidAtlas.empty() && !invalidInfo.available && invalidW == 0 && invalidH == 0,
		std::string( label ) + ": unencodable atlas reports unavailable to direct callers" );
	pJob->release();
	std::remove( scenePath.c_str() );
}

static void RunSilhouetteDepthProbe( const char* label, const char* rasterizerChunk )
{
	const std::string scenePath = WriteTemp(
		( std::string( "agent_perception_silhouette_" ) + label + ".RISEscene" ).c_str(),
		BuildSilhouetteDepthScene( rasterizerChunk ) );
	Check( !scenePath.empty(), std::string( label ) + ": silhouette scene written" );
	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ),
		std::string( label ) + ": silhouette scene loads" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	AgentRenderParams params;
	params.perception = true;
	const AgentRenderResult render = session ? session->Render( params ) : AgentRenderResult();
	unsigned int atlasW = 0, atlasH = 0;
	AgentPerceptionInfo info;
	const std::vector<unsigned char> atlas = session
		? session->ReadPerception( 48, atlasW, atlasH, info )
		: std::vector<unsigned char>();
	Check( render.ok && !atlas.empty() && info.available,
		std::string( label ) + ": silhouette perception render succeeds" );
	Check( info.validDepthPixels > 0 && info.validDepthPixels < 24u * 24u &&
	       info.depthMin > 4.9 && info.depthMax < 6.1,
		std::string( label ) + ": silhouette misses do not dilute ~5-6-unit hit depth" );
	pJob->release();
	std::remove( scenePath.c_str() );
}

// Probe IRenderObserver: counts OnTileComplete / OnFrameComplete calls.
// Attached to the CANONICAL (display) FrameStore before an agent render;
// the isolation fix means it must observe EXACTLY ZERO of either during
// that render (the agent render must be writing into a DIFFERENT,
// private FrameStore the whole time).
struct ProbeObserver : public IRenderObserver
{
	int tileCompleteCount = 0;
	int frameCompleteCount = 0;

	void OnTileComplete( const Rect&, uint64_t ) override { ++tileCompleteCount; }
	void OnFrameComplete( unsigned, uint64_t ) override { ++frameCompleteCount; }
};

// Runs the full isolation probe against one rasterizer chunk text,
// tagging every Check() message with `label` for a legible failure
// report.  `expectOverrideWorks` lets the BDPT/VCM parametrizations
// (which may not honour the width/height override the same way PT
// does) report a finding rather than silently skip -- see the call
// sites' comments.
static void RunIsolationProbe( const std::string& label, const std::string& rasterizerChunk,
	const char* expectedPrefilter = "fast" )
{
	const std::string scenePath = WriteTemp(
		( "agent_framestore_isolation_" + label + ".RISEscene" ).c_str(),
		BuildScene( rasterizerChunk ) );
	Check( !scenePath.empty(), label + ": scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), label + ": scene loads via the CST path" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, label + ": AgentSession::WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	IRasterizer* rast = pJob->GetRasterizer();
	Check( rast != nullptr, label + ": Job has an active rasterizer" );
	if( !rast ) { pJob->release(); return; }

	Implementation::Rasterizer* concreteRast = dynamic_cast<Implementation::Rasterizer*>( rast );
	Check( concreteRast != nullptr, label + ": active rasterizer is an Implementation::Rasterizer" );
	if( !concreteRast ) { pJob->release(); return; }

	// Force the canonical FrameStore into existence (mirrors what
	// Job::LoadAsciiSceneViaCst's post-load PushJobFrameStoreToRasterizers
	// already does for an active camera + film) and capture it as "the
	// display store" a GUI's ViewportFrameStore would have attached to.
	Implementation::FrameStore* displayStore = concreteRast->GetFrameStore();
	Check( displayStore != nullptr, label + ": rasterizer has a canonical FrameStore before any render" );
	if( !displayStore ) { pJob->release(); return; }

	const IScenePriv* scenePriv = pJob->GetScene();
	const IFilm* film = scenePriv ? scenePriv->GetFilm() : nullptr;
	Check( film != nullptr, label + ": scene has a Film" );
	const unsigned int origW = film ? film->GetWidth()  : 0;
	const unsigned int origH = film ? film->GetHeight() : 0;
	Check( displayStore->Width()  == origW, label + ": canonical FrameStore width matches the film's authored width" );
	Check( displayStore->Height() == origH, label + ": canonical FrameStore height matches the film's authored height" );

	// ---- Case 1: NO-OVERRIDE render ------------------------------------
	{
		const uint64_t genBefore = displayStore->Generation();

		ProbeObserver probe;
		displayStore->AddObserver( &probe );

		AgentRenderParams params;
		params.perception = true;
		AgentRenderResult res = session->Render( params );

		displayStore->RemoveObserver( &probe );

		Check( res.ok, label + " (no-override): render succeeds" );
		Check( !res.png.empty(), label + " (no-override): PNG bytes are non-empty" );
		Check( res.perceptionAvailable,
			label + " (no-override): explicit perception is available for this rasterizer family" );
		Check( res.perceptionPersistentBytes == 24u * 24u * 7u &&
		       res.perceptionAuxiliaryPeakBytes == 24u * 24u * 84u,
			label + " (no-override): perception reports exact compact/peak payload bytes" );
		unsigned int atlasW = 0, atlasH = 0;
		AgentPerceptionInfo perceptionInfo;
		const std::vector<unsigned char> atlas = session->ReadPerception(
			0, atlasW, atlasH, perceptionInfo );
		Check( !atlas.empty() && perceptionInfo.available && atlasW == 48 && atlasH == 48,
			label + " (no-override): perception atlas is cached at native 2x2 dimensions" );
		Check( perceptionInfo.validDepthPixels > 0 && perceptionInfo.depthMin > 0.0 &&
		       perceptionInfo.depthMax >= perceptionInfo.depthMin,
			label + " (no-override): perception depth is populated and ordered" );
		Check( perceptionInfo.guidePrefilter == expectedPrefilter,
			label + " (no-override): guidePrefilter reports the configured producer semantics" );
		Check( displayStore->Generation() == genBefore,
			label + " (no-override): canonical FrameStore Generation() did NOT advance across the agent render" );
		Check( probe.tileCompleteCount == 0,
			label + " (no-override): probe observer saw ZERO OnTileComplete callbacks on the canonical store" );
		Check( probe.frameCompleteCount == 0,
			label + " (no-override): probe observer saw ZERO OnFrameComplete callbacks on the canonical store" );
		Check( concreteRast->GetFrameStore() == displayStore,
			label + " (no-override): rasterizer's FrameStore identity is restored to the captured display store" );
		if( Implementation::AutoRasterizer* autoRast =
				dynamic_cast<Implementation::AutoRasterizer*>( concreteRast ) ) {
			Check( autoRast->ForTest_GetDelegateFrameStore() == displayStore,
				label + " (no-override): auto delegate's FrameStore identity is restored immediately" );
		}
	}

	// ---- Case 2: FILM-DIMS-OVERRIDE render -----------------------------
	{
		const uint64_t genBefore = displayStore->Generation();

		ProbeObserver probe;
		displayStore->AddObserver( &probe );

		AgentRenderParams params;
		params.width  = 12;
		params.height = 12;
		AgentRenderResult res = session->Render( params );

		displayStore->RemoveObserver( &probe );

		Check( res.ok, label + " (override): render succeeds" );
		Check( !res.png.empty(), label + " (override): PNG bytes are non-empty" );
		Check( res.width == 12 && res.height == 12, label + " (override): render ran at the overridden dims" );
		Check( displayStore->Generation() == genBefore,
			label + " (override): canonical FrameStore Generation() did NOT advance across the agent render" );
		Check( probe.tileCompleteCount == 0,
			label + " (override): probe observer saw ZERO OnTileComplete callbacks on the canonical store" );
		Check( probe.frameCompleteCount == 0,
			label + " (override): probe observer saw ZERO OnFrameComplete callbacks on the canonical store" );
		Check( concreteRast->GetFrameStore() == displayStore,
			label + " (override): rasterizer's FrameStore identity is restored to the captured display store" );

		Implementation::FrameStore* restored = concreteRast->GetFrameStore();
		if( restored ) {
			Check( static_cast<unsigned int>( restored->Width() )  == origW &&
			       static_cast<unsigned int>( restored->Height() ) == origH,
				label + " (override): restored FrameStore dims are back to the ORIGINAL film dims (F2 override-rebind-and-strand closed)" );
		}

		// A plain Render(-1) right after the override render must also see
		// the Document/Film back at the original dims (no permanent
		// mutation leaked out of the override window).
		const IFilm* filmAfter = scenePriv ? scenePriv->GetFilm() : nullptr;
		Check( filmAfter && filmAfter->GetWidth() == origW && filmAfter->GetHeight() == origH,
			label + " (override): scene Film dims restored to original after the override render" );
	}

	// ---- Case 3: SAME-DIMS OVERRIDE render (P1-1 regression lock) -----
	//
	// A render whose requested params.width/height EQUAL the scene's
	// CURRENT film dims still counts as `wantFilmOverride == true`
	// (AgentSession.cpp: `wantFilmOverride = (params.width > 0 &&
	// params.height > 0)` -- it does not compare against the current
	// dims).  Pre-P1-1-fix, the private-store install was gated on
	// `!wantFilmOverride`, so THIS case relied entirely on
	// Job::SetFilm's own PushJobFrameStoreToRasterizers to supply a
	// fresh FrameStore -- but Job::SetFilm short-circuits and returns
	// true WITHOUT pushing anything when the requested dims already
	// match the current Film (Job.cpp's "Same-dim short-circuit"
	// block).  Net effect pre-fix: this exact case painted straight
	// into the shared display FrameStore.  Must FAIL if the
	// unconditional private-store install regresses back to being
	// gated on `!wantFilmOverride`.
	{
		const uint64_t genBefore = displayStore->Generation();

		ProbeObserver probe;
		displayStore->AddObserver( &probe );

		AgentRenderParams params;
		params.width  = origW;
		params.height = origH;
		AgentRenderResult res = session->Render( params );

		displayStore->RemoveObserver( &probe );

		Check( res.ok, label + " (same-dims override): render succeeds" );
		Check( !res.png.empty(), label + " (same-dims override): PNG bytes are non-empty" );
		Check( res.width == origW && res.height == origH,
			label + " (same-dims override): render ran at the (unchanged) overridden dims" );
		Check( displayStore->Generation() == genBefore,
			label + " (same-dims override) P1-1 LOCK: canonical FrameStore Generation() did NOT advance across the agent render" );
		Check( probe.tileCompleteCount == 0,
			label + " (same-dims override) P1-1 LOCK: probe observer saw ZERO OnTileComplete callbacks on the canonical store" );
		Check( probe.frameCompleteCount == 0,
			label + " (same-dims override) P1-1 LOCK: probe observer saw ZERO OnFrameComplete callbacks on the canonical store" );
		Check( concreteRast->GetFrameStore() == displayStore,
			label + " (same-dims override): rasterizer's FrameStore identity is restored to the captured display store" );
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// P1-A regression lock: THROW-DURING-OVERRIDE.
//
// Forces AgentSession::RenderCore_'s doRenderWork to throw a
// std::runtime_error immediately before mJob->Rasterize() -- via the
// ForTest_SetThrowBeforeRasterize test-only seam -- during an OVERRIDE
// render (so the film-dims override + private-store install have
// already run by the time the throw unwinds the lambda).  This is
// exactly the code shape a real OIDN-class throw hits.
//
// The money assertions target the FrameStoreIsolationGuard /
// RenderOverrideRestoreGuard construction-order bug: pre-fix,
// FrameStoreIsolationGuard was constructed LAST (so it destructed
// FIRST on unwind), meaning the film-dims restore's
// SetFilm(origW, origH, ...) reallocated a brand new FrameStore AFTER
// the identity-restore had already dropped its own ref on the
// captured display store, orphaning it (a live-lldb session
// reproduced a SIGABRT use-after-free in FrameStore::RemoveObserver
// from exactly this shape).  With the fix (FrameStoreIsolationGuard
// constructed FIRST, so it destructs LAST), the display store survives
// the film-dims restore and is correctly rebound.
//
// Model-B F2 slice S2a note: RenderCore_ does NOT let a thrown
// exception escape as a raw C++ exception -- it is already caught
// inside RenderCore_ and reported as an ordinary AgentRenderResult with
// ok=false (see AgentSession.cpp's "S1-delta doc-truth fix" comment).
// So "it threw" is verified via res.ok/res.message, not a caught C++
// exception at this call site -- the try/catch below is a defensive
// belt in case a future regression lets the exception escape raw
// (which would itself be worth knowing about, hence CATCHING it rather
// than letting the test process abort).
//////////////////////////////////////////////////////////////////////
static void RunThrowDuringOverrideTest()
{
	std::printf( "=== AgentFrameStoreIsolationTest: P1-A throw-during-override (no UAF) ===\n" );

	const std::string scenePath = WriteTemp(
		"agent_framestore_isolation_throw.RISEscene", BuildScene( kPtRasterizer ) );
	Check( !scenePath.empty(), "throw-during-override: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "throw-during-override: scene loads via the CST path" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "throw-during-override: AgentSession::WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	IRasterizer* rast = pJob->GetRasterizer();
	Implementation::Rasterizer* concreteRast = rast ? dynamic_cast<Implementation::Rasterizer*>( rast ) : nullptr;
	Check( concreteRast != nullptr, "throw-during-override: active rasterizer is an Implementation::Rasterizer" );
	if( !concreteRast ) { pJob->release(); return; }

	Implementation::FrameStore* displayStore = concreteRast->GetFrameStore();
	Check( displayStore != nullptr, "throw-during-override: rasterizer has a canonical FrameStore before any render" );
	if( !displayStore ) { pJob->release(); return; }

	const IScenePriv* scenePriv = pJob->GetScene();
	const IFilm* film = scenePriv ? scenePriv->GetFilm() : nullptr;
	const unsigned int origW = film ? film->GetWidth()  : 0;
	const unsigned int origH = film ? film->GetHeight() : 0;
	Check( origW == 24 && origH == 24, "throw-during-override: pre-override Film dims are the scene's authored 24x24" );

	const uint64_t genBefore = displayStore->Generation();

	// Arm the test-only seam, then run an OVERRIDE render (dims DIFFERENT
	// from the current film, so the film-dims-restore's SetFilm actually
	// reallocates a FrameStore -- the exact shape the P1-A bug needed).
	session->ForTest_SetThrowBeforeRasterize( true );

	AgentRenderParams params;
	params.width  = 16;
	params.height = 16;

	AgentRenderResult res;
	bool escaped = false;
	std::string escapedWhat;
	try {
		res = session->Render( params );
	}
	catch( const std::exception& e ) { escaped = true; escapedWhat = e.what(); }
	catch( ... )                     { escaped = true; escapedWhat = "unknown exception"; }

	// (a) "it threw": RenderCore_ catches the seam's throw internally and
	// reports it as an ordinary ok=false result (Model-B F2 slice S2a
	// doc-truth fix) -- it must NOT escape as a raw C++ exception (that
	// would itself be a regression, hence failing loudly here instead of
	// silently accepting either shape).
	Check( !escaped, "throw-during-override: the forced throw did NOT escape RenderCore_ as a raw C++ exception" );
	if( escaped ) {
		std::printf( "  (raw exception escaped: %s)\n", escapedWhat.c_str() );
	}
	Check( !res.ok, "throw-during-override: render reports ok=false (the seam's throw actually fired)" );
	Check( res.message.find( "ForTest_ThrowBeforeRasterize" ) != std::string::npos,
		"throw-during-override: failure message names the forced test-seam throw" );

	// (b) NO CRASH: reaching this line at all (the process is still
	// running, the test binary hasn't SIGABRT'd) IS the assertion -- the
	// live reviewer reproduced a SIGABRT use-after-free in
	// FrameStore::RemoveObserver from exactly this call shape pre-fix.
	Check( true, "throw-during-override: process did NOT crash (SIGABRT/UAF closed)" );

	// (c) identity restored: the rasterizer's FrameStore is back to the
	// EXACT captured display-store pointer, even though the film-dims
	// restore (inside RenderOverrideRestoreGuard's destructor, which ran
	// BEFORE FrameStoreIsolationGuard's per the fixed construction order)
	// reallocated an intermediate FrameStore along the way.
	Check( concreteRast->GetFrameStore() == displayStore,
		"throw-during-override: rasterizer's FrameStore identity is restored to the captured display store after the throw" );

	// (d) Generation() unchanged: the throw fires BEFORE mJob->Rasterize()
	// ever runs, so no BeginTile/EndTile/MarkFrameComplete touched
	// anything -- the canonical store's generation counter must be
	// bit-for-bit where it started.
	Check( displayStore->Generation() == genBefore,
		"throw-during-override: canonical FrameStore Generation() did NOT advance" );

	// (e) the display store is still ALIVE and USABLE: read its dims (a
	// freed/UAF object would be undefined behaviour here, not merely a
	// wrong answer) and confirm they are the ORIGINAL 24x24 -- proving
	// FrameStoreIsolationGuard's own addref (taken before either guard is
	// constructed) kept it alive through RenderOverrideRestoreGuard's
	// SetFilm-driven reallocation.
	Check( static_cast<unsigned int>( displayStore->Width() )  == origW &&
	       static_cast<unsigned int>( displayStore->Height() ) == origH,
		"throw-during-override: the display FrameStore is still ALIVE and reports its ORIGINAL 24x24 dims (no UAF)" );

	const IFilm* filmAfter = scenePriv ? scenePriv->GetFilm() : nullptr;
	Check( filmAfter && filmAfter->GetWidth() == origW && filmAfter->GetHeight() == origH,
		"throw-during-override: scene Film dims restored to original after the throwing override render" );

	// RED-PROVE the red-prove: the session must still be USABLE after the
	// throw -- disarm the seam and confirm a clean follow-up render
	// succeeds (the guard's restore did not itself leave the Job/
	// rasterizer in a broken state).
	session->ForTest_SetThrowBeforeRasterize( false );
	AgentRenderResult clean = session->Render( -1 );
	Check( clean.ok, "throw-during-override: a follow-up non-throwing Render() succeeds after the earlier throw+restore" );
	Check( clean.width == origW && clean.height == origH,
		"throw-during-override: follow-up render is at the authored 24x24 (no lingering override)" );

	std::printf( "=== P1-A throw-during-override: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );

	pJob->release();
	std::remove( scenePath.c_str() );
}

// Round-2 P3 follow-up: the NO-OVERRIDE throw path.  The override case
// (above) is the one that exposed the P1-A UAF -- because
// RenderOverrideRestoreGuard's SetFilm reallocates a FrameStore on unwind.
// The no-override path has no SetFilm restore, so the guard-order bug does
// not bite the same way; but the FrameStoreIsolationGuard destructor still
// runs on a throw here (it installed a private store), and this locks in
// that it restores the display store's identity + leaves it alive without
// the film-restore machinery in play.
static void RunThrowNoOverrideTest()
{
	std::printf( "=== AgentFrameStoreIsolationTest: no-override throw (guard-only restore) ===\n" );

	const std::string scenePath = WriteTemp(
		"agent_framestore_isolation_throw_noov.RISEscene", BuildScene( kPtRasterizer ) );
	Check( !scenePath.empty(), "throw-no-override: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "throw-no-override: scene loads via the CST path" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "throw-no-override: AgentSession::WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	IRasterizer* rast = pJob->GetRasterizer();
	Implementation::Rasterizer* concreteRast = rast ? dynamic_cast<Implementation::Rasterizer*>( rast ) : nullptr;
	Check( concreteRast != nullptr, "throw-no-override: active rasterizer is an Implementation::Rasterizer" );
	if( !concreteRast ) { pJob->release(); return; }

	Implementation::FrameStore* displayStore = concreteRast->GetFrameStore();
	Check( displayStore != nullptr, "throw-no-override: rasterizer has a canonical FrameStore before any render" );
	if( !displayStore ) { pJob->release(); return; }

	const IScenePriv* scenePriv = pJob->GetScene();
	const IFilm* film = scenePriv ? scenePriv->GetFilm() : nullptr;
	const unsigned int origW = film ? film->GetWidth()  : 0;
	const unsigned int origH = film ? film->GetHeight() : 0;
	const uint64_t genBefore = displayStore->Generation();

	// Arm the seam, then run a NO-OVERRIDE render (default params -> the
	// private-store install is OUR explicit one; no SetFilm realloc).
	session->ForTest_SetThrowBeforeRasterize( true );

	AgentRenderResult res;
	bool escaped = false;
	try { res = session->Render( -1 ); }
	catch( ... ) { escaped = true; }

	Check( !escaped, "throw-no-override: the forced throw did NOT escape RenderCore_ as a raw C++ exception" );
	Check( !res.ok, "throw-no-override: render reports ok=false (the seam fired)" );
	Check( true, "throw-no-override: process did NOT crash (SIGABRT/UAF)" );
	Check( concreteRast->GetFrameStore() == displayStore,
		"throw-no-override: rasterizer's FrameStore identity is restored to the captured display store" );
	Check( displayStore->Generation() == genBefore,
		"throw-no-override: canonical FrameStore Generation() did NOT advance" );
	Check( static_cast<unsigned int>( displayStore->Width() )  == origW &&
	       static_cast<unsigned int>( displayStore->Height() ) == origH,
		"throw-no-override: the display FrameStore is still ALIVE with its original dims (no UAF)" );

	// Usable after: disarm and confirm a clean follow-up render.
	session->ForTest_SetThrowBeforeRasterize( false );
	AgentRenderResult clean = session->Render( -1 );
	Check( clean.ok, "throw-no-override: a follow-up non-throwing Render() succeeds after the throw+restore" );

	std::printf( "=== no-override throw: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// Toolkit slice 2 (quality:"draft"): a draft render constructs its OWN
// ephemeral InteractiveMaterialPreviewPipeline and must NEVER reference
// the PRODUCTION rasterizer, its FrameStore, or its `outs` sink list --
// a STRONGER claim than the override-restore proofs above (which show
// identity is RESTORED after being touched).  This proves the
// production rasterizer is never touched AT ALL during a draft render:
// its FrameStore Generation() doesn't advance, a probe observer on that
// FrameStore sees zero callbacks, its FrameStore pointer identity never
// even changes (nothing to "restore" because nothing was swapped), and
// -- the strongest of the four -- its own `outs` sink-list ENTRY COUNT
// is unchanged (the production path calls mJob->RemoveRasterizerOutputs()
// + rast->AddRasterizerOutput(sink) on THIS SAME rasterizer; the draft
// path attaches its sink to the wholly separate ephemeral instance
// instead, so `rast`'s own outs list is never touched).
//////////////////////////////////////////////////////////////////////

// Counts IRasterizerOutput entries via IRasterizer::EnumerateRasterizerOutputs
// -- the only public way to observe a rasterizer's own `outs` list size
// without a friend/internal accessor.
struct CountingOutputsEnumCallback : public IEnumCallback<IRasterizerOutput>
{
	int count = 0;
	bool operator()( const IRasterizerOutput& ) override { ++count; return true; }
};

static void RunProductionSinkDetachmentProbe( const char* label, const char* rasterizerChunk )
{
	std::printf( "=== AgentFrameStoreIsolationTest: transactional production-sink detachment (%s) ===\n", label );

	const std::string tempName = std::string( "agent_framestore_sink_detachment_" ) + label + ".RISEscene";
	const std::string scenePath = WriteTemp(
		tempName.c_str(), BuildScene( rasterizerChunk ) );
	Check( !scenePath.empty(), "sink-detachment: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "sink-detachment: scene loads via the CST path" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "sink-detachment: AgentSession wraps the locally-owned Job" );
	IRasterizer* rast = pJob->GetRasterizer();
	Check( rast != nullptr, "sink-detachment: Job has an active rasterizer" );

	if( session && rast ) {
		AgentRenderParams params;
		params.perception = true;
		const AgentRenderResult good = session->Render( params );
		Check( good.ok && good.perceptionAvailable,
			"sink-detachment: successful production render publishes a perception observation" );

		CountingOutputsEnumCallback afterSuccess;
		rast->EnumerateRasterizerOutputs( afterSuccess );
		Check( afterSuccess.count == 0,
			"sink-detachment: successful render leaves no full observation attached to the rasterizer" );

		session->ForTest_SetThrowBeforeRasterize( true );
		const AgentRenderResult failed = session->Render( params );
		Check( !failed.ok, "sink-detachment: forced production failure reaches the rollback path" );
		CountingOutputsEnumCallback afterFailure;
		rast->EnumerateRasterizerOutputs( afterFailure );
		Check( afterFailure.count == 0,
			"sink-detachment: failed render leaves no partial observation attached to the rasterizer" );
		session->ForTest_SetThrowBeforeRasterize( false );

		unsigned int atlasW = 0, atlasH = 0;
		AgentPerceptionInfo info;
		const std::vector<unsigned char> prior = session->ReadPerception( 16, atlasW, atlasH, info );
		Check( info.available && !prior.empty(),
			"sink-detachment: rollback preserves the session-owned last successful observation" );
	}

	pJob->release();
	std::remove( scenePath.c_str() );
	std::printf( "=== production-sink detachment (%s): %d passed, %d failed (cumulative) ===\n",
		label, g_pass, g_fail );
}

static void RunDraftIsolationTest()
{
	std::printf( "=== AgentFrameStoreIsolationTest: Toolkit slice 2 draft-quality isolation ===\n" );

	const std::string scenePath = WriteTemp(
		"agent_framestore_isolation_draft.RISEscene", BuildScene( kPtRasterizer ) );
	Check( !scenePath.empty(), "draft-isolation: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "draft-isolation: scene loads via the CST path" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "draft-isolation: AgentSession::WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	IRasterizer* rast = pJob->GetRasterizer();
	Check( rast != nullptr, "draft-isolation: Job has an active (production) rasterizer" );
	if( !rast ) { pJob->release(); return; }

	Implementation::Rasterizer* concreteRast = dynamic_cast<Implementation::Rasterizer*>( rast );
	Check( concreteRast != nullptr, "draft-isolation: active rasterizer is an Implementation::Rasterizer" );
	if( !concreteRast ) { pJob->release(); return; }

	Implementation::FrameStore* displayStore = concreteRast->GetFrameStore();
	Check( displayStore != nullptr, "draft-isolation: production rasterizer has a canonical FrameStore before any render" );
	if( !displayStore ) { pJob->release(); return; }

	const uint64_t genBefore = displayStore->Generation();

	ProbeObserver probe;
	displayStore->AddObserver( &probe );

	CountingOutputsEnumCallback outsBefore;
	rast->EnumerateRasterizerOutputs( outsBefore );

	AgentRenderParams params;
	params.quality = AgentRenderQuality::Draft;
	AgentRenderResult res = session->Render( params );

	displayStore->RemoveObserver( &probe );

	Check( res.ok, "draft-isolation: the draft render succeeds" );
	Check( res.renderMode == "draft", "draft-isolation: the result reports renderMode==\"draft\"" );
	Check( !res.png.empty(), "draft-isolation: PNG bytes are non-empty" );

	Check( displayStore->Generation() == genBefore,
		"draft-isolation MONEY ASSERTION: the PRODUCTION rasterizer's canonical FrameStore Generation() did NOT advance -- "
		"never referenced by the draft render at all" );
	Check( probe.tileCompleteCount == 0,
		"draft-isolation MONEY ASSERTION: probe observer on the production FrameStore saw ZERO OnTileComplete callbacks" );
	Check( probe.frameCompleteCount == 0,
		"draft-isolation MONEY ASSERTION: probe observer on the production FrameStore saw ZERO OnFrameComplete callbacks" );
	Check( concreteRast->GetFrameStore() == displayStore,
		"draft-isolation MONEY ASSERTION: the production rasterizer's FrameStore IDENTITY is untouched "
		"(still the SAME pointer -- nothing was ever swapped away and back)" );

	CountingOutputsEnumCallback outsAfter;
	rast->EnumerateRasterizerOutputs( outsAfter );
	Check( outsAfter.count == outsBefore.count,
		"draft-isolation MONEY ASSERTION (strongest): the production rasterizer's OWN `outs` sink-list entry count is "
		"UNCHANGED -- a draft render attaches its sink to a wholly SEPARATE ephemeral rasterizer instance, never "
		"calling mJob->RemoveRasterizerOutputs() / rast->AddRasterizerOutput() on THIS rasterizer at all" );

	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== draft-quality isolation: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// Round-2 P2-B: draft-quality THROW-PATH regression lock.
//
// doDraftRenderWork wires the SAME ForTest_SetThrowBeforeRasterize seam
// the production path's RunThrowDuringOverrideTest/RunThrowNoOverrideTest
// already exercise (AgentSession.cpp ~2208-2213, right before the
// ephemeral pipeline's RasterizeScene() call) -- but until now no test
// exercised the DRAFT copy of that seam. This proves the ephemeral
// pipeline's own exception-safety: a forced throw immediately before
// ephemeralRast->RasterizeScene() must not escape RenderCore_ as a raw
// C++ exception, must not crash the process, must name the seam in the
// failure message, must leave the session USABLE afterward (a clean
// follow-up draft render succeeds -- proving EphemeralPipelineGuard's
// three-owned-pointer release actually ran on the throwing exit, not
// just the ordinary one), and -- since doDraftRenderWork never
// references `rast` at all (see its own doc) -- must leave the
// PRODUCTION rasterizer's FrameStore identity/Generation completely
// untouched throughout, exactly like RunDraftIsolationTest's clean-render
// probe above but now under the throw seam.
//////////////////////////////////////////////////////////////////////
static void RunDraftThrowTest()
{
	std::printf( "=== AgentFrameStoreIsolationTest: Round-2 P2-B draft-quality throw-path ===\n" );

	const std::string scenePath = WriteTemp(
		"agent_framestore_isolation_draft_throw.RISEscene", BuildScene( kPtRasterizer ) );
	Check( !scenePath.empty(), "draft-throw: scratch scene file written" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "draft-throw: scene loads via the CST path" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "draft-throw: AgentSession::WrapJob wraps the locally-owned Job" );
	if( !session ) { pJob->release(); return; }

	IRasterizer* rast = pJob->GetRasterizer();
	Implementation::Rasterizer* concreteRast = rast ? dynamic_cast<Implementation::Rasterizer*>( rast ) : nullptr;
	Check( concreteRast != nullptr, "draft-throw: active (production) rasterizer is an Implementation::Rasterizer" );
	if( !concreteRast ) { pJob->release(); return; }

	Implementation::FrameStore* displayStore = concreteRast->GetFrameStore();
	Check( displayStore != nullptr, "draft-throw: production rasterizer has a canonical FrameStore before any render" );
	if( !displayStore ) { pJob->release(); return; }

	const uint64_t genBefore = displayStore->Generation();

	session->ForTest_SetThrowBeforeRasterize( true );

	AgentRenderParams params;
	params.quality = AgentRenderQuality::Draft;
	params.width   = 32;   // film-dims override too, per the fix-round request ("for good measure")
	params.height  = 32;

	AgentRenderResult res;
	bool escaped = false;
	std::string escapedWhat;
	try {
		res = session->Render( params );
	}
	catch( const std::exception& e ) { escaped = true; escapedWhat = e.what(); }
	catch( ... )                     { escaped = true; escapedWhat = "unknown exception"; }

	Check( !escaped, "draft-throw: the forced throw did NOT escape RenderCore_ as a raw C++ exception" );
	if( escaped ) {
		std::printf( "  (raw exception escaped: %s)\n", escapedWhat.c_str() );
	}
	Check( !res.ok, "draft-throw: render reports ok=false (the seam's throw actually fired)" );
	Check( res.renderMode == "draft", "draft-throw: the failed result still reports renderMode==\"draft\"" );
	Check( res.message.find( "ForTest_ThrowBeforeRasterize" ) != std::string::npos,
		"draft-throw: failure message names the forced test-seam throw" );
	Check( res.message.find( "draft path" ) != std::string::npos,
		"draft-throw: failure message names the DRAFT-path throw site specifically (not the production one)" );

	// (b) NO CRASH: reaching this line at all IS the assertion.
	Check( true, "draft-throw: process did NOT crash" );

	// The production rasterizer was NEVER touched by the draft throw --
	// same probes RunDraftIsolationTest uses for a clean draft render.
	Check( concreteRast->GetFrameStore() == displayStore,
		"draft-throw: production rasterizer's FrameStore identity is untouched (draft path never references `rast`)" );
	Check( displayStore->Generation() == genBefore,
		"draft-throw: production rasterizer's canonical FrameStore Generation() did NOT advance" );

	// RED-PROVE usable-after: disarm the seam and confirm a clean
	// follow-up DRAFT render succeeds -- no leak of the ephemeral
	// pipeline wedges the session.
	session->ForTest_SetThrowBeforeRasterize( false );
	AgentRenderParams cleanParams;
	cleanParams.quality = AgentRenderQuality::Draft;
	const AgentRenderResult clean = session->Render( cleanParams );
	Check( clean.ok, "draft-throw: a follow-up clean draft render succeeds after the throw (session still usable)" );
	Check( clean.renderMode == "draft", "draft-throw: follow-up render reports renderMode==\"draft\"" );

	std::printf( "=== Round-2 P2-B draft-quality throw-path: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );

	pJob->release();
	std::remove( scenePath.c_str() );
}

static void RunConcurrentReadLeasePeakProbe()
{
	const std::string scenePath = WriteTemp(
		"agent_perception_read_lease_peak.RISEscene", BuildScene( kPtRasterizer ) );
	Check( !scenePath.empty(), "read-lease peak: scratch scene file written" );
	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ),
		"read-lease peak: scene loads" );
	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "read-lease peak: session wraps job" );
	if( !session ) { pJob->release(); std::remove( scenePath.c_str() ); return; }

	AgentRenderParams params;
	params.perception = true;
	const AgentRenderResult first = session->Render( params );
	Check( first.ok && first.perceptionAuxiliaryPeakBytes == 24u * 24u * 84u,
		"read-lease peak: first render has the cold 84-byte peak" );

	std::mutex hookMutex;
	std::condition_variable hookCv;
	bool leaseAcquired = false;
	bool releaseReader = false;
	session->ForTest_SetReadPerceptionAfterLeaseHook( [&]() {
		std::unique_lock<std::mutex> lk( hookMutex );
		leaseAcquired = true;
		hookCv.notify_all();
		hookCv.wait( lk, [&]() { return releaseReader; } );
	} );
	std::thread reader( [&]() {
		unsigned int atlasW = 0, atlasH = 0;
		AgentPerceptionInfo info;
		session->ReadPerception( 48, atlasW, atlasH, info );
	} );
	bool readerIsLeased = false;
	{
		std::unique_lock<std::mutex> lk( hookMutex );
		readerIsLeased = hookCv.wait_for( lk, std::chrono::seconds( 5 ),
			[&]() { return leaseAcquired; } );
	}
	Check( readerIsLeased,
		"read-lease peak: reader registers its sink before unlocked encoding" );
	if( !readerIsLeased ) {
		{
			std::lock_guard<std::mutex> lk( hookMutex );
			releaseReader = true;
		}
		hookCv.notify_all();
		reader.join();
		session.reset();
		pJob->release();
		std::remove( scenePath.c_str() );
		return;
	}

	const AgentRenderResult second = session->Render( params );
	const AgentRenderResult third = session->Render( params );
	Check( second.ok && second.perceptionAuxiliaryPeakBytes == 24u * 24u * 91u,
		"read-lease peak: first replacement counts the current leased sidecar once" );
	Check( third.ok && third.perceptionAuxiliaryPeakBytes == 24u * 24u * 98u,
		"read-lease peak: second replacement counts current plus superseded leased sidecars" );

	{
		std::lock_guard<std::mutex> lk( hookMutex );
		releaseReader = true;
	}
	hookCv.notify_all();
	reader.join();
	session->ForTest_SetReadPerceptionAfterLeaseHook( std::function<void()>() );
	session.reset();
	pJob->release();
	std::remove( scenePath.c_str() );
}

int main()
{
	std::printf( "=== AgentFrameStoreIsolationTest ===\n" );

	RunIsolationProbe( "pt", kPtRasterizer );

	// BDPT / VCM: per PixelBasedRasterizerHelper.cpp's AcquireRenderImage
	// dims-exact-match gate and BDPTRasterizerBase.cpp / VCMRasterizerBase.cpp,
	// both integrators write through the SAME canonical FrameStore as PT
	// when its dims match the render dims -- so the isolation fix (which
	// operates purely on the Implementation::Rasterizer base's
	// GetFrameStore/SetFrameStore/AcceptsFrameStorePush surface, never on
	// integrator-specific code) should generalize without a crash or a
	// dims mismatch.  If either of these trips a FAIL, that is a genuine
	// finding narrowing Slice-0 scope to the PT family -- not something to
	// paper over.
	RunIsolationProbe( "bdpt", kBdptRasterizer );
	RunIsolationProbe( "vcm",  kVcmRasterizer );

	// P2: MLT opted back into the FrameStore push (commit 36809dcf,
	// "L6d-2b") -- it is genuinely gated into this private-store path
	// today (AcceptsFrameStorePush() == true), so it must be covered
	// here, not just PT/BDPT/VCM.
	RunIsolationProbe( "mlt", kMltRasterizer );
	RunIsolationProbe( "auto", kAutoRasterizer, "accurate" );

	RunDepthContractProbe( "shader_pel", kDepthShaderPel );
	RunDepthContractProbe( "shader_spectral_nm", kDepthShaderSpectralNM );
	RunDepthContractProbe( "shader_spectral_hwss", kDepthShaderSpectralHWSS );
	RunDepthContractProbe( "pt_pel", kDepthPtPel );
	RunDepthContractProbe( "pt_spectral_nm", kDepthPtSpectralNM );
	RunDepthContractProbe( "pt_spectral_hwss", kDepthPtSpectralHWSS );
	RunDepthContractProbe( "bdpt_pel", kDepthBdptPel );
	RunDepthContractProbe( "bdpt_spectral_nm", kDepthBdptSpectralNM );
	RunDepthContractProbe( "bdpt_spectral_hwss", kDepthBdptSpectralHWSS );
	RunDepthContractProbe( "vcm_pel", kDepthVcmPel );
	RunDepthContractProbe( "vcm_spectral_nm", kDepthVcmSpectralNM );
	RunDepthContractProbe( "vcm_spectral_hwss", kDepthVcmSpectralHWSS );

	// Adversarial root-definition coverage: shader recursion and participating
	// media must not redirect the primary-depth observation.
	RunDepthContractProbe( "transparent_shader_pel", kDepthShaderPel, kTransparencyDepth );
	RunDepthContractProbe( "transparent_shader_spectral_nm", kDepthShaderSpectralNM, kTransparencyDepth );
	RunDepthContractProbe( "transparent_shader_spectral_hwss", kDepthShaderSpectralHWSS, kTransparencyDepth );
	RunDepthContractProbe( "medium_shader_pel", kDepthShaderPel, kMediumDepth );
	RunDepthContractProbe( "medium_bdpt_pel", kDepthBdptPel, kMediumDepth );
	RunDepthContractProbe( "medium_bdpt_spectral_hwss", kDepthBdptSpectralHWSS, kMediumDepth );
	RunDepthContractProbe( "medium_vcm_pel", kDepthVcmPel, kMediumDepth );

	// Regression for hit-conditioned normalization at anti-aliased silhouettes.
	// Shader dispatch is a control; PT/BDPT/VCM exposed the original defect.
	RunSilhouetteDepthProbe( "silhouette_shader_pel", kSilhouetteShaderPel );
	RunSilhouetteDepthProbe( "silhouette_pt_pel", kSilhouettePtPel );
	RunSilhouetteDepthProbe( "silhouette_bdpt_pel", kSilhouetteBdptPel );
	RunSilhouetteDepthProbe( "silhouette_vcm_pel", kSilhouetteVcmPel );

	RunThrowDuringOverrideTest();
	RunThrowNoOverrideTest();
	RunProductionSinkDetachmentProbe( "pt", kPtRasterizer );
	RunProductionSinkDetachmentProbe( "auto", kAutoRasterizer );
	RunConcurrentReadLeasePeakProbe();
	RunDraftIsolationTest();
	RunDraftThrowTest();

	std::printf( "=== AgentFrameStoreIsolationTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
