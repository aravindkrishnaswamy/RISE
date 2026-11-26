//////////////////////////////////////////////////////////////////////
//
//  OslShaderNodes.h - Strict IEEE single-precision ports of Cycles'
//    Gradient Texture, Brick Texture, Fresnel, and Layer Weight nodes.
//
//  SOURCE & LICENCE.  The kernels in OslShaderNodes.cpp are ported,
//  with modifications, from Blender's Cycles renderer (intern/cycles,
//  Blender commit afc6d7a4):
//
//    intern/cycles/kernel/svm/gradient.h
//    intern/cycles/kernel/svm/brick.h
//    intern/cycles/kernel/svm/fresnel.h
//    intern/cycles/kernel/closure/bsdf_util.h
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    Licensed under the Apache License, Version 2.0; a copy is in
//    licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//  MODIFIED FOR RISE: rewritten as standalone C++ functions in
//  RISE::OslShaderNodes with non-finite and degenerate-divisor guards
//  and out-of-range enum clamping.
//
//  PRECISION CONTRACT.  OslShaderNodes.cpp MUST be compiled without
//  fast-math or FP contraction (-fno-fast-math -ffp-contract=off on
//  GCC/Clang, /fp:strict on MSVC); OslShaderNodes.cpp rejects
//  __FAST_MATH__ / _M_FP_FAST at compile time.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#ifndef OSL_SHADER_NODES_H_
#define OSL_SHADER_NODES_H_

namespace RISE
{
	namespace OslShaderNodes
	{
		//! Cycles Gradient Texture (`intern/cycles/kernel/svm/gradient.h`).
		//! `type`: 0=Linear, 1=Quadratic, 2=Easing, 3=Diagonal, 4=Radial,
		//! 5=QuadraticSphere, 6=Spherical.  Returns clamped factor in [0, 1].
		float Gradient(
			const float x,
			const float y,
			const float z,
			const int type
			);

		//! Cycles Brick Texture (`intern/cycles/kernel/svm/brick.h`).
		//! Writes `outTint` (brick colour mix factor in [0, 1]) and
		//! `outMortar` (mortar mask in [0, 1]).
		void Brick(
			const float px,
			const float py,
			const float mortar_size,
			const float mortar_smooth,
			const float bias,
			const float brick_width,
			const float row_height,
			const float offset_amount,
			const int offset_frequency,
			const float squash_amount,
			const int squash_frequency,
			float& outTint,
			float& outMortar
			);

		//! Cycles `fresnel_dielectric_cos` (`intern/cycles/kernel/closure/bsdf_util.h`).
		float FresnelDielectricCos(
			const float cosi,
			const float eta
			);

		//! Cycles Fresnel shader node (`intern/cycles/kernel/svm/fresnel.h`).
		float FresnelNode(
			const float ior,
			const float cosi,
			const bool backfacing
			);

		//! Cycles Layer Weight shader node (`intern/cycles/kernel/svm/fresnel.h`).
		//! `isFacing == false` -> Fresnel output; `isFacing == true` -> Facing output.
		float LayerWeightNode(
			const float blend,
			const float cosi,
			const bool backfacing,
			const bool isFacing
			);
	}
}

#endif // OSL_SHADER_NODES_H_
