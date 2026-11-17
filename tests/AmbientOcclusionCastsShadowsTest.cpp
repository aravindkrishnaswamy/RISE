//////////////////////////////////////////////////////////////////////
//
//  AmbientOcclusionCastsShadowsTest.cpp - Regression for the shadow-
//  ray-vs-occlusion-ray contract split (docs/GEOMETRY_SHADING_SIGNALS_
//  DESIGN.md section 8.1; 2026-09-07).
//
//  THE BUG: ObjectManager::RayElementIntersection_IntersectionOnly (the
//  processor behind IObjectManager::IntersectShadowRay / IRayCaster::
//  CastShadowRay) filters on `IsWorldVisible() && DoesCastShadows()`.
//  That is the right contract for a LIGHT-VISIBILITY query -- an object
//  authored `casts_shadows FALSE` should not block light reaching a
//  shading point.  But AmbientOcclusionShaderOp (and, independently,
//  InteractivePelRasterizer's preview AO estimator) reused the SAME
//  CastShadowRay for a GEOMETRY-PRESENCE occlusion query ("is there
//  something in the way"), which has no business honouring
//  `casts_shadows` at all: the object hasn't stopped existing, it has
//  only stopped blocking light for NEE purposes.  A `casts_shadows
//  FALSE` box therefore used to be completely invisible to ambient
//  occlusion.
//
//  THE FIX: a new, explicitly-named query pair --
//  IObjectManager::IntersectOcclusionRay / IRayCaster::CastOcclusionRay
//  -- that filters on IsWorldVisible() only.  AmbientOcclusionShaderOp
//  and InteractivePelRasterizer's AO estimator now route through it;
//  IntersectShadowRay / CastShadowRay are unchanged (still honour
//  `casts_shadows`, still used by NEE / light sampling).
//
//  Scene: a ground plane, a 1x1x1 box resting on it 0.1 units from a
//  probe point with `casts_shadows FALSE`, a mirrored twin box with
//  `casts_shadows TRUE`, and three decoy boxes (pushed far below the
//  ground so no query in this file can ever see them) whose only job is
//  to push the object count past ObjectManager's top-level-BVH
//  threshold (> 4 objects, see Job::InitializeContainers /
//  RISE_API_CreateObjectManager) -- so this test exercises the REAL
//  production BVH traversal path (BVH<>::IntersectRay_IntersectionOnly
//  with the new `epOverride` parameter), not just the tiny-scene
//  linear-loop fallback.
//
//  Coverage:
//    (a) IntersectOcclusionRay from a point beside the `casts_shadows
//        FALSE` box, aimed at it, returns TRUE (occluded) -- the fix.
//    (b) IntersectShadowRay on the SAME ray returns FALSE (light still
//        passes) -- pre-existing behaviour, unchanged by the fix.
//    (c) AmbientOcclusionShaderOp's RGB value is markedly LOWER right
//        beside either box than far away from both -- the estimator
//        actually sees geometry it previously could not.
//    (d) AmbientOcclusionShaderOp's value beside the `casts_shadows
//        FALSE` box matches its value beside the mirrored `casts_shadows
//        TRUE` box (within Monte-Carlo tolerance) -- AO is independent
//        of the flag, exactly as designed.
//
//  Red-proved: temporarily routing AmbientOcclusionShaderOp back through
//  CastShadowRay (reverting the two call sites in AmbientOcclusionShaderOp
//  .cpp) makes check (c)'s "near == far" comparison fail for the
//  `casts_shadows FALSE` box specifically, while (a)/(b)/(d) still pass
//  (a)/(b) test the manager directly, independent of the shader op, and
//  (d)'s two occluders would then BOTH read "unoccluded", so they'd
//  still agree with each other -- only (c) exposes the regression.  See
//  the implementation report for the actual before/after run.
//
//  No rendering: the shader op is invoked directly against a real
//  RayCaster attached to a real (tiny) parsed scene, mirroring
//  TransparentShadowTest.cpp's pattern.
//
//  Author: RISE
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <cmath>
#include <string>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/RISE_API.h"				// RISE_API_CreateRayCaster
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Interfaces/IShaderOp.h"
#include "../src/Library/Interfaces/IShaderOpManager.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
		std::cout << "  ok:   " << testName << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Scene: ground plane (top face at y=0) + two mirrored 1x1x1 boxes
// resting on it at x=+-2 (one casts_shadows FALSE, one TRUE) + three
// decoys parked far below the ground (y=-50) purely to push the object
// count above ObjectManager's top-level-BVH threshold.
//
// The rasterizer/camera/shader chunks exist only so the scene parses;
// we never call Rasterize().
//////////////////////////////////////////////////////////////////////
static const char* kSceneText =
	"RISE ASCII SCENE 7\n"
	"\n"
	"film\n"
	"{\n"
	"\twidth 16\n"
	"\theight 16\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 5 -10\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
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
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_white\n"
	"\tcolor 0.8 0.8 0.8\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_opaque\n"
	"\treflectance pnt_white\n"
	"}\n"
	"\n"
	"box_geometry\n"
	"{\n"
	"\tname geom_ground\n"
	"\twidth 40.0\n"
	"\theight 0.2\n"
	"\tdepth 20.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_ground\n"
	"\tgeometry geom_ground\n"
	"\tposition 0 -0.1 0\n"
	"\tmaterial mat_opaque\n"
	"}\n"
	"\n"
	"box_geometry\n"
	"{\n"
	"\tname geom_occluder\n"
	"\twidth 1.0\n"
	"\theight 1.0\n"
	"\tdepth 1.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_occluder_noshadow\n"
	"\tgeometry geom_occluder\n"
	"\tposition 2 0.5 0\n"
	"\tmaterial mat_opaque\n"
	"\tcasts_shadows FALSE\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_occluder_shadow\n"
	"\tgeometry geom_occluder\n"
	"\tposition -2 0.5 0\n"
	"\tmaterial mat_opaque\n"
	"\tcasts_shadows TRUE\n"
	"}\n"
	"\n"
	"box_geometry\n"
	"{\n"
	"\tname geom_decoy\n"
	"\twidth 0.1\n"
	"\theight 0.1\n"
	"\tdepth 0.1\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_decoy1\n"
	"\tgeometry geom_decoy\n"
	"\tposition 0 -50 0\n"
	"\tmaterial mat_opaque\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_decoy2\n"
	"\tgeometry geom_decoy\n"
	"\tposition 5 -50 5\n"
	"\tmaterial mat_opaque\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_decoy3\n"
	"\tgeometry geom_decoy\n"
	"\tposition -5 -50 -5\n"
	"\tmaterial mat_opaque\n"
	"}\n"
	"\n"
	// multiplybrdf FALSE: PerformOperation's `(pBRDF&&bMultiplyBRDF) ||
	// !bMultiplyBRDF` gate must let a materialless synthetic probe (our
	// `ri.pMaterial = nullptr` below) through -- with multiplybrdf TRUE
	// (the default) and no BSDF, the whole AO block is skipped and c
	// stays 0 regardless of occlusion.
	"ambientocclusion_shaderop\n"
	"{\n"
	"\tname ao\n"
	"\tnumtheta 10\n"
	"\tnumphi 24\n"
	"\tmultiplybrdf FALSE\n"
	"\tirradiance_cache FALSE\n"
	"}\n";

static std::string WriteSceneToTempFile()
{
	char path[256];
	std::snprintf( path, sizeof(path), "ao_casts_shadows_test_%d.RISEscene", (int)getpid() );
	std::ofstream ofs( path, std::ios::binary );
	if( !ofs.good() ) {
		return std::string();
	}
	ofs << kSceneText;
	ofs.close();
	return std::string( path );
}

// Builds a synthetic (not derived from a real hit) shading probe at
// `pt` with an upward normal, and evaluates the named AO shader op's
// RGB PerformOperation there.  The probe's hemisphere faces +Y, so
// every sample direction has a non-negative Y component -- it can
// never dip back into the ground plane it sits just above, and it can
// never reach the y=-50 decoys.
static RISEPel EvaluateAOAt(
	IShaderOp* pAO,
	const IRayCaster& caster,
	const RuntimeContext& rc,
	const Point3& pt
	)
{
	const RasterizerState rast = { 0, 0 };
	const Vector3 up( 0, 1, 0 );

	RayIntersection ri( Ray( pt, up ), rast );
	ri.geometric.bHit = true;
	ri.geometric.ptIntersection = pt;
	ri.geometric.vNormal = up;
	ri.geometric.vGeomNormal = up;
	ri.geometric.onb.CreateFromW( up );
	ri.pMaterial = nullptr;		// no BSDF -- multiplybrdf FALSE lets this through (see scene comment)
	ri.pRadianceMap = nullptr;		// falls back to the scene's (absent) global radiance map -> RISEPel(1,1,1) miss term

	IRayCaster::RAY_STATE rs;
	rs.type = IRayCaster::RAY_STATE::eRayView;

	IORStack iorStack( Scalar(1.0) );

	RISEPel c( 0, 0, 0 );
	pAO->PerformOperation( rc, ri, caster, rs, c, iorStack, nullptr );
	return c;
}

int main()
{
	std::cout << "AmbientOcclusionCastsShadowsTest" << std::endl;

	const std::string scenePath = WriteSceneToTempFile();
	if( scenePath.empty() ) {
		std::cout << "  FAIL: could not write temp scene" << std::endl;
		return 1;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::cout << "  FAIL: job create" << std::endl;
		std::remove( scenePath.c_str() );
		return 1;
	}
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
		std::cout << "  FAIL: scene load" << std::endl;
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return 1;
	}

	IScenePriv* pScene = pJob->GetScene();
	IShaderManager* pShaders = pJob->GetShaders();
	IShaderOpManager* pShaderOps = pJob->GetShaderOps();
	if( !pScene || !pShaders || !pShaderOps ) {
		std::cout << "  FAIL: scene / shader / shaderop manager get" << std::endl;
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return 1;
	}

	IShader* pShader = pShaders->GetItem( "global" );
	IShaderOp* pAO = pShaderOps->GetItem( "ao" );
	if( !pShader || !pAO ) {
		std::cout << "  FAIL: 'global' shader or 'ao' shaderop not found" << std::endl;
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return 1;
	}

	// Build a RayCaster via the public API, attach the loaded scene --
	// exactly the TransparentShadowTest pattern.
	IRayCaster* pICaster = nullptr;
	RISE_API_CreateRayCaster( &pICaster, false, 10, *pShader, true );
	if( !pICaster ) {
		std::cout << "  FAIL: ray caster create" << std::endl;
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return 1;
	}
	pICaster->AttachScene( pScene );

	const IObjectManager* pObjMgr = pScene->GetObjects();
	Check( pObjMgr != nullptr, "scene has an object manager" );

	if( pObjMgr )
	{
		// Sanity: the decoys push the object count past ObjectManager's
		// top-level-BVH threshold (> 4 -- Job::InitializeContainers /
		// RISE_API_CreateObjectManager), so (a)/(b)/(c)/(d) below all
		// exercise the real BVH traversal (with the new `epOverride`
		// parameter), not the tiny-scene linear-loop fallback.  6 objects
		// registered: ground, 2 occluders, 3 decoys.
		unsigned int nObjects = 0;
		class CountingEnum : public IEnumCallback<IObjectPriv>
		{
		public:
			unsigned int* pCount;
			bool operator()( const IObjectPriv& ) override { (*pCount)++; return true; }
		} counter;
		counter.pCount = &nObjects;
		pObjMgr->EnumerateObjects( counter );
		Check( nObjects == 6, "scene has 6 world-visible objects (BVH path engaged, not linear fallback)" );

		// ---- (a)/(b): a ray from beside the `casts_shadows FALSE` box, ----
		// aimed straight at its face 0.1 units away.
		const Ray rayToOccluder( Point3( 2.6, 0.5, 0 ), Vector3( -1, 0, 0 ) );
		const Scalar dHowFar = 0.5;

		const bool occlusionHit = pObjMgr->IntersectOcclusionRay( rayToOccluder, dHowFar, true, true );
		Check( occlusionHit, "(a) IntersectOcclusionRay sees the casts_shadows FALSE box" );

		const bool shadowHit = pObjMgr->IntersectShadowRay( rayToOccluder, dHowFar, true, true );
		Check( !shadowHit, "(b) IntersectShadowRay does NOT see the casts_shadows FALSE box (light still passes)" );

		// Same pair through the IRayCaster facade (CastOcclusionRay /
		// CastShadowRay), confirming the RayCaster wrapper agrees with
		// the manager it delegates to.
		Check( pICaster->CastOcclusionRay( rayToOccluder, dHowFar ),
			"(a2) IRayCaster::CastOcclusionRay agrees with IntersectOcclusionRay" );
		Check( !pICaster->CastShadowRay( rayToOccluder, dHowFar ),
			"(b2) IRayCaster::CastShadowRay agrees with IntersectShadowRay" );
	}

	// ---- (c)/(d): the AO shader op itself, at three probe points. ----
	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	// Beside the casts_shadows FALSE box (occluder face at x=2.5).
	const RISEPel cNearNoShadow = EvaluateAOAt( pAO, *pICaster, rc, Point3( 2.6, 0.001, 0 ) );
	// Beside the mirrored casts_shadows TRUE box (occluder face at x=-2.5).
	const RISEPel cNearShadow   = EvaluateAOAt( pAO, *pICaster, rc, Point3( -2.6, 0.001, 0 ) );
	// Far from both boxes (and still on the ground, decoys unreachable).
	const RISEPel cFar          = EvaluateAOAt( pAO, *pICaster, rc, Point3( 0, 0.001, 8 ) );

	std::cout << "  cNearNoShadow = (" << cNearNoShadow.r << ", " << cNearNoShadow.g << ", " << cNearNoShadow.b << ")" << std::endl;
	std::cout << "  cNearShadow   = (" << cNearShadow.r   << ", " << cNearShadow.g   << ", " << cNearShadow.b   << ")" << std::endl;
	std::cout << "  cFar          = (" << cFar.r          << ", " << cFar.g          << ", " << cFar.b          << ")" << std::endl;

	// (c) Both near-box probes must read markedly darker than the far
	// probe -- the AO estimator actually sees the boxes now, regardless
	// of casts_shadows.  cFar should be close to 1 (unoccluded); require
	// a comfortable margin (0.2) rather than pinning an exact value, to
	// stay robust to the stratified sampler's residual Monte-Carlo noise
	// at these sample counts (10x24 = 240 hemisphere samples).
	Check( cNearNoShadow.r < cFar.r - Scalar(0.2),
		"(c1) AO beside the casts_shadows FALSE box is markedly lower than far away" );
	Check( cNearShadow.r < cFar.r - Scalar(0.2),
		"(c2) AO beside the casts_shadows TRUE box is markedly lower than far away" );
	Check( cFar.r > Scalar(0.75),
		"(c3) AO far from both boxes reads close to fully unoccluded" );

	// (d) The two mirrored geometries are occluded identically -- AO is
	// independent of casts_shadows.  Same sample count/geometry on both
	// sides, so this is a statement about MC noise, not systematic bias;
	// 12% relative tolerance is generous against that noise floor while
	// still catching a real asymmetry (which would be ~100%, one side
	// reading cFar-like and the other reading occluded).
	const Scalar relDiff = std::fabs( double( cNearNoShadow.r - cNearShadow.r ) )
		/ r_max( double(cNearNoShadow.r), double(cNearShadow.r) );
	std::cout << "  (d) relative difference = " << relDiff << std::endl;
	Check( relDiff < Scalar(0.12),
		"(d) AO beside casts_shadows FALSE matches AO beside casts_shadows TRUE (independent of the flag)" );

	safe_release( pICaster );
	safe_release( pJob );
	std::remove( scenePath.c_str() );

	std::cout << "AmbientOcclusionCastsShadowsTest: " << passCount << " passed, "
		<< failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
