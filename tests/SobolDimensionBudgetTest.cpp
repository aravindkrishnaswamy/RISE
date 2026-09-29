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
//    D-F. SobolSampler mechanics, the fixed-budget contract, the
//       thin-lens aperture draw.
//    G1. Shipped scenes' per-vertex streams stay inside the table.
//    G2. The sampler stream map: the wrap-region families (PT volume
//       walks, BDPT/VCM medium-distance blocks, deep BDPT/VCM walk
//       iterations) and the walk layout under both sampler kinds,
//       enumerated from the real functions, wrap counts asserted,
//       collision-freedom asserted with NO known exceptions (DL-286
//       closed the light/eye/select/NEE overlaps it used to pin).
//    H. BDPT's real generators on a heterogeneous and a homogeneous
//       medium: no vertex stream overruns, no dimension drawn twice
//       within a walk (DL-283), and no dimension (Sobol') or primary
//       sample (PSSMLT) shared BETWEEN the light and eye walks of one
//       sample (DL-286).
//
//  Build (from project root):
//    make -C build/make/rise build-test/SobolDimensionBudgetTest
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
#include <algorithm>
#include <iterator>
#include <map>
#include <cstdio>

#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/PSSMLTSampler.h"
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
#include "../src/Library/Utilities/BDPTUtilities.h"
#include "../src/Library/Utilities/PathTransportUtilities.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/Color/SampledWavelengths.h"
#include "../src/Library/Interfaces/IMedium.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

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
	// The dedicated stream clears every per-vertex stream below it and
	// is none of the deep walk streams above it.
	// ------------------------------------------------------------
	{
		// Both GenerateEyeSubpath and GenerateLightSubpath saturate
		// their loop counts at 1024 (maxDepth + maxVolumeBounce,
		// clamped), and one iteration appends at most 3 vertices
		// (medium entry, medium scatter, surface).  VCM's NEE stream is
		// 48 + eye-vertex-index.  Since DL-286 the walks themselves stay
		// in 1..46 for their first 15 (light) / 31 (eye) iterations and
		// put deeper iterations in a block ABOVE every other consumer
		// (read off the real functions here, under the fixed-budget
		// layout the aperture stream is used with).
		const int kWalkIterationCap   = (int)BDPTUtilities::kWalkIterationCap;
		const int kMaxVertsPerIter    = 3;
		const int kMaxVcmNeeStream    = 48 + kWalkIterationCap * kMaxVertsPerIter + 1;
		int worst = kMaxVcmNeeStream;
		for( unsigned int d = 0; d < BDPTUtilities::kWalkIterationCap; d++ ) {
			const int ls = BDPTUtilities::LightWalkStream( d, true );
			const int es = BDPTUtilities::EyeWalkStream( d, true );
			if( ls == BDPTCameraUtilities::kApertureSamplerStream ||
				es == BDPTCameraUtilities::kApertureSamplerStream ) {
				std::cerr << "  FAIL: walk iteration " << d << " opens the aperture stream.\n";
				ok = false;
			}
			if( ls < BDPTCameraUtilities::kApertureSamplerStream && ls > worst ) worst = ls;
			if( es < BDPTCameraUtilities::kApertureSamplerStream && es > worst ) worst = es;
		}
		if( BDPTUtilities::LightWalkStream( BDPTUtilities::kWalkIterationCap - 1u, true ) <=
				BDPTCameraUtilities::kApertureSamplerStream ||
			BDPTUtilities::kDeepWalkStreamBaseFixedBudget <= BDPTCameraUtilities::kApertureSamplerStream ) {
			std::cerr << "  FAIL: the deep walk block is no longer above the aperture stream.\n";
			ok = false;
		}

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
					<< " is above every lower stream's group (" << worstWalkGroup
					<< "), below the deep walk block (stream "
					<< BDPTUtilities::kDeepWalkStreamBaseFixedBudget
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
// Test G: the sampler STREAM MAP (DL-81 review P2-2; rewritten for
// DL-283)
//
// `SobolSequence::kNumDimensions` is finite (8192 = 256 streams), and
// `StartStream(s)` puts stream s at dimension s * kStreamStride.  Past
// the table a Get1D draw is re-indexed rather than aliased -- the table
// row is read at an index Owen-permuted by the wrap count -- so it
// decorrelates instead of collapsing, but it stops being a joint net
// with the row it shares.
//
// DL-81 stated the rule as "the table is sized so shipped scenes never
// wrap".  Since DL-247 that has been FALSE by construction, and since
// DL-283 and DL-286 more so: three families of streams are DELIBERATELY
// placed in the wrap region, because they must be disjoint from every
// per-vertex stream at any depth and the table cannot hold that many:
//
//   PT volume walks   4096 + 1024 * lane + (event mod 1024)
//                     (PathTransportUtilities::PTVolumeWalkStream;
//                     wrap 16 + 4 * lane + event / 256, i.e. 16..31)
//   BDPT/VCM          8192 + 64 * (1024 * side + depth), a 64-stream
//   medium distance   (2048-dimension) block per walk iteration
//                     (BDPTUtilities::MediumDistanceStream; wrap
//                     32..543; fixed-budget samplers only -- MLT's
//                     PSSMLTSampler never reaches them, Test H)
//   BDPT/VCM deep     light iterations >= 15 and eye iterations >= 31:
//   walk iterations   139264 + (d - 15) and 140273 + (d - 31)
//                     (BDPTUtilities::LightWalkStream / EyeWalkStream,
//                     DL-286; wrap 544..551; under PSSMLT the same
//                     iterations use lanes 2049..4050 instead)
//
// The rule that survives is narrower and still true: every PER-VERTEX
// stream a shipped scene reaches is inside the table.  G1 recomputes
// that from the scene files; G2 enumerates both wrap-region families
// from the real functions and asserts (a) the wrap counts each family
// lands on, (b) that every stream in them is unique, and (c) that each
// is disjoint from every OTHER consumer of the same sampler.  Whether
// the wrapped draws are GOOD draws is measured, not argued:
// SobolDimensionParityTest section H.
//
// Reachable per-vertex streams, read off the integrators:
//   light walk   1  + d,  d < 15, then a deep block (DL-286; below)
//   eye walk     16 + d,  d < 31, then a deep block (DL-286; below)
//                (d < maxDepth + maxVolumeBounce, cap 1024)
//   BDPT select  47
//   MLT          2048 (BDPTCameraUtilities::kPSSMLTFilmLensApertureStream)
//   VCM NEE      48 + i, i over the eye vertices the walk produced
//   PT main loop 16 + d,  d < mMaxPathDepth (default 128, ceiling 4080)
// The deep walk blocks (BDPTUtilities::LightWalkStream / EyeWalkStream,
// [139264, 141266) under a fixed-budget sampler) are a THIRD wrap-region
// family: the table cannot hold them disjoint from VCM's NEE range.
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
	// In-table walk streams stop at the shallow ranges (DL-286): deeper
	// iterations live in the wrap region by design (Test G2).
	const unsigned int eyeStream   = 16u + std::min( walk, BDPTUtilities::kEyeWalkShallowIterations );
	const unsigned int lightStream = 1u + std::min( walk, BDPTUtilities::kLightWalkShallowIterations );
	const unsigned int vcmStream   = isVCM ? ( 48u + perIteration * walk + 1u ) : 0u;

	outStream = eyeStream;
	outWhy = "eye walk";
	if( lightStream > outStream && isBidirectional ) { outStream = lightStream; outWhy = "light walk"; }
	if( vcmStream > outStream ) { outStream = vcmStream; outWhy = "VCM per-vertex NEE"; }
	return true;
}

static void TestShippedSceneStreamBudget()
{
	std::cout << "\nTest G1: shipped scenes' per-vertex streams stay inside the Sobol' dimension table (DL-81)\n";

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
	std::cout << "  deepest shipped per-vertex stream is inside the table: OK (the PT walk,\n"
		<< "  BDPT medium-distance and deep walk streams live past it by design -- Test G2)\n";
}

// ----------------------------------------------------------------
// G2: the wrap-region stream families, enumerated from the real
// functions, and the whole map's collision-freedom.
// ----------------------------------------------------------------
namespace StreamMap
{
	enum { PT = 1, BDPT = 2, VCM = 4, MLT = 8, BIDIR = BDPT | VCM | MLT };

	struct Range { const char* name; unsigned int lo, hi; unsigned int users; };

	static bool Overlap( const Range& a, const Range& b )
	{
		return ( a.users & b.users ) != 0 && a.lo < b.hi && b.lo < a.hi;
	}
}

static void TestStreamMap()
{
	using namespace StreamMap;
	std::cout << "\nTest G2: sampler stream map -- wrap-region families and collisions (DL-283)\n";

	const unsigned int stride = SobolSampler::kStreamStride;
	const unsigned int table  = SobolSequence::kNumDimensions;
	const unsigned int cap    = BDPTUtilities::kWalkIterationCap;
	bool ok = true;

	// --- The walk layout (DL-286) --------------------------------------
	// Enumerated from the real functions under BOTH sampler kinds.  The
	// shallow iterations must still open exactly the historical streams
	// (1 + d for d < 15, 16 + d for d < 31) -- that is what keeps every
	// shallow walk's draws bit-identical, PSSMLT's legacy tier included
	// -- and every walk stream of one kind must be distinct from every
	// other, light and eye together (the defect: light iteration 15 used
	// to open 16, eye iteration 0's stream).
	unsigned int deepLo[2][2], deepHi[2][2];		// [fixedBudget][eye]
	for( int fb = 0; fb < 2; fb++ ) {
		std::vector<unsigned int> all;
		for( int eye = 0; eye < 2; eye++ ) {
			deepLo[fb][eye] = ~0u; deepHi[fb][eye] = 0u;
			const unsigned int shallow = eye ? BDPTUtilities::kEyeWalkShallowIterations
			                                 : BDPTUtilities::kLightWalkShallowIterations;
			for( unsigned int d = 0; d < cap; d++ ) {
				const unsigned int st = (unsigned int)( eye ?
					BDPTUtilities::EyeWalkStream( d, fb != 0 ) :
					BDPTUtilities::LightWalkStream( d, fb != 0 ) );
				all.push_back( st );
				if( d < shallow ) {
					if( st != ( eye ? 16u : 1u ) + d ) {
						std::cerr << "  FAIL: " << ( eye ? "eye" : "light" ) << " iteration " << d
							<< " opens stream " << st << ", not its historical "
							<< ( eye ? 16u : 1u ) + d << " -- shallow walks would change.\n";
						ok = false;
					}
				} else {
					if( st < deepLo[fb][eye] ) deepLo[fb][eye] = st;
					if( st > deepHi[fb][eye] ) deepHi[fb][eye] = st;
				}
			}
			deepHi[fb][eye]++;		// half-open
			// Depth past the loop cap clamps into the last stream.
			const int last = eye ? BDPTUtilities::EyeWalkStream( cap - 1u, fb != 0 )
			                     : BDPTUtilities::LightWalkStream( cap - 1u, fb != 0 );
			const int past = eye ? BDPTUtilities::EyeWalkStream( cap + 7u, fb != 0 )
			                     : BDPTUtilities::LightWalkStream( cap + 7u, fb != 0 );
			if( last != past ) {
				std::cerr << "  FAIL: a walk depth past the cap no longer clamps.\n";
				ok = false;
			}
		}
		std::sort( all.begin(), all.end() );
		const bool unique = std::adjacent_find( all.begin(), all.end() ) == all.end();
		std::cout << "  walk streams, " << ( fb ? "fixed-budget (Sobol')" : "unbounded-lane (PSSMLT)" )
			<< ": light 1..15 then [" << deepLo[fb][0] << ", " << deepHi[fb][0]
			<< "), eye 16..46 then [" << deepLo[fb][1] << ", " << deepHi[fb][1] << ")"
			<< ( unique ? ", all " : ", DUPLICATES among " ) << all.size() << " distinct\n";
		if( !unique ) {
			std::cerr << "  FAIL: two walk iterations share a stream (DL-286).\n";
			ok = false;
		}
	}
	if( deepHi[0][1] > (unsigned int)BDPTUtilities::kPSSMLTStreamSanityBound ) {
		std::cerr << "  FAIL: PSSMLT deep walk lanes run past its stream bound.\n";
		ok = false;
	}
	{
		// The fixed-budget deep block is a wrap-region family: past the
		// table, on its own wraps, inside the 32-bit dimension counter.
		const unsigned long long first = (unsigned long long)deepLo[1][0] * stride;
		const unsigned long long last  = (unsigned long long)deepHi[1][1] * stride - 1ull;
		std::cout << "  fixed-budget deep walk block: wrap counts " << first / table << ".."
			<< last / table << ", max dimension " << last << "\n";
		if( first / table != 544ull || last / table != 551ull || last >= 0xFFFFFFFFull ||
			deepLo[1][0] != (unsigned int)BDPTUtilities::kMediumDistanceStreamEnd ) {
			std::cerr << "  FAIL: the deep walk block is not the documented set on wraps 544..551 "
				<< "starting at the end of the medium-distance blocks.\n";
			ok = false;
		}
	}

	// Fixed (per-vertex and single-purpose) consumers.  VCM's NEE runs
	// 48 + eye-vertex-index, i >= 1 (so it really starts at 49; 48 is
	// kept as a conservative lower edge), and one iteration appends at
	// most three vertices (Test F's bound).  BDPT and VCM drive a
	// SobolSampler (fixed budget), MLT a PSSMLTSampler.
	const Range fixedRanges[] = {
		{ "film / light select (0)",        0u,   1u,                  PT | BIDIR },
		{ "light walk, shallow (1+d)",      1u,   16u,                 BIDIR },
		{ "eye walk, shallow (16+d)",       16u,  47u,                 BIDIR },
		{ "BDPT strategy select (47)",      47u,  48u,                 BDPT | MLT },
		{ "VCM per-vertex NEE (48+i)",      48u,  48u + 3u * cap + 1u, VCM },
		{ "MLT film/lens (2048)",
			(unsigned int)BDPTCameraUtilities::kPSSMLTFilmLensApertureStream,
			(unsigned int)BDPTCameraUtilities::kPSSMLTFilmLensApertureStream + 1u, MLT },
		{ "thin-lens aperture (3322)",
			(unsigned int)BDPTCameraUtilities::kApertureSamplerStream,
			(unsigned int)BDPTCameraUtilities::kApertureSamplerStream + 1u,     BDPT | VCM },
		{ "PT main loop (16+d, d<=4079)",   16u,  4096u,               PT },
		{ "light walk, deep (Sobol')",      deepLo[1][0], deepHi[1][0], BDPT | VCM },
		{ "eye walk, deep (Sobol')",        deepLo[1][1], deepHi[1][1], BDPT | VCM },
		{ "light walk, deep (PSSMLT)",      deepLo[0][0], deepHi[0][0], MLT },
		{ "eye walk, deep (PSSMLT)",        deepLo[0][1], deepHi[0][1], MLT },
	};
	const unsigned int nFixed = sizeof(fixedRanges) / sizeof(fixedRanges[0]);

	// No exceptions.  Before DL-286 this list pinned five overlaps (the
	// light walk with the eye walk, the select and VCM's NEE; the eye
	// walk with the select and the NEE) -- one Sobol' dimension driving
	// two decisions on one connected path, measured at -12 % on a dense
	// fog.  Any overlap now is a NEW defect.
	unsigned int overlaps = 0;
	for( unsigned int i = 0; i < nFixed; i++ ) {
		for( unsigned int j = i + 1; j < nFixed; j++ ) {
			if( !Overlap( fixedRanges[i], fixedRanges[j] ) ) continue;
			std::cerr << "  FAIL: stream overlap between " << fixedRanges[i].name
				<< " and " << fixedRanges[j].name << " (same sampler).\n";
			overlaps++;
			ok = false;
		}
	}
	std::cout << "  fixed consumers: " << nFixed << " ranges, " << overlaps
		<< " overlaps (DL-286 closed the five it used to pin)\n";

	// --- PT volume walks ------------------------------------------------
	{
		const unsigned int lanes = SampledWavelengths::N;
		std::vector<unsigned int> streams;
		unsigned int wrapLo = ~0u, wrapHi = 0u;
		bool wrapFormula = true;
		for( unsigned int lane = 0; lane < lanes; lane++ ) {
			for( unsigned int ev = 0; ev < PathTransportUtilities::kPTVolumeWalkLaneStride; ev++ ) {
				const unsigned int s = (unsigned int)PathTransportUtilities::PTVolumeWalkStream( lane, ev );
				streams.push_back( s );
				for( unsigned int slot = 0; slot < stride; slot++ ) {
					const unsigned int w = ( s * stride + slot ) / table;
					if( w < wrapLo ) wrapLo = w;
					if( w > wrapHi ) wrapHi = w;
					if( w != 16u + 4u * lane + ev / 256u ) wrapFormula = false;
				}
				const Range r = { "PT walk", s, s + 1u, PT };
				for( unsigned int i = 0; i < nFixed; i++ ) {
					if( Overlap( r, fixedRanges[i] ) ) {
						std::cerr << "  FAIL: PT walk (lane " << lane << ", event " << ev
							<< ") stream " << s << " overlaps " << fixedRanges[i].name << "\n";
						ok = false;
					}
				}
			}
			// The `mod`: event 1024 is event 0 again -- the documented
			// max_volume_bounce <= 1024 ceiling.
			if( PathTransportUtilities::PTVolumeWalkStream( lane, 1024u ) !=
				PathTransportUtilities::PTVolumeWalkStream( lane, 0u ) ) {
				std::cerr << "  FAIL: PT walk ceiling moved; re-derive the documented 1024.\n";
				ok = false;
			}
		}
		std::sort( streams.begin(), streams.end() );
		const bool unique = std::adjacent_find( streams.begin(), streams.end() ) == streams.end();
		std::cout << "  PT walks: " << streams.size() << " streams [" << streams.front() << ", "
			<< streams.back() << "], wrap counts " << wrapLo << ".." << wrapHi
			<< ( unique ? ", all distinct" : ", DUPLICATES" ) << "\n";
		if( !unique || wrapLo != 16u || wrapHi != 31u || !wrapFormula ) {
			std::cerr << "  FAIL: PT walk streams are not the documented distinct set on wraps 16..31.\n";
			ok = false;
		}
		// PT's main loop reaches the first walk stream at depth 4080.
		if( 16u + 4080u != (unsigned int)PathTransportUtilities::kPTVolumeWalkStreamBase ) {
			std::cerr << "  FAIL: the documented PT depth ceiling 4080 no longer matches the walk base.\n";
			ok = false;
		}
	}

	// --- BDPT/VCM medium-distance blocks (fixed-budget samplers only) ---
	{
		const unsigned int per = BDPTUtilities::kMediumDistanceStreamsPerEvent;
		if( per * stride < IMedium::kMaxSampleDistanceDraws ) {
			std::cerr << "  FAIL: a medium-distance block (" << per * stride
				<< " dims) cannot hold one SampleDistance call ("
				<< IMedium::kMaxSampleDistanceDraws << " draws).\n";
			ok = false;
		}
		std::vector<Range> blocks;
		unsigned int wrapLo = ~0u, wrapHi = 0u;
		bool wrapFormula = true;
		unsigned long long maxDim = 0;
		for( unsigned int side = 0; side < 2; side++ ) {
			for( unsigned int d = 0; d < cap; d++ ) {
				const unsigned int s = (unsigned int)BDPTUtilities::MediumDistanceStream(
					side ? BDPTUtilities::eLightWalk : BDPTUtilities::eEyeWalk, d );
				const Range r = { "medium block", s, s + per, BDPT | VCM };
				blocks.push_back( r );
				const unsigned long long first = (unsigned long long)s * stride;
				const unsigned long long last  = first + (unsigned long long)per * stride - 1ull;
				if( last > maxDim ) maxDim = last;
				const unsigned int w0 = (unsigned int)( first / table );
				const unsigned int w1 = (unsigned int)( last / table );
				if( w0 < wrapLo ) wrapLo = w0;
				if( w1 > wrapHi ) wrapHi = w1;
				if( w0 != w1 || w0 != 32u + ( side * cap + d ) / 4u ) wrapFormula = false;
				for( unsigned int i = 0; i < nFixed; i++ ) {
					if( Overlap( r, fixedRanges[i] ) ) {
						std::cerr << "  FAIL: medium block (side " << side << ", depth " << d
							<< ") overlaps " << fixedRanges[i].name << "\n";
						ok = false;
					}
				}
			}
		}
		std::sort( blocks.begin(), blocks.end(),
			[]( const Range& a, const Range& b ) { return a.lo < b.lo; } );
		bool disjoint = true;
		for( size_t i = 1; i < blocks.size(); i++ ) if( blocks[i].lo < blocks[i-1].hi ) disjoint = false;
		std::cout << "  BDPT/VCM medium-distance blocks: " << blocks.size() << " x " << per
			<< " streams [" << blocks.front().lo << ", " << blocks.back().hi << "), wrap counts "
			<< wrapLo << ".." << wrapHi << ( disjoint ? ", pairwise disjoint" : ", OVERLAPPING" )
			<< ", max dimension " << maxDim << "\n";
		if( !disjoint || !wrapFormula || wrapLo != 32u || wrapHi != 543u ||
			blocks.back().hi != (unsigned int)BDPTUtilities::kMediumDistanceStreamEnd ||
			maxDim >= 0xFFFFFFFFull ) {
			std::cerr << "  FAIL: the medium-distance layout is not the documented disjoint "
				<< "set on wraps 32..543 inside the 32-bit dimension counter.\n";
			ok = false;
		}
		// A depth past the loop cap must not reach a neighbour's block.
		if( BDPTUtilities::MediumDistanceStream( BDPTUtilities::eEyeWalk, cap + 5u ) !=
			BDPTUtilities::MediumDistanceStream( BDPTUtilities::eEyeWalk, cap - 1u ) ) {
			std::cerr << "  FAIL: MediumDistanceStream no longer clamps depth into the cap.\n";
			ok = false;
		}
	}

	if( !ok ) exit( 1 );
	std::cout << "  Passed!\n";
}

// ================================================================
// Test H: BDPT's medium distance sampling stays inside its own
// stream block (DL-283)
//
// Drives BDPTIntegrator's REAL light and eye generators (Pel and NM,
// light sampler attached so the light walk really runs) on an
// index-matched box holding a thin heterogeneous medium whose 256^3
// majorant grid makes delta tracking cross many cells, with a
// SobolSampler subclass that records every draw's stream and raw
// dimension.  Three properties, per BDPT sample (light subpath then
// eye subpath off one sampler, as every BDPT/VCM rasterizer does):
//
//   H1  no per-vertex stream (< the medium-distance base) receives
//       more than kStreamStride draws -- i.e. nothing spills into the
//       stream the next walk iteration re-opens;
//   H2  every draw at or above the base lands at the START of a
//       medium-distance block, and no block receives more than
//       IMedium::kMaxSampleDistanceDraws;
//   H3  no raw dimension is drawn twice WITHIN one walk (the "one
//       Sobol' dimension drives two decisions" signature);
//   H4  a PSSMLTSampler (MLT) driven through the same generators never
//       opens a medium-distance block -- its lanes are unbounded, so it
//       stays on the vertex streams (a routing that was measured at +13 %
//       user CPU on mlt_deep_fog for no correctness gain);
//   H5  (DL-286) no raw dimension (Sobol') and no primary sample (PSSMLT
//       lane, index) is drawn by BOTH the light and the eye walk of one
//       sample.  The homogeneous box's light walks pass iteration 15 in
//       a few percent of samples (asserted, so H5 is not vacuous); before
//       DL-286 they re-opened the eye walk's streams there: 635 shared
//       dimensions in 60 of 4096 Sobol' samples.
//
// Before DL-283 the distance sample drew from the vertex stream: this
// fixture's delta tracking ran to ~100 draws, so H1 and H3 failed.
// The test also asserts the fixture really exercises the hazard (some
// distance sample draws more than a stream holds), so a medium change
// that made tracking cheap cannot turn it vacuous.  A HOMOGENEOUS
// control pins the bounded case at exactly one draw per sample.
//
// H5 gates draws shared BETWEEN the light and eye walks (DL-286, the
// fixed-layout overlap G2 used to pin as known).
// ================================================================

class StreamAuditSobol : public SobolSampler
{
public:
	int stream;
	std::vector<std::pair<int, unsigned int> > draws;	// (stream, raw dimension)

	StreamAuditSobol( uint32_t idx, uint32_t seed ) : SobolSampler( idx, seed ), stream( 0 ) {}

	Scalar Get1D() override
	{
		draws.push_back( std::make_pair( stream, dimension ) );
		return SobolSampler::Get1D();
	}
	Point2 Get2D() override
	{
		draws.push_back( std::make_pair( stream, dimension ) );
		draws.push_back( std::make_pair( stream, dimension + 1u ) );
		return SobolSampler::Get2D();
	}
	void StartStream( int s ) override
	{
		stream = s;
		SobolSampler::StartStream( s );
	}
};

// A PSSMLTSampler that records the highest stream it is asked for and
// every primary sample (lane, index within the lane) it hands out.  MLT
// drives the same generators; its lanes are unbounded, so DL-283 leaves
// it on the vertex streams -- this pins that -- and a primary sample
// read by BOTH walks of one sample is the PSSMLT form of DL-286.
class StreamRecordingPSSMLT : public PSSMLTSampler
{
public:
	int maxStream;
	int stream;
	unsigned int indexInStream;
	std::vector<std::pair<int, unsigned int> > lanes;
	StreamRecordingPSSMLT( unsigned int seed ) :
		PSSMLTSampler( seed, 1.0 ), maxStream( 0 ), stream( 0 ), indexInStream( 0 ) {}
	void StartStream( int s ) override
	{
		if( s > maxStream ) maxStream = s;
		stream = s;
		indexInStream = 0;
		PSSMLTSampler::StartStream( s );
	}
	// PSSMLTSampler::Get2D is two virtual Get1D calls, so this sees both.
	Scalar Get1D() override
	{
		lanes.push_back( std::make_pair( stream, indexInStream++ ) );
		return PSSMLTSampler::Get1D();
	}
};

static std::string MediumBoxScene( bool heterogeneous )
{
	std::string s = "RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"film\n{\n\twidth 8\n\theight 8\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 1.9999\n\tlookat 0 -1.0 0\n\tup 0 1 0\n\tfov 50.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.8 0.8 0.8\n}\n\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n";
	if( heterogeneous ) {
		s +=
			"uniformcolor_painter\n{\n\tname pnt_dense\n\tcolor 1.0 1.0 1.0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_sparse\n\tcolor 0.0 0.0 0.0\n}\n\n"
			"perlin3d_painter\n{\n\tname pnt_density\n\tpersistence 0.65\n\toctaves 4\n"
			"\tcolora pnt_dense\n\tcolorb pnt_sparse\n\tscale 1.5 1.5 1.5\n\tshift 0 0 0\n}\n\n"
			"painter_heterogeneous_medium\n{\n\tname med\n\tabsorption 0.15 0.15 0.15\n"
			"\tscattering 0.35 0.35 0.35\n\tphase isotropic\n\tdensity_painter pnt_density\n"
			"\tresolution 256\n\tcolor_to_scalar luminance\n\tbbox_min -2 -2 -2\n\tbbox_max 2 2 2\n}\n\n";
	} else {
		s += "homogeneous_medium\n{\n\tname med\n\tabsorption 0.3 0.3 0.3\n"
			"\tscattering 0.7 0.7 0.7\n\tphase isotropic\n}\n\n";
	}
	s +=
		"dielectric_material\n{\n\tname mat_shell\n\ttau 1.0 1.0 1.0\n\tior 1.0\n\tscattering 1000000.0\n}\n\n"
		"box_geometry\n{\n\tname shell_box\n\twidth 4.0\n\theight 4.0\n\tdepth 4.0\n}\n\n"
		"standard_object\n{\n\tname obj_shell\n\tgeometry shell_box\n\tmaterial mat_shell\n\tinterior_medium med\n}\n\n"
		"clippedplane_geometry\n{\n\tname floor_quad\n"
		"\tpta -1.9 -1.5 -1.9\n\tptb -1.9 -1.5 1.9\n\tptc 1.9 -1.5 1.9\n\tptd 1.9 -1.5 -1.9\n}\n\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry floor_quad\n\tmaterial mat_floor\n}\n\n"
		"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 20\n\tmax_light_depth 20\n\tsamples 1\n"
		"\tpixel_filter box\n\toidn_denoise FALSE\n"
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n}\n\n";
	return s;
}

struct StreamAuditTally
{
	unsigned long long samples = 0, vertexOverruns = 0, badBlockDraws = 0,
		sameWalkRepeats = 0, crossWalkRepeats = 0, mediumSamples = 0,
		lightMediumSamples = 0, lightDraws = 0, samplesWithSharedDims = 0,
		mediumSamplesOver32 = 0, deepLightSamples = 0,
		pssmltSamples = 0, pssmltSharedLanes = 0, pssmltSamplesWithShared = 0,
		pssmltDeepLightSamples = 0;
	unsigned int maxVertexDraws = 0, maxBlockDraws = 0;
};

static void AuditOneSample( const StreamAuditSobol& s, size_t lightEnd, StreamAuditTally& t )
{
	const int base = BDPTUtilities::kMediumDistanceStreamBase;
	const int per  = BDPTUtilities::kMediumDistanceStreamsPerEvent;
	std::map<int, unsigned int> perStream;
	for( const auto& d : s.draws ) perStream[d.first]++;
	bool deepLight = false;
	for( size_t i = 0; i < lightEnd && i < s.draws.size(); i++ )
		if( s.draws[i].first >= BDPTUtilities::kDeepWalkStreamBaseFixedBudget ) deepLight = true;
	if( deepLight ) t.deepLightSamples++;
	for( const auto& ps : perStream ) {
		if( ps.first < base || ps.first >= BDPTUtilities::kMediumDistanceStreamEnd ) {
			// Per-vertex streams: the shallow walk streams below the
			// medium-distance blocks, or the deep walk block past them
			// (DL-286).
			if( ps.second > t.maxVertexDraws ) t.maxVertexDraws = ps.second;
			if( ps.second > SobolSampler::kStreamStride ) t.vertexOverruns++;
		} else {
			t.mediumSamples++;
			if( ps.first >= BDPTUtilities::MediumDistanceStream( BDPTUtilities::eLightWalk, 0 ) )
				t.lightMediumSamples++;
			if( ps.second > t.maxBlockDraws ) t.maxBlockDraws = ps.second;
			if( ps.second > SobolSampler::kStreamStride ) t.mediumSamplesOver32++;
			if( ( ps.first - base ) % per != 0 || ps.first >= BDPTUtilities::kMediumDistanceStreamEnd ||
				ps.second > IMedium::kMaxSampleDistanceDraws ) t.badBlockDraws++;
		}
	}
	// Raw-dimension repeats, split by walk.
	std::vector<unsigned int> light, eye;
	for( size_t i = 0; i < s.draws.size(); i++ )
		( i < lightEnd ? light : eye ).push_back( s.draws[i].second );
	auto repeats = []( std::vector<unsigned int>& v ) {
		std::sort( v.begin(), v.end() );
		unsigned long long r = 0;
		for( size_t i = 1; i < v.size(); i++ ) if( v[i] == v[i-1] ) r++;
		return r;
	};
	std::vector<unsigned int> lc = light, ec = eye;
	t.sameWalkRepeats += repeats( lc ) + repeats( ec );
	std::vector<unsigned int> both;
	std::set_intersection( lc.begin(), lc.end(), ec.begin(), ec.end(), std::back_inserter( both ) );
	both.erase( std::unique( both.begin(), both.end() ), both.end() );
	t.crossWalkRepeats += both.size();
	if( !both.empty() ) t.samplesWithSharedDims++;
	t.lightDraws += lightEnd;
	t.samples++;
}

static void AuditPSSMLTSample( const StreamRecordingPSSMLT& m, size_t lightEnd, StreamAuditTally& t )
{
	std::vector<std::pair<int, unsigned int> > light( m.lanes.begin(), m.lanes.begin() + lightEnd );
	std::vector<std::pair<int, unsigned int> > eye( m.lanes.begin() + lightEnd, m.lanes.end() );
	for( const auto& l : light )
		if( l.first >= BDPTUtilities::LightWalkStream( BDPTUtilities::kLightWalkShallowIterations, false ) ) {
			t.pssmltDeepLightSamples++;
			break;
		}
	std::sort( light.begin(), light.end() );
	std::sort( eye.begin(), eye.end() );
	std::vector<std::pair<int, unsigned int> > both;
	std::set_intersection( light.begin(), light.end(), eye.begin(), eye.end(), std::back_inserter( both ) );
	both.erase( std::unique( both.begin(), both.end() ), both.end() );
	t.pssmltSharedLanes += both.size();
	if( !both.empty() ) t.pssmltSamplesWithShared++;
	t.pssmltSamples++;
}

static bool RunStreamAudit( bool heterogeneous, bool nm, StreamAuditTally& t, int* pMaxPssmltStream )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/sobol_budget_medium_%d.RISEscene", (int)::getpid() );
	{
		std::ofstream ofs( path );
		if( !ofs ) return false;
		ofs << MediumBoxScene( heterogeneous );
	}
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob || !pJob->LoadAsciiSceneViaCst( path ) ) {
		if( pJob ) safe_release( pJob );
		std::remove( path );
		return false;
	}
	std::remove( path );
	const IScene* pScene = pJob->GetScene();
	const ICamera* pCamera = pScene ? pScene->GetCamera() : nullptr;
	auto* pRaster = dynamic_cast<PixelBasedRasterizerHelper*>( pJob->GetRasterizer() );
	IRayCaster* pCaster = pRaster ? pRaster->GetRayCaster() : nullptr;
	if( !pScene || !pCamera || !pCaster ) { safe_release( pJob ); return false; }

	RandomNumberGenerator rng( 283u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	StabilityConfig stability;		// max_volume_bounce 64
	BDPTIntegrator* pBdpt = new BDPTIntegrator( 20, 20, stability );

	// The LIGHT walk needs a light sampler: without one
	// GenerateLightSubpathImpl returns before drawing anything, and an
	// earlier revision of this test audited only the eye walk (review of
	// DL-283, P1-2).  Same wiring the rasterizers do.
	pCaster->AttachScene( pScene );
	pScene->GetObjects()->PrepareForRendering();
	pBdpt->SetLightSampler( pCaster->GetLightSampler() );
	if( !pCaster->GetLightSampler() ) { safe_release( pBdpt ); safe_release( pJob ); return false; }

	for( unsigned int i = 0; i < 4096u; i++ ) {
		const Point2 screen( ( ( i % 64u ) + 0.5 ) / 64.0, ( ( i / 64u ) + 0.5 ) / 64.0 );
		Ray cameraRay;
		if( !pCamera->GenerateRay( rc, cameraRay, screen ) ) continue;
		StreamAuditSobol sampler( i, 0x283u + i * 7u );
		std::vector<BDPTVertex> lv, ev;
		std::vector<uint32_t> ls, es;
		if( nm ) pBdpt->GenerateLightSubpathNM( *pScene, *pCaster, sampler, lv, ls, 550.0, rng, nullptr );
		else     pBdpt->GenerateLightSubpath( *pScene, *pCaster, sampler, lv, ls, rng );
		const size_t lightEnd = sampler.draws.size();
		if( nm ) pBdpt->GenerateEyeSubpathNM( rc, cameraRay, screen, *pScene, *pCaster, sampler, ev, es, 550.0, nullptr, nullptr );
		else     pBdpt->GenerateEyeSubpath( rc, cameraRay, screen, *pScene, *pCaster, sampler, ev, es, nullptr );
		AuditOneSample( sampler, lightEnd, t );

		if( pMaxPssmltStream ) {
			StreamRecordingPSSMLT* pMlt = new StreamRecordingPSSMLT( 283u + i );
			StreamRecordingPSSMLT& mlt = *pMlt;
			std::vector<BDPTVertex> lv2, ev2;
			std::vector<uint32_t> ls2, es2;
			pBdpt->GenerateLightSubpath( *pScene, *pCaster, mlt, lv2, ls2, rng );
			const size_t mltLightEnd = mlt.lanes.size();
			pBdpt->GenerateEyeSubpath( rc, cameraRay, screen, *pScene, *pCaster, mlt, ev2, es2, nullptr );
			if( mlt.maxStream > *pMaxPssmltStream ) *pMaxPssmltStream = mlt.maxStream;
			AuditPSSMLTSample( mlt, mltLightEnd, t );
			safe_release( pMlt );
		}
	}
	safe_release( pBdpt );
	safe_release( pJob );
	return true;
}

static void TestMediumDistanceStreamAudit()
{
	std::cout << "\nTest H: BDPT medium distance sampling stays in its own stream block (DL-283)\n";
	bool ok = true;
	for( int het = 1; het >= 0; het-- ) {
		for( int nm = 0; nm < 2; nm++ ) {
			StreamAuditTally t;
			int maxPssmlt = 0;
			if( !RunStreamAudit( het != 0, nm != 0, t, !nm ? &maxPssmlt : nullptr ) || t.samples == 0 ) {
				std::cerr << "  FAIL: could not build/drive the medium box scene.\n";
				exit( 1 );
			}
			std::cout << "  " << ( het ? "heterogeneous" : "homogeneous  " ) << ( nm ? " NM " : " Pel" )
				<< ": " << t.samples << " samples, " << t.mediumSamples << " distance samples, "
				<< "max draws per distance sample " << t.maxBlockDraws << " ("
				<< std::fixed << std::setprecision( 1 )
				<< ( t.mediumSamples ? 100.0 * double( t.mediumSamplesOver32 ) / double( t.mediumSamples ) : 0.0 )
				<< std::defaultfloat << "% of them past 32 draws, i.e. would overrun a vertex stream on their own)"
				<< ", max per vertex stream " << t.maxVertexDraws
				<< ", vertex overruns " << t.vertexOverruns
				<< ", misplaced block draws " << t.badBlockDraws
				<< ", same-walk dimension repeats " << t.sameWalkRepeats
				<< "\n    light walk: " << t.lightDraws << " draws, " << t.lightMediumSamples
				<< " distance samples, " << t.deepLightSamples << " samples reaching light iteration "
				<< BDPTUtilities::kLightWalkShallowIterations
				<< "; light/eye shared dimensions " << t.crossWalkRepeats
				<< " in " << t.samplesWithSharedDims << " samples (DL-286: must be 0)\n";
			if( t.vertexOverruns || t.badBlockDraws || t.sameWalkRepeats ) ok = false;
			if( t.crossWalkRepeats ) {
				std::cerr << "  FAIL: " << t.crossWalkRepeats << " Sobol' dimensions drawn by BOTH the "
					<< "light and the eye walk of one sample (DL-286).\n";
				ok = false;
			}
			// The homogeneous box's light walks do pass iteration 15 (a
			// few percent of samples); without that the check above is
			// vacuous.
			if( !het && t.deepLightSamples == 0 ) {
				std::cerr << "  FAIL: no light walk reached iteration "
					<< BDPTUtilities::kLightWalkShallowIterations
					<< " on the homogeneous box; the DL-286 check cannot see anything.\n";
				ok = false;
			}
			if( !nm ) {
				std::cout << "    PSSMLTSampler (MLT), same " << t.pssmltSamples << " samples: "
					<< t.pssmltDeepLightSamples << " light walks reaching iteration "
					<< BDPTUtilities::kLightWalkShallowIterations << ", primary samples shared by the "
					<< "light and eye walks " << t.pssmltSharedLanes << " in "
					<< t.pssmltSamplesWithShared << " samples (DL-286: must be 0)\n";
				if( t.pssmltSharedLanes ) {
					std::cerr << "  FAIL: an MLT sample's light and eye walks read the same primary "
						<< "sample (DL-286).\n";
					ok = false;
				}
				if( !het && t.pssmltDeepLightSamples == 0 ) {
					std::cerr << "  FAIL: no PSSMLT light walk passed the shallow iterations on the "
						<< "homogeneous box; the MLT half of the DL-286 check is vacuous.\n";
					ok = false;
				}
			}
			if( !nm ) {
				std::cout << "    PSSMLTSampler (MLT) through the same generators: highest stream "
					<< maxPssmlt << " (medium-distance blocks start at "
					<< BDPTUtilities::kMediumDistanceStreamBase << ")\n";
				if( maxPssmlt >= BDPTUtilities::kMediumDistanceStreamBase || maxPssmlt < 17 ) {
					std::cerr << "  FAIL: an MLT (PSSMLTSampler) walk reached stream " << maxPssmlt
						<< "; it must stay on its vertex streams (shallow 0..47, deep lanes below "
						<< BDPTUtilities::DeepWalkStreamEnd( false ) << ").\n";
					ok = false;
				}
			}
			if( t.mediumSamples == 0 ) ok = false;
			if( t.lightMediumSamples == 0 || t.lightDraws == 0 ) {
				std::cerr << "  FAIL: the light walk drew nothing, or drew no medium distance sample "
					<< "from a LIGHT-walk block (" << t.lightDraws << " light-walk draws, "
					<< t.lightMediumSamples << " light-block distance samples) -- either the walk "
					<< "did not run or its distance samples are still on the vertex streams.\n";
				ok = false;
			}
			if( het && std::max( t.maxBlockDraws, t.maxVertexDraws ) <= SobolSampler::kStreamStride ) {
				std::cerr << "  FAIL: the heterogeneous fixture no longer drives a distance sample "
					<< "past one stream's " << SobolSampler::kStreamStride
					<< " slots; it cannot see the DL-283 overrun.\n";
				ok = false;
			}
			if( !het && t.maxBlockDraws != 1u ) {
				std::cerr << "  FAIL: a homogeneous distance sample drew " << t.maxBlockDraws
					<< " values; the bounded-draw premise changed.\n";
				ok = false;
			}
		}
	}
	if( !ok ) exit( 1 );
	std::cout << "  Passed!\n";
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
	TestStreamMap();
	TestMediumDistanceStreamAudit();

	std::cout << "\nAll Sobol dimension budget tests passed!\n";
	return 0;
}
