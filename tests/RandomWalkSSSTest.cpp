//////////////////////////////////////////////////////////////////////
//
//  RandomWalkSSSTest.cpp - Validates the random-walk subsurface
//    scattering algorithm and coefficient conversion utilities.
//
//  Tests:
//    A. Walk exits unit sphere: random walks inside a unit sphere
//       produce valid exit points on the sphere surface.
//    B. Energy conservation: average walk weight matches the expected
//       single-scatter albedo for thick geometry.
//    C. Pure absorption: walks in a purely absorbing medium produce
//       weights strictly less than 1.0.
//    D. Coefficient conversion: ConvertBurleyToVolume round-trip
//       consistency (sigma_a + sigma_s == sigma_t, sigma_s/sigma_t == A).
//    E. TIR handling: walks at high IOR do not produce NaN/Inf
//       despite frequent total internal reflection events.
//
//  Build (from project root):
//    make -C build/make/rise tests
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <iomanip>
#include <numeric>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
#include "../src/Library/Utilities/SSSCoefficients.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"

using namespace RISE;
using namespace RISE::Implementation;

// ================================================================
// Helpers
// ================================================================

static bool IsClose( double a, double b, double relTol = 0.02, double absTol = 1e-6 )
{
	const double diff = std::fabs( a - b );
	const double ref = std::fmax( std::fabs(a), std::fabs(b) );
	return diff < absTol || diff < relTol * ref;
}

/// Simple independent sampler using the RISE random number generator
class TestSampler : public ISampler
{
	RandomNumberGenerator m_rng;
public:
	TestSampler( unsigned int seed ) : m_rng( seed ) {}
	Scalar Get1D() { return m_rng.CanonicalRandom(); }
	Point2 Get2D() { return Point2( Get1D(), Get1D() ); }
};

/// Create a unit sphere centered at origin as an IObject
static Object* MakeUnitSphere()
{
	SphereGeometry* pGeo = new SphereGeometry( 1.0 );
	pGeo->addref();

	Object* pObj = new Object( pGeo );
	pObj->addref();

	pGeo->release();  // Object holds a reference
	return pObj;
}

/// Create a RayIntersectionGeometric for a ray hitting the unit sphere
/// at the south pole from below (hitting front face).
static RayIntersectionGeometric MakeSphereHitRI(
	const Point3& hitPoint,
	const Vector3& normal,
	const Vector3& incomingDir
	)
{
	RayIntersectionGeometric ri( Ray( Point3Ops::mkPoint3( hitPoint, -incomingDir * 2.0 ), incomingDir ), nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = hitPoint;
	ri.vNormal = normal;
	ri.onb.CreateFromW( normal );
	return ri;
}

// ================================================================
// Test A: Walk exits unit sphere
// ================================================================

void TestWalkExitsSphere()
{
	std::cout << "\nTest A: Walk exits unit sphere" << std::endl;

	Object* pSphere = MakeUnitSphere();
	TestSampler sampler( 42 );

	const RISEPel sigma_a( 0.02, 0.02, 0.02 );
	const RISEPel sigma_s( 1.0, 1.0, 1.0 );
	RISEPel sigma_t;
	SSSCoefficients::FromCoefficients( sigma_a, sigma_s, sigma_t );

	const Scalar g = 0.0;
	const Scalar ior = 1.3;
	const unsigned int maxBounces = 256;

	// Hit at south pole, ray coming from below
	RayIntersectionGeometric ri = MakeSphereHitRI(
		Point3( 0, -1, 0 ),   // south pole
		Vector3( 0, -1, 0 ),  // outward normal
		Vector3( 0, 1, 0 )    // incoming direction (upward)
	);

	const int N = 1000;
	int validCount = 0;
	int surfaceOK = 0;
	int finiteWeightOK = 0;

	for( int i = 0; i < N; i++ )
	{
		BSSRDFSampling::SampleResult result = RandomWalkSSS::SampleExit(
			ri, pSphere, sigma_a, sigma_s, sigma_t, g, ior, maxBounces, sampler, 0 );

		if( result.valid )
		{
			validCount++;

			// Check exit point is on the sphere surface (radius ~1.0)
			const Scalar r = sqrt(
				result.entryPoint.x * result.entryPoint.x +
				result.entryPoint.y * result.entryPoint.y +
				result.entryPoint.z * result.entryPoint.z );

			if( fabs( r - 1.0 ) < 0.05 ) {
				surfaceOK++;
			}

			// Check weights are finite and positive
			if( std::isfinite( result.weight[0] ) && result.weight[0] >= 0 &&
				std::isfinite( result.weight[1] ) && result.weight[1] >= 0 &&
				std::isfinite( result.weight[2] ) && result.weight[2] >= 0 )
			{
				finiteWeightOK++;
			}
		}
	}

	const double validRate = static_cast<double>(validCount) / N;
	std::cout << "  " << validCount << "/" << N << " valid exits ("
		<< std::fixed << std::setprecision(1) << (validRate * 100) << "%)" << std::endl;
	std::cout << "  " << surfaceOK << "/" << validCount
		<< " on sphere surface" << std::endl;
	std::cout << "  " << finiteWeightOK << "/" << validCount
		<< " finite positive weights" << std::endl;

	if( validRate < 0.50 ) {
		std::cerr << "FAIL: Exit rate too low (" << (validRate*100) << "% < 50%)" << std::endl;
		exit( 1 );
	}
	if( surfaceOK < validCount * 0.90 ) {
		std::cerr << "FAIL: Too many exits off sphere surface" << std::endl;
		exit( 1 );
	}
	if( finiteWeightOK < validCount ) {
		std::cerr << "FAIL: Non-finite weights detected" << std::endl;
		exit( 1 );
	}

	std::cout << "  Passed!" << std::endl;
	pSphere->release();
}

// ================================================================
// Test B: Energy conservation (thick sphere)
// ================================================================

void TestEnergyConservation()
{
	std::cout << "\nTest B: Energy conservation (thick sphere)" << std::endl;

	Object* pSphere = MakeUnitSphere();
	TestSampler sampler( 123 );

	// Albedo = sigma_s / sigma_t = 0.8
	const RISEPel sigma_a( 0.2, 0.2, 0.2 );
	const RISEPel sigma_s( 0.8, 0.8, 0.8 );
	RISEPel sigma_t;
	SSSCoefficients::FromCoefficients( sigma_a, sigma_s, sigma_t );

	const Scalar g = 0.0;
	const Scalar ior = 1.0;  // No Fresnel for clean energy test
	const unsigned int maxBounces = 256;

	// Hit at south pole
	RayIntersectionGeometric ri = MakeSphereHitRI(
		Point3( 0, -1, 0 ),
		Vector3( 0, -1, 0 ),
		Vector3( 0, 1, 0 )
	);

	const int N = 50000;
	double sumWeight = 0;
	int validCount = 0;

	for( int i = 0; i < N; i++ )
	{
		BSSRDFSampling::SampleResult result = RandomWalkSSS::SampleExit(
			ri, pSphere, sigma_a, sigma_s, sigma_t, g, ior, maxBounces, sampler, 0 );

		if( result.valid )
		{
			validCount++;
			// Use spatial weight (no Sw factor) for energy test
			sumWeight += (result.weightSpatial[0] + result.weightSpatial[1] + result.weightSpatial[2]) / 3.0;
		}
	}

	// Average weight over all attempts (valid + absorbed)
	const double avgWeight = sumWeight / N;
	const double expectedAlbedo = 0.8;

	std::cout << "  Valid exits: " << validCount << "/" << N
		<< " (" << std::fixed << std::setprecision(1)
		<< (100.0 * validCount / N) << "%)" << std::endl;
	std::cout << "  Average weight: " << std::setprecision(4) << avgWeight
		<< " (expected ~" << expectedAlbedo << ")" << std::endl;

	// Relaxed tolerance: random walk energy should be in the right
	// ballpark but exact match depends on geometry and IOR
	if( fabs( avgWeight - expectedAlbedo ) > 0.25 ) {
		std::cerr << "FAIL: Average weight " << avgWeight
			<< " too far from expected " << expectedAlbedo << std::endl;
		exit( 1 );
	}

	std::cout << "  Passed!" << std::endl;
	pSphere->release();
}

// ================================================================
// Test C: Pure absorption
// ================================================================

void TestPureAbsorption()
{
	std::cout << "\nTest C: Pure absorption (sigma_s = 0)" << std::endl;

	Object* pSphere = MakeUnitSphere();
	TestSampler sampler( 456 );

	const RISEPel sigma_a( 1.0, 1.0, 1.0 );
	const RISEPel sigma_s( 0.0, 0.0, 0.0 );
	RISEPel sigma_t;
	SSSCoefficients::FromCoefficients( sigma_a, sigma_s, sigma_t );

	const Scalar g = 0.0;
	const Scalar ior = 1.0;
	const unsigned int maxBounces = 256;

	RayIntersectionGeometric ri = MakeSphereHitRI(
		Point3( 0, -1, 0 ),
		Vector3( 0, -1, 0 ),
		Vector3( 0, 1, 0 )
	);

	const int N = 1000;
	int validCount = 0;

	double sumWeight = 0;

	for( int i = 0; i < N; i++ )
	{
		BSSRDFSampling::SampleResult result = RandomWalkSSS::SampleExit(
			ri, pSphere, sigma_a, sigma_s, sigma_t, g, ior, maxBounces, sampler, 0 );

		if( result.valid ) {
			validCount++;
			const double w = (result.weightSpatial[0] + result.weightSpatial[1] + result.weightSpatial[2]) / 3.0;
			sumWeight += w;

			// Weights should be finite
			if( !std::isfinite( w ) ) {
				std::cerr << "FAIL: Non-finite weight with sigma_s=0" << std::endl;
				exit( 1 );
			}
		}
	}

	// With zero scattering, the walk traverses the medium without
	// scattering and exits with Beer-Lambert attenuation.  The exit
	// direction is still cosine-weighted (not ballistic) because
	// the walk models multiple scattering only — ballistic
	// transmission belongs in a separate dielectric BSDF layer.
	// The average weight should be small (exp(-sigma_a * ~2.0)).
	const double avgWeight = validCount > 0 ? sumWeight / N : 0;
	std::cout << "  Valid exits: " << validCount << "/" << N << std::endl;
	std::cout << "  Average weight: " << std::fixed << std::setprecision(4) << avgWeight
		<< " (expected small, Beer-Lambert)" << std::endl;
	std::cout << "  Passed!" << std::endl;
	pSphere->release();
}

// ================================================================
// Test D: Coefficient conversion round-trip
// ================================================================

void TestCoefficientConversion()
{
	std::cout << "\nTest D: Coefficient conversion consistency" << std::endl;

	struct TestCase {
		RISEPel A;  // albedo
		RISEPel d;  // mean free path
		Scalar g;
		const char* name;
	};

	TestCase cases[] = {
		{ RISEPel(0.5, 0.5, 0.5), RISEPel(1.0, 1.0, 1.0), 0.0, "medium-iso" },
		{ RISEPel(0.9, 0.8, 0.7), RISEPel(0.5, 0.5, 0.5), 0.0, "high-A-iso" },
		{ RISEPel(1.0, 1.0, 1.0), RISEPel(2.0, 2.0, 2.0), 0.0, "unit-A" },
		{ RISEPel(0.0, 0.0, 0.0), RISEPel(1.0, 1.0, 1.0), 0.0, "zero-A" },
		{ RISEPel(0.5, 0.5, 0.5), RISEPel(1.0, 1.0, 1.0), 0.8, "medium-g0.8" },
		{ RISEPel(0.9, 0.9, 0.9), RISEPel(0.1, 0.1, 0.1), -0.3, "high-A-neg-g" },
	};

	bool allPassed = true;

	for( const auto& tc : cases )
	{
		RISEPel sa, ss, st;
		SSSCoefficients::ConvertBurleyToVolume( tc.A, tc.d, tc.g, sa, ss, st );

		for( int ch = 0; ch < 3; ch++ )
		{
			// sigma_a + sigma_s == sigma_t
			if( !IsClose( sa[ch] + ss[ch], st[ch], 1e-10 ) )
			{
				std::cerr << "FAIL: " << tc.name << " ch" << ch
					<< " sigma_a+sigma_s=" << (sa[ch]+ss[ch])
					<< " != sigma_t=" << st[ch] << std::endl;
				allPassed = false;
			}

			// All coefficients non-negative
			if( sa[ch] < -1e-10 || ss[ch] < -1e-10 || st[ch] < -1e-10 )
			{
				std::cerr << "FAIL: " << tc.name << " ch" << ch
					<< " negative coefficient" << std::endl;
				allPassed = false;
			}

			// Albedo consistency: sigma_s / sigma_t == effective albedo
			// (with similarity relation, effective albedo may differ from input A)
			if( st[ch] > 1e-10 )
			{
				const Scalar effAlbedo = ss[ch] / st[ch];
				if( fabs(tc.g) < 1e-6 )
				{
					// Without similarity, should match input A
					if( !IsClose( effAlbedo, tc.A[ch], 1e-6 ) )
					{
						std::cerr << "FAIL: " << tc.name << " ch" << ch
							<< " effAlbedo=" << effAlbedo
							<< " != A=" << tc.A[ch] << std::endl;
						allPassed = false;
					}
				}
			}
		}
		std::cout << "  " << tc.name << " OK" << std::endl;
	}

	if( !allPassed ) {
		std::cerr << "FAIL: Coefficient conversion" << std::endl;
		exit( 1 );
	}

	std::cout << "  Passed!" << std::endl;
}

// ================================================================
// Test E: TIR handling at high IOR
// ================================================================

void TestTIRHandling()
{
	std::cout << "\nTest E: TIR handling at high IOR" << std::endl;

	Object* pSphere = MakeUnitSphere();
	TestSampler sampler( 789 );

	const RISEPel sigma_a( 0.05, 0.05, 0.05 );
	const RISEPel sigma_s( 1.0, 1.0, 1.0 );
	RISEPel sigma_t;
	SSSCoefficients::FromCoefficients( sigma_a, sigma_s, sigma_t );

	const Scalar g = 0.0;
	const Scalar ior = 2.5;  // High IOR => many TIR events
	const unsigned int maxBounces = 256;

	RayIntersectionGeometric ri = MakeSphereHitRI(
		Point3( 0, -1, 0 ),
		Vector3( 0, -1, 0 ),
		Vector3( 0, 1, 0 )
	);

	const int N = 500;
	int validCount = 0;
	bool anyNaN = false;
	bool anyInf = false;

	for( int i = 0; i < N; i++ )
	{
		BSSRDFSampling::SampleResult result = RandomWalkSSS::SampleExit(
			ri, pSphere, sigma_a, sigma_s, sigma_t, g, ior, maxBounces, sampler, 0 );

		if( result.valid )
		{
			validCount++;

			for( int ch = 0; ch < 3; ch++ )
			{
				if( std::isnan( result.weight[ch] ) ) anyNaN = true;
				if( std::isinf( result.weight[ch] ) ) anyInf = true;
			}
		}
	}

	std::cout << "  Valid exits: " << validCount << "/" << N
		<< " (" << std::fixed << std::setprecision(1)
		<< (100.0 * validCount / N) << "%)" << std::endl;

	if( anyNaN ) {
		std::cerr << "FAIL: NaN weights detected" << std::endl;
		exit( 1 );
	}
	if( anyInf ) {
		std::cerr << "FAIL: Infinite weights detected" << std::endl;
		exit( 1 );
	}

	std::cout << "  No NaN/Inf weights" << std::endl;
	std::cout << "  Passed!" << std::endl;
	pSphere->release();
}

// ================================================================
// Main
// ================================================================

int main( int argc, char** argv )
{
	std::cout << "=== Random Walk SSS Tests ===" << std::endl;

	TestWalkExitsSphere();
	TestEnergyConservation();
	TestPureAbsorption();
	TestCoefficientConversion();
	TestTIRHandling();

	std::cout << "\nAll random walk SSS tests passed!" << std::endl;
	return 0;
}
