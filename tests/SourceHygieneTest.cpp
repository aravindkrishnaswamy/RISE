//////////////////////////////////////////////////////////////////////
//
//  SourceHygieneTest.cpp - mechanical guardrail against the recurring
//    "false-green test" disease.
//
//  RISE builds with -ffast-math (-ffinite-math-only); under it the
//  compiler may assume no NaN/Inf and FOLD a NaN-sentinel comparison to
//  a constant.  A test that returns std::nan("") as a "not found"
//  sentinel and then asserts `abs(x - K) < eps` therefore silently
//  PASSES even when the lookup failed -- a false-green that hid a real
//  bug THREE times during the snapshot/transaction work (see
//  docs/skills/red-proof-and-test-integrity.md).
//
//  This test scans every other tests/*.cpp for foldable not-found
//  sentinels and FAILS the suite if any is found, so the disease can
//  never reach a second file again.  A genuinely-intentional NaN/Inf use
//  (e.g. a test that verifies the renderer's own NaN handling) opts out
//  with a `// HYGIENE-OK: <reason>` comment on the same line.
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) { passCount++; }
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

// Forbidden -ffast-math-foldable constructs.  A NaN/Inf VALUE compiled
// under -ffinite-math-only is undefined-ish: comparisons may fold, so it
// must never be used as a control-flow sentinel in a test.
static const char* kForbidden[] = {
	"std::nan(",
	"quiet_NaN(",
	"signaling_NaN(",
	"::infinity(",
};

// Locate the tests/ directory regardless of the binary's working dir
// (run_all_tests.sh runs from the repo root; ad-hoc runs may differ).
static fs::path FindTestsDir()
{
	const char* candidates[] = { "tests", "../tests", "../../tests", "../../../tests" };
	for( const char* c : candidates ) {
		fs::path p( c );
		if( fs::exists( p / "SourceHygieneTest.cpp" ) ) { return p; }
	}
	return fs::path();
}

int main()
{
	std::cout << "=== SourceHygieneTest ===" << std::endl;

	const fs::path testsDir = FindTestsDir();
	Check( !testsDir.empty(), "tests/ directory located" );
	if( testsDir.empty() ) {
		std::cout << "  (could not find tests/ from cwd; skipping scan)" << std::endl;
		std::cout << std::endl << passCount << " passed, " << failCount << " failed." << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	std::vector<std::string> offenders;
	int scanned = 0;

	for( const auto& entry : fs::directory_iterator( testsDir ) ) {
		if( !entry.is_regular_file() ) { continue; }
		const fs::path& f = entry.path();
		if( f.extension() != ".cpp" ) { continue; }
		if( f.filename() == "SourceHygieneTest.cpp" ) { continue; }   // don't scan ourselves
		++scanned;

		std::ifstream in( f );
		std::string line;
		int lineNo = 0;
		while( std::getline( in, line ) ) {
			++lineNo;
			if( line.find( "HYGIENE-OK" ) != std::string::npos ) { continue; }
			for( const char* tok : kForbidden ) {
				const size_t tokPos = line.find( tok );
				if( tokPos == std::string::npos ) { continue; }
				// Skip comments: a NaN/Inf mentioned in a // comment is fine.
				const size_t commentPos = line.find( "//" );
				if( commentPos != std::string::npos && commentPos < tokPos ) { continue; }
				// The DISEASE is a NaN/Inf RETURNED as a not-found sentinel
				// (e.g. `if( !l ) return std::nan("");`).  A NaN/Inf used as a
				// test INPUT (constructed and passed into the code under test)
				// is legitimate, so only flag a same-line `return ... <tok>`.
				const size_t retPos = line.find( "return" );
				if( retPos == std::string::npos || retPos > tokPos ) { continue; }
				offenders.push_back(
					f.filename().string() + ":" + std::to_string( lineNo )
					+ "  ->  return " + tok );
			}
		}
	}

	Check( scanned > 0, "scanned at least one test file" );

	for( const std::string& o : offenders ) {
		std::cout << "  FORBIDDEN foldable NaN/Inf sentinel: " << o << std::endl;
	}
	Check( offenders.empty(),
	       "no -ffast-math-foldable NaN/Inf sentinels in tests/ (use a finite "
	       "poison or an explicit existence Check; see docs/skills/"
	       "red-proof-and-test-integrity.md)" );

	// ---- Start-screen starter-template sync (docs/gui/START_SCREEN.md §5.1)
	// The canonical scenes/Templates/empty_starter.RISEscene is copied into
	// each GUI build's resources (Mac: build/XCode/rise/RISE-GUI/Resources/).
	// The asset headers CLAIM a test keeps the copies identical -- this is
	// that test.  A one-sided edit would otherwise silently desync what the
	// create-with-agent path actually loads from what the repo documents.
	{
		const fs::path repoRoot = testsDir.parent_path();
		const fs::path canonical =
			repoRoot / "scenes" / "Templates" / "empty_starter.RISEscene";
		const fs::path macCopy = repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "Resources" / "empty_starter.RISEscene";
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string canonicalBytes = slurp( canonical );
		Check( !canonicalBytes.empty(),
		       "canonical starter template exists (scenes/Templates/empty_starter.RISEscene)" );
		const std::string macBytes = slurp( macCopy );
		Check( !macBytes.empty(),
		       "Mac bundle copy of the starter template exists (RISE-GUI/Resources)" );
		Check( canonicalBytes == macBytes,
		       "starter-template copies are byte-identical (edit the canonical, re-copy to Resources)" );
	}

	// ---- N-up shell preset parity (docs/gui/RENDER_MODES.md §7.2) ----
	// The preset is intentionally shell-owned, but it must not drift between
	// macOS and Windows: a typo is accepted only as a failed setter and then
	// retried forever, while a one-sided valid edit silently gives the two
	// desktop apps different first-reveal behavior.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string mac = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "MultiPaneViewport.swift" );
		const std::string win = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ViewportWidget.cpp" );
		const std::string design = slurp( repoRoot / "docs" / "gui" / "RENDER_MODES.md" );
		const std::string ledger = slurp( repoRoot / "docs" / "gui" / "OPEN_ITEMS.md" );
		Check( mac.find( "[\"preview\", \"wireframe\", \"normals\", \"depth\"]" )
		       != std::string::npos,
		       "macOS N-up preset is exactly Preview/Wireframe/Normals/Depth" );
		Check( win.find( "{ \"preview\", \"wireframe\", \"normals\", \"depth\" }" )
		       != std::string::npos,
		       "Windows N-up preset is exactly Preview/Wireframe/Normals/Depth" );
		Check( design.find( "pane 1 = `wireframe`, pane 2 = `normals`, pane 3 =\n`depth`" )
		       != std::string::npos,
		       "RENDER_MODES documents the desktop preset exactly" );
		Check( ledger.find( "pane 1 = `wireframe`, pane 2 = `normals`, and (in Quad) pane 3\n  as `depth`" )
		       != std::string::npos,
		       "OPEN_ITEMS records the shipped desktop preset exactly" );
	}

	// ---- Post-load timeline reveal parity ----
	// The timeline used to be constructed/hidden from a load-time-only flag.
	// Keep both desktop shells wired to their existing live-scene polls so an
	// agent insertion of the first keyframe reveals it without a scene reload.
	// This is deliberately a source-wiring guard: the portable runtime test in
	// SceneEditorAnimationFramesTest covers the mutex-safe live snapshot, while
	// the platform GUI builds type-check the code reached by these markers.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string macModel = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "RenderViewModel.swift" );
		const std::string macChat = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ChatViewModel.swift" );
		const std::string macContent = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ContentView.swift" );
		const std::string macViewport = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "ViewportView.swift" );
		const std::string macTimeline = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "App" / "TimelineSlider.swift" );
		const std::string macBridge = slurp( repoRoot / "build" / "XCode" / "rise"
			/ "RISE-GUI" / "Bridge" / "RISEViewportBridge.mm" );
		const std::string win = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "MainWindow.cpp" );
		const std::string winHeader = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "MainWindow.h" );
		const std::string winChat = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ChatPanel.cpp" );
		const std::string winChatHeader = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ChatPanel.h" );
		const std::string winBridge = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ViewportBridge.cpp" );
		const std::string winTimeline = slurp( repoRoot / "build" / "VS2022"
			/ "RISE-GUI" / "ViewportTimeline.cpp" );
		const size_t winRangeBegin = winTimeline.find( "void ViewportTimeline::setRange" );
		const size_t winRangeEnd = winTimeline.find(
			"void ViewportTimeline::setAnimationFrameCount", winRangeBegin );
		const std::string winSetRange =
			( winRangeBegin != std::string::npos && winRangeEnd != std::string::npos )
			? winTimeline.substr( winRangeBegin, winRangeEnd - winRangeBegin )
			: std::string();
		Check( macModel.find( "RunLoop.main.add(timer, forMode: .common)" )
		       != std::string::npos
		       && macModel.find( "self.pollRefinementState(vb)" ) != std::string::npos
		       && macModel.find( "let liveAnimationPresence = vb.animationPresence" )
		       != std::string::npos
		       && macModel.find( "hasAnimation = liveHasAnimation" ) != std::string::npos
		       && macContent.find( "timelineVisible: viewModel.hasAnimation" )
		          != std::string::npos
		       && macContent.find( "timelineRange: viewModel.animationTimeStart...viewModel.animationTimeEnd" )
		          != std::string::npos
		       && macViewport.find( "range: timelineRange" ) != std::string::npos
		       && macBridge.find( "RISE_API_SceneEditController_GetHasAnimation(_controller, &hasAnimation)" )
		          != std::string::npos,
		       "post-load timeline reveal remains wired into the macOS live-scene poll" );
		Check( win.find( "connect(m_cstSyncTimer, &QTimer::timeout, this, &MainWindow::onCstSyncTick);" )
		       != std::string::npos
		       && win.find( "if (m_cstSyncTimer) m_cstSyncTimer->start();" )
		          != std::string::npos
		       && win.find( "const int animationPresence = m_viewportBridge->animationPresence();" )
		       != std::string::npos
		       && win.find( "m_viewportTimeline->setVisible(liveHasAnimation);" )
		          != std::string::npos
		       && winBridge.find( "RISE_API_SceneEditController_GetHasAnimation(m_controller, &hasAnimation)" )
		          != std::string::npos,
		       "post-load timeline reveal remains wired into the Windows live-scene poll" );
		Check( macModel.find( "if optionsChanged { stopPreviewPlay() }" )
		       != std::string::npos
		       && macModel.find( "reconcileSceneTime(to: clampedTime, using: vb)" )
		          != std::string::npos
		       && macModel.find( "if !optionsChanged || !manualTimelineScrubActive" )
		          != std::string::npos
		       && macViewport.find( "consumePreappliedSceneTime(newValue)" )
		          != std::string::npos
		       && macViewport.find( "endManualTimelineScrub(using: bridge)" )
		          != std::string::npos
		       && macTimeline.find( "onScrubMove(newTime)" ) != std::string::npos
		       && macTimeline.find( "onJump(t)" ) != std::string::npos,
		       "macOS live range changes stop stale playback and reconcile scene time" );
		const size_t macChatRenderFunction = macChat.find(
			"private func executeRenderToolCallAsync" );
		const size_t macChatRenderFinalize = macChat.find(
			"chatRenderWillSubmit()", macChatRenderFunction );
		const size_t macChatRenderSubmit = macChat.find(
			"vb.agentHandleLine(asyncLine)", macChatRenderFinalize );
		Check( macChatRenderFunction != std::string::npos
		       && macChatRenderFinalize != std::string::npos
		       && macChatRenderSubmit != std::string::npos
		       && macChatRenderFinalize < macChatRenderSubmit
		       && macModel.find( "chat.chatRenderWillSubmit = { [weak self] in" )
		          != std::string::npos
		       && macModel.find( "self.stopPreviewPlay()" ) != std::string::npos
		       && macViewport.find( "viewModel.stopPreviewPlay()" ) != std::string::npos,
		       "macOS chat render stops timeline playback before controller submission" );
		Check( winSetRange.find( "stopPlayback();" ) != std::string::npos
		       && winSetRange.find( "if (m_scrubbing)" ) != std::string::npos
		       && winSetRange.find( "m_hasPendingRange = true;" ) != std::string::npos
		       && winTimeline.find( "applyPendingRange();" ) != std::string::npos
		       && winSetRange.find( "const double clampedTime = std::clamp(canonicalTime, m_minT, m_maxT);" )
		       != std::string::npos
		       && winSetRange.find( "jumpToTime(clampedTime);" ) != std::string::npos,
		       "Windows live range changes stop stale playback and reconcile scene time" );
		Check( win.find( "finalizeOpenTimelineInteraction();" ) != std::string::npos
		       && winTimeline.find( "void ViewportTimeline::finalizeOpenTimelineInteraction()" )
		          != std::string::npos
		       && winTimeline.find( "m_slider->setSliderDown(false);" ) != std::string::npos
		       && winTimeline.find( "emit scrubEnd();" ) != std::string::npos
		       && winTimeline.find( "m_hasPendingRange = false;" ) != std::string::npos,
		       "Windows timeline removal finalizes an open manual scrub" );
		const size_t chatRenderFinalize = winChat.find( "emit chatRenderWillSubmit();" );
		const size_t chatRenderSubmit = winChat.find(
			"m_bridge->agentHandleLine(asyncLine)", chatRenderFinalize );
		Check( chatRenderFinalize != std::string::npos
		       && chatRenderSubmit != std::string::npos
		       && chatRenderFinalize < chatRenderSubmit
		       && winChatHeader.find( "void chatRenderWillSubmit();" ) != std::string::npos
		       && win.find( "&ChatPanel::chatRenderWillSubmit" ) != std::string::npos
		       && win.find( "m_viewportTimeline->finalizeOpenTimelineInteraction();" )
		          != std::string::npos,
		       "Windows chat render finalizes timeline before controller submission" );
		Check( winChat.find(
			"if (m_stopRequested || !m_bridge || !m_sceneEditableExternal)" )
		       == std::string::npos
		       && winChat.find( "if (!m_renderCancellationDraining" )
		          != std::string::npos
		       && winChat.find( "&& (m_stopRequested || !m_sceneEditableExternal)" )
		          != std::string::npos,
		       "Windows chat render polls through its own occupancy gate" );
		const size_t winCancelRenderBegin = winChat.find(
			"void ChatPanel::cancelOutstandingRender()" );
		const size_t winCancelRenderEnd = winChat.find(
			"void ChatPanel::drainPendingToolCallsAsCancelled", winCancelRenderBegin );
		const std::string winCancelRender =
			( winCancelRenderBegin != std::string::npos
			  && winCancelRenderEnd != std::string::npos )
			? winChat.substr( winCancelRenderBegin,
			                  winCancelRenderEnd - winCancelRenderBegin )
			: std::string();
		Check( winCancelRender.find( "m_renderCancellationDraining = true;" )
		       != std::string::npos
		       && winCancelRender.find( "m_renderPollTimer->start();" )
		          != std::string::npos
		       && winCancelRender.find( "setOutstandingRenderJobId(0);" )
		          != std::string::npos
		       && winCancelRender.find( "if (!m_bridge)" )
		          < winCancelRender.find( "setOutstandingRenderJobId(0);" ),
		       "Windows chat render retains occupancy through cancellation drain" );
		const size_t winDestructor = win.find( "MainWindow::~MainWindow()" );
		const size_t winDestructorEnd = win.find(
			"// ============================================================", winDestructor );
		const std::string winDestructorBody =
			( winDestructor != std::string::npos && winDestructorEnd != std::string::npos )
			? win.substr( winDestructor, winDestructorEnd - winDestructor )
			: std::string();
		Check( winDestructor != std::string::npos
		       && win.find( "MainWindow::~MainWindow()", winDestructor + 1 ) == std::string::npos
		       && winHeader.find( "~MainWindow() override;" ) != std::string::npos
		       && winHeader.find( "~MainWindow() override;",
		                          winHeader.find( "~MainWindow() override;" ) + 1 )
		          == std::string::npos
		       && winDestructorBody.find( "m_engine->cancelAndJoinInFlightWork();" )
		          != std::string::npos
		       && winDestructorBody.find( "teardownViewport();" ) != std::string::npos,
		       "Windows MainWindow has one ordered shutdown path" );
	}

	// ---- IJob vtable append-only manifest (round-4 review, 2026-07-22) ----
	// IJob is a public abstract interface: its virtual DECLARATION ORDER is
	// the vtable ABI.  The append-only convention lived only in tail comments
	// and was violated (a new virtual landed mid-vtable next to its semantic
	// sibling, shifting every later slot).  This makes the convention
	// MECHANICAL: extract the ordered virtual names from IJob.h and compare
	// against tests/IJobVtableManifest.txt.  A legal tail append = one new
	// line at the END of the manifest, same commit.  A mid-insert / reorder /
	// removal mismatches at some index and fails the suite.
	{
		const fs::path repoRoot = testsDir.parent_path();
		const fs::path header = repoRoot / "src" / "Library" / "Interfaces" / "IJob.h";
		const fs::path manifestPath = testsDir / "IJobVtableManifest.txt";

		// Extract the ordered virtual-method names from `class IJob`'s body.
		// Brace-count CODE only (strip `//` comments first -- doc text contains
		// braces); skip the destructor (a `~` before the name).
		std::vector<std::string> extracted;
		{
			std::ifstream in( header );
			std::string line;
			bool inClass = false, started = false;
			int depth = 0;
			while( std::getline( in, line ) ) {
				const size_t cpos = line.find( "//" );
				const std::string code = ( cpos == std::string::npos ) ? line : line.substr( 0, cpos );
				if( !inClass ) {
					const size_t k = code.find( "class IJob" );
					if( k != std::string::npos
					 && ( code.size() <= k + 10 || !isalnum( (unsigned char)code[k + 10] ) )
					 && code.find( ';' ) == std::string::npos ) {
						inClass = true;
						for( char c : code ) { if( c == '{' ) ++depth; else if( c == '}' ) --depth; }
						started = depth > 0;
					}
					continue;
				}
				for( char c : code ) { if( c == '{' ) ++depth; else if( c == '}' ) --depth; }
				if( !started && depth > 0 ) started = true;
				if( started && depth <= 0 ) break;
				// A declaration line: first token after stripping tabs is `virtual`.
				size_t b = code.find_first_not_of( " \t" );
				if( b == std::string::npos ) continue;
				if( code.compare( b, 8, "virtual " ) != 0 ) continue;
				const size_t paren = code.find( '(', b );
				if( paren == std::string::npos ) continue;
				if( code.rfind( '~', paren ) != std::string::npos
				 && code.rfind( '~', paren ) > b ) continue;   // destructor
				size_t e = paren;
				while( e > b && ( code[e-1] == ' ' || code[e-1] == '\t' ) ) --e;
				size_t s = e;
				while( s > b && ( isalnum( (unsigned char)code[s-1] ) || code[s-1] == '_' ) ) --s;
				if( s < e ) extracted.push_back( code.substr( s, e - s ) );
			}
		}
		Check( extracted.size() > 200,
		       "IJob.h parsed: extracted the virtual-method order (sanity: >200 methods)" );

		std::vector<std::string> manifest;
		{
			std::ifstream in( manifestPath );
			std::string line;
			while( std::getline( in, line ) ) {
				if( line.empty() || line[0] == '#' ) continue;
				manifest.push_back( line );
			}
		}
		Check( !manifest.empty(), "tests/IJobVtableManifest.txt loaded" );

		size_t firstDiff = 0;
		const size_t common = std::min( extracted.size(), manifest.size() );
		while( firstDiff < common && extracted[firstDiff] == manifest[firstDiff] ) ++firstDiff;
		if( firstDiff < common ) {
			std::cout << "  IJob VTABLE ORDER MISMATCH at slot " << firstDiff
			          << ": header has `" << extracted[firstDiff]
			          << "`, manifest has `" << manifest[firstDiff] << "`" << std::endl
			          << "  A new IJob virtual must be APPENDED at the class tail (append-only"
			          << " vtable ABI); a rename/removal is an ABI break -- see"
			          << " abi-preserving-api-evolution." << std::endl;
		}
		Check( firstDiff == common,
		       "IJob virtual order matches the manifest prefix (no mid-vtable insert/reorder)" );
		if( extracted.size() < manifest.size() ) {
			std::cout << "  IJob.h is MISSING manifest tail entries (removal = ABI break):"
			          << " first missing `" << manifest[extracted.size()] << "`" << std::endl;
		} else if( extracted.size() > manifest.size() ) {
			std::cout << "  NEW IJob tail virtual(s) not yet in the manifest -- append `"
			          << extracted[manifest.size()]
			          << "` (and any after it) to tests/IJobVtableManifest.txt in this commit."
			          << std::endl;
		}
		Check( extracted.size() == manifest.size(),
		       "IJob virtual count matches the manifest (tail appends update the manifest consciously)" );
	}

	std::cout << std::endl
	          << "(scanned " << scanned << " test files) "
	          << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
