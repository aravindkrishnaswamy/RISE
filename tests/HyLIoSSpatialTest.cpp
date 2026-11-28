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
#include "../src/Library/Materials/HyLIoS/HyLIoSReciprocalBoundary.h"
#include "../src/Library/Rendering/PathTracingSpectralRasterizer.h"
#include "../src/Library/Rendering/HyperspectralRadiometricRasterizer.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Rendering/RadianceMap.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Materials/MixMaterial.h"
#include "../src/Library/Materials/LambertianMaterial.h"
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

void TestNonlocalNEEAndSmoothBoundaryRefusal()
{
	TempDirGuard tmp( "nee_mis" );

	// 1. Black-interior specimen (mu_a = 1e8 m^-1) to isolate first-surface `K_r` NEE
	//    under a calibrated physical_directional_light.
	const std::string blackSpec =
		"{\"schema_version\":1,\"approximate\":true,\"composition_revision\":\"black-v1\","
		"\"coverage_profile_id\":\"explicit-synthetic-approximation\","
		"\"spectral_bundle_sha256\":\"black_b\",\"resolved_specimen_sha256\":\"black_s\","
		"\"preset_id\":\"S3\",\"interpretation\":\"black interior\","
		"\"units\":{\"length\":\"m\",\"coefficient\":\"m^-1\",\"wavelength\":\"nm\",\"mass_concentration\":\"kg/m^3\"},"
		"\"wavelength_nm\":[450,550,650],\"surface_fold_aspect_ratio\":0.75,\"provenance\":{\"retained\":\"yes\"},"
		"\"layers\":["
		"{\"layer_id\":\"stratum_corneum\",\"thickness_m\":0.001,\"host_absorption_m_inv\":[1e8,1e8,1e8],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[1e8,1e8,1e8]},\"particles\":null},"
		"{\"layer_id\":\"stratum_granulosum\",\"thickness_m\":0.001,\"host_absorption_m_inv\":[1e8,1e8,1e8],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[1e8,1e8,1e8]},\"particles\":null},"
		"{\"layer_id\":\"stratum_spinosum\",\"thickness_m\":0.001,\"host_absorption_m_inv\":[1e8,1e8,1e8],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[1e8,1e8,1e8]},\"particles\":null},"
		"{\"layer_id\":\"stratum_basale\",\"thickness_m\":0.001,\"host_absorption_m_inv\":[1e8,1e8,1e8],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[1e8,1e8,1e8]},\"particles\":null},"
		"{\"layer_id\":\"papillary_dermis\",\"thickness_m\":0.001,\"host_absorption_m_inv\":[1e8,1e8,1e8],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[1e8,1e8,1e8]},\"particles\":null},"
		"{\"layer_id\":\"reticular_dermis\",\"thickness_m\":0.001,\"host_absorption_m_inv\":[1e8,1e8,1e8],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[1e8,1e8,1e8]},\"particles\":null}"
		"]}\n";
	const std::filesystem::path blackPath = tmp.dir / "black.json";
	{
		std::ofstream ofs( blackPath, std::ios::binary );
		ofs.write( blackSpec.data(), static_cast<std::streamsize>( blackSpec.size() ) );
	}
	const std::string blackPathStr = blackPath.string();

	IJob* pJobI = 0;
	RISE_CreateJob( &pJobI );
	Job* pJob = dynamic_cast<Job*>( pJobI );
	const double loc[3] = { 0, 0, 2 }, look[3] = { 0, 0, 0 }, up[3] = { 0, 1, 0 };
	const double oriZero[3] = { 0, 0, 0 }, targetZero[2] = { 0, 0 };
	pJob->SetFilm( 8, 8, 1.0 );
	pJob->AddPinholeCamera( "cam", loc, look, up, 0.8, 0.0, 0.0, 0.0, oriZero, targetZero );
	pJob->AddBoxGeometry( "slab", 20.0, 20.0, 0.2 );

	HyLIoSSkinMaterialConfig blackCfg;
	blackCfg.transportMode = "nonlocal";
	blackCfg.boundaryMode = "rise_reciprocal_boundary_v1";
	blackCfg.boundaryRoughness = 0.25;
	blackCfg.spatialDomain = "planar_polygon";
	blackCfg.domainHalfWidthMetres = 0.5;
	blackCfg.exteriorIOR = 1.0;
	blackCfg.specimenSourcePath = blackPathStr.c_str();
	Check( pJob->AddHyLIoSSkinMaterial( "skin_black", blackCfg ), "Add black-interior nonlocal material" );
	const double pos[3] = { 0, 0, -0.1 }, sca[3] = { 1, 1, 1 };
	pJob->AddObject( "skin_obj", "slab", "skin_black", 0, 0, RadianceMapConfig(), pos, oriZero, sca, true, true );

	const double wls[3] = { 450.0, 550.0, 650.0 };
	const double irr[3] = { 20.0, 20.0, 20.0 };
	const double dir[3] = { 0.0, 0.0, 1.0 }; // normal incidence
	Check( pJob->AddPhysicalDirectionalLight( "sun", 3, wls, irr, "zero", 1.0, dir ),
		"Add calibrated directional light for K_r NEE test" );

	IScenePriv* pScene = pJob->GetScene();
	pScene->GetObjects()->PrepareForRendering();
	std::vector<IShaderOp*> emptyOps;
	StandardShader* pDefShader = new StandardShader( emptyOps );
	RayCaster* pCaster = new RayCaster( true, 8, *pDefShader, true );
	pCaster->AttachScene( pScene );
	PathTracingIntegrator* pIntegrator = new PathTracingIntegrator( ManifoldSolverConfig(), StabilityConfig() );

	// Analytical K_r(wi=(0,0,1), wo=(0,0,1)) * E * cos(0)
	const HyLIoSReciprocalBoundary refB( 0.25, 1.0, 1.45 );
	const double expectedKrL = refB.EvalReflection( { 0, 0, 1 }, { 0, 0, 1 } ) * 20.0;
	const Ray camRay( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) );
	{
		RandomNumberGenerator rng( 123u );
		IndependentSampler smp( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		const Scalar measured = pIntegrator->IntegrateRayNM( rc, nullRasterizerState, camRay, 550.0, *pScene, *pCaster, smp, 0 );
		Near( measured, expectedKrL, 1e-4 * expectedKrL,
			"Black-interior first-surface K_r NEE matches analytic K_r * E" );
	}

	// 1b. Scattering skin under directional light: K_t conditional exit NEE adds strictly
	//     positive subsurface radiance on top of K_r * E (continuation-only could never hit
	//     a Dirac directional light!).
	{
		const std::string scatSpec =
			"{\"schema_version\":1,\"approximate\":true,\"composition_revision\":\"scat-v1\","
			"\"coverage_profile_id\":\"explicit-synthetic-approximation\","
			"\"spectral_bundle_sha256\":\"scat_b\",\"resolved_specimen_sha256\":\"scat_s\","
			"\"preset_id\":\"S3\",\"interpretation\":\"scattering interior\","
			"\"units\":{\"length\":\"m\",\"coefficient\":\"m^-1\",\"wavelength\":\"nm\",\"mass_concentration\":\"kg/m^3\"},"
			"\"wavelength_nm\":[450,550,650],\"surface_fold_aspect_ratio\":0.75,\"provenance\":{\"retained\":\"yes\"},"
			"\"layers\":["
			"{\"layer_id\":\"stratum_corneum\",\"thickness_m\":0.00002,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[10,10,10]},\"particles\":null},"
			"{\"layer_id\":\"stratum_granulosum\",\"thickness_m\":0.00003,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[10,10,10]},\"particles\":null},"
			"{\"layer_id\":\"stratum_spinosum\",\"thickness_m\":0.00005,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[10,10,10]},\"particles\":null},"
			"{\"layer_id\":\"stratum_basale\",\"thickness_m\":0.00002,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[10,10,10]},\"particles\":null},"
			"{\"layer_id\":\"papillary_dermis\",\"thickness_m\":0.0002,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.38,1.38,1.38],\"constituent_absorption_m_inv\":{\"s\":[10,10,10]},\"particles\":null},"
			"{\"layer_id\":\"reticular_dermis\",\"thickness_m\":0.0015,\"host_absorption_m_inv\":[10,10,10],\"real_refractive_index\":[1.38,1.38,1.38],\"constituent_absorption_m_inv\":{\"s\":[10,10,10]},\"particles\":null}"
			"]}\n";
		const std::filesystem::path scatPath = tmp.dir / "scat.json";
		{
			std::ofstream ofs( scatPath, std::ios::binary );
			ofs.write( scatSpec.data(), static_cast<std::streamsize>( scatSpec.size() ) );
		}
		const std::string scatPathStr = scatPath.string();

		IJob* pScatJobI = 0;
		RISE_CreateJob( &pScatJobI );
		Job* pScatJob = dynamic_cast<Job*>( pScatJobI );
		pScatJob->SetFilm( 8, 8, 1.0 );
		pScatJob->AddPinholeCamera( "cam", loc, look, up, 0.8, 0.0, 0.0, 0.0, oriZero, targetZero );
		pScatJob->AddBoxGeometry( "slab", 20.0, 20.0, 0.2 );
		HyLIoSSkinMaterialConfig scatCfg = blackCfg;
		scatCfg.specimenSourcePath = scatPathStr.c_str();
		Check( pScatJob->AddHyLIoSSkinMaterial( "skin_scat", scatCfg ), "Add scattering nonlocal material" );
		pScatJob->AddObject( "scat_obj", "slab", "skin_scat", 0, 0, RadianceMapConfig(), pos, oriZero, sca, true, true );
		pScatJob->AddPhysicalDirectionalLight( "sun", 3, wls, irr, "zero", 1.0, dir );

		IScenePriv* pScatScene = pScatJob->GetScene();
		pScatScene->GetObjects()->PrepareForRendering();
		RayCaster* pScatCaster = new RayCaster( true, 8, *pDefShader, true );
		pScatCaster->AttachScene( pScatScene );

		double sumScat = 0.0;
		const int nS = 200;
		for( int s = 0; s < nS; ++s ) {
			RandomNumberGenerator rng( 4000u + static_cast<unsigned int>( s ) );
			IndependentSampler smp( rng );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			sumScat += pIntegrator->IntegrateRayNM( rc, nullRasterizerState, camRay, 550.0, *pScatScene, *pScatCaster, smp, 0 );
		}
		const double meanScat = sumScat / nS;
		Check( meanScat > expectedKrL + 0.05,
			"Conditional K_t exit NEE under directional light adds strictly positive subsurface radiance" );
		safe_release( pScatCaster );
		safe_release( pScatJobI );
	}

	// 2. Smooth-boundary (boundary_roughness == 0) + delta light preflight refusal
	{
		IJob* pSmoothJobI = 0;
		RISE_CreateJob( &pSmoothJobI );
		Job* pSmoothJob = dynamic_cast<Job*>( pSmoothJobI );
		pSmoothJob->SetFilm( 4, 4, 1.0 );
		pSmoothJob->AddPinholeCamera( "cam", loc, look, up, 0.8, 0.0, 0.0, 0.0, oriZero, targetZero );
		pSmoothJob->AddBoxGeometry( "slab", 20.0, 20.0, 0.2 );
		HyLIoSSkinMaterialConfig smoothCfg = blackCfg;
		smoothCfg.boundaryRoughness = 0.0;
		Check( pSmoothJob->AddHyLIoSSkinMaterial( "skin_smooth", smoothCfg ), "Add smooth nonlocal material" );
		pSmoothJob->AddObject( "smooth_obj", "slab", "skin_smooth", 0, 0, RadianceMapConfig(), pos, oriZero, sca, true, true );
		pSmoothJob->AddPhysicalDirectionalLight( "sun", 3, wls, irr, "zero", 1.0, dir );

		IScenePriv* pSmoothScene = pSmoothJob->GetScene();
		PathTracingSpectralRasterizer* pPTSpec = new PathTracingSpectralRasterizer(
			pCaster, 450.0, 650.0, 32, 1, ManifoldSolverConfig(), AdaptiveSamplingConfig(), StabilityConfig(), false, true );
		Check( !pPTSpec->ValidateSceneForRasterizer( *pSmoothScene ),
			"PathTracingSpectralRasterizer refuses smooth nonlocal boundary + delta light" );
		safe_release( pPTSpec );

		HyperspectralRadiometricConfig radCfg;
		radCfg.wavelengthsNm = { 450.0, 550.0, 650.0 };
		radCfg.samplesPerPixel = 4;
		radCfg.outputPrefix = ( tmp.dir / "smooth_out" ).string();
		HyperspectralRadiometricRasterizer* pRad = new HyperspectralRadiometricRasterizer( pCaster, radCfg );
		std::vector<double> resolvedWls;
		std::string err;
		Check( !pRad->ValidateSceneAndConfig( *pSmoothScene, resolvedWls, err ) && !err.empty(),
			"HyperspectralRadiometricRasterizer refuses smooth nonlocal boundary + delta light" );
		safe_release( pRad );
		safe_release( pSmoothJobI );
	}

	safe_release( pIntegrator );
	safe_release( pCaster );
	safe_release( pDefShader );
	safe_release( pJobI );
}

void TestSlice2P2KernelGuideAndContainment()
{
	TempDirGuard tmp( "slice2_p2" );

	// Specimen with individually dispersed melanosome organelles in stratum_basale
	// (validates real P2 HyLIoSForwardTransport organelle stepping inside nonlocal mode).
	const std::string orgSpec =
		"{\"schema_version\":1,\"approximate\":true,\"composition_revision\":\"org-v1\","
		"\"coverage_profile_id\":\"explicit-synthetic-approximation\","
		"\"spectral_bundle_sha256\":\"org_b\",\"resolved_specimen_sha256\":\"org_s\","
		"\"preset_id\":\"S3\",\"interpretation\":\"organelle P2 nonlocal test\","
		"\"units\":{\"length\":\"m\",\"coefficient\":\"m^-1\",\"wavelength\":\"nm\",\"mass_concentration\":\"kg/m^3\"},"
		"\"wavelength_nm\":[450,550,650],\"surface_fold_aspect_ratio\":0.75,\"provenance\":{\"retained\":\"yes\"},"
		"\"layers\":["
		"{\"layer_id\":\"stratum_corneum\",\"thickness_m\":0.00002,\"host_absorption_m_inv\":[40,20,10],\"real_refractive_index\":[1.45,1.45,1.45],\"constituent_absorption_m_inv\":{\"s\":[40,20,10]},\"particles\":null},"
		"{\"layer_id\":\"stratum_granulosum\",\"thickness_m\":0.00003,\"host_absorption_m_inv\":[60,30,12],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[60,30,12]},\"particles\":null},"
		"{\"layer_id\":\"stratum_spinosum\",\"thickness_m\":0.00005,\"host_absorption_m_inv\":[60,30,12],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[60,30,12]},\"particles\":null},"
		"{\"layer_id\":\"stratum_basale\",\"thickness_m\":0.00002,\"host_absorption_m_inv\":[80,40,15],\"real_refractive_index\":[1.4,1.4,1.4],\"constituent_absorption_m_inv\":{\"s\":[80,40,15]},"
		"\"particles\":{\"enabled\":true,\"organization\":\"individually_dispersed_particles\",\"organelle_fraction\":0.05,\"semi_minor_m\":1.5e-7,\"semi_major_m\":3.0e-7,\"real_refractive_index\":1.7,\"interior_absorption_evaluated\":true,\"interior_absorption_m_inv\":[4.0e5,1.5e5,4.0e4]}},"
		"{\"layer_id\":\"papillary_dermis\",\"thickness_m\":0.0002,\"host_absorption_m_inv\":[100,50,15],\"real_refractive_index\":[1.38,1.38,1.38],\"constituent_absorption_m_inv\":{\"s\":[100,50,15]},\"particles\":null},"
		"{\"layer_id\":\"reticular_dermis\",\"thickness_m\":0.0015,\"host_absorption_m_inv\":[100,50,15],\"real_refractive_index\":[1.38,1.38,1.38],\"constituent_absorption_m_inv\":{\"s\":[100,50,15]},\"particles\":null}"
		"]}\n";
	const std::filesystem::path orgPath = tmp.dir / "org.json";
	{
		std::ofstream ofs( orgPath, std::ios::binary );
		ofs.write( orgSpec.data(), static_cast<std::streamsize>( orgSpec.size() ) );
	}
	const std::string orgPathStr = orgPath.string();

	// 1. Nonlocal mode rejects non-empty response_asset
	{
		HyLIoSSkinMaterialConfig badCfg;
		badCfg.transportMode = "nonlocal";
		badCfg.boundaryMode = "rise_reciprocal_boundary_v1";
		badCfg.specimenSourcePath = orgPathStr.c_str();
		badCfg.responseAssetPath = "should_be_rejected.hylios_local";
		IMaterial* pBad = 0;
		Check( !RISE_API_CreateHyLIoSSkinMaterial( &pBad, badCfg ) && pBad == 0,
			"Nonlocal mode rejects non-empty response_asset" );
	}

	// 2. Cached 1D MC visible guide: deterministic, sensitive to exterior_ior, positive with organelles
	HyLIoSSkinMaterialConfig cfgAir;
	cfgAir.transportMode = "nonlocal";
	cfgAir.boundaryMode = "rise_reciprocal_boundary_v1";
	cfgAir.boundaryRoughness = 0.25;
	cfgAir.spatialDomain = "planar_polygon";
	cfgAir.domainHalfWidthMetres = 0.05;
	cfgAir.exteriorIOR = 1.0;
	cfgAir.specimenSourcePath = orgPathStr.c_str();

	IMaterial* pMatAir1 = 0;
	IMaterial* pMatAir2 = 0;
	Check( RISE_API_CreateHyLIoSSkinMaterial( &pMatAir1, cfgAir ) && pMatAir1 != 0, "Create organelle nonlocal mat 1" );
	Check( RISE_API_CreateHyLIoSSkinMaterial( &pMatAir2, cfgAir ) && pMatAir2 != 0, "Create organelle nonlocal mat 2" );
	const HyLIoSSkinMaterial* pH1 = dynamic_cast<const HyLIoSSkinMaterial*>( pMatAir1 );
	const HyLIoSSkinMaterial* pH2 = dynamic_cast<const HyLIoSSkinMaterial*>( pMatAir2 );
	Check( pH1 && pH2, "Organelle materials cast to HyLIoSSkinMaterial" );
	if( pH1 && pH2 ) {
		const RISEPel g1 = pH1->GetVisibleGuideRGB();
		const RISEPel g2 = pH2->GetVisibleGuideRGB();
		Check( g1.r > 0.01 && g1.g > 0.01 && g1.b > 0.01, "Organelle 1D MC visible guide positive" );
		Near( g1.r, g2.r, 1e-12, "Cached 1D MC visible guide deterministic R" );
		Near( g1.g, g2.g, 1e-12, "Cached 1D MC visible guide deterministic G" );
		Near( g1.b, g2.b, 1e-12, "Cached 1D MC visible guide deterministic B" );

		// Change exterior_ior to water (1.333) -> must use distinct cache key & different guide
		HyLIoSSkinMaterialConfig cfgWater = cfgAir;
		cfgWater.exteriorIOR = 1.333;
		IMaterial* pMatWater = 0;
		Check( RISE_API_CreateHyLIoSSkinMaterial( &pMatWater, cfgWater ) && pMatWater != 0, "Create water-immersed nonlocal mat" );
		const HyLIoSSkinMaterial* pHW = dynamic_cast<const HyLIoSSkinMaterial*>( pMatWater );
		if( pHW ) {
			const RISEPel gW = pHW->GetVisibleGuideRGB();
			Check( std::fabs( gW.r - g1.r ) > 1e-5 || std::fabs( gW.b - g1.b ) > 1e-5,
				"Changing exterior_ior produces distinct cached 1D MC visible guide" );
		}
		safe_release( pMatWater );

		// 3. HasSubsurfaceLeaf() == true for nonlocal HyLIoS and spatially varying MixMaterial flags PathDependence
		Check( pH1->HasSubsurfaceLeaf(), "Nonlocal HyLIoS reports HasSubsurfaceLeaf() == true" );
		struct SpatialScalarWeight : public virtual IScalarPainter, public virtual Reference {
			ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override {
				const Scalar w = ( ri.ptIntersection.x > Scalar( 0 ) ) ? Scalar( 0.75 ) : Scalar( 0.25 );
				return ScalarTriple( w, w, w );
			}
			bool IsPositionIndependent() const override { return false; }
		};
		UniformColorPainter* pWhite = new UniformColorPainter( RISEPel( 1, 1, 1 ) );
		SpatialScalarWeight* pWeight = new SpatialScalarWeight();
		LambertianMaterial* pLamb = new LambertianMaterial( *pWhite );
		const IMaterial* children[2] = { pMatAir1, pLamb };
		const IScalarPainter* weights[2] = { pWeight, pWeight };
		const IRayIntersectionModifier* mods[2] = { 0, 0 };
		MixMaterial* pMix = new MixMaterial( children, weights, mods, 2, 0 );
		Check( pMix->GetPathDependence() != PathDependence::kNone,
			"MixMaterial with spatially varying weight over nonlocal HyLIoS flags PathDependence" );
		safe_release( pMix );
		safe_release( pLamb );
		safe_release( pWeight );
		safe_release( pWhite );

		// 4. Direct SampleNonlocalNM with organelle-bearing specimen produces finite returns
		const Ray camRay( Point3( 0, 0, 2 ), Vector3( 0, 0, -1 ) );
		RayIntersectionGeometric ri( camRay, nullRasterizerState );
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( ri.vNormal );
		RandomNumberGenerator rng( 2026u );
		IndependentSampler smp( rng );
		int escapedSubsurface = 0;
		for( int s = 0; s < 200; ++s ) {
			HyLIoSNonlocalSample nl;
			if( pH1->SampleNonlocalNM( ri, 650.0, smp, nl ) && nl.escaped ) {
				Check( std::isfinite( nl.throughput ) && nl.throughput > 0.0, "Organelle nonlocal throughput finite positive" );
				if( !nl.isExternalReflection ) {
					++escapedSubsurface;
				}
			}
		}
		Check( escapedSubsurface > 0, "Organelle-bearing P2 nonlocal walk produces subsurface returns" );
	}
	safe_release( pMatAir2 );
	safe_release( pMatAir1 );
}

} // namespace

int main()
{
	TestSpatialDomains();
	TestNonlocalMaterialCSTAndIterativePT();
	TestNonlocalNEEAndSmoothBoundaryRefusal();
	TestSlice2P2KernelGuideAndContainment();

	if( gFailures == 0 ) {
		std::cout << "HyLIoSSpatialTest: ALL PASSED\n";
		return 0;
	}
	std::cerr << "HyLIoSSpatialTest: " << gFailures << " FAILURES\n";
	return 1;
}
