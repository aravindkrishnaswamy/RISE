//////////////////////////////////////////////////////////////////////
//
//  HairGeometry.cpp - Implementation of the runtime curve primitive.
//
//  See HairGeometry.h for the storage rationale, the Catmull-Rom
//  phantom-endpoint convention, the face-flag / exit-info conventions,
//  and the attribution note for the PBRT-Curve-derived intersection
//  strategy.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HairGeometry.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/IWriteBuffer.h"
#include "../Interfaces/IReadBuffer.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/FiniteMath.h"
#include <cmath>
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Runtime recursion cap for the adaptive splitter.  2^10 pieces is
	//! far beyond anything a plausibly-authored strand needs; it exists
	//! so a degenerate (e.g. NaN-adjacent, or absurdly kinked) span
	//! cannot spin the traversal.
	const int	kMaxRuntimeSplitDepth	= 10;

	//! Flatness target for the RUNTIME splitter, as a fraction of the
	//! strand's width: recursion stops once the piece's deviation from
	//! its own chord is bounded below this times the width, at which
	//! point the base case's straight-line approximation is well inside
	//! the width test's own resolution.  0.05 matches the constant PBRT
	//! uses for the same purpose.
	const Scalar kRuntimeFlatnessFraction = Scalar(0.05);

	//! Evaluates the power-basis cubic  c0 + u c1 + u^2 c2 + u^3 c3.
	inline Vector3 EvalCubic( const Vector3 c[4], const Scalar u )
	{
		return c[0] + ( c[1] + ( c[2] + c[3] * u ) * u ) * u;
	}

	//! Its derivative  c1 + 2u c2 + 3u^2 c3.
	inline Vector3 EvalCubicDeriv( const Vector3 c[4], const Scalar u )
	{
		return c[1] + ( c[2] * Scalar(2.0) + c[3] * ( Scalar(3.0) * u ) ) * u;
	}

	//! Reparametrises the cubic `c` onto the sub-interval [a, b]:
	//! returns the power-basis coefficients `q` of  q(w) = c(a + w(b-a)),
	//! w in [0,1].  Straight Taylor expansion about `a`, scaled by
	//! h = b - a:
	//!    q0 = c(a)
	//!    q1 = h  c'(a)
	//!    q2 = h^2 c''(a)/2  = h^2 (c2 + 3 c3 a)
	//!    q3 = h^3 c'''/6    = h^3 c3
	inline void Reparametrise( const Vector3 c[4], const Scalar a, const Scalar b, Vector3 q[4] )
	{
		const Scalar h  = b - a;
		const Scalar h2 = h * h;
		q[0] = EvalCubic( c, a );
		q[1] = EvalCubicDeriv( c, a ) * h;
		q[2] = ( c[2] + c[3] * ( Scalar(3.0) * a ) ) * h2;
		q[3] = c[3] * ( h2 * h );
	}

	//! Power basis -> Bernstein (cubic Bezier) control points.  The
	//! curve on [0,1] lies inside the CONVEX HULL of these four points,
	//! which is what makes the traversal's box rejection conservative.
	inline void ToBernstein( const Vector3 q[4], Vector3 b[4] )
	{
		const Scalar third = Scalar(1.0) / Scalar(3.0);
		b[0] = q[0];
		b[1] = q[0] + q[1] * third;
		b[2] = q[0] + q[1] * ( Scalar(2.0) * third ) + q[2] * third;
		b[3] = q[0] + q[1] + q[2] + q[3];
	}

	//! An ORTHONORMAL ray frame.  Deviation from PBRT, which permutes
	//! and SHEARS: a shear is cheaper but distorts distances, so pbrt
	//! has to compensate inside the width test.  A rotation preserves
	//! them exactly, so the 2D point-to-line distance below IS the true
	//! world-space distance from the ray axis to the curve and can be
	//! compared against the half width directly.  `ez` is the UNIT ray
	//! direction, so a ray-space z is a distance; the ray parameter that
	//! RISE reports (`ri.range`, which multiplies the possibly-unnormalised
	//! `ray.Dir()`) is recovered as z * invDirLength.
	struct RayFrame
	{
		Point3	o;
		Vector3	ex, ey, ez;
		Scalar	dirLength;		///< |ray.Dir()|
		Scalar	invDirLength;	///< 1 / |ray.Dir()|
		bool	degenerate;		///< true if ray.Dir() was too near zero to normalise -- see IntersectSegment, which checks this and returns false immediately rather than relying on the z-range test to reject on its own

		explicit RayFrame( const Ray& ray ) : degenerate( false )
		{
			o  = ray.origin;
			Vector3 d = ray.Dir();
			dirLength = Vector3Ops::Magnitude( d );
			if( dirLength < NEARZERO ) {
				// Degenerate ray: fabricate a valid orthonormal frame so
				// downstream arithmetic stays finite, and flag it.  An
				// earlier version of this comment claimed the z-range
				// test rejects everything on its own -- that was FALSE:
				// with d == (0,0,0), ez defaults to (0,0,1) and the
				// fabricated frame's z-window ([0, zMax]) still passes
				// for any curve point at z ~ 0, so nothing was actually
				// being rejected before this flag existed.
				dirLength    = Scalar(1.0);
				invDirLength = Scalar(1.0);
				ez = Vector3( 0, 0, 1 );
				degenerate   = true;
			} else {
				invDirLength = Scalar(1.0) / dirLength;
				ez = d * invDirLength;
			}
			// Any axis not parallel to ez.
			const Vector3 helper = ( fabs( ez.x ) < Scalar(0.8) ) ? Vector3( 1, 0, 0 ) : Vector3( 0, 1, 0 );
			ex = Vector3Ops::Normalize( Vector3Ops::Cross( helper, ez ) );
			ey = Vector3Ops::Cross( ez, ex );
		}

		//! Projects a coefficient set into the frame.  `c[0]` is a
		//! POSITION (so the ray origin is subtracted); `c[1..3]` are
		//! DIRECTION-like (so they are only rotated).
		void ProjectCubic( const Vector3 c[4], Vector3 out[4] ) const
		{
			const Vector3 d0( c[0].x - o.x, c[0].y - o.y, c[0].z - o.z );
			out[0] = Vector3( Vector3Ops::Dot( d0, ex ), Vector3Ops::Dot( d0, ey ), Vector3Ops::Dot( d0, ez ) );
			for( int k = 1; k < 4; ++k ) {
				out[k] = Vector3( Vector3Ops::Dot( c[k], ex ), Vector3Ops::Dot( c[k], ey ), Vector3Ops::Dot( c[k], ez ) );
			}
		}
	};

	//! Everything the recursive walk needs, gathered once per
	//! sub-segment so the recursion carries a single reference.
	struct SegmentContext
	{
		Vector3	rc[4];			///< the SPAN's cubic, in ray space
		Scalar	halfMaxWidth;	///< conservative half width used to pad the hull test
		Scalar	zMin, zMax;		///< admissible ray-space depth window
		Scalar	spanBase;		///< the span index, so uGlobal = spanBase + uLocal
		Scalar	rootWidth, tipWidth;
		Scalar	arcAtSpanStart, arcAtSpanEnd, arcTotal;
	};

	//! Full width of the strand at the given LOCAL span parameter,
	//! obtained via the arc-length fraction so it agrees exactly with
	//! the `s` the hit reports.
	inline Scalar WidthAtLocal( const SegmentContext& ctx, const Scalar uLocal, Scalar& outArcFraction )
	{
		Scalar sArc = 0;
		if( ctx.arcTotal > 0 ) {
			sArc = ( ctx.arcAtSpanStart + uLocal * ( ctx.arcAtSpanEnd - ctx.arcAtSpanStart ) ) / ctx.arcTotal;
		}
		if( sArc < 0 ) sArc = 0;
		if( sArc > 1 ) sArc = 1;
		outArcFraction = sArc;
		return ctx.rootWidth + sArc * ( ctx.tipWidth - ctx.rootWidth );
	}

	//! Base case: the piece [a, b] is flat enough to treat as a straight
	//! line.  Project the ray axis (the ray-space origin) onto that
	//! line, clamp the parameter into the piece, evaluate the curve
	//! there, and accept if the 2D distance is within the half width and
	//! the depth is inside the window.
	//!
	//! NOTE on the clamp: clamping to the piece's endpoints means the
	//! ROOT and TIP of a strand get a rounded cap of the local half
	//! width, and means neighbouring pieces both "own" their shared
	//! joint.  The former is intended (a hair tip is not a flat cut);
	//! the latter is harmless because both pieces then report the same
	//! crossing and the caller keeps the nearer one.  PBRT instead adds
	//! two endpoint tangent tests to reject those; they are omitted here
	//! deliberately, and the differential test in
	//! tests/HairGeometryTest.cpp uses the same clamped convention in
	//! its independent reference so the two agree by design rather than
	//! by accident.
	bool TestFlatPiece( const SegmentContext& ctx, const Scalar a, const Scalar b,
	                    Scalar& outZ, Scalar& outULocal )
	{
		const Vector3 pa = EvalCubic( ctx.rc, a );
		const Vector3 pb = EvalCubic( ctx.rc, b );

		const Scalar sx = pb.x - pa.x;
		const Scalar sy = pb.y - pa.y;
		const Scalar den = sx * sx + sy * sy;

		Scalar w = 0;
		if( den > NEARZERO ) {
			w = ( -pa.x * sx - pa.y * sy ) / den;
			if( w < 0 ) w = 0;
			if( w > 1 ) w = 1;
		}

		const Scalar u  = a + w * ( b - a );
		const Vector3 pc = EvalCubic( ctx.rc, u );

		Scalar sArc = 0;
		const Scalar width = WidthAtLocal( ctx, u, sArc );
		const Scalar halfW = width * Scalar(0.5);

		const Scalar dist2 = pc.x * pc.x + pc.y * pc.y;
		if( dist2 > halfW * halfW ) {
			return false;
		}
		if( pc.z < ctx.zMin || pc.z > ctx.zMax ) {
			return false;
		}

		outZ      = pc.z;
		outULocal = u;
		return true;
	}

	//! Recursive adaptive splitter.  Returns true if a crossing was
	//! found, and reports the NEAREST one within this piece.
	bool RecursivePieceIntersect( const SegmentContext& ctx, const Scalar a, const Scalar b,
	                              const int depth, Scalar& bestZ, Scalar& bestU )
	{
		// Conservative rejection against the Bernstein hull of the piece.
		Vector3 q[4], hull[4];
		Reparametrise( ctx.rc, a, b, q );
		ToBernstein( q, hull );

		Scalar minX = hull[0].x, maxX = hull[0].x;
		Scalar minY = hull[0].y, maxY = hull[0].y;
		Scalar minZ = hull[0].z, maxZ = hull[0].z;
		for( int k = 1; k < 4; ++k ) {
			minX = std::min( minX, hull[k].x );  maxX = std::max( maxX, hull[k].x );
			minY = std::min( minY, hull[k].y );  maxY = std::max( maxY, hull[k].y );
			minZ = std::min( minZ, hull[k].z );  maxZ = std::max( maxZ, hull[k].z );
		}

		const Scalar pad = ctx.halfMaxWidth;
		if( minX - pad > 0 || maxX + pad < 0 ) return false;
		if( minY - pad > 0 || maxY + pad < 0 ) return false;
		if( maxZ + pad < ctx.zMin || minZ - pad > ctx.zMax ) return false;

		if( depth <= 0 ) {
			Scalar z = 0, u = 0;
			if( !TestFlatPiece( ctx, a, b, z, u ) ) return false;
			bestZ = z;
			bestU = u;
			return true;
		}

		const Scalar m = ( a + b ) * Scalar(0.5);
		Scalar z0 = 0, u0 = 0, z1 = 0, u1 = 0;
		const bool h0 = RecursivePieceIntersect( ctx, a, m, depth - 1, z0, u0 );
		const bool h1 = RecursivePieceIntersect( ctx, m, b, depth - 1, z1, u1 );

		if( h0 && h1 ) {
			if( z0 <= z1 ) { bestZ = z0; bestU = u0; } else { bestZ = z1; bestU = u1; }
			return true;
		}
		if( h0 ) { bestZ = z0; bestU = u0; return true; }
		if( h1 ) { bestZ = z1; bestU = u1; return true; }
		return false;
	}

	//! Bound on how far a cubic strays from its own chord over [a, b], in
	//! the FULL 3D sense (x, y, AND z).  This is called from two sites
	//! with two different meanings for those axes:
	//!   * BUILD time (HairGeometry's ctor, choosing the per-span
	//!     sub-segment split): `c` is the span's OBJECT-SPACE
	//!     coefficient set -- there is no ray yet, so x, y, z are
	//!     ordinary spatial axes and all three can carry real curvature
	//!     that the AABB split must resolve.
	//!   * RUN time (IntersectSegment, choosing the adaptive recursion
	//!     depth): `c` is the RAY-FRAME-projected coefficient set, so z
	//!     is depth along the ray and the width test downstream only
	//!     resolves x/y (perpendicular-to-ray) deviation directly.
	//!     Including z here too is still correct -- folding in an extra
	//!     nonnegative term into the L2 norm can only INCREASE the
	//!     chosen split depth, never under-split -- and cheap.
	//! An earlier version of this bound used x/y only, on the incorrect
	//! premise that z was ray-space depth at BOTH call sites.  At the
	//! build call site that premise is false: a span curving sharply in
	//! object-space z alone was getting the shallowest split depth
	//! regardless of how curved it actually was.
	//!
	//! For q(w) on [0,1], |q''(w)| = |2 q2 + 6 q3 w| attains its maximum
	//! at an endpoint, and a curve deviates from its chord by at most
	//! max|q''| / 8.  Splitting into 2^n pieces scales q2 by 4^-n and q3
	//! by 8^-n, so the bound scales by (at worst) 4^-n; solving
	//! L / (8 * 4^n) <= eps gives n = ceil( log2(L / (8 eps)) / 2 ).
	int ChooseSplitDepth( const Vector3 c[4], const Scalar a, const Scalar b,
	                      const Scalar eps, const int maxDepth )
	{
		if( !( eps > 0 ) ) return maxDepth;

		Vector3 q[4];
		Reparametrise( c, a, b, q );

		const Scalar ax = Scalar(2.0) * q[2].x, ay = Scalar(2.0) * q[2].y, az = Scalar(2.0) * q[2].z;
		const Scalar bx = ax + Scalar(6.0) * q[3].x, by = ay + Scalar(6.0) * q[3].y, bz = az + Scalar(6.0) * q[3].z;
		const Scalar L  = std::max( sqrt( ax*ax + ay*ay + az*az ), sqrt( bx*bx + by*by + bz*bz ) );

		if( !RISE::IsFiniteDouble( L ) ) return maxDepth;

		const Scalar ratio = L / ( Scalar(8.0) * eps );
		if( ratio <= 1 ) return 0;

		const int d = (int)ceil( log( ratio ) / log( Scalar(4.0) ) );
		if( d < 0 ) return 0;
		if( d > maxDepth ) return maxDepth;
		return d;
	}
}

//////////////////////////////////////////////////////////////////////
//  Construction
//////////////////////////////////////////////////////////////////////

HairGeometry::HairGeometry( const std::vector<StrandDesc>& strands ) :
  pSegBVH( 0 ),
  bbox( Point3( 0, 0, 0 ), Point3( 0, 0, 0 ) ),
  nSegments( 0 ),
  nRejectedStrands( 0 )
{
	strandCPBegin.push_back( 0 );

	BoundingBox accum( Point3(  RISE_INFINITY,  RISE_INFINITY,  RISE_INFINITY ),
	                   Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );

	std::vector<HairSegmentRef> segs;

	for( size_t si = 0; si < strands.size(); ++si )
	{
		const StrandDesc& sd = strands[si];

		// ---- validation.  Rejections are per-strand, not fatal: a
		// groom is generated data, and one bad follicle must not cost
		// the other 999,999.
		if( sd.controlPoints.size() < 2 ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"HairGeometry:: strand %u rejected -- a strand needs at least 2 control points, got %u",
				(unsigned)si, (unsigned)sd.controlPoints.size() );
			++nRejectedStrands;
			continue;
		}
		if( sd.controlPoints.size() > kMaxControlPointsPerStrand ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"HairGeometry:: strand %u rejected -- %u control points exceeds the %u limit imposed by the segment record's 16-bit span index",
				(unsigned)si, (unsigned)sd.controlPoints.size(), kMaxControlPointsPerStrand );
			++nRejectedStrands;
			continue;
		}
		if( !( sd.rootWidth > 0 ) || !( sd.tipWidth > 0 ) ||
		    !RISE::IsFiniteDouble( sd.rootWidth ) || !RISE::IsFiniteDouble( sd.tipWidth ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"HairGeometry:: strand %u rejected -- root/tip widths must be finite and strictly positive, got %g / %g",
				(unsigned)si, (double)sd.rootWidth, (double)sd.tipWidth );
			++nRejectedStrands;
			continue;
		}
		{
			bool bad = false;
			for( size_t k = 0; k < sd.controlPoints.size() && !bad; ++k ) {
				const Point3& p = sd.controlPoints[k];
				bad = !RISE::IsFiniteDouble( p.x ) || !RISE::IsFiniteDouble( p.y ) || !RISE::IsFiniteDouble( p.z );
			}
			if( bad ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"HairGeometry:: strand %u rejected -- one or more control points are not finite",
					(unsigned)si );
				++nRejectedStrands;
				continue;
			}
		}

		// ---- accept: append the control points (float storage).
		const unsigned int newStrand = (unsigned int)( strandCPBegin.size() - 1 );
		for( size_t k = 0; k < sd.controlPoints.size(); ++k ) {
			const Point3& p = sd.controlPoints[k];
			cps.push_back( (float)p.x );
			cps.push_back( (float)p.y );
			cps.push_back( (float)p.z );
			cpArcCum.push_back( 0.0f );		// filled below
		}
		strandCPBegin.push_back( (uint32_t)( cps.size() / 3 ) );
		strandRootWidth.push_back( (float)sd.rootWidth );
		strandTipWidth.push_back( (float)sd.tipWidth );
		strandRootU.push_back( (float)sd.rootUV.x );
		strandRootV.push_back( (float)sd.rootUV.y );

		const unsigned int nCP    = numControlPointsOfStrand( newStrand );
		const unsigned int nSpans = nCP - 1;
		const unsigned int base   = strandCPBegin[newStrand];

		// ---- per-span arc length (dense polyline sampling) and the
		// build-time sub-segment split.
		Scalar running = 0;
		cpArcCum[base] = 0.0f;
		const Scalar maxW = StrandMaxWidth( newStrand );

		for( unsigned int j = 0; j < nSpans; ++j )
		{
			Vector3 c[4];
			EvalSpanCoefficients( newStrand, j, c );

			Vector3 prev = EvalCubic( c, 0 );
			for( unsigned int k = 1; k <= kArcSamplesPerSpan; ++k ) {
				const Vector3 cur = EvalCubic( c, Scalar(k) / Scalar(kArcSamplesPerSpan) );
				running += Vector3Ops::Magnitude( cur - prev );
				prev = cur;
			}
			cpArcCum[base + j + 1] = (float)running;

			// Build-time split bound.  eps_build = max(strand width,
			// 2% of the span's chord length): stop splitting once the
			// span's deviation from its chord is smaller than the width
			// the AABB has to be padded by anyway (below which extra
			// splits buy nothing), or smaller than 2% of the span's own
			// length (which keeps a long gentle span from being called
			// "flat" merely because it is wide).
			const Vector3 chord = EvalCubic( c, 1 ) - EvalCubic( c, 0 );
			const Scalar epsBuild = std::max( maxW, Scalar(0.02) * Vector3Ops::Magnitude( chord ) );
			const int d = ChooseSplitDepth( c, 0, 1, epsBuild, (int)kMaxBuildSplitDepth );

			// Emit the sub-segment records, accumulating the whole-groom
			// box from their EXACT boxes as we go.
			//
			// Why not just hull the control points?  Because Catmull-Rom
			// does NOT stay inside its control polygon's hull -- it
			// overshoots at a direction reversal -- so a control-point
			// hull padded by half a width can genuinely clip the curve,
			// and the amount it clips by depends on the neighbouring
			// chord lengths rather than on anything constant.  The
			// per-segment box is derived from the piece's BERNSTEIN hull,
			// which does contain the curve exactly, so accumulating those
			// is both tight and provably conservative.
			const unsigned int nSub = 1u << d;
			for( unsigned int t = 0; t < nSub; ++t ) {
				HairSegmentRef ref;
				ref.strand   = (uint32_t)newStrand;
				ref.span     = (uint16_t)j;
				ref.subIndex = (uint8_t)t;
				ref.subDepth = (uint8_t)d;
				segs.push_back( ref );

				const BoundingBox sb = SegmentBoundingBox( ref );
				accum.Include( sb.ll );
				accum.Include( sb.ur );
			}
		}
	}

	nSegments = (unsigned int)segs.size();

	if( nSegments > 0 ) {
		bbox = accum;

		AccelerationConfig cfg;
		cfg.maxLeafSize         = 4;
		cfg.binCount            = 32;
		cfg.sahTraversalCost    = 1.0;
		cfg.sahIntersectionCost = 1.0;
		cfg.doubleSided         = true;		// set for parity with the mesh pattern (a ribbon has no back side to cull); INERT today -- grep confirms nothing in src/Library/Acceleration/ reads AccelerationConfig::doubleSided

		pSegBVH = new BVH<HairSegmentRef>( *this, segs, bbox, cfg );
		GlobalLog()->PrintNew( pSegBVH, __FILE__, __LINE__, "hair segment BVH" );
	} else {
		// Empty groom (no strands, or every strand rejected).  The
		// bounding box stays the zero-extent box at the object-space
		// origin set in the initialiser list: a DEGENERATE-BUT-VALID
		// box (ll == ur), never the inverted infinity box, because
		// Object::IntersectRay's pre-hit test feeds it straight to
		// RayBoxIntersection.  A zero-extent box is missed by virtually
		// every ray and, on the measure-zero ray that does graze it,
		// IntersectRay below still reports nothing.
		GlobalLog()->PrintEx( eLog_Warning,
			"HairGeometry:: groom is empty (%u strands supplied, %u rejected) -- it will intersect nothing",
			(unsigned)strands.size(), nRejectedStrands );
	}
}

HairGeometry::~HairGeometry()
{
	safe_release( pSegBVH );
}

//////////////////////////////////////////////////////////////////////
//  Curve evaluation
//////////////////////////////////////////////////////////////////////

Point3 HairGeometry::ControlPoint( unsigned int s, unsigned int i ) const
{
	const size_t o = ( (size_t)strandCPBegin[s] + i ) * 3;
	return Point3( (Scalar)cps[o], (Scalar)cps[o+1], (Scalar)cps[o+2] );
}

//! Control point `i` of strand `s` with the REFLECTED phantom-endpoint
//! convention of the class comment applied outside [0, n).
static inline Vector3 CPWithPhantom( const HairGeometry& g, unsigned int s, const int i, const int n )
{
	if( i < 0 ) {
		const Point3 p0 = g.ControlPoint( s, 0 );
		const Point3 p1 = g.ControlPoint( s, 1 );
		return Vector3( 2*p0.x - p1.x, 2*p0.y - p1.y, 2*p0.z - p1.z );
	}
	if( i > n - 1 ) {
		const Point3 pl = g.ControlPoint( s, (unsigned int)(n-1) );
		const Point3 pp = g.ControlPoint( s, (unsigned int)(n-2) );
		return Vector3( 2*pl.x - pp.x, 2*pl.y - pp.y, 2*pl.z - pp.z );
	}
	const Point3 p = g.ControlPoint( s, (unsigned int)i );
	return Vector3( p.x, p.y, p.z );
}

//! Uniform Catmull-Rom power-basis coefficients for span `span` of
//! strand `s`.  All arithmetic in Scalar (double) -- the float storage
//! is widened at the ControlPoint() boundary and never used again.
void HairGeometry::EvalSpanCoefficients( unsigned int s, unsigned int span, Vector3 c[4] ) const
{
	const int n = (int)numControlPointsOfStrand( s );
	const Vector3 P0 = CPWithPhantom( *this, s, (int)span - 1, n );
	const Vector3 P1 = CPWithPhantom( *this, s, (int)span,     n );
	const Vector3 P2 = CPWithPhantom( *this, s, (int)span + 1, n );
	const Vector3 P3 = CPWithPhantom( *this, s, (int)span + 2, n );

	c[0] = P1;
	c[1] = ( P2 - P0 ) * Scalar(0.5);
	c[2] = ( P0 * Scalar(2.0) - P1 * Scalar(5.0) + P2 * Scalar(4.0) - P3 ) * Scalar(0.5);
	c[3] = ( P1 * Scalar(3.0) - P2 * Scalar(3.0) + P3 - P0 ) * Scalar(0.5);
}

Point3 HairGeometry::EvaluateStrand( unsigned int s, const Scalar u, Vector3* outDeriv ) const
{
	const unsigned int nSpans = numControlPointsOfStrand( s ) - 1;
	Scalar uu = u;
	if( uu < 0 ) uu = 0;
	if( uu > (Scalar)nSpans ) uu = (Scalar)nSpans;

	unsigned int span = (unsigned int)floor( uu );
	if( span >= nSpans ) span = nSpans - 1;
	const Scalar local = uu - (Scalar)span;

	Vector3 c[4];
	EvalSpanCoefficients( s, span, c );
	if( outDeriv ) {
		*outDeriv = EvalCubicDeriv( c, local );
	}
	const Vector3 p = EvalCubic( c, local );
	return Point3( p.x, p.y, p.z );
}

Scalar HairGeometry::StrandArcLength( unsigned int s ) const
{
	const unsigned int nCP = numControlPointsOfStrand( s );
	return (Scalar)cpArcCum[ strandCPBegin[s] + nCP - 1 ];
}

Scalar HairGeometry::ArcFractionAt( unsigned int s, const Scalar u ) const
{
	const unsigned int nSpans = numControlPointsOfStrand( s ) - 1;
	const unsigned int base   = strandCPBegin[s];
	const Scalar total = StrandArcLength( s );
	if( !( total > 0 ) ) return 0;

	Scalar uu = u;
	if( uu < 0 ) uu = 0;
	if( uu > (Scalar)nSpans ) uu = (Scalar)nSpans;

	unsigned int span = (unsigned int)floor( uu );
	if( span >= nSpans ) span = nSpans - 1;
	const Scalar local = uu - (Scalar)span;

	const Scalar a0 = (Scalar)cpArcCum[base + span];
	const Scalar a1 = (Scalar)cpArcCum[base + span + 1];
	Scalar f = ( a0 + local * ( a1 - a0 ) ) / total;
	if( f < 0 ) f = 0;
	if( f > 1 ) f = 1;
	return f;
}

Scalar HairGeometry::StrandWidthAt( unsigned int s, const Scalar sArc ) const
{
	const Scalar r = (Scalar)strandRootWidth[s];
	const Scalar t = (Scalar)strandTipWidth[s];
	return r + sArc * ( t - r );
}

Scalar HairGeometry::StrandMaxWidth( unsigned int s ) const
{
	return std::max( (Scalar)strandRootWidth[s], (Scalar)strandTipWidth[s] );
}

//////////////////////////////////////////////////////////////////////
//  Segment bounds
//////////////////////////////////////////////////////////////////////

BoundingBox HairGeometry::SegmentBoundingBox( const MYOBJ elem ) const
{
	Vector3 c[4];
	EvalSpanCoefficients( elem.strand, elem.span, c );

	const Scalar inv = Scalar(1.0) / (Scalar)( 1u << elem.subDepth );
	const Scalar a = (Scalar)elem.subIndex * inv;
	const Scalar b = a + inv;

	Vector3 q[4], hull[4];
	Reparametrise( c, a, b, q );
	ToBernstein( q, hull );

	// The Bernstein hull bounds the curve exactly; pad by the strand's
	// max HALF width (the swept radius).
	const Scalar pad = StrandMaxWidth( elem.strand ) * Scalar(0.5);

	BoundingBox out( Point3(  RISE_INFINITY,  RISE_INFINITY,  RISE_INFINITY ),
	                 Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
	for( int k = 0; k < 4; ++k ) {
		out.Include( Point3( hull[k].x - pad, hull[k].y - pad, hull[k].z - pad ) );
		out.Include( Point3( hull[k].x + pad, hull[k].y + pad, hull[k].z + pad ) );
	}
	return out;
}

BoundingBox HairGeometry::GetElementBoundingBox( const MYOBJ elem ) const
{
	return SegmentBoundingBox( elem );
}

bool HairGeometry::ElementBoxIntersection( const MYOBJ elem, const BoundingBox& box ) const
{
	// Conservative box-vs-box, matching ObjectManager's element
	// specialization (ObjectManager.cpp:66-69) rather than the mesh's
	// exact triangle-vs-box test: a swept cubic has no cheap exact
	// test, and the BVH builder does not consume this predicate at all
	// (only GetElementBoundingBox and the ray tests) -- it exists to
	// satisfy the TreeElementProcessor contract.
	return box.DoIntersect( SegmentBoundingBox( elem ) );
}

char HairGeometry::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
{
	// Same reasoning as ElementBoxIntersection: classify the segment's
	// (conservative) bounding box, exactly as ObjectManager does for
	// whole objects.  Conservative in the safe direction -- a segment
	// that is really on one side may be reported as straddling, never
	// the other way round.
	return GeometricUtilities::WhichSideOfPlane( plane, SegmentBoundingBox( elem ) );
}

//////////////////////////////////////////////////////////////////////
//  Intersection
//////////////////////////////////////////////////////////////////////

bool HairGeometry::IntersectSegment( const Ray& ray, const Scalar tMax, const MYOBJ elem,
                                     Scalar& outT, Scalar& outU ) const
{
	const RayFrame frame( ray );
	if( frame.degenerate ) {
		return false;
	}

	Vector3 c[4];
	EvalSpanCoefficients( elem.strand, elem.span, c );

	SegmentContext ctx;
	frame.ProjectCubic( c, ctx.rc );

	const Scalar maxW = StrandMaxWidth( elem.strand );
	ctx.halfMaxWidth = maxW * Scalar(0.5);

	// NOTE: this zMin (exactly 0) and the final `t > NEARZERO` gate
	// below are DELIBERATELY not the same value.  zMin bounds candidate
	// SELECTION inside the recursive splitter -- a piece's whole
	// Bernstein-hull box is rejected or accepted against it, so it has
	// to be the mathematically correct z >= 0 half-space, not an
	// epsilon-shifted one, or a piece straddling z = 0 could be dropped
	// wrongly.  The final `t` gate exists purely to avoid
	// self-intersection against the ray's own origin.  This two-gate
	// mismatch is inherent to a ONE-CANDIDATE-PER-LEAF design (the
	// recursive splitter can only report the single nearest z within a
	// piece, so the self-intersection epsilon has to be re-applied at
	// the end rather than folded into the box test) -- pbrt's Curve
	// shares the identical structure for the same reason.
	ctx.zMin = 0;
	// tMax arrives as RISE_INFINITY on an unbounded closest-hit query;
	// scaling that by |dir| would overflow, so clamp rather than
	// multiply.
	ctx.zMax = ( tMax >= RISE_INFINITY * Scalar(0.5) ) ? RISE_INFINITY : ( tMax * frame.dirLength );

	const unsigned int base = strandCPBegin[elem.strand];
	ctx.spanBase       = (Scalar)elem.span;
	ctx.rootWidth      = (Scalar)strandRootWidth[elem.strand];
	ctx.tipWidth       = (Scalar)strandTipWidth[elem.strand];
	ctx.arcAtSpanStart = (Scalar)cpArcCum[base + elem.span];
	ctx.arcAtSpanEnd   = (Scalar)cpArcCum[base + elem.span + 1];
	ctx.arcTotal       = StrandArcLength( elem.strand );

	const Scalar inv = Scalar(1.0) / (Scalar)( 1u << elem.subDepth );
	const Scalar a = (Scalar)elem.subIndex * inv;
	const Scalar b = a + inv;

	const int depth = ChooseSplitDepth( ctx.rc, a, b,
	                                    kRuntimeFlatnessFraction * maxW,
	                                    kMaxRuntimeSplitDepth );

	Scalar z = 0, uLocal = 0;
	if( !RecursivePieceIntersect( ctx, a, b, depth, z, uLocal ) ) {
		return false;
	}

	const Scalar t = z * frame.invDirLength;
	if( !( t > NEARZERO ) || !( t < tMax ) ) {
		return false;
	}

	outT = t;
	outU = ctx.spanBase + uLocal;
	return true;
}

void HairGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem,
                                           const bool /*bHitFrontFaces*/, const bool /*bHitBackFaces*/ ) const
{
	// Face flags deliberately ignored -- see the class comment.
	Scalar t = 0, u = 0;
	if( !IntersectSegment( ri.ray, ri.range, elem, t, u ) ) {
		return;
	}

	// NATIVE CLOSEST-HIT GUARD.  The BVH shares one `ri` across every
	// primitive in a leaf and relies on the processor to keep the
	// closest-hit invariant itself (see the identical note on
	// TriangleMeshGeometryIndexed::RayElementIntersection).
	if( !( t < ri.range ) ) {
		return;
	}

	const unsigned int s = elem.strand;

	// ---- curve frame at the hit
	Vector3 dPdu;
	const Point3 C = EvaluateStrand( s, u, &dPdu );

	Vector3 T = dPdu;
	if( Vector3Ops::SquaredModulus( T ) < NEARZERO ) {
		// A stationary point of the parameterisation (coincident control
		// points).  Fall back to the span's chord direction; if that is
		// degenerate too, any unit vector keeps the outputs finite and
		// the hit is a sub-pixel speck of a zero-length strand.
		//
		// The chord is probed as (floor(u), floor(u)+1) EXCEPT at or past
		// the strand's tip, where floor(u)+1 would exceed nSpans and
		// EvaluateStrand's own clamp into [0, nSpans] would collapse both
		// probes onto the SAME point -- always reporting a zero chord at
		// the tip regardless of the strand's actual direction there.
		// Probe backward, (floor(u)-1, floor(u)), in that case instead.
		const Scalar nSpans = (Scalar)( numControlPointsOfStrand( s ) - 1 );
		const Scalar u0 = floor( u );
		Point3 e0, e1;
		if( u0 + Scalar(1.0) > nSpans ) {
			e0 = EvaluateStrand( s, std::max( Scalar(0.0), u0 - Scalar(1.0) ), nullptr );
			e1 = EvaluateStrand( s, u0, nullptr );
		} else {
			e0 = EvaluateStrand( s, u0, nullptr );
			e1 = EvaluateStrand( s, u0 + Scalar(1.0), nullptr );
		}
		T = Vector3Ops::mkVector3( e1, e0 );
		if( Vector3Ops::SquaredModulus( T ) < NEARZERO ) {
			T = Vector3( 0, 0, 1 );
		}
	}
	T = Vector3Ops::Normalize( T );

	const Vector3 D = Vector3Ops::Normalize( ri.ray.Dir() );
	const Point3  H = ri.ray.PointAtLength( t );

	// A = the across-width axis: perpendicular to BOTH the view
	// direction and the fibre tangent, which is exactly the axis a
	// ray-facing ribbon spreads its width along.
	Vector3 A = Vector3Ops::Cross( D, T );
	Vector3 Nflat;
	Scalar  h = 0;

	if( Vector3Ops::SquaredModulus( A ) < Scalar(1e-16) ) {
		// GRAZING DEGENERACY (measure zero): the ray runs along the
		// fibre, so "across the width" is undefined.  Pick any axis
		// perpendicular to T, report the fibre centre (h = 0), and let
		// the cylinder rule collapse to the flat normal.  A hair struck
		// exactly end-on has no meaningful h and the Chiang model's
		// h-conditioning is degenerate there regardless.
		const Vector3 helper = ( fabs( T.x ) < Scalar(0.8) ) ? Vector3( 1, 0, 0 ) : Vector3( 0, 1, 0 );
		A = Vector3Ops::Normalize( Vector3Ops::Cross( helper, T ) );
		Nflat = Vector3Ops::Normalize( Vector3Ops::Cross( A, T ) );
		h = 0;
	} else {
		A = Vector3Ops::Normalize( A );
		Nflat = Vector3Ops::Normalize( Vector3Ops::Cross( A, T ) );
		// INVARIANT (construction-forced, not a runtime condition): Nflat
		// already faces the ray origin here, i.e. dot(Nflat, D) < 0,
		// ALWAYS -- no flip is needed.  Derivation: with
		// A = (D x T)/|D x T| and Nflat = (A x T)/|A x T|, the vector
		// triple product identity (X x Y) x Z = Y(X.Z) - X(Y.Z) gives
		// (D x T) x T = T(D.T) - D(T.T) = -(D - (D.T)T) = -D_perp, where
		// D_perp is D's component perpendicular to T.  Since T and D are
		// unit, |D x T| = |D_perp| too, so Nflat = -D_perp / |D_perp|
		// exactly (already unit, before Normalize() above, which is
		// therefore a no-op up to floating point).  Then, using
		// D = D_perp + (D.T)T and D_perp perp T:
		//   dot(Nflat, D) = dot(-D_perp/|D_perp|, D_perp + (D.T)T)
		//                 = -|D_perp|  <=  0
		// with equality only at the |D_perp| == 0 degeneracy the branch
		// above already special-cases.  A naive port of a pbrt-style
		// "flip if facing away" step would therefore never fire here and
		// was removed.

		const Scalar sArcForWidth = ArcFractionAt( s, u );
		const Scalar width = StrandWidthAt( s, sArcForWidth );
		const Vector3 off = Vector3Ops::mkVector3( H, C );
		if( width > 0 ) {
			h = ( Scalar(2.0) * Vector3Ops::Dot( off, A ) ) / width;
		}
		if( h < -1 ) h = -1;
		if( h >  1 ) h =  1;
	}

	// CYLINDER-mode shading normal: the EXACT normal field of a cylinder
	// of the same width, evaluated in the {A, Nflat} cross-section
	// frame.  In that frame the unit circle is parameterised by the
	// across-width coordinate h in [-1, 1] as (h, sqrt(1 - h^2)) -- h
	// along A, sqrt(1-h^2) along Nflat -- and for a circle centred at
	// the origin the outward normal AT a point IS that point, so
	//     Ncyl = Nflat * sqrt(1 - h^2)  +  A * h.
	// A and Nflat are orthonormal and both perpendicular to T, so the
	// result is automatically unit length (h^2 + (1-h^2) = 1) and
	// perpendicular to T, by construction.
	//
	// This DELIBERATELY departs from pbrt-v4's Curve cylinder mode,
	// which instead sweeps the normal by an ANGLE theta = h * pi/2:
	// Ncyl_pbrt = Nflat*cos(theta) + A*sin(theta).  That traces the same
	// unit circle but at a non-uniform rate in h -- it agrees with the
	// exact field only at h in {-1, 0, 1} and is off by up to ~18.9
	// degrees at |h| ~= 0.77 (where cos(theta) departs furthest from
	// sqrt(1-h^2)).  pbrt's own comment only promises "a cylindrical
	// appearance", not the exact field; RISE already has h in hand as a
	// linear cross-section coordinate, so computing the exact field
	// costs one sqrt and no transcendentals, instead of a sin/cos pair.
	const Scalar radial = sqrt( std::max( Scalar(0.0), Scalar(1.0) - h * h ) );
	const Vector3 Ncyl = Nflat * radial + A * h;

	ri.bHit   = true;
	ri.range  = t;
	ri.range2 = t;					// no volume: exit == entry (class comment)
	ri.ptIntersection = H;			// object space; Object::IntersectRay recomputes + transforms

	ri.vNormal      = Ncyl;
	ri.vGeomNormal  = Nflat;
	ri.bGeomNormalOrientedToRay = true;

	// (s, t): s = arc-length fraction root->tip, t = across-width in
	// [0,1] so the BSDF's near-field offset is h = 2t - 1.
	ri.ptCoord  = Point2( ArcFractionAt( s, u ), Scalar(0.5) * ( h + Scalar(1.0) ) );
	ri.ptCoord1 = StrandRootUV( s );
	ri.bHasTexCoord1 = true;

	ri.bShadingTangentFromGeometry = true;
	ri.vShadingTangent  = T;
	ri.bHasShadingTangent = true;

	// `onb` is deliberately NOT written -- matching every other RISE
	// geometry (sphere, cylinder, mesh).  Object::IntersectRay (and
	// CSGObject::IntersectRay, for a CSG-composed fibre) builds it from
	// the world-space normal after transforming, honouring
	// vShadingTangent as the fibre tangent for the ONB's u-axis.
}

void HairGeometry::RayElementIntersection( RayIntersection& ri, const MYOBJ elem,
                                           const bool bHitFrontFaces, const bool bHitBackFaces,
                                           const bool /*bComputeExitInfo*/ ) const
{
	// Exit info is entry info for a zero-thickness ribbon, and the
	// geometric routine already sets range2 = range; the remaining exit
	// fields are filled once, after traversal, in IntersectRay().
	RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
}

bool HairGeometry::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem,
                                                            const bool /*bHitFrontFaces*/, const bool /*bHitBackFaces*/ ) const
{
	Scalar t = 0, u = 0;
	return IntersectSegment( ray, dHowFar, elem, t, u );
}

void HairGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces,
                                 const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( !pSegBVH ) {
		return;
	}

	// Both flags forced true at the BVH call, exactly as a DOUBLE-SIDED
	// triangle mesh does (TriangleMeshGeometryIndexed.cpp:168).  See the
	// class comment for why a ray-facing ribbon has no cullable side.
	(void)bHitFrontFaces; (void)bHitBackFaces;

	const Scalar rangeBefore = ri.range;
	pSegBVH->IntersectRay( ri, true, true );

	// Only OUR hit gets exit info.  `ri` is shared with whatever the
	// caller already found, so a strictly-smaller range is the proof
	// that this groom won.
	if( bComputeExitInfo && ri.bHit && ri.range < rangeBefore ) {
		// ptExit is NOT written here: Object::IntersectRay recomputes it
		// (and re-derives range2 in world space) from range2 alone --
		// see Object.cpp's ptObjExit/ptExit block -- matching the
		// CircularDiskGeometry precedent, which likewise writes only
		// range2 / vNormal2 / vGeomNormal2 and leaves ptExit to the
		// caller.
		ri.vNormal2     = ri.vNormal;
		ri.vGeomNormal2 = ri.vGeomNormal;
		// range2 was already set to range by RayElementIntersection.
	}
}

bool HairGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar,
                                                  const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( !pSegBVH ) {
		return false;
	}
	(void)bHitFrontFaces; (void)bHitBackFaces;		// see IntersectRay
	return pSegBVH->IntersectRay_IntersectionOnly( ray, dHowFar, true, true );
}

//////////////////////////////////////////////////////////////////////
//  Bounds / stubs / serialization
//////////////////////////////////////////////////////////////////////

BoundingBox HairGeometry::GenerateBoundingBox() const
{
	return bbox;
}

void HairGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3( ( bbox.ll.x + bbox.ur.x ) * Scalar(0.5),
	                   ( bbox.ll.y + bbox.ur.y ) * Scalar(0.5),
	                   ( bbox.ll.z + bbox.ur.z ) * Scalar(0.5) );
	radius = Vector3Ops::Magnitude( Vector3Ops::mkVector3( bbox.ur, ptCenter ) );
}

void HairGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// CanBeAreaLight() == false keeps every sampler off this path; this
	// mirrors SDFGeometry's degenerate-field branch in returning
	// something deterministic rather than uninitialised memory should an
	// unguarded caller ever arrive.
	if( point )  { *point = Point3( ( bbox.ll.x + bbox.ur.x ) * Scalar(0.5),
	                                ( bbox.ll.y + bbox.ur.y ) * Scalar(0.5),
	                                ( bbox.ll.z + bbox.ur.z ) * Scalar(0.5) ); }
	if( normal ) { *normal = Vector3( 0, 0, 1 ); }
	if( coord )  { *coord = Point2( prand.x, prand.y ); }
}

void HairGeometry::SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const
{
	buffer.setUInt( elem.strand );
	buffer.setUInt( ( (unsigned int)elem.span << 16 ) |
	                ( (unsigned int)elem.subIndex << 8 ) |
	                  (unsigned int)elem.subDepth );
}

void HairGeometry::DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const
{
	ret.strand = buffer.getUInt();
	const unsigned int packed = buffer.getUInt();
	ret.span     = (uint16_t)( ( packed >> 16 ) & 0xFFFFu );
	ret.subIndex = (uint8_t)( ( packed >> 8 ) & 0xFFu );
	ret.subDepth = (uint8_t)( packed & 0xFFu );
}

size_t HairGeometry::ApproximateMemoryBytes() const
{
	size_t total = 0;
	total += cps.capacity()             * sizeof(float);
	total += cpArcCum.capacity()        * sizeof(float);
	total += strandCPBegin.capacity()   * sizeof(uint32_t);
	total += strandRootWidth.capacity() * sizeof(float);
	total += strandTipWidth.capacity()  * sizeof(float);
	total += strandRootU.capacity()     * sizeof(float);
	total += strandRootV.capacity()     * sizeof(float);
	if( pSegBVH ) {
		total += pSegBVH->numPrims()  * sizeof(HairSegmentRef);
		total += pSegBVH->numNodes()  * sizeof(BVH<HairSegmentRef>::Node);
		total += pSegBVH->numNodes4() * sizeof(BVH<HairSegmentRef>::BVH4Node);
	}
	return total;
}
