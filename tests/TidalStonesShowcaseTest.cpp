//////////////////////////////////////////////////////////////////////
//
//  TidalStonesShowcaseTest.cpp -- the Phase-3 showcase for `interior(r)`
//  (docs/PROXIMITY_SHOWCASES.md section 2), driven against the TRACKED
//  scene scenes/FeatureBased/Textures/tidal_stones.RISEscene.
//
//  Five stones rest in a sand-and-water pool; the water box's
//  `DeepestOtherContainment` is what `interior(0.02)` reads on every
//  stone.  Every station below is DERIVED from the scene's own chunk
//  parameters (never a hard-coded number copied from a render), exactly
//  as MeshClosestPointTest (g) and ProximitySignalTest (l) do it:
//  `Cst::ParseToCst` + `Cst::DeriveToJob` through the real CST loader,
//  `IObjectManager::NearestOtherSurface` / `DeepestOtherContainment` /
//  `SurfaceSignalInfo::Interior` at points computed from the chunks.
//
//  SECTIONS:
//    (a) B1-B5  -- stone_a, a plain sphere half in/out of the water.
//    (b) B6     -- stone_d, buried in dry sand (not water).
//    (c) B7     -- stone_b, an ellipsoid, waterline solved in its own
//                  local frame then rotated by its 20 deg yaw.
//    (d) B8     -- stone_c, an SDF pebble; the surface point comes from
//                  an OBJECT-SPACE bisection of `SDFGeometry::EvaluateParts`.
//    (e) B9     -- stone_e, the tilted flagstone; four target heights on
//                  its top face, solved for the signed offset `s` along
//                  the face's rise direction `t`.
//    (f) The painter-probe harness: a PROBE copy (`expr` -> `vec3(buried,
//        buried, buried)`) and a CONTROL copy (`expr` -> `vec3(1,1,1)`)
//        built in-process from the tracked file via `Cst::DocSetOrAddParamValue`
//        (never `DocSetParamValue`), rendered at 512 spp with
//        `oidn_denoise FALSE` -- comment carries the literal substring
//        `oidn_denoise FALSE` for SourceHygieneTest -- and read back
//        through `Job::RemoveRasterizerOutputs()` +
//        `AddRasterizerOutput` + `Job::Rasterize()`, exactly as
//        `EnvLightBalanceTest` does it.  Stations B1 and the four B9
//        points are read from the spec's OVERHEAD probe camera, which
//        the probe/control copies dial in by re-pointing the shipped
//        `thinlens_camera`'s `location`/`lookat`/`up`/`focus_distance`.
//    (g) The cost gate: live vs `def buried 0` in all three chunks that
//        declare it, at the shipped 32 spp, 2 warm-ups then >= 3
//        interleaved pairs.
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
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <filesystem>

#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Interfaces/SurfaceSignalProximity.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Utilities/ExpressionMemo.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
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

static fs::path FindRepoRoot()
{
	const char* candidates[] = { ".", "..", "../..", "../../.." };
	for( const char* c : candidates ) {
		const fs::path p( c );
		if( fs::exists( p / "scenes" / "FeatureBased" / "Textures" / "tidal_stones.RISEscene" ) ) {
			return p;
		}
	}
	return fs::path();
}

static std::string ReadFile( const fs::path& p )
{
	std::ifstream in( p );
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

struct Scene
{
	Job*			job = 0;
	IObjectManager*	mgr = 0;
};

static bool LoadScene( const fs::path& scenePath, Scene& out, const char* tag )
{
	const std::string text = ReadFile( scenePath );
	if( text.empty() ) return false;

	Cst::Document doc = Cst::ParseToCst( text );
	out.job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *out.job, &diags );
	for( std::size_t i = 0; i < diags.size(); ++i ) {
		std::cout << "  [" << tag << "] scene diagnostic: " << diags[i] << std::endl;
	}
	Check( diags.empty(), std::string( tag ) + ": scene derives with NO diagnostics" );

	out.mgr = out.job->GetObjects();
	if( !out.mgr ) return false;
	out.mgr->PrepareForRendering();
	return true;
}

static Scalar InteriorAt( IObjectManager* mgr, const Point3& p, const IObjectPriv* self, const Scalar r )
{
	SurfaceSignalInfo si;
	si.pScene  = mgr;
	si.pSelf   = self;
	si.ptWorld = p;
	ExpressionMemo::Invalidate();
	return si.Interior( r );
}

//======================================================================
// Live-geometry derivation helpers (2026-09-10 P1 fix).  Every station
// below used to be a TYPED LITERAL that merely equalled the chunk's own
// numbers -- a scene edit that moved a stone within the same slab (same
// containment verdict) would leave the test green while it no longer
// sampled the stone's surface.  These helpers pull the object's centre,
// radius/semi-axes/half-extents and the water/sand "top" height from the
// LIVE object (`GetFinalTransformMatrix()` + the geometry's own
// `GenerateBoundingSphere`/`GenerateBoundingBox`, both public IGeometry
// members), never from a hand-typed number.  `GenerateBoundingBox()` on
// SphereGeometry/EllipsoidGeometry/BoxGeometry each report the EXACT
// object-space extents (verified against the .cpp sources: sphere returns
// (-R,-R,-R)/(R,R,R), ellipsoid returns the exact per-axis semi-axes, box
// returns (-w/2,-h/2,-d/2)/(w/2,h/2,d/2) -- none of the three pad or
// approximate), so no separate accessor is needed on the geometry classes.
//======================================================================

//! World-space centre of an object: the live transform applied to its
//! own local origin.
static Point3 ObjectCentre( const IObjectPriv* obj )
{
	return Point3Ops::Transform( obj->GetFinalTransformMatrix(), Point3( 0, 0, 0 ) );
}

//! Exact radius of a (uniform) sphere, read from the geometry itself --
//! SphereGeometry::GenerateBoundingSphere reports `m_dRadius` verbatim.
static Scalar SphereRadius( const IObjectPriv* obj )
{
	Point3 c; Scalar r = 0;
	obj->GetGeometry()->GenerateBoundingSphere( c, r );
	return r;
}

//! The three object-space semi-axes of an ellipsoid --
//! EllipsoidGeometry::GenerateBoundingBox returns exactly
//! (-m_vRadius, +m_vRadius), so `ur` IS the semi-axis vector.
static Vector3 EllipsoidSemiAxes( const IObjectPriv* obj )
{
	const BoundingBox bb = obj->GetGeometry()->GenerateBoundingBox();
	return Vector3( bb.ur.x, bb.ur.y, bb.ur.z );
}

//! The three object-space half-extents of a box --
//! BoxGeometry::GenerateBoundingBox returns exactly
//! (-w/2,-h/2,-d/2)/(w/2,h/2,d/2), so `ur` IS the half-extent vector.
static Vector3 BoxHalfExtents( const IObjectPriv* obj )
{
	const BoundingBox bb = obj->GetGeometry()->GenerateBoundingBox();
	return Vector3( bb.ur.x, bb.ur.y, bb.ur.z );
}

//! World-space Y of an AXIS-ALIGNED box object's top face (water, sand,
//! bed -- none of them carry a rotation): the live transform applied to
//! the geometry's own local top-face point (0, halfHeight, 0).
static Scalar BoxTopY( const IObjectPriv* obj )
{
	const Vector3 half = BoxHalfExtents( obj );
	const Point3 top = Point3Ops::Transform( obj->GetFinalTransformMatrix(), Point3( 0, half.y, 0 ) );
	return top.y;
}

//! The tilted flagstone's live frame: centre, top-face normal `n` and
//! rise direction `t`, and the top-face centre -- shared by (e)
//! TestStoneE and (f) TestPainterProbe so the two never carry independent
//! copies of the same derivation to drift apart.  `n`/`t` come from
//! transforming the box's own local +Y (top-face normal) and local -X
//! (rise direction, matching `orientation 0 0 -25`'s Z-only yaw) axes by
//! the LIVE matrix -- no hard-coded degree value anywhere, so this holds
//! for whatever orientation the chunk carries, not just -25.
struct FlagstoneFrame
{
	Point3	centre;
	Vector3	n;
	Vector3	t;
	Point3	faceCentre;
	Vector3	halfExtents;	// object-space half (width, height, depth)
};

static FlagstoneFrame ComputeFlagstoneFrame( const IObjectPriv* stoneE )
{
	FlagstoneFrame f;
	const Matrix4 toWorld = stoneE->GetFinalTransformMatrix();
	f.centre = Point3Ops::Transform( toWorld, Point3( 0, 0, 0 ) );
	f.n = Vector3Ops::Transform( toWorld, Vector3( 0, 1, 0 ) );
	f.t = Vector3Ops::Transform( toWorld, Vector3( -1, 0, 0 ) );
	f.halfExtents = BoxHalfExtents( stoneE );
	f.faceCentre = Point3(
		f.centre.x + f.halfExtents.y * f.n.x,
		f.centre.y + f.halfExtents.y * f.n.y,
		f.centre.z + f.halfExtents.y * f.n.z );
	return f;
}

//! A point on a sphere's silhouette in the XZ plane at world height `y`:
//! `dy` is measured from the sphere's own centre (NOT from any container
//! top), so this same formula produces the dry apex (`y = centre.y +
//! radius`), the equator (`y = centre.y`), the bottom (`y = centre.y -
//! radius`) and any partial-depth ring in between -- exactly the family
//! of B1-B6 stations, all from one derivation.
static Point3 SphereStationAtY( const Point3& centre, const Scalar radius, const Scalar y )
{
	const Scalar dy = centre.y - y;
	const Scalar rad2 = radius * radius - dy * dy;
	const Scalar horizontal = ( rad2 > Scalar( 0 ) ) ? std::sqrt( rad2 ) : Scalar( 0 );
	return Point3( centre.x + horizontal, y, centre.z );
}

//======================================================================
// (a) B1-B5 -- stone_a, a plain sphere
//======================================================================

static void TestStoneA( Scene& s )
{
	std::cout << "(a) B1-B5 -- stone_a, a plain sphere half in/out of the water" << std::endl;

	IObjectPriv* stoneA = s.mgr->GetItem( "stone_a" );
	Check( stoneA != 0, "(a) stone_a is present" );
	if( !stoneA ) return;
	IObjectPriv* water = s.mgr->GetItem( "water" );
	Check( water != 0, "(a) water is present" );
	if( !water ) return;

	// Derived from the LIVE objects -- centre + radius from stone_a's own
	// transform/geometry, water top from the water box's own
	// transform/geometry -- never a typed literal that merely equals the
	// chunk's numbers (2026-09-10 P1 fix).
	const Point3 centre = ObjectCentre( stoneA );
	const Scalar radius = SphereRadius( stoneA );
	const Scalar waterTop = BoxTopY( water );
	std::cout << "    stone_a centre=(" << (double)centre.x << "," << (double)centre.y << ","
		<< (double)centre.z << ") radius=" << (double)radius << " waterTop=" << (double)waterTop << std::endl;

	// B1: dry apex -- centre.y + radius, no containment.
	{
		const Point3 p = SphereStationAtY( centre, radius, centre.y + radius );
		const Scalar v = InteriorAt( s.mgr, p, stoneA, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 0 ), Scalar( 1e-9 ), "(a) MONEY B1: stone_a top interior(0.02) = 0" );
	}
	// B2: waterline -- y = waterTop exactly.
	{
		const Point3 p = SphereStationAtY( centre, radius, waterTop );
		const Scalar v = InteriorAt( s.mgr, p, stoneA, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 0 ), Scalar( 1e-9 ), "(a) MONEY B2: stone_a at the waterline interior(0.02) = 0" );
	}
	// B3: 1 cm under the waterline (0.01 is the PROBE's own recipe -- a
	// chosen sample depth, not a scene chunk value) -> 0.5.
	{
		const Point3 p = SphereStationAtY( centre, radius, waterTop - Scalar( 0.01 ) );
		const Scalar v = InteriorAt( s.mgr, p, stoneA, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 0.5 ), Scalar( 1e-9 ), "(a) MONEY B3: stone_a 1 cm under interior(0.02) = 0.5" );
	}
	// B4: 2 cm under the waterline -> 1.0.
	{
		const Point3 p = SphereStationAtY( centre, radius, waterTop - Scalar( 0.02 ) );
		const Scalar v = InteriorAt( s.mgr, p, stoneA, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 1.0 ), Scalar( 1e-9 ), "(a) MONEY B4: stone_a 2 cm under interior(0.02) = 1.0" );
	}
	// B5: bottom -- centre.y - radius -- water top ~0.05 away, bed's own
	// tiny closed-form depth dominated but present in the running max -> 1.0.
	{
		const Point3 p = SphereStationAtY( centre, radius, centre.y - radius );
		const Scalar v = InteriorAt( s.mgr, p, stoneA, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 1.0 ), Scalar( 1e-9 ), "(a) MONEY B5: stone_a bottom interior(0.02) = 1.0" );

		// And the bed's own contribution really is counted, at its tiny
		// closed-form depth -- the header's claim, pinned rather than
		// merely asserted through the (dominated) running max above.
		IObjectPriv* bed = s.mgr->GetItem( "pool_bed" );
		Check( bed != 0, "(a) pool_bed is present" );
		if( bed ) {
			const Scalar wantDepth = waterTop - p.y;
			Scalar depth = 0;
			Check( s.mgr->DeepestOtherContainment( p, stoneA, Scalar( 10 ), depth ),
				"(a) ...the scene-wide containment query answers at B5" );
			CheckClose( depth, wantDepth, Scalar( 1e-6 ),
				"(a) ...with the water's own depth (waterTop - p.y), the running MAXIMUM over every "
				"containing neighbour (the bed's contribution is dominated, not absent)" );
		}
	}
}

//======================================================================
// (b) B6 -- stone_d, buried in dry sand
//======================================================================

static void TestStoneD( Scene& s )
{
	std::cout << "(b) B6 -- stone_d, buried in dry sand (not water)" << std::endl;

	IObjectPriv* stoneD = s.mgr->GetItem( "stone_d" );
	Check( stoneD != 0, "(b) stone_d is present" );
	if( !stoneD ) return;
	IObjectPriv* sandNz = s.mgr->GetItem( "sand_nz" );
	Check( sandNz != 0, "(b) sand_nz is present" );
	if( !sandNz ) return;

	// stone_d was moved 2026-09-10 (P1-3) from (0.26, 0.035, 0.09) on `sand_px`
	// to (0.05, 0.035, -0.22) on the FAR strip `sand_nz` -- the original
	// position projected ~31.0 deg off the beauty camera's view axis against
	// the lens/sensor's 19.8 deg horizontal half-FOV (out of frame); see the
	// scene header's Deviations item 4.  Stations below are the SAME formula
	// (equator / mid-depth / bottom relative to the sphere's own centre and
	// radius) as (a) above, re-derived from the LIVE stone_d/sand_nz objects
	// rather than typed against the current position -- a scene edit that
	// moves the stone within the sand strip stays correct here.
	const Point3 centre = ObjectCentre( stoneD );
	const Scalar radius = SphereRadius( stoneD );
	const Scalar sandTop = BoxTopY( sandNz );
	std::cout << "    stone_d centre=(" << (double)centre.x << "," << (double)centre.y << ","
		<< (double)centre.z << ") radius=" << (double)radius << " sandTop=" << (double)sandTop << std::endl;

	// B6a: equator -- y = centre.y (dy = 0, horizontal = radius) -> 0.
	{
		const Point3 p = SphereStationAtY( centre, radius, centre.y );
		const Scalar v = InteriorAt( s.mgr, p, stoneD, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 0 ), Scalar( 1e-9 ), "(b) MONEY B6a: stone_d equator interior(0.02) = 0" );
	}
	// B6b: mid-depth, 1 cm below the sand top (the PROBE's own recipe, not
	// a scene chunk value) -> 0.5.
	{
		const Point3 p = SphereStationAtY( centre, radius, sandTop - Scalar( 0.01 ) );
		const Scalar v = InteriorAt( s.mgr, p, stoneD, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 0.5 ), Scalar( 1e-9 ), "(b) MONEY B6b: stone_d mid-depth interior(0.02) = 0.5" );
	}
	// B6c: bottom -- centre.y - radius -> 1.0.
	{
		const Point3 p = SphereStationAtY( centre, radius, centre.y - radius );
		const Scalar v = InteriorAt( s.mgr, p, stoneD, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 1.0 ), Scalar( 1e-9 ), "(b) MONEY B6c: stone_d bottom interior(0.02) = 1.0" );
	}
}

//======================================================================
// (c) B7 -- stone_b, an ellipsoid
//======================================================================

static void TestStoneB( Scene& s )
{
	std::cout << "(c) B7 -- stone_b, an ellipsoid (20 deg yaw)" << std::endl;

	IObjectPriv* stoneB = s.mgr->GetItem( "stone_b" );
	Check( stoneB != 0, "(c) stone_b is present" );
	if( !stoneB ) return;
	IObjectPriv* water = s.mgr->GetItem( "water" );
	Check( water != 0, "(c) water is present" );
	if( !water ) return;

	const Matrix4 toWorld = stoneB->GetFinalTransformMatrix();
	const Point3 centre = ObjectCentre( stoneB );
	const Vector3 axes = EllipsoidSemiAxes( stoneB );	// (a, b, c) semi-axes, live from the geometry
	const Scalar waterTop = BoxTopY( water );
	std::cout << "    stone_b centre=(" << (double)centre.x << "," << (double)centre.y << ","
		<< (double)centre.z << ") axes=(" << (double)axes.x << "," << (double)axes.y << ","
		<< (double)axes.z << ") waterTop=" << (double)waterTop << std::endl;

	// B7a: bottom -- local (0,-b,0) transformed by the live matrix -> depth
	// (waterTop - world.y) -> 1.0.
	{
		const Point3 local( 0, -axes.y, 0 );
		const Point3 p = Point3Ops::Transform( toWorld, local );
		const Scalar v = InteriorAt( s.mgr, p, stoneB, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 1.0 ), Scalar( 1e-9 ), "(c) MONEY B7a: stone_b bottom interior(0.02) = 1.0" );
	}
	// B7b: waterline point on the ellipsoid.  The chunk's orientation is
	// `0 20 0` (Y-only yaw), and a rotation about Y leaves the Y basis
	// vector fixed -- checked below rather than assumed -- so world.y =
	// centre.y + localY exactly, with NO dependence on the yaw angle.
	// localY is therefore just `waterTop - centre.y` (solved from the
	// LIVE water top and stone_b centre, not typed from the chunk's
	// numbers).  The local xz ellipse at that height has semi-axes
	// `(a, c) * sqrt(1 - (localY/b)^2)`; take the point at local +x
	// (z = 0), then transform by the live matrix (rotate + translate).
	{
		const Vector3 worldYAxis = Vector3Ops::Transform( toWorld, Vector3( 0, 1, 0 ) );
		std::cout << "    stone_b's local +Y axis maps to world (" << (double)worldYAxis.x << ", "
			<< (double)worldYAxis.y << ", " << (double)worldYAxis.z << ")  (expect (0,1,0))" << std::endl;
		Check( std::fabs( (double)worldYAxis.x ) < 1e-9 && std::fabs( (double)worldYAxis.y - 1.0 ) < 1e-9
				&& std::fabs( (double)worldYAxis.z ) < 1e-9,
			"(c) ...stone_b's orientation is Y-only (rotation preserves the Y axis), so localY maps 1:1 to world Y" );

		const Scalar localY = waterTop - centre.y;
		const Scalar ratio = localY / axes.y;
		const Scalar under = Scalar( 1 ) - ratio * ratio;
		const Scalar aLocal = axes.x * std::sqrt( under > Scalar( 0 ) ? under : Scalar( 0 ) );
		const Point3 local( aLocal, localY, 0 );
		const Point3 world = Point3Ops::Transform( toWorld, local );
		std::cout << "    B7b world point: (" << (double)world.x << ", " << (double)world.y
			<< ", " << (double)world.z << ")  (expect y = waterTop = " << (double)waterTop << ")" << std::endl;
		CheckClose( world.y, waterTop, Scalar( 1e-6 ),
			"(c) ...the rotated/translated point really lands on the waterline" );
		const Scalar v = InteriorAt( s.mgr, world, stoneB, Scalar( 0.02 ) );
		CheckClose( v, Scalar( 0 ), Scalar( 1e-9 ), "(c) MONEY B7b: stone_b waterline interior(0.02) = 0" );
	}
}

//======================================================================
// (d) B8 -- stone_c, an SDF pebble
//======================================================================

//! Bisect `SDFGeometry::EvaluateParts` along OBJECT-space -x, at fixed
//! object y/z, over [xLo, xHi] with f(xLo) > 0 (outside) and f(xHi) < 0
//! (inside) -- exactly the bracket the design's SDF query descends.
static Scalar BisectSDFAlongX( const std::vector<SDFGeometry::Part>& parts,
	Scalar y, Scalar z, Scalar xLo, Scalar xHi, Scalar tol )
{
	Scalar lo = xLo, hi = xHi;
	const Scalar fLo = SDFGeometry::EvaluateParts( parts, Point3( lo, y, z ) );
	const Scalar fHi = SDFGeometry::EvaluateParts( parts, Point3( hi, y, z ) );
	Check( fLo > Scalar( 0 ), "(d) bisection bracket: f(xLo) > 0 (outside)" );
	Check( fHi < Scalar( 0 ), "(d) bisection bracket: f(xHi) < 0 (inside)" );
	for( int i = 0; i < 200 && (double)( hi - lo ) > (double)tol; ++i ) {
		const Scalar mid = ( lo + hi ) * Scalar( 0.5 );
		const Scalar f = SDFGeometry::EvaluateParts( parts, Point3( mid, y, z ) );
		if( f > Scalar( 0 ) ) lo = mid; else hi = mid;
	}
	return ( lo + hi ) * Scalar( 0.5 );
}

static void TestStoneC( Scene& s )
{
	std::cout << "(d) B8 -- stone_c, an SDF pebble (object-space bisection)" << std::endl;

	IObjectPriv* stoneC = s.mgr->GetItem( "stone_c" );
	Check( stoneC != 0, "(d) stone_c is present" );
	if( !stoneC ) return;

	const SDFGeometry* sdf = dynamic_cast<const SDFGeometry*>( stoneC->GetGeometry() );
	Check( sdf != 0, "(d) stone_c's geometry is an SDFGeometry" );
	if( !sdf ) return;

	const Scalar xRoot = BisectSDFAlongX( sdf->GetParts(), Scalar( 0.01 ), Scalar( 0 ),
		Scalar( -0.07 ), Scalar( 0 ), Scalar( 1e-10 ) );
	std::cout << "    B8 object-space root: x = " << (double)xRoot << "  (expect ~ -0.028284)" << std::endl;
	CheckClose( xRoot, Scalar( -0.0282843 ), Scalar( 1e-4 ),
		"(d) ...the bisected root is near the small sphere's own -x surface point" );

	// Transform the object-space bisection root (x = xRoot, the SAME
	// y = 0.01 / z = 0 the bisection itself queried at) by the LIVE
	// transform, rather than typing the chunk's position numbers directly
	// -- stone_c carries no rotation/scale, so this reduces to a translate,
	// but it now tracks a scene edit to `position` automatically.
	const Point3 objectPoint( xRoot, Scalar( 0.01 ), 0 );
	const Point3 world = Point3Ops::Transform( stoneC->GetFinalTransformMatrix(), objectPoint );
	const Point3 centre = ObjectCentre( stoneC );
	std::cout << "    B8 world point: (" << (double)world.x << ", " << (double)world.y
		<< ", " << (double)world.z << ")  (expect y = centre.y + 0.01 = "
		<< (double)( centre.y + Scalar( 0.01 ) ) << ")" << std::endl;
	CheckClose( world.y, centre.y + Scalar( 0.01 ), Scalar( 1e-6 ),
		"(d) ...world y really is the object-space probe's y translated by the live centre" );

	const Scalar v = InteriorAt( s.mgr, world, stoneC, Scalar( 0.02 ) );
	CheckClose( v, Scalar( 0.5 ), Scalar( 1e-9 ), "(d) MONEY B8: stone_c pebble surface interior(0.02) = 0.5" );

	// Feeding WORLD coordinates to the object-space evaluator would land on
	// the waterline and read 0 -- the mistake the spec calls out by name.
	{
		const Scalar fWorldAsObject = SDFGeometry::EvaluateParts( sdf->GetParts(), world );
		std::cout << "    (control) EvaluateParts fed WORLD coords directly: " << (double)fWorldAsObject
			<< "  (should be positive -- outside; a worker who skips the -position translate "
			<< "would land on the waterline, not the pebble)" << std::endl;
	}
}

//======================================================================
// (e) B9 -- stone_e, the tilted flagstone
//======================================================================

static void TestStoneE( Scene& s )
{
	std::cout << "(e) B9 -- stone_e, the tilted flagstone (orientation 0 0 -25)" << std::endl;

	IObjectPriv* stoneE = s.mgr->GetItem( "stone_e" );
	Check( stoneE != 0, "(e) stone_e is present" );
	if( !stoneE ) return;
	IObjectPriv* bed = s.mgr->GetItem( "pool_bed" );
	Check( bed != 0, "(e) pool_bed is present" );
	if( !bed ) return;

	const FlagstoneFrame f = ComputeFlagstoneFrame( stoneE );
	std::cout.precision( 12 );
	std::cout << "    stone_e centre=(" << (double)f.centre.x << "," << (double)f.centre.y << ","
		<< (double)f.centre.z << ") n=(" << (double)f.n.x << "," << (double)f.n.y << "," << (double)f.n.z
		<< ") t=(" << (double)f.t.x << "," << (double)f.t.y << "," << (double)f.t.z << ")" << std::endl;

	// Sanity print/check ONLY (never the source of a coordinate below): the
	// resting-height model -- bed top + halfWidth*sin(angle) +
	// halfHeight*cos(angle), with `angle` itself read back out of the live
	// normal `n` (atan2, no hard-coded degree literal) -- predicts the
	// SAME centre.y the live transform already reports.
	{
		const Scalar bedTop = BoxTopY( bed );
		const Scalar angle = std::atan2( f.n.x, f.n.y );
		const Scalar predictedCy = bedTop + f.halfExtents.x * std::sin( angle ) + f.halfExtents.y * std::cos( angle );
		std::cout << "    resting-height model predicts centre y = " << (double)predictedCy
			<< "  (live centre y = " << (double)f.centre.y << ")" << std::endl;
		CheckClose( predictedCy, f.centre.y, Scalar( 1e-8 ),
			"(e) sanity: the algebraic resting-height model matches the live centre y" );
	}
	std::cout << "    face centre = (" << (double)f.faceCentre.x << ", " << (double)f.faceCentre.y
		<< ", " << (double)f.faceCentre.z << ")" << std::endl;

	const Scalar targets[4] = { Scalar( 0.0 ), Scalar( 0.01 ), Scalar( 0.02 ), Scalar( 0.035 ) };
	const Scalar wantDepth[4] = { Scalar( 0.03 ), Scalar( 0.02 ), Scalar( 0.01 ), Scalar( -0.005 ) };
	const Scalar wantInterior[4] = { Scalar( 1.0 ), Scalar( 1.0 ), Scalar( 0.5 ), Scalar( 0.0 ) };
	Point3 b9world[4];

	for( int i = 0; i < 4; ++i ) {
		const Scalar s_ = ( targets[i] - f.faceCentre.y ) / f.t.y;
		const Point3 world( f.faceCentre.x + s_ * f.t.x, f.faceCentre.y + s_ * f.t.y, f.faceCentre.z + s_ * f.t.z );
		b9world[i] = world;
		CheckClose( world.y, targets[i], Scalar( 1e-9 ), "(e) B9 point lands on the target height" );
		Check( std::fabs( (double)s_ ) <= (double)f.halfExtents.x + 1e-9, "(e) ...within the half-length" );

		const Scalar v = InteriorAt( s.mgr, world, stoneE, Scalar( 0.02 ) );
		std::cout << "    B9 target y=" << (double)targets[i] << ": s=" << (double)s_
			<< " world=(" << (double)world.x << "," << (double)world.y << "," << (double)world.z
			<< ") interior=" << (double)v << "  (want " << (double)wantInterior[i] << ")" << std::endl;
		CheckClose( v, wantInterior[i], Scalar( 1e-9 ),
			"(e) MONEY B9: stone_e top face interior(0.02) matches its target depth" );

		// And the depth itself, via the containment query directly.
		Scalar depth = 0;
		const bool inside = s.mgr->DeepestOtherContainment( world, stoneE, Scalar( 10 ), depth );
		if( wantDepth[i] > Scalar( 0 ) ) {
			Check( inside, "(e) ...the point really is inside the water (positive depth)" );
			if( inside ) CheckClose( depth, wantDepth[i], Scalar( 1e-6 ), "(e) ...at the predicted depth" );
		}
		else {
			Check( !inside, "(e) ...the driest station is NOT inside the water" );
		}
	}
}

//======================================================================
// (f) The painter-probe harness -- PROBE (`vec3(buried,buried,buried)`)
// vs CONTROL (`vec3(1,1,1)`), 512 spp, `oidn_denoise FALSE` (the probe
// copies force it off -- SourceHygieneTest's literal, next to the call
// below).
//======================================================================

//! Same shape as EnvLightBalanceTest's CapturingRasterizerOutput: stores
//! the linear radiance buffer for in-test pixel reads, no sRGB.
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width( 0 ), height( 0 ) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage( const IRasterImage& pImage, const Rect*, const unsigned int ) override
	{
		width = pImage.GetWidth();
		height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = pImage.GetPEL( x, y );
			}
		}
	}
};

//! First top-level Chunk item in a throwaway single-chunk Document --
//! the precedent's own extraction (`Job::ApplyCstInsertChunk`), scanning
//! rather than assuming index 0 so leading trivia can never misalign it.
static Cst::NodeRef FirstChunkItem( const Cst::Document& d )
{
	const int n = Cst::DocItemCount( d );
	for( int i = 0; i < n; ++i ) {
		Cst::NodeRef it = Cst::DocResolveNodeId( d, Cst::DocNodeIdAt( d, i ) );
		if( it && it->kind == Cst::NodeKind::Chunk ) return it;
	}
	return Cst::NodeRef();
}

static Cst::NodeRef NewlineTrivia()
{
	Cst::Document d = Cst::ParseToCst( std::string( "\n" ) );
	return Cst::DocResolveNodeId( d, Cst::DocNodeIdAt( d, 0 ) );
}

//! The three-leaf insert `[leadSep][chunk][trailSep]` at `atIndex`,
//! `atIndex+1`, `atIndex+2` -- `Job::ApplyCstInsertCameraChunk` /
//! `Job::ApplyCstInsertChunk`'s own idiom, unconditionally, so a bare
//! chunk can never glue its `}` onto the next keyword.
static Cst::Document InsertChunkThreeLeaf( const Cst::Document& doc, int atIndex, const std::string& chunkText )
{
	Cst::NodeRef chunkItem = FirstChunkItem( Cst::ParseToCst( chunkText ) );
	Cst::NodeRef lead  = NewlineTrivia();
	Cst::NodeRef trail = NewlineTrivia();
	Cst::Document d = doc;
	d = Cst::DocInsertItem( d, atIndex,     lead );
	d = Cst::DocInsertItem( d, atIndex + 1, chunkItem );
	d = Cst::DocInsertItem( d, atIndex + 2, trail );
	return d;
}

//! `DocFindByName` -> `DocIndexOfNodeId`, both asserted with the RIGHT
//! sentinel (0 absent for the former, -1 absent for the latter -- 0 is a
//! LEGAL index).
static int IndexOfName( const Cst::Document& doc, const std::string& namePath, Cst::NodeId& outId )
{
	outId = Cst::DocFindByName( doc, namePath );
	Check( outId > 0, "IndexOfName: `" + namePath + "` found" );
	if( outId <= 0 ) return -1;
	const int idx = Cst::DocIndexOfNodeId( doc, outId, nullptr );
	Check( idx >= 0, "IndexOfName: `" + namePath + "` has a resolvable index" );
	return idx;
}

//! Builds the PROBE (`isProbe` true) or CONTROL (`isProbe` false) copy
//! of the tracked scene text, entirely through `Cst::DocSetOrAddParamValue`
//! / `DocInsertItem` (never `DocSetParamValue`, which silently no-ops on
//! an absent parameter).  Both copies: `expr` swap, the overhead-probe
//! camera re-point, the receivers' Lambertian rebind, the black-out of
//! every OTHER object (water excepted -- it keeps its dielectric), and
//! the two rasterizer setters -- `oidn_denoise FALSE` next to this call
//! is the literal SourceHygieneTest's `oidn_denoise false` search wants.
static Cst::Document BuildVariant( const std::string& trackedText, bool isProbe, const char* tag )
{
	Cst::Document doc = Cst::ParseToCst( trackedText );

	// 1. `expr` swap on the shared `expr_stones` painter.
	Cst::NodeId exprId = Cst::DocFindByName( doc, "expression_painter/expr_stones" );
	Check( exprId > 0, std::string( tag ) + ": expr_stones chunk found" );
	doc = Cst::DocSetOrAddParamValue( doc, exprId, "expr", 0,
		isProbe ? "vec3(buried, buried, buried)" : "vec3(1, 1, 1)" );

	// 2. The rasterizer chunk is UNNAMED -- find it by role (ends in
	// `_rasterizer`), not by name.  oidn_denoise FALSE (SourceHygieneTest's
	// literal substring is right there) + samples 512.
	Cst::NodeId rastId = 0;
	{
		const int n = Cst::DocItemCount( doc );
		for( int i = 0; i < n; ++i ) {
			const Cst::NodeId id = Cst::DocNodeIdAt( doc, i );
			Cst::NodeRef it = Cst::DocResolveNodeId( doc, id );
			if( it && it->kind == Cst::NodeKind::Chunk ) {
				const std::string& role = it->role;
				if( role.size() > 11 && role.compare( role.size() - 11, 11, "_rasterizer" ) == 0 ) {
					rastId = id;
					break;
				}
			}
		}
	}
	Check( rastId > 0, std::string( tag ) + ": rasterizer chunk found by role" );
	doc = Cst::DocSetOrAddParamValue( doc, rastId, "oidn_denoise", 0, "FALSE" ); // oidn_denoise FALSE
	doc = Cst::DocSetOrAddParamValue( doc, rastId, "samples", 0, "512" );

	// 3. Overhead probe camera -- re-point the shipped `thinlens_camera`
	// (location/lookat/up/focus_distance); the f/22 and focal length stay.
	Cst::NodeId camId = Cst::DocFindByName( doc, "thinlens_camera/beauty_cam" );
	Check( camId > 0, std::string( tag ) + ": beauty_cam found" );
	doc = Cst::DocSetOrAddParamValue( doc, camId, "location",       0, "-0.016 0.60 -0.10" );
	doc = Cst::DocSetOrAddParamValue( doc, camId, "lookat",         0, "-0.016 0.0 -0.10" );
	doc = Cst::DocSetOrAddParamValue( doc, camId, "up",             0, "0 0 -1" );
	doc = Cst::DocSetOrAddParamValue( doc, camId, "focus_distance", 0, "0.58" );

	// 4. Rebind the two receivers (stone_a, stone_e) to ONE inserted
	// Lambertian naming the shared expr_stones painter -- inserted before
	// the first of the two (stone_a).
	Cst::NodeId stoneAId = 0;
	const int stoneAIdx = IndexOfName( doc, "standard_object/stone_a", stoneAId );
	if( stoneAIdx >= 0 ) {
		const std::string lamChunk = "lambertian_material\n{\n\tname probe_stone\n\treflectance expr_stones\n}";
		doc = InsertChunkThreeLeaf( doc, stoneAIdx, lamChunk );
	}
	// Re-resolve by NAME after the insert (NodeId survives; index shifted).
	stoneAId = Cst::DocFindByName( doc, "standard_object/stone_a" );
	Check( stoneAId > 0, std::string( tag ) + ": stone_a re-findable after insert" );
	doc = Cst::DocSetOrAddParamValue( doc, stoneAId, "material", 0, "probe_stone" );
	Cst::NodeId stoneEId = Cst::DocFindByName( doc, "standard_object/stone_e" );
	Check( stoneEId > 0, std::string( tag ) + ": stone_e found" );
	doc = Cst::DocSetOrAddParamValue( doc, stoneEId, "material", 0, "probe_stone" );

	// 5. Black-out every OTHER object (water excepted) -- inserted before
	// the first non-receiver object in document order (pool_bed).
	Cst::NodeId poolBedId = 0;
	const int poolBedIdx = IndexOfName( doc, "standard_object/pool_bed", poolBedId );
	if( poolBedIdx >= 0 ) {
		const std::string blackChunk = "lambertian_material\n{\n\tname probe_black\n}";
		doc = InsertChunkThreeLeaf( doc, poolBedIdx, blackChunk );
	}
	static const char* kBlackout[] = {
		"pool_bed", "sand_px", "sand_nx", "sand_pz", "sand_nz", "stone_b", "stone_c", "stone_d" };
	for( const char* nm : kBlackout ) {
		const Cst::NodeId id = Cst::DocFindByName( doc, std::string( "standard_object/" ) + nm );
		Check( id > 0, std::string( tag ) + ": blackout target found: " + nm );
		if( id > 0 ) doc = Cst::DocSetOrAddParamValue( doc, id, "material", 0, "probe_black" );
	}

	return doc;
}

static bool RenderVariant( const Cst::Document& doc, CapturingRasterizerOutput** outCap, const char* tag )
{
	Job* job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *job, &diags );
	for( std::size_t i = 0; i < diags.size(); ++i ) {
		std::cout << "  [" << tag << "] scene diagnostic: " << diags[i] << std::endl;
	}
	Check( diags.empty(), std::string( tag ) + ": probe/control copy derives with NO diagnostics" );
	if( !diags.empty() ) { job->release(); return false; }

	job->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
	job->GetRasterizer()->AddRasterizerOutput( cap );
	const bool rendered = job->Rasterize();
	Check( rendered, std::string( tag ) + ": Job::Rasterize() succeeded" );
	*outCap = cap;
	job->release();
	return rendered;
}

//! Projects a world point to the OVERHEAD probe camera's pixel
//! coordinates via the CHIEF-ray pinhole formula (the thin lens only
//! blurs AROUND the chief ray; it does not shift it), using the SAME
//! basis `CameraTransforms::SafeUnitUp_` derives for this camera: with
//! `up 0 0 -1` (non-degenerate here -- perpendicular to the straight-down
//! view, unlike the default `0 1 0`), forward = (0,-1,0), right = (1,0,0),
//! actualUp = (0,0,-1).  Verified against the header's own stated angles
//! (B9's y=0.02 station at 0.13 deg / ~3 px, the deepest B9 station at
//! 4.0 deg) before being trusted here.
struct PixelCoord { double x; double y; };

static PixelCoord ProjectOverhead( const Point3& p )
{
	const Point3 loc( -0.016, 0.60, -0.10 );
	const Vector3 forward( 0, -1, 0 );
	const Vector3 right( 1, 0, 0 );
	const Vector3 up( 0, 0, -1 );
	const Vector3 delta = Vector3Ops::mkVector3( p, loc ); // p - loc
	const double zView = (double)Vector3Ops::Dot( delta, forward );
	const double xView = (double)Vector3Ops::Dot( delta, right );
	const double yView = (double)Vector3Ops::Dot( delta, up );

	const double focal = 50.0;      // mm
	const double sensorW = 36.0;    // mm
	const double sensorH = sensorW * 600.0 / 800.0;

	const double xNdc = ( xView / zView ) * ( focal / ( sensorW * 0.5 ) );
	const double yNdc = ( yView / zView ) * ( focal / ( sensorH * 0.5 ) );

	PixelCoord out;
	out.x = ( xNdc * 0.5 + 0.5 ) * 800.0;
	out.y = ( 1.0 - ( yNdc * 0.5 + 0.5 ) ) * 600.0; // row 0 = top
	return out;
}

//! The showcase-2 sightline MARCH (spec §0): front faces only, restart
//! from the water's ENTRY point on a hit named "water" (recomputing the
//! remaining distance from the NEW origin -- never decrementing by
//! range), any other nearer hit is an occlusion, 4 hops exhausted is an
//! occlusion.  `outHops` is the number of ray casts consumed to reach a
//! VISIBLE verdict (1 + the loop's `hop` index at the return) -- spec §0:
//! ONE for a station above the waterline (its ray reaches the station
//! before the water), TWO for a submerged one (the water's entry, then
//! the station).
static bool StationVisibleOverhead( IObjectManager* mgr, IObjectPriv* water,
	const Point3& origin, const Point3& station, std::string& why, int& outHops )
{
	Point3 cur = origin;
	for( int hop = 0; hop < 4; ++hop ) {
		const Vector3 u = Vector3Ops::Normalize( Vector3Ops::mkVector3( station, cur ) );
		RayIntersection ri( Ray( cur, u ), nullRasterizerState );
		mgr->IntersectRay( ri, true, false, false );
		const Scalar dist = Vector3Ops::Magnitude( Vector3Ops::mkVector3( station, cur ) );
		if( !ri.geometric.bHit || ri.geometric.range >= dist - Scalar( 1e-6 ) ) {
			outHops = hop + 1;
			return true; // VISIBLE
		}
		if( ri.pObject == water ) {
			cur = Point3Ops::mkPoint3( ri.geometric.ptIntersection, u * Scalar( 1e-6 ) );
			continue;
		}
		why = "occluded by another (nearer) object";
		outHops = hop + 1;
		return false;
	}
	why = "march exhausted 4 hops";
	outHops = 4;
	return false;
}

struct StationSpec
{
	const char*	label;
	Point3		world;
	Scalar		wantRatio;
	Scalar		band;
	int			expectHops;	// 1 above the waterline, 2 submerged (spec §0)
};

static void TestPainterProbe( Scene& s, const fs::path& scenePath )
{
	std::cout << "(f) the painter-probe harness (overhead camera, 512 spp)" << std::endl;

	IObjectPriv* water = s.mgr->GetItem( "water" );
	Check( water != 0, "(f) water object found for the sightline march" );
	IObjectPriv* stoneA = s.mgr->GetItem( "stone_a" );
	Check( stoneA != 0, "(f) stone_a is present" );
	IObjectPriv* stoneE = s.mgr->GetItem( "stone_e" );
	Check( stoneE != 0, "(f) stone_e is present" );
	if( !water || !stoneA || !stoneE ) return;

	// The five stations the overhead camera reads: B1 (dry) and the four
	// B9 points, re-derived from the SAME live-object helpers/frame as (a)
	// and (e) -- `ComputeFlagstoneFrame` is the single source shared by
	// both, rather than an independent hand-typed copy of the formula.
	const Point3 centreA = ObjectCentre( stoneA );
	const Scalar radiusA = SphereRadius( stoneA );
	const FlagstoneFrame f = ComputeFlagstoneFrame( stoneE );

	std::vector<StationSpec> stations;
	// B1 is above the waterline (dry) -> 1 hop.
	{
		const Point3 b1 = SphereStationAtY( centreA, radiusA, centreA.y + radiusA );
		stations.push_back( { "B1 (stone_a dry top)", b1, Scalar( 0 ), Scalar( 0.08 ), 1 } );
	}
	const Scalar targets[4]  = { Scalar( 0.0 ), Scalar( 0.01 ), Scalar( 0.02 ), Scalar( 0.035 ) };
	const Scalar want[4]     = { Scalar( 1.0 ), Scalar( 1.0 ), Scalar( 0.43 ), Scalar( 0.0 ) };
	const char* labels[4] = { "B9a (y=0.0)", "B9b (y=0.01)", "B9c (y=0.02)", "B9d (y=0.035)" };
	for( int i = 0; i < 4; ++i ) {
		const Scalar sOff = ( targets[i] - f.faceCentre.y ) / f.t.y;
		const Point3 world( f.faceCentre.x + sOff * f.t.x, f.faceCentre.y + sOff * f.t.y, f.faceCentre.z + sOff * f.t.z );
		StationSpec sp;
		sp.label = labels[i]; sp.world = world; sp.wantRatio = want[i];
		sp.band = ( i == 2 ) ? Scalar( 0.15 ) : Scalar( 0.08 );
		// B9d (y=0.035) is the dry station -> 1 hop; B9a/B9b/B9c are
		// submerged (target y <= 0.02, all below the y=0.03 waterline) ->
		// 2 hops (the water's entry, then the station).
		sp.expectHops = ( i == 3 ) ? 1 : 2;
		stations.push_back( sp );
	}

	// Occlusion, checked against the TRACKED scene's geometry (identical
	// to the probe/control copies -- only materials differ).  Also assert
	// the hop count itself (spec §0): 1 above the waterline, 2 submerged.
	const Point3 camLoc( -0.016, 0.60, -0.10 );
	for( const StationSpec& sp : stations ) {
		std::string why;
		int hops = 0;
		const bool vis = StationVisibleOverhead( s.mgr, water, camLoc, sp.world, why, hops );
		std::cout << "    " << sp.label << " visible=" << ( vis ? "yes" : "no" )
			<< " hops=" << hops << ( vis ? "" : ( " (" + why + ")" ) ) << std::endl;
		Check( vis, std::string( "(f) sightline to " ) + sp.label + " is unoccluded from the overhead camera" );
		if( vis ) {
			Check( hops == sp.expectHops,
				std::string( "(f) " ) + sp.label + " hop count matches the march (1 above the waterline, 2 submerged)" );
		}
	}

	// Build + render the probe and control copies.
	const std::string trackedText = ReadFile( scenePath );
	Check( !trackedText.empty(), "(f) tracked scene text read for the probe harness" );
	if( trackedText.empty() ) return;

	Cst::Document probeDoc   = BuildVariant( trackedText, true,  "probe" );
	Cst::Document controlDoc = BuildVariant( trackedText, false, "control" );

	CapturingRasterizerOutput* probeCap = 0;
	CapturingRasterizerOutput* controlCap = 0;
	const bool probeOk   = RenderVariant( probeDoc, &probeCap, "probe" );
	const bool controlOk = RenderVariant( controlDoc, &controlCap, "control" );
	if( !probeOk || !controlOk ) {
		if( probeCap ) probeCap->release();
		if( controlCap ) controlCap->release();
		return;
	}

	for( const StationSpec& sp : stations ) {
		const PixelCoord pc = ProjectOverhead( sp.world );
		const int px = (int)std::lround( pc.x );
		const int py = (int)std::lround( pc.y );
		std::cout << "    " << sp.label << " projects to pixel (" << px << ", " << py << ")" << std::endl;
		Check( px >= 0 && px < (int)probeCap->width && py >= 0 && py < (int)probeCap->height,
			std::string( "(f) " ) + sp.label + " projects inside the frame" );
		if( px < 0 || px >= (int)probeCap->width || py < 0 || py >= (int)probeCap->height ) continue;

		const RISEColor& probePel   = probeCap->pixels[py * probeCap->width + px];
		const RISEColor& controlPel = controlCap->pixels[py * controlCap->width + px];
		std::cout << "      probe   r=" << probePel.base.r << std::endl;
		std::cout << "      control r=" << controlPel.base.r << std::endl;
		Check( controlPel.base.r > 0.0, std::string( "(f) " ) + sp.label + " control pixel is non-zero (asserted before dividing)" );
		if( controlPel.base.r <= 0.0 ) continue;

		const double ratio = probePel.base.r / controlPel.base.r;
		std::cout << "      MEASURED ratio = " << ratio << "  (want " << (double)sp.wantRatio
			<< " +/- " << (double)sp.band << ")" << std::endl;
		CheckClose( Scalar( ratio ), sp.wantRatio, sp.band,
			std::string( "(f) MONEY: " ) + sp.label + " painter-probe ratio" );
	}

	probeCap->release();
	controlCap->release();
}

//======================================================================
// (g) The cost gate -- live vs `def buried 0` in all three chunks, 32 spp.
//======================================================================

static double RenderWallClockSeconds( const std::string& sceneText )
{
	Cst::Document doc = Cst::ParseToCst( sceneText );
	Job* job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *job, &diags );
	if( !diags.empty() ) { job->release(); return -1.0; }
	job->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
	job->GetRasterizer()->AddRasterizerOutput( cap );

	const auto t0 = std::chrono::steady_clock::now();
	const bool ok = job->Rasterize();
	const auto t1 = std::chrono::steady_clock::now();

	cap->release();
	job->release();
	if( !ok ) return -1.0;
	return std::chrono::duration<double>( t1 - t0 ).count();
}

static std::string ReplaceAll( std::string s, const std::string& from, const std::string& to )
{
	std::size_t pos = 0;
	while( ( pos = s.find( from, pos ) ) != std::string::npos ) {
		s.replace( pos, from.length(), to );
		pos += to.length();
	}
	return s;
}

static void TestCostGate( const std::string& trackedText )
{
	std::cout << "(g) cost gate -- live vs `def buried 0` in all three chunks (32 spp)" << std::endl;

	// The no-signal baseline: `def buried interior(0.02)` -> `def buried 0`
	// in EACH of the three chunks that declare it.  The expression VM has
	// no constant folding, so the rest of each program still executes --
	// the delta isolates the signal call + its L1 memo lookup.
	const std::string noSignalText = ReplaceAll( trackedText, "interior(0.02)", "0" );
	// `interior(0.02)` also appears in the header's prose (the DOCTRINE
	// paragraph, the station-table caption, the "ONE FIELD DRIVES THREE
	// SLOTS" comment) -- ReplaceAll touches those too, harmlessly (they are
	// `#` comments). What actually matters is that exactly the THREE real
	// `def buried interior(0.02)` lines (expr_stones, sp_stone_rough,
	// sp_stone_coat) became `def buried 0` -- count the exact def-line
	// pattern rather than a loose "text differs" check that comment-only
	// edits would also satisfy.
	{
		const std::string defPattern = "def\t\t\tburied  0";
		std::size_t count = 0, pos = 0;
		while( ( pos = noSignalText.find( defPattern, pos ) ) != std::string::npos ) { ++count; pos += defPattern.length(); }
		std::cout << "    no-signal `def buried 0` line count = " << count << " (want 3)" << std::endl;
		Check( count == 3, "(g) the no-signal text replaced exactly the 3 `def buried interior(0.02)` lines" );
	}

	std::vector<double> liveRuns, noSigRuns;
	// 2 warm-ups (discarded) then >= 3 interleaved pairs.
	for( int i = 0; i < 2; ++i ) { RenderWallClockSeconds( trackedText ); RenderWallClockSeconds( noSignalText ); }
	for( int i = 0; i < 3; ++i ) {
		const double live  = RenderWallClockSeconds( trackedText );
		const double noSig = RenderWallClockSeconds( noSignalText );
		std::cout << "    pair " << i << ": live=" << live << "s  no-signal=" << noSig << "s"
			<< "  ratio=" << ( noSig > 0 ? live / noSig : -1.0 ) << std::endl;
		if( live > 0 && noSig > 0 ) { liveRuns.push_back( live ); noSigRuns.push_back( noSig ); }
	}

	Check( liveRuns.size() >= 3, "(g) at least 3 interleaved pairs rendered successfully" );
	if( liveRuns.size() < 3 ) return;

	double liveMean = 0, noSigMean = 0;
	for( double v : liveRuns ) liveMean += v;
	for( double v : noSigRuns ) noSigMean += v;
	liveMean /= liveRuns.size();
	noSigMean /= noSigRuns.size();
	const double ratio = liveMean / noSigMean;
	std::cout << "    MEASURED: live mean = " << liveMean << "s, no-signal mean = " << noSigMean
		<< "s, ratio = " << ratio << "  (target <= 1.15x)" << std::endl;
	Check( ratio <= 1.15, "(g) cost ratio is within the spec's 1.15x target "
		"(recorded exactly in the scene header/report)" );
}

int main()
{
	std::cout << "=== TidalStonesShowcaseTest (interior(r) showcase, Phase 3) ===" << std::endl;

	const fs::path root = FindRepoRoot();
	if( root.empty() ) {
		std::cout << "  FAIL: could not find repo root" << std::endl;
		return 1;
	}
	const fs::path scenePath = root / "scenes" / "FeatureBased" / "Textures" / "tidal_stones.RISEscene";

	Scene s;
	if( !LoadScene( scenePath, s, "query" ) ) {
		std::cout << "  FAIL: could not load tidal_stones.RISEscene" << std::endl;
		return 1;
	}

	TestStoneA( s );
	TestStoneD( s );
	TestStoneB( s );
	TestStoneC( s );
	TestStoneE( s );
	TestPainterProbe( s, scenePath );

	s.job->release();

	if( const char* skipCost = std::getenv( "TIDAL_SKIP_COST_GATE" ) ) {
		(void)skipCost;
		std::cout << "(g) cost gate SKIPPED (TIDAL_SKIP_COST_GATE set)" << std::endl;
	}
	else {
		TestCostGate( ReadFile( scenePath ) );
	}

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
