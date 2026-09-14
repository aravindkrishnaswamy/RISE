//////////////////////////////////////////////////////////////////////
//
//  TranslucentInitialContainmentTest.cpp - Regression guard for DL-46:
//    a camera/light origin already inside a closed TranslucentMaterial
//    object was never seeded with its membership, so the first
//    physical crossing was misclassified as an ENTRY instead of an
//    EXIT.
//
//  THE BUG THIS TEST GUARDS AGAINST
//
//    `IORStackSeeding::SeedFromPoint`'s probe only tracked materials
//    whose `GetSpecularInfo` reported `canRefract=true` (a real
//    dielectric with its own numeric IOR).  `TranslucentMaterial`
//    inherited `IMaterial`'s invalid/non-refracting default
//    `GetSpecularInfo`, so the probe silently skipped it: an origin
//    strictly inside a closed translucent object was never pushed onto
//    the seed stack, even though `TranslucentSPF::Scatter`/`ScatterNM`
//    classify entry vs. exit purely from `ior_stack.containsCurrent()`
//    -- exactly like a real refractor.  The first real hit on the
//    object's boundary therefore ran the ENTERING branch (front
//    reflection + a NEW push) instead of the EXITING branch (Beer
//    extinction + a pop), a wrong-lobe-family misclassification
//    parallel to the submerged-camera bug this whole seeding mechanism
//    exists to fix (docs/SUBMERGED_CAMERA_IOR_SEEDING.md).
//
//  THE FIX
//
//    `SpecularInfo` gained a `hasInterior` flag (SpecularInfo.h) for a
//    material that tracks its own containment like a refractor but is
//    NOT specular and has no distinct IOR of its own.
//    `TranslucentMaterial::GetSpecularInfo` now reports it.
//    `SeedFromPoint` accepts `canRefract || hasInterior`, and for a
//    `hasInterior`-only entry it re-pushes whatever IOR is already on
//    the stack (rather than a captured constant) -- matching
//    `TranslucentSPF::Scatter`'s own `push(ior_stack.top())` -- so
//    nesting a translucent object inside a distinct refractive
//    enclosure preserves that enclosure's numeric IOR exactly.
//
//  COVERAGE
//
//    A real scene (real `sphere_geometry` + `standard_object` +
//    `translucent_material`/`perfectrefractor_material`/
//    `perfectreflector_material`/`lambertian_material`, loaded via the
//    production CST loader) with:
//      - a translucent sphere sitting in AIR,
//      - a translucent sphere NESTED inside a distinct glass (ior 1.5)
//        shell,
//      - a perfect-reflector sphere and a Lambertian sphere as
//        negative controls (neither reports canRefract nor
//        hasInterior; must NOT be seeded -- confirms hasInterior is
//        additive and doesn't leak onto unrelated materials).
//
//    Sub-test 1 -- SeedFromPoint direct assertions at five positions
//      (in-air translucent interior, nested translucent interior,
//      clearly outside everything, inside the mirror, inside the
//      Lambertian sphere).
//    Sub-test 2 -- production SeedFromPoint followed by a REAL ray
//      cast (IObjectManager::IntersectRay, not a synthetic stub) from
//      each seeded interior point outward to the object's own real
//      geometric boundary, then the REAL TranslucentSPF::Scatter/
//      ScatterNM at that hit: the first physical crossing must be
//      classified as EXIT (Beer attenuation + a pop -- not an entry's
//      new push), and the popped stack must reveal exactly the
//      enclosing numeric IOR (1.0 for the in-air case, 1.5 for the
//      nested-in-glass case).
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `translucent`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IObjectManager.h"

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
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/translucent_initial_containment_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

//////////////////////////////////////////////////////////////////////
//  Scene: a translucent sphere in air (radius 0.5, origin), a
//  translucent sphere (radius 0.5) NESTED inside a distinct glass
//  shell (radius 2.0, ior 1.5) both centered at z=6, a perfect
//  reflector sphere at z=12, and a Lambertian sphere at z=18 -- the
//  latter two are negative controls.
//////////////////////////////////////////////////////////////////////
static const char* kSceneText =
	"RISE ASCII SCENE 7\n"
	"\n"
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 -20\n"
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
	"\toidn_denoise FALSE\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname color_white\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"scalar_painter\n"
	"{\n"
	"\tname pnt_ior_glass\n"
	"\tvalue 1.5\n"
	"}\n"
	"\n"
	"perfectrefractor_material\n"
	"{\n"
	"\tname mat_glass\n"
	"\trefractance color_white\n"
	"\tior pnt_ior_glass\n"
	"}\n"
	"\n"
	"translucent_material\n"
	"{\n"
	"\tname mat_trans\n"
	"\tref color_white\n"
	"\ttau color_white\n"
	"\text 0\n"
	"\tN 1.0\n"
	"\tscattering 0.3\n"
	"}\n"
	"\n"
	"perfectreflector_material\n"
	"{\n"
	"\tname mat_mirror\n"
	"\treflectance color_white\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_lambert\n"
	"\treflectance color_white\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname sph_small\n"
	"\tradius 0.5\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname sph_big\n"
	"\tradius 2.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_trans_air\n"
	"\tgeometry sph_small\n"
	"\tposition 0 0 0\n"
	"\tmaterial mat_trans\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_glass_shell\n"
	"\tgeometry sph_big\n"
	"\tposition 0 0 6\n"
	"\tmaterial mat_glass\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_trans_nested\n"
	"\tgeometry sph_small\n"
	"\tposition 0 0 6\n"
	"\tmaterial mat_trans\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_mirror\n"
	"\tgeometry sph_small\n"
	"\tposition 0 0 12\n"
	"\tmaterial mat_mirror\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lambert\n"
	"\tgeometry sph_small\n"
	"\tposition 0 0 18\n"
	"\tmaterial mat_lambert\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
//  Sub-test 1: direct SeedFromPoint assertions.
//////////////////////////////////////////////////////////////////////
static void TestSeedFromPoint( IScenePriv& scene )
{
	std::cout << "Sub-test 1: SeedFromPoint direct assertions" << std::endl;

	// (A) Inside the translucent sphere sitting in air: must be seeded,
	// with the enclosing (air) IOR of 1.0 preserved -- translucent has
	// no distinct IOR of its own.  RED-PROOF: pre-fix, this stack stayed
	// empty because GetSpecularInfo reported canRefract=false/invalid.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,0), scene );
		const IObject* seeded = stack.topObject();
		Check( seeded != 0, "(A) translucent-in-air interior: something was seeded onto the stack" );
		if( seeded ) {
			stack.SetCurrentObject( seeded );
			Check( stack.containsCurrent() && std::fabs(stack.top() - 1.0) < 1e-9,
				"(A) translucent-in-air interior seeded with enclosing IOR 1.0" );
		}
	}

	// (B) Inside the translucent sphere NESTED inside the glass shell:
	// both objects must be seeded (the point is inside both), with the
	// glass's numeric IOR (1.5) preserved as top() once the translucent
	// object (no IOR of its own) is pushed on top of it.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,6), scene );
		Check( std::fabs(stack.top() - 1.5) < 1e-9,
			"(B) translucent-nested-in-glass: enclosing glass IOR 1.5 preserved on top" );
	}

	// (C) Clearly outside everything: stack stays untouched.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(10,10,-20), scene );
		Check( stack.topObject() == 0 && std::fabs(stack.top() - 1.0) < 1e-9,
			"(C) outside-everything: stack unaffected" );
	}

	// (D) Inside the pure reflector sphere: mirrors are not media and
	// must NOT be seeded (neither canRefract nor hasInterior) -- guards
	// against hasInterior leaking onto an unrelated material.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,12), scene );
		Check( stack.topObject() == 0 && std::fabs(stack.top() - 1.0) < 1e-9,
			"(D) mirror interior: stack unaffected (mirrors are not media)" );
	}

	// (E) Inside the Lambertian sphere: same non-media control.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0,0,18), scene );
		Check( stack.topObject() == 0 && std::fabs(stack.top() - 1.0) < 1e-9,
			"(E) Lambertian interior: stack unaffected (not a medium)" );
	}
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2: seeded stack -> real ray cast -> real Scatter/ScatterNM
//  classifies the first crossing as EXIT and preserves the enclosing
//  IOR through the pop.
//////////////////////////////////////////////////////////////////////
static void CheckFirstCrossingIsExit(
	IScenePriv& scene, const Point3& seedPos, Scalar expectedEnclosingIOR,
	const char* label )
{
	IORStack stack( 1.0 );
	IORStackSeeding::SeedFromPoint( stack, seedPos, scene );

	// Cast a real ray from the seed point straight out along +Z to the
	// object's own real geometric boundary (radius 0.5 spheres all
	// centered at the seed's (x,y) -- +Z exits at distance 0.5).
	RayIntersection ri( Ray( seedPos, Vector3(0,0,1) ), nullRasterizerState );
	scene.GetObjects()->IntersectRay( ri, true, true, false );

	Check( ri.geometric.bHit && ri.pObject != 0 && ri.pMaterial != 0,
		( std::string(label) + ": real ray finds the object's boundary" ).c_str() );
	if( !ri.geometric.bHit || !ri.pObject || !ri.pMaterial ) {
		return;
	}

	stack.SetCurrentObject( ri.pObject );
	const bool seededContains = stack.containsCurrent();
	Check( seededContains,
		( std::string(label) + ": seeded stack already contains the object (exit, not entry)" ).c_str() );

	ISPF* pSPF = ri.pMaterial->GetSPF();
	Check( pSPF != 0, ( std::string(label) + ": material exposes an SPF" ).c_str() );
	if( !pSPF ) {
		return;
	}

	RandomNumberGenerator rng( 12345 );
	IndependentSampler sampler( rng );

	for( unsigned int spectral = 0; spectral < 2; spectral++ ) {
		ScatteredRayContainer scattered;
		if( spectral ) {
			pSPF->ScatterNM( ri.geometric, sampler, 550.0, scattered, stack );
		} else {
			pSPF->Scatter( ri.geometric, sampler, scattered, stack );
		}

		// The EXIT branch's diffuse ray carries a non-null ior_stack that
		// has POPPED the object (containsCurrent() false for it) and
		// reveals the enclosing medium.  The ENTRY branch's translucent
		// lobe carries a non-null ior_stack that has PUSHED the object
		// (containsCurrent() true) -- the pre-fix misclassification.
		bool sawPoppedExit = false;
		Scalar revealedIOR = -1;
		for( unsigned int i = 0; i < scattered.Count(); i++ ) {
			if( scattered[i].ior_stack != 0 ) {
				IORStack* s = scattered[i].ior_stack;
				s->SetCurrentObject( ri.pObject );
				if( !s->containsCurrent() ) {
					sawPoppedExit = true;
					revealedIOR = s->top();
				}
			}
		}
		const char* tag = spectral ? "NM" : "RGB";
		Check( sawPoppedExit,
			( std::string(label) + " " + tag + ": first crossing classified as EXIT (popped stack), not entry" ).c_str() );
		if( sawPoppedExit ) {
			Check( std::fabs(revealedIOR - expectedEnclosingIOR) < 1e-6,
				( std::string(label) + " " + tag + ": popped stack reveals the correct enclosing IOR" ).c_str() );
		}
	}
}

static void TestFirstCrossing( IScenePriv& scene )
{
	std::cout << "Sub-test 2: seeded stack -> real Scatter/ScatterNM first-crossing classification" << std::endl;

	CheckFirstCrossingIsExit( scene, Point3(0,0,0), 1.0, "in-air" );
	CheckFirstCrossingIsExit( scene, Point3(0,0,6), 1.5, "nested-in-glass" );
}

int main()
{
	GlobalLog();

	std::cout << "TranslucentInitialContainmentTest: DL-46 initial-containment seeding" << std::endl;

	const std::string scenePath = WriteSceneToTempFile( kSceneText, "scene" );
	if( scenePath.empty() ) {
		Check( false, "scene write" );
	} else {
		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
			Check( false, "job create" );
		} else if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
			Check( false, "scene load" );
			safe_release( pJob );
		} else {
			IScenePriv* pScene = pJob->GetScene();
			if( !pScene ) {
				Check( false, "scene get" );
			} else {
				TestSeedFromPoint( *pScene );
				TestFirstCrossing( *pScene );
			}
			safe_release( pJob );
		}
		std::remove( scenePath.c_str() );
	}

	std::cout << std::endl << "Passed: " << passCount << std::endl << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
