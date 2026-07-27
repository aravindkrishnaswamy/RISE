//////////////////////////////////////////////////////////////////////
//
//  LightSoloTest.cpp - GUI render modes P2 "light solo" (docs/gui/
//    RENDER_MODES.md §3 Lighting, §9 P2b).  Covers the core mechanism
//    (LightSampler::SetSoloLight/SetSoloLuminary + RayCaster::
//    SetSoloLightByName + PathTracingIntegrator's PART-1 emission gate)
//    through the agent `render{light:}` surface, exactly the way an
//    agent actually reaches it.
//
//  THE MONEY TEST is RunUnbiasedPartitionTest below: direct lighting is
//  LINEAR in the light set, so an unbiased solo mechanism must PARTITION
//  exactly -- solo(lightA) + solo(lightB) === render(lightA, lightB)
//  (up to Monte-Carlo noise) when those are the only two lights in the
//  scene.  That is the proof that SetSoloLightByName's selection-pdf=1.0
//  bypass keeps NEE and the BSDF-hit emission MIS partner in agreement
//  (see LightSampler::CachedPdfSelectLuminary's solo branch) rather than
//  merely dimming the non-solo lights to "mostly dark".
//
//  Scene: a neutral grey floor lit by exactly two mesh area lights of
//  distinct, strongly saturated colours (red left, blue right) -- no
//  other emitters, no ambient, no environment map, so the ENTIRE image
//  is direct lighting from those two sources (plus a small amount of
//  colour-bleed GI, which the partition identity covers too since it's
//  still linear in the light set).
//
//  meanR/meanG/meanB (AgentRenderResult) are used for every quantitative
//  assertion here rather than decoded PNG bytes: they are the RAW LINEAR
//  per-channel FrameStore means, computed BEFORE the display transform's
//  tone curve/exposure and BEFORE 8-bit quantization (InMemoryRasterizer-
//  Output::MeanChannels reads straight off FrameStore::GetPEL; the
//  display transform is applied later, only inside ToPng()) -- so the
//  linear partition identity holds exactly regardless of the scene's
//  (or the default ACES) display transform, with no `file_rasterizer-
//  output{display_transform none}` scaffolding needed.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
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

//----------------------------------------------------------------------
// Scene: neutral grey floor, two mesh area lights (red left, blue
// right), nothing else emissive.  `oidn_denoise false` -- a raw MC
// estimator, not a denoised approximation, is what the linear-partition
// identity actually proves.
//----------------------------------------------------------------------
static const char* const kSceneTwoColoredLights =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 80\n\theight 60\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 12\n\theight 0.2\n\tdepth 12\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -1 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname redlight_pnt\n\tcolor 1.0 0.05 0.05\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname redlight_mat\n\texitance redlight_pnt\n\tmaterial none\n\tscale 8.0\n}\n\n"
	"box_geometry\n{\n\tname redlight_geo\n\twidth 2.5\n\theight 0.2\n\tdepth 2.5\n}\n\n"
	"standard_object\n{\n\tname redlight\n\tgeometry redlight_geo\n\tmaterial redlight_mat\n\tposition -3 4 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname bluelight_pnt\n\tcolor 0.05 0.05 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname bluelight_mat\n\texitance bluelight_pnt\n\tmaterial none\n\tscale 8.0\n}\n\n"
	"box_geometry\n{\n\tname bluelight_geo\n\twidth 2.5\n\theight 0.2\n\tdepth 2.5\n}\n\n"
	"standard_object\n{\n\tname bluelight\n\tgeometry bluelight_geo\n\tmaterial bluelight_mat\n\tposition 3 4 0\n}\n";

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

static std::unique_ptr<AgentSession> LoadSession( const std::string& scenePath, Job** outJob )
{
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
		pJob->release();
		*outJob = nullptr;
		return nullptr;
	}
	*outJob = pJob;
	return AgentSession::WrapJob( pJob );
}

//----------------------------------------------------------------------
// (1) An unresolved light name FAILS the render loudly, with the
//     available-name list in `message` -- same contract as `view`.
//----------------------------------------------------------------------
//! MIS-SENSITIVE partition fixture.  Same two-coloured-light setup as
//! kSceneTwoColoredLights, but the emitters are LARGE (10x10) and CLOSE
//! (y=1.4 over a floor at y=-1), so they subtend a big solid angle: a
//! cosine-weighted BSDF sample hits an emitter often, and the BSDF-hit
//! emission strategy therefore carries a large share of the energy.
//!
//! WHY THIS EXISTS: the original fixture's small, distant lights are
//! almost pure-NEE -- the emission-side MIS weight barely participates,
//! so a solo implementation whose NEE selection pdf (1.0, bypassed) and
//! emission-MIS pdf (CachedPdfSelectLuminary) DISAGREE still passes it.
//! Measured: mutating CachedPdfSelectLuminary's solo branch moved that
//! fixture's partition sum only ~1.1 points, inside its 3% tolerance.
//! With large close lights the same mutation is plainly visible, so THIS
//! fixture is the one that actually guards MIS consistency.
static const char* const kSceneTwoColoredLightsMISHeavy =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 80\n\theight 60\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 2 9\n\tlookat 0 -0.5 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 12\n\theight 0.2\n\tdepth 12\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -1 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname redlight_pnt\n\tcolor 1.0 0.05 0.05\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname redlight_mat\n\texitance redlight_pnt\n\tmaterial none\n\tscale 1.2\n}\n\n"
	"box_geometry\n{\n\tname redlight_geo\n\twidth 10\n\theight 0.2\n\tdepth 10\n}\n\n"
	"standard_object\n{\n\tname redlight\n\tgeometry redlight_geo\n\tmaterial redlight_mat\n\tposition -5.2 1.4 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname bluelight_pnt\n\tcolor 0.05 0.05 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname bluelight_mat\n\texitance bluelight_pnt\n\tmaterial none\n\tscale 1.2\n}\n\n"
	"box_geometry\n{\n\tname bluelight_geo\n\twidth 10\n\theight 0.2\n\tdepth 10\n}\n\n"
	"standard_object\n{\n\tname bluelight\n\tgeometry bluelight_geo\n\tmaterial bluelight_mat\n\tposition 5.2 1.4 0\n}\n";

static void RunUnknownLightFailsTest()
{
	std::printf( "=== LightSoloTest: unresolved light name fails loudly ===\n" );
	const std::string scenePath = WriteTemp( "rise_lightsolo_unknown.RISEscene", kSceneTwoColoredLights );
	Check( !scenePath.empty(), "wrote the two-light scene" );

	Job* pJob = nullptr;
	std::unique_ptr<AgentSession> session = LoadSession( scenePath, &pJob );
	Check( session != nullptr, "scene loads and session wraps" );
	if( !session ) { if( pJob ) pJob->release(); return; }

	AgentRenderParams p;
	p.light = "nonexistent_light";
	AgentRenderResult r = session->Render( p );

	Check( !r.ok, "MONEY ASSERTION: an unresolved light name fails the render (ok:false)" );
	Check( r.message.find( "unknown light" ) != std::string::npos,
	       "failure message names the problem as an unknown light" );
	Check( r.message.find( "redlight" ) != std::string::npos,
	       "failure message's available-name list includes \"redlight\"" );
	Check( r.message.find( "bluelight" ) != std::string::npos,
	       "failure message's available-name list includes \"bluelight\"" );
	std::printf( "  message: %s\n", r.message.c_str() );
}

//----------------------------------------------------------------------
// (2) `light` is silently ignored (honestly noted) under objectmap and
//     the false-colour view-mode diagnostics -- neither evaluates scene
//     lighting at all.
//----------------------------------------------------------------------
static void RunLightIgnoredUnderDataModesTest()
{
	std::printf( "=== LightSoloTest: light ignored (honestly) under objectmap/normals ===\n" );
	const std::string scenePath = WriteTemp( "rise_lightsolo_ignored.RISEscene", kSceneTwoColoredLights );
	Job* pJob = nullptr;
	std::unique_ptr<AgentSession> session = LoadSession( scenePath, &pJob );
	Check( session != nullptr, "scene loads and session wraps" );
	if( !session ) { if( pJob ) pJob->release(); return; }

	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ObjectMap;
		p.light = "redlight";
		AgentRenderResult r = session->Render( p );
		Check( r.ok, "objectmap + light:\"redlight\" still succeeds" );
		Check( r.message.find( "light is ignored" ) != std::string::npos,
		       "result message honestly notes light is ignored under objectmap" );
	}
	{
		AgentRenderParams p;
		p.renderTarget = AgentRenderTarget::ViewMode;
		p.viewMode     = Implementation::ViewportRenderMode::Normals;
		p.light        = "redlight";
		AgentRenderResult r = session->Render( p );
		Check( r.ok, "mode:\"normals\" + light:\"redlight\" still succeeds" );
		Check( r.message.find( "light is ignored" ) != std::string::npos,
		       "result message honestly notes light is ignored under normals" );
	}
}

//----------------------------------------------------------------------
// (3) THE MONEY TEST -- unbiased partition.  Render three ways: all
//     lights, solo(red), solo(blue).  Assert:
//       (a) solo(red) is red-dominant, solo(blue) is blue-dominant.
//       (b) solo(red) + solo(blue) ~= all-lights, per channel, within a
//           tight MC tolerance -- the actual unbiasedness proof.
//----------------------------------------------------------------------
static void RunUnbiasedPartitionTest()
{
	std::printf( "=== LightSoloTest: MONEY TEST -- solo(red)+solo(blue) ~= all-lights (unbiased partition) ===\n" );
	const std::string scenePath = WriteTemp( "rise_lightsolo_partition.RISEscene", kSceneTwoColoredLights );
	Check( !scenePath.empty(), "wrote the two-light scene" );

	Job* pJob = nullptr;
	std::unique_ptr<AgentSession> session = LoadSession( scenePath, &pJob );
	Check( session != nullptr, "scene loads and session wraps" );
	if( !session ) { if( pJob ) pJob->release(); return; }

	AgentRenderParams pAll;
	AgentRenderResult rAll = session->Render( pAll );
	Check( rAll.ok, "all-lights render succeeds" );

	AgentRenderParams pRed;
	pRed.light = "redlight";
	AgentRenderResult rRed = session->Render( pRed );
	Check( rRed.ok, "solo(redlight) render succeeds" );

	AgentRenderParams pBlue;
	pBlue.light = "bluelight";
	AgentRenderResult rBlue = session->Render( pBlue );
	Check( rBlue.ok, "solo(bluelight) render succeeds" );

	std::printf( "  all:              R=%.6f G=%.6f B=%.6f\n", rAll.meanR,  rAll.meanG,  rAll.meanB );
	std::printf( "  solo(red):        R=%.6f G=%.6f B=%.6f\n", rRed.meanR,  rRed.meanG,  rRed.meanB );
	std::printf( "  solo(blue):       R=%.6f G=%.6f B=%.6f\n", rBlue.meanR, rBlue.meanG, rBlue.meanB );
	const double sumR = rRed.meanR + rBlue.meanR;
	const double sumG = rRed.meanG + rBlue.meanG;
	const double sumB = rRed.meanB + rBlue.meanB;
	std::printf( "  solo(red)+solo(blue): R=%.6f G=%.6f B=%.6f\n", sumR, sumG, sumB );
	if( rAll.meanR > 0 )
		std::printf( "  relative diff: dR=%.4f%% dG=%.4f%% dB=%.4f%%\n",
			100.0 * ( sumR - rAll.meanR ) / rAll.meanR,
			100.0 * ( sumG - rAll.meanG ) / ( rAll.meanG > 0 ? rAll.meanG : 1.0 ),
			100.0 * ( sumB - rAll.meanB ) / ( rAll.meanB > 0 ? rAll.meanB : 1.0 ) );

	// (a) Dominance/discrimination: solo(red) reads strongly R-dominant
	// (R at least 4x either G or B); solo(blue) reads strongly
	// B-dominant.  Not near-zero in an ABSOLUTE sense -- both emitters
	// carry a small (0.05) off-axis component by design -- but clearly
	// and discriminatingly dominant in the RATIO sense.
	Check( rRed.meanR > 4.0 * rRed.meanG && rRed.meanR > 4.0 * rRed.meanB,
	       "MONEY ASSERTION (a): solo(redlight) is strongly R-dominant (blue contributes ~nothing)" );
	Check( rBlue.meanB > 4.0 * rBlue.meanR && rBlue.meanB > 4.0 * rBlue.meanG,
	       "MONEY ASSERTION (a): solo(bluelight) is strongly B-dominant (red contributes ~nothing)" );

	// (b) The actual unbiasedness proof: summing the two solo renders
	// reproduces the all-lights render, per channel, within a tight but
	// genuinely discriminating MC tolerance.  256 spp/pixel * 80*60 pixels
	// averaged over the whole frame keeps relative MC noise on the
	// FRAME MEAN well under 1%; 3% leaves comfortable headroom above
	// the observed noise floor while remaining far tighter than the
	// >=25% a naive alias-table-pdf MIS mismatch or a no-op solo would
	// produce (see the mutation-tested evidence in this file's
	// accompanying session report).
	const double kRelTol = 0.03;
	Check( std::fabs( sumR - rAll.meanR ) <= kRelTol * rAll.meanR,
	       "MONEY ASSERTION (b): solo(red)+solo(blue) R channel matches all-lights within 3%" );
	Check( std::fabs( sumG - rAll.meanG ) <= kRelTol * rAll.meanG + 1e-6,
	       "MONEY ASSERTION (b): solo(red)+solo(blue) G channel matches all-lights within 3%" );
	Check( std::fabs( sumB - rAll.meanB ) <= kRelTol * rAll.meanB,
	       "MONEY ASSERTION (b): solo(red)+solo(blue) B channel matches all-lights within 3%" );
}

//----------------------------------------------------------------------
// review-p2d P1-2: light solo is a PathTracingIntegrator mechanism.  A
// VCM scene reaching for it must FAIL LOUDLY -- before the fix it
// returned ok:true plus "light solo: X is the only active light" over an
// image with every light lit, because ApplyLightSoloByName's downcast
// reaches the RayCaster of any PixelBasedRasterizerHelper (which VCM is)
// while LightSampler's solo branches live only in the paths PT uses.
//----------------------------------------------------------------------
static const char* const kSceneVcmTwoLights =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"vcm_pel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 32\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 12\n\theight 0.2\n\tdepth 12\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -1 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname redlight_pnt\n\tcolor 1.0 0.05 0.05\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname redlight_mat\n\texitance redlight_pnt\n\tmaterial none\n\tscale 3.0\n}\n\n"
	"box_geometry\n{\n\tname redlight_geo\n\twidth 2.5\n\theight 0.2\n\tdepth 2.5\n}\n\n"
	"standard_object\n{\n\tname redlight\n\tgeometry redlight_geo\n\tmaterial redlight_mat\n\tposition -2.2 2.5 0\n}\n";

static void RunNonPathTracingIntegratorRefusesSoloTest()
{
	std::printf( "=== LightSoloTest: a non-PT integrator REFUSES light solo (review-p2d P1-2) ===\n" );
	const std::string scenePath = WriteTemp( "rise_lightsolo_vcm.RISEscene", kSceneVcmTwoLights );
	Check( !scenePath.empty(), "wrote the VCM scene" );

	Job* pJob = nullptr;
	std::unique_ptr<AgentSession> session = LoadSession( scenePath, &pJob );
	Check( session != nullptr, "VCM scene loads and session wraps" );
	if( !session ) { if( pJob ) pJob->release(); return; }

	// A render with NO light arg must still work -- the gate is scoped to
	// light solo and must not break ordinary VCM rendering.
	{
		AgentRenderParams pPlain;
		AgentRenderResult rPlain = session->Render( pPlain );
		Check( rPlain.ok, "a VCM render with no `light` arg still succeeds -- the P1-2 gate is scoped "
		                  "to light solo and does not break ordinary rendering" );
	}

	AgentRenderParams p;
	p.light = "redlight";   // a name that RESOLVES; only the integrator is wrong
	AgentRenderResult r = session->Render( p );
	std::printf( "  ok=%d message: %s\n", r.ok ? 1 : 0, r.message.c_str() );

	Check( !r.ok,
	       "MONEY ASSERTION: VCM + light solo FAILS rather than silently rendering every light while "
	       "reporting a solo.  The name resolves fine -- it is the integrator that cannot honour it." );
	Check( r.message.find( "path-tracing" ) != std::string::npos,
	       "the refusal explains that light solo is path-tracing-only (actionable, not a bare error)" );

	pJob->release();
}

//----------------------------------------------------------------------
// review-p2d P2-2 fixture: an EXPLICIT (non-mesh) light + a mesh
// luminary.  Covers SoloKind::Light, which every other fixture in this
// file misses -- they are all mesh luminaries (SoloKind::Luminary), so
// the light-manager branch of SetSoloLightByName, LightSampler's
// pLight-identity scan, and the Step-1 zero-exitance deterministic
// loop's solo filter were all unexercised with a solo target set.
//
// Green omni light vs red mesh luminary, so the channels discriminate
// WHICH source each solo actually captured, not merely that the total
// adds up.
//----------------------------------------------------------------------
static const char* const kSceneOmniPlusMeshLight =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 80\n\theight 60\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 12\n\theight 0.2\n\tdepth 12\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -1 0\n}\n\n"
	"omni_light\n{\n\tname greenomni\n\tpower 40.0\n\tposition 2.2 2.5 0\n\tcolor 0.05 1.0 0.05\n}\n\n"
	"uniformcolor_painter\n{\n\tname redlight_pnt\n\tcolor 1.0 0.05 0.05\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname redlight_mat\n\texitance redlight_pnt\n\tmaterial none\n\tscale 3.0\n}\n\n"
	"box_geometry\n{\n\tname redlight_geo\n\twidth 2.5\n\theight 0.2\n\tdepth 2.5\n}\n\n"
	"standard_object\n{\n\tname redlight\n\tgeometry redlight_geo\n\tmaterial redlight_mat\n\tposition -2.2 2.5 0\n}\n";

//----------------------------------------------------------------------
// (3d) MONEY TEST 4 -- SoloKind::Light (explicit light) partition.
//----------------------------------------------------------------------
static void RunExplicitLightPartitionTest()
{
	std::printf( "=== LightSoloTest: MONEY TEST 4 (EXPLICIT LIGHT) -- SoloKind::Light (review-p2d P2-2) ===\n" );
	const std::string scenePath = WriteTemp( "rise_lightsolo_omni.RISEscene", kSceneOmniPlusMeshLight );
	Check( !scenePath.empty(), "wrote the omni+mesh scene" );

	Job* pJob = nullptr;
	std::unique_ptr<AgentSession> session = LoadSession( scenePath, &pJob );
	Check( session != nullptr, "scene loads and session wraps" );
	if( !session ) { if( pJob ) pJob->release(); return; }

	AgentRenderParams pAll;
	AgentRenderResult rAll = session->Render( pAll );
	Check( rAll.ok, "all-lights omni+mesh render succeeds" );

	AgentRenderParams pOmni;
	pOmni.light = "greenomni";
	AgentRenderResult rOmni = session->Render( pOmni );
	Check( rOmni.ok, "solo(greenomni) render succeeds -- resolves via the LIGHT MANAGER branch of "
	                 "SetSoloLightByName (SoloKind::Light), not the object-manager fallback every "
	                 "other fixture here exercises" );

	AgentRenderParams pMesh;
	pMesh.light = "redlight";
	AgentRenderResult rMesh = session->Render( pMesh );
	Check( rMesh.ok, "solo(redlight) render succeeds" );

	// The REFERENCE for a solo render is not the all-lights render (see the
	// note below) -- it is the same light rendered ALONE in a scene that
	// contains no other light, with no solo applied at all.  That is exactly
	// what "solo" claims to reproduce, and it is a scene-composition-
	// independent ground truth.
	std::string meshOnly = kSceneOmniPlusMeshLight;
	{
		const std::string omniChunk =
			"omni_light\n{\n\tname greenomni\n\tpower 40.0\n\tposition 2.2 2.5 0\n\tcolor 0.05 1.0 0.05\n}\n\n";
		const size_t at = meshOnly.find( omniChunk );
		Check( at != std::string::npos, "located the omni chunk to strip for the reference scene" );
		if( at != std::string::npos ) meshOnly.erase( at, omniChunk.size() );
	}
	const std::string refPath = WriteTemp( "rise_lightsolo_meshonly.RISEscene", meshOnly.c_str() );
	Job* pRefJob = nullptr;
	std::unique_ptr<AgentSession> refSession = LoadSession( refPath, &pRefJob );
	Check( refSession != nullptr, "mesh-only reference scene loads" );
	AgentRenderResult rRef;
	if( refSession ) {
		AgentRenderParams pRef;   // NO solo -- the mesh light is the only light
		rRef = refSession->Render( pRef );
		Check( rRef.ok, "mesh-only reference render succeeds" );
	}

	std::printf( "  solo(greenomni):            R=%.6f G=%.6f B=%.6f\n", rOmni.meanR, rOmni.meanG, rOmni.meanB );
	std::printf( "  solo(redlight):             R=%.6f G=%.6f B=%.6f\n", rMesh.meanR, rMesh.meanG, rMesh.meanB );
	std::printf( "  mesh ALONE (no solo, ref):  R=%.6f G=%.6f B=%.6f\n", rRef.meanR,  rRef.meanG,  rRef.meanB );
	std::printf( "  mesh implied in all:         R=%.6f G=%.6f B=%.6f\n",
		rAll.meanR-rOmni.meanR, rAll.meanG-rOmni.meanG, rAll.meanB-rOmni.meanB );

	// (a) Each solo captured the right source.
	Check( rOmni.meanG > 4.0 * rOmni.meanR,
	       "MONEY ASSERTION (a): solo(greenomni) is strongly G-dominant -- SoloKind::Light resolved and "
	       "captured the omni, not the red mesh luminary" );
	Check( rMesh.meanR > 4.0 * rMesh.meanG,
	       "MONEY ASSERTION (a): solo(redlight) is strongly R-dominant -- the omni did not leak in" );

	// (b) THE SOLO CONTRACT: soloing a light reproduces that light rendered
	//     alone.  This is the assertion a SoloKind::Light mishandling breaks,
	//     and unlike a partition against the all-lights render it does not
	//     depend on any OTHER light being sampled correctly.
	const double kRelTol = 0.03;
	Check( rRef.meanR > 0 && std::fabs( rMesh.meanR - rRef.meanR ) <= kRelTol * rRef.meanR,
	       "MONEY ASSERTION (b): solo(redlight) in the omni scene == the mesh light rendered ALONE, "
	       "within 3% -- soloing reproduces the single-light render regardless of what else is in the "
	       "scene" );

	// (c) The regular all-lights estimator must keep the same mesh energy:
	// the red mesh contribution in all = all - solo(omni).  This used to
	// over-count by 10% because BoxGeometry::UniformRandomPoint selected
	// faces uniformly while NEE claimed a uniform-per-area density.
	const double meshInAllR = rAll.meanR - rOmni.meanR;
	const double meshInAllG = rAll.meanG - rOmni.meanG;
	const double meshInAllB = rAll.meanB - rOmni.meanB;
	Check( rRef.meanR > 0 && rRef.meanG > 0 && rRef.meanB > 0 &&
		std::fabs( meshInAllR - rRef.meanR ) <= kRelTol * rRef.meanR &&
		std::fabs( meshInAllG - rRef.meanG ) <= kRelTol * rRef.meanG &&
		std::fabs( meshInAllB - rRef.meanB ) <= kRelTol * rRef.meanB,
	       "MONEY ASSERTION (c): the RGB mesh component in all-lights == the mesh rendered ALONE, "
	       "within 3% -- NEE and BSDF-hit emission agree even with a delta light in the alias table" );

	if( pRefJob ) pRefJob->release();
	pJob->release();
}

//----------------------------------------------------------------------
// review-p2d P1-1 fixture: a mesh area light PLUS a uniform environment.
//
// WHY THIS EXISTS: neither partition fixture above declares a radiance
// map, so both were structurally blind to the environment.  Under
// SoloKind::Light/Luminary the env-NEE strategy is switched off in
// LightSampler, but every BSDF-side env-radiance add in
// PathTracingIntegrator stayed live -- so the environment leaked into a
// light-solo render at a FRACTIONAL MIS weight (power-heuristic against a
// partner strategy that no longer runs).  Not "one light", not an honest
// two-light render, and it broke the very identity light solo advertises.
//
// The env is bright and uniform, and the floor is diffuse, so env
// illumination is a LARGE share of the total -- a fractional leak moves
// the partition sum well outside tolerance rather than hiding in noise.
//----------------------------------------------------------------------
static const char* const kSceneLightPlusEnvironment =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"uniformcolor_painter\n{\n\tname env_pnt\n\tcolor 0.0 0.6 0.0\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 256\n\tpixel_filter box\n\toidn_denoise false\n"
		"\tradiance_map env_pnt\n\tradiance_scale 1.0\n\tradiance_background true\n}\n\n"
	"film\n{\n\twidth 80\n\theight 60\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 3 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 55.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname floor_pnt\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"lambertian_material\n{\n\tname floor_mat\n\treflectance floor_pnt\n}\n\n"
	"box_geometry\n{\n\tname floor_geo\n\twidth 12\n\theight 0.2\n\tdepth 12\n}\n\n"
	"standard_object\n{\n\tname floor_obj\n\tgeometry floor_geo\n\tmaterial floor_mat\n\tposition 0 -1 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname redlight_pnt\n\tcolor 1.0 0.05 0.05\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname redlight_mat\n\texitance redlight_pnt\n\tmaterial none\n\tscale 3.0\n}\n\n"
	"box_geometry\n{\n\tname redlight_geo\n\twidth 2.5\n\theight 0.2\n\tdepth 2.5\n}\n\n"
	"standard_object\n{\n\tname redlight\n\tgeometry redlight_geo\n\tmaterial redlight_mat\n\tposition -2.2 2.5 0\n}\n";

//----------------------------------------------------------------------
// (3c) MONEY TEST 3 -- the ENVIRONMENT half of the partition (P1-1).
//----------------------------------------------------------------------
static void RunEnvironmentPartitionTest()
{
	std::printf( "=== LightSoloTest: MONEY TEST 3 (ENVIRONMENT) -- solo(light)+solo(environment) ~= all (review-p2d P1-1) ===\n" );
	const std::string scenePath = WriteTemp( "rise_lightsolo_env.RISEscene", kSceneLightPlusEnvironment );
	Check( !scenePath.empty(), "wrote the light+environment scene" );

	Job* pJob = nullptr;
	std::unique_ptr<AgentSession> session = LoadSession( scenePath, &pJob );
	Check( session != nullptr, "scene loads and session wraps" );
	if( !session ) { if( pJob ) pJob->release(); return; }

	AgentRenderParams pAll;
	AgentRenderResult rAll = session->Render( pAll );
	Check( rAll.ok, "all-lights (light + env) render succeeds" );

	AgentRenderParams pLight;
	pLight.light = "redlight";
	AgentRenderResult rLight = session->Render( pLight );
	Check( rLight.ok, "solo(redlight) render succeeds" );

	AgentRenderParams pEnv;
	pEnv.light = "environment";
	AgentRenderResult rEnv = session->Render( pEnv );
	Check( rEnv.ok, "solo(environment) render succeeds -- the reserved \"environment\" name resolves "
	                "(RayCaster::SetSoloLightByName), without which this identity is unstatable" );

	std::printf( "  all:                 R=%.6f G=%.6f B=%.6f\n", rAll.meanR,   rAll.meanG,   rAll.meanB );
	std::printf( "  solo(redlight):      R=%.6f G=%.6f B=%.6f\n", rLight.meanR, rLight.meanG, rLight.meanB );
	std::printf( "  solo(environment):   R=%.6f G=%.6f B=%.6f\n", rEnv.meanR,   rEnv.meanG,   rEnv.meanB );
	const double sumR = rLight.meanR + rEnv.meanR;
	const double sumG = rLight.meanG + rEnv.meanG;
	const double sumB = rLight.meanB + rEnv.meanB;
	std::printf( "  solo(light)+solo(env): R=%.6f G=%.6f B=%.6f\n", sumR, sumG, sumB );

	// (a) The fixture actually puts substantial energy through BOTH halves
	//     -- otherwise the identity below could pass by one side being
	//     negligible, which is exactly how a leak hides.  The env is GREEN
	//     and the mesh light RED, so the channels also discriminate WHICH
	//     source each solo actually captured.
	Check( rEnv.meanG > 4.0 * rEnv.meanR,
	       "MONEY ASSERTION (a): solo(environment) is strongly G-dominant -- it captured the GREEN env, "
	       "not the red mesh light" );
	Check( rLight.meanR > 4.0 * rLight.meanG,
	       "MONEY ASSERTION (a): solo(redlight) is strongly R-dominant -- the GREEN environment did NOT "
	       "leak in.  This is the direct P1-1 regression guard: before the fix the env leaked at a "
	       "fractional MIS weight and dragged G up." );

	// (b) The partition identity itself, per channel.
	const double kRelTol = 0.03;
	Check( std::fabs( sumR - rAll.meanR ) <= kRelTol * rAll.meanR + 1e-6,
	       "MONEY ASSERTION (b): solo(light)+solo(env) R matches all within 3%" );
	Check( std::fabs( sumG - rAll.meanG ) <= kRelTol * rAll.meanG + 1e-6,
	       "MONEY ASSERTION (b): solo(light)+solo(env) G matches all within 3% -- the channel the "
	       "environment dominates, so this is the one a fractional env leak breaks" );
	Check( std::fabs( sumB - rAll.meanB ) <= kRelTol * rAll.meanB + 1e-6,
	       "MONEY ASSERTION (b): solo(light)+solo(env) B matches all within 3%" );

	pJob->release();
}

//----------------------------------------------------------------------
// (3b) MONEY TEST 2 -- the MIS-SENSITIVE partition.  Same identity as
//      (3), on the large-close-emitter fixture where the BSDF-hit
//      emission strategy carries real weight, so an NEE-vs-emission
//      selection-pdf disagreement is actually visible.  See
//      kSceneTwoColoredLightsMISHeavy's doc for why (3) alone is not
//      sufficient evidence.
//----------------------------------------------------------------------
static void RunUnbiasedPartitionMISHeavyTest()
{
	std::printf( "=== LightSoloTest: MONEY TEST 2 (MIS-SENSITIVE) -- large close lights, BSDF-hit strategy load-bearing ===\n" );
	const std::string scenePath = WriteTemp( "rise_lightsolo_partition_misheavy.RISEscene", kSceneTwoColoredLightsMISHeavy );
	Check( !scenePath.empty(), "wrote the MIS-heavy two-light scene" );

	Job* pJob = nullptr;
	std::unique_ptr<AgentSession> session = LoadSession( scenePath, &pJob );
	Check( session != nullptr, "scene loads and session wraps" );
	if( !session ) { if( pJob ) pJob->release(); return; }

	AgentRenderParams pAll;
	AgentRenderResult rAll = session->Render( pAll );
	Check( rAll.ok, "all-lights render succeeds" );

	AgentRenderParams pRed;
	pRed.light = "redlight";
	AgentRenderResult rRed = session->Render( pRed );
	Check( rRed.ok, "solo(redlight) render succeeds" );

	AgentRenderParams pBlue;
	pBlue.light = "bluelight";
	AgentRenderResult rBlue = session->Render( pBlue );
	Check( rBlue.ok, "solo(bluelight) render succeeds" );

	std::printf( "  all:              R=%.6f G=%.6f B=%.6f\n", rAll.meanR,  rAll.meanG,  rAll.meanB );
	std::printf( "  solo(red):        R=%.6f G=%.6f B=%.6f\n", rRed.meanR,  rRed.meanG,  rRed.meanB );
	std::printf( "  solo(blue):       R=%.6f G=%.6f B=%.6f\n", rBlue.meanR, rBlue.meanG, rBlue.meanB );
	const double sumR = rRed.meanR + rBlue.meanR;
	const double sumG = rRed.meanG + rBlue.meanG;
	const double sumB = rRed.meanB + rBlue.meanB;
	std::printf( "  solo(red)+solo(blue): R=%.6f G=%.6f B=%.6f\n", sumR, sumG, sumB );
	if( rAll.meanR > 0 )
		std::printf( "  relative diff: dR=%.4f%% dG=%.4f%% dB=%.4f%%\n",
			100.0 * ( sumR - rAll.meanR ) / rAll.meanR,
			100.0 * ( sumG - rAll.meanG ) / ( rAll.meanG > 0 ? rAll.meanG : 1.0 ),
			100.0 * ( sumB - rAll.meanB ) / ( rAll.meanB > 0 ? rAll.meanB : 1.0 ) );

	// (a) Dominance/discrimination: solo(red) reads strongly R-dominant
	// (R at least 4x either G or B); solo(blue) reads strongly
	// B-dominant.  Not near-zero in an ABSOLUTE sense -- both emitters
	// carry a small (0.05) off-axis component by design -- but clearly
	// and discriminatingly dominant in the RATIO sense.
	Check( rRed.meanR > 4.0 * rRed.meanG && rRed.meanR > 4.0 * rRed.meanB,
	       "MONEY ASSERTION (a): solo(redlight) is strongly R-dominant (blue contributes ~nothing)" );
	Check( rBlue.meanB > 4.0 * rBlue.meanR && rBlue.meanB > 4.0 * rBlue.meanG,
	       "MONEY ASSERTION (a): solo(bluelight) is strongly B-dominant (red contributes ~nothing)" );

	// (b) The actual unbiasedness proof: summing the two solo renders
	// reproduces the all-lights render, per channel, within a tight but
	// genuinely discriminating MC tolerance.  256 spp/pixel * 80*60 pixels
	// averaged over the whole frame keeps relative MC noise on the
	// FRAME MEAN well under 1%; 3% leaves comfortable headroom above
	// the observed noise floor while remaining far tighter than the
	// >=25% a naive alias-table-pdf MIS mismatch or a no-op solo would
	// produce (see the mutation-tested evidence in this file's
	// accompanying session report).
	const double kRelTol = 0.03;
	Check( std::fabs( sumR - rAll.meanR ) <= kRelTol * rAll.meanR,
	       "MONEY ASSERTION (b): solo(red)+solo(blue) R channel matches all-lights within 3%" );
	Check( std::fabs( sumG - rAll.meanG ) <= kRelTol * rAll.meanG + 1e-6,
	       "MONEY ASSERTION (b): solo(red)+solo(blue) G channel matches all-lights within 3%" );
	Check( std::fabs( sumB - rAll.meanB ) <= kRelTol * rAll.meanB,
	       "MONEY ASSERTION (b): solo(red)+solo(blue) B channel matches all-lights within 3%" );
}

//----------------------------------------------------------------------
// (4) `light` composes with a BeautyVariant mode (mode:"direct"), not
//     only plain beauty.
//----------------------------------------------------------------------
static void RunLightSoloComposesWithBeautyVariantTest()
{
	std::printf( "=== LightSoloTest: light solo composes with mode:\"direct\" (BeautyVariant) ===\n" );
	const std::string scenePath = WriteTemp( "rise_lightsolo_variant.RISEscene", kSceneTwoColoredLights );
	Job* pJob = nullptr;
	std::unique_ptr<AgentSession> session = LoadSession( scenePath, &pJob );
	Check( session != nullptr, "scene loads and session wraps" );
	if( !session ) { if( pJob ) pJob->release(); return; }

	AgentRenderParams p;
	p.renderTarget = AgentRenderTarget::ViewMode;
	p.viewMode     = Implementation::ViewportRenderMode::Direct;
	p.light        = "redlight";
	AgentRenderResult r = session->Render( p );
	Check( r.ok, "mode:\"direct\" + light:\"redlight\" succeeds" );
	if( r.ok ) {
		std::printf( "  direct+solo(red): R=%.6f G=%.6f B=%.6f\n", r.meanR, r.meanG, r.meanB );
		Check( r.meanR > 4.0 * r.meanG && r.meanR > 4.0 * r.meanB,
		       "MONEY ASSERTION: mode:\"direct\" + light:\"redlight\" is strongly R-dominant -- the "
		       "BeautyVariant ephemeral caster genuinely honours light solo, not just the production path" );
		Check( r.message.find( "light solo" ) != std::string::npos,
		       "result message notes the active light solo under a BeautyVariant mode" );
	}
}

int main()
{
	RunUnknownLightFailsTest();
	RunLightIgnoredUnderDataModesTest();
	RunUnbiasedPartitionTest();
	RunUnbiasedPartitionMISHeavyTest();
	RunEnvironmentPartitionTest();
	RunExplicitLightPartitionTest();
	RunNonPathTracingIntegratorRefusesSoloTest();
	RunLightSoloComposesWithBeautyVariantTest();

	std::printf( "\nLightSoloTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
