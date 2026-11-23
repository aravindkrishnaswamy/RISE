//////////////////////////////////////////////////////////////////////
//
//  ExpressionOslBuiltins.cpp - Signature table and dispatch for the
//    expression VM's `osl_` builtins.  See ExpressionOslBuiltins.h.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ExpressionOslBuiltins.h"
#include "../Utilities/OslNoise.h"
#include "../Utilities/OslVoronoi.h"
#include "../Utilities/OslColor.h"
#include "../Utilities/FiniteMath.h"

namespace RISE
{
	namespace Implementation
	{
		namespace OslBuiltins
		{
			namespace
			{
				enum Id
				{
					kIdSNoise = 1,
					kIdFractal,
					kIdNoiseTexture,
					kIdNoiseTextureColor,
					kIdWhiteNoise,
					kIdWhiteNoiseColor,
					kIdVoronoiDistance,
					kIdVoronoiColor,
					kIdVoronoiPosition,
					kIdVoronoiW,
					kIdVoronoiRadius,
					kIdRgb2Hsv,
					kIdHsv2Rgb,
					kIdRgb2Hsl,
					kIdHsl2Rgb,
					kIdMixColor
				};

				const ArgKind S = kScalarArg, V = kVec3Arg, L = kIntLiteralArg;

				// Per argument: its kind, then the literal range (litMin,
				// litMax) -- 0..0 for a non-literal slot, which nothing reads.
				// dims is 1..4, type 0..4 (OslNoise::FractalType), normalize
				// 0..1, feature 0..4 (OslVoronoi::Feature), metric 0..3
				// (OslVoronoi::Metric), mode 0..18 (OslColor::MixMode),
				// clamp_factor/clamp_result 0..1.
				const Sig kTable[] = {
					{ "osl_snoise", kIdSNoise, 3,
						{ V, S, L },
						{ 0, 0, 1 },
						{ 0, 0, 4 },
						false, true },
					{ "osl_fractal", kIdFractal, 10,
						{ V, S, L, L, S, S, S, S, S, L },
						{ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
						{ 0, 0, 4, OslNoise::kFractalTypeCount - 1, 0, 0, 0, 0, 0, 1 },
						false, true },
					{ "osl_noise_texture", kIdNoiseTexture, 11,
						{ V, S, L, L, S, S, S, S, S, S, L },
						{ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
						{ 0, 0, 4, OslNoise::kFractalTypeCount - 1, 0, 0, 0, 0, 0, 0, 1 },
						false, true },
					{ "osl_noise_texture_color", kIdNoiseTextureColor, 11,
						{ V, S, L, L, S, S, S, S, S, S, L },
						{ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
						{ 0, 0, 4, OslNoise::kFractalTypeCount - 1, 0, 0, 0, 0, 0, 0, 1 },
						true, true },
					// A few integer hashes, judged too cheap to be worth a
					// memo lookup (not measured; cellhash is treated the same).
					{ "osl_white_noise", kIdWhiteNoise, 3,
						{ V, S, L },
						{ 0, 0, 1 },
						{ 0, 0, 4 },
						false, false },
					{ "osl_white_noise_color", kIdWhiteNoiseColor, 3,
						{ V, S, L },
						{ 0, 0, 1 },
						{ 0, 0, 4 },
						true, false },
					// The Voronoi node's five outputs: (p, w, dims, feature,
					// metric, scale, detail, roughness, lacunarity, smoothness,
					// exponent, randomness, normalize).  Each runs the whole
					// node (one or two cell searches per octave); memo-worthy.
#define OSL_VORONOI_SIG( name, id, vec3 ) \
					{ name, id, 13, \
						{ V, S, L, L, L, S, S, S, S, S, S, S, L }, \
						{ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, \
						{ 0, 0, 4, OslVoronoi::kFeatureCount - 1, OslVoronoi::kMetricCount - 1, 0, 0, 0, 0, 0, 0, 0, 1 }, \
						vec3, true }
					OSL_VORONOI_SIG( "osl_voronoi_distance", kIdVoronoiDistance, false ),
					OSL_VORONOI_SIG( "osl_voronoi_color", kIdVoronoiColor, true ),
					OSL_VORONOI_SIG( "osl_voronoi_position", kIdVoronoiPosition, true ),
					OSL_VORONOI_SIG( "osl_voronoi_w", kIdVoronoiW, false ),
					OSL_VORONOI_SIG( "osl_voronoi_radius", kIdVoronoiRadius, false ),
#undef OSL_VORONOI_SIG
					// Colour conversions and Mix Color: straight arithmetic
					// with no loops or hashes, too cheap to be worth a memo
					// lookup (see OslColor.h, COST).
					{ "osl_rgb2hsv", kIdRgb2Hsv, 1,
						{ V }, { 0 }, { 0 }, true, false },
					{ "osl_hsv2rgb", kIdHsv2Rgb, 1,
						{ V }, { 0 }, { 0 }, true, false },
					{ "osl_rgb2hsl", kIdRgb2Hsl, 1,
						{ V }, { 0 }, { 0 }, true, false },
					{ "osl_hsl2rgb", kIdHsl2Rgb, 1,
						{ V }, { 0 }, { 0 }, true, false },
					// (mode, t, a, b, clamp_factor, clamp_result)
					{ "osl_mix_color", kIdMixColor, 6,
						{ L, S, V, V, L, L },
						{ 0, 0, 0, 0, 0, 0 },
						{ OslColor::kMixModeCount - 1, 0, 0, 0, 1, 1 },
						true, false },
				};

				const int kTableSize = (int)( sizeof( kTable ) / sizeof( kTable[0] ) );

				//! The coordinates `dims` names, from the (p.x, p.y, p.z, w)
				//! the call pushed.  See the header's table.
				void Gather( const Scalar* a, const int dims, Scalar* c )
				{
					c[0] = c[1] = c[2] = c[3] = Scalar(0);
					switch( dims ) {
					case 1: c[0] = a[3]; break;
					case 2: c[0] = a[0]; c[1] = a[1]; break;
					case 3: c[0] = a[0]; c[1] = a[1]; c[2] = a[2]; break;
					case 4: c[0] = a[0]; c[1] = a[1]; c[2] = a[2]; c[3] = a[3]; break;
					default: break;
					}
				}

				//! A literal argument as the int the compiler proved it is.
				//! The range was checked at compile time; this rounds rather
				//! than truncates only so a value that is not exactly an
				//! integer (impossible from the compiler) still lands on the
				//! nearest one instead of drifting down.
				inline int Lit( const Scalar x )
				{
					return (int)( x + ( x >= Scalar(0) ? Scalar(0.5) : Scalar(-0.5) ) );
				}

				inline Scalar Finite( const Scalar x )
				{
					return IsFiniteDouble( x ) ? x : Scalar(0);
				}
			}

			const Sig* Find( const std::string& name )
			{
				for( int i = 0; i < kTableSize; ++i ) {
					if( name == kTable[i].name ) return &kTable[i];
				}
				return 0;
			}

			const Sig* FindById( const int id )
			{
				for( int i = 0; i < kTableSize; ++i ) {
					if( kTable[i].id == id ) return &kTable[i];
				}
				return 0;
			}

			int ScalarArity( const Sig& s )
			{
				int n = 0;
				for( int i = 0; i < s.nArgs; ++i ) n += ( s.kind[i] == kVec3Arg ) ? 3 : 1;
				return n;
			}

			void Call( const int id, const Scalar* a, Scalar* out )
			{
				switch( id )
				{
				case kIdSNoise:
				{
					// (p, w, dims)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					out[0] = Finite( OslNoise::SNoise( dims, c ) );
					return;
				}
				case kIdFractal:
				{
					// (p, w, dims, type, detail, roughness, lacunarity, offset, gain, normalize)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					out[0] = Finite( OslNoise::Fractal( dims, c, Lit( a[5] ),
						a[6], a[7], a[8], a[9], a[10], Lit( a[11] ) != 0 ) );
					return;
				}
				case kIdNoiseTexture:
				case kIdNoiseTextureColor:
				{
					// (p, w, dims, type, detail, roughness, lacunarity, offset, gain, distortion, normalize)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					Scalar fac = Scalar(0);
					Scalar color[3] = { Scalar(0), Scalar(0), Scalar(0) };
					const bool wantColor = ( id == kIdNoiseTextureColor );
					OslNoise::NoiseTexture( dims, c, Lit( a[5] ),
						a[6], a[7], a[8], a[9], a[10], a[11], Lit( a[12] ) != 0,
						fac, wantColor ? color : 0 );
					if( wantColor ) {
						out[0] = Finite( color[0] );
						out[1] = Finite( color[1] );
						out[2] = Finite( color[2] );
					} else {
						out[0] = Finite( fac );
					}
					return;
				}
				case kIdWhiteNoise:
				case kIdWhiteNoiseColor:
				{
					// (p, w, dims)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					Scalar value = Scalar(0);
					Scalar color[3] = { Scalar(0), Scalar(0), Scalar(0) };
					const bool wantColor = ( id == kIdWhiteNoiseColor );
					OslNoise::WhiteNoise( dims, c, value, wantColor ? color : 0 );
					if( wantColor ) {
						out[0] = color[0]; out[1] = color[1]; out[2] = color[2];
					} else {
						out[0] = value;
					}
					return;
				}
				case kIdVoronoiDistance:
				case kIdVoronoiColor:
				case kIdVoronoiPosition:
				case kIdVoronoiW:
				case kIdVoronoiRadius:
				{
					// (p, w, dims, feature, metric, scale, detail, roughness,
					//  lacunarity, smoothness, exponent, randomness, normalize)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					OslVoronoi::Result r;
					OslVoronoi::VoronoiTexture( dims, c, Lit( a[5] ), Lit( a[6] ),
						a[7], a[8], a[9], a[10], a[11], a[12], a[13], Lit( a[14] ) != 0, r );
					out[0] = out[1] = out[2] = Scalar(0);
					switch( id ) {
					case kIdVoronoiDistance: out[0] = Finite( r.distance ); break;
					case kIdVoronoiColor:
						out[0] = Finite( r.color[0] ); out[1] = Finite( r.color[1] ); out[2] = Finite( r.color[2] );
						break;
					case kIdVoronoiPosition:
						out[0] = Finite( r.position[0] ); out[1] = Finite( r.position[1] ); out[2] = Finite( r.position[2] );
						break;
					case kIdVoronoiW: out[0] = Finite( r.w ); break;
					default: out[0] = Finite( r.radius ); break;
					}
					return;
				}
				case kIdRgb2Hsv:
				case kIdHsv2Rgb:
				case kIdRgb2Hsl:
				case kIdHsl2Rgb:
				{
					// (c) -> a[0..2]
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					switch( id ) {
					case kIdRgb2Hsv: OslColor::RgbToHsv( a, r ); break;
					case kIdHsv2Rgb: OslColor::HsvToRgb( a, r ); break;
					case kIdRgb2Hsl: OslColor::RgbToHsl( a, r ); break;
					default:         OslColor::HslToRgb( a, r ); break;
					}
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				case kIdMixColor:
				{
					// (mode, t, a, b, clamp_factor, clamp_result)
					// popped scalars: mode=a[0], t=a[1], a=a[2..4], b=a[5..7],
					// clamp_factor=a[8], clamp_result=a[9].
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					OslColor::MixColor( Lit( a[0] ), a[1], a + 2, a + 5,
						Lit( a[8] ) != 0, Lit( a[9] ) != 0, r );
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				default:
					// An id with no case (none is emitted: the parser only
					// emits table ids).  Zero all three, so a vec3 call
					// site never reads an unwritten value.
					out[0] = out[1] = out[2] = Scalar(0);
					return;
				}
			}
		}
	}
}
