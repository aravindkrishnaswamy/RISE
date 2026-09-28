//////////////////////////////////////////////////////////////////////
//
//  BDPTGuidedContinuationTest.cpp - Generator-level red-proof for
//    DL-67 (docs/DL67_GUIDED_GENERATING_DENSITY.md) on BDPT's eye and
//    light subpath generators: path guiding must not move the
//    expectation of the continuation weight at a MULTI-LOBE,
//    multi-emit SPF.
//
//  WHY GENERATOR LEVEL.  A full BDPT render cannot pin one-sample
//  guiding: the rasterizer scales the configured alpha by its adaptive
//  variance heuristic (`BDPTRasterizerBase`), which settles near 0.016
//  on the scene-level twin (BDPTStrategyBalanceTest's guided topology
//  L), so the one-sample branches fire on ~1% of vertices.  Here the
//  field is installed directly with `BDPTIntegrator::SetGuidingField`
//  at a FIXED alpha, and the quantity measured is the continuation
//  weight itself -- no MIS, no connection strategies, no film.
//
//  THE INVARIANT.
//
//   Eye side.  A camera ray hits a large plane at the origin at a
//   fixed incidence; the continuation escapes to a constant
//   environment, so the generator emits exactly three vertices
//   (camera, surface, synthetic env vertex) and the env vertex's stored
//   throughput IS the surface vertex's continuation weight.  Its mean
//   is the material's directional albedo `integral f(w) |cos w| dw`,
//   which a deterministic quadrature of the material's own `IBSDF`
//   gives independently of any integrator.  An UN-GUIDED row validates
//   that reference (green before and after the fix).
//
//   Light side.  A narrow spot light far along the same incidence
//   direction hits the plane; an enclosing sphere catches every
//   continuation, so the weight is `beta(v2) / beta(v1)`.  The
//   light-side estimator prices IMPORTANCE through a radiance-mode
//   `kray`, and `translucent_material` is not reciprocal (DL-223), so
//   the reference here is the UN-GUIDED generator's own mean at 4x the
//   sample count: guiding must reproduce it.
//
//  WHAT WAS BROKEN (pre-fix, `BDPTIntegrator.cpp` at master 6b91fd19).
//  A guide-SUBSTITUTED direction was priced `f_agg cos / (selectProb *
//  p_c)` -- a `1/selectProb` on a draw that is not conditioned on any
//  lobe; the kept direction divided `kray_I p_I` by a mixture built
//  from the lobe's OWN `p_I`; RIS candidate 0's proposal density was
//  `p_I` while candidate 1's was the aggregate, and a RIS pick of
//  candidate 0 was then priced as if substituted; a guide draw that
//  produced nothing fell back to the lobe's own direction; and a lobe
//  the generator never guides (`schlick_material`'s specular,
//  `translucent_material`'s transmission) kept `kray/q` un-weighted
//  while the guide, pricing the AGGREGATE BSDF, covered it too.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/PathGuidingField.h"
#include "../src/Library/Utilities/PathVertexEval.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"

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
	const double relErr = expected != 0 ? std::fabs( measured / expected - 1.0 ) : std::fabs( measured );
	const bool ok = relErr <= relTol;
	if( ok ) {
		passCount++;
		std::cout << "    ok   " << what << "  measured=" << measured << " expected="
			<< expected << " relErr=" << relErr * 100.0 << "%" << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << what << "  measured=" << measured << " expected="
			<< expected << " relErr=" << relErr * 100.0 << "% tol=" << relTol * 100.0 << "%"
			<< std::endl;
	}
}

#ifdef RISE_ENABLE_OPENPGL

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

//! Builds the Job and renders one throwaway frame so the live
//! RayCaster / LightSampler / EnvironmentSampler exist.
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
		std::snprintf( path, sizeof(path), "/tmp/bdpt_guided_cont_%s_%d.RISEscene",
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
// Scenes.
//////////////////////////////////////////////////////////////////////

//! The three materials under test, each named `mat_under_test`.
//! DL-67 round 2 added the last three: a BLACK-diffuse schlick (the
//! realization can hold only a zero-weight diffuse ray -- external review
//! P1-1) and two materials under a 30-degree TILTED SHADING NORMAL (a
//! constant `normal_map_modifier`; the diffuse draw is dropped below the
//! geometric horizon on some realizations -- review P2).
enum MaterialKind { kSchlick, kTranslucent, kLambertian,
	kSchlickBlackDiffuse, kLambertianTilted, kSchlickTilted };

static const char* MaterialName( MaterialKind k )
{
	switch( k ) {
		case kSchlick:             return "schlick_material";
		case kTranslucent:         return "translucent_material";
		case kSchlickBlackDiffuse: return "schlick_material (black diffuse)";
		case kLambertianTilted:    return "lambertian_material (30 deg tilted normal)";
		case kSchlickTilted:       return "schlick_material (30 deg tilted normal)";
		default:                   return "lambertian_material";
	}
}

static bool IsTilted( MaterialKind k )
{
	return k == kLambertianTilted || k == kSchlickTilted;
}

//! A constant tangent-space normal (sin 30, 0, cos 30), stored linear:
//! a uniform 30-degree tilt of the shading normal toward +u.
static std::string TiltModifier()
{
	return
		"uniformcolor_painter\n{\n\tname pnt_tilt\n\tcolor 0.75 0.5 0.9330127\n"
		"\tcolorspace Rec709RGB_Linear\n}\n"
		"normal_map_modifier\n{\n\tname nm_tilt\n\tnormal_map pnt_tilt\n\tscale 1.0\n}\n";
}

static std::string MaterialChunk( MaterialKind k )
{
	switch( k ) {
		case kSchlickBlackDiffuse:
			return
				"uniformcolor_painter\n{\n\tname pnt_rd\n\tcolor 0 0 0\n}\n"
				"uniformcolor_painter\n{\n\tname pnt_rs\n\tcolor 0.9 0.9 0.9\n}\n"
				"scalar_painter\n{\n\tname pnt_rough\n\tvalue 0.2\n}\n"
				"scalar_painter\n{\n\tname pnt_iso\n\tvalue 1.0\n}\n"
				"schlick_material\n{\n\tname mat_under_test\n\trd pnt_rd\n\trs pnt_rs\n"
				"\troughness pnt_rough\n\tisotropy pnt_iso\n}\n";
		case kLambertianTilted:
			return MaterialChunk( kLambertian ) + TiltModifier();
		case kSchlickTilted:
			return MaterialChunk( kSchlick ) + TiltModifier();
		case kSchlick:
			return
				"uniformcolor_painter\n{\n\tname pnt_rd\n\tcolor 0.5 0.5 0.5\n}\n"
				"uniformcolor_painter\n{\n\tname pnt_rs\n\tcolor 0.5 0.5 0.5\n}\n"
				"scalar_painter\n{\n\tname pnt_rough\n\tvalue 0.2\n}\n"
				"scalar_painter\n{\n\tname pnt_iso\n\tvalue 1.0\n}\n"
				"schlick_material\n{\n\tname mat_under_test\n\trd pnt_rd\n\trs pnt_rs\n"
				"\troughness pnt_rough\n\tisotropy pnt_iso\n}\n";
		case kTranslucent:
			return
				"uniformcolor_painter\n{\n\tname pnt_ref\n\tcolor 0.4 0.4 0.4\n}\n"
				"uniformcolor_painter\n{\n\tname pnt_tau\n\tcolor 0.5 0.5 0.5\n}\n"
				"translucent_material\n{\n\tname mat_under_test\n\tref pnt_ref\n\ttau pnt_tau\n"
				"\text 0.0\n\tN 4.0\n\tscattering 0.0\n}\n";
		default:
			return
				"uniformcolor_painter\n{\n\tname pnt_lam\n\tcolor 0.8 0.8 0.8\n}\n"
				"lambertian_material\n{\n\tname mat_under_test\n\treflectance pnt_lam\n}\n";
	}
}

static std::string Plane( MaterialKind k )
{
	return std::string(
		"clippedplane_geometry\n{\n\tname plane\n"
		"\tpta -50 -50 0\n\tptb 50 -50 0\n\tptc 50 50 0\n\tptd -50 50 0\n}\n"
		"standard_object\n{\n\tname obj_plane\n\tgeometry plane\n\tmaterial mat_under_test\n" )
		+ ( IsTilted( k ) ? "\tmodifier nm_tilt\n" : "" ) + "}\n";
}

static const char* kRasterizerAndCamera =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n"
	"film\n{\n\twidth 4\n\theight 4\n}\n"
	"pinhole_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n";

//! Eye scene: the plane under a constant environment.
static std::string EyeScene( MaterialKind k )
{
	return std::string( "RISE ASCII SCENE 7\n" )
		+ "uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 0.6 0.6 0.6\n}\n"
		+ MaterialChunk( k ) + Plane( k ) + kRasterizerAndCamera
		+ "bdpt_pel_rasterizer\n{\n\tsamples 1\n\tmax_eye_depth 3\n\tmax_light_depth 3\n"
		  "\tpixel_filter box\n\toidn_denoise FALSE\n\tradiance_map pnt_env\n"
		  "\tradiance_background TRUE\n}\n";
}

static const Scalar kIncidenceDeg = 40.0;
static const Scalar kSpotDist = 50.0;

//! Light scene: the plane, a narrow spot light far along the
//! incidence direction, and an enclosing sphere that catches every
//! continuation.
static std::string LightScene( MaterialKind k )
{
	const Scalar t = kIncidenceDeg * PI / 180.0;
	std::ostringstream ss;
	ss << "RISE ASCII SCENE 7\n"
		<< MaterialChunk( k ) << Plane( k )
		<< "uniformcolor_painter\n{\n\tname pnt_wall\n\tcolor 0.5 0.5 0.5\n}\n"
		<< "lambertian_material\n{\n\tname mat_wall\n\treflectance pnt_wall\n}\n"
		<< "sphere_geometry\n{\n\tname enclosure\n\tradius 200\n}\n"
		<< "standard_object\n{\n\tname obj_enclosure\n\tgeometry enclosure\n\tmaterial mat_wall\n}\n"
		<< "spot_light\n{\n\tname spot\n\tposition "
		<< kSpotDist * std::sin( t ) << " 0 " << kSpotDist * std::cos( t )
		<< "\n\ttarget 0 0 0\n\tcolor 1 1 1\n\tpower 100.0\n\tinner 0.25\n\touter 0.5\n}\n"
		<< kRasterizerAndCamera
		<< "bdpt_pel_rasterizer\n{\n\tsamples 1\n\tmax_eye_depth 3\n\tmax_light_depth 3\n"
		   "\tpixel_filter box\n\toidn_denoise FALSE\n}\n";
	return ss.str();
}

//! The incoming direction: from +x/+z toward the origin at
//! `kIncidenceDeg` off the +Z normal (the spot light's own direction).
static Vector3 Incidence()
{
	const Scalar t = kIncidenceDeg * PI / 180.0;
	return Vector3( -std::sin( t ), 0, -std::cos( t ) );
}

//////////////////////////////////////////////////////////////////////
// Deterministic quadrature of `value(w) |cos(w, +Z)|` over the full
// sphere, at the hit the eye camera ray really produces (probed through
// the live scene), evaluated through the SAME
// `PathVertexEval::EvalBSDFAtSurface` and live IOR stack the path
// integrators use.
//////////////////////////////////////////////////////////////////////
static Scalar AlbedoQuadrature( const Fixture& fx, const Vector3& inDir, bool& ok )
{
	ok = false;
	const RasterizerState rast{};
	RayIntersection probe( Ray( Point3( -inDir.x, -inDir.y, -inDir.z ), inDir ), rast );
	fx.pScene->GetObjects()->IntersectRay( probe, true, true, false );
	if( !probe.geometric.bHit || !probe.pMaterial || !probe.pMaterial->GetBSDF() ) {
		return 0;
	}
	// The generators apply the intersection modifier (the tilt) before
	// scattering; the quadrature must see the same shading frame.
	if( probe.pModifier ) {
		probe.pModifier->Modify( probe.geometric );
	}
	const Vector3 shadingN = probe.geometric.vNormal;
	IORStack stack( 1.0 );
	stack.SetCurrentObject( probe.pObject );
	const IBSDF* pB = probe.pMaterial->GetBSDF();

	const unsigned int nz = 2048, nphi = 2048;
	const double dz = 2.0 / nz;
	const double dphi = TWO_PI / nphi;
	double sum = 0;
	for( unsigned int iz = 0; iz < nz; ++iz ) {
		const double z = -1.0 + ( iz + 0.5 ) * dz;
		const double r = std::sqrt( r_max( 0.0, 1.0 - z * z ) );
		double ring = 0;
		for( unsigned int ip = 0; ip < nphi; ++ip ) {
			const double phi = ( ip + 0.5 ) * dphi;
			const Vector3 w( r * std::cos( phi ), r * std::sin( phi ), z );
			// The generators' cosine is against the SHADING normal.
			ring += ColorMath::MaxValue(
				PathVertexEval::EvalBSDFAtSurface( pB, w, probe.geometric, &stack ) ) *
				std::fabs( Vector3Ops::Dot( w, shadingN ) );
		}
		sum += ring;
	}
	ok = true;
	return static_cast<Scalar>( sum * dz * dphi );
}

//////////////////////////////////////////////////////////////////////
// A trained guiding field about `axis` (cos^power "incident radiance"),
// every sample deposited at the origin so the field has one spatial
// region -- the same construction as PTGuidingMISPartitionTest's.
//////////////////////////////////////////////////////////////////////
static PathGuidingField* BuildField( const Vector3& axis, const double power )
{
	PathGuidingConfig config;
	config.enabled = true;
	PathGuidingField* guide = new PathGuidingField( config,
		Point3( -2, -2, -2 ), Point3( 2, 2, 2 ) );
	GlobalLog()->PrintNew( guide, __FILE__, __LINE__, "guiding field" );
	guide->BeginTrainingIteration();
	const unsigned int kSamples = 16384;
	const Point3 at( 0, 0, 0 );
	for( unsigned int i = 0; i < kSamples; ++i ) {
		const Scalar z = -1 + 2 * ( i + 0.5 ) / kSamples;
		const Scalar phi = i * 2.399963229728653;
		const Scalar r = std::sqrt( 1 - z * z );
		const Vector3 dir( r * std::cos( phi ), r * std::sin( phi ), z );
		const Scalar c = Vector3Ops::Dot( dir, axis );
		const Scalar lum = c > 0 ? std::pow( (double)c, power ) : 0.0;
		if( lum <= 1e-9 ) {
			guide->AddZeroValueSample( at, dir );
		} else {
			guide->AddSample( at, dir, 1, 1 / ( 4 * PI ), lum, false );
		}
	}
	guide->EndTrainingIteration();
	return guide;
}

struct GuidingMode
{
	const char*			name;
	GuidingSamplingType	type;
	Scalar				alpha;		///< 0 = un-guided
};

static const GuidingMode kModes[] = {
	{ "one-sample alpha 0.7", eGuidingOneSampleMIS, 0.7 },
	{ "RIS",                  eGuidingRIS,          0.7 },
};

//////////////////////////////////////////////////////////////////////
// Eye-side batch: mean continuation weight over `n` eye subpaths.
//////////////////////////////////////////////////////////////////////
static Scalar EyeBatch(
	const Fixture& fx,
	PathGuidingField* guide,
	const GuidingMode& mode,
	const bool bNM,
	const unsigned int n,
	const unsigned int seedBase,
	unsigned int& nEscaped,
	Scalar* pStdErr = 0 )
{
	BDPTIntegrator* bdpt = new BDPTIntegrator( 3, 3, StabilityConfig() );
	GlobalLog()->PrintNew( bdpt, __FILE__, __LINE__, "bdpt integrator" );
	bdpt->SetLightSampler( fx.pCaster->GetLightSampler() );
	bdpt->SetGuidingField( mode.alpha > 0 ? guide : 0, 0, mode.alpha, 4, 0, mode.type, 2 );

	const Vector3 inDir = Incidence();
	const Ray camRay( Point3( -inDir.x, -inDir.y, -inDir.z ), inDir );
	std::vector<BDPTVertex> verts;
	std::vector<uint32_t> starts;
	double sum = 0;
	double sumSq = 0;
	nEscaped = 0;
	for( unsigned int i = 0; i < n; ++i ) {
		RandomNumberGenerator rng( seedBase + i );
		IndependentSampler sampler( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		verts.clear();
		starts.clear();
		if( bNM ) {
			bdpt->GenerateEyeSubpathNM( rc, camRay, Point2( 0, 0 ), *fx.pScene, *fx.pCaster,
				sampler, verts, starts, 550.0, 0 );
		} else {
			bdpt->GenerateEyeSubpath( rc, camRay, Point2( 0, 0 ), *fx.pScene, *fx.pCaster,
				sampler, verts, starts );
		}
		if( verts.size() == 3 && verts[2].pEnvLight != 0 ) {
			nEscaped++;
			const double w = bNM ? verts[2].throughputNM : ColorMath::MaxValue( verts[2].throughput );
			sum += w;
			sumSq += w * w;
		}
	}
	bdpt->release();
	const double mean = sum / n;
	if( pStdErr ) {
		*pStdErr = static_cast<Scalar>( std::sqrt( r_max( 0.0, sumSq / n - mean * mean ) / n ) );
	}
	return static_cast<Scalar>( mean );
}

//////////////////////////////////////////////////////////////////////
// Light-side batch: mean `beta(v2) / beta(v1)` over `n` light subpaths
// whose first surface vertex is the plane.
//////////////////////////////////////////////////////////////////////
static Scalar LightBatch(
	const Fixture& fx,
	PathGuidingField* guide,
	const GuidingMode& mode,
	const unsigned int n,
	const unsigned int seedBase,
	unsigned int& nOnPlane )
{
	BDPTIntegrator* bdpt = new BDPTIntegrator( 3, 3, StabilityConfig() );
	GlobalLog()->PrintNew( bdpt, __FILE__, __LINE__, "bdpt integrator" );
	bdpt->SetLightSampler( fx.pCaster->GetLightSampler() );
	bdpt->SetGuidingField( 0, mode.alpha > 0 ? guide : 0, mode.alpha, 0, 4, mode.type, 2 );

	std::vector<BDPTVertex> verts;
	std::vector<uint32_t> starts;
	double sum = 0;
	nOnPlane = 0;
	for( unsigned int i = 0; i < n; ++i ) {
		RandomNumberGenerator rng( seedBase + i );
		IndependentSampler sampler( rng );
		verts.clear();
		starts.clear();
		bdpt->GenerateLightSubpath( *fx.pScene, *fx.pCaster, sampler, verts, starts, rng );
		if( verts.size() < 2 || verts[1].type != BDPTVertex::SURFACE ||
			std::fabs( verts[1].position.z ) > 1e-3 ) {
			continue;
		}
		nOnPlane++;
		const Scalar b1 = ColorMath::MaxValue( verts[1].throughput );
		if( b1 <= 0 ) {
			continue;
		}
		if( verts.size() >= 3 ) {
			sum += ColorMath::MaxValue( verts[2].throughput ) / b1;
		}
	}
	bdpt->release();
	return nOnPlane > 0 ? static_cast<Scalar>( sum / nOnPlane ) : Scalar( 0 );
}

static void RunEyeFurnace( MaterialKind k )
{
	std::cout << "BDPT eye generator, " << MaterialName( k )
		<< " at " << kIncidenceDeg << " deg under a constant environment" << std::endl;
	Fixture fx;
	Check( fx.Build( EyeScene( k ), "eye" ), "eye fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	bool ok = false;
	const Scalar albedo = AlbedoQuadrature( fx, Incidence(), ok );
	Check( ok, "eye fixture: the camera ray hits the plane's material" );
	if( !ok ) return;
	std::cout << "    directional albedo (quadrature) " << albedo << std::endl;

	const unsigned int kN = 200000;
	unsigned int nEsc = 0;
	{
		const GuidingMode off{ "un-guided", eGuidingOneSampleMIS, 0.0 };
		const Scalar m = EyeBatch( fx, 0, off, false, kN, 1000, nEsc );
		Check( nEsc > kN / 2, "eye un-guided: most subpaths escape to the environment" );
		const std::string label = std::string( "eye " ) + MaterialName( k ) +
			" [un-guided]: continuation weight reads the quadrature albedo";
		CheckRel( m, albedo, 0.015, label.c_str() );
	}

	const Vector3 axes[2] = {
		Vector3Ops::Normalize( Vector3( -0.6, 0.0, 0.8 ) ),	// up, toward the mirror side
		Vector3Ops::Normalize( Vector3( 0.3, 0.0, -0.95 ) ) };	// below the horizon
	const double powers[2] = { 2.0, 64.0 };
	// Distinct, non-overlapping per-row seed ranges (external review P3:
	// `seed += 1000` with 200000 samples made consecutive rows share
	// ~99.5% of their per-sample seeds, so the rows were not independent).
	unsigned int seed = 10000000;
	for( unsigned int ai = 0; ai < 2; ++ai ) {
		for( unsigned int pi = 0; pi < 2; ++pi ) {
			PathGuidingField* guide = BuildField( axes[ai], powers[pi] );
			for( unsigned int mi = 0; mi < sizeof( kModes ) / sizeof( kModes[0] ); ++mi ) {
				for( unsigned int nm = 0; nm < 2; ++nm ) {
					if( nm == 1 && k != kSchlick ) {
						continue;
					}
					Scalar se = 0;
					const Scalar m = EyeBatch( fx, guide, kModes[mi], nm == 1, kN, seed, nEsc, &se );
					seed += kN;
					std::cout << "    z = " << ( m - albedo ) / r_max( se, Scalar( 1e-12 ) )
						<< " (standard error " << se << ")" << std::endl;
					std::ostringstream label;
					label << "eye " << MaterialName( k ) << " [" << kModes[mi].name
						<< ( nm ? ", NM" : ", RGB" ) << ", guide axis " << ai
						<< " cos^" << powers[pi] << "]: guided continuation reads the albedo";
					CheckRel( m, albedo, 0.015, label.str().c_str() );
				}
			}
			guide->release();
		}
	}
}

static void RunLightFurnace( MaterialKind k )
{
	std::cout << "BDPT light generator, " << MaterialName( k )
		<< ": guided vs un-guided continuation weight" << std::endl;
	Fixture fx;
	Check( fx.Build( LightScene( k ), "light" ), "light fixture builds" );
	if( !fx.pCaster || !fx.pScene ) return;

	unsigned int nOn = 0;
	const GuidingMode off{ "un-guided", eGuidingOneSampleMIS, 0.0 };
	const Scalar reference = LightBatch( fx, 0, off, 800000, 5000000, nOn );
	Check( nOn > 400000, "light un-guided: most light subpaths land on the plane" );
	std::cout << "    un-guided reference " << reference << " over " << nOn << " subpaths" << std::endl;

	const Vector3 axes[2] = {
		Vector3Ops::Normalize( Vector3( -0.6, 0.0, 0.8 ) ),
		Vector3Ops::Normalize( Vector3( 0.3, 0.0, -0.95 ) ) };
	unsigned int seed = 6000000;
	for( unsigned int ai = 0; ai < 2; ++ai ) {
		PathGuidingField* guide = BuildField( axes[ai], 2.0 );
		for( unsigned int mi = 0; mi < sizeof( kModes ) / sizeof( kModes[0] ); ++mi ) {
			const Scalar m = LightBatch( fx, guide, kModes[mi], 200000, seed, nOn );
			seed += 1000000;
			std::ostringstream label;
			label << "light " << MaterialName( k ) << " [" << kModes[mi].name
				<< ", guide axis " << ai << "]: guided continuation matches un-guided";
			CheckRel( m, reference, 0.015, label.str().c_str() );
		}
		guide->release();
	}
}

int main()
{
	GlobalLog();
	RunEyeFurnace( kLambertian );
	RunEyeFurnace( kSchlick );
	RunEyeFurnace( kTranslucent );
	RunEyeFurnace( kSchlickBlackDiffuse );
	RunEyeFurnace( kLambertianTilted );
	RunEyeFurnace( kSchlickTilted );
	RunLightFurnace( kLambertian );
	RunLightFurnace( kSchlick );
	RunLightFurnace( kTranslucent );
	RunLightFurnace( kSchlickBlackDiffuse );
	RunLightFurnace( kLambertianTilted );

	std::cout << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	if( failCount == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	}
	return 1;
}

#else

int main()
{
	std::cout << "BDPTGuidedContinuationTest: build without OpenPGL -- no guiding to test" << std::endl;
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	(void)Check;
	(void)CheckRel;
	return 0;
}

#endif
