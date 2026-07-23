//////////////////////////////////////////////////////////////////////
//
//  AgentRenderAsyncTest.cpp - Model-B F2 slice S2a (async agent render).
//
//    Proves the NEW controller/session surface this slice adds:
//
//      * SceneEditController::SubmitAgentRenderAsync / SubmitAgentRenderSync
//        run a caller's closure on a DEDICATED, long-lived worker thread
//        under the SAME cancel-and-park critical section
//        RunPreviewRenderParked already uses -- so the worker and the
//        interactive render loop are NEVER inside Rasterize()/
//        DoOneRenderPass at the same time.
//      * SceneEditController::GetRenderJobStatus / WaitForRenderJob --
//        the Status/Wait surface, including the ODD (session-local) id
//        rejection contract carried over from ce9f5e03.
//      * AgentSession::RenderAsync / RenderStatus / RenderWait -- the
//        session-level wrapper a JSON-RPC `render{"async":true}` /
//        `render_status` / `render_wait` verb calls into.
//      * The no-override PRE-EXISTING RACE CLOSURE: a controller-attached,
//        no-override AgentSession::Render() call used to invoke
//        mJob->Rasterize() DIRECTLY on the calling thread with NO park at
//        all (documented on AgentSession.h's Render(AgentRenderParams)
//        LIVE-MODE SAFETY note) -- this file RED-PROVES that gap is now
//        closed by driving a REAL concurrent interactive render thread
//        alongside a real Rasterize() call and asserting max observed
//        concurrency is 1.
//
//  Self-contained: an inline native-v7 scene (a lit sphere), OIDN off,
//  no RISE_MEDIA_PATH.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"   // slice S2b: render_cancel verb round-trip through HandleLine
#include "../src/Library/Agent/Json.h"       // slice S2b: JsonValue/JsonParse/JsonSerialize for the RPC-verb round-trip
#include "../src/Library/Job.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/CancellableProgressCallback.h"   // Model-B F2 slice S4: composed progress/cancel test
#include "../src/Library/Interfaces/ILogPriv.h"   // round-2 P1-1 red-prove: capture the escalating cancel-ignored warning

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
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

//////////////////////////////////////////////////////////////////////
// Fix-round-1 test infra: a WATCHDOG for "this call must not hang" RED-
// PROOFS.  Every headline fix in this round (P1-1 post-Stop() refusal,
// P1-A the async-lifetime drain, P2-C Stop()-during-a-render) claims a
// BOUNDED wait where the pre-fix code could hang forever -- a plain
// Check() around a call that genuinely hangs would just make the WHOLE
// TEST BINARY stall silently (no failure printed, no pass/fail count,
// the harness eventually times out with no diagnostic pointing at which
// assertion hung).  RunWatchdogged runs `fn` on a background thread and
// gives it `timeoutMs`; if `fn` has not signalled completion by then,
// this prints a loud, specific FAIL identifying which claim hung and
// returns false WITHOUT waiting for the stuck thread (a real hang in
// the code under test would otherwise wedge the test process itself --
// the watchdog thread is deliberately leaked/detached in that case so
// the test binary can still finish and report every OTHER test's
// results; a leaked thread on a FAILING run is an acceptable trade for
// "the suite still reports its failures instead of hanging CI").
//////////////////////////////////////////////////////////////////////
static bool RunWatchdogged( const std::string& what, unsigned int timeoutMs, const std::function<void()>& fn )
{
	auto done = std::make_shared<std::atomic<bool>>( false );
	std::thread worker( [fn, done]() {
		fn();
		done->store( true, std::memory_order_release );
	} );
	worker.detach();

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeoutMs );
	while( std::chrono::steady_clock::now() < deadline )
	{
		if( done->load( std::memory_order_acquire ) ) return true;
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
	std::printf( "  FAIL: WATCHDOG TRIPPED -- \"%s\" did not complete within %ums (this claim was supposed to be BOUNDED; treat as a hang, not a slow pass)\n",
	             what.c_str(), timeoutMs );
	return false;
}

// Mirrors AgentProposeRenderTest.cpp's kScene -- a small lit diffuse
// sphere, path-traced at a low sample count with OIDN off.
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
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

//////////////////////////////////////////////////////////////////////
// Shared concurrency-proof counter.  Both the interactive render loop
// (via a test controller's slowed DoOneRenderPass) and an agent render
// (via a Job subclass's Rasterize() override) bump the SAME atomic
// counter around their "in the render critical section" window and
// track the maximum simultaneously-observed value.  The S2a claim is
// that this maximum is NEVER > 1 -- i.e. the worker's cancel-and-park
// hold genuinely excludes the interactive loop, exactly like
// RunPreviewRenderParked already does.
//////////////////////////////////////////////////////////////////////
struct ConcurrencyProof
{
	std::atomic<int> current{ 0 };
	std::atomic<int> maxObserved{ 0 };

	void Enter()
	{
		const int now = current.fetch_add( 1, std::memory_order_acq_rel ) + 1;
		int prevMax = maxObserved.load( std::memory_order_acquire );
		while( now > prevMax && !maxObserved.compare_exchange_weak( prevMax, now, std::memory_order_acq_rel ) ) {}
	}
	void Leave()
	{
		current.fetch_sub( 1, std::memory_order_acq_rel );
	}
};

// A real render thread whose DoOneRenderPass simulates cancel-checked
// work (mirrors AgentProposeRenderTest.cpp's RenderJobIdTestController)
// AND participates in the shared ConcurrencyProof counter.
class ConcurrencyTestController : public SceneEditController
{
public:
	ConcurrencyTestController( IJobPriv& job, ConcurrencyProof& proof, unsigned int simulatedRenderMs = 20 )
	: SceneEditController( job, /*interactiveRasterizer*/0 )
	, mProof( proof )
	, mSimulatedRenderMs( simulatedRenderMs )
	{}

protected:
	void DoOneRenderPass() override
	{
		mProof.Enter();
		const unsigned int sliceMs = 2;
		const unsigned int slices  = ( mSimulatedRenderMs + sliceMs - 1 ) / sliceMs;
		for( unsigned int i = 0; i < slices; ++i )
		{
			if( IsCancelRequested() ) { mProof.Leave(); return; }
			std::this_thread::sleep_for( std::chrono::milliseconds( sliceMs ) );
		}
		mProof.Leave();
	}

private:
	ConcurrencyProof& mProof;
	unsigned int      mSimulatedRenderMs;
};

// A Job subclass whose Rasterize() override participates in the SAME
// ConcurrencyProof counter around the REAL base Job::Rasterize() call --
// this is the exact call site AgentSession::RenderCore_'s doRenderWork
// invokes, so a concurrency violation here is the real race, not a
// synthetic stand-in.
class ConcurrencyProofJob : public Job
{
public:
	explicit ConcurrencyProofJob( ConcurrencyProof& proof ) : Job(), mProof( proof ) {}
	bool Rasterize() override
	{
		mProof.Enter();
		mRasterizeCallCount.fetch_add( 1, std::memory_order_acq_rel );
		// A tiny extra hold so a concurrent interactive pass has a
		// realistic window to land in even though the real render itself
		// is fast on this tiny 24x24 scene.
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		const bool ok = Job::Rasterize();
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		mProof.Leave();
		return ok;
	}
	// Fix-round-3 (refused-fallback red-prove): a plain call counter,
	// independent of the concurrency-window accounting above -- lets a
	// test assert "Rasterize() was never even ATTEMPTED" (count stays 0)
	// rather than only "no concurrency was observed", which is the
	// stronger claim the refused-submission fix makes: a refused
	// production render must not call doRasterize() AT ALL, not merely
	// "call it without overlapping the occupant".
	int RasterizeCallCount() const { return mRasterizeCallCount.load( std::memory_order_acquire ); }
private:
	ConcurrencyProof& mProof;
	std::atomic<int>  mRasterizeCallCount{ 0 };
};

// A Job subclass whose Rasterize() sleeps in cancel-checked-ish slices
// (no real cancel token available at this layer, just a plain sleep) so
// SubmitAgentRenderAsync's "returns quickly, render finishes later" claim
// has a wide, deterministic window to observe.
class SlowRasterizeJob : public Job
{
public:
	SlowRasterizeJob() : Job() {}
	void SetSleepMs( unsigned int ms ) { mSleepMs = ms; }
	bool Rasterize() override
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( mSleepMs ) );
		return Job::Rasterize();
	}
private:
	unsigned int mSleepMs = 200;
};

// Fix-round-1 P1-A: a Job subclass whose Rasterize() bumps a shared
// counter around the sleep/base-Rasterize call -- lets a test PROVE the
// worker thread is genuinely INSIDE Rasterize() (not merely "submitted")
// before it does something dangerous (like destroying the AgentSession
// whose closure is running this call), and PROVE the worker has fully
// LEFT Rasterize() by the time some later event (like ~AgentSession
// returning) has occurred -- a direct, timing-independent ordering proof
// rather than an inference from wall-clock durations alone.
class CanaryJob : public Job
{
public:
	CanaryJob( unsigned int sleepMs, std::shared_ptr<std::atomic<int>> counter )
	: Job(), mSleepMs( sleepMs ), mCounter( std::move( counter ) ) {}
	bool Rasterize() override
	{
		mCounter->fetch_add( 1, std::memory_order_acq_rel );
		std::this_thread::sleep_for( std::chrono::milliseconds( mSleepMs ) );
		const bool ok = Job::Rasterize();
		mCounter->fetch_sub( 1, std::memory_order_acq_rel );
		return ok;
	}
private:
	unsigned int mSleepMs;
	std::shared_ptr<std::atomic<int>> mCounter;
};

// A Job subclass whose Rasterize() always throws -- mirrors
// AgentProposeRenderTest.cpp's ThrowingRasterizeJob (the real
// mJob->Rasterize() call still runs; the exception fires AFTER).
class ThrowingRasterizeJob : public Job
{
public:
	ThrowingRasterizeJob() : Job(), mThrowOnRasterize( true ) {}
	void SetThrowOnRasterize( bool on ) { mThrowOnRasterize = on; }
	bool Rasterize() override
	{
		const bool base = Job::Rasterize();
		if( mThrowOnRasterize ) {
			throw std::runtime_error( "AgentRenderAsyncTest: simulated OIDN-class throw from Rasterize()" );
		}
		return base;
	}
private:
	bool mThrowOnRasterize;
};

//////////////////////////////////////////////////////////////////////
// (a) SubmitAgentRenderAsync returns quickly while a slow render runs;
//     Status reports active; WaitForRenderJob completes with the result
//     retrievable via ReadImage() once done.
//////////////////////////////////////////////////////////////////////
static void RunAsyncReturnsQuicklyTest()
{
	std::printf( "=== AgentRenderAsyncTest: (a) SubmitAgentRenderAsync returns quickly, Status/Wait observe completion ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_quick.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the async-quick scene to a temp file" );

	SlowRasterizeJob* pJob = new SlowRasterizeJob();
	pJob->SetSleepMs( 200 );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowRasterizeJob loads the native-v7 scene via the CST path" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the slow Job" );
	if( session )
	{
		session->AttachController( &controller );

		AgentRenderParams p;   // no override -- exercises the no-override async path
		const auto t0 = std::chrono::steady_clock::now();
		const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
		const auto t1 = std::chrono::steady_clock::now();
		const long long submitMs = std::chrono::duration_cast<std::chrono::milliseconds>( t1 - t0 ).count();

		Check( ar.accepted, "RenderAsync accepts the submission" );
		Check( ar.renderJobId != 0, "RenderAsync assigns a nonzero renderJobId" );
		Check( ( ar.renderJobId % 2 ) == 0, "RenderAsync's id is EVEN (coordinator-tracked)" );
		Check( submitMs < 50, "RenderAsync returns in well under the 200ms simulated render (observed submit time < 50ms)" );

		// Status should report active soon after submission (the worker
		// may not have started the instant we check, but the job record
		// is set BEFORE the worker is even woken, in SubmitAgentRenderAsync
		// itself -- so this should be observable essentially immediately).
		bool sawActive = false;
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 500 );
			while( std::chrono::steady_clock::now() < deadline )
			{
				const AgentSession::AgentRenderJobStatus st = session->RenderStatus( ar.renderJobId );
				Check( st.found, "RenderStatus recognizes the freshly-submitted id" );
				if( st.active ) { sawActive = true; break; }
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
		}
		Check( sawActive, "RenderStatus observes the async job ACTIVE before it completes" );

		const bool completed = session->RenderWait( ar.renderJobId, 5000 );
		Check( completed, "RenderWait observes the async job complete within the timeout" );

		const AgentSession::AgentRenderJobStatus after = session->RenderStatus( ar.renderJobId );
		Check( after.found && !after.active, "RenderStatus reports the job inactive after RenderWait returns" );

		// The result is retrievable via ReadImage() once the async render
		// has completed (same cache-population tail as a synchronous
		// render).
		const std::vector<unsigned char> png = session->ReadImage();
		Check( !png.empty(), "ReadImage() returns non-empty bytes after the async render completes" );

		// Model-B F2 slice S2b: LastAsyncRenderResult retrieves the SAME
		// stats a synchronous Render() would have returned directly, for
		// a render that completed NATURALLY (not cancelled) -- the
		// positive-path sibling of the cancelled-render check in
		// RunCancelAsyncRenderRedProveTest.
		const AgentSession::AgentLastAsyncRenderResult lastResult = session->LastAsyncRenderResult( ar.renderJobId );
		Check( lastResult.found, "LastAsyncRenderResult finds the just-completed job's cached stats" );
		Check( lastResult.result.ok, "the naturally-completed async render's cached result reports ok=true" );
		Check( lastResult.result.renderJobId == ar.renderJobId, "the cached result's own renderJobId matches the job it was cached under" );
		Check( lastResult.result.width > 0 && lastResult.result.height > 0, "the cached result carries nonzero dimensions" );

		// A DIFFERENT (unrelated, never-submitted) job id must NOT match --
		// the strict-identity contract, not "whatever's most recently
		// cached".
		const AgentSession::AgentLastAsyncRenderResult wrongId = session->LastAsyncRenderResult( ar.renderJobId + 1000 );
		Check( !wrongId.found, "LastAsyncRenderResult reports found=false for a renderJobId that does not match the cached job (strict identity)" );

		session->AttachController( nullptr );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (a) SubmitAgentRenderAsync returns quickly: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (b) Second submit while one runs -> REJECTED with a clear status
//     (single-slot policy, depth-1 only).
//////////////////////////////////////////////////////////////////////
static void RunSingleSlotRejectionTest()
{
	std::printf( "=== AgentRenderAsyncTest: (b) single-slot rejection (second submit while one runs) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_singleslot.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the single-slot scene to a temp file" );

	SlowRasterizeJob* pJob = new SlowRasterizeJob();
	pJob->SetSleepMs( 300 );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowRasterizeJob loads the native-v7 scene via the CST path (single-slot test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the slow Job (single-slot test)" );
	if( session )
	{
		session->AttachController( &controller );

		AgentRenderParams p;
		const AgentSession::AgentRenderAsyncResult first = session->RenderAsync( p );
		Check( first.accepted, "the FIRST submission is accepted" );

		// Give the worker a brief moment to pick up the job (so the
		// second submit lands while it is genuinely running, not just
		// queued-but-not-yet-observed).
		std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );

		const AgentSession::AgentRenderAsyncResult second = session->RenderAsync( p );
		Check( !second.accepted, "RED-PROVE: a SECOND submission while the first is still running is REJECTED (single-slot policy, not a depth-N queue)" );
		Check( second.renderJobId == 0, "a rejected submission does NOT assign a renderJobId" );
		Check( !second.message.empty(), "a rejected submission carries a clear explanatory message" );

		Check( session->RenderWait( first.renderJobId, 5000 ), "the first (accepted) submission eventually completes" );

		// After the first completes, the slot frees up and a fresh
		// submission succeeds.
		const AgentSession::AgentRenderAsyncResult third = session->RenderAsync( p );
		Check( third.accepted, "a THIRD submission after the first completed is accepted (the slot freed up)" );
		Check( third.renderJobId > first.renderJobId, "the third submission's id is fresh and greater than the first's" );
		Check( session->RenderWait( third.renderJobId, 5000 ), "the third submission completes" );

		session->AttachController( nullptr );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (b) single-slot rejection: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (b2) Model-B F2 slice S3 RED-PROVE: pinned-vs-preview.  A PINNED render
//     in flight refuses ANY new submission (async or sync, pinned or not)
//     with the pinned-SPECIFIC message -- distinct from the ordinary
//     single-slot-busy refusal RunSingleSlotRejectionTest above proves.
//     Once the pinned job completes, the slot frees up normally.
//     render_cancel / Stop() still cancel a pinned render (pinned guards
//     against SUPERSESSION, not against an explicit cancel/teardown).
//////////////////////////////////////////////////////////////////////
static void RunPinnedRenderTests()
{
	std::printf( "=== AgentRenderAsyncTest: (b2) Model-B F2 slice S3 pinned-vs-preview ===\n" );

	//------------------------------------------------------------------
	// (1) RED-PROVE the rejection REASON for ASYNC: a pinned occupant
	//     refuses a second ASYNC submission with the pinned-specific
	//     message (not the generic busy one).  For SYNC: Model-B F2 S3
	//     fix round (P2 -- corrects a prior version of this comment,
	//     which claimed a short-timeout SYNC refusal here was a
	//     "pinned-specific refusal that does not queue behind it and
	//     does not run once the pinned job completes" -- that is NOT
	//     what this asserts.  SubmitAgentRenderSync's fairness wait only
	//     ever attempts the inline submit once the slot is ALREADY free,
	//     so the pinned-occupant refusal branch inside
	//     SubmitAgentRenderAsync_Locked is UNREACHABLE from the sync
	//     path -- see SceneEditController.h's SubmitAgentRenderSync doc.
	//     What the short-`timeoutMs` probe below actually proves is an
	//     ordinary FAIRNESS-WAIT TIMEOUT: a sync caller whose `timeoutMs`
	//     is shorter than the pinned render's remaining duration times
	//     out and returns false, exactly as it would against any other
	//     occupant of the slot.  A SECOND probe with a GENEROUS
	//     `timeoutMs` demonstrates the complementary wait-then-succeed
	//     path: the sync caller waits out the pinned render and is
	//     accepted once the slot frees -- pinned guards against silent
	//     supersession, not against fair queuing.  After the pinned job
	//     completes, both async and (already covered) sync submissions
	//     work again.
	//------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_pinned_reject.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the pinned-rejection scene to a temp file" );

		SlowRasterizeJob* pJob = new SlowRasterizeJob();
		pJob->SetSleepMs( 300 );
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowRasterizeJob loads the native-v7 scene via the CST path (pinned-rejection test)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the slow Job (pinned-rejection test)" );
		if( session )
		{
			session->AttachController( &controller );

			AgentRenderParams pinnedParams;
			pinnedParams.pinned = true;
			const AgentSession::AgentRenderAsyncResult pinnedSubmit = session->RenderAsync( pinnedParams );
			Check( pinnedSubmit.accepted, "the PINNED submission is accepted" );
			Check( pinnedSubmit.pinned, "the accepted submission's result echoes pinned=true" );

			// Give the worker a brief moment to pick it up.
			std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );

			const AgentSession::AgentRenderJobStatus midStatus = session->RenderStatus( pinnedSubmit.renderJobId );
			Check( midStatus.found && midStatus.active, "the pinned job is observed active mid-flight" );
			Check( midStatus.pinned, "RenderStatus echoes pinned=true for the in-flight pinned job" );

			// A SECOND async submission (itself NOT pinned) is refused --
			// RED-PROVE the REASON is pinned-specific, not the generic
			// "busy" message a plain preview occupant would give (see
			// RunSingleSlotRejectionTest's "a rejected submission carries
			// a clear explanatory message" -- this asserts something
			// STRONGER: which message).
			AgentRenderParams plainParams;   // pinned=false
			const AgentSession::AgentRenderAsyncResult secondAsync = session->RenderAsync( plainParams );
			Check( !secondAsync.accepted, "a second ASYNC submission while a PINNED render is in flight is refused" );
			Check( secondAsync.message.find( "pinned" ) != std::string::npos,
			       "MONEY ASSERTION: the refusal message is PINNED-SPECIFIC (\"pinned render is in flight\"), distinct from the generic "
			       "busy/transaction message RunSingleSlotRejectionTest observes against an ordinary preview occupant" );

			// A second SYNC submission with a SHORT timeoutMs (well under
			// the pinned render's remaining ~270ms) does not queue in
			// time -- this is an ORDINARY FAIRNESS-WAIT TIMEOUT (P2: NOT
			// a pinned-specific refusal -- SubmitAgentRenderSync never
			// reaches the pinned-occupant check; see that method's doc).
			bool syncTimedOut = false;
			const bool syncCallReturned = RunWatchdogged(
				"SubmitAgentRenderSync (short timeout) while a pinned render is in flight", 5000,
				[&]() {
					SceneEditController::RenderJobId dummyId = 0;
					syncTimedOut = !controller.SubmitAgentRenderSync(
						[](){}, String( "pinned_reject_sync_probe" ), &dummyId, /*timeoutMs=*/200 );
				} );
			Check( syncCallReturned, "the sync probe call itself returned (watchdog did not trip)" );
			Check( syncTimedOut,
			       "a second SYNC submission with a timeoutMs (200ms) shorter than the pinned render's remaining duration times out and "
			       "returns false -- an ordinary fairness-wait timeout, the SAME outcome a non-pinned occupant would produce, not a "
			       "pinned-specific rejection" );

			// P2 red-prove of the COMPLEMENTARY wait-then-succeed path:
			// a second SYNC submission with a GENEROUS timeoutMs (well
			// over the pinned render's remaining duration) WAITS for the
			// pinned occupant to finish and is THEN accepted -- pinned
			// guards against silent supersession, not against a sync
			// caller's fair turn.  Note: acceptance here claims the
			// single slot for THIS new submission, so
			// GetRenderJobStatus/RenderStatus against the ORIGINAL
			// pinnedSubmit.renderJobId can no longer find it (the status
			// record only ever tracks the MOST RECENT job -- see
			// GetRenderJobStatus's `mCurrentRenderJob.id != id` check) --
			// acceptance itself, which can only happen once
			// mAgentRenderPending is false, is the money assertion.
			bool syncAcceptedAfterWait = false;
			SceneEditController::RenderJobId waitedJobId = 0;
			const bool syncWaitReturned = RunWatchdogged(
				"SubmitAgentRenderSync (generous timeout) while a pinned render is in flight", 5000,
				[&]() {
					syncAcceptedAfterWait = controller.SubmitAgentRenderSync(
						[](){}, String( "pinned_wait_then_succeed_sync_probe" ), &waitedJobId, /*timeoutMs=*/5000 );
				} );
			Check( syncWaitReturned, "the generous-timeout sync probe call itself returned (watchdog did not trip)" );
			Check( syncAcceptedAfterWait,
			       "MONEY ASSERTION (P2): a SYNC submission with a GENEROUS timeoutMs WAITS out the pinned render's remaining duration "
			       "and is THEN accepted -- proving the coherent semantic is 'pinned makes a sync caller wait its fair turn', not "
			       "'pinned refuses every sync caller outright'" );

			// session->RenderWait on the ORIGINAL pinned job id still
			// returns true even though it has been superseded in
			// mCurrentRenderJob by the probe above -- WaitForRenderJob
			// (unlike GetRenderJobStatus) treats `mCurrentRenderJob.id !=
			// id` as "done" (a later job can only exist because this one
			// already finished on a single-slot controller).
			Check( session->RenderWait( pinnedSubmit.renderJobId, 5000 ), "the pinned render eventually completes" );

			// After completion, BOTH async and sync submissions work
			// again -- the slot is not permanently poisoned by having
			// hosted a pinned occupant.
			const AgentSession::AgentRenderAsyncResult postAsync = session->RenderAsync( plainParams );
			Check( postAsync.accepted, "a fresh async submission after the pinned job completed is accepted" );
			Check( session->RenderWait( postAsync.renderJobId, 5000 ), "the post-pinned async submission completes" );

			SceneEditController::RenderJobId postSyncId = 0;
			const bool postSyncAccepted = controller.SubmitAgentRenderSync(
				[](){}, String( "pinned_reject_sync_post" ), &postSyncId, /*timeoutMs=*/5000 );
			Check( postSyncAccepted, "a fresh sync submission after the pinned job completed is accepted" );

			session->AttachController( nullptr );
		}

		controller.Stop();
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	// A heavy PT scene (mirrors RunStopCancelsInFlightAgentRenderTest's /
	// RunCancelAsyncRenderRedProveTest's kHeavyCancelScene, declared later
	// in this file -- duplicated inline here rather than forward-
	// referencing it): a real cancel needs actual in-flight rasterizer
	// work with progress-callback checkpoints to interrupt.
	// SlowRasterizeJob's artificial sleep (used in sub-test (1) above) is
	// NOT cancel-checked at all, so it is the wrong tool for (2)/(3)
	// below -- cancelling during the sleep has no effect until the sleep
	// elapses on its own, which would make these tests pass for the WRONG
	// reason (or fail spuriously, as an earlier draft of this test did).
	const std::string heavyScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4096\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 96\n\theight 96\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
		"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
		"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
		"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

	//------------------------------------------------------------------
	// (2) RED-PROVE render_cancel still cancels a PINNED render --
	//     pinned protects against SILENT SUPERSESSION by a later
	//     submission, NOT against an explicit cancel.  Mirrors
	//     RunCancelAsyncRenderRedProveTest's GREEN side, with
	//     pinned=true this time.
	//------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_pinned_cancel.RISEscene", heavyScene );
		Check( !scenePath.empty(), "wrote the pinned-cancel heavy scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the heavy native-v7 scene via the CST path (pinned-cancel test)" );

		SceneEditController* controller = new SceneEditController( *pJob, /*interactiveRasterizer*/0 );
		controller->Start( /*suppressInitialRender=*/true );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the heavy Job (pinned-cancel test)" );
		if( session )
		{
			session->AttachController( controller );

			AgentRenderParams pinnedParams;
			pinnedParams.pinned = true;
			const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( pinnedParams );
			Check( ar.accepted, "the pinned async render is accepted (pinned-cancel test)" );

			std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );

			const AgentSession::AgentRenderJobStatus preCancel = session->RenderStatus( ar.renderJobId );
			Check( preCancel.found && preCancel.pinned, "the in-flight job is confirmed pinned before cancelling" );

			session->CancelAsyncRender( ar.renderJobId );

			const bool completedAfterCancel = RunWatchdogged(
				"render_wait(150ms) after CancelAsyncRender on a PINNED render", 5000,
				[&]() {
					Check( session->RenderWait( ar.renderJobId, 150 ),
					       "MONEY ASSERTION: CancelAsyncRender aborts a PINNED render promptly (well within the heavy scene's multi-second "
					       "natural duration, matching RunCancelAsyncRenderRedProveTest's GREEN-side bound) -- pinned guards against "
					       "supersession, NOT against an explicit cancel" );
				} );
			Check( completedAfterCancel, "the pinned-cancel wait+check itself returned (watchdog did not trip)" );

			session->AttachController( nullptr );
		}

		delete controller;
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	//------------------------------------------------------------------
	// (3) RED-PROVE Stop() still tears down a PINNED render promptly --
	//     mirrors RunStopCancelsInFlightAgentRenderTest, with pinned=true.
	//------------------------------------------------------------------
	{
		const std::string scenePath = WriteTemp( "rise_agent_pinned_stop.RISEscene", heavyScene );
		Check( !scenePath.empty(), "wrote the pinned-stop heavy scene to a temp file" );

		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the heavy native-v7 scene via the CST path (pinned-stop test)" );

		SceneEditController* controller = new SceneEditController( *pJob, /*interactiveRasterizer*/0 );
		controller->Start( /*suppressInitialRender=*/true );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the heavy Job (pinned-stop test)" );
		if( session )
		{
			session->AttachController( controller );

			AgentRenderParams pinnedParams;
			pinnedParams.pinned = true;
			const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( pinnedParams );
			Check( ar.accepted, "the pinned async render is accepted (pinned-stop test)" );

			std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );

			const auto t0 = std::chrono::steady_clock::now();
			const bool stoppedPromptly = RunWatchdogged(
				"Stop() during a PINNED render", 5000,
				[&]() { controller->Stop(); } );
			const auto t1 = std::chrono::steady_clock::now();
			const long long stopMs = std::chrono::duration_cast<std::chrono::milliseconds>( t1 - t0 ).count();
			Check( stoppedPromptly, "Stop() during a PINNED render returns within the watchdog bound" );
			// Same discriminating bound as RunStopCancelsInFlightAgentRenderTest
			// (well under this scene's multi-second natural duration, well
			// above the fixed behaviour's measured ~tens of ms).
			Check( stopMs < 800,
			       "MONEY ASSERTION: Stop() during an in-flight PINNED render tears down well under the heavy scene's natural duration -- "
			       "pinned protects against supersession by a NEW submission, not against teardown" );

			session->AttachController( nullptr );
		}

		delete controller;
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	std::printf( "=== (b2) pinned-vs-preview: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (c) RED-PROVE the race closure: a REAL concurrent interactive render
//     thread + a REAL agent Rasterize() call must never observe more
//     than 1 thread inside the shared "render critical section" at once.
//     Exercises BOTH the override path (RunPreviewRenderParked) and the
//     no-override path (SubmitAgentRenderSync, via the synchronous
//     Render() call) -- the no-override path is the one that used to be
//     completely unparked before this slice.
//////////////////////////////////////////////////////////////////////
static void RunConcurrencyRaceClosureTest()
{
	std::printf( "=== AgentRenderAsyncTest: (c) RED-PROVE no-override race closure (max observed concurrency == 1) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_race.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the race-closure scene to a temp file" );

	ConcurrencyProof proof;
	ConcurrencyProofJob* pJob = new ConcurrencyProofJob( proof );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ConcurrencyProofJob loads the native-v7 scene via the CST path" );

	ConcurrencyTestController controller( *pJob, proof, /*simulatedRenderMs*/15 );
	controller.Start();   // initial pass NOT suppressed -- this test needs a genuinely live interactive loop to race against

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the concurrency-proof Job" );
	if( session )
	{
		session->AttachController( &controller );

		// Fire a burst of NO-OVERRIDE synchronous Render() calls from a
		// background thread while a SECOND background thread keeps the
		// interactive loop genuinely busy via OnTimeScrub (a public,
		// pointer-free way to repeatedly KickRender() -- each call sets
		// mEditPending and wakes RenderLoop for another pass).  This is
		// the exact call shape (controller attached, no override) that
		// pre-S2a called mJob->Rasterize() DIRECTLY with no park.
		std::atomic<bool> stop{ false };
		std::thread agentThread( [&]() {
			while( !stop.load( std::memory_order_acquire ) )
			{
				AgentRenderParams p;   // no override
				session->Render( p );
			}
		} );
		std::thread scrubThread( [&]() {
			double t = 0.0;
			while( !stop.load( std::memory_order_acquire ) )
			{
				controller.OnTimeScrub( t );
				t += 0.01;
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
		} );

		// Let the race window run for a bit -- long enough for many
		// interleavings of interactive passes and agent renders.
		std::this_thread::sleep_for( std::chrono::milliseconds( 600 ) );
		stop.store( true, std::memory_order_release );
		agentThread.join();
		scrubThread.join();

		session->AttachController( nullptr );
	}

	controller.Stop();

	const int maxConcurrency = proof.maxObserved.load( std::memory_order_acquire );
	std::printf( "  [race-closure] max observed concurrency inside the shared render critical section: %d\n", maxConcurrency );
	Check( maxConcurrency == 1,
	       "RACE-CLOSURE MONEY ASSERTION: max observed concurrency between the interactive render loop and a no-override agent render is EXACTLY 1 -- "
	       "never simultaneous (S2a routes the no-override path through SubmitAgentRenderSync's cancel-and-park, closing the pre-existing direct-call race). "
	       "This assertion would FAIL (observe 2) against the pre-S2a code path, which called mJob->Rasterize() directly with no park for the no-override case." );

	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (c) race closure: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (c continued) Sync Render() still works identically for OVERRIDE and
// NO-OVERRIDE paths -- both succeed, produce images, and (no-override)
// now yield EVEN coordinator ids (the S2a rebase; see
// AgentProposeRenderTest.cpp's ITEM 2a for the detailed id-parity
// coverage -- this is a lighter smoke check scoped to THIS file's
// concurrency-proof Job).
//////////////////////////////////////////////////////////////////////
static void RunSyncStillWorksTest()
{
	std::printf( "=== AgentRenderAsyncTest: (c) sync Render() still works for override AND no-override ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_syncstillworks.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the sync-still-works scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (sync-still-works test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the Job (sync-still-works test)" );
	if( session )
	{
		session->AttachController( &controller );

		AgentRenderParams noOverride;
		const AgentRenderResult r1 = session->Render( noOverride );
		Check( r1.ok, "controller-attached, no-override sync Render() still succeeds" );
		Check( !r1.png.empty(), "no-override sync Render() produces PNG bytes" );
		Check( ( r1.renderJobId % 2 ) == 0, "no-override sync Render() now yields an EVEN (coordinator) id (S2a rebase)" );

		AgentRenderParams withOverride;
		withOverride.camera.hasLocation = true;  withOverride.camera.location = "3.5 0 0";
		withOverride.camera.hasLookAt   = true;  withOverride.camera.lookAt   = "0 0 0";
		const AgentRenderResult r2 = session->Render( withOverride );
		Check( r2.ok, "controller-attached, WITH-override sync Render() still succeeds" );
		Check( !r2.png.empty(), "with-override sync Render() produces PNG bytes" );
		Check( ( r2.renderJobId % 2 ) == 0, "with-override sync Render() yields an EVEN (coordinator) id" );
		Check( r2.renderJobId > r1.renderJobId, "ids keep increasing across calls" );

		// Headless (no controller) still works too.
		session->AttachController( nullptr );
		const AgentRenderResult r3 = session->Render( noOverride );
		Check( r3.ok, "headless (no controller) sync Render() still succeeds" );
		Check( ( r3.renderJobId % 2 ) == 1, "headless sync Render() yields an ODD (session-local) id" );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (c) sync still works: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (d) render_status/render_wait wire round-trip, including ODD-id
//     rejection -- via AgentSession's RenderStatus/RenderWait (the same
//     calls AgentRpc.cpp's render_status/render_wait verbs make).
//////////////////////////////////////////////////////////////////////
static void RunStatusWaitOddIdRejectionTest()
{
	std::printf( "=== AgentRenderAsyncTest: (d) render_status/render_wait ODD-id rejection ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_oddid.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the odd-id scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (odd-id test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the Job (odd-id test)" );
	if( session )
	{
		// Headless: get a session-local (ODD) id.
		session->AttachController( nullptr );
		const AgentRenderResult headlessResult = session->Render( -1 );
		Check( headlessResult.ok, "headless render succeeds (odd-id test)" );
		Check( ( headlessResult.renderJobId % 2 ) == 1, "headless render yields an ODD id" );

		// Now attach the controller and query status/wait for that ODD id
		// -- must be rejected outright (not found), per the parity
		// contract carried from ce9f5e03: the controller never minted
		// this id, so it must not alias it onto whatever job it currently
		// knows about.
		session->AttachController( &controller );

		const AgentSession::AgentRenderJobStatus oddStatus = session->RenderStatus( headlessResult.renderJobId );
		Check( !oddStatus.found, "RenderStatus REJECTS an ODD (session-local) id outright -- not found, not aliased" );

		const bool oddWait = session->RenderWait( headlessResult.renderJobId, 100 );
		Check( !oddWait, "RenderWait REJECTS an ODD (session-local) id outright (returns false immediately, not a timeout)" );

		// A genuinely unknown EVEN id (never minted by this controller)
		// is likewise not found.
		const AgentSession::AgentRenderJobStatus unknownStatus = session->RenderStatus( 999998 );
		Check( !unknownStatus.found, "RenderStatus reports not-found for an EVEN id this controller never minted" );

		// Zero (the reserved invalid id) is also not found.
		const AgentSession::AgentRenderJobStatus zeroStatus = session->RenderStatus( 0 );
		Check( !zeroStatus.found, "RenderStatus reports not-found for renderJobId==0 (the reserved invalid id)" );

		// A REAL coordinator-tracked id round-trips correctly: submit an
		// async render, then use RenderStatus/RenderWait on its id.
		AgentRenderParams p;
		const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
		Check( ar.accepted, "async render accepted for the round-trip check" );
		Check( ( ar.renderJobId % 2 ) == 0, "the async render's id is EVEN (coordinator)" );
		const AgentSession::AgentRenderJobStatus liveStatus = session->RenderStatus( ar.renderJobId );
		Check( liveStatus.found, "RenderStatus finds a REAL coordinator-tracked id" );
		Check( session->RenderWait( ar.renderJobId, 5000 ), "RenderWait completes for a REAL coordinator-tracked id" );

		session->AttachController( nullptr );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (d) status/wait odd-id rejection: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (e) Throw path: a thrown render -> ok=false result WITH renderJobId
//     (the S1-delta fix), the worker survives, and the next render
//     works.  Exercises the ASYNC path specifically (the sync/headless
//     throw shapes are covered by AgentProposeRenderTest.cpp's updated
//     RunRestoreOnThrowTest / RunPreS2HardeningTests).
//////////////////////////////////////////////////////////////////////
static void RunAsyncThrowSurvivesTest()
{
	std::printf( "=== AgentRenderAsyncTest: (e) async throw path -- worker survives, next render works ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_throw.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the async-throw scene to a temp file" );

	ThrowingRasterizeJob* pJob = new ThrowingRasterizeJob();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ThrowingRasterizeJob loads the native-v7 scene via the CST path (async-throw test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the throwing Job (async-throw test)" );
	if( session )
	{
		session->AttachController( &controller );

		AgentRenderParams p;   // no override -- exercises SubmitAgentRenderAsync's assumeParked path
		const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
		Check( ar.accepted, "the throwing render is ACCEPTED for submission (the throw happens once it actually runs)" );

		Check( session->RenderWait( ar.renderJobId, 5000 ), "RenderWait observes the throwing job complete (the worker caught the exception, did not hang)" );

		const AgentSession::AgentRenderJobStatus after = session->RenderStatus( ar.renderJobId );
		Check( after.found && !after.active, "the job is inactive after the throw -- the RAII guard flipped it false even though fn() threw" );

		// THE WORKER-SURVIVES ASSERTION: stop throwing, then submit a
		// SECOND async render through the SAME controller/session/worker
		// and confirm it completes normally -- proves the worker thread
		// (not just the process) is still alive and usable after
		// swallowing the previous submission's exception.
		pJob->SetThrowOnRasterize( false );
		const AgentSession::AgentRenderAsyncResult clean = session->RenderAsync( p );
		Check( clean.accepted, "a follow-up non-throwing async submission is accepted after the earlier throw (the worker is still alive)" );
		Check( clean.renderJobId > ar.renderJobId, "the follow-up submission gets a fresh id (the counter kept advancing through the throw)" );
		Check( session->RenderWait( clean.renderJobId, 5000 ), "the follow-up non-throwing render completes" );

		const std::vector<unsigned char> png = session->ReadImage();
		Check( !png.empty(), "ReadImage() reflects the follow-up SUCCESSFUL render's bytes (the earlier failed render did not poison the cache)" );

		session->AttachController( nullptr );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (e) async throw survives: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (f) First controller id is 2 (the S1-delta fix: the counter used to
//     start AT the stride and pre-increment past it, so the first
//     minted id was 4, not the documented 2).
//////////////////////////////////////////////////////////////////////
static void RunFirstControllerIdIsTwoTest()
{
	std::printf( "=== AgentRenderAsyncTest: (f) first controller-minted renderJobId is 2 (S1-delta fix) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_firstid.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the first-id scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (first-id test)" );

	// A FRESH controller -- its render-job counter has never minted
	// anything yet.  Do NOT call Start() (which would fire the
	// interactive loop's own initial render and consume id 2 on THAT
	// pass instead) -- submit an agent render FIRST via the dedicated
	// worker, which is spawned in the CONSTRUCTOR and independent of
	// Start()/Stop().
	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the Job (first-id test)" );
	if( session )
	{
		session->AttachController( &controller );

		AgentRenderParams p;
		const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
		Check( ar.accepted, "the very first submission on a fresh controller is accepted" );
		Check( ar.renderJobId == 2,
		       "S1-DELTA FIX MONEY ASSERTION: the FIRST id this controller ever mints is EXACTLY 2 (matches kControllerRenderJobIdStride's documented 'starts at 2' contract) -- "
		       "this would read 4 against the pre-fix counter init (mNextRenderJobId(kControllerRenderJobIdStride) then += stride on first mint)." );

		Check( session->RenderWait( ar.renderJobId, 5000 ), "the first submission completes" );

		session->AttachController( nullptr );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (f) first controller id is 2: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (g) Fix-round-1 P1-1: submit-after-Stop() is refused HONESTLY and
//     promptly -- no hang, no poisoned slot.  Also: a sync waiter
//     already QUEUED (fairness ticket claimed) when Stop() lands must
//     unblock with a refusal rather than wait out its full timeout.
//////////////////////////////////////////////////////////////////////
static void RunPostStopRefusalTest()
{
	std::printf( "=== AgentRenderAsyncTest: (g) P1-1 RED-PROVE: post-Stop() submission is refused, not hung ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_poststop.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the post-stop scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (post-stop test)" );

	SceneEditController* controller = new SceneEditController( *pJob, /*interactiveRasterizer*/0 );
	controller->Start( /*suppressInitialRender=*/true );
	controller->Stop();   // interactive loop AND the agent-render worker are both torn down now

	SceneEditController::RenderJobId dummyId = SceneEditController::kInvalidRenderJobId;
	const bool asyncAccepted = controller->SubmitAgentRenderAsync(
		[]() { /* must never run */ }, String( "post_stop_async" ), &dummyId );
	Check( !asyncAccepted, "RED-PROVE P1-1: SubmitAgentRenderAsync after Stop() is REFUSED (pre-fix: accepted into an orphaned slot)" );
	Check( dummyId == SceneEditController::kInvalidRenderJobId, "a post-Stop() refusal does not assign a renderJobId" );

	// The money assertion: SubmitAgentRenderSync after Stop() must return
	// (refused) WELL under its own timeoutMs, not hang for the worker to
	// service a submission no worker will ever pick up.  Watchdogged: a
	// regression here is a HANG, not merely a wrong return value.
	bool syncResult = true;   // sentinel -- overwritten inside the watchdog if it completes
	const bool completedInTime = RunWatchdogged(
		"SubmitAgentRenderSync after Stop() returns (refused)", 3000,
		[&]() {
			SceneEditController::RenderJobId syncId = SceneEditController::kInvalidRenderJobId;
			syncResult = controller->SubmitAgentRenderSync(
				[]() { /* must never run */ }, String( "post_stop_sync" ), &syncId, /*timeoutMs*/ 60000 );
			Check( syncId == SceneEditController::kInvalidRenderJobId, "a post-Stop() SYNC refusal does not assign a renderJobId either" );
		} );
	Check( completedInTime, "RED-PROVE P1-1 (hang class): SubmitAgentRenderSync after Stop() returns promptly instead of waiting out its 60s timeout" );
	if( completedInTime ) {
		Check( !syncResult, "SubmitAgentRenderSync after Stop() reports refused (false)" );
	}

	// Slot must not be poisoned: a Status/Wait call on ANY id must still
	// behave sanely (not found, not hung) after all this.
	const SceneEditController::RenderJobLookup lookup = controller->GetRenderJobStatus( 2 );
	Check( !lookup.found || !lookup.status.active, "GetRenderJobStatus after Stop() never reports a permanently-active phantom job" );

	delete controller;   // dtor calls Stop() again -- must be a harmless no-op
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (g) post-Stop() refusal: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (h) Fix-round-1 P1-1 continued: a SYNC waiter already queued (holding
//     a fairness ticket, blocked waiting for its turn) when Stop() is
//     called must unblock PROMPTLY with an honest refusal -- not hang
//     for its full fairness-wait timeout.
//////////////////////////////////////////////////////////////////////
static void RunQueuedSyncWaiterUnblocksOnStopTest()
{
	std::printf( "=== AgentRenderAsyncTest: (h) P1-1 RED-PROVE: a sync waiter queued at Stop()-time unblocks honestly ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_queuedstop.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the queued-stop scene to a temp file" );

	SlowRasterizeJob* pJob = new SlowRasterizeJob();
	pJob->SetSleepMs( 400 );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowRasterizeJob loads the native-v7 scene via the CST path (queued-stop test)" );

	SceneEditController* controller = new SceneEditController( *pJob, /*interactiveRasterizer*/0 );
	controller->Start( /*suppressInitialRender=*/true );

	// Occupy the slot with a slow render so a SECOND sync caller must
	// genuinely queue (claim a fairness ticket and block) rather than
	// submit immediately.
	SceneEditController::RenderJobId occupyId = SceneEditController::kInvalidRenderJobId;
	const bool occupied = controller->SubmitAgentRenderAsync(
		[&]() { std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) ); },
		String( "occupy" ), &occupyId );
	Check( occupied, "the occupying submission is accepted" );

	std::atomic<bool> waiterReturned{ false };
	std::atomic<bool> waiterResult{ true };
	std::thread queuedWaiter( [&]() {
		SceneEditController::RenderJobId waiterId = SceneEditController::kInvalidRenderJobId;
		// Long timeoutMs -- if Stop()'s wake-up fix regresses, this thread
		// would otherwise sit here for the FULL 60s before the outer
		// watchdog below even gets a chance to fail loud on IT.
		waiterResult.store(
			controller->SubmitAgentRenderSync( []() {}, String( "queued_waiter" ), &waiterId, /*timeoutMs*/ 60000 ),
			std::memory_order_release );
		waiterReturned.store( true, std::memory_order_release );
	} );

	// Give the waiter thread a moment to actually claim its ticket and
	// start waiting before we pull the rug out with Stop().
	std::this_thread::sleep_for( std::chrono::milliseconds( 60 ) );

	const bool stopReturnedPromptly = RunWatchdogged(
		"Stop() with a queued sync waiter", 3000,
		[&]() { controller->Stop(); } );
	Check( stopReturnedPromptly, "Stop() itself returns promptly even with a sync waiter queued behind the occupying render" );

	// Now confirm the QUEUED WAITER thread also unblocks promptly (it may
	// return slightly after Stop() itself, since it still has to observe
	// the notify and re-check its predicate) -- watchdog this too since a
	// regression here is exactly the "queued sync waiter left hanging"
	// bug this test exists to catch.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 3000 );
	bool waiterUnblocked = false;
	while( std::chrono::steady_clock::now() < deadline ) {
		if( waiterReturned.load( std::memory_order_acquire ) ) { waiterUnblocked = true; break; }
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
	Check( waiterUnblocked, "RED-PROVE P1-1: a SYNC waiter queued when Stop() lands unblocks within 3s (pre-fix: would wait out its full 60s timeoutMs, or hang forever if notified on the wrong lock)" );
	if( waiterUnblocked ) {
		Check( !waiterResult.load( std::memory_order_acquire ), "the queued waiter's eventual result is an honest refusal (false), not a stray success" );
		queuedWaiter.join();
	} else {
		queuedWaiter.detach();   // avoid blocking process exit on a genuinely stuck thread
	}

	delete controller;
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (h) queued sync waiter unblocks on Stop(): %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (i) Fix-round-1 P2-C RED-PROVE: Stop() during a SLOW agent render
//     returns promptly (cancellation actually aborts the render) rather
//     than blocking for the render's full natural duration.
//////////////////////////////////////////////////////////////////////
static void RunStopCancelsInFlightAgentRenderTest()
{
	std::printf( "=== AgentRenderAsyncTest: (i) P2-C RED-PROVE: Stop() cancels an in-flight agent render ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_stopcancel.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the stop-cancels scene to a temp file" );

	// A LARGE sample count so the render has plenty of progress-callback
	// checkpoints to actually observe a mid-flight cancel at -- a tiny
	// sample count could finish before Stop() even has a chance to act,
	// which would make this test pass for the WRONG reason (the render
	// finished naturally, not because it was cancelled).
	const std::string slowScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4096\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 96\n\theight 96\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
		"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
		"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
		"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";
	const std::string slowScenePath = WriteTemp( "rise_agent_async_stopcancel_scene.RISEscene", slowScene );
	Check( !slowScenePath.empty(), "wrote the heavy scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( slowScenePath.c_str() ), "Job loads the heavy native-v7 scene via the CST path (stop-cancels test)" );

	SceneEditController* controller = new SceneEditController( *pJob, /*interactiveRasterizer*/0 );
	controller->Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the heavy Job" );
	if( session )
	{
		session->AttachController( controller );

		AgentRenderParams p;
		const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
		Check( ar.accepted, "the heavy async render is accepted" );

		// Let it get properly under way before we pull the rug out.
		std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );

		const auto t0 = std::chrono::steady_clock::now();
		const bool stoppedPromptly = RunWatchdogged(
			"Stop() during a slow agent render", 5000,
			[&]() { controller->Stop(); } );
		const auto t1 = std::chrono::steady_clock::now();
		const long long stopMs = std::chrono::duration_cast<std::chrono::milliseconds>( t1 - t0 ).count();

		Check( stoppedPromptly, "RED-PROVE P2-C: Stop() during a slow (4096spp/96x96) agent render returns within the watchdog bound" );
		std::printf( "  [stop-cancels] Stop() wall time during the heavy render: %lldms\n", stopMs );
		// Tight bound, deliberately much smaller than the watchdog above.
		// Measured calibration on this machine: WITH the fix, Stop() lands
		// at ~17ms (cancelled after pass 3/128); WITHOUT it (this test was
		// run against a deliberately-disabled progress-hook install to
		// confirm the RED side), Stop() blocks for the render's full
		// natural duration -- ~2210ms (all 128 passes complete) -- because
		// the render finishes on its own before Stop()'s join ever notices
		// anything was cancelled.  800ms sits with a wide safety margin
		// above the fixed behaviour and a wide safety margin below the
		// broken behaviour, so this assertion actually DISCRIMINATES
		// "cancelled early" from "happened to finish quickly" rather than
		// just checking against the outer watchdog's generous 5s ceiling
		// (a bound that loose passed even with the fix disabled on this
		// machine, since the render's natural duration was itself < 5s).
		Check( stopMs < 800,
		       "RED-PROVE P2-C MONEY ASSERTION: Stop() completes in well under the render's natural duration (~2200ms measured) -- "
		       "the cancel signal actually reaches Rasterize() via the installed progress hook (pre-fix / hook-disabled: Stop() blocks "
		       "for the render's FULL natural duration because nothing downstream ever consults the tripped flag)." );

		session->AttachController( nullptr );
	}

	delete controller;
	pJob->release();
	std::remove( scenePath.c_str() );
	std::remove( slowScenePath.c_str() );

	std::printf( "=== (i) Stop() cancels in-flight agent render: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (i2) Model-B F2 slice S2b RED-PROVE: AgentSession::CancelAsyncRender /
//     the render_cancel RPC verb actually abort an in-flight async render
//     promptly, WITHOUT the caller blocking for the render's duration.
//
//     RED side (proves the assertion actually discriminates rather than
//     passing by construction): a SHORT render_wait timeout on an
//     UNCANCELLED heavy render times out (completed=false, active=true)
//     -- establishing that this scene/sample-count genuinely outlives the
//     short timeout on its own.  GREEN side: the SAME short timeout,
//     preceded by a render_cancel, observes completion (completed=true)
//     well within that same short window -- the cancel signal reached
//     Rasterize() via the shared progress hook, exactly like Stop()'s
//     P2-C fix (RunStopCancelsInFlightAgentRenderTest above).
//////////////////////////////////////////////////////////////////////
static const char* const kHeavyCancelScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4096\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 96\n\theight 96\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

static void RunCancelAsyncRenderRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (i2) S2b RED-PROVE: CancelAsyncRender / render_cancel abort an in-flight render promptly ===\n" );

	const std::string slowScenePath = WriteTemp( "rise_agent_async_cancelverb.RISEscene", kHeavyCancelScene );
	Check( !slowScenePath.empty(), "wrote the heavy render_cancel scene to a temp file" );

	// --- RED side: no cancel, short render_wait times out ------------------
	{
		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( slowScenePath.c_str() ),
		       "Job loads the heavy scene via the CST path (RED side)" );

		SceneEditController* controller = new SceneEditController( *pJob, /*interactiveRasterizer*/0 );
		controller->Start( /*suppressInitialRender=*/true );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the heavy Job (RED side)" );
		if( session )
		{
			session->AttachController( controller );
			AgentRenderParams p;
			const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
			Check( ar.accepted, "RED side: the heavy async render is accepted" );

			std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );   // let it get under way

			// A SHORT wait with NO cancel: this scene's natural duration
			// (~2200ms measured in RunStopCancelsInFlightAgentRenderTest)
			// vastly outlives a 150ms window, so this must time out --
			// establishing the RED baseline this test's GREEN side then
			// contrasts against.
			const bool completedNoCancel = RunWatchdogged(
				"render_wait(150ms) with no cancel (RED side)", 5000,
				[&]() {
					Check( !session->RenderWait( ar.renderJobId, 150 ),
					       "RED-PROVE baseline: an UNCANCELLED heavy render does NOT complete within a 150ms wait "
					       "(establishes that this scene genuinely outlives the short window on its own -- "
					       "so the GREEN side's completion within the SAME window is due to the cancel, not chance)" );
				} );
			Check( completedNoCancel, "the RED-side wait+check itself returned (watchdog did not trip)" );

			// Now actually stop the render before tearing down (avoid
			// leaking a runaway render thread into the next sub-test).
			session->CancelAsyncRender( ar.renderJobId );
			Check( session->RenderWait( ar.renderJobId, 5000 ), "RED side: the render eventually completes once genuinely cancelled for teardown" );
			session->AttachController( nullptr );
		}
		delete controller;
		pJob->release();
	}

	// --- GREEN side: CancelAsyncRender (direct C++ call) ---------------
	{
		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( slowScenePath.c_str() ),
		       "Job loads the heavy scene via the CST path (GREEN side, direct call)" );

		SceneEditController* controller = new SceneEditController( *pJob, /*interactiveRasterizer*/0 );
		controller->Start( /*suppressInitialRender=*/true );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the heavy Job (GREEN side, direct call)" );
		if( session )
		{
			session->AttachController( controller );
			AgentRenderParams p;
			const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
			Check( ar.accepted, "GREEN side (direct): the heavy async render is accepted" );

			std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );

			// CancelAsyncRender does NOT block -- prove that itself first.
			const auto tCancel0 = std::chrono::steady_clock::now();
			session->CancelAsyncRender( ar.renderJobId );
			const auto tCancel1 = std::chrono::steady_clock::now();
			const long long cancelCallMs = std::chrono::duration_cast<std::chrono::milliseconds>( tCancel1 - tCancel0 ).count();
			Check( cancelCallMs < 50, "CancelAsyncRender itself returns near-instantly (does not block for the render's duration)" );

			// A short render_wait now observes prompt completion -- the
			// SAME 150ms window the RED side proved times out without a
			// cancel.
			const bool completedAfterCancel = RunWatchdogged(
				"render_wait(150ms) after CancelAsyncRender (GREEN side)", 5000,
				[&]() {
					Check( session->RenderWait( ar.renderJobId, 150 ),
					       "MONEY ASSERTION: after CancelAsyncRender, render_wait(150ms) observes completion within the SAME short "
					       "window the RED-side baseline proved an uncancelled render blows through -- the cancel signal actually "
					       "reached Rasterize() via the shared progress hook." );
				} );
			Check( completedAfterCancel, "the GREEN-side (direct) wait+check itself returned (watchdog did not trip)" );

			const AgentSession::AgentRenderJobStatus st = session->RenderStatus( ar.renderJobId );
			Check( st.found && !st.active, "GREEN side (direct): RenderStatus confirms the job is no longer active after the cancelled completion" );

			session->AttachController( nullptr );
		}
		delete controller;
		pJob->release();
	}

	// --- GREEN side: render_cancel RPC verb (through HandleLine) -------
	{
		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( slowScenePath.c_str() ),
		       "Job loads the heavy scene via the CST path (GREEN side, RPC verb)" );

		SceneEditController* controller = new SceneEditController( *pJob, /*interactiveRasterizer*/0 );
		controller->Start( /*suppressInitialRender=*/true );

		std::unique_ptr<AgentSession> ownedSession = AgentSession::WrapJob( pJob );
		Check( ownedSession != nullptr, "AgentSession::WrapJob wraps the heavy Job (GREEN side, RPC verb)" );
		if( ownedSession )
		{
			ownedSession->AttachController( controller );
			AgentSession* session = ownedSession.get();
			AgentRpcDispatcher rpc( std::move( ownedSession ) );

			// render {"async":true} through the wire, exactly like the
			// Swift chat driver's submit call.
			JsonValue renderParams = JsonValue::MakeObject();
			renderParams.set( "async", JsonValue::MakeBool( true ) );
			JsonValue submitReq = JsonValue::MakeObject();
			submitReq.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
			submitReq.set( "id", JsonValue::MakeNumber( 1.0 ) );
			submitReq.set( "method", JsonValue::MakeString( "render" ) );
			submitReq.set( "params", renderParams );
			const std::string submitResp = rpc.HandleLine( JsonSerialize( submitReq ) );
			JsonValue submitEnv; std::string submitErr;
			Check( JsonParse( submitResp, submitEnv, submitErr ), "render{async:true} response parses as JSON" );
			const double jobId = submitEnv.get( "result" ).get( "renderJobId" ).asNumber( 0.0 );
			Check( jobId > 0.0 && ( static_cast<std::uint64_t>( jobId ) % 2 ) == 0,
			       "render{async:true} over the wire assigns a nonzero, EVEN (coordinator-tracked) renderJobId" );

			std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );

			// render_cancel {renderJobId} over the wire.
			JsonValue cancelParams = JsonValue::MakeObject();
			cancelParams.set( "renderJobId", JsonValue::MakeNumber( jobId ) );
			JsonValue cancelReq = JsonValue::MakeObject();
			cancelReq.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
			cancelReq.set( "id", JsonValue::MakeNumber( 2.0 ) );
			cancelReq.set( "method", JsonValue::MakeString( "render_cancel" ) );
			cancelReq.set( "params", cancelParams );
			const std::string cancelResp = rpc.HandleLine( JsonSerialize( cancelReq ) );
			JsonValue cancelEnv; std::string cancelErr;
			Check( JsonParse( cancelResp, cancelEnv, cancelErr ), "render_cancel response parses as JSON" );
			Check( cancelEnv.get( "result" ).get( "cancelled" ).asBool(),
			       "render_cancel reports cancelled=true (a live controller was attached to route it through)" );

			// render_wait {renderJobId, timeoutMs:150} over the wire --
			// the SAME short window the RED side (direct-API) proved an
			// uncancelled render blows through.
			JsonValue waitEnv;
			const bool waitCompletedAfterCancel = RunWatchdogged(
				"render_wait{timeoutMs:150} after render_cancel over the wire (GREEN side, RPC verb)", 5000,
				[&]() {
					JsonValue waitParams = JsonValue::MakeObject();
					waitParams.set( "renderJobId", JsonValue::MakeNumber( jobId ) );
					waitParams.set( "timeoutMs", JsonValue::MakeNumber( 150.0 ) );
					JsonValue waitReq = JsonValue::MakeObject();
					waitReq.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
					waitReq.set( "id", JsonValue::MakeNumber( 3.0 ) );
					waitReq.set( "method", JsonValue::MakeString( "render_wait" ) );
					waitReq.set( "params", waitParams );
					const std::string waitResp = rpc.HandleLine( JsonSerialize( waitReq ) );
					std::string waitErr;
					Check( JsonParse( waitResp, waitEnv, waitErr ), "render_wait response parses as JSON" );
					Check( waitEnv.get( "result" ).get( "completed" ).asBool(),
					       "MONEY ASSERTION (RPC verb): render_wait{timeoutMs:150} over the wire observes completion "
					       "after render_cancel, within the same short window the direct-API RED side proved times out uncancelled." );
				} );
			Check( waitCompletedAfterCancel, "the GREEN-side (RPC verb) wait+check itself returned (watchdog did not trip)" );

			// Model-B F2 slice S2b: render_wait's post-completion 'result'
			// echo -- since THIS renderJobId was submitted via
			// render{"async":true} on THIS session and has now completed,
			// render_wait must carry the full synchronous-shaped stats so
			// the chat driver can deliver an IDENTICAL tool-result contract
			// to the LLM regardless of the async detour.  The render was
			// cancelled mid-flight, so `ok` is expected false (see
			// RenderCore_'s wasCancelled branch: "render cancelled"), but
			// the shape itself -- and `integrator` in particular, which is
			// populated even on a cancelled render -- must still be present.
			const JsonValue& waitResult = waitEnv.get( "result" ).get( "result" );
			Check( waitResult.isObject(), "render_wait's completion echo carries a nested 'result' object with the synchronous render shape" );
			Check( !waitResult.get( "ok" ).asBool(), "the cancelled render's echoed result reports ok=false (it did not complete a full pass)" );
			Check( waitResult.get( "integrator" ).asString() == "pathtracing_pel_rasterizer",
			       "the echoed result's 'integrator' names the active rasterizer even for a cancelled render" );
			Check( waitResult.get( "renderJobId" ).asNumber( -1.0 ) == jobId,
			       "the echoed result's renderJobId matches the polled job (strict identity, not just 'some' cached result)" );

			// Headless-equivalent honesty check: render_cancel with no
			// outstanding job (nothing to cancel) reports cancelled=true
			// (a controller IS attached) but found=false for a stale id --
			// distinct from "errored".  Use a fresh bogus id.
			JsonValue noopParams = JsonValue::MakeObject();
			noopParams.set( "renderJobId", JsonValue::MakeNumber( 999998.0 ) );
			JsonValue noopReq = JsonValue::MakeObject();
			noopReq.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
			noopReq.set( "id", JsonValue::MakeNumber( 4.0 ) );
			noopReq.set( "method", JsonValue::MakeString( "render_cancel" ) );
			noopReq.set( "params", noopParams );
			const std::string noopResp = rpc.HandleLine( JsonSerialize( noopReq ) );
			JsonValue noopEnv; std::string noopErr;
			Check( JsonParse( noopResp, noopEnv, noopErr ), "render_cancel (no outstanding job) response parses as JSON" );
			Check( !noopEnv.get( "result" ).get( "found" ).asBool(),
			       "render_cancel against a stale/unrecognized renderJobId reports found=false (not an RPC error)" );

			// render_cancel with NO renderJobId param at all (the fully
			// optional shape the Swift driver's cancelTurn path uses,
			// since it does not necessarily know the id) -- must not
			// error, and still routes the cancel through (cancelled=true).
			JsonValue bareReq = JsonValue::MakeObject();
			bareReq.set( "jsonrpc", JsonValue::MakeString( "2.0" ) );
			bareReq.set( "id", JsonValue::MakeNumber( 5.0 ) );
			bareReq.set( "method", JsonValue::MakeString( "render_cancel" ) );
			bareReq.set( "params", JsonValue::MakeObject() );
			const std::string bareResp = rpc.HandleLine( JsonSerialize( bareReq ) );
			JsonValue bareEnv; std::string bareErr;
			Check( JsonParse( bareResp, bareEnv, bareErr ), "render_cancel with no renderJobId param parses as JSON" );
			Check( bareEnv.get( "result" ).get( "cancelled" ).asBool(),
			       "render_cancel with NO renderJobId param still reports cancelled=true (routes through the attached controller)" );
			Check( !bareEnv.get( "result" ).get( "found" ).asBool(),
			       "render_cancel with no renderJobId param reports found=false (nothing to look up)" );

			session->AttachController( nullptr );
		}
		delete controller;
		pJob->release();
	}

	std::remove( slowScenePath.c_str() );
	std::printf( "=== (i2) CancelAsyncRender / render_cancel red-prove: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (j) Fix-round-1 P1-2 RED-PROVE: fair reservation for sync callers
//     under async-submission contention.  Reviewer's exact scenario: an
//     async-spam thread competing against 200 sync attempts.  Pre-fix,
//     sync success was ~0-2/200; post-fix it must be >90%.
//////////////////////////////////////////////////////////////////////
static void RunFairSlotReservationTest()
{
	std::printf( "=== AgentRenderAsyncTest: (j) P1-2 RED-PROVE: fair slot reservation under async-spam contention ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_fairness.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the fairness scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (fairness test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::atomic<bool> stop{ false };
	std::atomic<int>  asyncAccepted{ 0 };
	std::atomic<int>  asyncRefused{ 0 };

	// The async-spam thread: fires SubmitAgentRenderAsync as fast as
	// possible with a trivially-fast `fn` (no real render work needed --
	// the fairness scheme is about SLOT ARBITRATION, not render cost) so
	// there is maximal contention for the single slot.
	std::thread spamThread( [&]() {
		while( !stop.load( std::memory_order_acquire ) ) {
			SceneEditController::RenderJobId id = SceneEditController::kInvalidRenderJobId;
			const bool ok = controller.SubmitAgentRenderAsync( []() {}, String( "spam" ), &id );
			if( ok ) asyncAccepted.fetch_add( 1, std::memory_order_relaxed );
			else     asyncRefused.fetch_add( 1, std::memory_order_relaxed );
		}
	} );

	const int kSyncAttempts = 200;
	int syncSuccesses = 0;
	for( int i = 0; i < kSyncAttempts; ++i )
	{
		SceneEditController::RenderJobId id = SceneEditController::kInvalidRenderJobId;
		const bool ok = controller.SubmitAgentRenderSync( []() {}, String( "sync" ), &id, /*timeoutMs*/ 5000 );
		if( ok ) ++syncSuccesses;
	}

	stop.store( true, std::memory_order_release );
	spamThread.join();

	std::printf( "  [fairness] sync successes: %d/%d ; async accepted=%d refused=%d\n",
	             syncSuccesses, kSyncAttempts, asyncAccepted.load(), asyncRefused.load() );
	// Round-2 P2-C tightened this from the round-1 ">90%" threshold: with
	// the ticket-release-to-submit gap closed (SubmitAgentRenderSync now
	// holds mAgentRenderSlotMutex continuously from its fairness wait's
	// wake-up through the inline slot claim), NOTHING can steal a fairly-
	// won turn out from under a sync waiter anymore -- the only
	// remaining legitimate failure mode is Stop() landing exactly on this
	// waiter's turn (not exercised by this test) or the 5000ms fairness
	// wait itself timing out (a generous bound against a trivially-fast
	// `fn` and a single-slot occupant, essentially never in practice).
	// 100% minus that one-timeout allowance: require ALL BUT AT MOST ONE
	// of the 200 attempts to succeed, rather than the loose >90% the
	// round-1 fix left on the table.
	Check( syncSuccesses >= ( kSyncAttempts - 1 ),
	       "RED-PROVE P2-C MONEY ASSERTION: under async-spam contention, sync submitters succeed on ALL BUT AT MOST ONE of 200 attempts -- "
	       "the ticket-release-to-submit gap is now fully closed (round-1 left a real cross-thread window here, verified by "
	       "temporarily reverting to the round-trip SubmitAgentRenderAsync call, which regressed this back toward the old "
	       "looser bound); pre-round-1-fix (naive submit-or-reject), the reviewer measured ~0-2/200 sync successes against "
	       "the same spam pattern." );

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (j) fair slot reservation: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (k) Fix-round-1 P2-B RED-PROVE: a render requested WITH an override
//     while an editor transaction is open is refused ONCE, cleanly --
//     not the old compound "preview override skipped: ... -- render
//     refused: ..." double-failure message.  Post-transaction, the same
//     call succeeds.
//////////////////////////////////////////////////////////////////////
static void RunTxnOpenRenderRefusalTest()
{
	std::printf( "=== AgentRenderAsyncTest: (k) P2-B RED-PROVE: txn-open override render is refused ONCE, cleanly ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_txnrender.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the txn-render scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (txn-render test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the Job (txn-render test)" );
	if( session )
	{
		session->AttachController( &controller );

		Check( controller.BeginTransaction(), "transaction opens" );
		Check( controller.IsTransactionOpen(), "transaction reports open" );

		AgentRenderParams withOverride;
		withOverride.camera.hasLocation = true;  withOverride.camera.location = "3.5 0 0";
		withOverride.camera.hasLookAt   = true;  withOverride.camera.lookAt   = "0 0 0";
		const AgentRenderResult r = session->Render( withOverride );

		Check( !r.ok, "RED-PROVE P2-B: a WITH-override render while a transaction is open is refused (ok=false)" );
		std::printf( "    message=\"%s\"\n", r.message.c_str() );
		Check( r.message.find( "editor transaction" ) != std::string::npos,
		       "the refusal message names the actual cause (an open editor transaction)" );
		Check( r.message.find( "preview override skipped" ) == std::string::npos,
		       "RED-PROVE P2-B: the refusal is NOT the old compound message (dead fallback removed -- "
		       "pre-fix this re-entered RenderCore_(noOverride), which ALSO refused via SubmitAgentRenderSync, "
		       "producing \"preview override skipped: ... -- render refused: ...\")" );
		// A single clean cause should not ALSO carry the OTHER refusal's
		// wording glued on with " -- ".
		Check( r.message.find( " -- render refused" ) == std::string::npos,
		       "the refusal message is not a double-refusal concatenation" );

		Check( controller.RollbackTransaction(), "transaction rolls back" );
		Check( !controller.IsTransactionOpen(), "transaction reports closed" );

		// Control: the SAME override render succeeds once the transaction
		// is closed -- the refusal is transaction-scoped, not a broken path.
		const AgentRenderResult r2 = session->Render( withOverride );
		Check( r2.ok, "the same WITH-override render succeeds once the transaction is closed" );
		Check( !r2.png.empty(), "post-transaction override render produces PNG bytes" );
		Check( r2.cameraOverridden, "post-transaction override render actually applied the camera override" );

		session->AttachController( nullptr );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (k) txn-open render refusal: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (l) Fix-round-1 P1-A RED-PROVE: destroying the AgentSession WHILE an
//     async render is in flight must not crash / UAF -- the destructor
//     blocks (drains) until the worker's closure has genuinely finished
//     touching `this`.  Proven via a canary: a slow render that flips a
//     flag at its VERY END (after the point where a UAF would land if
//     the destructor returned early), checked to be true once
//     ~AgentSession has returned.  AttachController(nullptr) mid-flight
//     is proven the same way as a second sub-case.
//////////////////////////////////////////////////////////////////////
static void RunSessionDestroyedMidFlightTest()
{
	std::printf( "=== AgentRenderAsyncTest: (l) P1-A RED-PROVE: session destroyed mid-async-render -- no UAF ===\n" );

	// Sub-case 1: ~AgentSession while a render is in flight.
	//
	// The MONEY proof here is ORDERING, not just "didn't crash" -- a
	// canary counter that the WORKER THREAD increments at Rasterize()
	// entry (inside RenderCore_'s doRenderWork, i.e. INSIDE the closure
	// that holds the raw AgentSession `this` pointer) and decrements at
	// Rasterize() exit.  If `delete rawSession` returns while that
	// counter is still > 0, the destructor did NOT wait for the async
	// closure to finish touching `this` -- a direct, timing-independent
	// proof of the missing drain (ASan is not in the default build, so a
	// real UAF might not crash on this run; this canary catches the BUG,
	// not just its occasional visible symptom).
	{
		const std::string scenePath = WriteTemp( "rise_agent_async_destroymidflight.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the destroy-mid-flight scene to a temp file" );

		auto insideRasterize = std::make_shared<std::atomic<int>>( 0 );
		CanaryJob* pJob = new CanaryJob( 300, insideRasterize );
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "CanaryJob loads the native-v7 scene via the CST path (destroy-mid-flight test)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		AgentSession* rawSession = AgentSession::WrapJob( pJob ).release();
		Check( rawSession != nullptr, "AgentSession::WrapJob wraps the canary Job (destroy-mid-flight test)" );
		if( rawSession )
		{
			rawSession->AttachController( &controller );

			AgentRenderParams p;
			const AgentSession::AgentRenderAsyncResult ar = rawSession->RenderAsync( p );
			Check( ar.accepted, "the slow async render is accepted (destroy-mid-flight test)" );

			// Wait until the worker is GENUINELY inside Rasterize() (not
			// just "submitted") before destroying the session out from
			// under it -- this is the actual danger window.
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
				while( insideRasterize->load( std::memory_order_acquire ) == 0 &&
				       std::chrono::steady_clock::now() < deadline ) {
					std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
				}
			}
			Check( insideRasterize->load( std::memory_order_acquire ) > 0, "the worker is genuinely inside Rasterize() before we destroy the session" );

			const bool destroyReturnedPromptly = RunWatchdogged(
				"~AgentSession while an async render is in flight", 5000,
				[&]() { delete rawSession; } );
			Check( destroyReturnedPromptly, "RED-PROVE P1-A (hang class): ~AgentSession while an async render is in flight returns within the watchdog bound (the drain cancels rather than waiting out the full 300ms+ render)" );

			Check( insideRasterize->load( std::memory_order_acquire ) == 0,
			       "RED-PROVE P1-A MONEY ASSERTION: by the time ~AgentSession has RETURNED, the worker thread is no longer inside Rasterize() -- "
			       "i.e. the destructor's drain genuinely waited for the async closure (which holds a raw `this`) to finish BEFORE returning "
			       "(pre-fix: the destructor returned immediately while the worker was still mid-render, holding a now-dangling `this` -- a real UAF window)." );
		}

		controller.Stop();
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	// Sub-case 2: AttachController(nullptr) mid-flight (detach, not full
	// destruction) must drain the SAME way -- same canary-counter proof.
	{
		const std::string scenePath = WriteTemp( "rise_agent_async_detachmidflight.RISEscene", kScene );
		Check( !scenePath.empty(), "wrote the detach-mid-flight scene to a temp file" );

		auto insideRasterize2 = std::make_shared<std::atomic<int>>( 0 );
		CanaryJob* pJob = new CanaryJob( 300, insideRasterize2 );
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "CanaryJob loads the native-v7 scene via the CST path (detach-mid-flight test)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "AgentSession::WrapJob wraps the canary Job (detach-mid-flight test)" );
		if( session )
		{
			session->AttachController( &controller );

			AgentRenderParams p;
			const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
			Check( ar.accepted, "the slow async render is accepted (detach-mid-flight test)" );

			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
				while( insideRasterize2->load( std::memory_order_acquire ) == 0 &&
				       std::chrono::steady_clock::now() < deadline ) {
					std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
				}
			}
			Check( insideRasterize2->load( std::memory_order_acquire ) > 0, "the worker is genuinely inside Rasterize() before we detach (detach-mid-flight test)" );

			const bool detachReturnedPromptly = RunWatchdogged(
				"AttachController(nullptr) while an async render is in flight", 5000,
				[&]() { session->AttachController( nullptr ); } );
			Check( detachReturnedPromptly, "RED-PROVE P1-A (hang class): AttachController(nullptr) mid-flight returns within the watchdog bound (drains via cancel, not a full-duration wait)" );

			Check( insideRasterize2->load( std::memory_order_acquire ) == 0,
			       "RED-PROVE P1-A MONEY ASSERTION: by the time AttachController(nullptr) has RETURNED, the worker thread is no longer inside Rasterize() -- "
			       "the detach genuinely drained the outstanding async render before reassigning mController." );

			// The session is now detached -- destroying it (and the
			// controller) must be uneventful since nothing is outstanding.
		}

		controller.Stop();
		pJob->release();
		std::remove( scenePath.c_str() );
	}

	std::printf( "=== (l) session destroyed mid-flight: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// Round-2 P1-1 red-prove infrastructure: a Job whose Rasterize() ignores
// cancellation entirely (models an MLT/VCM checkpoint gap or a wedged
// OIDN call -- the exact case DrainAsyncRender_'s unbounded loop exists
// for) plus a capturing ILogPrinter so the test can assert the
// escalating "ignoring cancellation" warning actually fired.
//////////////////////////////////////////////////////////////////////

// Sleeps for the FULL requested duration in small slices, but -- unlike
// SlowRasterizeJob -- deliberately never consults IsCancelRequested() or
// any progress hook, so installing this controller's mCancelProgress as
// the Job's progress callback (AgentSession::RenderCore_'s normal P2-C
// install) has NO EFFECT: this render cannot be aborted early no matter
// how many times CancelAgentRender_() is called.  Also participates in a
// canary counter (same pattern as CanaryJob) so the test can prove
// exactly when the worker is inside vs. outside this call.
class CancelIgnoringSlowJob : public Job
{
public:
	CancelIgnoringSlowJob( unsigned int totalMs, std::shared_ptr<std::atomic<int>> counter )
	: Job(), mTotalMs( totalMs ), mCounter( std::move( counter ) ) {}
	bool Rasterize() override
	{
		mCounter->fetch_add( 1, std::memory_order_acq_rel );
		const unsigned int sliceMs = 10;
		const unsigned int slices  = ( mTotalMs + sliceMs - 1 ) / sliceMs;
		for( unsigned int i = 0; i < slices; ++i ) {
			// No cancel check here -- that is the point of this class.
			std::this_thread::sleep_for( std::chrono::milliseconds( sliceMs ) );
		}
		const bool ok = Job::Rasterize();
		mCounter->fetch_sub( 1, std::memory_order_acq_rel );
		return ok;
	}
private:
	unsigned int mTotalMs;
	std::shared_ptr<std::atomic<int>> mCounter;
};

// A minimal ILogPrinter that records every message containing `needle`
// (case-sensitive substring match) into a shared, mutex-guarded vector --
// installed once via GlobalLogPriv()->AddPrinter and never removed (this
// test binary has no OTHER test that depends on log output being absent,
// and RemoveAllPrinters would also silently kill the default stdout/file
// printers for every test that runs AFTER this one in the same process).
class CapturingLogPrinter : public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
{
public:
	explicit CapturingLogPrinter( std::string needle ) : mNeedle( std::move( needle ) ) {}

	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( mNeedle ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mMatches.push_back( msg );
		}
	}
	void Flush() override {}

	int MatchCount() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return static_cast<int>( mMatches.size() );
	}
	std::string LastMatch() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mMatches.empty() ? std::string() : mMatches.back();
	}

protected:
	~CapturingLogPrinter() override {}

private:
	std::string                mNeedle;
	mutable std::mutex         mMutex;
	std::vector<std::string>   mMatches;
};

//////////////////////////////////////////////////////////////////////
// (m) Round-2 P1-1 RED-PROVE: DrainAsyncRender_ loops UNBOUNDED across
//     MULTIPLE chunks against a render that ignores cancellation --
//     ~AgentSession must block for the render's FULL natural duration
//     (not just one chunk), the escalating warning must fire at least
//     once, and the canary must show zero worker activity once the
//     destructor has returned.  Chunk is set small (200ms) via the
//     ForTest_ hook so a ~1s cancel-ignoring render forces several loop
//     iterations without the test itself waiting out the 5000ms
//     production default.
//////////////////////////////////////////////////////////////////////
static void RunUnboundedDrainLoopTest()
{
	std::printf( "=== AgentRenderAsyncTest: (m) P1-1 RED-PROVE: DrainAsyncRender_ loops unbounded across multiple chunks ===\n" );

	CapturingLogPrinter* pCapOwned = new CapturingLogPrinter( "ignoring cancellation" );
	RISE::GlobalLogPriv()->AddPrinter( pCapOwned );
	// AddPrinter addref'd it (refcount 2: ours + the log's) -- keep a SEPARATE
	// raw pointer for the rest of this function's reads (safe_release nulls
	// its argument, so reusing pCapOwned after this line would silently read
	// through a null pointer even though the underlying object is still
	// alive, kept referenced by the log's printer list for the process's
	// remaining lifetime -- this test never calls RemoveAllPrinters).
	CapturingLogPrinter* pCap = pCapOwned;
	safe_release( pCapOwned );   // drop OUR construction ref; pCap remains valid via the log's own reference

	const std::string scenePath = WriteTemp( "rise_agent_async_unbounded_drain.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the unbounded-drain scene to a temp file" );

	// A render whose natural duration (~1000ms) is comfortably more than
	// one 200ms chunk -- if DrainAsyncRender_ only waited a single chunk
	// (the pre-fix bug), it would give up and return at ~200ms while the
	// worker is still genuinely inside Rasterize().
	auto insideRasterize = std::make_shared<std::atomic<int>>( 0 );
	CancelIgnoringSlowJob* pJob = new CancelIgnoringSlowJob( 1000, insideRasterize );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "CancelIgnoringSlowJob loads the native-v7 scene via the CST path" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	AgentSession* rawSession = AgentSession::WrapJob( pJob ).release();
	Check( rawSession != nullptr, "AgentSession::WrapJob wraps the cancel-ignoring Job" );
	if( rawSession )
	{
		rawSession->AttachController( &controller );
		rawSession->ForTest_SetDrainChunkMs( 200 );   // force multiple loop iterations against the ~1000ms render

		AgentRenderParams p;
		const AgentSession::AgentRenderAsyncResult ar = rawSession->RenderAsync( p );
		Check( ar.accepted, "the cancel-ignoring async render is accepted" );

		// Wait until the worker is genuinely inside Rasterize() before we
		// destroy the session out from under it.
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
			while( insideRasterize->load( std::memory_order_acquire ) == 0 &&
			       std::chrono::steady_clock::now() < deadline ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
			}
		}
		Check( insideRasterize->load( std::memory_order_acquire ) > 0, "the worker is genuinely inside the cancel-ignoring Rasterize() before we destroy the session" );

		const auto t0 = std::chrono::steady_clock::now();
		const bool destroyReturnedEventually = RunWatchdogged(
			"~AgentSession draining a cancel-ignoring render (multi-chunk loop)", 5000,
			[&]() { delete rawSession; } );
		const auto t1 = std::chrono::steady_clock::now();
		const long long destroyMs = std::chrono::duration_cast<std::chrono::milliseconds>( t1 - t0 ).count();

		Check( destroyReturnedEventually, "~AgentSession eventually returns (within the watchdog's generous outer bound) even though the render ignores cancellation" );
		std::printf( "  [unbounded-drain] ~AgentSession wall time against a cancel-ignoring ~1000ms render: %lldms\n", destroyMs );

		// THE MONEY ASSERTION: the destructor blocked for close to the
		// render's FULL natural duration -- i.e. MULTIPLE 200ms chunks --
		// not just the first one.  A pre-fix (single bounded wait, discard
		// the timeout) implementation would return at ~200ms; this asserts
		// well beyond that, proving the loop kept re-cancelling and
		// re-waiting rather than giving up early.
		Check( destroyMs >= 700,
		       "RED-PROVE P1-1 MONEY ASSERTION: ~AgentSession blocks for close to the render's FULL natural duration (>=700ms, several 200ms chunks) "
		       "rather than returning after a single chunk (~200ms) -- this assertion would FAIL against the pre-fix code (a single bounded "
		       "WaitForRenderJob(id, timeoutMs) whose result is discarded), which proceeds to free `this` after the first chunk while the "
		       "cancel-ignoring worker is still genuinely inside Rasterize()." );

		Check( insideRasterize->load( std::memory_order_acquire ) == 0,
		       "RED-PROVE P1-1: by the time ~AgentSession has RETURNED, the worker is no longer inside Rasterize() -- "
		       "the canary shows ZERO post-dtor worker activity (no UAF window left open)." );

		Check( pCap->MatchCount() > 0,
		       "RED-PROVE P1-1: the escalating 'ignoring cancellation' eLog_Warning fired at least once during the multi-chunk drain" );
		if( pCap->MatchCount() > 0 ) {
			std::printf( "  [unbounded-drain] last escalation warning: %s\n", pCap->LastMatch().c_str() );
		}
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (m) unbounded drain loop: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (n) Round-2 P1-2 RED-PROVE: mAsyncOutstandingJobId is never left
//     stale-nonzero after a completed async render (publish-before-clear
//     ordering), and a completed session's teardown never spuriously
//     cancels a DIFFERENT, concurrently-running interactive render.
//////////////////////////////////////////////////////////////////////
static void RunNoStaleOutstandingIdTest()
{
	std::printf( "=== AgentRenderAsyncTest: (n) P1-2 RED-PROVE: no stale mAsyncOutstandingJobId after a fast render ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_nostaleid.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the no-stale-id scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (no-stale-id test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the Job (no-stale-id test)" );
	if( session )
	{
		session->AttachController( &controller );

		// Money check A: fire a burst of trivially-fast async renders back
		// to back (maximal opportunity for the worker to complete BEFORE
		// the submitter gets back around to publishing, which is exactly
		// the window the pre-fix code left open) and, after each one is
		// observed complete via RenderWait, assert the outstanding-id
		// accessor reads back to 0 -- never left stuck at a stale nonzero
		// value naming an already-finished job.
		const int kIterations = 50;
		int staleObserved = 0;
		for( int i = 0; i < kIterations; ++i )
		{
			AgentRenderParams p;
			const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( p );
			Check( ar.accepted, "each trivially-fast async submission in the burst is accepted" );
			if( !ar.accepted ) continue;
			Check( session->RenderWait( ar.renderJobId, 5000 ), "each trivially-fast async render completes" );
			// Give the worker's OutstandingGuard destructor a brief window
			// to run (RenderWait observes mCurrentRenderJob.active==false,
			// which the ActiveFlipGuard sets BEFORE the OutstandingGuard's
			// own mAsyncCacheMutex-guarded clear further down the same
			// closure -- so poll briefly rather than assume instantaneous).
			bool clearedInTime = false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 500 );
			while( std::chrono::steady_clock::now() < deadline ) {
				if( session->ForTest_GetAsyncOutstandingJobId() == 0 ) { clearedInTime = true; break; }
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			}
			if( !clearedInTime ) ++staleObserved;
		}
		std::printf( "  [no-stale-id] stale-nonzero observations across %d fast-render iterations: %d\n", kIterations, staleObserved );
		Check( staleObserved == 0,
		       "RED-PROVE P1-2 MONEY ASSERTION: across a burst of 50 trivially-fast async renders, mAsyncOutstandingJobId is NEVER "
		       "left stale-nonzero after RenderWait observes completion -- this is exactly the window the pre-fix code left open "
		       "(SubmitAgentRenderAsync's internal notify_all() can let the worker run the WHOLE closure, including the "
		       "OutstandingGuard's clear, before the submitting thread gets back around to the id publish; with the old "
		       "release-then-reacquire mAsyncCacheMutex sequencing, that publish then landed AFTER the clear and stuck permanently)." );

		// Money check B: session teardown after a completed fast render
		// must NOT cancel a DIFFERENT, concurrently-running interactive
		// render.  Use a controller whose interactive DoOneRenderPass
		// participates in a ConcurrencyProof-style counter and reports
		// whether IT OBSERVED a cancellation mid-pass -- if the stale-id
		// bug were present, ~AgentSession (or AttachController(nullptr))
		// draining a long-cleared id would still call CancelAgentRender_(),
		// tripping the SHARED mCancelProgress and cancelling whatever
		// unrelated render happens to be in flight at that moment.
		AgentRenderParams p;
		const AgentSession::AgentRenderAsyncResult finalRender = session->RenderAsync( p );
		Check( finalRender.accepted, "the final (pre-teardown) async render is accepted" );
		Check( session->RenderWait( finalRender.renderJobId, 5000 ), "the final async render completes before teardown" );

		session->AttachController( nullptr );
	}

	// Now start a GENUINELY CONCURRENT interactive render on the SAME
	// controller/Job and, WHILE it is in flight, destroy the (detached,
	// fully-completed) session -- if a stale id survived, this destroy's
	// drain would call CancelAgentRender_() and truncate the interactive
	// pass early.
	std::atomic<bool> stopScrub{ false };
	std::atomic<unsigned int> cancelCountBefore{ controller.ForTest_GetCancelCount() };
	std::thread scrubThread( [&]() {
		double t = 0.0;
		while( !stopScrub.load( std::memory_order_acquire ) ) {
			controller.OnTimeScrub( t );
			t += 0.01;
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
	} );
	std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );   // let the interactive loop get genuinely busy

	session.reset();   // destroys the (already-detached) AgentSession -- must be uneventful

	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	stopScrub.store( true, std::memory_order_release );
	scrubThread.join();

	const unsigned int cancelCountAfter = controller.ForTest_GetCancelCount();
	std::printf( "  [no-stale-id] interactive cancel count before=%u after=%u\n", cancelCountBefore.load(), cancelCountAfter );
	Check( cancelCountAfter == cancelCountBefore.load(),
	       "RED-PROVE P1-2 BEHAVIORAL ASSERTION: destroying an already-completed, detached AgentSession does not spuriously cancel a "
	       "concurrently-running UNRELATED interactive render (mCancelCount is unchanged) -- a stale mAsyncOutstandingJobId would have "
	       "made DrainAsyncRender_ call CancelAgentRender_() against a job that no longer belongs to this session." );

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (n) no stale outstanding id: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (o) Round-2 P2-A RED-PROVE: when a CONTROLLER-ATTACHED render's
//     Rasterize() throws, the Job's progress-callback hook (installed as
//     mController->AgentRenderProgress() before Rasterize()) is still
//     restored to the in-slot prior (null in this test's config; since
//     the 2026-07-12 hardening the restore value is whatever the
//     exchange-capture found, not a hardcoded null) -- the RAII
//     ProgressRestoreGuard runs on the exceptional exit, not just the
//     ordinary one.  Pre-fix, the
//     `mJob->SetProgress( nullptr )` call sat AFTER Rasterize() with no
//     guard, so a throw skipped it entirely and left this controller's
//     mCancelProgress installed as the Job's progress hook forever.
//////////////////////////////////////////////////////////////////////
static void RunProgressRestoredOnThrowTest()
{
	std::printf( "=== AgentRenderAsyncTest: (o) P2-A RED-PROVE: progress hook restored after a controller-attached throw ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_progressthrow.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the progress-throw scene to a temp file" );

	ThrowingRasterizeJob* pJob = new ThrowingRasterizeJob();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ThrowingRasterizeJob loads the native-v7 scene via the CST path (progress-throw test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	Check( pJob->GetProgress() == nullptr, "the Job's progress hook is null before any render (baseline)" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the throwing Job (progress-throw test)" );
	if( session )
	{
		session->AttachController( &controller );

		// SYNCHRONOUS controller-attached render (routes through
		// SubmitAgentRenderSync -> the worker's assumeParked RenderCore_,
		// which installs mController->AgentRenderProgress() before
		// Rasterize() -- see RenderCore_'s P2-C comment) whose Rasterize()
		// throws.
		AgentRenderParams p;
		const AgentRenderResult r = session->Render( p );
		Check( !r.ok, "the controller-attached throwing render reports ok=false" );
		Check( r.message.find( "AgentRenderAsyncTest" ) != std::string::npos,
		       "the failure message names the thrown exception's text" );

		Check( pJob->GetProgress() == nullptr,
		       "RED-PROVE P2-A MONEY ASSERTION: after a controller-attached render whose Rasterize() throws, the Job's progress "
		       "hook is back to null -- the RAII ProgressRestoreGuard restored it on the exceptional exit. Pre-fix, the plain "
		       "`mJob->SetProgress(nullptr)` call sat AFTER Rasterize() with nothing guarding it, so the throw skipped straight "
		       "past it and left mController->AgentRenderProgress() installed as this Job's progress hook forever." );

		// Control: a NEXT, non-throwing render still installs and restores
		// the hook normally (the guard didn't break the ordinary path).
		pJob->SetThrowOnRasterize( false );
		const AgentRenderResult r2 = session->Render( p );
		Check( r2.ok, "a follow-up non-throwing render succeeds after the earlier throw" );
		Check( pJob->GetProgress() == nullptr, "the progress hook is null again after a normal, non-throwing render completes" );

		session->AttachController( nullptr );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (o) progress restored on throw: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// Fix-round-3 (churn UAF) test infra -----------------------------------
//
// A controller whose DoOneRenderPass can be held open on a manually-
// controlled gate, so a test can deterministically land an agent-render
// SUBMIT while an interactive pass is provably mid-flight (past its mint,
// not yet at its completion write) -- the exact seam the clobber needs.
//////////////////////////////////////////////////////////////////////
class SeamController : public SceneEditController
{
public:
	explicit SeamController( IJobPriv& job ) : SceneEditController( job, /*interactiveRasterizer*/0 ) {}

	// Called by the interactive pass; blocks until the test releases it.
	std::atomic<bool> passEntered{ false };
	std::atomic<bool> releaseGate{ false };

protected:
	// Deliberately does NOT consult IsCancelRequested() -- the test needs
	// this pass to stay genuinely mid-flight (mRendering still true) until
	// it EXPLICITLY releases the gate, mirroring a real render that has
	// not yet reached a cancellation checkpoint.  A production rasterizer
	// pass DOES check cancellation, but this seam's whole point is
	// simulating the pre-checkpoint window where that hasn't happened yet.
	void DoOneRenderPass() override
	{
		passEntered.store( true, std::memory_order_release );
		while( !releaseGate.load( std::memory_order_acquire ) )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
};

//////////////////////////////////////////////////////////////////////
// (p) Fix-round-3 DETERMINISTIC unit-shape RED-PROVE: mint an agent job
//     WHILE an interactive pass is provably mid-flight (past its own
//     mint, blocked inside DoOneRenderPass on a test-controlled gate),
//     then let the interactive pass complete, and assert the AGENT
//     record's `active` is NOT clobbered false by the interactive
//     pass's completion write -- and that WaitForRenderJob does not
//     false-complete for the still-running agent job either.
//////////////////////////////////////////////////////////////////////
static void RunInterleavedMintNoClobberTest()
{
	std::printf( "=== AgentRenderAsyncTest: (p) fix-round-3 RED-PROVE: interactive completion does not clobber an interleaved agent job ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_clobber.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the clobber scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (clobber test)" );

	SeamController controller( *pJob );
	controller.Start();   // NOT suppressed -- we need a genuine live interactive pass

	// Wait for the interactive loop's initial pass to be mid-flight (past
	// its mint, blocked on our gate).
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !controller.passEntered.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.passEntered.load( std::memory_order_acquire ), "the interactive pass is genuinely mid-flight before we mint the agent job" );

	// The interactive pass's mint already ran (it happens BEFORE
	// DoOneRenderPass is called) -- confirm the current record is
	// Interactive and active, then submit an agent render.  Because the
	// interactive pass is parked on our gate (not yet at ITS completion
	// write), a submission now genuinely interleaves: the agent mint runs
	// WHILE the interactive pass is still "in flight" from the record's
	// perspective, and the interactive pass's completion write will race
	// the agent's own record.
	const SceneEditController::RenderJobStatus beforeSubmit = controller.CurrentRenderJob();
	Check( beforeSubmit.active && beforeSubmit.renderClass == SceneEditController::RenderClass::Interactive,
	       "the interactive pass's own record is active before we submit the agent job" );

	// The agent closure is deliberately SLOW (well past the interactive
	// pass's own completion-write window below) -- otherwise, once the
	// gate releases and the interactive pass's own completion write
	// races the agent's, the agent's OWN closure could also finish and
	// legitimately clear `active` in the same window, masking a clobber
	// bug behind a real completion (both would read the same false
	// result for different reasons).  Slowing the closure isolates the
	// ILLEGITIMATE clear (interactive pass, wrong id) from the
	// legitimate one (agent's own ActiveFlipGuard, right id).
	std::atomic<bool> agentRan{ false };
	SceneEditController::RenderJobId agentJobId = SceneEditController::kInvalidRenderJobId;
	const bool accepted = controller.SubmitAgentRenderAsync(
		[&]() {
			std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
			agentRan.store( true, std::memory_order_release );
		},
		String( "clobber_probe" ), &agentJobId );
	Check( accepted, "the agent submission is accepted" );
	Check( agentJobId != SceneEditController::kInvalidRenderJobId, "the agent submission gets a real id" );

	// Give the worker a moment to actually mint+claim (it does not need to
	// RUN the closure yet -- the worker's own CancelAndParkRender_ is
	// waiting on mRendering, which is still true because our gate is
	// still closed).  Poll GetRenderJobStatus for the agent's own id
	// becoming the CURRENT record.
	bool sawAgentRecord = false;
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( std::chrono::steady_clock::now() < deadline ) {
			const SceneEditController::RenderJobLookup lk = controller.GetRenderJobStatus( agentJobId );
			if( lk.found ) { sawAgentRecord = true; break; }
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( sawAgentRecord, "the agent job's record becomes the CURRENT record (mint interleaved while the interactive pass is still mid-flight)" );

	// NOW release the interactive pass's gate -- its completion write
	// races the agent job, which is still sitting in the slot (the
	// worker's CancelAndParkRender_ has not yet observed mRendering==false).
	controller.releaseGate.store( true, std::memory_order_release );

	// Give the interactive pass's completion write time to land.
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	const SceneEditController::RenderJobLookup afterInteractiveCompletes = controller.GetRenderJobStatus( agentJobId );
	Check( afterInteractiveCompletes.found, "the agent job's record is still found after the interactive pass completes" );
	Check( afterInteractiveCompletes.status.active,
	       "FIX-ROUND-3 MONEY ASSERTION: the agent job's record is STILL ACTIVE after the interleaved interactive pass's completion write -- "
	       "the ownership-checked clear means the interactive pass's completion (a DIFFERENT job id) does not clobber this record. "
	       "Pre-fix, the interactive pass's unconditional `mCurrentRenderJob.active = false` would have cleared this exact record." );

	// WaitForRenderJob must likewise NOT false-complete for the agent job
	// while the agent's own closure has not yet run (the worker is still
	// legitimately blocked behind the -- now released -- interactive
	// pass's CancelAndParkRender_ wait; the closure runs trivially fast
	// once unblocked).
	Check( controller.WaitForRenderJob( agentJobId, 5000 ), "WaitForRenderJob eventually observes the agent job's REAL completion" );
	Check( agentRan.load( std::memory_order_acquire ), "the agent closure actually ran by the time WaitForRenderJob returned true" );

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (p) interleaved mint no-clobber: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// Fix-round-4 P2 test infra ---------------------------------------------
//
// A controller that gates on BOTH sides of the real P2 race:
//
//   * ForTest_OnAboutToMintInteractivePass -- RenderLoop calls this at the
//     END of its unlocked window, immediately BEFORE it re-acquires mMutex
//     to (re-check the agent gate and, if still clear) mint this pass's
//     mCurrentRenderJob record.
//   * ForTest_OnAgentWorkerAboutToParkRender -- the agent worker calls this
//     right after pulling a submission out of the slot (the mint + flag-
//     set in SubmitAgentRenderAsync_Locked has ALREADY happened by this
//     point), but BEFORE it acquires mMutex via CancelAndParkRender_.
//
// Both hooks fire UNLOCKED, so a test can independently control which side
// reaches its own mMutex acquisition first -- releasing RenderLoop's gate
// while the worker's gate is STILL held guarantees RenderLoop's mint block
// is the one that wins the mMutex race, which is exactly the ordering the
// P2 bug needs: the agent's mint + flag-set already landed, but RenderLoop
// still gets to mMutex FIRST.  Without this second (worker-side) gate, the
// worker's own CancelAndParkRender_ mMutex acquisition can win that race
// first purely by scheduler luck, holding mMutex for the whole (slow)
// closure and making RenderLoop's mint block WAIT on the mutex rather than
// race it -- which cannot exercise the bug (measured: the single-gate
// version of this test passed even with the P2 fix's in-lock re-check
// physically deleted, because the worker always got to mMutex first in
// practice).
//////////////////////////////////////////////////////////////////////
class MintSeamController : public SceneEditController
{
public:
	explicit MintSeamController( IJobPriv& job ) : SceneEditController( job, /*interactiveRasterizer*/0 ) {}

	std::atomic<bool> mintHookEntered{ false };
	std::atomic<bool> releaseMintGate{ false };
	std::atomic<bool> workerHookEntered{ false };
	std::atomic<bool> releaseWorkerGate{ false };

protected:
	void ForTest_OnAboutToMintInteractivePass() override
	{
		mintHookEntered.store( true, std::memory_order_release );
		while( !releaseMintGate.load( std::memory_order_acquire ) )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}

	void ForTest_OnAgentWorkerAboutToParkRender() override
	{
		workerHookEntered.store( true, std::memory_order_release );
		while( !releaseWorkerGate.load( std::memory_order_acquire ) )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
};

//////////////////////////////////////////////////////////////////////
// Queued-cancellation seam: holds the worker after it has accepted a slot
// occupant but before it can take mMutex and perform its per-render Reset().
// This makes the otherwise scheduler-dependent submit-then-cancel handoff
// deterministic.
//////////////////////////////////////////////////////////////////////
class QueuedCancellationSeamController : public SceneEditController
{
public:
	explicit QueuedCancellationSeamController( IJobPriv& job )
	: SceneEditController( job, /*interactiveRasterizer*/0 ) {}

	std::atomic<bool> workerHookEntered{ false };
	std::atomic<bool> releaseWorkerGate{ false };

protected:
	void ForTest_OnAgentWorkerAboutToParkRender() override
	{
		workerHookEntered.store( true, std::memory_order_release );
		while( !releaseWorkerGate.load( std::memory_order_acquire ) )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
};

//////////////////////////////////////////////////////////////////////
// (q0) Queued cancellation must survive the worker's per-render Reset().
// A cancel after an async submission but before the worker starts used to be
// silently erased, turning Stop/render_cancel into a full render.
//////////////////////////////////////////////////////////////////////
static void RunQueuedCancellationSurvivesWorkerStartTest()
{
	std::printf( "=== AgentRenderAsyncTest: (q0) queued cancellation survives worker Reset ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_queued_cancel.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the queued-cancel scene" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the queued-cancel scene" );

	QueuedCancellationSeamController controller( *pJob );
	std::atomic<bool> firstSawCancellation{ false };
	SceneEditController::RenderJobId firstId = SceneEditController::kInvalidRenderJobId;
	const bool firstAccepted = controller.SubmitAgentRenderAsync(
		[&]() {
			firstSawCancellation.store(
				controller.AgentRenderProgress()->IsCancelled(), std::memory_order_release );
		},
		String( "queued_cancel_probe" ), &firstId );
	Check( firstAccepted, "the queued-cancel agent submission is accepted" );

	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
		while( !controller.workerHookEntered.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.workerHookEntered.load( std::memory_order_acquire ),
	       "the worker is deterministically parked before its Reset()" );

	// This is the precise formerly-lost handoff: the slot is occupied, but
	// the worker has not reset or invoked the closure yet.
	controller.CancelAgentRender_();
	controller.releaseWorkerGate.store( true, std::memory_order_release );
	Check( controller.WaitForRenderJob( firstId, 5000 ),
	       "the queued-cancel job drains after its worker gate is released" );
	Check( firstSawCancellation.load( std::memory_order_acquire ),
	       "MONEY ASSERTION (q0): a cancellation issued before worker start is still visible inside the submitted closure "
	       "after the worker's Reset() -- pre-fix Reset cleared it and this read was false" );

	// The per-job handoff must not poison the next slot occupant.
	std::atomic<bool> nextStartedClean{ false };
	SceneEditController::RenderJobId nextId = SceneEditController::kInvalidRenderJobId;
	const bool nextAccepted = controller.SubmitAgentRenderAsync(
		[&]() {
			nextStartedClean.store(
				!controller.AgentRenderProgress()->IsCancelled(), std::memory_order_release );
		},
		String( "queued_cancel_control" ), &nextId );
	Check( nextAccepted, "a fresh agent submission is accepted after queued cancellation drains" );
	Check( controller.WaitForRenderJob( nextId, 5000 ), "the fresh control job completes" );
	Check( nextStartedClean.load( std::memory_order_acquire ),
	       "the queued cancellation is scoped to its original slot occupant, not the next job" );

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (q0) queued cancellation survives worker Reset: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (q) Fix-round-4 P2 DETERMINISTIC RED-PROVE: mint an agent job (its mint
//     + flag-set fully lands), with BOTH RenderLoop's mint attempt and the
//     agent worker's own mMutex acquisition held open on separate gates,
//     then release RenderLoop's gate FIRST (while the worker is still
//     held back) so RenderLoop's mint block deterministically wins the
//     mMutex race and runs its (re-check-and-skip, post-fix) or
//     (unconditional-clobber, pre-fix) mint logic against the agent's
//     already-landed record.  This is the MINT-clobber sibling of test
//     (p)'s completion-clobber -- (p) forces the race at the interactive
//     pass's completion write; this test forces it at the interactive
//     pass's MINT itself, which is the P2 gap: pre-fix, RenderLoop's mint
//     block took mMutex and wrote mCurrentRenderJob UNCONDITIONALLY with
//     no re-check of mAgentRenderBlocksInteractive.
//////////////////////////////////////////////////////////////////////
static void RunMintClobberRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (q) fix-round-4 P2 RED-PROVE: RenderLoop's own mint does not clobber an interleaved agent mint ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_mintclobber.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the mint-clobber scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (mint-clobber test)" );

	MintSeamController controller( *pJob );
	controller.Start();   // NOT suppressed -- we need RenderLoop to actually reach the pre-mint hook

	// Wait for RenderLoop to be parked in the pre-mint gap (past its own
	// line-4473 gate snapshot -- which read mAgentRenderBlocksInteractive
	// as false, since nothing has submitted yet -- and blocked in our
	// hook, not yet at the in-lock re-check/mint).
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !controller.mintHookEntered.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.mintHookEntered.load( std::memory_order_acquire ), "RenderLoop is genuinely parked in the pre-mint gap before we submit the agent job" );

	// Submit an agent render NOW, while RenderLoop is parked in the gap
	// between its own gate snapshot and its mint.  SubmitAgentRenderAsync_Locked
	// mints {agentJobId, AgentPreview, active=true} into mCurrentRenderJob
	// and only afterward sets mAgentRenderBlocksInteractive -- so the
	// agent's mint genuinely lands BEFORE RenderLoop's flag is set, let
	// alone before RenderLoop's own in-lock re-check runs.  The worker
	// thread will wake, pull the submission out of the slot, and then
	// park on its OWN gate (ForTest_OnAgentWorkerAboutToParkRender) before
	// it ever touches mMutex -- so it cannot race RenderLoop for mMutex
	// yet, no matter how the scheduler would otherwise order the two
	// threads.
	std::atomic<bool> agentRan{ false };
	SceneEditController::RenderJobId agentJobId = SceneEditController::kInvalidRenderJobId;
	const bool accepted = controller.SubmitAgentRenderAsync(
		[&]() { agentRan.store( true, std::memory_order_release ); },
		String( "mint_clobber_probe" ), &agentJobId );
	Check( accepted, "the agent submission is accepted while RenderLoop sits in the pre-mint gap" );
	Check( agentJobId != SceneEditController::kInvalidRenderJobId, "the agent submission gets a real id" );

	// Confirm the agent's record is genuinely the CURRENT record before we
	// release RenderLoop -- this is the state the pre-fix code would have
	// clobbered.
	bool sawAgentRecord = false;
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( std::chrono::steady_clock::now() < deadline ) {
			const SceneEditController::RenderJobLookup lk = controller.GetRenderJobStatus( agentJobId );
			if( lk.found ) { sawAgentRecord = true; break; }
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( sawAgentRecord, "the agent job's record is the CURRENT record while RenderLoop is still parked pre-mint" );

	// Confirm the WORKER is genuinely parked on ITS gate too, before mMutex
	// -- i.e. it cannot yet contend for mMutex, so releasing RenderLoop's
	// gate next is guaranteed to let RenderLoop's mint block reach mMutex
	// first.
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !controller.workerHookEntered.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.workerHookEntered.load( std::memory_order_acquire ), "the agent worker is genuinely parked before mMutex, unable to contend for it yet" );

	// NOW release RenderLoop's pre-mint gate ONLY -- the worker is still
	// held back on its own gate, so RenderLoop's mint block is GUARANTEED
	// to reach mMutex first.  Pre-fix: RenderLoop immediately mints a
	// fresh Interactive record unconditionally, stomping the agent's
	// record we just confirmed above.  Post-fix: RenderLoop re-checks
	// mAgentRenderBlocksInteractive (true, set by the accepted submission
	// above) inside that SAME mMutex hold and skips the mint entirely for
	// this iteration.
	controller.releaseMintGate.store( true, std::memory_order_release );

	// Give RenderLoop's (correct, post-fix) skip -- or (buggy, pre-fix)
	// clobbering mint -- time to land.  RenderLoop is not competing with
	// the worker for mMutex (still gated), so this is a generous margin,
	// not a race window of its own.
	std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );

	const SceneEditController::RenderJobLookup afterRelease = controller.GetRenderJobStatus( agentJobId );
	Check( afterRelease.found,
	       "FIX-ROUND-4 P2 MONEY ASSERTION: the agent job's record is STILL FOUND after RenderLoop's mint block has had a clean, "
	       "uncontested shot at mMutex -- pre-fix, RenderLoop's unconditional mint at this site would have overwritten "
	       "mCurrentRenderJob with a fresh Interactive-class record, and GetRenderJobStatus(agentJobId) would report "
	       "not-found for the whole remainder of the agent job's real run." );
	Check( afterRelease.found && afterRelease.status.renderClass == SceneEditController::RenderClass::AgentPreview,
	       "the record found (if any) is still the AgentPreview record, not a clobbering Interactive one" );

	// NOW release the worker's gate -- let the agent job actually run and
	// complete.
	controller.releaseWorkerGate.store( true, std::memory_order_release );

	Check( controller.WaitForRenderJob( agentJobId, 5000 ), "WaitForRenderJob eventually observes the agent job's REAL completion" );
	Check( agentRan.load( std::memory_order_acquire ), "the agent closure actually ran" );

	// The interactive loop must resume normally afterward -- not wedged by
	// the skip-and-continue this fix adds.  This test doesn't assert
	// anything about whether the ORIGINAL edit's own pass survives the
	// bounce (Fix-round-5, below, is the dedicated red-prove for that) --
	// it just needs to confirm the loop isn't permanently wedged, so it
	// nudges a FRESH edit via the public OnTimeScrub kick (mirrors this
	// file's other tests' use of it as a pointer-free KickRender) and
	// requires at least one MORE interactive pass to complete.
	const unsigned int countAfterAgent = controller.ForTest_GetRenderCount();
	controller.OnTimeScrub( 0.01 );
	Check( controller.ForTest_WaitForRenders( countAfterAgent + 1, 5000 ),
	       "the interactive loop resumes and completes at least one more pass after the agent job releases the gate (skip-and-continue does not wedge RenderLoop)" );

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (q) mint-clobber RED-PROVE: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (r) Fix-round-5 LOST-EDIT WEDGE RED-PROVE: the mint-site re-check this
//     file's own (q) test above exercises (RenderLoop wins the mMutex
//     race against the agent worker's own park, and bounces via
//     `continue` because mAgentRenderBlocksInteractive is set) has a
//     SECOND consequence (q) doesn't check: by the time that bounce
//     runs, the loop's own `mEditPending.exchange( false, ... )` at line
//     ~4467 has ALREADY consumed the edit that woke this iteration
//     (isExplicitEdit is its result).  Bouncing without restoring
//     mEditPending drops that edit on the floor -- the agent worker's
//     completion path clears mAgentRenderBlocksInteractive and
//     notify_all()s, but never sets mEditPending, so the wake predicate
//     stays false and RenderLoop parks forever.  The ORIGINAL edit's
//     interactive pass never runs; render count stays wedged at whatever
//     it was before the bounce.
//
//     This test reproduces exactly that: park RenderLoop in the pre-mint
//     gap (via the initial-render edit Start() queues, same trigger (q)
//     uses), submit an agent job so its mint + flag-set lands first,
//     confirm both sides are genuinely parked on their own gates, then
//     release ONLY RenderLoop's gate so it deterministically loses the
//     re-check and bounces.  Release the worker's gate last so the
//     agent completes and clears the interactive gate.  Unlike (q), this
//     test does NOT nudge a fresh edit afterward -- it asserts the
//     ORIGINAL (pre-bounce) edit's own interactive pass eventually runs
//     with NO further external kick.  Pre-fix, this hangs/fails (render
//     count never advances past 0); post-fix, restoring mEditPending on
//     the bounce lets the next wake re-exchange it and the pass runs.
//////////////////////////////////////////////////////////////////////
static void RunLostEditWedgeRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (r) fix-round-5 RED-PROVE: a bounced mint-site re-check does not drop the edit that woke it ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_lostedit.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the lost-edit-wedge scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (lost-edit-wedge test)" );

	MintSeamController controller( *pJob );
	controller.Start();   // NOT suppressed -- Start() queues the initial-render edit that this test's original "edit" is; RenderLoop reaches the pre-mint hook trying to service it.

	// Wait for RenderLoop to be parked in the pre-mint gap, exactly as (q)
	// does -- this is the point where isExplicitEdit is already latched
	// true (from the initial-render edit) and mEditPending has ALREADY
	// been exchanged false for it.
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !controller.mintHookEntered.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.mintHookEntered.load( std::memory_order_acquire ), "RenderLoop is genuinely parked in the pre-mint gap servicing the initial-render edit" );

	Check( controller.ForTest_GetRenderCount() == 0, "no interactive pass has run yet -- the original edit is still unserviced" );

	// Submit an agent render NOW, while RenderLoop is parked pre-mint --
	// its mint + flag-set lands before RenderLoop's own in-lock re-check
	// runs, same setup as (q).
	std::atomic<bool> agentRan{ false };
	SceneEditController::RenderJobId agentJobId = SceneEditController::kInvalidRenderJobId;
	const bool accepted = controller.SubmitAgentRenderAsync(
		[&]() { agentRan.store( true, std::memory_order_release ); },
		String( "lost_edit_wedge_probe" ), &agentJobId );
	Check( accepted, "the agent submission is accepted while RenderLoop sits in the pre-mint gap" );
	Check( agentJobId != SceneEditController::kInvalidRenderJobId, "the agent submission gets a real id" );

	// Confirm the worker is genuinely parked on its OWN gate too, before
	// mMutex, so releasing RenderLoop's gate next is guaranteed to let
	// RenderLoop reach mMutex -- and lose the re-check -- first.
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !controller.workerHookEntered.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.workerHookEntered.load( std::memory_order_acquire ), "the agent worker is genuinely parked before mMutex, unable to contend for it yet" );

	// Release RenderLoop's gate ONLY.  It reaches mMutex, re-checks
	// mAgentRenderBlocksInteractive (true, set by the accepted submission),
	// and bounces via `continue` -- pre-fix, silently dropping the
	// already-exchanged isExplicitEdit; post-fix, restoring mEditPending
	// before the bounce.
	controller.releaseMintGate.store( true, std::memory_order_release );

	// Give the bounce time to land, then release the worker so the agent
	// job actually runs and clears mAgentRenderBlocksInteractive.
	std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
	controller.releaseWorkerGate.store( true, std::memory_order_release );

	Check( controller.WaitForRenderJob( agentJobId, 5000 ), "WaitForRenderJob eventually observes the agent job's REAL completion" );
	Check( agentRan.load( std::memory_order_acquire ), "the agent closure actually ran" );

	// MONEY ASSERTION: with the agent gate now clear, RenderLoop's wake
	// predicate must go true on ITS OWN -- from the RESTORED mEditPending,
	// not from any external kick this test issues -- and the ORIGINAL
	// edit's interactive pass must run.  Pre-fix (no restore), mEditPending
	// stays false forever after the bounce and this wait times out with
	// the render count stuck at 0 -- the exact "render count stuck at 1"
	// (here: stuck at 0, since this test's edit is the FIRST one)
	// symptom the reviewer's probe identified.  No OnTimeScrub / KickRender
	// call appears anywhere below this point in the test.
	Check( controller.ForTest_WaitForRenders( 1, 5000 ),
	       "FIX-ROUND-5 MONEY ASSERTION: the original edit's interactive pass runs to completion with NO further external kick after the agent job releases the gate (the bounce restored mEditPending instead of dropping it)" );

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (r) lost-edit-wedge RED-PROVE: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (s) Fix-round-3 CHURN RED-PROVE / permanent regression test: the
//     reviewer's repro -- churn N throwaway AgentSessions (mint,
//     RenderAsync, destroy immediately) while a CONCURRENT interactive
//     thread keeps kicking real render passes via OnTimeScrub.  This is
//     the pattern that crashed ~35-40% of the time pre-fix (measured
//     10/10 process-level crashes -- SIGSEGV -- on this machine's
//     churn driver against unfixed HEAD; see the task's repro notes).
//     Post-fix this must complete cleanly every time.  Watchdogged: a
//     regression here is either a crash (the harness process dies,
//     which run_all_tests.sh observes as a nonzero exit / missing
//     PASS line) or -- if it merely hangs instead -- the watchdog below
//     reports a loud, specific failure.
//////////////////////////////////////////////////////////////////////
static void RunChurnConcurrentInteractiveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (s) fix-round-3 RED-PROVE: session churn under a concurrent interactive stream (no UAF) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_churn.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the churn scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (churn test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start();   // genuinely live interactive loop -- REQUIRED to hit the clobber window

	std::atomic<bool> stopScrub{ false };
	std::thread scrubThread( [&]() {
		double t = 0.0;
		while( !stopScrub.load( std::memory_order_acquire ) )
		{
			controller.OnTimeScrub( t );
			t += 0.01;
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	} );

	const int kIterations = 150;
	const bool completedInTime = RunWatchdogged(
		"churn loop (150 session mint/RenderAsync/destroy cycles under a concurrent interactive stream)", 60000,
		[&]() {
			for( int i = 0; i < kIterations; ++i )
			{
				std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
				session->AttachController( &controller );
				AgentRenderParams p;
				session->RenderAsync( p );
				// session destroyed HERE, immediately -- ~AgentSession must
				// drain genuinely (not just observe a clobbered `active`)
				// before this scope exits, or the worker can go on to
				// dereference this freed session.
			}
		} );
	Check( completedInTime, "the churn loop completes within the watchdog bound (no hang)" );

	stopScrub.store( true, std::memory_order_release );
	scrubThread.join();

	// Reaching here at all (no crash) is the primary assertion for this
	// test -- a UAF crash kills the process, which run_all_tests.sh
	// observes directly. Also confirm the controller is still in a sane
	// state (no poisoned slot) by running one more clean submission
	// BEFORE Stop() (a post-Stop() submission is honestly refused by
	// design -- see RunPostStopRefusalTest -- so that check belongs here,
	// not after teardown).
	{
		std::unique_ptr<AgentSession> finalSession = AgentSession::WrapJob( pJob );
		finalSession->AttachController( &controller );
		AgentRenderParams p;
		const AgentSession::AgentRenderAsyncResult finalAr = finalSession->RenderAsync( p );
		Check( finalAr.accepted, "a fresh submission after the churn loop is still accepted (the slot was not left poisoned)" );
		if( finalAr.accepted ) {
			Check( finalSession->RenderWait( finalAr.renderJobId, 5000 ), "the post-churn submission completes normally" );
		}
		finalSession->AttachController( nullptr );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (s) churn under concurrent interactive: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// Fix-round-6 (save-vs-render race) test infra ------------------------
//
// SaveGateMintController combines the existing MintSeamController mint-
// site gate (ForTest_OnAboutToMintInteractivePass, parks RenderLoop right
// before its in-lock mint-site re-check) with a NEW save-side gate
// (ForTest_OnSaveEngineAboutToRun, parks RequestSave right after it has
// set mSaving=true and released mMutex, before SaveEngine::Save runs).
// Holding BOTH gates open lets the test deterministically reproduce the
// exact ordering the race needs: mSaving is genuinely true (RequestSave's
// step 1 already ran), and RenderLoop is genuinely parked in the pre-mint
// gap -- so releasing RenderLoop's gate next is guaranteed to make its
// mint-site re-check the very next thing that touches mMutex, with
// mSaving still held true by the still-gated save thread.  This mirrors
// this file's (q)/(r) agent-flag tests exactly, with mSaving standing in
// for mAgentRenderBlocksInteractive and the save-gate hook standing in
// for the agent worker's own park hook.
//////////////////////////////////////////////////////////////////////
class SaveGateMintController : public SceneEditController
{
public:
	explicit SaveGateMintController( IJobPriv& job ) : SceneEditController( job, /*interactiveRasterizer*/0 ) {}

	std::atomic<bool> mintHookEntered{ false };
	std::atomic<bool> releaseMintGate{ false };
	std::atomic<bool> saveHookEntered{ false };
	std::atomic<bool> releaseSaveGate{ false };

protected:
	void ForTest_OnAboutToMintInteractivePass() override
	{
		mintHookEntered.store( true, std::memory_order_release );
		while( !releaseMintGate.load( std::memory_order_acquire ) )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}

	void ForTest_OnSaveEngineAboutToRun() override
	{
		saveHookEntered.store( true, std::memory_order_release );
		while( !releaseSaveGate.load( std::memory_order_acquire ) )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
};

//////////////////////////////////////////////////////////////////////
// (t) Fix-round-6 SAVE-VS-RENDER RED-PROVE: RequestSave sets mSaving=true
//     (step 1) and parks on ForTest_OnSaveEngineAboutToRun BEFORE running
//     SaveEngine::Save, while RenderLoop is independently parked in the
//     pre-mint gap (ForTest_OnAboutToMintInteractivePass) servicing the
//     initial-render edit Start() queues -- same trigger (q)/(r) use.
//     Releasing RenderLoop's gate while the save thread is STILL held on
//     its own gate guarantees RenderLoop's mint-site re-check runs while
//     mSaving is genuinely true.
//
//     Pre-fix (mint-site checks only mAgentRenderBlocksInteractive): the
//     mint-site re-check does not see mSaving at all, so RenderLoop mints
//     and DoOneRenderPass runs CONCURRENTLY with the (still-parked, about
//     to call SaveEngine::Save) save thread -- i.e. an interactive render
//     pass would be free to run while a save is "in flight" from the
//     wake-predicate's point of view.  This test proves that never
//     happens: render count must stay AT ZERO for the whole time mSaving
//     is held true, no matter how long the save gate is held.
//
//     Post-fix, RenderLoop's mint-site bounces (continue) and -- mirroring
//     Fix-round-5's lost-edit-wedge rescue -- restores mEditPending since
//     the original wake was a real (isExplicitEdit) edit.  Releasing the
//     save gate lets RequestSave's step 3 clear mSaving and notify_one()
//     under mMutex; the MONEY ASSERTION is that the ORIGINAL edit's
//     interactive pass then runs to completion with NO further external
//     kick -- exactly the same "no external kick" shape (r) uses to prove
//     the restore actually rescues the edit rather than merely not
//     crashing.
//////////////////////////////////////////////////////////////////////
static void RunSaveVsRenderRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (t) fix-round-6 RED-PROVE: a save in flight bounces the interactive mint, and the bounced edit still renders with no further kick ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_savevsrender.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the save-vs-render scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (save-vs-render test)" );

	SaveGateMintController controller( *pJob );
	controller.Start();   // NOT suppressed -- Start() queues the initial-render edit that RenderLoop will be parked trying to service.

	// Wait for RenderLoop to be parked in the pre-mint gap, same as (q)/(r).
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !controller.mintHookEntered.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.mintHookEntered.load( std::memory_order_acquire ), "RenderLoop is genuinely parked in the pre-mint gap servicing the initial-render edit" );
	Check( controller.ForTest_GetRenderCount() == 0, "no interactive pass has run yet -- the original edit is still unserviced" );

	// Kick off RequestSave on its own thread -- it must run step 1 (cancel
	// -- a no-op here, nothing is rendering -- then set mSaving=true) and
	// reach its own gate BEFORE we release RenderLoop's gate below.
	std::thread saveThread( [&]() {
		SaveResult r = controller.RequestSave( scenePath );
		Check( Succeeded( r.status ), "the save completes successfully once released (Saved or NoOp)" );
	} );

	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !controller.saveHookEntered.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.saveHookEntered.load( std::memory_order_acquire ), "RequestSave is genuinely parked with mSaving=true, before SaveEngine::Save runs" );
	Check( controller.IsSaving(), "IsSaving() observes mSaving=true while the save thread is gated" );

	// NOW release RenderLoop's gate ONLY -- the save thread is still held
	// back on its own gate, so RenderLoop's mint-site re-check is
	// GUARANTEED to run while mSaving is still true.  Pre-fix: the
	// mint-site re-check doesn't consult mSaving at all, so RenderLoop
	// mints and runs DoOneRenderPass concurrently with the (still parked,
	// about to call SaveEngine::Save) save thread.  Post-fix: RenderLoop
	// re-checks mSaving (true) inside the same mMutex hold and bounces via
	// `continue`, restoring mEditPending since isExplicitEdit is true.
	controller.releaseMintGate.store( true, std::memory_order_release );

	// Give RenderLoop's bounce (or the buggy mint) time to land.  While
	// the save gate is STILL held, the render count must not move --
	// this is the MONEY ASSERTION for the "no concurrent mint during
	// save" half of the claim.  Poll for a generous window rather than a
	// single sleep+check so a slow-scheduled machine doesn't produce a
	// false pass by finishing the check before a buggy mint would have
	// landed.
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 300 );
		bool sawMint = false;
		while( std::chrono::steady_clock::now() < deadline ) {
			if( controller.ForTest_GetRenderCount() != 0 ) { sawMint = true; break; }
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
		Check( !sawMint,
		       "FIX-ROUND-6 MONEY ASSERTION (a): no interactive mint lands while mSaving is held true -- RenderLoop's mint-site re-check "
		       "bounced instead of minting/running DoOneRenderPass concurrently with the (still parked) save thread" );
	}
	Check( controller.IsSaving(), "mSaving is still true -- the save thread has not been released yet (sanity check on the test's own gating)" );

	// NOW release the save gate -- SaveEngine::Save actually runs (fast;
	// this is a 24x24 8-sample scene) and RequestSave's step 3 clears
	// mSaving + notify_one()s under mMutex.
	controller.releaseSaveGate.store( true, std::memory_order_release );
	saveThread.join();
	Check( !controller.IsSaving(), "mSaving is cleared once the save thread completes" );

	// MONEY ASSERTION (b): with mSaving now clear, RenderLoop's wake
	// predicate must go true on ITS OWN -- from the RESTORED mEditPending,
	// not from any external kick this test issues -- and the ORIGINAL
	// edit's interactive pass must run.  Pre-fix (no restore on the
	// mSaving bounce), mEditPending would stay false forever after the
	// bounce and this wait would time out with the render count stuck at
	// 0.  No OnTimeScrub / KickRender call appears anywhere below this
	// point in the test.
	Check( controller.ForTest_WaitForRenders( 1, 5000 ),
	       "FIX-ROUND-6 MONEY ASSERTION (b): the original edit's interactive pass runs to completion with NO further external kick after the save releases mSaving (the bounce restored mEditPending instead of dropping it)" );

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (t) save-vs-render RED-PROVE: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (t2) SAVE-SNAPSHOT RED-PROVE: RequestSave captures the persistent CST
//      head before unlocked IO. While the save hook holds IO back, an
//      agent commit is allowed to replace the live Document. The first
//      save must write the pre-edit snapshot and leave the newer head
//      dirty; a second save writes that newer head and clears dirty.
//
//      Pre-fix, SaveEngine borrowed Job::pCstDocument after the hook.
//      Depending on timing this either serialized the post-snapshot edit
//      while claiming the earlier head, or held a raw pointer that a
//      writer could free. The exact disk/live split below pins the safe
//      snapshot linearization point without relying on a UAF crash.
//////////////////////////////////////////////////////////////////////
static void RunSaveSnapshotConcurrentEditTest()
{
	std::printf( "=== AgentRenderAsyncTest: (t2) save serializes an immutable CST snapshot while a newer edit remains dirty ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_async_save_snapshot.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the save-snapshot scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the save-snapshot scene" );
	SaveGateMintController controller( *pJob );

	SaveResult firstSave;
	std::thread saveThread( [&]() {
		firstSave = controller.RequestSave( scenePath );
	} );
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !controller.saveHookEntered.load( std::memory_order_acquire )
		    && std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( controller.saveHookEntered.load( std::memory_order_acquire ),
		"RequestSave captured its snapshot and is parked before IO" );

	const SceneEditController::AgentCommitResult edit = controller.ApplyAgentParamEdit(
		String( "mat_emit" ), String( "material" ), String( "scale" ), String( "45.0" ), nullptr );
	Check( edit.applied, "a newer agent edit can commit while snapshot IO is pending" );
	Check( std::string( controller.SerializedSceneText().c_str() ).find( "scale 45.0" ) != std::string::npos,
		"live retained Document contains the newer scale 45.0 edit" );

	controller.releaseSaveGate.store( true, std::memory_order_release );
	saveThread.join();
	Check( Succeeded( firstSave.status ), "first snapshot save completes successfully" );
	Check( controller.HasUnsavedChanges(),
		"MONEY (t2-a): newer head remains dirty after the older snapshot is saved" );

	std::ifstream firstDiskIn( scenePath.c_str(), std::ios::binary );
	std::stringstream firstDiskBytes;
	firstDiskBytes << firstDiskIn.rdbuf();
	const std::string firstDisk = firstDiskBytes.str();
	Check( firstDisk.find( "scale 30.0" ) != std::string::npos
	    && firstDisk.find( "scale 45.0" ) == std::string::npos,
		"MONEY (t2-b): first save wrote exactly the pre-edit snapshot, not the concurrently replaced live Document" );

	const SaveResult secondSave = controller.RequestSave( scenePath );
	Check( Succeeded( secondSave.status ), "second save writes the newer live head" );
	Check( !controller.HasUnsavedChanges(), "second save baselines the unchanged newer head as clean" );
	std::ifstream secondDiskIn( scenePath.c_str(), std::ios::binary );
	std::stringstream secondDiskBytes;
	secondDiskBytes << secondDiskIn.rdbuf();
	Check( secondDiskBytes.str().find( "scale 45.0" ) != std::string::npos,
		"MONEY (t2-c): second save persisted the newer edit" );

	pJob->release();
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// Model-B F2 slice S4: SubmitProductionRenderSync tests.
//
// (u) TRIPLE-CLASS concurrency proof: a production submission
//     (SubmitProductionRenderSync), an agent-render spam loop
//     (SubmitAgentRenderAsync), and the interactive loop (via a live
//     scrub thread) must never observe more than 1 thread inside the
//     shared render critical section at once -- extending (c)'s
//     two-class proof to all THREE classes now that Production exists.
//     RED-PROVE half: a raw, UNCOORDINATED direct Job::Rasterize() call
//     (bypassing the coordinator entirely, i.e. the pre-S4 GUI shape)
//     racing the SAME live interactive loop DOES hit concurrency 2 --
//     demonstrating the gap this slice closes.
//////////////////////////////////////////////////////////////////////
static void RunProductionTripleConcurrencyProofTest()
{
	std::printf( "=== AgentRenderAsyncTest: (u) production/agent/interactive triple-class concurrency proof ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_production_triple.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the production-triple scene to a temp file" );

	ConcurrencyProof proof;
	ConcurrencyProofJob* pJob = new ConcurrencyProofJob( proof );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ConcurrencyProofJob loads the native-v7 scene via the CST path (production-triple test)" );

	ConcurrencyTestController controller( *pJob, proof, /*simulatedRenderMs*/15 );
	controller.Start();   // genuinely live interactive loop to race against

	std::atomic<bool> stop{ false };
	std::atomic<int>  productionRuns{ 0 };
	std::atomic<int>  agentRuns{ 0 };

	// Production submissions, one at a time, sync (mirrors the GUI's
	// "rasterize is blocking, call it from a background thread" shape).
	// A short gap between submissions is deliberate: SubmitProductionRenderSync
	// (like any SubmitAgentRenderSync caller) registers a FAIR-QUEUE ticket
	// that refuses any NEW async submission for as long as it is
	// outstanding -- a production loop with NO gap would keep a ticket
	// registered almost continuously and starve the agent thread below by
	// construction of the fairness policy (sync waiters are owed the next
	// free slot ahead of async submitters), which would defeat this test's
	// OWN "both classes actually ran" sanity check for a reason that has
	// nothing to do with the concurrency invariant under test.
	std::thread productionThread( [&]() {
		while( !stop.load( std::memory_order_acquire ) )
		{
			SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
			const bool ok = controller.SubmitProductionRenderSync(
				[&]() { productionRuns.fetch_add( 1, std::memory_order_acq_rel ); pJob->Rasterize(); },
				String( "gui_production_probe" ), &jobId, /*queueTimeoutMs*/ 2000 );
			(void)ok;   // refusals are expected under contention; only concurrency matters here
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
	} );

	// Agent-render spam, exactly like (c)'s race-closure thread.
	std::thread agentThread( [&]() {
		while( !stop.load( std::memory_order_acquire ) )
		{
			SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
			controller.SubmitAgentRenderAsync(
				[&]() { agentRuns.fetch_add( 1, std::memory_order_acq_rel ); pJob->Rasterize(); },
				String( "agent_probe" ), &jobId );
			std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
		}
	} );

	// Interactive scrub thread, exactly like (c).
	std::thread scrubThread( [&]() {
		double t = 0.0;
		while( !stop.load( std::memory_order_acquire ) )
		{
			controller.OnTimeScrub( t );
			t += 0.01;
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
	} );

	std::this_thread::sleep_for( std::chrono::milliseconds( 600 ) );
	stop.store( true, std::memory_order_release );
	productionThread.join();
	agentThread.join();
	scrubThread.join();

	controller.Stop();

	const int maxConcurrency = proof.maxObserved.load( std::memory_order_acquire );
	std::printf( "  [production-triple] production runs=%d agent runs=%d max observed concurrency=%d\n",
	             productionRuns.load(), agentRuns.load(), maxConcurrency );
	Check( productionRuns.load( std::memory_order_acquire ) > 0, "at least one production submission actually ran its closure" );
	Check( agentRuns.load( std::memory_order_acquire ) > 0, "at least one agent submission actually ran its closure" );
	Check( maxConcurrency == 1,
	       "MONEY ASSERTION: max observed concurrency across Production + AgentPreview + Interactive is EXACTLY 1 -- "
	       "SubmitProductionRenderSync routes through the SAME single-slot coordinator as agent renders, so all three "
	       "classes are mutually exclusive inside the shared render critical section." );

	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (u) production triple concurrency proof: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (u continued) RED-PROVE: a DIRECT, uncoordinated Job::Rasterize() call
//     (the pre-S4 GUI shape -- calling straight into Rasterize() with no
//     park at all) racing the SAME live interactive loop DOES observe
//     concurrency 2.  This is the exact gap SubmitProductionRenderSync
//     closes; demonstrating it here (rather than just trusting the
//     positive proof above) proves the fixed path is actually doing
//     something -- a coordinator that vacuously never contends would
//     also show maxConcurrency==1 above for the wrong reason.
//////////////////////////////////////////////////////////////////////
static void RunDirectRasterizeRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (u) RED-PROVE: direct uncoordinated Rasterize() call hits concurrency 2 ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_production_direct_redprove.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the direct-rasterize red-prove scene to a temp file" );

	ConcurrencyProof proof;
	ConcurrencyProofJob* pJob = new ConcurrencyProofJob( proof );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ConcurrencyProofJob loads the native-v7 scene via the CST path (direct-rasterize red-prove test)" );

	ConcurrencyTestController controller( *pJob, proof, /*simulatedRenderMs*/15 );
	controller.Start();   // genuinely live interactive loop to race against

	std::atomic<bool> stop{ false };
	// Deliberately bypasses SceneEditController entirely -- this is
	// exactly what RISEBridge.mm's -rasterize / RenderEngine.cpp's
	// startRender called BEFORE this slice: mJob->Rasterize() straight
	// from the platform shell's own thread, no coordinator involved.
	std::thread directThread( [&]() {
		while( !stop.load( std::memory_order_acquire ) )
		{
			pJob->Rasterize();
		}
	} );
	std::thread scrubThread( [&]() {
		double t = 0.0;
		while( !stop.load( std::memory_order_acquire ) )
		{
			controller.OnTimeScrub( t );
			t += 0.01;
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
	} );

	std::this_thread::sleep_for( std::chrono::milliseconds( 600 ) );
	stop.store( true, std::memory_order_release );
	directThread.join();
	scrubThread.join();

	controller.Stop();

	const int maxConcurrency = proof.maxObserved.load( std::memory_order_acquire );
	std::printf( "  [direct-rasterize-red-prove] max observed concurrency: %d\n", maxConcurrency );
	Check( maxConcurrency >= 2,
	       "RED-PROVE: a direct, uncoordinated Job::Rasterize() call racing the live interactive loop DOES observe "
	       "concurrency >= 2 -- this is the pre-S4 gap (both RISEBridge.mm and RenderEngine.cpp called Rasterize() "
	       "directly with no park at all). Contrast with (u)'s positive proof, where routing the SAME kind of call "
	       "through SubmitProductionRenderSync keeps max observed concurrency at exactly 1." );

	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (u) direct-rasterize RED-PROVE: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (v) Progress composition: a production render's composed callback
//     receives progress ticks AND aborts promptly when EITHER the
//     controller's own cancel trips (Stop()) OR the caller-supplied
//     (GUI-side) cancel closure trips.  Mirrors the RunProductionRender-
//     ThroughController shape both platform shells use: SetInner points
//     the controller's mCancelProgress at a test "GUI" callback for the
//     render's duration.
//////////////////////////////////////////////////////////////////////
class SlowProgressJob : public Job
{
public:
	SlowProgressJob() : Job() {}
	bool Rasterize() override
	{
		// Report a handful of progress ticks through whatever callback is
		// installed, honouring its cancel return -- mirrors a real
		// rasterizer's block-fetch loop polling IProgressCallback between
		// blocks.
		IProgressCallback* cb = GetProgress();
		for( int i = 0; i < 50; ++i )
		{
			if( cb )
			{
				const bool keepGoing = cb->Progress( static_cast<double>( i ), 50.0 );
				if( !keepGoing ) { mAborted = true; return false; }
			}
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
		return true;
	}
	bool mAborted = false;
};

static void RunProductionProgressCompositionTest()
{
	std::printf( "=== AgentRenderAsyncTest: (v) production render progress/cancel composition ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_production_progress.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the production-progress scene to a temp file" );

	// ---- (v-1) GUI-side ("inner") progress ticks are forwarded, and a
	//      GUI-side cancel (the closure returning false) aborts promptly.
	{
		SlowProgressJob* pJob = new SlowProgressJob();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowProgressJob loads the native-v7 scene via the CST path (progress test, v-1)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		std::atomic<int>  guiTicks{ 0 };
		std::atomic<bool> guiCancel{ false };
		class TestGuiCallback : public IProgressCallback
		{
		public:
			std::atomic<int>*  ticks;
			std::atomic<bool>* cancelFlag;
			bool Progress( const double, const double ) override
			{
				ticks->fetch_add( 1, std::memory_order_acq_rel );
				return !cancelFlag->load( std::memory_order_acquire );
			}
			void SetTitle( const char* ) override {}
		} guiCb;
		guiCb.ticks = &guiTicks;
		guiCb.cancelFlag = &guiCancel;

		// Mirrors RunProductionRenderThroughController: point the
		// coordinator's own mCancelProgress at the GUI callback for the
		// render's duration, install it on the Job, run, restore.
		auto* coordProgress = static_cast<CancellableProgressCallback*>( controller.AgentRenderProgress() );
		coordProgress->SetInner( &guiCb );

		SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
		const bool submitted = controller.SubmitProductionRenderSync(
			[&]() {
				pJob->SetProgress( coordProgress );
				pJob->Rasterize();
				pJob->SetProgress( nullptr );
			},
			String( "progress_probe_v1" ), &jobId );
		coordProgress->SetInner( nullptr );

		Check( submitted, "the production submission is accepted (v-1)" );
		Check( guiTicks.load( std::memory_order_acquire ) > 0,
		       "the GUI-side (inner) callback receives progress ticks forwarded through the coordinator's composed callback" );
		Check( !pJob->mAborted, "with no cancel requested, the render is NOT aborted (control for v-1)" );

		controller.Stop();
		pJob->release();
	}

	// ---- (v-2) the controller's OWN cancel (Stop()) aborts a production
	//      render promptly, even with a GUI-side callback that never
	//      itself requests cancel.
	{
		SlowProgressJob* pJob = new SlowProgressJob();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowProgressJob loads the native-v7 scene via the CST path (progress test, v-2)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		class NeverCancelCallback : public IProgressCallback
		{
		public:
			bool Progress( const double, const double ) override { return true; }
			void SetTitle( const char* ) override {}
		} neverCancelCb;

		auto* coordProgress = static_cast<CancellableProgressCallback*>( controller.AgentRenderProgress() );
		coordProgress->SetInner( &neverCancelCb );

		std::thread productionThread( [&]() {
			SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
			controller.SubmitProductionRenderSync(
				[&]() {
					pJob->SetProgress( coordProgress );
					pJob->Rasterize();
					pJob->SetProgress( nullptr );
				},
				String( "progress_probe_v2" ), &jobId );
		} );

		// Give the render a moment to genuinely start (SlowProgressJob
		// sleeps 5ms/tick for up to 250ms total).
		std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );

		const bool stoppedPromptly = RunWatchdogged( "Stop() during a production render returns promptly", 3000, [&]() {
			controller.Stop();
		} );
		Check( stoppedPromptly, "Stop() during an in-flight production render returns within the watchdog bound (v-2)" );

		productionThread.join();
		coordProgress->SetInner( nullptr );

		Check( pJob->mAborted,
		       "MONEY ASSERTION (v-2): the controller's OWN cancel (Stop(), via CancelAgentRender_) aborts the production render "
		       "even though the GUI-side inner callback never itself requested cancel -- CancellableProgressCallback::Progress "
		       "refuses the instant EITHER cancel source trips." );

		pJob->release();
	}

	std::remove( scenePath.c_str() );

	std::printf( "=== (v) production progress/cancel composition: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (w) Queue semantics + status record: a production submit while an
//     agent render runs waits (ticket) then runs; an agent submit while
//     a production render runs is refused (single-slot, matching the
//     existing preview-occupant refusal semantics); the status record
//     shows RenderClass::Production + the submitted clientLabel while a
//     production render occupies the slot.
//////////////////////////////////////////////////////////////////////
static void RunProductionQueueSemanticsTest()
{
	std::printf( "=== AgentRenderAsyncTest: (w) production queue semantics + status record ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_production_queue.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the production-queue scene to a temp file" );

	// ---- (w-1) status record: Production class + clientLabel visible
	//      while a production render occupies the slot.
	{
		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (queue test, w-1)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		std::atomic<bool> releaseGate{ false };
		std::thread productionThread( [&]() {
			SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
			controller.SubmitProductionRenderSync(
				[&]() {
					while( !releaseGate.load( std::memory_order_acquire ) ) {
						std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
					}
				},
				String( "status_probe_w1" ), &jobId );
		} );

		SceneEditController::RenderJobStatus st;
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
			while( std::chrono::steady_clock::now() < deadline ) {
				st = controller.CurrentRenderJob();
				if( st.active && st.renderClass == SceneEditController::RenderClass::Production ) break;
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			}
		}
		Check( st.active && st.renderClass == SceneEditController::RenderClass::Production,
		       "MONEY ASSERTION (w-1): the status record shows renderClass==Production while a production render occupies the slot" );
		Check( st.clientLabel == "status_probe_w1", "the status record echoes the clientLabel a production submission passed in" );

		releaseGate.store( true, std::memory_order_release );
		productionThread.join();

		controller.Stop();
		pJob->release();
	}

	// ---- (w-2) production submit while an agent render runs: waits
	//      (ticket), then runs once the agent render completes.
	{
		Job* pJob = new Job();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (queue test, w-2)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		std::atomic<bool> agentReleaseGate{ false };
		std::atomic<bool> agentRunning{ false };
		SceneEditController::RenderJobId agentJobId = SceneEditController::kInvalidRenderJobId;
		const bool agentAccepted = controller.SubmitAgentRenderAsync(
			[&]() {
				agentRunning.store( true, std::memory_order_release );
				while( !agentReleaseGate.load( std::memory_order_acquire ) ) {
					std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
				}
			},
			String( "agent_holds_slot_w2" ), &agentJobId );
		Check( agentAccepted, "the agent render is accepted and occupies the slot (w-2)" );

		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
			while( !agentRunning.load( std::memory_order_acquire ) && std::chrono::steady_clock::now() < deadline ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			}
		}
		Check( agentRunning.load( std::memory_order_acquire ), "the agent render is genuinely running before we submit the production render (w-2)" );

		std::atomic<bool> productionRan{ false };
		std::atomic<bool> productionSubmitReturned{ false };
		std::thread productionThread( [&]() {
			SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
			const bool ok = controller.SubmitProductionRenderSync(
				[&]() { productionRan.store( true, std::memory_order_release ); },
				String( "production_waits_w2" ), &jobId, /*queueTimeoutMs*/ 5000 );
			Check( ok, "the production submission is EVENTUALLY accepted once the agent render's slot frees (w-2)" );
			productionSubmitReturned.store( true, std::memory_order_release );
		} );

		// While the agent render still holds the slot, the production
		// submission must NOT have run yet (it's fairly queued behind the
		// occupant, not refused outright the way SubmitAgentRenderAsync
		// would be -- SubmitProductionRenderSync forwards to
		// SubmitAgentRenderSync's fairness-wait machinery).
		std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
		Check( !productionRan.load( std::memory_order_acquire ),
		       "MONEY ASSERTION (w-2a): the production submission has NOT run yet while the agent render still holds the slot -- "
		       "it WAITS (fairness ticket), it is not refused outright" );

		// A concurrent agent submit while production waits its turn is
		// refused outright (SubmitAgentRenderAsync's own fair-queue
		// refusal: a waiting sync caller -- here, the production submit's
		// underlying SubmitAgentRenderSync call -- must not be jumped by a
		// new async submitter).
		SceneEditController::RenderJobId cutInJobId = SceneEditController::kInvalidRenderJobId;
		const bool cutInAccepted = controller.SubmitAgentRenderAsync(
			[&]() {}, String( "cut_in_attempt_w2" ), &cutInJobId );
		Check( !cutInAccepted,
		       "a NEW agent submission while the production submit is fairly queued behind the running agent render is REFUSED "
		       "(fair-queue semantics -- an async submitter cannot jump a waiting caller's turn)" );

		// Release the agent render; the production submission should then
		// proceed and run.
		agentReleaseGate.store( true, std::memory_order_release );
		productionThread.join();

		Check( productionSubmitReturned.load( std::memory_order_acquire ), "the production submission call returns" );
		Check( productionRan.load( std::memory_order_acquire ),
		       "MONEY ASSERTION (w-2b): once the agent render's slot frees, the queued production submission proceeds and runs its closure" );

		controller.Stop();
		pJob->release();
	}

	std::remove( scenePath.c_str() );

	std::printf( "=== (w) production queue semantics: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (x) Stop()-during-production: unblocks/refuses honestly, no hang.
//     Mirrors the existing (l) RunQueuedSyncWaiterUnblocksOnStopTest /
//     (m) RunStopCancelsInFlightAgentRenderTest shape, but for a
//     Production-class submission via SubmitProductionRenderSync.
//////////////////////////////////////////////////////////////////////
static void RunStopDuringProductionTest()
{
	std::printf( "=== AgentRenderAsyncTest: (x) Stop() during an in-flight production render unblocks honestly, no hang ===\n" );

	// A LARGE sample count so the render has plenty of progress-callback
	// checkpoints to actually observe a mid-flight cancel at -- mirrors (i)
	// RunStopCancelsInFlightAgentRenderTest's heavy scene exactly.  A tiny
	// sample count (or a closure that merely sleeps with no progress hook
	// installed at all) could "pass" for the WRONG reason -- the render
	// either finishes naturally before Stop() acts, or never has anything
	// for CancelAgentRender_'s tripped flag to reach, since nothing except
	// the installed progress hook consults it.  Installing that hook (via
	// AgentRenderProgress(), exactly like RunProductionRenderThroughController
	// does in the platform shells) inside the submitted closure is the
	// point of this test, not incidental.
	const std::string heavyScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4096\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 96\n\theight 96\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
		"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
		"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
		"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";
	const std::string scenePath = WriteTemp( "rise_agent_production_stop_scene.RISEscene", heavyScene );
	Check( !scenePath.empty(), "wrote the heavy production-stop scene to a temp file" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the heavy native-v7 scene via the CST path (production-stop test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	std::atomic<bool> productionSubmitReturned{ false };
	std::thread productionThread( [&]() {
		SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
		controller.SubmitProductionRenderSync(
			[&]() {
				// Mirrors RunProductionRenderThroughController: install the
				// controller's own cancel-aware progress hook before
				// Rasterize() so CancelAgentRender_ (which Stop() calls) has
				// something downstream to actually abort against, then
				// restore afterward.
				IProgressCallback* coordProgress = controller.AgentRenderProgress();
				pJob->SetProgress( coordProgress );
				pJob->Rasterize();
				pJob->SetProgress( nullptr );
			},
			String( "production_stop_probe" ), &jobId );
		productionSubmitReturned.store( true, std::memory_order_release );
	} );

	// Let the heavy render get properly under way before we pull the rug out.
	std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );

	const bool stoppedPromptly = RunWatchdogged( "Stop() during an in-flight production render returns promptly", 5000, [&]() {
		controller.Stop();
	} );
	Check( stoppedPromptly,
	       "MONEY ASSERTION: Stop() during an in-flight production render (submitted via SubmitProductionRenderSync) returns within "
	       "the watchdog bound -- it does not hang waiting out the render's full natural duration, mirroring CancelAgentRender_'s "
	       "existing behaviour for AgentPreview-class renders (see (i) RunStopCancelsInFlightAgentRenderTest)." );

	// The submitting thread must also unblock (Stop() releases queued/
	// running slot occupants honestly -- see SubmitAgentRenderAsync_Locked's
	// Stop() refusal + the worker's own cancel-and-drain).
	const bool submitReturnedPromptly = RunWatchdogged( "the production submit call itself returns after Stop()", 3000, [&]() {
		productionThread.join();
	} );
	Check( submitReturnedPromptly, "the SubmitProductionRenderSync call unblocks (join returns) after Stop(), no hang" );
	Check( productionSubmitReturned.load( std::memory_order_acquire ), "the production submission call did in fact return" );

	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (x) Stop() during production: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (y) Fix-round S4-1 RED-PROVE: SceneEditController::RunProductionRenderComposed
//     RAII-resets mCancelProgress's `inner` back to null on a throw out of
//     `doRasterize`, so a LATER agent render (which installs the SAME
//     mCancelProgress on the Job -- see AgentSession::doRenderWork) never
//     forwards ticks through a dangling pointer to the earlier, now-
//     destroyed production-side counting callback.
//
//     Pre-fix shape (both platform shells, byte-identical before this
//     fix): `coordProgress->SetInner(prior); ... result = doRasterize();
//     /* THROW POINT */ ... coordProgress->SetInner(nullptr);` -- the
//     final SetInner(nullptr) sat as a plain statement AFTER doRasterize(),
//     with only the JOB's progress SLOT (not mCancelProgress's `inner`)
//     covered by a guard.  A throw out of doRasterize() skipped it,
//     leaving mCancelProgress.mInner pointed at `prior` -- here, a
//     stack-local counting callback that is destroyed the instant this
//     test function's scope holding it unwinds past the throw.  The next
//     render that installs mCancelProgress on the Job (an agent render,
//     or the interactive loop) then forwards ticks through that freed
//     pointer -- a recurring UAF, not a one-shot.
//////////////////////////////////////////////////////////////////////
static void RunInnerResetOnThrowRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (y) RED-PROVE: RunProductionRenderComposed resets mCancelProgress's inner on a throw ===\n" );

	// A dedicated (larger) scene, NOT the shared 24x24 `kScene` -- this test
	// needs the real progress-callback dispatcher to actually fire multiple
	// ticks (RasterizeBlockDispatcher::GetNextBlock only calls Progress()
	// once tile idx>0, so a scene small enough to collapse to a single tile
	// would give a false "0 ticks" reading that has nothing to do with the
	// fix under test).  128x128 with a low sample count comfortably clears
	// the ~8px minimum tile size on any thread count while staying fast.
	const std::string innerResetScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 128\n\theight 128\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
		"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
		"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
		"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

	const std::string scenePath = WriteTemp( "rise_agent_async_innerreset.RISEscene", innerResetScene );
	Check( !scenePath.empty(), "wrote the inner-reset scene to a temp file" );

	ThrowingRasterizeJob* pJob = new ThrowingRasterizeJob();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "ThrowingRasterizeJob loads the native-v7 scene via the CST path (inner-reset test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	// A counting IProgressCallback confined to THIS scope -- mirrors the
	// live reviewer's probe.  If mCancelProgress.mInner is left dangling
	// at this object after it goes out of scope below, any LATER render
	// that installs mCancelProgress on the Job will write through freed
	// memory the instant Progress() is called (UBSan/ASan would flag it
	// immediately; even without a sanitizer, the tick counter silently
	// stops being a safe write target).
	{
		std::atomic<int> countingTicks{ 0 };
		class CountingCallback : public IProgressCallback
		{
		public:
			std::atomic<int>* ticks;
			bool Progress( const double, const double ) override
			{
				ticks->fetch_add( 1, std::memory_order_acq_rel );
				return true;
			}
			void SetTitle( const char* ) override {}
		} countingCb;
		countingCb.ticks = &countingTicks;

		// Drive the EXACT composition RISEBridge.mm / RenderEngine.cpp use,
		// through the shared, tested implementation -- with the counting
		// callback as BOTH the Job's pre-existing ("prior") progress hook
		// AND the explicit `guiProgress` parameter (mirrors macOS, whose
		// persistent BlockProgressCallback install means the two happen to
		// be the same value -- see RunProductionRenderComposed's header doc
		// for why Windows is NOT required to keep them equal).
		pJob->SetProgress( &countingCb );
		bool threw = false;
		try
		{
			SceneEditController::RunProductionRenderComposed(
				*pJob, &controller, String( "inner_reset_probe" ), &countingCb,
				[pJob]() -> bool { return pJob->Rasterize(); } );
		}
		catch( ... )
		{
			threw = true;
		}
		Check( threw, "the composed production render propagates the simulated OIDN-class throw to the caller" );
		std::printf( "    [inner-reset] countingTicks=%d\n", countingTicks.load( std::memory_order_acquire ) );
		Check( countingTicks.load( std::memory_order_acquire ) > 0,
		       "the counting callback DID receive at least one tick during the throwing render (control: composition worked before the throw)" );

		pJob->SetProgress( nullptr );
		// countingCb (and countingTicks) go out of scope here.  If
		// mCancelProgress.mInner still points at &countingCb, the very next
		// render that installs mCancelProgress on the Job writes through a
		// freed stack object.
	}

	// A follow-up AGENT render (installs mController->AgentRenderProgress()
	// == the SAME mCancelProgress instance the production path just used --
	// see AgentSession::doRenderWork) must NOT observe a stale `inner` left
	// over from the destroyed counting callback above.  We can't directly
	// assert "no UAF happened" from outside, but we CAN assert the only
	// externally-observable channel a stale inner would use: the render
	// completes normally and cleanly, and mCancelProgress's inner is null
	// immediately before this render installs its own hook (proving the
	// composed helper's teardown ran).
	pJob->SetThrowOnRasterize( false );
	auto* coordProgress = static_cast<CancellableProgressCallback*>( controller.AgentRenderProgress() );
	Check( pJob->GetProgress() == nullptr, "the Job's progress hook is null again before the follow-up agent render (production teardown completed)" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the Job for the follow-up agent render" );
	if( session )
	{
		session->AttachController( &controller );
		AgentRenderParams p;
		const AgentRenderResult r = session->Render( p );
		std::printf( "    [inner-reset] follow-up agent render ok=%d message=\"%s\"\n", r.ok ? 1 : 0, r.message.c_str() );
		Check( r.ok,
		       "RED-PROVE MONEY ASSERTION: a follow-up agent render (sharing mCancelProgress with the earlier throwing production "
		       "render via AgentRenderProgress()) completes successfully with no crash/UAF -- pre-fix, mCancelProgress.mInner "
		       "would still point at the destroyed counting callback from the block above, and this render's progress ticks would "
		       "write through freed stack memory instead of being silently absorbed by a null inner." );
		session->AttachController( nullptr );
	}

	// Belt-and-suspenders direct assertion: mCancelProgress's inner is
	// unconditionally null right now (Reset() only clears the cancel flag,
	// never `inner` -- see CancellableProgressCallback::Reset), reached via
	// the same public accessor AgentRenderProgress() already exposes, by
	// re-running the composed helper once more with a fresh, still-in-scope
	// counting callback and confirming teardown leaves it uninstalled.
	{
		std::atomic<int> secondTicks{ 0 };
		class CountingCallback2 : public IProgressCallback
		{
		public:
			std::atomic<int>* ticks;
			bool Progress( const double, const double ) override
			{
				ticks->fetch_add( 1, std::memory_order_acq_rel );
				return true;
			}
			void SetTitle( const char* ) override {}
		} countingCb2;
		countingCb2.ticks = &secondTicks;

		pJob->SetProgress( &countingCb2 );
		const bool ok = SceneEditController::RunProductionRenderComposed(
			*pJob, &controller, String( "inner_reset_probe_2" ), &countingCb2,
			[pJob]() -> bool { return pJob->Rasterize(); } );
		Check( ok, "a normal (non-throwing) composed production render still succeeds after the earlier throw" );
		pJob->SetProgress( nullptr );

		Check( coordProgress->IsCancelRequested() == false,
		       "mCancelProgress's cancel flag is clear after a normal completion (control: Reset() semantics unaffected by this fix)" );
	}

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (y) inner reset on throw: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (z) Fix-round 2 RED-PROVE: SceneEditController::RunProductionRenderComposed
//     forwards ticks to an EXPLICIT `guiProgress` callback even when that
//     callback is NOT pre-installed on the Job -- the exact Windows shape.
//     RenderEngine.cpp's startRender/startAnimationRender deliberately skip
//     `m_job->SetProgress(progressCb)` when a controller is attached (so the
//     coordinator owns the Job's progress slot for the render's duration),
//     which means job.GetProgress() reads nullptr at composition time.  The
//     pre-fix implementation derived BOTH "inner" (what receives ticks) and
//     "prior" (what gets restored) from that single job.GetProgress() read
//     -- so on Windows inner was nullptr for the whole render,
//     ProgressCallbackAdapter::Progress was never called, and the Windows
//     Cancel button + progress/ETA UI went dead for every coordinator-routed
//     production render.  This test drives the fixed, explicit-parameter
//     signature with the callback withheld from the Job (mirroring
//     Windows), and would read zero ticks on the pre-fix code path (which
//     only had a way to receive job.GetProgress(), never a caller-supplied
//     value).
//////////////////////////////////////////////////////////////////////
static void RunExplicitGuiProgressNotInstalledOnJobRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (z) RED-PROVE: explicit guiProgress reaches the render even when NOT installed on the Job (Windows shape) ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_production_windows_shape.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the windows-shape scene to a temp file" );

	// A counting + cancellable callback that mirrors ProgressCallbackAdapter
	// exactly: Progress() ticks a counter and returns `!cancelRequested`.
	class CountingCancellableCallback : public IProgressCallback
	{
	public:
		std::atomic<int>*  ticks;
		std::atomic<bool>* cancelRequested;
		bool Progress( const double, const double ) override
		{
			ticks->fetch_add( 1, std::memory_order_acq_rel );
			return !cancelRequested->load( std::memory_order_acquire );
		}
		void SetTitle( const char* ) override {}
	};

	// ---- (z-1) progress ticks: the callback receives ticks during the
	//      render despite never being installed on the Job.
	{
		SlowProgressJob* pJob = new SlowProgressJob();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowProgressJob loads the native-v7 scene via the CST path (windows-shape test, z-1)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		std::atomic<int>  guiTicks{ 0 };
		std::atomic<bool> guiCancel{ false };
		CountingCancellableCallback guiCb;
		guiCb.ticks = &guiTicks;
		guiCb.cancelRequested = &guiCancel;

		// The Windows shape: the GUI callback is passed as the explicit
		// `guiProgress` parameter WITHOUT ever calling pJob->SetProgress()
		// first -- job.GetProgress() reads nullptr throughout, exactly as
		// it does on Windows when a controller is attached (startRender
		// deliberately skips the direct install; see that method's
		// comment).
		Check( pJob->GetProgress() == nullptr,
		       "the Job's progress hook is null before the render -- guiCb was never installed on the Job (Windows shape control)" );

		const bool ok = SceneEditController::RunProductionRenderComposed(
			*pJob, &controller, String( "windows_shape_probe_z1" ), &guiCb,
			[pJob]() -> bool { return pJob->Rasterize(); } );

		Check( ok, "the composed production render completes successfully (z-1)" );
		std::printf( "    [windows-shape] guiTicks=%d\n", guiTicks.load( std::memory_order_acquire ) );
		Check( guiTicks.load( std::memory_order_acquire ) > 0,
		       "RED-PROVE MONEY ASSERTION (z-1): the explicit guiProgress callback receives progress ticks during the render even "
		       "though it was never installed on the Job -- pre-fix, RunProductionRenderComposed had no `guiProgress` parameter at "
		       "all and instead composed in whatever job.GetProgress() returned (nullptr here), so this callback would receive "
		       "ZERO ticks -- the exact Windows Cancel/progress-UI regression this fix-round closes." );

		Check( pJob->GetProgress() == nullptr,
		       "the Job's progress hook is restored to nullptr (the honest in-slot prior) after completion (z-1)" );
		Check( static_cast<CancellableProgressCallback*>( controller.AgentRenderProgress() )->IsCancelRequested() == false,
		       "mCancelProgress's cancel flag is clear after a normal completion (control, z-1)" );

		controller.Stop();
		pJob->release();
	}

	// ---- (z-2) cancel-button parity: flipping the GUI callback's own
	//      cancel-return aborts the render promptly, even though that
	//      callback was never installed on the Job directly (the Cancel
	//      button's actual code path on Windows: ProgressCallbackAdapter
	//      returns !m_cancelFlag, and m_cancelFlag is flipped by the
	//      Cancel button handler independent of the coordinator).
	{
		SlowProgressJob* pJob = new SlowProgressJob();
		Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowProgressJob loads the native-v7 scene via the CST path (windows-shape test, z-2)" );

		SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
		controller.Start( /*suppressInitialRender=*/true );

		std::atomic<int>  guiTicks{ 0 };
		std::atomic<bool> guiCancel{ false };
		CountingCancellableCallback guiCb;
		guiCb.ticks = &guiTicks;
		guiCb.cancelRequested = &guiCancel;

		std::atomic<bool> renderDone{ false };
		bool renderResult = true;
		std::thread productionThread( [&]() {
			renderResult = SceneEditController::RunProductionRenderComposed(
				*pJob, &controller, String( "windows_shape_probe_z2" ), &guiCb,
				[pJob]() -> bool { return pJob->Rasterize(); } );
			renderDone.store( true, std::memory_order_release );
		} );

		// Give the render a moment to genuinely start ticking (SlowProgressJob
		// sleeps 5ms/tick for up to 50 ticks == 250ms total).
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
			while( guiTicks.load( std::memory_order_acquire ) == 0 &&
			       std::chrono::steady_clock::now() < deadline ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			}
		}
		Check( guiTicks.load( std::memory_order_acquire ) > 0, "the render has genuinely started ticking before we request cancel (z-2)" );

		// The Cancel-button path: flip the GUI callback's OWN cancel flag
		// (NOT the coordinator's Stop()/CancelAgentRender_) and confirm the
		// render aborts promptly -- this is the actual regression surface:
		// pre-fix, this callback was never wired into the composed
		// `inner` at all on the Windows shape, so this flag being flipped
		// would have had no effect on the render whatsoever.
		guiCancel.store( true, std::memory_order_release );

		const bool joinedPromptly = RunWatchdogged( "the render thread joins promptly after guiCancel is requested", 3000, [&]() {
			productionThread.join();
		} );
		Check( joinedPromptly, "the production render thread joins within the watchdog bound after guiCancel (z-2)" );

		Check( pJob->mAborted,
		       "RED-PROVE MONEY ASSERTION (z-2): flipping the explicit guiProgress callback's OWN cancel return aborts the "
		       "production render promptly -- this is the Windows Cancel button's exact code path "
		       "(ProgressCallbackAdapter::Progress returning !m_cancelFlag). Pre-fix, guiProgress had no route into the composed "
		       "callback chain on the Windows shape (job.GetProgress() read nullptr), so this cancel signal would never reach the "
		       "render at all." );
		Check( !renderResult, "RunProductionRenderComposed reports failure for the cancelled render (z-2)" );

		Check( pJob->GetProgress() == nullptr,
		       "the Job's progress hook is restored to nullptr after the cancelled render completes (z-2)" );
		Check( static_cast<CancellableProgressCallback*>( controller.AgentRenderProgress() )->IsCancelRequested() == false,
		       "mCancelProgress's OWN cancel flag is untouched by a guiProgress-side cancel -- only the inner's return value "
		       "carried the abort signal, matching CancellableProgressCallback::Progress's OR-of-both-sources semantics (control, z-2)" );

		controller.Stop();
		pJob->release();
	}

	std::remove( scenePath.c_str() );

	std::printf( "=== (z) explicit guiProgress not installed on Job: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (aa) Fix-round-3 RED-PROVE: SceneEditController::RunProductionRenderComposed
//     REFUSES a production render outright (no fallback call to
//     `doRasterize`) when SubmitProductionRenderSync itself is refused --
//     e.g. an agent render already occupies the single slot. Pre-fix, the
//     refused branch called `doRasterize()` directly on the CALLER's
//     thread with no park/coordination at all -- exactly the pre-S4,
//     uncoordinated shape, and reachable in NORMAL use whenever a user's
//     production render loses the slot race against an in-flight agent
//     render. That fallback could run CONCURRENTLY with the occupant
//     (two threads inside Rasterize() at once -- invariant I1, the thing
//     S4 exists to prevent) and carried zero progress/cancel wiring for
//     `guiProgress`.
//
//     This test occupies the slot with a slow agent render, then calls
//     RunProductionRenderComposed with a short `queueTimeoutMs` (via a
//     thin SubmitProductionRenderSync probe) so the production submission
//     is refused while the occupant is still running, and asserts:
//       (1) the refused call returns false,
//       (2) doRasterize (counted via ConcurrencyProofJob::RasterizeCallCount)
//           was NEVER invoked by the refused call -- pre-fix this would be
//           1 (and concurrent with the occupant, i.e. the SAME gap
//           RunDirectRasterizeRedProveTest above demonstrates generically),
//       (3) the occupant completes unharmed (its own call count reaches
//           the expected value, no crash/UAF).
//     A trailing control confirms the NO-CONTROLLER shape (the wrapper's
//     own null-check at the very top of RunProductionRenderComposed) is
//     UNCHANGED -- it still calls doRasterize directly.
//////////////////////////////////////////////////////////////////////
static void RunRefusedProductionSubmitNoFallbackRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (aa) RED-PROVE: refused production submit does not fall back to an uncoordinated Rasterize() ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_production_refused_fallback.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the refused-fallback scene to a temp file" );

	// A dedicated slow-and-counting Job: the occupant needs to hold the
	// slot comfortably longer than the production submission's short
	// `queueTimeoutMs` below (the real render on this tiny 24x24 scene
	// completes in a few ms, which would let the fairness wait succeed
	// before the timeout and defeat this test's own premise -- unlike
	// ConcurrencyProofJob's fixed 10ms holds, this class's sleep is
	// tunable). Still participates in the shared ConcurrencyProof counter
	// and exposes a plain call counter, same contract as
	// ConcurrencyProofJob::RasterizeCallCount.
	class SlowCountingConcurrencyProofJob : public Job
	{
	public:
		SlowCountingConcurrencyProofJob( ConcurrencyProof& proof, unsigned int sleepMs )
		: Job(), mProof( proof ), mSleepMs( sleepMs ) {}
		bool Rasterize() override
		{
			mProof.Enter();
			mRasterizeCallCount.fetch_add( 1, std::memory_order_acq_rel );
			std::this_thread::sleep_for( std::chrono::milliseconds( mSleepMs ) );
			const bool ok = Job::Rasterize();
			mProof.Leave();
			return ok;
		}
		int RasterizeCallCount() const { return mRasterizeCallCount.load( std::memory_order_acquire ); }
	private:
		ConcurrencyProof& mProof;
		unsigned int      mSleepMs;
		std::atomic<int>  mRasterizeCallCount{ 0 };
	};

	ConcurrencyProof proof;
	SlowCountingConcurrencyProofJob* pJob = new SlowCountingConcurrencyProofJob( proof, /*sleepMs*/ 800 );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowCountingConcurrencyProofJob loads the native-v7 scene via the CST path (refused-fallback test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	// Occupy the single slot with a slow AGENT render (mirrors the gap's
	// own framing: "a user production render losing the slot race
	// against an agent render").
	std::atomic<bool> occupantStarted{ false };
	std::atomic<bool> occupantDone{ false };
	SceneEditController::RenderJobId occupantJobId = SceneEditController::kInvalidRenderJobId;
	const bool occupantAccepted = controller.SubmitAgentRenderAsync(
		[&]() {
			occupantStarted.store( true, std::memory_order_release );
			pJob->Rasterize();
			occupantDone.store( true, std::memory_order_release );
		},
		String( "occupant_agent_render" ), &occupantJobId );
	Check( occupantAccepted, "the occupant agent render is accepted (slot is free at the start)" );

	// Wait until the occupant has genuinely entered Rasterize() before
	// attempting the production submission below -- otherwise a fast
	// scheduler could let the production call land before the occupant
	// claims the slot, which would defeat this test's own premise.
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( !occupantStarted.load( std::memory_order_acquire ) &&
		       std::chrono::steady_clock::now() < deadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( occupantStarted.load( std::memory_order_acquire ), "the occupant genuinely started running before the production submission attempt" );

	// The refused production submission: a short queueTimeoutMs so the
	// fairness wait gives up promptly while the occupant (an 800ms sleep
	// before the real Rasterize() call, comfortably longer than this
	// timeout) is still holding the slot.
	std::atomic<int>  guiTicks{ 0 };
	class CountingCallback : public IProgressCallback
	{
	public:
		std::atomic<int>* ticks;
		bool Progress( const double, const double ) override
		{
			ticks->fetch_add( 1, std::memory_order_acq_rel );
			return true;
		}
		void SetTitle( const char* ) override {}
	} guiCb;
	guiCb.ticks = &guiTicks;

	const int rasterizeCountBefore = pJob->RasterizeCallCount();

	bool productionResult = true;   // deliberately pre-set to the WRONG value; must become false
	const bool joinedPromptly = RunWatchdogged( "the refused production submission returns promptly", 3000, [&]() {
		productionResult = SceneEditController::RunProductionRenderComposed(
			*pJob, &controller, String( "refused_production_probe" ), &guiCb,
			[pJob]() -> bool { return pJob->Rasterize(); },
			/*queueTimeoutMs*/ 50 );
	} );
	Check( joinedPromptly, "the refused production submission does not hang (aa)" );

	Check( !productionResult,
	       "MONEY ASSERTION (aa-1): RunProductionRenderComposed returns false when SubmitProductionRenderSync is refused "
	       "(slot busy with the occupant agent render) -- honest refusal, no silent success." );

	const int rasterizeCountAfter = pJob->RasterizeCallCount();
	Check( rasterizeCountAfter == rasterizeCountBefore,
	       "MONEY ASSERTION (aa-2) RED-PROVE: doRasterize was NEVER invoked by the refused production submission -- "
	       "pre-fix, the refused branch called doRasterize() directly (an UNCOORDINATED extra Rasterize() call, "
	       "concurrent with the still-running occupant -- the exact invariant-I1 violation RunDirectRasterizeRedProveTest "
	       "demonstrates generically); post-fix the refused call returns false without ever touching doRasterize." );

	Check( guiTicks.load( std::memory_order_acquire ) == 0,
	       "no progress ticks were delivered to guiProgress by the refused submission (nothing was ever wired up, "
	       "since doRasterize never ran)" );

	// The occupant must complete unharmed -- the refused submission must
	// not have disturbed it in any way (no crash, no stolen slot).
	Check( controller.WaitForRenderJob( occupantJobId, 5000 ), "the occupant agent render eventually completes (unharmed by the refused submission)" );
	Check( occupantDone.load( std::memory_order_acquire ), "the occupant's closure ran to completion" );

	const int maxConcurrency = proof.maxObserved.load( std::memory_order_acquire );
	std::printf( "  [refused-fallback] rasterizeCountBefore=%d rasterizeCountAfter=%d occupant max observed concurrency=%d\n",
	             rasterizeCountBefore, rasterizeCountAfter, maxConcurrency );
	Check( maxConcurrency == 1, "the occupant alone never observes concurrency > 1 (no uncoordinated fallback ran alongside it)" );

	controller.Stop();
	pJob->release();

	// Control: the NO-CONTROLLER shape (the wrapper's own null-check at
	// the very top of RunProductionRenderComposed) is UNCHANGED -- with no
	// controller attached at all, doRasterize still runs directly. This is
	// the ONLY legitimate direct-render path; it is untouched by this fix.
	{
		SlowRasterizeJob* pNoCtrlJob = new SlowRasterizeJob();
		pNoCtrlJob->SetSleepMs( 5 );
		Check( pNoCtrlJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowRasterizeJob loads the native-v7 scene via the CST path (no-controller control, aa)" );

		std::atomic<int> noCtrlTicks{ 0 };
		CountingCallback noCtrlCb;
		noCtrlCb.ticks = &noCtrlTicks;

		bool ranDirectly = false;
		const bool ok = SceneEditController::RunProductionRenderComposed(
			*pNoCtrlJob, /*controller*/nullptr, String( "no_controller_probe" ), &noCtrlCb,
			[pNoCtrlJob, &ranDirectly]() -> bool { ranDirectly = true; return pNoCtrlJob->Rasterize(); } );
		Check( ranDirectly, "CONTROL: with NO controller attached, RunProductionRenderComposed still calls doRasterize directly (unchanged pre-S4 / headless shape)" );
		Check( ok, "CONTROL: the no-controller direct render completes successfully" );

		pNoCtrlJob->release();
	}

	std::remove( scenePath.c_str() );

	std::printf( "=== (aa) refused production submit no-fallback RED-PROVE: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (bb) Model-B F2 slice S4 fix round 4 RED-PROVE: mirrors the ACTUAL
//      app call order (RenderViewModel.swift's startRender /
//      startAnimationRender on macOS; MainWindow::onRender /
//      onRenderAnimation on Windows) --
//
//          controller.Start()
//          controller.StopInteractive()   // "pause the viewport"
//          RunProductionRenderComposed(...)   // the production submit
//
//      Pre-fix, the platform shells called the MONOLITHIC Stop() in the
//      middle step, which ALSO set mAgentRenderStop=true and permanently
//      joined mAgentRenderThread (spawned once, in the ctor; nothing
//      ever respawns it) -- so the production submit a few lines later
//      was refused with "SceneEditController: agent render submission
//      refused -- controller stopped.", surfacing to the user as
//      RunProductionRenderComposed's "production render submission to
//      the coordinator was refused (busy or stopped)" warning and NO
//      RENDER EVER RUNNING.  This test asserts the submit SUCCEEDS
//      post-fix (red-prove: FAILS on the pre-fix monolithic Stop()),
//      that the interactive loop can be restarted afterward (Start()
//      re-entry: mRunning CAS + thread respawn), and that a SECOND
//      production render on the SAME controller after that restart also
//      succeeds -- the exact "Render, then Render again" user flow the
//      bug report described.
//////////////////////////////////////////////////////////////////////
static void RunViewportPauseThenProductionSubmitRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (bb) RED-PROVE: app call order (Start -> StopInteractive -> production submit) succeeds ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_viewport_pause_production.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the viewport-pause-production scene to a temp file" );

	// SlowProgressJob (defined above, used by tests (v)/(z)): its
	// Rasterize() reports 50 explicit progress ticks through whatever
	// callback is installed, unlike a plain Job's real rasterizer on
	// this tiny 24x24/8spp scene, which can legitimately complete in a
	// single pass with zero *tile*-granularity progress callbacks --
	// that would make a tick-count assertion meaningless/flaky. Using
	// the same job class the house progress-composition tests already
	// rely on keeps the "the render genuinely ran doRasterize" proof
	// deterministic.
	SlowProgressJob* pJob = new SlowProgressJob();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "SlowProgressJob loads the native-v7 scene via the CST path (viewport-pause-production test)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );

	// Step 1: mirror the app's scene-load-time Start() (Mac: loadScene's
	// success path calls vb.start(); Windows: onStateChanged's
	// SceneLoaded branch calls m_viewportBridge->start()).
	controller.Start( /*suppressInitialRender=*/false );
	Check( controller.IsRunning(), "the interactive loop is running after the initial Start()" );

	// Step 2: mirror startRender()/onRender()'s viewport-pause call --
	// StopInteractive(), NOT the monolithic Stop().
	controller.StopInteractive();
	Check( !controller.IsRunning(), "StopInteractive() halts the interactive loop (IsRunning() false)" );

	// Step 3: the production submit itself -- RunProductionRenderComposed,
	// exactly what -[RISEBridge rasterize] / RenderEngine::startRender
	// call.
	std::atomic<int> guiTicks{ 0 };
	class CountingCallback : public IProgressCallback
	{
	public:
		std::atomic<int>* ticks;
		bool Progress( const double, const double ) override
		{
			ticks->fetch_add( 1, std::memory_order_acq_rel );
			return true;
		}
		void SetTitle( const char* ) override {}
	} guiCb;
	guiCb.ticks = &guiTicks;

	bool firstResult = false;
	const bool firstCompletedInTime = RunWatchdogged(
		"first production submit after StopInteractive()", 5000,
		[&]() {
			firstResult = SceneEditController::RunProductionRenderComposed(
				*pJob, &controller, String( "app_flow_production_1" ), &guiCb,
				[pJob]() -> bool { return pJob->Rasterize(); } );
		} );
	Check( firstCompletedInTime, "the first production submit does not hang" );
	Check( firstResult,
	       "MONEY ASSERTION (bb-1) RED-PROVE: RunProductionRenderComposed SUCCEEDS immediately after "
	       "Start()+StopInteractive() -- pre-fix (StopInteractive calling the monolithic Stop()) this "
	       "was refused with \"controller stopped\" and the render NEVER RAN." );
	Check( guiTicks.load( std::memory_order_acquire ) > 0, "the composed progress callback actually received ticks (the render genuinely ran doRasterize)" );
	Check( !pJob->mAborted, "the first production render was not aborted (control)" );

	// Step 4: mirror the app's post-render restart.  Real callers use
	// startSuppressingInitialRender (Mac's renderTask completion block;
	// Windows' onStateChanged renderEnded branch) so the just-finished
	// production image isn't immediately overwritten -- but THAT choice
	// deliberately parks the render thread until the next edit/gesture,
	// which would make "an interactive pass actually runs" untestable
	// without inventing a synthetic edit-injection hook.  Using the
	// ordinary Start() here (initial pass NOT suppressed) instead proves
	// the same re-entry claim Start()'s header doc makes -- "a subsequent
	// Start() renders normally" -- with a real, observable pass.
	const unsigned int countBeforeRestart = controller.ForTest_GetRenderCount();
	controller.Start( /*suppressInitialRender=*/false );
	Check( controller.IsRunning(), "the interactive loop restarts after StopInteractive() + a production render (Start() re-entry)" );

	bool sawNewPass = false;
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 3000 );
		while( std::chrono::steady_clock::now() < deadline )
		{
			if( controller.ForTest_GetRenderCount() != countBeforeRestart ) { sawNewPass = true; break; }
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
	}
	Check( sawNewPass, "the restarted interactive loop actually services a render pass (not just IsRunning() == true)" );

	// Step 5: a SECOND production render on the same controller -- the
	// "Render, then edit, then Render again" flow -- must ALSO succeed,
	// proving the fix isn't a one-shot fluke (e.g. some latent state only
	// happening to be right the first time).
	controller.StopInteractive();
	Check( !controller.IsRunning(), "StopInteractive() halts the interactive loop again for the second render" );

	bool secondResult = false;
	const bool secondCompletedInTime = RunWatchdogged(
		"second production submit after a second StopInteractive()", 5000,
		[&]() {
			secondResult = SceneEditController::RunProductionRenderComposed(
				*pJob, &controller, String( "app_flow_production_2" ), &guiCb,
				[pJob]() -> bool { return pJob->Rasterize(); } );
		} );
	Check( secondCompletedInTime, "the second production submit does not hang" );
	Check( secondResult,
	       "MONEY ASSERTION (bb-2): a SECOND production render on the same controller, after another "
	       "Start()/StopInteractive() cycle, also succeeds -- the fix is not a one-shot fluke." );
	Check( guiTicks.load( std::memory_order_acquire ) > 50, "the second production render also delivered fresh progress ticks (guiTicks advanced past the first render's 50)" );
	Check( !pJob->mAborted, "the second production render was not aborted (control)" );

	// Control (unchanged full-Stop() semantics): a subsequent call to the
	// REAL Stop() must still permanently retire the agent worker -- the
	// pre-existing (g)/(h) red-proves below cover this in detail; this
	// inline check is a narrower belt-and-braces assertion that THIS
	// controller, which has now been through StopInteractive() twice and
	// two production renders, still honors full Stop()'s contract.
	controller.Stop();
	Check( !controller.IsRunning(), "the interactive loop is stopped after the real Stop()" );

	SceneEditController::RenderJobId postStopId = SceneEditController::kInvalidRenderJobId;
	const bool postStopAccepted = controller.SubmitAgentRenderAsync(
		[]() { /* must never run */ }, String( "post_full_stop_after_bb" ), &postStopId );
	Check( !postStopAccepted,
	       "CONTROL: after the REAL Stop() (not StopInteractive()), a submission is still refused -- "
	       "full Stop()'s agent-worker-retirement contract is unchanged by this fix." );

	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (bb) viewport-pause-then-production-submit RED-PROVE: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (z2) Progress-slot ownership hardening (2026-07-11):
//     (a) Job::ClearProgressIfCurrent is a CAS -- "clear what *I*
//         installed" never stomps a callback some OTHER owner (an agent
//         render on the coordinator worker thread) put in the slot in
//         the meantime.  This is the API the GUI completion handlers
//         now use instead of an unconditional SetProgress(nullptr).
//     (b) a null-slot Rasterize() DETACHES the rasterizer's persistently
//         retained progress callback (Rasterizer::SetProgressCallback
//         keeps the raw pointer until the next call) instead of leaving
//         it aimed at the previous render's -- typically deleted --
//         callback.  Pre-fix, the second render below drove the OLD
//         callback again through the retained pointer: with real
//         callers (both GUIs delete their adapter after each render)
//         that retained pointer is a dormant use-after-free.
//////////////////////////////////////////////////////////////////////
static void RunProgressSlotAtomicClearTest()
{
	std::printf( "=== AgentRenderAsyncTest: (z2) progress-slot CAS clear + null-slot rasterize detach ===\n" );

	// kScene at 96x96 / 2 spp: the block dispatcher reports Progress()
	// only from the SECOND block on (RasterizeDispatchers.h `idx > 0`),
	// so the 24x24 shared scene -- a single block -- would fire zero
	// ticks and starve the z2-b control assertion.  96x96 yields a
	// multi-block grid at any plausible block size while staying fast.
	std::string sceneText( kScene );
	auto replaceOnce = []( std::string& s, const char* from, const char* to ) {
		const std::string::size_type at = s.find( from );
		Check( at != std::string::npos, std::string( "z2 scene tweak found its anchor: " ) + from );
		if( at != std::string::npos ) s.replace( at, std::strlen( from ), to );
	};
	replaceOnce( sceneText, "width 24", "width 96" );
	replaceOnce( sceneText, "height 24", "height 96" );
	replaceOnce( sceneText, "samples 8", "samples 2" );
	const std::string scenePath = WriteTemp( "rise_agent_progress_slot_clear.RISEscene", sceneText );
	Check( !scenePath.empty(), "wrote the progress-slot scene to a temp file (z2)" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (z2)" );

	class TickCountingCallback : public IProgressCallback
	{
	public:
		std::atomic<int> ticks{ 0 };
		bool Progress( const double, const double ) override
		{
			ticks.fetch_add( 1, std::memory_order_acq_rel );
			return true;
		}
		void SetTitle( const char* ) override {}
	};

	// ---- (z2-a) CAS protocol, deterministic single-thread proof.
	{
		TickCountingCallback cbA, cbB;
		pJob->SetProgress( &cbA );
		Check( !pJob->ClearProgressIfCurrent( &cbB ), "ClearProgressIfCurrent(<not the installed callback>) refuses (z2-a)" );
		Check( pJob->GetProgress() == &cbA, "...and leaves the installed callback untouched (z2-a)" );
		Check( pJob->ClearProgressIfCurrent( &cbA ), "ClearProgressIfCurrent(<the installed callback>) clears (z2-a)" );
		Check( pJob->GetProgress() == nullptr, "...and the slot reads null afterward (z2-a)" );
		Check( !pJob->ClearProgressIfCurrent( &cbA ), "a repeat ClearProgressIfCurrent on the now-null slot refuses (idempotent, z2-a)" );
		pJob->SetProgress( &cbA );
		Check( !pJob->ClearProgressIfCurrent( nullptr ), "ClearProgressIfCurrent(nullptr) is refused by contract (z2-a)" );
		Check( pJob->GetProgress() == &cbA, "...and a null `expected` leaves the installed callback untouched (z2-a)" );
		Check( pJob->ClearProgressIfCurrent( &cbA ), "cleanup clear after the null-contract probe succeeds (z2-a)" );

		// Install-side CAS twin (SetProgressIfCurrent, 2026-07-12 hardening):
		// unlike ClearProgressIfCurrent, a null `expected` is MEANINGFUL
		// ("install only if the slot is empty").
		Check( !pJob->SetProgressIfCurrent( &cbA, &cbB ), "SetProgressIfCurrent(<not current>, next) refuses on an empty slot (z2-a)" );
		Check( pJob->GetProgress() == nullptr, "...and leaves the empty slot empty (z2-a)" );
		Check( pJob->SetProgressIfCurrent( nullptr, &cbA ), "SetProgressIfCurrent(nullptr, next) claims an EMPTY slot (z2-a)" );
		Check( pJob->GetProgress() == &cbA, "...and installs `next` (z2-a)" );
		Check( !pJob->SetProgressIfCurrent( nullptr, &cbB ), "SetProgressIfCurrent(nullptr, next) refuses a slot someone else occupies (z2-a)" );
		Check( pJob->GetProgress() == &cbA, "...and the occupant survives (z2-a)" );
		Check( pJob->SetProgressIfCurrent( &cbA, &cbB ), "SetProgressIfCurrent(<current>, next) swaps (z2-a)" );
		Check( pJob->GetProgress() == &cbB, "...to `next` (z2-a)" );
		Check( pJob->SetProgressIfCurrent( &cbB, nullptr ), "SetProgressIfCurrent(<current>, nullptr) acts as a generalized clear (z2-a)" );
		Check( pJob->GetProgress() == nullptr, "...leaving the slot empty (z2-a)" );
		Check( pJob->SetProgressIfCurrent( nullptr, nullptr ), "SetProgressIfCurrent(nullptr, nullptr) on an empty slot succeeds VACUOUSLY -- TRUE means the compare matched, not that something was installed (documented edge, z2-a)" );
		Check( pJob->GetProgress() == nullptr, "...and the slot is (still) empty (z2-a)" );

		// The reported cross-owner interleave, replayed deterministically:
		// the GUI installed A for its render; before the GUI's clear
		// landed, an agent render claimed the coordinator slot and
		// installed B.  The GUI-side conditional clear must leave B alone
		// -- an unconditional SetProgress(nullptr) here is exactly the
		// stomp this API replaces.
		pJob->SetProgress( &cbA );
		pJob->SetProgress( &cbB );
		Check( !pJob->ClearProgressIfCurrent( &cbA ),
		       "MONEY (z2-a): the GUI-side conditional clear REFUSES once the agent render's callback occupies the slot" );
		Check( pJob->GetProgress() == &cbB, "...and the agent render's callback is still installed afterward (z2-a)" );
		pJob->SetProgress( nullptr );
	}

	// ---- (z2-b) null-slot rasterize detaches the retained callback.
	{
		TickCountingCallback cb;
		pJob->SetProgress( &cb );
		Check( pJob->Rasterize(), "control render with an installed progress callback succeeds (z2-b)" );
		const int ticksAfterControl = cb.ticks.load( std::memory_order_acquire );
		Check( ticksAfterControl > 0, "control: the installed callback received ticks -- the rasterizer really consumed it (z2-b)" );

		pJob->SetProgress( nullptr );
		Check( pJob->Rasterize(), "the null-slot re-render succeeds (z2-b)" );
		Check( cb.ticks.load( std::memory_order_acquire ) == ticksAfterControl,
		       "MONEY (z2-b): the null-slot re-render fired ZERO ticks on the previous render's callback -- pre-fix, "
		       "Job::Rasterize skipped SetProgressCallback on a null slot and the rasterizer's retained pointer kept "
		       "driving the old (with real callers: freed) callback" );
	}

	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (z2) progress-slot CAS clear + null-slot rasterize detach: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (z3) Slot-ownership hardening RED-PROVE (2026-07-12):
//     RunProductionRenderComposed captures its RESTORE value ("prior")
//     INSIDE the coordinator slot, not at function entry on the
//     submitting thread.  Pre-hardening, the entry-time read ran BEFORE
//     the fairness wait and could observe a TRANSIENT occupant's
//     callback (an agent render's coordProgress installed from the
//     coordinator worker); the restore then re-installed that
//     occupant's callback permanently -- and a Job outliving its
//     controller carried a dangling pointer into freed controller
//     storage.  This test parks a fake agent occupant in the slot (its
//     callback installed on the Job, exactly as AgentSession does),
//     submits a composed production render DURING that occupancy, and
//     asserts the post-render slot is what the slot held INSIDE the
//     composed render's turn (null -- the occupant restored it), NOT
//     the occupant's transient callback.
//////////////////////////////////////////////////////////////////////
static void RunComposedPriorCapturedInSlotRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (z3) RED-PROVE: composed render captures its restore value IN-SLOT, not the transient occupant ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_prior_in_slot.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the prior-in-slot scene to a temp file (z3)" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (z3)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	class NullProgress : public IProgressCallback
	{
	public:
		bool Progress( const double, const double ) override { return true; }
		void SetTitle( const char* ) override {}
	};
	NullProgress occupantCb;   // the transient agent occupant's callback
	NullProgress guiCb;        // the composed render's explicit gui sink

	std::atomic<bool> occupantInstalled{ false };
	std::atomic<bool> releaseOccupant{ false };

	// The occupant: claims the coordinator slot and installs its callback
	// on the Job (exactly the AgentSession install shape), then parks
	// until released, then restores what it found (null) -- so the honest
	// in-slot steady-state value for the NEXT turn is null.
	std::atomic<bool> occupantSubmitOk{ false };
	std::thread occupantThread( [&]() {
		SceneEditController::RenderJobId occupantJobId = SceneEditController::kInvalidRenderJobId;
		const bool sub = controller.SubmitAgentRenderSync(
			[&]() {
				pJob->SetProgress( &occupantCb );
				occupantInstalled.store( true, std::memory_order_release );
				// Bounded park (10s cap) so a wedged main thread can never
				// leave this worker spinning forever -- the deadline turns a
				// hypothetical future hang into a loud test FAILURE instead
				// of a silent CI stall (same rationale as RunWatchdogged).
				for( int i = 0; i < 5000 && !releaseOccupant.load( std::memory_order_acquire ); ++i ) {
					std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
				}
				pJob->SetProgress( nullptr );
			},
			String( "z3_transient_occupant" ), &occupantJobId );
		occupantSubmitOk.store( sub, std::memory_order_release );
	} );

	// Wait (bounded) until the occupant genuinely owns the slot with its
	// callback installed -- the exact window the pre-hardening entry-time
	// capture was vulnerable in.  A deadline rather than an unbounded spin:
	// if a future regression makes the submission refuse, this must FAIL
	// loudly, not hang the suite.
	{
		const auto installDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
		while( !occupantInstalled.load( std::memory_order_acquire )
		       && std::chrono::steady_clock::now() < installDeadline ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}
	Check( occupantInstalled.load( std::memory_order_acquire ),
	       "watchdog (z3): the occupant claimed the coordinator slot and installed its callback within 10s "
	       "(a refusal/hang here is a regression in SubmitAgentRenderSync, not a slow pass)" );
	if( !occupantInstalled.load( std::memory_order_acquire ) ) {
		// Bail without the money assertions -- unblock + join whatever did
		// start so the suite can keep reporting other tests.
		releaseOccupant.store( true, std::memory_order_release );
		occupantThread.join();
		controller.Stop();
		pJob->release();
		std::remove( scenePath.c_str() );
		return;
	}
	Check( pJob->GetProgress() == &occupantCb,
	       "control (z3): the transient occupant's callback IS the Job's slot value during the fairness window" );

	// Submit the composed production render DURING the occupancy.  Its
	// (pre-hardening) entry-time GetProgress() read would capture
	// &occupantCb here; it then blocks in the fairness wait until the
	// occupant is released below.
	std::atomic<bool> composedOk{ false };
	std::thread composedThread( [&]() {
		const bool ok = SceneEditController::RunProductionRenderComposed(
			*pJob, &controller, String( "z3_composed" ), &guiCb,
			[pJob]() -> bool { return pJob->Rasterize(); } );
		composedOk.store( ok, std::memory_order_release );
	} );

	// Give the composed submission time to perform its (would-be)
	// entry-time capture and enter the fairness wait, then release the
	// occupant.  100ms is enormous headroom for two statements; the
	// assertions below do not depend on this margin for CORRECTNESS
	// (only the red-prove's ability to catch the pre-fix capture does).
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	releaseOccupant.store( true, std::memory_order_release );

	composedThread.join();
	occupantThread.join();

	Check( occupantSubmitOk.load( std::memory_order_acquire ), "the occupant's SubmitAgentRenderSync was accepted (z3)" );
	Check( composedOk.load( std::memory_order_acquire ), "the composed production render completes successfully (z3)" );
	Check( pJob->GetProgress() == nullptr,
	       "MONEY (z3): after the composed render, the Job's progress slot holds what the slot held INSIDE the composed "
	       "render's coordinator turn (null -- the occupant had restored it), NOT the transient occupant's callback -- "
	       "pre-hardening, the entry-time prior capture re-installed &occupantCb here, a pointer into (in real usage) "
	       "freed controller storage once the occupant's controller was destroyed" );

	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (z3) composed prior captured in-slot: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

//////////////////////////////////////////////////////////////////////
// (z4) Slot-ownership hardening RED-PROVE (2026-07-12):
//     an agent render restores the Job's progress slot to the IN-SLOT
//     prior it found -- preserving a platform's PERSISTENT progress
//     callback (the macOS RISEBridge shape) -- instead of the old
//     hardcoded restore-to-null, which wiped the persistent callback
//     from the slot after every agent render (the GUI's progress hook
//     silently went dead until the next setProgressBlock:).
//////////////////////////////////////////////////////////////////////
static void RunAgentRenderRestoresPersistentCallbackRedProveTest()
{
	std::printf( "=== AgentRenderAsyncTest: (z4) RED-PROVE: agent render restores the persistent (mac-shape) progress callback ===\n" );

	const std::string scenePath = WriteTemp( "rise_agent_persistent_restore.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the persistent-restore scene to a temp file (z4)" );

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "Job loads the native-v7 scene via the CST path (z4)" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );

	class NullProgress : public IProgressCallback
	{
	public:
		bool Progress( const double, const double ) override { return true; }
		void SetTitle( const char* ) override {}
	} persistentCb;

	// Simulate the macOS bridge: a persistent callback installed on the
	// Job for the app's whole lifetime (setProgressBlock:).
	pJob->SetProgress( &persistentCb );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession::WrapJob wraps the Job (z4)" );
	if( session )
	{
		session->AttachController( &controller );
		AgentRenderParams p;
		const AgentRenderResult r = session->Render( p );
		Check( r.ok, "the agent render completes successfully (z4)" );
		Check( pJob->GetProgress() == &persistentCb,
		       "MONEY (z4): the agent render restored the Job's progress slot to the persistent callback it found "
		       "in-slot -- pre-hardening, AgentSession's hardcoded restore-to-null wiped the platform's persistent "
		       "progress hook after every agent render on a live-GUI Job" );
		session->AttachController( nullptr );
	}

	pJob->SetProgress( nullptr );
	controller.Stop();
	pJob->release();
	std::remove( scenePath.c_str() );

	std::printf( "=== (z4) agent render restores persistent callback: %d passed, %d failed (cumulative) ===\n", g_pass, g_fail );
}

// (guard) render-owns-scene: a UI-callable mMutex-locking method, called on
// another thread WHILE a production/agent render owns the scene (holds mMutex
// across its closure), must NO-OP and return immediately -- never block on
// mMutex for the render's whole duration.  Deterministic proof of the
// render-owns-scene guard that closes the chat-render wedge class (a chat/agent
// render holds mMutex WITHOUT flipping renderState, so the GUI can still call
// these mid-render).  RunWatchdogged turns a wedge (block >> the render) into a
// test FAILURE rather than a suite hang.
static void RunRenderOwnsSceneGuardTest()
{
	std::printf( "=== AgentRenderAsyncTest: (guard) UI mMutex methods no-op (never wedge) while a render owns the scene ===\n" );

	const std::string scenePath = WriteTemp( "rise_render_owns_scene_guard.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the guard-test scene" );

	auto insideRasterize = std::make_shared<std::atomic<int>>( 0 );
	CanaryJob* pJob = new CanaryJob( /*sleepMs*/ 800, insideRasterize );
	Check( pJob->LoadAsciiSceneViaCst( scenePath.c_str() ), "CanaryJob loads the guard-test scene via the CST path" );

	SceneEditController controller( *pJob, /*interactiveRasterizer*/0 );
	controller.Start( /*suppressInitialRender=*/true );
	Check( !controller.ForTest_RenderOwnsScene(), "render-owns-scene flag is FALSE before any render" );

	std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
	Check( session != nullptr, "AgentSession wraps the CanaryJob" );
	if( session )
	{
		session->AttachController( &controller );
		const AgentSession::AgentRenderAsyncResult ar = session->RenderAsync( AgentRenderParams() );
		Check( ar.accepted && ar.renderJobId != 0, "async render accepted" );

		// Wait until the worker is genuinely INSIDE Rasterize() -- i.e. the RAII
		// RenderOwnershipScope has set mRenderOwnsScene true and mMutex is held
		// for the ~800ms sleep.  CanaryJob bumps `insideRasterize` on entry.
		bool inside = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 2000 );
		while( std::chrono::steady_clock::now() < deadline ) {
			if( insideRasterize->load( std::memory_order_acquire ) > 0 ) { inside = true; break; }
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
		Check( inside, "worker entered Rasterize() (render now owns mMutex)" );
		Check( controller.ForTest_RenderOwnsScene(), "render-owns-scene flag is TRUE while the render runs" );

		// MONEY: a guarded GETTER (GetEnvironment -- the exact method whose
		// main-thread call caused the original production-render hang) and a
		// guarded MUTATOR (GoToHomeView), invoked while the render holds mMutex
		// for ~800ms, must each return in well under 200ms (no-op) -- NOT block
		// for the render.  A wedge trips the 200ms watchdog -> FAIL.
		const bool getterQuick = RunWatchdogged(
			"GetEnvironment() while a render owns the scene", 200, [&]{
				SceneEditController::EnvironmentInfo env;
				(void)controller.GetEnvironment( env );
			} );
		Check( getterQuick, "MONEY: GetEnvironment() did NOT wedge on mMutex during a render (< 200ms of an 800ms render)" );

		const bool mutatorQuick = RunWatchdogged(
			"GoToHomeView() while a render owns the scene", 200, [&]{
				(void)controller.GoToHomeView();
			} );
		Check( mutatorQuick, "MONEY: GoToHomeView() did NOT wedge on mMutex during a render" );

		const bool completed = session->RenderWait( ar.renderJobId, 5000 );
		Check( completed, "render completed within the timeout" );
		Check( !controller.ForTest_RenderOwnsScene(), "render-owns-scene flag CLEARED after the render finished" );

		session->AttachController( nullptr );
	}
	controller.Stop();
}

int main()
{
	RunAsyncReturnsQuicklyTest();
	RunRenderOwnsSceneGuardTest();
	RunSingleSlotRejectionTest();
	RunPinnedRenderTests();
	RunConcurrencyRaceClosureTest();
	RunSyncStillWorksTest();
	RunStatusWaitOddIdRejectionTest();
	RunAsyncThrowSurvivesTest();
	RunFirstControllerIdIsTwoTest();
	RunPostStopRefusalTest();
	RunQueuedSyncWaiterUnblocksOnStopTest();
	RunStopCancelsInFlightAgentRenderTest();
	RunCancelAsyncRenderRedProveTest();
	RunFairSlotReservationTest();
	RunTxnOpenRenderRefusalTest();
	RunSessionDestroyedMidFlightTest();
	RunUnboundedDrainLoopTest();
	RunNoStaleOutstandingIdTest();
	RunProgressRestoredOnThrowTest();
	RunInterleavedMintNoClobberTest();
	RunQueuedCancellationSurvivesWorkerStartTest();
	RunMintClobberRedProveTest();
	RunLostEditWedgeRedProveTest();
	RunSaveVsRenderRedProveTest();
	RunSaveSnapshotConcurrentEditTest();
	RunChurnConcurrentInteractiveTest();
	RunProductionTripleConcurrencyProofTest();
	RunDirectRasterizeRedProveTest();
	RunProductionProgressCompositionTest();
	RunProductionQueueSemanticsTest();
	RunStopDuringProductionTest();
	RunInnerResetOnThrowRedProveTest();
	RunExplicitGuiProgressNotInstalledOnJobRedProveTest();
	RunRefusedProductionSubmitNoFallbackRedProveTest();
	RunViewportPauseThenProductionSubmitRedProveTest();
	RunProgressSlotAtomicClearTest();
	RunComposedPriorCapturedInSlotRedProveTest();
	RunAgentRenderRestoresPersistentCallbackRedProveTest();

	std::printf( "=== AgentRenderAsyncTest TOTAL: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
