//////////////////////////////////////////////////////////////////////
//
//  OslNoise.h - Ports of Blender Cycles' OSL-compatible gradient noise,
//    fractal noise, Noise Texture node, and White Noise node.
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17):
//    gradient noise   intern/cycles/kernel/svm/noise.h (lookup3 hash,
//                     quintic fade, per-dim scales, 100000 wrap)
//    fractal noise    intern/cycles/kernel/svm/fractal_noise.h
//    Noise Texture    intern/cycles/kernel/svm/noisetex.h
//    White Noise      intern/cycles/kernel/svm/white_noise.h
//    hashes/helpers   intern/cycles/util/{hash.h,math_base.h}
//  Verified against tests/data/osl_noise_golden.inc by OslNoiseTest.
//
//  PRECISION.  Evaluates in strict IEEE 32-bit float in Cycles' scalar
//  order of operations (no FMA, reassociation, or reciprocal math;
//  OslNoise.cpp is compiled with -fno-fast-math -ffp-contract=off or
//  /fp:strict), converting double `Scalar` only at the interface.
//  Float rounding is part of Cycles' pattern (e.g. Color/distortion
//  offsets + octave lacunarity^i); at high detail and high roughness,
//  strict IEEE and FMA/SIMD Cycles builds agree statistically rather
//  than bit-for-bit.  Uses fmodf wrap like Cycles' scalar path (SIMD
//  differs above |p| ~ 5.4e8).
//
//  DEPARTURES / NON-FINITE INPUTS.  SNoise, Fractal, and NoiseTexture
//  return 0 (and zero Color) on non-finite coordinates (checked before
//  fmodf) instead of invoking UB on float->int casts; overflowing
//  fractal octaves read snoise as 0.  Fractal clamps detail to
//  [kMinDetail, kMaxDetail] (NaN -> 0).  WhiteNoise hashes raw float
//  bit patterns without int conversion, matching Cycles.
//
//  LICENCES.  RISE's own licence is BSD-style (LICENSE.TXT).  The ported
//  code carries its sources' licences, reproduced here as they require:
//
//    noise.h -- BSD-3-Clause, adapted by Blender from Open Shading Language:
//      SPDX-FileCopyrightText: 2009-2010 Sony Pictures Imageworks Inc., et al.
//                              All Rights Reserved.
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: BSD-3-Clause
//      The full licence text is licenses/BSD-3-Clause.txt.
//
//    fractal_noise.h, noisetex.h, white_noise.h, hash.h, math_base.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    The lookup3 hash inside hash.h is Bob Jenkins's public-domain code
//    (http://burtleburtle.net/bob/c/lookup3.c).
//
//    MODIFIED FOR RISE: rewritten as standalone functions, the four
//    per-dimension overloads of each fractal merged into one, and guards
//    added for non-finite coordinates (gradient noise) and runaway detail.
//    licenses/Cycles-NOTICE.txt summarises these notices for binary
//    distributions.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#ifndef OSL_NOISE_
#define OSL_NOISE_

#include <cstdint>
#include "Math3D/Math3D.h"	// Scalar

namespace RISE
{
	namespace OslNoise
	{
		//////////////////////////////////////////////////////////////
		// Hashes (util/hash.h).  Exact 32-bit integer arithmetic, so
		// these match Cycles bit for bit on every input.
		//////////////////////////////////////////////////////////////

		uint32_t HashUint( uint32_t kx );
		uint32_t HashUint2( uint32_t kx, uint32_t ky );
		uint32_t HashUint3( uint32_t kx, uint32_t ky, uint32_t kz );
		uint32_t HashUint4( uint32_t kx, uint32_t ky, uint32_t kz, uint32_t kw );

		//! [0, 2^32-1] -> [0, 1] in float, as Cycles' uint_to_float_incl.
		float UintToFloatIncl( uint32_t n );

		//! Hash 32-bit float bit patterns (Cycles' hash_floatN_to_float).
		//! Exact only on inputs representable identically in float.
		float HashFloatToFloat( float k );
		float HashFloat2ToFloat( float kx, float ky );
		float HashFloat3ToFloat( float kx, float ky, float kz );
		float HashFloat4ToFloat( float kx, float ky, float kz, float kw );

		//////////////////////////////////////////////////////////////
		// The White Noise node (kernel/svm/white_noise.h)
		//////////////////////////////////////////////////////////////

		//! White Noise Value and optional Color for `dims` in 1..4.
		//! Hashes 32-bit float bit patterns (-0 != +0; non-finite values hash
		//! as raw bits; discontinuous across 1-ULP changes). `dims` outside
		//! 1..4 returns Cycles' fallback: Value 0, Color (1, 0, 1).
		void WhiteNoise( int dims, const Scalar* p, Scalar& value, Scalar* color );

		//////////////////////////////////////////////////////////////
		// Gradient noise (kernel/svm/noise.h)
		//////////////////////////////////////////////////////////////

		//! Signed noise (snoise_1d..4d): fmod(p, 100000) wrap (+0.5 for
		//! |p| >= 1e6), lattice noise, and OSL's per-dim scale (~[-1, 1]).
		//! Returns 0 for `dims` outside 1..4 or any non-finite coordinate.
		Scalar SNoise( int dims, const Scalar* p );

		//////////////////////////////////////////////////////////////
		// Fractal noise (kernel/svm/fractal_noise.h)
		//////////////////////////////////////////////////////////////

		//! Cycles' NodeNoiseType numbering.
		enum FractalType
		{
			eMultifractal         = 0,
			eFbm                  = 1,
			eHybridMultifractal   = 2,
			eRidgedMultifractal   = 3,
			eHeteroTerrain        = 4
		};
		static const int kFractalTypeCount = 5;

		//! Maximum detail evaluated by Fractal (matches Noise Texture's clamp
		//! and prevents unbounded loops on linked detail / +inf).
		static const int kMaxDetail = 15;

		//! Minimum detail passed to Fractal (-2^24), keeping int conversion
		//! defined while preserving Cycles' zero-octave/zero-remainder behavior.
		static const int kMinDetail = -16777216;

		//! Cycles' noise_select over `dims` in 1..4. Clamps detail to
		//! [kMinDetail, kMaxDetail] (NaN -> 0), preserving negative detail as in
		//! Cycles. `normalize` applies to fBM only; unknown type returns 0.
		Scalar Fractal( int dims, const Scalar* p, int type,
			Scalar detail, Scalar roughness, Scalar lacunarity,
			Scalar offset, Scalar gain, bool normalize );

		//////////////////////////////////////////////////////////////
		// The Noise Texture node (kernel/svm/noisetex.h)
		//////////////////////////////////////////////////////////////

		//! Noise Texture node (caller applies Scale): clamps detail to [0, 15]
		//! and roughness to >= 0, applies distortion warp, evaluates Fac, and
		//! (when `color` is non-null) evaluates G/B at fixed random offsets.
		void NoiseTexture( int dims, const Scalar* p, int type,
			Scalar detail, Scalar roughness, Scalar lacunarity,
			Scalar offset, Scalar gain, Scalar distortion, bool normalize,
			Scalar& fac, Scalar* color );
	}
}

#endif
