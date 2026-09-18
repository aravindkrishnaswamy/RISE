//////////////////////////////////////////////////////////////////////
//
//  SpectralSplatActiveCountTest.cpp - Red-proof and regression guard
//    for DL-201: the spectral BDPT/VCM t==1 light-tracing SPLAT deposit
//    was not renormalized for HWSS companion-wavelength termination,
//    while `SplatFilm::Resolve` divides every pixel by the FIXED global
//    `nSpectralSamples * SampledWavelengths::N`.
//
//  THE DEFECT.  Under HWSS, `BDPTSpectralRasterizer`/`VCMSpectralRasterizer`
//  call `swl.TerminateSecondary()` whenever a bundle's subpaths contain a
//  dispersive delta vertex or a null-BSDF continuation vertex (DL-126), and
//  the companion loop then correctly SKIPS those lanes' splat deposits.  The
//  non-splat (per-pixel) path compensates by dividing by `totalActive`; the
//  splat path had no such compensation, because `SplatFilm::Resolve` applies
//  ONE global scalar to the whole film.  A terminated bundle therefore
//  deposited one lane's worth of splat energy where the denominator had
//  reserved `SampledWavelengths::N` -- a factor `1/N` = 0.25 deficit on any
//  pixel reached predominantly through terminated bundles.  Because
//  termination is driven by (among other things) the LIGHT subpath's own
//  content, and the light subpath is also what determines WHERE its splat
//  lands, the deficit is SPATIALLY CORRELATED: a caustic cast through a
//  dispersive object darkens while the rest of the frame does not.
//
//  THE INSTRUMENT.  A floor lit by one small emitter through TWO horizontal
//  glass slabs, one over each half of the light's downward cone:
//
//      * LEFT slab  (x in [-3.0, -0.4]): DISPERSIVE (Sellmeier BK7 `ior`).
//        Every light subpath that reaches the floor's left region crosses it,
//        so `HasDispersiveDeltaVertex` fires and that bundle's companions
//        terminate.
//      * RIGHT slab (x in [+0.4, +3.0]): NON-dispersive (`ior 1.5168`, the
//        same glass at its own d-line index).  Geometrically identical, same
//        Fresnel losses, same delta vertices -- but `HasDispersiveDeltaVertex`
//        compares the IOR at hero vs companion wavelength and finds no
//        difference, so nothing terminates.
//
//  The two regions are therefore a matched pair differing ONLY in whether
//  their bundles terminate, which is exactly the variable DL-201 is about.
//  The camera sits at (0, 2.5, 6) looking at the origin: every camera ray and
//  every splat connection ray crosses the slabs' plane (y = 1.5) at
//  z = 3.6 + 0.4*z_floor, i.e. z >= 2.4, outside both slabs' z in [-2, 2]
//  extent -- so neither slab occludes or refracts the view of either region.
//
//  `max_eye_depth 0` truncates the eye subpath to the camera vertex alone, so
//  only t==1 strategies survive and THE WHOLE IMAGE IS THE SPLAT FILM.  That
//  is the instrument that isolates the film's denominator; it is not the only
//  configuration the defect reaches, which is why the VCM rows below ALSO run
//  at an ordinary `max_eye_depth 5` (VCM's t==1 strategy carries enough of the
//  caustic there for the defect to remain plainly visible at production
//  settings -- see the measured table).
//
//  MEASURED (32x32, hwss FALSE at 512 spp vs hwss TRUE at 128 spp; the
//  hero-only render gets 4x the samples because an HWSS bundle carries
//  `SampledWavelengths::N` wavelengths per path.  Achromatic mean per column
//  OCTANT, ratio hwss TRUE / hwss FALSE.  Octants 0-1 are the DISPERSIVE
//  half, 6-7 the non-dispersive half):
//
//    row                                pre-fix oct0/oct1   post-fix oct0/oct1
//    BDPT eye0  dispersive+flat slabs    0.2386 / 0.2387     0.9959 / 0.9553
//    BDPT eye0  both slabs flat (ctrl)   0.9697 / 0.9508     0.9573 / 0.9530
//    VCM  eye0  dispersive+flat slabs    0.2445 / 0.2414     0.9966 / 0.9519
//    VCM  eye5  dispersive+flat slabs    0.2834 / 0.3017     0.9925 / 0.9838
//    VCM  eye5  both slabs flat (ctrl)   0.9764 / 0.9765     0.9807 / 0.9859
//
//  The pre-fix 0.2386 is `1/SampledWavelengths::N` = 0.25 times the control
//  row's own ~0.95 hwss-TRUE/FALSE baseline (0.2375 predicted) -- i.e. the
//  measured deficit is EXACTLY the predicted one, because essentially 100% of
//  the bundles depositing in that region are dispersion-terminated.
//
//  WHY THE FIX IS AT THE DEPOSIT SITE AND NOT IN `SplatFilm`.  DL-201's own
//  recipe proposed a per-pixel weight accumulator in `SplatFilm`, summed at
//  deposit time.  That is not a correct denominator for a splat estimator: a
//  light subpath may land at ANY pixel, so every lane that ran is a sample
//  "for" every pixel, and the ones that happened to deposit at a given pixel
//  are a biased subset of them.  Accumulating weight only where a deposit
//  landed computes a mean over CONTRIBUTING samples and over-brightens sparse
//  splat regions without bound.  (`SplatPixel::weight` already counts deposits
//  for precisely this reason and is deliberately consumed only as a nonzero
//  flag.)  Renormalizing each BUNDLE at its own deposit site is both exact and
//  strictly finer-grained: termination is a per-bundle property, so correcting
//  it before the energy is pooled removes the spatial correlation entirely.
//
//  THE CONTROL ROWS MATTER.  Both `both slabs flat` rows read ~0.95-0.98 both
//  before and after -- an hwss TRUE vs FALSE baseline offset that has nothing
//  to do with this row (the two configurations are different estimators at
//  different sample counts).  Gate against that baseline, not against 1.0.
//
//  MLT is NOT affected and has no row here: `MLTSpectralRasterizer` scales
//  every strategy -- splat and non-splat alike -- by `1/activeWavelengthCount`
//  before building its `MLTSample`s (DL-126 review round 4, P1-1), and it
//  never consults `GetSplatSampleScale()`.  The Pel (RGB) rasterizers are
//  unaffected by construction: `BidirectionalRasterizerBase::GetSplatSampleScale`
//  returns 1.0 for them and they have no wavelength bundle at all.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 18, 2026
//  Tabs: 4
//  Comments:
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

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput -- see BDPTStrategyBalanceTest.cpp for the
// rationale; duplicated per this codebase's convention of each test
// .cpp being a self-contained translation unit.
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

//////////////////////////////////////////////////////////////////////
// Column-octant statistics.  The `max_eye_depth 0` rows produce a pure
// splat image whose per-pixel ALPHA is meaningless (no eye-side sample
// ever covers the pixel), so read `base` directly rather than weighting
// by coverage the way the beauty-image tests do.
//////////////////////////////////////////////////////////////////////
static const int kNumOctants = 8;

struct RegionStats
{
	double achro[kNumOctants];		///< achromatic mean per column octant
	double whole;
	bool   valid;
};

static RegionStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	RegionStats s{};
	if( cap.pixels.empty() || cap.width == 0 || cap.height == 0 ) {
		return s;
	}

	double sum[kNumOctants] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	double cnt[kNumOctants] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	double total = 0;
	for( unsigned int y = 0; y < cap.height; y++ ) {
		for( unsigned int x = 0; x < cap.width; x++ ) {
			const RISEColor& c = cap.pixels[y * cap.width + x];
			const double a = ( c.base.r + c.base.g + c.base.b ) / 3.0;
			if( !std::isfinite( a ) ) {
				return RegionStats{};
			}
			const unsigned int oct = ( x * kNumOctants ) / cap.width;
			sum[oct] += a;
			cnt[oct] += 1.0;
			total += a;
		}
	}
	for( int i = 0; i < kNumOctants; i++ ) {
		s.achro[i] = cnt[i] > 0 ? sum[i] / cnt[i] : 0.0;
	}
	s.whole = total / double( cap.pixels.size() );
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/spectral_splat_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static RegionStats RenderAndComputeStats( const char* scenePath )
{
	RegionStats result{};

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	// Fresh libc seed per render.  Worker scheduling still makes repeats
	// non-bit-reproducible (BlockRasterizeSequence shuffles from
	// std::random_device) -- see the rise-render-seeding memory note.
	static unsigned renderIndex = 0;
	std::srand( 9201u + renderIndex++ );
	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = ComputeStats( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

static void PrintStats( const char* label, const RegionStats& s )
{
	if( !s.valid ) {
		std::cout << "    " << label << ": INVALID (render failed)" << std::endl;
		return;
	}
	std::cout << "    " << label << ": whole=" << s.whole << "  octants=[";
	for( int i = 0; i < kNumOctants; i++ ) {
		std::cout << s.achro[i] << ( i < kNumOctants - 1 ? ", " : "" );
	}
	std::cout << "]" << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Scene body.  See the file header for the geometry rationale and for
// why the camera never looks through either slab.
//////////////////////////////////////////////////////////////////////

static std::string SceneBody( bool leftDispersive, bool rightDispersive )
{
	std::string s =
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n"
		"\tlocation 0 2.5 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n"
		"}\n\n"
		// ---- floor (the receiver the splats land on) ---------------
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.6 0.6 0.6\n}\n\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_floor\n"
		"\tpta -3 0 -3\n\tptb 3 0 -3\n\tptc 3 0 3\n\tptd -3 0 3\n}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry geo_floor\n\tmaterial mat_floor\n}\n\n"
		// ---- emitter, small, above the origin, facing down ---------
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_emit\n"
		"\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_emit\n"
		"\tpta -0.2 3 -0.2\n\tptb 0.2 3 -0.2\n\tptc 0.2 3 0.2\n\tptd -0.2 3 0.2\n}\n\n"
		"standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n"
		// ---- the two slabs -----------------------------------------
		// `sellmeier` is BK7 (n ~ 1.5168 at the d line, dn ~ 0.018 across
		// the visible) -- far above HasDispersiveDeltaVertex's own 1e-6
		// IOR-difference threshold.  `pnt_ior_flat` is that same glass
		// pinned to its d-line index, so the two slabs are optically
		// near-identical and differ only in whether HWSS terminates.
		// `scattering 1000000` is the delta pass-through spelling (a
		// non-delta transmission lobe would not be a `isDelta` vertex
		// and HasDispersiveDeltaVertex would never see it at all).
		"scalar_painter\n{\n\tname pnt_tau\n\tvalue 1.0\n}\n\n"
		"scalar_painter\n{\n\tname pnt_ior_disp\n"
		"\tsellmeier 1.03961212 0.231792344 1.01046945 0.00600069867 0.0200179144 103.560653\n"
		"}\n\n"
		"scalar_painter\n{\n\tname pnt_ior_flat\n\tvalue 1.5168\n}\n\n"
		"box_geometry\n{\n\tname geo_slab\n"
		"\twidth 2.6\n\theight 0.2\n\tdepth 4.0\n}\n\n";

	for( int side = 0; side < 2; side++ )
	{
		const bool disp = ( side == 0 ) ? leftDispersive : rightDispersive;
		const char* nm  = ( side == 0 ) ? "L" : "R";
		const double px = ( side == 0 ) ? -1.7 : 1.7;
		char buf[768];
		std::snprintf( buf, sizeof(buf),
			"dielectric_material\n{\n\tname mat_slab%s\n"
			"\tior %s\n\ttau pnt_tau\n\tscattering 1000000\n}\n\n"
			"standard_object\n{\n\tname obj_slab%s\n\tgeometry geo_slab\n"
			"\tmaterial mat_slab%s\n\tposition %.2f 1.5 0\n}\n\n",
			nm, disp ? "pnt_ior_disp" : "pnt_ior_flat", nm, nm, px );
		s += buf;
	}

	return s;
}

//////////////////////////////////////////////////////////////////////
// Rasterizer fragments.
//////////////////////////////////////////////////////////////////////

static std::string BdptRasterizer( bool hwss, unsigned int samples, unsigned int eyeDepth )
{
	char buf[1024];
	std::snprintf( buf, sizeof(buf),
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"bdpt_spectral_rasterizer\n{\n"
		"\tmax_eye_depth %u\n\tmax_light_depth 5\n"
		"\tsamples %u\n\thwss %s\n"
		"\tpixel_filter box\n\toidn_denoise FALSE\n"
		"}\n\n",
		eyeDepth, samples, hwss ? "TRUE" : "FALSE" );
	return std::string( buf );
}

static std::string VcmRasterizer( bool hwss, unsigned int samples, unsigned int eyeDepth )
{
	char buf[1024];
	std::snprintf( buf, sizeof(buf),
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"vcm_spectral_rasterizer\n{\n"
		"\tmax_eye_depth %u\n\tmax_light_depth 5\n"
		"\tsamples %u\n\thwss %s\n"
		"\tvc_enabled true\n\tvm_enabled false\n"
		"\tpixel_filter box\n\toidn_denoise FALSE\n"
		"}\n\n",
		eyeDepth, samples, hwss ? "TRUE" : "FALSE" );
	return std::string( buf );
}

//////////////////////////////////////////////////////////////////////
// The ladder.  `loBand`/`hiBand` gate EVERY octant's hwss TRUE / hwss
// FALSE ratio.  The band is deliberately wide (0.85 - 1.15) because the
// two configurations are different estimators at different sample
// counts and the control rows themselves sit at ~0.95; it is still two
// and a half times tighter than the pre-fix 0.24 the dispersive rows
// measured, which is what makes it a real red-proof rather than a
// consistency pin.
//////////////////////////////////////////////////////////////////////
static void RunLadder( const std::string& label, const std::string& rastNo,
	const std::string& rastHW, const std::string& body,
	double loBand, double hiBand )
{
	std::cout << "Testing " << label << std::endl;

	const std::string sceneNo = std::string("RISE ASCII SCENE 7\n") + rastNo + body;
	const std::string sceneHW = std::string("RISE ASCII SCENE 7\n") + rastHW + body;

	const std::string pathNo = WriteSceneToTempFile( sceneNo.c_str(), "nohwss" );
	const std::string pathHW = WriteSceneToTempFile( sceneHW.c_str(), "hwss" );
	if( pathNo.empty() || pathHW.empty() ) {
		Check( false, label + ": temp file write" );
		return;
	}

	const RegionStats no = RenderAndComputeStats( pathNo.c_str() );
	const RegionStats hw = RenderAndComputeStats( pathHW.c_str() );

	PrintStats( "hwss FALSE", no );
	PrintStats( "hwss TRUE ", hw );

	std::remove( pathNo.c_str() );
	std::remove( pathHW.c_str() );

	Check( no.valid, label + ": hwss FALSE render produced output" );
	Check( hw.valid, label + ": hwss TRUE render produced output" );
	if( !no.valid || !hw.valid ) {
		return;
	}

	// The scene must actually be lit, or every ratio below is vacuous.
	Check( no.whole > 1e-9, label + ": reference image is non-black" );
	if( no.whole <= 1e-9 ) {
		return;
	}

	std::cout << "    ratios per octant: [";
	for( int i = 0; i < kNumOctants; i++ ) {
		const double r = no.achro[i] > 1e-12 ? hw.achro[i] / no.achro[i] : -1.0;
		std::cout << r << ( i < kNumOctants - 1 ? ", " : "" );
	}
	std::cout << "]" << std::endl;

	const double wholeRatio = hw.whole / no.whole;
	std::cout << "    whole ratio = " << wholeRatio << std::endl;

	for( int i = 0; i < kNumOctants; i++ ) {
		// An octant with no reference energy carries no information;
		// every octant of every row here is lit well above this floor,
		// but guard rather than divide by a near-zero.
		if( no.achro[i] <= 1e-12 ) {
			continue;
		}
		const double r = hw.achro[i] / no.achro[i];
		char nameBuf[256];
		std::snprintf( nameBuf, sizeof(nameBuf),
			"%s: octant %d hwss ratio %.4f in [%.2f, %.2f]",
			label.c_str(), i, r, loBand, hiBand );
		Check( r >= loBand && r <= hiBand, nameBuf );
	}

	char wholeBuf[256];
	std::snprintf( wholeBuf, sizeof(wholeBuf),
		"%s: whole-image hwss ratio %.4f in [%.2f, %.2f]",
		label.c_str(), wholeRatio, loBand, hiBand );
	Check( wholeRatio >= loBand && wholeRatio <= hiBand, wholeBuf );
}

int main()
{
	std::cout << "=== SpectralSplatActiveCountTest (DL-201) ===" << std::endl;

	// ---- the money rows: one half's bundles terminate, the other's
	// do not, and both halves must read the same hwss baseline.
	RunLadder( "BDPT spectral pure-splat (eye0), dispersive + flat slabs",
		BdptRasterizer( false, 512, 0 ), BdptRasterizer( true, 128, 0 ),
		SceneBody( true, false ), 0.85, 1.15 );

	RunLadder( "VCM spectral pure-splat (eye0), dispersive + flat slabs",
		VcmRasterizer( false, 512, 0 ), VcmRasterizer( true, 128, 0 ),
		SceneBody( true, false ), 0.85, 1.15 );

	// ---- the same defect at an ORDINARY eye depth.  BDPT's own t==1
	// share is too small at eye depth 5 on this scene for the deficit to
	// clear the noise floor, so only VCM carries a production-settings
	// row; the eye0 BDPT row above is the one that isolates the film.
	RunLadder( "VCM spectral eye5 (production depth), dispersive + flat slabs",
		VcmRasterizer( false, 512, 5 ), VcmRasterizer( true, 128, 5 ),
		SceneBody( true, false ), 0.85, 1.15 );

	// ---- controls: geometrically identical scenes whose slabs are both
	// NON-dispersive, so nothing terminates and the splat denominator was
	// never wrong.  Green before AND after the fix -- they establish the
	// ~0.95 hwss TRUE/FALSE baseline the money rows are read against, and
	// they prove the money rows' signal is termination and not "a render
	// with glass in it".
	RunLadder( "CONTROL BDPT spectral pure-splat (eye0), both slabs flat",
		BdptRasterizer( false, 512, 0 ), BdptRasterizer( true, 128, 0 ),
		SceneBody( false, false ), 0.85, 1.15 );

	RunLadder( "CONTROL VCM spectral eye5, both slabs flat",
		VcmRasterizer( false, 512, 5 ), VcmRasterizer( true, 128, 5 ),
		SceneBody( false, false ), 0.85, 1.15 );

	std::cout << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
