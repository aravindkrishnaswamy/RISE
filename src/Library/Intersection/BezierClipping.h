//////////////////////////////////////////////////////////////////////
//
//  BezierClipping.h - Parameter-space Bezier-clipping primitives for
//                     direct analytic ray / bicubic-Bezier-patch
//                     intersection.  Header-only, included from one TU
//                     (RayBezierPatchIntersection.cpp) plus the unit test.
//
//  All functions operate on a 4x4 "distance grid" d[i][j] that holds
//  the Bernstein-basis scalar coefficients of
//        F_k(u,v) = N_k . (P(u,v) - O_k)     with (u,v) in [0,1]^2
//  for a single plane N_k.  The clipping algorithm finds parameter-space
//  sub-ranges where F_1 = F_2 = 0 simultaneously (i.e., where the patch
//  crosses both planes that define the ray).
//
//  References:
//    [Nishita 1990]  T. Nishita, T.W. Sederberg, M. Kakimoto,
//                    "Ray tracing trimmed rational surface patches",
//                    SIGGRAPH 1990, pp. 337-345.  Introduces Bezier
//                    clipping for ray-patch intersection.
//    [Efremov 2005]  A. Efremov, V. Havran, H-P. Seidel,
//                    "Robust and numerically stable Bezier clipping
//                    method for ray tracing NURBS surfaces", SCCG 2005.
//                    Tolerance treatment + split-on-slow-clip policy.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BEZIER_CLIPPING_
#define BEZIER_CLIPPING_

#include "../Utilities/Math3D/Math3D.h"

#include <cmath>
#include <algorithm>

namespace RISE {
namespace BezierClip {

// 4x4 Bernstein-basis scalar grid — one per signed-distance plane.
// Memory layout: 16 doubles = 128 bytes, stack-allocated and passed by ref.
struct DistanceGrid {
	Scalar d[4][4];                         // d[u_index][v_index]
};

// Parameter-space sub-rectangle tracking where the current DistanceGrid
// lives relative to the ORIGINAL (u,v) in [0,1]^2 patch coordinates.
// Lets us recover original (u,v) for a converged candidate.
struct ParamBox {
	Scalar u_lo, u_hi, v_lo, v_hi;

	Scalar UWidth() const { return u_hi - u_lo; }
	Scalar VWidth() const { return v_hi - v_lo; }
	Scalar Area()   const { return UWidth() * VWidth(); }

	// Map a point (u',v') in [0,1]^2 sub-patch coords back to the
	// original patch's (u,v).
	void   Map( Scalar u_sub, Scalar v_sub, Scalar& u_out, Scalar& v_out ) const
	{
		u_out = u_lo + UWidth() * u_sub;
		v_out = v_lo + VWidth() * v_sub;
	}
};

// ----- De Casteljau subdivision ------------------------------------------
//
// DeCasteljauU splits the current sub-patch in the u direction at parameter
// t in [0,1] (relative to the sub-patch's own [0,1]^2).  For each of the 4
// v-columns, runs a standard 4-control-point de Casteljau and fills in both
// halves.  Left half covers u' in [0,t]; right half covers u' in [t,1].
inline void DeCasteljauU(
	const DistanceGrid& in, const Scalar t,
	DistanceGrid& left, DistanceGrid& right )
{
	const Scalar one_t = 1.0 - t;
	for( int j = 0; j < 4; ++j ) {
		// Initial control points along u for this v column.
		const Scalar p0 = in.d[0][j];
		const Scalar p1 = in.d[1][j];
		const Scalar p2 = in.d[2][j];
		const Scalar p3 = in.d[3][j];

		// First-level de Casteljau: one lerp between consecutive points.
		const Scalar q0 = one_t*p0 + t*p1;
		const Scalar q1 = one_t*p1 + t*p2;
		const Scalar q2 = one_t*p2 + t*p3;

		// Second level.
		const Scalar r0 = one_t*q0 + t*q1;
		const Scalar r1 = one_t*q1 + t*q2;

		// Third (final) level.
		const Scalar s  = one_t*r0 + t*r1;

		// Left sub-patch control points (u' in [0,t]):  [p0, q0, r0, s]
		left.d[0][j]  = p0;
		left.d[1][j]  = q0;
		left.d[2][j]  = r0;
		left.d[3][j]  = s;

		// Right sub-patch (u' in [t,1]):  [s, r1, q2, p3]
		right.d[0][j] = s;
		right.d[1][j] = r1;
		right.d[2][j] = q2;
		right.d[3][j] = p3;
	}
}

// DeCasteljauV — symmetric along the v axis.  For each u row, run
// de Casteljau on the 4 v-columns.
inline void DeCasteljauV(
	const DistanceGrid& in, const Scalar t,
	DistanceGrid& left, DistanceGrid& right )
{
	const Scalar one_t = 1.0 - t;
	for( int i = 0; i < 4; ++i ) {
		const Scalar p0 = in.d[i][0];
		const Scalar p1 = in.d[i][1];
		const Scalar p2 = in.d[i][2];
		const Scalar p3 = in.d[i][3];

		const Scalar q0 = one_t*p0 + t*p1;
		const Scalar q1 = one_t*p1 + t*p2;
		const Scalar q2 = one_t*p2 + t*p3;
		const Scalar r0 = one_t*q0 + t*q1;
		const Scalar r1 = one_t*q1 + t*q2;
		const Scalar s  = one_t*r0 + t*r1;

		left.d[i][0]  = p0;
		left.d[i][1]  = q0;
		left.d[i][2]  = r0;
		left.d[i][3]  = s;

		right.d[i][0] = s;
		right.d[i][1] = r1;
		right.d[i][2] = q2;
		right.d[i][3] = p3;
	}
}

// ----- Sub-range extraction ----------------------------------------------
//
// ExtractSubRangeU extracts the sub-grid for u' in [u_lo, u_hi] from the
// current sub-patch grid.  Two DeCasteljauU calls with the appropriate t
// mappings: cut off the left (u < u_lo) portion, then cut the remainder at
// the rescaled u_hi to drop the right (u > u_hi) portion.
inline void ExtractSubRangeU(
	const DistanceGrid& in, const Scalar u_lo, const Scalar u_hi,
	DistanceGrid& out )
{
	if( u_lo <= 0.0 && u_hi >= 1.0 ) { out = in; return; }

	DistanceGrid tmp_left, tmp_right;
	if( u_lo > 0.0 ) {
		DeCasteljauU( in, u_lo, tmp_left, tmp_right );
		// tmp_right now covers u' in [u_lo, 1].
	} else {
		tmp_right = in;
	}

	if( u_hi < 1.0 ) {
		// Map u_hi into the rescaled range of tmp_right (which is [0,1]
		// representing original [u_lo, 1]).  Re-target parameter:
		const Scalar rescaled = (u_lo > 0.0) ? (u_hi - u_lo) / (1.0 - u_lo) : u_hi;
		DeCasteljauU( tmp_right, rescaled, out, tmp_left );
		// out covers u' in [0, rescaled] of tmp_right = original [u_lo, u_hi].
	} else {
		out = tmp_right;
	}
}

// ExtractSubRangeV — symmetric.
inline void ExtractSubRangeV(
	const DistanceGrid& in, const Scalar v_lo, const Scalar v_hi,
	DistanceGrid& out )
{
	if( v_lo <= 0.0 && v_hi >= 1.0 ) { out = in; return; }

	DistanceGrid tmp_left, tmp_right;
	if( v_lo > 0.0 ) {
		DeCasteljauV( in, v_lo, tmp_left, tmp_right );
	} else {
		tmp_right = in;
	}

	if( v_hi < 1.0 ) {
		const Scalar rescaled = (v_lo > 0.0) ? (v_hi - v_lo) / (1.0 - v_lo) : v_hi;
		DeCasteljauV( tmp_right, rescaled, out, tmp_left );
	} else {
		out = tmp_right;
	}
}

// ----- Convex-hull clip --------------------------------------------------
//
// ConvexHullClipU finds the u-interval [out_lo, out_hi] in [0,1] where the
// current sub-patch's F(u, .) could still be zero somewhere.  Uses the
// standard Bezier-clipping envelope bound:
//
//   upper_i = max_j( d[i][j] )                  // bounds F(u, .) from above
//   lower_i = min_j( d[i][j] )                  //            from below
//
//   F(u, .) >= sum_i B_i(u) * lower_i           // for every v
//   F(u, .) <= sum_i B_i(u) * upper_i           // for every v
//
// F(u, v) = 0 is possible iff F_max(u) >= 0 AND F_min(u) <= 0 simultaneously.
// These are TWO different conditions, each reducing to a 1-D hull clip:
//
//   - Range where F_max(u) could be >= -eps:   "cubic could be at least T"
//   - Range where F_min(u) could be <=  eps:   "cubic could be at most T"
//
// Each is bounded by the convex hull of the control points (i/3, y_i).
// The range where a cubic COULD be >= threshold is NOT the same as the
// range where the cubic CROSSES threshold — the latter over-prunes.  E.g.
// for y = [1, 1, -1, -1], the cubic is >= 0 on [0, 2/3] (crossing only
// at 2/3), not on [1/3, 2/3].  See detail::ClipWhereCubicCouldBeAtLeast
// for the corrected logic.
//
// Returns false if the envelopes prove the patch doesn't contain a root
// (upper entirely below -eps, or lower entirely above +eps).
// eps: pass 1e-6 * GridMaxAbs(grid) + 1e-10 for the standard tolerance.
namespace detail {

// Conservative superset of t in [0,1] where the cubic Bezier curve with
// control values y[i] at t_i = i/3 could have value >= threshold.
// Bounded by the control polygon's convex hull.
inline bool ClipWhereCubicCouldBeAtLeast(
	const Scalar y[4], const Scalar threshold,
	Scalar& out_lo, Scalar& out_hi )
{
	bool all_below = true;          // all y[i] < threshold ?
	bool all_above = true;          // all y[i] >= threshold ?
	for( int i = 0; i < 4; ++i ) {
		if( y[i] >= threshold ) all_below = false;
		if( y[i] <  threshold ) all_above = false;
	}
	if( all_below ) return false;
	if( all_above ) { out_lo = 0.0; out_hi = 1.0; return true; }

	// Leftmost/rightmost crossings of any control-polygon edge with y = threshold.
	// For cubic control points this is a superset of the hull edge crossings;
	// loose but valid.  We use these only to extend the accepted region from
	// the endpoints of [0,1] — see the y[0] / y[3] branches below.
	Scalar min_xc =  2.0;
	Scalar max_xc = -1.0;
	const Scalar x[4] = { 0.0, 1.0/3.0, 2.0/3.0, 1.0 };
	for( int a = 0; a < 4; ++a ) {
		for( int b = a + 1; b < 4; ++b ) {
			const Scalar ya = y[a] - threshold;
			const Scalar yb = y[b] - threshold;
			if( ( ya > 0.0 && yb > 0.0 ) || ( ya < 0.0 && yb < 0.0 ) ) continue;
			const Scalar denom = yb - ya;
			if( std::fabs( denom ) < 1e-30 ) continue;
			const Scalar xc = x[a] + ( x[b] - x[a] ) * ( -ya ) / denom;
			if( xc < min_xc ) min_xc = xc;
			if( xc > max_xc ) max_xc = xc;
		}
	}

	Scalar t_L, t_R;
	if( y[0] >= threshold ) {
		t_L = 0.0;
	} else {
		t_L = ( min_xc < 0.0 ) ? 0.0 : min_xc;
	}
	if( y[3] >= threshold ) {
		t_R = 1.0;
	} else {
		t_R = ( max_xc > 1.0 ) ? 1.0 : max_xc;
	}
	if( t_R < t_L ) return false;
	out_lo = t_L;
	out_hi = t_R;
	return true;
}

// Conservative superset of t where the cubic could be <= threshold.
inline bool ClipWhereCubicCouldBeAtMost(
	const Scalar y[4], const Scalar threshold,
	Scalar& out_lo, Scalar& out_hi )
{
	bool all_above = true;          // all y[i] > threshold ?
	bool all_below = true;          // all y[i] <= threshold ?
	for( int i = 0; i < 4; ++i ) {
		if( y[i] <= threshold ) all_above = false;
		if( y[i] >  threshold ) all_below = false;
	}
	if( all_above ) return false;
	if( all_below ) { out_lo = 0.0; out_hi = 1.0; return true; }

	Scalar min_xc =  2.0;
	Scalar max_xc = -1.0;
	const Scalar x[4] = { 0.0, 1.0/3.0, 2.0/3.0, 1.0 };
	for( int a = 0; a < 4; ++a ) {
		for( int b = a + 1; b < 4; ++b ) {
			const Scalar ya = y[a] - threshold;
			const Scalar yb = y[b] - threshold;
			if( ( ya > 0.0 && yb > 0.0 ) || ( ya < 0.0 && yb < 0.0 ) ) continue;
			const Scalar denom = yb - ya;
			if( std::fabs( denom ) < 1e-30 ) continue;
			const Scalar xc = x[a] + ( x[b] - x[a] ) * ( -ya ) / denom;
			if( xc < min_xc ) min_xc = xc;
			if( xc > max_xc ) max_xc = xc;
		}
	}

	Scalar t_L, t_R;
	if( y[0] <= threshold ) {
		t_L = 0.0;
	} else {
		t_L = ( min_xc < 0.0 ) ? 0.0 : min_xc;
	}
	if( y[3] <= threshold ) {
		t_R = 1.0;
	} else {
		t_R = ( max_xc > 1.0 ) ? 1.0 : max_xc;
	}
	if( t_R < t_L ) return false;
	out_lo = t_L;
	out_hi = t_R;
	return true;
}

} // namespace detail

inline bool ConvexHullClipU(
	const DistanceGrid& in, const Scalar eps,
	Scalar& out_lo, Scalar& out_hi )
{
	Scalar upper[4], lower[4];
	for( int i = 0; i < 4; ++i ) {
		Scalar mx = in.d[i][0], mn = in.d[i][0];
		for( int j = 1; j < 4; ++j ) {
			const Scalar v = in.d[i][j];
			if( v > mx ) mx = v;
			if( v < mn ) mn = v;
		}
		upper[i] = mx;
		lower[i] = mn;
	}

	Scalar loU, hiU;
	if( !detail::ClipWhereCubicCouldBeAtLeast( upper, -eps, loU, hiU ) ) return false;
	Scalar loL, hiL;
	if( !detail::ClipWhereCubicCouldBeAtMost ( lower,  eps, loL, hiL ) ) return false;

	const Scalar lo = ( loU > loL ) ? loU : loL;
	const Scalar hi = ( hiU < hiL ) ? hiU : hiL;
	if( hi < lo ) return false;

	out_lo = lo;
	out_hi = hi;
	return true;
}

// ConvexHullClipV — symmetric along v.
inline bool ConvexHullClipV(
	const DistanceGrid& in, const Scalar eps,
	Scalar& out_lo, Scalar& out_hi )
{
	Scalar upper[4], lower[4];
	for( int j = 0; j < 4; ++j ) {
		Scalar mx = in.d[0][j], mn = in.d[0][j];
		for( int i = 1; i < 4; ++i ) {
			const Scalar v = in.d[i][j];
			if( v > mx ) mx = v;
			if( v < mn ) mn = v;
		}
		upper[j] = mx;
		lower[j] = mn;
	}

	Scalar loU, hiU;
	if( !detail::ClipWhereCubicCouldBeAtLeast( upper, -eps, loU, hiU ) ) return false;
	Scalar loL, hiL;
	if( !detail::ClipWhereCubicCouldBeAtMost ( lower,  eps, loL, hiL ) ) return false;

	const Scalar lo = ( loU > loL ) ? loU : loL;
	const Scalar hi = ( hiU < hiL ) ? hiU : hiL;
	if( hi < lo ) return false;

	out_lo = lo;
	out_hi = hi;
	return true;
}

// ----- Grid utility ------------------------------------------------------
//
// Max absolute distance over the 16 grid values.  Used to build relative
// clip tolerance:   eps = 1e-6 * GridMaxAbs() + 1e-10
// Matches the tolerance recipe in ProjectPatchOntoPlane's convex-hull
// pre-filter, so we stay consistent with the resultant path's floor.
inline Scalar GridMaxAbs( const DistanceGrid& g )
{
	Scalar m = 0.0;
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			const Scalar a = std::fabs( g.d[i][j] );
			if( a > m ) m = a;
		}
	}
	return m;
}

}}  // namespace RISE::BezierClip

#endif
