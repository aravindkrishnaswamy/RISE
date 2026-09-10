//////////////////////////////////////////////////////////////////////
//
//  ProximitySignalTest.cpp - Phase 1 of the CROSS-OBJECT proximity
//  signal (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md), driven by the
//  fixture scene scenes/Tests/Signals/proximity_closed_forms.RISEscene
//  (that document's scene C).
//
//  WHAT THIS SUITE IS FOR.  `proximity(r)` answers "how close is the
//  nearest OTHER surface" -- the quantity contact grime actually is.  Its
//  whole safety argument is ONE-SIDED: every family's answer is the true
//  distance or an UPPER bound on it, never a lower one, because
//  over-reading contact paints grime that is not there (a wrong render)
//  while under-reading it merely leaves a seam unpainted (a feature
//  failure).  So almost every check below is a CLOSED FORM computed by
//  hand from the fixture's own geometry, and the two families that
//  cannot be exact are checked against their STATED bound rather than
//  against a number somebody recorded from a run.
//
//  IT DRIVES THE REAL SCENE FILE, through the real CST loader, rather
//  than building fixtures inline.  Two reasons: the scene file is a
//  deliverable of its own (an author reads it to learn what the signal
//  does), and a fixture that only exists in C++ cannot go stale against
//  a scene that nobody loads.  Where a fixture CANNOT be authored --
//  a degenerate `scale 0` transform, a null signal channel -- the test
//  builds it through the API and says so.
//
//  THE SECTIONS:
//    (a) THE EXACT FAMILIES.  Plane, sphere, box, capped and open
//        cylinder, disk, coplanar clipped plane, torus -- each against
//        its closed form at 1e-9.
//    (b) THE BOUNDED FAMILIES.  The ellipsoid against its semi-axis
//        ratio; the exact single-sphere SDF at 1e-9; the composed
//        1-Lipschitz SDF between its Map lower bound and a GRID SEARCH
//        of the same {Map <= 0} set, with gap_max REPORTED as a number.
//    (c) THE EXCLUSION RULES.  Self, emitters, CSG operands and
//        composites, refusing families, and the ones that must NOT be
//        excluded (`casts_shadows FALSE`, instanced copies).
//    (d) THE TRANSFORM.  Uniform scale exactly; non-uniform scale as an
//        upper bound, and the radius conversion that cannot miss a
//        neighbour; a degenerate transform refuses.
//    (e) THE SIGNAL'S OWN CONVENTIONS.  Unsigned (below reads as above),
//        interpenetration reads 1, the radius cut-off, every neutral.
//    (f) THE BUILTIN, end to end through an ExpressionPainter at a real
//        hit -- including the DynR remap guard, whose absence would
//        silently compile a computed-radius proximity() as a convexity
//        call, and the parse-time diagnostic's unit.
//    (h) THE SIGNED LOWER BOUND (Phase 3).  The other direction from
//        (a)-(d): an exact SIGN with a LOWER-bound magnitude, per family,
//        plus the exactness flag a CSG composite's boundary arm consumes.
//        Every sheet refuses it while still answering the unsigned one.
//    (j) CSG COMPOSITES (Phase 3).  A union's `min`; an intersection's
//        and a subtraction's bracket over the composed signed field, with
//        the two landing arms, the phantom sweep that a tolerance would
//        have admitted, the composite's own transform layer, and the
//        nesting rules.
//    (k) interior(r) (Phase 3).  The SIGNED cross-object builtin -- the
//        depth mapping, the running maximum over overlapping solids, the
//        union composite's documented under-read, the sheet families'
//        silent zero, and the builtin's own parse diagnostic.
//    (i) EXACT SIGMA (Phase 3).  The three branches of the transform's
//        singular-value pair -- the similarity fast path, the one-sided
//        Jacobi SVD, and the loose Frobenius/determinant fallback -- each
//        against a reference written into the test rather than recorded
//        from a run.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <string>
#include <vector>
#include <filesystem>

#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Interfaces/SurfaceSignalProximity.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/ExpressionMemo.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace fs = std::filesystem;

static int passCount = 0;
static int failCount = 0;

static void Check( const bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( const Scalar got, const Scalar want, const Scalar tol, const std::string& name )
{
	if( std::fabs( (double)( got - want ) ) <= (double)tol ) { ++passCount; }
	else {
		++failCount;
		std::cout << "  FAIL: " << name << "  got " << (double)got
			<< " want " << (double)want << " (tol " << (double)tol << ")" << std::endl;
	}
}

//======================================================================
// Scene loading
//======================================================================

//! Locate the repo root regardless of the binary's working directory
//! (run_all_tests.sh runs from the repo root; an ad-hoc run may not).
static fs::path FindRepoRoot()
{
	const char* candidates[] = { ".", "..", "../..", "../../.." };
	for( const char* c : candidates ) {
		const fs::path p( c );
		if( fs::exists( p / "scenes" / "Tests" / "Signals" / "proximity_closed_forms.RISEscene" ) ) {
			return p;
		}
	}
	return fs::path();
}

//! The loaded fixture scene, plus the handles every check needs.  Held in
//! one struct because a probe needs THREE things at once -- the manager to
//! ask, the receiver object to exclude as `self`, and the neighbour under
//! test -- and passing them separately invites getting `self` wrong, which
//! would silently turn a self-exclusion check into a no-op.
struct Fixture
{
	Job*			job;
	IObjectManager*	mgr;
	IObjectPriv*	floorObj;

	Fixture() : job( 0 ), mgr( 0 ), floorObj( 0 ) {}

	IObjectPriv* Obj( const char* name ) const
	{
		return mgr ? mgr->GetItem( name ) : 0;
	}
};

static bool LoadFixture( Fixture& out )
{
	const fs::path root = FindRepoRoot();
	if( root.empty() ) return false;

	const fs::path scenePath = root / "scenes" / "Tests" / "Signals" / "proximity_closed_forms.RISEscene";
	std::ifstream in( scenePath );
	if( !in ) return false;
	std::stringstream ss;
	ss << in.rdbuf();

	Cst::Document doc = Cst::ParseToCst( ss.str() );
	out.job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *out.job, &diags );

	// A derive diagnostic here is not cosmetic: a chunk that failed to
	// apply is a fixture that is not in the scene, and every check that
	// names it would then pass or fail for the wrong reason.
	for( std::size_t i = 0; i < diags.size(); ++i ) {
		std::cout << "  scene diagnostic: " << diags[i] << std::endl;
	}
	Check( diags.empty(), "scene C derives with NO diagnostics (every fixture is present)" );

	out.mgr = out.job->GetObjects();
	if( !out.mgr ) return false;
	// The AABB snapshot the query scans is built here, exactly as a render
	// would build it.
	out.mgr->PrepareForRendering();

	out.floorObj = out.Obj( "floor" );
	return out.floorObj != 0;
}

//! One probe: distance from a world point to the nearest surface other
//! than `self`'s, within `r`.  Returns the answer or a negative sentinel
//! when the query refused, so a check can distinguish "far" from "close".
static Scalar Probe( const Fixture& f, const Point3& p, const IObjectPriv* self, const Scalar r )
{
	Scalar d = Scalar( 0 );
	if( f.mgr->NearestOtherSurface( p, self, r, d ) ) return d;
	return Scalar( -1 );
}

//! The signal itself -- what an expression would read -- built from the
//! channel by hand so a check can drive it without a hit record.
static Scalar ProximityAt( const Fixture& f, const Point3& p, const IObjectPriv* self, const Scalar r )
{
	SurfaceSignalInfo s;
	s.pScene  = f.mgr;
	s.pSelf   = self;
	s.ptWorld = p;
	// Every call gets a cold memo: these probes deliberately repeat one
	// point with different radii and different `self`, and a warm table
	// would make a dropped key field look like a pass.
	ExpressionMemo::Invalidate();
	return s.Proximity( r );
}

//======================================================================
// (a) THE EXACT FAMILIES
//======================================================================

static void TestExactFamilies( const Fixture& f )
{
	std::cout << "(a) the exact families, against closed forms at 1e-9" << std::endl;

	const Scalar tol = Scalar( 1e-9 );

	// --- SPHERE.  Centre (0,3,0), R = 1.  From a floor point at lateral
	// offset s the exact distance is sqrt(s^2 + 9) - 1.  This IS the
	// design's contact model (a body of cross-section radius rho resting
	// on a plane reads sqrt(s^2 + rho^2) - rho at offset s), so getting it
	// right here is what makes the gate numbers in the design meaningful.
	{
		const Scalar d0 = Probe( f, Point3( 0, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d0, Scalar( 2 ), tol, "(a) sphere: directly beneath, 3 - 1 = 2" );

		const Scalar s = Scalar( 1.7 );
		const Scalar want = std::sqrt( s*s + Scalar( 9 ) ) - Scalar( 1 );
		const Scalar d1 = Probe( f, Point3( 0, 0, s ), f.floorObj, Scalar( 5 ) );
		CheckClose( d1, want, tol, "(a) sphere: at lateral offset 1.7, sqrt(s^2+9) - 1" );
	}

	// --- INFINITE PLANE.  The floor is a neighbour too, for anything that
	// is not the floor.  From the sphere's own centre the distance to the
	// plane is exactly the height.
	{
		const Scalar d = Probe( f, Point3( 0, 3, 0 ), f.Obj( "n_sphere" ), Scalar( 5 ) );
		CheckClose( d, Scalar( 3 ), tol, "(a) infinite plane: |y| from a point 3 above it" );
	}

	// --- BOX.  2 x 2 x 2 at (6,3,0): underside at y = 2.
	{
		const Scalar d = Probe( f, Point3( 6, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d, Scalar( 2 ), tol, "(a) box: directly beneath the face, 3 - 1 = 2" );

		// Past a corner in x AND z, the nearest point is the corner edge,
		// so the answer is the 3D hypotenuse: the point (6+2, 0, 2) is
		// (1, 2, 1) away from the nearest box corner (7, 2, 1).
		const Scalar want = std::sqrt( Scalar( 1 ) + Scalar( 4 ) + Scalar( 1 ) );
		const Scalar d2 = Probe( f, Point3( 8, 0, 2 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d2, want, tol, "(a) box: past a corner, the 3D hypotenuse to the corner" );
	}

	// --- CAPPED CYLINDER.  R = 1, height 4, axis Y, centre (12,4,0): the
	// bottom CAP DISK is at y = 2, so a point under the axis is 2 away.
	{
		const Scalar d = Probe( f, Point3( 12, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d, Scalar( 2 ), tol, "(a) capped cylinder: the cap disk is the nearest surface, 2" );
	}

	// --- OPEN CYLINDER.  THE DISCRIMINATING CHECK of the pair: the same
	// tube with NO caps is a different SURFACE, and a point under the axis
	// is nearest the bottom RIM, at sqrt(1^2 + 2^2) = sqrt(5) = 2.236.
	// Reusing the capped form here would report 2 -- an UNDER-report of
	// 0.236, which is the forbidden direction.
	{
		const Scalar want = std::sqrt( Scalar( 5 ) );
		const Scalar d = Probe( f, Point3( 18, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d, want, tol, "(a) MONEY -- open cylinder: the RIM, sqrt(5), not the absent cap's 2" );
		Check( d > Scalar( 2 ) + Scalar( 0.2 ),
			"(a) ...and it is clearly further than the capped twin's 2, so the two forms really differ" );
	}

	// --- CIRCULAR DISK.  R = 1.5, axis Y, at (24,2,0): a flat sheet.
	{
		const Scalar d = Probe( f, Point3( 24, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d, Scalar( 2 ), tol, "(a) disk: under its centre, the axial drop, 2" );

		// 1 unit past the rim: hypotenuse of the 2 drop and the 1 overshoot.
		const Scalar want = std::sqrt( Scalar( 4 ) + Scalar( 1 ) );
		const Scalar d2 = Probe( f, Point3( 24 + 2.5, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d2, want, tol, "(a) disk: past the rim, sqrt(drop^2 + overshoot^2)" );
	}

	// --- COPLANAR CLIPPED PLANE.  A 2 x 2 square lying flat at y = 2.5.
	{
		const Scalar d = Probe( f, Point3( 30, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d, Scalar( 2.5 ), tol, "(a) coplanar quad: under its middle, the perpendicular, 2.5" );

		// Outside the quad in x: the nearest point is the edge at x = 31,
		// so the answer is the hypotenuse of 2.5 and the 1.0 overshoot.
		const Scalar want = std::sqrt( Scalar( 6.25 ) + Scalar( 1 ) );
		const Scalar d2 = Probe( f, Point3( 32, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d2, want, tol, "(a) coplanar quad: past its edge, the edge-segment distance" );
	}

	// --- TORUS.  Major 1.5, minor 0.5, ring in XZ and tube along Y, at
	// (42,3,0).  Under the RING CENTRE LINE (lateral offset 1.5) the
	// nearest tube point is directly above, at 3 - 0.5 = 2.5.
	{
		const Scalar d = Probe( f, Point3( 42, 0, 1.5 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d, Scalar( 2.5 ), tol, "(a) torus: under the ring centre line, 3 - 0.5 = 2.5" );

		// Under the torus's own centre, the nearest point is on the inner
		// ring: horizontal offset 1.5, vertical 3, minus the tube radius.
		const Scalar want = std::sqrt( Scalar( 1.5*1.5 ) + Scalar( 9 ) ) - Scalar( 0.5 );
		const Scalar d2 = Probe( f, Point3( 42, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d2, want, tol, "(a) torus: under the hole, sqrt(1.5^2 + 3^2) - 0.5" );
	}
}

//======================================================================
// (b) THE BOUNDED FAMILIES
//======================================================================

//! Brute-force reference for an SDF's true distance: the closest point of
//! the {Map <= 0} SET, found by scanning a grid of the geometry's own
//! object space.
//!
//! THE REFERENCE IS THE ZERO SET, NOT THE TRACER'S BAND, and saying so is
//! the point of this helper.  `SDFGeometry`'s sphere tracer accepts a hit
//! anywhere within |Map| <= 2*eps, which is a band AROUND the surface;
//! the surface itself is the zero set, and that is what a distance is
//! measured to.  Scanning `EvaluateParts` -- the same fold `Map` runs --
//! for points with a NON-POSITIVE field gives exactly the solid, so the
//! nearest such point over-estimates the true distance by at most the
//! grid's own resolution.  Both this and the reported answer are
//! therefore >= the truth, which is why the assertion below is a gap and
//! not an equality.
static Scalar GridSearchDistanceToSolid(
	const std::vector<SDFGeometry::Part>& parts,
	const Point3& pObject, const Scalar lo, const Scalar hi, const int steps )
{
	Scalar best = RISE_INFINITY;
	const Scalar span = ( hi - lo ) / Scalar( steps );
	for( int i = 0; i <= steps; ++i ) {
		const Scalar x = lo + span * Scalar( i );
		for( int j = 0; j <= steps; ++j ) {
			const Scalar y = lo + span * Scalar( j );
			for( int k = 0; k <= steps; ++k ) {
				const Scalar z = lo + span * Scalar( k );
				const Point3 q( x, y, z );
				if( SDFGeometry::EvaluateParts( parts, q ) <= Scalar( 0 ) ) {
					const Scalar d = Vector3Ops::Magnitude( Vector3Ops::mkVector3( q, pObject ) );
					if( d < best ) best = d;
				}
			}
		}
	}
	return best;
}

static void TestBoundedFamilies( const Fixture& f )
{
	std::cout << "(b) the bounded families -- ellipsoid, exact SDF, composed SDF" << std::endl;

	// --- ELLIPSOID, semi-axes (2,1,1) at (48,3,0).  Under its centre the
	// TRUE distance is 3 - 1 = 2, and the reported one is the unit-sphere
	// answer scaled by the LARGEST semi-axis.  Two things must hold, and
	// the second is the one that matters: the answer is an UPPER bound
	// (never below the truth), and it is no worse than the semi-axis
	// ratio.
	{
		const Scalar d = Probe( f, Point3( 48, 0, 0 ), f.floorObj, Scalar( 8 ) );
		Check( d > Scalar( 0 ), "(b) ellipsoid answers" );
		Check( d >= Scalar( 2 ) - Scalar( 1e-9 ),
			"(b) MONEY -- the ellipsoid bound never reads BELOW the true distance (2)" );
		// unit-sphere distance from (0,-3,0)/(2,1,1) = (0,-3,0): |q| = 3,
		// so d_unit = 2, scaled by sigmaMax = 2 -> 4.  The ratio bound is
		// max/min = 2.
		Check( d <= Scalar( 2 ) * Scalar( 2 ) + Scalar( 1e-9 ),
			"(b) ...and over-reports by at most the semi-axis ratio (2x)" );
		std::cout << "    ellipsoid: true 2, reported " << (double)d
			<< " (bound 4 = 2 x the semi-axis ratio)" << std::endl;
	}

	// --- SDF, EXACT FIELD.  One `sphere union` part at unit part scale is
	// a genuine Euclidean distance field outside the solid, so the
	// bracketed procedure's bounds coincide and the closed form holds
	// tightly.  The tolerance is the tracer's own surface epsilon rather
	// than 1e-9: the probe's LAST doubling step is what bounds the
	// over-report, and it starts at max(eps, Map).
	{
		const Scalar d = Probe( f, Point3( 54, 0, 0 ), f.floorObj, Scalar( 5 ) );
		Check( d > Scalar( 0 ), "(b) exact-field SDF answers" );
		Check( d >= Scalar( 2 ) - Scalar( 1e-9 ),
			"(b) exact-field SDF never reads below the true distance (2)" );
		CheckClose( d, Scalar( 2 ), Scalar( 1e-3 ),
			"(b) MONEY -- an exact SDF field lands on the closed form, 3 - 1 = 2" );
		std::cout << "    exact SDF: true 2, reported " << (double)d
			<< " (over-report " << (double)( d - Scalar( 2 ) ) << ")" << std::endl;
	}

	// --- SDF, COMPOSED (1-LIPSCHITZ) FIELD.  smin + subtract, so `Map`
	// may report LESS than the true distance and the field value itself is
	// unusable as an answer.  Two assertions, and gap_max REPORTED:
	//   lower <= reported     (the Map value is a true lower bound)
	//   reported <= reference + gap_max
	{
		IObjectPriv* obj = f.Obj( "n_sdf_composed" );
		Check( obj != 0, "(b) the composed-SDF fixture exists" );
		const SDFGeometry* sdf = obj
			? dynamic_cast<const SDFGeometry*>( obj->GetGeometry() ) : 0;
		Check( sdf != 0, "(b) ...and it is an SDFGeometry" );

		if( sdf ) {
			// The object sits at (60,3,0) with no rotation or scale, so
			// world -> object is a pure translation and the probe point
			// (60, 0, 0) is (0, -3, 0) in the geometry's own space.
			const Point3 pObj( 0, -3, 0 );

			const Scalar reported = Probe( f, Point3( 60, 0, 0 ), f.floorObj, Scalar( 6 ) );
			Check( reported > Scalar( 0 ), "(b) composed-SDF answers" );

			// The LOWER bound: `Map` under-reads, so it is <= the truth,
			// and the reported answer must be at least the truth.
			const Scalar lower = SDFGeometry::EvaluateParts( sdf->GetParts(), pObj );

			// The REFERENCE: a grid search of {Map <= 0}.  The parts span
			// roughly [-1.6, 1.6] in x and [-1, 1.4] in y, so a cube of
			// [-2, 2] at 200 steps (spacing 0.02, diagonal 0.035) covers
			// the solid with room to spare.
			const Scalar kLo = Scalar( -2 ), kHi = Scalar( 2 );
			const int    kSteps = 200;

			// THE CUBE MUST CONTAIN THE SOLID, and that is asserted rather
			// than assumed.  The grid bounds are hard-coded to this
			// fixture's parts (two unit spheres at x = +/-0.6, a 0.5
			// subtractor at y = 0.9); an edit to those that pushed the
			// solid past [-2, 2] would silently make the reference wrong --
			// too large, or missing the nearest region entirely -- and the
			// gap assertion below would then pass or fail for a reason that
			// has nothing to do with the bracket.  A solid strictly inside
			// the cube has NO inside point on the cube's boundary, which is
			// exactly what this scans for.
			bool solidTouchesBoundary = false;
			{
				const Scalar span = ( kHi - kLo ) / Scalar( kSteps );
				for( int i = 0; i <= kSteps && !solidTouchesBoundary; ++i ) {
					for( int j = 0; j <= kSteps && !solidTouchesBoundary; ++j ) {
						const Scalar u = kLo + span * Scalar( i );
						const Scalar v = kLo + span * Scalar( j );
						const Point3 faces[6] = {
							Point3( kLo, u, v ), Point3( kHi, u, v ),
							Point3( u, kLo, v ), Point3( u, kHi, v ),
							Point3( u, v, kLo ), Point3( u, v, kHi ) };
						for( int fI = 0; fI < 6; ++fI ) {
							if( SDFGeometry::EvaluateParts( sdf->GetParts(), faces[fI] ) <= Scalar( 0 ) ) {
								solidTouchesBoundary = true;
								break;
							}
						}
					}
				}
			}
			Check( !solidTouchesBoundary,
				"(b) the grid cube [-2,2]^3 strictly CONTAINS the composed solid, so the "
				"reference below is a search over the whole of it" );

			const Scalar reference = GridSearchDistanceToSolid(
				sdf->GetParts(), pObj, kLo, kHi, kSteps );

			// gap_max, DERIVED and then MEASURED.  The two contributions
			// are the grid's own diagonal (sqrt(3) * 0.02 = 0.0347, since
			// the reference over-estimates by up to one grid cell) and the
			// probe's last doubling step, which starts at max(eps, Map)
			// with eps a small fraction of this SDF's bbox diagonal.  The
			// value this fixture actually produces is 0.0155 (printed
			// above on every run, so a regression is visible even when the
			// assertion still passes); the bound is set at 0.05, about
			// three times that, which is tight enough to catch a real
			// change in the bracket and loose enough not to be a recorded
			// golden.
			const Scalar gapMax = Scalar( 0.05 );
			const Scalar gap = reported - reference;

			std::cout << "    composed SDF (1-Lipschitz): lower(Map) = " << (double)lower
				<< ", grid reference = " << (double)reference
				<< ", reported = " << (double)reported
				<< ", gap_max MEASURED = " << (double)gap
				<< " (asserted <= " << (double)gapMax << ")" << std::endl;

			Check( reported >= lower - Scalar( 1e-9 ),
				"(b) MONEY -- the composed SDF's answer is at or above its Map lower bound "
				"(it never over-reads contact)" );
			Check( gap <= gapMax,
				"(b) MONEY -- and at most gap_max above a grid search of the same {Map <= 0} set" );
		}
	}

	// --- HEIGHTFIELD-MODE SDF REFUSES.  Its field is divided by a single
	// global Lipschitz bound, so the probe would step by a systematically
	// wrong length; refusing is the honest answer.
	//
	// ASKED OF THE OBJECT DIRECTLY, not through the manager -- the same
	// reason the non-coplanar clipped-plane check in (c) does.  A manager
	// probe at (66, 0, 0) with r = 5 also has `n_sdf_composed` in range
	// (4.83 away, and it is the object whose own probe budget the manager
	// answer would then be reporting on), so a green manager probe could
	// mean "the heightfield refused" OR "the composed SDF ran out of
	// budget" -- different facts, and only one of them is under test here.
	{
		IObjectPriv* hf = f.Obj( "n_sdf_heightfield" );
		Check( hf != 0, "(b) the heightfield fixture exists" );

		Scalar d = Scalar( 0 );
		Check( hf && !hf->DistanceToSurface( Point3( 66, 0, 0 ), Scalar( 5 ), d ),
			"(b) MONEY -- a heightfield-mode SDF REFUSES when asked directly" );

		// TEETH: an ordinary-mode SDF at the same radius ANSWERS, so the
		// refusal is heightfield mode and not the SDF family as a whole.
		// `n_sdf_sphere` is the exact-field fixture checked above.
		IObjectPriv* ok = f.Obj( "n_sdf_sphere" );
		Check( ok && ok->DistanceToSurface( Point3( 54, 0, 0 ), Scalar( 5 ), d ),
			"(b) ...teeth: an ordinary-mode SDF at the same radius answers" );
	}
}

//======================================================================
// (c) THE EXCLUSION RULES
//======================================================================

static void TestExclusions( const Fixture& f )
{
	std::cout << "(c) the exclusion rules -- self, emitters, CSG, refusing families" << std::endl;

	// --- SELF is excluded by IDENTITY.  A point ON the floor is at
	// distance 0 from the floor's own surface, so without the exclusion
	// every probe in this file would read 0 and every other check would
	// pass for the wrong reason.  The answer must be the sphere's 2.
	{
		const Scalar d = Probe( f, Point3( 0, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d, Scalar( 2 ), Scalar( 1e-9 ),
			"(c) MONEY -- self is excluded: a point ON the floor does not read 0" );

		// And the complement: with a DIFFERENT self, the floor does count,
		// and the same point now reads 0 because it is on the floor.
		const Scalar d2 = Probe( f, Point3( 0, 0, 0 ), f.Obj( "n_sphere" ), Scalar( 5 ) );
		CheckClose( d2, Scalar( 0 ), Scalar( 1e-9 ),
			"(c) ...and the floor DOES count for a different self (the exclusion is per-object)" );
	}

	// --- AN EMITTER NEVER COUNTS.  `n_emitter` is a 2x2x2 box whose
	// underside is at y = 2, so without the rule this would read 2.  The
	// nearest non-emitting neighbour is 6 units away in x, past the
	// radius, so the query must REFUSE.
	{
		const Scalar d = Probe( f, Point3( 84, 0, 0 ), f.floorObj, Scalar( 3 ) );
		Check( d < Scalar( 0 ),
			"(c) MONEY -- an emissive neighbour does not count (a light panel paints no grime)" );

		// TEETH: the same probe with a NON-emissive box the same distance
		// away does answer, so the refusal above is the emitter rule and
		// not an empty neighbourhood.
		const Scalar d2 = Probe( f, Point3( 90, 0, 0 ), f.floorObj, Scalar( 3 ) );
		CheckClose( d2, Scalar( 2 ), Scalar( 1e-9 ),
			"(c) ...teeth: an identical NON-emissive box at the same height reads 2" );
	}

	// --- `casts_shadows FALSE` DOES NOT EXEMPT.  That is the check
	// immediately above: `n_noshadow` carries the flag and still counts.
	// Stated separately so the intent is visible in the output.
	{
		IObjectPriv* ns = f.Obj( "n_noshadow" );
		Check( ns && !ns->DoesCastShadows(),
			"(c) the n_noshadow fixture really does carry casts_shadows FALSE" );
		const Scalar d = Probe( f, Point3( 90, 0, 0 ), f.floorObj, Scalar( 3 ) );
		Check( d > Scalar( 0 ),
			"(c) MONEY -- casts_shadows FALSE does NOT exempt: this is geometry presence" );
	}

	// --- CSG: the composite ANSWERS since Phase 3, and its operands still
	// never count separately.  `csg_a` / `csg_b` are registered in the
	// manager but are not world-visible, so they are filtered like any
	// other invisible object; only the composite is asked.
	//
	// THIS CHECK USED TO ASSERT A REFUSAL.  `n_csg` is `csg_a` (a 2x2x2
	// box whose underside is at y = 2) minus `csg_b` (a unit sphere at
	// y = 4, which bites the box's TOP), so the nearest point of the real
	// solid from the floor beneath it is the box's underside, 2 away --
	// and the subtraction's bracket lands there on the BOUNDARY arm,
	// because the box operand is an exact closed form under an exact
	// sigma.  Section (j) is where the composite queries are tested; this
	// is the manager-level statement that a composite is now a NEIGHBOUR
	// like any other.
	{
		IObjectPriv* a = f.Obj( "csg_a" );
		IObjectPriv* b = f.Obj( "csg_b" );
		Check( a && b, "(c) the CSG operands are registered in the manager" );
		Check( a && !a->IsWorldVisible(), "(c) ...and operand A is not world-visible" );
		Check( b && !b->IsWorldVisible(), "(c) ...and operand B is not world-visible" );

		const Scalar d = Probe( f, Point3( 96, 0, 0 ), f.floorObj, Scalar( 3 ) );
		CheckClose( d, Scalar( 2 ), Scalar( 1e-9 ),
			"(c) MONEY -- a CSG composite ANSWERS since Phase 3, with the minuend's underside "
			"at 2 -- and its operands still never count separately" );

		// TEETH on the operand exclusion: the SPHERE operand's own surface
		// is 3 away from this point (centre y = 4, radius 1), so if the
		// operands were being scanned separately the answer would still be
		// the box's 2 -- which proves nothing.  Ask instead from a point
		// where the SPHERE would win: directly above it, at y = 6, the
		// sphere's top is 1 away while the composite's real surface up
		// there has been REMOVED by that very sphere, so the nearest real
		// surface is the box's top rim.
		const Scalar dAbove = Probe( f, Point3( 96, 6, 0 ), f.Obj( "n_box" ), Scalar( 1.5 ) );
		Check( dAbove < Scalar( 0 ) || dAbove > Scalar( 1 ) + Scalar( 1e-9 ),
			"(c) MONEY -- ...and a point above the BITE does not read the subtrahend's own "
			"surface at 1: the operand is not a neighbour, only the composite is" );
	}

	// --- REFUSING FAMILIES.  A Bezier patch has no closed form; a
	// non-indexed RAW mesh has no BVH to traverse.  Both contribute
	// nothing rather than guessing.
	{
		const Scalar dPatch = Probe( f, Point3( 120, 0, 0 ), f.floorObj, Scalar( 5 ) );
		Check( dPatch < Scalar( 0 ), "(c) a Bezier patch REFUSES" );

		const Scalar dRaw = Probe( f, Point3( 126, 0, 0 ), f.floorObj, Scalar( 5 ) );
		Check( dRaw < Scalar( 0 ), "(c) a non-indexed RAW mesh REFUSES" );
	}

	// --- A NON-COPLANAR CLIPPED PLANE REFUSES, and so does a COPLANAR
	// NON-CONVEX one, while the coplanar convex twin is exact (checked in
	// (a)).  Both refusals are the same fact: this class traces the
	// BILINEAR PATCH through its four corners, and answering with the flat
	// POLYGON's distance is only right when the patch's image IS that
	// polygon -- which needs coplanarity AND convexity.  A skew quad is
	// genuinely curved; a coplanar DART folds, so over its reflex lobe the
	// polygon form reports |h| while the surface is further away.  Both
	// would be SMALLER than the truth, the forbidden direction.
	{
		// Asked of the OBJECT directly, not through the manager: what is
		// under test is the FAMILY's refusal, and a manager probe would
		// also be answering for whatever else happens to sit within the
		// radius -- so a pass could mean "it refused" or "nothing was
		// near", which are different facts.
		IObjectPriv* skew = f.Obj( "n_quad_skew" );
		IObjectPriv* flat = f.Obj( "n_quad" );
		IObjectPriv* dart = f.Obj( "n_quad_dart" );
		Check( skew && flat && dart, "(c) all three clipped-plane fixtures exist" );

		Scalar d = Scalar( 0 );
		Check( skew && !skew->DistanceToSurface( Point3( 36, 0, 0 ), Scalar( 5 ), d ),
			"(c) MONEY -- a NON-coplanar clipped plane refuses (it is a bilinear patch)" );

		// AND THE DART.  Its corners are all at y = 2.5, so it passes the
		// coplanarity test outright; what disqualifies it is that the
		// bilinear image of a NON-CONVEX planar quad is a proper subset of
		// the polygon, so the polygon form under-reports over the reflex
		// lobe.  The probe sits below the fixture, where the polygon form
		// would have answered 2.5 -- exactly the wrong number to accept.
		Check( dart && !dart->DistanceToSurface( Point3( 132, 0, 0 ), Scalar( 5 ), d ),
			"(c) MONEY -- a COPLANAR but NON-CONVEX clipped plane (a dart) refuses too: "
			"coplanarity alone does not make the polygon form exact" );

		// TEETH: the coplanar CONVEX twin, four corners of the same size
		// and at the same height, answers -- so the two refusals above are
		// the coplanarity and convexity tests, not the clipped-plane family
		// as a whole going quiet.
		Check( flat && flat->DistanceToSurface( Point3( 30, 0, 0 ), Scalar( 5 ), d ),
			"(c) ...teeth: the COPLANAR CONVEX twin answers" );
		CheckClose( d, Scalar( 2.5 ), Scalar( 1e-9 ), "(c) ...with its closed form, 2.5" );
	}

	// --- INSTANCED COPIES ARE SEPARATE OBJECTS and do count against each
	// other.  Two boxes share one geometry: `n_inst_src` (top at y = 4)
	// and `n_inst_copy` (bottom at y = 6).  A point midway at y = 5 is 1
	// from each, and BOTH directions must answer -- exclusion is by object
	// identity, not by geometry.
	{
		IObjectPriv* src  = f.Obj( "n_inst_src" );
		IObjectPriv* copy = f.Obj( "n_inst_copy" );
		Check( src && copy, "(c) both instanced objects exist" );
		Check( src && copy && src->GetGeometry() == copy->GetGeometry(),
			"(c) ...and they really do share ONE geometry" );

		const Scalar d1 = Probe( f, Point3( 102, 5, 0 ), src, Scalar( 2 ) );
		CheckClose( d1, Scalar( 1 ), Scalar( 1e-9 ),
			"(c) MONEY -- an instanced COPY counts against its source (distance 1)" );
		const Scalar d2 = Probe( f, Point3( 102, 5, 0 ), copy, Scalar( 2 ) );
		CheckClose( d2, Scalar( 1 ), Scalar( 1e-9 ),
			"(c) ...and the source counts against the copy, symmetrically" );
	}
}

//======================================================================
// (d) THE TRANSFORM
//======================================================================

static void TestTransform( const Fixture& f )
{
	std::cout << "(d) the transform -- exact uniform, bounded anisotropic, degenerate refuses" << std::endl;

	// --- UNIFORM SCALE + ROTATION IS EXACT.  `n_rot_sphere` is a unit
	// sphere under `orientation 37 21 53` and `scale 1.5`, centred at
	// (78, 4.5, 0).  M^T M = s^2 I holds exactly for that composition, so
	// the sigma detection returns 1.5 for both bounds and the answer is
	// the closed form to 1e-9: 4.5 - 1.5 = 3.
	{
		const Scalar d = Probe( f, Point3( 78, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( d, Scalar( 3 ), Scalar( 1e-9 ),
			"(d) MONEY -- a ROTATED, uniformly scaled sphere is EXACT (4.5 - 1.5 = 3)" );
	}

	// --- NON-UNIFORM SCALE IS AN UPPER BOUND.  `n_scaled_sdf` is the
	// exact-field unit-sphere SDF under `scale 2 1 1` at (72,3,0) -- an
	// ellipsoid with semi-axes (2,1,1) in world.  Under its centre the
	// TRUE distance is 3 - 1 = 2.  The Frobenius/determinant bounds give
	// sigmaMax = sqrt(4+1+1) = 2.449 and sigmaMin = 2/sigmaMax^2 = 0.333,
	// so the reported answer is above the truth but finite and bounded.
	{
		const Scalar d = Probe( f, Point3( 72, 0, 0 ), f.floorObj, Scalar( 8 ) );
		Check( d > Scalar( 0 ), "(d) the non-uniformly scaled SDF answers" );
		Check( d >= Scalar( 2 ) - Scalar( 1e-6 ),
			"(d) MONEY -- an anisotropic transform never reads BELOW the true distance" );
		std::cout << "    anisotropic scale 2 1 1: true 2, reported " << (double)d << std::endl;
	}

	// --- THE RADIUS CONVERSION, and an honest correction to what it buys.
	//
	// The design says the radius goes into object space DIVIDED by the
	// smallest singular value so a neighbour within `r` "cannot be
	// missed".  That conversion is SOUND and this checks it is in force --
	// but the property it was reasoned to protect turns out not to be
	// separately observable, and pretending otherwise would be a check
	// with no teeth.  The argument: the answer comes back MULTIPLIED by
	// sigmaMax, so a candidate whose reported distance lands within `r`
	// necessarily had `d_object <= r / sigmaMax <= r / sigmaMin` -- i.e.
	// the tighter conversion would have found it too.  Any candidate the
	// tighter conversion would have skipped reports a distance ABOVE `r`
	// and is dropped by the caller regardless.  So `r / sigmaMin` is
	// conservative in the safe direction and never changes an outcome.
	//
	// What IS observable, and is the behaviour an author will meet: at a
	// radius between the TRUE distance (2) and the BOUND (4.899), the
	// anisotropic neighbour is legitimately not painted.  That is the
	// documented under-paint, not a miss.
	{
		const Scalar dTight = Probe( f, Point3( 72, 0, 0 ), f.floorObj, Scalar( 2.05 ) );
		Check( dTight < Scalar( 0 ),
			"(d) at a radius between the true distance and the bound, an anisotropic "
			"neighbour is not painted -- the documented UNDER-paint, never a false contact" );

		const Scalar dWide = Probe( f, Point3( 72, 0, 0 ), f.floorObj, Scalar( 6 ) );
		Check( dWide > Scalar( 0 ),
			"(d) ...and at a radius above the bound it IS found" );
		Check( dWide >= Scalar( 2 ) - Scalar( 1e-6 ),
			"(d) ...still never below the truth" );
	}

	// --- A DEGENERATE TRANSFORM REFUSES.  Built through the API rather
	// than authored: a scene carrying `scale 0` produces log noise from
	// every other consumer of that object, and what is under test here is
	// one predicate, not a scene.
	{
		SphereGeometry* g = new SphereGeometry( Scalar( 1 ) );
		Object* flat = new Object( g );
		g->release();
		flat->SetStretch( Vector3( Scalar( 1 ), Scalar( 0 ), Scalar( 1 ) ) );	// det == 0
		flat->FinalizeTransformations();

		Scalar d = Scalar( 0 );
		Check( !flat->DistanceToSurface( Point3( 0, 5, 0 ), Scalar( 10 ), d ),
			"(d) MONEY -- a DEGENERATE (det == 0) transform refuses rather than answering "
			"through Matrix4Ops::Inverse's silent identity fallback" );

		// TEETH: the same object at a NON-degenerate scale does answer, so
		// the refusal above is the degeneracy and not the geometry.
		//
		// The geometry is released exactly as `flat`'s is above: `Object`'s
		// constructor addrefs it, so the constructing reference is ours to
		// drop and `new Object( new SphereGeometry(1) )` leaks it.
		SphereGeometry* g2 = new SphereGeometry( Scalar( 1 ) );
		Object* fine = new Object( g2 );
		g2->release();
		fine->SetStretch( Vector3( Scalar( 1 ), Scalar( 1 ), Scalar( 1 ) ) );
		fine->FinalizeTransformations();
		Check( fine->DistanceToSurface( Point3( 0, 5, 0 ), Scalar( 10 ), d ),
			"(d) ...teeth: the same sphere at a non-degenerate scale answers" );
		CheckClose( d, Scalar( 4 ), Scalar( 1e-9 ), "(d) ...with the closed form, 5 - 1 = 4" );

		fine->release();
		flat->release();
	}
}

//======================================================================
// (g) THE SNAPSHOT STAYS FRESH WHEN AN OBJECT IS ADDED
//======================================================================

//! THE HAZARD THIS CLOSES, and why it is specific to this signal.
//!
//! The manager's contract is "InvalidateSpatialStructure, then
//! PrepareForRendering, after any structural change", and the TLAS relies on
//! it entirely.  But the TLAS is only BUILT when the object count exceeds
//! `nMaxObjectsPerNode` (4); below that, `IntersectRay` walks the item map
//! LIVE, so the contract has never actually been load-bearing for a small
//! scene -- and `Job::AddObject` does NOT invalidate (confirmed by reading
//! it: it calls RegisterOrDiag and returns).
//!
//! The proximity snapshot IS built for a small scene, because a four-object
//! scene needs its boxes as much as a four-hundred-object one.  That would
//! have made a small scene the first place a missed invalidate produces a
//! WRONG answer rather than a stale one: an object added after Prepare would
//! RENDER (the linear loop sees it) and be INVISIBLE to every proximity query
//! (the snapshot would not).  So EnsureBoxSnapshot compares the entry count
//! as well as the null pointer.
//!
//! Built through the API rather than on scene C: what is under test is the
//! manager's own freshness, and scene C has far more than four objects, so it
//! could not exercise the regime the hazard lives in.
//!
//! NOTE ON WHAT `PrepareForRendering` DOES HERE.  Since the demand gate
//! shipped, the EAGER snapshot build is skipped when no live painter calls
//! `proximity()` -- which is the case in this API-built fixture -- so the
//! first `NearestOtherSurface` below takes the LAZY build under the tree
//! mutex instead.  That is deliberate coverage, not an accident: it means
//! this section exercises the lazy path and the count-check path in one
//! sequence, and the property under test (an added object is seen with no
//! invalidate) is independent of which of the two built the snapshot.
static void TestSnapshotFreshnessOnAdd()
{
	std::cout << "(g) an object added after PrepareForRendering is visible to the query" << std::endl;

	IObjectManager* mgr = 0;
	Check( RISE_API_CreateObjectManager( &mgr, true, false, 4, 32 ), "(g) a manager" );
	if( !mgr ) return;

	// TWO objects -- below the TLAS threshold, which is the regime the
	// hazard lives in.
	SphereGeometry* gA = new SphereGeometry( Scalar( 1 ) );
	Object* a = new Object( gA );
	gA->release();
	a->FinalizeTransformations();
	mgr->AddItem( a, "a" );

	SphereGeometry* gB = new SphereGeometry( Scalar( 1 ) );
	Object* b = new Object( gB );
	gB->release();
	b->SetPosition( Point3( 0, 10, 0 ) );
	b->FinalizeTransformations();
	mgr->AddItem( b, "b" );

	mgr->PrepareForRendering();

	Scalar d = Scalar( 0 );
	Check( !mgr->NearestOtherSurface( Point3( 0, 4, 0 ), a, Scalar( 2 ), d ),
		"(g) nothing is within 2 of the probe point yet" );

	// ADD A THIRD, without invalidating anything -- exactly what
	// Job::AddObject does.
	SphereGeometry* gC = new SphereGeometry( Scalar( 1 ) );
	Object* c = new Object( gC );
	gC->release();
	c->SetPosition( Point3( 0, 5, 0 ) );
	c->FinalizeTransformations();
	mgr->AddItem( c, "c" );

	Check( mgr->NearestOtherSurface( Point3( 0, 4, 0 ), a, Scalar( 2 ), d ),
		"(g) MONEY -- the newly added object IS seen, with no invalidate: a snapshot that "
		"only checked its null pointer would have missed it while the renderer drew it" );
	CheckClose( d, Scalar( 0 ), Scalar( 1e-9 ),
		"(g) ...and the probe point is on its surface, so the distance is 0" );

	c->release();
	b->release();
	a->release();
	safe_release( mgr );
}

//======================================================================
// (g2) THE SAME STALENESS CONTRACT, ON A TLAS-BACKED SCENE, THROUGH
// Job::AddObject -- NOT the raw ObjectManager::AddItem (g) above uses.
//======================================================================

//! (g) above proves the count-check keeps a <=4-object (flat-scan) scene
//! honest. This section asks the opposite-regime question the design's own
//! comment at ObjectManager.cpp ~682 states as a CONTRACT SHIFT: on a
//! TLAS-backed scene (more than `nMaxObjectsPerNode` objects) there is NO
//! count check, so a proximity query is now "exactly as stale as the
//! RENDER" -- an object added without an invalidate must be invisible to
//! BOTH `NearestOtherSurface` AND `IntersectRay`, not silently visible to
//! one and not the other.
//!
//! Driven through `Job::AddObject` (src/Library/Job.cpp ~7416), the actual
//! entry point the parser's `standard_object` chunk calls and the one the
//! agent's incremental derive reaches for a live edit -- not
//! `ObjectManager::AddItem` directly (g) uses -- because what is under
//! test here is that THIS call, read end to end, never invalidates: it
//! resolves references, creates-or-repoints the object, assigns bindings,
//! transforms it, and (for a fresh create) calls `RegisterOrDiag` ->
//! `IManager::AddItem` -- and nothing in that path touches
//! `InvalidateSpatialStructure` or `PrepareForRendering`.
static void TestTLASStalenessViaJobAddObject()
{
	std::cout << "(g2) TLAS-backed staleness through Job::AddObject: proximity sees the "
		"7th object exactly when IntersectRay does" << std::endl;

	Job* job = new Job();
	Check( job->AddSphereGeometry( "sph", 1.0 ), "(g2) sphere geometry registered" );

	// SIX objects, well clear of `nMaxObjectsPerNode` (4, Job's own
	// ObjectManager default -- Job.cpp's InitializeContainers), so
	// PrepareForRendering below builds a TLAS rather than leaving the
	// small-scene flat-scan path (g)'s regime already covers. Spread along
	// x so none of them is anywhere near the 7th object added later at
	// x = 100.
	const double orient[3] = { 0, 0, 0 };
	const double scale[3]  = { 1, 1, 1 };
	RadianceMapConfig radCfg;
	for( int i = 0; i < 6; ++i ) {
		char name[16];
		snprintf( name, sizeof( name ), "s%d", i );
		const double pos[3] = { (double)i * 10.0, 0, 0 };
		Check( job->AddObject( name, "sph", 0, 0, 0, radCfg, pos, orient, scale, true, true ),
			"(g2) seed object added" );
	}

	IObjectManager* mgr = job->GetObjects();
	Check( mgr != 0, "(g2) the job has an object manager" );
	if( !mgr ) { job->release(); return; }
	mgr->PrepareForRendering();

	// THE PROBE POINT AND RAY, both aimed at where the 7th object will be
	// (x = 100), far from every seed object above (nearest is at x = 50).
	const Point3 proxProbe( 100, 0, 3 );		// 2 units off a radius-1 sphere at (100,0,0)
	const Point3 rayOrigin( 100, 0, -50 );
	const Vector3 rayDir( 0, 0, 1 );			// travels through (100,0,0) toward +z

	Scalar dPre = 0;
	Check( !mgr->NearestOtherSurface( proxProbe, mgr->GetItem( "s0" ), Scalar( 5 ), dPre ),
		"(g2) before the 7th object exists, proximity finds nothing near x=100" );
	{
		RayIntersection ri( Ray( rayOrigin, rayDir ), RasterizerState() );
		mgr->IntersectRay( ri, true, true, false );
		Check( !ri.geometric.bHit,
			"(g2) ...and IntersectRay agrees: nothing along that ray either" );
	}

	// ADD THE 7TH, through Job::AddObject -- exactly the call a
	// `standard_object` chunk (initial OR incremental) resolves to, and
	// deliberately NOT followed by an invalidate.
	const double pos7[3] = { 100, 0, 0 };
	Check( job->AddObject( "s6", "sph", 0, 0, 0, radCfg, pos7, orient, scale, true, true ),
		"(g2) the 7th object is added via Job::AddObject, with no invalidate" );

	Scalar dStale = 0;
	const bool proxStillBlind = !mgr->NearestOtherSurface( proxProbe, mgr->GetItem( "s0" ), Scalar( 5 ), dStale );
	bool rayStillBlind = false;
	{
		RayIntersection ri( Ray( rayOrigin, rayDir ), RasterizerState() );
		mgr->IntersectRay( ri, true, true, false );
		rayStillBlind = !ri.geometric.bHit;
	}
	Check( proxStillBlind,
		"(g2) MONEY -- Job::AddObject does NOT invalidate: proximity is STILL blind to the 7th "
		"object on this TLAS-backed scene, exactly as ObjectManager.cpp's own contract-shift "
		"comment states" );
	Check( rayStillBlind,
		"(g2) ...and IntersectRay is EQUALLY blind -- the picture and the signal are stale "
		"together, not one before the other" );
	// NOTE: `proxStillBlind == rayStillBlind` is NOT checked as a separate
	// assertion here -- with both operands individually pinned to `true` by
	// the two Checks immediately above, that equality is a tautology (true
	// == true) and would read as an independent guard while proving
	// nothing beyond what they already established. The identity the
	// design promises -- proximity is exactly as stale as the render,
	// never staler and never fresher -- is exactly what those two Checks,
	// taken together, already state.

	// REBUILD -- the documented recovery -- and both must now see it.
	mgr->InvalidateSpatialStructure();
	mgr->PrepareForRendering();

	Scalar dFresh = 0;
	const bool proxSeesIt = mgr->NearestOtherSurface( proxProbe, mgr->GetItem( "s0" ), Scalar( 5 ), dFresh );
	bool raySeesIt = false;
	Scalar rayHitZ = 0;
	{
		RayIntersection ri( Ray( rayOrigin, rayDir ), RasterizerState() );
		mgr->IntersectRay( ri, true, true, false );
		raySeesIt = ri.geometric.bHit;
		if( raySeesIt ) rayHitZ = ri.geometric.ptIntersection.z;
	}
	Check( proxSeesIt && raySeesIt,
		"(g2) MONEY -- after InvalidateSpatialStructure + PrepareForRendering, BOTH proximity "
		"and IntersectRay see the 7th object" );
	CheckClose( dFresh, Scalar( 2 ), Scalar( 1e-9 ),
		"(g2) ...proximity reads the closed form (3 - 1 radius = 2)" );
	Check( raySeesIt && rayHitZ < Scalar( 0 ),
		"(g2) ...and the ray now hits the sphere's near face (z = -1), not a miss" );

	job->release();
}

//======================================================================
// (e) THE SIGNAL'S OWN CONVENTIONS
//======================================================================

static void TestConventions( const Fixture& f )
{
	std::cout << "(e) unsigned, interpenetration, the cut-off, and every neutral" << std::endl;

	// --- UNSIGNED: a neighbour BELOW reads the same as one above.
	// `n_below` is a box at (108,-3,0), underside... top at y = -2, so a
	// floor point above it is 2 away -- exactly what `n_box` at (6,3,0)
	// reads from below it.
	{
		const Scalar dBelow = Probe( f, Point3( 108, 0, 0 ), f.floorObj, Scalar( 3 ) );
		const Scalar dAbove = Probe( f, Point3( 6, 0, 0 ), f.floorObj, Scalar( 3 ) );
		CheckClose( dBelow, Scalar( 2 ), Scalar( 1e-9 ), "(e) a neighbour BELOW reads 2" );
		CheckClose( dBelow, dAbove, Scalar( 1e-12 ),
			"(e) MONEY -- the signal is UNSIGNED: below and above read identically" );
	}

	// --- INTERPENETRATION READS 1, and this is a CONVENTION rather than a
	// measurement.  A point at the centre of a 2x2x2 box is exactly 1 away
	// from its nearest face, and the box's signed field knows that
	// exactly -- but the signal does not report it.  Its contract is that
	// a point inside a neighbour is IN CONTACT with it, so the distance is
	// 0 and the signal saturates at 1.  Checked at the centre precisely
	// because that is where "distance to the surface" and "0" differ most:
	// a check just inside the face would pass either way.
	{
		const Scalar d = Probe( f, Point3( 6, 3, 0 ), f.floorObj, Scalar( 3 ) );
		CheckClose( d, Scalar( 0 ), Scalar( 1e-12 ),
			"(e) MONEY -- a point INSIDE a neighbour reads distance 0, not its 1.0 "
			"distance-to-nearest-face (interpenetration IS contact)" );
		const Scalar v = ProximityAt( f, Point3( 6, 3, 0 ), f.floorObj, Scalar( 3 ) );
		CheckClose( v, Scalar( 1 ), Scalar( 1e-12 ),
			"(e) ...so the signal saturates at 1" );

		// The same rule across the other solid families, so the convention
		// is not one family's accident: the sphere, the capped cylinder,
		// the torus's tube, the ellipsoid and the SDF all read 0 inside.
		CheckClose( Probe( f, Point3( 0, 3, 0 ), f.Obj( "n_box" ), Scalar( 3 ) ),
			Scalar( 0 ), Scalar( 1e-12 ), "(e) ...sphere: inside reads 0" );
		CheckClose( Probe( f, Point3( 12, 4, 0 ), f.Obj( "n_box" ), Scalar( 3 ) ),
			Scalar( 0 ), Scalar( 1e-12 ), "(e) ...capped cylinder: inside reads 0" );
		CheckClose( Probe( f, Point3( 42, 3, 1.5 ), f.Obj( "n_box" ), Scalar( 3 ) ),
			Scalar( 0 ), Scalar( 1e-12 ), "(e) ...torus tube: inside reads 0" );
		CheckClose( Probe( f, Point3( 48, 3, 0 ), f.Obj( "n_box" ), Scalar( 3 ) ),
			Scalar( 0 ), Scalar( 1e-12 ), "(e) ...ellipsoid: inside reads 0" );
		CheckClose( Probe( f, Point3( 54, 3, 0 ), f.Obj( "n_box" ), Scalar( 3 ) ),
			Scalar( 0 ), Scalar( 1e-12 ), "(e) ...SDF: inside reads 0" );
	}

	// --- THE CUT-OFF IS EXACTLY AT r.  `n_box`'s underside is 2 below;
	// a radius of 2 puts it exactly at the boundary (proximity 0), and
	// anything smaller refuses outright.
	{
		const Scalar vJustIn = ProximityAt( f, Point3( 6, 0, 0 ), f.floorObj, Scalar( 4 ) );
		CheckClose( vJustIn, Scalar( 0.5 ), Scalar( 1e-9 ),
			"(e) at r = 4 a neighbour 2 away reads 1 - 2/4 = 0.5" );

		const Scalar vAtEdge = ProximityAt( f, Point3( 6, 0, 0 ), f.floorObj, Scalar( 2 ) );
		CheckClose( vAtEdge, Scalar( 0 ), Scalar( 1e-9 ),
			"(e) at r exactly the distance, the signal is 0 -- the cut-off is not a fade-out" );

		const Scalar vOut = ProximityAt( f, Point3( 6, 0, 0 ), f.floorObj, Scalar( 1.5 ) );
		CheckClose( vOut, Scalar( 0 ), Scalar( 1e-12 ),
			"(e) beyond the radius the answer is exactly the neutral 0" );

		// And the FAR fixture, 30 units up, is out of every sane radius.
		const Scalar vFar = ProximityAt( f, Point3( 114, 0, 0 ), f.floorObj, Scalar( 5 ) );
		CheckClose( vFar, Scalar( 0 ), Scalar( 1e-12 ),
			"(e) a neighbour far past the radius reads the neutral 0, not a small positive" );
	}

	// --- EVERY NEUTRAL.  A channel with no scene, no self, a non-finite
	// point, and a computed radius that is <= 0 or non-finite.
	{
		SurfaceSignalInfo blank;
		CheckClose( blank.Proximity( Scalar( 1 ) ), Scalar( 0 ), Scalar( 0 ),
			"(e) a default-constructed channel (no scene) reads the neutral 0" );

		SurfaceSignalInfo noSelf;
		noSelf.pScene  = f.mgr;
		noSelf.ptWorld = Point3( 0, 0, 0 );
		ExpressionMemo::Invalidate();
		CheckClose( noSelf.Proximity( Scalar( 5 ) ), Scalar( 0 ), Scalar( 0 ),
			"(e) a channel with a scene but NO self (a miss) reads the neutral 0" );

		SurfaceSignalInfo bad;
		bad.pScene  = f.mgr;
		bad.pSelf   = f.floorObj;
		bad.ptWorld = Point3( 0, std::numeric_limits<double>::quiet_NaN(), 0 );	// HYGIENE-OK: an input, not a sentinel
		ExpressionMemo::Invalidate();
		CheckClose( bad.Proximity( Scalar( 5 ) ), Scalar( 0 ), Scalar( 0 ),
			"(e) a NON-FINITE hit point reads the neutral 0" );

		const Scalar vNeg = ProximityAt( f, Point3( 0, 0, 0 ), f.floorObj, Scalar( -1 ) );
		CheckClose( vNeg, Scalar( 0 ), Scalar( 0 ),
			"(e) a computed radius <= 0 reads the neutral 0" );
		const Scalar vNan = ProximityAt( f, Point3( 0, 0, 0 ), f.floorObj,
			(Scalar)std::numeric_limits<double>::quiet_NaN() );	// HYGIENE-OK: an input, not a sentinel
		CheckClose( vNan, Scalar( 0 ), Scalar( 0 ),
			"(e) a NON-FINITE computed radius reads the neutral 0" );
	}
}

//======================================================================
// (f) THE BUILTIN, END TO END
//======================================================================

static bool CompileWithContext( const std::string& body, ExpressionProgram& out )
{
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	return b.Finalize( body, out );
}

//! Fire a ray THROUGH THE OBJECT MANAGER (never through an object
//! directly) and evaluate `body` at the hit.  Going through the manager is
//! the whole point: it is the only site that stamps `pScene` / `pSelf` /
//! `ptWorld`, so an `obj->IntersectRay` shortcut would silently test the
//! neutral path.
static bool EvalAtManagerHit( const Fixture& f, const Point3& origin, const Vector3& dir,
	const std::string& body, Scalar& outValue )
{
	ExpressionProgram prog = ExpressionProgram::Invalid();
	if( !CompileWithContext( body, prog ) ) return false;

	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	RayIntersection ri( Ray( origin, dir ), RasterizerState() );
	ExpressionMemo::Invalidate();
	f.mgr->IntersectRay( ri, true, true, false );
	const bool hit = ri.geometric.bHit;
	if( hit ) {
		outValue = painter->GetValuesAt( ri.geometric ).v[0];
	}
	painter->release();
	return hit;
}

static void TestBuiltinEndToEnd( const Fixture& f )
{
	std::cout << "(f) the builtin end to end -- the stamp, the remap guard, the diagnostic" << std::endl;

	// --- THE STAMP.  A ray fired through the manager onto the FLOOR, at
	// x = 8.5: past the box (which spans x in [5,7]) so the ray reaches
	// the floor, and near enough that the box is the nearest neighbour.
	// Straight down at x = 0 would hit the SPHERE, not the floor, and
	// every check below would then be about the wrong receiver.
	{
		RayIntersection ri( Ray( Point3( 8.5, 5, 0 ), Vector3( 0, -1, 0 ) ), RasterizerState() );
		f.mgr->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit, "(f) the probe ray hits" );
		Check( ri.geometric.signals.pScene == f.mgr,
			"(f) MONEY -- the manager stamped itself on the winning record" );
		Check( ri.geometric.signals.pSelf == ri.pObject,
			"(f) ...and pSelf is ri.pObject, so the two identities cannot disagree" );
		CheckClose( ri.geometric.signals.ptWorld.y, ri.geometric.ptIntersection.y, Scalar( 0 ),
			"(f) ...and ptWorld is the hit point" );
	}

	// --- THE BUILTIN AT A REAL HIT.  The floor point (8.5, 0, 0) is
	// exactly 2.5 from the box's nearest corner edge (7, 2, 0):
	// sqrt(1.5^2 + 2^2) = 2.5.  So `proximity(4)` reads 1 - 2.5/4 = 0.375.
	// The receiver is an INFINITE PLANE, which publishes no signal
	// provider at all -- so this also proves the signal works where
	// occlusion / convexity / thickness cannot answer.
	{
		Scalar v = Scalar( -1 );
		Check( EvalAtManagerHit( f, Point3( 8.5, 5, 0 ), Vector3( 0, -1, 0 ), "proximity(4.0)", v ),
			"(f) the expression evaluates at a manager hit" );
		CheckClose( v, Scalar( 0.375 ), Scalar( 1e-6 ),
			"(f) MONEY -- proximity(4.0) reads 1 - 2.5/4 on a floor 2.5 from a box corner" );

		// The same hit's self-signals are all neutral, because an infinite
		// plane publishes no provider -- the contrast that makes the point.
		Scalar occ = Scalar( -1 );
		Check( EvalAtManagerHit( f, Point3( 8.5, 5, 0 ), Vector3( 0, -1, 0 ), "occlusion(0.1)", occ ),
			"(f) occlusion evaluates at the same hit" );
		CheckClose( occ, Scalar( 1 ), Scalar( 0 ),
			"(f) ...and reads its NEUTRAL 1, because this receiver has no provider at all" );
	}

	// --- THE DynR REMAP GUARD.  `ParseCall`'s remap turns a signal call
	// whose radius is not a bare literal into its `...DynR` twin, and its
	// final ternary arm used to be an UNGUARDED fall-through to
	// kFnConvexityDynR.  Without the guard, `proximity(0.1*2)` -- a
	// computed radius -- compiles to a CONVEXITY call.
	//
	// PROVED BEHAVIOURALLY, not by reading the emitted id: the compiled
	// code is not exposed, and behaviour is the thing that matters anyway.
	// The receiver is an infinite plane, so convexity reads its neutral 0
	// while proximity reads 0.5 -- two values that cannot be confused.
	{
		Scalar vLiteral = Scalar( -1 ), vComputed = Scalar( -1 );
		Check( EvalAtManagerHit( f, Point3( 8.5, 5, 0 ), Vector3( 0, -1, 0 ), "proximity(4.0)", vLiteral ),
			"(f) literal-radius proximity compiles and evaluates" );
		Check( EvalAtManagerHit( f, Point3( 8.5, 5, 0 ), Vector3( 0, -1, 0 ), "proximity(2.0*2.0)", vComputed ),
			"(f) COMPUTED-radius proximity compiles and evaluates" );

		CheckClose( vComputed, vLiteral, Scalar( 1e-12 ),
			"(f) MONEY -- a COMPUTED radius gives the same answer as the literal one: the DynR "
			"remap's unguarded arm is not turning proximity() into convexity()" );
		Check( vComputed > Scalar( 0.3 ),
			"(f) ...and it is emphatically not convexity's neutral 0 on this provider-less receiver" );
	}

	// --- THE PARSE-TIME DIAGNOSTIC NAMES THE RIGHT UNIT.  A literal
	// radius <= 0 is a compile error on every signal builtin; the three
	// self-signals are told it is a FRACTION, and proximity must be told
	// it is a WORLD LENGTH -- telling a proximity author "it is a
	// fraction" would send them to fix the one thing that was right.
	{
		ExpressionProgram bad = ExpressionProgram::Invalid();
		Check( !CompileWithContext( "proximity(-1.0)", bad ),
			"(f) a non-positive literal proximity radius is a COMPILE error" );
		const std::string err = bad.Error();
		Check( err.find( "WORLD LENGTH" ) != std::string::npos,
			"(f) MONEY -- and the message says WORLD LENGTH: " + err );
		Check( err.find( "FRACTION" ) == std::string::npos,
			"(f) ...and does NOT repeat the self-signals' fraction wording" );

		ExpressionProgram badOcc = ExpressionProgram::Invalid();
		Check( !CompileWithContext( "occlusion(-1.0)", badOcc ),
			"(f) teeth: occlusion's own diagnostic still fires" );
		Check( badOcc.Error().find( "FRACTION" ) != std::string::npos,
			"(f) ...and still says FRACTION, unchanged" );
	}

	// --- THE UV-ONLY SURFACE STILL REFUSES IT.  `proximity` joined
	// `isSignalFn`, which gates the expression_function2d refusal; a
	// builder with context vars OFF must reject it with the dedicated
	// message rather than "unknown function".
	{
		ExpressionProgram p = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;			// context vars OFF (the default)
		Check( !b.Finalize( "proximity(0.5)", p ),
			"(f) proximity() is refused on the UV-only expression_function2d surface" );
		Check( p.Error().find( "3D surface context" ) != std::string::npos,
			"(f) ...with the dedicated diagnostic, not `unknown function`" );
	}

	// --- IT IS RECORDED AS A SIGNAL CALL SITE, which is what makes a body
	// containing it memo-worthy and what SurfaceSignalDemand counts.
	{
		ExpressionProgram p = ExpressionProgram::Invalid();
		Check( CompileWithContext( "proximity(0.002)", p ), "(f) a proximity body compiles" );
		Check( p.SurfaceSignalCalls().size() == 1, "(f) one signal call site recorded" );
		if( p.SurfaceSignalCalls().size() == 1 ) {
			Check( p.SurfaceSignalCalls()[0].fn == ExpressionProgram::kFnProximity,
				"(f) ...recorded as kFnProximity, not as one of the self-signals" );
			Check( p.SurfaceSignalCalls()[0].radiusIsLiteral,
				"(f) ...with its literal radius proved" );
			CheckClose( p.SurfaceSignalCalls()[0].radiusLiteral, Scalar( 0.002 ), Scalar( 1e-12 ),
				"(f) ...and its value recorded" );
		}
		Check( p.MemoWorthy(), "(f) a body calling proximity() is memo-worthy" );
	}
}


//======================================================================
// (h) THE SIGNED LOWER BOUND
//======================================================================

//! One signed probe against ONE object, bypassing the manager: what
//! `interior` and a CSG composite's composed field both read.  Returns
//! true and fills both outputs, or false on a refusal.
//! DELIBERATELY DOES NOT PRE-CLEAR ITS OUT-PARAMETERS.  The contract on
//! `IGeometry::SignedDistanceLower` is that the REFUSING path clears
//! `outExact` itself, "so an implementer that forgets it cannot leak a
//! stale `true` from the caller's stack" -- and a helper that zeroed the
//! flag on the way in would make every "the refusal CLEARS the flag" check
//! below untestable, proving only that the refusal does not positively SET
//! it.  Callers that want teeth poison the flag with `true` first.
static bool SignedAt( const IObjectPriv* obj, const Point3& p, const Scalar r,
	Scalar& outSigned, bool& outExact )
{
	return obj && obj->SignedDistanceLower( p, r, outSigned, outExact );
}

//! BRUTE FORCE: the shortest distance from `p` to the surface of the
//! ellipsoid with semi-axes (a,b,c) centred at the origin, by a dense
//! parametric grid.  A minimum over a SUBSET of the surface is an UPPER
//! reference on the true distance, which is why the check that consumes it
//! compares it against a closed form rather than standing on it alone.
static Scalar BruteForceDistanceToEllipsoidSurface(
	const Point3& p, const Scalar a, const Scalar b, const Scalar c,
	const int nTheta, const int nPhi )
{
	Scalar best = RISE_INFINITY;
	for( int i = 0; i <= nTheta; ++i ) {
		const double th = PI * (double)i / (double)nTheta;
		const double st = std::sin( th ), ct = std::cos( th );
		for( int j = 0; j < nPhi; ++j ) {
			const double ph = 2.0 * PI * (double)j / (double)nPhi;
			const double x = (double)a * st * std::cos( ph );
			const double y = (double)b * ct;
			const double z = (double)c * st * std::sin( ph );
			const double dx = x - (double)p.x, dy = y - (double)p.y, dz = z - (double)p.z;
			const Scalar d = (Scalar)std::sqrt( dx*dx + dy*dy + dz*dz );
			if( d < best ) best = d;
		}
	}
	return best;
}

//! THE SIGNED LOWER BOUND, per family (design 5.6).  Three properties,
//! and they are not the unsigned query's:
//!   * the SIGN is exact -- negative strictly inside, positive strictly
//!     outside -- or the family REFUSES.  Every SHEET refuses: it cannot
//!     say what "inside" means.
//!   * the MAGNITUDE is a LOWER bound, the opposite direction from
//!     `DistanceToSurface`'s upper one.
//!   * the EXACTNESS FLAG is set only by the four closed-form SOLIDS, and
//!     only under a similarity transform.  A composite's boundary arm
//!     consumes it, so a family that sets it wrongly re-admits the phantom
//!     touching set.
static void TestSignedLowerBound( const Fixture& f )
{
	std::cout << "(h) the SIGNED lower bound -- exact sign, lower magnitude, the exactness flag" << std::endl;

	// --- THE FOUR CLOSED-FORM SOLIDS, INSIDE.  These are the SAME six
	// interpenetration probes section (e) checks read 0 through the
	// unsigned query; here the depth the unsigned query throws away at its
	// clamp is the answer, and it is exact for these four.
	{
		Scalar fv = 0; bool ex = false;

		Check( SignedAt( f.Obj( "n_sphere" ), Point3( 0, 3, 0 ), Scalar( 10 ), fv, ex ),
			"(h) sphere answers the signed query at its centre" );
		CheckClose( fv, Scalar( -1 ), Scalar( 1e-9 ),
			"(h) MONEY -- sphere R=1: its centre is -1, the DEPTH the unsigned query clamps to 0" );
		Check( ex, "(h) ...and the sphere's magnitude is EXACT" );

		Check( SignedAt( f.Obj( "n_box" ), Point3( 6, 3, 0 ), Scalar( 10 ), fv, ex ),
			"(h) box answers the signed query at its centre" );
		CheckClose( fv, Scalar( -1 ), Scalar( 1e-9 ),
			"(h) MONEY -- box 2x2x2: its centre is -1 from the nearest face" );
		Check( ex, "(h) ...and the box's magnitude is EXACT" );

		Check( SignedAt( f.Obj( "n_cyl_capped" ), Point3( 12, 4, 0 ), Scalar( 10 ), fv, ex ),
			"(h) capped cylinder answers the signed query on its axis" );
		CheckClose( fv, Scalar( -1 ), Scalar( 1e-9 ),
			"(h) MONEY -- capped cylinder R=1 h=4: its centre is -1 (the RADIAL wall, not the "
			"2.0 cap distance)" );
		Check( ex, "(h) ...and the capped cylinder's magnitude is EXACT" );

		// The torus probe sits on the TUBE'S CENTRE CIRCLE, at lateral
		// offset 1.5 from the ring centre -- the deepest point of the
		// tube, and the only one whose depth is the minor radius exactly.
		Check( SignedAt( f.Obj( "n_torus" ), Point3( 42, 3, 1.5 ), Scalar( 10 ), fv, ex ),
			"(h) torus answers the signed query on its tube's centre circle" );
		CheckClose( fv, Scalar( -0.5 ), Scalar( 1e-9 ),
			"(h) MONEY -- torus minor 0.5: the tube's centre circle is -0.5" );
		Check( ex, "(h) ...and the torus's magnitude is EXACT" );
	}

	// --- THE SAME FOUR, OUTSIDE: the signed answer is the PLUS distance,
	// and it agrees with the unsigned query exactly where the unsigned one
	// is exact.  Teeth against a sign-flip or a clamp leaking in.
	{
		Scalar fv = 0; bool ex = false;
		Check( SignedAt( f.Obj( "n_box" ), Point3( 6, 0, 0 ), Scalar( 10 ), fv, ex ) && ex,
			"(h) box answers exactly from a floor point below it" );
		CheckClose( fv, Scalar( 2 ), Scalar( 1e-9 ),
			"(h) MONEY -- OUTSIDE the signed answer is +2, the same number the unsigned query gives" );

		Check( SignedAt( f.Obj( "n_sphere" ), Point3( 0, 0, 0 ), Scalar( 10 ), fv, ex ) && ex,
			"(h) sphere answers exactly from the floor below it" );
		CheckClose( fv, Scalar( 2 ), Scalar( 1e-9 ), "(h) ...sphere: 3 - 1 = +2" );

		Check( SignedAt( f.Obj( "n_cyl_capped" ), Point3( 12, 0, 0 ), Scalar( 10 ), fv, ex ) && ex,
			"(h) capped cylinder answers exactly from the floor below it" );
		CheckClose( fv, Scalar( 2 ), Scalar( 1e-9 ), "(h) ...capped cylinder: bottom cap at y=2, so +2" );

		Check( SignedAt( f.Obj( "n_torus" ), Point3( 42, 0, 1.5 ), Scalar( 10 ), fv, ex ) && ex,
			"(h) torus answers exactly from the floor under its tube" );
		CheckClose( fv, Scalar( 2.5 ), Scalar( 1e-9 ), "(h) ...torus: 3 - 0.5 = +2.5" );
	}

	// --- THE ELLIPSOID: the SIGN is exact, the MAGNITUDE is
	// `dUnit x min(a,b,c)` and therefore a LOWER bound, and the flag is
	// NEVER set.  The centre probe is where the bound is TIGHT (the depth
	// to the nearest surface point IS the smallest semi-axis), which is
	// why the design keeps the interpenetration probe there and says so.
	{
		Scalar fv = 0; bool ex = false;
		Check( SignedAt( f.Obj( "n_ellipsoid" ), Point3( 48, 3, 0 ), Scalar( 10 ), fv, ex ),
			"(h) ellipsoid answers the signed query at its centre" );
		CheckClose( fv, Scalar( -1 ), Scalar( 1e-9 ),
			"(h) MONEY -- ellipsoid (2,1,1) at its centre: -1 = dUnit x min(a,b,c), and the "
			"bound is TIGHT there (the true depth is the smallest semi-axis)" );
		Check( !ex, "(h) MONEY -- ...but the ellipsoid NEVER carries the exactness flag" );

		// OUTSIDE and OFF the minor axis, where the bound is genuinely
		// loose: 4 units along +x from the centre, the true distance is
		// 4 - 2 = 2 and the reported lower bound is dUnit(=1) x min(=1).
		Check( SignedAt( f.Obj( "n_ellipsoid" ), Point3( 52, 3, 0 ), Scalar( 10 ), fv, ex ),
			"(h) ellipsoid answers 4 units out along its MAJOR axis" );
		Check( !ex, "(h) ...still without the flag" );
		Check( fv > Scalar( 0 ), "(h) ...with an exact POSITIVE sign outside" );
		Check( fv <= Scalar( 2 ) + Scalar( 1e-9 ),
			"(h) MONEY -- and the magnitude is a LOWER bound on the true distance 2" );
		std::cout << "    ellipsoid (2,1,1), station 4 along +x: true 2, signed lower bound "
			<< (double)fv << std::endl;
	}

	// --- THE SDF: `Map` itself.  Sign exact, magnitude a lower bound,
	// flag never set -- and, unlike the unsigned query, NO RANGE REFUSAL:
	// a tiny budget still gets an answer, because a CSG descent asks its
	// operands about points far outside any radius.
	{
		Scalar fv = 0; bool ex = false;
		Check( SignedAt( f.Obj( "n_sdf_sphere" ), Point3( 54, 3, 0 ), Scalar( 10 ), fv, ex ),
			"(h) the exact-field SDF answers the signed query at its centre" );
		CheckClose( fv, Scalar( -1 ), Scalar( 1e-9 ),
			"(h) MONEY -- SDF sphere R=1: Map at the centre is -1, the exact depth" );
		Check( !ex, "(h) MONEY -- ...and an SDF NEVER carries the exactness flag" );

		Check( SignedAt( f.Obj( "n_sdf_sphere" ), Point3( 54, 0, 0 ), Scalar( 10 ), fv, ex ),
			"(h) ...and answers from outside" );
		CheckClose( fv, Scalar( 2 ), Scalar( 1e-9 ), "(h) ...+2 outside, this field being exact" );

		// NO RANGE REFUSAL.  The unsigned query's step-1 early-out would
		// return false here (Map = 2 > 0.001); the signed one must not.
		Scalar fTiny = 0; bool exTiny = false;
		Check( SignedAt( f.Obj( "n_sdf_sphere" ), Point3( 54, 0, 0 ), Scalar( 0.001 ), fTiny, exTiny ),
			"(h) MONEY -- the signed query does NOT refuse for RANGE: a 1 mm budget still "
			"answers, because a composite's descent evaluates operands well outside any radius" );
		CheckClose( fTiny, Scalar( 2 ), Scalar( 1e-9 ), "(h) ...with the same number" );
		Scalar dUnsigned = Scalar( 0 );
		Check( !f.Obj( "n_sdf_sphere" )->DistanceToSurface( Point3( 54, 0, 0 ), Scalar( 0.001 ), dUnsigned ),
			"(h) ...teeth: the UNSIGNED query at the same budget refuses, as it should" );

		// The COMPOSED (1-Lipschitz, not exact) SDF: sign still exact.
		Check( SignedAt( f.Obj( "n_sdf_composed" ), Point3( 59.4, 3, 0 ), Scalar( 10 ), fv, ex ),
			"(h) the composed SDF answers the signed query" );
		Check( fv < Scalar( 0 ),
			"(h) MONEY -- ...and its sign is exact inside the smin-blended solid" );
		Check( !ex, "(h) ...without the flag" );
	}

	// --- EVERY SHEET REFUSES THE SIGNED QUERY, and the ones that answer
	// the UNSIGNED one still do.  This pairing is the whole point: a sheet
	// is a perfectly good NEIGHBOUR (it has a distance) and a hopeless
	// OPERAND (it has no inside), so the two queries must disagree about
	// it -- which is what makes a subtracted plane force a composite to
	// refuse rather than report a chord to a face that removes nothing.
	{
		struct Row { const char* name; bool answersUnsigned; const char* why; };
		const Row rows[] = {
			{ "floor",        true,  "infinite plane" },
			{ "n_disk",       true,  "disk" },
			{ "n_quad",       true,  "coplanar CONVEX clipped plane" },
			{ "n_cyl_open",   true,  "OPEN cylinder (a tube encloses nothing)" },
			{ "n_quad_skew",  false, "non-coplanar clipped plane" },
			{ "n_quad_dart",  false, "coplanar NON-CONVEX clipped plane" },
			{ "n_patch",      false, "Bezier patch" },
			{ "n_rawmesh",    false, "RAW mesh" },
			{ "n_sdf_heightfield", false, "heightfield SDF" },
		};
		for( std::size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
			const IObjectPriv* const o = f.Obj( rows[i].name );
			Check( o != 0, std::string( "(h) fixture present: " ) + rows[i].name );
			if( !o ) continue;

			Scalar fv = Scalar( 12345 ); bool ex = true;
			Check( !SignedAt( o, Point3( 0, 0, 0 ), Scalar( 1000 ), fv, ex ),
				std::string( "(h) MONEY -- " ) + rows[i].why + " REFUSES the signed query" );
			Check( !ex, std::string( "(h) ...and the refusal CLEARS the exactness flag (" )
				+ rows[i].name + ")" );

			// ONE point for all nine rows, at an UNBOUNDED radius: what is
			// under test is the FAMILY's answer/refusal, not a distance, so
			// the station only has to be somewhere the query is well posed
			// -- and an unbounded radius makes every answering family
			// answer from anywhere.
			Scalar d = Scalar( 0 );
			const bool ans = o->DistanceToSurface( Point3( 0, 0, 0 ), RISE_INFINITY, d );
			Check( ans == rows[i].answersUnsigned,
				std::string( "(h) ...and its UNSIGNED answer is unchanged (" ) + rows[i].name + ")" );
		}
	}

	// --- THE ANISOTROPIC TRANSFORM, both directions at once.  A unit
	// sphere under `scale (3, 1, 0.4)` is an ellipsoid with those
	// semi-axes; the design names this exact transform because it is where
	// the two conversions visibly diverge -- the unsigned answer goes out
	// x sigmaMax and must land ABOVE the truth, the signed one goes out
	// x sigmaMin and must land BELOW it.
	{
		SphereGeometry* g = new SphereGeometry( Scalar( 1 ) );
		Object* aniso = new Object( g );
		g->release();
		aniso->SetStretch( Vector3( Scalar( 3 ), Scalar( 1 ), Scalar( 0.4 ) ) );
		aniso->FinalizeTransformations();

		// The station is on the y axis, where the distance to the
		// ellipsoid has a CLOSED FORM: minimising 9u^2 + (v-h)^2 +
		// 0.16w^2 on u^2+v^2+w^2 = 1 puts the minimiser at (0,1,0) for
		// every h > 1, so the distance is h - 1 = 4 at h = 5.
		const Point3 station( 0, 5, 0 );
		const Scalar closedForm = Scalar( 4 );

		// ...and the brute force AGREES with it, which is what makes the
		// closed form a fact rather than an assertion.  A grid minimum is
		// an UPPER reference (it minimises over a subset of the surface),
		// so it may sit a hair above; 1201 x 2400 samples put it within
		// 1e-6 here.
		const Scalar brute = BruteForceDistanceToEllipsoidSurface(
			station, Scalar( 3 ), Scalar( 1 ), Scalar( 0.4 ), 1200, 2400 );
		CheckClose( brute, closedForm, Scalar( 1e-5 ),
			"(h) the brute-force distance to the (3,1,0.4) solid matches its closed form" );

		Scalar fv = 0; bool ex = false;
		Check( SignedAt( aniso, station, Scalar( 100 ), fv, ex ),
			"(h) the (3,1,0.4) sphere answers the signed query" );
		Check( fv > Scalar( 0 ), "(h) ...with an exact positive sign outside" );
		Check( fv <= brute + Scalar( 1e-9 ),
			"(h) MONEY -- the SIGNED lower bound lands BELOW the brute-force distance" );
		Check( !ex,
			"(h) MONEY -- and an ANISOTROPICALLY scaled SOLID reports exact = FALSE, so a "
			"composite reaching it can only take the strict arm" );

		Scalar d = Scalar( 0 );
		Check( aniso->DistanceToSurface( station, Scalar( 100 ), d ),
			"(h) ...the unsigned query answers too" );
		Check( d >= brute - Scalar( 1e-9 ),
			"(h) MONEY -- ...and the UNSIGNED answer lands ABOVE it: the two bounds bracket "
			"the truth from opposite sides" );
		std::cout << "    scale (3,1,0.4) at (0,5,0): brute force " << (double)brute
			<< ", signed lower bound " << (double)fv
			<< ", unsigned upper bound " << (double)d << std::endl;

		// TEETH on the exactness flag: the SAME geometry under a
		// SIMILARITY does carry it, so `false` above is the anisotropy and
		// not the family.
		SphereGeometry* g2 = new SphereGeometry( Scalar( 1 ) );
		Object* simil = new Object( g2 );
		g2->release();
		simil->SetStretch( Vector3( Scalar( 1.5 ), Scalar( 1.5 ), Scalar( 1.5 ) ) );
		simil->FinalizeTransformations();
		Scalar fs = 0; bool exs = false;
		Check( SignedAt( simil, Point3( 0, 5, 0 ), Scalar( 100 ), fs, exs ),
			"(h) the uniformly scaled sphere answers" );
		CheckClose( fs, Scalar( 3.5 ), Scalar( 1e-9 ), "(h) ...5 - 1.5 = 3.5, exactly" );
		Check( exs, "(h) MONEY -- teeth: under a SIMILARITY the same family DOES carry the flag" );

		// A DEGENERATE transform refuses the signed query too.
		SphereGeometry* g3 = new SphereGeometry( Scalar( 1 ) );
		Object* flat = new Object( g3 );
		g3->release();
		flat->SetStretch( Vector3( Scalar( 1 ), Scalar( 0 ), Scalar( 1 ) ) );
		flat->FinalizeTransformations();
		Scalar fd = 0; bool exd = true;
		Check( !SignedAt( flat, Point3( 0, 5, 0 ), Scalar( 100 ), fd, exd ),
			"(h) a DEGENERATE transform refuses the signed query as it refuses the unsigned one" );
		Check( !exd, "(h) ...clearing the flag" );

		flat->release();
		simil->release();
		aniso->release();
	}

	// --- A DEGENERATE OPERAND REFUSES.  A zero-radius sphere's closure is
	// a point: the composite renders nothing there, and admitting it to a
	// boundary arm would land on a set with no interior nearby.  The
	// unsigned query is deliberately NOT changed by this (it answers |p|,
	// which is still an upper bound), so the two queries disagree here as
	// they do about sheets.
	{
		SphereGeometry* g = new SphereGeometry( Scalar( 0 ) );
		Object* pointish = new Object( g );
		g->release();
		pointish->FinalizeTransformations();
		Scalar fv = 0; bool ex = true;
		Check( !SignedAt( pointish, Point3( 0, 5, 0 ), Scalar( 100 ), fv, ex ),
			"(h) MONEY -- a DEGENERATE (zero-radius) operand refuses the signed query" );
		Check( !ex, "(h) ...clearing the flag" );
		pointish->release();
	}
}


//======================================================================
// (i) EXACT SIGMA
//======================================================================

//! THE SIGMA PAIR, and the three branches that can fill it (design 5.6,
//! "Exact sigma").  Phase 1 shipped only two -- an exact fast path for
//! similarities and the loose Frobenius/determinant pair for everything
//! else -- and the loose pair inflated the object-space search radius
//! 8.47x on `scale (3, 1, 0.4)` against a true 2.5x.  Phase 3 puts a
//! one-sided Jacobi SVD between them.
//!
//! WHY THE ROUTINE IS CALLED DIRECTLY HERE.  `Loose` is now unreachable
//! from any real transform (a 3x3 Jacobi converges in a handful of sweeps
//! and the cap is thirty), so the only way to cover that branch is to hand
//! the routine a ZERO-sweep budget and read the state it RETURNS -- the
//! log line that names the state sits behind `Object::DistanceToSurface`
//! and cannot be reached without an object that also has a geometry.  That
//! same call doubles as the reference for "8.47x before": it computes the
//! very pair Phase 1 would have cached.
static void TestExactSigma()
{
	std::cout << "(i) exact sigma -- the fast path, Jacobi, and the loose fallback" << std::endl;

	// --- THE FAST PATH, three ways, each against a written reference.
	// A rotation, a reflection and a uniform scale all satisfy
	// `M^T M = s^2 I` exactly, so both bounds are `s` and the routine
	// never reaches Jacobi at all.
	{
		SphereGeometry* g = new SphereGeometry( Scalar( 1 ) );
		Object* rot = new Object( g );
		g->release();
		rot->SetOrientation( Vector3( Scalar( 37 ), Scalar( 21 ), Scalar( 53 ) ) );
		rot->FinalizeTransformations();
		CheckClose( rot->SigmaMax(), Scalar( 1 ), Scalar( 1e-12 ), "(i) rotation: sigmaMax == 1" );
		CheckClose( rot->SigmaMin(), Scalar( 1 ), Scalar( 1e-12 ), "(i) rotation: sigmaMin == 1" );
		rot->release();

		SphereGeometry* g2 = new SphereGeometry( Scalar( 1 ) );
		Object* refl = new Object( g2 );
		g2->release();
		refl->SetStretch( Vector3( Scalar( -1 ), Scalar( 1 ), Scalar( 1 ) ) );
		refl->FinalizeTransformations();
		CheckClose( refl->SigmaMax(), Scalar( 1 ), Scalar( 1e-12 ),
			"(i) MONEY -- a REFLECTION needs nothing special: the singular values of M are "
			"those of |M|, so sigmaMax == 1" );
		CheckClose( refl->SigmaMin(), Scalar( 1 ), Scalar( 1e-12 ), "(i) reflection: sigmaMin == 1" );
		refl->release();

		SphereGeometry* g3 = new SphereGeometry( Scalar( 1 ) );
		Object* uni = new Object( g3 );
		g3->release();
		uni->SetStretch( Vector3( Scalar( 1.5 ), Scalar( 1.5 ), Scalar( 1.5 ) ) );
		uni->FinalizeTransformations();
		CheckClose( uni->SigmaMax(), Scalar( 1.5 ), Scalar( 1e-12 ), "(i) uniform scale: sigmaMax == 1.5" );
		CheckClose( uni->SigmaMin(), Scalar( 1.5 ), Scalar( 1e-12 ), "(i) uniform scale: sigmaMin == 1.5" );
		uni->release();
	}

	// --- JACOBI, on `scale (3, 1, 0.4)`.  The columns are already
	// orthogonal, so the first sweep rotates nothing and converges
	// immediately on the column norms themselves -- the true singular
	// values, 3 and 0.4.  Phase 1's pair on this same transform was
	// `||M||_F` = 3.187 and `|det|/||M||_F^2` = 0.1181.
	{
		SphereGeometry* g = new SphereGeometry( Scalar( 1 ) );
		Object* aniso = new Object( g );
		g->release();
		aniso->SetStretch( Vector3( Scalar( 3 ), Scalar( 1 ), Scalar( 0.4 ) ) );
		aniso->FinalizeTransformations();

		CheckClose( aniso->SigmaMax(), Scalar( 3 ), Scalar( 1e-9 ),
			"(i) MONEY -- scale (3,1,0.4): sigmaMax is the TRUE 3, not ||M||_F = 3.187" );
		CheckClose( aniso->SigmaMin(), Scalar( 0.4 ), Scalar( 1e-9 ),
			"(i) MONEY -- ...and sigmaMin is the TRUE 0.4, not |det|/||M||_F^2 = 0.1181" );
		Check( aniso->SigmaMax() >= Scalar( 3 ),
			"(i) ...with the relative, condition-scaled widening upward on sigmaMax, so "
			"`d_w <= sigmaMax * d_o` survives rounding" );
		Check( aniso->SigmaMin() <= Scalar( 0.4 ),
			"(i) ...and downward on sigmaMin, so `d_w >= sigmaMin * d_o` does too" );

		// THE SEARCH-RADIUS INFLATION, which is the cost side and the
		// number 8's gate names.  The `Loose` reference beside it is
		// computed from the SAME matrix by the SAME routine with the sweep
		// cap at zero -- so "8.47x before" is derived here, not quoted.
		const Scalar inflationNow = Scalar( 1 ) / aniso->SigmaMin();

		Scalar looseMin = 0, looseMax = 0;
		SigmaSource looseSrc = SigmaSource::Exact;
		Matrix4 mAniso;
		mAniso._00 = Scalar( 3 ); mAniso._11 = Scalar( 1 ); mAniso._22 = Scalar( 0.4 );
		Check( ComputeSigmaExtremes( mAniso, 0, looseMin, looseMax, looseSrc ),
			"(i) the routine answers with a ZERO-sweep budget" );
		Check( looseSrc == SigmaSource::Loose,
			"(i) MONEY -- ...and RETURNS the Loose state, which is the only way to reach that "
			"branch: no real transform hits the thirty-sweep cap" );
		const Scalar inflationBefore = Scalar( 1 ) / looseMin;

		CheckClose( inflationNow, Scalar( 2.5 ), Scalar( 1e-9 ),
			"(i) MONEY -- the object-space search radius is inflated 1/sigmaMin = 2.5x" );
		CheckClose( inflationBefore, Scalar( 8.4666666666 ), Scalar( 1e-6 ),
			"(i) ...where the loose pair inflated it 8.47x, the number 5.6 quotes" );
		std::cout << "    scale (3,1,0.4): search-radius inflation " << (double)inflationBefore
			<< "x (loose) -> " << (double)inflationNow << "x (Jacobi);  sigma ratio "
			<< (double)( looseMax / looseMin ) << " -> "
			<< (double)( aniso->SigmaMax() / aniso->SigmaMin() ) << std::endl;

		// AND EXACTNESS IS STILL NOT CLAIMED.  `x sigmaMax` is attained
		// only along the top singular vector, so the unsigned answer at a
		// station off that vector is STILL above the truth -- Phase 3
		// removed the pair's slack, not the anisotropy.
		const Point3 station( 0, 5, 0 );
		const Scalar trueDist = Scalar( 4 );			// closed form, see section (h)
		Scalar d = 0;
		Check( aniso->DistanceToSurface( station, Scalar( 100 ), d ),
			"(i) the anisotropic object answers the unsigned query" );
		Check( d >= trueDist - Scalar( 1e-9 ),
			"(i) MONEY -- the unsigned answer is STILL an upper bound after the tightening" );
		Scalar fv = 0; bool ex = false;
		Check( aniso->SignedDistanceLower( station, Scalar( 100 ), fv, ex ),
			"(i) ...and the signed query answers" );
		Check( fv <= trueDist + Scalar( 1e-9 ),
			"(i) ...still a lower bound" );
		Check( !ex, "(i) ...and still not exact: Jacobi is not a similarity" );
		std::cout << "    scale (3,1,0.4) at (0,5,0): true 4, unsigned " << (double)d
			<< " (over-report " << (double)( d / trueDist ) << "x), signed lower bound "
			<< (double)fv << std::endl;

		aniso->release();
	}

	// --- JACOBI, on a SHEAR, which is where the columns are genuinely
	// non-orthogonal and the sweeps do real work.  `M` maps
	// e0 -> (1,0,0), e1 -> (1,1,0), e2 -> (0,0,1); `M^T M`'s upper 2x2 is
	// [[1,1],[1,2]] with eigenvalues (3 +- sqrt 5)/2, so the singular
	// values are the golden ratio and its reciprocal -- a written
	// reference, computed by hand rather than recorded from a run.
	{
		const Scalar phi    = (Scalar)( ( 1.0 + std::sqrt( 5.0 ) ) / 2.0 );
		const Scalar invPhi = (Scalar)( ( std::sqrt( 5.0 ) - 1.0 ) / 2.0 );

		const Matrix4 shear(
			Scalar(1), Scalar(0), Scalar(0), Scalar(0),
			Scalar(1), Scalar(1), Scalar(0), Scalar(0),
			Scalar(0), Scalar(0), Scalar(1), Scalar(0),
			Scalar(0), Scalar(0), Scalar(0), Scalar(1) );

		SphereGeometry* g = new SphereGeometry( Scalar( 1 ) );
		Object* sh = new Object( g );
		g->release();
		sh->PushTopTransStack( shear );
		sh->FinalizeTransformations();

		CheckClose( sh->SigmaMax(), phi, Scalar( 1e-9 ),
			"(i) MONEY -- a SHEAR converges to the golden ratio 1.6180339887" );
		CheckClose( sh->SigmaMin(), invPhi, Scalar( 1e-9 ),
			"(i) MONEY -- ...and to its reciprocal 0.6180339887" );
		Check( sh->SigmaMax() >= phi && sh->SigmaMin() <= invPhi,
			"(i) ...widened OUTWARD by the relative, condition-scaled factor, never inward" );

		// TEETH: the loose pair on the SAME shear is visibly worse, so the
		// two checks above are measuring Jacobi and not an accident of the
		// fallback landing on the right answer.
		Scalar lmin = 0, lmax = 0;
		SigmaSource src = SigmaSource::Exact;
		Check( ComputeSigmaExtremes( shear, 0, lmin, lmax, src ) && src == SigmaSource::Loose,
			"(i) the same shear at zero sweeps falls back" );
		Check( lmax > phi + Scalar( 0.1 ) && lmin < invPhi - Scalar( 0.1 ),
			"(i) ...to a pair that is visibly wider on both ends" );
		std::cout << "    shear: Jacobi " << (double)sh->SigmaMin() << " / " << (double)sh->SigmaMax()
			<< "  vs loose " << (double)lmin << " / " << (double)lmax << std::endl;

		sh->release();
	}

	// --- THE DEGENERATE REFUSAL RUNS BEFORE JACOBI, which is what makes
	// `sigmaMin > 0` true after the downward nudge.
	{
		Matrix4 flat;
		flat._11 = Scalar( 0 );			// det == 0
		Scalar mn = Scalar( 1 ), mx = Scalar( 1 );
		SigmaSource src = SigmaSource::Exact;
		Check( !ComputeSigmaExtremes( flat, 30, mn, mx, src ),
			"(i) MONEY -- a DEGENERATE linear part is refused, and before Jacobi runs" );
		CheckClose( mn, Scalar( 0 ), Scalar( 0 ), "(i) ...with the pair zeroed (min)" );
		CheckClose( mx, Scalar( 0 ), Scalar( 0 ), "(i) ...with the pair zeroed (max)" );
	}
}


//======================================================================
// (j) CSG COMPOSITES
//======================================================================

//! One operand, positioned, finalized.  `Object`'s constructor addrefs the
//! geometry, so the constructing reference is ours to drop.
static Object* MakeOperand( IGeometry* g, const Point3& pos )
{
	Object* o = new Object( g );
	g->release();
	o->SetPosition( pos );
	o->FinalizeTransformations();
	return o;
}

//! One composite.  `AssignObjects` addrefs both operands, so the caller's
//! own references are dropped here and the composite owns them.
static CSGObject* MakeCsg( const CSG_OP op, Object* a, Object* b,
	const Point3& pos, const Vector3& orient )
{
	CSGObject* c = new CSGObject( op );
	c->AssignObjects( a, b );
	a->release();
	b->release();
	c->SetPosition( pos );
	c->SetOrientation( orient );
	c->FinalizeTransformations();
	return c;
}

//! A two-triangle INDEXED mesh -- a sheet that ANSWERS the unsigned query
//! and REFUSES the signed one, which is the pairing a union has to accept
//! and an intersection has to refuse on.
static TriangleMeshGeometryIndexed* BuildTinyMesh()
{
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertex( Point3( -1, 0, -1 ) );
	mesh->AddVertex( Point3(  1, 0, -1 ) );
	mesh->AddVertex( Point3(  1, 0,  1 ) );
	mesh->AddVertex( Point3( -1, 0,  1 ) );
	mesh->AddNormal( Vector3( 0, 1, 0 ) );
	mesh->AddTexCoord( Point2( 0, 0 ) );
	IndexedTriangle t;
	t.iNormals[0] = t.iNormals[1] = t.iNormals[2] = 0;
	t.iCoords[0]  = t.iCoords[1]  = t.iCoords[2]  = 0;
	t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2;
	mesh->AddIndexedTriangle( t );
	t.iVertices[0] = 0; t.iVertices[1] = 2; t.iVertices[2] = 3;
	mesh->AddIndexedTriangle( t );
	mesh->DoneIndexedTriangles();
	mesh->Realize();
	return mesh;
}

//! GRID SEARCH over a composite's SOLID, by STRICT operand membership on
//! the operands' EXACT signed distances -- deliberately NOT `f <= 0`,
//! which is `closure(A) n complement(int B)` and contains the PHANTOM
//! touching set where the two boundaries merely graze.  A minimum over
//! grid points is an UPPER reference on the true distance (it minimises
//! over a subset of the solid), which is why the gap it measures is
//! reported rather than asserted at a tolerance somebody chose.
static Scalar GridSearchDistanceToComposite(
	const IObjectPriv* a, const IObjectPriv* b, const CSG_OP op,
	const Point3& p, const Point3& lo, const Point3& hi, const int steps )
{
	Scalar best = RISE_INFINITY;
	const Scalar sx = ( hi.x - lo.x ) / Scalar( steps );
	const Scalar sy = ( hi.y - lo.y ) / Scalar( steps );
	const Scalar sz = ( hi.z - lo.z ) / Scalar( steps );
	for( int i = 0; i <= steps; ++i ) {
		for( int j = 0; j <= steps; ++j ) {
			for( int k = 0; k <= steps; ++k ) {
				const Point3 q( lo.x + sx*Scalar(i), lo.y + sy*Scalar(j), lo.z + sz*Scalar(k) );
				Scalar fa = 0, fb = 0; bool ea = false, eb = false;
				if( !a->SignedDistanceLower( q, RISE_INFINITY, fa, ea ) ) continue;
				if( !b->SignedDistanceLower( q, RISE_INFINITY, fb, eb ) ) continue;
				const bool inside = ( op == CSG_INTERSECTION )
					? ( fa < Scalar( 0 ) && fb < Scalar( 0 ) )
					: ( fa < Scalar( 0 ) && fb > Scalar( 0 ) );
				if( !inside ) continue;
				const Scalar d = Vector3Ops::Magnitude( Vector3Ops::mkVector3( q, p ) );
				if( d < best ) best = d;
			}
		}
	}
	return best;
}

static const char* ArmName( const CsgLandingArm a )
{
	switch( a ) {
	case CsgLandingArm::Strict:   return "STRICT";
	case CsgLandingArm::Boundary: return "BOUNDARY";
	default:                      return "none";
	}
}

//! CSG COMPOSITES (design 5.6).  Until Phase 3 a composite refused both
//! queries, so its own surface was invisible to every neighbour.  A UNION
//! can answer `min`; an INTERSECTION and a SUBTRACTION cannot -- the
//! nearest operand-surface point may not be on the composite's surface at
//! all -- so they compose the operands' SIGNED LOWER BOUNDS into a field
//! and bracket it, admitting a landing only when the operands' OWN SIGNS
//! prove it lies in the closure of the real solid.
static void TestCsgComposites()
{
	std::cout << "(j) CSG composites -- union min, the bracket, and the two landing arms" << std::endl;

	// --- A UNION OF TWO SPHERES answers `min` within 1e-9.  Both operands
	// are exact, so the union's answer is the true distance: every
	// union-boundary point lies on one operand's boundary.
	{
		Object* a = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, 0, 0 ) );
		Object* b = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 3, 0, 0 ) );
		CSGObject* u = MakeCsg( CSG_UNION, a, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		Scalar d = 0;
		Check( u->DistanceToSurface( Point3( 0, 5, 0 ), Scalar( 100 ), d ),
			"(j) a union of two spheres ANSWERS (it refused before Phase 3)" );
		CheckClose( d, Scalar( 4 ), Scalar( 1e-9 ),
			"(j) MONEY -- union: min(4, sqrt(9+25)-1 = 4.831) = 4" );

		// From the other side the OTHER operand wins, so `min` is really a
		// min and not operand A leaking through.
		Check( u->DistanceToSurface( Point3( 3, 5, 0 ), Scalar( 100 ), d ),
			"(j) ...and answers from above operand B" );
		CheckClose( d, Scalar( 4 ), Scalar( 1e-9 ), "(j) ...with B's 4 this time" );

		// EXACTNESS EXPORT: NO COMPOSITE CARRIES THE FLAG, a union of two
		// exact operands included.  §5.6 allows one to; that rule is
		// UNSOUND and the fixture two blocks down is the counterexample.
		Scalar fv = 0; bool ex = true;
		Check( u->SignedDistanceLower( Point3( 0, 5, 0 ), Scalar( 100 ), fv, ex ),
			"(j) the union answers the SIGNED query" );
		CheckClose( fv, Scalar( 4 ), Scalar( 1e-9 ), "(j) ...with min(f_A, f_B) = 4" );
		Check( !ex, "(j) MONEY -- a UNION does NOT export exactness, even over two exact "
			"operands: `min(f_A, f_B)` is 0 on its INTERIOR seams wherever the operands abut" );

		// ...and INSIDE the union it is a depth.
		Check( u->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 100 ), fv, ex ),
			"(j) the union answers inside" );
		Check( fv < Scalar( 0 ), "(j) ...negatively, the sign being exact" );

		u->release();
	}

	// --- A UNION WITH A MESH OPERAND ANSWERS.  A mesh is a SHEET: it has
	// an unsigned distance and no inside, so it can never say whether a
	// point is within it.  A union takes it anyway -- `d <= d_A <= u_A`
	// holds whatever B does -- and refuses only if BOTH operands refuse.
	{
		TriangleMeshGeometryIndexed* mg = BuildTinyMesh();
		Object* a = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, 3, 0 ) );
		Object* b = MakeOperand( mg, Point3( 0, 0, 0 ) );
		CSGObject* u = MakeCsg( CSG_UNION, a, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		Scalar d = 0;
		Check( u->DistanceToSurface( Point3( 0, 6, 0 ), Scalar( 100 ), d ),
			"(j) MONEY -- a union with a MESH operand ANSWERS (no refusal)" );
		CheckClose( d, Scalar( 2 ), Scalar( 1e-9 ),
			"(j) ...with the sphere's 6 - 3 - 1 = 2, the nearer of the two" );

		Check( u->DistanceToSurface( Point3( 0, 0.5, 0 ), Scalar( 100 ), d ),
			"(j) ...and answers where the MESH is the nearer operand" );
		CheckClose( d, Scalar( 0.5 ), Scalar( 1e-9 ), "(j) ...with the mesh's 0.5" );

		// But the SIGNED query needs BOTH, and the mesh refuses it -- so
		// this union cannot be an operand of an intersection, and a point
		// inside a mesh operand reads a positive distance rather than
		// contact.  The under-paint direction, disclosed.
		Scalar fv = 0; bool ex = false;
		Check( !u->SignedDistanceLower( Point3( 0, 6, 0 ), Scalar( 100 ), fv, ex ),
			"(j) MONEY -- ...but the SIGNED query REFUSES with a sheet operand" );
		Check( !ex, "(j) ...clearing the flag" );

		u->release();
	}

	// --- THE EXACT STATION: a CYLINDER minus a tangent SLAB, queried
	// radially.  This is `glass_pavilion`'s fluted column shape.  The
	// radial landing IS the nearest point by symmetry, both operands are
	// exact closed forms, and the arithmetic is exact by Sterbenz:
	// `f_A = 0.26 (-) 0.25` is exact, so `0.26 (-) f_A` is 0.25 exactly
	// and `sqrt(x*x) = x` makes `f_A` a TRUE zero at the landing.  So the
	// boundary arm fires with gap 0 and the answer is the CLOSED FORM --
	// not a grid reference, which would be `d + O(spacing)`.
	{
		Object* col  = MakeOperand( new CylinderGeometry( 'y', Scalar( 0.25 ), Scalar( 5 ), true ),
			Point3( 0, 2.5, 0 ) );
		Object* slot = MakeOperand( new BoxGeometry( Scalar( 0.08 ), Scalar( 5.2 ), Scalar( 0.5 ) ),
			Point3( 0, 2.5, 0 ) );
		CSGObject* c = MakeCsg( CSG_SUBTRACTION, col, slot, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		Scalar d = 0;
		CsgLandingArm arm = CsgLandingArm::None;
		Check( c->DistanceToSurfaceWithArm( Point3( 0.26, 1, 0 ), Scalar( 1 ), d, arm ),
			"(j) cylinder-minus-slab answers at the radial station" );
		CheckClose( d, Scalar( 0.01 ), Scalar( 1e-12 ),
			"(j) MONEY -- reported == the CLOSED FORM 0.26 - 0.25 to 1e-12" );
		Check( arm == CsgLandingArm::Boundary,
			std::string( "(j) MONEY -- ...and the BOUNDARY arm fired (gap 0), not the probe: " )
			+ ArmName( arm ) );

		// THE PHANTOM SWEEP.  The slot is 0.5 deep -- the column's
		// diameter -- so its +-z faces are TANGENT to the cylinder at
		// z = +-0.25.  From a station 1 cm outside that face the radial
		// descent lands ON the tangency, where an `f <= 0` test would
		// report a 1.00 cm chord while the nearest REAL surface is the
		// slot-wall/cylinder corner at 4.21 cm -- an OVER-read of contact
		// by 3 cm, the forbidden direction.  Every station in the band
		// where a tolerant arm admitted a quarter of the landings must
		// REFUSE instead.
		const Scalar trueCorner = (Scalar)std::sqrt(
			0.04*0.04 + ( 0.26 - std::sqrt( 0.25*0.25 - 0.04*0.04 ) )
			          * ( 0.26 - std::sqrt( 0.25*0.25 - 0.04*0.04 ) ) );
		int answered = 0;
		const int nSweep = 60;
		for( int i = 0; i < nSweep; ++i ) {
			const Scalar x = Scalar( 1e-7 ) + ( Scalar( 3e-6 ) - Scalar( 1e-7 ) )
				* Scalar( i ) / Scalar( nSweep - 1 );
			Scalar ds = 0;
			CsgLandingArm a2 = CsgLandingArm::None;
			if( c->DistanceToSurfaceWithArm( Point3( x, 1, 0.26 ), Scalar( 0.1 ), ds, a2 ) ) {
				++answered;
				std::cout << "    phantom station x = " << (double)x << " ANSWERED "
					<< (double)ds << " (" << ArmName( a2 ) << ")" << std::endl;
			}
		}
		Check( answered == 0,
			"(j) MONEY -- all 60 stations of the phantom sweep x in [1e-7, 3e-6] at local "
			"(x, 1, 0.26) REFUSE; none reports the 1.00 cm tangency chord" );

		// THE POSITIVE CONTROL, without which "the guard refuses the
		// phantom" is indistinguishable from "this composite refuses
		// everything near that face".  Local (0.06, 1, 0.26) is past the
		// slot's own half-width of 0.04, so the radial descent lands on a
		// piece of cylinder wall the slot does NOT remove -- a real
		// surface -- and the query must ANSWER there, at the closed form
		// sqrt(0.06^2 + 0.26^2) - 0.25.
		{
			const Scalar rhoCtl = (Scalar)std::sqrt( 0.06*0.06 + 0.26*0.26 );
			const Scalar dCtl   = rhoCtl - Scalar( 0.25 );
			Scalar dGot = 0;
			CsgLandingArm aCtl = CsgLandingArm::None;
			Check( c->DistanceToSurfaceWithArm( Point3( 0.06, 1, 0.26 ), Scalar( 0.1 ), dGot, aCtl ),
				"(j) MONEY -- POSITIVE CONTROL: past the slot's half-width in x, the SAME "
				"composite at the SAME radius ANSWERS -- so the 60 refusals above are the "
				"phantom guard, not a composite that has gone quiet near that face" );
			Check( dGot >= dCtl - Scalar( 1e-12 ),
				"(j) ...at or above its closed form " + std::to_string( (double)dCtl ) );
			std::cout << "    positive control at local (0.06, 1, 0.26): closed form "
				<< (double)dCtl << ", reported " << (double)dGot
				<< ", arm " << ArmName( aCtl ) << std::endl;
		}
		std::cout << "    the phantom sweep's true corner distance is "
			<< (double)trueCorner << " (4.21 cm), which the refusal under-paints" << std::endl;
		Check( trueCorner > Scalar( 0.04 ),
			"(j) ...and that corner really is far past the 1 cm the phantom would have reported" );

		// AN OBLIQUE STATION on the same fixture: off the axis, so the
		// landing residual is whatever the central-difference gradient
		// leaves, and the reached operand (the cylinder wall) carries the
		// composite's nearest point.  `d` is the closed form there.
		{
			const Point3 st( 0.20, 1, 0.20 );
			const Scalar rho = (Scalar)std::sqrt( 0.20*0.20 + 0.20*0.20 );
			const Scalar dTrue = rho - Scalar( 0.25 );
			Scalar dr = 0;
			CsgLandingArm a3 = CsgLandingArm::None;
			Check( c->DistanceToSurfaceWithArm( st, Scalar( 1 ), dr, a3 ),
				"(j) the oblique station answers" );
			Check( dr >= dTrue - Scalar( 1e-12 ),
				"(j) MONEY -- reported >= d at the oblique station: the invariant a tolerance "
				"on the landing test would have broken" );
			const Scalar epsLocal = Scalar( 5e-5 ) * (Scalar)std::sqrt( 0.5*0.5 + 5.0*5.0 + 0.5*0.5 );
			Check( dr <= dTrue + epsLocal,
				"(j) ...and within one probe step (eps) of it" );
			std::cout << "    oblique station (0.20, 1, 0.20): closed form " << (double)dTrue
				<< ", reported " << (double)dr << ", gap " << (double)( dr - dTrue )
				<< ", arm " << ArmName( a3 ) << ", eps " << (double)epsLocal << std::endl;
		}

		// A POINT INSIDE THE COMPOSITE READS 0.
		Check( c->DistanceToSurface( Point3( 0.15, 1, 0 ), Scalar( 1 ), d ),
			"(j) a point inside the composite answers" );
		CheckClose( d, Scalar( 0 ), Scalar( 0 ),
			"(j) MONEY -- ...reading exactly 0: interpenetration IS contact" );

		// ...AND A POINT INSIDE THE SLOT DOES NOT.  It is inside operand
		// A and inside B, so it is OUTSIDE the subtraction.
		Scalar fv = 0; bool ex = false;
		Check( c->SignedDistanceLower( Point3( 0, 1, 0.20 ), Scalar( 1 ), fv, ex ),
			"(j) the composite answers the signed query inside the slot" );
		Check( fv > Scalar( 0 ),
			"(j) MONEY -- a point inside the SUBTRAHEND is OUTSIDE the composite (positive)" );
		Check( !ex,
			"(j) MONEY -- a SUBTRACTION never exports exactness: max(a, -b) under-reads near a "
			"seam and its zero set IS the phantom touching set" );

		c->release();
	}

	// --- AN OBLIQUE STATION WITH A TORUS OPERAND, where the field is
	// curved and the single descent step's residual is genuinely nonzero
	// rather than exact by Sterbenz.
	{
		Object* t = MakeOperand( new TorusGeometry( Scalar( 1.5 ), Scalar( 0.5 ) ), Point3( 0, 0, 0 ) );
		Object* b = MakeOperand( new BoxGeometry( Scalar( 0.4 ), Scalar( 0.4 ), Scalar( 0.4 ) ),
			Point3( 1.5, 0, 0 ) );
		CSGObject* c = MakeCsg( CSG_SUBTRACTION, t, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		const Point3 st( -1.0, 0.8, -1.0 );
		const Scalar rho = (Scalar)std::sqrt( 2.0 ) - Scalar( 1.5 );
		const Scalar q   = (Scalar)std::sqrt( (double)( rho*rho ) + 0.8*0.8 );
		const Scalar dTrue = q - Scalar( 0.5 );

		Scalar d = 0;
		CsgLandingArm arm = CsgLandingArm::None;
		Check( c->DistanceToSurfaceWithArm( st, Scalar( 5 ), d, arm ),
			"(j) the torus-minus-box composite answers at an oblique station" );
		Check( d >= dTrue - Scalar( 1e-12 ),
			"(j) MONEY -- reported >= the closed form at a CURVED oblique station" );
		// The composite's own eps: A's box for a subtraction -- the torus,
		// 4 x 1 x 4 -- so eps is 5e-5 x its diagonal.  THE FIRST PROBE
		// STEP IS `max(eps, f)`, and the descent exits its loop as soon as
		// `f <= kBackoff * eps` with kBackoff = 2 -- so the residual `f`
		// handed to the probe can be up to 2 eps and the first step with
		// it.  The bound that actually holds is therefore 2 eps, not eps;
		// 8's Phase-3 gate says "one probe step, eps", which is the step
		// SIZE at the floor and not the bound.  Measured here at 1.21 eps.
		const Scalar epsLocal = Scalar( 5e-5 ) * (Scalar)std::sqrt( 4.0*4.0 + 1.0*1.0 + 4.0*4.0 );
		Check( d <= dTrue + Scalar( 2 ) * epsLocal,
			"(j) ...and within one probe step (bounded by kBackoff * eps = 2 eps) of it" );
		std::cout << "    torus oblique station: closed form " << (double)dTrue
			<< ", reported " << (double)d << ", gap " << (double)( d - dTrue )
			<< ", arm " << ArmName( arm ) << ", eps " << (double)epsLocal << std::endl;
		c->release();
	}

	// --- THE GRID REFERENCE, for an INTERSECTION and a SUBTRACTION.  The
	// reference is the composite's SOLID by STRICT operand membership on
	// the operands' EXACT signed distances -- never `f <= 0`.  A grid
	// minimum is an UPPER reference, so `gap = reported - reference` is
	// MEASURED and printed rather than asserted at a chosen tolerance.
	{
		// INTERSECTION: two unit spheres 1.2 apart -- a lens.
		Object* a = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, 0, 0 ) );
		Object* b = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 1.2, 0, 0 ) );
		Object* aRef = a; Object* bRef = b;
		aRef->addref(); bRef->addref();
		CSGObject* c = MakeCsg( CSG_INTERSECTION, a, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		// 120 steps over a 1.6 x 2.4 x 2.4 box: spacing 0.013333 in x and
		// 0.02 in y and z.  The `cellDiag` below uses the LARGEST spacing
		// on every axis (0.02 * sqrt 3 = 0.03464 against the true
		// 0.03127), which is the conservative direction for a slack that
		// is SUBTRACTED from the reference.
		const int steps = 120;
		const Point3 lo( -0.2, -1.2, -1.2 ), hi( 1.4, 1.2, 1.2 );
		const Point3 stations[4] = {
			Point3( 0.6, 2.0, 0 ), Point3( 0.6, 0, 2.0 ),
			Point3( -1.5, 0, 0 ),  Point3( 0.6, 1.5, 1.5 ) };

		// THE GRID CELL'S DIAGONAL is the whole slack on the reference:
		// it minimises over grid POINTS strictly inside the solid, so
		// `d <= ref <= d + cellDiagonal`.  That two-sided bracket is what
		// lets the LOWER side be asserted, and the lower side is the one
		// that matters -- `reported < d` is contact painted where there is
		// none, and `gap_max` alone (an upper bound on `reported - ref`)
		// says nothing about it.
		const Scalar cellDiag = Scalar( 0.02 ) * (Scalar)std::sqrt( 3.0 );
		Scalar gapMax = Scalar( -RISE_INFINITY );
		Scalar gapMin = Scalar( RISE_INFINITY );
		int answered = 0;
		for( int i = 0; i < 4; ++i ) {
			Scalar d = 0;
			CsgLandingArm arm = CsgLandingArm::None;
			if( !c->DistanceToSurfaceWithArm( stations[i], Scalar( 20 ), d, arm ) ) continue;
			++answered;
			const Scalar ref = GridSearchDistanceToComposite(
				aRef, bRef, CSG_INTERSECTION, stations[i], lo, hi, steps );
			Check( ref < RISE_INFINITY,
				"(j) the grid found the solid at all (an all-refusing operand would make every "
				"gap check below trivially true)" );
			Scalar lower = 0; bool ex = false;
			Check( c->SignedDistanceLower( stations[i], Scalar( 20 ), lower, ex ),
				"(j) the intersection answers its own signed lower bound" );
			Check( lower <= d + Scalar( 1e-9 ),
				"(j) MONEY -- lower <= reported at every answering intersection station" );
			const Scalar gap = d - ref;
			if( gap > gapMax ) gapMax = gap;
			if( gap < gapMin ) gapMin = gap;
			Check( gap >= -cellDiag - Scalar( 1e-9 ),
				"(j) MONEY -- reported >= ref - cellDiagonal at this intersection station, i.e. NEVER "
				"BELOW the true distance. This is the never-over-read invariant checked against "
				"an INDEPENDENT reference; `lower <= reported` beside it compares the code "
				"against itself" );
			std::cout << "    intersection station " << i << ": lower " << (double)lower
				<< ", grid ref " << (double)ref << ", reported " << (double)d
				<< ", gap " << (double)gap << ", arm " << ArmName( arm ) << std::endl;
		}
		Check( answered > 0, "(j) the intersection answered at least one station" );
		Check( gapMax <= Scalar( 0.05 ),
			"(j) MONEY -- intersection gap_max MEASURED (grid 121^3 samples, spacing 0.0133 in x and "
			"0.02 in y/z), asserted <= 0.05" );
		Check( gapMin >= -cellDiag - Scalar( 1e-9 ),
			"(j) MONEY -- ...and gap_MIN is bounded below by one grid-cell diagonal, which is "
			"the side an over-read would break" );
		std::cout << "    intersection gap_max = " << (double)gapMax << ", gap_min = " << (double)gapMin
			<< " over " << answered << " stations (grid spacing 0.0133 in x, 0.02 in y/z; "
			   "cell-diagonal slack " << (double)cellDiag << ")" << std::endl;

		aRef->release(); bRef->release();
		c->release();
	}
	{
		// SUBTRACTION: a 2x2x2 box with a corner bitten out by a sphere.
		Object* a = MakeOperand( new BoxGeometry( Scalar( 2 ), Scalar( 2 ), Scalar( 2 ) ), Point3( 0, 0, 0 ) );
		Object* b = MakeOperand( new SphereGeometry( Scalar( 0.6 ) ), Point3( 1, 1, 1 ) );
		Object* aRef = a; Object* bRef = b;
		aRef->addref(); bRef->addref();
		CSGObject* c = MakeCsg( CSG_SUBTRACTION, a, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		const int steps = 110;			// spacing 0.02 over a 2.2-unit box
		const Point3 lo( -1.1, -1.1, -1.1 ), hi( 1.1, 1.1, 1.1 );
		const Point3 stations[4] = {
			Point3( -1.5, 0, 0 ), Point3( 0, -1.3, 0 ),
			Point3( 0, 0, -1.7 ), Point3( -1.2, 0.5, 0.5 ) };

		// THE GRID CELL'S DIAGONAL is the whole slack on the reference:
		// it minimises over grid POINTS strictly inside the solid, so
		// `d <= ref <= d + cellDiagonal`.  That two-sided bracket is what
		// lets the LOWER side be asserted, and the lower side is the one
		// that matters -- `reported < d` is contact painted where there is
		// none, and `gap_max` alone (an upper bound on `reported - ref`)
		// says nothing about it.
		const Scalar cellDiag = Scalar( 0.02 ) * (Scalar)std::sqrt( 3.0 );
		Scalar gapMax = Scalar( -RISE_INFINITY );
		Scalar gapMin = Scalar( RISE_INFINITY );
		int answered = 0;
		for( int i = 0; i < 4; ++i ) {
			Scalar d = 0;
			CsgLandingArm arm = CsgLandingArm::None;
			if( !c->DistanceToSurfaceWithArm( stations[i], Scalar( 20 ), d, arm ) ) continue;
			++answered;
			const Scalar ref = GridSearchDistanceToComposite(
				aRef, bRef, CSG_SUBTRACTION, stations[i], lo, hi, steps );
			Check( ref < RISE_INFINITY,
				"(j) the grid found the solid at all (an all-refusing operand would make every "
				"gap check below trivially true)" );
			Scalar lower = 0; bool ex = false;
			Check( c->SignedDistanceLower( stations[i], Scalar( 20 ), lower, ex ),
				"(j) the subtraction answers its own signed lower bound" );
			Check( lower <= d + Scalar( 1e-9 ),
				"(j) MONEY -- lower <= reported at every answering subtraction station" );
			const Scalar gap = d - ref;
			if( gap > gapMax ) gapMax = gap;
			if( gap < gapMin ) gapMin = gap;
			Check( gap >= -cellDiag - Scalar( 1e-9 ),
				"(j) MONEY -- reported >= ref - cellDiagonal at this subtraction station, i.e. NEVER "
				"BELOW the true distance. This is the never-over-read invariant checked against "
				"an INDEPENDENT reference; `lower <= reported` beside it compares the code "
				"against itself" );
			std::cout << "    subtraction station " << i << ": lower " << (double)lower
				<< ", grid ref " << (double)ref << ", reported " << (double)d
				<< ", gap " << (double)gap << ", arm " << ArmName( arm ) << std::endl;
		}
		Check( answered > 0, "(j) the subtraction answered at least one station" );
		Check( gapMax <= Scalar( 0.05 ),
			"(j) MONEY -- subtraction gap_max MEASURED (grid spacing 0.02, 111^3 samples), "
			"asserted <= 0.05" );
		Check( gapMin >= -cellDiag - Scalar( 1e-9 ),
			"(j) MONEY -- ...and gap_MIN is bounded below by one grid-cell diagonal, which is "
			"the side an over-read would break" );
		std::cout << "    subtraction gap_max = " << (double)gapMax << ", gap_min = " << (double)gapMin
			<< " over " << answered << " stations (grid spacing 0.02, cell diagonal "
			<< (double)cellDiag << ")" << std::endl;

		aRef->release(); bRef->release();
		c->release();
	}

	// --- A SHEET OPERAND makes an intersection or a subtraction REFUSE.
	// A sheet REFUSES the signed query outright -- not merely "lacks the
	// exactness flag", which a BOUND operand (an ellipsoid, an SDF) also
	// does without forcing a refusal.  A subtracted PLANE admitted to the
	// boundary arm would report the chord to a face the subtraction
	// removes nothing at: the phantom in a new form.
	{
		Object* a = MakeOperand( new BoxGeometry( Scalar( 2 ), Scalar( 2 ), Scalar( 2 ) ), Point3( 0, 0, 0 ) );
		Object* pl = MakeOperand( new InfinitePlaneGeometry( Scalar( 1 ), Scalar( 1 ) ), Point3( 0, 0, 0 ) );
		CSGObject* c = MakeCsg( CSG_SUBTRACTION, a, pl, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );
		Scalar d = 0;
		Check( !c->DistanceToSurface( Point3( 0, 5, 0 ), Scalar( 100 ), d ),
			"(j) MONEY -- a SUBTRACTED PLANE operand makes the composite REFUSE" );
		Scalar fv = 0; bool ex = true;
		Check( !c->SignedDistanceLower( Point3( 0, 5, 0 ), Scalar( 100 ), fv, ex ),
			"(j) ...and so does its signed query" );
		Check( !ex, "(j) ...clearing the flag" );
		c->release();

		// TEETH: the same box against a SOLID subtrahend answers, so the
		// refusal above is the sheet and not the fixture.
		Object* a2 = MakeOperand( new BoxGeometry( Scalar( 2 ), Scalar( 2 ), Scalar( 2 ) ), Point3( 0, 0, 0 ) );
		Object* s2 = MakeOperand( new SphereGeometry( Scalar( 0.6 ) ), Point3( 1, 1, 1 ) );
		CSGObject* c2 = MakeCsg( CSG_SUBTRACTION, a2, s2, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );
		Check( c2->DistanceToSurface( Point3( 0, 5, 0 ), Scalar( 100 ), d ),
			"(j) ...teeth: the same box minus a SOLID answers" );
		CheckClose( d, Scalar( 4 ), Scalar( 1e-9 ), "(j) ...with 5 - 1 = 4" );
		c2->release();

		// An INTERSECTION with a mesh operand refuses for the same reason,
		// even though a UNION with the same mesh answers (above).
		TriangleMeshGeometryIndexed* mg = BuildTinyMesh();
		Object* a3 = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, 0, 0 ) );
		Object* m3 = MakeOperand( mg, Point3( 0, 0, 0 ) );
		CSGObject* c3 = MakeCsg( CSG_INTERSECTION, a3, m3, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );
		Check( !c3->DistanceToSurface( Point3( 0, 5, 0 ), Scalar( 100 ), d ),
			"(j) MONEY -- an INTERSECTION with a MESH operand refuses, where the UNION answered" );
		c3->release();
	}

	// --- A POINT INSIDE OPERAND A OF AN INTERSECTION, OUTSIDE B, reads a
	// POSITIVE distance.  It is outside the composite -- correct, not a
	// violation, and stated here so nobody "fixes" it.
	{
		Object* a = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, 0, 0 ) );
		Object* b = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 1.5, 0, 0 ) );
		CSGObject* c = MakeCsg( CSG_INTERSECTION, a, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		Scalar fv = 0; bool ex = false;
		Check( c->SignedDistanceLower( Point3( -0.5, 0, 0 ), Scalar( 10 ), fv, ex ),
			"(j) the intersection answers at a point inside A and outside B" );
		Check( fv > Scalar( 0 ),
			"(j) MONEY -- inside operand A but outside B is OUTSIDE the intersection (positive)" );
		Check( !ex,
			"(j) MONEY -- an INTERSECTION never exports exactness, even over two exact operands" );

		Scalar d = 0;
		CsgLandingArm arm = CsgLandingArm::None;
		Check( c->DistanceToSurfaceWithArm( Point3( -0.5, 0, 0 ), Scalar( 10 ), d, arm ),
			"(j) ...and the unsigned query answers there" );
		Check( d > Scalar( 0 ), "(j) ...with a POSITIVE distance, not 0" );
		CheckClose( d, Scalar( 1 ), Scalar( 1e-9 ),
			"(j) ...the closed form: the lens's near tip is at x = 0.5, one unit away" );

		// ...and INSIDE the lens it reads 0.
		Check( c->DistanceToSurface( Point3( 0.75, 0, 0 ), Scalar( 10 ), d ),
			"(j) the intersection answers inside the lens" );
		CheckClose( d, Scalar( 0 ), Scalar( 0 ), "(j) MONEY -- ...reading exactly 0" );

		c->release();
	}

	// --- AN ANISOTROPIC SOLID OPERAND forces the STRICT arm.  `x sigmaMin`
	// under a non-uniform transform is a bound, not the distance, so the
	// operand cannot carry the exactness flag and the boundary arm cannot
	// fire on it -- the probe has to walk until the landing is strictly
	// inside.  The answer is then above the truth, which is the safe
	// direction.
	{
		Object* a = MakeOperand( new BoxGeometry( Scalar( 2 ), Scalar( 2 ), Scalar( 2 ) ), Point3( 0, 0, 0 ) );
		a->SetStretch( Vector3( Scalar( 3 ), Scalar( 1 ), Scalar( 0.4 ) ) );
		a->FinalizeTransformations();
		Object* b = MakeOperand( new SphereGeometry( Scalar( 0.2 ) ), Point3( 10, 10, 10 ) );
		CSGObject* c = MakeCsg( CSG_SUBTRACTION, a, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		Scalar fv = 0; bool ex = true;
		Check( a->SignedDistanceLower( Point3( 0, 5, 0 ), Scalar( 100 ), fv, ex ),
			"(j) the (3,1,0.4) operand answers its signed query" );
		Check( !ex, "(j) ...reporting exact = FALSE, the similarity-only rule" );

		Scalar d = 0;
		CsgLandingArm arm = CsgLandingArm::None;
		Check( c->DistanceToSurfaceWithArm( Point3( 0, 5, 0 ), Scalar( 100 ), d, arm ),
			"(j) the composite over it answers" );
		Check( arm == CsgLandingArm::Strict,
			std::string( "(j) MONEY -- ...taking the STRICT arm, since a bound operand can never "
			"prove a boundary landing: " ) + ArmName( arm ) );
		Check( d >= Scalar( 4 ) - Scalar( 1e-9 ),
			"(j) ...and the answer is at or above the true 4" );
		std::cout << "    (3,1,0.4) operand: true 4, reported " << (double)d
			<< ", arm " << ArmName( arm ) << std::endl;

		c->release();
	}

	// --- A TRANSFORMED COMPOSITE.  Operands are NOT in world space:
	// `IntersectRay` maps the ray by the composite's own inverse and THEN
	// calls each operand, which applies its own on top.  So the composite
	// must implement its own transform layer, and the proof is that a
	// composite carrying `position`/`orientation` answers the same as the
	// one built with that transform FOLDED INTO the operands.
	//
	// OPERAND A IS AXISYMMETRIC about the rotation axis on purpose: folding
	// a rotation into a BOX operand grows its parent-frame AABB, which
	// moves eps and shifts a probe-stepped answer by ~eps -- five orders
	// above 1e-9.  The box-minus-box fixture below is compared at ~eps
	// instead, which is the honest tolerance for it.
	{
		const Point3  pos( 2.5, 2.5, 2.5 );
		const Vector3 orient( 0, 45, 0 );
		const Point3  station( 2.76, 3.5, 2.5 );		// 1 cm outside the wall, in world

		Object* ca = MakeOperand( new CylinderGeometry( 'y', Scalar( 0.25 ), Scalar( 5 ), true ),
			Point3( 0, 2.5, 0 ) );
		Object* cb = MakeOperand( new BoxGeometry( Scalar( 0.08 ), Scalar( 5.2 ), Scalar( 0.5 ) ),
			Point3( 0, 2.5, 0 ) );
		CSGObject* transformed = MakeCsg( CSG_SUBTRACTION, ca, cb, pos, orient );

		// The FOLDED twin: the composite's OWN world matrix pushed onto
		// each operand's transform stack (stack entries LEFT-multiply, so
		// this is exactly `outer * operand-local`), and the composite
		// itself left at the identity.
		Object* ga = MakeOperand( new CylinderGeometry( 'y', Scalar( 0.25 ), Scalar( 5 ), true ),
			Point3( 0, 2.5, 0 ) );
		Object* gb = MakeOperand( new BoxGeometry( Scalar( 0.08 ), Scalar( 5.2 ), Scalar( 0.5 ) ),
			Point3( 0, 2.5, 0 ) );
		const Matrix4 outerMx = transformed->GetFinalTransformMatrix();
		ga->PushTopTransStack( outerMx );  ga->FinalizeTransformations();
		gb->PushTopTransStack( outerMx );  gb->FinalizeTransformations();
		CSGObject* folded = MakeCsg( CSG_SUBTRACTION, ga, gb, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		Scalar dT = 0, dF = 0;
		CsgLandingArm aT = CsgLandingArm::None, aF = CsgLandingArm::None;
		const bool okT = transformed->DistanceToSurfaceWithArm( station, Scalar( 1 ), dT, aT );
		const bool okF = folded->DistanceToSurfaceWithArm( station, Scalar( 1 ), dF, aF );
		Check( okT && okF, "(j) both the transformed and the folded composite answer" );
		// eps is the SAME for both, which is the point of choosing an
		// axisymmetric operand A: A's parent-frame AABB is unchanged by a
		// rotation about its own axis, so the probe step does not move.
		const Scalar epsLocal = Scalar( 5e-5 ) * (Scalar)std::sqrt( 0.5*0.5 + 5.0*5.0 + 0.5*0.5 );
		if( okT && okF ) {
			// BOTH ARE AT OR ABOVE THE CLOSED FORM -- the invariant that
			// matters, and the one a tolerance on the landing test would
			// have broken.
			Check( dT >= Scalar( 0.01 ) - Scalar( 1e-12 ) && dF >= Scalar( 0.01 ) - Scalar( 1e-12 ),
				"(j) MONEY -- both the transformed and the folded composite are at or above the "
				"closed form 0.01 at a station 1 cm off the wall" );
			CheckClose( dT, Scalar( 0.01 ), Scalar( 1e-12 ),
				"(j) ...and the TRANSFORMED one hits it exactly (its boundary arm fires)" );
			// AND THEY AGREE TO ONE PROBE STEP, not to 1e-9.  8's gate
			// asked for 1e-9 on an AXISYMMETRIC operand A, reasoning that
			// folding a rotation into a BOX operand would move eps; that
			// reasoning is right about eps and incomplete about the
			// LANDING.  In the unrotated frame the descent's arithmetic is
			// exact by Sterbenz -- `0.26 (-) 0.25` is exact, so the landing
			// has `f_A` a true zero and the BOUNDARY arm fires with gap 0.
			// Push the same point through a 45-degree rotation and that
			// exactness is gone: `f_A` at the landing misses zero by ulps,
			// the boundary arm declines, and the probe takes exactly one
			// eps step -- `d` plus a step instead of `d`, the SAFE
			// direction the design names for a `+1 ulp` miss.  So the two
			// agree to a probe step and not to rounding, and the arms
			// differ.
			Check( std::fabs( (double)( dT - dF ) ) <= (double)( epsLocal * Scalar( 1.05 ) ),
				"(j) MONEY -- a TRANSFORMED composite agrees with the same composite built with "
				"the transform FOLDED into its operands to within one probe step" );
			std::cout << "    transformed composite " << (double)dT << " vs folded "
				<< (double)dF << " (arms " << ArmName( aT ) << " / " << ArmName( aF )
				<< "), difference " << (double)( dF - dT ) << " = "
				<< (double)( ( dF - dT ) / epsLocal ) << " eps" << std::endl;
		}
		transformed->release();
		folded->release();

		// THE BOX-MINUS-BOX TWIN, compared at ~eps as the design's own
		// note prescribes: operand A is NOT axisymmetric here, so folding
		// the rotation grows its parent-frame AABB by sqrt(2) in x and z
		// and eps moves with it.
		{
			Object* ba = MakeOperand( new BoxGeometry( Scalar( 1 ), Scalar( 1 ), Scalar( 1 ) ), Point3( 0, 0, 0 ) );
			Object* bb = MakeOperand( new BoxGeometry( Scalar( 0.4 ), Scalar( 0.4 ), Scalar( 2 ) ), Point3( 0, 0, 0 ) );
			CSGObject* bt = MakeCsg( CSG_SUBTRACTION, ba, bb, pos, orient );

			Object* fba = MakeOperand( new BoxGeometry( Scalar( 1 ), Scalar( 1 ), Scalar( 1 ) ), Point3( 0, 0, 0 ) );
			Object* fbb = MakeOperand( new BoxGeometry( Scalar( 0.4 ), Scalar( 0.4 ), Scalar( 2 ) ), Point3( 0, 0, 0 ) );
			const Matrix4 om = bt->GetFinalTransformMatrix();
			fba->PushTopTransStack( om ); fba->FinalizeTransformations();
			fbb->PushTopTransStack( om ); fbb->FinalizeTransformations();
			CSGObject* bf = MakeCsg( CSG_SUBTRACTION, fba, fbb, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

			// A world station 0.3 above the composite's centre, clear of
			// the slot (which runs along the composite's local z).
			const Point3 bst( 2.5, 3.3, 2.5 );
			Scalar d1 = 0, d2 = 0;
			CsgLandingArm a1 = CsgLandingArm::None, a2 = CsgLandingArm::None;
			const bool ok1 = bt->DistanceToSurfaceWithArm( bst, Scalar( 2 ), d1, a1 );
			const bool ok2 = bf->DistanceToSurfaceWithArm( bst, Scalar( 2 ), d2, a2 );
			Check( ok1 && ok2, "(j) the box-minus-box pair both answer" );
			// The FOLDED one's eps is the larger of the two, so it is the
			// bound the comparison has to use.
			const Scalar epsFolded = Scalar( 5e-5 ) * (Scalar)std::sqrt(
				2.0*2.0 + 1.0*1.0 + 2.0*2.0 );
			if( ok1 && ok2 ) {
				Check( d1 >= Scalar( 0.3 ) - Scalar( 1e-12 ) && d2 >= Scalar( 0.3 ) - Scalar( 1e-12 ),
					"(j) ...both at or above the closed form 0.3" );
				Check( std::fabs( (double)( d1 - d2 ) ) <= (double)( Scalar( 2 ) * epsFolded ),
					"(j) MONEY -- a NON-axisymmetric operand A: the transformed and folded "
					"composites agree at ~eps, which is the honest tolerance once folding the "
					"rotation grows A's parent-frame AABB" );
				std::cout << "    box-minus-box: transformed " << (double)d1 << " vs folded "
					<< (double)d2 << " (arms " << ArmName( a1 ) << " / " << ArmName( a2 )
					<< "), eps_folded " << (double)epsFolded << std::endl;
			}
			bt->release();
			bf->release();
		}
	}

	// --- NESTED COMPOSITES.  A union inside a subtraction, and an
	// INTERSECTION inside a subtraction: the inner composite exports its
	// own composed field as `SignedDistanceLower`, and the outer one
	// consumes it exactly as it would a leaf operand.
	{
		// union-in-subtraction: box minus (two overlapping spheres).
		Object* s1 = MakeOperand( new SphereGeometry( Scalar( 0.5 ) ), Point3( 0.5, 1, 0 ) );
		Object* s2 = MakeOperand( new SphereGeometry( Scalar( 0.5 ) ), Point3( -0.5, 1, 0 ) );
		CSGObject* inner = MakeCsg( CSG_UNION, s1, s2, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );
		Object* box = MakeOperand( new BoxGeometry( Scalar( 2 ), Scalar( 2 ), Scalar( 2 ) ), Point3( 0, 0, 0 ) );
		CSGObject* outer = MakeCsg( CSG_SUBTRACTION, box, inner, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		Scalar d = 0;
		CsgLandingArm arm = CsgLandingArm::None;
		Check( outer->DistanceToSurfaceWithArm( Point3( 0, 0, -3 ), Scalar( 10 ), d, arm ),
			"(j) MONEY -- a UNION nested inside a SUBTRACTION composes and answers" );
		CheckClose( d, Scalar( 2 ), Scalar( 1e-9 ), "(j) ...with the box's 3 - 1 = 2" );
		outer->release();
	}

	// --- THE ABUTTING UNION, which is the fixture the two spheres above
	// could not be.  Their boundaries meet on a CIRCLE that lies ON the
	// union's own boundary; two boxes STACKED into a cube meet on a shared
	// FACE that lies in the union's INTERIOR, and there `min(f_A, f_B)` is
	// 0 while the true signed distance is the cube's depth.
	//
	// Exported with an exactness flag -- which §5.6 allows and this
	// implementation refuses -- that lets the parent subtraction's
	// boundary arm admit a landing strictly INSIDE the subtrahend, i.e.
	// not in the real solid, and report a chord SHORTER than the truth.
	// That is an OVER-READ of contact, the one direction the signal must
	// never fail in, and it is the round-3 phantom one level up.
	{
		Object* b1 = MakeOperand( new BoxGeometry( Scalar( 2 ), Scalar( 2 ), Scalar( 1 ) ),
			Point3( 0, 0, -0.5 ) );
		Object* b2 = MakeOperand( new BoxGeometry( Scalar( 2 ), Scalar( 2 ), Scalar( 1 ) ),
			Point3( 0, 0, 0.5 ) );
		Object* b1Ref = b1; Object* b2Ref = b2;
		b1Ref->addref(); b2Ref->addref();
		CSGObject* cube = MakeCsg( CSG_UNION, b1, b2, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		// THE SEAM ITSELF: the two boxes abut at z = 0, so the union's
		// exported field reads 0 at the CUBE'S CENTRE -- an interior point
		// one whole unit from the nearest real surface.
		Scalar fSeam = 0; bool exSeam = true;
		Check( cube->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 10 ), fSeam, exSeam ),
			"(j) the abutting union answers at its interior seam" );
		CheckClose( fSeam, Scalar( 0 ), Scalar( 1e-12 ),
			"(j) MONEY -- `min(f_A, f_B)` is EXACTLY 0 at the cube's centre, an INTERIOR point "
			"1.0 from the nearest surface: the union's zero set is not its boundary" );
		Check( !exSeam,
			"(j) MONEY -- ...and the union does NOT flag that 0 as exact. If it did, the parent "
			"subtraction below would admit this point as a boundary landing and report contact "
			"1.0 unit from anything real" );

		Object* big = MakeOperand( new BoxGeometry( Scalar( 4 ), Scalar( 4 ), Scalar( 4 ) ),
			Point3( 0, 0, 0 ) );
		CSGObject* carved = MakeCsg( CSG_SUBTRACTION, big, cube, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		// The probe sits in the CAVITY at (0, 0, -0.25).  The descent's
		// single step lands exactly on the seam (the arithmetic is
		// Sterbenz-exact), so this is the station that breaks if the flag
		// ever comes back.  The nearest REAL surface is the cavity wall at
		// z = -1, three times further away.
		const Point3 probe( 0, 0, -0.25 );
		const Scalar trueDist = Scalar( 0.75 );
		Scalar dc = 0;
		CsgLandingArm ac = CsgLandingArm::None;
		// COUNTED, and asserted non-zero below.  A refusal here is
		// legitimately SAFE (an under-paint), so it cannot itself be a
		// failure -- but if BOTH probes ever refuse, the only fixture
		// pinning the over-read this block exists for would go silent with
		// nothing red.  Review round 2 asked for the counter.
		int abuttingAnswers = 0;
		const bool answered = carved->DistanceToSurfaceWithArm( probe, Scalar( 10 ), dc, ac );
		if( answered ) {
			++abuttingAnswers;
			Check( dc >= trueDist - Scalar( 1e-9 ),
				"(j) MONEY -- the carved cube NEVER reports closer than the true 0.75 from inside "
				"its cavity. A union that exported exactness reported 0.25 here -- contact "
				"painted 3x too close, which is how this fixture was found" );
			std::cout << "    abutting union-in-subtraction: true 0.75, reported " << (double)dc
				<< ", arm " << ArmName( ac ) << std::endl;
		}
		else {
			std::cout << "    abutting union-in-subtraction: REFUSED (an under-paint, which is "
				"the safe direction)" << std::endl;
		}

		// ...and the degenerate twin: a query point ON the seam.  It has
		// `f0 == 0`, skips the inside early-return, and must not be
		// admitted at zero distance.
		Scalar ds = 0;
		CsgLandingArm as = CsgLandingArm::None;
		if( carved->DistanceToSurfaceWithArm( Point3( 0, 0, 0 ), Scalar( 10 ), ds, as ) ) {
			++abuttingAnswers;
			Check( ds >= Scalar( 1 ) - Scalar( 1e-9 ),
				"(j) MONEY -- a query point ON the seam is 1.0 from the real solid and never "
				"reads contact" );
		}
		Check( abuttingAnswers > 0,
			"(j) MONEY -- at least ONE of the two abutting-union probes ANSWERED, so the "
			"over-read assertions above are live rather than skipped -- the durability guard "
			"review round 2 asked for on the fixture that pins P1-A" );

		b1Ref->release(); b2Ref->release();
		carved->release();
	}
	{
		// intersection-in-subtraction, and the nested composite's own
		// exactness export checked directly.
		Object* s1 = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, 0.6, 0 ) );
		Object* s2 = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, -0.6, 0 ) );
		Object* innerRef = 0;
		CSGObject* inner = 0;
		{
			CSGObject* i2 = new CSGObject( CSG_INTERSECTION );
			i2->AssignObjects( s1, s2 );
			s1->release(); s2->release();
			i2->FinalizeTransformations();
			inner = i2;
			innerRef = i2;
			innerRef->addref();
		}

		Scalar fv = 0; bool ex = true;
		Check( inner->SignedDistanceLower( Point3( 0, 0, 3 ), Scalar( 10 ), fv, ex ),
			"(j) the nested intersection answers its parent's signed query" );
		Check( !ex,
			"(j) MONEY -- a nested INTERSECTION reports exact = FALSE to its parent: its zero "
			"set is the phantom touching set, so the parent's boundary arm must not land on it" );

		Object* box = MakeOperand( new BoxGeometry( Scalar( 4 ), Scalar( 4 ), Scalar( 4 ) ), Point3( 0, 0, 0 ) );
		CSGObject* outer = MakeCsg( CSG_SUBTRACTION, box, inner, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );
		Scalar d = 0;
		CsgLandingArm arm = CsgLandingArm::None;
		Check( outer->DistanceToSurfaceWithArm( Point3( 0, 0, 5 ), Scalar( 10 ), d, arm ),
			"(j) MONEY -- an INTERSECTION nested inside a SUBTRACTION composes and answers" );
		CheckClose( d, Scalar( 3 ), Scalar( 1e-9 ), "(j) ...with the box's 5 - 2 = 3" );
		std::cout << "    intersection-in-subtraction: reported " << (double)d
			<< ", arm " << ArmName( arm ) << std::endl;
		outer->release();
		innerRef->release();
	}

	// --- `DescribeKind` NAMES THE OPERATION, which is what the proximity
	// refusal log prints in place of the old "(no geometry)".
	{
		Object* a = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, 0, 0 ) );
		Object* b = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 1, 0, 0 ) );
		CSGObject* c = MakeCsg( CSG_SUBTRACTION, a, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );
		Check( std::string( c->DescribeKind() ) == "csg subtraction",
			std::string( "(j) MONEY -- DescribeKind names the OPERATION: " ) + c->DescribeKind() );
		c->SetOperation( CSG_UNION );
		Check( std::string( c->DescribeKind() ) == "csg union", "(j) ...and follows a re-point" );
		c->release();

		// An ordinary Object still names its geometry's type.
		Object* o = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 0, 0, 0 ) );
		Check( std::string( o->DescribeKind() ).find( "SphereGeometry" ) != std::string::npos,
			std::string( "(j) ...teeth: an Object names its geometry: " ) + o->DescribeKind() );
		o->release();
	}
}


//======================================================================
// (k) interior(r) -- THE SIGNED BUILTIN
//======================================================================

//! The signal itself, built from the channel by hand exactly as
//! `ProximityAt` is, with a cold memo per call for the same reason.
static Scalar InteriorAt( const Fixture& f, const Point3& p, const IObjectPriv* self, const Scalar r )
{
	SurfaceSignalInfo s;
	s.pScene  = f.mgr;
	s.pSelf   = self;
	s.ptWorld = p;
	ExpressionMemo::Invalidate();
	return s.Interior( r );
}

//! `interior(r)` (design 5.6) -- `clamp(depth / r, 0, 1)` for `depth` the
//! LARGEST inside-depth lower bound over every other world-visible
//! non-emissive object that CONTAINS the hit point.  0 = inside no
//! neighbour, 1 = at least `r` deep; neutral 0; radius a WORLD LENGTH and
//! mandatory.
//!
//! WHY A SECOND BUILTIN AND NOT A SIGN ON THE FIRST: together the two
//! cover the signed distance without a sign convention an author has to
//! remember, and `proximity`'s "interpenetration IS contact" clamp (which
//! reads 1 anywhere inside a neighbour, at any depth) is a convention
//! worth keeping rather than one to negotiate with.
static void TestInterior( const Fixture& f )
{
	std::cout << "(k) interior(r) -- the signed builtin" << std::endl;

	// --- 0 OUTSIDE.  Every scene-C probe that sits outside its neighbours
	// reads exactly the neutral, not a small positive.
	{
		const Point3 outside[5] = {
			Point3( 0, 0, 0 ), Point3( 6, 0, 0 ), Point3( 12, 0, 0 ),
			Point3( 42, 0, 1.5 ), Point3( 54, 0, 0 ) };
		for( int i = 0; i < 5; ++i ) {
			CheckClose( InteriorAt( f, outside[i], f.floorObj, Scalar( 3 ) ), Scalar( 0 ), Scalar( 0 ),
				"(k) MONEY -- a floor point OUTSIDE its neighbour reads exactly the neutral 0" );
		}
	}

	// --- THE SIX INTERPENETRATION PROBES, against `min(depth/r, 1)`.
	// These are the same six points section (e) checks read `proximity`
	// 1 at; here the DEPTH the unsigned signal throws away is the answer.
	//
	// The ELLIPSOID row is a LOWER BOUND that is tight only where the
	// unit-sphere pull-back's nearest direction is the smallest semi-axis
	// -- true along the whole minor axis, and the CENTRE is the point the
	// fixture has, so the probe stays there and the test says so.
	{
		struct Row { const char* name; Point3 p; Scalar depth; };
		const Row rows[6] = {
			{ "box",             Point3( 6, 3, 0 ),    Scalar( 1 )   },
			{ "sphere",          Point3( 0, 3, 0 ),    Scalar( 1 )   },
			{ "capped cylinder", Point3( 12, 4, 0 ),   Scalar( 1 )   },
			{ "torus tube",      Point3( 42, 3, 1.5 ), Scalar( 0.5 ) },
			{ "ellipsoid",       Point3( 48, 3, 0 ),   Scalar( 1 )   },
			{ "SDF",             Point3( 54, 3, 0 ),   Scalar( 1 )   } };

		for( int i = 0; i < 6; ++i ) {
			// A radius ABOVE the depth: the signal reads depth/r.
			const Scalar rBig = Scalar( 4 );
			const Scalar want = rows[i].depth / rBig;
			CheckClose( InteriorAt( f, rows[i].p, f.floorObj, rBig ), want, Scalar( 1e-9 ),
				std::string( "(k) MONEY -- " ) + rows[i].name + ": interior = depth/r" );

			// A radius AT OR BELOW the depth: exactly 1.
			const Scalar rSmall = rows[i].depth;
			CheckClose( InteriorAt( f, rows[i].p, f.floorObj, rSmall ), Scalar( 1 ), Scalar( 1e-9 ),
				std::string( "(k) ...and exactly 1 where r <= the depth (" ) + rows[i].name + ")" );
		}
	}

	// --- INSIDE TWO OVERLAPPING SOLIDS the answer is the LARGER depth.
	// Leaving their UNION needs at least the larger of the two, so a max
	// is still a lower bound -- the under-paint direction.
	{
		Object* big   = MakeOperand( new SphereGeometry( Scalar( 2 ) ), Point3( 0, 0, 0 ) );
		Object* small = MakeOperand( new SphereGeometry( Scalar( 1 ) ), Point3( 1.5, 0, 0 ) );

		// A point inside BOTH: 0.5 from the small sphere's centre (depth
		// 0.5) and 1.0 from the big one's (depth 1.0).
		const Point3 p( 1.0, 0, 0 );
		Scalar fBig = 0, fSmall = 0; bool ex = false;
		Check( big->SignedDistanceLower( p, Scalar( 10 ), fBig, ex ) && fBig < Scalar( 0 ),
			"(k) the big sphere contains the probe" );
		Check( small->SignedDistanceLower( p, Scalar( 10 ), fSmall, ex ) && fSmall < Scalar( 0 ),
			"(k) ...and so does the small one" );
		CheckClose( -fBig,   Scalar( 1.0 ), Scalar( 1e-9 ), "(k) ...at depths 1.0" );
		CheckClose( -fSmall, Scalar( 0.5 ), Scalar( 1e-9 ), "(k) ...and 0.5" );

		SphereGeometry* g = new SphereGeometry( Scalar( 0.1 ) );
		Object* receiver = new Object( g );
		g->release();
		receiver->SetPosition( Point3( 20, 0, 0 ) );
		receiver->FinalizeTransformations();

		IObjectManager* mgr = 0;
		Check( RISE_API_CreateObjectManager( &mgr, true, false, 4, 32 ), "(k) a manager" );
		if( !mgr ) return;
		mgr->AddItem( big,      "big" );
		mgr->AddItem( small,    "small" );
		mgr->AddItem( receiver, "receiver" );
		mgr->PrepareForRendering();

		Scalar depth = 0;
		Check( mgr->DeepestOtherContainment( p, receiver, Scalar( 10 ), depth ),
			"(k) the manager finds a containment" );
		CheckClose( depth, Scalar( 1.0 ), Scalar( 1e-9 ),
			"(k) MONEY -- inside TWO overlapping spheres the answer is the LARGER depth (1.0), "
			"not the smaller and not their sum" );

		// A point inside only the SMALL one: no max to take.
		Scalar d2 = 0;
		Check( mgr->DeepestOtherContainment( Point3( 2.4, 0, 0 ), receiver, Scalar( 10 ), d2 ),
			"(k) ...and a point inside only the small sphere answers" );
		CheckClose( d2, Scalar( 0.1 ), Scalar( 1e-9 ), "(k) ...with the small one's own depth" );

		// OUTSIDE both: a refusal, which `interior` reads as its neutral.
		Scalar d3 = 0;
		Check( !mgr->DeepestOtherContainment( Point3( 9, 0, 0 ), receiver, Scalar( 10 ), d3 ),
			"(k) MONEY -- a point inside NOTHING refuses, which is the neutral 0" );

		mgr->release();
		receiver->release();
		small->release();
		big->release();
	}

	// --- INSIDE A UNION COMPOSITE'S OVERLAP the exported depth UNDER-READS.
	// A union exports `min(f_A, f_B)` as its signed lower bound -- whose
	// MAGNITUDE is `max(depth_A, depth_B)`, the DEEPER of the two, which is
	// the right lower bound because leaving the union means leaving both.
	// It is still only a lower bound: at a point deeper in the UNION than
	// either operand alone, the union's true depth exceeds both.  §10
	// records this as a residual; here it is asserted, both as an
	// inequality and as a STRICT one.
	{
		Object* a = MakeOperand( new SphereGeometry( Scalar( 2 ) ), Point3( 0, 0, 0 ) );
		Object* b = MakeOperand( new SphereGeometry( Scalar( 2 ) ), Point3( 1.5, 0, 0 ) );
		Object* aRef = a; Object* bRef = b;
		aRef->addref(); bRef->addref();
		CSGObject* u = MakeCsg( CSG_UNION, a, b, Point3( 0, 0, 0 ), Vector3( 0, 0, 0 ) );

		// The probe sits OFF THE MID-PLANE, deliberately: at (0.75, 0, 0)
		// the two operand depths are both 1.25, and `std::min` of two equal
		// numbers cannot tell `min` from `max`, from a mean, or from
		// "whichever operand was visited first".  At (0.4, 0, 0) they are
		// 1.6 and 0.9, which discriminates.
		const Point3 p( 0.4, 0, 0 );
		Scalar fA = 0, fB = 0, fU = 0; bool eA = false, eB = false, eU = false;
		Check( aRef->SignedDistanceLower( p, Scalar( 10 ), fA, eA )
		    && bRef->SignedDistanceLower( p, Scalar( 10 ), fB, eB )
		    && u->SignedDistanceLower( p, Scalar( 10 ), fU, eU ),
			"(k) both operands and the union answer at the overlap probe" );

		// THE TRUE DEPTH OF THE UNION, and it is the CREASE -- not either
		// sphere's wall.  The probe is 0.4 from A's centre and 1.1 from
		// B's, so the operand depths are 1.6 and 0.9.  A's wall is 1.9596
		// away straight out along +y, and that point really is ON the
		// union's boundary (it lies outside B) -- but the two spheres'
		// surfaces MEET on a circle, and that crease is nearer.  Solving
		// `|p| = 2` and `|p - (1.5,0,0)| = 2` puts it at x = 0.75,
		// rho = 1.85405, which from (0.4, 0, 0) is
		// hypot(0.35, 1.85405) = 1.886796 away.
		//
		// REVIEW ROUND 2 CAUGHT THE PERPENDICULAR 1.9596 SITTING HERE, and
		// the error mattered in the forbidden direction: as a reference it
		// made the lower-bound check below PERMISSIVE by 0.073, admitting
		// any export in [1.886796, 1.959592] -- an OVER-read.  A reference
		// for a never-over-read invariant has to be the true MINIMUM over
		// the boundary, not a convenient point on it.
		const Scalar trueDepth = (Scalar)std::sqrt(
			( 0.75 - 0.4 ) * ( 0.75 - 0.4 ) + ( 4.0 - 0.75 * 0.75 ) );
		CheckClose( -fA, Scalar( 1.6 ), Scalar( 1e-9 ), "(k) operand A's depth is 1.6" );
		CheckClose( -fB, Scalar( 0.9 ), Scalar( 1e-9 ), "(k) ...and operand B's is 0.9" );
		// `min` is taken on the SIGNED values, so it picks the most
		// NEGATIVE -- i.e. `|min(f_A, f_B)| = max(depth_A, depth_B)`, the
		// DEEPER of the two.  That is the right lower bound: leaving the
		// union means leaving BOTH operands, so the union's depth is at
		// least the larger of the individual depths.  A probe on the
		// mid-plane, where the two depths are equal, cannot tell that from
		// the shallower one -- and this check asserted the shallower until
		// the probe was moved off it.
		CheckClose( -fU, Scalar( 1.6 ), Scalar( 1e-9 ),
			"(k) MONEY -- the union exports the DEEPER of the two depths (1.6), which is "
			"`|min(f_A, f_B)| = max(depth_A, depth_B)` -- and a mid-plane probe, where both are "
			"1.25, could not have told that from the shallower" );
		CheckClose( -fU, std::max( -fA, -fB ), Scalar( 1e-12 ),
			"(k) ...i.e. exactly max(depth_A, depth_B), the magnitude of min(f_A, f_B)" );
		Check( -fU <= trueDepth + Scalar( 1e-9 ),
			"(k) MONEY -- the exported depth is a LOWER bound on the union's true depth" );
		Check( -fU < trueDepth - Scalar( 1e-6 ),
			"(k) MONEY -- and STRICTLY under-reads at a point deeper in the union than either "
			"operand alone: the residual §10 records, asserted rather than claimed" );
		std::cout << "    union overlap at (0.4,0,0): operand depths " << (double)(-fA)
			<< " / " << (double)(-fB) << ", exported " << (double)(-fU)
			<< ", true union depth " << (double)trueDepth << std::endl;

		aRef->release(); bRef->release();
		u->release();
	}

	// --- A MESH NEIGHBOUR CONTRIBUTES 0.  A triangle soup carries no
	// inside test, so it refuses the signed query at every point -- and
	// SILENTLY, because a shared refusal latch would print the proximity
	// message for every mesh and every plane in the scene.
	{
		TriangleMeshGeometryIndexed* mg = BuildTinyMesh();
		Object* mesh = new Object( mg );
		mg->release();
		mesh->FinalizeTransformations();

		SphereGeometry* g = new SphereGeometry( Scalar( 0.1 ) );
		Object* receiver = new Object( g );
		g->release();
		receiver->SetPosition( Point3( 20, 0, 0 ) );
		receiver->FinalizeTransformations();

		IObjectManager* mgr = 0;
		Check( RISE_API_CreateObjectManager( &mgr, true, false, 4, 32 ), "(k) a second manager" );
		if( !mgr ) return;
		mgr->AddItem( mesh,     "mesh" );
		mgr->AddItem( receiver, "receiver" );
		mgr->PrepareForRendering();

		Scalar depth = 0;
		Check( !mgr->DeepestOtherContainment( Point3( 0, 0, 0 ), receiver, Scalar( 10 ), depth ),
			"(k) MONEY -- a MESH neighbour contributes NOTHING to interior, even at a point "
			"on it: a sheet has no inside" );

		// TEETH: the same manager's UNSIGNED query does answer there, so
		// the refusal above is the signed query's sheet rule and not an
		// empty scene.
		Scalar d = 0;
		Check( mgr->NearestOtherSurface( Point3( 0, 0.5, 0 ), receiver, Scalar( 10 ), d ),
			"(k) ...teeth: the same mesh answers the UNSIGNED query" );
		CheckClose( d, Scalar( 0.5 ), Scalar( 1e-9 ), "(k) ...with 0.5" );

		mgr->release();
		receiver->release();
		mesh->release();
	}

	// --- THE NEUTRALS, the same five `proximity` has.
	{
		SurfaceSignalInfo blank;
		CheckClose( blank.Interior( Scalar( 1 ) ), Scalar( 0 ), Scalar( 0 ),
			"(k) a default-constructed channel (no scene) reads the neutral 0" );

		SurfaceSignalInfo noSelf;
		noSelf.pScene  = f.mgr;
		noSelf.ptWorld = Point3( 6, 3, 0 );
		ExpressionMemo::Invalidate();
		CheckClose( noSelf.Interior( Scalar( 4 ) ), Scalar( 0 ), Scalar( 0 ),
			"(k) a channel with a scene but NO self reads the neutral 0" );

		SurfaceSignalInfo bad;
		bad.pScene  = f.mgr;
		bad.pSelf   = f.floorObj;
		bad.ptWorld = Point3( 0, std::numeric_limits<double>::quiet_NaN(), 0 );	// HYGIENE-OK: an input, not a sentinel
		ExpressionMemo::Invalidate();
		CheckClose( bad.Interior( Scalar( 5 ) ), Scalar( 0 ), Scalar( 0 ),
			"(k) a NON-FINITE hit point reads the neutral 0" );

		CheckClose( InteriorAt( f, Point3( 6, 3, 0 ), f.floorObj, Scalar( -1 ) ), Scalar( 0 ), Scalar( 0 ),
			"(k) a computed radius <= 0 reads the neutral 0" );
		CheckClose( InteriorAt( f, Point3( 6, 3, 0 ), f.floorObj,
			(Scalar)std::numeric_limits<double>::quiet_NaN() ), Scalar( 0 ), Scalar( 0 ),	// HYGIENE-OK: an input, not a sentinel
			"(k) a NON-FINITE computed radius reads the neutral 0" );
	}

	// --- THE BUILTIN, THROUGH THE COMPILER.
	{
		ExpressionProgram p = ExpressionProgram::Invalid();
		Check( CompileWithContext( "interior(0.002)", p ), "(k) an interior body compiles" );
		Check( p.SurfaceSignalCalls().size() == 1, "(k) one signal call site recorded" );
		if( p.SurfaceSignalCalls().size() == 1 ) {
			Check( p.SurfaceSignalCalls()[0].fn == ExpressionProgram::kFnInterior,
				"(k) MONEY -- recorded as kFnInterior, not as proximity or one of the self-signals" );
			Check( p.SurfaceSignalCalls()[0].radiusIsLiteral,
				"(k) ...with its literal radius proved" );
		}
		Check( p.MemoWorthy(), "(k) a body calling interior() is memo-worthy" );
		Check( p.UsesSurfaceSignals(), "(k) ...and registers as a surface-signal consumer" );
		Check( p.UsesCrossObject(),
			"(k) MONEY -- and UsesCrossObject() is TRUE for an interior-only program, which is "
			"what gates the eager AABB snapshot" );

		// TEETH on the rename: a proximity-only program still says yes,
		// and a signal-free one still says no.
		ExpressionProgram pp = ExpressionProgram::Invalid();
		Check( CompileWithContext( "proximity(0.002)", pp ) && pp.UsesCrossObject(),
			"(k) ...teeth: a proximity-only program still registers" );
		ExpressionProgram po = ExpressionProgram::Invalid();
		Check( CompileWithContext( "occlusion(0.1)", po ) && !po.UsesCrossObject(),
			"(k) ...and an occlusion-only one does NOT" );
	}

	// --- THE PARSE DIAGNOSTIC NAMES A WORLD LENGTH, and its OWN sentence.
	// The ternary is three-way since `interior` joined: handing an
	// `interior` author proximity's wording about surfaces that stop
	// registering as contact would describe the wrong quantity.
	{
		ExpressionProgram bad = ExpressionProgram::Invalid();
		Check( !CompileWithContext( "interior(-1.0)", bad ),
			"(k) a non-positive literal interior radius is a COMPILE error" );
		const std::string err = bad.Error();
		Check( err.find( "WORLD LENGTH" ) != std::string::npos,
			"(k) MONEY -- and the message says WORLD LENGTH: " + err );
		Check( err.find( "FRACTION" ) == std::string::npos,
			"(k) ...and does NOT repeat the self-signals' fraction wording" );
		Check( err.find( "depth of burial" ) != std::string::npos,
			"(k) MONEY -- and it is interior's OWN sentence, not proximity's: " + err );

		ExpressionProgram badProx = ExpressionProgram::Invalid();
		Check( !CompileWithContext( "proximity(-1.0)", badProx ), "(k) teeth: proximity still errors" );
		Check( badProx.Error().find( "depth of burial" ) == std::string::npos,
			"(k) ...with its own wording, unchanged" );
	}

	// --- THE UV-ONLY SURFACE REFUSES IT, as it refuses the other four.
	{
		ExpressionProgram p = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;			// context vars OFF (the default)
		Check( !b.Finalize( "interior(0.5)", p ),
			"(k) interior() is refused on the UV-only expression_function2d surface" );
		Check( p.Error().find( "3D surface context" ) != std::string::npos,
			"(k) ...with the dedicated diagnostic, not `unknown function`" );
	}

	// --- A COMPUTED RADIUS IS ACCEPTED, and reaches the same answer as a
	// literal one.  `interior` has no `DynR` twin, so ParseCall's remap
	// must fall through to its own id -- the guard `proximity` needed, one
	// signal later.  Without it a computed-radius `interior(...)` compiles
	// to `kFnConvexityDynR`.
	//
	// THE RECEIVER IS THE SDF SPHERE, NOT THE FLOOR, and that choice is
	// the whole test.  On the infinite plane -- where the proximity twin
	// of this check runs -- there is no signal provider, so a mis-compiled
	// convexity would read its NEUTRAL 0; and `interior` on a floor point
	// inside nothing is ALSO 0.  The equality would then hold with the
	// guard removed: `0 == 0`, a tautology.  An SDF publishes a provider,
	// so a mis-compiled convexity reads a real non-zero number there while
	// `interior` still reads 0 -- two values that cannot be confused.  The
	// third probe below asserts that separation is live rather than
	// assumed.
	{
		const Point3  origin( 54, 6, 0 );		// above `n_sdf_sphere` at (54, 3, 0), R = 1
		const Vector3 down( 0, -1, 0 );

		Scalar vConv = Scalar( -1 );
		Check( EvalAtManagerHit( f, origin, down, "convexity(0.5)", vConv ),
			"(k) convexity evaluates at the SDF hit" );
		Check( vConv > Scalar( 0.01 ),
			"(k) MONEY -- ...and reads a REAL non-zero value there (" + std::to_string( (double)vConv ) +
			"), so a mis-compiled interior would be visibly different from the 0 below -- this is "
			"what makes the equality that follows a test rather than a tautology" );

		Scalar vLit = Scalar( -1 ), vComp = Scalar( -1 );
		Check( EvalAtManagerHit( f, origin, down, "interior(0.5)", vLit ),
			"(k) literal-radius interior compiles and evaluates at the same hit" );
		Check( EvalAtManagerHit( f, origin, down, "interior(0.25*2.0)", vComp ),
			"(k) COMPUTED-radius interior compiles and evaluates" );
		CheckClose( vLit, Scalar( 0 ), Scalar( 1e-12 ),
			"(k) the SDF's own surface is inside no OTHER object, so interior reads 0 there" );
		CheckClose( vComp, vLit, Scalar( 1e-12 ),
			"(k) MONEY -- a COMPUTED radius gives the same answer as the literal one, and on a "
			"receiver where convexity would NOT: the DynR remap's fall-through is not turning "
			"interior() into convexity()" );

		// ...and the builtin is capable of a non-zero reading at all, so
		// the 0 above is the geometry and not a dead signal.
		const Scalar vInside = InteriorAt( f, Point3( 6, 3, 0 ), f.floorObj, Scalar( 4 ) );
		CheckClose( vInside, Scalar( 0.25 ), Scalar( 1e-9 ),
			"(k) ...teeth: the same builtin reads 0.25 at the centre of `n_box`, so the 0 above "
			"is the absence of a container, not a signal that never fires" );
	}
}


//======================================================================
// (l) THE SHOWCASE: glass_pavilion's fluted column
//======================================================================

//! THE PHASE-3 SHOWCASE GATE (design §8), driven against the TRACKED
//! scene rather than a fixture, because what it proves is that a real
//! composite in a real scene is now a NEIGHBOUR.
//!
//! `column2` is a `subtraction` -- a capped cylinder of radius 0.25
//! spanning world y ∈ [0, 5] minus four thin flute slabs -- carrying its
//! OWN `position 2.5 2.5 2.5` / `orientation 0 45 0`, with UNTRANSFORMED
//! operands.  That is exactly the frame problem §5.6 calls the round-1
//! blocker: the operands live in the COMPOSITE's local frame, not in
//! world space.  It stands on `cap2`, a 0.7 × 0.15 × 0.7 box whose top is
//! y = 0.175, and the floor beneath is a 10 × 0.2 × 10 slab whose top is
//! y = 0.1 -- so the receiver is the CAP'S TOP FACE and the floor is
//! 0.075 below it, out of a 2 cm radius.
//!
//! Every station is expressed in the COMPOSITE'S LOCAL FRAME and pushed
//! through `column2`'s own final matrix, so the test cannot disagree with
//! the engine about what `orientation 0 45 0` means.
//! `ProximityAt`'s twin for a manager the Fixture struct does not hold --
//! same cold-memo discipline, same hand-built channel.
static Scalar ProximityAtMgr( IObjectManager* mgr, const Point3& p,
	const IObjectPriv* self, const Scalar r )
{
	SurfaceSignalInfo s;
	s.pScene  = mgr;
	s.pSelf   = self;
	s.ptWorld = p;
	ExpressionMemo::Invalidate();
	return s.Proximity( r );
}

static void TestGlassPavilionShowcase()
{
	std::cout << "(l) the showcase -- glass_pavilion's fluted column as a NEIGHBOUR" << std::endl;

	const fs::path root = FindRepoRoot();
	const fs::path scenePath = root / "scenes" / "FeatureBased" / "Combined" / "glass_pavilion.RISEscene";
	std::ifstream in( scenePath );
	Check( in.good(), "(l) the tracked glass_pavilion scene is readable" );
	if( !in ) return;
	std::stringstream ss;
	ss << in.rdbuf();

	Cst::Document doc = Cst::ParseToCst( ss.str() );
	Job* job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *job, &diags );
	IObjectManager* mgr = job->GetObjects();
	Check( mgr != 0, "(l) it derives with an object manager" );
	if( !mgr ) { job->release(); return; }
	mgr->PrepareForRendering();

	IObjectPriv* col2   = mgr->GetItem( "column2" );
	IObjectPriv* cap2   = mgr->GetItem( "cap2" );
	IObjectPriv* colCyl = mgr->GetItem( "col2_cyl" );
	Check( col2 && cap2 && colCyl, "(l) column2, cap2 and the column's cylinder operand are present" );
	if( !col2 || !cap2 || !colCyl ) { job->release(); return; }

	// --- THE `capped TRUE` PIN, with teeth.  An OPEN cylinder is a SHEET:
	// it refuses the signed query, which would make this subtraction refuse
	// outright and void every station below.  The scene spells the default
	// out so a flip is a visible edit; this is what notices if it happens.
	{
		Scalar f = 0; bool ex = false;
		Check( colCyl->SignedDistanceLower( Point3( 0, 3, 0 ), Scalar( 10 ), f, ex ),
			"(l) MONEY -- `colcylgeom` answers the SIGNED query, i.e. it is CAPPED. An open tube "
			"is a sheet, refuses it, and would make every fluted column stop answering proximity()" );
		Check( ex, "(l) ...exactly, so a boundary landing on the column wall is admissible" );
	}

	const Matrix4 toWorld = col2->GetFinalTransformMatrix();
	// World y = 0.175 (the cap's top face) is local y = 0.175 - 2.5; the
	// rotation is about Y, so the y component passes through untouched.
	const Scalar capTopLocalY = Scalar( 0.175 ) - Scalar( 2.5 );

	// --- THE THREE CAP-TOP STATIONS, along the composite's local +x, 90°
	// from every flute wedge.  An upright wall gives `d(s) = s`, so
	// `proximity(0.02)` predicts 0.5 / 0 / 0 at 1 / 2 / 4 cm out from the
	// nominal wall at radius 0.25.
	{
		const Scalar offsets[3] = { Scalar( 0.01 ), Scalar( 0.02 ), Scalar( 0.04 ) };
		const Scalar want[3]    = { Scalar( 0.5 ),  Scalar( 0 ),    Scalar( 0 )    };
		for( int i = 0; i < 3; ++i ) {
			const Point3 local( Scalar( 0.25 ) + offsets[i], capTopLocalY, Scalar( 0 ) );
			const Point3 world = Point3Ops::Transform( toWorld, local );
			const Scalar v = ProximityAtMgr( mgr, world, cap2, Scalar( 0.02 ) );
			CheckClose( v, want[i], Scalar( 0.05 ),
				"(l) MONEY -- cap top at " + std::to_string( (double)offsets[i] ) +
				" from the column wall reads " + std::to_string( (double)want[i] ) );
			std::cout << "    cap-top station at " << (double)offsets[i]
				<< " m from the wall: proximity(0.02) = " << (double)v << std::endl;
		}
		// AND THE 1 cm STATION IS TIGHT, not merely inside 0.05.  The radial
		// landing IS the nearest point by symmetry and the cylinder operand
		// is exact, so the BOUNDARY arm fires and the answer is 0.5 to
		// rounding; `0.5 - eps/r` = 0.4874 is the floor if a `+1 ulp` miss
		// sends it to the probe instead.
		const Point3 local( Scalar( 0.26 ), capTopLocalY, Scalar( 0 ) );
		const Point3 world = Point3Ops::Transform( toWorld, local );
		const Scalar v = ProximityAtMgr( mgr, world, cap2, Scalar( 0.02 ) );
		Check( v <= Scalar( 0.5 ) + Scalar( 1e-9 ),
			"(l) MONEY -- and it never reads ABOVE 0.5: an over-read here would be contact "
			"painted where there is none" );
		Check( v >= Scalar( 0.4874 ) - Scalar( 1e-4 ),
			"(l) ...and never below the one-probe-step floor 0.5 - eps/r = 0.4874" );
	}

	// --- THE FLUTE STATION, and the phantom it refuses.  Local
	// (0, ·, 0.26) is 1 cm outside a slot's TANGENT face -- the slot is
	// 0.5 deep, the column's diameter, so its ±z faces graze the cylinder
	// at z = ±0.25.  An `f <= 0` landing test would report a 1.00 cm chord
	// here; the nearest REAL surface is the slot-wall/cylinder corner at
	// 4.21 cm.
	{
		const Point3 local( Scalar( 0 ), capTopLocalY, Scalar( 0.26 ) );
		const Point3 world = Point3Ops::Transform( toWorld, local );

		const Scalar v = ProximityAtMgr( mgr, world, cap2, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 0 ), Scalar( 1e-12 ),
			"(l) MONEY -- the flute station reads proximity(0.02) = 0. A phantom landing on the "
			"tangency would have read 0.5" );

		// THE PER-OBJECT CALL, which is what actually pins the refusal: the
		// SCENE-WIDE query at radius 0.1 would ANSWER from the floor top at
		// y = 0.1, 0.075 below the cap top, and a passing scene-wide check
		// would prove nothing about the composite.
		Scalar dObj = Scalar( 0 );
		Check( !col2->DistanceToSurface( world, Scalar( 0.1 ), dObj ),
			"(l) MONEY -- `column2->DistanceToSurface` at radius 0.1 REFUSES at this station -- "
			"the bracket keeps probing into the slot and never finds an admitted landing" );

		Scalar dScene = Scalar( 0 );
		Check( mgr->NearestOtherSurface( world, cap2, Scalar( 0.1 ), dScene ),
			"(l) ...while the SCENE-WIDE query at the same radius ANSWERS" );
		CheckClose( dScene, Scalar( 0.075 ), Scalar( 1e-9 ),
			"(l) ...with 0.075, the floor top at y = 0.1 under the cap top at 0.175 -- which is "
			"exactly why the refusal above has to be asked PER OBJECT" );

		// The true distance the refusal under-paints, recomputed here rather
		// than quoted: the slot-wall/cylinder corner.
		const double corner = std::sqrt( 0.04*0.04 +
			( 0.26 - std::sqrt( 0.25*0.25 - 0.04*0.04 ) ) * ( 0.26 - std::sqrt( 0.25*0.25 - 0.04*0.04 ) ) );
		std::cout << "    flute station: proximity(0.02) = " << (double)v
			<< "; column2 refuses at r = 0.1; the true corner distance the refusal under-paints is "
			<< corner << " m (4.21 cm)" << std::endl;
		Check( corner > 0.04 && corner < 0.043,
			"(l) ...and that corner really is 4.21 cm, four times the 1 cm a phantom would report" );
	}

	job->release();
}

//======================================================================

int main()
{
	std::cout << "=== ProximitySignalTest (cross-object proximity, Phase 1) ===" << std::endl;

	Fixture f;
	if( !LoadFixture( f ) ) {
		std::cout << "  FAIL: could not load scenes/Tests/Signals/proximity_closed_forms.RISEscene"
			<< std::endl;
		std::cout << std::endl << passCount << " passed, " << ( failCount + 1 ) << " failed." << std::endl;
		return 1;
	}

	TestExactFamilies( f );
	TestBoundedFamilies( f );
	TestExclusions( f );
	TestTransform( f );
	TestSnapshotFreshnessOnAdd();
	TestTLASStalenessViaJobAddObject();
	TestConventions( f );
	TestBuiltinEndToEnd( f );
	TestSignedLowerBound( f );
	TestExactSigma();
	TestCsgComposites();
	TestInterior( f );
	TestGlassPavilionShowcase();

	f.job->release();

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
