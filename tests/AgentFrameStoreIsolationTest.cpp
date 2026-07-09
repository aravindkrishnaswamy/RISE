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
//    assertions, then re-runs the same probe against bdpt_pel_rasterizer
//    and vcm_pel_rasterizer scenes to confirm the fix generalizes across
//    the pixel-based integrator family (both write through the same
//    canonical FrameStore via AcquireRenderImage's dims-exact-match
//    gate, per PixelBasedRasterizerHelper.cpp / BDPTRasterizerBase.cpp /
//    VCMRasterizerBase.cpp).
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

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

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

static const char* const kPtRasterizer =
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}";
static const char* const kBdptRasterizer =
	"bdpt_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}";
static const char* const kVcmRasterizer =
	"vcm_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}";

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
static void RunIsolationProbe( const std::string& label, const std::string& rasterizerChunk )
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

		AgentRenderResult res = session->Render( -1 );

		displayStore->RemoveObserver( &probe );

		Check( res.ok, label + " (no-override): render succeeds" );
		Check( !res.png.empty(), label + " (no-override): PNG bytes are non-empty" );
		Check( displayStore->Generation() == genBefore,
			label + " (no-override): canonical FrameStore Generation() did NOT advance across the agent render" );
		Check( probe.tileCompleteCount == 0,
			label + " (no-override): probe observer saw ZERO OnTileComplete callbacks on the canonical store" );
		Check( probe.frameCompleteCount == 0,
			label + " (no-override): probe observer saw ZERO OnFrameComplete callbacks on the canonical store" );
		Check( concreteRast->GetFrameStore() == displayStore,
			label + " (no-override): rasterizer's FrameStore identity is restored to the captured display store" );
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

	pJob->release();
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

	std::printf( "=== AgentFrameStoreIsolationTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
