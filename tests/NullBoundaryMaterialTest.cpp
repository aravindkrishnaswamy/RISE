//////////////////////////////////////////////////////////////////////
//
//  NullBoundaryMaterialTest.cpp - r39/r40 exact null-boundary gates.
//
//  Strong oracles:
//    * Four nested null-boundary crossings equal a miss-ray reference for
//      Pel, NM, and HWSS on both RayCaster and PathTracingIntegrator.
//    * max_path_depth=1 still reaches the environment, proving crossings do
//      not consume path depth.
//    * Binary and transparent shadow modes pass null boundaries with unit
//      transmittance while an ordinary object still blocks.
//    * SeedFromPoint places null containers on enclosure state only, in
//      outer-to-inner order, leaving optical IOR exactly ambient.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
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

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IRadianceMap.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Materials/NullBoundaryMaterial.h"
#include "../src/Library/Rendering/AOVBuffers.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/RuntimeContext.h"

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

	void Check( const bool condition, const char* label )
	{
		if( condition ) {
			++passed;
		} else {
			++failed;
			std::cout << "FAIL: " << label << std::endl;
		}
	}

	bool SamePel( const RISEPel& a, const RISEPel& b )
	{
		return a.r == b.r && a.g == b.g && a.b == b.b;
	}

	const char* SceneText()
	{
		return
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\n}\n"
			"null_boundary_material\n{\nname medium_boundary\n}\n"
			"sphere_geometry\n{\nname outer_geometry\nradius 1.5\n}\n"
			"sphere_geometry\n{\nname inner_geometry\nradius 0.75\n}\n"
			"box_geometry\n{\nname blocker_geometry\nwidth 1\nheight 1\ndepth 1\n}\n"
			"box_geometry\n{\nname thin_boundary_geometry\nwidth 1\nheight 1\ndepth 0.000002\n}\n"
			"box_geometry\n{\nname thin_blocker_geometry\nwidth 1\nheight 1\ndepth 0.000002\n}\n"
			"standard_object\n{\nname outer_boundary\ngeometry outer_geometry\n"
			"material medium_boundary\nposition 0 0 3\ncasts_shadows TRUE\n}\n"
			"standard_object\n{\nname inner_boundary\ngeometry inner_geometry\n"
			"material medium_boundary\nposition 0 0 3\ncasts_shadows TRUE\n}\n"
			"standard_object\n{\nname medium_container\ngeometry outer_geometry\n"
			"material medium_boundary\nposition -3 0 3\ncasts_shadows TRUE\n}\n"
			"standard_object\n{\nname ordinary_blocker\ngeometry blocker_geometry\n"
			"material none\nposition 3 0 3\ncasts_shadows TRUE\n}\n"
			"standard_object\n{\nname thin_boundary\ngeometry thin_boundary_geometry\n"
			"material medium_boundary\nposition 6 0 3\ncasts_shadows TRUE\n}\n"
			"standard_object\n{\nname thin_blocker\ngeometry thin_blocker_geometry\n"
			"material none\nposition 6 0 3.00002\ncasts_shadows TRUE\n}\n";
	}

	class DerivedNullBoundaryMaterial : public NullBoundaryMaterial
	{
	};

	class StreamCountingSampler : public ISampler
	{
		const RandomNumberGenerator& random;
		unsigned int streamStarts;

	public:
		explicit StreamCountingSampler( const RandomNumberGenerator& random_ ) :
			random( random_ ), streamStarts( 0 ) {}

		Scalar Get1D() override { return random.CanonicalRandom(); }
		Point2 Get2D() override
		{
			return Point2( random.CanonicalRandom(), random.CanonicalRandom() );
		}
		void StartStream( int ) override { ++streamStarts; }
		unsigned int StreamStarts() const { return streamStarts; }
	};

	class ConstantAdditiveMedium :
		public virtual IMedium,
		public virtual Reference
	{
	public:
		MediumCoefficients GetCoefficients( const Point3& ) const override
		{
			MediumCoefficients c;
			c.sigma_t = RISEPel( 0, 0, 0 );
			c.sigma_s = RISEPel( 0, 0, 0 );
			c.emission = RISEPel( 1, 1, 1 );
			return c;
		}

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3&, const Scalar ) const override
		{
			MediumCoefficientsNM c;
			c.sigma_t = 0.0;
			c.sigma_s = 0.0;
			c.emission = 1.0;
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
		RISEPel EvalTransmittance( const Ray&, const Scalar ) const override
		{
			return RISEPel( 1, 1, 1 );
		}
		Scalar EvalTransmittanceNM(
			const Ray&, const Scalar, const Scalar ) const override { return 1.0; }
		bool IsHomogeneous() const override { return true; }

	protected:
		~ConstantAdditiveMedium() override = default;
	};

	struct Fixture
	{
		IJobPriv* job;
		IRayCaster* caster;
		PathTracingIntegrator* integrator;
		IPainter* environmentPainter;
		IRadianceMap* environment;
		std::filesystem::path scenePath;

		Fixture() : job( 0 ), caster( 0 ), integrator( 0 ),
			environmentPainter( 0 ), environment( 0 ) {}

		~Fixture()
		{
			safe_release( environment );
			safe_release( environmentPainter );
			safe_release( integrator );
			safe_release( caster );
			safe_release( job );
			if( !scenePath.empty() ) std::filesystem::remove( scenePath );
		}

		bool Initialize()
		{
			char filename[128];
			std::snprintf( filename, sizeof(filename),
				"rise_null_boundary_%d.RISEscene", static_cast<int>( ::getpid() ) );
			scenePath = std::filesystem::temp_directory_path() / filename;
			{
				std::ofstream output( scenePath );
				if( !output.is_open() ) return false;
				output << SceneText();
			}

			if( !RISE_CreateJobPriv( &job ) || !job ||
				!job->LoadAsciiSceneViaCst( scenePath.string().c_str() ) ) return false;
			IObjectPriv* mediumContainer =
				job->GetObjects()->GetItem( "medium_container" );
			ConstantAdditiveMedium* additiveMedium = new ConstantAdditiveMedium();
			if( !mediumContainer ||
				!mediumContainer->AssignInteriorMedium( *additiveMedium ) ) {
				safe_release( additiveMedium );
				return false;
			}
			safe_release( additiveMedium );
			job->GetScene()->GetObjects()->PrepareForRendering();
			IShader* shader = job->GetShaders()->GetItem( "global" );
			if( !shader || !RISE_API_CreateRayCaster(
				&caster, true, 1, *shader, true ) || !caster ) return false;
			caster->AttachScene( job->GetScene() );

			if( !RISE_API_CreateUniformColorPainter(
				&environmentPainter, RISEPel( 0.25, 0.5, 1.0 ),
				eSpectrumKind_Unbounded ) || !environmentPainter ) return false;
			if( !RISE_API_CreateRadianceMap(
				&environment, *environmentPainter, 1.0 ) || !environment ) return false;

			integrator = new PathTracingIntegrator(
				ManifoldSolverConfig(), StabilityConfig() );
			integrator->SetMaxPathDepth( 1 );
			return true;
		}

		RayCaster* ConcreteCaster() const
		{
			return dynamic_cast<RayCaster*>( caster );
		}

		RISEPel CastPel( const Ray& ray, const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			const RasterizerState rast = { 0, 0 };
			IRayCaster::RAY_STATE state;
			RISEPel value( 0, 0, 0 );
			caster->CastRay( rc, rast, ray, value, state, 0, environment );
			return value;
		}

		Scalar CastNM( const Ray& ray, const Scalar nm, const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			const RasterizerState rast = { 0, 0 };
			IRayCaster::RAY_STATE state;
			Scalar value = 0.0;
			caster->CastRayNM( rc, rast, ray, value, state, nm, 0, environment );
			return value;
		}

		PixelAOV CastNMAOV( const Ray& ray, const Scalar nm,
			const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			PixelAOV aov;
			rc.pAOV = &aov;
			const RasterizerState rast = { 0, 0 };
			IRayCaster::RAY_STATE state;
			Scalar value = 0.0;
			caster->CastRayNM( rc, rast, ray, value, state, nm, 0, environment );
			return aov;
		}

		PixelAOV CastNMWithCapturedAOV( const Ray& ray, const Scalar nm,
			const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			PixelAOV aov;
			aov.primaryDepthCaptured = true;
			aov.depth = 7.0;
			rc.pAOV = &aov;
			const RasterizerState rast = { 0, 0 };
			IRayCaster::RAY_STATE state;
			Scalar value = 0.0;
			caster->CastRayNM( rc, rast, ray, value, state, nm, 0, environment );
			return aov;
		}

		void CastHWSS( const Ray& ray, Scalar values[SampledWavelengths::N],
			SampledWavelengths wavelengths, const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			const RasterizerState rast = { 0, 0 };
			IRayCaster::RAY_STATE state;
			IORStack stack( 1.0 );
			caster->CastRayHWSS( rc, rast, ray, values, state, wavelengths,
				0, environment, stack );
		}

		RISEPel TracePel( const Ray& ray, const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			return integrator->IntegrateRay( rc, rast, ray, *job->GetScene(),
				*caster, sampler, environment, 0 );
		}

		Scalar TraceNM( const Ray& ray, const Scalar nm, const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			return integrator->IntegrateRayNM( rc, rast, ray, nm,
				*job->GetScene(), *caster, sampler, environment, 0 );
		}

		PixelAOV TraceNMAOV( const Ray& ray, const Scalar nm,
			const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			PixelAOV aov;
			integrator->IntegrateRayNM( rc, rast, ray, nm,
				*job->GetScene(), *caster, sampler, environment, &aov );
			return aov;
		}

		unsigned int TraceNMStreamStarts(
			const Ray& ray,
			const Scalar nm,
			const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			StreamCountingSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			integrator->IntegrateRayNM( rc, rast, ray, nm,
				*job->GetScene(), *caster, sampler, environment, 0 );
			return sampler.StreamStarts();
		}

		void TraceHWSS( const Ray& ray, Scalar values[SampledWavelengths::N],
			SampledWavelengths wavelengths, const unsigned int seed ) const
		{
			RandomNumberGenerator rng( seed );
			RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
			IndependentSampler sampler( rng );
			const RasterizerState rast = { 0, 0 };
			integrator->IntegrateRayHWSS( rc, rast, ray, wavelengths,
				*job->GetScene(), *caster, sampler, environment, values, 0 );
		}
	};

	void TestExactTypeAndParser( const Fixture& fixture )
	{
		IMaterial* boundary = fixture.job->GetMaterials()->GetItem( "medium_boundary" );
		IMaterial* none = fixture.job->GetMaterials()->GetItem( "none" );
		Check( boundary && IsExactNullBoundaryMaterial( boundary ),
			"null_boundary_material parser creates the exact transport type" );
		Check( none && !IsExactNullBoundaryMaterial( none ),
			"the terminating none material remains distinct" );
		Check( boundary && MaterialIntrospection::GetTypeName( *boundary ) ==
			String( "Null Medium Boundary" ),
			"scene editor identifies the null boundary material" );

		DerivedNullBoundaryMaterial* derived = new DerivedNullBoundaryMaterial();
		derived->addref();
		Check( !IsExactNullBoundaryMaterial( derived ),
			"a subclass does not inherit exact null-boundary semantics" );
		derived->release();
	}

	void TestSeedingIsEnclosureOnly( const Fixture& fixture )
	{
		IObjectPriv* outer = fixture.job->GetObjects()->GetItem( "outer_boundary" );
		IObjectPriv* inner = fixture.job->GetObjects()->GetItem( "inner_boundary" );
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint(
			stack, Point3( 0, 0, 3 ), *fixture.job->GetScene() );
		std::vector<const IObject*> enclosures;
		stack.AppendObjectStack( enclosures );
		Check( outer && inner && enclosures.size() == 2 &&
			enclosures[0] == outer && enclosures[1] == inner,
			"seeding orders nested null containers outer-to-inner" );
		Check( stack.top() == 1.0 && !stack.containsCurrent() &&
			stack.containsCurrentEnclosure() && stack.topObject() == inner,
			"null seeding leaves optical IOR ambient and seeds enclosure only" );
		Check( stack.DebugOpticalStackIsEnclosureSubsequence(),
			"null seeding preserves the optical/enclosure subsequence invariant" );
	}

	void TestTransportEqualsMissReference( const Fixture& fixture )
	{
		const Vector3 direction = Vector3Ops::Normalize( Vector3( 0.1, 0.0, 1.0 ) );
		const Ray through( Point3( 0, 0, 0 ), direction );
		const Ray miss( Point3( 5, 0, 0 ), direction );

		const RISEPel casterPelReference = fixture.CastPel( miss, 11 );
		const RISEPel casterPelThrough = fixture.CastPel( through, 11 );
		Check( SamePel( casterPelThrough, casterPelReference ),
			"RayCaster Pel crosses four null boundaries with unit response" );

		const Scalar casterNMReference = fixture.CastNM( miss, 550.0, 12 );
		const Scalar casterNMThrough = fixture.CastNM( through, 550.0, 12 );
		Check( casterNMThrough == casterNMReference,
			"RayCaster NM crosses four null boundaries with unit response" );

		const RISEPel ptPelReference = fixture.TracePel( miss, 13 );
		const RISEPel ptPelThrough = fixture.TracePel( through, 13 );
		Check( SamePel( ptPelThrough, ptPelReference ),
			"PathTracingIntegrator Pel crosses null boundaries without consuming max-depth=1" );

		const Scalar ptNMReference = fixture.TraceNM( miss, 550.0, 14 );
		const Scalar ptNMThrough = fixture.TraceNM( through, 550.0, 14 );
		Check( ptNMThrough == ptNMReference,
			"PathTracingIntegrator NM crosses null boundaries without consuming max-depth=1" );
		Check( fixture.TraceNMStreamStarts( through, 550.0, 17 ) == 2,
			"PathTracingIntegrator adds no sampler-stream restart across four null crossings" );

		const SampledWavelengths wavelengths =
			SampledWavelengths::SampleEquidistant( 0.2, 400.0, 700.0 );
		Scalar casterHWReference[SampledWavelengths::N];
		Scalar casterHWThrough[SampledWavelengths::N];
		fixture.CastHWSS( miss, casterHWReference, wavelengths, 15 );
		fixture.CastHWSS( through, casterHWThrough, wavelengths, 15 );
		Scalar ptHWReference[SampledWavelengths::N];
		Scalar ptHWThrough[SampledWavelengths::N];
		fixture.TraceHWSS( miss, ptHWReference, wavelengths, 16 );
		fixture.TraceHWSS( through, ptHWThrough, wavelengths, 16 );
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			Check( casterHWThrough[i] == casterHWReference[i],
				"RayCaster HWSS null-boundary result equals miss reference" );
			Check( ptHWThrough[i] == ptHWReference[i],
				"PathTracingIntegrator HWSS null-boundary result equals miss reference" );
		}
	}

	void TestShadowTransparency( const Fixture& fixture )
	{
		RayCaster* caster = fixture.ConcreteCaster();
		Check( caster != 0, "fixture exposes the concrete RayCaster shadow surface" );
		if( !caster ) return;

		const Ray nullChain( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
		const Ray blocked( Point3( 3, 0, 0 ), Vector3( 0, 0, 1 ) );
		Check( !caster->CastShadowRay( nullChain, 6.0 ),
			"binary shadow ray ignores casts_shadows on exact null boundaries" );
		Check( caster->CastShadowRay( blocked, 6.0 ),
			"ordinary casts_shadows object still blocks the binary shadow ray" );

		RISEPel transmittance;
		caster->SetTransparentShadows( false );
		Check( !caster->CastShadowRayAuto(
			nullChain, 6.0, true, 550.0, transmittance ) &&
			SamePel( transmittance, RISEPel( 1, 1, 1 ) ),
			"null boundary is unit-transparent with transparent_shadows disabled" );
		caster->SetTransparentShadows( true );
		Check( !caster->CastShadowRayAuto(
			nullChain, 6.0, true, 550.0, transmittance ) &&
			SamePel( transmittance, RISEPel( 1, 1, 1 ) ),
			"null boundary is unit-transparent with transparent_shadows enabled" );

		const Ray thinThenBlocked( Point3( 6, 0, 0 ), Vector3( 0, 0, 1 ) );
		Check( caster->CastShadowRayAuto(
			thinThenBlocked, 4.0, true, 550.0, transmittance ),
			"exact null traversal does not step over a resolvably close blocker" );
	}

	void TestTraversalChangesAndRestoresMedium( const Fixture& fixture )
	{
		const Ray through( Point3( -3, 0, 0 ), Vector3( 0, 0, 1 ) );
		const Ray miss( Point3( -6, 0, 0 ), Vector3( 0, 0, 1 ) );
		const Scalar casterDelta =
			fixture.CastNM( through, 550.0, 21 ) - fixture.CastNM( miss, 550.0, 21 );
		const Scalar ptDelta =
			fixture.TraceNM( through, 550.0, 22 ) - fixture.TraceNM( miss, 550.0, 22 );
		if( std::fabs( casterDelta - 3.0 ) > 1e-10 ) {
			std::cout << "  caster chord delta=" << casterDelta << std::endl;
		}
		Check( std::fabs( casterDelta - 3.0 ) <= 1e-10,
			"RayCaster enters the null enclosure for exactly its three-unit chord" );
		Check( std::fabs( ptDelta - 3.0 ) <= 1e-10,
			"PathTracingIntegrator enters then restores the null-enclosed medium" );

		const Ray startsInside( Point3( -3, 0, 3 ), Vector3( 0, 0, 1 ) );
		const Scalar casterInsideDelta = fixture.CastNM(
			startsInside, 550.0, 23 ) - fixture.CastNM( miss, 550.0, 23 );
		const Scalar ptInsideDelta = fixture.TraceNM(
			startsInside, 550.0, 24 ) - fixture.TraceNM( miss, 550.0, 24 );
		Check( std::fabs( casterInsideDelta - 1.5 ) <= 1e-10,
			"RayCaster root seeding starts inside the null-enclosed medium" );
		Check( std::fabs( ptInsideDelta - 1.5 ) <= 1e-10,
			"PathTracingIntegrator root seeding starts inside the null-enclosed medium" );

		const SampledWavelengths wavelengths =
			SampledWavelengths::SampleEquidistant( 0.3, 400.0, 700.0 );
		Scalar casterInsideHW[SampledWavelengths::N];
		Scalar casterMissHW[SampledWavelengths::N];
		fixture.CastHWSS( startsInside, casterInsideHW, wavelengths, 27 );
		fixture.CastHWSS( miss, casterMissHW, wavelengths, 27 );
		for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
			Check( std::fabs( casterInsideHW[i] - casterMissHW[i] - 1.5 ) <= 1e-10,
				"RayCaster HWSS root seeding starts inside the null-enclosed medium" );
		}
	}

	void TestDifferentialsAndPrimaryAOV( const Fixture& fixture )
	{
		Ray differentialRay( Point3( 1, 2, 3 ), Vector3( 0, 0, 1 ) );
		differentialRay.hasDifferentials = true;
		differentialRay.diffs.rxOrigin = Vector3( 1, 0, 0 );
		differentialRay.diffs.ryOrigin = Vector3( 0, 2, 0 );
		differentialRay.diffs.rxDir = Vector3( 0.25, 0, 0 );
		differentialRay.diffs.ryDir = Vector3( 0, 0.5, 0 );
		const Ray continued = ContinueExactNullBoundaryRay( differentialRay, 4.0 );
		Check( continued.hasDifferentials &&
			Point3Ops::AreEqual( continued.origin, Point3( 1, 2, 7 ), 0.0 ) &&
			Vector3Ops::AreEqual( continued.diffs.rxOrigin, Vector3( 2, 0, 0 ), 0.0 ) &&
			Vector3Ops::AreEqual( continued.diffs.ryOrigin, Vector3( 0, 4, 0 ), 0.0 ),
			"exact null continuation free-propagates ray differentials" );

		const Ray thinThenSurface( Point3( 6, 0, 0 ), Vector3( 0, 0, 1 ) );
		const PixelAOV casterAOV = fixture.CastNMAOV( thinThenSurface, 550.0, 25 );
		const PixelAOV ptAOV = fixture.TraceNMAOV( thinThenSurface, 550.0, 26 );
		if( !(casterAOV.primaryDepthCaptured && casterAOV.valid &&
			casterAOV.depth > 3.00001 && casterAOV.depth < 3.00003) ) {
			std::cout << "  caster AOV captured=" << casterAOV.primaryDepthCaptured
				<< " valid=" << casterAOV.valid << " depth=" << casterAOV.depth
				<< std::endl;
		}
		Check( casterAOV.primaryDepthCaptured && casterAOV.valid &&
			casterAOV.depth > 3.00001 && casterAOV.depth < 3.00003,
			"RayCaster primary AOV describes the surface behind a null boundary" );
		Check( ptAOV.primaryDepthCaptured && ptAOV.valid &&
			ptAOV.depth > 3.00001 && ptAOV.depth < 3.00003,
			"PathTracingIntegrator primary AOV describes the surface behind a null boundary" );

		const PixelAOV retainedAOV = fixture.CastNMWithCapturedAOV(
			Ray( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) ), 550.0, 28 );
		Check( retainedAOV.primaryDepthCaptured && retainedAOV.depth == 7.0,
			"secondary RayCaster null traversal cannot alter captured primary depth" );
	}
}

int main()
{
	Fixture fixture;
	Check( fixture.Initialize(), "null-boundary fixture initializes" );
	if( fixture.job && fixture.caster && fixture.integrator && fixture.environment ) {
		TestExactTypeAndParser( fixture );
		TestSeedingIsEnclosureOnly( fixture );
		TestTransportEqualsMissReference( fixture );
		TestShadowTransparency( fixture );
		TestTraversalChangesAndRestoresMedium( fixture );
		TestDifferentialsAndPrimaryAOV( fixture );
	}

	std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
	return failed == 0 ? 0 : 1;
}
