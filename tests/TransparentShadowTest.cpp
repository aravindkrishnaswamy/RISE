//////////////////////////////////////////////////////////////////////
//
//  TransparentShadowTest.cpp - Unit tests for transparent (Fresnel-
//  transmittance-attenuated) shadow rays.
//
//  Exercises RayCaster::CastShadowRayTransmittance directly against a
//  tiny in-process scene (one glass slab, one opaque slab), built via
//  the Job API + scene parser.  No rendering — the shadow-ray walk is
//  called at the API level so the assertions pin the physics
//  independent of any integrator.
//
//  Coverage:
//    (1) RGB transmittance through a clear specular-dielectric slab:
//        a straight +Z shadow ray crossing two air<->glass interfaces
//        returns ~ (1 - F)^2 where F is the normal-incidence Fresnel
//        reflectance for eta = 1.76 (sapphire).  Expected ~0.8541.
//    (2) NM (single-wavelength) path returns the same transmittance.
//    (3) An OPAQUE (Lambertian) slab fully blocks in BOTH the binary
//        and the transmittance walk (transmittance -> 0, returns true).
//    (4) A clear segment (no geometry between the two points) returns
//        transmittance 1 and "not occluded".
//    (5) The binary CastShadowRay still reports the glass slab as a
//        full occluder (default behaviour is unchanged).
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

#include "../src/Library/RISE_API.h"		// RISE_API_CreateRayCaster
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Rendering/RayCaster.h"
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

static void CheckClose( double got, double want, double tol, const char* testName )
{
	const bool ok = std::fabs( got - want ) <= tol;
	if( ok ) {
		passCount++;
		std::cout << "  ok:   " << testName
			<< "  (got " << got << ", want " << want << " +/- " << tol << ")" << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName
			<< "  (got " << got << ", want " << want << " +/- " << tol << ")" << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Scene: a glass (perfect-refractor, eta = 1.76) slab and an opaque
// (Lambertian) slab.  Both are thin in Z, wide in X/Y, so a +Z shadow
// ray through the center crosses two faces of one slab.
//
//   glass slab : centered at (0, 0, 0),  depth 0.4  -> faces z = -0.2, +0.2
//   opaque slab: centered at (0, 0, 8),  depth 0.4  -> faces z = +7.8, +8.2
//
// The rasterizer chunk is pathtracing_pel_rasterizer purely so the
// scene parses and the default "global" shader exists; we never call
// Rasterize() in this test.
//////////////////////////////////////////////////////////////////////
static const char* kSceneText =
	"RISE ASCII SCENE 6\n"
	"\n"
	"film\n"
	"{\n"
	"\twidth 16\n"
	"\theight 16\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 -5\n"
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
	"scalar_painter\n"
	"{\n"
	"\tname pnt_ior\n"
	"\tvalue 1.76\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_refr\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_white\n"
	"\tcolor 0.8 0.8 0.8\n"
	"}\n"
	"\n"
	"perfectrefractor_material\n"
	"{\n"
	"\tname mat_glass\n"
	"\trefractance pnt_refr\n"
	"\tior pnt_ior\n"
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
	"\tname slab_geom\n"
	"\twidth 4.0\n"
	"\theight 4.0\n"
	"\tdepth 0.4\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_glass\n"
	"\tgeometry slab_geom\n"
	"\tposition 0 0 0\n"
	"\tmaterial mat_glass\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_opaque\n"
	"\tgeometry slab_geom\n"
	"\tposition 0 0 8\n"
	"\tmaterial mat_opaque\n"
	"}\n"
	"\n"
	"scalar_painter\n"
	"{\n"
	"\tname pnt_zero\n"
	"\tvalue 0.0\n"
	"}\n"
	"\n"
	"scalar_painter\n"
	"{\n"
	"\tname pnt_scat\n"
	"\tvalue 1.0\n"
	"}\n"
	"\n"
	"subsurfacescattering_material\n"
	"{\n"
	"\tname mat_sss\n"
	"\tior pnt_ior\n"
	"\tabsorption pnt_zero\n"
	"\tscattering pnt_scat\n"
	"\tg pnt_zero\n"
	"\troughness pnt_zero\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_sss\n"
	"\tgeometry slab_geom\n"
	"\tposition 0 0 16\n"
	"\tmaterial mat_sss\n"
	"}\n";

static std::string WriteSceneToTempFile()
{
	char path[512];
	std::snprintf( path, sizeof(path), "transparent_shadow_test_%d.RISEscene", (int)getpid() );
	std::ofstream ofs( path, std::ios::binary );
	if( !ofs.good() ) {
		return std::string();
	}
	ofs << kSceneText;
	ofs.close();
	return std::string( path );
}

int main()
{
	std::cout << "TransparentShadowTest" << std::endl;

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
	if( !pJob->LoadAsciiScene( scenePath.c_str() ) ) {
		std::cout << "  FAIL: scene load" << std::endl;
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return 1;
	}

	IScenePriv* pScene = pJob->GetScene();
	IShaderManager* pShaders = pJob->GetShaders();
	if( !pScene || !pShaders ) {
		std::cout << "  FAIL: scene / shader manager get" << std::endl;
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return 1;
	}

	IShader* pShader = pShaders->GetItem( "global" );
	if( !pShader ) {
		std::cout << "  FAIL: default shader 'global' not found" << std::endl;
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return 1;
	}

	// Build a RayCaster via the public API, attach the loaded scene.
	IRayCaster* pICaster = nullptr;
	RISE_API_CreateRayCaster( &pICaster, false, 10, *pShader, true );
	if( !pICaster ) {
		std::cout << "  FAIL: ray caster create" << std::endl;
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return 1;
	}
	pICaster->AttachScene( pScene );

	// CastShadowRayTransmittance is a concrete-class method (not on the
	// IRayCaster interface — ABI: no new virtual on the abstract
	// interface).  dynamic_cast to reach it, exactly as LightSampler does.
	RayCaster* pCaster = dynamic_cast<RayCaster*>( pICaster );
	Check( pCaster != nullptr, "dynamic_cast IRayCaster -> RayCaster" );

	// Expected normal-incidence Fresnel for eta = 1.76.
	const double eta = 1.76;
	const double F = ( (eta - 1.0) / (eta + 1.0) ) * ( (eta - 1.0) / (eta + 1.0) );
	const double perSurfaceT = 1.0 - F;
	const double slabT = perSurfaceT * perSurfaceT;	// two interfaces

	if( pCaster )
	{
		// A straight +Z ray from z = -2 toward the light at z = +2.
		// Distance 4; the glass slab faces are at z = -0.2 and z = +0.2.
		const Ray rayGlass( Point3( 0, 0, -2 ), Vector3( 0, 0, 1 ) );
		const Scalar distToLight = 4.0;

		// ---- (5) Binary path STILL blocks the glass slab (default). ----
		{
			const bool occludedBinary = pCaster->CastShadowRay( rayGlass, distToLight - 0.001 );
			Check( occludedBinary, "(5) binary CastShadowRay: glass slab blocks (default unchanged)" );
		}

		// ---- (1) RGB transparent path: transmittance ~ (1-F)^2. ----
		pCaster->SetTransparentShadows( true );
		Check( pCaster->GetTransparentShadows(), "transparent shadows flag set" );
		{
			RISEPel T( 0, 0, 0 );
			const bool occluded = pCaster->CastShadowRayTransmittance(
				rayGlass, distToLight - 0.001, false, 0.0, T );
			Check( !occluded, "(1) RGB: glass slab is NOT a full occluder" );
			// All channels equal (clear glass, no dispersion / tint).
			CheckClose( T.r, slabT, 0.02 * slabT, "(1) RGB: transmittance.r ~ (1-F)^2" );
			CheckClose( T.g, slabT, 0.02 * slabT, "(1) RGB: transmittance.g ~ (1-F)^2" );
			CheckClose( T.b, slabT, 0.02 * slabT, "(1) RGB: transmittance.b ~ (1-F)^2" );
		}

		// ---- (2) NM path: same transmittance at a single wavelength. ----
		{
			RISEPel T( 0, 0, 0 );
			const bool occluded = pCaster->CastShadowRayTransmittance(
				rayGlass, distToLight - 0.001, true, 550.0, T );
			Check( !occluded, "(2) NM: glass slab is NOT a full occluder" );
			CheckClose( T.r, slabT, 0.02 * slabT, "(2) NM: transmittance ~ (1-F)^2" );
		}

		// ---- (4) Clear segment: a short ray that ends BEFORE the slab. ----
		{
			RISEPel T( 0, 0, 0 );
			// From z = -2 toward +Z but stop at z = -1 (distance 1): no
			// geometry in [-2, -1], so transmittance must be exactly 1.
			const bool occluded = pCaster->CastShadowRayTransmittance(
				rayGlass, 1.0, false, 0.0, T );
			Check( !occluded, "(4) clear segment: not occluded" );
			CheckClose( T.r, 1.0, 1e-9, "(4) clear segment: transmittance == 1" );
		}

		// ---- (3) Opaque slab fully blocks in BOTH modes. ----
		{
			// Ray from z = +6 toward +Z to a light at z = +10; the opaque
			// Lambertian slab sits at z in [+7.8, +8.2].
			const Ray rayOpaque( Point3( 0, 0, 6 ), Vector3( 0, 0, 1 ) );
			const Scalar distOpaque = 4.0;

			// Binary mode is currently ON (we set transparent shadows true),
			// so test the transmittance walk first.
			RISEPel T( 1, 1, 1 );
			const bool occludedT = pCaster->CastShadowRayTransmittance(
				rayOpaque, distOpaque - 0.001, false, 0.0, T );
			Check( occludedT, "(3) opaque slab blocks in transparent mode" );

			// And the binary path.
			pCaster->SetTransparentShadows( false );
			const bool occludedBin = pCaster->CastShadowRay( rayOpaque, distOpaque - 0.001 );
			Check( occludedBin, "(3) opaque slab blocks in binary mode" );
			pCaster->SetTransparentShadows( true );
		}

		// ---- (6) Smooth SUBSURFACE-SCATTERING slab BLOCKS in transparent
		// mode.  It reports isSpecular && canRefract (a smooth dielectric
		// boundary) but is NOT a clear shell -- the volume behind it
		// scatters.  Regression for the clearTransmission predicate gate:
		// before it, this leaked light straight through a diffusing solid.
		{
			// Ray from z = +14 toward +Z to a light at z = +20; the SSS slab
			// sits at z in [+15.8, +16.2].
			const Ray raySSS( Point3( 0, 0, 14 ), Vector3( 0, 0, 1 ) );
			const Scalar distSSS = 6.0;
			RISEPel T( 1, 1, 1 );
			const bool occluded = pCaster->CastShadowRayTransmittance(
				raySSS, distSSS - 0.001, false, 0.0, T );
			Check( occluded, "(6) smooth SSS slab blocks (clearTransmission gate)" );
		}

		// ---- Extra: with the flag OFF, the helper path is binary. ----
		// (Sanity that toggling back works.)
		pCaster->SetTransparentShadows( false );
		Check( !pCaster->GetTransparentShadows(), "transparent shadows flag cleared" );
	}

	safe_release( pICaster );
	safe_release( pJob );
	std::remove( scenePath.c_str() );

	std::cout << "TransparentShadowTest: " << passCount << " passed, "
		<< failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
