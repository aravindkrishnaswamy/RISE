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

//! Drives the BSSRDF site and hands back the RAW per-tile training state.
//! Seeded per sample, so two calls against fixtures that differ ONLY in
//! environment brightness walk identical paths.
static void DriveBssrdfSite(
	const Fixture& fx,
	const PathTracingIntegrator& integrator,
	Object& object,
	SubSurfaceScatteringMaterial& material,
	OptimalMISAccumulator& acc,
	double& sumBsdf,
	unsigned int& countBsdf )
{
	const RasterizerState rast{};
	// Exit point on the sphere's +Z pole, ray arriving from outside.
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
			/*considerEmission_*/ true, /*importance_*/ 1,
			IRayCaster::RAY_STATE::eRayDiffuse,
			0, 0, 0, 0, 0, 0, false, false );
	}
	double sumNee = 0;
	unsigned int countNee = 0;
	acc.GetTileTraining( 0, 0, sumNee, sumBsdf, countNee, countBsdf );
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

int main()
{
	GlobalLog();

	RunVolumeSite();
	RunBssrdfSite();

	std::cout << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	if( failCount == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	}
	return 1;
}
