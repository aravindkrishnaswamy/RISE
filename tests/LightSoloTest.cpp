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
	std::string path = dir + name;
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
	"uniformcolor_painter\n{\n\tname redlight_pnt\n\tcolor 1.0 0.0 0.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname redlight_mat\n\texitance redlight_pnt\n\tmaterial none\n\tscale 1.2\n}\n\n"
	"box_geometry\n{\n\tname redlight_geo\n\twidth 10\n\theight 0.2\n\tdepth 10\n}\n\n"
	"standard_object\n{\n\tname redlight\n\tgeometry redlight_geo\n\tmaterial redlight_mat\n\tposition -5.2 1.4 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname bluelight_pnt\n\tcolor 0.0 0.0 1.0\n}\n\n"
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
	// genuinely discriminating MC tolerance.  8 spp/pixel * 80*60 pixels
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
	// genuinely discriminating MC tolerance.  8 spp/pixel * 80*60 pixels
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
	RunLightSoloComposesWithBeautyVariantTest();

	std::printf( "\nLightSoloTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
