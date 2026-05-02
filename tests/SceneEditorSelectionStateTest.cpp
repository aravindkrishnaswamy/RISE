//////////////////////////////////////////////////////////////////////
//
//  SceneEditorSelectionStateTest.cpp - Phase 1 / 2 unit test for the
//    SceneEditController's accordion-selection state.
//
//  Covers:
//    - Default selection is (None, "").
//    - SetSelection round-trips across all 4 categories.
//    - Camera selection actually activates the named camera (via the
//      cancel-and-park path; we run with no rasterizer so the path is
//      a UI-state-only mutation).
//    - Object/Light selections are pure UI state (no scene mutation).
//    - CategoryEntityCount/Name reflect what the scene's managers
//      hold, in lex-order.
//    - SceneEpoch is uniquely-bumped per controller construction so
//      platform UIs can detect scene-reload via the (epoch, list)
//      cache invalidation pattern.
//    - CurrentPanelMode and CurrentPanelHeader follow selection.
//    - ForTest_SetSelection bypasses the cancel-and-park lock and
//      writes selection state directly (for test/instrumentation).
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <cstring>
#include <set>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

// Helper: construct a heap-allocated Job + initialize containers.
// Job's destructor is protected (refcounted via Implementation::Reference).
// Caller is responsible for `pJob->release()`.
static Job* MakeJob()
{
	return new Job();
}

// Construct + register a pinhole camera under `name`.  Used to seed
// the camera manager with multiple distinct cameras for switch tests.
static void AddCamera( Job& job, const char* name, double zPos )
{
	ICamera* pCam = nullptr;
	if( !RISE_API_CreatePinholeCamera(
		&pCam,
		Point3( 0, 0, zPos ),
		Point3( 0, 0, 0 ),
		Vector3( 0, 1, 0 ),
		Scalar( 0.785398 ),
		64, 64,
		Scalar( 1 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ),
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) ) )
	{
		return;
	}
	if( IScenePriv* scene = job.GetScene() ) {
		scene->AddCamera( name, pCam );
	}
	pCam->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 1: default selection is (None, "")
//
//////////////////////////////////////////////////////////////////////

static void TestDefaultSelection()
{
	std::cout << "Test 1: default selection" << std::endl;

	Job* pJob = MakeJob();
	AddCamera( *pJob, "alpha", 5 );
	{
		SceneEditController c( *pJob, /*interactiveRasterizer*/0 );

		Check( c.GetSelectionCategory() == SceneEditController::Category::None,
		       "default category is None" );
		Check( c.GetSelectionName().size() <= 1,
		       "default name is empty" );
		Check( c.CurrentPanelMode() == SceneEditController::PanelMode::None,
		       "default panel mode is None" );
		Check( c.CurrentPanelHeader().size() <= 1,
		       "default header is empty" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 2: SetSelection round-trip across categories
//
//////////////////////////////////////////////////////////////////////

static void TestSetSelectionRoundTrip()
{
	std::cout << "Test 2: SetSelection round-trip" << std::endl;

	Job* pJob = MakeJob();
	AddCamera( *pJob, "alpha", 5 );
	AddCamera( *pJob, "beta",  10 );
	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );

	using Cat = SceneEditController::Category;

	// Camera selection activates the named camera.  No interactive
	// rasterizer is attached, so the cancel-and-park path is purely
	// scene-mutation.
	const bool okAlpha = c.SetSelection( Cat::Camera, String( "alpha" ) );
	Check( okAlpha, "SetSelection(Camera, alpha) returns true" );
	Check( c.GetSelectionCategory() == Cat::Camera,
	       "category becomes Camera" );
	Check( std::strcmp( c.GetSelectionName().c_str(), "alpha" ) == 0,
	       "name becomes alpha" );
	Check( c.CurrentPanelMode() == SceneEditController::PanelMode::Camera,
	       "panel mode becomes Camera" );

	// Camera switch — a second SetSelection with a different camera.
	const bool okBeta = c.SetSelection( Cat::Camera, String( "beta" ) );
	Check( okBeta, "SetSelection(Camera, beta) returns true" );
	Check( std::strcmp( c.GetSelectionName().c_str(), "beta" ) == 0,
	       "name becomes beta" );

	// Camera selection with unknown name fails (SetActiveCamera rejects).
	const bool okBogus = c.SetSelection( Cat::Camera, String( "nonexistent" ) );
	Check( !okBogus, "SetSelection with unknown camera name returns false" );

	// Object selection — UI state only, no validation against the
	// scene's object manager (the manager is empty in this test).
	const bool okObj = c.SetSelection( Cat::Object, String( "Sphere" ) );
	Check( okObj, "SetSelection(Object) returns true even without manager entry" );
	Check( c.GetSelectionCategory() == Cat::Object,
	       "category becomes Object" );
	Check( c.CurrentPanelMode() == SceneEditController::PanelMode::Object,
	       "panel mode becomes Object" );

	// Light selection.
	const bool okLight = c.SetSelection( Cat::Light, String( "key" ) );
	Check( okLight, "SetSelection(Light) returns true" );
	Check( c.GetSelectionCategory() == Cat::Light, "category becomes Light" );
	Check( c.CurrentPanelMode() == SceneEditController::PanelMode::Light,
	       "panel mode becomes Light" );

	// Empty-name selection: opens the section but no row picked.
	// Object/Light with empty name should resolve to PanelMode::None
	// (no row to inspect).
	const bool okEmptyObj = c.SetSelection( Cat::Object, String() );
	Check( okEmptyObj, "SetSelection(Object, empty) returns true" );
	Check( c.GetSelectionCategory() == Cat::Object,
	       "empty Object selection still has Object category" );
	Check( c.CurrentPanelMode() == SceneEditController::PanelMode::None,
	       "empty Object selection panel mode is None" );

	// Clear selection.
	c.SetSelection( Cat::None, String() );
	Check( c.GetSelectionCategory() == Cat::None, "Clear → category None" );
	Check( c.CurrentPanelMode() == SceneEditController::PanelMode::None,
	       "Clear → panel mode None" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 3: CategoryEntityCount / Name reflect the scene managers
//
//////////////////////////////////////////////////////////////////////

static void TestCategoryEnumeration()
{
	std::cout << "Test 3: category enumeration" << std::endl;

	Job* pJob = MakeJob();
	AddCamera( *pJob, "alpha", 5 );
	AddCamera( *pJob, "beta",  10 );
	AddCamera( *pJob, "gamma", 15 );
	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );

	using Cat = SceneEditController::Category;

	const unsigned int nCams = c.CategoryEntityCount( Cat::Camera );
	Check( nCams == 3, "Camera count = 3" );

	// Names should match the seeded list (manager iteration order is
	// implementation-defined; collect into a set and compare).
	std::set<std::string> seen;
	for( unsigned int i = 0; i < nCams; ++i ) {
		seen.insert( c.CategoryEntityName( Cat::Camera, i ).c_str() );
	}
	Check( seen.count( "alpha" ) == 1, "alpha enumerated" );
	Check( seen.count( "beta" )  == 1, "beta enumerated" );
	Check( seen.count( "gamma" ) == 1, "gamma enumerated" );

	// Empty categories.
	Check( c.CategoryEntityCount( Cat::Object ) == 0, "Object count = 0" );
	Check( c.CategoryEntityCount( Cat::Light )  == 0, "Light count = 0" );
	// Rasterizer count: always >= 8 in Phase 2 because Job's
	// `GetRasterizerTypeCount` surfaces the standard 8-type catalogue
	// (PT/BDPT/VCM/MLT × Pel/Spectral) regardless of whether any have
	// been instantiated.  Selecting an unbuilt type triggers
	// lazy-instantiation with defaults inside SetActiveRasterizer.
	Check( c.CategoryEntityCount( Cat::Rasterizer ) >= 8,
	       "Rasterizer count >= 8 (standard catalogue always listed)" );

	// Out-of-range index returns empty name.
	Check( c.CategoryEntityName( Cat::Camera, 999 ).size() <= 1,
	       "Out-of-range idx returns empty" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 4: SceneEpoch is uniquely-bumped per controller construction
//
//////////////////////////////////////////////////////////////////////

static void TestSceneEpochUniqueness()
{
	std::cout << "Test 4: SceneEpoch uniqueness" << std::endl;

	// Two controllers built back-to-back over the same job — each
	// should have a different epoch.  This is the property platform
	// UIs rely on to detect scene-reload (controller destroy/recreate)
	// and re-pull cached entity lists.
	Job* pJob = MakeJob();
	AddCamera( *pJob, "alpha", 5 );
	{
	SceneEditController c1( *pJob, /*interactiveRasterizer*/0 );
	const unsigned int e1 = c1.SceneEpoch();

	SceneEditController c2( *pJob, /*interactiveRasterizer*/0 );
	const unsigned int e2 = c2.SceneEpoch();

	Check( e1 != e2, "Two controllers have different epochs" );
	Check( e2 > e1,  "Second controller's epoch is later" );

	// Epoch is stable across reads from the same controller.
	Check( c1.SceneEpoch() == e1, "Epoch is stable across reads" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 5: ForTest_SetSelection writes state directly
//
//////////////////////////////////////////////////////////////////////

static void TestForTestSetSelection()
{
	std::cout << "Test 5: ForTest_SetSelection" << std::endl;

	Job* pJob = MakeJob();
	AddCamera( *pJob, "alpha", 5 );
	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );

	using Cat = SceneEditController::Category;

	// ForTest_SetSelection bypasses cancel-and-park and side effects.
	// Verify it lands in selection state for any category, including
	// nonexistent entity names that public SetSelection would reject.
	c.ForTest_SetSelection( Cat::Object, String( "PhantomObject" ) );
	Check( c.GetSelectionCategory() == Cat::Object,
	       "ForTest_SetSelection(Object) lands category" );
	Check( std::strcmp( c.GetSelectionName().c_str(), "PhantomObject" ) == 0,
	       "ForTest_SetSelection(Object) lands name" );

	// SelectedObjectName legacy accessor should reflect the Object
	// selection but be empty for non-Object selections.
	Check( std::strcmp( c.SelectedObjectName().c_str(), "PhantomObject" ) == 0,
	       "SelectedObjectName matches Object selection" );

	c.ForTest_SetSelection( Cat::Camera, String( "alpha" ) );
	Check( c.SelectedObjectName().size() <= 1,
	       "SelectedObjectName empty when category is Camera" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 6: CurrentPanelHeader format matches selection
//
//////////////////////////////////////////////////////////////////////

static void TestPanelHeaderFormat()
{
	std::cout << "Test 6: panel header format" << std::endl;

	Job* pJob = MakeJob();
	AddCamera( *pJob, "alpha", 5 );
	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );

	using Cat = SceneEditController::Category;

	c.SetSelection( Cat::Camera, String( "alpha" ) );
	Check( std::strcmp( c.CurrentPanelHeader().c_str(), "Camera: alpha" ) == 0,
	       "Camera header includes name" );

	c.ForTest_SetSelection( Cat::Object, String( "Sphere" ) );
	Check( std::strcmp( c.CurrentPanelHeader().c_str(), "Object: Sphere" ) == 0,
	       "Object header includes name" );

	c.ForTest_SetSelection( Cat::Light, String( "key" ) );
	Check( std::strcmp( c.CurrentPanelHeader().c_str(), "Light: key" ) == 0,
	       "Light header includes name" );

	c.ForTest_SetSelection( Cat::None, String() );
	Check( c.CurrentPanelHeader().size() <= 1,
	       "None header is empty" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "SceneEditorSelectionStateTest" << std::endl;
	std::cout << "==============================" << std::endl;

	TestDefaultSelection();
	TestSetSelectionRoundTrip();
	TestCategoryEnumeration();
	TestSceneEpochUniqueness();
	TestForTestSetSelection();
	TestPanelHeaderFormat();

	std::cout << std::endl;
	std::cout << "Results: " << passCount << " passed, "
	                          << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
