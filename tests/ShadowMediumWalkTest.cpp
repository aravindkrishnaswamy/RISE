//////////////////////////////////////////////////////////////////////
//
//  ShadowMediumWalkTest.cpp - Phase-B gate 1 numeric regressions.
//
//  Exercises the production shadow-medium walk directly.  The fixtures pin
//  dynamic nesting, an uncapped boundary count, segment-local evaluation of
//  an interrupted heterogeneous global medium, and preservation of small but
//  nonzero transmittance below the historical hard cutoff.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/IORStack.h"
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
	const Scalar kWavelengthNM = 500.0;

	void Check( const bool condition, const char* label )
	{
		if( condition ) {
			++passed;
		} else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	void CheckClose(
		const Scalar actual,
		const Scalar expected,
		const Scalar relativeTolerance,
		const char* label
		)
	{
		const Scalar scale = std::fmax( std::fabs( expected ), Scalar( 1e-300 ) );
		Check( std::fabs( actual - expected ) <= relativeTolerance * scale, label );
		if( std::fabs( actual - expected ) > relativeTolerance * scale ) {
			std::cout << "  got " << actual << ", expected " << expected << std::endl;
		}
	}

	IJobPriv* LoadScene( const std::string& text )
	{
		std::ostringstream name;
		name << "rise_shadow_medium_walk_" << static_cast<int>( ::getpid() )
			<< "_" << fixtureSerial++ << ".RISEscene";
		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / name.str();
		{
			std::ofstream output( path );
			output << text;
		}

		IJobPriv* job = nullptr;
		const bool created = RISE_CreateJobPriv( &job ) && job;
		const bool loaded = created && job->LoadAsciiSceneViaCst( path.string().c_str() );
		std::filesystem::remove( path );
		if( !loaded ) {
			safe_release( job );
			return nullptr;
		}
		job->GetScene()->GetObjects()->PrepareForRendering();
		return job;
	}

	void AppendHomogeneousMedium(
		std::ostringstream& scene,
		const std::string& name,
		const Scalar extinction
		)
	{
		scene <<
			"homogeneous_medium\n{\n"
			"name " << name << "\n"
			"absorption " << extinction << " " << extinction << " " << extinction << "\n"
			"scattering 0 0 0\n"
			"phase isotropic\n"
			"}\n";
	}

	void AppendBox(
		std::ostringstream& scene,
		const std::string& name,
		const Scalar depth,
		const Scalar centerZ,
		const std::string& medium
		)
	{
		scene <<
			"box_geometry\n{\n"
			"name " << name << "_geometry\n"
			"width 2\nheight 2\ndepth " << depth << "\n"
			"}\n"
			"standard_object\n{\n"
			"name " << name << "\n"
			"geometry " << name << "_geometry\n"
			"position 0 0 " << centerZ << "\n"
			"interior_medium " << medium << "\n"
			"casts_shadows FALSE\n"
			"}\n";
	}

	void AppendScaledBox(
		std::ostringstream& scene,
		const std::string& name,
		const Scalar depth,
		const Scalar centerZ,
		const Scalar scaleZ,
		const std::string& medium
		)
	{
		scene <<
			"box_geometry\n{\n"
			"name " << name << "_geometry\n"
			"width 2\nheight 2\ndepth " << depth << "\n"
			"}\n"
			"standard_object\n{\n"
			"name " << name << "\n"
			"geometry " << name << "_geometry\n"
			"position 0 0 " << centerZ << "\n"
			"scale 1 1 " << scaleZ << "\n"
			"interior_medium " << medium << "\n"
			"casts_shadows FALSE\n"
			"}\n";
	}

	void EvaluateBoth(
		const IScene* scene,
		const Scalar maxDist,
		RISEPel& rgb,
		Scalar& spectral,
		bool& rgbOK,
		bool& spectralOK
		)
	{
		const Ray ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		rgbOK = EvaluateShadowMediumTransmittance(
			ray, maxDist, nullptr, nullptr, scene, true, rgb );
		spectralOK = EvaluateShadowMediumTransmittanceNM(
			ray, maxDist, nullptr, nullptr, scene, true, kWavelengthNM, spectral );
	}

	void TestMoreThanFourNestedMedia()
	{
		std::cout << "TestMoreThanFourNestedMedia" << std::endl;
		std::ostringstream scene;
		scene << "RISE ASCII SCENE 7\n";
		Scalar opticalDepth = 0.0;
		for( unsigned int i = 1; i <= 5; ++i ) {
			std::ostringstream mediumName;
			mediumName << "medium" << i;
			const Scalar extinction = 0.1 * Scalar( i );
			AppendHomogeneousMedium( scene, mediumName.str(), extinction );
			std::ostringstream objectName;
			objectName << "box" << i;
			AppendBox( scene, objectName.str(), 12.0 - 2.0 * Scalar( i ), 6.0,
				mediumName.str() );
			opticalDepth += 2.0 * extinction;
		}

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "five-deep nested-medium fixture loads" );
		if( job ) {
			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 12.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -opticalDepth );
			Check( rgbOK && spectralOK, "five-deep walk completes without a stack cap" );
			CheckClose( rgb.r, expected, 1e-10,
				"RGB five-deep walk uses every innermost segment" );
			CheckClose( spectral, expected, 1e-10,
				"NM five-deep walk uses every innermost segment" );
		}
		safe_release( job );
	}

	void TestMoreThanSixteenCrossings()
	{
		std::cout << "TestMoreThanSixteenCrossings" << std::endl;
		std::ostringstream scene;
		scene << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "fog", 0.1 );
		for( unsigned int i = 1; i <= 9; ++i ) {
			std::ostringstream objectName;
			objectName << "slab" << i;
			AppendBox( scene, objectName.str(), 0.5, Scalar( i ), "fog" );
		}

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "eighteen-crossing fixture loads" );
		if( job ) {
			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 10.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -0.45 );
			Check( rgbOK && spectralOK, "eighteen-boundary walk completes without a crossing cap" );
			CheckClose( rgb.r, expected, 1e-10,
				"RGB walk retains the ninth disjoint slab" );
			CheckClose( spectral, expected, 1e-10,
				"NM walk retains the ninth disjoint slab" );
		}
		safe_release( job );
	}

	void TestMoreThanFourNonLifoOverlaps()
	{
		std::cout << "TestMoreThanFourNonLifoOverlaps" << std::endl;
		std::ostringstream scene;
		scene << "RISE ASCII SCENE 7\n";
		for( unsigned int i = 1; i <= 5; ++i ) {
			std::ostringstream mediumName;
			mediumName << "overlap_medium" << i;
			AppendHomogeneousMedium( scene, mediumName.str(), 0.1 * Scalar( i ) );
		}
		AppendBox( scene, "overlap_a", 6.0, 4.0, "overlap_medium1" );
		AppendBox( scene, "overlap_b", 6.0, 5.0, "overlap_medium2" );
		AppendBox( scene, "overlap_c", 6.0, 6.0, "overlap_medium3" );
		AppendBox( scene, "overlap_d", 6.0, 7.0, "overlap_medium4" );
		AppendBox( scene, "overlap_e", 1.0, 5.5, "overlap_medium5" );

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "five-way non-LIFO overlap fixture loads" );
		if( job ) {
			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 11.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -3.1 );
			Check( rgbOK && spectralOK,
				"five-way non-LIFO overlap removes buried entries without truncation" );
			CheckClose( rgb.r, expected, 1e-10,
				"RGB non-LIFO overlap preserves the most-recent active medium" );
			CheckClose( spectral, expected, 1e-10,
				"NM non-LIFO overlap preserves the most-recent active medium" );
		}
		safe_release( job );
	}

	void TestResolvableThinSegmentIsNotSkipped()
	{
		std::cout << "TestResolvableThinSegmentIsNotSkipped" << std::endl;
		std::ostringstream scene;
		scene << std::setprecision( 17 ) << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "thin_medium", 100000.0 );
		AppendBox( scene, "thin_slab", 5e-6, 1.0, "thin_medium" );

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "sub-1e-5 medium segment fixture loads" );
		if( job ) {
			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 2.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -0.5 );
			Check( rgbOK && spectralOK,
				"sub-1e-5 segment completes without a geometric step-off" );
			CheckClose( rgb.r, expected, 1e-8,
				"RGB thin segment retains its full optical depth" );
			CheckClose( spectral, expected, 1e-8,
				"NM thin segment retains its full optical depth" );
		}
		safe_release( job );
	}

	void TestScaledNearCoincidentBoundariesUseExactOrdering()
	{
		std::cout << "TestScaledNearCoincidentBoundariesUseExactOrdering" << std::endl;
		std::ostringstream scene;
		scene << std::setprecision( 17 ) << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "near_medium", 0.7 );
		AppendHomogeneousMedium( scene, "scaled_medium", 0.2 );
		// Exact entries are A=1 and B=1.0000005.  B's local 1e-12
		// shading offset becomes 1e-6 after its z scale, so legacy ranges
		// reverse those two events even though both exact values are readily
		// representable.
		AppendBox( scene, "near_box", 1.0, 1.5, "near_medium" );
		AppendScaledBox( scene, "scaled_box", 2e-6, 2.0000005,
			1e6, "scaled_medium" );

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "scaled near-coincident fixture loads" );
		if( job ) {
			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 4.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -(0.7 * 5e-7 + 0.2 * 2.0) );
			Check( rgbOK && spectralOK,
				"scaled near-coincident topology walk completes" );
			CheckClose( rgb.r, expected, 1e-9,
				"RGB closest-hit ordering uses exact geometric boundaries" );
			CheckClose( spectral, expected, 1e-9,
				"NM closest-hit ordering uses exact geometric boundaries" );
		}
		safe_release( job );
	}

	void TestOverlappingCsgUnionUsesComposedExactBoundaries()
	{
		std::cout << "TestOverlappingCsgUnionUsesComposedExactBoundaries" << std::endl;
		std::ostringstream scene;
		scene << std::setprecision( 17 ) << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "csg_medium", 0.4 );
		AppendBox( scene, "csg_a", 3.0, 2.5, "csg_medium" );
		AppendBox( scene, "csg_b", 3.0, 4.5, "csg_medium" );
		scene <<
			"csg_object\n{\n"
			"name csg_union\n"
			"obja csg_a\n"
			"objb csg_b\n"
			"operation union\n"
			"position 0 0 0.25\n"
			"casts_shadows FALSE\n"
			"}\n";

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "overlapping CSG-union fixture loads" );
		if( job ) {
			Check( job->SetObjectInteriorMedium( "csg_union", "csg_medium" ),
				"CSG union receives its composite interior medium" );
			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 8.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -0.4 * 5.0 );
			Check( rgbOK && spectralOK,
				"overlapping CSG union walk completes across its hidden overlap" );
			CheckClose( rgb.r, expected, 1e-10,
				"RGB CSG union exits at the composed far boundary" );
			CheckClose( spectral, expected, 1e-10,
				"NM CSG union exits at the composed far boundary" );
		}
		safe_release( job );
	}

	void TestOriginInsideNestedMediaRestoresOuterStack()
	{
		std::cout << "TestOriginInsideNestedMediaRestoresOuterStack" << std::endl;
		std::ostringstream scene;
		scene << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "outer_medium", 0.1 );
		AppendHomogeneousMedium( scene, "middle_medium", 0.2 );
		AppendHomogeneousMedium( scene, "inner_medium", 0.3 );
		AppendBox( scene, "outer_box", 10.0, 6.0, "outer_medium" );
		AppendBox( scene, "middle_box", 6.0, 6.0, "middle_medium" );
		AppendBox( scene, "inner_box", 2.0, 6.0, "inner_medium" );

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "nested-origin fixture loads" );
		if( job ) {
			const IObject* outer = job->GetScene()->GetObjects()->GetItem( "outer_box" );
			const IObject* middle = job->GetScene()->GetObjects()->GetItem( "middle_box" );
			const IObject* inner = job->GetScene()->GetObjects()->GetItem( "inner_box" );
			Check( outer && middle && inner, "nested-origin objects resolve" );
			if( outer && middle && inner ) {
				IORStack mediumStack( 1.0 );
				mediumStack.SetCurrentObject( outer );
				mediumStack.push( 1.0 );
				mediumStack.SetCurrentObject( middle );
				mediumStack.push( 1.0 );
				mediumStack.SetCurrentObject( inner );
				mediumStack.push( 1.0 );

				const IMedium* innerMedium = inner->GetInteriorMedium();
				const Ray ray( Point3( 0, 0, 6 ), Vector3( 0, 0, 1 ) );
				RISEPel rgb;
				Scalar spectral = 0.0;
				const bool rgbOK = EvaluateShadowMediumTransmittance(
					ray, 6.0, innerMedium, inner, job->GetScene(), true, rgb, &mediumStack );
				const bool spectralOK = EvaluateShadowMediumTransmittanceNM(
					ray, 6.0, innerMedium, inner, job->GetScene(), true,
					kWavelengthNM, spectral, &mediumStack );
				const Scalar expected = std::exp( -0.9 );
				Check( rgbOK && spectralOK, "nested-origin walk consumes the full supplied stack" );
				CheckClose( rgb.r, expected, 1e-10,
					"RGB inner exit restores middle then outer medium" );
				CheckClose( spectral, expected, 1e-10,
					"NM inner exit restores middle then outer medium" );
			}
		}
		safe_release( job );
	}

	class LinearZMedium :
		public virtual IMedium,
		public virtual Reference
	{
		const Scalar m_base;
		const Scalar m_slope;

		Scalar SigmaAt( const Point3& pt ) const
		{
			return m_base + m_slope * pt.z;
		}

		Scalar Transmittance( const Ray& ray, const Scalar dist ) const
		{
			const Scalar tau = m_base * dist + m_slope *
				(ray.origin.z * dist + 0.5 * ray.Dir().z * dist * dist);
			return std::exp( -tau );
		}

	protected:
		~LinearZMedium() override = default;

	public:
		LinearZMedium( const Scalar base, const Scalar slope ) :
		  m_base( base ), m_slope( slope ) {}

		MediumCoefficients GetCoefficients( const Point3& pt ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( SigmaAt( pt ) );
			c.sigma_s = RISEPel( 0.0 );
			c.emission = RISEPel( 0.0 );
			return c;
		}

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = SigmaAt( pt );
			c.sigma_s = 0.0;
			c.emission = 0.0;
			return c;
		}

		const IPhaseFunction* GetPhaseFunction() const override { return nullptr; }
		Scalar SampleDistance(
			const Ray&, const Scalar maxDist, ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		Scalar SampleDistanceNM(
			const Ray&, const Scalar maxDist, const Scalar,
			ISampler&, bool& scattered ) const override
		{
			scattered = false;
			return maxDist;
		}
		RISEPel EvalTransmittance( const Ray& ray, const Scalar dist ) const override
		{
			return RISEPel( Transmittance( ray, dist ) );
		}
		Scalar EvalTransmittanceNM(
			const Ray& ray, const Scalar dist, const Scalar ) const override
		{
			return Transmittance( ray, dist );
		}
		bool IsHomogeneous() const override { return false; }
	};

	void TestInterruptedGlobalMediumUsesSegmentOrigins()
	{
		std::cout << "TestInterruptedGlobalMediumUsesSegmentOrigins" << std::endl;
		std::ostringstream scene;
		scene << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "object_medium", 0.3 );
		AppendBox( scene, "interrupting_box", 2.0, 3.0, "object_medium" );

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "interrupted-global fixture loads" );
		if( job ) {
			LinearZMedium* global = new LinearZMedium( 0.1, 0.05 );
			job->GetScene()->SetGlobalMedium( global );
			safe_release( global );

			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 6.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -1.6 );
			Check( rgbOK && spectralOK, "interrupted-global walk completes" );
			CheckClose( rgb.r, expected, 1e-10,
				"RGB global segments are evaluated from z=0 and z=4" );
			CheckClose( spectral, expected, 1e-10,
				"NM global segments are evaluated from z=0 and z=4" );
		}
		safe_release( job );
	}

	void TestMediumlessInnerStackEntryRestoresOuterMedium()
	{
		std::cout << "TestMediumlessInnerStackEntryRestoresOuterMedium" << std::endl;
		std::ostringstream scene;
		scene << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "outer_medium", 0.1 );
		AppendBox( scene, "outer_box", 10.0, 6.0, "outer_medium" );
		scene <<
			"box_geometry\n{\nname cavity_geometry\nwidth 2\nheight 2\ndepth 2\n}\n"
			"standard_object\n{\nname cavity\ngeometry cavity_geometry\n"
			"position 0 0 6\ncasts_shadows FALSE\n}\n";

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "mediumless-inner fixture loads" );
		if( job ) {
			const IObject* outer = job->GetScene()->GetObjects()->GetItem( "outer_box" );
			const IObject* cavity = job->GetScene()->GetObjects()->GetItem( "cavity" );
			Check( outer && cavity, "mediumless-inner objects resolve" );
			if( outer && cavity ) {
				IORStack mediumStack( 1.0 );
				mediumStack.SetCurrentObject( outer );
				mediumStack.push( 1.0 );
				mediumStack.SetCurrentObject( cavity );
				mediumStack.push( 1.0 );
				const Ray ray( Point3( 0, 0, 6 ), Vector3( 0, 0, 1 ) );
				RISEPel rgb;
				Scalar spectral = 0.0;
				const bool rgbOK = EvaluateShadowMediumTransmittance(
					ray, 6.0, nullptr, nullptr, job->GetScene(), true, rgb, &mediumStack );
				const bool spectralOK = EvaluateShadowMediumTransmittanceNM(
					ray, 6.0, nullptr, nullptr, job->GetScene(), true,
					kWavelengthNM, spectral, &mediumStack );
				const Scalar expected = std::exp( -0.4 );
				Check( rgbOK && spectralOK, "mediumless-inner stack walk completes" );
				CheckClose( rgb.r, expected, 1e-10,
					"RGB cavity suppresses outer medium until its exit" );
				CheckClose( spectral, expected, 1e-10,
					"NM cavity suppresses outer medium until its exit" );
			}
		}
		safe_release( job );
	}

	void TestEnteringMediumlessCavityUsesWorldThenRestoresOuter()
	{
		std::cout << "TestEnteringMediumlessCavityUsesWorldThenRestoresOuter" << std::endl;
		std::ostringstream scene;
		scene << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "outer_medium", 0.1 );
		AppendBox( scene, "outer_box", 10.0, 6.0, "outer_medium" );
		scene <<
			"box_geometry\n{\nname cavity_geometry\nwidth 2\nheight 2\ndepth 2\n}\n"
			"standard_object\n{\nname cavity\ngeometry cavity_geometry\n"
			"position 0 0 6\ncasts_shadows FALSE\n}\n";

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "outside-to-mediumless-cavity fixture loads" );
		if( job ) {
			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 12.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -0.8 );
			Check( rgbOK && spectralOK,
				"mediumless cavity entry and exit update the topology stack" );
			CheckClose( rgb.r, expected, 1e-10,
				"RGB cavity uses world medium then restores outer fog" );
			CheckClose( spectral, expected, 1e-10,
				"NM cavity uses world medium then restores outer fog" );
		}
		safe_release( job );
	}

	void TestSmallNonzeroTransmittanceIsNotHardZeroed()
	{
		std::cout << "TestSmallNonzeroTransmittanceIsNotHardZeroed" << std::endl;
		std::ostringstream scene;
		scene << "RISE ASCII SCENE 7\n";
		AppendHomogeneousMedium( scene, "dense", 16.0 );
		AppendBox( scene, "dense_box", 1.0, 1.0, "dense" );

		IJobPriv* job = LoadScene( scene.str() );
		Check( job != nullptr, "small-transmittance fixture loads" );
		if( job ) {
			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = false;
			bool spectralOK = false;
			EvaluateBoth( job->GetScene(), 2.0, rgb, spectral, rgbOK, spectralOK );
			const Scalar expected = std::exp( -16.0 );
			Check( rgbOK && spectralOK, "optically dense walk completes" );
			Check( rgb.r > 0.0 && spectral > 0.0,
				"transmittance below 1e-6 remains nonzero" );
			CheckClose( rgb.r, expected, 1e-10,
				"RGB walk preserves the exact small transmittance" );
			CheckClose( spectral, expected, 1e-10,
				"NM walk preserves the exact small transmittance" );
		}
		safe_release( job );
	}

	class HalfDensityAccessor :
		public virtual IVolumeAccessor,
		public virtual Reference
	{
	public:
		Scalar GetValue( Scalar, Scalar, Scalar ) const override { return 0.5; }
		Scalar GetValue( int, int, int ) const override { return 0.5; }
		void BindVolume( const IVolume* ) override {}
	protected:
		~HalfDensityAccessor() override = default;
	};

	void TestUnrepresentableRatioStepFailsExplicitly()
	{
		std::cout << "TestUnrepresentableRatioStepFailsExplicitly" << std::endl;
		IJobPriv* job = LoadScene( "RISE ASCII SCENE 7\n" );
		Check( job != nullptr, "non-progressing ratio-step fixture loads" );
		if( job ) {
			HalfDensityAccessor* accessor = new HalfDensityAccessor();
			IPhaseFunction* phase = nullptr;
			RISE_API_CreateIsotropicPhaseFunction( &phase );
			HeterogeneousMedium* medium = new HeterogeneousMedium(
				RISEPel( 1e20, 1e20, 1e20 ), RISEPel( 0, 0, 0 ),
				*phase, *accessor, 2, 2, 2,
				Point3( -1, -1, 1 ), Point3( 1, 1, 3 ) );
			job->GetScene()->SetGlobalMedium( medium );
			safe_release( medium );
			safe_release( phase );
			safe_release( accessor );

			RISEPel rgb;
			Scalar spectral = 0.0;
			bool rgbOK = true;
			bool spectralOK = true;
			EvaluateBoth( job->GetScene(), 4.0, rgb, spectral, rgbOK, spectralOK );
			Check( !rgbOK && !spectralOK,
				"RGB and NM shadow walks reject a ratio step smaller than the ray-parameter ULP" );
			Check( rgb.r == 0.0 && rgb.g == 0.0 && rgb.b == 0.0 && spectral == 0.0,
				"explicit ratio-step failure cannot leak a partial transmittance" );
			if( rgbOK || spectralOK || rgb.r != 0.0 || rgb.g != 0.0 ||
				rgb.b != 0.0 || spectral != 0.0 )
			{
				std::cout << "  rgbOK=" << rgbOK << " spectralOK=" << spectralOK <<
					" rgb=(" << rgb.r << "," << rgb.g << "," << rgb.b <<
					") spectral=" << spectral << std::endl;
			}
		}
		safe_release( job );
	}
}

int main()
{
	TestMoreThanFourNestedMedia();
	TestMoreThanSixteenCrossings();
	TestMoreThanFourNonLifoOverlaps();
	TestResolvableThinSegmentIsNotSkipped();
	TestScaledNearCoincidentBoundariesUseExactOrdering();
	TestOverlappingCsgUnionUsesComposedExactBoundaries();
	TestOriginInsideNestedMediaRestoresOuterStack();
	TestInterruptedGlobalMediumUsesSegmentOrigins();
	TestMediumlessInnerStackEntryRestoresOuterMedium();
	TestEnteringMediumlessCavityUsesWorldThenRestoresOuter();
	TestSmallNonzeroTransmittanceIsNotHardZeroed();
	TestUnrepresentableRatioStepFailsExplicitly();
	std::cout << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
