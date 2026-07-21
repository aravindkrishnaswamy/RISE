//////////////////////////////////////////////////////////////////////
//
//  ViewportPaneSchedulerTest.cpp - P3a slice 2: the context-switch
//    pane scheduler (docs/gui/RENDER_MODES.md §7.3).
//
//  THE MONEY EVIDENCE for the rotation: a recording DoOneRenderPass
//  subclass captures the (pane) sequence the render thread actually
//  executes, and the assertions check the SCHEDULING CONTRACT --
//  priority order, pane-local invalidation, gesture pinning, rotation
//  quiescence -- with no real rendering (skeleton mode, null
//  interactive rasterizer).
//
//  Determinism notes: every scenario drives the loop via KickRender /
//  config setters, then WAITS on the recorder (condition variable with
//  a generous timeout) for an EXACT number of passes; after each
//  scenario we assert quiescence (no further passes within a settle
//  window) so a runaway rotation cannot pass by accident.  The pass
//  body sleeps ~2ms so gesture tests have a real window to overlap.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace RISE;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

//----------------------------------------------------------------------
// Minimal scene: one camera, one box.  Skeleton-mode controllers never
// rasterize, but the controller requires a loaded scene.
//----------------------------------------------------------------------
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 1\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 32\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj\n\tgeometry geo\n\tmaterial mat\n}\n";

static std::string WriteTemp( const char* name, const char* content )
{
	std::string path = std::string( "/tmp/" ) + name;
	std::ofstream f( path.c_str(), std::ios::trunc );
	if( !f ) return std::string();
	f << content;
	return path;
}

//----------------------------------------------------------------------
// The recorder.  Captures the pane id per pass via ForTest_CurrentPane
// -- safe from inside DoOneRenderPass because the mint lock is released
// before the pass body runs (see the override's comment).
//----------------------------------------------------------------------
class RecordingController : public SceneEditController
{
public:
	RecordingController( IJobPriv& job )
	: SceneEditController( job, /*interactiveRasterizer*/ nullptr )
	{}

	std::vector<unsigned int> Sequence() const
	{
		std::lock_guard<std::mutex> lk( mSeqMutex );
		return mSeq;
	}

	//! Blocks until the recorded sequence reaches `count` passes (true)
	//! or `timeoutMs` elapses (false).
	bool WaitForPassCount( std::size_t count, unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mSeqMutex );
		return mSeqCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mSeq.size() >= count; } );
	}

	//! True when NO further pass lands within `settleMs` -- rotation
	//! quiescence.  (A sleeping loop stays asleep; a runaway one keeps
	//! appending and fails this.)
	bool SettlesAt( std::size_t count, unsigned int settleMs )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( settleMs ) );
		std::lock_guard<std::mutex> lk( mSeqMutex );
		return mSeq.size() == count;
	}

	void ClearSequence()
	{
		std::lock_guard<std::mutex> lk( mSeqMutex );
		mSeq.clear();
	}

protected:
	void DoOneRenderPass() override
	{
		// ForTest_CurrentPane locks mMutex -- safe HERE because the mint
		// lock is released before DoOneRenderPass runs (the lock_guard
		// scope closes at the mint block's end), and mCurrentPane is
		// stable for the whole pass (only mutated inside the mint lock,
		// before mRendering flips true).
		{
			std::lock_guard<std::mutex> lk( mSeqMutex );
			mSeq.push_back( ForTest_CurrentPane() );
		}
		mSeqCV.notify_all();
		// A small real duration so gesture scenarios can overlap a pass.
		std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
	}

private:
	mutable std::mutex          mSeqMutex;
	std::condition_variable     mSeqCV;
	std::vector<unsigned int>   mSeq;
};

struct Fixture
{
	Job*                  job = nullptr;
	RecordingController*  ctrl = nullptr;

	explicit Fixture( const char* tmpName )
	{
		const std::string path = WriteTemp( tmpName, kScene );
		job = new Job();
		if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) { job->release(); job = nullptr; return; }
		ctrl = new RecordingController( *job );
	}
	~Fixture()
	{
		if( ctrl ) { ctrl->Stop(); delete ctrl; }
		if( job ) job->release();
	}
};

static const unsigned int kWaitMs   = 4000;
static const unsigned int kSettleMs = 250;

//----------------------------------------------------------------------
// (a) MONEY: rotation order after a scene edit -- primary first, then
//     secondaries by index (§7.3 rule 2).
//----------------------------------------------------------------------
static void RunRotationOrderTest()
{
	std::printf( "=== scheduler (a): edit invalidates all; rotation is primary-first, then index order ===\n" );
	Fixture f( "pane_sched_a.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	Check( f.ctrl->SetPrimaryPane( 2 ), "primary = pane 2" );

	// Start SUPPRESSING the initial render: the loop parks until the kick
	// below, so the recorded sequence begins at OUR edit, deterministic.
	f.ctrl->Start( /*suppressInitialRender*/ true );
	f.ctrl->ForTest_KickRender();

	// One scene edit -> all four visible panes dirty -> exactly 4 passes:
	// primary (2) first, then 0, 1, 3.
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "4 passes complete after one edit" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	Check( seq.size() >= 4 && seq[0] == 2,
	       "MONEY ASSERTION (a): the PRIMARY pane (2) renders FIRST after an edit" );
	Check( seq.size() >= 4 && seq[1] == 0 && seq[2] == 1 && seq[3] == 3,
	       "MONEY ASSERTION (a): secondaries follow in index order (0, 1, 3)" );
	Check( f.ctrl->SettlesAt( seq.size(), kSettleMs ),
	       "rotation QUIESCES once every pane has rendered (no runaway passes)" );

	// ROUND 2 -- the same contract driven by a REAL edit-invalidation.
	// Round 1 alone is a weak witness: PaneRenderState.dirty defaults
	// TRUE at construction, so the first rotation ever runs off default
	// state and passes even if the edit path never marks a pane (caught
	// by mutation-testing MarkAllVisiblePanesDirtyLocked_: round 1
	// survived the mutation, round 2 does not).
	f.ctrl->ClearSequence();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
	       "MONEY ASSERTION (a, round 2): a SECOND edit re-invalidates ALL panes (4 fresh passes)" );
	const std::vector<unsigned int> seq2 = f.ctrl->Sequence();
	Check( seq2.size() >= 4 && seq2[0] == 2 && seq2[1] == 0 && seq2[2] == 1 && seq2[3] == 3,
	       "MONEY ASSERTION (a, round 2): same order (primary 2, then 0, 1, 3) from the edit path" );
	Check( f.ctrl->SettlesAt( seq2.size(), kSettleMs ), "round 2 quiesces too" );
}

//----------------------------------------------------------------------
// (b) Pane-local config change re-renders THAT pane only (§7.3
//     invalidation matrix row 2).
//----------------------------------------------------------------------
static void RunPaneLocalInvalidationTest()
{
	std::printf( "=== scheduler (b): pane-local mode change re-renders that pane only ===\n" );
	Fixture f( "pane_sched_b.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "initial rotation completes (4 passes)" );
	f.ctrl->ClearSequence();

	// A mode change on pane 3 is NOT a scene edit: only pane 3 re-renders.
	Check( f.ctrl->SetPaneRenderMode( 3, "normals" ), "SetPaneRenderMode(3, normals)" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "one pass follows the pane-local change" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	Check( !seq.empty() && seq[0] == 3,
	       "MONEY ASSERTION (b): the changed pane (3) is the one that re-renders" );
	Check( f.ctrl->SettlesAt( seq.size(), kSettleMs ),
	       "MONEY ASSERTION (b): NO other pane re-renders (their content is still valid)" );
}

//----------------------------------------------------------------------
// (c) Gesture pinning: while the pointer is down, passes pin to the
//     gestured pane (pane 0 until slice-3 routing) even with other
//     panes dirty; the rotation resumes after release (§7.3).
//----------------------------------------------------------------------
static void RunGesturePinningTest()
{
	std::printf( "=== scheduler (c): gesture pins the scheduler; rotation resumes on release ===\n" );
	Fixture f( "pane_sched_c.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "layout TwoH" );
	Check( f.ctrl->SetPrimaryPane( 1 ), "primary = pane 1 (so pinning is distinguishable from priority)" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "initial rotation completes (2 passes)" );
	f.ctrl->ClearSequence();

	// Pointer down forces pane 0 current and pins the pick.  Every edit
	// the gesture generates renders pane 0 ONLY, even though each edit
	// marks pane 1 (the primary!) dirty too.
	f.ctrl->OnPointerDown( Point2( 5, 5 ) );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "a gesture-window pass lands" );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "a second gesture-window pass lands" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		bool allPane0 = !seq.empty();
		for( std::size_t i = 0; i < seq.size(); ++i ) {
			if( seq[i] != 0 ) allPane0 = false;
		}
		Check( allPane0,
		       "MONEY ASSERTION (c): every pass during the gesture is pane 0 -- the PRIMARY (1) is dirty "
		       "but the gesture pin outranks priority" );
	}

	// Release: the dirty primary now gets its pass (rotation resumes).
	f.ctrl->OnPointerUp( Point2( 5, 5 ) );
	Check( f.ctrl->WaitForPassCount( f.ctrl->Sequence().size() + 1, kWaitMs ),
	       "rotation resumes after pointer-up" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		bool sawPane1 = false;
		for( std::size_t i = 0; i < seq.size(); ++i ) {
			if( seq[i] == 1 ) sawPane1 = true;
		}
		Check( sawPane1,
		       "MONEY ASSERTION (c): the dirty primary (1) renders once the gesture ends" );
	}

	// review-r2-B P2 regression: the polish-chain continuation after a
	// gesture must NOT masquerade as a scene edit (which would spuriously
	// re-dirty and re-render every settled pane on every gesture end).
	// Let the polish chain drain, then require quiescence.
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
		const std::size_t settled = f.ctrl->Sequence().size();
		Check( f.ctrl->SettlesAt( settled, kSettleMs ),
		       "gesture-end polish chain quiesces without re-rendering settled panes "
		       "(the continuation kick is a rotation wake, not a fake edit)" );
	}
}

//----------------------------------------------------------------------
// (d) Single layout: the rotation never leaves pane 0, and the
//     scheduler adds ZERO passes over today's behaviour.
//----------------------------------------------------------------------
static void RunSingleLayoutBaselineTest()
{
	std::printf( "=== scheduler (d): Single layout == today's single-viewport behaviour ===\n" );
	Fixture f( "pane_sched_d.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "the edit's single pass lands" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	bool allPane0 = !seq.empty();
	for( std::size_t i = 0; i < seq.size(); ++i ) {
		if( seq[i] != 0 ) allPane0 = false;
	}
	Check( allPane0, "every pass is pane 0" );
	Check( f.ctrl->SettlesAt( seq.size(), kSettleMs ),
	       "MONEY ASSERTION (d): ONE pass per edit -- the pane machinery adds no extra passes in "
	       "Single layout (byte-identical single-viewport behaviour)" );
}

//----------------------------------------------------------------------
// (e) Hidden panes never render: dirty state on a pane hidden by a
//     layout shrink must not produce passes.
//----------------------------------------------------------------------
static void RunHiddenPaneNeverRendersTest()
{
	std::printf( "=== scheduler (e): a hidden pane never renders ===\n" );
	Fixture f( "pane_sched_e.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	Check( f.ctrl->SetPaneRenderMode( 3, "depth" ), "configure pane 3 (so it has non-default state)" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "shrink to TwoH (panes 2,3 hidden)" );

	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "the two VISIBLE panes render" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	bool sawHidden = false;
	for( std::size_t i = 0; i < seq.size(); ++i ) {
		if( seq[i] >= 2 ) sawHidden = true;
	}
	Check( !sawHidden,
	       "MONEY ASSERTION (e): panes 2/3 (hidden by the shrink) never render, dirty or not" );
	Check( f.ctrl->SettlesAt( seq.size(), kSettleMs ), "rotation quiesces at the visible-pane count" );
}

//----------------------------------------------------------------------
// (f) review-r2 P1 regression: a layout GROW while the loop is idle
//     must wake it -- newly-revealed panes render without waiting for
//     an unrelated edit.  (Before the fix, SetViewportLayout mutated
//     state and returned; the loop slept on a predicate that never saw
//     the new panes.)
//----------------------------------------------------------------------
static void RunLayoutGrowWakesLoopTest()
{
	std::printf( "=== scheduler (f): layout grow while idle wakes the loop (review-r2 P1) ===\n" );
	Fixture f( "pane_sched_f.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	// Start in Single, render, and QUIESCE -- the loop is now asleep.
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "single-layout pass lands" );
	Check( f.ctrl->SettlesAt( f.ctrl->Sequence().size(), kSettleMs ), "loop is idle before the grow" );
	f.ctrl->ClearSequence();

	// Grow to Quad with NO accompanying edit.
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "grow Single -> Quad" );
	Check( f.ctrl->WaitForPassCount( 3, kWaitMs ),
	       "MONEY ASSERTION (f): the three newly-revealed panes render WITHOUT any edit -- the grow "
	       "itself wakes the loop" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	bool sawAll = false;
	{
		bool p1 = false, p2 = false, p3 = false;
		for( std::size_t i = 0; i < seq.size(); ++i ) {
			if( seq[i] == 1 ) p1 = true;
			if( seq[i] == 2 ) p2 = true;
			if( seq[i] == 3 ) p3 = true;
		}
		sawAll = p1 && p2 && p3;
	}
	Check( sawAll, "panes 1, 2 and 3 each got their reveal pass" );
	Check( f.ctrl->SettlesAt( seq.size(), kSettleMs ), "and the rotation quiesces after" );
}

//----------------------------------------------------------------------
// (g) review-r2 P1 regression: updating a named view PROPAGATES to the
//     panes bound to it (§7.2).  Before the fix, the binding resolved
//     once and cached forever; UpdateNamedView touched no pane state.
//----------------------------------------------------------------------
static void RunNamedViewUpdatePropagatesTest()
{
	std::printf( "=== scheduler (g): UpdateNamedView re-renders bound panes (review-r2 P1) ===\n" );
	Fixture f( "pane_sched_g.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "layout TwoH" );
	Check( f.ctrl->CaptureNamedView( "keyview" ), "capture a named view" );
	Check( f.ctrl->SetPaneVantageNamedView( 1, "keyview" ), "bind pane 1 to it" );

	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "initial rotation completes" );
	Check( f.ctrl->SettlesAt( f.ctrl->Sequence().size(), kSettleMs ), "loop idle before the update" );
	f.ctrl->ClearSequence();

	// Update the view (index 0 -- the only one).  The BOUND pane must
	// re-render; nothing else should.
	Check( f.ctrl->UpdateNamedView( 0 ), "UpdateNamedView(0)" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
	       "MONEY ASSERTION (g): the bound pane re-renders after the view update -- the binding "
	       "FOLLOWS the view instead of caching its first resolution forever" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	Check( !seq.empty() && seq[0] == 1, "and the re-rendered pane is the bound one (1)" );
	Check( f.ctrl->SettlesAt( seq.size(), kSettleMs ),
	       "the UNBOUND pane (0) does not re-render -- propagation is targeted, not a global kick" );
}

int main()
{
	RunRotationOrderTest();
	RunPaneLocalInvalidationTest();
	RunGesturePinningTest();
	RunSingleLayoutBaselineTest();
	RunHiddenPaneNeverRendersTest();
	RunLayoutGrowWakesLoopTest();
	RunNamedViewUpdatePropagatesTest();

	std::printf( "\nViewportPaneSchedulerTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
