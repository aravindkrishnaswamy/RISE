//////////////////////////////////////////////////////////////////////
//
//  ViewportPaneConfigTest.cpp - N-up multi-viewport pane model, P3a
//    slice 1 (docs/gui/RENDER_MODES.md §7.2 "Pane model" / §7.4 "C-ABI"):
//    the CONFIG LAYER ONLY for SceneEditController's four always-present
//    pane slots.  Slice 1 never renders -- there is no scheduler yet
//    (that is a later slice, §7.3) -- so every test here runs in
//    SKELETON mode (SceneEditController ctrl(*pJob, nullptr)) and only
//    exercises the config CRUD: PaneCountForLayout, {Set,Get}ViewportLayout,
//    {Set,Get}PrimaryPane, {Set,Get}PaneRenderMode, SetPaneVantage{Scene
//    Camera,NamedView}/GetPaneVantage, plus the matching
//    RISE_API_SceneEditController_* C-ABI wrappers.
//
//    ALIAS CONTRACT (the load-bearing design fact this file guards):
//    pane 0 is NOT independent state -- it is an alias view of the
//    EXISTING single-viewport members.  SetPaneRenderMode(0, name)
//    forwards to SetViewportRenderMode(name) verbatim (same return value,
//    same refusal reasons, including "no interactive rasterizer" in
//    skeleton mode); GetPaneRenderMode(0) forwards to
//    GetViewportRenderMode(); SetPaneVantageSceneCamera(0) forwards to
//    ExitFreeFly() (with a no-op-true postcondition when free-fly isn't
//    active); SetPaneVantageNamedView(0, name) forwards to
//    SetViewportPose(); GetPaneVantage(0, ...) reports FreeFly/SceneCamera
//    from IsFreeFlyActive().  Panes 1-3 hold real, independently-stored
//    PaneConfig entries that persist across layout hide/reshow (§7.2:
//    "toggling 2x2 -> 1 -> 2x2 must not lose a setup") and refuse
//    mutation while hidden (§7.4: "unknown pane / hidden pane / render
//    owns scene => false, nothing mutated") but remain READABLE while
//    hidden (the header doc: "getters work on any valid index").
//
//    P1  Defaults: layout Single, primary 0, panes 1-3 default to
//        {mode: preview, vantage: SceneCamera, name: ""}, pane 0 also
//        reads SceneCamera (no free-fly entered yet), and
//        PaneCountForLayout's fixed table (1/2/3/4).
//    P2  Layout switching: Single -> Quad, no-op re-set, an out-of-range
//        int cast via the C-ABI (7, -1) refused with the layout left
//        unchanged.
//    P3  Primary-pane invariants: SetPrimaryPane succeeds on a visible
//        pane, a layout shrink that hides the current primary falls back
//        to pane 0 (§7.2's MONEY assertion), a hidden pane is refused,
//        and pane 4 is refused in EVERY layout (kViewportPaneCount == 4,
//        valid indices are 0-3).
//    P4  Pane mode CRUD on panes 1-3 (Quad): a valid registry name sets
//        and reads back; an unknown name and the non-viewport-selectable
//        "objectmap" are both refused without mutating the stored mode;
//        a hidden-pane SET is refused (§7.4) but the hidden-pane GET
//        still returns the last-stored value; and a set survives a full
//        hide (shrink to Single) / reshow (grow back to Quad) cycle.
//    P5  Pane 0 aliasing: GetPaneRenderMode(0) == GetViewportRenderMode();
//        SetPaneRenderMode(0, ...) forwards SetViewportRenderMode's
//        skeleton-mode refusal for both a registry-valid name and an
//        unknown one (skeleton mode refuses on "no interactive
//        rasterizer" BEFORE it even looks at the name).
//    P6  Vantage CRUD: SetPaneVantageNamedView refuses a nonexistent
//        view; CaptureNamedView works in skeleton mode (it only reads
//        the loaded scene's active camera, never the interactive
//        rasterizer) so the positive named-view path is exercised too;
//        SetPaneVantageNamedView(1, existing) -> NamedView kind + the
//        name; SetPaneVantageSceneCamera(1) reverts to SceneCamera with
//        the name cleared; pane 0's SetPaneVantageSceneCamera is a
//        no-op-true when free-fly was never entered.
//    P7  C-ABI: null-controller refusal on every setter plus the two
//        getters' fail-closed defaults ("preview" / false);
//        RISE_API_SceneEditController_GetPaneVantage's null-out and
//        cap==0 refusals on a LIVE controller; small-cap truncation
//        (cap=3 on an 8-character view name yields 2 chars + NUL, per
//        RISE_API.cpp's `n + 1 < cap` copy loop); and a full
//        layout/primary/mode round-trip through the C wrappers.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;

namespace
{
	int g_pass = 0, g_fail = 0;
	void Check( bool c, const std::string& what )
	{
		if( c ) { ++g_pass; std::printf( "  ok  : %s\n", what.c_str() ); }
		else    { ++g_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	std::string TempPath( const char* name )
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		return dir + name;
	}

	Job* LoadScene( const std::string& text, const std::string& path )
	{
		{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
		Job* pJob = new Job();
		if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) )
		{
			pJob->release();
			std::remove( path.c_str() );
			return nullptr;
		}
		return pJob;
	}

	// Minimal renderable scene (never actually rendered here -- slice 1 is
	// config-only) -- same shape as ViewportRenderModeTest.cpp's kPlainScene.
	const char* const kPlainScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 32\nheight 24\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname white\ncolor 0.8 0.8 0.8\n}\n\n"
		"lambertian_material\n{\nname m\nreflectance white\n}\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry s\nmaterial m\n}\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	//------------------------------------------------------------------
	// P1: defaults.
	//------------------------------------------------------------------
	void TestDefaults()
	{
		std::printf( "P1: defaults (layout/primary/pane config/PaneCountForLayout)...\n" );
		const std::string tmp = TempPath( "pane_defaults.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );   // skeleton: slice 1 never renders
		using Layout = SceneEditController::ViewportLayout;
		using Kind   = SceneEditController::PaneVantageKind;

		Check( ctrl.GetViewportLayout() == Layout::Single, "default layout is Single" );
		Check( ctrl.GetPrimaryPane() == 0, "default primary pane is 0" );

		for( unsigned int pane = 1; pane <= 3; ++pane )
		{
			char label[96];
			std::snprintf( label, sizeof( label ), "GetPaneRenderMode(%u) defaults to \"preview\"", pane );
			Check( std::string( ctrl.GetPaneRenderMode( pane ) ) == "preview", label );

			Kind kind = Kind::FreeFly;      // dirty seed -- a no-op bug would be visible
			String namedView( "dirty" );
			std::snprintf( label, sizeof( label ), "GetPaneVantage(%u) succeeds on an unhidden default pane", pane );
			Check( ctrl.GetPaneVantage( pane, kind, namedView ), label );
			std::snprintf( label, sizeof( label ), "pane %u default vantage kind is SceneCamera", pane );
			Check( kind == Kind::SceneCamera, label );
			std::snprintf( label, sizeof( label ), "pane %u default named-view name is empty", pane );
			Check( namedView == "", label );
		}

		// Pane 0 (alias view): no free-fly entered yet, so it also reads
		// as SceneCamera -- same default as panes 1-3, via a different
		// code path (IsFreeFlyActive() rather than stored PaneConfig).
		Kind kind0 = Kind::FreeFly;
		String namedView0;
		Check( ctrl.GetPaneVantage( 0, kind0, namedView0 ), "GetPaneVantage(0) succeeds" );
		Check( kind0 == Kind::SceneCamera, "pane 0 default vantage kind is SceneCamera (no free-fly active yet)" );
		Check( namedView0 == "", "pane 0 default named-view name is empty" );

		Check( SceneEditController::PaneCountForLayout( Layout::Single )     == 1, "PaneCountForLayout(Single) == 1" );
		Check( SceneEditController::PaneCountForLayout( Layout::TwoH )       == 2, "PaneCountForLayout(TwoH) == 2" );
		Check( SceneEditController::PaneCountForLayout( Layout::OnePlusTwo ) == 3, "PaneCountForLayout(OnePlusTwo) == 3" );
		Check( SceneEditController::PaneCountForLayout( Layout::Quad )       == 4, "PaneCountForLayout(Quad) == 4" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// P2: layout switching.
	//------------------------------------------------------------------
	void TestLayoutSwitching()
	{
		std::printf( "P2: layout switching (Single->Quad, no-op re-set, invalid int via C-ABI)...\n" );
		const std::string tmp = TempPath( "pane_layout_switch.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		using Layout = SceneEditController::ViewportLayout;

		Check( ctrl.GetViewportLayout() == Layout::Single, "starts at Single" );
		Check( ctrl.SetViewportLayout( Layout::Quad ), "SetViewportLayout(Quad) succeeds from Single" );
		Check( ctrl.GetViewportLayout() == Layout::Quad, "GetViewportLayout reports Quad" );

		Check( ctrl.SetViewportLayout( Layout::Quad ), "re-setting the SAME layout (Quad) is a no-op success" );
		Check( ctrl.GetViewportLayout() == Layout::Quad, "still Quad after the no-op re-set" );

		// Out-of-range int cast via the C-ABI: SetViewportLayout's switch
		// has no matching case for 7 / -1, so it hits the fail-closed
		// default -- false, nothing mutated (the controller method itself
		// is unreachable with an invalid enumerator from C++, so this is
		// the ONLY way to exercise that default branch).
		Check( !RISE_API_SceneEditController_SetViewportLayout( &ctrl, 7 ), "C-ABI SetViewportLayout(7) refused (out-of-range int)" );
		Check( ctrl.GetViewportLayout() == Layout::Quad, "layout unchanged (still Quad) after the refused 7" );
		Check( !RISE_API_SceneEditController_SetViewportLayout( &ctrl, -1 ), "C-ABI SetViewportLayout(-1) refused (out-of-range int)" );
		Check( ctrl.GetViewportLayout() == Layout::Quad, "layout unchanged (still Quad) after the refused -1" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// P3: primary-pane invariants.
	//------------------------------------------------------------------
	void TestPrimaryInvariants()
	{
		std::printf( "P3: primary-pane invariants (shrink fallback, hidden refusal, pane 4 always invalid)...\n" );
		const std::string tmp = TempPath( "pane_primary.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		using Layout = SceneEditController::ViewportLayout;

		Check( ctrl.GetPrimaryPane() == 0, "primary starts at pane 0" );
		Check( ctrl.SetViewportLayout( Layout::Quad ), "switch to Quad" );
		Check( ctrl.SetPrimaryPane( 3 ), "SetPrimaryPane(3) succeeds in Quad (pane 3 visible)" );
		Check( ctrl.GetPrimaryPane() == 3, "GetPrimaryPane reports 3" );

		// Shrink Quad -> Single hides pane 3, the current primary; §7.2
		// says a shrink that hides the primary falls back to pane 0.
		Check( ctrl.SetViewportLayout( Layout::Single ), "shrink to Single (hides the current primary, pane 3)" );
		Check( ctrl.GetPrimaryPane() == 0, "MONEY ASSERTION (§7.2): primary falls back to pane 0 when the shrink hides it" );

		// Pane 1 is hidden in Single (only pane 0 is visible).
		Check( !ctrl.SetPrimaryPane( 1 ), "SetPrimaryPane(1) refused in Single (pane 1 hidden)" );
		Check( ctrl.GetPrimaryPane() == 0, "primary unchanged (still 0) after the refused set" );

		// Pane 4 is out of range for EVERY layout, including Quad (max
		// valid index is 3; kViewportPaneCount == 4).
		Check( !ctrl.SetPrimaryPane( 4 ), "SetPrimaryPane(4) refused in Single (out of range)" );
		Check( ctrl.SetViewportLayout( Layout::Quad ), "switch back to Quad (all 4 panes visible)" );
		Check( !ctrl.SetPrimaryPane( 4 ), "SetPrimaryPane(4) ALSO refused in Quad -- pane 4 doesn't exist in any layout" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// P4: pane mode CRUD on panes 1-3, hidden-pane refusal/readability,
	// and config survival across a hide/reshow cycle.
	//------------------------------------------------------------------
	void TestPaneModeCrud()
	{
		std::printf( "P4: pane mode CRUD (panes 1-3) + hidden-pane refusal + config survives hide/reshow...\n" );
		const std::string tmp = TempPath( "pane_mode_crud.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		using Layout = SceneEditController::ViewportLayout;

		Check( ctrl.SetViewportLayout( Layout::Quad ), "switch to Quad so panes 1-3 are visible" );

		Check( ctrl.SetPaneRenderMode( 1, "depth" ), "SetPaneRenderMode(1, \"depth\") succeeds in Quad" );
		Check( std::string( ctrl.GetPaneRenderMode( 1 ) ) == "depth", "GetPaneRenderMode(1) reports \"depth\"" );

		Check( ctrl.SetPaneRenderMode( 2, "wireframe" ), "SetPaneRenderMode(2, \"wireframe\") succeeds in Quad" );
		Check( std::string( ctrl.GetPaneRenderMode( 2 ) ) == "wireframe", "GetPaneRenderMode(2) reports \"wireframe\"" );

		Check( !ctrl.SetPaneRenderMode( 3, "not_a_real_mode" ), "SetPaneRenderMode(3, unknown-name) refused" );
		Check( std::string( ctrl.GetPaneRenderMode( 3 ) ) == "preview", "pane 3 stays at the default \"preview\" after the refused unknown-name set" );

		Check( !ctrl.SetPaneRenderMode( 3, "objectmap" ), "SetPaneRenderMode(3, \"objectmap\") refused (non-viewport-selectable)" );
		Check( std::string( ctrl.GetPaneRenderMode( 3 ) ) == "preview", "pane 3 still \"preview\" after the refused objectmap set" );

		// Config survives hide/reshow: set pane 3's mode while visible
		// (Quad), shrink to Single (hides panes 1-3), grow back to Quad,
		// confirm the stored mode is unchanged (§7.2: "toggling 2x2 -> 1
		// -> 2x2 must not lose a setup").
		Check( ctrl.SetPaneRenderMode( 3, "facets" ), "SetPaneRenderMode(3, \"facets\") succeeds in Quad" );
		Check( std::string( ctrl.GetPaneRenderMode( 3 ) ) == "facets", "GetPaneRenderMode(3) reports \"facets\" before the hide" );
		Check( ctrl.SetViewportLayout( Layout::Single ), "shrink to Single (hides panes 1-3)" );
		Check( std::string( ctrl.GetPaneRenderMode( 3 ) ) == "facets",
			"GetPaneRenderMode(3) STILL reports \"facets\" while pane 3 is hidden (getters work on any valid index)" );
		Check( ctrl.SetViewportLayout( Layout::Quad ), "grow back to Quad" );
		Check( std::string( ctrl.GetPaneRenderMode( 3 ) ) == "facets",
			"MONEY ASSERTION (§7.2): GetPaneRenderMode(3) reports \"facets\" again after the reshow -- config survived the hide/reshow cycle" );

		// Hidden-pane SET refusal (§7.4): TwoH makes only panes 0,1
		// visible, so pane 2 is hidden even though it holds a valid
		// stored mode ("wireframe" from above) -- the SETTER refuses but
		// the GETTER still returns the stored value.
		Check( ctrl.SetViewportLayout( Layout::TwoH ), "switch to TwoH (panes 0,1 visible; 2,3 hidden)" );
		Check( !ctrl.SetPaneRenderMode( 2, "depth" ), "SetPaneRenderMode(2, ...) refused while pane 2 is hidden in TwoH (§7.4)" );
		Check( std::string( ctrl.GetPaneRenderMode( 2 ) ) == "wireframe",
			"GetPaneRenderMode(2) still returns the STORED \"wireframe\" -- the refused set did not mutate it, and the getter works on hidden panes" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// P5: pane 0 aliasing -- SetPaneRenderMode/GetPaneRenderMode(0)
	// forward to the existing single-viewport calls verbatim.
	//------------------------------------------------------------------
	void TestPaneZeroAliasing()
	{
		std::printf( "P5: pane 0 aliasing (forwards to SetViewportRenderMode/GetViewportRenderMode)...\n" );
		const std::string tmp = TempPath( "pane_zero_alias.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );   // skeleton: no interactive rasterizer

		Check( std::string( ctrl.GetPaneRenderMode( 0 ) ) == std::string( ctrl.GetViewportRenderMode() ),
			"GetPaneRenderMode(0) == GetViewportRenderMode() (alias contract)" );
		Check( std::string( ctrl.GetPaneRenderMode( 0 ) ) == "preview", "both report \"preview\" by default" );

		// Skeleton mode: SetViewportRenderMode refuses on "no interactive
		// rasterizer" BEFORE it even looks at the name (SceneEditController.cpp:
		// the `if (!mInteractiveImpl) return false;` guard precedes the
		// registry lookup) -- SetPaneRenderMode(0, ...) must forward that
		// EXACT refusal for both a registry-valid name and an unknown one.
		Check( !ctrl.SetViewportRenderMode( "depth" ), "control: SetViewportRenderMode(\"depth\") is refused in skeleton mode" );
		Check( !ctrl.SetPaneRenderMode( 0, "depth" ), "SetPaneRenderMode(0, \"depth\") forwards the SAME skeleton-mode refusal" );
		Check( !ctrl.SetPaneRenderMode( 0, "not_a_real_mode" ), "SetPaneRenderMode(0, unknown-name) is ALSO refused (forwarded, same reason)" );
		Check( std::string( ctrl.GetPaneRenderMode( 0 ) ) == "preview", "pane 0 / viewport mode unchanged by the refused calls" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// P6: vantage CRUD (named-view + scene-camera) on pane 1, plus pane
	// 0's no-op-true SceneCamera contract.
	//------------------------------------------------------------------
	void TestVantageCrud()
	{
		std::printf( "P6: pane vantage CRUD (named-view + scene-camera)...\n" );
		const std::string tmp = TempPath( "pane_vantage.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		using Kind = SceneEditController::PaneVantageKind;

		// Layout FIRST: the fixture defaults to Single, where pane 1 is
		// HIDDEN and every setter on it correctly refuses (the §7.4
		// contract P4 verifies on purpose).  This scenario is about the
		// vantage machinery, so make pane 1 visible before exercising it.
		Check( ctrl.SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
			"P6 setup: layout Quad so pane 1 is visible" );

		// A named view that doesn't exist yet is refused regardless of
		// whether CaptureNamedView works in skeleton mode.
		Check( !ctrl.SetPaneVantageNamedView( 1, "no_such_view" ), "SetPaneVantageNamedView refuses a nonexistent view name" );

		// CaptureNamedView (via CaptureCurrentView) only needs a loaded
		// scene + its active camera -- SceneEditController.cpp's
		// CaptureCurrentView reads mJob.GetScene()->GetCamera() directly
		// and never touches mInteractiveImpl, so it works in skeleton
		// mode with this fixture's pinhole camera. Verified by reading
		// the impl before writing this test (no non-skeleton fallback
		// needed).
		Check( ctrl.CaptureNamedView( "testview" ), "CaptureNamedView(\"testview\") succeeds in skeleton mode (scene-camera capture, no rasterizer needed)" );

		Check( ctrl.SetPaneVantageNamedView( 1, "testview" ), "SetPaneVantageNamedView(1, \"testview\") succeeds once the view exists" );
		Kind kind = Kind::FreeFly;             // dirty seed
		String namedView( "dirty" );
		Check( ctrl.GetPaneVantage( 1, kind, namedView ), "GetPaneVantage(1) succeeds after the named-view set" );
		Check( kind == Kind::NamedView, "pane 1 vantage kind is NamedView after the set" );
		Check( namedView == "testview", "pane 1 named-view name is \"testview\"" );

		Check( ctrl.SetPaneVantageSceneCamera( 1 ), "SetPaneVantageSceneCamera(1) succeeds" );
		kind = Kind::NamedView;                // dirty seed, opposite direction this time
		namedView = String( "dirty" );
		Check( ctrl.GetPaneVantage( 1, kind, namedView ), "GetPaneVantage(1) succeeds after reverting to SceneCamera" );
		Check( kind == Kind::SceneCamera, "pane 1 vantage kind is back to SceneCamera" );
		Check( namedView == "", "pane 1 named-view name is cleared on reverting to SceneCamera" );

		// Pane 0's no-op-true contract: not in free-fly, so "track the
		// scene camera" is already satisfied -- ExitFreeFly() is not even
		// called (it would return false for "not active", but that's not
		// a failure of THIS surface).
		Check( !ctrl.IsFreeFlyActive(), "pane 0 / controller starts NOT in free-fly" );
		Check( ctrl.SetPaneVantageSceneCamera( 0 ), "SetPaneVantageSceneCamera(0) succeeds (no-op-true: already tracking the scene camera)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// P7: C-ABI wrappers -- null-controller refusal, GetPaneVantage's
	// null-out/cap==0 refusals, small-cap truncation, and a full
	// layout/primary/mode round-trip.
	//------------------------------------------------------------------
	void TestCAbiWrappers()
	{
		std::printf( "P7: C-ABI null-controller refusals + GetPaneVantage null-out/cap-0 + truncation + round-trip...\n" );

		// Null controller: every setter refuses.
		Check( !RISE_API_SceneEditController_SetViewportLayout( nullptr, 0 ), "C-ABI SetViewportLayout refuses null controller" );
		Check( !RISE_API_SceneEditController_SetPrimaryPane( nullptr, 0 ), "C-ABI SetPrimaryPane refuses null controller" );
		Check( !RISE_API_SceneEditController_SetPaneRenderMode( nullptr, 1, "depth" ), "C-ABI SetPaneRenderMode refuses null controller" );
		Check( !RISE_API_SceneEditController_SetPaneVantageSceneCamera( nullptr, 1 ), "C-ABI SetPaneVantageSceneCamera refuses null controller" );
		Check( !RISE_API_SceneEditController_SetPaneVantageNamedView( nullptr, 1, "x" ), "C-ABI SetPaneVantageNamedView refuses null controller" );

		// Null controller: getters fail closed to their documented defaults.
		Check( std::string( RISE_API_SceneEditController_GetPaneRenderMode( nullptr, 1 ) ) == "preview",
			"C-ABI GetPaneRenderMode(null) reports \"preview\" (fail-closed default)" );
		int kindSentinel = -1;
		char nameSentinelBuf[64];
		Check( !RISE_API_SceneEditController_GetPaneVantage( nullptr, 1, &kindSentinel, nameSentinelBuf, sizeof( nameSentinelBuf ) ),
			"C-ABI GetPaneVantage refuses null controller" );

		// Live controller for the rest.
		const std::string tmp = TempPath( "pane_cabi.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		// Layout FIRST (same reason as P6): pane 1 must be visible for the
		// positive-path assertions below; the later round-trip re-set of
		// Quad is then a no-op-true, which its assertion still accepts.
		Check( RISE_API_SceneEditController_SetViewportLayout( &ctrl,
			static_cast<int>( SceneEditController::ViewportLayout::Quad ) ),
			"P7 setup: layout Quad so pane 1 is visible" );

		// GetPaneVantage null-outs / cap==0 refuse even with a live controller.
		int kindOut = -1;
		char buf[64];
		Check( !RISE_API_SceneEditController_GetPaneVantage( &ctrl, 1, nullptr, buf, sizeof( buf ) ), "C-ABI GetPaneVantage refuses a null outKind" );
		Check( !RISE_API_SceneEditController_GetPaneVantage( &ctrl, 1, &kindOut, nullptr, sizeof( buf ) ), "C-ABI GetPaneVantage refuses a null outNamedView" );
		Check( !RISE_API_SceneEditController_GetPaneVantage( &ctrl, 1, &kindOut, buf, 0 ), "C-ABI GetPaneVantage refuses cap==0" );

		// Small-cap truncation: capture + assign a named view whose name
		// is longer than the truncated capacity, then read it back
		// through a cap=3 buffer -- RISE_API.cpp's `n + 1 < cap` copy loop
		// yields exactly 2 chars + NUL for cap=3.
		Check( ctrl.CaptureNamedView( "testview" ), "CaptureNamedView(\"testview\") succeeds (skeleton mode, scene camera only)" );
		Check( RISE_API_SceneEditController_SetPaneVantageNamedView( &ctrl, 1, "testview" ), "C-ABI SetPaneVantageNamedView(1, \"testview\") succeeds" );
		char smallBuf[3] = { 'X', 'X', 'X' };
		int smallKind = -1;
		Check( RISE_API_SceneEditController_GetPaneVantage( &ctrl, 1, &smallKind, smallBuf, 3 ), "C-ABI GetPaneVantage succeeds with cap=3" );
		Check( smallKind == static_cast<int>( SceneEditController::PaneVantageKind::NamedView ),
			"C-ABI GetPaneVantage reports NamedView kind even when the name truncates" );
		Check( smallBuf[0] == 't' && smallBuf[1] == 'e' && smallBuf[2] == '\0',
			"MONEY ASSERTION: C-ABI GetPaneVantage truncates \"testview\" to 2 chars + NUL at cap=3" );

		// Round-trip layout/primary/mode through the C wrappers.
		Check( RISE_API_SceneEditController_SetViewportLayout( &ctrl, static_cast<int>( SceneEditController::ViewportLayout::Quad ) ),
			"C-ABI SetViewportLayout(Quad) succeeds" );
		int layoutOut = -1;
		Check( RISE_API_SceneEditController_GetViewportLayout( &ctrl, &layoutOut ), "C-ABI GetViewportLayout succeeds" );
		Check( layoutOut == static_cast<int>( SceneEditController::ViewportLayout::Quad ), "C-ABI GetViewportLayout round-trips Quad" );

		Check( RISE_API_SceneEditController_SetPrimaryPane( &ctrl, 2 ), "C-ABI SetPrimaryPane(2) succeeds in Quad" );
		unsigned int primaryOut = 99;
		Check( RISE_API_SceneEditController_GetPrimaryPane( &ctrl, &primaryOut ), "C-ABI GetPrimaryPane succeeds" );
		Check( primaryOut == 2, "C-ABI GetPrimaryPane round-trips 2" );

		Check( RISE_API_SceneEditController_SetPaneRenderMode( &ctrl, 1, "normals" ), "C-ABI SetPaneRenderMode(1, \"normals\") succeeds" );
		Check( std::string( RISE_API_SceneEditController_GetPaneRenderMode( &ctrl, 1 ) ) == "normals",
			"C-ABI GetPaneRenderMode(1) round-trips \"normals\"" );

		pJob->release();
		std::remove( tmp.c_str() );
	}
}   // anonymous namespace

int main()
{
	std::printf( "=== ViewportPaneConfigTest ===\n" );
	TestDefaults();
	TestLayoutSwitching();
	TestPrimaryInvariants();
	TestPaneModeCrud();
	TestPaneZeroAliasing();
	TestVantageCrud();
	TestCAbiWrappers();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
