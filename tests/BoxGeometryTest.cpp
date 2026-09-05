//////////////////////////////////////////////////////////////////////
//
//  BoxGeometryTest.cpp - regression guard for debt 25's root-cause fix
//  (docs/CLOTH_FABRIC_DESIGN.md section 15): `BoxGeometry::IntersectRay`
//  / `IntersectRay_IntersectionOnly` used to report a self-hit root for
//  a ray whose origin was PUBLISHED (Object::IntersectRay backs the hit
//  point off along the ray by SURFACE_INTERSEC_ERROR = 1e-12) on one of
//  the box's own faces -- a tiny, direction-dependent root that
//  straddled NEARZERO and either self-occluded the origin's own face or
//  discarded the box entirely (see `DropSelfHitRoot` in
//  src/Library/Geometry/BoxGeometry.cpp for the full mechanism).
//
//  This file exercises the fixed primitive DIRECTLY (no rasterizer, no
//  scene parse) at exactly the two publish conventions
//  `Object::IntersectRay` uses -- `p = ray.PointAtLength(ri.range -
//  1e-12)` from an entry (outside) hit and from an exit (inside) hit --
//  across a sweep of continuation-ray angles down to grazing incidence,
//  the three front/back-face flag combinations the fix's exit/entry
//  predicate governs ((false,false) rejects regardless), and one
//  coordinate-scale check (~1000) to confirm
//  the self-hit band's per-axis tolerance
//  (`4 * NEARZERO + 64 * DBL_EPSILON * |origin.axis|`) tracks genuine
//  faces at any scale rather than a fixed absolute threshold.
//
//  Style follows tests/ClippedPlaneGeometryTest.cpp: plain asserts,
//  the geometry class constructed directly, `RayIntersectionGeometric(
//  Ray(origin,dir), nullRasterizerState )` for the full-info entry
//  point.
//
//  Author: Claude Sonnet 5
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <cmath>
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CircularDiskGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

static bool IsClose( const Scalar a, const Scalar b, const Scalar epsilon = 1e-6 )
{
	return std::fabs(a - b) <= epsilon;
}

static bool IsVectorClose( const Vector3& a, const Vector3& b, const Scalar epsilon = 1e-6 )
{
	return IsClose(a.x, b.x, epsilon) &&
		IsClose(a.y, b.y, epsilon) &&
		IsClose(a.z, b.z, epsilon);
}

static RayIntersectionGeometric MakeIntersection( const Point3& origin, const Vector3& dir )
{
	return RayIntersectionGeometric( Ray(origin, dir), nullRasterizerState );
}

//////////////////////////////////////////////////////////////////////
// 1+2. Origin published from an OUTSIDE (entry) hit on the +Z face:
// p = ray.PointAtLength(ri.range - 1e-12), ~1e-12 outside the box.
//
// A sweep of INGOING continuation-ray angles from p, parameterized by
// cos(theta) = the angle to the +Z face normal, from normal incidence
// (1.0) down to grazing (0.02).  Below tan(theta) = halfWidth/depth =
// 1.4 (scale-invariant: both scale together) the ray's lateral drift
// while crossing the box's thin depth stays under the half-width and
// it exits through the OPPOSITE (-Z) face; above that angle the
// lateral drift exceeds the half-width first and it exits through the
// +X SIDE face instead.  Both are legitimate "the far root survives"
// outcomes for the fix (DropSelfHitRoot swaps in whatever the OTHER
// root is, not specifically the parallel opposite face), so the sweep
// deliberately covers both regimes with independently hand-derived
// expected ranges/normals -- not a call back into the code under test.
//
// Then: an OUTGOING ray from the same p (continuing through the face
// away from the box) must report NO HIT -- nothing remains ahead.
//////////////////////////////////////////////////////////////////////
static void RunFrontFaceIngoingSweep( double scale )
{
	std::cout << "Testing BoxGeometry ingoing/outgoing from a published front-face (outside) origin (scale "
		<< scale << ")..." << std::endl;

	const double W = 2.8 * scale, H = 2.8 * scale, D = 1.0 * scale;
	BoxGeometry* pBox = new BoxGeometry( W, H, D );

	// Oblique camera-like ray from far outside, aimed exactly at the +Z
	// face center so the analytic exit derivation below (which assumes
	// p == (0,0,0.5*scale) to the ~1e-12*scale backoff) holds.
	const Point3 camOrigin( 2.1 * scale, -1.7 * scale, 4.0 * scale );
	const Point3 target( 0.0, 0.0, 0.5 * scale );
	const Vector3 camDir = Vector3Ops::Normalize(
		Vector3( target.x - camOrigin.x, target.y - camOrigin.y, target.z - camOrigin.z ) );
	Ray camRay( camOrigin, camDir );

	RayIntersectionGeometric riCam = MakeIntersection( camOrigin, camDir );
	pBox->IntersectRay( riCam, true, false, false );
	assert( riCam.bHit );
	assert( IsVectorClose( riCam.vNormal, Vector3(0.0, 0.0, 1.0), 1e-9 ) );

	const Point3 p = camRay.PointAtLength( riCam.range - 1e-12 );

	const double halfWidth = 1.4 * scale;
	const double depth = 1.0 * scale;
	const double cosVals[] = { 1.0, 0.9, 0.7, 0.5, 0.3, 0.1, 0.02 };

	for( double cosT : cosVals )
	{
		const double sinT = std::sqrt( std::fmax( 0.0, 1.0 - cosT * cosT ) );

		bool viaOpposite;
		double expectedRange;
		Vector3 expectedNormal;
		if( sinT < 1e-12 ) {
			viaOpposite = true;
			expectedRange = depth / cosT;
			expectedNormal = Vector3( 0.0, 0.0, -1.0 );
		} else {
			const double tZFar  = depth / cosT;
			const double tXSide = halfWidth / sinT;
			viaOpposite = ( tZFar <= tXSide );
			expectedRange = viaOpposite ? tZFar : tXSide;
			expectedNormal = viaOpposite ? Vector3(0.0, 0.0, -1.0) : Vector3(1.0, 0.0, 0.0);
		}

		const Vector3 dir( sinT, 0.0, -cosT );
		const double tol = 1e-6 * expectedRange + 1e-9;

		RayIntersectionGeometric ri = MakeIntersection( p, dir );
		pBox->IntersectRay( ri, true, true, false );
		assert( ri.bHit );
		assert( ri.range >= 1e-6 );	// never the ~1e-12 self-hit root
		assert( IsClose( ri.range, expectedRange, tol ) );
		assert( IsVectorClose( ri.vNormal, expectedNormal, 1e-9 ) );

		const bool hitOnly = pBox->IntersectRay_IntersectionOnly(
			Ray(p, dir), expectedRange * 2.0, true, true );
		assert( hitOnly );
	}

	// OUTGOING: continuing straight away from the box through the same
	// face p sits on -- nothing remains ahead, so this must be NO HIT.
	// (Degenerate-case guard only: p sits OUTSIDE the box here, so both
	// slab roots are already behind the ray and the pre-fix code passes
	// this too.  The pre-fix self-occlusion failure mode is pinned by the
	// INSIDE-published outgoing check in RunBackFaceIngoingOutgoing.)
	{
		const Vector3 outDir( 0.0, 0.0, 1.0 );
		RayIntersectionGeometric ri = MakeIntersection( p, outDir );
		pBox->IntersectRay( ri, true, true, false );
		assert( !ri.bHit );

		const bool hitOnly = pBox->IntersectRay_IntersectionOnly(
			Ray(p, outDir), 1e6 * scale, true, true );
		assert( !hitOnly );
	}

	safe_release( pBox );
	std::cout << "  ...Passed! (scale " << scale << ")" << std::endl;
}

//////////////////////////////////////////////////////////////////////
// 3. Origin published from an INSIDE (exit) hit on the -Z face:
// p2 = ray.PointAtLength(ri.range - 1e-12), which now lies ~1e-12
// INSIDE the box (backing off an exit hit moves toward where the ray
// came from, i.e. further into the box it is leaving).
//
// OUTGOING (continuing the same direction, finishing the exit) must be
// NO HIT.  INGOING (reversing direction, back into the box) must hit
// the OPPOSITE (+Z) face, not a self-hit on the face p2 sits on.
//////////////////////////////////////////////////////////////////////
static void RunBackFaceIngoingOutgoing( double scale )
{
	std::cout << "Testing BoxGeometry ingoing/outgoing from a published back-face (inside) origin (scale "
		<< scale << ")..." << std::endl;

	const double W = 2.8 * scale, H = 2.8 * scale, D = 1.0 * scale;
	BoxGeometry* pBox = new BoxGeometry( W, H, D );

	// From the box center toward the -Z face, with a small lateral
	// component -- small enough that the lateral drift over the box's
	// depth stays well under its half-width, so this genuinely exits
	// through the -Z face and not a side.
	const Point3 insideOrigin( 0.0, 0.0, 0.0 );
	const Vector3 dir2 = Vector3Ops::Normalize( Vector3( 0.05, 0.03, -1.0 ) );
	Ray insideRay( insideOrigin, dir2 );

	RayIntersectionGeometric riInside = MakeIntersection( insideOrigin, dir2 );
	pBox->IntersectRay( riInside, true, true, false );
	assert( riInside.bHit );
	assert( IsVectorClose( riInside.vNormal, Vector3(0.0, 0.0, -1.0), 1e-9 ) );

	const Point3 p2 = insideRay.PointAtLength( riInside.range - 1e-12 );

	// OUTGOING: same direction, finishing the exit -- no hit.
	{
		RayIntersectionGeometric ri = MakeIntersection( p2, dir2 );
		pBox->IntersectRay( ri, true, true, false );
		assert( !ri.bHit );

		const bool hitOnly = pBox->IntersectRay_IntersectionOnly(
			Ray(p2, dir2), 1e6 * scale, true, true );
		assert( !hitOnly );
	}

	// INGOING: reversed direction, back into the box -- must hit the
	// OPPOSITE (+Z) face at depth / cos(angle to the Z axis), computed
	// from the ACTUAL normalized dir2 rather than hard-coded, so the
	// expectation doesn't depend on this file's own rounding of dir2.
	{
		const Vector3 backIn( -dir2.x, -dir2.y, -dir2.z );
		const double cosZ = std::fabs( dir2.z );
		const double expectedRange = ( 1.0 * scale ) / cosZ;
		const double tol = 1e-6 * expectedRange + 1e-9;

		RayIntersectionGeometric ri = MakeIntersection( p2, backIn );
		pBox->IntersectRay( ri, true, true, false );
		assert( ri.bHit );
		assert( ri.range >= 1e-6 );	// never the ~1e-12 self-hit root
		assert( IsClose( ri.range, expectedRange, tol ) );
		assert( IsVectorClose( ri.vNormal, Vector3(0.0, 0.0, 1.0), 1e-9 ) );

		const bool hitOnly = pBox->IntersectRay_IntersectionOnly(
			Ray(p2, backIn), expectedRange * 2.0, true, true );
		assert( hitOnly );
	}

	safe_release( pBox );
	std::cout << "  ...Passed! (scale " << scale << ")" << std::endl;
}

//////////////////////////////////////////////////////////////////////
// 4. Front/back-face flag rules at an on-face (published, outside)
// origin going IN (straight-in direction, the cos(theta)=1.0 case from
// the sweep above):
//   (front=true,  back=false) -> NO HIT.  An on-face origin going in is
//     treated as an EXIT hit (its own-face root was dropped), so
//     front-faces-only correctly rejects it.
//   (front=false, back=true)  -> the far (-Z) face IS hit.  This is the
//     strict-inside early-out the fix replaced: pre-fix, an on-face
//     origin read as "outside" (not strictly `RayBeginsInBox`) and
//     back-faces-only wrongly rejected the far face.
// And a genuinely OUTSIDE origin (the far camera origin) with
// (front=false, back=true) -> NO HIT: nothing to see without first
// crossing (and reporting) the front face.
//////////////////////////////////////////////////////////////////////
static void RunFrontBackFaceFlagRules( double scale )
{
	std::cout << "Testing BoxGeometry front/back-face flag rules (scale " << scale << ")..." << std::endl;

	const double W = 2.8 * scale, H = 2.8 * scale, D = 1.0 * scale;
	BoxGeometry* pBox = new BoxGeometry( W, H, D );

	const Point3 camOrigin( 2.1 * scale, -1.7 * scale, 4.0 * scale );
	const Point3 target( 0.0, 0.0, 0.5 * scale );
	const Vector3 camDir = Vector3Ops::Normalize(
		Vector3( target.x - camOrigin.x, target.y - camOrigin.y, target.z - camOrigin.z ) );
	Ray camRay( camOrigin, camDir );

	RayIntersectionGeometric riCam = MakeIntersection( camOrigin, camDir );
	pBox->IntersectRay( riCam, true, false, false );
	assert( riCam.bHit );
	const Point3 p = camRay.PointAtLength( riCam.range - 1e-12 );

	const Vector3 straightIn( 0.0, 0.0, -1.0 );

	// (front=true, back=false): on-face origin going in -> NO HIT.
	{
		const bool hitOnly = pBox->IntersectRay_IntersectionOnly(
			Ray(p, straightIn), 1e6 * scale, true, false );
		assert( !hitOnly );

		RayIntersectionGeometric ri = MakeIntersection( p, straightIn );
		pBox->IntersectRay( ri, true, false, false );
		assert( !ri.bHit );
	}

	// (front=false, back=true): the far (-Z) face IS hit.
	{
		const bool hitOnly = pBox->IntersectRay_IntersectionOnly(
			Ray(p, straightIn), 1e6 * scale, false, true );
		assert( hitOnly );

		RayIntersectionGeometric ri = MakeIntersection( p, straightIn );
		pBox->IntersectRay( ri, false, true, false );
		assert( ri.bHit );
		assert( IsVectorClose( ri.vNormal, Vector3(0.0, 0.0, -1.0), 1e-9 ) );
	}

	// Genuinely outside origin (the camera origin itself, far from any
	// face) with (front=false, back=true): NO HIT.
	{
		const bool hitOnly = pBox->IntersectRay_IntersectionOnly(
			camRay, 1e6 * scale, false, true );
		assert( !hitOnly );

		RayIntersectionGeometric ri = MakeIntersection( camOrigin, camDir );
		pBox->IntersectRay( ri, false, true, false );
		assert( !ri.bHit );
	}

	safe_release( pBox );
	std::cout << "  ...Passed! (scale " << scale << ")" << std::endl;
}

//////////////////////////////////////////////////////////////////////
// 6. Regression guard: an ORDINARY outside-to-outside ray (no self-hit
// involvement at all) with bComputeExitInfo=TRUE still reports range2 /
// vNormal2 exactly as before the fix -- the fix only changes behavior
// for an on-face origin, never for a clean two-face crossing.
//////////////////////////////////////////////////////////////////////
static void TestExitInfoRegression()
{
	std::cout << "Testing BoxGeometry bComputeExitInfo regression (ordinary outside origin)..." << std::endl;

	BoxGeometry* pBox = new BoxGeometry( 2.8, 2.8, 1.0 );
	const Point3 origin( 0.0, 0.0, 5.0 );
	const Vector3 dir( 0.0, 0.0, -1.0 );

	RayIntersectionGeometric ri = MakeIntersection( origin, dir );
	pBox->IntersectRay( ri, true, true, true );

	assert( ri.bHit );
	assert( IsClose( ri.range, 4.5, 1e-9 ) );
	assert( IsClose( ri.range2, 5.5, 1e-9 ) );
	assert( IsVectorClose( ri.vNormal, Vector3(0.0, 0.0, 1.0), 1e-9 ) );
	assert( IsVectorClose( ri.vNormal2, Vector3(0.0, 0.0, -1.0), 1e-9 ) );

	safe_release( pBox );
	std::cout << "  ...Passed!" << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Standoff re-entry contract (debt-25 review rounds 1-2, P2-1 / P1).
//
// `CSGObject::AdoptCsgExitFacePayloadViaProbe` stands a probe origin a
// small margin PAST an operand's exit face and fires it straight back,
// accepting only a hit within ~2.1 x margin.  That is, by construction,
// an origin close to one of the box's own faces -- exactly what
// `DropSelfHitRoot` exists to suppress -- so the two sides share a
// contract: the box's own-face band is, per axis and in box-local
// units,  eps = 4 * NEARZERO + 64 * DBL_EPSILON * |origin.axis|,  and
// the CSG probe stands off by at least 2 * eps / |cos(exit angle)|
// (mapped through the operand's transform).  This pins the BOX side of
// that contract: a re-entry ray from a standoff of 2 * eps / |cos|
// outside a face must hit THAT face at range ~ standoff, and one from
// 0.5 * eps must NOT (it is inside the band and reads as the origin's
// own face).  The CSG side is pinned end-to-end by
// tests/CsgSurfacePayloadTest.cpp (Tests 4/11/15 go red if its floor
// shrinks below the band, Test 14 if it grows with a transverse
// coordinate), which is why a reduced floor or a coordinate-coupled
// floor cannot land silently.  Also pins the promoted-root publish
// convention (`range2 == 0`, the RaySphereIntersection / CSG
// inside-sentinel) at an on-face ingoing origin.
//////////////////////////////////////////////////////////////////////
static void RunStandoffReentryContract( double scale )
{
	std::cout << "Testing standoff re-entry contract (scale " << scale << ")..." << std::endl;

	BoxGeometry* pBox = new BoxGeometry( 2.8 * scale, 2.8 * scale, 1.0 * scale );
	const Vector3 exitNormal( 0.0, 0.0, -1.0 );   // the -Z face is the exit for a -Z-going ray
	const double kUlpFactor = 64.0 * 2.2204460492503131e-16;

	const Vector3 dirs[2] = {
		Vector3( 0.0, 0.0, -1.0 ),
		Vector3Ops::Normalize( Vector3( 0.0, 0.8, -0.6 ) )   // |cos| = 0.6 against -Z; y travel across the 1.0 depth is 1.33 < 2.8, so the ray still exits through -Z
	};
	for( int k = 0; k < 2; k++ )
	{
		const Vector3& dir = dirs[k];
		// Start the ray so it passes through the box centre (the oblique
		// direction would otherwise miss the box entirely).
		const Point3 origin( 0.0, -dir.y * ( 3.0 * scale / std::fabs( dir.z ) ), 3.0 * scale );
		RayIntersectionGeometric riCam = MakeIntersection( origin, dir );
		pBox->IntersectRay( riCam, true, true, true );
		assert( riCam.bHit );
		assert( IsVectorClose( riCam.vNormal2, exitNormal ) );

		// Object::IntersectRay publishes the exit point FORWARD-biased
		// (range2 + SURFACE_INTERSEC_ERROR); the exit plane is z = -0.5*scale.
		const Point3 ptExit = Ray( origin, dir ).PointAtLength( riCam.range2 + 1e-12 );
		const double eps = 4.0 * NEARZERO + kUlpFactor * std::fabs( ptExit.z );
		const double cosExit = std::fabs( dir.z );
		const Vector3 probeDir( -dir.x, -dir.y, -dir.z );

		// (a) standoff = 2 * eps / |cos|: OUTSIDE the band -> that face.
		{
			const double standoff = 2.0 * eps / cosExit;
			const Point3 probeOrigin( ptExit.x + dir.x * standoff, ptExit.y + dir.y * standoff, ptExit.z + dir.z * standoff );
			RayIntersectionGeometric probe = MakeIntersection( probeOrigin, probeDir );
			pBox->IntersectRay( probe, true, true, false );
			assert( probe.bHit );
			assert( probe.range <= 2.1 * standoff );          // the SAME face, not the far one
			assert( IsVectorClose( probe.vNormal, exitNormal ) );
			const bool hitOnly = pBox->IntersectRay_IntersectionOnly( Ray( probeOrigin, probeDir ), 2.1 * standoff, true, true );
			assert( hitOnly );
		}
		// (b) standoff = 0.5 * eps / |cos|: INSIDE the band -> reads as the
		//     origin's own face, so the re-entry lands on the FAR face.
		{
			const double standoff = 0.5 * eps / cosExit;
			const Point3 probeOrigin( ptExit.x + dir.x * standoff, ptExit.y + dir.y * standoff, ptExit.z + dir.z * standoff );
			RayIntersectionGeometric probe = MakeIntersection( probeOrigin, probeDir );
			pBox->IntersectRay( probe, true, true, false );
			assert( probe.bHit );
			// The opposite (+Z) face: the box's full depth along this
			// direction, 1.0*scale / |cos| (plus the sub-1e-11 standoff).
			assert( IsClose( probe.range, 1.0 * scale / cosExit, 1e-6 * scale ) );
			assert( IsVectorClose( probe.vNormal, Vector3( 0.0, 0.0, 1.0 ) ) );
		}
	}

	// Promoted-root publish convention: an on-face ingoing origin (the
	// PT shadow-ray case) reports the far face as `range` and 0 as
	// `range2`, never the ~1e-12 self-root as a second "exit" behind it.
	{
		const Vector3 dir( 0.0, 0.0, -1.0 );
		const Point3 origin( 0.0, 0.0, 3.0 * scale );
		RayIntersectionGeometric riCam = MakeIntersection( origin, dir );
		pBox->IntersectRay( riCam, true, true, false );
		const Point3 p = Ray( origin, dir ).PointAtLength( riCam.range - 1e-12 );
		RayIntersectionGeometric ri = MakeIntersection( p, dir );
		pBox->IntersectRay( ri, true, true, true );
		assert( ri.bHit );
		assert( IsClose( ri.range, 1.0 * scale, 1e-6 * scale ) );
		assert( ri.range2 == 0.0 );
		assert( IsVectorClose( ri.vNormal, exitNormal ) );
	}

	// Sibling of the above (review round 3, P2-2): a STRICTLY interior
	// origin publishes the same sentinel -- range = the exit face ahead,
	// range2 = 0 -- not the negative entry root behind it (which Object::
	// IntersectRay turned into a positive "exit behind the entry" that
	// CSGObject's interval ordering misread as a phantom interior wall).
	{
		const Vector3 dir( 0.0, 0.0, -1.0 );
		const Point3 inside( 0.1 * scale, 0.05 * scale, 0.0 );
		RayIntersectionGeometric ri = MakeIntersection( inside, dir );
		pBox->IntersectRay( ri, true, true, true );
		assert( ri.bHit );
		assert( IsClose( ri.range, 0.5 * scale, 1e-6 * scale ) );
		assert( ri.range2 == 0.0 );
		assert( IsVectorClose( ri.vNormal, exitNormal ) );
		// vNormal2 keeps naming the entry face BEHIND the origin (the box's
		// interior convention, unchanged); range2 = 0 is the "nothing
		// ahead" signal, so pin the pair rather than leave it implicit.
		assert( IsVectorClose( ri.vNormal2, Vector3( 0.0, 0.0, 1.0 ) ) );
	}

	safe_release( pBox );
	std::cout << "  ...Passed!" << std::endl;
}

//////////////////////////////////////////////////////////////////////
// SelfHitRootFloor contract, per geometry (adversarial review of
// a8bef210).
//
// a8bef210 made every primitive's self-hit gate SCALE-RELATIVE, which
// means a caller that deliberately stands a ray off a surface in order to
// re-hit it can no longer hard-code a constant: the required standoff now
// depends on the geometry, its size, and where on it the point sits.  A
// sphere of radius 4 rejects roots below ~5e-12; a 1000-unit one below
// ~2e-9.  `CSGObject::AdoptCsgExitFacePayloadViaProbe` is that caller, and
// it was standing off by a BOX-derived band -- correct for a box operand,
// short for every other primitive past a few units, at which point the
// probe missed and the composite silently reported the operand's ENTRY
// face payload on its EXIT face (the antipodal point of a carving sphere;
// see CsgSurfacePayloadTest Test 27 for the end-to-end version).
//
// The fix is `IGeometry::SelfHitRootFloor( localOrigin, localDir,
// localNormal )` -- each geometry reports its own gate -- and THIS test
// pins each override against the intersection routine it claims to
// describe, which is the only thing that keeps the two from drifting
// apart again:
//
//   (a) a re-entry ray standing off by 2 x floor MUST hit that same face,
//       at a range within ~2.1 x the standoff (not the far side);
//   (b) one standing off by 0.5 x floor MUST NOT report that face -- it
//       is inside the gate, so the routine drops the root and the ray
//       either reports the far surface or misses entirely.
//
// (b) is the half that matters: it proves the reported floor is not
// wildly over-stated (an over-stated floor would inflate CSG's same-face
// acceptance radius and re-open decoy-payload adoption, the trade
// CsgSurfacePayloadTest Test 15 guards).  Together they bracket the true
// gate within a factor of 4.
//
// Each case is run at unit scale AND at 1000x, because a floor that is
// accidentally absolute rather than scale-relative passes at one and
// fails at the other.  BoxGeometry is included even though its own
// standoff contract is pinned above, so that its `SelfHitRootFloor`
// (which must additionally divide the per-axis band by the incidence
// cosine) is checked against the same routine.
//////////////////////////////////////////////////////////////////////

// MEASURE the routine's real gate and compare the geometry's CLAIM to it.
//
// A fixed 2x-accept / 0.5x-reject bracket is a factor-of-four window, which
// is too loose to catch the mistakes that actually happen here: dropping
// the sphere's `+ radius` term, for instance, under-states its floor by at
// most 2x (a point on a sphere of radius R has |o|_1 in [R, sqrt(3) R], so
// the true and truncated floors are always within a factor of two) and
// would sail through such a bracket.  So instead of asserting AROUND the
// claim, BISECT for the smallest standoff the intersection routine actually
// accepts, and compare the claim to that measurement:
//
//   floor >= sMin   -- never UNDER-states.  This is the property CSGObject's
//                      exit-face probe depends on; violating it is the P1.
//   floor <= 8 sMin -- never wildly OVER-states.  An inflated floor inflates
//                      the probe's same-face acceptance radius and re-opens
//                      decoy-payload adoption (CsgSurfacePayloadTest Test 15's
//                      trade).  8x leaves room for the deliberate slack in a
//                      conservative bound (the mesh / patch classes bound
//                      every primitive at once via the bounding box) without
//                      admitting an order of magnitude.
//
// `Accept(s)` is the probe the production caller performs: stand off `s`
// past the face along the original ray direction, fire back, and require a
// hit at range <= 2.1 s -- i.e. THIS face, not the far side (a rejected root
// makes the ray report the far surface, or nothing at all on an open one).
static bool AcceptsStandoff(
	const IGeometry* pGeom,
	const Point3& ptFace,
	const Vector3& exitDir,
	double standoff )
{
	const Vector3 probeDir( -exitDir.x, -exitDir.y, -exitDir.z );
	const Point3 o( ptFace.x + exitDir.x * standoff,
	                ptFace.y + exitDir.y * standoff,
	                ptFace.z + exitDir.z * standoff );
	RayIntersectionGeometric ri = MakeIntersection( o, probeDir );
	pGeom->IntersectRay( ri, true, true, false );
	return ri.bHit && ri.range <= 2.1 * standoff;
}

static void CheckFloorBrackets(
	const IGeometry* pGeom,
	const char* what,
	const Point3& ptFace,
	const Vector3& exitDir,       // the direction the original ray was travelling
	const Vector3& faceNormal,    // outward unit normal of the face being re-hit
	double scale )
{
	const Vector3 probeDir( -exitDir.x, -exitDir.y, -exitDir.z );
	const double floorAt = pGeom->SelfHitRootFloor( ptFace, probeDir, faceNormal );
	assert( floorAt > 0.0 );

	// Bracket the bisection: `lo` must be rejected, `hi` accepted.  Both are
	// generous (1e-18 is far below every gate in play; 1e-5 x scale is far
	// above them all, and still far below any feature these test geometries
	// have) so the bisection never runs on an unbracketed interval.
	double lo = 1e-18 * ( scale > 1.0 ? scale : 1.0 );
	double hi = 1e-5  * ( scale > 1.0 ? scale : 1.0 );
	if( AcceptsStandoff( pGeom, ptFace, exitDir, lo ) ) {
		std::cout << "    FAILED bracket: lo accepted for " << what << " scale " << scale << std::endl;
	}
	assert( !AcceptsStandoff( pGeom, ptFace, exitDir, lo ) );
	if( !AcceptsStandoff( pGeom, ptFace, exitDir, hi ) ) {
		std::cout << "    FAILED bracket: hi rejected for " << what << " scale " << scale << std::endl;
	}
	assert( AcceptsStandoff( pGeom, ptFace, exitDir, hi ) );

	// 200 halvings of the log interval is far more than enough to pin the
	// crossing to well inside the 8x window asserted below.
	for( int i = 0; i < 200; i++ ) {
		const double mid = std::sqrt( lo * hi );
		if( AcceptsStandoff( pGeom, ptFace, exitDir, mid ) ) {
			hi = mid;
		} else {
			lo = mid;
		}
		if( hi <= lo * 1.000001 ) {
			break;
		}
	}
	const double sMin = hi;   // smallest standoff the routine actually accepts

	// 0.999, not 1.0: the bisection measures the smallest standoff whose
	// RECOMPUTED root survives the gate, and that root is recomputed from
	// coordinates carrying their own ulp of round-off, so the measurement
	// lands a few parts in 1e5 ABOVE the analytic gate (e.g. 9.00036e-12
	// measured against the sphere's exact 9e-12 at R = 4).  A 0.1 % slack
	// absorbs that and nothing else -- every mistake this assertion exists
	// to catch is a factor of two or more.
	if( !( floorAt >= sMin * 0.999 ) ) {
		std::cout << "    FAILED: claimed floor UNDER-states the routine's gate: " << what
		          << " scale " << scale << " claim " << floorAt << " measured " << sMin << std::endl;
	}
	assert( floorAt >= sMin * 0.999 );

	if( !( floorAt <= 8.0 * sMin ) ) {
		std::cout << "    FAILED: claimed floor OVER-states the routine's gate: " << what
		          << " scale " << scale << " claim " << floorAt << " measured " << sMin << std::endl;
	}
	assert( floorAt <= 8.0 * sMin );

	// And the contract as the caller uses it: standing off by the claim (with
	// the production probe's own 2x headroom) re-hits the SAME face.
	if( !AcceptsStandoff( pGeom, ptFace, exitDir, 2.0 * floorAt ) ) {
		std::cout << "    FAILED: 2x the claimed floor does not re-hit the face: " << what
		          << " scale " << scale << std::endl;
	}
	assert( AcceptsStandoff( pGeom, ptFace, exitDir, 2.0 * floorAt ) );
}

static void RunSelfHitRootFloorContract( double scale )
{
	std::cout << "Testing SelfHitRootFloor per-geometry contract (scale " << scale << ")..." << std::endl;

	// Ray direction used throughout: straight down -Z, so the exit face
	// of each closed primitive is its -Z pole / face, outward normal
	// (0,0,-1).  The exact surface point is read from the geometry's own
	// `range2` rather than assumed, so nothing here depends on a
	// hand-computed coordinate.
	const Vector3 dir( 0.0, 0.0, -1.0 );
	const Vector3 nOut( 0.0, 0.0, -1.0 );
	const Point3  camOrigin( 0.0, 0.0, 3.0 * scale );

	// --- SphereGeometry -> RaySphereIntersection ---
	// Radius deliberately > ~3.5 at unit scale: that is exactly where the
	// sphere's floor (NEARZERO*(1+|o|_1+R)) overtakes the box band the CSG
	// probe used to assume, i.e. the P1's own crossover.
	{
		SphereGeometry* g = new SphereGeometry( 4.0 * scale );
		RayIntersectionGeometric riCam = MakeIntersection( camOrigin, dir );
		// The camera origin must be outside a 4-unit sphere: push it out.
		const Point3 farOrigin( 0.0, 0.0, 12.0 * scale );
		riCam = MakeIntersection( farOrigin, dir );
		g->IntersectRay( riCam, true, true, true );
		assert( riCam.bHit );
		const Point3 ptExit = Ray( farOrigin, dir ).PointAtLength( riCam.range2 );
		CheckFloorBrackets( g, "sphere R=4", ptExit, dir, nOut, scale );
		safe_release( g );
	}

	// --- CylinderGeometry (capped solid) -> IntersectCappedSolid ---
	// Axis along Z so the -Z CAP is the exit face; radius 4 puts it past
	// the same crossover.
	{
		CylinderGeometry* g = new CylinderGeometry( 'z', 4.0 * scale, 6.0 * scale, true );
		const Point3 farOrigin( 0.5 * scale, 0.0, 12.0 * scale );
		RayIntersectionGeometric riCam = MakeIntersection( farOrigin, dir );
		g->IntersectRay( riCam, true, true, true );
		assert( riCam.bHit );
		const Point3 ptExit = Ray( farOrigin, dir ).PointAtLength( riCam.range2 );
		CheckFloorBrackets( g, "capped cylinder R=4", ptExit, dir, nOut, scale );
		safe_release( g );
	}

	// --- BoxGeometry -> DropSelfHitRoot's per-axis band / |cos| ---
	// Both normal AND oblique incidence, since the box is the one
	// geometry whose floor is angle-dependent.
	{
		BoxGeometry* g = new BoxGeometry( 8.0 * scale, 8.0 * scale, 8.0 * scale );
		const Vector3 dirs[2] = {
			Vector3( 0.0, 0.0, -1.0 ),
			Vector3Ops::Normalize( Vector3( 0.0, 0.8, -0.6 ) )
		};
		for( int k = 0; k < 2; k++ ) {
			const Vector3& d = dirs[k];
			const Point3 o( 0.0, -d.y * ( 12.0 * scale / std::fabs( d.z ) ), 12.0 * scale );
			RayIntersectionGeometric riCam = MakeIntersection( o, d );
			g->IntersectRay( riCam, true, true, true );
			assert( riCam.bHit );
			const Point3 ptExit = Ray( o, d ).PointAtLength( riCam.range2 );
			CheckFloorBrackets( g, k == 0 ? "box, normal incidence" : "box, oblique incidence",
				ptExit, d, nOut, scale );
		}
		safe_release( g );
	}

	// --- InfinitePlaneGeometry -> RayPlaneIntersection ---
	// The plane and the disk take IGeometry's DEFAULT floor, so these two
	// rows are what pins that default against RayPlaneIntersection.  The
	// probe point sits far out IN the plane (x, y ~ 8*scale), where the
	// routine's |o|_1 coordinate scale is eight times the perpendicular
	// one -- so a "tightening" of either side to the perpendicular
	// component alone (the shape an adversarial review of a8bef210
	// proposed, and which measurement rejected: see the note in
	// RayPlaneIntersection.cpp) shows up here as an over-statement rather
	// than passing silently.
	{
		InfinitePlaneGeometry* g = new InfinitePlaneGeometry( 1.0, 1.0 );
		const Point3 ptFace( 8.0 * scale, 8.0 * scale, 0.0 );
		CheckFloorBrackets( g, "infinite plane, offset in-plane", ptFace, dir, nOut, scale );
		safe_release( g );
	}

	// --- CircularDiskGeometry -> RayPlaneIntersection ---
	{
		CircularDiskGeometry* g = new CircularDiskGeometry( 16.0 * scale, 'z' );
		const Point3 ptFace( 8.0 * scale, 8.0 * scale, 0.0 );
		CheckFloorBrackets( g, "circular disk, offset in-plane", ptFace, dir, nOut, scale );
		safe_release( g );
	}

	// --- TriangleMeshGeometryIndexed -> RayTriangleIntersection ---
	// One big triangle in the z = 0 plane whose FAR corner carries a much
	// larger coordinate than the first-stored vertex -- the P2-2 shape:
	// charging vPt1's L1 alone would under-state the floor here, and (a)
	// would then pass at a standoff the routine actually rejects.
	{
		TriangleMeshGeometryIndexed* g = new TriangleMeshGeometryIndexed(
			true,    // double sided
			true );  // use face normals
		g->BeginIndexedTriangles();
		g->AddVertex( Point3( -1.0 * scale, -1.0 * scale, 0.0 ) );
		g->AddVertex( Point3( 40.0 * scale, -1.0 * scale, 0.0 ) );
		g->AddVertex( Point3( -1.0 * scale, 40.0 * scale, 0.0 ) );
		g->AddTexCoord( Point2( 0, 0 ) );
		g->AddTexCoord( Point2( 1, 0 ) );
		g->AddTexCoord( Point2( 0, 1 ) );
		IndexedTriangle t;
		t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2;
		t.iCoords[0]   = 0; t.iCoords[1]   = 1; t.iCoords[2]   = 2;
		g->AddIndexedTriangle( t );
		g->DoneIndexedTriangles();
		const Point3 ptFace( 1.0 * scale, 1.0 * scale, 0.0 );
		CheckFloorBrackets( g, "indexed triangle, far corner dominates", ptFace, dir, nOut, scale );
		safe_release( g );
	}

	std::cout << "  ...Passed!" << std::endl;
}

int main()
{
	RunStandoffReentryContract( 1.0 );
	RunStandoffReentryContract( 1000.0 );

	RunSelfHitRootFloorContract( 1.0 );
	RunSelfHitRootFloorContract( 1000.0 );

	RunFrontFaceIngoingSweep( 1.0 );
	RunBackFaceIngoingOutgoing( 1.0 );
	RunFrontBackFaceFlagRules( 1.0 );

	// Coordinate-scale check (~1000): the self-hit band is
	// 4 * NEARZERO + 64 * DBL_EPSILON * |origin.axis|, so re-running the same
	// suites at 1000x scale confirms it tracks genuine faces rather than
	// firing on a fixed absolute threshold that would be swamped at this
	// scale, or failing to fire on a scale that would swamp it.
	RunFrontFaceIngoingSweep( 1000.0 );
	RunBackFaceIngoingOutgoing( 1000.0 );
	RunFrontBackFaceFlagRules( 1000.0 );

	TestExitInfoRegression();

	std::cout << "All BoxGeometryTest cases passed!" << std::endl;
	return 0;
}
