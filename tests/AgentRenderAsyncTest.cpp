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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
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

int main()
{
	RunAsyncReturnsQuicklyTest();
	RunSingleSlotRejectionTest();
	RunConcurrencyRaceClosureTest();
	RunSyncStillWorksTest();
	RunStatusWaitOddIdRejectionTest();
	RunAsyncThrowSurvivesTest();
	RunFirstControllerIdIsTwoTest();

	std::printf( "=== AgentRenderAsyncTest TOTAL: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
