//////////////////////////////////////////////////////////////////////
//
//  MultichannelHeterogeneousMediumTest.cpp
//
//  Phase-A gate for the painter-baked carbon + temperature medium:
//  shared trilinear lattice, derived phi(T) grey optics, the 10^-3
//  g/m^3 conversion, scene-unit invariance, and §9 requiredness.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
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
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Parsers/ChunkParserRegistry.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

namespace
{
	int passed = 0;
	int failed = 0;

	void Check( const bool condition, const char* label )
	{
		if( condition ) {
			++passed;
		} else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	bool Near( const Scalar actual, const Scalar expected, const Scalar tolerance )
	{
		return std::fabs( actual - expected ) <= tolerance;
	}

	Scalar ScalarFromBits( const std::uint64_t bits )
	{
		static_assert( sizeof( Scalar ) == sizeof( bits ),
			"non-finite regression inputs require binary64 Scalar" );
		volatile std::uint64_t barrier = bits;
		const std::uint64_t materialised = barrier;
		Scalar value = 0.0;
		std::memcpy( &value, &materialised, sizeof( value ) );
		return value;
	}

	class AffineWorldScalarPainter :
		public virtual IScalarPainter,
		public virtual Implementation::Reference
	{
	public:
		Scalar bias;
		Scalar x;
		Scalar y;
		Scalar z;

		AffineWorldScalarPainter( Scalar bias_, Scalar x_, Scalar y_, Scalar z_ ) :
		  bias( bias_ ), x( x_ ), y( y_ ), z( z_ )
		{
		}

		ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
		{
			return ScalarTriple(
				bias + x * ri.ptIntersection.x + y * ri.ptIntersection.y + z * ri.ptIntersection.z );
		}

		bool HasPerChannelVariation() const override { return false; }

	protected:
		~AffineWorldScalarPainter() override = default;
	};

	IMedium* CreateMedium(
		const IScalarPainter& carbon,
		const IScalarPainter& temperature,
		const Point3& bboxMin,
		const Point3& bboxMax,
		const Scalar sceneUnitMeters,
		const unsigned int resolution = 8
		)
	{
		IMedium* medium = nullptr;
		const bool ok = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, carbon, temperature,
			resolution, resolution, resolution,
			bboxMin, bboxMax, sceneUnitMeters,
			0.26, 1800.0, 0.10, 0.5,
			8.7, 1.2, 0.6, 0.6 );
		return ok ? medium : nullptr;
	}

	struct FactoryInputs
	{
		Point3 bboxMin;
		Point3 bboxMax;
		Scalar sceneUnitMeters;
		Scalar sootEm;
		Scalar sootDensity;
		Scalar sootAlbedoHot;
		Scalar sootGHot;
		Scalar smokeKmCarbon;
		Scalar smokeNCarbon;
		Scalar smokeAlbedoCarbon;
		Scalar smokeGCarbon;

		FactoryInputs() :
		  bboxMin( 0, 0, 0 ), bboxMax( 1, 1, 1 ), sceneUnitMeters( 1.0 ),
		  sootEm( 0.26 ), sootDensity( 1800.0 ), sootAlbedoHot( 0.10 ), sootGHot( 0.5 ),
		  smokeKmCarbon( 8.7 ), smokeNCarbon( 1.2 ),
		  smokeAlbedoCarbon( 0.6 ), smokeGCarbon( 0.6 )
		{
		}
	};

	bool FactoryRejects(
		const IScalarPainter& carbon,
		const IScalarPainter& temperature,
		const FactoryInputs& inputs
		)
	{
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, carbon, temperature, 2, 2, 2,
			inputs.bboxMin, inputs.bboxMax, inputs.sceneUnitMeters,
			inputs.sootEm, inputs.sootDensity, inputs.sootAlbedoHot, inputs.sootGHot,
			inputs.smokeKmCarbon, inputs.smokeNCarbon,
			inputs.smokeAlbedoCarbon, inputs.smokeGCarbon );
		safe_release( medium );
		return !created && !medium;
	}

	void TestBakedTrilinearChannelsAndOptics()
	{
		std::cout << "TestBakedTrilinearChannelsAndOptics" << std::endl;
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 2.0, 1.0, 2.0, 3.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 600.0, 400.0, 0.0, 0.0 );
		IMedium* medium = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		Check( medium != nullptr, "factory creates the two-channel medium" );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( fire != nullptr && fire->IsValid(), "factory returns the multichannel concrete type" );
		if( !fire ) {
			safe_release( medium );
			safe_release( carbon );
			safe_release( temperature );
			return;
		}

		const Point3 p( 0.4375, 0.4375, 0.4375 );
		const Scalar expectedCarbon = 4.625;
		const Scalar expectedTemperature = 775.0;
		const Scalar smoothArg = (expectedTemperature - 700.0) / 200.0;
		const Scalar expectedPhi = smoothArg * smoothArg * (3.0 - 2.0 * smoothArg);
		Check( Near( fire->LookupCarbon( p ), expectedCarbon, 1e-12 ),
			"trilinear bake reproduces an affine carbon field between voxels" );
		Check( Near( fire->LookupTemperature( p ), expectedTemperature, 1e-12 ),
			"temperature shares the same trilinear lattice" );
		Check( Near( fire->HotOpticsFraction( p ), expectedPhi, 1e-12 ),
			"phi(T) is the pinned 700-900 K smoothstep" );

		const Scalar hotAbsorptionMass =
			6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
		const Scalar hotAbsorption = expectedCarbon * expectedPhi * hotAbsorptionMass;
		const Scalar hotScattering = hotAbsorption * 0.10 / 0.90;
		const Scalar coolExtinction = expectedCarbon * (1.0 - expectedPhi) * 8.7;
		const Scalar coolScattering = coolExtinction * 0.6;
		const Scalar expectedSigmaS = hotScattering + coolScattering;
		const Scalar expectedSigmaT = hotAbsorption + hotScattering + coolExtinction;
		const MediumCoefficients c = fire->GetCoefficients( p );
		Check( Near( c.sigma_t[0], expectedSigmaT, 1e-11 ) &&
			Near( c.sigma_t[1], expectedSigmaT, 1e-11 ) &&
			Near( c.sigma_t[2], expectedSigmaT, 1e-11 ),
			"grey 633-nm extinction follows the hot/cool carbon partition" );
		Check( Near( c.sigma_s[0], expectedSigmaS, 1e-11 ),
			"grey scattering applies both authored single-scattering albedos" );
		const MediumCoefficientsNM cnm = fire->GetCoefficientsNM( p, 500.0 );
		Check( Near( cnm.sigma_t, expectedSigmaT, 1e-11 ) &&
			Near( cnm.sigma_s, expectedSigmaS, 1e-11 ),
			"ordered pre-chromatic NM path matches the grey Pel coefficients" );

		carbon->bias = 200.0;
		temperature->bias = 2000.0;
		Check( Near( fire->LookupCarbon( p ), expectedCarbon, 1e-12 ) &&
			Near( fire->LookupTemperature( p ), expectedTemperature, 1e-12 ),
			"channel painters are baked once rather than sampled during rendering" );

		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestPhysicalUnitsAndSceneScale()
	{
		std::cout << "TestPhysicalUnitsAndSceneScale" << std::endl;
		IScalarPainter* carbon = nullptr;
		IScalarPainter* temperature = nullptr;
		RISE_API_CreateUniformScalarPainter( &carbon, 1.0 );
		RISE_API_CreateUniformScalarPainter( &temperature, 1000.0 );
		IMedium* metres = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0 );
		IMedium* centimetres = CreateMedium(
			*carbon, *temperature, Point3( 0, 0, 0 ), Point3( 100, 100, 100 ), 0.01 );
		MultichannelHeterogeneousMedium* m =
			dynamic_cast<MultichannelHeterogeneousMedium*>( metres );
		MultichannelHeterogeneousMedium* cm =
			dynamic_cast<MultichannelHeterogeneousMedium*>( centimetres );
		Check( m != nullptr && cm != nullptr, "metre and centimetre fixtures construct" );
		if( m && cm ) {
			Check( Near( m->HotSootVolumeFraction( Point3( 0.5, 0.5, 0.5 ) ),
				5.555555555555556e-7, 1e-18 ),
				"1 g/m^3 at phi=1 gives f_v=5.5556e-7" );
			const MediumCoefficients cM = m->GetCoefficients( Point3( 0.5, 0.5, 0.5 ) );
			const MediumCoefficients cCM = cm->GetCoefficients( Point3( 50, 50, 50 ) );
			Check( Near( cCM.sigma_t[0], 0.01 * cM.sigma_t[0], 1e-13 ),
				"inverse-length coefficients convert from SI to scene units" );

			const Ray rayM( Point3( 0.5, 0.5, 0.125 ), Vector3( 0, 0, 1 ) );
			const Ray rayCM( Point3( 50, 50, 12.5 ), Vector3( 0, 0, 1 ) );
			const Scalar transM = m->EvalDistancePdf( rayM, 0.5, false, 0.5 );
			const Scalar transCM = cm->EvalDistancePdf( rayCM, 50.0, false, 50.0 );
			const Scalar expectedTau = cM.sigma_t[0] * 0.5;
			Check( Near( -std::log( transM ), expectedTau, 1e-10 ),
				"deterministic optical depth matches the uniform slab" );
			Check( Near( transCM, transM, 1e-12 ),
				"the same physical slab is invariant between metre and centimetre scenes" );
		}

		safe_release( metres );
		safe_release( centimetres );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestPhiSupMajorant()
	{
		std::cout << "TestPhiSupMajorant" << std::endl;
		// Opposing gradients make every x-cell endpoint product small while
		// carbon*k(phi(T)) is large in the interior.  A grid built from the
		// nonlinear product's knots is non-conservative; the required bound
		// is max_phi(k) times the carbon-only lattice majorant.
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 1.0, -2.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 700.0, 400.0, 0.0, 0.0 );
		const Scalar targetHotMassExtinction = 100.0;
		const Scalar sootEm = targetHotMassExtinction * 633.0e-9 * 1800.0 /
			(6.0 * PI * 1.0e-3);
		IMedium* medium = nullptr;
		const bool created = RISE_API_CreateMultichannelHeterogeneousMedium(
			&medium, *carbon, *temperature,
			2, 2, 2, Point3( 0, 0, 0 ), Point3( 1, 1, 1 ), 1.0,
			sootEm, 1800.0, 0.0, 0.5,
			1.0, 1.2, 0.0, 0.6 );
		MultichannelHeterogeneousMedium* fire =
			dynamic_cast<MultichannelHeterogeneousMedium*>( medium );
		Check( created && fire, "adversarial majorant fixture constructs" );
		if( fire ) {
			const Point3 interior( 0.25, 0.25, 0.25 );
			const Scalar sigmaT = fire->GetCoefficients( interior ).sigma_t[0];
			const Scalar majorant = fire->TrackingMajorantAt( interior );
			Check( Near( sigmaT, 25.25, 1e-11 ),
				"opposing carbon/temperature gradients create the intended interior maximum" );
			Check( majorant >= sigmaT,
				"phi-sup carbon majorant bounds the nonlinear interior extinction" );
		}
		safe_release( medium );
		safe_release( carbon );
		safe_release( temperature );
	}

	void TestNonFiniteRejection()
	{
		std::cout << "TestNonFiniteRejection" << std::endl;
		const Scalar infinity = ScalarFromBits( UINT64_C(0x7FF0000000000000) );
		const Scalar nan = ScalarFromBits( UINT64_C(0x7FF8000000000000) );
		AffineWorldScalarPainter* carbon = new AffineWorldScalarPainter( 1.0, 0.0, 0.0, 0.0 );
		AffineWorldScalarPainter* temperature = new AffineWorldScalarPainter( 1000.0, 0.0, 0.0, 0.0 );

		AffineWorldScalarPainter* infiniteCarbon =
			new AffineWorldScalarPainter( infinity, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *infiniteCarbon, *temperature, FactoryInputs() ),
			"+Inf carbon painter is rejected before majorant construction" );
		safe_release( infiniteCarbon );

		AffineWorldScalarPainter* nanTemperature =
			new AffineWorldScalarPainter( nan, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *carbon, *nanTemperature, FactoryInputs() ),
			"NaN temperature painter is rejected under fast-math" );
		safe_release( nanTemperature );

		AffineWorldScalarPainter* zeroTemperature =
			new AffineWorldScalarPainter( 0.0, 0.0, 0.0, 0.0 );
		Check( FactoryRejects( *carbon, *zeroTemperature, FactoryInputs() ),
			"non-positive temperature is rejected" );
		safe_release( zeroTemperature );

		FactoryInputs invalid;
		invalid.bboxMin.x = nan;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"NaN bbox component is rejected by the direct factory" );
		invalid = FactoryInputs();
		invalid.bboxMax.z = infinity;
		Check( FactoryRejects( *carbon, *temperature, invalid ),
			"+Inf bbox component is rejected by the direct factory" );

		struct ScalarInput
		{
			const char* label;
			Scalar FactoryInputs::* member;
		};
		const ScalarInput scalarInputs[] = {
			{ "scene_unit", &FactoryInputs::sceneUnitMeters },
			{ "soot_em", &FactoryInputs::sootEm },
			{ "soot_density", &FactoryInputs::sootDensity },
			{ "soot_albedo_hot", &FactoryInputs::sootAlbedoHot },
			{ "soot_g_hot", &FactoryInputs::sootGHot },
			{ "smoke_km_carbon", &FactoryInputs::smokeKmCarbon },
			{ "smoke_n_carbon", &FactoryInputs::smokeNCarbon },
			{ "smoke_albedo_carbon", &FactoryInputs::smokeAlbedoCarbon },
			{ "smoke_g_carbon", &FactoryInputs::smokeGCarbon }
		};
		for( const ScalarInput& input : scalarInputs ) {
			invalid = FactoryInputs();
			invalid.*(input.member) = infinity;
			Check( FactoryRejects( *carbon, *temperature, invalid ),
				(std::string( "+Inf " ) + input.label + " is rejected by the direct factory").c_str() );
		}

		safe_release( carbon );
		safe_release( temperature );
	}

	const IAsciiChunkParser* FindMultichannelParser(
		const std::vector<ChunkParserEntry>& entries
		)
	{
		for( const ChunkParserEntry& entry : entries ) {
			if( entry.keyword == "multichannel_heterogeneous_medium" ) {
				return entry.parser.get();
			}
		}
		return nullptr;
	}

	void FillValidBag( ParseStateBag& bag, const char* omit )
	{
		struct Pair { const char* key; const char* value; };
		static const Pair values[] = {
			{ "name", "fire" },
			{ "channel_carbon", "painter carbon" },
			{ "channel_temperature", "painter temperature" },
			{ "bake_resolution", "4 4 4" },
			{ "bbox_min", "0 0 0" },
			{ "bbox_max", "1 1 1" },
			{ "soot_em", "0.26" },
			{ "soot_density", "1800" },
			{ "soot_albedo_hot", "0.10" },
			{ "soot_g_hot", "0.5" },
			{ "smoke_km_carbon", "8.7" },
			{ "smoke_n_carbon", "1.2" },
			{ "smoke_albedo_carbon", "0.6" },
			{ "smoke_g_carbon", "0.6" }
		};
		for( const Pair& value : values ) {
			if( !omit || std::string( value.key ) != omit ) {
				bag.SetSingle( value.key, value.value );
			}
		}
	}

	void TestDescriptorAndRequiredness()
	{
		std::cout << "TestDescriptorAndRequiredness" << std::endl;
		std::vector<ChunkParserEntry> entries = CreateAllChunkParsers();
		const IAsciiChunkParser* parser = FindMultichannelParser( entries );
		Check( parser != nullptr, "chunk is registered" );
		if( !parser ) return;

		const ChunkDescriptor& descriptor = parser->Describe();
		std::vector<std::string> required;
		for( const ParameterDescriptor& parameter : descriptor.parameters ) {
			if( parameter.required ) required.push_back( parameter.name );
		}
		Check( required.size() == 14, "all 14 Phase-A fields are descriptor-required" );

		IJobPriv* job = nullptr;
		RISE_CreateJobPriv( &job );
		Check( job != nullptr, "requiredness fixture job constructs" );
		if( !job ) return;

		for( const std::string& omitted : required ) {
			ParseStateBag bag( &descriptor );
			FillValidBag( bag, omitted.c_str() );
			Check( !parser->Finalize( bag, *job ),
				("omitting required parameter " + omitted + " fails").c_str() );
		}
		safe_release( job );
	}

	std::string SceneText()
	{
		return
			"RISE ASCII SCENE 7\n\n"
			"scene_options\n{\nscene_unit 0.01\n}\n\n"
			"scalar_painter\n{\nname carbon\nvalue 1\n}\n\n"
			"scalar_painter\n{\nname temperature\nvalue 1000\n}\n\n"
			"multichannel_heterogeneous_medium\n{\n"
			"name fire\n"
			"channel_carbon painter carbon\n"
			"channel_temperature painter temperature\n"
			"bake_resolution 4 4 4\n"
			"bbox_min 0 0 0\n"
			"bbox_max 100 100 100\n"
			"soot_em 0.26\n"
			"soot_density 1800\n"
			"soot_albedo_hot 0.10\n"
			"soot_g_hot 0.5\n"
			"smoke_km_carbon 8.7\n"
			"smoke_n_carbon 1.2\n"
			"smoke_albedo_carbon 0.6\n"
			"smoke_g_carbon 0.6\n"
			"}\n";
	}

	void TestSceneLanguageAndSceneUnitPropagation()
	{
		std::cout << "TestSceneLanguageAndSceneUnitPropagation" << std::endl;
		char filename[128];
		std::snprintf( filename, sizeof(filename),
			"rise_multichannel_medium_%d.RISEscene", static_cast<int>( ::getpid() ) );
		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / filename;
		{
			std::ofstream output( path );
			output << SceneText();
		}

		IJobPriv* job = nullptr;
		RISE_CreateJobPriv( &job );
		const bool loaded = job && job->LoadAsciiSceneViaCst( path.string().c_str() );
		Check( loaded, "complete multichannel scene chunk loads" );
		if( loaded ) {
			const IMedium* medium = job->GetMedium( "fire" );
			const MultichannelHeterogeneousMedium* fire =
				dynamic_cast<const MultichannelHeterogeneousMedium*>( medium );
			Check( fire != nullptr, "scene chunk registers the concrete medium" );
			if( fire ) {
				const Scalar hotAbsorptionMass =
					6.0 * PI * 0.26 * 1.0e-3 / (633.0e-9 * 1800.0);
				const Scalar expectedSigmaT = 0.01 * hotAbsorptionMass / 0.90;
				Check( Near( fire->GetCoefficients( Point3( 50, 50, 50 ) ).sigma_t[0],
					expectedSigmaT, 1e-12 ),
					"scene_options.scene_unit reaches medium construction" );
			}
		}

		safe_release( job );
		std::filesystem::remove( path );
	}
}

int main()
{
	TestBakedTrilinearChannelsAndOptics();
	TestPhysicalUnitsAndSceneScale();
	TestPhiSupMajorant();
	TestNonFiniteRejection();
	TestDescriptorAndRequiredness();
	TestSceneLanguageAndSceneUnitPropagation();

	std::cout << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
