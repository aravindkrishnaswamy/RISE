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
//    BDPTIntegrator uses streams 0-47 internally (see
//    CameraUtilities.h's BDPTCameraUtilities::kApertureSamplerStream
//    comment for the authoritative table):
//      - Stream 0:            light source sampling
//      - Streams 1..1+maxLightDepth+maxVolumeBounce:  light subpath
//        bounces
//      - Streams 16..16+maxEyeDepth+maxVolumeBounce:  eye subpath
//        bounces (both walk loops saturate their iteration count at
//        1024, giving a documented ceiling of stream 1039 for the eye
//        walk -- BDPTCameraUtilities::kMaxBdptWalkStreamUnderPSSMLT)
//      - Streams 31-46:       SMS (reserved; unused today)
//      - Stream 47:           BDPT (s,t) strategy selection
//        (BDPTIntegrator.cpp's `StartStream( 47 )`)
//
//    MLTRasterizer uses BDPTCameraUtilities::kPSSMLTFilmLensApertureStream
//    (2048 since the DL-08 / debt 29 fix, 2026-09-17) for the film
//    position.  Before that fix it was the literal stream 48, which the
//    eye walk's own StartStream(16u+depth) reached at eye depth 32 --
//    StabilityConfig::maxVolumeBounce defaults to 64, so ordinary
//    scattering-medium scenes got there with no unusual settings --
//    producing a literal same-integer collision (not modular aliasing)
//    between the film position and the 32nd eye bounce's scattering
//    direction: spatially-varying bias (shifted shadows, wrong light
//    direction as a function of pixel position) on any chain deep
//    enough to reach it.  See docs/DL08_PSSMLT_LANE_LAYOUT.md and
//    Test F below for the red-proof.  In general, if kNumStreams <= N,
//    stream N+kNumStreams aliases with N.
//
//  Tests:
//    A. Stream independence: samples drawn from the MLT reserved
//       stream occupy different primary vector entries than samples
//       from BDPTIntegrator streams (0, 1, 16, 47).
//    B. Mutation isolation: a small-step mutation on the MLT reserved
//       stream does not alter the values returned by streams 0-47.
//    C. kNumStreams minimum: kNumStreams > the MLT reserved stream,
//       which itself clears kMaxBdptWalkStreamUnderPSSMLT.
//    D. Source guard: MLTRasterizer uses the MLT reserved stream (not
//       0 or the bare literal 48) for film position, and does not set
//       streams 1 or 2 before integrator calls.
//    E. Screen coordinate convention: MLTRasterizer uses (height - py)
//       not (height - 1 - py) to match the BDPT pel rasterizer.
//    F. Deep eye-walk aliasing (debt 29 / DL-08): the eye walk's own
//       stream 48 (16 + eye depth 32) must never collide with the MLT
//       reserved stream, at any eye depth PSSMLTSampler's loop caps
//       allow.
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
#include "../src/Library/Cameras/CameraUtilities.h"
#include "../src/Library/Cameras/ThinLensCamera.h"

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

// Exposes the sampler's own lane count.  It is protected on purpose --
// callers are not supposed to guess at it -- so the test that pins the
// lane arithmetic reads it through the derived-class accessor pattern
// rather than duplicating the literal.
class StreamLaneProbe : public PSSMLTSampler
{
public:
	static int Lanes() { return kDefaultNumStreams; }
};

// ================================================================
// Test A: Stream independence — no index aliasing
//
// Draws samples from the MLT-reserved film/lens/aperture stream
// (BDPTCameraUtilities::kPSSMLTFilmLensApertureStream, 2048 since
// DL-08 -- it was the literal 48 before) and several integrator
// streams (0, 1, 16, 47) and verifies they produce different
// values.  If kNumStreams is too small, aliased streams read
// the same vector entries and return identical sequences.
// ================================================================

static void TestStreamIndexIndependence()
{
	std::cout << "\nTest A: Stream index independence (no aliasing)\n";

	const int kMaxIntegratorStream = 47;
	const int kFilmStream = BDPTCameraUtilities::kPSSMLTFilmLensApertureStream;
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

	const int kFilmStream = BDPTCameraUtilities::kPSSMLTFilmLensApertureStream;
	const int kSamples = 5;

	// Critical stream pair: film (reserved stream) vs light source (0).
	// This is the exact pair that caused the shifted shadow bug.
	const int testStreams[] = { 0, 1, 16, 47 };
	const int nTestStreams = sizeof(testStreams) / sizeof(testStreams[0]);

	// Run multiple large-step iterations and verify that the film
	// stream values are NOT equal to any integrator stream values
	// at the same sample index.  With kNumStreams too small, stream 0
	// and the film stream would read the same vector entry, producing
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

	std::cout << "  Film stream (" << kFilmStream << ") vs integrator streams (0,1,16,47): "
		<< "no shared entries across " << kIterations << " iterations\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test C: kNumStreams minimum value
//
// Verifies that streams 0-47 (every stream BDPTIntegrator's own
// StartStream calls can reach under PSSMLTSampler, per
// BDPTCameraUtilities::kMaxBdptWalkStreamUnderPSSMLT's derivation)
// plus the MLT-reserved film/lens/aperture stream all produce
// distinct initial values.  If kNumStreams <= N, then stream
// N+kNumStreams aliases with N.
// ================================================================

static void TestKNumStreamsMinimum()
{
	std::cout << "\nTest C: kNumStreams minimum value\n";

	const int kMltStream = BDPTCameraUtilities::kPSSMLTFilmLensApertureStream;

	PSSMLTSampler* pSampler = MakeSampler( 54321, 1.0 );
	pSampler->StartIteration();

	std::vector<Scalar> firstValues( 48 );
	for( int s = 0; s <= 47; s++ )
	{
		pSampler->StartStream( s );
		firstValues[s] = pSampler->Get1D();
	}
	pSampler->StartStream( kMltStream );
	const Scalar mltFirstValue = pSampler->Get1D();

	bool anyAlias = false;
	for( int i = 0; i < 48; i++ )
	{
		for( int j = i + 1; j < 48; j++ )
		{
			if( firstValues[i] == firstValues[j] )
			{
				std::cerr << "  FAIL: Stream " << i << " and stream "
					<< j << " returned identical first values ("
					<< firstValues[i] << ") — aliased!\n";
				anyAlias = true;
			}
		}
		if( firstValues[i] == mltFirstValue )
		{
			std::cerr << "  FAIL: Stream " << i << " and the MLT reserved "
				<< "stream " << kMltStream << " returned identical first "
				<< "values (" << firstValues[i] << ") — aliased!\n";
			anyAlias = true;
		}
	}

	pSampler->release();

	if( anyAlias )
	{
		std::cerr << "  kNumStreams is too small.  Must be > "
			<< kMltStream << " to give streams 0-47 and the MLT "
			<< "reserved stream independent lanes.\n";
		exit( 1 );
	}

	std::cout << "  Streams 0-47 and the MLT reserved stream (" << kMltStream
		<< ") all produce distinct values: OK\n";

	// ------------------------------------------------------------
	// C2: the MLT film/lens/APERTURE block on the reserved stream.
	//
	// Debt 28's t==1 aperture point is drawn as a further Get2D on
	// the reserved stream, contiguous with the film and lens samples.
	// This probes `MLTRasterizer` (RGB): film Get2D + lens Get2D
	// consume 4 lanes (sample indices 0-3), so the aperture Get2D
	// lands at sample indices 4 and 5 -- six consecutive draws on the
	// reserved stream.  `MLTSpectralRasterizer` pre-consumes
	// `nSpectralSamples` (S) additional wavelength Get1Ds before the
	// aperture draw, so its aperture lanes are at sample indices 4+S
	// and 5+S (8/9 at the default S=4) -- not probed here, see
	// CameraUtilities.h's `APERTURE_CURRENT_STREAM` doc.
	//
	// Six consecutive draws on the reserved stream must be six
	// distinct primary samples, and none of them may equal a sample
	// any integrator stream 0..47 can reach at the same depth.  This
	// is the residue-mod-kNumStreams argument: it holds for ANY
	// stream that stays below kNumStreams, PROVIDED the reserved
	// stream itself is chosen above every stream the eye/light walks
	// can reach (kMaxBdptWalkStreamUnderPSSMLT) -- unlike the
	// historical literal 48, which the eye walk's own
	// `StartStream( 16u + depth )` reached at eye depth 32 (DL-08 /
	// debt 29, docs/DL08_PSSMLT_LANE_LAYOUT.md).  See Test F below for
	// that specific collision, red-proved against the pre-fix code.
	// ------------------------------------------------------------
	{
		PSSMLTSampler* pS = MakeSampler( 99991, 1.0 );
		pS->StartIteration();

		pS->StartStream( kMltStream );
		std::vector<Scalar> filmBlock;
		for( int k = 0; k < 6; k++ ) filmBlock.push_back( pS->Get1D() );

		bool dup = false;
		for( int i = 0; i < 6; i++ ) {
			for( int j = i + 1; j < 6; j++ ) {
				if( filmBlock[i] == filmBlock[j] ) {
					std::cerr << "  FAIL: reserved-stream samples " << i << " and "
						<< j << " are the same primary sample ("
						<< filmBlock[i] << ")\n";
					dup = true;
				}
			}
		}

		// And against every integrator stream at every depth the
		// reserved-stream block spans.
		for( int st = 0; st <= 47 && !dup; st++ ) {
			pS->StartStream( st );
			for( int k = 0; k < 6; k++ ) {
				const Scalar v = pS->Get1D();
				for( int j = 0; j < 6; j++ ) {
					if( v == filmBlock[j] ) {
						std::cerr << "  FAIL: stream " << st << " sample " << k
							<< " aliases reserved-stream sample " << j << "\n";
						dup = true;
					}
				}
			}
		}

		pS->release();
		if( dup ) {
			std::cerr << "  The film / lens / aperture block on stream "
				<< kMltStream << " must occupy six private lanes.\n";
			exit( 1 );
		}
		std::cout << "  Reserved-stream (" << kMltStream
			<< ") film+lens+aperture block is private: OK\n";
	}

	// ------------------------------------------------------------
	// C3: the DL-08 safety property itself -- the reserved stream
	// sits strictly above every stream BDPT's own walks can reach
	// under PSSMLTSampler, and is still a real (non-aliased) lane.
	//
	// Read through a probe subclass because kDefaultNumStreams is
	// protected -- deliberately: this is the sampler's own invariant,
	// not a number a caller should be guessing at.
	//
	// (Pre-DL-08 this test pinned a specific historical mis-mapping:
	// the constant 80, once used for a dedicated aperture stream
	// under the old kNumStreams==49, computed to stream 31's sample
	// 1.  That trivia no longer applies once kNumStreams changes, and
	// it is not what this debt row is about, so it is not re-pinned
	// here -- the property that matters is the one below.)
	// ------------------------------------------------------------
	{
		const int lanes = StreamLaneProbe::Lanes();

		if( lanes != 4096 ) {
			std::cerr << "  FAIL: PSSMLTSampler::kDefaultNumStreams is " << lanes
				<< ", not 4096.  Every stream index in the integrators and the "
				<< "MLT rasterizers must be re-checked against the new bound "
				<< "before this test is updated.\n";
			exit( 1 );
		}

		if( kMltStream != 2048 ) {
			std::cerr << "  FAIL: kPSSMLTFilmLensApertureStream is " << kMltStream
				<< ", not 2048 -- re-derive the checks below against the new "
				<< "value.\n";
			exit( 1 );
		}

		// The reserved stream must be a REAL lane (not aliased).
		if( !( kMltStream < lanes ) ) {
			std::cerr << "  FAIL: kPSSMLTFilmLensApertureStream (" << kMltStream
				<< ") is >= kNumStreams (" << lanes << "), so the MLT "
				<< "film/lens/aperture block aliases stream "
				<< ( kMltStream % lanes ) << ".\n";
			exit( 1 );
		}

		// The core DL-08 safety property: the reserved stream sits
		// strictly above every stream BDPT's own eye/light walks can
		// reach under PSSMLTSampler (kMaxBdptWalkStreamUnderPSSMLT ==
		// 16 + 1024, the eye walk's saturating loop cap -- see that
		// constant's own derivation in CameraUtilities.h), so the
		// eye walk can NEVER compute a stream equal to the reserved
		// one, at any depth the walk's loop cap allows.
		const int kMaxWalk = BDPTCameraUtilities::kMaxBdptWalkStreamUnderPSSMLT;
		if( !( kMltStream > kMaxWalk ) ) {
			std::cerr << "  FAIL: kPSSMLTFilmLensApertureStream (" << kMltStream
				<< ") does not clear kMaxBdptWalkStreamUnderPSSMLT (" << kMaxWalk
				<< ") -- BDPT's eye walk could reach the reserved stream at "
				<< "some depth.\n";
			exit( 1 );
		}

		// The Sobol-only dedicated aperture stream remains a documented
		// convention, not a structural guarantee: PSSMLTSampler is never
		// actually driven with StartStream(kApertureSamplerStream) (the
		// MLT rasterizers always pass APERTURE_CURRENT_STREAM, never
		// APERTURE_DEDICATED_STREAM), so whether 3322 happens to fall
		// inside or outside PSSMLT's own (now much larger) lane space is
		// harmless either way -- unlike pre-DL-08, when PSSMLT's lane
		// space was tiny (49) and 3322 was unambiguously outside it.
		const int kDedicated = BDPTCameraUtilities::kApertureSamplerStream;
		std::cout << "  kNumStreams = " << lanes << "; reserved stream "
			<< kMltStream << " is a real lane and clears kMaxBdptWalkStreamUnderPSSMLT ("
			<< kMaxWalk << "); kApertureSamplerStream (" << kDedicated
			<< ", Sobol-only by convention, never driven through PSSMLTSampler): OK\n";
	}

	// ------------------------------------------------------------
	// C4: BEHAVIOURAL guard on the real PSSMLTSampler + the real
	// `DrawApertureSample`.  (B2 P2-3, debt 28 round 2.)
	//
	// C3 above only checks the ARITHMETIC of the multiplexing rule
	// against literal constants -- it never calls `DrawApertureSample`
	// or touches a real sampler.  Test D (below) is the complementary
	// SOURCE-TEXT guard: a substring search over the MLT rasterizer
	// files for `APERTURE_CURRENT_STREAM` / `kApertureSamplerStream`.
	// Neither actually exercises the claim that the aperture draw stays
	// on the CURRENTLY ACTIVE stream (whatever it is -- the RGB
	// rasterizer's case is the MLT reserved stream) -- a rename of
	// `DrawApertureSample` to something not containing those literal
	// tokens would sail through Test D, and a refactor that quietly
	// called `StartStream` again inside the `APERTURE_CURRENT_STREAM`
	// branch (breaking the residue invariant this whole file is about)
	// would sail through C3, since C3 never calls the function it is
	// reasoning about.
	//
	// This probe does what EvaluateSampleSpectral does at S=0 (no
	// wavelength draws): StartStream(kMltStream), draw the film + lens
	// Get2Ds (4 raw draws, sampleIndex 0..3), then call the REAL
	// `DrawApertureSample( thinLens, sampler, APERTURE_CURRENT_STREAM )`
	// and capture the two values it returns.  A second, independent
	// sampler with the IDENTICAL seed reproduces the same six draws by
	// hand (`StartStream(kMltStream)` then six raw `Get1D()`s) with no
	// camera or aperture helper involved at all.  `PSSMLTSampler`'s
	// per-lane value is a deterministic function of (seed, streamIndex,
	// sampleIndex) alone (see Get1D's `idx` formula), so the two must
	// agree bit-for-bit IF AND ONLY IF `DrawApertureSample` really did
	// nothing more than two more `Get1D`s on the stream that was
	// already active -- which is exactly the residue claim, tested on
	// the running code rather than asserted about it.
	// ------------------------------------------------------------
	{
		ThinLensCamera* thinLens = new ThinLensCamera(
			Point3( 0, 0, 6 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			36.0, 50.0, 2.8, 4.0, 1.0,
			64, 64, 1.0, 0.0, 0.0, 0.0,
			Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
			0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );

		PSSMLTSampler* pReal = MakeSampler( 424242, 1.0 );
		pReal->StartIteration();
		pReal->StartStream( kMltStream );
		Scalar filmLens[4];
		for( int i = 0; i < 4; i++ ) filmLens[i] = pReal->Get1D();
		const Point2 apertureSample = BDPTCameraUtilities::DrawApertureSample(
			*thinLens, *pReal, BDPTCameraUtilities::APERTURE_CURRENT_STREAM );

		PSSMLTSampler* pShadow = MakeSampler( 424242, 1.0 );
		pShadow->StartIteration();
		pShadow->StartStream( kMltStream );
		Scalar shadow[6];
		for( int i = 0; i < 6; i++ ) shadow[i] = pShadow->Get1D();

		bool behaviourOk = true;
		for( int i = 0; i < 4; i++ ) {
			if( filmLens[i] != shadow[i] ) {
				std::cerr << "  FAIL: film/lens draw " << i
					<< " diverged between the two identically-seeded samplers "
					<< "before DrawApertureSample was even called -- MakeSampler "
					<< "is not reproducing the same stream.\n";
				behaviourOk = false;
			}
		}
		if( apertureSample.x != shadow[4] || apertureSample.y != shadow[5] ) {
			std::cerr << "  FAIL: DrawApertureSample( APERTURE_CURRENT_STREAM ) "
				<< "returned (" << apertureSample.x << ", " << apertureSample.y
				<< ") but two more raw Get1D()s on the SAME already-active "
				<< "stream " << kMltStream << " give (" << shadow[4] << ", "
				<< shadow[5] << ").  DrawApertureSample is doing something "
				<< "other than drawing from the currently active stream under "
				<< "APERTURE_CURRENT_STREAM -- the residue argument no longer "
				<< "describes what the code does.\n";
			behaviourOk = false;
		}

		pReal->release();
		pShadow->release();
		thinLens->release();

		if( !behaviourOk ) exit( 1 );
		std::cout << "  DrawApertureSample( APERTURE_CURRENT_STREAM ) behaviourally "
			<< "matches two more raw draws on the already-active stream "
			<< kMltStream << ": OK\n";
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test D: Source guard — MLT rasterizer stream assignments
//
// Verifies that:
//   1. MLTRasterizer uses StartStream(kPSSMLTFilmLensApertureStream)
//      for film position (DL-08: no longer the bare literal 48, which
//      the eye walk's own StartStream(16u+depth) could reach)
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
		bool foundMltStream = false;
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

			// Check for StartStream( kPSSMLTFilmLensApertureStream )
			// (any BDPTCameraUtilities:: qualification/whitespace) --
			// and flag the pre-DL-08 bare literal 48 as a regression.
			if( line.find( "StartStream(" ) != std::string::npos &&
				line.find( "kPSSMLTFilmLensApertureStream" ) != std::string::npos )
			{
				foundMltStream = true;
			}
			if( ( line.find( "StartStream( 48 )" ) != std::string::npos ||
				  line.find( "StartStream(48)" ) != std::string::npos ) )
			{
				std::cerr << "    FAIL: " << labels[f] << " line " << lineNum
					<< " uses the bare literal StartStream(48) -- this is the "
					<< "exact DL-08 / debt 29 regression (the eye walk's own "
					<< "StartStream(16u+depth) reaches stream 48 at eye depth "
					<< "32).  Use BDPTCameraUtilities::kPSSMLTFilmLensApertureStream.\n";
				allPassed = false;
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

		if( !foundMltStream )
		{
			std::cerr << "    FAIL: " << labels[f] << " does not use "
				<< "StartStream(BDPTCameraUtilities::kPSSMLTFilmLensApertureStream) "
				<< "for film position.\n";
			std::cerr << "    The film stream must clear "
				<< "kMaxBdptWalkStreamUnderPSSMLT to avoid aliasing with "
				<< "BDPTIntegrator's eye/light walks at deep bounces (DL-08 / "
				<< "debt 29).\n";
			allPassed = false;
		}
		else
		{
			std::cout << "    StartStream(kPSSMLTFilmLensApertureStream) for film position: OK\n";
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

		// Debt 28 (A P1-1): the t==1 aperture sample must be drawn on
		// the MLT rasterizer's OWN reserved stream
		// (kPSSMLTFilmLensApertureStream), never on
		// kApertureSamplerStream.  Under PSSMLT that constant does not
		// name a private lane relative to the reserved stream's own
		// modulus -- it aliases stream (constant mod kNumStreams) at
		// sample (constant / kNumStreams).  Whole-file check, not
		// EvaluateSample-only, because the draw moved out of that
		// function in the spectral rasterizer.
		//
		// (B2 P2-3, debt 28 round 2) What this pins, precisely: it is a
		// SOURCE-TEXT substring search for the literal tokens
		// `APERTURE_CURRENT_STREAM` / `kApertureSamplerStream` /
		// `APERTURE_DEDICATED_STREAM` in the two MLT rasterizer files --
		// nothing here calls `DrawApertureSample` or touches a real
		// sampler.  A rename of the enum values, or a refactor that
		// stopped calling `DrawApertureSample` by name and inlined its
		// body instead, would silently pass or fail this guard without
		// the underlying behaviour changing at all.  The BEHAVIOURAL
		// twin -- constructing a real `PSSMLTSampler`, calling the real
		// `DrawApertureSample`, and checking its draws land where the
		// residue argument says they should -- is C4 in
		// TestKNumStreamsMinimum above; this is its source-text
		// complement, not a replacement for it.
		{
			std::ifstream whole( paths[f] );
			std::string wline;
			bool usesDedicated = false;
			bool usesCurrent = false;
			while( std::getline( whole, wline ) )
			{
				std::string t = wline;
				const size_t nz = t.find_first_not_of( " \t" );
				if( nz != std::string::npos && t.substr( nz, 2 ) == "//" ) continue;
				if( wline.find( "kApertureSamplerStream" ) != std::string::npos ||
					wline.find( "APERTURE_DEDICATED_STREAM" ) != std::string::npos ) {
					usesDedicated = true;
				}
				if( wline.find( "APERTURE_CURRENT_STREAM" ) != std::string::npos ) {
					usesCurrent = true;
				}
			}

			if( usesDedicated )
			{
				std::cerr << "    FAIL: " << labels[f] << " draws the t==1 "
					<< "aperture sample from a dedicated stream.  PSSMLTSampler "
					<< "multiplexes lanes as idx = stream + kNumStreams*sample; "
					<< "any stream index >= kNumStreams aliases an existing lane "
					<< "instead of getting a new one.  Draw it as a third Get2D "
					<< "on the reserved stream (APERTURE_CURRENT_STREAM).\n";
				allPassed = false;
			}
			else if( !usesCurrent )
			{
				std::cerr << "    FAIL: " << labels[f] << " has no "
					<< "APERTURE_CURRENT_STREAM aperture draw.  Debt 28's t==1 "
					<< "connection needs one, on the reserved stream.\n";
				allPassed = false;
			}
			else
			{
				std::cout << "    Aperture drawn on the reserved stream "
					<< "(APERTURE_CURRENT_STREAM), not a dedicated stream: OK\n";
			}
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
// Test F: Deep eye-walk stream aliasing (debt 29 / DL-08)
//
// BDPTIntegrator's eye-subpath walk calls
// `sampler.StartStream( 16u + depth )` once per loop iteration
// (BDPTIntegrator.cpp:1749), where `depth` is a plain loop counter
// that advances once per bounce -- surface OR volume -- regardless of
// what kind of vertex resulted.  `StabilityConfig::maxVolumeBounce`
// defaults to 64 (StabilityConfig.h), so an ordinary scattering-medium
// scene reaches eye depth 32 with no unusual settings at all.
//
// At eye depth 32, `16 + 32 == 48` -- and prior to this fix, 48 is
// *also* the literal stream MLTRasterizer.cpp / MLTSpectralRasterizer.cpp
// hardcode via `sampler.StartStream( 48 )` for the film position, lens
// position, and (debt 28) aperture point.  This is not the modular
// wraparound aliasing Test C above probes for (that starts one bounce
// later, at eye depth 33 -> stream 49 -> lane 49 mod kNumStreams); it
// is a literal same-integer collision between two semantically
// unrelated sampling dimensions.  A chain whose accepted path reaches
// that depth has its 32nd eye-bounce scattering direction and its film
// position living in the EXACT SAME primary-sample-vector slot: a
// small PSSMLT film mutation silently perturbs the 32nd bounce's BSDF
// sample, and accepting a path with a different 32nd-bounce outcome
// silently moves the film position.
//
// This is demonstrated directly against PSSMLTSampler -- no scene or
// render needed, because the bug lives entirely in the (stream,
// sampleIndex) -> primary-vector-index arithmetic, independent of what
// BDPT or MLTRasterizer do with the returned values: two identically
// seeded samplers, one replaying "the eye walk just reached depth 32"
// and the other replaying "MLTRasterizer is about to draw the film
// position", MUST diverge.  On the code this test was written against
// (pre-fix), they do not: PSSMLTSampler's per-lane value is a pure
// function of (seed, streamIndex, sampleIndex), and stream 16+32 IS
// stream 48.
// ================================================================

static void TestDeepEyeWalkAliasing()
{
	std::cout << "\nTest F: Deep eye-walk stream aliasing (debt 29 / DL-08)\n";

	const int kEyeDepthReachingFilmStream = 32; // BDPTIntegrator.cpp:1749 -> 16+32 == 48
	const int kEyeWalkStream = 16 + kEyeDepthReachingFilmStream;

	// MLTRasterizer.cpp / MLTSpectralRasterizer.cpp's REAL reserved
	// stream for the film / lens / (debt 28) aperture block, read from
	// the same named constant production code uses (not re-typed as a
	// literal, and NOT hardcoded to the historical 48 -- comparing
	// kEyeWalkStream against a hardcoded 48 would be a tautology, since
	// 16+32 == 48 is pure arithmetic that no fix changes; the thing
	// that must change is WHICH stream MLT reserves).  Test D above
	// independently greps the two MLT rasterizer source files for this
	// exact symbol, so if production ever stops using it, Test D
	// catches it there.
	const int kMltReservedStream = BDPTCameraUtilities::kPSSMLTFilmLensApertureStream;

	const int kSamplesPerStream = 6;

	PSSMLTSampler* pEyeWalk = MakeSampler( 777001, 1.0 );  // all large steps
	pEyeWalk->StartIteration();
	pEyeWalk->StartStream( kEyeWalkStream );
	std::vector<Scalar> eyeWalkVals( kSamplesPerStream );
	for( int i = 0; i < kSamplesPerStream; i++ ) {
		eyeWalkVals[i] = pEyeWalk->Get1D();
	}
	pEyeWalk->release();

	PSSMLTSampler* pFilm = MakeSampler( 777001, 1.0 );  // identical seed
	pFilm->StartIteration();
	pFilm->StartStream( kMltReservedStream );
	std::vector<Scalar> filmVals( kSamplesPerStream );
	for( int i = 0; i < kSamplesPerStream; i++ ) {
		filmVals[i] = pFilm->Get1D();
	}
	pFilm->release();

	int identicalCount = 0;
	for( int i = 0; i < kSamplesPerStream; i++ ) {
		if( eyeWalkVals[i] == filmVals[i] ) identicalCount++;
	}

	// FIXED behaviour: the eye walk's stream (48, unaffected by this fix
	// -- it is still literally 16+32) and MLTRasterizer's reserved
	// stream must be different lanes with different values.  On the
	// pre-fix code (kDefaultNumStreams == 49, MLT reserved stream ==
	// literal 48) this fails: both sides read stream 48 by construction
	// and identicalCount == kSamplesPerStream.
	if( identicalCount > 0 )
	{
		std::cerr << "  FAIL: eye-walk stream " << kEyeWalkStream
			<< " (16 + eye depth " << kEyeDepthReachingFilmStream
			<< ") and MLT's reserved stream " << kMltReservedStream
			<< " produced " << identicalCount << "/" << kSamplesPerStream
			<< " identical values -- they are the SAME primary-sample-vector "
			<< "lane.  A PSSMLT film-position mutation is aliased with the "
			<< "eye walk's " << kEyeDepthReachingFilmStream
			<< "th-bounce scattering direction (debt 29 / DL-08): "
			<< "MLTRasterizer's reserved stream must sit strictly above "
			<< "every stream BDPTIntegrator's own StartStream calls can "
			<< "reach under PSSMLTSampler.\n";
		exit( 1 );
	}

	std::cout << "  Eye-walk stream " << kEyeWalkStream << " (eye depth "
		<< kEyeDepthReachingFilmStream << ") vs MLT reserved stream "
		<< kMltReservedStream << ": independent (0/" << kSamplesPerStream
		<< " matches)\n";

	// A second, deeper probe: the eye walk must never wrap back onto ANY
	// low-numbered BDPT stream (light source = 0, light bounces, eye
	// bounces, BDPT strategy select = 47) at any depth PSSMLTSampler's
	// own saturating loop cap allows (BDPTIntegrator.cpp caps total eye
	// depth at 1024; see the `maxEyeTotalDepth` ternary).  Pre-fix, with
	// kNumStreams == 49, eye depth 33 (stream 49) aliases stream 0's
	// SECOND sample (idx == 49 either way); this walks a modest range
	// past the historical 49-lane boundary and confirms every eye-walk
	// stream in that range is still a private lane.
	{
		const int kProbeDepthLo = 32;
		const int kProbeDepthHi = 130; // comfortably past the old 49-lane modulus
		bool anyCollision = false;

		for( int depth = kProbeDepthLo; depth <= kProbeDepthHi && !anyCollision; depth++ )
		{
			const int eyeStream = 16 + depth;

			PSSMLTSampler* pA = MakeSampler( 424242 + depth, 1.0 );
			pA->StartIteration();
			pA->StartStream( eyeStream );
			const Scalar eyeVal = pA->Get1D();
			pA->release();

			// Compare against every "low" BDPT stream (0, 1, 16, 47) AND
			// the MLT reserved stream, at whichever sample index the
			// interleaving formula says could collide (idx = stream +
			// kNumStreams*sampleIndex, so eyeStream's idx at sampleIndex
			// 0 can only collide with a low stream's idx at some OTHER
			// sampleIndex -- probe a generous range).
			const int lowStreams[] = { 0, 1, 16, 47, kMltReservedStream };
			for( unsigned int ls = 0; ls < sizeof(lowStreams)/sizeof(lowStreams[0]) && !anyCollision; ls++ )
			{
				PSSMLTSampler* pB = MakeSampler( 424242 + depth, 1.0 );
				pB->StartIteration();
				pB->StartStream( lowStreams[ls] );
				for( int k = 0; k < 4; k++ )
				{
					const Scalar lowVal = pB->Get1D();
					if( lowVal == eyeVal )
					{
						std::cerr << "  FAIL: eye-walk stream " << eyeStream
							<< " (depth " << depth << ") collides with stream "
							<< lowStreams[ls] << " sample " << k << "\n";
						anyCollision = true;
						break;
					}
				}
				pB->release();
			}
		}

		if( anyCollision )
		{
			std::cerr << "  kNumStreams is too small to give every eye-walk "
				<< "depth in [" << kProbeDepthLo << ", " << kProbeDepthHi
				<< "] its own lane.\n";
			exit( 1 );
		}

		std::cout << "  Eye-walk streams " << kProbeDepthLo << ".." << kProbeDepthHi
			<< " (as 16+depth) vs streams {0,1,16,47," << kMltReservedStream
			<< "}: no collisions\n";
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
	TestDeepEyeWalkAliasing();

	std::cout << "\nAll PSSMLT stream aliasing tests passed!\n";
	return 0;
}
