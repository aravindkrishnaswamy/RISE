//////////////////////////////////////////////////////////////////////
//
//  OslMath.h - Cycles' Safe Math (41 ops), Vector Math (30 ops),
//    Clamp (2 modes), and Map Range (Float & Vector, 4 modes) kernels.
//
//  A PORT, not new math.  It reproduces the CPU kernel of Blender's
//  Cycles renderer -- intern/cycles/kernel/svm/math_util.h (svm_math,
//  svm_vector_math), intern/cycles/kernel/svm/math.h,
//  intern/cycles/kernel/svm/clamp.h, intern/cycles/kernel/svm/map_range.h,
//  and the scalar/float3 helpers in util/math_base.h and
//  util/math_float3.h -- so that a Blender material translated into a
//  RISE expression evaluates Math, Vector Math, Clamp and Map Range
//  nodes the same way Cycles does.
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17), as
//  OslNoise.h, OslVoronoi.h and OslColor.h are.  The golden values,
//  tests/data/osl_math_golden.inc, were dumped by
//  tools/osl_golden/dump_math_golden.sh, which runs Cycles' own
//  svm_node_math, svm_node_vector_math<float3>, svm_node_clamp,
//  svm_node_map_range and svm_node_vector_map_range compiled three ways
//  (the SIMD path as Blender's arm64 build compiles it, the scalar path
//  with Blender's math flags, and the scalar path as strict IEEE);
//  tests/OslMathTest.cpp gates on the strict column.
//
//  PRECISION.  As OslNoise.h, OslVoronoi.h and OslColor.h: 32-bit float
//  in the order of Cycles' scalar path ("standard implementation"),
//  strict IEEE (no reassociation, no reciprocal substitution, no fused
//  multiply-add; OslMath.cpp is compiled that way by every build),
//  double only at the interface.  Measured against the strict IEEE
//  golden table on Apple M1 Max (arm64, Apple clang 21):
//    * Bit-exact (sign of zero included) on all 1247 rows of
//      tests/data/osl_math_golden.inc on Apple arm64 (0 non-exact):
//      - Math (779 rows): vs Blender SIMD/scalar, 775 exact, 4 rounding
//        (maxRel 1.168e-7), 0 sign0, 0 nonfin.
//      - VectorMath (330 rows): vs Blender SIMD, 323 exact, 4 sign0,
//        3 rounding (maxRel 1.102e-7); vs Blender scalar, 318 exact,
//        12 rounding (maxRel 1.484e-7).
//      - Clamp (26 rows): 26/26 exact against all 3 builds.
//      - MapRange (72 rows): 70 exact, 2 rounding (maxRel 6.535e-8).
//      - VectorMapRange (40 rows): 40/40 exact against all 3 builds.
//    * Operations that call libm transcendentals (sinf, cosf, tanf,
//      asinf, acosf, atanf, atan2f, sinhf, coshf, tanhf, expf, logf,
//      powf) are bit-exact on Apple arm64 and gated to 1e-6 relative
//      off Apple arm64 (the same kLibmIsGoldenPlatform pattern as
//      OslVoronoiTest.cpp).
//
//  WHERE THE PORT DEPARTS FROM CYCLES.
//    1. `safe_powf(a, b)` in Cycles checks `a < 0.0f && b != float_to_int(b)`
//       where `float_to_int` is a raw C cast `(int)b`, which is undefined
//       behaviour in C++ when `b` is NaN, +/-inf, or outside int32 range
//       `[-2147483648.0f, 2147483648.0f)`.  This port guards the cast:
//       when `a < 0.0f`, if `!(b >= -2147483648.0f && b < 2147483648.0f)`
//       it returns `0.0f` without invoking UB (matching Cycles' intent of
//       returning 0 on non-integer/invalid exponents for negative bases).
//       For all finite `b` in `[-2147483648.0f, 2147483520.0f]` it is
//       bit-identical to Cycles.
//    2. Out-of-range enum values return `0.0` (or `(0, 0, 0)`), matching
//       Cycles' `default:` cases.  At the expression-VM boundary
//       (ExpressionOslBuiltins.cpp), op/type/clamp arguments are checked
//       as compile-time integer literals and any non-finite output channel
//       reads 0.
//
//  LICENCES.  RISE's own licence is BSD-style (LICENSE.TXT).  The ported
//  code carries its sources' licences, reproduced here as they require:
//
//    kernel/svm/math_util.h, kernel/svm/math.h, kernel/svm/clamp.h,
//    kernel/svm/map_range.h, util/math_base.h (including wrapf adapted
//    from godot-engine math_funcs.h) --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    util/math_float3.h --
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
//    arguments at the interface, SVM stack unpacking removed, and the
//    float-to-int range guard added in safe_powf.
//    licenses/Cycles-NOTICE.txt summarises these notices for binary
//    distributions.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#ifndef OSL_MATH_
#define OSL_MATH_

#include "Math3D/Math3D.h"	// Scalar

namespace RISE
{
	namespace OslMath
	{
		//! Cycles' NodeMathType numbering (intern/cycles/kernel/svm/types.h).
		enum MathOp
		{
			eMathAdd           = 0,
			eMathSubtract      = 1,
			eMathMultiply      = 2,
			eMathDivide        = 3,
			eMathSine          = 4,
			eMathCosine        = 5,
			eMathTangent       = 6,
			eMathArcsine       = 7,
			eMathArccosine     = 8,
			eMathArctangent    = 9,
			eMathPower         = 10,
			eMathLogarithm     = 11,
			eMathMinimum       = 12,
			eMathMaximum       = 13,
			eMathRound         = 14,
			eMathLessThan      = 15,
			eMathGreaterThan   = 16,
			eMathModulo        = 17,
			eMathAbsolute      = 18,
			eMathArctan2       = 19,
			eMathFloor         = 20,
			eMathCeil          = 21,
			eMathFraction      = 22,
			eMathSqrt          = 23,
			eMathInvSqrt       = 24,
			eMathSign          = 25,
			eMathExponent      = 26,
			eMathRadians       = 27,
			eMathDegrees       = 28,
			eMathSinh          = 29,
			eMathCosh          = 30,
			eMathTanh          = 31,
			eMathTrunc         = 32,
			eMathSnap          = 33,
			eMathWrap          = 34,
			eMathCompare       = 35,
			eMathMultiplyAdd   = 36,
			eMathPingPong      = 37,
			eMathSmoothMin     = 38,
			eMathSmoothMax     = 39,
			eMathFlooredModulo = 40,
			kMathOpCount       = 41
		};

		//! Cycles' NodeVectorMathType numbering (intern/cycles/kernel/svm/types.h).
		enum VectorMathOp
		{
			eVecAdd          = 0,
			eVecSubtract     = 1,
			eVecMultiply     = 2,
			eVecDivide       = 3,
			eVecCross        = 4,
			eVecProject      = 5,
			eVecReflect      = 6,
			eVecDot          = 7,
			eVecDistance     = 8,
			eVecLength       = 9,
			eVecScale        = 10,
			eVecNormalize    = 11,
			eVecSnap         = 12,
			eVecFloor        = 13,
			eVecCeil         = 14,
			eVecModulo       = 15,
			eVecFraction     = 16,
			eVecAbsolute     = 17,
			eVecMinimum      = 18,
			eVecMaximum      = 19,
			eVecWrap         = 20,
			eVecSine         = 21,
			eVecCosine       = 22,
			eVecTangent      = 23,
			eVecRefract      = 24,
			eVecFaceforward  = 25,
			eVecMultiplyAdd  = 26,
			eVecPower        = 27,
			eVecSign         = 28,
			eVecRound        = 29,
			kVecMathOpCount  = 30
		};

		//! Cycles' NodeClampType numbering (intern/cycles/kernel/svm/types.h).
		enum ClampType
		{
			eClampMinMax     = 0,
			eClampRange      = 1,
			kClampTypeCount  = 2
		};

		//! Cycles' NodeMapRangeType numbering (intern/cycles/kernel/svm/types.h).
		enum MapRangeType
		{
			eMapLinear        = 0,
			eMapStepped       = 1,
			eMapSmoothstep    = 2,
			eMapSmootherstep  = 3,
			kMapRangeTypeCount = 4
		};

		//! Cycles' svm_math (all 41 NodeMathType operations).
		Scalar Math( int op, Scalar a, Scalar b, Scalar c );

		//! Cycles' svm_vector_math scalar-output operations (Dot=7, Distance=8,
		//! Length=9).  Returns 0 for any other op.
		Scalar VectorMathScalar( int op, const Scalar a[3], const Scalar b[3] );

		//! Cycles' svm_vector_math vector-output operations (0..6, 10..29).
		//! Writes (0, 0, 0) for scalar-output ops (7..9) or unknown ops.
		void VectorMathVector(
			int op,
			const Scalar a[3],
			const Scalar b[3],
			const Scalar c[3],
			Scalar scale,
			Scalar out[3] );

		//! Cycles' svm_node_clamp (0 = MINMAX, 1 = RANGE).
		Scalar Clamp( int type, Scalar value, Scalar mn, Scalar mx );

		//! Cycles' svm_node_map_range (0..3) with optional RANGE clamp
		//! (ignored for SMOOTHSTEP / SMOOTHERSTEP, as in Blender).
		Scalar MapRange(
			int type,
			bool clampResult,
			Scalar value,
			Scalar fromMin,
			Scalar fromMax,
			Scalar toMin,
			Scalar toMax,
			Scalar steps );

		//! Cycles' svm_node_vector_map_range (0..3) with optional per-channel
		//! RANGE clamp (ignored for SMOOTHSTEP / SMOOTHERSTEP, as in Cycles).
		void VectorMapRange(
			int type,
			bool clampResult,
			const Scalar value[3],
			const Scalar fromMin[3],
			const Scalar fromMax[3],
			const Scalar toMin[3],
			const Scalar toMax[3],
			const Scalar steps[3],
			Scalar out[3] );
	}
}

#endif
