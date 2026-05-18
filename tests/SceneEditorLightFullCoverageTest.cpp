//////////////////////////////////////////////////////////////////////
//
//  SceneEditorLightFullCoverageTest.cpp - Full editing coverage for
//    every light type × every parser parameter, with round-trip,
//    undo/redo, and invalid-input rejection.
//
//  Complements SceneEditorPhase3EditingTest's spot-checks with an
//  exhaustive matrix that locks down the descriptor-driven edit
//  contract: the GUI sends CHUNK vocabulary (power / inner / outer /
//  shootphotons), the dispatch in SceneEditor.cpp translates to
//  KEYFRAME vocabulary (energy / inner_angle / outer_angle) via
//  ChunkNameToKeyframeName, and per-type virtuals stage the new value
//  through SetIntermediateValue + RegenerateData.  shootphotons is
//  the one exception — it bypasses the keyframe pipeline and dispatches
//  to ILight::SetCanGeneratePhotons directly.
//
//  Catches regressions in:
//   - chunk↔keyframe translation table drift
//   - per-light KeyframeFromParameters / SetIntermediateValue ID
//     allocator-vs-consumer mismatch (the AmbientLight 1000/100 bug)
//   - undo round-trip — ReadLightProperty must cover every editable
//     param or the prev value gets captured as empty and undo silently
//     drops the entry
//   - shootphotons editability gate — Point/Spot accept the toggle,
//     Ambient/Directional hard-code false and SetCanGeneratePhotons
//     is a no-op (override absent) so the chunk descriptor for those
//     types intentionally OMITS shootphotons from its parameter list,
//     and SetProperty("shootphotons") on Ambient/Directional should
//     not even be reachable from the panel — but if a programmatic
//     caller tries it, behaviour stays safe (the apply succeeds but
//     CanGeneratePhotons() still returns false).
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstring>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ILight.h"
#include "../src/Library/Interfaces/ILightPriv.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) passCount++;
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

// Minimal Job factory: pinhole camera + default shader, no objects.
// Light edit tests don't need a sphere/material to exist.
static Job* MakeMinimalJob()
{
	Job* job = new Job();

	ICamera* pCam = nullptr;
	if( RISE_API_CreatePinholeCamera(
		&pCam,
		Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		Scalar( 0.785398 ), 64, 64,
		Scalar( 1 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ),
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) ) )
	{
		job->GetScene()->AddCamera( "default", pCam );
		pCam->release();
	}
	const char* shaderOps[] = { "DefaultDirectLighting" };
	job->AddStandardShader( "global", 1, shaderOps );
	return job;
}

//////////////////////////////////////////////////////////////////////
// AmbientLight: parser params (power, color).  shootphotons not in
// ambient_light descriptor → not surfaced → not tested as a chunk
// dispatch.  Regression for the kColorID/kEnergyID mismatch that used
// to silently no-op every ambient edit while reporting success.
//////////////////////////////////////////////////////////////////////

static void TestAmbientLightFullMatrix()
{
	std::cout << "Test: AmbientLight full edit matrix (chunk vocabulary)" << std::endl;

	Job* pJob = MakeMinimalJob();
	double col[3] = { 0.5, 0.5, 0.5 };
	pJob->AddAmbientLight( "amb", 2.0, col );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "amb" ) );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "amb" );

	// -- power round-trip + undo + redo --
	const Scalar e0 = light->emissionEnergy();
	Check( c.SetProperty( String( "power" ), String( "3.7" ) ),
	       "amb SetProperty(power) returns true" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 3.7 ) < 1e-6,
	       "amb power applied (regression: ID mismatch would no-op)" );
	Check( c.Editor().Undo(), "amb power undo returns true" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() - e0 ) ) < 1e-6,
	       "amb power restored after undo" );
	Check( c.Editor().Redo(), "amb power redo returns true" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 3.7 ) < 1e-6,
	       "amb power reapplied after redo" );
	Check( c.Editor().Undo(), "amb power undo (cleanup before color test)" );

	// -- color round-trip + undo --
	const RISEPel c0 = light->emissionColor();
	Check( c.SetProperty( String( "color" ), String( "0.8 0.4 0.1" ) ),
	       "amb SetProperty(color) returns true" );
	Check( std::abs( static_cast<double>( light->emissionColor().r ) - 0.8 ) < 1e-6,
	       "amb color.r applied" );
	Check( std::abs( static_cast<double>( light->emissionColor().g ) - 0.4 ) < 1e-6,
	       "amb color.g applied" );
	Check( std::abs( static_cast<double>( light->emissionColor().b ) - 0.1 ) < 1e-6,
	       "amb color.b applied" );
	Check( c.Editor().Undo(), "amb color undo" );
	Check( std::abs( static_cast<double>( light->emissionColor().r - c0.r ) ) < 1e-6,
	       "amb color.r restored after undo" );
	Check( c.Editor().Redo(), "amb color redo" );
	Check( std::abs( static_cast<double>( light->emissionColor().r ) - 0.8 ) < 1e-6,
	       "amb color.r reapplied after redo" );

	// -- invalid value rejected (sscanf fails, KeyframeFromParameters
	//    falls through to Transformable which doesn't know "color",
	//    returns null, Apply returns false) --
	Check( !c.SetProperty( String( "color" ), String( "not-a-vec3" ) ),
	       "amb invalid color rejected with false return" );

	// -- invalid scalar rejected (strict parse: "garbage" / "nan" /
	//    "inf" must NOT silently flow into light state as 0 / NaN /
	//    ±inf via the prior atof() path) --
	const Scalar guardEnergy = light->emissionEnergy();
	Check( !c.SetProperty( String( "power" ), String( "garbage" ) ),
	       "amb invalid power rejected (was: atof returned 0)" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() - guardEnergy ) ) < 1e-9,
	       "amb power unchanged after rejected garbage edit" );
	Check( !c.SetProperty( String( "power" ), String( "nan" ) ),
	       "amb NaN power rejected (was: atof returned NaN)" );
	Check( !c.SetProperty( String( "power" ), String( "inf" ) ),
	       "amb inf power rejected (was: atof returned +inf)" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() - guardEnergy ) ) < 1e-9,
	       "amb power still unchanged after all rejections" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// PointLight: parser params (power, position, color, shootphotons).
// position is dispatched through Transformable::KeyframeFromParameters,
// stored in m_kfPosition, then PointLight::FinalizeTransformations
// recomputes ptPosition from m_mxFinalTrans during RegenerateData.
//////////////////////////////////////////////////////////////////////

static void TestPointLightFullMatrix()
{
	std::cout << "Test: PointLight full edit matrix (chunk vocabulary)" << std::endl;

	Job* pJob = MakeMinimalJob();
	double pos[3] = { 1, 2, 3 };
	double col[3] = { 1, 1, 1 };
	pJob->AddPointOmniLight( "key", 1.0, col, pos, true );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "key" ) );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "key" );

	// -- power --
	Check( c.SetProperty( String( "power" ), String( "2.5" ) ), "pt power set" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 2.5 ) < 1e-6,
	       "pt power applied" );
	Check( c.Editor().Undo(), "pt power undo" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 1.0 ) < 1e-6,
	       "pt power restored" );

	// -- position (Transformable fall-through round-trip) --
	const Point3 p0 = light->position();
	Check( c.SetProperty( String( "position" ), String( "10 20 30" ) ), "pt position set" );
	const Point3 p1 = light->position();
	Check( std::abs( static_cast<double>( p1.x ) - 10.0 ) < 1e-6,
	       "pt position.x applied (Transformable→FinalizeTransformations round-trip)" );
	Check( std::abs( static_cast<double>( p1.y ) - 20.0 ) < 1e-6, "pt position.y applied" );
	Check( std::abs( static_cast<double>( p1.z ) - 30.0 ) < 1e-6, "pt position.z applied" );
	Check( c.Editor().Undo(), "pt position undo" );
	const Point3 p2 = light->position();
	Check( std::abs( static_cast<double>( p2.x - p0.x ) ) < 1e-6, "pt position.x restored" );
	Check( c.Editor().Redo(), "pt position redo" );
	const Point3 p3 = light->position();
	Check( std::abs( static_cast<double>( p3.x ) - 10.0 ) < 1e-6,
	       "pt position.x reapplied after redo" );

	// -- color --
	Check( c.SetProperty( String( "color" ), String( "0.3 0.6 0.9" ) ), "pt color set" );
	Check( std::abs( static_cast<double>( light->emissionColor().g ) - 0.6 ) < 1e-6,
	       "pt color.g applied" );
	Check( c.Editor().Undo(), "pt color undo" );

	// -- shootphotons (special dispatch path through SetCanGeneratePhotons) --
	const bool s0 = light->CanGeneratePhotons();
	Check( c.SetProperty( String( "shootphotons" ), String( s0 ? "false" : "true" ) ),
	       "pt shootphotons set" );
	Check( light->CanGeneratePhotons() != s0, "pt shootphotons toggled" );
	Check( c.Editor().Undo(), "pt shootphotons undo" );
	Check( light->CanGeneratePhotons() == s0, "pt shootphotons restored" );

	// -- invalid bool rejected --
	Check( !c.SetProperty( String( "shootphotons" ), String( "maybe" ) ),
	       "pt invalid shootphotons rejected" );

	// -- invalid vec3 rejected --
	Check( !c.SetProperty( String( "position" ), String( "not a vec" ) ),
	       "pt invalid position rejected" );

	// -- invalid scalar rejected (strict parse on power) --
	const Scalar guardEnergy = light->emissionEnergy();
	Check( !c.SetProperty( String( "power" ), String( "garbage" ) ),
	       "pt invalid power rejected (was: atof returned 0)" );
	Check( !c.SetProperty( String( "power" ), String( "nan" ) ),
	       "pt NaN power rejected" );
	Check( !c.SetProperty( String( "power" ), String( "inf" ) ),
	       "pt inf power rejected" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() - guardEnergy ) ) < 1e-9,
	       "pt power unchanged after invalid-scalar rejections" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// SpotLight: parser params (power, inner, outer, position, target,
// color, shootphotons).  Chunk vocabulary uses "inner"/"outer" — the
// keyframe IDs are "inner_angle"/"outer_angle" and the translation
// happens in ChunkNameToKeyframeName.
//////////////////////////////////////////////////////////////////////

static void TestSpotLightFullMatrix()
{
	std::cout << "Test: SpotLight full edit matrix (chunk vocabulary)" << std::endl;

	Job* pJob = MakeMinimalJob();
	const double DEG = static_cast<double>( PI ) / 180.0;
	double pos[3] = { 1, 2, 3 };
	double tgt[3] = { 4, 5, 6 };
	double col[3] = { 1, 1, 1 };
	pJob->AddPointSpotLight( "spot", 1.0, col, tgt, 30.0 * DEG, 60.0 * DEG, pos, true );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "spot" ) );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "spot" );

	// -- power --
	Check( c.SetProperty( String( "power" ), String( "4" ) ), "spot power set" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 4.0 ) < 1e-6,
	       "spot power applied" );
	Check( c.Editor().Undo(), "spot power undo" );

	// -- inner (chunk → inner_angle keyframe) --
	Check( c.SetProperty( String( "inner" ), String( "20" ) ), "spot inner set" );
	Check( std::abs( static_cast<double>( light->emissionInnerAngle() ) - 20.0 * DEG ) < 1e-4,
	       "spot 'inner' translated to 'inner_angle' and applied as radians" );
	Check( c.Editor().Undo(), "spot inner undo" );

	// -- outer (chunk → outer_angle keyframe) --
	Check( c.SetProperty( String( "outer" ), String( "75" ) ), "spot outer set" );
	Check( std::abs( static_cast<double>( light->emissionOuterAngle() ) - 75.0 * DEG ) < 1e-4,
	       "spot 'outer' translated to 'outer_angle' and applied as radians" );
	Check( c.Editor().Undo(), "spot outer undo" );

	// -- position --
	Check( c.SetProperty( String( "position" ), String( "100 200 300" ) ), "spot position set" );
	const Point3 newPos = light->position();
	Check( std::abs( static_cast<double>( newPos.x ) - 100.0 ) < 1e-4,
	       "spot position applied via Transformable" );
	Check( c.Editor().Undo(), "spot position undo" );

	// -- target --
	Check( c.SetProperty( String( "target" ), String( "50 60 70" ) ), "spot target set" );
	Check( std::abs( static_cast<double>( light->emissionTarget().x ) - 50.0 ) < 1e-4,
	       "spot target.x applied" );
	Check( c.Editor().Undo(), "spot target undo" );
	Check( std::abs( static_cast<double>( light->emissionTarget().x ) - 4.0 ) < 1e-4,
	       "spot target.x restored" );

	// -- color --
	Check( c.SetProperty( String( "color" ), String( "0.1 0.2 0.3" ) ), "spot color set" );
	Check( c.Editor().Undo(), "spot color undo" );

	// -- invalid vec3 inputs rejected for every vec3 slot --
	Check( !c.SetProperty( String( "target" ), String( "bad target" ) ),
	       "spot invalid target rejected" );
	Check( !c.SetProperty( String( "position" ), String( "x y z" ) ),
	       "spot invalid position rejected" );
	Check( !c.SetProperty( String( "color" ), String( "garbage" ) ),
	       "spot invalid color rejected" );

	// -- shootphotons --
	const bool s0 = light->CanGeneratePhotons();
	Check( c.SetProperty( String( "shootphotons" ), String( s0 ? "false" : "true" ) ),
	       "spot shootphotons set" );
	Check( light->CanGeneratePhotons() != s0, "spot shootphotons toggled" );
	Check( c.Editor().Undo(), "spot shootphotons undo" );
	Check( light->CanGeneratePhotons() == s0, "spot shootphotons restored" );
	Check( c.Editor().Redo(), "spot shootphotons redo" );
	Check( light->CanGeneratePhotons() != s0, "spot shootphotons re-toggled" );

	// -- invalid scalar rejected on every scalar slot --
	const Scalar guardEnergy = light->emissionEnergy();
	const Scalar guardInner = light->emissionInnerAngle();
	const Scalar guardOuter = light->emissionOuterAngle();
	Check( !c.SetProperty( String( "power" ), String( "garbage" ) ),
	       "spot invalid power rejected" );
	Check( !c.SetProperty( String( "power" ), String( "nan" ) ),
	       "spot NaN power rejected" );
	Check( !c.SetProperty( String( "inner" ), String( "garbage" ) ),
	       "spot invalid inner rejected" );
	Check( !c.SetProperty( String( "inner" ), String( "inf" ) ),
	       "spot inf inner rejected" );
	Check( !c.SetProperty( String( "outer" ), String( "junk" ) ),
	       "spot invalid outer rejected" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() - guardEnergy ) ) < 1e-9,
	       "spot power unchanged after invalid-scalar rejections" );
	Check( std::abs( static_cast<double>( light->emissionInnerAngle() - guardInner ) ) < 1e-9,
	       "spot inner_angle unchanged after invalid-scalar rejections" );
	Check( std::abs( static_cast<double>( light->emissionOuterAngle() - guardOuter ) ) < 1e-9,
	       "spot outer_angle unchanged after invalid-scalar rejections" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// DirectionalLight: parser params (power, direction, color).
// shootphotons not in directional_light descriptor → not surfaced.
// Direction is normalized in SetIntermediateValue (DirectionalLight.cpp).
//////////////////////////////////////////////////////////////////////

static void TestDirectionalLightFullMatrix()
{
	std::cout << "Test: DirectionalLight full edit matrix (chunk vocabulary)" << std::endl;

	Job* pJob = MakeMinimalJob();
	double col[3] = { 1, 1, 1 };
	double dir[3] = { 0, 1, 0 };
	pJob->AddDirectionalLight( "sun", 1.0, col, dir );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "sun" ) );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "sun" );

	// -- power --
	Check( c.SetProperty( String( "power" ), String( "7.5" ) ), "dir power set" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 7.5 ) < 1e-6,
	       "dir power applied" );
	Check( c.Editor().Undo(), "dir power undo" );

	// -- direction (must be normalized after edit) --
	// Input (3, 0, 4) has magnitude 5 → expect (0.6, 0, 0.8).
	Check( c.SetProperty( String( "direction" ), String( "3 0 4" ) ), "dir direction set" );
	const Vector3 d = light->emissionDirection();
	const double mag = std::sqrt( static_cast<double>( d.x * d.x + d.y * d.y + d.z * d.z ) );
	Check( std::abs( mag - 1.0 ) < 1e-4,
	       "dir direction normalized to unit length after SetIntermediateValue" );
	Check( std::abs( static_cast<double>( d.x ) - 0.6 ) < 1e-4, "dir direction.x = 0.6 (3/5)" );
	Check( std::abs( static_cast<double>( d.z ) - 0.8 ) < 1e-4, "dir direction.z = 0.8 (4/5)" );
	Check( c.Editor().Undo(), "dir direction undo" );

	// -- color --
	Check( c.SetProperty( String( "color" ), String( "0.9 0.8 0.7" ) ), "dir color set" );
	Check( std::abs( static_cast<double>( light->emissionColor().r ) - 0.9 ) < 1e-6,
	       "dir color.r applied" );
	Check( c.Editor().Undo(), "dir color undo" );
	Check( c.Editor().Redo(), "dir color redo" );
	Check( std::abs( static_cast<double>( light->emissionColor().r ) - 0.9 ) < 1e-6,
	       "dir color.r reapplied after redo" );

	// -- invalid vec3 inputs rejected --
	Check( !c.SetProperty( String( "direction" ), String( "bogus" ) ),
	       "dir invalid direction rejected" );
	Check( !c.SetProperty( String( "color" ), String( "x y z" ) ),
	       "dir invalid color rejected" );

	// -- invalid scalar power rejected (strict parse) --
	const Scalar guardEnergy = light->emissionEnergy();
	Check( !c.SetProperty( String( "power" ), String( "garbage" ) ),
	       "dir invalid power rejected" );
	Check( !c.SetProperty( String( "power" ), String( "nan" ) ),
	       "dir NaN power rejected" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() - guardEnergy ) ) < 1e-9,
	       "dir power unchanged after invalid-scalar rejections" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Explicit regression for the chunk → keyframe translation table.
// Guards against drift in ChunkNameToKeyframeName (SceneEditor.cpp:469).
// The introspection panel always sends chunk vocabulary, so if a future
// edit breaks the table the panel goes silent for these three params.
//////////////////////////////////////////////////////////////////////

static void TestChunkVocabularyDispatch()
{
	std::cout << "Test: ChunkNameToKeyframeName translation for power/inner/outer" << std::endl;

	Job* pJob = MakeMinimalJob();
	const double DEG = static_cast<double>( PI ) / 180.0;
	double pos[3] = { 0, 0, 0 };
	double tgt[3] = { 1, 0, 0 };
	double col[3] = { 1, 1, 1 };
	pJob->AddPointSpotLight( "spot", 1.0, col, tgt, 10.0 * DEG, 20.0 * DEG, pos, true );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "spot" ) );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "spot" );

	Check( c.SetProperty( String( "power" ), String( "9" ) ),
	       "chunk 'power' → keyframe 'energy' dispatch returns true" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 9.0 ) < 1e-6,
	       "power=9 actually applied through translation" );

	Check( c.SetProperty( String( "inner" ), String( "33" ) ),
	       "chunk 'inner' → keyframe 'inner_angle' dispatch returns true" );
	Check( std::abs( static_cast<double>( light->emissionInnerAngle() ) - 33.0 * DEG ) < 1e-4,
	       "inner=33° actually applied" );

	Check( c.SetProperty( String( "outer" ), String( "77" ) ),
	       "chunk 'outer' → keyframe 'outer_angle' dispatch returns true" );
	Check( std::abs( static_cast<double>( light->emissionOuterAngle() ) - 77.0 * DEG ) < 1e-4,
	       "outer=77° actually applied" );
	}
	pJob->release();
}

int main()
{
	std::cout << "SceneEditorLightFullCoverageTest" << std::endl;

	TestAmbientLightFullMatrix();
	TestPointLightFullMatrix();
	TestSpotLightFullMatrix();
	TestDirectionalLightFullMatrix();
	TestChunkVocabularyDispatch();

	std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
