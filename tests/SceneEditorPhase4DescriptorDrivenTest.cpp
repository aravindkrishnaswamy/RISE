//////////////////////////////////////////////////////////////////////
//
//  SceneEditorPhase4DescriptorDrivenTest.cpp - Phase 4 tests for
//    descriptor-driven introspection across all four entity
//    categories.
//
//  Phase 4 made every introspection class source its row list from
//  the chunk descriptor that the parser uses to LOAD the entity:
//   - CameraIntrospection (already descriptor-driven since Phase 1)
//   - LightIntrospection  (Phase 4 refactor): omni_light / spot_light
//                          / directional_light / ambient_light
//   - RasterizerIntrospection (Phase 4 refactor): pulls from each
//                          rasterizer chunk's descriptor
//   - ObjectIntrospection (Phase 4 refactor): standard_object
//
//  Tests verify:
//   - Each introspection emits rows matching descriptor parameters
//     (modulo per-type filters: name skipped, etc.).
//   - Editable-vs-read-only flags follow the runtime-writability of
//     each descriptor parameter.
//   - Descriptor descriptions / unit labels propagate to panel rows.
//   - The chunk-name → keyframe-name translation in SetLightProperty
//     handles the `power` ↔ `energy` and `inner` ↔ `inner_angle`
//     name divergences correctly.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cstring>
#include <cmath>
#include <set>
#include <string>

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
#include "../src/Library/SceneEditor/LightIntrospection.h"
#include "../src/Library/SceneEditor/ObjectIntrospection.h"
#include "../src/Library/SceneEditor/RasterizerIntrospection.h"
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) passCount++;
	else { failCount++; std::cout << "  FAIL: " << name << std::endl; }
}

static Job* MakeJobAndSeed()
{
	Job* job = new Job();
	ICamera* pCam = nullptr;
	if( RISE_API_CreatePinholeCamera(
		&pCam, Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		Scalar( 0.785398 ), 64, 64,
		Scalar( 1 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ),
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) ) )
	{
		job->GetScene()->AddCamera( "default", pCam );
		pCam->release();
	}
	const char* shaderOps[] = { "DefaultDirectLighting" };
	job->AddStandardShader( "global", 1, shaderOps );

	job->AddSphereGeometry( "sphere_geom", 1.0 );
	double white[3] = { 1, 1, 1 };
	job->AddUniformColorPainter( "white", white, "sRGB" );
	job->AddLambertianMaterial( "lambert_white", "white" );
	const double zero3[3] = { 0, 0, 0 };
	const double one3[3]  = { 1, 1, 1 };
	RadianceMapConfig nilRMap;
	job->AddObject(
		"sphere", "sphere_geom",
		"lambert_white", nullptr, nullptr,
		nilRMap, zero3, zero3, one3, true, true );

	return job;
}

// Build the set of parameter names a descriptor declares (skipping
// "name" because it's surfaced as the panel header, not a row).
static std::set<std::string> DescriptorParamNames( const ChunkDescriptor* desc )
{
	std::set<std::string> out;
	if( !desc ) return out;
	for( const ParameterDescriptor& p : desc->parameters ) {
		if( p.name == "name" ) continue;
		out.insert( p.name );
	}
	return out;
}

//////////////////////////////////////////////////////////////////////
// Test 1: Light introspection sources rows from the chunk descriptor
//////////////////////////////////////////////////////////////////////

static void TestLightDescriptorDriven()
{
	std::cout << "Test 1: light introspection is descriptor-driven" << std::endl;

	Job* pJob = MakeJobAndSeed();
	double pos[3] = { 1, 2, 3 };
	double col[3] = { 1, 1, 1 };
	pJob->AddPointOmniLight( "omni", 1.0, col, pos, true );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "omni" );

	std::vector<CameraProperty> rows = LightIntrospection::Inspect( String( "omni" ), *light );

	// Must include every descriptor param (skipping name).
	const ChunkDescriptor* desc = DescriptorForKeyword( String( "omni_light" ) );
	const std::set<std::string> expected = DescriptorParamNames( desc );

	std::set<std::string> seen;
	for( const CameraProperty& r : rows ) {
		seen.insert( r.name.c_str() );
	}
	for( const std::string& name : expected ) {
		Check( seen.count( name ) == 1,
		       ( "omni_light row '" + name + "' surfaced" ).c_str() );
	}

	// Power/color rows pick up descriptor description text.  Just
	// verify they're non-empty (we override `power`'s description so
	// equality with descriptor would fail; checking non-empty is the
	// invariant that matters).
	for( const CameraProperty& r : rows ) {
		if( r.name == String( "color" ) ) {
			Check( r.description.size() > 1, "color row has description" );
			Check( r.kind == ValueKind::DoubleVec3, "color row kind is DoubleVec3" );
		}
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 2: Spot light surfaces every spot-only param
//////////////////////////////////////////////////////////////////////

static void TestSpotLightDescriptorDriven()
{
	std::cout << "Test 2: spot light surfaces inner/outer/target from descriptor" << std::endl;

	Job* pJob = MakeJobAndSeed();
	const double DEG = static_cast<double>( PI ) / 180.0;
	double pos[3] = { 1, 2, 3 };
	double tgt[3] = { 4, 5, 6 };
	double col[3] = { 1, 1, 1 };
	pJob->AddPointSpotLight( "spot", 1.0, col, tgt, 30.0 * DEG, 60.0 * DEG, pos, true );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "spot" );
	std::vector<CameraProperty> rows = LightIntrospection::Inspect( String( "spot" ), *light );

	std::set<std::string> seen;
	for( const CameraProperty& r : rows ) seen.insert( r.name.c_str() );

	Check( seen.count( "inner" )  == 1, "spot row 'inner' surfaced (chunk name)" );
	Check( seen.count( "outer" )  == 1, "spot row 'outer' surfaced (chunk name)" );
	Check( seen.count( "target" ) == 1, "spot row 'target' surfaced" );
	Check( seen.count( "power" )  == 1, "spot row 'power' surfaced (chunk name; not 'energy')" );

	// Inner / outer rows should have degree unit hint.
	for( const CameraProperty& r : rows ) {
		if( r.name == String( "inner" ) || r.name == String( "outer" ) ) {
			Check( r.unitLabel == String( "°" ),
			       ( "spot " + std::string( r.name.c_str() ) + " has degree unit hint" ).c_str() );
		}
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 3: SetProperty accepts both chunk and keyframe vocabularies
//////////////////////////////////////////////////////////////////////

static void TestLightSetPropertyAcceptsBothVocabularies()
{
	std::cout << "Test 3: light SetProperty accepts chunk + keyframe vocab" << std::endl;

	Job* pJob = MakeJobAndSeed();
	double pos[3] = { 0, 0, 0 };
	double col[3] = { 1, 1, 1 };
	pJob->AddPointOmniLight( "key", 1.0, col, pos, true );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "key" ) );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "key" );

	// "power" is the chunk-name; the controller should translate to
	// "energy" for KeyframeFromParameters internally.
	Check( c.SetProperty( String( "power" ), String( "3.5" ) ),
	       "SetProperty(power) accepted (chunk vocab)" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 3.5 ) < 1e-6,
	       "energy = 3.5 after chunk-vocab edit" );

	// "energy" is the keyframe-name; should also work for backward compat.
	Check( c.SetProperty( String( "energy" ), String( "7.0" ) ),
	       "SetProperty(energy) accepted (keyframe vocab)" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 7.0 ) < 1e-6,
	       "energy = 7.0 after keyframe-vocab edit" );

	// Undo replays through the chunk-name path.
	Check( c.Editor().Undo(), "Undo last edit" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 3.5 ) < 1e-6,
	       "energy restored to 3.5 after undo" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 4: Rasterizer introspection lists every descriptor param
//////////////////////////////////////////////////////////////////////

static void TestRasterizerDescriptorDriven()
{
	std::cout << "Test 4: rasterizer introspection surfaces descriptor params" << std::endl;

	Job* pJob = MakeJobAndSeed();
	pJob->SetActiveRasterizer( "bdpt_pel_rasterizer" );

	std::vector<CameraProperty> rows = RasterizerIntrospection::Inspect(
		*pJob, String( "bdpt_pel_rasterizer" ) );

	const ChunkDescriptor* desc = DescriptorForKeyword( String( "bdpt_pel_rasterizer" ) );
	std::set<std::string> expected = DescriptorParamNames( desc );
	// `defaultshader` is filtered (surfaced as a separate read-only
	// footer "Default Shader" below the descriptor loop — Reference
	// params need a presets dropdown + rebuild plumbing).  Skip it
	// from the descriptor-coverage assertion.
	expected.erase( "defaultshader" );

	std::set<std::string> seen;
	for( const CameraProperty& r : rows ) seen.insert( r.name.c_str() );

	// Every other BDPT-pel descriptor param (except `name`) is in the panel.
	for( const std::string& n : expected ) {
		Check( seen.count( n ) == 1,
		       ( "bdpt_pel row '" + n + "' surfaced" ).c_str() );
	}

	// Default-shader footer is present.
	Check( seen.count( "Default Shader" ) == 1,
	       "rasterizer 'Default Shader' read-only footer present" );

	// Editable params we have a reader for (samples, max_eye_depth,
	// max_light_depth, show_luminaires, oidn_denoise) are editable.
	for( const CameraProperty& r : rows ) {
		const std::string n( r.name.c_str() );
		if( n == "samples" || n == "max_eye_depth" || n == "max_light_depth"
		 || n == "show_luminaires" || n == "oidn_denoise" )
		{
			Check( r.editable, ( "bdpt row '" + n + "' is editable" ).c_str() );
		}
	}

	// Round-trip: edit samples, verify the snapshot updated and the
	// row reflects the new value.
	const bool ok = pJob->SetRasterizerParameter( "bdpt_pel_rasterizer", "samples", "16" );
	Check( ok, "SetRasterizerParameter(samples=16) descriptor-name path" );
	std::vector<CameraProperty> rows2 = RasterizerIntrospection::Inspect(
		*pJob, String( "bdpt_pel_rasterizer" ) );
	for( const CameraProperty& r : rows2 ) {
		if( r.name == String( "samples" ) ) {
			Check( r.value == String( "16" ), "samples row reflects updated value" );
		}
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 5: Object introspection sourced from standard_object
//////////////////////////////////////////////////////////////////////

static void TestObjectDescriptorDriven()
{
	std::cout << "Test 5: object introspection sourced from standard_object" << std::endl;

	Job* pJob = MakeJobAndSeed();

	IObjectManager* objs = pJob->GetObjects();
	const IObject* obj = objs->GetItem( "sphere" );

	std::vector<CameraProperty> rows = ObjectIntrospection::Inspect(
		String( "sphere" ), *obj, pJob->GetMaterials(), pJob->GetShaders() );

	const ChunkDescriptor* desc = DescriptorForKeyword( String( "standard_object" ) );
	const std::set<std::string> expected = DescriptorParamNames( desc );

	std::set<std::string> seen;
	for( const CameraProperty& r : rows ) seen.insert( r.name.c_str() );

	for( const std::string& n : expected ) {
		Check( seen.count( n ) == 1,
		       ( "standard_object row '" + n + "' surfaced" ).c_str() );
	}

	// Position / orientation / scale / material / shader /
	// casts_shadows / receives_shadows are editable (have writers).
	// geometry / modifier / quaternion / matrix / interior_medium /
	// radiance_* are read-only (construction-time-only).
	for( const CameraProperty& r : rows ) {
		const std::string n( r.name.c_str() );
		if( n == "position" || n == "orientation" || n == "scale"
		 || n == "material" || n == "shader"
		 || n == "casts_shadows" || n == "receives_shadows" )
		{
			Check( r.editable, ( "object row '" + n + "' is editable" ).c_str() );
		}
		if( n == "geometry" || n == "modifier" || n == "quaternion"
		 || n == "matrix"   || n == "interior_medium" )
		{
			Check( !r.editable, ( "object row '" + n + "' is read-only (construction-time)" ).c_str() );
		}
	}

	// `scale` row is DoubleVec3 (per-axis), matching descriptor.
	for( const CameraProperty& r : rows ) {
		if( r.name == String( "scale" ) ) {
			Check( r.kind == ValueKind::DoubleVec3, "scale row kind is DoubleVec3 (descriptor)" );
		}
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 6: Object scale edit dispatches per-axis SetObjectStretch
//////////////////////////////////////////////////////////////////////

static void TestObjectScaleVec3Dispatch()
{
	std::cout << "Test 6: object 'scale' edit routes to SetObjectStretch (per-axis)" << std::endl;

	Job* pJob = MakeJobAndSeed();
	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	Check( c.SetProperty( String( "scale" ), String( "2 3 4" ) ),
	       "SetProperty(scale=2 3 4)" );

	IObjectManager* objs = pJob->GetObjects();
	const IObject* obj = objs->GetItem( "sphere" );
	const Matrix4 m = obj->GetFinalTransformMatrix();
	const double lx = std::sqrt( static_cast<double>( m._00*m._00 + m._01*m._01 + m._02*m._02 ) );
	const double ly = std::sqrt( static_cast<double>( m._10*m._10 + m._11*m._11 + m._12*m._12 ) );
	const double lz = std::sqrt( static_cast<double>( m._20*m._20 + m._21*m._21 + m._22*m._22 ) );
	Check( std::abs( lx - 2.0 ) < 1e-5, "x-stretch = 2" );
	Check( std::abs( ly - 3.0 ) < 1e-5, "y-stretch = 3" );
	Check( std::abs( lz - 4.0 ) < 1e-5, "z-stretch = 4" );
	}
	pJob->release();
}

int main()
{
	std::cout << "SceneEditorPhase4DescriptorDrivenTest" << std::endl;
	std::cout << "=====================================" << std::endl;

	TestLightDescriptorDriven();
	TestSpotLightDescriptorDriven();
	TestLightSetPropertyAcceptsBothVocabularies();
	TestRasterizerDescriptorDriven();
	TestObjectDescriptorDriven();
	TestObjectScaleVec3Dispatch();

	std::cout << std::endl;
	std::cout << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
