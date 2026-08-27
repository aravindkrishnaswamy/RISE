//////////////////////////////////////////////////////////////////////
//
//  HairGeometryTest.cpp - Validation of the runtime curve primitive
//    (src/Library/Geometry/HairGeometry.{h,cpp}), slice C1 of the
//    hair / fur system.
//
//  Six groups:
//
//    1. DIFFERENTIAL INTERSECTION.  ~10k pseudo-random rays against a
//       50-strand groom, production `IntersectRay` versus a SLOW,
//       INDEPENDENT reference intersector written from scratch in this
//       file: no BVH, no adaptive splitting, no shared code -- a dense
//       uniform polyline sweep over every strand with its own
//       Catmull-Rom evaluator and its own ray frame.  Agreement is
//       checked on hit/miss and, for agreed hits, on t.
//
//       The two intersectors CANNOT agree bit-for-bit near a
//       silhouette: production stops splitting once the piece's
//       deviation from its chord is bounded by 5% of the strand width
//       (`kRuntimeFlatnessFraction`), so a ray passing within a few
//       percent of a width of the fibre edge is genuinely on the
//       boundary between the two approximations.  Rays whose reference
//       margin |halfWidth - distance| falls inside `kSilhouetteBand`
//       (a fraction of the local width, printed with the results) are
//       therefore tallied separately.  The gate is on rays OUTSIDE the
//       band; the in-band rate is printed so a regression that widens
//       the ambiguous population is still visible.
//
//    2. OUTPUT VALIDITY.  For every production hit: (s, t) in range;
//       the reported shading tangent is unit and agrees with a
//       numerically-differentiated curve tangent at the nearest curve
//       parameter; the shading normal is unit, perpendicular to the
//       tangent, and satisfies the CYLINDER rule exactly
//       (dot(N_shading, N_geometric) == sqrt(1 - h^2), h = 2t-1 --
//       this is a CHANGE-DETECTOR wired from the same h production
//       reports, not an independent certification of the rule; see the
//       check site for what it does and does not catch);
//       the geometric normal faces the ray; `ptCoord1` decodes to the
//       strand the hit is geometrically nearest to; and both
//       `bShadingTangentFromGeometry` and `bHasShadingTangent` are set.
//       Plus the WHITE-BOX checks that the reflected phantom-endpoint
//       convention makes a 2-control-point strand exactly straight,
//       that bComputeExitInfo reports exit == entry on a real hit (and
//       leaves the exit fields untouched when our hit is not closer
//       than a prior one already in `ri`), and that IntersectRay still
//       hits with both face flags false (the dielectric exit-probe
//       case the class comment defends).
//
//    3. INTERSECTION-ONLY EQUIVALENCE.  `IntersectRay_IntersectionOnly`
//       agrees with `IntersectRay` on hit/miss over the same ray set,
//       and honours dHowFar on both sides of the reported t.
//
//    4. BOUNDS.  Every reported hit point lies inside the geometry's
//       own bounding box (and the bounding sphere), and the box is not
//       absurdly loose.
//
//    5. DEGENERATES.  Per-strand rejection of <2 control points, of a
//       control-point count past the segment record's 16-bit span
//       limit, of non-positive / non-finite widths, and of non-finite
//       control points -- with the surviving strands still building.
//       Plus the empty groom: no hit, no shadow hit, finite box.
//
//    6. SCALE.  100k strands x 8 control points built end to end; build
//       time and resident size are REPORTED, not asserted (the gate is
//       that it completes and that the strand / segment counts are
//       right).
//
//  Every random draw in this file comes from an explicitly-seeded
//  generator, so a failure is reproducible from the binary alone.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/HairGeometry.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Harness
// ============================================================

static int g_checks   = 0;
static int g_failures = 0;

static void Check( const bool ok, const std::string& what )
{
	++g_checks;
	if( !ok ) {
		++g_failures;
		std::cout << "  FAIL: " << what << std::endl;
	}
}

//! Reference-counted RISE objects have protected destructors; this is
//! the local RAII holder (same shape as HairBSDFTest's).
template<class T>
class Ref
{
public:
	explicit Ref( T* q ) : p( q ) { p->addref(); }
	~Ref() { p->release(); }
	T* operator->() const { return p; }
	T& operator*()  const { return *p; }
	T* get()        const { return p; }
private:
	T* p;
	Ref( const Ref& );
	Ref& operator=( const Ref& );
};

//! Master seed.  `RandomNumberGenerator`'s default argument is rand(),
//! i.e. libc's global state -- an unseeded construction is only
//! accidentally reproducible.
static const unsigned int kSeed = 20260826u;

// ============================================================
//  Group 1/2 reference: a completely independent curve evaluator
//  and intersector.  Nothing below calls into HairGeometry except
//  where a test explicitly compares against it.
// ============================================================

//! One strand, as the reference keeps it.
struct RefStrand
{
	std::vector<Point3>	cp;
	double				rootWidth;
	double				tipWidth;
	Point2				rootUV;

	// Derived at setup:
	std::vector<double>	spanArcCum;		///< arc length at each control point (size cp.size())
	double				arcTotal;
	Point3				bbMin, bbMax;	///< strand bounds, padded by max half width
};

//! Reference Catmull-Rom evaluation, written independently of the
//! production coefficient routine.  Uses the same documented
//! convention: uniform Catmull-Rom, reflected phantom endpoints.
//! Deliberately expressed in the tangent (Hermite) form rather than
//! the production power-basis form so an algebra slip in one does not
//! reproduce itself in the other.
static Point3 RefCP( const RefStrand& s, int i )
{
	const int n = (int)s.cp.size();
	if( i < 0 ) {
		return Point3( 2*s.cp[0].x - s.cp[1].x, 2*s.cp[0].y - s.cp[1].y, 2*s.cp[0].z - s.cp[1].z );
	}
	if( i > n-1 ) {
		return Point3( 2*s.cp[n-1].x - s.cp[n-2].x, 2*s.cp[n-1].y - s.cp[n-2].y, 2*s.cp[n-1].z - s.cp[n-2].z );
	}
	return s.cp[i];
}

//! Evaluates the strand at GLOBAL parameter u in [0, numSpans].
//! `deriv` (optional) receives dP/du.
static Point3 RefEval( const RefStrand& s, double u, Vector3* deriv )
{
	const int nSpans = (int)s.cp.size() - 1;
	if( u < 0 ) u = 0;
	if( u > (double)nSpans ) u = (double)nSpans;
	int span = (int)floor( u );
	if( span >= nSpans ) span = nSpans - 1;
	const double w = u - (double)span;

	const Point3 p0 = RefCP( s, span - 1 );
	const Point3 p1 = RefCP( s, span     );
	const Point3 p2 = RefCP( s, span + 1 );
	const Point3 p3 = RefCP( s, span + 2 );

	// Hermite form: endpoints p1, p2; tangents m1 = (p2-p0)/2, m2 = (p3-p1)/2.
	const Vector3 m1( ( p2.x - p0.x )*0.5, ( p2.y - p0.y )*0.5, ( p2.z - p0.z )*0.5 );
	const Vector3 m2( ( p3.x - p1.x )*0.5, ( p3.y - p1.y )*0.5, ( p3.z - p1.z )*0.5 );

	const double w2 = w*w, w3 = w2*w;
	const double h00 =  2*w3 - 3*w2 + 1;
	const double h10 =    w3 - 2*w2 + w;
	const double h01 = -2*w3 + 3*w2;
	const double h11 =    w3 -   w2;

	if( deriv ) {
		const double d00 =  6*w2 - 6*w;
		const double d10 =  3*w2 - 4*w + 1;
		const double d01 = -6*w2 + 6*w;
		const double d11 =  3*w2 - 2*w;
		*deriv = Vector3(
			d00*p1.x + d10*m1.x + d01*p2.x + d11*m2.x,
			d00*p1.y + d10*m1.y + d01*p2.y + d11*m2.y,
			d00*p1.z + d10*m1.z + d01*p2.z + d11*m2.z );
	}

	return Point3(
		h00*p1.x + h10*m1.x + h01*p2.x + h11*m2.x,
		h00*p1.y + h10*m1.y + h01*p2.y + h11*m2.y,
		h00*p1.z + h10*m1.z + h01*p2.z + h11*m2.z );
}

//! Samples per span used to build the reference's arc-length table.
//! Must MATCH HairGeometry::kArcSamplesPerSpan, because the
//! arc-length parameterisation is a documented CONVENTION (per-span
//! cumulative from a 16-sample polyline, linear in the span parameter
//! inside a span), not a quantity under test.  Reproduced here from
//! that contract rather than read from the production object.
static const unsigned int kRefArcSamplesPerSpan = 16;

static void RefPrepare( RefStrand& s )
{
	const int nSpans = (int)s.cp.size() - 1;
	s.spanArcCum.assign( s.cp.size(), 0.0 );
	double running = 0;
	for( int j = 0; j < nSpans; ++j ) {
		Point3 prev = RefEval( s, (double)j, 0 );
		for( unsigned int k = 1; k <= kRefArcSamplesPerSpan; ++k ) {
			const Point3 cur = RefEval( s, (double)j + (double)k / (double)kRefArcSamplesPerSpan, 0 );
			running += Point3Ops::Distance( cur, prev );
			prev = cur;
		}
		s.spanArcCum[j+1] = running;
	}
	s.arcTotal = running;

	// Strand bounds from a dense sweep, padded by the max half width.
	const double pad = std::max( s.rootWidth, s.tipWidth ) * 0.5;
	s.bbMin = Point3(  1e300,  1e300,  1e300 );
	s.bbMax = Point3( -1e300, -1e300, -1e300 );
	const int N = nSpans * 64;
	for( int i = 0; i <= N; ++i ) {
		const Point3 p = RefEval( s, (double)nSpans * (double)i / (double)N, 0 );
		s.bbMin.x = std::min( s.bbMin.x, p.x - pad );  s.bbMax.x = std::max( s.bbMax.x, p.x + pad );
		s.bbMin.y = std::min( s.bbMin.y, p.y - pad );  s.bbMax.y = std::max( s.bbMax.y, p.y + pad );
		s.bbMin.z = std::min( s.bbMin.z, p.z - pad );  s.bbMax.z = std::max( s.bbMax.z, p.z + pad );
	}
}

static double RefArcFraction( const RefStrand& s, double u )
{
	if( !( s.arcTotal > 0 ) ) return 0;
	const int nSpans = (int)s.cp.size() - 1;
	if( u < 0 ) u = 0;
	if( u > (double)nSpans ) u = (double)nSpans;
	int span = (int)floor( u );
	if( span >= nSpans ) span = nSpans - 1;
	const double w = u - (double)span;
	const double a0 = s.spanArcCum[span];
	const double a1 = s.spanArcCum[span+1];
	double f = ( a0 + w * ( a1 - a0 ) ) / s.arcTotal;
	if( f < 0 ) f = 0;
	if( f > 1 ) f = 1;
	return f;
}

static double RefWidth( const RefStrand& s, double u )
{
	const double f = RefArcFraction( s, u );
	return s.rootWidth + f * ( s.tipWidth - s.rootWidth );
}

//! Slab test against the strand's padded bounds -- a cheap reject so
//! the brute-force sweep stays inside the runtime budget.  Written
//! plainly here rather than reusing RISE's RayBoxIntersection so the
//! reference path shares nothing with production.
static bool RefBoxHit( const Point3& o, const Vector3& d, const Point3& lo, const Point3& hi, double tFar )
{
	double t0 = 0, t1 = tFar;
	const double od[3] = { o.x, o.y, o.z };
	const double dd[3] = { d.x, d.y, d.z };
	const double lod[3] = { lo.x, lo.y, lo.z };
	const double hid[3] = { hi.x, hi.y, hi.z };
	for( int k = 0; k < 3; ++k ) {
		if( fabs( dd[k] ) < 1e-30 ) {
			if( od[k] < lod[k] || od[k] > hid[k] ) return false;
			continue;
		}
		double ta = ( lod[k] - od[k] ) / dd[k];
		double tb = ( hid[k] - od[k] ) / dd[k];
		if( ta > tb ) std::swap( ta, tb );
		t0 = std::max( t0, ta );
		t1 = std::min( t1, tb );
		if( t0 > t1 ) return false;
	}
	return true;
}

struct RefHit
{
	bool	hit;
	double	t;
	int		strand;
	double	u;
	//! halfWidth - distance at the accepted crossing (>= 0 on a hit).
	//! On a MISS, the smallest (distance - halfWidth) seen over all
	//! candidates that passed the depth test, i.e. how close the ray
	//! came to grazing a fibre.  This is what classifies a ray as
	//! silhouette-ambiguous.
	double	margin;
	//! The local width at the classifying point, so the band can be
	//! expressed as a fraction of the width rather than an absolute.
	double	widthAtMargin;

	RefHit() : hit( false ), t( 1e300 ), strand( -1 ), u( 0 ), margin( 1e300 ), widthAtMargin( 1 ) {}
};

//! Samples per span in the brute-force sweep.  The polyline's own
//! deviation from the curve at this density is orders of magnitude
//! below a strand width for the grooms built here, so it does not
//! contribute measurably to the hit/miss tallies.
//!
//! 384 rather than a cheaper 96 because of the "hits ahead of the
//! fibre" gate below, which needs the reference to locate the fibre
//! ENTRY, not merely to agree on hit/miss.  The reference's candidates
//! are the closest-approach points of its sub-segments, so it can
//! place the entry LATE by roughly one sub-segment's projection onto
//! the ray.  Measured on this groom: at 96/span that is up to 1.3e-2
//! of a local width (which trips the gate); at 384/span it drops to
//! 1.5e-6.  Sharpening the reference is the right move here rather
//! than widening the gate -- the gate is what stands between this
//! suite and a phantom-surface regression.
static const int kRefSamplesPerSpan = 384;

static RefHit RefIntersect( const std::vector<RefStrand>& strands, const Ray& ray, double tFar )
{
	RefHit best;

	const Vector3 d = ray.Dir();
	const double dirLen = Vector3Ops::Magnitude( d );
	if( !( dirLen > 0 ) ) return best;
	const Vector3 ez = d * ( 1.0 / dirLen );

	// Independent orthonormal frame: pick the helper axis as the one
	// with the SMALLEST component of ez (production picks by a 0.8
	// threshold on ez.x), so the two frames genuinely differ.
	Vector3 helper( 0, 0, 0 );
	if( fabs( ez.x ) <= fabs( ez.y ) && fabs( ez.x ) <= fabs( ez.z ) )      helper = Vector3( 1, 0, 0 );
	else if( fabs( ez.y ) <= fabs( ez.z ) )                                  helper = Vector3( 0, 1, 0 );
	else                                                                     helper = Vector3( 0, 0, 1 );
	const Vector3 ex = Vector3Ops::Normalize( Vector3Ops::Cross( helper, ez ) );
	const Vector3 ey = Vector3Ops::Cross( ez, ex );

	const double zFar = ( tFar >= 1e300 ) ? 1e300 : tFar * dirLen;

	for( size_t si = 0; si < strands.size(); ++si )
	{
		const RefStrand& s = strands[si];
		if( !RefBoxHit( ray.origin, d, s.bbMin, s.bbMax, tFar ) ) continue;

		const int nSpans = (int)s.cp.size() - 1;
		const int N = nSpans * kRefSamplesPerSpan;

		// Project the dense polyline into the ray frame once.
		std::vector<double> px( N+1 ), py( N+1 ), pz( N+1 ), pu( N+1 );
		for( int i = 0; i <= N; ++i ) {
			const double u = (double)nSpans * (double)i / (double)N;
			const Point3 p = RefEval( s, u, 0 );
			const Vector3 rel( p.x - ray.origin.x, p.y - ray.origin.y, p.z - ray.origin.z );
			px[i] = Vector3Ops::Dot( rel, ex );
			py[i] = Vector3Ops::Dot( rel, ey );
			pz[i] = Vector3Ops::Dot( rel, ez );
			pu[i] = u;
		}

		for( int i = 0; i < N; ++i )
		{
			const double sx = px[i+1] - px[i];
			const double sy = py[i+1] - py[i];
			const double den = sx*sx + sy*sy;
			double w = 0;
			if( den > 1e-24 ) {
				w = ( -px[i]*sx - py[i]*sy ) / den;
				if( w < 0 ) w = 0;
				if( w > 1 ) w = 1;
			}
			const double u = pu[i] + w * ( pu[i+1] - pu[i] );

			// Evaluate the EXACT curve at the clamped parameter (the
			// polyline is only used to locate it), matching the
			// documented base-case convention.
			const Point3 pc = RefEval( s, u, 0 );
			const Vector3 rel( pc.x - ray.origin.x, pc.y - ray.origin.y, pc.z - ray.origin.z );
			const double cx = Vector3Ops::Dot( rel, ex );
			const double cy = Vector3Ops::Dot( rel, ey );
			const double cz = Vector3Ops::Dot( rel, ez );

			if( cz < 0 || cz > zFar ) continue;

			const double width = RefWidth( s, u );
			const double halfW = width * 0.5;
			const double dist  = sqrt( cx*cx + cy*cy );

			if( dist <= halfW ) {
				const double t = cz / dirLen;
				if( t > 1e-12 && t < tFar && t < best.t ) {
					best.hit           = true;
					best.t             = t;
					best.strand        = (int)si;
					best.u             = u;
					best.margin        = halfW - dist;
					best.widthAtMargin = width;
				}
			} else if( !best.hit ) {
				const double gap = dist - halfW;
				if( gap < best.margin ) {
					best.margin        = gap;
					best.widthAtMargin = width;
				}
			}
		}
	}
	return best;
}

// ============================================================
//  Groom construction (shared by groups 1-4)
// ============================================================

//! MUTATION-FRAGILITY NOTE: a mutation campaign against this file's
//! groom/ray counts (kNumStrands here, kNumRays below) was 10/10 caught
//! at these values.  Two of the ten corruptions (an AABB-shrink bug and
//! a coarsened runtime flatness target) were caught ONLY by the
//! exact-identity gates (outsideFibre/earlyHit/wrongCrossing,
//! badCylinderRule, etc.) firing on just 1-2 rays out of the full set --
//! not by any statistical/percentage gate.  Do not reduce kNumRays or
//! the strand count without re-running a mutation pass; a smaller
//! sample could silently drop below the population needed to hit those
//! 1-2-ray regressions.
static const int kNumStrands = 50;

//! Builds the test groom.  Strands vary in curvature (from nearly
//! straight to a full half-turn of curl), in length, and in root/tip
//! width ratio, so neither intersector is exercised on a single
//! regime.  Each strand's root UV encodes its index as
//! ((i + 0.5)/N, 0.5), which group 2 decodes to check `ptCoord1`.
static void BuildGroom( std::vector<RefStrand>& out, std::vector<HairGeometry::StrandDesc>& descs )
{
	RandomNumberGenerator rng( kSeed );

	for( int i = 0; i < kNumStrands; ++i )
	{
		RefStrand s;
		const double rx = rng.CanonicalRandom()*2.0 - 1.0;
		const double ry = rng.CanonicalRandom()*2.0 - 1.0;
		const double len   = 0.8 + rng.CanonicalRandom()*1.2;
		const double curl  = rng.CanonicalRandom()*rng.CanonicalRandom();	// biased toward straight
		const double phase = rng.CanonicalRandom()*6.2831853;
		const double turns = 0.15 + rng.CanonicalRandom()*0.85;

		const int nCP = 6;
		for( int k = 0; k < nCP; ++k ) {
			const double f = (double)k / (double)(nCP-1);
			const double a = phase + turns*6.2831853*f;
			s.cp.push_back( Point3(
				rx + curl*0.35*f*cos(a),
				ry + curl*0.35*f*sin(a),
				len*f ) );
		}
		s.rootWidth = 0.02 + rng.CanonicalRandom()*0.04;
		s.tipWidth  = s.rootWidth * ( 0.2 + rng.CanonicalRandom()*0.8 );
		s.rootUV    = Point2( ( (double)i + 0.5 ) / (double)kNumStrands, 0.5 );

		RefPrepare( s );
		out.push_back( s );

		HairGeometry::StrandDesc d;
		d.controlPoints = s.cp;
		d.rootWidth     = s.rootWidth;
		d.tipWidth      = s.tipWidth;
		d.rootUV        = s.rootUV;
		descs.push_back( d );
	}
}

//! Generates the shared ray set: origins on a sphere enclosing the
//! groom, aimed at random points inside a slightly-inflated groom box,
//! so a healthy fraction both hit and miss.
static void BuildRays( std::vector<Ray>& rays, const int count, const Point3& lo, const Point3& hi )
{
	RandomNumberGenerator rng( kSeed + 7u );
	const Point3 ctr( (lo.x+hi.x)*0.5, (lo.y+hi.y)*0.5, (lo.z+hi.z)*0.5 );
	const double radius = Point3Ops::Distance( hi, ctr ) * 2.5 + 1.0;

	for( int i = 0; i < count; ++i ) {
		const double z   = rng.CanonicalRandom()*2.0 - 1.0;
		const double phi = rng.CanonicalRandom()*6.2831853;
		const double r   = sqrt( std::max( 0.0, 1.0 - z*z ) );
		const Point3 o( ctr.x + radius*r*cos(phi), ctr.y + radius*r*sin(phi), ctr.z + radius*z );

		// Inflate the target box by 15% so a good share of rays graze
		// or miss entirely.
		const double ix = ( hi.x - lo.x )*0.15, iy = ( hi.y - lo.y )*0.15, iz = ( hi.z - lo.z )*0.15;
		const Point3 tgt(
			lo.x - ix + rng.CanonicalRandom()*( hi.x - lo.x + 2*ix ),
			lo.y - iy + rng.CanonicalRandom()*( hi.y - lo.y + 2*iy ),
			lo.z - iz + rng.CanonicalRandom()*( hi.z - lo.z + 2*iz ) );

		rays.push_back( Ray( o, Vector3Ops::Normalize( Vector3Ops::mkVector3( tgt, o ) ) ) );
	}
}

// ============================================================
//  Shared: nearest point on the groom (used by groups 1 and 2)
// ============================================================

//! Finds the (strand, u) whose curve point is nearest `p`, by a dense
//! scan followed by a golden-section refinement.  Independent of
//! anything the geometry reports.
static void NearestOnGroom( const std::vector<RefStrand>& strands, const Point3& p,
                            int& outStrand, double& outU, double& outDist )
{
	outStrand = -1; outU = 0; outDist = 1e300;
	for( size_t si = 0; si < strands.size(); ++si ) {
		const RefStrand& s = strands[si];
		// Cheap reject: `p` is a reported HIT point, so the strand that
		// owns it must have `p` inside its (already width-padded)
		// bounds.  Keeps the dense scan off the other 48 strands.
		if( p.x < s.bbMin.x || p.x > s.bbMax.x ||
		    p.y < s.bbMin.y || p.y > s.bbMax.y ||
		    p.z < s.bbMin.z || p.z > s.bbMax.z ) continue;
		const int nSpans = (int)s.cp.size() - 1;
		const int N = nSpans * 256;
		int bestI = 0; double bestD = 1e300;
		for( int i = 0; i <= N; ++i ) {
			const double u = (double)nSpans * (double)i / (double)N;
			const double d = Point3Ops::Distance( RefEval( s, u, 0 ), p );
			if( d < bestD ) { bestD = d; bestI = i; }
		}
		// Golden-section refine inside the neighbouring cells.
		double lo = (double)nSpans * (double)std::max( 0, bestI-1 ) / (double)N;
		double hi = (double)nSpans * (double)std::min( N, bestI+1 ) / (double)N;
		const double gr = 0.6180339887498949;
		double a = hi - gr*(hi-lo), b = lo + gr*(hi-lo);
		double fa = Point3Ops::Distance( RefEval( s, a, 0 ), p );
		double fb = Point3Ops::Distance( RefEval( s, b, 0 ), p );
		for( int it = 0; it < 60; ++it ) {
			if( fa < fb ) { hi = b; b = a; fb = fa; a = hi - gr*(hi-lo); fa = Point3Ops::Distance( RefEval( s, a, 0 ), p ); }
			else          { lo = a; a = b; fa = fb; b = lo + gr*(hi-lo); fb = Point3Ops::Distance( RefEval( s, b, 0 ), p ); }
		}
		const double u = 0.5*(lo+hi);
		const double d = Point3Ops::Distance( RefEval( s, u, 0 ), p );
		if( d < outDist ) { outDist = d; outStrand = (int)si; outU = u; }
	}
}

// ============================================================
//  Group 1 -- differential intersection
// ============================================================

//! Silhouette ambiguity band, as a fraction of the LOCAL strand width.
//! Production stops splitting at a chord-deviation bound of 5% of the
//! width, so agreement inside a band of that order is not something
//! either implementation promises.  The value here is 2% -- chosen
//! after measuring the disagreement distribution (see the printed
//! by-band table): outside it the two intersectors agree on
//! essentially every ray.
static const double kSilhouetteBand = 0.02;

//! Slack on the "production must not report a surface IN FRONT of the
//! fibre" gate, expressed as a fraction of the LOCAL STRAND WIDTH --
//! the only scale the question has.  The reference locates the fibre
//! entry only to its own sampling granularity (see
//! `kRefSamplesPerSpan`), so it can place the entry a hair LATE.
//! Measured worst overshoot on this groom at 384 samples/span is
//! 1.5e-6 of a width, so 1e-3 leaves a ~670x margin while still being
//! three orders of magnitude below the displacement a genuine
//! phantom-surface bug produces (the red-proof mutation -- inflating
//! the accepted half width by 2% -- moved hits by up to 68 widths and
//! tripped both this gate and the fibre-membership one).
static const double kEarlyHitSlackWidths = 1e-3;

static const int kNumRays = 10000;

//////////////////////////////////////////////////////////////////////
//
//  WHY t IS NOT COMPARED DIRECTLY (measured, 2026-08-26).
//
//  Both intersectors locate a hit by CLOSEST APPROACH within a piece
//  of curve, so the reported t depends on how finely the curve is
//  chopped: a coarse piece reports the point of closest approach --
//  which is INSIDE the swept fibre, roughly its middle -- while an
//  arbitrarily fine chopping converges instead on the point where the
//  ray first ENTERS the fibre.  The reference here is chopped ~96x per
//  span; production chops adaptively (typically far coarser).  The two
//  therefore differ along the ray by up to one chord of the fibre.
//
//  This was verified rather than assumed: tightening production's
//  runtime flatness target 25x (0.05 -> 0.002 of a width) moved the
//  observed max |dt| only from 9.8e-2 to 6.2e-2 and CHANGED NO
//  hit/miss verdict -- i.e. the gap is the convention, not an accuracy
//  defect that more splitting would close.  Gating on |dt| would
//  therefore be gating on the reference's sampling rate.
//
//  What IS certified instead, and is convention-free:
//    (a) hit/miss agreement, ray for ray;
//    (b) the production hit point lies INSIDE the swept fibre -- no
//        phantom surface;
//    (c) production never reports a hit IN FRONT of where the fibre
//        actually starts (t_prod >= t_ref - slack);
//    (d) production's hit is on the SAME crossing -- t_prod - t_ref is
//        within the longest chord a ray at that incidence can cut
//        through a fibre of that width, 2 r / sin(angle to the fibre
//        axis), with a 2x margin for the reference's own granularity.
//  |dt| is still printed, as a regression tripwire.
//
//////////////////////////////////////////////////////////////////////

struct DiffStats
{
	int rays, refHits, prodHits;
	int agree, disagreeInBand, disagreeOutBand;
	int outsideFibre, earlyHit, wrongCrossing;
	double maxAbsDeltaT, minSignedDeltaT, worstFibreExcess;
	// disagreement counts at a ladder of bands, for the printed table
	int disagreeAtBand[5];
};

static void RunDifferential( const HairGeometry& geom, const std::vector<RefStrand>& refStrands,
                             const std::vector<Ray>& rays, const double diag )
{
	std::cout << "=== 1. Differential intersection (production vs independent brute force) ===" << std::endl;

	const double bandLadder[5] = { 0.0, 0.001, 0.005, 0.02, 0.05 };

	DiffStats st;
	st.rays = st.refHits = st.prodHits = 0;
	st.agree = st.disagreeInBand = st.disagreeOutBand = 0;
	st.outsideFibre = st.earlyHit = st.wrongCrossing = 0;
	st.maxAbsDeltaT = 0; st.minSignedDeltaT = 0; st.worstFibreExcess = -1e300;
	double worstEarlyWidths = 0;
	for( int k = 0; k < 5; ++k ) st.disagreeAtBand[k] = 0;

	for( size_t i = 0; i < rays.size(); ++i )
	{
		const Ray& ray = rays[i];

		RayIntersectionGeometric ri( ray, nullRasterizerState );
		geom.IntersectRay( ri, true, true, false );

		const RefHit rh = RefIntersect( refStrands, ray, 1e300 );

		++st.rays;
		if( rh.hit )    ++st.refHits;
		if( ri.bHit )   ++st.prodHits;

		const double relMargin = ( rh.widthAtMargin > 0 ) ? ( rh.margin / rh.widthAtMargin ) : 1e300;

		if( rh.hit == (bool)ri.bHit )
		{
			++st.agree;
			if( rh.hit ) {
				const double dtSigned = ri.range - rh.t;
				st.maxAbsDeltaT    = std::max( st.maxAbsDeltaT, fabs( dtSigned ) );
				st.minSignedDeltaT = std::min( st.minSignedDeltaT, dtSigned );

				// (b) the reported point is inside the swept fibre.
				int sIdx = -1; double uNear = 0, dNear = 0;
				NearestOnGroom( refStrands, ri.ptIntersection, sIdx, uNear, dNear );
				if( sIdx < 0 ) {
					++st.outsideFibre;
				} else {
					const double halfW  = RefWidth( refStrands[sIdx], uNear ) * 0.5;
					const double excess = ( dNear - halfW ) / std::max( halfW, 1e-12 );
					st.worstFibreExcess = std::max( st.worstFibreExcess, excess );
					if( excess > 1e-6 ) ++st.outsideFibre;
				}

				{
					Vector3 tangent;
					RefEval( refStrands[rh.strand], rh.u, &tangent );
					tangent = Vector3Ops::Normalize( tangent );
					const double sinAng = Vector3Ops::Magnitude(
						Vector3Ops::Cross( Vector3Ops::Normalize( ray.Dir() ), tangent ) );
					const double width  = RefWidth( refStrands[rh.strand], rh.u );

					// (c) no hit in front of where the fibre actually starts.
					if( dtSigned < 0 ) {
						worstEarlyWidths = std::max( worstEarlyWidths, -dtSigned / width );
					}
					if( dtSigned < -kEarlyHitSlackWidths * width ) ++st.earlyHit;

					// (d) same crossing: bound by the longest possible
					//     chord through a fibre of this width at this
					//     incidence, 2r/sin(angle), with a 2x margin.
					const double chord = width / std::max( sinAng, 1e-9 );
					if( dtSigned > 2.0 * chord + kEarlyHitSlackWidths * width ) ++st.wrongCrossing;
				}
			}
		}
		else
		{
			// Tally which bands this disagreement falls OUTSIDE of.
			for( int k = 0; k < 5; ++k ) {
				if( relMargin > bandLadder[k] ) ++st.disagreeAtBand[k];
			}
			if( relMargin <= kSilhouetteBand ) ++st.disagreeInBand;
			else                                ++st.disagreeOutBand;
		}
	}

	const double pctOut = 100.0 * (double)st.disagreeOutBand / (double)st.rays;
	const double pctIn  = 100.0 * (double)st.disagreeInBand  / (double)st.rays;

	std::cout << "  rays                       : " << st.rays << std::endl;
	std::cout << "  reference hits             : " << st.refHits
	          << " (" << std::fixed << std::setprecision(2)
	          << 100.0*(double)st.refHits/(double)st.rays << "%)" << std::endl;
	std::cout << "  production hits            : " << st.prodHits << std::endl;
	std::cout << "  hit/miss agreements        : " << st.agree << std::endl;
	std::cout << "  disagreements, in  band    : " << st.disagreeInBand  << " (" << pctIn  << "%)" << std::endl;
	std::cout << "  disagreements, out of band : " << st.disagreeOutBand << " (" << pctOut << "%)" << std::endl;
	std::cout << "  disagreement rate vs band (band = fraction of local width):" << std::endl;
	for( int k = 0; k < 5; ++k ) {
		std::cout << "      band " << std::setw(6) << std::setprecision(3) << bandLadder[k]
		          << "  ->  " << std::setw(5) << st.disagreeAtBand[k]
		          << "  (" << std::setprecision(3)
		          << 100.0*(double)st.disagreeAtBand[k]/(double)st.rays << "%)" << std::endl;
	}
	std::cout << "  max |dt| on agreed hits    : " << std::scientific << std::setprecision(3)
	          << st.maxAbsDeltaT << "  (informational -- see the note above; bbox diagonal "
	          << diag << ")" << std::endl;
	std::cout << "  most negative dt           : " << st.minSignedDeltaT
	          << "  (= " << worstEarlyWidths << " local widths ahead of the fibre start)" << std::endl;
	std::cout << "  worst fibre excess         : " << st.worstFibreExcess
	          << "  (hit distance beyond the local half width, relative; negative = strictly inside)"
	          << std::defaultfloat << std::endl;
	std::cout << "  hits outside the fibre     : " << st.outsideFibre << std::endl;
	std::cout << "  hits ahead of the fibre    : " << st.earlyHit << std::endl;
	std::cout << "  hits on a different cross. : " << st.wrongCrossing << std::endl;

	Check( st.refHits  > 1000, "reference intersector produced a meaningful hit population (>1000)" );
	Check( st.prodHits > 1000, "production intersector produced a meaningful hit population (>1000)" );
	Check( st.disagreeOutBand == 0, "zero hit/miss disagreements outside the silhouette band" );
	Check( st.outsideFibre  == 0, "every reported hit point lies inside the swept fibre" );
	Check( st.earlyHit      == 0, "no hit is reported in front of where the fibre actually starts" );
	Check( st.wrongCrossing == 0, "every hit is on the same fibre crossing the reference found" );
}

// ============================================================
//  Group 2 -- output validity
// ============================================================

static void RunOutputValidity( const HairGeometry& geom, const std::vector<RefStrand>& refStrands,
                               const std::vector<Ray>& rays )
{
	std::cout << "=== 2. Output validity ===" << std::endl;

	int hits = 0;
	int badRange = 0, badTangentUnit = 0, badTangentAngle = 0, badTangentNear = 0;
	int badNormalUnit = 0, badCylinderRule = 0, badPerp = 0, badFacing = 0;
	int badUV1 = 0, badFlags = 0, badArcPosition = 0;
	double worstTangentDeg = 0, worstTangentNearDeg = 0, worstCylinder = 0, worstArcExcess = -1e300;
	double worstArcExcessRatio = 0;	// worst (measured excess / derived quantisation threshold); see badArcPosition below

	// Only sample a subset for the (expensive) nearest-point search.
	const size_t stride = 20;

	for( size_t i = 0; i < rays.size(); i += stride )
	{
		RayIntersectionGeometric ri( rays[i], nullRasterizerState );
		geom.IntersectRay( ri, true, true, false );
		if( !ri.bHit ) continue;
		++hits;

		// -- (s, t) in range
		if( !( ri.ptCoord.x >= 0 && ri.ptCoord.x <= 1 && ri.ptCoord.y >= 0 && ri.ptCoord.y <= 1 ) ) ++badRange;

		// -- flags
		if( !ri.bShadingTangentFromGeometry || !ri.bHasShadingTangent || !ri.bHasTexCoord1 ) ++badFlags;

		// -- identify the strand geometrically
		const Point3 H = ri.ptIntersection;
		int   sIdx = -1; double u = 0, dist = 0;
		NearestOnGroom( refStrands, H, sIdx, u, dist );

		// -- ptCoord1 must be that strand's root UV
		const int decoded = (int)floor( ri.ptCoord1.x * (double)kNumStrands );
		if( decoded != sIdx || fabs( ri.ptCoord1.y - 0.5 ) > 1e-9 ) ++badUV1;

		// -- tangent unit length: CONSTRUCTION-FORCED (T is explicitly
		//    Vector3Ops::Normalize()'d in RayElementIntersection before
		//    it is reported), not an independent property under test.
		//    What this catches is a refactor that decouples the frame
		//    (e.g. drops that Normalize() call, or reports a raw
		//    fallback vector on some path), not a numerical-accuracy
		//    regression in the tangent's DIRECTION -- that is what the
		//    two angle checks below are for.
		const double tanLen = Vector3Ops::Magnitude( ri.vShadingTangent );
		if( fabs( tanLen - 1.0 ) > 1e-9 ) ++badTangentUnit;

		// The PRIMARY, exact tangent check inverts the reported s back
		// to a curve parameter and differentiates there.  s is a
		// monotone function of u (arc length only increases), so the
		// inversion is a plain bisection on the reference's own arc
		// table -- no production code involved.  This ties the three
		// reported quantities (s, hit position, tangent) to each other
		// exactly, instead of comparing against a parameter recovered
		// by a nearest-point search whose own error would dominate.
		double uFromS = 0;
		{
			const RefStrand& rs = refStrands[sIdx];
			double lo = 0, hi = (double)( rs.cp.size() - 1 );
			for( int it = 0; it < 80; ++it ) {
				const double mid = 0.5*( lo + hi );
				if( RefArcFraction( rs, mid ) < ri.ptCoord.x ) lo = mid; else hi = mid;
			}
			uFromS = 0.5*( lo + hi );

			// (i) the curve point at that parameter must be within the
			//     local half width of the reported hit position --
			//     i.e. s really does name where the hit is.
			//
			//     The gate is an HONEST threshold derived from
			//     production's own storage, not a magic 1e-6: `s` is
			//     read back from `cpArcCum`, which HairGeometry stores
			//     in FLOAT (see HairGeometry.h) while everything on this
			//     reference side is double.  Float carries a relative
			//     quantisation of ~2^-23 ~= 1.2e-7, which maps to an
			//     ABSOLUTE position error of ~1.2e-7 * strandArcLength
			//     along the curve; expressed, like `excess`, as a
			//     fraction of the LOCAL half width, the honest bound is
			//     1.2e-7 * strandArcLength / halfW.
			const Point3 C = RefEval( rs, uFromS, 0 );
			const double halfW  = RefWidth( rs, uFromS ) * 0.5;
			const double excess = ( Point3Ops::Distance( C, H ) - halfW ) / std::max( halfW, 1e-12 );
			const double sQuantThreshold = 1.2e-7 * rs.arcTotal / std::max( halfW, 1e-12 );
			worstArcExcess = std::max( worstArcExcess, excess );
			worstArcExcessRatio = std::max( worstArcExcessRatio, excess / std::max( sQuantThreshold, 1e-300 ) );
			if( excess > sQuantThreshold ) ++badArcPosition;

			// (ii) the reported tangent must be the curve derivative
			//      at that same parameter.
			const double du = 1e-7;
			const Point3 pA = RefEval( rs, uFromS - du, 0 );
			const Point3 pB = RefEval( rs, uFromS + du, 0 );
			const Vector3 numeric = Vector3Ops::Normalize( Vector3Ops::mkVector3( pB, pA ) );
			double c = Vector3Ops::Dot( numeric, ri.vShadingTangent );
			if( c >  1 ) c =  1;
			if( c < -1 ) c = -1;
			const double deg = acos( c ) * 180.0 / 3.14159265358979323846;
			worstTangentDeg = std::max( worstTangentDeg, deg );
			// Tightened from 0.5 deg: measured worst on this groom is
			// 2.1e-5 deg, so 0.05 still leaves ~2400x headroom over the
			// measured value while giving up ~10x of the original
			// (unjustifiably loose) slack -- room for platform/compiler
			// variance in the trig transcendentals without masking a
			// real regression.
			if( deg > 0.05 ) ++badTangentAngle;
		}

		// SECONDARY, loose cross-check against the parameter recovered
		// by a nearest-point search.  This one carries its own method
		// error -- the nearest point to H is not exactly the curve
		// parameter the intersector used, because H sits off-axis by up
		// to a half width -- so it is gated loosely and reported.  It
		// exists to catch a tangent that is coherent-but-wrong (e.g.
		// taken from the wrong strand), which check (ii) alone could
		// not see if s were wrong in the same way.
		{
			const double du = 1e-6;
			const Point3 pA = RefEval( refStrands[sIdx], u - du, 0 );
			const Point3 pB = RefEval( refStrands[sIdx], u + du, 0 );
			const Vector3 numeric = Vector3Ops::Normalize( Vector3Ops::mkVector3( pB, pA ) );
			double c = Vector3Ops::Dot( numeric, ri.vShadingTangent );
			if( c >  1 ) c =  1;
			if( c < -1 ) c = -1;
			const double deg = acos( c ) * 180.0 / 3.14159265358979323846;
			worstTangentNearDeg = std::max( worstTangentNearDeg, deg );
			if( deg > 10.0 ) ++badTangentNear;
		}

		// -- normals
		if( fabs( Vector3Ops::Magnitude( ri.vNormal ) - 1.0 ) > 1e-9 ) ++badNormalUnit;
		if( fabs( Vector3Ops::Magnitude( ri.vGeomNormal ) - 1.0 ) > 1e-9 ) ++badNormalUnit;

		// Cylinder rule: dot(N_shading, N_geometric) == sqrt(1 - h^2),
		// h = 2t-1.  NOTE: this is a CHANGE-DETECTOR, not an independent
		// certification -- `h` is read from the SAME ri.ptCoord.y that
		// production computed h from in the first place, so this ties
		// production's normal-construction arithmetic to its own
		// h-reporting arithmetic rather than to an outside derivation.
		// What it catches: a mutation of the cylinder-normal formula
		// (verified -- reintroducing the pbrt angular-sweep form,
		// theta=h*pi/2, drops the measured agreement to ~0.54 here) or
		// a refactor that desyncs the two.  What it CANNOT catch: both
		// sides being wrong in the same way (e.g. a shared sign error
		// in how h itself is derived from the hit).
		{
			const double h = 2.0*ri.ptCoord.y - 1.0;
			const double expected = sqrt( std::max( 0.0, 1.0 - h*h ) );
			const double got = Vector3Ops::Dot( ri.vNormal, ri.vGeomNormal );
			worstCylinder = std::max( worstCylinder, fabs( got - expected ) );
			if( fabs( got - expected ) > 1e-9 ) ++badCylinderRule;
		}

		// Both normals are perpendicular to the fibre tangent:
		// CONSTRUCTION-FORCED (A and Nflat are built via Cross(_, T)
		// then Normalize() in RayElementIntersection), not an
		// independent property under test.  Catches a refactor that
		// decouples the frame, not a numerical-accuracy regression.
		if( fabs( Vector3Ops::Dot( ri.vNormal,     ri.vShadingTangent ) ) > 1e-9 ) ++badPerp;
		if( fabs( Vector3Ops::Dot( ri.vGeomNormal, ri.vShadingTangent ) ) > 1e-9 ) ++badPerp;

		// The ribbon plane normal faces the ray origin:
		// CONSTRUCTION-FORCED (see the invariant derivation at the
		// removed flip in HairGeometry.cpp's RayElementIntersection --
		// dot(Nflat, D) = -|D_perp| <= 0 always, given the {A, Nflat}
		// construction), not an independent property under test.
		// Catches a refactor that decouples the frame.
		if( !( Vector3Ops::Dot( ri.vGeomNormal, rays[i].Dir() ) < 0 ) ) ++badFacing;

	}

	std::cout << "  hits inspected             : " << hits << std::endl;
	std::cout << "  worst tangent angle at s   : " << std::fixed << std::setprecision(6) << worstTangentDeg << " deg" << std::endl;
	std::cout << "  worst tangent angle, loose : " << worstTangentNearDeg << " deg  (nearest-point cross-check)" << std::endl;
	std::cout << "  worst cylinder-rule error  : " << std::scientific << std::setprecision(3) << worstCylinder << std::endl;
	std::cout << "  worst s-position excess    : " << worstArcExcess
	          << "  (relative to the local half width; negative = strictly inside)" << std::defaultfloat << std::endl;
	std::cout << "  worst s-position excess/thr: " << worstArcExcessRatio
	          << "  (measured / derived float-quantisation threshold; <1 means inside the derived headroom)" << std::endl;

	Check( hits > 50, "group 2 inspected a meaningful number of hits" );
	Check( badRange == 0,        "every hit reports (s, t) inside [0,1]^2" );
	Check( badFlags == 0,        "every hit sets bShadingTangentFromGeometry, bHasShadingTangent, bHasTexCoord1" );
	Check( badUV1 == 0,          "ptCoord1 equals the root UV of the strand the hit is nearest to" );
	Check( badTangentUnit == 0,  "the reported shading tangent is unit length" );
	Check( badTangentAngle == 0, "the shading tangent is the curve derivative at the reported s (within 0.05 deg)" );
	Check( badTangentNear == 0,  "the shading tangent also matches the nearest-point derivative (loose, 10 deg)" );
	Check( badNormalUnit == 0,   "shading and geometric normals are unit length" );
	Check( badCylinderRule == 0, "the shading normal follows the cylinder rule exactly" );
	Check( badPerp == 0,         "both normals are perpendicular to the fibre tangent" );
	Check( badFacing == 0,       "the geometric (ribbon-plane) normal faces the ray origin" );
	Check( badArcPosition == 0,  "the curve point at the reported s is within the derived float-quantisation threshold of the hit position" );

	// -- WHITE BOX: the reflected phantom-endpoint convention must make
	//    a 2-control-point strand exactly straight.
	{
		std::vector<HairGeometry::StrandDesc> d( 1 );
		d[0].controlPoints.push_back( Point3( -0.3, 0.7, -0.2 ) );
		d[0].controlPoints.push_back( Point3(  1.1, -0.4, 2.5 ) );
		d[0].rootWidth = 0.05;
		d[0].tipWidth  = 0.02;
		d[0].rootUV    = Point2( 0.25, 0.75 );
		Ref<HairGeometry> g( new HairGeometry( d ) );

		// Compare against the STORED (float-rounded) control points, not
		// the doubles handed in: control points are deliberately kept in
		// float (see HairGeometry.h), so lerping the originals would
		// measure the storage quantisation (~2.5e-8 here) rather than
		// the basis.  What is under test is that the phantom-endpoint
		// convention cancels the quadratic and cubic terms EXACTLY.
		const Point3 a = g->ControlPoint( 0, 0 );
		const Point3 b = g->ControlPoint( 0, 1 );
		double worst = 0;
		for( int k = 0; k <= 20; ++k ) {
			const double t = (double)k / 20.0;
			const Point3 got = g->EvaluateStrand( 0, t, nullptr );
			const Point3 want( a.x + t*( b.x - a.x ), a.y + t*( b.y - a.y ), a.z + t*( b.z - a.z ) );
			worst = std::max( worst, Point3Ops::Distance( got, want ) );
		}
		std::cout << "  2-CP straightness error    : " << std::scientific << std::setprecision(3)
		          << worst << std::defaultfloat << std::endl;
		Check( worst < 1e-9, "a 2-control-point strand is exactly a straight line (phantom-endpoint convention)" );
	}

	// -- WHITE BOX: exit-info coverage.  A zero-thickness ribbon reports
	//    exit == entry (see the class comment); bComputeExitInfo=true on
	//    a real hit must reflect that, and must NOT touch the shared
	//    `ri`'s exit fields when this groom's hit is not closer than
	//    something already sitting in it.
	{
		int exitChecked = 0, badExitEq = 0, badExitGuard = 0;
		for( size_t i = 0; i < rays.size(); i += stride )
		{
			RayIntersectionGeometric ri( rays[i], nullRasterizerState );
			geom.IntersectRay( ri, true, true, true );
			if( !ri.bHit ) continue;
			++exitChecked;

			if( ri.range2 != ri.range )            ++badExitEq;
			if( ri.vNormal2.x != ri.vNormal.x || ri.vNormal2.y != ri.vNormal.y ||
			    ri.vNormal2.z != ri.vNormal.z )     ++badExitEq;
			if( ri.vGeomNormal2.x != ri.vGeomNormal.x || ri.vGeomNormal2.y != ri.vGeomNormal.y ||
			    ri.vGeomNormal2.z != ri.vGeomNormal.z ) ++badExitEq;

			// Guard case: simulate a prior, nearer hit already sitting in
			// `ri` (as if some other object in the scene had already
			// claimed it) before this groom is consulted.  Because the
			// groom's own IntersectSegment is handed ri.range as its
			// tMax, and this sentinel range is nearer than the real hit,
			// the groom must find nothing and must leave every sentinel
			// field untouched.
			RayIntersectionGeometric guard( rays[i], nullRasterizerState );
			guard.bHit   = true;
			guard.range  = ri.range * Scalar(0.5);
			guard.range2 = guard.range;
			const Vector3 sentinelNormal( Scalar(0.123), Scalar(0.456), Scalar(0.789) );
			guard.vNormal2     = sentinelNormal;
			guard.vGeomNormal2 = sentinelNormal;
			const Scalar sentinelRange2 = guard.range2;

			geom.IntersectRay( guard, true, true, true );

			if( guard.range2 != sentinelRange2 ) ++badExitGuard;
			if( guard.vNormal2.x != sentinelNormal.x || guard.vNormal2.y != sentinelNormal.y ||
			    guard.vNormal2.z != sentinelNormal.z ) ++badExitGuard;
			if( guard.vGeomNormal2.x != sentinelNormal.x || guard.vGeomNormal2.y != sentinelNormal.y ||
			    guard.vGeomNormal2.z != sentinelNormal.z ) ++badExitGuard;
		}
		std::cout << "  exit-info hits checked      : " << exitChecked << std::endl;
		Check( exitChecked > 50, "exit-info coverage inspected a meaningful number of hits" );
		Check( badExitEq    == 0, "bComputeExitInfo reports exit == entry (range2 / vNormal2 / vGeomNormal2)" );
		Check( badExitGuard == 0, "exit fields are untouched when our hit is not closer than a prior one already in `ri`" );
	}

	// -- WHITE BOX: face-flag coverage.  The class comment defends
	//    ignoring bHitFrontFaces/bHitBackFaces entirely so that fur
	//    stays visible to a dielectric refraction walk's exit-side
	//    probe (which issues bHitFrontFaces=false, bHitBackFaces=false).
	//    Confirm IntersectRay still finds the SAME crossing with both
	//    flags false.
	{
		int checked = 0, mismatched = 0;
		for( size_t i = 0; i < rays.size(); i += stride )
		{
			RayIntersectionGeometric baseline( rays[i], nullRasterizerState );
			geom.IntersectRay( baseline, true, true, false );
			if( !baseline.bHit ) continue;
			++checked;

			RayIntersectionGeometric flagged( rays[i], nullRasterizerState );
			geom.IntersectRay( flagged, false, false, false );
			if( !flagged.bHit || flagged.range != baseline.range ) ++mismatched;
		}
		std::cout << "  face-flag-false hits checked : " << checked << std::endl;
		Check( checked > 50, "face-flag coverage inspected a meaningful number of hits" );
		Check( mismatched == 0, "IntersectRay(false, false, ...) still finds the same crossing (face flags ignored)" );
	}
}

// ============================================================
//  Group 3 -- IntersectionOnly equivalence
// ============================================================

static void RunIntersectionOnly( const HairGeometry& geom, const std::vector<Ray>& rays )
{
	std::cout << "=== 3. IntersectRay_IntersectionOnly equivalence ===" << std::endl;

	int mismatches = 0, shortMiss = 0, longHit = 0, hits = 0;

	for( size_t i = 0; i < rays.size(); ++i )
	{
		RayIntersectionGeometric ri( rays[i], nullRasterizerState );
		geom.IntersectRay( ri, true, true, false );

		const bool any = geom.IntersectRay_IntersectionOnly( rays[i], RISE_INFINITY, true, true );
		if( any != (bool)ri.bHit ) ++mismatches;

		if( ri.bHit ) {
			++hits;
			// Just short of the reported hit: nothing may be found,
			// because the reported hit IS the closest one.
			if( geom.IntersectRay_IntersectionOnly( rays[i], ri.range * 0.99, true, true ) ) ++shortMiss;
			// Just past it: the same crossing must still be found.
			if( !geom.IntersectRay_IntersectionOnly( rays[i], ri.range * 1.01, true, true ) ) ++longHit;
		}
	}

	std::cout << "  rays                       : " << rays.size() << std::endl;
	std::cout << "  hits                       : " << hits << std::endl;
	std::cout << "  hit/miss mismatches        : " << mismatches << std::endl;
	std::cout << "  found inside 0.99*t        : " << shortMiss << std::endl;
	std::cout << "  missed inside 1.01*t       : " << longHit << std::endl;

	Check( mismatches == 0, "IntersectionOnly agrees with IntersectRay on hit/miss for every ray" );
	Check( shortMiss  == 0, "IntersectionOnly finds nothing closer than the reported closest hit" );
	Check( longHit    == 0, "IntersectionOnly still finds the hit when dHowFar is just past it" );
}

// ============================================================
//  Group 4 -- bounds
// ============================================================

static void RunBounds( const HairGeometry& geom, const std::vector<Ray>& rays )
{
	std::cout << "=== 4. Bounds ===" << std::endl;

	const BoundingBox bb = geom.GenerateBoundingBox();
	Point3 sphC; Scalar sphR = 0;
	geom.GenerateBoundingSphere( sphC, sphR );

	int outsideBox = 0, outsideSphere = 0, hits = 0;
	const double pad = 1e-9;

	for( size_t i = 0; i < rays.size(); ++i )
	{
		RayIntersectionGeometric ri( rays[i], nullRasterizerState );
		geom.IntersectRay( ri, true, true, false );
		if( !ri.bHit ) continue;
		++hits;

		const Point3& p = ri.ptIntersection;
		if( p.x < bb.ll.x - pad || p.x > bb.ur.x + pad ||
		    p.y < bb.ll.y - pad || p.y > bb.ur.y + pad ||
		    p.z < bb.ll.z - pad || p.z > bb.ur.z + pad ) ++outsideBox;

		if( Point3Ops::Distance( p, sphC ) > sphR + pad ) ++outsideSphere;
	}

	const double vol = ( bb.ur.x - bb.ll.x ) * ( bb.ur.y - bb.ll.y ) * ( bb.ur.z - bb.ll.z );

	std::cout << "  hits                       : " << hits << std::endl;
	std::cout << "  box  [" << std::fixed << std::setprecision(4)
	          << bb.ll.x << ", " << bb.ll.y << ", " << bb.ll.z << "] .. ["
	          << bb.ur.x << ", " << bb.ur.y << ", " << bb.ur.z << "]  vol " << vol
	          << std::defaultfloat << std::endl;
	std::cout << "  hits outside box           : " << outsideBox << std::endl;
	std::cout << "  hits outside sphere        : " << outsideSphere << std::endl;

	// Derive an honest upper bound on the box volume from BuildGroom's
	// OWN construction constants (mirrored here -- keep these in sync
	// if BuildGroom's literals change) instead of an arbitrary "1000"
	// that would pass even if GenerateBoundingBox were padding by a
	// wildly wrong amount:
	//   x, y control points = rx/ry (range [-1,1]) + a curl offset of
	//                         magnitude <= 0.35 (curl<=1, |f*cos/sin|<=1)
	//   z control points    = len*f, len in [0.8, 2.0], f in [0,1]
	//                         (>= 0 by construction)
	//   padding              = max half width; rootWidth <= 0.06 => <= 0.03
	// Each per-axis bound is already loose on its own (no single strand
	// hits all three per-axis maxima simultaneously, let alone the
	// union over 50 strands at different phases); a further explicit 2x
	// factor is kept on top as a safety margin.
	const double kBuildRxRyRange  = 1.0;
	const double kBuildCurlOffset = 0.35;
	const double kBuildLenMax     = 2.0;
	const double kBuildWidthMax   = 0.06;
	const double halfWidthMax = kBuildWidthMax * 0.5;
	const double xyHalfExtent = kBuildRxRyRange + kBuildCurlOffset + halfWidthMax;
	const double zExtent      = kBuildLenMax + halfWidthMax;
	const double volBound     = 2.0 * ( 2.0 * xyHalfExtent ) * ( 2.0 * xyHalfExtent ) * zExtent;
	std::cout << "  derived volume bound        : " << volBound << std::endl;

	Check( outsideBox    == 0, "every reported hit point lies inside GenerateBoundingBox" );
	Check( outsideSphere == 0, "every reported hit point lies inside GenerateBoundingSphere" );
	Check( vol > 0 && vol < volBound, "the groom bounding box is neither degenerate nor looser than BuildGroom's own extents predict (2x margin)" );
}

// ============================================================
//  Group 5 -- degenerates
// ============================================================

static void RunDegenerates()
{
	std::cout << "=== 5. Degenerate input ===" << std::endl;
	std::cout << "  (per-strand rejection warnings below are EXPECTED)" << std::endl;

	// -- a strand with a single control point is rejected; a good
	//    strand alongside it still builds.
	{
		std::vector<HairGeometry::StrandDesc> d( 2 );
		d[0].controlPoints.push_back( Point3( 0, 0, 0 ) );
		d[0].rootWidth = 0.01; d[0].tipWidth = 0.01;

		d[1].controlPoints.push_back( Point3( 0, 0, 0 ) );
		d[1].controlPoints.push_back( Point3( 0, 0, 1 ) );
		d[1].rootWidth = 0.01; d[1].tipWidth = 0.005;

		Ref<HairGeometry> g( new HairGeometry( d ) );
		Check( g->numStrands() == 1,         "a strand with 1 control point is rejected" );
		Check( g->numRejectedStrands() == 1, "the 1-control-point rejection is counted" );
		Check( g->numSegments() >= 1,        "the surviving strand still produced segments" );
	}

	// -- zero and negative widths are rejected.
	{
		std::vector<HairGeometry::StrandDesc> d( 3 );
		for( int k = 0; k < 3; ++k ) {
			d[k].controlPoints.push_back( Point3( (Scalar)k, 0, 0 ) );
			d[k].controlPoints.push_back( Point3( (Scalar)k, 0, 1 ) );
		}
		d[0].rootWidth =  0.0;  d[0].tipWidth = 0.01;	// zero root
		d[1].rootWidth =  0.01; d[1].tipWidth = -0.02;	// negative tip
		d[2].rootWidth =  0.01; d[2].tipWidth = 0.01;	// good

		Ref<HairGeometry> g( new HairGeometry( d ) );
		Check( g->numStrands() == 1,         "zero and negative widths are rejected" );
		Check( g->numRejectedStrands() == 2, "both bad-width rejections are counted" );
	}

	// -- a non-finite control point is rejected.
	{
		std::vector<HairGeometry::StrandDesc> d( 1 );
		d[0].controlPoints.push_back( Point3( 0, 0, 0 ) );
		d[0].controlPoints.push_back( Point3( 0, 0, sqrt( -1.0 ) ) );	// NaN
		d[0].rootWidth = 0.01; d[0].tipWidth = 0.01;

		Ref<HairGeometry> g( new HairGeometry( d ) );
		Check( g->numStrands() == 0,         "a non-finite control point is rejected" );
		Check( g->numRejectedStrands() == 1, "the non-finite rejection is counted" );
	}

	// -- a strand past the 16-bit span limit is rejected.
	{
		std::vector<HairGeometry::StrandDesc> d( 1 );
		d[0].controlPoints.resize( HairGeometry::kMaxControlPointsPerStrand + 1 );
		for( size_t k = 0; k < d[0].controlPoints.size(); ++k ) {
			d[0].controlPoints[k] = Point3( 0, 0, (Scalar)k * 1e-4 );
		}
		d[0].rootWidth = 0.01; d[0].tipWidth = 0.01;

		Ref<HairGeometry> g( new HairGeometry( d ) );
		Check( g->numStrands() == 0,         "a strand past kMaxControlPointsPerStrand is rejected" );
		Check( g->numRejectedStrands() == 1, "the over-long-strand rejection is counted" );
	}

	// -- the empty groom.
	{
		std::vector<HairGeometry::StrandDesc> none;
		Ref<HairGeometry> g( new HairGeometry( none ) );

		Check( g->numStrands()  == 0, "an empty groom has no strands" );
		Check( g->numSegments() == 0, "an empty groom has no segments" );

		const BoundingBox bb = g->GenerateBoundingBox();
		// DOCUMENTED behaviour: a zero-extent box at the object-space
		// origin, never the inverted-infinity box, because
		// Object::IntersectRay feeds it straight to RayBoxIntersection.
		Check( bb.ll.x == 0 && bb.ll.y == 0 && bb.ll.z == 0 &&
		       bb.ur.x == 0 && bb.ur.y == 0 && bb.ur.z == 0,
		       "an empty groom reports the zero-extent box at the origin (documented)" );

		Point3 c; Scalar r = -1;
		g->GenerateBoundingSphere( c, r );
		Check( r == 0, "an empty groom's bounding sphere has zero radius" );

		int anyHit = 0;
		RandomNumberGenerator rng( kSeed + 99u );
		for( int i = 0; i < 200; ++i ) {
			const Point3 o( rng.CanonicalRandom()*4-2, rng.CanonicalRandom()*4-2, rng.CanonicalRandom()*4-2 );
			const Vector3 dir = Vector3Ops::Normalize( Vector3Ops::mkVector3(
				Point3( rng.CanonicalRandom()*4-2, rng.CanonicalRandom()*4-2, rng.CanonicalRandom()*4-2 ), o ) );
			const Ray ray( o, dir );
			RayIntersectionGeometric ri( ray, nullRasterizerState );
			g->IntersectRay( ri, true, true, true );
			if( ri.bHit ) ++anyHit;
			if( g->IntersectRay_IntersectionOnly( ray, RISE_INFINITY, true, true ) ) ++anyHit;
		}
		Check( anyHit == 0, "an empty groom intersects nothing" );
	}

	// -- the area-light / tessellation refusals.
	{
		std::vector<HairGeometry::StrandDesc> d( 1 );
		d[0].controlPoints.push_back( Point3( 0, 0, 0 ) );
		d[0].controlPoints.push_back( Point3( 0, 0, 1 ) );
		d[0].rootWidth = 0.01; d[0].tipWidth = 0.01;
		Ref<HairGeometry> g( new HairGeometry( d ) );

		Check( !g->CanBeAreaLight(), "hair refuses to serve as an area light" );
		Check( !g->CanTessellate(),  "hair refuses to tessellate" );
		Check( g->GetArea() == 0,    "hair reports zero area" );

		IndexTriangleListType tris; VerticesListType verts;
		NormalsListType norms; TexCoordsListType coords;
		Check( !g->TessellateToMesh( tris, verts, norms, coords, 8 ),
		       "TessellateToMesh refuses (base-class default)" );

		// UniformRandomPoint must be deterministic even though it is
		// unreachable through a guarded caller.
		Point3 p1, p2; Vector3 n1, n2; Point2 c1, c2;
		g->UniformRandomPoint( &p1, &n1, &c1, Point3( 0.3, 0.7, 0.1 ) );
		g->UniformRandomPoint( &p2, &n2, &c2, Point3( 0.3, 0.7, 0.1 ) );
		Check( Point3Ops::Distance( p1, p2 ) == 0, "UniformRandomPoint is deterministic for the same prand" );
	}
}

// ============================================================
//  Group 6 -- scale sanity
// ============================================================

static void RunScale()
{
	std::cout << "=== 6. Scale sanity (100k strands x 8 control points) ===" << std::endl;

	const int kStrands = 100000;
	const int kCP      = 8;

	std::vector<HairGeometry::StrandDesc> descs;
	descs.reserve( kStrands );

	// Procedural spiral fill: strands seeded on a disc by the golden
	// angle, each curling as it rises.  Deterministic -- no RNG.
	const double golden = 2.399963229728653;
	for( int i = 0; i < kStrands; ++i )
	{
		const double a = golden * (double)i;
		const double r = 2.0 * sqrt( (double)i / (double)kStrands );
		const double len = 0.4 + 0.3 * ( (double)( i % 7 ) / 6.0 );

		HairGeometry::StrandDesc d;
		d.controlPoints.reserve( kCP );
		for( int k = 0; k < kCP; ++k ) {
			const double f = (double)k / (double)( kCP - 1 );
			const double th = a + 1.2*f;
			d.controlPoints.push_back( Point3(
				( r + 0.08*f ) * cos( th ),
				( r + 0.08*f ) * sin( th ),
				len * f ) );
		}
		d.rootWidth = 0.004;
		d.tipWidth  = 0.001;
		d.rootUV    = Point2( r / 2.0, a / golden / (double)kStrands );
		descs.push_back( d );
	}

	const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	HairGeometry* raw = new HairGeometry( descs );
	Ref<HairGeometry> g( raw );
	const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

	const double buildMs = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
	const size_t bytes   = g->ApproximateMemoryBytes();

	std::cout << "  strands accepted           : " << g->numStrands() << std::endl;
	std::cout << "  control points             : " << g->numControlPoints() << std::endl;
	std::cout << "  segments                   : " << g->numSegments()
	          << "  (" << std::fixed << std::setprecision(2)
	          << (double)g->numSegments() / (double)( kStrands * ( kCP - 1 ) )
	          << " per span)" << std::defaultfloat << std::endl;
	std::cout << "  build time                 : " << std::fixed << std::setprecision(1)
	          << buildMs << " ms" << std::defaultfloat << std::endl;
	std::cout << "  approximate resident size  : " << std::fixed << std::setprecision(1)
	          << (double)bytes / ( 1024.0*1024.0 ) << " MiB"
	          << "  (" << std::setprecision(1) << (double)bytes / (double)kStrands << " B/strand)"
	          << std::defaultfloat << std::endl;

	Check( g->numStrands() == (unsigned int)kStrands, "every generated strand was accepted" );
	Check( g->numRejectedStrands() == 0,              "no strand was rejected at scale" );
	Check( g->numControlPoints() == (unsigned int)( kStrands * kCP ), "the control-point count is right" );
	Check( g->numSegments() >= (unsigned int)( kStrands * ( kCP - 1 ) ), "every span produced at least one segment" );

	// Fire a modest ray batch so the tree is actually walked at scale.
	const BoundingBox bb = g->GenerateBoundingBox();
	std::vector<Ray> rays;
	BuildRays( rays, 2000, bb.ll, bb.ur );

	int hits = 0;
	const std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
	for( size_t i = 0; i < rays.size(); ++i ) {
		RayIntersectionGeometric ri( rays[i], nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		if( ri.bHit ) ++hits;
	}
	const std::chrono::steady_clock::time_point t3 = std::chrono::steady_clock::now();

	std::cout << "  2000 rays                  : " << hits << " hits in "
	          << std::fixed << std::setprecision(1)
	          << std::chrono::duration<double, std::milli>( t3 - t2 ).count() << " ms"
	          << std::defaultfloat << std::endl;

	Check( hits > 0, "the 100k-strand groom is actually hit by the probe rays" );
}

// ============================================================
//  main
// ============================================================

int main()
{
	std::cout << "===== HairGeometry tests =====" << std::endl << std::endl;

	std::vector<RefStrand> refStrands;
	std::vector<HairGeometry::StrandDesc> descs;
	BuildGroom( refStrands, descs );

	Ref<HairGeometry> geom( new HairGeometry( descs ) );

	const BoundingBox bb = geom->GenerateBoundingBox();
	const double diag = Point3Ops::Distance( bb.ur, bb.ll );

	std::vector<Ray> rays;
	BuildRays( rays, kNumRays, bb.ll, bb.ur );

	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

	RunDifferential( *geom, refStrands, rays, diag );
	std::cout << std::endl;
	RunOutputValidity( *geom, refStrands, rays );
	std::cout << std::endl;
	RunIntersectionOnly( *geom, rays );
	std::cout << std::endl;
	RunBounds( *geom, rays );
	std::cout << std::endl;
	RunDegenerates();
	std::cout << std::endl;
	RunScale();
	std::cout << std::endl;

	const std::chrono::steady_clock::time_point stop = std::chrono::steady_clock::now();
	std::cout << "total test wall time: " << std::fixed << std::setprecision(1)
	          << std::chrono::duration<double>( stop - start ).count() << " s"
	          << std::defaultfloat << std::endl << std::endl;

	std::cout << "===== Summary =====" << std::endl;
	std::cout << "checks run:    " << g_checks << std::endl;
	std::cout << "checks failed: " << g_failures << std::endl;

	if( g_failures > 0 ) {
		std::cout << std::endl << "HairGeometryTest FAILED" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All hair geometry tests passed!" << std::endl;
	return 0;
}
