//////////////////////////////////////////////////////////////////////
//
//  SDFGeometryTest.cpp - Unit tests for the sphere-traced signed-
//  distance-field geometry.  Validates that ray intersection + ray
//  marching are correct independent of any scene: hit distance and
//  normal match the analytic primitive across many ray directions,
//  smooth-min bulges the seam outward, the bounding box contains the
//  surface, shadow (intersection-only) queries respect dHowFar, misses
//  miss, inside-start rays exit, and transforms / non-uniform scale
//  land where expected.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>
#include <limits>
#include <mutex>
#include <string>
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Interfaces/ILogPriv.h"		// degenerate-part warning capture
#include "../src/Library/Functions/ConstantFunctions.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/FiniteMath.h"	// Test 36d's finite-bbox assertion (-Ofast-resistant)

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

// A minimal ILogPrinter that records every message containing `needle`.  Same
// shape (and the same reasoning) as ObjectMirrorTest's / CstSourceInstanceTest's:
// ParsePartLines' degenerate-shape-parameters diagnostic is a WARNING, not a
// parse failure (the part still parses and composes -- see the superellipsoid
// exponent clamp it sits next to), so it is invisible to a `ParsePartLines`
// bool return and has to be observed on the log itself.
class CapturingLogPrinter : public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
{
public:
	explicit CapturingLogPrinter( std::string needle ) : mNeedle( std::move( needle ) ) {}

	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( mNeedle ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mMatches.push_back( msg );
		}
	}
	void Flush() override {}

	int MatchCount() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return static_cast<int>( mMatches.size() );
	}
	std::string LastMatch() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mMatches.empty() ? std::string() : mMatches.back();
	}
	void Clear()
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mMatches.clear();
	}

protected:
	~CapturingLogPrinter() override {}

private:
	std::string                mNeedle;
	mutable std::mutex         mMutex;
	std::vector<std::string>   mMatches;
};

// Installed once in main(), before any test runs (see the CapturingLogPrinter
// comment above for why this can't just check ParsePartLines' return value).
static CapturingLogPrinter* g_degenerateShapeWarn = 0;

static bool IsClose( Scalar a, Scalar b, Scalar eps = 2e-3 ) { return std::fabs(a-b) <= eps; }
static Scalar Len( const Vector3& v ) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
static bool VClose( const Vector3& a, const Vector3& b, Scalar eps = 6e-3 )
{
	return IsClose(a.x,b.x,eps) && IsClose(a.y,b.y,eps) && IsClose(a.z,b.z,eps);
}
static RayIntersectionGeometric MkRI( const Point3& o, const Vector3& d )
{
	return RayIntersectionGeometric( Ray(o,d), nullRasterizerState );
}
static Vector3 norm3( Scalar x, Scalar y, Scalar z )
{
	const Scalar l = std::sqrt(x*x+y*y+z*z);
	return Vector3( x/l, y/l, z/l );
}

// build an SDFGeometry from a single part / a list (tight eps for accuracy)
static SDFGeometry* MakeGeom( const std::vector<SDFGeometry::Part>& parts )
{
	return new SDFGeometry( parts, 512, Scalar(1e-5) );
}

//////////////////////////////////////////////////////////////////////

static void TestSphereMatchesAnalytic()
{
	std::cout << "Test 1: SDF sphere == analytic sphere (range + normal, many dirs)" << std::endl;
	const Scalar R = 5.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	// cast from |P|=20 toward the origin: the near hit is at distance 20-R, normal = P/|P|
	const Point3 origins[6] = {
		Point3(0,0,20), Point3(20,0,0), Point3(0,20,0),
		Point3(12,12,8), Point3(-9,6,16), Point3(-11,-11,-9)
	};
	for( int i = 0; i < 6; ++i )
	{
		const Point3 P = origins[i];
		const Scalar Rmag = std::sqrt(P.x*P.x + P.y*P.y + P.z*P.z);
		const Vector3 dir( -P.x/Rmag, -P.y/Rmag, -P.z/Rmag );  // toward origin
		RayIntersectionGeometric ri = MkRI( P, dir );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "sphere ray hits" );
		Check( IsClose( ri.range, Rmag - R, 3e-3 ), "sphere hit distance == analytic" );
		Check( VClose( ri.vNormal, norm3(P.x,P.y,P.z) ), "sphere normal == radial" );
		Check( IsClose( Len(ri.vNormal), 1.0, 1e-4 ), "sphere normal is unit" );
	}
	safe_release( g );
}

static void TestBoxMatchesAnalytic()
{
	std::cout << "Test 2: SDF box == analytic box (face hits + normals)" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3.0, 2.0, 4.0, 0 ) );   // half-extents
	SDFGeometry* g = MakeGeom( parts );

	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 16.0), "box +Z face hit distance" );
		Check( VClose(ri.vNormal, Vector3(0,0,1)), "box +Z face normal" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "box +X face hit distance" );
		Check( VClose(ri.vNormal, Vector3(1,0,0)), "box +X face normal" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(0,-20,0), Vector3(0,1,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 18.0), "box -Y face hit distance" );
		Check( VClose(ri.vNormal, Vector3(0,-1,0)), "box -Y face normal" ); }
	safe_release( g );
}

static void TestSmoothMin()
{
	std::cout << "Test 3: smooth-min bulges the seam outward vs hard union" << std::endl;
	// two spheres r=4 at y=+/-3; a ray along +X through the neck (y=0,z=0)
	const Vector3 dir(1,0,0);
	const Point3  o(-20,0,0);

	std::vector<SDFGeometry::Part> uni;
	uni.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0, Point3(0,-3,0),0,0,0,Vector3(1,1,1),4,0,0,0 ) );
	uni.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0, Point3(0, 3,0),0,0,0,Vector3(1,1,1),4,0,0,0 ) );
	SDFGeometry* gu = MakeGeom( uni );

	std::vector<SDFGeometry::Part> smn;
	smn.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,   Point3(0,-3,0),0,0,0,Vector3(1,1,1),4,0,0,0 ) );
	smn.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin,  3.0, Point3(0, 3,0),0,0,0,Vector3(1,1,1),4,0,0,0 ) );
	SDFGeometry* gs = MakeGeom( smn );

	RayIntersectionGeometric ru = MkRI(o,dir);  gu->IntersectRay( ru, true, true, false );
	RayIntersectionGeometric rs = MkRI(o,dir);  gs->IntersectRay( rs, true, true, false );
	Check( ru.bHit && rs.bHit, "both union + smin hit the neck" );
	// the smin fillet bulges OUTWARD at the neck, so the front surface is closer to the ray origin -> smaller range
	Check( rs.range < ru.range - 1e-3, "smin front is outside the hard-union crease (smaller range)" );
	safe_release( gu ); safe_release( gs );
}

static void TestBoundingBoxContainsSurface()
{
	std::cout << "Test 4: bounding box contains the surface" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(2,0,0), 0,0,0, Vector3(1,1,1), 5.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );
	BoundingBox bb = g->GenerateBoundingBox();
	// sphere r5 centred at (2,0,0) -> surface spans x in [-3,7], y,z in [-5,5]
	Check( bb.ll.x <= -3.0 && bb.ur.x >= 7.0, "bbox spans sphere in X" );
	Check( bb.ll.y <= -5.0 && bb.ur.y >= 5.0, "bbox spans sphere in Y" );
	Check( bb.ll.z <= -5.0 && bb.ur.z >= 5.0, "bbox spans sphere in Z" );
	// a hit should land inside the bbox
	RayIntersectionGeometric ri = MkRI( Point3(2,0,20), Vector3(0,0,-1) );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit && ri.ptIntersection.z <= bb.ur.z + 1e-3 && ri.ptIntersection.z >= bb.ll.z - 1e-3,
	       "hit point inside bbox" );
	safe_release( g );
}

// The bbox must be OP-AWARE: an `intersect` part CLIPS the field to within its
// own box, and a `subtract` part only carves -- neither should inflate the AABB
// to a giant additive part's full extent.  This mirrors the domed-crystal
// construction (a sphere of radius ~68 kept only inside a thin cap slab, with
// marker cavities subtracted): unioning every part's box regardless of op would
// blow the box to +-68 even though the visible surface is a ~43-wide, ~3.5-tall
// cap, fattening the TLAS leaf and starving the marching-tet grid.
static void TestBoundingBoxOpAware()
{
	std::cout << "Test 4b: bbox respects intersect (clip) and subtract (no-grow)" << std::endl;

	// (1) giant sphere r=68 at z=-63 (apex ~+5), INTERSECT a keep-slab box
	// half(22,22,2.35) at z=3.65 -> the surviving solid is the cap in z in
	// [1.3, 6.0], xy within +-22.  The bbox must clip to the slab, NOT span the
	// whole sphere (which alone would give z in [-131, +5], xy in [-68, 68]).
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,0,-63.0), 0,0,0, Vector3(1,1,1), 68.0, 0,0,0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpIntersect, 0,
			Point3(0,0,3.65), 0,0,0, Vector3(1,1,1), 22.0, 22.0, 2.35, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const BoundingBox bb = g->GenerateBoundingBox();
		// clipped to the slab (allow the small pad/margin on each side)
		Check( bb.ll.x >= -22.0 - 0.5 && bb.ur.x <= 22.0 + 0.5, "intersect clips X to the slab (not +-68)" );
		Check( bb.ll.y >= -22.0 - 0.5 && bb.ur.y <= 22.0 + 0.5, "intersect clips Y to the slab (not +-68)" );
		Check( bb.ll.z >= 1.3 - 0.5 && bb.ur.z <= 6.0 + 0.5,    "intersect clips Z to the slab (not z=-131)" );
		// but the box must still CONTAIN the surviving cap: a ray angled down at
		// the axis hits the cap's outer dome inside the slab (z in [1.3, 5]).
		// (A pure-axis ray would enter the clipped bbox EXACTLY at the sphere's
		// tangent apex z~=5.001 -- inside the marcher's surface band -- and step
		// off it, a known grazing-entry edge case unrelated to the clip; come in
		// at a slight angle so the entry is a clean transverse crossing.)
		RayIntersectionGeometric ri = MkRI( Point3(2,0,20), Vector3Ops::Normalize( Vector3(-0.05,0,-1) ) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && ri.ptIntersection.z >= 1.3 - 5e-3 && ri.ptIntersection.z <= 5.0 + 5e-3,
		       "clipped cap is hittable, hit lands on the cap (z in [1.3,5])" );
		Check( ri.ptIntersection.z <= bb.ur.z + 1e-3 && ri.ptIntersection.z >= bb.ll.z - 1e-3, "cap hit inside clipped bbox" );
		safe_release( g );
	}

	// (2) a SUBTRACT part reaching far outside the solid must not grow the box:
	// box half(3,3,3) at origin, subtract a huge box half(50,50,50) offset so it
	// pokes far in +X -- the carve shrinks the solid but the AABB stays ~[-3,3].
	{
		std::vector<SDFGeometry::Part> base;
		base.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3,3,3, 0 ) );
		SDFGeometry* gBase = MakeGeom( base );
		const BoundingBox bbBase = gBase->GenerateBoundingBox();
		safe_release( gBase );

		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3,3,3, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 0,
			Point3(40,0,0), 0,0,0, Vector3(1,1,1), 50,50,50, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const BoundingBox bb = g->GenerateBoundingBox();
		Check( IsClose( bb.ur.x, bbBase.ur.x, 1e-6 ) && IsClose( bb.ll.x, bbBase.ll.x, 1e-6 ), "subtract does not grow X" );
		Check( IsClose( bb.ur.y, bbBase.ur.y, 1e-6 ) && IsClose( bb.ll.y, bbBase.ll.y, 1e-6 ), "subtract does not grow Y" );
		Check( IsClose( bb.ur.z, bbBase.ur.z, 1e-6 ) && IsClose( bb.ll.z, bbBase.ll.z, 1e-6 ), "subtract does not grow Z" );
		safe_release( g );
	}

	// (3) ORDER-AWARENESS: the bbox must fold like Map() (a strict left fold),
	// NOT split by op-class.  Parts: union box half(1,1,1)@origin, INTERSECT box
	// half(1,1,1)@origin, then union box half(1,1,1)@(10,0,0).  The true solid is
	// ( box[-1,1] INTERSECT box[-1,1] ) UNION box[9,11] -> x in [-1,1] U [9,11],
	// so the AABB x-extent is [-1, 11].  An order-BLIND class split (union the two
	// additive boxes -> x[-1,11], intersect the clip box -> x[-1,1], combine ->
	// x[-1,1]) would clip the second lobe entirely and bbox-gate every ray to it.
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1,1,1, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpIntersect, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1,1,1, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(10,0,0), 0,0,0, Vector3(1,1,1), 1,1,1, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const BoundingBox bb = g->GenerateBoundingBox();
		// the bound must reach the second lobe (>= 11 minus the tiny safety pad)
		// AND still contain the first lobe (<= -1 plus the pad).
		Check( bb.ur.x >= 10.9, "order-aware bbox reaches the post-intersect union lobe (x>=~11)" );
		Check( bb.ll.x <= -0.9, "order-aware bbox still contains the first lobe (x<=~-1)" );
		// the regression an order-blind bbox would cause: a ray aimed at the second
		// lobe gets gated out by the (wrongly clipped) box and never hits.  Fire one
		// straight down onto the top face of the second box (top at z=+1).
		RayIntersectionGeometric ri = MkRI( Point3(10,0,5), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.ptIntersection.z, 1.0, 5e-3 ), "second lobe is hittable (top face z=+1)" );
		safe_release( g );
	}
}

// A WIDE-but-THIN field has a HUGE bbox diagonal, so m_eps (= diagonal*epsFrac)
// and the marcher's surfBand (= 2*m_eps) blow up well past a fixed 1e-3 bbox pad.
// The old fixed pad then let a camera ray entering through the thin TOP face land
// INSIDE the surface band: the step-off logic (meant only for continuation rays
// spawned ON a surface) marched it DOWN into the solid, read side=-1, concluded
// the ray began inside, and reported the EXIT (bottom) face -- the top face was
// silently skipped (emissive boxes whose sides glow but tops render dark/holes).
// The fix sizes the bbox pad to clear the band (pad = max(1e-3, 3*eps)).  Build
// the field at the SCENE's epsFrac (5e-5) so the band (~5.4e-3) is unambiguously
// larger than the old 1e-3 pad -- this case fails on the pre-fix engine.
static void TestWideThinTopFaceEntry()
{
	std::cout << "Test 4c: wide-thin SDF -- top-face entry not mis-sided into the exit" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,1.5), 0,0,0, Vector3(1,1,1), 19.0, 19.0, 0.2, 0 ) );  // half-extents: top z=1.7, bottom z=1.3
	SDFGeometry* g = new SDFGeometry( parts, 512, Scalar(5e-5) );          // scene epsFrac -> band ~5.4e-3 >> old 1e-3 pad

	// Camera ray straight down through the TOP face: must HIT the TOP (z~=1.7),
	// NOT tunnel to the bottom (z~=1.3 is what the old mis-siding reported), and
	// the front-face normal must point +Z.
	{	RayIntersectionGeometric ri = MkRI( Point3(5,5,50), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "wide-thin: top-entry ray hits" );
		Check( ri.bHit && IsClose( ri.ptIntersection.z, 1.7, 1e-2 ), "wide-thin: hit lands on the TOP face z~=1.7 (not the exit z=1.3)" );
		Check( ri.bHit && ri.ptIntersection.z > 1.5, "wide-thin: hit is the top half, never the bottom exit" );
		Check( VClose( ri.vNormal, Vector3(0,0,1) ), "wide-thin: top-face normal points +Z (front side)" ); }

	// Entry through a SIDE face still works (the thin axis is z; +X face at x=19).
	{	RayIntersectionGeometric ri = MkRI( Point3(50,5,1.5), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.ptIntersection.x, 19.0, 1e-2 ), "wide-thin: side-face entry hits +X face x~=19" );
		Check( VClose( ri.vNormal, Vector3(1,0,0) ), "wide-thin: side-face normal points +X" ); }
	safe_release( g );
}

// A FLAT heightfield SDF (f(u,v)=1 -> surface z = scale) is the sharpest form of
// the top-entry-vs-tunnel bug the part-based Test 4c guards: here the surface
// COINCIDES with the bbox top face, so NO bbox pad can move the eye-ray's clipped
// start out of the surface band -- the pad-sizing fix that saved the part-based
// case cannot save this one.  Before the Map(origin)-gated step-off, an eye ray
// straight down the axis got mis-sided into the solid and reported the BOTTOM
// (z=0) or missed, so a refractive dielectric heightfield over a substrate (the
// enamel dial over the silver dome) rendered SOLID BLACK -- the eye ray tunnelled
// through the top without a refraction event, never entering the glass.  Built at
// the SCENE's auto epsilon (0.0 -> pad ~1e-3 < the |scale|-tied band ~3e-3) so it
// tunnels on the pre-fix engine and this case is a genuine discriminator.
static void TestHeightfieldFlatTopEntry()
{
	std::cout << "Test 4d: FLAT heightfield SDF -- eye ray enters the top, not tunnelling to the bottom" << std::endl;
	const Scalar R = Scalar(1.6), S = Scalar(0.30);
	ConstantFunction2D* flat = new ConstantFunction2D( Scalar(1.0) );   // f(u,v) == 1
	SDFGeometry* g = new SDFGeometry( flat, R, S, 512, Scalar(0.0) );   // 0.0 = scene auto epsilon

	// Straight-down eye ray at the disk centre: must HIT the TOP (z==scale) as a
	// FRONT face, never the bottom exit (z=0) and never a miss.
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,5), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "heightfield: top-entry eye ray hits (pre-fix tunnels to a miss/bottom)" );
		Check( ri.bHit && IsClose( ri.ptIntersection.z, S, 5e-3 ), "heightfield: hit is the TOP face z==scale (not the z=0 bottom)" );
		Check( ri.bHit && ri.ptIntersection.z > S*Scalar(0.5), "heightfield: hit is the top half, never the z=0 exit" );
		Check( ri.bHit && VClose( ri.vNormal, Vector3(0,0,1) ), "heightfield: top-face normal points +Z (front side)" ); }

	// An off-centre eye ray (still inside the disk) must also enter the top.
	{	RayIntersectionGeometric ri = MkRI( Point3(0.8,0.3,5), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.ptIntersection.z, S, 5e-3 ), "heightfield: off-centre eye ray also enters the top face" ); }

	// A continuation ray SPAWNED on the top surface going down (the refracted ray
	// entering the glass) must still step off its OWN band -- the fix must not
	// break self-hit avoidance.  The heightfield is a half-space (semi-infinite
	// below the surface, no bottom face), so a straight-down ray at the centre
	// correctly finds no further surface; the invariant is only that it must NOT
	// report a paper-thin self-hit back at the spawn surface z=S (which is what
	// re-blackens the glass).
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,S), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( !( ri.bHit && ri.ptIntersection.z > S - 1e-2 ), "heightfield: on-surface continuation ray steps off its own band (no self-hit at z=scale)" ); }

	// SHADOW / visibility fast path (IntersectRay_IntersectionOnly) shares the same
	// March, so it tunnelled the same way pre-fix -- a light around or below the
	// dial would leak straight through the enamel top.  Existing Test 5 only covers
	// a sphere (surface not coincident with the bbox face), so it never caught this.
	// Lock the sibling explicitly: the flat top must OCCLUDE a ray that reaches it
	// and NOT occlude one that stops short.
	Check(  g->IntersectRay_IntersectionOnly( Ray( Point3(0,0,5), Vector3(0,0,-1) ), Scalar(10.0), true, true ),
	        "heightfield: shadow path sees the top as occluding within reach (pre-fix tunnels -> not occluding)" );
	Check( !g->IntersectRay_IntersectionOnly( Ray( Point3(0,0,5), Vector3(0,0,-1) ), Scalar(4.0),  true, true ),
	        "heightfield: shadow path does not occlude a ray that stops short of the top (dHowFar honoured)" );
	safe_release( g );
	safe_release( flat );   // SDFGeometry addref'd its own ref; release the one `new` gave us (Reference starts at refcount 1)

	// Non-vacuous step-off coverage: a CLOSED part-based slab (top + bottom faces)
	// so a continuation ray spawned ON the top going down must step off its own
	// band AND reach the far (bottom) surface -- the "find the next surface" half of
	// the step-off that a half-space heightfield cannot exercise.
	{	std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,1.5), 0,0,0, Vector3(1,1,1), 4.0, 4.0, 0.2, 0 ) );   // top z=1.7, bottom z=1.3
		SDFGeometry* slab = new SDFGeometry( parts, 512, Scalar(5e-5) );
		RayIntersectionGeometric ri = MkRI( Point3(0,0,1.7), Vector3(0,0,-1) );   // spawned ON the top face
		slab->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.ptIntersection.z, 1.3, 1e-2 ),
		       "slab: on-top continuation ray steps off and reaches the BOTTOM face z~=1.3 (not a self-hit at z=1.7)" );
		safe_release( slab ); }
}

// Heightfield mode's domain is a DISK of radius R centred at the origin in the
// local XY plane, NOT the square [-R,R]^2 that older comments/descriptor text
// claimed (see SDFGeometry::Map's "Clip the heightfield to a CIRCULAR disk"
// branch, which max()s the field against rho = sqrt(x^2+y^2) - R).  A corner
// point at 0.99R on both axes sits inside the square but at radius
// 0.99R*sqrt(2) ~= 1.40R from the origin -- outside the disk -- and must MISS
// entirely; a point at 0.7R on both axes sits at 0.7R*sqrt(2) ~= 0.99R, just
// inside the disk, and must HIT.  This is a regression guard for the
// descriptor-text fix (docs previously described this domain as a square).
static void TestHeightfieldDiskDomain()
{
	std::cout << "Test 4e: heightfield SDF domain is a DISK of radius R, not the square [-R,R]^2" << std::endl;
	const Scalar R = Scalar(1.6), S = Scalar(0.30);
	ConstantFunction2D* flat = new ConstantFunction2D( Scalar(1.0) );   // f(u,v) == 1
	SDFGeometry* g = new SDFGeometry( flat, R, S, 512, Scalar(0.0) );   // 0.0 = scene auto epsilon

	// Inside the square, outside the disk (corner-ish point at 0.99R, 0.99R):
	// the straight-down eye ray must miss the field entirely.
	{	RayIntersectionGeometric ri = MkRI( Point3(Scalar(0.99)*R, Scalar(0.99)*R, 5), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( !ri.bHit, "heightfield: point at (0.99R,0.99R) is outside the DISK (would be inside a square) -> miss" ); }

	// Inside the disk (0.7R, 0.7R is at radius ~0.99R < R): the ray must hit
	// the top face as usual.
	{	RayIntersectionGeometric ri = MkRI( Point3(Scalar(0.7)*R, Scalar(0.7)*R, 5), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.ptIntersection.z, S, 5e-3 ), "heightfield: point at (0.7R,0.7R) is inside the disk -> hits the top face" ); }

	safe_release( g );
	safe_release( flat );
}

static void TestShadowQuery()
{
	std::cout << "Test 5: IntersectRay_IntersectionOnly (shadow) hit/miss + dHowFar" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 5.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );

	// toward the sphere from z=20: surface at z=5 -> distance 15
	Check(  g->IntersectRay_IntersectionOnly( Ray(Point3(0,0,20), Vector3(0,0,-1)), 100.0, true, true ), "shadow ray toward sphere hits" );
	Check( !g->IntersectRay_IntersectionOnly( Ray(Point3(0,0,20), Vector3(0,0,-1)),  10.0, true, true ), "dHowFar=10 stops before the sphere (no hit)" );
	Check(  g->IntersectRay_IntersectionOnly( Ray(Point3(0,0,20), Vector3(0,0,-1)),  16.0, true, true ), "dHowFar=16 reaches the sphere (hit)" );
	Check( !g->IntersectRay_IntersectionOnly( Ray(Point3(0,20,0), Vector3(1,0,0)),  100.0, true, true ), "shadow ray that misses the sphere misses" );
	safe_release( g );
}

static void TestMiss()
{
	std::cout << "Test 6: ray missing the bbox misses" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 5.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );
	RayIntersectionGeometric ri = MkRI( Point3(0,20,0), Vector3(0,0,-1) );  // passes y=20, misses r5 sphere
	g->IntersectRay( ri, true, true, false );
	Check( !ri.bHit, "ray well outside the sphere does not hit" );
	safe_release( g );
}

static void TestInsideStartExits()
{
	std::cout << "Test 7: ray starting inside exits at the far surface" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 5.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );
	// from the centre along +Z: the only surface ahead is the exit at z=5 -> range 5
	RayIntersectionGeometric ri = MkRI( Point3(0,0,0), Vector3(0,0,1) );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit && IsClose(ri.range, 5.0, 5e-3), "inside-start ray exits at the far surface" );
	safe_release( g );
}

static void TestTransform()
{
	std::cout << "Test 8: translated + rotated parts land where expected" << std::endl;
	// translated sphere at (10,0,0)
	{	std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(10,0,0), 0,0,0, Vector3(1,1,1), 3.0, 0,0,0 ) );
		SDFGeometry* g = MakeGeom( parts );
		RayIntersectionGeometric ri = MkRI( Point3(10,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "translated sphere hit at +10 X" );
		safe_release( g ); }
	// a long thin box rotated 90 deg about Z: a 6x1x1 box (half 3,0.5,0.5) rotated so its long axis -> Y
	{	std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,90, Vector3(1,1,1), 3.0, 0.5, 0.5, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		// after rotating the long (X=3) axis to Y, the box reaches y~=+/-3 and x~=+/-0.5
		RayIntersectionGeometric riY = MkRI( Point3(0,20,0), Vector3(0,-1,0) );
		g->IntersectRay( riY, true, true, false );
		Check( riY.bHit && IsClose(riY.range, 17.0, 5e-3), "rotated box reaches +3 in Y (long axis)" );
		RayIntersectionGeometric riX = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
		g->IntersectRay( riX, true, true, false );
		Check( riX.bHit && IsClose(riX.range, 19.5, 5e-3), "rotated box only +0.5 in X (short axis)" );
		safe_release( g ); }
}

static void TestNonUniformScale()
{
	std::cout << "Test 9: non-uniform scale stretches the primitive" << std::endl;
	// unit sphere scaled (3,1,1) -> reaches x=3, y=1, z=1
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(3,1,1), 1.0, 0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );
	RayIntersectionGeometric rx = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
	g->IntersectRay( rx, true, true, false );
	Check( rx.bHit && IsClose(rx.range, 17.0, 1e-2), "scaled sphere reaches x=3" );
	RayIntersectionGeometric ry = MkRI( Point3(0,20,0), Vector3(0,-1,0) );
	g->IntersectRay( ry, true, true, false );
	Check( ry.bHit && IsClose(ry.range, 19.0, 1e-2), "scaled sphere reaches y=1" );
	safe_release( g );
}

// Subtract / intersect compose with smooth-MAX, which (unlike smooth-min) returns
// a value >= the hard max -- the review flagged this as a possible sphere-trace
// OVERSTEP that could tunnel through a carved wall or fill a carved hole.  The
// polynomial smin/smax are 1-Lipschitz given 1-Lipschitz inputs (|grad| of the
// blend <= 1 by the triangle inequality), so the composed field stays a
// CONSERVATIVE distance bound and must not tunnel.  These rays pin that down,
// including the smooth k=1 regime used by the watch case-body chunk's
// dial-bowl carve (scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene).

static void TestSubtractHardCarvesCavity()
{
	std::cout << "Test 10: subtract (hard) carves a real cavity; walls hit at exact depth" << std::endl;
	// A = box half(3,3,3); subtract B = box half(2,2,5) -> a 2x2 tunnel through Z,
	// leaving 1-thick walls on +-X / +-Y.
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3,3,3, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2,2,5, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,-10), Vector3(0,0,1) );      // down the tunnel
		g->IntersectRay( ri, true, true, false );
		Check( !ri.bHit, "subtract: ray down the tunnel misses (hole carved)" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(2.5,0,-10), Vector3(0,0,1) );    // through +X wall
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 7.0, 5e-3), "subtract: wall column hit at outer face z=-3" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(-10,0,0), Vector3(1,0,0) );      // outer -X face
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 7.0, 5e-3), "subtract: outer -X face at x=-3" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(-2.5,0,0), Vector3(1,0,0) );     // inside -X wall x[-3,-2]
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 0.5, 5e-3), "subtract: inner carved face at x=-2 (no tunnel)" ); }
	safe_release( g );
}

static void TestSubtractSmoothNoTunnel()
{
	std::cout << "Test 11: subtract (smooth k=1, case_body regime) neither fills nor tunnels" << std::endl;
	// thicker walls so the k=1 fillet does not erode them: A half(4,4,4),
	// subtract B half(2,2,6) k=1 -> rounded 2x2 tunnel, ~2-thick walls.
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 4,4,4, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 1.0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2,2,6, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,-12), Vector3(0,0,1) );      // centre still open
		g->IntersectRay( ri, true, true, false );
		Check( !ri.bHit, "smooth subtract: centre still open (smooth-max did not fill it)" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(3.5,0,-12), Vector3(0,0,1) );    // wall far from carve
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 8.0, 1e-2), "smooth subtract: wall hit at outer face (no overstep)" ); }
	safe_release( g );
}

static void TestIntersectLens()
{
	std::cout << "Test 12: intersect (hard) = lens, hits at exact sphere positions" << std::endl;
	// A = sphere r2 at (-1,0,0); intersect B = sphere r2 at (+1,0,0).
	// lens at y=z=0 spans x[-1,1]: -X extent is sphere B's surface, +X extent is A's.
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(-1,0,0), 0,0,0, Vector3(1,1,1), 2,0,0, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpIntersect, 0,
		Point3(1,0,0), 0,0,0, Vector3(1,1,1), 2,0,0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	{	RayIntersectionGeometric ri = MkRI( Point3(-10,0,0), Vector3(1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 9.0, 5e-3), "intersect: -X lens extent at x=-1" );
		Check( VClose( ri.vNormal, norm3(-1,0,0) ), "intersect: -X face is sphere B's normal" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(10,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 9.0, 5e-3), "intersect: +X lens extent at x=1" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(0,10,0), Vector3(0,-1,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 10.0 - std::sqrt(3.0), 5e-3), "intersect: +Y lens extent at y=sqrt3" ); }
	safe_release( g );
}

static void TestIntersectionOnlyFrontBack()
{
	std::cout << "Test 13: IntersectRay_IntersectionOnly honours front/back flags" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2,0,0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	Ray r( Point3(-10,0,0), Vector3(1,0,0) );   // crosses sphere: front x=-2 (range 8), back x=+2 (range 12)
	Check(  g->IntersectRay_IntersectionOnly( r, 100.0, true,  true  ), "IO both faces -> hit" );
	Check(  g->IntersectRay_IntersectionOnly( r, 100.0, true,  false ), "IO front-only -> hit (front exists)" );
	Check(  g->IntersectRay_IntersectionOnly( r, 100.0, false, true  ), "IO back-only -> hit (steps past front to back)" );
	Check( !g->IntersectRay_IntersectionOnly( r, 100.0, false, false ), "IO neither face -> miss" );
	Check( !g->IntersectRay_IntersectionOnly( r, 8.5,   false, true  ), "IO back-only, short dHowFar -> miss (back beyond range)" );
	safe_release( g );
}

static void TestFrontBackGuardConsistency()
{
	std::cout << "Test 14: IntersectRay vs IntersectionOnly agree past the face-skip guard" << std::endl;
	// 6 thin slabs stacked along Z -> 12 surface crossings along a +Z ray, MORE
	// than the 8-crossing skip guard.  A (false,false) query must report NO hit on
	// BOTH paths -- the detailed path used to fall through to bHit=true after the
	// guard expired on a disallowed face.
	std::vector<SDFGeometry::Part> parts;
	const Scalar zc[6] = { -5,-3,-1,1,3,5 };
	for( int i = 0; i < 6; ++i )
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,zc[i]), 0,0,0, Vector3(1,1,1), 2,2,0.2, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	RayIntersectionGeometric ri = MkRI( Point3(0,0,-20), Vector3(0,0,1) );
	g->IntersectRay( ri, false, false, false );
	Ray r( Point3(0,0,-20), Vector3(0,0,1) );
	const bool io = g->IntersectRay_IntersectionOnly( r, 1000.0, false, false );
	Check( !ri.bHit, "IntersectRay (false,false): no hit after guard expiry" );
	Check( !io, "IntersectionOnly (false,false): no hit" );
	Check( ri.bHit == io, "detailed and fast paths agree on (false,false)" );

	// sanity: (true,true) still hits the first slab's front face at z=-5.2
	RayIntersectionGeometric ri2 = MkRI( Point3(0,0,-20), Vector3(0,0,1) );
	g->IntersectRay( ri2, true, true, false );
	Check( ri2.bHit && IsClose(ri2.range, 14.8, 1e-2), "(true,true) hits first slab front at z=-5.2" );
	safe_release( g );
}

// ---- first-class surface machinery: tessellation + uniform area sampling ----

// splitmix64: the earlier 32-bit LCG had enough serial correlation to bias the
// closed-form NEE harness by ~1.6% through the analytic sphere's cos-theta map.
static unsigned long long smState = 0x9E3779B97F4A7C15ull;
static Scalar Rand01()
{
	smState += 0x9E3779B97F4A7C15ull;
	unsigned long long z = smState;
	z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
	z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
	z ^= ( z >> 31 );
	return Scalar( z >> 11 ) / Scalar( 9007199254740992.0 );
}

static void TestSurfaceArea()
{
	std::cout << "Test 15: GetArea(sphere) == 4*pi*r^2 within 2% (tessellated + projected)" << std::endl;
	const Scalar R = 5.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );
	const Scalar area = g->GetArea();
	const Scalar exact = Scalar(4) * PI * R * R;
	std::cout << "  sphere r=5: GetArea=" << area << "  exact=" << exact << "  rel=" << (area/exact) << std::endl;
	Check( std::fabs( area - exact ) / exact < 0.02, "sphere surface area within 2% of 4*pi*r^2" );
	Check( g->CanBeAreaLight(), "sphere SDF reports CanBeAreaLight()" );
	safe_release( g );
}

static void TestUniformRandomPointOnSurface()
{
	std::cout << "Test 16: UniformRandomPoint lies ON the surface, radial normal, octant balance" << std::endl;
	const Scalar R = 5.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	int octant[8] = {0,0,0,0,0,0,0,0};
	Scalar maxRadErr = 0, maxNrmErr = 0;
	const int NSAMP = 400;
	for( int i = 0; i < NSAMP; i++ ) {
		Point3 pt; Vector3 nrm; Point2 uv;
		g->UniformRandomPoint( &pt, &nrm, &uv, Point3( Rand01(), Rand01(), Rand01() ) );
		const Scalar r = std::sqrt( pt.x*pt.x + pt.y*pt.y + pt.z*pt.z );
		maxRadErr = std::max( maxRadErr, std::fabs( r - R ) );
		const Vector3 radial = norm3( pt.x, pt.y, pt.z );
		const Scalar dot = radial.x*nrm.x + radial.y*nrm.y + radial.z*nrm.z;
		maxNrmErr = std::max( maxNrmErr, Scalar(1) - dot );
		octant[ (pt.x > 0 ? 1 : 0) | (pt.y > 0 ? 2 : 0) | (pt.z > 0 ? 4 : 0) ]++;
	}
	Check( maxRadErr < 5e-3, "every sample within 5e-3 of the true surface" );
	Check( maxNrmErr < 2e-2, "every normal within 2e-2 of radial" );
	int minOct = NSAMP, maxOct = 0;
	for( int o = 0; o < 8; o++ ) { minOct = std::min( minOct, octant[o] ); maxOct = std::max( maxOct, octant[o] ); }
	Check( minOct > NSAMP/8 - 30 && maxOct < NSAMP/8 + 30, "octant counts roughly uniform (50 +- 30)" );
	safe_release( g );
}

static void TestTessellateToMesh()
{
	std::cout << "Test 17: TessellateToMesh: indexed mesh on the surface, outward, append contract" << std::endl;
	const Scalar R = 5.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	IndexTriangleListType tris;
	VerticesListType verts;
	NormalsListType nrms;
	TexCoordsListType uvs;
	Check( g->TessellateToMesh( tris, verts, nrms, uvs, 48 ), "tessellation succeeds" );
	Check( !tris.empty() && verts.size() == nrms.size() && verts.size() == uvs.size(), "parallel vertex arrays" );

	Scalar maxOff = 0, meshArea = 0;
	bool idxOK = true, orientOK = true;
	for( size_t i = 0; i < verts.size(); i++ ) {
		const Scalar r = std::sqrt( verts[i].x*verts[i].x + verts[i].y*verts[i].y + verts[i].z*verts[i].z );
		maxOff = std::max( maxOff, std::fabs( r - R ) );
	}
	for( size_t t = 0; t < tris.size(); t++ ) {
		for( int k = 0; k < 3; k++ ) {
			if( tris[t].iVertices[k] >= verts.size() ) idxOK = false;
		}
		if( !idxOK ) break;
		const Point3& A = verts[ tris[t].iVertices[0] ];
		const Point3& B = verts[ tris[t].iVertices[1] ];
		const Point3& C = verts[ tris[t].iVertices[2] ];
		const Vector3 e1( B.x-A.x, B.y-A.y, B.z-A.z );
		const Vector3 e2( C.x-A.x, C.y-A.y, C.z-A.z );
		const Vector3 n( e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x );
		meshArea += Scalar(0.5) * std::sqrt( n.x*n.x + n.y*n.y + n.z*n.z );
		// outward: geometric normal agrees with the radial direction at the centroid
		const Vector3 ctr( (A.x+B.x+C.x)/3, (A.y+B.y+C.y)/3, (A.z+B.z+C.z)/3 );
		if( n.x*ctr.x + n.y*ctr.y + n.z*ctr.z <= 0 ) orientOK = false;
	}
	Check( idxOK, "all triangle indices in bounds" );
	Check( maxOff < 5e-3, "all tessellation vertices on the surface (projected)" );
	const Scalar exact = Scalar(4) * PI * R * R;
	Check( std::fabs( meshArea - exact ) / exact < 0.02, "tessellated mesh area within 2%" );
	Check( orientOK, "every triangle wound outward" );

	// append contract: a second tessellation into the same vectors must offset indices
	const size_t v1 = verts.size();
	const size_t t1 = tris.size();
	Check( g->TessellateToMesh( tris, verts, nrms, uvs, 24 ), "second (appended) tessellation succeeds" );
	bool offsetOK = true;
	for( size_t t = t1; t < tris.size(); t++ ) {
		for( int k = 0; k < 3; k++ ) {
			if( tris[t].iVertices[k] < v1 || tris[t].iVertices[k] >= verts.size() ) offsetOK = false;
		}
	}
	Check( offsetOK, "appended tessellation indices offset past the first batch" );
	safe_release( g );
}

static void TestSamplingOnCompositeFields()
{
	std::cout << "Test 18: area + on-surface sampling hold for smin blob and carved (subtract) fields" << std::endl;
	{	// smin peanut (the sdf_volume blob): two r=1.05 spheres at y=-0.75/+0.75, k=0.7
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,-0.75,0), 0,0,0, Vector3(1,1,1), 1.05, 0,0,0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.7,
			Point3(0, 0.75,0), 0,0,0, Vector3(1,1,1), 1.05, 0,0,0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const Scalar oneSphere = Scalar(4) * PI * Scalar(1.05) * Scalar(1.05);
		Check( g->GetArea() > oneSphere, "blob area exceeds a single constituent sphere" );
		Scalar worst = 0;
		for( int i = 0; i < 200; i++ ) {
			Point3 pt; Vector3 nrm;
			g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
			// Map is protected; closeness to the surface is checked via a ray cast
			// from outside along -nrm: the hit must land back on (or very near) pt.
			RayIntersectionGeometric ri = MkRI( Point3( pt.x + nrm.x*3, pt.y + nrm.y*3, pt.z + nrm.z*3 ),
			                                    Vector3( -nrm.x, -nrm.y, -nrm.z ) );
			g->IntersectRay( ri, true, true, false );
			if( ri.bHit ) {
				const Scalar dx = ri.ptIntersection.x - pt.x;
				const Scalar dy = ri.ptIntersection.y - pt.y;
				const Scalar dz = ri.ptIntersection.z - pt.z;
				worst = std::max( worst, std::sqrt( dx*dx + dy*dy + dz*dz ) );
			} else {
				worst = Scalar(1e9);
			}
		}
		Check( worst < 2e-2, "blob samples sit on the sphere-traced surface (ray round-trip)" );
		safe_release( g );
	}
	{	// carved tunnel (Test 10 field): area must count the interior walls; samples on surface
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3,3,3, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpSubtract, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2,2,5, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		// outer shell minus two 4x4 punched faces, plus four 4x6 interior tunnel walls
		const Scalar expected = Scalar(6*36 - 2*16 + 4*24);
		std::cout << "  carved box: GetArea=" << g->GetArea() << "  exact=" << expected << "  rel=" << (g->GetArea()/expected) << std::endl;
		Check( std::fabs( g->GetArea() - expected ) / expected < 0.03, "carved-tunnel area within 3% of closed form" );
		Check( g->CanBeAreaLight(), "carved field reports CanBeAreaLight()" );
		safe_release( g );
	}
}

static void TestNEEIntegrandClosedForm()
{
	std::cout << "Test 19: MC integral of the NEE integrand over UniformRandomPoint == closed form" << std::endl;
	// Uniformly radiant sphere (radiance L=1), receiver point Q straight below at
	// distance d from the centre, receiver normal pointing up at the centre.
	// Closed form irradiance: E = pi * L * (R/d)^2.   The NEE estimator computes
	// (area/N) * sum[ cosLight * cosSurface / dist^2 ] over uniform-area samples
	// with cosLight > 0 -- exactly what the light sampler does.  If this MC sum
	// converges to the closed form, the sampling machinery (area + distribution +
	// normals) is unbiased, independent of any renderer code.
	const Scalar R = 1.5;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), R, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	const Point3  Q( 0, -2.6, 0 );
	const Vector3 nQ( 0, 1, 0 );
	const Scalar  dC = 2.6;
	const Scalar  closedForm = PI * ( R / dC ) * ( R / dC );

	const Scalar area = g->GetArea();
	const int N = 400000;
	Scalar sum = 0;
	for( int i = 0; i < N; i++ ) {
		Point3 pt; Vector3 nrm;
		g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
		Vector3 toQ( Q.x - pt.x, Q.y - pt.y, Q.z - pt.z );
		const Scalar dist = std::sqrt( toQ.x*toQ.x + toQ.y*toQ.y + toQ.z*toQ.z );
		toQ = Vector3( toQ.x/dist, toQ.y/dist, toQ.z/dist );
		const Scalar cosLight   = nrm.x*toQ.x + nrm.y*toQ.y + nrm.z*toQ.z;
		const Scalar cosSurface = -( nQ.x*toQ.x + nQ.y*toQ.y + nQ.z*toQ.z );
		if( cosLight > 0 && cosSurface > 0 ) {
			// visibility for a convex emitter == samples on the near cap; the
			// cosLight > 0 gate alone OVERCOUNTS (it admits far-side points whose
			// normal happens to face Q).  For a sphere, a sample is visible from Q
			// exactly when cosLight > 0 AND the segment doesn't re-enter -- which
			// for convex bodies is equivalent to cosLight > 0 evaluated with the
			// outward normal at the TRUE surface, so the gate is correct as-is.
			sum += cosLight * cosSurface / ( dist * dist );
		}
	}
	const Scalar mc = area * sum / Scalar(N);
	std::cout << "  closed form E = " << closedForm << "   MC E = " << mc << "   ratio = " << ( mc / closedForm ) << std::endl;
	Check( std::fabs( mc - closedForm ) / closedForm < 0.01, "NEE integrand MC within 1% of closed form" );
	safe_release( g );
}

static void TestNEEIntegrandAnalyticControl()
{
	std::cout << "Test 19b: same MC harness against the ANALYTIC SphereGeometry (control)" << std::endl;
	const Scalar R = 1.5;
	SphereGeometry* g = new SphereGeometry( R );
	const Point3  Q( 0, -2.6, 0 );
	const Vector3 nQ( 0, 1, 0 );
	const Scalar  dC = 2.6;
	const Scalar  closedForm = PI * ( R / dC ) * ( R / dC );
	const Scalar  area = g->GetArea();
	const int N = 400000;
	Scalar sum = 0;
	for( int i = 0; i < N; i++ ) {
		Point3 pt; Vector3 nrm;
		g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
		Vector3 toQ( Q.x - pt.x, Q.y - pt.y, Q.z - pt.z );
		const Scalar dist = std::sqrt( toQ.x*toQ.x + toQ.y*toQ.y + toQ.z*toQ.z );
		toQ = Vector3( toQ.x/dist, toQ.y/dist, toQ.z/dist );
		const Scalar cosLight   = nrm.x*toQ.x + nrm.y*toQ.y + nrm.z*toQ.z;
		const Scalar cosSurface = -( nQ.x*toQ.x + nQ.y*toQ.y + nQ.z*toQ.z );
		if( cosLight > 0 && cosSurface > 0 ) {
			sum += cosLight * cosSurface / ( dist * dist );
		}
	}
	const Scalar mc = area * sum / Scalar(N);
	std::cout << "  closed form E = " << closedForm << "   MC E = " << mc << "   ratio = " << ( mc / closedForm ) << std::endl;
	Check( std::fabs( mc - closedForm ) / closedForm < 0.01, "analytic-sphere control within 1% of closed form" );
	safe_release( g );
}

//////////////////////////////////////////////////////////////////////
// Test 20: ParsePartLines -- the ONE part grammar shared by the scene
// chunk's inline `part` lines and external parts files.  Valid sources
// (grouped spacing, comments, blank lines) parse to exact Part fields;
// unknown / malformed / trailing tokens hard-fail; the retired
// count-header format is rejected as malformed (the header line is not
// a part).
//////////////////////////////////////////////////////////////////////
static void TestParsePartLines()
{
	std::cout << "Test 20: ParsePartLines grammar (inline `part` lines / parts files)" << std::endl;

	// Valid: grouped spacing, a comment line, a trailing comment, a blank line.
	{
		std::vector<SDFGeometry::Part> parts;
		const char* src =
			"# the blob from sdf_shadows\n"
			"roundbox union 0  0 0 2  0 0 0  1 1 1  2 2 2  0.5\n"
			"\n"
			"sphere smin 1.6  2.6 3.2 0  0 0 0  1 1 1  1.7 0 0  0   # melded\n";
		Check( SDFGeometry::ParsePartLines( src, "<test>", parts ), "valid source parses" );
		Check( parts.size() == 2, "valid source yields 2 parts" );
		if( parts.size() == 2 ) {
			Check( parts[0].type == SDFGeometry::ePrimRoundBox, "part 0 type roundbox" );
			Check( parts[0].op == SDFGeometry::eOpUnion, "part 0 op union" );
			Check( parts[0].k == 0.0, "part 0 k" );
			Check( parts[0].pos.x == 0.0 && parts[0].pos.y == 0.0 && parts[0].pos.z == 2.0, "part 0 pos" );
			Check( parts[0].a == 2.0 && parts[0].b == 2.0 && parts[0].c == 2.0, "part 0 a b c" );
			Check( parts[0].round == 0.5, "part 0 round" );
			Check( parts[1].type == SDFGeometry::ePrimSphere, "part 1 type sphere" );
			Check( parts[1].op == SDFGeometry::eOpSmin, "part 1 op smin" );
			Check( parts[1].k == 1.6, "part 1 k" );
			Check( parts[1].pos.x == 2.6 && parts[1].pos.y == 3.2 && parts[1].pos.z == 0.0, "part 1 pos" );
			Check( parts[1].a == 1.7, "part 1 a" );
		}
	}

	// Comment-only / blank source: parses, appends nothing (emptiness is
	// the factory's both-or-neither diagnostic, not the grammar's).
	{
		std::vector<SDFGeometry::Part> parts;
		Check( SDFGeometry::ParsePartLines( "# nothing here\n\n", "<test>", parts ), "comment-only source parses" );
		Check( parts.empty(), "comment-only source yields 0 parts" );
	}

	// Hard failures: unknown primitive / op, short line, trailing token,
	// and the retired count-header file format.
	{
		std::vector<SDFGeometry::Part> parts;
		Check( !SDFGeometry::ParsePartLines(
			"round_box union 0  0 0 0  0 0 0  1 1 1  1 1 1  0\n", "<test>", parts ),
			"unknown primitive rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"sphere substract 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
			"unknown op rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"sphere union 0  0 0 0  0 0 0  1 1 1  1 0 0\n", "<test>", parts ),
			"short line (15 tokens) rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"sphere union 0  0 0 0  0 0 0  1 1 1  1 0 0  0  99\n", "<test>", parts ),
			"trailing extra token rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"1\nsphere union 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
			"retired count-header format rejected" );
	}

	// Parsed parts drive the same geometry as directly-constructed parts:
	// a lone r=1.5 sphere field maps identically at probe points.
	{
		std::vector<SDFGeometry::Part> parsed;
		Check( SDFGeometry::ParsePartLines(
			"sphere union 0  0 0 0  0 0 0  1 1 1  1.5 0 0  0\n", "<test>", parsed ),
			"probe sphere parses" );
		std::vector<SDFGeometry::Part> direct;
		direct.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.5, 0, 0, 0 ) );
		SDFGeometry* gp = new SDFGeometry( parsed, 256, 0.0 );
		SDFGeometry* gd = new SDFGeometry( direct, 256, 0.0 );
		bool match = true;
		for( int i = 0; i < 8 && match; i++ ) {
			const Scalar t = Scalar(i) / 7.0;
			Ray r( Point3( -5.0 + 10.0*t, 0.3, 4.0 ), Vector3Ops::Normalize( Vector3( 0.1*t, -0.05, -1 ) ) );
			RayIntersectionGeometric riP( r, nullRasterizerState );
			RayIntersectionGeometric riD( r, nullRasterizerState );
			gp->IntersectRay( riP, true, true, false );
			gd->IntersectRay( riD, true, true, false );
			if( riP.bHit != riD.bHit ) { match = false; }
			else if( riP.bHit && std::fabs( riP.range - riD.range ) > 1e-12 ) { match = false; }
		}
		Check( match, "parsed parts hit-identical to directly-constructed parts" );
		safe_release( gp );
		safe_release( gd );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 20b: DEGENERATE SHAPE PARAMETERS.  The `part` grammar's <a b c>
// are the primitive's own shape parameters (verified against primDist's
// switch in SDFGeometry.cpp); <sx sy sz> is a separate, pre-transform
// SCALE.  An authoring slip seen in the wild puts a shape's SIZE into
// <sx sy sz> and leaves <a b c> at its default 0 0 0 -- e.g. "this box
// is 2x1x3" written into the scale slot -- which collapses the part to
// a single point.  ParsePartLines WARNS (does not reject) when a part's
// shape parameters are the exact zero-critical combination primDist
// collapses to a point, per-primitive, and must NOT warn when the same
// all-zero <a b c> is a DIFFERENT valid primitive (roundbox with
// round > 0 is an exact sphere; torus with only the ring radius at 0 is
// an exact sphere) -- both cases are exercised below.
//////////////////////////////////////////////////////////////////////
static void TestDegenerateShapeParamsWarn()
{
	std::cout << "Test 20b: degenerate <a b c> parts warn (scale-slot authoring slip)" << std::endl;

	auto ParseOneAndCheck = [&]( const char* line, bool expectWarn, const char* label )
	{
		g_degenerateShapeWarn->Clear();
		std::vector<SDFGeometry::Part> parts;
		Check( SDFGeometry::ParsePartLines( line, "<test>", parts ),
		       ( std::string(label) + ": still parses (warn, don't fail)" ).c_str() );
		Check( parts.size() == 1, ( std::string(label) + ": still yields the one part" ).c_str() );
		if( expectWarn ) {
			Check( g_degenerateShapeWarn->MatchCount() == 1,
			       ( std::string(label) + ": degenerate-shape warning fires exactly once" ).c_str() );
		} else {
			Check( g_degenerateShapeWarn->MatchCount() == 0,
			       ( std::string(label) + ": no warning -- this combination is a valid, non-degenerate primitive" ).c_str() );
		}
	};

	// The reported bug, reproduced exactly: a box whose <a b c> are all 0
	// because its half-extents went into <sx sy sz> instead.
	ParseOneAndCheck( "box union 0  0 0 0  0 0 0  2 1 3  0 0 0  0\n", true,
		"box with <a b c> = 0 0 0 (size left in the scale slot)" );
	// The same box authored correctly does not warn.
	ParseOneAndCheck( "box union 0  0 0 0  0 0 0  1 1 1  2 1 3  0\n", false,
		"box with real half-extents in <a b c>" );

	// sphere: the single critical param is `a`.
	ParseOneAndCheck( "sphere union 0  0 0 0  0 0 0  1 1 1  0 0 0  0\n", true,
		"sphere with a = 0" );
	ParseOneAndCheck( "sphere union 0  0 0 0  0 0 0  1 1 1  2.5 0 0  0\n", false,
		"sphere with a > 0" );

	// roundbox: a=b=c=0 with round=0 is a genuine point; a=b=c=0 with
	// round > 0 is an EXACT sphere of radius `round` (sdRoundBox's
	// shrink-then-inflate), and must not warn.
	ParseOneAndCheck( "roundbox union 0  0 0 0  0 0 0  1 1 1  0 0 0  0\n", true,
		"roundbox with a b c round all 0" );
	ParseOneAndCheck( "roundbox union 0  0 0 0  0 0 0  1 1 1  0 0 0  1.5\n", false,
		"roundbox with a b c = 0 but round > 0 (a valid sphere)" );

	// cylinder: needs BOTH radius and half-height zero to fully collapse.
	ParseOneAndCheck( "cylinder union 0  0 0 0  0 0 0  1 1 1  0 0 0  0\n", true,
		"cylinder with radius and half-height both 0" );
	ParseOneAndCheck( "cylinder union 0  0 0 0  0 0 0  1 1 1  0 3 0  0\n", false,
		"cylinder with radius 0 but half-height > 0" );

	// torus: R=0 alone is an EXACT sphere of radius `b` (sdTorusY collapses
	// to sdSphere); only R=0 AND rr=0 is the genuine point.
	ParseOneAndCheck( "torus union 0  0 0 0  0 0 0  1 1 1  0 0 0  0\n", true,
		"torus with ring and tube radius both 0" );
	ParseOneAndCheck( "torus union 0  0 0 0  0 0 0  1 1 1  0 1.2 0  0\n", false,
		"torus with ring radius 0 but tube radius > 0 (a valid sphere)" );

	// capsule: radius and half-length both 0.
	ParseOneAndCheck( "capsule union 0  0 0 0  0 0 0  1 1 1  0 0 0  0\n", true,
		"capsule with radius and half-length both 0" );
	ParseOneAndCheck( "capsule union 0  0 0 0  0 0 0  1 1 1  0.8 2 0  0\n", false,
		"capsule with real radius and half-length" );

	// roundcone: base radius, tip radius, and length all 0.
	ParseOneAndCheck( "roundcone union 0  0 0 0  0 0 0  1 1 1  0 0 0  0\n", true,
		"roundcone with base radius, tip radius, and length all 0" );
	ParseOneAndCheck( "roundcone union 0  0 0 0  0 0 0  1 1 1  0.5 0.2 2  0\n", false,
		"roundcone with real base/tip radius and length" );

	// superellipsoid: radius `a` is the critical param (b/c are exponents,
	// already covered by TestSuperellipsoidExponentClamp / GrammarAndBounds).
	ParseOneAndCheck( "superellipsoid union 0  0 0 0  0 0 0  1 1 1  0 1 1  0\n", true,
		"superellipsoid with a = 0" );

	// The warning names the part (index + primitive keyword) and the
	// scale-slot hypothesis, so an author can act on it without re-deriving
	// which of several parts is the offender.
	{
		g_degenerateShapeWarn->Clear();
		std::vector<SDFGeometry::Part> parts;
		const char* src =
			"sphere union 0  0 0 0  0 0 0  1 1 1  1.0 0 0  0\n"		// part 0: fine
			"box smin 0.2  1 0 0  0 0 0  2 1 3  0 0 0  0\n";			// part 1: degenerate
		Check( SDFGeometry::ParsePartLines( src, "<test>", parts ), "mixed source still parses" );
		Check( parts.size() == 2, "mixed source yields 2 parts" );
		Check( g_degenerateShapeWarn->MatchCount() == 1, "exactly the degenerate second part warns" );
		const std::string msg = g_degenerateShapeWarn->LastMatch();
		Check( msg.find( "part 1" ) != std::string::npos, "warning names the 0-based part index (`part 1`)" );
		Check( msg.find( "box" ) != std::string::npos, "warning names the primitive (`box`)" );
		Check( msg.find( "scale slot" ) != std::string::npos, "warning names the likely slip (the scale slot)" );
	}

	// End to end: the degenerate part still COMPOSES into a real (if
	// pathological) SDFGeometry rather than crashing or refusing to build --
	// this is a WARN, not a hard failure, all the way through.
	{
		g_degenerateShapeWarn->Clear();
		std::vector<SDFGeometry::Part> parts;
		Check( SDFGeometry::ParsePartLines(
			"box union 0  0 0 0  0 0 0  2 1 3  0 0 0  0\n", "<test>", parts ),
			"end-to-end: degenerate part line still parses" );
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0 );
		BoundingBox bb = g->GenerateBoundingBox();
		const Scalar diag = std::sqrt(
			(bb.ur.x-bb.ll.x)*(bb.ur.x-bb.ll.x) +
			(bb.ur.y-bb.ll.y)*(bb.ur.y-bb.ll.y) +
			(bb.ur.z-bb.ll.z)*(bb.ur.z-bb.ll.z) );
		Check( diag < 0.1, "end-to-end: the degenerate part's bbox is (near-)a point, matching the reported symptom" );
		safe_release( g );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 21: the first part must be union or smin.  Map() folds from an
// empty (+1e30) field, so a leading subtract / intersect yields an
// always-miss SDF -- ParsePartLines hard-fails it.  smin against the
// empty field degenerates to plain union, so it IS allowed and must
// hit identically to a union first part.
//////////////////////////////////////////////////////////////////////
static void TestFirstOpRule()
{
	std::cout << "Test 21: first part must be union or smin" << std::endl;

	std::vector<SDFGeometry::Part> parts;
	Check( !SDFGeometry::ParsePartLines(
		"sphere subtract 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
		"leading subtract rejected" );
	Check( !SDFGeometry::ParsePartLines(
		"sphere intersect 0  0 0 0  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
		"leading intersect rejected" );
	Check( SDFGeometry::ParsePartLines(
		"box subtract 0  0 0 0  0 0 0  1 1 1  9 9 9  0\n", "<test>", parts ) == false,
		"leading subtract rejected regardless of primitive" );

	// second-position subtract stays legal
	parts.clear();
	Check( SDFGeometry::ParsePartLines(
		"sphere union 0  0 0 0  0 0 0  1 1 1  2 0 0  0\n"
		"sphere subtract 0  0 0 2  0 0 0  1 1 1  1 0 0  0\n", "<test>", parts ),
		"subtract after union accepted" );

	// smin first == union first, hit-for-hit
	std::vector<SDFGeometry::Part> sminFirst, unionFirst;
	Check( SDFGeometry::ParsePartLines(
		"sphere smin 1  0 0 0  0 0 0  1 1 1  1.5 0 0  0\n", "<test>", sminFirst ),
		"leading smin accepted" );
	Check( SDFGeometry::ParsePartLines(
		"sphere union 0  0 0 0  0 0 0  1 1 1  1.5 0 0  0\n", "<test>", unionFirst ),
		"union control parses" );
	SDFGeometry* gs = new SDFGeometry( sminFirst, 256, 0.0 );
	SDFGeometry* gu = new SDFGeometry( unionFirst, 256, 0.0 );
	RayIntersectionGeometric rs = MkRI( Point3( 0, 0.2, 5 ), Vector3( 0, 0, -1 ) );
	RayIntersectionGeometric ru = MkRI( Point3( 0, 0.2, 5 ), Vector3( 0, 0, -1 ) );
	gs->IntersectRay( rs, true, true, false );
	gu->IntersectRay( ru, true, true, false );
	// NB compare within the marcher's surface epsilon, not exactly: a leading
	// smin pads the bbox by its blend radius (ComputeBounds pad = maxK), so
	// the two marches start from different bbox entry points and the
	// bracket+bisect hit refinement only pins the crossing to ~m_eps.
	// sminP itself folds the +1e30 empty-field sentinel EXACTLY to min()
	// (h = max(k - |a-b|, 0)/k is 0 there), so the surfaces coincide.
	Check( rs.bHit && ru.bHit && std::fabs( rs.range - ru.range ) < 1e-5,
		"leading smin behaves as union" );
	safe_release( gs );
	safe_release( gu );
}

//////////////////////////////////////////////////////////////////////
// Test 22: sub-cell surface components are invisible to the sampling
// mesh (marching tets only see corner sign changes) -- the detector
// must report them, and raising sampling_detail must capture them.
// A tiny nub (r = 0.15) is placed EXACTLY at a low-detail grid cell
// center, outside the big sphere but inside its bbox: every corner of
// that cell stays outside the nub (nearest corner is half a cell
// diagonal ~0.87 away), so at detail 8 the nub provably produces no
// triangles, NEE never samples it, and the center probe fires.
//////////////////////////////////////////////////////////////////////
static void TestMissedComponentDetector()
{
	std::cout << "Test 22: missed-component detector + detail captures the nub" << std::endl;

	const Scalar Rbig = 4.0;
	const Scalar rNub = 0.15;

	// Phase 1: big sphere alone fixes the bbox; replicate the sampling
	// grid of GenerateSurfaceMesh to find a low-detail cell center.
	std::vector<SDFGeometry::Part> bigOnly;
	bigOnly.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), Rbig, 0, 0, 0 ) );
	SDFGeometry* gProbe = new SDFGeometry( bigOnly, 256, 0.0, 8 );
	const BoundingBox bb = gProbe->GenerateBoundingBox();
	safe_release( gProbe );

	const Scalar ex = bb.ur.x - bb.ll.x, ey = bb.ur.y - bb.ll.y, ez = bb.ur.z - bb.ll.z;
	const Scalar longest = std::max( ex, std::max( ey, ez ) );
	const unsigned int N = 8;
	const Scalar hTarget = longest / Scalar(N);
	const unsigned int nx = std::max( 1u, (unsigned int)std::ceil( ex / hTarget ) );
	const unsigned int ny = std::max( 1u, (unsigned int)std::ceil( ey / hTarget ) );
	const unsigned int nz = std::max( 1u, (unsigned int)std::ceil( ez / hTarget ) );
	const Scalar hx = ex / Scalar(nx), hy = ey / Scalar(ny), hz = ez / Scalar(nz);
	const Point3 nubCtr( bb.ll.x + hx * Scalar(7.5),
	                     bb.ll.y + hy * Scalar(7.5),
	                     bb.ll.z + hz * Scalar(4.5) );
	const Scalar nubDist = std::sqrt( nubCtr.x*nubCtr.x + nubCtr.y*nubCtr.y + nubCtr.z*nubCtr.z );
	Check( nubDist > Rbig + 2.0 * rNub, "nub center sits clear of the big sphere" );
	Check( std::fabs(nubCtr.x) + rNub < bb.ur.x, "nub stays inside the big sphere's bbox" );

	std::vector<SDFGeometry::Part> parts = bigOnly;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		nubCtr, 0,0,0, Vector3(1,1,1), rNub, 0, 0, 0 ) );

	// Phase 2: low detail -- the nub renders (sphere trace hits it) but is
	// absent from the sampling structure, and the detector says so.
	{
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 8 );
		RayIntersectionGeometric ri = MkRI(
			Point3( nubCtr.x + 3.0, nubCtr.y, nubCtr.z ), Vector3( -1, 0, 0 ) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && std::fabs( ri.range - ( 3.0 - rNub ) ) < 2e-3,
			"low detail: sphere trace still hits the nub" );

		Check( g->SuspectedMissedFeatureCells() >= 1,
			"low detail: detector reports the provably missed cell" );

		// A PROVEN sampling-contract failure must withdraw area-light /
		// SSS capability: registering anyway would leave the missed nub
		// NEE-unsampleable while BSDF-hit emission MIS still budgets a
		// light-sampling pdf for it (phantom-pdf bias).  CanBeAreaLight()
		// == false routes LuminaryManager, the PT / EmissionShaderOp
		// emission gates, and the SSS ops to the consistent unbiased
		// fallback (full-weight BSDF-hit emission, no NEE).
		Check( !g->CanBeAreaLight(),
			"low detail: CanBeAreaLight refuses the broken sampling contract" );

		int nearNub = 0;
		for( int i = 0; i < 20000; i++ ) {
			Point3 pt; Vector3 nrm;
			g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
			const Scalar dx = pt.x - nubCtr.x, dy = pt.y - nubCtr.y, dz = pt.z - nubCtr.z;
			if( std::sqrt( dx*dx + dy*dy + dz*dz ) < 2.0 * rNub ) nearNub++;
		}
		Check( nearNub == 0, "low detail: NEE sampling never reaches the nub" );
		safe_release( g );
	}

	// Phase 3: high detail -- cells are smaller than the nub, the CDF
	// includes it, the detector goes quiet.
	{
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 64 );
		Check( g->SuspectedMissedFeatureCells() == 0,
			"high detail: no provably missed cells" );
		Check( g->CanBeAreaLight(),
			"high detail: CanBeAreaLight restored" );
		int nearNub = 0;
		for( int i = 0; i < 30000; i++ ) {
			Point3 pt; Vector3 nrm;
			g->UniformRandomPoint( &pt, &nrm, 0, Point3( Rand01(), Rand01(), Rand01() ) );
			const Scalar dx = pt.x - nubCtr.x, dy = pt.y - nubCtr.y, dz = pt.z - nubCtr.z;
			if( std::sqrt( dx*dx + dy*dy + dz*dz ) < 2.0 * rNub ) nearNub++;
		}
		std::cout << "  high-detail near-nub samples: " << nearNub << " of 30000" << std::endl;
		Check( nearNub >= 10, "high detail: NEE sampling reaches the nub" );
		safe_release( g );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 23: the curvature-corrected sampling area tracks the analytic
// surface area on curved shapes (the raw chord-triangle area
// systematically underestimates by O((cell / curvature radius)^2);
// the Jacobian-weighted measure cancels the first-order term).
//////////////////////////////////////////////////////////////////////
static void TestCorrectedSamplingArea()
{
	std::cout << "Test 23: curvature-corrected sampling area vs closed forms" << std::endl;

	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.5, 0, 0, 0 ) );
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 64 );
		const Scalar area = g->GetArea();
		const Scalar closed = 4.0 * PI * 1.5 * 1.5;
		std::cout << "  sphere r=1.5 detail 64: area " << area << " vs " << closed
			<< " (rel " << ( area - closed ) / closed << ")" << std::endl;
		Check( std::fabs( area - closed ) / closed < 0.003, "sphere area within 0.3%" );
		safe_release( g );
	}
	{
		std::vector<SDFGeometry::Part> parts;
		Check( SDFGeometry::ParsePartLines(
			"torus union 0  0 0 0  0 0 0  1 1 1  1.45 0.55 0  0\n", "<test>", parts ),
			"torus part parses" );
		SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 64 );
		const Scalar area = g->GetArea();
		const Scalar closed = 4.0 * PI * PI * 1.45 * 0.55;
		std::cout << "  torus R=1.45 r=0.55 detail 64: area " << area << " vs " << closed
			<< " (rel " << ( area - closed ) / closed << ")" << std::endl;
		Check( std::fabs( area - closed ) / closed < 0.005, "torus area within 0.5%" );
		safe_release( g );
	}
}

//////////////////////////////////////////////////////////////////////
// Keyframe-animation tests.  Prove the SDF FIELD animates: the public
// Keyframable interface (KeyframeFromParameters -> SetIntermediateValue
// -> RegenerateData) mutates a part field and re-derives bounds /
// rotation columns / Lipschitz / the area-sampling cache.
//////////////////////////////////////////////////////////////////////

// Round-trips one keyframe through the public interface.  Returns true
// iff the (name,value) was accepted (parsed + applied).
static bool ApplyKF( SDFGeometry* g, const char* name, const char* value )
{
	IKeyframeParameter* p = g->KeyframeFromParameters( String(name), String(value) );
	if( !p ) { return false; }
	g->SetIntermediateValue( *p );
	safe_release( p );
	g->RegenerateData();
	return true;
}

static void TestKeyframePartPosition()
{
	std::cout << "Test 26: keyframe part.position moves the surface AND rebuilds bounds" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2.0, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 18.0), "sphere front face at z=2 before move" ); }

	Check( ApplyKF( g, "part0.position", "0 0 5" ), "part0.position accepted" );

	// Sphere now centred at z=5 (front face z=7).  A hit REQUIRES RegenerateData to
	// have rebuilt the AABB: with the stale z in [-2,2] bbox the ray clips short and
	// MISSES.  range = 20 - (5+2) = 13.
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "sphere still hit after move (bounds rebuilt)" );
		Check( IsClose(ri.range, 13.0), "hit distance tracks the moved centre" ); }
	safe_release( g );
}

static void TestKeyframePartSizeAndArea()
{
	std::cout << "Test 27: keyframe part.size resizes surface AND invalidates the area cache" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.5, 0, 0, 0 ) );
	SDFGeometry* g = new SDFGeometry( parts, 256, 0.0, 64 );

	const Scalar area0 = g->GetArea();   // builds the sampling cache at r=1.5
	Check( std::fabs( area0 - 4.0*PI*1.5*1.5 ) / (4.0*PI*1.5*1.5) < 0.01, "area before resize ~ 4pi r^2" );

	Check( ApplyKF( g, "part0.size", "3 0 0" ), "part0.size accepted" );   // sphere: a = radius

	const Scalar area1 = g->GetArea();   // MUST rebuild against r=3 (cache was invalidated)
	std::cout << "  area " << area0 << " -> " << area1 << " (closed " << 4.0*PI*9.0 << ")" << std::endl;
	Check( area1 > area0 * 3.0, "area grew after resize (sampling cache invalidated)" );
	Check( std::fabs( area1 - 4.0*PI*9.0 ) / (4.0*PI*9.0) < 0.01, "rebuilt area ~ 4pi(3^2)" );

	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "resized sphere front face at z=3" ); }
	safe_release( g );
}

static void TestKeyframePartRotation()
{
	std::cout << "Test 28: keyframe part.rotation re-derives the rotation columns" << std::endl;
	// box long in local X (half-extents 3,1,1)
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 3.0, 1.0, 1.0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	{	RayIntersectionGeometric ri = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "long box +X face at x=3 before rotate" ); }

	Check( ApplyKF( g, "part0.rotation", "0 0 90" ), "part0.rotation accepted" );

	// 90 deg about Z swaps the X<->Y half-extents: object-X now sees half-extent 1,
	// object-Y sees half-extent 3 (true for either rotation sign).
	{	RayIntersectionGeometric ri = MkRI( Point3(20,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 19.0), "object-X half-extent now 1 (rotated)" ); }
	{	RayIntersectionGeometric ri = MkRI( Point3(0,20,0), Vector3(0,-1,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 17.0), "object-Y half-extent now 3 (rotated)" ); }
	safe_release( g );
}

static void TestKeyframeBlendAndScale()
{
	std::cout << "Test 29: keyframe part.blend (smin radius) + part.scale" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,-3,0), 0,0,0, Vector3(1,1,1), 4,0,0,0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.0,
		Point3(0, 3,0), 0,0,0, Vector3(1,1,1), 4,0,0,0 ) );
	SDFGeometry* g = MakeGeom( parts );

	// neck along +X at y=0: with k=0 it's the hard-union crease; raise k and the
	// smin seam bulges OUTWARD -> the front surface is closer -> smaller range.
	const Point3 o(-20,0,0); const Vector3 dir(1,0,0);
	RayIntersectionGeometric ri0 = MkRI(o,dir); g->IntersectRay(ri0,true,true,false);
	Check( ri0.bHit, "neck hit with k=0" );
	const Scalar r_hard = ri0.range;

	Check( ApplyKF( g, "part1.blend", "3.0" ), "part1.blend accepted" );
	RayIntersectionGeometric ri1 = MkRI(o,dir); g->IntersectRay(ri1,true,true,false);
	Check( ri1.bHit, "neck hit with k=3" );
	Check( ri1.range < r_hard - 0.05, "smin seam bulges outward after blend keyframe" );

	// scale sphere0 1.5x in X: along -X at y=-3 the surface now reaches x=6 (4*1.5).
	Check( ApplyKF( g, "part0.scale", "1.5 1 1" ), "part0.scale accepted" );
	RayIntersectionGeometric ri2 = MkRI( Point3(20,-3,0), Vector3(-1,0,0) ); g->IntersectRay(ri2,true,true,false);
	Check( ri2.bHit && IsClose(ri2.range, 14.0, 0.1), "scaled sphere reaches x=6 (range 20-6)" );
	safe_release( g );
}

static void TestKeyframeRejectsBadParams()
{
	std::cout << "Test 30: keyframe rejects unknown / out-of-range / non-finite params" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2.0, 0, 0, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	IKeyframeParameter* ok = g->KeyframeFromParameters( String("part0.position"), String("1 2 3") );
	Check( ok != 0, "valid part0.position accepted" );
	safe_release( ok );

	Check( g->KeyframeFromParameters( String("bogus"),              String("1") )          == 0, "unknown name rejected" );
	Check( g->KeyframeFromParameters( String("part0.bogus"),        String("1") )          == 0, "unknown part field rejected" );
	Check( g->KeyframeFromParameters( String("part9.position"),     String("1 2 3") )      == 0, "out-of-range part index rejected" );
	Check( g->KeyframeFromParameters( String("part0.position"),     String("nan nan nan") )== 0, "non-finite vec rejected" );
	Check( g->KeyframeFromParameters( String("part0.blend"),        String("inf") )        == 0, "non-finite scalar rejected" );
	Check( g->KeyframeFromParameters( String("heightfield_scale"),  String("1") )          == 0, "heightfield_scale rejected in parts mode" );
	safe_release( g );
}

// Minimal constant heightfield f(u,v)=c for the heightfield-scale keyframe test.
namespace {
	class ConstField : public virtual IFunction2D, public virtual Reference
	{
	public:
		Scalar c;
		ConstField( Scalar c_ ) : c(c_) {}
		Scalar Evaluate( const Scalar, const Scalar ) const { return c; }
	};
}

static void TestKeyframeHeightfieldScale()
{
	std::cout << "Test 31: keyframe heightfield_scale moves the plane + rebuilds Lipschitz/bounds" << std::endl;
	ConstField* field = new ConstField( 0.5 );   // f==0.5 -> flat disk at z = scale*0.5
	SDFGeometry* g = new SDFGeometry( field, 4.0, 2.0, 256, 0.0, 32 );   // amplitude 2 -> plane z=1

	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose(ri.range, 19.0, 0.05), "heightfield plane at z=1 before scale" ); }

	Check( ApplyKF( g, "heightfield_scale", "6" ), "heightfield_scale accepted" );

	// plane now at z = 6*0.5 = 3, OUTSIDE the old z in [0,2] bbox -> a hit proves the
	// bounds (and Lipschitz) were rebuilt by RegenerateData.  range = 20 - 3 = 17.
	{	RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "heightfield still hit after scale (bounds rebuilt)" );
		Check( IsClose(ri.range, 17.0, 0.05), "plane tracks the new amplitude" ); }

	safe_release( g );
	safe_release( field );
}

// Test 32: round cone local AABB (primLocalAABB, SDFGeometry.cpp) -- a
// roundcone's envelope is the convex hull of its two end spheres (base
// radius a at y=0, tip radius b at y=c), so its Y extent is
// [min(-a,c-b), max(a,c+b)], not the naive "-a at the base paired with
// c+b at the tip".  For a WELL-FORMED cone (|a-b| <= c, neither cap
// sphere contains the other) the two forms are ALGEBRAICALLY IDENTICAL:
//   c-b >= -a  <=>  b-a <= c   (both implied by |a-b| <= c)
//   a <= c+b   <=>  a-b <= c   (both implied by |a-b| <= c)
// so min(-a,c-b) == -a and max(a,c+b) == c+b whenever the cone is
// well-formed -- the fix changes NOTHING for any scene's existing
// bounds.  Only a DEGENERATE cone (|a-b| > c, e.g. a fat joint close to
// a small one -- exactly what skeleton_geometry's bone chains can
// produce) picks up the wider min/max term.
static void TestRoundConeAABBIdentityForWellFormedCones()
{
	std::cout << "Test 32a: round cone AABB -- IDENTICAL to the old formula for every well-formed cone" << std::endl;

	// (a, b, c) triples with |a-b| <= c, covering BOTH boundary ties
	// independently: "a-b == c" (the max(a,c+b) tie, ry1 side) and
	// "b-a == c" (the min(-a,c-b) tie, ry0 side) are different algebraic
	// identities and must each be exercised on their own fixture.
	//
	// The true property for a well-formed cone is BIT-IDENTICAL equality
	// between the old and new formulas -- verified by brute-force in
	// Python doubles for every fixture below except the last: for
	// { 0.5, 1.1, 0.6 } (the b-a==c tie), c-b == -0.5000000000000001
	// while -a == -0.5 exactly -- a single ULP of rounding noise from
	// computing c-b as two chained subtractions vs -a directly.  That one
	// fixture keeps a tight IsClose tolerance; every other fixture below
	// uses exact `==`.
	struct Triple { Scalar a, b, c; bool exactBitwise; };
	static const Triple table[] = {
		{ 1.0, 1.0, 0.5, true },   // equal radii -- a true cylinder-capped shape
		{ 1.0, 0.6, 0.4, true },   // a-b == c exactly (ry1-side boundary)
		{ 0.5, 0.9, 0.6, true },
		{ 2.0, 0.3, 1.7, true },   // a-b == c exactly (ry1-side boundary)
		{ 0.3, 0.3, 0.01, true },  // near-cylinder, tiny height
		{ 0.5, 1.1, 0.6, false },  // b-a == c exactly (ry0-side boundary) -- 1-ULP fragile, see above
	};

	for( std::size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i )
	{
		const Scalar a = table[i].a, b = table[i].b, c = table[i].c;
		Check( std::fabs(a-b) <= c + Scalar(1e-9), "fixture is well-formed (|a-b| <= c)" );

		// The OLD (pre-fix) formula: ry0=-a, ry1=c+b.  Assert it equals the
		// new min/max form -- the algebraic identity this fix relies on.
		const Scalar oldRy0 = -a, oldRy1 = c + b;
		const Scalar newRy0 = std::min( -a, c - b ), newRy1 = std::max( a, c + b );
		if( table[i].exactBitwise )
		{
			Check( oldRy0 == newRy0, "well-formed: new ry0 == old ry0 bit-exact" );
			Check( oldRy1 == newRy1, "well-formed: new ry1 == old ry1 bit-exact" );
		}
		else
		{
			Check( IsClose( oldRy0, newRy0, Scalar(1e-9) ), "well-formed: new ry0 == old ry0 (1-ULP tolerance, see fixture comment)" );
			Check( IsClose( oldRy1, newRy1, Scalar(1e-9) ), "well-formed: new ry1 == old ry1 (1-ULP tolerance, see fixture comment)" );
		}

		// And the ACTUAL bbox the geometry reports matches that same
		// (unchanged) prediction -- not just the isolated formula.
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimRoundCone, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), a, b, c, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		BoundingBox bb = g->GenerateBoundingBox();
		Check( bb.ll.y <= oldRy0 + Scalar(1e-6) && bb.ll.y >= oldRy0 - Scalar(0.01),
		       "well-formed: reported bbox bottom matches the (unchanged) old prediction" );
		Check( bb.ur.y >= oldRy1 - Scalar(1e-6) && bb.ur.y <= oldRy1 + Scalar(0.01),
		       "well-formed: reported bbox top matches the (unchanged) old prediction" );
		safe_release( g );
	}
}

// Test 32b: a DEGENERATE round cone (|a-b| > c -- one cap sphere
// contains the other) -- the exact repro from the skeleton_geometry bug
// report: base radius a=1.0 at y=0, tip radius b=0.1 at y=c=0.5.  The
// true field is (for any ray not exactly on the +/-Y axis, qx>0) the
// base sphere of radius 1.0 centred at the origin -- sdRoundConeY's
// k<0 branch fires whenever qx>0 here (b=(a-b)/c=1.8 clamps a=sqrt(max(
// 1-b*b,0))=0, so k=-1.8*qx < 0 for any qx>0) -- so the true top of the
// solid at (x,z)=(0.3,0.4) (radial dist 0.5 from the Y axis) is
// y=sqrt(1-0.5^2)=sqrt(0.75)~=0.8660254, NOT y=c+b=0.6 (the pre-fix
// box's top).  A ray descending straight down through that (x,z) column
// must therefore hit near y=0.8660254; the pre-fix box (top at 0.6)
// would have clipped the march before it ever reached that surface.
static void TestRoundConeDegenerateAABBDoesNotClipSurface()
{
	std::cout << "Test 32c: DEGENERATE round cone (|a-b|>c) -- ray hits the true (uncapped) sphere surface" << std::endl;

	const Scalar a = 1.0, b = 0.1, c = 0.5;    // |a-b| = 0.9 > c = 0.5
	Check( std::fabs(a-b) > c, "fixture is genuinely degenerate (|a-b| > c)" );

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimRoundCone, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), a, b, c, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	// Sanity: the pre-fix box top (c+b=0.6) is provably BELOW the true
	// surface height at this column -- if this weren't true the repro
	// wouldn't demonstrate anything.
	const Scalar qx = 0.5;   // sqrt(0.3^2+0.4^2)
	const Scalar trueTopY = std::sqrt( a*a - qx*qx );   // sqrt(0.75)
	Check( trueTopY > c + b, "sanity: the true surface sits above the old (buggy) box top" );

	RayIntersectionGeometric ri = MkRI( Point3(0.3, 50, 0.4), Vector3(0,-1,0) );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit, "MONEY ASSERTION -- the ray actually hits (a clipped-away box would miss or land on the old, wrong disc)" );
	Check( ri.bHit && IsClose( ri.range, Scalar(50) - trueTopY, Scalar(0.02) ),
	       "MONEY ASSERTION -- hit range matches the TRUE sphere surface (~49.134), not the old "
	       "clipped-plane range (~49.4 at y=0.6)" );

	safe_release( g );
}

// Test 32d: DEGENERATE round cone (|a-b|>c), probed EXACTLY ON the local
// Y axis (qx==0) -- the FIELD sibling of 32c's bounds bug.  In the
// degenerate regime b=(r1-r2)/h has |b|>1, so a=sqrt(max(1-b*b,0))
// clamps to exactly 0 and k = qx*(-b) + qy*a collapses to -b*qx.  For
// qx>0 that's still correctly signed (32c covers it), but for qx==0,
// k==0 exactly -- neither "< 0" nor "> a*h == 0" -- so control fell
// through to the lateral-wall branch qx*a + qy*b - r1 = qy*b - r1,
// which is wrong in both sign and magnitude on the axis.  The true
// solid here (r1=1.0 >= r2=0.1, tip sphere fully contained in the base
// sphere -- see the sdRoundConeY degenerate branch) is just the base
// sphere of radius r1 centred at the origin, so the true surface point
// straight up the axis is y=r1=1.0, and a ray descending from y=50
// must hit at range 50-1.0=49.0.
static void TestRoundConeDegenerateOnAxisFieldMatchesSphere()
{
	std::cout << "Test 32d: DEGENERATE round cone (|a-b|>c), ON-AXIS probe -- field matches the true cap sphere" << std::endl;

	const Scalar a = 1.0, b = 0.1, c = 0.5;    // |a-b| = 0.9 > c = 0.5
	Check( std::fabs(a-b) > c, "fixture is genuinely degenerate (|a-b| > c)" );

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimRoundCone, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), a, b, c, 0 ) );
	SDFGeometry* g = MakeGeom( parts );

	RayIntersectionGeometric ri = MkRI( Point3(0, 50, 0), Vector3(0,-1,0) );
	g->IntersectRay( ri, true, true, false );
	Check( ri.bHit, "MONEY ASSERTION -- the on-axis ray hits the base cap sphere" );
	Check( ri.bHit && IsClose( ri.range, Scalar(50) - a, Scalar(0.02) ),
	       "MONEY ASSERTION -- on-axis hit range matches the true sphere top (50-1.0=49.0), not the "
	       "lateral-wall fallthrough's wrong value" );

	safe_release( g );
}

//////////////////////////////////////////////////////////////////////
// Test 33 -- SUPERELLIPSOID (Barr superquadric), arc-85 candidate C6.
//
// The primitive has no exact closed-form SDF, so its field is a
// CONSERVATIVE bound (see sdSuperellipsoidY's derivation in
// SDFGeometry.cpp).  These tests pin, in order: the two closed-form
// identities the family must reduce to (sphere, box), the octahedron
// and cylinder limits (which also pin WHICH exponent is which), the
// conservativeness itself, that the sphere tracer actually converges on
// the implicit surface, and that a composed field stays 1-Lipschitz.
//////////////////////////////////////////////////////////////////////

// The composed field is protected; a probe subclass is the honest way to
// assert on the SHIPPED Map (a test-local re-implementation could drift
// from it silently, which is precisely what these tests exist to catch).
namespace
{
	class FieldProbe : public SDFGeometry
	{
	public:
		FieldProbe( const std::vector<SDFGeometry::Part>& parts )
			: SDFGeometry( parts, 512, Scalar(1e-5) ) {}
		Scalar FieldAt( const Point3& p ) const { return Map( p ); }
	protected:
		virtual ~FieldProbe() {}
	};

	// ---- reference math, INDEPENDENT of the shipped implementation ----
	//
	// Deliberately the STRAIGHT nested-power form of the gauge, not
	// sdSuperellipsoidY's max-factored one: at the moderate exponents and
	// coordinates used here the two are numerically equivalent, so a
	// disagreement is a real disagreement and not two copies of one bug.
	//
	//   g(p) = ( ( |x/a|^q + |z/a|^q )^(p/q) + |y/a|^p )^(1/p),  p = 2/e1, q = 2/e2
	//
	// The implicit is F = g^p, surface at F = 1 <=> g = 1.
	Scalar SEGauge( const Point3& v, const Scalar a, const Scalar e1, const Scalar e2 )
	{
		const Scalar p = Scalar(2)/e1, q = Scalar(2)/e2;
		const Scalar ax = std::fabs(v.x)/a, ay = std::fabs(v.y)/a, az = std::fabs(v.z)/a;
		const Scalar cross = std::pow( std::pow(ax,q) + std::pow(az,q), p/q );
		return std::pow( cross + std::pow(ay,p), Scalar(1)/p );
	}
	Scalar SEImplicitF( const Point3& v, const Scalar a, const Scalar e1, const Scalar e2 )
	{
		return std::pow( SEGauge(v,a,e1,e2), Scalar(2)/e1 );
	}
	// The conservative inradius the primitive scales by (kept here so the
	// closed-form expectations below are written in the test's own terms).
	Scalar SEInradius( const Scalar a, const Scalar e1, const Scalar e2 )
	{
		return a / ( std::pow( Scalar(2), std::max( Scalar(0), (e1-Scalar(1))*Scalar(0.5) ) )
		           * std::pow( Scalar(2), std::max( Scalar(0), (e2-Scalar(1))*Scalar(0.5) ) ) );
	}
	// Barr's surface parameterization -- verified against the implicit in
	// TestSuperellipsoidConservativeDistance before it is trusted.
	Point3 SESurfacePoint( const Scalar eta, const Scalar om, const Scalar a, const Scalar e1, const Scalar e2 )
	{
		const Scalar ce = std::copysign( std::pow( std::fabs(std::cos(eta)), e1 ), std::cos(eta) );
		const Scalar se = std::copysign( std::pow( std::fabs(std::sin(eta)), e1 ), std::sin(eta) );
		const Scalar co = std::copysign( std::pow( std::fabs(std::cos(om )), e2 ), std::cos(om ) );
		const Scalar so = std::copysign( std::pow( std::fabs(std::sin(om )), e2 ), std::sin(om ) );
		return Point3( a*ce*co, a*se, a*ce*so );
	}
	Scalar SEDist2( const Point3& s, const Point3& p )
	{
		const Scalar dx=s.x-p.x, dy=s.y-p.y, dz=s.z-p.z;
		return dx*dx + dy*dy + dz*dz;
	}
	// ---- the TRUE-DISTANCE reference, and the proof that it IS the truth ----
	//
	// Sampling the surface by DIRECTION.  The gauge is positively homogeneous,
	// so for any non-zero d the point d / g(d) lies EXACTLY on {g = 1}; a
	// Fibonacci lattice of directions therefore lands genuine surface points
	// spread by solid angle.  This replaces sampling Barr's (eta, omega)
	// parameterization on a uniform grid, which is a BAD parametrization once
	// an exponent leaves the neighbourhood of 1: it piles almost all of its
	// parameter area onto the flat faces, leaves the corner and edge regions
	// to a handful of samples, and a local refine started there settles into a
	// LOCAL minimum.  Measured looseness of that old grid against the truth
	// (bracketed below), over test 33d's own 75-point probe set at a = 1.3:
	//     e = (1.0, 1.0)   +0.00 %      e = (0.1, 1.0)    +2.26 %
	//     e = (0.5, 0.5)   +0.00 %      e = (0.1, 2.0)   +10.21 %
	//     e = (0.3, 1.7)   +0.05 %      e = (2.0, 0.1)  +111.50 %
	//     e = (0.1, 0.1)  +73.79 %      e = (1.0, 0.1)  +242.79 %
	// A `<= 1 + 1e-6` money assertion resting on a +242 % upper bound would
	// have passed a field overestimating the distance by 3.4x -- squarely
	// inside the near-box range most authoring uses.  The direction lattice
	// plus tangent-frame refine below is within 1e-8 RELATIVE of the truth at
	// every one of those pairs, and the bracket asserted in 33d proves it
	// rather than assuming it.
	Vector3 SEFibDir( const int i, const int n )
	{
		const Scalar z  = Scalar(1) - Scalar(2)*(Scalar(i) + Scalar(0.5))/Scalar(n);
		const Scalar r  = std::sqrt( std::max( Scalar(0), Scalar(1) - z*z ) );
		const Scalar th = Scalar(PI) * ( Scalar(3) - std::sqrt(Scalar(5)) ) * Scalar(i);
		return Vector3( r*std::cos(th), z, r*std::sin(th) );
	}
	Point3 SESurfaceFromDir( const Vector3& d, const Scalar a, const Scalar e1, const Scalar e2 )
	{
		const Scalar g = SEGauge( Point3(d.x,d.y,d.z), a, e1, e2 );
		return Point3( d.x/g, d.y/g, d.z/g );
	}
	void SEFrame( const Vector3& n, Vector3& t1, Vector3& t2 )
	{
		const Vector3 h = ( std::fabs(n.x) < Scalar(0.9) ) ? Vector3(1,0,0) : Vector3(0,1,0);
		t1 = norm3( h.y*n.z - h.z*n.y, h.z*n.x - h.x*n.z, h.x*n.y - h.y*n.x );
		t2 = Vector3( n.y*t1.z - n.z*t1.y, n.z*t1.x - n.x*t1.z, n.x*t1.y - n.y*t1.x );
	}
	// UPPER bound on dist(p, surface): the minimum over genuine surface points,
	// which is the direction the conservativeness assertion needs (est <= this
	// => est <= true).  `witness`, when given, receives the nearest surface
	// point found -- 33d turns it into the matching LOWER bound.
	Scalar SETrueDistance( const Point3& p, const Scalar a, const Scalar e1, const Scalar e2,
	                       Point3* witness = 0 )
	{
		const int NDIR = 4096, REFINE = 30;
		Scalar  best = Scalar(1e30);
		Vector3 bd( 0, 0, 1 );
		for( int i = 0; i < NDIR; ++i ) {
			const Vector3 d = SEFibDir( i, NDIR );
			const Scalar dd = SEDist2( SESurfaceFromDir(d,a,e1,e2), p );
			if( dd < best ) { best = dd; bd = d; }
		}
		// tangent-frame descent, window shrinking from ~2.5 lattice spacings
		Scalar w = Scalar(2.5) * std::sqrt( Scalar(4)*Scalar(PI)/Scalar(NDIR) );
		for( int r = 0; r < REFINE; ++r ) {
			Vector3 t1, t2; SEFrame( bd, t1, t2 );
			Vector3 cd = bd; Scalar cb = best;
			for( int i = -3; i <= 3; ++i )
			for( int j = -3; j <= 3; ++j ) {
				if( i == 0 && j == 0 ) { continue; }
				const Scalar u = w*Scalar(i)/Scalar(3), v = w*Scalar(j)/Scalar(3);
				const Vector3 d = norm3( bd.x + t1.x*u + t2.x*v,
				                         bd.y + t1.y*u + t2.y*v,
				                         bd.z + t1.z*u + t2.z*v );
				const Scalar dd = SEDist2( SESurfaceFromDir(d,a,e1,e2), p );
				if( dd < cb ) { cb = dd; cd = d; }
			}
			bd = cd; best = cb; w *= Scalar(0.62);
		}
		if( witness ) { *witness = SESurfaceFromDir( bd, a, e1, e2 ); }
		return std::sqrt( best );
	}
	// SUPPORT FUNCTION, in CLOSED FORM: h(n) = sup{ <n,v> : g(v) <= 1 }.  The
	// solid is the unit ball of a mixed norm l_p(l_q) -- p = 2/e1 along the
	// pole axis, q = 2/e2 across it -- whose dual is the mixed norm with the
	// CONJUGATE exponents p' = p/(p-1), q' = q/(q-1) (r' = infinity exactly
	// when r == 1, where the dual norm degenerates to a max: that is the
	// e = 2 octahedron corner, and it is why the two branches are here).  So
	// h(n) = a * ||n||_{p'(q')}, evaluated, not searched.
	//
	// This is what makes the harness SELF-VALIDATING.  {g <= 1} is convex for
	// every supported exponent pair, so for an EXTERIOR point P
	//     dist(P, solid)  =  max over unit n of ( <n,P> - h(n) )
	// -- every n names a supporting hyperplane and hence a rigorous LOWER
	// bound, while every surface sample gives a rigorous UPPER bound.  Test
	// 33d evaluates the lower bound along (P - nearest-found) and asserts the
	// two close: where they do, the upper bound the money assertion rests on
	// IS the true distance, proven rather than hoped.
	Scalar SESupportH( const Vector3& n, const Scalar a, const Scalar e1, const Scalar e2 )
	{
		const Scalar p = Scalar(2)/e1, q = Scalar(2)/e2;
		const Scalar nx = std::fabs(n.x), ny = std::fabs(n.y), nz = std::fabs(n.z);
		Scalar A;
		if( q <= Scalar(1) + Scalar(1e-15) ) {			// q' = infinity
			A = std::max( nx, nz );
		} else {
			const Scalar qd = q/(q-Scalar(1));
			A = std::pow( std::pow(nx,qd) + std::pow(nz,qd), Scalar(1)/qd );
		}
		Scalar G;
		if( p <= Scalar(1) + Scalar(1e-15) ) {			// p' = infinity
			G = std::max( A, ny );
		} else {
			const Scalar pd = p/(p-Scalar(1));
			G = std::pow( std::pow(A,pd) + std::pow(ny,pd), Scalar(1)/pd );
		}
		return a*G;
	}
	// max over the surface of <n, x>, by the same direction lattice + tangent
	// refine the distance search uses.  Only the ATTAINMENT half of the support
	// check needs it: a bare 4096-direction lattice under-reads h(n) by ~3 % at
	// the octahedron corner, where a single VERTEX is the only attaining point.
	Scalar SESupportReached( const Vector3& n, const Scalar a, const Scalar e1, const Scalar e2 )
	{
		const int NDIR = 4096, REFINE = 30;
		Scalar  best = -Scalar(1e30);
		Vector3 bd( 0, 0, 1 );
		for( int i = 0; i < NDIR; ++i ) {
			const Vector3 d = SEFibDir( i, NDIR );
			const Point3  sfc = SESurfaceFromDir( d, a, e1, e2 );
			const Scalar  v = n.x*sfc.x + n.y*sfc.y + n.z*sfc.z;
			if( v > best ) { best = v; bd = d; }
		}
		Scalar w = Scalar(2.5) * std::sqrt( Scalar(4)*Scalar(PI)/Scalar(NDIR) );
		for( int r = 0; r < REFINE; ++r ) {
			Vector3 t1, t2; SEFrame( bd, t1, t2 );
			Vector3 cd = bd; Scalar cb = best;
			for( int i = -3; i <= 3; ++i )
			for( int j = -3; j <= 3; ++j ) {
				if( i == 0 && j == 0 ) { continue; }
				const Scalar u = w*Scalar(i)/Scalar(3), v = w*Scalar(j)/Scalar(3);
				const Vector3 d = norm3( bd.x + t1.x*u + t2.x*v,
				                         bd.y + t1.y*u + t2.y*v,
				                         bd.z + t1.z*u + t2.z*v );
				const Point3 sfc = SESurfaceFromDir( d, a, e1, e2 );
				const Scalar val = n.x*sfc.x + n.y*sfc.y + n.z*sfc.z;
				if( val > cb ) { cb = val; cd = d; }
			}
			bd = cd; best = cb; w *= Scalar(0.62);
		}
		return best;
	}
	// First entry point of the ray o + t*dir into the solid, analytically.
	// The gauge is CONVEX (that is the whole basis of the distance bound), so
	// along a line it is convex too: {t : g <= 1} is a single interval.  A
	// ternary search finds its minimum, a bisection the entry crossing.
	// Returns false when the ray misses.
	bool SERayEntry( const Point3& o, const Vector3& dir, const Scalar tMax,
	                 const Scalar a, const Scalar e1, const Scalar e2, Scalar& tEntry )
	{
		Scalar lo = 0, hi = tMax;
		for( int i = 0; i < 200; ++i ) {
			const Scalar m1 = lo + (hi-lo)/3, m2 = hi - (hi-lo)/3;
			const Point3 p1( o.x+dir.x*m1, o.y+dir.y*m1, o.z+dir.z*m1 );
			const Point3 p2( o.x+dir.x*m2, o.y+dir.y*m2, o.z+dir.z*m2 );
			if( SEGauge(p1,a,e1,e2) < SEGauge(p2,a,e1,e2) ) { hi = m2; } else { lo = m1; }
		}
		const Scalar tMin = (lo+hi)/2;
		const Point3 pm( o.x+dir.x*tMin, o.y+dir.y*tMin, o.z+dir.z*tMin );
		if( SEGauge(pm,a,e1,e2) > Scalar(1) ) { return false; }   // ray misses the solid
		Scalar t0 = 0, t1 = tMin;
		for( int i = 0; i < 200; ++i ) {
			const Scalar tm = (t0+t1)/2;
			const Point3 pt( o.x+dir.x*tm, o.y+dir.y*tm, o.z+dir.z*tm );
			if( SEGauge(pt,a,e1,e2) > Scalar(1) ) { t0 = tm; } else { t1 = tm; }
		}
		tEntry = (t0+t1)/2;
		return true;
	}

	FieldProbe* MakeSuperProbe( const Scalar a, const Scalar e1, const Scalar e2 )
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSuperellipsoid, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), a, e1, e2, 0 ) );
		return new FieldProbe( parts );
	}
}

// Test 33a: e1 = e2 = 1 IS the sphere.  The superquadric reduces exactly
// (p = q = 2 makes the mixed norm the Euclidean one, and Cp = Cq = 1 makes
// the inradius the radius), so the field must match sdSphere -- |p| - R --
// to round-off, not merely to sphere-trace tolerance.
static void TestSuperellipsoidIsSphereAtUnitExponents()
{
	std::cout << "Test 33a: superellipsoid e1 = e2 = 1 == exact analytic sphere (field + trace)" << std::endl;
	const Scalar R = 1.7;
	FieldProbe* g = MakeSuperProbe( R, 1.0, 1.0 );

	Scalar worst = 0;
	for( int i = -6; i <= 6; ++i )
	for( int j = -6; j <= 6; ++j )
	for( int k = -6; k <= 6; ++k ) {
		const Point3 p( i*0.53, j*0.47, k*0.61 );
		const Scalar expected = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z) - R;   // sdSphere
		worst = std::max( worst, std::fabs( g->FieldAt(p) - expected ) );
	}
	Check( worst < 1e-12, "MONEY -- field == sdSphere at 2197 sample points (max |delta| < 1e-12)" );

	// ...and the ray path agrees with the analytic sphere too.
	const Point3 origins[4] = { Point3(0,0,9), Point3(7,0,0), Point3(5,5,4), Point3(-6,3,-5) };
	for( int i = 0; i < 4; ++i ) {
		const Point3 P = origins[i];
		const Scalar L = std::sqrt(P.x*P.x+P.y*P.y+P.z*P.z);
		RayIntersectionGeometric ri = MkRI( P, Vector3(-P.x/L,-P.y/L,-P.z/L) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.range, L - R, 3e-3 ), "e1=e2=1 hit distance == analytic sphere" );
		Check( VClose( ri.vNormal, norm3(P.x,P.y,P.z) ), "e1=e2=1 normal == radial" );
	}
	safe_release( g );
}

// Test 33b: the BOX limit.  As the exponents shrink the solid converges to
// the box of half-extent a from the INSIDE-out.  Both probes are closed
// form, so nothing here is a tuned constant:
//   * on the box's own edge midpoint (a,a,0) the gauge is 2^(e1/2), so the
//     field is exactly a*(2^(e1/2) - 1);
//   * on its corner (a,a,a) the gauge is 3^(e/2) (isotropic exponents), so
//     the field is exactly a*(3^(e/2) - 1);
//   * on an exterior point whose nearest box feature is a FACE, the field is
//     >= sdBox and decreases monotonically toward it.
// All three tend to the box value monotonically as e shrinks -- which is the
// convergence assertion, no magic threshold involved.
static void TestSuperellipsoidBoxLimit()
{
	std::cout << "Test 33b: superellipsoid -> box as the exponents shrink (closed form, monotone)" << std::endl;
	const Scalar a = 1.25;
	const Scalar es[5] = { 1.0, 0.7, 0.4, 0.2, 0.1 };

	Scalar prevEdge = 1e30, prevCorner = 1e30, prevFace = 1e30;
	for( int i = 0; i < 5; ++i ) {
		const Scalar e = es[i];
		FieldProbe* g = MakeSuperProbe( a, e, e );

		// face centre: the axis intercept -- exactly ON the surface at EVERY
		// exponent (F on the +x axis is |x/a|^(2/e1)), so the field is 0.
		Check( std::fabs( g->FieldAt( Point3(a,0,0) ) ) < 1e-12, "box limit: +X face centre is on the surface at every e" );

		const Scalar edge   = g->FieldAt( Point3(a,a,0) );
		const Scalar corner = g->FieldAt( Point3(a,a,a) );
		const Scalar face   = g->FieldAt( Point3(2*a,0.5*a,0.5*a) );   // nearest box feature is the +X FACE, sdBox = a

		Check( IsClose( edge,   a*(std::pow(Scalar(2),e/2)-1), 1e-12 ), "box limit: edge midpoint field == a*(2^(e/2)-1) exactly" );
		Check( IsClose( corner, a*(std::pow(Scalar(3),e/2)-1), 1e-12 ), "box limit: corner field == a*(3^(e/2)-1) exactly" );
		Check( face >= a - Scalar(1e-12), "box limit: exterior over-a-face field never dips below sdBox" );

		if( i > 0 ) {
			Check( edge   < prevEdge,   "box limit: edge-midpoint field decreases monotonically toward 0" );
			Check( corner < prevCorner, "box limit: corner field decreases monotonically toward 0" );
			Check( face   < prevFace,   "box limit: over-a-face field decreases monotonically toward sdBox" );
		}
		prevEdge = edge; prevCorner = corner; prevFace = face;
		safe_release( g );
	}
	// the tail of those monotone sequences really is the box, to the closed
	// form's own value (2^0.05 - 1 = 3.5 %, 3^0.05 - 1 = 5.6 %)
	Check( prevEdge   < a*Scalar(0.036), "box limit: edge field has converged to within 3.6 % of the box" );
	Check( prevCorner < a*Scalar(0.057), "box limit: corner field has converged to within 5.7 % of the box" );
	Check( prevFace   < a*Scalar(1.06),  "box limit: over-a-face field has converged to within 6 % of sdBox" );
}

// Test 33c: the OCTAHEDRON (e1 = e2 = 2) and the CYLINDER (e1 -> 0, e2 = 1)
// limits.  The cylinder half also pins WHICH exponent is which: swapping
// e1/e2 turns the cylinder into a square-section barrel, and the two
// disagree on the SIGN of the field at a probe point and on the hit range of
// one descending ray.
static void TestSuperellipsoidOctahedronAndCylinder()
{
	std::cout << "Test 33c: octahedron (e=2,2) axis intercepts + cylinder (e1->0,e2=1), which pins e1 vs e2" << std::endl;
	const Scalar a = 1.4;

	{	// e1 = e2 = 2 is |x| + |y| + |z| = a
		FieldProbe* g = MakeSuperProbe( a, 2.0, 2.0 );
		Check( std::fabs( g->FieldAt( Point3(a,0,0) ) ) < 1e-12, "octahedron: +X axis intercept at a" );
		Check( std::fabs( g->FieldAt( Point3(0,a,0) ) ) < 1e-12, "octahedron: +Y axis intercept at a" );
		Check( std::fabs( g->FieldAt( Point3(0,0,-a) ) ) < 1e-12, "octahedron: -Z axis intercept at a" );
		// a face point: |x|+|y|+|z| = a with all three non-zero
		Check( std::fabs( g->FieldAt( Point3(a/3,a/3,a/3) ) ) < 1e-12, "octahedron: (a/3,a/3,a/3) is on the surface" );
		Check( g->FieldAt( Point3(0.4*a,0.4*a,0) ) < 0, "octahedron: 0.8a along an edge diagonal is INSIDE" );
		Check( g->FieldAt( Point3(0.6*a,0.6*a,0) ) > 0, "octahedron: 1.2a along an edge diagonal is OUTSIDE" );
		// through the tracer: the +X intercept and the (1,1,1) face
		{	RayIntersectionGeometric ri = MkRI( Point3(9,0,0), Vector3(-1,0,0) );
			g->IntersectRay( ri, true, true, false );
			Check( ri.bHit && IsClose( ri.range, 9 - a, 3e-3 ), "octahedron: traced +X intercept == a" ); }
		{	const Scalar s = 5.0, L = s*std::sqrt(Scalar(3));
			RayIntersectionGeometric ri = MkRI( Point3(s,s,s), norm3(-1,-1,-1) );
			g->IntersectRay( ri, true, true, false );
			// the (1,1,1) face is hit where 3t/sqrt(3) = a  =>  |p| = a/sqrt(3)
			Check( ri.bHit && IsClose( ri.range, L - a/std::sqrt(Scalar(3)), 3e-3 ), "octahedron: traced (1,1,1) face at a/sqrt(3)" ); }
		safe_release( g );
	}

	{	// e1 -> 0 with e2 = 1: a CYLINDER about local Y, radius a, half-height a
		FieldProbe* cyl  = MakeSuperProbe( a, 0.1, 1.0 );
		FieldProbe* swap = MakeSuperProbe( a, 1.0, 0.1 );   // the SAME numbers, transposed
		Check( cyl->FieldAt( Point3(0.97*a,0,0) ) < 0,      "cylinder: near the barrel wall is inside" );
		Check( cyl->FieldAt( Point3(0,0.97*a,0) ) < 0,      "cylinder: near the flat cap is inside" );
		Check( cyl->FieldAt( Point3(1.03*a,0,0) ) > 0,      "cylinder: just past the barrel wall is outside" );
		// THE ORIENTATION PROBE: (0.9a, 0.9a, 0) is comfortably inside a
		// cylinder (rho and |y| both < a) but OUTSIDE the transposed shape,
		// whose square cross-section shrinks as sqrt(1-(y/a)^2).
		Check( cyl->FieldAt(  Point3(0.9*a,0.9*a,0) ) < 0, "MONEY (e1 vs e2) -- cylinder puts (0.9a,0.9a,0) INSIDE" );
		Check( swap->FieldAt( Point3(0.9*a,0.9*a,0) ) > 0, "MONEY (e1 vs e2) -- the transposed exponents put it OUTSIDE" );
		// and through the tracer: a ray descending at x = a/2 meets the
		// cylinder's flat cap at y ~= a, but the transposed shape's round
		// profile at y = a*sqrt(1 - 1/4) = 0.866a.
		{	RayIntersectionGeometric ri = MkRI( Point3(0.5*a,9,0), Vector3(0,-1,0) );
			cyl->IntersectRay( ri, true, true, false );
			Check( ri.bHit && IsClose( ri.range, 9 - a, 4e-3 ), "cylinder: descending ray meets the FLAT cap at y = a" ); }
		{	RayIntersectionGeometric ri = MkRI( Point3(0.5*a,9,0), Vector3(0,-1,0) );
			swap->IntersectRay( ri, true, true, false );
			Check( ri.bHit && IsClose( ri.range, 9 - a*std::sqrt(Scalar(0.75)), 4e-3 ), "transposed: same ray meets the ROUND profile at 0.866a" ); }
		safe_release( cyl );
		safe_release( swap );
	}
}

// Test 33d: CONSERVATIVENESS -- THE MONEY TEST.  SDFGeometry's sphere tracer
// requires every part field to be <= 1-Lipschitz, i.e. never to OVERESTIMATE
// the distance to its own zero set; an overestimate anywhere is a March
// overshoot waiting to happen (surface silently missed).  The superquadric
// has no exact SDF, so this is the assertion that the chosen bound really is
// a bound: over a grid of exponent pairs spanning the whole supported range
// (plus the two anisotropic corners), at points inside, outside and close to
// the surface, |field| must never exceed the true nearest-surface distance
// found by dense numerical search.
static void TestSuperellipsoidConservativeDistance()
{
	std::cout << "Test 33d: MONEY -- the field NEVER overestimates the true distance to the surface" << std::endl;
	const Scalar a = 1.3;

	// First: the search harness itself is trustworthy.  At e1 = e2 = 1 the
	// true distance is analytic (||p| - a|), so any error in the dense search
	// shows up here before it can weaken the assertions below.
	{
		Scalar worstSearch = 0;
		const Point3 probes[5] = { Point3(2.2,0.4,-0.9), Point3(-0.3,1.9,0.2), Point3(0.2,0.1,0.15),
		                           Point3(-1.4,-1.4,1.1), Point3(0,0,3.0) };
		for( int i = 0; i < 5; ++i ) {
			const Point3& p = probes[i];
			const Scalar analytic = std::fabs( std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z) - a );
			worstSearch = std::max( worstSearch, std::fabs( SETrueDistance(p,a,1.0,1.0) - analytic ) );
		}
		Check( worstSearch < 1e-6, "search harness reproduces the analytic sphere distance (< 1e-6)" );
	}

	// Second: the closed-form SUPPORT function really does describe supporting
	// hyperplanes of THIS solid -- no surface point may exceed h(n), and some
	// surface point must attain it.  Verified numerically against the same
	// direction-sampled surface the distance search uses, at the exponent
	// corners where the conjugate exponent goes to infinity (e = 2, both
	// axes) as well as in the smooth interior.  Without this the lower bound
	// in the bracket below would be an unchecked assumption.
	{
		struct Pr { Scalar e1, e2; };
		const Pr sp[6] = { {1.0,1.0}, {0.5,0.5}, {2.0,2.0}, {2.0,1.0}, {1.0,2.0}, {0.3,1.7} };
		Scalar worstOver = 0, worstSlack = 0;
		for( int ip = 0; ip < 6; ++ip ) {
			for( int k = 0; k < 16; ++k ) {
				const Vector3 n = SEFibDir( k, 16 );
				const Scalar h = SESupportH( n, a, sp[ip].e1, sp[ip].e2 );
				const Scalar reached = SESupportReached( n, a, sp[ip].e1, sp[ip].e2 );
				worstOver  = std::max( worstOver,  reached - h );		// must be <= 0
				worstSlack = std::max( worstSlack, h - reached );		// must be ~0
			}
		}
		Check( worstOver <= 1e-9,
		       "MONEY -- the closed-form support function is never exceeded by a surface point (it SUPPORTS)" );
		Check( worstSlack < 1e-6,
		       "...and it is attained (tight), so it is the true support function, not merely an over-bound" );
	}

	struct Pair { Scalar e1, e2; };
	const Pair pairs[10] = {
		{1.0,1.0}, {0.5,0.5}, {0.1,0.1}, {2.0,2.0}, {1.5,1.5},
		{0.1,1.0}, {1.0,0.1}, {2.0,0.1}, {0.1,2.0}, {0.3,1.7}
	};

	// a deterministic, reproducible spread: a lattice of directions x three
	// radial shells (inside / just outside the surface / well outside).
	Scalar worstRatio = 0, worstBracket = 0;
	Scalar worstE1 = 0, worstE2 = 0;
	Point3 worstAt(0,0,0);
	for( int ip = 0; ip < 10; ++ip ) {
		const Scalar e1 = pairs[ip].e1, e2 = pairs[ip].e2;
		// the parameterization used by the reference search must actually lie
		// on the implicit surface, or the "true distance" is fiction
		Check( std::fabs( SEImplicitF( SESurfacePoint(0.7,1.9,a,e1,e2), a, e1, e2 ) - 1 ) < 1e-9,
		       "reference surface parameterization satisfies F == 1" );

		FieldProbe* g = MakeSuperProbe( a, e1, e2 );

		// The CENTRE is the deepest interior point and pins the inradius
		// scaling exactly: the gauge is 0 there, so the field must be
		// -rin.  Drop the rin factor (or replace it with the local surface
		// radius) and this closed form breaks before the search-based
		// assertion below even runs.
		Check( IsClose( g->FieldAt( Point3(0,0,0) ), -SEInradius(a,e1,e2), 1e-12 ),
		       "field at the centre == -(conservative inradius), exactly" );
		{	const Scalar truCentre = SETrueDistance( Point3(0,0,0), a, e1, e2 );
			Check( std::fabs( g->FieldAt( Point3(0,0,0) ) ) <= truCentre + 1e-9,
			       "centre depth never exceeds the TRUE inradius" ); }

		for( int i = 0; i < 5; ++i )
		for( int j = 0; j < 5; ++j )
		for( int s = 0; s < 3; ++s ) {
			// a direction lattice that dodges the exact axes and diagonals on
			// most cells but hits a few of them squarely
			const Vector3 d = norm3( Scalar(i)-2 + Scalar(0.13), Scalar(j)-2 - Scalar(0.07), Scalar(i-j)*Scalar(0.6) + Scalar(0.21) );
			const Scalar shell[3] = { Scalar(0.45), Scalar(1.05), Scalar(2.30) };
			const Point3 p( d.x*a*shell[s], d.y*a*shell[s], d.z*a*shell[s] );
			const Scalar est  = g->FieldAt( p );
			Point3 witness( 0, 0, 0 );
			const Scalar tru  = SETrueDistance( p, a, e1, e2, &witness );
			if( tru < 1e-6 ) { continue; }              // sitting on the surface: ratio is meaningless
			// BRACKET.  For an exterior probe the supporting hyperplane through
			// the nearest surface point found gives a rigorous LOWER bound on
			// the same distance; where it meets the upper bound, `tru` IS the
			// true distance and the ratio below is exact rather than lenient.
			if( SEGauge( p, a, e1, e2 ) > Scalar(1) ) {
				const Vector3 n = norm3( p.x-witness.x, p.y-witness.y, p.z-witness.z );
				const Scalar lo = ( n.x*p.x + n.y*p.y + n.z*p.z ) - SESupportH( n, a, e1, e2 );
				worstBracket = std::max( worstBracket, (tru - lo)/tru );
			}
			const Scalar ratio = std::fabs(est) / tru;
			if( ratio > worstRatio ) { worstRatio = ratio; worstE1 = e1; worstE2 = e2; worstAt = p; }
		}
		safe_release( g );
	}
	if( worstRatio > 1 + 1e-6 ) {
		std::cout << "    worst |field|/true = " << worstRatio << " at e1=" << worstE1 << " e2=" << worstE2
		          << " p=(" << worstAt.x << "," << worstAt.y << "," << worstAt.z << ")" << std::endl;
	}
	// The reference is pinned BEFORE it is leaned on: a loose upper bound is
	// exactly how a 44 %-overestimating field would sail through the assertion
	// that follows (see the measured table above SETrueDistance).
	if( worstBracket > 1e-4 ) {
		std::cout << "    worst (upper-lower)/upper on the true-distance bracket = " << worstBracket << std::endl;
	}
	Check( worstBracket < 1e-4,
	       "MONEY -- the true-distance reference is bracketed to < 1e-4 relative, so it IS the true distance" );
	Check( worstRatio <= 1 + 1e-6,
	       "MONEY ASSERTION -- |field| <= true distance at every probe, every supported exponent pair" );
}

// Test 33e: SPHERE-TRACE ROUND TRIP.  A conservative field is worthless if
// March never converges on it (or tunnels straight through).  Fire rays from
// several directions at several exponent pairs and assert the reported hit
// (a) satisfies the implicit F == 1, and (b) is the FIRST crossing -- the
// analytic entry point, found by ternary search + bisection on the (convex)
// gauge along the ray, so a hit on the far side fails rather than passes.
static void TestSuperellipsoidSphereTraceRoundTrip()
{
	std::cout << "Test 33e: sphere-trace round trip -- hits land ON the implicit surface, at the FIRST crossing" << std::endl;
	const Scalar a = 1.3;
	struct Pair { Scalar e1, e2; };
	const Pair pairs[6] = { {1.0,1.0}, {0.6,0.6}, {0.2,0.2}, {2.0,2.0}, {0.15,1.0}, {1.6,0.4} };
	const Vector3 dirs[6] = { norm3(0,0,-1), norm3(-1,0,0), norm3(0,-1,0),
	                          norm3(-1,-1,-1), norm3(-0.4,-1,0.3), norm3(0.9,-0.2,-1) };

	for( int ip = 0; ip < 6; ++ip ) {
		const Scalar e1 = pairs[ip].e1, e2 = pairs[ip].e2;
		FieldProbe* g = MakeSuperProbe( a, e1, e2 );
		for( int i = 0; i < 6; ++i ) {
			const Vector3 d = dirs[i];
			const Point3 o( -d.x*8, -d.y*8, -d.z*8 );
			RayIntersectionGeometric ri = MkRI( o, d );
			g->IntersectRay( ri, true, true, false );
			Check( ri.bHit, "round trip: ray hits (March converged, no tunnelling)" );
			if( !ri.bHit ) { continue; }
			const Point3 h( o.x + d.x*ri.range, o.y + d.y*ri.range, o.z + d.z*ri.range );
			// (a) the implicit.  F = g^(2/e1), so a gauge error eps shows up in
			// F multiplied by 2/e1 -- scale the tolerance the same way instead
			// of pretending one absolute number fits every exponent.
			Check( std::fabs( SEGauge(h,a,e1,e2) - 1 ) < 2e-3, "round trip: |gauge(hit) - 1| < 2e-3" );
			Check( std::fabs( SEImplicitF(h,a,e1,e2) - 1 ) < 2e-3 * (2/e1) + 1e-3, "round trip: |F(hit) - 1| within the gauge tolerance carried through F" );
			// (b) the FIRST crossing, not the far side
			Scalar tEntry = 0;
			Check( SERayEntry( o, d, 16.0, a, e1, e2, tEntry ), "round trip: the analytic entry exists" );
			Check( IsClose( ri.range, tEntry, 4e-3 ), "MONEY -- traced range == the analytic FIRST entry point" );
		}
		safe_release( g );
	}
}

// Test 33f: COMPOSITION.  The point of shipping this as an SDF part rather
// than a standalone chunk is that it composes -- smin with a sphere, carved
// by a subtract -- and sminP/smaxP's conservativeness argument holds only
// while EVERY part field is <= 1-Lipschitz.  Finite-difference the composed
// field along sampled lines and assert the gradient magnitude never exceeds
// 1 (which is exactly the composed-field precondition March relies on).
static void TestSuperellipsoidCompositionStaysLipschitz()
{
	std::cout << "Test 33f: superellipsoid smin sphere (+ a subtract) stays <= 1-Lipschitz" << std::endl;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSuperellipsoid, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1.6,1.0,0.8), 1.2, 0.45, 0.7, 0 ) );      // a cushion, non-uniformly scaled
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.8,
		Point3(1.9,0.3,0), 0,0,0, Vector3(1,1,1), 0.9, 0, 0, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSuperellipsoid, SDFGeometry::eOpSubtract, 0.4,
		Point3(-1.2,0.9,0), 20,0,35, Vector3(1,1,1), 0.7, 1.8, 0.3, 0 ) );      // an octahedral-ish carve
	FieldProbe* g = new FieldProbe( parts );

	const Scalar h = 1e-4;
	Scalar worst = 0;
	for( int line = 0; line < 8; ++line ) {
		const Vector3 d = norm3( std::cos(line*0.7), std::sin(line*1.1)*0.6 + 0.2, std::sin(line*0.4) );
		for( int t = 0; t < 400; ++t ) {
			const Scalar s = -3.5 + 7.0*t/399.0;
			const Point3 p( d.x*s + 0.13, d.y*s - 0.07, d.z*s + 0.05 );
			const Scalar gx = ( g->FieldAt(Point3(p.x+h,p.y,p.z)) - g->FieldAt(Point3(p.x-h,p.y,p.z)) ) / (2*h);
			const Scalar gy = ( g->FieldAt(Point3(p.x,p.y+h,p.z)) - g->FieldAt(Point3(p.x,p.y-h,p.z)) ) / (2*h);
			const Scalar gz = ( g->FieldAt(Point3(p.x,p.y,p.z+h)) - g->FieldAt(Point3(p.x,p.y,p.z-h)) ) / (2*h);
			worst = std::max( worst, std::sqrt(gx*gx + gy*gy + gz*gz) );
		}
	}
	Check( worst <= 1 + 1e-3, "MONEY -- composed |grad| never exceeds 1 along 3200 sampled points" );
	// the sampling really does cross the blend and the carve (a field that is
	// flat everywhere would pass the assertion above vacuously)
	Check( worst > 0.9, "the sampled lines really do traverse the field (max |grad| is near 1)" );
	safe_release( g );
}

// Test 33g: OUT-OF-RANGE exponents.  The field clamps to [0.1, 2]
// unconditionally, because `size` is keyframable and a runtime value never
// passes through ParsePartLines.  Two things follow, and both are asserted:
// an out-of-range part renders exactly the CLAMPED shape, and it is still
// conservative (at e = 3 the unclamped bound overestimates by 2.7x -- an
// overshoot -- which is precisely why the clamp is in the field).
static void TestSuperellipsoidExponentClamp()
{
	std::cout << "Test 33g: out-of-range exponents clamp in the FIELD (keyframing bypasses the parser)" << std::endl;
	const Scalar a = 1.3;
	FieldProbe* wild    = MakeSuperProbe( a, 4.0, 3.0 );      // MakePart does not clamp
	FieldProbe* clamped = MakeSuperProbe( a, 2.0, 2.0 );
	FieldProbe* tiny    = MakeSuperProbe( a, 0.001, 0.02 );
	FieldProbe* floored = MakeSuperProbe( a, 0.1, 0.1 );

	Scalar worstHi = 0, worstLo = 0;
	for( int i = -4; i <= 4; ++i )
	for( int j = -4; j <= 4; ++j )
	for( int k = -4; k <= 4; ++k ) {
		const Point3 p( i*0.6, j*0.55, k*0.5 );
		worstHi = std::max( worstHi, std::fabs( wild->FieldAt(p) - clamped->FieldAt(p) ) );
		worstLo = std::max( worstLo, std::fabs( tiny->FieldAt(p) - floored->FieldAt(p) ) );
	}
	Check( worstHi < 1e-12, "above-range exponents render exactly the e = 2 shape" );
	Check( worstLo < 1e-12, "below-range exponents render exactly the e = 0.1 shape" );

	// ...and the clamped field is still conservative against the shape it
	// actually renders (the e = 2 solid), which is the property the clamp
	// exists for.  Same 75-point lattice and same bracketed reference as
	// 33d -- 16 probe points was not enough surface to catch an overestimate
	// that only appears near the octahedron's edges.
	Scalar worstRatio = 0, worstBracket = 0;
	for( int i = 0; i < 5; ++i )
	for( int j = 0; j < 5; ++j )
	for( int sh = 0; sh < 3; ++sh ) {
		const Vector3 d = norm3( Scalar(i)-2 + Scalar(0.13), Scalar(j)-2 - Scalar(0.07), Scalar(i-j)*Scalar(0.6) + Scalar(0.21) );
		const Scalar shell[3] = { Scalar(0.45), Scalar(1.05), Scalar(2.30) };
		const Point3 p( d.x*a*shell[sh], d.y*a*shell[sh], d.z*a*shell[sh] );
		Point3 witness( 0, 0, 0 );
		const Scalar tru = SETrueDistance( p, a, 2.0, 2.0, &witness );
		if( tru < 1e-6 ) { continue; }
		if( SEGauge( p, a, 2.0, 2.0 ) > Scalar(1) ) {
			const Vector3 n = norm3( p.x-witness.x, p.y-witness.y, p.z-witness.z );
			const Scalar lo = ( n.x*p.x + n.y*p.y + n.z*p.z ) - SESupportH( n, a, 2.0, 2.0 );
			worstBracket = std::max( worstBracket, (tru - lo)/tru );
		}
		worstRatio = std::max( worstRatio, std::fabs( wild->FieldAt(p) ) / tru );
	}
	Check( worstBracket < 1e-4, "the e = 2 true-distance reference is bracketed to < 1e-4 relative" );
	Check( worstRatio <= 1 + 1e-6, "MONEY -- an e = (4,3) part is STILL conservative, because the field clamped it" );

	safe_release( wild ); safe_release( clamped ); safe_release( tiny ); safe_release( floored );
}

// Test 33h: the part GRAMMAR accepts the new token, and the local AABB is the
// radii box (tight: the axis intercepts touch it at every exponent).
static void TestSuperellipsoidGrammarAndBounds()
{
	std::cout << "Test 33h: `superellipsoid` part token parses; local AABB is the tight radii box" << std::endl;
	{
		std::vector<SDFGeometry::Part> parts;
		Check( SDFGeometry::ParsePartLines(
			"superellipsoid union 0  0.5 1 -2  10 20 30  1 2 3  0.8 0.45 1.6  0\n", "<test>", parts ),
			"superellipsoid part line parses" );
		Check( parts.size() == 1, "one part" );
		if( parts.size() == 1 ) {
			Check( parts[0].type == SDFGeometry::ePrimSuperellipsoid, "type is ePrimSuperellipsoid" );
			Check( parts[0].a == 0.8 && parts[0].b == 0.45 && parts[0].c == 1.6, "a = radius, b = e1, c = e2" );
			Check( parts[0].scale.x == 1 && parts[0].scale.y == 2 && parts[0].scale.z == 3, "proportions come from the per-part scale" );
		}
	}
	{	// out of range: WARNS and clamps rather than rejecting the scene
		std::vector<SDFGeometry::Part> parts;
		Check( SDFGeometry::ParsePartLines(
			"superellipsoid union 0  0 0 0  0 0 0  1 1 1  1 7 -3  0\n", "<test>", parts ),
			"out-of-range exponents do NOT reject the line" );
		if( parts.size() == 1 ) {
			Check( parts[0].b == 2.0 && parts[0].c == 0.1, "out-of-range exponents are clamped to [0.1, 2] at parse time" );
		}
	}
	{	// still 16 tokens, still a hard grammar
		std::vector<SDFGeometry::Part> parts;
		Check( !SDFGeometry::ParsePartLines(
			"superellipsoid union 0  0 0 0  0 0 0  1 1 1  1 1 1\n", "<test>", parts ),
			"short superellipsoid line still rejected" );
		Check( !SDFGeometry::ParsePartLines(
			"super_ellipsoid union 0  0 0 0  0 0 0  1 1 1  1 1 1  0\n", "<test>", parts ),
			"a near-miss spelling is still rejected (no silent fallback)" );
	}
	{	// bounds: [-a,a]^3 in local space, at exponents on both sides of 1
		const Scalar a = 1.1;
		const Scalar es[3] = { 0.2, 1.0, 2.0 };
		for( int i = 0; i < 3; ++i ) {
			FieldProbe* g = MakeSuperProbe( a, es[i], es[i] );
			BoundingBox bb = g->GenerateBoundingBox();
			// contains the surface (the axis intercepts sit exactly at +-a)
			Check( bb.ll.x <= -a && bb.ur.x >= a && bb.ll.y <= -a && bb.ur.y >= a && bb.ll.z <= -a && bb.ur.z >= a,
			       "local AABB contains the +-a axis intercepts" );
			// and is not loose: only the fixed safety pad beyond them
			Check( bb.ll.x >= -a - 0.02 && bb.ur.x <= a + 0.02,
			       "local AABB is TIGHT to the radii box (pad only)" );
			safe_release( g );
		}
	}
}

// Test 33i: NON-FINITE COORDINATES.  A NaN or infinite coordinate can reach
// a part field through a degenerate part transform (invScale from a scale
// keyframed through zero) or through caller arithmetic.  The gauge's own
// max-factoring LAUNDERS it: inf/inf is NaN, and std::max(NaN, v) is
// (a<b)?b:a == NaN, after which every `> 0` test is false and the gauge
// collapses to the ORIGIN value -- so the pre-fix field answered -rin, the
// point reported MAXIMALLY INSIDE.  Through Map's min() fold a constant
// negative does not merely add noise, it FILLS the bounding box.  Measured on
// the pre-fix form at a = 1.3, e = (1,1): x = +-inf -> -1.3, x = NaN -> -1.0,
// z = +-inf or NaN -> -1.3; only a non-finite y (-> 1e30) was ever caught.
// Every row must now be the definite-miss 1e30, on every code path.
static void TestSuperellipsoidNonFiniteCoordinates()
{
	std::cout << "Test 33i: a non-finite coordinate is a definite MISS, never a negative distance" << std::endl;
	const Scalar a = 1.3;
	const Scalar inf = std::numeric_limits<Scalar>::infinity();
	const Scalar nan = std::numeric_limits<Scalar>::quiet_NaN();
	const Scalar bad[3] = { inf, -inf, nan };
	const char*  nm [3] = { "+inf", "-inf", "NaN" };

	// one probe per code path in sdSuperellipsoidY: sphere fast path,
	// equal-exponent fast path, general nested path, degenerate radius.
	struct Cfg { Scalar a, e1, e2; const char* what; };
	const Cfg cfgs[4] = {
		{ a,   1.0, 1.0, "sphere fast path"  },
		{ a,   0.45,0.45,"equal-exponent fast path" },
		{ a,   0.3, 1.7, "general nested path" },
		{ 0.0, 1.0, 1.0, "degenerate radius"  }
	};

	for( int c = 0; c < 4; ++c ) {
		FieldProbe* g = MakeSuperProbe( cfgs[c].a, cfgs[c].e1, cfgs[c].e2 );
		for( int axis = 0; axis < 3; ++axis )
		for( int k = 0; k < 3; ++k ) {
			Scalar v[3] = { Scalar(0.3), Scalar(0.3), Scalar(0.3) };
			v[axis] = bad[k];
			const Scalar d = g->FieldAt( Point3(v[0],v[1],v[2]) );
			const bool ok = ( d >= Scalar(1e29) );
			if( !ok ) {
				std::cout << "    " << cfgs[c].what << ": " << "xyz"[axis] << " = " << nm[k]
				          << " -> " << d << std::endl;
			}
			Check( ok, "MONEY -- non-finite coordinate returns the definite-miss value, not a negative distance" );
		}
		safe_release( g );
	}

	// ...and the composed field agrees: a non-finite probe must not read as
	// INSIDE a two-part union (which is how the pre-fix -rin filled the bbox).
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSuperellipsoid, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), a, 0.5, 0.5, 0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(3,0,0), 0,0,0, Vector3(1,1,1), 0.7, 0, 0, 0 ) );
		FieldProbe* g = new FieldProbe( parts );
		Check( g->FieldAt( Point3(inf,0.3,0.3) ) > 0, "composed field: infinite x is not INSIDE" );
		Check( g->FieldAt( Point3(0.3,0.3,nan) ) > 0, "composed field: NaN z is not INSIDE" );
		safe_release( g );
	}
}

// Test 33j: THE TWO EXACT SPECIALIZATIONS.  The general gauge costs six pow()
// calls; on a 6-part scene at maxsteps 320 that is ~96 us/ray against ~6 us
// for the analytic primitives.  Two cases collapse ALGEBRAICALLY, not
// approximately, and the field takes both:
//   * e1 == e2 (after clamping) makes p == q, so the outer exponent ratio p/q
//     is 1 and the nested l_p(l_q) gauge becomes ONE 3-term l_p norm;
//   * e1 == e2 == 1 makes p == q == 2 and rin == a, so the field is exactly
//     |point| - a, i.e. sdSphere.
// "Exact" is not taken on faith here.  Each specialization is checked against
// (a) the test's own INDEPENDENT nested-power reference, and (b) the SHIPPED
// general branch itself, reached by nudging one exponent by 1e-11 -- a
// perturbation whose true effect on the field is ~1e-11, so a specialization
// that were merely approximate could not hide.  Conservativeness on the
// specialized paths is re-checked against the bracketed true distance.
static void TestSuperellipsoidFastPathsAreExact()
{
	std::cout << "Test 33j: the sphere / equal-exponent specializations are EXACT, not approximate" << std::endl;
	const Scalar a = 1.3;

	// (a) vs the independent nested-power reference, across the whole range
	{
		Scalar worstRef = 0, worstSphere = 0;
		for( int ei = 0; ei <= 20; ++ei ) {
			const Scalar e = Scalar(0.1) + (Scalar(2.0)-Scalar(0.1))*ei/20;
			FieldProbe* g = MakeSuperProbe( a, e, e );
			for( int i = -4; i <= 4; ++i )
			for( int j = -4; j <= 4; ++j )
			for( int k = -4; k <= 4; ++k ) {
				const Point3 p( i*0.47, j*0.53, k*0.61 );
				const Scalar ref = SEInradius(a,e,e) * ( SEGauge(p,a,e,e) - 1 );
				worstRef = std::max( worstRef, std::fabs( g->FieldAt(p) - ref ) );
			}
			safe_release( g );
		}
		FieldProbe* sph = MakeSuperProbe( a, 1.0, 1.0 );
		for( int i = -6; i <= 6; ++i )
		for( int j = -6; j <= 6; ++j )
		for( int k = -6; k <= 6; ++k ) {
			const Point3 p( i*0.43, j*0.51, k*0.59 );
			const Scalar analytic = std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z) - a;
			worstSphere = std::max( worstSphere, std::fabs( sph->FieldAt(p) - analytic ) );
		}
		safe_release( sph );
		Check( worstRef < 1e-12,
		       "MONEY -- equal-exponent path == the independent nested-power gauge (21 exponents x 729 points)" );
		Check( worstSphere < 1e-12,
		       "MONEY -- e = (1,1) path == the analytic sdSphere |p| - a exactly (2197 points)" );
	}

	// (b) vs the SHIPPED general branch, reached by an infinitesimal exponent
	// split.  Interior exponents only: nudging 0.1 down or 2.0 up would be
	// clamped straight back onto the equal-exponent path.
	{
		const Scalar es[5] = { 0.2, 0.45, 1.0, 1.5, 1.9 };
		Scalar worstSplit = 0;
		for( int ei = 0; ei < 5; ++ei ) {
			const Scalar e = es[ei];
			FieldProbe* eq  = MakeSuperProbe( a, e, e );
			FieldProbe* gen = MakeSuperProbe( a, e, e*(Scalar(1) - Scalar(1e-11)) );	// takes the general branch
			for( int i = -4; i <= 4; ++i )
			for( int j = -4; j <= 4; ++j )
			for( int k = -4; k <= 4; ++k ) {
				const Point3 p( i*0.47, j*0.53, k*0.61 );
				worstSplit = std::max( worstSplit, std::fabs( eq->FieldAt(p) - gen->FieldAt(p) ) );
			}
			safe_release( eq ); safe_release( gen );
		}
		Check( worstSplit < 1e-9,
		       "MONEY -- specialized field == the SHIPPED general branch at a 1e-11 exponent split" );
	}

	// (c) and the specializations are still CONSERVATIVE against the bracketed
	// true distance -- an exact algebraic identity would be worthless if the
	// evaluation order it saves had cost the never-overestimate property.
	{
		struct Pair { Scalar e1, e2; };
		const Pair pairs[4] = { {1.0,1.0}, {0.45,0.45}, {2.0,2.0}, {0.1,0.1} };
		Scalar worstRatio = 0, worstBracket = 0;
		for( int ip = 0; ip < 4; ++ip ) {
			const Scalar e1 = pairs[ip].e1, e2 = pairs[ip].e2;
			FieldProbe* g = MakeSuperProbe( a, e1, e2 );
			for( int i = 0; i < 5; ++i )
			for( int j = 0; j < 5; ++j )
			for( int sh = 0; sh < 3; ++sh ) {
				const Vector3 d = norm3( Scalar(i)-2 + Scalar(0.13), Scalar(j)-2 - Scalar(0.07), Scalar(i-j)*Scalar(0.6) + Scalar(0.21) );
				const Scalar shell[3] = { Scalar(0.45), Scalar(1.05), Scalar(2.30) };
				const Point3 p( d.x*a*shell[sh], d.y*a*shell[sh], d.z*a*shell[sh] );
				Point3 witness( 0, 0, 0 );
				const Scalar tru = SETrueDistance( p, a, e1, e2, &witness );
				if( tru < 1e-6 ) { continue; }
				if( SEGauge( p, a, e1, e2 ) > Scalar(1) ) {
					const Vector3 n = norm3( p.x-witness.x, p.y-witness.y, p.z-witness.z );
					const Scalar lo = ( n.x*p.x + n.y*p.y + n.z*p.z ) - SESupportH( n, a, e1, e2 );
					worstBracket = std::max( worstBracket, (tru - lo)/tru );
				}
				worstRatio = std::max( worstRatio, std::fabs( g->FieldAt(p) ) / tru );
			}
			safe_release( g );
		}
		Check( worstBracket < 1e-4, "specialized-path true-distance reference is bracketed to < 1e-4 relative" );
		Check( worstRatio <= 1 + 1e-6,
		       "MONEY -- the specialized paths never overestimate the true distance either" );
	}
}

// Test 33k: INTERSECT.  The other three ops were covered (union by every test
// above, smin and subtract by 33f); `intersect` was not, and it is the op that
// reads the superellipsoid field with the OPPOSITE sign convention through
// smaxP.  Both hard (k = 0) and smooth (k > 0) are pinned against closed
// forms: an OCTAHEDRON clipped by a SPHERE, where each surface governs on a
// different ray so a clip that silently dropped one field would show.
static void TestSuperellipsoidIntersect()
{
	std::cout << "Test 33k: superellipsoid `intersect` -- octahedron clipped by a sphere, hard and smooth" << std::endl;
	const Scalar aOct = 1.45, aSph = 1.2;

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSuperellipsoid, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), aOct, 2.0, 2.0, 0 ) );			// octahedron
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSuperellipsoid, SDFGeometry::eOpIntersect, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), aSph, 1.0, 1.0, 0 ) );			// == a sphere, exactly
	FieldProbe* g = new FieldProbe( parts );

	// membership, from the closed forms: |x|+|y|+|z| <= aOct AND |p| <= aSph
	Check( g->FieldAt( Point3(1.10,0,0) ) < 0, "intersect: inside both octahedron and sphere" );
	Check( g->FieldAt( Point3(1.35,0,0) ) > 0, "MONEY -- inside the OCTAHEDRON but outside the sphere reads OUTSIDE" );
	Check( g->FieldAt( Point3(0.55,0.55,0.55) ) > 0, "MONEY -- inside the SPHERE but outside the octahedron reads OUTSIDE" );

	// through the tracer, on two rays whose hit is governed by a DIFFERENT
	// one of the two surfaces
	{	RayIntersectionGeometric ri = MkRI( Point3(9,0,0), Vector3(-1,0,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.range, 9 - aSph, 3e-3 ), "intersect: +X ray stops at the SPHERE (1.2), not the octahedron (1.45)" ); }
	{	const Scalar s = 5.0, L = s*std::sqrt(Scalar(3));
		RayIntersectionGeometric ri = MkRI( Point3(s,s,s), norm3(-1,-1,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.range, L - aOct/std::sqrt(Scalar(3)), 3e-3 ),
		       "intersect: (1,1,1) ray stops at the OCTAHEDRON face (a/sqrt3 = 0.837), inside the sphere" ); }

	// SMOOTH intersect: smaxP >= the hard max, so the solid can only SHRINK --
	// the same ray must stop no earlier, and the composed field must still be
	// <= 1-Lipschitz (the precondition March relies on).
	std::vector<SDFGeometry::Part> sparts = parts;
	sparts[1].k = Scalar(0.35);
	FieldProbe* gs = new FieldProbe( sparts );
	{	RayIntersectionGeometric rh = MkRI( Point3(9,0,0), Vector3(-1,0,0) );
		RayIntersectionGeometric rs = MkRI( Point3(9,0,0), Vector3(-1,0,0) );
		g ->IntersectRay( rh, true, true, false );
		gs->IntersectRay( rs, true, true, false );
		Check( rs.bHit && rs.range >= rh.range - 3e-3, "smooth intersect only shrinks the solid (hit no earlier)" ); }
	{
		const Scalar h = 1e-4;
		Scalar worst = 0;
		for( int line = 0; line < 6; ++line ) {
			const Vector3 d = norm3( std::cos(line*0.9), std::sin(line*1.3)*0.7 + 0.15, std::sin(line*0.5) );
			for( int t = 0; t < 300; ++t ) {
				const Scalar u = -3.0 + 6.0*t/299.0;
				const Point3 p( d.x*u + 0.11, d.y*u - 0.05, d.z*u + 0.07 );
				const Scalar gx = ( gs->FieldAt(Point3(p.x+h,p.y,p.z)) - gs->FieldAt(Point3(p.x-h,p.y,p.z)) ) / (2*h);
				const Scalar gy = ( gs->FieldAt(Point3(p.x,p.y+h,p.z)) - gs->FieldAt(Point3(p.x,p.y-h,p.z)) ) / (2*h);
				const Scalar gz = ( gs->FieldAt(Point3(p.x,p.y,p.z+h)) - gs->FieldAt(Point3(p.x,p.y,p.z-h)) ) / (2*h);
				worst = std::max( worst, std::sqrt(gx*gx + gy*gy + gz*gz) );
			}
		}
		Check( worst <= 1 + 1e-3, "MONEY -- smooth-intersect composed |grad| never exceeds 1 (1800 points)" );
		Check( worst > 0.9,       "the sampled lines really do traverse the field" );
	}
	safe_release( gs );
	safe_release( g );
}

// Test 33l: THE KEYFRAME PATH.  Both the exponent clamp and the capsule's
// half-extent bound are argued in SDFGeometry.cpp on the grounds that
// `part<i>.size` is KEYFRAMABLE and SetIntermediateValue writes a / b / c RAW,
// so a parser-side guard cannot be the whole story.  Nothing exercised that
// claim.  This drives the real animation entry points --
// KeyframeFromParameters -> SetIntermediateValue -> RegenerateData -- and
// asserts the field and the bounds survive an exponent driven out of range
// and an extent driven through zero.
static void TestSDFKeyframedSizeStaysConservative()
{
	std::cout << "Test 33l: keyframed `part0.size` -- out-of-range exponent + extent through zero" << std::endl;
	const Scalar a = 1.3;

	// (1) an exponent animated far out of range renders exactly the CLAMPED shape
	{
		FieldProbe* anim = MakeSuperProbe( a, 1.0, 1.0 );
		IKeyframeParameter* kp = anim->KeyframeFromParameters( String("part0.size"), String("1.3 7.0 -3.0") );
		Check( kp != 0, "part0.size keyframe parameter is created" );
		if( kp ) {
			anim->SetIntermediateValue( *kp );
			anim->RegenerateData();
			safe_release( kp );
		}
		FieldProbe* clamped = MakeSuperProbe( a, 2.0, 0.1 );			// what (7, -3) must clamp to
		Scalar worst = 0;
		for( int i = -4; i <= 4; ++i )
		for( int j = -4; j <= 4; ++j )
		for( int k = -4; k <= 4; ++k ) {
			const Point3 p( i*0.55, j*0.49, k*0.61 );
			worst = std::max( worst, std::fabs( anim->FieldAt(p) - clamped->FieldAt(p) ) );
		}
		Check( worst < 1e-12, "MONEY -- keyframing e = (7, -3) renders exactly the clamped (2.0, 0.1) shape" );

		// ...and is still conservative there, against the bracketed reference
		Scalar worstRatio = 0;
		for( int i = 0; i < 4; ++i )
		for( int j = 0; j < 4; ++j )
		for( int sh = 0; sh < 3; ++sh ) {
			const Vector3 d = norm3( Scalar(i)-1.5 + Scalar(0.13), Scalar(j)-1.5 - Scalar(0.07), Scalar(i-j)*Scalar(0.6) + Scalar(0.21) );
			const Scalar shell[3] = { Scalar(0.45), Scalar(1.05), Scalar(2.30) };
			const Point3 p( d.x*a*shell[sh], d.y*a*shell[sh], d.z*a*shell[sh] );
			const Scalar tru = SETrueDistance( p, a, 2.0, 0.1 );
			if( tru < 1e-6 ) { continue; }
			worstRatio = std::max( worstRatio, std::fabs( anim->FieldAt(p) ) / tru );
		}
		Check( worstRatio <= 1 + 1e-6, "MONEY -- the keyframed out-of-range part is still conservative" );
		safe_release( clamped );
		safe_release( anim );
	}

	// (2) the RADIUS animated through zero.  a <= 0 collapses the solid to the
	// local origin, whose exact field is |p| - 0 = |p|: everywhere >= 0, so
	// nothing may read as inside, and the rebuilt bbox must stay well formed.
	{
		FieldProbe* anim = MakeSuperProbe( a, 0.45, 0.45 );
		IKeyframeParameter* kp = anim->KeyframeFromParameters( String("part0.size"), String("0 0.45 0.45") );
		if( kp ) { anim->SetIntermediateValue( *kp ); anim->RegenerateData(); safe_release( kp ); }
		Scalar worst = 0, mostNegative = 0;
		for( int i = -4; i <= 4; ++i )
		for( int j = -4; j <= 4; ++j )
		for( int k = -4; k <= 4; ++k ) {
			const Point3 p( i*0.55, j*0.49, k*0.61 );
			const Scalar d = anim->FieldAt(p);
			worst = std::max( worst, std::fabs( d - std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z) ) );
			mostNegative = std::min( mostNegative, d );
		}
		Check( worst < 1e-12,       "radius keyframed to 0: the field is exactly |p| (the collapsed solid)" );
		Check( mostNegative >= 0,   "MONEY -- a zero-radius part never reads as INSIDE anywhere" );
		BoundingBox bb = anim->GenerateBoundingBox();
		Check( bb.ll.x <= bb.ur.x && bb.ll.y <= bb.ur.y && bb.ll.z <= bb.ur.z, "rebuilt bbox is still well formed" );
		safe_release( anim );
	}

	// (3) a CAPSULE half-height eased through zero into the negative branch --
	// the exact route the ePrimCapsule bound comment cites, driven for real.
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimCapsule, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.0, 1.0, 0, 0 ) );
		SDFGeometry* geo = MakeGeom( parts );
		IKeyframeParameter* kp = geo->KeyframeFromParameters( String("part0.size"), String("1.0 -2.5 0") );
		if( kp ) { geo->SetIntermediateValue( *kp ); geo->RegenerateData(); safe_release( kp ); }
		// solid is now the cap sphere of radius 1 centred at y = +2.5; an
		// upward ray at x = 0.5 meets it at y = 2.5 - sqrt(1 - 0.25)
		const Scalar yHit = Scalar(2.5) - std::sqrt(Scalar(0.75));
		RayIntersectionGeometric ri = MkRI( Point3(0.5,-6,0), Vector3(0,1,0) );
		geo->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.range, 6 + yHit, 5e-3 ),
		       "MONEY -- a capsule half-height keyframed NEGATIVE still renders (bound did not clip it)" );
		safe_release( geo );
	}
}

// Test 35: CAPSULE with a NEGATIVE half-height -- the third member of the
// primLocalAABB under-bound family (round cone in Test 32c, round box in Test
// 34).  sdCapsuleY's core segment is clampS(y, -b, b); for b < 0 that interval
// is INVERTED (lo = |b| > hi = -|b|), so clampS returns |b| for every y < |b|
// and -|b| above it.  The solid is therefore the cap SPHERE of radius a
// centred at y = +|b|, whose top is |b| -- while the old bound's b + a is
// a - |b|, an under-bound by 2|b| - a once |b| > a/2, and the surface it
// clips out is gated away by the bbox test before the march ever runs.
// (For b < 0 the FIELD is also discontinuous at y = |b|; that is a separate,
// pre-existing property of the primitive.  The bound's only job is not to
// clip surface that the field does render, and this test pins exactly that.)
// b >= 0 is bit-identical, which the first block asserts.
static void TestCapsuleNegativeHalfHeightDoesNotClipSurface()
{
	std::cout << "Test 35: capsule with a negative half-height -- AABB must not clip the displaced cap" << std::endl;

	// well-formed first: the bound must not have MOVED for the ordinary case
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimCapsule, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 0.8, 2.0, 0, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		BoundingBox bb = g->GenerateBoundingBox();
		Check( bb.ur.y >= 2.8 && bb.ur.y <= 2.8 + 0.02, "well-formed capsule: bound unchanged at half-height + radius" );
		Check( bb.ur.x >= 0.8 && bb.ur.x <= 0.8 + 0.02, "well-formed capsule: lateral bound unchanged at the radius" );
		RayIntersectionGeometric ri = MkRI( Point3(0,20,0), Vector3(0,-1,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.range, 17.2 ), "well-formed capsule: +Y cap still hit at 2.8" );
		safe_release( g );
	}

	// degenerate: a = 1, b = -2.5.  The solid is the sphere of radius 1 at
	// y = +2.5; the OLD bound was |a + b| = 1.5, whose top sits exactly at the
	// solid's BOTTOM, so every off-axis ray was gated away before marching.
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimCapsule, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.0, -2.5, 0, 0 ) );
		SDFGeometry* g = MakeGeom( parts );
		BoundingBox bb = g->GenerateBoundingBox();
		Check( bb.ur.y >= 2.5, "MONEY -- the bound now reaches the displaced cap's top at |b| (old bound stopped at 1.5)" );

		// an off-axis upward ray meets the cap sphere at y = 2.5 - sqrt(1 - 0.25)
		const Scalar yHit = Scalar(2.5) - std::sqrt(Scalar(0.75));
		RayIntersectionGeometric ri = MkRI( Point3(0.5,-6,0), Vector3(0,1,0) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "MONEY -- the ray hits (the pre-fix bound clipped the march at y = 1.5 and it missed)" );
		Check( ri.bHit && IsClose( ri.range, 6 + yHit, 5e-3 ),
		       "MONEY -- hit range matches the TRUE displaced cap sphere, not the old clipped box" );

		// a second off-axis ray, on the other side, to rule out a lucky axis
		RayIntersectionGeometric ri2 = MkRI( Point3(-0.8,-6,0), Vector3(0,1,0) );
		g->IntersectRay( ri2, true, true, false );
		Check( ri2.bHit && IsClose( ri2.range, 6 + Scalar(2.5) - std::sqrt(Scalar(1.0-0.64)), 5e-3 ),
		       "second off-axis ray lands on the displaced cap too" );
		safe_release( g );
	}
}

// Test 34: ROUNDBOX with `round` LARGER than a half-extent -- the sibling of
// the round-cone AABB bug (Test 32c), found by auditing primLocalAABB for the
// same defect class while adding the superellipsoid.  sdRoundBox shrinks the
// core box by r and then INFLATES by r:
//     sdBox( ..., max(bx-r,0), max(by-r,0), max(bz-r,0) ) - r
// so its surface reaches max(bx, r) along x, not bx.  Once r exceeds a
// half-extent the solid IS the sphere of radius r -- and the local AABB, which
// used the half-extents alone, under-bounded it and CLIPPED real surface: a ray
// that should hit is gated away by the bbox test before it ever marches.
// The bound is now max(half-extent, round) per axis, which is BIT-IDENTICAL
// for every well-formed rounded box (r <= min half-extent, the only regime any
// existing scene authors) and only widens in the degenerate one.
static void TestRoundBoxLargeRoundDoesNotClipSurface()
{
	std::cout << "Test 34: roundbox with round > half-extent -- AABB must not clip the inflated surface" << std::endl;

	// well-formed first: the bound must not have MOVED for the ordinary case
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimRoundBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2.0, 2.0, 2.0, 0.5 ) );
		SDFGeometry* g = MakeGeom( parts );
		BoundingBox bb = g->GenerateBoundingBox();
		Check( bb.ur.x >= 2.0 && bb.ur.x <= 2.0 + 0.02, "well-formed roundbox: bound unchanged at the half-extent" );
		RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit && IsClose( ri.range, 18.0 ), "well-formed roundbox: +Z face still hit at the half-extent" );
		safe_release( g );
	}

	// degenerate: r = 3 against half-extents of 1 -- the solid is EXACTLY the
	// sphere of radius 3 (the core box collapses to a point), so a ray down
	// the +Z axis must hit at z = 3, and the old bound (top at z = 1) clipped it.
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimRoundBox, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 1.0, 1.0, 1.0, 3.0 ) );
		SDFGeometry* g = MakeGeom( parts );
		RayIntersectionGeometric ri = MkRI( Point3(0,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "MONEY -- the ray hits (the pre-fix bound clipped the march at z = 1 and it missed)" );
		Check( ri.bHit && IsClose( ri.range, 17.0, 0.02 ),
		       "MONEY -- hit range matches the TRUE inflated surface (sphere of radius 3), not the old clipped box" );
		// off-axis too: at (x,y) = (1.8, 0) the sphere top is sqrt(9-3.24) = 2.4
		RayIntersectionGeometric ri2 = MkRI( Point3(1.8,0,20), Vector3(0,0,-1) );
		g->IntersectRay( ri2, true, true, false );
		Check( ri2.bHit && IsClose( ri2.range, 20.0 - std::sqrt(Scalar(9.0-3.24)), 0.02 ),
		       "off-axis ray lands on the inflated surface too" );
		safe_release( g );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 36: the smin AABB bulge is k/4 PER PART, not k on the RUNNING BOX.
//
// ComputeBounds used to union the raw part box and then pad the WHOLE running
// box by the FULL blend radius k, once per smin part.  Both halves were wrong:
//   * sminP(a,b,k) = min(a,b) - h*h*k/4 with h in [0,1], so a smin dips at most
//     k/4 below the hard min -- a QUARTER of k, straight out of the formula.
//   * the pad landed on the running box, so an N-part blend chain accumulated
//     sum(k) per side rather than reserving one shared budget.
// A 9-part smin creature (parts extent 0.375 x 0.169 x 0.353, sum(k) = 0.136)
// reported 0.622 x 0.413 x 0.575 -- ~1.7x per axis.  Nothing about that is
// cosmetic: the agent surface's isolate auto-framer fits the reported box (so
// the subject rendered at ~4.5 % of frame instead of ~40 %) and its
// largest-diagonal object pick reads the same number.
//
// The two halves of this test pull in OPPOSITE directions on purpose.  36a
// pins CONSERVATIVENESS (nothing solid may escape the box) so the tightening
// cannot be "fixed" by simply shrinking; 36b pins TIGHTNESS against the
// analytic hard-union extent plus the theoretical sum(k)/4 budget, and is the
// half the old code fails.
//////////////////////////////////////////////////////////////////////

// 36a -- CONSERVATIVE.  A deliberately hostile fold: rotated, anisotropically
// scaled superellipsoids (including the e = 2 octahedral corner, where the
// field under-reports local distance by a factor of 2 and the per-part
// inflation has to carry an F > 1 stretch), a capsule, a round cone, an
// intersect clip, a subtract, and a smin AFTER the clip so the fold's
// order-awareness is live.  Dense-sample a box 1.5x the reported one and
// assert no point of the solid { Map <= 0 } sits outside the report.
static void TestSminBoundStaysConservative()
{
	std::cout << "Test 36a: multi-part smin bound still contains the whole solid" << std::endl;

	std::vector<SDFGeometry::Part> parts;
	// seed (parser guarantees the first op is additive)
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 0.6, 0,0,0 ) );
	// rotated + anisotropic superellipsoid, e2 > 1 (F = 2^0.2)
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSuperellipsoid, SDFGeometry::eOpSmin, 0.50,
		Point3(0.8,0.2,0.0), 0,0,35, Vector3(1.3,0.6,0.9), 0.50, 0.6, 1.4, 0 ) );
	// the octahedral corner e1 = e2 = 2 -- the largest stretch the family has
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSuperellipsoid, SDFGeometry::eOpSmin, 0.40,
		Point3(-0.7,0.3,0.4), 20,40,10, Vector3(0.8,1.5,0.7), 0.45, 2.0, 2.0, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimCapsule, SDFGeometry::eOpSmin, 0.45,
		Point3(0.2,-0.8,0.3), 0,0,60, Vector3(1,1,1), 0.25, 0.5, 0, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimRoundCone, SDFGeometry::eOpSmin, 0.30,
		Point3(-0.3,0.9,-0.5), 15,0,25, Vector3(1.1,1.0,0.9), 0.30, 0.15, 0.70, 0 ) );
	// clip, then carve, then a LATER smin -- the lobe added after the clip must
	// survive, and it must carry its own budget
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimBox, SDFGeometry::eOpIntersect, 0.20,
		Point3(0,0,0), 0,0,0, Vector3(1,1,1), 2.0, 2.0, 1.2, 0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSubtract, 0.25,
		Point3(0.5,0.5,0.5), 0,0,0, Vector3(1,1,1), 0.40, 0,0,0 ) );
	parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.35,
		Point3(1.4,-0.4,0.2), 0,0,0, Vector3(1,1,1), 0.35, 0,0,0 ) );

	FieldProbe* g = new FieldProbe( parts );
	const BoundingBox bb = g->GenerateBoundingBox();

	const Point3 c( (bb.ll.x+bb.ur.x)*Scalar(0.5), (bb.ll.y+bb.ur.y)*Scalar(0.5), (bb.ll.z+bb.ur.z)*Scalar(0.5) );
	const Scalar hx = (bb.ur.x-bb.ll.x)*Scalar(0.75);	// 1.5x the box, about its centre
	const Scalar hy = (bb.ur.y-bb.ll.y)*Scalar(0.75);
	const Scalar hz = (bb.ur.z-bb.ll.z)*Scalar(0.75);

	const int    N = 72;
	int          escapes = 0;
	Scalar       worstDepth = 0, worstOut = 0;
	Point3       worstP(0,0,0);
	int          inside = 0;
	for( int ix = 0; ix <= N; ++ix )
	for( int iy = 0; iy <= N; ++iy )
	for( int iz = 0; iz <= N; ++iz )
	{
		const Point3 p( c.x + hx*(Scalar(2)*ix/N - 1),
		                c.y + hy*(Scalar(2)*iy/N - 1),
		                c.z + hz*(Scalar(2)*iz/N - 1) );
		const Scalar d = g->FieldAt( p );
		if( d > 0 ) continue;
		++inside;
		// how far OUTSIDE the reported box does this solid point sit?
		const Scalar ox = std::max( bb.ll.x - p.x, p.x - bb.ur.x );
		const Scalar oy = std::max( bb.ll.y - p.y, p.y - bb.ur.y );
		const Scalar oz = std::max( bb.ll.z - p.z, p.z - bb.ur.z );
		const Scalar out = std::max( ox, std::max( oy, oz ) );
		if( out > 0 ) {
			++escapes;
			if( out > worstOut ) { worstOut = out; worstDepth = d; worstP = p; }
		}
	}
	std::cout << "    solid grid points: " << inside << ", escaping the bbox: " << escapes << std::endl;
	std::cout << "    bbox = [" << bb.ll.x << "," << bb.ur.x << "] x ["
	          << bb.ll.y << "," << bb.ur.y << "] x [" << bb.ll.z << "," << bb.ur.z << "]" << std::endl;
	if( escapes ) {
		std::cout << "    worst escape " << worstOut << " at (" << worstP.x << "," << worstP.y << ","
		          << worstP.z << "), Map = " << worstDepth << std::endl;
	}
	Check( inside > 1000, "the hostile fixture actually has a solid to bound (sanity)" );
	Check( escapes == 0, "MONEY -- no point of { Map <= 0 } escapes the reported AABB" );
	safe_release( g );
}

// 36b -- TIGHT.  Uniform scale, exact-SDF primitives, five spheres chained by
// four smins with a large k.  The bound may exceed the analytic HARD-union
// extent by at most 2*(sum(k)/4) -- one shared budget per side -- plus the
// fixed safety pad.  The old form padded the running box by k four times over,
// i.e. 2*sum(k) = 4x the allowance, and fails every axis.
static void TestSminBoundIsTight()
{
	std::cout << "Test 36b: smin bound spends sum(k)/4 per side, not sum(k)" << std::endl;

	const Scalar r = 0.5, k = 0.4, step = 0.6;
	const int    n = 5;			// 1 union seed + 4 smins
	std::vector<SDFGeometry::Part> parts;
	Scalar sumK = 0;
	for( int i = 0; i < n; ++i ) {
		const bool first = ( i == 0 );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere,
			first ? SDFGeometry::eOpUnion : SDFGeometry::eOpSmin, first ? Scalar(0) : k,
			Point3( step*i, 0, 0 ), 0,0,0, Vector3(1,1,1), r, 0,0,0 ) );
		if( !first ) sumK += k;
	}

	SDFGeometry* g = MakeGeom( parts );
	const BoundingBox bb = g->GenerateBoundingBox();

	// analytic HARD-union extent of the five spheres
	const Scalar hardX = ( step*(n-1) + r ) - ( -r );
	const Scalar hardY = 2*r, hardZ = 2*r;
	// The whole allowance: one shared sum(k)/4 budget per side, plus the safety
	// pad ComputeBounds always applies.  The pad is DERIVED from the same
	// formula the implementation uses -- pad = max(1e-3, 3*eps0) with
	// eps0 = max(diag0*epsFrac, 1e-6) off the UNPADDED box -- rather than
	// hardcoded, so a deliberate future pad change is absorbed here instead of
	// silently eating the budget headroom this test is measuring.  (The
	// unpadded box the implementation sees is the hard union grown by one
	// budget per side, which for these uniform-scale exact-SDF spheres is
	// exactly what the analytic form below reconstructs.)  epsFrac = 1e-5 comes
	// from MakeGeom, so on this ~4-unit box the 1e-3 floor wins -- but the test
	// no longer depends on that staying true.
	//
	// NOTE ON HEADROOM: X clears the cap by ~0.3, but Y and Z land ~0.002 under
	// it, and that is EXACT rather than lucky -- a smin part whose box is
	// concentric with the running box in Y/Z spends its whole budget on those
	// axes, so hard + 2*sum(k)/4 IS the theoretical bound there and the pad is
	// the only slack.  Deliberate: a cap loosened to "feel safer" would stop
	// measuring the budget at all.  If this ever fails by a hair on Y/Z, look
	// for a pad change, not for a budget bug.
	const Scalar bud   = sumK*Scalar(0.25);
	const Scalar unpX  = hardX + 2*bud, unpY = hardY + 2*bud, unpZ = hardZ + 2*bud;
	const Scalar diag0 = std::sqrt( unpX*unpX + unpY*unpY + unpZ*unpZ );
	const Scalar eps0  = std::max( diag0*Scalar(1e-5), Scalar(1e-6) );	// MakeGeom's epsFrac
	const Scalar pad   = std::max( Scalar(1e-3), Scalar(3)*eps0 );
	const Scalar allow = 2*bud + 2*pad + Scalar(1e-9);

	const Scalar gotX = bb.ur.x-bb.ll.x, gotY = bb.ur.y-bb.ll.y, gotZ = bb.ur.z-bb.ll.z;
	std::cout << "    sum(k) = " << sumK << ", budget/side = " << bud
	          << ", pad = " << pad << ", allowance = " << allow << std::endl;
	std::cout << "    extent X " << gotX << " (hard " << hardX << ", cap " << hardX+allow << ")" << std::endl;
	std::cout << "    extent Y " << gotY << " (hard " << hardY << ", cap " << hardY+allow << ")" << std::endl;
	std::cout << "    extent Z " << gotZ << " (hard " << hardZ << ", cap " << hardZ+allow << ")" << std::endl;

	Check( gotX >= hardX, "bound still contains the hard-union X extent" );
	Check( gotY >= hardY, "bound still contains the hard-union Y extent" );
	Check( gotZ >= hardZ, "bound still contains the hard-union Z extent" );
	Check( gotX <= hardX + allow, "MONEY -- X extent within hard + 2*sum(k)/4 (old form spent 2*sum(k))" );
	Check( gotY <= hardY + allow, "MONEY -- Y extent within hard + 2*sum(k)/4 (old form spent 2*sum(k))" );
	Check( gotZ <= hardZ + allow, "MONEY -- Z extent within hard + 2*sum(k)/4 (old form spent 2*sum(k))" );
	safe_release( g );
}

// 36c -- the LIVE SHAPE, scaled down to the regime that produced the report:
// nine small smin parts, k comparable to the part size.  Pins the aggregate
// number the auto-framer consumes (the box diagonal) rather than a per-axis
// extent, because that is what the regression was measured through.
static void TestSminBoundOnSmallBlobChain()
{
	std::cout << "Test 36c: nine-part small-blob chain -- diagonal near the parts' own" << std::endl;

	const Scalar k = 0.015;		// 9 parts -> sum(k) = 0.12, budget/side = 0.03
	std::vector<SDFGeometry::Part> parts;
	const Scalar px[9] = { 0.00, 0.07,-0.06, 0.11,-0.10, 0.04,-0.03, 0.14,-0.13 };
	const Scalar py[9] = { 0.00, 0.03,-0.02, 0.05, 0.01,-0.04, 0.06,-0.01, 0.02 };
	const Scalar pz[9] = { 0.00,-0.05, 0.06, 0.02,-0.07, 0.08,-0.06, 0.03, 0.05 };
	for( int i = 0; i < 9; ++i ) {
		const bool first = ( i == 0 );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere,
			first ? SDFGeometry::eOpUnion : SDFGeometry::eOpSmin, first ? Scalar(0) : k,
			Point3( px[i], py[i], pz[i] ), 0,0,0, Vector3(1,1,1), 0.06, 0,0,0 ) );
	}
	SDFGeometry* g = MakeGeom( parts );
	const BoundingBox bb = g->GenerateBoundingBox();

	// analytic hard union of nine r = 0.06 spheres at the listed centres
	Scalar lo[3] = { 1e9, 1e9, 1e9 }, hi[3] = { -1e9, -1e9, -1e9 };
	for( int i = 0; i < 9; ++i ) {
		const Scalar cc[3] = { px[i], py[i], pz[i] };
		for( int a = 0; a < 3; ++a ) {
			lo[a] = std::min( lo[a], cc[a]-Scalar(0.06) );
			hi[a] = std::max( hi[a], cc[a]+Scalar(0.06) );
		}
	}
	const Scalar hardDiag = std::sqrt( (hi[0]-lo[0])*(hi[0]-lo[0]) + (hi[1]-lo[1])*(hi[1]-lo[1]) + (hi[2]-lo[2])*(hi[2]-lo[2]) );
	const Scalar gotDiag  = std::sqrt( (bb.ur.x-bb.ll.x)*(bb.ur.x-bb.ll.x)
	                                 + (bb.ur.y-bb.ll.y)*(bb.ur.y-bb.ll.y)
	                                 + (bb.ur.z-bb.ll.z)*(bb.ur.z-bb.ll.z) );
	std::cout << "    hard-union extent " << (hi[0]-lo[0]) << " x " << (hi[1]-lo[1]) << " x " << (hi[2]-lo[2])
	          << " (diag " << hardDiag << ")" << std::endl;
	std::cout << "    reported  extent " << (bb.ur.x-bb.ll.x) << " x " << (bb.ur.y-bb.ll.y) << " x " << (bb.ur.z-bb.ll.z)
	          << " (diag " << gotDiag << "), ratio " << gotDiag/hardDiag << std::endl;
	// budget/side = sum(k)/4 = 0.03.  Growing the 0.39 x 0.22 x 0.27 hard box
	// by 0.06 per axis caps the diagonal at 0.624, i.e. 1.194x -- so 1.25x is
	// above the theoretical bound with margin, while the MEASURED numbers on
	// either side of the fix are 1.079x (fixed) and 1.498x (the old
	// running-box-by-full-k form).  Comfortably discriminating both ways.
	Check( gotDiag >= hardDiag, "diagonal still contains the hard union" );
	Check( gotDiag <= hardDiag*Scalar(1.25), "MONEY -- diagonal within 1.25x the parts' own (measured 1.079x fixed, 1.498x pre-fix)" );
	safe_release( g );
}

// 36d -- DEGENERATE INPUTS to the per-part inflation.  Both reach the field
// through KEYFRAMING (`part<i>.scale` and `part<i>.blend` are animatable, and
// SetIntermediateValue writes them RAW), so neither is a synthetic worry.
//
// (a) SUB-FLOOR SCALE.  partEval divides by pt.invScale and multiplies by
//     pt.minScale, both built from RecomputePartDerived's
//     `fabs(s) > 1e-9 ? s : 1e-9` flooring -- so a part authored at scale 0
//     behaves as though it were scaled by the FLOOR, and its world
//     tau-sublevel still reaches tau.  The inflation divides by that same
//     floored minScale; if the corner transform were to multiply by the RAW
//     scale instead, it would scale the inflated box back to a point and
//     under-bound by the whole budget.
// (b) NON-FINITE BLEND.  An infinite k makes the suffix budget infinite and
//     the inflation infinite.  Under strict IEEE the corner transform then
//     evaluates 0*inf for every zero rotation-matrix entry, every corner comes
//     out NaN, and min/max against NaN leaves worldAABB's +-RISE_INFINITY
//     seeds in place -- an INVERTED box (ll = +INF, ur = -INF) that reaches
//     the TLAS / octree as a NaN centroid.
//
//     MEASURED CAVEAT, because it changes what this test can assert: on THIS
//     build (macOS, -ffast-math with -fno-finite-math-only) the unguarded form
//     does NOT invert -- the 0*inf terms fold away and the box degrades to
//     [-inf, +inf], which passes an `ll <= ur` check.  So well-formedness
//     ALONE cannot red-prove the guard here.  The assertion is therefore
//     FINITENESS, which is what the guard actually delivers and which catches
//     BOTH degradations: an inverted box has ll = +INF, so it fails the finite
//     test too, on whichever platform produces it.
//
// Both halves assert well-formedness (ll <= ur on every axis) -- the property
// every downstream consumer assumes -- and (b) additionally asserts the box is
// finite.
static void TestSminBoundDegenerateInputs()
{
	std::cout << "Test 36d: sub-floor scale + non-finite blend still give a well-formed box" << std::endl;

	// (a) a zero-scale part inside an smin chain
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 0.5, 0,0,0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin, 0.4,
			Point3(0.6,0,0), 0,0,0, Vector3(0,0,0), 0.5, 0,0,0 ) );
		FieldProbe* g = new FieldProbe( parts );
		const BoundingBox bb = g->GenerateBoundingBox();
		std::cout << "    zero-scale part: bbox = [" << bb.ll.x << "," << bb.ur.x << "] x ["
		          << bb.ll.y << "," << bb.ur.y << "] x [" << bb.ll.z << "," << bb.ur.z << "]" << std::endl;
		Check( bb.ll.x <= bb.ur.x && bb.ll.y <= bb.ur.y && bb.ll.z <= bb.ur.z,
		       "zero-scale smin part: the reported box is well-formed" );

		// and it still CONTAINS the solid: same dense sweep as 36a, over 1.5x
		// the reported box.  This is the half that catches a corner transform
		// that multiplied by the raw (zero) scale after inflating by the
		// floored one -- the box would collapse and the seed sphere would
		// escape it.
		const Point3 c( (bb.ll.x+bb.ur.x)*Scalar(0.5), (bb.ll.y+bb.ur.y)*Scalar(0.5), (bb.ll.z+bb.ur.z)*Scalar(0.5) );
		const Scalar hx = (bb.ur.x-bb.ll.x)*Scalar(0.75), hy = (bb.ur.y-bb.ll.y)*Scalar(0.75), hz = (bb.ur.z-bb.ll.z)*Scalar(0.75);
		const int N = 56;
		int escapes = 0, inside = 0;
		Scalar worstOut = 0;
		for( int ix = 0; ix <= N; ++ix )
		for( int iy = 0; iy <= N; ++iy )
		for( int iz = 0; iz <= N; ++iz )
		{
			const Point3 p( c.x + hx*(Scalar(2)*ix/N - 1), c.y + hy*(Scalar(2)*iy/N - 1), c.z + hz*(Scalar(2)*iz/N - 1) );
			if( g->FieldAt( p ) > 0 ) continue;
			++inside;
			const Scalar out = std::max( std::max( bb.ll.x - p.x, p.x - bb.ur.x ),
			                   std::max( std::max( bb.ll.y - p.y, p.y - bb.ur.y ),
			                             std::max( bb.ll.z - p.z, p.z - bb.ur.z ) ) );
			if( out > 0 ) { ++escapes; worstOut = std::max( worstOut, out ); }
		}
		std::cout << "    zero-scale part: solid grid points " << inside << ", escaping " << escapes
		          << " (worst " << worstOut << ")" << std::endl;
		Check( inside > 500, "zero-scale fixture still has a solid to bound (sanity)" );
		Check( escapes == 0, "MONEY -- a sub-floor-scale smin part does not let the solid escape the box" );
		safe_release( g );
	}

	// (b) an infinite blend radius
	{
		std::vector<SDFGeometry::Part> parts;
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
			Point3(0,0,0), 0,0,0, Vector3(1,1,1), 0.5, 0,0,0 ) );
		parts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin,
			std::numeric_limits<Scalar>::infinity(),
			Point3(0.6,0,0), 0,0,0, Vector3(1,1,1), 0.5, 0,0,0 ) );
		SDFGeometry* g = MakeGeom( parts );
		const BoundingBox bb = g->GenerateBoundingBox();
		std::cout << "    infinite k: bbox = [" << bb.ll.x << "," << bb.ur.x << "] x ["
		          << bb.ll.y << "," << bb.ur.y << "] x [" << bb.ll.z << "," << bb.ur.z << "]" << std::endl;
		Check( bb.ll.x <= bb.ur.x && bb.ll.y <= bb.ur.y && bb.ll.z <= bb.ur.z,
		       "an infinite blend radius gives a well-formed box, not an INVERTED one" );
		const bool finiteBox =
			RISE::IsFiniteDouble( bb.ll.x ) && RISE::IsFiniteDouble( bb.ur.x ) &&
			RISE::IsFiniteDouble( bb.ll.y ) && RISE::IsFiniteDouble( bb.ur.y ) &&
			RISE::IsFiniteDouble( bb.ll.z ) && RISE::IsFiniteDouble( bb.ur.z );
		Check( finiteBox,
		       "MONEY -- an infinite blend radius is dropped from the budget, so the box stays FINITE "
		       "(without the guard it degrades to [-inf,inf] here, and inverts under strict IEEE)" );
		// and it is the box the same parts would give with the smin's blend
		// disabled -- the budget contributes nothing, nothing else moves
		// hard union of spheres r=0.5 at x=0 and x=0.6 is [-0.5, 1.1]; the only
		// growth left is ComputeBounds' safety pad (1e-3 on this small box)
		Check( bb.ur.x <= Scalar(1.1) + Scalar(2e-3) && bb.ll.x >= Scalar(-0.5) - Scalar(2e-3),
		       "the finite box is the ordinary hard-union one (spheres at 0 and 0.6, r 0.5, + pad)" );
		safe_release( g );
	}
}

int main()
{
	std::cout << "SDFGeometryTest" << std::endl;
	std::cout << "===============" << std::endl;

	// Installed for the whole run, before any test: Test 20b counts
	// ParsePartLines' degenerate-shape-parameters advisory, which is a
	// WARNING (the part still parses) and so never shows up in a
	// ParsePartLines bool return or in `parts` itself.
	{
		CapturingLogPrinter* owned = new CapturingLogPrinter( "has degenerate shape parameters" );
		RISE::GlobalLogPriv()->AddPrinter( owned );
		g_degenerateShapeWarn = owned;   // AddPrinter addref'd it; keep a raw read handle
		safe_release( owned );           // drop OUR construction ref (safe_release nulls its arg)
	}

	TestSphereMatchesAnalytic();
	TestBoxMatchesAnalytic();
	TestSmoothMin();
	TestBoundingBoxContainsSurface();
	TestBoundingBoxOpAware();
	TestWideThinTopFaceEntry();
	TestHeightfieldFlatTopEntry();
	TestHeightfieldDiskDomain();
	TestShadowQuery();
	TestMiss();
	TestInsideStartExits();
	TestTransform();
	TestNonUniformScale();
	TestSubtractHardCarvesCavity();
	TestSubtractSmoothNoTunnel();
	TestIntersectLens();
	TestIntersectionOnlyFrontBack();
	TestFrontBackGuardConsistency();
	TestSurfaceArea();
	TestUniformRandomPointOnSurface();
	TestTessellateToMesh();
	TestSamplingOnCompositeFields();
	TestNEEIntegrandClosedForm();
	TestNEEIntegrandAnalyticControl();
	TestParsePartLines();
	TestDegenerateShapeParamsWarn();
	TestFirstOpRule();
	TestMissedComponentDetector();
	TestCorrectedSamplingArea();
	TestKeyframePartPosition();
	TestKeyframePartSizeAndArea();
	TestKeyframePartRotation();
	TestKeyframeBlendAndScale();
	TestKeyframeRejectsBadParams();
	TestKeyframeHeightfieldScale();
	TestRoundConeAABBIdentityForWellFormedCones();
	TestRoundConeDegenerateAABBDoesNotClipSurface();
	TestRoundConeDegenerateOnAxisFieldMatchesSphere();
	TestSuperellipsoidIsSphereAtUnitExponents();
	TestSuperellipsoidBoxLimit();
	TestSuperellipsoidOctahedronAndCylinder();
	TestSuperellipsoidConservativeDistance();
	TestSuperellipsoidSphereTraceRoundTrip();
	TestSuperellipsoidCompositionStaysLipschitz();
	TestSuperellipsoidExponentClamp();
	TestSuperellipsoidGrammarAndBounds();
	TestSuperellipsoidNonFiniteCoordinates();
	TestSuperellipsoidFastPathsAreExact();
	TestSuperellipsoidIntersect();
	TestSDFKeyframedSizeStaysConservative();
	TestRoundBoxLargeRoundDoesNotClipSurface();
	TestCapsuleNegativeHalfHeightDoesNotClipSurface();
	TestSminBoundStaysConservative();
	TestSminBoundIsTight();
	TestSminBoundOnSmallBlobChain();
	TestSminBoundDegenerateInputs();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
