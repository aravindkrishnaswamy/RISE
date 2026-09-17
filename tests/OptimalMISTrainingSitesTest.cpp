//////////////////////////////////////////////////////////////////////
//
//  OptimalMISTrainingSitesTest.cpp - DL-72 integration guard: the two
//    continuation sites the DL-72 arc has repeatedly wired, unwired and
//    rewired -- RayCaster's internal VOLUME phase-scatter continuation
//    and PathTracingIntegrator's BSSRDF exit continuation -- must
//    actually train `OptimalMISAccumulator`'s BSDF technique when they
//    escape to the environment, with a COUNT and a MOMENT, not one
//    without the other.
//
//  WHY A NEW TEST
//
//    `OptimalMISAccumulatorTest`'s Test 12 drives the accumulator's
//    public API by hand.  That pins the accumulator's own arithmetic --
//    an unpaired `Accumulate` really does inflate `Mbsdf` and depress
//    alpha -- but it does NOT execute one line of `RayCaster.cpp` or
//    `PathTracingIntegrator.cpp`, so it cannot tell whether those files
//    call the API at all.  Both DL-72 regressions were exactly that:
//    round 1 called `Accumulate` with no `AccumulateCount`; round 2
//    then removed both.  A hand-driven accumulator test is blind to
//    either.  This file closes that gap by running the REAL sites and
//    reading the accumulator's own verdict back out.
//
//  HOW "COUNT AND MOMENT BOTH ADVANCED" IS OBSERVED
//
//    `OptimalMISAccumulator` exposes no per-tile counters, but
//    `Solve()`'s branch structure is already a complete decision table
//    over exactly the two quantities in question, and `GetAlpha()`
//    reports which branch was taken:
//
//      alpha == 0.5        neither technique reached minSamplesPerTile,
//                          or neither accumulated any moment
//      alpha == clampMin   BSDF short of attempts (NO COUNT), or enough
//                          attempts with zero moment (NO MOMENT)
//      alpha == clampMax   NEE short of attempts / moment, OR the solved
//                          Mbsdf is so small beside Mnee that the ratio
//                          saturates the clamp
//      strictly interior   BOTH techniques cleared minSamplesPerTile,
//                          BOTH accumulated a positive moment, and the
//                          BSDF moment is a comparable quantity rather
//                          than a residual
//
//    So an interior alpha, with the clamps pushed out to 0.001 / 0.999 so
//    the interior band is unmistakable, IS the assertion "the count and
//    the moment both advanced at this site".  Each case additionally
//    reports a fresh, untouched accumulator (0.5) as the "nothing
//    trained" reference the measured value has to differ from.
//
//    Measured with the two files reverted to ddf05c6c -- DL-72's round-2
//    "leave the arm UNWIRED" state, where the volume site contributed
//    neither a count nor a moment and the BSSRDF site contributed no
//    moment: the volume case reads exactly clampMin and the BSSRDF case
//    exactly clampMax.  Both are interior with the sites wired.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <thread>

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Utilities/OptimalMISAccumulator.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/PerfectReflectorMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Lights/LightSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* what )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << what << std::endl;
	}
}

class NullRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
protected:
	virtual ~NullRasterizerOutput() {}
public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override {}
};

struct Fixture
{
	IJobPriv* pJob;
	IRayCaster* pCaster;
	const IScene* pScene;
	std::string scenePath;

	Fixture() : pJob( 0 ), pCaster( 0 ), pScene( 0 ) {}

	bool Build( const std::string& sceneText, const char* tag )
	{
		char path[512];
		std::snprintf( path, sizeof(path), "/tmp/optmis_sites_%s_%d.RISEscene",
			tag, static_cast<int>( ::getpid() ) );
		scenePath = path;
		{
			std::ofstream ofs( path );
			if( !ofs.is_open() ) return false;
			ofs << sceneText;
		}
		if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return false;
		if( !pJob->LoadAsciiSceneAuto( scenePath.c_str() ) ) return false;

		pJob->RemoveRasterizerOutputs();
		NullRasterizerOutput* pOut = new NullRasterizerOutput();
		GlobalLog()->PrintNew( pOut, __FILE__, __LINE__, "test null output" );
		pJob->GetRasterizer()->AddRasterizerOutput( pOut );
		safe_release( pOut );

		std::srand( 7717u );
		if( !pJob->Rasterize() ) return false;

		PixelBasedRasterizerHelper* pHelper =
			dynamic_cast<PixelBasedRasterizerHelper*>( pJob->GetRasterizer() );
		if( !pHelper ) return false;
		pCaster = pHelper->GetRayCaster();
		pScene = pJob->GetScene();
		return pCaster != 0 && pScene != 0;
	}

	~Fixture()
	{
		safe_release( pJob );
		if( !scenePath.empty() ) {
			std::remove( scenePath.c_str() );
		}
	}
};

//////////////////////////////////////////////////////////////////////
// Clamps pushed to the extremes so the "interior" band is unmistakable
// and every degenerate Solve() branch lands on a recognisable value.
//////////////////////////////////////////////////////////////////////
static OptimalMISAccumulator::Config MakeConfig()
{
	OptimalMISAccumulator::Config config;
	config.tileSize = 64;			// one tile covers the whole fixture
	config.minSamplesPerTile = 16;
	config.alphaClampMin = 0.001;
	config.alphaClampMax = 0.999;
	return config;
}

//////////////////////////////////////////////////////////////////////
// DETERMINISM (round-7 review P2-1).  Every driven walk in this file
// runs on a FRESH thread, with libc's `rand()` re-seeded immediately
// before that thread is spawned.
//
// THE MECHANISM (measured, not inferred).  `HeterogeneousMedium`'s
// ratio-tracking transmittance estimator keeps ONE per-thread Mersenne
// Twister -- `static thread_local RandomNumberGenerator tl_rng` /
// `tl_rng_nm` in `HeterogeneousMedium.cpp` -- default-constructed, i.e.
// seeded from libc `rand()` on first use on that thread.  `Fixture::
// Build`'s throwaway `Rasterize()` is multithreaded AND the CALLING
// thread participates in the work (`ThreadPool.cpp`: "The caller thread
// participates in ParallelFor by draining"), so by the time a driven
// walk runs on the main thread that thread's `tl_rng` has been advanced
// by a nondeterministic number of render blocks.  Every medium shadow
// ray the walk then casts reads a different point of that stream on
// every process run.
//
// Three measurements pin it, all on the floor-fog row:
//   * main thread, as this file used to drive it:  691 / 686 / 690
//     attempts on three consecutive runs of the SAME binary;
//   * main thread with an extra `std::srand()` after the throwaway
//     render:  693 / 690 / 690 -- STILL drifting, because the main
//     thread's `tl_rng` was already constructed and advanced by then,
//     and re-seeding `rand()` cannot reset an existing engine;
//   * a fresh thread:  692 / 692 / 692, and the bright-fixture replay
//     lands on the same 692, making the k^2 radiance law EXACT rather
//     than approximate.
// A fresh thread with NO `std::srand` is also stable (681) but at a
// value that depends on whatever `rand()` state the process happens to
// be in, so the `std::srand` is kept: it makes the seed a property of
// this file alone.
//
// This SUPERSEDES the "floating-point noise in two independently-built
// EnvironmentSampler importance tables" explanation this file and
// docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md carried in round 6.
// That explanation was wrong: two separately-built Job/Scene instances
// are not involved at all (the drift reproduces on ONE fixture), and
// the EnvironmentSampler is built deterministically.
//////////////////////////////////////////////////////////////////////
template< typename F >
static void DriveOnFreshThread( unsigned int randSeed, F body )
{
	std::srand( randSeed );
	std::thread worker( body );
	worker.join();
}

//////////////////////////////////////////////////////////////////////
// The NEE half of the tile is supplied SYNTHETICALLY, on purpose.
//
// `Solve()`'s decision table only reaches the branch that reads
// `rawBsdf` as a number once the NEE technique has both enough attempts
// and a positive moment.  Supplying that half from the test -- rather
// than relying on whichever production NEE arm happens to fire in each
// fixture -- makes the solved alpha a function of the BSDF half ALONE,
// which is the half these two continuation sites are responsible for:
//
//   BSDF count short OR BSDF moment zero  ->  alpha == clampMin (0.01)
//   BSDF count and moment both present    ->  interior alpha
//
// The synthetic moment is a fixed 4.0 at pdf 1.0, so the interior value
// is also stable run to run.
//////////////////////////////////////////////////////////////////////
static void FeedSyntheticNee( OptimalMISAccumulator& acc )
{
	for( unsigned int i = 0; i < 64; ++i ) {
		acc.AccumulateCount( 0, 0, kTechniqueNEE );
		acc.Accumulate( 0, 0, 4.0, 1.0, kTechniqueNEE );
	}
}

static void CheckTrainedInterior( const OptimalMISAccumulator& acc, const char* site )
{
	const Scalar alpha = acc.GetAlpha( 0, 0 );
	std::cout << "    " << site << ": solved alpha = " << alpha << std::endl;

	const bool interior = ( alpha > 0.0011 && alpha < 0.9989 &&
		std::fabs( (double)alpha - 0.5 ) > 1e-6 );
	if( !interior ) {
		std::cout << "      (0.5 = neither technique trained; 0.001 = BSDF technique "
			"short of attempts OR zero moment; 0.999 = the BSDF moment is absent or "
			"negligible beside NEE's)" << std::endl;
	}
	Check( interior, site );
}

//////////////////////////////////////////////////////////////////////
// Site 1: RayCaster's internal volume phase-scatter continuation.
//
// A purely-scattering global fog in an object-free scene under a bright
// environment.  The fog is deliberately OPTICALLY THIN (sigma_s 0.02
// across a 40-unit box, optical depth < 1): a meaningful fraction of
// cast rays scatter once, and the phase-sampled continuation then
// ESCAPES on its first try rather than re-scattering until the volume
// bounce limit kills it -- the escape is the arm whose training this
// guards, and a thick fog never reaches it.
//////////////////////////////////////////////////////////////////////
static std::string VolumeScene( double envLevel = 1.0 )
{
	std::ostringstream envss;
	envss << envLevel << " " << envLevel << " " << envLevel;
	return std::string(
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor " + envss.str() + "\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_density\n\tcolor 1.0 1.0 1.0\n}\n"
		"\n"
		"painter_heterogeneous_medium\n{\n"
		"\tname fog\n"
		"\tabsorption 0.0 0.0 0.0\n"
		"\tscattering 0.02 0.02 0.02\n"
		"\tphase isotropic\n"
		"\tdensity_painter pnt_density\n"
		"\tresolution 4\n"
		"\tcolor_to_scalar luminance\n"
		"\tbbox_min -20 -20 -20\n"
		"\tbbox_max 20 20 20\n"
		"}\n"
		"\n"
		"global_medium\n{\n\tmedium fog\n}\n"
		"\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n"
		"\n"
		"pixelpel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n"
		"\toidn_denoise FALSE\n\tradiance_map pnt_env\n\tradiance_background TRUE\n}\n"
		"\n"
		"film\n{\n\twidth 4\n\theight 4\n}\n"
		"\n"
		"pinhole_camera\n{\n\tlocation 0 0 0\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov 10.0\n}\n"
		"\n" );
}

//! Drives the volume site against a fixture whose environment radiance is
//! `envLevel`, and hands back the RAW per-tile training state.  The path
//! decisions are a function of the per-sample seeds alone (the medium's
//! distance sampling, the phase sample and the Russian roulette all read
//! `throughput`, never radiance), so two calls at different `envLevel`
//! visit the SAME vertices in the same order -- which is what makes the
//! radiance-scaling law below an exact identity rather than an average.
static void DriveVolumeSite(
	const Fixture& fx,
	OptimalMISAccumulator& acc,
	double& sumBsdf,
	unsigned int& countBsdf )
{
	const RasterizerState rast{};
	DriveOnFreshThread( 5101u, [&]() {
	for( unsigned int s = 0; s < 1200; ++s ) {
		RandomNumberGenerator rng( 31000 + s );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		rc.pOptimalMIS = &acc;

		IRayCaster::RAY_STATE rs;
		rs.depth = 1;
		rs.importance = 1.0;
		rs.considerEmission = true;

		const Scalar z = -1 + 2 * ( s + 0.5 ) / 1200;
		const Scalar phi = s * 2.399963229728653;
		const Scalar r = std::sqrt( 1 - z * z );
		const Ray ray( Point3( 0, 0, 0 ),
			Vector3( r * std::cos( phi ), r * std::sin( phi ), z ) );

		RISEPel c( 0, 0, 0 );
		Scalar dist = 0;
		fx.pCaster->CastRay( rc, rast, ray, c, rs, &dist, 0 );
	}
	} );
	double sumNee = 0;
	unsigned int countNee = 0;
	acc.GetTileTraining( 0, 0, sumNee, sumBsdf, countNee, countBsdf );
}

static void RunVolumeSite()
{
	std::cout << "DL-72 site 1: RayCaster volume phase-scatter continuation" << std::endl;

	Fixture fx;
	Check( fx.Build( VolumeScene(), "volume" ), "volume fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;
	Check( fx.pCaster->GetLightSampler() != 0, "volume fixture has a LightSampler" );

	{
		OptimalMISAccumulator fresh;
		fresh.Initialize( 64, 64, MakeConfig() );
		fresh.Solve();
		Check( std::fabs( (double)fresh.GetAlpha( 0, 0 ) - 0.5 ) < 1e-9,
			"reference: an accumulator nothing ever trained solves to the 0.5 fallback" );
	}

	// The environment's OWN radiance, read back through the interface the
	// escape arm reads it through -- never an assumed formula.
	const RasterizerState rast{};
	const IRadianceMap* pEnv = fx.pScene->GetGlobalRadianceMap();
	Check( pEnv != 0, "volume fixture has a global radiance map" );
	if( !pEnv ) return;
	const Scalar Lenv = ColorMath::MaxValue(
		pEnv->GetRadiance( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), rast ) );
	Check( Lenv > 0, "volume fixture env radiance probe is positive" );
	if( Lenv <= 0 ) return;

	OptimalMISAccumulator acc;
	acc.Initialize( 64, 64, MakeConfig() );
	double sumBsdf = 0;
	unsigned int countBsdf = 0;
	DriveVolumeSite( fx, acc, sumBsdf, countBsdf );

	// ------------------------------------------------------------------
	// THE TRAINED QUANTITY, not just its ratio.
	//
	// At a volume vertex the contract is `bsdfTimesCos = phasePdf`
	// (broadcast to all three channels -- a phase function's "BSDF times
	// cos" IS its value, and for an isotropic phase that value equals its
	// own pdf) paired with `bsdfPdf = effectivePdf`, which with guiding OFF
	// is that same `phasePdf`.  The escape arm forms
	//
	//     f^2 / p^2 = (L_env * phasePdf)^2 / phasePdf^2 = L_env^2
	//
	// for EVERY accumulation, at every scatter depth, with no dependence on
	// the direction or on how deep the walk got.  So the accumulated sum
	// is an exact whole multiple of L_env^2, and that multiple (the number
	// of continuations that reached the environment) cannot exceed the
	// attempt count.  Solve()'s alpha is a RATIO and would be unmoved by a
	// `bsdfTimesCos` scaled by any constant; these two assertions are not.
	// The classic mis-shaping -- `bsdfTimesCos = 1` for a phase function --
	// inflates every term by 1/phasePdf^2 = (4*PI)^2 ~ 158 and breaks both.
	// ------------------------------------------------------------------
	const double accPerLenv2 = sumBsdf / ( (double)Lenv * (double)Lenv );
	const double nearestWhole = std::floor( accPerLenv2 + 0.5 );
	std::cout << "    volume site: sum(f/p)^2 = " << sumBsdf
		<< " = " << accPerLenv2 << " x L_env^2 (L_env = " << Lenv
		<< "), over " << countBsdf << " attempts" << std::endl;
	Check( sumBsdf > 0, "volume site: the BSDF technique accumulated a positive moment" );
	Check( accPerLenv2 > 0 && std::fabs( accPerLenv2 - nearestWhole ) < 1e-6,
		"volume site: the accumulated moment is a WHOLE multiple of L_env^2 "
		"(every escape contributes exactly (L_env*phasePdf)^2/phasePdf^2)" );
	Check( accPerLenv2 <= (double)countBsdf + 1e-6,
		"volume site: the number of L_env^2 contributions does not exceed the attempt count" );

	// Radiance-scaling law: the same fixture with a 3x brighter, still
	// uniform environment must accumulate exactly 9x the moment -- same
	// seeds, same paths, and the only thing that changed is the integrand's
	// radiance factor.  This is what pins the moment to the FULL integrand
	// (radiance * bsdfTimesCos) rather than to bsdfTimesCos alone.
	{
		Fixture fxBright;
		Check( fxBright.Build( VolumeScene( 3.0 ), "volume3" ), "bright volume fixture builds" );
		if( fxBright.pCaster && fxBright.pScene ) {
			OptimalMISAccumulator accBright;
			accBright.Initialize( 64, 64, MakeConfig() );
			double sumBright = 0;
			unsigned int countBright = 0;
			DriveVolumeSite( fxBright, accBright, sumBright, countBright );
			const double ratio = sumBsdf > 0 ? sumBright / sumBsdf : 0;
			std::cout << "    volume site: moment ratio at 3x radiance = " << ratio
				<< " (exact target 9)" << std::endl;
			Check( countBright == countBsdf,
				"volume site: the brighter fixture visited the same number of continuations" );
			Check( std::fabs( ratio - 9.0 ) < 1e-6,
				"volume site: the accumulated moment scales exactly as L_env^2" );
		}
	}

	FeedSyntheticNee( acc );
	acc.Solve();
	CheckTrainedInterior( acc,
		"volume continuation trains BOTH a count and a moment for the BSDF technique" );
}

//////////////////////////////////////////////////////////////////////
// Site 2: PathTracingIntegrator's BSSRDF exit continuation.
//
// A real SubSurfaceScatteringMaterial on a real sphere Object (the
// BSSRDF entry probe traces against the object directly, so the object
// need not be in the scene's own manager), driven through the
// production `IntegrateFromHit` inside an object-free, environment-lit
// scene: the BSSRDF exit continuation therefore always escapes to the
// environment, which is the arm whose training this guards.
//////////////////////////////////////////////////////////////////////
static std::string EnvOnlyScene( double envLevel = 1.0 )
{
	std::ostringstream envss;
	envss << envLevel << " " << envLevel << " " << envLevel;
	return std::string(
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor " + envss.str() + "\n}\n"
		"\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n"
		"\n"
		"pixelpel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n"
		"\toidn_denoise FALSE\n\tradiance_map pnt_env\n\tradiance_background TRUE\n}\n"
		"\n"
		"film\n{\n\twidth 4\n\theight 4\n}\n"
		"\n"
		"pinhole_camera\n{\n\tlocation 0 0 0\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov 10.0\n}\n"
		"\n" );
}

//////////////////////////////////////////////////////////////////////
// THE SPATIAL-WEIGHT SCALING LAW (DL-72 P2-3).
//
// `OptimalMISAccumulator`'s moment is `E[(f/p)^2]` where `f` is the
// INTEGRAND -- the whole vertex-local contribution -- so at a BSSRDF
// exit `f = weightSpatial * Sw(w) * cos(w) * L(w)`, whose
// `weightSpatial = Rd * Ft(exit) / pdfSurface` is an AREA-measure
// factor, not an O(1) directional one.  Round 3 trained `Sw*cos*L`,
// dropping it on the BSDF side, and `LightSampler` dropped the very
// same factor on the NEE side (the caller applies it AFTER the call).
//
// This decorator multiplies the profile's `Rd` -- and ONLY `Rd` -- by a
// constant `k`.  `Rd` appears in `weight` and `weightSpatial` and
// NOWHERE in the sampling: `BSSRDFSampling` draws its radius from
// `SampleRadius`/`PdfRadius` and its exit direction from a cosine
// lobe, none of which this touches.  So the identical seeds visit the
// identical vertices (asserted below via the attempt COUNTS), while
// every trained integrand scales by exactly `k` -- and the moment,
// being its square, by exactly `k^2`.
//
// Pre-fix both moments are INVARIANT to `k` (neither numerator carries
// `weightSpatial`), which is the red-proof: a ratio of 1 where the
// derivation says 4.
//////////////////////////////////////////////////////////////////////
class ScaledRdProfile :
	public virtual ISubSurfaceDiffusionProfile,
	public virtual Reference
{
	ISubSurfaceDiffusionProfile*	real;
	Scalar							k;

protected:
	~ScaledRdProfile() override {}

public:
	ScaledRdProfile( ISubSurfaceDiffusionProfile* r, Scalar scale )
		: real( r ), k( scale ) {}

	// The ONLY two methods that are scaled.
	RISEPel EvaluateProfile( const Scalar r, const RayIntersectionGeometric& ri ) const override
	{ return real->EvaluateProfile( r, ri ) * k; }
	Scalar EvaluateProfileNM( const Scalar r, const RayIntersectionGeometric& ri, const Scalar nm ) const override
	{ return real->EvaluateProfileNM( r, ri, nm ) * k; }

	// Everything that steers SAMPLING is forwarded verbatim.
	Scalar SampleRadius( const Scalar u, const int channel, const RayIntersectionGeometric& ri ) const override
	{ return real->SampleRadius( u, channel, ri ); }
	Scalar PdfRadius( const Scalar r, const int channel, const RayIntersectionGeometric& ri ) const override
	{ return real->PdfRadius( r, channel, ri ); }
	Scalar FresnelTransmission( const Scalar cosTheta, const RayIntersectionGeometric& ri ) const override
	{ return real->FresnelTransmission( cosTheta, ri ); }
	Scalar GetIOR( const RayIntersectionGeometric& ri ) const override
	{ return real->GetIOR( ri ); }
	Scalar GetMaximumDistanceForError( const Scalar error ) const override
	{ return real->GetMaximumDistanceForError( error ); }
	RISEPel ComputeTotalExtinction( const Scalar distance ) const override
	{ return real->ComputeTotalExtinction( distance ); }
};

class ScaledRdMaterial : public SubSurfaceScatteringMaterial
{
	ScaledRdProfile* pScaled;

protected:
	~ScaledRdMaterial() override { safe_release( pScaled ); }

public:
	ScaledRdMaterial(
		const IScalarPainter& ior, const IScalarPainter& absorption,
		const IScalarPainter& scattering, Scalar g, Scalar roughness, Scalar k )
		: SubSurfaceScatteringMaterial( ior, absorption, scattering, g, roughness )
	{
		pScaled = new ScaledRdProfile( pProfile, k );
		GlobalLog()->PrintNew( pScaled, __FILE__, __LINE__, "Rd-scaled profile" );
	}

	ISubSurfaceDiffusionProfile* GetDiffusionProfile() const override { return pScaled; }
};

//! Drives the BSSRDF site and hands back the RAW per-tile training state.
//! Seeded per sample, so two calls against fixtures that differ ONLY in
//! environment brightness walk identical paths.
static void DriveBssrdfSite(
	const Fixture& fx,
	const PathTracingIntegrator& integrator,
	Object& object,
	IMaterial& material,
	OptimalMISAccumulator& acc,
	double& sumBsdf,
	unsigned int& countBsdf,
	double* pSumNee = 0,
	unsigned int* pCountNee = 0,
	//! Path importance handed to `IntegrateFromHit`.  The default 1 is
	//! what the rows above use.  The scaling-law row passes a large
	//! value so that `RayCaster`'s own `rs.importance < RC_RR_THRESHOLD`
	//! survival test (RayCaster.cpp) cannot fire for EITHER run: that
	//! test reads the BSSRDF throughput, which the Rd scale multiplies,
	//! so at importance 1 the two runs would escape on slightly
	//! different sample SETS and the law would only hold on average.
	Scalar importance = 1,
	//! Wire the accumulator into the LightSampler as well, so the NEE
	//! half of the pair trains too.  Off by default -- the rows above
	//! measure the BSDF half against a SYNTHETIC NEE half on purpose.
	bool trainNee = false )
{
	const RasterizerState rast{};
	const LightSampler* pLS = fx.pCaster ? fx.pCaster->GetLightSampler() : 0;
	if( trainNee && pLS ) {
		pLS->SetOptimalMIS( &acc );
	}
	// Exit point on the sphere's +Z pole, ray arriving from outside.
	DriveOnFreshThread( 5102u, [&]() {
	for( unsigned int s = 0; s < 600; ++s ) {
		RandomNumberGenerator rng( 52000 + s );
		IndependentSampler sampler( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		rc.pOptimalMIS = &acc;

		RayIntersection hit( Ray( Point3( 0, 0, 12 ), Vector3( 0, 0, -1 ) ), rast );
		hit.geometric.bHit = true;
		hit.geometric.range = 2;
		hit.geometric.ptIntersection = Point3( 0, 0, 10 );
		hit.geometric.vNormal = Vector3( 0, 0, 1 );
		hit.geometric.vGeomNormal = Vector3( 0, 0, 1 );
		hit.geometric.onb.CreateFromW( hit.geometric.vNormal );
		hit.pObject = &object;
		hit.pMaterial = &material;

		IORStack stack( 1.0 );

		integrator.IntegrateFromHit(
			rc, rast, hit, *fx.pScene, *fx.pCaster, sampler,
			/*pRadianceMap*/ 0, /*startDepth*/ 0, stack,
			/*bsdfPdf_*/ 0, /*bsdfTimesCos_*/ RISEPel( 0, 0, 0 ),
			/*considerEmission_*/ true, importance,
			IRayCaster::RAY_STATE::eRayDiffuse,
			0, 0, 0, 0, 0, 0, false, false );
	}
	} );
	if( trainNee && pLS ) {
		pLS->SetOptimalMIS( 0 );
	}
	double sumNee = 0;
	unsigned int countNee = 0;
	acc.GetTileTraining( 0, 0, sumNee, sumBsdf, countNee, countBsdf );
	if( pSumNee ) *pSumNee = sumNee;
	if( pCountNee ) *pCountNee = countNee;
}

static void RunBssrdfSite()
{
	std::cout << "DL-72 site 2: PathTracingIntegrator BSSRDF exit continuation" << std::endl;

	Fixture fx;
	Check( fx.Build( EnvOnlyScene(), "bssrdf" ), "BSSRDF fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;
	Check( fx.pCaster->GetLightSampler() != 0, "BSSRDF fixture has a LightSampler" );

	UniformScalarPainter* ior = new UniformScalarPainter( 1.3 );
	RGBScalarPainter* absorption = new RGBScalarPainter( 0.05, 0.10, 0.20 );
	RGBScalarPainter* scattering = new RGBScalarPainter( 1.0, 1.0, 1.0 );
	SubSurfaceScatteringMaterial* material =
		new SubSurfaceScatteringMaterial( *ior, *absorption, *scattering, 0.0, 0.2 );
	GlobalLog()->PrintNew( material, __FILE__, __LINE__, "sss material" );

	SphereGeometry* sphere = new SphereGeometry( 10.0 );
	GlobalLog()->PrintNew( sphere, __FILE__, __LINE__, "sss sphere" );
	sphere->addref();
	Object* object = new Object( sphere );
	GlobalLog()->PrintNew( object, __FILE__, __LINE__, "sss object" );
	object->addref();
	sphere->release();
	object->AssignMaterial( *material );

	PathTracingIntegrator* integrator =
		new PathTracingIntegrator( ManifoldSolverConfig(), StabilityConfig() );
	GlobalLog()->PrintNew( integrator, __FILE__, __LINE__, "integrator" );
	integrator->SetMaxPathDepth( 3 );

	OptimalMISAccumulator acc;
	acc.Initialize( 64, 64, MakeConfig() );

	double sumBsdf = 0;
	unsigned int countBsdf = 0;
	DriveBssrdfSite( fx, *integrator, *object, *material, acc, sumBsdf, countBsdf );

	// ------------------------------------------------------------------
	// THE TRAINED QUANTITY.
	//
	// Unlike the volume site, this one has no constant per-sample moment:
	// the BSSRDF exit's `bsdfTimesCos` is the Sw-times-cosine quantity
	// recovered from the profile's own weight split, so `(f/p)^2` differs
	// from probe to probe.  What IS closed-form is its dependence on the
	// environment: the moment is formed from the FULL integrand
	// `L_env * bsdfTimesCos` divided by a radiance-INDEPENDENT density, so
	// re-running the identical seeds under a 3x brighter uniform
	// environment must give exactly 9x the sum.  A moment that dropped the
	// radiance factor (or that accumulated a bare throughput) would be
	// invariant instead, and Solve()'s alpha -- a ratio -- cannot see the
	// difference either way.
	// ------------------------------------------------------------------
	Check( sumBsdf > 0, "BSSRDF site: the BSDF technique accumulated a positive moment" );
	{
		Fixture fxBright;
		Check( fxBright.Build( EnvOnlyScene( 3.0 ), "bssrdf3" ), "bright BSSRDF fixture builds" );
		if( fxBright.pCaster && fxBright.pScene ) {
			OptimalMISAccumulator accBright;
			accBright.Initialize( 64, 64, MakeConfig() );
			double sumBright = 0;
			unsigned int countBright = 0;
			DriveBssrdfSite( fxBright, *integrator, *object, *material,
				accBright, sumBright, countBright );
			const double ratio = sumBsdf > 0 ? sumBright / sumBsdf : 0;
			std::cout << "    BSSRDF site: sum(f/p)^2 = " << sumBsdf
				<< " over " << countBsdf << " attempts; ratio at 3x radiance = "
				<< ratio << " (exact target 9)" << std::endl;
			Check( countBright == countBsdf,
				"BSSRDF site: the brighter fixture visited the same number of continuations" );
			Check( std::fabs( ratio - 9.0 ) < 1e-6,
				"BSSRDF site: the accumulated moment scales exactly as L_env^2" );
		}
	}

	// ------------------------------------------------------------------
	// THE SPATIAL-WEIGHT SCALING LAW.  See ScaledRdProfile's derivation:
	// scaling the profile's Rd by k scales the vertex-local integrand of
	// BOTH techniques at this vertex by k, and their moments by k^2,
	// while leaving every sampling decision untouched.  Run against a
	// SECOND, pristine accumulator so the k=1 numbers above are not
	// disturbed.
	// ------------------------------------------------------------------
	{
		const Scalar k = 2.0;
		double sumB1 = 0, sumN1 = 0, sumB2 = 0, sumN2 = 0;
		unsigned int cntB1 = 0, cntN1 = 0, cntB2 = 0, cntN2 = 0;

		OptimalMISAccumulator acc1;
		acc1.Initialize( 64, 64, MakeConfig() );
		DriveBssrdfSite( fx, *integrator, *object, *material, acc1,
			sumB1, cntB1, &sumN1, &cntN1, 1.0e6, true );

		ScaledRdMaterial* scaledMat = new ScaledRdMaterial(
			*ior, *absorption, *scattering, 0.0, 0.2, k );
		GlobalLog()->PrintNew( scaledMat, __FILE__, __LINE__, "Rd-scaled sss material" );
		SphereGeometry* sphere2 = new SphereGeometry( 10.0 );
		GlobalLog()->PrintNew( sphere2, __FILE__, __LINE__, "sss sphere (scaled)" );
		sphere2->addref();
		Object* object2 = new Object( sphere2 );
		GlobalLog()->PrintNew( object2, __FILE__, __LINE__, "sss object (scaled)" );
		object2->addref();
		sphere2->release();
		object2->AssignMaterial( *scaledMat );

		OptimalMISAccumulator acc2;
		acc2.Initialize( 64, 64, MakeConfig() );
		DriveBssrdfSite( fx, *integrator, *object2, *scaledMat, acc2,
			sumB2, cntB2, &sumN2, &cntN2, 1.0e6, true );

		std::cout << "    BSSRDF site: Rd x" << k
			<< "  BSDF sum " << sumB1 << " -> " << sumB2
			<< " (attempts " << cntB1 << " / " << cntB2 << ")"
			<< " ,  NEE sum " << sumN1 << " -> " << sumN2
			<< " (attempts " << cntN1 << " / " << cntN2 << ")" << std::endl;

		// Premise: Rd steers no sampling decision, so the same seeds
		// visited the same vertices and made the same attempts.
		Check( cntB1 == cntB2 && cntB1 > 0,
			"BSSRDF site: scaling Rd changes no BSDF sampling decision (identical attempt count)" );
		Check( cntN1 == cntN2 && cntN1 > 0,
			"BSSRDF site: scaling Rd changes no NEE sampling decision (identical attempt count)" );

		// The law itself, on both halves of the pair.
		//
		// THE BOUND, and why it is not exactly k^2.  A tile's sums are
		// over EVERY training event the walk produced, and this walk has
		// a second vertex whose events are Rd-INDEPENDENT: the SSS
		// SURFACE itself (its own NEE, and its own continuation's escape
		// to the environment).  So each sum is
		//     S(k) = k^2 * S_bssrdf + S_surface
		// and the ratio is bounded, for any nonzero S_surface, by
		//     1 < S(k)/S(1) < k^2
		// -- strictly below k^2, approaching it as the BSSRDF share of
		// the tile grows.  A numerator that DROPPED `weightSpatial`
		// (round 3's `Sw*cos`, and `LightSampler`'s unscaled `contrib`)
		// makes S_bssrdf itself Rd-independent and pins the ratio at
		// EXACTLY 1 -- which is what the pre-fix library reads, to every
		// digit, on both halves.  The gate below therefore asks for the
		// k^2 end of that interval, not for the midpoint.
		const double target = (double)k * (double)k;
		const double rB = sumB1 > 0 ? sumB2 / sumB1 : 0;
		const double rN = sumN1 > 0 ? sumN2 / sumN1 : 0;
		std::cout << "    BSSRDF site: moment ratios  BSDF " << rB
			<< " , NEE " << rN << "  (bounded 1 < r < " << target << ")" << std::endl;
		Check( sumB1 > 0 && rB > 0.975 * target && rB <= target + 1e-9,
			"BSSRDF site: the BSDF moment carries weightSpatial (scales as Rd^2)" );
		Check( sumN1 > 0 && rN > 0.975 * target && rN <= target + 1e-9,
			"BSSRDF site: the entry-NEE moment carries the same weightSpatial (scales as Rd^2)" );

		object2->release();
		scaledMat->release();
	}

	FeedSyntheticNee( acc );
	acc.Solve();
	CheckTrainedInterior( acc,
		"BSSRDF exit continuation trains BOTH a count and a moment for the BSDF technique" );

	integrator->release();
	object->release();
	material->release();
	scattering->release();
	absorption->release();
	ior->release();
}

//////////////////////////////////////////////////////////////////////
// Site 3 (DL-84): PathTracingIntegrator's OWN in-loop volume phase-
// scatter continuation -- `IntegrateFromHitTemplated`'s
// `bsdfPdf = effectivePdf` site, reached only AFTER a surface bounce
// (a camera ray's first medium interaction is a different walk,
// `IntegrateRayTemplated`, which is not this row's subject).
//
// CONSTRUCTION: a perfectly-specular (delta) mirror floor in the SAME
// optically-thin global fog `VolumeScene()` builds for Site 1, so the
// reflected ray continues straight into the medium.  A delta lobe is
// deliberately chosen for the floor: PathTracingIntegrator's no-BSDF
// (SPF-only) branch that handles it sets `bsdfPdf = bsdfMisPdf = 0`
// and `bsdfTimesCos = Traits::zero()` for a delta scatter and calls
// NEITHER `AccumulateCount` NOR `Accumulate` anywhere in that branch
// (confirmed by reading -- grep finds zero training calls in
// `PathTracingIntegrator.cpp`'s "Specular surfaces (no BSDF)" block).
// So the mirror bounce itself is training-INERT by construction, and
// this fixture has no other geometry for the reflected ray to hit --
// EVERY count and moment this fixture's accumulator ever records can
// only have come from the medium vertex the reflected ray travels
// into, i.e. DL-84's site and nothing else.  A nonzero `countBsdf` is
// therefore itself the direct evidence that this site (not some other
// producer) is the one training; the ledger row's own pre-fix state
// (no `AccumulateCount` call at this site at all) reads `countBsdf == 0`
// here -- verified below by literally reverting the two-line fix and
// re-running (see the fix commit message for the paired numbers).
//////////////////////////////////////////////////////////////////////

//! Drives the DL-84 site and hands back the RAW per-tile training
//! state.  Seeded per sample, so two calls against fixtures that
//! differ only in environment brightness walk identical paths -- the
//! incoming ray direction (hence the delta-mirror's reflected
//! direction) and every downstream phase-function / Russian-roulette
//! decision are functions of the RNG stream alone, never of radiance.
static void DriveFloorFogSite(
	const Fixture& fx,
	const PathTracingIntegrator& integrator,
	Object& object,
	IMaterial& material,
	OptimalMISAccumulator& acc,
	double& sumBsdf,
	unsigned int& countBsdf )
{
	const RasterizerState rast{};
	DriveOnFreshThread( 5103u, [&]() {
	for( unsigned int s = 0; s < 1200; ++s )
	{
		RandomNumberGenerator rng( 61000 + s );
		IndependentSampler sampler( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		rc.pOptimalMIS = &acc;

		// Incoming ray from the downward hemisphere (Fibonacci lattice,
		// matching DriveVolumeSite's construction) striking a horizontal
		// mirror floor at the fog box's own centre -- the reflected ray
		// therefore always points into the UPWARD hemisphere, straight
		// into the bulk of the medium (bbox +-20).
		const Scalar z = -1 + 1 * ( s + 0.5 ) / 1200;			// in (-1, 0): downward only
		const Scalar phi = s * 2.399963229728653;
		const Scalar r = std::sqrt( 1 - z * z );
		const Vector3 inDir( r * std::cos( phi ), z, r * std::sin( phi ) );

		const Point3 origin( -inDir[0] * 10, -inDir[1] * 10, -inDir[2] * 10 );
		RayIntersection hit( Ray( origin, inDir ), rast );
		hit.geometric.bHit = true;
		hit.geometric.range = 10;
		hit.geometric.ptIntersection = Point3( 0, 0, 0 );
		hit.geometric.vNormal = Vector3( 0, 1, 0 );
		hit.geometric.vGeomNormal = Vector3( 0, 1, 0 );
		hit.geometric.onb.CreateFromW( hit.geometric.vNormal );
		hit.pObject = &object;
		hit.pMaterial = &material;

		IORStack stack( 1.0 );

		integrator.IntegrateFromHit(
			rc, rast, hit, *fx.pScene, *fx.pCaster, sampler,
			/*pRadianceMap*/ 0, /*startDepth*/ 0, stack,
			/*bsdfPdf_*/ 0, /*bsdfTimesCos_*/ RISEPel( 0, 0, 0 ),
			/*considerEmission_*/ true, /*importance*/ 1,
			IRayCaster::RAY_STATE::eRaySpecular,
			0, 0, 0, 0, 0, 0, false, false );
	}
	} );
	double sumNee = 0;
	unsigned int countNee = 0;
	acc.GetTileTraining( 0, 0, sumNee, sumBsdf, countNee, countBsdf );
}

static void RunFloorFogSite()
{
	std::cout << "DL-84 site: PathTracingIntegrator's own in-loop volume "
		"phase-scatter continuation" << std::endl;

	Fixture fx;
	Check( fx.Build( VolumeScene(), "floorfog" ), "floor-fog fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;
	Check( fx.pCaster->GetLightSampler() != 0, "floor-fog fixture has a LightSampler" );

	const RasterizerState rast{};
	const IRadianceMap* pEnv = fx.pScene->GetGlobalRadianceMap();
	Check( pEnv != 0, "floor-fog fixture has a global radiance map" );
	if( !pEnv ) return;
	const Scalar Lenv = ColorMath::MaxValue(
		pEnv->GetRadiance( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), rast ) );
	Check( Lenv > 0, "floor-fog fixture env radiance probe is positive" );
	if( Lenv <= 0 ) return;

	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( white, __FILE__, __LINE__, "floor-fog mirror painter" );
	PerfectReflectorMaterial* material = new PerfectReflectorMaterial( *white );
	GlobalLog()->PrintNew( material, __FILE__, __LINE__, "floor-fog mirror material" );

	SphereGeometry* sphere = new SphereGeometry( 10.0 );
	GlobalLog()->PrintNew( sphere, __FILE__, __LINE__, "floor-fog placeholder geometry" );
	sphere->addref();
	Object* object = new Object( sphere );
	GlobalLog()->PrintNew( object, __FILE__, __LINE__, "floor-fog placeholder object" );
	object->addref();
	sphere->release();
	object->AssignMaterial( *material );

	PathTracingIntegrator* integrator =
		new PathTracingIntegrator( ManifoldSolverConfig(), StabilityConfig() );
	GlobalLog()->PrintNew( integrator, __FILE__, __LINE__, "floor-fog integrator" );
	integrator->SetMaxPathDepth( 6 );

	OptimalMISAccumulator acc;
	acc.Initialize( 64, 64, MakeConfig() );
	double sumBsdf = 0;
	unsigned int countBsdf = 0;
	DriveFloorFogSite( fx, *integrator, *object, *material, acc, sumBsdf, countBsdf );

	std::cout << "    floor-fog site: sum(f/p)^2 = " << sumBsdf
		<< " over " << countBsdf << " attempts" << std::endl;
	Check( countBsdf > 0,
		"floor-fog site: the in-loop volume continuation counted at least one attempt "
		"(the mirror floor's own delta bounce contributes none)" );
	Check( sumBsdf > 0,
		"floor-fog site: the in-loop volume continuation accumulated a positive moment" );

	// ------------------------------------------------------------------
	// THE TRAINED QUANTITY.  With guiding inactive (this fixture never
	// configures rc.pGuidingField), `effectivePdf == phasePdf` and, for
	// the isotropic phase function VolumeScene() configures, `phaseVal
	// == pPhase->Evaluate(...) == phasePdf` too (both are the same
	// constant 1/(4*pi) for every direction).  So, exactly as in
	// DriveVolumeSite's own site,
	//     f^2/p^2 = (L_env * phaseVal)^2 / effectivePdf^2 = L_env^2
	// for every accumulation, regardless of direction or bounce depth.
	// ------------------------------------------------------------------
	const double accPerLenv2 = sumBsdf / ( (double)Lenv * (double)Lenv );
	const double nearestWhole = std::floor( accPerLenv2 + 0.5 );
	Check( accPerLenv2 > 0 && std::fabs( accPerLenv2 - nearestWhole ) < 1e-6,
		"floor-fog site: the accumulated moment is a WHOLE multiple of L_env^2 "
		"(every escape contributes exactly (L_env*phaseVal)^2/effectivePdf^2)" );
	Check( accPerLenv2 <= (double)countBsdf + 1e-6,
		"floor-fog site: the number of L_env^2 contributions does not exceed the attempt count" );

	// Radiance-scaling law (k^2 = 9 for k=3), mirroring both existing
	// sites' construction: nothing in this fixture's SAMPLING depends on
	// radiance, so a 3x brighter, still-uniform environment visits the
	// same vertices and scales the accumulated moment by exactly 9.
	//
	// ROUND-7 REVIEW P2-1 / P3.  Round 6 ran this row with a +-2%
	// "cross-build noise" tolerance and blamed a ~1% attempt-count drift
	// on floating-point noise between two independently-built
	// EnvironmentSampler importance tables.  That diagnosis was WRONG --
	// see `DriveOnFreshThread`'s own comment for the measured mechanism
	// (a `rand()`-seeded thread_local RNG inside HeterogeneousMedium's
	// ratio-tracking transmittance, advanced a nondeterministic number
	// of times on the MAIN thread by the throwaway multithreaded
	// `Rasterize()` inside `Fixture::Build`).  With every driven walk
	// now on its own fresh thread the two fixtures are EXACT replays of
	// each other: identical attempt counts and a ratio of exactly 9, so
	// both checks below are exact.
	{
		Fixture fxBright;
		Check( fxBright.Build( VolumeScene( 3.0 ), "floorfog3" ), "bright floor-fog fixture builds" );
		if( fxBright.pCaster && fxBright.pScene ) {
			OptimalMISAccumulator accBright;
			accBright.Initialize( 64, 64, MakeConfig() );
			double sumBright = 0;
			unsigned int countBright = 0;
			DriveFloorFogSite( fxBright, *integrator, *object, *material,
				accBright, sumBright, countBright );
			const double ratio = sumBsdf > 0 ? sumBright / sumBsdf : 0;
			const double countRatio = countBsdf > 0 ? (double)countBright / (double)countBsdf : 0;
			std::cout << "    floor-fog site: moment ratio at 3x radiance = " << ratio
				<< " (exact target 9); attempts " << countBsdf
				<< " / " << countBright << " (exact ratio " << countRatio << ")" << std::endl;
			Check( countBright == countBsdf,
				"floor-fog site: the brighter fixture visited exactly the same continuations" );
			Check( std::fabs( ratio - 9.0 ) < 1e-9,
				"floor-fog site: the accumulated moment scales as L_env^2 (exactly)" );
		}
	}

	FeedSyntheticNee( acc );
	acc.Solve();
	CheckTrainedInterior( acc,
		"in-loop volume continuation trains BOTH a count and a moment for the BSDF technique" );

	integrator->release();
	object->release();
	material->release();
	white->release();
}

//////////////////////////////////////////////////////////////////////
// RR CONVENTION CHECK: the REALIZED-MOMENT ruling (round 7).
//
// THE CONTRACT.  `OptimalMISAccumulator::Solve()` computes
// `alpha = M_nee / (M_nee + M_bsdf)`, i.e. each technique's coefficient
// is `1 / M_i` (Kondapaneni 2019).  The `M_i` that belongs in that
// expression is the second moment of the estimator the FILM ACTUALLY
// SEES for technique `i` -- so if technique `i`'s sample is subjected
// to Russian roulette, RR is part of its EFFECTIVE density, not a
// separate layer sitting above the moment.
//
// THE DERIVATION.  Write the realized two-technique estimator, with the
// BSDF branch RR'd at survival probability `q` and compensated:
//
//     F = w_n(x_n) f(x_n)/p_n(x_n)
//       + S * w_b(x_b) f(x_b) / ( p_b(x_b) * q(x_b) ),    S ~ Bern(q)
//
// which is exactly sampling the BSDF branch from the DEFECTIVE density
// `p~_b = q * p_b`.  Its second moment is
//
//     E[F_b^2] = q * E_{x~p_b}[ ( w_b f / (p_b q) )^2 ]
//              = integral w_b^2 f^2 / (p_b q)
//
// so the quantity that belongs in alpha's denominator is
//
//     M_bsdf = integral f^2 / (p_b q) = E_pre / q            (E_pre = integral f^2/p_b)
//
// -- the REALIZED moment.  Russian roulette makes a technique WORSE
// (more variance), and `1/M` must see that.
//
// THE THREE CANDIDATE WIRINGS, and what each estimates.  The estimator
// of `M_i` is (sum of accumulated per-sample moments) / (attempt count):
//
//   (a) numerator POST-RR (as carried), count EVERY attempt
//         E[sum]/N = (1/N) * N * q * E[(f/(p q))^2] = E_pre / q   <== CORRECT
//   (b) numerator PRE-RR, count EVERY attempt
//         E[sum]/N = q * E_pre                       -- off by q^2
//   (c) numerator PRE-RR, count only SURVIVORS
//         E[sum]/N = E_pre                           -- off by q
//
// Round 6 ruled (b) and wired the ordinary surface continuation to it,
// on the argument that "RR is a separate later estimator, not part of
// the vertex-local integrand".  THAT RULING IS RETRACTED.  It is also
// not what (b) computes: (b) divides a survivor-only numerator by an
// all-attempts denominator, so it is neither the pre-RR moment nor the
// realized one -- it is the pre-RR moment scaled DOWN by `q`, i.e. the
// realized moment scaled by `q^2`.  Quadrature over a family of
// two-technique toys (RunAlphaQualityCheck below) puts (a) at the
// lowest combined variance and (b) at the highest in every config.
//
// Round 6's second argument -- "NEE undergoes no RR, so only the BSDF
// side would carry the factor" -- is also false:  `LightSampler`'s
// mesh-luminary arm has its own light-sample Russian roulette
// (`rrSurvivalCompensation`, scene knob `light_rr_threshold`, default
// 0), applied AFTER its `AccumulateCount` and EXCLUDED from the
// accumulated `contrib`, so with that knob on NEE trained wiring (b)
// by the identical mechanism.  `RunLightRRConventionCheck` below is
// that row.
//
// THE FIXTURE.  A Lambertian floor of reflectance rho = 0.5 in an
// object-free, environment-lit scene (so every SURVIVING continuation
// escapes straight to the environment -- there is nothing else for it
// to hit), driven at `startDepth = rrMinDepth` (StabilityConfig's
// default 3, so Russian roulette evaluates on this very first
// continuation) with `importance = 1`, so
//
//     rrProb = min(1, importance*rho / max(importance, rrThreshold))
//            = rho                              (importance=1 >> rrThreshold=0.05)
//
// is an EXACT, deterministic 0.5 for every sample -- only the
// accept/reject coin flip is random.  A cosine-sampled Lambertian's
// `kray` (hence `scatterThroughput`) is exactly `rho`, direction-
// independent (the BSDF/pdf ratio cancels analytically), so:
//
//   CORRECT, wiring (a):  a survivor's POST-RR `scatterThroughput` is
//     rho/rrProb = rho/rho = 1, so f/p = 1 and each survivor's moment
//     is L_env^2 = (L_env*rho)^2 / rho^2.
//   ROUND 6, wiring (b):  the PRE-RR `rho`, so f/p = rho and each
//     survivor's moment is (L_env*rho)^2.
//
// `AccumulateCount` fires for EVERY attempt (survivor or not), and
// survival is a convention-independent Bernoulli(0.5) coin flip, so
//
//     sum(f/p)^2 / ( attempts * (L_env*rho)^2 )
//
// converges to  q / rho^2 = 1/rho = 2.0  under the correct wiring and
// to  q = 0.5  under round 6's -- a clean 4x discriminator that needs
// no in-file code toggle.  Round 6 asserted the 0.5 end.
//////////////////////////////////////////////////////////////////////
static void RunRRConventionCheck()
{
	std::cout << "DL-84 (RR convention): the surface continuation trains the REALIZED (post-RR) moment" << std::endl;

	Fixture fx;
	Check( fx.Build( EnvOnlyScene(), "rrconv" ), "RR-convention fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const RasterizerState rast{};
	const IRadianceMap* pEnv = fx.pScene->GetGlobalRadianceMap();
	Check( pEnv != 0, "RR-convention fixture has a global radiance map" );
	if( !pEnv ) return;
	const Scalar Lenv = ColorMath::MaxValue(
		pEnv->GetRadiance( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), rast ) );
	Check( Lenv > 0, "RR-convention fixture env radiance probe is positive" );
	if( Lenv <= 0 ) return;

	const Scalar rho = 0.5;
	UniformColorPainter* grey = new UniformColorPainter( RISEPel( rho, rho, rho ) );
	GlobalLog()->PrintNew( grey, __FILE__, __LINE__, "RR-convention floor painter" );
	LambertianMaterial* material = new LambertianMaterial( *grey );
	GlobalLog()->PrintNew( material, __FILE__, __LINE__, "RR-convention floor material" );

	SphereGeometry* sphere = new SphereGeometry( 10.0 );
	GlobalLog()->PrintNew( sphere, __FILE__, __LINE__, "RR-convention placeholder geometry" );
	sphere->addref();
	Object* object = new Object( sphere );
	GlobalLog()->PrintNew( object, __FILE__, __LINE__, "RR-convention placeholder object" );
	object->addref();
	sphere->release();
	object->AssignMaterial( *material );

	PathTracingIntegrator* integrator =
		new PathTracingIntegrator( ManifoldSolverConfig(), StabilityConfig() );
	GlobalLog()->PrintNew( integrator, __FILE__, __LINE__, "RR-convention integrator" );
	integrator->SetMaxPathDepth( 6 );

	OptimalMISAccumulator acc;
	acc.Initialize( 64, 64, MakeConfig() );

	const unsigned int rrMinDepth = StabilityConfig().rrMinDepth;	// 3: forces RR to evaluate immediately
	const unsigned int N = 4000;
	DriveOnFreshThread( 5104u, [&]() {
	for( unsigned int s = 0; s < N; ++s )
	{
		RandomNumberGenerator rng( 91000 + s );
		IndependentSampler sampler( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		rc.pOptimalMIS = &acc;

		RayIntersection hit( Ray( Point3( 0, 0, 10 ), Vector3( 0, 0, -1 ) ), rast );
		hit.geometric.bHit = true;
		hit.geometric.range = 10;
		hit.geometric.ptIntersection = Point3( 0, 0, 0 );
		hit.geometric.vNormal = Vector3( 0, 0, 1 );
		hit.geometric.vGeomNormal = Vector3( 0, 0, 1 );
		hit.geometric.onb.CreateFromW( hit.geometric.vNormal );
		hit.pObject = object;
		hit.pMaterial = material;

		IORStack stack( 1.0 );

		integrator->IntegrateFromHit(
			rc, rast, hit, *fx.pScene, *fx.pCaster, sampler,
			/*pRadianceMap*/ 0, /*startDepth*/ rrMinDepth, stack,
			/*bsdfPdf_*/ 0, /*bsdfTimesCos_*/ RISEPel( 0, 0, 0 ),
			/*considerEmission_*/ true, /*importance*/ 1.0,
			IRayCaster::RAY_STATE::eRayDiffuse,
			0, 0, 0, 0, 0, 0, false, false );
	}
	} );

	double sumNee = 0, sumBsdf = 0;
	unsigned int countNee = 0, countBsdf = 0;
	acc.GetTileTraining( 0, 0, sumNee, sumBsdf, countNee, countBsdf );

	const double denom = (double)countBsdf * (double)Lenv * (double)Lenv * (double)rho * (double)rho;
	const double observed = denom > 0 ? sumBsdf / denom : -1;
	std::cout << "    RR-convention: sum(f/p)^2 = " << sumBsdf << " over " << countBsdf
		<< " attempts; sum / (attempts * (L_env*rho)^2) = " << observed
		<< "  (expect ~2.0 = q/rho^2, the REALIZED moment; round 6's PRE-RR wiring reads ~0.5)"
		<< std::endl;
	Check( countBsdf > 0, "RR-convention: the surface continuation counted attempts" );
	Check( sumBsdf > 0, "RR-convention: the surface continuation accumulated a positive moment" );
	Check( observed > 1.85 && observed < 2.15,
		"RR-convention: the trained moment is the REALIZED one, E_pre/q (~2.0) -- "
		"not round 6's q*E_pre (~0.5), and not the bare pre-RR moment E_pre (~1.0)" );

	integrator->release();
	object->release();
	material->release();
	grey->release();
}

//////////////////////////////////////////////////////////////////////
// LIGHT-SAMPLE RR CONVENTION (round 7).
//
// Round 6's ruling leaned on "NEE undergoes no RR at this vertex, so
// only the BSDF side would carry an RR factor".  `LightSampler`'s
// mesh-luminary arm has its OWN Russian roulette:
//
//     AccumulateCount( ..., kTechniqueNEE );          <-- every attempt
//     ...
//     if( lightSampleRRThreshold > 0 ) {
//         pSurvive = min( estimate / lightSampleRRThreshold, 1 );
//         if( sampler.Get1D() >= pSurvive ) break;    <-- killed
//         rrSurvivalCompensation = 1 / pSurvive;
//     }
//     ...
//     Accumulate( ..., lum(contrib)^2, pdfAlias, kTechniqueNEE );
//     result += contrib * (rrSurvivalCompensation * risWeight / pdfAlias);
//
// so `rrSurvivalCompensation` is in the ESTIMATOR the film sees but not
// in the trained moment: exactly wiring (b) again, on the NEE side,
// whenever `light_rr_threshold` is non-zero.
//
// THE ASSERTION, with no need to know `q` anywhere.  The two runs below
// use ONE fixture, ONE set of per-sample seeds and `maxPathDepth = 1`
// (so there is EXACTLY one NEE attempt per sample and the light-RR coin
// is the last random draw of the walk -- nothing downstream can diverge
// between the two runs).  The sampled point on the emitter is therefore
// IDENTICAL, sample for sample, with the threshold off and on.  Writing
// `s_i` for sample i's un-RR'd moment `(contrib_i/pdfAlias_i)^2`:
//
//     sum_off       = sum_i s_i
//     sum_on (a)    = sum_{survivors} s_i / q_i^2 ,  E = sum_i s_i / q_i
//     sum_on (b)    = sum_{survivors} s_i         ,  E = sum_i s_i * q_i
//
// and `q_i = min(estimate_i/threshold, 1) <= 1` POINTWISE, so
//
//     ratio (a) = sum_on/sum_off >= 1      (strictly > 1 once RR fires)
//     ratio (b) = sum_on/sum_off <= 1      (strictly < 1 once RR fires)
//
// A rigorous two-sided discriminator with no tuned constant and no
// closed form for the light's geometry.
//////////////////////////////////////////////////////////////////////

static const Scalar kLightRRSphereRadius = 2.0;
static const Scalar kLightRRSphereDist   = 5.0;
//! Chosen so that the typical `estimate` lands well BELOW it and the
//! roulette actually fires on most samples; the assertions above hold
//! for any positive value, this one just makes the gap large.
static const Scalar kLightRRThreshold    = 5000.0;

static std::string AreaLightScene()
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n"
		"\n"
		"lambertian_luminaire_material\n{\n\tname emitter\n\texitance white\n"
		"\tscale 4.0\n\tmaterial none\n}\n"
		"\n"
		"sphere_geometry\n{\n\tname lightball\n\tradius " << kLightRRSphereRadius << "\n}\n"
		"\n"
		"standard_object\n{\n\tname light_object\n\tgeometry lightball\n"
		"\tmaterial emitter\n\tposition 0 0 " << kLightRRSphereDist << "\n}\n"
		"\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n"
		"\n"
		"pixelpel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n"
		"\n"
		"film\n{\n\twidth 4\n\theight 4\n}\n"
		"\n"
		"pinhole_camera\n{\n\tlocation 0 0 -3\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov 40.0\n}\n"
		"\n";
	return ss.str();
}

static void DriveLightRRSite(
	const Fixture& fx,
	const PathTracingIntegrator& integrator,
	Object& object,
	IMaterial& material,
	Scalar rrThreshold,
	double& sumNee,
	unsigned int& countNee )
{
	const RasterizerState rast{};
	const LightSampler* pLS = fx.pCaster->GetLightSampler();

	OptimalMISAccumulator acc;
	acc.Initialize( 64, 64, MakeConfig() );

	const_cast<LightSampler*>( pLS )->SetLightSampleRRThreshold( rrThreshold );
	pLS->SetOptimalMIS( &acc );

	DriveOnFreshThread( 5105u, [&]() {
		for( unsigned int s = 0; s < 4000; ++s )
		{
			RandomNumberGenerator rng( 71000 + s );
			IndependentSampler sampler( rng );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			rc.pOptimalMIS = &acc;

			// Arriving from the +Z side, so the emitter at +Z sits in the
			// front hemisphere (the same construction the RR-convention
			// row above uses).
			RayIntersection hit( Ray( Point3( 0, 0, 10 ), Vector3( 0, 0, -1 ) ), rast );
			hit.geometric.bHit = true;
			hit.geometric.range = 10;
			hit.geometric.ptIntersection = Point3( 0, 0, 0 );
			hit.geometric.vNormal = Vector3( 0, 0, 1 );
			hit.geometric.vGeomNormal = Vector3( 0, 0, 1 );
			hit.geometric.onb.CreateFromW( hit.geometric.vNormal );
			hit.pObject = &object;
			hit.pMaterial = &material;

			IORStack stack( 1.0 );

			integrator.IntegrateFromHit(
				rc, rast, hit, *fx.pScene, *fx.pCaster, sampler,
				/*pRadianceMap*/ 0, /*startDepth*/ 0, stack,
				/*bsdfPdf_*/ 0, /*bsdfTimesCos_*/ RISEPel( 0, 0, 0 ),
				/*considerEmission_*/ true, /*importance*/ 1.0,
				IRayCaster::RAY_STATE::eRayDiffuse,
				0, 0, 0, 0, 0, 0, false, false );
		}
	} );

	pLS->SetOptimalMIS( 0 );
	const_cast<LightSampler*>( pLS )->SetLightSampleRRThreshold( 0.0 );

	double sumBsdf = 0;
	unsigned int countBsdf = 0;
	acc.GetTileTraining( 0, 0, sumNee, sumBsdf, countNee, countBsdf );
}

static void RunLightRRConventionCheck()
{
	std::cout << "DL-84 (light-sample RR): LightSampler's NEE arm trains the REALIZED moment"
		<< std::endl;

	Fixture fx;
	Check( fx.Build( AreaLightScene(), "lightrr" ), "light-RR fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;
	Check( fx.pCaster->GetLightSampler() != 0, "light-RR fixture has a LightSampler" );
	if( !fx.pCaster->GetLightSampler() ) return;

	UniformColorPainter* grey = new UniformColorPainter( RISEPel( 0.8, 0.8, 0.8 ) );
	GlobalLog()->PrintNew( grey, __FILE__, __LINE__, "light-RR floor painter" );
	LambertianMaterial* material = new LambertianMaterial( *grey );
	GlobalLog()->PrintNew( material, __FILE__, __LINE__, "light-RR floor material" );

	SphereGeometry* sphere = new SphereGeometry( 10.0 );
	GlobalLog()->PrintNew( sphere, __FILE__, __LINE__, "light-RR placeholder geometry" );
	sphere->addref();
	Object* object = new Object( sphere );
	GlobalLog()->PrintNew( object, __FILE__, __LINE__, "light-RR placeholder object" );
	object->addref();
	sphere->release();
	object->AssignMaterial( *material );

	PathTracingIntegrator* integrator =
		new PathTracingIntegrator( ManifoldSolverConfig(), StabilityConfig() );
	GlobalLog()->PrintNew( integrator, __FILE__, __LINE__, "light-RR integrator" );
	// EXACTLY one NEE attempt per driven sample: the light-RR coin is
	// then the last random draw of the walk, so the two runs below visit
	// the identical emitter points.
	integrator->SetMaxPathDepth( 1 );

	double sumOff = 0, sumOn = 0;
	unsigned int countOff = 0, countOn = 0;
	DriveLightRRSite( fx, *integrator, *object, *material, 0.0, sumOff, countOff );
	DriveLightRRSite( fx, *integrator, *object, *material, kLightRRThreshold, sumOn, countOn );

	const double ratio = sumOff > 0 ? sumOn / sumOff : -1;
	std::cout << "    light-RR: NEE moment sum " << sumOff << " (threshold 0) -> " << sumOn
		<< " (threshold " << kLightRRThreshold << "); attempts " << countOff << " / " << countOn
		<< "; ratio = " << ratio
		<< "  (expect > 1: q <= 1 pointwise, so E_pre/q >= E_pre.  Round 6's wiring reads < 1)"
		<< std::endl;

	Check( countOff == 4000 && countOn == 4000,
		"light-RR: exactly one counted NEE attempt per driven sample, in both runs "
		"(AccumulateCount fires before the roulette)" );
	Check( sumOff > 0 && sumOn > 0, "light-RR: both runs accumulated a positive NEE moment" );
	Check( ratio > 1.0,
		"light-RR: turning the light-sample roulette ON RAISES the trained NEE moment "
		"(the realized moment E_pre/q), rather than lowering it (round 6's q*E_pre)" );
	// A strictly-greater-than-1 gate alone would pass on a fixture where
	// the roulette never fires.  This one asserts it really did.
	Check( ratio > 1.2,
		"light-RR: the roulette actually fired on this fixture (ratio well above 1)" );

	integrator->release();
	object->release();
	material->release();
	grey->release();
}

//////////////////////////////////////////////////////////////////////
// ALPHA QUALITY (round 7): the three candidate RR wirings, ranked by
// the COMBINED VARIANCE their alpha produces, on two-technique toys
// where every integral is done by quadrature.
//
// This is the row that says the round-7 ruling is not merely a
// different reading of a doc comment but the LOWER-VARIANCE one.  For a
// 1-D toy on [0,1] with `p_n = 1`, a BSDF density `p_b`, an integrand
// `f` and a constant RR survival `q` on the BSDF branch, the realized
// combined estimator is
//
//     F(alpha) = w_n f/p_n  +  S * w_b f/(p_b q)
//     w_b(x) = alpha p_b / (alpha p_b + (1-alpha) p_n),  w_n = 1 - w_b
//
// (the weights use the NOMINAL densities, exactly as
// `MISWeights::OptimalMIS2Weight` does), whose variance is
//
//     Var(alpha) = integral w_n^2 f^2/p_n
//                + integral w_b^2 f^2/(p_b q)
//                - (integral w_n f)^2 - (integral w_b f)^2
//
// -- unbiased for every alpha, so alpha is a pure variance knob.  The
// three wirings feed `Solve()` three different `M_bsdf`:
//
//     (a) realized      E_pre/q      (b) round 6   q*E_pre     (c) E_pre
//
// Also reported: a brute-force grid minimum, because
// `alpha = M_n/(M_n+M_b)` is the 1/M NORMALISATION, not the exact
// minimiser -- the claim being gated is the RANKING of the three, not
// that any of them hits the true optimum.
//
// The second half feeds `OptimalMISAccumulator` real Monte Carlo draws
// under wiring (a) and checks `Solve()` returns the alpha the closed
// form predicts.  Config 1 is chosen for it because its per-survivor
// realized moment is EXACTLY 1: with f = x, p_b = 2x, q = 1/2,
// (f/(p_b q))^2 = (x/x)^2 = 1, so the BSDF half has ZERO sampling
// variance and the round-trip is tight.
//////////////////////////////////////////////////////////////////////

typedef double (*ToyFn)( double );

static double ToyF_x( double x )     { return x; }
static double ToyF_x2( double x )    { return x * x; }
static double ToyF_exp( double x )   { return std::exp( -4.0 * x ); }
static double ToyPb_2x( double x )   { return 2.0 * x; }
static double ToyPb_3x2( double x )  { return 3.0 * x * x; }
static double ToyPb_1( double )      { return 1.0; }

static const unsigned int kToyQuadN = 20000;

//! Midpoint quadrature of the realized combined variance at `alpha`.
static double ToyVariance( ToyFn f, ToyFn pb, double q, double alpha )
{
	const double h = 1.0 / (double)kToyQuadN;
	double t1 = 0, t2 = 0, m1 = 0, m2 = 0;
	for( unsigned int i = 0; i < kToyQuadN; ++i ) {
		const double x = ( (double)i + 0.5 ) * h;
		const double fv = f( x );
		const double pbv = pb( x );
		const double den = alpha * pbv + ( 1.0 - alpha );
		const double wb = den > 0 ? alpha * pbv / den : 0.0;
		const double wn = 1.0 - wb;
		t1 += wn * wn * fv * fv * h;
		if( pbv > 0 ) t2 += wb * wb * fv * fv / pbv * h / q;
		m1 += wn * fv * h;
		m2 += wb * fv * h;
	}
	return t1 + t2 - m1 * m1 - m2 * m2;
}

static void ToyMoments( ToyFn f, ToyFn pb, double& Mn, double& Ipre )
{
	const double h = 1.0 / (double)kToyQuadN;
	Mn = 0; Ipre = 0;
	for( unsigned int i = 0; i < kToyQuadN; ++i ) {
		const double x = ( (double)i + 0.5 ) * h;
		const double fv = f( x );
		const double pbv = pb( x );
		Mn += fv * fv * h;
		if( pbv > 0 ) Ipre += fv * fv / pbv * h;
	}
}

static void RunOneToy( const char* name, ToyFn f, ToyFn pb, double q )
{
	double Mn = 0, Ipre = 0;
	ToyMoments( f, pb, Mn, Ipre );

	const double aRealized = Mn / ( Mn + Ipre / q );
	const double aRound6   = Mn / ( Mn + q * Ipre );
	const double aBare     = Mn / ( Mn + Ipre );

	const double vRealized = ToyVariance( f, pb, q, aRealized );
	const double vRound6   = ToyVariance( f, pb, q, aRound6 );
	const double vBare     = ToyVariance( f, pb, q, aBare );

	double vBest = vRealized, aBest = aRealized;
	for( unsigned int i = 1; i < 250; ++i ) {
		const double a = (double)i / 250.0;
		const double v = ToyVariance( f, pb, q, a );
		if( v < vBest ) { vBest = v; aBest = a; }
	}

	std::cout << "    toy " << name << " q=" << q
		<< " | grid-min alpha=" << aBest
		<< " | realized a=" << aRealized << " var=" << vRealized
		<< " (+" << 100.0 * ( vRealized / vBest - 1.0 ) << "%)"
		<< " | bare a=" << aBare << " var=" << vBare
		<< " (+" << 100.0 * ( vBare / vBest - 1.0 ) << "%)"
		<< " | round6 a=" << aRound6 << " var=" << vRound6
		<< " (+" << 100.0 * ( vRound6 / vBest - 1.0 ) << "%)"
		<< std::endl;

	Check( vRealized < vBare,
		"alpha quality: the realized moment beats the bare pre-RR moment" );
	Check( vBare < vRound6,
		"alpha quality: the bare pre-RR moment beats round 6's q*E_pre" );
	Check( vRealized < vRound6,
		"alpha quality: the realized moment beats round 6's q*E_pre" );
}

static void RunAlphaQualityCheck()
{
	std::cout << "DL-84 (alpha quality): which RR wiring minimises the combined variance"
		<< std::endl;

	RunOneToy( "f=x,pb=2x",    ToyF_x,   ToyPb_2x,  0.5 );
	RunOneToy( "f=x,pb=2x",    ToyF_x,   ToyPb_2x,  0.2 );
	RunOneToy( "f=x^2,pb=3x^2", ToyF_x2, ToyPb_3x2, 0.5 );
	RunOneToy( "f=e^-4x,pb=1", ToyF_exp, ToyPb_1,   0.5 );

	// Round-trip: real Monte Carlo draws, fed to the production
	// accumulator under wiring (a), must solve to the closed-form alpha.
	{
		const double q = 0.5;
		double Mn = 0, Ipre = 0;
		ToyMoments( ToyF_x, ToyPb_2x, Mn, Ipre );
		const double aRealized = Mn / ( Mn + Ipre / q );

		OptimalMISAccumulator acc;
		acc.Initialize( 64, 64, MakeConfig() );

		RandomNumberGenerator rng( 8123u );
		const unsigned int N = 200000;
		for( unsigned int i = 0; i < N; ++i ) {
			// NEE technique: x ~ p_n = U(0,1), no roulette.
			const double xn = rng.CanonicalRandom();
			acc.AccumulateCount( 0, 0, kTechniqueNEE );
			acc.Accumulate( 0, 0, (Scalar)( ToyF_x( xn ) * ToyF_x( xn ) ), 1.0, kTechniqueNEE );

			// BSDF technique: x ~ p_b = 2x (inverse CDF sqrt(u)), then
			// roulette at q; the survivor's numerator is the AS-CARRIED
			// f/(p_b q), i.e. wiring (a).
			const double xb = std::sqrt( rng.CanonicalRandom() );
			acc.AccumulateCount( 0, 0, kTechniqueBSDF );
			if( rng.CanonicalRandom() < q ) {
				const double carried = ToyF_x( xb ) / q;		// f / q, over p_b below
				acc.Accumulate( 0, 0, (Scalar)( carried * carried ),
					(Scalar)ToyPb_2x( xb ), kTechniqueBSDF );
			}
		}
		acc.Solve();
		const double solved = (double)acc.GetAlpha( 0, 0 );
		std::cout << "    alpha round-trip: closed form " << aRealized
			<< ", accumulator Solve() " << solved << std::endl;
		Check( std::fabs( solved - aRealized ) < 0.005,
			"alpha quality: Solve() reproduces the closed-form realized-moment alpha" );
	}
}

int main()
{
	GlobalLog();

	RunVolumeSite();
	RunBssrdfSite();
	RunFloorFogSite();
	RunRRConventionCheck();
	RunLightRRConventionCheck();
	RunAlphaQualityCheck();

	std::cout << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	if( failCount == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	}
	return 1;
}
