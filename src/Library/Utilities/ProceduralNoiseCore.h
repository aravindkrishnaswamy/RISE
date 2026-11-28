//////////////////////////////////////////////////////////////////////
//
//  ProceduralNoiseCore.h - Allocation-free noise primitives shared
//  between Perlin3DPainter, Worley3DPainter, and the expression VM.
//
//  PerlinOctave3D matches SmoothedNoise3D + InterpolatedNoise3D (with
//  RealLinearInterpolator) bit-for-bit, and WorleySample3D matches
//  WorleyNoise3D::Evaluate bit-for-bit, without heap-allocated
//  IFunction3D objects (verified by TestNoiseFactoringParity).
//  Fbm3D/Turbulence3D/Ridged3D compose PerlinOctave3D with runtime
//  lacunarity and optional Nyquist filter-width fading.
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

			//! One trilinearly-interpolated octave, bit-identical to
			//! InterpolatedNoise3D::Evaluate with RealLinearInterpolator; [-1, 1].
			Scalar PerlinOctave3D( Scalar x, Scalar y, Scalar z );

			//! Unnormalized layered Perlin sum (amp = gain^i, freq = lacunarity^i),
			//! bounded to [-sum(gain^i), sum(gain^i)]. Clamps octaves to
			//! [1, kMaxOctaves]; non-finite gain/lacunarity -> 0. Optional `fw`
			//! (in x,y,z units) applies Nyquist octave fade (exact 1.0 when fw==0).
			Scalar Fbm3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw = Scalar(0) );

			//! Normalized layered sum of |PerlinOctave3D| in [0, 1]. `fw`: see Fbm3D.
			Scalar Turbulence3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw = Scalar(0) );

			//! Normalized layered sum of (1-|PerlinOctave3D|)^2 in [0, 1]. `fw`: see Fbm3D.
			Scalar Ridged3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw = Scalar(0) );

			//! Nyquist octave fade weight: 1 - smoothstep(0.2, 0.6, fw) for
			//! per-octave filter width `fw` (1 for fw <= 0.2, 0 for fw >= 0.6).
			//! Exposed publicly for unit-test pinning.
			Scalar OctaveFadeWeight( Scalar fw );

			//! Octave cap enforced at compile time and runtime.
			static const int kMaxOctaves = 10;

			//! UB-safe floor-to-int: clamps in double space to int's representable
			//! range before truncating (saturating on overflow; non-finite -> 0).
			//! Shared by all noise/painter sites taking unbounded Scalar coords.
			int SafeFloorToInt( Scalar v );

			//////////////////////////////////////////////////////////
			//  Worley (cellular) noise (Worley3DPainter's engine)
			//////////////////////////////////////////////////////////

			enum WorleyMetric { eMetricEuclidean = 0, eMetricManhattan = 1, eMetricChebyshev = 2 };
			enum WorleyMode   { eModeF1 = 0, eModeF2 = 1, eModeF2MinusF1 = 2 };

			//! Bit-identical to WorleyNoise3D::HashCell (Noise/WorleyNoise.cpp).
			unsigned int WorleyHashCell( int ix, int iy, int iz );

			//! 3x3x3 jittered-grid search matching WorleyNoise3D::Evaluate.
			//! Outputs raw F1/F2 distances and the F1 feature point's cell coords.
			void WorleySample3D( Scalar x, Scalar y, Scalar z, Scalar jitter, WorleyMetric metric,
								  Scalar& outF1, Scalar& outF2, int& outF1CellX, int& outF1CellY, int& outF1CellZ );

			//! Bit-identical normalization + clamp to WorleyNoise3D::Evaluate (~[0,1]).
			Scalar WorleyNormalize( Scalar raw, WorleyMetric metric, WorleyMode mode );

			//! Deterministic positive integer-valued cell id (mod 1000003) safe
			//! to pass into CellHash as a per-cell attribute seed.
			Scalar WorleyIdOf( int cellX, int cellY, int cellZ );

			//////////////////////////////////////////////////////////
			//  Generic scalar hash (expression-VM `cellhash` builtin)
			//////////////////////////////////////////////////////////

			//! floor(x) -> Noise1D mix -> [0, 1). Deterministic per-cell hash.
			Scalar CellHash( Scalar x );
		}
	}
}

#endif
