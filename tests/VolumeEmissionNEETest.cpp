//////////////////////////////////////////////////////////////////////
//
//  VolumeEmissionNEETest.cpp - Phase-B gate 2 numeric regressions.
//
//  Gates the shared visible-band quadrature, exact emission-bin ownership,
//  support-safe two-level CDF, labeled multi-medium density, and the
//  standalone spectral volume-NEE estimator before MIS activation.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Utilities/Color/RGBSpectra.h"
#include "../src/Library/Utilities/GaussLegendreQuadrature.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/PlanckRadiance.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

namespace
{
	int passed = 0;
	int failed = 0;
	unsigned int fixtureSerial = 0;

	void Check( const bool condition, const char* label )
	{
		if( condition ) ++passed;
		else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	void CheckRelative(
		const Scalar actual,
		const Scalar expected,
		const Scalar tolerance,
		const char* label
		)
	{
		const Scalar scale = std::fmax( std::fabs(expected), Scalar(1e-300) );
		const bool ok = std::fabs(actual - expected) <= tolerance * scale;
		Check( ok, label );
		if( !ok ) std::cout << "  got " << actual << ", expected " << expected << std::endl;
	}

	class IsolatedCarbonPainter :
		public virtual IScalarPainter,
		public virtual Reference
	{
	public:
		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			const Scalar tolerance = 1e-12;
			return ScalarTriple(
				std::fabs(ri.ptIntersection.x - 0.125) < tolerance &&
				std::fabs(ri.ptIntersection.y - 0.125) < tolerance &&
				std::fabs(ri.ptIntersection.z - 0.125) < tolerance ? 1.0 : 0.0 );
		}
		bool HasPerChannelVariation() const override { return false; }
	protected:
		~IsolatedCarbonPainter() override = default;
	};

	class UnitBSDF :
		public virtual IBSDF,
		public virtual Reference
	{
	public:
		RISEPel value( const Vector3&, const RayIntersectionGeometric& ) const override
		{
			return RISEPel( 1, 1, 1 );
		}
		Scalar valueNM(
			const Vector3&,
			const RayIntersectionGeometric&,
			const Scalar
			) const override { return 1.0; }
	protected:
		~UnitBSDF() override = default;
	};

	class FixedSampler : public ISampler
	{
	public:
		explicit FixedSampler( const std::vector<Scalar>& values_ ) :
		  values( values_ ), index( 0 )
		{}
		Scalar Get1D() override
		{
			const Scalar value = values[index % values.size()];
			++index;
			return value;
		}
		Point2 Get2D() override { return Point2( Get1D(), Get1D() ); }
	private:
		std::vector<Scalar> values;
		unsigned int index;
	};

	std::filesystem::path WriteScene( const std::string& text )
	{
		std::ostringstream name;
		name << "rise_volume_nee_" << static_cast<int>(::getpid()) <<
			"_" << fixtureSerial++ << ".RISEscene";
		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / name.str();
		std::ofstream output( path );
		output << text;
		return path;
	}

	IJobPriv* LoadScene( const std::string& text )
	{
		const std::filesystem::path path = WriteScene( text );
		IJobPriv* job = nullptr;
		const bool ok = RISE_CreateJobPriv( &job ) && job &&
			job->LoadAsciiSceneViaCst( path.string().c_str() );
		std::filesystem::remove( path );
		if( !ok ) safe_release( job );
		return ok ? job : nullptr;
	}

	IMedium* CreateUniformFire(
		const Scalar carbon,
		const Scalar temperature,
		const unsigned int resolution = 4
		)
	{
		IScalarPainter* carbonPainter = nullptr;
		IScalarPainter* temperaturePainter = nullptr;
		IMedium* medium = nullptr;
		const bool ok = RISE_API_CreateUniformScalarPainter( &carbonPainter, carbon ) &&
			RISE_API_CreateUniformScalarPainter( &temperaturePainter, temperature ) &&
			RISE_API_CreateMultichannelHeterogeneousMedium(
				&medium, *carbonPainter, *temperaturePainter,
				resolution, resolution, resolution,
				Point3(0,0,0), Point3(1,1,1), 1.0,
				0.26, 1800.0, 0.1, 0.5, 8.7, 1.2, 0.6, 0.6 );
		safe_release( carbonPainter );
		safe_release( temperaturePainter );
		if( !ok ) safe_release( medium );
		return ok ? medium : nullptr;
	}

	void TestSharedVisibleBandRule()
	{
		std::cout << "TestSharedVisibleBandRule" << std::endl;
		const Scalar integral = GaussLegendre21::IntegrateVisibleBand(
			[]( const Scalar nm ) {
				return PlanckSpectralRadianceNM( nm, 1800.0 ) * 500.0 / nm;
			} );
		CheckRelative( integral, 1090.4051857, 2e-11,
			"shared GL21 rule matches the r37 soot-Planck anchor" );

		IJobPriv* job = LoadScene(
			"RISE ASCII SCENE 7\n"
			"omni_light\n{\nname key\npower 3\ncolor 0.8 0.3 0.1\nposition 0 0 0\n}\n" );
		Check( job != nullptr, "visible-band point-light fixture loads" );
		if( job ) {
			const ILightManager::LightsList& lights = job->GetScene()->GetLights()->getLights();
			Check( lights.size() == 1, "point-light fixture has one light" );
			if( lights.size() == 1 ) {
				const RGBIlluminantSpectrum spectrum =
					RGBIlluminantSpectrum::FromRGB( lights[0]->radiantExitance() );
				const Scalar expected = GaussLegendre21::IntegrateVisibleBand(
					[&spectrum]( const Scalar nm ) { return spectrum.Eval(nm); } );
				CheckRelative( lights[0]->EstimateVisibleBandPower(), expected, 1e-15,
					"positional-light band power routes through the shared GL21 implementation" );
			}
		}
		safe_release( job );
	}

	void TestCDFNormalizationSamplingAndOwnership()
	{
		std::cout << "TestCDFNormalizationSamplingAndOwnership" << std::endl;
		IMedium* medium = CreateUniformFire( 0.2, 1800.0 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( fire && fire->GetThermalEmissionImportance() > 0.0,
			"uniform fire builds a nonzero emission importance structure" );
		if( fire ) {
			unsigned int nx = 0, ny = 0, nz = 0;
			fire->GetThermalEmissionBinDimensions( nx, ny, nz );
			Scalar probabilitySum = 0.0;
			for( unsigned int z = 0; z < nz; ++z )
				for( unsigned int y = 0; y < ny; ++y )
					for( unsigned int x = 0; x < nx; ++x )
						probabilitySum += fire->GetThermalEmissionBinProbability(x,y,z);
			CheckRelative( probabilitySum, 1.0, 2e-14,
				"emission-bin probabilities normalize to one" );

			const Scalar binVolume = 1.0 / Scalar(nx*ny*nz);
			const Point3 center( 0.125, 0.125, 0.125 );
			CheckRelative( fire->ThermalEmissionPdf(center) * binVolume,
				fire->GetThermalEmissionBinProbability(0,0,0), 2e-15,
				"O(1) point pdf agrees with its bin probability" );

			const Scalar below = std::nextafter( Scalar(0.25), Scalar(0.0) );
			const Scalar above = std::nextafter( Scalar(0.25), Scalar(1.0) );
			CheckRelative( fire->ThermalEmissionPdf(Point3(below,0.125,0.125))*binVolume,
				fire->GetThermalEmissionBinProbability(0,0,0), 2e-15,
				"point below a knot plane belongs to the lower half-open bin" );
			CheckRelative( fire->ThermalEmissionPdf(Point3(0.25,0.125,0.125))*binVolume,
				fire->GetThermalEmissionBinProbability(1,0,0), 2e-15,
				"knot plane belongs to the upper half-open bin" );
			CheckRelative( fire->ThermalEmissionPdf(Point3(above,0.125,0.125))*binVolume,
				fire->GetThermalEmissionBinProbability(1,0,0), 2e-15,
				"point above a knot plane stays in the upper bin" );
			Check( fire->ThermalEmissionPdf(Point3(1,1,1)) > 0.0,
				"closed global upper face clamps to the final bin" );
			Check( fire->ThermalEmissionPdf(Point3(std::nextafter(1.0,2.0),1,1)) == 0.0,
				"point beyond the closed upper face is outside the medium" );

			RandomNumberGenerator rng( 0xcdf21u );
			IndependentSampler sampler( rng );
			std::vector<unsigned int> counts( nx*ny*nz, 0u );
			const unsigned int samples = 64000;
			bool samplesValid = true;
			for( unsigned int i = 0; i < samples; ++i ) {
				Point3 p;
				Scalar pdf = 0.0;
				if( !fire->SampleThermalEmission(sampler,p,pdf) ||
					std::fabs(pdf-fire->ThermalEmissionPdf(p)) > 1e-12*pdf ) {
					samplesValid = false;
					break;
				}
				const unsigned int x = std::min(
					static_cast<unsigned int>(p.x*nx), nx-1u );
				const unsigned int y = std::min(
					static_cast<unsigned int>(p.y*ny), ny-1u );
				const unsigned int z = std::min(
					static_cast<unsigned int>(p.z*nz), nz-1u );
				++counts[z*nx*ny+y*nx+x];
			}
			Check( samplesValid, "two-level samples report their exact O(1) pdf" );
			Scalar maxFrequencyError = 0.0;
			for( unsigned int z = 0; z < nz; ++z )
				for( unsigned int y = 0; y < ny; ++y )
					for( unsigned int x = 0; x < nx; ++x ) {
						const unsigned int idx = z*nx*ny+y*nx+x;
						const Scalar observed = Scalar(counts[idx])/Scalar(samples);
						const Scalar expected = fire->GetThermalEmissionBinProbability(x,y,z);
						maxFrequencyError = std::fmax(maxFrequencyError,std::fabs(observed-expected));
					}
			Check( maxFrequencyError < 0.003,
				"two-level sampled frequencies match the reported bin pmf" );
		}
		safe_release( medium );
	}

	void TestSupportInflationAtBoundary()
	{
		std::cout << "TestSupportInflationAtBoundary" << std::endl;
		IsolatedCarbonPainter* carbon = new IsolatedCarbonPainter();
		IScalarPainter* temperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &temperature, 1800.0 );
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature, 4, 4, 4,
			Point3(0,0,0), Point3(1,1,1), 1.0,
			0.26, 1800.0, 0.1, 0.5, 8.7, 1.2, 0.6, 0.6 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( created && fire, "isolated boundary-lattice fire fixture builds" );
		if( fire ) {
			bool completeSupport = true;
			for( unsigned int z = 0; z <= 1; ++z )
				for( unsigned int y = 0; y <= 1; ++y )
					for( unsigned int x = 0; x <= 1; ++x )
						completeSupport = completeSupport &&
							fire->GetThermalEmissionBinProbability(x,y,z) > 0.0;
			Check( completeSupport,
				"support component covers every boundary bin touched by isolated trilinear emission" );
			Check( fire->GetThermalEmissionBinProbability(2,0,0) == 0.0,
				"support component does not inflate beyond the trilinear stencil" );
		}
		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	std::string TwoMediumScene()
	{
		return
			"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
			"scalar_painter\n{\nname carbon_a\nvalue 0.1\n}\n"
			"scalar_painter\n{\nname carbon_b\nvalue 0.3\n}\n"
			"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire_a\nchannel_carbon painter carbon_a\n"
			"channel_temperature painter temperature\nbake_resolution 4 4 4\n"
			"bbox_min -3 -1 -1\nbbox_max -1 1 1\nsoot_em 0.26\nsoot_density 1800\n"
			"soot_albedo_hot 0.1\nsoot_g_hot 0.5\nsmoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire_b\nchannel_carbon painter carbon_b\n"
			"channel_temperature painter temperature\nbake_resolution 4 4 4\n"
			"bbox_min 1 -1 -1\nbbox_max 3 1 1\nsoot_em 0.26\nsoot_density 1800\n"
			"soot_albedo_hot 0.1\nsoot_g_hot 0.5\nsmoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"box_geometry\n{\nname box_geom\nwidth 2\nheight 2\ndepth 2\n}\n"
			"standard_object\n{\nname box_a\ngeometry box_geom\nmaterial none\nposition -2 0 0\n"
			"interior_medium fire_a\ncasts_shadows FALSE\n}\n"
			"standard_object\n{\nname box_b\ngeometry box_geom\nmaterial none\nposition 2 0 0\n"
			"interior_medium fire_b\ncasts_shadows FALSE\n}\n";
	}

	void TestLabeledMultiMediumDensity()
	{
		std::cout << "TestLabeledMultiMediumDensity" << std::endl;
		IJobPriv* job = LoadScene( TwoMediumScene() );
		IRayCaster* caster = nullptr;
		Check( job != nullptr, "two-emissive-medium fixture loads" );
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			Check( shader && RISE_API_CreateRayCaster(&caster,false,0,*shader,true) && caster,
				"two-emissive-medium caster initializes" );
			if( caster ) caster->AttachScene( job->GetScene() );
		}
		const RayCaster* concrete = dynamic_cast<const RayCaster*>(caster);
		const LightSampler* lights = concrete ? concrete->GetLightSampler() : nullptr;
		Check( lights && lights->GetVolumeEmissionMediumCount() == 2,
			"prepared sampler deduplicates two labeled emissive media" );
		if( lights ) {
			RandomNumberGenerator rng( 0x1abe1u );
			IndependentSampler sampler( rng );
			const unsigned int samples = 40000;
			const IMedium* labels[2] = { nullptr, nullptr };
			unsigned int counts[2] = { 0, 0 };
			bool densitiesAgree = true;
			for( unsigned int i = 0; i < samples; ++i ) {
				VolumeEmissionSample sample;
				if( !lights->SampleVolumeEmission(sampler,sample) ) {
					densitiesAgree = false;
					break;
				}
				unsigned int label = 0;
				if( labels[0] && labels[0] != sample.pMedium ) label = 1;
				if( !labels[label] ) labels[label] = sample.pMedium;
				++counts[label];
				const Scalar evaluated = lights->VolumeEmissionPdf(
					*sample.pMedium, sample.point );
				if( std::fabs(evaluated-sample.pdf) > 2e-14*sample.pdf ||
					std::fabs(sample.pdf-sample.mediumSelectionPdf*sample.pointPdf) >
					2e-14*sample.pdf ) densitiesAgree = false;
			}
			Check( densitiesAgree,
				"every endpoint carries q_m^V times its per-medium point density" );
			if( labels[0] && labels[1] ) {
				const Scalar w0 = labels[0]->GetThermalEmissionImportance();
				const Scalar w1 = labels[1]->GetThermalEmissionImportance();
				const Scalar expected0 = w0/(w0+w1);
				const Scalar observed0 = Scalar(counts[0])/Scalar(samples);
				Check( std::fabs(observed0-expected0) < 0.012,
					"labeled medium frequency follows W_m over the cross-medium sum" );
			}
		}
		safe_release( caster );
		safe_release( job );
	}

	std::string GlobalMediumScene()
	{
		return
			"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
			"scalar_painter\n{\nname carbon\nvalue 1e-12\n}\n"
			"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire\nchannel_carbon painter carbon\n"
			"channel_temperature painter temperature\nbake_resolution 4 4 4\n"
			"bbox_min -1 -1 -1\nbbox_max 1 1 1\nsoot_em 0.26\nsoot_density 1800\n"
			"soot_albedo_hot 0.1\nsoot_g_hot 0.5\nsmoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"global_medium\n{\nmedium fire\n}\n";
	}

	void TestStandaloneEstimatorFormula()
	{
		std::cout << "TestStandaloneEstimatorFormula" << std::endl;
		IJobPriv* job = LoadScene( GlobalMediumScene() );
		IRayCaster* caster = nullptr;
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(&caster,false,0,*shader,true) && caster )
				caster->AttachScene( job->GetScene() );
		}
		const RayCaster* concrete = dynamic_cast<const RayCaster*>(caster);
		const LightSampler* lights = concrete ? concrete->GetLightSampler() : nullptr;
		Check( lights && lights->GetVolumeEmissionMediumCount() == 1,
			"global-fire estimator fixture prepares one emitter" );
		if( lights && job && caster ) {
			const std::vector<Scalar> randomValues = {
				0.21, 0.63, 0.42, 0.31, 0.57, 0.83
			};
			FixedSampler endpointSampler( randomValues );
			VolumeEmissionSample endpoint;
			const bool sampled = lights->SampleVolumeEmission(endpointSampler,endpoint);
			Check( sampled, "standalone estimator endpoint draw succeeds" );
			if( sampled ) {
				const Scalar nm = 550.0;
				const RasterizerState rast = {0,0};
				const Ray viewRay( Point3(0,0,-1), Vector3(0,0,-1) );
				RayIntersectionGeometric ri( viewRay, rast );
				ri.ptIntersection = Point3(0,0,-1);
				ri.vNormal = Vector3(0,0,1);
				Vector3 direction = Vector3Ops::mkVector3(endpoint.point,ri.ptIntersection);
				const Scalar distance = Vector3Ops::NormalizeMag(direction);
				Scalar tr = 0.0;
				const bool walked = EvaluateShadowMediumTransmittanceNM(
					Ray(ri.ptIntersection,direction), distance,
					job->GetScene()->GetGlobalMedium(), nullptr, job->GetScene(),
					false, nm, tr, nullptr );
				const Scalar expected = Vector3Ops::Dot(direction,ri.vNormal) * tr *
					endpoint.pMedium->GetThermalEmissionNM(endpoint.point,nm) /
					(distance*distance*endpoint.pdf);
				UnitBSDF* unit = new UnitBSDF();
				FixedSampler estimatorSampler( randomValues );
				const Scalar actual = lights->EvaluateVolumeDirectLightingNM(
					ri, *unit, nm, *caster, estimatorSampler, nullptr,
					job->GetScene()->GetGlobalMedium(), false, nullptr, nullptr );
				Check( walked, "standalone estimator uses the complete shadow-medium walk" );
				CheckRelative( actual, expected, 2e-14,
					"surface volume NEE equals f cos Tr epsilon over r^2 labeled-pdf" );

				FixedSampler volumeEstimatorSampler( randomValues );
				const Scalar volumeActual = lights->EvaluateVolumeDirectLightingNM(
					ri, *unit, nm, *caster, volumeEstimatorSampler, nullptr,
					job->GetScene()->GetGlobalMedium(), true, nullptr, nullptr );
				const Scalar volumeExpected = expected /
					Vector3Ops::Dot(direction,ri.vNormal);
				CheckRelative( volumeActual, volumeExpected, 2e-14,
					"medium volume NEE uses phase response without a second sigma_s or cosine" );
				safe_release( unit );
			}
		}
		safe_release( caster );
		safe_release( job );
	}
}

int main()
{
	std::cout << "Volume Emission NEE Phase-B Gate 2" << std::endl;
	TestSharedVisibleBandRule();
	TestCDFNormalizationSamplingAndOwnership();
	TestSupportInflationAtBoundary();
	TestLabeledMultiMediumDensity();
	TestStandaloneEstimatorFormula();
	std::cout << "Passed: " << passed << " Failed: " << failed << std::endl;
	return failed == 0 ? 0 : 1;
}
