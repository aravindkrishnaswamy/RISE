//////////////////////////////////////////////////////////////////////
//
//  OslColor.h - Cycles' RGB/HSV/HSL colour-space conversions and the
//    Mix Color node's 19 blend modes.
//
//  A PORT, not new colour math.  It reproduces the CPU kernel of
//  Blender's Cycles renderer -- intern/cycles/util/color.h (rgb_to_hsv,
//  hsv_to_rgb, rgb_to_hsl, hsl_to_rgb), intern/cycles/kernel/svm/
//  color_util.h (svm_mix and its 19 blend helpers), and intern/cycles/
//  kernel/svm/mix.h (svm_node_mix_color's factor and result clamps) --
//  so that a Blender material translated into a RISE expression
//  evaluates Separate/Combine Color, Hue/Saturation/Value and Mix
//  (Color) the same way Cycles does.  Cycles' legacy MixRGB node
//  (svm_node_mix) is identical to MixColor with clampFactor = true and
//  clampResult = use_clamp.
//
//  Ported from Blender commit afc6d7a4 (5.2 alpha, 2026-03-17), as
//  OslNoise.h and OslVoronoi.h are.  The golden values,
//  tests/data/osl_color_golden.inc, were dumped by
//  tools/osl_golden/dump_color_golden.sh, which runs Cycles' own
//  svm_node_separate_color, svm_node_combine_color and
//  svm_node_mix_color compiled three ways (the SIMD path as Blender's
//  arm64 build compiles it, the scalar path with Blender's math flags,
//  and the scalar path as strict IEEE); tests/OslColorTest.cpp gates on
//  the strict column.
//
//  PRECISION.  As OslNoise.h and OslVoronoi.h: 32-bit float in the
//  order of Cycles' scalar path ("standard implementation"), strict
//  IEEE (no reassociation, no reciprocal substitution, no fused
//  multiply-add; OslColor.cpp is compiled that way by every build),
//  double only at the interface.  Where Cycles' scalar path divides a
//  float3 by a scalar -- `(cmax3 - rgb) / cdelta` in rgb_to_hsv -- it
//  multiplies by `1.0f / cdelta`, and so does the port.  Every
//  operation is a correctly rounded IEEE 754 operation (no libm
//  transcendentals), so the strict column is bit-identical on every
//  IEEE platform (built and run so far on macOS arm64 only).
//
//  The port equals the strict build bit for bit (sign of zero included)
//  on all 1098 rows of tests/data/osl_color_golden.inc (300 conversion
//  rows and 798 Mix Color rows).  Against Cycles as Blender builds it
//  (the SIMD and scalar columns: Cycles' sources rebuilt with Blender's
//  CYCLES_MATH_FLAGS, not a shipped binary; on this arm64 build those
//  two columns are bit-identical to each other on every row), Tests 1a
//  and 1b of tests/OslColorTest.cpp measure only float rounding (from
//  -ffp-contract=on FMA and -freciprocal-math on scalar divides such as
//  `/ 6.0f`), with 0 sign-of-zero-only rows, 0 non-finite
//  disagreements, and 0 departures above 1e-5 relative to max(1, |v|):
//    * Colour conversions (300 rows): 251 bit-exact, 49 rounding rows,
//      max relative error 1.24e-7 (conv row 139, hsl2rgb).
//    * Mix Color (798 rows): 761 bit-exact, 37 rounding rows, max
//      relative error 5.07e-7 (mix_edge row 241, sat, cf=0, cr=0).
//  (Counts and maxima are of this table, not bounds: on adversarial
//  inputs near a discontinuous branch -- e.g. Burn's `tm + t*b <= 0`
//  or Dodge's `1 - t*b <= 0` with unclamped or out-of-gamut values --
//  FMA rounding could flip the branch.)
//
//  COST.  Straight float arithmetic with no loops, hashes or
//  transcendentals (not memo-worthy in the expression VM).  Measured by
//  tools/osl_golden/bench_color.cpp (Apple M1 Max, Apple clang 21, -O3
//  with this file's strict flags, one thread, two runs after a warm-up
//  agreeing within ~9%, 2e6 calls each; 9 of the 19 Mix modes timed
//  with clampFactor = true, clampResult = false on in-gamut [0.05..0.95]
//  inputs), in nanoseconds per call:
//    RgbToHsv 5.1, HsvToRgb 3.1, RgbToHsl 4.5, HslToRgb 3.6;
//    MixColor: Blend 2.7, Soft 4.2, Overlay 5.5, Dodge 5.5, Burn 6.6,
//              Val 10.4, Sat 11.2, Color 12.0, Hue 12.2.
//  For scale, 3D Noise Texture at detail 2 is ~100 ns on the same
//  machine (tools/osl_golden/bench_voronoi.cpp).
//
//  WHERE THE PORT DEPARTS FROM CYCLES.  Almost nowhere: none of these
//  functions converts float to int or invokes undefined behaviour on
//  non-finite inputs, so even NaN and +/-inf inputs match Cycles'
//  macOS/Linux strict IEEE build bit for bit (except that Cycles'
//  Windows build overrides fmaxf with `(a > b) ? a : b`, whereas this
//  port uses IEEE std::fmax everywhere).  The only departure is an
//  unknown `mode` passed to MixColor: Cycles' svm_mix switch also
//  handles the internal sentinel NODE_MIX_CLAMP (19), returning
//  saturate(a), whereas MixColor treats any `mode` outside 0..18 as
//  invalid and returns (0, 0, 0).  The expression builtin checks `mode`
//  in 0..18 as a compile-time integer literal, so only a direct C++
//  caller can pass 19.  At the expression-VM boundary
//  (ExpressionOslBuiltins.cpp), any non-finite output channel reads 0,
//  as for the other osl_ builtins.
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

		//! kernel/svm/mix.h `svm_node_mix_color`: optionally clamps `t`
		//! to [0, 1] (`clampFactor`), applies Cycles' `svm_mix` for
		//! `mode` in 0..18, then optionally clamps each output channel to
		//! [0, 1] (`clampResult`).  `mode` outside 0..18 writes (0, 0, 0).
		//! May alias `a` or `b` with `out`.
		void MixColor( int mode, Scalar t, const Scalar a[3], const Scalar b[3],
			bool clampFactor, bool clampResult, Scalar out[3] );
	}
}

#endif
