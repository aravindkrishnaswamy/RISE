//////////////////////////////////////////////////////////////////////
//
//  DirectionalFogTest.cpp - Regression guard for residual-ledger item
//    10 of docs/PT_ENV_MIS_DOUBLECOUNT.md: a DIRECTIONAL LIGHT seen by
//    a MEDIUM SCATTER vertex.
//
//  TWO DEFECTS, ONE VERTEX.  `LightSampler::EvaluateDirectLighting{,NM}`
//  Step 1 is the deterministic "zero exitance" sweep -- the only lights
//  that reach it are ambient and directional, because every other light
//  kind has nonzero `radiantExitance()` and is evaluated INLINE by
//  Step 2.  Step 1 is therefore the one NEE row whose arithmetic lives
//  behind the `ILight::ComputeDirectLighting{,NM}` virtual instead of in
//  LightSampler itself, and it is the one row that missed both of the
//  volume-vertex accommodations the other rows already had.
//
//    (a) COSINE.  `MediumTransport::EvaluateInScattering{,NM}` reuses
//        this surface-shaped interface at a phase-function vertex by
//        synthesising a `RayIntersectionGeometric` whose `vNormal` is
//        the OUTGOING direction `wo`.  `DirectionalLight` then computed
//        `fDot = Dot(vDirection, ri.vNormal)` against that synthetic
//        normal and returned early on `fDot <= 0`.  Both halves are
//        wrong at a volume vertex: the factor is meaningless, and the
//        rejection throws away HALF THE SPHERE at a vertex whose phase
//        function scatters over all of it.  Steps 2 and 3 handle this
//        properly -- they test `isVolumeScatter` and force
//        `cosSurface = 1.0`, leaving the phase function (supplied as
//        the `brdf`) to carry the whole angular term.
//
//        The fix threads a trailing defaulted `bVolumeReceiver` down
//        the virtual, exactly as commit 8367fcf7 threaded
//        `bFullSphereReceiver`.  Derivation, restated from ILight.h:
//        the in-scattering integrand is
//            Ls(x, wo) = sigma_s(x) INT_{S^2} p(wi, wo) Li(x, wi) dwi
//        -- full sphere, and its only angular factor is `p`.  There is
//        no `|cos theta|` because the surface form's cosine is the
//        projected-area Jacobian dA_perp/dA of an ORIENTED SURFACE
//        PATCH, and a scattering volume element has neither patch nor
//        orientation.  `p` is already handed to the light as the
//        `brdf`, and `sigma_s` is applied by the caller, so the light
//        owes the estimator `Li` and nothing else: RADIANCE ONLY.
//
//    (b) MEDIUM ATTENUATION.  Step 1's shadow ray is cast inside the
//        light (`CastShadowRayAuto`), which answers only the
//        binary/Fresnel visibility question and never reaches
//        `EvalShadowTransmittance`.  Steps 2 and 3 post-multiply that
//        transmittance on every row.  So a directional light's
//        contribution was not attenuated by the medium it crosses --
//        fog was lit by the full unattenuated beam no matter how much
//        of the medium the beam had to traverse to arrive.  The fix
//        post-multiplies inside LightSampler Step 1 (where `pMedium`,
//        `pMediumObject`, `pPreparedScene` and `bSceneHasObjectMedia`
//        are all already in scope) rather than threading four more
//        parameters through a virtual on every light.
//
//  WHY THE TWO CELLS CAN BE RED-PROVED INDEPENDENTLY.  Both cells use
//  the SAME scene and differ only in the sign of the light's
//  `direction`, and that sign is exactly what separates the defects:
//
//    * Cell 1, LIGHT DOWNRANGE (`direction 0 0 1`).  The camera looks
//      along +Z and the light is at +Z, so at every camera-ray scatter
//      vertex `ri.vNormal == wo == +Z` and `fDot == Dot(+Z, +Z) == 1`
//      EXACTLY.  Defect (a) is therefore INERT here -- a cosine of 1 is
//      a no-op factor and passes the gate -- and the cell measures
//      defect (b) alone.
//
//    * Cell 2, LIGHT BEHIND THE CAMERA (`direction 0 0 -1`).  Now
//      `fDot == Dot(-Z, +Z) == -1` at every camera-ray scatter vertex,
//      so defect (a) rejects EVERY ONE of them and the pre-fix image is
//      essentially black.  The cell measures defect (a).
//
//  THE SCENE, AND WHY EVERY NUMBER IN IT IS THE WAY IT IS.
//
//    A PER-OBJECT fog box -- an object carrying an `interior_medium` --
//    NOT a global medium.  With a global medium, Part B's walk limit of
//    `RISE_INFINITY` makes the transmittance exp(-sigma_t * DBL_MAX)
//    == 0 (physically right for an unbounded medium; see the Part B
//    comment in LightSampler.cpp) and there is no closed form left to
//    assert.  A per-object box gives the boundary walk a real exit
//    point and hence a finite, exactly derivable optical depth.
//
//    Box: x,y in [-10, 10], z in [-10, 200].  Camera at the ORIGIN,
//    INSIDE the box, looking down +Z through a 1-degree field of view.
//    `IORStackSeeding::SeedFromPoint` (PathTracingIntegrator.cpp:3621)
//    seeds the eye stack from the camera position, so the camera ray
//    starts in the fog and the camera-ray inline medium walk -- the
//    site that calls MediumTransport::EvaluateInScattering -- is
//    entered directly, with no surface hit in between.
//
//    The 1-degree fov is what makes the closed forms EXACT rather than
//    approximate.  Every camera ray is within 0.5 degrees of +Z, so
//    (i) a scatter vertex at ray parameter t sits at z = t to within
//    4e-5 relative, (ii) the light ray from that vertex, travelling
//    along +-Z, leaves through a Z FACE and not a side wall (the
//    transverse spread over the full 200 units is 200*tan(0.5deg) =
//    1.75, against a half-width of 10), and (iii) `fDot` in cell 1 is
//    cos(0.5deg) = 0.99996, i.e. 1.
//
//    The box's material is a `perfectrefractor_material` at ior 1.0:
//    a delta pass-through with no bending (n1 == n2) and zero Fresnel
//    reflectance, so the walls are optically invisible and exist only
//    to (1) carry the interior medium and (2) give
//    `EvalShadowTransmittance`'s boundary walk something to pop the
//    medium at.  `casts_shadows FALSE` keeps the walls from blocking
//    the NEE shadow ray, exactly as scenes/Tests/Volumes/
//    pt_nested_media.RISEscene does for its fog spheres.
//
//    sigma_t = 0.005 with sigma_s = 0.00025 (single-scatter albedo
//    0.05).  sigma_t is chosen so the camera-to-far-wall optical depth
//    is exactly 1.0 (0.005 * 200), which puts a big, easily measured
//    spread of light-side optical depths in frame.  The albedo is
//    chosen LOW on purpose: the closed forms below are SINGLE-SCATTER
//    forms, and the multiple-scatter tail they omit is O(albedo).  At
//    0.05 that tail is a few percent, which the tolerance bands absorb
//    explicitly (see below).  Scaling sigma_s does not change the
//    relative variance of the estimator, so buying the low albedo costs
//    nothing but a compensating bump in the light's `power`.
//
//  THE CLOSED FORMS.  Write D = 200 (camera to the far, +Z wall),
//  a = 10 (camera to the near, -Z wall), p = 1/(4 pi) for the isotropic
//  phase function, and I = color * power for the directional light's
//  incident radiance.  With the fix in, the light contributes
//  `I * p * Tr_light` at a scatter vertex (radiance only, times the
//  phase function, times the medium transmittance along the light
//  ray), and the medium walk contributes `sigma_s * exp(-sigma_t t)`
//  per unit length -- the distance-sampling pdf `sigma_t exp(-sigma_t t)`
//  times the single-scatter albedo weight `sigma_s/sigma_t` that
//  PathTracingIntegrator's `medWeight` reduces to for a grey homogeneous
//  medium.  So:
//
//    CELL 1 (light downrange, direction +Z).  A vertex at z = t sees
//    the +Z wall at distance d(t) = D - t, so Tr_light = e^-sigma_t(D-t)
//    and the two exponentials CANCEL:
//
//      L1 = INT_0^D sigma_s e^-sigma_t t * p * I * e^-sigma_t (D - t) dt
//         = sigma_s * p * I * D * e^-(sigma_t D)
//
//    with sigma_t D = 1.  The PRE-FIX value (no light-side attenuation,
//    Tr_light == 1) is instead
//
//      L1_prefix = sigma_s * p * I * (1 - e^-(sigma_t D)) / sigma_t
//
//    a factor (1 - e^-1)/(D sigma_t e^-1) = 1.7183 too bright.  d(t) is
//    exact because the light direction is axis-aligned: the ray from
//    (x, y, t) along +Z meets the plane z = 200 at distance 200 - t
//    regardless of x and y.
//
//    CELL 2 (light behind the camera, direction -Z).  A vertex at
//    z = t sees the -Z wall at distance d(t) = t + a, so the two
//    exponentials REINFORCE:
//
//      L2 = INT_0^D sigma_s e^-sigma_t t * p * I * e^-sigma_t (t + a) dt
//         = sigma_s * p * I * e^-(sigma_t a) * (1 - e^-(2 sigma_t D))
//           / (2 sigma_t)
//
//    The PRE-FIX value is ~0: every camera-ray scatter vertex is
//    rejected by `fDot <= 0`.  What little survives comes from
//    multiple-scatter vertices whose `wo` happens to point backwards.
//
//  MEASURED, THIS MACHINE, POST-FIX.  Four SAMPLES (n = 4 repeat runs
//  of this binary), not measured bounds -- much of RISE's sampling is
//  deterministic per pixel at a fixed sample count, so repeat runs
//  under-sample the true run-to-run range and their min/max must not be
//  read as one.  The bands below are sized off the VALUES with explicit
//  headroom, never off this spread.  Ratios to the closed forms:
//
//    cell 1  L / L1        0.99685 / 0.99691 / 0.99813 / 0.99902
//    cell 2  L / L2        1.00113 / 0.99879 / 0.99743 / 0.99626
//    cell 3  L / L3        1.00000 / 1.00000 / 1.00000 / 1.00000
//    cell 4  ratio / pred  0.98874 / 0.99512 / 0.98670 / 0.98838
//
//  Cells 1 and 2 sit within 0.4 % of their single-scatter closed forms.
//  Two effects of that order are known to be present and to pull in
//  OPPOSITE directions, and no attempt is made here to separate them:
//  the omitted multiple-scatter tail is positive and worth roughly
//  albedo * tau_escape ~ 0.05 * 0.07 ~ 0.35 % (a scattered ray leaves
//  through a side wall after ~10-20 units, so its escape optical depth
//  is small by construction), while the walk's Russian roulette and
//  volume-bounce bound lose a little from the same tail.  The residual
//  is a fifth of the band either way, and neither effect can move the
//  cell anywhere near its pre-fix value.
//
//  Cell 3 is EXACT to all printed digits, which is the expected result
//  and not a coincidence: every path there is primary-hit plus NEE
//  against a delta-direction light, so there is no Monte Carlo variance
//  to speak of.  A cell-3 failure is a real behaviour change, never
//  noise -- its band is +-1 % only to absorb film/filter arithmetic.
//
//  CELL 3 is the unchanged-surfaces guard: a Lambertian plane lit
//  head-on by a directional light in a scene with NO media at all.
//  Both halves of the fix must be exactly invisible there -- Part A
//  because `bVolumeReceiver` is false at a surface vertex and the
//  expression reduces textually to the pre-existing one, Part B because
//  `EvalShadowTransmittance` early-returns (1,1,1) when the scene has
//  no origin medium, no global medium and no object media.  The
//  assertion is the exact Lambertian closed form rho/pi * I * cos, with
//  cos == 1 by construction.
//
//  CELL 4 is the SPECTRAL twin, guarding
//  `EvaluateDirectLightingNM` / `ComputeDirectLightingNM` -- the sibling
//  site of every change above.  It asserts the RATIO L2/L1 rather than
//  two absolute values, so it is immune to any overall spectral-pipeline
//  normalisation offset while still failing on BOTH defects: reverting
//  Part A collapses L2 to ~0 and the ratio with it; reverting Part B
//  removes the light-side exponential from both cells, whose closed
//  forms then become identical and the ratio goes to 1.0.
//
//  RED-PROVE, BOTH HALVES, INDEPENDENTLY.  Each half was reverted in
//  place, the tree rebuilt, this binary re-run, and the source restored
//  byte-for-byte.
//
//    (A) `bVolumeReceiver` made inert in DirectionalLight only -- the
//        `fDot` expression and the `<= 0` gate restored to their
//        pre-fix form in BOTH the RGB and the NM overload:
//
//                        reverted   fixed     band
//          cell 1        0.99686    0.99737   [0.97, 1.05]  unchanged
//          cell 2        0.00000    0.99721   [0.97, 1.05]  FAILS
//          cell 3        1.00000    1.00000   [0.99, 1.01]  unchanged
//          cell 4 ratio  0.00000    1.10916   1.1179 +-4 %  FAILS
//
//        3 of the file's 11 checks fail.  Cell 2's measured value is
//        EXACTLY zero: every camera-ray scatter vertex has
//        `Dot(vDirection, wo) == -1` and is rejected, and the surviving
//        multiple-scatter vertices contribute nothing measurable at
//        albedo 0.05.  Cell 1 moving by 0.05 % is the evidence that the
//        two cells really do isolate the two defects.
//
//    (B) The `EvalShadowTransmittance` / `EvalShadowTransmittanceNM`
//        post-multiply dropped from LightSampler Step 1 (both the RGB
//        and the NM site):
//
//                        reverted   fixed     band
//          cell 1        1.71438    0.99737   [0.97, 1.05]  FAILS
//          cell 2        1.53703    0.99721   [0.97, 1.05]  FAILS
//          cell 3        1.00000    1.00000   [0.99, 1.01]  unchanged
//          cell 4 ratio  0.99347    1.10916   1.1179 +-4 %  FAILS
//
//        3 of the file's 11 checks fail.  Cell 1's reverted mean is
//        1.00377 against the DERIVED unattenuated closed form
//        `sigma_s p I (1 - e^-1)/sigma_t` = 1.00605 -- agreement to
//        0.23 %, which is the sharpest confirmation in this file that
//        the mechanism is understood and not merely bracketed.  Cell 4's
//        reverted ratio lands at 0.993, i.e. the 1.0 the closed forms
//        collapse to once the light-side exponential is gone.
//
//        Cell 3 is unchanged under BOTH reverts, which is its whole job.
//
//  HARNESS PATTERN: tests/VolumeEnvFurnaceTest.cpp -- scene text as
//  ordinary `RISE ASCII SCENE 7` chunks in C++ string literals, written
//  to a temp file, loaded through `IJobPriv::LoadAsciiSceneViaCst` (the
//  real CST path `bin/rise` uses), rendered, captured in memory by a
//  `CapturingRasterizerOutput`.  No scene file is added under scenes/,
//  so no CST golden regeneration is needed.  `oidn_denoise FALSE`
//  everywhere: `OutputDenoisedImage`'s default forwards POST-denoise
//  pixels to `OutputImage`, which a capture overriding only
//  `OutputImage` would silently pick up.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 27, 2026
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
#include <sstream>
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
// CapturingRasterizerOutput -- same shape as VolumeEnvFurnaceTest's.
// Deliberately does NOT override OutputPreDenoisedImage /
// OutputDenoisedImage; see the file header.
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

struct ImageStats
{
	double luminance;	// mean over pixels of (r+g+b)/3
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	double sum = 0;
	for( const RISEColor& c : cap.pixels ) {
		sum += (c.base.r + c.base.g + c.base.b) / 3.0;
	}
	s.luminance = sum / double( cap.pixels.size() );
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/directional_fog_test_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderAndComputeStats( const std::string& sceneText, const char* tag )
{
	ImageStats result{};

	const std::string path = WriteSceneToTempFile( sceneText, tag );
	if( path.empty() ) {
		return result;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path.c_str() );
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		std::remove( path.c_str() );
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( bRendered ) {
		result = ComputeStats( *pCap );
	}

	std::remove( path.c_str() );
	safe_release( pCap );
	safe_release( pJob );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Scene constants -- every one of them is load-bearing for a closed
// form above.  See the file header for the derivations.
//////////////////////////////////////////////////////////////////////

static const double kSigmaT      = 0.005;		// extinction
static const double kSigmaS      = 0.00025;		// scattering (albedo 0.05)
static const double kSigmaA      = kSigmaT - kSigmaS;
static const double kNearWall    = 10.0;		// camera to the -Z wall  (a)
static const double kFarWall     = 200.0;		// camera to the +Z wall  (D)
static const double kBoxHalfXY   = 10.0;
static const double kFogFovDeg   = 1.0;
static const double kLightPower  = 400.0;		// I = color(1,1,1) * power
static const double kInvFourPi   = 1.0 / (4.0 * 3.14159265358979323846);

// Cell 3 (media-free surface control).
static const double kPlaneZ       = 50.0;
static const double kPlaneHalfXY  = 100.0;
static const double kPlaneRho     = 0.5;
static const double kPlanePower   = 1.0;
static const double kPlaneFovDeg  = 30.0;

/// The fog scene.  `dirZ` is the light's `direction` z component:
/// +1 puts the light DOWNRANGE (cell 1, defect (b) isolated),
/// -1 puts it BEHIND THE CAMERA (cell 2, defect (a) isolated).
static std::string SceneFog( unsigned int width, unsigned int height, double dirZ )
{
	// box_geometry is centred on the object's origin, so a depth of
	// (kNearWall + kFarWall) placed at z = (kFarWall - kNearWall)/2
	// spans exactly [-kNearWall, +kFarWall].
	const double boxDepth = kNearWall + kFarWall;
	const double boxCentreZ = (kFarWall - kNearWall) / 2.0;

	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		// Camera sits INSIDE the box at the origin; IORStackSeeding
		// picks the box up so the eye ray starts in the medium.
		"pinhole_camera\n{\n\tlocation 0 0 0\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov "
			<< kFogFovDeg << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1.0 1.0 1.0\n}\n\n"
		// ior 1.0 => no bending, zero Fresnel reflectance: an optically
		// invisible wall that exists only to carry the interior medium
		// and to bound the shadow walk.
		"perfectrefractor_material\n{\n\tname mat_null\n\trefractance pnt_white\n\tior 1.0\n}\n\n"
		"homogeneous_medium\n{\n"
		"\tname fog\n"
		"\tabsorption " << kSigmaA << " " << kSigmaA << " " << kSigmaA << "\n"
		"\tscattering " << kSigmaS << " " << kSigmaS << " " << kSigmaS << "\n"
		"\tphase isotropic\n"
		"}\n\n"
		"box_geometry\n{\n\tname fogbox_geom\n"
		"\twidth "  << (2.0 * kBoxHalfXY) << "\n"
		"\theight " << (2.0 * kBoxHalfXY) << "\n"
		"\tdepth "  << boxDepth << "\n"
		"}\n\n"
		"standard_object\n{\n\tname fogbox\n\tgeometry fogbox_geom\n"
		"\tposition 0 0 " << boxCentreZ << "\n"
		"\tmaterial mat_null\n"
		"\tinterior_medium fog\n"
		// The walls must not occlude the NEE shadow ray; the medium
		// walk finds them regardless (it uses a plain intersect).
		"\tcasts_shadows FALSE\n"
		"}\n\n"
		// direction is FROM the surface TO the light (SCENE_CONVENTIONS
		// section 1), so dirZ = +1 is a light downrange at +Z.
		"directional_light\n{\n\tname sun\n\tcolor 1.0 1.0 1.0\n"
		"\tpower " << kLightPower << "\n"
		"\tdirection 0 0 " << dirZ << "\n}\n\n";
	return ss.str();
}

/// Cell 3's scene: a Lambertian plane facing the camera, a directional
/// light head-on, and NO media anywhere.
static std::string ScenePlane( unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 0\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov "
			<< kPlaneFovDeg << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_grey\n\tcolor "
			<< kPlaneRho << " " << kPlaneRho << " " << kPlaneRho << "\n}\n\n"
		"lambertian_material\n{\n\tname mat_grey\n\treflectance pnt_grey\n}\n\n"
		// Winding pta->ptb->ptc gives N = (ptb-pta) x (ptc-ptb) = -Z,
		// i.e. the plane faces the camera.
		"clippedplane_geometry\n{\n\tname plane_geom\n"
		"\tpta " << -kPlaneHalfXY << " " << -kPlaneHalfXY << " " << kPlaneZ << "\n"
		"\tptb " << -kPlaneHalfXY << " " <<  kPlaneHalfXY << " " << kPlaneZ << "\n"
		"\tptc " <<  kPlaneHalfXY << " " <<  kPlaneHalfXY << " " << kPlaneZ << "\n"
		"\tptd " <<  kPlaneHalfXY << " " << -kPlaneHalfXY << " " << kPlaneZ << "\n"
		"}\n\n"
		"standard_object\n{\n\tname plane\n\tgeometry plane_geom\n\tmaterial mat_grey\n}\n\n"
		// direction -Z: the light is behind the camera, shining onto the
		// plane's -Z-facing side, so N . direction == 1 exactly.
		"directional_light\n{\n\tname sun\n\tcolor 1.0 1.0 1.0\n"
		"\tpower " << kPlanePower << "\n\tdirection 0 0 -1\n}\n\n";
	return ss.str();
}

static std::string RasterizerPTRgb( unsigned int samples )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/directional_fog_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string RasterizerPTSpectral( unsigned int samples )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_spectral_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
		"\thwss false\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/directional_fog_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string AssembleScene( const std::string& common, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + common + rasterizer;
}

//////////////////////////////////////////////////////////////////////
// The closed forms.  Written as code rather than as baked literals so
// the derivation in the file header is checkable against the constants
// above without a calculator.
//////////////////////////////////////////////////////////////////////

/// Cell 1: light DOWNRANGE.  Camera-side and light-side exponentials
/// cancel; see the header.
///   L1 = sigma_s * p * I * D * exp(-sigma_t * D)
static double PredictLightDownrange()
{
	return kSigmaS * kInvFourPi * kLightPower * kFarWall * std::exp( -kSigmaT * kFarWall );
}

/// Cell 1 PRE-FIX (Part B reverted -- no light-side attenuation):
///   L1_prefix = sigma_s * p * I * (1 - exp(-sigma_t D)) / sigma_t
static double PredictLightDownrangeNoAttenuation()
{
	return kSigmaS * kInvFourPi * kLightPower *
		( 1.0 - std::exp( -kSigmaT * kFarWall ) ) / kSigmaT;
}

/// Cell 2: light BEHIND THE CAMERA.  Both exponentials reinforce.
///   L2 = sigma_s * p * I * exp(-sigma_t a) * (1 - exp(-2 sigma_t D))
///        / (2 sigma_t)
static double PredictLightBehind()
{
	return kSigmaS * kInvFourPi * kLightPower *
		std::exp( -kSigmaT * kNearWall ) *
		( 1.0 - std::exp( -2.0 * kSigmaT * kFarWall ) ) / ( 2.0 * kSigmaT );
}

/// Cell 3: Lambertian plane, head-on directional light, no media.
///   L3 = rho / pi * I * cos, cos == 1
static double PredictPlane()
{
	return kPlaneRho / 3.14159265358979323846 * kPlanePower;
}

int main()
{
	std::cout << "Running DirectionalFogTest..." << std::endl;
	std::cout << "  (directional light at a MEDIUM SCATTER vertex:" << std::endl;
	std::cout << "   radiance-only shading + medium attenuation)" << std::endl;

	const unsigned int kRes        = 32;
	const unsigned int kSamplesRgb = 256;
	const unsigned int kSamplesSpc = 96;

	//////////////////////////////////////////////////////////////////
	// CELL 1 -- MEDIUM ATTENUATION on the direct beam (defect (b)).
	//
	// Light downrange, so fDot == 1 at every camera-ray scatter vertex
	// and defect (a) is inert.  The measured mean must match
	//   sigma_s * p * I * D * e^-(sigma_t D)
	// and must be FAR below the unattenuated form, which is 1.7183x
	// larger.
	//
	// BAND: [-3 %, +5 %] around the closed form.  Measured 0.9969 to
	// 0.9990 over four runs, so there is ~13x the observed run-to-run
	// spread of headroom below and ~24x above.  The pre-fix state reads
	// 1.7183x the closed form, which misses the upper cap by 1.64x.
	//////////////////////////////////////////////////////////////////
	const double pred1 = PredictLightDownrange();
	const double pred1NoAtten = PredictLightDownrangeNoAttenuation();
	const ImageStats s1 = RenderAndComputeStats(
		AssembleScene( SceneFog( kRes, kRes, +1.0 ), RasterizerPTRgb( kSamplesRgb ) ),
		"downrange" );

	Check( s1.valid, "cell 1 (light downrange, RGB) rendered" );
	if( s1.valid ) {
		const double ratio = s1.luminance / pred1;
		std::cout << "  cell 1  light downrange   measured " << s1.luminance
			<< "  predicted " << pred1
			<< "  ratio " << ratio
			<< "   (pre-fix prediction " << pred1NoAtten << ")" << std::endl;
		Check( ratio >= 0.97, "cell 1: not below the single-scatter closed form" );
		Check( ratio <= 1.05, "cell 1: MEDIUM ATTENUATION applied to the directional beam" );
	}

	//////////////////////////////////////////////////////////////////
	// CELL 2 -- NO SPURIOUS COSINE at the volume vertex (defect (a)).
	//
	// Light behind the camera: `Dot(vDirection, wo) == -1` at every
	// camera-ray scatter vertex, so the pre-fix `fDot <= 0` gate
	// rejected all of them and the image was essentially black.  The
	// measured mean must match the derived isotropic-phase
	// single-scatter value.
	//
	// BAND: [-3 %, +5 %], same reasoning as cell 1.  Measured 0.9963 to
	// 1.0011 over four runs.  The separate "materially above pre-fix"
	// assertion is stated against a floor of HALF the prediction, which
	// the pre-fix state misses outright: it measures EXACTLY zero (every
	// camera-ray scatter vertex is rejected, and the multiple-scatter
	// vertices whose `wo` points backwards contribute nothing measurable
	// at albedo 0.05).  It is kept as a separate, differently-worded
	// check from the band below so a future band widening cannot quietly
	// retire the defect-(a) guard.
	//////////////////////////////////////////////////////////////////
	const double pred2 = PredictLightBehind();
	const ImageStats s2 = RenderAndComputeStats(
		AssembleScene( SceneFog( kRes, kRes, -1.0 ), RasterizerPTRgb( kSamplesRgb ) ),
		"behind" );

	Check( s2.valid, "cell 2 (light behind camera, RGB) rendered" );
	if( s2.valid ) {
		const double ratio = s2.luminance / pred2;
		std::cout << "  cell 2  light behind cam  measured " << s2.luminance
			<< "  predicted " << pred2
			<< "  ratio " << ratio << std::endl;
		Check( s2.luminance >= 0.5 * pred2,
			"cell 2: below-horizon volume vertices are lit at all (no fDot<=0 rejection)" );
		Check( ratio >= 0.97, "cell 2: not below the single-scatter closed form" );
		Check( ratio <= 1.05, "cell 2: matches the derived isotropic single-scatter value" );
	}

	//////////////////////////////////////////////////////////////////
	// CELL 3 -- SURFACE RECEIVERS UNCHANGED.
	//
	// A Lambertian plane, a head-on directional light, and no media at
	// all.  Part A is inert (bVolumeReceiver is false at a surface, and
	// the expression reduces textually to the pre-existing one); Part B
	// is inert (EvalShadowTransmittance early-returns (1,1,1) with no
	// origin medium, no global medium and no object media).  The exact
	// Lambertian answer rho/pi * I must survive to within MC noise --
	// and there is essentially none here, since every path is
	// primary-hit + NEE against a delta-direction light.
	//////////////////////////////////////////////////////////////////
	const double pred3 = PredictPlane();
	const ImageStats s3 = RenderAndComputeStats(
		AssembleScene( ScenePlane( kRes, kRes ), RasterizerPTRgb( 16 ) ),
		"plane" );

	Check( s3.valid, "cell 3 (media-free lit surface) rendered" );
	if( s3.valid ) {
		const double ratio = s3.luminance / pred3;
		std::cout << "  cell 3  lit plane, no fog measured " << s3.luminance
			<< "  predicted " << pred3
			<< "  ratio " << ratio << std::endl;
		Check( std::fabs( ratio - 1.0 ) <= 0.01,
			"cell 3: media-free surface shading is unchanged" );
	}

	//////////////////////////////////////////////////////////////////
	// CELL 4 -- SPECTRAL (NM) TWIN.
	//
	// `EvaluateDirectLightingNM` / `ComputeDirectLightingNM` are the
	// sibling sites of every change cells 1 and 2 exercise, and they
	// carried the identical defects.  Asserted as the RATIO L2/L1 so
	// the cell is immune to any overall spectral-pipeline
	// normalisation offset while still failing on both defects:
	//   * Part A reverted  -> L2 -> ~0, ratio -> ~0.
	//   * Part B reverted  -> the light-side exponential disappears
	//                         from both closed forms, which then become
	//                         IDENTICAL, so the ratio -> 1.0.
	// The predicted ratio is 1.1178.  Measured 0.9867 to 0.9951 of that
	// over four runs -- a ~1.2 % systematic shortfall which is NOT
	// separately diagnosed here and is a third of the band.  The +-4 %
	// band therefore has ~2x headroom on the low side, while the Part-B
	// reverted state (ratio -> 1.0, i.e. 0.894 of predicted) misses it
	// by 2.6x and the Part-A reverted state misses it outright.
	//////////////////////////////////////////////////////////////////
	const double predRatio = pred2 / pred1;
	const ImageStats s4a = RenderAndComputeStats(
		AssembleScene( SceneFog( kRes, kRes, +1.0 ), RasterizerPTSpectral( kSamplesSpc ) ),
		"spectral_downrange" );
	const ImageStats s4b = RenderAndComputeStats(
		AssembleScene( SceneFog( kRes, kRes, -1.0 ), RasterizerPTSpectral( kSamplesSpc ) ),
		"spectral_behind" );

	Check( s4a.valid && s4b.valid, "cell 4 (spectral NM twin) rendered" );
	if( s4a.valid && s4b.valid && s4a.luminance > 0 ) {
		const double ratio = s4b.luminance / s4a.luminance;
		std::cout << "  cell 4  spectral NM      downrange " << s4a.luminance
			<< "  behind " << s4b.luminance
			<< "  ratio " << ratio
			<< "  predicted " << predRatio << std::endl;
		Check( std::fabs( ratio / predRatio - 1.0 ) <= 0.04,
			"cell 4: the NM path applies both the radiance-only rule and the medium attenuation" );
	}

	std::cout << std::endl;
	std::cout << "DirectionalFogTest: " << passCount << " passed, "
		<< failCount << " failed" << std::endl;

	return failCount == 0 ? 0 : 1;
}
