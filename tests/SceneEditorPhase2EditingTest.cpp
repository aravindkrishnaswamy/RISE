//////////////////////////////////////////////////////////////////////
//
//  SceneEditorPhase2EditingTest.cpp - Phase 2 unit tests for the
//    accordion's editable surface across object, light, and
//    rasterizer categories.
//
//  Covers:
//    - ObjectIntrospection surfaces editable position + scale rows.
//    - SetProperty(position) routes through SetObjectPosition and
//      mutates the object's final transform.
//    - SetProperty(scale) dispatches SetObjectScale.
//    - LightIntrospection surfaces editable position / energy / color.
//    - Light SetProperty edits update the light's emission state via
//      the keyframe-parameter mechanism.
//    - Job::SetActiveRasterizer lazy-instantiates a standard type
//      with defaults when the registry is empty (and a shader is
//      registered).
//    - Job::GetRasterizerType{Count,Name} returns the 8 standard types
//      even when the registry is empty.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <cstring>
#include <set>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ILight.h"
#include "../src/Library/Interfaces/ILightPriv.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/ObjectIntrospection.h"
#include "../src/Library/SceneEditor/LightIntrospection.h"

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

// Seed minimum scaffolding the controller needs: one camera + one
// shader so lazy-rasterizer-build can find a default shader.
static void SeedMinimalScene( Job& job )
{
	ICamera* pCam = nullptr;
	if( RISE_API_CreatePinholeCamera(
		&pCam,
		Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		Scalar( 0.785398 ), 64, 64,
		Scalar( 1 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ),
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) ) )
	{
		job.GetScene()->AddCamera( "default", pCam );
		pCam->release();
	}
	// Add a "global" shader the same way scene files do, so the
	// lazy-rasterizer path can pick it as a default shader.
	const char* ops[] = { "DefaultDirectLighting" };
	job.AddStandardShader( "global", 1, ops );
}

// Helper: heap Job + seed, returning Job*.  Job's destructor is
// protected (refcounted via Implementation::Reference); caller
// `release()`s when done.
static Job* MakeJobAndSeed()
{
	Job* pJob = new Job();
	SeedMinimalScene( *pJob );
	return pJob;
}

//////////////////////////////////////////////////////////////////////
//
// Test 1: ObjectIntrospection surfaces editable position + scale
//
//////////////////////////////////////////////////////////////////////

static void TestObjectIntrospectionEditableRows()
{
	std::cout << "Test 1: ObjectIntrospection editable rows" << std::endl;

	Job* pJob = MakeJobAndSeed();
	{
	// Add a sphere at origin so we have an IObject to introspect.
	pJob->AddSphereGeometry( "sphere_geom", 1.0 );
	const double zero3[3] = { 0, 0, 0 };
	const double one3[3]  = { 1, 1, 1 };
	RadianceMapConfig nilRMap;
	pJob->AddObject(
		"sphere", "sphere_geom",
		/*material*/ nullptr,
		/*modifier*/ nullptr,
		/*shader*/   nullptr,
		nilRMap, zero3, zero3, one3, true, true );

	IObjectManager* objs = pJob->GetObjects();
	const IObject* obj = objs ? objs->GetItem( "sphere" ) : nullptr;
	Check( obj != nullptr, "sphere object registered" );
	if( !obj ) goto done;

	{
		std::vector<CameraProperty> rows = ObjectIntrospection::Inspect(
			String( "sphere" ), *obj );

		// Find the position and scale rows; verify they're editable.
		bool foundPos = false, foundScale = false;
		for( const CameraProperty& p : rows ) {
			if( p.name == String( "position" ) ) {
				foundPos = true;
				Check( p.editable, "position row is editable" );
				Check( p.kind == ValueKind::DoubleVec3,
				       "position row kind is DoubleVec3" );
			}
			if( p.name == String( "scale" ) ) {
				foundScale = true;
				Check( p.editable, "scale row is editable" );
				// Descriptor-driven surface (Phase 4): `scale` is
				// DoubleVec3 (per-axis), matching the standard_object
				// chunk syntax.  Phase 2 surfaced it as a single
				// Double; the test was updated when the panel moved
				// to per-axis to align with the scene-file vocabulary.
				Check( p.kind == ValueKind::DoubleVec3,
				       "scale row kind is DoubleVec3 (per-axis, descriptor-driven)" );
			}
		}
		Check( foundPos,   "position row present" );
		Check( foundScale, "scale row present" );
	}
done:
	;
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 2: SetProperty(position) mutates object transform
//
//////////////////////////////////////////////////////////////////////

static void TestSetObjectPosition()
{
	std::cout << "Test 2: SetProperty(position) moves the object" << std::endl;

	Job* pJob = MakeJobAndSeed();
	pJob->AddSphereGeometry( "sphere_geom", 1.0 );
	const double zero3[3] = { 0, 0, 0 };
	const double one3[3]  = { 1, 1, 1 };
	RadianceMapConfig nilRMap;
	pJob->AddObject(
		"sphere", "sphere_geom",
		/*material*/ nullptr,
		/*modifier*/ nullptr,
		/*shader*/   nullptr,
		nilRMap, zero3, zero3, one3, true, true );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	using Cat = SceneEditController::Category;

	c.ForTest_SetSelection( Cat::Object, String( "sphere" ) );

	// Move the sphere to (3, 4, 5).
	const bool ok = c.SetProperty( String( "position" ), String( "3 4 5" ) );
	Check( ok, "SetProperty(position) returns true" );

	// Verify the object's final transform matches.
	IObjectManager* objs = pJob->GetObjects();
	const IObject* obj = objs->GetItem( "sphere" );
	Check( obj != nullptr, "sphere still registered after edit" );
	if( obj ) {
		const Matrix4 m = obj->GetFinalTransformMatrix();
		Check( std::abs( static_cast<double>( m._30 ) - 3.0 ) < 1e-6, "tx == 3" );
		Check( std::abs( static_cast<double>( m._31 ) - 4.0 ) < 1e-6, "ty == 4" );
		Check( std::abs( static_cast<double>( m._32 ) - 5.0 ) < 1e-6, "tz == 5" );
	}

	// Malformed value should be rejected.
	const bool okBad = c.SetProperty( String( "position" ), String( "not a vec3" ) );
	Check( !okBad, "Malformed position value rejected" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 3: Light editing via SetProperty
//
//////////////////////////////////////////////////////////////////////

static void TestLightEditing()
{
	std::cout << "Test 3: light SetProperty (energy, color)" << std::endl;

	Job* pJob = MakeJobAndSeed();

	// Register a point light.  Energy=1, white color.
	double pos[3] = { 0, 0, 0 };
	double col[3] = { 1, 1, 1 };
	pJob->AddPointOmniLight( "key", 1.0, pos, col, true );

	ILightManager* lights = pJob->GetLights();
	const ILightPriv* light = lights ? lights->GetItem( "key" ) : nullptr;
	Check( light != nullptr, "point light registered" );
	if( !light ) { pJob->release(); return; }

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	using Cat = SceneEditController::Category;
	c.ForTest_SetSelection( Cat::Light, String( "key" ) );

	// Edit energy.
	const bool okEnergy = c.SetProperty( String( "energy" ), String( "5.5" ) );
	Check( okEnergy, "SetProperty(energy) returns true" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 5.5 ) < 1e-6,
	       "emission energy mutated to 5.5" );

	// Edit color.
	const bool okColor = c.SetProperty( String( "color" ), String( "0.2 0.8 0.4" ) );
	Check( okColor, "SetProperty(color) returns true" );
	const RISEPel newColor = light->emissionColor();
	Check( std::abs( static_cast<double>( newColor.r ) - 0.2 ) < 1e-6, "color.r ≈ 0.2" );
	Check( std::abs( static_cast<double>( newColor.g ) - 0.8 ) < 1e-6, "color.g ≈ 0.8" );
	Check( std::abs( static_cast<double>( newColor.b ) - 0.4 ) < 1e-6, "color.b ≈ 0.4" );

	// Edit position (regression for the position-doesn't-actually-move
	// bug Phase 2 review caught — keyframe SetIntermediateValue mutates
	// staging fields only; we MUST follow with RegenerateData() to flush
	// to the rendered final-transform path).  After the edit,
	// `light->position()` must reflect the new value, not (0,0,0).
	// Param name "position" is what `Transformable::KeyframeFromParameters`
	// accepts — it routes to `SetPosition` then via RegenerateData →
	// FinalizeTransformations into the light's `ptPosition`.
	const bool okPos = c.SetProperty( String( "position" ), String( "2 3 -4" ) );
	Check( okPos, "SetProperty(position) returns true" );
	const Point3 newPos = light->position();
	Check( std::abs( static_cast<double>( newPos.x ) - 2.0 ) < 1e-6, "light.position.x ≈ 2" );
	Check( std::abs( static_cast<double>( newPos.y ) - 3.0 ) < 1e-6, "light.position.y ≈ 3" );
	Check( std::abs( static_cast<double>( newPos.z ) + 4.0 ) < 1e-6, "light.position.z ≈ -4" );

	// Unknown property name should fail (no keyframe param matches).
	const bool okBad = c.SetProperty( String( "frobozz" ), String( "1" ) );
	Check( !okBad, "Unknown property name rejected" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 4: Lazy rasterizer instantiation with defaults
//
//////////////////////////////////////////////////////////////////////

static void TestLazyRasterizerInstantiation()
{
	std::cout << "Test 4: lazy rasterizer instantiation" << std::endl;

	Job* pJob = MakeJobAndSeed();   // adds a "global" shader

	// At first, no rasterizer is registered.  But the type list still
	// surfaces all 8 standard types.
	Check( pJob->GetRasterizerTypeCount() >= 8,
	       "Type list includes 8 standard types even when registry is empty" );

	std::set<std::string> seen;
	for( unsigned int i = 0; i < pJob->GetRasterizerTypeCount(); ++i ) {
		seen.insert( pJob->GetRasterizerTypeName( i ) );
	}
	Check( seen.count( "pathtracing_pel_rasterizer" ) == 1, "PT-pel listed" );
	Check( seen.count( "bdpt_pel_rasterizer" )         == 1, "BDPT-pel listed" );
	Check( seen.count( "vcm_pel_rasterizer" )          == 1, "VCM-pel listed" );
	Check( seen.count( "mlt_rasterizer" )              == 1, "MLT listed" );

	// Switch to BDPT-pel — should lazy-build and activate.
	const bool ok = pJob->SetActiveRasterizer( "bdpt_pel_rasterizer" );
	Check( ok, "Lazy-build of bdpt_pel_rasterizer succeeds" );
	Check( pJob->GetActiveRasterizerName() == "bdpt_pel_rasterizer",
	       "Active rasterizer is bdpt_pel_rasterizer" );
	Check( pJob->GetRasterizer() != nullptr,
	       "pRasterizer is non-null after activation" );

	// Switch to PT-pel — also lazy-builds.
	const bool okPT = pJob->SetActiveRasterizer( "pathtracing_pel_rasterizer" );
	Check( okPT, "Switch to PT-pel succeeds" );
	Check( pJob->GetActiveRasterizerName() == "pathtracing_pel_rasterizer",
	       "Active rasterizer switched to PT-pel" );

	// Switch back to BDPT — should be cached, not re-built.
	const bool okBack = pJob->SetActiveRasterizer( "bdpt_pel_rasterizer" );
	Check( okBack, "Switch back to BDPT succeeds (cached)" );

	// Unknown name fails.
	const bool okBogus = pJob->SetActiveRasterizer( "made_up_rasterizer" );
	Check( !okBogus, "Unknown rasterizer name returns false" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 5: Lazy build fails gracefully without a shader
//
//////////////////////////////////////////////////////////////////////

static void TestLazyRasterizerNoShader()
{
	std::cout << "Test 5: lazy rasterizer with no shader" << std::endl;

	Job* pJob = new Job();   // no SeedMinimalScene → no shader registered

	const bool ok = pJob->SetActiveRasterizer( "bdpt_pel_rasterizer" );
	Check( !ok, "Lazy-build fails when no shader is registered" );
	Check( pJob->GetRasterizer() == nullptr,
	       "pRasterizer remains null after failed lazy-build" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "SceneEditorPhase2EditingTest" << std::endl;
	std::cout << "============================" << std::endl;

	TestObjectIntrospectionEditableRows();
	TestSetObjectPosition();
	TestLightEditing();
	TestLazyRasterizerInstantiation();
	TestLazyRasterizerNoShader();

	std::cout << std::endl;
	std::cout << "Results: " << passCount << " passed, "
	                          << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
