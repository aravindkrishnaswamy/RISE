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
#include <iterator>
#include <limits>
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
#include "../src/Library/Utilities/EquiangularSampler.h"
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

	std::filesystem::path FindRepoRoot()
	{
		const char* candidates[] = { ".", "..", "../..", "../../.." };
		for( const char* candidate : candidates ) {
			const std::filesystem::path root( candidate );
			if( std::filesystem::exists(root/"docs"/"FIRE_SMOKE_DESIGN.md") &&
				std::filesystem::exists(root/"src"/"Library"/"Utilities"/
					"GaussLegendreQuadrature.cpp") ) return root;
		}
		return std::filesystem::path();
	}

	std::string ReadFile( const std::filesystem::path& path )
	{
		std::ifstream input( path, std::ios::binary );
		return std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>() );
	}

	unsigned int CountOccurrences(
		const std::string& text,
		const std::string& needle
		)
	{
		unsigned int count = 0;
		std::string::size_type at = 0;
		while( (at=text.find(needle,at)) != std::string::npos ) {
			++count;
			at += needle.size();
		}
		return count;
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

	class RampCarbonPainter :
		public virtual IScalarPainter,
		public virtual Reference
	{
	public:
		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple( 0.01 + 3.0*ri.ptIntersection.x*ri.ptIntersection.x );
		}
		bool HasPerChannelVariation() const override { return false; }
	protected:
		~RampCarbonPainter() override = default;
	};

	class ExtremeContrastCarbonPainter :
		public virtual IScalarPainter,
		public virtual Reference
	{
	public:
		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple( ri.ptCoord.x < 0.5 ? 1.0e-250 : 1.0 );
		}
		bool HasPerChannelVariation() const override { return false; }
	protected:
		~ExtremeContrastCarbonPainter() override = default;
	};

	class ProbabilityUnderflowCarbonPainter :
		public virtual IScalarPainter,
		public virtual Reference
	{
	public:
		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple( ri.ptCoord.x < 0.5 ? 1.0e-200 : 1.0e200 );
		}
		bool HasPerChannelVariation() const override { return false; }
	protected:
		~ProbabilityUnderflowCarbonPainter() override = default;
	};

	class OffsetStepCarbonPainter :
		public virtual IScalarPainter,
		public virtual Reference
	{
	public:
		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple( ri.ptCoord.x < 1.0e16+8.0 ? 0.1 : 0.4 );
		}
		bool HasPerChannelVariation() const override { return false; }
	protected:
		~OffsetStepCarbonPainter() override = default;
	};

	class SpectralResponseBSDF :
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
			const Scalar nm
			) const override { return 0.25 + 0.001*nm; }
	protected:
		~SpectralResponseBSDF() override = default;
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
		unsigned int Consumed() const { return index; }
	private:
		std::vector<Scalar> values;
		unsigned int index;
	};

	class RecordingSampler : public ISampler
	{
	public:
		explicit RecordingSampler( const unsigned int seed ) :
		  rng( seed ), sampler( rng )
		{}
		Scalar Get1D() override
		{
			const Scalar value = sampler.Get1D();
			values.push_back( value );
			return value;
		}
		Point2 Get2D() override { return Point2( Get1D(), Get1D() ); }
		const std::vector<Scalar>& Values() const { return values; }
	private:
		RandomNumberGenerator rng;
		IndependentSampler sampler;
		std::vector<Scalar> values;
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
		const unsigned int resolution = 4,
		const Point3& bboxMin = Point3(0,0,0),
		const Point3& bboxMax = Point3(1,1,1),
		const Scalar sceneUnitMeters = 1.0
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
				bboxMin, bboxMax, sceneUnitMeters,
				0.26, 1800.0, 0.1, 0.5, 8.7, 1.2, 0.6, 0.6 );
		safe_release( carbonPainter );
		safe_release( temperaturePainter );
		if( !ok ) safe_release( medium );
		return ok ? medium : nullptr;
	}

	void TestSharedQuadratureStructure()
	{
		std::cout << "TestSharedQuadratureStructure" << std::endl;
		const std::filesystem::path root = FindRepoRoot();
		Check( !root.empty(), "repository root found for shared-rule source gate" );
		if( root.empty() ) return;

		unsigned int nodeAnchorCount = 0;
		unsigned int weightAnchorCount = 0;
		unsigned int nodeDefinitionCount = 0;
		unsigned int weightDefinitionCount = 0;
		const std::filesystem::path library = root/"src"/"Library";
		for( const auto& entry : std::filesystem::recursive_directory_iterator(library) ) {
			if( !entry.is_regular_file() ) continue;
			const std::filesystem::path& path = entry.path();
			if( path.extension() != ".cpp" && path.extension() != ".h" ) continue;
			const std::string source = ReadFile( path );
			nodeAnchorCount += CountOccurrences(source,"0.0031239146898053");
			weightAnchorCount += CountOccurrences(source,"0.0080086141288871");
			nodeDefinitionCount += CountOccurrences(source,
				"const Scalar GaussLegendre21::Nodes");
			weightDefinitionCount += CountOccurrences(source,
				"const Scalar GaussLegendre21::Weights");
		}
		// The symmetric endpoint weight appears at both ends of the one table.
		Check( nodeAnchorCount == 1 && weightAnchorCount == 2 &&
			nodeDefinitionCount == 1 && weightDefinitionCount == 1,
			"GL21 nodes and weights have one shared definition and no copied table" );

		const std::string mediumSource = ReadFile(
			library/"Materials"/"HeterogeneousMedium.cpp" );
		const std::string lightSource = ReadFile(
			library/"Interfaces"/"ILight.h" );
		Check( CountOccurrences(mediumSource,
			"GaussLegendre21::IntegrateVisibleBand(") == 1,
			"thermal-emission importance calls the shared visible-band integrator" );
		Check( CountOccurrences(lightSource,
			"GaussLegendre21::IntegrateVisibleBand(") == 1,
			"positional-light band power calls the shared visible-band integrator" );
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

		IMedium* medium = CreateUniformFire( 0.2, 1800.0 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( fire != nullptr, "shared-rule medium fixture builds" );
		if( fire ) {
			const Scalar spatialNodes[2] = {
				0.5*(1.0-1.0/std::sqrt(3.0)),
				0.5*(1.0+1.0/std::sqrt(3.0))
			};
			Scalar proposal = 0.0;
			for( unsigned int z = 0; z < 4; ++z )
				for( unsigned int y = 0; y < 4; ++y )
					for( unsigned int x = 0; x < 4; ++x )
						for( unsigned int gz = 0; gz < 2; ++gz )
							for( unsigned int gy = 0; gy < 2; ++gy )
								for( unsigned int gx = 0; gx < 2; ++gx ) {
									const Point3 samplePoint(
										(Scalar(x)+spatialNodes[gx])/4.0,
										(Scalar(y)+spatialNodes[gy])/4.0,
										(Scalar(z)+spatialNodes[gz])/4.0 );
									proposal += GaussLegendre21::IntegrateVisibleBand(
										[fire,&samplePoint]( const Scalar nm ) {
											return fire->GetThermalEmissionNM( samplePoint, nm );
										} ) / (64.0*8.0);
								}
			const Scalar hotAbsorption633 =
				6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
			const Scalar upper = 400.0 * 0.2 * hotAbsorption633 *
				(633.0/380.0) * PlanckSpectralRadianceNM(780.0,1800.0);
			const Scalar expectedW = (1.0-1.0/1024.0)*proposal +
				(1.0/1024.0)*upper;
			CheckRelative( fire->GetThermalEmissionImportance(), expectedW, 3e-14,
				"W_m proposal uses the same shared GL21 band integral" );
		}
		safe_release( medium );
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
			Check( fire->ThermalEmissionPdf(Point3(
				std::numeric_limits<Scalar>::quiet_NaN(),0,0)) == 0.0,
				"non-finite endpoint coordinates have zero density" );

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

	void TestNonuniformTwoLevelDistribution()
	{
		std::cout << "TestNonuniformTwoLevelDistribution" << std::endl;
		RampCarbonPainter* carbon = new RampCarbonPainter();
		IScalarPainter* temperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &temperature, 1800.0 );
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature, 8, 8, 8,
			Point3(0,0,0), Point3(1,1,1), 1.0,
			0.26, 1800.0, 0.1, 0.5, 8.7, 1.2, 0.6, 0.6 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( created && fire, "nonuniform 8-cubed two-level fixture builds" );
		if( fire ) {
			const Scalar spatialNodes[2] = {
				0.5*(1.0-1.0/std::sqrt(3.0)),
				0.5*(1.0+1.0/std::sqrt(3.0))
			};
			const Scalar binVolume = 1.0/512.0;
			const Scalar hotAbsorption633 =
				6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
			const Scalar commonUpperFactor = binVolume * 400.0 *
				hotAbsorption633 * (633.0/380.0) *
				PlanckSpectralRadianceNM(780.0,1800.0);
			std::vector<Scalar> expectedWeights( 512, 0.0 );
			Scalar expectedTotal = 0.0;
			for( unsigned int z = 0; z < 8; ++z )
				for( unsigned int y = 0; y < 8; ++y )
					for( unsigned int x = 0; x < 8; ++x ) {
						Scalar proposalAverage = 0.0;
						for( unsigned int gz = 0; gz < 2; ++gz )
							for( unsigned int gy = 0; gy < 2; ++gy )
								for( unsigned int gx = 0; gx < 2; ++gx ) {
									const Point3 samplePoint(
										(Scalar(x)+spatialNodes[gx])/8.0,
										(Scalar(y)+spatialNodes[gy])/8.0,
										(Scalar(z)+spatialNodes[gz])/8.0 );
									proposalAverage += GaussLegendre21::IntegrateVisibleBand(
										[fire,&samplePoint]( const Scalar nm ) {
											return fire->GetThermalEmissionNM(samplePoint,nm);
										} ) / 8.0;
								}
						const unsigned int maxLatticeX = std::min(x+1u,7u);
						const Scalar centerX = (Scalar(maxLatticeX)+0.5)/8.0;
						const Scalar maxCarbon = 0.01 + 3.0*centerX*centerX;
						const Scalar proposal = binVolume*proposalAverage;
						const Scalar upper = commonUpperFactor*maxCarbon;
						const Scalar weight = (1.0-1.0/1024.0)*proposal +
							(1.0/1024.0)*upper;
						const unsigned int index = z*64u+y*8u+x;
						expectedWeights[index] = weight;
						expectedTotal += weight;
					}

			bool storedProbabilitiesAgree = true;
			for( unsigned int z = 0; z < 8; ++z )
				for( unsigned int y = 0; y < 8; ++y )
					for( unsigned int x = 0; x < 8; ++x ) {
						const Scalar expected = expectedWeights[z*64u+y*8u+x]/expectedTotal;
						const Scalar actual = fire->GetThermalEmissionBinProbability(x,y,z);
						storedProbabilitiesAgree = storedProbabilitiesAgree &&
							std::fabs(actual-expected) <= 4e-14*expected;
					}
			Check( storedProbabilitiesAgree,
				"nonuniform bin pmf matches independent GL21 plus support construction" );

			RandomNumberGenerator rng( 0x2c0ffeeu );
			IndependentSampler sampler( rng );
			const unsigned int sampleCount = 200000;
			std::vector<unsigned int> counts(512,0u);
			bool sampledPdfsAgree = true;
			for( unsigned int i = 0; i < sampleCount; ++i ) {
				Point3 point;
				Scalar pdf = 0.0;
				if( !fire->SampleThermalEmission(sampler,point,pdf) ||
					std::fabs(pdf-fire->ThermalEmissionPdf(point)) > 2e-14*pdf ) {
					sampledPdfsAgree = false;
					break;
				}
				const unsigned int x = std::min(static_cast<unsigned int>(point.x*8),7u);
				const unsigned int y = std::min(static_cast<unsigned int>(point.y*8),7u);
				const unsigned int z = std::min(static_cast<unsigned int>(point.z*8),7u);
				++counts[z*64u+y*8u+x];
			}
			Check( sampledPdfsAgree,
				"nonuniform two-level samples return the matching point density" );
			Scalar maxFrequencyError = 0.0;
			for( unsigned int i = 0; i < 512; ++i ) {
				const Scalar observed = Scalar(counts[i])/Scalar(sampleCount);
				const Scalar expected = expectedWeights[i]/expectedTotal;
				maxFrequencyError = std::fmax(maxFrequencyError,std::fabs(observed-expected));
			}
			Check( maxFrequencyError < 0.0008,
				"cell and within-cell alias draws follow the independent nonuniform pmf" );
		}
		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestSampledBoundaryOwnership()
	{
		std::cout << "TestSampledBoundaryOwnership" << std::endl;
		RampCarbonPainter* carbon = new RampCarbonPainter();
		IScalarPainter* temperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &temperature, 1800.0 );
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature, 17, 2, 2,
			Point3(0,0,0), Point3(1,1,1), 1.0,
			0.26, 1800.0, 0.1, 0.5, 8.7, 1.2, 0.6, 0.6 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( created && fire, "17-bin nonuniform boundary fixture builds" );
		bool foundThirdBoundary = false;
		bool densityMatchesSelectedBin = true;
		unsigned int selectedCellRoll = 0u;
		unsigned int selectedBinRoll = 0u;
		unsigned int selectedY = 0u;
		unsigned int selectedZ = 0u;
		if( fire ) {
			const Scalar binVolume = (1.0/17.0)*0.5*0.5;
			for( unsigned int cellRoll = 0; cellRoll < 256 && !foundThirdBoundary;
				++cellRoll ) {
				for( unsigned int binRoll = 0; binRoll < 256; ++binRoll ) {
					FixedSampler sampler( {
						(Scalar(cellRoll)+0.5)/256.0,
						(Scalar(binRoll)+0.5)/256.0,
						0.0, 0.5, 0.5 } );
					Point3 point;
					Scalar sampledPdf = 0.0;
					if( !fire->SampleThermalEmission(sampler,point,sampledPdf) ) continue;
					const unsigned int selectedX = static_cast<unsigned int>(
						std::llround(point.x*17.0) );
					if( selectedX != 3u ) continue;
					foundThirdBoundary = true;
					selectedCellRoll = cellRoll;
					selectedBinRoll = binRoll;
					selectedY = std::min(
						static_cast<unsigned int>(point.y*2.0), 1u );
					selectedZ = std::min(
						static_cast<unsigned int>(point.z*2.0), 1u );
					const Scalar expectedPdf =
						fire->GetThermalEmissionBinProbability(
							selectedX,selectedY,selectedZ ) / binVolume;
					densityMatchesSelectedBin =
						std::fabs(sampledPdf-expectedPdf) <= 2e-14*expectedPdf &&
						std::fabs(fire->ThermalEmissionPdf(point)-expectedPdf) <=
							2e-14*expectedPdf;
					break;
				}
			}
		}
		Check( foundThirdBoundary,
			"fixed-zero spatial draw reaches the lower face of nonuniform bin three" );
		Check( densityMatchesSelectedBin,
			"sampled and evaluated density retain the selected half-open bin at its lower face" );
		bool upperRoundingMatchesSelectedBin = false;
		if( fire && foundThirdBoundary ) {
			FixedSampler sampler( {
				(Scalar(selectedCellRoll)+0.5)/256.0,
				(Scalar(selectedBinRoll)+0.5)/256.0,
				std::nextafter(Scalar(1.0),Scalar(0.0)), 0.5, 0.5 } );
			Point3 point;
			Scalar sampledPdf = 0.0;
			const Scalar expectedPdf = fire->GetThermalEmissionBinProbability(
				3u,selectedY,selectedZ ) / ((1.0/17.0)*0.5*0.5);
			upperRoundingMatchesSelectedBin =
				fire->SampleThermalEmission(sampler,point,sampledPdf) &&
				point.x < 4.0/17.0 &&
				std::fabs(sampledPdf-expectedPdf) <= 2e-14*expectedPdf &&
				std::fabs(fire->ThermalEmissionPdf(point)-expectedPdf) <=
					2e-14*expectedPdf;
		}
		Check( upperRoundingMatchesSelectedBin,
			"maximal [0,1) draw stays inside its selected nonuniform bin" );
		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestSceneUnitInvariantImportance()
	{
		std::cout << "TestSceneUnitInvariantImportance" << std::endl;
		IMedium* meterMedium = CreateUniformFire(
			0.2, 1800.0, 4, Point3(0,0,0), Point3(1,1,1), 1.0 );
		IMedium* centimeterMedium = CreateUniformFire(
			0.2, 1800.0, 4, Point3(0,0,0), Point3(100,100,100), 0.01 );
		MultichannelHeterogeneousMedium* meter =
			dynamic_cast<MultichannelHeterogeneousMedium*>( meterMedium );
		MultichannelHeterogeneousMedium* centimeter =
			dynamic_cast<MultichannelHeterogeneousMedium*>( centimeterMedium );
		Check( meter && centimeter,
			"metre and centimetre importance fixtures build" );
		if( meter && centimeter ) {
			CheckRelative( centimeter->GetThermalEmissionPowerProxy(),
				meter->GetThermalEmissionPowerProxy(), 3e-14,
				"A_m=4pi s^2 W_m is invariant under metre-to-centimetre scene scaling" );
			bool probabilitiesAgree = true;
			for( unsigned int z = 0; z < 4; ++z )
				for( unsigned int y = 0; y < 4; ++y )
					for( unsigned int x = 0; x < 4; ++x )
						probabilitiesAgree = probabilitiesAgree &&
							std::fabs(meter->GetThermalEmissionBinProbability(x,y,z) -
							centimeter->GetThermalEmissionBinProbability(x,y,z)) < 3e-15;
			Check( probabilitiesAgree,
				"q_v is unchanged by metre-to-centimetre scene scaling" );
			const Point3 meterPoint(0.37,0.42,0.81);
			const Point3 centimeterPoint(37,42,81);
			CheckRelative( centimeter->ThermalEmissionPdf(centimeterPoint) * 1.0e6,
				meter->ThermalEmissionPdf(meterPoint), 3e-14,
				"per-scene-volume density transforms by the coordinate Jacobian" );
		}

		IMedium* overflowIntermediateMedium = CreateUniformFire(
			0.2, 1800.0, 4, Point3(0,0,0),
			Point3(1.0e-100,1.0e-100,1.0e-100), 1.0e155 );
		IMedium* underflowIntermediateMedium = CreateUniformFire(
			0.2, 1800.0, 4, Point3(0,0,0),
			Point3(1.0e98,1.0e98,1.0e98), 1.0e-200 );
		MultichannelHeterogeneousMedium* overflowIntermediate =
			dynamic_cast<MultichannelHeterogeneousMedium*>( overflowIntermediateMedium );
		MultichannelHeterogeneousMedium* underflowIntermediate =
			dynamic_cast<MultichannelHeterogeneousMedium*>( underflowIntermediateMedium );
		Check( overflowIntermediate && underflowIntermediate,
			"power-proxy range fixtures build" );
		if( overflowIntermediate && underflowIntermediate ) {
			const Scalar overflowExpected = std::exp(
				std::log(FOUR_PI) + 2.0*std::log(1.0e155) +
				std::log(overflowIntermediate->GetThermalEmissionImportance()) );
			const Scalar underflowExpected = std::exp(
				std::log(FOUR_PI) + 2.0*std::log(1.0e-200) +
				std::log(underflowIntermediate->GetThermalEmissionImportance()) );
			CheckRelative( overflowIntermediate->GetThermalEmissionPowerProxy(),
				overflowExpected, 2e-13,
				"A_m remains finite when the left-associated scene-scale square overflows" );
			CheckRelative( underflowIntermediate->GetThermalEmissionPowerProxy(),
				underflowExpected, 2e-13,
				"A_m remains positive when the left-associated scene-scale square underflows" );
		}
		safe_release( meterMedium );
		safe_release( centimeterMedium );
		safe_release( overflowIntermediateMedium );
		safe_release( underflowIntermediateMedium );
	}

	void TestTinyFiniteBBoxDensityFormulation()
	{
		std::cout << "TestTinyFiniteBBoxDensityFormulation" << std::endl;
		const Scalar side = 1.0e-55;
		IMedium* medium = CreateUniformFire(
			0.2, 1800.0, 4, Point3(0,0,0), Point3(side,side,side), 1.0 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( fire && fire->GetThermalEmissionImportance() > 0.0,
			"tiny finite bbox retains representable positive importance" );
		if( fire ) {
			const Scalar pointPdf = fire->ThermalEmissionPdf(
				Point3(0.125*side,0.125*side,0.125*side) );
			const Scalar binSide = side/4.0;
			const Scalar binVolume = binSide*binSide*binSide;
			Check( RISE::IsFiniteDouble(pointPdf) && pointPdf > 0.0,
				"q_v divided by V_v stays finite when W_m times V_v underflows" );
			CheckRelative( pointPdf*binVolume,
				fire->GetThermalEmissionBinProbability(0,0,0), 3e-14,
				"tiny-bbox density still equals its bin probability per volume" );
		}
		safe_release( medium );

		IMedium* unrepresentable = CreateUniformFire(
			0.2, 1800.0, 4, Point3(0,0,0),
			Point3(1.0e-106,1.0e-106,1.0e-106), 1.0 );
		Check( unrepresentable == nullptr,
			"medium construction rejects a bbox whose q_v per-volume density overflows" );
		safe_release( unrepresentable );

		IMedium* collapsedBoundary = CreateUniformFire(
			0.2, 1800.0, 2, Point3(1.0e16,0,0),
			Point3(1.0e16+2.0,1,1), 1.0 );
		Check( collapsedBoundary == nullptr,
			"medium construction rejects a bin with no representable spatial interior" );
		safe_release( collapsedBoundary );

		IMedium* adjacentBoundaries = CreateUniformFire(
			0.2, 1800.0, 2, Point3(1.0e16,0,0),
			Point3(1.0e16+4.0,1,1), 1.0 );
		Check( adjacentBoundaries == nullptr,
			"medium construction rejects adjacent boundaries with no interior value" );
		safe_release( adjacentBoundaries );

		ExtremeContrastCarbonPainter* contrast = new ExtremeContrastCarbonPainter();
		IScalarPainter* temperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &temperature, 1800.0 );
		IMedium* underflowingDensity = nullptr;
		const bool contrastCreated = RISE_API_CreateMultichannelHeterogeneousMedium(
			&underflowingDensity, *contrast, *temperature, 8, 8, 8,
			Point3(0,0,0), Point3(1e33,1e33,1e33), 1.0,
			0.26, 1800.0, 0.1, 0.5, 8.7, 1.2, 0.6, 0.6 );
		Check( !contrastCreated && underflowingDensity == nullptr,
			"positive bin support whose q_v per-volume density underflows is rejected" );
		safe_release( underflowingDensity );
		safe_release( contrast );

		ProbabilityUnderflowCarbonPainter* probabilityContrast =
			new ProbabilityUnderflowCarbonPainter();
		IMedium* underflowingProbability = nullptr;
		const bool probabilityCreated = RISE_API_CreateMultichannelHeterogeneousMedium(
			&underflowingProbability, *probabilityContrast, *temperature, 8, 8, 8,
			Point3(0,0,0), Point3(1,1,1), 1.0,
			0.26, 1800.0, 0.1, 0.5, 8.7, 1.2, 0.6, 0.6 );
		Check( !probabilityCreated && underflowingProbability == nullptr,
			"positive raw bin support whose normalized q_v underflows is rejected" );
		safe_release( underflowingProbability );
		safe_release( probabilityContrast );
		safe_release( temperature );
	}

	void TestRepresentedBinVolumeNormalization()
	{
		std::cout << "TestRepresentedBinVolumeNormalization" << std::endl;
		OffsetStepCarbonPainter* carbon = new OffsetStepCarbonPainter();
		IScalarPainter* temperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &temperature, 1800.0 );
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature, 2, 2, 2,
			Point3(1.0e16,0,0), Point3(1.0e16+14.0,1,1), 1.0,
			0.26, 1800.0, 0.1, 0.5, 8.7, 1.2, 0.6, 0.6 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( created && fire,
			"unequal represented-width emission-bin fixture builds" );
		if( fire ) {
			Scalar integral = 0.0;
			for( unsigned int z = 0; z < 2; ++z ) {
				for( unsigned int y = 0; y < 2; ++y ) {
					const Scalar py = Scalar(y)*0.5 + 0.25;
					const Scalar pz = Scalar(z)*0.5 + 0.25;
					integral += fire->ThermalEmissionPdf(
						Point3(1.0e16+2.0,py,pz) ) * (8.0*0.5*0.5);
					integral += fire->ThermalEmissionPdf(
						Point3(1.0e16+10.0,py,pz) ) * (6.0*0.5*0.5);
				}
			}
			CheckRelative( integral, 1.0, 3e-14,
				"piecewise density normalizes over unequal represented bin volumes" );
			Check( fire->GetThermalEmissionBinProbability(0,0,0) !=
				fire->GetThermalEmissionBinProbability(1,0,0),
				"offset normalization fixture has nonuniform bin probabilities" );
		}
		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestAnisotropicBinVolumeFormulation()
	{
		std::cout << "TestAnisotropicBinVolumeFormulation" << std::endl;
		IMedium* overflowOrder = CreateUniformFire(
			0.2, 1800.0, 2, Point3(0,0,0),
			Point3(1.0e200,1.0e200,1.0e-200), 1.0 );
		IMedium* underflowOrder = CreateUniformFire(
			0.2, 1800.0, 2, Point3(0,0,0),
			Point3(1.0e-200,1.0e-200,1.0e200), 1.0 );
		MultichannelHeterogeneousMedium* overflowFire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( overflowOrder );
		MultichannelHeterogeneousMedium* underflowFire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( underflowOrder );
		Check( overflowFire && underflowFire,
			"finite anisotropic bin volumes build in both multiplication orders" );
		if( overflowFire ) {
			const Point3 center(2.5e199,2.5e199,2.5e-201);
			CheckRelative( overflowFire->ThermalEmissionPdf(center) *
				(8.0*1.25e199), 1.0, 4e-14,
				"overflow-prone anisotropic represented-bin density normalizes" );
		}
		if( underflowFire ) {
			const Point3 center(2.5e-201,2.5e-201,2.5e199);
			CheckRelative( underflowFire->ThermalEmissionPdf(center) *
				(8.0*1.25e-201), 1.0, 4e-14,
				"underflow-prone anisotropic represented-bin density normalizes" );
		}
		safe_release( overflowOrder );
		safe_release( underflowOrder );
	}

	void TestPositiveSupportUnderflowRejection()
	{
		std::cout << "TestPositiveSupportUnderflowRejection" << std::endl;
		IMedium* medium = CreateUniformFire(
			1.0e-200, 1800.0, 2,
			Point3(0,0,0), Point3(1.0e-50,1.0e-50,1.0e-50), 1.0 );
		Check( medium == nullptr,
			"positive subnormal thermal support cannot silently become a zero strategy" );
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

	std::string TwoScaleMediumScene(
		const char* sideA,
		const char* sideB,
		const char* carbonA = "0.2",
		const char* carbonB = "0.2"
		)
	{
		std::ostringstream scene;
		scene <<
			"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
			"scalar_painter\n{\nname carbon_a\nvalue " << carbonA << "\n}\n"
			"scalar_painter\n{\nname carbon_b\nvalue " << carbonB << "\n}\n"
			"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire_a\nchannel_carbon painter carbon_a\n"
			"channel_temperature painter temperature\nbake_resolution 4 4 4\n"
			"bbox_min 0 0 0\nbbox_max " << sideA << " " << sideA << " " << sideA <<
			"\nsoot_em 0.26\nsoot_density 1800\nsoot_albedo_hot 0.1\nsoot_g_hot 0.5\n"
			"smoke_km_carbon 8.7\nsmoke_n_carbon 1.2\nsmoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire_b\nchannel_carbon painter carbon_b\n"
			"channel_temperature painter temperature\nbake_resolution 4 4 4\n"
			"bbox_min 0 0 0\nbbox_max " << sideB << " " << sideB << " " << sideB <<
			"\nsoot_em 0.26\nsoot_density 1800\nsoot_albedo_hot 0.1\nsoot_g_hot 0.5\n"
			"smoke_km_carbon 8.7\nsmoke_n_carbon 1.2\nsmoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"box_geometry\n{\nname box_geom\nwidth 1\nheight 1\ndepth 1\n}\n"
			"standard_object\n{\nname box_a\ngeometry box_geom\nmaterial none\nposition -2 0 0\n"
			"interior_medium fire_a\ncasts_shadows FALSE\n}\n"
			"standard_object\n{\nname box_b\ngeometry box_geom\nmaterial none\nposition 2 0 0\n"
			"interior_medium fire_b\ncasts_shadows FALSE\n}\n";
		return scene.str();
	}

	const LightSampler* PrepareLightSampler(
		IJobPriv* job,
		IRayCaster*& caster
		)
	{
		caster = nullptr;
		if( !job ) return nullptr;
		IShader* shader = job->GetShaders()->GetItem("global");
		if( !shader || !RISE_API_CreateRayCaster(&caster,false,0,*shader,true) || !caster )
			return nullptr;
		caster->AttachScene( job->GetScene() );
		const RayCaster* concrete = dynamic_cast<const RayCaster*>(caster);
		return concrete ? concrete->GetLightSampler() : nullptr;
	}

	void TestScaleSafeCrossMediumNormalization()
	{
		std::cout << "TestScaleSafeCrossMediumNormalization" << std::endl;
		IJobPriv* overflowJob = LoadScene( TwoScaleMediumScene("6e101","6e101") );
		IRayCaster* overflowCaster = nullptr;
		const LightSampler* overflowLights = PrepareLightSampler(
			overflowJob, overflowCaster );
		Check( overflowLights && overflowLights->GetVolumeEmissionMediumCount() == 2 &&
			overflowLights->IsVolumeEmissionDistributionValid(),
			"two finite W_m values remain selectable when their raw sum overflows" );
		if( overflowLights ) {
			RandomNumberGenerator rng( 0x5ca1eu );
			IndependentSampler sampler( rng );
			bool finiteHalfDensities = true;
			for( unsigned int i = 0; i < 64; ++i ) {
				VolumeEmissionSample sample;
				finiteHalfDensities = finiteHalfDensities &&
					overflowLights->SampleVolumeEmission(sampler,sample) &&
					std::fabs(sample.mediumSelectionPdf-0.5) < 2e-15 &&
					RISE::IsFiniteDouble(sample.pdf) && sample.pdf > 0.0;
			}
			Check( finiteHalfDensities,
				"max-scaled cross-medium alias preserves the finite equal-weight ratio" );
		}
		safe_release( overflowCaster );
		safe_release( overflowJob );

		IJobPriv* rangeJob = LoadScene( TwoScaleMediumScene("1e50","1e-50") );
		IRayCaster* rangeCaster = nullptr;
		const LightSampler* rangeLights = PrepareLightSampler(rangeJob,rangeCaster);
		const IObject* smallObject = rangeJob ?
			rangeJob->GetScene()->GetObjects()->GetItem("box_b") : nullptr;
		const IMedium* smallMedium = smallObject ? smallObject->GetInteriorMedium() : nullptr;
		Check( rangeLights && rangeLights->IsVolumeEmissionDistributionValid() &&
			smallMedium && rangeLights->VolumeEmissionPdf(
				*smallMedium,Point3(5e-51,5e-51,5e-51)) > 0.0,
			"a representable 1e-300 medium-selection ratio stays positive" );
		safe_release( rangeCaster );
		safe_release( rangeJob );

		IJobPriv* productJob = LoadScene(
			TwoScaleMediumScene("1e33","1e33","1","1e-250") );
		IRayCaster* productCaster = nullptr;
		const LightSampler* productLights = PrepareLightSampler(
			productJob,productCaster );
		RandomNumberGenerator productRng( 0x5ca20u );
		IndependentSampler productSampler( productRng );
		VolumeEmissionSample productSample;
		Check( productLights && !productLights->IsVolumeEmissionDistributionValid() &&
			!productLights->SampleVolumeEmission(productSampler,productSample),
			"q_m and p_m that are separately positive cannot form a zero labeled density" );
		safe_release( productCaster );
		safe_release( productJob );

		IJobPriv* rejectedJob = LoadScene( TwoScaleMediumScene("1e100","1e-100") );
		IRayCaster* rejectedCaster = nullptr;
		const LightSampler* rejectedLights = PrepareLightSampler(
			rejectedJob,rejectedCaster );
		RandomNumberGenerator rejectedRng( 0x5ca1fu );
		IndependentSampler rejectedSampler( rejectedRng );
		VolumeEmissionSample rejectedSample;
		Check( rejectedLights && !rejectedLights->IsVolumeEmissionDistributionValid() &&
			!rejectedLights->SampleVolumeEmission(rejectedSampler,rejectedSample),
			"unrepresentable positive labeled ratio fails preparation instead of becoming zero" );
		safe_release( rejectedCaster );
		safe_release( rejectedJob );
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

	std::string NestedMediumScene()
	{
		return
			"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
			"null_boundary_material\n{\nname medium_boundary\n}\n"
			"scalar_painter\n{\nname carbon_outer\nvalue 0.01\n}\n"
			"scalar_painter\n{\nname carbon_inner\nvalue 0.03\n}\n"
			"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
			"multichannel_heterogeneous_medium\n{\nname outer_fire\nchannel_carbon painter carbon_outer\n"
			"channel_temperature painter temperature\nbake_resolution 4 4 4\n"
			"bbox_min -2 -2 -2\nbbox_max 2 2 2\nsoot_em 0.26\nsoot_density 1800\n"
			"soot_albedo_hot 0.1\nsoot_g_hot 0.5\nsmoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"multichannel_heterogeneous_medium\n{\nname inner_fire\nchannel_carbon painter carbon_inner\n"
			"channel_temperature painter temperature\nbake_resolution 4 4 4\n"
			"bbox_min -0.5 -0.5 -0.5\nbbox_max 0.5 0.5 0.5\nsoot_em 0.26\nsoot_density 1800\n"
			"soot_albedo_hot 0.1\nsoot_g_hot 0.5\nsmoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"box_geometry\n{\nname outer_geom\nwidth 4\nheight 4\ndepth 4\n}\n"
			"box_geometry\n{\nname inner_geom\nwidth 1\nheight 1\ndepth 1\n}\n"
			"standard_object\n{\nname outer_box\ngeometry outer_geom\nmaterial medium_boundary\n"
			"interior_medium outer_fire\ncasts_shadows FALSE\n}\n"
			"standard_object\n{\nname inner_box\ngeometry inner_geom\nmaterial medium_boundary\n"
			"interior_medium inner_fire\ncasts_shadows FALSE\n}\n";
	}

	void TestInactiveNestedLabelReturnsZero()
	{
		std::cout << "TestInactiveNestedLabelReturnsZero" << std::endl;
		IJobPriv* job = LoadScene( NestedMediumScene() );
		IRayCaster* caster = nullptr;
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(&caster,false,0,*shader,true) && caster )
				caster->AttachScene( job->GetScene() );
		}
		const RayCaster* concrete = dynamic_cast<const RayCaster*>(caster);
		const LightSampler* lights = concrete ? concrete->GetLightSampler() : nullptr;
		const IObject* outerObject = job ?
			job->GetScene()->GetObjects()->GetItem("outer_box") : nullptr;
		const IObject* innerObject = job ?
			job->GetScene()->GetObjects()->GetItem("inner_box") : nullptr;
		const IMedium* outer = outerObject ? outerObject->GetInteriorMedium() : nullptr;
		const IMedium* inner = innerObject ? innerObject->GetInteriorMedium() : nullptr;
		Check( lights && outer && inner && lights->GetVolumeEmissionMediumCount() == 2,
			"nested fixture prepares both labeled emissive media" );
		std::vector<Scalar> inactiveReplayValues;
		std::vector<Scalar> activeReplayValues;
		Point3 inactivePoint;
		if( lights && outer && inner ) {
			for( unsigned int seed = 1; seed <= 4096 &&
				(inactiveReplayValues.empty() || activeReplayValues.empty()); ++seed ) {
				RecordingSampler sampler( seed );
				VolumeEmissionSample endpoint;
				if( lights->SampleVolumeEmission(sampler,endpoint) &&
					endpoint.pMedium == outer &&
					std::fabs(endpoint.point.x) < 0.5 &&
					std::fabs(endpoint.point.y) < 0.5 &&
					std::fabs(endpoint.point.z) < 0.5 ) {
					inactiveReplayValues = sampler.Values();
					inactivePoint = endpoint.point;
				}
				if( endpoint.pMedium == inner && activeReplayValues.empty() ) {
					activeReplayValues = sampler.Values();
				}
			}
		}
		Check( !inactiveReplayValues.empty() && outer &&
			lights->VolumeEmissionPdf(*outer,inactivePoint) > 0.0,
			"outer labeled density remains positive inside the nested medium" );
		Check( !activeReplayValues.empty(),
			"nested fixture finds an endpoint labeled with the active inner medium" );
		if( !inactiveReplayValues.empty() && !activeReplayValues.empty() &&
			lights && caster && outer && inner ) {
			const RasterizerState rast = {0,0};
			const Ray viewRay( Point3(-3,0,0), Vector3(-1,0,0) );
			RayIntersectionGeometric ri( viewRay, rast );
			ri.ptIntersection = Point3(-3,0,0);
			ri.vNormal = Vector3(1,0,0);
			SpectralResponseBSDF* unit = new SpectralResponseBSDF();
			FixedSampler sampler( inactiveReplayValues );
			const Scalar contribution = lights->EvaluateVolumeDirectLightingNM(
				ri, *unit, 550.0, sampler,
				nullptr, true, nullptr, nullptr );
			Check( contribution == 0.0,
				"outer label inside nested exact-null medium returns zero after stack resolution" );

			FixedSampler endpointSampler( activeReplayValues );
			VolumeEmissionSample endpoint;
			const bool endpointSampled = lights->SampleVolumeEmission(
				endpointSampler, endpoint );
			Vector3 direction = endpointSampled ? Vector3Ops::mkVector3(
				endpoint.point, ri.ptIntersection ) : Vector3(0,0,0);
			const Scalar distance = endpointSampled ?
				Vector3Ops::NormalizeMag(direction) : 0.0;
			const Scalar tOuter = endpointSampled ? 1.0/direction.x : 0.0;
			const Scalar tInner = endpointSampled ? 2.5/direction.x : 0.0;
			Scalar expected = 0.0;
			if( endpointSampled && endpoint.pMedium==inner &&
				distance > tInner && tInner > tOuter ) {
				const Ray outerRay(
					Ray(ri.ptIntersection,direction).PointAtLength(tOuter), direction );
				const Ray innerRay(
					Ray(ri.ptIntersection,direction).PointAtLength(tInner), direction );
				const Scalar transmittance = outer->EvalDistancePdfNM(
					outerRay,tInner-tOuter,false,tInner-tOuter,550.0) *
					inner->EvalDistancePdfNM(
						innerRay,distance-tInner,false,distance-tInner,550.0);
				expected = 0.8 * transmittance *
					inner->GetThermalEmissionNM(endpoint.point,550.0) /
					(distance*distance*endpoint.pdf);
			}
			Scalar activeContribution = 0.0;
			const unsigned int trials = 20000;
			for( unsigned int i = 0; i < trials; ++i ) {
				FixedSampler activeSampler( activeReplayValues );
				activeContribution += lights->EvaluateVolumeDirectLightingNM(
					ri, *unit, 550.0, activeSampler,
					nullptr, true, nullptr, nullptr );
			}
			activeContribution /= static_cast<Scalar>(trials);
			Check( expected > 0.0,
				"nested exact-null reference has positive piecewise attenuation" );
			CheckRelative( activeContribution, expected, 0.02,
				"nested exact-null walk applies outer then inner attenuation and active label" );
			safe_release( unit );
		}
		safe_release( caster );
		safe_release( job );
	}

	std::string GlobalMediumScene(
		const bool addBlocker = false,
		const bool castsShadows = true,
		const bool exactNullBoundary = false
		)
	{
		std::string scene =
			"RISE ASCII SCENE 7\nstandard_shader\n{\nname global\n}\n"
			"scalar_painter\n{\nname carbon\nvalue 1e-12\n}\n"
			"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire\nchannel_carbon painter carbon\n"
			"channel_temperature painter temperature\nbake_resolution 4 4 4\n"
			"bbox_min -1 -1 -1\nbbox_max 1 1 1\nsoot_em 0.26\nsoot_density 1800\n"
			"soot_albedo_hot 0.1\nsoot_g_hot 0.5\nsmoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"global_medium\n{\nmedium fire\n}\n";
		if( addBlocker ) {
			if( exactNullBoundary ) {
				scene += "null_boundary_material\n{\nname medium_boundary\n}\n";
			}
			scene +=
				"box_geometry\n{\nname blocker_geom\nwidth 4\nheight 4\ndepth 0.1\n}\n"
				"standard_object\n{\nname blocker\ngeometry blocker_geom\nmaterial ";
			scene += exactNullBoundary ? "medium_boundary\n" : "none\n";
			scene += "position 0 0 -0.5\ncasts_shadows ";
			scene += castsShadows ? "TRUE\n}\n" : "FALSE\n}\n";
		}
		return scene;
	}

	std::string ScaledMixedPivotScene(
		const Scalar sceneUnitMeters,
		const Scalar coordinateScale
		)
	{
		std::ostringstream scene;
		scene << std::setprecision(17) <<
			"RISE ASCII SCENE 7\nscene_options\n{\nscene_unit " <<
			sceneUnitMeters << "\n}\nstandard_shader\n{\nname global\n}\n"
			"scalar_painter\n{\nname carbon\nvalue 1e-12\n}\n"
			"scalar_painter\n{\nname temperature\nvalue 1800\n}\n"
			"multichannel_heterogeneous_medium\n{\nname fire\n"
			"channel_carbon painter carbon\nchannel_temperature painter temperature\n"
			"bake_resolution 4 4 4\nbbox_min " << -coordinateScale << " " <<
			-coordinateScale << " " << -coordinateScale << "\nbbox_max " <<
			coordinateScale << " " << coordinateScale << " " << coordinateScale <<
			"\nsoot_em 0.26\nsoot_density 1800\nsoot_albedo_hot 0.1\n"
			"soot_g_hot 0.5\nsmoke_km_carbon 8.7\nsmoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\nsmoke_g_carbon 0.6\n}\n"
			"global_medium\n{\nmedium fire\n}\nomni_light\n{\nname key\n"
			"power 3\ncolor 0.8 0.3 0.1\nposition " << 2.0*coordinateScale <<
			" 0 0\n}\n";
		return scene.str();
	}

	void TestCombinedPivotSceneUnitInvariance()
	{
		std::cout << "TestCombinedPivotSceneUnitInvariance" << std::endl;
		IJobPriv* meterJob = LoadScene( ScaledMixedPivotScene(1.0,1.0) );
		IJobPriv* centimeterJob = LoadScene( ScaledMixedPivotScene(0.01,100.0) );
		IRayCaster* meterCaster = nullptr;
		IRayCaster* centimeterCaster = nullptr;
		if( meterJob ) {
			IShader* shader = meterJob->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(
				&meterCaster,false,0,*shader,true) && meterCaster ) {
				meterCaster->AttachScene( meterJob->GetScene() );
			}
		}
		if( centimeterJob ) {
			IShader* shader = centimeterJob->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(
				&centimeterCaster,false,0,*shader,true) && centimeterCaster ) {
				centimeterCaster->AttachScene( centimeterJob->GetScene() );
			}
		}
		const RayCaster* meterConcrete =
			dynamic_cast<const RayCaster*>(meterCaster);
		const RayCaster* centimeterConcrete =
			dynamic_cast<const RayCaster*>(centimeterCaster);
		const LightSampler* meter = meterConcrete ?
			meterConcrete->GetLightSampler() : nullptr;
		const LightSampler* centimeter = centimeterConcrete ?
			centimeterConcrete->GetLightSampler() : nullptr;
		Check( meter && centimeter && meter->GetEquiangularPivotEntryCount()==2 &&
			centimeter->GetEquiangularPivotEntryCount()==2,
			"mixed metre/centimetre pivot fixtures prepare the same two labels" );
		if( meter && centimeter && meter->GetEquiangularPivotEntryCount()==2 &&
			centimeter->GetEquiangularPivotEntryCount()==2 ) {
			for( unsigned int i = 0; i < 2; ++i ) {
				CheckRelative( centimeter->GetEquiangularPivotSelectionPdf(i),
					meter->GetEquiangularPivotSelectionPdf(i), 3e-14,
					"mixed light/medium pivot PMF is scene-unit invariant" );
			}

			const std::vector<Scalar> draws = {
				0.11,0.23,0.31,0.41,0.53,0.67,0.73,0.79 };
			FixedSampler meterSampler(draws);
			FixedSampler centimeterSampler(draws);
			VolumeEmissionPivotState meterPivots;
			VolumeEmissionPivotState centimeterPivots;
			const bool sampledPivots =
				meter->SampleVolumeEmissionPivots(meterSampler,meterPivots) &&
				centimeter->SampleVolumeEmissionPivots(
					centimeterSampler,centimeterPivots);
			Check( sampledPivots && meterPivots.mediumPivots.size()==1 &&
				centimeterPivots.mediumPivots.size()==1,
				"paired scene-unit fixtures retain one shared-U medium pivot" );
			if( sampledPivots ) {
				const Point3& meterMediumPivot = meterPivots.mediumPivots[0];
				const Point3& centimeterMediumPivot = centimeterPivots.mediumPivots[0];
				CheckRelative( centimeterMediumPivot.x, 100.0*meterMediumPivot.x,
					3e-14, "medium pivot x coordinate scales by 100" );
				CheckRelative( centimeterMediumPivot.y, 100.0*meterMediumPivot.y,
					3e-14, "medium pivot y coordinate scales by 100" );
				CheckRelative( centimeterMediumPivot.z, 100.0*meterMediumPivot.z,
					3e-14, "medium pivot z coordinate scales by 100" );

				Point3 meterSelected;
				Point3 centimeterSelected;
				Scalar meterSelectionPdf = 0.0;
				Scalar centimeterSelectionPdf = 0.0;
				const Scalar entryXi = 0.73;
				const bool selected = meter->SampleEquiangularPivot(
					meterPivots,entryXi,meterSelected,meterSelectionPdf) &&
					centimeter->SampleEquiangularPivot(
						centimeterPivots,entryXi,centimeterSelected,
						centimeterSelectionPdf);
				Check( selected, "paired scene-unit fixtures select the same pivot label" );
				if( selected ) {
					CheckRelative( centimeterSelectionPdf,meterSelectionPdf,3e-14,
						"selected pivot label has the same probability" );
					CheckRelative( centimeterSelected.x,100.0*meterSelected.x,3e-14,
						"selected pivot x coordinate scales by 100" );
					CheckRelative( centimeterSelected.y,100.0*meterSelected.y,3e-14,
						"selected pivot y coordinate scales by 100" );
					CheckRelative( centimeterSelected.z,100.0*meterSelected.z,3e-14,
						"selected pivot z coordinate scales by 100" );
					const Ray meterRay(Point3(0,0,-3),Vector3(0,0,1));
					const Ray centimeterRay(Point3(0,0,-300),Vector3(0,0,1));
					const EquiangularSampling::Sample meterSample =
						EquiangularSampling::SampleDistance(
							meterRay,meterSelected,0.0,600.0/100.0,true,0.37);
					const EquiangularSampling::Sample centimeterSample =
						EquiangularSampling::SampleDistance(
							centimeterRay,centimeterSelected,0.0,600.0,true,0.37);
					CheckRelative( centimeterSample.t,100.0*meterSample.t,5e-14,
						"paired equiangular sample distance scales by 100" );
					CheckRelative( centimeterSample.pdf*100.0,meterSample.pdf,5e-14,
						"paired equiangular sample PDF carries the inverse-length Jacobian" );
					const Scalar meterQuery = 2.25;
					const Scalar meterDensity = meter->EquiangularDistancePdf(
						meterPivots,meterRay,0.0,6.0,true,meterQuery);
					const Scalar centimeterDensity = centimeter->EquiangularDistancePdf(
						centimeterPivots,centimeterRay,0.0,600.0,true,
						100.0*meterQuery);
					CheckRelative( centimeterDensity*100.0,meterDensity,5e-14,
						"complete fixed-U mixture density carries the inverse-length Jacobian" );
				}
			}
		}
		safe_release( meterCaster );
		safe_release( centimeterCaster );
		safe_release( meterJob );
		safe_release( centimeterJob );
	}

	void TestIndependentPivotVectorAndPowerMixture()
	{
		std::cout << "TestIndependentPivotVectorAndPowerMixture" << std::endl;
		const std::string scene = GlobalMediumScene() +
			"omni_light\n{\nname key\npower 3\ncolor 0.8 0.3 0.1\nposition 2 0 0\n}\n";
		IJobPriv* job = LoadScene( scene );
		IRayCaster* caster = nullptr;
		if( job ) {
			IShader* shader = job->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(&caster,false,0,*shader,true) && caster )
				caster->AttachScene( job->GetScene() );
		}
		const RayCaster* concrete = dynamic_cast<const RayCaster*>(caster);
		const LightSampler* lights = concrete ? concrete->GetLightSampler() : nullptr;
		const IMedium* fire = job ? job->GetScene()->GetGlobalMedium() : nullptr;
		const ILightManager::LightsList* sceneLights = job ?
			&job->GetScene()->GetLights()->getLights() : nullptr;
		Check( lights && fire && sceneLights && sceneLights->size()==1,
			"point-light plus flame pivot fixture prepares" );
		if( lights && fire && sceneLights && sceneLights->size()==1 ) {
			Check( lights->IsEquiangularPivotDistributionValid() &&
				lights->GetEquiangularPivotEntryCount()==2,
				"one positional light and one emissive medium form two pivot entries" );
			const Scalar lightPower = (*sceneLights)[0]->EstimateVisibleBandPower();
			const Scalar mediumPower = fire->GetThermalEmissionPowerProxy();
			const Scalar totalPower = lightPower + mediumPower;
			CheckRelative( lights->GetEquiangularPivotPower(0), lightPower, 1e-15,
				"positional pivot uses visible-band power rather than RGB exitance" );
			CheckRelative( lights->GetEquiangularPivotPower(1), mediumPower, 1e-15,
				"medium pivot uses the 4-pi-s-squared thermal power proxy" );
			CheckRelative( lights->GetEquiangularPivotSelectionPdf(0),
				lightPower/totalPower, 2e-15,
				"positional pivot pmf is normalized in the shared watt measure" );
			CheckRelative( lights->GetEquiangularPivotSelectionPdf(1),
				mediumPower/totalPower, 2e-15,
				"medium pivot pmf is normalized in the shared watt measure" );

			FixedSampler sampler( {
				0.11, 0.23, 0.31, 0.41, 0.53,
				0.67, 0.73, 0.79, 0.83, 0.89, 0.97 } );
			VolumeEmissionPivotState pivots;
			const bool pivoted = lights->SampleVolumeEmissionPivots(sampler,pivots);
			Check( pivoted && pivots.mediumPivots.size()==1,
				"pivot vector draws one unconditional point per emissive medium" );
			VolumeEmissionSample endpoint;
			const bool endpointSampled = lights->SampleVolumeEmission(sampler,endpoint);
			Check( endpointSampled && sampler.Consumed()==11,
				"NEE endpoint consumes a distinct draw after the complete pivot vector" );
			if( pivoted && endpointSampled ) {
				const Vector3 delta = Vector3Ops::mkVector3(
					endpoint.point, pivots.mediumPivots[0] );
				Check( Vector3Ops::Magnitude(delta) > 1e-12,
					"auxiliary medium pivot and NEE endpoint are different random variables" );
			}

			Point3 selectedPivot;
			Scalar selectedPdf = 0.0;
			const bool selected = lights->SampleEquiangularPivot(
				pivots, 0.37, selectedPivot, selectedPdf );
			Check( selected && selectedPdf > 0.0,
				"combined power distribution selects a retained shared pivot" );

			const Ray ray( Point3(0,0,-3), Vector3(0,0,1) );
			const Scalar tMin = 0.0;
			const Scalar tMax = 6.0;
			const Scalar t = 2.25;
			const Scalar expectedDensity =
				lights->GetEquiangularPivotSelectionPdf(0) *
					EquiangularSampling::Pdf(
						ray, lights->GetPositionalLightPosition(0),
						tMin, tMax, true, t ) +
				lights->GetEquiangularPivotSelectionPdf(1) *
					EquiangularSampling::Pdf(
						ray, pivots.mediumPivots[0],
						tMin, tMax, true, t );
			CheckRelative( lights->EquiangularDistancePdf(
				pivots, ray, tMin, tMax, true, t ), expectedDensity, 2e-15,
				"fixed-U distance density sums every light and medium pivot entry" );

			Scalar integral = 0.0;
			const unsigned int bins = 20000;
			const Scalar dt = (tMax-tMin)/static_cast<Scalar>(bins);
			for( unsigned int i = 0; i < bins; ++i ) {
				integral += lights->EquiangularDistancePdf(
					pivots, ray, tMin, tMax, true,
					tMin+(static_cast<Scalar>(i)+0.5)*dt ) * dt;
			}
			CheckRelative( integral, 1.0, 2e-7,
				"combined fixed-U equiangular density is normalized" );
		}
		safe_release( caster );
		safe_release( job );
	}

	void TestOpaqueGeometricVisibility()
	{
		std::cout << "TestOpaqueGeometricVisibility" << std::endl;
		IJobPriv* clearJob = LoadScene( GlobalMediumScene(false) );
		IJobPriv* blockedJob = LoadScene( GlobalMediumScene(true) );
		IJobPriv* shadowDisabledJob = LoadScene(
			GlobalMediumScene(true,false,false) );
		IJobPriv* nullBoundaryJob = LoadScene(
			GlobalMediumScene(true,false,true) );
		IRayCaster* clearCaster = nullptr;
		IRayCaster* blockedCaster = nullptr;
		IRayCaster* shadowDisabledCaster = nullptr;
		IRayCaster* nullBoundaryCaster = nullptr;
		if( clearJob ) {
			IShader* shader = clearJob->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(&clearCaster,false,0,*shader,true) && clearCaster )
				clearCaster->AttachScene( clearJob->GetScene() );
		}
		if( blockedJob ) {
			IShader* shader = blockedJob->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(&blockedCaster,false,0,*shader,true) && blockedCaster )
				blockedCaster->AttachScene( blockedJob->GetScene() );
		}
		if( shadowDisabledJob ) {
			IShader* shader = shadowDisabledJob->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(
				&shadowDisabledCaster,false,0,*shader,true) && shadowDisabledCaster )
				shadowDisabledCaster->AttachScene( shadowDisabledJob->GetScene() );
		}
		if( nullBoundaryJob ) {
			IShader* shader = nullBoundaryJob->GetShaders()->GetItem("global");
			if( shader && RISE_API_CreateRayCaster(
				&nullBoundaryCaster,false,0,*shader,true) && nullBoundaryCaster )
				nullBoundaryCaster->AttachScene( nullBoundaryJob->GetScene() );
		}
		const RayCaster* clearConcrete = dynamic_cast<const RayCaster*>(clearCaster);
		const RayCaster* blockedConcrete = dynamic_cast<const RayCaster*>(blockedCaster);
		const LightSampler* clearLights = clearConcrete ? clearConcrete->GetLightSampler() : nullptr;
		const LightSampler* blockedLights = blockedConcrete ? blockedConcrete->GetLightSampler() : nullptr;
		const RayCaster* shadowDisabledConcrete =
			dynamic_cast<const RayCaster*>(shadowDisabledCaster);
		const RayCaster* nullBoundaryConcrete =
			dynamic_cast<const RayCaster*>(nullBoundaryCaster);
		const LightSampler* shadowDisabledLights = shadowDisabledConcrete
			? shadowDisabledConcrete->GetLightSampler() : nullptr;
		const LightSampler* nullBoundaryLights = nullBoundaryConcrete
			? nullBoundaryConcrete->GetLightSampler() : nullptr;
		Check( clearLights && blockedLights && shadowDisabledLights && nullBoundaryLights,
			"clear, ordinary-interface, and exact-null visibility fixtures prepare" );

		std::vector<Scalar> replayValues;
		if( clearLights ) {
			for( unsigned int seed = 1; seed <= 256 && replayValues.empty(); ++seed ) {
				RecordingSampler sampler( seed );
				VolumeEmissionSample endpoint;
				if( clearLights->SampleVolumeEmission(sampler,endpoint) &&
					endpoint.point.z > 0.25 ) replayValues = sampler.Values();
			}
		}
		Check( !replayValues.empty(),
			"visibility fixture finds an endpoint beyond the blocker plane" );
		if( !replayValues.empty() && clearLights && blockedLights &&
			shadowDisabledLights && nullBoundaryLights && clearJob && blockedJob &&
			shadowDisabledJob && nullBoundaryJob ) {
			const RasterizerState rast = {0,0};
			const Ray viewRay( Point3(0,0,-1), Vector3(0,0,-1) );
			RayIntersectionGeometric ri( viewRay, rast );
			ri.ptIntersection = Point3(0,0,-1);
			ri.vNormal = Vector3(0,0,1);
			SpectralResponseBSDF* response = new SpectralResponseBSDF();
			FixedSampler clearSampler( replayValues );
			const Scalar clearContribution = clearLights->EvaluateVolumeDirectLightingNM(
				ri, *response, 550.0, clearSampler,
				clearJob->GetScene()->GetGlobalMedium(), true, nullptr, nullptr );
			FixedSampler blockedSampler( replayValues );
			const Scalar blockedContribution = blockedLights->EvaluateVolumeDirectLightingNM(
				ri, *response, 550.0, blockedSampler,
				blockedJob->GetScene()->GetGlobalMedium(), true, nullptr, nullptr );
			FixedSampler shadowDisabledSampler( replayValues );
			const Scalar shadowDisabledContribution =
				shadowDisabledLights->EvaluateVolumeDirectLightingNM(
					ri, *response, 550.0, shadowDisabledSampler,
					shadowDisabledJob->GetScene()->GetGlobalMedium(), true, nullptr, nullptr );
			FixedSampler nullBoundarySampler( replayValues );
			const Scalar nullBoundaryContribution =
				nullBoundaryLights->EvaluateVolumeDirectLightingNM(
					ri, *response, 550.0, nullBoundarySampler,
					nullBoundaryJob->GetScene()->GetGlobalMedium(), true, nullptr, nullptr );
			Check( clearContribution > 0.0,
				"unobstructed paired endpoint contributes positive volume NEE" );
			Check( blockedContribution == 0.0,
				"opaque geometry supplies the estimator visibility factor" );
			Check( shadowDisabledContribution == 0.0,
				"non-null interface blocks volume NEE independent of casts_shadows" );
			CheckRelative( nullBoundaryContribution, clearContribution, 2e-14,
				"exact null boundary passes volume NEE independent of casts_shadows" );
			safe_release( response );
		}
		safe_release( clearCaster );
		safe_release( blockedCaster );
		safe_release( shadowDisabledCaster );
		safe_release( nullBoundaryCaster );
		safe_release( clearJob );
		safe_release( blockedJob );
		safe_release( shadowDisabledJob );
		safe_release( nullBoundaryJob );
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
				const Scalar expected = 0.8 * Vector3Ops::Dot(direction,ri.vNormal) * tr *
					endpoint.pMedium->GetThermalEmissionNM(endpoint.point,nm) /
					(distance*distance*endpoint.pdf);
				SpectralResponseBSDF* unit = new SpectralResponseBSDF();
				FixedSampler estimatorSampler( randomValues );
				const Scalar actual = lights->EvaluateVolumeDirectLightingNM(
					ri, *unit, nm, estimatorSampler,
					job->GetScene()->GetGlobalMedium(), false, nullptr, nullptr );
				Check( walked, "standalone estimator uses the complete shadow-medium walk" );
				CheckRelative( actual, expected, 2e-14,
					"surface volume NEE equals f cos Tr epsilon over r^2 labeled-pdf" );

				FixedSampler volumeEstimatorSampler( randomValues );
				const Scalar volumeActual = lights->EvaluateVolumeDirectLightingNM(
					ri, *unit, nm, volumeEstimatorSampler,
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
	TestSharedQuadratureStructure();
	TestSharedVisibleBandRule();
	TestCDFNormalizationSamplingAndOwnership();
	TestNonuniformTwoLevelDistribution();
	TestSampledBoundaryOwnership();
	TestSceneUnitInvariantImportance();
	TestTinyFiniteBBoxDensityFormulation();
	TestRepresentedBinVolumeNormalization();
	TestAnisotropicBinVolumeFormulation();
	TestPositiveSupportUnderflowRejection();
	TestSupportInflationAtBoundary();
	TestScaleSafeCrossMediumNormalization();
	TestLabeledMultiMediumDensity();
	TestInactiveNestedLabelReturnsZero();
	TestIndependentPivotVectorAndPowerMixture();
	TestCombinedPivotSceneUnitInvariance();
	TestOpaqueGeometricVisibility();
	TestStandaloneEstimatorFormula();
	std::cout << "Passed: " << passed << " Failed: " << failed << std::endl;
	return failed == 0 ? 0 : 1;
}
