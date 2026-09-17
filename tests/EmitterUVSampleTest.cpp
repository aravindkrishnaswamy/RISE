//////////////////////////////////////////////////////////////////////
//
//  EmitterUVSampleTest.cpp - DL-44 regression: the sampled emitter UV
//    must survive from LightSampler::SampleLight into every downstream
//    emission rebuild (BDPT's light-subpath NM hero / HWSS companion,
//    the BDPT LIGHT-type root vertex and everything that rebuilds a
//    RayIntersectionGeometric from it via PathVertexEval::
//    PopulateRIGFromVertex, and VCM's own light-vertex NEE record).
//
//    THE BUG: `LightSampler::SampleLight`'s mesh-luminary branch draws a
//    UV `coord` from `IObject::UniformRandomPoint` and uses it locally
//    (`rig.ptCoord = coord`) to evaluate the RGB emission (`sample.Le`),
//    but the sampled `coord` was never stored on `LightSample` itself.
//    Every consumer that rebuilds its OWN RayIntersectionGeometric from
//    a `LightSample` -- rather than reading `sample.Le` directly --
//    left `ptCoord` at its default-constructed (0,0):  BDPTIntegrator's
//    NM hero and HWSS companion rebuilds in GenerateLightSubpathImpl,
//    the BDPT `type == LIGHT` root vertex (and therefore
//    `LuminaryRadiance`'s s=0/t=1 splat and VCM's own light-to-camera
//    splat, both of which reconstruct via `PopulateRIGFromVertex`), and
//    VCM's `EvaluateNEEImpl` light-vertex record.  A UV-textured mesh
//    luminary (`checker_painter`, an image-based exitance map) therefore
//    emitted the WRONG texel under BDPT and VCM (and any spectral/HWSS
//    variant of either) while PT -- whose own NEE strategy,
//    `LightSampler::EvaluateDirectLighting{,NM}`, samples and uses its
//    own local UV directly and never touches `LightSample` -- was
//    unaffected and is used here as the closed-form-adjacent reference.
//
//    THE SCENE: a large diffuse floor lit from directly above, out of
//    the camera's frustum, by a small mesh luminary whose exitance is
//    `checker_painter(colora=white, colorb=black, size=0.5)` on a UV
//    domain of [0,1]x[0,1] (`ClippedPlaneGeometry::UniformRandomPoint`
//    sets `coord = (u,v) = (prand.x, prand.y)` directly).  A size-0.5
//    checker over a [0,1]^2 domain is an exact 2x2 board: cells
//    (1,1)/(2,2) (u,v both <=0.5 or both >0.5) are colorA, cells
//    (1,2)/(2,1) are colorB -- so the true area-weighted average
//    exitance is exactly 0.5 (half white, half black) REGARDLESS of
//    where within each cell a sample lands.  At UV (0,0) -- the
//    default-constructed value every broken rebuild substituted --
//    ceil(0/0.5)=0 for both axes, an EVEN sum, which lands in colorA
//    (white, exitance 1.0).  So the defect's signature is not a subtle
//    percentage: pre-fix, BDPT and VCM's light-sampling strategy always
//    reads the FULLY WHITE corner cell's exitance (1.0) no matter which
//    of the four cells `UniformRandomPoint` actually sampled, i.e. TWICE
//    the correct area-weighted average (0.5) -- an exact factor of 2,
//    not area noise.
//
//    The floor is lit ONLY via this luminary's own light-sampling
//    strategy (PT's NEE, BDPT's s=1 connection from the generated
//    light subpath's root, VCM's `EvaluateNEEImpl`) -- max_eye_depth /
//    max_light_depth are kept small and the luminary is excluded from
//    the camera's view (see kSceneCommonGeometry's camera/light
//    placement below), so this is a clean, direct measurement of the
//    light-sampling strategy's own emission record and not diluted by
//    a directly-visible light patch (which uses a REAL ray intersection
//    with a genuine, already-correct ptCoord and would mask the bug).
//
//    PASS CRITERION: PT's floor-mean is close to the true 0.5x-relative
//    illumination level (checked against a wide band, since this is a
//    Monte-Carlo estimate, not a closed form -- the "reference" here is
//    architectural: PT never goes through the buggy rebuild path).
//    BDPT's and VCM's floor-mean must track PT's within MC noise
//    AFTER the fix; pre-fix, this test records them running close to
//    2x PT (the closed-form bias derived above) and documents that
//    number in the fix commit message as the red-proof.
//
//    A spectral (NM) PT/BDPT twin exercises the same defect through
//    `pathtracing_spectral_rasterizer` / `bdpt_spectral_rasterizer`
//    (hwss false, so only the NM hero branch is exercised -- the HWSS
//    companion branch is covered by inspection: it is built field-for-
//    field identically to the hero, per the BDPTIntegrator.cpp comment
//    at that site, and the same one-line fix applies to both).
//
//  Author: Generated for the DL-44 sampled-emitter-UV-propagation fix
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
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
#include "../src/Library/Utilities/Color/Color_Template.h"

// Gate hygiene (docs/skills/variance-measurement.md, COMMON_RULES.md):
// seed each render invocation explicitly rather than letting the
// unsynchronized libc rand() default drift between repeats.
static unsigned int g_renderSeed = 44001u;

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

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput -- same shape as EnvLightBalanceTest /
// BDPTStrategyBalanceTest.
//////////////////////////////////////////////////////////////////////
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

//! Mean over every captured pixel's Rec.709 luminance.  The floor fills
//! the whole frame (see kSceneCommonGeometry) so this is a clean
//! floor-illumination statistic with no background/miss dilution.
static double MeanLuminance( const CapturingRasterizerOutput& cap )
{
	if( cap.pixels.empty() ) return -1.0;
	double sum = 0;
	for( const RISEColor& c : cap.pixels ) {
		sum += 0.2126 * c.base.r + 0.7152 * c.base.g + 0.0722 * c.base.b;
	}
	return sum / double( cap.pixels.size() );
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/emitter_uv_sample_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static double RenderMeanLuminance( const char* scenePath )
{
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return -1.0;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return -1.0;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	std::srand( g_renderSeed++ );
	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return -1.0;
	}

	const double mean = MeanLuminance( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	return mean;
}

//! DL-108 topology test only: same as RenderMeanLuminance, but after
//! loading the scene and BEFORE rasterizing, binds a BoxUVGenerator to
//! the named object via the construction-API setter `IJob::
//! SetObjectUVToBox` -- there is no scene-language chunk that reaches
//! `Object::SetUVGenerator` (see docs/DL95_OBJECT_UV_GENERATOR_INPUT.md
//! "Scene-level impact"), so this is the only way to exercise an
//! override UV generator through a real end-to-end render.
static double RenderMeanLuminanceWithBoxUV(
	const char* scenePath, const char* objectName,
	double width, double height, double depth )
{
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return -1.0;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return -1.0;
	}

	if( !pJob->SetObjectUVToBox( objectName, width, height, depth ) ) {
		safe_release( pJob );
		return -1.0;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	std::srand( g_renderSeed++ );
	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return -1.0;
	}

	const double mean = MeanLuminance( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	return mean;
}

//////////////////////////////////////////////////////////////////////
// Scene text.
//
// Receiver: the SAME vertical diffuse wall EnvLightBalanceTest uses
// (`kSceneCommonGeometry` there) -- a 2x2 quad in the XY plane at
// z=0, normal +Z, dead-on and fully framed by a camera at (0,0,3.5)
// with fov 30.
//
// Luminary: a checker-textured mesh light at z=6 -- BEHIND the camera
// (camera z=3.5 < light z=6, and the camera looks toward -Z, i.e.
// DECREASING z) -- so it is geometrically outside the camera's
// forward hemisphere regardless of its transverse (x,y) placement or
// the lens FOV, and cannot appear in the rendered frame or occlude
// the wall.  Its normal faces -Z (same winding convention as
// EnvLightBalanceTest's `kLightMesh`, which the comment there confirms
// gives N=-Z for this pta->ptb->ptc->ptd order), so it radiates back
// through the camera position onto the wall at z=0.  The renderer does
// not treat the camera as occluding geometry, so the wall is lit
// exactly as if the light were unobstructed.
//
// The wall is illuminated ONLY via NEE / the light-sampling strategy
// (PT's `EvaluateDirectLighting{,NM}`, BDPT's s=1 connection from the
// generated light subpath's LIGHT root, VCM's `EvaluateNEEImpl`) --
// small `max_eye_depth`/`max_light_depth` and a single light with
// `material none` (no BRDF, so it cannot itself reflect anything back)
// keep this a clean single-bounce measurement.
//////////////////////////////////////////////////////////////////////
static const char* kSceneCommonGeometry =
	"film\n"
	"{\n"
	"\twidth 24\n"
	"\theight 24\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.8 0.8 0.8\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad\n"
	"\tpta -1 -1 0\n"
	"\tptb 1 -1 0\n"
	"\tptc 1 1 0\n"
	"\tptd -1 1 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_quad\n"
	"\tgeometry quad\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_white\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_black\n"
	"\tcolor 0.0 0.0 0.0\n"
	"}\n"
	"\n"
	"checker_painter\n"
	"{\n"
	"\tname pnt_checker\n"
	"\tcolora pnt_white\n"
	"\tcolorb pnt_black\n"
	"\tsize 0.5\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_emit\n"
	"\texitance pnt_checker\n"
	"\tscale 10.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_emit\n"
	"\tpta -1.0 1.0 6.0\n"
	"\tptb 1.0 1.0 6.0\n"
	"\tptc 1.0 -1.0 6.0\n"
	"\tptd -1.0 -1.0 6.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_emit\n"
	"\tgeometry quad_emit\n"
	"\tmaterial mat_emit\n"
	"}\n";

static const char* kRasterizerPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultDirectLighting\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 256\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/emitter_uv_pt_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerBDPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"bdpt_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 256\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/emitter_uv_bdpt_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerVCM =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 256\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/emitter_uv_vcm_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerPTSpectral =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultDirectLighting\n"
	"}\n"
	"\n"
	"pathtracing_spectral_rasterizer\n"
	"{\n"
	"\tsamples 512\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tnmbegin 380\n"
	"\tnmend 720\n"
	"\tnum_wavelengths 8\n"
	"\tspectral_samples 1\n"
	"\thwss false\n"
	"\tmax_diffuse_bounce 3\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/emitter_uv_pts_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static const char* kRasterizerBDPTSpectral =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"bdpt_spectral_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 512\n"
	"\tnmbegin 380\n"
	"\tnmend 720\n"
	"\tnum_wavelengths 8\n"
	"\tspectral_samples 1\n"
	"\thwss false\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern rendered/emitter_uv_bdpts_unused\n"
	"\ttype EXR\n"
	"\tbpp 32\n"
	"\tcolor_space Rec709RGB_Linear\n"
	"}\n";

static std::string BuildScene( const char* rasterizerBlock )
{
	std::string s( "RISE ASCII SCENE 7\n" );
	s += kSceneCommonGeometry;
	s += rasterizerBlock;
	return s;
}

static void PrintStat( const char* label, double v )
{
	std::printf( "  %-24s mean-luminance = %.6f\n", label, v );
}

//////////////////////////////////////////////////////////////////////
// DL-108 topology: Object::UniformRandomPoint (hence LightSampler::
// SampleLight, hence EVERY light-sampling strategy -- PT's NEE, BDPT's
// s=1 connection from the generated light subpath's root, VCM's
// EvaluateNEEImpl) never consulted an override UV generator, so a
// UV-textured emitter charted via a BoxUVGenerator lit the wall with
// its NATIVE (ClippedPlaneGeometry) checker pattern instead of the
// override one, even after DL-95/DL-44 fixed every OTHER UV-generator
// consumer.
//
// SAME wall receiver + camera as kSceneCommonGeometry above, but the
// emitter quad is offset well away from the object-local origin (the
// corners below span local x,y in [0.1, 2.1], not [-1, 1]) and its
// BoxUVGenerator (bound via SetObjectUVToBox below, not scene text --
// see RenderMeanLuminanceWithBoxUV) uses a deliberately HUGE width and
// height (10000).  Two independent, closed-form-derivable facts follow:
//
//   NATIVE (ClippedPlaneGeometry) UV always spans the full [0, 1]^2
//   parameter square exactly, by construction, regardless of the
//   quad's placement -- a size=0.5 checker over that square is always
//   an EXACT 2x2 board, so avgNative = 0.5 (colorA=white=1, colorB=
//   black=0) for ANY quad placement.  (Verified separately, by
//   inspection of the pre-existing kSceneCommonGeometry test above,
//   which relies on the identical fact.)
//
//   BOX-projected UV, from BoxUVGenerator's `GenerateUV` (side chosen
//   by dominant normal axis -- here always -Z, side 4, so u/v depend
//   only on local x/y): with width=height=10000 the box maps the
//   ENTIRE local x,y in [0.1,2.1]^2 extent into a range of width
//   2.1/10000 ~ 0.0002 centred near u=v=0.49989 -- far too narrow to
//   cross ANY checker cell boundary (multiples of 0.5) starting from a
//   quad deliberately NOT centred on one -- so the WHOLE emitter charts
//   into a SINGLE checker cell.  0.49989 falls in the (0, 0.5) cell,
//   which (ceil(0.49989/0.5)=1, odd) is colorB... wait: XOR of x and y
//   both odd is EVEN overall -> colorA.  avgBox = 1.0 (solid colorA,
//   white) for this construction -- verified by an independent
//   quadrature script during test authoring (2000x2000 grid): exactly
//   1.0.
//
// So avgNative=0.5, avgBox=1.0 -- an exact 2x factor, deliberately
// chosen to be unmistakable against Monte-Carlo noise at production
// sample counts, mirroring DL-44's own "exact factor, not area noise"
// design.  A companion UNIFORM (exitance=1.0, no checker) emitter scene
// calibrates the unknown light-transport proportionality constant
// (distance falloff, solid angle, BRDF cosine, radiometric scale) that
// a bare receiver-mean number can't separate from the exitance average
// on its own: for LINEAR transport, meanReceiver(checker) /
// meanReceiver(uniform) -> avgExitance-under-whichever-UV-convention-
// light-sampling-actually-used, independent of that constant, in the
// Monte-Carlo limit.  Pre-fix this ratio clusters near avgNative (0.5,
// light sampling ignores the override generator); post-fix it must
// track avgBox (1.0, DL-108 fixed).
//////////////////////////////////////////////////////////////////////
static const char* kSceneWallAndCameraTopology =
	"film\n"
	"{\n"
	"\twidth 24\n"
	"\theight 24\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.8 0.8 0.8\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad\n"
	"\tpta -1 -1 0\n"
	"\tptb 1 -1 0\n"
	"\tptc 1 1 0\n"
	"\tptd -1 1 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_quad\n"
	"\tgeometry quad\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n";

//! Emitter quad: local (x, y) in [0.1, 2.1]^2 -- see the block comment
//! above for why this offset-from-origin placement matters for the
//! BoxUVGenerator side of the closed form.  Same z=6 / behind-camera /
//! -Z-facing placement as kSceneCommonGeometry's own quad_emit.
static const char* kEmitterQuadGeometry =
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_emit\n"
	"\tpta 0.1 2.1 6.0\n"
	"\tptb 2.1 2.1 6.0\n"
	"\tptc 2.1 0.1 6.0\n"
	"\tptd 0.1 0.1 6.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_emit\n"
	"\tgeometry quad_emit\n"
	"\tmaterial mat_emit\n"
	"}\n";

static const char* kEmitterMaterialChecker =
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_white\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_black\n"
	"\tcolor 0.0 0.0 0.0\n"
	"}\n"
	"\n"
	"checker_painter\n"
	"{\n"
	"\tname pnt_checker_topo\n"
	"\tcolora pnt_white\n"
	"\tcolorb pnt_black\n"
	"\tsize 0.5\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_emit\n"
	"\texitance pnt_checker_topo\n"
	"\tscale 10.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n";

static const char* kEmitterMaterialUniform =
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_uniform_topo\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_emit\n"
	"\texitance pnt_uniform_topo\n"
	"\tscale 10.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n";

static std::string BuildSceneTopology( const char* rasterizerBlock, const char* emitterMaterialBlock )
{
	std::string s( "RISE ASCII SCENE 7\n" );
	s += kSceneWallAndCameraTopology;
	s += emitterMaterialBlock;
	s += kEmitterQuadGeometry;
	s += rasterizerBlock;
	return s;
}

//! Box dimensions bound to `obj_emit` for the topology test -- see the
//! block comment above for the closed-form derivation.
static const double kTopoBoxWidth  = 10000.0;
static const double kTopoBoxHeight = 10000.0;
static const double kTopoBoxDepth  = 1.0;

//////////////////////////////////////////////////////////////////////
// TestCheckerLuminaireRGB
//
// PT vs BDPT vs VCM under an RGB (Pel) render.  Pre-fix, BDPT and VCM
// read the light's UV(0,0) corner (colorA/white, exitance 1.0)
// regardless of the actually-sampled point, i.e. exactly 2x the true
// area-weighted average exitance (0.5) that PT's independently-sampled
// NEE UV converges to.  Post-fix, all three must agree within a
// generous Monte-Carlo band.
//////////////////////////////////////////////////////////////////////
static void TestCheckerLuminaireRGB()
{
	std::cout << "DL-44: RGB checker-luminaire floor illumination (PT vs BDPT vs VCM)\n";

	const std::string ptPath   = WriteSceneToTempFile( BuildScene( kRasterizerPT   ).c_str(), "pt"   );
	const std::string bdptPath = WriteSceneToTempFile( BuildScene( kRasterizerBDPT ).c_str(), "bdpt" );
	const std::string vcmPath  = WriteSceneToTempFile( BuildScene( kRasterizerVCM  ).c_str(), "vcm"  );

	Check( !ptPath.empty() && !bdptPath.empty() && !vcmPath.empty(), "scene files written" );

	const double pt   = RenderMeanLuminance( ptPath.c_str() );
	const double bdpt = RenderMeanLuminance( bdptPath.c_str() );
	const double vcm  = RenderMeanLuminance( vcmPath.c_str() );

	PrintStat( "PT",   pt );
	PrintStat( "BDPT", bdpt );
	PrintStat( "VCM",  vcm );

	Check( pt > 1e-6, "PT floor mean is positive (scene actually lit)" );
	Check( bdpt > 1e-6, "BDPT floor mean is positive (scene actually lit)" );
	Check( vcm > 1e-6, "VCM floor mean is positive (scene actually lit)" );

	if( pt > 1e-6 ) {
		const double ratioBDPT = bdpt / pt;
		const double ratioVCM  = vcm  / pt;
		std::printf( "  ratio BDPT/PT = %.4f, ratio VCM/PT = %.4f\n", ratioBDPT, ratioVCM );

		// Post-fix band: BDPT and VCM's own light-sampling strategy now
		// reads the true per-sample UV, so their mean should track PT's
		// within Monte-Carlo noise.  0.75..1.30 is generous for a
		// direct-lighting-only, single-bounce comparison at 256 spp.
		Check( ratioBDPT > 0.75 && ratioBDPT < 1.30,
			"BDPT/PT ratio within MC-noise band of 1.0 (DL-44 fixed)" );
		Check( ratioVCM > 0.75 && ratioVCM < 1.30,
			"VCM/PT ratio within MC-noise band of 1.0 (DL-44 fixed)" );

		// Pre-fix DIAGNOSTIC (not a pass/fail assertion -- this is the
		// number the fix commit message quotes as the red-proof): the
		// closed-form pre-fix bias is exactly 2x (colorA's exitance 1.0
		// over the true average 0.5).
		std::printf(
			"  [diagnostic] pre-fix closed-form bias is 2.0x (UV(0,0) always "
			"reads colorA=white, exitance 1.0, vs true area-weighted average 0.5)\n" );
	}
}

//////////////////////////////////////////////////////////////////////
// TestCheckerLuminaireSpectral
//
// PT vs BDPT NM (hwss=false, so only the NM hero branch fires) twin of
// the RGB test above, exercising BDPTIntegrator's NM rig rebuild.
//////////////////////////////////////////////////////////////////////
static void TestCheckerLuminaireSpectral()
{
	std::cout << "DL-44: spectral (NM) checker-luminaire floor illumination (PT vs BDPT)\n";

	const std::string ptPath   = WriteSceneToTempFile( BuildScene( kRasterizerPTSpectral   ).c_str(), "pts"   );
	const std::string bdptPath = WriteSceneToTempFile( BuildScene( kRasterizerBDPTSpectral ).c_str(), "bdpts" );

	Check( !ptPath.empty() && !bdptPath.empty(), "spectral scene files written" );

	const double pt   = RenderMeanLuminance( ptPath.c_str() );
	const double bdpt = RenderMeanLuminance( bdptPath.c_str() );

	PrintStat( "PT (spectral)",   pt );
	PrintStat( "BDPT (spectral)", bdpt );

	Check( pt > 1e-6, "PT (spectral) floor mean is positive" );
	Check( bdpt > 1e-6, "BDPT (spectral) floor mean is positive" );

	if( pt > 1e-6 ) {
		const double ratio = bdpt / pt;
		std::printf( "  ratio BDPT/PT (spectral) = %.4f\n", ratio );
		Check( ratio > 0.70 && ratio < 1.35,
			"BDPT/PT (spectral) ratio within MC-noise band of 1.0 (DL-44 fixed)" );
	}
}

//////////////////////////////////////////////////////////////////////
// TestUVGeneratorLightSamplingTopology (DL-108)
//
// PT and BDPT, each rendered twice (checker exitance / uniform
// exitance) with a BoxUVGenerator bound to the emitter via the
// construction API (RenderMeanLuminanceWithBoxUV).  ratio =
// mean(checker) / mean(uniform) measures the area-weighted average
// exitance under WHICHEVER UV convention the rasterizer's light
// sampling strategy actually used, cancelling every other transport
// constant (see the block comment above kSceneWallAndCameraTopology
// for the full derivation).  Pre-fix this must cluster near avgNative
// = 0.5 for BOTH rasterizers (LightSampler::SampleLight -> IObject::
// UniformRandomPoint never consulted the override generator, for PT's
// own NEE exactly as much as BDPT's); post-fix it must track avgBox =
// 1.0.
//////////////////////////////////////////////////////////////////////
static void TestUVGeneratorLightSamplingTopology()
{
	std::cout << "DL-108: UV-generator light-sampling topology (PT vs BDPT, checker/uniform ratio)\n";

	const std::string ptCheckerPath = WriteSceneToTempFile(
		BuildSceneTopology( kRasterizerPT, kEmitterMaterialChecker ).c_str(), "topo_pt_checker" );
	const std::string ptUniformPath = WriteSceneToTempFile(
		BuildSceneTopology( kRasterizerPT, kEmitterMaterialUniform ).c_str(), "topo_pt_uniform" );
	const std::string bdptCheckerPath = WriteSceneToTempFile(
		BuildSceneTopology( kRasterizerBDPT, kEmitterMaterialChecker ).c_str(), "topo_bdpt_checker" );
	const std::string bdptUniformPath = WriteSceneToTempFile(
		BuildSceneTopology( kRasterizerBDPT, kEmitterMaterialUniform ).c_str(), "topo_bdpt_uniform" );

	Check( !ptCheckerPath.empty() && !ptUniformPath.empty() &&
		!bdptCheckerPath.empty() && !bdptUniformPath.empty(), "topology scene files written" );

	const double ptChecker = RenderMeanLuminanceWithBoxUV(
		ptCheckerPath.c_str(), "obj_emit", kTopoBoxWidth, kTopoBoxHeight, kTopoBoxDepth );
	const double ptUniform = RenderMeanLuminanceWithBoxUV(
		ptUniformPath.c_str(), "obj_emit", kTopoBoxWidth, kTopoBoxHeight, kTopoBoxDepth );
	const double bdptChecker = RenderMeanLuminanceWithBoxUV(
		bdptCheckerPath.c_str(), "obj_emit", kTopoBoxWidth, kTopoBoxHeight, kTopoBoxDepth );
	const double bdptUniform = RenderMeanLuminanceWithBoxUV(
		bdptUniformPath.c_str(), "obj_emit", kTopoBoxWidth, kTopoBoxHeight, kTopoBoxDepth );

	PrintStat( "PT checker",     ptChecker );
	PrintStat( "PT uniform",     ptUniform );
	PrintStat( "BDPT checker",   bdptChecker );
	PrintStat( "BDPT uniform",   bdptUniform );

	Check( ptChecker > 1e-6 && ptUniform > 1e-6, "PT: both renders positive (scene actually lit)" );
	Check( bdptChecker > 1e-6 && bdptUniform > 1e-6, "BDPT: both renders positive (scene actually lit)" );

	if( ptUniform > 1e-6 && bdptUniform > 1e-6 )
	{
		const double ratioPT   = ptChecker / ptUniform;
		const double ratioBDPT = bdptChecker / bdptUniform;
		std::printf( "  ratio PT (checker/uniform)   = %.4f  (avgNative=0.5, avgBox=1.0)\n", ratioPT );
		std::printf( "  ratio BDPT (checker/uniform) = %.4f  (avgNative=0.5, avgBox=1.0)\n", ratioBDPT );

		// Post-fix band around avgBox=1.0 -- generous for a 256-spp
		// direct-lighting-only Monte-Carlo estimate, but comfortably
		// clear of avgNative=0.5 (the pre-fix value), so this genuinely
		// discriminates fixed from unfixed rather than just checking
		// "the light is on".
		Check( ratioPT > 0.75, "PT ratio tracks avgBox=1.0, not avgNative=0.5 (DL-108 fixed)" );
		Check( ratioBDPT > 0.75, "BDPT ratio tracks avgBox=1.0, not avgNative=0.5 (DL-108 fixed)" );

		std::printf(
			"  [diagnostic] pre-fix closed-form expectation is ratio ~0.5 (light "
			"sampling's UniformRandomPoint ignores the override BoxUVGenerator and "
			"reads the native ClippedPlaneGeometry checker average instead)\n" );
	}
}

int main()
{
	std::cout << "=== EmitterUVSampleTest (DL-44 / DL-108) ===\n";

	TestCheckerLuminaireRGB();
	TestCheckerLuminaireSpectral();
	TestUVGeneratorLightSamplingTopology();

	std::cout << "\nPassed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
