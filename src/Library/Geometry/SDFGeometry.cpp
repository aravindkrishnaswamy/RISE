//////////////////////////////////////////////////////////////////////
//
//  SDFGeometry.cpp - Sphere-traced signed-distance-field geometry.
//
//  Distance functions + smooth minimum follow Inigo Quilez,
//  "Distance Functions" (https://iquilezles.org/articles/distfunctions/).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SDFGeometry.h"

#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <cstdio>		// sscanf in ParsePartLines
#include <cctype>		// isspace in ParsePartLines
#include <cstring>		// strcmp in the part-grammar token maps
#include <cstdlib>		// strtoul for the robust part-index parse
#include <cassert>		// freeze-guard assert (compiles out in release)
#include <string>

#include "GeometryUtilities.h"		// MakeIndexedTriangleSameIdx for TessellateToMesh
#include "../Animation/KeyframableHelper.h"	// Parameter<>, Point3/Vector3Keyframe, ParseStrict*
#include "../Utilities/RenderParallelScope.h"	// g_renderParallelDepth -- single-thread-mutation tripwire
#include "../Utilities/FiniteMath.h"		// RISE::IsFiniteDouble -- the superellipsoid's non-finite guards
#include "../Utilities/SurfaceCurvature.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	inline Scalar clampS( const Scalar x, const Scalar lo, const Scalar hi )
	{
		return x < lo ? lo : ( x > hi ? hi : x );
	}

	// Shared cylindrical-ish UV about object Y -- used by IntersectRay,
	// TessellateToMesh, and UniformRandomPoint so all three agree.
	inline Point2 cylUV( const Point3& p, const BoundingBox& bb )
	{
		const Scalar u = Scalar(0.5) + std::atan2( p.z, p.x ) / Scalar(TWO_PI);
		const Scalar hgt = bb.ur.y - bb.ll.y;
		const Scalar v = ( hgt > NEARZERO ) ? ( p.y - bb.ll.y ) / hgt : Scalar(0.5);
		return Point2( u, v );
	}

	// ---- primitive signed-distance functions (point already in local frame) ----

	inline Scalar sdSphere( const Scalar x, const Scalar y, const Scalar z, const Scalar r )
	{
		return std::sqrt( x*x + y*y + z*z ) - r;
	}

	inline Scalar sdBox( const Scalar x, const Scalar y, const Scalar z,
	                     const Scalar bx, const Scalar by, const Scalar bz )
	{
		const Scalar qx = std::fabs(x) - bx;
		const Scalar qy = std::fabs(y) - by;
		const Scalar qz = std::fabs(z) - bz;
		const Scalar ax = std::max( qx, Scalar(0) );
		const Scalar ay = std::max( qy, Scalar(0) );
		const Scalar az = std::max( qz, Scalar(0) );
		const Scalar outside = std::sqrt( ax*ax + ay*ay + az*az );
		const Scalar inside  = std::min( std::max( qx, std::max( qy, qz ) ), Scalar(0) );
		return outside + inside;
	}

	inline Scalar sdRoundBox( const Scalar x, const Scalar y, const Scalar z,
	                          const Scalar bx, const Scalar by, const Scalar bz, const Scalar r )
	{
		return sdBox( x, y, z, std::max( bx - r, Scalar(0) ),
		                       std::max( by - r, Scalar(0) ),
		                       std::max( bz - r, Scalar(0) ) ) - r;
	}

	// capped cylinder, axis = local Y
	inline Scalar sdCylinderY( const Scalar x, const Scalar y, const Scalar z,
	                           const Scalar ra, const Scalar hh )
	{
		const Scalar dr = std::sqrt( x*x + z*z ) - ra;
		const Scalar dy = std::fabs( y ) - hh;
		const Scalar ar = std::max( dr, Scalar(0) );
		const Scalar ay = std::max( dy, Scalar(0) );
		const Scalar outside = std::sqrt( ar*ar + ay*ay );
		const Scalar inside  = std::min( std::max( dr, dy ), Scalar(0) );
		return outside + inside;
	}

	// torus, ring in local XZ plane (around local Y)
	inline Scalar sdTorusY( const Scalar x, const Scalar y, const Scalar z,
	                        const Scalar R, const Scalar rr )
	{
		const Scalar qx = std::sqrt( x*x + z*z ) - R;
		return std::sqrt( qx*qx + y*y ) - rr;
	}

	// capsule core segment along local Y, half-length hh, radius ra
	inline Scalar sdCapsuleY( const Scalar x, const Scalar y, const Scalar z,
	                          const Scalar ra, const Scalar hh )
	{
		const Scalar yy = clampS( y, -hh, hh );
		const Scalar dy = y - yy;
		return std::sqrt( x*x + dy*dy + z*z ) - ra;
	}

	// round cone along local Y: radius r1 at y=0 tapering to r2 at y=h
	inline Scalar sdRoundConeY( const Scalar x, const Scalar y, const Scalar z,
	                            const Scalar r1, const Scalar r2, const Scalar h )
	{
		const Scalar qx = std::sqrt( x*x + z*z );
		const Scalar qy = y;
		const Scalar b  = ( r1 - r2 ) / std::max( h, Scalar(1e-8) );
		const Scalar bb = b * b;
		if( bb >= Scalar(1) )
		{
			// Degenerate regime: |r1-r2| > h, so one cap sphere fully
			// contains the other and there is no lateral cone wall -- the
			// solid IS the larger cap sphere, exactly.  The general formula
			// below would clamp a = sqrt(max(1-b*b,0)) to exactly 0 here,
			// collapsing k = qx*(-b) + qy*a to -b*qx: correctly signed for
			// qx > 0, but exactly 0 on the local Y axis (qx == 0) where it
			// neither satisfies k<0 nor k>a*h==0 and falls through to the
			// lateral-wall branch with the wrong sign/magnitude.  Mirrors
			// the degenerate-regime handling in this primitive's local
			// bounds computation (ePrimRoundCone case, further down in
			// this file) -- keep the two in sync.
			return ( r1 >= r2 ) ? std::sqrt( qx*qx + qy*qy ) - r1
			                     : std::sqrt( qx*qx + (qy-h)*(qy-h) ) - r2;
		}
		const Scalar a  = std::sqrt( Scalar(1) - bb );
		const Scalar k  = qx * (-b) + qy * a;
		if( k < Scalar(0) )   return std::sqrt( qx*qx + qy*qy ) - r1;
		if( k > a * h )       { const Scalar dyt = qy - h; return std::sqrt( qx*qx + dyt*dyt ) - r2; }
		return qx * a + qy * b - r1;
	}

	// ---- superellipsoid (Barr superquadric), pole axis = local Y ----
	//
	// SUPPORTED EXPONENT RANGE.  Both exponents are clamped into
	// [kSEMinExp, kSEMaxExp] = [0.1, 2] before anything else touches them.
	//   UPPER 2 is LOAD-BEARING, not taste: it is exactly the convexity
	//     boundary (see the proof below), and past it the returned distance
	//     OVERESTIMATES -- i.e. March would step through surface.  Measured
	//     on the same dense-search harness the unit test uses: worst
	//     |estimate| / true distance is 1.000 at e = 2 (never above), 1.24 at
	//     e = 2.2, 2.68 at e = 3, 94 at e = 4.
	//   LOWER 0.1 is numerical/expressive headroom: 2/e is the pow exponent,
	//     and at e = 0.1 the shape already sits within 3.5 % of the box that
	//     `box` / `roundbox` render EXACTLY and far more cheaply (the gauge at
	//     an edge midpoint is 2^(e/2) = 1.035), so the clamp costs no
	//     expressible shape.
	// The clamp lives HERE, in the field, not (only) in the parser: `a`, `b`,
	// `c` are KEYFRAMABLE (`part<i>.size`), and SetIntermediateValue writes
	// them straight into the Part -- a parser-only clamp would be bypassed
	// mid-animation.  ParsePartLines clamps too, but only to WARN the author.
	const Scalar kSEMinExp = Scalar(0.1);
	const Scalar kSEMaxExp = Scalar(2.0);

	//! Clamp a superellipsoid exponent into the supported range, NaN-safely
	//! (a bare clampS would pass a NaN straight through -- both compares are
	//! false -- and NaN^x poisons the whole field).  Non-finite maps to 1,
	//! the ellipsoid identity.
	inline Scalar clampSuperExp( const Scalar e )
	{
		if( !RISE::IsFiniteDouble( e ) ) {
			return Scalar(1);
		}
		return clampS( e, kSEMinExp, kSEMaxExp );
	}

	//! |(fx,fy,fz)| for NON-NEGATIVE, FINITE components, computed MAX-FACTORED
	//! ( |v| = m * |v/m|, m = max component ) so that a huge-but-finite
	//! coordinate cannot square to +inf.  An infinite result would be an
	//! OVER-estimate of the distance -- the one direction a sphere-traced
	//! field must never err in -- which is why this is not a plain
	//! sqrt(x*x + y*y + z*z).
	inline Scalar superMagnitude( const Scalar fx, const Scalar fy, const Scalar fz )
	{
		const Scalar m = std::max( fx, std::max( fy, fz ) );
		if( !( m > Scalar(0) ) ) {
			return Scalar(0);
		}
		const Scalar im = Scalar(1) / m;
		const Scalar bx = fx*im, by = fy*im, bz = fz*im;
		return m * std::sqrt( bx*bx + by*by + bz*bz );
	}

	// Implicit form, radius a, latitude (north-south) exponent e1, longitude
	// (east-west) exponent e2:
	//
	//   F(p) = ( |x/a|^(2/e2) + |z/a|^(2/e2) )^(e2/e1) + |y/a|^(2/e1),  surface F = 1
	//
	// e1 = e2 = 1 is the sphere; both -> 0 the box; e1 -> 0 with e2 = 1 the
	// cylinder about Y; e1 = e2 = 2 the octahedron.  F is homogeneous of
	// degree 2/e1 -- F(t*p) = t^(2/e1) F(p) -- so the solid is the unit ball
	// of the MIXED NORM (an l_p of an l_q; p = 2/e1 along the pole axis,
	// q = 2/e2 across it), whose Minkowski GAUGE is
	//
	//   g(p) = F(p)^(e1/2) = ( ( |x/a|^q + |z/a|^q )^(p/q) + |y/a|^p )^(1/p)
	//
	// g < 1 inside, = 1 on the surface, > 1 outside.
	//
	// CONSERVATIVENESS -- the load-bearing property.  SDFGeometry's sphere
	// tracer requires every part field to be <= 1-Lipschitz, i.e. to NEVER
	// overestimate the distance to its own zero set, or March overshoots and
	// misses surface (same contract sminP/smaxP's comment relies on).  A
	// superquadric has NO exact closed-form SDF.  What we return is
	//
	//   d(p) = rin * ( g(p) - 1 )
	//
	// and it is conservative by this argument:
	//   (1) For p, q >= 1 the mixed norm IS a norm: |y| and ||(x,z)||_q are
	//       each convex and positively homogeneous, and the outer l_p norm on
	//       R^2 is convex AND nondecreasing in each |component|, so the
	//       composition is convex.  Hence g is SUBADDITIVE and the solid
	//       {g <= 1} is CONVEX.  p, q >= 1  <=>  e1, e2 <= 2 -- which is
	//       exactly why kSEMaxExp is 2.
	//   (2) Let rin be any radius with the ball B(0, rin) inside the solid.
	//       Then g(v) <= |v| / rin for all v, so by subadditivity
	//       |g(p) - g(r)| <= g(p - r) <= |p - r| / rin: g is 1/rin-Lipschitz,
	//       and d = rin*(g - 1) is 1-LIPSCHITZ.
	//   (3) d vanishes on the surface {g = 1}.  A 1-Lipschitz function that
	//       vanishes on a set S obeys, for EVERY s in S,
	//       |d(p)| = |d(p) - d(s)| <= |p - s|, hence |d(p)| <= dist(p, S).
	//       Conservative both OUTSIDE and INSIDE, at every in-range exponent.
	// Note what does NOT work: the naive radial estimate
	// |p| * (1 - F(p)^(-e1/2)) -- the distance from p to the surface ALONG THE
	// RAY THROUGH THE ORIGIN.  That ray lands on an ACTUAL surface point, so
	// it is an OVER-estimate of the true (nearest-point) distance -- ~10 % off
	// a near-box face column.  d above is that same radial estimate rescaled
	// from the LOCAL surface radius down to the GLOBAL inradius; the rescale
	// is what makes it safe.
	//
	// rin: the closed-form lower bound rin = a / (Cp*Cq), C = 2^max(0,(e-1)/2).
	// Derivation from the standard l_p-vs-l_2 constants on R^2:
	// ||(x,z)||_q <= Cq*||(x,z)||_2 and ||(s,t)||_p <= Cp*||(s,t)||_2, and the
	// outer norm is monotone, so g(v) <= Cp*Cq*|v| / a -- i.e. the ball of
	// radius a/(Cp*Cq) is inside the solid.  It is EXACT for e1, e2 <= 1
	// (Cp = Cq = 1: the sphere / cushion / box regime most authoring lives in,
	// where d is the EXACT SDF at e1 = e2 = 1), exact for the (e1=2, e2=1)
	// bicone, and 13 % conservative at the far corner e1 = e2 = 2 (bound a/2
	// vs the octahedron's true inradius a/sqrt(3)) -- which costs a few extra
	// sphere-trace steps there and nothing else.
	//
	// Empirically re-verified by SDFGeometryTest Test 33d over 10 exponent
	// pairs spanning the whole supported range, against a true-distance
	// reference that is itself PINNED rather than assumed: the UPPER bound
	// comes from surface points sampled by DIRECTION (d/g(d) lies exactly on
	// {g = 1}) plus a tangent-frame refine, the matching LOWER bound from the
	// analytic support function of the DUAL mixed norm (conjugate exponents
	// p/(p-1), q/(q-1)) taken through the nearest point found.  The two
	// bracket the truth to < 1e-4 relative at every probe, so the "never
	// overestimates" assertion is exact rather than lenient.  Tests 33g / 33j
	// re-run the same check on the clamped and the specialized paths.
	inline Scalar sdSuperellipsoidY( const Scalar x, const Scalar y, const Scalar z,
	                                 const Scalar a, const Scalar e1raw, const Scalar e2raw )
	{
		const Scalar fx = std::fabs(x);
		const Scalar fy = std::fabs(y);
		const Scalar fz = std::fabs(z);

		// NON-FINITE GUARD, ON THE INPUTS.  It has to be here, BEFORE the
		// gauge: the max-factoring below turns inf/inf into NaN, and
		// std::max(NaN, v) is (a<b)?b:a == NaN, after which the `> 0` tests
		// are FALSE (every compare against NaN is) and u / g silently collapse
		// to the ORIGIN value 0 -- so a NaN or infinite coordinate would come
		// back as -rin, i.e. the point reported MAXIMALLY INSIDE.  Folded
		// through Map's min() that is far worse than a stray NaN: a constant
		// negative FILLS the bounding box.  A guard on the RESULT alone CANNOT
		// see it -- the NaN is erased before it is ever reached (measured on
		// the pre-fix form: an inf or NaN in x or z, or a NaN in y, returned
		// a NEGATIVE distance; only a non-finite y reached the result check).
		//   ONE add-and-test covers all three coordinates, because fx/fy/fz
		// are non-negative by construction: no cancellation can hide an inf,
		// and any NaN propagates through the sum.  The answer is 1e30, a
		// definite MISS -- an UNDER-estimate of the true distance at any such
		// point, hence safe for the sphere trace.
		if( !RISE::IsFiniteDouble( fx + fy + fz ) ) {
			return Scalar(1e30);
		}

		// Degenerate radius (0, negative, or non-finite): the solid collapses
		// to the local origin, whose EXACT distance field is |p| - a.  Handled
		// up front because every expression below divides by a.
		if( !( RISE::IsFiniteDouble( a ) && a > Scalar(1e-12) ) ) {
			const Scalar aa = ( RISE::IsFiniteDouble( a ) && a > Scalar(0) ) ? a : Scalar(0);
			return superMagnitude( fx, fy, fz ) - aa;
		}

		const Scalar e1 = clampSuperExp( e1raw );
		const Scalar e2 = clampSuperExp( e2raw );

		// FAST PATH 1 -- THE ELLIPSOID/SPHERE, and EXACTLY sdSphere.
		// e1 = e2 = 1 gives p = q = 2, so the mixed norm IS the Euclidean one;
		// and both l_p-vs-l_2 constants are 2^max(0,(1-1)/2) = 1, so rin = a
		// EXACTLY.  d = rin*(g-1) = a*(|point|/a - 1) = |point| - a: the same
		// closed form sdSphere returns, with nothing approximated -- the
		// general form below spends four pow() calls arriving at it.  This is
		// the most commonly authored member of the family (the ellipsoidal
		// PROPORTIONS come from the part's own <sx sy sz>, so `a` stays a
		// single radius), and on the general path it costs ~17x a `sphere`
		// part (52.1 vs 3.04 ns/eval, -O2); specialised it costs ~1.4x
		// (4.35 ns/eval).  superMagnitude, not a literal
		// sqrt(x*x+y*y+z*z), so a huge-but-finite coordinate cannot square to
		// +inf and hand the tracer an OVERestimate.
		if( e1 == Scalar(1) && e2 == Scalar(1) )
		{
			return superMagnitude( fx, fy, fz ) - a;	// g = |point|/a, rin = a
		}

		const Scalar ax = fx / a;
		const Scalar ay = fy / a;
		const Scalar az = fz / a;

		// Second guard, for the DIVISION: a finite-but-huge coordinate over a
		// near-1e-12 radius can overflow to inf, which the max-factoring would
		// again launder into a spurious -rin.  (The sphere path above never
		// divides by a, so it does not need this.)
		if( !RISE::IsFiniteDouble( ax + ay + az ) ) {
			return Scalar(1e30);
		}

		// Both l-norms are evaluated MAX-FACTORED: ||v||_n = m * ||v/m||_n with
		// m = max|component|.  Every base handed to pow() is then in [0,1], so
		// no exponent the clamp permits (up to 20) can overflow -- checked to
		// |coord| = 1e300 and down to 1e-160 denormals.  A base that UNDERFLOWS
		// to 0 drops a term worth < 1e-300 RELATIVE to the retained max term,
		// and that MAGNITUDE bound is the whole safety argument: the resulting
		// |dg| <= 1e-300*g is orders below the field's own round-off, on either
		// side of the surface.  (It is NOT a direction argument -- d = rin*(g-1),
		// so a smaller g means a SMALLER |d| outside but a LARGER |d| inside.)
		Scalar g, rin;
		if( e1 == e2 )
		{
			// FAST PATH 2 -- EQUAL EXPONENTS, also exact.  p == q makes the
			// outer exponent ratio p/q == 1, so the nested gauge
			//   ( ( |x|^q + |z|^q )^(p/q) + |y|^p )^(1/p)
			// collapses ALGEBRAICALLY to the single 3-term l_p norm
			//   ( |x|^p + |y|^p + |z|^p )^(1/p)
			// -- four pow() calls instead of six, and one max-factoring
			// instead of two (49.8 -> 26.2 ns/eval at e = 0.45, -O2).  rin
			// uses the SAME constant twice, so squaring one pow() is
			// bit-identical to the general form's product of two.
			const Scalar p = Scalar(2) / e1;
			const Scalar C = std::pow( Scalar(2), std::max( Scalar(0), (e1-Scalar(1))*Scalar(0.5) ) );
			rin = a / ( C * C );
			const Scalar m = std::max( ax, std::max( ay, az ) );
			if( m > Scalar(0) ) {
				const Scalar im = Scalar(1) / m;
				const Scalar bx = ax*im, by = ay*im, bz = az*im;
				g = m * std::pow( std::pow(bx,p) + std::pow(by,p) + std::pow(bz,p), Scalar(1)/p );
			} else {
				g = Scalar(0);
			}
		}
		else
		{
			const Scalar p = Scalar(2) / e1;
			const Scalar q = Scalar(2) / e2;
			const Scalar m = std::max( ax, az );
			const Scalar u = ( m > Scalar(0) )
				? m * std::pow( std::pow( ax/m, q ) + std::pow( az/m, q ), Scalar(1)/q )
				: Scalar(0);
			const Scalar M = std::max( u, ay );
			g = ( M > Scalar(0) )
				? M * std::pow( std::pow( u/M, p ) + std::pow( ay/M, p ), Scalar(1)/p )
				: Scalar(0);	// p == local origin: g = 0, d = -rin (deepest point)
			rin = a / ( std::pow( Scalar(2), std::max( Scalar(0), (e1-Scalar(1))*Scalar(0.5) ) )
			          * std::pow( Scalar(2), std::max( Scalar(0), (e2-Scalar(1))*Scalar(0.5) ) ) );
		}

		// Residual overflow guard on the RESULT.  With ax/ay/az already known
		// finite the only way out is m * (a bounded factor <= 3) overflowing
		// for m near DBL_MAX.  Same answer, same reason: a definite MISS.
		if( !RISE::IsFiniteDouble( g ) ) {
			return Scalar(1e30);
		}

		return rin * ( g - Scalar(1) );
	}

	// Quilez polynomial smooth minimum (k = blend radius in world units)
	inline Scalar sminP( const Scalar a, const Scalar b, const Scalar k )
	{
		if( k <= Scalar(0) ) return std::min( a, b );
		const Scalar h = std::max( k - std::fabs( a - b ), Scalar(0) ) / k;
		return std::min( a, b ) - h * h * k * Scalar(0.25);
	}

	// smax = -smin(-a,-b) composes smooth intersection / subtraction.  smax's VALUE
	// exceeds the (already non-exact) hard max, which LOOKS like a sphere-trace
	// overstep hazard -- but it is not: smin and smax are 1-Lipschitz when their
	// inputs are (the gradient of the quadratic blend is a convex combination of
	// two unit gradients, magnitude <= 1 by the triangle inequality), and every
	// part field is <= 1-Lipschitz (minScale compensates non-uniform scale).  So
	// the COMPOSED field is <= 1-Lipschitz and vanishes on its own zero set, i.e.
	// |Map(p)| <= true distance to the surface -- a CONSERVATIVE bound that cannot
	// step past the surface, for any blend radius k.  Verified by SDFGeometryTest
	// Test 10/11/12: hard AND smooth-k=1 subtract + intersect neither tunnel a
	// carved wall nor fill a carved hole.
	inline Scalar smaxP( const Scalar a, const Scalar b, const Scalar k )
	{
		return -sminP( -a, -b, k );
	}

	inline Scalar primDist( const SDFGeometry::Part& pt, const Scalar lx, const Scalar ly, const Scalar lz )
	{
		switch( pt.type )
		{
		case SDFGeometry::ePrimSphere:    return sdSphere   ( lx, ly, lz, pt.a );
		case SDFGeometry::ePrimBox:       return sdBox      ( lx, ly, lz, pt.a, pt.b, pt.c );
		case SDFGeometry::ePrimRoundBox:  return sdRoundBox ( lx, ly, lz, pt.a, pt.b, pt.c, pt.round );
		case SDFGeometry::ePrimCylinder:  return sdCylinderY( lx, ly, lz, pt.a, pt.b );
		case SDFGeometry::ePrimTorus:     return sdTorusY   ( lx, ly, lz, pt.a, pt.b );
		case SDFGeometry::ePrimCapsule:   return sdCapsuleY ( lx, ly, lz, pt.a, pt.b );
		case SDFGeometry::ePrimRoundCone: return sdRoundConeY( lx, ly, lz, pt.a, pt.b, pt.c );
		case SDFGeometry::ePrimSuperellipsoid: return sdSuperellipsoidY( lx, ly, lz, pt.a, pt.b, pt.c );
		}
		return Scalar(1e30);
	}

	// distance of point p (object space) to a single part
	inline Scalar partEval( const SDFGeometry::Part& pt, const Point3& p )
	{
		const Scalar dx = p.x - pt.pos.x;
		const Scalar dy = p.y - pt.pos.y;
		const Scalar dz = p.z - pt.pos.z;
		// Rinv*(p-pos) = ( colX.(p-pos), colY.(p-pos), colZ.(p-pos) )
		Scalar lx = ( pt.cx.x*dx + pt.cx.y*dy + pt.cx.z*dz ) * pt.invScale.x;
		Scalar ly = ( pt.cy.x*dx + pt.cy.y*dy + pt.cy.z*dz ) * pt.invScale.y;
		Scalar lz = ( pt.cz.x*dx + pt.cz.y*dy + pt.cz.z*dz ) * pt.invScale.z;
		return primDist( pt, lx, ly, lz ) * pt.minScale;
	}

	// local AABB of a primitive (before scale/rotation/translation)
	void primLocalAABB( const SDFGeometry::Part& pt, Point3& lmin, Point3& lmax )
	{
		Scalar rx, ry0, ry1, rz;
		switch( pt.type )
		{
		case SDFGeometry::ePrimSphere:    rx = rz = pt.a;            ry0 = -pt.a;        ry1 = pt.a;        break;
		case SDFGeometry::ePrimBox:
		case SDFGeometry::ePrimRoundBox:  rx = pt.a; rz = pt.c;      ry0 = -pt.b;        ry1 = pt.b;        break;
		case SDFGeometry::ePrimCylinder:  rx = rz = pt.a;            ry0 = -pt.b;        ry1 = pt.b;        break;
		case SDFGeometry::ePrimTorus:     rx = rz = pt.a + pt.b;     ry0 = -pt.b;        ry1 = pt.b;        break;
		case SDFGeometry::ePrimCapsule:
			// Half-height |b|, NOT b.  sdCapsuleY's core segment is
			// clampS(y, -b, b), and for a NEGATIVE b that interval has lo > hi:
			// clampS then returns lo = |b| for every y < |b| and hi = -|b| for
			// every y >= |b|, so the solid is the cap sphere of radius a centred
			// at y = +|b| (plus, when a >= 2|b|, a second lobe from the sphere at
			// y = -|b|).  Its true top is |b|, while b+a = a-|b| -- an UNDER-bound
			// by 2|b|-a once |b| > a/2, which CLIPS real surface out of the march
			// (the bbox gate rejects rays that should hit).  |b| bounds both
			// lobes: [-(|b|+a), |b|+a] contains [|b|-a, max(|b|, a-|b|)].
			// BIT-IDENTICAL for every b >= 0 (fabs is the identity there), which
			// is every capsule ParsePartLines has ever produced -- but `size` is
			// KEYFRAMABLE and SetIntermediateValue writes a/b/c RAW, so a
			// half-height easing through zero reaches the negative branch.
			// Same defect class as the ePrimRoundCone and roundbox rows below.
			{ const Scalar hh = std::fabs( pt.b );
			  rx = rz = pt.a;            ry0 = -(hh+pt.a);   ry1 = hh+pt.a;   }
			break;
		case SDFGeometry::ePrimSuperellipsoid:
			// EXACT and tight for EVERY exponent pair, in range or not: on the
			// surface F = 1 is a sum of NON-NEGATIVE terms, so each term is
			// <= 1 individually -- |y/a|^(2/e1) <= 1 gives |y| <= a, and
			// (|x/a|^(2/e2) + |z/a|^(2/e2))^(e2/e1) <= 1 gives |x|, |z| <= a.
			// Tight because the three axis intercepts sit exactly AT +-a for
			// every exponent (F on the +x axis is |x/a|^(2/e1) = 1).  So the
			// e > 1 regime (which pulls the surface IN toward the octahedral
			// diagonals) and the e < 1 regime (which pushes it OUT toward the
			// box) share one bound, and no clamp interaction can invalidate
			// it.  Same box as ePrimSphere -- but spelled out rather than
			// folded into the sphere row, because the two coincide for a
			// DIFFERENT reason and only the sphere's is a radius.
			rx = rz = pt.a;            ry0 = -pt.a;        ry1 = pt.a;        break;
		case SDFGeometry::ePrimRoundCone:
			// Envelope of a round cone (base cap radius a at y=0, tip cap
			// radius b at y=c) is the convex hull of its two end spheres --
			// so its Y extent is the union of BOTH spheres' own extents, not
			// just the naive "-a at the base, c+b at the tip" pairing.  For a
			// WELL-FORMED cone (|a-b| <= c, i.e. neither cap sphere contains
			// the other) the two forms agree exactly: c-b >= -a <=> b-a <= c,
			// and a <= c+b <=> a-b <= c, both guaranteed by |a-b| <= c.  For a
			// DEGENERATE cone (|a-b| > c -- a fat joint close to a small one,
			// which skeleton_geometry's bone chains can produce) the naive
			// form under-bounds by |a-b|-c and clips real surface; min/max
			// over both spheres is exact for both regimes.
			rx = rz = std::max( pt.a, pt.b );
			ry0 = std::min( -pt.a, pt.c - pt.b );
			ry1 = std::max(  pt.a, pt.c + pt.b );
			break;
		default:                          rx = rz = pt.a;            ry0 = -pt.a;        ry1 = pt.a;        break;
		}
		// box geometry uses (a,b,c) per axis
		if( pt.type == SDFGeometry::ePrimBox || pt.type == SDFGeometry::ePrimRoundBox ) {
			// A ROUNDBOX reaches max(half-extent, round) per axis, not the
			// half-extent: sdRoundBox SHRINKS the core box by `round` and then
			// INFLATES the result by `round`, i.e.
			//   sdBox( ..., max(bx-r,0), ... ) - r   ->   surface at max(bx, r).
			// For every well-formed rounded box (r <= min half-extent -- the
			// only regime a scene sanely authors) max(bx,r) == bx and this is
			// BIT-IDENTICAL to the old bound.  Once r exceeds a half-extent the
			// core box collapses and the solid IS the sphere of radius r, which
			// the half-extents alone UNDER-bound -- and an under-bound clips
			// real surface out of the march (the bbox gate rejects rays that
			// should hit).  Same defect class as the roundcone envelope above;
			// keep this in sync with sdRoundBox's shrink-then-inflate form.
			// `round` is IGNORED by the plain box field, so it must not widen
			// the plain box's bound.
			const Scalar r  = ( pt.type == SDFGeometry::ePrimRoundBox ) ? std::max( pt.round, Scalar(0) ) : Scalar(0);
			const Scalar ex = std::max( pt.a, r ), ey = std::max( pt.b, r ), ez = std::max( pt.c, r );
			lmin = Point3( -ex, -ey, -ez );
			lmax = Point3(  ex,  ey,  ez );
		} else {
			lmin = Point3( -rx, ry0, -rz );
			lmax = Point3(  rx, ry1,  rz );
		}
	}

	// FIELD-TO-LOCAL-DISTANCE STRETCH.  ComputeBounds needs, for a sublevel
	// threshold tau >= 0, a box containing { l : primDist(pt,l) <= tau } -- not
	// just the solid { primDist <= 0 } that primLocalAABB bounds.  Write that
	// containment as "primLocalAABB inflated by F*tau on every axis"; this
	// returns F.
	//
	// F = 1 whenever the primitive's field is the EXACT exterior distance,
	// because then { d <= tau } = solid (+) B(0,tau), which the axis-inflated
	// AABB contains.  Note the direction that matters here is the OPPOSITE of
	// the sphere-trace contract: tracing needs d <= true distance (never
	// overestimate), while this bound needs d >= true distance (never
	// UNDERestimate) -- so a merely CONSERVATIVE field is not automatically
	// F = 1, and each primitive has to be exact or carry its own F.
	//
	//   sphere / box / roundbox / cylinder / torus / capsule / roundcone: all
	//   seven are exact exterior SDFs, so F = 1.
	//     - sdBox / sdRoundBox: IQ's exact box form (outside = |max(q,0)|).
	//     - sdCylinderY: the solid is the intersection of the radial and the
	//       axial slab constraints, whose gradients are ORTHOGONAL, so
	//       sqrt(max(dr,0)^2 + max(dy,0)^2) is the exact exterior distance.
	//     - sdTorusY: dist(p, generating circle) is exact for every R > 0
	//       (including on the axis, where qx = -R gives sqrt(R^2+y^2)), and a
	//       tube is the rr-sublevel of that exact distance.
	//     - sdCapsuleY: exact point-to-segment distance minus the radius.
	//     - sdRoundConeY: the lateral branch returns a*qx + b*qy - r1, which IS
	//       the signed distance to the line of unit normal (a,b) tangent to
	//       BOTH cap circles: tangency to circle1 (centre origin, radius r1)
	//       forces the offset c = r1, tangency to circle2 (centre (0,h),
	//       radius r2) forces b*h - c = -r2, i.e. b = (r1-r2)/h -- exactly the
	//       code's b, with a = sqrt(1-b^2).  The k < 0 / k > a*h branches hand
	//       off to the two cap spheres at precisely the tangency points, and
	//       the bb >= 1 branch IS the containing cap sphere.  Exact throughout.
	//
	//   superellipsoid: d = rin*(g-1) with g the gauge -- a deliberate UNDER-
	//   estimate of the true distance (see sdSuperellipsoidY's derivation), so
	//   F > 1 in general.  g is homogeneous of degree 1, hence
	//   { d <= tau } = { g <= 1 + tau/rin } = (1 + tau/rin) * { g <= 1 }, and
	//   { g <= 1 } sits exactly within [-a,a]^3 (primLocalAABB's row above), so
	//   the per-axis growth is a*tau/rin -- i.e. F = a/rin, with `a` cancelling:
	//     F = 2^max(0,(e1-1)/2) * 2^max(0,(e2-1)/2).
	//   F = 1 over the whole e <= 1 regime (sphere / cushion / box), rising to
	//   2 at the octahedral corner e1 = e2 = 2.  The exponents are read through
	//   clampSuperExp so this agrees with the rin the FIELD actually used --
	//   a raw e > 2 would otherwise claim a stretch the clamped field does not
	//   have, and a NaN e would poison the box.
	inline Scalar primFieldStretch( const SDFGeometry::Part& pt )
	{
		if( pt.type != SDFGeometry::ePrimSuperellipsoid ) {
			return Scalar(1);
		}
		const Scalar e1 = clampSuperExp( pt.b );
		const Scalar e2 = clampSuperExp( pt.c );
		return std::pow( Scalar(2), std::max( Scalar(0), (e1-Scalar(1))*Scalar(0.5) ) )
		     * std::pow( Scalar(2), std::max( Scalar(0), (e2-Scalar(1))*Scalar(0.5) ) );
	}
}

//////////////////////////////////////////////////////////////////////

// Recomputes a part's DERIVED fields (rotation columns + inverse/min scale)
// from its RAW authoring fields (euler degrees, per-axis scale).  ONE source
// of truth for this math: MakePart calls it at build time, SetIntermediateValue
// calls it when keyframing rotation / scale -- so the two can never drift.
void SDFGeometry::RecomputePartDerived( Part& pt )
{
	const Scalar DEG = Scalar(PI) / Scalar(180.0);
	const Scalar ex = pt.euler.x*DEG, ey = pt.euler.y*DEG, ez = pt.euler.z*DEG;
	const Scalar cx = std::cos(ex), sx = std::sin(ex);
	const Scalar cy = std::cos(ey), sy = std::sin(ey);
	const Scalar cz = std::cos(ez), sz = std::sin(ez);

	// R = Rz(ez) * Ry(ey) * Rx(ex); store its COLUMNS (local axes in object space).
	pt.cx = Vector3(  cz*cy,                 sz*cy,                 -sy     );
	pt.cy = Vector3(  cz*sy*sx - sz*cx,      sz*sy*sx + cz*cx,      cy*sx   );
	pt.cz = Vector3(  cz*sy*cx + sz*sx,      sz*sy*cx - cz*sx,      cy*cx   );

	const Scalar sxA = ( std::fabs(pt.scale.x) > Scalar(1e-9) ) ? pt.scale.x : Scalar(1e-9);
	const Scalar syA = ( std::fabs(pt.scale.y) > Scalar(1e-9) ) ? pt.scale.y : Scalar(1e-9);
	const Scalar szA = ( std::fabs(pt.scale.z) > Scalar(1e-9) ) ? pt.scale.z : Scalar(1e-9);
	pt.invScale = Vector3( Scalar(1)/sxA, Scalar(1)/syA, Scalar(1)/szA );
	pt.minScale = std::min( std::fabs(sxA), std::min( std::fabs(syA), std::fabs(szA) ) );
}

SDFGeometry::Part SDFGeometry::MakePart(
	const SDFPrim type, const SDFOp op, const Scalar k,
	const Point3& pos, const Scalar exDeg, const Scalar eyDeg, const Scalar ezDeg,
	const Vector3& scale, const Scalar a, const Scalar b, const Scalar c, const Scalar round )
{
	Part pt;
	pt.type = type; pt.op = op; pt.k = k; pt.pos = pos;
	pt.a = a; pt.b = b; pt.c = c; pt.round = round;
	pt.scale = scale;
	pt.euler = Vector3( exDeg, eyDeg, ezDeg );
	RecomputePartDerived( pt );
	return pt;
}

SDFGeometry::SDFGeometry( const std::vector<Part>& parts, const unsigned int maxSteps, const Scalar surfaceEpsilonFraction, const unsigned int samplingDetail ) :
	m_parts( parts ),
	m_maxSteps( maxSteps > 0 ? maxSteps : 256 ),
	m_epsFrac( surfaceEpsilonFraction > 0 ? surfaceEpsilonFraction : Scalar(5e-5) ),
	m_eps( Scalar(1e-4) ),
	m_diagonal( Scalar(1) ),
	m_samplingDetail( samplingDetail < 8 ? 8 : ( samplingDetail > 256 ? 256 : samplingDetail ) ),
	m_samplingOnce( std::make_unique<std::once_flag>() ),
	m_surfaceArea( 0 )
{
	ComputeBounds();
}

// Heightfield mode: the analytic exact-surface twin of DisplacedGeometry.
// Leaves m_parts empty -- Map()/ComputeBounds() take the heightfield branch.
// The init list is in member-declaration ORDER (the new m_hf* members come
// LAST in the header, after the sampling members), so -Wreorder stays quiet.
SDFGeometry::SDFGeometry( const IFunction2D* field, const Scalar radius, const Scalar scale,
	const unsigned int maxSteps, const Scalar surfaceEpsilonFraction, const unsigned int samplingDetail ) :
	m_maxSteps( maxSteps > 0 ? maxSteps : 256 ),
	m_epsFrac( surfaceEpsilonFraction > 0 ? surfaceEpsilonFraction : Scalar(5e-5) ),
	m_eps( Scalar(1e-4) ),
	m_diagonal( Scalar(1) ),
	m_samplingDetail( samplingDetail < 8 ? 8 : ( samplingDetail > 256 ? 256 : samplingDetail ) ),
	m_samplingOnce( std::make_unique<std::once_flag>() ),
	m_surfaceArea( 0 ),
	m_isHeightfield( true ),
	m_pHeightfield( field ),
	m_hfRadius( radius ),
	m_hfScale( scale ),
	m_hfLip( 2 )
{
	if( m_pHeightfield ) m_pHeightfield->addref();
	ComputeHeightfieldLipschitz();
	ComputeBounds();
}

// (Re)computes m_hfLip from the heightfield field + current amplitude.  Shared
// by the heightfield ctor and RegenerateData so a keyframed heightfield_scale
// re-derives a SAFE sphere-trace Lipschitz bound (an amplitude bump that left
// m_hfLip stale would let the trace overstep the steeper grooves).
void SDFGeometry::ComputeHeightfieldLipschitz()
{
	m_hfLip = 2;
	if( m_pHeightfield && m_hfScale != Scalar(0) ) {
		const Scalar R = m_hfRadius; const int N = 128; const Scalar du = Scalar(1)/N;
		Scalar maxg = 0;
		for( int j=1; j<N; ++j ) for( int i=1; i<N; ++i ) {
			const Scalar u=i*du, v=j*du, cx=u-Scalar(0.5), cy=v-Scalar(0.5);
			if( cx*cx+cy*cy < Scalar(0.0144) ) continue;          // skip centre (0.12^2)
			const Scalar f0=m_pHeightfield->Evaluate(u,v);
			const Scalar fx=m_pHeightfield->Evaluate(std::min(u+du,Scalar(1)),v);
			const Scalar fy=m_pHeightfield->Evaluate(u,std::min(v+du,Scalar(1)));
			const Scalar gx=m_hfScale*(fx-f0)/du/(2*R), gy=m_hfScale*(fy-f0)/du/(2*R);
			const Scalar g=std::sqrt(gx*gx+gy*gy); if(g>maxg) maxg=g;
		}
		// 1.5x safety factor: a 128-grid chord forward-difference UNDER-bounds
		// the true gradient at sharp V-cusps (probe resolution sets the chord,
		// not the wall slope), so the bare sqrt(1+maxg^2) lets the trace overstep
		// the groove floors.  Over-bounding L = slightly smaller steps (a few more
		// iters) but never overshoot.  Ceiling 256 keeps a runaway-step backstop.
		m_hfLip = Scalar(1.5)*std::sqrt(Scalar(1)+maxg*maxg);
		if(m_hfLip<Scalar(1)) m_hfLip=Scalar(1);
		if(m_hfLip>Scalar(256)) m_hfLip=Scalar(256);
	}
}

SDFGeometry::~SDFGeometry()
{
	if( m_pHeightfield ) m_pHeightfield->release();
}

void SDFGeometry::ComputeBounds()
{
	if( m_isHeightfield ) {
		const Scalar R=m_hfRadius;
		const Scalar zlo=std::min(Scalar(0),m_hfScale), zhi=std::max(Scalar(0),m_hfScale);
		Point3 mn(-R,-R,zlo), mx(R,R,zhi);
		const Scalar dx0=mx.x-mn.x,dy0=mx.y-mn.y,dz0=mx.z-mn.z;
		const Scalar diag0=std::sqrt(dx0*dx0+dy0*dy0+dz0*dz0);
		const Scalar eps0=std::max(diag0*m_epsFrac,Scalar(1e-6));
		const Scalar pad=std::max(Scalar(1e-3),Scalar(3)*eps0);
		mn.x-=pad;mn.y-=pad;mn.z-=pad;mx.x+=pad;mx.y+=pad;mx.z+=pad;
		m_bbox=BoundingBox(mn,mx);
		const Scalar dx=mx.x-mn.x,dy=mx.y-mn.y,dz=mx.z-mn.z;
		m_diagonal=std::sqrt(dx*dx+dy*dy+dz*dz);
		// Surface band off the FEATURE size (z-amplitude), NOT the diagonal: the
		// bbox diagonal is dominated by the 2R lateral extent, so m_diagonal*epsFrac
		// is a band ~3% of a shallow groove's depth -> bisection smears the cusps and
		// craters GGX contrast.  Tie it to |m_hfScale| (~0.5% of depth).
		m_eps=std::max( std::fabs(m_hfScale)*Scalar(0.005), Scalar(1e-6) );
		return;
	}

	// Order-aware AABB.  Map() folds the field as a strict LEFT FOLD over the
	// parts -- d = op( d_prev, d_part ) for each part in declaration order -- so
	// the bound must fold the SAME way on axis-aligned boxes, or it under-bounds
	// any field whose op order is not "all additive, then all clip".  A bbox built
	// by class (union the additive boxes, intersect the clip boxes, then combine)
	// is ORDER-BLIND: a union/smin part appearing AFTER an intersect re-grows the
	// solid OUTSIDE the clip box, and a class-split bound would miss that lobe
	// (bbox-gated rays would then skip real surface, and the marching grid would
	// clip it).  Folding sequentially is conservative and order-correct:
	//   union / smin -> running = AABB-union       ( running, partBox_i )
	//   intersect    -> running = AABB-intersection( running, partBox_i )
	//   subtract     -> no-op (a carve only ever shrinks the solid)
	// e.g. for [ unionA1, intersectC, unionA2 ] this yields
	// ( box(A1) INTERSECT box(C) ) UNION box(A2) -- the second lobe survives.
	//
	// THE SMIN BULGE IS A PER-PART SUBLEVEL SET, NOT A RUNNING-BOX PAD.
	//
	// What a smin can do to the bound: sminP(a,b,k) = min(a,b) - h*h*k/4 with
	// h = max(k-|a-b|,0)/k in [0,1], so it dips BELOW the hard min by at most
	//     k/4  -- a QUARTER of the blend radius, straight out of the formula --
	// and never rises above it.  Its dual smaxP(a,b,k) = -sminP(-a,-b,k) is
	// therefore always >= max(a,b): intersect and subtract only ever RAISE the
	// field, i.e. shrink the solid, and can be ignored by a bound (subtract
	// entirely; intersect still clips).
	//
	// So the only downward pressure comes from smin, and it is bounded by a
	// SUFFIX SUM.  Let d_i be the accumulator after part i and
	//     T_i = sum over j >= i of ( op_j == smin ? max(k_j,0)/4 : 0 )
	// (built below as `budget`, right-to-left).  Fold invariant, maintained by
	// downward induction with B_0 = empty:
	//     { p : d_i(p) <= T_{i+1} }  is contained in  B_i.
	// Step i+1 with sublevel tau = T_{i+1}:
	//   union:     d_{i+1} = min(d_i, dp) <= T_{i+2} = T_{i+1} forces d_i <= tau
	//              OR dp <= tau -> B_i UNION partBox(tau).
	//   smin(k):   d_{i+1} <= T_{i+2} = T_{i+1} - k/4, and
	//              min(d_i,dp) <= d_{i+1} + k/4 <= T_{i+1} = tau -- the SAME
	//              union, with the k/4 already reserved inside tau.
	//   intersect: d_{i+1} >= max(d_i, dp), so d_{i+1} <= tau forces BOTH
	//              d_i <= tau and dp <= tau -> B_i INTERSECT partBox(tau).
	//   subtract:  d_{i+1} >= d_i, so d_{i+1} <= tau forces d_i <= tau -> B_i.
	// T_N = 0 at the end, which is exactly the containment we want: the solid
	// { Map <= 0 } lies inside the final box.
	//
	// This REPLACES an earlier form that unioned the raw part box and then
	// padded the WHOLE RUNNING BOX by the full k, once per smin part.  That was
	// wrong twice over -- k instead of k/4, and cumulative over the chain, so an
	// N-part blend grew by sum(k) per side.  Measured on a 9-part smin creature
	// (parts extent 0.375 x 0.169 x 0.353, sum(k) = 0.136) it reported
	// 0.622 x 0.413 x 0.575, ~1.7x per axis -- which then drove auto-framing
	// and largest-extent object picking to visibly wrong answers.  It was also
	// UNCONSERVATIVE for anisotropically scaled parts, which the per-part form
	// below handles exactly (see partBox's tau plumbing).
	//
	// (The pad is per-part rather than on the running box for a second reason:
	// the running box has no primitive behind it, so there is nothing to inflate
	// in the part's own local frame -- and the local frame is where the
	// anisotropic-scale correction is exact.)
	//
	// ACCEPTED LOOSENESS, so nobody re-derives it as a bug.  The bound is a
	// UNION of per-part sublevel boxes, and it charges EVERY part the full
	// budget independently.  The real blend region is smaller than that: a
	// point only gets pulled into the solid where BOTH fields are within about
	// 5k/4 of zero at once -- h > 0 needs |d_prev - d_part| < k before the
	// quadratic can subtract anything -- so the true bulge lives in the
	// INTERSECTION of the two neighbourhoods (a collar around the seam), not
	// the union of them.  A part sitting far from every seam therefore pays for
	// a bulge it can never have; on strongly ANISOTROPIC parts, where lambda is
	// amplified by scale_max/scale_min, that shows up as a visibly loose box
	// (the live cat: 1.27x the tight extent instead of ~1.06x).  Deliberately
	// accepted: it errs in the CONSERVATIVE direction, and the form it replaced
	// -- a world-space pad of k -- was outright UNSAFE for exactly these
	// anisotropic parts (see partBox's tau plumbing).  The seam-intersection
	// refinement (intersect each pair's inflated boxes before unioning) is the
	// tightening to reach for if a real scene ever needs it; it costs O(N^2)
	// box work and nothing in the tree has asked for it yet.

	// Per-part smin budget T_i, as a suffix sum (budget[N] = 0).
	//
	// The FINITENESS test is load-bearing, not defensive noise.  `k` is
	// KEYFRAMABLE (`part<i>.blend`), so an eased value can arrive as +inf or
	// NaN.  An infinite budget makes lambda infinite below, which makes every
	// transformed corner NaN, and min/max against NaN keeps the sentinel seeds
	// -- worldAABB would hand back an INVERTED box (ll = +INF, ur = -INF) that
	// propagates into the TLAS / octree as a NaN centroid.  The OLD full-k pad
	// degraded to a well-formed [-inf, +inf] instead, so this is a regression
	// the per-part form has to close explicitly.  A NaN k is already excluded
	// by `k > 0` (every compare against NaN is false); note that sminP does NOT
	// filter it -- a NaN k makes the FIELD NaN at that part -- but a garbage
	// field is no reason to also hand the TLAS a garbage box.
	std::vector<Scalar> budget( m_parts.size() + 1, Scalar(0) );
	for( size_t i = m_parts.size(); i-- > 0; ) {
		const Part& pt = m_parts[i];
		const bool spends = ( pt.op == eOpSmin && pt.k > 0 && RISE::IsFiniteDouble( pt.k ) );
		budget[i] = budget[i+1] + ( spends ? pt.k * Scalar(0.25) : Scalar(0) );
	}

	// World-space AABB of the tau-SUBLEVEL SET of one part's field, i.e. of
	// { p : partEval(pt,p) <= tau }: 8 local corners (inflated for tau) ->
	// scale -> rotate ( R*v = cx*vx + cy*vy + cz*vz ) -> translate.
	//
	// The inflation happens in the LOCAL frame, BEFORE the scale, because that
	// is where it is exact.  partEval is primDist(local) * minScale, so a WORLD
	// sublevel tau is a LOCAL sublevel tau/minScale, and a local sublevel is
	// contained in the local AABB grown by
	//     lambda = ( tau / minScale ) * primFieldStretch(pt)
	// per axis.  Running THAT box through the existing corner transform lets the
	// per-axis scale and the rotation spread lambda correctly and tightly.  A
	// world-space pad of tau cannot do this: for a flattened part (say
	// scale = (1,1,0.01)) the world field is 0.01x the local one along x, so its
	// tau-sublevel genuinely reaches 100*tau out in x -- the blend really can
	// pull surface that far -- and a tau pad would UNDER-bound it.
	auto worldAABB = []( const Part& pt, const Scalar tau, Point3& outMn, Point3& outMx )
	{
		Point3 lmin, lmax;
		primLocalAABB( pt, lmin, lmax );
		// Sort before inflating.  Every primLocalAABB row is min-first for sane
		// authoring, but `size` is KEYFRAMABLE (SetIntermediateValue writes a/b/c
		// raw), and an inverted row would turn the +-lambda growth into a SHRINK.
		// The corner loop below is order-blind, so this costs nothing else.
		Scalar xs[2] = { std::min(lmin.x,lmax.x), std::max(lmin.x,lmax.x) };
		Scalar ys[2] = { std::min(lmin.y,lmax.y), std::max(lmin.y,lmax.y) };
		Scalar zs[2] = { std::min(lmin.z,lmax.z), std::max(lmin.z,lmax.z) };
		// THE SCALE USED HERE IS THE FLOORED ONE, and it has to be: partEval
		// divides the world offset by pt.invScale, which RecomputePartDerived
		// built from the SAME `fabs(s) > 1e-9 ? s : 1e-9` flooring, and takes
		// minScale from those floored magnitudes too.  Read pt.scale RAW in the
		// corner transform and the two halves disagree exactly where it hurts:
		// with every |scale| component at or below the floor, lambda inflates
		// the local box by tau/1e-9 while the transform multiplies it back by
		// ~0, so the reported box collapses to a point even though the world
		// tau-sublevel genuinely reaches tau (the effective world scale is the
		// FLOOR, 1e-9, not the authored 0).  Measured deficit 0.099 -- far past
		// the safety pad -- for scale = (0, 1e-9, 1e-9) on two a = 0.5 spheres
		// with smin k = 0.4.  Recomputed locally rather than cached on the Part
		// so a hand-built Part that never went through RecomputePartDerived is
		// handled the same way.
		const Scalar sxA = ( std::fabs(pt.scale.x) > Scalar(1e-9) ) ? pt.scale.x : Scalar(1e-9);
		const Scalar syA = ( std::fabs(pt.scale.y) > Scalar(1e-9) ) ? pt.scale.y : Scalar(1e-9);
		const Scalar szA = ( std::fabs(pt.scale.z) > Scalar(1e-9) ) ? pt.scale.z : Scalar(1e-9);
		if( tau > Scalar(0) ) {
			// The matching minScale, from the same floored magnitudes -- NOT
			// pt.minScale, which a hand-built Part may never have had derived.
			const Scalar ms  = std::min( std::fabs(sxA), std::min( std::fabs(syA), std::fabs(szA) ) );
			const Scalar lam = ( tau / ms ) * primFieldStretch( pt );
			xs[0] -= lam; xs[1] += lam;
			ys[0] -= lam; ys[1] += lam;
			zs[0] -= lam; zs[1] += lam;
		}
		outMn = Point3(  RISE_INFINITY,  RISE_INFINITY,  RISE_INFINITY );
		outMx = Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );
		for( int cxi = 0; cxi < 2; ++cxi )
		for( int cyi = 0; cyi < 2; ++cyi )
		for( int czi = 0; czi < 2; ++czi )
		{
			const Scalar vx = xs[cxi] * sxA;
			const Scalar vy = ys[cyi] * syA;
			const Scalar vz = zs[czi] * szA;
			const Scalar wx = pt.pos.x + pt.cx.x*vx + pt.cy.x*vy + pt.cz.x*vz;
			const Scalar wy = pt.pos.y + pt.cx.y*vx + pt.cy.y*vy + pt.cz.y*vz;
			const Scalar wz = pt.pos.z + pt.cx.z*vx + pt.cy.z*vy + pt.cz.z*vz;
			outMn.x = std::min( outMn.x, wx ); outMx.x = std::max( outMx.x, wx );
			outMn.y = std::min( outMn.y, wy ); outMx.y = std::max( outMx.y, wy );
			outMn.z = std::min( outMn.z, wz ); outMx.z = std::max( outMx.z, wz );
		}
	};

	Point3 mn(-1,-1,-1), mx(1,1,1);	// empty-field fallback (no parts)
	bool   have = false;

	for( size_t i = 0; i < m_parts.size(); ++i )
	{
		const Part& pt = m_parts[i];
		if( pt.op == eOpSubtract ) {
			continue;	// a carve never extends the solid -> no-op on the bound
		}

		Point3 pmn, pmx;
		worldAABB( pt, budget[i], pmn, pmx );

		if( !have ) {
			// The parser guarantees the first part is union/smin (see
			// SDFGeometry::ParsePartLines / SDFGeometryTest TestFirstOpRule), so
			// the fold seeds from a real additive box, never an intersect/subtract.
			mn = pmn; mx = pmx;
			have = true;
		} else if( pt.op == eOpIntersect ) {
			// running = running INTERSECT partBox.  A degenerate (empty) overlap
			// means the running solid and this clip don't meet -> empty field;
			// keep the running box rather than an inverted one (matches the prior
			// degenerate-overlap fallback so downstream stays well-formed).
			Point3 cmn( std::max(mn.x,pmn.x), std::max(mn.y,pmn.y), std::max(mn.z,pmn.z) );
			Point3 cmx( std::min(mx.x,pmx.x), std::min(mx.y,pmx.y), std::min(mx.z,pmx.z) );
			if( !( cmn.x > cmx.x || cmn.y > cmx.y || cmn.z > cmx.z ) ) {
				mn = cmn; mx = cmx;
			}
		} else {	// eOpUnion / eOpSmin
			mn.x = std::min( mn.x, pmn.x ); mx.x = std::max( mx.x, pmx.x );
			mn.y = std::min( mn.y, pmn.y ); mx.y = std::max( mx.y, pmx.y );
			mn.z = std::min( mn.z, pmn.z ); mx.z = std::max( mx.z, pmx.z );
			// No running-box pad here: the smin bulge this part (and every smin
			// after it) can produce is already inside budget[i], which widened
			// pmn/pmx in the part's own local frame.
		}
	}

	// Safety-margin pad (the smin bulge is already folded in per-part above).
	// INVARIANT: pad > surfBand so rays entering the AABB start strictly outside
	// any surface's eps band -- entering rays must never trip the step-off (that
	// path is for continuation rays spawned ON a surface).  surfBand = 2*m_eps and
	// m_eps scales with the box diagonal, so a fixed 1e-3 pad is too small for a
	// WIDE-but-THIN field (huge diagonal -> band wider than 1e-3): a camera ray
	// entering through the thin face would land inside the band, the step-off would
	// march it into the solid, read the wrong side, and skip the entry face.  Size
	// the pad off the UNPADDED box's eps first, then keep the 1e-3 floor; 3x the
	// provisional eps clears the 2x band with margin (eps grows negligibly when the
	// pad enlarges the diagonal, well within that headroom).
	const Scalar dx0 = mx.x - mn.x, dy0 = mx.y - mn.y, dz0 = mx.z - mn.z;
	const Scalar diag0 = std::sqrt( dx0*dx0 + dy0*dy0 + dz0*dz0 );
	const Scalar eps0  = std::max( diag0 * m_epsFrac, Scalar(1e-6) );
	const Scalar pad   = std::max( Scalar(1e-3), Scalar(3) * eps0 );
	mn.x -= pad; mn.y -= pad; mn.z -= pad;
	mx.x += pad; mx.y += pad; mx.z += pad;
	m_bbox = BoundingBox( mn, mx );

	const Scalar dx = mx.x - mn.x, dy = mx.y - mn.y, dz = mx.z - mn.z;
	m_diagonal = std::sqrt( dx*dx + dy*dy + dz*dz );
	m_eps = std::max( m_diagonal * m_epsFrac, Scalar(1e-6) );
}

// IGeometry::SelfHitRootFloor -- see the header for the full derivation of the
// band and of the Lipschitz divisor.  Defined here because the OWNER criterion
// needs `partEval`, this translation unit's per-part field evaluator.
//
// The shape of the answer is `2 * m_eps / shrink`, where `shrink` is the worst
// factor by which the field UNDER-reports true distance near the queried point.
// `shrink` was the global minimum over every part, which charges the whole
// field the worst squash any lobe applies anywhere -- the 50x over-statement
// the header quotes.  It is now the minimum over the parts that own the point.
//
// THE BAND.  A part is an owner when |partEval| <= band.  The band has to be at
// least twice the widest floor this function can return, because that is the
// distance the probe it feeds may stand off: within that reach a part whose
// surface is nearer than the band can still become the fold's arg-min and drag
// the field down, so it must be charged.  `2 * globalFloor` is exactly that
// bound and is itself computed from the global (widest) shrink, so it does not
// depend on the answer it is helping to compute.
//
// WHY EXCLUSION IS SOUND.  `partEval` multiplies a unit-frame primitive
// distance by the part's conservative `minScale`, so it is a LOWER bound on the
// true distance to that part's surface (that is the property the sphere-tracer
// itself rests on).  Hence |partEval| > band PROVES the surface is farther than
// `band`, and a part that far away cannot be the arg-min anywhere the probe
// reaches.  The test can only ever admit a part it did not have to -- an
// over-statement, the safe direction -- never drop one it did.
//
// BLENDS.  `sminP` / `smaxP` let a part influence the fold from up to `k` away,
// so a blended part's band carries its own `k`.  That covers the VALUE, not the
// GRADIENT: two opposed unit gradients average toward zero across a seam, which
// flattens the field further than any per-part ratio predicts (the review
// measured up to 1.8x under-statement at k = 3).  That is the same
// already-documented blend caveat the header's P2-2 paragraph records, it
// predates this change, and it is NOT charged here -- a probe on such a seam
// misses and takes the probe's graceful entry-payload fallback, which is a
// quality outcome, never a wrong-face adoption.
Scalar SDFGeometry::SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const
{
	(void)localDir; (void)localNormal;

	// Per-part field-growth-per-unit-distance ratio, and the global worst.
	// (<= 1 by construction; a uniformly-scaled part contributes exactly 1.)
	Scalar globalShrink = Scalar(1);
	for( std::size_t i = 0; i < m_parts.size(); i++ ) {
		const Vector3& s = m_parts[i].scale;
		const Scalar maxScale = std::max( std::fabs( s.x ), std::max( std::fabs( s.y ), std::fabs( s.z ) ) );
		const Scalar minScale = std::min( std::fabs( s.x ), std::min( std::fabs( s.y ), std::fabs( s.z ) ) );
		if( maxScale > Scalar(0) && minScale > Scalar(0) ) {
			globalShrink = std::min( globalShrink, minScale / maxScale );
		}
	}
	if( !( globalShrink > Scalar(0) ) ) {
		globalShrink = Scalar(1);			// degenerate (zero-scale) part: no usable ratio
	}

	// Heightfield mode has no parts: nothing to own, keep the bare band.
	if( m_parts.empty() ) {
		return Scalar(2) * m_eps / globalShrink;
	}

	const Scalar band = Scalar(2) * ( Scalar(2) * m_eps / globalShrink );

	Scalar ownerShrink = Scalar(1);
	bool anyOwner = false;
	for( std::size_t i = 0; i < m_parts.size(); i++ ) {
		const Part& pt = m_parts[i];
		const Scalar reach = ( pt.op == eOpUnion ) ? band : ( band + std::fabs( pt.k ) );
		if( !( std::fabs( partEval( pt, localOrigin ) ) <= reach ) ) {
			continue;						// provably farther than the probe can reach
		}
		const Vector3& s = pt.scale;
		const Scalar maxScale = std::max( std::fabs( s.x ), std::max( std::fabs( s.y ), std::fabs( s.z ) ) );
		const Scalar minScale = std::min( std::fabs( s.x ), std::min( std::fabs( s.y ), std::fabs( s.z ) ) );
		if( maxScale > Scalar(0) && minScale > Scalar(0) ) {
			ownerShrink = std::min( ownerShrink, minScale / maxScale );
			anyOwner = true;
		}
	}

	// No qualifying part -- a blend seam, or a degenerate part list.  Fall back
	// to the global minimum, i.e. exactly the previous behaviour.
	const Scalar shrink = anyOwner ? ownerShrink : globalShrink;
	return Scalar(2) * m_eps / shrink;
}

Scalar SDFGeometry::Map( const Point3& p ) const
{
	if( m_isHeightfield ) {
		const Scalar R=m_hfRadius;
		const Scalar u=(p.x+R)/(2*R), v=(p.y+R)/(2*R);
		const Scalar uu=u<0?Scalar(0):(u>1?Scalar(1):u);
		const Scalar vv=v<0?Scalar(0):(v>1?Scalar(1):v);
		const Scalar hgt=m_hfScale*( m_pHeightfield ? m_pHeightfield->Evaluate(uu,vv) : Scalar(0) );
		Scalar d=(p.z-hgt)/m_hfLip;
		// Clip the heightfield to a CIRCULAR disk of radius R (matching the
		// cartesian_disk base, so the silhouette matches the mesh/bump dials):
		// intersect the below-surface half-space with the disk-edge cylinder.
		// rho>0 outside the disk -> the trace sees the vertical rim, not the
		// extrapolated field; max() of two valid lower bounds stays conservative.
		const Scalar rho=std::sqrt(p.x*p.x+p.y*p.y)-R;
		if( rho>d ) d=rho;
		return d;
	}

	return EvaluateParts( m_parts, p );
}

//! Blend-domain-control slice (2026-08-25): see SDFGeometry.h's own doc for
//! why this is a public static -- Map() forwards here so the two can never
//! drift.  Byte-identical to the fold this replaced (verified: the whole
//! existing SDF test suite, unchanged, still passes).
Scalar SDFGeometry::EvaluateParts( const std::vector<Part>& parts, const Point3& p )
{
	Scalar d = Scalar(1e30);
	for( size_t i = 0; i < parts.size(); ++i )
	{
		const Part& pt = parts[i];
		const Scalar dp = partEval( pt, p );
		switch( pt.op )
		{
		case eOpUnion:     d = std::min( d, dp );      break;
		case eOpSmin:      d = sminP( d, dp, pt.k );    break;
		case eOpSubtract:  d = smaxP( d, -dp, pt.k );   break;
		case eOpIntersect: d = smaxP( d, dp, pt.k );    break;
		}
	}
	return d;
}

Vector3 SDFGeometry::GradientNormal( const Point3& p ) const
{
	const Scalar h = m_eps * Scalar(0.75);
	const Scalar gx = Map( Point3( p.x+h, p.y, p.z ) ) - Map( Point3( p.x-h, p.y, p.z ) );
	const Scalar gy = Map( Point3( p.x, p.y+h, p.z ) ) - Map( Point3( p.x, p.y-h, p.z ) );
	const Scalar gz = Map( Point3( p.x, p.y, p.z+h ) ) - Map( Point3( p.x, p.y, p.z-h ) );
	const Scalar len = std::sqrt( gx*gx + gy*gy + gz*gz );
	if( len < Scalar(1e-12) ) return Vector3( 0, 1, 0 );
	return Vector3( gx/len, gy/len, gz/len );
}

Scalar SDFGeometry::CurvatureFDStep() const
{
	// See the header doc.  Both terms matter: m_eps*8 keeps the stencil out
	// of the surface-epsilon noise floor on a tiny object, m_diagonal*5e-4
	// keeps it from collapsing to nothing on a large one.
	return std::max( m_eps * Scalar(8), m_diagonal * Scalar(5e-4) );
}

Scalar SDFGeometry::DivergenceOfUnitNormal( const Point3& y, const Vector3& n, const Scalar hfd ) const
{
	if( !( hfd > Scalar(0) ) ) {
		return Scalar(0);
	}
	// One-sided forward difference of the unit normal field, one axis per
	// component: div n_hat = dnx/dx + dny/dy + dnz/dz.  This is the standard
	// SDF mean-curvature form div(grad f / |grad f|) (= k1 + k2 = 2H),
	// evaluated at an ON-SURFACE point -- callers must project first.
	const Vector3 nX = GradientNormal( Point3( y.x + hfd, y.y, y.z ) );
	const Vector3 nY = GradientNormal( Point3( y.x, y.y + hfd, y.z ) );
	const Vector3 nZ = GradientNormal( Point3( y.x, y.y, y.z + hfd ) );
	const Scalar div = ( ( nX.x - n.x ) + ( nY.y - n.y ) + ( nZ.z - n.z ) ) / hfd;
	// GradientNormal has its own degenerate fallback (a fabricated +Y unit
	// vector where the gradient collapses), which can make this difference
	// meaningless but never non-finite; guard anyway so no consumer can be
	// handed a NaN from a pathological field.
	return RISE::IsFiniteDouble( static_cast<double>( div ) ) ? div : Scalar(0);
}

//////////////////////////////////////////////////////////////////////
// ISurfaceSignalProvider -- PHASE-2 geometry-derived shading signals
// (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §6.2).
//
// Both are pure const functions of the field: no rays, no scene access,
// no locks, no mutable state.  Every render thread calls them
// concurrently on one shared geometry.
//
// Both are LAZY by construction -- they run only when an expression
// actually calls `occlusion()` / `thickness()`, which is why neither
// needs the up-front consumption gate `curv` requires.
//////////////////////////////////////////////////////////////////////

namespace
{
	//! Number of cavity taps.  Five is the Evans/IQ figure and is not
	//! arbitrary: with the geometric tap spacing below it covers three
	//! octaves of scale around the query radius, which is the range over
	//! which a contact-shadow term reads as one continuous signal rather
	//! than as banding.
	const int kOcclusionTaps = 5;
}

bool SDFGeometry::ComputeOcclusion( const SurfaceSignalInfo& hit,
	const Scalar radiusFraction, const bool /*bRadiusIsConstant*/, Scalar& outValue ) const
{
	// A LIVE field answers any radius, constant or computed -- see the
	// header's note on why this provider ignores the constant-radius flag
	// the baked mesh family requires.
	const Point3&  ptObject = hit.ptObject;
	const Vector3& nObject  = hit.nObject;

	// REFUSE rather than fabricate.  The caller (SurfaceSignalInfo) already
	// screens a non-finite / non-positive radius; this is the field-side
	// half: a degenerate bbox has no characteristic length to take a
	// fraction OF, so there is no honest answer to give.
	if( !( m_diagonal > Scalar(0) ) || !( radiusFraction > Scalar(0) ) ) {
		return false;
	}

	// Heightfield mode divides Map() by a single GLOBAL Lipschitz bound
	// (m_hfLip, see ComputeHeightfieldLipschitz) so the whole field is a
	// conservative lower bound everywhere, sized to the field's STEEPEST
	// slope.  The Evans estimator's identity map(p + h*n_hat) == h (used
	// above to say "a plane or convex body reads occ = 0") only holds where
	// the LOCAL slope matches the bound used to derive it -- on a heightfield
	// that is true only at the single steepest point.  Everywhere flatter,
	// Map() under-reports the true distance by a factor of m_hfLip, and the
	// estimator misreads that shortfall as occlusion: a perfectly flat,
	// unoccluded point next to one steep bump reads ao ~= 1/m_hfLip (e.g.
	// ~0.15 for a bump steep enough to need m_hfLip ~= 6.5), not 1.  A
	// locally-normalized estimator (dividing by the LOCAL slope instead of
	// the global bound) could fix this properly; until one exists, refuse
	// honestly rather than report a systematically wrong number.
	if( m_isHeightfield ) {
		return false;
	}

	// Query radius in this geometry's own object-space units.  The tap
	// distances are a fraction OF it, so the whole estimator is
	// scale-relative and the result is transform-invariant.
	const Scalar R = radiusFraction * m_diagonal;
	if( !RISE::IsFiniteDouble( static_cast<double>( R ) ) || !( R > Scalar(0) ) ) {
		return false;
	}

	// The reported hit sits INSIDE the +-m_eps sphere-trace band rather than
	// exactly on the zero set, so Map(p) is a small residual, not 0.
	// Subtracting it below removes that bias from every tap -- without this,
	// a tiny query radius on a large object would read the band residual as
	// occlusion and darken a perfectly convex surface.
	const Scalar d0 = Map( ptObject );

	// occ = SUM w_i * (h_i - d_i) / SUM w_i * h_i, with
	//   h_i = R * 2^(i-N)   (geometrically increasing: R/16 .. R)
	//   w_i = 2^(1-i)       (near taps weighted most -- contact occlusion)
	// The chosen pair makes every w_i*h_i equal (R/16 each here), so each
	// octave contributes the same share of the normalizer and the estimator
	// has no preferred scale within [R/16, R].
	//
	// Both ends are exact and meaningful:
	//   * a plane or a convex body has d_i == h_i for every tap (an SDF's
	//     value at p + h*n is exactly h there) -> occ = 0 -> ao = 1;
	//   * a point whose every tap lands ON the surface (d_i == 0), i.e. a
	//     fully enclosed pocket, gives occ = 1 -> ao = 0.
	Scalar num = Scalar(0);
	Scalar den = Scalar(0);
	Scalar h   = R;
	Scalar w   = Scalar(1);
	for( int i = 0; i < kOcclusionTaps; ++i ) {
		h *= Scalar(0.5);
	}
	// h is now R * 2^-N; walk outward, halving the weight each step.
	for( int i = 0; i < kOcclusionTaps; ++i ) {
		h *= Scalar(2);
		const Point3 tap( ptObject.x + nObject.x*h, ptObject.y + nObject.y*h, ptObject.z + nObject.z*h );
		const Scalar d = Map( tap ) - d0;
		num += w * ( h - d );
		den += w * h;
		w *= Scalar(0.5);
	}

	if( !( den > Scalar(0) ) ) {
		return false;
	}

	Scalar ao = Scalar(1) - ( num / den );
	if( !RISE::IsFiniteDouble( static_cast<double>( ao ) ) ) {
		return false;
	}
	// A conservative (Lipschitz-scaled) part distance can under-report the
	// true distance, and a concave pocket can drive the sum past 1, so clamp
	// -- the interface promises [0,1] and the consumer's own clamp must never
	// be the only one.
	if( ao < Scalar(0) ) ao = Scalar(0);
	if( ao > Scalar(1) ) ao = Scalar(1);
	outValue = ao;
	return true;
}

bool SDFGeometry::ComputeThickness( const SurfaceSignalInfo& hit,
	const Scalar radiusFraction, const bool /*bRadiusIsConstant*/, Scalar& outValue ) const
{
	const Point3&  ptObject = hit.ptObject;
	const Vector3& nObject  = hit.nObject;

	if( !( m_diagonal > Scalar(0) ) || !( radiusFraction > Scalar(0) ) ) {
		return false;
	}

	// Same global-vs-local Lipschitz mis-scaling as ComputeOcclusion above:
	// March() steps through Map(), which on a heightfield is scaled by the
	// single global m_hfLip rather than the local slope, so the marched
	// distance-to-exit is systematically wrong everywhere the local slope is
	// below the global max.  Refuse rather than report it.
	if( m_isHeightfield ) {
		return false;
	}

	const Scalar R = radiusFraction * m_diagonal;
	if( !RISE::IsFiniteDouble( static_cast<double>( R ) ) || !( R > Scalar(0) ) ) {
		return false;
	}

	// Straight INWARD from the hit, to the far side.  March() is the same
	// sphere-trace the intersector uses, including its step-off for a ray
	// that starts ON the surface -- which is exactly our situation, so the
	// far crossing it finds is the exit face, never the entry we started on.
	const Vector3 inward( -nObject.x, -nObject.y, -nObject.z );
	Scalar tExit = Scalar(0);
	if( !March( ptObject, inward, Scalar(0), R, tExit ) ) {
		// No far side within the query radius: the solid is at least as thick
		// as we asked about.  That is a MEASUREMENT, not an absence -- report
		// the saturated 1 rather than refusing, so `thickness(0.02)` reads a
		// flat 1 across a wall's face instead of falling back to the neutral
		// value by accident.
		outValue = Scalar(1);
		return true;
	}

	Scalar t = tExit / R;
	if( !RISE::IsFiniteDouble( static_cast<double>( t ) ) ) {
		return false;
	}
	if( t < Scalar(0) ) t = Scalar(0);
	if( t > Scalar(1) ) t = Scalar(1);
	outValue = t;
	return true;
}

// March along (o + t*dir), dir UNIT, from tStart up to t1, to the next surface.
bool SDFGeometry::March( const Point3& o, const Vector3& dir, const Scalar tStart, const Scalar t1, Scalar& tHit ) const
{
	Scalar t = tStart;
	Scalar d0 = Map( Point3( o.x + dir.x*t, o.y + dir.y*t, o.z + dir.z*t ) );

	// Continuation rays (refraction / reflection / shadow) are spawned EXACTLY on
	// the surface -- PerfectRefractorSPF sets the new ray origin to ri.ptIntersection
	// with no offset.  Our reported hit points sit inside the +-m_eps band (the
	// from-outside march stops at the first dist < m_eps, just shy of the zero
	// crossing), so choosing the march `side` from Map's sign at the start is
	// ambiguous: a refracted ray going inward reads side=+1 and instantly re-hits
	// the band it was spawned on (paper-thin self-hit -> black glass).  Step off the
	// originating band first, then pick the side from a point that is unambiguously
	// inside or outside.  This is the SDF analogue of how the analytic primitives
	// reject the t~=0 self-intersection root and return the next surface.
	//
	// Gate the step-off on the ray ORIGIN sitting in the band, NOT the clipped
	// start `d0`.  A continuation ray is spawned on the surface, so its ORIGIN is
	// in the band and must be stepped past.  A PRIMARY (camera/eye) ray merely
	// ENTERS the bbox from far away: when the surface COINCIDES with the bbox face
	// -- a FLAT heightfield, whose surface z IS the bbox top -- the clipped start
	// `tStart` lands in the +-band by coincidence.  Stepping THAT off marches
	// straight past the entry surface to the far side, so the ray tunnels through
	// (a refractive dielectric over a substrate then never gets entered and renders
	// solid black).  Map(origin) tells them apart: ~0 for a spawned-on-surface
	// continuation ray, large for a primary ray clipped to the bbox.
	const Scalar surfBand = m_eps * Scalar(2);
	const Scalar dOrigin  = Map( o );
	if( std::fabs( dOrigin ) <= surfBand ) {
		bool cleared = false;
		for( int e = 0; e < 64; ++e ) {
			t += m_eps * Scalar(4);
			if( t > t1 || t < 0 ) return false;
			d0 = Map( Point3( o.x + dir.x*t, o.y + dir.y*t, o.z + dir.z*t ) );
			if( std::fabs( d0 ) > surfBand ) { cleared = true; break; }
		}
		if( !cleared ) return false;   // ray runs tangent within the band: no clean crossing
	}

	const Scalar side = ( d0 >= 0 ) ? Scalar(1) : Scalar(-1);

	for( unsigned int i = 0; i < m_maxSteps; ++i )
	{
		const Point3 p( o.x + dir.x*t, o.y + dir.y*t, o.z + dir.z*t );
		const Scalar dist = Map( p );
		if( side * dist < m_eps )
		{
			// Entered the +-m_eps band.  Reporting band ENTRY (the classic
			// sphere-trace stop) leaves the hit up to m_eps/cos(theta) short of
			// the true surface along the ray -- at grazing incidence that beats
			// the light sampler's fixed shadow-shave and falsely self-occludes
			// NEE samples on an emissive SDF.  Refine to the TRUE zero crossing:
			// walk forward in half-band steps until the signed field flips, then
			// bisect the bracket.  If the field instead climbs back out of the
			// band without flipping, this was a TANGENT pass (no crossing) --
			// resume the march rather than manufacture a phantom grazing hit.
			Scalar tA = t;
			Scalar tB = t;
			bool   bracketed = false, rangeEnd = false;
			for( int j = 0; j < 64; ++j ) {
				tB = tA + m_eps * Scalar(0.5);
				if( tB > t1 ) { rangeEnd = true; break; }
				const Scalar dB = Map( Point3( o.x + dir.x*tB, o.y + dir.y*tB, o.z + dir.z*tB ) );
				if( side * dB <= 0 ) { bracketed = true; break; }
				if( side * dB >= m_eps ) { break; }	// tangent exit: resume the march below
				tA = tB;
			}
			if( bracketed ) {
				for( int j = 0; j < 10; ++j ) {		// band/1024 -- effectively exact
					const Scalar tM = ( tA + tB ) * Scalar(0.5);
					const Scalar dM = Map( Point3( o.x + dir.x*tM, o.y + dir.y*tM, o.z + dir.z*tM ) );
					if( side * dM <= 0 ) { tB = tM; }
					else                { tA = tM; }
				}
				tHit = ( tA + tB ) * Scalar(0.5);
				return true;
			}
			if( rangeEnd ) {
				// The true crossing (if any) lies BEYOND the allowed range.  This
				// is the shadow-ray-to-a-surface-sample case: the ray ends just
				// short of the surface (the caller shaves dHowFar), and at grazing
				// incidence the BAND extends inside the range even though the
				// CROSSING does not.  Claiming a hit here falsely self-occludes
				// every grazing NEE sample on an emissive SDF.  No crossing in
				// range = no hit.
				return false;
			}
			// tangent exit, or walk cap exhausted while still inside the band:
			// resume the march from where the walk stopped.  Re-entering the band
			// next iteration continues the walk in capped chunks, so a real
			// crossing further along is still found -- without ever reporting a
			// band ENTRY as if it were a surface.
			t = tB;
			continue;
		}
		t += side * dist;
		if( t > t1 || t < 0 ) return false;
	}
	// Step budget exhausted.  Accept ONLY if we actually converged near the
	// surface (a genuine grazing/asymptotic approach); a march that stalls far
	// from any surface must NOT manufacture a hit -- that becomes a phantom
	// occluder or a firefly photon downstream.  IntersectRay_IntersectionOnly
	// re-tests proximity too; keep IntersectRay consistent.
	tHit = t;
	const Scalar dEnd = Map( Point3( o.x + dir.x*t, o.y + dir.y*t, o.z + dir.z*t ) );
	return std::fabs( dEnd ) < m_eps * Scalar(4);
}

void SDFGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	const Point3& o = ri.ray.origin;
	const Vector3& rd = ri.ray.Dir();
	const Scalar dlen = std::sqrt( rd.x*rd.x + rd.y*rd.y + rd.z*rd.z );
	if( dlen < Scalar(1e-12) ) return;
	const Vector3 d( rd.x/dlen, rd.y/dlen, rd.z/dlen );   // unit march direction

	// clip to bbox (slab test) in Euclidean t
	Scalar tb0 = -RISE_INFINITY, tb1 = RISE_INFINITY;
	const Scalar bl[3] = { m_bbox.ll.x, m_bbox.ll.y, m_bbox.ll.z };
	const Scalar bu[3] = { m_bbox.ur.x, m_bbox.ur.y, m_bbox.ur.z };
	const Scalar oc[3] = { o.x, o.y, o.z };
	const Scalar dc[3] = { d.x, d.y, d.z };
	for( int ax = 0; ax < 3; ++ax )
	{
		if( std::fabs( dc[ax] ) < Scalar(1e-12) ) {
			if( oc[ax] < bl[ax] || oc[ax] > bu[ax] ) return;   // parallel + outside slab
		} else {
			Scalar ta = ( bl[ax] - oc[ax] ) / dc[ax];
			Scalar tb = ( bu[ax] - oc[ax] ) / dc[ax];
			if( ta > tb ) std::swap( ta, tb );
			tb0 = std::max( tb0, ta );
			tb1 = std::min( tb1, tb );
		}
	}
	if( tb1 < tb0 || tb1 < 0 ) return;

	Scalar tStart = std::max( tb0, Scalar(NEARZERO) );
	if( tStart > tb1 ) return;

	Scalar tHit;
	if( !March( o, d, tStart, tb1, tHit ) ) return;

	Point3 hp( o.x + d.x*tHit, o.y + d.y*tHit, o.z + d.z*tHit );
	Vector3 n = GradientNormal( hp );
	bool faceFront = ( n.x*d.x + n.y*d.y + n.z*d.z ) < 0;

	// honour front/back filtering: step past hits the caller doesn't want
	int guard = 0;
	while( ( ( faceFront && !bHitFrontFaces ) || ( !faceFront && !bHitBackFaces ) ) && guard < 8 )
	{
		Scalar tNext;
		if( !March( o, d, tHit + m_eps*Scalar(4), tb1, tNext ) ) return;
		tHit = tNext;
		hp = Point3( o.x + d.x*tHit, o.y + d.y*tHit, o.z + d.z*tHit );
		n = GradientNormal( hp );
		faceFront = ( n.x*d.x + n.y*d.y + n.z*d.z ) < 0;
		++guard;
	}

	// The guard caps how many disallowed crossings we step past.  If it expired
	// while still on a face the caller disabled (a (false,false) query, or >8
	// rejected crossings through alternating lobes), report NO hit rather than a
	// disabled face -- matches IntersectRay_IntersectionOnly so the detailed and
	// fast paths can't disagree.
	if( ( faceFront && !bHitFrontFaces ) || ( !faceFront && !bHitBackFaces ) ) return;

	ri.bHit = true;
	ri.range = tHit / dlen;          // back to ray-parameter units
	ri.ptIntersection = hp;
	ri.vNormal = n;
	ri.vGeomNormal = n;

	ri.ptCoord = ( m_isHeightfield ? Point2( (hp.x+m_hfRadius)/(2*m_hfRadius), (hp.y+m_hfRadius)/(2*m_hfRadius) ) : cylUV( hp, m_bbox ) );

	// Heightfield mode shares the cartesian_disk's linear UV
	// ((x+R)/2R,(y+R)/2R), so it must also share the disk's shading
	// tangent so a common anisotropic `tangent_rotation` (the groove-
	// direction flash) aligns on both realizations.  The default ONB
	// (CreateFromW) derives the u-axis from cross(normal, canonical-Y),
	// which SWIMS as the per-groove normal tilts -- an incoherent base
	// frame.  Ask Object::IntersectRay to instead build the u-axis from
	// world-X projected into the (world-space) shading-normal plane, the
	// same direction the flat disk's constant-+Z normal yields.  We only
	// raise the flag here (object-space normal isn't world-space yet);
	// the projection happens in Object::IntersectRay once vNormal has
	// been transformed.  Parts/CSG mode leaves the flag at its default
	// false and is byte-identical.
	if( m_isHeightfield ) {
		ri.bShadingTangentFromGeometry = true;
	}

	// PHASE-1 GEOMETRY-DERIVED SHADING SIGNALS: DIRECT curvature
	// (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 5.4).
	//
	// The SDF family reports curvature DIRECTLY rather than synthesizing a
	// dndu/dndv pair, because an implicit surface has no natural (u, v) for
	// which those partials would mean anything -- but `div n_hat = k1 + k2`
	// is a few field evaluations away and is smooth and resolution-free,
	// which is exactly what makes this the highest-quality family for the
	// signal.  H = div/2, in OBJECT-space 1/length; Object::IntersectRay
	// divides by |det M|^(1/3) to land it in world measure, the same fold
	// that multiplies scaleHint.  Sign follows the shared convention
	// (positive = convex): the gradient normal is outward, so a sphere of
	// radius r gives div = 2/r and H = +1/r.
	//
	// GATED, and this is the gate's whole reason for existing: three extra
	// GradientNormal calls == ~18 extra Map() evaluations per hit, each
	// O(#parts).  On a scene whose expressions never mention `curv` this is
	// one relaxed atomic load and nothing else.  `derivatives.valid` stays
	// FALSE -- there is no dpdu/dpdv here to be valid -- and `curvatureValid`
	// is deliberately independent of it for that reason.
	// PHASE-2 GEOMETRY-DERIVED SHADING SIGNALS: publish the query channel
	// (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §6.1).  A pointer and two
	// vectors, in THIS geometry's own object space -- deliberately NOT
	// transformed by Object::IntersectRay, because both signals are
	// dimensionless and the provider expects its own frame.
	//
	// UNGATED, unlike the curvature block below, and that asymmetry is the
	// point: this stamp performs no field evaluation at all, whereas the
	// curvature FD costs ~18 Map() calls.  The expensive half of THIS
	// feature -- ComputeOcclusion / ComputeThickness -- runs only if an
	// expression calls the builtin, so laziness already delivers the
	// "costs nothing when unused" guarantee that curvature needs a counter
	// for.  See SurfaceSignalInfo's own doc for the full argument.
	//
	// `n` is the outward field gradient regardless of which face was hit,
	// which is what both estimators want.
	ri.signals.pProvider = this;
	ri.signals.ptObject  = hp;
	ri.signals.nObject   = n;

	if( SurfaceCurvatureDemand::Any() ) {
		ri.derivatives.scaleHint = m_diagonal;
		const Scalar div = DivergenceOfUnitNormal( hp, n, CurvatureFDStep() );
		ri.derivatives.curvature = Scalar(0.5) * div;
		ri.derivatives.curvatureValid = true;
	}

	if( bComputeExitInfo )
	{
		Scalar tEx;
		if( March( o, d, tHit + m_eps*Scalar(4), tb1, tEx ) ) {
			ri.range2 = tEx / dlen;
			Point3 ep( o.x + d.x*tEx, o.y + d.y*tEx, o.z + d.z*tEx );
			Vector3 ne = GradientNormal( ep );
			ri.vNormal2 = ne; ri.vGeomNormal2 = ne; ri.ptExit = ep;
		} else {
			// No exit face found (numerically possible on a near-tangent ray, or a
			// non-closed field).  Use the range2 == 0 "no second hit" sentinel that
			// Object.cpp and the analytic primitives (e.g. TorusGeometry) test, rather
			// than range2 == range, which reads as a zero-thickness slab and slips
			// past those guards.
			ri.range2 = 0; ri.vNormal2 = n; ri.vGeomNormal2 = n; ri.ptExit = hp;
		}
	}
}

bool SDFGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	const Point3& o = ray.origin;
	const Vector3& rd = ray.Dir();
	const Scalar dlen = std::sqrt( rd.x*rd.x + rd.y*rd.y + rd.z*rd.z );
	if( dlen < Scalar(1e-12) ) return false;
	const Vector3 d( rd.x/dlen, rd.y/dlen, rd.z/dlen );

	Scalar tb0 = -RISE_INFINITY, tb1 = RISE_INFINITY;
	const Scalar bl[3] = { m_bbox.ll.x, m_bbox.ll.y, m_bbox.ll.z };
	const Scalar bu[3] = { m_bbox.ur.x, m_bbox.ur.y, m_bbox.ur.z };
	const Scalar oc[3] = { o.x, o.y, o.z };
	const Scalar dc[3] = { d.x, d.y, d.z };
	for( int ax = 0; ax < 3; ++ax )
	{
		if( std::fabs( dc[ax] ) < Scalar(1e-12) ) {
			if( oc[ax] < bl[ax] || oc[ax] > bu[ax] ) return false;
		} else {
			Scalar ta = ( bl[ax] - oc[ax] ) / dc[ax];
			Scalar tb = ( bu[ax] - oc[ax] ) / dc[ax];
			if( ta > tb ) std::swap( ta, tb );
			tb0 = std::max( tb0, ta );
			tb1 = std::min( tb1, tb );
		}
	}
	if( tb1 < tb0 || tb1 < 0 ) return false;

	const Scalar tMaxEuclid = std::min( tb1, dHowFar * dlen );
	const Scalar tStart = std::max( tb0, Scalar(NEARZERO) );
	if( tStart > tMaxEuclid ) return false;

	// March to the first crossing, then honour the front/back-face filter exactly
	// like IntersectRay: step past any crossing whose facing the caller didn't ask
	// for, so this fast (shadow / visibility) path can't disagree with the detailed
	// path.  With both flags set (the usual shadow-ray case) the first hit returns.
	Scalar tHit;
	if( !March( o, d, tStart, tMaxEuclid, tHit ) ) return false;
	for( int guard = 0; guard < 8; ++guard )
	{
		const Point3 hp( o.x + d.x*tHit, o.y + d.y*tHit, o.z + d.z*tHit );
		const Vector3 nrm = GradientNormal( hp );
		const bool faceFront = ( nrm.x*d.x + nrm.y*d.y + nrm.z*d.z ) < 0;
		if( ( faceFront && bHitFrontFaces ) || ( !faceFront && bHitBackFaces ) )
			return ( tHit > NEARZERO ) && ( ( tHit / dlen ) < dHowFar );
		if( !March( o, d, tHit + m_eps*Scalar(4), tMaxEuclid, tHit ) ) return false;
	}
	return false;
}

void SDFGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3( ( m_bbox.ll.x + m_bbox.ur.x )*Scalar(0.5),
	                   ( m_bbox.ll.y + m_bbox.ur.y )*Scalar(0.5),
	                   ( m_bbox.ll.z + m_bbox.ur.z )*Scalar(0.5) );
	radius = m_diagonal * Scalar(0.5);
}

BoundingBox SDFGeometry::GenerateBoundingBox() const
{
	return m_bbox;
}

// Newton-projects p onto the zero set.  Marching-tet vertices start h^2-close
// (linear interpolation of a true distance field along a cell edge), so two
// iterations land within ~m_eps of the sphere-traced surface.  Map is a
// CONSERVATIVE bound (|Map| <= true distance), which only shortens the Newton
// step -- convergence is preserved.
Point3 SDFGeometry::ProjectToSurface( const Point3& p ) const
{
	Point3 q = p;
	for( int i = 0; i < 2; ++i ) {
		const Scalar d = Map( q );
		if( std::fabs( d ) <= m_eps * Scalar(0.25) ) break;
		const Vector3 g = GradientNormal( q );
		q = Point3( q.x - d*g.x, q.y - d*g.y, q.z - d*g.z );
	}
	return q;
}

// Marching TETRAHEDRA surface extraction.
//
// Each grid cell splits into the Freudenthal/Kuhn 6 tetrahedra around the main
// diagonal (corner bitmask: bit0=+x, bit1=+y, bit2=+z; every tet walks 0 -> 7
// one axis at a time).  This decomposition is face-consistent across a uniform
// grid and each tet has NO ambiguous sign cases, so the extracted surface is
// watertight by construction -- no 256-entry marching-cubes tables to get wrong.
//
// Edge crossings are linearly interpolated then Newton-projected onto the zero
// set, and welded through a global-edge-key map (neighbouring tets and cells
// share the same grid edge -> the same vertex index).  Triangles are wound so
// the geometric normal faces AWAY from the generating tet's inside corner,
// i.e. outward, without any extra field evaluations.
void SDFGeometry::GenerateSurfaceMesh( const unsigned int cells, std::vector<Point3>& verts, std::vector<unsigned int>& triIndices ) const
{
	const Scalar ex = m_bbox.ur.x - m_bbox.ll.x;
	const Scalar ey = m_bbox.ur.y - m_bbox.ll.y;
	const Scalar ez = m_bbox.ur.z - m_bbox.ll.z;
	const Scalar longest = std::max( ex, std::max( ey, ez ) );
	if( !(longest > 0) ) {
		return;
	}

	const unsigned int N = cells < 2 ? 2 : cells;
	const Scalar hTarget = longest / Scalar(N);
	const unsigned int nx = std::max( 1u, (unsigned int)std::ceil( ex / hTarget ) );
	const unsigned int ny = std::max( 1u, (unsigned int)std::ceil( ey / hTarget ) );
	const unsigned int nz = std::max( 1u, (unsigned int)std::ceil( ez / hTarget ) );
	const unsigned int X = nx + 1, Y = ny + 1;
	const Scalar hx = ex / Scalar(nx), hy = ey / Scalar(ny), hz = ez / Scalar(nz);

	// Field values cached two z-slabs at a time: full-grid memory is never
	// allocated, so high tessellation detail stays cheap on RAM.
	std::vector<Scalar> slabA( (size_t)X * Y ), slabB( (size_t)X * Y );
	std::vector<Scalar>* slab[2] = { &slabA, &slabB };
	const BoundingBox& bb = m_bbox;

	struct SlabFiller {
		const SDFGeometry* g; const BoundingBox& bb;
		unsigned int X, Y; Scalar hx, hy, hz;
		void fill( std::vector<Scalar>& sl, const unsigned int gz ) const {
			const Scalar pz = bb.ll.z + hz * Scalar(gz);
			for( unsigned int gy = 0; gy < Y; ++gy ) {
				const Scalar py = bb.ll.y + hy * Scalar(gy);
				for( unsigned int gx = 0; gx < X; ++gx ) {
					sl[ (size_t)gy * X + gx ] = g->Map( Point3( bb.ll.x + hx * Scalar(gx), py, pz ) );
				}
			}
		}
	};
	const SlabFiller filler = { this, bb, X, Y, hx, hy, hz };
	filler.fill( *slab[0], 0 );

	// global-grid-edge -> welded vertex index
	std::unordered_map<unsigned long long, unsigned int> edgeVerts;
	edgeVerts.reserve( (size_t)X * Y * 4 );

	// sliver cutoff in |cross|^2 units (|cross| = 2*area)
	const Scalar minCross2 = ( hx * hy ) * ( hx * hy ) * Scalar(1e-12);

	// Member-function-local helpers: local classes inside a member function may
	// access the enclosing class's protected members (ProjectToSurface), which
	// keeps these out of the header.
	struct EdgeWeld {
		const SDFGeometry* g;
		std::unordered_map<unsigned long long, unsigned int>& edgeVerts;
		std::vector<Point3>& verts;
		// welded, projected crossing vertex on the global grid edge (ga, gb)
		unsigned int vert( const unsigned long long ga, const unsigned long long gb,
		                   const Point3& pa, const Point3& pb,
		                   const Scalar da, const Scalar db )
		{
			const unsigned long long klo = ga < gb ? ga : gb;
			const unsigned long long khi = ga < gb ? gb : ga;
			const unsigned long long key = ( klo << 32 ) | khi;
			std::unordered_map<unsigned long long, unsigned int>::const_iterator f = edgeVerts.find( key );
			if( f != edgeVerts.end() ) {
				return f->second;
			}
			const Scalar t = da / ( da - db );		// signs differ, so da - db != 0
			Point3 p( pa.x + t * ( pb.x - pa.x ), pa.y + t * ( pb.y - pa.y ), pa.z + t * ( pb.z - pa.z ) );
			p = g->ProjectToSurface( p );
			const unsigned int id = (unsigned int)verts.size();
			verts.push_back( p );
			edgeVerts.insert( std::make_pair( key, id ) );
			return id;
		}
	};
	EdgeWeld weld = { this, edgeVerts, verts };

	struct TriEmit {
		const std::vector<Point3>& verts;
		std::vector<unsigned int>& triIndices;
		Scalar minCross2;
		// emits (va,vb,vc) wound so the geometric normal faces AWAY from the
		// generating tet's inside corner (i.e. outward); drops slivers.
		void emit( const unsigned int va, const unsigned int vbIn, const unsigned int vcIn, const Point3& insidePt )
		{
			unsigned int vb = vbIn, vc = vcIn;
			if( va == vb || vb == vc || va == vc ) {
				return;
			}
			const Point3& A = verts[va];
			const Point3& B = verts[vb];
			const Point3& C = verts[vc];
			const Vector3 e1( B.x - A.x, B.y - A.y, B.z - A.z );
			const Vector3 e2( C.x - A.x, C.y - A.y, C.z - A.z );
			const Vector3 n( e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x );
			const Scalar n2 = n.x*n.x + n.y*n.y + n.z*n.z;
			if( !( n2 > minCross2 ) ) {
				return;		// sliver / degenerate (also rejects NaN)
			}
			const Scalar side = n.x*( insidePt.x - A.x ) + n.y*( insidePt.y - A.y ) + n.z*( insidePt.z - A.z );
			if( side > 0 ) {
				const unsigned int tmp = vb; vb = vc; vc = tmp;
			}
			triIndices.push_back( va );
			triIndices.push_back( vb );
			triIndices.push_back( vc );
		}
	};
	TriEmit emitter = { verts, triIndices, minCross2 };

	// Freudenthal 6-tet split: every tet walks the main diagonal 0 -> 7.
	static const int kTets[6][4] = {
		{ 0, 1, 3, 7 }, { 0, 1, 5, 7 }, { 0, 2, 3, 7 },
		{ 0, 2, 6, 7 }, { 0, 4, 5, 7 }, { 0, 4, 6, 7 }
	};

	Scalar  val[8];
	Point3  pos[8];
	unsigned long long gid[8];

	int cur = 0;
	for( unsigned int gz = 0; gz < nz; ++gz )
	{
		filler.fill( *slab[1 - cur], gz + 1 );
		const std::vector<Scalar>& lo = *slab[cur];
		const std::vector<Scalar>& hi = *slab[1 - cur];

		for( unsigned int gy = 0; gy < ny; ++gy )
		for( unsigned int gx = 0; gx < nx; ++gx )
		{
			int inCount = 0;
			for( int k = 0; k < 8; ++k ) {
				const unsigned int cx = gx + (unsigned int)( k & 1 );
				const unsigned int cy = gy + (unsigned int)( ( k >> 1 ) & 1 );
				val[k] = ( ( k >> 2 ) & 1 ? hi : lo )[ (size_t)cy * X + cx ];
				if( val[k] < 0 ) ++inCount;
			}
			if( inCount == 0 || inCount == 8 ) {
				continue;	// cell entirely inside or outside: no surface
			}
			for( int k = 0; k < 8; ++k ) {
				const unsigned int cx  = gx + (unsigned int)( k & 1 );
				const unsigned int cy  = gy + (unsigned int)( ( k >> 1 ) & 1 );
				const unsigned int czg = gz + (unsigned int)( ( k >> 2 ) & 1 );
				pos[k] = Point3( bb.ll.x + hx * Scalar(cx), bb.ll.y + hy * Scalar(cy), bb.ll.z + hz * Scalar(czg) );
				gid[k] = (unsigned long long)cx + (unsigned long long)cy * X + (unsigned long long)czg * X * Y;
			}

			for( int t = 0; t < 6; ++t )
			{
				const int* tv = kTets[t];
				int ins[4], outs[4];
				int ni = 0, no = 0;
				for( int k = 0; k < 4; ++k ) {
					if( val[ tv[k] ] < 0 ) ins[ni++] = tv[k];
					else                   outs[no++] = tv[k];
				}
				if( ni == 0 || ni == 4 ) {
					continue;
				}

				if( ni == 1 ) {
					const unsigned int q0 = weld.vert( gid[ins[0]], gid[outs[0]], pos[ins[0]], pos[outs[0]], val[ins[0]], val[outs[0]] );
					const unsigned int q1 = weld.vert( gid[ins[0]], gid[outs[1]], pos[ins[0]], pos[outs[1]], val[ins[0]], val[outs[1]] );
					const unsigned int q2 = weld.vert( gid[ins[0]], gid[outs[2]], pos[ins[0]], pos[outs[2]], val[ins[0]], val[outs[2]] );
					emitter.emit( q0, q1, q2, pos[ ins[0] ] );
				} else if( ni == 3 ) {
					const unsigned int q0 = weld.vert( gid[outs[0]], gid[ins[0]], pos[outs[0]], pos[ins[0]], val[outs[0]], val[ins[0]] );
					const unsigned int q1 = weld.vert( gid[outs[0]], gid[ins[1]], pos[outs[0]], pos[ins[1]], val[outs[0]], val[ins[1]] );
					const unsigned int q2 = weld.vert( gid[outs[0]], gid[ins[2]], pos[outs[0]], pos[ins[2]], val[outs[0]], val[ins[2]] );
					emitter.emit( q0, q1, q2, pos[ ins[0] ] );
				} else {	// ni == 2: quad ring around the cut, split into two tris
					const unsigned int q0 = weld.vert( gid[ins[0]], gid[outs[0]], pos[ins[0]], pos[outs[0]], val[ins[0]], val[outs[0]] );
					const unsigned int q1 = weld.vert( gid[ins[0]], gid[outs[1]], pos[ins[0]], pos[outs[1]], val[ins[0]], val[outs[1]] );
					const unsigned int q2 = weld.vert( gid[ins[1]], gid[outs[1]], pos[ins[1]], pos[outs[1]], val[ins[1]], val[outs[1]] );
					const unsigned int q3 = weld.vert( gid[ins[1]], gid[outs[0]], pos[ins[1]], pos[outs[0]], val[ins[1]], val[outs[0]] );
					emitter.emit( q0, q1, q2, pos[ ins[0] ] );
					emitter.emit( q0, q2, q3, pos[ ins[0] ] );
				}
			}
		}
		cur = 1 - cur;
	}
}

// Tessellates the SDF into an indexed triangle mesh (appended).  `detail` =
// cells along the longest bbox axis, clamped to [8, 512].  Normals are the
// exact field gradient at each (projected) vertex.
bool SDFGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	const unsigned int cells = detail < 8 ? 8 : ( detail > 512 ? 512 : detail );

	std::vector<Point3> verts;
	std::vector<unsigned int> idx;
	GenerateSurfaceMesh( cells, verts, idx );
	if( verts.empty() || idx.size() < 3 ) {
		return false;
	}

	const unsigned int base = (unsigned int)vertices.size();
	for( size_t i = 0; i < verts.size(); ++i ) {
		vertices.push_back( verts[i] );
		normals.push_back( GradientNormal( verts[i] ) );
		coords.push_back( ( m_isHeightfield ? Point2( (verts[i].x+m_hfRadius)/(2*m_hfRadius), (verts[i].y+m_hfRadius)/(2*m_hfRadius) ) : cylUV( verts[i], m_bbox ) ) );
	}
	for( size_t t = 0; t + 2 < idx.size(); t += 3 ) {
		tris.push_back( MakeIndexedTriangleSameIdx( base + idx[t], base + idx[t+1], base + idx[t+2] ) );
	}
	return true;
}

// Lazily builds the uniform-by-area sampling CDF over the tessellated surface.
// Thread-safe via std::call_once; the cache is derived-immutable, so the const
// methods below stay conceptually const.  Only SDFs that are actually surface-
// sampled (area-light emitters, point-set SSS) ever pay this cost.
void SDFGeometry::EnsureSamplingStructure() const
{
	std::call_once( *m_samplingOnce, [this]() {
		std::vector<Point3> verts;
		std::vector<unsigned int> idx;
		GenerateSurfaceMesh( m_samplingDetail, verts, idx );

		// Collect triangles, then order them along a Morton (Z-order) curve of
		// their centroids BEFORE building the CDF, so nearby sampler values land
		// on nearby surface patches (preserves what stratification can survive a
		// surface-curve parameterization).  NB an SDF emitter still measures ~2x
		// the pixel variance of an analytic sphere emitter at equal samples: the
		// visibility/cos gate crosses a surface-filling curve many times (vs one
		// cos-theta interval for the analytic map), which fragments stratification
		// regardless of triangle order.  Mean is unbiased (closed-form harness
		// ratio 1.000); the gap is variance only.
		struct TriTmp { Point3 a, b, c; Scalar area; unsigned long long key; };
		std::vector<TriTmp> tmp;
		tmp.reserve( idx.size() / 3 );
		const Scalar bx = m_bbox.ur.x - m_bbox.ll.x;
		const Scalar by = m_bbox.ur.y - m_bbox.ll.y;
		const Scalar bz = m_bbox.ur.z - m_bbox.ll.z;
		struct MortonExpand {
			static unsigned long long spread( unsigned long long v ) {
				v &= 0x1FFFFF;
				v = ( v | ( v << 32 ) ) & 0x1F00000000FFFFull;
				v = ( v | ( v << 16 ) ) & 0x1F0000FF0000FFull;
				v = ( v | ( v << 8 ) )  & 0x100F00F00F00F00Full;
				v = ( v | ( v << 4 ) )  & 0x10C30C30C30C30C3ull;
				v = ( v | ( v << 2 ) )  & 0x1249249249249249ull;
				return v;
			}
		};
		for( size_t t = 0; t + 2 < idx.size(); t += 3 ) {
			const Point3& A = verts[ idx[t] ];
			const Point3& B = verts[ idx[t+1] ];
			const Point3& C = verts[ idx[t+2] ];
			const Vector3 e1( B.x - A.x, B.y - A.y, B.z - A.z );
			const Vector3 e2( C.x - A.x, C.y - A.y, C.z - A.z );
			const Vector3 n( e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x );
			const Scalar area = Scalar(0.5) * std::sqrt( n.x*n.x + n.y*n.y + n.z*n.z );
			if( !( area > 0 ) ) {
				continue;
			}
			TriTmp tt;
			tt.a = A; tt.b = B; tt.c = C; tt.area = area;
			const Scalar cxn = ( bx > 0 ) ? ( ( A.x + B.x + C.x ) / Scalar(3) - m_bbox.ll.x ) / bx : Scalar(0.5);
			const Scalar cyn = ( by > 0 ) ? ( ( A.y + B.y + C.y ) / Scalar(3) - m_bbox.ll.y ) / by : Scalar(0.5);
			const Scalar czn = ( bz > 0 ) ? ( ( A.z + B.z + C.z ) / Scalar(3) - m_bbox.ll.z ) / bz : Scalar(0.5);
			const unsigned long long qx = (unsigned long long)( clampS( cxn, 0, 1 ) * Scalar(2097151) );
			const unsigned long long qy = (unsigned long long)( clampS( cyn, 0, 1 ) * Scalar(2097151) );
			const unsigned long long qz = (unsigned long long)( clampS( czn, 0, 1 ) * Scalar(2097151) );
			tt.key = MortonExpand::spread( qx ) | ( MortonExpand::spread( qy ) << 1 ) | ( MortonExpand::spread( qz ) << 2 );
			tmp.push_back( tt );
		}
		std::sort( tmp.begin(), tmp.end(), []( const TriTmp& l, const TriTmp& r ) { return l.key < r.key; } );

		// Curvature-corrected triangle weights.  UniformRandomPoint samples a
		// PLANAR chord triangle and Newton-projects onto the zero set; that
		// projection is not area-preserving, so weighting the CDF by raw chord
		// area makes the on-surface density -- and GetArea, which LightSampler
		// turns into pdfPosition = 1/area -- first-order biased by the local
		// chord offset x curvature (the projection of a chord at signed
		// offset d expands/contracts area by J = dA_surface / dA_chord
		// ~= 1 - d * (k1 + k2), with k1 + k2 = div n_hat evaluated at the
		// foot point).  Weighting each triangle by chordArea * Jbar (3-point
		// edge-midpoint quadrature -- exact for the quadratic chord-offset
		// profile, so the per-triangle MEAN of J is captured and the residual
		// within-triangle term is zero-mean) and returning the same corrected
		// measure from GetArea keeps the sampled density and the claimed pdf
		// consistent to the next order in (cell / curvature radius).  J is
		// clamped to [0.5, 2] near creases where FD curvature spikes; the
		// clamp count is reported in the build diagnostic.
		const Scalar hfd = CurvatureFDStep();
		unsigned int jClamped = 0;
		auto jacobianAt = [this, hfd, &jClamped]( const Point3& x ) -> Scalar {
			const Point3 y = ProjectToSurface( x );
			const Vector3 n = GradientNormal( y );
			const Scalar d = ( x.x - y.x )*n.x + ( x.y - y.y )*n.y + ( x.z - y.z )*n.z;
			// one-sided FD divergence of the unit normal = k1 + k2 at y.
			// Extracted to DivergenceOfUnitNormal so the intersection-time
			// `curv` signal (design doc 5.4) shares this exact stencil
			// instead of carrying a second copy of it.
			const Scalar div = DivergenceOfUnitNormal( y, n, hfd );
			Scalar J = Scalar(1) - d * div;
			if( J < Scalar(0.5) ) { J = Scalar(0.5); jClamped++; }
			if( J > Scalar(2) )   { J = Scalar(2);   jClamped++; }
			return J;
		};

		Scalar total = 0;
		m_sampleTris.reserve( tmp.size() );
		for( size_t t = 0; t < tmp.size(); ++t ) {
			const Point3& A = tmp[t].a;
			const Point3& B = tmp[t].b;
			const Point3& C = tmp[t].c;
			const Point3 m1( ( A.x + B.x )*Scalar(0.5), ( A.y + B.y )*Scalar(0.5), ( A.z + B.z )*Scalar(0.5) );
			const Point3 m2( ( B.x + C.x )*Scalar(0.5), ( B.y + C.y )*Scalar(0.5), ( B.z + C.z )*Scalar(0.5) );
			const Point3 m3( ( C.x + A.x )*Scalar(0.5), ( C.y + A.y )*Scalar(0.5), ( C.z + A.z )*Scalar(0.5) );
			const Scalar Jbar = ( jacobianAt( m1 ) + jacobianAt( m2 ) + jacobianAt( m3 ) ) / Scalar(3);
			total += tmp[t].area * Jbar;
			SampleTri st;
			st.a = tmp[t].a; st.b = tmp[t].b; st.c = tmp[t].c; st.cumArea = total;
			m_sampleTris.push_back( st );
		}
		m_surfaceArea = total;

		// Missed-component detector.  Marching tets only see sign changes at
		// CELL CORNERS, so a feature thinner than a cell (a tiny nub, a thin
		// tube) can sphere-trace/render fine yet be ABSENT from this sampling
		// structure -- as an emitter, NEE then never samples it while the
		// BSDF-hit MIS weight still budgets a light-sampling density for it.
		// Definite-miss probe: a cell whose 8 corners share one sign but
		// whose CENTER has the other provably contains surface the mesh
		// missed.  Best-effort lower bound (a sub-half-cell feature can evade
		// the center too); the cure is a higher sampling_detail.
		{
			unsigned int missed = 0;
			Point3 firstMiss( 0, 0, 0 );
			const Scalar ex = m_bbox.ur.x - m_bbox.ll.x;
			const Scalar ey = m_bbox.ur.y - m_bbox.ll.y;
			const Scalar ez = m_bbox.ur.z - m_bbox.ll.z;
			const Scalar longest = std::max( ex, std::max( ey, ez ) );
			if( longest > 0 ) {
				const unsigned int N = m_samplingDetail < 2 ? 2 : m_samplingDetail;
				const Scalar hTarget = longest / Scalar(N);
				const unsigned int nx = std::max( 1u, (unsigned int)std::ceil( ex / hTarget ) );
				const unsigned int ny = std::max( 1u, (unsigned int)std::ceil( ey / hTarget ) );
				const unsigned int nz = std::max( 1u, (unsigned int)std::ceil( ez / hTarget ) );
				const unsigned int X = nx + 1, Y = ny + 1;
				const Scalar hx = ex / Scalar(nx), hy = ey / Scalar(ny), hz = ez / Scalar(nz);
				std::vector<Scalar> slabA( (size_t)X * Y ), slabB( (size_t)X * Y );
				std::vector<Scalar>* slab[2] = { &slabA, &slabB };
				auto fillSlab = [this, X, Y, hx, hy, hz]( std::vector<Scalar>& sl, const unsigned int gz ) {
					const Scalar pz = m_bbox.ll.z + hz * Scalar(gz);
					for( unsigned int gy = 0; gy < Y; ++gy ) {
						const Scalar py = m_bbox.ll.y + hy * Scalar(gy);
						for( unsigned int gx = 0; gx < X; ++gx ) {
							sl[ (size_t)gy * X + gx ] = Map( Point3( m_bbox.ll.x + hx * Scalar(gx), py, pz ) );
						}
					}
				};
				fillSlab( *slab[0], 0 );
				for( unsigned int gz = 0; gz < nz; ++gz ) {
					fillSlab( *slab[1], gz + 1 );
					for( unsigned int gy = 0; gy < ny; ++gy ) {
						for( unsigned int gx = 0; gx < nx; ++gx ) {
							const Scalar c000 = (*slab[0])[ (size_t)gy * X + gx ];
							const Scalar c100 = (*slab[0])[ (size_t)gy * X + gx + 1 ];
							const Scalar c010 = (*slab[0])[ (size_t)( gy + 1 ) * X + gx ];
							const Scalar c110 = (*slab[0])[ (size_t)( gy + 1 ) * X + gx + 1 ];
							const Scalar c001 = (*slab[1])[ (size_t)gy * X + gx ];
							const Scalar c101 = (*slab[1])[ (size_t)gy * X + gx + 1 ];
							const Scalar c011 = (*slab[1])[ (size_t)( gy + 1 ) * X + gx ];
							const Scalar c111 = (*slab[1])[ (size_t)( gy + 1 ) * X + gx + 1 ];
							const bool allPos = c000 > 0 && c100 > 0 && c010 > 0 && c110 > 0 &&
							                    c001 > 0 && c101 > 0 && c011 > 0 && c111 > 0;
							const bool allNeg = c000 < 0 && c100 < 0 && c010 < 0 && c110 < 0 &&
							                    c001 < 0 && c101 < 0 && c011 < 0 && c111 < 0;
							if( !allPos && !allNeg ) {
								continue;	// the mesher saw this cell
							}
							const Point3 ctr( m_bbox.ll.x + hx * ( Scalar(gx) + Scalar(0.5) ),
							                  m_bbox.ll.y + hy * ( Scalar(gy) + Scalar(0.5) ),
							                  m_bbox.ll.z + hz * ( Scalar(gz) + Scalar(0.5) ) );
							const Scalar dc = Map( ctr );
							if( ( allPos && dc < 0 ) || ( allNeg && dc > 0 ) ) {
								if( missed == 0 ) {
									firstMiss = ctr;
								}
								missed++;
							}
						}
					}
					std::swap( slab[0], slab[1] );
				}
			}
			m_missedFeatureCells = missed;
			if( missed > 0 ) {
				char warn[320];
				snprintf( warn, sizeof(warn),
					"SDFGeometry:: sampling mesh PROVABLY missed surface in %u grid cell(s) (first near %.4g %.4g %.4g) -- "
					"the surface-sampling contract is broken, so CanBeAreaLight() reports false (no NEE area light, no SSS; "
					"emission falls back to full-weight BSDF hits).  Raise sampling_detail (currently %u) to resolve the feature",
					missed, (double)firstMiss.x, (double)firstMiss.y, (double)firstMiss.z, m_samplingDetail );
				GlobalLog()->Print( eLog_Warning, warn );
			}
		}

		char diag[200];
		snprintf( diag, sizeof(diag), "SDFGeometry:: sampling structure built: %u triangles, corrected surface area %.6f (detail %u, J-clamped %u, missed-cells %u)",
			(unsigned int)m_sampleTris.size(), (double)m_surfaceArea, m_samplingDetail, jClamped, m_missedFeatureCells );
		GlobalLog()->PrintEasyInfo( diag );
	} );
}

// Uniform-by-area surface sampling: triangle CDF weighted by chord area x
// projection Jacobian (see EnsureSamplingStructure), sqrt-barycentric point on
// the planar triangle, Newton-projected onto the zero set (so the sample lies
// ON the sphere-traced surface and shadow rays see a consistent occluder),
// normal from the exact gradient.  The J-weighted CDF and the J-corrected
// GetArea make the realized on-surface density consistent with the light
// sampler's pdfPosition = 1/GetArea() up to the zero-mean within-triangle
// Jacobian residual and the J clamp near creases -- one order better than the
// raw chord-area CDF, but NOT exact: surface components the sampling mesh
// missed entirely are never sampled (see SuspectedMissedFeatureCells).
void SDFGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	EnsureSamplingStructure();

	if( m_sampleTris.empty() ) {
		// Degenerate field (no extractable surface).  CanBeAreaLight() == false
		// keeps samplers away from this path; return something deterministic.
		const Point3 ctr( ( m_bbox.ll.x + m_bbox.ur.x )*Scalar(0.5),
		                  ( m_bbox.ll.y + m_bbox.ur.y )*Scalar(0.5),
		                  ( m_bbox.ll.z + m_bbox.ur.z )*Scalar(0.5) );
		if( point )  { *point  = ctr; }
		if( normal ) { *normal = GradientNormal( ctr ); }
		if( coord )  { *coord  = Point2( prand.x, prand.y ); }
		return;
	}

	// Pick a triangle by the area CDF, then REUSE the pick variable: rescaling
	// (target - cumPrev) / triArea yields a fresh uniform in [0,1), so the whole
	// sample needs only TWO random dims (prand.x, prand.y) -- the same consumption
	// pattern as every other geometry's UniformRandomPoint.  Consuming a third
	// dim is hazardous under stratified / padded samplers (dims can be
	// correlated), and measurably biased the NEE estimate when prand.z fed the
	// barycentric directly.
	const Scalar r0 = clampS( prand.x, Scalar(0), Scalar(0.9999999) );
	const Scalar target = r0 * m_surfaceArea;
	size_t lo = 0, hi = m_sampleTris.size() - 1;
	while( lo < hi ) {
		const size_t mid = ( lo + hi ) >> 1;
		if( m_sampleTris[mid].cumArea < target ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	const SampleTri& st = m_sampleTris[lo];
	const Scalar cumPrev = ( lo > 0 ) ? m_sampleTris[lo-1].cumArea : Scalar(0);
	const Scalar triArea = st.cumArea - cumPrev;
	const Scalar r1 = ( triArea > 0 ) ? clampS( ( target - cumPrev ) / triArea, Scalar(0), Scalar(1) ) : clampS( prand.z, Scalar(0), Scalar(1) );

	// uniform point on the triangle (sqrt warp)
	const Scalar sq = std::sqrt( r1 );
	const Scalar r2 = clampS( prand.y, Scalar(0), Scalar(1) );
	const Scalar wa = Scalar(1) - sq;
	const Scalar wb = sq * ( Scalar(1) - r2 );
	const Scalar wc = sq * r2;
	Point3 pSample( st.a.x*wa + st.b.x*wb + st.c.x*wc,
	                st.a.y*wa + st.b.y*wb + st.c.y*wc,
	                st.a.z*wa + st.b.z*wb + st.c.z*wc );
	pSample = ProjectToSurface( pSample );

	if( point )  { *point  = pSample; }
	if( normal ) { *normal = GradientNormal( pSample ); }
	if( coord )  { *coord  = ( m_isHeightfield ? Point2( (pSample.x+m_hfRadius)/(2*m_hfRadius), (pSample.y+m_hfRadius)/(2*m_hfRadius) ) : cylUV( pSample, m_bbox ) ); }
}

Scalar SDFGeometry::GetArea() const
{
	EnsureSamplingStructure();
	return m_surfaceArea;
}

unsigned int SDFGeometry::SuspectedMissedFeatureCells() const
{
	EnsureSamplingStructure();
	return m_missedFeatureCells;
}

//////////////////////////////////////////////////////////////////////
// Keyframable interface -- animate the FIELD (per-part geometry + the
// heightfield amplitude).  The timeline targets the geometry directly
// (`element_type geometry`); the wrapping standard_object keeps owning
// the rigid transform.  Per-part fields are packed into the keyframe
// parameter id as  id = partIndex * SDF_FIELDS_PER_PART + fieldCode,
// which SetIntermediateValue decodes back to (part, field).
//////////////////////////////////////////////////////////////////////
namespace
{
	const unsigned int SDF_FIELDS_PER_PART = 8;	// stride per part (room beyond the 6 fields below)
	enum SDFKeyframeField
	{
		eKFPos   = 0,	//!< Point3  : part origin (object space)
		eKFRot   = 1,	//!< Vector3 : Euler degrees (Rz*Ry*Rx)
		eKFScale = 2,	//!< Vector3 : per-axis scale
		eKFSize  = 3,	//!< Vector3 : primitive size params (a,b,c)
		eKFK     = 4,	//!< Scalar  : smin / boolean blend radius
		eKFRound = 5	//!< Scalar  : extra rounding radius
	};
	// Heightfield amplitude: a single id placed FAR above any realistic
	// partIndex*8+field, so the two id spaces can never collide.
	const unsigned int SDF_HF_SCALE_ID = 0x10000001u;
}

IKeyframeParameter* SDFGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	// Heightfield-mode amplitude (morphs the displacement depth).
	if( name == "heightfield_scale" ) {
		if( !m_isHeightfield ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SDFGeometry:: keyframe `heightfield_scale` ignored -- this SDF is not in heightfield mode" );
			return 0;
		}
		Scalar v;
		if( !ParseStrictScalar( value, v ) ) {
			return 0;
		}
		IKeyframeParameter* p = new Parameter<Scalar>( v, SDF_HF_SCALE_ID );
		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
		return p;
	}

	// Per-part fields: `part<i>.<field>`.  Parse the index with strtoul rather
	// than `%u` (which SILENTLY WRAPS an out-of-range integer to a small valid
	// index, e.g. "part4294967296.*" -> part 0, mis-targeting the wrong part).
	// Require the literal `part` prefix, a real digit run, a sane index ceiling
	// (also catches strtoul's overflow -> ULONG_MAX), and a `.` before the field.
	const char* nm = name.c_str();
	if( strncmp( nm, "part", 4 ) != 0 ) {
		return 0;	// not a name this geometry recognizes -- let the caller report "unknown"
	}
	const char* digits = nm + 4;
	char* endp = 0;
	const unsigned long parsed = strtoul( digits, &endp, 10 );
	if( endp == digits || *endp != '.' || parsed > 0xFFFFFFul ) {
		return 0;	// no digits / missing dot / absurd-or-overflowing index
	}
	const unsigned int idx = (unsigned int)parsed;
	char field[64] = {0};
	strncpy( field, endp + 1, sizeof(field) - 1 );	// field token after the dot (bounded, NUL-padded)
	if( idx >= m_parts.size() ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SDFGeometry:: keyframe `%s` ignored -- part index %u is out of range (%u part(s))",
			name.c_str(), idx, (unsigned int)m_parts.size() );
		return 0;
	}

	IKeyframeParameter* p = 0;
	double d[3];
	Scalar s;
	if( !strcmp( field, "position" ) ) {
		if( ParseStrictVec3( value, d ) ) p = new Point3Keyframe ( Point3 ( d[0], d[1], d[2] ), idx*SDF_FIELDS_PER_PART + eKFPos );
	} else if( !strcmp( field, "rotation" ) ) {
		if( ParseStrictVec3( value, d ) ) p = new Vector3Keyframe( Vector3( d[0], d[1], d[2] ), idx*SDF_FIELDS_PER_PART + eKFRot );
	} else if( !strcmp( field, "scale" ) ) {
		if( ParseStrictVec3( value, d ) ) p = new Vector3Keyframe( Vector3( d[0], d[1], d[2] ), idx*SDF_FIELDS_PER_PART + eKFScale );
	} else if( !strcmp( field, "size" ) ) {
		if( ParseStrictVec3( value, d ) ) p = new Vector3Keyframe( Vector3( d[0], d[1], d[2] ), idx*SDF_FIELDS_PER_PART + eKFSize );
	} else if( !strcmp( field, "blend" ) ) {
		if( ParseStrictScalar( value, s ) ) p = new Parameter<Scalar>( s, idx*SDF_FIELDS_PER_PART + eKFK );
	} else if( !strcmp( field, "round" ) ) {
		if( ParseStrictScalar( value, s ) ) p = new Parameter<Scalar>( s, idx*SDF_FIELDS_PER_PART + eKFRound );
	} else {
		GlobalLog()->PrintEx( eLog_Warning,
			"SDFGeometry:: keyframe `%s` ignored -- unknown part field `%s` (want position|rotation|scale|size|blend|round)",
			name.c_str(), field );
		return 0;
	}

	if( p ) {
		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	}
	return p;
}

void SDFGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	const unsigned int id = val.getID();

	if( id == SDF_HF_SCALE_ID ) {
		m_hfScale = *(Scalar*)val.getValue();
		return;	// m_hfLip + bounds are re-derived in RegenerateData (once per frame)
	}

	const unsigned int idx   = id / SDF_FIELDS_PER_PART;
	const unsigned int field = id % SDF_FIELDS_PER_PART;
	if( idx >= m_parts.size() ) {
		return;	// defensive: a stale id from a parts list that changed underneath us
	}
	Part& pt = m_parts[idx];
	switch( field )
	{
	case eKFPos:   pt.pos   = *(Point3*) val.getValue();                                  break;
	case eKFRot:   pt.euler = *(Vector3*)val.getValue(); RecomputePartDerived( pt );      break;
	case eKFScale: pt.scale = *(Vector3*)val.getValue(); RecomputePartDerived( pt );      break;
	case eKFSize:  { const Vector3 sz = *(Vector3*)val.getValue(); pt.a = sz.x; pt.b = sz.y; pt.c = sz.z; } break;
	case eKFK:     pt.k     = *(Scalar*) val.getValue();                                  break;
	case eKFRound: pt.round = *(Scalar*) val.getValue();                                  break;
	}
}

void SDFGeometry::RegenerateData()
{
	// Re-derive everything downstream of the (now animated) field: the
	// heightfield Lipschitz bound, the order-folded AABB + surface epsilon,
	// and the lazily-built area-light / SSS sampling cache.  Runs ONCE per
	// element per frame, after all that element's SetIntermediateValue calls.
	//
	// This MUTATES shared geometry state (m_bbox, m_hfLip, the sampling cache)
	// and so must NOT overlap the parallel render of a frame.  The engine
	// guarantees that: frame-stepping evaluates the animator BETWEEN frames
	// (before RenderParallelScope is entered), and the per-sample motion-blur
	// path (exposure>0) is forced single-threaded for the whole frame
	// (PixelBasedRasterizerHelper's `threads>1 && exposure==0` gate).  The
	// assert in InvalidateSamplingStructure makes that contract self-enforcing.
	if( m_isHeightfield ) {
		ComputeHeightfieldLipschitz();
	}
	ComputeBounds();
	InvalidateSamplingStructure();
}

void SDFGeometry::InvalidateSamplingStructure()
{
	// DEBUG freeze guard (mirrors DisplacedGeometry::Realize): reassigning the
	// once_flag frees the old one, which would be use-after-free if a worker
	// were mid `std::call_once(*m_samplingOnce, ...)`.  That cannot happen while
	// mutation stays single-threaded (see RegenerateData); assert it so a future
	// change that parallelizes animation/photon evaluation fails LOUDLY here
	// instead of corrupting the heap.  Compiles out in release.
	assert( g_renderParallelDepth.load( std::memory_order_seq_cst ) == 0 &&
		"SDFGeometry sampling-cache invalidation during the parallel render — animator must be evaluated single-threaded between frames" );

	// Swap in a fresh once_flag so the next GetArea / UniformRandomPoint
	// rebuilds the CDF against the current (animated) surface.
	m_samplingOnce = std::make_unique<std::once_flag>();
	m_sampleTris.clear();
	m_surfaceArea = 0;
	m_missedFeatureCells = 0;
}

//////////////////////////////////////////////////////////////////////
// ParsePartLines
//
// The ONE part grammar, shared by the scene chunk's inline `part`
// lines and external parts files (one part per line; blank lines and
// `#` comments skipped):
//
//   <prim> <op> <k>  <px py pz>  <exDeg eyDeg ezDeg>  <sx sy sz>  <a b c>  <round>
//
// No silent fallback: an unknown primitive / op token, a malformed
// line, or trailing tokens fail the parse with `szContext` + 1-based
// line number (a typo like `round_box` / `substract` must not render
// a different shape).  When a future extension adds shapes, ops, or
// per-part fields, extend THIS parser (and the token maps below) --
// the scene chunk forwards `part` lines verbatim, so the chunk parser
// needs no change.
//////////////////////////////////////////////////////////////////////
namespace
{
	bool ParseSDFPrimToken( const char* s, SDFGeometry::SDFPrim& out )
	{
		if( !strcmp(s,"sphere") )    { out = SDFGeometry::ePrimSphere;    return true; }
		if( !strcmp(s,"box") )       { out = SDFGeometry::ePrimBox;       return true; }
		if( !strcmp(s,"roundbox") )  { out = SDFGeometry::ePrimRoundBox;  return true; }
		if( !strcmp(s,"cylinder") )  { out = SDFGeometry::ePrimCylinder;  return true; }
		if( !strcmp(s,"torus") )     { out = SDFGeometry::ePrimTorus;     return true; }
		if( !strcmp(s,"capsule") )   { out = SDFGeometry::ePrimCapsule;   return true; }
		if( !strcmp(s,"roundcone") ) { out = SDFGeometry::ePrimRoundCone; return true; }
		if( !strcmp(s,"superellipsoid") ) { out = SDFGeometry::ePrimSuperellipsoid; return true; }
		return false;
	}
	bool ParseSDFOpToken( const char* s, SDFGeometry::SDFOp& out )
	{
		if( !strcmp(s,"union") )     { out = SDFGeometry::eOpUnion;     return true; }
		if( !strcmp(s,"smin") )      { out = SDFGeometry::eOpSmin;      return true; }
		if( !strcmp(s,"subtract") )  { out = SDFGeometry::eOpSubtract;  return true; }
		if( !strcmp(s,"intersect") ) { out = SDFGeometry::eOpIntersect; return true; }
		return false;
	}
}

bool SDFGeometry::ParsePartLines(
	const char* szSource,
	const char* szContext,
	std::vector<Part>& out )
{
	if( !szSource ) {
		return false;
	}
	const char* ctx = ( szContext && szContext[0] ) ? szContext : "(unknown source)";

	const char* p = szSource;
	unsigned int lineNo = 0;
	while( *p ) {
		const char* eol = p;
		while( *eol && *eol != '\n' ) {
			++eol;
		}
		lineNo++;

		std::string line( p, static_cast<std::string::size_type>( eol - p ) );
		p = ( *eol == '\n' ) ? eol + 1 : eol;

		// Strip `#` comments, then skip blank lines.
		const std::string::size_type hash = line.find( '#' );
		if( hash != std::string::npos ) {
			line.erase( hash );
		}
		bool blank = true;
		for( std::string::size_type i = 0; i < line.size(); ++i ) {
			if( !isspace( static_cast<unsigned char>( line[i] ) ) ) { blank = false; break; }
		}
		if( blank ) {
			continue;
		}

		char ts[64] = {0}, os[64] = {0};
		double k, px, py, pz, ex, ey, ez, sx, sy, sz, a, b, c, rnd;
		int consumed = 0;
		const int got = sscanf( line.c_str(),
			" %63s %63s %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %n",
			ts, os, &k, &px, &py, &pz, &ex, &ey, &ez, &sx, &sy, &sz, &a, &b, &c, &rnd, &consumed );
		bool trailing = false;
		if( got == 16 ) {
			for( const char* t = line.c_str() + consumed; *t; ++t ) {
				if( !isspace( static_cast<unsigned char>( *t ) ) ) { trailing = true; break; }
			}
		}
		if( got != 16 || trailing ) {
			GlobalLog()->PrintEx( eLog_Error,
				"SDFGeometry::ParsePartLines:: malformed part at line %u of %s (want `<prim> <op> <k> <px py pz> <ex ey ez> <sx sy sz> <a b c> <round>`, exactly 16 tokens)",
				lineNo, ctx );
			return false;
		}

		SDFPrim prim;
		SDFOp   op;
		if( !ParseSDFPrimToken( ts, prim ) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"SDFGeometry::ParsePartLines:: unknown primitive `%s` at line %u of %s (want sphere|box|roundbox|cylinder|torus|capsule|roundcone|superellipsoid)",
				ts, lineNo, ctx );
			return false;
		}
		if( !ParseSDFOpToken( os, op ) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"SDFGeometry::ParsePartLines:: unknown op `%s` at line %u of %s (want union|smin|subtract|intersect)",
				os, lineNo, ctx );
			return false;
		}

		// A superellipsoid's b / c are its EXPONENTS (e1 north-south, e2
		// east-west), and only [kSEMinExp, kSEMaxExp] is supported -- above
		// kSEMaxExp the solid leaves the convex regime and the primitive's
		// distance bound stops being conservative (a sphere-trace overshoot).
		// The field clamps unconditionally (it has to: `size` is keyframable,
		// so a runtime value can arrive without passing through here), which
		// would make an out-of-range authored exponent SILENTLY render a
		// different shape.  Clamp here too, purely so the author gets told.
		// Clamp-and-warn rather than reject: the nearest supported shape is a
		// useful answer, and an unparseable line would fail the whole scene
		// load over what is a taste-level authoring slip.  (NaN compares
		// unequal to its clamp, so a NaN exponent warns here as well.)
		if( prim == ePrimSuperellipsoid ) {
			const Scalar cb = clampSuperExp( b ), cc = clampSuperExp( c );
			if( cb != b || cc != c ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SDFGeometry::ParsePartLines:: superellipsoid exponents (%g, %g) at line %u of %s are outside "
					"the supported [%g, %g]; clamped to (%g, %g).  Above the max the distance bound stops being "
					"conservative; below the min use `box`, which is exact and cheaper",
					b, c, lineNo, ctx, kSEMinExp, kSEMaxExp, cb, cc );
				b = cb; c = cc;
			}
		}

		// DEGENERATE-BY-CONSTRUCTION shape params: an authoring slip seen in
		// the wild is putting a shape's SIZE into <sx sy sz> (the per-part
		// SCALE, applied on top of the primitive) and leaving <a b c> -- the
		// primitive's own shape parameters, per primDist's switch above -- at
		// their default 0 0 0.  A box built that way is a single point (a
		// 0.002-wide degenerate bbox is the field symptom): wrong bbox feeds
		// wrong scaleHint/curv/occlusion radii downstream, and GetArea() on an
		// emissive/SSS part goes to 0.  Checked per-primitive against the
		// EXACT zero-critical combination that collapses primDist to a point
		// -- an unused slot (e.g. a sphere's b/c) must never suppress this,
		// and a combination that primDist maps to a DIFFERENT, still-valid
		// primitive (roundbox with a=b=c=0 but round>0 is an exact sphere of
		// radius `round`; torus with a=0 but b>0 is an exact sphere of radius
		// b -- both via sdRoundBox / sdTorusY's own algebra) must not warn.
		// WARN, don't fail -- matching the superellipsoid clamp above: the
		// part still parses and composes (as a point, or not at all), and
		// failing the whole chunk over one degenerate part would be worse
		// than the degenerate part itself.
		{
			const char* degenerateWhy = 0;
			switch( prim )
			{
			case ePrimSphere:
				if( a == 0.0 ) { degenerateWhy = "radius `a` is 0"; }
				break;
			case ePrimBox:
				if( a == 0.0 && b == 0.0 && c == 0.0 ) { degenerateWhy = "half-extents <a b c> are all 0"; }
				break;
			case ePrimRoundBox:
				if( a == 0.0 && b == 0.0 && c == 0.0 && rnd == 0.0 ) { degenerateWhy = "half-extents <a b c> and `round` are all 0"; }
				break;
			case ePrimCylinder:
				if( a == 0.0 && b == 0.0 ) { degenerateWhy = "radius `a` and half-height `b` are both 0"; }
				break;
			case ePrimTorus:
				if( a == 0.0 && b == 0.0 ) { degenerateWhy = "ring radius `a` and tube radius `b` are both 0"; }
				break;
			case ePrimCapsule:
				if( a == 0.0 && b == 0.0 ) { degenerateWhy = "radius `a` and half-length `b` are both 0"; }
				break;
			case ePrimRoundCone:
				if( a == 0.0 && b == 0.0 && c == 0.0 ) { degenerateWhy = "base radius `a`, tip radius `b`, and length `c` are all 0"; }
				break;
			case ePrimSuperellipsoid:
				if( a == 0.0 ) { degenerateWhy = "radius `a` is 0"; }
				break;
			default:
				break;
			}
			if( degenerateWhy ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SDFGeometry::ParsePartLines:: part %u (`%s`) at line %u of %s has degenerate shape parameters "
					"(%s) -- the part collapses to a single point.  Did you put the shape's size into the <sx sy sz> "
					"scale slot instead of <a b c>?  <sx sy sz> is a pre-transform scale, not the shape's size",
					(unsigned int)out.size(), ts, lineNo, ctx, degenerateWhy );
			}
		}

		// The running field starts EMPTY (Map's fold begins at +1e30), so a
		// first part of `subtract` / `intersect` composes against nothing and
		// yields an empty always-miss field that would otherwise parse fine.
		// Inline authoring makes that an easy mistake -- hard-fail it.
		// (`smin` against the empty field degenerates to plain union -- the
		// polynomial blend weight is 0 at distance |1e30 - d| >> k -- so it
		// IS allowed first.)
		if( out.empty() && ( op == eOpSubtract || op == eOpIntersect ) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"SDFGeometry::ParsePartLines:: first part at line %u of %s has op `%s` -- the field starts empty, so the first part must be `union` or `smin`",
				lineNo, ctx, os );
			return false;
		}

		out.push_back( MakePart( prim, op, k,
			Point3(px,py,pz), ex, ey, ez, Vector3(sx,sy,sz), a, b, c, rnd ) );
	}
	return true;
}
