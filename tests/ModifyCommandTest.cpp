//////////////////////////////////////////////////////////////////////
//
//  ModifyCommandTest.cpp - Verifies the generalized `> modify` scene
//  command surface that lets a scene "mode" (day / night / dramatic) be
//  expressed as a self-contained command block instead of hand-editing
//  chunk bodies.
//
//  Covers, both at the IJob API level (direct calls) AND at the command
//  parser level (feeding `modify ...` lines through ParseCommand):
//
//    modify object   <name> material <materialName>
//    modify object   <name> shader   <shaderName>
//    modify material <name> scale    <value>          (luminaire emission)
//    modify rasterizer radiance_scale <value>         (active env scale)
//
//  Asserts:
//    - SetObjectMaterial / SetObjectShader rebind the object (verified
//      by comparing the object's GetMaterial()/GetShader() pointer
//      against the manager's item pointer).
//    - SetMaterialEmissionScale rescales a luminaire (verified via the
//      emitter's averageRadiantExitance, which is scale-proportional for
//      a uniform-colour exitance painter) and REJECTS a non-luminaire
//      material (returns false, leaves it unchanged).
//    - SetActiveRasterizerRadianceScale returns true and updates BOTH the
//      RayCaster's stored override AND the scene radiance map's scale
//      (direct-view / NEE consistency).
//    - Unknown object / material / shader names return false.
//    - The command parser routes each `modify ...` form to the matching
//      IJob method (success on valid targets, false on bad ones / bad
//      subtokens).
//
//  Standalone-executable style: no test framework, Check() tallies, main
//  returns nonzero on any failure.  Mirrors OverrideObjectParserTest.cpp.
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
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Interfaces/IRadianceMap.h"
#include "../src/Library/Interfaces/ISceneParser.h"   // ICommandParser
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;

namespace RISE
{
    bool RISE_CreateJobPriv( IJobPriv** ppi );
    bool RISE_API_CreateAsciiCommandParser( ICommandParser** ppi );
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

static bool CloseEnough( double a, double b, double tol = 1e-6 )
{
    return std::fabs( a - b ) <= tol;
}

// Average of the three RGB channels of a luminaire's average radiant
// exitance.  Scale-proportional for a uniform-colour exitance painter,
// so the RATIO after a SetEmissionScale equals the scale ratio exactly.
static double EmitterMeanExitance( const IMaterial* pMat )
{
    if( !pMat ) return -1.0;
    const IEmitter* e = pMat->GetEmitter();
    if( !e ) return -1.0;
    const RISEPel m = e->averageRadiantExitance();
    return ( m[0] + m[1] + m[2] ) / 3.0;
}

// Reach the concrete RayCaster of the active rasterizer the same way
// Job::SetActiveRasterizerRadianceScale does, so the test can read the
// stored radiance-scale override back.
static RISE::Implementation::RayCaster* ActiveCaster( IJobPriv* pJob )
{
    IRasterizer* pRaster = pJob->GetRasterizer();
    RISE::Implementation::PixelBasedRasterizerHelper* pHelper =
        dynamic_cast<RISE::Implementation::PixelBasedRasterizerHelper*>( pRaster );
    if( !pHelper ) return nullptr;
    return dynamic_cast<RISE::Implementation::RayCaster*>( pHelper->GetRayCaster() );
}

// A complete little scene: one uniform-colour painter, a base lambertian
// material, a lambertian_luminaire material (scale 100), two shaders, a
// sphere, an object bound to the base material + shaderA, and a PT
// rasterizer that installs a global radiance map at scale 2.0.
static const char* kScene =
    "RISE ASCII SCENE 6\n"
    "uniformcolor_painter\n{\n"
    "    name pnt_white\n"
    "    color 1.0 1.0 1.0\n"
    "}\n"
    "lambertian_material\n{\n"
    "    name lambMat\n"
    "    reflectance pnt_white\n"
    "}\n"
    "lambertian_luminaire_material\n{\n"
    "    name lumMat\n"
    "    material none\n"
    "    exitance pnt_white\n"
    "    scale 100.0\n"
    "}\n"
    // The PT rasterizer's default shader name is "global"; author it so
    // the rasterizer chunk finalizes.  shaderB is the swap target.
    "standard_shader\n{\n"
    "    name global\n"
    "    shaderop DefaultPathTracing\n"
    "}\n"
    "standard_shader\n{\n"
    "    name shaderB\n"
    "    shaderop DefaultEmission\n"
    "}\n"
    "sphere_geometry\n{\n"
    "    name sph\n"
    "    radius 1.0\n"
    "}\n"
    "standard_object\n{\n"
    "    name obj1\n"
    "    geometry sph\n"
    "    material lambMat\n"
    "    shader global\n"
    "}\n"
    "pathtracing_pel_rasterizer\n{\n"
    "    samples 4\n"
    "    radiance_map pnt_white\n"
    "    radiance_scale 2.0\n"
    "}\n";

static IJobPriv* LoadScene( const char* tag )
{
    char path[512];
    std::snprintf( path, sizeof(path),
        "/tmp/modify_command_test_%s_%d.RISEscene", tag, (int)::getpid() );
    std::ofstream ofs( path );
    if( !ofs.is_open() ) return nullptr;
    ofs << kScene;
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
// Direct IJob API exercises.
// --------------------------------------------------------------------

static void TestSetObjectMaterial()
{
    gCurrentTest = "TestSetObjectMaterial";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadScene( "set_obj_mat" );
    Check( pJob != nullptr, "scene parsed" );
    if( !pJob ) return;

    IObjectPriv* pObj = pJob->GetObjects()->GetItem( "obj1" );
    const IMaterial* lamb = pJob->GetMaterials()->GetItem( "lambMat" );
    const IMaterial* lum  = pJob->GetMaterials()->GetItem( "lumMat" );
    Check( pObj && lamb && lum, "object + materials resolved" );

    // Starts on lambMat (per the scene file).
    Check( pObj && pObj->GetMaterial() == lamb, "object starts on lambMat" );

    // Swap to the luminaire material.
    Check( pJob->SetObjectMaterial( "obj1", "lumMat" ) == true, "SetObjectMaterial returns true" );
    Check( pObj && pObj->GetMaterial() == lum, "object now on lumMat" );

    // Swap back.
    Check( pJob->SetObjectMaterial( "obj1", "lambMat" ) == true, "swap back returns true" );
    Check( pObj && pObj->GetMaterial() == lamb, "object back on lambMat" );

    // Unknown object / unknown material both fail and DON'T mutate.
    Check( pJob->SetObjectMaterial( "no_such_obj", "lambMat" ) == false, "unknown object fails" );
    Check( pJob->SetObjectMaterial( "obj1", "no_such_mat" ) == false, "unknown material fails" );
    Check( pObj && pObj->GetMaterial() == lamb, "object unchanged after failed swaps" );

    safe_release( pJob );
}

static void TestSetObjectShader()
{
    gCurrentTest = "TestSetObjectShader";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadScene( "set_obj_shader" );
    Check( pJob != nullptr, "scene parsed" );
    if( !pJob ) return;

    IObjectPriv* pObj = pJob->GetObjects()->GetItem( "obj1" );
    const IShader* shGlobal = pJob->GetShaders()->GetItem( "global" );
    const IShader* shB       = pJob->GetShaders()->GetItem( "shaderB" );
    Check( pObj && shGlobal && shB, "object + shaders resolved" );

    // Starts on the `global` shader (per the scene file).
    Check( pObj && pObj->GetShader() == shGlobal, "object starts on global shader" );

    Check( pJob->SetObjectShader( "obj1", "shaderB" ) == true, "SetObjectShader returns true" );
    Check( pObj && pObj->GetShader() == shB, "object now on shaderB" );

    Check( pJob->SetObjectShader( "no_such_obj", "global" ) == false, "unknown object fails" );
    Check( pJob->SetObjectShader( "obj1", "no_such_shader" ) == false, "unknown shader fails" );
    Check( pObj && pObj->GetShader() == shB, "object unchanged after failed swaps" );

    safe_release( pJob );
}

static void TestSetMaterialEmissionScale()
{
    gCurrentTest = "TestSetMaterialEmissionScale";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadScene( "set_mat_scale" );
    Check( pJob != nullptr, "scene parsed" );
    if( !pJob ) return;

    const IMaterial* lum  = pJob->GetMaterials()->GetItem( "lumMat" );
    const IMaterial* lamb = pJob->GetMaterials()->GetItem( "lambMat" );
    Check( lum && lamb, "materials resolved" );

    const double before = EmitterMeanExitance( lum );
    Check( before > 0.0, "luminaire has positive exitance at scale 100" );

    // Double the scale -> exitance doubles (uniform painter => exact ratio).
    Check( pJob->SetMaterialEmissionScale( "lumMat", 200.0 ) == true, "rescale luminaire returns true" );
    const double after = EmitterMeanExitance( lum );
    Check( after > 0.0 && CloseEnough( after / before, 2.0, 1e-4 ), "exitance doubled at scale 200" );

    // Halve it back below the original.
    Check( pJob->SetMaterialEmissionScale( "lumMat", 50.0 ) == true, "rescale to 50 returns true" );
    const double half = EmitterMeanExitance( lum );
    Check( CloseEnough( half / before, 0.5, 1e-4 ), "exitance halved at scale 50" );

    // Non-luminaire material rejects (default IMaterial::SetEmissionScale).
    Check( pJob->SetMaterialEmissionScale( "lambMat", 5.0 ) == false, "non-luminaire rejects scale" );

    // Unknown material fails.
    Check( pJob->SetMaterialEmissionScale( "no_such_mat", 5.0 ) == false, "unknown material fails" );

    safe_release( pJob );
}

static void TestSetActiveRasterizerRadianceScale()
{
    gCurrentTest = "TestSetActiveRasterizerRadianceScale";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadScene( "set_radiance_scale" );
    Check( pJob != nullptr, "scene parsed" );
    if( !pJob ) return;

    // The scene authored radiance_scale 2.0, so the map starts at 2.0.
    const IRadianceMap* pRm = pJob->GetScene()->GetGlobalRadianceMap();
    Check( pRm != nullptr, "global radiance map installed" );
    if( pRm ) Check( CloseEnough( pRm->GetScale(), 2.0 ), "map scale starts at 2.0" );

    // Before any override the caster reports a negative (no-override)
    // sentinel.
    RISE::Implementation::RayCaster* pCaster = ActiveCaster( pJob );
    Check( pCaster != nullptr, "reached active RayCaster" );
    if( pCaster ) Check( pCaster->GetRadianceScale() < 0.0, "no override before modify" );

    // Apply the override.
    Check( pJob->SetActiveRasterizerRadianceScale( 0.25 ) == true,
           "SetActiveRasterizerRadianceScale returns true" );

    // Both the caster override AND the scene map scale move to 0.25 so
    // NEE (env sampler, rebuilt from the override on next attach) and the
    // direct-view background (the map's own scale) stay consistent.
    if( pCaster ) Check( CloseEnough( pCaster->GetRadianceScale(), 0.25 ), "caster override == 0.25" );
    if( pRm )     Check( CloseEnough( pRm->GetScale(), 0.25 ), "map scale updated to 0.25" );

    // Negative radiance scale is nonphysical -> rejected at the boundary,
    // leaving BOTH the caster override and the map scale unchanged (and
    // never colliding with the -1 "no override" sentinel).
    Check( pJob->SetActiveRasterizerRadianceScale( -1.0 ) == false, "negative radiance scale rejected" );
    if( pCaster ) Check( CloseEnough( pCaster->GetRadianceScale(), 0.25 ), "caster override unchanged after rejected negative" );
    if( pRm )     Check( CloseEnough( pRm->GetScale(), 0.25 ), "map scale unchanged after rejected negative" );

    safe_release( pJob );
}

// --------------------------------------------------------------------
// Command-parser smoke: feed `modify ...` lines through ParseCommand and
// confirm they route to the right IJob method.
// --------------------------------------------------------------------

static void TestCommandParserRouting()
{
    gCurrentTest = "TestCommandParserRouting";
    std::cout << gCurrentTest << "..." << std::endl;
    IJobPriv* pJob = LoadScene( "cmd_parser" );
    Check( pJob != nullptr, "scene parsed" );
    if( !pJob ) return;

    ICommandParser* parser = nullptr;
    RISE_API_CreateAsciiCommandParser( &parser );
    Check( parser != nullptr, "command parser created" );
    if( !parser ) { safe_release( pJob ); return; }

    IObjectPriv* pObj = pJob->GetObjects()->GetItem( "obj1" );
    const IMaterial* lum  = pJob->GetMaterials()->GetItem( "lumMat" );
    const IShader*   shB  = pJob->GetShaders()->GetItem( "shaderB" );

    // modify object <name> material <materialName>  (name-first grammar)
    Check( parser->ParseCommand( "modify object obj1 material lumMat", *pJob ) == true,
           "parse: modify object material" );
    Check( pObj && pObj->GetMaterial() == lum, "command swapped material to lumMat" );

    // modify object <name> shader <shaderName>
    Check( parser->ParseCommand( "modify object obj1 shader shaderB", *pJob ) == true,
           "parse: modify object shader" );
    Check( pObj && pObj->GetShader() == shB, "command swapped shader to shaderB" );

    // modify material <name> scale <value>
    const double before = EmitterMeanExitance( lum );
    Check( parser->ParseCommand( "modify material lumMat scale 300.0", *pJob ) == true,
           "parse: modify material scale" );
    const double after = EmitterMeanExitance( lum );
    Check( before > 0.0 && CloseEnough( after / before, 3.0, 1e-4 ), "command tripled exitance (100->300)" );

    // modify rasterizer radiance_scale <value>
    Check( parser->ParseCommand( "modify rasterizer radiance_scale 0.5", *pJob ) == true,
           "parse: modify rasterizer radiance_scale" );
    const IRadianceMap* pRm = pJob->GetScene()->GetGlobalRadianceMap();
    if( pRm ) Check( CloseEnough( pRm->GetScale(), 0.5 ), "command set map scale to 0.5" );

    // --- Negative cases: bad targets / bad subtokens all return false. ---
    Check( parser->ParseCommand( "modify object no_such_obj material lumMat", *pJob ) == false,
           "parse: bad object target fails" );
    Check( parser->ParseCommand( "modify object obj1 material no_such_mat", *pJob ) == false,
           "parse: bad material target fails" );
    Check( parser->ParseCommand( "modify material lambMat scale 5.0", *pJob ) == false,
           "parse: non-luminaire scale fails" );
    Check( parser->ParseCommand( "modify material lumMat bogus 5.0", *pJob ) == false,
           "parse: unknown material subtoken fails" );
    Check( parser->ParseCommand( "modify rasterizer bogus 1.0", *pJob ) == false,
           "parse: unknown rasterizer subtoken fails" );
    Check( parser->ParseCommand( "modify bogus foo bar", *pJob ) == false,
           "parse: unknown modify element fails" );

    safe_release( parser );
    safe_release( pJob );
}

// --------------------------------------------------------------------

int main()
{
    std::cout << "===== ModifyCommandTest =====" << std::endl;
    TestSetObjectMaterial();
    TestSetObjectShader();
    TestSetMaterialEmissionScale();
    TestSetActiveRasterizerRadianceScale();
    TestCommandParserRouting();
    std::cout << "passed " << passCount << ", failed " << failCount << std::endl;
    return failCount == 0 ? 0 : 1;
}
