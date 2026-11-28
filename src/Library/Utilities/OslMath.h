//////////////////////////////////////////////////////////////////////
//
//  OslMath.h - Ports of Blender Cycles' Safe Math (41 ops), Vector Math
//    (30 ops), Clamp (2 modes), and Map Range (Float & Vector, 4 modes).
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17):
//    intern/cycles/kernel/svm/{math_util.h,math.h,clamp.h,map_range.h}
//    and util/{math_base.h,math_float3.h}.  Verified against
//    tests/data/osl_math_golden.inc by OslMathTest.
//
//  PRECISION.  Evaluates in strict IEEE 32-bit float in Cycles' scalar
//  order of operations (no FMA, reassociation, or reciprocal math;
//  OslMath.cpp is compiled with -fno-fast-math -ffp-contract=off or
//  /fp:strict), double `Scalar` only at the interface.  Ops calling libm
//  transcendentals (sinf..tanhf, expf, logf, powf) may differ in the last
//  bit across platforms (gated to 1e-6 off Apple arm64).
//
//  DEPARTURES.  (1) `safe_powf(a, b)` guards the float->int cast when
//  `a < 0.0f` so NaN, +/-inf, or out-of-int32 `b` returns 0.0f without
//  UB.  (2) Out-of-range enums return 0 / (0, 0, 0); VM builtins validate
//  enums at compile time and map non-finite output channels to 0.
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
