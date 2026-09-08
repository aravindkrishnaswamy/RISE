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
#include "../Utilities/OrthonormalBasis3D.h"	// occlusion's cosine-weighted march directions

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

// Smallest and largest of a part's scale MAGNITUDES, floored at 1e-9 -- the
// same flooring `RecomputePartDerived` applies before deriving `invScale` and
// `minScale`, and the same one `ComputeBounds` documents at its corner
// transform.  Both outputs are >= 1e-9, so the ratio is always a usable
// positive number and a NaN component floors to 1e-9 rather than poisoning it.
static void FlooredScaleExtremes_( const Vector3& s, Scalar& outMin, Scalar& outMax )
{
	const Scalar sx = ( std::fabs( s.x ) > Scalar(1e-9) ) ? std::fabs( s.x ) : Scalar(1e-9);
	const Scalar sy = ( std::fabs( s.y ) > Scalar(1e-9) ) ? std::fabs( s.y ) : Scalar(1e-9);
	const Scalar sz = ( std::fabs( s.z ) > Scalar(1e-9) ) ? std::fabs( s.z ) : Scalar(1e-9);
	outMin = std::min( sx, std::min( sy, sz ) );
	outMax = std::max( sx, std::max( sy, sz ) );
}

// Smallest per-axis scale MAGNITUDE a part may be AUTHORED with, and the verdict
// of checking one against it (adversarial review of 384e3752, P1-2).
//
// `<sx sy sz>` is a pre-transform scale on the primitive's own unit frame, and
// nothing downstream treats a 0 there as an error: `RecomputePartDerived` floors
// the magnitude at 1e-9 so `invScale` stays finite, and `SelfHitRootFloor` then
// reads the part's Lipschitz shrink as that floored `min/max` -- 1e-9 -- and
// divides the sphere-tracer's step-off band by it.  Measured on a unit sphere
// part (epsFrac 1e-5, field 2.83 units across): `scale (1,1,1)` claims 6.9e-5,
// `scale (1,1,0)` claims 5.66e4 -- nine orders wider, twenty thousand times the
// field's own size.  Inside a CSG composite that floor is an ownership-ray
// REACH, so the degenerate part charges its floor on geometry it has nothing to
// do with -- a wrong-face payload from one mistyped digit.
//
// So the authoring surfaces refuse to pass a 0 through silently.  1e-6 is the
// clamp because it is `m_eps`'s own absolute floor -- the smallest length this
// geometry treats as meaningful anywhere -- and it is 1e3 above the 1e-9 poison
// floor; a part that genuinely wants to be a millionth of its primitive should
// scale the primitive's `<a b c>` instead, which costs the field no Lipschitz
// quality at all.  The clamp is a diagnostic, NOT the guard: at 1e-6 the same
// part still claims 5.66e1 on that field, and what bounds it is
// `SelfHitRootFloor`'s bbox-diagonal cap below.  That is also why nothing is
// clamped at the CONSTRUCTOR -- the cap is the load-bearing layer, and the tests
// need a way to build a degenerate field to prove it works.
static const Scalar kMinPartScaleMag = Scalar(1e-6);

enum PartScaleVerdict { ePartScaleOk = 0, ePartScaleClamped = 1, ePartScaleNonFinite = 2 };

//! Clamps every sub-1e-6 component of `s` to +-1e-6 (sign preserved -- a
//! negative scale is a legitimate mirror) and reports what happened.  A
//! non-finite component is reported WITHOUT touching `s`: there is no sensible
//! magnitude to clamp a NaN to, so the caller rejects the whole value instead.
static int SanitizePartScale_( Vector3& s )
{
	Scalar* const comp[3] = { &s.x, &s.y, &s.z };
	for( int i = 0; i < 3; i++ ) {
		if( !RISE::IsFiniteDouble( static_cast<double>( *comp[i] ) ) ) {
			return ePartScaleNonFinite;
		}
	}
	int verdict = ePartScaleOk;
	for( int i = 0; i < 3; i++ ) {
		if( std::fabs( *comp[i] ) < kMinPartScaleMag ) {
			*comp[i] = ( *comp[i] < Scalar(0) ) ? -kMinPartScaleMag : kMinPartScaleMag;
			verdict = ePartScaleClamped;
		}
	}
	return verdict;
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
//
// INCIDENCE.  `2 * m_eps / shrink` is a distance PERPENDICULAR to the surface:
// the field is a perpendicular distance, and the step-off band is measured in
// it.  The interface's answer is a RANGE along `localDir`, and a ray leaving at
// `theta` off the normal covers only `cos(theta)` of perpendicular distance per
// unit of range -- so the band divides by the incidence cosine, exactly as
// `BoxGeometry::SelfHitRootFloor` divides its plane-distance band.  Ignoring it
// under-states by `1/cos(theta)`, which grows without bound: measured on a
// single uniform SDF sphere R=3, claim/bisected-gate came out 1.00 at normal
// incidence, 0.866 at 30 deg, 0.707 at 45, 0.500 at 60 and 0.26 at 75 -- i.e.
// past 30 deg the claim is BELOW the standoff the sphere-tracer actually
// enforces, and a probe that trusts it is marched straight past its own face.
// The cosine is clamped at 1/20, the same grazing clamp BoxGeometry and
// CSGObject's probe use, so a tangential query returns a large but finite
// number.  A caller that hands in an unusable normal gets no division at all
// rather than a blanket 20x.
//
// The owner BAND divides by the same cosine, because the band is defined as
// twice the widest floor this call can return and that is what the probe it
// feeds may travel.  Both inputs to it -- the global shrink and the incidence
// -- are arguments, so it still does not depend on the answer it helps compute.
Scalar SDFGeometry::SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const
{
	// Per-part field-growth-per-unit-distance ratio, and the global worst.
	// (<= 1 by construction; a uniformly-scaled part contributes exactly 1.)
	//
	// THE MAGNITUDES ARE FLOORED AT 1e-9, matching `RecomputePartDerived` (and
	// the same flooring `ComputeBounds` documents at its own corner transform).
	// `partEval` multiplies by `pt.minScale`, which came from those FLOORED
	// magnitudes -- so a part authored with a zero-scale axis really does squash
	// the field by 1e-9 per unit, and reading `pt.scale` raw here instead made
	// such a part contribute NOTHING (`minScale > 0` was false and it was
	// skipped) when it is in fact the most extreme shrink in the field.
	Scalar globalShrink = Scalar(1);
	for( std::size_t i = 0; i < m_parts.size(); i++ ) {
		Scalar mn, mx;
		FlooredScaleExtremes_( m_parts[i].scale, mn, mx );
		globalShrink = std::min( globalShrink, mn / mx );
	}
	if( !( globalShrink > Scalar(0) ) ) {
		globalShrink = Scalar(1);			// defensive: the flooring cannot produce this
	}

	// Incidence cosine, in RANGE-per-perpendicular-distance (see the note).
	// A normal that is not usable leaves the answer perpendicular, undivided.
	const Scalar nMag = std::sqrt( localNormal.x*localNormal.x +
		localNormal.y*localNormal.y + localNormal.z*localNormal.z );
	const Scalar cosI = ( nMag > NEARZERO )
		? std::max( std::fabs( localDir.x * localNormal.x +
		                       localDir.y * localNormal.y +
		                       localDir.z * localNormal.z ) / nMag, Scalar(0.05) )
		: Scalar(1);

	// THE ANSWER IS CAPPED AT HALF THE FIELD'S OWN BOUNDING-BOX DIAGONAL
	// (adversarial review of 384e3752, P1-2).  Everything above is a RATIO
	// argument -- the band divided by a Lipschitz shrink and an incidence
	// cosine -- and a ratio has no upper bound: a part authored with a zero
	// scale axis makes `globalShrink` the 1e-9 poison floor and the answer
	// comes out 5.66e4 on a field 2.83 units across.  A self-hit floor LARGER
	// THAN THE OBJECT is never useful -- the standoff it sanctions leaves the
	// field entirely -- and it is actively harmful, because `CSGObject::
	// SelfHitRootFloor` uses `2 * floorChild` as an ownership-ray REACH and
	// would charge this field's floor on geometry tens of units away.
	//
	// 0.5 * diagonal is far above every non-degenerate configuration: the
	// uncapped answer is `2 * m_epsFrac * diag / shrink / cosI` (m_eps is
	// m_epsFrac of the diagonal except on a sub-1e-6/epsFrac field, where the
	// 1e-6 floor takes over and the padded diagonal is at least 3.4e-3), so at
	// the 5e-5 scene default with uniform parts it is 1e-4 of the diagonal, and
	// even at the 1/20 grazing clamp and the 0.02 shrink of BoxGeometryTest's
	// thinnest row it is 0.1 of it.  Reaching 0.5 would take an `epsFrac` above
	// 0.25 at normal incidence (0.0125 at the grazing clamp) -- i.e. a surface
	// epsilon of a quarter of the shape.  When it does bite, it UNDER-states,
	// which for this contract is the graceful direction: the probe that trusted
	// it is marched past its face, misses, and falls back to the entry payload;
	// only OVER-statement adopts a wrong face.
	//
	// An unbuilt / non-finite diagonal disables the cap rather than poisoning
	// the answer (`ComputeBounds` always sets one, so this is defensive).
	const bool haveDiag = RISE::IsFiniteDouble( static_cast<double>( m_diagonal ) ) && m_diagonal > Scalar(0);
	const Scalar maxFloor = haveDiag ? Scalar(0.5) * m_diagonal : RISE_INFINITY;

	// Heightfield mode has no parts: nothing to own, keep the bare band.
	if( m_parts.empty() ) {
		return std::min( Scalar(2) * m_eps / globalShrink / cosI, maxFloor );
	}

	// Twice the widest floor this call can now return -- which is the capped
	// one, so the cap enters here too and the band stays exactly the bound its
	// derivation claims (never narrower than 2x any answer below: `ownerShrink`
	// is >= `globalShrink`, so the owner-side answer is <= this one's argument).
	const Scalar band = Scalar(2) * std::min( Scalar(2) * m_eps / globalShrink / cosI, maxFloor );

	Scalar ownerShrink = Scalar(1);
	bool anyOwner = false;
	for( std::size_t i = 0; i < m_parts.size(); i++ ) {
		const Part& pt = m_parts[i];
		const Scalar reach = ( pt.op == eOpUnion ) ? band : ( band + std::fabs( pt.k ) );
		if( !( std::fabs( partEval( pt, localOrigin ) ) <= reach ) ) {
			continue;						// provably farther than the probe can reach
		}
		Scalar mn, mx;
		FlooredScaleExtremes_( pt.scale, mn, mx );
		ownerShrink = std::min( ownerShrink, mn / mx );
		anyOwner = true;
	}

	// No qualifying part -- a blend seam, or a degenerate part list.  Fall back
	// to the global minimum, i.e. exactly the previous behaviour.
	const Scalar shrink = anyOwner ? ownerShrink : globalShrink;
	return std::min( Scalar(2) * m_eps / shrink / cosI, maxFloor );
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
// ISurfaceSignalProvider -- geometry-derived shading signals
// (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §6.2 for the channel;
//  docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md for the estimator).
//
// All three are pure const functions of the field: no rays, no scene
// access, no locks, no mutable state.  Every render thread calls them
// concurrently on one shared geometry.
//
// All three are LAZY by construction -- they run only when an expression
// actually calls `occlusion()` / `thickness()` / `convexity()`, which is
// why none needs the up-front consumption gate `curv` requires.
//
// THE TWO ESTIMATORS, AND WHY THEY ARE NOT THE SAME ONE.
//
// `occlusion(r)` is DIRECTIONAL VISIBILITY over the outward hemisphere --
// the classic ambient-occlusion question, and bit-for-bit the same
// question the mesh family's bake asks with rays: of N directions, how
// many escape a distance R without entering the solid?  A plane and every
// convex feature read EXACTLY 1 (every outward ray escapes); a wedge of
// empty opening alpha reads alpha/pi; a slot, a fold or a pocket goes
// dark.  It marches the field, one sphere trace per direction.
//
// `convexity(r)` is the BALL-VOLUME EXCESS -- the fraction A of the ball
// of radius R about the hit that lies outside the solid, which is exactly
// 1/2 on a plane, more on a convex feature and less in a cavity, read as
// `clamp(2A - 1, 0, 1)`.  It needs no marching at all: 80 point samples,
// no rays, no tangent frame.
//
// WHY NOT ONE MEASURE FOR BOTH, which an earlier draft of this work did
// try.  `clamp(2A, 0, 1)` off the same ball is a beautiful occlusion on
// paper -- one measurement, two clamps -- and it is WRONG on the feature
// occlusion exists for.  VOLUME IS NOT VISIBILITY: on the wall of the
// 2.6 mm end check in plank_closeup, queried at 18.6 mm, the ball reaches
// up out of the slot into open air and the slot itself removes only ~5 %
// of a ball that much bigger than it, so A came out at ~0.55 and the crack
// read UNOCCLUDED.  Rendered, its dirt simply vanished.  Half-ball
// variants (empty-in-front over solid-behind) move the number to ~0.81 and
// do not fix it, for the same reason: the open sky above a shallow crack
// is genuinely most of the volume in front of its wall, and only a
// DIRECTIONAL test knows that none of that sky is actually reachable.
//
// The converse also holds, which is why convexity keeps the ball: solid
// ANGLE cannot see the convexity of a smooth body at all.  From a point on
// a sphere of ANY radius the solid subtends exactly a hemisphere, so a
// directional convexity reads 0 on every sphere; the ball's volume reads
// 3R/(8*rho), which is the "this bead is proud of its surroundings at
// scale R" an edge-wear mask wants.  On WEDGES -- edges, creases, corners,
// the features both are mostly used on -- the two agree exactly.
//
// WHY NOT THE EVANS NORMAL-LINE ESTIMATOR occlusion REPLACED.  That one
// read the SHORTFALL of |Map| along the normal,
// `1 - avg((h - Map(p+h*n))/h)`, which is correct only where Map is the
// EXACT Euclidean distance.  RISE's composed field is a conservative LOWER
// bound (hard `max` for intersect/subtract, the smin/smax blends,
// partEval's minScale fold), and a lower bound is one-sided: it can only
// ever darken.  At a convex edge whose two faces' normals are 2*gamma
// apart the hard `max` gives Map(p + h*n) = h*cos(gamma) exactly, so it
// returned `ao = cos(gamma)` -- 0.7071 at a plain 90-degree arris, for ANY
// query radius and ANY fillet radius, on surface with no cavity at all.
// Worse, a CONCAVE 90-degree valley returned the same 0.7071, so the two
// were indistinguishable.  Both estimators here read only the SIGN of the
// field, which IS exact for the surface the sphere trace actually renders
// (the zero set of the composed field is the blended surface), and both
// take their planar reference from the geometry of their own sample set
// rather than from the field's magnitude.
//////////////////////////////////////////////////////////////////////

namespace
{
	//! Half-count of the ball point set: `kBallPairs` lattice points, each
	//! used with BOTH signs, so 2*kBallPairs = 32 field evaluations per
	//! query.
	//!
	//! WAS 40 PAIRS, AND CAME DOWN ONLY BECAUSE THE SET IS NOW ROTATED PER
	//! HIT (kSignalRotations below).  A FIXED point set has to carry the
	//! closed forms POINTWISE, at whatever orientation the feature happens
	//! to present, and that is what 40 was buying: at 32 fixed pairs the
	//! sphere identity `convexity = 3R/(8*rho)` read 0.159 against 0.1875
	//! (tolerance 0.02) purely on orientation luck, and at 20 the
	//! 90-degree arris read 0.416 against 0.5.  Rotating the set removes
	//! the orientation as a variable instead of out-voting it, and the
	//! ROTATION-AVERAGED reading is flat in the count: 0.185 / 0.178 /
	//! 0.183 / 0.181 at 8 / 16 / 24 / 40 pairs against the same 0.1875
	//! (modelled over 64 rotations; the residual -0.004 is the smoothing
	//! band, not the count).  16 is taken with that margin, at 40 % of the
	//! field evaluations.
	const int kBallPairs = 16;

	//! Directions the OCCLUSION march spends, and the one knob that decides
	//! what this feature costs: each is a sphere trace, where a convexity
	//! sample is a single field evaluation.
	//!
	//! WAS 24, AND CAME DOWN FOR THE SAME REASON.  A fixed set answers
	//! `escaped/N`, so its quantum is 1/N and a closed form has to land on
	//! a multiple of it: at 24 fixed directions the 90-degree wedge hits
	//! `sin^2(alpha/2) = 0.5` exactly, at 12 fixed directions the SAME
	//! fixture reads 0.583 -- and a sweep of the frame azimuth shows the
	//! fixed set landing anywhere in [0.417, 0.583], i.e. the value is
	//! decided by where the branchless ONB happens to put `u`.  Rotating
	//! the set about the normal per hit makes the azimuth a sampled
	//! variable rather than an accident: the azimuth-AVERAGED reading is
	//! 0.5000 at N = 8, 12, 16 and 24 alike, and 0.7486 / 0.7492 / 0.7495 /
	//! 0.7497 against the 120-degree closed form 0.7500.  The elevation
	//! stratification is NOT rotated (see BallLattice) -- it is a midpoint
	//! rule in cos^2(theta) whose own error is the 0.001 in that row.
	//! 12 is taken with that margin, at half the sphere traces.
	const int kOcclusionDirs = 12;

	//! How many pre-built ROTATIONS of the two point sets exist, one picked
	//! per hit by a hash of the hit position.
	//!
	//! WHY A TABLE AND NOT A LIVE ANGLE.  These estimators are called far
	//! more often than "once per visible pixel": on plank_closeup, 210.8
	//! MILLION times for 14.7 million camera samples, because the field
	//! feeds three material slots plus a relief modifier and every one of
	//! them re-runs the whole expression.  At that call rate a single
	//! `sin`/`cos` pair per query is not free (~1 s of a ~55 s frame) and
	//! per-DIRECTION trigonometry would cost more than the marching it is
	//! meant to make cheaper.  So the rotations are baked once, at process
	//! start, and a query pays one hash and one indexed read.
	//!
	//! 32 is enough to stop the choice reading as a repeat: the value a
	//! hit gets is one of 32, the sub-pixel jitter puts every camera sample
	//! at a different position, and 48 spp averages ~32 distinct rotations
	//! per pixel.  Both point sets keep their exact identities under EVERY
	//! rotation in the table -- see the two notes in BallLattice.
	const int kSignalRotations = 32;
	//! SignalRotationIndex masks rather than divides, so this must stay a
	//! power of two.
	static_assert( ( kSignalRotations & ( kSignalRotations - 1 ) ) == 0,
		"kSignalRotations must be a power of two -- SignalRotationIndex masks with (kSignalRotations - 1)" );

	//! Width of the smoothed indicator's transition band, as a fraction of
	//! the query radius.  A HARD `Map > 0` test makes A jump by 1/(2*pairs)
	//! every time a sample crosses the surface -- deterministic in
	//! position, so it appears as spatial BANDING rather than as noise,
	//! which is worse.  Smoothing spreads each crossing over this band.
	const Scalar kBallBandFraction = Scalar( 0.1875 );	// 3/16

	//! van der Corput radical inverse, base 2 -- the lattice's azimuth.
	inline Scalar BallRadicalInverse2( unsigned int i )
	{
		i = ( i << 16 ) | ( i >> 16 );
		i = ( ( i & 0x55555555u ) << 1 ) | ( ( i & 0xAAAAAAAAu ) >> 1 );
		i = ( ( i & 0x33333333u ) << 2 ) | ( ( i & 0xCCCCCCCCu ) >> 2 );
		i = ( ( i & 0x0F0F0F0Fu ) << 4 ) | ( ( i & 0xF0F0F0F0u ) >> 4 );
		i = ( ( i & 0x00FF00FFu ) << 8 ) | ( ( i & 0xFF00FF00u ) >> 8 );
		return Scalar( i ) * Scalar( 2.3283064365386963e-10 );	// / 2^32
	}

	//! Radical inverse, base 3 -- the lattice's radius.  A DIFFERENT base
	//! from the azimuth's on purpose: sharing one would correlate radius
	//! with azimuth and collapse the point set onto a spiral shell.
	inline Scalar BallRadicalInverse3( unsigned int i )
	{
		Scalar f = Scalar(1) / Scalar(3), r = Scalar(0);
		while( i ) { r += f * Scalar( i % 3u ); i /= 3u; f /= Scalar(3); }
		return r;
	}

	//! Radical inverse in an arbitrary small base -- the rotation table's
	//! third coordinate (base 5), so the (base 2, base 3, base 5) triple
	//! that drives a rotation is a Halton point and the 32 rotations are
	//! spread rather than clumped.
	inline Scalar BallRadicalInverseB( unsigned int i, const unsigned int b )
	{
		Scalar f = Scalar(1) / Scalar(b), r = Scalar(0);
		while( i ) { r += f * Scalar( i % b ); i /= b; f /= Scalar(b); }
		return r;
	}

	//! WHICH ROTATION THIS HIT GETS: a hash of the hit position, in the
	//! provider's OWN OBJECT SPACE.
	//!
	//! Object space, not world, for the reason the whole radius convention
	//! is object-relative: two instances of one geometry at different world
	//! scales must read the same signal at the same place on the surface,
	//! and they see the same `ptObject`.  (SurfaceSignalsTest case (i) is
	//! the guard.)
	//!
	//! POSITION, not a sample index, because a position is all the estimator
	//! is given -- and it is the right key anyway.  Every camera sample in a
	//! pixel lands at a different sub-pixel position, so a pixel averages
	//! many rotations; a shading point queried repeatedly WITHIN one hit
	//! (three material slots, plus the relief modifier's four-tap stencil,
	//! which holds `signals` fixed across the stencil by construction) is
	//! one position and gets ONE rotation, so the answer stays consistent
	//! where consistency is what matters.
	//!
	//! FNV-1a over the raw bit patterns, then a 64-bit avalanche, so
	//! neighbouring positions -- which differ only in the low mantissa
	//! bits -- land on unrelated rotations.
	inline int SignalRotationIndex( const Point3& p )
	{
		const double c[3] = { (double)p.x, (double)p.y, (double)p.z };
		unsigned long long h = 14695981039346656037ULL;
		for( int i = 0; i < 3; ++i ) {
			unsigned long long b = 0;
			std::memcpy( &b, &c[i], sizeof(b) );
			h ^= b;
			h *= 1099511628211ULL;
		}
		h ^= h >> 33;  h *= 0xff51afd7ed558ccdULL;
		h ^= h >> 33;  h *= 0xc4ceb9fe1a85ec53ULL;
		h ^= h >> 33;
		return (int)( h & (unsigned long long)( kSignalRotations - 1 ) );
	}

	//! The point set, in the UNIT ball, built once per process.
	//!
	//! CENTRAL SYMMETRY IS THE LOAD-BEARING PROPERTY, and it is why these
	//! are object-space offsets with no tangent frame anywhere in sight.
	//! Each point is used with both signs, and central symmetry through the
	//! query point maps the half-space {d < 0} onto {d > 0} for ANY plane
	//! through that point -- so a planar surface reads A = 1/2 EXACTLY, at
	//! every orientation and every sample count.  (An earlier draft aligned
	//! the set to the hit normal and mirrored about the tangent plane,
	//! which is also exact on a plane but needs an orthonormal basis, and
	//! every branchless ONB has a discontinuity somewhere on the sphere:
	//! measured frame-rotation spread of A at a 90-degree wedge was 0.028,
	//! i.e. a 5.5 % step in the mask wherever the basis branch flipped.)
	//!
	//! Uniform BY VOLUME (`rho = u^(1/3)`), which is what makes A a volume
	//! fraction rather than a weighted one.  Deterministic: a fixed
	//! Hammersley-style triple, no RNG, no per-hit state, identical on
	//! every thread and every run.
	struct BallLattice
	{
		//! Volume-uniform ball offsets (convexity), in `kSignalRotations`
		//! independently ROTATED copies.
		//!
		//! THE ROTATIONS ARE FULL 3-D AND UNIFORM, not a spin about one
		//! axis, because this set has no frame to spin about -- that is the
		//! point of it (see the central-symmetry note below).  A rotation
		//! about a fixed WORLD axis would leave any feature whose edge runs
		//! parallel to that axis sampled identically by all 32 entries,
		//! which is exactly the orientation accident the table exists to
		//! remove.  Shoemake's uniform-quaternion map of a Halton triple
		//! gives the uniform measure on SO(3) with 32 well-spread members.
		//!
		//! CENTRAL SYMMETRY SURVIVES EVERY ROTATION, which is what lets the
		//! count come down at all: each point is still used with BOTH
		//! signs, and `-(Q y) == Q(-y)`, so the rotated set is centrally
		//! symmetric too and a planar surface still reads A = 1/2 EXACTLY,
		//! at every orientation, every rotation and every count.
		Point3  pt[kSignalRotations][kBallPairs];
		//! COSINE-WEIGHTED hemisphere directions about +Z, rotated into the
		//! hit's frame per query (occlusion).  Cosine rather than uniform,
		//! and for two reasons that agree: it is what the mesh family's
		//! occlusion bake samples, so the two answer the same integral; and
		//! near-tangent directions -- the ones that cost a sphere trace its
		//! whole step budget -- are exactly the ones a cosine weight makes
		//! rare.
		Vector3 cosDir[kOcclusionDirs];
		//! The per-hit AZIMUTHAL rotation, as a cos/sin pair, applied to the
		//! hit's tangent frame rather than to the directions -- so a query
		//! pays two extra multiply-adds ONCE, not per direction, and
		//! `cosDir` stays a single canonical set.
		//!
		//! ONLY THE AZIMUTH IS ROTATED, and deliberately.  The elevations
		//! stay stratified at the midpoints `cos(theta)_i = sqrt((i+1/2)/N)`
		//! because that is a midpoint rule in cos^2(theta) whose error is
		//! already below a thousandth on the wedge family (0.7492 vs 0.7500
		//! at N = 12), while jittering them within their strata would let a
		//! direction land arbitrarily close to the tangent plane -- and a
		//! near-tangent ray is precisely the one that burns the entire
		//! 24-step budget.  Rotating what is free and stratifying what is
		//! expensive is the whole trade.
		//!
		//! It also retires the branchless-ONB discontinuity as a source of
		//! spread: the set is uniformly spun about the normal, so which
		//! branch `CreateFromW` took stops being observable.
		Scalar  azCos[kSignalRotations];
		Scalar  azSin[kSignalRotations];
		BallLattice()
		{
			Point3 base[kBallPairs];
			for( int i = 0; i < kBallPairs; ++i ) {
				// cos(theta) stratified over the UPPER hemisphere; the
				// lower half comes from the sign flip at use.
				const Scalar cz = ( Scalar(i) + Scalar(0.5) ) / Scalar(kBallPairs);
				const Scalar sz = std::sqrt( ( cz < Scalar(1) ) ? ( Scalar(1) - cz*cz ) : Scalar(0) );
				const Scalar phi = Scalar(TWO_PI) * BallRadicalInverse2( (unsigned int)( i + 1 ) );
				Scalar u = BallRadicalInverse3( (unsigned int)( i + 1 ) );
				// i+1 is never 0, so RadicalInverse3 is never 0 for the
				// counts we use; guard anyway so a future kBallPairs cannot
				// silently place a sample at the query point itself.
				if( !( u > Scalar(0) ) ) u = Scalar(0.5) / Scalar(kBallPairs);
				const Scalar rho = std::pow( u, Scalar(1)/Scalar(3) );
				base[i] = Point3( rho*sz*std::cos(phi), rho*sz*std::sin(phi), rho*cz );
			}
			for( int v = 0; v < kSignalRotations; ++v ) {
				// Shoemake 1992: a uniform rotation from three uniforms,
				// via the unit quaternion (x,y,z,w).
				const Scalar u1 = BallRadicalInverseB( (unsigned int)( v + 1 ), 2u );
				const Scalar u2 = BallRadicalInverseB( (unsigned int)( v + 1 ), 3u );
				const Scalar u3 = BallRadicalInverseB( (unsigned int)( v + 1 ), 5u );
				const Scalar s1 = std::sqrt( Scalar(1) - u1 ), s2 = std::sqrt( u1 );
				const Scalar qx = s1 * std::sin( Scalar(TWO_PI) * u2 );
				const Scalar qy = s1 * std::cos( Scalar(TWO_PI) * u2 );
				const Scalar qz = s2 * std::sin( Scalar(TWO_PI) * u3 );
				const Scalar qw = s2 * std::cos( Scalar(TWO_PI) * u3 );
				const Scalar m00 = 1-2*(qy*qy+qz*qz), m01 = 2*(qx*qy-qz*qw), m02 = 2*(qx*qz+qy*qw);
				const Scalar m10 = 2*(qx*qy+qz*qw), m11 = 1-2*(qx*qx+qz*qz), m12 = 2*(qy*qz-qx*qw);
				const Scalar m20 = 2*(qx*qz-qy*qw), m21 = 2*(qy*qz+qx*qw), m22 = 1-2*(qx*qx+qy*qy);
				for( int i = 0; i < kBallPairs; ++i ) {
					const Point3& b = base[i];
					pt[v][i] = Point3( m00*b.x + m01*b.y + m02*b.z,
					                   m10*b.x + m11*b.y + m12*b.z,
					                   m20*b.x + m21*b.y + m22*b.z );
				}
				// The occlusion set's azimuth, from the SAME low-discrepancy
				// sequence the quaternion's first coordinate uses, so the 32
				// spins are spread over the circle rather than clumped.
				const Scalar az = Scalar(TWO_PI) * u1;
				azCos[v] = std::cos( az );
				azSin[v] = std::sin( az );
			}
			// Cosine-weighted about +Z: cos(theta) = sqrt(u1) with u1
			// stratified, azimuth from the same radical inverse.  Built ONCE,
			// in a canonical frame, so a query costs one rotation per
			// direction and no trigonometry at all.  Its own loop because it
			// has its own count -- see kOcclusionDirs.
			for( int i = 0; i < kOcclusionDirs; ++i ) {
				const Scalar ct  = std::sqrt( ( Scalar(i) + Scalar(0.5) ) / Scalar(kOcclusionDirs) );
				const Scalar st  = std::sqrt( ( ct < Scalar(1) ) ? ( Scalar(1) - ct*ct ) : Scalar(0) );
				const Scalar phi = Scalar(TWO_PI) * BallRadicalInverse2( (unsigned int)( i + 1 ) );
				cosDir[i] = Vector3( st*std::cos(phi), st*std::sin(phi), ct );
			}
		}
	};
	//! Marching constants for the DIRECTIONAL occlusion estimator.
	//!
	//! `kMarchStart` lifts the first sample off the surface: a sphere trace
	//! begun exactly ON the zero set steps by 0 forever.  As a fraction of
	//! the query radius, so it is scale-relative like everything else here.
	//!
	//! `kMarchMinStep` keeps a near-tangent ray from stalling in the sliver
	//! of tiny field values just above a surface.  It is the one place this
	//! estimator can overstep a thin occluder; the step cap below bounds the
	//! damage, and the direction it errs in is toward ESCAPED, i.e. toward
	//! the neutral 1.
	//!
	//! `kMarchMaxSteps` bounds the per-direction cost.  Running out is
	//! reported as ESCAPED, and that is the correct answer far more often
	//! than not: the rays that exhaust the budget are the near-tangent ones
	//! over open surface, which genuinely do escape.  In a cavity it
	//! under-darkens slightly, again toward neutral.
	const Scalar kMarchStart    = Scalar( 1.0 / 64.0 );
	const Scalar kMarchMinStep  = Scalar( 1.0 / 64.0 );
	const int    kMarchMaxSteps = 24;

	//! Lift of the march ORIGIN along the hit normal, as a fraction of the
	//! query radius.  A ray begun exactly on the zero set in a direction
	//! near the tangent plane sits at height ~0 and reads as INSIDE, so a
	//! flat surface would report a couple of its 40 directions blocked and
	//! occlusion would come out at 0.95 instead of 1 -- measured, before
	//! this existed.  The same lift is what the mesh family's bake applies
	//! for the same reason (MeshSignalBake::kOriginEpsilonFraction), and it
	//! errs the same way: toward ESCAPED, i.e. toward the neutral.
	const Scalar kMarchLift = Scalar( 1.0 / 64.0 );

	const BallLattice& TheBallLattice()
	{
		// Function-local static: one instance across the process,
		// thread-safe initialization guaranteed since C++11, and read-only
		// from then on.
		static const BallLattice lattice;
		return lattice;
	}

	//! Odd-symmetric smoothed Heaviside: Hs(u) + Hs(-u) == 1 EXACTLY, which
	//! is what lets the smoothing coexist with the central-symmetry
	//! guarantee above (for a plane the paired samples sum to exactly 1, so
	//! A stays exactly 1/2 however wide the band).
	inline Scalar BallSmoothStep( const Scalar u, const Scalar w )
	{
		Scalar t = u / w;
		if( t < Scalar(-1) ) t = Scalar(-1); else if( t > Scalar(1) ) t = Scalar(1);
		return Scalar(0.5) + Scalar(0.5) * ( Scalar(1.5)*t - Scalar(0.5)*t*t*t );
	}
}

bool SDFGeometry::PrepareSignalQuery( const SurfaceSignalInfo& hit,
	const Scalar radiusFraction, Scalar& outR, Scalar& outD0 ) const
{
	// A LIVE field answers any radius, constant or computed -- see the
	// header's note on why this provider ignores the constant-radius flag
	// the baked mesh family requires.

	// REFUSE rather than fabricate.  The caller (SurfaceSignalInfo) already
	// screens a non-finite / non-positive radius; this is the field-side
	// half: a degenerate bbox has no characteristic length to take a
	// fraction OF, so there is no honest answer to give.
	if( !( m_diagonal > Scalar(0) ) || !( radiusFraction > Scalar(0) ) ) {
		return false;
	}

	// Heightfield mode divides Map() by a single GLOBAL Lipschitz bound
	// (m_hfLip, see ComputeHeightfieldLipschitz), sized to the field's
	// STEEPEST slope, so its MAGNITUDE is wrong everywhere the local slope
	// is below that maximum.  The SIGN is correct there, which is all the
	// convexity estimator reads -- but the occlusion march steps by the
	// field VALUE, so on a heightfield it would crawl by a factor of
	// m_hfLip and exhaust its step budget (reporting everything escaped)
	// wherever the local slope is gentle.  Neither signal is worth
	// publishing under a systematically wrong step length, so both refuse,
	// as thickness already does for the same underlying reason.
	if( m_isHeightfield ) {
		return false;
	}

	// Query radius in this geometry's own object-space units.  Every sample
	// offset and march length is a fraction OF it, so the whole estimator is
	// scale-relative and the result is transform-invariant.
	const Scalar R = radiusFraction * m_diagonal;
	if( !RISE::IsFiniteDouble( static_cast<double>( R ) ) || !( R > Scalar(0) ) ) {
		return false;
	}

	// The reported hit sits INSIDE the +-m_eps sphere-trace band rather than
	// exactly on the zero set, so Map(p) is a small residual, not 0.
	// Subtracting it re-centres both estimators on the actual surface: for a
	// plane, Map(p + y) - Map(p) == y . n_hat exactly.
	outR  = R;
	outD0 = Map( hit.ptObject );
	return true;
}

//////////////////////////////////////////////////////////////////////
// IGeometry::DistanceToSurface -- the cross-object proximity query's
// SDF family (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2).
//
// See the header for why this cannot report `Map` and what it reports
// instead.  Three budget constants, all local because nothing else in the
// file has a use for them:
//
//   kDescentIters   how many Newton-ish descent steps before giving up on
//                   getting close and switching to the probe.  SIX: each
//                   iteration is a GradientNormal (six Map calls) plus one
//                   more for the step, so 7 x O(parts) apiece, and the
//                   design's cost budget is "of the same order as one
//                   occlusion() call" -- which on plank_closeup is 181.7
//                   field evaluations.
//   kProbeSteps     how many doublings of the probe step.  The step starts
//                   at max(m_eps, Map) and doubles, so N steps cover
//                   (2^N - 1) x start; at m_eps ~ 1e-5 of the diagonal,
//                   forty doublings is astronomically past any maxDist and
//                   the real terminator is the maxDist test inside the
//                   loop.  It is a runaway guard, not a tuning knob.
//   kBackoff        the descent stops early once |Map| falls below this
//                   multiple of m_eps -- there is no point polishing a
//                   point the probe is about to walk past anyway.
//////////////////////////////////////////////////////////////////////

bool SDFGeometry::DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const
{
	// HEIGHTFIELD MODE REFUSES, for the same reason PrepareSignalQuery
	// makes the other signals refuse: its field is divided by a single
	// GLOBAL Lipschitz bound sized to the steepest slope, so its magnitude
	// is systematically wrong wherever the local slope is gentler -- and
	// this procedure's probe steps BY that magnitude.
	if( m_isHeightfield ) {
		return false;
	}
	// An empty part list has no surface at all.  `EvaluateParts` returns
	// its +1e30 "nothing here" sentinel, which would survive every test
	// below and produce a meaningless answer.
	if( m_parts.empty() ) {
		return false;
	}
	if( !( maxDistObject > Scalar( 0 ) ) || !RISE::IsFiniteDouble( (double)maxDistObject ) ) {
		return false;
	}
	if( !( m_eps > Scalar( 0 ) ) ) {
		return false;
	}

	const Scalar f0 = Map( ptObject );
	if( !RISE::IsFiniteDouble( (double)f0 ) ) {
		return false;
	}

	// ALREADY INSIDE (or on the surface): distance 0.  Unsigned, as the
	// query's contract requires -- interpenetration is contact.  This is
	// also exact, not a bound, and it is the ONE branch here that is.
	if( f0 <= Scalar( 0 ) ) {
		outDist = Scalar( 0 );
		return true;
	}

	// STEP 1 -- THE LOWER-BOUND EARLY-OUT, and the one place `Map`'s
	// under-reading is an ASSET.  `Map <= true distance` everywhere, so
	// `Map(p) > maxDist` PROVES the true distance is past the radius and
	// this candidate cannot contribute.  Skipping here can never lose a
	// neighbour that was actually within range.
	if( f0 > maxDistObject ) {
		return false;
	}

	const int    kDescentIters = 6;
	const int    kProbeSteps   = 40;
	const Scalar kBackoff      = Scalar( 2 );

	// STEP 2 -- DESCEND.  `p <- p - Map(p) * grad_hat(p)`.  A step of
	// |Map| along the negated unit gradient cannot overshoot the zero set
	// of a 1-Lipschitz field (the field can fall by at most |step| over
	// that distance), so the iteration approaches the surface monotonically
	// from outside and never crosses it -- which is what keeps `q` below an
	// honest bracket rather than a guess.
	Point3 p = ptObject;
	Scalar f = f0;
	Vector3 g( 0, 1, 0 );
	bool haveGradient = false;
	for( int it = 0; it < kDescentIters; ++it ) {
		if( f <= kBackoff * m_eps ) {
			break;
		}
		g = GradientNormal( p );
		haveGradient = true;
		// GradientNormal fabricates (0,1,0) where the gradient collapses
		// below 1e-12 -- a flat blend seam.  Descent then walks an
		// arbitrary axis and this candidate will most likely REFUSE below,
		// which is a feature failure (an unpainted seam), never a wrong
		// answer.  Documented rather than special-cased: there is no better
		// direction to invent.
		const Point3 next( p.x - f * g.x, p.y - f * g.y, p.z - f * g.z );
		const Scalar fn = Map( next );
		if( !RISE::IsFiniteDouble( (double)fn ) ) {
			break;
		}
		// A step that did not reduce the field means the local gradient is
		// lying to us (a blend seam, a scaled part); stop rather than
		// wander.  The probe below still gets its chance from here.
		if( fn >= f ) {
			break;
		}
		p = next;
		f = fn;
	}

	// STEP 3 -- PROBE for the sign change, along the last descent
	// direction, with a DOUBLING step.  The first point with `Map <= 0` is
	// on or inside the solid, so the surface lies on the segment from the
	// ORIGINAL query point to it -- and therefore `|ptObject - q|` is at
	// least the true distance.  That is the upper bound this whole
	// procedure exists to produce.
	//
	// THE DESCENT LOOP MAY NEVER HAVE RUN -- a query point already inside
	// the backoff band exits it on the first test -- and `g` would then
	// still hold its (0,1,0) INITIALISER, which is an arbitrary axis, not a
	// direction toward the surface.  Probing along it would wander and the
	// candidate would refuse for no reason, so take the real gradient here.
	if( !haveGradient ) {
		g = GradientNormal( p );
	}
	Scalar step = std::max( m_eps, ( f > Scalar( 0 ) ) ? f : m_eps );
	Scalar walked = Scalar( 0 );
	Point3 q = p;
	bool crossed = ( f <= Scalar( 0 ) );

	for( int s = 0; !crossed && s < kProbeSteps; ++s ) {
		walked += step;
		// A CHEAP BUDGET TEST, not the authoritative one.  `f0 + walked` is
		// a proxy for how far `q` has got from the ORIGINAL point; it is
		// neither reliably above nor reliably below the true separation
		// (the descent's own path can exceed `f0`), and it does not need to
		// be.  Overshooting it only ends the walk early -- a refusal, which
		// under-paints -- and undershooting it only costs a few more Map
		// calls, because the FINAL `d > maxDistObject` test below is what
		// actually decides whether the answer is in range.
		if( f0 + walked > maxDistObject ) {
			break;
		}
		q = Point3( p.x - walked * g.x, p.y - walked * g.y, p.z - walked * g.z );
		const Scalar fq = Map( q );
		if( !RISE::IsFiniteDouble( (double)fq ) ) {
			break;
		}
		if( fq <= Scalar( 0 ) ) {
			crossed = true;
			break;
		}
		step *= Scalar( 2 );
	}

	// STEP 4 -- NO CROSSING FOUND: REFUSE.  Never the unconverged point's
	// distance, which would be smaller than the truth and so over-read
	// contact.  This candidate contributes nothing and the caller treats it
	// as far.
	if( !crossed ) {
		return false;
	}

	const Scalar d = Vector3Ops::Magnitude( Vector3Ops::mkVector3( q, ptObject ) );
	if( !RISE::IsFiniteDouble( (double)d ) || d > maxDistObject ) {
		return false;
	}
	outDist = d;
	return true;
}

bool SDFGeometry::ComputeOcclusion( const SurfaceSignalInfo& hit,
	const Scalar radiusFraction, const bool /*bRadiusIsConstant*/, Scalar& outValue ) const
{
	Scalar R = 0, d0 = 0;
	if( !PrepareSignalQuery( hit, radiusFraction, R, d0 ) ) {
		return false;
	}

	// Only the OUTWARD hemisphere is traced: the inward half is inside the
	// solid by definition, so it would answer "blocked" at its first sample
	// and contribute nothing but cost.  That asymmetry is also what makes
	// the planar reference free -- over a plane EVERY outward direction
	// escapes, so the answer is exactly 1 with no normalisation, at any
	// orientation and any direction count.
	const Vector3& n = hit.nObject;
	const Scalar nLen2 = n.x*n.x + n.y*n.y + n.z*n.z;
	if( !( nLen2 > NEARZERO ) ) {
		return false;
	}
	const Scalar invLen = Scalar(1) / std::sqrt( nLen2 );
	const Vector3 nHat( n.x*invLen, n.y*invLen, n.z*invLen );
	OrthonormalBasis3D onb;
	onb.CreateFromW( nHat );

	// A CSG_SUBTRACTION that credited this surface to the SUBTRAHEND has
	// inverted the sense of "solid": the empty region is this field's
	// INTERIOR.  `nObject` is flipped at the same four CSG sites, so the
	// outward side is already right; only the sense needs this.
	const Scalar sense = hit.bComplementedField ? Scalar(-1) : Scalar(1);

	// Lift the march origin off the zero set (see kMarchLift).
	const Point3 origin(
		hit.ptObject.x + nHat.x * R * kMarchLift,
		hit.ptObject.y + nHat.y * R * kMarchLift,
		hit.ptObject.z + nHat.z * R * kMarchLift );

	const BallLattice& L = TheBallLattice();

	// SPIN THE FRAME, NOT THE DIRECTIONS.  `cosDir` is one canonical set;
	// rotating the hit's tangent axes by this hit's azimuth (kSignalRotations)
	// rotates all of them at once, for two multiply-adds per query rather
	// than per direction, and leaves `w()` -- the normal -- untouched, so
	// the set stays a cosine-weighted OUTWARD hemisphere and the planar
	// identity below is unaffected.
	const int    rot = SignalRotationIndex( hit.ptObject );
	const Scalar ca = L.azCos[rot], sa = L.azSin[rot];
	const Vector3 su(  ca*onb.u().x + sa*onb.v().x,  ca*onb.u().y + sa*onb.v().y,  ca*onb.u().z + sa*onb.v().z );
	const Vector3 sv( -sa*onb.u().x + ca*onb.v().x, -sa*onb.u().y + ca*onb.v().y, -sa*onb.u().z + ca*onb.v().z );

	// THE MARCH IS RUN IN LOCKSTEP ACROSS THE DIRECTIONS, one step of every
	// still-live ray before the next step of any of them, rather than one
	// ray to completion before the next is begun.  The two orders visit
	// EXACTLY the same points and apply exactly the same per-ray step rule
	// and exit tests -- Map is a pure function, so the answer is bit
	// identical and the field-evaluation COUNT is unchanged.  What changes
	// is the dependency structure the hardware sees.  Ray-at-a-time is one
	// long serial chain: step k+1's sample point cannot be formed until
	// step k's Map has returned, so the core stalls on Map's latency
	// (square roots, the CSG min/max fold) once per step and its issue
	// width sits idle.  Lockstep hands it `nActive` INDEPENDENT Map
	// evaluations at a time, which pipeline against each other.  Measured
	// on plank_closeup, where this is 99.3 % of the occlusion cost: 71.1 s
	// -> 38.9 s of a 120 s frame, i.e. the same 181.7 evaluations per query
	// at 1.86 ns each become 1.02 ns each.  (The comparison that names the
	// cause: convexity's 80 samples have no chain between them at all and
	// already cost 0.94 ns each.)
	Vector3 dir[kOcclusionDirs];
	Scalar  tRay[kOcclusionDirs];
	int     live[kOcclusionDirs];
	Scalar  dVal[kOcclusionDirs];
	for( int i = 0; i < kOcclusionDirs; ++i ) {
		const Vector3& c = L.cosDir[i];
		dir[i] = Vector3(
			su.x*c.x + sv.x*c.y + onb.w().x*c.z,
			su.y*c.x + sv.y*c.y + onb.w().y*c.z,
			su.z*c.x + sv.z*c.y + onb.w().z*c.z );
		tRay[i] = R * kMarchStart;
		live[i] = i;
	}
	int nLive = kOcclusionDirs;
	int escaped = 0;
	for( int step = 0; step < kMarchMaxSteps && nLive > 0; ++step ) {
		for( int j = 0; j < nLive; ++j ) {
			const int k = live[j];
			const Vector3& w = dir[k];
			const Scalar   t = tRay[k];
			dVal[j] = sense * ( Map( Point3( origin.x + w.x*t, origin.y + w.y*t, origin.z + w.z*t ) ) - d0 );
		}
		int m = 0;
		for( int j = 0; j < nLive; ++j ) {
			const int    k = live[j];
			const Scalar d = dVal[j];
			// Inside the solid: this direction is BLOCKED, and drops out
			// without being counted.
			if( !( d > Scalar(0) ) ) continue;
			// Nothing within the remaining reach can be nearer than `d`, so
			// once `d` covers it the ray provably escapes -- the early-out
			// that keeps an open hemisphere from marching all the way out.
			if( d >= R - tRay[k] ) { ++escaped; continue; }
			tRay[k] += ( d > R*kMarchMinStep ) ? d : R*kMarchMinStep;
			if( tRay[k] >= R ) { ++escaped; continue; }
			live[m++] = k;
		}
		nLive = m;
	}
	// Ran out of steps -- reported ESCAPED, as the step-cap note above says.
	escaped += nLive;

	Scalar ao = Scalar( escaped ) / Scalar( kOcclusionDirs );
	if( !RISE::IsFiniteDouble( static_cast<double>( ao ) ) ) {
		return false;
	}
	if( ao < Scalar(0) ) ao = Scalar(0);
	if( ao > Scalar(1) ) ao = Scalar(1);
	outValue = ao;
	return true;
}

bool SDFGeometry::ComputeConvexity( const SurfaceSignalInfo& hit,
	const Scalar radiusFraction, const bool /*bRadiusIsConstant*/, Scalar& outValue ) const
{
	Scalar R = 0, d0 = 0;
	if( !PrepareSignalQuery( hit, radiusFraction, R, d0 ) ) {
		return false;
	}

	// BALL-VOLUME ACCESSIBILITY, and NO TANGENT FRAME.  Each lattice point
	// is used with both signs, and central symmetry through the query point
	// maps the half-space {d < 0} onto {d > 0} for ANY plane through it --
	// so a planar surface gives A = 1/2 EXACTLY, at every orientation and
	// every sample count.  (An earlier draft aligned the set to the hit
	// normal and mirrored about the tangent plane, which is also exact on a
	// plane but needs an orthonormal basis, and every branchless ONB has a
	// discontinuity somewhere on the sphere: measured frame-rotation spread
	// of A at a 90-degree wedge was 0.028, a 5.5 % step in the mask wherever
	// the branch flipped.)
	const Scalar w = R * kBallBandFraction;
	const Scalar sense = hit.bComplementedField ? Scalar(-1) : Scalar(1);
	const BallLattice& L = TheBallLattice();
	const Point3& p = hit.ptObject;
	// This hit's rotation of the ball set (kSignalRotations).  Central
	// symmetry is preserved by every entry in the table, so the planar
	// A = 1/2 identity the paragraph above turns on is untouched.
	const Point3* pts = L.pt[ SignalRotationIndex( hit.ptObject ) ];
	Scalar acc = Scalar(0);
	for( int i = 0; i < kBallPairs; ++i ) {
		const Scalar ox = R * pts[i].x, oy = R * pts[i].y, oz = R * pts[i].z;
		acc += BallSmoothStep( sense * ( Map( Point3( p.x + ox, p.y + oy, p.z + oz ) ) - d0 ), w );
		acc += BallSmoothStep( sense * ( Map( Point3( p.x - ox, p.y - oy, p.z - oz ) ) - d0 ), w );
	}
	const Scalar A = acc / Scalar( 2 * kBallPairs );
	if( !RISE::IsFiniteDouble( static_cast<double>( A ) ) ) {
		return false;
	}

	// The EXCESS above the planar half.  0 on a plane and in every cavity
	// (occlusion owns that side); 0.5 at a 90-degree arris, 0.75 at a
	// three-face corner, -> 1 at a knife edge; 3R/(8*rho) on a convex sphere
	// of radius rho.
	Scalar cvx = Scalar(2) * A - Scalar(1);
	if( cvx < Scalar(0) ) cvx = Scalar(0);
	if( cvx > Scalar(1) ) cvx = Scalar(1);
	outValue = cvx;
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
	// A keyframed scale reaches the field WITHOUT passing through
	// `ParsePartLines`, so it carries its own copy of that function's clamp --
	// an interpolated scale sweeping through 0 is exactly how a degenerate part
	// appears at runtime in an otherwise well-authored scene.  A non-finite
	// value is REJECTED (the part keeps the scale it had) rather than clamped:
	// there is no magnitude to clamp a NaN to.  Both paths warn every frame
	// they fire, which is the intent -- the field is silently mis-scaled
	// otherwise, and `SelfHitRootFloor` inflates with it.
	case eKFScale: {
		Vector3 sc = *(Vector3*)val.getValue();
		const int verdict = SanitizePartScale_( sc );
		if( verdict == ePartScaleNonFinite ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SDFGeometry::SetIntermediateValue:: keyframed scale for part %u is not a finite number "
				"(%g, %g, %g) -- ignored, the part keeps its previous scale",
				idx, sc.x, sc.y, sc.z );
			break;
		}
		if( verdict == ePartScaleClamped ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SDFGeometry::SetIntermediateValue:: keyframed scale for part %u has a component below the "
				"minimum magnitude %g; clamped to (%g, %g, %g).  A zero-scale axis squashes the field's "
				"Lipschitz ratio to 1e-9 and inflates this geometry's self-hit floor past its own size",
				idx, kMinPartScaleMag, sc.x, sc.y, sc.z );
		}
		pt.scale = sc;
		RecomputePartDerived( pt );
	} break;
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

		// DEGENERATE SCALE (adversarial review of 384e3752, P1-2).  The other
		// half of the same authoring slip: a 0 (or a typo'd 0.0000001) in the
		// `<sx sy sz>` slot.  Nothing downstream reads it as an error -- the
		// magnitude is floored at 1e-9 so `invScale` stays finite -- but that
		// floor IS the part's Lipschitz shrink, and `SelfHitRootFloor` divides
		// the sphere-tracer's step-off band by it: a unit sphere part claims
		// 6.9e-5 at `scale (1,1,1)` and 5.66e4 at `scale (1,1,0)`.  Inside a CSG
		// composite that number is an ownership-ray reach, so the degenerate
		// part charges its floor on geometry tens of units away.  Clamp
		// and warn (the superellipsoid pattern above); a NON-FINITE component
		// has no magnitude to clamp to, so it is a hard parse error like any
		// other malformed token.
		{
			Vector3 sc( sx, sy, sz );
			const int verdict = SanitizePartScale_( sc );
			if( verdict == ePartScaleNonFinite ) {
				GlobalLog()->PrintEx( eLog_Error,
					"SDFGeometry::ParsePartLines:: part %u (`%s`) at line %u of %s has a non-finite scale "
					"component in <sx sy sz> (%g, %g, %g)",
					(unsigned int)out.size(), ts, lineNo, ctx, sx, sy, sz );
				return false;
			}
			if( verdict == ePartScaleClamped ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SDFGeometry::ParsePartLines:: part %u (`%s`) at line %u of %s has a scale component below "
					"the minimum magnitude %g in <sx sy sz> (%g, %g, %g); clamped to (%g, %g, %g).  A zero-scale "
					"axis squashes the field's Lipschitz ratio to 1e-9, which inflates this geometry's self-hit "
					"floor past its own size; scale the primitive's <a b c> instead",
					(unsigned int)out.size(), ts, lineNo, ctx, kMinPartScaleMag,
					sx, sy, sz, sc.x, sc.y, sc.z );
			}
			sx = sc.x; sy = sc.y; sz = sc.z;
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
