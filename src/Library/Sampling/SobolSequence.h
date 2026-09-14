//////////////////////////////////////////////////////////////////////
//
//  SobolSequence.h - Owen-scrambled Sobol' sequence.
//
//    Two draw shapes, deliberately different (see "Two draw shapes"
//    below):
//      Sample()      - one dimension's own Sobol' sequence, built from
//                      its own primitive polynomial over GF(2) and the
//                      Joe-Kuo initial direction numbers, then Owen-
//                      scrambled with a per-dimension hash of the seed.
//      SamplePair()  - PADDED: the (0,2)-net formed by Sobol'
//                      dimensions 0 and 1, evaluated at a sample index
//                      permuted per dimension group.
//
//    Convergence improves from O(N^-1) (independent sampling) to
//    O(N^-3/2 (log N)^(s-1)) for smooth integrands.
//
//  History -- why this is NOT wholesale "padded" Sobol (DL-81, 2026-09)
//  -------------------------------------------------------------------
//    This class used to implement the "padding" trick for EVERY draw:
//    only Sobol' dimensions 0 and 1 were ever generated, the requested
//    dimension was reduced to `dimension & 1`, and dimensions of the
//    same parity were distinguished ONLY by their Owen scramble seed.
//
//    That is not a legitimate use of padding.  Padding (Burley 2020;
//    PBRT-v4's `PaddedSobolSampler`) also PERMUTES THE SAMPLE INDEX per
//    dimension group; without that permutation the two draws are
//    `Owen_a(v_i)` and `Owen_b(v_i)` for the SAME base value `v_i`, and
//    Owen scrambling is defined digit by digit -- the leading digit of
//    each output is the leading digit of `v_i` XOR one per-dimension
//    bit.  Two same-parity dimensions were therefore locked together at
//    the top digit whatever their seeds: measured over 2^20 samples,
//    such a pair occupied only TWO of the four dyadic 2x2 boxes, each
//    at double density (`SobolDimensionParityTest`).
//
//    `SobolSampler::kStreamStride` is 32 -- even -- so every bounce's
//    stream started at a dimension of matching parity to every other
//    bounce's, and the collapsed pairs were exactly the draws that must
//    be independent: Fresnel reflect/refract choice, multi-lobe
//    selection and Russian roulette at successive bounces.  The
//    render-visible consequence was a systematic per-channel bias in
//    RGB dispersion (two channels at the SAME index of refraction
//    rendering 12.1% apart).  See docs/DL81_SOBOL_DIMENSION_PARITY.md.
//
//  Two draw shapes, and why they differ
//  ------------------------------------
//    `Sample()` (the Get1D path) gives each dimension its OWN Sobol'
//    dimension.  Nothing is padded, nothing is reduced modulo 2, and
//    the multi-dimensional net structure of the sequence is what
//    stratifies one dimension against another.
//
//    `SamplePair()` (the Get2D path) is genuine padding, index
//    permutation included.  Dimensions 0 and 1 are a PERFECT (0,2)-net
//    -- every 2D Get2D consumer (a film position, a lens point, a
//    hemisphere sample, a light-surface point) wants exactly that -- so
//    the pair is drawn from those two dimensions at a permuted index
//    rather than from two arbitrary high dimensions whose joint
//    2-dimensional projection has a large t-value.  The permutation is
//    what decorrelates one group from the next; see `ScrambleIndex`
//    for the property that makes it legitimate.
//
//    Before DL-81 round 2, `Get2D` drew two CONSECUTIVE dimensions from
//    the per-dimension table.  Measured on that table, its 2310
//    consecutive pairs have mean t-value 2.51 at m = 8 and max 7, and
//    the thin-lens aperture pair (dimensions 2309, 2310) sat at t = 6.
//    Under the original padding those same two consecutive dimensions
//    had been Sobol' 0 and 1 -- a perfect net -- so restoring the net
//    here recovers a property the first DL-81 fix had silently
//    dropped.
//
//  Direction numbers
//  -----------------
//    Initial direction numbers come from Joe & Kuo's searched tables
//    (`new-joe-kuo-6.21201`, criterion D(6)), embedded by
//    `tools/GenerateSobolDirectionNumbers.cpp` into
//    `SobolDirectionNumbers.cpp` -- which carries their licence, the
//    provenance and the regeneration command.  Each record is
//    `s, a, m_1 .. m_s`: the degree of the dimension's primitive
//    polynomial, the number whose bits are the polynomial's interior
//    coefficients, and the initial direction numbers.  The remaining
//    direction numbers follow from Sobol's recurrence.
//
//    Only the initial numbers are embedded, not the expanded 32-per-
//    dimension table: that is 555 KiB of read-only data against the
//    1 MiB the expansion would need, and the expansion itself is one
//    pass of XORs at first use.  The generator re-verifies the data
//    every time it runs -- admissibility (m_i odd, m_i < 2^i), actual
//    primitivity of every (s, a), and the canonical Sobol' ordering
//    with no gaps -- and `SobolDimensionParityTest` section E re-checks
//    all three against independently written code at test time.
//
//    Why searched initial numbers and not a hash.  A deterministic
//    hashed admissible choice is a valid (t,s)-sequence -- Sobol's
//    theorem makes the t-value depend only on the polynomial degrees --
//    but it leaves the pairwise 2-dimensional projections to chance.
//    Measured over the production dimension set (streams 0..24 x slots
//    0..7), hashed initial numbers collapse 163 of 19900 dimension
//    pairs at 256 samples per pixel and 40 at 1024; Joe-Kuo's collapse
//    131 and 23.  Their searched tables are also the reason to prefer
//    them for the low dimensions specifically: dimensions 0..199 taken
//    consecutively collapse only 75 pairs at 256 spp and NONE at 512,
//    which is the pigeonhole floor (see below).
//
//  What this does NOT fix, and why no direction numbers could
//  ---------------------------------------------------------
//    Some PAIRS of these dimensions are still poorly distributed
//    against each other at low sample counts, and that is a counting
//    fact, not a defect of the initial direction numbers.  Over the
//    first 2^M samples only index bits 0..M-1 vary, so a dimension's
//    generator matrix is truncated to M columns and its LEADING row
//    takes one of only 2^(M-1) values (m_1 = 1 pins the top bit).  Two
//    dimensions whose leading rows are equal have a fully collapsed
//    dyadic 2x2 occupancy -- half the boxes empty, half at double
//    density -- for as long as the render stays below 2^M samples.
//
//    The production set a render draws from is streams 0..24 x slots
//    0..7, which is 200 dimensions.  At M = 6 (64 samples per pixel)
//    there are 32 leading rows to go round, so AT LEAST 528 of the
//    19900 pairs must collide however the direction numbers are
//    chosen; at M = 8 (256 spp), 128 rows, at least 72.  Measured with
//    Joe-Kuo: 616 and 131.  It is therefore NOT possible to certify
//    this set pairwise-distinct at 64 or 256 samples per pixel -- the
//    claim would have to be that 200 objects fit in 32 (or 128) boxes.
//    `SobolDimensionParityTest` section C sweeps the whole set at 64,
//    256 and 1024 spp, prints the counting floor beside the measured
//    count, and pins the measured count against regression.
//
//    A dimension-layout change was measured and REJECTED: mapping
//    (stream, slot) to `slot * 256 + stream` instead of
//    `stream * 32 + slot` moves the production set into a different
//    part of the table but not into a better one (151 collided pairs
//    at M = 8 against the strided layout's 131).  Only genuine
//    COMPACTION into dimensions 0..199 improves on it (75 pairs --
//    close to, but not exactly, the counting floor of 72 for 200
//    dimensions in 128 boxes at M = 8; consecutive Joe-Kuo dimensions
//    are merely near-optimal, not floor-exact), and a progressive
//    renderer cannot compact: it does not know at draw time which
//    (stream, slot) pairs the walk will reach.
//
//    The pre-DL-81 padding had this collapse on 100% of same-parity
//    pairs -- which was every bounce-to-bounce pair -- at EVERY sample
//    count, so the change is 100% to 0.66% of production pairs at 256
//    samples per pixel, and better from there.
//
//  Get2D's index-scramble collapse at low spp (round-2 review, P2-1)
//  -------------------------------------------------------------------
//    `ScrambleIndex` has its own counting floor, independent of the
//    direction-number one above, and it is already essentially AT that
//    floor -- no scramble function can do meaningfully better.
//
//    For a sample index below 2^M, `ScrambleIndex(index, group)`'s
//    output bit 0 (which becomes, via `SobolDim0`'s bit-reversal, the
//    LEADING and therefore coarsest digit of `u`) has the form
//    `i0 XOR g_group(i1 .. i_{M-1})` for some Boolean function g_group
//    of the index's other M-1 bits -- Owen scrambling is a digit tree,
//    so the coarsest digit's flip can depend only on OTHER, not finer,
//    digits.  There are only 2^(2^(M-1)) possible such functions.  Two
//    Get2D groups whose `g` happens to match (probability ~2 in that
//    count, matching or exactly complementary) have their leading
//    digit locked together for EVERY sample index below 2^M, in every
//    pixel (the index scramble is seeded from the group id alone, per
//    `ScrambleIndex`'s own doc above) -- the same dyadic-2x2 signature
//    DL-81 was about, just between two Get2D GROUPS instead of two
//    Get1D dimensions.
//
//    At M = 2 (4 spp) the family has only 4 members, so with the
//    production Get2D group set (streams 0..24 x slots 0..7 plus the
//    BDPT strategy-select group, 201 groups) the pigeonhole bound is
//    essentially "50% of all group pairs collide", and this table's
//    ScrambleIndex measures at exactly that: 50.000%, 12.4% (M=3,
//    floor 12.5%), 0.776% (M=4, floor 0.78%), ~0.02% (M=5), 0% (M>=6)
//    of the 80400 (group, coordinate) pair combinations
//    (`SobolDimensionParityTest` section F).  A two-round composition
//    of `ScrambleIndex` (independently seeded) was measured against
//    this and did not reliably improve it (12.5% became 12.5%, 0.78%
//    stayed 0.78%) -- consistent with the floor already being met, not
//    with a defect in the mixing.  So, as with the direction-number
//    floor: THIS IS NOT FIXABLE by a better index scramble, and no
//    change was made here.  Judge any render sensitive to Get2D
//    stratification (film position, lens point, hemisphere or
//    light-surface sample) at 64 samples per pixel or above, where the
//    floor is already zero.
//
//  References:
//    - Sobol', "On the distribution of points in a cube and the
//      approximate evaluation of integrals", USSR Comp. Math. 1967
//    - Bratley and Fox, "ALGORITHM 659: Implementing Sobol's
//      quasirandom sequence generator", ACM TOMS 1988
//    - Joe and Kuo, "Constructing Sobol sequences with better
//      two-dimensional projections", SIAM J. Sci. Comput. 2008
//    - Burley, "Practical Hash-based Owen Scrambling", JCGT 2020
//    - Owen, "Randomly Permuted (t,m,s)-Nets and (t,s)-Sequences",
//      Monte Carlo and Quasi-Monte Carlo Methods, 1995
//    - Pharr, Jakob, Humphreys, "Physically Based Rendering" (4e),
//      Chapter 8: Sampling and Reconstruction
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 27, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SOBOL_SEQUENCE_H
#define SOBOL_SEQUENCE_H

#include <stdint.h>

namespace RISE
{
	//
	// Joe-Kuo initial direction numbers.  Defined in
	// SobolDirectionNumbers.cpp, which is GENERATED -- see that file
	// for the source data, its licence and the regeneration command.
	// Declared here rather than in a header of their own so that the
	// 555 KiB of table data is parsed by exactly ONE translation unit.
	//
	//! Flat `s, a, m_1 .. m_s` records for Sobol' dimensions 1 .. N-1.
	const uint32_t* SobolJoeKuoInitialNumbers();
	//! How many Sobol' dimensions the records cover, dimension 0 included.
	unsigned int SobolJoeKuoDimensionCount();
	//! How many uint32_t values the record array holds.
	unsigned int SobolJoeKuoRecordCount();

	//
	// Owen-scrambled Sobol' sequence utilities.
	//
	// All functions are static and stateless; a draw is fully
	// determined by (sampleIndex, dimension, seed).
	//
	class SobolSequence
	{
	public:

		//////////////////////////////////////////////////////////////
		// ReverseBits32 - bit-reversal of a 32-bit integer.
		// Uses hardware RBIT on ARM64 (Apple Silicon) when available,
		// otherwise falls back to a portable 5-step swap.
		//////////////////////////////////////////////////////////////
		static inline uint32_t ReverseBits32( uint32_t n )
		{
#if defined(__aarch64__) || defined(_M_ARM64)
			// ARM64: single-cycle RBIT instruction
			uint32_t result;
			__asm__("rbit %w0, %w1" : "=r"(result) : "r"(n));
			return result;
#else
			n = (n << 16) | (n >> 16);
			n = ((n & 0x00ff00ff) << 8) | ((n & 0xff00ff00) >> 8);
			n = ((n & 0x0f0f0f0f) << 4) | ((n & 0xf0f0f0f0) >> 4);
			n = ((n & 0x33333333) << 2) | ((n & 0xcccccccc) >> 2);
			n = ((n & 0x55555555) << 1) | ((n & 0xaaaaaaaa) >> 1);
			return n;
#endif
		}

		//////////////////////////////////////////////////////////////
		// SobolDim0 - Sobol dimension 0 (Van der Corput in base 2).
		// Equivalent to ReverseBits32(index) interpreted as a
		// fixed-point fraction.
		//////////////////////////////////////////////////////////////
		static inline uint32_t SobolDim0( uint32_t index )
		{
			return ReverseBits32( index );
		}

		//////////////////////////////////////////////////////////////
		// SobolDim1 - Sobol dimension 1 of the (0,2)-sequence.
		// Uses direction numbers generated by the recurrence for
		// primitive polynomial x+1 (degree 1, a=0):
		//   d_0 = 1 << 31
		//   d_{i+1} = d_i ^ (d_i >> 1)
		//
		// The sample is built by XOR-ing direction numbers for
		// each set bit of the index (Gray-code-like construction).
		//////////////////////////////////////////////////////////////
		static inline uint32_t SobolDim1( uint32_t index )
		{
			uint32_t v = 0;
			uint32_t d = 1u << 31;
			while( index != 0 )
			{
				if( index & 1 )
					v ^= d;
				d ^= (d >> 1);		// next direction number
				index >>= 1;
			}
			return v;
		}

		//! How many genuinely distinct Sobol' dimensions are built.
		//!
		//! 8192 = 256 * `SobolSampler::kStreamStride`, i.e. stream
		//! indices 0..255 each get 32 dimensions of their own.  That
		//! covers every stream a SHIPPED scene can reach:
		//!
		//!   film / light-source select        stream 0
		//!   light-subpath bounce d            1 + d
		//!   eye-subpath bounce d              16 + d
		//!   BDPT strategy select              47
		//!   MLT film / lens / aperture        48
		//!   VCM per-eye-vertex NEE            48 + i, i >= 1
		//!   thin-lens aperture (Get2D only)   3322 * 32, see
		//!                                     BDPTCameraUtilities
		//!
		//! Both walk loops run `maxEyeDepth + maxVolumeBounce`
		//! iterations (saturated at 1024), and VCM's per-vertex NEE
		//! stream is `48 + i` over the eye vertices the walk produced.
		//! The deepest shipped scene is
		//! scenes/FeatureBased/Combined/diamond_teapot_pour.RISEscene
		//! -- `vcm_pel_rasterizer`, `max_eye_depth 128`, default
		//! `max_volume_bounce 64` -- giving 192 iterations and so a
		//! highest stream of 48 + 192 + 1 = 241, inside 256.
		//! `SobolDimensionBudgetTest` Test G recomputes that bound from
		//! the scene files themselves, so a scene that raises a depth
		//! past the table turns the test red.
		//!
		//! A walk iteration CAN append a second vertex (the BSSRDF
		//! entry vertex), which would double VCM's stream count on a
		//! subsurface scene deep enough to matter; the deepest shipped
		//! subsurface scene is at depth 16 and nowhere near it.  Beyond
		//! the table, draws do not alias -- they are re-indexed, see
		//! `Sample` -- so this is a quality bound, not a correctness
		//! one.
		//!
		//! Cost: 8192 * 32 * 4 = 1 MiB of expanded table, built once
		//! per process at first use, from 555 KiB of embedded initial
		//! direction numbers.
		static const unsigned int kNumDimensions = 8192;

		//! Bits of the sample index a direction-number set covers.
		static const unsigned int kNumBits = 32;

	private:

		//////////////////////////////////////////////////////////////
		// DirectionNumbers - the expanded table.
		//
		// v[j][i] is the i-th direction number of Sobol' dimension j,
		// left-aligned in 32 bits (v_i * 2^32).  Built once, on first
		// use, from the embedded Joe-Kuo initial numbers.
		//////////////////////////////////////////////////////////////
		struct DirectionNumbers
		{
			uint32_t v[kNumDimensions][kNumBits];

			DirectionNumbers()
			{
				// Dimension 0: the van der Corput sequence.  No
				// polynomial; direction numbers are the plain powers of
				// two, so the sample is the bit-reversed index.
				for( unsigned int i = 0; i < kNumBits; i++ ) {
					v[0][i] = 1u << ( kNumBits - 1u - i );
				}

				// Dimensions 1..N-1: one variable-length record each,
				// laid end to end.  The record order IS the dimension
				// order, so a linear walk needs no offset table.
				const uint32_t* rec = SobolJoeKuoInitialNumbers();
				const unsigned int avail = SobolJoeKuoDimensionCount();
				const unsigned int last =
					( avail < kNumDimensions ) ? avail : kNumDimensions;

				for( unsigned int dim = 1; dim < last; dim++ )
				{
					const unsigned int s = *rec++;
					const uint32_t a     = *rec++;
					uint32_t* w = v[dim];

					// m_i is admissible (odd, m_i < 2^i), left-aligned.
					for( unsigned int i = 1; i <= s; i++ ) {
						w[i - 1] = rec[i - 1] << ( kNumBits - i );
					}
					rec += s;

					// Sobol's recurrence, in left-aligned form:
					//   V_i = V_(i-s) ^ (V_(i-s) >> s)
					//         ^ XOR over k of a_k * V_(i-k)
					// with a_k the coefficient of x^(s-k) in the
					// polynomial p = x^s + a_1 x^(s-1) + ... + 1, whose
					// bit pattern is (1 << s) | (a << 1) | 1.
					const uint32_t p = ( 1u << s ) | ( a << 1 ) | 1u;
					for( unsigned int i = s; i < kNumBits; i++ ) {
						uint32_t t = w[i - s] ^ ( w[i - s] >> s );
						for( unsigned int k = 1; k < s; k++ ) {
							if( ( p >> ( s - k ) ) & 1u ) t ^= w[i - k];
						}
						w[i] = t;
					}
				}

				// Defensive: if the embedded data ever covers fewer
				// dimensions than the table, leave the rest as
				// dimension 0 rather than as zeros (a zero row would
				// return 0 for every index).  The parity test's
				// section E asserts this never happens in a shipped
				// build.
				for( unsigned int dim = last; dim < kNumDimensions; dim++ ) {
					for( unsigned int i = 0; i < kNumBits; i++ ) {
						v[dim][i] = v[0][i];
					}
				}
			}
		};

		//! The table, built on first use.  C++11 guarantees the
		//! initialisation is run exactly once and is thread-safe.
		static inline const DirectionNumbers& Directions()
		{
			static const DirectionNumbers table;
			return table;
		}

	public:

		//////////////////////////////////////////////////////////////
		// DirectionNumber - the i-th direction number of dimension
		// `dim`, exactly as the table holds it.
		//
		// Exists so a test can read what the generator BUILT rather
		// than what `Sobol` returns: `Sobol` short-circuits dimensions
		// 0 and 1 to their closed forms, so comparing it against those
		// same closed forms proves nothing about the table.
		//////////////////////////////////////////////////////////////
		static inline uint32_t DirectionNumber( uint32_t dim, uint32_t i )
		{
			if( dim >= kNumDimensions || i >= kNumBits ) return 0;
			return Directions().v[dim][i];
		}

		//////////////////////////////////////////////////////////////
		// Sobol - raw (unscrambled) Sobol' sample for dimension
		// `dim`, returned as a uint32_t in [0, 2^32).
		//
		// A dimension at or above kNumDimensions is reduced modulo the
		// table size.  Callers that care about WHICH dimension they
		// land on after that reduction must reduce themselves and
		// re-index the sample -- `Sample` does exactly that; this
		// reduction is only here so a stray argument cannot read off
		// the end of the table.
		//
		// Dimensions 0 and 1 keep their closed forms: they are what
		// the table's first two rows contain, and skipping the table
		// keeps the image-plane pair off the one-time-init guard.
		//////////////////////////////////////////////////////////////
		static inline uint32_t Sobol( uint32_t index, uint32_t dim )
		{
			if( dim == 0 ) return SobolDim0( index );
			if( dim == 1 ) return SobolDim1( index );
			if( dim >= kNumDimensions ) dim %= kNumDimensions;

			const uint32_t* w = Directions().v[dim];
			uint32_t r = 0;
			for( unsigned int i = 0; index != 0; index >>= 1, i++ ) {
				if( index & 1u ) r ^= w[i];
			}
			return r;
		}

		//////////////////////////////////////////////////////////////
		// HashCombine - mix two 32-bit values into a well-distributed
		// hash.  Uses the MurmurHash3 32-bit finalizer (fmix32) for
		// strong avalanche properties — every input bit affects
		// every output bit with ~50% probability.
		//////////////////////////////////////////////////////////////
		static inline uint32_t HashCombine( uint32_t a, uint32_t b )
		{
			// Combine inputs with golden-ratio offset to avoid
			// degenerate zero-seed when both inputs are zero
			a ^= b + 0x9e3779b9u + (a << 6) + (a >> 2);

			// MurmurHash3 fmix32 finalizer
			a ^= a >> 16;
			a *= 0x85ebca6bu;
			a ^= a >> 13;
			a *= 0xc2b2ae35u;
			a ^= a >> 16;
			return a;
		}

		//////////////////////////////////////////////////////////////
		// OwenScramble - Laine-Karras hash approximation of Owen
		// scrambling (Burley, JCGT 2020).
		//
		// Owen scrambling recursively permutes the digits of a
		// radical-inverse sample in a tree-structured fashion,
		// preserving all stratification properties while randomizing
		// the sequence.  The Laine-Karras hash is a fast O(1)
		// approximation that operates on the bit-reversed Sobol
		// value.
		//
		// Input v: raw Sobol sample (uint32_t in [0, 2^32))
		// Input seed: scramble seed (different per dimension group)
		// Returns: scrambled value in [0, 2^32)
		//////////////////////////////////////////////////////////////
		static inline uint32_t OwenScramble( uint32_t v, uint32_t seed )
		{
			// The Laine-Karras hash operates on the reversed
			// representation (MSB = coarsest digit for Owen tree
			// traversal), applies mixing, then reverses back.
			v = ReverseBits32( v );

			// Mixing rounds (Burley 2020, Listing 1)
			v ^= v * 0x3d20adeau;
			v += seed;
			v *= (seed >> 16) | 1u;
			v ^= v * 0x05526c56u;
			v ^= v * 0x53a22864u;

			v = ReverseBits32( v );
			return v;
		}

		//////////////////////////////////////////////////////////////
		// ScrambleIndex - permute the SAMPLE INDEX, preserving every
		// dyadic prefix's net property.
		//
		// This is the piece the pre-DL-81 padding was missing, and the
		// piece PBRT-v4's `PaddedSobolSampler` supplies with
		// `PermutationElement(sampleIndex, samplesPerPixel, hash)` --
		// which needs the samples-per-pixel count, a number a
		// progressive renderer does not have at draw time.  An Owen
		// scramble of the index needs no such parameter, because of
		// this property:
		//
		//   `OwenScramble` is unitriangular from the TOP -- output bit
		//   j depends only on input bits >= j (that is what the
		//   reverse / mix / reverse sandwich buys; the mix itself is
		//   lower-triangular, every step being `x ^= x * even`,
		//   `x += c` or `x *= odd`).  So for every input below 2^M,
		//   output bits >= M depend only on input bits >= M, which are
		//   all zero -- they take one CONSTANT value c.  The map is a
		//   bijection, so
		//
		//       ScrambleIndex( {0 .. 2^M - 1} )  =  {c*2^M .. c*2^M + 2^M - 1}
		//
		//   simultaneously for EVERY M: a prefix goes to a dyadic
		//   BLOCK of the same length, aligned at a multiple of its
		//   length.  Every such block of a (0,2)-sequence is itself a
		//   (0,M,2)-net, so the first 2^M samples of a padded group are
		//   as well stratified as the unpermuted prefix, at every M at
		//   once, and two groups land on different blocks.
		//
		// Seeded from the dimension group alone, NOT from the pixel
		// seed: all pixels must permute the index identically or
		// `ZSobolSampler`'s Morton-ordered global index -- the whole
		// mechanism behind its cross-pixel blue noise -- would be
		// shuffled away.  Per-pixel decorrelation is the job of the
		// value-side Owen scramble, which does take the pixel seed.
		//////////////////////////////////////////////////////////////
		static inline uint32_t ScrambleIndex( uint32_t index, uint32_t group )
		{
			return OwenScramble( index, HashCombine( 0x9e3779b1u, group ) );
		}

		//////////////////////////////////////////////////////////////
		// Sample - one dimension's own Sobol' sequence, Owen-scrambled.
		//
		// This is the Get1D path.  Each dimension draws from its OWN
		// Sobol' dimension and gets its own Owen scramble seed derived
		// from the base seed.  See this file's header for why the
		// dimension is NOT reduced modulo 2 (DL-81) -- that reduction
		// made same-parity dimensions a rigid pair rather than
		// independent decisions.
		//
		// Dimensions at or above kNumDimensions do not simply alias
		// onto a lower dimension, which would recreate exactly the
		// DL-81 collapse for the aliased pair: the SAMPLE INDEX is
		// Owen-permuted by the wrap count first, so a wrapped draw is
		// the same Sobol' dimension read over a DIFFERENT, equally
		// well stratified dyadic block (see `ScrambleIndex`).  Two
		// streams that wrap onto each other are decorrelated; they are
		// not a joint net, which is why `kNumDimensions` is sized to
		// keep every shipped scene off this path entirely.
		//
		// sampleIndex: which sample in the sequence (0, 1, 2, ...)
		// dimension:   which dimension (0, 1, 2, 3, ...)
		// seed:        base scramble seed (e.g., hash of pixel coords)
		//////////////////////////////////////////////////////////////
		static inline double Sample(
			uint32_t sampleIndex,
			uint32_t dimension,
			uint32_t seed
			)
		{
			uint32_t sobolDim = dimension;
			uint32_t index    = sampleIndex;
			if( dimension >= kNumDimensions ) {
				const uint32_t wrap = dimension / kNumDimensions;
				sobolDim = dimension - wrap * kNumDimensions;
				// Complemented so a wrap count can never collide with
				// a `SamplePair` group id, which is a raw dimension.
				index    = ScrambleIndex( sampleIndex, ~wrap );
			}

			uint32_t v = Sobol( index, sobolDim );

			// Derive a per-dimension scramble seed
			uint32_t dimSeed = HashCombine( seed, dimension );
			v = OwenScramble( v, dimSeed );

			return ToUnit( v );
		}

		//////////////////////////////////////////////////////////////
		// SamplePair - the Get2D path: a padded (0,2)-net pair.
		//
		// Both coordinates come from Sobol' dimensions 0 and 1, which
		// are a perfect (0,2)-net, evaluated at an index permuted per
		// dimension group (`ScrambleIndex`) so that consecutive groups
		// are decorrelated.  The two coordinates then get their own
		// Owen scramble seeds, exactly as `Sample` does; Owen
		// scrambling applied per coordinate preserves the net (Owen
		// 1995), so every Get2D is a (0,m,2)-net for every m at once.
		//
		// Group 0 -- the image-plane / primary pair -- is drawn at the
		// UNPERMUTED index, which keeps it bit-identical to every
		// version of this class and, more importantly, leaves
		// `ZSobolSampler`'s Morton-ordered index reaching Sobol'
		// dimensions 0 and 1 untouched, since that pair is the one its
		// screen-space blue noise is about.
		//
		// `dimension` is the FIRST of the two dimension slots the
		// caller is consuming, and is used raw -- not reduced modulo
		// kNumDimensions -- so a deliberately distant stream (the
		// thin-lens aperture at stream 3322) keys its own group and
		// can never share one with a walk stream.
		//
		// Round-2-review note (P3-1, not fixed): at `dimension == 0`
		// ONLY, `SamplePair(i, 0, seed).outU` is bit-for-bit identical
		// to `Sample(i, 0, seed)`, and `.outV` to `Sample(i, 1, seed)`
		// -- group 0 draws at the unpermuted index with the SAME
		// per-dimension seed `Sample` would use, so the two functions
		// coincide exactly at this one group.  For dimension > 0 they
		// never coincide (`SamplePair` reads a `ScrambleIndex`-permuted
		// index there; `Sample` never permutes its index below
		// `kNumDimensions`), so this is a dimension-0-only artefact,
		// not a general property.  It is dormant today -- the only
		// `StartStream(0)` consumer (`SampleLight`) draws with `Get1D`
		// first, never both at the same dimension -- and it is left
		// AS IS rather than "fixed" by tagging `SamplePair`'s seed,
		// because the fix would also retag group 0's seed and break
		// the bit-identity this comment documents two paragraphs up
		// (every `SobolSampler` render's film-plane jitter would shift).
		// `SobolDimensionParityTest` section E pins the coincidence
		// explicitly so a future change to either function makes a
		// conscious choice about it instead of an accidental one.
		//////////////////////////////////////////////////////////////
		static inline void SamplePair(
			uint32_t sampleIndex,
			uint32_t dimension,
			uint32_t seed,
			double& outU,
			double& outV
			)
		{
			const uint32_t index = ( dimension == 0 )
				? sampleIndex
				: ScrambleIndex( sampleIndex, dimension );

			const uint32_t u = OwenScramble( SobolDim0( index ),
				HashCombine( seed, dimension ) );
			const uint32_t v = OwenScramble( SobolDim1( index ),
				HashCombine( seed, dimension + 1u ) );

			outU = ToUnit( u );
			outV = ToUnit( v );
		}

	private:

		//! Fixed-point 32-bit value to [0, 1).  Multiply, not divide.
		static inline double ToUnit( uint32_t v )
		{
			static const double kToUnit = 1.0 / 4294967296.0;
			return double(v) * kToUnit;
		}
	};
}

#endif
