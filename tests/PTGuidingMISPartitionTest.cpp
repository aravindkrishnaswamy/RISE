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
#include "../src/Library/Materials/PolishedSPF.h"
#include "../src/Library/Materials/TranslucentSPF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
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
// Row (f)'s ONLY difference from row (a): this SPF's sampling density
// depends on the IOR STACK it is handed.
//
// WHY THAT IS THE VARIABLE UNDER TEST.  The BSDF-sampling side of the
// MIS pair evaluates the material's aggregate pdf against the LIVE
// stack (`PTEvalPdfAtSurface(pSPF, ..., iorStack)`), while
// `LightSampler`'s four NEE arms evaluate it against a
// `static const IORStack defaultIOR(1.0)` sentinel.  For any material
// whose pdf reads the stack the two `p_aggregate` values differ, and
// the nominal MIS density built on top of them stops being ONE
// function of direction -- the exact failure DL-74 exists to close.
//
// Production materials that read the stack in `Pdf()`:
//   * `PolishedSPF::Pdf`   -- `ior_stack.top()` drives the Fresnel Rs
//                             that weights its specular/diffuse lobe
//                             mixture (PolishedSPF.cpp).
//   * `TranslucentSPF::Pdf` -- `!ior_stack.containsCurrent()` selects
//                             whether the geometric-horizon gate runs
//                             (TranslucentSPF.cpp).
// `RealMaterialStackPremise()` below asserts that dependence directly
// on the production code, so this decorator is a controlled stand-in
// for a measured behaviour, not a straw man.
//
// WHY A DECORATOR AND NOT `polished_material` ITSELF.  The furnace
// target here is a CLOSED FORM (`L_out == L_env` for an albedo-1
// surface under a constant environment), which needs a material whose
// directional albedo is exactly 1.  Neither PolishedSPF nor
// TranslucentSPF is exactly energy-conserving, so neither has a
// closed-form furnace value, and their unguided readings cannot serve
// as a reference either (a multi-lobe material's unguided escape side
// stores the SELECTED lobe's pdf while NEE uses the aggregate -- the
// separate, still-open DL-67 mismatch).  This decorator keeps the
// albedo-1 Lambertian BRDF (so the closed form holds exactly) and
// varies ONLY the stack-dependence of the sampling density.
//
// THE TWO LOBES, and why both are legitimate sampling densities:
//   stack.top() <= 1.2 (the `defaultIOR` sentinel's value):
//       delegate to the real LambertianSPF -- cosine hemisphere about
//       the shading normal, pdf = cos/PI, kray = reflectance.
//   stack.top() >  1.2 (inside the dielectric, the live stack):
//       uniform over the hemisphere about a TILTED axis
//       n' = normalize(N + kTilt*(0,1,0)), pdf = 1/(2*PI) inside that
//       hemisphere and 0 outside it, kray = reflectance*2*max(0,cos).
// MIS is unbiased for ANY valid sampling density, so the furnace still
// reads L_env exactly once the two sides agree -- including over the
// wedge where the tilted hemisphere does NOT cover the upper
// hemisphere, where `p_aggregate` is legitimately 0 and DL-74's
// documented "no BSDF-side partner exists, NEE takes the whole sample"
// rule has to fire on both sides at once.
//
// The tilted lobe always emits exactly one ray (no horizon rejection):
// a Scatter() that sometimes returns NOTHING would make PART 3 break
// out before the guiding block and silently drop the guided half of
// the one-sample mixture, which would bias the row for a reason that
// has nothing to do with the stack.
//////////////////////////////////////////////////////////////////////
static const Scalar kStackAwareTilt = 1.0;
static const Scalar kInsideIOR      = 1.5;

class StackAwareSPF : public virtual ISPF, public virtual Reference
{
	const ISPF& real;
	const IPainter& reflectance;

	static bool Inside( const IORStack& stack ) { return stack.top() > 1.2; }

	static Vector3 TiltedAxis( const RayIntersectionGeometric& ri )
	{
		const Vector3 n = ( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO )
			? -ri.onb.w() : ri.onb.w();
		return Vector3Ops::Normalize(
			n + Vector3( 0, kStackAwareTilt, 0 ) );
	}

	void ScatterTilted( const RayIntersectionGeometric& ri, ISampler& sampler,
		ScatteredRay& out ) const
	{
		const Vector3 axis = TiltedAxis( ri );
		OrthonormalBasis3D onb;
		onb.CreateFromW( axis );

		const Scalar u0 = sampler.Get1D();
		const Scalar u1 = sampler.Get1D();
		const Scalar z = u0;								// uniform in cos-free z
		const Scalar r = std::sqrt( r_max( Scalar(0), Scalar(1) - z * z ) );
		const Scalar phi = TWO_PI * u1;
		const Vector3 dir = onb.Transform(
			Vector3( r * std::cos( phi ), r * std::sin( phi ), z ) );

		out.type = ScatteredRay::eRayDiffuse;
		out.ray.Set( ri.ptIntersection, dir );
		out.pdf = 1.0 / TWO_PI;
		out.isDelta = false;
	}

	Scalar TiltedPdf( const RayIntersectionGeometric& ri, const Vector3& wo ) const
	{
		return Vector3Ops::Dot( wo, TiltedAxis( ri ) ) > 0 ? Scalar( 1.0 / TWO_PI ) : Scalar( 0 );
	}

	//! kray is the FULL throughput factor BSDF*cos/pdf (ISPF.h's
	//! contract -- LambertianSPF's cosine-sampled kray is exactly the
	//! reflectance for the same reason).
	Scalar TiltedCosFactor( const RayIntersectionGeometric& ri, const Vector3& wo ) const
	{
		const Vector3 n = ( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO )
			? -ri.onb.w() : ri.onb.w();
		const Scalar c = Vector3Ops::Dot( wo, n );
		return c > 0 ? 2.0 * c : 0.0;
	}

protected:
	~StackAwareSPF() override {}

public:
	StackAwareSPF( const ISPF& delegate, const IPainter& ref )
		: real( delegate ), reflectance( ref ) {}

	void Scatter( const RayIntersectionGeometric& ri, ISampler& sampler,
		ScatteredRayContainer& rays, const IORStack& stack ) const override
	{
		if( !Inside( stack ) ) {
			real.Scatter( ri, sampler, rays, stack );
			return;
		}
		ScatteredRay s;
		ScatterTilted( ri, sampler, s );
		s.kray = reflectance.GetColor( ri ) * TiltedCosFactor( ri, s.ray.Dir() );
		rays.AddScatteredRay( s );
	}

	void ScatterNM( const RayIntersectionGeometric& ri, ISampler& sampler, Scalar nm,
		ScatteredRayContainer& rays, const IORStack& stack ) const override
	{
		if( !Inside( stack ) ) {
			real.ScatterNM( ri, sampler, nm, rays, stack );
			return;
		}
		ScatteredRay s;
		ScatterTilted( ri, sampler, s );
		s.krayNM = reflectance.GetColorNM( ri, nm ) * TiltedCosFactor( ri, s.ray.Dir() );
		rays.AddScatteredRay( s );
	}

	Scalar Pdf( const RayIntersectionGeometric& ri, const Vector3& wo,
		const IORStack& stack ) const override
	{
		return Inside( stack ) ? TiltedPdf( ri, wo ) : real.Pdf( ri, wo, stack );
	}

	Scalar PdfNM( const RayIntersectionGeometric& ri, const Vector3& wo, Scalar nm,
		const IORStack& stack ) const override
	{
		return Inside( stack ) ? TiltedPdf( ri, wo ) : real.PdfNM( ri, wo, nm, stack );
	}
};

class StackAwareLambertianMaterial : public LambertianMaterial
{
	StackAwareSPF* pStackAware;
protected:
	~StackAwareLambertianMaterial() override { safe_release( pStackAware ); }
public:
	StackAwareLambertianMaterial( const IPainter& ref )
		: LambertianMaterial( ref )
	{
		pStackAware = new StackAwareSPF( *pSPF, ref );
		GlobalLog()->PrintNew( pStackAware, __FILE__, __LINE__, "stack-aware SPF" );
	}
	ISPF* GetSPF() const override { return pStackAware; }
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
	unsigned int seedBase,
	// DL-74 P2 (round-3 review): when non-null, the walk starts INSIDE a
	// dielectric -- this object is pushed onto the entry IORStack with
	// ior 1.5, so `stack.top()` is 1.5 rather than the ambient 1.0.  The
	// NEE arms' historical `IORStack(1.0)` sentinel therefore evaluates
	// the material's aggregate pdf under a DIFFERENT stack than the
	// escape side's `PTEvalPdfAtSurface(..., iorStack)` does.
	const IObject* pEnclosing = 0 )
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
		if( pEnclosing ) {
			stack.SetCurrentObject( pEnclosing );
			stack.push( 1.5 );
		}

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

//////////////////////////////////////////////////////////////////////
// PREMISE for row (f): production materials really do read the IOR
// stack inside `Pdf()`, so the `IORStack(1.0)` sentinel the NEE arms
// historically used is not equivalent to the live stack the escape
// side uses.  Deterministic -- no sampling, no render.
//////////////////////////////////////////////////////////////////////
static void RealMaterialStackPremise()
{
	std::cout << "DL-74 P2 premise: a production material's aggregate Pdf reads the IOR stack"
		<< std::endl;

	const RasterizerState rast{};
	StubObject* enclosing = new StubObject();
	GlobalLog()->PrintNew( enclosing, __FILE__, __LINE__, "enclosing object" );

	RayIntersectionGeometric ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), rast );
	ri.bHit = true;
	ri.range = 1;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( ri.vNormal );

	IORStack ambient( 1.0 );
	IORStack inside( 1.0 );
	inside.SetCurrentObject( enclosing );
	inside.push( kInsideIOR );

	const Vector3 wo = Vector3Ops::Normalize( Vector3( 0.3, 0.2, 0.93 ) );

	// PolishedSPF: ior_stack.top() feeds the Fresnel Rs that weights the
	// specular-vs-diffuse lobe mixture its Pdf returns.
	{
		UniformColorPainter* rd = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );
		UniformScalarPainter* tau = new UniformScalarPainter( 1.0 );
		UniformScalarPainter* nt = new UniformScalarPainter( 1.6 );
		UniformScalarPainter* s = new UniformScalarPainter( 50000.0 );
		PolishedSPF* spf = new PolishedSPF( *rd, *tau, *nt, *s, false );
		GlobalLog()->PrintNew( spf, __FILE__, __LINE__, "polished spf" );

		const Scalar pAmbient = spf->Pdf( ri, wo, ambient );
		const Scalar pInside  = spf->Pdf( ri, wo, inside );
		std::cout << "    PolishedSPF::Pdf  stack-top 1.0 -> " << pAmbient
			<< " ,  stack-top " << kInsideIOR << " -> " << pInside << std::endl;
		Check( std::fabs( (double)pAmbient - (double)pInside ) > 1e-6,
			"premise: PolishedSPF::Pdf differs between the defaultIOR sentinel and the live stack" );

		spf->release();
		s->release();
		nt->release();
		tau->release();
		rd->release();
	}

	// TranslucentSPF: `!ior_stack.containsCurrent()` selects whether the
	// entry-only geometric-horizon gate runs.  The gate is only
	// observable where the shading normal and the geometric normal
	// disagree, so this probe tilts them apart -- exactly the horizon
	// band the DL-74 review named.
	{
		RayIntersectionGeometric tilted( ri );
		tilted.vNormal = Vector3Ops::Normalize( Vector3( 0.0, 0.6, 0.8 ) );
		tilted.onb.CreateFromW( tilted.vNormal );
		const Vector3 band = Vector3Ops::Normalize( Vector3( 0.0, 0.92, -0.02 ) );

		UniformColorPainter* rf = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );
		UniformColorPainter* tr = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );
		UniformScalarPainter* ext = new UniformScalarPainter( 1.0 );
		UniformScalarPainter* nn = new UniformScalarPainter( 10.0 );
		UniformScalarPainter* sc = new UniformScalarPainter( 0.5 );
		TranslucentSPF* spf = new TranslucentSPF( *rf, *tr, *ext, *nn, *sc );
		GlobalLog()->PrintNew( spf, __FILE__, __LINE__, "translucent spf" );

		// `containsCurrent()` is what the SPF reads, so the stack has to
		// name the SHADING object as current in both probes; only its
		// membership differs.
		IORStack outsideT( 1.0 );
		outsideT.SetCurrentObject( enclosing );
		IORStack insideT( 1.0 );
		insideT.SetCurrentObject( enclosing );
		insideT.push( kInsideIOR );

		const Scalar pOut = spf->Pdf( tilted, band, outsideT );
		const Scalar pIn  = spf->Pdf( tilted, band, insideT );
		std::cout << "    TranslucentSPF::Pdf  not-in-stack -> " << pOut
			<< " ,  in-stack -> " << pIn << std::endl;
		Check( std::fabs( (double)pOut - (double)pIn ) > 1e-6,
			"premise: TranslucentSPF::Pdf differs between the defaultIOR sentinel and the live stack" );

		spf->release();
		sc->release();
		nn->release();
		ext->release();
		tr->release();
		rf->release();
	}

	enclosing->release();
}

//////////////////////////////////////////////////////////////////////
// Row (f): the vertex is INSIDE a dielectric (entry IOR stack top
// 1.5) and the material's aggregate pdf reads the stack.  The escape
// side evaluates that pdf against the live stack; the NEE arms
// evaluated it against `IORStack(1.0)`.  Same white furnace, same
// closed form.
//////////////////////////////////////////////////////////////////////
static void RunIorStackRow()
{
	std::cout << "DL-74 P2: the two sides must evaluate p_aggregate under the SAME IOR stack"
		<< std::endl;

	Fixture fx;
	Check( fx.Build( EnvOnlyScene(), "iorstack" ), "ior-stack fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	const LightSampler* pLS = fx.pCaster->GetLightSampler();
	Check( pGlobal != 0 && pLS != 0, "ior-stack fixture has a radiance map and a LightSampler" );
	if( !pGlobal || !pLS ) return;
	const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
	Check( pES != 0 && pES->IsValid(), "ior-stack fixture has a valid EnvironmentSampler" );
	if( !pES || !pES->IsValid() ) return;

	const RasterizerState rast{};
	const Ray probe( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	const Scalar Lenv = ColorMath::MaxValue( pGlobal->GetRadiance( probe, rast ) );
	if( Lenv <= 0 ) { Check( false, "ior-stack fixture env radiance is positive" ); return; }

	UniformColorPainter* whiteP = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( whiteP, __FILE__, __LINE__, "white" );
	StackAwareLambertianMaterial* stackMat = new StackAwareLambertianMaterial( *whiteP );
	GlobalLog()->PrintNew( stackMat, __FILE__, __LINE__, "stack-aware furnace material" );
	StubObject* enclosing = new StubObject();
	GlobalLog()->PrintNew( enclosing, __FILE__, __LINE__, "enclosing dielectric" );

	PathGuidingField* guide = BuildSkewedField(
		Point3( 0, 0, 0 ), Vector3Ops::Normalize( Vector3( 0.6, 0.0, 0.8 ) ) );
	Check( guide->IsTrained(), "(f) skewed guiding field is trained" );

	const unsigned int kN = 160000;

	// Control: the SAME material and the SAME entry stack with guiding
	// OFF.  The blend never runs, so nothing in this row's fix is
	// engaged -- it proves the tilted-lobe SPF itself is an unbiased
	// albedo-1 estimator and the closed form is reachable.
	{
		RowConfig cfg{ "(f) control", false, false, false, eGuidingOneSampleMIS, 0.0 };
		const Scalar m = RunBatch( fx, *stackMat, guide, cfg, kN, 8000, enclosing );
		CheckRel( m, Lenv, 0.015,
			"(f) CONTROL stack-aware material inside a dielectric, guiding OFF: furnace reads L_env" );
	}

	{
		RowConfig cfg{ "(f)", false, false, false, eGuidingOneSampleMIS, 0.5 };
		const Scalar m = RunBatch( fx, *stackMat, guide, cfg, kN, 9000, enclosing );
		CheckRel( m, Lenv, 0.015,
			"(f) stack-aware material inside a dielectric, guiding ON: furnace reads L_env" );
	}

	guide->release();
	enclosing->release();
	stackMat->release();
	whiteP->release();
}

//////////////////////////////////////////////////////////////////////
// Rows (h) and (i): the ZERO-AGGREGATE-PDF WEDGE (DL-74 P2-2).
//
// THE REGION.  `StackAwareSPF`'s inside-the-dielectric lobe samples
// uniformly over the hemisphere about a TILTED axis
// `a = normalize(N + (0,1,0))` (45 degrees off `N`), and its `Pdf`
// returns exactly 0 outside that hemisphere.  The BRDF, though, is the
// albedo-1 Lambertian over the WHOLE upper hemisphere.  So the lune
//
//     W = { w : dot(w,N) > 0  and  dot(w,a) <= 0 }
//
// is a set of directions that
//   * carry a NONZERO BSDF value and pass every horizon gate, so NEE
//     samples them and takes their contribution, and
//   * have `p_aggregate(w) == 0`, so the material's OWN sampler never
//     proposes them -- but the GUIDE does, and the guided mixture
//     `alpha*guide + (1-alpha)*p_agg` therefore has positive density
//     there and the continuation really is traced into W.
//
// WHAT WAS BROKEN.  Both MIS sides treated `p_aggregate == 0` as "the
// BSDF-sampling technique never generates this direction, so NEE takes
// the whole sample": `PTGuidingMisPdf::Eval` returned 0 unchanged, and
// the four `LightSampler` NEE arms ran the blend INSIDE their
// `pdf > 0` gate so the hook was not even consulted.  For every
// direction in W that gave `w_nee = 1` AND `w_bsdf = 1` -- the two
// halves of one partition both claiming the whole sample -- so W's
// energy was counted TWICE.
//
// WHY TWO ROWS.  The two MIS pairs are independent code: row (h) is
// env-NEE against the env ESCAPE weight, row (i) is area-NEE against
// the EMITTER-HIT weight, and each has its own `p_bsdf > 0` gate.
// (The NM twins of both arms take the identical edit; no NM driver
// exists in this harness.)
//
// THE CLOSED FORMS, and why W does not disturb them.  MIS is unbiased
// for ANY weights that partition to one, over any sampling densities
// that cover their own integrand -- which both do here (the env
// sampler covers the sphere; the mixture covers W through its guide
// term).  So row (h) is still the albedo-1 white furnace `L_env`, and
// row (i) is still the spherical-emitter form factor below.
//
// THE PREDICTED PRE-FIX ERROR is the wedge integral counted a second
// time.  Both estimators are unbiased for their own weighted integral,
// so the pre-fix mean is
//
//     INTEGRAL_upper f L  +  INTEGRAL_W f L
//
// and the excess is W's COSINE-WEIGHTED share of the upper hemisphere.
// For a hemisphere clipped by a plane tilted by `phi` that share is
// `(1 - cos(phi)) / 2` (the complement of Malley's-method disk
// projection, the same closed form `TranslucentSPF::ExitValidFraction`
// uses), so at `phi = 45 deg`:
//
//     row (h):  (1 - cos(45 deg)) / 2 = 14.6 %  of L_env
//     row (i):  100 %  -- the emitter lies ENTIRELY inside W, so the
//               whole of its contribution is the doubled part
//
// MEASURED on the pre-fix library (commit 0860f78f): row (i) reads
// +100.354 %, dead on its closed form.  Row (h) reads +10.57 % at this
// row's 160k samples and +12.23 % at 640k -- an under-converged
// estimate of the ~13.7 % the guided branch's `combinedPdf > NEARZERO`
// gate leaves reachable (14.6 % is the untruncated closed form; review
// round 5 measured 0.1369 by quadrature), because the guide is a narrow cos^64 lobe
// sitting INSIDE a 45-degree-wide lune, so the pre-fix escape side's
// own estimate of `INTEGRAL_W f L` is heavy-tailed.  The assertions
// below are against the CLOSED FORMS, never against those figures.
//
//////////////////////////////////////////////////////////////////////

//! A direction inside the wedge: above the surface horizon
//! (`dot(.,+Z) > 0`) and below the tilted sampling hemisphere
//! (`dot(., a) < 0`).  30 degrees of elevation puts it 15 degrees clear
//! of the wedge's own boundary, which is what lets row (i)'s emitter fit
//! ENTIRELY inside W.
static Vector3 WedgeDirection()
{
	return Vector3Ops::Normalize( Vector3( 0.0, -0.8660254, 0.5 ) );
}

//! Deterministic premise for both rows: the wedge really is the region
//! the comment claims -- nonzero BRDF support, zero aggregate pdf, and
//! positive guide density.  No sampling, no render.
static void WedgePremise( const IMaterial& stackMat, PathGuidingField& guide,
	const IObject* enclosing )
{
	const RasterizerState rast{};
	RayIntersectionGeometric ri( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), rast );
	ri.bHit = true;
	ri.range = 1;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( ri.vNormal );

	IORStack inside( 1.0 );
	inside.SetCurrentObject( enclosing );
	inside.push( kInsideIOR );

	const Vector3 w = WedgeDirection();
	Check( Vector3Ops::Dot( w, ri.vNormal ) > 0,
		"wedge premise: the probe direction is ABOVE the surface horizon" );

	const ISPF* pSPF = stackMat.GetSPF();
	Check( pSPF != 0, "wedge premise: the stack-aware material has an SPF" );
	if( !pSPF ) return;

	const Scalar pWedge = pSPF->Pdf( ri, w, inside );
	// Control: the tilted axis itself, which the same lobe DOES sample.
	const Vector3 axis = Vector3Ops::Normalize(
		Vector3( 0, 0, 1 ) + Vector3( 0, kStackAwareTilt, 0 ) );
	const Scalar pAxis = pSPF->Pdf( ri, axis, inside );
	std::cout << "    wedge premise: p_aggregate(wedge) = " << pWedge
		<< " , p_aggregate(tilted axis) = " << pAxis << std::endl;
	Check( pWedge == 0, "wedge premise: the material's aggregate pdf is EXACTLY zero in the wedge" );
	Check( pAxis > 0, "wedge premise: the same lobe has positive pdf on its own axis" );

	GuidingDistributionHandle h;
	const bool ok = guide.InitDistribution( h, Point3( 0, 0, 0 ), 0.5 );
	Check( ok, "wedge premise: the guiding distribution initialises at the shading point" );
	if( ok ) {
		const Scalar g = guide.Pdf( h, w );
		std::cout << "    wedge premise: guide pdf in the wedge = " << g << std::endl;
		Check( g > 0,
			"wedge premise: the GUIDE reaches the wedge, so the mixture density there is positive" );
	}
}

// Row (h): env-NEE against the env escape, guide aimed INTO the wedge.
static void RunZeroAggregateEnvRow()
{
	std::cout << "DL-74 P2-2: a zero AGGREGATE pdf is not a zero MIXTURE pdf (env arm)"
		<< std::endl;

	Fixture fx;
	Check( fx.Build( EnvOnlyScene(), "wedgeenv" ), "wedge-env fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	const LightSampler* pLS = fx.pCaster->GetLightSampler();
	Check( pGlobal != 0 && pLS != 0, "wedge-env fixture has a radiance map and a LightSampler" );
	if( !pGlobal || !pLS ) return;
	const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
	Check( pES != 0 && pES->IsValid(), "wedge-env fixture has a valid EnvironmentSampler" );
	if( !pES || !pES->IsValid() ) return;

	const RasterizerState rast{};
	const Ray probe( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	const Scalar Lenv = ColorMath::MaxValue( pGlobal->GetRadiance( probe, rast ) );
	if( Lenv <= 0 ) { Check( false, "wedge-env fixture env radiance is positive" ); return; }

	UniformColorPainter* whiteP = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( whiteP, __FILE__, __LINE__, "white" );
	StackAwareLambertianMaterial* stackMat = new StackAwareLambertianMaterial( *whiteP );
	GlobalLog()->PrintNew( stackMat, __FILE__, __LINE__, "stack-aware furnace material" );
	StubObject* enclosing = new StubObject();
	GlobalLog()->PrintNew( enclosing, __FILE__, __LINE__, "enclosing dielectric" );

	PathGuidingField* guide = BuildSkewedField( Point3( 0, 0, 0 ), WedgeDirection() );
	Check( guide->IsTrained(), "(h) wedge-aimed guiding field is trained" );

	WedgePremise( *stackMat, *guide, enclosing );

	const unsigned int kN = 160000;

	{
		RowConfig cfg{ "(h) control", false, false, false, eGuidingOneSampleMIS, 0.0 };
		const Scalar m = RunBatch( fx, *stackMat, guide, cfg, kN, 12000, enclosing );
		CheckRel( m, Lenv, 0.015,
			"(h) CONTROL guiding OFF: the wedge is unreachable by the BSDF technique and the furnace reads L_env" );
	}

	{
		RowConfig cfg{ "(h)", false, false, false, eGuidingOneSampleMIS, 0.5 };
		const Scalar m = RunBatch( fx, *stackMat, guide, cfg, kN, 13000, enclosing );
		std::cout << "    (h) guided " << m << " vs L_env " << Lenv
			<< "  (pre-fix: the doubled wedge integral, 14.6 % of L_env)" << std::endl;
		CheckRel( m, Lenv, 0.015,
			"(h) guide aimed into the zero-aggregate wedge: furnace still reads L_env" );
	}

	guide->release();
	enclosing->release();
	stackMat->release();
	whiteP->release();
}

// Row (i): area-NEE against the emitter hit, with the emitter placed
// ENTIRELY inside the wedge.
static const Scalar kWedgeSphereRadius = 1.0;
static const Scalar kWedgeSphereDist   = 5.0;

static std::string WedgeAreaLightScene()
{
	const Vector3 w = WedgeDirection();
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n"
		"\n"
		"lambertian_luminaire_material\n{\n\tname emitter\n\texitance white\n"
		"\tscale 4.0\n\tmaterial none\n}\n"
		"\n"
		"sphere_geometry\n{\n\tname lightball\n\tradius " << kWedgeSphereRadius << "\n}\n"
		"\n"
		"standard_object\n{\n\tname light_object\n\tgeometry lightball\n"
		"\tmaterial emitter\n\tposition "
			<< w.x * kWedgeSphereDist << " "
			<< w.y * kWedgeSphereDist << " "
			<< w.z * kWedgeSphereDist << "\n}\n"
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

static void RunZeroAggregateAreaRow()
{
	std::cout << "DL-74 P2-2: a zero AGGREGATE pdf is not a zero MIXTURE pdf (area arm)"
		<< std::endl;

	Fixture fx;
	Check( fx.Build( WedgeAreaLightScene(), "wedgearea" ), "wedge-area fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;
	Check( fx.pScene->GetGlobalRadianceMap() == 0,
		"wedge-area fixture has NO env map (so only the area-light MIS pair is exercised)" );

	const RasterizerState rast{};
	const Vector3 w = WedgeDirection();
	Scalar Le = 0;
	{
		RayIntersection probe( Ray( Point3( 0, 0, 0 ), w ), rast );
		fx.pScene->GetObjects()->IntersectRay( probe, true, true, false );
		Check( probe.geometric.bHit && probe.pMaterial != 0,
			"wedge-area fixture: the emissive sphere is hit along the wedge direction" );
		if( !probe.geometric.bHit || !probe.pMaterial ) return;
		IEmitter* pEm = probe.pMaterial->GetEmitter();
		Check( pEm != 0, "wedge-area fixture: the sphere carries an emitter" );
		if( !pEm ) return;
		Le = ColorMath::MaxValue( pEm->emittedRadiance(
			probe.geometric, -probe.geometric.ray.Dir(), probe.geometric.vNormal ) );
		Check( Le > 0, "wedge-area fixture: emitted radiance probe is positive" );
		if( Le <= 0 ) return;
	}

	// Closed form for a uniform-radiance sphere of angular radius
	// `alpha = asin(R/d)` whose centre sits at polar angle `beta` from
	// the shading normal and which lies ENTIRELY above the horizon
	// (`alpha + beta < 90 deg`): the projected solid angle is
	// `PI * sin^2(alpha) * cos(beta)` (the differential-element-to-sphere
	// form factor `(R/d)^2 cos(beta)`), so for an albedo-1 Lambertian
	//
	//     L_out = L_e * (R/d)^2 * cos(beta)
	//
	// Here alpha = asin(1/5) = 11.54 deg and beta = 60 deg (the wedge
	// direction's elevation is 30 deg), so alpha + beta = 71.5 deg: the
	// sphere clears the horizon, and it also clears the wedge's own
	// boundary, which is 15 deg away.
	const Scalar cosBeta = Vector3Ops::Dot( w, Vector3( 0, 0, 1 ) );
	const Scalar expected = Le * ( kWedgeSphereRadius * kWedgeSphereRadius ) /
		( kWedgeSphereDist * kWedgeSphereDist ) * cosBeta;

	UniformColorPainter* whiteP = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( whiteP, __FILE__, __LINE__, "white" );
	StackAwareLambertianMaterial* stackMat = new StackAwareLambertianMaterial( *whiteP );
	GlobalLog()->PrintNew( stackMat, __FILE__, __LINE__, "stack-aware furnace material" );
	StubObject* enclosing = new StubObject();
	GlobalLog()->PrintNew( enclosing, __FILE__, __LINE__, "enclosing dielectric" );

	PathGuidingField* guide = BuildSkewedField( Point3( 0, 0, 0 ), w );
	Check( guide->IsTrained(), "(i) wedge-aimed guiding field is trained" );

	WedgePremise( *stackMat, *guide, enclosing );

	const unsigned int kN = 200000;

	// Control: guiding OFF.  The tilted lobe cannot sample the wedge at
	// all, so the emitter is reachable ONLY by NEE -- which is exactly
	// the "no BSDF-side partner" case, and weight 1 is then correct.
	{
		RowConfig cfg{ "(i) control", false, false, false, eGuidingOneSampleMIS, 0.0 };
		const Scalar m = RunBatch( fx, *stackMat, guide, cfg, kN, 14000, enclosing );
		CheckRel( m, expected, 0.02,
			"(i) CONTROL guiding OFF: NEE alone reads L_e*(R/d)^2*cos(beta)" );
	}

	{
		RowConfig cfg{ "(i)", false, false, false, eGuidingOneSampleMIS, 0.9 };
		const Scalar m = RunBatch( fx, *stackMat, guide, cfg, kN, 15000, enclosing );
		std::cout << "    (i) guided " << m << " vs expected " << expected
			<< "  (pre-fix: the emitter is entirely inside the wedge, so +100 %)" << std::endl;
		CheckRel( m, expected, 0.02,
			"(i) emitter entirely inside the zero-aggregate wedge: area-NEE and the emitter hit still partition to 1" );
	}

	guide->release();
	enclosing->release();
	stackMat->release();
	whiteP->release();
}

//////////////////////////////////////////////////////////////////////
// Row (g): the SHADER-OP BOUNDARY.
//
// `RayCaster`'s volume phase-scatter continuation is the one producer
// that sets the two `RAY_STATE` pdf fields to DIFFERENT values:
// `bsdfPdf = effectivePdf` (the guided mixture the direction was drawn
// from -- the training denominator) and `bsdfMisPdf = phasePdf` (the
// raw density env-NEE and area-NEE at a volume vertex weight against,
// DL-73's ruling).  When that continuation hits an EMISSIVE SURFACE it
// leaves `RayCaster` through the shader dispatch --
// `PathTracingShaderOp::PerformOperation` -> `IntegrateFromHit` -> the
// PART 1 emitter-hit MIS weight.  If the boundary forwards `bsdfPdf`
// instead of `MisPartnerPdf()`, that weight is built from the guided
// combined pdf while the volume vertex's own area-NEE arm weighted
// against the raw phase pdf, and the pair stops summing to one.
//
// THE INVARIANT.  Path guiding changes only the SAMPLING distribution;
// a correct estimator's EXPECTATION is identical with it on and off.
// So the guiding-off reading of this fixture is the reference, and it
// needs no closed form -- which matters here, because a conservative
// medium's multiple scattering has no closed form at this optical
// depth.  (The guiding-off arm has no partition asymmetry of its own
// to worry about: with guiding off the producer sets both fields to
// `phasePdf`, exactly what the NEE arm uses.)
//////////////////////////////////////////////////////////////////////
static std::string VolumeAreaLightScene()
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_density\n\tcolor 1 1 1\n}\n"
		"\n"
		"lambertian_luminaire_material\n{\n\tname emitter\n\texitance white\n"
		"\tscale 4.0\n\tmaterial none\n}\n"
		"\n"
		"sphere_geometry\n{\n\tname lightball\n\tradius " << kSphereRadius << "\n}\n"
		"\n"
		"standard_object\n{\n\tname light_object\n\tgeometry lightball\n"
		"\tmaterial emitter\n\tposition 0 0 " << kSphereDist << "\n}\n"
		"\n"
		"painter_heterogeneous_medium\n{\n"
		"\tname fog\n"
		"\tabsorption 0.02 0.02 0.02\n"
		"\tscattering 0.05 0.05 0.05\n"
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
		// DefaultPathTracing (not DefaultDirectLighting): the emitter hit
		// this row measures is weighted inside PathTracingShaderOp ->
		// IntegrateFromHit, which is the boundary under test.
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n"
		"\n"
		"pixelpel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n"
		"\toidn_denoise FALSE\n\tmax_recursion 3\n}\n"
		"\n"
		"film\n{\n\twidth 4\n\theight 4\n}\n"
		"\n"
		"pinhole_camera\n{\n\tlocation 0 0 -3\n\tlookat 0 0 1\n\tup 0 1 0\n\tfov 40.0\n}\n"
		"\n";
	return ss.str();
}

//! A trained VOLUME guiding field whose incident-radiance estimate is
//! concentrated about `axis` at every scatter position along the probe
//! ray, so `SampleVolume`/`PdfVolume` differ from the isotropic phase
//! pdf by orders of magnitude.  Surface samples are deposited too so
//! the field trains both distributions from one storage pass (the
//! surface half is never queried here -- the only surface in the scene
//! is a pure luminaire).
static PathGuidingField* BuildSkewedVolumeField( const Vector3& axis )
{
	PathGuidingConfig config;
	config.enabled = true;
	PathGuidingField* guide = new PathGuidingField( config,
		Point3( -22, -22, -22 ), Point3( 22, 22, 22 ) );
	GlobalLog()->PrintNew( guide, __FILE__, __LINE__, "skewed volume guiding field" );

	guide->BeginTrainingIteration();
	const unsigned int kPositions = 48;
	const unsigned int kDirs = 512;
	for( unsigned int p = 0; p < kPositions; ++p ) {
		const Point3 at( 0, 0, -20.0 * ( p + 0.5 ) / kPositions );
		for( unsigned int i = 0; i < kDirs; ++i ) {
			const Scalar z = -1 + 2 * ( i + 0.5 ) / kDirs;
			const Scalar phi = i * 2.399963229728653;
			const Scalar r = std::sqrt( r_max( Scalar(0), Scalar(1) - z * z ) );
			const Vector3 dir( r * std::cos( phi ), r * std::sin( phi ), z );
			const Scalar c = Vector3Ops::Dot( dir, axis );
			const Scalar lum = c > 0 ? std::pow( (double)c, 16.0 ) : 0.0;
			if( lum <= 1e-9 ) {
				guide->AddZeroValueVolumeSample( at, dir );
				guide->AddZeroValueSample( at, dir );
			} else {
				guide->AddVolumeSample( at, dir, 1.0, 1 / ( 4 * PI ), lum, false );
				guide->AddSample( at, dir, 1.0, 1 / ( 4 * PI ), lum, false );
			}
		}
	}
	guide->EndTrainingIteration();
	return guide;
}

static Scalar RunVolumeBatch(
	const Fixture& fx,
	PathGuidingField* guide,
	Scalar alpha,
	unsigned int nSamples,
	unsigned int seedBase )
{
	const RasterizerState rast{};
	Scalar sum = 0;

	for( unsigned int s = 0; s < nSamples; ++s ) {
		RandomNumberGenerator rng( seedBase + s );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		if( alpha > 0 ) {
			rc.pGuidingField = guide;
			rc.guidingAlpha = alpha;
			rc.guidingLearnedAlpha = false;
			rc.maxGuidingDepth = 4;
			rc.guidingSamplingType = eGuidingOneSampleMIS;
		}

		IRayCaster::RAY_STATE rs;
		rs.depth = 0;
		rs.importance = 1.0;
		rs.considerEmission = true;
		rs.type = IRayCaster::RAY_STATE::eRayDiffuse;

		// Pointed AWAY from the emitter: the unscattered ray hits nothing
		// and the fixture has no radiance map, so every unit of the
		// measured radiance came through a volume scatter event.
		const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, -1 ) );

		RISEPel c( 0, 0, 0 );
		Scalar dist = 0;
		fx.pCaster->CastRay( rc, rast, ray, c, rs, &dist, 0 );
		sum += ColorMath::MaxValue( c );
	}

	return sum / nSamples;
}

static void RunVolumeEmitterRow()
{
	std::cout << "DL-74 P1: the volume vertex's MIS partner must survive the shader-op boundary"
		<< std::endl;

	Fixture fx;
	Check( fx.Build( VolumeAreaLightScene(), "volemit" ), "volume+emitter fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;
	Check( fx.pScene->GetGlobalRadianceMap() == 0,
		"volume+emitter fixture has NO env map (so only the area-light MIS pair is exercised)" );
	Check( fx.pScene->GetGlobalMedium() != 0, "volume+emitter fixture has a global medium" );

	PathGuidingField* guide = BuildSkewedVolumeField( Vector3( 0, 0, 1 ) );
	Check( guide->IsTrained(), "(g) skewed volume guiding field is trained" );
	Check( guide->GetLastAddedVolumeSampleCount() > 0,
		"(g) the field really received VOLUME training samples" );

	const unsigned int kN = 120000;

	const Scalar reference = RunVolumeBatch( fx, guide, 0.0, kN, 11000 );
	Check( reference > 0, "(g) the unguided volumetric reading is positive" );
	const Scalar guided = RunVolumeBatch( fx, guide, 0.9, kN, 11000 );

	std::cout << "    (g) unguided " << reference << " , guided " << guided << std::endl;
	CheckRel( guided, reference, 0.03,
		"(g) volume vertex + trained volume guiding + area emitter: guiding does not change the expectation" );

	guide->release();
}

static void Run()
{
	RunEnvRows();
	RunAreaLightRow();
	RealMaterialStackPremise();
	RunIorStackRow();
	RunZeroAggregateEnvRow();
	RunZeroAggregateAreaRow();
	RunVolumeEmitterRow();
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
