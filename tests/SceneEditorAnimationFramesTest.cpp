//////////////////////////////////////////////////////////////////////
//
//  SceneEditorAnimationFramesTest.cpp - Coverage for the interactive
//    editor's "Animation" accordion Category: enumerating the scene's
//    named animations, and the single editable property (frame count
//    of the active animation) that lets the user make the rendered /
//    previewed clip longer or shorter.
//
//    Exercises the GUI-only path (SceneEditController per-category
//    property build + SetPropertyForCategory) that the CLI can't reach:
//      - Category::Animation lists every named animation.
//      - The "frames" property reports the ACTIVE animation's
//        num_frames as an editable UInt.
//      - Editing it updates num_frames (verified via GetAnimationOptions)
//        while preserving the time range; garbage / zero is rejected.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cstring>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"

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

static int FindFramesRow( SceneEditController& c )
{
	using Cat = SceneEditController::Category;
	const unsigned int n = c.PropertyCountFor( Cat::Animation );
	for( unsigned int i = 0; i < n; ++i ) {
		if( c.PropertyNameFor( Cat::Animation, i ) == String( "frames" ) ) {
			return static_cast<int>( i );
		}
	}
	return -1;
}

static unsigned int ActiveNumFrames( Job& job )
{
	double ts = 0, te = 1; unsigned int nf = 0; bool df = false, invf = false;
	job.GetAnimationOptions( ts, te, nf, df, invf );
	return nf;
}

int main()
{
	using Cat = SceneEditController::Category;
	std::cout << "SceneEditorAnimationFramesTest" << std::endl;

	Job* pJob = MakeMinimalJob();

	// Two named animations; "Spin" is active with 24 frames.
	pJob->DeclareAnimation( "Spin",  0.0, 1.0, 24, false, false, /*make_active*/ true  );
	pJob->DeclareAnimation( "Drift", 0.0, 2.0, 48, false, false, /*make_active*/ false );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/ 0 );

	// --- The Animation category lists both named animations ---
	Check( c.CategoryEntityCount( Cat::Animation ) == 2, "Animation category lists 2 animations" );

	// --- Selecting the section surfaces an editable "frames" row ---
	c.ForTest_SetSelection( Cat::Animation, String( "Spin" ) );
	c.RefreshProperties();

	const int row = FindFramesRow( c );
	Check( row >= 0, "Animation section exposes a 'frames' property" );
	if( row >= 0 ) {
		Check( c.PropertyValueFor( Cat::Animation, row ) == String( "24" ),
		       "frames row reports the active animation's num_frames (24)" );
		Check( c.PropertyEditableFor( Cat::Animation, row ) == true,
		       "frames row is editable" );
		Check( c.PropertyKindFor( Cat::Animation, row ) == static_cast<int>( ValueKind::UInt ),
		       "frames row kind is UInt" );
	}

	// --- Editing the frame count updates num_frames, preserving the range ---
	Check( c.SetPropertyForCategory( Cat::Animation, String( "frames" ), String( "60" ) ),
	       "SetPropertyForCategory(frames=60) succeeds" );
	Check( ActiveNumFrames( *pJob ) == 60, "active animation num_frames is now 60" );
	{
		double ts = 0, te = 1; unsigned int nf = 0; bool df = false, invf = false;
		pJob->GetAnimationOptions( ts, te, nf, df, invf );
		Check( ts == 0.0 && te == 1.0, "editing frames preserved the time range (0..1)" );
	}

	// The refreshed row reflects the new value.
	c.RefreshProperties();
	{
		const int row2 = FindFramesRow( c );
		Check( row2 >= 0 && c.PropertyValueFor( Cat::Animation, row2 ) == String( "60" ),
		       "frames row re-reads as 60 after the edit" );
	}

	// --- Garbage / zero is rejected; num_frames is left unchanged ---
	Check( !c.SetPropertyForCategory( Cat::Animation, String( "frames" ), String( "0" ) ),
	       "frames=0 is rejected" );
	Check( !c.SetPropertyForCategory( Cat::Animation, String( "frames" ), String( "abc" ) ),
	       "frames=abc is rejected" );
	Check( ActiveNumFrames( *pJob ) == 60, "num_frames unchanged after rejected edits" );
	}

	pJob->release();

	std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
