//////////////////////////////////////////////////////////////////////
//
//  HairMedullaProfile.cpp - Interpolation and exact importance
//    sampling of the baked medulla scattering profiles.  See
//    HairMedullaProfile.h for what the table holds and
//    tools/HairMedullaProfileGen.cpp for how it was produced.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HairMedullaProfile.h"
#include "../Utilities/FiniteMath.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	inline Scalar Clamp( const Scalar x, const Scalar lo, const Scalar hi )
	{
		return x < lo ? lo : ( x > hi ? hi : x );
	}

	//! Wrap into [-PI, PI).  Bounded loop: callers pass a value that
	//! is already the difference of two bounded angles.
	inline Scalar WrapPi( Scalar x )
	{
		if( !RISE::IsFiniteDouble( x ) ) {
			return 0;
		}
		while( x >= PI )  { x -= TWO_PI; }
		while( x < -PI )  { x += TWO_PI; }
		return x;
	}

	//! The eight-corner trilinear stencil over (b, tau, g), resolved
	//! once and shared by both public entry points so they cannot
	//! disagree about which cells they read.
	struct Stencil
	{
		size_t	cellBase[8];	//!< float offset of each corner cell
		Scalar	weight[8];
		bool	mirrored;
	};

	//! `bSigned` in [-1, 1]; `tau` and `g` are clamped to the table.
	void BuildStencil( const Scalar bSigned, const Scalar tau, const Scalar g, Stencil& st )
	{
		st.mirrored = ( bSigned < 0 );

		const unsigned int nB   = kHairMedullaNumB;
		const unsigned int nT   = kHairMedullaNumTau;
		const unsigned int nG   = kHairMedullaNumG;
		const unsigned int perCell = kHairMedullaPhiBins + 1u;

		// --- b: uniform on [0, 1] -------------------------------------
		const Scalar bAbs = Clamp( fabs( bSigned ), 0.0, 1.0 );
		const Scalar bPos = bAbs * (Scalar)( nB - 1u );
		unsigned int ib0 = (unsigned int)bPos;
		if( ib0 >= nB - 1u ) { ib0 = nB - 2u; }
		const Scalar fb = Clamp( bPos - (Scalar)ib0, 0.0, 1.0 );

		// --- tau: GEOMETRIC on [min, max] -----------------------------
		// Clamping (rather than extrapolating) is deliberate: below
		// tau_min the scattered lobe carries ~no energy, and above
		// tau_max the profile has converged to the diffusive limit.
		const Scalar tauC = Clamp( tau, (Scalar)kHairMedullaTauMin, (Scalar)kHairMedullaTauMax );
		const Scalar logSpan = log( (Scalar)kHairMedullaTauMax / (Scalar)kHairMedullaTauMin );
		const Scalar tPos = ( logSpan > 0 )
			? ( log( tauC / (Scalar)kHairMedullaTauMin ) / logSpan ) * (Scalar)( nT - 1u )
			: Scalar( 0 );
		unsigned int it0 = (unsigned int)Clamp( tPos, 0.0, (Scalar)( nT - 2u ) );
		if( it0 >= nT - 1u ) { it0 = nT - 2u; }
		const Scalar ft = Clamp( tPos - (Scalar)it0, 0.0, 1.0 );

		// --- g: uniform on [-gMax, +gMax] -----------------------------
		const Scalar gC = Clamp( g, -(Scalar)kHairMedullaGMax, (Scalar)kHairMedullaGMax );
		const Scalar gPos = ( gC + (Scalar)kHairMedullaGMax ) /
		                    ( 2 * (Scalar)kHairMedullaGMax ) * (Scalar)( nG - 1u );
		unsigned int ig0 = (unsigned int)Clamp( gPos, 0.0, (Scalar)( nG - 2u ) );
		if( ig0 >= nG - 1u ) { ig0 = nG - 2u; }
		const Scalar fg = Clamp( gPos - (Scalar)ig0, 0.0, 1.0 );

		int n = 0;
		for( unsigned int db = 0; db < 2; db++ ) {
			const Scalar wb = db ? fb : ( 1 - fb );
			for( unsigned int dt = 0; dt < 2; dt++ ) {
				const Scalar wt = dt ? ft : ( 1 - ft );
				for( unsigned int dg = 0; dg < 2; dg++ ) {
					const Scalar wg = dg ? fg : ( 1 - fg );
					const size_t cell = ( ( (size_t)( ib0 + db ) * nT + ( it0 + dt ) ) * nG + ( ig0 + dg ) );
					st.cellBase[n] = cell * perCell;
					st.weight[n]   = wb * wt * wg;
					n++;
				}
			}
		}
	}
}

namespace RISE
{
	namespace Implementation
	{
		Scalar HairMedullaProfile::Eval( Scalar dphi ) const
		{
			if( nBins == 0 ) {
				return 1.0 / TWO_PI;
			}
			if( mirrored ) { dphi = -dphi; }
			dphi = WrapPi( dphi );

			const Scalar w = TWO_PI / (Scalar)nBins;
			// Node i sits at the CENTRE of bin i: -PI + (i + 0.5) * w.
			const Scalar x = ( dphi + PI ) / w - 0.5;
			Scalar fi = floor( x );
			const Scalar frac = x - fi;
			int i0 = (int)fi;
			// Periodic wrap -- the segment from node nBins-1 to node 0
			// crosses the +/-PI seam and is a real segment like any other.
			i0 = ( ( i0 % (int)nBins ) + (int)nBins ) % (int)nBins;
			const int i1 = ( i0 + 1 ) % (int)nBins;

			const Scalar v = bins[i0] + frac * ( bins[i1] - bins[i0] );
			return ( v > 0 && RISE::IsFiniteDouble( v ) ) ? v : Scalar( 0 );
		}

		Scalar HairMedullaProfile::Sample( const Scalar u ) const
		{
			if( nBins == 0 ) {
				return WrapPi( ( mirrored ? -1 : 1 ) * ( -PI + TWO_PI * Clamp( u, 0.0, 1.0 - 1e-12 ) ) );
			}

			const Scalar w = TWO_PI / (Scalar)nBins;
			const Scalar target = Clamp( u, 0.0, 1.0 - 1e-12 );

			// Segment j runs from node j to node (j+1) mod nBins and has
			// area (v_j + v_j+1) / 2 * w.  The areas sum to
			// w * sum_i v_i == 1 for a normalised profile, so a plain
			// running sum against `target` is a correct inversion.
			Scalar cdf = 0;
			Scalar segStart = 0;
			Scalar segArea = 0;
			unsigned int j = 0;
			Scalar a = bins[0];
			Scalar b = bins[1 % nBins];
			bool found = false;
			for( unsigned int k = 0; k < nBins; k++ ) {
				const Scalar va = bins[k];
				const Scalar vb = bins[( k + 1 ) % nBins];
				const Scalar area = 0.5 * ( va + vb ) * w;
				if( target < cdf + area ) {
					j = k; a = va; b = vb; segStart = cdf; segArea = area;
					found = true;
					break;
				}
				cdf += area;
			}
			if( !found ) {
				// The running sum fell a rounding error short of 1 (or
				// the profile is not exactly normalised).  Land in the
				// last segment rather than off the end.
				j = nBins - 1;
				a = bins[nBins - 1];
				b = bins[0];
				segArea = 0.5 * ( a + b ) * w;
				segStart = cdf - segArea;
			}

			// Invert the linear ramp inside the chosen segment.
			const Scalar want = Clamp( target - segStart, 0.0, segArea );
			const Scalar d = b - a;
			Scalar s;
			if( fabs( d ) < 1e-12 ) {
				s = ( a > 0 ) ? ( want / a ) : ( 0.5 * w );
			} else {
				const Scalar disc = a * a + 2 * d * want / w;
				const Scalar root = ( disc > 0 ) ? sqrt( disc ) : Scalar( 0 );
				s = w * ( root - a ) / d;
			}
			if( !RISE::IsFiniteDouble( s ) ) { s = 0.5 * w; }
			s = Clamp( s, 0.0, w );

			// Node j is the centre of bin j.
			const Scalar centre = -PI + ( (Scalar)j + 0.5 ) * w;
			const Scalar dphi = WrapPi( centre + s );
			return mirrored ? -dphi : dphi;
		}

		void HairMedullaLookup( const Scalar bSigned, const Scalar tau, const Scalar g,
		                        HairMedullaProfile& out )
		{
			// The struct is fixed-capacity so the BCSDF can build it on
			// the stack; a regenerated table with more bins must bump
			// kMaxBins rather than silently truncate.
			if( kHairMedullaPhiBins > (unsigned int)HairMedullaProfile::kMaxBins ) {
				out.nBins = 0;
				out.longVariance = 0;
				out.mirrored = false;
				return;
			}

			Stencil st;
			BuildStencil( bSigned, tau, g, st );

			out.nBins = kHairMedullaPhiBins;
			out.mirrored = st.mirrored;
			out.longVariance = 0;
			for( unsigned int k = 0; k < kHairMedullaPhiBins; k++ ) {
				out.bins[k] = 0;
			}

			for( int c = 0; c < 8; c++ ) {
				const Scalar wgt = st.weight[c];
				if( !( wgt > 0 ) ) { continue; }
				const float* cell = &kHairMedullaProfileData[st.cellBase[c]];
				for( unsigned int k = 0; k < kHairMedullaPhiBins; k++ ) {
					out.bins[k] += wgt * (Scalar)cell[k];
				}
				out.longVariance += wgt * (Scalar)cell[kHairMedullaPhiBins];
			}
		}
	}
}
