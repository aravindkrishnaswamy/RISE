//////////////////////////////////////////////////////////////////////
//
//  SobolSampler.h - ISampler implementation backed by Owen-scrambled
//    Sobol' sequences.
//
//    Replaces IndependentSampler in BDPT (and any future integrator
//    that consumes ISampler).  Each call to Get1D() / Get2D() draws
//    the next dimension(s) from the Sobol sequence for the current
//    sample index, with Owen scrambling seeded by pixel coordinates.
//
//    The caller must supply:
//      sampleIndex - which sample within this pixel (0, 1, 2, ...)
//      seed        - a per-pixel base seed (e.g., hash of pixel coords)
//
//    StartStream() advances the dimension counter so that different
//    sub-path generators (light, eye, connection) draw from
//    independent dimension ranges — same mechanism as PSSMLTSampler.
//
//    NOTE (DL-81, 2026-09-14): `kStreamStride` being EVEN used to be
//    fatal, because `SobolSequence::Sample` reduced the dimension to
//    `dimension & 1` -- every bounce's k-th draw then had the parity of
//    every other bounce's k-th draw, and two same-parity dimensions
//    were two Owen-scrambled copies of ONE base value.  Distinct
//    dimensions are now distinct SEQUENCES, so an even stride is
//    harmless; see SobolSequence.h's header for the mechanism and
//    docs/DL81_SOBOL_DIMENSION_PARITY.md for the measurements.  There
//    is now a finite dimension supply (`SobolSequence::kNumDimensions`
//    = 8192 = 256 streams' worth), sized so that every PER-VERTEX
//    stream a SHIPPED scene reaches has dimensions of its own; past
//    it, Get1D draws are re-indexed rather than aliased (they
//    decorrelate, but stop being a joint net) and Get2D draws are
//    unaffected, since they are padded and keyed by the raw dimension.
//    `SobolDimensionBudgetTest` Test G1 recomputes the shipped bound
//    from the scene files.
//
//    NOTE (DL-283, 2026-09-27): "shipped scenes never wrap" is NOT the
//    rule, and has not been since DL-247.  Three stream families live
//    past the table ON PURPOSE, because they must be disjoint from
//    every per-vertex stream at any depth: PT's volume-walk streams
//    (`PathTransportUtilities::PTVolumeWalkStream`, 4096..8191, wrap
//    counts 16..31), BDPT/VCM's medium distance-sampling blocks
//    (`BDPTUtilities::MediumDistanceStream`, 8192..139263, wraps
//    32..543) and -- DL-286 -- BDPT/VCM's DEEP walk iterations (light
//    iteration >= 15, eye >= 31; `BDPTUtilities::LightWalkStream` /
//    `EyeWalkStream`, 139264..141265, wraps 544..551).  Every medium
//    render therefore draws wrapped dimensions.  Measured harmless by
//    per-pixel variance at 8 spp (no worse than pre-DL-283; DL-81 doc
//    section 9 -- ParityTest section H's collapse statistic is only a
//    sanity floor), and the deep-walk block by the salted Sobol'-vs-
//    independent deep-fog rows (DL-81 doc section 10).  Test G2
//    enumerates all three families and asserts their wrap counts and
//    collision-freedom.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 27, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SOBOL_SAMPLER_H
#define SOBOL_SAMPLER_H

#include "ISampler.h"
#include "../Sampling/SobolSequence.h"
#include <atomic>

namespace RISE
{
	namespace Implementation
	{
		//////////////////////////////////////////////////////////////
		// SobolSamplerTestHooks -- TEST-ONLY seams (DL-283 review).
		//
		// Production code never writes these; both default to "off",
		// and a SobolSampler reads them ONCE, at construction, so the
		// per-draw paths pay one predictable member-bool branch.  A test
		// sets them between renders (a render's worker hand-off goes
		// through the thread pool's own synchronisation, which orders
		// the write before every sampler the render constructs).
		//
		//  ValueSalt   XORed into every sampler's scramble seed.  That
		//              seed feeds only the VALUE-side Owen scramble
		//              (`HashCombine(seed, dimension)`); `ScrambleIndex`
		//              is keyed by the dimension group / wrap count and
		//              never sees it.  A distinct salt per render makes
		//              repeated renders INDEPENDENT randomized-QMC
		//              replicates; without it every render of a scene
		//              reuses the identical Sobol' points, so the
		//              run-to-run sd omits the QMC error entirely and
		//              a mean difference can look significant (or
		//              insignificant) for the wrong reason.
		//  Independent Replace every draw by an i.i.d. value from a
		//              STATEFUL splitmix64 stream seeded by (seed ^
		//              salt, sampleIndex), keeping the stream/dimension
		//              bookkeeping and `HasFixedDimensionBudget() ==
		//              true` -- the integrators take the identical code
		//              path, but a dimension drawn twice yields two
		//              FRESH values.  An unbiased reference no
		//              sampler-correlation defect can reach.
		//////////////////////////////////////////////////////////////
		struct SobolSamplerTestHooks
		{
			static std::atomic<uint32_t>& ValueSalt()
			{
				static std::atomic<uint32_t> v( 0u );
				return v;
			}
			static std::atomic<bool>& Independent()
			{
				static std::atomic<bool> v( false );
				return v;
			}
		};

		class SobolSampler :
			public ISampler
		{
		protected:
			uint32_t sampleIndex;		// Which sample in the sequence
			uint32_t seed;				// Per-pixel base scramble seed
			unsigned int dimension;		// Current dimension counter
			bool independent;			// SobolSamplerTestHooks::Independent at construction
			uint64_t rngState;			// splitmix64 state, independent mode only

			//! splitmix64 -> [0,1) with 53 bits (independent test mode).
			double NextIndependent()
			{
				uint64_t z = ( rngState += 0x9E3779B97F4A7C15ull );
				z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
				z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
				z = z ^ ( z >> 31 );
				return double( z >> 11 ) * ( 1.0 / 9007199254740992.0 );
			}

			// Dimensions are partitioned into fixed-size phases so
			// that each bounce always starts at the same dimension
			// offset regardless of how many dimensions previous
			// bounces consumed (material-dependent).  This preserves
			// cross-pixel Sobol stratification.
			//
			// Encoding: phase = streamBase + bounceIndex
			//   Light source sampling:   phase 0
			//   Light bounces d < 15:    phases 1 + d        (1..15)
			//   Eye bounces d < 31:      phases 16 + d       (16..46)
			//   BDPT strategy select:    phase 47
			//   VCM NEE, eye vertex i:   phase 48 + i (i >= 1)
			//   Deeper light / eye bounces (DL-286): a block of their
			//   own past the medium-distance blocks, 139264 + (d - 15)
			//   and 140273 + (d - 31) -- BDPTUtilities::
			//   LightWalkStream / EyeWalkStream.
			// The bounce ranges were laid out for 15 bounces; until
			// DL-286 deeper walks continued as 1 + d / 16 + d and ran
			// into each other's phases (light bounce 15 re-opened eye
			// bounce 0's, and so on) -- one dimension driving a
			// light-vertex and an eye-vertex decision of one path, a
			// -12 % bias in dense fog.
			// The full map, including the wrap-region families past
			// the dimension table, is SobolDimensionBudgetTest G2.
			//
		public:
			// kStreamStride must be >= max dimensions consumed by
			// any single phase (BioSpecSkinSPF uses ~20 + lobe
			// selection + BSSRDF = ~25 max).  Public so a stream
			// layout that must hold an open-ended consumer can
			// static_assert its width (BDPTUtilities::
			// MediumDistanceStream, DL-283).
			static const unsigned int kStreamStride = 32;

			virtual ~SobolSampler(){};

		public:
			SobolSampler(
				uint32_t sampleIndex_,
				uint32_t seed_
				) :
				sampleIndex( sampleIndex_ ),
				seed( seed_ ^ SobolSamplerTestHooks::ValueSalt().load( std::memory_order_relaxed ) ),
				dimension( 0 ),
				independent( SobolSamplerTestHooks::Independent().load( std::memory_order_relaxed ) ),
				rngState( ( uint64_t( seed ) << 32 ) ^ uint64_t( sampleIndex_ ) ^ 0xD1B54A32D192ED03ull )
			{
			}

			//! Returns a single Owen-scrambled Sobol sample in [0,1)
			Scalar Get1D()
			{
				if( independent ) { dimension++; return Scalar( NextIndependent() ); }
				return Scalar( SobolSequence::Sample( sampleIndex, dimension++, seed ) );
			}

			//! Returns a 2D Owen-scrambled Sobol sample in [0,1)^2.
			//!
			//! Drawn as a PADDED (0,2)-net pair -- Sobol' dimensions 0
			//! and 1 at an index permuted by this dimension group --
			//! not as two consecutive rows of the per-dimension table.
			//! Two consecutive table rows are a legitimate 2D
			//! projection but not a net: measured on the first
			//! DL-81 table, the 2310 consecutive pairs have mean
			//! t-value 2.51 at 256 samples per pixel and max 7, and
			//! the thin-lens aperture pair reached 6 -- where
			//! dimensions 0 and 1 are t = 0 by construction.  See
			//! SobolSequence::SamplePair.
			Point2 Get2D()
			{
				double u = 0.0, v = 0.0;
				if( independent ) {
					u = NextIndependent();
					v = NextIndependent();
					dimension += 2;
					return Point2( Scalar( u ), Scalar( v ) );
				}
				SobolSequence::SamplePair( sampleIndex, dimension, seed, u, v );
				dimension += 2;
				return Point2( Scalar( u ), Scalar( v ) );
			}

			//! SobolSampler uses fixed-size phases (kStreamStride dimensions
			//! each).  Variable-length algorithms must use IndependentSampler.
			bool HasFixedDimensionBudget() const { return true; }

			//! Advance to stream's dimension range.
			//! Stream 0 = film/camera, 1 = light subpath, 2 = eye subpath, etc.
			void StartStream( int streamIndex )
			{
				dimension = static_cast<unsigned int>(streamIndex) * kStreamStride;
			}
		};
	}
}

#endif
