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
//    = 8192 = 256 streams' worth), sized so that every stream a
//    SHIPPED scene reaches has dimensions of its own; past it, Get1D
//    draws are re-indexed rather than aliased (they decorrelate, but
//    stop being a joint net) and Get2D draws are unaffected, since
//    they are padded and keyed by the raw dimension.
//    `SobolDimensionBudgetTest` Test G recomputes the shipped bound
//    from the scene files.
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
#include <cstdio>
// ==== DL283 SCRATCH INSTRUMENTATION (reverted before merge) ====
namespace DL283ScratchSobol {
	inline std::atomic<unsigned long long>* Hist() { static std::atomic<unsigned long long> h[2100]; return h; }
	struct Dumper { ~Dumper() { unsigned long long t = 0; for( int i = 0; i < 2100; i++ ) t += Hist()[i].load();
		if( !t ) return; std::fprintf( stderr, "DL283STREAM total=%llu\n", t );
		for( int i = 0; i < 2100; i++ ) { const unsigned long long c = Hist()[i].load(); if( c ) std::fprintf( stderr, "DL283H stream %d %llu\n", i, c ); } } };
	inline Dumper& D() { static Dumper d; return d; }
	// Per-render scramble salt so repeated renders are INDEPENDENT QMC
	// randomizations (otherwise every render reuses identical Sobol points).
	inline uint32_t& Salt() { static uint32_t s = 0; return s; }
}

namespace RISE
{
	namespace Implementation
	{
		class SobolSampler :
			public ISampler
		{
		protected:
			uint32_t sampleIndex;		// Which sample in the sequence
			uint32_t seed;				// Per-pixel base scramble seed
			unsigned int dimension;		// Current dimension counter

			// Dimensions are partitioned into fixed-size phases so
			// that each bounce always starts at the same dimension
			// offset regardless of how many dimensions previous
			// bounces consumed (material-dependent).  This preserves
			// cross-pixel Sobol stratification.
			//
			// Encoding: phase = streamBase + bounceIndex
			//   Light source sampling: phase 0
			//   Light bounces 0..14:   phases 1..15
			//   Eye bounces 0..14:     phases 16..30
			//   SMS:                   phases 31..46
			//
		public:
			// kStreamStride must be >= max dimensions consumed by
			// any single phase (BioSpecSkinSPF uses ~20 + lobe
			// selection + BSSRDF = ~25 max).  Public so a stream
			// layout that must hold an open-ended consumer can
			// static_assert its width (BDPTUtilities::
			// MediumDistanceStream, DL-283).
			static const unsigned int kStreamStride = 32;

			virtual ~SobolSampler(){ RecordStream(); };
			int curStream = -1;
			void RecordStream() {
				if( curStream >= 1 && curStream < 1040 ) {
					(void)DL283ScratchSobol::D();
					const unsigned int used = dimension - unsigned(curStream) * kStreamStride;
					DL283ScratchSobol::Hist()[ used < 2099 ? used : 2099 ].fetch_add( 1, std::memory_order_relaxed );
				}
			}

		public:
			SobolSampler(
				uint32_t sampleIndex_,
				uint32_t seed_
				) :
				sampleIndex( sampleIndex_ ),
				seed( seed_ ^ DL283ScratchSobol::Salt() ),
				dimension( 0 )
			{
			}

			//! Returns a single Owen-scrambled Sobol sample in [0,1)
			Scalar Get1D()
			{
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
				RecordStream();
				curStream = streamIndex;
				dimension = static_cast<unsigned int>(streamIndex) * kStreamStride;
			}
		};
	}
}

#endif
