//////////////////////////////////////////////////////////////////////
//
//  SceneEditorMemoryStressTest.cpp - Regression gate for the
//    interactive scene-editor allocation pattern.
//
//  Drives N=1000 SceneEditController::OnTimeScrub calls against a
//  scene with a displaced_geometry chunk (which forwards through
//  the RefreshMeshVertices observer path on every animator tick),
//  each followed by a DoOneRenderPass, and asserts both peak
//  process RSS AND peak-minus-baseline growth stay under fixed
//  budgets.  The two-bound formulation matters on macOS, where
//  MACH_TASK_BASIC_INFO.resident_size includes shared-cache pages
//  and inflates absolute readings — see the budget comment near
//  the assertion site.
//
//  This is a regression gate, not a targeted reproducer.  It exists
//  to fire when ANY allocation pattern in the interactive edit path
//  goes quadratic — a per-frame heap-allocated staging buffer that
//  isn't released, an observer subscription leak, an EditHistory
//  retention bug, etc.  The motivating failure was the per-frame
//  std::vector<uint8_t> + NSBitmapImageRep churn in
//  RISEViewportBridge.mm that fragmented the macOS xzone allocator
//  after a few minutes of interactive use; that fix already landed,
//  but without an automated harness any future variant of the same
//  shape would slip through code review.
//
//  The controller is constructed in test mode (no interactive
//  rasterizer wired) so DoOneRenderPass is a 1ms sleep — the
//  rasterizer's own working-set is irrelevant to the regression
//  this test gates.  What we're measuring is the residue of edit
//  application + scene mutation, the part the production GUI runs
//  unconditionally regardless of which rasterizer is installed.
//
//  Acceptance: passes on macOS arm64.  RSS sampling is gated on
//  __APPLE__ — non-Apple platforms run the loop and report a 0/0
//  measurement (the test still verifies the loop completes without
//  asserting / crashing).
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "../src/Library/Job.h"
#include "../src/Library/Utilities/MediaPathLocator.h"
#include "../src/Library/SceneEditor/SceneEdit.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Subclass solely to surface the controller's protected
	// DoOneRenderPass as a public Tick() — the production code
	// drives it from the spawned render thread, but this test wants
	// the loop on the main thread so RSS samples line up with edit
	// boundaries.
	class TestController : public SceneEditController
	{
	public:
		explicit TestController( IJobPriv& job )
			: SceneEditController( job, /*interactiveRasterizer*/0 )
		{}

		void Tick() { DoOneRenderPass(); }
	};

#if defined(__APPLE__)
	static size_t SampleResidentBytes()
	{
		mach_task_basic_info_data_t   info;
		mach_msg_type_number_t        count = MACH_TASK_BASIC_INFO_COUNT;
		const kern_return_t kr = task_info(
			mach_task_self(),
			MACH_TASK_BASIC_INFO,
			reinterpret_cast<task_info_t>( &info ),
			&count );
		if( kr != KERN_SUCCESS ) {
			return 0;
		}
		return static_cast<size_t>( info.resident_size );
	}
#else
	static size_t SampleResidentBytes() { return 0; }
#endif

	static bool FileExists( const std::string& path )
	{
		struct stat st;
		return stat( path.c_str(), &st ) == 0;
	}

	// Locate the repo root by walking upward until we find the
	// teapot scene file.  Honours RISE_MEDIA_PATH if it's already
	// pointing at the right tree; otherwise falls back to a CWD walk
	// so the test runs cleanly under run_all_tests.sh, IDE invocations,
	// and direct binary execution.
	static std::string FindRepoRoot()
	{
		const char* relScene =
			"scenes/FeatureBased/Geometry/teapot.RISEscene";

		if( const char* env = std::getenv( "RISE_MEDIA_PATH" ) ) {
			std::string root( env );
			while( !root.empty() && root.back() == '/' ) {
				root.pop_back();
			}
			if( FileExists( root + "/" + relScene ) ) {
				return root;
			}
		}

		char cwd[4096];
		if( !getcwd( cwd, sizeof( cwd ) ) ) {
			return std::string();
		}
		std::string p = cwd;
		for( int depth = 0; depth < 16; ++depth ) {
			if( FileExists( p + "/" + relScene ) ) {
				return p;
			}
			const size_t slash = p.find_last_of( '/' );
			if( slash == std::string::npos || slash == 0 ) {
				break;
			}
			p.resize( slash );
		}
		return std::string();
	}
}

int main()
{
	std::cout << "=== SceneEditor Memory Stress Test ===" << std::endl;

	const std::string repoRoot = FindRepoRoot();
	if( repoRoot.empty() ) {
		std::cout << "  FAIL: could not locate scenes/FeatureBased/Geometry/"
		          << "teapot.RISEscene from CWD or $RISE_MEDIA_PATH"
		          << std::endl;
		return 1;
	}

	// Mirror the rise CLI: set the env var (some rasterizer-output
	// chunks consult getenv directly rather than the locator), and
	// register the located root with the global media-path locator
	// so relative paths inside the scene (.bezier, hdr probe, png
	// displacement, .RISEscript include) resolve via
	// GlobalMediaPathLocator().Find( ... ).
	const std::string mediaPath = repoRoot + "/";
	setenv( "RISE_MEDIA_PATH", mediaPath.c_str(), /*overwrite*/0 );
	GlobalMediaPathLocator().AddPath( mediaPath.c_str() );

	const std::string scenePath =
		repoRoot + "/scenes/FeatureBased/Geometry/teapot.RISEscene";

	Job* pJob = new Job();
	if( !pJob->LoadAsciiScene( scenePath.c_str() ) ) {
		std::cout << "  FAIL: LoadAsciiScene returned false for "
		          << scenePath << std::endl;
		pJob->release();
		return 1;
	}

	// Read the scene's animation_options block so the scrubbed time
	// values land inside the declared range (matches what the GUI
	// timeline widget does).  Defaults (0,1,30) are fine if the
	// scene didn't declare an animation window.
	double timeStart = 0.0, timeEnd = 1.0;
	unsigned int numFrames = 30;
	bool doFields = false, invertFields = false;
	pJob->GetAnimationOptions( timeStart, timeEnd, numFrames,
	                           doFields, invertFields );
	if( timeEnd <= timeStart ) {
		timeEnd = timeStart + 1.0;
	}

	TestController c( *pJob );

	const size_t baselineRSS = SampleResidentBytes();
	size_t       peakRSS     = baselineRSS;

	const int kIterations = 1000;

	c.OnTimeScrubBegin();
	for( int i = 0; i < kIterations; ++i )
	{
		// Triangle wave through the animation window so the
		// animator observer chain (RefreshMeshVertices on
		// displaced_geometry, BSP rebuilds on its child mesh)
		// churns over a full range of times instead of repeatedly
		// re-evaluating the same one — a quadratic-leak symptom
		// hides if the test only ever visits one time value.
		const double phase = static_cast<double>( i % 200 ) / 200.0;
		const double tri   = phase < 0.5 ? phase * 2.0
		                                 : ( 1.0 - phase ) * 2.0;
		const double t     = timeStart + tri * ( timeEnd - timeStart );
		c.OnTimeScrub( static_cast<Scalar>( t ) );

		// Drive the same render-pass entry point the GUI's render
		// thread invokes per frame.  In test mode (no interactive
		// rasterizer wired) this is a 1ms sleep — enough cadence
		// that any per-pass allocation pattern has a chance to
		// land but cheap enough to keep the harness fast.
		c.Tick();

		const size_t rss = SampleResidentBytes();
		if( rss > peakRSS ) peakRSS = rss;
	}
	c.OnTimeScrubEnd();

	pJob->release();

	// Two-budget gate.  MACH_TASK_BASIC_INFO.resident_size on macOS
	// arm64 includes mapped shared-cache pages from the system dyld
	// shared cache (libsystem, Foundation, etc.), so even a freshly
	// loaded Hello-World process reports a few hundred MB resident.
	// A 500 MB absolute peak is therefore not a meaningful gate
	// by itself on Apple platforms — the failing 1.7 GB crash needs
	// a higher absolute bound, AND the more useful regression
	// signal is per-iteration growth (a leaky pattern in the
	// interactive loop pushes RSS upward across the 1000 scrubs;
	// a clean implementation holds steady).
	//
	//   - kPeakBudgetBytes (1 GB): catches catastrophic-allocation
	//     regressions like the 1.7 GB xzone-fragmentation crash that
	//     motivated this test, while leaving comfortable headroom
	//     above the macOS arm64 shared-cache baseline (~640 MB on
	//     today's hardware/OS).
	//
	//   - kGrowthBudgetBytes (256 MB): catches per-iteration leaks.
	//     1000 iterations * 256 KB/iter = 256 MB, so a leak of
	//     ~256 KB per scrub would just trip this; smaller leaks
	//     would need a longer run to surface.  Today's baseline
	//     value of `growth` is 0 MB, so the bound has plenty of
	//     headroom for legitimate amortised growth (animator
	//     observer subscriptions, BSP rebuilds at first hit, etc.).
	const size_t kPeakBudgetBytes   = 1024ULL * 1024ULL * 1024ULL;
	const size_t kGrowthBudgetBytes =  256ULL * 1024ULL * 1024ULL;
	const size_t baseMB   = baselineRSS / ( 1024 * 1024 );
	const size_t peakMB   = peakRSS     / ( 1024 * 1024 );
	const size_t growth   = peakRSS > baselineRSS
	                          ? peakRSS - baselineRSS
	                          : 0;
	const size_t growthMB = growth / ( 1024 * 1024 );

	std::cout << "  iterations  : " << kIterations << std::endl;
	std::cout << "  baseline RSS: " << baseMB   << " MB" << std::endl;
	std::cout << "  peak RSS    : " << peakMB   << " MB" << std::endl;
	std::cout << "  growth      : " << growthMB << " MB" << std::endl;
	std::cout << "  peak budget : " << ( kPeakBudgetBytes   / (1024*1024) )
	          << " MB" << std::endl;
	std::cout << "  grow budget : " << ( kGrowthBudgetBytes / (1024*1024) )
	          << " MB" << std::endl;

#if defined(__APPLE__)
	bool fail = false;
	if( peakRSS > kPeakBudgetBytes ) {
		std::cout << "  FAIL: peak RSS exceeded peak budget" << std::endl;
		fail = true;
	}
	if( growth > kGrowthBudgetBytes ) {
		std::cout << "  FAIL: RSS growth across 1000 scrubs exceeded growth budget"
		          << " — likely per-iteration allocation leak in interactive path"
		          << std::endl;
		fail = true;
	}
	if( fail ) return 1;
	std::cout << "=== PASS ===" << std::endl;
#else
	// On non-Apple platforms task_info isn't available; SampleResidentBytes
	// returns 0.  The loop still ran to completion — no UAF, no assert —
	// which is itself a useful smoke test, so we treat that as a pass.
	std::cout << "  (RSS sampling skipped on non-Apple platforms — loop completed)"
	          << std::endl;
	std::cout << "=== PASS ===" << std::endl;
#endif

	return 0;
}
