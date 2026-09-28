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

		//! True when the lane can clip SOME direction pair: Rd > 1 - A_top.
		//! When false the coupled term is Rd exactly, for every pair.
		inline bool CanClip( const Lane& L, const double rho, const double rd )
		{
			double t0, t5;
			TopMoments( L, t0, t5 );
			return rd > 1.0 - Albedo( rho, t0, t5 );
		}

		//! THE coupled diffuse reflectance of one channel for the pair
		//! (mu_i, mu_o).  Every consumer -- SchlickBRDF::value / valueNM,
		//! SchlickSPF's diffuse kray, its Pdf's C_D weight and
		//! EvaluateKrayNM / EvaluateLobeFNM -- goes through this one
		//! function, so they cannot drift.  Returns `rd` itself (bit for
		//! bit the pre-DL-310 additive term) whenever the lane cannot clip.
		inline double CoupledDiffuseAt( const Lane& L, const double rho, const double rd, const double muI, const double muO )
		{
			if( !CanClip( L, rho, rd ) ) {
				return rd;
			}
			double a0, a5, b0, b5;
			Moments( L, muI, a0, a5 );
			Moments( L, muO, b0, b5 );
			return CoupledDiffuse( rd, Albedo( rho, a0, a5 ), Albedo( rho, b0, b5 ) );
		}

		//! One lane's moment row at the mu nodes (the (r, p) bilinear
		//! blend done once), for the Pdf / albedo band quadrature.
		struct LaneRow
		{
			double M0[ kNumMu ];
			double M5[ kNumMu ];
			bool   belowR;
		};

		inline void BuildRow( const Lane& L, LaneRow& row )
		{
			row.belowR = L.belowR;
			if( !L.specular ) {
				for( int j = 0; j < kNumMu; ++j ) { row.M0[j] = row.M5[j] = 0; }
				return;
			}
			const int pStride = kNumR * kNumMu;
			const int i0 = L.base * kNumMu;
			const double w00 = (1 - L.fr) * (1 - L.fp), w10 = L.fr * (1 - L.fp);
			const double w01 = (1 - L.fr) * L.fp, w11 = L.fr * L.fp;
			for( int j = 0; j < kNumMu; ++j ) {
				const int i = i0 + j;
				row.M0[j] = w00 * kM0[i] + w10 * kM0[i + kNumMu] + w01 * kM0[i + pStride] + w11 * kM0[i + pStride + kNumMu];
				row.M5[j] = w00 * kM5[i] + w10 * kM5[i + kNumMu] + w01 * kM5[i + pStride] + w11 * kM5[i + pStride + kNumMu];
			}
		}

		//! A at table coordinate u = sqrt(mu) (kNumMu - 1): the SAME
		//! multilinear interpolant Moments() evaluates (blended in the
		//! other order), including its r < 1e-5 branch.
		inline double RowAlbedo( const LaneRow& row, const double rho, const double u )
		{
			const int j0 = std::max( 0, std::min( int( u ), kNumMu - 2 ) );
			const double fu = std::max( 0.0, std::min( 1.0, u - j0 ) );
			double m0 = (1 - fu) * row.M0[j0] + fu * row.M0[j0 + 1];
			double m5 = (1 - fu) * row.M5[j0] + fu * row.M5[j0 + 1];
			if( row.belowR ) {
				const double x = u / double( kNumMu - 1 );
				const double f = 1.0 - x * x, f2 = f * f;
				m0 = 1.0;
				m5 = std::max( m5, f2 * f2 * f );
			}
			return Albedo( rho, m0, m5 );
		}

		//! Probability that a cosine-sampled diffuse direction at cosine
		//! `mu` (azimuth uniform) passes the geometric-horizon gate
		//! mu gz + sin(theta) gt cos(phi - phi_g) > 0, where (gt, gz) are
		//! the tangential length and normal component of the
		//! (ray-anchored) geometric normal in the sampling frame.
		//! Integrates over s = 1 - mu^2 to (1 + gz)/2 -- the closed form
		//! SchlickDiffuseAcceptFraction uses.
		inline double AcceptFraction( const double mu, const double gz, const double gt )
		{
			const double st = std::sqrt( std::max( 0.0, 1.0 - mu * mu ) );
			if( gt <= 0 || st <= 0 ) {
				return ( mu * gz > 0 ) ? 1.0 : 0.0;
			}
			const double k = -mu * gz / ( st * gt );
			if( k <= -1.0 ) return 1.0;
			if( k >= 1.0 ) return 0.0;
			return std::acos( k ) / 3.14159265358979323846;
		}

		//! The diffuse channels of one shading point, for the quadrature
		//! over the DIFFUSE draw that SchlickSPF::Pdf's specular
		//! coefficient and SchlickBRDF::albedo need once the diffuse
		//! weight depends on the drawn direction.
		struct DiffuseChannels
		{
			int     count;			//!< 3 (RGB) or 1 (NM)
			bool    active[3];		//!< the channel can clip
			double  K[3];			//!< min(Rd, 1 - A(mu_i)), >= 0 (active) / Rd (inactive)
			double  rho[3];
			const LaneRow* row[3];
		};

		//! W_c at table coordinate u.
		inline void ChannelWeights( const DiffuseChannels& d, const double u, double W[3] )
		{
			for( int c = 0; c < d.count; ++c ) {
				W[c] = d.active[c]
					? std::max( 0.0, std::min( d.K[c], 1.0 - RowAlbedo( *d.row[c], d.rho[c], u ) ) )
					: d.K[c];
			}
		}

		//! The nodes of a deterministic quadrature of
		//!     int_0^1 ds P(mu(s)) [ G(W(mu(s))) - G(W0) ],   mu = sqrt(1 - s),
		//! over the band where some channel clips (outside it W == W0 and
		//! the integrand vanishes identically).  In the table coordinate
		//! u = sqrt(mu) (kNumMu - 1) every channel's A is linear between
		//! nodes, so each table interval that reaches a clip is split at
		//! every channel's exact crossing and at the gate's kink, and each
		//! piece gets 3-point Gauss-Legendre (ds = 4 u^3 / 32^4 du).  The
		//! caller evaluates G at `nodeW` and weights by `nodeWeight`.
		struct BandNodes
		{
			static const int kMax = ( kNumMu - 1 ) * 6 * 3;
			int    n;
			double weight[ kMax ];
			double W[ kMax ][ 3 ];
			double W0[ 3 ];
		};

		inline void BuildBand( const DiffuseChannels& d, const double gz, const double gt, BandNodes& b )
		{
			b.n = 0;
			for( int c = 0; c < 3; ++c ) b.W0[c] = ( c < d.count ) ? d.K[c] : 0.0;
			bool any = false;
			for( int c = 0; c < d.count; ++c ) any = any || d.active[c];
			if( !any ) return;

			static const double gx[3] = { 0.1127016653792583, 0.5, 0.8872983346207417 };
			static const double gw[3] = { 5.0 / 18.0, 8.0 / 18.0, 5.0 / 18.0 };
			const double N = double( kNumMu - 1 );
			const double N4 = N * N * N * N;
			// The gate's kink: k = -mu gz / (sin(theta) gt) crosses -1 or 1
			// at mu = gt / sqrt(gt^2 + gz^2).
			double uGate = -1;
			if( gt > 0 ) {
				const double mg = gt / std::sqrt( gt * gt + gz * gz );
				uGate = std::sqrt( mg ) * N;
			}
			for( int j = 0; j + 1 < kNumMu; ++j ) {
				double cuts[8];
				int nc = 0;
				cuts[nc++] = j;
				bool inBand = false;
				for( int c = 0; c < d.count; ++c ) {
					if( !d.active[c] ) continue;
					const double a = RowAlbedo( *d.row[c], d.rho[c], double( j ) );
					const double e = RowAlbedo( *d.row[c], d.rho[c], double( j + 1 ) );
					const double thr = 1.0 - d.K[c];
					if( std::max( a, e ) > thr ) inBand = true;
					if( ( a - thr ) * ( e - thr ) < 0 && !d.row[c]->belowR ) {
						cuts[nc++] = j + ( thr - a ) / ( e - a );
					}
				}
				if( !inBand ) continue;
				if( uGate > j && uGate < j + 1 ) cuts[nc++] = uGate;
				cuts[nc++] = j + 1;
				std::sort( cuts, cuts + nc );
				for( int k = 0; k + 1 < nc; ++k ) {
					const double lo = cuts[k], hi = cuts[k + 1];
					if( hi <= lo ) continue;
					for( int q = 0; q < 3; ++q ) {
						const double u = lo + ( hi - lo ) * gx[q];
						const double mu = ( u / N ) * ( u / N );
						const double w = gw[q] * ( hi - lo ) * 4.0 * u * u * u / N4 * AcceptFraction( mu, gz, gt );
						if( w <= 0 ) continue;
						b.weight[b.n] = w;
						ChannelWeights( d, u, b.W[b.n] );
						for( int c = d.count; c < 3; ++c ) b.W[b.n][c] = 0.0;
						b.n++;
					}
				}
			}
		}
	}
}

#endif
