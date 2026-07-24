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
#include "../src/Library/RISE_API.h"   // user-review P1-1: C-ABI contract (unindexed nav aliases pane 0)
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"

#include <atomic>
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
	RecordingController( IJobPriv& job, IRasterizer* interactiveRasterizer = nullptr,
		bool runRealPass = false )
	: SceneEditController( job, interactiveRasterizer )
	, mRunRealPass( runRealPass )
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

	void ArmReleaseInterleaving()
	{
		std::lock_guard<std::mutex> lk( mGateMutex );
		mReleaseInterleavingArmed = true;
		mMintObserved = false;
		mAllowMintedPass = false;
	}

	void ReleaseMintedPass()
	{
		std::lock_guard<std::mutex> lk( mGateMutex );
		mAllowMintedPass = true;
		mReleaseInterleavingArmed = false;
		mGateCV.notify_all();
	}

	void ArmSaveInterleaving()
	{
		std::lock_guard<std::mutex> lk( mSaveGateMutex );
		mSaveGateArmed = true;
		mSaveGateReached = false;
		mReleaseSaveGate = false;
	}

	bool WaitForSaveInterleaving( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mSaveGateMutex );
		return mSaveGateCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mSaveGateReached; } );
	}

	void ReleaseSaveInterleaving()
	{
		std::lock_guard<std::mutex> lk( mSaveGateMutex );
		mReleaseSaveGate = true;
		mSaveGateArmed = false;
		mSaveGateCV.notify_all();
	}

protected:
	void DoOneRenderPass() override
	{
		// ForTest_CurrentPane locks mMutex -- safe HERE because the mint
		// lock is released before DoOneRenderPass runs (the lock_guard
		// scope closes at the mint block's end), and mCurrentPane is
		// stable for the whole pass (only mutated inside the mint lock,
		// before mRendering flips true).
		const unsigned int pane = ForTest_CurrentPane();
		if( mRunRealPass )
		{
			SceneEditController::DoOneRenderPass();
		}
		{
			std::lock_guard<std::mutex> lk( mSeqMutex );
			mSeq.push_back( pane );
		}
		mSeqCV.notify_all();
		if( !mRunRealPass )
		{
			// A small real duration so gesture scenarios can overlap a pass.
			std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
		}
	}

	void ForTest_OnInteractivePassMinted() override
	{
		std::unique_lock<std::mutex> lk( mGateMutex );
		if( mReleaseInterleavingArmed )
		{
			mMintObserved = true;
			mGateCV.notify_all();
			mGateCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
				[&]{ return mAllowMintedPass || !mReleaseInterleavingArmed; } );
		}
	}

	void ForTest_OnPointerUpAfterFinalRenderArmed() override
	{
		std::unique_lock<std::mutex> lk( mGateMutex );
		if( !mReleaseInterleavingArmed ) return;
		mGateCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&]{ return mMintObserved; } );
	}

	void ForTest_OnSaveEngineAboutToRun() override
	{
		std::unique_lock<std::mutex> lk( mSaveGateMutex );
		if( !mSaveGateArmed ) return;
		mSaveGateReached = true;
		mSaveGateCV.notify_all();
		mSaveGateCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&]{ return mReleaseSaveGate || !mSaveGateArmed; } );
	}

private:
	mutable std::mutex          mSeqMutex;
	std::condition_variable     mSeqCV;
	std::vector<unsigned int>   mSeq;
	bool                        mRunRealPass = false;
	std::mutex                  mGateMutex;
	std::condition_variable     mGateCV;
	bool                        mReleaseInterleavingArmed = false;
	bool                        mMintObserved = false;
	bool                        mAllowMintedPass = false;
	std::mutex                  mSaveGateMutex;
	std::condition_variable     mSaveGateCV;
	bool                        mSaveGateArmed = false;
	bool                        mSaveGateReached = false;
	bool                        mReleaseSaveGate = false;
};

struct Fixture
{
	Job*                  job = nullptr;
	RecordingController*  ctrl = nullptr;
	IRasterizer*          rasterizer = nullptr;
	IRayCaster*           previewCaster = nullptr;
	IRayCaster*           polishCaster = nullptr;

	explicit Fixture( const char* tmpName, bool withInteractivePipeline = false )
	{
		const std::string path = WriteTemp( tmpName, kScene );
		job = new Job();
		if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) { job->release(); job = nullptr; return; }
		if( withInteractivePipeline &&
		    !Implementation::CreateInteractiveMaterialPreviewPipeline(
			    &rasterizer, &previewCaster, &polishCaster ) )
		{
			job->release();
			job = nullptr;
			return;
		}
		ctrl = new RecordingController( *job, rasterizer, withInteractivePipeline );
	}
	~Fixture()
	{
		if( ctrl ) { ctrl->Stop(); delete ctrl; }
		if( rasterizer ) rasterizer->release();
		if( previewCaster ) previewCaster->release();
		if( polishCaster ) polishCaster->release();
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

	// SetViewportLayout queued a grow wake before Start.  Drain that
	// construction/default-dirty rotation first; otherwise racing an explicit
	// Kick against the queued grow can legitimately render the primary twice
	// (grow quantum, then edit quantum) and make the order oracle flaky.
	f.ctrl->Start( /*suppressInitialRender*/ true );
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "layout-grow rotation completes" );
	Check( f.ctrl->SettlesAt( 4, kSettleMs ), "layout-grow rotation settles" );
	f.ctrl->ClearSequence();

	// One scene edit -> all four visible panes dirty -> exactly 4 passes:
	// primary (2) first, then 0, 1, 3.
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "4 passes complete after one edit" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	Check( seq.size() >= 4 && seq[0] == 2,
	       "MONEY ASSERTION (a): the PRIMARY pane (2) renders FIRST after an edit" );
	Check( seq.size() >= 4 && seq[1] == 0 && seq[2] == 1 && seq[3] == 3,
	       "MONEY ASSERTION (a): secondaries follow in index order (0, 1, 3)" );
	Check( f.ctrl->SettlesAt( seq.size(), kSettleMs ),
	       "rotation QUIESCES once every pane has rendered (no runaway passes)" );

	// ROUND 2 -- repeat the same real edit-invalidation contract.
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

	// review-r3 P1: arm a CAMERA-MOTION tool first.  The polish chain
	// (FinalRegularRunning -> PolishQueued -> polish pass) only arms on
	// gesture-end when the tool was a motion tool -- with the default
	// Tool::Select the entire post-roll branch under test in the settle
	// assertion below NEVER RUNS, making that assertion vacuous (proven
	// empirically by the round-3 auditor: reverting the guarded fix line
	// left this test green).
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );

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

	// Release.  review-r3 P1 (second attempt -- the first settle design
	// was PROVEN VACUOUS by mutation: the spurious passes happen DURING
	// the drain, so post-drain quiescence never counts them): assert the
	// EXACT pass delta of the release sequence.
	//
	// With the motion tool armed, OnPointerUp = KickRender (a real edit:
	// both panes dirty) + FinalRegularRunning.  Correct routing yields
	// EXACTLY 3 passes: pane 1 (primary-dirty first), pane 0 (the
	// final-regular pass), pane 0 again (the polish pass via the
	// ROTATION-flag continuation).  The broken routing (continuation
	// kick faking a scene edit) re-dirties BOTH panes at the
	// continuation, yielding a 4th pass on pane 1 -- distinguishable by
	// count, not by quiescence.
	const std::size_t nAtRelease = f.ctrl->Sequence().size();
	// Deterministically force the render thread to select/mint its first
	// post-release pane before OnPointerUp returns.  The polish marker must
	// already belong to pane 0 when that switch happens; a Kick-then-store
	// implementation races and stamps the sibling's live register instead.
	f.ctrl->ArmReleaseInterleaving();
	f.ctrl->OnPointerUp( Point2( 5, 5 ) );
	Check( f.ctrl->ForTest_CurrentPane() == 1,
	       "forced interleaving minted the dirty primary sibling first" );
	Check( f.ctrl->ForTest_PaneHasFinalRegularPolish( 0 ),
	       "MONEY ASSERTION (c/T0): gestured pane 0 owns FinalRegularRunning after the sibling switch" );
	Check( !f.ctrl->ForTest_PaneHasFinalRegularPolish( 1 ),
	       "MONEY ASSERTION (c/T0): sibling pane 1 never inherits the gestured pane's polish marker" );
	f.ctrl->ReleaseMintedPass();
	Check( f.ctrl->WaitForPassCount( nAtRelease + 3, kWaitMs ),
	       "the 3-pass release sequence lands (primary, final-regular, polish)" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );   // drain any EXTRA passes
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		const std::size_t delta = seq.size() - nAtRelease;
		bool sawPane1 = false;
		for( std::size_t i = nAtRelease; i < seq.size(); ++i ) {
			if( seq[i] == 1 ) sawPane1 = true;
		}
		Check( sawPane1,
		       "MONEY ASSERTION (c): the dirty primary (1) renders once the gesture ends" );
		Check( delta == 3,
		       "MONEY ASSERTION (c/r3): the release sequence is EXACTLY 3 passes -- a 4th pass means "
		       "the polish continuation faked a scene edit and re-rendered settled panes "
		       "(review-r2-B P2 regression guard, count-exact this time)" );
	}
}

//----------------------------------------------------------------------
// (c2) T0 regression: a BeautyVariant object-gizmo edit dirties every
//      visible pane, but gesture exclusivity must NOT turn that deferred
//      sibling work into a self-sustaining stream of passes on the pinned
//      pane.  The live macOS repro was Quad + Indirect + Move: the variant
//      divisor stays pinned, so the runaway rendered full 12-SPP/OIDN
//      quanta continuously and made the GUI appear hung.
//----------------------------------------------------------------------
static void RunVariantGizmoGestureDoesNotSpinTest()
{
	std::printf( "=== scheduler (c2/T0): variant gizmo gesture renders once per edit, never spins on frozen siblings ===\n" );
	Fixture f( "pane_sched_c2.RISEscene", true );
	Check( f.ctrl != nullptr, "live-pipeline fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "layout TwoH" );
	Check( f.ctrl->SetViewportRenderMode( "indirect" ), "pane 0 switches to indirect BeautyVariant" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "initial TwoH rotation completes" );
	Check( f.ctrl->SettlesAt( f.ctrl->Sequence().size(), kSettleMs ), "initial rotation settles" );
	f.ctrl->ClearSequence();

	f.ctrl->ForTest_SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	f.ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	f.ctrl->RefreshGizmoHandles();
	int center = -1;
	for( unsigned int i = 0; i < f.ctrl->GizmoHandleCount(); ++i ) {
		if( f.ctrl->GizmoHandleKind( i )
		    == static_cast<int>( SceneEditController::GizmoHandle::Kind::ScreenCenter ) ) {
			center = static_cast<int>( i );
			break;
		}
	}
	Check( center >= 0, "a ScreenCenter transform handle is available" );
	if( center < 0 ) return;
	const Point2 down( f.ctrl->GizmoHandleScreenX( static_cast<unsigned int>( center ) ),
	                   f.ctrl->GizmoHandleScreenY( static_cast<unsigned int>( center ) ) );
	Check( f.ctrl->OnPanePointerDown( 0, down ), "variant-pane gizmo Down accepted" );
	Check( f.ctrl->IsGizmoDragActive() && f.ctrl->Editor().IsCompositeOpen(),
	       "the ScreenCenter hit captured a real gizmo drag and opened its composite" );
	Check( f.ctrl->OnPanePointerMove( 0, Point2( down.x + 4.0, down.y + 3.0 ) ),
	       "variant-pane gizmo Move accepted" );
	Check( f.ctrl->Editor().HasPendingCstObjectTransforms(),
	       "the gizmo Move accumulated a real CST-pending object transform" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "the gizmo edit produces one pinned-pane pass" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ),
	       "MONEY (c2/T0): while the pointer stays down, deferred dirty siblings DO NOT "
	       "self-rearm the pinned pane -- one edit produces one pass, not an infinite variant loop" );
	Check( f.ctrl->OnPanePointerUp( 0, Point2( down.x + 4.0, down.y + 3.0 ) ),
	       "variant-pane gizmo Up accepted" );
	Check( f.ctrl->WaitForPassCount( 3, kWaitMs ),
	       "variant gesture release repaints both visible panes" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		bool sawSibling = false;
		for( std::size_t i = 1; i < seq.size(); ++i ) {
			if( seq[i] == 1 ) sawSibling = true;
		}
		Check( sawSibling,
		       "MONEY (c2/T0): pointer-up resumes the deferred sibling rotation" );
		Check( f.ctrl->SettlesAt( 3, kSettleMs ),
		       "variant post-gesture rotation quiesces" );
	}
}

//----------------------------------------------------------------------
// (c3) Property-panel scrubs use the same scheduler pin as pointer
// gestures.  Exercise both the explicit End path and the watchdog path
// that recovers a dropped End while a fixed-divisor variant is active.
//----------------------------------------------------------------------
static void RunVariantPropertyScrubRecoveryTest()
{
	std::printf( "=== scheduler (c3/T0): variant property scrub neither spins nor strands siblings ===\n" );
	Fixture f( "pane_sched_c3.RISEscene", true );
	Check( f.ctrl != nullptr, "live-pipeline fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "layout TwoH" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "initial TwoH rotation completes" );
	Check( f.ctrl->SettlesAt( 2, kSettleMs ), "initial rotation settles" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPaneRenderMode( 1, "indirect" ),
	       "current pane 1 switches to indirect" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "variant pane realization completes" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ), "variant realization settles" );
	f.ctrl->ClearSequence();

	f.ctrl->BeginPropertyScrub();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "one property-scrub edit pass lands" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ),
	       "MONEY (c3/T0): an active property scrub also suppresses sibling self-rearm" );
	f.ctrl->EndPropertyScrub();
	Check( f.ctrl->WaitForPassCount( 3, kWaitMs ),
	       "explicit EndPropertyScrub repaints both panes" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		bool sawSibling = false;
		for( std::size_t i = 1; i < seq.size(); ++i ) {
			if( seq[i] == 0 ) sawSibling = true;
		}
		Check( sawSibling, "explicit property-scrub End resumes the deferred sibling" );
		Check( f.ctrl->SettlesAt( 3, kSettleMs ),
		       "explicit property-scrub recovery quiesces" );
	}

	f.ctrl->ClearSequence();
	// Omit EndPropertyScrub deliberately.  The 1500-ms watchdog must clear
	// the pin, restore final quality, and mint an all-pane recovery rotation.
	f.ctrl->BeginPropertyScrub();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "lost-End scrub's edit pass lands" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ),
	       "lost-End scrub is quiescent while still inside the watchdog window" );
	Check( f.ctrl->WaitForPassCount( 3, kWaitMs ),
	       "watchdog mints the final-quality two-pane recovery" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		bool sawSibling = false;
		for( std::size_t i = 1; i < seq.size(); ++i ) {
			if( seq[i] == 0 ) sawSibling = true;
		}
		Check( sawSibling,
		       "MONEY (c3/T0): watchdog recovery renders the sibling a dropped End had deferred" );
		Check( f.ctrl->SettlesAt( 3, kSettleMs ),
		       "watchdog recovery quiesces instead of restarting a loop" );
	}
}

//----------------------------------------------------------------------
// (c4) Stopping the interactive loop is the cancellation boundary for a
// pointer/property gesture whose platform release callback was dropped.
//----------------------------------------------------------------------
static void RunStopRecoversLostPointerUpTest()
{
	std::printf( "=== scheduler (c4/T0): StopInteractive recovers a lost pointer-up ===\n" );
	Fixture f( "pane_sched_c4.RISEscene", true );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "layout TwoH" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "initial rotation completes" );
	Check( f.ctrl->SettlesAt( 2, kSettleMs ), "initial rotation settles" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPaneRenderMode( 1, "indirect" ),
	       "current pane 1 switches to fixed-divisor indirect mode" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "variant pane realization completes" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ), "variant realization settles" );
	f.ctrl->ClearSequence();

	f.ctrl->ForTest_SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	f.ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	f.ctrl->RefreshGizmoHandles();
	int center = -1;
	for( unsigned int i = 0; i < f.ctrl->GizmoHandleCount(); ++i ) {
		if( f.ctrl->GizmoHandleKind( i )
		    == static_cast<int>( SceneEditController::GizmoHandle::Kind::ScreenCenter ) ) {
			center = static_cast<int>( i );
			break;
		}
	}
	Check( center >= 0, "a ScreenCenter transform handle is available" );
	if( center < 0 ) return;
	const Point2 down( f.ctrl->GizmoHandleScreenX( static_cast<unsigned int>( center ) ),
	                   f.ctrl->GizmoHandleScreenY( static_cast<unsigned int>( center ) ) );
	Check( f.ctrl->OnPanePointerDown( 1, down ), "pane-1 transform Down accepted" );
	Check( f.ctrl->OnPanePointerMove( 1, Point2( down.x + 4.0, down.y + 3.0 ) ),
	       "pane-1 transform Move accepted" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "one pinned edit pass lands" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ), "lost-Up gesture no longer spins" );

	// Deliberately omit Up.  A lifecycle stop must commit/close/clear before
	// any coordinated render or later interactive restart is admitted.
	f.ctrl->StopInteractive();
	Check( !f.ctrl->Editor().IsCompositeOpen(),
	       "MONEY (c4/T0): stop closes the orphaned gesture composite" );
	Check( !f.ctrl->Editor().HasPendingCstObjectTransforms(),
	       "stop commits the orphaned gizmo's pending CST transform" );
	Check( !f.ctrl->IsGizmoDragActive(), "stop clears the orphaned gizmo drag" );

	std::mutex heldMutex;
	std::condition_variable heldCV;
	bool heldStarted = false;
	bool releaseHeld = false;
	std::atomic<bool> productionAccepted( false );
	std::thread productionThread( [&]{
		SceneEditController::RenderJobId heldJobId =
			SceneEditController::kInvalidRenderJobId;
		const bool ok = f.ctrl->SubmitProductionRenderSync(
			[&]{
				std::unique_lock<std::mutex> lk( heldMutex );
				heldStarted = true;
				heldCV.notify_all();
				heldCV.wait( lk, [&]{ return releaseHeld; } );
			},
			String( "t0-stop-recovery-held" ), &heldJobId, kWaitMs );
		productionAccepted.store( ok, std::memory_order_release );
	} );
	{
		std::unique_lock<std::mutex> lk( heldMutex );
		Check( heldCV.wait_for( lk, std::chrono::milliseconds( kWaitMs ),
			[&]{ return heldStarted; } ),
		       "coordinated render is admitted after stop clears the stale gesture" );
	}
	std::atomic<bool> repeatedStopReturned( false );
	std::thread repeatedStop( [&]{
		f.ctrl->StopInteractive();
		repeatedStopReturned.store( true, std::memory_order_release );
	} );
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	Check( repeatedStopReturned.load( std::memory_order_acquire ),
	       "MONEY (c4/T0): repeated Stop returns promptly while a coordinated render owns the scene" );
	{
		std::lock_guard<std::mutex> lk( heldMutex );
		releaseHeld = true;
	}
	heldCV.notify_all();
	repeatedStop.join();
	productionThread.join();
	Check( productionAccepted.load( std::memory_order_acquire ),
	       "held coordinated render completed normally after repeated Stop" );

	f.ctrl->ClearSequence();
	f.ctrl->Start( true );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ),
	       "restart consumes the deferred edit and repaints both panes" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		bool sawSibling = false;
		for( std::size_t i = 0; i < seq.size(); ++i ) {
			if( seq[i] == 0 ) sawSibling = true;
		}
		Check( sawSibling,
		       "MONEY (c4/T0): restart settles the sibling deferred by the lost pointer-up" );
		Check( f.ctrl->SettlesAt( 2, kSettleMs ), "restart rotation quiesces" );
	}

	// The same cleanup must be idempotent while already paused: controller
	// edits intentionally remain available with the render loop stopped.
	f.ctrl->PauseRefinement();
	f.ctrl->ClearSequence();
	f.ctrl->RefreshGizmoHandles();
	center = -1;
	for( unsigned int i = 0; i < f.ctrl->GizmoHandleCount(); ++i ) {
		if( f.ctrl->GizmoHandleKind( i )
		    == static_cast<int>( SceneEditController::GizmoHandle::Kind::ScreenCenter ) ) {
			center = static_cast<int>( i );
			break;
		}
	}
	Check( center >= 0, "paused controller still exposes the transform gizmo" );
	if( center < 0 ) return;
	const Point2 pausedDown(
		f.ctrl->GizmoHandleScreenX( static_cast<unsigned int>( center ) ),
		f.ctrl->GizmoHandleScreenY( static_cast<unsigned int>( center ) ) );
	Check( f.ctrl->OnPanePointerDown( 1, pausedDown ),
	       "pointer Down is accepted while refinement is paused" );
	Check( f.ctrl->OnPanePointerMove(
		       1, Point2( pausedDown.x + 3.0, pausedDown.y + 2.0 ) ),
	       "pointer Move is accepted while refinement is paused" );
	Check( f.ctrl->Editor().IsCompositeOpen(),
	       "precondition: paused lost-Up gesture opened a composite" );
	const std::string saveRacePath = "/tmp/pane_sched_c4_save_race.RISEscene";
	std::remove( saveRacePath.c_str() );
	SaveResult saveRaceResult;
	f.ctrl->ArmSaveInterleaving();
	std::thread saveThread( [&]{
		saveRaceResult = f.ctrl->RequestSave( saveRacePath );
	} );
	Check( f.ctrl->WaitForSaveInterleaving( kWaitMs ),
	       "save reaches post-snapshot IO window while paused gesture is pending" );
	f.ctrl->StopInteractive();   // already stopped + saving: must still sanitize
	Check( !f.ctrl->Editor().IsCompositeOpen(),
	       "MONEY (c4/T0): repeated Stop cleans a paused gesture during save IO" );
	Check( !f.ctrl->Editor().HasPendingCstObjectTransforms(),
	       "repeated Stop commits the paused gesture while save uses its old snapshot" );
	f.ctrl->ReleaseSaveInterleaving();
	saveThread.join();
	Check( Succeeded( saveRaceResult.status ), "held save completes" );
	Check( f.ctrl->Editor().Dirty().HasAnyDirty(),
	       "MONEY (c4/T0): head change during save remains dirty instead of baselining stale bytes" );
	f.ctrl->ResumeRefinement();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ),
	       "Resume after paused cleanup repaints both panes" );
	Check( f.ctrl->SettlesAt( 2, kSettleMs ),
	       "MONEY (c4/T0): paused lost-Up recovery resumes without a stale scheduler pin" );
}

//----------------------------------------------------------------------
// (c4b) A plain pause with no live gesture preserves the pane's owed
// final->polish state; gesture sanitation must not erase normal refinement.
//----------------------------------------------------------------------
static void RunPausePreservesPolishStateTest()
{
	std::printf( "=== scheduler (c4b/T0): no-interaction pause preserves owed polish ===\n" );
	Fixture f( "pane_sched_c4b.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "initial pass completes" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ), "initial pass settles" );
	f.ctrl->ClearSequence();
	f.ctrl->ForTest_ArmFinalRegularPolish();
	f.ctrl->PauseRefinement();
	Check( f.ctrl->ForTest_PaneHasFinalRegularPolish( 0 ),
	       "MONEY (c4b/T0): plain Pause preserves FinalRegularRunning" );
	f.ctrl->ResumeRefinement();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ),
	       "Resume completes the preserved final pass and its polish continuation" );
	Check( f.ctrl->SettlesAt( 2, kSettleMs ),
	       "preserved polish sequence quiesces exactly" );
}

//----------------------------------------------------------------------
// (c5) Gesture exclusivity includes the live register set itself.  A
// pane-0 setter must not context-switch away from a secondary gesture.
//----------------------------------------------------------------------
static void RunMidGesturePaneSwitchRefusedTest()
{
	std::printf( "=== scheduler (c5/T0): pane-0 setter cannot switch registers mid-secondary gesture ===\n" );
	Fixture f( "pane_sched_c5.RISEscene", true );
	Check( f.ctrl != nullptr, "live-pipeline fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "layout TwoH" );
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "initial rotation completes" );
	Check( f.ctrl->SettlesAt( 2, kSettleMs ), "initial rotation settles" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->PaneEnterFreeFly( 0 ), "preconfigure pane 0 as FreeFly" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "pane-0 FreeFly change repaints both panes" );
	Check( f.ctrl->SettlesAt( 2, kSettleMs ), "pane-0 FreeFly rotation settles" );
	CameraSnapshot pane0Pose;
	Check( f.ctrl->GetViewportPose( pane0Pose, 0 ), "capture pane-0 FreeFly pose" );
	f.ctrl->ClearSequence();

	Check( f.ctrl->OnPanePointerDown( 1, Point2( 10, 10 ) ),
	       "secondary orbit Down accepted" );
	Check( f.ctrl->ForTest_CurrentPane() == 1,
	       "secondary gesture owns the live pane registers" );
	Check( !f.ctrl->SetViewportPose( pane0Pose, 0 ),
	       "pane-0 pose setter is refused during the pane-1 gesture" );
	Check( !f.ctrl->ExitFreeFly( 0 ),
	       "pane-0 ExitFreeFly is refused during the pane-1 gesture" );
	Check( f.ctrl->IsFreeFlyActive( 0 ),
	       "refused pose/exit setters preserve pane 0's FreeFly state" );
	Check( !f.ctrl->SetViewportRenderMode( "normals" ),
	       "MONEY (c5/T0): pane-0 mode setter is refused during the pane-1 gesture" );
	Check( std::string( f.ctrl->GetViewportRenderMode() ) == "preview",
	       "refused setter leaves pane 0's configured mode unchanged" );
	Check( f.ctrl->ForTest_CurrentPane() == 1,
	       "refused setter cannot switch the live registers away from pane 1" );
	Check( f.ctrl->OnPanePointerMove( 1, Point2( 36, 24 ) ),
	       "the owning pane's next orbit Move remains accepted" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "pane-1 move renders once" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		Check( seq.size() == 1 && seq[0] == 1,
		       "MONEY (c5/T0): post-refusal gesture work stays on pane 1" );
	}
	Check( f.ctrl->OnPanePointerUp( 1, Point2( 36, 24 ) ), "secondary orbit Up accepted" );
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "release final/polish sequence completes" );
	Check( f.ctrl->SettlesAt( 4, kSettleMs ), "release sequence quiesces" );
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

//----------------------------------------------------------------------
// (h) P3a slice 3 pointer half: a pane-indexed camera gesture on a
//     SECONDARY pane promotes it to primary, converts it to per-pane
//     FreeFly, pins the scheduler to it, flies ITS camera -- and never
//     mutates the scene camera (§7.2, 2026-07-21 amendment).
//----------------------------------------------------------------------
static void RunPaneIndexedGestureTest()
{
	std::printf( "=== scheduler (h): pane-indexed gesture -- promote + convert + fly, scene camera untouched ===\n" );
	Fixture f( "pane_sched_h.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "layout TwoH" );
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "initial rotation completes" );
	f.ctrl->ClearSequence();

	// Scene-camera pose BEFORE the secondary-pane fly (free-fly is not
	// active for pane 0, so CaptureCurrentView reads the scene camera).
	CameraSnapshot before;
	Check( f.ctrl->CaptureNamedView( "h_before" ), "captured the scene camera before the fly" );
	Check( f.ctrl->FindNamedViewPose( String( "h_before" ), before ), "read back the before-pose" );

	// The pane-indexed gesture on the secondary pane.
	Check( f.ctrl->OnPanePointerDown( 1, Point2( 10, 10 ) ), "pane-indexed Down on pane 1 accepted" );
	// A physical gesture is bound to the accepted pane.  In particular, a
	// hidden/mismatched pane's trailing event must neither be reported as
	// accepted nor complete the live pane-1 gesture.
	Check( !f.ctrl->OnPanePointerMove( 3, Point2( 20, 20 ) ),
	       "MONEY: Move from hidden/mismatched pane 3 is refused while pane 1 owns the gesture" );
	Check( !f.ctrl->OnPanePointerUp( 3, Point2( 20, 20 ) ),
	       "MONEY: Up from hidden/mismatched pane 3 is refused and cannot end pane 1's gesture" );
	// review-s3 P1: the FIRST version of this assertion demanded promotion
	// here and was tagged "§7.8 decision 1" -- but the ratified text says
	// "navigation drags NEVER steal primary", and OrbitCamera IS a
	// navigation tool.  The test had enshrined the violation.  The pane
	// becomes the RENDER TARGET (gesture exclusivity, asserted below);
	// primary must stay where it was.
	Check( f.ctrl->GetPrimaryPane() == 0,
	       "MONEY ASSERTION (h/§7.8): a NAVIGATION drag (OrbitCamera) does NOT steal primary -- "
	       "pane 0 remains primary" );
	{
		SceneEditController::PaneVantageKind kind;
		String nv;
		Check( f.ctrl->GetPaneVantage( 1, kind, nv )
		    && kind == SceneEditController::PaneVantageKind::FreeFly,
		       "MONEY ASSERTION (h): the camera gesture CONVERTED pane 1 to per-pane FreeFly (§7.2)" );
	}
	Check( f.ctrl->OnPanePointerMove( 1, Point2( 60, 34 ) ), "pane-indexed Move accepted (the fly op)" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "the fly re-renders" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		bool allPane1 = !seq.empty();
		for( std::size_t i = 0; i < seq.size(); ++i ) {
			if( seq[i] != 1 ) allPane1 = false;
		}
		Check( allPane1,
		       "MONEY ASSERTION (h): the gesture pins the scheduler to the GESTURED pane (1), "
		       "not pane 0" );
	}
	Check( f.ctrl->OnPanePointerUp( 1, Point2( 60, 34 ) ), "pane-indexed Up accepted" );
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
	       "orbit release drains its two-pane final/polish sequence" );
	Check( f.ctrl->SettlesAt( 4, kSettleMs ), "orbit release sequence quiesces" );

	// The scene camera must be untouched: pane 1's fly went to ITS
	// override camera.  (IsFreeFlyActive reads pane 0's state via the
	// alias getter, so CaptureCurrentView still reads the scene camera.)
	Check( !f.ctrl->IsFreeFlyActive(),
	       "pane 0 (the alias surface) is NOT in free-fly -- the conversion was pane-1-local" );
	CameraSnapshot after;
	Check( f.ctrl->CaptureNamedView( "h_after" ), "captured the scene camera after the fly" );
	Check( f.ctrl->FindNamedViewPose( String( "h_after" ), after ), "read back the after-pose" );
	// review-of-own-test: LOCATION is the WRONG witness for an orbit --
	// orbit mutates target_orientation, not vPosition (that is exactly
	// what SceneEditorCameraAnglesTest guards).  Compare every field the
	// orbit/pan/zoom/roll family can move.  Proven by mutation: with a
	// location-only witness, removing the fly routing (camera edits fall
	// through to the scene camera) still passed 71/0.
	// The OTHER half of §7.8 decision 1: a NON-navigation click DOES
	// promote.  Switch to Select and click pane 1.
	f.ctrl->SetTool( SceneEditController::Tool::Select );
	Check( f.ctrl->OnPanePointerDown( 1, Point2( 12, 12 ) ), "Select-tool Down on pane 1 accepted" );
	Check( f.ctrl->GetPrimaryPane() == 1,
	       "MONEY ASSERTION (h/§7.8): a NON-navigation click PROMOTES pane 1 to primary" );
	f.ctrl->OnPanePointerUp( 1, Point2( 12, 12 ) );
	Check( f.ctrl->WaitForPassCount( 6, kWaitMs ),
	       "pointer release sequences drain before the free-fly twin checks" );
	Check( f.ctrl->SettlesAt( 6, kSettleMs ),
	       "pane-indexed gesture release sequences quiesce" );

	// review-s3 P2 coverage: hidden-pane and free-fly-twin contracts.
	Check( !f.ctrl->OnPanePointerDown( 3, Point2( 5, 5 ) ),
	       "pane-indexed Down on a HIDDEN pane (3 in TwoH) is refused" );
	Check( f.ctrl->PaneEnterFreeFly( 1 ), "PaneEnterFreeFly(1) accepted" );
	{
		SceneEditController::PaneVantageKind kind;
		String nv;
		Check( f.ctrl->GetPaneVantage( 1, kind, nv )
		    && kind == SceneEditController::PaneVantageKind::FreeFly,
		       "PaneEnterFreeFly set the pane's vantage to FreeFly" );
	}
	Check( f.ctrl->PaneExitFreeFly( 1 ), "PaneExitFreeFly(1) accepted" );
	{
		SceneEditController::PaneVantageKind kind;
		String nv;
		Check( f.ctrl->GetPaneVantage( 1, kind, nv )
		    && kind == SceneEditController::PaneVantageKind::SceneCamera,
		       "PaneExitFreeFly returned the pane to SceneCamera tracking" );
	}
	{
		int phase = -1; unsigned int div = 0;
		Check( f.ctrl->GetPaneRefinementStatus( 1, phase, div ),
		       "GetPaneRefinementStatus succeeds for a valid pane" );
		Check( !f.ctrl->GetPaneRefinementStatus( 7, phase, div ),
		       "GetPaneRefinementStatus refuses an out-of-range pane" );
	}

	Check( before.location[0] == after.location[0]
	    && before.location[1] == after.location[1]
	    && before.location[2] == after.location[2]
	    && before.lookat[0] == after.lookat[0]
	    && before.lookat[1] == after.lookat[1]
	    && before.lookat[2] == after.lookat[2]
	    && before.orientation[0] == after.orientation[0]
	    && before.orientation[1] == after.orientation[1]
	    && before.orientation[2] == after.orientation[2]
	    && before.target_orientation[0] == after.target_orientation[0]
	    && before.target_orientation[1] == after.target_orientation[1],
	       "MONEY ASSERTION (h): the SCENE camera is byte-identical across every pose field the "
	       "camera-op family can move -- secondary-pane navigation never mutates it (§7.2)" );
}

//----------------------------------------------------------------------
// (j) user-review P2#1: a layout SHRINK that hides the currently-
//     scheduled pane must relocate the scheduler to the (visible)
//     primary and stop rendering the hidden pane -- otherwise a hidden
//     pane keeps rendering and its pixels get reported as the current
//     frame.
//----------------------------------------------------------------------
static void RunShrinkCancelsHiddenPaneTest()
{
	std::printf( "=== scheduler (j): shrink relocates the scheduler off a now-hidden pane (user-review P2#1) ===\n" );
	Fixture f( "pane_sched_j.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "initial Quad rotation completes (4 passes)" );
	Check( f.ctrl->SettlesAt( f.ctrl->Sequence().size(), kSettleMs ), "Quad settles" );
	// After a settled Quad rotation the scheduler's current pane is a
	// high index (the last one rendered) -- i.e. one that Single hides.
	Check( f.ctrl->ForTest_CurrentPane() >= 1,
	       "precondition: the current pane is a secondary (Single will hide it)" );
	f.ctrl->ClearSequence();

	// Shrink to Single.  The fix must relocate to pane 0 and repaint it,
	// and NO further pass may land on a hidden pane (index >= 1).
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Single ), "shrink Quad -> Single" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
	       "MONEY ASSERTION (j): the shrink repaints the visible primary (pane 0)" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );   // drain any stray passes
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	bool anyHidden = false;
	for( std::size_t i = 0; i < seq.size(); ++i ) {
		if( seq[i] >= 1 ) anyHidden = true;
	}
	Check( !anyHidden,
	       "MONEY ASSERTION (j): NO pass lands on a hidden pane (>=1) after the shrink -- the "
	       "scheduler relocated to pane 0 instead of continuing on the now-invisible pane" );
	Check( f.ctrl->ForTest_CurrentPane() == 0,
	       "the scheduler's current pane is the visible primary (0) after the shrink" );
}

//----------------------------------------------------------------------
// (j2) A shrink can interrupt an object-transform drag, not just an idle
// scheduler slot.  It must complete the same transaction cleanup as PointerUp:
// close the composite, persist the pending transform, drop the gizmo drag, and
// never schedule the hidden pane again.
//----------------------------------------------------------------------
static void RunShrinkFinalizesHiddenGestureTest()
{
	std::printf( "=== scheduler (j2): shrink finalizes an active hidden-pane transform gesture ===\n" );
	Fixture f( "pane_sched_j2.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "initial Quad rotation completes" );
	Check( f.ctrl->SettlesAt( f.ctrl->Sequence().size(), kSettleMs ), "Quad settles" );

	f.ctrl->ForTest_SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	f.ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	f.ctrl->RefreshGizmoHandles();
	int center = -1;
	for( unsigned int i = 0; i < f.ctrl->GizmoHandleCount(); ++i ) {
		if( f.ctrl->GizmoHandleKind( i )
		    == static_cast<int>( SceneEditController::GizmoHandle::Kind::ScreenCenter ) ) {
			center = static_cast<int>( i );
			break;
		}
	}
	Check( center >= 0, "a ScreenCenter transform handle is available" );
	if( center < 0 ) return;
	const Point2 down( f.ctrl->GizmoHandleScreenX( static_cast<unsigned int>( center ) ),
	                   f.ctrl->GizmoHandleScreenY( static_cast<unsigned int>( center ) ) );
	Check( f.ctrl->OnPanePointerDown( 3, down ), "pane-3 transform Down is accepted" );
	Check( f.ctrl->Editor().IsCompositeOpen() && f.ctrl->IsGizmoDragActive(),
	       "precondition: pane-3 transform gesture has an open composite and active gizmo drag" );
	f.ctrl->OnPanePointerMove( 3, Point2( down.x + 4.0, down.y + 3.0 ) );
	Check( f.ctrl->Editor().HasPendingCstObjectTransforms(),
	       "precondition: transform gesture accumulated a CST-pending object edit" );
	f.ctrl->ClearSequence();

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::OnePlusTwo ),
	       "shrink Quad -> OnePlusTwo hides the gestured pane 3" );
	Check( !f.ctrl->Editor().IsCompositeOpen(),
	       "MONEY (j2): shrink closes the interrupted gesture composite" );
	Check( !f.ctrl->Editor().HasPendingCstObjectTransforms(),
	       "MONEY (j2): shrink commits the interrupted gesture's pending CST transform" );
	Check( !f.ctrl->IsGizmoDragActive(),
	       "MONEY (j2): shrink clears the interrupted gizmo drag state" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
	       "shrink queues a repaint for the remaining visible panes" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	bool renderedHidden = false;
	for( std::size_t i = 0; i < seq.size(); ++i ) {
		if( seq[i] >= 3 ) renderedHidden = true;
	}
	Check( !renderedHidden,
	       "MONEY (j2): no post-shrink render targets hidden pane 3 after a finalized drag" );
}

//----------------------------------------------------------------------
// (j2b) mGesturePane belongs to pointer gestures only.  A later property
// scrub must not be cancelled merely because a layout shrink hides the
// stale pane number left by an already-completed pointer gesture.
//----------------------------------------------------------------------
static void RunShrinkPreservesUnrelatedPropertyScrubTest()
{
	std::printf( "=== scheduler (j2b/T0): shrink ignores stale pointer pane during property scrub ===\n" );
	Fixture f( "pane_sched_j2b.RISEscene", true );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "initial Quad rotation completes" );
	Check( f.ctrl->SettlesAt( 4, kSettleMs ), "Quad settles" );

	// Complete a pane-3 pointer gesture, leaving mGesturePane's historical
	// value at 3, then force the scheduler's current context to pane 0.
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( f.ctrl->OnPanePointerDown( 3, Point2( 5, 5 ) ), "pane-3 Down accepted" );
	Check( f.ctrl->OnPanePointerUp( 3, Point2( 5, 5 ) ), "pane-3 Up accepted" );
	Check( f.ctrl->WaitForPassCount( 9, kWaitMs ),
	       "completed pane-3 gesture drains its Quad final/polish sequence" );
	Check( f.ctrl->SettlesAt( 9, kSettleMs ), "completed pointer gesture settles" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPrimaryPane( 3 ),
	       "make pane 3 primary so the next Quad rotation finishes on visible pane 2" );
	Check( f.ctrl->SetViewportRenderMode( "normals" ),
	       "pane-0 mode switch makes pane 0 current" );
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "mode change repaints Quad" );
	Check( f.ctrl->SettlesAt( 4, kSettleMs ), "pane-0 mode rotation settles" );
	Check( f.ctrl->ForTest_CurrentPane() == 2,
	       "precondition: current pane 2 remains visible after OnePlusTwo shrink" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPaneRenderMode( 2, "indirect" ),
	       "current pane 2 uses a fixed-divisor variant during the scrub oracle" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "pane-2 variant realization completes" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ), "pane-2 variant realization settles" );
	f.ctrl->ClearSequence();

	f.ctrl->BeginPropertyScrub();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::OnePlusTwo ),
	       "shrink Quad -> OnePlusTwo hides only the stale pointer pane 3" );
	Check( f.ctrl->SettlesAt( 0, kSettleMs ),
	       "MONEY (j2b/T0): stale pointer pane does not make the shrink queue a spurious pass" );
	f.ctrl->ClearSequence();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "one scrub edit pass lands" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ),
	       "MONEY (j2b/T0): the unrelated property scrub remains active and pins pane 0" );
	f.ctrl->EndPropertyScrub();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
	       "explicit scrub End resumes all three visible panes" );
	Check( f.ctrl->SettlesAt( 4, kSettleMs ), "post-End three-pane rotation quiesces" );

	// Concurrent gestures are legal controller state: hiding the pointer
	// gesture's pane must finalize only that pointer gesture and preserve the
	// independent property scrub's motion-quality divisor on the new pane.
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetViewportRenderMode( "preview" ),
	       "destination pane 0 is unpinned Preview for the divisor-transfer oracle" );
	Check( f.ctrl->WaitForPassCount( 3, kWaitMs ), "pane-0 Preview change repaints three panes" );
	Check( f.ctrl->SettlesAt( 3, kSettleMs ), "pane-0 Preview rotation settles" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
	       "grow OnePlusTwo -> Quad for concurrent-gesture shrink" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "revealed pane 3 renders" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ), "grow settles on pane 3" );
	f.ctrl->ForTest_SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	f.ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	f.ctrl->RefreshGizmoHandles();
	int center = -1;
	for( unsigned int i = 0; i < f.ctrl->GizmoHandleCount(); ++i ) {
		if( f.ctrl->GizmoHandleKind( i )
		    == static_cast<int>( SceneEditController::GizmoHandle::Kind::ScreenCenter ) ) {
			center = static_cast<int>( i );
			break;
		}
	}
	Check( center >= 0, "concurrent-shrink ScreenCenter handle exists" );
	if( center < 0 ) return;
	const Point2 down( f.ctrl->GizmoHandleScreenX( static_cast<unsigned int>( center ) ),
	                   f.ctrl->GizmoHandleScreenY( static_cast<unsigned int>( center ) ) );
	Check( f.ctrl->OnPanePointerDown( 3, down ), "pane-3 pointer gesture starts" );
	f.ctrl->BeginPropertyScrub();
	int phase = -1;
	unsigned int expectedMotionDivisor = 0;
	Check( f.ctrl->GetPaneRefinementStatus( 3, phase, expectedMotionDivisor )
	    && expectedMotionDivisor > 1,
	       "precondition: property scrub establishes a motion divisor on pane 3" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::OnePlusTwo ),
	       "shrink finalizes hidden pointer gesture while property scrub remains active" );
	unsigned int divisor = 0;
	Check( f.ctrl->GetPaneRefinementStatus( 0, phase, divisor )
	    && divisor == expectedMotionDivisor,
	       "MONEY (j2c/T0): surviving scrub transfers the exact motion divisor to unpinned pane 0" );
	f.ctrl->ClearSequence();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "surviving scrub edit renders once" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		Check( seq.size() == 1 && seq[0] == 0,
		       "surviving property scrub keeps the edit on new current pane 0" );
	}
	f.ctrl->EndPropertyScrub();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
	       "concurrent-shrink scrub End resumes all three panes" );
	Check( f.ctrl->SettlesAt( 4, kSettleMs ),
	       "concurrent-shrink recovery quiesces" );
}

//----------------------------------------------------------------------
// (j3) Platform paint/timer code asks for layout and primary while an agent
// render holds mMutex for its full duration.  Those UI-read snapshots must be
// lock-free; a waiter is intentionally held open long enough to catch a
// blocking getter without depending on render speed.
//----------------------------------------------------------------------
static void RunLayoutSnapshotsDoNotBlockParkedRenderTest()
{
	std::printf( "=== scheduler (j3): layout/primary snapshots stay nonblocking during parked render ===\n" );
	Fixture f( "pane_sched_j3.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	Check( f.ctrl->SetPrimaryPane( 2 ), "primary := pane 2" );

	std::mutex gateMutex;
	std::condition_variable gateCV;
	bool entered = false, release = false, parked = false;
	std::thread holder( [&]() {
		parked = f.ctrl->RunPreviewRenderParked( [&]() {
			std::unique_lock<std::mutex> lk( gateMutex );
			entered = true;
			gateCV.notify_all();
			gateCV.wait( lk, [&]{ return release; } );
		} );
	} );
	{
		std::unique_lock<std::mutex> lk( gateMutex );
		Check( gateCV.wait_for( lk, std::chrono::milliseconds( kWaitMs ), [&]{ return entered; } ),
		       "parked render closure entered while holding the controller mutex" );
	}

	SceneEditController::ViewportLayout observedLayout = SceneEditController::ViewportLayout::Single;
	unsigned int observedPrimary = 99;
	bool gettersDone = false;
	std::thread reader( [&]() {
		observedLayout = f.ctrl->GetViewportLayout();
		observedPrimary = f.ctrl->GetPrimaryPane();
		std::lock_guard<std::mutex> lk( gateMutex );
		gettersDone = true;
		gateCV.notify_all();
	} );
	{
		std::unique_lock<std::mutex> lk( gateMutex );
		Check( gateCV.wait_for( lk, std::chrono::milliseconds( 250 ), [&]{ return gettersDone; } ),
		       "MONEY (j3): layout/primary getters return while the agent render still owns mMutex" );
		release = true;
		gateCV.notify_all();
	}
	holder.join();
	reader.join();
	Check( parked, "parked render completed after the test released it" );
	Check( observedLayout == SceneEditController::ViewportLayout::Quad && observedPrimary == 2,
	       "MONEY (j3): lock-free getters return the coherent published Quad / pane-2 snapshot" );
}

//----------------------------------------------------------------------
// (k) user-review P1#6 (round 2): with the PRIMARY pane set to a
//     SECONDARY, the pane-0 ALIAS getters/setters must STILL target pane
//     0.  The nav funnel targets the primary pane; the pane-0 alias
//     forwarders pass an EXPLICIT 0.  Regression under review: repointing
//     the funnel at mPrimaryPane WITHOUT splitting the alias made
//     GetPaneVantage(0) (and the pane-0 vantage setters) operate on the
//     PRIMARY pane instead of pane 0.
//----------------------------------------------------------------------
static void RunPaneZeroAliasUnderSecondaryPrimaryTest()
{
	std::printf( "=== scheduler (k): pane-0 alias unaffected by a secondary primary (user-review P1#6 r2) ===\n" );
	Fixture f( "pane_sched_k.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	using Kind = SceneEditController::PaneVantageKind;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	Check( f.ctrl->SetPrimaryPane( 2 ), "primary := pane 2 (a secondary)" );
	// Put the PRIMARY (pane 2) into free-fly, then pump a full rotation so
	// the scheduler realizes it into pane 2's registers/slot (poseActive).
	Check( f.ctrl->PaneEnterFreeFly( 2 ), "pane 2 enters free-fly" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ), "a full Quad rotation runs (pane 2's free-fly realized)" );
	Check( f.ctrl->SettlesAt( f.ctrl->Sequence().size(), kSettleMs ), "settles" );

	// The primary pane reports its OWN free-fly (config-backed for a
	// secondary) -- confirms the setup took.
	Kind k2 = Kind::SceneCamera; String nv2;
	Check( f.ctrl->GetPaneVantage( 2, k2, nv2 ), "GetPaneVantage(2) succeeds" );
	Check( k2 == Kind::FreeFly, "pane 2 (the primary) reports FreeFly" );

	// MONEY: pane 0's alias getter reports pane 0's OWN vantage
	// (SceneCamera) -- NOT the secondary primary's FreeFly.  The buggy
	// funnel routed GetPaneVantage(0) -> IsFreeFlyActive() at mPrimaryPane==2
	// and would report FreeFly here.
	Kind k0 = Kind::FreeFly; String nv0( "dirty" );
	Check( f.ctrl->GetPaneVantage( 0, k0, nv0 ), "GetPaneVantage(0) succeeds" );
	Check( k0 == Kind::SceneCamera,
	       "MONEY ASSERTION (k): GetPaneVantage(0) reports pane 0's OWN vantage "
	       "(SceneCamera), NOT the secondary primary pane 2's FreeFly -- the pane-0 "
	       "alias forwards an EXPLICIT 0, unaffected by mPrimaryPane" );
	Check( nv0.size() <= 1, "pane 0 named-view name is empty (SceneCamera)" );
}

//----------------------------------------------------------------------
// (l) user-review P1-1: the UNINDEXED nav path aliases PANE 0 (the §7.4
//     C-ABI contract "existing calls = pane 0"), NOT the primary pane.
//     The pane-INDEXED path targets the named pane.  Regression: making
//     the funnel default to mPrimaryPane made a legacy unindexed Snap /
//     Home / FreeFly mutate a SECONDARY pane whenever it was primary.
//----------------------------------------------------------------------
static void RunUnindexedNavAliasesPaneZeroTest()
{
	std::printf( "=== scheduler (l): unindexed nav aliases pane 0; indexed targets the pane (user-review P1-1) ===\n" );
	Fixture f( "pane_sched_l.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	using Kind = SceneEditController::PaneVantageKind;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad" );
	Check( f.ctrl->SetPrimaryPane( 2 ), "primary := pane 2 (a secondary)" );

	// UNINDEXED snap (explicit pane 0, exactly what the legacy C-ABI wrapper
	// passes) must free-fly PANE 0 and leave the secondary primary untouched.
	Check( RISE_API_SceneEditController_SnapViewToAxis( f.ctrl, 0, 0 ),
	       "UNINDEXED C-ABI SnapViewToAxis succeeds (must alias pane 0)" );
	Kind k0 = Kind::SceneCamera; String nv0;
	Check( f.ctrl->GetPaneVantage( 0, k0, nv0 ), "GetPaneVantage(0) succeeds" );
	Check( k0 == Kind::FreeFly, "MONEY (l): unindexed snap put PANE 0 into free-fly" );
	Kind k2 = Kind::FreeFly; String nv2;
	Check( f.ctrl->GetPaneVantage( 2, k2, nv2 ), "GetPaneVantage(2) succeeds" );
	Check( k2 == Kind::SceneCamera,
	       "MONEY (l): the secondary PRIMARY pane 2 is UNTOUCHED by the unindexed "
	       "(pane-0) nav -- the §7.4 alias contract holds even when primary != 0" );

	// INDEXED snap targets pane 2.
	Check( RISE_API_SceneEditController_SnapPaneViewToAxis( f.ctrl, 2, 1, 0 ),
	       "INDEXED C-ABI SnapPaneViewToAxis(pane 2) succeeds" );
	Kind k2b = Kind::SceneCamera; String nv2b;
	Check( f.ctrl->GetPaneVantage( 2, k2b, nv2b ), "GetPaneVantage(2) succeeds after indexed snap" );
	Check( k2b == Kind::FreeFly, "MONEY (l): the INDEXED snap put pane 2 into free-fly" );
}

//----------------------------------------------------------------------
// (m) Entering Free-Fly from a secondary NamedView must preserve the view
//     being displayed.  Regression: PaneEnterFreeFly captured the scene
//     camera, visibly jumping away from the named view as soon as the menu
//     action was selected.
//----------------------------------------------------------------------
static void RunPaneFreeFlyPreservesNamedViewTest()
{
	std::printf( "=== scheduler (m): secondary Free-Fly preserves its NamedView pose ===\n" );
	Fixture f( "pane_sched_m.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ), "layout TwoH" );
	Check( f.ctrl->SnapViewToAxis( 0, false, 0 ), "pane 0 snap creates a non-scene-camera side view" );
	Check( f.ctrl->CaptureNamedView( "side" ), "capture side NamedView from pane 0 free-fly" );
	CameraSnapshot expected;
	Check( f.ctrl->FindNamedViewPose( String( "side" ), expected ), "read back side NamedView pose" );
	Check( f.ctrl->ExitFreeFly( 0 ), "pane 0 returns to the scene camera" );
	Check( f.ctrl->SetPaneVantageNamedView( 1, "side" ), "pane 1 displays the side NamedView" );

	Check( f.ctrl->PaneEnterFreeFly( 1 ), "pane 1 enters Free-Fly" );
	SceneEditController::PaneVantageKind kind;
	String named;
	Check( f.ctrl->GetPaneVantage( 1, kind, named )
	    && kind == SceneEditController::PaneVantageKind::FreeFly,
	       "pane 1 now reports Free-Fly" );

	// Realize the configured pane once, then inspect its saved/free-fly pose.
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "both TwoH panes render" );
	CameraSnapshot actual;
	Check( f.ctrl->GetViewportPose( actual, 1 ), "pane 1 has a realized Free-Fly pose" );
	Check( actual.location[0] == expected.location[0]
	    && actual.location[1] == expected.location[1]
	    && actual.location[2] == expected.location[2]
	    && actual.lookat[0] == expected.lookat[0]
	    && actual.lookat[1] == expected.lookat[1]
	    && actual.lookat[2] == expected.lookat[2],
	       "MONEY (m): pane 1 Free-Fly keeps the NamedView pose, not the scene camera" );
}

//----------------------------------------------------------------------
// (n) Pane* C-ABI nav wrappers forward `pane` RAW.  The nav funnels must
//     reject both an out-of-range index and an in-range HIDDEN pane: neither
//     may park/cancel a pass, switch the scheduler, or mutate pane state.
//----------------------------------------------------------------------
static void RunOutOfRangePaneNavRejectedTest()
{
	std::printf( "=== scheduler (n): out-of-range pane nav fails closed, no OOB (user-review round 2) ===\n" );
	Fixture f( "pane_sched_m.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	const unsigned int bad = 9;   // >= kViewportPaneCount (4)
	const unsigned int privateSentinel = SceneEditController::kViewportNavPrimary;

	Check( !RISE_API_SceneEditController_SnapPaneViewToAxis( f.ctrl, bad, 0, 0 ),
	       "MONEY (n): SnapPaneViewToAxis(out-of-range) fails closed (no OOB write to mPaneConfigs)" );
	Check( !RISE_API_SceneEditController_IsPaneFreeFlyActive( f.ctrl, bad ),
	       "MONEY (n): IsPaneFreeFlyActive(out-of-range) fails closed (no OOB read of mPaneRender)" );
	Check( !RISE_API_SceneEditController_PaneSetHomeView( f.ctrl, bad ),
	       "MONEY (n): PaneSetHomeView(out-of-range) fails closed" );
	Check( !RISE_API_SceneEditController_PaneGoToHomeView( f.ctrl, bad ),
	       "MONEY (n): PaneGoToHomeView(out-of-range) fails closed" );
	char nm[64] = {0};
	Check( !RISE_API_SceneEditController_PaneStampViewToNewCamera( f.ctrl, bad, "v", nm, sizeof( nm ) ),
	       "MONEY (n): PaneStampViewToNewCamera(out-of-range) fails closed" );

	// The internal C++ default-argument sentinel is UINT_MAX.  It means
	// "primary pane" only inside the C++ nav funnel; public indexed C callers
	// must never be able to smuggle it through as a pane and mutate primary.
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
	       "layout Quad for private-sentinel checks" );
	Check( f.ctrl->SetPrimaryPane( 1 ), "primary := pane 1 for private-sentinel checks" );
	Check( RISE_API_SceneEditController_SnapPaneViewToAxis( f.ctrl, 1, 0, 0 ),
	       "control: indexed snap on actual primary pane succeeds" );
	Check( !RISE_API_SceneEditController_SnapPaneViewToAxis( f.ctrl, privateSentinel, 0, 0 ),
	       "MONEY (n): private nav sentinel is rejected by indexed SnapPaneViewToAxis" );
	Check( !RISE_API_SceneEditController_IsPaneFreeFlyActive( f.ctrl, privateSentinel ),
	       "MONEY (n): private nav sentinel is rejected by indexed IsPaneFreeFlyActive" );
	Check( RISE_API_SceneEditController_PaneSetHomeView( f.ctrl, 1 ),
	       "control: indexed SetHome on actual primary pane succeeds" );
	Check( !RISE_API_SceneEditController_PaneSetHomeView( f.ctrl, privateSentinel ),
	       "MONEY (n): private nav sentinel is rejected by indexed PaneSetHomeView" );
	Check( !RISE_API_SceneEditController_PaneGoToHomeView( f.ctrl, privateSentinel ),
	       "MONEY (n): private nav sentinel is rejected by indexed PaneGoToHomeView" );
	const unsigned int cameraCountBefore = f.job->GetScene()->GetCameras()->getItemCount();
	Check( !RISE_API_SceneEditController_PaneStampViewToNewCamera(
		f.ctrl, privateSentinel, "v", nm, sizeof( nm ) ),
	       "MONEY (n): private nav sentinel is rejected by indexed PaneStampViewToNewCamera" );
	Check( f.job->GetScene()->GetCameras()->getItemCount() == cameraCountBefore,
	       "MONEY (n): rejected private sentinel never creates a camera on the primary pane" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Single ),
	       "restore Single for hidden-pane checks" );
	// In Single layout, pane 2 is a valid array
	// slot but hidden.  This is the real regression topology: bounds-only
	// validation let SnapViewToAxis switch the scheduler to this invisible pane.
	const unsigned int hidden = 2;
	Check( !RISE_API_SceneEditController_SnapPaneViewToAxis( f.ctrl, hidden, 0, 0 ),
	       "MONEY (n): SnapPaneViewToAxis(hidden) fails closed without selecting a hidden pane" );
	Check( !RISE_API_SceneEditController_PaneSetHomeView( f.ctrl, hidden ),
	       "MONEY (n): PaneSetHomeView(hidden) fails closed" );
	Check( !RISE_API_SceneEditController_PaneGoToHomeView( f.ctrl, hidden ),
	       "MONEY (n): PaneGoToHomeView(hidden) fails closed" );
	Check( !RISE_API_SceneEditController_PaneStampViewToNewCamera( f.ctrl, hidden, "v", nm, sizeof( nm ) ),
	       "MONEY (n): PaneStampViewToNewCamera(hidden) fails closed" );

	// Control: an in-range call is well-defined and doesn't crash.
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ), "layout Quad (control)" );
	Check( RISE_API_SceneEditController_IsPaneFreeFlyActive( f.ctrl, 1 ),
	       "control: the valid pane-1 query still observes the successful control snap" );
}

int main()
{
	RunRotationOrderTest();
	RunPaneLocalInvalidationTest();
	RunGesturePinningTest();
	RunVariantGizmoGestureDoesNotSpinTest();
	RunVariantPropertyScrubRecoveryTest();
	RunStopRecoversLostPointerUpTest();
	RunPausePreservesPolishStateTest();
	RunMidGesturePaneSwitchRefusedTest();
	RunSingleLayoutBaselineTest();
	RunHiddenPaneNeverRendersTest();
	RunLayoutGrowWakesLoopTest();
	RunNamedViewUpdatePropagatesTest();
	RunPaneIndexedGestureTest();
	RunShrinkCancelsHiddenPaneTest();
	RunShrinkFinalizesHiddenGestureTest();
	RunShrinkPreservesUnrelatedPropertyScrubTest();
	RunLayoutSnapshotsDoNotBlockParkedRenderTest();
	RunPaneZeroAliasUnderSecondaryPrimaryTest();
	RunUnindexedNavAliasesPaneZeroTest();
	RunPaneFreeFlyPreservesNamedViewTest();
	RunOutOfRangePaneNavRejectedTest();

	std::printf( "\nViewportPaneSchedulerTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
