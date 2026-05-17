//////////////////////////////////////////////////////////////////////
//
//  SceneEditorPhase4bSelectionTest.cpp — Per-category selection
//  model + Object→Material auto-sync (Phase 4b).
//
//  Scope:
//    1. SetSelection(Object, name) auto-fills the Material section's
//       per-category selection with the object's bound material name.
//    2. SetSelection(Material, name) — when primary was Object —
//       CLEARS the Object selection per the user-spec
//       "Object selection clears when Material is picked directly".
//    3. SetSelection(None, _) clears every per-category selection.
//    4. GetSelectionNameForCategory(cat) returns the per-cat pick.
//    5. PropertyCountFor(cat) populates rows only for sections with
//       a non-empty per-cat selection.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <iostream>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;

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

namespace {

// Build a Job with: one camera, one Lambertian material "red", one
// object "sphere" that uses "red".  Returns the Job; tests call
// AddObject etc. via the IJob API since Job.cpp is the canonical
// scene-construction surface (matches every scene-load + GUI flow).
Job* BuildJob()
{
	Job* job = new Job();
	job->SetFilm( 64, 64, 1.0 );

	// Camera (needed so scene->GetCamera() returns non-null for the
	// controller's various accessors).
	double loc[3] = { 0, 0, 5 };
	double la[3]  = { 0, 0, 0 };
	double up[3]  = { 0, 1, 0 };
	double orient[3] = { 0, 0, 0 };
	double target[2] = { 0, 0 };
	job->AddPinholeCamera( "default", loc, la, up,
		0.785398, 1.0, 0, 0, orient, target, 0.0, 0.0 );

	// Painter + material.
	const double red[3] = { 1.0, 0.2, 0.2 };
	job->AddUniformColorPainter( "red_paint", red, "Rec709RGB_Linear" );
	job->AddLambertianMaterial( "red", "red_paint" );

	const double white[3] = { 1.0, 1.0, 1.0 };
	job->AddUniformColorPainter( "white_paint", white, "Rec709RGB_Linear" );
	job->AddLambertianMaterial( "white", "white_paint" );

	// Object using `red` material.  AddSphereGeometry first, then
	// AddObject referencing the geometry + material.
	job->AddSphereGeometry( "sphere_geom", 1.0 );
	double pos[3] = { 0, 0, 0 };
	double objOrient[3] = { 0, 0, 0 };
	double objScale[3] = { 1, 1, 1 };
	RadianceMapConfig rm;
	job->AddObject( "sphere", "sphere_geom", "red", 0, 0, rm, pos, objOrient, objScale, true, true );

	return job;
}

}  // namespace

//////////////////////////////////////////////////////////////////////

static void TestObjectPickAutoSyncsMaterial()
{
	std::cout << "Testing SetSelection(Object) auto-fills Material section..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 /* no interactive rasterizer for test */ );

	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	// Primary should be Object.
	Check( ctrl.GetSelectionCategory() == SceneEditController::Category::Object,
	       "primary category is Object" );
	Check( ctrl.GetSelectionName() == String( "sphere" ),
	       "primary name is 'sphere'" );

	// Per-category accessors: Object pick non-empty.
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Object ) == String( "sphere" ),
	       "Object section has 'sphere'" );
	// Auto-fill: Material section now shows the object's bound material 'red'.
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Material ) == String( "red" ),
	       "Material section auto-filled with 'red' (object's bound material)" );
	// Other sections empty.
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Light ).size() <= 1,
	       "Light section empty" );

	job->release();
}

//////////////////////////////////////////////////////////////////////

static void TestObjectRepickReplacesMaterialAutoSync()
{
	std::cout << "Testing second Object pick replaces Material auto-sync..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	// Pick the sphere -> Material auto-fills to 'red'.
	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Material ) == String( "red" ),
	       "first pick: Material = 'red'" );

	// Re-pick the sphere (idempotent) — still 'red'.
	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Material ) == String( "red" ),
	       "re-pick: Material still 'red'" );

	// Repoint the object's material to 'white' via the SceneEdit
	// path, then re-pick: Material should now follow to 'white'.
	IObjectPriv* obj = const_cast<IObjectManager*>( job->GetScene()->GetObjects() )->GetItem( "sphere" );
	Check( obj != 0, "sphere object resolves" );
	IMaterial* white = job->GetMaterials()->GetItem( "white" );
	Check( white != 0, "white material resolves" );
	obj->AssignMaterial( *white );

	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Material ) == String( "white" ),
	       "after rebinding sphere.material=white, Object pick fills Material with 'white'" );

	job->release();
}

//////////////////////////////////////////////////////////////////////

static void TestMaterialPickClearsObject()
{
	std::cout << "Testing direct Material pick clears Object selection..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	// First: pick object (fills Material auto-sync).
	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Object ) == String( "sphere" ),
	       "pre-condition: Object = 'sphere'" );

	// Now: pick Material directly.
	ctrl.SetSelection( SceneEditController::Category::Material, String( "white" ) );

	Check( ctrl.GetSelectionCategory() == SceneEditController::Category::Material,
	       "primary is now Material" );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Material ) == String( "white" ),
	       "Material section has 'white'" );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Object ).size() <= 1,
	       "Object section cleared per user-spec 'Material direct pick clears Object'" );

	job->release();
}

//////////////////////////////////////////////////////////////////////

static void TestSelectionNoneClearsAll()
{
	std::cout << "Testing SetSelection(None) clears every per-category selection..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Object ).size() > 1, "pre: Object set" );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Material ).size() > 1, "pre: Material auto-set" );

	ctrl.SetSelection( SceneEditController::Category::None, String() );

	Check( ctrl.GetSelectionCategory() == SceneEditController::Category::None,
	       "primary is None" );
	for( int i = 1; i < 7; ++i ) {
		const auto cat = static_cast<SceneEditController::Category>( i );
		Check( ctrl.GetSelectionNameForCategory( cat ).size() <= 1,
		       "all per-category selections cleared" );
	}

	job->release();
}

//////////////////////////////////////////////////////////////////////

static void TestRefreshPopulatesPerCategorySnapshots()
{
	std::cout << "Testing RefreshProperties populates per-category snapshots..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	// No selection: nothing to display.
	ctrl.RefreshProperties();
	Check( ctrl.PropertyCountFor( SceneEditController::Category::Object ) == 0,
	       "Object snapshot empty with no selection" );

	// Pick object -> both Object and Material sections have rows.
	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	ctrl.RefreshProperties();

	Check( ctrl.PropertyCountFor( SceneEditController::Category::Object ) > 0,
	       "Object section has rows after pick" );
	Check( ctrl.PropertyCountFor( SceneEditController::Category::Material ) > 0,
	       "Material section auto-populated with rows" );
	// Other sections empty.
	Check( ctrl.PropertyCountFor( SceneEditController::Category::Light ) == 0,
	       "Light section empty" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// Section-expansion is tracked SEPARATELY from per-category
// selection (the Phase 4b regression-fix).  A header click that
// sends an empty-name SetSelection expands the section but doesn't
// pick a specific entity.
//////////////////////////////////////////////////////////////////////

static void TestHeaderClickExpandsWithoutPicking()
{
	std::cout << "Testing header-click (empty-name SetSelection) expands section..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	// Pre-condition: nothing expanded.
	for( int i = 1; i < 7; ++i ) {
		Check( !ctrl.IsSectionExpanded( static_cast<SceneEditController::Category>( i ) ),
		       "pre: section not expanded" );
	}

	// Header click on Camera: empty-name SetSelection.  Section
	// should expand even though no specific camera is picked.
	ctrl.SetSelection( SceneEditController::Category::Camera, String() );

	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Camera ),
	       "Camera section expanded after header click" );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Camera ).size() <= 1,
	       "Camera section has no entity picked (header-only click)" );

	// CollapseSection on Camera: clears both flags.
	ctrl.CollapseSection( SceneEditController::Category::Camera );
	Check( !ctrl.IsSectionExpanded( SceneEditController::Category::Camera ),
	       "Camera section collapsed" );

	job->release();
}

static void TestObjectPickExpandsObjectAndMaterial()
{
	std::cout << "Testing Object pick auto-expands BOTH Object and Material sections..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Object ),
	       "Object section expanded" );
	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Material ),
	       "Material section auto-expanded (Object had bound material)" );
	Check( !ctrl.IsSectionExpanded( SceneEditController::Category::Light ),
	       "Light section stays collapsed" );

	job->release();
}

static void TestCollapseSectionLeavesOthersAlone()
{
	std::cout << "Testing CollapseSection(Material) leaves Object expanded..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	// Pick object: both Object + Material expand.
	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Object ), "pre: Object expanded" );
	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Material ), "pre: Material expanded" );

	// User collapses just the Materials section.
	ctrl.CollapseSection( SceneEditController::Category::Material );

	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Object ),
	       "Object still expanded after collapsing Material" );
	Check( !ctrl.IsSectionExpanded( SceneEditController::Category::Material ),
	       "Material section collapsed" );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Material ).size() <= 1,
	       "Material per-cat selection cleared by collapse" );

	job->release();
}

// Regression: PickAt was writing to mSelectionCategory/mSelectionName
// directly, bypassing the per-category state arrays.  This made
// viewport clicks invisible to the multi-section panel (sections
// never expanded, dropdowns stayed empty).  ForTest_SetSelection
// has the same bypass risk and is now updated too.
static void TestForTestSetSelectionUpdatesPerCategoryState()
{
	std::cout << "Testing ForTest_SetSelection populates per-category state..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	// Use the test-bypass entry point and confirm BOTH the primary
	// tuple AND the per-cat state reflect the selection — the same
	// shape PickAt produces when routed through SetSelection.
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	Check( ctrl.GetSelectionCategory() == SceneEditController::Category::Object,
	       "primary is Object" );
	Check( ctrl.GetSelectionName() == String( "sphere" ),
	       "primary name is 'sphere'" );
	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Object ),
	       "Object section flagged expanded by ForTest_SetSelection" );
	Check( ctrl.GetSelectionNameForCategory( SceneEditController::Category::Object ) == String( "sphere" ),
	       "Object per-cat selection set" );

	job->release();
}

static void TestSelectionNoneAlsoClearsExpansion()
{
	std::cout << "Testing SetSelection(None) clears every section's expanded flag..." << std::endl;
	Job* job = BuildJob();
	SceneEditController ctrl( *job, 0 );

	ctrl.SetSelection( SceneEditController::Category::Camera, String() );
	ctrl.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Camera ), "pre: Camera expanded" );
	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Object ), "pre: Object expanded" );
	Check( ctrl.IsSectionExpanded( SceneEditController::Category::Material ), "pre: Material auto-expanded" );

	ctrl.SetSelection( SceneEditController::Category::None, String() );

	for( int i = 1; i < 7; ++i ) {
		Check( !ctrl.IsSectionExpanded( static_cast<SceneEditController::Category>( i ) ),
		       "all sections collapsed after SetSelection(None)" );
	}

	job->release();
}

//////////////////////////////////////////////////////////////////////

int main()
{
	TestObjectPickAutoSyncsMaterial();
	TestObjectRepickReplacesMaterialAutoSync();
	TestMaterialPickClearsObject();
	TestSelectionNoneClearsAll();
	TestRefreshPopulatesPerCategorySnapshots();
	TestHeaderClickExpandsWithoutPicking();
	TestObjectPickExpandsObjectAndMaterial();
	TestCollapseSectionLeavesOthersAlone();
	TestForTestSetSelectionUpdatesPerCategoryState();
	TestSelectionNoneAlsoClearsExpansion();

	std::cout << std::endl;
	std::cout << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
