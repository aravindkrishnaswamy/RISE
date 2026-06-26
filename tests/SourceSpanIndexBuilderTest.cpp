//////////////////////////////////////////////////////////////////////
//
//  SourceSpanIndexBuilderTest.cpp - exercises Phase 6.1 of the
//    round-trip-save pipeline.  Loads hand-crafted scene files and
//    asserts that:
//      - SourceSpanIndex entries exist for every standard_object.
//      - authorMode reflects the source's transform precedence.
//      - parameterSpans cover each transform line + value bytes.
//      - chunkRevisited flips TRUE on FOR-body re-entry; FOR 2..N
//        runtime entities have NO SourceSpan but DO have a
//        CreationLocation.
//      - BaseTransformSnapshot + LoadedTransformSnapshot are
//        populated and have the same name set as SourceSpanIndex
//        (V1: no override_object chunks here; Phase 6.2 adds those).
//
//  Spec: docs/ROUND_TRIP_SAVE_PLAN.md §6.4 + §7.4.
//
//  Scene-language note: RISE parser requires `{` on its own line
//  (NOT on the chunk-name line), uses `!VAR` for integer FOR-body
//  expansion (4-digit zero-padded), `@VAR` for floating-point
//  substitution, and `$(...)` for math expressions.  Names are
//  passed verbatim to the IObjectManager — quotes are part of the
//  key.  Tests use bare identifiers (`name alpha`) to keep the
//  manager keys readable.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <iterator>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/SceneEditor/SourceSpanIndex.h"
#include "../src/Library/SceneEditor/DirtyTracker.h"   // EntityCategory
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

// Write a scene file with a minimal geometry chunk before the body,
// load it, and return the IJobPriv (caller releases).  Returns null
// on parse failure.
static IJobPriv* LoadScene( const std::string& body, const char* tag )
{
    char path[512];
    std::snprintf( path, sizeof(path),
        "/tmp/source_span_test_%s_%d.RISEscene", tag, (int)::getpid() );
    std::ofstream ofs( path );
    if( !ofs.is_open() ) return nullptr;
    ofs << "RISE ASCII SCENE 6\n";
    // A minimal geometry that every standard_object can reference.
    // RISE parser requires `{` on its own line.
    ofs << "sphere_geometry\n{\n";
    ofs << "    name sph\n";
    ofs << "    radius 1.0\n";
    ofs << "}\n";
    ofs << body;
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

static void TestSingleEulerObject()
{
    gCurrentTest = "TestSingleEulerObject";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name alpha\n"
        "    geometry sph\n"
        "    position 1.5 2.5 3.5\n"
        "    orientation 0 90 0\n"
        "    scale 2 2 2\n"
        "}\n",
        "single_euler"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;

    const SourceSpanIndex* idx = pJob->GetSourceSpanIndex();
    Check( idx != nullptr && idx->Count() == 1, "exactly one SourceSpan entry" );
    const SourceSpan* sp = idx->Find( "alpha" );
    Check( sp != nullptr, "alpha has SourceSpan" );
    if( sp ) {
        Check( sp->authorMode == AuthorMode::Euler, "authorMode == Euler" );
        Check( !sp->chunkRevisited, "chunkRevisited == false" );
        Check( sp->parameterSpans.count("position") == 1, "position span recorded" );
        Check( sp->parameterSpans.count("orientation") == 1, "orientation span recorded" );
        Check( sp->parameterSpans.count("scale") == 1, "scale span recorded" );
        Check( sp->parameterSpans.count("matrix") == 0, "no matrix span" );
        // Sanity: byte offsets monotonically grow.
        Check( sp->chunkBeginOffset < sp->bodyOpenBraceOffset, "begin < open-brace" );
        Check( sp->bodyOpenBraceOffset < sp->bodyCloseBraceOffset, "open-brace < close-brace" );
        Check( sp->bodyCloseBraceOffset < sp->chunkEndOffset, "close-brace < end" );
        // Per-parameter spans should have isSymbolic == false (no `$` macros).
        auto pit = sp->parameterSpans.find("position");
        if( pit != sp->parameterSpans.end() ) {
            Check( !pit->second.isSymbolic, "position isSymbolic == false" );
            Check( pit->second.valueBegin < pit->second.valueEnd, "position has non-empty value range" );
        }
    }

    const TransformSnapshot* base   = pJob->GetBaseTransformSnapshot();
    const TransformSnapshot* loaded = pJob->GetLoadedTransformSnapshot();
    Check( base != nullptr && base->Count() == 1, "BaseTransformSnapshot has one entry" );
    Check( loaded != nullptr && loaded->Count() == 1, "LoadedTransformSnapshot has one entry" );
    Check( base && base->Contains("alpha"), "base has alpha" );
    Check( loaded && loaded->Contains("alpha"), "loaded has alpha" );

    Check( idx->GetCreationOffsetEnd("alpha") != SourceSpanIndex::kNoCreationOffset,
           "creation offset recorded" );
    Check( !idx->GetCreationFilePath("alpha").empty(),
           "creation file path recorded" );

    safe_release( pJob );
}

static void TestMatrixAuthorMode()
{
    gCurrentTest = "TestMatrixAuthorMode";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name mtx\n"
        "    geometry sph\n"
        "    matrix 1 0 0 0  0 1 0 0  0 0 1 0  3 5 7 1\n"
        "}\n",
        "matrix_author"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;

    const SourceSpan* sp = pJob->GetSourceSpanIndex()->Find( "mtx" );
    Check( sp != nullptr, "mtx has SourceSpan" );
    if( sp ) {
        Check( sp->authorMode == AuthorMode::Matrix, "authorMode == Matrix" );
        Check( sp->parameterSpans.count("matrix") == 1, "matrix span recorded" );
    }
    safe_release( pJob );
}

static void TestQuaternionAuthorMode()
{
    gCurrentTest = "TestQuaternionAuthorMode";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name qrot\n"
        "    geometry sph\n"
        "    position 1 2 3\n"
        "    quaternion 0 0 0 1\n"
        "    scale 1 1 1\n"
        "}\n",
        "quat_author"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;

    const SourceSpan* sp = pJob->GetSourceSpanIndex()->Find( "qrot" );
    Check( sp != nullptr, "qrot has SourceSpan" );
    if( sp ) {
        Check( sp->authorMode == AuthorMode::Quaternion, "authorMode == Quaternion" );
        Check( sp->parameterSpans.count("quaternion") == 1, "quaternion span recorded" );
    }
    safe_release( pJob );
}

static void TestForLoopRevisit()
{
    gCurrentTest = "TestForLoopRevisit";
    std::cout << gCurrentTest << "..." << std::endl;
    // FOR loop generating 3 objects.  RISE expands `!I` as a 4-digit
    // zero-padded integer macro, so names are sphere_0000, sphere_0001,
    // sphere_0002.  Each iteration re-reads the SAME chunk body:
    //   - SourceSpan for sphere_0000 (first visit), chunkRevisited == true
    //   - NO SourceSpan for sphere_0001 or sphere_0002
    //   - All three have CreationLocation entries
    //   - All three have BaseTransformSnapshot entries
    IJobPriv* pJob = LoadScene(
        "FOR I 0 2 1\n"
        "standard_object\n{\n"
        "    name sphere_!I\n"
        "    geometry sph\n"
        "    position !I 0 0\n"
        "}\n"
        "ENDFOR\n",
        "for_revisit"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;

    const SourceSpanIndex* idx = pJob->GetSourceSpanIndex();
    Check( idx != nullptr, "SourceSpanIndex available" );
    if( idx ) {
        Check( idx->Find("sphere_0000") != nullptr, "sphere_0000 has SourceSpan" );
        Check( idx->Find("sphere_0001") == nullptr, "sphere_0001 has NO SourceSpan (FOR-revisit)" );
        Check( idx->Find("sphere_0002") == nullptr, "sphere_0002 has NO SourceSpan (FOR-revisit)" );
        const SourceSpan* sp0 = idx->Find( "sphere_0000" );
        if( sp0 ) {
            Check( sp0->chunkRevisited, "sphere_0000 chunkRevisited == true after FOR completes" );
        }
        // CreationLocation populated for every iteration.
        Check( idx->HasCreationLocation("sphere_0000"), "sphere_0000 has CreationLocation" );
        Check( idx->HasCreationLocation("sphere_0001"), "sphere_0001 has CreationLocation" );
        Check( idx->HasCreationLocation("sphere_0002"), "sphere_0002 has CreationLocation" );
        // All three share the same chunk-end offset (same source chunk).
        Check(
            idx->GetCreationOffsetEnd("sphere_0000") == idx->GetCreationOffsetEnd("sphere_0001") &&
            idx->GetCreationOffsetEnd("sphere_0001") == idx->GetCreationOffsetEnd("sphere_0002"),
            "all three FOR siblings share chunkEnd offset" );
    }

    // Base / loaded snapshots have all three entries.
    const TransformSnapshot* base = pJob->GetBaseTransformSnapshot();
    Check( base && base->Count() == 3, "BaseTransformSnapshot has all 3 FOR iterations" );
    safe_release( pJob );
}

static void TestSymbolicParameter()
{
    gCurrentTest = "TestSymbolicParameter";
    std::cout << gCurrentTest << "..." << std::endl;
    // A flat (non-FOR) chunk with a single position-component
    // expressed as a `$(...)` math expression.  Pinned 2.5: only THAT
    // parameter line is marked isSymbolic; siblings stay direct.
    // RISE substitutes the @CY macro first, then evaluates the
    // expression.  The RAW token (captured by Phase 0 before
    // substitution) contains `$`, so isSymbolic flips true.
    IJobPriv* pJob = LoadScene(
        "DEFINE CY 5\n"
        "standard_object\n{\n"
        "    name sym\n"
        "    geometry sph\n"
        "    position 0 $(@CY) 0\n"
        "    orientation 0 0 0\n"
        "}\n",
        "symbolic"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;

    const SourceSpan* sp = pJob->GetSourceSpanIndex()->Find( "sym" );
    Check( sp != nullptr, "sym has SourceSpan" );
    if( sp ) {
        auto pit = sp->parameterSpans.find("position");
        auto oit = sp->parameterSpans.find("orientation");
        Check( pit != sp->parameterSpans.end() && pit->second.isSymbolic,
               "position is symbolic" );
        Check( oit != sp->parameterSpans.end() && !oit->second.isSymbolic,
               "orientation is NOT symbolic (per-parameter classification)" );
        Check( !sp->chunkRevisited, "flat chunk is not revisited" );
    }
    safe_release( pJob );
}

// Coverage added in response to Phase 6.1 adversarial review.

static void TestMatrixAndQuaternionPrecedence()
{
    gCurrentTest = "TestMatrixAndQuaternionPrecedence";
    std::cout << gCurrentTest << "..." << std::endl;
    // Matrix > Quaternion > Euler precedence (§6.3 / AsciiSceneParser
    // line ~5500).  authorMode should report MATRIX even when both
    // matrix and quaternion are present in the chunk.
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name mq\n"
        "    geometry sph\n"
        "    matrix 1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1\n"
        "    quaternion 0 0 0 1\n"
        "}\n",
        "matrix_quat_precedence"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const SourceSpan* sp = pJob->GetSourceSpanIndex()->Find( "mq" );
    Check( sp != nullptr, "mq has SourceSpan" );
    if( sp ) {
        Check( sp->authorMode == AuthorMode::Matrix, "authorMode == Matrix (beats quaternion)" );
        Check( sp->parameterSpans.count("matrix") == 1, "matrix span recorded" );
        Check( sp->parameterSpans.count("quaternion") == 1, "quaternion span also recorded" );
    }
    safe_release( pJob );
}

static void TestNameAfterTransformParams()
{
    gCurrentTest = "TestNameAfterTransformParams";
    std::cout << gCurrentTest << "..." << std::endl;
    // `name` parameter declared AFTER `position`/`orientation`.
    // ExtractObjectName scans all chunkparams for the LAST `name`
    // entry so ordering doesn't matter — should still resolve to
    // "tail" and produce a SourceSpan + BaseTransform entry.
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    geometry sph\n"
        "    position 4 5 6\n"
        "    orientation 0 0 0\n"
        "    name tail\n"
        "}\n",
        "name_after"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    Check( pJob->GetSourceSpanIndex()->Find("tail") != nullptr, "tail has SourceSpan" );
    Check( pJob->GetBaseTransformSnapshot()->Contains("tail"), "tail in BaseTransformSnapshot" );
    safe_release( pJob );
}

static void TestNoTransformParameters()
{
    gCurrentTest = "TestNoTransformParameters";
    std::cout << gCurrentTest << "..." << std::endl;
    // standard_object with only name + geometry (no transform fields).
    // SourceSpan should exist but parameterSpans empty.  authorMode
    // defaults to Euler (no matrix/quaternion present).
    IJobPriv* pJob = LoadScene(
        "standard_object\n{\n"
        "    name bare\n"
        "    geometry sph\n"
        "}\n",
        "no_transforms"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const SourceSpan* sp = pJob->GetSourceSpanIndex()->Find( "bare" );
    Check( sp != nullptr, "bare has SourceSpan" );
    if( sp ) {
        Check( sp->authorMode == AuthorMode::Euler, "authorMode == Euler default" );
        Check( sp->parameterSpans.count("position") == 0, "no position span" );
        Check( sp->parameterSpans.count("orientation") == 0, "no orientation span" );
        Check( sp->parameterSpans.count("scale") == 0, "no scale span" );
        Check( sp->parameterSpans.count("matrix") == 0, "no matrix span" );
    }
    // Base transform should still be captured (identity-derived).
    Check( pJob->GetBaseTransformSnapshot()->Contains("bare"),
           "bare in BaseTransformSnapshot even with no transforms" );
    safe_release( pJob );
}

static void TestRepeatedParseClears()
{
    gCurrentTest = "TestRepeatedParseClears";
    std::cout << gCurrentTest << "..." << std::endl;
    // Two sequential LoadAsciiScene calls on the same Job: the
    // SourceSpanIndex from the first parse must NOT bleed into the
    // second.
    IJobPriv* pJob1 = LoadScene(
        "standard_object\n{\n"
        "    name alpha\n"
        "    geometry sph\n"
        "    position 1 0 0\n"
        "}\n",
        "repeat1"
    );
    Check( pJob1 != nullptr, "first parse succeeded" );
    if( !pJob1 ) return;
    Check( pJob1->GetSourceSpanIndex()->Find("alpha") != nullptr, "alpha after first parse" );

    // Write a second scene file with a fresh sphere geometry name +
    // a different standard_object.  Reload via pJob1->LoadAsciiScene.
    // The parser should clear the prior SourceSpanIndex before
    // populating.  Note: GenericManager hard-errors on duplicate
    // names (no automatic clear-and-reload), so we use a fresh
    // geometry name to avoid that overlap and exercise just the
    // Phase 6.1 clear path.
    char path[512];
    std::snprintf( path, sizeof(path),
        "/tmp/source_span_test_repeat2_%d.RISEscene", (int)::getpid() );
    std::ofstream ofs( path );
    ofs << "RISE ASCII SCENE 6\n"
        << "sphere_geometry\n{\n    name sph2\n    radius 1.0\n}\n"
        << "standard_object\n{\n"
        << "    name beta\n"
        << "    geometry sph2\n"
        << "    position 0 1 0\n"
        << "}\n";
    ofs.close();
    const bool ok2 = pJob1->LoadAsciiScene( path );
    std::remove( path );
    Check( ok2, "second parse succeeded" );
    if( ok2 ) {
        Check( pJob1->GetSourceSpanIndex()->Find("alpha") == nullptr,
               "alpha cleared after second parse" );
        Check( pJob1->GetSourceSpanIndex()->Find("beta") != nullptr,
               "beta present after second parse" );
    }
    safe_release( pJob1 );
}

// --------------------------------------------------------------------

static void TestTabSeparatedEntityName()
{
    gCurrentTest = "TestTabSeparatedEntityName";
    std::cout << gCurrentTest << "..." << std::endl;
    // Corpus media/entities use TAB-separated names (e.g. `name\t\tglobal_haze`).  ExtractObjectName and
    // the new HasExplicitName guard must AGREE on these or a real tab-named entity gets mis-indexed as
    // "noname" (old) or wrongly refused (new).  This pins the name-detection's whitespace handling.
    IJobPriv* pJob = LoadScene(
        "homogeneous_medium\n{\n"
        "\tname\ttabfog\n"
        "\tabsorption 0.002 0.0015 0.004\n"
        "\tscattering 0.015 0.013 0.009\n"
        "}\n",
        "tab_name"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const SourceSpanIndex* idx = pJob->GetSourceSpanIndex();
    Check( idx && idx->FindEntity(EntityCategory::Medium, "tabfog") != nullptr,
           "tab-separated medium name IS indexed (not refused)" );
    Check( idx && idx->FindEntity(EntityCategory::Medium, "noname") == nullptr,
           "tab-named medium NOT mis-indexed as 'noname'" );
    safe_release( pJob );
}

static void TestNamelessLightSettingNotIndexed()
{
    gCurrentTest = "TestNamelessLightSettingNotIndexed";
    std::cout << gCurrentTest << "..." << std::endl;
    // The nameless-refuse is GENERIC across entity categories, not Medium-specific: a nameless Light
    // SETTINGS chunk (hosek_wilkie_skylight has no `name`) must not be indexed as (Light,"noname").
    IJobPriv* pJob = LoadScene(
        "hosek_wilkie_skylight\n{\n"
        "    solar_elevation 60.0\n"
        "    solar_azimuth 135.0\n"
        "    turbidity 3.0\n"
        "    sky_intensity_scale 1.0\n"
        "    sun_intensity_scale 3.14\n"
        "    create_sun FALSE\n"
        "}\n",
        "nameless_light"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const SourceSpanIndex* idx = pJob->GetSourceSpanIndex();
    Check( idx && idx->FindEntity(EntityCategory::Light, "noname") == nullptr,
           "nameless hosek_wilkie_skylight NOT indexed as (Light,'noname')" );
    safe_release( pJob );
}

static void TestNamelessMediumStillIndexed()
{
    gCurrentTest = "TestNamelessMediumStillIndexed";
    std::cout << gCurrentTest << "..." << std::endl;
    // A PRODUCER chunk whose DESCRIPTOR declares `name` creates a real entity even when the source OMITS
    // the name (default "noname").  Such an entity MUST be indexed for round-trip save -- the refuse is
    // descriptor-based (settings chunks with no `name` param), NOT source-text-based.
    IJobPriv* pJob = LoadScene(
        "homogeneous_medium\n{\n"
        "    absorption 0.002 0.0015 0.004\n"
        "    scattering 0.015 0.013 0.009\n"
        "}\n",
        "nameless_medium"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const SourceSpanIndex* idx = pJob->GetSourceSpanIndex();
    Check( idx && idx->FindEntity(EntityCategory::Medium, "noname") != nullptr,
           "name-omitted homogeneous_medium IS indexed as (Medium,'noname')" );
    safe_release( pJob );
}

static void TestNamelessMaterialStillIndexed()
{
    gCurrentTest = "TestNamelessMaterialStillIndexed";
    std::cout << gCurrentTest << "..." << std::endl;
    // Cross-category: a name-omitted lambertian_material (descriptor has `name`) creates material "noname".
    IJobPriv* pJob = LoadScene(
        "lambertian_material\n{\n"
        "}\n",
        "nameless_material"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const SourceSpanIndex* idx = pJob->GetSourceSpanIndex();
    Check( idx && idx->FindEntity(EntityCategory::Material, "noname") != nullptr,
           "name-omitted lambertian_material IS indexed as (Material,'noname')" );
    safe_release( pJob );
}

static void TestGlobalMediumNotIndexedAsEntity()
{
    gCurrentTest = "TestGlobalMediumNotIndexedAsEntity";
    std::cout << gCurrentTest << "..." << std::endl;
    // global_medium{} has NO `name` -- it is a SETTING categorized ChunkCategory::Medium, not a named
    // entity, so it must NOT be indexed as a savable entity (ExtractObjectName would default it to
    // "noname").  Here a real medium is named "realfog"; the index must hold "realfog" and NO "noname".
    IJobPriv* pJob = LoadScene(
        "homogeneous_medium\n{\n"
        "    name realfog\n"
        "    absorption 0.002 0.0015 0.004\n"
        "    scattering 0.015 0.013 0.009\n"
        "}\n"
        "global_medium\n{\n"
        "    medium realfog\n"
        "}\n",
        "global_medium_entity"
    );
    Check( pJob != nullptr, "parse succeeded" );
    if( !pJob ) return;
    const SourceSpanIndex* idx = pJob->GetSourceSpanIndex();
    Check( idx != nullptr, "SourceSpanIndex available" );
    if( idx ) {
        Check( idx->FindEntity(EntityCategory::Medium, "realfog") != nullptr, "real medium 'realfog' IS indexed" );
        Check( idx->FindEntity(EntityCategory::Medium, "noname")  == nullptr, "nameless global_medium{} NOT indexed as (Medium,'noname')" );
        Check( idx->EntityCount() == 1, "exactly one entity span (the real medium; global_medium refused)" );
    }
    safe_release( pJob );
}

static void TestGlobalMediumNonameCollision()
{
    gCurrentTest = "TestGlobalMediumNonameCollision";
    std::cout << gCurrentTest << "..." << std::endl;
    // The exact collision: a REAL medium named "noname" + a nameless global_medium{} referencing it.
    // Pre-fix the global_medium chunk indexed as entity (Medium,"noname") and could OVERWRITE the real
    // medium's source span, so a later medium edit would splice into the wrong chunk.  Post-fix the
    // "noname" span must cover the homogeneous_medium chunk, not the global_medium chunk.  Keep the file
    // so we can read the recorded span bytes.
    char path[512];
    std::snprintf( path, sizeof(path), "/tmp/source_span_gm_collision_%d.RISEscene", (int)::getpid() );
    {
        std::ofstream ofs( path );
        ofs << "RISE ASCII SCENE 6\n"
            << "sphere_geometry\n{\n    name sph\n    radius 1.0\n}\n"
            << "homogeneous_medium\n{\n    name noname\n    absorption 0.002 0.0015 0.004\n    scattering 0.015 0.013 0.009\n}\n"
            << "global_medium\n{\n    medium noname\n}\n";
    }
    IJobPriv* pJob = nullptr;
    if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check(false,"job create"); std::remove(path); return; }
    const bool ok = pJob->LoadAsciiScene( path );
    Check( ok, "parse succeeded" );
    if( ok ) {
        const SourceSpan* sp = pJob->GetSourceSpanIndex()->FindEntity( EntityCategory::Medium, "noname" );
        Check( sp != nullptr, "(Medium,'noname') has a SourceSpan" );
        if( sp ) {
            std::ifstream ifs( path, std::ios::binary );
            std::string all( (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>() );
            const std::string span =
                ( sp->chunkBeginOffset < sp->chunkEndOffset && sp->chunkEndOffset <= all.size() )
                ? all.substr( sp->chunkBeginOffset, sp->chunkEndOffset - sp->chunkBeginOffset )
                : std::string();
            Check( span.find("homogeneous_medium") != std::string::npos,
                   "'noname' span covers the homogeneous_medium chunk" );
            Check( span.find("global_medium") == std::string::npos,
                   "'noname' span does NOT cover the global_medium chunk" );
        }
    }
    safe_release( pJob );
    std::remove( path );
}

int main()
{
    std::cout << "===== SourceSpanIndexBuilderTest =====" << std::endl;
    TestSingleEulerObject();
    TestMatrixAuthorMode();
    TestQuaternionAuthorMode();
    TestForLoopRevisit();
    TestSymbolicParameter();
    TestMatrixAndQuaternionPrecedence();
    TestNameAfterTransformParams();
    TestNoTransformParameters();
    TestRepeatedParseClears();
    TestTabSeparatedEntityName();
    TestNamelessLightSettingNotIndexed();
    TestNamelessMediumStillIndexed();
    TestNamelessMaterialStillIndexed();
    TestGlobalMediumNotIndexedAsEntity();
    TestGlobalMediumNonameCollision();
    std::cout << "passed " << passCount << ", failed " << failCount << std::endl;
    return failCount == 0 ? 0 : 1;
}
