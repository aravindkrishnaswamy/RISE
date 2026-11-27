//////////////////////////////////////////////////////////////////////
//
//  HyLIoSSpatialTest.cpp - P7 nonlocal spatial skin domain & iterative
//    PathTracingIntegrator continuation tests:
//    1. PlanarPolygonDomain: interior exit vs absorbing side-wall
//       `DomainBoundaryLoss` tallied distinctly from bulk absorption.
//    2. ConcentricSphereDomain & ConcentricCylinderDomain: exact exit
//       on outer radius `R_outer`, outward normal alignment, and
//       rejection of `R_outer <= H_total`.
//    3. IntrinsicLayerVolumeDomain: valid chart mapping, UV boundary
//       loss, and singular-curvature refusal.
//    4. End-to-end `hylios_skin_material` in `transport_mode nonlocal`
//       (NO `response_asset` required!): CST parse, preparation-time
//       visible guide RGB, iterative `IntegrateRayNM` & `IntegrateRayHWSS`
//       transport, and distinct domain-boundary loss tally on tight footprint.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSSkinMaterial.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSSpatialGeometry.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Rendering/RadianceMap.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Color/SampledWavelengths.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::HyLIoS;

namespace {

int gFailures = 0;

void Check( const bool cond, const std::string& msg )
{
	if( !cond ) {
		++gFailures;
		std::cerr << "FAIL: " << msg << "\n";
	}
}

void Near( const double actual, const double expected, const double tol, const std::string& msg )
{
	if( !std::isfinite( actual ) || std::fabs( actual - expected ) > tol ) {
		++gFailures;
		std::cerr << "FAIL: " << msg << " (actual=" << actual << ", expected=" << expected
				  << ", tol=" << tol << ")\n";
	}
}

struct TempDirGuard
{
	std::filesystem::path dir;
	explicit TempDirGuard( const char* tag )
	{
		dir = std::filesystem::temp_directory_path() / ( std::string( "rise_hylios_sp_" ) + tag + "_" + std::to_string( std::rand() ) );
		std::filesystem::create_directories( dir );
	}
	~TempDirGuard()
	{
		std::error_code ec;
		std::filesystem::remove_all( dir, ec );
	}
};

void TestSpatialDomains()
{
	const SurfaceFrame3D frame{
		Point3( 0, 0, 0 ),
		Vector3( 1, 0, 0 ),
		Vector3( 0, 1, 0 ),
		Vector3( 0, 0, 1 )
	};
	const SlabVector outDir{ 0.0, 0.0, 1.0 };

	// 1. PlanarPolygonDomain
	{
		const PlanarPolygonDomain poly = PlanarPolygonDomain::MakeRectangle( 0.01, 0.01, 0.002, 2.0 );
		const SpatialExitMapping inside = poly.MapExit( frame, 0.0, 0.0, 0.005, -0.004, outDir );
		Check( inside.outcome == DomainMapOutcome::OuterSurfaceExit, "PlanarPolygon inside exit" );
		Near( inside.worldPos.x, 0.010, 1e-12, "PlanarPolygon physicalScale x" );
		Near( inside.worldPos.y, -0.008, 1e-12, "PlanarPolygon physicalScale y" );

		const SpatialExitMapping outside = poly.MapExit( frame, 0.0, 0.0, 0.02, 0.0, outDir );
		Check( outside.outcome == DomainMapOutcome::DomainBoundaryLoss, "PlanarPolygon absorbing side wall loss" );
	}

	// 2. ConcentricSphereDomain
	{
		const double R = 0.05;
		const Point3 center( 0, 0, -R );
		const ConcentricSphereDomain sph( center, R, 0.002, 1.0 );
		const SpatialExitMapping m = sph.MapExit( frame, 0.01, 0.005, outDir );
		Check( m.outcome == DomainMapOutcome::OuterSurfaceExit, "ConcentricSphere exit" );
		const Vector3 rel = Vector3Ops::mkVector3( m.worldPos, center );
		Near( Vector3Ops::Magnitude( rel ), R, 1e-12, "ConcentricSphere exit lies on R_outer" );
		const Vector3 expectedN = Vector3Ops::Normalize( rel );
		Near( Vector3Ops::Dot( m.worldNormal, expectedN ), 1.0, 1e-12, "ConcentricSphere outward normal exact" );

		bool threw = false;
		try { ConcentricSphereDomain bad( center, 0.001, 0.002, 1.0 ); }
		catch( const std::invalid_argument& ) { threw = true; }
		Check( threw, "ConcentricSphere rejects R_outer <= H_total" );
	}

	// 3. ConcentricCylinderDomain
	{
		const double R = 0.04;
		const Point3 axisOrigin( 0, 0, -R );
		const ConcentricCylinderDomain cyl( axisOrigin, Vector3( 0, 1, 0 ), R, 0.02, 0.002, 1.0 );
		const SpatialExitMapping m = cyl.MapExit( frame, 0.01, 0.005, outDir );
		Check( m.outcome == DomainMapOutcome::OuterSurfaceExit, "ConcentricCylinder interior exit" );
		const double rDist = std::hypot( m.worldPos.x, m.worldPos.z + R );
		Near( rDist, R, 1e-12, "ConcentricCylinder exit lies on R_outer" );

		const SpatialExitMapping capLoss = cyl.MapExit( frame, 0.0, 0.05, outDir );
		Check( capLoss.outcome == DomainMapOutcome::DomainBoundaryLoss, "ConcentricCylinder absorbing end cap" );
	}

	// 4. IntrinsicLayerVolumeDomain
	{
		IntrinsicLayerChart chart;
		chart.uMin = 0.0; chart.uMax = 1.0;
		chart.vMin = 0.0; chart.vMax = 1.0;
		chart.metresPerU = 0.02; chart.metresPerV = 0.02;
		chart.maxCurvatureInvM = 10.0; // 10 * 0.002 = 0.02 < 1
		const IntrinsicLayerVolumeDomain dom( chart, 0.002, 1.0 );
		Check( dom.ValidateCoordinate( 0.5, 0.5, 0.001 ), "IntrinsicLayer valid coordinate" );
		Check( !dom.ValidateCoordinate( 0.5, 0.5, 0.005 ), "IntrinsicLayer rejects depth > H_total" );
		const SpatialExitMapping inside = dom.MapExit( frame, 0.5, 0.5, 0.004, 0.0, outDir );
		Check( inside.outcome == DomainMapOutcome::OuterSurfaceExit, "IntrinsicLayer inside exit" );
		const SpatialExitMapping outside = dom.MapExit( frame, 0.5, 0.5, 0.05, 0.0, outDir );
		Check( outside.outcome == DomainMapOutcome::DomainBoundaryLoss, "IntrinsicLayer lateral loss" );

		bool threw = false;
		try {
			IntrinsicLayerChart bad = chart;
			bad.maxCurvatureInvM = 600.0; // 600 * 0.002 = 1.2 >= 1
			IntrinsicLayerVolumeDomain badDom( bad, 0.002, 1.0 );
		} catch( const std::invalid_argument& ) { threw = true; }
		Check( threw, "IntrinsicLayer rejects singular curvature * H_total >= 1" );
	}
}

void TestNonlocalMaterialCSTAndIterativePT()
{
	TempDirGuard tmp( "nonlocal_pt" );

	const std::string specText =
		"{\"schema_version\":1,\"approximate\":true,\"composition_revision\":\"synthetic-v1\","
		"\"coverage_profile_id\":\"explicit-synthetic-approximation\","
		"\"spectral_bundle_sha256\":\"untrusted_bundle_label\","
		"\"resolved_specimen_sha256\":\"untrusted_specimen_label\","
		"\"preset_id\":\"S3\",\"interpretation\":\"nonlocal test\","
		"\"units\":{\"length\":\"m\",\"coefficient\":\"m^-1\",\"wavelength\":\"nm\",\"mass_concentration\":\"kg/m^3\"},"
		"\"wavelength_nm\":[400,500,600,700],\"surface_fold_aspect_ratio\":0.75,\"provenance\":{\"retained\":\"yes\"},"
		"\"layers\":["
		"{\"layer_id\":\"stratum_corneum\",\"thickness_m\":0.00002,\"host_absorption_m_inv\":[50,30,15,10],\"real_refractive_index\":[1.45,1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[50,30,15,10]},\"particles\":null},"
		"{\"layer_id\":\"stratum_granulosum\",\"thickness_m\":0.00003,\"host_absorption_m_inv\":[100,60,25,15],\"real_refractive_index\":[1.4,1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[100,60,25,15]},\"particles\":null},"
		"{\"layer_id\":\"stratum_spinosum\",\"thickness_m\":0.00005,\"host_absorption_m_inv\":[120,70,30,15],\"real_refractive_index\":[1.4,1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[120,70,30,15]},\"particles\":null},"
		"{\"layer_id\":\"stratum_basale\",\"thickness_m\":0.00002,\"host_absorption_m_inv\":[200,100,40,20],\"real_refractive_index\":[1.4,1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[200,100,40,20]},\"particles\":null},"
		"{\"layer_id\":\"papillary_dermis\",\"thickness_m\":0.0002,\"host_absorption_m_inv\":[150,80,25,12],\"real_refractive_index\":[1.38,1.38,1.38,1.38],\"constituent_absorption_m_inv\":{\"s\":[150,80,25,12]},\"particles\":null},"
		"{\"layer_id\":\"reticular_dermis\",\"thickness_m\":0.0015,\"host_absorption_m_inv\":[150,80,25,12],\"real_refractive_index\":[1.38,1.38,1.38,1.38],\"constituent_absorption_m_inv\":{\"s\":[150,80,25,12]},\"particles\":null}"
		"]}\n";
	const std::filesystem::path specPath = tmp.dir / "s3_nonlocal.json";
	{
		std::ofstream ofs( specPath, std::ios::binary );
		ofs.write( specText.data(), static_cast<std::streamsize>( specText.size() ) );
	}

	// Parse nonlocal hylios_skin_material WITHOUT response_asset!
	const std::string sceneText =
		"RISE ASCII SCENE 7\n"
		"hylios_skin_material\n"
		"{\n"
		"  name skin_nl\n"
		"  preset S3\n"
		"  transport_mode nonlocal\n"
		"  boundary_mode rise_reciprocal_boundary_v1\n"
		"  boundary_roughness 0.25\n"
		"  spatial_domain planar_polygon\n"
		"  domain_half_width_m 0.05\n"
		"  exterior_ior 1.0\n"
		"  specimen_source " + specPath.string() + "\n"
		"}\n";

	const Cst::Document doc = Cst::ParseToCst( sceneText );
	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	std::vector<std::string> diags;
	Check( Cst::DeriveToJob( doc, *pJobI, &diags ), "DeriveToJob nonlocal hylios_skin_material without response_asset" );

	IMaterial* pMat = pJob->GetMaterials()->GetItem( "skin_nl" );
	const HyLIoSSkinMaterial* pHyLIoS = dynamic_cast<const HyLIoSSkinMaterial*>( pMat );
	Check( pHyLIoS != 0 && pHyLIoS->IsNonlocalSkin(), "Registered material is nonlocal HyLIoSSkinMaterial" );
	if( !pHyLIoS ) { safe_release( pJobI ); return; }

	// Preparation-time visible guide RGB is positive and shared with AOV guides
	const RISEPel guide = pHyLIoS->GetVisibleGuideRGB();
	Check( guide.r > 0.01 && guide.g > 0.01 && guide.b > 0.01, "Nonlocal preparation-time visible guide RGB positive" );
	Check( guide.r > guide.b, "Skin visible guide is warmer in red than blue (lower red absorption)" );

	// Build scene and run iterative NM + HWSS path tracing
	const double loc[3] = { 0, 0, 2 }, look[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
	const double oriZero[3] = { 0, 0, 0 }, targetZero[2] = { 0, 0 };
	pJob->SetFilm( 16, 16, 1.0 );
	pJob->AddPinholeCamera( "cam", loc, look, up, 0.8, 0.0, 0.0, 0.0, oriZero, targetZero );
	pJob->AddBoxGeometry( "slab", 20.0, 20.0, 0.2 );
	const double pos[3] = { 0, 0, -0.1 }, sca[3] = { 1, 1, 1 };
	pJob->AddObject( "skin_obj", "slab", "skin_nl", 0, 0, RadianceMapConfig(), pos, oriZero, sca, true, true );

	UniformColorPainter* pEnvPainter = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) );
	RadianceMap* pEnvMap = new RadianceMap( *pEnvPainter, 1.0 );
	pJob->SetGlobalRadianceMap( pEnvMap );

	IScenePriv* pScene = pJob->GetScene();
	pScene->GetObjects()->PrepareForRendering();
	std::vector<IShaderOp*> emptyOps;
	StandardShader* pDefShader = new StandardShader( emptyOps );
	RayCaster* pCaster = new RayCaster( true, 8, *pDefShader, true );
	pCaster->AttachScene( pScene );
	PathTracingIntegrator* pIntegrator = new PathTracingIntegrator( ManifoldSolverConfig(), StabilityConfig() );
	const IRadianceMap* pEnv = pScene->GetGlobalRadianceMap();

	const Ray camRay( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) );
	double sum450 = 0.0, sum650 = 0.0;
	double sumHW450 = 0.0, sumHW650 = 0.0;
	const int N = 200;
	for( int s = 0; s < N; ++s ) {
		RandomNumberGenerator rng( 3000u + static_cast<unsigned int>( s ) );
		IndependentSampler smp( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		const Scalar l450 = pIntegrator->IntegrateRayNM( rc, nullRasterizerState, camRay, 450.0, *pScene, *pCaster, smp, pEnv );
		const Scalar l650 = pIntegrator->IntegrateRayNM( rc, nullRasterizerState, camRay, 650.0, *pScene, *pCaster, smp, pEnv );
		Check( std::isfinite( l450 ) && l450 >= 0.0 && std::isfinite( l650 ) && l650 >= 0.0, "Nonlocal NM finite non-negative" );
		sum450 += l450;
		sum650 += l650;

		RandomNumberGenerator rngH( 9000u + static_cast<unsigned int>( s ) );
		IndependentSampler smpH( rngH );
		RuntimeContext rcH( rngH, RuntimeContext::PASS_NORMAL, false );
		SampledWavelengths swl;
		swl.lambda[0] = 450.0; swl.lambda[1] = 520.0; swl.lambda[2] = 600.0; swl.lambda[3] = 650.0;
		for( int i = 0; i < 4; ++i ) { swl.pdf[i] = 1.0; swl.terminated[i] = false; }
		Scalar res[SampledWavelengths::N] = { 0 };
		pIntegrator->IntegrateRayHWSS( rcH, nullRasterizerState, camRay, swl, *pScene, *pCaster, smpH, pEnv, res );
		Check( std::isfinite( res[0] ) && std::isfinite( res[3] ), "Nonlocal HWSS delegated lanes finite" );
		sumHW450 += res[0];
		sumHW650 += res[3];
	}
	Check( sum450 / N > 0.01 && sum650 / N > 0.05, "Nonlocal NM produces positive subsurface + boundary radiance" );
	Check( sumHW450 / N > 0.01 && sumHW650 / N > 0.05, "Nonlocal HWSS produces positive subsurface + boundary radiance" );

	// Tight planar footprint (half-width = 1e-4 m) triggers distinct DomainBoundaryLoss tally
	HyLIoSSkinMaterialConfig tightCfg;
	tightCfg.transportMode = "nonlocal";
	tightCfg.boundaryMode = "rise_reciprocal_boundary_v1";
	tightCfg.boundaryRoughness = 0.25;
	tightCfg.spatialDomain = "planar_polygon";
	tightCfg.domainHalfWidthMetres = 1e-4;
	const std::string specPathStr = specPath.string();
	tightCfg.specimenSourcePath = specPathStr.c_str();
	IMaterial* pTightI = 0;
	Check( RISE_API_CreateHyLIoSSkinMaterial( &pTightI, tightCfg ) && pTightI != 0, "Create tight nonlocal material" );
	const HyLIoSSkinMaterial* pTight = dynamic_cast<const HyLIoSSkinMaterial*>( pTightI );
	if( pTight ) {
		RayIntersectionGeometric ri( camRay, nullRasterizerState );
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( ri.vNormal );
		RandomNumberGenerator rng( 777u );
		IndependentSampler smp( rng );
		for( int s = 0; s < 300; ++s ) {
			HyLIoSNonlocalSample nl;
			pTight->SampleNonlocalNM( ri, 650.0, smp, nl );
		}
		Check( pTight->DomainBoundaryLossCount() > 0, "Tight footprint tallies distinct DomainBoundaryLoss > 0" );
	}
	safe_release( pTightI );

	safe_release( pIntegrator );
	safe_release( pCaster );
	safe_release( pDefShader );
	safe_release( pEnvMap );
	safe_release( pEnvPainter );
	safe_release( pJobI );
}

} // namespace

int main()
{
	TestSpatialDomains();
	TestNonlocalMaterialCSTAndIterativePT();

	if( gFailures == 0 ) {
		std::cout << "HyLIoSSpatialTest: ALL PASSED\n";
		return 0;
	}
	std::cerr << "HyLIoSSpatialTest: " << gFailures << " FAILURES\n";
	return 1;
}
