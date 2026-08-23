//////////////////////////////////////////////////////////////////////
//
//  SkeletonGeometryChunkTest.cpp - Contract test for the
//  `skeleton_geometry` chunk (C1, docs/agentic-redesign/85-geometry-
//  expressiveness-candidates.md): a JOINT GRAPH that expands, at PARSE
//  TIME, into a single `sdf_geometry` -- one `roundcone` part per bone
//  (parent -> child), smin-blended, registered under the chunk's OWN
//  `name` (unlike shape_light/rect_light's `__geo`-suffixed expansion).
//
//  WHAT THIS TEST OWNS:
//
//    1. EXPANSION + NAMING.  A joint chain parses and lands in the
//       geometry manager under the CHUNK'S OWN NAME -- with no stray
//       `<name>__geo` entry (that suffix convention belongs to
//       shape_light/rect_light, not this chunk; see the struct-level
//       comment in ChunkParserRegistry.cpp for why the own-name
//       convention matters for CST incremental apply/remove).
//
//    2. POSE MATH, FROM FIRST PRINCIPLES -- the highest-value section.
//       A roundcone's spherical END CAPS sit EXACTLY at its base (parent
//       joint) and tip (child joint) positions, with exactly their
//       declared radii, REGARDLESS of any orientation math -- the BASE
//       cap because it is literally the part's untransformed `pos`, and
//       the TIP cap because it is `pos + c * (rotated local +Y)`, which
//       equals the intended child position if and ONLY IF the chunk's
//       direction-to-Euler-degrees conversion is correct.  So: cast rays
//       at the KNOWN, ground-truth parent/child WORLD positions (never
//       recomputed through the chunk's own rotation math) from many
//       approach angles within the geometrically safe cap region, and
//       assert each hits at exactly (approach distance - radius).  A
//       swapped or sign-flipped Euler formula moves the TIP cap away
//       from where the joint was declared, so these rays would miss or
//       hit at the wrong range -- exactly what get red-proved below by
//       temporarily breaking the formula, observing the failure, and
//       reverting (see the report for the transcript).
//
//    3. BLEND SEMANTICS, CLOSED-FORM.  SDFGeometry::Map folds parts
//       sequentially: `d = sminP(d, partEval(part), part.k)`.  At a
//       point exactly on the ray leaving a joint AWAY from both of its
//       incident bones (perpendicular to one, past the tip of the
//       other), both bones' individual SDF values are IDENTICAL by
//       construction, so Quilez's polynomial smin (`h = max(k-|a-b|,0)/
//       k`, here always 1) reduces to a CONSTANT offset of exactly
//       `k/4` closer to the joint than a hard union -- an exact,
//       closed-form prediction, not just a qualitative "it differs"
//       check.  `k` uses the LAST-folded bone's own
//       `blend * min(parent radius, child radius)`, so this also proves
//       the "multiplies the SMALLER radius" claim quantitatively.
//
//    4. ISOLATED JOINT.  A single joint with no parent and no children
//       gets an explicit `sphere` part (otherwise invisible) -- checked
//       via its bounding box and area.
//
//    4c. ASPECT (doc 90 slice B, the optional 7th `joint` token).
//       Back-compat by an EXACT ray-range digest against the pre-token
//       expansion; the axis convention and the pure-squash mapping by a
//       closed-form ellipse probe that separates it from the
//       area-preserving rival by a factor of ~1.4; the untouched axial
//       extent; and no tunneling when a squashed bone is smin-blended
//       against a round one.  See that section's own comment block.
//
//    5. REJECTIONS.  One change at a time from a known-good 3-joint
//       baseline: no joints; malformed joint lines (wrong token count,
//       non-numeric radius, trailing garbage); duplicate joint name;
//       unknown parent; forward-referenced parent; self-parent; a
//       non-positive radius; a coincident parent/child position; and a
//       negative `blend`.
//
//    6. ROUND-TRIP.  The retained CST Document serializes back
//       byte-identically, still containing the SHORT `skeleton_geometry`
//       chunk and NOT the expanded `sdf_geometry` parts.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#ifdef _WIN32
	#include <process.h>
	#include <io.h>
	#define getpid _getpid
	// NOT `#define close _close`: this file also calls std::ofstream::close()
	// (the macro would rewrite it).  Prefixed names, matching
	// BDPTPhantomStrategyWeightTest.cpp's convention.
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>			// getpid(), dup(), dup2(), close()
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Geometry/SDFGeometry.h"		// F3 guard -- builds hand-authored
														// reference/spurious variants directly

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

namespace {

//////////////////////////////////////////////////////////////////////
// Scene plumbing -- ShapeLightChunkTest's pattern, unchanged.
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_skeleton_geo_" + tag + "_" + pid + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << body;
	f.close();
	return path;
}

bool ParseBodyInto( const std::string& tag, const std::string& body, IJobPriv& job )
{
	const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );
	const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );
	remove( path.c_str() );
	return ok;
}

bool ParseBody( const std::string& tag, const std::string& body )
{
	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) return false;
	const bool ok = ParseBodyInto( tag, body, *job );
	safe_release( job );
	return ok;
}

//////////////////////////////////////////////////////////////////////
// Scene fragments
//////////////////////////////////////////////////////////////////////

//! Builds a `skeleton_geometry` chunk from raw `joint` line bodies
//! (each already `<name> <parent|none> <x> <y> <z> <radius> [aspect]`).
//! `blend` is omitted (default 0.35) when null.
std::string SkeletonChunk( const char* name, const std::vector<std::string>& joints,
                           const char* blend = 0 )
{
	std::string s = "skeleton_geometry\n{\n";
	if( name ) { s += "\tname\t"; s += name; s += "\n"; }
	for( std::size_t i = 0; i < joints.size(); ++i ) {
		s += "\tjoint\t"; s += joints[i]; s += "\n";
	}
	if( blend ) { s += "\tblend\t"; s += blend; s += "\n"; }
	s += "}\n";
	return s;
}

//! A well-formed 3-joint chain -- the shared REJECTION baseline and the
//! fixture for the naming test.
std::vector<std::string> GoodChain()
{
	std::vector<std::string> j;
	j.push_back( "root none 0 0 0 0.5" );
	j.push_back( "mid root 1 1 0 0.4" );
	j.push_back( "tip mid 2 2 0 0.2" );
	return j;
}

static bool IsClose( double a, double b, double eps ) { return std::fabs(a-b) <= eps; }

static RayIntersectionGeometric MkRI( const Point3& o, const Vector3& d )
{
	return RayIntersectionGeometric( Ray(o,d), nullRasterizerState );
}

//! Any unit vector perpendicular to `d` (Gram-Schmidt off a reference
//! axis chosen to avoid near-parallel degeneracy).
static Vector3 PerpBasis( const Vector3& d )
{
	Vector3 ref( 0, 1, 0 );
	if( std::fabs( Vector3Ops::Dot( d, ref ) ) > 0.9 ) ref = Vector3( 1, 0, 0 );
	return Vector3Ops::Normalize( Vector3Ops::Cross( d, ref ) );
}

//! Test-side MIRROR of ChunkParserRegistry.cpp's private
//! SkeletonGeometryAsciiChunkParser::DirectionToEulerDeg -- deliberately
//! duplicated (that function isn't exported) so TestNoRedundantJointSphere
//! below can hand-build the SAME two roundcone parts the chunk emits, to
//! compare against.  Any drift between the two copies would show up as a
//! spurious area mismatch in that test, so this mirror is self-checking.
//! NEAR-VERTICAL FIX (doc 90 R4): the production function now returns a
//! third angle, ez, spent on a Gram-Schmidt-stabilized roll instead of
//! being zeroed, for any bone leaning less than kNearVerticalHoriz off
//! vertical -- see that function's own comment for the derivation.  This
//! mirror carries the identical formula so a near-vertical direction run
//! through both copies still proves non-tautologically.
static const double kNearVerticalHorizMirror = 0.01;   // ~0.57 degrees of lean

static void DirToEulerDeg( double dx, double dy, double dz, double& exDeg, double& eyDeg, double& ezDeg )
{
	const double RAD_TO_DEG = 180.0 / PI;
	const double horiz = std::sqrt( dx*dx + dz*dz );
	if( horiz < kNearVerticalHorizMirror ) {
		const double vx = 1.0 - dx*dx, vy = -dx*dy, vz = -dx*dz;
		const double vlen = std::sqrt( vx*vx + vy*vy + vz*vz );
		const double xhx = vx / vlen, xhy = vy / vlen, xhz = vz / vlen;
		const double zhz = xhx*dy - xhy*dx;
		const double c2 = std::sqrt( xhx*xhx + xhy*xhy );
		eyDeg = std::atan2( -xhz, c2 ) * RAD_TO_DEG;
		ezDeg = std::atan2( xhy, xhx ) * RAD_TO_DEG;
		exDeg = std::atan2( dz, zhz ) * RAD_TO_DEG;
		return;
	}
	exDeg = std::atan2( horiz, dy ) * RAD_TO_DEG;
	eyDeg = std::atan2( dx, dz ) * RAD_TO_DEG;
	ezDeg = 0.0;
}

//////////////////////////////////////////////////////////////////////
// 1 -- expansion + naming
//////////////////////////////////////////////////////////////////////

void TestExpansionAndNaming()
{
	std::cout << "Test: a joint chain expands under the chunk's OWN name (no `__geo` suffix)" << std::endl;

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	const bool ok = ParseBodyInto( "expand", SkeletonChunk( "critter", GoodChain() ), *job );
	Check( ok, "expand: a 3-joint chain parses" );

	Check( job->GetGeometries() && job->GetGeometries()->GetItem( "critter" ) != 0,
	       "expand: the geometry is registered under the chunk's OWN name `critter`" );
	Check( job->GetGeometries() && job->GetGeometries()->GetItem( "critter__geo" ) == 0,
	       "expand: MONEY ASSERTION -- there is NO stray `critter__geo` entry (that suffix "
	       "convention belongs to shape_light/rect_light, not this chunk)" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 2 -- pose math, from first principles (the money section)
//////////////////////////////////////////////////////////////////////

//! For one bone direction: build a 2-joint skeleton (root -> child),
//! fetch its geometry, and fire rays at the KNOWN world positions of
//! both the base cap (root, radius rp -- position-only, insensitive to
//! any rotation bug) and the tip cap (child, radius rc -- moves with
//! the direction-to-Euler conversion, so THIS is what a wrong formula
//! breaks).  Approach directions are swept within a wide half-angle
//! (<=60 degrees) of the pure axial direction -- comfortably inside the
//! region sdRoundConeY resolves as the plain spherical cap for any
//! reasonable taper (root/child radii differ by only 0.3 over a length
//! of 3, a taper half-angle of ~6 degrees), so there is no ambiguity
//! with the tapered lateral wall.
void RunPoseCase( const char* label, double rawDx, double rawDy, double rawDz )
{
	const Vector3 d = Vector3Ops::Normalize( Vector3( rawDx, rawDy, rawDz ) );
	const Point3  P( 1.0, 2.0, -1.0 );          // root -- an off-origin position on purpose
	const double  len = 3.0, rp = 0.5, rc = 0.2;
	const Point3  C( P.x + len*d.x, P.y + len*d.y, P.z + len*d.z );

	char jointBuf[256];
	std::vector<std::string> joints;
	std::snprintf( jointBuf, sizeof(jointBuf), "root none %.17g %.17g %.17g %.17g", P.x, P.y, P.z, rp );
	joints.push_back( jointBuf );
	std::snprintf( jointBuf, sizeof(jointBuf), "child root %.17g %.17g %.17g %.17g", C.x, C.y, C.z, rc );
	joints.push_back( jointBuf );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, std::string("job created -- ") + label ); return; }
	const bool ok = ParseBodyInto( std::string("pose_") + label, SkeletonChunk( "bone", joints, "0.3" ), *job );
	Check( ok, std::string("pose ") + label + ": parses" );
	const IGeometry* geo = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "bone" ) : 0;
	if( !geo ) { Check( false, std::string("pose ") + label + ": geometry retrievable" ); safe_release( job ); return; }

	const Vector3 e1 = PerpBasis( d );
	const Vector3 e2 = Vector3Ops::Normalize( Vector3Ops::Cross( d, e1 ) );
	const double Rbig = 50.0;
	const double thetasDeg[] = { 0.0, 30.0, 60.0 };
	const double phisDeg[]   = { 0.0, 90.0, 180.0, 270.0 };

	bool allTip = true, allBase = true;
	double worstTip = 0.0, worstBase = 0.0;

	for( std::size_t ti = 0; ti < sizeof(thetasDeg)/sizeof(thetasDeg[0]); ++ti ) {
		const double theta = thetasDeg[ti] * PI / 180.0;
		const int nPhi = ( ti == 0 ) ? 1 : (int)( sizeof(phisDeg)/sizeof(phisDeg[0]) ); // theta=0 has no azimuth freedom
		for( int pi = 0; pi < nPhi; ++pi ) {
			const double phi = phisDeg[pi] * PI / 180.0;
			const Vector3 lateral( e1.x*std::cos(phi) + e2.x*std::sin(phi),
			                       e1.y*std::cos(phi) + e2.y*std::sin(phi),
			                       e1.z*std::cos(phi) + e2.z*std::sin(phi) );

			// TIP cap: approach from beyond the tip, along +d-ish.
			{
				const Vector3 appr = Vector3Ops::Normalize(
					Vector3( d.x*std::cos(theta) + lateral.x*std::sin(theta),
					         d.y*std::cos(theta) + lateral.y*std::sin(theta),
					         d.z*std::cos(theta) + lateral.z*std::sin(theta) ) );
				const Point3 origin( C.x + appr.x*Rbig, C.y + appr.y*Rbig, C.z + appr.z*Rbig );
				const Vector3 dir( -appr.x, -appr.y, -appr.z );
				RayIntersectionGeometric ri = MkRI( origin, dir );
				geo->IntersectRay( ri, true, true, false );
				const double want = Rbig - rc;
				if( !( ri.bHit && IsClose( (double)ri.range, want, 5e-2 ) ) ) {
					allTip = false;
					const double got = ri.bHit ? (double)ri.range : -1.0;
					worstTip = std::fabs( got - want ) > worstTip ? std::fabs( got - want ) : worstTip;
				}
			}
			// BASE cap: approach from before the root, along -d-ish.
			{
				const Vector3 appr = Vector3Ops::Normalize(
					Vector3( -d.x*std::cos(theta) + lateral.x*std::sin(theta),
					         -d.y*std::cos(theta) + lateral.y*std::sin(theta),
					         -d.z*std::cos(theta) + lateral.z*std::sin(theta) ) );
				const Point3 origin( P.x + appr.x*Rbig, P.y + appr.y*Rbig, P.z + appr.z*Rbig );
				const Vector3 dir( -appr.x, -appr.y, -appr.z );
				RayIntersectionGeometric ri = MkRI( origin, dir );
				geo->IntersectRay( ri, true, true, false );
				const double want = Rbig - rp;
				if( !( ri.bHit && IsClose( (double)ri.range, want, 5e-2 ) ) ) {
					allBase = false;
					const double got = ri.bHit ? (double)ri.range : -1.0;
					worstBase = std::fabs( got - want ) > worstBase ? std::fabs( got - want ) : worstBase;
				}
			}
		}
	}

	Check( allTip, std::string("pose ") + label + ": MONEY ASSERTION -- the TIP cap sits exactly at "
	       "the declared child joint (proves the direction-to-Euler conversion), worst miss " +
	       std::to_string( worstTip ) );
	Check( allBase, std::string("pose ") + label + ": the BASE cap sits exactly at the declared root "
	       "joint (position-only control, insensitive to rotation), worst miss " + std::to_string( worstBase ) );

	safe_release( job );
}

void TestPoseMath()
{
	std::cout << "Test: bone pose (position + orientation), first-principles ray proof" << std::endl;
	RunPoseCase( "+X",      1.0,  0.0,  0.0 );
	RunPoseCase( "+Z",      0.0,  0.0,  1.0 );
	RunPoseCase( "-Y",      0.0, -1.0,  0.0 );
	RunPoseCase( "+Y",      0.0,  1.0,  0.0 );
	RunPoseCase( "oblique", 0.4,  0.7, -0.5 );
}

//////////////////////////////////////////////////////////////////////
// 2b -- F1 REGRESSION: a DEGENERATE bone (|rp-rc| > bone length -- one
// joint's cap sphere entirely contains the other's) used to under-bound
// the local AABB (SDFGeometry.cpp primLocalAABB's roundcone case), so
// IntersectRay's march (and the TLAS box) clipped away real surface.
// Exact repro from the bug report: joint `a` (no parent, radius 1.0) ->
// joint `b` (parent `a`, offset (0,0.5,0), radius 0.1).  |1.0-0.1|=0.9 >
// bone length 0.5.  With `blend 0` the fold is a hard union, so the
// field is (for any ray off the exact Y axis) the plain unit sphere at
// the origin -- SEE tests/SDFGeometryTest.cpp's TestRoundCone* pair for
// the from-first-principles derivation of why.  A ray descending through
// (x,z)=(0.3,0.4) (radial distance 0.5 from the axis) must hit near
// y=sqrt(1-0.5^2)=sqrt(0.75)~=0.8660254 -- the OLD, buggy box (top at
// y=0.6) would have clipped the march before it got there.  Also checks
// the non-rejecting diagnostic (naming both joints) actually fires.
//////////////////////////////////////////////////////////////////////

void TestDegenerateBoneAABB()
{
	std::cout << "Test: F1 regression -- a degenerate bone (|rp-rc|>len) is not clipped, and warns" << std::endl;

	std::vector<std::string> joints;
	joints.push_back( "a none 0 0 0 1.0" );
	joints.push_back( "b a 0 0.5 0 0.1" );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	// Capture stdout (GlobalLog's default sink -- eLog_Console includes
	// eLog_Warning) around the parse so the non-rejecting diagnostic can
	// be asserted on, not just inferred.  Same fd-dup/dup2 technique as
	// CSGNullGeometryLuminaireCrashTest.cpp's RenderSceneCapturingStdout,
	// inlined here since this is the only site in this file that needs it.
	std::string capturedOutput;
	{
		const char* tmpEnv = getenv( "TMPDIR" );
		std::string dir = tmpEnv ? tmpEnv : "/tmp/";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
		char pidbuf[32];
		std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
		const std::string capPath = dir + "rise_skeleton_geo_degen_stdout_" + pidbuf + ".txt";

		std::fflush( stdout );
		const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
		FILE* capFile = std::fopen( capPath.c_str(), "w" );
		if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

		const bool ok = ParseBodyInto( "degen", SkeletonChunk( "bonebug", joints, "0" ), *job );

		std::fflush( stdout );
		if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
		if( capFile ) std::fclose( capFile );

		std::ifstream ifs( capPath.c_str() );
		if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); capturedOutput = oss.str(); }
		remove( capPath.c_str() );

		Check( ok, "degenerate bone: fixture parses (this is a WARNING, not a rejection)" );
	}

	Check( capturedOutput.find( "skeleton_geometry" ) != std::string::npos &&
	       capturedOutput.find( "`a`" ) != std::string::npos &&
	       capturedOutput.find( "`b`" ) != std::string::npos &&
	       capturedOutput.find( "degenerate" ) != std::string::npos,
	       "degenerate bone: MONEY ASSERTION -- the non-rejecting diagnostic fires, naming both joint `a` and `b`" );

	const IGeometry* geo = job->GetGeometries() ? job->GetGeometries()->GetItem( "bonebug" ) : 0;
	if( !geo ) { Check( false, "degenerate bone: geometry retrievable" ); safe_release( job ); return; }

	const double trueTopY = std::sqrt( 1.0 - 0.5*0.5 );   // sqrt(0.75), radial dist 0.5 from axis
	RayIntersectionGeometric ri = MkRI( Point3( 0.3, 50, 0.4 ), Vector3( 0, -1, 0 ) );
	geo->IntersectRay( ri, true, true, false );
	Check( ri.bHit && IsClose( (double)ri.range, 50.0 - trueTopY, 0.02 ),
	       "degenerate bone: MONEY ASSERTION -- the ray hits the TRUE (uncapped) sphere surface "
	       "(~49.134), not the old clipped-plane range (~49.4 at y=0.6)" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 3 -- blend semantics, closed-form
//////////////////////////////////////////////////////////////////////

//! root=(-2,0,0) r=0.6 -> mid=(0,0,0) r=0.5 -> tip=(0,2,0) r=0.4.  Along
//! the ray from far away on +X toward the origin, BOTH bones' individual
//! SDF values reduce to (x - mid.r) exactly (root's segment and tip's
//! segment both have their nearest point at `mid` for x>0), so
//! SDFGeometry::Map's fold gives EXACTLY (x - mid.r) for hard union and
//! EXACTLY (x - mid.r) - k/4 for a positive-k smin, where
//! k = blend * min(mid.r, tip.r) (the LAST-folded bone's own k).  This
//! is a closed-form prediction, not just "the hit moves" -- and the k/4
//! offset is exactly `blend * min(mid.r,tip.r) / 4`, so it directly
//! proves `blend` multiplies the SMALLER of the two radii.
void TestBlendSemantics()
{
	std::cout << "Test: `blend` closed-form -- smin shifts the hit by exactly blend*min(r)/4" << std::endl;

	std::vector<std::string> joints;
	joints.push_back( "root none -2 0 0 0.6" );
	joints.push_back( "mid  root  0 0 0 0.5" );
	joints.push_back( "tip  mid   0 2 0 0.4" );

	const Point3 origin( 50, 0, 0 );
	const Vector3 dir( -1, 0, 0 );
	const double midR = 0.5;

	struct Case { const char* blend; double expectedK; };
	static const Case kCases[] = {
		{ "0",   0.0 },
		{ "0.5", 0.5 * 0.4 },   // min(mid.r=0.5, tip.r=0.4) = 0.4
		{ "2.0", 2.0 * 0.4 },
	};

	double prevRange = 1e300;
	for( std::size_t i = 0; i < sizeof(kCases)/sizeof(kCases[0]); ++i ) {
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
		const bool ok = ParseBodyInto( std::string("blend_") + std::to_string(i),
			SkeletonChunk( "elbow", joints, kCases[i].blend ), *job );
		Check( ok, std::string("blend `") + kCases[i].blend + "`: parses" );
		const IGeometry* geo = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "elbow" ) : 0;
		if( !geo ) { Check( false, "blend: geometry retrievable" ); safe_release( job ); continue; }

		RayIntersectionGeometric ri = MkRI( origin, dir );
		geo->IntersectRay( ri, true, true, false );
		const double wantRange = 50.0 - ( midR + kCases[i].expectedK / 4.0 );
		std::cout << "    blend " << kCases[i].blend << ": range=" << ( ri.bHit ? (double)ri.range : -1.0 )
		          << "  want=" << wantRange << std::endl;
		Check( ri.bHit && IsClose( (double)ri.range, wantRange, 1e-2 ),
		       std::string("blend `") + kCases[i].blend + "`: MONEY ASSERTION -- hit range matches the "
		       "closed-form k/4 bulge exactly (want " + std::to_string(wantRange) + ")" );

		if( i > 0 ) {
			Check( (double)ri.range < prevRange - 1e-4,
			       std::string("blend `") + kCases[i].blend + "`: a LARGER blend bulges the surface "
			       "further outward (smaller range) than the previous case, monotonically" );
		}
		prevRange = ri.bHit ? (double)ri.range : prevRange;
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 3b -- the "no separate joint sphere" invariant, GUARDED (not just
// documented).  The struct-level comment in ChunkParserRegistry.cpp
// says "DO NOT fix this by adding a sphere at every joint", but nothing
// above actually FAILS if a regression did exactly that -- the pose-math
// and blend-semantics probes above happen to sit where a plausible
// spurious addition is invisible or within tolerance.
//
// A first version of this test guarded via total surface AREA, but a
// redundant joint sphere is EXACTLY coincident with the bone end cap
// already there (same centre, same radius): under a hard union the
// field is mathematically IDENTICAL, so area is unchanged; under smin
// with k>0 the duplicate only bulges the surface outward by k/4 near
// that one joint (Quilez smin, |a-b|=0 -> h=1 -> result = min - k/4) --
// a small LOCAL shift that global-area sampling drowns in marching-tets
// tessellation noise.  So this test instead guards via SDFGeometry's
// exact, blend-independent PART COUNT (`SDFGeometry::NumParts()`): the
// chunk is supposed to emit exactly one `roundcone` bone per joint that
// HAS a parent (root->mid, mid->tip = 2 parts for this fixture), and
// nothing per interior joint.  A spurious sphere at a covered joint
// changes that count by +1, unambiguously and independent of blend.
//////////////////////////////////////////////////////////////////////

void TestNoRedundantJointSphere()
{
	std::cout << "Test: no spurious sphere at a covered joint -- guarded via exact PART COUNT, not just a probe" << std::endl;

	// Same 3-joint fixture as TestBlendSemantics (root -> mid -> tip),
	// so `mid` is an interior joint covered by TWO bones' end caps and
	// therefore -- per the struct comment -- gets no sphere of its own.
	struct J { const char* name; const char* parent; double x,y,z,r; };
	static const J kJ[] = {
		{ "root", "none", -2, 0, 0, 0.6 },
		{ "mid",  "root",  0, 0, 0, 0.5 },
		{ "tip",  "mid",   0, 2, 0, 0.4 },
	};
	const double blend = 0.4;

	std::vector<std::string> joints;
	for( std::size_t i = 0; i < sizeof(kJ)/sizeof(kJ[0]); ++i ) {
		char buf[128];
		std::snprintf( buf, sizeof(buf), "%s %s %g %g %g %g", kJ[i].name, kJ[i].parent, kJ[i].x, kJ[i].y, kJ[i].z, kJ[i].r );
		joints.push_back( buf );
	}
	char blendBuf[32];
	std::snprintf( blendBuf, sizeof(blendBuf), "%g", blend );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
	const bool ok = ParseBodyInto( "noredundant", SkeletonChunk( "elbow3", joints, blendBuf ), *job );
	Check( ok, "no-redundant-sphere: fixture parses" );
	const IGeometry* actualIface = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "elbow3" ) : 0;
	if( !actualIface ) { Check( false, "no-redundant-sphere: geometry retrievable" ); safe_release( job ); return; }
	const SDFGeometry* actual = dynamic_cast<const SDFGeometry*>( actualIface );
	if( !actual ) { Check( false, "no-redundant-sphere: chunk expands to an SDFGeometry" ); safe_release( job ); return; }

	// The chunk is supposed to emit exactly one `roundcone` bone per
	// joint that HAS A PARENT -- root->mid, mid->tip -- and nothing extra
	// for `mid` itself, since its two bone end caps already cover it.
	const std::size_t expectedParts = 2;
	const std::size_t actualParts = actual->NumParts();
	std::cout << "    actual parts=" << actualParts << "  expected=" << expectedParts << std::endl;

	Check( actualParts == expectedParts,
	       "no-redundant-sphere: MONEY ASSERTION -- the chunk emits EXACTLY the two bone parts "
	       "(root->mid, mid->tip), no separate sphere at the shared joint `mid`" );

	// Non-tautology proof: hand-build the SAME two bones -- same MakePart
	// calls, same Euler math (mirrored via DirToEulerDeg above), same
	// maxsteps/eps/sampling_detail defaults (256 / 0.0 -> auto / 64) the
	// chunk's Finalize passes -- PLUS a spurious third sphere at `mid`,
	// smin-blended with the SAME `blend * radius` convention
	// TestIsolatedJoint's sphere already uses -- exactly what a "fix"
	// that added a sphere at every joint would emit.  Applying the SAME
	// count check to THIS geometry must fail (3 != 2), proving the
	// MONEY ASSERTION above is a genuine guard, not a tautology that
	// would pass regardless of the bug.
	auto bonePart = []( const J& par, const J& child, double blendW ) -> SDFGeometry::Part
	{
		const double dx = child.x-par.x, dy = child.y-par.y, dz = child.z-par.z;
		const double len = std::sqrt(dx*dx+dy*dy+dz*dz);
		double exDeg=0, eyDeg=0, ezDeg=0;
		DirToEulerDeg( dx/len, dy/len, dz/len, exDeg, eyDeg, ezDeg );
		const double k = blendW * std::min( par.r, child.r );
		return SDFGeometry::MakePart( SDFGeometry::ePrimRoundCone, SDFGeometry::eOpSmin, k,
			Point3(par.x,par.y,par.z), exDeg, eyDeg, ezDeg, Vector3(1,1,1), par.r, child.r, len, 0 );
	};

	std::vector<SDFGeometry::Part> spuriousParts;
	spuriousParts.push_back( bonePart( kJ[0], kJ[1], blend ) );   // root -> mid
	spuriousParts.push_back( bonePart( kJ[1], kJ[2], blend ) );   // mid -> tip
	spuriousParts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimSphere, SDFGeometry::eOpSmin,
		blend * kJ[1].r, Point3(kJ[1].x,kJ[1].y,kJ[1].z), 0,0,0, Vector3(1,1,1), kJ[1].r, 0,0,0 ) );
	SDFGeometry* spuriousGeo = new SDFGeometry( spuriousParts, 256, 0.0, 64 );
	const std::size_t spuriousPartCount = spuriousGeo->NumParts();

	std::cout << "    +spurious-sphere parts=" << spuriousPartCount << std::endl;

	Check( spuriousPartCount == expectedParts + 1,
	       "no-redundant-sphere: NON-TAUTOLOGY PROOF -- a spurious joint sphere DOES change the "
	       "part count (3 vs the expected 2), so the MONEY ASSERTION above would catch it" );

	safe_release( job );
	safe_release( spuriousGeo );
}

//////////////////////////////////////////////////////////////////////
// 4 -- isolated joint
//////////////////////////////////////////////////////////////////////

void TestIsolatedJoint()
{
	std::cout << "Test: a lone joint (no parent, no children) emits a sphere" << std::endl;

	std::vector<std::string> joints;
	joints.push_back( "lonely none 3 4 5 1.25" );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
	const bool ok = ParseBodyInto( "isolated", SkeletonChunk( "orb", joints ), *job );
	Check( ok, "isolated: a single-joint skeleton parses" );

	const IGeometry* geo = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "orb" ) : 0;
	if( geo ) {
		const BoundingBox bb = geo->GenerateBoundingBox();
		// The SDF bbox pads outward for the sphere-trace safety margin, so
		// this is a CONTAINMENT check (>=), matching SDFGeometryTest's own
		// bbox test style, not an exact-equality one.
		Check( bb.ll.x <= 3.0 - 1.25 + 1e-6 && bb.ur.x >= 3.0 + 1.25 - 1e-6 &&
		       bb.ll.y <= 4.0 - 1.25 + 1e-6 && bb.ur.y >= 4.0 + 1.25 - 1e-6 &&
		       bb.ll.z <= 5.0 - 1.25 + 1e-6 && bb.ur.z >= 5.0 + 1.25 - 1e-6,
		       "isolated: bbox contains a radius-1.25 sphere centred at (3,4,5)" );
		Check( geo->GetArea() > 0.0, "isolated: non-zero surface area" );
	} else {
		Check( false, "isolated: geometry retrievable" );
	}
	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 4b -- BRANCHING: a joint with 2+ children.  Every test above this
// point uses 1-, 2-, or 3-joint CHAINS -- the `childCount > 1` path
// (the entire point of a joint GRAPH, not a joint LIST) is otherwise
// only exercised by scenes/Tests/Geometry/skeleton_basic.RISEscene,
// which carries no assertions.  A root ("hips") with THREE children
// ("leg1", "leg2", "tail"), all sharing the SAME child radius so their
// three bones have IDENTICAL blend widths k -- which keeps the
// N-way-junction closed form below order-independent (see its comment).
//////////////////////////////////////////////////////////////////////

void TestBranchingJoint()
{
	std::cout << "Test: a joint with 2+ children (branching, not just a chain)" << std::endl;

	// hips at the origin; three children spread in a cone around +Z
	// (25 degrees off-axis, 120 degrees apart in azimuth) so that ANY
	// point on the -Z axis through hips is, by rotational symmetry,
	// equidistant from all three bones -- see the closed-form probe
	// below.  Same child radius (0.3) on all three -> identical k.
	const double hipsR = 0.5, childR = 0.3, L = 2.5, blend = 0.4;
	const double theta = 25.0 * PI / 180.0;
	struct Tip { const char* name; double az; Point3 pos; };
	Tip tips[3] = {
		{ "leg1", 0.0,   Point3(0,0,0) },
		{ "leg2", 120.0, Point3(0,0,0) },
		{ "tail", 240.0, Point3(0,0,0) },
	};
	for( int i = 0; i < 3; ++i ) {
		const double az = tips[i].az * PI / 180.0;
		const Vector3 d( std::sin(theta)*std::cos(az), std::sin(theta)*std::sin(az), std::cos(theta) );
		tips[i].pos = Point3( L*d.x, L*d.y, L*d.z );
	}

	char buf[128];
	std::vector<std::string> joints;
	std::snprintf( buf, sizeof(buf), "hips none 0 0 0 %g", hipsR );
	joints.push_back( buf );
	for( int i = 0; i < 3; ++i ) {
		std::snprintf( buf, sizeof(buf), "%s hips %.17g %.17g %.17g %g",
			tips[i].name, tips[i].pos.x, tips[i].pos.y, tips[i].pos.z, childR );
		joints.push_back( buf );
	}
	char blendBuf[32];
	std::snprintf( blendBuf, sizeof(blendBuf), "%g", blend );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
	const bool ok = ParseBodyInto( "branch", SkeletonChunk( "tripod", joints, blendBuf ), *job );
	Check( ok, "branching: a 3-child skeleton parses" );
	const IGeometry* geo = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "tripod" ) : 0;
	if( !geo ) { Check( false, "branching: geometry retrievable" ); safe_release( job ); return; }

	// (1) CONTAINS ALL BONES: ray-cast toward each branch tip's cap
	// sphere from beyond it, along the bone's own direction -- exactly
	// TestPoseMath's RunPoseCase strategy.  A branch that never made it
	// into the expansion (e.g. an emission-order bug dropping all but
	// one child) would miss here.
	const double Rbig = 50.0;
	for( int i = 0; i < 3; ++i ) {
		const Vector3 d = Vector3Ops::Normalize( Vector3( tips[i].pos.x, tips[i].pos.y, tips[i].pos.z ) );
		const Point3 origin( tips[i].pos.x + d.x*Rbig, tips[i].pos.y + d.y*Rbig, tips[i].pos.z + d.z*Rbig );
		RayIntersectionGeometric ri = MkRI( origin, Vector3(-d.x,-d.y,-d.z) );
		geo->IntersectRay( ri, true, true, false );
		const double want = Rbig - childR;
		Check( ri.bHit && IsClose( (double)ri.range, want, 5e-2 ),
		       std::string("branching: MONEY ASSERTION -- branch `") + tips[i].name +
		       "`'s tip cap is present at its declared position (range " +
		       std::to_string( ri.bHit ? (double)ri.range : -1.0 ) + ", want " + std::to_string(want) + ")" );
	}

	// (2) A PROBE AMONG THE SIBLINGS, CLOSED-FORM.  Approach hips from
	// far down the -Z axis (opposite the cone of children).  By the
	// rotational symmetry above, all three bones' RAW (unblended)
	// distances are IDENTICAL at every point on this axis -- each bone's
	// base cap is the sphere of radius hipsR at hips, and a point on the
	// symmetry axis is equidistant from that sphere regardless of which
	// bone "owns" it.  SDFGeometry::Map folds sequentially:
	//   d1 = x          (first bone; initial d=1e30 makes its own smin a no-op union)
	//   d2 = smin(d1,x,k) = x - k/4                      (two EQUAL values -> h=1)
	//   d3 = smin(d2,x,k) = (x-k/4) - (9/16)*k/4 = x - (25/64)k
	// (h for the third fold is max(k-k/4,0)/k = 3/4, offset = h^2*k/4 =
	// (9/16)(k/4) = 9k/64; total pull-in = k/4 + 9k/64 = 25k/64 =
	// 0.390625*k). This 0.390625k is LARGER than the 2-way k/4 = 0.25k a
	// reader of TestBlendSemantics might expect -- an N-way junction
	// accumulates MORE pull-in than a 2-way one, not the same amount.
	// The formula is order-independent HERE specifically because all
	// three raw distances are equal by construction (any fold order
	// combines "two equal values" first, then combines that result with
	// the equal third) -- it would NOT be order-independent for
	// unequal per-branch k, which is why this fixture deliberately uses
	// the SAME child radius on all three branches.
	{
		const double k = blend * std::min( hipsR, childR );   // same on all 3 bones
		const double pullIn = ( 25.0 / 64.0 ) * k;
		const Point3 origin( 0, 0, -Rbig );
		const Vector3 dir( 0, 0, 1 );
		RayIntersectionGeometric ri = MkRI( origin, dir );
		geo->IntersectRay( ri, true, true, false );
		const double want = Rbig - ( hipsR + pullIn );
		std::cout << "    3-way junction probe: range=" << ( ri.bHit ? (double)ri.range : -1.0 )
		          << "  want=" << want << " (pull-in " << pullIn << " = 0.390625*k, k=" << k << ")" << std::endl;
		Check( ri.bHit && IsClose( (double)ri.range, want, 1e-2 ),
		       "branching: MONEY ASSERTION -- the 3-way junction probe matches the closed-form "
		       "25k/64 pull-in (not the 2-way k/4), proving all three sibling bones actually "
		       "fold into the SAME field at their shared joint" );
	}

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 4c -- DOC 90 SLICE B: the optional 7th `joint` token, `aspect`.
//
// Four things need pinning, and every one of them is a NUMBER this test
// can predict from first principles rather than a shape it can only
// eyeball:
//
//   (a) BACK-COMPAT.  A 6-token joint line must expand EXACTLY as it did
//       before the token existed.  The expansion emits `<sx> 1 1` where
//       it used to hardcode `1 1 1`, and aspect == 1 formats as "1"
//       under %.17g, so the claim is byte-identity of the generated part
//       text.  The test can't see that text, so it proves the stronger
//       observable: a ray-range DIGEST over a 30-ray fan is EXACTLY
//       equal (not merely close) between the chunk expansion and
//       hand-built parts carrying the literal Vector3(1,1,1) the old
//       code emitted -- the same hand-build/mirror technique
//       TestNoRedundantJointSphere already uses.
//
//   (b) THE AXIS CONVENTION, closed-form.  A VERTICAL bone with equal
//       end radii is a capsule; squashed by `<aspect> 1 1` its
//       mid-section is the ELLIPSE (x/aspect)^2 + z^2 = r^2, because
//       the bone's local frame for a +Y direction is the identity
//       (DirectionToEulerDeg gives ex = ey = 0) so local X is world X.
//       So the surface sits at |x| = aspect*sqrt(r^2 - z^2) along X and
//       |z| = r*sqrt(1 - (x/(aspect*r))^2) along Z -- exact ranges, from
//       many (x, z) stations, not a "does it look flatter" check.
//
//   (c) THE MAPPING, distinguished from its plausible rival.  Those same
//       ranges separate PURE SQUASH from AREA-PRESERVING by a mile: at
//       aspect 0.5, r 0.5, the X probe reads 0.250 pure vs 0.354
//       area-preserving, and the Z probe 0.500 vs 0.707.  The Z probe
//       doubles as the "radius still means what it meant" assertion --
//       the unflattened axis is EXACTLY the declared radius -- and the
//       axial probe below it proves local Y is untouched, i.e. `aspect`
//       cannot move a cap off its declared joint.
//
//   (d) NO TUNNELING when a squashed bone is smin-blended against a
//       round one.  SDFGeometry's partEval multiplies the primitive
//       distance by `minScale`, which caps the composed field at
//       1-Lipschitz for any positive per-axis scale, so blending must
//       only ever pull the surface OUTWARD and by at most k/4.  Probed
//       as a differential against the same skeleton at `blend 0`: every
//       ray that hits hard-union must still hit blended, and the range
//       must move outward by more than 0 and at most k/4.
//
// RED-PROVED (see the report): emitting the scale as `1 1 <aspect>`
// instead of `<aspect> 1 1` -- i.e. flattening the OTHER perpendicular
// -- fails the (b)/(c) probes; the transcript is in the task report.
//////////////////////////////////////////////////////////////////////

//! Ray-range digest over a fixed 30-ray fan aimed at `centre` from
//! radius `Rbig`.  Deterministic and geometry-independent, so two
//! geometries that are field-identical produce EXACTLY equal digests.
static void RangeDigest( const IGeometry& geo, const Point3& centre, double Rbig,
                         std::vector<double>& out )
{
	out.clear();
	for( int i = 0; i < 6; ++i ) {
		const double phi = ( i * 60.0 ) * PI / 180.0;
		for( int j = 0; j < 5; ++j ) {
			const double th = ( 20.0 + j * 35.0 ) * PI / 180.0;
			const Vector3 appr( std::sin(th)*std::cos(phi), std::cos(th), std::sin(th)*std::sin(phi) );
			const Point3 origin( centre.x + appr.x*Rbig, centre.y + appr.y*Rbig, centre.z + appr.z*Rbig );
			RayIntersectionGeometric ri = MkRI( origin, Vector3(-appr.x,-appr.y,-appr.z) );
			geo.IntersectRay( ri, true, true, false );
			out.push_back( ri.bHit ? (double)ri.range : -1.0 );
		}
	}
}

void TestAspectBackCompat()
{
	std::cout << "Test: aspect -- a 6-token joint line expands EXACTLY as it did before the token existed" << std::endl;

	// The same 3-joint fixture TestNoRedundantJointSphere hand-builds,
	// authored with SIX tokens per line (no aspect at all).
	struct J { const char* name; const char* parent; double x,y,z,r; };
	static const J kJ[] = {
		{ "root", "none", -2, 0, 0, 0.6 },
		{ "mid",  "root",  0, 0, 0, 0.5 },
		{ "tip",  "mid",   0, 2, 0, 0.4 },
	};
	const double blend = 0.4;

	std::vector<std::string> joints;
	for( std::size_t i = 0; i < sizeof(kJ)/sizeof(kJ[0]); ++i ) {
		char buf[128];
		std::snprintf( buf, sizeof(buf), "%s %s %g %g %g %g", kJ[i].name, kJ[i].parent, kJ[i].x, kJ[i].y, kJ[i].z, kJ[i].r );
		joints.push_back( buf );
	}

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
	const bool ok = ParseBodyInto( "backcompat", SkeletonChunk( "oldform", joints, "0.4" ), *job );
	Check( ok, "back-compat: the 6-token fixture parses" );
	const IGeometry* actual = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "oldform" ) : 0;
	if( !actual ) { Check( false, "back-compat: geometry retrievable" ); safe_release( job ); return; }

	// The PRE-doc-90 expansion, hand-built: identical parts, and the
	// literal Vector3(1,1,1) scale the old PartLine hardcoded.
	std::vector<SDFGeometry::Part> oldParts;
	for( std::size_t i = 1; i < sizeof(kJ)/sizeof(kJ[0]); ++i ) {
		const J& par = kJ[i-1];
		const J& ch  = kJ[i];
		const double dx = ch.x-par.x, dy = ch.y-par.y, dz = ch.z-par.z;
		const double len = std::sqrt(dx*dx+dy*dy+dz*dz);
		double exDeg=0, eyDeg=0, ezDeg=0;
		DirToEulerDeg( dx/len, dy/len, dz/len, exDeg, eyDeg, ezDeg );
		oldParts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimRoundCone, SDFGeometry::eOpSmin,
			blend * std::min( par.r, ch.r ), Point3(par.x,par.y,par.z), exDeg, eyDeg, ezDeg,
			Vector3(1,1,1), par.r, ch.r, len, 0 ) );
	}
	SDFGeometry* reference = new SDFGeometry( oldParts, 256, 0.0, 64 );

	std::vector<double> dNew, dOld;
	RangeDigest( *actual,    Point3( -1, 1, 0 ), 40.0, dNew );
	RangeDigest( *reference, Point3( -1, 1, 0 ), 40.0, dOld );

	bool sameSize = ( dNew.size() == dOld.size() && !dNew.empty() );
	Check( sameSize, "back-compat: both digests have the same 30 entries" );
	int hits = 0;
	double worst = 0.0;
	if( sameSize ) {
		for( std::size_t i = 0; i < dNew.size(); ++i ) {
			if( dNew[i] > 0 ) ++hits;
			const double d = std::fabs( dNew[i] - dOld[i] );
			if( d > worst ) worst = d;
		}
	}
	std::cout << "    back-compat digest: " << hits << "/" << dNew.size()
	          << " rays hit, worst |delta| = " << worst << std::endl;
	// The fixture is an L (root -> mid -> tip turns 90 degrees), so a
	// uniform fan legitimately misses on the concave side; this floor
	// exists only to reject a VACUOUS all-miss match, where two empty
	// digests would compare equal and prove nothing.  Measured 18/30.
	Check( hits >= 12, "back-compat: the digest fan actually hits the geometry (not a vacuous all-miss match)" );
	Check( sameSize && worst == 0.0,
	       "back-compat: MONEY ASSERTION -- a 6-token skeleton is EXACTLY (bit-for-bit range equality, "
	       "not merely close) the pre-aspect expansion with its literal `1 1 1` scale" );

	// And the explicit `1` spells the same thing as omitting it.
	{
		std::vector<std::string> j7;
		for( std::size_t i = 0; i < sizeof(kJ)/sizeof(kJ[0]); ++i ) {
			char buf[128];
			std::snprintf( buf, sizeof(buf), "%s %s %g %g %g %g 1", kJ[i].name, kJ[i].parent, kJ[i].x, kJ[i].y, kJ[i].z, kJ[i].r );
			j7.push_back( buf );
		}
		IJobPriv* job7 = nullptr;
		if( RISE_CreateJobPriv( &job7 ) && job7 ) {
			const bool ok7 = ParseBodyInto( "aspect1", SkeletonChunk( "newform", j7, "0.4" ), *job7 );
			Check( ok7, "back-compat: the same fixture with an explicit `aspect 1` parses" );
			const IGeometry* g7 = ( ok7 && job7->GetGeometries() ) ? job7->GetGeometries()->GetItem( "newform" ) : 0;
			std::vector<double> d7;
			if( g7 ) RangeDigest( *g7, Point3( -1, 1, 0 ), 40.0, d7 );
			bool same = ( d7.size() == dNew.size() );
			if( same ) for( std::size_t i = 0; i < d7.size(); ++i ) if( d7[i] != dNew[i] ) same = false;
			Check( same, "back-compat: an explicit `aspect 1` is bit-identical to omitting the token" );
			safe_release( job7 );
		}
	}

	safe_release( reference );
	safe_release( job );
}

//! Builds the vertical-bone aspect fixture and returns its geometry
//! (a capsule: equal end radii, so its mid-section is exactly a circle
//! of radius `r` before the squash, an ellipse after).
void RunAspectAxisCase( double aspect )
{
	const double r = 0.5, len = 2.0, Rbig = 50.0;
	char buf[128];
	std::vector<std::string> joints;
	joints.push_back( "a none 0 0 0 0.5" );
	std::snprintf( buf, sizeof(buf), "b a 0 %g 0 %g %g", len, r, aspect );
	joints.push_back( buf );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
	std::snprintf( buf, sizeof(buf), "axis_%g", aspect );
	// blend is irrelevant for a ONE-part skeleton (Map's fold starts at
	// 1e30, so the first smin is a plain union), but spell 0 anyway so
	// the closed forms below are unconditional.
	const bool ok = ParseBodyInto( buf, SkeletonChunk( "flatbone", joints, "0" ), *job );
	Check( ok, std::string("aspect ") + std::to_string(aspect) + ": parses" );
	const IGeometry* geo = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "flatbone" ) : 0;
	if( !geo ) { Check( false, "aspect: geometry retrievable" ); safe_release( job ); return; }

	// (b)+(c) -- ALONG WORLD X, the flattened axis: x = aspect*sqrt(r^2 - z^2).
	{
		static const double kZ[] = { 0.0, 0.2, 0.3 };
		bool all = true;
		double worst = 0.0;
		for( std::size_t i = 0; i < sizeof(kZ)/sizeof(kZ[0]); ++i ) {
			const double wantX = aspect * std::sqrt( r*r - kZ[i]*kZ[i] );
			RayIntersectionGeometric ri = MkRI( Point3( Rbig, 1.0, kZ[i] ), Vector3(-1,0,0) );
			geo->IntersectRay( ri, true, true, false );
			const double got = ri.bHit ? (double)ri.range : -1.0;
			const double d = std::fabs( got - ( Rbig - wantX ) );
			if( !( ri.bHit && d <= 5e-3 ) ) { all = false; }
			if( d > worst ) worst = d;
			std::cout << "    aspect " << aspect << " X-probe z=" << kZ[i] << ": half-width "
			          << ( Rbig - got ) << "  want " << wantX << std::endl;
		}
		Check( all, std::string("aspect ") + std::to_string(aspect) + ": MONEY ASSERTION -- the flattened "
		       "axis is WORLD X for a vertical bone, at exactly aspect*sqrt(r^2-z^2) (pure squash; "
		       "area-preserving would read sqrt(aspect) times that), worst miss " + std::to_string(worst) );
	}

	// (b)+(c) -- ALONG WORLD Z, the axis aspect leaves alone:
	// z = r*sqrt(1 - (x/(aspect*r))^2), and at x=0 that is EXACTLY the
	// declared radius -- `radius` still means what it always meant.
	{
		static const double kX[] = { 0.0, 0.5, 0.8 };   // as a FRACTION of the squashed half-width
		bool all = true;
		double worst = 0.0;
		for( std::size_t i = 0; i < sizeof(kX)/sizeof(kX[0]); ++i ) {
			const double x = kX[i] * aspect * r;
			const double wantZ = r * std::sqrt( 1.0 - kX[i]*kX[i] );
			RayIntersectionGeometric ri = MkRI( Point3( x, 1.0, Rbig ), Vector3(0,0,-1) );
			geo->IntersectRay( ri, true, true, false );
			const double got = ri.bHit ? (double)ri.range : -1.0;
			const double d = std::fabs( got - ( Rbig - wantZ ) );
			if( !( ri.bHit && d <= 5e-3 ) ) { all = false; }
			if( d > worst ) worst = d;
			std::cout << "    aspect " << aspect << " Z-probe x=" << x << ": half-depth "
			          << ( Rbig - got ) << "  want " << wantZ << std::endl;
		}
		Check( all, std::string("aspect ") + std::to_string(aspect) + ": MONEY ASSERTION -- the UNflattened "
		       "axis is world Z and reads EXACTLY the declared radius at x=0, so `radius` keeps its "
		       "meaning and aspect never makes a bone THICKER, worst miss " + std::to_string(worst) );
	}

	// Local Y is untouched: the tip cap still sits at the child joint.
	{
		RayIntersectionGeometric ri = MkRI( Point3( 0, Rbig, 0 ), Vector3(0,-1,0) );
		geo->IntersectRay( ri, true, true, false );
		const double want = Rbig - ( len + r );
		Check( ri.bHit && IsClose( (double)ri.range, want, 5e-3 ),
		       std::string("aspect ") + std::to_string(aspect) + ": the AXIAL extent is untouched -- the "
		       "tip cap is still exactly at the declared child joint (proves the mapping leaves local Y "
		       "at 1, which it must: the roundcone's `c` IS the bone length)" );
	}

	safe_release( job );
}

void TestAspectAxisAndMapping()
{
	std::cout << "Test: aspect -- which axis flattens, and by how much (closed-form ellipse)" << std::endl;
	RunAspectAxisCase( 0.5 );
	RunAspectAxisCase( 0.25 );
	RunAspectAxisCase( 1.6 );    // > 1 widens the same axis; the same closed form holds
}

void TestAspectBlendConservative()
{
	std::cout << "Test: aspect -- smin between a squashed bone and a round one stays conservative" << std::endl;

	// A very flat vertical bone (aspect 0.18 -> a 0.09 half-width slab)
	// joined at its tip to a round bone angled away.  If the composed
	// field were NOT capped at 1-Lipschitz, the marcher would step past
	// the slab and rays would MISS.
	std::vector<std::string> joints;
	joints.push_back( "a none 0 0 0 0.5" );
	joints.push_back( "b a 0 2 0 0.5 0.18" );
	joints.push_back( "c b 1.1 2.7 0.2 0.4" );

	IJobPriv* jobHard = nullptr;
	IJobPriv* jobSoft = nullptr;
	if( !RISE_CreateJobPriv( &jobHard ) || !jobHard ) { Check( false, "job created" ); return; }
	if( !RISE_CreateJobPriv( &jobSoft ) || !jobSoft ) { Check( false, "job created" ); safe_release( jobHard ); return; }

	const bool okH = ParseBodyInto( "cons_hard", SkeletonChunk( "mix", joints, "0" ),   *jobHard );
	const bool okS = ParseBodyInto( "cons_soft", SkeletonChunk( "mix", joints, "0.6" ), *jobSoft );
	Check( okH && okS, "conservativeness: both the hard-union and blended fixtures parse" );
	const IGeometry* gH = ( okH && jobHard->GetGeometries() ) ? jobHard->GetGeometries()->GetItem( "mix" ) : 0;
	const IGeometry* gS = ( okS && jobSoft->GetGeometries() ) ? jobSoft->GetGeometries()->GetItem( "mix" ) : 0;
	if( !gH || !gS ) { Check( false, "conservativeness: both geometries retrievable" ); safe_release(jobHard); safe_release(jobSoft); return; }

	std::vector<double> dH, dS;
	RangeDigest( *gH, Point3( 0.3, 1.6, 0.05 ), 40.0, dH );
	RangeDigest( *gS, Point3( 0.3, 1.6, 0.05 ), 40.0, dS );

	int hitH = 0, tunnelled = 0, inward = 0;
	double worstBulge = 0.0;
	for( std::size_t i = 0; i < dH.size(); ++i ) {
		if( !( dH[i] > 0 ) ) continue;
		++hitH;
		if( !( dS[i] > 0 ) ) { ++tunnelled; continue; }
		const double bulge = dH[i] - dS[i];       // > 0 => the blended surface is FURTHER OUT
		if( bulge < -5e-3 ) ++inward;
		if( bulge > worstBulge ) worstBulge = bulge;
	}
	std::cout << "    conservativeness: " << hitH << " hard-union hits, " << tunnelled
	          << " tunnelled, " << inward << " moved inward (worst outward bulge " << worstBulge << ")" << std::endl;

	Check( hitH >= 20, "conservativeness: the fan actually hits the mixed skeleton" );
	Check( tunnelled == 0,
	       "conservativeness: MONEY ASSERTION -- NOT ONE ray tunnels through the 0.09-half-width squashed "
	       "bone when it is smin-blended against a round one (partEval's minScale keeps the composed "
	       "field <= 1-Lipschitz for any positive per-axis scale)" );
	Check( inward == 0, "conservativeness: blending never pulls the surface INWARD" );
	// NOT bounded by k/4 here, deliberately.  k/4 bounds the FIELD offset
	// along the surface NORMAL; a ray that grazes the fillet filling the
	// crevice between two bones converts that normal offset into a range
	// change of k/4 / |cos theta|, which is unbounded as the ray goes
	// tangent (measured 0.35 on this fan for k/4 = 0.06).  The k/4 claim
	// is pinned properly, at normal incidence, by TestBlendSemantics.
	// What IS asserted at normal incidence here is that blending leaves
	// the squashed bone's own field UNTOUCHED away from the junction:
	// mid-bone, the ellipse half-width is still exactly aspect*r whether
	// or not a round neighbour is being blended in.
	{
		const double wantX = 0.18 * 0.5;
		RayIntersectionGeometric riH = MkRI( Point3( 50, 1.0, 0 ), Vector3(-1,0,0) );
		RayIntersectionGeometric riS = MkRI( Point3( 50, 1.0, 0 ), Vector3(-1,0,0) );
		gH->IntersectRay( riH, true, true, false );
		gS->IntersectRay( riS, true, true, false );
		std::cout << "    conservativeness mid-bone half-width: hard " << ( 50.0 - (double)riH.range )
		          << "  blended " << ( 50.0 - (double)riS.range ) << "  want " << wantX << std::endl;
		Check( riH.bHit && IsClose( 50.0 - (double)riH.range, wantX, 5e-3 ) &&
		       riS.bHit && IsClose( 50.0 - (double)riS.range, wantX, 5e-3 ),
		       "conservativeness: away from the junction the squashed bone still measures EXACTLY "
		       "aspect*radius across the flattened axis, blended or not" );
	}

	safe_release( jobHard );
	safe_release( jobSoft );
}

void TestAspectInertOnRootWarns()
{
	std::cout << "Test: aspect -- an aspect that can shape nothing is WARNED about, not dropped in silence" << std::endl;

	// `hips` is a root WITH a child: no bone arrives at it and it emits
	// no part of its own, so its aspect is inert.
	std::vector<std::string> joints;
	joints.push_back( "hips none 0 1 0 0.4 0.5" );
	joints.push_back( "knee hips 0 0.3 0 0.3" );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	// Same fd-dup/dup2 capture TestDegenerateBoneAABB uses for the other
	// non-rejecting diagnostic this chunk owns.
	std::string captured;
	bool ok = false;
	{
		const char* tmpEnv = getenv( "TMPDIR" );
		std::string dir = tmpEnv ? tmpEnv : "/tmp/";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
		char pidbuf[32];
		std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
		const std::string capPath = dir + "rise_skeleton_geo_inert_stdout_" + pidbuf + ".txt";

		std::fflush( stdout );
		const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
		FILE* capFile = std::fopen( capPath.c_str(), "w" );
		if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

		ok = ParseBodyInto( "inert", SkeletonChunk( "legs", joints, "0.3" ), *job );

		std::fflush( stdout );
		if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
		if( capFile ) std::fclose( capFile );

		std::ifstream ifs( capPath.c_str() );
		if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); captured = oss.str(); }
		remove( capPath.c_str() );
	}

	Check( ok, "inert aspect: the fixture parses (this is a WARNING, not a rejection)" );
	Check( captured.find( "skeleton_geometry" ) != std::string::npos &&
	       captured.find( "`hips`" ) != std::string::npos &&
	       captured.find( "shapes" ) != std::string::npos,
	       "inert aspect: MONEY ASSERTION -- the diagnostic fires and NAMES the joint whose aspect is inert" );

	// The mirror case must be SILENT: the same skeleton with the aspect
	// on the CHILD (where it does shape the arriving bone) warns about
	// nothing, so the check above is not just "any parse logs something".
	{
		std::vector<std::string> good;
		good.push_back( "hips none 0 1 0 0.4" );
		good.push_back( "knee hips 0 0.3 0 0.3 0.5" );

		IJobPriv* job2 = nullptr;
		if( RISE_CreateJobPriv( &job2 ) && job2 ) {
			std::string cap2;
			const char* tmpEnv = getenv( "TMPDIR" );
			std::string dir = tmpEnv ? tmpEnv : "/tmp/";
			if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
			char pidbuf[32];
			std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
			const std::string capPath = dir + "rise_skeleton_geo_inert2_stdout_" + pidbuf + ".txt";

			std::fflush( stdout );
			const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
			FILE* capFile = std::fopen( capPath.c_str(), "w" );
			if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

			const bool ok2 = ParseBodyInto( "inert_ok", SkeletonChunk( "legs2", good, "0.3" ), *job2 );

			std::fflush( stdout );
			if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
			if( capFile ) std::fclose( capFile );

			std::ifstream ifs( capPath.c_str() );
			if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); cap2 = oss.str(); }
			remove( capPath.c_str() );

			Check( ok2 && cap2.find( "shapes" ) == std::string::npos,
			       "inert aspect: NON-TAUTOLOGY -- the SAME skeleton with the aspect on the child (where "
			       "it shapes the arriving bone) warns about nothing" );
			safe_release( job2 );
		}
	}

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 4d -- NEAR-VERTICAL STABILITY (doc 90 R4, P2-2).  Pre-fix,
// DirectionToEulerDeg sent horiz < 1e-9 to a canonical (ex=0/180, ey=0)
// branch and everything else -- including a bone at (0.001, 0.999999, 0),
// noise in an author's third decimal -- through raw
// ey = atan2(dx,dz), which is maximally sensitive to approach direction
// as (dx,dz) -> (0,0).  The fix widens the canonical band to
// kNearVerticalHoriz (~0.6 degrees of lean) AND, inside that band, spends
// the previously-always-zero roll angle ez on a Gram-Schmidt-stabilized
// flatten axis instead of snapping ex/ey to exact-vertical values -- see
// DirectionToEulerDeg's own comment for the derivation of why that keeps
// the tip cap EXACT while the flatten axis stays close to (and, in the
// dx=0 case, exactly at) world X regardless of which tiny horizontal
// direction the lean points.
//
// This section proves three things: (a) endpoints stay exact at a 0.3
// degree lean, several azimuths (RunPoseCase's ground-truth ray
// technique, independent of the chunk's own rotation math); (b) the
// flatten axis reads the SAME world-X-flattened / world-Z-full closed
// form regardless of azimuth at that lean -- the MONEY assertion, and
// exactly what flips with a 90 degree jump pre-fix; (c) a CLEARLY leaning
// bone (5 degrees, well above threshold) still uses the documented
// lean-derived axis, not the canonical one -- proving the fix narrowed
// the snap band rather than widening it into the regime authors actually
// lean bones in.
//////////////////////////////////////////////////////////////////////

void TestNearVerticalEndpointExact()
{
	std::cout << "Test: near-vertical -- the tip cap stays EXACT at a fractional-degree lean, any azimuth" << std::endl;
	// 0.3 degrees is inside kNearVerticalHoriz (~0.57 degrees); several
	// azimuths, including the pure-dx-lean the bug report's
	// (0.001, 0.999999, 0) example is drawn from.
	RunPoseCase( "nearvert_0deg_az",   std::sin(0.3*PI/180.0), std::cos(0.3*PI/180.0), 0.0 );
	RunPoseCase( "nearvert_37deg_az",  std::sin(0.3*PI/180.0)*std::cos(37.0*PI/180.0),
	             std::cos(0.3*PI/180.0), std::sin(0.3*PI/180.0)*std::sin(37.0*PI/180.0) );
	RunPoseCase( "nearvert_90deg_az",  0.0, std::cos(0.3*PI/180.0), std::sin(0.3*PI/180.0) );
	RunPoseCase( "nearvert_200deg_az", std::sin(0.3*PI/180.0)*std::cos(200.0*PI/180.0),
	             std::cos(0.3*PI/180.0), std::sin(0.3*PI/180.0)*std::sin(200.0*PI/180.0) );
}

//! Test-side mirror of the FIRST column (the `aspect` flatten axis)
//! RecomputePartDerived computes from (eyDeg,ezDeg) -- SDFGeometry.cpp's
//! `pt.cx = (cz*cy, sz*cy, -sy)` with ez=0 forced only outside the
//! near-vertical band; here `eyDeg`/`ezDeg` come straight from
//! DirToEulerDeg above so this reproduces whatever branch it took.
static Vector3 FlattenAxisFromEuler( double eyDeg, double ezDeg )
{
	const double ey = eyDeg * PI / 180.0, ez = ezDeg * PI / 180.0;
	return Vector3( std::cos(ez)*std::cos(ey), std::sin(ez)*std::cos(ey), -std::sin(ey) );
}

//! For one bone direction (unit dx,dy,dz) and `aspect`: build a 2-joint
//! skeleton (root at origin -> child at dx*len,dy*len,dz*len), then probe
//! EXACTLY along the computed flatten axis (cx, from the mirror above)
//! and its perpendicular partner (cz = cx x direction) THROUGH THE TRUE
//! BONE CENTRE (direction*len/2 -- NOT a fixed world point, which is only
//! the true centre when the bone is exactly vertical).  This is the
//! closed-form ellipse probe RunAspectAxisCase uses, generalized to an
//! arbitrary tilt: probing along the TRUE local axes through the TRUE
//! centre is EXACT (no residual from the bone no longer being
//! world-Y-aligned), so the same tight tolerance (5e-3, sphere-trace
//! numerical precision only) applies regardless of lean.
//!
//! The STABILITY claim -- that the flatten axis does not flip with the
//! lean's azimuth -- is checked separately and more directly: cx itself,
//! compared against world +X, with NO ray tracing involved (see callers).
void RunTiltedAxisCase( double dx, double dy, double dz, double aspect,
                         double exDeg, double eyDeg, double ezDeg,
                         const char* label )
{
	const double r = 0.5, len = 2.0, Rbig = 50.0;
	(void)exDeg;

	char buf[160];
	std::vector<std::string> joints;
	joints.push_back( "a none 0 0 0 0.5" );
	std::snprintf( buf, sizeof(buf), "b a %.17g %.17g %.17g %g %g", dx*len, dy*len, dz*len, r, aspect );
	joints.push_back( buf );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
	const bool ok = ParseBodyInto( std::string("tilt_") + label, SkeletonChunk( "flatbone", joints, "0" ), *job );
	Check( ok, std::string("tilted axis ") + label + ": parses" );
	const IGeometry* geo = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "flatbone" ) : 0;
	if( !geo ) { Check( false, std::string("tilted axis ") + label + ": geometry retrievable" ); safe_release( job ); return; }

	const Vector3 d( dx, dy, dz );
	const Vector3 cx = FlattenAxisFromEuler( eyDeg, ezDeg );
	const Vector3 cz = Vector3Ops::Cross( cx, d );          // matches RecomputePartDerived's cz = cx x cy
	const Point3  M( d.x*len*0.5, d.y*len*0.5, d.z*len*0.5 ); // TRUE bone centre, not a fixed world point

	auto probe = [&]( const Vector3& axis, double want, const char* axisName )
	{
		const Point3 origin( M.x - axis.x*Rbig, M.y - axis.y*Rbig, M.z - axis.z*Rbig );
		RayIntersectionGeometric ri = MkRI( origin, axis );
		geo->IntersectRay( ri, true, true, false );
		const double got = ri.bHit ? (double)ri.range : -1.0;
		const double miss = std::fabs( got - ( Rbig - want ) );
		std::cout << "    tilted " << label << " " << axisName << ": half-width " << ( Rbig - got )
		          << "  want " << want << std::endl;
		Check( ri.bHit && miss <= 5e-3, std::string("tilted axis ") + label + ": " + axisName +
		       ", miss " + std::to_string(miss) );
	};

	probe( cx, aspect * r, "flatten-axis (aspect*r, MONEY ASSERTION)" );
	probe( cz, r,          "perpendicular axis (full r)" );

	safe_release( job );
}

void TestNearVerticalAxisStability()
{
	std::cout << "Test: near-vertical -- the flatten axis is STABLE across azimuth at a fixed tiny lean" << std::endl;
	// Same 0.3 degree lean (inside kNearVerticalHoriz, ~0.57 degrees),
	// four different horizontal directions.  A pre-fix (or
	// reverted-threshold) run computes a flatten axis that FLIPS between
	// roughly world X and world Z depending on which side of the old
	// 1e-9 threshold the azimuth's dx/dz component lands on; the fix
	// keeps cx close to world X for all four -- checked directly (no ray
	// tracing) against a bound of 2x the lean angle, comfortably tight
	// next to the 90 degree flip the bug produced.
	const double leanDeg = 0.3;
	const double azimuths[] = { 0.0, 37.0, 90.0, 200.0 };
	for( double azDeg : azimuths ) {
		const double leanRad = leanDeg * PI / 180.0, azRad = azDeg * PI / 180.0;
		const double dx = std::sin(leanRad) * std::cos(azRad);
		const double dz = std::sin(leanRad) * std::sin(azRad);
		const double dy = std::cos(leanRad);

		double exDeg=0, eyDeg=0, ezDeg=0;
		DirToEulerDeg( dx, dy, dz, exDeg, eyDeg, ezDeg );
		const Vector3 cx = FlattenAxisFromEuler( eyDeg, ezDeg );

		char label[32];
		std::snprintf( label, sizeof(label), "0.3deg_az%g", azDeg );

		const double cosBound = std::cos( 2.0 * leanRad );
		std::cout << "    near-vertical az=" << azDeg << ": cx=(" << cx.x << "," << cx.y << "," << cx.z
		          << ")  dot(cx,worldX)=" << cx.x << "  bound=" << cosBound << std::endl;
		Check( cx.x >= cosBound, std::string("near-vertical axis stability ") + label +
		       ": MONEY ASSERTION -- the flatten axis stays within 2x the lean angle of world X, "
		       "not flipped to a perpendicular axis by lean-direction noise (dot=" +
		       std::to_string(cx.x) + ", bound=" + std::to_string(cosBound) + ")" );

		RunTiltedAxisCase( dx, dy, dz, 0.5, exDeg, eyDeg, ezDeg, label );
	}
}

void TestClearlyLeaningAxisUnchanged()
{
	std::cout << "Test: clearly-leaning (5 degrees) -- still the DOCUMENTED lean-derived axis, not canonical" << std::endl;
	// Pure +X lean (azimuth 0, dz = 0 exactly, well above the ~0.6 degree
	// threshold): ey = atan2(dx,0) = 90 degrees, ez = 0 (the ORIGINAL,
	// UNCHANGED formula), so local +X maps to world -Z -- the flattened
	// axis is world Z here, the OPPOSITE assignment from the
	// near-vertical/canonical case above, proving the widened threshold
	// did not creep out to 5 degrees.
	const double leanDeg = 5.0;
	const double leanRad = leanDeg * PI / 180.0;
	const double dx = std::sin(leanRad), dy = std::cos(leanRad), dz = 0.0;

	double exDeg=0, eyDeg=0, ezDeg=0;
	DirToEulerDeg( dx, dy, dz, exDeg, eyDeg, ezDeg );
	Check( ezDeg == 0.0, "clearly-leaning: ez is exactly 0 -- the fix's near-vertical branch did not fire "
	       "at 5 degrees of lean" );
	const Vector3 cx = FlattenAxisFromEuler( eyDeg, ezDeg );
	Check( std::fabs( cx.z ) > 0.99, "clearly-leaning: MONEY ASSERTION -- the flatten axis is world Z "
	       "(cx.z close to +-1), not world X -- the LEAN-DERIVED axis the pre-existing formula always "
	       "used, unaffected by the near-vertical fix (cx=(" + std::to_string(cx.x) + "," +
	       std::to_string(cx.y) + "," + std::to_string(cx.z) + "))" );

	RunTiltedAxisCase( dx, dy, dz, 0.5, exDeg, eyDeg, ezDeg, "leaning5deg" );
}

//! Extends TestAspectBackCompat's back-compat digest style to a
//! genuinely near-vertical (0.3 degree) lean, ONE azimuth: pure +Z lean
//! (dx = 0 exactly).  DirectionToEulerDeg's comment explains why dx = 0
//! is special -- it is the ONE azimuth where the near-vertical
//! Gram-Schmidt fix and the pre-fix formula AGREE exactly (both give
//! ey = ez = 0), so this is a genuine bit-for-bit (worst |delta| == 0.0,
//! not merely close) regression pin for a lean well inside the widened
//! band.  At any OTHER azimuth in the band the fix's nonzero roll and the
//! unfixed formula's implicit zero roll are only field-EQUIVALENT for
//! aspect 1 (the shape is rotationally symmetric about the bone axis, so
//! both describe the identical solid), not bit-identical -- floating
//! point is not associative under an arbitrary in-plane rotation, so
//! bit-for-bit is not a claim this fixture can honestly make off-axis.
void TestAspectOneNearVerticalBitIdentity()
{
	std::cout << "Test: aspect 1 -- a near-vertical (0.3 degree) bone still digests bit-identically" << std::endl;

	const double leanDeg = 0.3, len = 2.0, r = 0.5;
	const double dz = len * std::sin( leanDeg * PI / 180.0 );
	const double dy = len * std::cos( leanDeg * PI / 180.0 );

	char buf[160];
	std::vector<std::string> joints;
	joints.push_back( "a none 0 0 0 0.5" );
	std::snprintf( buf, sizeof(buf), "b a 0 %.17g %.17g %g", dy, dz, r );
	joints.push_back( buf );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
	const bool ok = ParseBodyInto( "nv_aspect1", SkeletonChunk( "nvbone", joints, "0" ), *job );
	Check( ok, "near-vertical aspect1: parses" );
	const IGeometry* actual = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "nvbone" ) : 0;
	if( !actual ) { Check( false, "near-vertical aspect1: geometry retrievable" ); safe_release( job ); return; }

	double exDeg=0, eyDeg=0, ezDeg=0;
	DirToEulerDeg( 0.0, dy/len, dz/len, exDeg, eyDeg, ezDeg );
	Check( ezDeg == 0.0 && eyDeg == 0.0, "near-vertical aspect1: pure-Z-axis lean gives ez = ey = 0 from "
	       "the mirror formula, so this fixture is a genuine bit-exactness pin, not merely a "
	       "field-equivalence one" );

	std::vector<SDFGeometry::Part> refParts;
	refParts.push_back( SDFGeometry::MakePart( SDFGeometry::ePrimRoundCone, SDFGeometry::eOpSmin,
		0.0, Point3(0,0,0), exDeg, eyDeg, ezDeg, Vector3(1,1,1), 0.5, r, len, 0 ) );
	SDFGeometry* reference = new SDFGeometry( refParts, 256, 0.0, 64 );

	std::vector<double> dNew, dOld;
	RangeDigest( *actual,    Point3( 0, 1, 0 ), 40.0, dNew );
	RangeDigest( *reference, Point3( 0, 1, 0 ), 40.0, dOld );

	bool same = ( dNew.size() == dOld.size() && !dNew.empty() );
	Check( same, "near-vertical aspect1: both digests have the same 30 entries" );
	int hits = 0;
	double worst = 0.0;
	if( same ) {
		for( std::size_t i = 0; i < dNew.size(); ++i ) {
			if( dNew[i] > 0 ) ++hits;
			const double d = std::fabs( dNew[i] - dOld[i] );
			if( d > worst ) worst = d;
		}
	}
	std::cout << "    near-vertical aspect1 digest: " << hits << "/" << dNew.size()
	          << " rays hit, worst |delta| = " << worst << std::endl;
	Check( hits >= 12, "near-vertical aspect1: the digest fan actually hits the geometry" );
	Check( same && worst == 0.0, "near-vertical aspect1: MONEY ASSERTION -- bit-for-bit identical to the "
	       "mirror-built reference at a genuinely near-vertical (0.3 degree) lean, worst " + std::to_string(worst) );

	safe_release( job );
	safe_release( reference );
}

//////////////////////////////////////////////////////////////////////
// 4e -- ISOLATED JOINT ASPECT (P2-1): the lone joint's `sphere` part
// flattens along world X unconditionally too (no bone direction exists
// to derive an axis from -- world X is the only sensible canonical
// choice, and there is no near-vertical ambiguity to stabilize because
// there is no direction at all).  Closed-form probe: half-width aspect*r
// along world X, r (untouched) along world Y and world Z.
//////////////////////////////////////////////////////////////////////

void RunIsolatedAspectCase( double aspect )
{
	const double r = 0.6, Rbig = 50.0;
	const Point3 C( 2.0, 3.0, -1.0 );
	char buf[128];
	std::vector<std::string> joints;
	std::snprintf( buf, sizeof(buf), "lonely none %.17g %.17g %.17g %g %g", C.x, C.y, C.z, r, aspect );
	joints.push_back( buf );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }
	char tag[32];
	std::snprintf( tag, sizeof(tag), "iso_%g", aspect );
	const bool ok = ParseBodyInto( tag, SkeletonChunk( "orb", joints ), *job );
	Check( ok, std::string("isolated aspect ") + std::to_string(aspect) + ": parses" );
	const IGeometry* geo = ( ok && job->GetGeometries() ) ? job->GetGeometries()->GetItem( "orb" ) : 0;
	if( !geo ) { Check( false, "isolated aspect: geometry retrievable" ); safe_release( job ); return; }

	auto probe = [&]( const Vector3& dir, double want, const char* axisName )
	{
		const Point3 origin( C.x - dir.x*Rbig, C.y - dir.y*Rbig, C.z - dir.z*Rbig );
		RayIntersectionGeometric ri = MkRI( origin, dir );
		geo->IntersectRay( ri, true, true, false );
		const double got = ri.bHit ? (double)ri.range : -1.0;
		const double d = std::fabs( got - ( Rbig - want ) );
		std::cout << "    isolated aspect " << aspect << " " << axisName << ": half-width " << ( Rbig - got )
		          << "  want " << want << std::endl;
		Check( ri.bHit && d <= 5e-3, std::string("isolated aspect ") + std::to_string(aspect) + ": " +
		       axisName + ", miss " + std::to_string(d) );
	};

	probe( Vector3(1,0,0), aspect*r, "world X (flattened, MONEY ASSERTION)" );
	probe( Vector3(0,1,0), r,        "world Y (untouched)" );
	probe( Vector3(0,0,1), r,        "world Z (untouched)" );

	safe_release( job );
}

void TestIsolatedJointAspect()
{
	std::cout << "Test: isolated joint -- aspect flattens world X unconditionally (closed-form)" << std::endl;
	RunIsolatedAspectCase( 0.5 );
	RunIsolatedAspectCase( 0.3 );
}

//////////////////////////////////////////////////////////////////////
// 5 -- rejections
//////////////////////////////////////////////////////////////////////

void TestRejections()
{
	std::cout << "Test: skeleton_geometry rejection paths" << std::endl;

	Check( ParseBody( "ok", SkeletonChunk( "s", GoodChain() ) ),
	       "reject-baseline: the 3-joint chain parses" );

	// No joints at all.
	Check( !ParseBody( "nojoints", SkeletonChunk( "s", std::vector<std::string>() ) ),
	       "reject: no `joint` lines at all" );

	// Malformed joint lines: wrong token counts, non-numeric, trailing garbage.
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0" );   // 5 tokens -- missing radius
		Check( !ParseBody( "5tok", SkeletonChunk( "s", j ) ), "reject: joint line with 5 tokens (missing radius)" );
	}
	{
		// DOC 90 SLICE B moved this case's REASON, deliberately: 7 tokens
		// is now a legal arity (the 7th is `aspect`), so what rejects this
		// line is no longer the token COUNT but the trailing token failing
		// the numeric run.  Same input, same verdict, different mechanism
		// -- kept because trailing garbage is exactly what an author
		// typos, and the 8-token case below now carries the arity half.
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 0.5 extra" );   // 7 tokens, non-numeric aspect
		Check( !ParseBody( "7tok", SkeletonChunk( "s", j ) ), "reject: joint line whose 7th token is not a number" );
	}
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 0.5 1.0 extra" );   // 8 tokens
		Check( !ParseBody( "8tok", SkeletonChunk( "s", j ) ), "reject: joint line with 8 tokens (past the optional aspect)" );
	}
	// aspect is a RATIO: 0 and negative are meaningless, not merely extreme.
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 0.5 0" );
		Check( !ParseBody( "asp0", SkeletonChunk( "s", j ) ), "reject: aspect 0" );
	}
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 0.5 -0.6" );
		Check( !ParseBody( "aspneg", SkeletonChunk( "s", j ) ), "reject: a negative aspect" );
	}
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 0.5 nan" );
		Check( !ParseBody( "aspnan", SkeletonChunk( "s", j ) ), "reject: a non-finite aspect (`nan`)" );
	}
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 0.5 0.6abc" );
		Check( !ParseBody( "aspglued", SkeletonChunk( "s", j ) ), "reject: aspect with glued trailing garbage" );
	}
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 0.5 0.6" );
		Check( ParseBody( "aspok", SkeletonChunk( "s", j ) ), "reject: a positive aspect on an isolated joint is ACCEPTED" );
	}
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 abc" );   // non-numeric radius
		Check( !ParseBody( "nonnum", SkeletonChunk( "s", j ) ), "reject: non-numeric radius" );
	}
	{
		std::vector<std::string> j;
		j.push_back( "root none 0 0 0 0.5abc" );   // numeric with glued trailing garbage
		Check( !ParseBody( "glued", SkeletonChunk( "s", j ) ), "reject: radius with glued trailing garbage" );
	}

	// Duplicate joint name.
	{
		std::vector<std::string> j = GoodChain();
		j.push_back( "root none 9 9 9 0.3" );   // reuses `root`
		Check( !ParseBody( "dup", SkeletonChunk( "s", j ) ), "reject: duplicate joint name `root`" );
	}

	// Unknown parent (never declared anywhere in the chunk).
	{
		std::vector<std::string> j;
		j.push_back( "a none 0 0 0 0.3" );
		j.push_back( "b ghost 1 1 1 0.3" );
		Check( !ParseBody( "unknown", SkeletonChunk( "s", j ) ), "reject: `parent` names a joint that is never declared" );
	}

	// Forward-referenced parent (declared, but LATER in the chunk).
	{
		std::vector<std::string> j;
		j.push_back( "a later 0 0 0 0.3" );   // `later` not yet declared at this point
		j.push_back( "later none 1 1 1 0.3" );
		Check( !ParseBody( "forward", SkeletonChunk( "s", j ) ),
		       "reject: MONEY ASSERTION -- `parent` names a joint declared LATER in the chunk "
		       "(declare-before-use, the mechanism that makes cycles impossible)" );
	}

	// Self-parent.
	{
		std::vector<std::string> j;
		j.push_back( "a a 0 0 0 0.3" );
		Check( !ParseBody( "self", SkeletonChunk( "s", j ) ), "reject: a joint naming itself as `parent`" );
	}

	// Non-positive radius.
	{
		std::vector<std::string> j;
		j.push_back( "a none 0 0 0 0" );
		Check( !ParseBody( "r0", SkeletonChunk( "s", j ) ), "reject: radius 0" );
	}
	{
		std::vector<std::string> j;
		j.push_back( "a none 0 0 0 -1" );
		Check( !ParseBody( "rneg", SkeletonChunk( "s", j ) ), "reject: a negative radius" );
	}

	// Coincident parent/child position -- a zero-length bone.
	{
		std::vector<std::string> j;
		j.push_back( "a none 1 2 3 0.4" );
		j.push_back( "b a 1 2 3 0.3" );   // same position as `a`
		Check( !ParseBody( "coincident", SkeletonChunk( "s", j ) ),
		       "reject: a child at the EXACT same position as its parent (zero-length bone)" );
	}

	// Negative blend.
	Check( !ParseBody( "blendneg", SkeletonChunk( "s", GoodChain(), "-0.1" ) ),
	       "reject: a negative `blend`" );
	Check( ParseBody( "blendzero", SkeletonChunk( "s", GoodChain(), "0" ) ),
	       "reject: `blend 0` is accepted (hard union, not a rejection)" );
}

//////////////////////////////////////////////////////////////////////
// 6 -- round-trip
//////////////////////////////////////////////////////////////////////

void TestRoundTrip()
{
	std::cout << "Test: a skeleton_geometry scene round-trips as the COMPACT form" << std::endl;

	const std::string body = SkeletonChunk( "critter", GoodChain(), "0.4" ) +
		"lambertian_material\n{\n\tname\t\tcritter_mat\n\treflectance\tnone\n}\n"
		"standard_object\n{\n\tname\t\tcritter_obj\n\tgeometry\tcritter\n\tmaterial\tcritter_mat\n}\n";
	const std::string text = "RISE ASCII SCENE 7\n" + body;
	const std::string path = WriteTempScene( "roundtrip", text );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); remove( path.c_str() ); return; }

	const bool loaded = job->LoadAsciiSceneViaCst( path.c_str() );
	Check( loaded, "roundtrip: the scene file loads" );
	Check( job->GetObjects() && job->GetObjects()->GetItem( "critter_obj" ) != 0,
	       "roundtrip: the skeleton_geometry derived into a usable geometry, bound to an object" );

	Check( job->HasRetainedCstDocument(),
	       "roundtrip: the Job retains a CST Document (what save serializes)" );
	const RISE::Cst::Document* doc = job->GetCstDocument();
	if( doc ) {
		const std::string saved = RISE::Cst::SerializeCst( *doc );
		Check( saved.find( "skeleton_geometry" ) != std::string::npos,
		       "roundtrip: the saved text still contains `skeleton_geometry` (the compact form persists)" );
		Check( saved.find( "sdf_geometry" ) == std::string::npos &&
		       saved.find( "roundcone" ) == std::string::npos,
		       "roundtrip: the save does NOT write out the expanded sdf_geometry parts" );
		Check( saved == text, "roundtrip: an unedited save is byte-identical to the source file" );
	} else {
		Check( false, "roundtrip: CST Document retrievable" );
	}

	safe_release( job );
	remove( path.c_str() );
}

} // anonymous namespace

int main()
{
	std::cout << "=== SkeletonGeometryChunkTest ===" << std::endl;

	TestExpansionAndNaming();
	TestPoseMath();
	TestDegenerateBoneAABB();
	TestBlendSemantics();
	TestNoRedundantJointSphere();
	TestIsolatedJoint();
	TestBranchingJoint();
	TestAspectBackCompat();
	TestAspectAxisAndMapping();
	TestAspectBlendConservative();
	TestAspectInertOnRootWarns();
	TestNearVerticalEndpointExact();
	TestNearVerticalAxisStability();
	TestClearlyLeaningAxisUnchanged();
	TestAspectOneNearVerticalBitIdentity();
	TestIsolatedJointAspect();
	TestRejections();
	TestRoundTrip();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
