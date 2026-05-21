//////////////////////////////////////////////////////////////////////
//
//  OverrideObjectParserTest.cpp - Phase 6.2 verification.
//
//  Loads scene files that contain `override_object` chunks and asserts:
//    - The chunk parses successfully, target lookup works, and the
//      object's runtime transform reflects the override's applied
//      values (per-field path, quaternion path, matrix path).
//    - Missing-target is a hard parse error.
//    - Empty chunk is a soft warning (parse succeeds, no mutation).
//    - LoadedTransformSnapshot reflects the post-override state while
//      BaseTransformSnapshot reflects the pre-override state.
//    - OverrideSpanIndex records every chunk with the correct
//      managed/unmanaged classification.
//
//  Spec: docs/ROUND_TRIP_SAVE_PLAN.md §8.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
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
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/SceneEditor/OverrideSpanIndex.h"
#include "../src/Library/SceneEditor/SourceSpanIndex.h"
#include "../src/Library/SceneEditor/TransformSnapshot.h"

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

static bool CloseEnough( double a, double b, double tol = 1e-9 )
{
    return std::fabs( a - b ) <= tol;
}

// Standard scene preamble: minimal sphere geometry.
static const char* kPreamble =
    "RISE ASCII SCENE 6\n"
    "sphere_geometry\n{\n"
    "    name sph\n"
    "    radius 1.0\n"
    "}\n";

static IJobPriv* LoadScene( const std::string& body, const char* tag )
{
    char path[512];
    std::snprintf( path, sizeof(path),
        "/tmp/override_object_test_%s_%d.RISEscene", tag, (int)::getpid() );
    std::ofstream ofs( path );
    if( !ofs.is_open() ) return nullptr;
    ofs << kPreamble << body;
    ofs.close();

    IJobPriv* pJob = nullptr;
    if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
        std::remove( path );
        return nullptr;
    }
    const bool ok = pJob->LoadAsciiScene( path );
    std::remove( path );
    if( !ok ) {
        safe_release( pJob );
        return nullptr;
    }
    return pJob;
}

// --------------------------------------------------------------------

static void TestPerFieldOverride()
{
    gCurrentTest = "TestPerFieldOverride";
    std::cout << gCurrentTest << "..." << std::endl;
    // standard_object at origin, override_object moves it to (5, 0, 0).
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name alpha\n"
        "    geometry sph\n"
        "    position 0 0 0\n"
        "}\n"
        "override_object\n{\n"
        "    name alpha\n"
        "    position 5 0 0\n"
        "}\n",
        "per_field"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;

    // Base snapshot (pre-override) should be at origin; loaded snapshot
    // (post-override) should reflect (5, 0, 0).
    const TransformSnapshot* base = pJob->GetBaseTransformSnapshot();
    const TransformSnapshot* loaded = pJob->GetLoadedTransformSnapshot();
    Check( base && base->Contains("alpha"), "alpha in base snapshot" );
    Check( loaded && loaded->Contains("alpha"), "alpha in loaded snapshot" );
    if( base && loaded ) {
        const Matrix4* mb = base->Find( "alpha" );
        const Matrix4* ml = loaded->Find( "alpha" );
        Check( mb && ml, "matrices retrieved" );
        if( mb && ml ) {
            // Translation column in our column-major mapping is _30/_31/_32.
            Check( CloseEnough(mb->_30, 0.0), "base translation x = 0" );
            Check( CloseEnough(ml->_30, 5.0), "loaded translation x = 5" );
        }
    }

    // OverrideSpanIndex records the chunk as UNMANAGED (no sentinels).
    const OverrideSpanIndex* ovIdx = pJob->GetOverrideSpanIndex();
    Check( ovIdx && ovIdx->Count() == 1, "one OverrideSpan recorded" );
    if( ovIdx && ovIdx->Count() == 1 ) {
        Check( ovIdx->HasUnmanagedFor("alpha"), "alpha has unmanaged override" );
        Check( ovIdx->FindManaged("alpha") == nullptr, "no managed record" );
        const OverrideRecord& r = ovIdx->Entries()[0];
        Check( r.targetName == "alpha", "record targets alpha" );
        Check( r.hasPosition, "record has position field" );
        Check( !r.hasOrientation, "no orientation field" );
        Check( !r.hasMatrix, "no matrix field" );
        Check( CloseEnough(r.position.x, 5.0) &&
               CloseEnough(r.position.y, 0.0) &&
               CloseEnough(r.position.z, 0.0),
               "recorded position == (5,0,0)" );
    }
    safe_release( pJob );
}

static void TestMatrixOverride()
{
    gCurrentTest = "TestMatrixOverride";
    std::cout << gCurrentTest << "..." << std::endl;
    // override_object with matrix field — column-major identity but
    // with translation (3, 5, 7).  Should replace the runtime transform
    // entirely.
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name beta\n"
        "    geometry sph\n"
        "    position 99 99 99\n"
        "}\n"
        "override_object\n{\n"
        "    name beta\n"
        "    matrix 1 0 0 0  0 1 0 0  0 0 1 0  3 5 7 1\n"
        "}\n",
        "matrix"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const Matrix4* ml = pJob->GetLoadedTransformSnapshot()->Find( "beta" );
    Check( ml != nullptr, "beta has loaded matrix" );
    if( ml ) {
        Check( CloseEnough(ml->_30, 3.0) &&
               CloseEnough(ml->_31, 5.0) &&
               CloseEnough(ml->_32, 7.0),
               "loaded translation == (3,5,7) from matrix override" );
    }
    const OverrideSpanIndex* ovIdx = pJob->GetOverrideSpanIndex();
    if( ovIdx && ovIdx->Count() == 1 ) {
        Check( ovIdx->Entries()[0].hasMatrix, "record has matrix field" );
    }
    safe_release( pJob );
}

static void TestManagedSentinelClassification()
{
    gCurrentTest = "TestManagedSentinelClassification";
    std::cout << gCurrentTest << "..." << std::endl;
    // Override INSIDE sentinel comments → managed.  Override OUTSIDE
    // sentinels → unmanaged.
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name a1\n"
        "    geometry sph\n"
        "    position 0 0 0\n"
        "}\n"
        "standard_object\n{\n"
        "    name a2\n"
        "    geometry sph\n"
        "    position 0 0 0\n"
        "}\n"
        "override_object\n{\n"
        "    name a1\n"
        "    position 1 0 0\n"
        "}\n"
        "# === RISE editor overrides (managed by interactive editor) ===\n"
        "override_object\n{\n"
        "    name a2\n"
        "    position 2 0 0\n"
        "}\n"
        "# === end RISE editor overrides ===\n",
        "managed_classify"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const OverrideSpanIndex* ovIdx = pJob->GetOverrideSpanIndex();
    Check( ovIdx && ovIdx->Count() == 2, "two override records" );
    if( ovIdx && ovIdx->Count() == 2 ) {
        Check( ovIdx->HasUnmanagedFor("a1") && !ovIdx->FindManaged("a1"),
               "a1 is unmanaged" );
        Check( !ovIdx->HasUnmanagedFor("a2") && ovIdx->FindManaged("a2") != nullptr,
               "a2 is managed" );
        const OverrideRecord* a2 = ovIdx->FindManaged("a2");
        if( a2 ) {
            Check( a2->managed, "a2 record's managed flag is true" );
            Check( CloseEnough(a2->position.x, 2.0), "a2 position recorded == 2" );
        }
    }
    safe_release( pJob );
}

static void TestMissingTargetIsHardError()
{
    gCurrentTest = "TestMissingTargetIsHardError";
    std::cout << gCurrentTest << "..." << std::endl;
    // override_object referencing an object that doesn't exist should
    // fail parse.  Pinned 8.5 / §8.2.
    IJobPriv* pJob = LoadScene(
        "override_object\n{\n"
        "    name no_such_object\n"
        "    position 1 2 3\n"
        "}\n",
        "missing_target"
    );
    Check( pJob == nullptr, "parse failed on missing target" );
    if( pJob ) safe_release( pJob );
}

static void TestEmptyOverrideIsSoftWarning()
{
    gCurrentTest = "TestEmptyOverrideIsSoftWarning";
    std::cout << gCurrentTest << "..." << std::endl;
    // override_object with no transform fields — soft warning, parse
    // succeeds, no transform change.  §8.4.
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name gamma\n"
        "    geometry sph\n"
        "    position 7 7 7\n"
        "}\n"
        "override_object\n{\n"
        "    name gamma\n"
        "}\n",
        "empty_override"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    // gamma's position should still be (7, 7, 7) — empty override didn't mutate.
    const Matrix4* ml = pJob->GetLoadedTransformSnapshot()->Find( "gamma" );
    if( ml ) {
        Check( CloseEnough(ml->_30, 7.0), "gamma translation unchanged" );
    }
    // The chunk is STILL recorded in OverrideSpanIndex (so the save
    // engine knows about it), just with no transform-field flags set.
    const OverrideSpanIndex* ovIdx = pJob->GetOverrideSpanIndex();
    Check( ovIdx && ovIdx->Count() == 1, "empty chunk still recorded" );
    if( ovIdx && ovIdx->Count() == 1 ) {
        const OverrideRecord& r = ovIdx->Entries()[0];
        Check( !r.hasPosition && !r.hasOrientation && !r.hasMatrix && !r.hasScale,
               "no fields recorded" );
    }
    safe_release( pJob );
}

static void TestPrecedenceMatrixOverQuaternion()
{
    gCurrentTest = "TestPrecedenceMatrixOverQuaternion";
    std::cout << gCurrentTest << "..." << std::endl;
    // Matrix beats quaternion when both are present in an override.
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name delta\n"
        "    geometry sph\n"
        "}\n"
        "override_object\n{\n"
        "    name delta\n"
        "    matrix 1 0 0 0  0 1 0 0  0 0 1 0  9 0 0 1\n"
        "    quaternion 0 0 0 1\n"
        "}\n",
        "precedence"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const Matrix4* ml = pJob->GetLoadedTransformSnapshot()->Find( "delta" );
    if( ml ) {
        Check( CloseEnough(ml->_30, 9.0), "matrix translation applied" );
    }
    safe_release( pJob );
}

static void TestBaseVsLoadedDiffer()
{
    gCurrentTest = "TestBaseVsLoadedDiffer";
    std::cout << gCurrentTest << "..." << std::endl;
    // BaseTransformSnapshot captures the standard_object's transform
    // BEFORE override_object runs.  LoadedTransformSnapshot captures
    // it AFTER.  The two should differ exactly where the override
    // applied.  §7.4.
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name eps\n"
        "    geometry sph\n"
        "    position 1 2 3\n"
        "}\n"
        "override_object\n{\n"
        "    name eps\n"
        "    position 10 20 30\n"
        "}\n",
        "base_vs_loaded"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const Matrix4* mb = pJob->GetBaseTransformSnapshot()->Find( "eps" );
    const Matrix4* ml = pJob->GetLoadedTransformSnapshot()->Find( "eps" );
    Check( mb && ml, "both snapshots populated" );
    if( mb && ml ) {
        Check( CloseEnough(mb->_30, 1.0), "base position x = 1" );
        Check( CloseEnough(mb->_31, 2.0), "base position y = 2" );
        Check( CloseEnough(mb->_32, 3.0), "base position z = 3" );
        Check( CloseEnough(ml->_30, 10.0), "loaded position x = 10" );
        Check( CloseEnough(ml->_31, 20.0), "loaded position y = 20" );
        Check( CloseEnough(ml->_32, 30.0), "loaded position z = 30" );
    }
    safe_release( pJob );
}

// --------------------------------------------------------------------

int main()
{
    std::cout << "===== OverrideObjectParserTest =====" << std::endl;
    TestPerFieldOverride();
    TestMatrixOverride();
    TestManagedSentinelClassification();
    TestMissingTargetIsHardError();
    TestEmptyOverrideIsSoftWarning();
    TestPrecedenceMatrixOverQuaternion();
    TestBaseVsLoadedDiffer();
    std::cout << "passed " << passCount << ", failed " << failCount << std::endl;
    return failCount == 0 ? 0 : 1;
}
