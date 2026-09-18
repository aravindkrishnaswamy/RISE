//////////////////////////////////////////////////////////////////////
//
//  LegacyChainMISPartnerTest.cpp - Red-proof for DL-171 (docs/
//    DL170_DL171_HWSS_AND_LEGACY_MIS_PARTNERS.md): the legacy
//    shader-op chain (`DistributionTracingShaderOp`, `ReflectionShaderOp`,
//    `RefractionShaderOp`, `FinalGatherShaderOp`) never populated the
//    continuation's `RAY_STATE::bsdfPdf`/`bsdfMisPdf`, so
//    `EmissionShaderOp` took a BSDF-sampled emitter hit at FULL weight
//    (or, when the continuation deliberately suppresses emission,
//    zero weight) while `DirectLightingShaderOp` -> `LightSampler`'s
//    NEE arm still weighted its own sample against the material's
//    real aggregate `Pdf()`/`PdfNM()` -- the pair did not partition to
//    one in either direction.
//
//  THE INVARIANT.  Same closed forms as `PTGuidingMISPartitionTest`'s
//  DL-74/DL-103 rows: an albedo-1 Lambertian surface re-radiates
//  exactly the incident radiance.  Under a CONSTANT ENVIRONMENT that
//  is `L_env`; under a uniform-radiance sphere of radius R at distance
//  d along the normal (R < d) that is `L_e * R^2 / d^2`.
//
//  WHY THIS ROW NEEDS TWO DIFFERENT LIGHT SHAPES, NOT ONE.
//  `RayCaster::CastRay`'s own env-escape branch
//  (`RayCasterEnvEscapeMISWeight`) applies its weight UNCONDITIONALLY
//  -- it never reads `RAY_STATE::considerEmission` -- so a background
//  environment can only ever show this row's OVER sign (before the
//  fix, an absent partner reads as weight 1, i.e. no MIS discount, on
//  top of NEE's own env-arm sample).  `EmissionShaderOp`, by contrast,
//  DOES gate on `considerEmission`, which is exactly the knob
//  `DistributionTracingShaderOp`'s own `bForceCheckEmitters`/
//  material-has-a-BSDF logic drives -- so demonstrating BOTH signs on
//  ONE construction needs a REAL emitter OBJECT the traced ray can
//  hit, not a background map.
//
//  THE SHADER WIRING.  `Job::AddStandardShader` auto-prepends a
//  `DefaultEmission` (`EmissionShaderOp`) op ahead of any requested op
//  that doesn't `HandlesEmission()` itself (`Job.cpp` ~8964-8990), so
//  building the emitter object's shader as `standard_shader { shaderop
//  DefaultDirectLighting }` gives it the REAL production
//  `[EmissionShaderOp, DirectLightingShaderOp]` chain.  The SHADING
//  POINT under test is a synthetic `RayIntersection` (same idiom as
//  `PTGuidingMISPartitionTest`'s `IntegrateOneSample`) at which THIS
//  file instantiates `DistributionTracingShaderOp`/`DirectLightingShaderOp`/
//  `ReflectionShaderOp` directly and sums their `PerformOperation`
//  outputs -- exactly what `StandardShader::Shade` does internally.  A
//  scene-language chunk DOES exist for a bare distribution-tracing op
//  (`distributiontracing_shaderop`, e.g. `scenes/Tests/Shaders/
//  blurry_floor.RISEscene`'s `shaderop dt` + `shaderop
//  DefaultDirectLighting` pairing) and `DefaultReflection`/
//  `DefaultRefraction` presets exist too (`Job::InitializeContainers`)
//  -- this file uses direct C++ construction instead only so the
//  `bForceCheckEmitters` comparison and the delta-lobe control can each
//  build their OWN minimal, hand-picked op list without a full scene
//  round-trip; it is a methodology choice, not a workaround for a
//  missing chunk.
//
//  A KNOWN, NARROW RESIDUAL THIS FIX DOES NOT COVER (see DL-209 in the
//  closure doc): the fix above assumes a matching NEE strategy exists
//  wherever it stamps a non-delta partner, but `DistributionTracingShaderOp`
//  has no way to know whether its OWN shader's op list actually
//  contains a `DirectLightingShaderOp` -- each op is independent, with
//  no back-reference to its shader.  Every shipped scene that uses
//  `distributiontracing_shaderop` pairs it with `DefaultDirectLighting`
//  in the SAME `standard_shader` (`blurry_floor`, `blurry_glass`,
//  `dielectric_dispersion`, `pillow`, `showroom`, `spotlight_drama`,
//  `different_rmaps`) EXCEPT ONE: `scenes/Tests/Shaders/
//  dt_with_irrcache.RISEscene`'s `dist` shader is `[dist,
//  DefaultEmission]` with NO NEE at all, relying on
//  `force_check_emitters TRUE` for a pure BSDF-sampling-only indirect
//  gather onto its `refsphere` receiver -- exactly the configuration
//  where this fix's MIS discount is wrong (there is no competing NEE
//  term for it to partition against).  Measured (isolated A/B, this
//  scene at 80x120/32spp, PNG sRGB, illuminated-ROI pixels >20/255):
//  pre-fix ROI mean 199.746, post-fix 194.808, ratio 0.9753 (~2.5%
//  darker) -- a real, small, one-directional UNDER-shift, not noise.
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
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Shaders/DistributionTracingShaderOp.h"
#include "../src/Library/Shaders/DirectLightingShaderOp.h"
#include "../src/Library/Shaders/ReflectionShaderOp.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/PerfectReflectorMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Color/ColorUtils.h"
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
		std::cout << "    ok   " << what << std::endl;
		passCount++;
	} else {
		std::cout << "  FAIL: " << what << std::endl;
		failCount++;
	}
}

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

static void CheckRel( double measured, double expected, double relTol, const char* what )
{
	const double denom = std::fabs( expected ) > 1e-12 ? std::fabs( expected ) : 1.0;
	const double relErr = std::fabs( measured - expected ) / denom;
	std::cout << "    " << what << "  measured=" << measured << " expected=" << expected
		<< " relErr=" << ( relErr * 100.0 ) << "%" << ( relErr > relTol ? " tol=" : "" );
	if( relErr > relTol ) std::cout << ( relTol * 100.0 ) << "%";
	std::cout << std::endl;
	Check( relErr <= relTol, what );
}

//////////////////////////////////////////////////////////////////////
// Fixture -- same idiom as PTGuidingMISPartitionTest.cpp: build a real
// Job/Scene/RayCaster from scene text, exposing the live IRayCaster*/
// IScene* production code uses.
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
		std::snprintf( path, sizeof(path), "/tmp/legacy_mis_%s_%d.RISEscene",
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

		std::srand( 4171u );
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

static const Scalar kSphereRadius = 2.0;
static const Scalar kSphereDist   = 5.0;
static const Scalar kSphereScale  = 4.0;

//! Same construction as `PTGuidingMISPartitionTest`'s own `AreaLightScene`
//! -- a Lambertian-luminaire sphere overhead, reached both by NEE
//! (`DirectLightingShaderOp`) and by a BSDF-sampled continuation ray
//! (`DistributionTracingShaderOp`).  Its OWN shader
//! (`standard_shader { shaderop DefaultDirectLighting }`) gets
//! `DefaultEmission` auto-prepended by `Job::AddStandardShader`, so a
//! ray landing on it is shaded by the REAL production
//! `[EmissionShaderOp, DirectLightingShaderOp]` chain.
static std::string AreaLightScene()
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 7\n"
		"\n"
		"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n"
		"\n"
		"lambertian_luminaire_material\n{\n\tname emitter\n\texitance white\n"
		"\tscale " << kSphereScale << "\n\tmaterial none\n}\n"
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

//! A synthetic front-facing hit at the origin, normal along +Z (the
//! same idiom as PTGuidingMISPartitionTest's IntegrateOneSample) --
//! shared by every row below.
static void BuildHit( RayIntersection& hit, const IMaterial& material, const IObject& obj )
{
	hit.geometric.bHit = true;
	hit.geometric.range = 1;
	hit.geometric.ptIntersection = Point3( 0, 0, 0 );
	hit.geometric.vNormal = Vector3( 0, 0, 1 );
	hit.geometric.vGeomNormal = Vector3( 0, 0, 1 );
	hit.geometric.onb.CreateFromW( hit.geometric.vNormal );
	hit.pObject = &obj;
	hit.pMaterial = &material;
}

//////////////////////////////////////////////////////////////////////
// One legacy-chain furnace sample.  `pNee`/`pDist` are each optional
// (0 = "this op is not in the chain"), and `chainHasNEEOp`/
// `chainHasBsdfContinuationOp` are the CHAIN-COMPOSITION facts a real
// StandardShader would resolve from its own op list and stamp onto
// `rs` before dispatching (see RAY_STATE::chainHasNEEOp's doc) --
// passed explicitly here so each row can construct whatever chain
// shape it needs without a full scene-language shader chunk.
//////////////////////////////////////////////////////////////////////
static Scalar RunLegacyChainSample(
	const Fixture& fx,
	const IMaterial& material,
	DirectLightingShaderOp* pNee,
	DistributionTracingShaderOp* pDist,
	bool chainHasNEEOp,
	bool chainHasBsdfContinuationOp,
	RuntimeContext& rc,
	StubObject& shadingObject
	)
{
	const RasterizerState rast{};
	RayIntersection hit( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), rast );
	BuildHit( hit, material, shadingObject );

	IORStack stack( 1.0 );
	IRayCaster::RAY_STATE rs;
	rs.depth = 0;
	rs.importance = 1.0;
	rs.considerEmission = true;
	rs.type = IRayCaster::RAY_STATE::eRayDiffuse;
	rs.chainHasNEEOp = chainHasNEEOp;
	rs.chainHasBsdfContinuationOp = chainHasBsdfContinuationOp;

	RISEPel total( 0, 0, 0 );
	RISEPel c( 0, 0, 0 );
	if( pNee ) {
		pNee->PerformOperation( rc, hit, *fx.pCaster, rs, c, stack, 0 );
		total = total + c;
	}
	if( pDist ) {
		pDist->PerformOperation( rc, hit, *fx.pCaster, rs, c, stack, 0 );
		total = total + c;
	}
	return ColorMath::MaxValue( total );
}

static Scalar RunLegacyChainBatch(
	const Fixture& fx,
	const IMaterial& material,
	bool forceCheckEmitters,
	bool includeNee,
	bool includeDist,
	bool chainHasNEEOp,
	bool chainHasBsdfContinuationOp,
	unsigned int nSamples,
	unsigned int seedBase
	)
{
	DirectLightingShaderOp* nee = includeNee ? new DirectLightingShaderOp( 0 ) : 0;
	if( nee ) GlobalLog()->PrintNew( nee, __FILE__, __LINE__, "legacy chain NEE op" );
	// numSamples=1, irradiancecaching=false, forcecheckemitters=<row>,
	// reflections=refractions=diffuse=translucents=true (trace
	// whatever the material's own SPF emits).
	DistributionTracingShaderOp* dist = includeDist ? new DistributionTracingShaderOp(
		1, false, forceCheckEmitters, true, true, true, true ) : 0;
	if( dist ) GlobalLog()->PrintNew( dist, __FILE__, __LINE__, "legacy chain distribution-tracing op" );

	StubObject* shadingObject = new StubObject();
	GlobalLog()->PrintNew( shadingObject, __FILE__, __LINE__, "legacy chain shading object" );

	Scalar sum = 0;
	for( unsigned int i = 0; i < nSamples; ++i ) {
		RandomNumberGenerator rng( seedBase + i );
		IndependentSampler sampler( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		(void)sampler;
		sum += RunLegacyChainSample( fx, material, nee, dist,
			chainHasNEEOp, chainHasBsdfContinuationOp, rc, *shadingObject );
	}

	shadingObject->release();
	if( dist ) dist->release();
	if( nee ) nee->release();
	return sum / nSamples;
}

static void RunEnvRow()
{
	std::cout << "DL-171 legacy chain: env-background furnace, the CANONICAL paired "
		"chain [DirectLightingShaderOp, DistributionTracingShaderOp] (the OVER sign -- "
		"RayCaster's env-escape weight never gates on considerEmission)" << std::endl;

	Fixture fx;
	Check( fx.Build( EnvOnlyScene(), "env" ), "legacy chain env fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	Check( pGlobal != 0, "legacy chain env fixture has a radiance map" );
	if( !pGlobal ) return;
	const RasterizerState rast{};
	const Scalar Lenv = ColorMath::MaxValue(
		pGlobal->GetRadiance( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), rast ) );
	Check( Lenv > 0, "legacy chain env radiance probe is positive" );
	if( Lenv <= 0 ) return;

	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( white, __FILE__, __LINE__, "legacy chain white painter" );
	LambertianMaterial* mat = new LambertianMaterial( *white );
	GlobalLog()->PrintNew( mat, __FILE__, __LINE__, "legacy chain Lambertian" );

	const unsigned int kN = 100000;
	// Canonical chain: BOTH ops present, both chain flags true (matches
	// what a real StandardShader with [DirectLighting, dt] would stamp).
	const Scalar m = RunLegacyChainBatch( fx, *mat, /*forceCheckEmitters*/ false,
		/*includeNee*/ true, /*includeDist*/ true,
		/*chainHasNEEOp*/ true, /*chainHasBsdfContinuationOp*/ true, kN, 51000 );
	std::cout << "    env furnace (legacy chain) " << m << " , expected " << Lenv << std::endl;
	CheckRel( m, Lenv, 0.02,
		"DL-171: legacy chain (DirectLightingShaderOp + DistributionTracingShaderOp) "
		"env furnace reads L_env" );

	mat->release();
	white->release();
}

static void RunAreaEmitterRows()
{
	std::cout << "DL-171 legacy chain: area-emitter furnace, the CANONICAL paired chain -- "
		"default vs bForceCheckEmitters=TRUE, both must now partition to 1" << std::endl;

	Fixture fx;
	Check( fx.Build( AreaLightScene(), "area" ), "legacy chain area fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const Scalar Le = kSphereScale / PI;
	const Scalar expected = Le * ( kSphereRadius * kSphereRadius ) / ( kSphereDist * kSphereDist );
	Check( expected > 0, "legacy chain area closed form is positive" );

	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( white, __FILE__, __LINE__, "legacy chain white painter (area)" );
	LambertianMaterial* mat = new LambertianMaterial( *white );
	GlobalLog()->PrintNew( mat, __FILE__, __LINE__, "legacy chain Lambertian (area)" );

	const unsigned int kN = 100000;

	// Default (canonical [DirectLighting, dt] chain, force_check_emitters
	// FALSE): the pre-fix "suppress emission whenever the source material
	// has a BSDF" hack used to zero the BSDF-sampled emitter strategy
	// here entirely, leaving NEE's own MIS-weighted sample as the whole
	// total -- genuinely and knowably LESS than the closed form (the
	// UNDER sign this row's own ledger entry names).  The ruling retires
	// that hack: with the partner this continuation now stamps, the
	// escape strategy contributes its properly-weighted share too, and
	// the pair must partition to 1 -- a real closed-form gate, not a
	// bounded sanity check.
	{
		const Scalar m = RunLegacyChainBatch( fx, *mat, /*forceCheckEmitters*/ false,
			/*includeNee*/ true, /*includeDist*/ true,
			/*chainHasNEEOp*/ true, /*chainHasBsdfContinuationOp*/ true, kN, 52000 );
		std::cout << "    area furnace, default (canonical chain) " << m
			<< " , expected " << expected << std::endl;
		CheckRel( m, expected, 0.03,
			"DL-171: legacy chain area furnace, default (canonical [DirectLighting, dt] "
			"chain, considerEmission NOT suppressed): partition to 1" );
	}

	// bForceCheckEmitters=TRUE: now a NO-OP for the (removed) BSDF-has-a-
	// material suppression clause -- both this row and the default above
	// read the same considerEmission=true, so this is a regression pin
	// that the flag stays harmless (kept, per the ruling, as a
	// no-op-compatible flag rather than retired outright, since it still
	// matters for the orthogonal caustic-spectral-map suppression this
	// fixture doesn't exercise).
	{
		const Scalar m = RunLegacyChainBatch( fx, *mat, /*forceCheckEmitters*/ true,
			/*includeNee*/ true, /*includeDist*/ true,
			/*chainHasNEEOp*/ true, /*chainHasBsdfContinuationOp*/ true, kN, 53000 );
		std::cout << "    area furnace, bForceCheckEmitters=TRUE " << m
			<< " , expected " << expected << std::endl;
		CheckRel( m, expected, 0.03,
			"DL-171: legacy chain area furnace, bForceCheckEmitters=TRUE (now a no-op): "
			"partition to 1" );
	}

	mat->release();
	white->release();
}

//////////////////////////////////////////////////////////////////////
// DL-171 ruling point 2: a DirectLightingShaderOp-ALONE chain (no
// DistributionTracingShaderOp sibling at all) has no competing
// BSDF-sampled strategy, so NEE must take weight 1 (the closed form
// exactly, since it is the SOLE estimator).  `chainHasBsdfContinuationOp
// = true` reproduces the PRE-RULING assumption (every caller has a real
// partner) to quantify the UNDER magnitude that assumption cost here;
// `= false` is the post-ruling, chain-aware answer.
//////////////////////////////////////////////////////////////////////
static void RunDirectLightingAloneRow()
{
	std::cout << "DL-171/DL-209: DirectLightingShaderOp ALONE (no dt sibling) -- "
		"chainHasBsdfContinuationOp=false must read the closed form; =true quantifies "
		"the pre-ruling UNDER" << std::endl;

	Fixture fx;
	Check( fx.Build( AreaLightScene(), "dlalone" ), "legacy chain DirectLighting-alone fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const Scalar Le = kSphereScale / PI;
	const Scalar expected = Le * ( kSphereRadius * kSphereRadius ) / ( kSphereDist * kSphereDist );

	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( white, __FILE__, __LINE__, "legacy chain white painter (dlalone)" );
	LambertianMaterial* mat = new LambertianMaterial( *white );
	GlobalLog()->PrintNew( mat, __FILE__, __LINE__, "legacy chain Lambertian (dlalone)" );

	const unsigned int kN = 100000;

	// Pre-ruling assumption: a partner "exists" even though nothing in
	// this chain ever traces a matching BSDF-sampled continuation --
	// NEE's own PowerHeuristic weight discounts against a phantom
	// competitor, UNDER-counting the sole estimator for this light.
	{
		const Scalar m = RunLegacyChainBatch( fx, *mat, /*forceCheckEmitters*/ false,
			/*includeNee*/ true, /*includeDist*/ false,
			/*chainHasNEEOp*/ false, /*chainHasBsdfContinuationOp*/ true, kN, 54000 );
		std::cout << "    DirectLighting-alone, chainHasBsdfContinuationOp=TRUE (pre-ruling UNDER) "
			<< m << " , full closed form " << expected << std::endl;
		Check( m > 0 && m < expected * Scalar( 0.9 ),
			"DL-171/DL-209: DirectLighting-alone with a PHANTOM partner reads meaningfully "
			"BELOW the closed form (the pre-ruling UNDER)" );
	}

	// Chain-aware: no DistributionTracingShaderOp sibling, so NEE takes
	// weight 1 -- the closed form exactly (NEE is the sole estimator).
	{
		const Scalar m = RunLegacyChainBatch( fx, *mat, /*forceCheckEmitters*/ false,
			/*includeNee*/ true, /*includeDist*/ false,
			/*chainHasNEEOp*/ false, /*chainHasBsdfContinuationOp*/ false, kN, 55000 );
		std::cout << "    DirectLighting-alone, chainHasBsdfContinuationOp=FALSE " << m
			<< " , expected " << expected << std::endl;
		CheckRel( m, expected, 0.02,
			"DL-171: DirectLightingShaderOp alone (no BSDF-sampled sibling): NEE takes "
			"weight 1 and reads the closed form exactly" );
	}

	mat->release();
	white->release();
}

//////////////////////////////////////////////////////////////////////
// DL-171 ruling point 3 / DL-209: a DistributionTracingShaderOp in a
// chain with NO DirectLightingShaderOp sibling has no NEE to partition
// against -- its continuation's partner must be 0 (full, unweighted
// credit), closing DL-209's own shape
// (scenes/Tests/Shaders/dt_with_irrcache.RISEscene's `[dist,
// DefaultEmission]`).  `chainHasNEEOp = true` reproduces the DL-171-v1
// (pre-this-ruling) behaviour that DL-209 was filed against.
//////////////////////////////////////////////////////////////////////
static void RunDistributionTracingWithoutDirectLightingRow()
{
	std::cout << "DL-209: DistributionTracingShaderOp WITHOUT a DirectLighting sibling -- "
		"chainHasNEEOp=false must close to the closed form (was DL-209's own residual)" << std::endl;

	Fixture fx;
	Check( fx.Build( AreaLightScene(), "dtalone" ), "legacy chain dt-alone fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const Scalar Le = kSphereScale / PI;
	const Scalar expected = Le * ( kSphereRadius * kSphereRadius ) / ( kSphereDist * kSphereDist );

	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( white, __FILE__, __LINE__, "legacy chain white painter (dtalone)" );
	LambertianMaterial* mat = new LambertianMaterial( *white );
	GlobalLog()->PrintNew( mat, __FILE__, __LINE__, "legacy chain Lambertian (dtalone)" );

	const unsigned int kN = 100000;

	// DL-209's own (pre-fix) shape: `chainHasNEEOp=true` stamps a real
	// partner even though nothing in THIS chain does NEE, so the
	// escape/emitter-hit strategy is wrongly discounted -- UNDER.
	{
		const Scalar m = RunLegacyChainBatch( fx, *mat, /*forceCheckEmitters*/ false,
			/*includeNee*/ false, /*includeDist*/ true,
			/*chainHasNEEOp*/ true, /*chainHasBsdfContinuationOp*/ false, kN, 56000 );
		std::cout << "    dt-alone, chainHasNEEOp=TRUE (DL-209's own pre-fix UNDER) " << m
			<< " , full closed form " << expected << std::endl;
		Check( m > 0 && m < expected * Scalar( 0.9 ),
			"DL-209: dt-alone with a PHANTOM NEE partner reads meaningfully BELOW the "
			"closed form (the pre-fix UNDER)" );
	}

	// Chain-aware: no DirectLightingShaderOp sibling, so the escape
	// strategy takes weight 1 -- the closed form exactly.
	{
		const Scalar m = RunLegacyChainBatch( fx, *mat, /*forceCheckEmitters*/ false,
			/*includeNee*/ false, /*includeDist*/ true,
			/*chainHasNEEOp*/ false, /*chainHasBsdfContinuationOp*/ false, kN, 57000 );
		std::cout << "    dt-alone, chainHasNEEOp=FALSE " << m << " , expected " << expected << std::endl;
		CheckRel( m, expected, 0.03,
			"DL-209 CLOSED: DistributionTracingShaderOp alone (no NEE sibling) takes "
			"weight 1 and reads the closed form exactly" );
	}

	mat->release();
	white->release();
}

//////////////////////////////////////////////////////////////////////
// DL-171 ruling point 1's own control: a
// [DirectLightingShaderOp, ReflectionShaderOp] chain on a PURE MIRROR
// (no diffuse component -- `GetBSDF()` is null) is UNCHANGED whichever
// way `chainHasBsdfContinuationOp` reads, because NEE has nothing to
// weight in the first place (no BSDF means `DirectLightingShaderOp`
// returns 0 before it ever reaches the partner question) and
// `ReflectionShaderOp`'s own delta lobe is untouched by this row's
// mechanism -- confirms a delta `Reflection`/`Refraction` sibling is
// correctly NOT classified as a competing BSDF-sampled strategy either
// (dynamic_cast in the resolver only recognises
// DistributionTracingShaderOp).
//////////////////////////////////////////////////////////////////////
static void RunDirectLightingPlusMirrorRow()
{
	std::cout << "DL-171 control: [DirectLightingShaderOp, ReflectionShaderOp] on a "
		"pure mirror is UNCHANGED regardless of chainHasBsdfContinuationOp" << std::endl;

	Fixture fx;
	Check( fx.Build( EnvOnlyScene(), "dlmirror" ), "legacy chain DirectLighting+mirror fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	if( !pGlobal ) { Check( false, "legacy chain DirectLighting+mirror fixture has a radiance map" ); return; }
	const RasterizerState rast{};
	const Scalar Lenv = ColorMath::MaxValue(
		pGlobal->GetRadiance( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), rast ) );

	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( white, __FILE__, __LINE__, "legacy chain mirror painter (dl+mirror)" );
	PerfectReflectorMaterial* mirror = new PerfectReflectorMaterial( *white );
	GlobalLog()->PrintNew( mirror, __FILE__, __LINE__, "legacy chain mirror material (dl+mirror)" );

	StubObject* shadingObject = new StubObject();
	GlobalLog()->PrintNew( shadingObject, __FILE__, __LINE__, "dl+mirror shading object" );

	DirectLightingShaderOp* nee = new DirectLightingShaderOp( 0 );
	GlobalLog()->PrintNew( nee, __FILE__, __LINE__, "dl+mirror NEE op" );
	ReflectionShaderOp* refl = new ReflectionShaderOp();
	GlobalLog()->PrintNew( refl, __FILE__, __LINE__, "dl+mirror reflection op" );

	const RasterizerState rast2{};
	RayIntersection hit( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), rast2 );
	BuildHit( hit, *mirror, *shadingObject );

	IORStack stack( 1.0 );
	RandomNumberGenerator rng( 62000u );
	IndependentSampler sampler( rng );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	ScatteredRayContainer scattered;
	mirror->GetSPF()->Scatter( hit.geometric, sampler, scattered, stack );

	// No DistributionTracingShaderOp in this chain -> chainHasBsdfContinuationOp
	// should read FALSE from a real StandardShader; run BOTH values to
	// confirm the mirror's own answer is identical either way.
	for( int pass = 0; pass < 2; ++pass ) {
		const bool chainHasBsdfContinuationOp = ( pass == 1 );
		IRayCaster::RAY_STATE rs;
		rs.depth = 0;
		rs.importance = 1.0;
		rs.considerEmission = true;
		rs.type = IRayCaster::RAY_STATE::eRayDiffuse;
		rs.chainHasNEEOp = true;
		rs.chainHasBsdfContinuationOp = chainHasBsdfContinuationOp;

		RISEPel c( 0, 0, 0 ), total( 0, 0, 0 );
		nee->PerformOperation( rc, hit, *fx.pCaster, rs, c, stack, 0 );
		total = total + c;
		refl->PerformOperation( rc, hit, *fx.pCaster, rs, c, stack, &scattered );
		total = total + c;

		const Scalar m = ColorMath::MaxValue( total );
		std::cout << "    dl+mirror, chainHasBsdfContinuationOp=" << ( chainHasBsdfContinuationOp ? "TRUE" : "FALSE" )
			<< " " << m << " , expected " << Lenv << std::endl;
		CheckRel( m, Lenv, 1e-6,
			"DL-171 control: [DirectLighting, Reflection] on a pure mirror reads L_env "
			"exactly, unchanged by chainHasBsdfContinuationOp (no BSDF means NEE "
			"contributes 0 regardless; the delta lobe is untouched)" );
	}

	refl->release();
	nee->release();
	shadingObject->release();
	mirror->release();
	white->release();
}

//////////////////////////////////////////////////////////////////////
// Delta-op control: ReflectionShaderOp on a PERFECT MIRROR under a
// constant environment.  A delta lobe has NO distribution for an
// aggregate density to describe, so this row's partner is 0 before
// AND after the fix (DL-74's rule) -- the mean must be identical to
// within floating-point noise on both sides, proving the fix does not
// perturb the delta-lobe path `ReflectionShaderOp`/`RefractionShaderOp`
// are actually used for in production.
//////////////////////////////////////////////////////////////////////
static void RunDeltaControlRow()
{
	std::cout << "DL-171 control: ReflectionShaderOp on a perfect mirror is UNCHANGED "
		"(a delta lobe's partner is 0 before and after)" << std::endl;

	Fixture fx;
	Check( fx.Build( EnvOnlyScene(), "mirror" ), "legacy chain mirror fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	const IRadianceMap* pGlobal = fx.pScene->GetGlobalRadianceMap();
	if( !pGlobal ) { Check( false, "legacy chain mirror fixture has a radiance map" ); return; }
	const RasterizerState rast{};
	const Scalar Lenv = ColorMath::MaxValue(
		pGlobal->GetRadiance( Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), rast ) );

	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
	GlobalLog()->PrintNew( white, __FILE__, __LINE__, "legacy chain mirror painter" );
	PerfectReflectorMaterial* mirror = new PerfectReflectorMaterial( *white );
	GlobalLog()->PrintNew( mirror, __FILE__, __LINE__, "legacy chain mirror material" );

	StubObject* shadingObject = new StubObject();
	GlobalLog()->PrintNew( shadingObject, __FILE__, __LINE__, "mirror shading object" );

	ReflectionShaderOp* refl = new ReflectionShaderOp();
	GlobalLog()->PrintNew( refl, __FILE__, __LINE__, "legacy chain reflection op" );

	const RasterizerState rast2{};
	RayIntersection hit( Ray( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) ), rast2 );
	BuildHit( hit, *mirror, *shadingObject );

	IORStack stack( 1.0 );
	IRayCaster::RAY_STATE rs;
	rs.depth = 0;
	rs.importance = 1.0;
	rs.considerEmission = true;
	rs.type = IRayCaster::RAY_STATE::eRayDiffuse;

	RandomNumberGenerator rng( 61000u );
	IndependentSampler sampler( rng );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	ScatteredRayContainer scattered;
	mirror->GetSPF()->Scatter( hit.geometric, sampler, scattered, stack );
	Check( scattered.Count() == 1 && scattered[0].isDelta &&
		scattered[0].type == ScatteredRay::eRayReflection,
		"legacy chain mirror emits exactly one delta eRayReflection lobe" );

	RISEPel c( 0, 0, 0 );
	refl->PerformOperation( rc, hit, *fx.pCaster, rs, c, stack, &scattered );
	const Scalar m = ColorMath::MaxValue( c );
	std::cout << "    mirror reflecting a constant env " << m << " , expected " << Lenv << std::endl;
	CheckRel( m, Lenv, 1e-6,
		"DL-171 control: perfect mirror through ReflectionShaderOp reads L_env exactly "
		"(delta lobe, no MIS weighting applies)" );

	refl->release();
	shadingObject->release();
	mirror->release();
	white->release();
}

static void Run()
{
	RunEnvRow();
	RunAreaEmitterRows();
	RunDirectLightingAloneRow();
	RunDistributionTracingWithoutDirectLightingRow();
	RunDirectLightingPlusMirrorRow();
	RunDeltaControlRow();
}

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
