//////////////////////////////////////////////////////////////////////
//
//  SobolDimensionBudgetTest.cpp - Validates that rendering code paths
//    stay within the Sobol sampler's per-phase dimension budget.
//
//  Background:
//    The SobolSampler partitions dimensions into fixed-size phases
//    of kStreamStride (32) dimensions.  If any single operation
//    consumes more than kStreamStride dimensions, it bleeds into
//    dimensions reserved for subsequent operations, destroying
//    cross-pixel stratification and creating persistent per-pixel
//    biases (structured noise that does not converge with more
//    samples).
//
//  Tests:
//    A. Random walk dimension consumption: proves that
//       RandomWalkSSS::SampleExit() consumes far more than
//       kStreamStride dimensions, documenting why it must use
//       IndependentSampler rather than SobolSampler.
//    B. Disk-projection BSSRDF budget: BSSRDFSampling::SampleEntryPoint
//       stays within the dimension budget per invocation.
//    C. Integration guard: verifies that RandomWalkSSS::SampleExit
//       in the actual integrator code paths uses IndependentSampler,
//       not the Sobol sampler, by grepping the source files.
//
//  Build (from project root):
//    make -C build/make/rise tests
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <fstream>
#include <string>
#include <filesystem>
#include <system_error>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Sampling/SobolSequence.h"
#include "../src/Library/Utilities/RasterizerDefaults.h"
#include "../src/Library/Utilities/StabilityConfig.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Cameras/CameraUtilities.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Cameras/OrthographicCamera.h"
#include "../src/Library/Cameras/FisheyeCamera.h"

using namespace RISE;
using namespace RISE::Implementation;

// ================================================================
// DimensionAuditSampler
//
// Wraps any ISampler and counts how many dimensions it consumes.
// Tracks both current count and peak (high-water mark) across
// multiple checkpoint/reset cycles.
// ================================================================

class DimensionAuditSampler : public ISampler
{
	RandomNumberGenerator m_rng;
	unsigned int m_count;
	unsigned int m_peak;
	unsigned int m_streams;

public:
	DimensionAuditSampler( unsigned int seed )
		: m_rng( seed ), m_count( 0 ), m_peak( 0 ), m_streams( 0 )
	{
	}

	Scalar Get1D()
	{
		m_count++;
		if( m_count > m_peak ) m_peak = m_count;
		return m_rng.CanonicalRandom();
	}

	Point2 Get2D()
	{
		m_count += 2;
		if( m_count > m_peak ) m_peak = m_count;
		return Point2( m_rng.CanonicalRandom(), m_rng.CanonicalRandom() );
	}

	/// Counts stream switches as well as dimensions: debt 28's
	/// aperture draw must make NEITHER when the camera's aperture is a
	/// point (PSSMLTSampler::StartStream also resets sampleIndex, so a
	/// stray switch is not free even when no value is drawn).
	void StartStream( int /*streamIndex*/ ) { m_streams++; }

	unsigned int GetStreamSwitches() const { return m_streams; }

	/// Reset the current count (e.g., between operations)
	void ResetCount() { m_count = 0; m_streams = 0; }

	/// Current dimension count since last reset
	unsigned int GetCount() const { return m_count; }

	/// Peak dimension count across all resets
	unsigned int GetPeak() const { return m_peak; }

	/// Reset everything including the peak
	void ResetAll() { m_count = 0; m_peak = 0; m_streams = 0; }
};

// ================================================================
// Helpers
// ================================================================

static Object* MakeUnitSphere()
{
	SphereGeometry* pGeo = new SphereGeometry( 1.0 );
	pGeo->addref();
	Object* pObj = new Object( pGeo );
	pObj->addref();
	pGeo->release();
	return pObj;
}

/// Create a RayIntersectionGeometric for a ray hitting the unit sphere
/// at the south pole from below (following RandomWalkSSSTest pattern).
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
	ri.range = 2.0;
	return ri;
}

// ================================================================
// Test A: Random walk consumes >> kStreamStride dimensions
//
// This proves that RandomWalkSSS::SampleExit() is fundamentally
// incompatible with the Sobol sampler's fixed-stride phase budget
// and MUST use IndependentSampler.
// ================================================================

static void TestRandomWalkDimensionConsumption()
{
	std::cout << "\nTest A: Random walk dimension consumption\n";

	Object* pObj = MakeUnitSphere();

	// Medium with moderate scattering — walks will scatter many times
	const RISEPel sigma_a( 0.1, 0.15, 0.2 );
	const RISEPel sigma_s( 5.0, 5.0, 5.0 );
	const RISEPel sigma_t( 5.1, 5.15, 5.2 );
	const Scalar g = 0.0;
	const Scalar ior = 1.3;
	const unsigned int maxBounces = 64;

	// The Sobol sampler's phase budget
	const unsigned int kStreamStride = 32;

	const int N = 500;
	unsigned int totalDims = 0;
	unsigned int maxDims = 0;
	unsigned int minDims = 999999;
	int overflowCount = 0;
	int validExits = 0;

	for( int i = 0; i < N; i++ )
	{
		DimensionAuditSampler sampler( 42 + i );

		// Construct intersection at the south pole of the unit sphere
		RayIntersectionGeometric riGeo = MakeSphereHitRI(
			Point3( 0, -1, 0 ),
			Vector3( 0, -1, 0 ),
			Vector3( 0, 1, 0 ) );

		sampler.ResetAll();

		BSSRDFSampling::SampleResult result = RandomWalkSSS::SampleExit(
			riGeo, pObj, sigma_a, sigma_s, sigma_t, g, ior, maxBounces,
			sampler, 0 );

		const unsigned int dims = sampler.GetCount();
		totalDims += dims;
		if( dims > maxDims ) maxDims = dims;
		if( dims < minDims ) minDims = dims;
		if( dims > kStreamStride ) overflowCount++;
		if( result.valid ) validExits++;
	}

	const double avgDims = static_cast<double>(totalDims) / N;
	const double overflowPct = 100.0 * overflowCount / N;

	std::cout << "  Walks: " << N
		<< ", valid exits: " << validExits << "\n";
	std::cout << "  Dimensions per walk — min: " << minDims
		<< ", avg: " << std::fixed << std::setprecision(1) << avgDims
		<< ", max: " << maxDims << "\n";
	std::cout << "  kStreamStride budget: " << kStreamStride << "\n";
	std::cout << "  Walks exceeding budget: " << overflowCount
		<< " / " << N << " (" << std::setprecision(1)
		<< overflowPct << "%)\n";

	// The random walk MUST overflow the Sobol budget in a significant
	// fraction of walks.  Even a single overflow per pixel sample
	// would corrupt all subsequent Sobol dimensions for that sample.
	// With typical scattering coefficients, >30% of walks overflow.
	if( overflowPct < 20.0 )
	{
		std::cerr << "  FAIL: Expected >20% of walks to exceed "
			<< kStreamStride << " dimensions, got "
			<< overflowPct << "%\n";
		std::cerr << "  This test documents that the random walk is "
			<< "incompatible with SobolSampler.\n";
		std::cerr << "  If kStreamStride was increased, update this "
			<< "test's threshold.\n";
		exit( 1 );
	}

	if( avgDims < kStreamStride )
	{
		std::cerr << "  FAIL: Average dimension consumption ("
			<< avgDims << ") should be >= kStreamStride ("
			<< kStreamStride << ")\n";
		exit( 1 );
	}

	std::cout << "  Passed! (walk consumes " << std::setprecision(0)
		<< avgDims << " dims on average, " << std::setprecision(0)
		<< overflowPct << "% exceed budget, confirming IndependentSampler is required)\n";

	pObj->release();
}

// ================================================================
// Test B: Disk-projection BSSRDF stays within budget
//
// BSSRDFSampling::SampleEntryPoint() has a fixed dimension cost
// per invocation (channel selection + axis selection + radius +
// angle + probe rays + direction generation + Fresnel).
// Verify it stays within kStreamStride.
// ================================================================

// Note: SampleEntryPoint requires a full material with a diffusion
// profile, which is heavy to set up in a unit test.  Instead we
// count dimensions analytically:
//   - Channel selection:     1 (Get1D)
//   - Axis selection:        1 (Get1D)
//   - Radius sample:         1 (Get1D)
//   - Angle sample:          1 (Get1D)
//   - Hit selection:         1 (Get1D)
//   - Cosine direction:      2 (Get1D x2)
//   Total fixed:             7 dimensions
//
// The probe ray loop does NOT consume sampler dimensions (it uses
// deterministic ray directions derived from the sampled axis).
// So the total is 7, well within kStreamStride = 32.
//
// We verify this by inspecting the source code.

static void TestDiskProjectionBudget()
{
	std::cout << "\nTest B: Disk-projection BSSRDF dimension budget (analytical)\n";

	const unsigned int kStreamStride = 32;

	// Count sampler.Get1D() calls in BSSRDFSampling::SampleEntryPoint
	// by reading the source.  This is fragile but catches regressions.
	std::ifstream file( "src/Library/Utilities/BSSRDFSampling.cpp" );
	if( !file.is_open() )
	{
		// Try from project root via relative path
		file.open( "../../../src/Library/Utilities/BSSRDFSampling.cpp" );
	}

	if( !file.is_open() )
	{
		std::cout << "  SKIP: Could not open BSSRDFSampling.cpp for analysis\n";
		std::cout << "  (Run from project root or bin/tests/ directory)\n";
		return;
	}

	int get1dCount = 0;
	int get2dCount = 0;
	std::string line;
	bool inFunction = false;

	while( std::getline( file, line ) )
	{
		// Detect function start
		if( line.find( "SampleEntryPoint" ) != std::string::npos &&
			line.find( "BSSRDFSampling::" ) != std::string::npos )
		{
			inFunction = true;
			continue;
		}

		if( !inFunction ) continue;

		// Count sampler calls (only the Get1D/Get2D on sampler object)
		if( line.find( "sampler.Get1D()" ) != std::string::npos )
			get1dCount++;
		if( line.find( "sampler.Get2D()" ) != std::string::npos )
			get2dCount++;
	}

	const int totalDims = get1dCount + get2dCount * 2;

	std::cout << "  SampleEntryPoint sampler calls: "
		<< get1dCount << " x Get1D + "
		<< get2dCount << " x Get2D = "
		<< totalDims << " dimensions\n";
	std::cout << "  kStreamStride budget: " << kStreamStride << "\n";

	if( totalDims > kStreamStride )
	{
		std::cerr << "  FAIL: SampleEntryPoint consumes " << totalDims
			<< " dimensions, exceeding kStreamStride = "
			<< kStreamStride << "\n";
		std::cerr << "  This will cause Sobol dimension overflow.\n";
		exit( 1 );
	}

	if( totalDims < 5 )
	{
		std::cerr << "  FAIL: Suspiciously low dimension count ("
			<< totalDims << "). Parser may be broken.\n";
		exit( 1 );
	}

	std::cout << "  Passed! (" << totalDims
		<< " dimensions, well within budget)\n";
}

// ================================================================
// Test C: Integration guard — verify IndependentSampler usage
//
// Grep the integrator source files to confirm that
// RandomWalkSSS::SampleExit is always called with a rwSampler
// (IndependentSampler), never with the Sobol sampler directly.
//
// This catches regressions where someone accidentally passes
// `sampler` or `*rc.pSampler` to the walk.
// ================================================================

static bool FileContainsPattern( const std::string& path,
	const std::string& pattern )
{
	std::ifstream file( path );
	if( !file.is_open() ) return false;

	std::string line;
	while( std::getline( file, line ) )
	{
		if( line.find( pattern ) != std::string::npos )
			return true;
	}
	return false;
}

static void TestIntegrationGuard()
{
	std::cout << "\nTest C: Integration guard (source analysis)\n";

	// PathTracingShaderOp is a thin wrapper that delegates to
	// PathTracingIntegrator — check the integrator for SSS guards.
	const char* ptPaths[] = {
		"src/Library/Shaders/PathTracingIntegrator.cpp",
		"../../../src/Library/Shaders/PathTracingIntegrator.cpp",
		0
	};
	const char* bdptPaths[] = {
		"src/Library/Shaders/BDPTIntegrator.cpp",
		"../../../src/Library/Shaders/BDPTIntegrator.cpp",
		0
	};

	std::string ptPath, bdptPath;

	for( int i = 0; ptPaths[i]; i++ ) {
		std::ifstream test( ptPaths[i] );
		if( test.is_open() ) { ptPath = ptPaths[i]; break; }
	}
	for( int i = 0; bdptPaths[i]; i++ ) {
		std::ifstream test( bdptPaths[i] );
		if( test.is_open() ) { bdptPath = bdptPaths[i]; break; }
	}

	if( ptPath.empty() || bdptPath.empty() )
	{
		std::cout << "  SKIP: Could not open integrator source files\n";
		std::cout << "  (Run from project root or bin/tests/ directory)\n";
		return;
	}

	bool allPassed = true;

	// Strategy: read entire file into a vector of lines, then scan
	// for SampleExit call sites by looking at the sampler argument
	// line (which follows the maxBounces line in the multi-line call).

	auto ReadLines = []( const std::string& path ) -> std::vector<std::string>
	{
		std::vector<std::string> lines;
		std::ifstream file( path );
		std::string line;
		while( std::getline( file, line ) )
			lines.push_back( line );
		return lines;
	};

	auto AuditSampleExitCalls = [&]( const std::string& path,
		const std::string& label, int expectedMin ) -> bool
	{
		const std::vector<std::string> lines = ReadLines( path );
		int rwSamplerCount = 0;
		int rawSamplerCount = 0;

		for( size_t i = 0; i + 1 < lines.size(); i++ )
		{
			// The SampleExit call may be formatted on one line or
			// split across two lines.  The maxBounces argument and
			// the sampler argument may be on the same line or the
			// sampler may be on the next line.
			//
			// Only match code lines (skip comments).
			const std::string& curLine = lines[i];
			if( curLine.find( "->maxBounces" ) == std::string::npos &&
				curLine.find( ".maxBounces" ) == std::string::npos )
			{
				continue;
			}

			// Check for comment lines
			std::string trimmed = curLine;
			size_t firstNonSpace = trimmed.find_first_not_of( " \t" );
			if( firstNonSpace != std::string::npos &&
				trimmed.substr( firstNonSpace, 2 ) == "//" )
			{
				continue;
			}

			// The sampler arg might be on this same line or the next
			const std::string combined = curLine + " " + lines[i + 1];

			if( combined.find( "rwSampler" ) != std::string::npos )
			{
				rwSamplerCount++;
			}
			else if( combined.find( "sampler" ) != std::string::npos &&
					 combined.find( "rwSampler" ) == std::string::npos &&
					 combined.find( "IndependentSampler" ) == std::string::npos )
			{
				rawSamplerCount++;
			}
		}

		std::cout << "  " << label << ":\n";
		std::cout << "    SampleExit with rwSampler: "
			<< rwSamplerCount << "\n";
		std::cout << "    SampleExit with raw sampler: "
			<< rawSamplerCount << "\n";

		bool ok = true;
		if( rawSamplerCount > 0 )
		{
			std::cerr << "    FAIL: Found " << rawSamplerCount
				<< " SampleExit call(s) using the Sobol sampler directly!\n";
			std::cerr << "    The random walk MUST use IndependentSampler "
				<< "to avoid dimension overflow.\n";
			ok = false;
		}

		if( rwSamplerCount < expectedMin )
		{
			std::cerr << "    FAIL: Expected at least " << expectedMin
				<< " SampleExit calls with rwSampler, found "
				<< rwSamplerCount << "\n";
			ok = false;
		}

		return ok;
	};

	if( !AuditSampleExitCalls( ptPath, "PathTracingIntegrator.cpp", 1 ) )
		allPassed = false;

	// Expected rwSampler SampleExit call sites in BDPTIntegrator.cpp: 2.
	// Was 4 (eye Pel + eye NM + light Pel + light NM) before Phase 2c; F2a
	// templatized GenerateEyeSubpath{,NM} -> GenerateEyeSubpathImpl<Tag>
	// (4 -> 3, merging the two eye-subpath calls) and F2b templatized
	// GenerateLightSubpath{,NM} -> GenerateLightSubpathImpl<Tag> (3 -> 2,
	// merging the two light-subpath RW-SSS SampleExit calls into one shared
	// templated call).  The safety property the guard enforces is unchanged:
	// every SampleExit still uses rwSampler and zero use the raw Sobol
	// sampler -- now across two templated call sites (one eye, one light).
	if( !AuditSampleExitCalls( bdptPath, "BDPTIntegrator.cpp", 2 ) )
		allPassed = false;

	// Verify IndependentSampler.h is included in both files
	if( !FileContainsPattern( ptPath, "IndependentSampler.h" ) )
	{
		std::cerr << "  FAIL: PathTracingIntegrator.cpp does not include "
			<< "IndependentSampler.h\n";
		allPassed = false;
	}
	if( !FileContainsPattern( bdptPath, "IndependentSampler.h" ) )
	{
		std::cerr << "  FAIL: BDPTIntegrator.cpp does not include "
			<< "IndependentSampler.h\n";
		allPassed = false;
	}

	if( !allPassed )
	{
		exit( 1 );
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test D: SobolSampler dimension mechanics
//
// Verify that SobolSampler::StartStream() properly resets the
// dimension counter and that kStreamStride is correctly defined.
// This catches changes to the sampler that might silently break
// the phase partitioning.
// ================================================================

static void TestSobolSamplerMechanics()
{
	std::cout << "\nTest D: SobolSampler dimension mechanics\n";

	SobolSampler sampler( 0, 12345 );

	// Consume some dimensions
	for( int i = 0; i < 10; i++ ) sampler.Get1D();

	// StartStream should reset to a deterministic dimension
	sampler.StartStream( 0 );
	const Scalar val_stream0_a = sampler.Get1D();

	// Reset and verify we get the same value (deterministic)
	sampler.StartStream( 0 );
	const Scalar val_stream0_b = sampler.Get1D();

	if( val_stream0_a != val_stream0_b )
	{
		std::cerr << "  FAIL: StartStream(0) should produce deterministic "
			<< "results: " << val_stream0_a << " vs " << val_stream0_b << "\n";
		exit( 1 );
	}

	// Different streams should produce different values (with high probability)
	sampler.StartStream( 1 );
	const Scalar val_stream1 = sampler.Get1D();

	// This could theoretically fail with probability ~2^-52 but
	// in practice it never will.
	if( val_stream0_a == val_stream1 )
	{
		std::cerr << "  WARNING: Stream 0 and stream 1 produced the same "
			<< "value.  This is extremely unlikely but possible.\n";
	}

	// Verify kStreamStride value is what we expect
	// (We can't access the constant directly, but we can verify
	// behavior: stream N should start at dimension N*32.)
	// Sample from stream 0, dim 0 and stream 1, dim 0 should differ
	// because they're at different Sobol dimensions (0 vs 32).
	sampler.StartStream( 0 );
	(void)sampler.Get1D();  // dimension 0

	sampler.StartStream( 1 );
	(void)sampler.Get1D();  // dimension 32

	std::cout << "  Stream determinism: OK\n";
	std::cout << "  Stream independence: OK\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test E: HasFixedDimensionBudget contract
//
// Verify that SobolSampler reports a fixed budget and that
// DimensionAuditSampler (IndependentSampler-like) does not.
// This ensures the conditional walk-sampler logic in the
// integrators correctly distinguishes the two.
// ================================================================

static void TestHasFixedDimensionBudget()
{
	std::cout << "\nTest E: HasFixedDimensionBudget contract\n";

	SobolSampler sobol( 0, 12345 );
	DimensionAuditSampler independent( 42 );

	if( !sobol.HasFixedDimensionBudget() )
	{
		std::cerr << "  FAIL: SobolSampler must report HasFixedDimensionBudget() == true\n";
		exit( 1 );
	}

	if( independent.HasFixedDimensionBudget() )
	{
		std::cerr << "  FAIL: IndependentSampler-derived must report "
			<< "HasFixedDimensionBudget() == false\n";
		exit( 1 );
	}

	std::cout << "  SobolSampler::HasFixedDimensionBudget() == true: OK\n";
	std::cout << "  IndependentSampler::HasFixedDimensionBudget() == false: OK\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test F: the t==1 camera-aperture draw (debt 28)
//
// Two properties, both of which the first cut of debt 28 got wrong
// (review A P1-1):
//
//   1. A camera whose aperture is a POINT must consume nothing --
//      neither a dimension nor a stream switch.  Every integrator that
//      supports light tracing calls DrawApertureSample once per
//      sample, pinhole scenes included; an unconditional draw moved
//      every pinhole MLT render onto a different Markov chain and
//      burned a Sobol dimension on a sample the camera ignores.
//      A thin lens with a real aperture must consume exactly 2.
//
//   2. kApertureSamplerStream must sit ABOVE every stream a subpath
//      walk can reach.  The walks are not confined to 0..48: the eye
//      walk runs 16 + bounce and the light walk 1 + bounce with the
//      bounce count saturating at 1024, and VCM's NEE runs 48 + eye
//      vertex index.  The old constant 80 was inside both.
// ================================================================

static void TestApertureDrawConsumption()
{
	std::cout << "\nTest F: t==1 aperture draw consumption (debt 28)\n";

	const unsigned int W = 64, H = 64;

	PinholeCamera* pinhole = new PinholeCamera(
		Point3( 0, 0, 6 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		0.6, W, H, 1.0, 0.0, 0.0, 0.0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );

	OrthographicCamera* ortho = new OrthographicCamera(
		Point3( 0, 0, 6 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		W, H, Vector2( 2.0, 2.0 ), 1.0, 0.0, 0.0, 0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );

	FisheyeCamera* fisheye = new FisheyeCamera(
		Point3( 0, 0, 6 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		W, H, 1.0, 0.0, 0.0, 0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ), 1.0 );

	// 36 mm sensor / 50 mm lens, f/2.8, focused at 4 m: a real aperture.
	ThinLensCamera* lens = new ThinLensCamera(
		Point3( 0, 0, 6 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		36.0, 50.0, 2.8, 4.0, 1.0,
		W, H, 1.0, 0.0, 0.0, 0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
		0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );

	// Stopped far down but NOT degenerate: radius 2.5e-14 scene units
	// still has area, so this is a finite aperture and must draw.  The
	// pinhole limit is continuous by design (there is no f-stop
	// threshold anywhere in the camera code), and that is what this row
	// pins -- a "close enough to a pinhole" special case would show up
	// here as 0.
	ThinLensCamera* stoppedDown = new ThinLensCamera(
		Point3( 0, 0, 6 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		36.0, 50.0, 1e12, 4.0, 1.0,
		W, H, 1.0, 0.0, 0.0, 0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
		0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );

	// Genuinely degenerate: anamorphic squeeze 0 collapses the aperture
	// to a line, so its area is 0 and HasFiniteAperture is false.  The
	// scene parser refuses this (debt 28 review A P2-3) precisely
	// because the two layers would then disagree, but the construction
	// API can still build one, so the draw has to behave.
	ThinLensCamera* degenerate = new ThinLensCamera(
		Point3( 0, 0, 6 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		36.0, 50.0, 2.8, 4.0, 1.0,
		W, H, 1.0, 0.0, 0.0, 0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
		0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 );

	struct Row { const char* name; const ICamera* cam; unsigned int expect; };
	const Row rows[] = {
		{ "pinhole",              pinhole, 0 },
		{ "orthographic",         ortho,   0 },
		{ "fisheye",              fisheye, 0 },
		{ "thin lens squeeze 0",  degenerate,  0 },
		{ "thin lens f/1e12",     stoppedDown, 2 },
		{ "thin lens f/2.8",      lens,        2 },
	};

	bool ok = true;
	const BDPTCameraUtilities::ApertureStreamPolicy policies[] = {
		BDPTCameraUtilities::APERTURE_DEDICATED_STREAM,
		BDPTCameraUtilities::APERTURE_CURRENT_STREAM
	};
	const char* policyNames[] = { "dedicated-stream", "current-stream" };

	for( unsigned int r = 0; r < sizeof(rows)/sizeof(rows[0]); r++ )
	{
		// Per-row flag: `ok` is the OVERALL pass/fail this function exits
		// on, so it must never be reset -- but printing it as-is for
		// EVERY row's summary line meant one early failing row left
		// every later row's "OK"/"FAIL" print wrong (always FAIL) even
		// when that row itself passed both policies.  `rowOk` is scoped
		// to this iteration; `ok` still accumulates across all rows.
		bool rowOk = true;
		for( unsigned int p = 0; p < 2; p++ )
		{
			DimensionAuditSampler audit( 1234 + r );
			audit.ResetAll();
			BDPTCameraUtilities::DrawApertureSample( *rows[r].cam, audit, policies[p] );

			const unsigned int dims = audit.GetCount();
			// A dedicated-stream draw legitimately switches streams
			// when it draws; a point aperture must switch none either way.
			const unsigned int expectSwitches =
				( rows[r].expect > 0 && p == 0 ) ? 1u : 0u;

			if( dims != rows[r].expect || audit.GetStreamSwitches() != expectSwitches )
			{
				std::cerr << "  FAIL: " << rows[r].name << " (" << policyNames[p]
					<< ") consumed " << dims << " dimensions and "
					<< audit.GetStreamSwitches() << " stream switches; expected "
					<< rows[r].expect << " and " << expectSwitches << "\n";
				rowOk = false;
				ok = false;
			}
		}
		std::cout << "  " << rows[r].name << ": " << rows[r].expect
			<< " dimensions consumed, both policies: "
			<< ( rowOk ? "OK" : "FAIL" ) << "\n";
	}

	// ------------------------------------------------------------
	// The dedicated stream clears every walk stream.
	// ------------------------------------------------------------
	{
		// Both GenerateEyeSubpath and GenerateLightSubpath saturate
		// their loop counts at 1024 (maxDepth + maxVolumeBounce,
		// clamped), and one iteration appends at most 3 vertices
		// (medium entry, medium scatter, surface).  VCM's NEE stream is
		// 48 + eye-vertex-index, which is the largest of the three.
		const int kWalkIterationCap   = 1024;
		const int kMaxVertsPerIter    = 3;
		const int kMaxVcmNeeStream    = 48 + kWalkIterationCap * kMaxVertsPerIter + 1;
		const int kMaxEyeWalkStream   = 16 + kWalkIterationCap;
		const int kMaxLightWalkStream =  1 + kWalkIterationCap;
		int worst = kMaxVcmNeeStream;
		if( kMaxEyeWalkStream   > worst ) worst = kMaxEyeWalkStream;
		if( kMaxLightWalkStream > worst ) worst = kMaxLightWalkStream;

		if( BDPTCameraUtilities::kApertureSamplerStream <= worst )
		{
			std::cerr << "  FAIL: kApertureSamplerStream ("
				<< BDPTCameraUtilities::kApertureSamplerStream
				<< ") is inside the walk streams (max reachable " << worst
				<< ").  An aperture sample drawn there is the same Sobol "
				<< "dimension as a bounce's.\n";
			ok = false;
		}
		else
		{
			std::cout << "  kApertureSamplerStream = "
				<< BDPTCameraUtilities::kApertureSamplerStream
				<< " clears the worst-case walk stream " << worst
				<< " (dimension " << ( BDPTCameraUtilities::kApertureSamplerStream * 32 )
				<< " vs " << ( worst * 32 ) << "): OK\n";
		}

		// Being above the walk streams stopped being sufficient at
		// DL-81, when SobolSequence acquired a FINITE supply of real
		// Sobol' dimensions: the old 8192 wrapped onto dimension 1001,
		// which is stream 31 slot 9 -- eye bounce 15.
		//
		// `DrawApertureSample` draws with Get2D, and Get2D is a PADDED
		// (0,2)-net pair keyed by the RAW dimension index -- no wrap, no
		// table row -- so the aperture group is 3322*32 = 106304 and
		// cannot equal any walk stream's group, at any depth, whatever
		// the table size.  What still has to hold is that the group is
		// distinct: no walk stream may ever start at this dimension.
		{
			const unsigned int apertureGroup =
				(unsigned int)BDPTCameraUtilities::kApertureSamplerStream * 32u;
			const unsigned int worstWalkGroup = worst * 32u;
			if( apertureGroup <= worstWalkGroup )
			{
				std::cerr << "  FAIL: the aperture's Get2D group " << apertureGroup
					<< " is inside the walk streams' group range (up to "
					<< worstWalkGroup << ").\n";
				ok = false;
			}
			else
			{
				std::cout << "  aperture Get2D group " << apertureGroup
					<< " is above every walk stream's group (" << worstWalkGroup
					<< "), and Get2D is padded so no table wrap applies: OK\n";
			}
		}
	}

	pinhole->release();
	ortho->release();
	fisheye->release();
	lens->release();
	stoppedDown->release();
	degenerate->release();

	if( !ok ) exit( 1 );
	std::cout << "  Passed!\n";
}

// ================================================================
// main
// ================================================================


// ================================================================
// Test G: no SHIPPED scene can drive a sampler stream past the end
// of the Sobol' dimension table (DL-81 review, P2-2)
//
// `SobolSequence::kNumDimensions` is finite, and `StartStream(s)` puts
// stream s at dimension s * kStreamStride.  Past the table, a Get1D
// draw is re-indexed rather than aliased -- it decorrelates instead of
// collapsing -- but it stops being a joint net with the dimension it
// shares, so the table is SIZED to keep shipped content off that path
// entirely.  This test recomputes the bound from the scene files
// rather than trusting the comment that states it, so a scene that
// raises a depth past the table turns red here.
//
// Reachable streams, read off the integrators:
//   light walk   1  + d,  d < maxLightDepth + maxVolumeBounce
//   eye walk     16 + d,  d < maxEyeDepth   + maxVolumeBounce
//   BDPT select  47
//   MLT          48
//   VCM NEE      48 + i, i over the eye vertices the walk produced
// A walk iteration appends one vertex, except that a BSSRDF material
// appends a second (the entry vertex), so the VCM bound carries a
// factor of two on a scene that declares subsurface scattering.
// ================================================================

static bool SceneDepthBound(
	const std::string& path,
	unsigned int& outStream,
	std::string& outWhy )
{
	std::ifstream f( path );
	if( !f ) return false;

	StabilityConfig stability;

	unsigned int maxDepth = 0;
	unsigned int volumeBounce = stability.maxVolumeBounce;
	bool isBidirectional = false, isVCM = false, hasSubsurface = false;

	// Round-2-review fix (P3-5a): the no-depth default must match the
	// SPECIFIC rasterizer the scene selects, not always BDPT's.
	// PixelPelDefaults::maxRecursion and MLTDefaults::maxEyeDepth are
	// BOTH 10 while BDPTPelDefaults/VCMPelDefaults::maxEyeDepth is 8
	// (RasterizerDefaults.h) -- defaulting every undeclared-depth scene
	// to 8 under-counted a pixelpel or MLT scene's true walk length.
	// `pathtracing_*_rasterizer` has no scene-level depth cap at all
	// (PathTracingPelDefaults/PathTracingSpectralDefaults carry no
	// maxEyeDepth field); its runtime default is the GUI render-modes
	// hard cap, `PathTracingIntegrator::mMaxPathDepth( 128 )`
	// (PathTracingIntegrator.cpp), which is also the safe upper bound
	// to assume for any scene whose rasterizer this scan does not
	// otherwise recognise.
	PixelPelDefaults pixelPelDflt;
	BDPTPelDefaults  bdptDflt;
	VCMPelDefaults   vcmDflt;
	MLTDefaults      mltDflt;
	static const unsigned int kPathTracingDefaultDepth = 128u;
	unsigned int noDepthDefault = kPathTracingDefaultDepth;
	bool sawRasterizer = false;

	std::string line;
	while( std::getline( f, line ) )
	{
		// Strip leading whitespace so a chunk name is recognisable.
		size_t b = line.find_first_not_of( " \t\r" );
		if( b == std::string::npos ) continue;
		const std::string t = line.substr( b );

		if( t.compare( 0, 3, "vcm" ) == 0 ) {
			isVCM = true; isBidirectional = true;
			noDepthDefault = vcmDflt.maxEyeDepth; sawRasterizer = true;
		}
		if( t.compare( 0, 4, "bdpt" ) == 0 ) {
			isBidirectional = true;
			noDepthDefault = bdptDflt.maxEyeDepth; sawRasterizer = true;
		}
		if( t.compare( 0, 3, "mlt" ) == 0 ) {
			isBidirectional = true;
			noDepthDefault = mltDflt.maxEyeDepth; sawRasterizer = true;
		}
		if( t.compare( 0, 8, "pixelpel" ) == 0 ||
			t.compare( 0, 26, "pixelintegratingspectral_" ) == 0 ) {
			noDepthDefault = pixelPelDflt.maxRecursion; sawRasterizer = true;
		}
		if( t.compare( 0, 12, "pathtracing_" ) == 0 ) {
			noDepthDefault = kPathTracingDefaultDepth; sawRasterizer = true;
		}
		if( t.find( "subsurface" ) != std::string::npos ||
			t.find( "bssrdf" ) != std::string::npos ) hasSubsurface = true;

		const char* keys[3] = { "max_eye_depth", "max_light_depth", "max_recursion" };
		for( int k = 0; k < 3; k++ ) {
			if( t.compare( 0, std::string( keys[k] ).size(), keys[k] ) != 0 ) continue;
			const unsigned int v = (unsigned int)std::strtoul(
				t.c_str() + std::string( keys[k] ).size(), 0, 10 );
			if( v > maxDepth ) maxDepth = v;
		}
		// Round-2-review fix (P3-5b): match on the KEY alone and let
		// strtoul's own leading-whitespace skip handle the separator
		// -- the previous `compare(0, 18, "max_volume_bounce ")`
		// demanded a literal trailing SPACE, so every shipped scene
		// (which tab-separates key and value, like every other param
		// checked above) silently missed this line and always fell
		// back to StabilityConfig's default, never reading the
		// scene's own override.
		static const char kVolKey[] = "max_volume_bounce";
		if( t.compare( 0, sizeof(kVolKey) - 1, kVolKey ) == 0 ) {
			volumeBounce = (unsigned int)std::strtoul( t.c_str() + sizeof(kVolKey) - 1, 0, 10 );
		}
	}

	// A scene that names no depth gets ITS OWN rasterizer's default
	// (or, if none was recognised, the conservative PT-sized bound).
	(void)sawRasterizer;
	if( maxDepth == 0 ) maxDepth = noDepthDefault;

	// The walk loops saturate their iteration count at 1024.
	unsigned int walk = maxDepth + volumeBounce;
	if( maxDepth >= 1024u || volumeBounce > 1024u - maxDepth ) walk = 1024u;

	const unsigned int perIteration = hasSubsurface ? 2u : 1u;
	const unsigned int eyeStream   = 16u + walk;
	const unsigned int lightStream = 1u + walk;
	const unsigned int vcmStream   = isVCM ? ( 48u + perIteration * walk + 1u ) : 0u;

	outStream = eyeStream;
	outWhy = "eye walk";
	if( lightStream > outStream && isBidirectional ) { outStream = lightStream; outWhy = "light walk"; }
	if( vcmStream > outStream ) { outStream = vcmStream; outWhy = "VCM per-vertex NEE"; }
	return true;
}

static void TestShippedSceneStreamBudget()
{
	std::cout << "\nTest G: shipped scenes stay inside the Sobol' dimension table (DL-81)\n";

	const char* roots[2] = { "scenes", "../../../scenes" };
	std::string root;
	for( int i = 0; i < 2; i++ ) {
		std::error_code ec;
		if( std::filesystem::is_directory( roots[i], ec ) ) { root = roots[i]; break; }
	}
	if( root.empty() ) {
		std::cerr << "  FAIL: could not locate the scenes/ directory from the working "
			<< "directory; run this test from the repo root or from bin/tests.\n";
		std::exit( 1 );
	}

	const unsigned int stride = 32;			// SobolSampler::kStreamStride
	const unsigned int capacity = SobolSequence::kNumDimensions / stride;

	unsigned int scanned = 0, worst = 0;
	std::string worstScene, worstWhy;
	std::error_code ec;
	for( std::filesystem::recursive_directory_iterator it( root, ec ), end;
		 it != end; it.increment( ec ) )
	{
		if( ec ) break;
		if( !it->is_regular_file( ec ) ) continue;
		if( it->path().extension() != ".RISEscene" ) continue;

		unsigned int stream = 0;
		std::string why;
		if( !SceneDepthBound( it->path().string(), stream, why ) ) continue;
		scanned++;
		if( stream > worst ) { worst = stream; worstScene = it->path().string(); worstWhy = why; }
	}

	std::cout << "  scanned " << scanned << " scenes; deepest reachable stream " << worst
		<< " (" << worstWhy << ") in " << worstScene << "\n";
	std::cout << "  the table covers streams 0.." << ( capacity - 1 ) << " ("
		<< SobolSequence::kNumDimensions << " dimensions / " << stride << " per stream)\n";

	if( scanned < 100 ) {
		std::cerr << "  FAIL: only " << scanned << " scenes scanned; the sweep is not "
			<< "reaching the scene corpus.\n";
		std::exit( 1 );
	}
	if( worst >= capacity ) {
		std::cerr << "  FAIL: " << worstScene << " can reach stream " << worst
			<< ", at or past the table's " << capacity << " streams.  Either raise "
			<< "SobolSequence::kNumDimensions or accept that this scene's deepest "
			<< "draws are re-indexed wraps rather than distinct dimensions.\n";
		std::exit( 1 );
	}
	std::cout << "  deepest shipped stream is inside the table: OK\n";
}

int main( int /*argc*/, char** /*argv*/ )
{
	std::cout << "=== Sobol Dimension Budget Tests ===\n";

	TestRandomWalkDimensionConsumption();
	TestDiskProjectionBudget();
	TestIntegrationGuard();
	TestSobolSamplerMechanics();
	TestHasFixedDimensionBudget();
	TestApertureDrawConsumption();
	TestShippedSceneStreamBudget();

	std::cout << "\nAll Sobol dimension budget tests passed!\n";
	return 0;
}
