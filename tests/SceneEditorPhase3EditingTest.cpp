//////////////////////////////////////////////////////////////////////
//
//  SceneEditorPhase3EditingTest.cpp - Phase 3 editing surfaces.
//
//  Covers:
//   - Rasterizer parameter edit + re-instantiation (samples,
//     max_eye_depth, etc.) preserving non-edited params.
//   - Object orientation / stretch / material / shader / shadow-flag
//     edits routed through SceneEditor::Apply with undo/redo.
//   - Light SetLightProperty edits (energy / color / position) with
//     undo restoring the prior value.
//   - Per-light-type virtuals: LightType / emissionTarget /
//     emissionInnerAngle / emissionOuterAngle return correct values
//     for spot vs. point vs. directional vs. ambient.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cstring>
#include <cmath>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
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

static Job* MakeJobAndSeed()
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

	// Seed a sphere object with a Lambertian material so we have an
	// actual material to swap.
	job->AddSphereGeometry( "sphere_geom", 1.0 );
	double white[3] = { 1, 1, 1 };
	job->AddUniformColorPainter( "white", white, "sRGB" );
	job->AddLambertianMaterial( "lambert_white", "white" );

	const double zero3[3] = { 0, 0, 0 };
	const double one3[3]  = { 1, 1, 1 };
	RadianceMapConfig nilRMap;
	job->AddObject(
		"sphere", "sphere_geom",
		"lambert_white",
		nullptr, nullptr,
		nilRMap, zero3, zero3, one3, true, true );

	return job;
}

//////////////////////////////////////////////////////////////////////
// Test 1: rasterizer parameter edit re-instantiates with new value
//////////////////////////////////////////////////////////////////////

static void TestRasterizerParamEdit()
{
	std::cout << "Test 1: rasterizer parameter edit" << std::endl;

	Job* pJob = MakeJobAndSeed();

	// Lazy-build a BDPT-pel rasterizer.
	const bool ok = pJob->SetActiveRasterizer( "bdpt_pel_rasterizer" );
	Check( ok, "Lazy-build BDPT-pel" );
	IRasterizer* before = pJob->GetRasterizer();
	Check( before != nullptr, "Active rasterizer is non-null" );

	// Verify the snapshot was recorded.  Defaults come from
	// `BDPTPelDefaults` in `Utilities/RasterizerDefaults.h` (single
	// source of truth).  Update these literals if the canonical
	// defaults change there — the consistency check in
	// `RasterizerDefaultsConsistencyTest.cpp` enforces that the
	// parser, descriptor hints, and Job lazy-build path all agree.
	const Job::RasterizerParams* params = pJob->GetRasterizerParams( "bdpt_pel_rasterizer" );
	Check( params != nullptr, "Snapshot exists" );
	Check( params && params->numPixelSamples == 32, "Default numPixelSamples = 32 (BDPTPelDefaults)" );
	Check( params && params->maxEyeDepth     == 8,  "Default maxEyeDepth = 8" );

	// Edit numPixelSamples → 4.  This re-instantiates the rasterizer.
	const bool okEdit = pJob->SetRasterizerParameter( "bdpt_pel_rasterizer", "samples", "4" );
	Check( okEdit, "SetRasterizerParameter(samples=4) returns true" );

	// Snapshot reflects new value.
	const Job::RasterizerParams* updated = pJob->GetRasterizerParams( "bdpt_pel_rasterizer" );
	Check( updated && updated->numPixelSamples == 4, "Snapshot numPixelSamples updated to 4" );
	Check( updated && updated->maxEyeDepth     == 8, "Other params (maxEyeDepth) preserved" );

	// Get-back as string round-trips.
	const std::string val = pJob->GetRasterizerParameter( "bdpt_pel_rasterizer", "samples" );
	Check( val == "4", "GetRasterizerParameter round-trips" );

	// Edit max_eye_depth → 12.
	const bool okDepth = pJob->SetRasterizerParameter( "bdpt_pel_rasterizer", "max_eye_depth", "12" );
	Check( okDepth, "SetRasterizerParameter(max_eye_depth=12)" );
	const Job::RasterizerParams* depthUpdated = pJob->GetRasterizerParams( "bdpt_pel_rasterizer" );
	Check( depthUpdated && depthUpdated->maxEyeDepth == 12, "maxEyeDepth = 12" );
	Check( depthUpdated && depthUpdated->numPixelSamples == 4, "numPixelSamples (4) preserved" );

	// Unknown param fails.
	Check( !pJob->SetRasterizerParameter( "bdpt_pel_rasterizer", "unknown_param", "9" ),
	       "Unknown param rejected" );

	// Unknown rasterizer fails.
	Check( !pJob->SetRasterizerParameter( "no_such_rasterizer", "samples", "1" ),
	       "Unknown rasterizer rejected" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 2: object orientation / stretch edit + undo
//////////////////////////////////////////////////////////////////////

static void TestObjectOrientationStretch()
{
	std::cout << "Test 2: object orientation / stretch edit + undo" << std::endl;

	Job* pJob = MakeJobAndSeed();
	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	// Stretch: per-axis (1.5, 2, 0.5).
	const bool okStretch = c.SetProperty( String( "stretch" ), String( "1.5 2 0.5" ) );
	Check( okStretch, "SetProperty(stretch)" );

	IObjectManager* objs = pJob->GetObjects();
	const IObject* obj = objs->GetItem( "sphere" );
	Check( obj != nullptr, "object still registered" );
	if( obj ) {
		const Matrix4 m = obj->GetFinalTransformMatrix();
		const double lx = std::sqrt( static_cast<double>( m._00*m._00 + m._01*m._01 + m._02*m._02 ) );
		Check( std::abs( lx - 1.5 ) < 1e-5, "x-stretch ≈ 1.5" );
	}

	// Undo the stretch.
	Check( c.Editor().Undo(), "Undo stretch returns true" );
	if( obj ) {
		const Matrix4 m = obj->GetFinalTransformMatrix();
		const double lx = std::sqrt( static_cast<double>( m._00*m._00 + m._01*m._01 + m._02*m._02 ) );
		Check( std::abs( lx - 1.0 ) < 1e-5, "x-stretch restored to 1 after undo" );
	}

	// Redo.
	Check( c.Editor().Redo(), "Redo stretch returns true" );
	if( obj ) {
		const Matrix4 m = obj->GetFinalTransformMatrix();
		const double lx = std::sqrt( static_cast<double>( m._00*m._00 + m._01*m._01 + m._02*m._02 ) );
		Check( std::abs( lx - 1.5 ) < 1e-5, "x-stretch back to 1.5 after redo" );
	}
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 3: object material / shader edit + undo
//////////////////////////////////////////////////////////////////////

static void TestObjectMaterialShaderEdit()
{
	std::cout << "Test 3: object material / shader edit + undo" << std::endl;

	Job* pJob = MakeJobAndSeed();

	// Add a second material for switching.
	double red[3] = { 1, 0, 0 };
	pJob->AddUniformColorPainter( "red", red, "sRGB" );
	pJob->AddLambertianMaterial( "lambert_red", "red" );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	IObjectManager* objs = pJob->GetObjects();
	const IObject* obj = objs->GetItem( "sphere" );
	Check( obj != nullptr, "sphere registered" );
	const IMaterial* before = obj ? obj->GetMaterial() : nullptr;
	Check( before != nullptr, "sphere has a material before edit" );

	// Switch material to lambert_red.
	const bool okMat = c.SetProperty( String( "material" ), String( "lambert_red" ) );
	Check( okMat, "SetProperty(material)" );

	const IMaterial* after = obj ? obj->GetMaterial() : nullptr;
	IMaterialManager* mats = pJob->GetMaterials();
	Check( after == mats->GetItem( "lambert_red" ),
	       "object's material now points at lambert_red" );

	// Undo: material reverts to lambert_white.
	Check( c.Editor().Undo(), "Undo material edit" );
	const IMaterial* afterUndo = obj ? obj->GetMaterial() : nullptr;
	Check( afterUndo == mats->GetItem( "lambert_white" ),
	       "material restored to lambert_white" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 4: object shadow flags + undo
//////////////////////////////////////////////////////////////////////

static void TestObjectShadowFlags()
{
	std::cout << "Test 4: object shadow-flags edit + undo" << std::endl;

	Job* pJob = MakeJobAndSeed();
	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	IObjectManager* objs = pJob->GetObjects();
	const IObject* obj = objs->GetItem( "sphere" );
	Check( obj && obj->DoesCastShadows(),    "sphere casts shadows by default" );
	Check( obj && obj->DoesReceiveShadows(), "sphere receives shadows by default" );

	// Disable shadow casting.
	Check( c.SetProperty( String( "casts_shadows" ), String( "false" ) ),
	       "SetProperty(casts_shadows=false)" );
	Check( !obj->DoesCastShadows(),    "casts now false" );
	Check( obj->DoesReceiveShadows(),  "receives still true (unaffected)" );

	// Undo restores.
	Check( c.Editor().Undo(), "Undo shadow edit" );
	Check( obj->DoesCastShadows(), "casts restored to true" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 5: light edit with undo/redo via SetLightProperty
//////////////////////////////////////////////////////////////////////

static void TestLightEditUndoRedo()
{
	std::cout << "Test 5: light SetLightProperty + undo/redo" << std::endl;

	Job* pJob = MakeJobAndSeed();
	double pos[3] = { 0, 0, 0 };
	double col[3] = { 1, 1, 1 };
	pJob->AddPointOmniLight( "key", 1.0, col, pos, true );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "key" ) );

	ILightManager* lights = pJob->GetLights();
	const ILightPriv* light = lights->GetItem( "key" );
	Check( light != nullptr, "light registered" );

	const Scalar before = light->emissionEnergy();
	Check( std::abs( static_cast<double>( before ) - 1.0 ) < 1e-9, "energy starts at 1" );

	// Edit energy → 5.5.
	Check( c.SetProperty( String( "energy" ), String( "5.5" ) ),
	       "SetProperty(energy=5.5)" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 5.5 ) < 1e-6,
	       "energy = 5.5 after forward" );

	// Undo: energy returns to 1.
	Check( c.Editor().Undo(), "Undo light edit" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 1.0 ) < 1e-6,
	       "energy restored to 1 after undo" );

	// Redo: energy goes back to 5.5.
	Check( c.Editor().Redo(), "Redo light edit" );
	Check( std::abs( static_cast<double>( light->emissionEnergy() ) - 5.5 ) < 1e-6,
	       "energy = 5.5 after redo" );

	// Edit color, undo, verify roundtrip.  Capture the *initial* color
	// (which is the ROMM-RGB conversion of sRGB white — not necessarily
	// 1,1,1 in ROMM space) so the undo-roundtrip assertion compares
	// against the actual stored value rather than the user-typed value.
	const RISEPel colorBefore = light->emissionColor();
	Check( c.SetProperty( String( "color" ), String( "0.2 0.5 0.8" ) ),
	       "SetProperty(color)" );
	Check( std::abs( static_cast<double>( light->emissionColor().r ) - 0.2 ) < 1e-6,
	       "color.r = 0.2 after edit (KeyframeFromParameters takes ROMM doubles directly)" );
	Check( c.Editor().Undo(), "Undo color edit" );
	const RISEPel colorAfter = light->emissionColor();
	Check( std::abs( static_cast<double>( colorAfter.r - colorBefore.r ) ) < 1e-4,
	       "color.r restored after undo" );
	Check( std::abs( static_cast<double>( colorAfter.g - colorBefore.g ) ) < 1e-4,
	       "color.g restored after undo" );
	Check( std::abs( static_cast<double>( colorAfter.b - colorBefore.b ) ) < 1e-4,
	       "color.b restored after undo" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 6: per-light-type virtuals
//////////////////////////////////////////////////////////////////////

static void TestPerLightTypeVirtuals()
{
	std::cout << "Test 6: per-light-type LightType + accessors" << std::endl;

	Job* pJob = MakeJobAndSeed();
	double pos[3]    = { 1, 2, 3 };
	double tgt[3]    = { 4, 5, 6 };
	double col[3]    = { 1, 1, 1 };
	double dir[3]    = { 0, -1, 0 };

	const double DEG = static_cast<double>( PI ) / 180.0;
	pJob->AddPointOmniLight(  "omni",  1.0, col, pos, true );
	pJob->AddPointSpotLight(  "spot",  1.0, col, tgt, 30.0 * DEG, 60.0 * DEG, pos, true );
	pJob->AddDirectionalLight("dir",   1.0, col, dir );
	pJob->AddAmbientLight(    "amb",   1.0, col );

	ILightManager* lights = pJob->GetLights();

	const ILight* omni  = lights->GetItem( "omni" );
	const ILight* spot  = lights->GetItem( "spot" );
	const ILight* dirL  = lights->GetItem( "dir" );
	const ILight* amb   = lights->GetItem( "amb" );

	Check( omni && omni->lightType() == ILight::LightType::Point,       "omni → Point" );
	Check( spot && spot->lightType() == ILight::LightType::Spot,        "spot → Spot" );
	Check( dirL && dirL->lightType() == ILight::LightType::Directional, "dir → Directional" );
	Check( amb  && amb->lightType()  == ILight::LightType::Ambient,     "amb → Ambient" );

	// Spot extras: target, inner / outer angle.  AddPointSpotLight
	// takes inner/outer in degrees and converts to radians internally;
	// ILight::emissionInnerAngle returns the FULL cone angle in radians
	// (which the SpotLight stored).  Check via degree round-trip:
	if( spot ) {
		const Point3 t = spot->emissionTarget();
		Check( std::abs( static_cast<double>( t.x ) - 4.0 ) < 1e-9, "spot.target.x = 4" );

		const double innerDeg = static_cast<double>( spot->emissionInnerAngle() ) * 180.0 / static_cast<double>( PI );
		Check( std::abs( innerDeg - 30.0 ) < 1e-3, "spot inner = 30°" );

		const double outerDeg = static_cast<double>( spot->emissionOuterAngle() ) * 180.0 / static_cast<double>( PI );
		Check( std::abs( outerDeg - 60.0 ) < 1e-3, "spot outer = 60°" );
	}

	// Defaults for non-spot types.
	Check( omni && omni->emissionTarget().x == 0,      "non-spot target → origin" );

	pJob->release();
}

// Test 7: spot-light per-type properties (target/inner/outer)
// round-trip through SetLightProperty with undo restoring the
// captured prev value.  Regression for the bug where
// `ReadLightProperty` only handled position/energy/color/target
// and missed inner_angle/outer_angle/direction — undo would
// silently drop the edit from history.
static void TestSpotLightFullEditUndo()
{
	std::cout << "Test 7: spot-light inner/outer/target edit undo" << std::endl;

	Job* pJob = MakeJobAndSeed();
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

	// Edit inner_angle to 45°.
	const double initialInner = static_cast<double>( light->emissionInnerAngle() ) * 180.0 / static_cast<double>( PI );
	Check( c.SetProperty( String( "inner_angle" ), String( "45" ) ),
	       "SetProperty(inner_angle=45)" );
	const double afterEdit = static_cast<double>( light->emissionInnerAngle() ) * 180.0 / static_cast<double>( PI );
	Check( std::abs( afterEdit - 45.0 ) < 1e-3, "inner_angle = 45° after edit" );

	// Undo restores 30°.  This used to silently fail because
	// ReadLightProperty didn't cover "inner_angle"; the edit was
	// captured with empty prev, the keyframe parse on undo
	// returned null, undo returned false and dropped the entry.
	Check( c.Editor().Undo(), "Undo inner_angle returns true (regression: was false)" );
	const double afterUndo = static_cast<double>( light->emissionInnerAngle() ) * 180.0 / static_cast<double>( PI );
	Check( std::abs( afterUndo - initialInner ) < 1e-3,
	       "inner_angle restored to initial after undo" );

	// Same regression for outer_angle.
	Check( c.SetProperty( String( "outer_angle" ), String( "75" ) ), "SetProperty(outer_angle=75)" );
	Check( c.Editor().Undo(), "Undo outer_angle" );
	const double outerAfter = static_cast<double>( light->emissionOuterAngle() ) * 180.0 / static_cast<double>( PI );
	Check( std::abs( outerAfter - 60.0 ) < 1e-3, "outer_angle restored to 60°" );

	// Same for target.
	Check( c.SetProperty( String( "target" ), String( "10 20 30" ) ), "SetProperty(target)" );
	Check( c.Editor().Undo(), "Undo target" );
	const Point3 tgtAfter = light->emissionTarget();
	Check( std::abs( static_cast<double>( tgtAfter.x ) - 4.0 ) < 1e-3, "target.x restored to 4" );
	Check( std::abs( static_cast<double>( tgtAfter.y ) - 5.0 ) < 1e-3, "target.y restored to 5" );
	Check( std::abs( static_cast<double>( tgtAfter.z ) - 6.0 ) < 1e-3, "target.z restored to 6" );
	}
	pJob->release();
}

// Test 7b: ambient-light power/color edit actually mutates the light.
// Regression for the bug where AmbientLight::KeyframeFromParameters
// allocated IDs 1000/1001 while SetIntermediateValue's switch handled
// 100/101 — every ambient edit silently no-op'd while reporting
// success and entering undo history as a phantom edit.
static void TestAmbientLightEditApplies()
{
	std::cout << "Test 7b: ambient light power/color edit applies (ID-mismatch regression)" << std::endl;

	Job* pJob = MakeJobAndSeed();
	double col[3] = { 1, 1, 1 };
	pJob->AddAmbientLight( "amb", 1.0, col );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "amb" ) );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "amb" );

	const Scalar initialEnergy = light->emissionEnergy();

	Check( c.SetProperty( String( "energy" ), String( "4.25" ) ),
	       "SetProperty(energy=4.25) returns true" );
	const double afterEdit = static_cast<double>( light->emissionEnergy() );
	Check( std::abs( afterEdit - 4.25 ) < 1e-6,
	       "ambient energy actually updated to 4.25 (would no-op pre-fix)" );

	Check( c.Editor().Undo(), "Undo ambient energy edit" );
	const double afterUndo = static_cast<double>( light->emissionEnergy() );
	Check( std::abs( afterUndo - static_cast<double>( initialEnergy ) ) < 1e-6,
	       "ambient energy restored on undo" );
	}
	pJob->release();
}

// Test 7c: invalid material/shader edit returns false and DOES NOT
// pollute the undo history.  Regression for the bug where
// ApplyObjectOpForward silently skipped the assign on an unknown
// material name, but Apply still pushed the edit + returned true.
static void TestInvalidMaterialShaderRejected()
{
	std::cout << "Test 7c: invalid material/shader name is rejected" << std::endl;

	Job* pJob = MakeJobAndSeed();
	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	const unsigned int undoBefore = c.Editor().History().UndoDepth();

	// Unknown material name — must be rejected.
	Check( !c.SetProperty( String( "material" ), String( "no_such_material" ) ),
	       "Unknown material name rejected" );
	Check( c.Editor().History().UndoDepth() == undoBefore,
	       "Undo history NOT polluted by rejected material edit" );

	// Unknown shader name — must be rejected.
	Check( !c.SetProperty( String( "shader" ), String( "no_such_shader" ) ),
	       "Unknown shader name rejected" );
	Check( c.Editor().History().UndoDepth() == undoBefore,
	       "Undo history NOT polluted by rejected shader edit" );
	}
	pJob->release();
}

// Test 8: directional light "direction" round-trip
static void TestDirectionalLightUndo()
{
	std::cout << "Test 8: directional light direction edit undo" << std::endl;

	Job* pJob = MakeJobAndSeed();
	double col[3] = { 1, 1, 1 };
	double dir[3] = { 0, -1, 0 };
	pJob->AddDirectionalLight( "sun", 1.0, col, dir );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Light, String( "sun" ) );

	ILightManager* lights = pJob->GetLights();
	const ILight* light = lights->GetItem( "sun" );

	const Vector3 dirBefore = light->emissionDirection();
	Check( c.SetProperty( String( "direction" ), String( "1 0 0" ) ),
	       "SetProperty(direction=1 0 0)" );
	Check( c.Editor().Undo(), "Undo direction (regression: undo was silently a no-op)" );
	const Vector3 dirAfter = light->emissionDirection();
	Check( std::abs( static_cast<double>( dirAfter.x - dirBefore.x ) ) < 1e-3, "direction.x restored" );
	Check( std::abs( static_cast<double>( dirAfter.y - dirBefore.y ) ) < 1e-3, "direction.y restored" );
	Check( std::abs( static_cast<double>( dirAfter.z - dirBefore.z ) ) < 1e-3, "direction.z restored" );
	}
	pJob->release();
}

int main()
{
	std::cout << "SceneEditorPhase3EditingTest" << std::endl;
	std::cout << "============================" << std::endl;

	TestRasterizerParamEdit();
	TestObjectOrientationStretch();
	TestObjectMaterialShaderEdit();
	TestObjectShadowFlags();
	TestLightEditUndoRedo();
	TestPerLightTypeVirtuals();
	TestSpotLightFullEditUndo();
	TestAmbientLightEditApplies();
	TestInvalidMaterialShaderRejected();
	TestDirectionalLightUndo();

	std::cout << std::endl;
	std::cout << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
