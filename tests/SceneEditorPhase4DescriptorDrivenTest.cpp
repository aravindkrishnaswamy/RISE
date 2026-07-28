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
#include <fstream>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <set>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
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
#include "../src/Library/Utilities/FiniteMath.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static Scalar QuietNaNForTest()
{
	const std::uint64_t bits = 0x7FF8000000000000ULL;
	Scalar value = 0;
	std::memcpy( &value, &bits, sizeof( value ) );
	return value;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static bool FiniteForMatrixCompare( const double value )
{
	return RISE::IsFiniteDouble( value );
}

static void Check( bool cond, const char* name )
{
	if( cond ) passCount++;
	else { failCount++; std::cout << "  FAIL: " << name << std::endl; }
}

static bool MatrixClose( const Matrix4& a, const Matrix4& b, double epsilon = 1e-6 )
{
	const Scalar* av = &a._00;
	const Scalar* bv = &b._00;
	for( int i = 0; i < 16; ++i ) {
		const double actual = static_cast<double>( av[i] );
		const double expected = static_cast<double>( bv[i] );
		const double difference = actual - expected;
		if( !FiniteForMatrixCompare( actual ) || !FiniteForMatrixCompare( expected )
		 || !FiniteForMatrixCompare( difference ) || std::fabs( difference ) > epsilon ) return false;
	}
	return true;
}

static double LinearDeterminant( const Matrix4& m )
{
	return static_cast<double>(
		m._00 * ( m._11 * m._22 - m._21 * m._12 )
	  - m._10 * ( m._01 * m._22 - m._21 * m._02 )
	  + m._20 * ( m._01 * m._12 - m._11 * m._02 ) );
}

static Matrix4 NormalizeLinearColumns( const Matrix4& input )
{
	Matrix4 result = input;
	Scalar* columns[3] = { &result._00, &result._10, &result._20 };
	for( int column = 0; column < 3; ++column ) {
		const double length = std::sqrt( static_cast<double>(
			columns[column][0] * columns[column][0]
		  + columns[column][1] * columns[column][1]
		  + columns[column][2] * columns[column][2] ) );
		if( length > 0 ) {
			columns[column][0] /= length;
			columns[column][1] /= length;
			columns[column][2] /= length;
		}
	}
	return result;
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

static Job* LoadOverrideObjectFixture( const std::string& overrides, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof( path ),
		"scene_editor_phase4_override_%s_%d.RISEscene", tag, static_cast<int>( ::getpid() ) );
	std::ofstream scene( path );
	if( !scene.is_open() ) return 0;
	scene <<
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
		"lambertian_material\n{\nname matte\nreflectance white\n}\n"
		"sphere_geometry\n{\nname sphere_geom\nradius 1\n}\n"
		"standard_object\n{\nname matrix_first\ngeometry sphere_geom\nmaterial matte\n}\n"
		"standard_object\n{\nname matrix_once\ngeometry sphere_geom\nmaterial matte\n}\n"
		"standard_object\n{\nname matrix_twice\ngeometry sphere_geom\nmaterial matte\n}\n"
		"standard_object\n{\nname quaternion_object\ngeometry sphere_geom\nmaterial matte\n}\n"
		<< overrides;
	scene.close();

	Job* job = new Job();
	const bool loaded = job->LoadAsciiSceneViaCst( path );
	std::remove( path );
	if( !loaded ) {
		job->release();
		return 0;
	}
	return job;
}

static double LinearColumnLength( const Matrix4& matrix, const int column )
{
	const Scalar* values = &matrix._00 + column * 4;
	return std::sqrt( static_cast<double>(
		values[0] * values[0] + values[1] * values[1] + values[2] * values[2] ) );
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

	// Matrix-authored objects use one authoritative final affine matrix. Public
	// absolute setters (including animation's SetIntermediateValue path) must
	// replace fields of that matrix rather than composing hidden P/O/S state
	// beneath it.
	const Matrix4 authoredMatrix = Matrix4Ops::Translation( Vector3( 5, -2, 3 ) )
		* Matrix4Ops::YRotation( 0.4 )
		* Matrix4Ops::Stretch( Vector3( 2, 3, 4 ) );
	double authoredValues[16];
	const Scalar* authoredSource = &authoredMatrix._00;
	for( int i = 0; i < 16; ++i ) authoredValues[i] = authoredSource[i];
	RadianceMapConfig authoredRMap;
	Check( pJob->AddObjectMatrix(
		"matrix_sphere", "sphere_geom", "lambert_white", nullptr, nullptr,
		authoredRMap, authoredValues, true, true ),
		"matrix-authored setter fixture created" );
	IObjectPriv* matrixObject = pJob->GetObjects()
		? pJob->GetObjects()->GetItem( "matrix_sphere" ) : 0;
	Check( matrixObject != 0, "matrix-authored setter fixture resolves" );
	if( matrixObject ) {
		const Matrix4 beforeMalformedRestore = matrixObject->GetFinalTransformMatrix();
		TransformStateV2 malformedState;
		malformedState.finalMatrixOnStack = true;
		malformedState.finalScaleBaseValid = true;
		Check( !RISE_API_RestoreTransformStateV2( matrixObject, &malformedState ),
		       "public V2 restore rejects an active state with no authoritative entry" );
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), beforeMalformedRestore ),
		       "rejected malformed V2 restore leaves the target unchanged" );
		TransformStateV2 nonFiniteState;
		Check( RISE_API_CaptureTransformStateV2( matrixObject, &nonFiniteState ),
		       "public V2 state captured for non-finite rejection check" );
		nonFiniteState.stackEntries[nonFiniteState.authoritativeStackIndex]._00 = QuietNaNForTest();
		Check( !RISE_API_RestoreTransformStateV2( matrixObject, &nonFiniteState ),
		       "public V2 restore rejects a bit-injected NaN stack entry under fast-math" );
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), beforeMalformedRestore ),
		       "rejected non-finite V2 restore leaves the target unchanged" );

		const Matrix4 localPush = Matrix4Ops::Translation( Vector3( 1, 0, 0 ) );
		matrixObject->PushTopTransStack( localPush );
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), authoredMatrix * localPush ),
		       "matrix-authored PushTop preserves legacy stack order" );
		matrixObject->PopTopTransStack();
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), authoredMatrix ),
		       "matrix-authored PopTop restores the prior matrix" );
		const Matrix4 worldPush = Matrix4Ops::ZRotation( 0.2 );
		matrixObject->PushBottomTransStack( worldPush );
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), worldPush * authoredMatrix ),
		       "matrix-authored PushBottom preserves legacy stack order" );
		matrixObject->PopBottomTransStack();
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), authoredMatrix ),
		       "matrix-authored PopBottom restores the prior matrix" );

		// V2 is representation-exact, not merely final-matrix exact. Preserve
		// non-commuting entries so later endpoint pops keep their public meaning.
		const Matrix4 savedLocal = Matrix4Ops::XRotation( 0.31 );
		const Matrix4 savedWorld = Matrix4Ops::Translation( Vector3( -4, 2, 1 ) );
		matrixObject->PushTopTransStack( savedLocal );
		matrixObject->PushBottomTransStack( savedWorld );
		matrixObject->FinalizeTransformations();
		TransformStateV2 exactStackState;
		Check( RISE_API_CaptureTransformStateV2( matrixObject, &exactStackState ),
		       "V2 captures a multi-entry non-commuting transform stack" );
		matrixObject->PopTopTransStack();
		matrixObject->PopBottomTransStack();
		Check( RISE_API_RestoreTransformStateV2( matrixObject, &exactStackState ),
		       "V2 restores the exact multi-entry stack" );
		matrixObject->PopTopTransStack();
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), savedWorld * authoredMatrix ),
		       "V2 restore preserves PopTop endpoint semantics" );
		Check( RISE_API_RestoreTransformStateV2( matrixObject, &exactStackState ),
		       "V2 stack can be restored repeatedly" );
		matrixObject->PopBottomTransStack();
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), authoredMatrix * savedLocal ),
		       "V2 restore preserves PopBottom endpoint semantics" );

		Check( RISE_API_RestoreTransformStateV2( matrixObject, &exactStackState ),
		       "V2 stack restored before absolute-setter endpoint checks" );
		matrixObject->SetPosition( Point3( 9, 8, 7 ) );
		matrixObject->FinalizeTransformations();
		TransformStateV2 positionedStackState;
		Check( RISE_API_CaptureTransformStateV2( matrixObject, &positionedStackState ),
		       "V2 captures a positioned multi-entry stack" );
		Check( positionedStackState.stackEntries.size() == 3
		    && positionedStackState.authoritativeStackIndex == 1,
		       "SetPosition preserves caller-owned stack entries and authoritative index" );
		Matrix4 positionedAuthoritative = authoredMatrix;
		positionedAuthoritative._30 = 9;
		positionedAuthoritative._31 = 8;
		positionedAuthoritative._32 = 7;
		matrixObject->PopTopTransStack();
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(),
			savedWorld * positionedAuthoritative ),
		       "PopTop after SetPosition removes the caller's local entry" );

		Check( RISE_API_RestoreTransformStateV2( matrixObject, &exactStackState ),
		       "V2 stack restored before orientation endpoint check" );
		matrixObject->SetOrientation( Vector3( 0, 0, 0 ) );
		matrixObject->FinalizeTransformations();
		TransformStateV2 orientedStackState;
		Check( RISE_API_CaptureTransformStateV2( matrixObject, &orientedStackState ),
		       "V2 captures an oriented multi-entry stack" );
		Check( orientedStackState.stackEntries.size() == 3
		    && orientedStackState.authoritativeStackIndex == 1,
		       "SetOrientation preserves caller-owned stack entries and authoritative index" );
		const Matrix4 unrotatedAuthoritative =
			Matrix4Ops::Translation( Vector3( 5, -2, 3 ) )
			* Matrix4Ops::Stretch( Vector3( 2, 3, 4 ) );
		matrixObject->PopBottomTransStack();
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(),
			unrotatedAuthoritative * savedLocal ),
		       "PopBottom after SetOrientation removes the caller's world entry" );
		Check( RISE_API_RestoreTransformStateV2( matrixObject, &exactStackState ),
		       "V2 stack restored for cleanup" );
		matrixObject->PopTopTransStack();
		matrixObject->PopBottomTransStack();
		matrixObject->FinalizeTransformations();

		// A collapsed scale axis must not discard its recoverable base when a
		// no-op stack entry is pushed and popped.
		matrixObject->SetStretch( Vector3( 0, 3, 4 ) );
		matrixObject->PushTopTransStack( Matrix4Ops::Identity() );
		matrixObject->PopTopTransStack();
		matrixObject->SetStretch( Vector3( 2, 3, 4 ) );
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), authoredMatrix ),
		       "zero-scale base survives an identity PushTop/PopTop round-trip" );

		matrixObject->SetPosition( Point3( 9, 8, 7 ) );
		matrixObject->FinalizeTransformations();
		Matrix4 repositioned = authoredMatrix;
		repositioned._30 = 9; repositioned._31 = 8; repositioned._32 = 7;
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), repositioned ),
		       "public SetPosition replaces matrix translation without composing" );

		IKeyframeParameter* positionSample = matrixObject->KeyframeFromParameters(
			String( "position" ), String( "9 8 7" ) );
		Check( positionSample != 0, "position keyframe parses" );
		if( positionSample ) {
			matrixObject->SetIntermediateValue( *positionSample );
			matrixObject->RegenerateData();
			matrixObject->SetIntermediateValue( *positionSample );
			matrixObject->RegenerateData();
			TransformStateV2 repeatedPositionState;
			Check( RISE_API_CaptureTransformStateV2( matrixObject, &repeatedPositionState ),
			       "repeated position-keyframe state can be captured" );
			Check( repeatedPositionState.stackEntries.size() == 1,
			       "repeated absolute position samples do not grow the transform stack" );
			matrixObject->PopBottomTransStack();
			matrixObject->FinalizeTransformations();
			Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), Matrix4Ops::Identity() ),
			       "PopBottom after absolute SetPosition sees no hidden correction entry" );
			Check( RISE_API_RestoreTransformStateV2( matrixObject, &repeatedPositionState ),
			       "position state restores after endpoint-pop regression" );
			safe_release( positionSample );
		}

		Matrix4 authoredRotation, authoredResidual;
		ObjectIntrospection::DecomposeFinalAffine(
			matrixObject->GetFinalTransformMatrix(), authoredRotation, authoredResidual );
		matrixObject->SetOrientation( Vector3( 0, 0, 0 ) );
		matrixObject->FinalizeTransformations();
		const Matrix4 expectedUnrotated = Matrix4Ops::Translation( Vector3( 9, 8, 7 ) )
			* authoredResidual;
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), expectedUnrotated ),
		       "public SetOrientation replaces rotation and preserves affine residual" );

		const Matrix4 scaleBase = NormalizeLinearColumns( expectedUnrotated );
		matrixObject->SetStretch( Vector3( 5, 6, 7 ) );
		matrixObject->FinalizeTransformations();
		Check( MatrixClose( matrixObject->GetFinalTransformMatrix(),
			scaleBase * Matrix4Ops::Stretch( Vector3( 5, 6, 7 ) ) ),
		       "public SetStretch replaces matrix column magnitudes without composing" );

		IKeyframeParameter* signedScale = matrixObject->KeyframeFromParameters(
			String( "scale" ), String( "-5 6 7" ) );
		Check( signedScale != 0, "signed scale keyframe parses" );
		if( signedScale ) {
			matrixObject->SetIntermediateValue( *signedScale );
			matrixObject->RegenerateData();
			const Matrix4 firstSignedSample = matrixObject->GetFinalTransformMatrix();
			matrixObject->SetIntermediateValue( *signedScale );
			matrixObject->RegenerateData();
			Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), firstSignedSample ),
			       "repeated signed scale keyframe samples are absolute and idempotent" );

			matrixObject->SetPosition( Point3(
				firstSignedSample._30, firstSignedSample._31, firstSignedSample._32 ) );
			matrixObject->SetIntermediateValue( *signedScale );
			matrixObject->RegenerateData();
			Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), firstSignedSample ),
			       "signed scale remains idempotent after an absolute position sample" );
			matrixObject->SetOrientation( Vector3( 0, 0, 0 ) );
			matrixObject->RegenerateData();
			const Matrix4 signedAfterOrientation = matrixObject->GetFinalTransformMatrix();
			matrixObject->SetIntermediateValue( *signedScale );
			matrixObject->RegenerateData();
			Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), signedAfterOrientation ),
			       "signed scale remains idempotent after an absolute orientation sample" );

			matrixObject->TranslateObject( Vector3( 1, 2, 3 ) );
			matrixObject->FinalizeTransformations();
			const Matrix4 translatedSignedSample = matrixObject->GetFinalTransformMatrix();
			matrixObject->SetIntermediateValue( *signedScale );
			matrixObject->RegenerateData();
			Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), translatedSignedSample ),
			       "signed scale remains idempotent after a world-stack translation" );

			TransformStateV2 signedState;
			const bool captured = RISE_API_CaptureTransformStateV2( matrixObject, &signedState );
			Check( captured, "public API captures complete V2 transform snapshot" );
			if( captured ) {
				matrixObject->SetStretch( Vector3( 1, 1, 1 ) );
				matrixObject->FinalizeTransformations();
				Check( RISE_API_RestoreTransformStateV2( matrixObject, &signedState ),
				       "public API restores complete V2 transform snapshot" );
				Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), translatedSignedSample ),
				       "V2 undo snapshot restores signed authoritative metadata exactly" );
				matrixObject->SetIntermediateValue( *signedScale );
				matrixObject->RegenerateData();
				Check( MatrixClose( matrixObject->GetFinalTransformMatrix(), translatedSignedSample ),
				       "signed keyframe remains idempotent after V2 restore" );
			}
			safe_release( signedScale );
		}
	}

	const Matrix4 zeroScaleMatrix = Matrix4Ops::Translation( Vector3( -3, 4, 2 ) )
		* Matrix4Ops::Stretch( Vector3( 0, 0, 0 ) );
	double zeroScaleValues[16];
	const Scalar* zeroScaleSource = &zeroScaleMatrix._00;
	for( int i = 0; i < 16; ++i ) zeroScaleValues[i] = zeroScaleSource[i];
	RadianceMapConfig zeroScaleRMap;
	Check( pJob->AddObjectMatrix(
		"zero_matrix_sphere", "sphere_geom", "lambert_white", nullptr, nullptr,
		zeroScaleRMap, zeroScaleValues, true, true ),
		"rank-zero matrix setter fixture created" );
	IObjectPriv* zeroScaleObject = pJob->GetObjects()
		? pJob->GetObjects()->GetItem( "zero_matrix_sphere" ) : 0;
	Check( zeroScaleObject != 0, "rank-zero matrix setter fixture resolves" );
	if( zeroScaleObject ) {
		zeroScaleObject->SetOrientation( Vector3( 0, 0.5, 0 ) );
		zeroScaleObject->SetStretch( Vector3( 1, 1, 1 ) );
		zeroScaleObject->FinalizeTransformations();
		const Matrix4 expectedRevived = Matrix4Ops::Translation( Vector3( -3, 4, 2 ) )
			* Matrix4Ops::YRotation( 0.5 );
		Check( MatrixClose( zeroScaleObject->GetFinalTransformMatrix(), expectedRevived ),
		       "orientation set at rank zero survives a later non-zero stretch" );
	}
	IObjectPriv* emptyPopObject = pJob->GetObjects()
		? pJob->GetObjects()->GetItem( "sphere" ) : 0;
	if( emptyPopObject ) {
		emptyPopObject->ClearTransformStack();
		emptyPopObject->PopTopTransStack();
		emptyPopObject->PopBottomTransStack();
		emptyPopObject->FinalizeTransformations();
		Check( MatrixClose( emptyPopObject->GetFinalTransformMatrix(), Matrix4Ops::Identity() ),
		       "popping either end of an empty transform stack is a safe no-op" );
	}
	IObjectPriv* editable = pJob->GetObjects() ? pJob->GetObjects()->GetItem( "sphere" ) : 0;
	Check( editable != 0, "editable imported-transform object resolves" );
	Matrix4 imported = Matrix4Ops::Identity();
	if( editable ) {
		Matrix4 shear = Matrix4Ops::Identity();
		shear._10 = 0.25;
		imported = Matrix4Ops::Translation( Vector3( 2, 1, -3 ) )
			* Matrix4Ops::YRotation( 0.6 )
			* shear
			* Matrix4Ops::Stretch( Vector3( -10, 6, 4 ) );
		editable->ClearAllTransforms();
		editable->PushTopTransStack( imported );
		editable->FinalizeTransformations();
		const std::vector<CameraProperty> importedRows = ObjectIntrospection::Inspect(
			String( "sphere" ), *editable );
		for( const CameraProperty& row : importedRows ) {
			if( row.name == String( "orientation" ) ) {
				Check( row.value != String( "0 0 0" ),
				       "orientation inspector reflects imported final rotation" );
			}
		}
	}

	Check( c.SetProperty( String( "scale" ), String( "2 3 4" ) ),
	       "SetProperty(scale=2 3 4)" );

	IObjectManager* objs = pJob->GetObjects();
	const IObject* obj = objs->GetItem( "sphere" );
	const Matrix4 m = obj->GetFinalTransformMatrix();
	const Matrix4 expectedScaled = NormalizeLinearColumns( imported )
		* Matrix4Ops::Stretch( Vector3( 2, 3, 4 ) );
	Check( MatrixClose( m, expectedScaled ),
	       "absolute scale produces the complete expected imported affine matrix" );
	const double lx = std::sqrt( static_cast<double>( m._00*m._00 + m._01*m._01 + m._02*m._02 ) );
	const double ly = std::sqrt( static_cast<double>( m._10*m._10 + m._11*m._11 + m._12*m._12 ) );
	const double lz = std::sqrt( static_cast<double>( m._20*m._20 + m._21*m._21 + m._22*m._22 ) );
	Check( std::abs( lx - 2.0 ) < 1e-5, "x-stretch = 2" );
	Check( std::abs( ly - 3.0 ) < 1e-5, "y-stretch = 3" );
	Check( std::abs( lz - 4.0 ) < 1e-5, "z-stretch = 4" );
	Check( std::abs( static_cast<double>( m._30 ) - 2.0 ) < 1e-6
	    && std::abs( static_cast<double>( m._31 ) - 1.0 ) < 1e-6
	    && std::abs( static_cast<double>( m._32 ) + 3.0 ) < 1e-6,
	       "absolute scale preserves imported world position" );
	Check( c.Editor().Undo(), "absolute scale undo succeeds" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(), imported ),
	       "absolute scale undo restores the exact imported matrix" );
	Check( c.Editor().Redo(), "absolute scale redo succeeds" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(), expectedScaled ),
	       "absolute scale redo restores the complete expected matrix" );

	Check( c.SetProperty( String( "orientation" ), String( "0 0 0" ) ),
	       "SetProperty(orientation=0 0 0) clears imported world rotation" );
	const Matrix4 oriented = obj->GetFinalTransformMatrix();
	Matrix4 priorRotation, priorResidual;
	ObjectIntrospection::DecomposeFinalAffine( m, priorRotation, priorResidual );
	const Matrix4 expectedOriented = Matrix4Ops::Translation( Vector3( 2, 1, -3 ) )
		* priorResidual;
	Check( MatrixClose( oriented, expectedOriented ),
	       "absolute orientation replaces rotation but preserves affine residual" );
	Check( LinearDeterminant( oriented ) < 0,
	       "absolute orientation preserves reflection handedness" );
	Check( c.Editor().Undo(), "absolute orientation undo succeeds" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(), m ),
	       "absolute orientation undo restores the exact sheared/reflected input" );
	Check( c.Editor().Redo(), "absolute orientation redo succeeds" );
	const Matrix4 redone = obj->GetFinalTransformMatrix();
	Check( MatrixClose( redone, expectedOriented ),
	       "absolute orientation redo restores the exact affine result" );

	// A zero column must be revived along the inferred rotated frame, not a
	// possibly parallel world axis.
	if( editable ) {
		const Matrix4 collapsed = Matrix4Ops::ZRotation( PI / 2.0 )
			* Matrix4Ops::Stretch( Vector3( 0, 2, 3 ) );
		editable->ClearAllTransforms();
		editable->PushTopTransStack( collapsed );
		editable->FinalizeTransformations();
	}
	Check( c.SetProperty( String( "scale" ), String( "1 2 3" ) ),
	       "collapsed rotated X scale can be restored" );
	const Matrix4 revived = obj->GetFinalTransformMatrix();
	const Matrix4 expectedRevived = Matrix4Ops::ZRotation( PI / 2.0 )
		* Matrix4Ops::Stretch( Vector3( 1, 2, 3 ) );
	Check( MatrixClose( revived, expectedRevived ),
	       "revived rotated transform preserves the complete expected frame" );
	Check( c.Editor().Undo(), "collapsed-axis revival undo succeeds" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(),
		Matrix4Ops::ZRotation( PI / 2.0 )
			* Matrix4Ops::Stretch( Vector3( 0, 2, 3 ) ) ),
	       "collapsed-axis revival undo restores the exact singular matrix" );
	Check( c.Editor().Redo(), "collapsed-axis revival redo succeeds" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(), expectedRevived ),
	       "collapsed-axis revival redo restores the complete expected frame" );

	// Interactive absolute scale is a persistable magnitude. A zero destroys
	// the affine frame, while a negative sign is ambiguous once a CST scene
	// serializes the net matrix and re-derives it. Reject both before mutation;
	// scene-authored matrices remain free to contain either.
	const Matrix4 beforeRejectedScale = obj->GetFinalTransformMatrix();
	Check( !c.SetProperty( String( "scale" ), String( "0 2 3" ) ),
	       "zero per-axis absolute scale is rejected" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(), beforeRejectedScale ),
	       "rejected zero scale leaves the full transform unchanged" );
	Check( !c.SetProperty( String( "scale" ), String( "1 -2 3" ) ),
	       "negative per-axis absolute scale is rejected" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(), beforeRejectedScale ),
	       "rejected negative scale leaves the full transform unchanged" );
	Check( !c.SetProperty( String( "scale_uniform" ), String( "0" ) ),
	       "zero uniform absolute scale is rejected" );
	Check( !c.SetProperty( String( "stretch" ), String( "1 2 -3" ) ),
	       "legacy stretch alias also rejects negative scale" );
	Check( !c.SetProperty( String( "scale" ), String( "nan 2 3" ) ),
	       "non-finite per-axis scale is rejected" );
	Check( !c.SetProperty( String( "scale_uniform" ), String( "inf" ) ),
	       "non-finite uniform scale is rejected" );
	Check( !c.SetProperty( String( "position" ), String( "1 2 nan" ) ),
	       "non-finite position is rejected" );
	Check( !c.SetProperty( String( "orientation" ), String( "0 inf 0" ) ),
	       "non-finite orientation is rejected" );
	Check( !c.SetProperty( String( "scale" ), String( "1 2 3 trailing" ) ),
	       "scale parser rejects trailing junk" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(), beforeRejectedScale ),
	       "all rejected scale/pose text leaves the full transform unchanged" );

	// Exact gimbal lock must inspect to an equivalent Euler representative.
	const Matrix4 gimbal = Matrix4Ops::XRotation( 0.3 )
		* Matrix4Ops::YRotation( PI / 2.0 )
		* Matrix4Ops::ZRotation( 0.4 )
		* Matrix4Ops::Stretch( Vector3( 2, 3, 4 ) );
	if( editable ) {
		editable->ClearAllTransforms();
		editable->PushTopTransStack( gimbal );
		editable->FinalizeTransformations();
	}
	String inspectedOrientation;
	const std::vector<CameraProperty> gimbalRows = ObjectIntrospection::Inspect(
		String( "sphere" ), *obj );
	for( const CameraProperty& row : gimbalRows )
		if( row.name == String( "orientation" ) ) inspectedOrientation = row.value;
	Check( inspectedOrientation.size() > 1, "gimbal orientation is inspectable" );
	Check( c.SetProperty( String( "orientation" ), inspectedOrientation ),
	       "inspected gimbal orientation reapplies" );
	Check( MatrixClose( obj->GetFinalTransformMatrix(), gimbal, 1e-5 ),
	       "gimbal inspect/edit round-trip preserves the matrix" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 7: override_object parser preserves authoritative matrix state
//////////////////////////////////////////////////////////////////////

static void TestOverrideObjectTransformParsing()
{
	std::cout << "Test 7: override_object matrix/quaternion and partial overrides" << std::endl;
	const std::string matrixSetup =
		"override_object\n{\nname matrix_first\nmatrix -2 0 0 0 0 3 0 0 0 0 4 0 1 2 3 1\n}\n"
		"override_object\n{\nname matrix_first\nposition 8 9 10\n}\n"
		"override_object\n{\nname matrix_first\norientation 0 0 0\n}\n"
		"override_object\n{\nname matrix_first\nscale -5 6 7\n}\n"
		"override_object\n{\nname matrix_once\nmatrix -2 0 0 0 0 3 0 0 0 0 4 0 1 2 3 1\n}\n"
		"override_object\n{\nname matrix_once\nposition 8 9 10\n}\n"
		"override_object\n{\nname matrix_once\norientation 0 0 0\n}\n"
		"override_object\n{\nname matrix_once\nscale -5 6 7\n}\n"
		"override_object\n{\nname matrix_once\nscale -8 9 10\n}\n"
		"override_object\n{\nname matrix_twice\nmatrix -2 0 0 0 0 3 0 0 0 0 4 0 1 2 3 1\n}\n"
		"override_object\n{\nname matrix_twice\nposition 8 9 10\n}\n"
		"override_object\n{\nname matrix_twice\norientation 0 0 0\n}\n"
		"override_object\n{\nname matrix_twice\nscale -5 6 7\n}\n"
		"override_object\n{\nname matrix_twice\nscale -8 9 10\n}\n"
		"override_object\n{\nname matrix_twice\nscale -8 9 10\n}\n"
		"override_object\n{\nname quaternion_object\nposition 4 5 6\nquaternion 0 0 0.7071067811865476 0.7071067811865476\nscale 2 3 4\n}\n"
		"override_object\n{\nname quaternion_object\nposition 7 8 9\n}\n";
	Job* job = LoadOverrideObjectFixture( matrixSetup, "valid" );
	Check( job != 0, "override_object matrix/quaternion fixture loads" );
	if( job ) {
		IObjectManager* objects = job->GetObjects();
		IObjectPriv* first = objects ? objects->GetItem( "matrix_first" ) : 0;
		IObjectPriv* once = objects ? objects->GetItem( "matrix_once" ) : 0;
		IObjectPriv* twice = objects ? objects->GetItem( "matrix_twice" ) : 0;
		IObjectPriv* quaternion = objects ? objects->GetItem( "quaternion_object" ) : 0;
		Check( first && once && twice && quaternion, "all override targets resolve" );
		if( first && once && twice ) {
			const Matrix4 firstMatrix = first->GetFinalTransformMatrix();
			const Matrix4 onceMatrix = once->GetFinalTransformMatrix();
			const Matrix4 twiceMatrix = twice->GetFinalTransformMatrix();
			Check( std::fabs( LinearColumnLength( firstMatrix, 0 ) - 5.0 ) < 1e-6
			    && std::fabs( LinearColumnLength( firstMatrix, 1 ) - 6.0 ) < 1e-6
			    && std::fabs( LinearColumnLength( firstMatrix, 2 ) - 7.0 ) < 1e-6,
			       "first signed scale override is absolute" );
			Check( std::fabs( LinearColumnLength( onceMatrix, 0 ) - 8.0 ) < 1e-6
			    && std::fabs( LinearColumnLength( onceMatrix, 1 ) - 9.0 ) < 1e-6
			    && std::fabs( LinearColumnLength( onceMatrix, 2 ) - 10.0 ) < 1e-6,
			       "second signed scale override wins without accumulation" );
			Check( MatrixClose( NormalizeLinearColumns( firstMatrix ),
			                   NormalizeLinearColumns( onceMatrix ) ),
			       "sequential signed scale overrides preserve the affine base" );
			Check( MatrixClose( onceMatrix, twiceMatrix ),
			       "repeating the final signed override is idempotent" );
			Check( std::fabs( static_cast<double>( onceMatrix._30 ) - 8.0 ) < 1e-6
			    && std::fabs( static_cast<double>( onceMatrix._31 ) - 9.0 ) < 1e-6
			    && std::fabs( static_cast<double>( onceMatrix._32 ) - 10.0 ) < 1e-6,
			       "partial position override replaces matrix translation" );
		}
		if( quaternion ) {
			const Matrix4 matrix = quaternion->GetFinalTransformMatrix();
			Check( std::fabs( LinearColumnLength( matrix, 0 ) - 2.0 ) < 1e-6
			    && std::fabs( LinearColumnLength( matrix, 1 ) - 3.0 ) < 1e-6
			    && std::fabs( LinearColumnLength( matrix, 2 ) - 4.0 ) < 1e-6,
			       "quaternion override preserves authored per-axis scale" );
			Check( std::fabs( static_cast<double>( matrix._30 ) - 7.0 ) < 1e-6
			    && std::fabs( static_cast<double>( matrix._31 ) - 8.0 ) < 1e-6
			    && std::fabs( static_cast<double>( matrix._32 ) - 9.0 ) < 1e-6,
			       "partial position override follows quaternion override" );
		}
		job->release();
	}

	Job* nonFinite = LoadOverrideObjectFixture(
		"override_object\n{\nname matrix_first\nscale 1 nan 3\n}\n", "nonfinite" );
	Check( nonFinite == 0, "override_object rejects non-finite descriptor values" );
	if( nonFinite ) nonFinite->release();
	Job* malformed = LoadOverrideObjectFixture(
		"override_object\n{\nname matrix_first\nmatrix 1 0 0\n}\n", "arity" );
	Check( malformed == 0, "override_object rejects malformed matrix arity" );
	if( malformed ) malformed->release();
}

int main()
{
	std::cout << "SceneEditorPhase4DescriptorDrivenTest" << std::endl;
	std::cout << "=====================================" << std::endl;
	Matrix4 nonFinite = Matrix4Ops::Identity();
	nonFinite._00 = QuietNaNForTest();
	Check( !MatrixClose( nonFinite, nonFinite ), "matrix comparisons reject NaN entries" );

	TestLightDescriptorDriven();
	TestSpotLightDescriptorDriven();
	TestLightSetPropertyAcceptsBothVocabularies();
	TestRasterizerDescriptorDriven();
	TestObjectDescriptorDriven();
	TestObjectScaleVec3Dispatch();
	TestOverrideObjectTransformParsing();

	std::cout << std::endl;
	std::cout << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
