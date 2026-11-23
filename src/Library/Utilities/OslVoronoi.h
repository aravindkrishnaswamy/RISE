//////////////////////////////////////////////////////////////////////
//
//  OslVoronoi.h - The Voronoi Texture node as Cycles defines it.
//
//  A PORT, not new noise.  It reproduces the CPU kernel of Blender's
//  Cycles renderer, intern/cycles/kernel/svm/voronoi.h, so that a Blender
//  material translated into a RISE expression produces the cell pattern
//  Cycles renders: the F1, F2, Smooth F1, Distance to Edge and N-Sphere
//  Radius features; the Euclidean, Manhattan, Chebychev and Minkowski
//  metrics; 1D to 4D; and the node's fractal parameters (Detail,
//  Roughness, Lacunarity, Normalize), as svm_node_tex_voronoi applies
//  them.  The cell hashes are util/hash.h's: PCG (hash_pcg2d_i ..
//  hash_pcg4d_i, as of Blender 5.0) in 2D to 4D, and lookup3 on the
//  float bits in 1D (OslNoise.h's hashes).
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17), as
//  OslNoise.h is.  The golden values, tests/data/osl_voronoi_golden.inc,
//  were dumped by tools/osl_golden/dump_voronoi_golden.sh, which runs
//  Cycles' own svm_node_tex_voronoi compiled three ways (the SIMD path
//  as Blender's arm64 build compiles it, the scalar path with Blender's
//  math flags, and the scalar path as strict IEEE); tests/
//  OslVoronoiTest.cpp gates on the strict column.
//
//  PRECISION.  As OslNoise.h: 32-bit float in the order of Cycles' scalar
//  path ("standard implementation"), strict IEEE (no reassociation, no
//  reciprocal substitution, no fused multiply-add; OslVoronoi.cpp is
//  compiled that way by every build), double only at the interface.
//  Where Cycles' scalar path divides a vector by a scalar it multiplies
//  by the reciprocal, and so does the port.  The Minkowski metric calls
//  powf, which C libraries do not round identically: the strict column
//  was dumped with macOS's libm, and RISE uses the platform's, so on
//  another platform a Minkowski distance can differ in its last bit (not
//  measured: built and run so far on macOS arm64 only; a correctly
//  rounded pow already parts from macOS's on one row).  The test
//  therefore gates the 123 rows that call powf bit for bit only on Apple
//  arm64, and to 1e-6 elsewhere.  Every other operation is a correctly
//  rounded IEEE operation.
//
//  The port equals the strict build exactly on all 1092 node rows of the
//  golden table (and on its 72 PCG hash rows).  Against Cycles as Blender
//  builds it (the SIMD and scalar columns: Cycles' sources rebuilt with
//  Blender's math flags, not a shipped binary) the two part in three
//  ways.  Test 1b of
//  tests/OslVoronoiTest.cpp counts each, calling a row a departure when
//  any output differs by more than 1e-5 relative to max(1, |v|); the
//  counts are of this table, not bounds:
//    * Rounding.  Where the builds round differently, every output of a row
//      that keeps the same cells moves by float rounding: at most 2.01e-7 on
//      the table (a Smooth F1 colour), against both builds.  But a rounding
//      difference can also change WHICH feature point is nearest, and then
//      Color and Position jump to the other point's, and N-Sphere Radius can
//      jump to another pair's, while the F1, F2 and Distance to Edge
//      distances stay continuous: 1 row of the table (an F2 colour and
//      position) against each build.  So cell colours match Cycles away from
//      cell boundaries and only in distribution on them.  In a render RISE
//      also rounds its double coordinate to float where Cycles computes it in
//      float (OslNoise.h, PRECISION), which moves boundary pixels the same
//      way.
//    * A 1D coordinate of -0 in an octave that runs (-0 after the Scale
//      multiply, or a later octave's coordinate times a zero or negative
//      Lacunarity: reachable in a render, e.g. W = 0 with a negative
//      Scale).  Blender's clang and GCC builds compile Cycles with
//      -fno-signed-zeros, which lets the compiler fold `cellPosition +
//      0.0f` to cellPosition and hash the bits of -0, where strict IEEE
//      (and the port) hash +0.  (Its MSVC build uses /fp:fast instead,
//      which does not promise signed zeros either; not measured.)  On the
//      table this moves F1 and F2 (cell, colour, distance and W) and
//      N-Sphere Radius: 8 of the 12 such rows depart from each build.
//      Smooth F1, where the compiler did not fold, and Distance to Edge,
//      which hashes the floored coordinate directly, agree.
//    * A cell id beyond int32 (below).  The port saturates, as Cycles'
//      arm64 scalar path does, and its 2D path (an (int) cast) in either
//      build; its SIMD path in 3D and 4D converts with sse2neon's
//      _mm_cvtps_epi32, which on Apple's arm64 targets (FRINT32X) gives
//      INT32_MIN for every out-of-range value, as x86-64 does (a generic
//      armv8-a build of sse2neon saturates, as the port does).  That
//      agrees with the port below the range and parts from it above: 4 of
//      the table's 8 3D/4D rows with a cell id above int32 depart from
//      the SIMD build.
//      Against the scalar build these rows are ordinary rounding rows.
//  Each output is a pure function of its inputs, and RISE's expression
//  memo compares double key fields bit for bit (SameDoubleBits in
//  ExpressionMemo.h), so a memoised body keeps -0 and +0 apart even for
//  1D Distance to Edge (which hashes the floored coordinate's bits and
//  is the one Voronoi feature that distinguishes them; every other
//  feature adds an offset first, and -0 + 0 is +0).
//
//  COST.  One call evaluates ceil(detail) + 1 octaves (one when Detail or
//  Roughness is 0, and always one for N-Sphere Radius), and each octave
//  searches 3^D cells -- 5^D for Smooth F1 (3^D at smoothness 0, where it
//  runs F1), and two searches for N-Sphere Radius (3^D, then 3^D - 1, the
//  centre skipped) and for Distance to Edge in 2D to 4D (1D reads 3 cells
//  once).  Measured by tools/osl_golden/bench_voronoi.cpp (Apple M1 Max, Apple
//  clang 21, -O3 with this file's strict flags, one thread, Euclidean,
//  Roughness 0.5, two runs after a warm-up agreeing to about 4%), in
//  nanoseconds per call:
//
//                   detail 0                     detail 15
//              1D    2D    3D     4D        1D     2D      3D      4D
//    F1        26    53   250    820       240    970    4100   13000
//    F2        28    85   250   1100       420   1600    3800   18000
//    Smooth F1 73   360  1800   9100      1100   5600   29000  146000
//    Edge      16    81   360   1300       130   1300    5700   21000
//    N-Sphere  20    69   400   1400       (no fractal)
//
//  For scale, the 3D Noise Texture node is about 100 ns at detail 2 and
//  530 ns at detail 15 on the same machine, so a default 3D F1 Voronoi
//  (detail 0) costs about two and a half of those.  High detail in 3D and
//  4D runs to tens of microseconds per call, and 4D Smooth F1 at detail 15
//  to about 150.
//
//  WHERE THE PORT DEPARTS FROM CYCLES.  Only where Cycles' result is
//  undefined behaviour or meaningless, and in one case a caller cannot
//  reach:
//    * A coordinate that is not finite after the Scale multiply gives
//      every output 0.  In 2D to 4D Cycles converts the NaN or infinite
//      cell position to an int, which is undefined; in 1D it converts
//      nothing, but the position within the cell is NaN, every
//      comparison fails, and its outputs are meaningless (FLT_MAX, NaN or
//      0 depending on the feature).
//    * Inside the fractal loop, an octave whose scaled coordinate is not
//      finite (a huge, infinite or NaN Lacunarity) ends the loop there, as if
//      Detail had ended before it; the octaves before it are kept.  Cycles
//      evaluates that octave, with the result just described.
//    * A finite cell position beyond the int32 range saturates to
//      INT32_MIN / INT32_MAX, which is what Cycles' arm64 scalar path
//      compiles its (int) conversion to; its SIMD path and an x86-64
//      build give INT32_MIN (above).  Cell ids then add with two's-
//      complement wrap, as Cycles' signed int addition does in practice.
//      The golden table's 11 rows that run such an octave equal the arm64
//      strict build exactly.  At such coordinates a float's spacing
//      exceeds a cell, so the pattern is not meaningful in any build.
//    * `feature` or `metric` outside Cycles' enums, or `dims` outside
//      1..4, gives every output 0.
//      Cycles would run F1 for an unknown feature, and for an unknown
//      metric read every 2D-4D distance as 0 (1D ignores the metric), so
//      that F1 and F2 pick points by loop order alone and Smooth F1
//      blends every point with weights set by that order (Distance to
//      Edge and N-Sphere Radius ignore the metric).  `dims` outside 1..4
//      also gives 0, where Cycles asserts.  The builtins check all three
//      as integer literals, so only a direct C++ caller can pass one.
//  Everything else is Cycles' arithmetic, including its results that are
//  not finite from finite inputs (a zero Lacunarity sends Position
//  through 1/0; a zero Minkowski exponent raises to 1/0); the expression
//  builtins read such an output as 0.  A zero Scale is finite: the node's
//  safe_divide gives Position 0.
//
//  LICENCES.  RISE's own licence is BSD-style (LICENSE.TXT).  The ported
//  code carries its sources' licences, reproduced here as they require:
//
//    voronoi.h, hash.h, math_base.h, math_float2.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    math_float3.h, math_float4.h (the float3 and float4 vector helpers:
//    dot, len_squared, distance, reductions, fabs, power, normalize, mix,
//    safe_divide) --
//      SPDX-FileCopyrightText: 2011-2013 Intel Corporation
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    all of them --
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    The Smooth F1 and Distance to Edge features inside voronoi.h --
//      SPDX-License-Identifier: MIT
//      Original code is copyright (c) 2013 Inigo Quilez.
//      The licence text is licenses/MIT-Inigo-Quilez.txt.
//
//    The PCG hashes inside hash.h are from Jarzynski and Olano, "Hash
//    Functions for GPU Rendering", JCGT 9(3), 2020, modified by Blender
//    to use signed integers.
//
//    MODIFIED FOR RISE: rewritten as standalone functions, the 2D, 3D and
//    4D copies of each feature merged into one template (1D keeps its own
//    functions, as in Cycles), the node entry point reduced to a function
//    of its inputs, and the departures listed above.
//    licenses/Cycles-NOTICE.txt summarises these notices for binary
//    distributions.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#ifndef OSL_VORONOI_
#define OSL_VORONOI_

#include <cstdint>
#include "Math3D/Math3D.h"	// Scalar

namespace RISE
{
	namespace OslVoronoi
	{
		//! Cycles' NodeVoronoiFeature numbering.
		enum Feature
		{
			eF1               = 0,
			eF2               = 1,
			eSmoothF1         = 2,
			eDistanceToEdge   = 3,
			eNSphereRadius    = 4
		};
		static const int kFeatureCount = 5;

		//! Cycles' NodeVoronoiDistanceMetric numbering.
		enum Metric
		{
			eEuclidean        = 0,
			eManhattan        = 1,
			eChebychev        = 2,
			eMinkowski        = 3
		};
		static const int kMetricCount = 4;

		//! The node's five outputs.  Cycles writes 0 to the outputs a
		//! feature does not produce: Distance to Edge writes only
		//! `distance`, N-Sphere Radius only `radius`, and the others every
		//! output but `radius`.  `w` is the fourth component of Cycles'
		//! position, which the node exposes only in 1D and 4D; in 2D it is
		//! 0, and in 3D it is whatever Cycles computes from a position
		//! whose fourth component is 1 (make_float4 of a float3), divided
		//! back by the scales -- an output Blender hides, kept for
		//! fidelity.  Likewise `position` is 0 in 1D, where Blender hides
		//! the Position output and the point is in `w`.  These zeros carry
		//! Cycles' sign: a negative Scale makes them -0.
		struct Result
		{
			Scalar distance;
			Scalar color[3];
			Scalar position[3];
			Scalar w;
			Scalar radius;
		};

		//! svm_node_tex_voronoi: `p` holds `dims` coordinates (1..4) BEFORE
		//! the node's Scale multiply, in the order the noise builtins read
		//! them (1D: w; 4D: x, y, z, w).  The node's clamps apply -- detail
		//! to [0, 15], roughness and randomness to [0, 1], smoothness / 2
		//! to [0, 0.5], each NaN to 0 -- then the multiply by `scale` in
		//! float, then the feature.  `dims`, `feature` or `metric` outside
		//! their ranges give every output 0 (WHERE THE PORT DEPARTS, above).
		void VoronoiTexture( int dims, const Scalar* p, int feature, int metric,
			Scalar scale, Scalar detail, Scalar roughness, Scalar lacunarity,
			Scalar smoothness, Scalar exponent, Scalar randomness, bool normalize,
			Result& out );

		//////////////////////////////////////////////////////////////
		// The PCG cell hashes (util/hash.h).  Exact integer arithmetic
		// (wrapping, as Cycles' signed arithmetic does in practice), so
		// these match Cycles bit for bit on every input.
		//////////////////////////////////////////////////////////////

		//! hash_int2_to_float2, hash_int3_to_float3, hash_int4_to_float4:
		//! components in [0, 1].
		void HashInt2ToFloat2( int32_t x, int32_t y, float out[2] );
		void HashInt3ToFloat3( int32_t x, int32_t y, int32_t z, float out[3] );
		void HashInt4ToFloat4( int32_t x, int32_t y, int32_t z, int32_t w, float out[4] );
	}
}

#endif
