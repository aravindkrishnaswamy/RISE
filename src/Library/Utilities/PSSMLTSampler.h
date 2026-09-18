//////////////////////////////////////////////////////////////////////
//
//  PSSMLTSampler.h - Primary Sample Space Metropolis Light Transport
//    sampler.  Implements the ISampler interface by recording,
//    replaying, and mutating the random number stream consumed by
//    BDPT.  This is the heart of PSSMLT (Kelemen et al. 2002).
//
//    CONTEXT AND APPROACH:
//    Every BDPT path is fully determined by the sequence of random
//    numbers consumed during subpath generation and connection.
//    Instead of drawing fresh i.i.d. samples each time (as
//    IndependentSampler does), PSSMLTSampler maintains a "primary
//    sample vector" X = {x_0, x_1, ...} in [0,1) and produces
//    new candidate paths by mutating this vector:
//
//      - Large step: replace all consumed samples with fresh
//        uniform randoms.  This ensures ergodicity -- the chain
//        can jump to any path in the space.
//      - Small step: perturb each consumed sample by a small
//        amount using an exponential distribution (Kelemen's
//        log-uniform trick), wrapped to [0,1).  This explores
//        the neighborhood of the current path for correlated
//        improvements.
//
//    After BDPT evaluates the candidate path, the MLTRasterizer
//    calls Accept() or Reject().  On rejection, all proposed
//    mutations are reverted from backups.
//
//    The vector grows lazily: if BDPT requests sample index i
//    and i >= X.size(), a fresh uniform value is appended.  This
//    means PSSMLTSampler automatically adapts to paths of any
//    length without pre-allocation.
//
//    REFERENCES:
//    - Kelemen, C., Szirmay-Kalos, L., Antal, G., Csonka, F.
//      "A Simple and Robust Mutation Strategy for the Metropolis
//      Light Transport Algorithm." Computer Graphics Forum, 2002.
//    - Veach, E. "Robust Monte Carlo Methods for Light Transport
//      Simulation." PhD Thesis, Stanford University, 1997.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PSSMLT_SAMPLER_
#define PSSMLT_SAMPLER_

#include "ISampler.h"
#include "Reference.h"
#include "RandomNumbers.h"
#include <vector>
#include <utility>
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		/// PSSMLTSampler records the random number stream consumed by BDPT
		/// and mutates it for Metropolis-Hastings exploration of path space.
		///
		/// INVARIANTS:
		/// - Between StartIteration() and Accept()/Reject(), the sampler is
		///   in "proposal" state: Get1D()/Get2D() return mutated values and
		///   backup the originals.
		/// - After Accept(), proposed values become the new current state.
		/// - After Reject(), all modified samples revert to their pre-mutation
		///   values.
		/// - sampleIndex resets to 0 at each StartIteration(), ensuring the
		///   same consumption order regardless of path outcomes.
		class PSSMLTSampler :
			public virtual ISampler,
			public virtual Reference
		{
		protected:

			/// A single element of the primary sample vector.
			/// Tracks the current accepted value, a backup for rejection,
			/// and the iteration at which it was last modified (for lazy
			/// catch-up of mutations across iterations).
			struct PrimarySample
			{
				Scalar		value;				///< Current accepted value in [0,1)
				Scalar		backup;				///< Saved value for rejection rollback
				unsigned int lastModIteration;	///< Iteration when last mutated
				unsigned int backupIteration;	///< Iteration of the backup

				PrimarySample() :
				value( 0 ),
				backup( 0 ),
				lastModIteration( 0 ),
				backupIteration( 0 )
				{
				}
			};

			// STORAGE (DL-08 follow-up, 2026-09-17 -- debt 29 review P1):
			//
			// Stream multiplexing gives each stream its own lane so that
			// mutations to one part of the path don't disturb others.
			// BDPTIntegrator uses streams 0-47 internally (light
			// source=0, light bounces 1.., eye bounces 16.., SMS
			// reserved 31-46, (s,t) strategy select=47) -- see
			// BDPTCameraUtilities::kMaxBdptWalkStreamUnderPSSMLT
			// (CameraUtilities.h) for the derived ceiling on how high
			// the eye-walk bounce stream can actually reach (1039).  The
			// MLT rasterizers reserve
			// BDPTCameraUtilities::kPSSMLTFilmLensApertureStream (2048)
			// for the film/lens/aperture block.
			//
			// The original design multiplexed EVERY stream into one
			// flat vector via `idx = streamIndex + kNumStreams *
			// sampleIndex` (a base-kNumStreams positional encoding).
			// That's collision-free by construction, but its lazy-grow
			// loop (`while (idx >= X.size()) X.push_back(...)`)
			// materialises every slot up to idx -- cost proportional to
			// `streamIndex`, not to how many samples were actually
			// drawn.  The DL-08 collision fix (`d7ebd453`) moved the
			// reserved stream to 2048 and kNumStreams to 4096 to clear
			// BDPT's own reachable ceiling, which made that cost
			// catastrophic: the film/lens/aperture block's first 6
			// draws alone (stream 2048, sample 0..5) require
			// `idx = 2048 + 4096*5 = 22528`, i.e. 22529 materialised
			// `PrimarySample` slots (~528 KB; see
			// docs/DL08_PSSMLT_LANE_LAYOUT.md for the exact byte count
			// and the measured bootstrap-time regression, ~5x on
			// `scenes/Tests/MLT/cornellbox_mlt_fast.RISEscene`) on
			// EVERY fresh sampler, vs 294 slots (~6.9 KB) before the
			// DL-08 collision fix.  The
			// analogy drawn at the time -- "this is like SobolSampler's
			// `kNumDimensions` table, just a bigger constant" -- does
			// NOT hold: Sobol's direction-number table
			// (`SobolDirectionNumbers.cpp`) is ONE GLOBAL ~1 MiB array
			// shared by every sampler instance and built once per
			// process; `X` here is PER PSSMLTSampler INSTANCE and its
			// materialised size scales with `reservedStreamIndex *
			// kNumStreams * samplesDrawn` -- raising kNumStreams is not
			// "a bigger table", it MULTIPLIES the per-draw cost of every
			// stream, including the ordinary ones at 0-47.
			//
			// Fix: two-tier storage, not one flat vector.
			//
			//  - `X`: the ORIGINAL flat vector, but its row width is
			//    pinned to `kLegacyNumStreams` (49) FOREVER, matching
			//    the pre-DL-08 layout exactly -- used only while
			//    `streamIndex < kLegacyNumStreams`.  A chain that only
			//    ever touches streams 0-48 (every shallow/ordinary BDPT
			//    walk) reproduces the base commit's draws bit-for-bit,
			//    because it is running the identical formula on the
			//    identical row width.
			//  - `XExtra`: a small (stream, vector) association list for
			//    `streamIndex >= kLegacyNumStreams` (the MLT reserved
			//    stream at 2048, and any BDPT eye-walk depth deep enough
			//    to pass 48), searched linearly and grown by
			//    `FindOrCreateExtraStream()`.  In production this list
			//    has exactly ONE entry per sampler instance for almost
			//    every render (the MLT reserved stream; BDPT-under-MLT
			//    scenes rarely reach eye/light depth 33+), so a linear
			//    scan is not just adequate but faster in practice than a
			//    hash map: `std::unordered_map` was tried first and
			//    measured ~2x slower bootstrap wall-clock than even the
			//    pre-DL-08 base commit on
			//    `scenes/Tests/MLT/cornellbox_mlt_fast.RISEscene`
			//    (100,000 bootstrap samples, each constructing and
			//    destroying its OWN `PSSMLTSampler` --
			//    `MLTRasterizer.cpp`'s bootstrap loop -- so a hash
			//    table's bucket-array allocation on first insert is paid
			//    100,000 times over) -- see
			//    docs/DL08_PSSMLT_LANE_LAYOUT.md for the measured
			//    numbers.  Touching stream 2048 for 6 samples still
			//    costs exactly 6 `PrimarySample` slots -- independent of
			//    the stream's numeric value -- and there is still no
			//    multiplicative encoding here, so no two distinct
			//    streams can ever alias in this tier regardless of how
			//    large a stream number some future caller picks --
			//    `kPSSMLTFilmLensApertureStream`'s specific value (2048)
			//    no longer has to be load-bearing for collision safety,
			//    though it is kept where DL-08 placed it (comfortably
			//    above `kMaxBdptWalkStreamUnderPSSMLT`) for clarity and
			//    defense in depth.  If a future consumer ever reserves
			//    MANY distinct extra streams per instance, revisit this
			//    (a small sorted vector with binary search, or a map,
			//    would beat linear scan past a few dozen entries) --
			//    not a concern for the two known consumers today.
			//
			// `modifiedIndices` (below) is a `ModifiedLane` per touched
			// sample, tagging which tier and which storage index/key
			// owns it, so `Reject()` can find the sample back without
			// re-deriving stream/sampleIndex arithmetic.
			//
			// See docs/DL08_PSSMLT_LANE_LAYOUT.md for the measured
			// before/after slot counts and bootstrap timings, and
			// tests/PSSMLTStreamAliasingTest.cpp's memory-cost and
			// determinism rows for the red-proof.
			std::vector<PrimarySample>		X;					///< Legacy flat vector, streamIndex < kLegacyNumStreams only
			std::vector<std::pair<int, std::vector<PrimarySample> > > XExtra; ///< (stream, vector) list for streamIndex >= kLegacyNumStreams, searched linearly

			/// Finds (creating if necessary) the per-stream vector for
			/// `stream` in `XExtra`.  Linear scan by design -- see the
			/// `XExtra` comment above for why this beats a hash map for
			/// the tiny number of distinct extra streams any one
			/// instance actually touches.
			std::vector<PrimarySample>& FindOrCreateExtraStream( int stream )
			{
				for( size_t i = 0; i < XExtra.size(); i++ )
				{
					if( XExtra[i].first == stream ) {
						return XExtra[i].second;
					}
				}
				XExtra.push_back( std::make_pair( stream, std::vector<PrimarySample>() ) );
				return XExtra.back().second;
			}

			/// Identifies one touched (stream, sampleIndex) lane so
			/// Accept()/Reject() can find its PrimarySample back
			/// without re-deriving which storage tier owns it.
			struct ModifiedLane
			{
				bool			legacy;			///< true: lane lives in X; false: lives in XExtra
				unsigned int	legacyIdx;		///< valid iff legacy: flat index into X
				int				stream;			///< valid iff !legacy: key into XExtra
				unsigned int	sampleIdx;		///< valid iff !legacy: index into XExtra's vector for `stream`
			};
			std::vector<ModifiedLane>		modifiedIndices;	///< Lanes modified in current proposal (for fast rollback)
			unsigned int					sampleIndex;		///< Current consumption position within current stream
			unsigned int					currentIteration;	///< Global mutation counter

			// kLegacyNumStreams is the row width of the ORIGINAL flat
			// vector `X` -- fixed at 49 forever (BDPT's streams 0-47 plus
			// the historical single reserved slot at 48), independent of
			// kNumStreams below.  This is what makes shallow chains
			// (streamIndex always < 49) bit-identical to the pre-DL-08
			// base commit: same formula, same row width, unchanged.
			static const int				kLegacyNumStreams = 49;

			// kNumStreams / kDefaultNumStreams is now a SANITY BOUND
			// only, not a collision-avoidance requirement -- with the
			// two-tier storage above, a stream index can never alias
			// another one, at any magnitude, because streams >=
			// kLegacyNumStreams route through XExtra, searched by the
			// literal stream number.  The bound still exists to catch
			// programming errors (a negative or absurdly large
			// streamIndex, e.g. from an uninitialized or overflowed
			// depth counter) loudly rather than silently building an
			// unbounded XExtra entry.  See Get1D()'s runtime check
			// (NOT a debug-only assert -- see its comment for why).
			//
			// kNumStreams was historically a static const; it became a
			// regular member so a future subclass can reserve additional
			// lanes above this default via the protected constructor
			// below.  When constructed via the public ctor, kNumStreams
			// = kDefaultNumStreams exactly as before.
			static const int				kDefaultNumStreams = 4096;
			int								kNumStreams;		///< Sanity bound on streamIndex (per-instance)
			int								streamIndex;		///< Current active stream

			// Mutation parameters
			Scalar							largeStepProb;		///< Probability of a large (independent) step
			bool							isLargeStep;		///< Whether the current proposal is a large step
			unsigned int					lastLargeStepIteration;	///< Last iteration that was a large step

			// Exponential perturbation range (Kelemen's log-uniform trick).
			// s1 and s2 define the min/max perturbation magnitudes.
			// Typical values: s1=1/1024, s2=1/64.
			static const Scalar				s1;					///< Minimum perturbation magnitude
			static const Scalar				s2;					///< Maximum perturbation magnitude
			static const Scalar				logRatio;			///< Precomputed -log(s2/s1)

			RandomNumberGenerator			rng;				///< Source of fresh randomness

			virtual ~PSSMLTSampler();

			/// Apply a small exponential perturbation to a value in [0,1).
			/// Uses the Kelemen log-uniform distribution:
			///   delta = s2 * exp(logRatio * u),  u ~ Uniform[0,1)
			///   result = (value +/- delta) mod 1.0
			/// This concentrates most mutations near the current value
			/// while still allowing occasional larger jumps within the
			/// perturbation range [s1, s2].
			Scalar Mutate( const Scalar value );

		protected:
			/// Subclass-only constructor that allows reserving additional
			/// sample streams beyond the 49 PSSMLT needs.  MMLTSampler
			/// uses this to allocate streams 49 ((s,t) selection) and
			/// 50 (lens position).  Pass numStreams_ = 49 to behave
			/// identically to the public PSSMLTSampler constructor.
			PSSMLTSampler(
				const unsigned int seed,
				const Scalar largeStepProb_,
				const int numStreams_
				);

		public:
			/// Construct a PSSMLTSampler.
			/// \param seed          RNG seed for this sampler instance
			/// \param largeStepProb_ Probability of taking a large (independent) step.
			///                       Default 0.3 balances exploration vs exploitation.
			PSSMLTSampler(
				const unsigned int seed,
				const Scalar largeStepProb_ = 0.3
				);

			//////////////////////////////////////////////////////////////
			// ISampler interface
			//////////////////////////////////////////////////////////////

			/// Returns a single uniform random sample in [0,1).
			/// If this is a large step, returns a fresh random.
			/// If this is a small step, returns a mutated version of
			/// the stored value at the current index.
			/// Lazily grows the sample vector if needed.
			Scalar Get1D();

			/// Returns a 2D uniform random sample in [0,1)^2.
			Point2 Get2D();

			/// Switch to a new sample stream and reset the within-stream
			/// sample index.  Streams 0-47 are used by BDPTIntegrator;
			/// stream `BDPTCameraUtilities::kPSSMLTFilmLensApertureStream`
			/// (2048, DL-08) is reserved for the MLT film/lens/aperture
			/// block -- see `kDefaultNumStreams`'s comment above for why
			/// it is no longer the literal 48.
			void StartStream( int streamIndex );

			//////////////////////////////////////////////////////////////
			// MLT-specific interface
			//////////////////////////////////////////////////////////////

			/// Begin a new mutation proposal.  Decides whether this
			/// iteration will be a large step (fresh random) or small
			/// step (perturbation), resets sampleIndex to 0.
			void StartIteration();

			/// Accept the current proposal: discard backups, advance
			/// the iteration counter.  Called when the Metropolis
			/// acceptance test passes.
			void Accept();

			/// Reject the current proposal: restore all modified samples
			/// from their backups.  Called when the acceptance test fails.
			void Reject();

			/// \return The large step probability for this sampler.
			Scalar GetLargeStepProb() const { return largeStepProb; }

			/// \return The current iteration number.
			unsigned int GetCurrentIteration() const { return currentIteration; }

			/// Re-seed the internal mutation RNG without disturbing the
			/// primary sample vector or iteration counters.  Used by
			/// MLTRasterizer::InitChain to make every chain start at
			/// the bootstrap-selected path (so the bootstrap CDF's
			/// importance sampling is preserved) but then diverge per
			/// chain via different mutation deltas.  Called between
			/// the chain's first Accept() and any subsequent
			/// StartIteration().
			void ReSeedRNG( const unsigned int newSeed )
			{
				rng = RandomNumberGenerator( newSeed );
			}
		};
	}
}

#endif
