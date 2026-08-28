//////////////////////////////////////////////////////////////////////
//
//  VolumeEnvFurnaceTest.cpp - Regression guard for the MEDIUM-SCATTER
//    paths of the path tracer.  Four defects, all closed, all guarded
//    here:
//
//      Fix 1 (slice F2 of the PT env-MIS arc) - the env MIS partition
//             at the scatter vertex.  Guarded by cells 1-3.
//      Fix 2 (residual wave 4) - the multiple-scatter continuation on
//             CAMERA rays, which the path used to truncate after ONE
//             scatter.  Guarded by cells 4-6.
//      Fix 3 (residual wave 4b) - the same truncation one bounce
//             further along: IntegrateFromHitHWSS, the shared bounce
//             loop for the hero bundle, did NEE at a volume-scatter
//             vertex and then broke out of the loop, so a ray that had
//             already bounced off a SURFACE lost its whole
//             multiple-scatter tail.  Guarded by cells 7-9.
//      Fix 4 (residual wave 4c) - NEE at a volume-scatter vertex cast
//             NO SHADOW RAY, so every medium vertex integrated light it
//             could not actually see.  Guarded by cells 7-9, which are
//             now ABSOLUTE furnace assertions because of it.
//
//  WHAT IS GUARDED - FIX 1 (env MIS partition).
//    PathTracingIntegrator::IntegrateRayTemplated handles a camera ray
//    that scatters in a participating medium before reaching any
//    surface inline: it evaluates env-NEE at the scatter point
//    (MediumTransport::EvaluateInScattering ->
//    LightSampler::EvaluateDirectLighting with isVolumeScatter=true,
//    which MIS-weights the env sample against the PHASE pdf), then
//    samples the phase function for a continuation.  When that
//    continuation escapes the scene it reads the environment.  That
//    read used to be added at MIS weight 1 -- the exact same defect
//    the preceding commit fixed on the SURFACE escape path -- so the
//    two env strategies summed to 1 + w_nee instead of 1.  For an
//    isotropic phase function under a uniform environment,
//    envPdf == phasePdf == 1/(4 pi), so w_nee = 0.5 under the power-2
//    heuristic and the single-scatter env term was 50 % too bright.
//    The HWSS twin (IntegrateRayHWSS) carried the identical defect.
//
//  WHAT IS GUARDED - FIX 2 (multiple-scatter continuation).
//    The same three sites used to handle exactly ONE scatter.  A
//    continuation that missed all geometry was TERMINATED with a
//    deterministic Beer-Lambert factor (`TrEsc`) times the
//    environment: the "scatter AGAIN" event was dropped outright, so
//    the estimator lost its whole multiple-scatter tail, an energy
//    LOSS of order tau*tau'.  (The main loop in
//    IntegrateFromHitTemplated always re-sampled the medium and looped;
//    the camera-ray inline path was a legacy single-scatter
//    approximation, and existed separately only because the main loop
//    is entered FROM A HIT and there is no RayIntersection to hand it
//    until the walk finds a surface.)  All three sites now run the
//    ordinary volumetric random walk, bounded by
//    stabilityConfig.maxVolumeBounce and Russian roulette exactly as
//    the main loop bounds its own volume bounces, with the env MIS
//    partition of Fix 1 re-closed at EVERY scatter vertex rather than
//    only the first.  The estimator, its invariants and the
//    indirect-only routing are derived in the comment on that walk in
//    PathTracingIntegrator.cpp; the numbers below are its measurement.
//
//  HARNESS PATTERN: EnvLightBalanceTest.cpp / HairRenderTest.cpp --
//    scene text assembled as ordinary `RISE ASCII SCENE 7` chunks in
//    C++ string literals, written to a temp file, loaded through
//    IJobPriv::LoadAsciiSceneViaCst (the real CST path `bin/rise`
//    uses), rendered, and captured in memory via a
//    CapturingRasterizerOutput.  No scene file is added to
//    scenes/Tests, so no CST golden regeneration is needed.
//    `oidn_denoise FALSE` everywhere: OutputDenoisedImage's default
//    forwards POST-denoise pixels to OutputImage, which a capture
//    that only overrides OutputImage would silently pick up.
//
//  THE TWO SCENES, AND WHY THERE ARE TWO.
//    Cells 1-3 (the thin COLUMN) predate Fix 2 and were shaped AROUND
//    the truncation, because with the truncation present a furnace
//    could not be used to measure Fix 1 directly: at moderate optical
//    depth the truncation deficit and the MIS over-count very nearly
//    CANCEL, so a plain fog box read ~1.0 with the MIS bug PRESENT and
//    low without it -- a "must read 1.0" assertion on that scene would
//    have had the sign of the test backwards.  The column removes the
//    truncation from the answer instead of modelling it: the medium is
//    a long, thin column along the view axis (bbox 3 x 3 x 101) and the
//    camera sits inside its near end looking down its length through a
//    1-degree field of view.  The VIEW ray accumulates a substantial
//    optical depth (sigma_t = 0.004 over ~100 units, tau ~= 0.40, so
//    ~33 % of camera rays scatter), while a scattered ray -- isotropic,
//    so essentially never within the ~0.9-degree cone that would send
//    it back down the column -- leaves through a side wall after only
//    ~1-3 units, i.e. an escape optical depth of ~0.01.  The dropped
//    multiple-scatter branch was thus worth only ~0.4 % there.
//
//    The column is KEPT as the Fix-1 guard (it is the geometry the
//    +16 % red-prove below was measured on, and its near-unit
//    single-scatter albedo pins the mean tightly), but it is no longer
//    the interesting cell for Fix 2.
//
//    Cells 4-6 (the cubical FOG BOX) are the Fix-2 guard and could only
//    be written once the walk continued: a 200-unit cube centred on the
//    camera, tau = 0.4 to a face and 0.69 into a corner, 40-degree
//    field of view.  sigma_a = 0 makes the closed-form furnace answer
//    exactly 1.0 at EVERY pixel whatever optical depth that pixel's ray
//    sees, so the spread of depths in frame means a fix that only
//    happened to work at one tau cannot pass.  The truncated estimator
//    instead reads
//
//      e^-tau + (1 - e^-tau) * E[Tr_escape]  ~= 0.89
//
//    i.e. every path that scattered ONCE was allowed to escape but
//    never to scatter again.  Measured pre-Fix-2: 0.880.
//
//  MEASURED, THIS MACHINE, POST-FIX (both fixes in).  Four SAMPLES
//    (n = 4 repeat runs of this binary), not measured bounds -- much of
//    RISE's sampling is deterministic per pixel at a fixed sample
//    count, so repeat runs under-sample the true run-to-run range and
//    their min/max must not be read as one.  The tolerances below are
//    sized off these values with explicit headroom, never off their
//    spread:
//
//      column  RGB PT              0.99854 / 0.99863 / 0.99812 / 0.99834
//      column  spectral hwss=false 1.00200 / 1.00397 / 1.00347 / 1.00273
//      column  spectral hwss=true  0.95686 / 0.95652 / 0.95681 / 0.95650
//      fog box RGB PT              0.99625 / 0.99681 / 0.99679 / 0.99622
//      fog box spectral hwss=false 1.00228 / 0.99916 / 1.00195 / 1.00109
//      fog box spectral hwss=true  0.95483 / 0.95510 / 0.95464 / 0.95438
//
//    Per-pixel spread over those runs: column RGB [0.970, 1.022],
//    hwss=false [0.953, 1.058], hwss=true [0.932, 0.982]; fog box RGB
//    [0.964, 1.024], hwss=false [0.948, 1.050], hwss=true
//    [0.934, 0.978].
//
//    RGB and hwss=false now sit within ~0.4 % of unity on BOTH
//    geometries.  On the column the residual moved from ~0.46 % under
//    to ~0.15 % under when Fix 2 landed, which is the ~0.4 % truncation
//    term that scene was designed to make small being collected rather
//    than dropped -- the direction and the order of magnitude both
//    match the prediction.  hwss=false crosses slightly OVER unity on
//    both (+0.2 to +0.4 %); that residual is not separately diagnosed
//    and is not claimed to be understood, and it is a fifth of the
//    band.
//
//  THE hwss=true DEFICIT IS PRE-EXISTING AND IS NEITHER FIX.  The
//    hero-wavelength bundle reads ~4.3 % under unity on the column and
//    ~4.5 % under on the fog box -- the SAME deficit on two geometries
//    whose truncation terms differ by 30x, which is itself evidence
//    that it is neither of the defects fixed here.  It is the
//    spectral-bundle env deficit CLAUDE.md's "High-Value Facts" already
//    records for hwss=true env-IBL (18 % under PT on the
//    EnvLightBalanceTest uniform env-only topology, at the disc-area
//    baseline, independent of any MIS migration).  Both red-proves
//    below settle it as pre-existing rather than introduced: reverting
//    Fix 1 moves column hwss=true from -4.67 % to +10.90 %, the same
//    15.5 pp shift the other two variants show (RGB 15.7 pp, hwss=false
//    16.4 pp); reverting Fix 2 moves fog-box hwss=true from -4.5 % to
//    -15.8 %, the same ~11 pp shift the other two show.  Each fix does
//    exactly one thing to the bundle path and the residual sits
//    underneath both untouched.  hwss=true accordingly gets its own
//    asymmetric band (below) rather than being dropped or having the
//    shared band widened to hide it.
//
//  RED-PROVE, FIX 1.  With Fix 1 reverted (the MIS weight dropped again
//    at both camera-ray medium-escape sites, i.e. the two
//    `if( pLS && phasePdf > 0 )` guards forced false), rebuilt and
//    re-run, this binary reported on the column cells:
//
//      RGB PT              1.15731 / 1.15732   (+15.73 %)
//      spectral hwss=false 1.16410 / 1.16395   (+16.39 %)
//      spectral hwss=true  1.10869 / 1.10904   (+10.90 %)
//
//    against a predicted +0.5 * (1 - e^-tau) * E[Tr_escape] ~= +16 %.
//    (Measured before the fog-box cells existed, hence column only.)
//
//  RED-PROVE, FIX 2.  Measured on the truncating build -- i.e. the tree
//    as it stood with Fix 1 in and the walk still stopping after one
//    scatter -- with the fog-box cells added but nothing else changed:
//
//      column  RGB PT              0.99519   (-0.48 %)
//      column  spectral hwss=false 0.99973   (-0.03 %)
//      column  spectral hwss=true  0.95371   (-4.63 %)
//      fog box RGB PT              0.87997   (-12.00 %)
//      fog box spectral hwss=false 0.88303   (-11.70 %)
//      fog box spectral hwss=true  0.84220   (-15.78 %)
//
//    against the predicted ~0.89 for the truncated estimator.  Six of
//    this file's checks fail in that state (all six on the fog box,
//    which is the point of adding it: the column moves only 0.3-0.4 pp
//    and would never have caught this).
//
//  WHAT IS GUARDED - FIX 3 (the SURFACE-BOUNCE volumetric walk).
//    Cells 1-6 only ever reach a medium from a CAMERA ray.  The other
//    volumetric walk -- the one inside the shared bounce loop, entered
//    when a ray that has already scattered off a surface then scatters
//    in a medium -- was untested by anything in the tree.  RGB and NM
//    share `IntegrateFromHitTemplated`, which always looped correctly;
//    the hero bundle's `IntegrateFromHitHWSS` did NEE at the volume
//    vertex and then `break`, dropping every subsequent scatter, the
//    surface hand-off and the escape.  Cells 7-9 add a scene that
//    reaches it: the same fog box, camera pointed straight down at a
//    perfectly white Lambertian floor 20 units below (see the scene's
//    own comment for the construction and why it is lossless).
//
//  RED-PROVE, FIX 3.  Same tree, HWSS walk reverted to the `break`.
//    Measured BEFORE Fix 4 landed, so every number here carries the
//    ~+6 % over-count Fix 4 removed; read them as a comparison between
//    two states of the same build, not as current absolutes:
//
//      floor RGB PT              1.06261   (unchanged: 1.06210 fixed)
//      floor spectral hwss=false 1.06903   (unchanged: 1.06704 fixed)
//      floor spectral hwss=true  0.72583   (1.01382 fixed)
//
//    i.e. the RGB and NM cells are byte-for-byte unaffected by the fix
//    -- which is the file's EVIDENCE, not its assumption, that those
//    two twins already continued -- while the hero bundle recovers from
//    a ratio-to-RGB of 0.683 to 0.955.  Cell 9's ratio band and its
//    structural-dark band both fail in that state; with the walk
//    continuing, every check in the file passes.  Post-Fix-4 the same
//    revert also fails cell 9's new absolute mean band (0.726 against a
//    -9 % floor, 3x outside).
//
//  WHAT IS GUARDED - FIX 4 (the UNSHADOWED volume-vertex NEE).
//    Cells 7-9 used to read ~+6 % over unity and could therefore only
//    assert RATIOS.  The excess was a pre-existing defect that this
//    scene is the first thing in the tree to reach, because reaching it
//    needs a MEDIUM VERTEX WITH GEOMETRY AROUND IT, and every prior
//    volume test in the tree is a bare medium in empty space.
//
//    MECHANISM.  `LightSampler::EvaluateDirectLighting{,NM}` gated all
//    three of its NEE rows' shadow rays on
//
//        if( pShadingObject && pShadingObject->DoesReceiveShadows() )
//
//    `pShadingObject` is NULL at a volume-scatter vertex --
//    MediumTransport::EvaluateInScattering{,NM} is the only caller that
//    passes 0, and it always pairs it with isVolumeScatter=true -- so
//    the gate was false and NO SHADOW RAY WAS EVER CAST there.  Every
//    medium vertex integrated the environment (and any delta or mesh
//    light) over the FULL sphere, including the directions the floor
//    blocks.  Its phase-sampled MIS partner does stop at the floor, so
//    the two strategies summed to more than 1 by exactly the blocked
//    solid angle's share.  Step 1 of the same function had always
//    spelled the gate `pShadingObject ? ...->DoesReceiveShadows() :
//    true`; the fix is to use that reading -- "a NULL means nobody
//    opted out", not "skip the visibility test" -- at all six sites.
//
//    HOW IT WAS FOUND, and the per-strategy numbers.  A compile-time
//    per-strategy tally on this scene (RGB, 16x16x4096) plus a 2-bit
//    A/B that can make SURFACE vertices and/or VOLUME vertices analog
//    (env-NEE off there, MIS weight forced to 1 on its partner):
//
//      mode              surf-NEE  vol-NEE  esc/surf  esc/vol  TOTAL
//      0 both MIS          0.1194   0.2356    0.5396   0.1673  1.0620
//      1 volume MIS only   0.0000   0.2370    0.6586   0.1679  1.0636
//      2 surface MIS only  0.1201   0.0000    0.5375   0.3357  0.9933
//      3 fully analog      0.0000   0.0000    0.6583   0.3349  0.9932
//
//    (vol-NEE and esc/vol pool the main loop's and the camera walk's
//    volume vertices.)  Modes 2 and 3 agreeing at 0.993 says the
//    transport and the SURFACE partition are both exact; mode 1 landing
//    +7 pp above mode 3 says the VOLUME partition is the whole defect.
//    In mode 1 the volume vertices' env-NEE reads 0.412 unweighted
//    against its partner's 0.298 -- two strategies for the same
//    integral disagreeing by +38 %, which is the blocked solid angle.
//    Post-fix all four modes agree at 0.993 and vol-NEE / esc-vol split
//    0.1488 / 0.1496, i.e. an exact half each, as w_nee = w_phase = 0.5
//    demands for an isotropic phase in a uniform environment.
//
//    Every previously-recorded symptom follows from this one cause:
//      - It vanished as sigma_s -> 0 (+6.20 % at 0.004, +1.16 % at
//        0.0005, -0.011 % at 1e-9) because that is the volume-vertex
//        count going to zero.
//      - It grew with the optical depth the BOUNCE rays traverse (floor
//        at y=-20 +6.25 %, at y=-80 +8.68 %, OUTSIDE the fog at y=-150
//        +9.41 %) because that is how many volume vertices sit where
//        the floor blocks a large solid angle.
//      - It was common-mode across RGB, NM and HWSS: all three reach
//        the same two LightSampler functions through
//        MediumTransport::EvaluateInScattering{,NM}.
//      - The surface env-NEE shadow ray really was medium-attenuated
//        (1.75M isVolumeScatter=false calls, mean Tr 0.604 = e^-0.5).
//        That instrumentation was correct and its refutation stands --
//        it measured the SURFACE calls, and the surface rows were never
//        the broken ones.
//      - Cells 4-6 closed at 1.0 throughout: with no geometry at all,
//        an unshadowed volume NEE is exactly right.  They still close
//        after the fix (the shadow ray they now cast can hit nothing),
//        which is this file's evidence that the fix is scoped to the
//        occluded case.
//
//    RED-PROVE, FIX 4.  Same tree, all six shadow gates restored to
//    `if( pShadingObject && pShadingObject->DoesReceiveShadows() )`:
//
//                                  reverted   fixed
//        RGB PT (cell 1)            0.99849   0.99777    unchanged
//        fog box RGB PT (cell 4)    0.99633   0.99522    unchanged
//        floor RGB PT (cell 7)      1.06205   0.99322    FAILS +2 %
//        floor hwss=false (cell 8)  1.06916   0.99794    FAILS +2 %
//        floor hwss=true  (cell 9)  1.02062   0.94900    borderline
//
//    3 of the file's 29 checks fail in that state.  Cells 1-6 are
//    unchanged to within MC noise, which is the evidence that the fix
//    touches only volume vertices that actually have geometry around
//    them.  Cell 9's +2.1 % here is only just over the cap and reads
//    +1.4 % on other runs, so it is not a dependable Fix-4 guard --
//    cells 7 and 8 are, at ~3x outside.
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
// CapturingRasterizerOutput -- same shape as EnvLightBalanceTest's /
// HairRenderTest's.  Deliberately does NOT override
// OutputPreDenoisedImage / OutputDenoisedImage; see the file header.
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
	double mean[3];
	double luminance;	// mean(mean[0..2])
	double minLum;
	double maxLum;
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	double sum[3] = { 0, 0, 0 };
	s.minLum = 1e30;
	s.maxLum = -1e30;
	for( const RISEColor& c : cap.pixels ) {
		sum[0] += c.base.r;
		sum[1] += c.base.g;
		sum[2] += c.base.b;
		const double lum = (c.base.r + c.base.g + c.base.b) / 3.0;
		s.minLum = std::fmin( s.minLum, lum );
		s.maxLum = std::fmax( s.maxLum, lum );
	}
	for( int c = 0; c < 3; c++ ) s.mean[c] = sum[c] / double(cap.pixels.size());
	s.luminance = (s.mean[0] + s.mean[1] + s.mean[2]) / 3.0;
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/volume_env_furnace_test_%s_%d.RISEscene",
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
// Scene assembly.
//
// The medium is a `painter_heterogeneous_medium` with a CONSTANT
// density painter rather than a `homogeneous_medium`, for one reason
// only: it is the sole medium chunk that carries an explicit AABB.  An
// unbounded global medium would make both the escape transmittance
// (EvalTransmittance over RISE_INFINITY) and every env shadow ray
// evaluate to exp(-sigma_t * DBL_MAX) == 0, and the whole env term
// would vanish rather than being MIS-weighted.  With a constant density
// of 1.0 the majorant equals the local extinction everywhere, so delta
// tracking takes no null collisions and the medium behaves exactly as a
// homogeneous one inside the box.
//
// There is deliberately NO geometry in the scene.  Any object the
// scattered ray could hit would route the continuation into
// IntegrateFromHitForTag (the main loop, whose env MIS was already
// correct) instead of the camera-ray escape branch this test guards.
//////////////////////////////////////////////////////////////////////

// Optical parameters -- see the file header for the derivation.
static const double kSigmaS      = 0.004;	// sigma_s == sigma_t (sigma_a = 0)
static const double kColumnHalfXY = 1.5;	// transverse half-extent of the fog column
static const double kColumnZNear = -1.0;	// camera sits at z = 0, inside
static const double kColumnZFar  = 100.0;
static const double kFovDegrees  = 1.0;		// keeps every ray inside the column

static std::string SceneCommon( unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 0\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov "
			<< kFovDegrees << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_density\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"painter_heterogeneous_medium\n{\n"
		"\tname furnace_fog\n"
		"\tabsorption 0.0 0.0 0.0\n"
		"\tscattering " << kSigmaS << " " << kSigmaS << " " << kSigmaS << "\n"
		"\tphase isotropic\n"
		"\tdensity_painter pnt_density\n"
		"\tresolution 4\n"
		"\tcolor_to_scalar luminance\n"
		"\tbbox_min " << -kColumnHalfXY << " " << -kColumnHalfXY << " " << kColumnZNear << "\n"
		"\tbbox_max " <<  kColumnHalfXY << " " <<  kColumnHalfXY << " " << kColumnZFar  << "\n"
		"}\n\n"
		"global_medium\n{\n\tmedium furnace_fog\n}\n\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// THE FOG-BOX FURNACE (cells 4-6).
//
// The thin column above was shaped to make the multiple-scatter term
// negligible, because at the time it was written that term was DROPPED
// by the camera-ray inline scatter path and would otherwise have
// cancelled against the MIS defect being measured.  With the camera
// walk now continuing (bounded by max_volume_bounce + RR, matching the
// main loop), a plain cubical fog box is finally a legitimate furnace,
// and it is the direct guard on that continuation: it is the geometry
// where the truncation is worth ~11 %, not ~0.4 %.
//
// A 200-unit cube centred on the camera at sigma_s = 0.004 gives
// tau = 0.4 along a face normal (0.69 into a corner).  sigma_a = 0, so
// the closed-form furnace answer is exactly 1.0 at EVERY pixel no
// matter what optical depth that pixel's ray sees -- an infinite
// uniform L = 1 field in equilibrium with a purely scattering medium.
// The truncated estimator instead reads
//
//   e^-tau + (1 - e^-tau) * E[Tr_escape]  ~= 0.89
//
// i.e. every path that scattered ONCE was allowed to escape but never
// to scatter AGAIN, so the (1 - e^-tau) fraction lost its own
// multiple-scatter tail.  40 degrees of field of view keeps a spread of
// optical depths in frame (centre vs corner), so a fix that only
// happened to work at one tau cannot pass.
//////////////////////////////////////////////////////////////////////
static const double kBoxHalfExtent = 100.0;	// tau = 0.4 from centre to a face
static const double kBoxFovDegrees = 40.0;

static std::string SceneCommonFogBox( unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 0\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov "
			<< kBoxFovDegrees << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_density\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"painter_heterogeneous_medium\n{\n"
		"\tname furnace_fog\n"
		"\tabsorption 0.0 0.0 0.0\n"
		"\tscattering " << kSigmaS << " " << kSigmaS << " " << kSigmaS << "\n"
		"\tphase isotropic\n"
		"\tdensity_painter pnt_density\n"
		"\tresolution 4\n"
		"\tcolor_to_scalar luminance\n"
		"\tbbox_min " << -kBoxHalfExtent << " " << -kBoxHalfExtent << " " << -kBoxHalfExtent << "\n"
		"\tbbox_max " <<  kBoxHalfExtent << " " <<  kBoxHalfExtent << " " <<  kBoxHalfExtent << "\n"
		"}\n\n"
		"global_medium\n{\n\tmedium furnace_fog\n}\n\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// THE FOG BOX WITH A FLOOR (cells 7-9).
//
// Cells 4-6 guard the CAMERA-RAY medium walk: the camera ray itself
// scatters, and the continuation never touches a surface.  They cannot
// reach the OTHER volumetric walk -- the one inside the shared bounce
// loop, entered when a ray that has already bounced off a SURFACE then
// scatters in a medium.  That loop is `IntegrateFromHitTemplated` for
// RGB/NM and `IntegrateFromHitHWSS` for the hero bundle, and until
// residual wave 4b the HWSS one did NEE at the volume-scatter vertex
// and then broke out of the loop -- the same dropped-continuation loss
// class wave 4 fixed on camera rays, one bounce further along.  Cells
// 4-6 stayed green through all of it, which is exactly why this scene
// exists.
//
// Construction: the same 200-unit fog box, with the camera pointed
// STRAIGHT DOWN at a perfectly white (rho = 1) Lambertian floor 20
// units below it.  tau on the camera segment is only 0.08, so ~92 % of
// camera rays reach the floor without scattering and enter the shared
// loop; the floor's cosine-sampled bounce then travels 80-200 units
// before leaving the box (tau 0.32-0.8), so roughly 40 % of those
// bounces scatter -- i.e. the majority of the image's energy flows
// through the site under test.
//
// It is still an exact furnace.  rho = 1 and sigma_a = 0 are both
// lossless, and the floor's normal is +Y so it integrates only the
// upper hemisphere, every direction of which sees L = 1 (directly or
// through the lossless fog).  So L_out = 1 at every point of the floor
// and 1 at every point of the medium: the closed-form answer is exactly
// 1.0 at every pixel, as in cells 1-6, and the same assertions apply
// unchanged.
//
// The floor spans +-100 in x and z, which is the fog box's own extent,
// so it is entirely inside the medium and the camera (fov 40, 20 units
// up) sees only its middle 15 units.
//////////////////////////////////////////////////////////////////////
static const double kFloorY        = -20.0;
static const double kFloorHalfXZ   = 100.0;
static const double kFloorFovDeg   = 40.0;

static std::string SceneCommonFogBoxFloor( unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		// Looking straight down (-Y); `up` must not be parallel to it.
		"pinhole_camera\n{\n\tlocation 0 0 0\n\tlookat 0 -1 0\n\tup 0 0 1\n\tfov "
			<< kFloorFovDeg << "\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_density\n\tcolor 1.0 1.0 1.0\n}\n\n"
		// rho = 1: lossless, which is what keeps the furnace exact.
		"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_white\n}\n\n"
		// Winding pta->ptb->ptc gives N = +Y, i.e. facing the camera.
		"clippedplane_geometry\n{\n\tname floor_quad\n"
		"\tpta " << -kFloorHalfXZ << " " << kFloorY << " " << -kFloorHalfXZ << "\n"
		"\tptb " << -kFloorHalfXZ << " " << kFloorY << " " <<  kFloorHalfXZ << "\n"
		"\tptc " <<  kFloorHalfXZ << " " << kFloorY << " " <<  kFloorHalfXZ << "\n"
		"\tptd " <<  kFloorHalfXZ << " " << kFloorY << " " << -kFloorHalfXZ << "\n"
		"}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry floor_quad\n\tmaterial mat_floor\n}\n\n"
		"painter_heterogeneous_medium\n{\n"
		"\tname furnace_fog\n"
		"\tabsorption 0.0 0.0 0.0\n"
		"\tscattering " << kSigmaS << " " << kSigmaS << " " << kSigmaS << "\n"
		"\tphase isotropic\n"
		"\tdensity_painter pnt_density\n"
		"\tresolution 4\n"
		"\tcolor_to_scalar luminance\n"
		"\tbbox_min " << -kBoxHalfExtent << " " << -kBoxHalfExtent << " " << -kBoxHalfExtent << "\n"
		"\tbbox_max " <<  kBoxHalfExtent << " " <<  kBoxHalfExtent << " " <<  kBoxHalfExtent << "\n"
		"}\n\n"
		"global_medium\n{\n\tmedium furnace_fog\n}\n\n";
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
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/volume_env_furnace_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string RasterizerPTSpectral( unsigned int samples, bool hwss )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_spectral_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
		"\thwss " << (hwss ? "true" : "false") << "\n"
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/volume_env_furnace_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string AssembleScene( const std::string& common, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + common + rasterizer;
}

//////////////////////////////////////////////////////////////////////
// The furnace assertions.
//
// TOLERANCES.  The mean band is expressed as an asymmetric pair
// (`loTol`, `hiTol`) around the closed-form furnace value of 1.0,
// because the two directions guard different things.
//
//   ABOVE unity is Fix 1's defect.  Both bands cap over-unity at 2 %.
//   Nothing in either scene can legitimately exceed 1.0: a sigma_a = 0
//   medium in equilibrium with a uniform L = 1 field is exactly 1.0, and
//   every bias still present can only LOSE energy (the bounce cap and
//   Russian roulette both truncate a tail; RR compensates in
//   expectation but never over-shoots the closed form on average).  The
//   Fix-1-reverted build reads +15.7 to +16.4 %, so the guard has ~8x
//   margin and cannot be satisfied by the bug it exists for.
//
//   BELOW unity is Fix 2's defect plus whatever structural residual
//   remains.  For RGB and hwss=false the floor is also 2 %, which
//   clears the worst measured deficit (0.38 %) by 5x while still
//   failing on the 2 %+ scale a genuinely lost transport term would
//   cost -- and the Fix-2-reverted fog box reads -12 %, six times the
//   floor.  hwss=true gets a 7 % floor instead -- it measures ~4.5 %
//   under for a documented, pre-existing, separately-red-proved reason
//   (file header) -- which still leaves ~1.5x headroom, still catches
//   an over-unity regression at the same 2 % as the others, and still
//   fails on the -15.8 % the truncating build gave it.
//
// PER-PIXEL BAND.  Deliberately loose (15 %) and deliberately NOT a
// precision check: at these sample counts the per-pixel spread is
// several percent of genuine MC noise (measured extremes in the file
// header), so a tight per-pixel band would be a flakiness generator.
// Its job is to catch a STRUCTURALLY broken frame -- black pixels, a
// wrongly-lit region, a half-frame discontinuity -- that a mean-level
// assertion could average away.
//////////////////////////////////////////////////////////////////////
static const double kFurnaceTolLo     = 0.02;	// how far BELOW 1.0 is allowed
static const double kFurnaceTolHi     = 0.02;	// how far ABOVE 1.0 is allowed
static const double kHwssFurnaceTolLo = 0.07;	// hwss=true only; see above
static const double kPixelSanityBand  = 0.15;

static ImageStats RunFurnaceCase(
	const char* label,
	const std::string& sceneText,
	const char* tag,
	const double loTol )
{
	const ImageStats s = RenderAndComputeStats( sceneText, tag );

	std::string validName = std::string( label ) + ": render produced output";
	Check( s.valid, validName.c_str() );
	if( !s.valid ) {
		return s;
	}

	std::cout << "  " << label
		<< ": mean = " << s.luminance
		<< "  (min " << s.minLum << ", max " << s.maxLum << ")"
		<< "  deviation from 1.0 = "
		<< ( (s.luminance - 1.0) * 100.0 ) << " %"
		<< "  [band -" << (loTol * 100.0) << " % .. +"
		<< (kFurnaceTolHi * 100.0) << " %]" << std::endl;

	std::string meanName = std::string( label ) + ": mean inside the furnace band around 1.0";
	Check( s.luminance >= 1.0 - loTol && s.luminance <= 1.0 + kFurnaceTolHi,
		   meanName.c_str() );

	std::string bandName = std::string( label ) + ": no pixel structurally off unity";
	Check( std::fabs( s.minLum - 1.0 ) <= kPixelSanityBand &&
		   std::fabs( s.maxLum - 1.0 ) <= kPixelSanityBand,
		   bandName.c_str() );
	return s;
}

//////////////////////////////////////////////////////////////////////
// The floor-scene assertions (cells 7-9): ABSOLUTE mean, one-sided
// per-pixel, plus two cross-variant RATIOS.
//
// WHY THE MEAN IS NOW ASSERTED.  Until residual wave 4c these three
// cells read ~+6 % over unity and could only assert ratios, which the
// common-mode excess cancelled out of.  Fix 4 (the unshadowed
// volume-vertex NEE -- see the file header) removed that excess, so the
// floor scene is now an exact furnace like cells 1-6 and its mean is
// asserted directly.  Measured over 12 consecutive runs:
//
//     cell 7  floor RGB PT              0.9922 .. 0.9950   (-0.50..-0.78 %)
//     cell 8  floor spectral hwss=false 0.9950 .. 1.0005   (-0.50..+0.05 %)
//     cell 9  floor spectral hwss=true  0.9479 .. 0.9560   (-4.40..-5.21 %)
//
// BANDS, DERIVED.
//   ABOVE unity is capped at kFurnaceTolHi (2 %) for all three, exactly
//   as cells 1-6: nothing in a sigma_a = 0, rho = 1 furnace can exceed
//   1.0, so any over-unity reading is a double-count.  This is the Fix-4
//   guard -- with the shadow gates reverted, cell 7 reads 1.0621 and
//   cell 8 reads 1.0692, both ~3x outside it (see RED-PROVE, FIX 4).
//
//   BELOW unity, cells 7 and 8 take the same kFurnaceTolLo (2 %) as
//   cells 1-6: the worst measured deficit is 0.78 %, so the floor has
//   ~2.6x headroom, and the wave-4b revert drops cell 9 to 0.726, which
//   is 3.0x outside its own band.
//
//   Cell 9 takes its own kFloorHwssTolLo (9 %) rather than the
//   kHwssFurnaceTolLo (7 %) cells 3 and 6 use.  It sits ~0.7 pp deeper
//   than those two (-5.2 % vs -4.5 %) for the same reason they sit below
//   unity at all -- the pre-existing spectral-bundle env deficit -- and
//   this scene runs more bounces per path, so it has a wider run-to-run
//   spread (0.81 pp over 12 runs vs ~0.25 pp for cell 6).  9 % leaves
//   3.8 pp of headroom, ~4.7 spread-widths, and still fails the wave-4b
//   revert (0.726, 3.0x outside).  Cell 9 is NOT a dependable Fix-4
//   guard: its pre-Fix-4 reading is only +1.4 to +2.1 % across runs --
//   it straddles the +2 % cap -- because the hero bundle's own -5 %
//   deficit masks most of the over-count.  Cells 7 and 8 carry that
//   job, at +6.2 % and +6.9 %.
//
// THE RATIOS ARE KEPT, and are the sharper of the two guards for Fix 3.
//   hwss=false / RGB: measured 1.0015 .. 1.0074 over 12 runs.  Band
//   [0.98, 1.02] -- the NM tag walks the SAME templated loop as the Pel
//   tag, so anything outside a couple of percent means the two tags
//   diverged.
//
//   hwss=true / RGB: measured 0.9535 .. 0.9577 over 12 runs.  The
//   reference is NOT 1.0: the hero bundle carries the documented
//   pre-existing spectral-bundle env deficit, which the no-geometry
//   cells measure independently at 0.959 (cell 6 / cell 4) and 0.958
//   (cell 3 / cell 1).  The floor scene lands on the same value, which
//   is the actual claim being asserted: HWSS loses nothing to the
//   surface-bounce walk beyond the bundle deficit it already had.  Band
//   [0.93, 1.05]: 2.4 % of headroom below; the top was originally 1.00
//   but a 36-render baseline sweep (wave 5, HEAD 561cf6d5, fix absent
//   AND present -- indistinguishable) observed the ratio span 0.954 ..
//   1.036, the heavy hwss tail documented below occasionally lifting a
//   whole 16x16 frame's mean, so 1.00 flaked ~1 run in 12.  1.05 keeps
//   the guard (the truncating build reads 0.683, far below the unchanged
//   0.93 floor -- the guard is low-side and did not move; a genuine
//   hwss over-count would have to beat the bundle deficit by >9 %).
//
// PER-PIXEL, AND WHY IT IS ONE-SIDED HERE.  Expressed against the
// cell's OWN mean rather than against 1.0, and only the LOW side is
// asserted.  Cells 1-6 render flat frames and can bound both sides at
// 15 %; this scene cannot bound the high side at any useful number.
// The floor's bounce rays can run nearly parallel to the floor and
// traverse up to tau ~ 1.6 before leaving the box, and the escape
// weight is Tr / pSurvival with pSurvival = e^-tau, so a surviving
// long-tau escape legitimately carries a weight of several.  Measured
// max/mean on cell 9 over 12 post-fix runs: 1.09 .. 1.79 (median
// ~1.16, worst 1.79) -- a genuinely heavy tail, and one the RGB
// cell does not show (1.04 to 1.05) because its per-channel weights
// stay correlated where the hero bundle's per-wavelength walks do not.
// It is VARIANCE, not bias: the cell-9 mean over those same 12 runs is
// stable to +-0.4 %.  A max-side cap that tolerated 1.8x would not be
// much of a guard, so the high side is left to the mean band and the
// ratios -- a NaN, an infinity or a runaway pixel moves the mean and
// fails those instead.  The low side still does its original job:
// black pixels or a wrongly-lit region.
//////////////////////////////////////////////////////////////////////
static const double kRelBandNoHwssLo = 0.98;
static const double kRelBandNoHwssHi = 1.02;
static const double kRelBandHwssLo   = 0.93;
static const double kRelBandHwssHi   = 1.05;
//! Floor-scene per-pixel structural LOW bound, relative to the cell's
//! own mean.  Worst measured over 12 post-fix runs: min/mean 0.911
//! (cell 8).  0.80 gives ~1.5x headroom.  There is deliberately no high
//! bound -- see the block above.
static const double kFloorPixelLo     = 0.80;
//! Cell 9's own below-unity floor; see "BANDS, DERIVED" above for why
//! it is wider than kHwssFurnaceTolLo.
static const double kFloorHwssTolLo   = 0.09;

static ImageStats RunFloorCase(
	const char* label,
	const std::string& sceneText,
	const char* tag,
	const double loTol )
{
	const ImageStats s = RenderAndComputeStats( sceneText, tag );

	std::string validName = std::string( label ) + ": render produced output";
	Check( s.valid, validName.c_str() );
	if( !s.valid ) {
		return s;
	}

	std::cout << "  " << label
		<< ": mean = " << s.luminance
		<< "  (min " << s.minLum << ", max " << s.maxLum << ")"
		<< "  deviation from 1.0 = "
		<< ( (s.luminance - 1.0) * 100.0 ) << " %"
		<< "  [band -" << (loTol * 100.0) << " % .. +"
		<< (kFurnaceTolHi * 100.0) << " %]" << std::endl;

	std::string meanName = std::string( label ) + ": mean inside the furnace band around 1.0";
	Check( s.luminance >= 1.0 - loTol && s.luminance <= 1.0 + kFurnaceTolHi,
		   meanName.c_str() );

	std::string bandName = std::string( label ) + ": no pixel structurally dark vs this cell's own mean";
	Check( s.luminance > 0.0 &&
		   s.minLum >= kFloorPixelLo * s.luminance,
		   bandName.c_str() );
	return s;
}

static void CheckFloorRatio(
	const char* label,
	const ImageStats& variant,
	const ImageStats& reference,
	const double lo,
	const double hi )
{
	if( !variant.valid || !reference.valid || reference.luminance <= 0.0 ) {
		Check( false, ( std::string( label ) + ": ratio computable" ).c_str() );
		return;
	}
	const double ratio = variant.luminance / reference.luminance;
	std::cout << "    " << label << " / floor RGB = " << ratio
		<< "  [band " << lo << " .. " << hi << "]" << std::endl;
	Check( ratio >= lo && ratio <= hi,
		   ( std::string( label ) + ": ratio to the RGB floor reference inside band" ).c_str() );
}

// 24x24 at 4096 spp is ~1.6 s per variant here (~5 s for the file).
// The frame is uniform by construction -- every pixel has the same
// optical depth -- so resolution buys nothing but wall time; the sample
// count is what converges the mean.
static const unsigned int kW = 24, kH = 24, kSamples = 4096;

static void TestVolumeEnvFurnaceRGB()
{
	std::cout << "=== 1. Camera-in-medium env furnace -- RGB PT ===" << std::endl;
	RunFurnaceCase( "RGB PT",
		AssembleScene( SceneCommon( kW, kH ), RasterizerPTRgb( kSamples ) ),
		"rgb", kFurnaceTolLo );
}

static void TestVolumeEnvFurnaceSpectral()
{
	std::cout << "=== 2. Camera-in-medium env furnace -- spectral PT, hwss=false ===" << std::endl;
	RunFurnaceCase( "spectral hwss=false",
		AssembleScene( SceneCommon( kW, kH ), RasterizerPTSpectral( kSamples, false ) ),
		"spec", kFurnaceTolLo );
}

static void TestVolumeEnvFurnaceHwss()
{
	std::cout << "=== 3. Camera-in-medium env furnace -- spectral PT, hwss=true ===" << std::endl;
	RunFurnaceCase( "spectral hwss=true",
		AssembleScene( SceneCommon( kW, kH ), RasterizerPTSpectral( kSamples, true ) ),
		"hwss", kHwssFurnaceTolLo );
}

// The fog box scatters far more than the thin column (every path can
// take several bounces before escaping), so it costs more per sample
// AND has a wider per-pixel spread.  4096 spp on 16x16 is ~1.3 s per
// variant here and lands the per-pixel extremes near +-5 %, i.e. a
// third of kPixelSanityBand; at 1024 spp the extremes reached +12 %,
// which is close enough to that band to be a flakiness generator.
static const unsigned int kBoxW = 16, kBoxH = 16, kBoxSamples = 4096;

static void TestFogBoxFurnaceRGB()
{
	std::cout << "=== 4. Cubical fog-box furnace -- RGB PT ===" << std::endl;
	RunFurnaceCase( "fog box RGB PT",
		AssembleScene( SceneCommonFogBox( kBoxW, kBoxH ), RasterizerPTRgb( kBoxSamples ) ),
		"boxrgb", kFurnaceTolLo );
}

static void TestFogBoxFurnaceSpectral()
{
	std::cout << "=== 5. Cubical fog-box furnace -- spectral PT, hwss=false ===" << std::endl;
	RunFurnaceCase( "fog box spectral hwss=false",
		AssembleScene( SceneCommonFogBox( kBoxW, kBoxH ), RasterizerPTSpectral( kBoxSamples, false ) ),
		"boxspec", kFurnaceTolLo );
}

static void TestFogBoxFurnaceHwss()
{
	std::cout << "=== 6. Cubical fog-box furnace -- spectral PT, hwss=true ===" << std::endl;
	RunFurnaceCase( "fog box spectral hwss=true",
		AssembleScene( SceneCommonFogBox( kBoxW, kBoxH ), RasterizerPTSpectral( kBoxSamples, true ) ),
		"boxhwss", kHwssFurnaceTolLo );
}

// The floor scene runs more bounces per path than either scene above
// (surface bounce, then a volumetric walk, then possibly the floor
// again), so it is the slowest of the three at equal spp.  Same 16x16 /
// 4096 spp as the fog box.
static const unsigned int kFloorW = 16, kFloorH = 16, kFloorSamples = 4096;

static void TestFloorFurnace()
{
	std::cout << "=== 7. Fog box + white floor (surface bounce -> medium) -- RGB PT ===" << std::endl;
	const ImageStats rgb = RunFloorCase( "floor RGB PT",
		AssembleScene( SceneCommonFogBoxFloor( kFloorW, kFloorH ), RasterizerPTRgb( kFloorSamples ) ),
		"floorrgb", kFurnaceTolLo );

	std::cout << "=== 8. Fog box + white floor -- spectral PT, hwss=false ===" << std::endl;
	const ImageStats nohwss = RunFloorCase( "floor spectral hwss=false",
		AssembleScene( SceneCommonFogBoxFloor( kFloorW, kFloorH ), RasterizerPTSpectral( kFloorSamples, false ) ),
		"floorspec", kFurnaceTolLo );
	CheckFloorRatio( "floor spectral hwss=false", nohwss, rgb,
		kRelBandNoHwssLo, kRelBandNoHwssHi );

	std::cout << "=== 9. Fog box + white floor -- spectral PT, hwss=true ===" << std::endl;
	const ImageStats hwss = RunFloorCase( "floor spectral hwss=true",
		AssembleScene( SceneCommonFogBoxFloor( kFloorW, kFloorH ), RasterizerPTSpectral( kFloorSamples, true ) ),
		"floorhwss", kFloorHwssTolLo );
	CheckFloorRatio( "floor spectral hwss=true", hwss, rgb,
		kRelBandHwssLo, kRelBandHwssHi );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "VolumeEnvFurnaceTest -- camera-in-medium environment MIS partition regression" << std::endl;

	TestVolumeEnvFurnaceRGB();
	TestVolumeEnvFurnaceSpectral();
	TestVolumeEnvFurnaceHwss();
	TestFogBoxFurnaceRGB();
	TestFogBoxFurnaceSpectral();
	TestFogBoxFurnaceHwss();
	TestFloorFurnace();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
