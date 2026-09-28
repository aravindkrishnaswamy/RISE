//////////////////////////////////////////////////////////////////////
//
//  SchlickDirectionalAlbedo.h - DL-310: the directional albedo of the
//    DL-225 bounded Schlick SPECULAR lobe, and the coupled diffuse
//    term SchlickBRDF / SchlickSPF build from it.
//
//  THE DEFECT (DL-310).  SchlickBRDF used to add Rd/pi to a specular
//  lobe whose directional albedo A(v) rises toward 1 at grazing
//  (Fresnel), so the FULL material exceeded 1 even for authored
//  Rd + rho <= 1 (Rd .9, rho .1, r .05: 1.1538 at 89 deg).
//
//  THE COUPLING.  The diffuse term is
//
//      f_D(i, o) = min( Rd, 1 - A(i), 1 - A(o) ) / pi
//
//  per channel (per lane).  It is RECIPROCAL (symmetric in i and o)
//  and BOUNDED: rho_d(i) = A_true(i) + int f_D cos <= A_true(i) +
//  (1 - A(i)) <= 1 whenever A >= A_true.  It is the additive Rd/pi
//  EXACTLY for every direction pair both of whose directions already
//  conserve energy under the additive model (Rd <= 1 - A(v)), and only
//  clips pairs where one of them does not -- the DL-225 ruling
//  applied to the diffuse term.  docs/DL178_DL212_BOUNDED_SCHLICK_WARD.md
//  "DL-310" compares it with the DL-37 product form (which moves every
//  material at every angle) and the Kulla-Conty normalisation.
//
//  THE TABLE.  A(v) = rho M0 + (1 - rho) M5 with M0 / M5 the lobe's
//  directional albedo at unit reflectance / its Fresnel moment.  The
//  table stores, per (mu = n.v, r, p) node, the MAXIMUM over the view
//  azimuth of each moment, raised so that the trilinear interpolant
//  bounds the true value from above at every probe point
//  (tools/SchlickDirectionalAlbedoGen.cpp).  A function of mu alone
//  keeps SchlickSPF::Pdf's diffuse-draw quadrature one-dimensional.
//  Outside the table: r < 1e-5 uses the proven M0 <= 1 (DL-225's bound)
//  and M5 = max(table, (1 - mu)^5) (measured within 3e-6 of an upper
//  bound at r <= 1e-6); r > 1 clamps to 1; isotropy below 1e-3 clamps
//  to 1e-3 (measured <= 7.4e-3 low, only beyond 89.99 deg at r <=
//  1e-3); isotropy > 1 folds to 1/p exactly (the azimuth maximum is
//  invariant under the quarter-turn fold).
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SCHLICK_DIRECTIONAL_ALBEDO_H
#define RISE_SCHLICK_DIRECTIONAL_ALBEDO_H

#include <algorithm>
#include <cmath>

namespace RISE
{
	namespace SchlickDirectionalAlbedo
	{
		//! MUST match tools/SchlickDirectionalAlbedoGen.cpp.
		static const int kNumMu = 33;		//!< mu_j = (j/32)^2 (node 0 = the grazing limit)
		static const int kNumR  = 31;		//!< log10 r uniform in [-5, 0]
		static const int kNumP  = 13;		//!< log10 p uniform in [-3, 0]
		static const double kLogRMin = -5.0;
		static const double kLogPMin = -3.0;

		//! [p][r][mu], row-major.
		extern const float kM0[ kNumP * kNumR * kNumMu ];
		extern const float kM5[ kNumP * kNumR * kNumMu ];
		//! [p][r]: the maximum over mu of each moment (a bound on the
		//! whole row, so a lane whose Rd <= 1 - A_top never clips).
		extern const float kTopM0[ kNumP * kNumR ];
		extern const float kTopM5[ kNumP * kNumR ];

		static_assert( sizeof( kM0 ) / sizeof( float ) == size_t( kNumP * kNumR * kNumMu ), "kM0 extent" );
		static_assert( sizeof( kTopM0 ) / sizeof( float ) == size_t( kNumP * kNumR ), "kTopM0 extent" );

		//! The (roughness, isotropy) bracket of one lane.
		struct Lane
		{
			int    base;			//!< index of the (p0, r0) row
			double fr, fp;			//!< blend fractions along r and p
			bool   specular;		//!< false: no specular lobe (A = 0)
			bool   belowR;			//!< r < 1e-5: M0 = 1, M5 >= (1-mu)^5
		};

		inline void PrepareLane( Lane& L, const double r, const double p )
		{
			L.base = 0; L.fr = L.fp = 0; L.belowR = false;
			L.specular = ( r > 0 ) && ( p > 0 ) && std::isfinite( r ) && std::isfinite( p );
			if( !L.specular ) {
				return;
			}
			const double pf = ( p > 1.0 ) ? 1.0 / p : p;
			double kr = ( std::log10( std::min( r, 1.0 ) ) - kLogRMin ) * double( kNumR - 1 ) / ( 0.0 - kLogRMin );
			if( kr < 0 ) { L.belowR = true; kr = 0; }
			double kp = ( std::log10( pf ) - kLogPMin ) * double( kNumP - 1 ) / ( 0.0 - kLogPMin );
			kp = std::max( 0.0, std::min( double( kNumP - 1 ), kp ) );
			kr = std::min( double( kNumR - 1 ), kr );
			const int r0 = std::min( int( kr ), kNumR - 2 );
			const int p0 = std::min( int( kp ), kNumP - 2 );
			L.fr = kr - r0;
			L.fp = kp - p0;
			L.base = p0 * kNumR + r0;
		}

		//! M0, M5 at cosine mu for a prepared lane (upper bounds).
		inline void Moments( const Lane& L, const double mu, double& M0, double& M5 )
		{
			if( !L.specular ) { M0 = M5 = 0; return; }
			const double m = std::max( 0.0, std::min( 1.0, mu ) );
			const double u = std::sqrt( m ) * double( kNumMu - 1 );
			const int j0 = std::min( int( u ), kNumMu - 2 );
			const double fu = u - j0;
			const int rowStride = kNumMu;
			const int pStride = kNumR * kNumMu;
			const int i00 = L.base * kNumMu + j0;
			const double w00 = (1 - L.fr) * (1 - L.fp), w10 = L.fr * (1 - L.fp);
			const double w01 = (1 - L.fr) * L.fp, w11 = L.fr * L.fp;
			double a0 = 0, a5 = 0;
			for( int dj = 0; dj < 2; ++dj ) {
				const double wu = dj ? fu : 1.0 - fu;
				const int i = i00 + dj;
				a0 += wu * ( w00 * kM0[i] + w10 * kM0[i + rowStride] + w01 * kM0[i + pStride] + w11 * kM0[i + pStride + rowStride] );
				a5 += wu * ( w00 * kM5[i] + w10 * kM5[i + rowStride] + w01 * kM5[i + pStride] + w11 * kM5[i + pStride + rowStride] );
			}
			if( L.belowR ) {
				const double f = 1.0 - m, f2 = f * f;
				a0 = 1.0;
				a5 = std::max( a5, f2 * f2 * f );
			}
			M0 = a0;
			M5 = a5;
		}

		//! The maximum over mu of each moment for a prepared lane.
		inline void TopMoments( const Lane& L, double& M0, double& M5 )
		{
			if( !L.specular ) { M0 = M5 = 0; return; }
			if( L.belowR ) { M0 = M5 = 1.0; return; }
			const int i = L.base;
			const double w00 = (1 - L.fr) * (1 - L.fp), w10 = L.fr * (1 - L.fp);
			const double w01 = (1 - L.fr) * L.fp, w11 = L.fr * L.fp;
			M0 = w00 * kTopM0[i] + w10 * kTopM0[i + 1] + w01 * kTopM0[i + kNumR] + w11 * kTopM0[i + kNumR + 1];
			M5 = w00 * kTopM5[i] + w10 * kTopM5[i + 1] + w01 * kTopM5[i + kNumR] + w11 * kTopM5[i + kNumR + 1];
		}

		//! A = rho M0 + (1 - rho) M5, for a reflectance rho in [0, 1]
		//! (clamped: the table bounds only that range).
		inline double Albedo( const double rho, const double M0, const double M5 )
		{
			const double c = std::max( 0.0, std::min( 1.0, rho ) );
			return c * M0 + ( 1.0 - c ) * M5;
		}

		//! The coupled diffuse reflectance min(Rd, 1 - A(i), 1 - A(o)),
		//! never negative.  f_D = this / pi; the SPF's cosine-sampled
		//! diffuse `kray` IS this.
		inline double CoupledDiffuse( const double rd, const double Ai, const double Ao )
		{
			return std::max( 0.0, std::min( rd, std::min( 1.0 - Ai, 1.0 - Ao ) ) );
		}
	}
}

#endif
