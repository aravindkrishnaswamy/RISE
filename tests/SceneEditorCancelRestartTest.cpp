//////////////////////////////////////////////////////////////////////
//
//  SceneEditorCancelRestartTest.cpp - Phase 2 integration test for
//    the SceneEditController's cancel-restart render-thread loop.
//
//  Validates:
//    - Start() spawns a worker that performs the initial render.
//    - KickRender() trips the cancel flag while a render is in
//      progress, and a fresh render fires immediately afterward.
//    - Many rapid edits coalesce — the controller never queues a
//      backlog of outdated renders.
//    - Stop() drains in-flight work and joins cleanly.
//
//  Uses a test-subclass that overrides DoOneRenderPass with a
//  simulated render that polls IsCancelRequested at frame-tick
//  granularity (the same polling discipline the real
//  RasterizeBlockDispatcher uses on tile boundaries).
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

// Attach a default pinhole camera to the job's scene so OrbitCamera /
// PanCamera / ZoomCamera edits actually mutate something — without a
// camera, SceneEditor::Apply early-returns false on every edit and the
// controller's pointer-driven cancel-restart path never fires.
static void AttachDefaultCamera( Job& job )
{
	ICamera* pCam = nullptr;
	if( !RISE_API_CreatePinholeCamera(
		&pCam,
		/*location*/   Point3( 0, 0, 5 ),
		/*lookAt*/     Point3( 0, 0, 0 ),
		/*up*/         Vector3( 0, 1, 0 ),
		/*fov*/        Scalar( 0.785398 ),  // ~45 deg
		/*xres*/       64,
		/*yres*/       64,
		/*pixelAR*/    Scalar( 1 ),
		/*exposure*/   Scalar( 0 ),
		/*scanRate*/   Scalar( 0 ),
		/*pixelRate*/  Scalar( 0 ),
		/*orient*/     Vector3( 0, 0, 0 ),
		/*targOrient*/ Vector2( 0, 0 ) ) )
	{
		return;
	}
	if( IScenePriv* scene = job.GetScene() )
	{
		scene->SetCamera( pCam );
	}
	pCam->release();
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition )
	{
		passCount++;
	}
	else
	{
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Test harness: a SceneEditController subclass whose DoOneRenderPass
// simulates ~50ms of render work in cancel-checked 5ms slices.
//////////////////////////////////////////////////////////////////////

class TestController : public SceneEditController
{
public:
	TestController( IJobPriv& job, unsigned int simulatedRenderMs = 50 )
	: SceneEditController( job, /*interactiveRasterizer*/0 )
	, mSimulatedRenderMs( simulatedRenderMs )
	, mAbortCount( 0 )
	, mCompletedCount( 0 )
	{}

	unsigned int AbortCount()     const { return mAbortCount.load(); }
	unsigned int CompletedCount() const { return mCompletedCount.load(); }

protected:
	void DoOneRenderPass() override
	{
		const unsigned int sliceMs = 5;
		const unsigned int slices  = ( mSimulatedRenderMs + sliceMs - 1 ) / sliceMs;
		for( unsigned int i = 0; i < slices; ++i )
		{
			if( IsCancelRequested() )
			{
				mAbortCount.fetch_add( 1 );
				return;
			}
			std::this_thread::sleep_for( std::chrono::milliseconds( sliceMs ) );
		}
		mCompletedCount.fetch_add( 1 );
	}

private:
	unsigned int              mSimulatedRenderMs;
	std::atomic<unsigned int> mAbortCount;
	std::atomic<unsigned int> mCompletedCount;
};

//////////////////////////////////////////////////////////////////////
// Tests
//////////////////////////////////////////////////////////////////////

static void TestStartStopIsClean()
{
	std::cout << "Testing Start/Stop lifecycle..." << std::endl;

	Job* pJob = new Job();
	TestController c( *pJob );

	c.Start();
	Check( c.IsRunning(), "controller running after Start" );

	// Wait for at least the initial render pass to complete.
	Check( c.ForTest_WaitForRenders( 1, /*timeoutMs*/2000 ),
	       "initial render fires within 2s" );

	c.Stop();
	Check( !c.IsRunning(), "controller not running after Stop" );

	// Idempotency
	c.Start();
	c.Start();   // second Start is a no-op
	Check( c.IsRunning(), "Start is idempotent" );
	c.Stop();
	c.Stop();    // second Stop is a no-op
	Check( !c.IsRunning(), "Stop is idempotent" );
	pJob->release();
}

static void TestSingleEditRestartsRender()
{
	std::cout << "Testing single edit triggers re-render..." << std::endl;

	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	TestController c( *pJob, /*simulatedRenderMs*/30 );
	c.Start();
	Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render completes" );

	const unsigned int rendersBefore = c.ForTest_GetRenderCount();

	// Apply one edit; this should kick the render thread for one more pass.
	SceneEdit camEdit;
	camEdit.op = SceneEdit::OrbitCamera;
	camEdit.v3a = Vector3( 0.1, 0.05, 0 );
	c.Editor().Apply( camEdit );
	// Manually kick — Editor().Apply doesn't go through the controller's
	// pointer-event path, so KickRender isn't called automatically.
	// We simulate the same effect via OnPointerDown/Move/Up which the
	// controller wires up.

	c.SetTool( SceneEditController::Tool::OrbitCamera );
	c.OnPointerDown( Point2( 100, 100 ) );
	c.OnPointerMove( Point2( 105, 100 ) );
	c.OnPointerUp( Point2( 105, 100 ) );

	Check( c.ForTest_WaitForRenders( rendersBefore + 1, 2000 ),
	       "render count increases after edit" );

	c.Stop();
	pJob->release();
}

static void TestRapidEditsCancelPriorRender()
{
	std::cout << "Testing rapid edits cancel in-flight renders..." << std::endl;

	// Long simulated render (200ms) makes it easy to cancel mid-flight.
	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	TestController c( *pJob, /*simulatedRenderMs*/200 );
	c.Start();

	// Configure tool and selection so OnPointerMove emits real edits.
	c.SetTool( SceneEditController::Tool::OrbitCamera );

	// Fire pointer events ~30Hz for 1 second — far faster than the 200ms
	// simulated render can complete.
	c.OnPointerDown( Point2( 100, 100 ) );
	for( int i = 0; i < 30; ++i )
	{
		c.OnPointerMove( Point2( 100 + i, 100 ) );
		std::this_thread::sleep_for( std::chrono::milliseconds( 33 ) );
	}
	c.OnPointerUp( Point2( 130, 100 ) );

	// Give the render thread a moment to settle on the final state.
	std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );
	c.Stop();

	const unsigned int cancels   = c.ForTest_GetCancelCount();
	const unsigned int aborts    = c.AbortCount();
	const unsigned int completed = c.CompletedCount();

	std::cout << "    cancels=" << cancels
	          << " aborts=" << aborts
	          << " completed=" << completed << std::endl;

	// We fired 30 edits over ~1 second; with each render taking 200ms
	// most of those edits should have arrived while a render was in
	// flight, tripping the cancel flag.  We expect at least a handful.
	Check( cancels >= 5,
	       "at least 5 cancellations were requested during rapid drag" );

	// The render thread should have aborted at least some renders.
	Check( aborts >= 5,
	       "at least 5 renders aborted mid-pass" );

	// And the final state should have rendered to completion at least once
	// (after the last edit, the render thread gets a clear run).
	Check( completed >= 1,
	       "at least one render completed after edits stopped" );
	pJob->release();
}

static void TestCancelDoesNotPersistAcrossPasses()
{
	std::cout << "Testing cancel flag is reset between render passes..." << std::endl;

	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	TestController c( *pJob, /*simulatedRenderMs*/30 );
	c.Start();

	// Wait for the initial render.
	Check( c.ForTest_WaitForRenders( 1, 2000 ), "initial render completes" );

	// Fire a sequence of edits separated by enough delay that the
	// inter-edit gap exceeds the render time.  Each edit should
	// trip the cancel flag (transiently — the prior render may
	// already have finished), then a fresh render fires and runs
	// to completion.  If Reset() weren't being called, the cancel
	// flag would persist across passes and every render would
	// abort, so CompletedCount would not grow past 1.
	c.SetTool( SceneEditController::Tool::OrbitCamera );
	for( int i = 0; i < 5; ++i )
	{
		c.OnPointerDown( Point2( 100 + 10*i, 100 ) );
		c.OnPointerMove( Point2( 105 + 10*i, 100 ) );
		c.OnPointerUp( Point2( 105 + 10*i, 100 ) );
		std::this_thread::sleep_for( std::chrono::milliseconds( 80 ) );
	}

	// Wait for the final render to settle.
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	const unsigned int completed = c.CompletedCount();

	c.Stop();

	Check( completed >= 3,
	       "multiple renders complete after intermittent edits — Reset() works" );
	pJob->release();
}

static void TestDestructorJoinsThread()
{
	std::cout << "Testing destructor joins the render thread..." << std::endl;

	Job* pJob = new Job();
	{
		TestController c( *pJob );
		c.Start();
		std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
		// Falls out of scope; destructor must call Stop() and join.
	}
	// If we reach here without hanging or asserting, the destructor
	// behaved correctly.
	Check( true, "destructor joined render thread without hanging" );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== SceneEditController Cancel-Restart Test (Phase 2) ===" << std::endl;

	TestStartStopIsClean();
	TestSingleEditRestartsRender();
	TestRapidEditsCancelPriorRender();
	TestCancelDoesNotPersistAcrossPasses();
	TestDestructorJoinsThread();

	std::cout << "\n=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;

	return failCount == 0 ? 0 : 1;
}
