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
					kIdWhiteNoiseColor
				};

				const ArgKind S = kScalarArg, V = kVec3Arg, L = kIntLiteralArg;

				// Per argument: its kind, then the literal range (litMin,
				// litMax) -- 0..0 for a non-literal slot, which nothing reads.
				// dims is 1..4, type 0..4 (OslNoise::FractalType), normalize 0..1.
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
