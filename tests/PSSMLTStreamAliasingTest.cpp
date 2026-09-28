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
#include "../src/Library/Utilities/BDPTUtilities.h"
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

// Exposes how many PrimarySample slots a sampler instance has actually
// materialised (DL-08 storage follow-up, 2026-09-17 -- debt 29 review
// P1).  `X` and `XExtra` are protected for the same reason as above:
// callers have no business poking at the storage layout, but the
// memory-cost red-proof needs to observe it from the same in-process
// harness the reviewer used, not re-derive it from wall-clock timing
// alone.
class PSSMLTStorageProbe : public PSSMLTSampler
{
public:
	PSSMLTStorageProbe( unsigned int seed, Scalar largeStepProb )
		: PSSMLTSampler( seed, largeStepProb )
	{
	}

	// Total PrimarySample slots materialised across BOTH storage tiers.
	size_t MaterializedSlotCount() const
	{
		size_t total = X.size();
		for( size_t i = 0; i < XExtra.size(); i++ )
		{
			total += XExtra[i].second.size();
		}
		return total;
	}

	// Number of distinct extra-tier streams touched (XExtra's own size --
	// the association-list entry count, not a PrimarySample count).
	size_t ExtraStreamCount() const { return XExtra.size(); }

	size_t MaterializedBytes() const
	{
		return MaterializedSlotCount() * sizeof( PrimarySample );
	}
};

// Independent, from-scratch reimplementation of the PRE-DL-08-STORAGE-FIX
// algorithm (a single flat vector, `idx = stream + 49*sample`, no extra
// tier) -- used as a determinism oracle so this suite can keep proving
// "streams < kLegacyNumStreams reproduce the base commit bit-for-bit"
// forever, without re-checking out historical commits.  This is NOT a
// subclass of PSSMLTSampler (that would inherit the two-tier storage
// this test exists to validate against) -- it is a standalone clone of
// the exact pre-fix Get1D()/Mutate()/Accept()/Reject() logic, built
// directly on RandomNumberGenerator the same way PSSMLTSampler itself
// is.  Constants (s1, s2, largeStepProb selection) are copied verbatim
// from PSSMLTSampler.cpp; if those ever change, this clone must change
// with them or this oracle silently stops being a valid reference --
// there is no way to enforce that statically, so a comment here is the
// best available guard.
class ReferenceLegacyPSSMLT
{
public:
	static const int kNumStreams = 49; // the base commit's ONLY width

	struct Sample
	{
		Scalar value, backup;
		unsigned int lastModIteration, backupIteration;
		Sample() : value(0), backup(0), lastModIteration(0), backupIteration(0) {}
	};

	ReferenceLegacyPSSMLT( unsigned int seed, Scalar largeStepProb_ )
		: sampleIndex(0), currentIteration(0), streamIndex(0),
		  largeStepProb(largeStepProb_), isLargeStep(true),
		  lastLargeStepIteration(0), rng(seed)
	{
	}

	void StartStream( int s ) { streamIndex = s; sampleIndex = 0; }

	void StartIteration()
	{
		isLargeStep = ( rng.CanonicalRandom() < largeStepProb );
		streamIndex = 0;
		sampleIndex = 0;
		modifiedIndices.clear();
	}

	void Accept()
	{
		if( isLargeStep ) lastLargeStepIteration = currentIteration;
		currentIteration++;
		modifiedIndices.clear();
	}

	void Reject()
	{
		for( size_t i = 0; i < modifiedIndices.size(); i++ )
		{
			Sample& s = X[modifiedIndices[i]];
			s.value = s.backup;
			s.lastModIteration = s.backupIteration;
		}
		currentIteration++;
		modifiedIndices.clear();
	}

	Scalar Get1D()
	{
		const unsigned int idx = streamIndex + kNumStreams * sampleIndex;
		sampleIndex++;

		while( idx >= X.size() )
		{
			Sample s;
			s.value = rng.CanonicalRandom();
			s.lastModIteration = currentIteration;
			s.backupIteration = currentIteration;
			X.push_back( s );
		}

		Sample& sample = X[idx];
		sample.backup = sample.value;
		sample.backupIteration = sample.lastModIteration;

		if( isLargeStep )
		{
			sample.value = rng.CanonicalRandom();
		}
		else
		{
			if( sample.lastModIteration < lastLargeStepIteration )
			{
				sample.value = rng.CanonicalRandom();
				sample.lastModIteration = lastLargeStepIteration;
			}
			const unsigned int nSmall = currentIteration - sample.lastModIteration;
			const unsigned int nMutations = nSmall > 0 ? nSmall : 1;
			for( unsigned int i = 0; i < nMutations; i++ ) {
				sample.value = Mutate( sample.value );
			}
		}

		sample.lastModIteration = currentIteration;
		modifiedIndices.push_back( idx );
		return sample.value;
	}

private:
	Scalar Mutate( Scalar value )
	{
		static const Scalar s1 = 1.0 / 1024.0;
		static const Scalar s2 = 1.0 / 64.0;
		static const Scalar logRatio = -log( s2 / s1 );

		const Scalar u = rng.CanonicalRandom();
		const Scalar delta = s2 * exp( logRatio * u );
		if( rng.CanonicalRandom() < 0.5 )
		{
			Scalar result = value + delta;
			if( result >= 1.0 ) result -= 1.0;
			return result;
		}
		else
		{
			Scalar result = value - delta;
			if( result < 0.0 ) result += 1.0;
			return result;
		}
	}

	std::vector<Sample> X;
	std::vector<unsigned int> modifiedIndices;
	unsigned int sampleIndex, currentIteration;
	int streamIndex;
	Scalar largeStepProb;
	bool isLargeStep;
	unsigned int lastLargeStepIteration;
	RandomNumberGenerator rng;
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

		// DL-283: 4096 -> 262144, re-checked against every stream index
		// in the integrators and the MLT rasterizers: BDPT's medium
		// distance samples now draw from per-iteration stream blocks up
		// to BDPTUtilities::kMediumDistanceStreamEnd (139264), which
		// MLT reaches through the shared generator.
		if( lanes != 262144 ) {
			std::cerr << "  FAIL: PSSMLTSampler::kDefaultNumStreams is " << lanes
				<< ", not 262144.  Every stream index in the integrators and the "
				<< "MLT rasterizers must be re-checked against the new bound "
				<< "before this test is updated.\n";
			exit( 1 );
		}
		if( lanes < BDPTUtilities::kMediumDistanceStreamEnd ) {
			std::cerr << "  FAIL: kDefaultNumStreams (" << lanes << ") is below the end of "
				<< "BDPT's medium-distance stream layout (" << BDPTUtilities::kMediumDistanceStreamEnd
				<< "): the first medium event under MLT would abort.\n";
			exit( 1 );
		}
		// ...and the reserved block must not be one of them.
		if( !( kMltStream < BDPTUtilities::kMediumDistanceStreamBase ) ) {
			std::cerr << "  FAIL: kPSSMLTFilmLensApertureStream (" << kMltStream
				<< ") falls inside BDPT's medium-distance stream layout.\n";
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
		// strictly above every PER-VERTEX stream BDPT's own eye/light
		// walks can reach under PSSMLTSampler (and, checked above, below
		// the DL-283 medium-distance blocks at 8192+) (kMaxBdptWalkStreamUnderPSSMLT ==
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
	// depth at 1024; see the `maxEyeTotalDepth` ternary).
	//
	// DL-08 storage follow-up (2026-09-17, debt 29 review P1): this used
	// to compare TWO SEPARATELY-CONSTRUCTED, identically-seeded, virgin
	// samplers (one touching only eyeStream, one touching only a low
	// stream) -- a valid way to probe the OLD single flat vector's
	// `idx = stream + kNumStreams*sampleIndex` arithmetic, because under
	// that scheme a virgin sampler's very first Get1D() call always
	// consumes exactly `idx+1` RNG draws, so two different idx values
	// necessarily land on different RNG sequence positions REGARDLESS OF
	// INSTANCE.  It is not a valid probe for the two-tier storage that
	// replaced it: a stream in the extra tier (>= kLegacyNumStreams) is
	// looked up by its own literal number in an independently-grown
	// per-stream vector, so on a VIRGIN instance its first-ever draw is
	// simply "this instance's first RNG call" -- identical, by
	// construction, to any OTHER virgin same-seed instance's own
	// first-ever draw regardless of which stream either one asked for.
	// Comparing across instances would flag that expected coincidence as
	// a false "collision".  The real invariant -- no two DISTINCT
	// (stream, sampleIndex) lanes of the SAME sampler instance ever
	// share a PrimarySample -- is what actually matters (it is what
	// BDPT+MLT's single shared PSSMLTSampler relies on), so this probe
	// now touches every stream through ONE instance, switching via
	// StartStream() exactly as production code does, and checks that no
	// later draw (from more-advanced RNG state) repeats an earlier one.
	{
		const int kProbeDepthLo = 32;
		const int kProbeDepthHi = 130; // comfortably past the old 49-lane modulus
		bool anyCollision = false;

		for( int depth = kProbeDepthLo; depth <= kProbeDepthHi && !anyCollision; depth++ )
		{
			const int eyeStream = 16 + depth;

			PSSMLTSampler* p = MakeSampler( 424242 + depth, 1.0 );
			p->StartIteration();

			p->StartStream( eyeStream );
			const Scalar eyeVal = p->Get1D();

			// Compare against every "low" BDPT stream (0, 1, 16, 47) AND
			// the MLT reserved stream, drawn from the SAME instance
			// (its RNG state has already advanced past the eyeStream
			// draw above, exactly as it would mid-chain in production).
			const int lowStreams[] = { 0, 1, 16, 47, kMltReservedStream };
			for( unsigned int ls = 0; ls < sizeof(lowStreams)/sizeof(lowStreams[0]) && !anyCollision; ls++ )
			{
				p->StartStream( lowStreams[ls] );
				for( int k = 0; k < 4; k++ )
				{
					const Scalar lowVal = p->Get1D();
					if( lowVal == eyeVal )
					{
						std::cerr << "  FAIL: eye-walk stream " << eyeStream
							<< " (depth " << depth << ") collides with stream "
							<< lowStreams[ls] << " sample " << k << "\n";
						anyCollision = true;
						break;
					}
				}
			}

			p->release();
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
			<< "}, single shared instance: no collisions\n";
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test G: Storage-cost red-proof (DL-08 storage follow-up, 2026-09-17
// -- debt 29 review P1)
//
// The DL-08 collision fix (d7ebd453) raised kNumStreams 49 -> 4096 and
// moved MLT's reserved film/lens/aperture stream to 2048.  Under the
// ORIGINAL single flat vector (`idx = streamIndex + kNumStreams *
// sampleIndex`, lazily grown by `while (idx >= X.size()) push_back`),
// touching stream 2048 for its first 6 samples requires materialising
// EVERY slot up to `idx = 2048 + 4096*5 = 22528` -- 22529 PrimarySample
// entries (~528 KB; sizeof(PrimarySample) == 24 on this platform,
// independently confirmed by building a throwaway probe against the
// unfixed library at commit 8080cd2a: "Slots=22529 Bytes=540696").
// MLTRasterizer.cpp's bootstrap phase constructs and destroys ONE
// PSSMLTSampler PER bootstrap sample (100,000 by default on
// scenes/Tests/MLT/cornellbox_mlt_fast.RISEscene), each paying this
// cost on its very first 6 draws -- measured bootstrap wall time (3
// runs each, this worktree): base (e290fc64, pre-DL-08 entirely) ~2.0-
// 2.8 s; 8080cd2a (post-collision-fix, pre-storage-fix) ~9.9-10.4 s;
// this fix ~1.78-1.82 s.  See docs/DL08_PSSMLT_LANE_LAYOUT.md for the
// full table.
//
// The two-tier storage fix (this commit) makes an unused/high-numbered
// stream cost nothing: touching stream 2048 for N samples costs
// exactly N PrimarySample slots in its own XExtra entry, independent
// of the stream's numeric value.
// ================================================================

static void TestStorageCostRedProof()
{
	std::cout << "\nTest G: Storage-cost red-proof (debt 29 review P1)\n";

	const int kFilmStream = BDPTCameraUtilities::kPSSMLTFilmLensApertureStream;

	PSSMLTStorageProbe probe( 55555, 0.3 );
	probe.StartIteration();
	probe.StartStream( kFilmStream );

	// Mirrors MLTRasterizer::EvaluateSample's real draw sequence: film
	// position (Get2D), lens position (Get2D), aperture point (Get2D)
	// -- 6 Get1D()s total, all on the one reserved stream.
	probe.Get2D();
	probe.Get2D();
	probe.Get2D();

	const size_t slots = probe.MaterializedSlotCount();
	const size_t bytes = probe.MaterializedBytes();
	const size_t extraStreams = probe.ExtraStreamCount();

	std::cout << "  After 6 draws on reserved stream " << kFilmStream
		<< ": " << slots << " PrimarySample slots (" << bytes
		<< " bytes) across " << extraStreams << " extra-tier stream(s)"
		<< " (pre-storage-fix: 22529 slots / 540696 bytes)\n";

	if( slots != 6 )
	{
		std::cerr << "  FAIL: expected exactly 6 materialised slots (one per "
			<< "draw on a virgin extra-tier stream), got " << slots
			<< " -- the extra tier is no longer O(1) per draw.\n";
		exit( 1 );
	}

	if( extraStreams != 1 )
	{
		std::cerr << "  FAIL: expected exactly 1 extra-tier stream entry, got "
			<< extraStreams << ".\n";
		exit( 1 );
	}

	// DL-283: BDPT's medium distance samples draw from per-iteration
	// stream blocks at 8192..139264.  Under PSSMLT a stream is an
	// unbounded lane, so each medium event must cost ONE extra-tier
	// entry and exactly as many slots as it draws -- never a function of
	// the (large) stream number.  64 eye + 64 light events of 40 draws
	// each (a long delta-tracking run) on top of the film block above.
	{
		const unsigned int kEvents = 64, kDraws = 40;
		for( unsigned int side = 0; side < 2; side++ ) {
			for( unsigned int d = 0; d < kEvents; d++ ) {
				probe.StartStream( BDPTUtilities::MediumDistanceStream(
					side ? BDPTUtilities::eLightWalk : BDPTUtilities::eEyeWalk, d ) );
				for( unsigned int k = 0; k < kDraws; k++ ) probe.Get1D();
			}
		}
		const size_t mSlots = probe.MaterializedSlotCount();
		const size_t mExtra = probe.ExtraStreamCount();
		std::cout << "  + 128 medium-distance events x " << kDraws << " draws (streams up to "
			<< BDPTUtilities::MediumDistanceStream( BDPTUtilities::eLightWalk, kEvents - 1 )
			<< "): " << mSlots << " slots across " << mExtra << " extra-tier streams\n";
		if( mSlots != 6u + 2u * kEvents * kDraws || mExtra != 1u + 2u * kEvents ) {
			std::cerr << "  FAIL: expected " << ( 6u + 2u * kEvents * kDraws ) << " slots and "
				<< ( 1u + 2u * kEvents ) << " extra-tier streams -- a medium-distance stream "
				<< "is not costing exactly one lane.\n";
			exit( 1 );
		}
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test H: Determinism and legacy-layout bit-identical reproduction
// (DL-08 storage follow-up, 2026-09-17 -- debt 29 review P1)
//
// Part 1: for streamIndex < kLegacyNumStreams (49), the two-tier fix's
// legacy path (a flat vector with the SAME formula and SAME row width
// PSSMLT used before this storage fix, and before the DL-08 collision
// fix ever changed kNumStreams) must reproduce that original algorithm
// bit-for-bit.  `ReferenceLegacyPSSMLT` is an independent, from-scratch
// reimplementation of that original algorithm (not a subclass of
// PSSMLTSampler -- it does not share the two-tier storage this test
// exists to validate).  Driving a real PSSMLTSampler and a
// ReferenceLegacyPSSMLT with the SAME seed through an IDENTICAL script
// of StartIteration()/StartStream()/Get1D()/Accept()/Reject() calls
// (varying draw counts per stream per iteration, and alternating
// accept/reject so both bookkeeping paths are exercised) must yield
// bit-identical values at every step -- this is exactly the "shallow
// scenes' chains are unchanged" claim: any BDPT walk that never
// reaches eye depth 33 uses ONLY streams < 49, so its render output is
// governed entirely by this reproduction property, and this test
// settles it without needing a full scene render.
//
// Part 2: the storage layout itself must be deterministic -- two
// otherwise-identical PSSMLTSampler instances, same seed, same script
// touching BOTH tiers, must produce identical sequences.
// ================================================================

static void TestLegacyLayoutDeterminism()
{
	std::cout << "\nTest H: Legacy-tier determinism vs an independent pre-fix reimplementation\n";

	const unsigned int seed = 90909;
	const Scalar largeStepProb = 0.3;

	// ---- Part 1: legacy tier vs the independent reference clone ----
	{
		PSSMLTSampler* p = MakeSampler( seed, largeStepProb );
		ReferenceLegacyPSSMLT ref( seed, largeStepProb );

		const int streams[] = { 0, 1, 16, 47 };
		const int nStreams = sizeof(streams)/sizeof(streams[0]);

		int totalChecked = 0;
		int mismatches = 0;

		for( int iter = 0; iter < 25; iter++ )
		{
			p->StartIteration();
			ref.StartIteration();

			for( int s = 0; s < nStreams; s++ )
			{
				// Deterministic function of (iter, s) so both objects
				// draw the SAME count per stream per iteration --
				// varying it mimics how a real BDPT walk's per-stream
				// depth changes path to path.
				const int nDraws = 1 + ( (iter * 3 + s * 5) % 6 ); // 1..6

				p->StartStream( streams[s] );
				ref.StartStream( streams[s] );

				for( int k = 0; k < nDraws; k++ )
				{
					const Scalar a = p->Get1D();
					const Scalar b = ref.Get1D();
					totalChecked++;
					if( a != b )
					{
						mismatches++;
						std::cerr << "  FAIL: iter " << iter << " stream " << streams[s]
							<< " draw " << k << ": fixed=" << a << " reference=" << b << "\n";
					}
				}
			}

			// Alternate accept/reject so both objects' backup/rollback
			// bookkeeping is exercised identically.
			if( iter % 3 == 2 ) {
				p->Reject();
				ref.Reject();
			} else {
				p->Accept();
				ref.Accept();
			}
		}

		p->release();

		if( mismatches > 0 )
		{
			std::cerr << "  " << mismatches << "/" << totalChecked << " draws diverged "
				<< "from the independent pre-fix reimplementation -- the two-tier "
				<< "storage fix broke bit-for-bit reproduction for streamIndex < "
				<< "kLegacyNumStreams.\n";
			exit( 1 );
		}

		std::cout << "  Part 1: " << totalChecked << "/" << totalChecked
			<< " draws bit-identical to the independent legacy (kNumStreams=49) "
			<< "reimplementation across streams {0,1,16,47}, 25 iterations, mixed "
			<< "accept/reject -- shallow chains (streamIndex < 49) reproduce the "
			<< "pre-storage-fix layout exactly.\n";
	}

	// ---- Part 2: two-tier layout is itself deterministic ----
	{
		const int streams[] = { 0, 16, 47, BDPTCameraUtilities::kPSSMLTFilmLensApertureStream, 200 };
		const int nStreams = sizeof(streams)/sizeof(streams[0]);

		std::vector<Scalar> runA, runB;

		for( int run = 0; run < 2; run++ )
		{
			std::vector<Scalar>& out = ( run == 0 ) ? runA : runB;
			PSSMLTSampler* p = MakeSampler( seed, largeStepProb );

			for( int iter = 0; iter < 10; iter++ )
			{
				p->StartIteration();
				for( int s = 0; s < nStreams; s++ )
				{
					p->StartStream( streams[s] );
					for( int k = 0; k < 4; k++ ) {
						out.push_back( p->Get1D() );
					}
				}
				if( iter % 3 == 2 ) p->Reject(); else p->Accept();
			}

			p->release();
		}

		if( runA.size() != runB.size() )
		{
			std::cerr << "  FAIL: two identically-seeded runs produced different "
				<< "draw counts (" << runA.size() << " vs " << runB.size() << ").\n";
			exit( 1 );
		}

		int mismatches2 = 0;
		for( size_t i = 0; i < runA.size(); i++ ) {
			if( runA[i] != runB[i] ) mismatches2++;
		}

		if( mismatches2 > 0 )
		{
			std::cerr << "  FAIL: " << mismatches2 << "/" << runA.size()
				<< " draws diverged between two identically-seeded runs spanning "
				<< "both storage tiers -- the two-tier layout is not deterministic.\n";
			exit( 1 );
		}

		std::cout << "  Part 2: " << runA.size() << "/" << runA.size()
			<< " draws bit-identical across two identically-seeded runs spanning "
			<< "both tiers (streams {0,16,47," << BDPTCameraUtilities::kPSSMLTFilmLensApertureStream
			<< ",200}).\n";
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
	TestStorageCostRedProof();
	TestLegacyLayoutDeterminism();

	std::cout << "\nAll PSSMLT stream aliasing tests passed!\n";
	return 0;
}
