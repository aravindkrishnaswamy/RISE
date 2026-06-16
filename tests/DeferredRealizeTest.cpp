//////////////////////////////////////////////////////////////////////
//
//  DeferredRealizeTest.cpp - End-to-end proof of the Phase-1
//  deferred-realization feature via the Job API.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-06-13
//  Tabs: 4
//  Comments:
//
//    Builds a scene with FOUR cases and renders it through the real
//    pipeline (Job::Rasterize -> RayCaster::AttachScene realize pass):
//
//      1. a PLAIN sphere object (always-realized; no displaced bake);
//      2. a displaced_geometry BOUND to an object  -> bakes;
//      3. a displaced_geometry NOT bound to any object -> never baked;
//      4. a displaced-of-displaced bound to an object -> bakes BOTH the
//         outer and (cascaded) its inner base.
//
//    DisplacedGeometry::GetBuildMeshCount() is the instrumentation: it
//    counts every actual tessellate+bake.  We assert:
//      - after PARSE (LoadAsciiScene) the count is 0 — nothing baked at
//        parse time (the whole point of deferral);
//      - after RENDER the count is exactly 3 — the bound single (1) plus
//        the nested outer + its cascaded inner (2); the UNBOUND one
//        contributed 0;
//      - the rendered image has non-black pixels (the displaced objects
//        are actually present — a ray hit after realize).
//
//    This proves deferral (parse bakes nothing), cascade (a displaced
//    base is realized before its dependent), and skip-unused (an unbound
//    displaced geometry is never baked).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_failures = 0;
static std::vector<std::string> g_tmpScenes;  // removed at end of main

static void Check( bool cond, const char* what )
{
	if( cond ) {
		std::cout << "  [ok] " << what << "\n";
	} else {
		std::cout << "  [FAIL] " << what << "\n";
		++g_failures;
	}
}

//-----------------------------------------------------------------------------
// Captures rendered pixels so we can confirm the displaced objects rendered
// (non-black) after the realize pass.
//-----------------------------------------------------------------------------
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	unsigned int width;
	unsigned int height;
	double       maxLum;

	CapturingRasterizerOutput() : width( 0 ), height( 0 ), maxLum( 0.0 ) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage(
		const IRasterImage& pImage,
		const Rect*,
		const unsigned int ) override
	{
		width  = pImage.GetWidth();
		height = pImage.GetHeight();
		maxLum = 0.0;
		for( unsigned int y = 0; y < height; ++y ) {
			for( unsigned int x = 0; x < width; ++x ) {
				const RISEColor c = pImage.GetPEL( x, y );
				const double lum = c.base.r + c.base.g + c.base.b;
				if( lum > maxLum ) maxLum = lum;
			}
		}
	}
};

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/deferred_realize_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	g_tmpScenes.push_back( path );
	return std::string( path );
}

// FOUR-case scene.  base_sphere is the shared tessellatable base.  Only
// obj_plain / obj_single / obj_nested are bound to objects; disp_unbound and
// (used only as a base) disp_inner are NOT bound directly.
static const char* kSceneText =
	"RISE ASCII SCENE 6\n"
	"\n"
	"film\n"
	"{\n"
	"\twidth 48\n"
	"\theight 48\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 -8\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 60.0\n"
	"}\n"
	"\n"
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 1\n"
	"}\n"
	"\n"
	"directional_light\n"
	"{\n"
	"\tname sun\n"
	"\tpower 3.14159\n"
	"\tcolor 1 1 1\n"
	"\tdirection 0 0 -1\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname albedo\n"
	"\tcolor 0.8 0.8 0.8\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname matte\n"
	"\treflectance albedo\n"
	"}\n"
	"\n"
	// Shared tessellatable base.
	"sphere_geometry\n"
	"{\n"
	"\tname base_sphere\n"
	"\tradius 1.0\n"
	"}\n"
	"\n"
	// A separate plain sphere for the plain object (case 1).
	"sphere_geometry\n"
	"{\n"
	"\tname plain_sphere\n"
	"\tradius 1.0\n"
	"}\n"
	"\n"
	// A nonzero displacement so the bake does real per-vertex work.
	"expression_function2d\n"
	"{\n"
	"\tname bump\n"
	"\texpr 0.1*sin(6.28318*u)*sin(6.28318*v)\n"
	"}\n"
	"\n"
	// Case 2: bound single displaced.
	"displaced_geometry\n"
	"{\n"
	"\tname disp_single\n"
	"\tbase_geometry base_sphere\n"
	"\tdetail 24\n"
	"\tdisplacement bump\n"
	"\tdisp_scale 0.1\n"
	"}\n"
	"\n"
	// Case 3: UNBOUND displaced (defined but never put on an object).
	"displaced_geometry\n"
	"{\n"
	"\tname disp_unbound\n"
	"\tbase_geometry base_sphere\n"
	"\tdetail 24\n"
	"\tdisplacement bump\n"
	"\tdisp_scale 0.1\n"
	"}\n"
	"\n"
	// Case 4: displaced-of-displaced.  disp_inner is used ONLY as the base
	// of disp_outer (not bound to an object directly); disp_outer is bound.
	"displaced_geometry\n"
	"{\n"
	"\tname disp_inner\n"
	"\tbase_geometry base_sphere\n"
	"\tdetail 24\n"
	"\tdisplacement bump\n"
	"\tdisp_scale 0.1\n"
	"}\n"
	"\n"
	"displaced_geometry\n"
	"{\n"
	"\tname disp_outer\n"
	"\tbase_geometry disp_inner\n"
	"\tdetail 0\n"
	"\tdisplacement bump\n"
	"\tdisp_scale 0.05\n"
	"}\n"
	"\n"
	// Objects: only these three bind geometry.
	"standard_object\n"
	"{\n"
	"\tname obj_plain\n"
	"\tgeometry plain_sphere\n"
	"\tposition -2.5 0 0\n"
	"\tmaterial matte\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_single\n"
	"\tgeometry disp_single\n"
	"\tposition 0 0 0\n"
	"\tmaterial matte\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_nested\n"
	"\tgeometry disp_outer\n"
	"\tposition 2.5 0 0\n"
	"\tmaterial matte\n"
	"}\n";

//-----------------------------------------------------------------------------
// Test A: deferral + cascade + skip-unused via the Job API.
//-----------------------------------------------------------------------------
static void TestDeferralAndCascade()
{
	std::cout << "Test A: deferral + cascade + skip-unused (Job render)...\n";

	const std::string scenePath = WriteSceneToTempFile( kSceneText, "fourcase" );
	Check( !scenePath.empty(), "scene temp file written" );
	if( scenePath.empty() ) return;

	DisplacedGeometry::ResetBuildMeshCount();

	IJobPriv* pJob = 0;
	const bool created = RISE_CreateJobPriv( &pJob ) && pJob;
	Check( created, "Job created" );
	if( !created ) return;

	const bool loaded = pJob->LoadAsciiScene( scenePath.c_str() );
	Check( loaded, "scene loaded (parse)" );

	// PROOF OF DEFERRAL: parse must bake NOTHING.  Pre-change, all four
	// displaced geometries would have baked in their constructors.
	const unsigned int afterParse = DisplacedGeometry::GetBuildMeshCount();
	Check( afterParse == 0,
		( "no displaced bakes at parse time (got " + std::to_string( afterParse ) + ", want 0)" ).c_str() );

	if( !loaded ) { safe_release( pJob ); return; }

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "deferred-realize test capture" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool rendered = pJob->Rasterize();
	Check( rendered, "scene rendered" );

	// PROOF OF SELECTIVE REALIZE + CASCADE: the render's AttachScene realize
	// pass walks objects -> geometry.  Expected bakes:
	//   disp_single (bound)            -> 1
	//   disp_outer (bound)             -> 1
	//   disp_inner (cascade of outer)  -> 1
	//   disp_unbound (no object)       -> 0
	// total = 3.
	const unsigned int afterRender = DisplacedGeometry::GetBuildMeshCount();
	Check( afterRender == 3,
		( "exactly 3 displaced bakes after render — bound single + nested(outer+inner), unbound skipped (got "
		  + std::to_string( afterRender ) + ", want 3)" ).c_str() );

	// PROOF OF HIT-AFTER-REALIZE: the displaced objects rendered (non-black).
	Check( pCap->maxLum > 0.0,
		( "rendered image has non-black pixels (maxLum=" + std::to_string( pCap->maxLum ) + ")" ).c_str() );

	safe_release( pCap );
	safe_release( pJob );
}

//-----------------------------------------------------------------------------
// Test B: re-rendering the SAME scene is idempotent — the realize pass does
// not re-bake already-realized geometry (Realize() short-circuits).
//-----------------------------------------------------------------------------
static void TestIdempotentReRender()
{
	std::cout << "Test B: re-render is idempotent (no extra bakes)...\n";

	const std::string scenePath = WriteSceneToTempFile( kSceneText, "fourcase_b" );
	if( scenePath.empty() ) { Check( false, "scene temp file written" ); return; }

	DisplacedGeometry::ResetBuildMeshCount();

	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check( false, "Job created" ); return; }
	if( !pJob->LoadAsciiScene( scenePath.c_str() ) ) { Check( false, "scene loaded" ); safe_release( pJob ); return; }

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "deferred-realize test capture B" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	pJob->Rasterize();
	const unsigned int afterFirst = DisplacedGeometry::GetBuildMeshCount();
	Check( afterFirst == 3, ( "3 bakes after first render (got " + std::to_string( afterFirst ) + ")" ).c_str() );

	// Render again — the same realized geometries must NOT re-bake.
	pJob->Rasterize();
	const unsigned int afterSecond = DisplacedGeometry::GetBuildMeshCount();
	Check( afterSecond == afterFirst,
		( "no extra bakes on re-render (got " + std::to_string( afterSecond )
		  + ", want " + std::to_string( afterFirst ) + ")" ).c_str() );

	safe_release( pCap );
	safe_release( pJob );
}

// CSG scene: disp_csg is bound to csg_opA, which is combined with csg_opB into
// a csg_object.  AssignObjects() marks both operands world-INVISIBLE, so the
// realize pass (visible-objects-only) reaches disp_csg ONLY through the CSG
// cascade.  plain_sphere (csg_opB) is the non-displaced operand.
static const char* kCSGSceneText =
	"RISE ASCII SCENE 6\n"
	"\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 -8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n"
	"directional_light\n{\n\tname sun\n\tpower 3.14159\n\tcolor 1 1 1\n\tdirection 0 0 -1\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"sphere_geometry\n{\n\tname base_sphere\n\tradius 1.0\n}\n\n"
	"sphere_geometry\n{\n\tname plain_sphere\n\tradius 1.0\n}\n\n"
	"expression_function2d\n{\n\tname bump\n\texpr 0.1*sin(6.28318*u)*sin(6.28318*v)\n}\n\n"
	"displaced_geometry\n{\n\tname disp_csg\n\tbase_geometry base_sphere\n\tdetail 24\n\tdisplacement bump\n\tdisp_scale 0.1\n}\n\n"
	"standard_object\n{\n\tname csg_opA\n\tgeometry disp_csg\n\tposition 0 0 0\n\tmaterial matte\n}\n\n"
	"standard_object\n{\n\tname csg_opB\n\tgeometry plain_sphere\n\tposition 0.5 0 0\n\tmaterial matte\n}\n\n"
	"csg_object\n{\n\tname csg_union\n\tobja csg_opA\n\tobjb csg_opB\n\toperation union\n\tposition 0 0 0\n\tmaterial matte\n}\n";

//-----------------------------------------------------------------------------
// Test C: a displaced geometry used as a CSG operand IS realized.  The CSG's
// two operands are world-INVISIBLE (AssignObjects hides them), so the realize
// pass — which enumerates only world-visible objects — reaches the displaced
// operand ONLY through CSGObject::Realize()'s explicit cascade.  Pre-fix this
// baked 0 and the operand rendered as MISSING; with the fix it bakes once.
//-----------------------------------------------------------------------------
static void TestCSGOperandRealized()
{
	std::cout << "Test C: a displaced CSG operand is realized via cascade...\n";

	const std::string scenePath = WriteSceneToTempFile( kCSGSceneText, "csg" );
	if( scenePath.empty() ) { Check( false, "csg scene temp file written" ); return; }

	DisplacedGeometry::ResetBuildMeshCount();

	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check( false, "Job created" ); return; }
	const bool loaded = pJob->LoadAsciiScene( scenePath.c_str() );
	Check( loaded, "csg scene loaded (parse)" );
	if( !loaded ) { safe_release( pJob ); return; }

	const unsigned int afterParse = DisplacedGeometry::GetBuildMeshCount();
	Check( afterParse == 0,
		( "no displaced bakes at parse for the CSG operand (got " + std::to_string( afterParse ) + ")" ).c_str() );

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "deferred-realize CSG capture" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	Check( pJob->Rasterize(), "csg scene rendered" );

	// THE FIX: disp_csg is the sole displaced geometry and sits on a world-
	// invisible CSG operand; it bakes exactly once iff the CSG cascade reached it.
	const unsigned int afterRender = DisplacedGeometry::GetBuildMeshCount();
	Check( afterRender == 1,
		( "CSG displaced operand realized via cascade (got " + std::to_string( afterRender ) + ", want 1)" ).c_str() );

	Check( pCap->maxLum > 0.0,
		( "csg image non-black (maxLum=" + std::to_string( pCap->maxLum ) + ")" ).c_str() );

	safe_release( pCap );
	safe_release( pJob );
}

//-----------------------------------------------------------------------------
// Test D (P1 regression): ObjectManager::PrepareForRendering realizes deferred
// geometry ON ITS OWN.  The GUI editor's production-render + picking paths call
// PrepareForRendering DIRECTLY, before RayCaster::AttachScene's realize pass.
// If PrepareForRendering didn't realize, it would build the top-level BVH from
// the displaced geometry's ZERO pre-realize bbox and then KEEP it (the `!pBVH`
// guard never rebuilds) -> displaced objects vanish / become unpickable.
//-----------------------------------------------------------------------------
static void TestPrepareForRenderingRealizes()
{
	std::cout << "Test D: PrepareForRendering realizes deferred geometry (editor path)...\n";

	const std::string scenePath = WriteSceneToTempFile( kSceneText, "prep" );
	if( scenePath.empty() ) { Check( false, "scene temp file written" ); return; }

	DisplacedGeometry::ResetBuildMeshCount();

	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check( false, "Job created" ); return; }
	if( !pJob->LoadAsciiScene( scenePath.c_str() ) ) { Check( false, "scene loaded" ); safe_release( pJob ); return; }
	Check( DisplacedGeometry::GetBuildMeshCount() == 0, "parse bakes nothing" );

	// The editor scenario: build the TLAS DIRECTLY, with no render / AttachScene.
	pJob->GetObjects()->PrepareForRendering();

	const unsigned int afterPrepare = DisplacedGeometry::GetBuildMeshCount();
	Check( afterPrepare == 3,
		( "PrepareForRendering realized the bound displaced geometries (got "
		  + std::to_string( afterPrepare ) + ", want 3)" ).c_str() );

	safe_release( pJob );
}

// A path_instances_geometry whose TEMPLATE is a displaced_geometry.  The
// factory tessellates the template at parse to stamp instances; a deferred
// DisplacedGeometry returns false from TessellateToMesh until realized, so
// pre-fix this scene failed at parse.  The factory now Realize()s the template
// first (a template that is instanced IS used), so it bakes at parse.
static const char* kPathInstanceSceneText =
	"RISE ASCII SCENE 6\n"
	"\n"
	"film\n{\n\twidth 48\n\theight 48\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 -8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60.0\n}\n\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 1\n}\n\n"
	"directional_light\n{\n\tname sun\n\tpower 3.14159\n\tcolor 1 1 1\n\tdirection 0 0 -1\n}\n\n"
	"uniformcolor_painter\n{\n\tname albedo\n\tcolor 0.8 0.8 0.8\n}\n\n"
	"lambertian_material\n{\n\tname matte\n\treflectance albedo\n}\n\n"
	"sphere_geometry\n{\n\tname base_sphere\n\tradius 0.4\n}\n\n"
	"expression_function2d\n{\n\tname bump\n\texpr 0.1*sin(6.28318*u)*sin(6.28318*v)\n}\n\n"
	"displaced_geometry\n{\n\tname disp_tmpl\n\tbase_geometry base_sphere\n\tdetail 12\n\tdisplacement bump\n\tdisp_scale 0.1\n}\n\n"
	"path_instances_geometry\n{\n\tname beads\n\tgeometry disp_tmpl\n\tpoint -2 0 0\n\tpoint 0 1 0\n\tpoint 2 0 0\n\tpitch 1.0\n\tdetail 12\n}\n\n"
	"standard_object\n{\n\tname obj_beads\n\tgeometry beads\n\tposition 0 0 0\n\tmaterial matte\n}\n";

//-----------------------------------------------------------------------------
// Test E (P2 regression): a displaced geometry used as a path_instances
// TEMPLATE.  The factory realizes the template before tessellating it, so the
// scene parses (pre-fix it failed: TessellateToMesh returns false on an
// unrealized DisplacedGeometry) and the instanced beads render.
//-----------------------------------------------------------------------------
static void TestPathInstanceOfDisplaced()
{
	std::cout << "Test E: displaced geometry as a path_instances template...\n";

	const std::string scenePath = WriteSceneToTempFile( kPathInstanceSceneText, "pathinst" );
	if( scenePath.empty() ) { Check( false, "scene temp file written" ); return; }

	DisplacedGeometry::ResetBuildMeshCount();

	IJobPriv* pJob = 0;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check( false, "Job created" ); return; }
	const bool loaded = pJob->LoadAsciiScene( scenePath.c_str() );
	Check( loaded, "scene with displaced path-instance template parses (was a parse failure pre-fix)" );
	if( !loaded ) { safe_release( pJob ); return; }

	// The factory realized the template AT PARSE to tessellate it (the template
	// is instanced -> used -> correctly baked, not deferred).
	const unsigned int afterParse = DisplacedGeometry::GetBuildMeshCount();
	Check( afterParse == 1,
		( "path-instances factory realized the displaced template (got "
		  + std::to_string( afterParse ) + ", want 1)" ).c_str() );

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "path-instance test capture" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );
	Check( pJob->Rasterize(), "path-instance scene rendered" );
	Check( pCap->maxLum > 0.0,
		( "instanced beads render non-black (maxLum=" + std::to_string( pCap->maxLum ) + ")" ).c_str() );

	safe_release( pCap );
	safe_release( pJob );
}

int main()
{
	TestDeferralAndCascade();
	TestIdempotentReRender();
	TestCSGOperandRealized();
	TestPrepareForRenderingRealizes();
	TestPathInstanceOfDisplaced();

	for( const std::string& f : g_tmpScenes ) { std::remove( f.c_str() ); }

	if( g_failures == 0 ) {
		std::cout << "All DeferredRealize tests passed.\n";
		return 0;
	}
	std::cout << g_failures << " DeferredRealize check(s) FAILED.\n";
	return 1;
}
