//////////////////////////////////////////////////////////////////////
//
//  OslColor.cpp - A port of Cycles' colour-space conversions and Mix
//    Color blend modes.  See OslColor.h for the source, the precision
//    contract and the single departure (unknown MixMode).
//
//  Each function below names the Cycles function it ports and keeps its
//  exact scalar-path order of operations, so a reader can check one
//  against the other line by line.
//
//  LICENCES.  This file contains code ported, with modifications, from
//  Blender's Cycles renderer (commit afc6d7a4):
//
//    intern/cycles/util/color.h (rgb_to_hsv, hsv_to_rgb, rgb_to_hsl,
//    hsl_to_rgb), intern/cycles/kernel/svm/color_util.h (svm_mix and
//    the 19 svm_mix_* helpers), intern/cycles/kernel/svm/mix.h
//    (svm_node_mix_color), and the scalar helpers (min, max, clamp,
//    saturatef) from util/math_base.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    the float3 helpers (interp, saturate, min, max, fabs, operator/ by
//    scalar) from util/math_float3.h --
//      SPDX-FileCopyrightText: 2011-2013 Intel Corporation
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    all of them --
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    MODIFIED FOR RISE: see OslColor.h.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OslColor.h"
#include <cmath>

// The kernels are strict IEEE ports, compiled by every build with
// -fno-fast-math -ffp-contract=off (or /fp:strict); OslNoise.cpp's
// "HOW THE STRICT ARITHMETIC IS ENFORCED" explains why the per-file flag
// is needed and what this guard catches.
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "OslColor.cpp must be compiled without fast-math (-fno-fast-math -ffp-contract=off, or /fp:strict): its kernels are strict IEEE ports of Cycles' colour functions"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace RISE
{
	namespace OslColor
	{
		namespace
		{
			//////////////////////////////////////////////////////////
			// Scalar and float3 helpers, matching Cycles' scalar path
			// in util/math_base.h and util/math_float3.h.
			//////////////////////////////////////////////////////////

			struct Float3
			{
				float x, y, z;
			};

			inline Float3 MakeFloat3( const float x, const float y, const float z )
			{
				const Float3 r = { x, y, z };
				return r;
			}

			//! util/math_base.h `min(float, float)`: returns b when a is NaN,
			//! and b when a and b are equal zeros of opposite sign.
			inline float MinF( const float a, const float b )
			{
				return ( a < b ) ? a : b;
			}

			//! util/math_base.h `max(float, float)`: returns b when a is NaN,
			//! and b when a and b are equal zeros of opposite sign.
			inline float MaxF( const float a, const float b )
			{
				return ( a > b ) ? a : b;
			}

			//! util/math_base.h `clamp(float, float, float)`: NaN clamps to mn.
			inline float ClampF( const float a, const float mn, const float mx )
			{
				return MinF( MaxF( a, mn ), mx );
			}

			//! util/math_base.h `saturatef(float)`.
			inline float SaturateF( const float a )
			{
				return ClampF( a, 0.0f, 1.0f );
			}

			//! util/math_float3.h `saturate(float3)`.
			inline Float3 Saturate3( const Float3 a )
			{
				return MakeFloat3( SaturateF( a.x ), SaturateF( a.y ), SaturateF( a.z ) );
			}

			//! util/math_float3.h `interp(float3, float3, float)`:
			//! `a + t * (b - a)` (Cycles' scalar `operator*(float, float3)`
			//! evaluates `a.k * f`, commutative with `f * a.k` in IEEE 754).
			inline Float3 Interp3( const Float3 a, const Float3 b, const float t )
			{
				return MakeFloat3(
					a.x + t * ( b.x - a.x ),
					a.y + t * ( b.y - a.y ),
					a.z + t * ( b.z - a.z ) );
			}

			//////////////////////////////////////////////////////////
			// Colour-space conversions (util/color.h)
			//////////////////////////////////////////////////////////

			//! util/color.h `rgb_to_hsv`.
			Float3 RgbToHsvF( const Float3 rgb )
			{
				const float cmax = std::fmax( rgb.x, std::fmax( rgb.y, rgb.z ) );
				const float cmin = MinF( rgb.x, MinF( rgb.y, rgb.z ) );
				const float cdelta = cmax - cmin;

				const float v = cmax;
				float s;
				float h;

				if( cmax != 0.0f ) {
					s = cdelta / cmax;
				} else {
					s = 0.0f;
					h = 0.0f;
				}

				if( s != 0.0f ) {
					// Cycles' scalar `operator/(float3, float)` multiplies by
					// `1.0f / cdelta` rather than dividing each component.
					const float inv = 1.0f / cdelta;
					const float cx = ( cmax - rgb.x ) * inv;
					const float cy = ( cmax - rgb.y ) * inv;
					const float cz = ( cmax - rgb.z ) * inv;

					if( rgb.x == cmax ) {
						h = cz - cy;
					} else if( rgb.y == cmax ) {
						h = 2.0f + cx - cz;
					} else {
						h = 4.0f + cy - cx;
					}

					h /= 6.0f;

					if( h < 0.0f ) {
						h += 1.0f;
					}
				} else {
					h = 0.0f;
				}

				return MakeFloat3( h, s, v );
			}

			//! util/color.h `hsv_to_rgb`.
			Float3 HsvToRgbF( const Float3 hsv )
			{
				float h = hsv.x;
				const float s = hsv.y;
				const float v = hsv.z;

				if( s != 0.0f ) {
					if( h == 1.0f ) {
						h = 0.0f;
					}

					h *= 6.0f;
					const float i = std::floor( h );
					const float f = h - i;
					const float p = v * ( 1.0f - s );
					const float q = v * ( 1.0f - ( s * f ) );
					const float t = v * ( 1.0f - ( s * ( 1.0f - f ) ) );

					if( i == 0.0f ) {
						return MakeFloat3( v, t, p );
					} else if( i == 1.0f ) {
						return MakeFloat3( q, v, p );
					} else if( i == 2.0f ) {
						return MakeFloat3( p, v, t );
					} else if( i == 3.0f ) {
						return MakeFloat3( p, q, v );
					} else if( i == 4.0f ) {
						return MakeFloat3( t, p, v );
					} else {
						return MakeFloat3( v, p, q );
					}
				} else {
					return MakeFloat3( v, v, v );
				}
			}

			//! util/color.h `rgb_to_hsl`.
			Float3 RgbToHslF( const Float3 rgb )
			{
				const float cmax = std::fmax( rgb.x, std::fmax( rgb.y, rgb.z ) );
				const float cmin = MinF( rgb.x, MinF( rgb.y, rgb.z ) );
				const float l = MinF( 1.0f, ( cmax + cmin ) / 2.0f );

				float h;
				float s;
				if( cmax == cmin ) {
					h = s = 0.0f;
				} else {
					const float cdelta = cmax - cmin;
					s = ( l > 0.5f ) ? ( cdelta / ( 2.0f - cmax - cmin ) ) : ( cdelta / ( cmax + cmin ) );
					if( cmax == rgb.x ) {
						h = ( rgb.y - rgb.z ) / cdelta + ( ( rgb.y < rgb.z ) ? 6.0f : 0.0f );
					} else if( cmax == rgb.y ) {
						h = ( rgb.z - rgb.x ) / cdelta + 2.0f;
					} else {
						h = ( rgb.x - rgb.y ) / cdelta + 4.0f;
					}
				}
				h /= 6.0f;

				return MakeFloat3( h, s, l );
			}

			//! util/color.h `hsl_to_rgb`.
			Float3 HslToRgbF( const Float3 hsl )
			{
				const float h = hsl.x;
				const float s = hsl.y;
				const float l = hsl.z;

				float nr = std::fabs( h * 6.0f - 3.0f ) - 1.0f;
				float ng = 2.0f - std::fabs( h * 6.0f - 2.0f );
				float nb = 2.0f - std::fabs( h * 6.0f - 4.0f );

				nr = ClampF( nr, 0.0f, 1.0f );
				nb = ClampF( nb, 0.0f, 1.0f );
				ng = ClampF( ng, 0.0f, 1.0f );

				const float chroma = ( 1.0f - std::fabs( 2.0f * l - 1.0f ) ) * s;

				return MakeFloat3(
					( nr - 0.5f ) * chroma + l,
					( ng - 0.5f ) * chroma + l,
					( nb - 0.5f ) * chroma + l );
			}

			//////////////////////////////////////////////////////////
			// The 19 Mix Color blend modes (kernel/svm/color_util.h)
			//////////////////////////////////////////////////////////

			//! svm_mix_blend
			Float3 MixBlend( const float t, const Float3 col1, const Float3 col2 )
			{
				return Interp3( col1, col2, t );
			}

			//! svm_mix_add
			Float3 MixAdd( const float t, const Float3 col1, const Float3 col2 )
			{
				return Interp3( col1, MakeFloat3( col1.x + col2.x, col1.y + col2.y, col1.z + col2.z ), t );
			}

			//! svm_mix_mul
			Float3 MixMul( const float t, const Float3 col1, const Float3 col2 )
			{
				return Interp3( col1, MakeFloat3( col1.x * col2.x, col1.y * col2.y, col1.z * col2.z ), t );
			}

			//! svm_mix_screen
			Float3 MixScreen( const float t, const Float3 col1, const Float3 col2 )
			{
				const float tm = 1.0f - t;
				return MakeFloat3(
					1.0f - ( tm + t * ( 1.0f - col2.x ) ) * ( 1.0f - col1.x ),
					1.0f - ( tm + t * ( 1.0f - col2.y ) ) * ( 1.0f - col1.y ),
					1.0f - ( tm + t * ( 1.0f - col2.z ) ) * ( 1.0f - col1.z ) );
			}

			//! svm_mix_overlay (one channel)
			inline float MixOverlayCh( const float t, const float tm, const float c1, const float c2 )
			{
				if( c1 < 0.5f ) {
					return c1 * ( tm + 2.0f * t * c2 );
				} else {
					return 1.0f - ( tm + 2.0f * t * ( 1.0f - c2 ) ) * ( 1.0f - c1 );
				}
			}

			//! svm_mix_overlay
			Float3 MixOverlay( const float t, const Float3 col1, const Float3 col2 )
			{
				const float tm = 1.0f - t;
				return MakeFloat3(
					MixOverlayCh( t, tm, col1.x, col2.x ),
					MixOverlayCh( t, tm, col1.y, col2.y ),
					MixOverlayCh( t, tm, col1.z, col2.z ) );
			}

			//! svm_mix_sub
			Float3 MixSub( const float t, const Float3 col1, const Float3 col2 )
			{
				return Interp3( col1, MakeFloat3( col1.x - col2.x, col1.y - col2.y, col1.z - col2.z ), t );
			}

			//! svm_mix_div
			Float3 MixDiv( const float t, const Float3 col1, const Float3 col2 )
			{
				const float tm = 1.0f - t;
				Float3 out = col1;
				if( col2.x != 0.0f ) {
					out.x = tm * out.x + t * out.x / col2.x;
				}
				if( col2.y != 0.0f ) {
					out.y = tm * out.y + t * out.y / col2.y;
				}
				if( col2.z != 0.0f ) {
					out.z = tm * out.z + t * out.z / col2.z;
				}
				return out;
			}

			//! svm_mix_diff
			Float3 MixDiff( const float t, const Float3 col1, const Float3 col2 )
			{
				return Interp3( col1, MakeFloat3(
					std::fabs( col1.x - col2.x ),
					std::fabs( col1.y - col2.y ),
					std::fabs( col1.z - col2.z ) ), t );
			}

			//! svm_mix_exclusion
			Float3 MixExclusion( const float t, const Float3 col1, const Float3 col2 )
			{
				const Float3 target = MakeFloat3(
					col1.x + col2.x - 2.0f * col1.x * col2.x,
					col1.y + col2.y - 2.0f * col1.y * col2.y,
					col1.z + col2.z - 2.0f * col1.z * col2.z );
				const Float3 r = Interp3( col1, target, t );
				return MakeFloat3( MaxF( r.x, 0.0f ), MaxF( r.y, 0.0f ), MaxF( r.z, 0.0f ) );
			}

			//! svm_mix_dark
			Float3 MixDark( const float t, const Float3 col1, const Float3 col2 )
			{
				return Interp3( col1, MakeFloat3(
					MinF( col1.x, col2.x ),
					MinF( col1.y, col2.y ),
					MinF( col1.z, col2.z ) ), t );
			}

			//! svm_mix_light
			Float3 MixLight( const float t, const Float3 col1, const Float3 col2 )
			{
				return Interp3( col1, MakeFloat3(
					MaxF( col1.x, col2.x ),
					MaxF( col1.y, col2.y ),
					MaxF( col1.z, col2.z ) ), t );
			}

			//! svm_mix_dodge (one channel)
			inline float MixDodgeCh( const float t, const float c1, const float c2 )
			{
				if( c1 != 0.0f ) {
					float tmp = 1.0f - t * c2;
					if( tmp <= 0.0f ) {
						return 1.0f;
					}
					tmp = c1 / tmp;
					if( tmp > 1.0f ) {
						return 1.0f;
					}
					return tmp;
				}
				return c1;
			}

			//! svm_mix_dodge
			Float3 MixDodge( const float t, const Float3 col1, const Float3 col2 )
			{
				return MakeFloat3(
					MixDodgeCh( t, col1.x, col2.x ),
					MixDodgeCh( t, col1.y, col2.y ),
					MixDodgeCh( t, col1.z, col2.z ) );
			}

			//! svm_mix_burn (one channel)
			inline float MixBurnCh( const float t, const float tm, const float c1, const float c2 )
			{
				float tmp = tm + t * c2;
				if( tmp <= 0.0f ) {
					return 0.0f;
				}
				tmp = 1.0f - ( 1.0f - c1 ) / tmp;
				if( tmp < 0.0f ) {
					return 0.0f;
				}
				if( tmp > 1.0f ) {
					return 1.0f;
				}
				return tmp;
			}

			//! svm_mix_burn
			Float3 MixBurn( const float t, const Float3 col1, const Float3 col2 )
			{
				const float tm = 1.0f - t;
				return MakeFloat3(
					MixBurnCh( t, tm, col1.x, col2.x ),
					MixBurnCh( t, tm, col1.y, col2.y ),
					MixBurnCh( t, tm, col1.z, col2.z ) );
			}

			//! svm_mix_hue
			Float3 MixHue( const float t, const Float3 col1, const Float3 col2 )
			{
				Float3 out = col1;
				const Float3 hsv2 = RgbToHsvF( col2 );
				if( hsv2.y != 0.0f ) {
					Float3 hsv = RgbToHsvF( out );
					hsv.x = hsv2.x;
					const Float3 tmp = HsvToRgbF( hsv );
					out = Interp3( out, tmp, t );
				}
				return out;
			}

			//! svm_mix_sat
			Float3 MixSat( const float t, const Float3 col1, const Float3 col2 )
			{
				const float tm = 1.0f - t;
				Float3 out = col1;
				Float3 hsv = RgbToHsvF( out );
				if( hsv.y != 0.0f ) {
					const Float3 hsv2 = RgbToHsvF( col2 );
					hsv.y = tm * hsv.y + t * hsv2.y;
					out = HsvToRgbF( hsv );
				}
				return out;
			}

			//! svm_mix_val
			Float3 MixVal( const float t, const Float3 col1, const Float3 col2 )
			{
				const float tm = 1.0f - t;
				Float3 hsv = RgbToHsvF( col1 );
				const Float3 hsv2 = RgbToHsvF( col2 );
				hsv.z = tm * hsv.z + t * hsv2.z;
				return HsvToRgbF( hsv );
			}

			//! svm_mix_color
			Float3 MixColorMode( const float t, const Float3 col1, const Float3 col2 )
			{
				Float3 out = col1;
				const Float3 hsv2 = RgbToHsvF( col2 );
				if( hsv2.y != 0.0f ) {
					Float3 hsv = RgbToHsvF( out );
					hsv.x = hsv2.x;
					hsv.y = hsv2.y;
					const Float3 tmp = HsvToRgbF( hsv );
					out = Interp3( out, tmp, t );
				}
				return out;
			}

			//! svm_mix_soft
			Float3 MixSoft( const float t, const Float3 col1, const Float3 col2 )
			{
				const float tm = 1.0f - t;
				const float scrx = 1.0f - ( 1.0f - col2.x ) * ( 1.0f - col1.x );
				const float scry = 1.0f - ( 1.0f - col2.y ) * ( 1.0f - col1.y );
				const float scrz = 1.0f - ( 1.0f - col2.z ) * ( 1.0f - col1.z );
				return MakeFloat3(
					tm * col1.x + t * ( ( 1.0f - col1.x ) * col2.x * col1.x + col1.x * scrx ),
					tm * col1.y + t * ( ( 1.0f - col1.y ) * col2.y * col1.y + col1.y * scry ),
					tm * col1.z + t * ( ( 1.0f - col1.z ) * col2.z * col1.z + col1.z * scrz ) );
			}

			//! svm_mix_linear
			Float3 MixLinear( const float t, const Float3 col1, const Float3 col2 )
			{
				return MakeFloat3(
					col1.x + t * ( 2.0f * col2.x + (-1.0f) ),
					col1.y + t * ( 2.0f * col2.y + (-1.0f) ),
					col1.z + t * ( 2.0f * col2.z + (-1.0f) ) );
			}
		}

		void RgbToHsv( const Scalar rgb[3], Scalar out[3] )
		{
			const Float3 r = RgbToHsvF( MakeFloat3( float( rgb[0] ), float( rgb[1] ), float( rgb[2] ) ) );
			out[0] = Scalar( r.x );
			out[1] = Scalar( r.y );
			out[2] = Scalar( r.z );
		}

		void HsvToRgb( const Scalar hsv[3], Scalar out[3] )
		{
			const Float3 r = HsvToRgbF( MakeFloat3( float( hsv[0] ), float( hsv[1] ), float( hsv[2] ) ) );
			out[0] = Scalar( r.x );
			out[1] = Scalar( r.y );
			out[2] = Scalar( r.z );
		}

		void RgbToHsl( const Scalar rgb[3], Scalar out[3] )
		{
			const Float3 r = RgbToHslF( MakeFloat3( float( rgb[0] ), float( rgb[1] ), float( rgb[2] ) ) );
			out[0] = Scalar( r.x );
			out[1] = Scalar( r.y );
			out[2] = Scalar( r.z );
		}

		void HslToRgb( const Scalar hsl[3], Scalar out[3] )
		{
			const Float3 r = HslToRgbF( MakeFloat3( float( hsl[0] ), float( hsl[1] ), float( hsl[2] ) ) );
			out[0] = Scalar( r.x );
			out[1] = Scalar( r.y );
			out[2] = Scalar( r.z );
		}

		void MixColor( const int mode, const Scalar t, const Scalar a[3], const Scalar b[3],
			const bool clampFactor, const bool clampResult, Scalar out[3] )
		{
			if( mode < 0 || mode >= kMixModeCount ) {
				out[0] = Scalar( 0 );
				out[1] = Scalar( 0 );
				out[2] = Scalar( 0 );
				return;
			}

			float ft = float( t );
			if( clampFactor ) {
				ft = SaturateF( ft );
			}
			const Float3 fa = MakeFloat3( float( a[0] ), float( a[1] ), float( a[2] ) );
			const Float3 fb = MakeFloat3( float( b[0] ), float( b[1] ), float( b[2] ) );

			Float3 r;
			switch( mode ) {
			case eBlend:     r = MixBlend( ft, fa, fb );     break;
			case eAdd:       r = MixAdd( ft, fa, fb );       break;
			case eMul:       r = MixMul( ft, fa, fb );       break;
			case eSub:       r = MixSub( ft, fa, fb );       break;
			case eScreen:    r = MixScreen( ft, fa, fb );    break;
			case eDiv:       r = MixDiv( ft, fa, fb );       break;
			case eDiff:      r = MixDiff( ft, fa, fb );      break;
			case eDark:      r = MixDark( ft, fa, fb );      break;
			case eLight:     r = MixLight( ft, fa, fb );     break;
			case eOverlay:   r = MixOverlay( ft, fa, fb );   break;
			case eDodge:     r = MixDodge( ft, fa, fb );     break;
			case eBurn:      r = MixBurn( ft, fa, fb );      break;
			case eHue:       r = MixHue( ft, fa, fb );       break;
			case eSat:       r = MixSat( ft, fa, fb );       break;
			case eVal:       r = MixVal( ft, fa, fb );       break;
			case eCol:       r = MixColorMode( ft, fa, fb ); break;
			case eSoft:      r = MixSoft( ft, fa, fb );      break;
			case eLinear:    r = MixLinear( ft, fa, fb );    break;
			case eExclusion: r = MixExclusion( ft, fa, fb ); break;
			default:         r = MakeFloat3( 0.0f, 0.0f, 0.0f ); break;
			}

			if( clampResult ) {
				r = Saturate3( r );
			}
			out[0] = Scalar( r.x );
			out[1] = Scalar( r.y );
			out[2] = Scalar( r.z );
		}
	}
}
