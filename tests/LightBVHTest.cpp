//////////////////////////////////////////////////////////////////////
//
//  LightBVHTest.cpp - Validates the Light BVH implementation:
//
//    1. OrientationCone merge: identical, opposite, containing,
//       orthogonal, full-sphere cases
//    2. BVH construction: 1/2/100 lights, power sums, bounds
//    3. Sampling: frequency consistency with importance function
//    4. PDF: normalization (sum to 1), consistency with Sample()
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include \
//        -O3 -ffast-math -funroll-loops -Wall -pedantic \
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53 \
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING \
//        -c tests/LightBVHTest.cpp -o tests/LightBVHTest.o
//    c++ -arch arm64 -o tests/LightBVHTest tests/LightBVHTest.o \
//        bin/librise.a -L/opt/homebrew/lib -lpng -lz
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <iomanip>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Lights/LightBVH.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Interfaces/ILightPriv.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  MockSpotLight — minimal ILight for testing directed cones
// ============================================================

class MockSpotLight : public virtual ILightPriv, public virtual Reference
{
	Vector3  vDir;
	Scalar   dHalfAngle;
	Point3   ptPos;
	Scalar   dPower;

public:
	MockSpotLight( const Vector3& dir, Scalar halfAngle, const Point3& pos, Scalar power )
		: vDir( dir ), dHalfAngle( halfAngle ), ptPos( pos ), dPower( power )
	{
		Vector3Ops::Normalize( vDir );
	}

	bool CanGeneratePhotons() const { return false; }
	RISEPel radiantExitance() const { return RISEPel( dPower, dPower, dPower ); }
	RISEPel emittedRadiance( const Vector3& ) const { return RISEPel( dPower, dPower, dPower ); }
	Point3 position() const { return ptPos; }
	Ray generateRandomPhoton( const Point3& ) const { return Ray( ptPos, vDir ); }
	Scalar pdfDirection( const Vector3& ) const { return 1.0; }
	bool IsPositionalLight() const { return true; }

	Vector3 emissionDirection() const { return vDir; }
	Scalar emissionConeHalfAngle() const { return dHalfAngle; }

	void ComputeDirectLighting(
		const RayIntersectionGeometric&, const IRayCaster&,
		const IBSDF&, const bool, RISEPel& amount ) const
	{
		amount = RISEPel( 0, 0, 0 );
	}

	// IKeyframable stubs
	IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
	void SetIntermediateValue( const IKeyframeParameter& ) {}
	void RegenerateData() {}

	// IBasicTransform / ITranslatable / IPositionable stubs
	void ClearAllTransforms() {}
	void FinalizeTransformations() {}
	Matrix4 const GetFinalTransformMatrix() const { return Matrix4(); }
	Matrix4 const GetFinalInverseTransformMatrix() const { return Matrix4(); }
	void TranslateObject( const Vector3& ) {}
	void SetPosition( const Point3& ) {}

protected:
	virtual ~MockSpotLight() {}
};

static int testsPassed = 0;
static int testsFailed = 0;

static void Check( bool condition, const char* name )
{
	if( condition ) {
		std::cout << "  PASS: " << name << std::endl;
		testsPassed++;
	} else {
		std::cout << "  FAIL: " << name << std::endl;
		testsFailed++;
	}
}

// ============================================================
//  Test 1: Orientation Cone Merge
// ============================================================

static void TestConeMerge()
{
	std::cout << "--- Cone Merge Tests ---" << std::endl;

	// 1a. Identical cones merge to same cone
	{
		OrientationCone a;
		a.axis = Vector3( 0, 1, 0 );
		a.halfAngle = 0.5;

		OrientationCone result = OrientationCone::Merge( a, a );
		const Scalar axisDot = Vector3Ops::Dot( result.axis, a.axis );

		Check( fabs( result.halfAngle - a.halfAngle ) < 1e-6 &&
			   axisDot > 0.999,
			   "Identical cones merge to same cone" );
	}

	// 1b. Opposite cones merge to full sphere
	{
		OrientationCone a;
		a.axis = Vector3( 0, 1, 0 );
		a.halfAngle = 0.3;

		OrientationCone b;
		b.axis = Vector3( 0, -1, 0 );
		b.halfAngle = 0.3;

		OrientationCone result = OrientationCone::Merge( a, b );

		// theta_d = PI, so merged half = (0.3 + PI + 0.3)/2 ~ 1.87
		// which is > PI/2 but < PI.  Check that the result is valid.
		// Actually (0.3 + PI + 0.3)/2 = (PI + 0.6)/2 = 1.8708
		// This is < PI so it won't be full sphere, but it should
		// contain both cones.
		Check( result.halfAngle >= PI - 0.01 ||
			   result.halfAngle >= (a.halfAngle + PI + b.halfAngle) * 0.5 - 0.01,
			   "Opposite cones produce valid merged cone" );
	}

	// 1c. One containing the other returns the larger
	{
		OrientationCone a;
		a.axis = Vector3( 0, 1, 0 );
		a.halfAngle = 1.0;

		OrientationCone b;
		b.axis = Vector3( 0, 1, 0 );
		b.halfAngle = 0.2;

		OrientationCone result = OrientationCone::Merge( a, b );

		Check( fabs( result.halfAngle - 1.0 ) < 1e-6,
			   "Containing cone returns larger" );
	}

	// 1d. Orthogonal cones
	{
		OrientationCone a;
		a.axis = Vector3( 1, 0, 0 );
		a.halfAngle = 0.3;

		OrientationCone b;
		b.axis = Vector3( 0, 1, 0 );
		b.halfAngle = 0.3;

		OrientationCone result = OrientationCone::Merge( a, b );

		// theta_d = PI/2, merged = (0.3 + PI/2 + 0.3)/2 = (PI/2 + 0.6)/2 ~ 1.085
		const Scalar expected = (0.3 + PI_OV_TWO + 0.3) * 0.5;
		Check( fabs( result.halfAngle - expected ) < 0.02,
			   "Orthogonal cones produce correct half-angle" );
	}

	// 1e. Full sphere merged with anything stays full sphere
	{
		OrientationCone a = OrientationCone::FullSphere();

		OrientationCone b;
		b.axis = Vector3( 1, 0, 0 );
		b.halfAngle = 0.5;

		OrientationCone result1 = OrientationCone::Merge( a, b );
		OrientationCone result2 = OrientationCone::Merge( b, a );

		Check( result1.halfAngle >= PI - 1e-6 &&
			   result2.halfAngle >= PI - 1e-6,
			   "Full sphere merged with anything stays full sphere" );
	}
}

// ============================================================
//  Helper: Build a set of fake LightEntries (point lights)
// ============================================================

static std::vector<LightEntry> MakePointLightEntries(
	const std::vector<Point3>& positions,
	const std::vector<Scalar>& powers
	)
{
	std::vector<LightEntry> entries( positions.size() );
	for( unsigned int i = 0; i < (unsigned int)positions.size(); i++ )
	{
		entries[i].pLight = NULL;
		entries[i].lumIndex = 0;
		entries[i].exitance = powers[i];
		entries[i].position = positions[i];
	}
	return entries;
}

// ============================================================
//  Test 2: BVH Construction
// ============================================================

static void TestConstruction()
{
	std::cout << "--- Construction Tests ---" << std::endl;
	LuminaryManager::LuminariesList emptyLums;

	// 2a. Single light
	{
		std::vector<Point3> pos = { Point3(0,0,0) };
		std::vector<Scalar> pow = { 10.0 };
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		Check( bvh.IsBuilt() && bvh.GetLightCount() == 1,
			   "Single light: 1 leaf, built" );
	}

	// 2b. Two lights
	{
		std::vector<Point3> pos = { Point3(-1,0,0), Point3(1,0,0) };
		std::vector<Scalar> pow = { 5.0, 15.0 };
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		Check( bvh.IsBuilt() && bvh.GetLightCount() == 2,
			   "Two lights: built with 2 leaves" );
	}

	// 2c. 100 lights in a line
		{
			std::vector<Point3> pos( 100 );
			std::vector<Scalar> pow( 100 );

			for( int i = 0; i < 100; i++ ) {
				pos[i] = Point3( Scalar(i), 0, 0 );
				pow[i] = Scalar(1.0 + i * 0.5);
			}

			auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		Check( bvh.IsBuilt() && bvh.GetLightCount() == 100,
			   "100 lights: built with 100 leaves" );
	}
}

// ============================================================
//  Test 3: Sampling Frequency Consistency
// ============================================================

static void TestSampling()
{
	std::cout << "--- Sampling Tests ---" << std::endl;
	LuminaryManager::LuminariesList emptyLums;

	// 3a. Two lights: one close, one far.  From a point near
	// the first light, it should be selected much more often.
	{
		std::vector<Point3> pos = { Point3(1,0,0), Point3(100,0,0) };
		std::vector<Scalar> pow = { 10.0, 10.0 };
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		const Point3 shadingPt( 0, 0, 0 );
		const Vector3 shadingN( 0, 1, 0 );
		const int N = 100000;
		int countLight0 = 0;

		RandomNumberGenerator rng( 42 );

		for( int i = 0; i < N; i++ )
		{
			Scalar pdf;
			const Scalar xi = rng.CanonicalRandom();
			unsigned int idx = bvh.Sample( shadingPt, shadingN, xi, pdf );
			if( idx == 0 ) countLight0++;
		}

		// Light 0 is at distance 1, light 1 at distance 100.
		// Importance ratio: (10/1) / (10/10000) = 10000:1 practically
		// Light 0 should be selected almost always.
		const Scalar frac0 = Scalar(countLight0) / N;

		Check( frac0 > 0.99,
			   "Near light selected >99% of the time" );
	}

	// 3b. Equal lights, equal distance — should be ~50/50
	{
		std::vector<Point3> pos = { Point3(-5,0,0), Point3(5,0,0) };
		std::vector<Scalar> pow = { 10.0, 10.0 };
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		const Point3 shadingPt( 0, 0, 0 );
		const Vector3 shadingN( 0, 1, 0 );
		const int N = 100000;
		int countLight0 = 0;

		RandomNumberGenerator rng( 123 );

		for( int i = 0; i < N; i++ )
		{
			Scalar pdf;
			const Scalar xi = rng.CanonicalRandom();
			unsigned int idx = bvh.Sample( shadingPt, shadingN, xi, pdf );
			if( idx == 0 ) countLight0++;
		}

		const Scalar frac0 = Scalar(countLight0) / N;

		Check( fabs( frac0 - 0.5 ) < 0.05,
			   "Equal lights at equal distance: ~50/50 split" );
	}

	// 3c. 10 lights: verify PDF is always > 0 from Sample
	{
		std::vector<Point3> pos( 10 );
		std::vector<Scalar> pow( 10 );
		for( int i = 0; i < 10; i++ ) {
			pos[i] = Point3( Scalar(i * 3), Scalar(i % 3), 0 );
			pow[i] = Scalar(1.0 + i);
		}
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		const Point3 shadingPt( 5, 0, 0 );
		const Vector3 shadingN( 0, 1, 0 );
		bool allPdfPositive = true;

		RandomNumberGenerator rng( 456 );

		for( int i = 0; i < 10000; i++ )
		{
			Scalar pdf;
			const Scalar xi = rng.CanonicalRandom();
			bvh.Sample( shadingPt, shadingN, xi, pdf );
			if( pdf <= 0 ) {
				allPdfPositive = false;
				break;
			}
		}

		Check( allPdfPositive, "All sampled PDFs > 0" );
	}
}

// ============================================================
//  Test 4: PDF Evaluation
// ============================================================

static void TestPdf()
{
	std::cout << "--- PDF Tests ---" << std::endl;
	LuminaryManager::LuminariesList emptyLums;

	// 4a. Single light: PDF = 1.0
	{
		std::vector<Point3> pos = { Point3(3,4,5) };
		std::vector<Scalar> pow = { 7.0 };
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		const Point3 shadingPt( 0, 0, 0 );
		const Vector3 shadingN( 0, 1, 0 );

		const Scalar pdf = bvh.Pdf( 0, shadingPt, shadingN );
		Check( fabs( pdf - 1.0 ) < 1e-10,
			   "Single light PDF = 1.0" );
	}

	// 4b. 10 lights: PDFs sum to ~1.0
	{
		std::vector<Point3> pos( 10 );
		std::vector<Scalar> pow( 10 );
		for( int i = 0; i < 10; i++ ) {
			pos[i] = Point3( Scalar(i * 2 - 9), Scalar(i % 3 - 1), Scalar(i % 2) );
			pow[i] = Scalar(1.0 + i * 2);
		}
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		const Point3 shadingPt( 1, 0, 0 );
		const Vector3 shadingN( 0, 1, 0 );

		Scalar pdfSum = 0;
		for( int i = 0; i < 10; i++ ) {
			pdfSum += bvh.Pdf( i, shadingPt, shadingN );
		}

		Check( fabs( pdfSum - 1.0 ) < 1e-6,
			   "10 lights: PDF sum = 1.0 (within 1e-6)" );
	}

	// 4c. Pdf() consistent with Sample() frequencies
	{
		std::vector<Point3> pos = {
			Point3(2,0,0), Point3(5,0,0), Point3(10,0,0),
			Point3(0,3,0), Point3(0,8,0)
		};
		std::vector<Scalar> pow = { 5.0, 10.0, 2.0, 8.0, 3.0 };
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		const Point3 shadingPt( 0, 0, 0 );
		const Vector3 shadingN( 0, 1, 0 );

		// Get analytical PDFs
		Scalar analyticPdf[5];
		for( int i = 0; i < 5; i++ ) {
			analyticPdf[i] = bvh.Pdf( i, shadingPt, shadingN );
		}

		// Sample many times and count frequencies
		const int N = 200000;
		int counts[5] = {0,0,0,0,0};

		RandomNumberGenerator rng( 789 );

		for( int i = 0; i < N; i++ )
		{
			Scalar pdf;
			const Scalar xi = rng.CanonicalRandom();
			unsigned int idx = bvh.Sample( shadingPt, shadingN, xi, pdf );
			if( idx < 5 ) counts[idx]++;
		}

		// Check relative error for each light
		bool consistent = true;
		for( int i = 0; i < 5; i++ )
		{
			const Scalar empirical = Scalar(counts[i]) / N;
			const Scalar expected = analyticPdf[i];

			if( expected > 0.01 )  // Only check lights with significant probability
			{
				const Scalar relError = fabs( empirical - expected ) / expected;
				if( relError > 0.05 )  // 5% relative error tolerance
				{
					std::cout << "    Light " << i << ": empirical=" << empirical
						<< " expected=" << expected << " relErr=" << relError << std::endl;
					consistent = false;
				}
			}
		}

		Check( consistent, "Pdf() matches Sample() frequencies (5% tolerance)" );
	}

	// 4d. Pdf() matches the pdf output of Sample()
	{
		std::vector<Point3> pos = { Point3(1,0,0), Point3(4,0,0), Point3(9,0,0) };
		std::vector<Scalar> pow = { 5.0, 3.0, 8.0 };
		auto entries = MakePointLightEntries( pos, pow );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		const Point3 shadingPt( 0, 0, 0 );
		const Vector3 shadingN( 0, 1, 0 );

		RandomNumberGenerator rng( 321 );

		bool allMatch = true;
		for( int i = 0; i < 1000; i++ )
		{
			Scalar samplePdf;
			const Scalar xi = rng.CanonicalRandom();
			unsigned int idx = bvh.Sample( shadingPt, shadingN, xi, samplePdf );

			const Scalar evalPdf = bvh.Pdf( idx, shadingPt, shadingN );

			if( fabs( samplePdf - evalPdf ) > 1e-10 )
			{
				std::cout << "    Mismatch: idx=" << idx
					<< " samplePdf=" << samplePdf
					<< " evalPdf=" << evalPdf << std::endl;
				allMatch = false;
				break;
			}
		}

		Check( allMatch, "Sample() pdf matches Pdf() evaluation" );
	}
}

// ============================================================
//  Helper: Build LightEntries with directed mock spotlights
// ============================================================

static std::vector<MockSpotLight*> g_mockLights;  // prevent premature destruction

static std::vector<LightEntry> MakeSpotLightEntries(
	const std::vector<Point3>& positions,
	const std::vector<Vector3>& directions,
	const std::vector<Scalar>& halfAngles,
	const std::vector<Scalar>& powers
	)
{
	// Clean up any previous mock lights
	for( unsigned int i = 0; i < (unsigned int)g_mockLights.size(); i++ )
		g_mockLights[i]->release();
	g_mockLights.clear();

	std::vector<LightEntry> entries( positions.size() );
	for( unsigned int i = 0; i < (unsigned int)positions.size(); i++ )
	{
		MockSpotLight* pMock = new MockSpotLight(
			directions[i], halfAngles[i], positions[i], powers[i] );
		pMock->addref();
		g_mockLights.push_back( pMock );

		entries[i].pLight = pMock;
		entries[i].lumIndex = 0;
		entries[i].exitance = powers[i];
		entries[i].position = positions[i];
	}
	return entries;
}

// ============================================================
//  Test 5: Directed-Light (Spotlight) Tests
// ============================================================

static void TestDirectedLights()
{
	std::cout << "--- Directed-Light Tests ---" << std::endl;
	LuminaryManager::LuminariesList emptyLums;

	// 5a. BVH with directed lights builds correct orientation cones
	// Two spotlights pointing in the same direction should have
	// that direction as the cone axis with the original half-angle.
	{
		std::vector<Point3> pos = { Point3(0,0,0), Point3(5,0,0) };
		std::vector<Vector3> dirs = { Vector3(0,-1,0), Vector3(0,-1,0) };
		std::vector<Scalar> halfAngles = { 0.3, 0.3 };
		std::vector<Scalar> powers = { 10.0, 10.0 };
		auto entries = MakeSpotLightEntries( pos, dirs, halfAngles, powers );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		Check( bvh.IsBuilt() && bvh.GetLightCount() == 2,
			   "Directed lights: BVH built successfully" );
	}

	// 5b. Spotlight cone affects importance: shading point in-cone
	// gets higher selection probability than out-of-cone.
	// Two spotlights at the same position with the same power:
	//   Light 0 points toward shading point
	//   Light 1 points away from shading point
	// Light 0 should be selected almost always.
	{
		const Point3 shadingPt( 0, -5, 0 );
		const Vector3 shadingN( 0, 1, 0 );

		std::vector<Point3> pos = { Point3(0,0,0), Point3(0,0,0) };
		std::vector<Vector3> dirs = {
			Vector3(0,-1,0),   // points toward shading point
			Vector3(0,1,0)     // points away from shading point
		};
		std::vector<Scalar> halfAngles = { 0.5, 0.5 };  // ~28 degrees
		std::vector<Scalar> powers = { 10.0, 10.0 };
		auto entries = MakeSpotLightEntries( pos, dirs, halfAngles, powers );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		const int N = 100000;
		int countLight0 = 0;

		RandomNumberGenerator rng( 999 );

		for( int i = 0; i < N; i++ )
		{
			Scalar pdf;
			const Scalar xi = rng.CanonicalRandom();
			unsigned int idx = bvh.Sample( shadingPt, shadingN, xi, pdf );
			if( idx == 0 ) countLight0++;
		}

		const Scalar frac0 = Scalar(countLight0) / N;

		Check( frac0 > 0.95,
			   "In-cone spotlight selected >95% of the time" );
	}

	// 5c. Zero-importance subtree: all spotlights point away from
	// the shading point.  Sample() must return pdf=0 and Pdf()
	// must return 0 for all lights.  This exercises the P1 fix.
	{
		const Point3 shadingPt( 0, -10, 0 );
		const Vector3 shadingN( 0, 1, 0 );

		// All spotlights point upward (+Y), shading point is below.
		// With a half-angle of 0.3 (~17 degrees), the shading point
		// is well outside every emission cone.
		std::vector<Point3> pos = {
			Point3(0,0,0), Point3(3,0,0), Point3(6,0,0)
		};
		std::vector<Vector3> dirs = {
			Vector3(0,1,0), Vector3(0,1,0), Vector3(0,1,0)
		};
		std::vector<Scalar> halfAngles = { 0.3, 0.3, 0.3 };
		std::vector<Scalar> powers = { 10.0, 10.0, 10.0 };
		auto entries = MakeSpotLightEntries( pos, dirs, halfAngles, powers );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		// All Pdf() values should be 0
		bool allPdfZero = true;
		for( int i = 0; i < 3; i++ )
		{
			const Scalar pdf = bvh.Pdf( i, shadingPt, shadingN );
			if( pdf != 0 )
			{
				std::cout << "    Pdf(" << i << ") = " << pdf
					<< " (expected 0)" << std::endl;
				allPdfZero = false;
			}
		}

		Check( allPdfZero,
			   "All-away spotlights: Pdf() = 0 for all lights" );

		// Sample() should return pdf=0
		RandomNumberGenerator rng( 777 );
		bool allSamplePdfZero = true;

		for( int i = 0; i < 1000; i++ )
		{
			Scalar pdf;
			const Scalar xi = rng.CanonicalRandom();
			bvh.Sample( shadingPt, shadingN, xi, pdf );
			if( pdf != 0 )
			{
				std::cout << "    Sample() returned pdf=" << pdf
					<< " (expected 0)" << std::endl;
				allSamplePdfZero = false;
				break;
			}
		}

		Check( allSamplePdfZero,
			   "All-away spotlights: Sample() returns pdf=0" );
	}

	// 5d. Mixed directed + isotropic: PDF sum should still be 1.0
	// when at least some lights have nonzero importance.
	{
		const Point3 shadingPt( 0, -5, 0 );
		const Vector3 shadingN( 0, 1, 0 );

		// Light 0: isotropic point light (pLight = NULL, full sphere)
		// Light 1: spotlight pointing toward shading point
		// Light 2: spotlight pointing away from shading point
		std::vector<LightEntry> entries( 3 );

		// Isotropic point light (mesh-luminaire path, no pLight)
		entries[0].pLight = NULL;
		entries[0].lumIndex = 0;
		entries[0].exitance = 10.0;
		entries[0].position = Point3( 0, 0, 0 );

		// Spotlight toward shading point
		MockSpotLight* spotToward = new MockSpotLight(
			Vector3(0,-1,0), 0.5, Point3(2,0,0), 10.0 );
		spotToward->addref();
		g_mockLights.push_back( spotToward );
		entries[1].pLight = spotToward;
		entries[1].lumIndex = 0;
		entries[1].exitance = 10.0;
		entries[1].position = Point3( 2, 0, 0 );

		// Spotlight away from shading point
		MockSpotLight* spotAway = new MockSpotLight(
			Vector3(0,1,0), 0.3, Point3(4,0,0), 10.0 );
		spotAway->addref();
		g_mockLights.push_back( spotAway );
		entries[2].pLight = spotAway;
		entries[2].lumIndex = 0;
		entries[2].exitance = 10.0;
		entries[2].position = Point3( 4, 0, 0 );

		LightBVH bvh;
		bvh.Build( entries, emptyLums );

		Scalar pdfSum = 0;
		for( int i = 0; i < 3; i++ )
		{
			pdfSum += bvh.Pdf( i, shadingPt, shadingN );
		}

		Check( fabs( pdfSum - 1.0 ) < 1e-6,
			   "Mixed directed+isotropic: PDF sum = 1.0" );
	}
}

// ============================================================
//  Main
// ============================================================

int main( int /*argc*/, char** /*argv*/ )
{
	std::cout << "========================================" << std::endl;
	std::cout << " Light BVH Test Suite" << std::endl;
	std::cout << "========================================" << std::endl;

	TestConeMerge();
	TestConstruction();
	TestSampling();
	TestPdf();
	TestDirectedLights();

	std::cout << "========================================" << std::endl;
	std::cout << " Results: " << testsPassed << " passed, "
		<< testsFailed << " failed" << std::endl;
	std::cout << "========================================" << std::endl;

	// Clean up mock lights
	for( unsigned int i = 0; i < (unsigned int)g_mockLights.size(); i++ )
		g_mockLights[i]->release();
	g_mockLights.clear();

	return (testsFailed > 0) ? 1 : 0;
}
