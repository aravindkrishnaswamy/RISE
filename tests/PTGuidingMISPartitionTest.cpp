//////////////////////////////////////////////////////////////////////
//
//  PTGuidingMISPartitionTest.cpp - Red-proof for DL-74 (docs/
//    DL74_ENV_NEE_GUIDING_PARTITION.md): under active OpenPGL path
//    guiding, PathTracingIntegrator's BSDF-sampling side and
//    LightSampler's next-event-estimation side must feed the SAME
//    function of direction to the MIS heuristic, or the two weights
//    stop summing to one and energy is created (or destroyed) at
//    every guided vertex.
//
//  THE INVARIANT THIS MEASURES
//
//    A white (reflectance 1) Lambertian surface under a CONSTANT-
//    radiance environment re-radiates exactly the environment's own
//    radiance:
//
//      L_out = INTEGRAL_hemisphere (1/PI) * L_env * cos(theta) dw
//            = L_env                                (albedo 1)
//
//    A single path-traced bounce from that surface estimates L_out
//    with exactly TWO strategies -- one env-NEE sample (weighted
//    w_nee) plus one BSDF/guided continuation that escapes to the
//    environment (weighted w_bsdf).  Their expectation is L_env if
//    and ONLY IF w_nee(w) + w_bsdf(w) == 1 for every direction w.
//    Any partition-of-unity violation shows up directly as a furnace
//    reading != 1.0 -- no reference render, no reference integrator,
//    no tolerance-shopping: the target is a closed form.
//
//    The same construction with an AREA light replacing the
//    environment (row (d)) uses the OTHER MIS pair on the same two
//    sides: `LightSampler`'s area-light NEE arm against
//    PathTracingIntegrator's emitter-HIT weight.  A uniform-radiance
//    sphere of radius R whose centre sits at distance d along the
//    surface normal (R < d, fully above the horizon) gives the
//    closed form
//
//      L_out = albedo * L_e * sin^2(theta_max) = L_e * R^2 / d^2
//
//    so that row is closed-form too.
//
//  WHAT WAS BROKEN (red on 8fcce0bf, the pre-fix HEAD)
//
//    The pre-fix code used the guided COMBINED pdf on the BSDF side
//    (`effectiveBsdfPdf`, the value the continuation's throughput was
//    actually divided by) and, for the env arm only and only in
//    one-sample-MIS mode, a DIFFERENTLY-BUILT combined pdf on the NEE
//    side.  The two disagreed in four independent ways:
//
//      1. alpha.  The escape side scales the base alpha by the
//         per-cell learned weight (`min(1, alpha*2*GetCellAlpha)`)
//         when `guidingLearnedAlpha` is on -- the DEFAULT -- while the
//         NEE side used the raw base `rc.guidingAlpha`.  Row (b).
//      2. alpha again.  `GuidingEffectiveAlpha` additionally HALVES
//         alpha for an `eRayReflection` lobe; NEE, which runs before
//         the lobe is chosen, could not and did not.  Row (c).
//      3. cosine product.  NEE applied `ApplyCosineProduct` to the
//         guide distribution unconditionally; the escape side applied
//         it only for `eRayDiffuse`.  Row (c).
//      4. coverage.  The area-light NEE arm got NO guiding term at all
//         (row (d)), and RIS-mode guiding was excluded outright (row
//         (e)), while both of their BSDF-side partners were guided.
//
//    Row (a) -- fixed alpha, diffuse lobe, environment, one-sample
//    mode -- is the one configuration where the two constructions
//    coincided, and is included as a CONTROL: it must be green both
//    before and after the fix, which is what proves the other four
//    rows are measuring the asymmetry and not the harness.
//
//  THE FIX THIS PINS (see docs/DL74_ENV_NEE_GUIDING_PARTITION.md)
//
//    Separate the guided pdf's two roles.  The THROUGHPUT denominator
//    stays the true sampling density (per-lobe alpha, per-lobe cosine
//    convention, combined/RIS pdf) -- untouched, because dividing by
//    anything else would bias the estimator.  The MIS PARTNER density
//    becomes a lobe-INDEPENDENT NOMINAL pdf
//
//      p_mis(w) = alpha_nom * guide(w) + (1 - alpha_nom) * p_agg(w)
//
//    evaluated identically on every side.  MIS is unbiased for ANY
//    weights that partition to one, whether or not they are built
//    from the true sampling densities -- so the nominal pdf costs
//    nothing in correctness and buys exact partition.
//
//  WHY A MEAN AND NOT A SINGLE SAMPLE
//
//    One NEE sample plus one escape sample sum to L_env only in
//    EXPECTATION (each is a Monte Carlo estimate of its own half of
//    the same integral).  The estimator is bounded here by
//    construction -- the escape throughput cannot exceed
//    1/(1-alpha) and the NEE contribution cannot exceed
//    L_env/envPdf_min -- so a fixed, independently-seeded batch
//    converges tightly and the assertion tolerance (1.5%) sits far
//    below every measured pre-fix error (the smallest was ~7%).
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
#include "../src/Library/Interfaces/IRadianceMap.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Rendering/EnvironmentSampler.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/PathGuidingField.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "TestStubObject.h"

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

static void CheckRel( double measured, double expected, double relTol, const char* what )
{
	const double denom = std::fabs( expected ) > 1e-12 ? std::fabs( expected ) : 1.0;
	const double relErr = std::fabs( measured - expected ) / denom;
	if( relErr <= relTol ) {
		passCount++;
		std::cout << "    ok   " << what << "  measured=" << measured
			<< " expected=" << expected << " relErr=" << (relErr * 100.0) << "%" << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << what << "  measured=" << measured
			<< " expected=" << expected << " relErr=" << (relErr * 100.0)
			<< "% tol=" << (relTol * 100.0) << "%" << std::endl;
	}
}

#ifdef RISE_ENABLE_OPENPGL

//////////////////////////////////////////////////////////////////////
// A no-op rasterizer output -- the fixture renders one throwaway
// frame only to make the Job build its real RayCaster / LightSampler /
// EnvironmentSampler; the image is never inspected.
//////////////////////////////////////////////////////////////////////
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

//////////////////////////////////////////////////////////////////////
// Row (c)'s ONLY difference from rows (a)/(b): the emitted lobe is
// tagged `eRayReflection` instead of `eRayDiffuse`.
//
// That tag is the sole input to BOTH asymmetries this row exists to
// measure -- `GuidingEffectiveAlpha`'s glossy half-damping, and
// PART 3's `pS->type == eRayDiffuse` gate on ApplyCosineProduct.
// Re-tagging a REAL LambertianSPF's output (rather than introducing a
// real glossy SPF) keeps every other variable -- the sampling
// density, the BSDF value, the geometric-horizon gate, and above all
// the albedo-1 white-furnace closed form -- bit-identical to rows
// (a)/(b), so a failure here can only be the lobe-type-dependent
// guiding asymmetry.  A production glossy lobe (GGXSPF's specular
// lobe, IsotropicPhongSPF, CoatedSPF's coat lobe) reaches the exact
// same integrator code with the same tag.
//////////////////////////////////////////////////////////////////////
class RetypingSPF : public virtual ISPF, public virtual Reference
{
	const ISPF& real;
	ScatteredRay::ScatRayType newType;

	void Retype( ScatteredRayContainer& rays ) const
	{
		for( unsigned int i = 0; i < rays.Count(); ++i ) {
			rays[i].type = newType;
		}
	}

protected:
	~RetypingSPF() override {}

public:
	RetypingSPF( const ISPF& delegate, ScatteredRay::ScatRayType t )
		: real( delegate ), newType( t ) {}

	void Scatter( const RayIntersectionGeometric& ri, ISampler& sampler,
		ScatteredRayContainer& rays, const IORStack& stack ) const override
	{
		real.Scatter( ri, sampler, rays, stack );
		Retype( rays );
	}
	void ScatterNM( const RayIntersectionGeometric& ri, ISampler& sampler, Scalar nm,
		ScatteredRayContainer& rays, const IORStack& stack ) const override
	{
		real.ScatterNM( ri, sampler, nm, rays, stack );
		Retype( rays );
	}
	Scalar Pdf( const RayIntersectionGeometric& ri, const Vector3& wo,
		const IORStack& stack ) const override { return real.Pdf( ri, wo, stack ); }
	Scalar PdfNM( const RayIntersectionGeometric& ri, const Vector3& wo, Scalar nm,
		const IORStack& stack ) const override { return real.PdfNM( ri, wo, nm, stack ); }
};

class RetypedLambertianMaterial : public LambertianMaterial
{
	RetypingSPF* pRetyped;
protected:
	~RetypedLambertianMaterial() override { safe_release( pRetyped ); }
public:
	RetypedLambertianMaterial( const IPainter& ref, ScatteredRay::ScatRayType t )
		: LambertianMaterial( ref )
	{
		pRetyped = new RetypingSPF( *pSPF, t );
		GlobalLog()->PrintNew( pRetyped, __FILE__, __LINE__, "retyping SPF" );
	}
	ISPF* GetSPF() const override { return pRetyped; }
};

//////////////////////////////////////////////////////////////////////
// Fixture: loads a scene, renders one cheap frame so RayCaster::
// AttachScene runs (which is what builds the real LightSampler and,
// for the env rows, its EnvironmentSampler -- note AttachScene calls
// Prepare() BEFORE SetEnvironmentSampler(), so the sampler only
// exists after a render, never straight after the load), then exposes
// the live IRayCaster* / IScene* through the same accessor production
// code uses (PixelBasedRasterizerHelper::GetRayCaster).
//////////////////////////////////////////////////////////////////////
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
		std::snprintf( path, sizeof(path), "/tmp/pt_guiding_mis_%s_%d.RISEscene",
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

		std::srand( 991u );
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
// Scene text.
//////////////////////////////////////////////////////////////////////
static std::string EnvOnlyScene()
{
	return std::string(
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 0.6 0.6 0.6\n}\n"
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

// Row (d): NO radiance map at all (so PT's env-escape arm is inert and
// the only MIS pair exercised is area-NEE against the emitter hit), one
// uniform-radiance emissive sphere centred on the shading point's
// normal axis.
static const Scalar kSphereRadius = 2.0;
static const Scalar kSphereDist   = 5.0;

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
		"sphere_geometry\n{\n\tname lightball\n\tradius " << kSphereRadius << "\n}\n"
		"\n"
		"standard_object\n{\n\tname light_object\n\tgeometry lightball\n"
		"\tmaterial emitter\n\tposition 0 0 " << kSphereDist << "\n}\n"
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

//////////////////////////////////////////////////////////////////////
// A SKEWED trained guiding field at the shading point: incident
// radiance concentrated in a narrow cone about `axis`, so the learned
// guide density varies by orders of magnitude across the hemisphere.
// That spread is what turns a pdf disagreement between the two MIS
// sides into a large, unmistakable furnace error -- a near-uniform
// guide would make the two constructions numerically indistinguishable
// no matter how differently they were built.
//
// Every sample is deposited at the SAME position, so the field has a
// single spatial region and `InitDistribution`'s stochastic region
// lookup is deterministic (asserted below).
//////////////////////////////////////////////////////////////////////
static PathGuidingField* BuildSkewedField( const Point3& at, const Vector3& axis )
{
	PathGuidingConfig config;
	config.enabled = true;
	PathGuidingField* guide = new PathGuidingField( config,
		Point3( at.x - 2, at.y - 2, at.z - 2 ), Point3( at.x + 2, at.y + 2, at.z + 2 ) );
	GlobalLog()->PrintNew( guide, __FILE__, __LINE__, "skewed guiding field" );

	guide->BeginTrainingIteration();
	const unsigned int kSamples = 16384;
	for( unsigned int i = 0; i < kSamples; ++i ) {
		const Scalar z = -1 + 2 * ( i + 0.5 ) / kSamples;
		const Scalar phi = i * 2.399963229728653;
		const Scalar r = std::sqrt( 1 - z * z );
		const Vector3 dir( r * std::cos( phi ), r * std::sin( phi ), z );
		// Sharply peaked "incident radiance": cos^64 about `axis`.
		const Scalar c = Vector3Ops::Dot( dir, axis );
		const Scalar lum = c > 0 ? std::pow( (double)c, 64.0 ) : 0.0;
		if( lum <= 1e-9 ) {
			guide->AddZeroValueSample( at, dir );
		} else {
			guide->AddSample( at, dir, 1, 1 / ( 4 * PI ), lum, false );
		}
	}
	guide->EndTrainingIteration();
	return guide;
}

//////////////////////////////////////////////////////////////////////
// Drive the per-cell learned alpha (Mueller 2017 v2 sigmoid) to a
// chosen extreme through the SAME public Adam entry point production
// uses.  `guidePdf < bsdfPdf` gives a positive gradient, which the
// Adam step subtracts from theta -> alpha falls; Adam's normalisation
// makes each step approximately `learningRate` in theta, and theta is
// clamped to [-8, 8] (alpha in ~[3.4e-4, 0.9997]).
//////////////////////////////////////////////////////////////////////
static void DriveCellAlphaDown( PathGuidingField& field, uint32_t cellId )
{
	for( unsigned int i = 0; i < 400; ++i ) {
		field.UpdateCellAlpha( cellId, /*bsdfPdf*/ 10.0, /*guidePdf*/ 1.0,
			/*f*/ 1.0, /*combinedPdf*/ 1.0, /*learningRate*/ 0.1 );
	}
}

//////////////////////////////////////////////////////////////////////
// One measurement batch.
//////////////////////////////////////////////////////////////////////
struct RowConfig
{
	const char*			name;
	bool				learnedAlpha;
	bool				skewCellAlpha;		///< drive the per-cell sigmoid to its floor
	bool				glossyLobe;			///< tag the lobe eRayReflection
	GuidingSamplingType	samplingType;
	Scalar				alpha;				///< 0 disables guiding entirely (reference row)
};

static Scalar RunBatch(
	const Fixture& fx,
	const IMaterial& material,
	PathGuidingField* guide,
	const RowConfig& cfg,
	unsigned int nSamples,
	unsigned int seedBase )
{
	PathTracingIntegrator* integrator =
		new PathTracingIntegrator( ManifoldSolverConfig(), StabilityConfig() );
	GlobalLog()->PrintNew( integrator, __FILE__, __LINE__, "integrator" );
	// One scattering event is all the geometry permits (the env rows have
	// no objects at all; the area row's only object is a luminaire with
	// `material none`, hence no SPF to continue through), so this cap is
	// belt-and-braces -- the control rows' exact closed-form agreement is
	// what actually proves no second bounce contributes.
	integrator->SetMaxPathDepth( 2 );

	StubObject* shadingObject = new StubObject();
	GlobalLog()->PrintNew( shadingObject, __FILE__, __LINE__, "shading object" );

	const RasterizerState rast{};
	Scalar sum = 0;

	for( unsigned int s = 0; s < nSamples; ++s ) {
		RandomNumberGenerator rng( seedBase + s );
		IndependentSampler sampler( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		if( cfg.alpha > 0 ) {
			rc.pGuidingField = guide;
			rc.guidingAlpha = cfg.alpha;
			rc.guidingLearnedAlpha = cfg.learnedAlpha;
			rc.maxGuidingDepth = 4;
			rc.guidingSamplingType = cfg.samplingType;
			rc.guidingRISCandidates = 2;
		}

		// Front-facing hit at the origin with the normal along +Z: the
		// incoming ray travels -Z, so Dot(dir, N) < 0 and both
		// LambertianSPF and GuidingCosineNormal keep the +Z hemisphere.
		RayIntersection hit( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), rast );
		hit.geometric.bHit = true;
		hit.geometric.range = 1;
		hit.geometric.ptIntersection = Point3( 0, 0, 0 );
		hit.geometric.vNormal = Vector3( 0, 0, 1 );
		hit.geometric.vGeomNormal = Vector3( 0, 0, 1 );
		hit.geometric.onb.CreateFromW( hit.geometric.vNormal );
		hit.pObject = shadingObject;
		hit.pMaterial = &material;

		IORStack stack( 1.0 );

		const RISEPel r = integrator->IntegrateFromHit(
			rc, rast, hit, *fx.pScene, *fx.pCaster, sampler,
			/*pRadianceMap*/ 0, /*startDepth*/ 0, stack,
			/*bsdfPdf_*/ 0, /*bsdfTimesCos_*/ RISEPel( 0, 0, 0 ),
			/*considerEmission_*/ true, /*importance_*/ 1,
			IRayCaster::RAY_STATE::eRayDiffuse,
			0, 0, 0, 0, 0, 0, false, false );

		sum += ColorMath::MaxValue( r );
	}

	integrator->release();
	shadingObject->release();

	return sum / nSamples;
}

//////////////////////////////////////////////////////////////////////
// Environment rows (a), (b), (c), (e).
//////////////////////////////////////////////////////////////////////
static void RunEnvRows()
{
	std::cout << "DL-74 env furnace: one env-NEE + one guided escape must partition to 1"
		<< std::endl;

	Fixture fx;
	Check( fx.Build( EnvOnlyScene(), "env" ), "env fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	Check( pGlobal != 0, "env fixture has a global radiance map" );
	const LightSampler* pLS = fx.pCaster->GetLightSampler();
	Check( pLS != 0, "env fixture has a LightSampler" );
	if( !pGlobal || !pLS ) return;
	const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
	Check( pES != 0 && pES->IsValid(), "env fixture has a valid EnvironmentSampler" );
	if( !pES || !pES->IsValid() ) return;

	// The furnace target is the map's OWN radiance, read back through the
	// same interface the integrator reads (never an assumed formula).
	const RasterizerState rast{};
	const Ray probe( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	const Scalar Lenv = ColorMath::MaxValue( pGlobal->GetRadiance( probe, rast ) );
	Check( Lenv > 0, "env radiance probe is positive" );
	if( Lenv <= 0 ) return;

	UniformColorPainter* whiteP = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( whiteP, __FILE__, __LINE__, "white" );
	LambertianMaterial* diffuseMat = new LambertianMaterial( *whiteP );
	GlobalLog()->PrintNew( diffuseMat, __FILE__, __LINE__, "diffuse furnace material" );
	RetypedLambertianMaterial* glossyMat =
		new RetypedLambertianMaterial( *whiteP, ScatteredRay::eRayReflection );
	GlobalLog()->PrintNew( glossyMat, __FILE__, __LINE__, "glossy-tagged furnace material" );

	PathGuidingField* guide = BuildSkewedField(
		Point3( 0, 0, 0 ), Vector3Ops::Normalize( Vector3( 0.6, 0.0, 0.8 ) ) );
	Check( guide->IsTrained(), "skewed guiding field is trained" );

	// One spatial region -> the stochastic region lookup is deterministic,
	// so the cell whose learned alpha row (b) drives is the cell the
	// integrator resolves.
	uint32_t cellId = 0;
	{
		GuidingDistributionHandle h0, h1;
		const bool ok0 = guide->InitDistribution( h0, Point3( 0, 0, 0 ), 0.05 );
		const bool ok1 = guide->InitDistribution( h1, Point3( 0, 0, 0 ), 0.95 );
		Check( ok0 && ok1, "guiding distribution initialises at the shading point" );
		cellId = guide->GetCellId( h0 );
		Check( ok0 && ok1 && cellId == guide->GetCellId( h1 ),
			"the field resolves ONE spatial region at the shading point" );
	}

	const unsigned int kN = 120000;

	// Reference row: guiding fully off (alpha 0).  Proves the harness
	// itself -- geometry, albedo, env sampler, NEE/BSDF MIS pair --
	// reproduces the closed form before any guiding is involved.
	{
		RowConfig cfg{ "reference (no guiding)", false, false, false, eGuidingOneSampleMIS, 0.0 };
		const Scalar m = RunBatch( fx, *diffuseMat, guide, cfg, kN, 1000 );
		CheckRel( m, Lenv, 0.015,
			"reference: unguided white furnace under a constant env reads L_env" );
	}

	// Row (a) CONTROL: fixed alpha + diffuse lobe + one-sample MIS is the
	// one configuration whose two pdf constructions already agreed.
	{
		RowConfig cfg{ "(a)", false, false, false, eGuidingOneSampleMIS, 0.9 };
		const Scalar m = RunBatch( fx, *diffuseMat, guide, cfg, kN, 2000 );
		CheckRel( m, Lenv, 0.015,
			"(a) CONTROL guidingLearnedAlpha=false + diffuse lobe: furnace reads L_env" );
	}

	// Row (b): learned per-cell alpha (the DEFAULT) driven to its floor,
	// so the escape side's effective alpha and the NEE side's base alpha
	// are maximally far apart.
	{
		DriveCellAlphaDown( *guide, cellId );
		GuidingDistributionHandle h;
		guide->InitDistribution( h, Point3( 0, 0, 0 ), 0.5 );
		const Scalar cellAlpha = guide->GetCellAlpha( h );
		std::cout << "    row (b) learned cell alpha driven to " << cellAlpha << std::endl;
		Check( cellAlpha < 0.1,
			"(b) the learned per-cell alpha really is far from its 0.5 neutral value" );

		RowConfig cfg{ "(b)", true, true, false, eGuidingOneSampleMIS, 0.9 };
		const Scalar m = RunBatch( fx, *diffuseMat, guide, cfg, kN, 3000 );
		CheckRel( m, Lenv, 0.015,
			"(b) guidingLearnedAlpha=true (default) + diffuse lobe: furnace reads L_env" );
	}

	// Row (c): the lobe is tagged eRayReflection -- GuidingEffectiveAlpha
	// halves alpha for it and PART 3's ApplyCosineProduct gate skips it,
	// while the NEE side does neither.  Run with the LEARNED alpha back at
	// its neutral setting (learnedAlpha=false) so this row isolates the
	// lobe-type asymmetry from row (b)'s alpha asymmetry.
	{
		RowConfig cfg{ "(c)", false, false, true, eGuidingOneSampleMIS, 0.9 };
		const Scalar m = RunBatch( fx, *glossyMat, guide, cfg, kN, 4000 );
		CheckRel( m, Lenv, 0.015,
			"(c) eRayReflection lobe (half alpha + no cosine product on the escape side): "
			"furnace reads L_env" );
	}

	// Row (e): RIS-mode guiding.  The escape side's effective pdf is the
	// RIS-normalised one; the nominal MIS pdf does not depend on the
	// sampling mode, so the same partition must hold.
	{
		RowConfig cfg{ "(e)", false, false, false, eGuidingRIS, 0.9 };
		const Scalar m = RunBatch( fx, *diffuseMat, guide, cfg, kN, 5000 );
		CheckRel( m, Lenv, 0.015,
			"(e) RIS-mode guiding: furnace reads L_env" );
	}

	guide->release();
	glossyMat->release();
	diffuseMat->release();
	whiteP->release();
}

//////////////////////////////////////////////////////////////////////
// Row (d): area light -- LightSampler's area-NEE arm against
// PathTracingIntegrator's emitter-HIT weight.
//////////////////////////////////////////////////////////////////////
static void RunAreaLightRow()
{
	std::cout << "DL-74 area-light furnace: one area-NEE + one guided emitter hit must partition to 1"
		<< std::endl;

	Fixture fx;
	Check( fx.Build( AreaLightScene(), "area" ), "area fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;
	Check( fx.pScene->GetGlobalRadianceMap() == 0,
		"area fixture has NO env map (so only the area-light MIS pair is exercised)" );

	// Read the emitter's actual radiance back through the live scene
	// rather than re-deriving exitance/PI/scale conventions.
	const RasterizerState rast{};
	Scalar Le = 0;
	{
		RayIntersection probe( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), rast );
		fx.pScene->GetObjects()->IntersectRay( probe, true, true, false );
		Check( probe.geometric.bHit && probe.pMaterial != 0,
			"area fixture: the emissive sphere is hit straight up the normal axis" );
		if( !probe.geometric.bHit || !probe.pMaterial ) return;
		IEmitter* pEm = probe.pMaterial->GetEmitter();
		Check( pEm != 0, "area fixture: the sphere carries an emitter" );
		if( !pEm ) return;
		Le = ColorMath::MaxValue( pEm->emittedRadiance(
			probe.geometric, -probe.geometric.ray.Dir(), probe.geometric.vNormal ) );
		Check( Le > 0, "area fixture: emitted radiance probe is positive" );
		if( Le <= 0 ) return;
	}

	// Closed form for a uniform-radiance sphere centred on the normal
	// axis, fully above the horizon:
	//   E = L_e * PI * sin^2(theta_max),  sin(theta_max) = R / d
	//   L_out = albedo * E / PI = L_e * R^2 / d^2      (albedo == 1)
	const Scalar expected = Le * ( kSphereRadius * kSphereRadius ) /
		( kSphereDist * kSphereDist );

	UniformColorPainter* whiteP = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( whiteP, __FILE__, __LINE__, "white" );
	LambertianMaterial* diffuseMat = new LambertianMaterial( *whiteP );
	GlobalLog()->PrintNew( diffuseMat, __FILE__, __LINE__, "diffuse furnace material" );

	// Skew the guide AWAY from the light so the guided proposal and the
	// light-sampling proposal disagree strongly -- the regime where a
	// mismatched MIS pair does the most damage.
	PathGuidingField* guide = BuildSkewedField(
		Point3( 0, 0, 0 ), Vector3Ops::Normalize( Vector3( 0.9, 0.0, 0.44 ) ) );
	Check( guide->IsTrained(), "area row: skewed guiding field is trained" );

	const unsigned int kN = 200000;

	{
		RowConfig cfg{ "area reference", false, false, false, eGuidingOneSampleMIS, 0.0 };
		const Scalar m = RunBatch( fx, *diffuseMat, guide, cfg, kN, 6000 );
		CheckRel( m, expected, 0.02,
			"area reference: unguided white furnace under a spherical emitter reads L_e*R^2/d^2" );
	}

	{
		RowConfig cfg{ "(d)", true, false, false, eGuidingOneSampleMIS, 0.9 };
		const Scalar m = RunBatch( fx, *diffuseMat, guide, cfg, kN, 7000 );
		CheckRel( m, expected, 0.02,
			"(d) AREA-light NEE vs the guided emitter-hit weight: furnace reads L_e*R^2/d^2" );
	}

	guide->release();
	diffuseMat->release();
	whiteP->release();
}

static void Run()
{
	RunEnvRows();
	RunAreaLightRow();
}

#else

static void Run()
{
	std::cout << "DL-74 guiding partition coverage unavailable: build without OpenPGL" << std::endl;
}

#endif

int main()
{
	GlobalLog();
	Run();

	std::cout << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	if( failCount == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	}
	return 1;
}
