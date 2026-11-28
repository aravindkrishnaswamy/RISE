//////////////////////////////////////////////////////////////////////
//
//  OslColor.h - Ports of Blender Cycles' RGB/HSV/HSL colour-space
//    conversions and Mix Color node (19 blend modes).
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17):
//    intern/cycles/util/color.h (rgb_to_hsv, hsv_to_rgb, rgb_to_hsl,
//    hsl_to_rgb), kernel/svm/color_util.h (svm_mix), and kernel/svm/mix.h
//    (svm_node_mix_color; legacy MixRGB is clampFactor=true, clampResult=
//    use_clamp).  Verified against tests/data/osl_color_golden.inc by
//    OslColorTest.
//
//  PRECISION.  Evaluates in strict IEEE 32-bit float in Cycles' scalar
//  order of operations (no FMA, reassociation, or reciprocal math;
//  OslColor.cpp is compiled with -fno-fast-math -ffp-contract=off or
//  /fp:strict), multiplying by `1.0f / cdelta` in RgbToHsv as Cycles
//  does; double `Scalar` only at the interface.  Pure IEEE arithmetic
//  (no libm transcendentals), bit-identical across IEEE platforms.
//
//  DEPARTURES.  Uses IEEE std::fmax everywhere (Cycles' Windows build
//  overrides fmaxf).  MixColor rejects `mode` outside 0..18 (including
//  Cycles' internal NODE_MIX_CLAMP = 19) and writes (0, 0, 0).  Non-
//  finite channels follow strict Cycles here; VM builtins map them to 0.
//
//  LICENCES.  RISE's own licence is BSD-style (LICENSE.TXT).  The ported
//  code carries its sources' licences, reproduced here as they require:
//
//    util/color.h, kernel/svm/color_util.h, kernel/svm/mix.h,
//    util/math_base.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    util/math_float3.h (the float3 helpers: interp, saturate, min,
//    max, fabs, operator/ by scalar) --
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
//    arrays at the interface, the SVM stack unpacking removed, and
//    NODE_MIX_CLAMP (19) excluded from MixColor (replaced by the
//    `clampResult` flag, matching svm_node_mix_color).
//    licenses/Cycles-NOTICE.txt summarises these notices for binary
//    distributions.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#ifndef OSL_COLOR_
#define OSL_COLOR_

#include "Math3D/Math3D.h"	// Scalar

namespace RISE
{
	namespace OslColor
	{
		//! Cycles' NodeMix numbering (intern/cycles/kernel/svm/types.h),
		//! covering the 19 user-selectable Mix Color blend modes.
		//! Cycles' internal sentinel NODE_MIX_CLAMP (19) is not a blend
		//! mode; use MixColor's `clampResult` parameter instead.
		enum MixMode
		{
			eBlend     = 0,		// NODE_MIX_BLEND     ("Mix")
			eAdd       = 1,		// NODE_MIX_ADD       ("Add")
			eMul       = 2,		// NODE_MIX_MUL       ("Multiply")
			eSub       = 3,		// NODE_MIX_SUB       ("Subtract")
			eScreen    = 4,		// NODE_MIX_SCREEN    ("Screen")
			eDiv       = 5,		// NODE_MIX_DIV       ("Divide")
			eDiff      = 6,		// NODE_MIX_DIFF      ("Difference")
			eDark      = 7,		// NODE_MIX_DARK      ("Darken")
			eLight     = 8,		// NODE_MIX_LIGHT     ("Lighten")
			eOverlay   = 9,		// NODE_MIX_OVERLAY   ("Overlay")
			eDodge     = 10,	// NODE_MIX_DODGE     ("Color Dodge")
			eBurn      = 11,	// NODE_MIX_BURN      ("Color Burn")
			eHue       = 12,	// NODE_MIX_HUE       ("Hue")
			eSat       = 13,	// NODE_MIX_SAT       ("Saturation")
			eVal       = 14,	// NODE_MIX_VAL       ("Value")
			eCol       = 15,	// NODE_MIX_COL       ("Color")
			eSoft      = 16,	// NODE_MIX_SOFT      ("Soft Light")
			eLinear    = 17,	// NODE_MIX_LINEAR    ("Linear Light")
			eExclusion = 18		// NODE_MIX_EXCLUSION ("Exclusion")
		};
		static const int kMixModeCount = 19;

		//! util/color.h `rgb_to_hsv`.  May alias `rgb` and `out`.
		void RgbToHsv( const Scalar rgb[3], Scalar out[3] );

		//! util/color.h `hsv_to_rgb`.  May alias `hsv` and `out`.
		void HsvToRgb( const Scalar hsv[3], Scalar out[3] );

		//! util/color.h `rgb_to_hsl`.  May alias `rgb` and `out`.
		void RgbToHsl( const Scalar rgb[3], Scalar out[3] );

		//! util/color.h `hsl_to_rgb`.  May alias `hsl` and `out`.
		void HslToRgb( const Scalar hsl[3], Scalar out[3] );

		//! Cycles' svm_node_mix_color: optionally clamps `t` (`clampFactor`),
		//! blends with `mode` in 0..18, and optionally clamps output channels
		//! to [0, 1] (`clampResult`). Out-of-range `mode` writes (0, 0, 0).
		//! May alias `a` or `b` with `out`.
		void MixColor( int mode, Scalar t, const Scalar a[3], const Scalar b[3],
			bool clampFactor, bool clampResult, Scalar out[3] );
	}
}

#endif
