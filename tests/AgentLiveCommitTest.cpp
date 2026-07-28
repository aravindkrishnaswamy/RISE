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
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
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

	//! Block until at least `count` passes have run to COMPLETION (i.e. were
	//! not cancelled).  Unlike ForTest_WaitForRenders -- which counts every
	//! pass, including one that returned early because an edit cancelled it --
	//! this only advances on real work, so "the edit kicked a FRESH pass" stays
	//! a real assertion even when the edit also cancelled a held predecessor.
	bool WaitForCompleted( unsigned int count, unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mHoldMutex );
		return mHoldCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mCompletedCount.load() >= count; } );
	}

	//! Arm the NEXT render pass to enter flight and STAY there until something
	//! trips the rasterizer cancel flag.
	//!
	//! Why this exists: SceneEditController::CancelAndParkRender_ only bumps
	//! the cancel counter `if( mRendering )`.  A test that asserts "this edit
	//! cancel-and-PARKED the in-flight pass" is therefore asserting an effect
	//! whose PRECONDITION is "a pass is in flight right now" -- and the render
	//! loop goes idle after each pass, so the precondition is not free.  Doing
	//! it by sleeping (or by hoping the loop happens to be busy) is a race that
	//! fails whenever the machine is loaded enough to slip the edit into the
	//! idle gap.  Arm this, kick a render, WaitForHeldPass(), and the pass is
	//! PROVABLY in flight when the code under test runs -- no timing assumption
	//! at all.  The hold is bounded so a genuine "nobody ever cancels" bug
	//! surfaces as a failed assertion rather than a hung suite.
	void ArmHeldPass()
	{
		std::lock_guard<std::mutex> lk( mHoldMutex );
		mHoldArmed = true;
		mHoldReached = false;
	}

	//! Block until the armed pass has entered flight (mRendering is true and
	//! the pass body is holding).  False on timeout.
	bool WaitForHeldPass( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mHoldMutex );
		return mHoldCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mHoldReached; } );
	}

protected:
	void DoOneRenderPass() override
	{
		bool held = false;
		{
			std::lock_guard<std::mutex> lk( mHoldMutex );
			if( mHoldArmed ) { mHoldArmed = false; mHoldReached = true; held = true; }
		}
		if( held )
		{
			mHoldCV.notify_all();
			// Stay in flight until the code under test cancels us.  The bound
			// only exists so a regression cannot wedge the suite; it is not a
			// timing assumption -- the assertion that follows is on the cancel
			// counter, which is what actually proves the park happened.
			const auto deadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds( 10 );
			while( !IsCancelRequested()
			    && std::chrono::steady_clock::now() < deadline )
			{
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			}
			return;   // cancelled: deliberately NOT counted as completed
		}
		const unsigned int sliceMs = 2;
		const unsigned int slices  = ( mSimulatedRenderMs + sliceMs - 1 ) / sliceMs;
		for( unsigned int i = 0; i < slices; ++i )
		{
			if( IsCancelRequested() ) return;
			std::this_thread::sleep_for( std::chrono::milliseconds( sliceMs ) );
		}
		{
			std::lock_guard<std::mutex> lk( mHoldMutex );
			mCompletedCount.fetch_add( 1 );
		}
		mHoldCV.notify_all();
	}

private:
	unsigned int              mSimulatedRenderMs;
	std::atomic<unsigned int> mCompletedCount;
	std::mutex                mHoldMutex;
	std::condition_variable   mHoldCV;
	bool                      mHoldArmed = false;
	bool                      mHoldReached = false;
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

// Shared-undo U1: a base scene whose `lum` OMITS the `scale` param (defaulted
// to 1.0 by the descriptor -- see LambertianLuminaireMaterialAsciiChunkParser's
// Finalize).  An agent edit to `scale` on this scene INSERTS the param (it was
// never in the source text), so Undo's inverse must be a REMOVE, not a re-SET
// of a nonexistent prior value.
static const char* kNoScaleScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"uniformcolor_painter\n{\nname grey\ncolor 0.5 0.5 0.5\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nmaterial none\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

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

// P1-1 red-prove helper: the G and B channels of `lum`'s average radiant exitance --
// same live-read rationale as LumR, but exposes all three components so a multi-token
// `color r g b` capture/restore can be checked component-by-component (the reported
// corruption was r=5 g=0 b=0 instead of 5 5 5 -- a single-channel read would miss it).
static double LumG( Job& j )
{
	IMaterial* x = j.GetMaterials() ? j.GetMaterials()->GetItem( "lum" ) : 0;
	if( !x || !x->GetEmitter() ) return -1.0;
	return (double)x->GetEmitter()->averageRadiantExitance().g;
}
static double LumB( Job& j )
{
	IMaterial* x = j.GetMaterials() ? j.GetMaterials()->GetItem( "lum" ) : 0;
	if( !x || !x->GetEmitter() ) return -1.0;
	return (double)x->GetEmitter()->averageRadiantExitance().b;
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
// is overridden per edit layer -- the GUI layer (SetProperty(Material)) routes
// through ApplyCstParamEdit, while the agent layer (ApplyAgentParamEdit) routes
// through the round-2 gated ApplyCstParamEditChecked, so BOTH virtuals are
// overridden with the same 2 -> 3 rewrite.  Requires a VARIANT scene so the
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
	int ApplyCstParamEditChecked( const char* entityName, const char* entityKind, const char* role, int occ, const char* newValue ) override
	{
		// The agent layer's route (round-2 P1-A: SceneEditController::ApplyAgentParamEdit and the
		// headless ProposePatch call the gated variant) -- same 2 -> 3 rewrite.
		const int base = Job::ApplyCstParamEditChecked( entityName, entityKind, role, occ, newValue );
		if( mForceCodeThree && base == 2 ) return 3;
		return base;
	}
	// Shared-undo U2 (Test 27): chunk-CRUD verbs are ALWAYS D2-class (never code 1) -- same 2 -> 3
	// rewrite technique so ApplyAgentInsertChunk/ApplyAgentRemoveChunk's forward apply AND their
	// Undo/Redo (RouteAgentChunkCrud_'s ApplyCstInsertChunk / ApplyCstRemoveChunk / ApplyCstRestoreChunkAt
	// calls) all observe a genuinely-replaced-but-diagnosed Scene at every step of the round trip.
	int ApplyCstInsertChunk( const char* chunkText, char* outKeyword, unsigned int keywordMax,
	                         char* outName, unsigned int nameMax, char* outDiag, unsigned int diagMax,
	                         int* outInsertedAt = nullptr ) override
	{
		const int base = Job::ApplyCstInsertChunk( chunkText, outKeyword, keywordMax, outName, nameMax, outDiag, diagMax, outInsertedAt );
		if( mForceCodeThree && base == 2 ) return 3;
		return base;
	}
	int ApplyCstRemoveChunk( const char* target, const char* kind,
	                         char* outKeyword, unsigned int keywordMax, char* outDiag, unsigned int diagMax ) override
	{
		const int base = Job::ApplyCstRemoveChunk( target, kind, outKeyword, keywordMax, outDiag, diagMax );
		if( mForceCodeThree && base == 2 ) return 3;
		return base;
	}
	int ApplyCstRestoreChunkAt( const char* bytesInOrder, int atIndex, bool restoreActiveRasterizer,
	                            char* outDiag, unsigned int diagMax ) override
	{
		const int base = Job::ApplyCstRestoreChunkAt( bytesInOrder, atIndex, restoreActiveRasterizer, outDiag, diagMax );
		if( mForceCodeThree && base == 2 ) return 3;
		return base;
	}
private:
	bool mForceCodeThree;
};

//////////////////////////////////////////////////////////////////////
// A2 (Model-B F5 polish): a Job subclass that forces a code-3 on the
// TRANSFORM-COMMIT seam specifically -- SceneEditor::CommitPendingCstObjectTransforms
// routes a standard_object's net transform through Job::ApplyCstObjectMatrixEdit, not
// through ApplyCstParamEdit (that's a PARAMETER edit; a transform commit rewrites the
// `matrix` param directly via a dedicated virtual -- see Job.h). CodeThreeJob above
// does not touch this path, so a SetProperty(Object, position/orientation/scale) edit
// against it would still report a clean commit even though A2 targets exactly this
// commit's return value. Same 2 -> 3 rewrite technique: the base call performs a REAL
// D2 re-derive (a scene_variant closure forces it, same recipe as kVariantScene), so
// the Scene + managers really are replaced; only the reported code is corrupted to 3.
//////////////////////////////////////////////////////////////////////
class CodeThreeTransformJob : public Job
{
public:
	CodeThreeTransformJob() : Job(), mForceCodeThree( true ) {}
	void SetForceCodeThree( bool on ) { mForceCodeThree = on; }
	int ApplyCstObjectMatrixEdit( const char* objectName, const char* matrix16 ) override
	{
		const int base = Job::ApplyCstObjectMatrixEdit( objectName, matrix16 );
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
		TestController c( *pJob, /*simulatedRenderMs*/300 );
		c.Start();

		// Put a pass PROVABLY in flight, then fire an agent commit.  This used
		// to be "sleep 40 ms into a 300 ms pass and hope" -- generous, but
		// still a bet on scheduler latency (and the same bet Test 5c lost).
		// The gate removes the bet entirely: the pass is holding in flight
		// until this very edit cancels it.
		c.ArmHeldPass();
		c.ForTest_KickRender();
		Check( c.WaitForHeldPass( 5000 ),
		       "a render pass is provably in flight before the agent commit" );
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

		// REJECTION DIAGNOSTICS ON THE LIVE PATH.  A dangling painter reference
		// is refused by the engine with the generic "would not derive in
		// context ... apply failed (e.g. unresolved reference); see log" -- a
		// message an AGENT cannot act on (it names no param, no value, and
		// points at a log it cannot read).  This is not hypothetical: a GUI
		// session driving a local model hit exactly this and stalled.  The GUI
		// is THIS path (attached controller), so the actionable clause must be
		// attached HERE, not only on the headless path where it is easier to
		// test.  Guards that wiring.
		{
			Agent::AgentChunkResult ir = sess->InsertChunk(
				"lambertian_material\n{\nname m_dangling\nreflectance whitte\n}\n" );
			Check( !ir.applied, "live insert with a dangling painter reference is rejected" );
			Check( ir.status == "rejected", "live dangling-reference insert reports status rejected" );
			Check( ir.issues.size() == 1,
			       "live rejection carries exactly one structured issue" );
			if( ir.issues.size() == 1 ) {
				Check( ir.issues[0].param == "reflectance",
				       "live rejection issue names the offending param" );
				Check( ir.issues[0].reason == "unresolved_reference",
				       "live rejection issue carries the unresolved_reference reason" );
				bool suggested = false;
				for( const std::string& s : ir.issues[0].suggestions )
					if( s == "white" ) suggested = true;
				Check( suggested,
				       "live rejection suggests the near-miss painter 'white' for 'whitte'" );
			}
			Check( ir.message.find( "ACTIONABLE" ) != std::string::npos,
			       "live rejection message carries the ACTIONABLE clause the agent can act on" );
			// The engine's own ground-truth text must SURVIVE -- the clause
			// augments it, never replaces it.
			Check( ir.message.find( "would not derive in context" ) != std::string::npos,
			       "live rejection preserves the engine's original cause text" );
		}

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
// Test 5b: actionable REJECTED propose_patch / remove_chunk diagnostics on
// the LIVE (attached-controller) path -- the propose_patch/remove_chunk
// siblings of Test 5's insert_chunk rejection-diagnostics block above.
// Same rationale: the LIVE path IS the GUI, so the actionable clause must
// be attached there too, not only on the (easier-to-test) headless path.
//////////////////////////////////////////////////////////////////////
static void TestActionableRejectionDiagnosticsLive()
{
	std::cout << "Test 5b: actionable propose_patch/remove_chunk rejection diagnostics (LIVE path)..." << std::endl;

	const char* tmp = "agentlive_actionable.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "actionable-diagnostics fixture loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->AttachController( &c );

		// (g) propose_patch, LIVE path: retarget lum's exitance to a
		// near-miss typo of the real painter "white" -> unresolved_reference.
		{
			Agent::AgentSetPatch p;
			p.target = "lum";
			p.kind   = "lambertian_luminaire_material";
			p.param  = "exitance";
			p.value  = "whitte";
			Agent::AgentPatchResult pr = sess->ProposePatch( p );
			Check( !pr.applied && pr.status == "rejected",
			       "(g) live propose_patch with a dangling painter retarget is rejected" );
			Check( pr.issues.size() == 1, "(g) live rejection carries exactly one structured issue" );
			if( pr.issues.size() == 1 ) {
				Check( pr.issues[0].param == "exitance",
				       "(g) live rejection issue names the offending param" );
				Check( pr.issues[0].value == "whitte",
				       "(g) live rejection issue carries the bad value" );
				Check( pr.issues[0].reason == "unresolved_reference",
				       "(g) live rejection issue carries the unresolved_reference reason" );
				bool suggested = false;
				for( const std::string& s : pr.issues[0].suggestions )
					if( s == "white" ) suggested = true;
				Check( suggested, "(g) live rejection suggests the near-miss painter 'white' for 'whitte'" );
			}
			Check( pr.message.find( "ACTIONABLE" ) != std::string::npos,
			       "(g) live rejection message carries the ACTIONABLE clause" );
			std::cout << "  (g) message: " << pr.message << std::endl;
		}

		// (i) remove_chunk, LIVE path: "white" is still bound by lum's
		// `exitance` param -> still_referenced, naming "lum".
		{
			Agent::AgentChunkResult cr = sess->RemoveChunk( "white" );
			Check( !cr.applied && cr.status == "rejected",
			       "(i) live remove of a still-referenced painter is rejected" );
			Check( cr.issues.size() == 1, "(i) live rejection carries exactly one structured issue" );
			if( cr.issues.size() == 1 ) {
				Check( cr.issues[0].value == "white",
				       "(i) live rejection issue value is the remove target's own name" );
				Check( cr.issues[0].reason == "still_referenced",
				       "(i) live rejection issue carries the still_referenced reason" );
				bool sawIt = false;
				for( const std::string& s : cr.issues[0].suggestions ) if( s == "lum" ) sawIt = true;
				Check( sawIt, "(i) live rejection suggestions NAME the blocking referrer 'lum'" );
			}
			Check( cr.message.find( "ACTIONABLE" ) != std::string::npos && cr.message.find( "lum" ) != std::string::npos,
			       "(i) live rejection message NAMES the blocking referrer, not just a hedge" );
			std::cout << "  (i) message: " << cr.message << std::endl;
		}

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 5c: insert_chunks (the BATCH form of insert_chunk) on the LIVE
// (attached-controller) path.  Mirrors Test 5's WrapJob + AttachController +
// TestController harness -- the recurring failure mode this codebase has
// hit before is wiring a mutating verb to the headless path only and
// missing the LIVE controller path (the GUI), so this proves
// AgentSession::InsertChunks reaches ApplyAgentInsertChunk (via its
// per-element InsertChunk delegate) for real, with a real render thread
// running underneath it, not just headlessly.
//
//   (a) ALL-APPLY: a 4-chunk batch (painter -> material -> geometry ->
//       object, in dependency order) applies IN FULL through the attached
//       controller: every result applied, the live managers grew, the
//       controller cancel-and-parked + re-kicked a render pass, and the
//       editor is DIRTY afterward -- the exact LIVE-commit contract Test 5
//       proves for a single insert_chunk, now proven for a whole batch in
//       one call.
//   (c) BEST-EFFORT: a 2-chunk batch where the FIRST element is a dangling
//       painter reference (rejected, with the SAME actionable `issues` Test
//       5 proves for a single live insert_chunk rejection) and the SECOND
//       element is independent and unrelated -- it still applies, proving
//       a live rejection does not abort the batch.
//////////////////////////////////////////////////////////////////////
static void TestInsertChunksLive()
{
	std::cout << "Test 5c: insert_chunks (BATCH insert_chunk) on the LIVE controller path..." << std::endl;

	const char* tmp = "agentlive_insert_chunks.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "insert_chunks live fixture loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		Check( sess != nullptr, "AgentSession wraps the live Job" );
		sess->AttachController( &c );
		Check( sess->HasController(), "session attached to the running controller" );

		//------------------------------------------------------------------
		// (a) ALL-APPLY: painter -> material (references it) -> geometry ->
		// object (binds geometry + material), all 4 in ONE InsertChunks call.
		//------------------------------------------------------------------
		{
			Check( !c.HasUnsavedChanges(), "scene is CLEAN before the live insert_chunks batch" );
			// Put a pass PROVABLY in flight before the batch runs.  The
			// cancel-and-park assertion below is about what the batch does to
			// an in-flight pass, and the render loop is idle between passes --
			// without this gate the batch could simply land in the idle gap,
			// bump no counter, and fail an assertion that describes correct
			// behaviour.  That was this test's flake (it inherited Test 4's
			// assertion without inheriting Test 4's mid-pass setup).
			c.ArmHeldPass();
			c.ForTest_KickRender();
			Check( c.WaitForHeldPass( 5000 ),
			       "(a) a render pass is provably in flight before the live batch" );
			// Sample the counters only ONCE the pass is holding, so the kick's
			// own (conditional) cancel can never be miscredited to the batch.
			const unsigned int cancelsBefore = c.ForTest_GetCancelCount();
			const unsigned int completedBefore = c.CompletedCount();

			std::vector<std::string> chunks;
			chunks.push_back( "uniformcolor_painter\n{\nname pnt_ic\ncolor 0.2 0.4 0.6\n}\n" );
			chunks.push_back( "lambertian_material\n{\nname mat_ic\nreflectance pnt_ic\n}\n" );
			chunks.push_back( "sphere_geometry\n{\nname sph_ic\nradius 0.4\n}\n" );
			chunks.push_back( "standard_object\n{\nname obj_ic\ngeometry sph_ic\nmaterial mat_ic\nposition 2 0 0\n}\n" );

			const std::vector<Agent::AgentChunkResult> results = sess->InsertChunks( chunks );
			Check( results.size() == 4, "(a) live insert_chunks returns one result per input chunk" );
			if( results.size() == 4 ) {
				Check( results[0].applied && results[0].status == "applied" &&
				       results[0].kind == "uniformcolor_painter" && results[0].name == "pnt_ic",
				       "(a) live batch element 0 (painter) applied" );
				Check( results[1].applied && results[1].status == "applied" &&
				       results[1].kind == "lambertian_material" && results[1].name == "mat_ic",
				       "(a) live batch element 1 (material referencing element 0) applied" );
				Check( results[1].issues.empty(),
				       "(a) the intra-batch reference resolved cleanly -- no unresolved_reference issue "
				       "on the material, because the painter already landed earlier in this SAME batch" );
				Check( results[2].applied && results[2].kind == "sphere_geometry" && results[2].name == "sph_ic",
				       "(a) live batch element 2 (geometry) applied" );
				Check( results[3].applied && results[3].kind == "standard_object" && results[3].name == "obj_ic",
				       "(a) live batch element 3 (object binding elements 1+2) applied" );
			}

			// The LIVE managers actually grew (this reached ApplyAgentInsertChunk,
			// not just the retained Document).
			Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "mat_ic" ) != nullptr,
			       "(a) the live managers carry the batch-inserted material" );
			Check( pJob->GetObjects() && pJob->GetObjects()->GetItem( "obj_ic" ) != nullptr,
			       "(a) the live managers carry the batch-inserted object" );

			// The SAME park/kick/dirty contract Test 5 proves for one insert_chunk.
			Check( c.ForTest_GetCancelCount() > cancelsBefore,
			       "(a) the live batch cancel-and-PARKED the in-flight pass" );
			Check( c.HasUnsavedChanges(), "(a) the live batch marked the editor DIRTY (Save enables)" );
			// Count COMPLETED passes, not all passes: the held predecessor the
			// batch just cancelled returns early and is deliberately not
			// counted, so this stays a real "a fresh pass ran" assertion.
			Check( c.WaitForCompleted( completedBefore + 1, 5000 ),
			       "(a) the live batch KICKED a fresh viewport pass" );
			Check( c.IsRunning(), "(a) render thread alive after the live insert_chunks batch" );
		}

		//------------------------------------------------------------------
		// (c) BEST-EFFORT: element 0 is a dangling painter reference (the
		// exact rejection Test 5 exercises for a single live insert_chunk);
		// element 1 is independent and unrelated.  The rejection must NOT
		// abort the batch -- element 1 still applies, and element 0's
		// `issues` survive the LIVE path exactly like Test 5's single-insert
		// case.
		//------------------------------------------------------------------
		{
			std::vector<std::string> chunks;
			chunks.push_back( "lambertian_material\n{\nname m_ic_dangling\nreflectance whitte\n}\n" );
			chunks.push_back( "sphere_geometry\n{\nname sph_ic2\nradius 0.25\n}\n" );

			const std::vector<Agent::AgentChunkResult> results = sess->InsertChunks( chunks );
			Check( results.size() == 2, "(c) live best-effort insert_chunks returns one result per input" );
			if( results.size() == 2 ) {
				Check( !results[0].applied && results[0].status == "rejected",
				       "(c) live batch element 0 (dangling reference) is rejected" );
				Check( results[0].issues.size() == 1,
				       "(c) live rejection carries exactly one structured issue, same as a single insert_chunk" );
				if( results[0].issues.size() == 1 ) {
					Check( results[0].issues[0].param == "reflectance",
					       "(c) live rejection issue names the offending param" );
					Check( results[0].issues[0].reason == "unresolved_reference",
					       "(c) live rejection issue carries the unresolved_reference reason" );
					bool suggested = false;
					for( const std::string& s : results[0].issues[0].suggestions )
						if( s == "white" ) suggested = true;
					Check( suggested, "(c) live rejection suggests the near-miss painter 'white' for 'whitte'" );
				}
				Check( results[0].message.find( "ACTIONABLE" ) != std::string::npos,
				       "(c) live rejection message carries the ACTIONABLE clause" );

				// The batch did NOT stop: element 1 still applied.
				Check( results[1].applied && results[1].status == "applied" &&
				       results[1].kind == "sphere_geometry" && results[1].name == "sph_ic2",
				       "(c) BEST-EFFORT: the rejected element 0 did not stop element 1 from applying" );
			}
			Check( pJob->GetObjects() == nullptr || pJob->GetObjects()->GetItem( "m_ic_dangling" ) == nullptr,
			       "(c) the rejected element never reached the live managers" );
			// sph_ic2 is a geometry, not directly queryable by name through
			// GetObjects() -- the applied==true result above (round-tripped
			// through the SAME ApplyAgentInsertChunk path as (a)) is the
			// live-reach proof for this element.
		}

		c.Stop();
		Check( !c.IsRunning(), "controller stops + joins cleanly after the live insert_chunks tests" );
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
// Test A2 (Model-B F5 polish: transform-commit code-3 parity): SetProperty's
// Object branch's Apply() for a position/orientation/scale edit is a direct LIVE
// mutate (SceneEditor::ApplyObjectOpForward) that always succeeds -- the CST
// commit happens SEPARATELY afterward via CommitPendingCstObjectTransforms, which
// routes through Job::ApplyCstObjectMatrixEdit. Pre-A2, SetProperty ignored that
// commit's return value entirely, so a diagnosed code-3 (or any other commit
// failure) on the FOLLOW-THROUGH still reported SetProperty() == true to the
// caller, even though the Document diverged from the live scene. This test
// drives a REAL code-3 on that specific seam (CodeThreeTransformJob) and asserts:
//   - SetProperty(Object, "position", ...) returns FALSE (the fix)
//   - the render kick still fires (the live scene DID change -- Apply succeeded
//     and CommitPendingCstObjectTransforms's D2 rebuild really replaced the
//     Scene+managers; the viewport must not show stale pixels)
//   - a control edit with the force OFF returns TRUE and still re-renders,
//     proving we didn't break the clean path.
//////////////////////////////////////////////////////////////////////
static void TestTransformCommitCodeThree()
{
	std::cout << "Test A2: transform-commit code-3 parity (SetProperty honest failure)..." << std::endl;

	const char* tmp = "agentlive_xform_code3.RISEscene";
	// Variant scene (as in TestCodeThreeRerender): any CST document edit on `obj` forces
	// a real D2 full re-derive (base ApplyCstObjectMatrixEdit returns 2), which
	// CodeThreeTransformJob rewrites to 3.
	{ std::ofstream o( tmp ); o << kVariantScene; }
	CodeThreeTransformJob* pJob = new CodeThreeTransformJob();
	Check( pJob->LoadAsciiSceneViaCst( tmp ), "variant scene loads into CodeThreeTransformJob via the CST path" );
	if( !pJob ) { std::remove( tmp ); return; }

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		c.SetSelection( SceneEditController::Category::Object, String( "obj" ) );

		//------------------------------------------------------------------
		// THE FIX: a position edit's Apply() succeeds (direct live mutate), but the
		// follow-through CST commit is forced to code 3 -- SetProperty must report FALSE.
		//------------------------------------------------------------------
		const unsigned int before = c.ForTest_GetRenderCount();
		const bool setOk = c.SetPropertyForCategory(
			SceneEditController::Category::Object, String( "position" ), String( "2 3 4" ) );
		Check( !setOk, "code-3 transform-commit: SetProperty returns FALSE (A2 fix)" );
		// The render kick is unconditional in the Object branch (stored before the
		// commits run), so it must still fire regardless of the commit's outcome.
		Check( c.ForTest_WaitForRenders( before + 1, 3000 ),
		       "code-3 transform-commit still RE-RENDERS the viewport (kick unaffected by commit outcome)" );

		//------------------------------------------------------------------
		// CONTROL: force OFF -- a clean transform commit (code 2, a genuine D2) must
		// report SUCCESS and still re-render, proving the clean path is unbroken.
		//------------------------------------------------------------------
		pJob->SetForceCodeThree( false );
		const unsigned int before2 = c.ForTest_GetRenderCount();
		const bool setOk2 = c.SetPropertyForCategory(
			SceneEditController::Category::Object, String( "position" ), String( "5 6 7" ) );
		Check( setOk2, "control: force-off transform commit reports SUCCESS" );
		Check( c.ForTest_WaitForRenders( before2 + 1, 3000 ),
		       "control: clean transform commit re-renders (shared kick path intact)" );

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
		// Round-2 pin: assert the PAIR IDENTITY too -- the one mark is
		// (Material, "lum"), not merely "some entity somewhere".
		{
			const std::vector<DirtyEntity> ents = c.Editor().Dirty().EntitySnapshot();
			Check( ents.size() == 1
			    && ents[0].first == EntityCategory::Material
			    && ents[0].second == "lum",
			       "the per-entity mark is (Material, \"lum\") -- category + name pinned" );
		}

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

//////////////////////////////////////////////////////////////////////
// Test 9 (Model-B F5 polish A1 round 2): a MID-TRANSACTION agent commit is
// REFUSED -- end-to-end through the REAL transaction API (BeginTransaction /
// RollbackTransaction are public on SceneEditController, so TestController
// drives the exact production sequence).
//
// Why: an agent commit's Document mutation has NO EditHistory record, so
// RollbackTransaction's inverse-edit Undo loop can never revert it, and a
// FULL rollback restores the pre-transaction dirty baseline -- a KNOWN-kind
// mark (material "lum" -> EntityCategory::Material) lands in mEntityDirty,
// which RestoreState plain-copies, so the mark would be WIPED while the
// mutated Document survives: HasUnsavedChanges()==false + a diverged scene
// == silent data loss on close-without-prompt.  (The round-1 OR-merge only
// shields the uncategorized CST-head BOOLEAN channel.)  The fix refuses the
// commit up front, before any mutation or park.
//
// RED-PROVE (guard removed -- verified by deleting the mTxnOpen block in
// ApplyAgentParamEdit): the commit APPLIES mid-transaction, so
//   - "mid-txn agent commit is refused"            FAILS (applied==true),
//   - "refusal is a code-0 reject"                 FAILS (rawCode==1),
//   - "refusal left the head revision unchanged"   FAILS (revision bumped),
//   - "refusal left the derived scene unchanged"   FAILS (LumR -> 9.5),
//   - after RollbackTransaction: "rollback left no clean-but-diverged state"
//     FAILS -- HasUnsavedChanges()==false while LumR stays 9.5 (the edit
//     survived in the Document with its dirty mark wiped) == the data loss.
//
// Round 3 adds the WIRE leg: the refusal is TRANSIENT (retry after the
// gesture completes) but a JSON-RPC client only sees status="rejected" --
// indistinguishable from a PERMANENT reject (unknown entity / empty field)
// without parsing free-text English.  The additive `retriable` bool is the
// machine-readable discriminator: this leg drives raw JSON-RPC
// propose_patch lines through AgentRpcDispatcher::HandleLine (Test 8's
// wiring: WrapJob + AttachController on the same controller) and asserts
// the mid-transaction response carries "retriable":true while a
// post-EndTransaction unknown-entity reject carries "retriable":false.
// RED-PROVED by hard-coding retriable=false at the mTxnOpen refusal site
// (see the commit message for the recorded failing checks).
//////////////////////////////////////////////////////////////////////
static void TestMidTransactionAgentCommitRefused()
{
	std::cout << "Test 9: mid-transaction agent commit refused (mTxnOpen guard)..." << std::endl;

	const char* tmp = "agentlive_midtxn.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const RISE::Cst::CstHeadVersion head0 = pJob->GetCstHeadVersion();
		const double lum0 = LumR( *pJob );
		Check( lum0 > 4.5 && lum0 < 5.5, "baseline lum scale is ~5.0" );
		Check( !c.HasUnsavedChanges(), "scene is CLEAN before the transaction" );

		// Open a REAL transaction (the gesture bracket intended for the
		// GUI -- no production caller yet).
		Check( c.BeginTransaction(), "transaction opens" );
		Check( c.IsTransactionOpen(), "transaction reports open" );

		//------------------------------------------------------------------
		// The mid-transaction agent commit: a KNOWN kind ("_material" ->
		// EntityCategory::Material), exactly the shape whose dirty mark the
		// rollback restore would wipe.  Must be refused, NON-MUTATING.
		//------------------------------------------------------------------
		const SceneEditController::AgentCommitResult r =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "9.5" ),
				/*baseVersionOrNull*/ nullptr );
		std::cout << "    rawCode=" << r.rawCode << " status=" << r.status.c_str()
		          << " message=\"" << r.message.c_str() << "\"" << std::endl;
		Check( !r.applied, "mid-txn agent commit is refused (applied=false)" );
		Check( r.rawCode == 0, "refusal is a code-0 reject" );
		Check( r.status == String( "rejected" ), "refusal status is \"rejected\" (honest -- NOT a faked conflict)" );
		Check( !r.conflict, "refusal is not reported as a conflict (the head did not move)" );
		Check( std::string( r.message.c_str() ).find( "transaction" ) != std::string::npos,
		       "refusal message tells the agent a transaction is in progress (actionable diagnostic)" );
		Check( r.headVersion == head0, "refusal result carries the UNCHANGED head" );
		Check( pJob->GetCstHeadVersion() == head0, "refusal left the head revision unchanged (Document untouched)" );
		Check( LumR( *pJob ) == lum0, "refusal left the derived scene unchanged" );
		Check( !c.HasUnsavedChanges(), "refusal marked nothing dirty" );

		//------------------------------------------------------------------
		// Roll the (empty) transaction back -- the data-loss window: with the
		// guard removed the commit landed above, its Material mark is wiped
		// by the baseline restore here, and the scene silently diverges.
		//------------------------------------------------------------------
		Check( c.RollbackTransaction(), "rollback completes (full revert)" );
		Check( !c.IsTransactionOpen(), "transaction reports closed after rollback" );
		Check( !c.HasUnsavedChanges(), "clean after rollback (nothing to save)" );
		Check( pJob->GetCstHeadVersion() == head0, "head revision still unchanged after rollback" );
		Check( LumR( *pJob ) == lum0, "the scene does NOT contain the agent's attempted edit after rollback" );
		Check( !( !c.HasUnsavedChanges() && LumR( *pJob ) != lum0 ),
		       "rollback left no clean-but-diverged state (the data-loss condition)" );

		//------------------------------------------------------------------
		// Control: with the transaction CLOSED, the SAME commit applies
		// cleanly -- the refusal is transaction-scoped, not a broken path.
		//------------------------------------------------------------------
		const SceneEditController::AgentCommitResult retry =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "9.5" ),
				/*baseVersionOrNull*/ nullptr );
		Check( retry.applied, "the same commit applies once the transaction is closed" );
		Check( LumR( *pJob ) > 9.0 && LumR( *pJob ) < 10.0, "post-transaction retry reached the scene (~9.5)" );
		Check( c.HasUnsavedChanges(), "post-transaction apply marks the editor dirty" );

		// EndTransaction (commit) closes the window too: refuse inside,
		// apply after -- proves BOTH close paths re-open the agent surface.
		c.Editor().ClearDirtyState();
		Check( c.BeginTransaction(), "second transaction opens" );
		const SceneEditController::AgentCommitResult r2 =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "2.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( !r2.applied && r2.status == String( "rejected" ), "refused inside the second transaction too" );
		Check( r2.retriable, "the C++ refusal is flagged retriable (transient, not permanent)" );

		//------------------------------------------------------------------
		// WIRE leg (round 3): the retriable discriminator must reach the
		// JSON-RPC client.  Mirror Test 8's wiring (WrapJob + AttachController
		// on the SAME running controller) and drive raw JSON-RPC lines:
		//   - mid-transaction propose_patch -> status "rejected" AND
		//     "retriable":true (the transient reject an agent may resubmit
		//     verbatim after the gesture completes);
		//   - post-EndTransaction propose_patch on an UNKNOWN entity ->
		//     status "rejected" AND "retriable":false (a permanent reject a
		//     retry loop must NOT spin on).
		//------------------------------------------------------------------
		std::unique_ptr<Agent::AgentSession> wireSess = Agent::AgentSession::WrapJob( pJob );
		Check( wireSess != nullptr, "AgentSession wraps the live Job for the wire leg" );
		if( wireSess ) wireSess->AttachController( &c );
		Agent::AgentRpcDispatcher wireDisp( std::move( wireSess ) );
		const std::string txnResp = wireDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"2.0\"}}" );
		std::cout << "    mid-txn wire resp=" << txnResp << std::endl;
		Agent::JsonValue txnResult;
		Check( JsonResultObj( txnResp, txnResult ), "mid-txn wire propose_patch returns a JSON-RPC result object" );
		const Agent::JsonValue* wStatus = txnResult.find( "status" );
		Check( wStatus && wStatus->isString() && wStatus->asString() == "rejected",
		       "mid-txn wire response status is \"rejected\"" );
		const Agent::JsonValue* wRetri = txnResult.find( "retriable" );
		Check( wRetri && wRetri->isBool() && wRetri->asBool(),
		       "mid-txn wire response carries \"retriable\":true (transient reject)" );
		const Agent::JsonValue* wApplied = txnResult.find( "applied" );
		Check( wApplied && wApplied->isBool() && !wApplied->asBool(),
		       "mid-txn wire propose_patch did not apply" );

		Check( c.EndTransaction(), "second transaction commits (EndTransaction)" );
		const SceneEditController::AgentCommitResult r3 =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "2.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( r3.applied, "the agent surface re-opens after EndTransaction" );
		Check( !r3.retriable, "a clean apply carries retriable=false (the flag is reject-only)" );

		// Wire leg, permanent reject: unknown entity, transaction CLOSED.
		const std::string permResp = wireDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"no_such_entity\",\"param\":\"scale\",\"value\":\"2.0\"}}" );
		std::cout << "    permanent-reject wire resp=" << permResp << std::endl;
		Agent::JsonValue permResult;
		Check( JsonResultObj( permResp, permResult ), "permanent-reject wire propose_patch returns a JSON-RPC result object" );
		const Agent::JsonValue* pStatus = permResult.find( "status" );
		Check( pStatus && pStatus->isString() && pStatus->asString() == "rejected",
		       "unknown-entity wire response status is \"rejected\"" );
		const Agent::JsonValue* pRetri = permResult.find( "retriable" );
		Check( pRetri && pRetri->isBool() && !pRetri->asBool(),
		       "unknown-entity wire response carries \"retriable\":false (permanent reject)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 10: preview-render safety -- SceneEditController::RunPreviewRenderParked
// (the seam AgentSession::Render's film-dims / camera-pose overrides use in
// LIVE mode so they cannot race DoOneRenderPass's own Film/camera swap).
//
//   (a) During a long in-flight render pass, RunPreviewRenderParked must
//       CANCEL-AND-PARK it (cancel counter advances) before running the
//       callback, and the callback must actually run (observable side
//       effect) -- proving the "safe against a live render thread" contract,
//       mirroring TestRenderParkedDuringEdit's park-observability recipe.
//   (b) Inside an open editor transaction, RunPreviewRenderParked must
//       REFUSE (return false) and NOT invoke the callback at all -- same
//       mTxnOpen rule as ApplyAgentParamEdit, RED-PROVEN by checking the
//       side-effect flag stays false.
//   (c) Outside a transaction (control), the SAME call returns true and DOES
//       invoke the callback -- proves (b) is transaction-scoped, not a
//       broken seam.
//////////////////////////////////////////////////////////////////////
static void TestPreviewRenderParked()
{
	std::cout << "Test 10: preview-render park seam (RunPreviewRenderParked)..." << std::endl;

	const char* tmp = "agentlive_previewpark.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		// (a) Hold a pass in flight so the callback provably lands mid-pass --
		// see ArmHeldPass for why "sleep 40 ms and hope" is not good enough.
		TestController c( *pJob, /*simulatedRenderMs*/300 );
		c.Start();
		c.ArmHeldPass();
		c.ForTest_KickRender();
		Check( c.WaitForHeldPass( 5000 ),
		       "a render pass is provably in flight before RunPreviewRenderParked" );
		const unsigned int cancelsBefore = c.ForTest_GetCancelCount();

		std::atomic<bool> ran{ false };
		const bool ok = c.RunPreviewRenderParked( [&]{ ran.store( true ); } );
		Check( ok, "RunPreviewRenderParked returns true outside a transaction" );
		Check( ran.load(), "the callback actually ran" );

		const unsigned int cancelsAfter = c.ForTest_GetCancelCount();
		std::cout << "    cancelsBefore=" << cancelsBefore
		          << " cancelsAfter=" << cancelsAfter << std::endl;
		Check( cancelsAfter > cancelsBefore,
		       "RunPreviewRenderParked tripped the render cancel (parked the in-flight pass)" );

		// The render thread must still be ALIVE after the park (no deadlock /
		// lost wakeup) -- NOT necessarily mid a FRESH pass: RunPreviewRenderParked
		// makes no scene edit, so (outside a gesture's refine-mode watchdog) the
		// loop legitimately stays parked on its idle wait until the next real
		// edit/gesture, exactly as documented ("no kick needed").  Asserting a
		// forced fresh pass here would be asserting the WRONG contract.
		Check( c.IsRunning(), "render thread still running after the preview-render park" );

		c.Stop();
	}

	{
		// (b) Mid-transaction refusal: the callback must NOT run.
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		Check( c.BeginTransaction(), "transaction opens" );
		Check( c.IsTransactionOpen(), "transaction reports open" );

		std::atomic<bool> ranMidTxn{ false };
		const bool okMidTxn = c.RunPreviewRenderParked( [&]{ ranMidTxn.store( true ); } );
		Check( !okMidTxn, "RunPreviewRenderParked REFUSES inside an open transaction" );
		Check( !ranMidTxn.load(), "the callback did NOT run when refused (RED-PROVEN: a broken guard would flip this)" );

		Check( c.RollbackTransaction(), "rollback completes" );
		Check( !c.IsTransactionOpen(), "transaction reports closed after rollback" );

		// (c) Control: same call, transaction now closed -> succeeds and runs.
		std::atomic<bool> ranAfter{ false };
		const bool okAfter = c.RunPreviewRenderParked( [&]{ ranAfter.store( true ); } );
		Check( okAfter, "the same call succeeds once the transaction is closed" );
		Check( ranAfter.load(), "the callback ran once unblocked (proves the refusal above was transaction-scoped)" );

		c.Stop();
	}

	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 11: shared-undo U1 headline -- an agent PARAM edit is a first-class
// undo/redo citizen.  RED-PROVE: assert the pre-fix behaviour (an agent edit
// leaves NO history record) would have failed this test -- i.e. BEFORE the
// fix, UndoDepth does not grow on an agent commit and a subsequent Undo has
// nothing of the agent's to revert.  Also proves the head-revision BUMPS on
// both Undo and Redo (agent clients' conflict gates must see the movement)
// and that the viewport is kicked (a fresh render fires) on both.
//////////////////////////////////////////////////////////////////////
static void TestAgentEditUndoRedo()
{
	std::cout << "Test 11: agent edit undo/redo (shared-undo U1 headline)..." << std::endl;

	const char* tmp = "agentlive_undoredo.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const unsigned int undoBefore = c.Editor().History().UndoDepth();

		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "7.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied.applied && applied.rawCode == 1, "agent commit applies cleanly (code 1, incremental)" );

		// RED-PROVE the headline: an agent edit MUST push a history record.
		Check( c.Editor().History().UndoDepth() == undoBefore + 1,
		       "RED-PROVE: the agent commit pushed exactly ONE history record "
		       "(at the pre-fix parent this stays UNCHANGED -- no record pushed)" );
		Check( LumR( *pJob ) > 6.0 && LumR( *pJob ) < 8.0, "the scale-7.0 edit reached the derived scene" );

		const RISE::Cst::CstHeadVersion afterApply = pJob->GetCstHeadVersion();
		const unsigned int rcBeforeUndo = c.ForTest_GetRenderCount();

		c.Undo();
		Check( c.ForTest_WaitForRenders( rcBeforeUndo + 1, 3000 ), "Undo kicked a fresh render pass" );
		Check( c.Editor().History().UndoDepth() == undoBefore, "Undo popped the agent record off the undo stack" );
		Check( c.Editor().History().RedoDepth() == 1, "the reverted agent edit landed on the redo stack" );

		// The param is BACK to the prior value in BOTH the derived scene AND
		// the serialized Document.
		Check( LumR( *pJob ) > 4.5 && LumR( *pJob ) < 5.5, "Undo restored the derived scene to scale~5.0 (the ORIGINAL kBaseScene value)" );
		{
			const std::string doc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			Check( doc.find( "scale 5" ) != std::string::npos || doc.find( "scale 5.0" ) != std::string::npos,
			       "Undo restored the Document's `scale` param text to the original 5.0" );
		}

		// Revision semantics: Undo MUTATES the Document (a revert-apply through
		// the same ApplyCstParamEdit machinery), so the head revision MUST BUMP
		// (monotonic, never reused) so a conflict-gated agent client sees the
		// movement.
		const RISE::Cst::CstHeadVersion afterUndo = pJob->GetCstHeadVersion();
		Check( afterUndo != afterApply, "Undo bumped the head-version (revert is itself a Document mutation)" );
		Check( afterUndo.revision > afterApply.revision, "the revision strictly ADVANCED (monotonic) on Undo" );

		// Redo re-applies.
		const unsigned int rcBeforeRedo = c.ForTest_GetRenderCount();
		c.Redo();
		Check( c.ForTest_WaitForRenders( rcBeforeRedo + 1, 3000 ), "Redo kicked a fresh render pass" );
		Check( c.Editor().History().UndoDepth() == undoBefore + 1, "Redo moved the record back to the undo stack" );
		Check( c.Editor().History().RedoDepth() == 0, "redo stack drained after the single Redo" );
		Check( LumR( *pJob ) > 6.0 && LumR( *pJob ) < 8.0, "Redo re-applied the scale-7.0 edit to the derived scene" );

		const RISE::Cst::CstHeadVersion afterRedo = pJob->GetCstHeadVersion();
		Check( afterRedo != afterUndo, "Redo bumped the head-version again" );
		Check( afterRedo.revision > afterUndo.revision, "the revision strictly ADVANCED (monotonic) on Redo" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

// A scene whose emissive luminaire material (`lum2`) is DECLARED but NOT
// bound to any live object (`obj` binds the ordinary `plain` material
// instead) -- isolates the SceneEditor-level bump this fix adds from the
// PRE-EXISTING, independent closure-driven bump in
// RISE::Cst::DeriveToJobIncremental (Cst.cpp's "Slice 3" closure-gated
// invariant pass, `if( emitter ) pJob.BumpLightTopologyGeneration();`),
// which ALSO bumps the SAME counter whenever an incremental re-derive
// closure re-points a live OBJECT bound to an emissive material.  When the
// edited material has NO bound object in its closure (this scene), that
// pre-existing mechanism has nothing to iterate over and CANNOT fire --
// so a generation bump here can ONLY come from the fix under test
// (SceneEditor::BumpSceneLightGenerationForAgentParamEdit, wired into
// SceneEditController::ApplyAgentParamEdit's forward commit and
// SceneEditor::ApplyRevertMutation / ApplyForwardMutation's SetAgentCstParam
// arms).  Using an object-bound emissive material here would give a FALSE
// PASS pre-fix (the Cst.cpp mechanism alone would satisfy the assertion).
static const char* kUnboundEmissiveScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"lambertian_material\n{\nname plain\nreflectance white\n}\n"
	"lambertian_luminaire_material\n{\nname lum2\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial plain\n}\n";

//////////////////////////////////////////////////////////////////////
// Test 11b (P2 fix): an agent-originated edit to an EMISSIVE material's
// emission-affecting param (`lum2`'s `scale`, a lambertian_luminaire_material
// with NO bound object -- see kUnboundEmissiveScene's doc for why) via a
// CODE-1 (incremental) path must bump the scene's light-topology generation
// -- else a reused RayCaster's LightSampler alias table keeps the STALE
// selection weight from before the edit (light SELECTION biased toward the
// old emission footprint; per-sample Le is still read live so the estimator
// itself stays unbiased, but convergence is worse).  Undo and Redo of that
// same edit must ALSO bump (SceneEditor::ApplyRevertMutation /
// ApplyForwardMutation's SetAgentCstParam arms) -- the same alias-table
// staleness applies to a reverted/redone emissive edit.
//
// RED-PROVE: at the pre-fix parent, ApplyAgentParamEdit / the Undo+Redo arms
// never called BumpSceneLightGenerationIfMaterialEmits for the agent path,
// and (per kUnboundEmissiveScene's doc) the independent Cst.cpp closure-gated
// bump cannot fire here either (no object in the edit closure) -- so
// GetLightTopologyGeneration() stays UNCHANGED across all three steps.
//////////////////////////////////////////////////////////////////////
static void TestAgentEmissiveEditBumpsLightGeneration()
{
	std::cout << "Test 11b: agent emissive-material edit bumps light-topology generation (P2 fix)..." << std::endl;

	const char* tmp = "agentlive_lightgen.RISEscene";
	Job* pJob = LoadScene( kUnboundEmissiveScene, tmp );
	Check( pJob != nullptr, "unbound-emissive scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		//------------------------------------------------------------------
		// (a) forward agent commit on `lum2` (emissive, UNBOUND) -- code-1 incremental.
		//------------------------------------------------------------------
		const unsigned int genBeforeApply = pJob->GetLightTopologyGeneration();
		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit(
				String( "lum2" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "8.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied.applied && applied.rawCode == 1, "agent commit on the emissive material applies cleanly (code 1)" );
		const unsigned int genAfterApply = pJob->GetLightTopologyGeneration();
		Check( genAfterApply != genBeforeApply,
		       "RED-PROVE: a code-1 agent edit on an EMISSIVE (unbound) material bumps the light-topology generation "
		       "(at the pre-fix parent this stays UNCHANGED)" );

		//------------------------------------------------------------------
		// (b) Undo the same edit -- must ALSO bump.
		//------------------------------------------------------------------
		const unsigned int genBeforeUndo = pJob->GetLightTopologyGeneration();
		c.Undo();
		const unsigned int genAfterUndo = pJob->GetLightTopologyGeneration();
		Check( genAfterUndo != genBeforeUndo,
		       "RED-PROVE: Undo of the emissive-material edit ALSO bumps the light-topology generation" );

		//------------------------------------------------------------------
		// Redo -- must ALSO bump.
		//------------------------------------------------------------------
		const unsigned int genBeforeRedo = pJob->GetLightTopologyGeneration();
		c.Redo();
		const unsigned int genAfterRedo = pJob->GetLightTopologyGeneration();
		Check( genAfterRedo != genBeforeRedo,
		       "RED-PROVE: Redo of the emissive-material edit ALSO bumps the light-topology generation" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 11c (P2 fix, negative control): a NON-material agent edit with a
// KNOWN kind (`standard_object`.`casts_shadows` on `obj`, a shadow-flag bool
// -- not a spatial/material op) must NOT bump the light-topology generation.
// `obj` is bound to the emissive `lum` material, so this also proves the
// resolution keys off the EDITED entity's own kind, not merely "some emissive
// material exists somewhere in the scene."
//////////////////////////////////////////////////////////////////////
// A scene with a SECOND, NON-emissive object (`obj2`, bound to a plain
// `lambertian_material`) alongside the base `lum`-lit `obj`.  Test 11c edits
// `obj2` specifically -- NOT `obj` -- so the assertion isolates "a known
// non-material kind on a NON-emissive target must not bump" from an
// unrelated pre-existing mechanism: Job's incremental CST re-derive of a
// standard_object closure removes-then-re-adds the live object
// (RISE::Cst::DeriveToJobIncremental), and Job::RemoveObject already bumps
// the light-topology generation whenever the removed object's CURRENT
// material is emissive (see Job.cpp's `wasEmissive` check in RemoveObject,
// P2a) -- regardless of what the edit itself touched.  Editing `obj`
// (bound to the emissive `lum`) would trip THAT pre-existing mechanism and
// give a false failure unrelated to this fix; `obj2` (bound to the
// non-emissive `plain`) does not.
static const char* kMixedEmissiveScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"uniformcolor_painter\n{\nname grey\ncolor 0.5 0.5 0.5\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"lambertian_material\n{\nname plain\nreflectance white\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"sphere_geometry\n{\nname s2\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n"
	"standard_object\n{\nname obj2\ngeometry s2\nmaterial plain\nposition 3 0 0\n}\n";

static void TestAgentNonMaterialEditDoesNotBumpLightGeneration()
{
	std::cout << "Test 11c: agent non-material edit (known kind, non-emissive target) does NOT bump light-topology generation..." << std::endl;

	const char* tmp = "agentlive_lightgen_nonmat.RISEscene";
	Job* pJob = LoadScene( kMixedEmissiveScene, tmp );
	Check( pJob != nullptr, "mixed-emissive scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const unsigned int genBefore = pJob->GetLightTopologyGeneration();
		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit(
				String( "obj2" ), String( "standard_object" ),
				String( "casts_shadows" ), String( "false" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied.applied, "agent commit on the KNOWN non-material kind (non-emissive target) applies cleanly" );
		const unsigned int genAfter = pJob->GetLightTopologyGeneration();
		Check( genAfter == genBefore,
		       "a KNOWN non-material-kind agent edit on a NON-emissive object (standard_object.casts_shadows) does NOT bump the light-topology generation" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 11d (P2 fix, documented conservative-bump behaviour): an agent edit
// whose entityKind the caller passes as EMPTY/unrecognized must bump the
// light-topology generation CONSERVATIVELY (see
// SceneEditor::BumpSceneLightGenerationForAgentParamEdit's doc) -- a spurious
// bump costs one alias-table rebuild; a missed bump on an actually-emissive
// target would be a rendering-correctness bug.  Drives the SAME `lum.scale`
// edit as 11b but with an empty entityKind, to isolate the "kind resolution"
// behaviour from "is this actually a material" -- an unresolved kind must
// bump regardless of what the entity turns out to be.
//////////////////////////////////////////////////////////////////////
static void TestAgentUnknownKindEditBumpsConservatively()
{
	std::cout << "Test 11d: unknown/empty-kind agent edit bumps light-topology generation conservatively..." << std::endl;

	const char* tmp = "agentlive_lightgen_unknown.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const unsigned int genBefore = pJob->GetLightTopologyGeneration();
		// Empty entityKind on the `white` PAINTER (definitely non-emissive --
		// painters have no material/emitter concept at all, and are outside
		// the Cst.cpp closure-gated invariant pass's object-emitter loop
		// entirely) -- isolates the conservative-bump rule cleanly: no other
		// mechanism in the engine could explain a bump here, so any bump
		// observed can ONLY be the fix's empty-kind fallback.
		// ApplyAgentParamEdit's CST-entity resolution still finds `white` by
		// name (DocFindByNameAnyRole tolerates an empty kind hint), but the
		// AGENT CALLER did not tell us what kind of entity it is -- the
		// conservative-bump rule must fire regardless of what it turns out
		// to be.
		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit(
				String( "white" ), String( "" ),
				String( "color" ), String( "0.9 0.9 0.9" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied.applied, "agent commit with an EMPTY entityKind applies cleanly" );
		const unsigned int genAfter = pJob->GetLightTopologyGeneration();
		Check( genAfter != genBefore,
		       "an EMPTY/unrecognized entityKind bumps the light-topology generation CONSERVATIVELY, even for a "
		       "definitely-non-emissive target (documented behaviour)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 12: the INTERLEAVING headline -- GUI edit A, agent edit B, GUI edit C;
// Undo x3 must revert C, then B (the agent edit!), then A, in strict LIFO
// order across BOTH clients (no special-casing by origin).  Redo x3 restores
// all three in forward order.  This is the concrete proof of the bug this
// slice fixes: previously, Undo after an agent edit would skip straight past
// it to an OLDER human edit, corrupting the user's mental model of "undo my
// last action."
//////////////////////////////////////////////////////////////////////
static void TestInterleavedLifoUndo()
{
	std::cout << "Test 12: interleaved GUI/agent LIFO undo/redo (the headline)..." << std::endl;

	const char* tmp = "agentlive_interleave.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.SetSelection( SceneEditController::Category::Material, String( "lum" ) );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const unsigned int undoBefore = c.Editor().History().UndoDepth();

		// A: GUI edit -- exitance white -> grey.
		Check( c.SetPropertyForCategory( SceneEditController::Category::Material, String( "exitance" ), String( "grey" ) ),
		       "[A] GUI edit (exitance -> grey) applies" );
		// B: agent edit -- scale -> 2.0.
		const SceneEditController::AgentCommitResult b =
			c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ),
			                       String( "scale" ), String( "2.0" ), nullptr );
		Check( b.applied, "[B] agent edit (scale -> 2.0) applies" );
		// C: GUI edit -- exitance grey -> white.
		Check( c.SetPropertyForCategory( SceneEditController::Category::Material, String( "exitance" ), String( "white" ) ),
		       "[C] GUI edit (exitance -> white) applies" );

		Check( c.Editor().History().UndoDepth() == undoBefore + 3, "all three edits (A, B, C) pushed history records" );
		// LumR right now: scale 2.0 * exitance white(1.0) = 2.0.
		Check( LumR( *pJob ) > 1.5 && LumR( *pJob ) < 2.5, "post-C derived state is scale2.0 * white ~= 2.0" );

		// Undo #1 -> reverts C (exitance back to grey); scale stays 2.0.
		c.Undo();
		Check( c.Editor().History().UndoDepth() == undoBefore + 2, "Undo #1 popped one record" );
		Check( LumR( *pJob ) > 0.9 && LumR( *pJob ) < 1.1, "Undo #1 reverted C: scale2.0 * grey0.5 ~= 1.0" );

		// Undo #2 -> reverts B (THE AGENT EDIT), scale back to 5.0; exitance stays grey.
		c.Undo();
		Check( c.Editor().History().UndoDepth() == undoBefore + 1, "Undo #2 popped one record" );
		Check( LumR( *pJob ) > 2.4 && LumR( *pJob ) < 2.6,
		       "Undo #2 reverted THE AGENT EDIT (B): scale5.0 * grey0.5 ~= 2.5 -- "
		       "RED-PROVE headline: at the pre-fix parent, this Undo would have "
		       "skipped B entirely and reverted an OLDER edit (or nothing)" );

		// Undo #3 -> reverts A (exitance back to white); scale stays 5.0 (original).
		c.Undo();
		Check( c.Editor().History().UndoDepth() == undoBefore, "Undo #3 popped the last record -- back to baseline" );
		Check( LumR( *pJob ) > 4.5 && LumR( *pJob ) < 5.5, "Undo #3 reverted A: back to the ORIGINAL scale5.0 * white1.0 ~= 5.0" );
		Check( c.Editor().History().RedoDepth() == 3, "all three reverted edits are on the redo stack" );

		// Redo x3 restores forward order: A, then B (the agent edit), then C.
		c.Redo();
		Check( LumR( *pJob ) > 2.4 && LumR( *pJob ) < 2.6, "Redo #1 re-applied A: scale5.0 * grey0.5 ~= 2.5" );
		c.Redo();
		Check( LumR( *pJob ) > 0.9 && LumR( *pJob ) < 1.1, "Redo #2 re-applied THE AGENT EDIT (B): scale2.0 * grey0.5 ~= 1.0" );
		c.Redo();
		Check( LumR( *pJob ) > 1.5 && LumR( *pJob ) < 2.5, "Redo #3 re-applied C: scale2.0 * white1.0 ~= 2.0" );
		Check( c.Editor().History().UndoDepth() == undoBefore + 3, "all three edits restored to the undo stack" );
		Check( c.Editor().History().RedoDepth() == 0, "redo stack fully drained" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 13: the defaulted-slot (absent-param) case.  `lum` on kNoScaleScene has
// NO `scale` line (defaults to 1.0 via the descriptor).  An agent edit to
// `scale` INSERTS the param; Undo's inverse must REMOVE it -- proven by a
// Document byte-comparison against the pre-edit serialization (not merely
// "scale is absent from a live read", which the derived-scene default would
// also satisfy even if the PARAM TEXT were wrongly left behind as "1.0").
//////////////////////////////////////////////////////////////////////
static void TestAgentEditDefaultedSlotUndo()
{
	std::cout << "Test 13: agent edit on a DEFAULTED (absent) slot -- undo REMOVES the inserted param..." << std::endl;

	const char* tmp = "agentlive_defaulted.RISEscene";
	Job* pJob = LoadScene( kNoScaleScene, tmp );
	Check( pJob != nullptr, "no-scale scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const std::string preEditDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( preEditDoc.find( "scale" ) == std::string::npos, "pre-edit Document has NO `scale` param (red-prove the absent-slot premise)" );
		Check( LumR( *pJob ) > 0.9 && LumR( *pJob ) < 1.1, "pre-edit derived scale defaults to 1.0" );

		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "9.0" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied.applied, "agent edit on the defaulted slot applies (INSERTS the param)" );
		Check( LumR( *pJob ) > 8.5 && LumR( *pJob ) < 9.5, "post-edit derived scale is 9.0" );
		{
			const std::string postEditDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			Check( postEditDoc.find( "scale" ) != std::string::npos, "the edit INSERTED a `scale` param into the Document" );
		}

		c.Undo();
		Check( LumR( *pJob ) > 0.9 && LumR( *pJob ) < 1.1, "Undo restored the derived scale to the descriptor default (1.0)" );

		// The precise assertion: Document BYTE-COMPARISON vs the pre-edit
		// serialization -- proves the inverse is a true REMOVE (the chunk's
		// param list is back to exactly what it was), not a re-SET to "1.0"
		// (which would leave a `scale 1.0` line the original text never had).
		const std::string postUndoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postUndoDoc.find( "scale" ) == std::string::npos,
		       "Undo REMOVED the param entirely (not re-set to the default) -- `scale` absent again" );
		Check( postUndoDoc == preEditDoc,
		       "Undo's Document is BYTE-IDENTICAL to the pre-edit Document (true remove, not a masked re-set)" );

		// Redo re-inserts it.
		c.Redo();
		Check( LumR( *pJob ) > 8.5 && LumR( *pJob ) < 9.5, "Redo re-inserted the param (derived scale back to 9.0)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 14: code-3 (diagnosed-but-mutated) is STILL undoable -- the documented
// choice (see ApplyAgentParamEdit's comment): the Document WAS mutated (the
// managers WERE replaced) even though the source contract reports code 3 as a
// failure, so treating it as un-revertable would be strictly worse than
// treating it as revertable.  Reuses the CodeThreeJob rewrite technique (a
// REAL D2 re-derive on a variant scene, with the reported code corrupted to 3
// after the fact) so the Scene + managers really are replaced.
//////////////////////////////////////////////////////////////////////
static void TestCodeThreeUndo()
{
	std::cout << "Test 14: code-3 (diagnosed) agent edit is STILL undo/redo-able (documented choice)..." << std::endl;

	const char* tmp = "agentlive_c3undo.RISEscene";
	{ std::ofstream o( tmp ); o << kVariantScene; }
	CodeThreeJob* pJob = new CodeThreeJob();
	Check( pJob->LoadAsciiSceneViaCst( tmp ), "variant scene loads into CodeThreeJob via the CST path" );

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const unsigned int undoBefore = c.Editor().History().UndoDepth();

		const SceneEditController::AgentCommitResult r =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "3.5" ),
				/*baseVersionOrNull*/ nullptr );
		Check( !r.applied && r.rawCode == 3, "the edit is a code-3 diagnosed-but-mutated commit" );

		// DOCUMENTED CHOICE: even though applied==false, the Document WAS
		// mutated -- a history record IS pushed (capture-before-apply already
		// captured the correct prior state regardless of the outcome code).
		Check( c.Editor().History().UndoDepth() == undoBefore + 1,
		       "a code-3 (diagnosed) commit STILL pushes a history record (documented choice)" );

		// Undo's revert is ITSELF a D2 re-derive on this variant scene, which
		// CodeThreeJob would otherwise ALSO force-rewrite to 3 -- and
		// RouteCstParamEdit_ (the revert's route helper, shared with every
		// other CST-routed property Undo in this codebase) treats a 3 as a
		// FAILED route, same as the forward path's own contract.  Turn the
		// force off for the revert so it takes the underlying clean D2 (code
		// 2) the base Job really produces -- this test is about whether a
		// code-3 FORWARD commit is undoable, not about double-diagnosing the
		// revert too.
		pJob->SetForceCodeThree( false );
		c.Undo();
		Check( c.Editor().History().UndoDepth() == undoBefore, "the code-3 edit IS undoable" );
		Check( LumR( *pJob ) > 4.0 && LumR( *pJob ) < 6.0, "Undo restored the pre-edit derived scale (~5.0)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 15: conflict-gate after an Undo of an agent edit -- the revision moved
// (Undo is itself a Document mutation), so a stale-base propose_patch built
// against the PRE-UNDO head must CONFLICT, exactly like any other head move.
//////////////////////////////////////////////////////////////////////
static void TestConflictGateAfterUndo()
{
	std::cout << "Test 15: conflict-gate sees the revision move caused by an Undo..." << std::endl;

	const char* tmp = "agentlive_conflictundo.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ),
			                       String( "scale" ), String( "7.0" ), nullptr );
		Check( applied.applied, "agent edit applies" );

		// A client reads the head HERE (pre-Undo) and will build its next
		// proposal against this now-stale base.
		const RISE::Cst::CstHeadVersion staleBase = pJob->GetCstHeadVersion();

		c.Undo();   // mutates the Document again -- the head MOVES.

		const SceneEditController::AgentCommitResult stale =
			c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ),
			                       String( "scale" ), String( "8.0" ), &staleBase );
		Check( stale.conflict, "a propose_patch against the PRE-UNDO base CONFLICTS (Undo moved the revision)" );
		Check( !stale.applied, "the conflicting commit did not apply" );

		// Re-read + retry succeeds.
		const RISE::Cst::CstHeadVersion fresh = stale.headVersion;
		const SceneEditController::AgentCommitResult retry =
			c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ),
			                       String( "scale" ), String( "8.0" ), &fresh );
		Check( retry.applied, "retry with the fresh (post-Undo) base applies cleanly" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 16: HasUnsavedChanges stays sane across an undo-to-clean round trip --
// an agent edit dirties the editor; undoing it back to the pre-edit baseline
// should leave HasUnsavedChanges() FALSE again (matching the existing GUI
// undo-to-clean behaviour -- DirtyTracker's per-entity channel is a SET, and
// Undo re-marks the SAME entity dirty rather than clearing it, so this pins
// the OBSERVED behaviour rather than asserting an idealized one).
//////////////////////////////////////////////////////////////////////
static void TestUndoToCleanDirtyState()
{
	std::cout << "Test 16: HasUnsavedChanges across an agent-edit undo-to-clean round trip..." << std::endl;

	const char* tmp = "agentlive_undoclean.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );
		Check( !c.HasUnsavedChanges(), "clean before any edit" );

		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ),
			                       String( "scale" ), String( "7.0" ), nullptr );
		Check( applied.applied, "agent edit applies" );
		Check( c.HasUnsavedChanges(), "dirty after the agent edit" );

		c.Undo();
		// Observed behaviour (matches the existing GUI undo path): Undo
		// re-marks the SAME entity dirty via MarkEditEntityDirty inside
		// SceneEditor::Undo, so HasUnsavedChanges() stays true rather than
		// clearing -- the per-entity channel is a SET (Material,"lum"), not a
		// counter, so undoing the edit that dirtied it does not remove it.
		// This test PINS that observed behaviour rather than asserting an
		// idealized "undo fully un-dirties" contract this codebase doesn't
		// implement anywhere (GUI undo has the identical property).
		Check( c.HasUnsavedChanges(), "still reports dirty after Undo (matches existing GUI undo-to-clean behaviour: "
		                              "the per-entity dirty SET is not cleared by an Undo re-mark)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 18 (P1-1 fix, round 1): a MULTI-TOKEN param value (`color 1 1 1`) must
// round-trip through capture+Undo WITHOUT truncation.  RED-PROVE: at the
// pre-fix parent, AgentReadFirstParamValue / S2ChunkParamValue kept only the
// FIRST pvalue token, so capturing "1 1 1" before the edit yielded "1", and
// Undo's re-SET of the captured "prior value" wrote back `color 1` -- the
// bug report's measured corruption (post-undo r=5 g=0 b=0 instead of 5 5 5,
// reproduced here on `white`'s RGB painter feeding `lum`'s exitance).
//////////////////////////////////////////////////////////////////////
static void TestMultiTokenParamUndo()
{
	std::cout << "Test 18: multi-token param (`color r g b`) survives capture+Undo intact (P1-1)..." << std::endl;

	const char* tmp = "agentlive_multitoken.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// Pre-edit: `white` is "1 1 1" (kBaseScene), feeding lum's exitance at scale 5.0.
		Check( LumR( *pJob ) > 4.5 && LumR( *pJob ) < 5.5, "pre-edit R ~5.0 (scale5.0 * white(1,1,1).r)" );
		Check( LumG( *pJob ) > 4.5 && LumG( *pJob ) < 5.5, "pre-edit G ~5.0" );
		Check( LumB( *pJob ) > 4.5 && LumB( *pJob ) < 5.5, "pre-edit B ~5.0" );

		// Agent edits `white`'s `color` to an ASYMMETRIC 3-component value -- if the
		// capture truncates to the first token, Undo will write back "0.2" alone
		// (a single-component re-tokenisation), not "0.2 0.4 0.6".
		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit(
				String( "white" ), String( "uniformcolor_painter" ),
				String( "color" ), String( "0.2 0.4 0.6" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied.applied, "agent multi-token color edit applies" );
		Check( LumR( *pJob ) > 0.9 && LumR( *pJob ) < 1.1, "post-edit R ~1.0 (scale5.0 * 0.2)" );
		Check( LumG( *pJob ) > 1.9 && LumG( *pJob ) < 2.1, "post-edit G ~2.0 (scale5.0 * 0.4)" );
		Check( LumB( *pJob ) > 2.9 && LumB( *pJob ) < 3.1, "post-edit B ~3.0 (scale5.0 * 0.6)" );

		c.Undo();

		// RED-PROVE: the derived scene's G and B channels are the load-bearing
		// assertions -- a truncated capture ("0.2" only) re-tokenises via
		// DocSetParamValue's SplitWs into a SINGLE-component color, which most
		// painter implementations broadcast/zero-fill asymmetrically (measured
		// bug-report shape: r=5 g=0 b=0). A correct capture restores ALL THREE
		// original components.
		Check( LumR( *pJob ) > 4.5 && LumR( *pJob ) < 5.5,
		       "Undo restored R ~5.0 (the ORIGINAL first component)" );
		Check( LumG( *pJob ) > 4.5 && LumG( *pJob ) < 5.5,
		       "RED-PROVE: Undo restored G ~5.0 -- at the pre-fix parent this is ~0 (truncated capture lost the G/B components)" );
		Check( LumB( *pJob ) > 4.5 && LumB( *pJob ) < 5.5,
		       "RED-PROVE: Undo restored B ~5.0 -- at the pre-fix parent this is ~0 (truncated capture lost the G/B components)" );

		// Document-level proof (independent of any painter-specific broadcast
		// semantics): the serialized `color` line must read back as all three
		// original tokens, not a single truncated one.
		const std::string doc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( doc.find( "color 1 1 1" ) != std::string::npos,
		       "RED-PROVE: the Document's `color` param text is byte-exact `1 1 1` after Undo "
		       "(at the pre-fix parent it reads `color 1`, missing the 2nd/3rd tokens)" );
		Check( doc.find( "color 0.2" ) == std::string::npos,
		       "no truncated remnant (`color 0.2` with no `0.4 0.6`) survives in the Document" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 19 (P1-2 fix, round 1): DocRemoveParam's pre-fix "strip EVERY
// occurrence" behaviour must NOT resurface through the agent Undo path for a
// REPEATABLE param.  Scenario: `sh` (a standard_shader) starts with ZERO
// `shaderop` lines; the agent inserts occ 0 (`prevValueWasAbsent` -> Undo's
// inverse is a REMOVE, per Test 13's pattern).  BEFORE the Undo fires, two
// MORE `shaderop` occurrences land on the SAME chunk via an entirely
// independent mechanism (chunk-CRUD remove+reinsert of the whole `sh` chunk,
// simulating "other edits" per the P1-2 spec -- the chunk's NodeId changes,
// but SceneEditor::RouteCstParamRemove_ resolves by NAME, exactly the
// chunk-CRUD-interleaving hazard RouteCstParamEditChecked_'s own doc
// describes). RED-PROVE: at the pre-fix parent (Cst::DocRemoveParam strips
// ALL same-role params), Undo deletes all THREE `shaderop` lines --
// irrecoverably losing the two occurrences the agent's Undo was never asked
// to touch. Post-fix (Cst::DocRemoveParamOcc), Undo removes ONLY occurrence 0.
//////////////////////////////////////////////////////////////////////
static const char* kShaderCrudScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n"
	"standard_shader\n{\nname sh\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\nshader global\n}\n";

// Extracts the substring covering the `standard_shader { name sh ... }` chunk out of a whole-Document
// serialization -- `sh` is not the only chunk in kShaderCrudScene with a `shaderop` param (`global` has one
// too), so counting "shaderop" occurrences over the WHOLE document would over-count. Scopes the search to
// between "name sh" and the very next "}".
static std::string ShChunkText( const std::string& doc )
{
	const size_t nameAt = doc.find( "name sh" );
	if( nameAt == std::string::npos ) return std::string();
	const size_t closeAt = doc.find( "\n}", nameAt );
	if( closeAt == std::string::npos ) return doc.substr( nameAt );
	return doc.substr( nameAt, closeAt - nameAt );
}

static void TestOccAwareParamRemoveUndo()
{
	std::cout << "Test 19: occ-aware param remove -- Undo deletes ONLY its own occurrence, not every occurrence (P1-2)..." << std::endl;

	const char* tmp = "agentlive_occaware.RISEscene";
	Job* pJob = LoadScene( kShaderCrudScene, tmp );
	Check( pJob != nullptr, "shader-crud scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		{
			const std::string preDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			const std::string shPre = ShChunkText( preDoc );
			Check( !shPre.empty(), "`sh`'s chunk text is found in the pre-edit Document" );
			Check( shPre.find( "shaderop" ) == std::string::npos,
			       "pre-edit: `sh` has NO `shaderop` line yet (red-prove the absent-slot premise)" );
		}

		// Agent inserts occ 0 on `sh` (the param was ABSENT -> prevValueWasAbsent).
		const SceneEditController::AgentCommitResult applied =
			c.ApplyAgentParamEdit(
				String( "sh" ), String( "standard_shader" ),
				String( "shaderop" ), String( "DefaultPathTracing" ),
				/*baseVersionOrNull*/ nullptr );
		Check( applied.applied, "agent insert of occ 0 (`shaderop`) on `sh` applies" );
		const unsigned int undoDepthAfterParamEdit = c.Editor().History().UndoDepth();

		// Independent mechanism ("other edits"): remove + reinsert the WHOLE `sh`
		// chunk with THREE shaderop lines (occ 0 unchanged in VALUE, occ 1 + 2 new).
		// Cst has no primitive to APPEND occurrence >0 of a repeatable param in
		// place (Cst::WithParamValueOrInsert only inserts at occ 0 when absent --
		// see its own doc), so a whole-chunk remove+reinsert is the only way to
		// manufacture a THIRD occurrence.  Routed DIRECTLY through Job's chunk-CRUD
		// primitives (NOT the controller's ApplyAgentRemoveChunk/ApplyAgentInsertChunk)
		// -- since Shared-undo U2 (this slice) made those two controller verbs
		// first-class undo/redo citizens, routing "other edits" through them would
		// ADD their own history entries and no longer simulate an INDEPENDENT
		// mechanism outside the shared-undo system (e.g. a second agent session, a
		// raw Document edit, a script) that leaves NO history footprint of its own.
		// Wrapped in RunPreviewRenderParked (the SAME cancel-and-park critical
		// section the controller's own agent-commit paths use) so the direct Job
		// mutation cannot race the live render thread, followed by the SAME
		// manual re-bind ApplyAgentChunkCrud_ itself would have done on a D2
		// (RebindEditorToJob is private; this reproduces it via the public
		// Editor()/RebindScene + manager-setter surface) -- else the controller's
		// cached scene/manager pointers would dangle after the ClearAll.  This
		// changes `sh`'s NodeId but NOT its name -- RouteCstParamRemove_ resolves
		// by name, so the agent's later Undo (still the NEXT entry to pop -- this
		// direct route added no history) is unaffected by the identity change
		// (the documented chunk-CRUD-interleaving hazard RouteCstParamEditChecked_'s
		// doc describes).
		Check( c.RunPreviewRenderParked( [&]{
			char kw[64]; char diag[256];
			const int rmCode = pJob->ApplyCstRemoveChunk( "sh", "standard_shader", kw, sizeof( kw ), diag, sizeof( diag ) );
			Check( rmCode == 2 || rmCode == 3, "removing the old `sh` chunk succeeds (independent of the agent Undo path, no history pushed)" );

			const char* newChunk =
				"standard_shader\n{\nname sh\nshaderop DefaultPathTracing\nshaderop DefaultDirectLighting\nshaderop DefaultEmission\n}\n";
			char kw2[64]; char nm2[64]; char diag2[256];
			const int insCode = pJob->ApplyCstInsertChunk( newChunk, kw2, sizeof( kw2 ), nm2, sizeof( nm2 ), diag2, sizeof( diag2 ) );
			Check( insCode == 2 || insCode == 3, "reinserting `sh` with THREE shaderop occurrences succeeds (no history pushed)" );

			// Manual rebind (mirrors SceneEditController::RebindEditorToJob, which is
			// private -- this reproduces it via the public surface).
			c.Editor().RebindScene( *pJob->GetScene() );
			c.Editor().SetMaterialManager( pJob->GetMaterials() );
			c.Editor().SetShaderManager( pJob->GetShaders() );
			c.Editor().SetPainterManager( pJob->GetPainters() );
			c.Editor().SetScalarPainterManager( pJob->GetScalarPainters() );
			c.Editor().SetJob( pJob );
		} ), "the parked direct-Job 'other edits' + manual rebind ran" );
		Check( c.Editor().History().UndoDepth() == undoDepthAfterParamEdit,
		       "the direct Job-level chunk-CRUD 'other edits' pushed NO history of their own -- the agent's "
		       "param-edit entry is still the next one to pop" );
		{
			// Scope the occurrence count to `sh`'s OWN chunk text -- `global` (a
			// SEPARATE standard_shader in this scene) also carries a `shaderop`
			// param, so counting over the WHOLE document would over-count.
			const std::string midDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			const std::string shMid = ShChunkText( midDoc );
			Check( !shMid.empty(), "`sh`'s chunk text is found in the mid-test Document" );
			int occurrences = 0;
			size_t pos = 0;
			while( ( pos = shMid.find( "shaderop", pos ) ) != std::string::npos ) { ++occurrences; pos += 8; }
			Check( occurrences == 3, "the `sh` chunk now carries THREE shaderop occurrences (0=agent's, 1+2=`other edits`)" );
		}

		// The Undo of the ORIGINAL agent insert must remove ONLY occurrence 0.
		c.Undo();

		const std::string postUndoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		const std::string shPostUndo = ShChunkText( postUndoDoc );
		Check( !shPostUndo.empty(), "`sh`'s chunk text is found in the post-Undo Document" );
		int occurrencesAfter = 0;
		{
			size_t pos = 0;
			while( ( pos = shPostUndo.find( "shaderop", pos ) ) != std::string::npos ) { ++occurrencesAfter; pos += 8; }
		}
		Check( occurrencesAfter == 2,
		       "RED-PROVE: exactly TWO shaderop occurrences survive Undo on `sh` -- at the pre-fix parent "
		       "(Cst::DocRemoveParam strips EVERY same-role occurrence) this is 0 (both `other-edit` "
		       "occurrences are irrecoverably lost)" );
		Check( shPostUndo.find( "shaderop DefaultDirectLighting" ) != std::string::npos,
		       "RED-PROVE: occurrence 1 (DefaultDirectLighting) survives Undo byte-exactly" );
		Check( shPostUndo.find( "shaderop DefaultEmission" ) != std::string::npos,
		       "RED-PROVE: occurrence 2 (DefaultEmission) survives Undo byte-exactly" );
		Check( shPostUndo.find( "shaderop DefaultPathTracing" ) == std::string::npos,
		       "occurrence 0 (the agent's own insert) IS removed by Undo (the inverse of ITS OWN insert) -- "
		       "scoped to `sh`'s OWN chunk (`global`'s separate DefaultPathTracing line is untouched)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 20 (P1-3 fix, round 1): an ARMED code-3 (diagnosed-but-mutated) revert
// must complete the undo/redo stack transfer, not just "still be undoable via
// a laundered clean re-derive" (Test 14 disarmed force-code-three before
// calling Undo specifically to dodge this path -- see its own comment). This
// test leaves force-code-three ARMED THROUGH the Undo (and the following
// Redo), so RouteCstParamEditChecked_ itself observes code 3 and must treat
// it as "mutation landed" for the stack-transfer bookkeeping, matching the
// forward path's own documented choice (ApplyAgentParamEdit pushes history on
// EVERY mutating code, including 3). RED-PROVE: at the pre-fix parent
// (`r == 1 || r == 2`), Undo's ApplyRevertMutation returns false even though
// the Document WAS mutated + rebound -- SceneEditor::Undo() then calls
// RestoreLastUndoFromRedo(), moving the entry BACK to the undo stack even
// though the live Document has ALREADY been reverted -- a subsequent Cmd-Z
// double-reverts (applies the SAME inverse a second time) instead of no-op /
// redo working correctly.
//////////////////////////////////////////////////////////////////////
static void TestArmedCodeThreeRevert()
{
	std::cout << "Test 20: ARMED code-3 revert completes the undo/redo stack transfer (P1-3)..." << std::endl;

	const char* tmp = "agentlive_c3armed.RISEscene";
	{ std::ofstream o( tmp ); o << kVariantScene; }
	CodeThreeJob* pJob = new CodeThreeJob();
	Check( pJob->LoadAsciiSceneViaCst( tmp ), "variant scene loads into CodeThreeJob via the CST path" );

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const unsigned int undoBefore = c.Editor().History().UndoDepth();

		// Forward commit: force-code-three is ARMED (CodeThreeJob's ctor default),
		// so this lands as a code-3 diagnosed-but-mutated commit (Test 14's setup).
		const SceneEditController::AgentCommitResult r =
			c.ApplyAgentParamEdit(
				String( "lum" ), String( "lambertian_luminaire_material" ),
				String( "scale" ), String( "3.5" ),
				/*baseVersionOrNull*/ nullptr );
		Check( !r.applied && r.rawCode == 3, "the forward edit is a code-3 diagnosed-but-mutated commit" );
		Check( c.Editor().History().UndoDepth() == undoBefore + 1, "the code-3 forward commit still pushed a history record" );
		Check( LumR( *pJob ) > 3.0 && LumR( *pJob ) < 4.0, "the live derived scene DID move to ~3.5 despite the diagnosed code" );

		// Undo with force-code-three STILL ARMED -- RouteCstParamEditChecked_'s
		// underlying ApplyCstParamEditChecked call ALSO returns 3 here (the revert
		// is itself a clean D2 re-derive that CodeThreeJob rewrites 2->3).
		c.Undo();

		// The entry MUST have moved to the redo stack -- NOT stayed on / been
		// restored to the undo stack (RestoreLastUndoFromRedo would leave undo
		// depth UNCHANGED at undoBefore+1).
		Check( c.Editor().History().UndoDepth() == undoBefore,
		       "RED-PROVE: the undo stack depth dropped back to baseline -- at the pre-fix parent "
		       "(bool return `r==1||r==2`) RouteCstParamEditChecked_ reports FALSE for code 3, "
		       "Undo() calls RestoreLastUndoFromRedo(), and this stays at undoBefore+1" );
		Check( c.Editor().History().RedoDepth() == 1,
		       "RED-PROVE: the reverted entry landed on the redo stack -- at the pre-fix parent it "
		       "never reaches the redo stack (moved back to undo instead)" );

		// The Document ACTUALLY reverted (the mutation landed even though the
		// code is diagnosed) -- byte-compare against the known pre-edit value.
		Check( LumR( *pJob ) > 4.0 && LumR( *pJob ) < 6.0, "Undo's diagnosed-but-mutated revert DID restore the derived scale to ~5.0" );
		{
			const std::string doc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			Check( doc.find( "scale 5" ) != std::string::npos,
			       "the reverted Document's `scale` param text reads back 5.0 (byte-level proof the revert landed)" );
		}

		// A subsequent Redo must work (not double-revert, not wedge). Still armed.
		c.Redo();
		Check( c.Editor().History().UndoDepth() == undoBefore + 1, "Redo moved the entry back to the undo stack" );
		Check( c.Editor().History().RedoDepth() == 0, "redo stack drained after the single Redo" );
		Check( LumR( *pJob ) > 3.0 && LumR( *pJob ) < 4.0, "Redo re-applied the scale-3.5 edit to the derived scene" );

		// Repeated Cmd-Z (Undo again) must revert ONCE more, cleanly -- not
		// double-revert (which would be the symptom of a wedged/duplicated
		// history entry from the pre-fix divergence).
		c.Undo();
		Check( c.Editor().History().UndoDepth() == undoBefore, "the second Undo again drops to baseline" );
		Check( LumR( *pJob ) > 4.0 && LumR( *pJob ) < 6.0, "the second Undo again restores ~5.0 (no double-revert artifact)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 21 (P1-4 fix, round 1): Undo/Redo of an agent edit must mark the
// editor dirty in BOTH directions when the mutation lands AWAY from the last
// saved bytes. RED-PROVE shape: edit -> Save (clears dirty) -> Redo of a
// LATER undo (re-applying a change) must leave HasUnsavedChanges()==true,
// and symmetrically edit -> Save -> Undo must ALSO leave it true. At the
// pre-fix parent, MarkEditEntityDirty falls to `default: break` for
// SetAgentCstParam, so neither call marks anything -- only the forward
// ApplyAgentParamEdit path's direct MarkCstHeadDirty call did, and Save
// clears the tracker Save sees. A close-without-prompt after either gap
// would silently discard the redone/undone edit.
//////////////////////////////////////////////////////////////////////
static void TestUndoRedoAfterSaveMarksDirty()
{
	std::cout << "Test 21: Undo/Redo of an agent edit marks dirty even AFTER a Save (P1-4)..." << std::endl;

	// ---- Redo-after-save direction -----------------------------------
	{
		const char* tmp = "agentlive_dirtyredo.RISEscene";
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "base scene loads (redo-after-save case)" );
		if( pJob ) {
			TestController c( *pJob, /*simulatedRenderMs*/20 );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

			const SceneEditController::AgentCommitResult applied =
				c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ),
				                       String( "scale" ), String( "7.0" ), nullptr );
			Check( applied.applied, "agent edit applies" );

			c.Undo();
			Check( LumR( *pJob ) > 4.5 && LumR( *pJob ) < 5.5, "Undo reverted to the original scale~5.0" );

			// Save NOW, at the reverted (pre-edit) state -- clears the dirty tracker.
			const char* saveAs = "agentlive_dirtyredo_save.RISEscene";
			std::remove( saveAs );
			const SaveResult sr = c.RequestSave( std::string( saveAs ) );
			Check( Succeeded( sr.status ), "save succeeds at the reverted (pre-edit) state" );
			Check( !c.HasUnsavedChanges(), "clean immediately after the save" );

			// Redo re-applies the scale-7.0 edit -- this mutates AWAY from the just-saved bytes.
			c.Redo();
			Check( LumR( *pJob ) > 6.0 && LumR( *pJob ) < 8.0, "Redo re-applied the scale-7.0 edit to the derived scene" );
			Check( c.HasUnsavedChanges(),
			       "RED-PROVE: HasUnsavedChanges() is TRUE after Redo-following-a-Save -- at the pre-fix parent "
			       "MarkEditEntityDirty has no SetAgentCstParam case (falls to default:break), so this stays "
			       "FALSE and a close-without-prompt would silently discard the redone edit" );

			c.Stop();
			std::remove( saveAs );
		}
		if( pJob ) pJob->release();
		std::remove( tmp );
	}

	// ---- Undo-after-save direction -----------------------------------
	{
		const char* tmp = "agentlive_dirtyundo.RISEscene";
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "base scene loads (undo-after-save case)" );
		if( pJob ) {
			TestController c( *pJob, /*simulatedRenderMs*/20 );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

			const SceneEditController::AgentCommitResult applied =
				c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ),
				                       String( "scale" ), String( "7.0" ), nullptr );
			Check( applied.applied, "agent edit applies" );

			// Save NOW, at the post-edit (scale~7.0) state -- clears the dirty tracker.
			const char* saveAs = "agentlive_dirtyundo_save.RISEscene";
			std::remove( saveAs );
			const SaveResult sr = c.RequestSave( std::string( saveAs ) );
			Check( Succeeded( sr.status ), "save succeeds at the post-edit state" );
			Check( !c.HasUnsavedChanges(), "clean immediately after the save" );

			// Undo reverts the scale-7.0 edit -- this mutates AWAY from the just-saved bytes.
			c.Undo();
			Check( LumR( *pJob ) > 4.5 && LumR( *pJob ) < 5.5, "Undo reverted to the original scale~5.0" );
			Check( c.HasUnsavedChanges(),
			       "RED-PROVE: HasUnsavedChanges() is TRUE after Undo-following-a-Save -- at the pre-fix parent "
			       "this stays FALSE (same MarkEditEntityDirty gap) and a close-without-prompt would silently "
			       "discard the undone edit (the scene would reopen at the SAVED scale~7.0 state, "
			       "silently losing the user's Undo)" );

			c.Stop();
			std::remove( saveAs );
		}
		if( pJob ) pJob->release();
		std::remove( tmp );
	}
}

//////////////////////////////////////////////////////////////////////
// Model-B shared-undo U2 fixtures + tests: agent chunk CRUD (insert_chunk /
// remove_chunk) is a first-class undo/redo citizen.  A base scene with THREE
// materials in document order (`matA`, `matB`, `matC`) so a remove of the
// MIDDLE one (`matB`) exercises the exact-position restore (the headline
// byte-identity claim requires a chunk that sits BETWEEN two others, not at
// the document's edge).
//////////////////////////////////////////////////////////////////////
static const char* kU2Scene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"lambertian_material\n{\nname matA\nreflectance white\n}\n"
	"lambertian_material\n{\nname matB\nreflectance white\n}\n"
	"lambertian_material\n{\nname matC\nreflectance white\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

//////////////////////////////////////////////////////////////////////
// Test 22: insert_chunk -> Undo -> byte-identical to pre-insert; derived
// scene coherent; Redo re-inserts byte-exactly; revision bumps on each;
// dirty marked in both directions.
//////////////////////////////////////////////////////////////////////
static void TestChunkInsertUndoRedo()
{
	std::cout << "Test 22: insert_chunk Undo/Redo -- byte-identical round trip (shared-undo U2)..." << std::endl;

	const char* tmp = "agentlive_u2_insert.RISEscene";
	Job* pJob = LoadScene( kU2Scene, tmp );
	Check( pJob != nullptr, "U2 fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const std::string preDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		const RISE::Cst::CstHeadVersion preVersion = pJob->GetCstHeadVersion();
		Check( !c.HasUnsavedChanges(), "clean before any edit" );

		const char* newChunk = "lambertian_material\n{\nname matD\nreflectance white\n}\n";
		const SceneEditController::AgentCommitResult ins = c.ApplyAgentInsertChunk( String( newChunk ), nullptr );
		Check( ins.applied, "insert_chunk applies" );
		Check( ins.chunkName == String( "matD" ), "insert echoes the resolved name" );
		Check( ins.headVersion.revision == preVersion.revision + 1, "insert bumped the revision by exactly one" );
		Check( c.HasUnsavedChanges(), "insert marks the editor dirty" );
		const std::string postInsertDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postInsertDoc.find( "matD" ) != std::string::npos, "the inserted chunk is present in the Document" );
		Check( postInsertDoc != preDoc, "the Document actually changed" );

		// Undo -> byte-identical to the pre-insert Document.
		c.Undo();
		const std::string postUndoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postUndoDoc == preDoc, "Undo of insert_chunk restores the Document BYTE-IDENTICAL to pre-insert" );
		Check( pJob->GetCstHeadVersion().revision == preVersion.revision + 2,
		       "Undo bumped the revision again (a genuine Document mutation, not a no-op)" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matD" ) == nullptr,
		       "the derived scene no longer has `matD` after Undo (live matches the reverted Document)" );

		// Redo -> re-inserts byte-exactly (same bytes the agent originally supplied).
		c.Redo();
		const std::string postRedoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postRedoDoc == postInsertDoc, "Redo of insert_chunk re-inserts BYTE-EXACTLY (matches the post-insert Document)" );
		Check( pJob->GetCstHeadVersion().revision == preVersion.revision + 3, "Redo bumped the revision a third time" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matD" ) != nullptr,
		       "the derived scene has `matD` again after Redo" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 23: remove_chunk on a MIDDLE chunk (matB, sandwiched between matA and
// matC) -> Undo -> byte-identical restore INCLUDING position -> Redo
// re-removes byte-exactly.  This is the headline exact-position claim:
// DocInsertItem-at-the-same-index is the documented byte-exact inverse of
// DocRemoveItem, so the WHOLE Document (not just the chunk's own text) must
// compare equal.
//////////////////////////////////////////////////////////////////////
static void TestChunkRemoveMiddleUndoRedo()
{
	std::cout << "Test 23: remove_chunk (middle chunk) Undo/Redo -- byte-identical WITH position (shared-undo U2)..." << std::endl;

	const char* tmp = "agentlive_u2_remove.RISEscene";
	Job* pJob = LoadScene( kU2Scene, tmp );
	Check( pJob != nullptr, "U2 fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const std::string preDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( preDoc.find( "matA" ) != std::string::npos
		    && preDoc.find( "matB" ) != std::string::npos
		    && preDoc.find( "matC" ) != std::string::npos,
		       "all three sandwiching materials present pre-edit" );
		const RISE::Cst::CstHeadVersion preVersion = pJob->GetCstHeadVersion();

		const SceneEditController::AgentCommitResult rm =
			c.ApplyAgentRemoveChunk( String( "matB" ), String( "material" ), nullptr );
		Check( rm.applied, "remove_chunk on the middle material (matB) applies" );
		Check( rm.headVersion.revision == preVersion.revision + 1, "remove bumped the revision by exactly one" );
		Check( c.HasUnsavedChanges(), "remove marks the editor dirty" );
		const std::string postRemoveDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postRemoveDoc.find( "matB" ) == std::string::npos, "matB is gone from the Document" );
		Check( postRemoveDoc.find( "matA" ) != std::string::npos && postRemoveDoc.find( "matC" ) != std::string::npos,
		       "matA and matC (the sandwiching neighbours) survive" );

		// Undo -> byte-identical restore INCLUDING position (the whole Document, not
		// just matB's own text, must compare equal -- proves matA/matC's relationship
		// to matB round-trips exactly, not just that matB's bytes happen to reappear
		// somewhere).
		c.Undo();
		const std::string postUndoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postUndoDoc == preDoc,
		       "Undo of remove_chunk restores the FULL Document BYTE-IDENTICAL to pre-remove, INCLUDING matB's exact position between matA and matC" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matB" ) != nullptr,
		       "the derived scene has `matB` again after Undo" );

		// Redo -> re-removes byte-exactly.
		c.Redo();
		const std::string postRedoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postRedoDoc == postRemoveDoc, "Redo of remove_chunk re-removes BYTE-EXACTLY (matches the post-remove Document)" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matB" ) == nullptr,
		       "the derived scene no longer has `matB` after Redo" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 24: interleaved LIFO with param edits -- agent insert X -> agent
// param edit on X -> Undo (param) -> Undo (insert gone) -> Redo x2 restores
// both; byte-compare at each step.
//////////////////////////////////////////////////////////////////////
static void TestChunkInsertThenParamEditInterleavedLifo()
{
	std::cout << "Test 24: interleaved insert + param-edit LIFO undo/redo (shared-undo U2)..." << std::endl;

	const char* tmp = "agentlive_u2_lifo.RISEscene";
	Job* pJob = LoadScene( kU2Scene, tmp );
	Check( pJob != nullptr, "U2 fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const std::string docBeforeInsert = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );

		// Insert X (a fresh material `matX`).
		const char* newChunk = "lambertian_material\n{\nname matX\nreflectance white\n}\n";
		const SceneEditController::AgentCommitResult ins = c.ApplyAgentInsertChunk( String( newChunk ), nullptr );
		Check( ins.applied, "insert of `matX` applies" );
		const std::string docAfterInsert = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );

		// Param edit on X: rebind `matX.reflectance` (currently absent -> prevValueWasAbsent
		// is false here since `reflectance` IS present in the authored chunk text -- this is
		// a plain value-edit, not an insert-of-absent-param).
		const SceneEditController::AgentCommitResult edit =
			c.ApplyAgentParamEdit( String( "matX" ), String( "material" ), String( "reflectance" ), String( "white" ), nullptr );
		Check( edit.applied, "param edit on `matX` applies (no-op VALUE change, still a real Document edit -- occ 0 re-SET)" );
		const std::string docAfterParamEdit = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );

		// Undo (param): reverts the param edit -- Document matches post-insert-pre-edit state.
		c.Undo();
		Check( RISE::Cst::SerializeCst( *pJob->GetCstDocument() ) == docAfterInsert,
		       "Undo #1 (param edit) restores the post-insert Document byte-identically" );

		// Undo (insert): matX is gone -- Document matches the ORIGINAL pre-insert state.
		c.Undo();
		Check( RISE::Cst::SerializeCst( *pJob->GetCstDocument() ) == docBeforeInsert,
		       "Undo #2 (insert) restores the ORIGINAL pre-insert Document byte-identically" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matX" ) == nullptr,
		       "matX is gone from the derived scene after both undos" );

		// Redo x2 restores both, in order.
		c.Redo();
		Check( RISE::Cst::SerializeCst( *pJob->GetCstDocument() ) == docAfterInsert,
		       "Redo #1 (re-insert) restores the post-insert Document byte-identically" );
		c.Redo();
		Check( RISE::Cst::SerializeCst( *pJob->GetCstDocument() ) == docAfterParamEdit,
		       "Redo #2 (re-apply the param edit) restores the post-edit Document byte-identically" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matX" ) != nullptr,
		       "matX is back in the derived scene after both redos" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 25: Undo-of-insert while the inserted chunk became REFERENCED
// afterward (another chunk now names it) -- the derivability gate (the same
// dry-run-guarded Job::ApplyCstRemoveChunk every manual remove_chunk uses)
// must refuse honestly: the entry stays on the undo stack, the Document is
// untouched, and the controller remains usable afterward.
//////////////////////////////////////////////////////////////////////
static void TestUndoInsertRefusedWhenReferenced()
{
	std::cout << "Test 25: Undo-of-insert refused when the chunk became referenced afterward (shared-undo U2 gate)..." << std::endl;

	const char* tmp = "agentlive_u2_gate.RISEscene";
	Job* pJob = LoadScene( kU2Scene, tmp );
	Check( pJob != nullptr, "U2 fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// Insert a fresh material `matRef` -- the ONLY entry this test puts on the
		// undo stack (its Undo is the one under test).
		const char* newChunk = "lambertian_material\n{\nname matRef\nreflectance white\n}\n";
		const SceneEditController::AgentCommitResult ins = c.ApplyAgentInsertChunk( String( newChunk ), nullptr );
		Check( ins.applied, "insert of `matRef` applies" );
		const std::string docAfterInsert = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		const unsigned int undoDepthAfterInsert = c.Editor().History().UndoDepth();

		// Retarget `obj.material` to `matRef` -- making it REFERENCED -- via the
		// SAME direct-Job + RunPreviewRenderParked + manual-rebind idiom Test 19
		// uses for its "independent mechanism, no history footprint" edits.  This
		// is deliberate: if the retarget instead pushed its OWN undo-stack entry
		// (e.g. via SetPropertyForCategory), it would sit ON TOP of the insert and
		// strict LIFO would force US to pop (and thereby un-reference) it before
		// ever reaching the insert's Undo -- which would prove nothing about the
		// gate.  Routing around history the same way an out-of-band mechanism
		// would (another agent session, a script) is what lets this test attempt
		// the insert's Undo while the reference is STILL standing.
		Check( c.RunPreviewRenderParked( [&]{
			const int rc = pJob->ApplyCstParamEditChecked( "obj", "standard_object", "material", 0, "matRef" );
			Check( rc == 1 || rc == 2 || rc == 3, "retargeting `obj.material` to `matRef` applies (no history pushed)" );
			if( rc >= 2 ) {
				// Defensive: a D2 replace would require the same manual rebind Test 19
				// performs (RebindEditorToJob is private) -- not expected here (a plain
				// reference retarget on a non-variant scene re-derives incrementally),
				// but reproduce it if it ever does so the controller stays usable.
				c.Editor().RebindScene( *pJob->GetScene() );
				c.Editor().SetMaterialManager( pJob->GetMaterials() );
				c.Editor().SetShaderManager( pJob->GetShaders() );
				c.Editor().SetPainterManager( pJob->GetPainters() );
				c.Editor().SetScalarPainterManager( pJob->GetScalarPainters() );
				c.Editor().SetJob( pJob );
			}
		} ), "the parked direct-Job retarget ran" );
		Check( c.Editor().History().UndoDepth() == undoDepthAfterInsert,
		       "the direct-Job retarget pushed NO history of its own -- the insert is still the ONLY undo entry" );
		Check( pJob->GetObjects() && pJob->GetObjects()->GetItem( "obj" ) != nullptr,
		       "`obj` still resolves after the retarget" );
		{
			const std::string doc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			Check( doc.find( "material matRef" ) != std::string::npos,
			       "the Document shows `obj` now referencing `matRef`" );
		}

		// Attempt to Undo the insert while `matRef` IS referenced: the derivability
		// gate (RouteAgentChunkCrud_'s insert-Undo arm -> Job::ApplyCstRemoveChunk,
		// the SAME dry-run-guarded primitive a manual remove_chunk uses) must refuse
		// honestly -- the entry stays on the undo stack (no divergence between the
		// stack and the Document), the Document is UNTOUCHED, and the controller
		// remains usable afterward (a subsequent, unrelated edit still works).
		const std::string docBeforeUndoAttempt = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		c.Undo();
		const std::string docAfterUndoAttempt = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( docAfterUndoAttempt == docBeforeUndoAttempt,
		       "RED-PROVE: the refused Undo left the Document BYTE-IDENTICAL (no partial/silent mutation)" );
		Check( c.Editor().History().UndoDepth() == undoDepthAfterInsert,
		       "RED-PROVE: the insert's entry is STILL on the undo stack (honest bookkeeping -- not silently dropped)" );
		Check( c.Editor().History().RedoDepth() == 0,
		       "the refused Undo did NOT move anything onto the redo stack" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matRef" ) != nullptr,
		       "matRef is untouched (still resolvable) after the refused Undo" );

		// Controller usable after: an unrelated edit still applies cleanly.
		const SceneEditController::AgentCommitResult after =
			c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ), String( "scale" ), String( "9.0" ), nullptr );
		Check( after.applied, "the controller remains usable after the refused Undo (an unrelated edit still applies)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

// A scene fixture that AUTHORS a rasterizer chunk (kU2Scene/kBaseScene rely on the
// engine's built-in default rasterizer and never declare one) -- needed for Test 26,
// which inserts a SECOND rasterizer and checks GetActiveRasterizerName() live == a
// fresh reload of the Document, both before and after Undo.
static const char* kU2RasterizerScene =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
	"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
	"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n"
	"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n"
	"film\n{\nwidth 16\nheight 16\n}\n"
	"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n"
	"sphere_geometry\n{\nname s\nradius 1\n}\n"
	"standard_object\n{\nname obj\ngeometry s\nmaterial lum\nshader global\n}\n";

//////////////////////////////////////////////////////////////////////
// Test 26: rasterizer insert -> Undo -> the ACTIVE rasterizer matches a
// FRESH RELOAD of the resulting Document (the live==reload invariant).
// Forward insert of a rasterizer-class chunk activates it (P1-B: the D2
// activation restore is skipped for a rasterizer insert -- see
// Job::ApplyCstInsertChunk); Undo removes it, and the pre-existing
// `pathtracing_pel_rasterizer` becomes the (last-and-only) active one again,
// via Job::ApplyCstRemoveChunk's ordinary activation-restore path (Undo of an
// insert routes through the SAME primitive a manual remove_chunk uses -- it
// is NOT itself a rasterizer-class insert, so no special-casing is needed on
// the Undo side; the invariant this test proves is that the ORDINARY
// restore-active-rasterizer path already gives the right answer).
//////////////////////////////////////////////////////////////////////
static void TestRasterizerInsertUndoMatchesReload()
{
	std::cout << "Test 26: rasterizer insert Undo -- active rasterizer matches a fresh reload (shared-undo U2)..." << std::endl;

	const char* tmp = "agentlive_u2_rasterizer.RISEscene";
	Job* pJob = LoadScene( kU2RasterizerScene, tmp );
	Check( pJob != nullptr, "rasterizer fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		Check( pJob->GetActiveRasterizerName() == "pathtracing_pel_rasterizer",
		       "the authored pathtracing rasterizer is active on load" );

		// Insert a DIFFERENT-keyword rasterizer -- it becomes active (P1-B).
		const SceneEditController::AgentCommitResult ins =
			c.ApplyAgentInsertChunk( String( "bdpt_pel_rasterizer\n{\nsamples 2\n}\n" ), nullptr );
		Check( ins.applied, "inserting a different-keyword rasterizer applies" );
		Check( pJob->GetActiveRasterizerName() == "bdpt_pel_rasterizer",
		       "the inserted rasterizer is ACTIVE live" );

		// Undo the insert -- the pre-existing pathtracing rasterizer must be active
		// again, AND a fresh reload of the (now-reverted) Document must agree.
		c.Undo();
		Check( pJob->GetActiveRasterizerName() == "pathtracing_pel_rasterizer",
		       "Undo restores the pre-existing rasterizer as ACTIVE live" );
		{
			const std::string bytes = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			const char* tmp2 = "agentlive_u2_rasterizer_reload.RISEscene";
			Job* fresh = LoadScene( bytes.c_str(), tmp2 );
			Check( fresh != nullptr, "the post-Undo head reloads cleanly" );
			if( fresh ) {
				Check( fresh->GetActiveRasterizerName() == pJob->GetActiveRasterizerName(),
				       "live == reload: the fresh reload activates the SAME rasterizer Undo left active live" );
				Check( fresh->GetActiveRasterizerName() == "pathtracing_pel_rasterizer",
				       "specifically: the reload activates pathtracing_pel_rasterizer (bdpt is gone)" );
				fresh->release();
			}
			std::remove( tmp2 );
		}

		// Redo the insert -- bdpt becomes active again live AND on reload.
		c.Redo();
		Check( pJob->GetActiveRasterizerName() == "bdpt_pel_rasterizer",
		       "Redo re-activates the inserted rasterizer live" );
		{
			const std::string bytes = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			const char* tmp2 = "agentlive_u2_rasterizer_reload2.RISEscene";
			Job* fresh = LoadScene( bytes.c_str(), tmp2 );
			Check( fresh != nullptr, "the post-Redo head reloads cleanly" );
			if( fresh ) {
				Check( fresh->GetActiveRasterizerName() == "bdpt_pel_rasterizer",
				       "live == reload after Redo too" );
				fresh->release();
			}
			std::remove( tmp2 );
		}

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 27: code-3 (diagnosed-but-mutated) armed through chunk-CRUD Undo AND
// Redo -- the stack transfer must complete cleanly (entry moves to redo on
// Undo, back to undo on Redo), the Document ACTUALLY mutates each time
// (diagnosed is not a no-op), and a subsequent save->reload is clean (no
// wedge, no corruption left behind by the diagnosed path).
//////////////////////////////////////////////////////////////////////
static void TestCodeThreeArmedChunkCrudUndoRedo()
{
	std::cout << "Test 27: code-3 ARMED through chunk-CRUD Undo/Redo -- stack transfer completes, no wedge (shared-undo U2)..." << std::endl;

	const char* tmp = "agentlive_u2_c3.RISEscene";
	{ std::ofstream o( tmp ); o << kU2Scene; }
	CodeThreeJob* pJob = new CodeThreeJob();
	Check( pJob->LoadAsciiSceneViaCst( tmp ), "U2 fixture scene loads into CodeThreeJob via the CST path" );

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const unsigned int undoBefore = c.Editor().History().UndoDepth();

		// Forward insert: force-code-three is ARMED (CodeThreeJob's ctor default),
		// so this lands as a code-3 diagnosed-but-mutated commit.
		const char* newChunk = "lambertian_material\n{\nname matC3\nreflectance white\n}\n";
		const SceneEditController::AgentCommitResult ins = c.ApplyAgentInsertChunk( String( newChunk ), nullptr );
		Check( !ins.applied && ins.rawCode == 3, "the forward insert is a code-3 diagnosed-but-mutated commit" );
		Check( c.Editor().History().UndoDepth() == undoBefore + 1, "the code-3 forward insert still pushed a history record" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matC3" ) != nullptr,
		       "the live derived scene DID gain `matC3` despite the diagnosed code" );

		// Undo with force-code-three STILL ARMED -- RouteAgentChunkCrud_'s underlying
		// ApplyCstRemoveChunk call ALSO returns 3 here (CodeThreeJob rewrites every
		// clean 2 -> 3, including the Undo-of-insert's remove).
		c.Undo();
		Check( c.Editor().History().UndoDepth() == undoBefore,
		       "the undo stack depth dropped back to baseline (the diagnosed revert counted as 'the mutation landed')" );
		Check( c.Editor().History().RedoDepth() == 1,
		       "the reverted entry landed on the redo stack (not stuck on / restored to undo)" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matC3" ) == nullptr,
		       "Undo's diagnosed-but-mutated revert DID remove matC3 from the live derived scene" );
		{
			const std::string doc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
			Check( doc.find( "matC3" ) == std::string::npos,
			       "the reverted Document no longer mentions matC3 (byte-level proof the revert landed)" );
		}

		// Redo must work (not double-revert, not wedge). Still armed.
		c.Redo();
		Check( c.Editor().History().UndoDepth() == undoBefore + 1, "Redo moved the entry back to the undo stack" );
		Check( c.Editor().History().RedoDepth() == 0, "redo stack drained after the single Redo" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matC3" ) != nullptr,
		       "Redo re-inserted matC3 into the live derived scene" );

		// Repeated Cmd-Z (Undo again) must revert ONCE more, cleanly -- no
		// double-revert / wedge artifact from the diagnosed-code stack transfer.
		c.Undo();
		Check( c.Editor().History().UndoDepth() == undoBefore, "the second Undo again drops to baseline" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matC3" ) == nullptr,
		       "the second Undo again removes matC3 (no double-revert artifact)" );

		// Save -> reload clean: disarm force-code-three first (Save/reload should
		// reflect a CLEAN state, matching Test 14/20's convention of disarming
		// before an operation whose own correctness isn't what's under test).
		pJob->SetForceCodeThree( false );
		const char* saveAs = "agentlive_u2_c3_save.RISEscene";
		std::remove( saveAs );
		const SaveResult sr = c.RequestSave( std::string( saveAs ) );
		Check( Succeeded( sr.status ), "save succeeds after the diagnosed-code Undo/Redo/Undo sequence" );
		{
			Job* fresh = new Job();
			Check( fresh->LoadAsciiSceneViaCst( saveAs ), "the saved Document reloads cleanly (no wedge left behind)" );
			Check( fresh->GetMaterials() && fresh->GetMaterials()->GetItem( "matC3" ) == nullptr,
			       "the reloaded scene correctly lacks matC3 (matches the final undone state)" );
			fresh->release();
		}
		std::remove( saveAs );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 28 (round-1 P1-A red-prove): insert a VARIANT-tagged overlay chunk
// that legitimately shares (kind,name) with an EXISTING base chunk (matA) --
// ApplyCstInsertChunk's own duplicate rule permits this (only an EXACT
// (kind,name,variant) duplicate is refused).  Pre-fix, the controller
// re-resolved the freshly-inserted chunk's index via DocFindByNameAnyRole,
// which sees TWO (kind,name) matches (the base + the overlay) and -- with no
// kind hint to narrow by (both are `lambertian_material`) -- correctly
// returns 0 (ambiguous).  `insertedIndex` stayed -1, and the pre-fix fallback
// `(insertedIndex >= 1) ? (insertedIndex - 1) : 0` pushed a FABRICATED index
// of 0.  Undo then called ApplyCstRemoveItemsAt(0, 3) -- which bounds-checks
// but (pre-P2-fix) has NO identity check -- silently deleting the document's
// FIRST three top-level items (the `white` painter + part of `lum`), NOT the
// overlay.  Post-fix: ApplyCstInsertChunk's own `outInsertedAt` out-param
// reports the EXACT splice position, so Undo removes exactly the overlay
// triple, byte-identical to pre-insert, with the base chunk and every
// unrelated item (including the first three) completely untouched.
//////////////////////////////////////////////////////////////////////
static void TestVariantOverlayInsertUndoP1A()
{
	std::cout << "Test 28: insert a variant overlay sharing (kind,name) with a base chunk -> Undo removes ONLY the overlay, byte-exact (round-1 P1-A)..." << std::endl;

	const char* tmp = "agentlive_p1a_variant.RISEscene";
	Job* pJob = LoadScene( kU2Scene, tmp );
	Check( pJob != nullptr, "U2 fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const std::string preDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( preDoc.find( "matA" ) != std::string::npos, "matA (the base chunk this overlay will shadow) is present pre-edit" );

		// Insert a VARIANT overlay of `matA` -- SAME (kind,name) as the existing base chunk,
		// but tagged with a `variant` param, so ApplyCstInsertChunk's duplicate check exempts
		// it (only an EXACT (kind,name,variant) triple is refused).  This is the scenario the
		// P1-A bug's own doc names explicitly: "a variant-tagged overlay chunk legitimately
		// shares its base chunk's (kind,name)".
		const char* overlayChunk =
			"lambertian_material\n{\nname matA\nvariant night\nreflectance white\n}\n";
		const SceneEditController::AgentCommitResult ins = c.ApplyAgentInsertChunk( String( overlayChunk ), nullptr );
		Check( ins.applied, "insert of the `matA` variant overlay applies" );
		Check( ins.chunkName == String( "matA" ), "insert echoes the resolved (shared) name `matA`" );
		const std::string postInsertDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postInsertDoc != preDoc, "the Document actually changed" );
		{
			// RED-PROVE premise: confirm the ambiguity DocFindByNameAnyRole would hit is real --
			// two top-level chunks now carry the "material/matA" name path (base + overlay).
			int occ = 0;
			const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
				*pJob->GetCstDocument(), "matA", &occ, "material", /*uniqueFallback*/ false );
			Check( id == 0 && occ == 2,
			       "RED-PROVE premise: post-insert, `matA` is ambiguous by (name,kind) alone (2 matches: base + overlay) -- "
			       "the pre-fix post-hoc re-resolution this test guards against would have failed exactly this way" );
		}

		// Undo -> byte-identical to the pre-insert Document.  At the pre-fix parent, this
		// instead deletes the Document's FIRST three top-level items (a fabricated index of
		// 0), which would corrupt the `white` painter chunk and part of `lum` -- assert
		// BOTH that we get the byte-identical restore AND that the base chunk (and the
		// scene's first items) are untouched, so a regression back to the pre-fix behaviour
		// is caught even if the byte-identity check alone were somehow satisfied by chance.
		c.Undo();
		const std::string postUndoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postUndoDoc == preDoc, "Undo of the variant-overlay insert restores the Document BYTE-IDENTICAL to pre-insert" );
		Check( postUndoDoc.find( "uniformcolor_painter" ) != std::string::npos && postUndoDoc.find( "name white" ) != std::string::npos,
		       "the `white` painter chunk (one of the pre-fix fabricated-index victims) survives intact" );
		Check( postUndoDoc.find( "name lum" ) != std::string::npos && postUndoDoc.find( "scale 5.0" ) != std::string::npos,
		       "the `lum` luminaire chunk (another pre-fix fabricated-index victim) survives intact" );
		Check( postUndoDoc.find( "variant night" ) == std::string::npos, "the overlay's `variant night` tag is gone (the overlay itself was removed)" );
		{
			int occ = 0;
			const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
				*pJob->GetCstDocument(), "matA", &occ, "material", /*uniqueFallback*/ false );
			Check( id != 0 && occ == 1, "exactly ONE `matA` remains after Undo (the base -- unambiguous again)" );
		}
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matA" ) != nullptr,
		       "the derived scene still resolves `matA` (the base) after Undo" );

		// Redo -> re-inserts the overlay byte-exactly.
		c.Redo();
		const std::string postRedoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postRedoDoc == postInsertDoc, "Redo of the variant-overlay insert re-inserts BYTE-EXACTLY" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 29 (round-1 P1-B red-prove): agent REMOVE -> an OUT-OF-BAND direct-Job
// insert lands BEFORE the removed chunk's captured position (shifting every
// index at/after it by +1, the same "independent mechanism" idiom Test 19
// uses) -> Undo (of the remove) must be GLUE-SAFE: no `}`-immediately-
// followed-by-non-whitespace adjacency anywhere in the restored Document
// (the reviewer's scan), the Document still derives, and it survives a
// save+reload cleanly.  Pre-fix, ApplyCstRestoreChunkAt spliced the captured
// bytes verbatim at the now-stale (shifted) index with no glue-safety check;
// this test's out-of-band insert is specifically an UNNAMED-chunk-adjacent
// splice engineered so the left neighbour at the (post-shift) restore index
// does NOT end in a newline, reproducing the `}<keyword>` glue the fix
// targets.
//////////////////////////////////////////////////////////////////////
static void TestGlueSafeRestoreAfterOutOfBandShiftP1B()
{
	std::cout << "Test 29: agent remove + out-of-band shift -> Undo restore is GLUE-SAFE (round-1 P1-B)..." << std::endl;

	const char* tmp = "agentlive_p1b_glue.RISEscene";
	Job* pJob = LoadScene( kU2Scene, tmp );
	Check( pJob != nullptr, "U2 fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// Agent removes matB (the middle material, sandwiched between matA and matC) --
		// captures its exact bytes + position under the shared-undo U2 machinery.
		const SceneEditController::AgentCommitResult rm =
			c.ApplyAgentRemoveChunk( String( "matB" ), String( "material" ), nullptr );
		Check( rm.applied, "remove_chunk on matB applies" );
		const unsigned int undoDepthAfterRemove = c.Editor().History().UndoDepth();

		// Out-of-band mechanism (Test-19-style: direct Job calls under RunPreviewRenderParked +
		// manual rebind, NO history footprint of its own): insert a FRESH painter chunk -- a
		// tier-0 (Painter/Function) declaration positions BEFORE every tier-1+ chunk (materials,
		// geometry, objects, ...), landing right after the scene header stray tokens, i.e. BEFORE
		// matB's captured position -- shifting every later top-level index (incl. matB's captured
		// one) down by exactly the 3 items ApplyCstInsertChunk's [leadSep][chunk][trailSep] triple
		// adds.  The shifted index then lands on the FIRST post-shift item at that position, whose
		// preceding neighbour is NOT newline-terminated (a Chunk item's own serialized bytes end in
		// `}`, not `\n` -- the inter-chunk separator is a SEPARATE Trivia item) unless the shift
		// happens to land exactly on a Trivia boundary -- engineered below to land mid-chunk-run.
		Check( c.RunPreviewRenderParked( [&]{
			char kw[64]; char nm[64]; char diag[256];
			const char* freshChunk = "uniformcolor_painter\n{\nname pShift\ncolor 0.2 0.2 0.2\n}\n";
			const int insCode = pJob->ApplyCstInsertChunk( freshChunk, kw, sizeof( kw ), nm, sizeof( nm ), diag, sizeof( diag ) );
			Check( insCode == 2 || insCode == 3, "the out-of-band shifting insert of `pShift` succeeds (no history pushed)" );

			c.Editor().RebindScene( *pJob->GetScene() );
			c.Editor().SetMaterialManager( pJob->GetMaterials() );
			c.Editor().SetShaderManager( pJob->GetShaders() );
			c.Editor().SetPainterManager( pJob->GetPainters() );
			c.Editor().SetScalarPainterManager( pJob->GetScalarPainters() );
			c.Editor().SetJob( pJob );
		} ), "the parked out-of-band shifting insert + manual rebind ran" );
		Check( c.Editor().History().UndoDepth() == undoDepthAfterRemove,
		       "the out-of-band insert pushed NO history of its own -- the agent's remove entry is still the next one to pop" );

		// Undo the ORIGINAL agent remove: the captured `agentChunkIndex` is now STALE (shifted by
		// the out-of-band insert) -- the glue-safety fix must still produce a WELL-FORMED Document
		// (no `}` immediately followed by a non-whitespace byte anywhere), even though byte-identity
		// to the pre-remove Document is no longer the bar under a genuine shift.
		c.Undo();
		const std::string postUndoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postUndoDoc.find( "matB" ) != std::string::npos, "matB's text is back in the Document after Undo" );
		Check( postUndoDoc.find( "pShift" ) != std::string::npos, "the out-of-band `pShift` chunk survives the restore" );

		// The reviewer's scan: no `}` immediately followed by a non-whitespace byte anywhere in
		// the whole Document (that is exactly the `}lambertian_material`-style glue corruption
		// the fix targets -- a NEWLINE, space, or end-of-string may follow `}`; a bare letter/
		// digit/underscore immediately after `}` never should).
		bool foundGlue = false;
		size_t glueAt = std::string::npos;
		for( size_t p = postUndoDoc.find( '}' ); p != std::string::npos; p = postUndoDoc.find( '}', p + 1 ) )
		{
			if( p + 1 < postUndoDoc.size() )
			{
				const char next = postUndoDoc[p + 1];
				if( next != '\n' && next != '\r' && next != ' ' && next != '\t' )
				{
					foundGlue = true;
					glueAt = p;
					break;
				}
			}
		}
		Check( !foundGlue, "RED-PROVE: no `}`-followed-by-non-whitespace glue adjacency anywhere in the restored Document" );
		if( foundGlue )
			std::cout << "    glue found at byte " << glueAt << ": ..." << postUndoDoc.substr( glueAt, 24 ) << "..." << std::endl;

		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matB" ) != nullptr,
		       "the derived scene resolves `matB` again after Undo" );
		Check( pJob->GetPainters() && pJob->GetPainters()->GetItem( "pShift" ) != nullptr,
		       "the derived scene still resolves the out-of-band `pShift` too" );

		// Save + reload clean (no wedge, no corruption left behind).
		const char* saveAs = "agentlive_p1b_glue_save.RISEscene";
		std::remove( saveAs );
		const SaveResult sr = c.RequestSave( std::string( saveAs ) );
		Check( Succeeded( sr.status ), "save succeeds after the glue-safe restore" );
		{
			Job* fresh = new Job();
			Check( fresh->LoadAsciiSceneViaCst( saveAs ), "the saved Document reloads CLEANLY (no wedge, no corruption)" );
			Check( fresh->GetMaterials() && fresh->GetMaterials()->GetItem( "matB" ) != nullptr, "the reload resolves `matB`" );
			Check( fresh->GetPainters() && fresh->GetPainters()->GetItem( "pShift" ) != nullptr, "the reload resolves `pShift`" );
			fresh->release();
		}
		std::remove( saveAs );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 30 (round-1 P2 red-prove): agent INSERT -> an out-of-band shift
// replaces the recorded triple's content with DIFFERENT (but still
// well-formed, still-derivable) content -- specifically, deletes an
// UNREFERENCED painter chunk positioned such that the deletion shifts the
// inserted chunk's triple to start exactly where a DIFFERENT, unrelated
// chunk (`matC`) now sits -> Undo must HONESTLY REFUSE (Document
// byte-unchanged, the entry stays on the undo stack, the controller remains
// usable) instead of silently deleting whatever three items now occupy that
// position.  Pre-fix, ApplyCstRemoveItemsAt is a pure positional splice with
// NO identity check -- since the wrong triple at the shifted position is
// itself well-formed and unreferenced, its own dry-run derives CLEANLY, so
// the pre-fix primitive would silently commit deleting matC (the WRONG
// chunk) instead of refusing.  Post-fix, the caller's captured exact chunk
// bytes (`SceneEdit::propertyValue`) are compared against whatever now sits
// at the middle of the recorded triple BEFORE any Document mutation, and a
// mismatch refuses with code 0.
//////////////////////////////////////////////////////////////////////
static void TestStaleIndexIdentityCheckRefusesP2()
{
	std::cout << "Test 30: agent insert + out-of-band shift onto DIFFERENT content -> Undo honestly REFUSES (round-1 P2)..." << std::endl;

	const char* tmp = "agentlive_p2_identity.RISEscene";
	Job* pJob = LoadScene( kU2Scene, tmp );
	Check( pJob != nullptr, "U2 fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// Insert a fresh, UNREFERENCED material `matZ` -- declaration-tier positioning lands it
		// appended after matC (materials have no consumers in kU2Scene to force an earlier slot),
		// so its triple sits AFTER matC's own chunk.  Capture the pre-insert layout so we can find
		// a shift that lands EXACTLY on matC's own position.
		const std::string preInsertDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		const char* newChunk = "lambertian_material\n{\nname matZ\nreflectance white\n}\n";
		const SceneEditController::AgentCommitResult ins = c.ApplyAgentInsertChunk( String( newChunk ), nullptr );
		Check( ins.applied, "insert of `matZ` applies" );
		const unsigned int undoDepthAfterInsert = c.Editor().History().UndoDepth();

		// Locate matZ's recorded triple-start index via the SAME resolution the controller uses
		// internally (round-1 P1-A's own out-param already proved this -- re-derive it here for
		// the test's own bookkeeping by counting top-level items before/after via the Document).
		const RISE::Cst::Document* docAfterInsert = pJob->GetCstDocument();
		int occ = 0;
		const RISE::Cst::NodeId matZId = RISE::Cst::DocFindByNameAnyRole( *docAfterInsert, "matZ", &occ, "material", false );
		Check( matZId != 0 && occ == 1, "matZ resolves uniquely post-insert" );
		const int matZIdx = RISE::Cst::DocIndexOfNodeId( *docAfterInsert, matZId, nullptr );
		Check( matZIdx >= 1, "matZ has a valid top-level index (its own leading separator sits one before it)" );

		// Out-of-band mechanism (Test-19-style: direct Job + RunPreviewRenderParked + manual
		// rebind, no history footprint): remove `white` (the shared, UNREFERENCED-once-materials-
		// are-gone... actually `white` IS referenced by matA/matB/matC/matZ's `reflectance`, so
		// removing it would fail the dry-run) -- instead remove `matC` (a plain, unreferenced-
		// by-anything-else material) OUTRIGHT, which shifts every index after it (incl. matZ's
		// triple) DOWN by however many top-level items matC's erase drops (1 or 2).  After this
		// shift, matZ's ORIGINAL recorded triple-start index now points at whatever used to sit
		// AFTER matZ's own triple -- in this fixture, that is the Document's tail (nothing, or
		// trailing trivia) since matZ was appended last; to land the stale index ON a DIFFERENT
		// but well-formed chunk instead of past-the-end, remove `matA` (the FIRST material)
		// instead -- shifting matZ's triple (and everything else) DOWN, so the ORIGINAL
		// (pre-shift) triple-start index now addresses content that used to be one triple later:
		// exactly `matB`'s triple survives the shift and slides into matZ's old slot's neighbourhood.
		// The precise landing content does not matter for this test's assertion (any DIFFERENT,
		// well-formed, derivable triple proves the point) -- what matters is that it is NOT matZ.
		Check( c.RunPreviewRenderParked( [&]{
			char kw[64]; char diag[256];
			const int rmCode = pJob->ApplyCstRemoveChunk( "matA", "material", kw, sizeof( kw ), diag, sizeof( diag ) );
			Check( rmCode == 2 || rmCode == 3, "the out-of-band removal of `matA` succeeds (no history pushed)" );

			c.Editor().RebindScene( *pJob->GetScene() );
			c.Editor().SetMaterialManager( pJob->GetMaterials() );
			c.Editor().SetShaderManager( pJob->GetShaders() );
			c.Editor().SetPainterManager( pJob->GetPainters() );
			c.Editor().SetScalarPainterManager( pJob->GetScalarPainters() );
			c.Editor().SetJob( pJob );
		} ), "the parked out-of-band removal of `matA` + manual rebind ran" );
		Check( c.Editor().History().UndoDepth() == undoDepthAfterInsert,
		       "the out-of-band removal pushed NO history of its own -- the agent's insert entry is still the next one to pop" );

		const std::string docBeforeUndoAttempt = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( docBeforeUndoAttempt.find( "name matA" ) == std::string::npos, "matA is confirmed gone (the out-of-band shift landed)" );
		Check( docBeforeUndoAttempt.find( "name matZ" ) != std::string::npos, "matZ is still present (Undo hasn't run yet)" );

		// Attempt Undo of the matZ insert: `agentChunkIndex` is now STALE (matA's removal shifted
		// every later index).  The identity check must catch that whatever now sits at the
		// recorded position is NOT matZ's chunk bytes, and refuse honestly -- Document
		// byte-unchanged, entry stays on the undo stack, controller stays usable.
		c.Undo();
		const std::string docAfterUndoAttempt = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( docAfterUndoAttempt == docBeforeUndoAttempt,
		       "RED-PROVE: the refused Undo left the Document BYTE-IDENTICAL (no partial/silent mutation, and specifically "
		       "matZ was NOT wrongly deleted alongside whatever now occupies its old recorded index)" );
		Check( docAfterUndoAttempt.find( "name matZ" ) != std::string::npos,
		       "RED-PROVE: matZ (the chunk the agent actually inserted) is STILL present -- the identity-checked Undo "
		       "refused rather than deleting whatever now sits at the stale index" );
		Check( c.Editor().History().UndoDepth() == undoDepthAfterInsert,
		       "RED-PROVE: the insert's entry is STILL on the undo stack (honest bookkeeping -- not silently dropped)" );
		Check( c.Editor().History().RedoDepth() == 0, "the refused Undo did NOT move anything onto the redo stack" );

		// Controller usable after: an unrelated edit still applies cleanly.
		const SceneEditController::AgentCommitResult after =
			c.ApplyAgentParamEdit( String( "lum" ), String( "lambertian_luminaire_material" ), String( "scale" ), String( "9.0" ), nullptr );
		Check( after.applied, "the controller remains usable after the refused Undo (an unrelated edit still applies)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 31 (round-2 P1 red-prove): the reviewer's exact repro shape -- a chunk
// (matTarget) glued DIRECTLY (zero whitespace) onto its LEFT neighbour (matA),
// so DocEraseChunkTidy's glue-safety walk (Cst.cpp, ItemEndsInNewline on the
// item BEFORE the removed chunk) finds the preceding item does NOT end in a
// newline and refuses to collapse matTarget's own trailing separator ->
// droppedCount == 1 -> SceneEditController's post-remove trim keeps ONLY
// matTarget's own chunk bytes (ending in `}`, never `\n`) as the Undo capture.
// An out-of-band insert then shifts indices so a DIFFERENT, non-whitespace-
// leading chunk lands immediately to the RIGHT of the restore point -> Undo
// must still be glue-safe on the RIGHT side (the round-1 fix only guarded the
// LEFT).  Pre-fix (round-1 only), this reproduces `}lambertian_luminaire_material`-
// style glue; post-fix (round-2), the symmetric right-side synthesis catches it.
//////////////////////////////////////////////////////////////////////
static void TestGlueSafeRestoreRightSideAfterOutOfBandShiftP1Round2()
{
	std::cout << "Test 31: zero-whitespace-glued neighbour forces droppedCount==1 -> out-of-band shift -> Undo restore is RIGHT-side GLUE-SAFE (round-2 P1)..." << std::endl;

	// matA's chunk ends `}` immediately followed (ZERO whitespace) by matTarget's
	// chunk -- the reviewer's engineered glue-unsafe left neighbour.  matTarget is
	// followed by the ORDINARY `\n`-separated matB, so a normal trailing separator
	// exists there but -- per the glueSafe rule -- DocEraseChunkTidy will decline to
	// collapse it (the check is on the PRECEDING item, i.e. matA's chunk, which does
	// NOT end in `\n`), leaving it in the Document and giving droppedCount == 1.
	const char* kGlueScene =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
		"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
		"lambertian_material\n{\nname matA\nreflectance white\n}"
		"lambertian_material\n{\nname matTarget\nreflectance white\n}\n"
		"lambertian_material\n{\nname matB\nreflectance white\n}\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n";

	const char* tmp = "agentlive_p1_round2_right_glue.RISEscene";
	Job* pJob = LoadScene( kGlueScene, tmp );
	Check( pJob != nullptr, "the zero-whitespace-glued fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		const int preRemoveCount = RISE::Cst::DocItemCount( *pJob->GetCstDocument() );

		const SceneEditController::AgentCommitResult rm =
			c.ApplyAgentRemoveChunk( String( "matTarget" ), String( "material" ), nullptr );
		Check( rm.applied, "remove_chunk on the glue-unsafe-left-neighbour matTarget applies" );
		const unsigned int undoDepthAfterRemove = c.Editor().History().UndoDepth();

		const int postRemoveCount = RISE::Cst::DocItemCount( *pJob->GetCstDocument() );
		Check( preRemoveCount - postRemoveCount == 1,
		       "RED-PROVE PRECONDITION: the erase dropped exactly 1 item (droppedCount==1 -- matTarget's own "
		       "trailing separator was NOT collapsed because its LEFT neighbour matA does not end in a newline), "
		       "so the Undo capture is chunk-ONLY bytes ending in `}` with no trailing separator of its own" );

		// Out-of-band mechanism (Test-19/29-style: direct Job call under RunPreviewRenderParked +
		// manual rebind, NO history footprint): insert a fresh tier-0 Painter chunk, which positions
		// BEFORE every tier-1+ chunk (materials, geometry, objects...) -- landing before matTarget's
		// captured restore position and shifting every later top-level index (incl. the restore point)
		// down by the 3 items ApplyCstInsertChunk's [leadSep][chunk][trailSep] triple adds.  After the
		// shift, the item that lands immediately at the (shifted) restore point is matB's own chunk --
		// its serialized bytes start with `lambertian_material`, i.e. NOT whitespace -- reproducing the
		// reviewer's exact right-side glue hazard.
		Check( c.RunPreviewRenderParked( [&]{
			char kw[64]; char nm[64]; char diag[256];
			const char* freshChunk = "uniformcolor_painter\n{\nname pShiftR2\ncolor 0.3 0.3 0.3\n}\n";
			const int insCode = pJob->ApplyCstInsertChunk( freshChunk, kw, sizeof( kw ), nm, sizeof( nm ), diag, sizeof( diag ) );
			Check( insCode == 2 || insCode == 3, "the out-of-band shifting insert of `pShiftR2` succeeds (no history pushed)" );

			c.Editor().RebindScene( *pJob->GetScene() );
			c.Editor().SetMaterialManager( pJob->GetMaterials() );
			c.Editor().SetShaderManager( pJob->GetShaders() );
			c.Editor().SetPainterManager( pJob->GetPainters() );
			c.Editor().SetScalarPainterManager( pJob->GetScalarPainters() );
			c.Editor().SetJob( pJob );
		} ), "the parked out-of-band shifting insert + manual rebind ran" );
		Check( c.Editor().History().UndoDepth() == undoDepthAfterRemove,
		       "the out-of-band insert pushed NO history of its own -- the agent's remove entry is still the next one to pop" );

		// Undo the ORIGINAL agent remove: the captured `agentChunkIndex` is now STALE (shifted by the
		// out-of-band insert), and the capture itself is chunk-only (no trailing separator) -- exactly
		// the shape that needs the ROUND-2 right-side synthesis to avoid `}`-followed-by-content glue.
		c.Undo();
		const std::string postUndoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postUndoDoc.find( "matTarget" ) != std::string::npos, "matTarget's text is back in the Document after Undo" );
		Check( postUndoDoc.find( "pShiftR2" ) != std::string::npos, "the out-of-band `pShiftR2` chunk survives the restore" );

		// The reviewer's scan: no `}` immediately followed by a non-whitespace byte anywhere in the
		// whole Document.
		bool foundGlue = false;
		size_t glueAt = std::string::npos;
		for( size_t p = postUndoDoc.find( '}' ); p != std::string::npos; p = postUndoDoc.find( '}', p + 1 ) )
		{
			if( p + 1 < postUndoDoc.size() )
			{
				const char next = postUndoDoc[p + 1];
				if( next != '\n' && next != '\r' && next != ' ' && next != '\t' )
				{
					foundGlue = true;
					glueAt = p;
					break;
				}
			}
		}
		Check( !foundGlue, "RED-PROVE: no `}`-followed-by-non-whitespace glue adjacency anywhere in the restored Document (round-2 right-side fix)" );
		if( foundGlue )
			std::cout << "    glue found at byte " << glueAt << ": ..." << postUndoDoc.substr( glueAt, 32 ) << "..." << std::endl;

		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matTarget" ) != nullptr,
		       "the derived scene resolves `matTarget` again after Undo" );
		Check( pJob->GetPainters() && pJob->GetPainters()->GetItem( "pShiftR2" ) != nullptr,
		       "the derived scene still resolves the out-of-band `pShiftR2` too" );

		// Save + reload clean (no wedge, no corruption left behind).
		const char* saveAs = "agentlive_p1_round2_right_glue_save.RISEscene";
		std::remove( saveAs );
		const SaveResult sr = c.RequestSave( std::string( saveAs ) );
		Check( Succeeded( sr.status ), "save succeeds after the glue-safe restore" );
		{
			Job* fresh = new Job();
			Check( fresh->LoadAsciiSceneViaCst( saveAs ), "the saved Document reloads CLEANLY (no wedge, no corruption)" );
			Check( fresh->GetMaterials() && fresh->GetMaterials()->GetItem( "matTarget" ) != nullptr, "the reload resolves `matTarget`" );
			Check( fresh->GetPainters() && fresh->GetPainters()->GetItem( "pShiftR2" ) != nullptr, "the reload resolves `pShiftR2`" );
			fresh->release();
		}
		std::remove( saveAs );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 32 (round-2 P1 red-prove): the SIMPLER common shape -- remove the LAST
// top-level chunk in the Document (no trailing separator EVER existed, so
// droppedCount == 1 unconditionally, per DocEraseChunkTidy's own "LAST-chunk"
// comment: after the chunk drop there is no item left at `index` to collapse).
// An out-of-band APPEND after it, then Undo, must still be glue-safe: the
// appended chunk now sits immediately to the right of the restore point.
//////////////////////////////////////////////////////////////////////
static void TestGlueSafeRestoreLastChunkAppendP1Round2()
{
	std::cout << "Test 32: remove LAST chunk + out-of-band append -> Undo restore is GLUE-SAFE (round-2 P1)..." << std::endl;

	// A scene whose file bytes end EXACTLY at the last chunk's closing `}` -- NO trailing
	// newline at all after it -- so there is truly NO item left in the Document after `obj`
	// once it is removed (DocEraseChunkTidy's own "LAST-chunk" comment: after the chunk drop
	// there is no item AT `index` to collapse).  kU2Scene (used elsewhere in this file) has a
	// trailing `\n` after its final chunk, which parses to a SEPARATE trailing Trivia item
	// that DOES get collapsed alongside the chunk (droppedCount==2 there) -- deliberately
	// avoided here to isolate the genuinely-no-separator-ever-existed shape.
	const char* kLastChunkScene =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
		"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
		"lambertian_material\n{\nname matA\nreflectance white\n}\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}";

	const char* tmp = "agentlive_p1_round2_lastchunk.RISEscene";
	Job* pJob = LoadScene( kLastChunkScene, tmp );
	Check( pJob != nullptr, "the no-trailing-newline fixture scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// `obj` (standard_object) is the LAST top-level chunk with NO trailing separator --
		// remove it (nothing references it, so the dry-run derives cleanly).
		const int preRemoveCount = RISE::Cst::DocItemCount( *pJob->GetCstDocument() );
		const SceneEditController::AgentCommitResult rm =
			c.ApplyAgentRemoveChunk( String( "obj" ), String( "object" ), nullptr );
		Check( rm.applied, "remove_chunk on the LAST chunk (obj) applies" );
		const unsigned int undoDepthAfterRemove = c.Editor().History().UndoDepth();

		const int postRemoveCount = RISE::Cst::DocItemCount( *pJob->GetCstDocument() );
		Check( preRemoveCount - postRemoveCount == 1,
		       "RED-PROVE PRECONDITION: the erase dropped exactly 1 item (droppedCount==1 -- `obj` was the LAST "
		       "top-level item, so no trailing separator ever existed to collapse or capture)" );

		// Out-of-band mechanism: append a fresh chunk AFTER the (now-last) remaining item, landing
		// exactly at the position `obj`'s restore will target -- no index SHIFT is even needed here
		// (the new chunk is appended past the current end, i.e. at the same index the removed `obj`
		// used to occupy), which is exactly the "remove the last chunk, append, Undo" common shape
		// called out in the bug report.  ApplyCstInsertChunk ALWAYS synthesizes its own
		// [leadSep][chunk][trailSep] triple (the lead separator is a pure `"\n"` Trivia), which would
		// itself be glue-safe on the right of the restore point regardless of this fix -- so a plain
		// ApplyCstInsertChunk append would NOT actually red-prove the bug.  To reproduce the reviewer's
        	// literal "out-of-band append" shape (a chunk landing with ZERO separator immediately after
		// the previous content, as a hand-authored append or an external tool's raw text splice would),
		// insert normally and then strip the synthesized lead separator back out via
		// ApplyCstRemoveItemsAt(insertedAt, 1, ...) -- a non-triple `count` bypasses that primitive's
		// own identity check (documented as scoped to the triple shape only), so this is a clean,
		// positional-only removal of just the one separator item, leaving `matAppendR2` glued directly
		// onto the Document's tail with no separator of its own.
		Check( c.RunPreviewRenderParked( [&]{
			char kw[64]; char nm[64]; char diag[256];
			const char* freshChunk = "lambertian_material\n{\nname matAppendR2\nreflectance white\n}\n";
			int insertedAt = -1;
			const int insCode = pJob->ApplyCstInsertChunk( freshChunk, kw, sizeof( kw ), nm, sizeof( nm ), diag, sizeof( diag ), &insertedAt );
			Check( insCode == 2 || insCode == 3, "the out-of-band append of `matAppendR2` succeeds (no history pushed)" );
			Check( insertedAt >= 0, "ApplyCstInsertChunk reports the triple's own leading index" );

			char diag2[256];
			const int stripCode = pJob->ApplyCstRemoveItemsAt( insertedAt, 1, diag2, sizeof( diag2 ), nullptr );
			Check( stripCode == 2 || stripCode == 3,
			       "stripping the synthesized lead separator (leaving matAppendR2 glued with ZERO separator) succeeds" );

			c.Editor().RebindScene( *pJob->GetScene() );
			c.Editor().SetMaterialManager( pJob->GetMaterials() );
			c.Editor().SetShaderManager( pJob->GetShaders() );
			c.Editor().SetPainterManager( pJob->GetPainters() );
			c.Editor().SetScalarPainterManager( pJob->GetScalarPainters() );
			c.Editor().SetJob( pJob );
		} ), "the parked out-of-band append + manual rebind ran" );
		Check( c.Editor().History().UndoDepth() == undoDepthAfterRemove,
		       "the out-of-band append pushed NO history of its own -- the agent's remove entry is still the next one to pop" );

		// Undo the ORIGINAL agent remove of `obj`: its capture is chunk-only bytes ending in `}`
		// (never `\n`, per the LAST-chunk shape), and the out-of-band append now sits immediately to
		// the right of the restore point -- exactly the shape needing the round-2 right-side synthesis.
		c.Undo();
		const std::string postUndoDoc = RISE::Cst::SerializeCst( *pJob->GetCstDocument() );
		Check( postUndoDoc.find( "name obj" ) != std::string::npos, "`obj`'s text is back in the Document after Undo" );
		Check( postUndoDoc.find( "matAppendR2" ) != std::string::npos, "the out-of-band `matAppendR2` chunk survives the restore" );

		bool foundGlue = false;
		size_t glueAt = std::string::npos;
		for( size_t p = postUndoDoc.find( '}' ); p != std::string::npos; p = postUndoDoc.find( '}', p + 1 ) )
		{
			if( p + 1 < postUndoDoc.size() )
			{
				const char next = postUndoDoc[p + 1];
				if( next != '\n' && next != '\r' && next != ' ' && next != '\t' )
				{
					foundGlue = true;
					glueAt = p;
					break;
				}
			}
		}
		Check( !foundGlue, "RED-PROVE: no `}`-followed-by-non-whitespace glue adjacency anywhere in the restored Document (round-2 right-side fix, last-chunk shape)" );
		if( foundGlue )
			std::cout << "    glue found at byte " << glueAt << ": ..." << postUndoDoc.substr( glueAt, 32 ) << "..." << std::endl;

		Check( pJob->GetObjects() && pJob->GetObjects()->GetItem( "obj" ) != nullptr,
		       "the derived scene resolves `obj` again after Undo" );
		Check( pJob->GetMaterials() && pJob->GetMaterials()->GetItem( "matAppendR2" ) != nullptr,
		       "the derived scene still resolves the out-of-band `matAppendR2` too" );

		// Save + reload clean.
		const char* saveAs = "agentlive_p1_round2_lastchunk_save.RISEscene";
		std::remove( saveAs );
		const SaveResult sr = c.RequestSave( std::string( saveAs ) );
		Check( Succeeded( sr.status ), "save succeeds after the glue-safe restore" );
		{
			Job* fresh = new Job();
			Check( fresh->LoadAsciiSceneViaCst( saveAs ), "the saved Document reloads CLEANLY (no wedge, no corruption)" );
			Check( fresh->GetObjects() && fresh->GetObjects()->GetItem( "obj" ) != nullptr, "the reload resolves `obj`" );
			Check( fresh->GetMaterials() && fresh->GetMaterials()->GetItem( "matAppendR2" ) != nullptr, "the reload resolves `matAppendR2`" );
			fresh->release();
		}
		std::remove( saveAs );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5a: proposal staging + owner-approval state machine.
// Two AgentSessions (sessionOwner: Owner authority; sessionExt: External
// authority) WrapJob the SAME live Job and AttachController the SAME
// controller -- mirroring Test 8's dispatcher wiring, but with two actors
// instead of one, exactly the "external proposes, GUI owner approves"
// shape this slice implements.
//////////////////////////////////////////////////////////////////////
static void TestProposalStagingBasics()
{
	std::cout << "Test 33: External propose stages (inert); owner approves -> applies; owner rejects -> no mutation..." << std::endl;

	const char* tmp = "agentlive_proposal_basics.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		Check( owner != nullptr && ext != nullptr, "both sessions wrap the live Job" );
		owner->AttachController( &c );
		ext->AttachController( &c );
		Check( owner->Authority() == Agent::AgentAuthority::Owner, "owner session reports Owner authority" );
		Check( ext->Authority() == Agent::AgentAuthority::External, "external session reports External authority" );

		//------------------------------------------------------------------
		// (a) External ProposePatch STAGES -- the document is UNCHANGED.
		//------------------------------------------------------------------
		const double before = LumR( *pJob );
		Agent::AgentSetPatch p;
		p.target = "lum";
		p.kind   = "lambertian_luminaire_material";
		p.param  = "scale";
		p.value  = "9.25";
		Agent::AgentPatchResult pr = ext->ProposePatch( p );
		Check( !pr.applied, "RED-PROVE: external propose does NOT apply (would be true if staging were skipped)" );
		Check( pr.status == "staged", "external propose reports status=\"staged\"" );
		Check( LumR( *pJob ) == before, "RED-PROVE: the document is UNCHANGED after an external stage (would move to ~9.25 if it committed)" );

		const std::vector<Agent::AgentSession::AgentProposalEntry> listed = owner->ListProposals();
		Check( listed.size() == 1, "ListProposals shows exactly the one staged proposal" );
		Check( !listed.empty() && listed[0].status == "pending", "the staged proposal is \"pending\"" );
		Check( !listed.empty() && listed[0].kind == "param_edit", "the staged proposal is kind=\"param_edit\"" );
		Check( !listed.empty() && listed[0].target == "lum", "the staged proposal echoes the target" );
		const std::uint64_t proposalId = listed.empty() ? 0 : listed[0].id;
		Check( proposalId != 0, "the staged proposal carries a non-zero id" );

		//------------------------------------------------------------------
		// (b) Owner approves -> the edit APPLIES now; EditHistory citizen
		// (a controller Undo reverts it).
		//------------------------------------------------------------------
		Agent::AgentSession::AgentResolveResult rr = owner->ResolveProposal( proposalId, /*approve=*/true );
		Check( rr.ok, "owner ResolveProposal(approve) runs" );
		Check( rr.status == "applied", "approved proposal resolves to status=\"applied\"" );
		Check( rr.paramResult.applied, "the folded paramResult reports applied=true" );
		Check( LumR( *pJob ) > 8.5 && LumR( *pJob ) < 10.0, "the approved proposal's edit reached the live scene (~9.25)" );
		Check( c.HasUnsavedChanges(), "the approved proposal marked the editor dirty" );

		c.Undo();
		Check( LumR( *pJob ) > 4.5 && LumR( *pJob ) < 5.5, "controller Undo reverted the approved proposal's edit (~5.0, the original) -- proof it is a normal EditHistory citizen" );
		c.Redo();
		Check( LumR( *pJob ) > 8.5 && LumR( *pJob ) < 10.0, "Redo re-applies it" );

		//------------------------------------------------------------------
		// (c) A second stage, this time REJECTED by the owner -> no mutation.
		//------------------------------------------------------------------
		const double beforeReject = LumR( *pJob );
		Agent::AgentSetPatch p2 = p;
		p2.value = "1.0";
		Agent::AgentPatchResult pr2 = ext->ProposePatch( p2 );
		Check( pr2.status == "staged", "second external propose also stages" );
		const std::vector<Agent::AgentSession::AgentProposalEntry> listed2 = owner->ListProposals();
		std::uint64_t secondId = 0;
		for( const auto& e : listed2 ) if( e.status == "pending" ) secondId = e.id;
		Check( secondId != 0, "the second proposal is found pending" );
		Agent::AgentSession::AgentResolveResult rrReject = owner->ResolveProposal( secondId, /*approve=*/false );
		Check( rrReject.ok, "owner ResolveProposal(reject) runs" );
		Check( rrReject.status == "rejected", "rejected proposal resolves to status=\"rejected\"" );
		Check( LumR( *pJob ) == beforeReject, "RED-PROVE: reject left the document UNCHANGED (would move to ~1.0 if applied)" );
		// Secure-MCP slice 5b fix round (P2-2) RED-PROVE, C++ level: the
		// reject's headVersion is the REAL current head (a non-zero uuid),
		// not the {0,0} default a caller would confuse for "no session /
		// unknown id" -- whichever of paramResult/chunkResult carries the
		// non-empty status (this proposal is a param edit) is where the
		// wire layer (AgentRpc.cpp) reads headVersion from.
		Check( rrReject.paramResult.status == "rejected",
		       "rejected proposal's paramResult.status == \"rejected\" (this is a param-edit proposal)" );
		Check( rrReject.paramResult.headVersion.uuid != 0,
		       "RED-PROVE: rejected proposal's paramResult.headVersion carries a real (non-zero) uuid, "
		       "not the {0,0} sentinel" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5a: the stale-approve invariant -- an owner's OWN direct
// edit moves the head between stage and approve; ResolveProposal must
// re-check and refuse to clobber.
//////////////////////////////////////////////////////////////////////
static void TestStaleApproveConflict()
{
	std::cout << "Test 34: stale approve (head moved between stage and approve) -> conflict, no apply..." << std::endl;

	const char* tmp = "agentlive_proposal_stale.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		owner->AttachController( &c );
		ext->AttachController( &c );

		Agent::AgentSetPatch p;
		p.target = "lum";
		p.kind   = "lambertian_luminaire_material";
		p.param  = "scale";
		p.value  = "6.0";
		Agent::AgentPatchResult pr = ext->ProposePatch( p );
		Check( pr.status == "staged", "external propose stages" );
		std::uint64_t id = 0;
		for( const auto& e : owner->ListProposals() ) if( e.status == "pending" ) id = e.id;
		Check( id != 0, "the proposal is pending" );

		// The OWNER makes a direct, intervening edit (a real head-moving
		// commit through the SAME controller) BETWEEN stage and approve.
		Agent::AgentSetPatch ownerEdit;
		ownerEdit.target = "lum";
		ownerEdit.kind   = "lambertian_luminaire_material";
		ownerEdit.param  = "scale";
		ownerEdit.value  = "2.0";
		Agent::AgentPatchResult ownerPr = owner->ProposePatch( ownerEdit );
		Check( ownerPr.applied, "the owner's intervening direct edit applies (Owner authority commits directly, unchanged)" );
		Check( LumR( *pJob ) > 1.5 && LumR( *pJob ) < 2.5, "the live scene now reflects the owner's intervening edit (~2.0)" );

		// Approve the STALE proposal -- RED-PROVE: without the base-version
		// re-check inside ResolveProposal, this would clobber the owner's
		// intervening edit with the stale 6.0.
		const double beforeApprove = LumR( *pJob );
		Agent::AgentSession::AgentResolveResult rr = owner->ResolveProposal( id, /*approve=*/true );
		Check( rr.ok, "resolve runs (the id is found)" );
		Check( rr.status == "conflict", "RED-PROVE: stale approve resolves to status=\"conflict\" (would be \"applied\" without the re-check)" );
		Check( LumR( *pJob ) == beforeApprove, "RED-PROVE: the conflict left the live scene UNCHANGED at the owner's edit (would move to ~6.0 without the re-check)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5a: reload invalidation.  A genuine SceneEditController
// reload mints a fresh Job (LoadAsciiSceneViaCst refuses a second load on
// the SAME Job -- there is no in-place reload hook to fake this against
// the SAME controller), so this test proves the invariant the way it
// actually manifests: a baseVersion carrying a DIFFERENT uuid (as a fresh
// load would mint) is a conflict at approval time even though the
// revision number might otherwise coincide -- the reload-collision guard
// CstHeadVersion::operator== requires BOTH fields to match.
//////////////////////////////////////////////////////////////////////
static void TestReloadInvalidatesStaleProposal()
{
	std::cout << "Test 35: a proposal staged against a DIFFERENT uuid (simulating a reload) -> conflict on approve..." << std::endl;

	const char* tmp = "agentlive_proposal_reload.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	// A second, independent Job load mints a genuinely DIFFERENT uuid
	// (Job::LoadAsciiSceneViaCst's NextCstHeadUuid() is a process-global
	// monotonic counter -- see Job.cpp) -- this stands in for "the uuid a
	// reload of the SAME file would mint", without needing an in-place
	// reload hook the controller does not expose.
	const char* tmp2 = "agentlive_proposal_reload_other.RISEscene";
	Job* otherJob = LoadScene( kBaseScene, tmp2 );
	Check( otherJob != nullptr, "a second, independent Job load succeeds" );
	if( otherJob )
	{
		Check( otherJob->GetCstHeadVersion().uuid != pJob->GetCstHeadVersion().uuid,
		       "the second load minted a DIFFERENT uuid (the reload-collision guard, Facet 5 slice 1a)" );
	}

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		owner->AttachController( &c );
		ext->AttachController( &c );

		// Stage a patch whose baseVersion is EXPLICITLY the other Job's
		// head (a foreign uuid) -- as if the proposing agent had read a
		// head that (from this controller's point of view) no longer
		// exists because the scene was reloaded.
		Agent::AgentSetPatch p;
		p.target = "lum";
		p.kind   = "lambertian_luminaire_material";
		p.param  = "scale";
		p.value  = "8.0";
		p.hasBaseVersion = true;
		p.baseVersion = otherJob ? otherJob->GetCstHeadVersion() : RISE::Cst::CstHeadVersion{};
		Agent::AgentPatchResult pr = ext->ProposePatch( p );
		Check( pr.status == "staged", "external propose stages even against a foreign baseVersion (staging never checks it)" );
		std::uint64_t id = 0;
		for( const auto& e : owner->ListProposals() ) if( e.status == "pending" ) id = e.id;
		Check( id != 0, "the proposal is pending" );

		const double before = LumR( *pJob );
		Agent::AgentSession::AgentResolveResult rr = owner->ResolveProposal( id, /*approve=*/true );
		Check( rr.ok, "resolve runs" );
		Check( rr.status == "conflict", "RED-PROVE: approving against a foreign (reload-simulating) uuid resolves to \"conflict\" (would be \"applied\" without the uuid check)" );
		Check( LumR( *pJob ) == before, "the live scene is unchanged after the reload-invalidated conflict" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
	if( otherJob ) otherJob->release();
	std::remove( tmp2 );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5a: two proposals staged against the SAME param;
// approving the first shifts the head, so approving the second (still
// carrying the ORIGINAL baseVersion) is a conflict.
//////////////////////////////////////////////////////////////////////
static void TestTwoProposalsSameParamSecondIsStale()
{
	std::cout << "Test 36: stage A, stage B (same param); approve A applies; approve B -> conflict (B's base is now stale)..." << std::endl;

	const char* tmp = "agentlive_proposal_twoprop.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		owner->AttachController( &c );
		ext->AttachController( &c );

		Agent::AgentSetPatch pa;
		pa.target = "lum";
		pa.kind   = "lambertian_luminaire_material";
		pa.param  = "scale";
		pa.value  = "3.0";
		Agent::AgentPatchResult prA = ext->ProposePatch( pa );
		Check( prA.status == "staged", "proposal A stages" );

		Agent::AgentSetPatch pb = pa;
		pb.value = "4.0";
		Agent::AgentPatchResult prB = ext->ProposePatch( pb );
		Check( prB.status == "staged", "proposal B stages (against the SAME pre-A head, since staging never mutates)" );

		const std::vector<Agent::AgentSession::AgentProposalEntry> listed = owner->ListProposals();
		Check( listed.size() == 2, "both proposals are on the queue" );
		std::uint64_t idA = 0, idB = 0;
		for( const auto& e : listed ) { if( e.value == "3.0" ) idA = e.id; if( e.value == "4.0" ) idB = e.id; }
		Check( idA != 0 && idB != 0, "both proposal ids resolved by their staged value" );

		Agent::AgentSession::AgentResolveResult rrA = owner->ResolveProposal( idA, /*approve=*/true );
		Check( rrA.ok && rrA.status == "applied", "approve A applies (A's base was the head at stage time -- still current)" );
		Check( LumR( *pJob ) > 2.5 && LumR( *pJob ) < 3.5, "the live scene reflects A (~3.0)" );

		Agent::AgentSession::AgentResolveResult rrB = owner->ResolveProposal( idB, /*approve=*/true );
		Check( rrB.ok, "resolve B runs" );
		Check( rrB.status == "conflict", "RED-PROVE: approve B -> conflict (B's base is now stale -- A's apply moved the head)" );
		Check( LumR( *pJob ) > 2.5 && LumR( *pJob ) < 3.5, "the live scene is UNCHANGED by B's conflict (still ~3.0, not 4.0)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5a: the authority x autonomy matrix refusals.
//   * External + Commit-shaped call (no staging bypass) + a live controller
//     attached still only ever STAGES -- there is no way to ask an
//     External session to commit directly; ProposePatch/InsertChunk/
//     RemoveChunk always route it through the stage branch.  This is
//     itself the "External never commits" proof (folded into the basics
//     test's RED-PROVE above); this test covers the TWO remaining matrix
//     cells: External+Propose+NO controller (headless) -> refused, and
//     External attempting to RESOLVE (even its own proposal) -> refused.
//   * Owner + a live controller -> still commits directly (regression
//     guard: byte-for-byte unchanged from every pre-existing test above).
//////////////////////////////////////////////////////////////////////
static void TestAuthorityMatrixRefusals()
{
	std::cout << "Test 37: authority matrix -- external headless propose refused; external resolve (even own) refused; owner unaffected..." << std::endl;

	// (a) External + Propose + NO controller (headless) -> refused, not a
	// silent commit.  This is the "no headless back door" hole this slice
	// closes: a headless External session has no queue to stage TO.
	{
		const char* tmp = "agentlive_proposal_headless_ext.RISEscene";
		{ std::ofstream o( tmp ); o << kBaseScene; }
		std::unique_ptr<Agent::AgentSession> headlessExt =
			Agent::AgentSession::LoadFromFile( std::string( tmp ), Agent::AgentAuthority::External );
		Check( headlessExt != nullptr, "a headless External session loads" );
		if( headlessExt )
		{
			Check( headlessExt->Authority() == Agent::AgentAuthority::External, "reports External authority" );
			Check( !headlessExt->HasController(), "headless -- no controller attached" );
			Agent::AgentSetPatch p;
			p.target = "lum";
			p.kind   = "lambertian_luminaire_material";
			p.param  = "scale";
			p.value  = "42.0";
			Agent::AgentPatchResult pr = headlessExt->ProposePatch( p );
			Check( !pr.applied, "headless external propose does not apply" );
			Check( pr.status == "rejected", "RED-PROVE: headless external propose is REFUSED, status=\"rejected\" (would be \"staged\" or \"applied\" without the no-controller refusal)" );
		}
		headlessExt.reset();
		std::remove( tmp );
	}

	// (b) External resolving -- even its OWN proposal -- is refused; and
	// (c) Owner + a live controller still commits directly (unchanged).
	{
		const char* tmp = "agentlive_proposal_matrix.RISEscene";
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "base scene loads via the CST path" );
		if( !pJob ) return;

		{
			TestController c( *pJob, /*simulatedRenderMs*/20 );
			c.Start();
			Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

			std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
			std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
			owner->AttachController( &c );
			ext->AttachController( &c );

			// (c) Owner + live controller -> commits directly, unchanged.
			Agent::AgentSetPatch ownerPatch;
			ownerPatch.target = "lum";
			ownerPatch.kind   = "lambertian_luminaire_material";
			ownerPatch.param  = "scale";
			ownerPatch.value  = "11.0";
			Agent::AgentPatchResult ownerPr = owner->ProposePatch( ownerPatch );
			Check( ownerPr.applied, "owner + live controller still commits directly (unchanged regression guard)" );
			Check( ownerPr.status == "applied", "owner propose status is \"applied\", not \"staged\"" );
			Check( LumR( *pJob ) > 10.0 && LumR( *pJob ) < 12.0, "owner's direct commit reached the live scene (~11.0)" );

			// (b) External stages, then tries to resolve its OWN proposal.
			Agent::AgentSetPatch extPatch;
			extPatch.target = "lum";
			extPatch.kind   = "lambertian_luminaire_material";
			extPatch.param  = "scale";
			extPatch.value  = "13.0";
			Agent::AgentPatchResult extPr = ext->ProposePatch( extPatch );
			Check( extPr.status == "staged", "external propose stages" );
			std::uint64_t id = 0;
			for( const auto& e : ext->ListProposals() ) if( e.status == "pending" ) id = e.id;
			Check( id != 0, "external session's OWN ListProposals sees the pending proposal (listing is authority-agnostic)" );

			const double beforeSelfResolve = LumR( *pJob );
			Agent::AgentSession::AgentResolveResult rr = ext->ResolveProposal( id, /*approve=*/true );
			Check( !rr.ok, "RED-PROVE: external resolving its OWN proposal is REFUSED (ok=false) -- would be ok=true/applied without the owner-only gate" );
			Check( LumR( *pJob ) == beforeSelfResolve, "the live scene is unchanged after the refused self-resolve" );

			// Owner CAN resolve it.
			Agent::AgentSession::AgentResolveResult rrOwner = owner->ResolveProposal( id, /*approve=*/true );
			Check( rrOwner.ok && rrOwner.status == "applied", "the owner CAN resolve the same proposal the external session was refused for" );
			Check( LumR( *pJob ) > 12.0 && LumR( *pJob ) < 14.0, "the owner's approval on the external's behalf reached the live scene (~13.0)" );

			c.Stop();
		}
		pJob->release();
		std::remove( tmp );
	}
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5a: insert_chunk / remove_chunk staging variants --
// mirror the param-edit basics test, proving the chunk-CRUD verbs stage +
// approve + apply correctly too (not just propose_patch).
//////////////////////////////////////////////////////////////////////
static void TestChunkCrudStaging()
{
	std::cout << "Test 38: insert_chunk / remove_chunk staging (stage -> inert; approve -> applies via the real chunk-CRUD path)..." << std::endl;

	const char* tmp = "agentlive_proposal_chunkcrud.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		owner->AttachController( &c );
		ext->AttachController( &c );

		//------------------------------------------------------------------
		// insert_chunk: stage a new, uniquely-named material; unchanged
		// until approved.
		//------------------------------------------------------------------
		const std::string newMatChunk =
			"uniformcolor_painter\n{\nname stagedPainter\ncolor 0.25 0.25 0.25\n}\n";
		Agent::AgentChunkResult insR = ext->InsertChunk( newMatChunk );
		Check( !insR.applied, "external insert_chunk does not apply" );
		Check( insR.status == "staged", "external insert_chunk reports status=\"staged\"" );
		Check( pJob->GetPainters() && pJob->GetPainters()->GetItem( "stagedPainter" ) == nullptr,
		       "RED-PROVE: the staged painter does NOT exist in the live scene yet (would exist if staging were skipped)" );

		std::uint64_t insertId = 0;
		for( const auto& e : owner->ListProposals() ) if( e.status == "pending" && e.kind == "insert_chunk" ) insertId = e.id;
		Check( insertId != 0, "the staged insert is listed with kind=\"insert_chunk\"" );

		Agent::AgentSession::AgentResolveResult insResolve = owner->ResolveProposal( insertId, /*approve=*/true );
		Check( insResolve.ok && insResolve.status == "applied", "owner approves the staged insert -> applied" );
		Check( insResolve.chunkResult.applied, "the folded chunkResult reports applied=true" );
		Check( pJob->GetPainters() && pJob->GetPainters()->GetItem( "stagedPainter" ) != nullptr,
		       "the approved insert's chunk now resolves in the live scene" );
		Check( c.HasUnsavedChanges(), "the approved insert marked the editor dirty" );

		// EditHistory citizen: Undo reverts the approved insert.
		c.Undo();
		Check( pJob->GetPainters() && pJob->GetPainters()->GetItem( "stagedPainter" ) == nullptr,
		       "controller Undo reverted the approved chunk insert -- proof it is a normal EditHistory citizen" );
		c.Redo();
		Check( pJob->GetPainters() && pJob->GetPainters()->GetItem( "stagedPainter" ) != nullptr,
		       "Redo re-applies the chunk insert" );

		//------------------------------------------------------------------
		// remove_chunk: stage removing the just-inserted (now-live, unreferenced)
		// painter; unchanged until approved.
		//------------------------------------------------------------------
		Agent::AgentChunkResult remR = ext->RemoveChunk( "stagedPainter", "uniformcolor_painter" );
		Check( !remR.applied, "external remove_chunk does not apply" );
		Check( remR.status == "staged", "external remove_chunk reports status=\"staged\"" );
		Check( pJob->GetPainters() && pJob->GetPainters()->GetItem( "stagedPainter" ) != nullptr,
		       "RED-PROVE: the painter STILL exists after staging the remove (would be gone if staging were skipped)" );

		std::uint64_t removeId = 0;
		for( const auto& e : owner->ListProposals() ) if( e.status == "pending" && e.kind == "remove_chunk" ) removeId = e.id;
		Check( removeId != 0, "the staged remove is listed with kind=\"remove_chunk\"" );

		Agent::AgentSession::AgentResolveResult remResolve = owner->ResolveProposal( removeId, /*approve=*/true );
		Check( remResolve.ok && remResolve.status == "applied", "owner approves the staged remove -> applied" );
		Check( pJob->GetPainters() && pJob->GetPainters()->GetItem( "stagedPainter" ) == nullptr,
		       "the approved remove's chunk is gone from the live scene" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 39: S5a hardening round -- RED-PROVE that an External session's
// staged baseVersion is a COHERENT head value, never a torn/impossible one,
// even under heavy concurrent commits.  Before this hardening,
// AgentSession::ProposePatch/InsertChunk/RemoveChunk's External-staging
// branches read mJob->GetCstHeadVersion() UNLOCKED (no mMutex) to populate
// the staged proposal's baseVersion -- a data race on the non-atomic
// 16-byte CstHeadVersion against the controller's render thread / a
// concurrent Owner commit, both of which write it UNDER mMutex (see
// SceneEditController::ApplyAgentParamEdit's doc, ~line 3193: "every head
// read is under mMutex").  The fix moves the read INTO
// SceneEditController::StageProposal itself, under the SAME mMutex hold
// that mints the proposal id and enqueues -- so the capture is now
// STRUCTURALLY race-free (the read literally cannot tear or observe a
// half-written value; the lock excludes every other head writer/reader).
//
// This is a genuine data race, so it is inherently timing-dependent to
// catch via a black-box symptom (a torn 16-byte struct on x86_64/ARM64
// rarely manifests as a crash; it would show up as a nonsensical
// revision -- e.g. one that never corresponds to any commit the test
// thread actually observed, or one "from the future" relative to the
// last commit the staging thread's snapshot could have seen).  The
// RED-PROVE here is: hammer many concurrent Owner commits (each bumping
// the revision under mMutex) against many concurrent External stages (each
// exercising the now-locked capture) and assert EVERY staged baseVersion:
//   (a) carries the SAME uuid as the live Job's retained head (never {0,0}
//       or a foreign uuid -- would indicate a torn/garbage read), and
//   (b) has a revision that is <= the highest revision any thread had
//       observed as "committed" AT THE TIME the stage call returned (never
//       "from the future" -- would indicate a torn read landing past where
//       any real commit had gotten to).
// Structural argument (documented per the task's RED-PROVE contract, since
// a torn read on this platform/struct is not reliably forced from a test):
// StageProposal's capture is now INSIDE the same std::lock_guard<mMutex>
// that performs the id-mint + enqueue, and every commit path
// (ApplyAgentParamEdit et al.) both READS and WRITES mCstHeadVersion only
// while holding that identical mMutex (see SceneEditController.cpp
// ApplyAgentParamEdit's "Cancel-and-park...THEN hold mMutex across the
// WHOLE commit" comment) -- so by mutual exclusion alone, no interleaving
// exists where StageProposal's read observes a partially-written
// CstHeadVersion.  The stress loop below is belt-and-suspenders: it did
// not reproduce a failure pre-fix on this platform either (torn reads on a
// 16-byte POD via two int64 stores/loads are legal-but-rare under TSan-free
// stress on arm64/x86_64), so the suite-green + the structural argument
// together are what the task's "best-effort" RED-PROVE calls for.
//////////////////////////////////////////////////////////////////////
static void TestExternalStageBaseVersionCoherentUnderConcurrency()
{
	std::cout << "Test 39: RED-PROVE -- external-stage baseVersion is coherent (never torn/future) under concurrent commits..." << std::endl;

	const char* tmp = "agentlive_stage_basever_stress.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/5 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> owner = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext   = Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		owner->AttachController( &c );
		ext->AttachController( &c );

		const RISE::Cst::CstHeadVersion startHead = pJob->GetCstHeadVersion();
		Check( startHead.uuid != 0, "the live head has a real (non-zero) uuid before the storm" );

		const int kIters = 300;
		std::atomic<std::uint64_t> highestCommittedRevision( startHead.revision );
		std::atomic<int> commitOk( 0 );
		std::atomic<int> stageOk( 0 );
		std::atomic<int> incoherentUuid( 0 );
		std::atomic<int> futureRevision( 0 );

		// Owner thread: hammer DIRECT commits (bumping mCstHeadVersion.revision
		// under the controller's mMutex on every clean apply) -- this is the
		// concurrent writer the External-stage read used to race.
		std::thread ownerThread( [&]{
			for( int i = 0; i < kIters; ++i )
			{
				char v[32];
				std::snprintf( v, sizeof(v), "%.3f", 2.0 + ( i % 7 ) );
				Agent::AgentSetPatch p;
				p.target = "lum";
				p.kind   = "lambertian_luminaire_material";
				p.param  = "scale";
				p.value  = v;
				const Agent::AgentPatchResult r = owner->ProposePatch( p );
				if( r.applied )
				{
					commitOk.fetch_add( 1 );
					// Publish the just-committed revision as a lower bound any
					// LATER stage call is allowed to see -- a racy stage read
					// that lags is fine (that's a legitimate "stale but
					// coherent" snapshot); one that's ahead of every commit
					// this thread itself just made would be a "from the
					// future" impossible value.
					std::uint64_t prev = highestCommittedRevision.load();
					while( r.headVersion.revision > prev &&
					       !highestCommittedRevision.compare_exchange_weak( prev, r.headVersion.revision ) ) {}
				}
			}
		} );

		// External thread: hammer STAGE calls (no explicit baseVersion) --
		// each one exercises StageProposal's now-locked capture.
		std::thread extThread( [&]{
			for( int i = 0; i < kIters; ++i )
			{
				Agent::AgentSetPatch p;
				p.target = "lum";
				p.kind   = "lambertian_luminaire_material";
				p.param  = "scale";
				p.value  = "5.5";   // value irrelevant -- never approved, only staged
				const Agent::AgentPatchResult r = ext->ProposePatch( p );
				if( r.status == "staged" )
				{
					stageOk.fetch_add( 1 );
					if( r.headVersion.uuid != startHead.uuid ) incoherentUuid.fetch_add( 1 );
					// Allow a small race window: the owner thread's
					// publish (above) happens AFTER its ProposePatch call
					// returns, so a stage that interleaved between the
					// owner's internal commit and its publish could
					// legitimately see a revision the bound hasn't
					// published yet.  Poll briefly for the bound to catch
					// up before judging "from the future" -- this keeps the
					// assertion honest (a REAL torn/garbage read will not
					// self-correct by waiting) without making the test
					// flaky on the ordinary publish-lag race.
					std::uint64_t bound = highestCommittedRevision.load();
					for( int spin = 0; r.headVersion.revision > bound && spin < 1000; ++spin )
					{
						std::this_thread::yield();
						bound = highestCommittedRevision.load();
					}
					if( r.headVersion.revision > bound ) futureRevision.fetch_add( 1 );
				}
			}
		} );

		ownerThread.join();
		extThread.join();

		std::cout << "    commitOk=" << commitOk.load() << " stageOk=" << stageOk.load()
		          << " incoherentUuid=" << incoherentUuid.load()
		          << " futureRevision=" << futureRevision.load() << std::endl;

		Check( commitOk.load() > 0, "owner thread landed at least one clean commit" );
		Check( stageOk.load() > 0, "external thread staged at least one proposal" );
		Check( incoherentUuid.load() == 0,
		       "RED-PROVE: every staged baseVersion carries the SAME uuid as the live head (never torn/foreign)" );
		Check( futureRevision.load() == 0,
		       "RED-PROVE: no staged baseVersion is a revision 'from the future' relative to any commit observed" );

		// The controller is still alive and the queue is internally
		// consistent after the storm (structural sanity, not just the
		// per-call checks above).
		Check( c.IsRunning(), "render thread still running after the stage/commit storm" );
		const std::vector<Agent::AgentSession::AgentProposalEntry> listed = owner->ListProposals();
		Check( (int)listed.size() == stageOk.load(), "ListProposals count matches the number of successful stages" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Test 40 (Secure-MCP slice 5b): the WIRE round-trip for list_proposals /
// resolve_proposal -- Test 33 (TestProposalStagingBasics) proved the C++
// AgentSession::ListProposals/ResolveProposal core end to end; this test
// proves the SAME state machine over raw JSON-RPC lines through
// AgentRpcDispatcher::HandleLine, mirroring Test 8's "live dispatcher path"
// pattern but with TWO dispatchers (an Owner one and an External one) over
// the SAME live Job + controller -- exactly the external-proposes/owner-
// approves topology a real GUI-hosted loopback server + external MCP
// client would realize.
//
//   (a) an EXTERNAL dispatcher's wire propose_patch -> {applied:false,
//       status:"staged"} -- the document is unchanged.
//   (b) `ownerDisp`'s wire list_proposals -> the one pending entry,
//       with the real field shapes (kind, target, param, value, status).
//   (c) `ownerDisp`'s wire resolve_proposal{approve:true} ->
//       {resolved:true, status:"applied"} and the live scene actually moved.
//   (d) the EXTERNAL dispatcher calling wire resolve_proposal on a fresh
//       stage -> {resolved:false} (Owner-only, refused at the SESSION layer
//       -- this External dispatcher runs at AgentAutonomy::Commit, so the
//       call REACHES AgentSession, which is what proves the session-layer
//       gate independent of the wire-autonomy gate Test (g) in
//       AgentAutonomyPolicyTest.cpp proves separately).
//   (e) Secure-MCP slice 5b fix round (P2-2) RED-PROVE: the OWNER's wire
//       resolve_proposal{approve:false} (reject) on that same still-pending
//       proposal reports headVersion as the REAL current head (a non-zero
//       uuid), NOT the {0,0} sentinel that used to collide with the "no
//       session / unknown id" refusal shape -- AgentRpc.h documents "the
//       CURRENT head, unmoved, on reject" and this is the first assertion
//       that actually checks it.
//////////////////////////////////////////////////////////////////////
static void TestProposalWireRoundTrip()
{
	std::cout << "Test 40: proposal wire round-trip (list_proposals/resolve_proposal over JSON-RPC)..." << std::endl;

	const char* tmp = "agentlive_proposal_wire.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		// `ownerDisp`: Owner-authority session, Commit autonomy (the shape
		// the GUI's in-process ADMINISTRATIVE dispatcher runs at --
		// unaffected by this slice).
		std::unique_ptr<Agent::AgentSession> ownerSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		Check( ownerSess != nullptr, "owner session wraps the live Job" );
		if( ownerSess ) ownerSess->AttachController( &c );
		Agent::AgentRpcDispatcher ownerDisp( std::move( ownerSess ), Agent::AgentAutonomy::Commit );

		// External dispatcher: External-authority session, Commit autonomy
		// AT THE WIRE LAYER -- deliberately NOT Propose here, so that
		// propose_patch/resolve_proposal REACH AgentSession unconditionally
		// and this test isolates the SESSION-layer (authority) gating from
		// the wire-layer (autonomy) gating Test (g) in
		// AgentAutonomyPolicyTest.cpp already covers. A real external wire
		// client is constructed at Propose autonomy (see commandconsole.cpp's
		// AuthorityForAutonomy) -- both gates independently refuse
		// resolve_proposal for it either way (see (d) below).
		std::unique_ptr<Agent::AgentSession> extSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		Check( extSess != nullptr, "external session wraps the live Job" );
		if( extSess ) extSess->AttachController( &c );
		Agent::AgentRpcDispatcher extDisp( std::move( extSess ), Agent::AgentAutonomy::Commit );

		//------------------------------------------------------------------
		// (a) EXTERNAL wire propose_patch -> staged, document unchanged.
		//------------------------------------------------------------------
		const double before = LumR( *pJob );
		const std::string proposeResp = extDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"6.5\"}}" );
		Agent::JsonValue proposeResult;
		Check( JsonResultObj( proposeResp, proposeResult ), "wire propose_patch (External) returns a JSON-RPC result object" );
		const Agent::JsonValue* applied = proposeResult.find( "applied" );
		Check( applied && applied->isBool() && !applied->asBool(), "wire propose_patch (External) reports applied=false" );
		const Agent::JsonValue* status = proposeResult.find( "status" );
		Check( status && status->isString() && status->asString() == "staged",
		       "wire propose_patch (External) reports status=\"staged\"" );
		Check( LumR( *pJob ) == before, "RED-PROVE: the document is UNCHANGED after the wire-staged propose_patch" );

		//------------------------------------------------------------------
		// (b) OWNER wire list_proposals -> the one pending entry, real shape.
		//------------------------------------------------------------------
		const std::string listResp = ownerDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"list_proposals\",\"params\":{}}" );
		Agent::JsonValue listResult;
		Check( JsonResultObj( listResp, listResult ), "wire list_proposals (Owner) returns a JSON-RPC result object" );
		const Agent::JsonValue* proposalsArr = listResult.find( "proposals" );
		Check( proposalsArr && proposalsArr->isArray() && proposalsArr->size() == 1,
		       "wire list_proposals (Owner) shows exactly the one staged proposal" );
		std::uint64_t proposalId = 0;
		if( proposalsArr && proposalsArr->isArray() && proposalsArr->size() == 1 ) {
			const Agent::JsonValue& entry = proposalsArr->at( 0 );
			const Agent::JsonValue* kind   = entry.find( "kind" );
			const Agent::JsonValue* target = entry.find( "target" );
			const Agent::JsonValue* param  = entry.find( "param" );
			const Agent::JsonValue* value  = entry.find( "value" );
			const Agent::JsonValue* pstat  = entry.find( "status" );
			const Agent::JsonValue* pid    = entry.find( "id" );
			Check( kind && kind->isString() && kind->asString() == "param_edit", "listed proposal kind==\"param_edit\"" );
			Check( target && target->isString() && target->asString() == "lum", "listed proposal echoes target==\"lum\"" );
			Check( param && param->isString() && param->asString() == "scale", "listed proposal echoes param==\"scale\"" );
			Check( value && value->isString() && value->asString() == "6.5", "listed proposal echoes value==\"6.5\"" );
			Check( pstat && pstat->isString() && pstat->asString() == "pending", "listed proposal status==\"pending\"" );
			Check( pid && pid->isNumber() && pid->asNumber() != 0.0, "listed proposal carries a non-zero id" );
			if( pid ) proposalId = static_cast<std::uint64_t>( pid->asNumber() );
		}
		Check( proposalId != 0, "captured a real proposal id from the wire listing" );

		//------------------------------------------------------------------
		// (c) OWNER wire resolve_proposal{approve:true} -> applied; the live
		// scene actually moved.
		//------------------------------------------------------------------
		char resolveLine[256];
		std::snprintf( resolveLine, sizeof( resolveLine ),
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resolve_proposal\",\"params\":"
			"{\"proposalId\":%llu,\"approve\":true}}",
			static_cast<unsigned long long>( proposalId ) );
		const std::string resolveResp = ownerDisp.HandleLine( resolveLine );
		Agent::JsonValue resolveResult;
		Check( JsonResultObj( resolveResp, resolveResult ), "wire resolve_proposal (Owner, approve) returns a JSON-RPC result object" );
		const Agent::JsonValue* resolved = resolveResult.find( "resolved" );
		Check( resolved && resolved->isBool() && resolved->asBool(), "wire resolve_proposal (Owner, approve) reports resolved=true" );
		const Agent::JsonValue* rstatus = resolveResult.find( "status" );
		Check( rstatus && rstatus->isString() && rstatus->asString() == "applied",
		       "wire resolve_proposal (Owner, approve) reports status=\"applied\"" );
		const Agent::JsonValue* rhv = resolveResult.find( "headVersion" );
		Check( rhv && rhv->isObject(), "wire resolve_proposal (Owner, approve) carries a headVersion object" );
		if( rhv ) {
			const Agent::JsonValue* u = rhv->find( "uuid" );
			Check( u && u->isNumber() && u->asNumber() != 0.0,
			       "wire resolve_proposal's headVersion carries a real (non-zero) uuid -- not the {0,0} refusal sentinel" );
		}
		const double afterApprove = LumR( *pJob );
		Check( afterApprove > 6.0 && afterApprove < 7.0, "the approved proposal's edit reached the LIVE scene (~6.5)" );
		Check( c.HasUnsavedChanges(), "the wire-approved proposal marked the editor dirty" );

		//------------------------------------------------------------------
		// (d) A fresh stage, then the EXTERNAL dispatcher tries to wire
		// resolve_proposal it itself -> refused at the SESSION layer
		// (resolved:false) even though this dispatcher's own autonomy is
		// Commit (so the call reaches AgentSession unimpeded by the wire
		// gate) -- isolating AgentSession::ResolveProposal's Owner-only
		// check as the thing that actually refuses it.
		//------------------------------------------------------------------
		const std::string proposeResp2 = extDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"2.0\"}}" );
		Agent::JsonValue proposeResult2;
		Check( JsonResultObj( proposeResp2, proposeResult2 ), "second wire propose_patch (External) returns a result object" );
		std::uint64_t secondId = 0;
		{
			const std::string listResp2 = ownerDisp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"list_proposals\",\"params\":{}}" );
			Agent::JsonValue listResult2;
			Check( JsonResultObj( listResp2, listResult2 ), "second wire list_proposals (Owner) returns a result object" );
			const Agent::JsonValue* arr2 = listResult2.find( "proposals" );
			if( arr2 && arr2->isArray() ) {
				for( std::size_t i = 0; i < arr2->size(); ++i ) {
					const Agent::JsonValue& e = arr2->at( i );
					const Agent::JsonValue* pstat = e.find( "status" );
					const Agent::JsonValue* pid   = e.find( "id" );
					if( pstat && pstat->isString() && pstat->asString() == "pending" && pid && pid->isNumber() )
						secondId = static_cast<std::uint64_t>( pid->asNumber() );
				}
			}
		}
		Check( secondId != 0, "found the second pending proposal via the wire listing" );

		char resolveLine2[256];
		std::snprintf( resolveLine2, sizeof( resolveLine2 ),
			"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resolve_proposal\",\"params\":"
			"{\"proposalId\":%llu,\"approve\":true}}",
			static_cast<unsigned long long>( secondId ) );
		const double beforeExternalResolveAttempt = LumR( *pJob );
		const std::string extResolveResp = extDisp.HandleLine( resolveLine2 );
		Agent::JsonValue extResolveResult;
		Check( JsonResultObj( extResolveResp, extResolveResult ), "External wire resolve_proposal returns a JSON-RPC result object (not a protocol error)" );
		const Agent::JsonValue* extResolved = extResolveResult.find( "resolved" );
		Check( extResolved && extResolved->isBool() && !extResolved->asBool(),
		       "RED-PROVE: External wire resolve_proposal reports resolved=false (Owner-only, refused at the SESSION layer)" );
		Check( LumR( *pJob ) == beforeExternalResolveAttempt,
		       "RED-PROVE: the External resolve attempt left the document UNCHANGED (would move to ~2.0 if it had applied)" );

		// The still-pending proposal can still be approved by the OWNER.
		char resolveLine3[256];
		std::snprintf( resolveLine3, sizeof( resolveLine3 ),
			"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"resolve_proposal\",\"params\":"
			"{\"proposalId\":%llu,\"approve\":false}}",
			static_cast<unsigned long long>( secondId ) );
		const std::string ownerRejectResp = ownerDisp.HandleLine( resolveLine3 );
		Agent::JsonValue ownerRejectResult;
		Check( JsonResultObj( ownerRejectResp, ownerRejectResult ), "owner wire resolve_proposal(reject) returns a result object" );
		const Agent::JsonValue* ownerRejectResolved = ownerRejectResult.find( "resolved" );
		Check( ownerRejectResolved && ownerRejectResolved->isBool() && ownerRejectResolved->asBool(),
		       "the OWNER's wire resolve_proposal(reject) on the SAME proposal succeeds (resolved=true) -- the External attempt above was a clean no-op, not a state corruption" );

		//------------------------------------------------------------------
		// Secure-MCP slice 5b fix round (P2-2) RED-PROVE: a REJECT's
		// headVersion is the REAL current head, not the {0,0} sentinel a
		// caller would otherwise confuse for "no session / unknown id".
		// AgentRpc.h documents "the CURRENT head, unmoved, on reject" --
		// SceneEditController::ResolveProposal's reject branch used to
		// return with *outResult left default-constructed, so this field
		// was silently {0,0} until the fix landed.
		//------------------------------------------------------------------
		const Agent::JsonValue* ownerRejectStatus = ownerRejectResult.find( "status" );
		Check( ownerRejectStatus && ownerRejectStatus->isString() && ownerRejectStatus->asString() == "rejected",
		       "the OWNER's wire resolve_proposal(reject) reports status=\"rejected\"" );
		const Agent::JsonValue* ownerRejectHv = ownerRejectResult.find( "headVersion" );
		Check( ownerRejectHv && ownerRejectHv->isObject(), "wire resolve_proposal(reject) carries a headVersion object" );
		if( ownerRejectHv ) {
			const Agent::JsonValue* u = ownerRejectHv->find( "uuid" );
			Check( u && u->isNumber() && u->asNumber() != 0.0,
			       "RED-PROVE: wire resolve_proposal(reject)'s headVersion carries a real (non-zero) uuid -- "
			       "the REAL current head, NOT the {0,0} sentinel (P2-2: the reject branch used to leave "
			       "*outResult default-constructed, silently reporting {0,0} instead of the documented "
			       "\"current head, unmoved\")" );
		}
		Check( LumR( *pJob ) == beforeExternalResolveAttempt,
		       "the current-head reject readback did not itself move the live scene (still the pre-reject value)" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 6: the pending-proposal queue cap
// (SceneEditController::kMaxPendingProposals) -- fill it to the cap via
// External wire propose_patch calls, prove the NEXT one is refused with a
// DISTINCT top-level JSON-RPC error (kProposalQueueFull, -32012 -- NOT a
// status="rejected" success envelope, which would be indistinguishable
// from a per-entity refusal), prove insert_chunk is refused the SAME way
// while the queue is still full (same gate, different verb), then prove
// resolving exactly ONE pending proposal frees exactly one slot -- the
// next stage succeeds again. RED-PROVE: temporarily removing the
// kMaxPendingProposals check in SceneEditController::StageProposal makes
// the overflow propose_patch/insert_chunk calls both succeed (status=
// "staged") instead of erroring, failing this test's core assertions.
//////////////////////////////////////////////////////////////////////
static void TestProposalQueueCapRedProve()
{
	std::cout << "Test 41: proposal pending-queue cap (Secure-MCP slice 6 RED-PROVE)..." << std::endl;

	const char* tmp = "agentlive_proposal_queuecap.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "queue-cap: base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/5 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "queue-cap: initial render fires" );

		std::unique_ptr<Agent::AgentSession> ownerSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		Check( ownerSess != nullptr, "queue-cap: owner session wraps the live Job" );
		if( ownerSess ) ownerSess->AttachController( &c );
		Agent::AgentRpcDispatcher ownerDisp( std::move( ownerSess ), Agent::AgentAutonomy::Commit );

		std::unique_ptr<Agent::AgentSession> extSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		Check( extSess != nullptr, "queue-cap: external session wraps the live Job" );
		if( extSess ) extSess->AttachController( &c );
		Agent::AgentRpcDispatcher extDisp( std::move( extSess ), Agent::AgentAutonomy::Commit );

		//------------------------------------------------------------------
		// Fill the pending queue to EXACTLY kMaxPendingProposals.
		//------------------------------------------------------------------
		const std::size_t cap = SceneEditController::kMaxPendingProposals;
		bool allStaged = true;
		for( std::size_t i = 0; i < cap; ++i ) {
			char line[256];
			std::snprintf( line, sizeof( line ),
				"{\"jsonrpc\":\"2.0\",\"id\":%zu,\"method\":\"propose_patch\",\"params\":"
				"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"%zu.0\"}}",
				i + 1, i + 10 );
			const std::string resp = extDisp.HandleLine( line );
			Agent::JsonValue result;
			if( !JsonResultObj( resp, result ) ) { allStaged = false; continue; }
			const Agent::JsonValue* status = result.find( "status" );
			if( !status || !status->isString() || status->asString() != "staged" ) allStaged = false;
		}
		Check( allStaged, "queue-cap: filled the pending queue to kMaxPendingProposals, every stage a clean \"staged\" success" );

		//------------------------------------------------------------------
		// The (cap+1)th propose_patch is REFUSED with a top-level JSON-RPC
		// error, not a success envelope.
		//------------------------------------------------------------------
		const std::string overflowResp = extDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":9000,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"99.0\"}}" );
		Agent::JsonValue overflowEnv;
		std::string perr;
		Check( Agent::JsonParse( overflowResp, overflowEnv, perr ), "queue-cap: overflow propose_patch response parses as JSON" );
		Check( overflowEnv.has( "error" ),
		       "RED-PROVE: the (cap+1)th propose_patch is a TOP-LEVEL JSON-RPC error, not the normal "
		       "success-envelope result shape every prior stage returned" );
		if( overflowEnv.has( "error" ) ) {
			const Agent::JsonValue& err = overflowEnv.get( "error" );
			const Agent::JsonValue* code = err.find( "code" );
			Check( code && code->isNumber() && code->asNumber() == -32012.0,
			       "RED-PROVE: the overflow error carries the distinct kProposalQueueFull code (-32012), "
			       "not kAutonomyRefused (-32011) or a standard JSON-RPC code" );
			const Agent::JsonValue* data = err.find( "data" );
			Check( data && data->isObject(), "overflow error carries a structured data object" );
			if( data ) {
				const Agent::JsonValue* verb = data->find( "verb" );
				Check( verb && verb->isString() && verb->asString() == "propose_patch",
				       "overflow error's data.verb names propose_patch" );
			}
		}

		//------------------------------------------------------------------
		// insert_chunk is refused the SAME way while the queue is still
		// full -- same gate (StageProposal), different verb.
		//------------------------------------------------------------------
		const std::string overflowInsertResp = extDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":9001,\"method\":\"insert_chunk\",\"params\":"
			"{\"chunkText\":\"uniformcolor_painter\\n{\\nname overflow_pad\\ncolor 1 1 1\\n}\\n\"}}" );
		Agent::JsonValue overflowInsertEnv;
		Check( Agent::JsonParse( overflowInsertResp, overflowInsertEnv, perr ), "queue-cap: overflow insert_chunk response parses as JSON" );
		Check( overflowInsertEnv.has( "error" ),
		       "insert_chunk is ALSO refused as a top-level error while the queue is full (same StageProposal gate)" );
		if( overflowInsertEnv.has( "error" ) ) {
			const Agent::JsonValue& err = overflowInsertEnv.get( "error" );
			const Agent::JsonValue* code = err.find( "code" );
			Check( code && code->isNumber() && code->asNumber() == -32012.0,
			       "overflow insert_chunk error ALSO carries kProposalQueueFull (-32012)" );
		}

		//------------------------------------------------------------------
		// list_proposals still shows EXACTLY `cap` entries -- the refused
		// overflow attempts never enqueued anything (fail-closed, not a
		// silent partial add).
		//------------------------------------------------------------------
		std::uint64_t firstPendingId = 0;
		{
			const std::string listResp = ownerDisp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":9002,\"method\":\"list_proposals\",\"params\":{}}" );
			Agent::JsonValue listResult;
			Check( JsonResultObj( listResp, listResult ), "queue-cap: list_proposals returns a result object" );
			const Agent::JsonValue* arr = listResult.find( "proposals" );
			Check( arr && arr->isArray() && arr->size() == cap,
			       "queue-cap: list_proposals shows EXACTLY kMaxPendingProposals entries -- "
			       "the refused overflow attempts never enqueued" );
			if( arr && arr->isArray() && arr->size() > 0 ) {
				const Agent::JsonValue& e = arr->at( 0 );
				const Agent::JsonValue* pid = e.find( "id" );
				if( pid && pid->isNumber() ) firstPendingId = static_cast<std::uint64_t>( pid->asNumber() );
			}
		}
		Check( firstPendingId != 0, "queue-cap: captured the first pending proposal's id" );

		//------------------------------------------------------------------
		// Resolve exactly ONE pending proposal -> frees exactly one slot.
		//------------------------------------------------------------------
		if( firstPendingId != 0 ) {
			char resolveLine[160];
			std::snprintf( resolveLine, sizeof( resolveLine ),
				"{\"jsonrpc\":\"2.0\",\"id\":9003,\"method\":\"resolve_proposal\",\"params\":"
				"{\"proposalId\":%llu,\"approve\":false}}",
				static_cast<unsigned long long>( firstPendingId ) );
			const std::string resolveResp = ownerDisp.HandleLine( resolveLine );
			Agent::JsonValue resolveResult;
			Check( JsonResultObj( resolveResp, resolveResult ), "queue-cap: resolve (reject) of the first proposal returns a result object" );
			const Agent::JsonValue* resolved = resolveResult.find( "resolved" );
			Check( resolved && resolved->isBool() && resolved->asBool(), "queue-cap: resolving the first pending proposal succeeds" );
		}

		//------------------------------------------------------------------
		// Now a fresh propose_patch succeeds again -- proves exactly one
		// slot was freed (not zero, not "still full").
		//------------------------------------------------------------------
		const std::string restageResp = extDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":9004,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"77.0\"}}" );
		Agent::JsonValue restageResult;
		Check( JsonResultObj( restageResp, restageResult ),
		       "queue-cap: after resolving one proposal, a fresh propose_patch returns a normal result object (not refused)" );
		const Agent::JsonValue* restageStatus = restageResult.find( "status" );
		Check( restageStatus && restageStatus->isString() && restageStatus->asString() == "staged",
		       "queue-cap: the freed slot lets a new proposal stage successfully" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 6: list_proposals' per-proposal echo cap (16 KiB on
// `value`/`chunkText`). A large insert_chunk chunkText is echoed back
// CLIPPED with truncated:true in the LISTING; the STORED proposal keeps
// the FULL text untouched -- proven by approving it and observing that
// the full chunk (name + trailing padding, well past 16 KiB) actually
// parsed and landed in the live scene (a clipped/truncated text would be
// missing its closing brace and FAIL to parse, never reaching
// status="applied").
//////////////////////////////////////////////////////////////////////
static void TestProposalListingTruncation()
{
	std::cout << "Test 42: list_proposals per-proposal echo truncation (Secure-MCP slice 6)..." << std::endl;

	const char* tmp = "agentlive_proposal_truncation.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "truncation: base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/5 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "truncation: initial render fires" );

		std::unique_ptr<Agent::AgentSession> ownerSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		Check( ownerSess != nullptr, "truncation: owner session wraps the live Job" );
		if( ownerSess ) ownerSess->AttachController( &c );
		Agent::AgentRpcDispatcher ownerDisp( std::move( ownerSess ), Agent::AgentAutonomy::Commit );

		std::unique_ptr<Agent::AgentSession> extSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		Check( extSess != nullptr, "truncation: external session wraps the live Job" );
		if( extSess ) extSess->AttachController( &c );
		Agent::AgentRpcDispatcher extDisp( std::move( extSess ), Agent::AgentAutonomy::Commit );

		// A valid, single insert_chunk chunk (a uniformcolor_painter),
		// padded with trailing `#` line comments so its TEXT exceeds the
		// 16 KiB echo cap while still parsing as exactly ONE chunk (the
		// comments are trivia, not additional chunks).
		std::string chunkText = "uniformcolor_painter\n{\nname bigpad\ncolor 0.25 0.5 0.75\n";
		const std::string padLine = "# padding-line-to-exceed-the-16KiB-echo-cap-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n";
		while( chunkText.size() < 20u * 1024u ) chunkText += padLine;
		chunkText += "}\n";
		Check( chunkText.size() > 16u * 1024u, "truncation: constructed chunkText exceeds the 16 KiB echo cap" );

		// JSON-escape the chunk text for the wire via Json.h's own
		// serializer rather than hand-rolling escaping.
		const Agent::JsonValue chunkTextJson = Agent::JsonValue::MakeString( chunkText );
		const std::string chunkTextEscaped = Agent::JsonSerialize( chunkTextJson );

		std::string insertLine = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"insert_chunk\",\"params\":{\"chunkText\":";
		insertLine += chunkTextEscaped;
		insertLine += "}}";
		const std::string insertResp = extDisp.HandleLine( insertLine );
		Agent::JsonValue insertResult;
		Check( JsonResultObj( insertResp, insertResult ), "truncation: external insert_chunk returns a result object" );
		const Agent::JsonValue* insertStatus = insertResult.find( "status" );
		Check( insertStatus && insertStatus->isString() && insertStatus->asString() == "staged",
		       "truncation: the large insert_chunk stages cleanly (staging is inert -- no size cap on the stored proposal)" );

		//------------------------------------------------------------------
		// list_proposals' echo is CLIPPED + flagged; the STORED proposal
		// (verified below, via approval) is untouched.
		//------------------------------------------------------------------
		const std::string listResp = ownerDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"list_proposals\",\"params\":{}}" );
		Agent::JsonValue listResult;
		Check( JsonResultObj( listResp, listResult ), "truncation: list_proposals returns a result object" );
		const Agent::JsonValue* arr = listResult.find( "proposals" );
		Check( arr && arr->isArray() && arr->size() == 1, "truncation: exactly one staged proposal is listed" );
		std::uint64_t proposalId = 0;
		if( arr && arr->isArray() && arr->size() == 1 ) {
			const Agent::JsonValue& e = arr->at( 0 );
			const Agent::JsonValue* truncated = e.find( "truncated" );
			Check( truncated && truncated->isBool() && truncated->asBool(),
			       "truncation: the listing carries truncated:true for the oversized chunkText" );
			const Agent::JsonValue* echoedChunkText = e.find( "chunkText" );
			Check( echoedChunkText && echoedChunkText->isString() && echoedChunkText->asString().size() == 16u * 1024u,
			       "truncation: the echoed chunkText is clipped to EXACTLY 16 KiB" );
			const Agent::JsonValue* pid = e.find( "id" );
			if( pid && pid->isNumber() ) proposalId = static_cast<std::uint64_t>( pid->asNumber() );
		}
		Check( proposalId != 0, "truncation: captured the staged proposal's id" );

		//------------------------------------------------------------------
		// Approve it -- the FULL (never-truncated) stored text must apply.
		//------------------------------------------------------------------
		if( proposalId != 0 ) {
			char resolveLine[128];
			std::snprintf( resolveLine, sizeof( resolveLine ),
				"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resolve_proposal\",\"params\":"
				"{\"proposalId\":%llu,\"approve\":true}}",
				static_cast<unsigned long long>( proposalId ) );
			const std::string resolveResp = ownerDisp.HandleLine( resolveLine );
			Agent::JsonValue resolveResult;
			Check( JsonResultObj( resolveResp, resolveResult ), "truncation: resolve_proposal(approve) returns a result object" );
			const Agent::JsonValue* resolved = resolveResult.find( "resolved" );
			Check( resolved && resolved->isBool() && resolved->asBool(), "truncation: the approve resolves" );
			const Agent::JsonValue* rstatus = resolveResult.find( "status" );
			Check( rstatus && rstatus->isString() && rstatus->asString() == "applied",
			       "RED-PROVE: the FULL chunkText parsed and applied cleanly -- a clipped-at-16KiB chunk "
			       "would be missing its closing brace and FAIL to parse (status would NOT be \"applied\")" );
		}

		// The chunk landed with its FULL content: the named painter this
		// (never-truncated) chunk declares actually exists in the live
		// derived scene.
		IPainter* pnt = pJob->GetPainters() ? pJob->GetPainters()->GetItem( "bigpad" ) : nullptr;
		Check( pnt != nullptr,
		       "truncation: the \"bigpad\" painter the FULL stored chunkText declares exists in the live scene -- "
		       "proof the STORED proposal (not the clipped listing echo) is what actually applied" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 6 fix round P1-TEST: the byte-length echo cap must
// walk back to a UTF-8 CHARACTER boundary, not clip on a raw byte
// boundary -- a clip landing mid multi-byte sequence emits invalid
// UTF-8 inside list_proposals' JSON (confirmed to blank the Mac owner-
// approval panel's strict JSONSerialization decode). This is a
// self-contained structural validator (not exhaustive Unicode
// validation -- just enough to catch a lone lead byte or an orphaned
// continuation byte at/near the tail, which is exactly what a raw
// byte-boundary clip mid-sequence produces).
//////////////////////////////////////////////////////////////////////
static bool IsStructurallyValidUtf8( const std::string& s )
{
	std::size_t i = 0;
	const std::size_t n = s.size();
	while( i < n )
	{
		const unsigned char b = static_cast<unsigned char>( s[i] );
		std::size_t seqLen;
		if( ( b & 0x80 ) == 0x00 )      seqLen = 1;
		else if( ( b & 0xE0 ) == 0xC0 ) seqLen = 2;
		else if( ( b & 0xF0 ) == 0xE0 ) seqLen = 3;
		else if( ( b & 0xF8 ) == 0xF0 ) seqLen = 4;
		else return false;   // invalid lead byte (stray continuation or 0xF8-0xFF)

		if( i + seqLen > n ) return false;   // sequence runs off the end -- truncated tail
		for( std::size_t k = 1; k < seqLen; ++k ) {
			if( ( static_cast<unsigned char>( s[i + k] ) & 0xC0 ) != 0x80 ) return false;
		}
		i += seqLen;
	}
	return true;
}

static void TestProposalListingUtf8BoundarySafety()
{
	std::cout << "Test 43: list_proposals echo cap walks back to a UTF-8 boundary (Secure-MCP slice 6 fix round P1)..." << std::endl;

	const char* tmp = "agentlive_proposal_utf8boundary.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "utf8-boundary: base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/5 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "utf8-boundary: initial render fires" );

		std::unique_ptr<Agent::AgentSession> ownerSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		Check( ownerSess != nullptr, "utf8-boundary: owner session wraps the live Job" );
		if( ownerSess ) ownerSess->AttachController( &c );
		Agent::AgentRpcDispatcher ownerDisp( std::move( ownerSess ), Agent::AgentAutonomy::Commit );

		std::unique_ptr<Agent::AgentSession> extSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		Check( extSess != nullptr, "utf8-boundary: external session wraps the live Job" );
		if( extSess ) extSess->AttachController( &c );
		Agent::AgentRpcDispatcher extDisp( std::move( extSess ), Agent::AgentAutonomy::Commit );

		//------------------------------------------------------------------
		// Build a chunkText whose bytes are an ASCII prefix (valid RISE
		// scene syntax + a comment marker) followed by a long run of the
		// 2-byte UTF-8 character U+00E9 ("e" acute, 0xC3 0xA9), SIZED so
		// the 16384-byte echo-cap boundary (index kCap-1 = 16383) lands
		// EXACTLY on the LEAD byte (0xC3) of one of those pairs -- i.e.
		// the byte-length clip alone would keep the lead byte but drop
		// its continuation byte, producing an invalid trailing sequence.
		//------------------------------------------------------------------
		const std::size_t kCap = 16u * 1024u;   // must match AgentRpc.cpp's kProposalFieldEchoCapBytes
		std::string prefix = "uniformcolor_painter\n{\nname utf8pad\ncolor 0.25 0.5 0.75\n#pad:";
		// Flip prefix parity (append one filler byte) if needed so that
		// (kCap - 1 - prefix.size()) is even -- i.e. the e-run's lead
		// bytes (at prefix.size() + 2*i) can land exactly on index kCap-1.
		if( ( ( kCap - 1 - prefix.size() ) % 2 ) != 0 ) prefix += "#";
		Check( prefix.size() < kCap - 1, "utf8-boundary: constructed prefix fits well before the cap boundary" );
		const std::size_t offsetIntoRun = ( kCap - 1 ) - prefix.size();   // even, by construction above
		const std::size_t pairsToBoundary = offsetIntoRun / 2 + 1;       // pairs needed so index kCap-1 is a lead byte
		const std::size_t totalPairs = pairsToBoundary + 64;             // plenty of padding past the boundary too

		std::string chunkText = prefix;
		for( std::size_t i = 0; i < totalPairs; ++i ) chunkText += "\xC3\xA9";   // U+00E9 "e" acute, 2 bytes each
		chunkText += "\n}\n";

		Check( chunkText.size() > kCap, "utf8-boundary: constructed chunkText exceeds the 16 KiB echo cap" );
		Check( static_cast<unsigned char>( chunkText[kCap - 1] ) == 0xC3,
		       "utf8-boundary: byte index kCap-1 of the constructed text is the LEAD byte of a 2-byte "
		       "sequence -- a raw byte-boundary clip would keep it and drop its continuation byte" );

		const Agent::JsonValue chunkTextJson = Agent::JsonValue::MakeString( chunkText );
		const std::string chunkTextEscaped = Agent::JsonSerialize( chunkTextJson );

		std::string insertLine = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"insert_chunk\",\"params\":{\"chunkText\":";
		insertLine += chunkTextEscaped;
		insertLine += "}}";
		const std::string insertResp = extDisp.HandleLine( insertLine );
		Agent::JsonValue insertResult;
		Check( JsonResultObj( insertResp, insertResult ), "utf8-boundary: external insert_chunk returns a result object" );
		const Agent::JsonValue* insertStatus = insertResult.find( "status" );
		Check( insertStatus && insertStatus->isString() && insertStatus->asString() == "staged",
		       "utf8-boundary: the large insert_chunk stages cleanly" );

		//------------------------------------------------------------------
		// list_proposals' echo must be VALID UTF-8, <= the cap, and still
		// flagged truncated -- the stored proposal is untouched.
		//------------------------------------------------------------------
		const std::string listResp = ownerDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"list_proposals\",\"params\":{}}" );
		Agent::JsonValue listResult;
		Check( JsonResultObj( listResp, listResult ), "utf8-boundary: list_proposals returns a result object" );
		const Agent::JsonValue* arr = listResult.find( "proposals" );
		Check( arr && arr->isArray() && arr->size() == 1, "utf8-boundary: exactly one staged proposal is listed" );
		std::uint64_t proposalId = 0;
		if( arr && arr->isArray() && arr->size() == 1 ) {
			const Agent::JsonValue& e = arr->at( 0 );
			const Agent::JsonValue* truncated = e.find( "truncated" );
			Check( truncated && truncated->isBool() && truncated->asBool(),
			       "utf8-boundary: the listing carries truncated:true for the oversized chunkText" );
			const Agent::JsonValue* echoedChunkText = e.find( "chunkText" );
			Check( echoedChunkText && echoedChunkText->isString(), "utf8-boundary: chunkText is echoed as a string" );
			if( echoedChunkText && echoedChunkText->isString() ) {
				const std::string& echoed = echoedChunkText->asString();
				Check( echoed.size() <= kCap,
				       "utf8-boundary: the echoed chunkText is AT MOST 16 KiB (may be a few bytes under, "
				       "not necessarily exactly == kCap, since the boundary walk-back may drop a partial "
				       "trailing sequence)" );
				const unsigned char lastByte = echoed.empty() ? 0 : static_cast<unsigned char>( echoed.back() );
				Check( !echoed.empty() && !( lastByte >= 0xC0 ),
				       "RED-PROVE: the echoed chunkText's last byte is NOT a UTF-8 lead byte (0xC0-0xFF) -- "
				       "a raw byte-boundary clip would leave a lone lead byte with its continuation dropped" );
				Check( IsStructurallyValidUtf8( echoed ),
				       "RED-PROVE: the echoed chunkText decodes as structurally valid UTF-8 end to end" );
			}
			const Agent::JsonValue* pid = e.find( "id" );
			if( pid && pid->isNumber() ) proposalId = static_cast<std::uint64_t>( pid->asNumber() );
		}
		Check( proposalId != 0, "utf8-boundary: captured the staged proposal's id" );

		//------------------------------------------------------------------
		// Approve it -- the FULL (never-clipped) stored text must apply,
		// proving the walk-back never touched the STORED proposal, only
		// the listing echo.
		//------------------------------------------------------------------
		if( proposalId != 0 ) {
			char resolveLine[128];
			std::snprintf( resolveLine, sizeof( resolveLine ),
				"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resolve_proposal\",\"params\":"
				"{\"proposalId\":%llu,\"approve\":true}}",
				static_cast<unsigned long long>( proposalId ) );
			const std::string resolveResp = ownerDisp.HandleLine( resolveLine );
			Agent::JsonValue resolveResult;
			Check( JsonResultObj( resolveResp, resolveResult ), "utf8-boundary: resolve_proposal(approve) returns a result object" );
			const Agent::JsonValue* rstatus = resolveResult.find( "status" );
			Check( rstatus && rstatus->isString() && rstatus->asString() == "applied",
			       "utf8-boundary: the FULL stored chunkText (never clipped) parsed and applied cleanly" );
		}
		IPainter* pnt = pJob->GetPainters() ? pJob->GetPainters()->GetItem( "utf8pad" ) : nullptr;
		Check( pnt != nullptr,
		       "utf8-boundary: the \"utf8pad\" painter the FULL stored chunkText declares exists in the live scene" );

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 6 fix round P2-1: prove the history-eviction filter
// (StageProposal's "evict the single oldest RESOLVED entry once
// mProposals.size() >= kMaxProposalHistory" step) never evicts a
// still-PENDING proposal, even one held at the FRONT of the vector
// (the oldest position, and therefore the first eviction candidate if
// the status filter were ever weakened to ignore status entirely).
//////////////////////////////////////////////////////////////////////
static void TestProposalHistoryEvictionSurvivesPending()
{
	std::cout << "Test 44: proposal history eviction never evicts a pending proposal (Secure-MCP slice 6 fix round P2-1)..." << std::endl;

	const char* tmp = "agentlive_proposal_historyevict.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "history-evict: base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/5 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "history-evict: initial render fires" );

		std::unique_ptr<Agent::AgentSession> ownerSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		Check( ownerSess != nullptr, "history-evict: owner session wraps the live Job" );
		if( ownerSess ) ownerSess->AttachController( &c );
		Agent::AgentRpcDispatcher ownerDisp( std::move( ownerSess ), Agent::AgentAutonomy::Commit );

		std::unique_ptr<Agent::AgentSession> extSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		Check( extSess != nullptr, "history-evict: external session wraps the live Job" );
		if( extSess ) extSess->AttachController( &c );
		Agent::AgentRpcDispatcher extDisp( std::move( extSess ), Agent::AgentAutonomy::Commit );

		//------------------------------------------------------------------
		// Stage ONE proposal and hold it PENDING at the FRONT (oldest
		// position) of mProposals -- never resolved through this test.
		//------------------------------------------------------------------
		const std::string heldStageResp = extDisp.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"propose_patch\",\"params\":"
			"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"5.0\"}}" );
		Agent::JsonValue heldStageResult;
		Check( JsonResultObj( heldStageResp, heldStageResult ), "history-evict: the held proposal stages cleanly" );
		const Agent::JsonValue* heldStatus = heldStageResult.find( "status" );
		Check( heldStatus && heldStatus->isString() && heldStatus->asString() == "staged",
		       "history-evict: the held proposal's status is \"staged\"" );

		std::uint64_t heldId = 0;
		{
			const std::string listResp = ownerDisp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"list_proposals\",\"params\":{}}" );
			Agent::JsonValue listResult;
			Check( JsonResultObj( listResp, listResult ), "history-evict: list_proposals returns a result object" );
			const Agent::JsonValue* arr = listResult.find( "proposals" );
			Check( arr && arr->isArray() && arr->size() == 1, "history-evict: exactly the held proposal is listed so far" );
			if( arr && arr->isArray() && arr->size() == 1 ) {
				const Agent::JsonValue* pid = arr->at( 0 ).find( "id" );
				if( pid && pid->isNumber() ) heldId = static_cast<std::uint64_t>( pid->asNumber() );
			}
		}
		Check( heldId != 0, "history-evict: captured the held pending proposal's id" );

		//------------------------------------------------------------------
		// Grow mProposals well past kMaxProposalHistory (256) via
		// stage+reject cycling -- each cycle adds exactly one RESOLVED
		// (rejected) entry after the held pending one, so the eviction
		// filter has plenty of resolved candidates to evict OTHER than
		// the held entry.
		//------------------------------------------------------------------
		const std::size_t kMaxHistory = SceneEditController::kMaxProposalHistory;
		const std::size_t cycles = kMaxHistory + 40;   // comfortably past the cap, several evictions occur
		bool allCyclesOk = true;
		for( std::size_t i = 0; i < cycles; ++i ) {
			char stageLine[256];
			std::snprintf( stageLine, sizeof( stageLine ),
				"{\"jsonrpc\":\"2.0\",\"id\":%zu,\"method\":\"propose_patch\",\"params\":"
				"{\"target\":\"lum\",\"kind\":\"lambertian_luminaire_material\",\"param\":\"scale\",\"value\":\"%zu.0\"}}",
				100 + i, 10 + ( i % 50 ) );
			const std::string stageResp = extDisp.HandleLine( stageLine );
			Agent::JsonValue stageResult;
			std::uint64_t cycId = 0;
			if( JsonResultObj( stageResp, stageResult ) ) {
				const Agent::JsonValue* st = stageResult.find( "status" );
				if( !st || !st->isString() || st->asString() != "staged" ) allCyclesOk = false;
			} else {
				allCyclesOk = false;
			}
			// Find the just-staged proposal's id via list_proposals (it is
			// always the LAST entry -- the held one is oldest/first, and
			// nothing else is ever left pending).
			const std::string listResp = ownerDisp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":9999,\"method\":\"list_proposals\",\"params\":{}}" );
			Agent::JsonValue listResult;
			if( JsonResultObj( listResp, listResult ) ) {
				const Agent::JsonValue* arr = listResult.find( "proposals" );
				if( arr && arr->isArray() && arr->size() > 0 ) {
					const Agent::JsonValue& last = arr->at( arr->size() - 1 );
					const Agent::JsonValue* pid = last.find( "id" );
					if( pid && pid->isNumber() ) cycId = static_cast<std::uint64_t>( pid->asNumber() );
				}
			}
			if( cycId != 0 ) {
				char resolveLine[160];
				std::snprintf( resolveLine, sizeof( resolveLine ),
					"{\"jsonrpc\":\"2.0\",\"id\":%zu,\"method\":\"resolve_proposal\",\"params\":"
					"{\"proposalId\":%llu,\"approve\":false}}",
					200 + i, static_cast<unsigned long long>( cycId ) );
				ownerDisp.HandleLine( resolveLine );
			} else {
				allCyclesOk = false;
			}
		}
		Check( allCyclesOk, "history-evict: all stage+reject cycles completed cleanly" );

		//------------------------------------------------------------------
		// RED-PROVE: the held pending proposal SURVIVES -- still listable
		// (with status "pending") and still resolvable, even though
		// mProposals has been driven well past kMaxProposalHistory and the
		// held entry has sat at the OLDEST position the entire time (the
		// first eviction candidate a status-blind filter would pick).
		//------------------------------------------------------------------
		bool heldStillListed = false;
		{
			const std::string listResp = ownerDisp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"list_proposals\",\"params\":{}}" );
			Agent::JsonValue listResult;
			Check( JsonResultObj( listResp, listResult ), "history-evict: final list_proposals returns a result object" );
			const Agent::JsonValue* arr = listResult.find( "proposals" );
			Check( arr && arr->isArray(), "history-evict: final list_proposals carries a proposals array" );
			if( arr && arr->isArray() ) {
				for( std::size_t i = 0; i < arr->size(); ++i ) {
					const Agent::JsonValue& e = arr->at( i );
					const Agent::JsonValue* pid = e.find( "id" );
					if( pid && pid->isNumber() && static_cast<std::uint64_t>( pid->asNumber() ) == heldId ) {
						heldStillListed = true;
						const Agent::JsonValue* st = e.find( "status" );
						Check( st && st->isString() && st->asString() == "pending",
						       "RED-PROVE: the held proposal's status is still \"pending\" after the history churn" );
						break;
					}
				}
			}
		}
		Check( heldStillListed, "RED-PROVE: the held pending proposal is STILL LISTED after growing history past "
		       "kMaxProposalHistory entirely via OTHER (resolved) entries -- a status-blind eviction filter would "
		       "have evicted it first, since it sat at the oldest position throughout" );

		if( heldStillListed ) {
			char resolveLine[160];
			std::snprintf( resolveLine, sizeof( resolveLine ),
				"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resolve_proposal\",\"params\":"
				"{\"proposalId\":%llu,\"approve\":false}}",
				static_cast<unsigned long long>( heldId ) );
			const std::string resolveResp = ownerDisp.HandleLine( resolveLine );
			Agent::JsonValue resolveResult;
			Check( JsonResultObj( resolveResp, resolveResult ), "history-evict: resolving the held proposal returns a result object" );
			const Agent::JsonValue* resolved = resolveResult.find( "resolved" );
			Check( resolved && resolved->isBool() && resolved->asBool(),
			       "RED-PROVE: the held pending proposal is STILL RESOLVABLE after the history churn" );
		}

		c.Stop();
	}
	pJob->release();
	std::remove( tmp );
}

//////////////////////////////////////////////////////////////////////
// A TRANSIENTLY refused resolve is reported as "did not resolve", not
// as "resolved and rejected" / "unknown id".
//
// SceneEditController::ResolveProposal leaves the proposal PENDING on
// both transient refusals -- the render-admission gate (it returns
// false WITHOUT ever consulting the queue) and a replay that hit an
// open editor transaction/gesture (it returns true with a retriable
// AgentCommitResult).  Before this fix AgentSession folded the first
// into "no pending proposal with that id (unknown id, or it was already
// resolved)" and the second into ok=true/status="rejected": in both
// cases a still-approvable proposal was reported as permanently gone.
//
// RED-PROVE for each leg is the pair (!ok, retriable) plus the
// still-"pending" listing, and then the SAME resolve succeeding once
// the gate clears -- the property the old shape denied.
//////////////////////////////////////////////////////////////////////
static void TestTransientResolveLeavesProposalPending()
{
	std::cout << "Test 45: a transiently refused resolve reports !ok+retriable and stays PENDING..." << std::endl;

	const char* tmp = "agentlive_resolve_transient.RISEscene";
	Job* pJob = LoadScene( kBaseScene, tmp );
	Check( pJob != nullptr, "base scene loads via the CST path" );
	if( !pJob ) return;

	{
		TestController c( *pJob, /*simulatedRenderMs*/20 );
		c.Start();
		Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render fires" );

		std::unique_ptr<Agent::AgentSession> ownerSess =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
		std::unique_ptr<Agent::AgentSession> ext =
			Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::External );
		Check( ownerSess != nullptr && ext != nullptr, "both sessions wrap the live Job" );
		if( !ownerSess || !ext ) { c.Stop(); pJob->release(); std::remove( tmp ); return; }
		ownerSess->AttachController( &c );
		ext->AttachController( &c );
		Agent::AgentSession* owner = ownerSess.get();

		auto stagePending = [&]( const char* value ) -> std::uint64_t {
			Agent::AgentSetPatch p;
			p.target = "lum";
			p.kind   = "lambertian_luminaire_material";
			p.param  = "scale";
			p.value  = value;
			const Agent::AgentPatchResult pr = ext->ProposePatch( p );
			Check( pr.status == "staged", "external propose stages the proposal" );
			std::uint64_t staged = 0;
			for( const auto& e : owner->ListProposals() ) if( e.status == "pending" ) staged = e.id;
			return staged;
		};
		auto isPending = [&]( std::uint64_t id ) -> bool {
			for( const auto& e : owner->ListProposals() ) if( e.id == id ) return e.status == "pending";
			return false;
		};

		//------------------------------------------------------------------
		// (a) OPEN EDITOR TRANSACTION -- the replay refuses, the controller
		// leaves the entry pending (its TRANSIENT-REFUSAL EXCEPTION).
		//------------------------------------------------------------------
		const std::uint64_t txnId = stagePending( "9.25" );
		Check( txnId != 0, "the txn-leg proposal is staged pending" );
		const double beforeTxn = LumR( *pJob );
		Check( c.BeginTransaction(), "transaction opens" );

		const Agent::AgentSession::AgentResolveResult rrTxn = owner->ResolveProposal( txnId, /*approve=*/true );
		std::cout << "    [txn leg] ok=" << rrTxn.ok << " retriable=" << rrTxn.retriable
		          << " status=\"" << rrTxn.status << "\" message=\"" << rrTxn.message << "\"" << std::endl;
		Check( !rrTxn.ok,
		       "MONEY RED-PROVE: a transaction-refused approve reports ok=FALSE -- it used to report "
		       "ok=true/status=\"rejected\" for a proposal the controller left PENDING" );
		Check( rrTxn.retriable, "MONEY: the refusal is flagged retriable (transient, resolvable again)" );
		Check( rrTxn.status.empty(), "no resolve ran, so `status` stays empty (AgentRpc.h: empty when resolved is false)" );
		Check( rrTxn.message.find( "transaction" ) != std::string::npos,
		       "the message names the transaction that is actually blocking" );
		Check( isPending( txnId ), "MONEY RED-PROVE: the proposal is STILL PENDING after the refused approve" );
		Check( LumR( *pJob ) == beforeTxn, "the refused approve left the document UNCHANGED" );

		// The same refusal on the WIRE: resolved=false AND retriable=true.
		{
			std::unique_ptr<Agent::AgentSession> wireSess =
				Agent::AgentSession::WrapJob( pJob, Agent::AgentAuthority::Owner );
			Check( wireSess != nullptr, "wire owner session wraps the live Job" );
			if( wireSess ) wireSess->AttachController( &c );
			Agent::AgentRpcDispatcher wireDisp( std::move( wireSess ), Agent::AgentAutonomy::Commit );
			char line[200];
			std::snprintf( line, sizeof( line ),
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resolve_proposal\",\"params\":"
				"{\"proposalId\":%llu,\"approve\":true}}",
				static_cast<unsigned long long>( txnId ) );
			Agent::JsonValue wireResult;
			Check( JsonResultObj( wireDisp.HandleLine( line ), wireResult ),
			       "wire resolve_proposal returns a result object mid-transaction" );
			const Agent::JsonValue* wResolved = wireResult.find( "resolved" );
			const Agent::JsonValue* wRetriable = wireResult.find( "retriable" );
			Check( wResolved && wResolved->isBool() && !wResolved->asBool(),
			       "MONEY RED-PROVE: the wire reports resolved=false mid-transaction" );
			Check( wRetriable && wRetriable->isBool() && wRetriable->asBool(),
			       "MONEY: the wire carries retriable=true, so a client can tell this apart from an unknown id" );
			Check( isPending( txnId ), "the wire attempt also left the proposal pending" );
		}

		Check( c.RollbackTransaction(), "transaction rolls back" );

		// The gate cleared -- the SAME resolve now works.  This is the whole
		// point of leaving it pending.
		const Agent::AgentSession::AgentResolveResult rrTxn2 = owner->ResolveProposal( txnId, /*approve=*/true );
		Check( rrTxn2.ok && rrTxn2.status == "applied",
		       "MONEY: the SAME proposal resolves cleanly once the transaction closes (it was not burned)" );
		Check( !rrTxn2.retriable, "a real resolve carries retriable=false" );
		Check( LumR( *pJob ) > 8.5 && LumR( *pJob ) < 10.0, "the retried approve reached the live scene (~9.25)" );

		//------------------------------------------------------------------
		// (b) RENDER-ADMISSION GATE -- SceneEditController::ResolveProposal
		// returns false WITHOUT consulting the queue.  This used to reach
		// the model as "no pending proposal with that id".
		//------------------------------------------------------------------
		const std::uint64_t busyId = stagePending( "2.0" );
		Check( busyId != 0, "the busy-leg proposal is staged pending" );
		const double beforeBusy = LumR( *pJob );

		std::atomic<bool> release{ false };
		std::atomic<bool> entered{ false };
		SceneEditController::RenderJobId jobId = 0;
		const bool accepted = c.SubmitAgentRenderAsync(
			[&] {
				entered.store( true, std::memory_order_release );
				while( !release.load( std::memory_order_acquire ) )
					std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			},
			String( "resolve-transient-occupant" ), &jobId );
		Check( accepted && jobId != 0, "the occupying agent render is accepted" );
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 5000 );
			while( !entered.load( std::memory_order_acquire ) &&
			       std::chrono::steady_clock::now() < deadline )
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
		Check( entered.load( std::memory_order_acquire ), "the occupying agent render is running" );
		Check( !c.IsTransactionOpen(), "PRECONDITION: NO transaction is open in this leg" );

		const Agent::AgentSession::AgentResolveResult rrBusy = owner->ResolveProposal( busyId, /*approve=*/true );
		std::cout << "    [busy leg] ok=" << rrBusy.ok << " retriable=" << rrBusy.retriable
		          << " message=\"" << rrBusy.message << "\"" << std::endl;
		Check( !rrBusy.ok, "the admission-gate refusal reports ok=false" );
		Check( rrBusy.retriable, "MONEY: it is flagged retriable -- the render clears and the proposal survives" );
		Check( rrBusy.message.find( "no pending proposal with that id" ) == std::string::npos,
		       "MONEY RED-PROVE: it does NOT claim the id is unknown/already resolved -- that permanent "
		       "message was what a transient render-admission refusal used to report" );
		Check( rrBusy.message.find( "render" ) != std::string::npos,
		       "the message names the render that is actually blocking" );
		Check( isPending( busyId ), "MONEY RED-PROVE: the proposal is STILL PENDING after the gate refusal" );
		Check( LumR( *pJob ) == beforeBusy, "the gate refusal left the document UNCHANGED" );

		release.store( true, std::memory_order_release );
		Check( c.WaitForRenderJob( jobId, 15000 ), "the occupying agent render completes" );

		const Agent::AgentSession::AgentResolveResult rrBusy2 = owner->ResolveProposal( busyId, /*approve=*/true );
		Check( rrBusy2.ok && rrBusy2.status == "applied",
		       "MONEY: the SAME proposal resolves cleanly once the render releases the gate" );
		Check( LumR( *pJob ) > 1.5 && LumR( *pJob ) < 2.5, "the retried approve reached the live scene (~2.0)" );

		//------------------------------------------------------------------
		// (c) CONTROL: a genuinely unknown id is still PERMANENT -- the new
		// `retriable` bit must not smear the two together.
		//------------------------------------------------------------------
		const Agent::AgentSession::AgentResolveResult rrUnknown = owner->ResolveProposal( 987654321u, /*approve=*/true );
		Check( !rrUnknown.ok && !rrUnknown.retriable,
		       "an unknown id is ok=false AND retriable=false (permanent, not a retry invitation)" );
		Check( rrUnknown.message.find( "no pending proposal with that id" ) != std::string::npos,
		       "the unknown-id message is unchanged" );

		// And re-resolving an ALREADY-resolved proposal stays permanent too.
		const Agent::AgentSession::AgentResolveResult rrAgain = owner->ResolveProposal( busyId, /*approve=*/true );
		Check( !rrAgain.ok && !rrAgain.retriable,
		       "re-resolving an already-applied proposal is ok=false AND retriable=false" );

		c.Stop();
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
	TestActionableRejectionDiagnosticsLive();
	TestInsertChunksLive();
	TestCodeThreeRerender();
	TestTransformCommitCodeThree();
	TestAgentEditMarksDirty();
	TestLiveDispatcherPath();
	TestMidTransactionAgentCommitRefused();
	TestPreviewRenderParked();
	TestAgentEditUndoRedo();
	TestAgentEmissiveEditBumpsLightGeneration();
	TestAgentNonMaterialEditDoesNotBumpLightGeneration();
	TestAgentUnknownKindEditBumpsConservatively();
	TestInterleavedLifoUndo();
	TestAgentEditDefaultedSlotUndo();
	TestCodeThreeUndo();
	TestConflictGateAfterUndo();
	TestUndoToCleanDirtyState();
	TestMultiTokenParamUndo();
	TestOccAwareParamRemoveUndo();
	TestArmedCodeThreeRevert();
	TestUndoRedoAfterSaveMarksDirty();
	TestChunkInsertUndoRedo();
	TestChunkRemoveMiddleUndoRedo();
	TestChunkInsertThenParamEditInterleavedLifo();
	TestUndoInsertRefusedWhenReferenced();
	TestRasterizerInsertUndoMatchesReload();
	TestCodeThreeArmedChunkCrudUndoRedo();
	TestVariantOverlayInsertUndoP1A();
	TestGlueSafeRestoreAfterOutOfBandShiftP1B();
	TestStaleIndexIdentityCheckRefusesP2();
	TestGlueSafeRestoreRightSideAfterOutOfBandShiftP1Round2();
	TestGlueSafeRestoreLastChunkAppendP1Round2();
	TestProposalStagingBasics();
	TestStaleApproveConflict();
	TestReloadInvalidatesStaleProposal();
	TestTwoProposalsSameParamSecondIsStale();
	TestAuthorityMatrixRefusals();
	TestChunkCrudStaging();
	TestExternalStageBaseVersionCoherentUnderConcurrency();
	TestProposalWireRoundTrip();
	TestProposalQueueCapRedProve();
	TestProposalListingTruncation();
	TestProposalListingUtf8BoundarySafety();
	TestProposalHistoryEvictionSurvivesPending();
	TestTransientResolveLeavesProposalPending();

	std::cout << "\n=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
