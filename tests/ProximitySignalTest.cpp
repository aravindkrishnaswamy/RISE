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

	// --- CSG: the composite refuses in v1, and its operands never count.
	// `n_csg` is a subtraction whose surface is NOT the min of its
	// operands' under subtraction, so answering from them would be wrong;
	// and `csg_a` / `csg_b` are registered in the manager but are not
	// world-visible, so they are filtered like any other invisible object.
	{
		IObjectPriv* a = f.Obj( "csg_a" );
		IObjectPriv* b = f.Obj( "csg_b" );
		Check( a && b, "(c) the CSG operands are registered in the manager" );
		Check( a && !a->IsWorldVisible(), "(c) ...and operand A is not world-visible" );
		Check( b && !b->IsWorldVisible(), "(c) ...and operand B is not world-visible" );

		const Scalar d = Probe( f, Point3( 96, 0, 0 ), f.floorObj, Scalar( 3 ) );
		Check( d < Scalar( 0 ),
			"(c) MONEY -- a CSG composite refuses in v1 and its operands never count separately" );
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
	Check( proxStillBlind == rayStillBlind,
		"(g2) ...stated as the identity the design promises: proximity is exactly as stale "
		"as the render, never staler and never fresher" );

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

	f.job->release();

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
