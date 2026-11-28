//////////////////////////////////////////////////////////////////////
//
//  OslVoronoi.h - Port of Blender Cycles' Voronoi Texture node.
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17),
//  intern/cycles/kernel/svm/voronoi.h and util/hash.h: F1, F2, Smooth F1,
//  Distance to Edge, N-Sphere Radius; Euclidean, Manhattan, Chebychev,
//  Minkowski metrics; 1D-4D with fractal Detail/Roughness/Lacunarity/
//  Normalize; PCG cell hashes (2D-4D) and lookup3 float-bit hashes (1D).
//  Verified against tests/data/osl_voronoi_golden.inc by OslVoronoiTest.
//
//  PRECISION.  Evaluates in strict IEEE 32-bit float in Cycles' scalar
//  order of operations (no FMA, reassociation, or reciprocal math;
//  OslVoronoi.cpp is compiled with -fno-fast-math -ffp-contract=off or
//  /fp:strict), multiplying by reciprocals where Cycles' scalar vector
//  division does; double `Scalar` only at the interface.  Minkowski `powf`
//  uses platform libm (last-bit differences possible off Apple arm64).
//  Rounding differences can flip nearest-cell ties on cell boundaries,
//  and Blender's -fno-signed-zeros builds differ on 1D -0 inputs.
//
//  DEPARTURES / NON-FINITE INPUTS.  (1) Non-finite post-Scale coordinates
//  return all-zero outputs.  (2) Non-finite scaled octave coordinates
//  break out of the fractal loop, keeping prior octaves.  (3) Out-of-
//  int32 cell positions saturate to [INT32_MIN, INT32_MAX] with wrapping
//  cell-id addition (matching arm64 scalar; SIMD _mm_cvtps_epi32 gives
//  INT32_MIN).  (4) Out-of-range dims/feature/metric returns all-zero
//  outputs.  Finite inputs producing non-finite results (e.g. zero
//  Lacunarity or Minkowski exponent) follow Cycles; VM builtins map them
//  to 0.
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

		//! Node outputs (unused outputs per feature are 0, carrying Cycles'
		//! sign of zero): Distance to Edge writes only `distance`, N-Sphere
		//! Radius only `radius`, others all except `radius`. In 1D `position`
		//! is 0 and the point is in `w`; in 3D `w` reflects Cycles' float4(p,1).
		struct Result
		{
			Scalar distance;
			Scalar color[3];
			Scalar position[3];
			Scalar w;
			Scalar radius;
		};

		//! Cycles' svm_node_tex_voronoi: `p` holds `dims` unscaled coords
		//! (1D: w; 4D: x, y, z, w). Clamps detail [0,15], roughness/randomness
		//! [0,1], smoothness/2 [0,0.5] (NaN -> 0), multiplies by `scale` in
		//! float, then evaluates `feature`. Invalid dims/feature/metric -> 0.
		void VoronoiTexture( int dims, const Scalar* p, int feature, int metric,
			Scalar scale, Scalar detail, Scalar roughness, Scalar lacunarity,
			Scalar smoothness, Scalar exponent, Scalar randomness, bool normalize,
			Result& out );

		//////////////////////////////////////////////////////////////
		// PCG cell hashes (util/hash.h) -- exact wrapping integer math.
		//////////////////////////////////////////////////////////////

		//! hash_int2_to_float2, hash_int3_to_float3, hash_int4_to_float4:
		//! components in [0, 1].
		void HashInt2ToFloat2( int32_t x, int32_t y, float out[2] );
		void HashInt3ToFloat3( int32_t x, int32_t y, int32_t z, float out[3] );
		void HashInt4ToFloat4( int32_t x, int32_t y, int32_t z, int32_t w, float out[4] );
	}
}

#endif
