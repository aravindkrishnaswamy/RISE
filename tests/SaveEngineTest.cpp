//////////////////////////////////////////////////////////////////////
//
//  SaveEngineTest.cpp - Phase 6.4 verification.
//
//  Covers the V1 essentials per docs/ROUND_TRIP_SAVE_PLAN.md §12:
//    - Mode A in-place rewrite of a direct, Euler-authored chunk
//    - Mode B managed override block for a FOR-generated entity
//    - Idempotent no-op save (load + no edits → byte-identical file)
//    - Drag → undo → save → NoOp status, byte-identical file
//    - Save outcome statuses (Saved / NoOp / Refused / Failed)
//    - Round-trip stability: save → reload → matrices match
//
//  Each test loads a hand-built scene, exercises SceneEditor edits,
//  invokes SaveEngine::Save, and asserts the result.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/SceneEditor/SaveEngine.h"
#include "../src/Library/SceneEditor/SceneEditor.h"
#include "../src/Library/SceneEditor/SceneEdit.h"
#include "../src/Library/SceneEditor/SourceSpanIndex.h"
#include "../src/Library/SceneEditor/TransformSnapshot.h"
#include "../src/Library/SceneEditor/OverrideSpanIndex.h"
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
    if( cond ) passCount++;
    else {
        failCount++;
        std::cout << "  FAIL [" << gCurrentTest << "]: " << msg << std::endl;
    }
}

// Write `body` (full RISE-ASCII-SCENE-6 content) to a temp file and
// return the path.
static std::string WriteSceneFile( const std::string& body, const char* tag )
{
    char path[512];
    std::snprintf( path, sizeof(path),
        "/tmp/save_engine_test_%s_%d.RISEscene", tag, (int)::getpid() );
    std::ofstream ofs( path );
    ofs << body;
    return std::string( path );
}

static std::string ReadFile( const std::string& path )
{
    std::ifstream ifs( path.c_str(), std::ios::binary );
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static IJobPriv* LoadSceneFromPath( const std::string& path )
{
    IJobPriv* pJob = nullptr;
    if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return nullptr;
    if( !pJob->LoadAsciiScene( path.c_str() ) ) {
        safe_release( pJob );
        return nullptr;
    }
    return pJob;
}

// Wire a SaveEngine from a Job + SceneEditor.
static SaveEngine MakeEngine( IJobPriv& job, SceneEditor& editor,
                              std::unordered_set<std::string>& sfaSetCopy )
{
    // SceneEditor::ScaleFromAnchorSet() is const; SaveEngine wants a
    // non-const reference (it clears it on success).  Copy into a
    // local set for V1 — the controller wires the live reference in
    // Phase 6.5.
    sfaSetCopy = editor.ScaleFromAnchorSet();
    return SaveEngine(
        job,
        *job.GetSourceSpanIndex(),
        *job.GetOverrideSpanIndex(),
        *job.GetBaseTransformSnapshot(),
        *job.GetLoadedTransformSnapshot(),
        editor.Dirty(),
        sfaSetCopy
    );
}

// --------------------------------------------------------------------

static const char* kSceneSimple =
    "RISE ASCII SCENE 6\n"
    "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
    "standard_object\n{\n"
    "    name alpha\n"
    "    geometry sph\n"
    "    position 0 0 0\n"
    "    orientation 0 0 0\n"
    "}\n";

static void TestNoOpSaveIsByteIdentical()
{
    gCurrentTest = "TestNoOpSaveIsByteIdentical";
    std::cout << gCurrentTest << "..." << std::endl;
    const std::string path = WriteSceneFile( kSceneSimple, "noop" );
    const std::string before = ReadFile( path );

    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }

    SceneEditor editor( *pJob->GetScene() );
    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );

    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::NoOp,
           "zero edits → NoOp" );
    Check( r.directRewriteCount == 0 && r.overrideRewriteCount == 0 &&
           r.matrixFallbackCount == 0 && r.noOpCount == 0,
           "all counters zero on no-edit save" );

    const std::string after = ReadFile( path );
    Check( before == after, "file byte-identical after NoOp save" );

    safe_release( pJob );
    std::remove( path.c_str() );
}

static void TestModeASplicePosition()
{
    gCurrentTest = "TestModeASplicePosition";
    std::cout << gCurrentTest << "..." << std::endl;
    const std::string path = WriteSceneFile( kSceneSimple, "modea_pos" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }

    SceneEditor editor( *pJob->GetScene() );
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "alpha";
    e.v3a = Vector3( 1.5, 2.5, 3.5 );
    Check( editor.Apply(e), "SetObjectPosition applied" );

    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Saved, "save succeeded" );
    Check( r.directRewriteCount == 1, "one Mode A rewrite" );
    Check( r.overrideRewriteCount == 0, "no Mode B entries" );

    // Reload and verify the new position landed.
    safe_release( pJob );
    IJobPriv* pJob2 = LoadSceneFromPath( path );
    Check( pJob2 != nullptr, "reload succeeded" );
    if( pJob2 ) {
        IObjectPriv* obj = pJob2->GetObjects()->GetItem( "alpha" );
        Check( obj != nullptr, "alpha found after reload" );
        if( obj ) {
            Matrix4 m = obj->GetFinalTransformMatrix();
            Check( std::fabs(m._30 - 1.5) < 1e-5
                && std::fabs(m._31 - 2.5) < 1e-5
                && std::fabs(m._32 - 3.5) < 1e-5,
                "reloaded position == (1.5, 2.5, 3.5)" );
        }
        safe_release( pJob2 );
    }
    std::remove( path.c_str() );
}

static void TestDragUndoSaveIsNoOp()
{
    gCurrentTest = "TestDragUndoSaveIsNoOp";
    std::cout << gCurrentTest << "..." << std::endl;
    const std::string path = WriteSceneFile( kSceneSimple, "drag_undo" );
    const std::string before = ReadFile( path );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }

    SceneEditor editor( *pJob->GetScene() );
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "alpha";
    e.v3a = Vector3( 10, 0, 0 );
    editor.Apply( e );
    Check( editor.Undo(), "undo succeeded" );

    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::NoOp,
           "drag → undo → save → NoOp" );
    Check( r.noOpCount == 1, "noOpCount == 1 (DirtyTracker had alpha)" );

    const std::string after = ReadFile( path );
    Check( before == after, "file byte-identical after drag/undo/save" );

    safe_release( pJob );
    std::remove( path.c_str() );
}

static void TestForLoopGoesToModeB()
{
    gCurrentTest = "TestForLoopGoesToModeB";
    std::cout << gCurrentTest << "..." << std::endl;
    // FOR-generated object has chunkRevisited=true on its first
    // SourceSpan (and no SourceSpan at all for 2..N).  Save engine
    // forces matrix-form override for these — Mode B managed block.
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "FOR I 0 2 1\n"
        "standard_object\n{\n"
        "    name sphere_!I\n"
        "    geometry sph\n"
        "    position !I 0 0\n"
        "}\n"
        "ENDFOR\n";
    const std::string path = WriteSceneFile( body, "for_modeb" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }

    // Edit sphere_0001 (an FOR 2..N entity — no SourceSpan).
    SceneEditor editor( *pJob->GetScene() );
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "sphere_0001";
    e.v3a = Vector3( 99, 88, 77 );
    Check( editor.Apply(e), "edit on FOR 2..N entity" );

    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Saved, "save succeeded" );
    Check( r.matrixFallbackCount == 1, "matrix-form override emitted" );
    Check( r.directRewriteCount == 0, "no Mode A rewrite" );

    // Saved file should contain the sentinel-bracketed managed block.
    const std::string after = ReadFile( path );
    Check( after.find("RISE editor overrides") != std::string::npos,
           "managed sentinel present" );
    Check( after.find("override_object") != std::string::npos,
           "override_object chunk emitted" );

    // Reload and verify sphere_0001's position is (99, 88, 77).
    safe_release( pJob );
    IJobPriv* pJob2 = LoadSceneFromPath( path );
    if( pJob2 ) {
        IObjectPriv* obj = pJob2->GetObjects()->GetItem( "sphere_0001" );
        if( obj ) {
            Matrix4 m = obj->GetFinalTransformMatrix();
            Check( std::fabs(m._30 - 99.0) < 1e-5,
                "reloaded sphere_0001 x == 99" );
            Check( std::fabs(m._31 - 88.0) < 1e-5,
                "reloaded sphere_0001 y == 88" );
        }
        safe_release( pJob2 );
    }
    std::remove( path.c_str() );
}

static void TestSecondSaveAfterModeBIsByteIdentical()
{
    gCurrentTest = "TestSecondSaveAfterModeBIsByteIdentical";
    std::cout << gCurrentTest << "..." << std::endl;
    // Save once with a managed block, reload, save again with no
    // edits — should be byte-identical (round-trip stability §8.8).
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "FOR I 0 1 1\n"
        "standard_object\n{\n    name s_!I\n    geometry sph\n    position !I 0 0\n}\n"
        "ENDFOR\n";
    const std::string path = WriteSceneFile( body, "roundtrip" );
    {
        IJobPriv* pJob = LoadSceneFromPath( path );
        SceneEditor editor( *pJob->GetScene() );
        SceneEdit e;
        e.op = SceneEdit::SetObjectPosition;
        e.objectName = "s_0001";
        e.v3a = Vector3( 5, 0, 0 );
        editor.Apply( e );
        std::unordered_set<std::string> sfa;
        SaveEngine engine = MakeEngine( *pJob, editor, sfa );
        engine.Save( path );
        safe_release( pJob );
    }
    const std::string afterFirst = ReadFile( path );

    // Second save with no further edits → byte-identical.
    {
        IJobPriv* pJob = LoadSceneFromPath( path );
        SceneEditor editor( *pJob->GetScene() );
        std::unordered_set<std::string> sfa;
        SaveEngine engine = MakeEngine( *pJob, editor, sfa );
        SaveResult r = engine.Save( path );
        Check( r.status == SaveResult::Status::NoOp,
               "second save: NoOp (no edits)" );
        safe_release( pJob );
    }
    const std::string afterSecond = ReadFile( path );
    Check( afterFirst == afterSecond,
           "byte-identical after second save (round-trip stability)" );
    std::remove( path.c_str() );
}

static void TestFailedOnNonexistentPath()
{
    gCurrentTest = "TestFailedOnNonexistentPath";
    std::cout << gCurrentTest << "..." << std::endl;
    // Save targeting a path that doesn't exist (the engine reads
    // the file first) → Failed.
    const std::string path = WriteSceneFile( kSceneSimple, "fail_nopath" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );
    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( "/tmp/does_not_exist/save_engine_no.RISEscene" );
    Check( r.status == SaveResult::Status::Failed,
           "save to missing dir → Failed" );
    Check( !r.errorMessage.empty(), "error message populated" );
    safe_release( pJob );
    std::remove( path.c_str() );
}

// --------------------------------------------------------------------
// Coverage added in response to Phase 6.4 adversarial review.
// P1-A: rotation round-trip (was failing silently in Y due to wrong
//       hand-derived Rx·Ry·Rz formulas).  P3-A.
// P3-B: matrix-authored + quaternion-authored chunks force matrix-form.
// P3-C: ScaleObjectFromAnchor path forces matrix-form.
// P3-G: insert-new-line path (chunk missing a transform field, edit
//       fills it in).
// P3-H: multi-field Mode A with differing value lengths.

static void TestRotationRoundTripPureY()
{
    gCurrentTest = "TestRotationRoundTripPureY";
    std::cout << gCurrentTest << "..." << std::endl;
    // P1-A: pure-Y rotation must survive save → reload byte-cycle.
    // Pre-fix, TryDecompose had sign-flipped formulas that masked
    // themselves via an equally-wrong rebuild check, so Y-rotations
    // saved as `(x, -y, z)` and reloaded as mirror-Y geometry.
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "standard_object\n{\n"
        "    name yrot\n"
        "    geometry sph\n"
        "    position 0 0 0\n"
        "    orientation 0 30 0\n"
        "}\n";
    const std::string path = WriteSceneFile( body, "yrot" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }

    // Edit position (changes Mfinal but keeps orientation intact).
    SceneEditor editor( *pJob->GetScene() );
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "yrot";
    e.v3a = Vector3( 5, 0, 0 );
    editor.Apply( e );

    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Saved, "saved" );
    safe_release( pJob );

    // Reload and compare orientation.
    IJobPriv* pJob2 = LoadSceneFromPath( path );
    Check( pJob2 != nullptr, "reload succeeded" );
    if( pJob2 ) {
        IObjectPriv* obj = pJob2->GetObjects()->GetItem( "yrot" );
        Check( obj != nullptr, "yrot present after reload" );
        if( obj ) {
            const Matrix4 m = obj->GetFinalTransformMatrix();
            // Position should be (5, 0, 0) — translation column.
            Check( std::fabs(m._30 - 5.0) < 1e-5, "translation x = 5" );
            // Orientation matrix block should match what 30° Y rotation
            // produces.  XRotation(0) * YRotation(30°) * ZRotation(0):
            //   _00 =  cos(30°)
            //   _02 = -sin(30°)
            //   _20 =  sin(30°)
            //   _22 =  cos(30°)
            const double cos30 = std::cos( 30.0 * 3.14159265358979 / 180.0 );
            const double sin30 = std::sin( 30.0 * 3.14159265358979 / 180.0 );
            Check( std::fabs(m._00 - cos30) < 1e-5, "R[0][0] preserved (cos30)" );
            Check( std::fabs(m._20 - sin30) < 1e-5, "R[0][2] preserved (sin30)" );
            Check( std::fabs(m._22 - cos30) < 1e-5, "R[2][2] preserved (cos30)" );
        }
        safe_release( pJob2 );
    }
    std::remove( path.c_str() );
}

static void TestMatrixAuthoredForcesMatrixOverride()
{
    gCurrentTest = "TestMatrixAuthoredForcesMatrixOverride";
    std::cout << gCurrentTest << "..." << std::endl;
    // P3-B: chunk authored via `matrix` keyword → save uses matrix-form
    // override even when Mfinal is decomposable.
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "standard_object\n{\n"
        "    name m\n"
        "    geometry sph\n"
        "    matrix 1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1\n"
        "}\n";
    const std::string path = WriteSceneFile( body, "mtx" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "m";
    e.v3a = Vector3( 1, 2, 3 );
    editor.Apply( e );
    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Saved, "saved" );
    Check( r.matrixFallbackCount == 1, "force-matrix gate fired" );
    Check( r.directRewriteCount == 0, "no Mode A rewrite" );
    safe_release( pJob );
    std::remove( path.c_str() );
}

static void TestScaleFromAnchorForcesMatrixOverride()
{
    gCurrentTest = "TestScaleFromAnchorForcesMatrixOverride";
    std::cout << gCurrentTest << "..." << std::endl;
    // P3-C: SFA op puts object in mScaleFromAnchorSet → save emits
    // matrix-form even when the result is cleanly decomposable.
    const std::string path = WriteSceneFile( kSceneSimple, "sfa" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );
    // SFA needs a prevTransform; the obj's current transform is identity.
    SceneEdit e;
    e.op = SceneEdit::ScaleObjectFromAnchor;
    e.objectName = "alpha";
    e.v3a = Vector3( 2, 2, 2 );
    e.prevTransform = Matrix4();
    editor.Apply( e );
    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Saved, "saved" );
    Check( r.matrixFallbackCount == 1, "SFA → matrix-form" );
    safe_release( pJob );
    std::remove( path.c_str() );
}

static void TestModeAInsertNewLine()
{
    gCurrentTest = "TestModeAInsertNewLine";
    std::cout << gCurrentTest << "..." << std::endl;
    // P3-G: source chunk has NO `orientation` line.  User edits
    // orientation via gizmo → engine should INSERT a new line just
    // before the closing brace, using the same chunk's indentation.
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "standard_object\n{\n"
        "    name ins\n"
        "    geometry sph\n"
        "    position 0 0 0\n"
        "}\n";
    const std::string path = WriteSceneFile( body, "insert" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );
    SceneEdit e;
    e.op = SceneEdit::SetObjectOrientation;
    e.objectName = "ins";
    // orientation values are RADIANS at the editor layer; the
    // save engine converts to degrees on emit.  Use 30° = π/6.
    e.v3a = Vector3( 0, 30.0 * 3.14159265358979 / 180.0, 0 );
    editor.Apply( e );
    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Saved, "saved" );
    Check( r.directRewriteCount >= 1, "at least one Mode A rewrite" );

    // Saved file should now have an `orientation` line.
    const std::string after = ReadFile( path );
    Check( after.find("orientation") != std::string::npos,
           "orientation line inserted into source chunk" );
    // Reload + verify the orientation took effect.
    safe_release( pJob );
    IJobPriv* pJob2 = LoadSceneFromPath( path );
    if( pJob2 ) {
        IObjectPriv* obj = pJob2->GetObjects()->GetItem( "ins" );
        if( obj ) {
            const Matrix4 m = obj->GetFinalTransformMatrix();
            // 30° Y rotation: _00 = cos(30°).
            const double cos30 = std::cos( 30.0 * 3.14159265358979 / 180.0 );
            Check( std::fabs(m._00 - cos30) < 1e-4,
                   "reloaded orientation reflects 30° Y rotation" );
        }
        safe_release( pJob2 );
    }
    std::remove( path.c_str() );
}

static void TestMultiFieldModeAValueLengthDelta()
{
    gCurrentTest = "TestMultiFieldModeAValueLengthDelta";
    std::cout << gCurrentTest << "..." << std::endl;
    // P3-H / pinned 2.21: multiple Mode A splices on one chunk where
    // the replacement values have DIFFERENT lengths from the originals.
    // Descending-`begin` apply must keep all ParameterSpan offsets
    // valid even when an earlier splice's `replacement.size() !=
    // end - begin`.
    //
    // Position 0 0 0 → -1.23456 2.5 0   (+9 chars)
    // Orientation 0 0 0 → 0 0 0   (unchanged length; just keep it set)
    // Scale 1 1 1 → 0.5 0.5 0.5   (+6 chars)
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "standard_object\n{\n"
        "    name multi\n"
        "    geometry sph\n"
        "    position 0 0 0\n"
        "    orientation 0 0 0\n"
        "    scale 1 1 1\n"
        "}\n";
    const std::string path = WriteSceneFile( body, "multifield" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "scene loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );

    SceneEdit pos;
    pos.op = SceneEdit::SetObjectPosition;
    pos.objectName = "multi";
    pos.v3a = Vector3( -1.23456, 2.5, 0 );
    editor.Apply( pos );

    SceneEdit scl;
    scl.op = SceneEdit::SetObjectStretch;
    scl.objectName = "multi";
    scl.v3a = Vector3( 0.5, 0.5, 0.5 );
    editor.Apply( scl );

    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Saved, "saved" );
    Check( r.directRewriteCount == 2, "two Mode A rewrites" );

    // Reload + verify both fields landed correctly.
    safe_release( pJob );
    IJobPriv* pJob2 = LoadSceneFromPath( path );
    if( pJob2 ) {
        IObjectPriv* obj = pJob2->GetObjects()->GetItem( "multi" );
        if( obj ) {
            const Matrix4 m = obj->GetFinalTransformMatrix();
            Check( std::fabs(m._30 - (-1.23456)) < 1e-4, "position x preserved" );
            Check( std::fabs(m._31 - 2.5)        < 1e-4, "position y preserved" );
            // Stretch: with identity rotation, m._00 = stretch.x = 0.5.
            Check( std::fabs(m._00 - 0.5) < 1e-4, "stretch x preserved" );
            Check( std::fabs(m._11 - 0.5) < 1e-4, "stretch y preserved" );
        }
        safe_release( pJob2 );
    }
    std::remove( path.c_str() );
}

// --------------------------------------------------------------------
// R-final review fixes:
//   P1 #2 — quaternion field round-trips through OverrideEntry
//   P1 #3 — external-modification (mtime) guard refuses save
//   P2    — sentinel comment that contains the marker substring
//           but doesn't EXACTLY match must not be classified managed

static void TestManagedBlockWithQuaternionRoundTrips()
{
    gCurrentTest = "TestManagedBlockWithQuaternionRoundTrips";
    std::cout << gCurrentTest << "..." << std::endl;
    // R-final P1 #2: a managed block whose only field is `quaternion`
    // must survive a load → no-edits → save round-trip.  Pre-fix,
    // OverrideEntry didn't have a quaternion field, so the seeding
    // pass dropped it and the second save emitted an empty managed
    // block.
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "standard_object\n{\n    name q\n    geometry sph\n    position 0 0 0\n}\n"
        "# === RISE editor overrides (managed by interactive editor) ===\n"
        "override_object\n{\n    name q\n    quaternion 0 0 0.7071 0.7071\n}\n"
        "# === end RISE editor overrides ===\n";
    const std::string path = WriteSceneFile( body, "quat_roundtrip" );

    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );
    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::NoOp || r.status == SaveResult::Status::Saved,
           "save succeeded (NoOp or Saved)" );
    safe_release( pJob );

    // Reload and confirm the override applied.
    IJobPriv* pJob2 = LoadSceneFromPath( path );
    Check( pJob2 != nullptr, "reload" );
    if( pJob2 ) {
        const OverrideSpanIndex* idx = pJob2->GetOverrideSpanIndex();
        Check( idx && idx->Count() == 1, "one override record persists" );
        if( idx && idx->Count() == 1 ) {
            const OverrideRecord& rec = idx->Entries()[0];
            Check( rec.hasQuaternion, "quaternion field survived round-trip" );
            Check( std::fabs(rec.quaternion[2] - 0.7071) < 1e-3,
                   "quaternion z value preserved" );
            Check( std::fabs(rec.quaternion[3] - 0.7071) < 1e-3,
                   "quaternion w value preserved" );
        }
        safe_release( pJob2 );
    }
    std::remove( path.c_str() );
}

static void TestExternalModificationGuardRefuses()
{
    gCurrentTest = "TestExternalModificationGuardRefuses";
    std::cout << gCurrentTest << "..." << std::endl;
    // R-final P1 #3: external edit between load and save must refuse
    // the save (its byte-offset assumptions would corrupt the file).
    const std::string path = WriteSceneFile( kSceneSimple, "extmod" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "alpha";
    e.v3a = Vector3( 5, 0, 0 );
    editor.Apply( e );

    // Simulate external edit: append a stray line + sleep enough for
    // mtime resolution (POSIX mtime is whole-second on many FSes —
    // pad by 1s plus a margin to ensure the mtime tick advances).
    std::this_thread::sleep_for( std::chrono::milliseconds( 1100 ) );
    {
        std::ofstream ofs( path.c_str(), std::ios::out | std::ios::app );
        ofs << "# tampered by external editor\n";
    }

    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Refused,
           "external mod → Refused" );
    Check( r.errorMessage.find( "modified externally" ) != std::string::npos,
           "diagnostic mentions external modification" );
    Check( editor.Dirty().IsDirty(),
           "DirtyTracker retained on Refused" );
    safe_release( pJob );
    std::remove( path.c_str() );
}

static void TestSubstringCommentDoesNotFlipManagedFlag()
{
    gCurrentTest = "TestSubstringCommentDoesNotFlipManagedFlag";
    std::cout << gCurrentTest << "..." << std::endl;
    // R-final P2: pre-fix, a comment line containing the substring
    // "RISE editor overrides" but NOT the exact sentinel string would
    // toggle `mInsideManagedOverrideBlock`, causing subsequent hand-
    // written override_objects to be classified `managed = true`
    // even though LocateManagedBlock (exact-line match) saw no
    // managed block.  Post-fix, both sides require exact-line match
    // — so a hand-written override AFTER such a comment stays
    // classified unmanaged.
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "standard_object\n{\n    name s\n    geometry sph\n    position 0 0 0\n}\n"
        "# TODO: RISE editor overrides go here at some point\n"
        "override_object\n{\n    name s\n    position 1 0 0\n}\n";
    const std::string path = WriteSceneFile( body, "substring_comment" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    const OverrideSpanIndex* idx = pJob->GetOverrideSpanIndex();
    Check( idx && idx->Count() == 1, "one override record" );
    if( idx && idx->Count() == 1 ) {
        Check( !idx->Entries()[0].managed,
               "override after substring-only comment stays unmanaged" );
        Check( idx->HasUnmanagedFor("s"),
               "unmanaged override visible to save engine" );
    }
    safe_release( pJob );
    std::remove( path.c_str() );
}

static void TestSecondSaveAfterLongerModeAValueWithoutReload()
{
    gCurrentTest = "TestSecondSaveAfterLongerModeAValueWithoutReload";
    std::cout << gCurrentTest << "..." << std::endl;
    // R-final P1 #1: after a Mode A splice that LENGTHENS a value
    // line, the in-memory SourceSpanIndex must adjust its byte
    // offsets so a subsequent save-without-reload splices at the
    // correct (post-write) positions.  Pre-fix, the second save
    // used load-time offsets that now pointed at content shifted
    // by the first splice's length delta — corruption in
    // unrelated bytes.
    //
    // Scene: one chunk with position + orientation lines.  First
    // save expands `position 0 0 0` (5 chars) to `-1.23456 2 3.5`
    // (14 chars, +9 bytes).  Second save (NO reload) then edits
    // orientation, whose original valueBegin was at some offset
    // past `position 0 0 0` — that offset must now be 9 bytes
    // higher in the file.
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "standard_object\n{\n"
        "    name obj\n"
        "    geometry sph\n"
        "    position 0 0 0\n"
        "    orientation 0 0 0\n"
        "}\n";
    const std::string path = WriteSceneFile( body, "second_save" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );

    // First save: lengthen the position line.
    SceneEdit e1;
    e1.op = SceneEdit::SetObjectPosition;
    e1.objectName = "obj";
    e1.v3a = Vector3( -1.23456, 2.0, 3.5 );
    editor.Apply( e1 );
    std::unordered_set<std::string> sfa1;
    SaveEngine engine1 = MakeEngine( *pJob, editor, sfa1 );
    SaveResult r1 = engine1.Save( path );
    Check( r1.status == SaveResult::Status::Saved, "first save: Saved" );
    Check( r1.directRewriteCount == 1, "one Mode A rewrite" );

    // Mtime needs to tick for the guard to accept the second save.
    std::this_thread::sleep_for( std::chrono::milliseconds( 1100 ) );

    // Second save WITHOUT reload: edit orientation.
    SceneEdit e2;
    e2.op = SceneEdit::SetObjectOrientation;
    e2.objectName = "obj";
    // 45 degrees Y in radians.
    e2.v3a = Vector3( 0, 45.0 * 3.14159265358979 / 180.0, 0 );
    editor.Apply( e2 );

    // Critical: the FileIdentity guard would block here if the post-
    // first-save refresh didn't happen.  And the offset adjustment
    // must have run, otherwise the orientation splice would write
    // 9 bytes earlier than the actual `orientation` line.
    std::unordered_set<std::string> sfa2;
    SaveEngine engine2 = MakeEngine( *pJob, editor, sfa2 );
    SaveResult r2 = engine2.Save( path );
    Check( r2.status == SaveResult::Status::Saved, "second save: Saved" );
    Check( r2.directRewriteCount == 1, "second save: one Mode A rewrite" );

    // Reload from disk and verify BOTH edits stuck.
    safe_release( pJob );
    IJobPriv* pJob3 = LoadSceneFromPath( path );
    Check( pJob3 != nullptr, "reload" );
    if( pJob3 ) {
        IObjectPriv* obj = pJob3->GetObjects()->GetItem( "obj" );
        Check( obj != nullptr, "obj reloaded" );
        if( obj ) {
            const Matrix4 m = obj->GetFinalTransformMatrix();
            // First save's position values.
            Check( std::fabs(m._30 - (-1.23456)) < 1e-4,
                   "first save's position x persisted" );
            Check( std::fabs(m._31 - 2.0) < 1e-4,
                   "first save's position y persisted" );
            Check( std::fabs(m._32 - 3.5) < 1e-4,
                   "first save's position z persisted" );
            // Second save's orientation: 45° around Y.
            // YRotation(45°): _00 = cos45, _20 = sin45.
            const double c45 = std::cos( 45.0 * 3.14159265358979 / 180.0 );
            const double s45 = std::sin( 45.0 * 3.14159265358979 / 180.0 );
            Check( std::fabs(m._00 - c45) < 1e-4,
                   "second save's orientation _00 preserved (cos45)" );
            Check( std::fabs(m._20 - s45) < 1e-4,
                   "second save's orientation _20 preserved (sin45)" );
        }
        // Also verify the saved file's text is well-formed (no byte
        // corruption from a stale-offset splice).
        const std::string final = ReadFile( path );
        Check( final.find("position -1.23456 2 3.5") != std::string::npos,
               "position line is intact after second save" );
        Check( final.find("orientation 0 45 0") != std::string::npos
            || final.find("orientation -0 45 -0") != std::string::npos,
               "orientation line was rewritten cleanly (no surrounding bytes corrupted)" );
        safe_release( pJob3 );
    }
    std::remove( path.c_str() );
}

static void TestSecondSaveAfterModeBSameSessionNoReload()
{
    gCurrentTest = "TestSecondSaveAfterModeBSameSessionNoReload";
    std::cout << gCurrentTest << "..." << std::endl;
    // R-final-3 P1: same-session Mode B save must not erase the
    // managed block on a follow-up save with no further edits.
    //
    // Pre-fix: after the first save inserted a managed block,
    // `mOverrideSpans` was still the PARSE-TIME catalog (which had no
    // managed records, since the load-time file had no block).  On a
    // second save in the same editor session with no dirty edits,
    // the engine's accumulator seed from FindManaged stayed empty,
    // dirtyNames was empty, and LocateManagedBlock(bytes) found the
    // block-on-disk — EditScript Case (b) erased it.  Reload would
    // then lose the override.
    //
    // Fix: after a successful save, rebuild OverrideSpanIndex so its
    // managed catalog mirrors what's actually in the file.  A second
    // save then re-seeds the same accumulator, re-renders the same
    // block bytes, and short-circuits to NoOp via byte-identity.
    //
    // FOR-generated entity → chunkRevisited true → forces Mode B.
    const std::string body =
        "RISE ASCII SCENE 6\n"
        "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
        "FOR I 0 1 1\n"
        "standard_object\n{\n    name s_!I\n    geometry sph\n    position !I 0 0\n}\n"
        "ENDFOR\n";
    const std::string path = WriteSceneFile( body, "modeb_samesession" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );

    // First save: drag the FOR-body entity so Mfinal != Mbase → Mode B.
    SceneEdit e1;
    e1.op = SceneEdit::SetObjectPosition;
    e1.objectName = "s_0001";
    e1.v3a = Vector3( 5, 0, 0 );
    editor.Apply( e1 );
    std::unordered_set<std::string> sfa1;
    SaveEngine engine1 = MakeEngine( *pJob, editor, sfa1 );
    SaveResult r1 = engine1.Save( path );
    Check( r1.status == SaveResult::Status::Saved, "first save: Saved" );
    // FOR-body entity → chunkRevisited → forceMatrixOverride →
    // matrixFallbackCount, not overrideRewriteCount.  Either signals
    // a managed block emission; both ARE per-field/per-fallback
    // counters off the same accumulator.
    Check( r1.matrixFallbackCount >= 1 || r1.overrideRewriteCount >= 1,
           "first save: managed entry emitted (matrix-fallback or per-field)" );

    const std::string afterFirst = ReadFile( path );
    Check( afterFirst.find("RISE editor overrides") != std::string::npos,
           "first save inserted managed block sentinel" );
    Check( afterFirst.find("override_object") != std::string::npos,
           "first save wrote an override_object chunk" );

    // Mtime needs to tick for the external-modification guard.
    std::this_thread::sleep_for( std::chrono::milliseconds( 1100 ) );

    // Second save in the SAME editor session, NO reload, NO new edits.
    // Pre-fix this erased the managed block; post-fix it's a NoOp
    // because the OverrideSpanIndex was rebuilt to mirror what's
    // already on disk.
    std::unordered_set<std::string> sfa2;
    SaveEngine engine2 = MakeEngine( *pJob, editor, sfa2 );
    SaveResult r2 = engine2.Save( path );
    Check( r2.status == SaveResult::Status::NoOp,
           "second save (same session, no edits): NoOp" );

    const std::string afterSecond = ReadFile( path );
    Check( afterSecond == afterFirst,
           "byte-identical after same-session second save (block preserved)" );
    Check( afterSecond.find("RISE editor overrides") != std::string::npos,
           "managed block sentinel STILL present after second save" );
    Check( afterSecond.find("override_object") != std::string::npos,
           "override_object chunk STILL present after second save" );

    // Belt-and-suspenders: a THIRD same-session save with no edits
    // must also be a NoOp.  Catches the case where rebuild produces
    // a managed catalog that subtly differs from the first save's
    // emit (e.g., a normalisation bug that flips a NoOp into a
    // length-zero replace-with-identical-bytes splice).  Even
    // identical-bytes-replace gets caught by the byte-identity NoOp
    // guard, but the third call asserts the steady-state property.
    std::this_thread::sleep_for( std::chrono::milliseconds( 1100 ) );
    std::unordered_set<std::string> sfa3;
    SaveEngine engine3 = MakeEngine( *pJob, editor, sfa3 );
    SaveResult r3 = engine3.Save( path );
    Check( r3.status == SaveResult::Status::NoOp,
           "third save (same session): also NoOp" );
    const std::string afterThird = ReadFile( path );
    Check( afterThird == afterFirst,
           "byte-identical after third same-session save" );

    // Finally reload and confirm the override survived the trip.
    safe_release( pJob );
    IJobPriv* pJob2 = LoadSceneFromPath( path );
    Check( pJob2 != nullptr, "reload after multi-save sequence" );
    if( pJob2 ) {
        IObjectPriv* obj = pJob2->GetObjects()->GetItem( "s_0001" );
        Check( obj != nullptr, "s_0001 reloaded" );
        if( obj ) {
            const Matrix4 m = obj->GetFinalTransformMatrix();
            Check( std::fabs(m._30 - 5.0) < 1e-4,
                   "override's position x persisted through three saves" );
        }
        safe_release( pJob2 );
    }
    std::remove( path.c_str() );
}

static void TestSaveClearsDirtyTracker()
{
    gCurrentTest = "TestSaveClearsDirtyTracker";
    std::cout << gCurrentTest << "..." << std::endl;
    const std::string path = WriteSceneFile( kSceneSimple, "clears_dirty" );
    IJobPriv* pJob = LoadSceneFromPath( path );
    Check( pJob != nullptr, "loaded" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    SceneEditor editor( *pJob->GetScene() );
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "alpha";
    e.v3a = Vector3( 2, 0, 0 );
    editor.Apply( e );
    Check( editor.Dirty().IsDirty(), "dirty before save" );
    std::unordered_set<std::string> sfa;
    SaveEngine engine = MakeEngine( *pJob, editor, sfa );
    SaveResult r = engine.Save( path );
    Check( r.status == SaveResult::Status::Saved, "saved" );
    Check( !editor.Dirty().IsDirty(),
           "DirtyTracker cleared on successful save" );
    safe_release( pJob );
    std::remove( path.c_str() );
}

// --------------------------------------------------------------------

int main()
{
    std::cout << "===== SaveEngineTest =====" << std::endl;
    TestNoOpSaveIsByteIdentical();
    TestModeASplicePosition();
    TestDragUndoSaveIsNoOp();
    TestForLoopGoesToModeB();
    TestSecondSaveAfterModeBIsByteIdentical();
    TestFailedOnNonexistentPath();
    TestRotationRoundTripPureY();
    TestMatrixAuthoredForcesMatrixOverride();
    TestScaleFromAnchorForcesMatrixOverride();
    TestModeAInsertNewLine();
    TestMultiFieldModeAValueLengthDelta();
    TestManagedBlockWithQuaternionRoundTrips();
    TestExternalModificationGuardRefuses();
    TestSubstringCommentDoesNotFlipManagedFlag();
    TestSecondSaveAfterLongerModeAValueWithoutReload();
    TestSecondSaveAfterModeBSameSessionNoReload();
    TestSaveClearsDirtyTracker();
    std::cout << "passed " << passCount << ", failed " << failCount << std::endl;
    return failCount == 0 ? 0 : 1;
}
