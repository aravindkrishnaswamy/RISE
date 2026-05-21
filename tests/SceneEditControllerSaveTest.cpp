//////////////////////////////////////////////////////////////////////
//
//  SceneEditControllerSaveTest.cpp - Phase 6.5 controller-level
//    integration test: drive SceneEditController::RequestSave and
//    verify the cancel-and-park + post-save state-clear semantics.
//
//  Spec: docs/ROUND_TRIP_SAVE_PLAN.md §9.9 + §10.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/SceneEditor.h"
#include "../src/Library/SceneEditor/SaveEngine.h"
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

static std::string WriteScene( const std::string& body, const char* tag )
{
    char path[512];
    std::snprintf( path, sizeof(path),
        "/tmp/controller_save_test_%s_%d.RISEscene", tag, (int)::getpid() );
    std::ofstream ofs( path );
    ofs << body;
    return std::string( path );
}

static const char* kScene =
    "RISE ASCII SCENE 6\n"
    "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
    "standard_object\n{\n"
    "    name a\n"
    "    geometry sph\n"
    "    position 0 0 0\n"
    "}\n";

// --------------------------------------------------------------------

static void TestRequestSaveSucceedsOnDirtyEdit()
{
    gCurrentTest = "TestRequestSaveSucceedsOnDirtyEdit";
    std::cout << gCurrentTest << "..." << std::endl;
    const std::string path = WriteScene( kScene, "dirty" );
    IJobPriv* pJob = nullptr;
    Check( RISE_CreateJobPriv(&pJob) && pJob, "job created" );
    if( !pJob ) { std::remove(path.c_str()); return; }
    Check( pJob->LoadAsciiScene( path.c_str() ), "scene loaded" );

    // Controller takes a rasterizer arg; nullptr is acceptable for
    // test mode (no render thread interaction needed for this test).
    SceneEditController ctrl( *pJob, nullptr );

    // Apply an edit through the controller's editor.
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "a";
    e.v3a = Vector3( 7, 0, 0 );
    Check( ctrl.Editor().Apply(e), "edit applied" );
    Check( ctrl.Editor().Dirty().IsDirty(), "dirty before save" );

    SaveResult r = ctrl.RequestSave( path );
    Check( r.status == SaveResult::Status::Saved, "saved" );
    Check( !ctrl.Editor().Dirty().IsDirty(),
           "DirtyTracker cleared after successful save" );
    Check( !ctrl.IsSaving(), "mSaving cleared after save" );
    Check( ctrl.LastSaveError().empty(), "no error message" );

    safe_release( pJob );
    std::remove( path.c_str() );
}

static void TestRequestSaveNoOpOnNoEdits()
{
    gCurrentTest = "TestRequestSaveNoOpOnNoEdits";
    std::cout << gCurrentTest << "..." << std::endl;
    const std::string path = WriteScene( kScene, "noop" );
    std::string before;
    {
        std::ifstream ifs( path.c_str(), std::ios::binary );
        std::stringstream ss; ss << ifs.rdbuf();
        before = ss.str();
    }

    IJobPriv* pJob = nullptr;
    RISE_CreateJobPriv( &pJob );
    pJob->LoadAsciiScene( path.c_str() );
    SceneEditController ctrl( *pJob, nullptr );
    SaveResult r = ctrl.RequestSave( path );
    Check( r.status == SaveResult::Status::NoOp, "NoOp on no edits" );
    Check( ctrl.LastSaveError().empty(), "no error" );
    Check( !ctrl.IsSaving(), "mSaving cleared" );

    // File untouched.
    std::ifstream ifs2( path.c_str(), std::ios::binary );
    std::stringstream ss2; ss2 << ifs2.rdbuf();
    Check( ss2.str() == before, "file byte-identical after NoOp" );

    safe_release( pJob );
    std::remove( path.c_str() );
}

static void TestRequestSaveFailedSetsLastSaveError()
{
    gCurrentTest = "TestRequestSaveFailedSetsLastSaveError";
    std::cout << gCurrentTest << "..." << std::endl;
    const std::string path = WriteScene( kScene, "fail" );
    IJobPriv* pJob = nullptr;
    RISE_CreateJobPriv( &pJob );
    pJob->LoadAsciiScene( path.c_str() );
    SceneEditController ctrl( *pJob, nullptr );
    SceneEdit e;
    e.op = SceneEdit::SetObjectPosition;
    e.objectName = "a";
    e.v3a = Vector3( 1, 0, 0 );
    ctrl.Editor().Apply( e );

    SaveResult r = ctrl.RequestSave( "/tmp/does_not_exist/file.RISEscene" );
    Check( r.status == SaveResult::Status::Failed, "save to bad path → Failed" );
    Check( !ctrl.LastSaveError().empty(), "LastSaveError populated" );
    Check( ctrl.Editor().Dirty().IsDirty(),
           "DirtyTracker retained after Failed save" );

    safe_release( pJob );
    std::remove( path.c_str() );
}

// --------------------------------------------------------------------

int main()
{
    std::cout << "===== SceneEditControllerSaveTest =====" << std::endl;
    TestRequestSaveSucceedsOnDirtyEdit();
    TestRequestSaveNoOpOnNoEdits();
    TestRequestSaveFailedSetsLastSaveError();
    std::cout << "passed " << passCount << ", failed " << failCount << std::endl;
    return failCount == 0 ? 0 : 1;
}
