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
#include "../src/Library/Cameras/CameraCommon.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/Rasterizer.h"
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Rendering/ViewportFrameStore.h"
#include "../src/Library/SceneEditor/CameraIntrospection.h"
#include "../src/Library/Utilities/Reference.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>			// getenv() -- TMPDIR lookup in WriteTemp
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif
#include <memory>
#include <stdexcept>
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
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n\texposure 0.01\n\tiso 100\n\tfstop 2\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"box_geometry\n{\n\tname geo\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj\n\tgeometry geo\n\tmaterial mat\n}\n";

// Single convex object + delta light: its camera-facing centre has direct
// NEE but no diffuse continuation can hit another surface, so Indirect is
// exactly black there.  Used by the paired-transport pixel oracle below.
static const char* const kSharedDirectScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 32\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tname cam\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt\n\tcolor 0.7 0.7 0.7\n}\n\n"
	"lambertian_material\n{\n\tname mat\n\treflectance pnt\n}\n\n"
	"sphere_geometry\n{\n\tname geo\n\tradius 1.5\n}\n\n"
	"standard_object\n{\n\tname obj\n\tgeometry geo\n\tmaterial mat\n}\n\n"
	"omni_light\n{\n\tname lgt\n\tpower 8.0\n\tcolor 1 1 1\n\tposition 0 3 4\n}\n";

static std::string WriteTemp( const char* name, const char* content )
{
	// Round-8: per-process filename.  These were fixed /tmp paths, so two
	// instances of this binary running at once (a stray earlier run, a
	// developer running it while the suite does) clobbered each other's
	// scene files mid-load and surfaced as a bogus "fixture constructs"
	// failure.  Reproduced by running 12 copies concurrently: 3 failed that
	// way; with the pid suffix, 0 do.
	// Round-9: honour TMPDIR like the other fourteen WriteTemp helpers do
	// (AgentRenderAsyncTest, AgentObjectMapTest, ...) -- fifteen test files
	// carry a copy of this helper; round 10 brought the last five
	// (AgentFirstSliceTest, AgentHeadVersionTest, AgentMcpAdapterTest,
	// AgentProposeRenderTest, AgentViewModeRenderTest) up to the same
	// TMPDIR + pid shape.  A hardcoded "/tmp/"
	// is not portable -- these tests are glob-enumerated into the Windows
	// cmake build, where /tmp typically does not exist -- and having one
	// member of the family differ is how the family drifts apart again.
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	std::string path = dir + std::to_string( (long)getpid() ) + "_" + name;
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
	struct VariantPassRecord
	{
		Implementation::ViewportRenderMode mode;
		bool liveGesture;
		int samples;
		bool denoiseKnown;
		bool denoiseEnabled;
	};

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

	std::vector<VariantPassRecord> VariantPassRecords() const
	{
		std::lock_guard<std::mutex> lk( mVariantPassMutex );
		return mVariantPassRecords;
	}

	void ClearVariantPassRecords()
	{
		std::lock_guard<std::mutex> lk( mVariantPassMutex );
		mVariantPassRecords.clear();
	}

	bool WaitForVariantPassCount( std::size_t count, unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mVariantPassMutex );
		return mVariantPassCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mVariantPassRecords.size() >= count; } );
	}

	bool LastVariantDenoiseSuppressed() const
	{
		std::lock_guard<std::mutex> lk( mVariantPassMutex );
		const Implementation::PathTracingPelRasterizer* pt =
			dynamic_cast<const Implementation::PathTracingPelRasterizer*>(
				mLastVariantRasterizer );
		return pt && pt->ForTest_IsInteractiveDenoiseSuppressed();
	}

	bool LastVariantAOVDimensions( unsigned int& width, unsigned int& height ) const
	{
		std::lock_guard<std::mutex> lk( mVariantPassMutex );
#ifdef RISE_ENABLE_OIDN
		const Implementation::PixelBasedRasterizerHelper* helper =
			dynamic_cast<const Implementation::PixelBasedRasterizerHelper*>(
				mLastVariantRasterizer );
		return helper && helper->ForTest_GetAOVBufferDimensions( width, height );
#else
		(void)width;
		(void)height;
		return false;
#endif
	}

	//! Blocks until the recorded sequence reaches `count` passes (true)
	//! or `timeoutMs` elapses (false).
	bool WaitForPassCount( std::size_t count, unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mSeqMutex );
		return mSeqCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mSeq.size() >= count; } );
	}

	bool WaitForCompletedPassCount( std::size_t count, unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mSeqMutex );
		return mSeqCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mCompletedPasses >= count; } );
	}

	bool WaitForPaneVantage( unsigned int pane,
		SceneEditController::PaneVantageKind expected,
		String& outName, unsigned int timeoutMs )
	{
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds( timeoutMs );
		do {
			SceneEditController::PaneVantageKind actual;
			if( GetPaneVantage( pane, actual, outName ) && actual == expected )
				return true;
			std::this_thread::yield();
		} while( std::chrono::steady_clock::now() < deadline );
		return false;
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
		std::unique_lock<std::mutex> lk( mSeqMutex );
		// DoOneRenderPass records at entry, while retirement (including output
		// detachment and mRendering=false publication) happens later.  A phase
		// reset must not erase an active pass and let its retirement/next quantum
		// leak into the new accounting window.
		const bool retired = mSeqCV.wait_for(
			lk, std::chrono::milliseconds( 4000 ), [&] {
			return mCompletedPasses >= mSeq.size();
		} );
		if( !retired ) return;   // preserve evidence; the next phase must fail
		mSeq.clear();
		mCompletedPasses = 0;
	}

	void SetSlowPassMs( unsigned int ms )
	{
		mSlowPassMs.store( ms, std::memory_order_release );
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

	bool WaitForMintedPass( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mGateMutex );
		return mGateCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mMintObserved; } );
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

	void ArmPointerMoveAdmissionInterleaving()
	{
		std::lock_guard<std::mutex> lk( mMoveGateMutex );
		mMoveGateArmed = true;
		mMoveGateReached = false;
		mReleaseMoveGate = false;
	}

	void ArmPointerDownAdmissionInterleaving()
	{
		std::lock_guard<std::mutex> lk( mDownGateMutex );
		mDownGateArmed = true;
		mDownGateReached = false;
		mReleaseDownGate = false;
	}

	bool WaitForPointerDownAdmission( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mDownGateMutex );
		return mDownGateCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mDownGateReached; } );
	}

	void ReleasePointerDownAdmission()
	{
		std::lock_guard<std::mutex> lk( mDownGateMutex );
		mReleaseDownGate = true;
		mDownGateArmed = false;
		mDownGateCV.notify_all();
	}

	bool WaitForPointerMoveAdmission( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mMoveGateMutex );
		return mMoveGateCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mMoveGateReached; } );
	}

	void ReleasePointerMoveAdmission()
	{
		std::lock_guard<std::mutex> lk( mMoveGateMutex );
		mReleaseMoveGate = true;
		mMoveGateArmed = false;
		mMoveGateCV.notify_all();
	}

	void ArmLastRenderDrainRegistration()
	{
		std::lock_guard<std::mutex> lk( mDrainGateMutex );
		mDrainGateArmed = true;
		mDrainGateReached = false;
		mReleaseDrainGate = false;
		mQuiesceReached = false;
	}

	bool WaitForLastRenderDrainRegistration( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mDrainGateMutex );
		return mDrainGateCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mDrainGateReached; } );
	}

	void ReleaseLastRenderDrainRegistration()
	{
		std::lock_guard<std::mutex> lk( mDrainGateMutex );
		mReleaseDrainGate = true;
		mDrainGateArmed = false;
		mDrainGateCV.notify_all();
	}

	void ArmRenderAdmissionAttempt()
	{
		std::lock_guard<std::mutex> lk( mRenderAttemptMutex );
		mRenderAttemptArmed = true;
		mRenderAttemptReached = false;
	}

	bool WaitForRenderAdmissionAttempt( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mRenderAttemptMutex );
		return mRenderAttemptCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mRenderAttemptReached; } );
	}

	void ArmLastRenderTransitionAttempt( unsigned int pane )
	{
		std::lock_guard<std::mutex> lk( mLastRenderAttemptMutex );
		mLastRenderAttemptPane = pane;
		mLastRenderAttemptReached = false;
	}

	bool WaitForLastRenderTransitionAttempt( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mLastRenderAttemptMutex );
		return mLastRenderAttemptCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mLastRenderAttemptReached; } );
	}

	bool WaitForLastRenderQuiesce( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mDrainGateMutex );
		return mDrainGateCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mQuiesceReached; } );
	}

	void ArmOutputDetachGate( unsigned int pane )
	{
		std::lock_guard<std::mutex> lk( mOutputDetachMutex );
		mOutputDetachPane = pane;
		mOutputDetachArmed = true;
		mOutputDetachReached = false;
		mReleaseOutputDetach = false;
	}

	bool WaitForOutputDetachGate( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mOutputDetachMutex );
		return mOutputDetachCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mOutputDetachReached; } );
	}

	void ReleaseOutputDetachGate()
	{
		std::lock_guard<std::mutex> lk( mOutputDetachMutex );
		mReleaseOutputDetach = true;
		mOutputDetachArmed = false;
		mOutputDetachCV.notify_all();
	}

protected:
	void ForTest_OnBeautyVariantPassConfigured(
		IRasterizer& rasterizer,
		Implementation::ViewportRenderMode mode,
		bool liveGesture ) override
	{
		VariantPassRecord record;
		record.mode = mode;
		record.liveGesture = liveGesture;
		record.samples = rasterizer.GetSampleCountOverride();
		record.denoiseKnown = false;
		record.denoiseEnabled = false;
#ifdef RISE_ENABLE_OIDN
		if( Implementation::Rasterizer* concrete =
				dynamic_cast<Implementation::Rasterizer*>( &rasterizer ) )
		{
			record.denoiseKnown = true;
			record.denoiseEnabled = concrete->GetDenoisingEnabled();
		}
#endif
		{
			std::lock_guard<std::mutex> lk( mVariantPassMutex );
			mLastVariantRasterizer = &rasterizer;
			mVariantPassRecords.push_back( record );
		}
		mVariantPassCV.notify_all();
	}

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
			// Round-8 w4: a configurable SLOW pass models a BeautyVariant
			// quantum (fixed spp + a non-cancel-interruptible OIDN denoise),
			// which is what makes the watchdog's clock choice load-bearing.
			const unsigned int ms = mSlowPassMs.load( std::memory_order_acquire );
			std::this_thread::sleep_for(
				std::chrono::milliseconds( ms ? ms : 2 ) );
		}
	}

	void ForTest_OnInteractivePassRetired() override
	{
		std::lock_guard<std::mutex> lk( mSeqMutex );
		++mCompletedPasses;
		mSeqCV.notify_all();
	}

	void ForTest_OnInteractivePassBeforeOutputDetach() override
	{
		std::unique_lock<std::mutex> lk( mOutputDetachMutex );
		if( !mOutputDetachArmed || ForTest_CurrentPane() != mOutputDetachPane ) return;
		mOutputDetachReached = true;
		mOutputDetachCV.notify_all();
		mOutputDetachCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&]{ return mReleaseOutputDetach || !mOutputDetachArmed; } );
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

	void ForTest_OnPointerMoveAdmitted() override
	{
		std::unique_lock<std::mutex> lk( mMoveGateMutex );
		if( !mMoveGateArmed ) return;
		mMoveGateReached = true;
		mMoveGateCV.notify_all();
		mMoveGateCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&]{ return mReleaseMoveGate || !mMoveGateArmed; } );
	}

	void ForTest_OnPointerDownAdmitted() override
	{
		std::unique_lock<std::mutex> lk( mDownGateMutex );
		if( !mDownGateArmed ) return;
		mDownGateReached = true;
		mDownGateCV.notify_all();
		mDownGateCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&]{ return mReleaseDownGate || !mDownGateArmed; } );
	}

	void ForTest_OnTimeScrubBeginAdmitted() override
	{
		std::unique_lock<std::mutex> lk( mDownGateMutex );
		if( !mDownGateArmed ) return;
		mDownGateReached = true;
		mDownGateCV.notify_all();
		mDownGateCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&]{ return mReleaseDownGate || !mDownGateArmed; } );
	}

	void ForTest_OnLastRenderDrainRegistered() override
	{
		std::unique_lock<std::mutex> lk( mDrainGateMutex );
		if( !mDrainGateArmed ) return;
		mDrainGateReached = true;
		mDrainGateCV.notify_all();
		mDrainGateCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&]{ return mReleaseDrainGate || !mDrainGateArmed; } );
	}

	void ForTest_OnLastRenderQuiesceStarted() override
	{
		std::lock_guard<std::mutex> lk( mDrainGateMutex );
		mQuiesceReached = true;
		mDrainGateCV.notify_all();
	}

	void ForTest_OnAgentWorkerAboutToParkRender() override
	{
		std::lock_guard<std::mutex> lk( mRenderAttemptMutex );
		if( !mRenderAttemptArmed ) return;
		mRenderAttemptReached = true;
		mRenderAttemptCV.notify_all();
	}

	void ForTest_OnCoordinatedRenderAdmissionAttempt() override
	{
		std::lock_guard<std::mutex> lk( mRenderAttemptMutex );
		if( !mRenderAttemptArmed ) return;
		mRenderAttemptReached = true;
		mRenderAttemptCV.notify_all();
	}

	void ForTest_OnLastRenderTransitionAttempt( unsigned int pane ) override
	{
		std::lock_guard<std::mutex> lk( mLastRenderAttemptMutex );
		if( pane != mLastRenderAttemptPane ) return;
		mLastRenderAttemptReached = true;
		mLastRenderAttemptCV.notify_all();
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
	std::size_t                 mCompletedPasses = 0;
	bool                        mRunRealPass = false;
	std::atomic<unsigned int>   mSlowPassMs { 0 };
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
	std::mutex                  mMoveGateMutex;
	std::condition_variable     mMoveGateCV;
	bool                        mMoveGateArmed = false;
	bool                        mMoveGateReached = false;
	bool                        mReleaseMoveGate = false;
	std::mutex                  mDownGateMutex;
	std::condition_variable     mDownGateCV;
	bool                        mDownGateArmed = false;
	bool                        mDownGateReached = false;
	bool                        mReleaseDownGate = false;
	std::mutex                  mDrainGateMutex;
	std::condition_variable     mDrainGateCV;
	bool                        mDrainGateArmed = false;
	bool                        mDrainGateReached = false;
	bool                        mReleaseDrainGate = false;
	bool                        mQuiesceReached = false;
	std::mutex                  mOutputDetachMutex;
	std::condition_variable     mOutputDetachCV;
	unsigned int                mOutputDetachPane = 0;
	bool                        mOutputDetachArmed = false;
	bool                        mOutputDetachReached = false;
	bool                        mReleaseOutputDetach = false;
	std::mutex                  mRenderAttemptMutex;
	std::condition_variable     mRenderAttemptCV;
	bool                        mRenderAttemptArmed = false;
	bool                        mRenderAttemptReached = false;
	std::mutex                  mLastRenderAttemptMutex;
	std::condition_variable     mLastRenderAttemptCV;
	unsigned int                mLastRenderAttemptPane = ~0u;
	bool                        mLastRenderAttemptReached = false;
	mutable std::mutex          mVariantPassMutex;
	std::condition_variable     mVariantPassCV;
	std::vector<VariantPassRecord> mVariantPassRecords;
	IRasterizer*                 mLastVariantRasterizer = nullptr;
};

class RecordingPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	unsigned int Count() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mCount;
	}

	unsigned int Width() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mWidth;
	}

	unsigned int Height() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mHeight;
	}

	bool WaitForDimensions( unsigned int width, unsigned int height,
		unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mMutex );
		return mCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ), [&]{
			return mWidth == width && mHeight == height;
		} );
	}

	RISEColor FirstPixel() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mFirstPixel;
	}

	RISEColor CenterPixel() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mCenterPixel;
	}

	unsigned int ExposureCount() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mExposureCount;
	}

	Scalar ExposureEV() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mExposureEV;
	}

	unsigned int PreDenoisedCount() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mPreDenoisedCount;
	}

	unsigned int DenoisedCount() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mDenoisedCount;
	}

	void SetCameraExposureCompensationEV( Scalar ev ) override
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mExposureEV = ev;
		++mExposureCount;
	}

	void OutputPreDenoisedImage(
		const IRasterImage&, const Rect*, const unsigned int ) override
	{
		std::lock_guard<std::mutex> lk( mMutex );
		++mPreDenoisedCount;
	}

	void OutputDenoisedImage(
		const IRasterImage& image, const Rect* region,
		const unsigned int frame ) override
	{
		{
			std::lock_guard<std::mutex> lk( mMutex );
			++mDenoisedCount;
		}
		OutputImage( image, region, frame );
	}

	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	void OutputImage(
		const IRasterImage& image, const Rect*, const unsigned int ) override
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mWidth = image.GetWidth();
		mHeight = image.GetHeight();
		mFirstPixel = ( mWidth && mHeight )
			? image.GetPEL( 0, 0 ) : RISEColor();
		mCenterPixel = ( mWidth && mHeight )
			? image.GetPEL( mWidth / 2, mHeight / 2 ) : RISEColor();
		++mCount;
		mCV.notify_all();
	}

protected:
	~RecordingPaneSink() override = default;

private:
	mutable std::mutex mMutex;
	std::condition_variable mCV;
	unsigned int       mCount = 0;
	unsigned int       mWidth = 0;
	unsigned int       mHeight = 0;
	RISEColor           mFirstPixel;
	RISEColor           mCenterPixel;
	Scalar              mExposureEV = 0;
	unsigned int        mExposureCount = 0;
	unsigned int        mPreDenoisedCount = 0;
	unsigned int        mDenoisedCount = 0;
};

class ReentrantSharedPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	ReentrantSharedPaneSink( SceneEditController& controller, unsigned int pane,
		IRasterizerOutput* replacement )
		: mController( controller ), mPane( pane ), mReplacement( replacement ) {}

	bool Reentered() const { return mReentered.load( std::memory_order_acquire ); }
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		if( !mReentered.exchange( true, std::memory_order_acq_rel ) ) {
			mController.SetPaneSink( mPane, mReplacement );
		}
	}

protected:
	~ReentrantSharedPaneSink() override = default;

private:
	SceneEditController& mController;
	unsigned int mPane;
	IRasterizerOutput* mReplacement;
	std::atomic<bool> mReentered { false };
};

class ThrowOnceSharedPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	unsigned int Attempts() const
	{
		return mAttempts.load( std::memory_order_acquire );
	}
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		const unsigned int attempt =
			mAttempts.fetch_add( 1, std::memory_order_acq_rel ) + 1;
		if( attempt == 1 ) throw std::runtime_error( "intentional shared Direct failure" );
	}

protected:
	~ThrowOnceSharedPaneSink() override = default;

private:
	std::atomic<unsigned int> mAttempts { 0 };
};

class BlockingPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	bool Entered() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mEntered;
	}
	bool WaitUntilEntered( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mMutex );
		return mCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&]{ return mEntered; } );
	}

	void Release()
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mReleased = true;
		mCV.notify_all();
	}

	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		std::unique_lock<std::mutex> lk( mMutex );
		mEntered = true;
		mCV.notify_all();
		mCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&]{ return mReleased; } );
	}

protected:
	~BlockingPaneSink() override = default;

private:
	mutable std::mutex      mMutex;
	std::condition_variable mCV;
	bool                    mEntered = false;
	bool                    mReleased = false;
};

class ThrowingPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	explicit ThrowingPaneSink( bool throwUnknown = false )
		: mThrowUnknown( throwUnknown ) {}
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		if( mThrowUnknown ) throw 17;
		throw std::runtime_error( "intentional Last Render sink failure" );
	}
protected:
	~ThrowingPaneSink() override = default;
private:
	bool mThrowUnknown;
};

class DrainBlockingPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	void Arm()
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mArmed = true;
		mEntered = false;
		mRelease = false;
	}
	bool WaitUntilEntered( unsigned int timeoutMs )
	{
		std::unique_lock<std::mutex> lk( mMutex );
		return mCV.wait_for( lk, std::chrono::milliseconds( timeoutMs ),
			[&] { return mEntered; } );
	}
	void Release()
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mRelease = true;
		mArmed = false;
		mCV.notify_all();
	}
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		std::unique_lock<std::mutex> lk( mMutex );
		if( !mArmed ) return;
		mEntered = true;
		mCV.notify_all();
		mCV.wait_for( lk, std::chrono::milliseconds( 4000 ),
			[&] { return mRelease || !mArmed; } );
	}
protected:
	~DrainBlockingPaneSink() override = default;
private:
	std::mutex mMutex;
	std::condition_variable mCV;
	bool mArmed = false;
	bool mEntered = false;
	bool mRelease = false;
};

class ReentrantPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	enum class Action { RegisterSelf, FreezeOther, RegisterOtherSink, StopController, PrepareController };
	ReentrantPaneSink( Action action, unsigned int pane, unsigned int otherPane )
		: mAction( action ), mPane( pane ), mOtherPane( otherPane ) {}
	void Bind( SceneEditController* ctrl, IRasterizerOutput* otherSink = nullptr )
	{
		mCtrl = ctrl;
		mOtherSink = otherSink;
	}
	unsigned int Count() const { return mCount.load( std::memory_order_acquire ); }
	bool ActionSucceeded() const { return mActionSucceeded.load( std::memory_order_acquire ); }
	void Arm() { mArmed.store( true, std::memory_order_release ); }

	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		mCount.fetch_add( 1, std::memory_order_acq_rel );
		if( mAction == Action::StopController )
		{
			if( mArmed.load( std::memory_order_acquire ) )
			{
				mCtrl->Stop();
				mActionSucceeded.store( true, std::memory_order_release );
			}
			return;
		}
		if( mAction == Action::PrepareController )
		{
			if( mArmed.load( std::memory_order_acquire ) )
			{
				mActionSucceeded.store(
					!mCtrl->PrepareForDestruction(), std::memory_order_release );
			}
			return;
		}
		bool ok = false;
		if( mAction == Action::RegisterSelf )
			ok = mCtrl->SetPaneSink( mPane, this );
		else if( mAction == Action::FreezeOther )
			ok = mCtrl->SetPaneContentSource(
				mOtherPane, SceneEditController::PaneContentSource::LastRender );
		else
			ok = mCtrl->SetPaneSink( mOtherPane, mOtherSink );
		mActionSucceeded.store( ok, std::memory_order_release );
	}

protected:
	~ReentrantPaneSink() override = default;

private:
	Action                    mAction;
	unsigned int              mPane;
	unsigned int              mOtherPane;
	SceneEditController*      mCtrl = nullptr;
	IRasterizerOutput*        mOtherSink = nullptr;
	std::atomic<unsigned int> mCount { 0 };
	std::atomic<bool>         mActionSucceeded { false };
	std::atomic<bool>         mArmed { false };
};

class DestroyingPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	void Bind( SceneEditController* ctrl ) { mCtrl = ctrl; }
	bool DestroyReturned() const { return mDestroyReturned.load( std::memory_order_acquire ); }
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		if( mDestroyStarted.exchange( true, std::memory_order_acq_rel ) ) return;
		RISE_API_DestroySceneEditController( mCtrl );
		mDestroyReturned.store( true, std::memory_order_release );
	}
protected:
	~DestroyingPaneSink() override = default;
private:
	SceneEditController* mCtrl = nullptr;
	std::atomic<bool> mDestroyStarted { false };
	std::atomic<bool> mDestroyReturned { false };
};

struct DestructorDestroyState
{
	std::atomic<bool> detached { false };
	std::atomic<bool> destructorDestroyReturned { false };
};

struct DestructorReentryState
{
	std::atomic<bool> destructorRan { false };
	std::atomic<bool> setterReturned { false };
};

class DestructorReenteringPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	DestructorReenteringPaneSink( SceneEditController* ctrl, unsigned int pane,
		std::shared_ptr<DestructorReentryState> state, bool shrinkLayout = false )
		: mCtrl( ctrl ), mPane( pane ), mState( std::move( state ) )
		, mShrinkLayout( shrinkLayout ) {}
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override {}
protected:
	~DestructorReenteringPaneSink() override
	{
		// Redundant deregistration is representative owner cleanup and exercises
		// ordinary controller re-entry, not the explicit lifecycle rejection path.
		const bool returned = mShrinkLayout
			? mCtrl->SetViewportLayout( SceneEditController::ViewportLayout::Single )
			: mCtrl->SetPaneSink( mPane, nullptr );
		mState->setterReturned.store( returned, std::memory_order_release );
		mState->destructorRan.store( true, std::memory_order_release );
	}
private:
	SceneEditController* mCtrl;
	unsigned int mPane;
	std::shared_ptr<DestructorReentryState> mState;
	bool mShrinkLayout;
};

class DestructorQueuesLastRenderPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	DestructorQueuesLastRenderPaneSink( SceneEditController* ctrl,
		unsigned int targetPane, std::shared_ptr<DestructorReentryState> state )
		: mCtrl( ctrl ), mTargetPane( targetPane ), mState( std::move( state ) ) {}
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override {}
protected:
	~DestructorQueuesLastRenderPaneSink() override
	{
		mState->setterReturned.store(
			mCtrl->SetPaneContentSource(
				mTargetPane, SceneEditController::PaneContentSource::LastRender ),
			std::memory_order_release );
		mState->destructorRan.store( true, std::memory_order_release );
	}
private:
	SceneEditController* mCtrl;
	unsigned int mTargetPane;
	std::shared_ptr<DestructorReentryState> mState;
};

class DestructorRollsBackTransactionPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	DestructorRollsBackTransactionPaneSink( SceneEditController* ctrl,
		std::shared_ptr<DestructorReentryState> state )
		: mCtrl( ctrl ), mState( std::move( state ) ) {}
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override {}
protected:
	~DestructorRollsBackTransactionPaneSink() override
	{
		mState->setterReturned.store(
			mCtrl->RollbackTransaction(), std::memory_order_release );
		mState->destructorRan.store( true, std::memory_order_release );
	}
private:
	SceneEditController* mCtrl;
	std::shared_ptr<DestructorReentryState> mState;
};

class DestructorDestroyingPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	DestructorDestroyingPaneSink( SceneEditController* ctrl, unsigned int pane,
		std::shared_ptr<DestructorDestroyState> state )
		: mCtrl( ctrl ), mPane( pane ), mState( std::move( state ) ) {}
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		if( mDetached ) return;
		mDetached = true;
		mState->detached.store(
			mCtrl->SetPaneSink( mPane, nullptr ), std::memory_order_release );
	}
protected:
	~DestructorDestroyingPaneSink() override
	{
		RISE_API_DestroySceneEditController( mCtrl );
		mState->destructorDestroyReturned.store( true, std::memory_order_release );
	}
private:
	SceneEditController* mCtrl;
	unsigned int mPane;
	std::shared_ptr<DestructorDestroyState> mState;
	bool mDetached = false;
};

struct Fixture
{
	Job*                  job = nullptr;
	RecordingController*  ctrl = nullptr;
	IRasterizer*          rasterizer = nullptr;
	IRayCaster*           previewCaster = nullptr;
	IRayCaster*           polishCaster = nullptr;

	explicit Fixture( const char* tmpName, bool withInteractivePipeline = false,
		const char* sceneText = kScene )
	{
		const std::string path = WriteTemp( tmpName, sceneText );
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
		// Round-8: neutralize the pointer-gesture watchdog by default.  These
		// scenarios assert the PIN contract, which is wall-clock-independent;
		// leaving the production threshold live made them load-sensitive (a
		// heavily oversubscribed machine can starve a scenario past it, and
		// scenario (h) then legitimately observes the unpinned rotation and
		// fails).  0 means EXPLICITLY OFF -- review P1: "a very large
		// threshold" does NOT disable it, because the clock is steady-time
		// since boot, so an unstamped read exceeds any finite value.  The
		// watchdog's own behavior is covered by w1, which opts back in.
		ctrl->ForTest_SetPointerWatchdogMs( 0 );   // 0 = explicitly OFF
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

static bool CaptureNamedCamera( Job& job, const char* name,
	CameraSnapshot& out )
{
	const IScene* scene = job.GetScene();
	const ICameraManager* cameras = scene ? scene->GetCameras() : nullptr;
	const ICamera* camera = cameras ? cameras->GetItem( name ) : nullptr;
	return camera
	    && CameraIntrospection::CaptureCameraSnapshot( *camera, out );
}

static bool SameCameraPose( const CameraSnapshot& a,
	const CameraSnapshot& b, double eps = 1e-9 )
{
	for( int i = 0; i < 3; ++i ) {
		if( std::fabs( a.location[i] - b.location[i] ) > eps
		 || std::fabs( a.lookat[i] - b.lookat[i] ) > eps
		 || std::fabs( a.up[i] - b.up[i] ) > eps
		 || std::fabs( a.orientation[i] - b.orientation[i] ) > eps )
			return false;
	}
	for( int i = 0; i < 2; ++i ) {
		if( std::fabs(
			a.target_orientation[i] - b.target_orientation[i] ) > eps )
			return false;
	}
	return true;
}

struct CameraFrameSnapshot
{
	Point3 origin;
	Vector3 u;
	Vector3 v;
	Vector3 w;
	bool explicitONB = false;
};

static bool CaptureNamedCameraFrame( Job& job, const char* name,
	CameraFrameSnapshot& out )
{
	const IScene* scene = job.GetScene();
	const ICameraManager* cameras = scene ? scene->GetCameras() : nullptr;
	const ICamera* camera = cameras ? cameras->GetItem( name ) : nullptr;
	const Implementation::CameraCommon* common =
		dynamic_cast<const Implementation::CameraCommon*>( camera );
	if( !common ) return false;
	const OrthonormalBasis3D basis = common->GetCurrentBasis();
	out.origin = common->GetLocation();
	out.u = basis.u();
	out.v = basis.v();
	out.w = basis.w();
	out.explicitONB = common->IsFromONB();
	return true;
}

static bool SameCameraFrame( const CameraFrameSnapshot& a,
	const CameraFrameSnapshot& b, double eps = 1e-9,
	bool compareRepresentation = true )
{
	const Scalar* av[] = {
		&a.origin.x, &a.origin.y, &a.origin.z,
		&a.u.x, &a.u.y, &a.u.z,
		&a.v.x, &a.v.y, &a.v.z,
		&a.w.x, &a.w.y, &a.w.z
	};
	const Scalar* bv[] = {
		&b.origin.x, &b.origin.y, &b.origin.z,
		&b.u.x, &b.u.y, &b.u.z,
		&b.v.x, &b.v.y, &b.v.z,
		&b.w.x, &b.w.y, &b.w.z
	};
	for( unsigned int i = 0; i < sizeof( av ) / sizeof( av[0] ); ++i ) {
		if( std::fabs( *av[i] - *bv[i] ) > eps ) return false;
	}
	return !compareRepresentation || a.explicitONB == b.explicitONB;
}

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

	// Same-mode selection is also the retry affordance after a lazy caster or
	// BeautyVariant realization failure.  It must not be optimized into a
	// no-op merely because desired state already names this mode.
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPaneRenderMode( 3, "normals" ),
		"re-selecting the desired mode is accepted as a realization retry" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs )
	    && f.ctrl->SettlesAt( 1, kSettleMs ),
		"MONEY ASSERTION (b): same-mode retry re-renders exactly that pane" );
}

//----------------------------------------------------------------------
// (b2) Changing a pane's mode while its old-mode quantum is in flight
// must cancel that obsolete work.  This is load-bearing for shell-applied
// N-up presets: SetViewportLayout wakes newly visible panes before the
// shell can assign Wireframe/Normals/Depth, so a Preview quantum may win
// the mint race.
//----------------------------------------------------------------------
static void RunInFlightPaneModeChangeCancelsObsoletePassTest()
{
	std::printf( "=== scheduler (b2): in-flight pane mode change cancels the obsolete pass ===\n" );
	Fixture f( "pane_sched_b2.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	Check( f.ctrl->SetPrimaryPane( 1 ), "primary = pane 1" );
	f.ctrl->Start( true );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"layout-grow rotation settles" );
	f.ctrl->ClearSequence();

	f.ctrl->ArmReleaseInterleaving();
	const unsigned int cancelsBefore = f.ctrl->ForTest_GetCancelCount();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForMintedPass( kWaitMs )
	    && f.ctrl->ForTest_CurrentPane() == 1,
		"old Preview pass is deterministically held after mint on pane 1" );
	Check( f.ctrl->SetPaneRenderMode( 1, "wireframe" ),
		"switch pane 1 to its N-up preset mode" );
	Check( f.ctrl->ForTest_GetCancelCount() == cancelsBefore + 1,
		"MONEY ASSERTION (b2): mode setter cancels exactly one obsolete in-flight pass" );
	f.ctrl->ReleaseMintedPass();
	Check( f.ctrl->WaitForPassCount( 3, kWaitMs ),
		"cancelled Preview, still-dirty sibling, and replacement Wireframe passes retire" );
	Check( f.ctrl->SettlesAt( 3, kSettleMs ),
		"replacement completes once; no self-armed pass remains after the three owed quanta" );
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
	std::this_thread::sleep_for( std::chrono::milliseconds( kSettleMs ) );
	{
		const std::vector<unsigned int> initial = f.ctrl->Sequence();
		bool saw0 = false, saw1 = false;
		for( std::size_t i = 0; i < initial.size(); ++i ) {
			if( initial[i] == 0 ) saw0 = true;
			if( initial[i] == 1 ) saw1 = true;
		}
		Check( saw0 && saw1 && f.ctrl->SettlesAt( initial.size(), kSettleMs ),
		       "initial rotation covers both panes and quiesces" );
	}
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
	Check( f.ctrl->WaitForCompletedPassCount( 1, kWaitMs ),
		"the edit's single pass retires" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ),
	       "MONEY ASSERTION (d): ONE pass per edit -- the pane machinery adds no extra passes in "
	       "Single layout (byte-identical single-viewport behaviour)" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	bool allPane0 = !seq.empty();
	for( std::size_t i = 0; i < seq.size(); ++i ) {
		if( seq[i] != 0 ) allPane0 = false;
	}
	Check( allPane0, "every pass is pane 0" );
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
	Check( f.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
		"the two VISIBLE pane passes retire" );
	Check( f.ctrl->SettlesAt( 2, kSettleMs ),
		"rotation quiesces at the visible-pane count" );
	const std::vector<unsigned int> seq = f.ctrl->Sequence();
	bool sawHidden = false;
	for( std::size_t i = 0; i < seq.size(); ++i ) {
		if( seq[i] >= 2 ) sawHidden = true;
	}
	Check( !sawHidden,
	       "MONEY ASSERTION (e): panes 2/3 (hidden by the shrink) never render, dirty or not" );
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
	Check( f.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
		"initial rotation retires both visible-pane passes" );
	Check( f.ctrl->SettlesAt( 2, kSettleMs ),
		"initial rotation quiesces before gesture accounting starts" );
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
	std::this_thread::sleep_for( std::chrono::milliseconds( kSettleMs ) );
	const std::size_t orbitReleaseCount = f.ctrl->Sequence().size();
	Check( orbitReleaseCount >= 4
	    && f.ctrl->SettlesAt( orbitReleaseCount, kSettleMs ),
	       "orbit release sequence quiesces" );

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
	Check( f.ctrl->WaitForPassCount( orbitReleaseCount + 2, kWaitMs ),
	       "pointer release sequences drain before the free-fly twin checks" );
	std::this_thread::sleep_for( std::chrono::milliseconds( kSettleMs ) );
	const std::size_t releaseCount = f.ctrl->Sequence().size();
	Check( releaseCount >= orbitReleaseCount + 2
	    && f.ctrl->SettlesAt( releaseCount, kSettleMs ),
	       "pane-indexed gesture release sequences quiesce" );

	// review-s3 P2 coverage: hidden-pane and free-fly-twin contracts.
	Check( !f.ctrl->OnPanePointerDown( 3, Point2( 5, 5 ) ),
	       "pane-indexed Down on a HIDDEN pane (3 in TwoH) is refused" );
	Check( f.ctrl->PaneEnterFreeFly( 1 ), "PaneEnterFreeFly(1) accepted" );
	{
		String nv;
		Check( f.ctrl->WaitForPaneVantage( 1,
			SceneEditController::PaneVantageKind::FreeFly, nv, kWaitMs ),
		       "PaneEnterFreeFly set the pane's vantage to FreeFly" );
	}
	Check( f.ctrl->PaneExitFreeFly( 1 ), "PaneExitFreeFly(1) accepted" );
	{
		String nv;
		Check( f.ctrl->WaitForPaneVantage( 1,
			SceneEditController::PaneVantageKind::SceneCamera, nv, kWaitMs ),
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
	// WaitForPassCount observes pass START.  Use the recorder's separate
	// completion counter before ending the scrub; otherwise EndPropertyScrub
	// can race that quantum's completion transition and legitimately
	// coalesce/mint a different recovery sequence.
	Check( f.ctrl->WaitForCompletedPassCount( 1, kWaitMs ),
	       "surviving property-scrub quantum completes before its End transition" );
	f.ctrl->EndPropertyScrub();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
	       "concurrent-shrink scrub End resumes all three panes" );
	std::this_thread::sleep_for( std::chrono::milliseconds( kSettleMs ) );
	{
		const std::vector<unsigned int> recovery = f.ctrl->Sequence();
		bool saw0 = false, saw1 = false, saw2 = false, sawHidden = false;
		for( std::size_t i = 0; i < recovery.size(); ++i ) {
			if( recovery[i] == 0 ) saw0 = true;
			else if( recovery[i] == 1 ) saw1 = true;
			else if( recovery[i] == 2 ) saw2 = true;
			else sawHidden = true;
		}
		Check( saw0 && saw1 && saw2 && !sawHidden
		    && f.ctrl->SettlesAt( recovery.size(), kSettleMs ),
		       "concurrent-shrink recovery covers every visible pane and quiesces" );
	}
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
	// Let the legitimate adaptive refinement tail drain before taking the
	// quiescence baseline.  Sampling Sequence().size() immediately after the
	// fourth layout quantum raced that tail and made this unrelated setup
	// assertion intermittently fail on otherwise-correct runs.
	std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );
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
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->WaitForCompletedPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
	       "both TwoH panes retire exactly once" );
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
// (m2) Entering Free-Fly from a named scene-camera pane must seed from
//      that bound camera, not from the unrelated active scene camera.
//----------------------------------------------------------------------
static void RunPaneFreeFlyPreservesNamedSceneCameraTest()
{
	std::printf( "=== scheduler (m2): secondary Free-Fly preserves its named scene camera ===\n" );
	Fixture f( "pane_sched_m2.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"select clone source" );
	char clonedName[64] = {0};
	Check( f.ctrl->CloneActiveCamera( String( "side" ), clonedName, sizeof( clonedName ) ),
		"clone side camera" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "side" ) ),
		"select side for setup edit" );
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "6 2 9" ) ),
		"give side a pose distinct from cam" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"restore unrelated active cam" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "side" ),
		"bind inactive side camera" );
	Check( f.ctrl->PaneEnterFreeFly( 1 ),
		"enter Free-Fly from the named scene-camera pane" );

	SceneEditController::PaneVantageKind kind;
	String reference;
	Check( f.ctrl->GetPaneVantage( 1, kind, reference )
	    && kind == SceneEditController::PaneVantageKind::FreeFly,
		"pane 1 transitions from kind 3 to Free-Fly" );

	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"kind-3 Free-Fly transition realizes exactly [0,1]" );
	CameraSnapshot freeFlyPose;
	Check( f.ctrl->GetViewportPose( freeFlyPose, 1 )
	    && std::fabs( freeFlyPose.location[0] - 6.0 ) < 1e-9
	    && std::fabs( freeFlyPose.location[1] - 2.0 ) < 1e-9
	    && std::fabs( freeFlyPose.location[2] - 9.0 ) < 1e-9,
		"MONEY ASSERTION (m2): kind-3 to Free-Fly seeds from side, not active cam" );
}

//----------------------------------------------------------------------
// (n) A secondary pane can bind one manager camera by name.  It
//     re-resolves that camera after a live camera edit, then preserves
//     its binding-time snapshot if the camera is deleted.
//----------------------------------------------------------------------
static void RunNamedSceneCameraReconcileAndFallbackTest()
{
	std::printf( "=== scheduler (n): named scene-camera live re-resolve + deletion fallback ===\n" );
	Fixture f( "pane_sched_n.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"select source camera" );
	char clonedName[64] = {0};
	Check( f.ctrl->CloneActiveCamera( String( "side" ), clonedName, sizeof( clonedName ) ),
		"clone a durable second scene camera" );
	Check( std::string( clonedName ) == "side", "clone uses requested unique name" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "side" ) ),
		"select bound camera for the later property edit" );
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "4 1 7" ) ),
		"make the binding-time pose distinct from the remaining active-camera fallback" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"restore cam as active before initial default-vantage realization" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );

	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ),
		"initial TwoH rotation realizes both default SceneCamera panes" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->WaitForCompletedPassCount( 2, kWaitMs )
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"initial default-vantage rotation is exactly [0,1] and settles" );

	// Bind AFTER pane 1's initial realization.  This is the discriminating
	// shape for the desired-state hazard: without the setter's configDirty,
	// same-pane scheduling would keep the old SceneCamera realization.
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "side" ),
		"pane 1 binds the named scene camera after its initial realization" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
		"pane-local binding schedules one pass" );
	const std::vector<unsigned int> bindSeq = f.ctrl->Sequence();
	Check( bindSeq.size() == 1 && bindSeq[0] == 1,
		"MONEY ASSERTION (n): binding config-dirties and re-renders only pane 1" );
	Check( f.ctrl->WaitForCompletedPassCount( 1, kWaitMs )
	    && f.ctrl->SettlesAt( 1, kSettleMs ),
		"pane-local binding fully retires and settles" );

	CameraSnapshot bindingSnapshot;
	Check( f.ctrl->GetViewportPose( bindingSnapshot, 1 ),
		"read pane 1 binding-time camera snapshot" );
	SceneEditController::PaneVantageKind kind;
	String referencedName;
	Check( f.ctrl->GetPaneVantage( 1, kind, referencedName )
	    && kind == SceneEditController::PaneVantageKind::SceneCameraNamed
	    && referencedName == "side",
		"pane 1 keeps kind 3 plus its scene-camera reference" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "side" ) ),
		"select the bound camera for subsequent property edits" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->WaitForCompletedPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"camera activation refresh retires before property-edit accounting" );

	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "2 1 6" ) ),
		"edit the bound camera through the normal camera-property transaction" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ),
		"camera edit schedules a complete TwoH refresh" );
	Check( f.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
		"camera-edit refresh fully retires before exact sequence accounting" );
	const std::vector<unsigned int> cameraEditSeq = f.ctrl->Sequence();
	const bool cameraEditSettled = f.ctrl->SettlesAt( 2, kSettleMs );
	if( cameraEditSeq != std::vector<unsigned int>{ 0, 1 } || !cameraEditSettled )
	{
		std::printf( "  camera-edit sequence:" );
		for( unsigned int pane : cameraEditSeq ) std::printf( " %u", pane );
		std::printf( " (settled=%d)\n", cameraEditSettled ? 1 : 0 );
	}
	Check( cameraEditSeq == std::vector<unsigned int>{ 0, 1 } && cameraEditSettled,
		"camera-edit refresh is exactly [0,1] and settles" );
	CameraSnapshot editedSnapshot;
	Check( f.ctrl->GetViewportPose( editedSnapshot, 1 ),
		"read pane 1 after bound camera edit" );
	if( std::fabs( editedSnapshot.location[0] - 2.0 ) >= 1e-9
	 || std::fabs( editedSnapshot.location[1] - 1.0 ) >= 1e-9
	 || std::fabs( editedSnapshot.location[2] - 6.0 ) >= 1e-9 )
	{
		std::printf( "  camera-edit pane-1 pose: %.17g %.17g %.17g\n",
			editedSnapshot.location[0], editedSnapshot.location[1], editedSnapshot.location[2] );
	}
	Check( std::fabs( editedSnapshot.location[0] - 2.0 ) < 1e-9
	    && std::fabs( editedSnapshot.location[1] - 1.0 ) < 1e-9
	    && std::fabs( editedSnapshot.location[2] - 6.0 ) < 1e-9,
		"MONEY ASSERTION (n): camera edit invalidates pane 1 and live re-resolution "
		"updates its realized snapshot" );

	// Exercise both camera-classification paths used by propose_patch:
	// an explicit descriptor keyword and the conservative name-only fallback.
	f.ctrl->ClearSequence();
	const SceneEditController::AgentCommitResult explicitAgentEdit =
		f.ctrl->ApplyAgentParamEdit(
			String( "side" ), String( "pinhole_camera" ),
			String( "location" ), String( "3 2 8" ), nullptr );
	Check( explicitAgentEdit.applied, "explicit-kind agent camera edit applies" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ),
		"explicit-kind agent edit refreshes TwoH" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"explicit-kind agent refresh is exactly [0,1] and settles" );
	CameraSnapshot explicitAgentSnapshot;
	Check( f.ctrl->GetViewportPose( explicitAgentSnapshot, 1 )
	    && std::fabs( explicitAgentSnapshot.location[0] - 3.0 ) < 1e-9
	    && std::fabs( explicitAgentSnapshot.location[1] - 2.0 ) < 1e-9
	    && std::fabs( explicitAgentSnapshot.location[2] - 8.0 ) < 1e-9,
		"MONEY ASSERTION (n): explicit-kind agent camera edit re-resolves the bound pane" );

	f.ctrl->ClearSequence();
	const SceneEditController::AgentCommitResult genericAgentEdit =
		f.ctrl->ApplyAgentParamEdit(
			String( "side" ), String( "camera" ),
			String( "location" ), String( "3.5 2 8" ), nullptr );
	Check( genericAgentEdit.applied, "generic-kind agent camera edit applies" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"generic-kind agent refresh is exactly [0,1]" );
	CameraSnapshot genericAgentSnapshot;
	Check( f.ctrl->GetViewportPose( genericAgentSnapshot, 1 )
	    && std::fabs( genericAgentSnapshot.location[0] - 3.5 ) < 1e-9
	    && std::fabs( genericAgentSnapshot.location[1] - 2.0 ) < 1e-9
	    && std::fabs( genericAgentSnapshot.location[2] - 8.0 ) < 1e-9,
		"MONEY ASSERTION (n): generic `camera` agent kind re-resolves the bound pane" );

	f.ctrl->ClearSequence();
	const SceneEditController::AgentCommitResult agentEdit =
		f.ctrl->ApplyAgentParamEdit(
			String( "side" ), String(), String( "location" ), String( "4 2 8" ), nullptr );
	Check( agentEdit.applied, "kind-omitted agent camera edit applies" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ),
		"kind-omitted agent edit refreshes TwoH" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"kind-omitted agent refresh is exactly [0,1] and settles" );
	CameraSnapshot agentSnapshot;
	Check( f.ctrl->GetViewportPose( agentSnapshot, 1 )
	    && std::fabs( agentSnapshot.location[0] - 4.0 ) < 1e-9
	    && std::fabs( agentSnapshot.location[1] - 2.0 ) < 1e-9
	    && std::fabs( agentSnapshot.location[2] - 8.0 ) < 1e-9,
		"MONEY ASSERTION (n): a kind-omitted agent camera edit re-resolves the bound pane" );

	// Undo and redo each mutate the live camera without going through the
	// property setter again.  Consume the forward realization first so each
	// subsequent exact refresh proves its own invalidation branch.
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "5 2 8" ) ),
		"apply forward camera edit for undo/redo coverage" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"forward undo/redo pose refresh is exactly [0,1]" );
	f.ctrl->ClearSequence();
	f.ctrl->Undo();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"undo camera property edit refreshes exactly [0,1]" );
	CameraSnapshot undoSnapshot;
	Check( f.ctrl->GetViewportPose( undoSnapshot, 1 )
	    && std::fabs( undoSnapshot.location[0] - 4.0 ) < 1e-9,
		"MONEY ASSERTION (n): undo invalidates and restores the bound pane pose" );
	f.ctrl->ClearSequence();
	f.ctrl->Redo();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"redo camera property edit refreshes exactly [0,1]" );
	CameraSnapshot redoSnapshot;
	Check( f.ctrl->GetViewportPose( redoSnapshot, 1 )
	    && std::fabs( redoSnapshot.location[0] - 5.0 ) < 1e-9,
		"MONEY ASSERTION (n): redo invalidates and reapplies the bound pane pose" );

	// Transaction rollback is another camera mutation.  Let the forward
	// pose render first so its configDirty is consumed, then roll it back.
	Check( f.ctrl->BeginTransaction(), "begin camera-edit transaction" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "6 3 9" ) ),
		"transactional camera edit applies" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "forward transaction pose renders" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"forward transaction refresh is exactly [0,1] and settles" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->RollbackTransaction(), "camera-edit transaction rolls back" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ), "rollback refreshes TwoH" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"rollback refresh is exactly [0,1] and settles" );
	CameraSnapshot rollbackSnapshot;
	Check( f.ctrl->GetViewportPose( rollbackSnapshot, 1 )
	    && std::fabs( rollbackSnapshot.location[0] - 5.0 ) < 1e-9
	    && std::fabs( rollbackSnapshot.location[1] - 2.0 ) < 1e-9
	    && std::fabs( rollbackSnapshot.location[2] - 8.0 ) < 1e-9,
		"MONEY ASSERTION (n): rollback discards the rejected realized pose and "
		"re-resolves the restored camera" );

	// Hidden panes retain desired state but are not scheduled.  Editing the
	// camera while pane 1 is hidden must still config-dirty its slot so the
	// later reveal cannot resurrect the stale realization.
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Single ),
		"hide bound pane 1" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
		"layout shrink refreshes the visible primary" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0 }
	    && f.ctrl->SettlesAt( 1, kSettleMs ),
		"layout shrink refresh is exactly [0] and settles" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "7 4 10" ) ),
		"edit bound camera while pane 1 is hidden" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
		"hidden-pane edit refreshes the one visible pane only" );
	Check( f.ctrl->SettlesAt( 1, kSettleMs ), "Single refresh settles" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"reveal the hidden bound pane" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
		"reveal schedules the formerly-hidden pane" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 1 }
	    && f.ctrl->SettlesAt( 1, kSettleMs ),
		"reveal refresh is exactly [1] and settles" );
	CameraSnapshot revealedSnapshot;
	Check( f.ctrl->GetViewportPose( revealedSnapshot, 1 )
	    && std::fabs( revealedSnapshot.location[0] - 7.0 ) < 1e-9
	    && std::fabs( revealedSnapshot.location[1] - 4.0 ) < 1e-9
	    && std::fabs( revealedSnapshot.location[2] - 10.0 ) < 1e-9,
		"MONEY ASSERTION (n): hidden bound pane re-resolves the camera on reveal" );

	f.ctrl->ClearSequence();
	const SceneEditController::AgentCommitResult removed =
		f.ctrl->RemoveEntity( SceneEditController::Category::Camera, String( "side" ) );
	Check( removed.applied, "remove the bound scene camera" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs ),
		"camera deletion schedules a complete TwoH refresh" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"camera-deletion refresh is exactly [0,1] and settles" );
	CameraSnapshot fallbackSnapshot;
	Check( f.ctrl->GetViewportPose( fallbackSnapshot, 1 ),
		"read pane 1 after the bound camera is deleted" );
	Check( std::fabs( fallbackSnapshot.location[0] - bindingSnapshot.location[0] ) < 1e-9
	    && std::fabs( fallbackSnapshot.location[1] - bindingSnapshot.location[1] ) < 1e-9
	    && std::fabs( fallbackSnapshot.location[2] - bindingSnapshot.location[2] ) < 1e-9,
		"MONEY ASSERTION (n): deleting the bound camera preserves the binding-time "
		"snapshot fallback instead of jumping to the active scene camera" );
	Check( f.ctrl->GetPaneVantage( 1, kind, referencedName )
	    && kind == SceneEditController::PaneVantageKind::SceneCameraNamed
	    && referencedName == "side",
		"deletion preserves the desired kind/name for a future same-name camera" );

	// Recreate the deleted name.  This is the inverse reconciliation edge:
	// the pane must leave its old fallback without requiring the user to
	// re-select the desired camera in the menu.
	f.ctrl->ClearSequence();
	char recreatedName[64] = {0};
	Check( f.ctrl->CloneActiveCamera(
			String( "side" ), recreatedName, sizeof( recreatedName ) ),
		"recreate the deleted camera name from the active cam" );
	Check( std::string( recreatedName ) == "side",
		"same-name recreation reuses the retained desired identity" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"same-name recreation refresh is exactly [0,1]" );
	CameraSnapshot recreatedSnapshot;
	Check( f.ctrl->GetViewportPose( recreatedSnapshot, 1 )
	    && std::fabs( recreatedSnapshot.location[0] - 0.0 ) < 1e-9
	    && std::fabs( recreatedSnapshot.location[1] - 0.0 ) < 1e-9
	    && std::fabs( recreatedSnapshot.location[2] - 5.0 ) < 1e-9,
		"MONEY ASSERTION (n): same-name recreation wakes the pane off its deletion fallback" );
}

//----------------------------------------------------------------------
// (n2) StampViewToNewCamera is the other manager-camera creation path.
//      It must also wake retained same-name bindings after deletion.
//----------------------------------------------------------------------
static void RunNamedSceneCameraStampRecreationTest()
{
	std::printf( "=== scheduler (n2): stamped same-name camera resumes live binding ===\n" );
	Fixture f( "pane_sched_n2.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"select clone source" );
	char clonedName[64] = {0};
	Check( f.ctrl->CloneActiveCamera( String( "side" ), clonedName, sizeof( clonedName ) ),
		"create side camera" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "side" ) ),
		"select side for setup edit" );
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "4 1 7" ) ),
		"give side a distinct fallback pose" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"restore cam active" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "side" ),
		"bind pane 1 to side" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"initial binding rotation settles" );

	f.ctrl->ClearSequence();
	Check( f.ctrl->RemoveEntity(
			SceneEditController::Category::Camera, String( "side" ) ).applied,
		"delete side while retaining pane desired state" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"deletion refresh is exactly [0,1]" );
	CameraSnapshot fallback;
	Check( f.ctrl->GetViewportPose( fallback, 1 )
	    && std::fabs( fallback.location[0] - 4.0 ) < 1e-9,
		"pane uses stored fallback after deletion" );

	f.ctrl->ClearSequence();
	Check( f.ctrl->EnterFreeFlyFromActiveCamera( 0 ),
		"prepare pane-0 active-camera view for stamping" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"pane-0 Free-Fly preparation refresh is exactly [0,1]" );

	f.ctrl->ClearSequence();
	char stampedName[64] = {0};
	Check( f.ctrl->StampViewToNewCamera(
			String( "side" ), stampedName, sizeof( stampedName ), 0 ),
		"stamp a new camera under the deleted desired name" );
	Check( std::string( stampedName ) == "side", "stamp recreates exact desired identity" );
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"stamp recreation refresh is exactly [0,1]" );
	CameraSnapshot resumed;
	Check( f.ctrl->GetViewportPose( resumed, 1 )
	    && std::fabs( resumed.location[0] - 0.0 ) < 1e-9
	    && std::fabs( resumed.location[1] - 0.0 ) < 1e-9
	    && std::fabs( resumed.location[2] - 5.0 ) < 1e-9,
		"MONEY ASSERTION (n2): stamp recreation wakes pane 1 off the old fallback" );
}

//----------------------------------------------------------------------
// (n3) A pane-0 camera gesture edits the active scene camera.  Secondary
//      panes explicitly bound to that same camera must re-resolve afterward.
//----------------------------------------------------------------------
static void RunActiveCameraGestureInvalidatesNamedPaneTest()
{
	std::printf( "=== scheduler (n3): active-camera gesture updates bound secondary ===\n" );
	Fixture f( "pane_sched_n3.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"select active camera" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "cam" ),
		"bind secondary to the active camera by name" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	// A pass is recorded at DoOneRenderPass entry, before its retirement hook.
	// Wait for both retirements before reading the pose or clearing the recorder;
	// otherwise a still-active pane-1 pass can make GetViewportPose's try-lock
	// fail and append into the next gesture phase after ClearSequence().
	Check( f.ctrl->WaitForCompletedPassCount( 2, kWaitMs )
	    && f.ctrl->SettlesAt( 2, kSettleMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 },
		"initial bound-camera rotation is exactly [0,1]" );
	CameraSnapshot before;
	Check( f.ctrl->GetViewportPose( before, 1 ), "capture bound pane before gesture" );

	f.ctrl->ClearSequence();
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( f.ctrl->OnPanePointerDown( 0, Point2( 10, 10 ) ),
		"pane-0 camera gesture starts" );
	Check( f.ctrl->OnPanePointerMove( 0, Point2( 30, 18 ) ),
		"pane-0 orbit mutates active cam" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
		"at least one pane-0 gesture quantum lands" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	const std::vector<unsigned int> gestureSeq = f.ctrl->Sequence();
	bool gestureOnlyPane0 = !gestureSeq.empty();
	for( std::size_t i = 0; i < gestureSeq.size(); ++i ) {
		if( gestureSeq[i] != 0 ) gestureOnlyPane0 = false;
	}
	Check( gestureOnlyPane0, "every gesture quantum observed is pinned to pane 0" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->OnPanePointerUp( 0, Point2( 30, 18 ) ),
		"pane-0 camera gesture ends" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
		"gesture release starts its refresh chain" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );
	const std::vector<unsigned int> releaseSeq = f.ctrl->Sequence();
	bool releaseSawBoundPane = false;
	for( std::size_t i = 0; i < releaseSeq.size(); ++i ) {
		if( releaseSeq[i] == 1 ) releaseSawBoundPane = true;
	}
	Check( releaseSawBoundPane
	    && f.ctrl->SettlesAt( releaseSeq.size(), kSettleMs ),
		"gesture release refreshes the bound pane and settles" );

	CameraSnapshot after;
	Check( f.ctrl->GetViewportPose( after, 1 )
	    && ( std::fabs( after.target_orientation[0] - before.target_orientation[0] ) > 1e-9
	      || std::fabs( after.target_orientation[1] - before.target_orientation[1] ) > 1e-9 ),
		"MONEY ASSERTION (n3): active-camera orbit invalidates and re-resolves bound pane 1" );
}

//----------------------------------------------------------------------
// (n3b/T3) Camera tools in a SceneCameraNamed secondary edit that exact
//          manager camera through history.  They never activate it, never
//          convert the pane to FreeFly, and undo restores the named target.
//          The same realized pane camera also drives projection + gizmos.
//----------------------------------------------------------------------
static void RunNamedCameraGestureRoutingTest()
{
	std::printf( "=== scheduler (n3b/T3): named-pane tools edit/undo the bound camera ===\n" );
	Fixture f( "pane_sched_n3b.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection(
			SceneEditController::Category::Camera, String( "cam" ) ),
		"select clone source" );
	char clonedName[64] = {0};
	Check( f.ctrl->CloneActiveCamera(
			String( "side" ), clonedName, sizeof( clonedName ) ),
		"create inactive side camera" );
	Check( f.ctrl->SetSelection(
			SceneEditController::Category::Camera, String( "side" ) ),
		"select side for setup" );
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "6 2 9" ) ),
		"give side a distinct eye" );
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "lookat" ), String( "1 0 0" ) ),
		"give side a distinct optical axis" );
	Check( f.ctrl->SetSelection(
			SceneEditController::Category::Camera, String( "cam" ) ),
		"restore cam as the unrelated active camera" );
	Check( f.ctrl->SetViewportLayout(
			SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "side" ),
		"bind pane 1 to inactive side" );

	CameraSnapshot activeBefore;
	CameraSnapshot sideRest;
	Check( CaptureNamedCamera( *f.job, "cam", activeBefore )
	    && CaptureNamedCamera( *f.job, "side", sideRest ),
		"capture both manager-camera baselines" );

	struct ToolCase {
		SceneEditController::Tool tool;
		const char* name;
		Point2 move;
	};
	const ToolCase cases[] = {
		{ SceneEditController::Tool::OrbitCamera, "orbit", Point2( 36, 24 ) },
		{ SceneEditController::Tool::PanCamera,   "pan",   Point2( 32, 28 ) },
		{ SceneEditController::Tool::ZoomCamera,  "zoom",  Point2( 10, 34 ) },
		{ SceneEditController::Tool::RollCamera,  "roll",  Point2( 38, 10 ) }
	};
	for( const ToolCase& tc : cases )
	{
		f.ctrl->SetTool( tc.tool );
		Check( f.ctrl->OnPanePointerDown( 1, Point2( 10, 10 ) ),
			std::string( tc.name ) + " Down accepted on named pane" );
		Check( f.ctrl->OnPanePointerMove( 1, tc.move ),
			std::string( tc.name ) + " Move accepted on named pane" );
		Check( f.ctrl->OnPanePointerUp( 1, tc.move ),
			std::string( tc.name ) + " Up accepted on named pane" );

		CameraSnapshot activeAfter;
		CameraSnapshot sideAfter;
		Check( f.job->GetActiveCameraName() == "cam"
		    && CaptureNamedCamera( *f.job, "cam", activeAfter )
		    && SameCameraPose( activeAfter, activeBefore ),
			std::string( "MONEY (T3/" ) + tc.name
			+ "): active identity and pose stay untouched" );
		Check( CaptureNamedCamera( *f.job, "side", sideAfter )
		    && !SameCameraPose( sideAfter, sideRest ),
			std::string( "MONEY (T3/" ) + tc.name
			+ "): bound inactive camera pose changes" );
		SceneEditController::PaneVantageKind kind;
		String reference;
		Check( f.ctrl->GetPaneVantage( 1, kind, reference )
		    && kind == SceneEditController::PaneVantageKind::SceneCameraNamed
		    && reference == "side",
			std::string( "MONEY (T3/" ) + tc.name
			+ "): pane remains kind 3, bound to side" );

		f.ctrl->Undo();
		CameraSnapshot sideUndo;
		Check( CaptureNamedCamera( *f.job, "side", sideUndo )
		    && SameCameraPose( sideUndo, sideRest ),
			std::string( "MONEY (T3/" ) + tc.name
			+ "): composite undo restores the named camera" );
		Check( f.job->GetActiveCameraName() == "cam",
			std::string( "T3/" ) + tc.name
			+ ": undo does not activate the target" );
	}

	// Run one named-camera edit with the scheduler live and TWO panes bound
	// to the same inactive camera.  This specifically guards the gesture
	// caller's exact-name configDirty route: global render dirtiness alone
	// can repaint a stale standalone override without reconciling its pose.
	Check( f.ctrl->SetViewportLayout(
			SceneEditController::ViewportLayout::Quad ),
		"grow to Quad for live reconciliation" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 2, "side" ),
		"bind a second pane to the same inactive side camera" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
		"initial live Quad rotation completes" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );
	Check( f.ctrl->SettlesAt(
			f.ctrl->Sequence().size(), kSettleMs ),
		"initial live Quad rotation settles" );

	f.ctrl->ClearSequence();
	f.ctrl->SetTool( SceneEditController::Tool::PanCamera );
	Check( f.ctrl->OnPanePointerDown( 1, Point2( 10, 10 ) ),
		"live named-camera pan starts on pane 1" );
	Check( f.ctrl->OnPanePointerMove( 1, Point2( 34, 26 ) ),
		"live named-camera pan mutates side" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ),
		"gesture-pinned pane 1 realizes the edited side pose" );

	f.ctrl->ClearSequence();
	Check( f.ctrl->OnPanePointerUp( 1, Point2( 34, 26 ) ),
		"live named-camera pan ends" );
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
		"gesture release resumes the Quad refresh" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
	const std::vector<unsigned int> releaseSeq = f.ctrl->Sequence();
	bool releaseSawPane2 = false;
	for( std::size_t i = 0; i < releaseSeq.size(); ++i ) {
		if( releaseSeq[i] == 2 ) releaseSawPane2 = true;
	}
	Check( releaseSawPane2
	    && f.ctrl->SettlesAt( releaseSeq.size(), kSettleMs ),
		"MONEY (T3/reconcile): release refreshes the sibling pane bound to side" );
	f.ctrl->Stop();
	CameraSnapshot liveSide;
	CameraSnapshot livePane1;
	CameraSnapshot livePane2;
	Check( CaptureNamedCamera( *f.job, "side", liveSide )
	    && f.ctrl->GetViewportPose( livePane1, 1 )
	    && f.ctrl->GetViewportPose( livePane2, 2 )
	    && SameCameraPose( livePane1, liveSide )
	    && SameCameraPose( livePane2, liveSide ),
		"MONEY (T3/reconcile): every side-bound pane realizes the edited manager pose" );
	f.ctrl->Undo();
	CameraSnapshot liveUndo;
	Check( CaptureNamedCamera( *f.job, "side", liveUndo )
	    && SameCameraPose( liveUndo, sideRest ),
		"live reconciliation gesture undo restores side" );
	Check( f.ctrl->SetViewportLayout(
			SceneEditController::ViewportLayout::TwoH ),
		"restore TwoH after the live reconciliation check" );

	// Switch the live register to pane 0 and record a non-central world
	// projection through the active camera.
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( f.ctrl->OnPanePointerDown( 0, Point2( 10, 10 ) ),
		"empty pane-0 navigation gesture switches the effective camera" );
	double activeX = 0, activeY = 0;
	Check( f.ctrl->ForTest_ProjectWorldToScreen(
			1, 0, 0, activeX, activeY ),
		"project through pane 0 active camera" );
	Check( f.ctrl->OnPanePointerUp( 0, Point2( 10, 10 ) ),
		"finish empty pane-0 gesture" );

	Check( f.ctrl->OnPanePointerDown( 1, Point2( 10, 10 ) ),
		"empty pane-1 navigation gesture switches to bound camera" );
	double boundX = 0, boundY = 0;
	Check( f.ctrl->ForTest_ProjectWorldToScreen(
			1, 0, 0, boundX, boundY ),
		"project through pane 1 bound camera" );
	Check( std::fabs( boundX - activeX ) > 1e-6
	    || std::fabs( boundY - activeY ) > 1e-6,
		"MONEY (T3/projection): named pane projection differs from active-camera projection" );
	Check( f.ctrl->OnPanePointerUp( 1, Point2( 10, 10 ) ),
		"finish empty pane-1 gesture" );

	f.ctrl->ForTest_SetSelection(
		SceneEditController::Category::Object, String( "obj" ) );
	Check( f.ctrl->SetPrimaryPane( 1 ),
		"make the named pane the gizmo-owning primary" );
	f.ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	f.ctrl->RefreshGizmoHandles();
	double pivotX = 0, pivotY = 0;
	Check( f.ctrl->ForTest_ProjectWorldToScreen(
			0, 0, 0, pivotX, pivotY ),
		"project selected-object pivot through the bound camera" );
	int center = -1;
	for( unsigned int i = 0; i < f.ctrl->GizmoHandleCount(); ++i ) {
		if( f.ctrl->GizmoHandleKind( i )
		 == static_cast<int>(
			SceneEditController::GizmoHandle::Kind::ScreenCenter ) )
		{
			center = static_cast<int>( i );
			break;
		}
	}
	Check( center >= 0, "named-pane object gizmo has a screen-center handle" );
	if( center >= 0 ) {
		const unsigned int i = static_cast<unsigned int>( center );
		Check( std::fabs( f.ctrl->GizmoHandleScreenX( i ) - pivotX ) < 1e-6
		    && std::fabs( f.ctrl->GizmoHandleScreenY( i ) - pivotY ) < 1e-6,
			"MONEY (T3/gizmo): handle projection follows pane 1's bound camera" );
	}

	Check( f.ctrl->RemoveEntity(
			SceneEditController::Category::Camera, String( "side" ) ).applied,
		"delete the bound camera, leaving pane 1 on its snapshot fallback" );
	CameraSnapshot activeBeforeRefusedGesture;
	Check( CaptureNamedCamera(
			*f.job, "cam", activeBeforeRefusedGesture ),
		"capture active camera before fallback navigation attempt" );
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( !f.ctrl->OnPanePointerDown( 1, Point2( 10, 10 ) ),
		"MONEY (T3/deletion): camera navigation refuses a snapshot-only fallback" );
	CameraSnapshot activeAfterRefusedGesture;
	Check( f.job->GetActiveCameraName() == "cam"
	    && CaptureNamedCamera( *f.job, "cam", activeAfterRefusedGesture )
	    && SameCameraPose(
			activeAfterRefusedGesture, activeBeforeRefusedGesture ),
		"MONEY (T3/deletion): refusal cannot spill onto the active camera" );
}

//----------------------------------------------------------------------
// (n4) Set Home must resolve desired kind-3 state even before that pane has
//      ever run.  Falling back to the active camera would save the wrong view.
//----------------------------------------------------------------------
static void RunNamedSceneCameraPreRealizationHomeTest()
{
	std::printf( "=== scheduler (n4): pre-realization Home resolves named scene camera ===\n" );
	Fixture f( "pane_sched_n4.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"select clone source" );
	char clonedName[64] = {0};
	Check( f.ctrl->CloneActiveCamera( String( "side" ), clonedName, sizeof( clonedName ) ),
		"create side camera" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "side" ) ),
		"select side for setup edit" );
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "6 2 9" ) ),
		"give side a distinct Home pose" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"restore unrelated active cam" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "side" ),
		"bind side before any pane realization" );
	Check( f.ctrl->SetHomeView( 1 ),
		"Set Home resolves the configured named camera before first pass" );

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "side" ) ),
		"select side for post-Home edit" );
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "2 1 7" ) ),
		"move live side away from the saved Home pose" );
	Check( f.ctrl->GoToHomeView( 1 ), "Go Home restores saved pane-1 pose" );

	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"Home realization rotation is exactly [0,1]" );
	CameraSnapshot home;
	Check( f.ctrl->GetViewportPose( home, 1 )
	    && std::fabs( home.location[0] - 6.0 ) < 1e-9
	    && std::fabs( home.location[1] - 2.0 ) < 1e-9
	    && std::fabs( home.location[2] - 9.0 ) < 1e-9,
		"MONEY ASSERTION (n4): pre-realization Home captured side, not active cam" );
}

//----------------------------------------------------------------------
// (n5) Exact-name invalidation is one-to-many: every pane bound to the
//      camera must be marked configDirty, not only the first match.
//----------------------------------------------------------------------
static void RunMultiplePanesBoundToSameCameraTest()
{
	std::printf( "=== scheduler (n5): one camera edit updates every bound pane ===\n" );
	Fixture f( "pane_sched_n5.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"select clone source" );
	char clonedName[64] = {0};
	Check( f.ctrl->CloneActiveCamera( String( "side" ), clonedName, sizeof( clonedName ) ),
		"create side camera" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"keep cam active" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "side" )
	    && f.ctrl->SetPaneVantageSceneCameraNamed( 2, "side" ),
		"bind panes 1 and 2 to the same inactive camera" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1, 2, 3 }
	    && f.ctrl->SettlesAt( 4, kSettleMs ),
		"initial Quad rotation is exactly [0,1,2,3]" );

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "side" ) ),
		"select the shared bound camera" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPropertyForCategory(
			SceneEditController::Category::Camera,
			String( "location" ), String( "7 3 11" ) ),
		"edit shared bound camera" );
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1, 2, 3 }
	    && f.ctrl->SettlesAt( 4, kSettleMs ),
		"shared-camera edit refresh is exactly [0,1,2,3]" );
	CameraSnapshot pane1;
	CameraSnapshot pane2;
	Check( f.ctrl->GetViewportPose( pane1, 1 )
	    && f.ctrl->GetViewportPose( pane2, 2 )
	    && std::fabs( pane1.location[0] - 7.0 ) < 1e-9
	    && std::fabs( pane1.location[1] - 3.0 ) < 1e-9
	    && std::fabs( pane1.location[2] - 11.0 ) < 1e-9
	    && std::fabs( pane2.location[0] - 7.0 ) < 1e-9
	    && std::fabs( pane2.location[1] - 3.0 ) < 1e-9
	    && std::fabs( pane2.location[2] - 11.0 ) < 1e-9,
		"MONEY ASSERTION (n5): exact-name invalidation updates every matching pane" );

}

//----------------------------------------------------------------------
// (o) Time evaluation may animate inactive named cameras.  A bound pane
//     must discard its standalone camera and capture the evaluated pose.
//----------------------------------------------------------------------
static void RunNamedSceneCameraTimeScrubTest()
{
	std::printf( "=== scheduler (o): named scene-camera follows timeline scrub ===\n" );
	std::string animatedScene( kScene );
	animatedScene +=
		"\npinhole_camera\n{\n"
		"\tname animated_side_a\n"
		"\tlocation 1 0 5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 45\n"
		"}\n"
		"\npinhole_camera\n{\n"
		"\tname animated_side_b\n"
		"\tlocation 2 0 5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 45\n"
		"}\n"
		"\ntimeline\n{\n"
		"\telement_type camera\n"
		"\telement animated_side_a\n"
		"\tparam location\n"
		"\ttime 0\n"
		"\tvalue 1 0 5\n"
		"\ttime 1\n"
		"\tvalue 8 0 5\n"
		"}\n"
		"\ntimeline\n{\n"
		"\telement_type camera\n"
		"\telement animated_side_b\n"
		"\tparam location\n"
		"\ttime 0\n"
		"\tvalue 2 0 5\n"
		"\ttime 1\n"
		"\tvalue 9 0 5\n"
		"}\n";
	Fixture f( "pane_sched_o.RISEscene", false, animatedScene.c_str() );
	Check( f.ctrl != nullptr, "animated-camera fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"keep cam active while two inactive animated cameras are pane-bound" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "animated_side_a" )
	    && f.ctrl->SetPaneVantageSceneCameraNamed( 2, "animated_side_b" ),
		"panes 1 and 2 bind distinct inactive animated cameras" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
		"initial animated-camera Quad rotation runs" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1, 2, 3 }
	    && f.ctrl->SettlesAt( 4, kSettleMs ),
		"initial animated-camera rotation is exactly [0,1,2,3] and settles" );

	f.ctrl->ClearSequence();
	Check( f.ctrl->OnTimeScrub( Scalar( 1 ) ), "scrub scene time to 1" );
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs ),
		"time scrub refreshes all Quad panes" );
	Check( f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1, 2, 3 }
	    && f.ctrl->SettlesAt( 4, kSettleMs ),
		"time-scrub refresh is exactly [0,1,2,3] and settles" );
	CameraSnapshot scrubbedA;
	CameraSnapshot scrubbedB;
	Check( f.ctrl->GetViewportPose( scrubbedA, 1 )
	    && f.ctrl->GetViewportPose( scrubbedB, 2 )
	    && std::fabs( scrubbedA.location[0] - 8.0 ) < 1e-9
	    && std::fabs( scrubbedB.location[0] - 9.0 ) < 1e-9,
		"MONEY ASSERTION (o): time scrub's all-helper updates every inactive named-camera pane" );
}

//----------------------------------------------------------------------
// (p) ONB pinhole cameras are valid manager cameras.  Binding one must
//     realize a ray-equivalent ordinary pinhole snapshot rather than reject
//     the authored ONB camera as the persistent clone path intentionally does.
//----------------------------------------------------------------------
static void RunNamedSceneCameraONBTest()
{
	std::printf( "=== scheduler (p): named ONB scene-camera binding ===\n" );
	std::string onbScene( kScene );
	onbScene +=
		"\nonb_pinhole_camera\n{\n"
		"\tname onb_side\n"
		"\tlocation 9 8 7\n"
		"\tva 1 0 0\n"
		"\tvb 0 1 0\n"
		"\tcomponents UV\n"
		"\tfov 31\n"
		"\texposure 0.125\n"
		"\tscanning_rate 0.25\n"
		"\tpixel_rate 0.5\n"
		"}\n";
	Fixture f( "pane_sched_p.RISEscene", false, onbScene.c_str() );
	Check( f.ctrl != nullptr, "ONB fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"keep ordinary cam active while binding inactive ONB camera" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	Check( f.ctrl->SetPaneVantageSceneCameraNamed( 1, "onb_side" ),
		"bind supported onb_pinhole_camera by manager name" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->Sequence() == std::vector<unsigned int>{ 0, 1 }
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"ONB binding rotation is exactly [0,1]" );

	CameraSnapshot onbPose;
	Check( f.ctrl->GetViewportPose( onbPose, 1 )
	    && onbPose.type == CameraSnapshot::Pinhole
	    && std::fabs( onbPose.location[0] - 9.0 ) < 1e-9
	    && std::fabs( onbPose.location[1] - 8.0 ) < 1e-9
	    && std::fabs( onbPose.location[2] - 7.0 ) < 1e-9
	    && std::fabs( onbPose.lookat[0] - 9.0 ) < 1e-9
	    && std::fabs( onbPose.lookat[1] - 8.0 ) < 1e-9
	    && std::fabs( onbPose.lookat[2] - 8.0 ) < 1e-9
	    && std::fabs( onbPose.up[0] ) < 1e-9
	    && std::fabs( onbPose.up[1] - 1.0 ) < 1e-9
	    && std::fabs( onbPose.up[2] ) < 1e-9
	    && std::fabs( onbPose.fov - 31.0 * DEG_TO_RAD ) < 1e-9
	    && std::fabs( onbPose.exposure - 0.125 ) < 1e-9
	    && std::fabs( onbPose.scanningRate - 0.25 ) < 1e-9
	    && std::fabs( onbPose.pixelRate - 0.5 ) < 1e-9,
		"MONEY ASSERTION (p): ONB basis and optics are captured as an equivalent navigable pinhole pose" );

	// The render thread is no longer needed; stopping makes each gesture /
	// commit / undo deterministic while still exercising the retained CST.
	f.ctrl->Stop();
	CameraFrameSnapshot onbRest;
	CameraSnapshot activeRest;
	Check( CaptureNamedCameraFrame( *f.job, "onb_side", onbRest )
	    && onbRest.explicitONB
	    && CaptureNamedCamera( *f.job, "cam", activeRest ),
		"capture explicit-ONB and active-camera baselines" );
	struct ONBToolCase {
		SceneEditController::Tool tool;
		const char* name;
		Point2 move;
	};
	const ONBToolCase cases[] = {
		{ SceneEditController::Tool::OrbitCamera, "orbit", Point2( 36, 24 ) },
		{ SceneEditController::Tool::PanCamera,   "pan",   Point2( 32, 28 ) },
		{ SceneEditController::Tool::ZoomCamera,  "zoom",  Point2( 10, 34 ) },
		{ SceneEditController::Tool::RollCamera,  "roll",  Point2( 38, 10 ) }
	};
	for( const ONBToolCase& tc : cases )
	{
		// Independent ray-equivalent ordinary camera: this is the contract
		// the ONB adapter must implement.  Applying the canonical tool math
		// here gives an exact forward oracle for origin + U/V/W; a sign error
		// in synthesized lookAt/up or a va/vb persistence swap cannot pass.
		ICamera* referenceBase = nullptr;
		const Point3 referenceLookAt(
			onbRest.origin.x + onbRest.w.x,
			onbRest.origin.y + onbRest.w.y,
			onbRest.origin.z + onbRest.w.z );
		Check( RISE_API_CreatePinholeCamera(
				&referenceBase, onbRest.origin, referenceLookAt, onbRest.v,
				Scalar( 31.0 * DEG_TO_RAD ), 32, 24, Scalar( 1 ),
				Scalar( 0.125 ), Scalar( 0.25 ), Scalar( 0.5 ),
				Vector3( 0, 0, 0 ), Vector2( 0, 0 ) ),
			std::string( "build ordinary reference for ONB " ) + tc.name );
		Implementation::CameraCommon* reference =
			dynamic_cast<Implementation::CameraCommon*>( referenceBase );
		SceneEdit expectedEdit;
		expectedEdit.op =
			( tc.tool == SceneEditController::Tool::OrbitCamera )
			? SceneEdit::OrbitCamera
			: ( tc.tool == SceneEditController::Tool::PanCamera )
			? SceneEdit::PanCamera
			: ( tc.tool == SceneEditController::Tool::ZoomCamera )
			? SceneEdit::ZoomCamera
			: SceneEdit::RollCamera;
		expectedEdit.v3a = Vector3(
			tc.move.x - 10.0, tc.move.y - 10.0, 0 );
		expectedEdit.s = tc.move.x - 10.0;
		CameraFrameSnapshot expected;
		if( reference ) {
			SceneEditor::ApplyCameraOpToCamera(
				*reference, expectedEdit, f.ctrl->Editor().SceneScale() );
			reference->RegenerateData();
			const OrthonormalBasis3D basis = reference->GetCurrentBasis();
			expected.origin = reference->GetLocation();
			expected.u = basis.u();
			expected.v = basis.v();
			expected.w = basis.w();
			expected.explicitONB = false;
		}

		f.ctrl->SetTool( tc.tool );
		Check( f.ctrl->OnPanePointerDown( 1, Point2( 10, 10 ) )
		    && f.ctrl->OnPanePointerMove( 1, tc.move )
		    && f.ctrl->OnPanePointerUp( 1, tc.move ),
			std::string( "ONB " ) + tc.name + " gesture completes" );
		CameraFrameSnapshot edited;
		CameraSnapshot activeAfter;
		Check( CaptureNamedCameraFrame( *f.job, "onb_side", edited )
		    && edited.explicitONB
		    && reference
		    && SameCameraFrame(
				edited, expected, 1e-9, /*compareRepresentation=*/false ),
			std::string( "MONEY (T3/ONB/" ) + tc.name
			+ "): explicit persisted frame exactly matches the equivalent lookAt-camera edit" );
		Check( f.job->GetActiveCameraName() == "cam"
		    && CaptureNamedCamera( *f.job, "cam", activeAfter )
		    && SameCameraPose( activeAfter, activeRest ),
			std::string( "T3/ONB/" ) + tc.name
			+ ": inactive edit never touches the active camera" );
		f.ctrl->Undo();
		CameraFrameSnapshot restored;
		Check( CaptureNamedCameraFrame( *f.job, "onb_side", restored )
		    && SameCameraFrame( restored, onbRest ),
			std::string( "MONEY (T3/ONB/" ) + tc.name
			+ "): undo restores the exact original ONB frame" );
		if( referenceBase ) referenceBase->release();
	}

	// Sibling-behavior contract: T3's ONB adapter is named-pane-only.
	// Making the ONB camera active and navigating pane 0 retains the legacy
	// fixed-basis/no-op behavior.
	Check( f.ctrl->SetSelection(
			SceneEditController::Category::Camera, String( "onb_side" ) ),
		"make ONB camera active for the unchanged SceneCamera control" );
	CameraFrameSnapshot activeONBBefore;
	Check( CaptureNamedCameraFrame(
			*f.job, "onb_side", activeONBBefore ),
		"capture active ONB control baseline" );
	const RISE::Cst::CstHeadVersion activeONBHeadBefore =
		f.job->GetCstHeadVersion();
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( f.ctrl->OnPanePointerDown( 0, Point2( 10, 10 ) )
	    && f.ctrl->OnPanePointerMove( 0, Point2( 36, 24 ) )
	    && f.ctrl->OnPanePointerUp( 0, Point2( 36, 24 ) ),
		"legacy pane-0 ONB orbit gesture completes" );
	CameraFrameSnapshot activeONBAfter;
	Check( CaptureNamedCameraFrame(
			*f.job, "onb_side", activeONBAfter )
	    && SameCameraFrame( activeONBAfter, activeONBBefore ),
		"MONEY (T3/sibling): active SceneCamera ONB behavior remains unchanged" );
	f.ctrl->Undo();
	CameraFrameSnapshot activeONBAfterUndo;
	Check( CaptureNamedCameraFrame(
			*f.job, "onb_side", activeONBAfterUndo )
	    && SameCameraFrame( activeONBAfterUndo, activeONBBefore )
	    && f.job->GetCstHeadVersion() == activeONBHeadBefore,
		"MONEY (T3/sibling): undo preserves the active ONB frame and retained CST head" );
}

//----------------------------------------------------------------------
// (q) Named-camera binding snapshots manager-camera state.  If a preview
//     pass is in flight, the setter must cancel and park before that read.
//----------------------------------------------------------------------
static void RunNamedSceneCameraSetterParksRenderTest()
{
	std::printf( "=== scheduler (q): named-camera setter cancels and parks render ===\n" );
	Fixture f( "pane_sched_q.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"select source camera" );
	char clonedName[64] = {0};
	Check( f.ctrl->CloneActiveCamera( String( "side" ), clonedName, sizeof( clonedName ) ),
		"create inactive bind target" );
	Check( f.ctrl->SetSelection( SceneEditController::Category::Camera, String( "cam" ) ),
		"restore active cam" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	Check( f.ctrl->SetPaneRenderMode( 1, "direct" ),
		"install an explicit non-Preview mode before the contention window" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 2, kWaitMs )
	    && f.ctrl->SettlesAt( 2, kSettleMs ),
		"initial rotation settles" );

	f.ctrl->ClearSequence();
	f.ctrl->ArmReleaseInterleaving();
	const unsigned int cancelsBefore = f.ctrl->ForTest_GetCancelCount();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForMintedPass( kWaitMs ),
		"force an in-flight pass before calling the setter" );

	std::atomic<bool> setterReturned( false );
	bool setterResult = false;
	std::thread setter( [&] {
		setterResult = f.ctrl->SetPaneVantageSceneCameraNamed( 1, "side" );
		setterReturned.store( true, std::memory_order_release );
	} );
	std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
	Check( !setterReturned.load( std::memory_order_acquire ),
		"MONEY ASSERTION (q): setter remains parked while the minted pass owns camera state" );
	Check( f.ctrl->ForTest_GetCancelCount() == cancelsBefore + 1,
		"MONEY ASSERTION (q): setter requests exactly one cancellation before parking" );

	// The setter now holds mMutex while waiting for the pass.  The shell's
	// first-reveal preset guard reads the mode in precisely this window.
	// Returning the fail-closed string "preview" on contention would make it
	// overwrite this explicit Direct choice with the slot preset.
	std::mutex getterMutex;
	std::condition_variable getterCV;
	bool getterStarted = false;
	bool getterReturned = false;
	std::string observedMode;
	std::thread getter( [&] {
		{
			std::lock_guard<std::mutex> lk( getterMutex );
			getterStarted = true;
		}
		getterCV.notify_one();
		const std::string mode = f.ctrl->GetPaneRenderMode( 1 );
		{
			std::lock_guard<std::mutex> lk( getterMutex );
			observedMode = mode;
			getterReturned = true;
		}
		getterCV.notify_one();
	} );
	bool getterFinishedDuringContention = false;
	{
		std::unique_lock<std::mutex> lk( getterMutex );
		const bool started = getterCV.wait_for(
			lk, std::chrono::milliseconds( kWaitMs ), [&]{ return getterStarted; } );
		getterFinishedDuringContention = started && getterCV.wait_for(
			lk, std::chrono::milliseconds( 250 ), [&]{ return getterReturned; } );
	}
	Check( getterFinishedDuringContention && observedMode == "direct",
		"MONEY ASSERTION (q): contended mode read is nonblocking and returns the real Direct mode" );
	f.ctrl->ReleaseMintedPass();
	setter.join();
	getter.join();
	Check( setterResult && setterReturned.load( std::memory_order_acquire ),
		"setter completes after the render releases camera state" );
	Check( getterReturned && observedMode == "direct",
		"MONEY ASSERTION (q): preset guard observes the preserved explicit Direct mode" );
}

//----------------------------------------------------------------------
// (r) Pane* C-ABI nav wrappers forward `pane` RAW.  The nav funnels must
//     reject both an out-of-range index and an in-range HIDDEN pane: neither
//     may park/cancel a pass, switch the scheduler, or mutate pane state.
//----------------------------------------------------------------------
static void RunOutOfRangePaneNavRejectedTest()
{
	std::printf( "=== scheduler (p): out-of-range pane nav fails closed, no OOB (user-review round 2) ===\n" );
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

//----------------------------------------------------------------------
// (s/T4) Last Render is a pane CONTENT SOURCE, not a registry mode.  It
// is inert under interactive invalidation and receives exactly one push
// per completed full render.  The retained image must outlive its source.
//----------------------------------------------------------------------
static void RunLastRenderPaneSourceTest()
{
	std::printf( "=== scheduler (s/T4): Last Render panes are persistent completion-fed sources ===\n" );
	Fixture f( "pane_sched_s_last_render.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	RecordingPaneSink* sink1 = new RecordingPaneSink();
	RecordingPaneSink* sink2 = new RecordingPaneSink();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	Check( f.ctrl->SetPaneSink( 1, sink1 ) && f.ctrl->SetPaneSink( 2, sink2 ),
		"pane sinks installed" );
	Check( Implementation::FindViewportRenderModeByName( "last_render" ) == nullptr,
		"MONEY (T4): Last Render is NOT a fake agent-visible render-mode registry row" );

	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"Last Render setter succeeds before any full render exists" );
	int wireSource = -1;
	Check( RISE_API_SceneEditController_GetPaneContentSource(
			f.ctrl, 1, &wireSource ) && wireSource == 1,
		"C ABI reports the stable LastRender wire value 1" );
	Check( !RISE_API_SceneEditController_SetPaneContentSource( f.ctrl, 1, 2 ),
		"C ABI refuses an unknown content-source wire value" );
	Check( sink1->Count() == 1 && sink1->Width() == 1 && sink1->Height() == 1
	    && sink1->FirstPixel().a < 0.0
	    && ViewportShellDisplayAlpha8( sink1->FirstPixel() ) == 0
	    && ViewportShellDisplayAlpha8(
			RISEColor( RISEPel( 0.25, 0.5, 0.75 ), 0.0 ) ) == 255,
		"MONEY (T4): pre-first-render state is an explicit transparent placeholder" );

	f.ctrl->SetTool( SceneEditController::Tool::Select );
	Check( f.ctrl->OnPanePointerDown( 2, Point2( 4, 4 ) ),
		"control: an Interactive pane accepts pointer Down" );
	Check( !f.ctrl->SetPaneContentSource(
			2, SceneEditController::PaneContentSource::LastRender )
	    && f.ctrl->GetPaneContentSource( 2 )
			== SceneEditController::PaneContentSource::Interactive,
		"MONEY (T4/review): source switch on the actively gestured pane is "
		"refused, so later Move/Up cannot commit behind frozen pixels" );
	Check( f.ctrl->OnPanePointerUp( 2, Point2( 4, 4 ) ),
		"the unchanged Interactive gesture completes normally" );

	f.ctrl->Start( true );
	Check( f.ctrl->WaitForPassCount( 3, kWaitMs ),
		"layout-grow rotation renders the three Interactive panes" );
	Check( f.ctrl->SettlesAt( 3, kSettleMs ),
		"MONEY (T4): Last Render pane is excluded from dirty rotation" );
	f.ctrl->ClearSequence();
	const unsigned int pushesBeforeEdit = sink1->Count();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 3, kWaitMs ),
		"scene edit renders the three Interactive panes" );
	Check( f.ctrl->SettlesAt( 3, kSettleMs )
	    && sink1->Count() == pushesBeforeEdit,
		"MONEY (T4): scene edits neither schedule nor refresh Last Render" );
	Check( !f.ctrl->OnPanePointerDown( 1, Point2( 4, 4 ) ),
		"MONEY (T4): picking/navigation/gizmos are refused on frozen content" );
	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPaneSurfaceDims( 1, 640, 360 ),
		"Last Render pane accepts surface-size bookkeeping" );
	Check( f.ctrl->SettlesAt( 0, kSettleMs ),
		"MONEY (T4): resizing Last Render never schedules an interactive pass" );

	IRasterImage* first = nullptr;
	Check( RISE_API_CreateRISEColorRasterImage(
			&first, 3, 2, RISEColor( RISEPel( 0.2, 0.3, 0.4 ), 1.0 ) )
	    && first,
		"constructed first full-render image" );
	bool firstPublished = false;
	SceneEditController::RenderJobId jobId = 0;
	Check( first && f.ctrl->SubmitAgentRenderSync(
			[&] {
				firstPublished = f.ctrl->PublishAgentRenderImageParked( *first );
			},
			String( "t4_first_completion" ), &jobId, kWaitMs ),
		"simulated coordinated full-render completion accepted" );
	Check( firstPublished && sink1->Count() == pushesBeforeEdit + 1
	    && sink1->Width() == 3 && sink1->Height() == 2,
		"MONEY (T4): one completion pushes exactly once at full resolution" );
	const RISEColor retainedPixel = sink1->FirstPixel();
	if( first )
	{
		first->SetPEL( 0, 0, RISEColor( RISEPel( 0.9, 0.1, 0.1 ), 1.0 ) );
		first->release();
		first = nullptr;
	}

	Check( f.ctrl->SetPaneContentSource(
			2, SceneEditController::PaneContentSource::LastRender ),
		"a second pane selects Last Render after a completion" );
	Check( sink2->Count() == 1 && sink2->Width() == 3 && sink2->Height() == 2
	    && sink2->FirstPixel().base.r == retainedPixel.base.r
	    && sink2->FirstPixel().base.g == retainedPixel.base.g
	    && sink2->FirstPixel().base.b == retainedPixel.base.b,
		"MONEY (T4): a later subscriber receives the retained deep copy, not mutated source memory" );

	IRasterImage* second = nullptr;
	Check( RISE_API_CreateRISEColorRasterImage(
			&second, 5, 4, RISEColor( RISEPel( 0.6, 0.5, 0.4 ), 1.0 ) )
	    && second,
		"constructed replacement full-render image" );
	const unsigned int sink1BeforeSecond = sink1->Count();
	const unsigned int sink2BeforeSecond = sink2->Count();
	bool secondPublished = false;
	Check( second && f.ctrl->SubmitAgentRenderSync(
			[&] {
				secondPublished = f.ctrl->PublishAgentRenderImageParked( *second );
			},
			String( "t4_second_completion" ), &jobId, kWaitMs ),
		"second coordinated full-render completion accepted" );
	Check( secondPublished
	    && sink1->Count() == sink1BeforeSecond + 1
	    && sink2->Count() == sink2BeforeSecond + 1
	    && sink1->Width() == 5 && sink2->Height() == 4,
		"MONEY (T4): replacement pushes exactly once to every Last Render pane" );
	if( second ) second->release();

	f.ctrl->ClearSequence();
	Check( f.ctrl->SetPaneRenderMode( 1, "normals" ),
		"choosing an interactive mode exits Last Render" );
	Check( f.ctrl->GetPaneContentSource( 1 )
	    == SceneEditController::PaneContentSource::Interactive,
		"render-mode selection restores the Interactive source" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs )
	    && f.ctrl->Sequence()[0] == 1
	    && f.ctrl->SettlesAt( 1, kSettleMs ),
		"MONEY (T4): returning to Interactive schedules one fresh pane-local pass" );

	// All-visible-frozen is a scheduler boundary, not a request for a
	// fallback interactive pane.  A later scene edit must stay quiescent.
	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender )
	    && f.ctrl->SetPaneContentSource(
			3, SceneEditController::PaneContentSource::LastRender ),
		"all nonzero panes are frozen before the pane-0 boundary check" );
	f.ctrl->ClearSequence();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs )
	    && f.ctrl->Sequence()[0] == 0
	    && f.ctrl->SettlesAt( 1, kSettleMs ),
		"T4 Single-transition setup leaves pane 0 as the current scheduler pane" );
	Check( f.ctrl->SetPaneContentSource(
			0, SceneEditController::PaneContentSource::LastRender ),
		"all four Quad panes can display Last Render" );
	f.ctrl->ClearSequence();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->SettlesAt( 0, kSettleMs ),
		"MONEY (T4): an all-LastRender layout remains quiescent after scene edits" );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Single ),
		"return to legacy Single layout" );
	Check( f.ctrl->GetPaneContentSource( 0 )
	    == SceneEditController::PaneContentSource::Interactive
	    && !f.ctrl->SetPaneContentSource(
			0, SceneEditController::PaneContentSource::LastRender ),
		"MONEY (T4): Single restores/refuses pane-0 Last Render for legacy compatibility" );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs )
	    && f.ctrl->Sequence()[0] == 0
	    && f.ctrl->SettlesAt( 1, kSettleMs ),
		"MONEY (T4): Single immediately repaints restored pane 0 instead of "
		"leaving the retained full-render pixels stranded on screen" );

	Check( f.ctrl->SetPaneSink( 1, nullptr ) && f.ctrl->SetPaneSink( 2, nullptr ),
		"pane sinks detached" );
	sink1->release();
	sink2->release();
}

//----------------------------------------------------------------------
// Round-8 review P2 #1: a pointer gesture whose Up is never delivered must
// not freeze the sibling panes forever.
//
// T0 stopped a completed quantum from self-arming the rotation while a
// gesture pins the scheduler (that was the spin fix).  The cost: a gesture
// flag that never clears freezes every other pane permanently.  That IS
// reachable live -- both shells guard their pointer handlers on an
// `interactionEnabled` flag that a chat/agent render request flips, while
// the core-side render is separately REFUSED because a gesture is open, so
// no teardown path runs either.  The watchdog is the recovery.
//
// Pinned here: (1) siblings repaint during the stationary hold; (2)
// mPointerDown stays set; (3) renewed Move reclaims the gesture pane and
// resumes instead of going inert; (4) Up still finalizes normally.
//----------------------------------------------------------------------
static void RunLostPointerUpWatchdogTest()
{
	std::printf( "=== scheduler (w1/round-8): a lost pointer-up does not freeze sibling panes ===\n" );
	Fixture f( "pane_sched_w1_lost_pointer_up.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	f.ctrl->ForTest_SetPointerWatchdogMs( 150 );   // keep the test fast
	f.ctrl->Start( /*suppressInitialRender*/ true );

	// Open an orbit gesture on pane 0 and let it pin the scheduler.
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( f.ctrl->OnPanePointerDown( 0, Point2( 8, 8 ) ), "pointer down on pane 0" );
	f.ctrl->OnPointerMove( Point2( 12, 9 ) );
	Check( f.ctrl->WaitForPassCount( 1, kWaitMs ), "the gestured pane renders" );

	// Dirty every pane (the in-gesture edit already did) and then simply never
	// deliver the Up -- exactly what the shell does when it drops the event.
	f.ctrl->ClearSequence();
	f.ctrl->ForTest_KickRender();

	// The watchdog must declare the gesture stale and resume the rotation.
	bool sawSibling = false;
	for( unsigned int i = 0; i < 40 && !sawSibling; ++i )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		for( std::size_t k = 0; k < seq.size(); ++k )
			if( seq[k] != 0 ) { sawSibling = true; break; }
	}
	Check( sawSibling,
		"MONEY (w1): sibling panes repaint again after the watchdog fires (they froze forever pre-fix)" );
	Check( f.ctrl->ForTest_PointerGestureStale(),
		"w1: the gesture is marked stale" );
	Check( f.ctrl->ForTest_PointerDown(),
		"MONEY (w1): mPointerDown is deliberately STILL SET -- clearing it would make the late Up early-return and strand its composite" );

	// A legitimate long hold is not an implicit cancel.  Continuing the
	// physical drag must reclaim pane 0 and clear stale before applying the
	// resumed delta; the round-8 behavior discarded this move and forced a
	// release/re-click.
	f.ctrl->OnPointerMove( Point2( 16, 11 ) );
	Check( !f.ctrl->ForTest_PointerGestureStale()
	    && f.ctrl->ForTest_CurrentPane() == 0,
		"MONEY (w1): motion after a >watchdog hold resumes the same gesture on its original pane" );

	// The late Up still finalizes normally, and clears the stale marker.
	f.ctrl->OnPointerUp( Point2( 16, 11 ) );
	Check( !f.ctrl->ForTest_PointerDown() && !f.ctrl->ForTest_PointerGestureStale(),
		"MONEY (w1): the late pointer-up still runs its normal finalization" );
	Check( !f.ctrl->Editor().IsCompositeOpen(),
		"MONEY (w1): the gesture's composite is closed, not stranded (a stranded one blocks agent D2 renders)" );

	// A fresh gesture is never born stale.
	Check( f.ctrl->OnPanePointerDown( 0, Point2( 8, 8 ) ), "a new gesture starts" );
	Check( !f.ctrl->ForTest_PointerGestureStale(),
		"w1: a fresh gesture is not stale" );
	f.ctrl->OnPointerUp( Point2( 8, 8 ) );
	f.ctrl->Stop();
}

//----------------------------------------------------------------------
// The lost-Up watchdog must remain clocked after the idle-refinement ladder
// reaches divisor 1.  The original timed wait was gated only by scale>1, so
// it became an indefinite wait before the production 10-second timeout and
// could never run without an unrelated wake.
//----------------------------------------------------------------------
static void RunPointerWatchdogAfterRefinementSettlesTest()
{
	std::printf( "=== scheduler (w1b): pointer watchdog survives scale-1 refinement idle ===\n" );
	Fixture f( "pane_sched_w1b_watchdog_after_refine.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	f.ctrl->ForTest_SetPointerWatchdogMs( 1200 );
	f.ctrl->Start( true );
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( f.ctrl->OnPanePointerDown( 0, Point2( 8, 8 ) ),
		"pointer down on pane 0" );
	f.ctrl->OnPointerMove( Point2( 12, 9 ) );

	bool reachedScaleOne = false;
	for( unsigned int i = 0; i < 20 && !reachedScaleOne; ++i )
	{
		int phase = -1;
		unsigned int divisor = 0;
		reachedScaleOne = f.ctrl->GetPaneRefinementStatus( 0, phase, divisor )
			&& divisor == 1;
		if( !reachedScaleOne )
			std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
	}
	Check( reachedScaleOne && !f.ctrl->ForTest_PointerGestureStale(),
		"precondition: the gesture reaches scale 1 before its watchdog deadline" );

	f.ctrl->ClearSequence();
	f.ctrl->ForTest_KickRender();
	bool sawSibling = false;
	for( unsigned int i = 0; i < 45 && !sawSibling; ++i )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		for( std::size_t k = 0; k < seq.size(); ++k )
			if( seq[k] != 0 ) { sawSibling = true; break; }
	}
	Check( f.ctrl->ForTest_PointerGestureStale() && sawSibling,
		"MONEY (w1b): watchdog still fires and releases siblings after refinement already reached scale 1" );
	f.ctrl->OnPointerUp( Point2( 12, 9 ) );
	f.ctrl->Stop();
}

//----------------------------------------------------------------------
// Round-8 review P1: the watchdog must NEVER fire during a genuinely active
// drag, even when a single render pass outlasts the whole watchdog window.
//
// The first cut gated on mLastEditTimeMs, which OnPointerMove only stamps
// AFTER its render-completion wait and a successful Apply.  On exactly the
// panes this feature exists for -- BeautyVariant, whose pinned divisor makes
// every in-drag quantum a full fixed-spp + OIDN pass, and whose denoise is
// not cancel-interruptible -- one pass can outlast the window, so a user
// still actively dragging got marked stale and the drag silently froze.
// The fix separates POINTER ACTIVITY from EDIT COMPLETION and additionally
// stands the watchdog down while a move is parked in that wait.
//
// Here: a 400 ms pass against a 120 ms watchdog, with moves driven
// continuously.  The gesture must stay live throughout.
//----------------------------------------------------------------------
static void RunActiveDragSurvivesSlowPassesTest()
{
	std::printf( "=== scheduler (w4/round-8): an active drag is never declared stale, even across slow passes ===\n" );
	Fixture f( "pane_sched_w4_slow_pass_drag.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	f.ctrl->SetSlowPassMs( 400 );                     // a BeautyVariant-class quantum
	f.ctrl->ForTest_SetPointerWatchdogMs( 120 );      // far SHORTER than one pass
	f.ctrl->Start( /*suppressInitialRender*/ true );

	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( f.ctrl->OnPanePointerDown( 0, Point2( 8, 8 ) ), "pointer down on pane 0" );

	// Drive a continuous drag for well over the watchdog window.  Each move
	// blocks for the in-flight pass, which is the whole point: the pointer is
	// active the entire time even though few edits complete.
	bool wentStale = false;
	for( unsigned int i = 0; i < 12 && !wentStale; ++i )
	{
		f.ctrl->OnPointerMove( Point2( 9.0 + i, 8.0 + i ) );
		if( f.ctrl->ForTest_PointerGestureStale() ) wentStale = true;
	}
	Check( !wentStale,
		"MONEY (w4): a continuously-dragging pointer is NEVER marked stale, even though each pass outlasts the watchdog window" );
	Check( f.ctrl->ForTest_PointerDown(), "w4: the gesture is still live" );

	f.ctrl->OnPointerUp( Point2( 24, 20 ) );
	f.ctrl->SetSlowPassMs( 0 );
	f.ctrl->Stop();
}

//----------------------------------------------------------------------
// Review-validation P1: move admission and the watchdog stale handoff must
// be one mMutex transition.  The hook holds the exact point after the move
// is accepted/counted.  Even after several watchdog windows, the render
// thread cannot mark it stale until the move leaves that protected handoff.
//----------------------------------------------------------------------
static void RunPointerMoveWatchdogHandoffTest()
{
	std::printf( "=== scheduler (w5): admitted pointer move cannot race watchdog stale handoff ===\n" );
	Fixture f( "pane_sched_w5_move_watchdog_handoff.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	f.ctrl->ForTest_SetPointerWatchdogMs( 80 );
	f.ctrl->Start( true );
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	Check( f.ctrl->OnPanePointerDown( 0, Point2( 8, 8 ) ),
		"pointer down on pane 0" );
	f.ctrl->ArmPointerMoveAdmissionInterleaving();
	std::thread moveThread( [&]{
		f.ctrl->OnPointerMove( Point2( 14, 10 ) );
	} );
	Check( f.ctrl->WaitForPointerMoveAdmission( kWaitMs ),
		"move reached the definitive admitted/in-flight handoff" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 240 ) );
	Check( !f.ctrl->ForTest_PointerGestureStale(),
		"MONEY (w5): watchdog cannot stale a move after it was admitted but before its in-flight publication" );
	f.ctrl->ForTest_SetPointerWatchdogMs( 0 );
	f.ctrl->ReleasePointerMoveAdmission();
	moveThread.join();
	Check( !f.ctrl->ForTest_PointerGestureStale(),
		"the admitted move completes as a live gesture" );
	f.ctrl->OnPointerUp( Point2( 14, 10 ) );
	f.ctrl->Stop();
}

//----------------------------------------------------------------------
// PointerDown itself is a multi-step admission transition: it publishes the
// live pointer flag, then opens the tool's composite.  A render must not
// finalize in between those steps and leave the tail of Down to open an
// orphaned composite after the render has already been admitted.
//----------------------------------------------------------------------
static void RunPointerDownRenderAdmissionHandoffTest()
{
	std::printf( "=== scheduler (w5b): render cannot finalize a half-constructed pointer down ===\n" );
	Fixture f( "pane_sched_w5b_down_render_handoff.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	f.ctrl->SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	f.ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	f.ctrl->ArmPointerDownAdmissionInterleaving();
	std::thread downThread( [&]{
		f.ctrl->OnPointerDown( Point2( 8, 8 ) );
	} );
	Check( f.ctrl->WaitForPointerDownAdmission( kWaitMs ),
		"pointer down reached the published-before-composite seam" );

	std::atomic<bool> renderRan( false );
	std::atomic<bool> renderAccepted( false );
	f.ctrl->ArmRenderAdmissionAttempt();
	std::thread renderThread( [&]{
		SceneEditController::RenderJobId jobId =
			SceneEditController::kInvalidRenderJobId;
		renderAccepted.store(
			f.ctrl->SubmitProductionRenderSync(
				[&]{ renderRan.store( true, std::memory_order_release ); },
				String( "w5b-down-before-render" ), &jobId, kWaitMs ),
			std::memory_order_release );
	} );
	Check( f.ctrl->WaitForRenderAdmissionAttempt( kWaitMs ),
		"render worker reached the admission attempt" );
	Check( !renderRan.load( std::memory_order_acquire ),
		"MONEY (w5b): render admission waits for PointerDown to finish constructing its composite" );

	f.ctrl->ReleasePointerDownAdmission();
	downThread.join();
	renderThread.join();
	Check( renderAccepted.load( std::memory_order_acquire )
	    && renderRan.load( std::memory_order_acquire ),
		"render runs after the completed Down transition is finalized" );
	Check( !f.ctrl->ForTest_PointerDown()
	    && !f.ctrl->Editor().IsCompositeOpen(),
		"MONEY (w5b): no live pointer or orphaned composite remains after the render" );
	f.ctrl->Stop();
}

//----------------------------------------------------------------------
// TimeScrubBegin has the same begin/finalize handoff shape as PointerDown:
// opening the composite and installing motion quality must be one admission
// turn, or a render can close the just-opened composite before Begin returns.
//----------------------------------------------------------------------
static void RunTimeScrubBeginRenderAdmissionHandoffTest()
{
	std::printf( "=== scheduler (w5c): render cannot finalize a half-constructed time scrub ===\n" );
	Fixture f( "pane_sched_w5c_scrub_render_handoff.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	f.ctrl->ArmPointerDownAdmissionInterleaving();
	std::atomic<bool> scrubBegan( false );
	std::thread scrubThread( [&]{
		scrubBegan.store( f.ctrl->OnTimeScrubBegin(), std::memory_order_release );
	} );
	Check( f.ctrl->WaitForPointerDownAdmission( kWaitMs ),
		"time scrub reached the opened-composite-before-return seam" );

	std::atomic<bool> renderRan( false );
	std::atomic<bool> renderAccepted( false );
	f.ctrl->ArmRenderAdmissionAttempt();
	std::thread renderThread( [&]{
		SceneEditController::RenderJobId jobId =
			SceneEditController::kInvalidRenderJobId;
		renderAccepted.store(
			f.ctrl->SubmitProductionRenderSync(
				[&]{ renderRan.store( true, std::memory_order_release ); },
				String( "w5c-scrub-before-render" ), &jobId, kWaitMs ),
			std::memory_order_release );
	} );
	Check( f.ctrl->WaitForRenderAdmissionAttempt( kWaitMs ),
		"render worker reached the time-scrub admission attempt" );
	Check( !renderRan.load( std::memory_order_acquire ),
		"MONEY (w5c): render admission waits for TimeScrubBegin to finish" );

	f.ctrl->ReleasePointerDownAdmission();
	scrubThread.join();
	renderThread.join();
	Check( scrubBegan.load( std::memory_order_acquire )
	    && renderAccepted.load( std::memory_order_acquire )
	    && renderRan.load( std::memory_order_acquire ),
		"time scrub begins, then the render finalizes and runs" );
	Check( !f.ctrl->Editor().IsCompositeOpen(),
		"MONEY (w5c): the render leaves no time-scrub composite stranded" );
	f.ctrl->Stop();
}

//----------------------------------------------------------------------
// Lost-Up render admission: render requests are a controller-owned gesture
// finalization boundary.  A pending live transform must reach the Document,
// close its composite, and then allow the render closure to run.
//----------------------------------------------------------------------
static void RunRenderFinalizesLostPointerUpTest()
{
	std::printf( "=== scheduler (w6): render admission finalizes a lost pointer-up ===\n" );
	Fixture f( "pane_sched_w6_render_finalizes_gesture.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	f.ctrl->SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	f.ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	f.ctrl->OnPointerDown( Point2( 8, 8 ) );
	f.ctrl->OnPointerMove( Point2( 24, 20 ) );
	Check( f.ctrl->Editor().HasPendingCstObjectTransforms()
	    && f.ctrl->Editor().IsCompositeOpen(),
		"lost-Up setup has a pending transform and open composite" );

	bool renderRan = false;
	SceneEditController::RenderJobId jobId =
		SceneEditController::kInvalidRenderJobId;
	const bool accepted = f.ctrl->SubmitProductionRenderSync(
		[&]{ renderRan = true; }, String( "w6-finalize-before-render" ),
		&jobId, kWaitMs );
	Check( accepted && renderRan,
		"MONEY (w6): the render is admitted instead of permanently refused" );
	Check( !f.ctrl->ForTest_PointerDown()
	    && !f.ctrl->Editor().IsCompositeOpen()
	    && !f.ctrl->Editor().HasPendingCstObjectTransforms(),
		"MONEY (w6): render admission persisted and closed the dropped-Up gesture" );
	f.ctrl->Stop();
}

//----------------------------------------------------------------------
// The legacy synchronous preview-render entry point owns an independent
// admission path from SubmitProductionRenderSync.  It must perform the same
// controller-owned interaction finalization, including the property-scrub
// branch that has no editor composite to expose a stranded-state failure.
//----------------------------------------------------------------------
static void RunDirectRenderFinalizesOpenInteractionsTest()
{
	std::printf( "=== scheduler (w6b): direct preview render finalizes open interactions ===\n" );
	Fixture f( "pane_sched_w6b_direct_render_finalizes.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	f.ctrl->SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	f.ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	f.ctrl->OnPointerDown( Point2( 8, 8 ) );
	f.ctrl->OnPointerMove( Point2( 24, 20 ) );
	Check( f.ctrl->Editor().HasPendingCstObjectTransforms()
	    && f.ctrl->Editor().IsCompositeOpen(),
		"direct-render setup has a pending transform and open pointer composite" );

	bool pointerRenderRan = false;
	Check( f.ctrl->RunPreviewRenderParked( [&]{ pointerRenderRan = true; } )
	    && pointerRenderRan,
		"direct preview render is admitted after finalizing the lost pointer-up" );
	Check( !f.ctrl->ForTest_PointerDown()
	    && !f.ctrl->Editor().IsCompositeOpen()
	    && !f.ctrl->Editor().HasPendingCstObjectTransforms(),
		"MONEY (w6b): direct preview admission persists and closes the dropped-Up gesture" );

	f.ctrl->BeginPropertyScrub();
	int phase = -1;
	unsigned int divisor = 0;
	Check( f.ctrl->GetPaneRefinementStatus( 0, phase, divisor ) && divisor > 1,
		"property scrub installs motion-quality state before direct admission" );
	bool scrubRenderRan = false;
	Check( f.ctrl->RunPreviewRenderParked( [&]{ scrubRenderRan = true; } )
	    && scrubRenderRan,
		"direct preview render is admitted after finalizing the property scrub" );
	Check( f.ctrl->GetPaneRefinementStatus( 0, phase, divisor ) && divisor == 1,
		"MONEY (w6b): direct preview admission clears property-scrub motion quality" );
	f.ctrl->Stop();
}

//----------------------------------------------------------------------
// Round-8 review P2 #2: ~SceneEditController must not touch mJob.
//
// T0 gave StopInteractive an orphaned-gesture cleanup that commits pending
// CST edits, and the destructor reaches it via Stop().  Every leg of that
// cleanup routes to mJob -- CommitPendingCstObjectTransforms directly, and
// even EndComposite through SceneEditor::Apply's HasRetainedCstDocument
// probe.  The dtor's own NOTE says it CANNOT touch mJob: owners legitimately
// release the Job first, so a stale-but-non-null mJob is a use-after-free.
//
// Rather than rely on a sanitizer to catch the UAF, this observes the
// invariant directly: a Job subclass records any late access, and the
// destructor must produce none.
//----------------------------------------------------------------------
namespace {
class JobAccessRecordingJob : public Job
{
public:
	void ArmAccessRecording() { mArmed = true; mTouched = false; }
	bool Touched() const { return mTouched; }

	int CstObjectTransformKind( const char* name ) const override
	{
		if( mArmed ) mTouched = true;
		return Job::CstObjectTransformKind( name );
	}
	bool HasRetainedCstDocument() const override
	{
		if( mArmed ) mTouched = true;
		return Job::HasRetainedCstDocument();
	}

private:
	bool         mArmed = false;
	mutable bool mTouched = false;
};

class FailingGestureCommitJob : public Job
{
public:
	void SetFailCommits( bool fail ) { mFailCommits = fail; }
	int ApplyCstObjectMatrixEdit( const char* objectName, const char* matrix ) override
	{
		return mFailCommits ? 0
			: Job::ApplyCstObjectMatrixEdit( objectName, matrix );
	}
private:
	bool mFailCommits = true;
};
}  // namespace

static void RunDestructorDoesNotTouchJobTest()
{
	std::printf( "=== scheduler (w2/round-8): the destructor never reaches mJob ===\n" );
	const std::string path = WriteTemp( "pane_sched_w2_dtor_job.RISEscene", kScene );
	JobAccessRecordingJob* job = new JobAccessRecordingJob();
	Check( job->LoadAsciiSceneViaCst( path.c_str() ), "fixture scene loads" );

	SceneEditController* ctrl = new SceneEditController( *job, nullptr );
	// Open an object-gizmo gesture so the cleanup has pending CST work to do
	// -- without pending state the cleanup is a no-op and proves nothing.
	ctrl->SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	ctrl->OnPointerDown( Point2( 8, 8 ) );
	ctrl->OnPointerMove( Point2( 24, 20 ) );
	const bool hasPending = ctrl->Editor().HasPendingCstObjectTransforms();
	Check( hasPending,
		"w2: the gizmo drag left a pending CST transform (the cleanup's trigger)" );

	// From here on, ANY mJob access is a contract violation.
	job->ArmAccessRecording();
	delete ctrl;
	Check( !job->Touched(),
		"MONEY (w2): destroying the controller mid-gesture makes NO mJob access (pre-fix it committed pending CST through a possibly-released Job)" );

	job->release();
	std::remove( path.c_str() );
}

static void RunCAbiDestructionPersistsGestureTest()
{
	std::printf( "=== scheduler (w7): C-ABI destruction persists an open gesture while Job is alive ===\n" );
	const std::string path = WriteTemp( "pane_sched_w7_cabi_prepare.RISEscene", kScene );
	Job* job = new Job();
	Check( job->LoadAsciiSceneViaCst( path.c_str() ), "fixture scene loads" );
	if( !job->HasRetainedCstDocument() )
	{
		Check( false, "fixture retains a CST Document" );
		job->release();
		std::remove( path.c_str() );
		return;
	}
	const std::string before = Cst::SerializeCst( *job->GetCstDocument() );
	SceneEditController* ctrl = nullptr;
	Check( RISE_API_CreateSceneEditController( job, nullptr, &ctrl ) && ctrl,
		"C-ABI controller constructs" );
	if( ctrl )
	{
		ctrl->SetSelection( SceneEditController::Category::Object, String( "obj" ) );
		ctrl->SetTool( SceneEditController::Tool::TranslateObject );
		ctrl->OnPointerDown( Point2( 8, 8 ) );
		ctrl->OnPointerMove( Point2( 24, 20 ) );
		Check( ctrl->Editor().HasPendingCstObjectTransforms(),
			"open drag has a pending live-only transform" );
		RISE_API_DestroySceneEditController( ctrl );
	}
	const std::string after = Cst::SerializeCst( *job->GetCstDocument() );
	Check( after != before && after.find( "\nmatrix " ) != std::string::npos,
		"MONEY (w7): destroy shim commits the final transform into the surviving Document" );
	job->release();
	std::remove( path.c_str() );
}

static void RunGestureCommitFailureIsReportedTest()
{
	std::printf( "=== scheduler (w7b): interaction persistence failures are reported truthfully ===\n" );
	const std::string path = WriteTemp( "pane_sched_w7b_commit_failure.RISEscene", kScene );
	FailingGestureCommitJob* job = new FailingGestureCommitJob();
	Check( job->LoadAsciiSceneViaCst( path.c_str() ), "fixture scene loads" );
	SceneEditController* ctrl = new SceneEditController( *job, nullptr );
	ctrl->SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	ctrl->OnPointerDown( Point2( 8, 8 ) );
	ctrl->OnPointerMove( Point2( 24, 20 ) );
	Check( ctrl->Editor().HasPendingCstObjectTransforms(),
		"failing-route setup has a pending transform" );
	Check( !RISE_API_SceneEditController_FinalizeOpenInteractions( ctrl ),
		"MONEY (w7b): FinalizeOpenInteractions reports the failed CST persistence route" );
	bool renderRan = false;
	Check( !ctrl->RunPreviewRenderParked( [&]{ renderRan = true; } ) && !renderRan,
		"MONEY (w7b): direct render admission remains closed after persistence failure" );
	Check( !RISE_API_SceneEditController_PrepareForDestruction( ctrl ),
		"MONEY (w7b): Job-live Prepare does not claim a failed interaction was persisted" );
	RISE_API_DestroySceneEditController( ctrl );
	job->release();
	std::remove( path.c_str() );
}

static void RunUndoCommitFailureClosesRenderAdmissionTest()
{
	std::printf( "=== scheduler (w7c): Undo persistence failure closes render admission ===\n" );
	const std::string path = WriteTemp( "pane_sched_w7c_undo_commit_failure.RISEscene", kScene );
	FailingGestureCommitJob* job = new FailingGestureCommitJob();
	job->SetFailCommits( false );
	Check( job->LoadAsciiSceneViaCst( path.c_str() ), "fixture scene loads" );
	SceneEditController* ctrl = new SceneEditController( *job, nullptr );
	ctrl->SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	ctrl->SetTool( SceneEditController::Tool::TranslateObject );
	ctrl->OnPointerDown( Point2( 8, 8 ) );
	ctrl->OnPointerMove( Point2( 24, 20 ) );
	ctrl->OnPointerUp( Point2( 24, 20 ) );
	Check( ctrl->Editor().History().UndoDepth() > 0,
		"successful gesture commit produced undo history" );
	job->SetFailCommits( true );
	ctrl->Undo();
	Check( !ctrl->FinalizeOpenInteractions(),
		"MONEY (w7c): failed Undo CST route trips the sticky persistence result" );
	bool renderRan = false;
	Check( !ctrl->RunPreviewRenderParked( [&]{ renderRan = true; } ) && !renderRan,
		"MONEY (w7c): render admission refuses the unrecorded live Undo" );
	RISE_API_DestroySceneEditController( ctrl );
	job->release();
	std::remove( path.c_str() );
}

//----------------------------------------------------------------------
// Round-8 review P2 #3: the last-render sink push must not run under mMutex.
//
// ADR rule 5 ("notifications fire outside locks").  Beyond the latent
// deadlock for any sink that re-enters the controller, the pre-fix path ran
// a full per-pixel conversion on the UI thread inside the lock.
//
// The oracle: from inside OutputImage, a HELPER THREAD calls a controller
// method that takes mMutex with a blocking lock.  If the publisher still
// held mMutex, that helper cannot complete.  Bounded by a timeout so a
// regression fails rather than hangs.
//----------------------------------------------------------------------
namespace {
class LockProbingPaneSink
	: public virtual IRasterizerOutput
	, public virtual Implementation::Reference
{
public:
	void Bind( SceneEditController* c ) { mCtrl = c; }
	bool Probed() const   { return mProbed; }
	bool LockWasFree() const { return mLockWasFree; }
	void JoinProbe() { if( mProbe.joinable() ) mProbe.join(); }

	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override
	{
		if( !mCtrl || mProbed ) return;
		mProbed = true;
		// Keep the probe JOINABLE on the sink.  If the publisher regresses to
		// holding mMutex, the bounded poll below fails and returns from this
		// callback; only then can the setter release mMutex and the test's
		// post-setter JoinProbe complete.  This produces a clean failure without
		// a detached raw-controller thread outliving the fixture.
		std::shared_ptr< std::atomic<bool> > done =
			std::make_shared< std::atomic<bool> >( false );
		SceneEditController* ctrl = mCtrl;
		mProbe = std::thread( [ctrl, done]{
			(void)ctrl->ForTest_CurrentPane();   // blocking mMutex acquisition
			done->store( true, std::memory_order_release );
		} );
		for( unsigned int i = 0; i < 100 && !done->load( std::memory_order_acquire ); ++i )
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		mLockWasFree = done->load( std::memory_order_acquire );
	}

protected:
	~LockProbingPaneSink() override { JoinProbe(); }

private:
	SceneEditController* mCtrl = nullptr;
	bool                 mProbed = false;
	bool                 mLockWasFree = false;
	std::thread          mProbe;
};
}  // namespace

static void RunLastRenderPublishOutsideLockTest()
{
	std::printf( "=== scheduler (w3/round-8): last-render delivery happens outside mMutex ===\n" );
	Fixture f( "pane_sched_w3_publish_lock.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	LockProbingPaneSink* sink = new LockProbingPaneSink();
	sink->Bind( f.ctrl );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	Check( f.ctrl->SetPaneSink( 1, sink ), "probing sink installed" );

	// Selecting Last Render publishes the placeholder through the sink.
	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"pane 1 becomes a Last Render pane" );
	sink->JoinProbe();
	Check( sink->Probed(),
		"w3: the sink was actually invoked (otherwise the oracle proves nothing)" );
	Check( sink->LockWasFree(),
		"MONEY (w3): mMutex was FREE during the sink callback (pre-fix the publisher held it across OutputImage)" );

	sink->release();
}

static void RunLastRenderReentrantCallbacksAreBoundedTest()
{
	std::printf( "=== scheduler (w8): same/cross-pane sink re-entry is bounded and deadlock-free ===\n" );
	Fixture f( "pane_sched_w8_last_render_reentry.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	ReentrantPaneSink* self = new ReentrantPaneSink(
		ReentrantPaneSink::Action::RegisterSelf, 1, 1 );
	ReentrantPaneSink* first = new ReentrantPaneSink(
		ReentrantPaneSink::Action::FreezeOther, 1, 2 );
	ReentrantPaneSink* second = new ReentrantPaneSink(
		ReentrantPaneSink::Action::RegisterOtherSink, 2, 1 );
	self->Bind( f.ctrl );
	first->Bind( f.ctrl );
	second->Bind( f.ctrl, first );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	Check( f.ctrl->SetPaneSink( 1, self ), "self-registering sink installed" );
	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"self-registering Last Render callback returns" );
	Check( self->Count() == 1 && self->ActionSucceeded(),
		"MONEY (w8): idempotent same-sink re-registration produces one callback, not recursive delivery" );

	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::Interactive )
	    && f.ctrl->SetPaneSink( 1, first )
	    && f.ctrl->SetPaneSink( 2, second ),
		"cross-pane reentrant sinks installed" );
	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"pane-1 callback synchronously freezes pane 2" );
	Check( first->Count() == 1 && second->Count() == 1
	    && first->ActionSucceeded() && second->ActionSucceeded(),
		"MONEY (w8): cross-pane setter re-entry drains sequentially and returns without AB/BA deadlock" );

	BlockingPaneSink* blockA = new BlockingPaneSink();
	BlockingPaneSink* blockB = new BlockingPaneSink();
	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::Interactive )
	    && f.ctrl->SetPaneContentSource(
			2, SceneEditController::PaneContentSource::Interactive )
	    && f.ctrl->SetPaneSink( 1, blockA )
	    && f.ctrl->SetPaneSink( 2, blockB ),
		"two cross-pane serialization probes installed" );
	std::thread paneOne( [&]{
		(void)f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender );
	} );
	Check( blockA->WaitUntilEntered( kWaitMs ),
		"pane-1 callback owns the global callback barrier" );
	f.ctrl->ArmLastRenderTransitionAttempt( 2 );
	std::thread paneTwo( [&]{
		(void)f.ctrl->SetPaneContentSource(
			2, SceneEditController::PaneContentSource::LastRender );
	} );
	Check( f.ctrl->WaitForLastRenderTransitionAttempt( kWaitMs ),
		"pane-2 transition reached the global barrier attempt" );
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	Check( !blockB->Entered(),
		"MONEY (w8): callbacks for distinct panes cannot enter concurrently" );
	blockA->Release();
	paneOne.join();
	Check( blockB->WaitUntilEntered( kWaitMs ),
		"pane-2 callback enters after pane 1 releases the global barrier" );
	blockB->Release();
	paneTwo.join();

	Check( f.ctrl->SetPaneSink( 1, nullptr )
	    && f.ctrl->SetPaneSink( 2, nullptr ),
		"pane sinks detached" );
	self->release();
	first->release();
	second->release();
	blockA->release();
	blockB->release();
}

//----------------------------------------------------------------------
// A successful source switch is a callback-start barrier: if an old frozen
// callback is already running, the setter waits for it; otherwise the pane
// generation changes before that callback can begin.  This closes the
// generation-check -> virtual-call TOCTOU window.
//----------------------------------------------------------------------
static void RunLastRenderSourceSwitchBarrierTest()
{
	std::printf( "=== scheduler (w9): source switch is atomic with Last Render callback start ===\n" );
	Fixture f( "pane_sched_w9_last_render_callback_barrier.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	BlockingPaneSink* sink = new BlockingPaneSink();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	Check( f.ctrl->SetPaneSink( 1, sink ), "blocking sink installed" );

	std::atomic<bool> frozenReturned( false );
	std::thread freezeThread( [&]{
		(void)f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender );
		frozenReturned.store( true, std::memory_order_release );
	} );
	Check( sink->WaitUntilEntered( kWaitMs ),
		"old Last Render callback is in flight" );

	std::atomic<bool> interactiveReturned( false );
	f.ctrl->ArmLastRenderTransitionAttempt( 1 );
	std::thread sourceThread( [&]{
		(void)f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::Interactive );
		interactiveReturned.store( true, std::memory_order_release );
	} );
	Check( f.ctrl->WaitForLastRenderTransitionAttempt( kWaitMs ),
		"interactive source setter reached the callback/start barrier attempt" );
	Check( !interactiveReturned.load( std::memory_order_acquire ),
		"MONEY (w9): source setter cannot return while the old callback is still running" );

	sink->Release();
	freezeThread.join();
	sourceThread.join();
	Check( frozenReturned.load( std::memory_order_acquire )
	    && interactiveReturned.load( std::memory_order_acquire )
	    && f.ctrl->GetPaneContentSource( 1 )
	       == SceneEditController::PaneContentSource::Interactive,
		"MONEY (w9): callback drains first, then the source transition completes" );
	Check( f.ctrl->SetPaneSink( 1, nullptr ), "blocking sink detached" );
	sink->release();
}

//----------------------------------------------------------------------
// IRasterizerOutput is not noexcept.  A custom sink failure must be contained
// at the notification boundary instead of escaping the agent worker thread
// (which would call std::terminate).
//----------------------------------------------------------------------
static void RunThrowingLastRenderSinkIsContainedTest()
{
	std::printf( "=== scheduler (w10): Last Render sink exceptions are contained ===\n" );
	Fixture f( "pane_sched_w10_throwing_last_render_sink.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	ThrowingPaneSink* sink = new ThrowingPaneSink();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	Check( f.ctrl->SetPaneSink( 1, sink ), "throwing sink installed" );
	bool escaped = false;
	bool changed = false;
	try
	{
		changed = f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender );
	}
	catch( ... )
	{
		escaped = true;
	}
	Check( changed && !escaped,
		"MONEY (w10): throwing sink is logged/contained and the source setter still completes" );
	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::Interactive )
	    && f.ctrl->SetPaneSink( 1, nullptr ),
		"standard-exception sink detached" );
	sink->release();

	ThrowingPaneSink* unknown = new ThrowingPaneSink( true );
	Check( f.ctrl->SetPaneSink( 2, unknown ), "unknown-exception sink installed" );
	escaped = false;
	changed = false;
	try
	{
		changed = f.ctrl->SetPaneContentSource(
			2, SceneEditController::PaneContentSource::LastRender );
	}
	catch( ... )
	{
		escaped = true;
	}
	Check( changed && !escaped,
		"MONEY (w10): non-std sink exceptions are also contained" );
	Check( f.ctrl->SetPaneSink( 2, nullptr ), "unknown-exception sink detached" );
	unknown->release();
}

//----------------------------------------------------------------------
// Teardown closes new drain registration, then waits without holding the
// delivery barrier a previously-registered drain still needs.  The two hooks
// deterministically expose the exact count++ -> delivery-lock seam.
//----------------------------------------------------------------------
static void RunLastRenderDrainQuiescenceTest()
{
	std::printf( "=== scheduler (w11): teardown quiesces a registered Last Render drain ===\n" );
	Fixture f( "pane_sched_w11_drain_quiescence.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;

	RecordingPaneSink* sink = new RecordingPaneSink();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && f.ctrl->SetPaneSink( 1, sink )
	    && f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"Last Render pane and sink installed" );
	IRasterImage* image = nullptr;
	Check( RISE_API_CreateRISEColorRasterImage(
			&image, 2, 2, RISEColor( RISEPel( 0.2, 0.4, 0.6 ), 1.0 ) )
	    && image,
		"replacement image constructs" );
	if( !image ) { sink->release(); return; }

	f.ctrl->ArmLastRenderDrainRegistration();
	std::atomic<bool> renderReturned( false );
	std::thread renderThread( [&]{
		(void)f.ctrl->RunPreviewRenderParked( [&]{
			(void)f.ctrl->PublishAgentRenderImageParked( *image );
		} );
		renderReturned.store( true, std::memory_order_release );
	} );
	Check( f.ctrl->WaitForLastRenderDrainRegistration( kWaitMs ),
		"render completion drain registered before taking delivery barrier" );

	std::atomic<bool> prepareReturned( false );
	std::atomic<bool> prepareResult( false );
	std::thread prepareThread( [&]{
		prepareResult.store( f.ctrl->PrepareForDestruction(), std::memory_order_release );
		prepareReturned.store( true, std::memory_order_release );
	} );
	Check( f.ctrl->WaitForLastRenderQuiesce( kWaitMs ),
		"teardown closed new drain registration and reached its wait" );
	Check( !prepareReturned.load( std::memory_order_acquire ),
		"MONEY (w11): Prepare waits for the already-registered drain" );

	f.ctrl->ReleaseLastRenderDrainRegistration();
	renderThread.join();
	prepareThread.join();
	Check( renderReturned.load( std::memory_order_acquire )
	    && prepareReturned.load( std::memory_order_acquire )
	    && prepareResult.load( std::memory_order_acquire ),
		"MONEY (w11): registered drain and teardown both retire without barrier/count deadlock" );
	image->release();
	sink->release();
}

static void RunDestroyFromLastRenderCallbackIsRejectedTest()
{
	std::printf( "=== scheduler (w12): callback-local Destroy is safely rejected ===\n" );
	Fixture f( "pane_sched_w12_callback_destroy_rejected.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	DestroyingPaneSink* sink = new DestroyingPaneSink();
	sink->Bind( f.ctrl );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && f.ctrl->SetPaneSink( 1, sink ),
		"destroy-attempting sink installed" );
	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"Last Render callback returns after its Destroy attempt" );
	Check( sink->DestroyReturned()
	    && f.ctrl->GetViewportLayout() == SceneEditController::ViewportLayout::Quad,
		"MONEY (w12): callback-local Destroy returns without deleting the active drain/controller" );
	Check( f.ctrl->SetPaneSink( 1, nullptr ),
		"owner can detach callback sink and perform ordinary teardown afterward" );
	sink->release();
}

static void RunDestroyFromSinkDestructorIsRejectedTest()
{
	std::printf( "=== scheduler (w12b): sink-destructor Destroy is safely rejected ===\n" );
	Fixture f( "pane_sched_w12b_sink_destructor_destroy.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	auto state = std::make_shared<DestructorDestroyState>();
	DestructorDestroyingPaneSink* sink = new DestructorDestroyingPaneSink(
		f.ctrl, 1, state );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && f.ctrl->SetPaneSink( 1, sink ),
		"self-detaching destructor-destroy sink installed" );
	// Leave the pane and detached drain batch as the only owners.  OutputImage
	// removes the pane reference; BatchReleaseGuard's later final release runs
	// the sink destructor while the drain registration is still active.
	sink->release();
	Check( f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"self-detaching callback and sink destructor return" );
	Check( state->detached.load( std::memory_order_acquire )
	    && state->destructorDestroyReturned.load( std::memory_order_acquire )
	    && f.ctrl->GetViewportLayout() == SceneEditController::ViewportLayout::Quad,
		"MONEY (w12b): drain-wide callback scope rejects destructor cleanup Destroy instead of self-waiting" );
}

static void RunDestroyFromOrdinarySinkDetachIsRejectedTest()
{
	std::printf( "=== scheduler (w12c): ordinary sink-detach destructor Destroy is safely rejected ===\n" );
	Fixture f( "pane_sched_w12c_sink_detach_destroy.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	auto state = std::make_shared<DestructorDestroyState>();
	DestructorDestroyingPaneSink* sink = new DestructorDestroyingPaneSink(
		f.ctrl, 1, state );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && f.ctrl->SetPaneSink( 1, sink ),
		"destructor-destroy sink installed for ordinary detach" );
	// Leave the pane slot as the only owner.  SetPaneSink(nullptr) detaches under
	// its mutation barriers, then performs the final release after dropping the
	// non-recursive locks; lifecycle teardown is still rejected for the duration
	// of that unlocked destructor callout.
	sink->release();
	Check( f.ctrl->SetPaneSink( 1, nullptr ),
		"ordinary pane-sink detach returns after final-release cleanup" );
	Check( state->destructorDestroyReturned.load( std::memory_order_acquire )
	    && f.ctrl->GetViewportLayout() == SceneEditController::ViewportLayout::Quad,
		"MONEY (w12c): ordinary final release rejects recursive Destroy and preserves the controller" );
}

static void RunDestroyFromQueuedSinkCleanupIsRejectedTest()
{
	std::printf( "=== scheduler (w12d): prepared sink teardown destructor Destroy is safely rejected ===\n" );
	Fixture f( "pane_sched_w12d_queued_sink_destroy.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	auto state = std::make_shared<DestructorDestroyState>();
	DestructorDestroyingPaneSink* sink = new DestructorDestroyingPaneSink(
		f.ctrl, 2, state );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && f.ctrl->SetPaneSink( 2, sink ),
		"destructor-destroy sink installed for queued teardown" );
	sink->release();

	// Terminal preparation now closes both drain registration and every
	// mutation gate.  The post-prepare source change must refuse; the installed
	// pane sink remains owned until the subsequent destructor cleanup.
	Check( f.ctrl->PrepareForDestruction(), "terminal preparation closes drain registration" );
	Check( !f.ctrl->SetPaneContentSource(
			2, SceneEditController::PaneContentSource::LastRender ),
		"MONEY (w12d): terminal preparation refuses a post-prepare source mutation" );
	RISE_API_DestroySceneEditController( f.ctrl );
	f.ctrl = nullptr;
	Check( state->destructorDestroyReturned.load( std::memory_order_acquire ),
		"MONEY (w12d): prepared pane-sink final release rejects recursive Destroy during controller teardown" );
}

static void RunOrdinarySinkDestructorReentryTest()
{
	std::printf( "=== scheduler (w12e): ordinary sink destructor may re-enter a setter ===\n" );
	Fixture f( "pane_sched_w12e_sink_setter_reentry.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	auto state = std::make_shared<DestructorReentryState>();
	DestructorReenteringPaneSink* sink = new DestructorReenteringPaneSink(
		f.ctrl, 1, state );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && f.ctrl->SetPaneSink( 1, sink ),
		"setter-reentering sink installed" );
	sink->release();
	Check( f.ctrl->SetPaneSink( 1, nullptr ),
		"outer ordinary sink detach returns" );
	Check( state->destructorRan.load( std::memory_order_acquire )
	    && state->setterReturned.load( std::memory_order_acquire ),
		"MONEY (w12e): final sink release runs after controller locks are dropped, so redundant setter re-entry returns" );
}

static void RunRasterizerOwnedSinkDestructorReentryTest()
{
	std::printf( "=== scheduler (w12f): rasterizer-final sink destructor may re-enter a setter ===\n" );
	Fixture f( "pane_sched_w12f_rasterizer_sink_reentry.RISEscene", true );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	auto state = std::make_shared<DestructorReentryState>();
	DestructorReenteringPaneSink* sink = new DestructorReenteringPaneSink(
		f.ctrl, 1, state, /*shrinkLayout*/ true );
	Check( f.ctrl->SetPaneSink( 1, sink ), "rendered setter-reentering sink installed" );
	sink->release();

	f.ctrl->ArmOutputDetachGate( 1 );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForOutputDetachGate( kWaitMs ),
		"pane-1 pass published mRendering=false while rasterizer still owns its sink" );
	Check( f.ctrl->SetPaneSink( 1, nullptr ),
		"controller pane ref detaches while rasterizer remains the final owner" );
	Check( !state->destructorRan.load( std::memory_order_acquire ),
		"precondition: rasterizer output ref keeps the detached sink alive" );
	f.ctrl->ReleaseOutputDetachGate();
	Check( f.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
		"rasterizer output retirement completes" );
	Check( state->destructorRan.load( std::memory_order_acquire )
	    && state->setterReturned.load( std::memory_order_acquire )
	    && f.ctrl->GetViewportLayout() == SceneEditController::ViewportLayout::Single,
		"MONEY (w12f): rasterizer-final release occurs unlocked after mRendering=false, so a park-capable layout mutation returns" );
}

static void RunRasterizerSinkDestructorQueuesLastRenderTest()
{
	std::printf( "=== scheduler (w12g): rasterizer-final destructor publish drains immediately ===\n" );
	Fixture f( "pane_sched_w12g_rasterizer_destructor_publish.RISEscene", true );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
		"layout TwoH" );
	RecordingPaneSink* pane0Sink = new RecordingPaneSink();
	auto state = std::make_shared<DestructorReentryState>();
	DestructorQueuesLastRenderPaneSink* pane1Sink =
		new DestructorQueuesLastRenderPaneSink( f.ctrl, 0, state );
	Check( f.ctrl->SetPaneSink( 0, pane0Sink )
	    && f.ctrl->SetPaneSink( 1, pane1Sink ),
		"pane-0 witness and pane-1 destructor-publish sink install" );
	pane1Sink->release();

	f.ctrl->ArmOutputDetachGate( 1 );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForOutputDetachGate( kWaitMs ),
		"pane-1 rasterizer retains the destructor-publish sink" );
	Check( f.ctrl->SetPaneSink( 1, nullptr ),
		"controller pane-1 ref detaches before rasterizer retirement" );
	f.ctrl->ReleaseOutputDetachGate();
	Check( f.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
		"pane-1 output retirement completes" );
	Check( state->destructorRan.load( std::memory_order_acquire )
	    && state->setterReturned.load( std::memory_order_acquire ),
		"rasterizer-final destructor queues pane 0 as Last Render" );
	Check( pane0Sink->WaitForDimensions( 1, 1, kWaitMs ),
		"MONEY (w12g): destructor-queued Last Render placeholder drains without an unrelated UI tick/pass" );
	Check( f.ctrl->SetPaneSink( 0, nullptr ), "pane-0 witness detaches" );
	pane0Sink->release();
}

static void RunDestructorRejectsOrdinarySinkReentryTest()
{
	std::printf( "=== scheduler (w12h): controller teardown rejects ordinary sink re-entry ===\n" );
	Fixture f( "pane_sched_w12h_teardown_setter_reentry.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
		"layout Quad" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForPassCount( 4, kWaitMs )
	    && f.ctrl->WaitForCompletedPassCount( 4, kWaitMs )
	    && f.ctrl->SettlesAt( 4, kSettleMs )
	    && f.ctrl->ForTest_CurrentPane() == 3,
		"precondition: teardown begins with pane 3 current" );
	auto state = std::make_shared<DestructorReentryState>();
	DestructorReenteringPaneSink* sink = new DestructorReenteringPaneSink(
		f.ctrl, 3, state, /*shrinkLayout*/ true );
	Check( f.ctrl->SetPaneSink( 3, sink ),
		"teardown setter-reentry sink installed on current pane 3" );
	sink->release();
	RISE_API_DestroySceneEditController( f.ctrl );
	f.ctrl = nullptr;
	Check( state->destructorRan.load( std::memory_order_acquire )
	    && !state->setterReturned.load( std::memory_order_acquire ),
		"MONEY (w12h): destructor-body sink release rejects Quad-to-Single mutation against partially retired state" );
}

static void RunDestructorRejectsRollbackReentryTest()
{
	std::printf( "=== scheduler (w12i): controller teardown rejects transaction rollback re-entry ===\n" );
	Fixture f( "pane_sched_w12i_teardown_rollback_reentry.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && f.ctrl->BeginTransaction(),
		"open transaction exists at teardown" );
	auto state = std::make_shared<DestructorReentryState>();
	DestructorRollsBackTransactionPaneSink* sink =
		new DestructorRollsBackTransactionPaneSink( f.ctrl, state );
	Check( f.ctrl->SetPaneSink( 3, sink ),
		"rollback-attempting sink installed" );
	sink->release();
	RISE_API_DestroySceneEditController( f.ctrl );
	f.ctrl = nullptr;
	Check( state->destructorRan.load( std::memory_order_acquire )
	    && !state->setterReturned.load( std::memory_order_acquire ),
		"MONEY (w12i): destructor-body RollbackTransaction fails closed before touching borrowed Job/editor state" );
}

static void RunStopFromLastRenderCallbackIsRejectedTest()
{
	std::printf( "=== scheduler (w13): callback-local Stop is safely rejected ===\n" );
	Fixture f( "pane_sched_w13_callback_stop_rejected.RISEscene" );
	Check( f.ctrl != nullptr, "fixture constructs" );
	if( !f.ctrl ) return;
	ReentrantPaneSink* sink = new ReentrantPaneSink(
		ReentrantPaneSink::Action::StopController, 1, 1 );
	sink->Bind( f.ctrl );
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && f.ctrl->SetPaneSink( 1, sink )
	    && f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"stop-attempting Last Render sink installed unarmed" );
	IRasterImage* image = nullptr;
	Check( RISE_API_CreateRISEColorRasterImage(
			&image, 2, 2, RISEColor( RISEPel( 0.3, 0.5, 0.7 ), 1.0 ) )
	    && image,
		"callback-stop image constructs" );
	if( !image ) { sink->release(); return; }

	sink->Arm();
	bool directPublished = false;
	Check( f.ctrl->RunPreviewRenderParked( [&]{
			directPublished = f.ctrl->PublishAgentRenderImageParked( *image );
		} ) && directPublished,
		"MONEY (w13): direct-render tail returns when its sink attempts Stop" );
	SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
	bool agentPublished = false;
	Check( f.ctrl->SubmitAgentRenderSync( [&]{
			agentPublished = f.ctrl->PublishAgentRenderImageParked( *image );
		}, String( "w13_agent_callback_stop" ), &jobId, kWaitMs )
	    && agentPublished && sink->ActionSucceeded(),
		"MONEY (w13): agent-worker completion callback rejects Stop instead of self-joining" );
	Check( f.ctrl->SetPaneSink( 1, nullptr ),
		"owner detaches stop-attempting sink for ordinary teardown" );
	image->release();
	sink->release();
}

static void RunRawDestructorClaimsBeforeDrainQuiescenceTest()
{
	std::printf( "=== scheduler (w14): raw destructor claims ownership before drain quiescence ===\n" );
	const std::string path = WriteTemp( "pane_sched_w14_raw_destroy_claim.RISEscene", kScene );
	Job* job = new Job();
	Check( job->LoadAsciiSceneViaCst( path.c_str() ), "fixture scene loads" );
	SceneEditController* controller = new SceneEditController( *job, nullptr );
	DrainBlockingPaneSink* sink = new DrainBlockingPaneSink();
	Check( controller->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && controller->SetPaneSink( 1, sink )
	    && controller->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
		"Last Render drain fixture installs" );
	IRasterImage* image = nullptr;
	Check( RISE_API_CreateRISEColorRasterImage(
			&image, 2, 2, RISEColor( RISEPel( 0.2, 0.4, 0.6 ), 1.0 ) )
	    && image,
		"drain image constructs" );
	if( !image ) { sink->release(); return; }

	// Hold a dirty callback first.  Its mutating SetProperty has already
	// released controller/render admission before notification delivery, so a
	// direct render can independently reach the registered-drain gate below.
	Check( controller->SetSelection(
			SceneEditController::Category::Camera, String( "cam" ) ),
		"camera selected for dirty callback" );
	std::atomic<bool> callbackEntered{ false };
	std::atomic<bool> allowCallbackDestroy{ false };
	std::atomic<bool> callbackDestroyReturned{ false };
	controller->SetDirtyChangedListener( [&]( bool ) {
		callbackEntered.store( true, std::memory_order_release );
		while( !allowCallbackDestroy.load( std::memory_order_acquire ) )
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		RISE_API_DestroySceneEditController( controller );
		callbackDestroyReturned.store( true, std::memory_order_release );
	} );
	std::thread editThread( [&] {
		(void)controller->SetProperty( String( "fov" ), String( "42" ) );
	} );
	for( unsigned int i = 0;
	     i < 5000 && !callbackEntered.load( std::memory_order_acquire ); ++i )
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	Check( callbackEntered.load( std::memory_order_acquire ),
		"dirty callback is held in flight" );

	sink->Arm();
	std::atomic<bool> renderReturned{ false };
	std::thread renderThread( [&] {
		(void)controller->RunPreviewRenderParked( [&] {
			(void)controller->PublishAgentRenderImageParked( *image );
		} );
		renderReturned.store( true, std::memory_order_release );
	} );
	Check( sink->WaitUntilEntered( kWaitMs ),
		"registered Last Render callback is held before raw destruction" );
	std::atomic<bool> rawDestroyReturned{ false };
	std::thread destroyThread( [&] {
		delete controller;
		rawDestroyReturned.store( true, std::memory_order_release );
	} );
	for( unsigned int i = 0;
	     i < 5000 && !controller->IsRawDestructionInProgress(); ++i )
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	Check( controller->IsRawDestructionInProgress(),
		"raw destructor publishes ownership before waiting on the registered drain" );
	allowCallbackDestroy.store( true, std::memory_order_release );
	editThread.join();
	Check( callbackDestroyReturned.load( std::memory_order_acquire )
	    && !rawDestroyReturned.load( std::memory_order_acquire ),
		"MONEY (w14): callback C Destroy is rejected while the raw owner remains blocked on its earlier drain" );
	sink->Release();
	renderThread.join();
	destroyThread.join();
	controller = nullptr;
	Check( renderReturned.load( std::memory_order_acquire )
	    && rawDestroyReturned.load( std::memory_order_acquire ),
		"raw owner completes after the registered drain retires" );
	image->release();
	sink->release();
	job->release();
	std::remove( path.c_str() );
}

static void RunCallbackPrepareRetryPersistsGestureTest()
{
	std::printf( "=== scheduler (w15): callback-local Prepare rejects without poisoning owner retry ===\n" );
	const std::string path = WriteTemp( "pane_sched_w15_callback_prepare.RISEscene", kScene );
	Job* job = new Job();
	Check( job->LoadAsciiSceneViaCst( path.c_str() ), "fixture scene loads" );
	const std::string before = Cst::SerializeCst( *job->GetCstDocument() );
	SceneEditController* controller = new SceneEditController( *job, nullptr );
	ReentrantPaneSink* sink = new ReentrantPaneSink(
		ReentrantPaneSink::Action::PrepareController, 1, 1 );
	sink->Bind( controller );
	Check( controller->SetViewportLayout( SceneEditController::ViewportLayout::Quad )
	    && controller->SetPaneSink( 1, sink ),
		"prepare-attempting Last Render sink installs unarmed" );
	controller->SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	controller->SetTool( SceneEditController::Tool::TranslateObject );
	controller->OnPointerDown( Point2( 8, 8 ) );
	controller->OnPointerMove( Point2( 24, 20 ) );
	Check( controller->Editor().HasPendingCstObjectTransforms(),
		"open drag has a pending live-only transform" );
	sink->Arm();
	Check( controller->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender )
	    && sink->ActionSucceeded(),
		"callback-local Prepare returns false without deadlock" );
	Check( controller->Editor().HasPendingCstObjectTransforms(),
		"callback-local refusal leaves the open interaction for the owner" );
	Check( controller->PrepareForDestruction(),
		"owner retries Prepare successfully after the callback returns" );
	const std::string after = Cst::SerializeCst( *job->GetCstDocument() );
	Check( after != before
	    && !controller->Editor().HasPendingCstObjectTransforms(),
		"MONEY (w15): owner retry persists and clears the open transform instead of inheriting a poisoned Prepared state" );
	RISE_API_DestroySceneEditController( controller );
	sink->release();
	job->release();
	std::remove( path.c_str() );
}

static void RunBeautyVariantLiveQualityPolicyTest()
{
	std::printf( "=== scheduler (perf): BeautyVariant live/final quality policy ===\n" );
	Fixture f( "pane_sched_variant_quality.RISEscene", true );
	Check( f.ctrl != nullptr, "live-pipeline fixture constructs" );
	if( !f.ctrl ) return;
	// Surface dimensions are render-target overrides only in N-up. Single's
	// pane-0 measurement is display-only so a Retina/window resize cannot
	// bypass the fit-capped interactive policy. Freeze pane 1 on Last Render
	// so this quality-policy test still schedules only pane 0.
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH )
	    && f.ctrl->SetPaneContentSource(
			1, SceneEditController::PaneContentSource::LastRender ),
	       "variant-quality fixture enters one-interactive-pane N-up layout" );
	Check( f.ctrl->SetViewportRenderMode( "direct" ),
	       "direct BeautyVariant installs" );
	f.ctrl->Start( true );
	Check( f.ctrl->WaitForVariantPassCount( 1, kWaitMs ),
	       "initial final-quality variant pass mints" );
	Check( f.ctrl->WaitForCompletedPassCount( 1, kWaitMs ),
	       "initial variant pass retires" );
#ifdef RISE_ENABLE_OIDN
	unsigned int initialAOVW = 0, initialAOVH = 0;
	Check( f.ctrl->LastVariantAOVDimensions( initialAOVW, initialAOVH )
	    && initialAOVW == 16 && initialAOVH == 12,
	       "initial full-quality pass retains correctly-sized 16x12 AOVs" );
#endif
	f.ctrl->ClearSequence();
	f.ctrl->ClearVariantPassRecords();

	auto checkBracket = [&]( const char* label, const auto& begin, const auto& end ) {
		begin();
		f.ctrl->ForTest_KickRender();
		Check( f.ctrl->WaitForVariantPassCount( 1, kWaitMs ),
		       std::string( label ) + ": live pass mints" );
		end();
		Check( f.ctrl->WaitForVariantPassCount( 2, kWaitMs ),
		       std::string( label ) + ": release pass mints" );
		const std::vector<RecordingController::VariantPassRecord> records =
			f.ctrl->VariantPassRecords();
		Check( records.size() >= 2 && records.front().liveGesture
		    && records.front().samples == 1,
		       std::string( "MONEY: " ) + label +
		       " controller mint classifies live work at 1 SPP" );
		Check( records.size() >= 2 && !records.back().liveGesture
		    && records.back().samples == 8,
		       std::string( "MONEY: " ) + label +
		       " controller release restores Direct's 8 SPP" );
#ifdef RISE_ENABLE_OIDN
		Check( records.size() >= 2 && records.front().denoiseKnown
		    && !records.front().denoiseEnabled,
		       std::string( "MONEY: " ) + label +
		       " live mint suppresses OIDN" );
		Check( records.size() >= 2 && records.back().denoiseKnown
		    && records.back().denoiseEnabled,
		       std::string( "MONEY: " ) + label +
		       " release mint restores OIDN" );
#endif
		Check( f.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
		       std::string( label ) + ": both passes retire" );
		f.ctrl->ClearSequence();
		f.ctrl->ClearVariantPassRecords();
	};

	// Deterministically hold a full-quality mint, begin a new pointer gesture
	// on another thread, and prove the admission path atomically suppresses
	// the obsolete quantum's OIDN tail before waiting for retirement.
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	f.ctrl->ArmReleaseInterleaving();
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForMintedPass( kWaitMs ),
	       "full-quality predecessor is held after mint" );
	std::atomic<bool> pointerDownReturned{ false };
	std::thread pointerDownThread( [&] {
		f.ctrl->OnPointerDown( Point2( 8, 8 ) );
		pointerDownReturned.store( true, std::memory_order_release );
	} );
	for( unsigned int i = 0;
	     i < 2000 && !f.ctrl->LastVariantDenoiseSuppressed(); ++i )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
	Check( f.ctrl->LastVariantDenoiseSuppressed(),
	       "MONEY: new gesture suppresses the in-flight full pass's OIDN tail" );
	Check( !pointerDownReturned.load( std::memory_order_acquire ),
	       "gesture admission waits only for the deliberately held path quantum" );
	f.ctrl->ReleaseMintedPass();
	pointerDownThread.join();
	Check( pointerDownReturned.load( std::memory_order_acquire ),
	       "gesture admission returns after obsolete quantum retires" );
	const std::size_t beforeSurfaceRecords =
		f.ctrl->VariantPassRecords().size();
	const std::size_t beforeSurfacePasses = f.ctrl->Sequence().size();
	Check( f.ctrl->SetPaneSurfaceDims( 0, 20, 14 ),
	       "live pointer pass changes surface dimensions" );
	Check( f.ctrl->WaitForVariantPassCount( beforeSurfaceRecords + 1, kWaitMs )
	    && f.ctrl->WaitForCompletedPassCount( beforeSurfacePasses + 1, kWaitMs ),
	       "1-SPP live pass at changed dimensions retires" );
	{
		const std::vector<RecordingController::VariantPassRecord> records =
			f.ctrl->VariantPassRecords();
		Check( records.size() >= 2 && records.back().liveGesture
		    && records.back().samples == 1,
		       "MONEY: replacement pointer pass is 1 SPP" );
	}
#ifdef RISE_ENABLE_OIDN
	unsigned int liveAOVW = 0, liveAOVH = 0;
	Check( !f.ctrl->LastVariantAOVDimensions( liveAOVW, liveAOVH ),
	       "MONEY: live no-OIDN pass deletes stale prior-resolution AOV storage" );
#endif
	f.ctrl->OnPointerUp( Point2( 8, 8 ) );
	const std::size_t beforeReleaseRecords = beforeSurfaceRecords + 1;
	const std::size_t beforeReleasePasses = beforeSurfacePasses + 1;
	Check( f.ctrl->WaitForVariantPassCount( beforeReleaseRecords + 1, kWaitMs )
	    && f.ctrl->WaitForCompletedPassCount( beforeReleasePasses + 1, kWaitMs ),
	       "pointer release full-quality pass retires" );
	{
		const std::vector<RecordingController::VariantPassRecord> records =
			f.ctrl->VariantPassRecords();
		Check( records.size() >= beforeReleaseRecords + 1
		    && !records.back().liveGesture
		    && records.back().samples == 8,
		       "pointer release restores Direct's 8 SPP" );
	}
#ifdef RISE_ENABLE_OIDN
	unsigned int restoredAOVW = 0, restoredAOVH = 0;
	Check( f.ctrl->LastVariantAOVDimensions( restoredAOVW, restoredAOVH )
	    && restoredAOVW == 10 && restoredAOVH == 7,
	       "release recreates AOV storage at the changed 10x7 dimensions" );
#endif
	f.ctrl->ClearSequence();
	f.ctrl->ClearVariantPassRecords();

	checkBracket( "property scrub",
		[&]{ f.ctrl->BeginPropertyScrub(); },
		[&]{ f.ctrl->EndPropertyScrub(); } );
	checkBracket( "timeline scrub",
		[&]{ Check( f.ctrl->OnTimeScrubBegin(), "timeline scrub begins" ); },
		[&]{ Check( f.ctrl->OnTimeScrubEnd(), "timeline scrub ends" ); } );

	// Deliberately omit the timeline End.  The shared scrub watchdog must
	// close the editor composite and restore a final-quality pass rather than
	// leaving every later variant quantum at 1 SPP forever.
	f.ctrl->ClearSequence();
	f.ctrl->ClearVariantPassRecords();
	Check( f.ctrl->OnTimeScrubBegin(), "lost-End timeline scrub begins" );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForVariantPassCount( 1, kWaitMs ),
	       "lost-End timeline live pass mints" );
	Check( f.ctrl->WaitForVariantPassCount( 2, kWaitMs ),
	       "timeline watchdog mints a recovery pass" );
	{
		const std::vector<RecordingController::VariantPassRecord> records =
			f.ctrl->VariantPassRecords();
		Check( records.size() >= 2 && records.front().liveGesture
		    && records.front().samples == 1,
		       "lost-End timeline starts at live 1-SPP quality" );
		Check( records.size() >= 2 && !records.back().liveGesture
		    && records.back().samples == 8,
		       "MONEY: timeline watchdog restores Direct's final 8-SPP quality" );
	}
	Check( !f.ctrl->Editor().IsCompositeOpen(),
	       "timeline watchdog closes the orphaned editor composite" );

	// A legitimate user can pause on one frame past the watchdog and then
	// resume without a second Begin callback.  The next value event must
	// reclaim live quality and a fresh undo group until the eventual End.
	f.ctrl->ClearSequence();
	f.ctrl->ClearVariantPassRecords();
	Check( f.ctrl->OnTimeScrub( 0.25 ),
	       "timeline motion after watchdog recovery is accepted" );
	Check( f.ctrl->Editor().IsCompositeOpen(),
	       "resumed timeline motion reopens its editor composite" );
	Check( f.ctrl->WaitForVariantPassCount( 1, kWaitMs ),
	       "resumed timeline motion mints a variant pass" );
	{
		const std::vector<RecordingController::VariantPassRecord> records =
			f.ctrl->VariantPassRecords();
		Check( !records.empty() && records.front().liveGesture
		    && records.front().samples == 1,
		       "MONEY: resumed timeline motion reclaims 1-SPP live quality" );
	}
	Check( f.ctrl->OnTimeScrubEnd(), "resumed timeline scrub ends cleanly" );
}

static void RunCrossPaneVariantAdmissionPolicyTest()
{
	std::printf( "=== scheduler (perf2): Preview-to-variant admission pins live quality ===\n" );
	Fixture f( "pane_sched_cross_variant_admission.RISEscene", true );
	Check( f.ctrl != nullptr, "live-pipeline fixture constructs" );
	if( !f.ctrl ) return;
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
	       "layout TwoH" );
	Check( f.ctrl->SetPaneRenderMode( 1, "direct" ),
	       "pane 1 installs Direct while pane 0 remains Preview" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
	       "initial two-pane rotation retires" );
	f.ctrl->ClearSequence();
	f.ctrl->ClearVariantPassRecords();

	// Hold a minted Preview pass.  The pane-1 Down must arm its destination
	// variant before the cancel/park wait, even though the currently loaded
	// pane has no variant rasterizer to suppress.
	f.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	f.ctrl->ArmReleaseInterleaving();
	// An all-pane kick leaves Direct dirty while primary Preview is held.
	// This is the exact retirement shape that used to let the dirty target
	// re-mint at final quality before pointer-down was published.
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForMintedPass( kWaitMs ), "Preview pass is held after mint" );
	std::atomic<bool> downAccepted{ false };
	std::thread downThread( [&] {
		downAccepted.store(
			f.ctrl->OnPanePointerDown( 1, Point2( 8, 8 ) ),
			std::memory_order_release );
	} );
	for( unsigned int i = 0;
	     i < 2000 && !f.ctrl->ForTest_VariantAdmissionPendingForPane( 1 ); ++i )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
	Check( f.ctrl->ForTest_VariantAdmissionPendingForPane( 1 ),
	       "MONEY: destination Direct pane is pinned live before the wrapper's first cancel wait" );
	f.ctrl->ReleaseMintedPass();
	downThread.join();
	Check( downAccepted.load( std::memory_order_acquire ),
	       "cross-pane pointer Down is admitted after Preview retires" );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForVariantPassCount( 1, kWaitMs ),
	       "target Direct pane mints after cross-pane admission" );
	{
		const std::vector<RecordingController::VariantPassRecord> records =
			f.ctrl->VariantPassRecords();
		Check( !records.empty() && records.front().liveGesture
		    && records.front().samples == 1,
		       "MONEY: Preview-to-Direct admission cannot mint a full-quality pass before pointer-down publishes" );
	}
	Check( f.ctrl->OnPanePointerUp( 1, Point2( 8, 8 ) ),
	       "cross-pane gesture releases cleanly" );

	// Inverse direction: a Preview destination does not need a pending
	// quality pin, but it still must suppress the currently-running Direct
	// predecessor before the wrapper's first cancel/wait.
	Fixture inverse( "pane_sched_cross_variant_predecessor.RISEscene", true );
	Check( inverse.ctrl != nullptr, "inverse live-pipeline fixture constructs" );
	if( !inverse.ctrl ) return;
	Check( inverse.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
	       "inverse layout TwoH" );
	Check( inverse.ctrl->SetPaneRenderMode( 1, "direct" ),
	       "inverse pane 1 installs Direct" );
	Check( inverse.ctrl->SetPrimaryPane( 1 ),
	       "inverse all-pane kick starts on Direct" );
	inverse.ctrl->Start( true );
	inverse.ctrl->ForTest_KickRender();
	Check( inverse.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
	       "inverse initial rotation retires" );
	inverse.ctrl->ClearSequence();
	inverse.ctrl->ClearVariantPassRecords();
	inverse.ctrl->SetTool( SceneEditController::Tool::OrbitCamera );
	inverse.ctrl->ArmReleaseInterleaving();
	inverse.ctrl->ForTest_KickRender();
	Check( inverse.ctrl->WaitForMintedPass( kWaitMs ),
	       "Direct predecessor is held after mint" );
	std::atomic<bool> inverseDownAccepted{ false };
	std::thread inverseDownThread( [&] {
		inverseDownAccepted.store(
			inverse.ctrl->OnPanePointerDown( 0, Point2( 8, 8 ) ),
			std::memory_order_release );
	} );
	for( unsigned int i = 0;
	     i < 2000 && !inverse.ctrl->LastVariantDenoiseSuppressed(); ++i )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
	Check( inverse.ctrl->LastVariantDenoiseSuppressed(),
	       "MONEY: Direct-to-Preview admission suppresses the obsolete Direct OIDN tail" );
	inverse.ctrl->ReleaseMintedPass();
	inverseDownThread.join();
	Check( inverseDownAccepted.load( std::memory_order_acquire ),
	       "Direct-to-Preview pointer Down retires cleanly" );
	Check( inverse.ctrl->OnPanePointerUp( 0, Point2( 8, 8 ) ),
	       "inverse cross-pane gesture releases cleanly" );
}

static void RunSharedDirectIndirectTransportTest()
{
	std::printf( "=== scheduler (perf3): one Indirect pass also satisfies Direct ===\n" );
	// A strong pixel oracle catches a superficially green implementation that
	// merely publishes the Indirect image twice.
	Fixture f( "pane_sched_shared_direct_indirect.RISEscene", true,
		kSharedDirectScene );
	Check( f.ctrl != nullptr, "shared-transport fixture constructs" );
	if( !f.ctrl ) return;
	RecordingPaneSink* directSink = new RecordingPaneSink();
	RecordingPaneSink* indirectSink = new RecordingPaneSink();
	Check( f.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH ),
	       "shared layout TwoH" );
	Check( f.ctrl->SetViewportRenderMode( "direct" ),
	       "pane 0 is Direct" );
	Check( f.ctrl->SetPaneRenderMode( 1, "indirect" ),
	       "pane 1 is Indirect" );
	Check( f.ctrl->SetPaneSurfaceDims( 0, 8, 6 )
	    && f.ctrl->SetPaneSurfaceDims( 1, 16, 12 ),
	       "Direct is a same-aspect lower-resolution subview" );
	Check( f.ctrl->SetPaneSink( 0, directSink )
	    && f.ctrl->SetPaneSink( 1, indirectSink ),
	       "both pane sinks install" );
	f.ctrl->Start( true );
	f.ctrl->ForTest_KickRender();
	Check( f.ctrl->WaitForCompletedPassCount( 1, kWaitMs ),
	       "shared source quantum retires" );
	Check( directSink->WaitForDimensions( 4, 3, kWaitMs ),
	       "Direct receives a 4x3 NNB companion" );
	Check( indirectSink->WaitForDimensions( 8, 6, kWaitMs ),
	       "Indirect receives its canonical 8x6 render" );
	const IScene* sharedScene = f.job ? f.job->GetScene() : nullptr;
	const ICamera* sharedCamera = sharedScene ? sharedScene->GetCamera() : nullptr;
	const Scalar expectedEV = sharedCamera
		? sharedCamera->GetExposureCompensationEV() : Scalar( 0 );
	Check( directSink->ExposureCount() == 1
	    && std::fabs( directSink->ExposureEV() - expectedEV ) < 1e-12,
	       "shared Direct receives the camera-exposure protocol callback" );
#ifdef RISE_ENABLE_OIDN
	Check( directSink->PreDenoisedCount() == 1
	    && directSink->DenoisedCount() == 1,
	       "shared Direct preserves pre-denoised/denoised output protocol" );
#else
	Check( directSink->PreDenoisedCount() == 0
	    && directSink->DenoisedCount() == 0,
	       "OIDN-disabled shared Direct uses the ordinary output protocol" );
#endif
	{
		const RISEColor directCenter = directSink->CenterPixel();
		const RISEColor indirectCenter = indirectSink->CenterPixel();
		const Scalar directPeak = std::max( directCenter.base.r,
			std::max( directCenter.base.g, directCenter.base.b ) );
		const Scalar indirectPeak = std::max( indirectCenter.base.r,
			std::max( indirectCenter.base.g, indirectCenter.base.b ) );
		Check( directPeak > 0.01,
		       "MONEY: paired Direct companion contains the primary NEE contribution" );
		// OIDN can reconstruct a small non-zero value into an estimator pixel
		// whose raw input is black, so compare the two separately-denoised
		// outputs by discrimination margin rather than requiring exact zero.
		Check( indirectPeak * 20 < directPeak,
		       "MONEY: canonical Indirect remains at least 20x dimmer than the paired primary-NEE Direct result" );
	}
	Check( f.ctrl->SettlesAt( 1, kSettleMs ),
	       "MONEY: one quantum satisfies both dirty panes; no second Direct trace runs" );
	{
		const std::vector<unsigned int> seq = f.ctrl->Sequence();
		Check( seq.size() == 1 && seq.front() == 1,
		       "MONEY: scheduler chooses Indirect first even though Direct pane 0 is primary" );
	}
	directSink->release();
	indirectSink->release();

	Fixture bound( "pane_sched_shared_bound_framestore.RISEscene", true,
		kSharedDirectScene );
	Check( bound.ctrl != nullptr, "shared bound-FrameStore fixture constructs" );
	if( !bound.ctrl ) return;
	Implementation::ViewportFrameStore* boundDirectSink =
		new Implementation::ViewportFrameStore();
	RecordingPaneSink* boundIndirectSink = new RecordingPaneSink();
	std::atomic<unsigned int> boundPreCount{ 0 };
	std::atomic<unsigned int> boundFinalCount{ 0 };
	boundDirectSink->SetPreDenoiseCompleteCallback(
		[&]( unsigned int, uint64_t ) {
			boundPreCount.fetch_add( 1, std::memory_order_acq_rel );
		} );
	boundDirectSink->SetDenoiseCompleteCallback(
		[&]( unsigned int, uint64_t ) {
			boundFinalCount.fetch_add( 1, std::memory_order_acq_rel );
		} );
	boundDirectSink->SetFrameCompleteCallback(
		[&]( unsigned int, uint64_t ) {
			boundFinalCount.fetch_add( 1, std::memory_order_acq_rel );
		} );
	Check( bound.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH )
	    && bound.ctrl->SetViewportRenderMode( "direct" )
	    && bound.ctrl->SetPaneRenderMode( 1, "indirect" )
	    && bound.ctrl->SetPaneSurfaceDims( 0, 8, 6 )
	    && bound.ctrl->SetPaneSurfaceDims( 1, 16, 12 )
	    && bound.ctrl->SetPaneSink( 0, boundDirectSink )
	    && bound.ctrl->SetPaneSink( 1, boundIndirectSink ),
	       "shared bound-FrameStore pair configures" );
	bound.ctrl->Start( true );
	bound.ctrl->ForTest_KickRender();
	Check( bound.ctrl->WaitForCompletedPassCount( 1, kWaitMs ),
	       "shared bound-FrameStore source quantum retires" );
	for( unsigned int i = 0;
	     i < 2000 && boundFinalCount.load( std::memory_order_acquire ) == 0; ++i )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
	unsigned int boundW = 0, boundH = 0;
	boundDirectSink->GetDimensions( boundW, boundH );
	Check( boundDirectSink->IsExternallyBound() && boundW == 4 && boundH == 3,
	       "MONEY: shared Direct rebinds the target pane's canonical FrameStore" );
	Check( boundFinalCount.load( std::memory_order_acquire ) == 1,
	       "MONEY: shared Direct completes the bound FrameStore observer exactly once" );
#ifdef RISE_ENABLE_OIDN
	Check( boundPreCount.load( std::memory_order_acquire ) == 1,
	       "shared Direct completes the bound pre-denoise observer exactly once" );
#else
	Check( boundPreCount.load( std::memory_order_acquire ) == 0,
	       "OIDN-disabled shared Direct does not signal pre-denoise" );
#endif
	const Implementation::FrameStore* boundStore = boundDirectSink->GetFrameStore();
	const RISEColor boundCenter = boundStore
		? boundStore->AsBeautyRasterImage().GetPEL( 2, 1 ) : RISEColor();
	Check( std::max( boundCenter.base.r,
		std::max( boundCenter.base.g, boundCenter.base.b ) ) > 0.01,
	       "MONEY: bound target FrameStore contains the Direct companion pixels" );
	Check( bound.ctrl->SettlesAt( 1, kSettleMs ),
	       "bound FrameStore delivery does not schedule a redundant Direct trace" );
	boundDirectSink->release();
	boundIndirectSink->release();

	Fixture incompatible( "pane_sched_unshared_direct_indirect.RISEscene", true,
		kSharedDirectScene );
	Check( incompatible.ctrl != nullptr,
	       "incompatible shared-transport fixture constructs" );
	if( !incompatible.ctrl ) return;
	Check( incompatible.ctrl->SetViewportLayout(
		SceneEditController::ViewportLayout::TwoH )
	    && incompatible.ctrl->SetViewportRenderMode( "direct" )
	    && incompatible.ctrl->SetPaneRenderMode( 1, "indirect" )
	    && incompatible.ctrl->SetPaneSurfaceDims( 0, 5, 4 )
	    && incompatible.ctrl->SetPaneSurfaceDims( 1, 10, 8 ),
	       "base-aspect-matched but rounded-pass-aspect-mismatched pair configures" );
	incompatible.ctrl->Start( true );
	incompatible.ctrl->ForTest_KickRender();
	Check( incompatible.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
	       "incompatible pair renders independently" );
	Check( incompatible.ctrl->SettlesAt( 2, kSettleMs ),
	       "MONEY: divisor rounding that changes pass aspect falls back to independent renders" );

	Fixture reentrant( "pane_sched_shared_reentrant_sink.RISEscene", true,
		kSharedDirectScene );
	Check( reentrant.ctrl != nullptr, "shared reentrant-sink fixture constructs" );
	if( !reentrant.ctrl ) return;
	RecordingPaneSink* replacementSink = new RecordingPaneSink();
	ReentrantSharedPaneSink* reentrantSink =
		new ReentrantSharedPaneSink( *reentrant.ctrl, 0, replacementSink );
	RecordingPaneSink* reentrantIndirectSink = new RecordingPaneSink();
	Check( reentrant.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH )
	    && reentrant.ctrl->SetViewportRenderMode( "direct" )
	    && reentrant.ctrl->SetPaneRenderMode( 1, "indirect" )
	    && reentrant.ctrl->SetPaneSurfaceDims( 0, 8, 6 )
	    && reentrant.ctrl->SetPaneSurfaceDims( 1, 16, 12 )
	    && reentrant.ctrl->SetPaneSink( 0, reentrantSink )
	    && reentrant.ctrl->SetPaneSink( 1, reentrantIndirectSink ),
	       "shared reentrant-sink pair configures" );
	reentrant.ctrl->Start( true );
	reentrant.ctrl->ForTest_KickRender();
	Check( reentrant.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
	       "shared pass plus replacement-sink retry retire" );
	Check( replacementSink->WaitForDimensions( 4, 3, kWaitMs ),
	       "replacement sink receives a fresh Direct render" );
	Check( reentrantSink->Reentered() && reentrant.ctrl->SettlesAt( 2, kSettleMs ),
	       "MONEY: shared sink replacement invalidates and retries without render-thread deadlock" );
	reentrantSink->release();
	replacementSink->release();
	reentrantIndirectSink->release();

	Fixture retry( "pane_sched_shared_throw_retry.RISEscene", true,
		kSharedDirectScene );
	Check( retry.ctrl != nullptr, "shared throwing-sink fixture constructs" );
	if( !retry.ctrl ) return;
	ThrowOnceSharedPaneSink* retrySink = new ThrowOnceSharedPaneSink();
	RecordingPaneSink* retryIndirectSink = new RecordingPaneSink();
	Check( retry.ctrl->SetViewportLayout( SceneEditController::ViewportLayout::TwoH )
	    && retry.ctrl->SetViewportRenderMode( "direct" )
	    && retry.ctrl->SetPaneRenderMode( 1, "indirect" )
	    && retry.ctrl->SetPaneSurfaceDims( 0, 8, 6 )
	    && retry.ctrl->SetPaneSurfaceDims( 1, 16, 12 )
	    && retry.ctrl->SetPaneSink( 0, retrySink )
	    && retry.ctrl->SetPaneSink( 1, retryIndirectSink ),
	       "shared throwing-sink pair configures" );
	retry.ctrl->Start( true );
	retry.ctrl->ForTest_KickRender();
	Check( retry.ctrl->WaitForCompletedPassCount( 2, kWaitMs ),
	       "throwing shared sink leaves Direct dirty and schedules a retry" );
	Check( retrySink->Attempts() >= 2 && retry.ctrl->SettlesAt( 2, kSettleMs ),
	       "MONEY: shared delivery failure is retried by an independent Direct quantum" );
	retrySink->release();
	retryIndirectSink->release();
}

int main()
{
	RunRotationOrderTest();
	RunPaneLocalInvalidationTest();
	RunInFlightPaneModeChangeCancelsObsoletePassTest();
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
	RunPaneFreeFlyPreservesNamedSceneCameraTest();
	RunNamedSceneCameraReconcileAndFallbackTest();
	RunNamedSceneCameraStampRecreationTest();
	RunActiveCameraGestureInvalidatesNamedPaneTest();
	RunNamedCameraGestureRoutingTest();
	RunNamedSceneCameraPreRealizationHomeTest();
	RunMultiplePanesBoundToSameCameraTest();
	RunNamedSceneCameraTimeScrubTest();
	RunNamedSceneCameraONBTest();
	RunNamedSceneCameraSetterParksRenderTest();
	RunOutOfRangePaneNavRejectedTest();
	RunLastRenderPaneSourceTest();
	RunLostPointerUpWatchdogTest();
	RunPointerWatchdogAfterRefinementSettlesTest();
	RunActiveDragSurvivesSlowPassesTest();
	RunPointerMoveWatchdogHandoffTest();
	RunPointerDownRenderAdmissionHandoffTest();
	RunTimeScrubBeginRenderAdmissionHandoffTest();
	RunRenderFinalizesLostPointerUpTest();
	RunDirectRenderFinalizesOpenInteractionsTest();
	RunDestructorDoesNotTouchJobTest();
	RunCAbiDestructionPersistsGestureTest();
	RunGestureCommitFailureIsReportedTest();
	RunUndoCommitFailureClosesRenderAdmissionTest();
	RunLastRenderPublishOutsideLockTest();
	RunLastRenderReentrantCallbacksAreBoundedTest();
	RunLastRenderSourceSwitchBarrierTest();
	RunThrowingLastRenderSinkIsContainedTest();
	RunLastRenderDrainQuiescenceTest();
	RunDestroyFromLastRenderCallbackIsRejectedTest();
	RunDestroyFromSinkDestructorIsRejectedTest();
	RunDestroyFromOrdinarySinkDetachIsRejectedTest();
	RunDestroyFromQueuedSinkCleanupIsRejectedTest();
	RunOrdinarySinkDestructorReentryTest();
	RunRasterizerOwnedSinkDestructorReentryTest();
	RunRasterizerSinkDestructorQueuesLastRenderTest();
	RunDestructorRejectsOrdinarySinkReentryTest();
	RunDestructorRejectsRollbackReentryTest();
	RunStopFromLastRenderCallbackIsRejectedTest();
	RunRawDestructorClaimsBeforeDrainQuiescenceTest();
	RunCallbackPrepareRetryPersistsGestureTest();
	RunBeautyVariantLiveQualityPolicyTest();
	RunCrossPaneVariantAdmissionPolicyTest();
	RunSharedDirectIndirectTransportTest();

	std::printf( "\nViewportPaneSchedulerTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
