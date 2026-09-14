//////////////////////////////////////////////////////////////////////
//
//  SobolSequence.h - Owen-scrambled multi-dimensional Sobol' sequence.
//
//    Every requested dimension draws from its OWN Sobol' dimension,
//    built from its own primitive polynomial over GF(2), and is then
//    Owen-scrambled with a per-dimension hash of the base seed.
//
//    Convergence improves from O(N^-1) (independent sampling) to
//    O(N^-3/2 (log N)^(s-1)) for smooth integrands.
//
//  History -- why this is NOT "padded" Sobol (DL-81, 2026-09-14)
//  ------------------------------------------------------------
//    This class used to implement the "padding" trick: only Sobol'
//    dimensions 0 and 1 were ever generated, the requested dimension
//    was reduced to `dimension & 1`, and dimensions of the same parity
//    were distinguished ONLY by their Owen scramble seed.
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
//    The index permutation is not available here: it has to be a
//    permutation OF THE SAMPLE SET, so it needs the samples-per-pixel
//    count, which a progressive renderer does not have at draw time.
//    Genuinely distinct Sobol' dimensions need no such parameter, so
//    that is the route taken.
//
//    Dimensions 0 and 1 are UNCHANGED by that move: dimension 0's
//    direction numbers are 2^31, 2^30, ... (the van der Corput
//    sequence, `SobolDim0`) and dimension 1 is the first primitive
//    polynomial, x+1, with unit initial direction numbers -- exactly
//    the recurrence `SobolDim1` already implemented.  The image plane's
//    Get2D() therefore still draws the same (0,2)-net it always did.
//
//  Direction numbers
//  -----------------
//    Generated at first use, not tabulated: primitive polynomials over
//    GF(2) are enumerated in the canonical Sobol' order (by degree,
//    then by increasing `a`, where the polynomial is
//    `x^d + a_1 x^(d-1) + ... + a_(d-1) x + 1` and
//    `a = (a_1 ... a_(d-1))_2`), tested by verifying that x has
//    multiplicative order exactly 2^d - 1 in GF(2)[x]/(p).  The
//    per-degree counts this produces are phi(2^d - 1)/d, checked
//    against an independent Euler-phi computation in
//    `SobolDimensionParityTest`.
//
//    Initial direction numbers m_1..m_d are admissible iff m_i is ODD
//    and m_i < 2^i -- which forces m_1 = 1 in every dimension -- and
//    Sobol's theorem makes the (t,s)-sequence property and its t-value
//    depend ONLY on the polynomial degrees, so any admissible choice
//    yields a valid sequence.  m_2..m_d here are drawn from a fixed
//    hash of (dimension, i), i.e. a deterministic random admissible
//    choice; this is Matousek-style linear scrambling of the generator
//    matrices, not an arbitrary shortcut.
//
//    They must NOT all be 1, which is the obvious-looking choice: with
//    m_i = 1 throughout, EVERY dimension of degree >= d shares the same
//    first d direction numbers 2^31, 2^30, ..., so two dimensions of
//    equal degree produce BIT-IDENTICAL values for every sample index
//    below 2^d.  Degree 13 covers dimensions 481..1110 -- the eye-bounce
//    range -- so at any production sample count (2^13 = 8192) that
//    choice would have reproduced the DL-81 collapse exactly, one
//    degree class at a time.  The pairwise quality of the hashed choice
//    is not asserted by construction; it is MEASURED, by
//    `SobolDimensionParityTest`'s bounce-to-bounce sweep at render-like
//    sample counts.
//
//  What this does NOT fix, and why no direction numbers could
//  ---------------------------------------------------------
//    Some PAIRS of these 2311 dimensions are still poorly distributed
//    against each other at low sample counts, and that is a counting
//    fact, not a defect of the initial direction numbers: over the
//    first 2^M samples only index bits 0..M-1 vary, so a dimension's
//    leading generator row has just 2^(M-1) possible values there
//    (m_1 = 1 pins one bit).  With 2311 dimensions and M = 8 (256
//    samples per pixel) there are 128 values to go round, so some pairs
//    MUST share a leading row -- which is a collapsed 2x2 dyadic
//    occupancy for that pair.  Measured, the hashed direction numbers
//    hit all 128 values and leave 0.779% of dimension pairs sharing one
//    (0.193% at 1024 samples, 0.048% at 4096); a perfectly balanced
//    assignment would give 0.734% at M = 8, so there is at most 6%
//    of headroom here and Joe & Kuo's searched tables could not claim
//    it either.  The same argument at the next level (a partial spread
//    of 2-dimensional subspaces of GF(2)^8 has at most 85 members)
//    bounds how many dimensions can be pairwise 4x4-exact at 256
//    samples per pixel.
//
//    The pre-DL-81 padding had this collapse on 100% of same-parity
//    pairs -- which was every bounce-to-bounce pair -- at EVERY sample
//    count, so the change is 100% to 0.78% at the worst sample count
//    and better from there.  Joe & Kuo's searched initial values would
//    still improve the higher dimensions' t-values, but their tables
//    are external data this build does not carry.
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
	// Owen-scrambled multi-dimensional Sobol' sequence utilities.
	//
	// All functions are static and stateless; the sequence is fully
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
		//! 2311 is the smallest prime above 50 * SobolSampler::kStreamStride
		//! (= 1600), so every stream index a production integrator uses
		//! -- film 0, light bounces 1..15, eye bounces 16..31+, BDPT's 47,
		//! VCM/MLT's 48+ -- lands on its own dimension with room to spare,
		//! and because the prime is coprime to the stride, the wraparound
		//! that a pathologically deep path eventually reaches can never
		//! align two whole streams with each other.
		static const unsigned int kNumDimensions = 2311;

		//! Bits of the sample index a direction-number set covers.
		static const unsigned int kNumBits = 32;

	private:

		//////////////////////////////////////////////////////////////
		// DirectionNumbers - the generated table.
		//
		// v[j][i] is the i-th direction number of Sobol' dimension j,
		// left-aligned in 32 bits (v_i * 2^32).  Built once, on first
		// use, by the constructor below; ~5 ms, ~289 KiB.
		//////////////////////////////////////////////////////////////
		struct DirectionNumbers
		{
			uint32_t v[kNumDimensions][kNumBits];

			//! x^e mod p in GF(2)[x], p of degree d.
			static uint32_t PolyPowMod( uint32_t base, uint32_t e, uint32_t p, unsigned int d )
			{
				uint32_t r = 1;
				while( e != 0 ) {
					if( e & 1u ) r = PolyMulMod( r, base, p, d );
					base = PolyMulMod( base, base, p, d );
					e >>= 1;
				}
				return r;
			}

			//! (a * b) mod p in GF(2)[x], p of degree d.
			static uint32_t PolyMulMod( uint32_t a, uint32_t b, uint32_t p, unsigned int d )
			{
				uint32_t r = 0;
				while( b != 0 ) {
					if( b & 1u ) r ^= a;
					b >>= 1;
					a <<= 1;
					if( ( a >> d ) & 1u ) a ^= p;
				}
				return r;
			}

			//! p (degree d, constant term 1) is primitive iff x generates
			//! the full multiplicative group of GF(2)[x]/(p), i.e. has
			//! order exactly 2^d - 1.  A polynomial for which that holds
			//! is necessarily irreducible, so no separate test is needed.
			static bool IsPrimitive( uint32_t p, unsigned int d )
			{
				const uint32_t n = ( 1u << d ) - 1u;
				if( PolyPowMod( 2u, n, p, d ) != 1u ) return false;	// x is 2
				uint32_t m = n;
				for( uint32_t q = 2; q * q <= m; q++ ) {
					if( m % q == 0 ) {
						if( PolyPowMod( 2u, n / q, p, d ) == 1u ) return false;
						while( m % q == 0 ) m /= q;
					}
				}
				if( m > 1 && PolyPowMod( 2u, n / m, p, d ) == 1u ) return false;
				return true;
			}

			DirectionNumbers()
			{
				// Dimension 0: the van der Corput sequence.  No
				// polynomial; direction numbers are the plain powers of
				// two, so the sample is the bit-reversed index.
				for( unsigned int i = 0; i < kNumBits; i++ ) {
					v[0][i] = 1u << ( kNumBits - 1u - i );
				}

				unsigned int dim = 1;
				for( unsigned int d = 1; d < kNumBits && dim < kNumDimensions; d++ )
				{
					const uint32_t aEnd = 1u << ( d - 1u );
					for( uint32_t a = 0; a < aEnd && dim < kNumDimensions; a++ )
					{
						const uint32_t p = ( 1u << d ) | ( a << 1 ) | 1u;
						if( !IsPrimitive( p, d ) ) continue;

						const unsigned int thisDim = dim++;
						uint32_t* w = v[thisDim];

						// Initial direction numbers m_1..m_d.  Admissible
						// iff m_i is ODD and m_i < 2^i -- which forces
						// m_1 = 1 in every dimension; m_2..m_d come from a
						// fixed hash of (dimension, i).  They must NOT all
						// be 1: see the header's "Direction numbers" note.
						for( unsigned int i = 1; i <= d; i++ ) {
							uint32_t m = 1u;
							if( i > 1 ) {
								const uint32_t h = HashCombine(
									HashCombine( thisDim, i ), 0x5cf6b1a3u );
								m = ( h & ( ( 1u << i ) - 1u ) ) | 1u;
							}
							w[i - 1] = m << ( kNumBits - i );
						}

						// Sobol's recurrence, in left-aligned form:
						//   V_i = V_(i-d) ^ (V_(i-d) >> d)
						//         ^ XOR over k of a_k * V_(i-k)
						// with a_k the coefficient of x^(d-k) in p.
						for( unsigned int i = d; i < kNumBits; i++ ) {
							uint32_t t = w[i - d] ^ ( w[i - d] >> d );
							for( unsigned int k = 1; k < d; k++ ) {
								if( ( p >> ( d - k ) ) & 1u ) t ^= w[i - k];
							}
							w[i] = t;
						}
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
		// Sobol - raw (unscrambled) Sobol' sample for dimension
		// `dim` < kNumDimensions, returned as a uint32_t in [0, 2^32).
		//
		// Dimensions 0 and 1 keep their closed forms: they are what
		// the table's first two rows contain, and skipping the table
		// keeps the image-plane pair off the one-time-init guard.
		//////////////////////////////////////////////////////////////
		static inline uint32_t Sobol( uint32_t index, uint32_t dim )
		{
			if( dim == 0 ) return SobolDim0( index );
			if( dim == 1 ) return SobolDim1( index );

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
		// Sample - the main entry point.
		//
		// Returns an Owen-scrambled Sobol' sample in [0, 1) for the
		// given sample index and dimension.
		//
		// Each dimension draws from its OWN Sobol' dimension and gets
		// its own Owen scramble seed derived from the base seed.  See
		// this file's header for why the dimension is NOT reduced
		// modulo 2 (DL-81) -- that reduction made same-parity
		// dimensions a rigid pair rather than independent decisions.
		//
		// Dimensions at or above kNumDimensions wrap.  The stride
		// between two streams is coprime to kNumDimensions, so a wrap
		// can only ever alias one individual draw of a very deep
		// bounce with one draw of a much shallower one, never two
		// streams as a whole.
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
			uint32_t sobolDim = dimension % kNumDimensions;
			uint32_t v = Sobol( sampleIndex, sobolDim );

			// Derive a per-dimension scramble seed
			uint32_t dimSeed = HashCombine( seed, dimension );
			v = OwenScramble( v, dimSeed );

			// Convert to [0, 1) — multiply is faster than division
			static const double kToUnit = 1.0 / 4294967296.0;
			return double(v) * kToUnit;
		}
	};
}

#endif
