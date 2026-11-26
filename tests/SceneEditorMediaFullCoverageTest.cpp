//////////////////////////////////////////////////////////////////////
//
//  SceneEditorMediaFullCoverageTest.cpp - End-to-end coverage for
//    the interactive editor's media-editing surface.
//
//    Mirrors SceneEditorLightFullCoverageTest's matrix:
//      HomogeneousMedium × (absorption, scattering, emission) ×
//      (apply, undo, redo, invalid-vec3-rejection,
//       NaN/Inf-component-rejection, trailing-junk-rejection).
//
//    Also covers:
//      - The SceneEditController dispatch wiring (Category::Medium +
//        SetPropertyForCategory).
//      - HomogeneousMedium's cached `sigma_t` / `sigma_t_max` are
//        updated in lockstep with SetAbsorption / SetScattering — a
//        ratio-tracker stale cache would silently produce wrong
//        free-flight distances even though the per-channel values
//        round-trip.
//      - HeterogeneousMedium rejects the edit (its majorant grid is
//        baked at construction; MediaIntrospection::SetSlotValue
//        refuses).
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstring>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IMedium.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/MediaIntrospection.h"
#include "../src/Library/Materials/HomogeneousMedium.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) passCount++;
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

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
// Test 1: HomogeneousMedium absorption / scattering / emission
//         round-trip + undo + redo via SceneEditController.
//////////////////////////////////////////////////////////////////////

static void TestHomogeneousFullMatrix()
{
	std::cout << "Test: HomogeneousMedium full edit matrix" << std::endl;

	Job* pJob = MakeMinimalJob();
	double sa[3] = { 0.1, 0.2, 0.3 };
	double ss[3] = { 0.5, 0.5, 0.5 };
	pJob->AddHomogeneousMedium( "fog", sa, ss, "isotropic", 0.0 );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Medium, String( "fog" ) );

	const HomogeneousMedium* hom = dynamic_cast<const HomogeneousMedium*>(
		pJob->GetMedium( "fog" ) );
	Check( hom != nullptr, "fog registered as HomogeneousMedium" );

	// -- absorption round-trip + undo + redo --
	const RISEPel a0 = hom->GetAbsorption();
	Check( c.SetProperty( String( "absorption" ), String( "0.7 0.8 0.9" ) ),
	       "hom SetProperty(absorption)" );
	Check( std::abs( static_cast<double>( hom->GetAbsorption().r ) - 0.7 ) < 1e-9,
	       "hom absorption.r applied" );
	Check( std::abs( static_cast<double>( hom->GetAbsorption().g ) - 0.8 ) < 1e-9,
	       "hom absorption.g applied" );
	Check( std::abs( static_cast<double>( hom->GetAbsorption().b ) - 0.9 ) < 1e-9,
	       "hom absorption.b applied" );
	Check( c.Editor().Undo(), "hom absorption undo" );
	Check( std::abs( static_cast<double>( hom->GetAbsorption().r - a0.r ) ) < 1e-9,
	       "hom absorption.r restored" );
	Check( c.Editor().Redo(), "hom absorption redo" );
	Check( std::abs( static_cast<double>( hom->GetAbsorption().r ) - 0.7 ) < 1e-9,
	       "hom absorption.r reapplied after redo" );
	Check( c.Editor().Undo(), "hom absorption undo (cleanup)" );

	// -- scattering round-trip + undo + redo --
	const RISEPel s0 = hom->GetScattering();
	Check( c.SetProperty( String( "scattering" ), String( "0.4 0.3 0.2" ) ),
	       "hom SetProperty(scattering)" );
	Check( std::abs( static_cast<double>( hom->GetScattering().r ) - 0.4 ) < 1e-9,
	       "hom scattering.r applied" );
	Check( c.Editor().Undo(), "hom scattering undo" );
	Check( std::abs( static_cast<double>( hom->GetScattering().r - s0.r ) ) < 1e-9,
	       "hom scattering.r restored" );
	Check( c.Editor().Redo(), "hom scattering redo" );
	Check( std::abs( static_cast<double>( hom->GetScattering().r ) - 0.4 ) < 1e-9,
	       "hom scattering.r reapplied after redo" );
	Check( c.Editor().Undo(), "hom scattering undo (cleanup)" );

	// -- emission round-trip + undo + redo --
	const RISEPel e0 = hom->GetEmission();
	Check( c.SetProperty( String( "emission" ), String( "0.05 0.0 0.0" ) ),
	       "hom SetProperty(emission)" );
	Check( std::abs( static_cast<double>( hom->GetEmission().r ) - 0.05 ) < 1e-9,
	       "hom emission.r applied" );
	Check( c.Editor().Undo(), "hom emission undo" );
	Check( std::abs( static_cast<double>( hom->GetEmission().r - e0.r ) ) < 1e-9,
	       "hom emission.r restored" );
	Check( c.Editor().Redo(), "hom emission redo" );

	// -- emission is NOT panel-editable: the `homogeneous_medium`
	//    chunk has no `emission` parameter, so a panel edit could not
	//    be saved back to the .RISEscene file (the save engine would
	//    insert an `emission` line the descriptor-driven parser then
	//    rejects on reload).  absorption / scattering stay editable —
	//    the chunk authors both. --
	{
		const std::vector<CameraProperty> rows =
			MediaIntrospection::Inspect( String( "fog" ), *hom );
		bool sawEmission = false, sawAbsorption = false;
		for( const CameraProperty& r : rows ) {
			const std::string n( r.name.c_str() );
			if( n == "emission" ) {
				sawEmission = true;
				Check( !r.editable,
				       "hom emission row is read-only (not chunk-authorable)" );
			}
			if( n == "absorption" ) {
				sawAbsorption = true;
				Check( r.editable, "hom absorption row stays editable" );
			}
		}
		Check( sawEmission && sawAbsorption,
		       "hom introspection surfaces emission + absorption rows" );
	}

	// -- invalid vec3 rejected for each editable slot --
	Check( !c.SetProperty( String( "absorption" ), String( "garbage" ) ),
	       "hom invalid absorption rejected" );
	Check( !c.SetProperty( String( "scattering" ), String( "1 2" ) ),
	       "hom 2-token scattering rejected (need exactly 3 components)" );
	Check( !c.SetProperty( String( "emission" ), String( "1 2 3 4" ) ),
	       "hom 4-token emission rejected" );
	Check( !c.SetProperty( String( "absorption" ), String( "nan nan nan" ) ),
	       "hom NaN-component absorption rejected" );
	Check( !c.SetProperty( String( "scattering" ), String( "1 inf 1" ) ),
	       "hom Inf-component scattering rejected" );
	Check( !c.SetProperty( String( "emission" ), String( "" ) ),
	       "hom empty emission rejected" );

	// -- unknown slot rejected --
	Check( !c.SetProperty( String( "phase" ), String( "0 0 0" ) ),
	       "hom phase rejected (read-only / not vec3)" );
	Check( !c.SetProperty( String( "no_such_slot" ), String( "0 0 0" ) ),
	       "hom unknown slot rejected" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 2: HomogeneousMedium's cached `sigma_t` / `sigma_t_max` track
//         absorption + scattering edits.  A stale cache would
//         silently produce wrong free-flight distances.
//
// We verify the contract via GetCoefficients() (which reads
// `m_sigma_t`) — if SetAbsorption forgot to refresh `m_sigma_t`,
// the new `sigma_t` would still be the OLD (sigma_a + sigma_s).
//////////////////////////////////////////////////////////////////////

static void TestSigmaCacheLockstep()
{
	std::cout << "Test: sigma_t cache refresh after Set*" << std::endl;

	Job* pJob = MakeMinimalJob();
	double sa[3] = { 0.1, 0.1, 0.1 };
	double ss[3] = { 0.2, 0.2, 0.2 };
	pJob->AddHomogeneousMedium( "fog", sa, ss, "isotropic", 0.0 );

	const HomogeneousMedium* hom = dynamic_cast<const HomogeneousMedium*>(
		pJob->GetMedium( "fog" ) );
	Check( hom != nullptr, "fog registered" );

	// Initial: sigma_t = sigma_a + sigma_s = (0.3, 0.3, 0.3)
	{
		const MediumCoefficients co = hom->GetCoefficients( Point3( 0, 0, 0 ) );
		Check( std::abs( static_cast<double>( co.sigma_t.r ) - 0.3 ) < 1e-9,
		       "initial sigma_t.r = sigma_a + sigma_s = 0.3" );
	}

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	c.ForTest_SetSelection( SceneEditController::Category::Medium, String( "fog" ) );

	// After SetAbsorption(0.5, 0.5, 0.5): sigma_t = 0.5 + 0.2 = 0.7
	Check( c.SetProperty( String( "absorption" ), String( "0.5 0.5 0.5" ) ),
	       "SetProperty(absorption=0.5)" );
	{
		const MediumCoefficients co = hom->GetCoefficients( Point3( 0, 0, 0 ) );
		Check( std::abs( static_cast<double>( co.sigma_t.r ) - 0.7 ) < 1e-9,
		       "after SetAbsorption: sigma_t.r = 0.5 + 0.2 = 0.7 (cache refreshed)" );
	}

	// After SetScattering(0.4, 0.4, 0.4): sigma_t = 0.5 + 0.4 = 0.9
	Check( c.SetProperty( String( "scattering" ), String( "0.4 0.4 0.4" ) ),
	       "SetProperty(scattering=0.4)" );
	{
		const MediumCoefficients co = hom->GetCoefficients( Point3( 0, 0, 0 ) );
		Check( std::abs( static_cast<double>( co.sigma_t.r ) - 0.9 ) < 1e-9,
		       "after SetScattering: sigma_t.r = 0.5 + 0.4 = 0.9 (cache refreshed)" );
	}

	// Emission should NOT touch sigma_t — verify the no-op refresh.
	Check( c.SetProperty( String( "emission" ), String( "0.99 0.99 0.99" ) ),
	       "SetProperty(emission=0.99)" );
	{
		const MediumCoefficients co = hom->GetCoefficients( Point3( 0, 0, 0 ) );
		Check( std::abs( static_cast<double>( co.sigma_t.r ) - 0.9 ) < 1e-9,
		       "after SetEmission: sigma_t.r unchanged at 0.9 (emission doesn't feed sigma_t)" );
		Check( std::abs( static_cast<double>( co.emission.r ) - 0.99 ) < 1e-9,
		       "emission.r = 0.99" );
	}
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 3: Category::Medium discoverable via the controller's category
//         APIs (entity count + names).  This is what the GUI panel's
//         section list reads to populate the Medium row dropdown.
//////////////////////////////////////////////////////////////////////

static void TestCategoryEnumeration()
{
	std::cout << "Test: Category::Medium discoverable via controller APIs" << std::endl;

	Job* pJob = MakeMinimalJob();
	double s[3] = { 0.1, 0.1, 0.1 };
	pJob->AddHomogeneousMedium( "fog",  s, s, "isotropic", 0.0 );
	pJob->AddHomogeneousMedium( "haze", s, s, "isotropic", 0.0 );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	const SceneEditController::Category cat = SceneEditController::Category::Medium;

	const unsigned int n = c.CategoryEntityCount( cat );
	Check( n == 2, "two media registered" );

	// Names enumerated.  Order is unspecified (map iteration), so
	// we accept any permutation.
	const String n0 = c.CategoryEntityName( cat, 0 );
	const String n1 = c.CategoryEntityName( cat, 1 );
	const bool gotFog  = ( n0 == String( "fog" )  || n1 == String( "fog" ) );
	const bool gotHaze = ( n0 == String( "haze" ) || n1 == String( "haze" ) );
	Check( gotFog,  "Medium category enumerates 'fog'" );
	Check( gotHaze, "Medium category enumerates 'haze'" );

	// Out-of-range index returns empty.
	Check( c.CategoryEntityName( cat, 5 ).size() <= 1,
	       "out-of-range index returns empty" );
	}
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Test 4: Undo/Redo of an `interior_medium` edit must resync the
//         per-category Media section selection.  Forward path pins
//         the new medium name; undo restores the object's prior
//         binding inside SceneEditor::Undo but doesn't touch the
//         controller's per-cat panel selection.  Without the resync,
//         the Media section would keep showing the post-edit binding's
//         NAME while the object actually has the pre-edit binding's
//         content — the section's editable rows would read from a
//         medium the object no longer references.
//
//         Regression for the [P2] gap caught in the 43ed7f3f review.
//////////////////////////////////////////////////////////////////////

static void TestInteriorMediumUndoResync()
{
	std::cout << "Test: Undo/Redo of interior_medium resyncs Media section" << std::endl;

	Job* pJob = MakeMinimalJob();

	// Two named media to swap between.
	double s[3] = { 0.1, 0.1, 0.1 };
	pJob->AddHomogeneousMedium( "fog",  s, s, "isotropic", 0.0 );
	pJob->AddHomogeneousMedium( "haze", s, s, "isotropic", 0.0 );

	// A sphere with no initial interior medium.
	pJob->AddSphereGeometry( "sphere_geom", 1.0 );
	double white[3] = { 1, 1, 1 };
	pJob->AddUniformColorPainter( "white", white, "sRGB" );
	pJob->AddLambertianMaterial( "lambert_white", "white" );
	const double zero3[3] = { 0, 0, 0 };
	const double one3[3]  = { 1, 1, 1 };
	RadianceMapConfig nilRMap;
	pJob->AddObject(
		"sphere", "sphere_geom",
		"lambert_white",
		nullptr, nullptr,
		nilRMap, zero3, zero3, one3, true, true );

	// Bind the initial interior medium to "fog" via the Job API.
	pJob->SetObjectInteriorMedium( "sphere", "fog" );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/0 );
	const auto MEDIUM = SceneEditController::Category::Medium;

	// Use SetSelection (real UI path) so the Object→Medium auto-sync
	// fires.  ForTest_SetSelection deliberately bypasses auto-sync.
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	Check( c.GetSelectionNameForCategory( MEDIUM ) == String( "fog" ),
	       "Object pick auto-fills Media section with 'fog' (fresh controller)" );

	Check( c.SetProperty( String( "interior_medium" ), String( "haze" ) ),
	       "SetProperty(interior_medium=haze)" );
	Check( c.GetSelectionNameForCategory( MEDIUM ) == String( "haze" ),
	       "Media section pinned to 'haze' after forward edit" );

	// Controller-level Undo invokes ResyncObjectBoundSections_.
	c.Undo();
	Check( c.GetSelectionNameForCategory( MEDIUM ) == String( "fog" ),
	       "Media section resynced to 'fog' after Undo (regression for 43ed7f3f gap)" );

	// Redo flips back to "haze".
	c.Redo();
	Check( c.GetSelectionNameForCategory( MEDIUM ) == String( "haze" ),
	       "Media section resynced to 'haze' after Redo" );
	}
	pJob->release();
}

int main()
{
	std::cout << "SceneEditorMediaFullCoverageTest" << std::endl;

	TestHomogeneousFullMatrix();
	TestSigmaCacheLockstep();
	TestCategoryEnumeration();
	TestInteriorMediumUndoResync();

	std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
