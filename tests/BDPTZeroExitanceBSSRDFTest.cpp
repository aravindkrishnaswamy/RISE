//////////////////////////////////////////////////////////////////////
//
//  BDPTZeroExitanceBSSRDFTest.cpp - Regression guard for DL-207:
//    BDPTIntegrator.cpp's deterministic zero-exitance-light sweep
//    (`EvaluateAllStrategiesImpl`, the block that hand-delivers
//    directional/ambient light -- lights with radiantExitance()==0,
//    excluded from the light-selection alias table -- to every eye
//    vertex) evaluated each eligible vertex through the material's
//    RAW AGGREGATE `IBSDF` via `ILight::ComputeDirectLighting`, never
//    through `PathVertexEval::EvalBSDFAtVertex` (the only site that
//    handles `isBSSRDFEntry`).  `SubSurfaceScatteringBSDF::value()`
//    returns 0 for every direction pair off its own narrow front-
//    reflection lobe by design (its header comment: "Subsurface
//    transport is handled by the diffusion profile, not the BSDF
//    connection") -- and at a BSSRDF entry vertex the position/normal
//    are the diffusion-profile ENTRY point (sampled by
//    BSSRDFSampling::SampleEntryPoint / RandomWalkSSS::SampleExit
//    inside the eye subpath's own BSSRDF-sampling block), not the
//    camera-visible EXIT point that reflection lobe is defined at, so
//    the raw-aggregate call was pricing the wrong physical quantity
//    at the wrong surface location -- not merely evaluating a
//    legitimate lobe at zero.  Net effect: BDPT (and MLT, which
//    constructs a `BDPTIntegrator`) delivered EXACTLY ZERO direct
//    light from a directional or ambient light to any
//    `subsurfacescattering_material` / `randomwalk_sss_material`
//    vertex, front face included.
//
//  THE FIX
//
//    The sweep now detects `eyeEnd.isBSSRDFEntry` and prices it
//    through the SAME stack-local `IBSDF` adapters
//    `PathTracingIntegrator.cpp`'s own BSSRDF-entry NEE already uses
//    (`RISE::BSSRDFAdapters::BSSRDFEntryBSDF` /
//    `RandomWalkEntryBSDF`, BSSRDFEntryAdapters.h) -- Sw(direction) =
//    Ft(cosTheta) / (c*PI), the same formula
//    `PathVertexEval::EvalBSDFAtVertex`'s own `isBSSRDFEntry` branch
//    evaluates for the general (s>=2) connection strategies.  This
//    deliberately BYPASSES the general `isConnectible` gate for a
//    BSSRDF entry vertex: a random-walk SSS entry is marked
//    `isConnectible = false` so that the GENERAL connection
//    strategies -- which divide by a real area-measure
//    `pdfFwd`/`pdfRev` for MIS -- never target a vertex whose
//    `pdfSurface` is only a placeholder (see that vertex's own
//    construction comment in BDPTIntegrator.cpp).  THIS sweep's MIS
//    weight is unconditionally 1.0 for every zero-exitance light (a
//    zero-exitance light is invisible to every other (s,t) strategy,
//    so there is nothing to partition against), so no pdf consistency
//    is needed and the exemption is safe.
//
//  WHAT EACH PART OF THIS FILE PROVES
//
//    All parts are RENDER-LEVEL, END-TO-END money tests (construction
//    API via `RISE_CreateJobPriv`+`LoadAsciiSceneViaCst`, the same
//    idiom `tests/BSSRDFOpenSheetEntryTest.cpp` Part B uses).  Parts
//    A-C use a flat, DOUBLE-sided `clippedplane_geometry` quad viewed
//    head-on -- double-sided (matching BSSRDFOpenSheetEntryTest's own
//    convention) rather than a winding order chosen to face the
//    camera, because `ClippedPlaneGeometry`'s single-sided path reports
//    the FIXED authored winding normal regardless of which side the
//    ray struck, while its double-sided path always reports the
//    RAY-FACING normal -- and the BSSRDF entry gate needs a SIGNED
//    cosine (confirmed empirically: a single-sided quad whose authored
//    winding happens to face away from the camera reads EXACTLY 0 for
//    the SSS material under PT itself, a scene-authoring trap, not a
//    BDPT question).  Radiance is position-independent across the flat
//    quad (no falloff, no occlusion, no indirect bounce), so PT's own
//    result has very low variance and is the reference every other
//    integrator is measured against.  Part D uses a sphere instead --
//    see `SphereObject()`'s own comment for why.
//
//    PART A (control).  A `lambertian_material` floor under a
//    `directional_light`: PT vs BDPT must match near-exactly, BEFORE
//    and AFTER the fix -- this is not a BSSRDF vertex, so DL-207's fix
//    changes nothing here, and BDPT's directional-light NEE was never
//    broken in general (only its BSSRDF-entry pricing was).
//
//    PART B.  A `subsurfacescattering_material` slab under a
//    `directional_light`: PT vs BDPT vs `mlt_rasterizer` (MLT
//    constructs a `BDPTIntegrator` internally, so it shares the exact
//    defect and fix).  Pre-fix BDPT/MLT read (money number) 0.0
//    exactly; post-fix both read within 15% of PT (measured agreement
//    across repeated runs is much tighter, well under 1%; the wider
//    band absorbs run-to-run MC noise -- RISE renders are not bit-
//    deterministic run to run, see rise-render-seeding notes).
//
//    PART C.  The same slab under an `ambient_light` -- the sweep's
//    OTHER zero-exitance light type, gated by the identical
//    `radiantExitance() == 0` test and the identical isBSSRDFEntry
//    branch.
//
//    PART D.  A `randomwalk_sss_material` twin of Part B (on a sphere,
//    not the flat quad -- see `SphereObject()`).  This exercises the
//    SECOND, `isConnectible == false` branch of the fix (the
//    diffusion-profile case of Part B/C is `isConnectible == true`
//    already, so it alone would not prove the isConnectible bypass is
//    load-bearing).
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `dl207`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Shaders/BSSRDFEntryAdapters.h"
#include "../src/Library/Materials/BurleyNormalizedDiffusionProfile.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAILED: " << testName << std::endl;
	}
}

namespace
{
	class CapturingRasterizerOutput
		: public virtual IRasterizerOutput
		, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
		unsigned int width;
		unsigned int height;

		CapturingRasterizerOutput() : width(0), height(0) {}

	protected:
		virtual ~CapturingRasterizerOutput() {}

	public:
		virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

		virtual void OutputImage(
			const IRasterImage& pImage,
			const Rect*,
			const unsigned int ) override
		{
			width = pImage.GetWidth();
			height = pImage.GetHeight();
			pixels.resize( width * height );
			for( unsigned int y = 0; y < height; y++ ) {
				for( unsigned int x = 0; x < width; x++ ) {
					pixels[y * width + x] = pImage.GetPEL( x, y );
				}
			}
		}
	};

	struct RenderResult
	{
		double mean[3];
		bool   valid;
	};

	std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
	{
		char path[512];
		std::snprintf( path, sizeof(path),
			"/tmp/dl207_zeroexit_%s_%d.RISEscene",
			tag, static_cast<int>(::getpid()) );
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return std::string();
		ofs << sceneText;
		ofs.close();
		return std::string( path );
	}

	RenderResult RenderAndComputeMean( const std::string& scenePath, unsigned int seed )
	{
		RenderResult result{ {0,0,0}, false };

		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return result;

		if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
			safe_release( pJob );
			return result;
		}

		pJob->RemoveRasterizerOutputs();

		CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
		pJob->GetRasterizer()->AddRasterizerOutput( pCap );

		std::srand( seed );
		const bool bRendered = pJob->Rasterize();
		if( !bRendered || pCap->pixels.empty() ) {
			safe_release( pCap );
			safe_release( pJob );
			return result;
		}

		bool allFinite = true;
		double sum[3] = {0,0,0};
		for( const RISEColor& c : pCap->pixels ) {
			const double cov = c.a;
			const double r = c.base.r * cov, g = c.base.g * cov, b = c.base.b * cov;
			if( !std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) ) { allFinite = false; break; }
			sum[0] += r; sum[1] += g; sum[2] += b;
		}
		if( allFinite ) {
			const double n = double( pCap->pixels.size() );
			result.mean[0] = sum[0] / n;
			result.mean[1] = sum[1] / n;
			result.mean[2] = sum[2] / n;
			result.valid = true;
		}

		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	double Luma( const double m[3] ) { return m[0] + m[1] + m[2]; }

	//! film + camera: a flat 4x4 quad at z=0 filling most of a 16x16
	//! frame, viewed head-on from +Z.
	std::string FilmAndCamera()
	{
		std::string s;
		s += "film\n{\n\twidth 16\n\theight 16\n}\n\n";
		s += "pinhole_camera\n{\n\tlocation 0 0 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 28.0\n}\n\n";
		return s;
	}

	//! A directional light shining FROM +Z (matches the camera side --
	//! see docs/SCENE_CONVENTIONS.md: `direction` points FROM the
	//! surface TO the light).
	std::string DirectionalLightBlock()
	{
		return "directional_light\n{\n\tname l_dir\n\tdirection 0 0 1\n\tpower 3.0\n\tcolor 1.0 1.0 1.0\n}\n\n";
	}

	std::string AmbientLightBlock()
	{
		return "ambient_light\n{\n\tname l_amb\n\tpower 1.0\n\tcolor 0.5 0.5 0.5\n}\n\n";
	}

	//! The flat quad + its owning object, bound to whatever material
	//! chunk `matName` names.  `doublesided TRUE` (matching
	//! tests/BSSRDFOpenSheetEntryTest.cpp's own convention) rather than
	//! authoring a winding order that happens to face the camera:
	//! `ClippedPlaneGeometry`'s DOUBLE-sided path always reports the
	//! RAY-FACING normal (`bGeomNormalOrientedToRay`), while its
	//! SINGLE-sided path reports the fixed authored winding normal
	//! regardless of which side the ray struck from.  The BSSRDF entry
	//! gate (`BSSRDFEntryFacing`) needs a SIGNED cosine and rejects a
	//! camera-facing hit outright when the authored winding happens to
	//! point away from the camera -- confirmed empirically (single-sided
	//! with this file's winding: PT reads EXACTLY 0 for an SSS material
	//! under either a directional or omni light, while a plain
	//! Lambertian control on the identical single-sided geometry is
	//! unaffected, since ordinary diffuse shading uses an unsigned
	//! cosine).  Double-sided sidesteps the whole question.
	std::string QuadObject( const char* matName )
	{
		std::string s;
		s += "clippedplane_geometry\n{\n\tname quad_geo\n";
		s += "\tpta -2 -2 0\n\tptb 2 -2 0\n\tptc 2 2 0\n\tptd -2 2 0\n";
		s += "\tdoublesided TRUE\n}\n\n";
		s += std::string("standard_object\n{\n\tname quad_obj\n\tgeometry quad_geo\n\tmaterial ") + matName + "\n}\n\n";
		return s;
	}

	std::string LambertianMaterialBlock()
	{
		std::string s;
		s += "uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.8 0.8 0.8\n}\n\n";
		s += "lambertian_material\n{\n\tname mat_lambert\n\treflectance pnt_albedo\n}\n\n";
		return s;
	}

	std::string SSSMaterialBlock()
	{
		// Documented physically-reasonable defaults: ior 1.3,
		// absorption 0.1, scattering 1.0, g 0, roughness 0.
		return "subsurfacescattering_material\n{\n\tname mat_sss\n}\n\n";
	}

	std::string RandomWalkMaterialBlock()
	{
		// Same defaults (ior 1.3, absorption 0.1, scattering 1.0).
		return "randomwalk_sss_material\n{\n\tname mat_rw\n}\n\n";
	}

	//! A genuine 3-D solid (a sphere), for `randomwalk_sss_material`
	//! ONLY.  `RandomWalkSSS::SampleExit` walks a real path THROUGH the
	//! medium and needs an enclosing volume to walk inside of; the flat,
	//! zero-thickness `clippedplane_geometry` `QuadObject()` above has
	//! no interior at all.  Confirmed empirically: PT itself (not a
	//! BDPT/DL-207 question) reads EXACTLY 0 for `randomwalk_sss_material`
	//! on that flat quad under EITHER a directional or an omni light,
	//! while the identical material on a sphere is non-trivially bright
	//! under either.  `subsurfacescattering_material`'s diffusion-profile
	//! approach has no such requirement (a local surface probe, not a
	//! volumetric walk), which is why Parts B/C keep the flat quad.
	std::string SphereObject( const char* matName, double radius )
	{
		std::ostringstream ss;
		ss << "sphere_geometry\n{\n\tname sph_geo\n\tradius " << radius << "\n}\n\n";
		ss << "standard_object\n{\n\tname sph_obj\n\tgeometry sph_geo\n\tmaterial " << matName << "\n}\n\n";
		return ss.str();
	}

	const char* kShader =
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

	std::string RasterizerPT( unsigned int samples )
	{
		std::ostringstream ss;
		ss << kShader
			<< "pathtracing_pel_rasterizer\n{\n\tsamples " << samples
			<< "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
			<< "file_rasterizeroutput\n{\n\tpattern rendered/dl207_pt_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		return ss.str();
	}

	std::string RasterizerBDPT( unsigned int samples )
	{
		std::ostringstream ss;
		ss << kShader
			<< "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples " << samples
			<< "\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
			<< "file_rasterizeroutput\n{\n\tpattern rendered/dl207_bdpt_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		return ss.str();
	}

	std::string RasterizerMLT( unsigned int bootstrap, unsigned int chains, unsigned int mutationsPerPixel )
	{
		std::ostringstream ss;
		ss << kShader
			<< "mlt_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tbootstrap_samples " << bootstrap
			<< "\n\tchains " << chains << "\n\tmutations_per_pixel " << mutationsPerPixel
			<< "\n\tlarge_step_prob 0.3\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
			<< "file_rasterizeroutput\n{\n\tpattern rendered/dl207_mlt_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		return ss.str();
	}
}

//////////////////////////////////////////////////////////////////////
// PART A -- Lambertian control: PT vs BDPT must agree near-exactly,
// unaffected by the fix either way (not a BSSRDF vertex).
//////////////////////////////////////////////////////////////////////
static void TestLambertianControl()
{
	std::cout << "Part A: Lambertian floor + directional light, PT vs BDPT (control)" << std::endl;

	const std::string common = FilmAndCamera() + DirectionalLightBlock() + LambertianMaterialBlock() + QuadObject( "mat_lambert" );

	const std::string ptScene   = std::string("RISE ASCII SCENE 7\n") + RasterizerPT( 64 )   + common;
	const std::string bdptScene = std::string("RISE ASCII SCENE 7\n") + RasterizerBDPT( 64 ) + common;

	const std::string ptPath   = WriteSceneToTempFile( ptScene,   "a_pt" );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene, "a_bdpt" );
	Check( !ptPath.empty() && !bdptPath.empty(), "A: scene temp files written" );

	const RenderResult pt   = RenderAndComputeMean( ptPath,   5001 );
	const RenderResult bdpt = RenderAndComputeMean( bdptPath, 5002 );
	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );

	Check( pt.valid,   "A: PT render produced a finite image" );
	Check( bdpt.valid, "A: BDPT render produced a finite image" );
	if( !pt.valid || !bdpt.valid ) return;

	const double ptLuma   = Luma( pt.mean );
	const double bdptLuma = Luma( bdpt.mean );
	std::cout << "    PT luma=" << ptLuma << "  BDPT luma=" << bdptLuma
		<< "  ratio(BDPT/PT)=" << (bdptLuma / ptLuma) << std::endl;

	Check( ptLuma > 1e-3, "A: (sanity) the lit floor is non-trivially bright" );
	Check( std::fabs( bdptLuma - ptLuma ) < 0.02 * ptLuma,
		"A MONEY: BDPT's directional-light zero-exitance sweep matches PT to within 2% at an ordinary (non-BSSRDF) vertex -- general directional-light NEE was never broken, only BSSRDF-entry pricing was" );
}

//////////////////////////////////////////////////////////////////////
// PART B -- subsurfacescattering_material + directional_light:
// PT vs BDPT vs MLT.
//////////////////////////////////////////////////////////////////////
static void TestSSSDirectional()
{
	std::cout << "Part B: subsurfacescattering_material slab + directional light, PT vs BDPT vs MLT" << std::endl;

	const std::string common = FilmAndCamera() + DirectionalLightBlock() + SSSMaterialBlock() + QuadObject( "mat_sss" );

	const std::string ptScene   = std::string("RISE ASCII SCENE 7\n") + RasterizerPT( 512 )   + common;
	const std::string bdptScene = std::string("RISE ASCII SCENE 7\n") + RasterizerBDPT( 512 ) + common;
	const std::string mltScene  = std::string("RISE ASCII SCENE 7\n") + RasterizerMLT( 20000, 128, 64 ) + common;

	const std::string ptPath   = WriteSceneToTempFile( ptScene,   "b_pt" );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene, "b_bdpt" );
	const std::string mltPath  = WriteSceneToTempFile( mltScene,  "b_mlt" );
	Check( !ptPath.empty() && !bdptPath.empty() && !mltPath.empty(), "B: scene temp files written" );

	const RenderResult pt   = RenderAndComputeMean( ptPath,   5101 );
	const RenderResult bdpt = RenderAndComputeMean( bdptPath, 5102 );
	const RenderResult mlt  = RenderAndComputeMean( mltPath,  5103 );
	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );
	std::remove( mltPath.c_str() );

	Check( pt.valid,   "B: PT render produced a finite image" );
	Check( bdpt.valid, "B: BDPT render produced a finite image" );
	Check( mlt.valid,  "B: MLT render produced a finite image" );

	const double ptLuma   = pt.valid   ? Luma( pt.mean )   : -1.0;
	const double bdptLuma = bdpt.valid ? Luma( bdpt.mean ) : -1.0;
	const double mltLuma  = mlt.valid  ? Luma( mlt.mean )  : -1.0;
	std::cout << "    PT luma=" << ptLuma << "  BDPT luma=" << bdptLuma
		<< "  MLT luma=" << mltLuma;
	if( pt.valid ) {
		std::cout << "  ratio(BDPT/PT)=" << (bdptLuma / std::max(ptLuma,1e-12))
			<< "  ratio(MLT/PT)=" << (mltLuma / std::max(ptLuma,1e-12));
	}
	std::cout << std::endl;

	if( !pt.valid ) return;
	Check( ptLuma > 1e-3, "B: (sanity) PT's directly-lit BSSRDF slab is non-trivially bright" );

	if( bdpt.valid ) {
		Check( bdptLuma > 0.85 * ptLuma,
			"B MONEY: BDPT delivers non-negligible direct light to a directional-lit subsurfacescattering_material vertex (pre-fix this read EXACTLY 0.0)" );
		Check( bdptLuma < 1.15 * ptLuma,
			"B: BDPT's mean stays within 15% of PT's (no gross over-count)" );
	}

	if( mlt.valid ) {
		Check( mltLuma > 0.85 * ptLuma,
			"B MONEY: MLT (constructs a BDPTIntegrator) shares the same fix -- non-negligible direct light delivered (pre-fix EXACTLY 0.0)" );
		Check( mltLuma < 1.15 * ptLuma,
			"B: MLT's mean stays within 15% of PT's" );
	}
}

//////////////////////////////////////////////////////////////////////
// PART C -- subsurfacescattering_material + ambient_light: BDPT.
//////////////////////////////////////////////////////////////////////
static void TestSSSAmbient()
{
	std::cout << "Part C: subsurfacescattering_material slab + ambient light, PT vs BDPT" << std::endl;

	const std::string common = FilmAndCamera() + AmbientLightBlock() + SSSMaterialBlock() + QuadObject( "mat_sss" );

	const std::string ptScene   = std::string("RISE ASCII SCENE 7\n") + RasterizerPT( 512 )   + common;
	const std::string bdptScene = std::string("RISE ASCII SCENE 7\n") + RasterizerBDPT( 512 ) + common;

	const std::string ptPath   = WriteSceneToTempFile( ptScene,   "c_pt" );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene, "c_bdpt" );
	Check( !ptPath.empty() && !bdptPath.empty(), "C: scene temp files written" );

	const RenderResult pt   = RenderAndComputeMean( ptPath,   5201 );
	const RenderResult bdpt = RenderAndComputeMean( bdptPath, 5202 );
	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );

	Check( pt.valid,   "C: PT render produced a finite image" );
	Check( bdpt.valid, "C: BDPT render produced a finite image" );
	if( !pt.valid || !bdpt.valid ) return;

	const double ptLuma   = Luma( pt.mean );
	const double bdptLuma = Luma( bdpt.mean );
	std::cout << "    PT luma=" << ptLuma << "  BDPT luma=" << bdptLuma
		<< "  ratio(BDPT/PT)=" << (bdptLuma / std::max(ptLuma,1e-12)) << std::endl;

	Check( ptLuma > 1e-3, "C: (sanity) PT's ambient-lit BSSRDF slab is non-trivially bright" );
	Check( bdptLuma > 0.85 * ptLuma,
		"C MONEY: BDPT delivers non-negligible direct light from an ambient_light to a subsurfacescattering_material vertex (pre-fix EXACTLY 0.0)" );
	Check( bdptLuma < 1.15 * ptLuma,
		"C: BDPT's mean stays within 15% of PT's" );
}

//////////////////////////////////////////////////////////////////////
// PART D -- randomwalk_sss_material twin of Part B.  This exercises
// the `isConnectible == false` branch of the fix specifically (the
// diffusion-profile vertex in B/C is already isConnectible == true).
//////////////////////////////////////////////////////////////////////
static void TestRandomWalkDirectional()
{
	std::cout << "Part D: randomwalk_sss_material slab + directional light, PT vs BDPT vs MLT" << std::endl;

	const std::string common = FilmAndCamera() + DirectionalLightBlock() + RandomWalkMaterialBlock() + SphereObject( "mat_rw", 1.5 );

	const std::string ptScene   = std::string("RISE ASCII SCENE 7\n") + RasterizerPT( 512 )   + common;
	const std::string bdptScene = std::string("RISE ASCII SCENE 7\n") + RasterizerBDPT( 512 ) + common;
	const std::string mltScene  = std::string("RISE ASCII SCENE 7\n") + RasterizerMLT( 20000, 128, 64 ) + common;

	const std::string ptPath   = WriteSceneToTempFile( ptScene,   "d_pt" );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene, "d_bdpt" );
	const std::string mltPath  = WriteSceneToTempFile( mltScene,  "d_mlt" );
	Check( !ptPath.empty() && !bdptPath.empty() && !mltPath.empty(), "D: scene temp files written" );

	const RenderResult pt   = RenderAndComputeMean( ptPath,   5301 );
	const RenderResult bdpt = RenderAndComputeMean( bdptPath, 5302 );
	const RenderResult mlt  = RenderAndComputeMean( mltPath,  5303 );
	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );
	std::remove( mltPath.c_str() );

	Check( pt.valid,   "D: PT render produced a finite image" );
	Check( bdpt.valid, "D: BDPT render produced a finite image" );
	Check( mlt.valid,  "D: MLT render produced a finite image" );

	const double ptLuma   = pt.valid   ? Luma( pt.mean )   : -1.0;
	const double bdptLuma = bdpt.valid ? Luma( bdpt.mean ) : -1.0;
	const double mltLuma  = mlt.valid  ? Luma( mlt.mean )  : -1.0;
	std::cout << "    PT luma=" << ptLuma << "  BDPT luma=" << bdptLuma
		<< "  MLT luma=" << mltLuma;
	if( pt.valid ) {
		std::cout << "  ratio(BDPT/PT)=" << (bdptLuma / std::max(ptLuma,1e-12))
			<< "  ratio(MLT/PT)=" << (mltLuma / std::max(ptLuma,1e-12));
	}
	std::cout << std::endl;

	if( !pt.valid ) return;
	Check( ptLuma > 1e-3, "D: (sanity) PT's directly-lit random-walk SSS slab is non-trivially bright" );

	if( bdpt.valid ) {
		Check( bdptLuma > 0.85 * ptLuma,
			"D MONEY: BDPT delivers non-negligible direct light to a directional-lit randomwalk_sss_material vertex, an isConnectible==false BSSRDF entry (pre-fix EXACTLY 0.0, doubly so: wrong BSDF AND excluded by the isConnectible gate)" );
		Check( bdptLuma < 1.15 * ptLuma,
			"D: BDPT's mean stays within 15% of PT's" );
	}

	if( mlt.valid ) {
		Check( mltLuma > 0.85 * ptLuma,
			"D MONEY: MLT shares the same fix for randomwalk_sss_material (pre-fix EXACTLY 0.0)" );
		Check( mltLuma < 1.15 * ptLuma,
			"D: MLT's mean stays within 15% of PT's" );
	}
}

// DL224: both adapter families use fixed outward support, including every
// spectral/HWSS lane. A diffusion chord can point inward and must not orient it.
static void TestEntryEvaluationFrame()
{
	using namespace BSSRDFAdapters;
	UniformScalarPainter* eta = new UniformScalarPainter( 1.3 );
	UniformScalarPainter* absorption = new UniformScalarPainter( 0.1 );
	UniformScalarPainter* scattering = new UniformScalarPainter( 1.0 );
	BurleyNormalizedDiffusionProfile* profile =
		new BurleyNormalizedDiffusionProfile( *eta, *absorption, *scattering, 0.0 );
	BSSRDFEntryBSDF diffusion( profile, 0.0 );
	RandomWalkEntryBSDF randomWalk( 1.3 );
	const IBSDF* adapters[] = { &diffusion, &randomWalk };
	const Vector3 normal( 0, 0, 1 );
	RayIntersectionGeometric ri( EntryEvaluationRay( Point3(0,0,0), normal ), nullRasterizerState );
	ri.vNormal = ri.vGeomNormal = normal;
	Check( Vector3Ops::Dot( ri.ray.Dir(), normal ) == -1.0,
		"entry evaluation ray is incoming through fixed outward hemisphere" );
	Check( ri.RayFacingShadingCosine( normal ) == 1.0 &&
		ri.RayFacingShadingCosine( -normal ) == -1.0,
		"entry frame keeps outward light and rejects inward light" );
	for( const IBSDF* adapter : adapters ) {
		const Scalar pel = adapter->value( normal, ri )[0];
		Check( pel > 0 && adapter->value( -normal, ri )[0] == 0,
			"diffusion/random-walk Pel adapter has fixed outward support" );
		for( Scalar nm : { Scalar(400), Scalar(500), Scalar(600), Scalar(700) } ) {
			Check( fabs( adapter->valueNM( normal, ri, nm ) - pel ) < 1e-14 &&
				adapter->valueNM( -normal, ri, nm ) == 0,
				"NM/HWSS companion entry support matches Pel at every lane" );
		}
	}
	profile->release();
	eta->release(); absorption->release(); scattering->release();
}

int main()
{
	std::cout << "=== BDPTZeroExitanceBSSRDFTest (DL-207) ===" << std::endl;

	TestEntryEvaluationFrame();
	TestLambertianControl();
	TestSSSDirectional();
	TestSSSAmbient();
	TestRandomWalkDirectional();

	std::cout << "\nPassed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
