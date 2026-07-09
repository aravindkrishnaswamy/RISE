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

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
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

	RunThrowDuringOverrideTest();

	std::printf( "=== AgentFrameStoreIsolationTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
