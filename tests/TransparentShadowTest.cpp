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
#include "../src/Library/Interfaces/ILightManager.h"		// ILightManager / ILightPriv — directional-light fix test
#include "../src/Library/Interfaces/IMaterial.h"			// IMaterial::GetBSDF
#include "../src/Library/Interfaces/IBSDF.h"				// IBSDF::value
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"	// RayIntersectionGeometric / nullRasterizerState
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

// Per-wavelength transparent-shadow attenuation of a positional light's NM
// direct-lighting at a fixed shading point: shadowed (flag ON) / unshadowed,
// at wavelength @a nm.  bReceivesShadows toggles ONLY the shadow term, so the
// ratio isolates the per-interface Fresnel transmittance the NM walk applied.
// Returns -1 if the unshadowed reference is ~0 (not lit).
static double AttenNM( const ILightPriv* L, RayCaster* C,
	IRayCaster* IC, const RayIntersectionGeometric& ri, IBSDF* B, double nm )
{
	C->SetTransparentShadows( true );
	const double on  = L->ComputeDirectLightingNM( ri, *IC, *B, true,  nm );
	const double ref = L->ComputeDirectLightingNM( ri, *IC, *B, false, nm );
	return ref > 1.0e-12 ? on / ref : -1.0;
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
	"}\n"
	"\n"
	// --- Geometry-parity fixtures: a PRIMITIVE glass sphere and an SDF glass
	// sphere of the SAME radius/material, off the z-axis so a +Z shadow ray
	// crosses each at normal incidence (two interfaces, like the slab).  Used
	// to pin that transparent_shadows is honored identically for analytic
	// primitives and SDFs (no impl-type divergence).
	"sphere_geometry\n"
	"{\n"
	"\tname prim_sph_geom\n"
	"\tradius 1.0\n"
	"}\n"
	"\n"
	"sdf_geometry\n"
	"{\n"
	"\tname sdf_sph_geom\n"
	"\tpart sphere union 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_prim_sph\n"
	"\tgeometry prim_sph_geom\n"
	"\tposition 5 0 0\n"
	"\tmaterial mat_glass\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_sdf_sph\n"
	"\tgeometry sdf_sph_geom\n"
	"\tposition -5 0 0\n"
	"\tmaterial mat_glass\n"
	"}\n"
	"\n"
	// --- Directional-light fixtures: a far glass slab with clear space beyond
	// it (so a +Z shadow ray crosses only this slab, then escapes), and a
	// directional light pointing toward +Z.  Used to verify the directional
	// light honors transparent_shadows.
	"standard_object\n"
	"{\n"
	"\tname obj_glass_far\n"
	"\tgeometry slab_geom\n"
	"\tposition 30 0 0\n"
	"\tmaterial mat_glass\n"
	"}\n"
	"\n"
	"directional_light\n"
	"{\n"
	"\tname dir_key\n"
	"\tpower 1.0\n"
	"\tcolor 1 1 1\n"
	"\tdirection 0 0 1\n"
	"}\n"
	"\n"
	// --- Dispersive-glass fixtures for the positional-light NM test.  A
	// sellmeier (sapphire) IOR makes the Fresnel transmittance WAVELENGTH-
	// DEPENDENT, so PointLight/SpotLight::ComputeDirectLightingNM (which must
	// use the per-wavelength IOR) gives a different attenuation at 450 vs 650nm
	// -- something the inherited RGB-projection fallback (single representative
	// IOR) cannot do.  The slab sits at x=40 with clear +Z space beyond it.
	"scalar_painter\n"
	"{\n"
	"\tname pnt_disp_ior\n"
	"\tsellmeier 1.4313493 0.65054713 5.3414021 0.0052799 0.0142383 325.0178\n"
	"}\n"
	"\n"
	"perfectrefractor_material\n"
	"{\n"
	"\tname mat_disp\n"
	"\trefractance pnt_refr\n"
	"\tior pnt_disp_ior\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_disp_glass\n"
	"\tgeometry slab_geom\n"
	"\tposition 40 0 0\n"
	"\tmaterial mat_disp\n"
	"}\n"
	"\n"
	"omni_light\n"
	"{\n"
	"\tname pnt_key\n"
	"\tpower 1.0\n"
	"\tposition 40 0 6\n"
	"\tcolor 1 1 1\n"
	"}\n"
	"\n"
	"spot_light\n"
	"{\n"
	"\tname spot_key\n"
	"\tpower 1.0\n"
	"\tinner 60\n"
	"\touter 120\n"
	"\tposition 40 0 6\n"
	"\ttarget 40 0 -2\n"
	"\tcolor 1 1 1\n"
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

		// ---- (7) GEOMETRY PARITY.  A primitive sphere_geometry glass and an
		// sdf_geometry sphere glass of identical radius/material occlude the
		// BINARY shadow ray identically (FALSE -> dark for both) and transmit
		// the Fresnel walk identically (TRUE -> lit for both).  Pins that
		// transparent_shadows is honored UNIFORMLY across analytic-primitive
		// and SDF dielectrics -- the regression guard against any future
		// impl-type divergence in the shadow-occlusion path.
		{
			// Both probes below call CastShadowRay / CastShadowRayTransmittance
			// directly (flag-independent), but pin the flag to a known state so
			// the block does not depend on prior blocks' mutations of it.
			pCaster->SetTransparentShadows( false );

			// primitive sphere @ (5,0,0) r=1; SDF sphere @ (-5,0,0) r=1.  A +Z
			// ray through each centre crosses two interfaces at normal
			// incidence, so the transmittance is (1-F)^2 == slabT.
			const Ray rayPrim( Point3(  5, 0, -3 ), Vector3( 0, 0, 1 ) );
			const Ray raySdf(  Point3( -5, 0, -3 ), Vector3( 0, 0, 1 ) );
			const Scalar dFar = 4.5;	// endpoint z=+1.5, just past the back face at z=+1

			// Binary occludes BOTH geometries.
			Check( pCaster->CastShadowRay( rayPrim, dFar ), "(7) binary: PRIMITIVE sphere glass occludes" );
			Check( pCaster->CastShadowRay( raySdf,  dFar ), "(7) binary: SDF sphere glass occludes" );

			// Transmittance passes BOTH with equal Fresnel attenuation.
			RISEPel Tprim( 0, 0, 0 ), Tsdf( 0, 0, 0 );
			const bool occPrim = pCaster->CastShadowRayTransmittance( rayPrim, dFar, false, 0.0, Tprim );
			const bool occSdf  = pCaster->CastShadowRayTransmittance( raySdf,  dFar, false, 0.0, Tsdf );
			Check( !occPrim, "(7) transmittance: PRIMITIVE sphere glass NOT a full occluder" );
			Check( !occSdf,  "(7) transmittance: SDF sphere glass NOT a full occluder" );
			CheckClose( Tprim.r, slabT, 0.03 * slabT, "(7) PRIMITIVE sphere transmittance ~ (1-F)^2" );
			CheckClose( Tsdf.r,  slabT, 0.03 * slabT, "(7) SDF sphere transmittance ~ (1-F)^2" );
			// The crux: primitive and SDF transmittance are equal to within the
			// SDF marcher's numerical noise -- NO primitive-vs-SDF divergence.
			CheckClose( Tprim.r, Tsdf.r, 0.015, "(7) PARITY: primitive == SDF transmittance" );
		}

		// ---- (8) DIRECTIONAL light honors transparent_shadows.  Before the
		// fix, DirectionalLight::ComputeDirectLighting always used the binary
		// CastShadowRay, so the flag was silently ignored for directional
		// lights (exactly the matte-dial-under-crystal use case the feature
		// targets).  Now a clear dielectric between the surface and the light
		// attenuates by Fresnel transmittance when the flag is on, and fully
		// blocks when it is off.  Asserted via DirectionalLight's NEE entry
		// point ComputeDirectLighting, comparing under-glass vs a clear point.
		{
			const ILightManager* pLights = pScene->GetLights();
			const ILightPriv* pDir = 0;
			if( pLights ) {
				const ILightManager::LightsList& lights = pLights->getLights();
				for( ILightManager::LightsList::const_iterator li = lights.begin(); li != lights.end(); ++li ) {
					if( (*li) && (*li)->lightType() == ILight::LightType::Directional ) { pDir = *li; break; }
				}
			}
			Check( pDir != 0, "(8) directional light present in scene" );

			IMaterialManager* pMatMgr = pJob->GetMaterials();
			IMaterial* pMatOpaque = pMatMgr ? pMatMgr->GetItem( "mat_opaque" ) : 0;
			IBSDF* pBSDF = pMatOpaque ? pMatOpaque->GetBSDF() : 0;
			Check( pBSDF != 0, "(8) opaque-material BSDF available" );

			if( pDir && pBSDF ) {
				const Vector3 N( 0, 0, 1 );
				// Viewer ray points -Z (looking at the +Z-facing front face), so
				// the Lambertian BRDF sees viewer and light on the same side of
				// N (LambertianBRDF::ShouldReflect) and returns a nonzero value.
				const Ray dummy( Point3( 30, 0, -2 ), Vector3( 0, 0, -1 ) );

				// Surface point UNDER the far glass slab (+Z shadow ray crosses it).
				RayIntersectionGeometric riGlass( dummy, nullRasterizerState );
				riGlass.bHit = true;
				riGlass.ptIntersection = Point3( 30, 0, -2 );
				riGlass.vNormal = N;  riGlass.vGeomNormal = N;
				riGlass.ptCoord = Point2( 0, 0 );
				riGlass.onb.CreateFromW( N );

				// CLEAR reference point (no glass in its +Z shadow path).
				RayIntersectionGeometric riClear( dummy, nullRasterizerState );
				riClear.bHit = true;
				riClear.ptIntersection = Point3( 50, 0, -2 );
				riClear.vNormal = N;  riClear.vGeomNormal = N;
				riClear.ptCoord = Point2( 0, 0 );
				riClear.onb.CreateFromW( N );

				RISEPel amtOff( 0, 0, 0 ), amtOn( 0, 0, 0 ), amtClear( 0, 0, 0 );

				pCaster->SetTransparentShadows( false );
				pDir->ComputeDirectLighting( riGlass, *pICaster, *pBSDF, true, amtOff );

				pCaster->SetTransparentShadows( true );
				pDir->ComputeDirectLighting( riGlass, *pICaster, *pBSDF, true, amtOn );
				pDir->ComputeDirectLighting( riClear, *pICaster, *pBSDF, true, amtClear );

				Check( amtOff.r <= 1.0e-6, "(8) FALSE: directional fully blocked by glass (amount==0)" );
				Check( amtOn.r  > 1.0e-6,  "(8) TRUE: directional transmits through glass (amount>0)" );
				Check( amtClear.r > 1.0e-6, "(8) clear reference is lit" );
				// ON is the clear contribution attenuated by the slab's (1-F)^2.
				if( amtClear.r > 1.0e-6 ) {
					CheckClose( amtOn.r / amtClear.r, slabT, 0.03, "(8) TRUE attenuation ~ (1-F)^2" );
				}

				// NM (spectral) path: ComputeDirectLightingNM must honor the
				// flag too -- the production watch uses the SPECTRAL rasterizer,
				// which routes the directional light through ComputeDirectLighting
				// NM, NOT the RGB path.  A broken/omitted NM fix would leave the
				// watch dial dark while the RGB assertions above still pass.
				const Scalar nm = 550.0;
				pCaster->SetTransparentShadows( false );
				const Scalar nmOff   = pDir->ComputeDirectLightingNM( riGlass, *pICaster, *pBSDF, true, nm );
				pCaster->SetTransparentShadows( true );
				const Scalar nmOn    = pDir->ComputeDirectLightingNM( riGlass, *pICaster, *pBSDF, true, nm );
				const Scalar nmClear = pDir->ComputeDirectLightingNM( riClear, *pICaster, *pBSDF, true, nm );
				Check( nmOff <= 1.0e-6, "(8) NM FALSE: directional fully blocked by glass" );
				Check( nmOn  > 1.0e-6,  "(8) NM TRUE: directional transmits through glass" );
				Check( nmClear > 1.0e-6, "(8) NM clear reference is lit" );
				if( nmClear > 1.0e-6 ) {
					CheckClose( nmOn / nmClear, slabT, 0.03, "(8) NM TRUE attenuation ~ (1-F)^2" );
				}
			}
		}

		// ---- (9) POSITIONAL lights (omni, spot) honor transparent_shadows in
		// the NM/spectral path with WAVELENGTH-SPECIFIC Fresnel.  PointLight and
		// SpotLight override ComputeDirectLightingNM (rather than inheriting
		// ILight's RGB-projection fallback, which would attenuate by a single
		// representative IOR), so through a DISPERSIVE (sellmeier sapphire) glass
		// the attenuation MUST differ between 450nm and 650nm.  The RGB fallback
		// would give a wavelength-independent attenuation and fail (9c)/(9f).
		{
			IMaterialManager* pMatMgr = pJob->GetMaterials();
			IMaterial* pMatO = pMatMgr ? pMatMgr->GetItem( "mat_opaque" ) : 0;
			IBSDF* pB = pMatO ? pMatO->GetBSDF() : 0;
			const ILightManager* pLM = pScene->GetLights();

			const ILightPriv* pPoint = 0;
			const ILightPriv* pSpot  = 0;
			if( pLM ) {
				const ILightManager::LightsList& ls = pLM->getLights();
				for( ILightManager::LightsList::const_iterator li = ls.begin(); li != ls.end(); ++li ) {
					if( !(*li) ) continue;
					if( (*li)->lightType() == ILight::LightType::Point ) pPoint = *li;
					if( (*li)->lightType() == ILight::LightType::Spot  ) pSpot  = *li;
				}
			}
			Check( pPoint != 0, "(9) omni light present in scene" );
			Check( pSpot  != 0, "(9) spot light present in scene" );

			if( pB && pPoint && pSpot ) {
				// Shading point UNDER the dispersive slab @ (40,0,0); +Z shadow
				// ray to the lights at (40,0,6) crosses it.  Viewer ray -Z so
				// the Lambertian BRDF returns nonzero (see block (8)).
				const Vector3 N( 0, 0, 1 );
				const Ray viewRay( Point3( 40, 0, -2 ), Vector3( 0, 0, -1 ) );
				RayIntersectionGeometric riD( viewRay, nullRasterizerState );
				riD.bHit = true;
				riD.ptIntersection = Point3( 40, 0, -2 );
				riD.vNormal = N;  riD.vGeomNormal = N;
				riD.ptCoord = Point2( 0, 0 );
				riD.onb.CreateFromW( N );

				// --- Omni (PointLight) ---
				const double pA450 = AttenNM( pPoint, pCaster, pICaster, riD, pB, 450.0 );
				const double pA650 = AttenNM( pPoint, pCaster, pICaster, riD, pB, 650.0 );
				Check( pA450 > 0.7 && pA450 < 1.0, "(9a) omni NM atten @450 is a sane Fresnel transmittance" );
				Check( pA650 > 0.7 && pA650 < 1.0, "(9b) omni NM atten @650 is a sane Fresnel transmittance" );
				Check( std::fabs( pA450 - pA650 ) > 5.0e-4,
					"(9c) omni NM Fresnel is WAVELENGTH-SPECIFIC (450 != 650; RGB fallback would tie)" );
				pCaster->SetTransparentShadows( false );
				Check( pPoint->ComputeDirectLightingNM( riD, *pICaster, *pB, true, 550.0 ) <= 1.0e-6,
					"(9d) omni NM FALSE: fully blocked by glass" );

				// --- Spot (SpotLight, point is on the cone axis -> full power) ---
				const double sA450 = AttenNM( pSpot, pCaster, pICaster, riD, pB, 450.0 );
				const double sA650 = AttenNM( pSpot, pCaster, pICaster, riD, pB, 650.0 );
				Check( sA450 > 0.7 && sA450 < 1.0, "(9e) spot NM atten @450 is a sane Fresnel transmittance" );
				Check( std::fabs( sA450 - sA650 ) > 5.0e-4,
					"(9f) spot NM Fresnel is WAVELENGTH-SPECIFIC (450 != 650; RGB fallback would tie)" );
				pCaster->SetTransparentShadows( false );
			}
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
