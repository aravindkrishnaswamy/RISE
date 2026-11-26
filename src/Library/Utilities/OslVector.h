//////////////////////////////////////////////////////////////////////
//
//  OslVector.h - Cycles' componentwise vec3 operations (min, max, abs,
//    floor, fract, pow, clamp) and Vector Rotate node (Rodrigues
//    axis-angle, X/Y/Z axis, and Euler XYZ with invert and center).
//
//  A PORT, not new vector math.  It reproduces the CPU kernel of
//  Blender's Cycles renderer -- intern/cycles/util/math_float3.h
//  (min, max, fabs, floor, safe_pow, clamp, rotate_around_axis),
//  intern/cycles/util/transform.h (euler_to_transform,
//  transform_direction, transform_direction_transposed), and
//  intern/cycles/kernel/svm/vector_rotate.h (svm_node_vector_rotate) --
//  so that a Blender material translated into a RISE expression
//  evaluates Vector Math componentwise ops and Vector Rotate the same
//  way Cycles does.
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17), as
//  OslNoise.h, OslVoronoi.h, OslColor.h and OslMath.h are.  The golden
//  values, tests/data/osl_vector_golden.inc, were dumped by
//  tools/osl_golden/dump_vector_golden.sh, which runs Cycles' own
//  float3 ops and svm_node_vector_rotate compiled three ways (SIMD with
//  Blender's math flags, scalar with those flags, and strict IEEE);
//  tests/OslVectorTest.cpp gates on the strict column.
//
//  PRECISION.  32-bit float in the exact scalar-path order of
//  operations, strict IEEE (no FMA, no reassociation, no reciprocal
//  substitution; OslVector.cpp is compiled that way by every build),
//  double only at the interface.  In particular:
//    * `axis / axis_length` multiplies by `1.0f / axis_length`
//      (matching Cycles' scalar `operator/(float3, float)`).
//    * `transform_direction` uses Cycles' scalar float4 dot pairing
//      `(d.x*t.x.x + d.y*t.x.y) + (d.z*t.x.z + 0.0f*0.0f)`, whereas
//      `transform_direction_transposed` uses float3 left-to-right dot
//      `(d.x*tx + d.y*ty) + d.z*tz`.
//    * Operations calling libm (`sinf`, `cosf`, `powf`) are bit-exact on
//      Apple arm64 (where the golden table was dumped) and gated to 1e-6
//      relative off Apple arm64.
//    * Measured on Apple M1 Max (arm64, Apple clang 21) against all 186
//      rows of tests/data/osl_vector_golden.inc: 186/186 bit-exact against
//      the strict IEEE column (0 non-exact):
//      - Componentwise ops (76 rows): 76/76 exact against all 3 builds.
//      - VectorRotate (110 rows): vs Blender SIMD, 83 exact, 27 rounding
//        (maxRel 2.384e-7); vs Blender scalar, 80 exact, 30 rounding
//        (maxRel 5.635e-7), 0 sign0, 0 nonfin.
//
//  WHERE THE PORT DEPARTS FROM CYCLES.
//    1. `VecPow` guards `safe_powf`'s float->int cast when `a < 0.0f`
//       against NaN, +/-inf, or out-of-int32-range exponents (same guard
//       as OslMath.h).
//    2. Out-of-range `type` values in `VectorRotate` behave like
//       Cycles' `default:` case (axis-angle around `axis`); the VM
//       builtin validates `type` in `0..4` and `invert` in `0..1` at
//       compile time, and maps non-finite output channels to 0.
//
//  LICENCES.  RISE's own licence is BSD-style (LICENSE.TXT).  The ported
//  code carries its sources' licences, reproduced here as they require:
//
//    kernel/svm/vector_rotate.h, util/transform.h, util/math_base.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    util/math_float3.h, util/math_float4.h --
//      SPDX-FileCopyrightText: 2011-2013 Intel Corporation
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    all of them --
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    MODIFIED FOR RISE: rewritten as standalone functions taking double
//    arrays at the interface, SVM stack unpacking removed, and the
//    float-to-int range guard added in VecPow.
//    licenses/Cycles-NOTICE.txt summarises these notices for binary
//    distributions.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#ifndef OSL_VECTOR_
#define OSL_VECTOR_

#include "Math3D/Math3D.h"	// Scalar

namespace RISE
{
	namespace OslVector
	{
		//! Cycles' NodeVectorRotateType numbering (intern/cycles/kernel/svm/types.h).
		enum RotateType
		{
			eRotateAxis     = 0,
			eRotateAxisX    = 1,
			eRotateAxisY    = 2,
			eRotateAxisZ    = 3,
			eRotateEulerXYZ = 4,
			kRotateTypeCount = 5
		};

		void VecMin( const Scalar a[3], const Scalar b[3], Scalar out[3] );
		void VecMax( const Scalar a[3], const Scalar b[3], Scalar out[3] );
		void VecAbs( const Scalar a[3], Scalar out[3] );
		void VecFloor( const Scalar a[3], Scalar out[3] );
		void VecFract( const Scalar a[3], Scalar out[3] );
		void VecPow( const Scalar a[3], const Scalar b[3], Scalar out[3] );
		void VecClamp( const Scalar a[3], const Scalar mn[3], const Scalar mx[3], Scalar out[3] );

		//! Rodrigues rotation of `v` around `axis` by `angle` radians.
		//! Normalizes `axis` (returning `v` unchanged if `len(axis) == 0`).
		void RotateAroundAxis( const Scalar v[3], const Scalar axis[3], Scalar angle, Scalar out[3] );

		//! Cycles' svm_node_vector_rotate (0=AXIS, 1=X, 2=Y, 3=Z, 4=EULER_XYZ).
		void VectorRotate(
			int type,
			bool invert,
			const Scalar v[3],
			const Scalar center[3],
			const Scalar axis[3],
			Scalar angle,
			const Scalar euler[3],
			Scalar out[3] );
	}
}

#endif
