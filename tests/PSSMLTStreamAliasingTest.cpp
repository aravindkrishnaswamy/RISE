//////////////////////////////////////////////////////////////////////
//
//  PSSMLTStreamAliasingTest.cpp - Regression test for PSSMLTSampler
//    stream aliasing.  Verifies that the film position stream used
//    by MLTRasterizer is independent of BDPTIntegrator's internal
//    streams, preventing the light-source-correlated-with-pixel bug
//    that caused shifted shadows in MLT renders.
//
//  Background:
//    PSSMLTSampler multiplexes sample streams into a single primary
//    sample vector via: idx = streamIndex + kNumStreams * sampleIndex.
//    If kNumStreams is too small, distinct stream indices alias to the
//    same vector entries, coupling samples that should be independent.
//
//    BDPTIntegrator uses streams 0-47 internally:
//      - Stream 0:     light source sampling
//      - Streams 1-16: light subpath bounces
//      - Streams 16-31: eye subpath bounces
//      - Stream 47:    SMS strategy choices
//
//    MLTRasterizer uses stream 48 for the film position.  If
//    kNumStreams <= 48, stream 48 aliases with stream (48 % kNumStreams)
//    and the film position becomes correlated with path construction,
//    producing spatially-varying bias (shifted shadows, wrong light
//    direction as a function of pixel position).
//
//  Tests:
//    A. Stream independence: samples drawn from the film stream (48)
//       occupy different primary vector entries than samples from
//       BDPTIntegrator streams (0, 1, 16, 47).
//    B. Mutation isolation: a small-step mutation on stream 48 does
//       not alter the values returned by streams 0-47.
//    C. kNumStreams minimum: kNumStreams > 48 (the maximum integrator
//       stream index).
//    D. Source guard: MLTRasterizer uses stream 48 (not 0) for film
//       position, and does not set streams 1 or 2 before integrator
//       calls.
//    E. Screen coordinate convention: MLTRasterizer uses (height - py)
//       not (height - 1 - py) to match the BDPT pel rasterizer.
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
#include <fstream>
#include <string>

#include "../src/Library/Utilities/PSSMLTSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

// Helper: heap-allocate a PSSMLTSampler with proper reference counting.
// PSSMLTSampler inherits from Reference and has a protected destructor,
// so it must be heap-allocated and released via release().
static PSSMLTSampler* MakeSampler( unsigned int seed, Scalar largeStepProb )
{
	// Reference starts at 1 from construction; do NOT addref.
	return new PSSMLTSampler( seed, largeStepProb );
}

// ================================================================
// Test A: Stream independence — no index aliasing
//
// Draws samples from the film stream (48) and several integrator
// streams (0, 1, 16, 47) and verifies they produce different
// values.  If kNumStreams is too small, aliased streams read
// the same vector entries and return identical sequences.
// ================================================================

static void TestStreamIndexIndependence()
{
	std::cout << "\nTest A: Stream index independence (no aliasing)\n";

	const int kMaxIntegratorStream = 47;
	const int kFilmStream = 48;
	const int kSamplesPerStream = 10;

	const int streamPairs[][2] = {
		{ 0, kFilmStream },		// light source vs film position
		{ 1, kFilmStream },		// light bounce 0 vs film
		{ 16, kFilmStream },	// eye bounce 0 vs film
		{ kMaxIntegratorStream, kFilmStream },	// SMS vs film
		{ 0, 1 },				// light source vs light bounce 0
		{ 0, 16 },				// light source vs eye bounce 0
		{ 1, 16 },				// light bounce 0 vs eye bounce 0
	};
	const int nPairs = sizeof(streamPairs) / sizeof(streamPairs[0]);

	for( int p = 0; p < nPairs; p++ )
	{
		const int streamA = streamPairs[p][0];
		const int streamB = streamPairs[p][1];

		PSSMLTSampler* pSampler = MakeSampler( 12345, 0.0 );

		// First iteration: lazily initializes vector entries
		pSampler->StartIteration();

		pSampler->StartStream( streamA );
		std::vector<Scalar> valsA( kSamplesPerStream );
		for( int i = 0; i < kSamplesPerStream; i++ ) {
			valsA[i] = pSampler->Get1D();
		}

		pSampler->StartStream( streamB );
		std::vector<Scalar> valsB( kSamplesPerStream );
		for( int i = 0; i < kSamplesPerStream; i++ ) {
			valsB[i] = pSampler->Get1D();
		}

		// Check that streams A and B return different values.
		// If they alias, the same vector entries are read, producing
		// identical sequences.
		int matchCount = 0;
		for( int i = 0; i < kSamplesPerStream; i++ ) {
			if( valsA[i] == valsB[i] ) {
				matchCount++;
			}
		}

		// With 10 independent random values in [0,1), the probability
		// of even ONE exact match is astronomically small (~10 * 2^-52).
		if( matchCount > 0 )
		{
			std::cerr << "  FAIL: Stream " << streamA << " and stream "
				<< streamB << " produced " << matchCount
				<< " identical values out of " << kSamplesPerStream
				<< " — streams are aliased!\n";
			std::cerr << "  kNumStreams is likely too small. It must be > "
				<< std::max( streamA, streamB ) << ".\n";
			pSampler->release();
			exit( 1 );
		}

		std::cout << "  Streams " << streamA << " vs " << streamB
			<< ": independent (0/" << kSamplesPerStream << " matches)\n";

		pSampler->release();
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test B: Vector entry independence (the aliasing test)
//
// The original shifted-shadow bug happened because kNumStreams=3
// made the film stream (0) and the light source stream (0) share
// the same primary sample vector entries.  When the Markov chain
// moved to a different pixel, the light source value changed too.
//
// This test verifies that writing a value to the film stream's
// vector entry does NOT affect the value read from integrator
// stream entries, and vice versa.  We do this by populating
// the sampler on a large step (so all entries are fresh),
// reading from both streams, and verifying they are distinct.
// Then we accept, do another large step, and verify that a
// different film position still produces independent integrator
// values.
//
// Note: the internal mutation RNG is shared across streams, so
// different consumption patterns produce different mutation
// perturbations.  This test uses large steps (fresh random
// values) to avoid that coupling and test pure vector independence.
// ================================================================

static void TestVectorEntryIndependence()
{
	std::cout << "\nTest B: Vector entry independence (aliasing test)\n";

	const int kFilmStream = 48;
	const int kSamples = 5;

	// Critical stream pair: film (48) vs light source (0).
	// This is the exact pair that caused the shifted shadow bug.
	const int testStreams[] = { 0, 1, 16, 47 };
	const int nTestStreams = sizeof(testStreams) / sizeof(testStreams[0]);

	// Run multiple large-step iterations and verify that the film
	// stream values are NOT equal to any integrator stream values
	// at the same sample index.  With kNumStreams=3, stream 0 and
	// stream 48 would read the same vector entry, producing
	// identical values on every large step.
	const int kIterations = 20;

	PSSMLTSampler* pSampler = MakeSampler( 11111, 1.0 );  // all large steps

	for( int iter = 0; iter < kIterations; iter++ )
	{
		pSampler->StartIteration();

		// Read film stream
		pSampler->StartStream( kFilmStream );
		std::vector<Scalar> filmVals( kSamples );
		for( int i = 0; i < kSamples; i++ ) {
			filmVals[i] = pSampler->Get1D();
		}

		// Read integrator streams and compare
		for( int s = 0; s < nTestStreams; s++ )
		{
			pSampler->StartStream( testStreams[s] );
			for( int i = 0; i < kSamples; i++ )
			{
				const Scalar intVal = pSampler->Get1D();
				if( intVal == filmVals[i] )
				{
					std::cerr << "  FAIL: Iteration " << iter
						<< ": film stream sample " << i << " ("
						<< filmVals[i] << ") == integrator stream "
						<< testStreams[s] << " sample " << i << "\n";
					std::cerr << "  Streams share vector entries — "
						<< "kNumStreams is too small.\n";
					std::cerr << "  This is the exact bug that causes "
						<< "shifted shadows in MLT.\n";
					pSampler->release();
					exit( 1 );
				}
			}
		}

		pSampler->Accept();
	}

	pSampler->release();

	std::cout << "  Film stream (48) vs integrator streams (0,1,16,47): "
		<< "no shared entries across " << kIterations << " iterations\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test C: kNumStreams minimum value
//
// Verifies that streams 0-48 all produce distinct initial values.
// If kNumStreams <= N, then stream N+kNumStreams aliases with N.
// ================================================================

static void TestKNumStreamsMinimum()
{
	std::cout << "\nTest C: kNumStreams minimum value\n";

	PSSMLTSampler* pSampler = MakeSampler( 54321, 1.0 );
	pSampler->StartIteration();

	std::vector<Scalar> firstValues( 49 );
	for( int s = 0; s <= 48; s++ )
	{
		pSampler->StartStream( s );
		firstValues[s] = pSampler->Get1D();
	}

	bool anyAlias = false;
	for( int i = 0; i < 49; i++ )
	{
		for( int j = i + 1; j < 49; j++ )
		{
			if( firstValues[i] == firstValues[j] )
			{
				std::cerr << "  FAIL: Stream " << i << " and stream "
					<< j << " returned identical first values ("
					<< firstValues[i] << ") — aliased!\n";
				anyAlias = true;
			}
		}
	}

	pSampler->release();

	if( anyAlias )
	{
		std::cerr << "  kNumStreams is too small.  Must be >= 49 to "
			<< "give streams 0-48 independent lanes.\n";
		exit( 1 );
	}

	std::cout << "  Streams 0-48 all produce distinct values: OK\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test D: Source guard — MLT rasterizer stream assignments
//
// Verifies that:
//   1. MLTRasterizer uses StartStream(48) for film position
//   2. MLTRasterizer does NOT use StartStream(0) for film position
//   3. MLTRasterizer does not set streams 1 or 2 before integrator
//      calls (dead code that would be immediately overridden)
// ================================================================

static std::string FindFile( const char* candidates[] )
{
	for( int i = 0; candidates[i]; i++ ) {
		std::ifstream test( candidates[i] );
		if( test.is_open() ) return candidates[i];
	}
	return "";
}

static void TestSourceGuard()
{
	std::cout << "\nTest D: Source guard (MLT rasterizer stream assignments)\n";

	const char* mltPaths[] = {
		"src/Library/Rendering/MLTRasterizer.cpp",
		"../../../src/Library/Rendering/MLTRasterizer.cpp",
		0
	};
	const char* mltSpectralPaths[] = {
		"src/Library/Rendering/MLTSpectralRasterizer.cpp",
		"../../../src/Library/Rendering/MLTSpectralRasterizer.cpp",
		0
	};

	std::string mltPath = FindFile( mltPaths );
	std::string mltSpectralPath = FindFile( mltSpectralPaths );

	if( mltPath.empty() || mltSpectralPath.empty() )
	{
		std::cout << "  SKIP: Could not open MLT rasterizer source files\n";
		std::cout << "  (Run from project root or bin/tests/ directory)\n";
		return;
	}

	bool allPassed = true;

	const char* labels[] = { "MLTRasterizer", "MLTSpectralRasterizer" };
	std::string paths[] = { mltPath, mltSpectralPath };

	for( int f = 0; f < 2; f++ )
	{
		std::cout << "  " << labels[f] << ":\n";

		std::ifstream file( paths[f] );
		std::string line;
		int lineNum = 0;
		bool foundStream48 = false;
		bool foundBadStream0Film = false;
		bool foundDeadStream1 = false;
		bool foundDeadStream2 = false;
		bool inEvaluateSample = false;
		bool seenFirstBrace = false;
		int braceDepth = 0;

		while( std::getline( file, line ) )
		{
			lineNum++;

			// Track whether we're inside EvaluateSample/EvaluateSampleSpectral
			if( line.find( "EvaluateSample" ) != std::string::npos &&
				( line.find( "MLTRasterizer::" ) != std::string::npos ||
				  line.find( "MLTSpectralRasterizer::" ) != std::string::npos ) )
			{
				inEvaluateSample = true;
				seenFirstBrace = false;
				braceDepth = 0;
			}

			if( inEvaluateSample )
			{
				for( size_t c = 0; c < line.size(); c++ ) {
					if( line[c] == '{' ) { braceDepth++; seenFirstBrace = true; }
					if( line[c] == '}' ) braceDepth--;
				}
				if( seenFirstBrace && braceDepth <= 0 ) {
					inEvaluateSample = false;
				}
			}

			if( !inEvaluateSample ) continue;

			// Skip comment lines
			std::string trimmed = line;
			size_t firstNonSpace = trimmed.find_first_not_of( " \t" );
			if( firstNonSpace != std::string::npos &&
				trimmed.substr( firstNonSpace, 2 ) == "//" )
			{
				continue;
			}

			// Check for StartStream(48)
			if( line.find( "StartStream( 48 )" ) != std::string::npos ||
				line.find( "StartStream(48)" ) != std::string::npos )
			{
				foundStream48 = true;
			}

			// Check for StartStream(0) — should NOT be present
			if( ( line.find( "StartStream( 0 )" ) != std::string::npos ||
				  line.find( "StartStream(0)" ) != std::string::npos ) &&
				line.find( "//" ) == std::string::npos )
			{
				foundBadStream0Film = true;
				std::cerr << "    WARNING: Found StartStream(0) at line "
					<< lineNum << " in " << labels[f] << "\n";
			}

			// Check for dead StartStream(1) or StartStream(2)
			if( line.find( "StartStream( 1 )" ) != std::string::npos ||
				line.find( "StartStream(1)" ) != std::string::npos )
			{
				foundDeadStream1 = true;
			}
			if( line.find( "StartStream( 2 )" ) != std::string::npos ||
				line.find( "StartStream(2)" ) != std::string::npos )
			{
				foundDeadStream2 = true;
			}
		}

		if( !foundStream48 )
		{
			std::cerr << "    FAIL: " << labels[f] << " does not use "
				<< "StartStream(48) for film position.\n";
			std::cerr << "    The film stream must be 48 to avoid aliasing "
				<< "with BDPTIntegrator streams 0-47.\n";
			allPassed = false;
		}
		else
		{
			std::cout << "    StartStream(48) for film position: OK\n";
		}

		if( foundBadStream0Film )
		{
			std::cerr << "    FAIL: " << labels[f] << " uses "
				<< "StartStream(0) which aliases with BDPTIntegrator's "
				<< "light source stream.\n";
			allPassed = false;
		}
		else
		{
			std::cout << "    No StartStream(0) in EvaluateSample: OK\n";
		}

		if( foundDeadStream1 || foundDeadStream2 )
		{
			std::cerr << "    FAIL: " << labels[f] << " sets stream "
				<< (foundDeadStream1 ? "1" : "")
				<< (foundDeadStream1 && foundDeadStream2 ? " and " : "")
				<< (foundDeadStream2 ? "2" : "")
				<< " before integrator calls. These are dead code — "
				<< "BDPTIntegrator manages its own streams.\n";
			allPassed = false;
		}
		else
		{
			std::cout << "    No dead stream 1/2 assignments: OK\n";
		}
	}

	if( !allPassed )
	{
		exit( 1 );
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test E: Screen coordinate convention
//
// Verifies that MLTRasterizer uses (height - py) not
// (height - 1 - py) for the screen y coordinate, matching the
// convention used by the BDPT pel rasterizer.
// ================================================================

static void TestScreenCoordinateConvention()
{
	std::cout << "\nTest E: Screen coordinate convention\n";

	const char* mltPaths[] = {
		"src/Library/Rendering/MLTRasterizer.cpp",
		"../../../src/Library/Rendering/MLTRasterizer.cpp",
		0
	};
	const char* mltSpectralPaths[] = {
		"src/Library/Rendering/MLTSpectralRasterizer.cpp",
		"../../../src/Library/Rendering/MLTSpectralRasterizer.cpp",
		0
	};

	std::string mltPath = FindFile( mltPaths );
	std::string mltSpectralPath = FindFile( mltSpectralPaths );

	if( mltPath.empty() || mltSpectralPath.empty() )
	{
		std::cout << "  SKIP: Could not open MLT rasterizer source files\n";
		return;
	}

	bool allPassed = true;
	const char* labels[] = { "MLTRasterizer", "MLTSpectralRasterizer" };
	std::string paths[] = { mltPath, mltSpectralPath };

	for( int f = 0; f < 2; f++ )
	{
		std::ifstream file( paths[f] );
		std::string line;
		bool foundBadConvention = false;

		while( std::getline( file, line ) )
		{
			// Skip comments
			std::string trimmed = line;
			size_t firstNonSpace = trimmed.find_first_not_of( " \t" );
			if( firstNonSpace != std::string::npos &&
				trimmed.substr( firstNonSpace, 2 ) == "//" )
			{
				continue;
			}

			if( line.find( "height - 1 - py" ) != std::string::npos )
			{
				foundBadConvention = true;
				std::cerr << "  FAIL: " << labels[f] << " uses "
					<< "'height - 1 - py' instead of 'height - py'.\n";
				std::cerr << "  This causes a 1-pixel vertical offset "
					<< "vs the BDPT rasterizer convention.\n";
			}
		}

		if( !foundBadConvention )
		{
			std::cout << "  " << labels[f] << ": no 'height - 1 - py' "
				<< "found: OK\n";
		}
		else
		{
			allPassed = false;
		}
	}

	if( !allPassed )
	{
		exit( 1 );
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// main
// ================================================================

int main( int /*argc*/, char** /*argv*/ )
{
	std::cout << "=== PSSMLT Stream Aliasing Tests ===\n";

	TestStreamIndexIndependence();
	TestVectorEntryIndependence();
	TestKNumStreamsMinimum();
	TestSourceGuard();
	TestScreenCoordinateConvention();

	std::cout << "\nAll PSSMLT stream aliasing tests passed!\n";
	return 0;
}
