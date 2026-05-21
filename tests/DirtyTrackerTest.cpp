//////////////////////////////////////////////////////////////////////
//
//  DirtyTrackerTest.cpp - Phase 6.3 verification.
//
//  Exercises:
//    - DirtyTracker basic ops (MarkDirty, Contains, Snapshot, Clear).
//    - SceneEditor::Apply marks transform ops dirty; property ops
//      do NOT mark dirty (V1 scope).
//    - Undo / Redo of transform ops mark dirty (the save engine's
//      matrix-equality compare resolves the "no-op rewrite" case).
//    - ScaleObjectFromAnchor populates SceneEditor::ScaleFromAnchorSet()
//      (pinned 2.8 always-matrix policy).
//    - ClearDirtyState() resets both.
//
//  Spec: docs/ROUND_TRIP_SAVE_PLAN.md §7.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/SceneEditor.h"
#include "../src/Library/SceneEditor/DirtyTracker.h"
#include "../src/Library/SceneEditor/SceneEdit.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;

namespace RISE
{
    bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;
static const char* gCurrentTest = "";

static void Check( bool cond, const char* msg )
{
    if( cond ) {
        passCount++;
    } else {
        failCount++;
        std::cout << "  FAIL [" << gCurrentTest << "]: " << msg << std::endl;
    }
}

static IJobPriv* LoadTwoObjectScene( const char* tag )
{
    char path[512];
    std::snprintf( path, sizeof(path),
        "/tmp/dirty_tracker_test_%s_%d.RISEscene", tag, (int)::getpid() );
    std::ofstream ofs( path );
    if( !ofs.is_open() ) return nullptr;
    ofs << "RISE ASCII SCENE 6\n"
        << "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        << "standard_object\n{\n    name objA\n    geometry sph\n    position 0 0 0\n}\n"
        << "standard_object\n{\n    name objB\n    geometry sph\n    position 5 0 0\n}\n";
    ofs.close();

    IJobPriv* pJob = nullptr;
    if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
        std::remove( path );
        return nullptr;
    }
    const bool ok = pJob->LoadAsciiScene( path );
    std::remove( path );
    if( !ok ) { safe_release( pJob ); return nullptr; }
    return pJob;
}

// --------------------------------------------------------------------

static void TestDirtyTrackerBasicOps()
{
    gCurrentTest = "TestDirtyTrackerBasicOps";
    std::cout << gCurrentTest << "..." << std::endl;
    DirtyTracker dt;
    Check( !dt.IsDirty(), "empty: IsDirty false" );
    Check( dt.Count() == 0, "empty: count 0" );

    dt.MarkDirty( "alpha" );
    Check( dt.IsDirty(), "after MarkDirty: IsDirty true" );
    Check( dt.Contains("alpha"), "Contains alpha" );
    Check( !dt.Contains("beta"), "doesn't contain beta" );
    Check( dt.Count() == 1, "count 1" );

    dt.MarkDirty( "alpha" );  // duplicate insert is idempotent
    Check( dt.Count() == 1, "duplicate MarkDirty is idempotent" );

    dt.MarkDirty( "beta" );
    dt.MarkDirty( "gamma" );
    Check( dt.Count() == 3, "count 3 after three marks" );

    auto snap = dt.Snapshot();
    Check( snap.size() == 3, "snapshot size 3" );
    // Sorted (deterministic)
    Check( snap[0] == "alpha" && snap[1] == "beta" && snap[2] == "gamma",
           "snapshot is sorted" );

    dt.Clear();
    Check( !dt.IsDirty(), "after Clear: IsDirty false" );
    Check( dt.Count() == 0, "after Clear: count 0" );
}

static void TestApplyTransformMarksDirty()
{
    gCurrentTest = "TestApplyTransformMarksDirty";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadTwoObjectScene( "apply_marks" );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) return;

    SceneEditor editor( *pJob->GetScene() );
    Check( !editor.Dirty().IsDirty(), "fresh editor: nothing dirty" );

    // Translate objA — should mark objA dirty.
    SceneEdit e;
    e.op = SceneEdit::TranslateObject;
    e.objectName = "objA";
    e.v3a = Vector3( 1, 2, 3 );
    Check( editor.Apply(e), "translate objA applied" );
    Check( editor.Dirty().Contains("objA"), "objA dirty after translate" );
    Check( !editor.Dirty().Contains("objB"), "objB still clean" );

    // SetObjectStretch on objB — should mark objB dirty.
    SceneEdit e2;
    e2.op = SceneEdit::SetObjectStretch;
    e2.objectName = "objB";
    e2.v3a = Vector3( 2, 2, 2 );
    Check( editor.Apply(e2), "stretch objB applied" );
    Check( editor.Dirty().Contains("objA"), "objA still dirty" );
    Check( editor.Dirty().Contains("objB"), "objB dirty after stretch" );
    Check( editor.Dirty().Count() == 2, "two dirty objects" );

    // Snapshot is sorted.
    auto snap = editor.Dirty().Snapshot();
    Check( snap.size() == 2 && snap[0] == "objA" && snap[1] == "objB",
           "snapshot is sorted" );

    safe_release( pJob );
}

static void TestScaleFromAnchorTracksSeparateSet()
{
    gCurrentTest = "TestScaleFromAnchorTracksSeparateSet";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadTwoObjectScene( "sfa_set" );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) return;

    SceneEditor editor( *pJob->GetScene() );
    Check( editor.ScaleFromAnchorSet().empty(), "fresh: SFA set empty" );

    SceneEdit e;
    e.op = SceneEdit::ScaleObjectFromAnchor;
    e.objectName = "objA";
    e.v3a = Vector3( 2, 2, 2 );
    // ScaleObjectFromAnchor needs prevTransform pre-populated by the
    // controller; pass the current transform as a placeholder.
    // The Apply path doesn't re-capture for SFA, so this works.
    e.prevTransform = Matrix4();  // identity
    Check( editor.Apply(e), "SFA applied" );
    Check( editor.Dirty().Contains("objA"), "objA also in DirtyTracker" );
    Check( editor.ScaleFromAnchorSet().count("objA") == 1,
           "objA in ScaleFromAnchorSet" );
    Check( editor.ScaleFromAnchorSet().count("objB") == 0,
           "objB NOT in ScaleFromAnchorSet" );

    safe_release( pJob );
}

static void TestPropertyOpDoesNotMarkDirty()
{
    gCurrentTest = "TestPropertyOpDoesNotMarkDirty";
    std::cout << gCurrentTest << "..." << std::endl;
    // SetObjectShadowFlags is a property op — must NOT mark dirty per
    // V1 scope (§7.6).
    IJobPriv* pJob = LoadTwoObjectScene( "property_op" );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) return;

    SceneEditor editor( *pJob->GetScene() );

    SceneEdit e;
    e.op = SceneEdit::SetObjectShadowFlags;
    e.objectName = "objA";
    e.s = 3;  // both shadow flags set
    Check( editor.Apply(e), "shadow-flags op applied" );
    Check( !editor.Dirty().IsDirty(),
           "property op did NOT mark dirty" );

    safe_release( pJob );
}

static void TestUndoMarksDirty()
{
    gCurrentTest = "TestUndoMarksDirty";
    std::cout << gCurrentTest << "..." << std::endl;
    // Undo of a transform op still marks dirty (§7.3).  The save
    // engine's matrix-equality compare resolves the no-op-rewrite case.
    IJobPriv* pJob = LoadTwoObjectScene( "undo_marks" );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) return;

    SceneEditor editor( *pJob->GetScene() );

    SceneEdit e;
    e.op = SceneEdit::TranslateObject;
    e.objectName = "objA";
    e.v3a = Vector3( 10, 0, 0 );
    editor.Apply( e );
    Check( editor.Dirty().Contains("objA"), "objA dirty after apply" );

    // Clear and undo — should re-mark dirty.
    editor.ClearDirtyState();
    Check( !editor.Dirty().IsDirty(), "cleared" );
    Check( editor.Undo(), "undo succeeded" );
    Check( editor.Dirty().Contains("objA"), "objA re-marked dirty on undo" );

    // Redo also marks dirty.
    editor.ClearDirtyState();
    Check( editor.Redo(), "redo succeeded" );
    Check( editor.Dirty().Contains("objA"), "objA re-marked dirty on redo" );

    safe_release( pJob );
}

static void TestClearDirtyStateResetsBoth()
{
    gCurrentTest = "TestClearDirtyStateResetsBoth";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadTwoObjectScene( "clear_both" );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) return;

    SceneEditor editor( *pJob->GetScene() );

    SceneEdit e;
    e.op = SceneEdit::ScaleObjectFromAnchor;
    e.objectName = "objA";
    e.v3a = Vector3( 3, 3, 3 );
    e.prevTransform = Matrix4();
    editor.Apply( e );
    Check( editor.Dirty().IsDirty(), "dirty after SFA" );
    Check( !editor.ScaleFromAnchorSet().empty(), "SFA set non-empty" );

    editor.ClearDirtyState();
    Check( !editor.Dirty().IsDirty(), "DirtyTracker cleared" );
    Check( editor.ScaleFromAnchorSet().empty(), "ScaleFromAnchorSet cleared" );

    safe_release( pJob );
}

// --------------------------------------------------------------------

int main()
{
    std::cout << "===== DirtyTrackerTest =====" << std::endl;
    TestDirtyTrackerBasicOps();
    TestApplyTransformMarksDirty();
    TestScaleFromAnchorTracksSeparateSet();
    TestPropertyOpDoesNotMarkDirty();
    TestUndoMarksDirty();
    TestClearDirtyStateResetsBoth();
    std::cout << "passed " << passCount << ", failed " << failCount << std::endl;
    return failCount == 0 ? 0 : 1;
}
