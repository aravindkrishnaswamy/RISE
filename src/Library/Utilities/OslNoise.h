//////////////////////////////////////////////////////////////////////
//
//  OslNoise.h - Gradient noise, fractal noise, the Noise Texture node
//    and the White Noise node as the Open Shading Language and Cycles
//    define them.
//
//  These are PORTS, not new noise.  They reproduce the CPU kernels of
//  Blender's Cycles renderer, which follow OSL's definitions, so that a
//  Blender material translated into a RISE expression produces the same
//  pattern Cycles renders:
//
//    gradient noise   intern/cycles/kernel/svm/noise.h
//                     (Perlin's improved noise as adapted in OSL: a
//                     Jenkins lookup3 lattice hash, a quintic fade,
//                     per-dimension output scales, and a wrap of every
//                     coordinate at 100000)
//    fractal noise    intern/cycles/kernel/svm/fractal_noise.h
//                     (fBM, multifractal, hybrid multifractal, ridged
//                     multifractal, heterogeneous terrain; fractional
//                     detail adds a final partial octave, in all but
//                     ridged)
//    Noise Texture    intern/cycles/kernel/svm/noisetex.h
//                     (distortion and the Color output's two offset
//                     evaluations)
//    hashes           intern/cycles/util/hash.h (lookup3)
//    White Noise      intern/cycles/kernel/svm/white_noise.h
//    helpers          intern/cycles/util/math_base.h (mix, floorfrac,
//                     saturatef)
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17).  The
//  golden values, tests/data/osl_noise_golden.inc, were dumped from those
//  sources compiled three ways -- the SIMD path (sse2neon on arm64) and
//  the scalar path as Blender builds them, and the scalar path as strict
//  IEEE -- by tools/osl_golden/dump_noise_golden.sh; tests/OslNoiseTest.cpp
//  gates on the strict column.
//
//  PRECISION.  The kernels evaluate in 32-bit float, in Cycles' order of
//  operations, and take and return RISE's double `Scalar` only at the
//  interface (a coordinate is rounded to float on the way in, as Cycles
//  stores it).  Float is not a shortcut here, it is the specification:
//  the Color and distortion offsets move a coordinate to about |p| + 150,
//  where a float ulp is ~1.5e-5, and octave i multiplies that rounding by
//  lacunarity^i, so where Cycles rounds is part of its pattern.  The
//  arithmetic is STRICT IEEE single precision in the order of Cycles'
//  scalar path ("standard implementation"): no reassociation, no
//  reciprocal substitution, no fused multiply-add.  Every build compiles
//  OslNoise.cpp that way (see the enforcement comment in that file), so the
//  pattern is the same bits on every platform by construction (it has been
//  built and run on macOS arm64 only; the Linux/GCC, VS2022 and Android
//  settings are untested), and it equals a strict-IEEE build of Cycles'
//  own sources EXACTLY on every row of tests/OslNoiseTest.cpp's table.
//
//  AGAINST CYCLES AS BLENDER BUILDS IT.  Cycles itself is not one
//  arithmetic: its clang builds fuse multiply-adds where the hardware has
//  FMA, and its SIMD path, which shipping builds run, groups the
//  interpolation differently.  The golden table carries both, rebuilt
//  from Cycles' sources with Blender's math flags (not a shipped binary).
//  THIS IS THE ONE PLACE THE MEASURED FIGURES ARE QUOTED; everything else
//  points here.
//
//  On identical float inputs, against the SIMD build, relative to
//  max(1, |value|): one octave of noise agrees to 6.4e-7 (every |p| below
//  5e8, see below), and the fractals and the Noise Texture node's Fac
//  without distortion agree to float rounding (below 1e-5) at every
//  roughness tested.  Where they part is wherever float rounding gets
//  AMPLIFIED: octave i scales a coordinate by lacunarity^i and weights it
//  by roughness^i, so an error in a coordinate grows roughly as
//  (roughness * lacunarity)^i through the highest octaves.  Two sources
//  feed it --
//    * outputs Cycles evaluates at SHIFTED coordinates, the Color G and B
//      channels (offset by 100..200, where a float ulp is ~1.5e-5) and
//      anything distorted, where Cycles' own rounding is larger; and
//    * in a render, every output: RISE rounds its double coordinate
//      (position times scale) to float where Cycles computes it in float,
//      and Cycles' own float hit point and transform round differently,
//      so the two can start one or more float ulps apart.
//  Measured by Test 1e of tests/OslNoiseTest.cpp, which prints this table:
//  3D, coordinates in [-4, 4], detail 15 (the Noise node's maximum),
//  lacunarity 2, offset 0.8, gain 1.5, normalize on; 8 points per type
//  at distortion 0 and 8 at distortion 0.5, the worst over all five types.
//  "Unshifted Fac" is Fac at distortion 0; "shifted outputs" are Color G/B
//  at distortion 0 and every output at distortion 0.5.
//  The figures are sample maxima, not bounds; denser sampling finds
//  somewhat larger ones.  Fewer octaves or a smaller lacunarity shrink
//  every figure.  The unshifted-Fac one-ulp column grows roughly in
//  proportion to |p|, because a float ulp does; the shifted one grows more
//  slowly until |p| nears the 100..200 offsets.  Both are larger than
//  shown at larger coordinates, and a render's coordinates can differ by
//  more than one ulp:
//
//    roughness   strict vs SIMD,    one-ulp input change moves:
//                shifted outputs    unshifted Fac    shifted outputs
//      0.5         1.6e-4             3.0e-6           4.6e-5
//      0.6         8.6e-4             7.7e-6           7.4e-4
//      0.7         4.4e-3             5.5e-5           1.1e-3
//      0.8         4.1e-2             4.2e-4           3.7e-2
//      0.9         0.25               2.7e-3           0.097
//      1.0         0.65               8.5e-3           1.1
//      1.1         7.3                0.32             0.3
//
//  The strict-vs-SIMD column equals, to two figures, the gap between a
//  strict build and a build of the SAME Cycles source with Blender's math
//  flags (which fuse multiply-adds and allow reciprocal substitution;
//  computed from the golden table, not printed), so Blender builds
//  whose arithmetic differs (FMA hardware or not) are expected to
//  disagree with each other by as much (inferred; no non-FMA build has
//  been run).  Where these
//  figures are large -- high roughness with high detail -- no port can
//  match Cycles pixel for pixel, and agreement is statistical.
//
//  One more difference is structural: for 3D and 4D the SIMD path wraps
//  a coordinate as a - trunc(a/1e5)*1e5 in float, which can stop matching
//  fmodf once |p| exceeds about 5.4e8 (where that product stops being
//  exact; the first disagreement in the table is at 1.1e9).  This port
//  uses fmodf, as the scalar path does.  At such coordinates a float's
//  spacing exceeds the noise's lattice cell, so neither result is a
//  meaningful pattern.
//
//  NON-FINITE INPUT.  Cycles does not guard its gradient noise: a NaN or
//  infinite coordinate reaches an int conversion, which is undefined
//  behaviour in C++.  SNoise, Fractal and NoiseTexture return 0 (and
//  NoiseTexture a zero Color) when any coordinate they read is not
//  finite, the same honest neutral the
//  expression VM's other builtins use; a double coordinate beyond float
//  range rounds to infinity and so also gives 0.  Inside a fractal, an
//  octave whose scaled coordinate overflows (a huge lacunarity) reads
//  snoise as 0 -- SNoise checks before its wrap, since fmod of an
//  infinity is NaN -- and the fractal carries on.  WhiteNoise is the
//  exception: it converts nothing to an int, so like Cycles it hashes a
//  non-finite coordinate's bit pattern (see its own comment).
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

		//! Hash the 32-bit float BIT PATTERN of the argument(s), as Cycles'
		//! hash_float_to_float / hash_float2_to_float.  Only exact on inputs
		//! that are exact in float -- see the White Noise note in
		//! docs/BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md §5.2(b).
		float HashFloatToFloat( float k );
		float HashFloat2ToFloat( float kx, float ky );
		float HashFloat3ToFloat( float kx, float ky, float kz );
		float HashFloat4ToFloat( float kx, float ky, float kz, float kw );

		//////////////////////////////////////////////////////////////
		// The White Noise node (kernel/svm/white_noise.h)
		//////////////////////////////////////////////////////////////

		//! The node's Value output, and -- when `color` is non-null -- its
		//! Color output, for `dims` coordinates (1..4) read as the noise
		//! builtins read them.  Each is a lookup3 hash of the coordinates'
		//! 32-bit float BIT PATTERNS (hash_floatN_to_float and
		//! hash_floatN_to_float3), so the result is integer arithmetic and
		//! equals Cycles exactly when the float coordinates are the same.
		//! But it is not a continuous function of them: a coordinate that
		//! differs by one float ulp hashes to an unrelated value.  RISE
		//! rounds its double coordinate to float on the way in, which is
		//! exact when the value is exact in float (an object's random
		//! number, an integer cell id) and otherwise matches Cycles in
		//! distribution, not per pixel -- docs/BLENDER_PROCEDURAL_
		//! TRANSLATION_DESIGN.md §5.2(b).  As in Cycles, -0 and +0 are
		//! different inputs, and an infinity or NaN is hashed like any other
		//! bit pattern (no guard is needed: nothing converts it to an int).
		//! `dims` outside 1..4 gives Cycles' fallback, Value 0 and Color
		//! (1, 0, 1).  One render-time difference falls under the
		//! in-distribution caveat: Cycles enables flush-to-zero for Embree
		//! on whichever thread constructs the Embree BVH (a TBB worker that
		//! may later shade), and its CUDA/OptiX kernels are compiled with
		//! --use_fast_math, which flushes (the other GPU backends use
		//! fast-math modes that may), so a subnormal coordinate Cycles
		//! computes can arrive as 0 where RISE hashes it.  (RISE's
		//! expression memo compares double key fields bit for bit, so a
		//! memoised body keeps -0 and +0 apart.)
		void WhiteNoise( int dims, const Scalar* p, Scalar& value, Scalar* color );

		//////////////////////////////////////////////////////////////
		// Gradient noise (kernel/svm/noise.h)
		//////////////////////////////////////////////////////////////

		//! Signed noise, snoise_1d .. snoise_4d: every coordinate wrapped
		//! by fmod(p, 100000) (plus 0.5 where |p| >= 1e6), the lattice
		//! noise, then OSL's per-dimension output scale.  Roughly [-1, 1].
		//! `dims` outside 1..4, or a coordinate that is not finite, gives 0.
		Scalar SNoise( int dims, const Scalar* p );

		//////////////////////////////////////////////////////////////
		// Fractal noise (kernel/svm/fractal_noise.h)
		//////////////////////////////////////////////////////////////

		//! Cycles' NodeNoiseType numbering, kept so the exporter and the
		//! kernels share one table.
		enum FractalType
		{
			eMultifractal         = 0,
			eFbm                  = 1,
			eHybridMultifractal   = 2,
			eRidgedMultifractal   = 3,
			eHeteroTerrain        = 4
		};
		static const int kFractalTypeCount = 5;

		//! The largest detail any fractal here evaluates: the Noise
		//! Texture node's own clamp.  Cycles' fractal kernels have no bound
		//! of their own (a linked detail of 1e9 would loop 1e9 times), so
		//! Fractal clamps a larger detail, +infinity included, to this one.
		//! That departs from Cycles only for a computed detail above 15
		//! (the Noise node clamps to 15 itself); Fractal's other departures
		//! are the NaN and non-finite-coordinate guards above.
		static const int kMaxDetail = 15;

		//! The smallest detail Fractal passes on, -2^24.  Every float at or
		//! below it is an integer, so clamping there keeps Cycles' result
		//! (no octave inside the loop, and a zero remainder) for every
		//! detail at which Cycles' own int conversion is defined; the bound
		//! exists only so ours always is.
		static const int kMinDetail = -16777216;

		//! noise_select: the fractal of `type` over `dims` coordinates.
		//! detail is clamped to [kMinDetail, kMaxDetail] (NaN -> 0); within
		//! that range the octave count and partial octave are Cycles', a
		//! negative detail included (unlike NoiseTexture, which clamps it to
		//! 0 as the node does).  `normalize` is read by fBM only.  An
		//! unknown type gives 0.
		Scalar Fractal( int dims, const Scalar* p, int type,
			Scalar detail, Scalar roughness, Scalar lacunarity,
			Scalar offset, Scalar gain, bool normalize );

		//////////////////////////////////////////////////////////////
		// The Noise Texture node (kernel/svm/noisetex.h)
		//////////////////////////////////////////////////////////////

		//! The node after its Scale multiply (the caller scales `p`):
		//! detail clamped to [0, 15], roughness to >= 0, the distortion
		//! warp, then the fractal.  Writes the Fac output, and -- when
		//! `color` is non-null -- the Color output, whose first channel is
		//! Fac and whose other two are the fractal at two fixed offsets.
		void NoiseTexture( int dims, const Scalar* p, int type,
			Scalar detail, Scalar roughness, Scalar lacunarity,
			Scalar offset, Scalar gain, Scalar distortion, bool normalize,
			Scalar& fac, Scalar* color );
	}
}

#endif
