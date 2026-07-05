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
#include "../src/Library/Job.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Interfaces/ILogPriv.h"   // round-2 P1-1 red-prove: capture the escalating cancel-ignored warning

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
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
		// A tiny extra hold so a concurrent interactive pass has a
		// realistic window to land in even though the real render itself
		// is fast on this tiny 24x24 scene.
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		const bool ok = Job::Rasterize();
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		mProof.Leave();
		return ok;
	}
private:
	ConcurrencyProof& mProof;
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
//     restored to null -- the RAII ProgressRestoreGuard runs on the
//     exceptional exit, not just the ordinary one.  Pre-fix, the
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

int main()
{
	RunAsyncReturnsQuicklyTest();
	RunSingleSlotRejectionTest();
	RunConcurrencyRaceClosureTest();
	RunSyncStillWorksTest();
	RunStatusWaitOddIdRejectionTest();
	RunAsyncThrowSurvivesTest();
	RunFirstControllerIdIsTwoTest();
	RunPostStopRefusalTest();
	RunQueuedSyncWaiterUnblocksOnStopTest();
	RunStopCancelsInFlightAgentRenderTest();
	RunFairSlotReservationTest();
	RunTxnOpenRenderRefusalTest();
	RunSessionDestroyedMidFlightTest();
	RunUnboundedDrainLoopTest();
	RunNoStaleOutstandingIdTest();
	RunProgressRestoredOnThrowTest();
	RunInterleavedMintNoClobberTest();
	RunMintClobberRedProveTest();
	RunLostEditWedgeRedProveTest();
	RunChurnConcurrentInteractiveTest();

	std::printf( "=== AgentRenderAsyncTest TOTAL: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
