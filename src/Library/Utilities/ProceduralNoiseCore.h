//////////////////////////////////////////////////////////////////////
//
//  ProceduralNoiseCore.h - Free-function, allocation-free noise
//  primitives shared between the 3D noise painters (Perlin3DPainter,
//  Worley3DPainter) and the texture-expression VM (ExpressionEval.h).
//
//  These are NOT new noise algorithms.  PerlinOctave3D below matches
//  RISE::Implementation::SmoothedNoise3D + InterpolatedNoise3D bit-for-
//  bit when the latter is used with a RealLinearInterpolator (the ONLY
//  interpolator Perlin3DPainter and Worley3DPainter ever construct with,
//  verified against every call site in the tree).  WorleySample3D
//  matches WorleyNoise3D::Evaluate's 3x3x3 jittered-grid search and hash
//  bit-for-bit.  The point of this file is to make that math callable
//  without a heap-allocated, Reference-counted IFunction3D object, so
//  it can run inside the allocation-free expression VM as well as
//  behind the two painters -- see TestNoiseFactoringParity() in
//  tests/TextureExpressionVMTest.cpp for the pre/post-factoring parity
//  proof.
//
//  Layered noise (fbm/turbulence/ridged) is a NEW composition of the
//  single-octave primitive with a RUNTIME-configurable lacunarity (the
//  existing Noise/PerlinNoise.h::frequency_lut bakes lacunarity=2 at
//  compile time, which the expression language's `fbm(p,octaves,gain,
//  lacunarity)` builtin cannot use) -- it is new call-site composition,
//  not a new lattice/hash algorithm.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PROCEDURAL_NOISE_CORE_
#define PROCEDURAL_NOISE_CORE_

#include "Math3D/Math3D.h"		// Scalar, Vector3

namespace RISE
{
	namespace Implementation
	{
		namespace NoiseCore
		{
			//////////////////////////////////////////////////////////
			//  Perlin-style lattice noise (Perlin3DPainter's engine)
			//////////////////////////////////////////////////////////

			//! Single lattice-point hash, bit-identical to Noise3D::Evaluate
			//! (Noise/Noise.h) for integer inputs.  Range: (-1, 1).
			Scalar LatticeHash3D( int x, int y, int z );

			//! 26-neighbor weighted smoothing of LatticeHash3D, bit-identical
			//! to SmoothedNoise3D::Evaluate (Noise/SmoothedNoise.h).
			Scalar SmoothedLatticeNoise3D( int x, int y, int z );

			//! One octave of trilinearly-interpolated lattice noise, bit-
			//! identical to InterpolatedNoise3D::Evaluate (Noise/
			//! InterpolatedNoise.h) when built with a linear interpolator.
			//! A convex combination of SmoothedLatticeNoise3D samples, so
			//! the result is bounded to exactly [-1, 1].
			Scalar PerlinOctave3D( Scalar x, Scalar y, Scalar z );

			//! Layered sum: amplitude_i = gain^i, frequency_i = lacunarity^i,
			//! i = 0..octaves-1.  Raw signed sum (not normalized), matching
			//! the convention Perlin3DPainter/PerlinNoise3D already use for
			//! persistence-weighted octave sums.  Bounded to
			//! [-S, S] where S = sum(gain^i).  octaves is clamped to
			//! [1, kMaxOctaves]; non-finite gain/lacunarity are treated as 0.
			Scalar Fbm3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity );

			//! Layered sum of |PerlinOctave3D| (sharp creases where the
			//! underlying noise crosses zero), normalized by the amplitude
			//! sum so the result lies in [0, 1].
			Scalar Turbulence3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity );

			//! Layered sum of (1-|PerlinOctave3D|)^2 (Musgrave-style ridge,
			//! without the previous-octave weighting term), normalized by
			//! the amplitude sum so the result lies in [0, 1].
			Scalar Ridged3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity );

			//! Compile-time-checkable octave cap; also the runtime clamp for
			//! a non-literal octave count so a hostile/huge value can never
			//! blow the eval budget.
			static const int kMaxOctaves = 10;

			//! floor(x) then a bare (int) cast is UB when the floored value
			//! is outside int's representable range -- e.g. cellhash(1e20)
			//! from the expression VM (UBSan-confirmed), or an unbounded
			//! painter parameter (tile_scale, cell_scale, ...) multiplied
			//! against an ordinary UV.  Clamp in Scalar (double) space,
			//! BEFORE the cast, to the widest window every int can hold; a
			//! value outside it saturates to the boundary instead of
			//! hitting the UB / platform-divergent truncation a raw cast
			//! would give.  Non-finite input is treated as 0.  This is the
			//! ONE fix point for every floor-then-truncate site in the tree
			//! that can receive an unbounded, user-authored Scalar --
			//! originally added (as an anonymous-namespace local) for
			//! CellHash / WorleySample3D / PerlinOctave3D in
			//! ProceduralNoiseCore.cpp; promoted to the public API here so
			//! new callers (StochasticTilePainter, ScatterPainter, ...)
			//! route through the same guarded cast instead of each growing
			//! its own bare (int)floor and re-introducing the UB.
			int SafeFloorToInt( Scalar v );

			//////////////////////////////////////////////////////////
			//  Worley (cellular) noise (Worley3DPainter's engine)
			//////////////////////////////////////////////////////////

			enum WorleyMetric { eMetricEuclidean = 0, eMetricManhattan = 1, eMetricChebyshev = 2 };
			enum WorleyMode   { eModeF1 = 0, eModeF2 = 1, eModeF2MinusF1 = 2 };

			//! Bit-identical to WorleyNoise3D::HashCell (Noise/WorleyNoise.cpp).
			unsigned int WorleyHashCell( int ix, int iy, int iz );

			//! 3x3x3 jittered-grid search, bit-identical to WorleyNoise3D::
			//! Evaluate's inner loop.  Outputs raw (unnormalized) F1/F2
			//! distances under the given metric, plus the integer cell
			//! coordinate the F1 feature point belongs to (for WorleyIdOf).
			void WorleySample3D( Scalar x, Scalar y, Scalar z, Scalar jitter, WorleyMetric metric,
								  Scalar& outF1, Scalar& outF2, int& outF1CellX, int& outF1CellY, int& outF1CellZ );

			//! Bit-identical normalization table + clamp to WorleyNoise3D::
			//! Evaluate's post-processing (maps raw distance to ~[0,1]).
			Scalar WorleyNormalize( Scalar raw, WorleyMetric metric, WorleyMode mode );

			//! A small, deterministic, positive integer-valued id for a
			//! Worley cell -- safe to feed into CellHash (below) without
			//! overflowing the int truncation CellHash performs.  Not
			//! collision-free (mod 1000003), which is fine for a per-cell
			//! "random attribute" seed.
			Scalar WorleyIdOf( int cellX, int cellY, int cellZ );

			//////////////////////////////////////////////////////////
			//  Generic scalar hash (expression-VM `cellhash` builtin)
			//////////////////////////////////////////////////////////

			//! floor(x) -> one hash round (same mix formula as Noise1D)
			//! -> [0, 1).  Deterministic, not a noise field (no smoothing/
			//! interpolation) -- meant for turning a cell id / index into a
			//! per-cell pseudo-random scalar attribute.
			Scalar CellHash( Scalar x );
		}
	}
}

#endif
