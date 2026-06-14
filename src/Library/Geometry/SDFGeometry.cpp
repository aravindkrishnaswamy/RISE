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
#include <string>

#include "GeometryUtilities.h"		// MakeIndexedTriangleSameIdx for TessellateToMesh

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
		const Scalar a  = std::sqrt( std::max( Scalar(1) - b*b, Scalar(0) ) );
		const Scalar k  = qx * (-b) + qy * a;
		if( k < Scalar(0) )   return std::sqrt( qx*qx + qy*qy ) - r1;
		if( k > a * h )       { const Scalar dyt = qy - h; return std::sqrt( qx*qx + dyt*dyt ) - r2; }
		return qx * a + qy * b - r1;
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
		case SDFGeometry::ePrimCapsule:   rx = rz = pt.a;            ry0 = -(pt.b+pt.a); ry1 = pt.b+pt.a;   break;
		case SDFGeometry::ePrimRoundCone: rx = rz = std::max(pt.a,pt.b); ry0 = -pt.a;   ry1 = pt.c + pt.b; break;
		default:                          rx = rz = pt.a;            ry0 = -pt.a;        ry1 = pt.a;        break;
		}
		// box geometry uses (a,b,c) per axis
		if( pt.type == SDFGeometry::ePrimBox || pt.type == SDFGeometry::ePrimRoundBox ) {
			lmin = Point3( -pt.a, -pt.b, -pt.c );
			lmax = Point3(  pt.a,  pt.b,  pt.c );
		} else {
			lmin = Point3( -rx, ry0, -rz );
			lmax = Point3(  rx, ry1,  rz );
		}
	}
}

//////////////////////////////////////////////////////////////////////

SDFGeometry::Part SDFGeometry::MakePart(
	const SDFPrim type, const SDFOp op, const Scalar k,
	const Point3& pos, const Scalar exDeg, const Scalar eyDeg, const Scalar ezDeg,
	const Vector3& scale, const Scalar a, const Scalar b, const Scalar c, const Scalar round )
{
	Part pt;
	pt.type = type; pt.op = op; pt.k = k; pt.pos = pos;
	pt.a = a; pt.b = b; pt.c = c; pt.round = round;
	pt.scale = scale;

	const Scalar DEG = Scalar(PI) / Scalar(180.0);
	const Scalar ex = exDeg*DEG, ey = eyDeg*DEG, ez = ezDeg*DEG;
	const Scalar cx = std::cos(ex), sx = std::sin(ex);
	const Scalar cy = std::cos(ey), sy = std::sin(ey);
	const Scalar cz = std::cos(ez), sz = std::sin(ez);

	// R = Rz(ez) * Ry(ey) * Rx(ex); store its COLUMNS (local axes in object space).
	pt.cx = Vector3(  cz*cy,                 sz*cy,                 -sy     );
	pt.cy = Vector3(  cz*sy*sx - sz*cx,      sz*sy*sx + cz*cx,      cy*sx   );
	pt.cz = Vector3(  cz*sy*cx + sz*sx,      sz*sy*cx - cz*sx,      cy*cx   );

	const Scalar sxA = ( std::fabs(scale.x) > Scalar(1e-9) ) ? scale.x : Scalar(1e-9);
	const Scalar syA = ( std::fabs(scale.y) > Scalar(1e-9) ) ? scale.y : Scalar(1e-9);
	const Scalar szA = ( std::fabs(scale.z) > Scalar(1e-9) ) ? scale.z : Scalar(1e-9);
	pt.invScale = Vector3( Scalar(1)/sxA, Scalar(1)/syA, Scalar(1)/szA );
	pt.minScale = std::min( std::fabs(sxA), std::min( std::fabs(syA), std::fabs(szA) ) );
	return pt;
}

SDFGeometry::SDFGeometry( const std::vector<Part>& parts, const unsigned int maxSteps, const Scalar surfaceEpsilonFraction, const unsigned int samplingDetail ) :
	m_parts( parts ),
	m_maxSteps( maxSteps > 0 ? maxSteps : 256 ),
	m_epsFrac( surfaceEpsilonFraction > 0 ? surfaceEpsilonFraction : Scalar(5e-5) ),
	m_eps( Scalar(1e-4) ),
	m_diagonal( Scalar(1) ),
	m_samplingDetail( samplingDetail < 8 ? 8 : ( samplingDetail > 256 ? 256 : samplingDetail ) ),
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
	m_surfaceArea( 0 ),
	m_isHeightfield( true ),
	m_pHeightfield( field ),
	m_hfRadius( radius ),
	m_hfScale( scale ),
	m_hfLip( 2 )
{
	if( m_pHeightfield ) m_pHeightfield->addref();
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
	ComputeBounds();
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
	//   union     -> running = AABB-union( running, partBox )
	//   smin      -> running = AABB-union( running, partBox ) then inflate by k
	//                (a polynomial smin of radius k bulges BOTH the part surface
	//                 and the running solid by at most k near the blend)
	//   intersect -> running = AABB-intersection( running, partBox )
	//   subtract  -> no-op (a carve only ever shrinks the solid)
	// e.g. for [ unionA1, intersectC, unionA2 ] this yields
	// ( box(A1) INTERSECT box(C) ) UNION box(A2) -- the second lobe survives.

	// World-space AABB of one part's primitive: 8 local corners -> scale ->
	// rotate ( R*v = cx*vx + cy*vy + cz*vz ) -> translate.
	auto worldAABB = []( const Part& pt, Point3& outMn, Point3& outMx )
	{
		Point3 lmin, lmax;
		primLocalAABB( pt, lmin, lmax );
		const Scalar xs[2] = { lmin.x, lmax.x };
		const Scalar ys[2] = { lmin.y, lmax.y };
		const Scalar zs[2] = { lmin.z, lmax.z };
		outMn = Point3(  RISE_INFINITY,  RISE_INFINITY,  RISE_INFINITY );
		outMx = Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );
		for( int cxi = 0; cxi < 2; ++cxi )
		for( int cyi = 0; cyi < 2; ++cyi )
		for( int czi = 0; czi < 2; ++czi )
		{
			const Scalar vx = xs[cxi] * pt.scale.x;
			const Scalar vy = ys[cyi] * pt.scale.y;
			const Scalar vz = zs[czi] * pt.scale.z;
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
		worldAABB( pt, pmn, pmx );

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
			if( pt.op == eOpSmin && pt.k > 0 ) {
				// the blend bulges both sides by up to k near the seam
				mn.x -= pt.k; mn.y -= pt.k; mn.z -= pt.k;
				mx.x += pt.k; mx.y += pt.k; mx.z += pt.k;
			}
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

Scalar SDFGeometry::Map( const Point3& p ) const
{
	if( m_isHeightfield ) {
		const Scalar R=m_hfRadius;
		const Scalar u=(p.x+R)/(2*R), v=(p.y+R)/(2*R);
		const Scalar uu=u<0?Scalar(0):(u>1?Scalar(1):u);
		const Scalar vv=v<0?Scalar(0):(v>1?Scalar(1):v);
		const Scalar hgt=m_hfScale*( m_pHeightfield ? m_pHeightfield->Evaluate(uu,vv) : Scalar(0) );
		Scalar d=(p.z-hgt)/m_hfLip;
		const Scalar ox=std::fabs(p.x)-R, oy=std::fabs(p.y)-R;
		if( ox>0 || oy>0 ){ const Scalar hx=ox>0?ox:Scalar(0), hy=oy>0?oy:Scalar(0);
			const Scalar horiz=std::sqrt(hx*hx+hy*hy); if(horiz>d) d=horiz; }
		return d;
	}

	Scalar d = Scalar(1e30);
	for( size_t i = 0; i < m_parts.size(); ++i )
	{
		const Part& pt = m_parts[i];
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
	const Scalar surfBand = m_eps * Scalar(2);
	if( std::fabs( d0 ) <= surfBand ) {
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
	std::call_once( m_samplingOnce, [this]() {
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
		const Scalar hfd = std::max( m_eps * Scalar(8), m_diagonal * Scalar(5e-4) );
		unsigned int jClamped = 0;
		auto jacobianAt = [this, hfd, &jClamped]( const Point3& x ) -> Scalar {
			const Point3 y = ProjectToSurface( x );
			const Vector3 n = GradientNormal( y );
			const Scalar d = ( x.x - y.x )*n.x + ( x.y - y.y )*n.y + ( x.z - y.z )*n.z;
			// one-sided FD divergence of the unit normal = k1 + k2 at y
			const Vector3 nX = GradientNormal( Point3( y.x + hfd, y.y, y.z ) );
			const Vector3 nY = GradientNormal( Point3( y.x, y.y + hfd, y.z ) );
			const Vector3 nZ = GradientNormal( Point3( y.x, y.y, y.z + hfd ) );
			const Scalar div = ( ( nX.x - n.x ) + ( nY.y - n.y ) + ( nZ.z - n.z ) ) / hfd;
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

IKeyframeParameter* SDFGeometry::KeyframeFromParameters( const String& /*name*/, const String& /*value*/ )
{
	return 0;   // v1: the SDF field is not keyframe-animatable
}

void SDFGeometry::SetIntermediateValue( const IKeyframeParameter& /*val*/ )
{
}

void SDFGeometry::RegenerateData()
{
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
				"SDFGeometry::ParsePartLines:: unknown primitive `%s` at line %u of %s (want sphere|box|roundbox|cylinder|torus|capsule|roundcone)",
				ts, lineNo, ctx );
			return false;
		}
		if( !ParseSDFOpToken( os, op ) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"SDFGeometry::ParsePartLines:: unknown op `%s` at line %u of %s (want union|smin|subtract|intersect)",
				os, lineNo, ctx );
			return false;
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
