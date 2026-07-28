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
//  It has since become the repo's home for SOURCE-GREP invariants
//  generally -- claims that live in text and cannot be checked by
//  running anything: cross-platform GUI preset parity, the IJob vtable
//  append-only manifest, the chat-render session routing, and (fix round
//  18) the two agent-facing ENUMERATION registries, which had produced a
//  stale count or a stale list in four consecutive review rounds.  The
//  rule for anything added here: derive the expected value from the CODE,
//  never restate it in this file -- a test carrying its own copy of the
//  list just moves the drift.
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <iterator>
#include <system_error>
#include <utility>
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
			"vb.agentHandleToolCall(asyncLine, autonomy: pinnedAutonomy)",
			macChatRenderFinalize );
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
			"m_bridge->agentHandleToolCall(asyncLine, pinnedAutonomy)",
			chatRenderFinalize );
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

		// ---- Chat render session routing (fix round 2, P1-B) ----
		// WHY THIS IS GUARDED IN SOURCE.  `AgentSession`'s last-render PNG
		// cache -- the pair `ReadImage()` serves and the `read_image` verb
		// returns (mLastPng / mLastSink, written at RenderCore_'s tail) --
		// is PER-SESSION.  `agentHandleLine` is the ADMINISTRATIVE session;
		// `agentHandleToolCall` is the autonomy-selected session every OTHER
		// chat tool call uses.  Both GUI drivers used to submit and poll the
		// chat `render` through `agentHandleLine`, so the agent's render
		// populated one session's cache and its follow-up `read_image` read
		// another's -- byteLength 0, or the stale objectmap left by
		// query_object_at.  A model then re-rendered for 3-9 turns trying to
		// get pixels back, and in one observed run judged a beauty render
		// from a flat segmentation image.
		//
		// The library-level AgentRenderAsyncTest "(img-cache)" case
		// characterizes WHY the caches are independent; it would still pass
		// if someone reverted this GUI routing entirely.  These markers are
		// what actually pin the routing.  Bodies are extracted by
		// find(begin_symbol)/find(next_symbol) so a marker cannot be
		// satisfied by an unrelated call elsewhere in the file.
		auto bodyBetween = []( const std::string& src, const char* begin,
		                       const char* next ) -> std::string {
			const size_t b = src.find( begin );
			if( b == std::string::npos ) return std::string();
			const size_t e = src.find( next, b );
			if( e == std::string::npos ) return std::string();
			return src.substr( b, e - b );
		};
		// Every render-lifecycle body must use the PINNED overload and must
		// contain NO `agentHandleLine` call at all.
		const std::string macRenderSubmitBody = bodyBetween( macChat,
			"private func executeRenderToolCallAsync", "private static func injectAsyncTrue" );
		Check( !macRenderSubmitBody.empty()
		       && macRenderSubmitBody.find( "vb.agentHandleToolCall(asyncLine, autonomy:" )
		          != std::string::npos
		       && macRenderSubmitBody.find( "vb.agentHandleToolCall(waitLine, autonomy:" )
		          != std::string::npos
		       && macRenderSubmitBody.find( "agentHandleLine" ) == std::string::npos,
		       "macOS chat render submit+poll run on the AUTONOMY-SELECTED session (agentHandleToolCall, pinned), "
		       "never the administrative agentHandleLine -- read_image's PNG cache is PER-SESSION, so splitting "
		       "them again makes read_image return 0 bytes or a stale objectmap" );
		const std::string macRenderDrainBody = bodyBetween( macChat,
			"private func waitForChatRenderDrain(", "private func finishChatRenderOccupancy" );
		Check( !macRenderDrainBody.empty()
		       && macRenderDrainBody.find( "vb.agentHandleToolCall(waitLine, autonomy:" )
		          != std::string::npos
		       && macRenderDrainBody.find( "agentHandleLine" ) == std::string::npos,
		       "macOS chat render DRAIN poll stays on the pinned tool-call session (same per-session cache reason)" );
		const std::string macRenderCancelBody = bodyBetween( macChat,
			"private func cancelAnyOutstandingChatRender()", "private func beginChatRenderDrain" );
		Check( !macRenderCancelBody.empty()
		       && macRenderCancelBody.find( "vb.agentHandleToolCall(line, autonomy:" )
		          != std::string::npos
		       && macRenderCancelBody.find( "agentHandleLine" ) == std::string::npos,
		       "macOS chat render CANCEL stays on the pinned tool-call session (same per-session cache reason)" );
		const std::string winRenderSubmitBody = bodyBetween( winChat,
			"void ChatPanel::startAsyncRenderToolCall", "void ChatPanel::pollOutstandingRender()" );
		Check( !winRenderSubmitBody.empty()
		       && winRenderSubmitBody.find( "m_bridge->agentHandleToolCall(asyncLine, pinnedAutonomy)" )
		          != std::string::npos
		       && winRenderSubmitBody.find( "agentHandleLine" ) == std::string::npos,
		       "Windows chat render SUBMIT runs on the autonomy-selected session (agentHandleToolCall, pinned), "
		       "never the administrative agentHandleLine -- read_image's PNG cache is PER-SESSION" );
		const std::string winRenderPollBody = bodyBetween( winChat,
			"void ChatPanel::pollOutstandingRender()", "void ChatPanel::cancelOutstandingRender()" );
		Check( !winRenderPollBody.empty()
		       && winRenderPollBody.find( "m_bridge->agentHandleToolCall(" ) != std::string::npos
		       && winRenderPollBody.find( "agentHandleLine" ) == std::string::npos,
		       "Windows chat render POLL stays on the pinned tool-call session (same per-session cache reason)" );
		Check( !winCancelRender.empty()
		       && winCancelRender.find( "m_bridge->agentHandleToolCall(" ) != std::string::npos
		       && winCancelRender.find( "agentHandleLine" ) == std::string::npos,
		       "Windows chat render CANCEL stays on the pinned tool-call session (same per-session cache reason)" );
		// INVERSE regression: the ORDINARY (non-render) tool dispatch site
		// must keep using agentHandleToolCall too -- "fix" the routing by
		// moving everything to agentHandleLine and the split reappears from
		// the other direction.  A no-agentHandleLine assertion is NOT made
		// here: processNextToolCall / driveTurn carry the historical
		// explanation of the old split in a comment, deliberately.
		const std::string winOrdinaryToolBody = bodyBetween( winChat,
			"void ChatPanel::processNextToolCall()", "void ChatPanel::startAsyncRenderToolCall" );
		Check( !winOrdinaryToolBody.empty()
		       && winOrdinaryToolBody.find( "m_bridge->agentHandleToolCall(toQString(line))" )
		          != std::string::npos,
		       "Windows ORDINARY tool dispatch still uses agentHandleToolCall -- render and read_image must share "
		       "ONE session, in both directions" );
		Check( macChat.find( "responseLine = vb.agentHandleToolCall(line)" ) != std::string::npos,
		       "macOS ORDINARY tool dispatch still uses agentHandleToolCall -- render and read_image must share "
		       "ONE session, in both directions" );
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

	// ---- Agent verb-set enumeration parity (fix round 18) --------------
	// WHY THIS EXISTS.  Nine review rounds on fix/gui-agent-chattiness; the
	// executable code has been clean since round 2 and EVERY finding for the
	// last five rounds was a doc/claim mismatch -- four of them COUNT or
	// ENUMERATION drift in the agent-facing comment family.  Round 13
	// declared this exact sweep done, fixed two instances, and missed the
	// one attached to IsReadSafeVerb itself; round 18 found that one plus
	// six siblings ("the 3 mutating verbs" where there are five, a
	// blind-edit mutation set missing its batch forms).  Hand-fixing the
	// cited number each round is what makes it recur, so the class is
	// mechanized here instead.
	//
	// GROUND TRUTH IS THE CODE, NOT A LIST IN THIS FILE.  The verb sets are
	// parsed out of AgentRpc.cpp's IsReadSafeVerb / IsProposeSafeVerb
	// bodies -- the two functions the dispatcher's autonomy choke point
	// actually calls.  A hardcoded copy here would just relocate the drift.
	//
	// WHAT IS ENFORCED (deliberately narrow, so unrelated prose edits do not
	// trip it -- an empirical sweep of src/Library/Agent showed a blanket
	// "any list of 3+ verb names" rule false-positives on the many LEGITIMATE
	// subsets, e.g. "the 3 single-item verbs", "read_document/read_schema/
	// read_skill/validate"):
	//   (A) A CLAIM of the form "<N> [<prefix>-]mutating verb(s)/tool(s)":
	//       N must equal the size of IsProposeSafeVerb's set, and when the
	//       claim is immediately followed by a parenthesised list of verb
	//       names, that list must BE that set.
	//   (B) A list introduced by "read-safe allowlist (" or by
	//       "IsReadSafeVerb -- " must be exactly IsReadSafeVerb's set.
	// Anything not carrying one of those anchors is left alone: an author
	// naming a subset in prose is not making a set claim.  The corollary --
	// stated here because it is the maintenance rule -- is that a NEW remote
	// restatement should use one of these anchor phrasings so it is covered.
	{
		const fs::path repoRoot = testsDir.parent_path();
		const fs::path agentDir = repoRoot / "src" / "Library" / "Agent";
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const std::string rpcCpp = slurp( agentDir / "AgentRpc.cpp" );
		Check( !rpcCpp.empty(), "verb-parity: AgentRpc.cpp read" );

		// The ordered string literals inside a `bool <fn>( ... )` body.
		auto verbsIn = [&]( const char* fn ) -> std::vector<std::string> {
			std::vector<std::string> v;
			const std::string sig = std::string( "bool " ) + fn + "( const std::string& method )";
			const size_t s = rpcCpp.find( sig );
			if( s == std::string::npos ) { return v; }
			const size_t open = rpcCpp.find( '{', s );
			if( open == std::string::npos ) { return v; }
			int depth = 0; size_t end = open;
			for( size_t i = open; i < rpcCpp.size(); ++i ) {
				if( rpcCpp[i] == '{' ) { ++depth; }
				else if( rpcCpp[i] == '}' ) { if( --depth == 0 ) { end = i; break; } }
			}
			for( size_t i = open; i < end; ++i ) {
				if( rpcCpp[i] != '"' ) { continue; }
				const size_t q = rpcCpp.find( '"', i + 1 );
				if( q == std::string::npos ) { break; }
				v.push_back( rpcCpp.substr( i + 1, q - i - 1 ) );
				i = q;
			}
			return v;
		};
		const std::vector<std::string> readSafe = verbsIn( "IsReadSafeVerb" );
		const std::vector<std::string> proposeSafe = verbsIn( "IsProposeSafeVerb" );
		Check( readSafe.size() >= 5,
		       "verb-parity: parsed IsReadSafeVerb's body (sanity: >=5 verbs; got "
		       + std::to_string( readSafe.size() ) + ")" );
		Check( proposeSafe.size() >= 3,
		       "verb-parity: parsed IsProposeSafeVerb's body (sanity: >=3 verbs; got "
		       + std::to_string( proposeSafe.size() ) + ")" );

		std::vector<std::string> readSorted = readSafe, proposeSorted = proposeSafe;
		std::sort( readSorted.begin(), readSorted.end() );
		std::sort( proposeSorted.begin(), proposeSorted.end() );
		{
			std::vector<std::string> both;
			std::set_intersection( readSorted.begin(), readSorted.end(),
			                       proposeSorted.begin(), proposeSorted.end(),
			                       std::back_inserter( both ) );
			Check( both.empty(),
			       "verb-parity: the read-safe and mutating sets are DISJOINT (a verb in "
			       "both would make the Read-posture refusal meaningless)" );
		}
		std::vector<std::string> known = readSorted;
		known.insert( known.end(), proposeSorted.begin(), proposeSorted.end() );
		std::sort( known.begin(), known.end() );
		auto isKnownVerb = [&]( const std::string& t ) {
			return std::binary_search( known.begin(), known.end(), t );
		};
		auto join = []( const std::vector<std::string>& v ) {
			std::string s;
			for( size_t i = 0; i < v.size(); ++i ) { if( i ) s += "/"; s += v[i]; }
			return s;
		};

		// Contiguous comment text, so a list wrapped across `//` lines joins
		// into one string.  A `//` that follows an odd number of quotes on the
		// line is inside a string literal, not a comment.  `starts` records the
		// offset in `text` at which each source line begins, so a finding can
		// name the EXACT line rather than the (often far-away) block start --
		// AgentRpc.h's file header is one 600-line block.
		struct Block {
			std::string text;
			std::vector<std::pair<size_t,int>> starts;   // (offset in text, source line)
			int LineAt( size_t off ) const {
				int best = starts.empty() ? 0 : starts.front().second;
				for( const auto& s : starts ) { if( s.first <= off ) { best = s.second; } else { break; } }
				return best;
			}
		};
		auto commentBlocks = []( const std::string& src ) {
			std::vector<Block> out;
			Block cur; int lineNo = 0;
			size_t pos = 0;
			while( pos <= src.size() ) {
				const size_t nl = src.find( '\n', pos );
				const std::string line = src.substr( pos, ( nl == std::string::npos ? src.size() : nl ) - pos );
				++lineNo;
				size_t c = std::string::npos;
				int quotes = 0;
				for( size_t i = 0; i + 1 < line.size(); ++i ) {
					if( line[i] == '"' && ( i == 0 || line[i-1] != '\\' ) ) { ++quotes; }
					if( line[i] == '/' && line[i+1] == '/' && ( quotes % 2 ) == 0 ) { c = i; break; }
				}
				if( c != std::string::npos ) {
					size_t b = c + 2;
					if( b < line.size() && line[b] == '!' ) { ++b; }
					if( !cur.text.empty() ) { cur.text += " "; }
					cur.starts.push_back( std::make_pair( cur.text.size(), lineNo ) );
					cur.text += line.substr( b );
				} else if( !cur.text.empty() ) {
					out.push_back( cur ); cur = Block();
				}
				if( nl == std::string::npos ) { break; }
				pos = nl + 1;
			}
			if( !cur.text.empty() ) { out.push_back( cur ); }
			return out;
		};

		auto lower = []( std::string s ) {
			for( char& ch : s ) { ch = (char)tolower( (unsigned char)ch ); }
			return s;
		};
		auto isVerbChar = []( char ch ) {
			return islower( (unsigned char)ch ) || isdigit( (unsigned char)ch ) || ch == '_';
		};
		// Parse the maximal `/`- or `,`-separated run of KNOWN verb names
		// starting at `i` (skipping leading space and one `(`).  Empty when
		// the text there is not a verb list at all -- e.g. "(IsProposeSafeVerb)"
		// or "(the 3 single-item verbs ...", both of which must be left alone.
		auto parseRun = [&]( const std::string& b, size_t i, bool requireParen ) {
			std::vector<std::string> run;
			while( i < b.size() && isspace( (unsigned char)b[i] ) ) { ++i; }
			if( i < b.size() && b[i] == '(' ) { ++i; }
			else if( requireParen ) { return run; }
			for( ;; ) {
				while( i < b.size() && isspace( (unsigned char)b[i] ) ) { ++i; }
				size_t e = i;
				while( e < b.size() && isVerbChar( b[e] ) ) { ++e; }
				const std::string tok = b.substr( i, e - i );
				if( tok.empty() || !isKnownVerb( tok ) ) { break; }
				run.push_back( tok );
				i = e;
				while( i < b.size() && isspace( (unsigned char)b[i] ) ) { ++i; }
				if( i < b.size() && ( b[i] == '/' || b[i] == ',' ) ) { ++i; continue; }
				break;
			}
			return run;
		};
		auto setOf = []( std::vector<std::string> v ) {
			std::sort( v.begin(), v.end() );
			v.erase( std::unique( v.begin(), v.end() ), v.end() );
			return v;
		};

		std::vector<std::string> verbProblems;
		int countedMutatingClaims = 0, mutatingRunsChecked = 0, readSafeRunsChecked = 0;
		std::vector<fs::path> agentFiles;
		for( const auto& e : fs::directory_iterator( agentDir ) ) {
			if( !e.is_regular_file() ) { continue; }
			const std::string ext = e.path().extension().string();
			if( ext == ".cpp" || ext == ".h" ) { agentFiles.push_back( e.path() ); }
		}
		std::sort( agentFiles.begin(), agentFiles.end() );
		Check( agentFiles.size() >= 5,
		       "verb-parity: found the src/Library/Agent sources to scan (got "
		       + std::to_string( agentFiles.size() ) + ")" );

		for( const fs::path& f : agentFiles ) {
			const std::string name = f.filename().string();
			for( const Block& blk : commentBlocks( slurp( f ) ) ) {
				const std::string& b = blk.text;
				const std::string lb = lower( b );
				auto where = [&]( size_t off ) {
					return name + ":" + std::to_string( blk.LineAt( off ) );
				};

				// (A) "<N> [<prefix>-]mutating verb(s)/tool(s)"
				for( size_t p = lb.find( "mutating" ); p != std::string::npos;
				     p = lb.find( "mutating", p + 1 ) ) {
					if( p > 0 && ( isalpha( (unsigned char)lb[p-1] ) || lb[p-1] == '_' ) ) { continue; }
					// right side: whitespace then verb/tool
					size_t r = p + 8;
					while( r < lb.size() && isspace( (unsigned char)lb[r] ) ) { ++r; }
					const bool isVerbWord = lb.compare( r, 4, "verb" ) == 0;
					const bool isToolWord = lb.compare( r, 4, "tool" ) == 0;
					if( !isVerbWord && !isToolWord ) { continue; }
					size_t afterWord = r + 4;
					if( afterWord < lb.size() && lb[afterWord] == 's' ) { ++afterWord; }
					// left side: optional hyphenated prefix word, then a count.
					size_t l = p;
					while( l > 0 && isspace( (unsigned char)lb[l-1] ) ) { --l; }
					if( l > 0 && lb[l-1] == '-' ) {
						--l;
						while( l > 0 && isalpha( (unsigned char)lb[l-1] ) ) { --l; }
						while( l > 0 && isspace( (unsigned char)lb[l-1] ) ) { --l; }
					}
					size_t numEnd = l, numBeg = l;
					while( numBeg > 0 && isalnum( (unsigned char)lb[numBeg-1] ) ) { --numBeg; }
					const std::string numTok = lb.substr( numBeg, numEnd - numBeg );
					static const char* kWords[] = { "zero","one","two","three","four","five",
					                                "six","seven","eight","nine","ten" };
					long stated = -1;
					if( !numTok.empty() && isdigit( (unsigned char)numTok[0] )
					    && numTok.find_first_not_of( "0123456789" ) == std::string::npos ) {
						stated = std::stol( numTok );
					} else {
						for( long w = 0; w < 11; ++w ) { if( numTok == kWords[w] ) { stated = w; break; } }
					}
					if( stated < 0 ) { continue; }   // not a counted claim
					++countedMutatingClaims;
					if( (size_t)stated != proposeSafe.size() ) {
						verbProblems.push_back(
							where( numBeg ) + ": claims \"" + numTok + " mutating "
							+ ( isVerbWord ? "verb" : "tool" ) + "s\" but IsProposeSafeVerb has "
							+ std::to_string( proposeSafe.size() ) + " (" + join( proposeSorted ) + ")" );
					}
					const std::vector<std::string> run = parseRun( b, afterWord, true );
					if( run.empty() ) { continue; }   // "(IsProposeSafeVerb)", "(the 3 ...)"
					++mutatingRunsChecked;
					if( setOf( run ) != proposeSorted ) {
						verbProblems.push_back(
							where( afterWord ) + ": the mutating-verb list \"" + join( run )
							+ "\" is not IsProposeSafeVerb's set (" + join( proposeSorted ) + ")" );
					}
				}

				// (B) "read-safe allowlist (" / "IsReadSafeVerb -- "
				const char* anchors[] = { "read-safe allowlist", "isreadsafeverb --" };
				for( int a = 0; a < 2; ++a ) {
					const std::string anchor = anchors[a];
					for( size_t p = lb.find( anchor ); p != std::string::npos;
					     p = lb.find( anchor, p + 1 ) ) {
						const std::vector<std::string> run =
							parseRun( b, p + anchor.size(), false );
						if( run.empty() ) { continue; }
						++readSafeRunsChecked;
						if( setOf( run ) != readSorted ) {
							verbProblems.push_back(
								where( p ) + ": the read-safe allowlist \"" + join( run )
								+ "\" is not IsReadSafeVerb's set (" + join( readSorted ) + ")" );
						}
					}
				}
			}
		}

		for( const std::string& p : verbProblems ) {
			std::cout << "  AGENT VERB-SET DRIFT: " << p << std::endl;
		}
		if( !verbProblems.empty() ) {
			std::cout << "  Fix the COMMENT to match the code (src/Library/Agent/AgentRpc.cpp's "
			          << "IsReadSafeVerb / IsProposeSafeVerb bodies are the source of truth); "
			          << "if the CODE is what changed, the comment still has to follow."
			          << std::endl;
		}
		Check( verbProblems.empty(),
		       "agent verb-set enumerations in src/Library/Agent match IsReadSafeVerb / "
		       "IsProposeSafeVerb" );
		// Liveness: the guard must actually be finding anchored claims.  Not a
		// fixed expected count (that would be the very defect being fixed) --
		// just proof that rewording every anchor away cannot silently disable
		// the check.
		Check( countedMutatingClaims > 0 && mutatingRunsChecked > 0 && readSafeRunsChecked > 0,
		       "agent verb-set guard is LIVE (counted-mutating claims: "
		       + std::to_string( countedMutatingClaims ) + ", mutating lists: "
		       + std::to_string( mutatingRunsChecked ) + ", read-safe lists: "
		       + std::to_string( readSafeRunsChecked )
		       + ") -- a zero here means the anchor phrasings were reworded away" );
	}

	// ---- read_viewport reason-code surface registry (fix round 18) -----
	// AgentSession.h's read_viewport authority block carries a hand-written
	// "every surface that enumerates the reason codes" list, and used to
	// carry hand-written counts of it ("EIGHT files, TEN places").  Both the
	// list and the counts went stale -- the counts twice.  The counts are now
	// deleted (a list is its own count) and the LIST is checked here, BOTH
	// WAYS:
	//   * every registered surface must still enumerate every reason value
	//     (catches a surface falling behind when a reason is added), and
	//   * no unregistered file may enumerate them all (catches a NEW surface
	//     being created without registration -- the failure the block exists
	//     to prevent, and the half a human reviewer never catches).
	// Ground truth is AgentSession.cpp's `outReason = "..."` assignments --
	// the code that actually produces the wire values.  This test names no
	// reason value of its own, so it cannot itself go stale.
	{
		const fs::path repoRoot = testsDir.parent_path();
		auto slurp = []( const fs::path& f ) -> std::string {
			std::ifstream in( f, std::ios::binary );
			return std::string( std::istreambuf_iterator<char>( in ),
			                    std::istreambuf_iterator<char>() );
		};
		const fs::path definer = repoRoot / "src" / "Library" / "Agent" / "AgentSession.cpp";
		const std::string definerSrc = slurp( definer );

		std::vector<std::string> reasons;
		{
			const std::string key = "outReason = \"";
			for( size_t p = definerSrc.find( key ); p != std::string::npos;
			     p = definerSrc.find( key, p + 1 ) ) {
				const size_t b = p + key.size();
				const size_t e = definerSrc.find( '"', b );
				if( e == std::string::npos ) { break; }
				reasons.push_back( definerSrc.substr( b, e - b ) );
			}
			std::sort( reasons.begin(), reasons.end() );
			reasons.erase( std::unique( reasons.begin(), reasons.end() ), reasons.end() );
		}
		Check( reasons.size() >= 3,
		       "reason-registry: parsed AgentSession.cpp's read_viewport reason values "
		       "(sanity: >=3; got " + std::to_string( reasons.size() ) + ")" );

		// WHOLE-TOKEN containment.  Not `"<name>"`: the model-facing surfaces
		// spell the values inside C++ string literals, where the closing
		// delimiter is an ESCAPED quote (`..._progress\"`), and the markdown
		// ones use backticks or bare prose.  Requiring a non-identifier
		// character on both sides matches all of those while still refusing a
		// longer identifier that merely CONTAINS a reason name.
		auto containsToken = []( const std::string& body, const std::string& tok ) {
			for( size_t p = body.find( tok ); p != std::string::npos; p = body.find( tok, p + 1 ) ) {
				const char before = ( p == 0 ) ? ' ' : body[p - 1];
				const size_t after = p + tok.size();
				const char nxt = ( after >= body.size() ) ? ' ' : body[after];
				auto ident = []( char ch ) {
					return isalnum( (unsigned char)ch ) || ch == '_';
				};
				if( !ident( before ) && !ident( nxt ) ) { return true; }
			}
			return false;
		};

		// The registry: repo-relative paths inside the delimited block.
		std::vector<std::string> registry;
		{
			const std::string sessionH = slurp( repoRoot / "src" / "Library" / "Agent" / "AgentSession.h" );
			const size_t b = sessionH.find( "[read_viewport-reason-surfaces]" );
			const size_t e = sessionH.find( "[/read_viewport-reason-surfaces]" );
			Check( b != std::string::npos && e != std::string::npos && b < e,
			       "reason-registry: AgentSession.h carries the delimited "
			       "[read_viewport-reason-surfaces] block (do not remove the markers -- "
			       "they are what makes the surface list checkable)" );
			if( b != std::string::npos && e != std::string::npos && b < e ) {
				const std::string block = sessionH.substr( b, e - b );
				static const char* roots[] = { "src/", "skills/", "docs/", "tests/" };
				for( const char* root : roots ) {
					for( size_t p = block.find( root ); p != std::string::npos;
					     p = block.find( root, p + 1 ) ) {
						size_t q = p;
						while( q < block.size()
						       && ( isalnum( (unsigned char)block[q] ) || block[q] == '/'
						            || block[q] == '.' || block[q] == '_' || block[q] == '-' ) ) { ++q; }
						std::string path = block.substr( p, q - p );
						while( !path.empty() && path.back() == '.' ) { path.pop_back(); }
						const size_t dot = path.rfind( '.' );
						if( dot == std::string::npos ) { continue; }
						const std::string ext = path.substr( dot );
						if( ext != ".cpp" && ext != ".h" && ext != ".md" ) { continue; }
						registry.push_back( path );
					}
				}
				std::sort( registry.begin(), registry.end() );
				registry.erase( std::unique( registry.begin(), registry.end() ), registry.end() );
			}
		}
		Check( registry.size() >= 3,
		       "reason-registry: extracted the registered surface paths (sanity: >=3; got "
		       + std::to_string( registry.size() ) + ")" );

		// Which tracked text files actually enumerate EVERY reason value?
		// Scope: the source/doc/skill/test trees a reader-facing enumeration
		// could plausibly live in.  Extensions are limited to the text kinds
		// this family uses; widening them is safe but slower.
		std::vector<std::string> enumerating;
		{
			static const char* scanRoots[] = { "src", "skills", "docs", "tests", "build" };
			for( const char* root : scanRoots ) {
				const fs::path base = repoRoot / root;
				if( !fs::exists( base ) ) { continue; }
				std::error_code ec;
				fs::recursive_directory_iterator it( base, ec ), end;
				for( ; it != end; it.increment( ec ) ) {
					if( ec ) { break; }
					if( it->is_directory() ) {
						const std::string d = it->path().filename().string();
						if( d == ".git" || d == "_out" || d == "DerivedData"
						    || d == "node_modules" || d == "build" ) { it.disable_recursion_pending(); }
						continue;
					}
					if( !it->is_regular_file() ) { continue; }
					const std::string ext = it->path().extension().string();
					if( ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".mm"
					    && ext != ".md" && ext != ".swift" ) { continue; }
					const std::string body = slurp( it->path() );
					bool all = true;
					for( const std::string& r : reasons ) {
						if( !containsToken( body, r ) ) { all = false; break; }
					}
					if( !all ) { continue; }
					enumerating.push_back( fs::relative( it->path(), repoRoot ).generic_string() );
				}
			}
			std::sort( enumerating.begin(), enumerating.end() );
		}
		Check( !enumerating.empty(), "reason-registry: found the enumerating surfaces to check" );

		// (i) every registered surface still enumerates every reason.
		std::vector<std::string> missing;
		for( const std::string& r : registry ) {
			if( !fs::exists( repoRoot / r ) ) { missing.push_back( r + " (registered path does not exist)" ); continue; }
			if( !std::binary_search( enumerating.begin(), enumerating.end(), r ) ) {
				const std::string body = slurp( repoRoot / r );
				std::string absent;
				for( const std::string& v : reasons ) {
					if( !containsToken( body, v ) ) {
						if( !absent.empty() ) absent += ", ";
						absent += v;
					}
				}
				missing.push_back( r + " (missing: " + absent + ")" );
			}
		}
		for( const std::string& m : missing ) {
			std::cout << "  READ_VIEWPORT REASON SURFACE INCOMPLETE: " << m << std::endl;
		}
		if( !missing.empty() ) {
			std::cout << "  A surface registered in AgentSession.h's "
			          << "[read_viewport-reason-surfaces] block must enumerate EVERY reason "
			          << "value AgentSession.cpp can emit.  Add the missing value(s) there "
			          << "(model-facing surfaces must also state retriability and the action)."
			          << std::endl;
		}
		Check( missing.empty(),
		       "every registered read_viewport reason surface enumerates all reason values" );

		// (ii) nothing else enumerates them without being registered.
		std::vector<std::string> unregistered;
		const std::string definerRel = fs::relative( definer, repoRoot ).generic_string();
		for( const std::string& e : enumerating ) {
			if( e == definerRel ) { continue; }   // the file that DEFINES the values
			if( std::find( registry.begin(), registry.end(), e ) == registry.end() ) {
				unregistered.push_back( e );
			}
		}
		for( const std::string& u : unregistered ) {
			std::cout << "  UNREGISTERED READ_VIEWPORT REASON SURFACE: " << u << std::endl;
		}
		if( !unregistered.empty() ) {
			std::cout << "  This file enumerates every read_viewport reason value but is not "
			          << "listed in AgentSession.h's [read_viewport-reason-surfaces] block.  "
			          << "Register it (with its repo-relative path) so the next reason added "
			          << "cannot silently leave it behind -- that is exactly what the block "
			          << "exists to prevent." << std::endl;
		}
		Check( unregistered.empty(),
		       "no unregistered file enumerates the read_viewport reason values" );
	}

	std::cout << std::endl
	          << "(scanned " << scanned << " test files) "
	          << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
