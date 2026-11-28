//////////////////////////////////////////////////////////////////////
//
//  OslVector.h - Ports of Blender Cycles' componentwise vec3 ops and
//    Vector Rotate node (axis-angle, X/Y/Z axis, Euler XYZ).
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17):
//    intern/cycles/util/math_float3.h, util/transform.h, and
//    kernel/svm/vector_rotate.h.  Verified against
//    tests/data/osl_vector_golden.inc by OslVectorTest.
//
//  PRECISION.  Evaluates in strict IEEE 32-bit float in Cycles' scalar
//  order of operations (no FMA, reassociation, or reciprocal math;
//  OslVector.cpp is compiled with -fno-fast-math -ffp-contract=off or
//  /fp:strict), double `Scalar` only at the interface.  Preserves
//  Cycles' scalar reciprocal multiply (`axis * (1.0f / len)`), float4
//  paired dot in `transform_direction`, and left-to-right float3 dot in
//  `transform_direction_transposed`.  Libm calls (`sinf`, `cosf`, `powf`)
//  may differ in the last bit across platforms (gated to 1e-6 off arm64).
//
//  DEPARTURES.  (1) `VecPow` guards `safe_powf`'s float->int cast when
//  `a < 0.0f` against NaN, +/-inf, or out-of-int32 exponents (same as
//  OslMath.h).  (2) Out-of-range `type` in `VectorRotate` falls through
//  to axis-angle (Cycles' default); VM builtins validate `type`/`invert`
//  and map non-finite output channels to 0.
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
