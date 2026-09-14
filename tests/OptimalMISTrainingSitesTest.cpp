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
static std::string VolumeScene()
{
	return std::string(
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n"
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

	OptimalMISAccumulator acc;
	acc.Initialize( 64, 64, MakeConfig() );
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
static std::string EnvOnlyScene()
{
	return std::string(
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n"
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
		hit.pObject = object;
		hit.pMaterial = material;

		IORStack stack( 1.0 );

		integrator->IntegrateFromHit(
			rc, rast, hit, *fx.pScene, *fx.pCaster, sampler,
			/*pRadianceMap*/ 0, /*startDepth*/ 0, stack,
			/*bsdfPdf_*/ 0, /*bsdfTimesCos_*/ RISEPel( 0, 0, 0 ),
			/*considerEmission_*/ true, /*importance_*/ 1,
			IRayCaster::RAY_STATE::eRayDiffuse,
			0, 0, 0, 0, 0, 0, false, false );
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
