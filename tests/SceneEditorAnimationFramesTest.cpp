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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>

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

static Job* MakeCstJob( std::string& outPath )
{
	const std::filesystem::path source =
		std::filesystem::path( "scenes/Tests/Animation/named_animations.RISEscene" );
	const std::filesystem::path target =
		std::filesystem::temp_directory_path() /
		"rise_scene_editor_animation_frames.RISEscene";
	std::ifstream in( source, std::ios::binary );
	std::ofstream out( target, std::ios::binary | std::ios::trunc );
	out << in.rdbuf();
	out.close();
	outPath = target.string();

	Job* job = new Job();
	Check( in.good() || in.eof(), "opened named-animation fixture" );
	Check( job->LoadAsciiSceneViaCst( outPath.c_str() ),
		"loaded named-animation fixture through retained CST path" );
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

	std::string scenePath;
	Job* pJob = MakeCstJob( scenePath );

	{
	SceneEditController c( *pJob, /*interactiveRasterizer*/ 0 );

	// --- The Animation category lists both named animations ---
	Check( c.CategoryEntityCount( Cat::Animation ) == 2, "Animation category lists 2 animations" );

	// --- Selecting the section surfaces an editable "frames" row ---
	c.ForTest_SetSelection( Cat::Animation, String( "Slide" ) );
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
	Check( c.HasUnsavedChanges(), "animation frame edit marks the retained Document dirty" );
	Check( std::string( c.SerializedSceneText().c_str() ).find( "num_frames\t\t60" ) != std::string::npos,
		"animation frame edit persists as animation.num_frames in the CST" );
	{
		double ts = 0, te = 1; unsigned int nf = 0; bool df = false, invf = false;
		pJob->GetAnimationOptions( ts, te, nf, df, invf );
		Check( ts == 0.0 && te == 1.0, "editing frames preserved the time range (0..1)" );
	}

	c.Undo();
	Check( ActiveNumFrames( *pJob ) == 24, "Undo restores the live animation frame count" );
	c.Redo();
	Check( ActiveNumFrames( *pJob ) == 60, "Redo restores the edited live animation frame count" );

	const SaveResult save = c.RequestSave( scenePath );
	Check( Succeeded( save.status ), "saving the animation frame edit succeeds" );
	Check( !c.HasUnsavedChanges(), "successful save clears animation dirty state" );
	Job* reloaded = new Job();
	Check( reloaded->LoadAsciiSceneViaCst( scenePath.c_str() ),
		"saved animation scene reloads through CST" );
	Check( ActiveNumFrames( *reloaded ) == 60,
		"saved/reloaded animation preserves the edited frame count" );
	reloaded->release();

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

	// --- Any-thread D2 edits vs UI metadata/history polling ---
	// The animation getter and Undo/Redo labels are polled by both shells.
	// Repeated D2 frame edits replace the Job graph and push EditHistory;
	// concurrent reads must either return one coherent generation or the
	// documented transient empty/false fallback, never touch freed graph or
	// deque storage.
	std::atomic<bool> stopReader{ false };
	std::atomic<bool> sawBadFrames{ false };
	std::atomic<unsigned int> successfulReads{ 0 };
	std::thread reader( [&] {
		while( !stopReader.load( std::memory_order_acquire ) ) {
			double ts = 0, te = 1;
			unsigned int nf = 0;
			if( c.GetAnimationOptions( ts, te, nf ) ) {
				if( nf != 60u && nf != 61u ) {
					sawBadFrames.store( true, std::memory_order_release );
				}
				successfulReads.fetch_add( 1, std::memory_order_relaxed );
			}
			c.UndoLabel();
			c.RedoLabel();
			std::this_thread::yield();
		}
	} );
	for( unsigned int i = 0; i < 20; ++i ) {
		const String value( ( i & 1u ) ? "60" : "61" );
		Check( c.SetPropertyForCategory( Cat::Animation, String( "frames" ), value ),
		       "concurrent D2 animation edit applies" );
		std::this_thread::yield();
	}
	stopReader.store( true, std::memory_order_release );
	reader.join();
	Check( successfulReads.load( std::memory_order_acquire ) > 0,
	       "concurrent UI poll observed animation snapshots" );
	Check( !sawBadFrames.load( std::memory_order_acquire ),
	       "concurrent animation getter returned only coherent frame counts" );
	Check( ActiveNumFrames( *pJob ) == 60,
	       "concurrency stress leaves the active animation at the final value" );
	}

	pJob->release();
	std::remove( scenePath.c_str() );

	std::cout << "\n" << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
