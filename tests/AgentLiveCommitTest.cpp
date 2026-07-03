//////////////////////////////////////////////////////////////////////
//
//  AgentLiveCommitTest.cpp - Facet 5 slice 1b: prove the agent commit,
//    routed through SceneEditController's cancel-and-park edit path, is
//    SAFE against a live render thread and the D2-re-derive rebind hazard.
//
//  Slice 1a made ProposePatch call Job::ApplyCstParamEdit DIRECTLY -- fine
//  HEADLESS (single-threaded), but in a live GUI the Job is driven by a
//  SceneEditController running a RENDER THREAD.  A background agent commit
//  that called ApplyCstParamEdit directly would (a) race the render thread
//  reading the live Scene, and (b) on a D2 full re-derive (ClearAll +
//  rebuild Scene/managers) leave the editor's cached pointers DANGLING.
//
//  Slice 1b routes the commit through SceneEditController::ApplyAgentParamEdit,
//  which cancel-and-parks the render thread (holding mMutex across the WHOLE
//  ApplyCstParamEdit + rebind + version-bump) and calls RebindEditorToJob on
//  a D2 (codes 2/3).  This test drives that path with REAL threads:
//
//    1. Two-client concurrency  -- a "GUI" thread (SetProperty) + an "agent"
//       thread (ApplyAgentParamEdit) hammer the same controller concurrently
//       while the render thread cycles; assert no crash, coherent final state,
//       render thread still alive.
//    2. Rebind-after-D2         -- an agent commit that forces a D2 full
//       re-derive (a material edit on a variant scene); assert the editor is
//       USABLE afterward (a subsequent SetProperty through mEditor succeeds --
//       proving no dangling pointer).
//    3. Conflict cross-path     -- capture baseHeadVersion, GUI SetProperty
//       bumps the revision, a stale agent commit -> conflict (no mutation);
//       re-read + retry -> success.  (1a's precondition through the 1b path.)
//    4. Render-parked during edit -- assert (via mRendering / cancel counter)
//       that an agent edit cancel-and-parks rather than racing an in-flight
//       render pass.
//
//  Self-contained: no RISE_MEDIA_PATH, an inline native-v7 scene, OIDN off
//  (no rasterizer -- a mock DoOneRenderPass simulates render work in
//  cancel-checked slices, so the render thread cycles fast and the CST edit
//  path is exercised for real).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <fstream>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n )
{
	if( c ) { ++passCount; }
	else    { ++failCount; std::cout << "  FAIL: " << n << std::endl; }
}

//////////////////////////////////////////////////////////////////////
// A SceneEditController subclass whose DoOneRenderPass simulates render
// work in cancel-checked slices (mirrors SceneEditorCancelRestartTest).
// This gives us a REAL render thread that HOLDS the "rendering" state for
// an observable window and observes IsCancelRequested at slice boundaries
// -- so an edit that cancel-and-parks is observable via the cancel counter
// / mRendering, and a race between the edit path and the render loop would
// surface as a crash under the repeated-iteration stress below.
//////////////////////////////////////////////////////////////////////
class TestController : public SceneEditController
{
public:
	TestController( IJobPriv& job, unsigned int simulatedRenderMs = 20 )
	: SceneEditController( job, /*interactiveRasterizer*/0 )
	, mSimulatedRenderMs( simulatedRenderMs )
	, mCompletedCount( 0 )
	{}

	unsigned int CompletedCount() const { return mCompletedCount.load(); }

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
		mCompletedCount.fetch_add( 1 );
	}

private:
	unsigned int              mSimulatedRenderMs;
	std::atomic<unsigned int> mCompletedCount;
};

// A base (variant-free) scene.  `lum` is a UNIQUELY-named luminaire so both
// the GUI path (rebind the `exitance` painter slot between `white`/`grey`) and
// the agent path (edit the numeric `scale` param) resolve unambiguously; each
// edit re-derives INCREMENTALLY (code 1) -- fast, non-D2 -- for the
// concurrency + conflict tests.  Two painters so the GUI has a distinct value
// to flip the slot to.
static const char* kBaseScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"uniformcolor_painter\n{\nname grey\ncolor 0.5 0.5 0.5\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

// A scene that DECLARES a scene_variant, so ANY edit forces a D2 full
// re-derive (DeriveToJobIncremental refuses WHOLESALE when the Job
// HasSceneVariants -- the override bake is whole-document).  The variant
// overrides a SEPARATE, `sky` material so `lum` stays UNIQUELY named -- an
// edit on `lum` is therefore both unambiguous AND a guaranteed D2.  This is
// the keystone rebind-after-D2 recipe.
static const char* kVariantScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"uniformcolor_painter\n{\nname grey\ncolor 0.5 0.5 0.5\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"lambertian_luminaire_material\n{\nname sky\nexitance white\nscale 2.0\nmaterial none\n}\n"
	"lambertian_luminaire_material\n{\nname sky\nvariant night\nexitance white\nscale 0.0\nmaterial none\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n"
	"scene_variant\n{\nname night\n}\n";

static Job* LoadScene( const char* text, const char* tmpPath )
{
	{ std::ofstream o( tmpPath ); o << text; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( tmpPath ) ) {
		pJob->release();
		std::remove( tmpPath );
		return nullptr;
	}
	return pJob;
}

// The luminaire's emissive scale (red channel of average radiant exitance),
// -1 if missing -- a live read of the DERIVED scene state so we can confirm an
// edit actually reached the managers.
static double LumR( Job& j )
{
	IMaterial* x = j.GetMaterials() ? j.GetMaterials()->GetItem( "lum" ) : 0;
	if( !x || !x->GetEmitter() ) return -1.0;
	return (double)x->GetEmitter()->averageRadiantExitance().r;
}

//////////////////////////////////////////////////////////////////////
// A Job subclass that FORCES a code-3 ("replaced-but-diagnosed") return from
// ApplyCstParamEdit while REALLY performing the underlying re-derive.  A genuine
// code 3 is a should-not-happen divergence (the validate dry-run passes but the
// real re-derive diagnoses), which cannot be synthesized from scene DATA alone.
// So we exploit the fact that Job::ApplyCstParamEdit is a virtual on IJob: the
// override calls the base (which does the REAL D2 ClearAll + rebuild -- the Scene
// and managers are genuinely replaced, so the caller's rebind is exercised for
// real, no UAF) and, when the base returns 2 (a clean D2 replace), rewrites the
// result to 3.  This drives the EXACT decoupling under test -- the live Scene was
// mutated (managers replaced) yet the code reports FAILURE -- through the real
// controller/editor code paths with a real replaced Scene.  Only ApplyCstParamEdit
// is overridden (both edit layers under test -- agent ApplyAgentParamEdit and GUI
// SetProperty(Material) -- route through it).  Requires a VARIANT scene so the
// edit takes the D2 path (base returns 2 -> we rewrite to 3); on a base scene the
// incremental code 1 is left untouched.
//////////////////////////////////////////////////////////////////////
class CodeThreeJob : public Job
{
public:
	CodeThreeJob() : Job(), mForceCodeThree( true ) {}
	void SetForceCodeThree( bool on ) { mForceCodeThree = on; }
	int ApplyCstParamEdit( const char* entityName, const char* entityKind, const char* role, int occ, const char* newValue ) override
	{
		const int base = Job::ApplyCstParamEdit( entityName, entityKind, role, occ, newValue );
		// Only rewrite a clean D2 replace (2) into a diagnosed replace (3): the Scene + managers
		// WERE really replaced by the base call, so the "rebind + re-render but report failure"
		// contract is exercised against a genuinely-replaced live Scene.
		if( mForceCodeThree && base == 2 ) return 3;
		return base;
	}
private:
	bool mForceCodeThree;
};

//////////////////////////////////////////////////////////////////////
// Test 1: two-client concurrency.  A "GUI" thread (SetProperty on the
// selected material) and an "agent" thread (ApplyAgentParamEdit) hammer the
// same controller concurrently while the render thread cycles.  Both mutate
// the SAME `lum.scale` param through the SAME cancel-and-park path, so they
// MUST serialize under mMutex.  Assert: no crash, the render thread keeps
// running, a coherent final Document/derived-scene, and edits actually landed.
//////////////////////////////////////////////////////////////////////
static void TestTwoClientConcurrency()
{
	std::cout << "Test 1: two-client concurrency (GUI SetProperty || agent commit)..." << std::endl;

	const char* tmp = "agentlive_concurrency.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );

		// Select the material so the GUI SetProperty(Material) path resolves.
		c.SetSelection( SceneEditController::Category::Material, String( "lum" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const int kIters = 200;
		std::atomic<int> guiOk( 0 ), agentApplied( 0 ), agentReject( 0 );

		std::thread guiThread( [&]{
			for( int i = 0; i < kIters; ++i ) {
				// GUI client: rebind the `exitance` painter slot (a valid,
				// CST-routed material edit that bumps the head), flipping
				// between two registered painters.
				const char* p = ( i & 1 ) ? "grey" : "white";
				if( c.SetPropertyForCategory( SceneEditController::Category::Material,
				                              String( "exitance" ), String( p ) ) )
					guiOk.fetch_add( 1 );
			}
		} );

		std::thread agentThread( [&]{
			for( int i = 0; i < kIters; ++i ) {
				char v[32];
				std::snprintf( v, sizeof(v), "%.3f", 2.0 + ( i % 5 ) );
				const SceneEditController::AgentCommitResult r =
					c.ApplyAgentParamEdit(
						String( "lum" ), String( "lambertian_luminaire_material" ),
						String( "scale" ), String( v ),
						/*baseVersionOrNull*/ nullptr );
				if( r.applied )        agentApplied.fetch_add( 1 );
				else if( !r.conflict ) agentReject.fetch_add( 1 );
			}
		} );

		guiThread.join();
		agentThread.join();

		// The render thread must still be alive + cycling after the storm.
		Check( c.IsRunning(), "render thread still running after concurrent edit storm" );
		const unsigned int rc = c.ForTest_GetRenderCount();
		Check( c.ForTest_WaitForRenders( rc + 1, 3000 ),
		       "render thread still produces fresh passes after the storm" );

		// Both clients made progress (the edits serialized rather than one
		// starving) and no agent edit spuriously rejected (all targets valid).
		std::cout << "    guiOk=" << guiOk.load()
		          << " agentApplied=" << agentApplied.load()
		          << " agentReject=" << agentReject.load() << std::endl;
		Check( guiOk.load() > 0,        "GUI thread landed at least one edit" );
		Check( agentApplied.load() > 0, "agent thread landed at least one clean apply" );
		Check( agentReject.load() == 0, "no agent edit spuriously rejected (all valid targets)" );

		// Coherent final state: the derived scene reflects SOME valid edited
		// scale (a well-formed emissive value), i.e. the concurrent mutation
		// did not corrupt the Document/managers into an unresolvable state.
		const double finalR = LumR( *pJob );
		Check( finalR >= 0.0, "final derived material is coherent (resolvable + emissive)" );

		c.Stop();
		Check( !c.IsRunning(), "controller stops + joins cleanly" );
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 2: rebind-after-D2 (the keystone).  An agent commit on a VARIANT
// scene forces a D2 full re-derive (rawCode 2), which ClearAll's + rebuilds
// the Scene + managers.  ApplyAgentParamEdit must RebindEditorToJob so the
// editor's cached pointers don't dangle -- proven by a subsequent edit
// THROUGH mEditor (SetProperty) succeeding rather than segfaulting.
//////////////////////////////////////////////////////////////////////
static void TestRebindAfterD2()
{
	std::cout << "Test 2: rebind-after-D2 (agent commit forces a full re-derive)..." << std::endl;

	const char* tmp = "agentlive_d2.RISEscene";
	Job* pJob = LoadScene( kVariantScene, tmp );
	Check( pJob != nullptr, "variant scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// Agent commit: edit lum.scale.  The variant closure forces a D2.
		const SceneEditController::AgentCommitResult r =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "3.5" ),
				/*baseVersionOrNull*/ nullptr );
		std::cout << "    rawCode=" << r.rawCode << " status=" << r.status.c_str() << std::endl;
		Check( r.applied, "agent commit applied cleanly" );
		Check( r.rawCode == 2, "the edit took the D2 full-re-derive path (variant closure)" );

		// The edit reached the (rebuilt) managers: scale is now ~3.5.
		Check( LumR( *pJob ) > 3.0 && LumR( *pJob ) < 4.0,
		       "derived scene reflects the D2 edit (lum scale ~3.5)" );

		// KEYSTONE: use the editor AFTER the D2.  If RebindEditorToJob wasn't
		// called, mEditor's cached scene/manager pointers dangle into freed
		// storage and this SetProperty (which routes through mEditor.Apply,
		// and itself re-derives -- another D2 on the variant scene) is a
		// use-after-free.  `lum` is uniquely named so the slot edit resolves.
		// Reaching + passing this proves the rebind kept mEditor valid.
		c.SetSelection( SceneEditController::Category::Material, String( "lum" ) );
		const bool editOk = c.SetPropertyForCategory(
			SceneEditController::Category::Material, String( "exitance" ), String( "grey" ) );
		Check( editOk, "SetProperty THROUGH mEditor succeeds after the D2 (editor rebound -- no UAF)" );

		// A second agent D2 commit + a THIRD editor edit -- exercise the
		// rebind repeatedly (each D2 rebuilds; each must rebind).
		const SceneEditController::AgentCommitResult r2 =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "6.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( r2.applied && r2.rawCode == 2, "second agent commit is a clean D2" );
		// LumR is scale * exitance-colour (the earlier keystone flipped exitance
		// to `grey`=0.5), so assert coherence rather than a raw scale -- the
		// clean rawCode-2 above already proves the edit landed.
		Check( LumR( *pJob ) >= 0.0, "second D2 left the material coherent (resolvable + emissive)" );
		const bool editOk2 = c.SetPropertyForCategory(
			SceneEditController::Category::Material, String( "exitance" ), String( "white" ) );
		Check( editOk2, "editor still usable after a second D2 rebind" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 3: conflict cross-path.  Capture baseHeadVersion, do a GUI
// SetProperty (bumps the revision), then an agent commit with the STALE base
// -> conflict (no mutation); re-read the head + retry -> success.  Proves
// 1a's optimistic-concurrency precondition works THROUGH the 1b controller
// path AND across the GUI/agent boundary.
//////////////////////////////////////////////////////////////////////
static void TestConflictCrossPath()
{
	std::cout << "Test 3: conflict cross-path (GUI bumps head, stale agent commit conflicts)..." << std::endl;

	const char* tmp = "agentlive_conflict.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.SetSelection( SceneEditController::Category::Material, String( "lum" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// Capture the base head-version the agent will build against.
		const RISE::Cst::CstHeadVersion base = pJob->GetCstHeadVersion();
		Check( base.uuid != 0, "captured a non-sentinel base head-version" );

		// GUI edit moves the head (a material slot rebind bumps the revision).
		const bool guiOk = c.SetPropertyForCategory(
			SceneEditController::Category::Material, String( "exitance" ), String( "grey" ) );
		Check( guiOk, "GUI SetProperty succeeds" );
		const RISE::Cst::CstHeadVersion afterGui = pJob->GetCstHeadVersion();
		Check( afterGui != base, "GUI edit bumped the head-version (revision advanced)" );

		// Stale agent commit (built against the pre-GUI base) -> CONFLICT, no mutation.
		const RISE::Cst::CstHeadVersion beforeStale = pJob->GetCstHeadVersion();
		const SceneEditController::AgentCommitResult stale =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "9.0" ),
				/*baseVersionOrNull*/ &base );
		Check( stale.conflict, "stale agent commit reports a conflict" );
		Check( !stale.applied, "stale agent commit did not apply" );
		Check( stale.status == String( "conflict" ), "status is \"conflict\"" );
		Check( pJob->GetCstHeadVersion() == beforeStale,
		       "conflict left the head byte-identical (no mutation)" );
		Check( stale.headVersion == beforeStale,
		       "conflict result carries the CURRENT head (for the caller to re-read)" );

		// Re-read the head + retry with the FRESH base -> success.
		const RISE::Cst::CstHeadVersion fresh = stale.headVersion;
		const SceneEditController::AgentCommitResult retry =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "9.0" ),
				/*baseVersionOrNull*/ &fresh );
		Check( retry.applied, "re-proposed agent commit (fresh base) applies cleanly" );
		Check( retry.headVersion != fresh, "successful retry bumped the head again" );
		// LumR = scale * exitance-colour; the GUI flipped exitance to `grey`
		// (0.5) above, so scale 9.0 -> ~4.5.  Assert coherence + that the value
		// moved measurably from the pre-retry state (the edit reached the scene).
		Check( LumR( *pJob ) > 4.0 && LumR( *pJob ) < 5.0,
		       "the retried edit reached the scene (scale 9.0 * grey 0.5 ~= 4.5)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 4: render-parked during edit.  A long render pass is in flight; an
// agent commit must cancel-and-park it (not race it).  Assert the cancel
// counter advances (the edit tripped the in-flight pass) and mRendering is
// false at the moment the edit returns (it waited for the pass to drain).
//////////////////////////////////////////////////////////////////////
static void TestRenderParkedDuringEdit()
{
	std::cout << "Test 4: render-parked during edit (agent commit cancel-and-parks)..." << std::endl;

	const char* tmp = "agentlive_park.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		// Long simulated render (300ms) so an edit reliably lands mid-pass.
		TestController c( *pJob, /*simulatedRenderMs*/300 );
		c.Start();

		// Let a render pass get in flight, then fire an agent commit.
		std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
		const unsigned int cancelsBefore = c.ForTest_GetCancelCount();

		const SceneEditController::AgentCommitResult r =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "3.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( r.applied, "agent commit applied" );

		// The commit returned only AFTER the render thread parked -- so when it
		// mutated the scene, mRendering was false (it held mMutex across the
		// park + the mutation).  We assert the cancel counter advanced, proving
		// the in-flight pass was cancelled by this edit rather than raced.
		const unsigned int cancelsAfter = c.ForTest_GetCancelCount();
		std::cout << "    cancelsBefore=" << cancelsBefore
		          << " cancelsAfter=" << cancelsAfter << std::endl;
		Check( cancelsAfter > cancelsBefore,
		       "agent commit tripped the render cancel (parked the in-flight pass)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 5: the AgentSession live-mode attach.  ProposePatch routed through an
// attached controller must behave identically to a direct ApplyAgentParamEdit
// (same apply/conflict semantics), and DETACHING reverts to the direct path.
//////////////////////////////////////////////////////////////////////
static void TestAgentSessionLiveMode()
{
	std::cout << "Test 5: AgentSession live-mode attach (ProposePatch -> controller)..." << std::endl;

	const char* tmp = "agentlive_session.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// Wrap the SAME Job the controller drives, then attach the controller.
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Check( sess != nullptr, "AgentSession wraps the live Job" );
		Check( !sess->HasController(), "session starts un-attached (direct-Job mode)" );
		sess->AttachController( &c );
		Check( sess->HasController(), "session reports attached after AttachController" );

		// A propose_patch now routes through the controller's render-safe path.
		Agent::AgentSetPatch p;
		p.target = "lum";
		p.kind   = "lambertian_luminaire_material";
		p.param  = "scale";
		p.value  = "3.75";
		Agent::AgentPatchResult pr = sess->ProposePatch( p );
		Check( pr.applied, "ProposePatch (attached) applies cleanly" );
		Check( pr.status == "applied", "attached ProposePatch status is \"applied\"" );
		Check( LumR( *pJob ) > 3.0 && LumR( *pJob ) < 4.5,
		       "attached ProposePatch reached the scene" );

		// The render thread is still alive after the attached commit (it went
		// through cancel-and-park, not a raw direct edit).
		Check( c.IsRunning(), "render thread alive after attached ProposePatch" );

		// Conflict through the attached path too: a stale base -> conflict.
		const RISE::Cst::CstHeadVersion cur = pJob->GetCstHeadVersion();
		RISE::Cst::CstHeadVersion staleBase = cur;
		staleBase.revision += 100;   // definitely not the current head
		Agent::AgentSetPatch pc = p;
		pc.value = "5.0";
		pc.hasBaseVersion = true;
		pc.baseVersion = staleBase;
		Agent::AgentPatchResult prc = sess->ProposePatch( pc );
		Check( prc.status == "conflict", "attached ProposePatch honours the stale-base conflict" );
		Check( !prc.applied, "conflicting attached ProposePatch did not apply" );

		// Detach -> the direct-Job path resumes.  We must STOP the controller
		// first (the direct path is not render-thread-safe by itself; detach
		// models "the GUI closed").  A direct propose then still applies.
		c.Stop();
		sess->AttachController( nullptr );
		Check( !sess->HasController(), "session detaches (direct-Job mode restored)" );
		Agent::AgentSetPatch p2 = p;
		p2.value = "2.5";
		Agent::AgentPatchResult pr2 = sess->ProposePatch( p2 );
		Check( pr2.applied, "detached ProposePatch applies via the direct-Job path" );
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 6 (Model-B code-3 re-render fix): a DIAGNOSED CST re-derive (Job code 3)
// REPLACED the live Scene + managers (best-effort, diagnostics logged) but reports
// FAILURE.  Both edit layers must still RE-RENDER the viewport to show the replaced
// Scene (else stale pre-edit pixels), while keeping code 3's failure semantics
// (agent: applied=false / status="diagnosed"; GUI: SetProperty returns false).
// Driven with CodeThreeJob, which rewrites a real D2 replace (code 2) to code 3.
//
// Kick observability: a kick stores mEditPending + notifies, which wakes the render
// loop for a fresh pass -- so a re-render is observable as the render count
// advancing.  We snapshot the count, do the code-3 edit, and assert a NEW pass
// fires (the fix) while the result reports failure.
//////////////////////////////////////////////////////////////////////
static void TestCodeThreeRerender()
{
	std::cout << "Test 6: code-3 diagnosed re-derive still re-renders (both layers)..." << std::endl;

	const char* tmp = "agentlive_code3.RISEscene";
	// Load the VARIANT scene (any edit forces a D2 -> base returns 2 -> CodeThreeJob rewrites to 3)
	// into a CodeThreeJob so ApplyCstParamEdit reports the forced code 3.
	{ std::ofstream o( tmp ); o << kVariantScene; }
	CodeThreeJob* pJob = new CodeThreeJob();
	Check( pJob->LoadAsciiSceneViaCst( tmp ), "variant scene loads into CodeThreeJob via the CST path" );
	if( !pJob ) { std::remove( tmp ); return; }

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		//------------------------------------------------------------------
		// AGENT LAYER: ApplyAgentParamEdit on the variant scene -> forced code 3.
		//------------------------------------------------------------------
		const unsigned int before = c.ForTest_GetRenderCount();
		const SceneEditController::AgentCommitResult r =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "3.5" ),
				/*baseVersionOrNull*/ nullptr );
		std::cout << "    [agent] rawCode=" << r.rawCode << " status=" << r.status.c_str() << std::endl;
		Check( r.rawCode == 3, "agent edit took the forced code-3 (diagnosed replace) path" );
		Check( !r.applied, "code-3 agent edit reports FAILURE (applied=false) -- semantics unchanged" );
		Check( r.status == String( "diagnosed" ), "code-3 agent edit status is \"diagnosed\" -- semantics unchanged" );
		// THE FIX: the viewport re-renders (a fresh pass fires) despite the failure,
		// because the live Scene was REPLACED.  Before the fix this timed out (kick was
		// gated on r.applied, which is false on code 3).
		Check( c.ForTest_WaitForRenders( before + 1, 3000 ),
		       "code-3 agent edit RE-RENDERS the viewport (kick decoupled from success)" );
		// The edit really landed in the (rebuilt) managers -- proves the Scene was replaced,
		// not just a spurious kick.
		Check( LumR( *pJob ) > 3.0 && LumR( *pJob ) < 4.0,
		       "code-3 re-derive replaced the live Scene (lum scale ~3.5 reflected)" );
		// F5 slice 1b (data-loss fix): a diagnosed code-3 MUTATED the retained
		// Document (revision bumped) -- so it is genuinely UNSAVED and must mark
		// the editor dirty, exactly like a clean apply.  (applied=false does NOT
		// mean "nothing changed" for code 3.)
		Check( c.HasUnsavedChanges(),
		       "code-3 diagnosed edit marks the editor DIRTY (the Document WAS mutated)" );

		//------------------------------------------------------------------
		// GUI LAYER: SetProperty(Material) -> mEditor.Apply -> RouteCstParamEdit_ -> forced code 3.
		//------------------------------------------------------------------
		c.SetSelection( SceneEditController::Category::Material, String( "lum" ) );
		const unsigned int before2 = c.ForTest_GetRenderCount();
		const bool guiOk = c.SetPropertyForCategory(
			SceneEditController::Category::Material, String( "exitance" ), String( "grey" ) );
		std::cout << "    [gui] SetProperty returned " << ( guiOk ? "true" : "false" ) << std::endl;
		Check( !guiOk, "code-3 GUI edit reports FAILURE (SetProperty returns false) -- semantics unchanged" );
		Check( c.ForTest_WaitForRenders( before2 + 1, 3000 ),
		       "code-3 GUI edit RE-RENDERS the viewport (kick decoupled from ok)" );

		//------------------------------------------------------------------
		// CONTROL: with the force OFF, a clean D2 (code 2) must still re-render AND report success
		// (proves we didn't break the clean path and the code-3 path shares the same kick).
		//------------------------------------------------------------------
		pJob->SetForceCodeThree( false );
		const unsigned int before3 = c.ForTest_GetRenderCount();
		const SceneEditController::AgentCommitResult r2 =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "6.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( r2.rawCode == 2 && r2.applied, "control: force-off edit is a clean D2 (code 2, applied)" );
		Check( c.ForTest_WaitForRenders( before3 + 1, 3000 ),
		       "control: clean D2 re-renders (shared kick path intact)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 7 (Model-B F5 slice 1b DATA-LOSS fix): an agent commit that mutates
// the retained CST head DIRECTLY (via ApplyCstParamEdit, bypassing
// mEditor.Apply) must MARK THE EDITOR DIRTY -- else the GUI still believes
// the scene is CLEAN, the Save button stays disabled, and a close-without-
// prompt path silently LOSES the agent's edit.
//
// Asserts, via the SAME GUI-facing surface the shells consult
// (SceneEditController::HasUnsavedChanges, which forwards to
// SceneEditor::HasUnsavedChanges):
//   - CLEAN before any edit (RED-PROVE: false pre-edit).  Against the
//     pre-fix code this stays false AFTER the edit too -> the applied-edit
//     assertion below fails at parent d010c37a.
//   - a code-1 clean apply flips HasUnsavedChanges() -> true.
//   - the dirty-changed LISTENER fires exactly once on the clean->dirty
//     transition (with true); a SECOND still-dirty edit does NOT re-fire.
//   - a code-0 REJECT (bogus entity) and a CONFLICT (stale base) do NOT
//     set dirty.
//   - a successful Save-As (RequestSave to a fresh path) CLEARS it
//     (proving the agent-set mark rides the existing save-clear path).
//////////////////////////////////////////////////////////////////////
static void TestAgentEditMarksDirty()
{
	std::cout << "Test 7: agent commit marks the editor dirty (data-loss fix)..." << std::endl;

	const char* tmp = "agentlive_dirty.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );

		// Install a dirty-changed listener BEFORE any edit so we observe the
		// clean->dirty transition.  It records every callback value + count.
		std::vector<bool> dirtyEvents;
		std::mutex        evtMutex;
		c.SetDirtyChangedListener( [&]( bool has ) {
			std::lock_guard<std::mutex> lk( evtMutex );
			dirtyEvents.push_back( has );
		} );

		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// RED-PROVE: a freshly-loaded scene with no edits is CLEAN.
		Check( !c.HasUnsavedChanges(), "scene is CLEAN before any agent edit (red-prove pre-edit false)" );

		//------------------------------------------------------------------
		// A code-0 REJECT (bogus entity) must NOT set dirty (head unchanged).
		//------------------------------------------------------------------
		const SceneEditController::AgentCommitResult reject =
			c.ApplyAgentParamEdit(
				String( "does_not_exist" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "3.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( !reject.applied && reject.rawCode == 0, "bogus-entity edit is a code-0 reject" );
		Check( !c.HasUnsavedChanges(), "a code-0 REJECT does NOT mark dirty (head unchanged)" );

		//------------------------------------------------------------------
		// A CONFLICT (stale base) must NOT set dirty either.
		//------------------------------------------------------------------
		const RISE::Cst::CstHeadVersion cur = pJob->GetCstHeadVersion();
		RISE::Cst::CstHeadVersion staleBase = cur;
		staleBase.revision += 100;   // definitely not the current head
		const SceneEditController::AgentCommitResult conflict =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "3.0" ),
				/*baseVersionOrNull*/ &staleBase );
		Check( conflict.conflict && !conflict.applied, "stale-base edit is a conflict (no mutation)" );
		Check( !c.HasUnsavedChanges(), "a CONFLICT does NOT mark dirty (head unchanged)" );

		// No dirty event yet -- nothing has transitioned.
		{
			std::lock_guard<std::mutex> lk( evtMutex );
			Check( dirtyEvents.empty(), "no dirty-changed event fired before the first APPLIED edit" );
		}

		//------------------------------------------------------------------
		// A clean code-1 apply flips HasUnsavedChanges() -> true.  This is
		// the RED-PROVE assertion: at parent d010c37a it stays FALSE.
		//------------------------------------------------------------------
		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "4.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied.applied && applied.rawCode == 1, "clean apply is a code-1 incremental" );
		Check( c.HasUnsavedChanges(), "an APPLIED agent edit marks the editor DIRTY (the fix)" );
		// A1 pin: "lambertian_luminaire_material" is a KNOWN kind
		// (endsWith "_material"), so the mark routes to the per-category
		// entity channel -- the object-transform channel (Phase 6 mNames)
		// stays EMPTY.
		Check( c.Editor().Dirty().Count() == 0,
		       "applied agent edit does NOT touch the object-transform channel" );
		// Positive routing pin: the KNOWN material kind lands in the
		// per-entity channel (a regression that routes known kinds to
		// the CST-head boolean channel can't pass silently).
		Check( c.Editor().Dirty().EntityCount() == 1,
		       "applied agent edit routes the KNOWN material kind to the per-entity channel" );

		// The listener fired exactly once, with true (clean->dirty).
		{
			std::lock_guard<std::mutex> lk( evtMutex );
			Check( dirtyEvents.size() == 1 && dirtyEvents.back() == true,
			       "dirty-changed listener fired ONCE with true on the clean->dirty transition" );
		}

		//------------------------------------------------------------------
		// A SECOND still-dirty apply must NOT re-fire the listener
		// (transition-only), and HasUnsavedChanges() stays true.
		//------------------------------------------------------------------
		const SceneEditController::AgentCommitResult applied2 =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "5.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied2.applied, "second agent edit applies" );
		Check( c.HasUnsavedChanges(), "editor STILL dirty after the second edit" );
		{
			std::lock_guard<std::mutex> lk( evtMutex );
			Check( dirtyEvents.size() == 1,
			       "a second still-dirty edit does NOT re-fire the listener (transition-only)" );
		}

		//------------------------------------------------------------------
		// A successful Save-As (fresh path -> no external-mod guard) CLEARS
		// dirty, proving the agent-set mark rides the existing save-clear
		// path (mEditor.ClearDirtyState on success).
		//------------------------------------------------------------------
		const char* saveAs = "agentlive_dirty_saveas.RISEscene";
		std::remove( saveAs );
		const SaveResult sr = c.RequestSave( std::string( saveAs ) );
		Check( Succeeded( sr.status ), "Save-As succeeds" );
		Check( !c.HasUnsavedChanges(), "a successful save CLEARS the agent-set dirty (rides the clear path)" );
		{
			std::lock_guard<std::mutex> lk( evtMutex );
			Check( dirtyEvents.size() == 2 && dirtyEvents.back() == false,
			       "dirty-changed listener fired again with false on the dirty->clean save transition" );
		}
		std::remove( saveAs );

		//------------------------------------------------------------------
		// Round-2 hardening (defensive): MarkCstHeadDirty must flip dirty
		// even for a KNOWN kind with an EMPTY name.  The mapped
		// MarkEntityDirty channel SILENTLY NO-OPS on an empty name, so an
		// empty name sets the tracker's first-class CST-head BOOLEAN
		// channel REGARDLESS of kind (the boolean set is unconditional --
		// no name to validate -- so the mark can never no-op).  The agent
		// caller pre-rejects empty names today, but this guards a future
		// 1c caller from re-opening the data-loss.  State is CLEAN here
		// (the save above cleared it).  Red-prove: against the pre-
		// hardening code, MarkEntityDirty(Object,"") no-ops -> stays clean.
		Check( !c.HasUnsavedChanges(), "clean before the empty-name mark probe" );
		c.Editor().MarkCstHeadDirty( "", "standard_object" );
		Check( c.HasUnsavedChanges(),
		       "MarkCstHeadDirty(empty name, KNOWN kind) still flips dirty via the CST-head boolean channel (no silent no-op)" );
		// A1 pin: the dirty state came from the BOOLEAN channel -- no
		// "__cst_head__" sentinel name lands in the object-transform set
		// (the pre-A1 hack) and the per-entity set stays empty.
		Check( c.Editor().Dirty().CstHeadDirty(),
		       "empty-name mark sets the CST-head boolean channel" );
		Check( c.Editor().Dirty().Count() == 0 && !c.Editor().Dirty().Contains( "__cst_head__" ),
		       "empty-name mark does NOT pollute the object-transform channel (no sentinel)" );
		Check( c.Editor().Dirty().EntityCount() == 0,
		       "empty-name mark does NOT touch the per-entity channel" );

		//------------------------------------------------------------------
		// A1 (first-class CST-head channel): an UNKNOWN kind -- the agent
		// can edit painters, and "uniformcolor_painter" maps to none of the
		// tracker's per-entity categories -- must ALSO set the boolean
		// channel, NOT park the painter name in the object-transform set
		// (the pre-A1 semantic overload).  Clear first so this probe
		// observes its own mark only.
		//------------------------------------------------------------------
		c.Editor().ClearDirtyState();
		Check( !c.HasUnsavedChanges(), "clean before the unknown-kind mark probe" );
		c.Editor().MarkCstHeadDirty( "grey", "uniformcolor_painter" );
		Check( c.HasUnsavedChanges(),
		       "MarkCstHeadDirty(painter name, UNKNOWN kind) flips dirty via the CST-head boolean channel" );
		Check( c.Editor().Dirty().CstHeadDirty(),
		       "unknown-kind mark sets the CST-head boolean channel" );
		Check( !c.Editor().Dirty().Contains( "grey" ) && c.Editor().Dirty().Count() == 0,
		       "unknown-kind mark does NOT park the painter name in the object-transform channel" );
		Check( c.Editor().Dirty().EntityCount() == 0,
		       "unknown-kind mark does NOT touch the per-entity channel" );

		//------------------------------------------------------------------
		// A1 review round 1 P2 (OR-merge across restore): a mid-transaction
		// agent commit's CST-head mark must SURVIVE a rollback's dirty-state
		// restore.  The agent path's Document mutation has no EditHistory
		// record, so the rollback's Undo loop can never revert it — if
		// RestoreState plain-copied the boolean back to the clean
		// pre-transaction baseline, HasUnsavedChanges() would go false while
		// the mutated Document survives, and a close-without-prompt would
		// silently LOSE the agent edit.  Simulates the transaction sequence
		// directly: capture clean dirty state (BeginTransaction baseline) →
		// agent mark (mid-transaction commit) → restore (rollback).
		// Red-prove: with a plain-value-copy RestoreState this goes CLEAN.
		//------------------------------------------------------------------
		c.Editor().ClearDirtyState();
		Check( !c.HasUnsavedChanges(), "clean before the restore-survival probe" );
		const auto cleanSnap = c.Editor().CaptureDirtyState();
		c.Editor().MarkCstHeadDirty( "grey", "uniformcolor_painter" );
		Check( c.HasUnsavedChanges(), "agent mark applied after the clean capture" );
		c.Editor().RestoreDirtyState( cleanSnap );
		Check( c.Editor().Dirty().CstHeadDirty(),
		       "CST-head flag SURVIVES a restore of the clean pre-transaction snapshot (OR-merge)" );
		Check( c.HasUnsavedChanges(),
		       "HasUnsavedChanges() still true after the rollback-style restore (no silent agent-edit loss)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 8 (Facet 5 slice 1c-1): the FULL LIVE DISPATCHER path -- exactly the
// mechanism the Mac GUI's `-agentHandleLine:` will invoke.  Slice 1b's Test 5
// proved AgentSession::ProposePatch (a C++ call) routes through an attached
// controller; this test proves the JSON-RPC TRANSPORT above it does too:
//
//   AgentRpcDispatcher::HandleLine("<json-rpc line>")
//     -> the wrapped AgentSession (WrapJob over the SAME live Job)
//     -> the attached SceneEditController::ApplyAgentParamEdit
//     -> the live scene changes + the editor goes dirty.
//
// This is the automated proof of the in-app injection loop: the GUI hands a
// typed JSON-RPC string to `agentHandleLine`, which calls exactly this
// HandleLine on the exact same dispatcher/session/controller wiring.  We drive
// it with RAW JSON-RPC lines (not the C++ struct), parse the JSON responses,
// and assert against the live derived scene + the controller's dirty flag,
// all while a REAL render thread cycles.
//
//   (a) read_document       -> a non-empty document + a non-sentinel headVersion.
//   (b) propose_patch        -> result.applied==true / status=="applied", the
//                               live `lum` scale actually moved, and
//                               HasUnsavedChanges() flipped true.
//   (c) stale propose_patch   -> status=="conflict", applied==false, no mutation.
//////////////////////////////////////////////////////////////////////
static bool JsonResultObj( const std::string& line, Agent::JsonValue& outResult )
{
	Agent::JsonValue root;
	std::string err;
	if( !Agent::JsonParse( line, root, err ) ) return false;
	if( !root.isObject() ) return false;
	const Agent::JsonValue* r = root.find( "result" );
	if( !r || !r->isObject() ) return false;
	outResult = *r;
	return true;
}

static void TestLiveDispatcherPath()
{
	std::cout << "Test 8: live dispatcher path (HandleLine -> attached controller -> live edit)..." << std::endl;

	const char* tmp = "agentlive_dispatcher.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// Build EXACTLY the wiring the GUI's viewport bridge builds:
		// WrapJob over the live Job, then attach the running controller.
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Check( sess != nullptr, "AgentSession wraps the live Job" );
		if( sess ) sess->AttachController( &c );
		Agent::AgentRpcDispatcher disp( std::move( sess ) );

		//------------------------------------------------------------------
		// (a) read_document -- a raw JSON-RPC line in, a JSON response out.
		//------------------------------------------------------------------
		const std::string docResp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"read_document\"}" );
		std::cout << "    read_document resp bytes=" << docResp.size() << std::endl;
		Agent::JsonValue docResult;
		Check( JsonResultObj( docResp, docResult ), "read_document returns a JSON-RPC result object" );
		const Agent::JsonValue* hasDoc = docResult.find( "hasDocument" );
		Check( hasDoc && hasDoc->isBool() && hasDoc->asBool(), "read_document reports hasDocument=true" );
		const Agent::JsonValue* document = docResult.find( "document" );
		Check( document && document->isString() && !document->asString().empty(),
		       "read_document carries the non-empty head text" );
		// The head text is the real serialized scene -- it names our luminaire.
		Check( document && document->asString().find( "lum" ) != std::string::npos,
		       "the document text is the real head (contains the `lum` entity)" );
		// Capture the head-version the dispatcher reported so we can build a
		// STALE base for the conflict check below.
		const Agent::JsonValue* hv = docResult.find( "headVersion" );
		Check( hv && hv->isObject(), "read_document carries a headVersion object" );
		double hvUuid = 0.0, hvRev = 0.0;
		if( hv && hv->isObject() ) {
			const Agent::JsonValue* u = hv->find( "uuid" );
			const Agent::JsonValue* r = hv->find( "revision" );
			if( u ) hvUuid = u->asNumber();
			if( r ) hvRev = r->asNumber();
		}
		Check( hvUuid != 0.0, "headVersion.uuid is non-sentinel (a real retained head)" );

		//------------------------------------------------------------------
		// (b) propose_patch -- recolor/rescale the luminaire THROUGH the
		// transport.  Assert applied + the live scene moved + dirty flipped.
		//------------------------------------------------------------------
		Check( !c.HasUnsavedChanges(), "scene is CLEAN before the dispatched propose_patch" );
		const double beforeR = LumR( *pJob );
		const std::string patchResp = disp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"7.0\"}}" );
		std::cout << "    propose_patch resp=" << patchResp << std::endl;
		Agent::JsonValue patchResult;
		Check( JsonResultObj( patchResp, patchResult ), "propose_patch returns a JSON-RPC result object" );
		const Agent::JsonValue* applied = patchResult.find( "applied" );
		Check( applied && applied->isBool() && applied->asBool(), "propose_patch response says applied=true" );
		const Agent::JsonValue* status = patchResult.find( "status" );
		Check( status && status->isString() && status->asString() == "applied",
		       "propose_patch response status is \"applied\"" );
		// The LIVE derived scene actually changed (scale 7.0 on the white
		// exitance -> LumR ~7, and measurably different from before).
		const double afterR = LumR( *pJob );
		std::cout << "    LumR before=" << beforeR << " after=" << afterR << std::endl;
		Check( afterR > 6.0 && afterR < 8.0, "the LIVE scene changed (lum scale ~7.0 reached the managers)" );
		Check( afterR != beforeR, "the dispatched edit measurably moved the derived material" );
		// The GUI-facing dirty flag flipped -- the Save button would enable.
		Check( c.HasUnsavedChanges(), "the dispatched propose_patch marked the editor DIRTY (Save enables)" );
		// The render thread is still alive (the commit cancel-and-parked, not raced).
		Check( c.IsRunning(), "render thread alive after the dispatched commit" );

		//------------------------------------------------------------------
		// (c) STALE propose_patch -- pass back the ORIGINAL (pre-edit)
		// headVersion as baseHeadVersion; it is now stale (the apply above
		// bumped the revision) -> status=="conflict", no mutation.
		//------------------------------------------------------------------
		const double beforeStale = LumR( *pJob );
		char staleLine[512];
		std::snprintf( staleLine, sizeof(staleLine),
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"1.0\","
			"\"baseHeadVersion\":{\"uuid\":%.0f,\"revision\":%.0f}}}",
			hvUuid, hvRev );
		const std::string staleResp = disp.HandleLine( std::string( staleLine ) );
		std::cout << "    stale propose_patch resp=" << staleResp << std::endl;
		Agent::JsonValue staleResult;
		Check( JsonResultObj( staleResp, staleResult ), "stale propose_patch returns a JSON-RPC result object" );
		const Agent::JsonValue* sStatus = staleResult.find( "status" );
		Check( sStatus && sStatus->isString() && sStatus->asString() == "conflict",
		       "stale propose_patch response status is \"conflict\"" );
		const Agent::JsonValue* sApplied = staleResult.find( "applied" );
		Check( sApplied && sApplied->isBool() && !sApplied->asBool(),
		       "stale propose_patch did not apply" );
		Check( LumR( *pJob ) == beforeStale, "the conflict left the live scene unchanged (no mutation)" );

		c.Stop();
		Check( !c.IsRunning(), "controller stops + joins cleanly after the dispatched edits" );
	}
	pJob->release();
	std::remove( tmp );
}

int main()
{
	std::cout << "=== Agent Live-Commit Test (Facet 5 slice 1b) ===" << std::endl;

	TestTwoClientConcurrency();
	TestRebindAfterD2();
	TestConflictCrossPath();
	TestRenderParkedDuringEdit();
	TestAgentSessionLiveMode();
	TestCodeThreeRerender();
	TestAgentEditMarksDirty();
	TestLiveDispatcherPath();

	std::cout << "\n=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
