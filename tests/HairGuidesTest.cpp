//////////////////////////////////////////////////////////////////////
//
//  HairGuidesTest.cpp - Contract test for AUTHORED GUIDE STRANDS: the
//    `hair_guides` chunk and `hair_geometry`'s `guides` field (Phase 2
//    slice P2-A of the hair/fur arc, docs/HAIR_FUR_DESIGN.md section
//    5.3 / section 7).
//
//  WHAT THIS TEST OWNS, and why it is its own file.  HairGeneratorTest
//  owns the recipe -> strands transformation of an UNGUIDED groom (root
//  placement, the density mask, comb / gravity / curl / frizz / clump)
//  and pins 106 checks on it.  This file owns exactly the delta guides
//  introduce -- which growth term they replace, what they leave alone,
//  and the chunk that authors them -- so neither suite has to be read to
//  understand the other.
//
//  THE ARITHMETIC IS CHECKED AGAINST A CLOSED FORM, NOT AGAINST ITSELF.
//  Every positional assertion below recomputes the expected control
//  point in the test from the documented RULE (resample the guide by
//  arc-length fraction, express it in the base surface's frame at the
//  guide's root, replay it in the strand's frame at the strand's root,
//  scale by the strand's length).  Precisely what "independent" means
//  here, stated honestly: the RULE itself -- arc-length fraction,
//  linear interpolation between the two bracketing points -- is
//  deliberately the SAME rule the generator implements, because that
//  rule is the spec (docs/HAIR_FUR_DESIGN.md section 5.3), not a detail
//  either side is free to reinvent.  What must NOT be shared is the
//  CODE PATH that locates the bracketing span: `GuideLocalWorld` below
//  finds it via `std::upper_bound` over a cumulative arc-length table
//  (a binary search), while HairGenerator.cpp's `PrepareGuides` finds it
//  by walking a `seg` cursor forward one breakpoint at a time (a linear
//  scan).  Two structurally different searches over the same table can
//  only land on the same interpolated point if the underlying rule is
//  actually implemented correctly on both sides -- a shared off-by-one
//  in a common "advance while" idiom would no longer pass both, because
//  there is no such idiom left to share.  The base fixtures are chosen
//  to make the arithmetic itself tractable: the unit quad's UV-derived
//  frame is exactly the world axes {X, Y, Z}, so on it the transport is
//  a pure translate and the expected point is one line of algebra.
//  Group 6 then uses a SECOND, differently-oriented face precisely so
//  the rotation the flat fixture cannot see is exercised on its own.
//
//  The seven groups:
//
//    1. ONE GUIDE, EXACTLY REPRODUCED.  A single bent guide, and every
//       strand in the groom is that guide -- translated to its own root
//       and scaled by `length` -- to the last float.  Also the two
//       invariants that make guides a STYLING input rather than a
//       placement one: the guided groom's roots are BIT-IDENTICAL to
//       the unguided groom's at the same seed, and a guide that is
//       straight along its root normal reproduces unguided growth
//       exactly.
//
//    2. TWO GUIDES, BLENDED.  Two separated guides with different
//       shapes.  The test computes the inverse-distance weights itself
//       from the guide roots and asserts every control point against
//       the weighted blend; strands near one guide are dominated by it
//       (weight > 0.9), and the blend weight is separately RECOVERED
//       from the generated control points via a 2-unknown linear solve
//       against the guides' own tip shapes, and checked to agree with
//       the same inverse-distance formula (not merely re-asserted from
//       it).
//
//    3. STREAM ISOLATION.  Frizz applied to a guided groom displaces
//       every control point by the SAME jitter vector as frizz applied
//       to the unguided groom at the same seed -- i.e. the interpolation
//       consumes no random draw, so binding `guides` restyles a groom
//       without re-rolling it.
//
//    4. THE CHUNK.  `hair_guides` parses and is usable from a
//       `hair_geometry`; the groom it produces measurably differs from
//       the same scene without the `guides` line (parsing alone would
//       not catch a dropped field).  An unknown guide name, a one-point
//       guide, a ragged (non-multiple-of-three) line, a zero-length
//       guide and a duplicate set name are each REFUSED with a
//       diagnostic.
//
//    5. DETERMINISM.  Two guided grooms from the same recipe are
//       bit-identical; and an unguided groom is unaffected by the
//       presence of the guide code path (two runs identical, and the
//       recipe is byte-unchanged from the pre-guides generator -- that
//       second half was verified once by hand against HEAD, see the
//       slice report; what is permanent here is the two-run check).
//
//    6. RIGID TRANSPORT.  One guide rooted on a +Z-facing face, strands
//       grown on a +X-facing face 10 units away: the guide's lateral
//       lean, authored along the first face's tangent, must come out
//       along the SECOND face's tangent -- which is a different world
//       direction.  This is the assertion that separates a real frame-
//       to-frame transport from a world-space copy.
//
//    7. COVERAGE: THE K-CLAMP AND THE COINCIDENT BRANCH.  Five guides on
//       one base exercise `SelectGuides`' eviction arm -- for every
//       strand, its actual control points are checked against the blend
//       of its own nearest THREE guides (whichever three that is; the
//       other two must contribute EXACTLY zero, not just "very little").
//       A second, separate probe builds a fresh single-strand groom, reads
//       its actual (RNG-placed) root position, then authors a three-guide
//       set where one guide's root is exactly that position -- bit-for-
//       bit, not merely close -- forcing the `best[0] <= kGuideCoincidentEps`
//       branch, and checks the strand collapses to that one guide's shape
//       alone (weight 1, the other two at 0).
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <iostream>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
	#include <process.h>
	#include <io.h>
	#define getpid _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"
#include "../src/Library/SceneEditor/CstIntrospection.h"
#include "../src/Library/Geometry/HairGenerator.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"

using namespace RISE;
using namespace RISE::Implementation;

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

namespace {

template<class T>
class Owned
{
public:
	explicit Owned( T* q = 0 ) : p( q ) {}
	~Owned() { if( p ) p->release(); }
	T*   operator->() const { return p; }
	T&   operator*()  const { return *p; }
	T*   get()        const { return p; }
	explicit operator bool() const { return p != 0; }
private:
	T* p;
	Owned( const Owned& );
	Owned& operator=( const Owned& );
};

//! Control points are stored as FLOAT in HairGeometry (a documented,
//! argued exception to Scalar=double), so a closed-form expectation
//! computed in double agrees only to float precision.  MOST fixtures
//! here work at unit scale, where float's ~7 significant digits give a
//! storage error around 1e-7 and 1e-5 sits a comfortable few orders
//! above it.  Group 6's two-facing fixture (`MakeTwoFacings`) is the
//! exception the earlier version of this comment missed: face B sits at
//! x in [10, 11], and a FIXED digit count means the ABSOLUTE storage
//! error scales with magnitude -- around 1e-6 there, not 1e-7.  At that
//! scale `kPosTol`'s real margin over the storage error is roughly 5x,
//! not the four orders of magnitude the unit-scale fixtures get -- still
//! comfortably tighter than any real formula difference (which shows up
//! at O(0.1), not O(1e-6)), but worth stating accurately rather than
//! implying one generous margin holds everywhere this constant is used.
const double kPosTol = 1e-5;

bool Near( const Point3& a, const Point3& b, const double tol = kPosTol )
{
	return std::fabs( a.x - b.x ) <= tol
	    && std::fabs( a.y - b.y ) <= tol
	    && std::fabs( a.z - b.z ) <= tol;
}

//////////////////////////////////////////////////////////////////////
// Base fixtures
//////////////////////////////////////////////////////////////////////

//! The unit square in z = 0, normals +Z, UV == (x, y) -- the same
//! fixture HairGeneratorTest uses, and chosen for the same reason: the
//! UV-derived root frame works out to EXACTLY the world axes
//! (tangent = +X, bitangent = +Y, normal = +Z) on BOTH triangles, so a
//! closed-form expectation needs no rotation term.
ITriangleMeshGeometryIndexed* MakeUnitQuad()
{
	ITriangleMeshGeometryIndexed* m = 0;
	if( !RISE_API_CreateTriangleMeshGeometryIndexed( &m, true, false ) || !m ) { return 0; }
	m->BeginIndexedTriangles();
	m->AddVertex( Point3( 0, 0, 0 ) );
	m->AddVertex( Point3( 1, 0, 0 ) );
	m->AddVertex( Point3( 1, 1, 0 ) );
	m->AddVertex( Point3( 0, 1, 0 ) );
	for( int i = 0; i < 4; ++i ) { m->AddNormal( Vector3( 0, 0, 1 ) ); }
	m->AddTexCoord( Point2( 0, 0 ) );
	m->AddTexCoord( Point2( 1, 0 ) );
	m->AddTexCoord( Point2( 1, 1 ) );
	m->AddTexCoord( Point2( 0, 1 ) );

	IndexedTriangle t;
	t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2;
	t.iNormals[0]  = 0; t.iNormals[1]  = 1; t.iNormals[2]  = 2;
	t.iCoords[0]   = 0; t.iCoords[1]   = 1; t.iCoords[2]   = 2;
	m->AddIndexedTriangle( t );
	t.iVertices[0] = 0; t.iVertices[1] = 2; t.iVertices[2] = 3;
	t.iNormals[0]  = 0; t.iNormals[1]  = 2; t.iNormals[2]  = 3;
	t.iCoords[0]   = 0; t.iCoords[1]   = 2; t.iCoords[2]   = 3;
	m->AddIndexedTriangle( t );
	m->DoneIndexedTriangles();
	return m;
}

//! TWO unit squares facing different ways, 10 units apart, for the
//! transport test:
//!
//!   face A  z = 0, x,y in [0,1]      -> frame T=(1,0,0) B=(0,1,0) N=(0,0,1)
//!   face B  x = 10, y,z in [0,1]     -> frame T=(0,1,0) B=(0,0,1) N=(1,0,0)
//!
//! Both carry UV = (the two in-plane coordinates), so both frames come
//! from the UV parameterization rather than from an edge, and the two
//! frames share NO axis direction -- which is what makes "the guide's
//! lean followed the frame" and "the guide's lean was copied in world
//! space" two visibly different answers.
ITriangleMeshGeometryIndexed* MakeTwoFacings()
{
	ITriangleMeshGeometryIndexed* m = 0;
	if( !RISE_API_CreateTriangleMeshGeometryIndexed( &m, true, false ) || !m ) { return 0; }
	m->BeginIndexedTriangles();
	// face A (z = 0), CCW about +Z
	m->AddVertex( Point3( 0, 0, 0 ) );
	m->AddVertex( Point3( 1, 0, 0 ) );
	m->AddVertex( Point3( 1, 1, 0 ) );
	m->AddVertex( Point3( 0, 1, 0 ) );
	for( int i = 0; i < 4; ++i ) { m->AddNormal( Vector3( 0, 0, 1 ) ); }
	// face B (x = 10), wound so the geometric normal is +X
	m->AddVertex( Point3( 10, 0, 0 ) );
	m->AddVertex( Point3( 10, 1, 0 ) );
	m->AddVertex( Point3( 10, 1, 1 ) );
	m->AddVertex( Point3( 10, 0, 1 ) );
	for( int i = 0; i < 4; ++i ) { m->AddNormal( Vector3( 1, 0, 0 ) ); }

	m->AddTexCoord( Point2( 0, 0 ) );
	m->AddTexCoord( Point2( 1, 0 ) );
	m->AddTexCoord( Point2( 1, 1 ) );
	m->AddTexCoord( Point2( 0, 1 ) );

	IndexedTriangle t;
	for( int f = 0; f < 2; ++f ) {
		const int v = f * 4;
		t.iVertices[0] = v+0; t.iVertices[1] = v+1; t.iVertices[2] = v+2;
		t.iNormals[0]  = v+0; t.iNormals[1]  = v+1; t.iNormals[2]  = v+2;
		t.iCoords[0]   = 0;   t.iCoords[1]   = 1;   t.iCoords[2]   = 2;
		m->AddIndexedTriangle( t );
		t.iVertices[0] = v+0; t.iVertices[1] = v+2; t.iVertices[2] = v+3;
		t.iNormals[0]  = v+0; t.iNormals[1]  = v+2; t.iNormals[2]  = v+3;
		t.iCoords[0]   = 0;   t.iCoords[1]   = 2;   t.iCoords[2]   = 3;
		m->AddIndexedTriangle( t );
	}
	m->DoneIndexedTriangles();
	return m;
}

//////////////////////////////////////////////////////////////////////
// Recipe plumbing
//////////////////////////////////////////////////////////////////////

//! An authored guide, held the way the test wants to reason about it
//! (a list of points) with the flattening to the descriptor's arrays
//! done at the boundary.
struct Guide
{
	std::vector<Point3> pts;
};

struct FlatGuides
{
	std::vector<double>       points;
	std::vector<unsigned int> counts;

	explicit FlatGuides( const std::vector<Guide>& gs )
	{
		for( std::size_t g = 0; g < gs.size(); ++g ) {
			counts.push_back( (unsigned int)gs[g].pts.size() );
			for( std::size_t k = 0; k < gs[g].pts.size(); ++k ) {
				points.push_back( (double)gs[g].pts[k].x );
				points.push_back( (double)gs[g].pts[k].y );
				points.push_back( (double)gs[g].pts[k].z );
			}
		}
	}
	void Bind( HairGroomRecipe& r ) const
	{
		if( counts.empty() ) { return; }
		r.guidePoints      = &points[0];
		r.guidePointCounts = &counts[0];
		r.numGuides        = (unsigned int)counts.size();
	}
};

HairGroomRecipe PlainRecipe( IGeometry* base, unsigned int count, unsigned int segments = 5 )
{
	HairGroomRecipe r;
	r.pBase        = base;
	r.p.count      = count;
	r.p.segments   = segments;
	r.p.seed       = 7;
	r.p.baseDetail = 4;
	r.p.length     = 0.5;
	r.p.widthRoot  = 0.02;
	r.p.widthTip   = 0.006;
	return r;
}

HairGeometry* BuildAndRealize( const HairGroomRecipe& r )
{
	IGeometry* g = 0;
	if( !RISE_API_CreateHairGeometryGroom( &g, r, "test_groom" ) || !g ) { return 0; }
	g->Realize();
	HairGeometry* h = dynamic_cast<HairGeometry*>( g );
	if( !h ) { g->release(); }
	return h;
}

bool GroomsIdentical( const HairGeometry& a, const HairGeometry& b )
{
	if( a.numStrands() != b.numStrands() ) return false;
	for( unsigned int s = 0; s < a.numStrands(); ++s ) {
		if( a.numControlPointsOfStrand( s ) != b.numControlPointsOfStrand( s ) ) return false;
		for( unsigned int k = 0; k < a.numControlPointsOfStrand( s ); ++k ) {
			const Point3 pa = a.ControlPoint( s, k );
			const Point3 pb = b.ControlPoint( s, k );
			if( !( pa.x == pb.x && pa.y == pb.y && pa.z == pb.z ) ) return false;
		}
	}
	return true;
}

//////////////////////////////////////////////////////////////////////
// The expected-shape arithmetic, written INDEPENDENTLY of the generator
//////////////////////////////////////////////////////////////////////

//! The guide's offset from its own root at arc-length fraction `t`,
//! divided by the guide's total arc length -- the dimensionless shape
//! the descriptor documents.  Locates the bracketing span via
//! `std::upper_bound` (binary search) over the cumulative arc-length
//! table, NOT via the generator's linear "advance while" cursor -- see
//! the file header for exactly what independence claim that does and
//! does not support.
Vector3 GuideLocalWorld( const Guide& g, const double t )
{
	std::vector<double> s( g.pts.size(), 0.0 );
	for( std::size_t k = 1; k < g.pts.size(); ++k ) {
		const double dx = g.pts[k].x - g.pts[k-1].x;
		const double dy = g.pts[k].y - g.pts[k-1].y;
		const double dz = g.pts[k].z - g.pts[k-1].z;
		s[k] = s[k-1] + std::sqrt( dx*dx + dy*dy + dz*dz );
	}
	const double L = s.back();
	const double target = t * L;

	// First breakpoint STRICTLY past `target`; the bracketing span is the
	// one just before it.  Clamp at both ends so `seg`/`seg+1` are always
	// a valid pair of indices, including for `target` landing exactly on
	// a breakpoint (upper_bound skips past equal elements) or at the two
	// extremes (t = 0 or t = 1).
	const std::vector<double>::const_iterator hi = std::upper_bound( s.begin(), s.end(), target );
	std::size_t seg = ( hi == s.begin() ) ? 0 : (std::size_t)( ( hi - s.begin() ) - 1 );
	if( seg + 1 >= s.size() ) { seg = ( s.size() >= 2 ) ? s.size() - 2 : 0; }

	const double span = s[seg+1] - s[seg];
	double f = ( span > 0 ) ? ( ( target - s[seg] ) / span ) : 0.0;
	if( f < 0 ) f = 0;
	if( f > 1 ) f = 1;
	const Point3& p0 = g.pts[seg];
	const Point3& p1 = g.pts[seg+1];
	return Vector3( ( p0.x + ( p1.x - p0.x ) * f - g.pts[0].x ) / L,
	                ( p0.y + ( p1.y - p0.y ) * f - g.pts[0].y ) / L,
	                ( p0.z + ( p1.z - p0.z ) * f - g.pts[0].z ) / L );
}

//! Normalised inverse-distance weights over ALL guides (the fixtures
//! below never use more than three, so no K clamp is needed here).
void ExpectedWeights( const Point3& root, const std::vector<Guide>& gs, std::vector<double>& w )
{
	w.assign( gs.size(), 0.0 );
	double sum = 0;
	for( std::size_t i = 0; i < gs.size(); ++i ) {
		const double dx = root.x - gs[i].pts[0].x;
		const double dy = root.y - gs[i].pts[0].y;
		const double dz = root.z - gs[i].pts[0].z;
		const double d = std::sqrt( dx*dx + dy*dy + dz*dz );
		w[i] = 1.0 / d;
		sum += w[i];
	}
	for( std::size_t i = 0; i < gs.size(); ++i ) { w[i] /= sum; }
}

//! Nearest-`k` selection + normalised inverse-distance weights over a
//! guide set LARGER than k, for Group 7's eviction coverage.  Mirrors
//! `SelectGuides`'s (HairGenerator.cpp) insertion-into-a-k-slot-array
//! tie-break rule exactly -- ties keep the LOWER guide index, because
//! guides are scanned in ascending index order and a candidate only
//! displaces an existing slot on a STRICT `<` -- so this reproduces the
//! same selection the generator makes, not merely an equivalent one.
//! Returns the number of guides actually selected and fills `idxOut` /
//! `wgtOut` for exactly that many, `idxOut` sorted by ascending distance
//! (as the generator's array is), not by guide index.
unsigned int SelectNearestKWeights( const Point3& root, const std::vector<Guide>& gs, const std::size_t k,
                                     std::vector<unsigned int>& idxOut, std::vector<double>& wgtOut )
{
	std::vector<double>       best;
	std::vector<unsigned int> idx;
	for( std::size_t g = 0; g < gs.size(); ++g ) {
		const double dx = root.x - gs[g].pts[0].x;
		const double dy = root.y - gs[g].pts[0].y;
		const double dz = root.z - gs[g].pts[0].z;
		const double d = std::sqrt( dx*dx + dy*dy + dz*dz );
		if( best.size() < k ) {
			best.push_back( d );
			idx.push_back( (unsigned int)g );
			std::size_t j = best.size() - 1;
			while( j > 0 && best[j] < best[j-1] ) {
				std::swap( best[j], best[j-1] ); std::swap( idx[j], idx[j-1] ); --j;
			}
		} else if( d < best[k-1] ) {
			best[k-1] = d; idx[k-1] = (unsigned int)g;
			std::size_t j = k - 1;
			while( j > 0 && best[j] < best[j-1] ) {
				std::swap( best[j], best[j-1] ); std::swap( idx[j], idx[j-1] ); --j;
			}
		}
	}

	idxOut = idx;
	wgtOut.assign( idx.size(), 0.0 );
	if( idx.empty() ) { return 0; }
	if( best[0] <= 1e-12 ) {
		wgtOut[0] = 1.0;
		return (unsigned int)idx.size();
	}
	double sum = 0;
	for( std::size_t i = 0; i < idx.size(); ++i ) { wgtOut[i] = 1.0 / best[i]; sum += wgtOut[i]; }
	for( std::size_t i = 0; i < idx.size(); ++i ) { wgtOut[i] /= sum; }
	return (unsigned int)idx.size();
}

//////////////////////////////////////////////////////////////////////
// Scene plumbing (HairGeneratorTest's pattern, unchanged)
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_hairguides_" + tag + "_" + pid + ".RISEscene";
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

bool ParseBodyCapturing( const std::string& tag, const std::string& body, IJobPriv& job,
                          std::string& capturedOutput )
{
	const char* tmpEnv = getenv( "TMPDIR" );
	std::string dir = tmpEnv ? tmpEnv : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pidbuf[32];
	std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
	const std::string capPath = dir + "rise_hairguides_stdout_" + tag + "_" + pidbuf + ".txt";

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capPath.c_str(), "w" );
	if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

	const bool ok = ParseBodyInto( tag, body, job );

	std::fflush( stdout );
	if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
	if( capFile ) std::fclose( capFile );

	std::ifstream ifs( capPath.c_str() );
	if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); capturedOutput = oss.str(); }
	remove( capPath.c_str() );

	return ok;
}

} // namespace

// ============================================================
//  1. One guide, exactly reproduced
// ============================================================

static void RunSingleGuide()
{
	std::cout << "=== 1. One guide, exactly reproduced ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	// Straight up the normal for one unit, then a 0.2-unit flick along
	// +X.  Total arc 1.2, so the flick is the last sixth of the strand:
	// the resampling has to land control points on BOTH legs, which is
	// what makes this a shape test and not a direction test.
	std::vector<Guide> gs( 1 );
	gs[0].pts.push_back( Point3( 0.5, 0.5, 0.0 ) );
	gs[0].pts.push_back( Point3( 0.5, 0.5, 1.0 ) );
	gs[0].pts.push_back( Point3( 0.7, 0.5, 1.0 ) );
	FlatGuides flat( gs );

	HairGroomRecipe rg = PlainRecipe( base.get(), 200 );
	flat.Bind( rg );
	Owned<HairGeometry> guided( BuildAndRealize( rg ) );

	HairGroomRecipe ru = PlainRecipe( base.get(), 200 );
	Owned<HairGeometry> plain( BuildAndRealize( ru ) );

	if( !guided || !plain ) { Check( false, "guided and unguided grooms both built" ); return; }
	Check( guided->numStrands() > 0, "the guided recipe grows strands" );
	Check( guided->numStrands() == plain->numStrands(),
	       "MONEY: guides do not change the strand COUNT (placement is still count + density)" );

	// -- roots are untouched.
	bool rootsMatch = ( guided->numStrands() == plain->numStrands() );
	for( unsigned int s = 0; rootsMatch && s < guided->numStrands(); ++s ) {
		const Point3 a = guided->ControlPoint( s, 0 );
		const Point3 b = plain->ControlPoint( s, 0 );
		rootsMatch = ( a.x == b.x && a.y == b.y && a.z == b.z );
	}
	Check( rootsMatch,
	       "MONEY: every guided strand's ROOT is bit-identical to the unguided groom's (guides are shape, not placement)" );

	// -- every control point equals the closed form.
	const unsigned int nCP = guided->numStrands() ? guided->numControlPointsOfStrand( 0 ) : 0;
	Check( nCP == 5, "the groom's `segments` (5), not the guide's point count (3), sets the control-point count" );

	unsigned int bad = 0;
	double worst = 0;
	for( unsigned int s = 0; s < guided->numStrands(); ++s ) {
		const Point3 root = guided->ControlPoint( s, 0 );
		for( unsigned int k = 0; k < nCP; ++k ) {
			const double t = (double)k / (double)( nCP - 1 );
			const Vector3 e = GuideLocalWorld( gs[0], t );
			// On this fixture the surface frame IS the world axes, at the
			// guide's root and at every strand's, so the transport is a
			// pure translate and the expected point is root + length * e.
			const Point3 want( root.x + 0.5 * e.x, root.y + 0.5 * e.y, root.z + 0.5 * e.z );
			const Point3 got = guided->ControlPoint( s, k );
			const double err = std::fabs( got.x - want.x ) + std::fabs( got.y - want.y ) + std::fabs( got.z - want.z );
			if( err > worst ) worst = err;
			if( !Near( got, want ) ) { ++bad; }
		}
	}
	std::cout << "  worst closed-form error    : " << worst << std::endl;
	Check( bad == 0,
	       "MONEY: every control point of every strand equals the guide, arc-length resampled, transported and length-scaled" );

	// -- a guide that is straight along the root normal is exactly the
	//    unguided groom.  This is the invariant that says the guided path
	//    REPLACES the growth term rather than adding to it.
	std::vector<Guide> up( 1 );
	up[0].pts.push_back( Point3( 0.5, 0.5, 0.0 ) );
	up[0].pts.push_back( Point3( 0.5, 0.5, 3.7 ) );		// arbitrary length: it is normalised away
	FlatGuides flatUp( up );
	HairGroomRecipe rUp = PlainRecipe( base.get(), 200 );
	flatUp.Bind( rUp );
	Owned<HairGeometry> straight( BuildAndRealize( rUp ) );
	if( straight ) {
		unsigned int off = 0;
		for( unsigned int s = 0; s < straight->numStrands(); ++s ) {
			for( unsigned int k = 0; k < nCP; ++k ) {
				if( !Near( straight->ControlPoint( s, k ), plain->ControlPoint( s, k ) ) ) { ++off; }
			}
		}
		Check( off == 0,
		       "MONEY: a guide straight along its root normal reproduces the UNGUIDED groom (and its own length is normalised away)" );
	} else {
		Check( false, "the straight-guide groom built" );
	}
}

// ============================================================
//  2. Two guides, blended
// ============================================================

static void RunTwoGuides()
{
	std::cout << "=== 2. Two guides, blended ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	// Guide A leans +X, guide B leans -Y, and their roots sit at opposite
	// ends of the quad, so a strand's weights are a strong function of
	// where it landed.
	std::vector<Guide> gs( 2 );
	gs[0].pts.push_back( Point3( 0.05, 0.5, 0.0 ) );
	gs[0].pts.push_back( Point3( 0.05, 0.5, 1.0 ) );
	gs[0].pts.push_back( Point3( 0.55, 0.5, 1.0 ) );
	gs[1].pts.push_back( Point3( 0.95, 0.5, 0.0 ) );
	gs[1].pts.push_back( Point3( 0.95, 0.5, 1.0 ) );
	gs[1].pts.push_back( Point3( 0.95, 0.1, 1.0 ) );
	FlatGuides flat( gs );

	HairGroomRecipe r = PlainRecipe( base.get(), 300 );
	flat.Bind( r );
	Owned<HairGeometry> g( BuildAndRealize( r ) );
	if( !g || g->numStrands() == 0 ) { Check( false, "the two-guide groom grew strands" ); return; }

	const unsigned int nCP = g->numControlPointsOfStrand( 0 );

	unsigned int bad = 0;
	double worst = 0;
	unsigned int nearA = 0, nearB = 0;
	for( unsigned int s = 0; s < g->numStrands(); ++s ) {
		const Point3 root = g->ControlPoint( s, 0 );
		std::vector<double> w;
		ExpectedWeights( root, gs, w );
		if( w[0] > 0.9 ) ++nearA;
		if( w[1] > 0.9 ) ++nearB;
		for( unsigned int k = 0; k < nCP; ++k ) {
			const double t = (double)k / (double)( nCP - 1 );
			const Vector3 eA = GuideLocalWorld( gs[0], t );
			const Vector3 eB = GuideLocalWorld( gs[1], t );
			const Vector3 e( w[0] * eA.x + w[1] * eB.x,
			                 w[0] * eA.y + w[1] * eB.y,
			                 w[0] * eA.z + w[1] * eB.z );
			const Point3 want( root.x + 0.5 * e.x, root.y + 0.5 * e.y, root.z + 0.5 * e.z );
			const Point3 got = g->ControlPoint( s, k );
			const double err = std::fabs( got.x - want.x ) + std::fabs( got.y - want.y ) + std::fabs( got.z - want.z );
			if( err > worst ) worst = err;
			if( !Near( got, want ) ) { ++bad; }
		}
	}
	std::cout << "  strands dominated by A / B : " << nearA << " / " << nearB << std::endl;
	std::cout << "  worst blend error          : " << worst << std::endl;

	Check( bad == 0,
	       "MONEY: every control point equals the NORMALISED INVERSE-DISTANCE blend of the two guides, computed independently" );
	Check( nearA > 0 && nearB > 0,
	       "both ends of the base carry strands dominated (weight > 0.9) by their own nearest guide" );

	// RECOVERED-WEIGHT CHECK (P2-A review round 1, item 6 -- replaces a
	// vacuous self-referential monotonicity assertion).  The earlier
	// version sorted `ExpectedWeights`'s OWN 1/d formula by root.x and
	// asserted it falls monotonically: that is a theorem about the
	// test's reference function, true by construction no matter what the
	// generator actually did, so it could never fail.
	//
	// This instead SOLVES FOR the blend weight the generator actually
	// used, purely from the generated control points and the two guides'
	// own local shapes -- no `ExpectedWeights` formula assumed going in.
	// Two guides means the model
	//     (tip - root) / length = w0 * eA(1) + w1 * eB(1)
	// is 3 equations (x, y, z) in 2 unknowns (w0, w1): an overdetermined
	// linear system, solved here via its normal equations.  Evaluated at
	// t=1 (the tip) deliberately: both guides rise identically for the
	// first 2/3 of their arc length before leaning apart, so any t on
	// that SHARED leg makes eA(t) and eB(t) parallel and the 2x2 system
	// singular by construction, not by bad luck.
	{
		const Vector3 eA = GuideLocalWorld( gs[0], 1.0 );
		const Vector3 eB = GuideLocalWorld( gs[1], 1.0 );
		const double a11 = eA.x*eA.x + eA.y*eA.y + eA.z*eA.z;
		const double a12 = eA.x*eB.x + eA.y*eB.y + eA.z*eB.z;
		const double a22 = eB.x*eB.x + eB.y*eB.y + eB.z*eB.z;
		const double det = a11*a22 - a12*a12;

		unsigned int nSolved = 0, nBad = 0;
		double worstWeightErr = 0, worstSumErr = 0;
		for( unsigned int s = 0; det > 1e-9 && s < g->numStrands(); ++s ) {
			const Point3 root = g->ControlPoint( s, 0 );
			const Point3 tip  = g->ControlPoint( s, nCP - 1 );
			const Vector3 d( ( tip.x - root.x ) / 0.5, ( tip.y - root.y ) / 0.5, ( tip.z - root.z ) / 0.5 );
			const double b1 = eA.x*d.x + eA.y*d.y + eA.z*d.z;
			const double b2 = eB.x*d.x + eB.y*d.y + eB.z*d.z;
			const double w0 = ( b1*a22 - b2*a12 ) / det;
			const double w1 = ( a11*b2 - a12*b1 ) / det;
			++nSolved;

			std::vector<double> want;
			ExpectedWeights( root, gs, want );
			const double weightErr = std::fabs( w0 - want[0] );
			const double sumErr    = std::fabs( ( w0 + w1 ) - 1.0 );
			if( weightErr > worstWeightErr ) worstWeightErr = weightErr;
			if( sumErr > worstSumErr ) worstSumErr = sumErr;
			if( weightErr > 1e-3 || sumErr > 1e-3 ) { ++nBad; }
		}
		std::cout << "  weight recovered for       : " << nSolved << " / " << g->numStrands() << " strands" << std::endl;
		std::cout << "  worst recovered-weight err : " << worstWeightErr << std::endl;
		std::cout << "  worst recovered w0+w1-1 err: " << worstSumErr << std::endl;
		Check( det > 1e-9, "the two guides' tip shapes are non-parallel, so the recovery system is non-singular" );
		Check( nSolved > 0, "the recovery system was actually solved for at least some strands" );
		Check( nBad == 0,
		       "MONEY: the blend weight RECOVERED FROM THE GENERATED CONTROL POINTS (a 2-unknown linear solve "
		       "against the guides' own shapes, not the test's 1/d formula) matches the normalised inverse-distance "
		       "weight, and the recovered w0+w1 comes back to 1" );
	}
}

// ============================================================
//  3. Stream isolation
// ============================================================

static void RunStreamIsolation()
{
	std::cout << "=== 3. Stream isolation (frizz) ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	std::vector<Guide> gs( 1 );
	gs[0].pts.push_back( Point3( 0.5, 0.5, 0.0 ) );
	gs[0].pts.push_back( Point3( 0.5, 0.5, 1.0 ) );
	gs[0].pts.push_back( Point3( 0.8, 0.2, 1.0 ) );
	FlatGuides flat( gs );

	const double frizz = 0.3;

	HairGroomRecipe u0 = PlainRecipe( base.get(), 150 );
	HairGroomRecipe u1 = PlainRecipe( base.get(), 150 ); u1.p.frizz = frizz;
	HairGroomRecipe g0 = PlainRecipe( base.get(), 150 ); flat.Bind( g0 );
	HairGroomRecipe g1 = PlainRecipe( base.get(), 150 ); flat.Bind( g1 ); g1.p.frizz = frizz;

	Owned<HairGeometry> U0( BuildAndRealize( u0 ) );
	Owned<HairGeometry> U1( BuildAndRealize( u1 ) );
	Owned<HairGeometry> G0( BuildAndRealize( g0 ) );
	Owned<HairGeometry> G1( BuildAndRealize( g1 ) );
	if( !U0 || !U1 || !G0 || !G1 ) { Check( false, "the four frizz-probe grooms built" ); return; }

	const bool sameCount = ( U0->numStrands() == U1->numStrands() )
	                    && ( U0->numStrands() == G0->numStrands() )
	                    && ( U0->numStrands() == G1->numStrands() );
	Check( sameCount, "all four frizz-probe grooms have the same strand count" );
	if( !sameCount ) { return; }

	unsigned int mismatched = 0, moved = 0;
	double worst = 0;
	for( unsigned int s = 0; s < U0->numStrands(); ++s ) {
		for( unsigned int k = 0; k < U0->numControlPointsOfStrand( s ); ++k ) {
			const Point3 a0 = U0->ControlPoint( s, k ), a1 = U1->ControlPoint( s, k );
			const Point3 b0 = G0->ControlPoint( s, k ), b1 = G1->ControlPoint( s, k );
			const double du[3] = { a1.x - a0.x, a1.y - a0.y, a1.z - a0.z };
			const double dg[3] = { b1.x - b0.x, b1.y - b0.y, b1.z - b0.z };
			double err = 0;
			for( int c = 0; c < 3; ++c ) { err += std::fabs( du[c] - dg[c] ); }
			if( err > worst ) worst = err;
			if( err > 1e-6 ) { ++mismatched; }
			if( std::fabs( du[0] ) + std::fabs( du[1] ) + std::fabs( du[2] ) > 1e-9 ) { ++moved; }
		}
	}
	std::cout << "  worst jitter-delta error   : " << worst << std::endl;
	Check( moved > 0, "frizz actually displaced control points (the probe is not vacuous)" );
	Check( mismatched == 0,
	       "MONEY: the frizz displacement is IDENTICAL guided and unguided at the same seed -- interpolation spends no random draw" );
}

// ============================================================
//  4. The chunk
// ============================================================

static const char* kBase =
	"box_geometry\n{\n\tname\tcc_base\n\twidth\t1\n\theight\t1\n\tdepth\t1\n}\n";

static void RunChunk()
{
	std::cout << "=== 4. The hair_guides chunk ===" << std::endl;

	const std::string kGuides =
		"hair_guides\n{\n\tname\tcc_guides\n"
		"\tguide\t0 0.5 0   0 0.7 0   0.15 0.75 0\n"
		"\tguide\t0.4 0.5 0   0.4 0.7 0   0.4 0.75 0.15\n"
		"}\n";

	// -- a `hair_guides` chunk parses, and a `hair_geometry` can bind it.
	{
		Owned<IJobPriv> job( []{ IJobPriv* j = 0; RISE_CreateJobPriv( &j ); return j; }() );
		if( !job ) { Check( false, "job created" ); return; }
		const std::string body = std::string( kBase ) + kGuides +
			"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t200\n\tlength\t0.1\n\tguides\tcc_guides\n}\n";
		Check( ParseBodyInto( "ok", body, *job ), "a hair_guides chunk parses and a hair_geometry binds it" );

		// -- THE CATEGORY SPLIT, live.  `hair_guides` carries its OWN
		//    ChunkCategory::HairGuides (not ::Geometry), backed by its own
		//    IJob enumeration hook over the Job-side guide table -- the same
		//    arrangement `medium` has.  Two halves, both asserted here
		//    because each is load-bearing on its own:
		//      (a) a guide set IS enumerable under HairGuides, so a
		//          `hair_geometry.guides` reference row can offer real
		//          candidates and the editor's dangling-reference guard can
		//          recognise a live guide set;
		//      (b) a guide set is NOT enumerable under Geometry -- it never
		//          enters the IGeometryManager -- which is exactly why
		//          sharing ChunkCategory::Geometry with real, renderable
		//          geometry was wrong, and why a guide name is no longer a
		//          legal candidate for `standard_object.geometry`
		//          (ConnectionLegalityTest 3i owns that verdict).
		{
			struct Collect : public IEnumCallback<const char*> {
				std::vector<std::string> names;
				bool operator()( const char* const& n ) override {
					if( n && n[0] ) names.push_back( std::string( n ) );
					return true;
				}
			} direct;
			job->EnumerateHairGuideNames( direct );
			Check( direct.names.size() == 1 && direct.names[0] == "cc_guides",
			       "IJob::EnumerateHairGuideNames yields the declared guide set, and only it" );

			const std::vector<String> asGuides =
				CstIntrospection::CandidateNamesForChunkCategory( *job, ChunkCategory::HairGuides );
			bool foundAsGuides = false;
			for( const String& n : asGuides ) if( std::string( n.c_str() ) == "cc_guides" ) foundAsGuides = true;
			Check( foundAsGuides,
			       "MONEY: CandidateNamesForChunkCategory(HairGuides) enumerates the guide set (the new "
			       "CstIntrospection arm, mirroring the Medium one)" );

			const std::vector<String> asGeometry =
				CstIntrospection::CandidateNamesForChunkCategory( *job, ChunkCategory::Geometry );
			bool foundAsGeometry = false, foundRealGeometry = false;
			for( const String& n : asGeometry ) {
				const std::string s( n.c_str() );
				if( s == "cc_guides" ) foundAsGeometry = true;
				if( s == "g" )         foundRealGeometry = true;   // the hair_geometry itself IS geometry
			}
			Check( !foundAsGeometry,
			       "MONEY: the guide set is NOT enumerated as Geometry -- it is data in the Job's guide "
			       "table, never an entry in the IGeometryManager" );
			Check( foundRealGeometry,
			       "control: the hair_geometry that READS the guides is still ordinary Geometry" );
		}
	}

	// -- BEHAVIOURAL: the same scene with and without the `guides` line
	//    produces measurably different strands.  Parsing alone would pass
	//    even if Finalize dropped the field on the floor.
	{
		auto Grow = []( const char* tag, const std::string& body, std::vector<Point3>& out ) -> bool {
			IJobPriv* job = 0;
			if( !RISE_CreateJobPriv( &job ) || !job ) { return false; }
			bool good = false;
			if( ParseBodyInto( tag, body, *job ) ) {
				IGeometry* geom = job->GetGeometries() ? job->GetGeometries()->GetItem( "g" ) : 0;
				HairGeometry* hg = geom ? dynamic_cast<HairGeometry*>( geom ) : 0;
				if( hg ) {
					hg->Realize();
					for( unsigned s = 0; s < hg->numStrands(); ++s ) {
						for( unsigned k = 0; k < hg->numControlPointsOfStrand( s ); ++k ) {
							out.push_back( hg->ControlPoint( s, k ) );
						}
					}
					good = !out.empty();
				}
			}
			safe_release( job );
			return good;
		};

		const std::string common =
			"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t150\n\tlength\t0.1\n\tsegments\t6\n\tseed\t3\n";
		std::vector<Point3> without, with;
		const bool a = Grow( "noguides", std::string( kBase ) + kGuides + common + "}\n", without );
		const bool b = Grow( "guides",   std::string( kBase ) + kGuides + common + "\tguides\tcc_guides\n}\n", with );
		Check( a && b, "both probe grooms built through the chunk" );
		if( a && b ) {
			Check( without.size() == with.size(), "binding `guides` does not change the strand count" );
			bool rootsSame = true, anyDifferent = false;
			for( std::size_t i = 0; i < without.size() && i < with.size(); ++i ) {
				const bool same = ( without[i].x == with[i].x && without[i].y == with[i].y && without[i].z == with[i].z );
				if( !same ) { anyDifferent = true; }
				if( i % 6 == 0 && !same ) { rootsSame = false; }		// every 6th point is a root
			}
			Check( anyDifferent, "MONEY: `guides` reaches the generator through the chunk (the strands changed shape)" );
			Check( rootsSame, "and it changed ONLY the shape -- every root is still where the unguided groom put it" );
		}
	}

	// -- negatives, each naming its own fault.
	struct Neg { const char* tag; std::string body; const char* needle; const char* what; };
	const std::vector<Neg> negs = {
		{ "unknown", std::string( kBase ) +
			"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0.1\n\tguides\tnope\n}\n",
			"not found", "an unknown `guides` name is refused" },
		{ "onepoint", std::string( kBase ) +
			"hair_guides\n{\n\tname\tgs\n\tguide\t0 0 0\n}\n",
			"at least 2 points", "a one-point guide is refused" },
		{ "ragged", std::string( kBase ) +
			"hair_guides\n{\n\tname\tgs\n\tguide\t0 0 0  1 1\n}\n",
			"per point", "a guide line whose token count is not a multiple of three is refused" },
		{ "zerolen", std::string( kBase ) +
			"hair_guides\n{\n\tname\tgs\n\tguide\t0.2 0.2 0.2   0.2 0.2 0.2\n}\n",
			"zero total length", "a guide whose points all coincide is refused" },
		{ "noguide", std::string( kBase ) +
			"hair_guides\n{\n\tname\tgs\n}\n",
			"at least one", "a hair_guides chunk with no `guide` line is refused" },
		{ "dupname", std::string( kBase ) +
			"hair_guides\n{\n\tname\tgs\n\tguide\t0 0 0  0 1 0\n}\n"
			"hair_guides\n{\n\tname\tgs\n\tguide\t0 0 0  0 2 0\n}\n",
			"duplicate", "a duplicate guide-set name is refused" },
	};

	for( std::size_t i = 0; i < negs.size(); ++i ) {
		IJobPriv* job = 0;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); continue; }
		std::string out;
		const bool ok = ParseBodyCapturing( negs[i].tag, negs[i].body, *job, out );
		Check( !ok, negs[i].what );
		Check( out.find( negs[i].needle ) != std::string::npos,
		       std::string( "...and the diagnostic says why (`" ) + negs[i].needle + "`)" );
		safe_release( job );
	}
}

// ============================================================
//  5. Determinism
// ============================================================

static void RunDeterminism()
{
	std::cout << "=== 5. Determinism ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	std::vector<Guide> gs( 2 );
	gs[0].pts.push_back( Point3( 0.2, 0.2, 0.0 ) );
	gs[0].pts.push_back( Point3( 0.2, 0.2, 1.0 ) );
	gs[0].pts.push_back( Point3( 0.6, 0.3, 1.1 ) );
	gs[1].pts.push_back( Point3( 0.8, 0.8, 0.0 ) );
	gs[1].pts.push_back( Point3( 0.8, 0.8, 1.0 ) );
	gs[1].pts.push_back( Point3( 0.5, 0.9, 1.2 ) );
	FlatGuides flat( gs );

	HairGroomRecipe r = PlainRecipe( base.get(), 250 );
	r.p.frizz = 0.2; r.p.gravity = 0.1; r.p.clump = 0.4; r.p.clumpSize = 0.25;
	flat.Bind( r );

	Owned<HairGeometry> a( BuildAndRealize( r ) );
	Owned<HairGeometry> b( BuildAndRealize( r ) );
	if( !a || !b ) { Check( false, "two guided grooms built from the same recipe" ); return; }
	Check( a->numStrands() > 0, "the guided styled recipe grows strands" );
	Check( GroomsIdentical( *a, *b ),
	       "MONEY: the same guided recipe + seed produces BIT-IDENTICAL strands across two builds" );

	HairGroomRecipe u = PlainRecipe( base.get(), 250 );
	u.p.frizz = 0.2; u.p.gravity = 0.1; u.p.clump = 0.4; u.p.clumpSize = 0.25;
	Owned<HairGeometry> c( BuildAndRealize( u ) );
	Owned<HairGeometry> d( BuildAndRealize( u ) );
	if( c && d ) {
		Check( GroomsIdentical( *c, *d ),
		       "an UNGUIDED groom is likewise bit-identical build to build with the guide code path present" );
	} else {
		Check( false, "two unguided grooms built" );
	}
}

// ============================================================
//  6. Rigid transport across two facings
// ============================================================

static void RunTransport()
{
	std::cout << "=== 6. Rigid transport ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeTwoFacings() );
	if( !base ) { Check( false, "two-facing base built" ); return; }

	// The guide is rooted on face A (z = 0), rises along that face's
	// normal (+Z) and then leans along that face's TANGENT (+X).
	std::vector<Guide> gs( 1 );
	gs[0].pts.push_back( Point3( 0.5, 0.5, 0.0 ) );
	gs[0].pts.push_back( Point3( 0.5, 0.5, 1.0 ) );
	gs[0].pts.push_back( Point3( 1.0, 0.5, 1.0 ) );		// lean = +X = face A's tangent
	FlatGuides flat( gs );

	HairGroomRecipe r = PlainRecipe( base.get(), 400 );
	flat.Bind( r );
	Owned<HairGeometry> g( BuildAndRealize( r ) );
	if( !g || g->numStrands() == 0 ) { Check( false, "the transport groom grew strands" ); return; }

	const unsigned int nCP = g->numControlPointsOfStrand( 0 );
	unsigned int onA = 0, onB = 0, badA = 0, badB = 0;

	for( unsigned int s = 0; s < g->numStrands(); ++s ) {
		const Point3 root = g->ControlPoint( s, 0 );
		const Point3 tip  = g->ControlPoint( s, nCP - 1 );
		const Vector3 d( tip.x - root.x, tip.y - root.y, tip.z - root.z );

		// The guide's tip offset, in its own frame, is
		//   e = (lean, 0, rise) / L,  lean = 0.5, rise = 1.0, L = 1.5
		// so a strand's tip should sit at length * (T*ex + N*ez) from its
		// root, with {T, N} that strand's own face frame.
		const double L = 1.5, ex = 0.5 / L, ez = 1.0 / L, len = 0.5;

		if( root.x < 5.0 ) {
			++onA;		// face A: T = +X, N = +Z
			const Vector3 want( len * ex, 0, len * ez );
			if( std::fabs( d.x - want.x ) > kPosTol || std::fabs( d.y - want.y ) > kPosTol
			 || std::fabs( d.z - want.z ) > kPosTol ) { ++badA; }
		} else {
			++onB;		// face B: T = +Y, N = +X  -- the lean must come out along +Y
			const Vector3 want( len * ez, len * ex, 0 );
			if( std::fabs( d.x - want.x ) > kPosTol || std::fabs( d.y - want.y ) > kPosTol
			 || std::fabs( d.z - want.z ) > kPosTol ) { ++badB; }
		}
	}

	std::cout << "  strands on face A / face B : " << onA << " / " << onB << std::endl;
	Check( onA > 0 && onB > 0, "the two-facing base grew strands on BOTH faces" );
	Check( badA == 0, "on the guide's OWN face the transport is the identity (tip offset matches the guide exactly)" );
	Check( badB == 0,
	       "MONEY: on the differently-oriented face the guide's lean comes out along THAT face's tangent (+Y), not along world +X" );

	// -- THE MIRROR CASE, and it is not redundant.  The check above only
	//    pins the STRAND side of the transport: face A's frame happens to
	//    be the world axes, so a guide rooted there would look identical
	//    whether the code read it in its surface frame or not.  Rooting
	//    the guide on FACE B instead makes the guide-side frame
	//    non-trivial: a lean authored along face B's tangent (+Y) has to
	//    reappear along face A's tangent (+X) for the strands over there.
	{
		std::vector<Guide> gb( 1 );
		gb[0].pts.push_back( Point3( 10.0, 0.5, 0.5 ) );
		gb[0].pts.push_back( Point3( 11.0, 0.5, 0.5 ) );		// rise along face B's normal (+X)
		gb[0].pts.push_back( Point3( 11.0, 1.0, 0.5 ) );		// lean along face B's tangent (+Y)
		FlatGuides flatB( gb );

		HairGroomRecipe rb = PlainRecipe( base.get(), 400 );
		flatB.Bind( rb );
		Owned<HairGeometry> gg( BuildAndRealize( rb ) );
		if( !gg || gg->numStrands() == 0 ) { Check( false, "the mirror-transport groom grew strands" ); return; }

		const unsigned int m = gg->numControlPointsOfStrand( 0 );
		unsigned int badMirrorA = 0, badMirrorB = 0, seenA = 0;
		const double L = 1.5, ex = 0.5 / L, ez = 1.0 / L, len = 0.5;
		for( unsigned int s = 0; s < gg->numStrands(); ++s ) {
			const Point3 root = gg->ControlPoint( s, 0 );
			const Point3 tip  = gg->ControlPoint( s, m - 1 );
			const Vector3 d( tip.x - root.x, tip.y - root.y, tip.z - root.z );
			if( root.x < 5.0 ) {
				++seenA;		// face A: T = +X, N = +Z
				const Vector3 want( len * ex, 0, len * ez );
				if( std::fabs( d.x - want.x ) > kPosTol || std::fabs( d.y - want.y ) > kPosTol
				 || std::fabs( d.z - want.z ) > kPosTol ) { ++badMirrorA; }
			} else {
				const Vector3 want( len * ez, len * ex, 0 );
				if( std::fabs( d.x - want.x ) > kPosTol || std::fabs( d.y - want.y ) > kPosTol
				 || std::fabs( d.z - want.z ) > kPosTol ) { ++badMirrorB; }
			}
		}
		Check( seenA > 0, "the mirror-transport groom grew strands on the far face" );
		Check( badMirrorB == 0, "a guide rooted on face B reproduces itself exactly on face B" );
		Check( badMirrorA == 0,
		       "MONEY: the GUIDE side of the transport is framed by the base too -- a lean authored along face B's tangent "
		       "arrives along face A's tangent, not along world +Y" );
	}
}

// ============================================================
//  7. Coverage: the K-clamp and the coincident branch
// ============================================================

static void RunCoverage()
{
	std::cout << "=== 7. Coverage: K-clamp eviction + coincident root ===" << std::endl;

	// ---- 7a. Five guides on the unit quad -- MORE than `kNearestGuides`
	//      (3) -- so every strand's own nearest-three selection is
	//      actually a SELECTION, not the whole set.  Each strand's shape
	//      is checked against the blend of its OWN nearest three (found
	//      independently by `SelectNearestKWeights`); the other two must
	//      contribute EXACTLY zero.
	{
		Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
		if( !base ) { Check( false, "unit-quad base built (7a)" ); return; }

		std::vector<Guide> gs( 5 );
		// Five roots spread along y=0.5 at x = 0.1, 0.3, 0.5, 0.7, 0.9,
		// each leaning a DIFFERENT amount in +X after the shared 1-unit
		// rise, so a wrong nearest-three subset produces a visibly
		// different blend rather than one that happens to agree anyway.
		for( unsigned int i = 0; i < 5; ++i ) {
			const double x = 0.1 + 0.2 * i;
			gs[i].pts.push_back( Point3( x, 0.5, 0.0 ) );
			gs[i].pts.push_back( Point3( x, 0.5, 1.0 ) );
			gs[i].pts.push_back( Point3( x + 0.05 * ( i + 1 ), 0.5, 1.0 ) );
		}
		FlatGuides flat( gs );

		HairGroomRecipe r = PlainRecipe( base.get(), 400 );
		flat.Bind( r );
		Owned<HairGeometry> g( BuildAndRealize( r ) );
		if( !g || g->numStrands() == 0 ) { Check( false, "the five-guide groom grew strands" ); return; }

		const unsigned int nCP = g->numControlPointsOfStrand( 0 );
		unsigned int bad = 0, checkedStrands = 0;
		double worst = 0;
		std::set<unsigned int> triplesSeen;	// packed idx[0..2] (each 0-4) -- proves >1 subset occurred
		for( unsigned int s = 0; s < g->numStrands(); ++s ) {
			const Point3 root = g->ControlPoint( s, 0 );
			std::vector<unsigned int> idx;
			std::vector<double> w;
			const unsigned int n = SelectNearestKWeights( root, gs, 3, idx, w );
			if( n != 3 ) { continue; }		// five guides are bound; should never clamp below 3
			++checkedStrands;
			triplesSeen.insert( idx[0] * 100 + idx[1] * 10 + idx[2] );

			for( unsigned int k = 0; k < nCP; ++k ) {
				const double t = (double)k / (double)( nCP - 1 );
				Vector3 e( 0, 0, 0 );
				for( unsigned int i = 0; i < 3; ++i ) {
					const Vector3 ei = GuideLocalWorld( gs[ idx[i] ], t );
					e.x += w[i] * ei.x; e.y += w[i] * ei.y; e.z += w[i] * ei.z;
				}
				const Point3 want( root.x + 0.5 * e.x, root.y + 0.5 * e.y, root.z + 0.5 * e.z );
				const Point3 got = g->ControlPoint( s, k );
				const double err = std::fabs( got.x - want.x ) + std::fabs( got.y - want.y ) + std::fabs( got.z - want.z );
				if( err > worst ) worst = err;
				if( !Near( got, want ) ) { ++bad; }
			}
		}
		std::cout << "  strands checked / distinct nearest-3 subsets : " << checkedStrands << " / " << triplesSeen.size() << std::endl;
		std::cout << "  worst nearest-3-of-5 blend error             : " << worst << std::endl;
		Check( checkedStrands > 0, "at least some strands were checked against their own nearest-three subset" );
		Check( triplesSeen.size() > 1,
		       "more than one nearest-three guide subset actually occurred across the groom (the eviction arm is "
		       "exercised, not vacuously satisfied by one fixed triple)" );
		Check( bad == 0,
		       "MONEY: every strand's shape equals the blend of its OWN nearest three guides of five bound -- the "
		       "other two contribute EXACTLY zero (SelectGuides' K clamp / eviction arm)" );
	}

	// ---- 7b. The coincident branch.  A fresh single-strand probe groom
	//      supplies an ACTUAL (RNG-placed) root position; a guide is then
	//      authored with its root at exactly that position -- bit-for-
	//      bit, read back rather than computed -- alongside two decoys
	//      placed CLOSER than they would need to be to matter, so a bug
	//      that fell through to the ordinary blend instead of the
	//      `best[0] <= kGuideCoincidentEps` short-circuit would visibly
	//      pull the strand toward them.
	{
		Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
		if( !base ) { Check( false, "unit-quad base built (7b)" ); return; }

		HairGroomRecipe probe = PlainRecipe( base.get(), 1 );
		Owned<HairGeometry> solo( BuildAndRealize( probe ) );
		if( !solo || solo->numStrands() == 0 ) { Check( false, "the single-strand probe groom grew a strand" ); return; }
		const Point3 pinnedRoot = solo->ControlPoint( 0, 0 );

		std::vector<Guide> gs( 3 );
		gs[0].pts.push_back( pinnedRoot );
		gs[0].pts.push_back( Point3( pinnedRoot.x, pinnedRoot.y, pinnedRoot.z + 1.0 ) );
		gs[0].pts.push_back( Point3( pinnedRoot.x + 0.3, pinnedRoot.y, pinnedRoot.z + 1.0 ) );
		gs[1].pts.push_back( Point3( pinnedRoot.x + 0.02, pinnedRoot.y, pinnedRoot.z ) );
		gs[1].pts.push_back( Point3( pinnedRoot.x + 0.02, pinnedRoot.y, pinnedRoot.z + 1.0 ) );
		gs[1].pts.push_back( Point3( pinnedRoot.x - 0.5,  pinnedRoot.y, pinnedRoot.z + 1.0 ) );
		gs[2].pts.push_back( Point3( pinnedRoot.x - 0.02, pinnedRoot.y, pinnedRoot.z ) );
		gs[2].pts.push_back( Point3( pinnedRoot.x - 0.02, pinnedRoot.y, pinnedRoot.z + 1.0 ) );
		gs[2].pts.push_back( Point3( pinnedRoot.x,        pinnedRoot.y + 0.5, pinnedRoot.z + 1.0 ) );
		FlatGuides flat( gs );

		HairGroomRecipe r = PlainRecipe( base.get(), 1 );
		flat.Bind( r );
		Owned<HairGeometry> pinned( BuildAndRealize( r ) );
		if( !pinned || pinned->numStrands() == 0 ) { Check( false, "the pinned-root probe groom grew a strand" ); return; }

		const Point3 root = pinned->ControlPoint( 0, 0 );
		Check( root.x == pinnedRoot.x && root.y == pinnedRoot.y && root.z == pinnedRoot.z,
		       "the probe strand's root is unchanged by binding guides (same base/seed/count as the unguided probe)" );

		const unsigned int nCP = pinned->numControlPointsOfStrand( 0 );
		unsigned int bad = 0;
		double worst = 0;
		for( unsigned int k = 0; k < nCP; ++k ) {
			const double t = (double)k / (double)( nCP - 1 );
			const Vector3 e = GuideLocalWorld( gs[0], t );		// weight 1 on guide 0 alone
			const Point3 want( root.x + 0.5 * e.x, root.y + 0.5 * e.y, root.z + 0.5 * e.z );
			const Point3 got = pinned->ControlPoint( 0, k );
			const double err = std::fabs( got.x - want.x ) + std::fabs( got.y - want.y ) + std::fabs( got.z - want.z );
			if( err > worst ) worst = err;
			if( !Near( got, want ) ) { ++bad; }
		}
		std::cout << "  worst coincident-branch error : " << worst << std::endl;
		Check( bad == 0,
		       "MONEY: a strand rooted exactly on a guide's root pins to weight 1 on THAT guide alone -- the two "
		       "closer-than-necessary decoys contribute EXACTLY zero (SelectGuides' coincident short-circuit)" );
	}
}

int main()
{
	std::cout << "===================================================" << std::endl;
	std::cout << " HairGuidesTest -- authored guide strands"           << std::endl;
	std::cout << "===================================================" << std::endl;

	RunSingleGuide();
	RunTwoGuides();
	RunStreamIsolation();
	RunChunk();
	RunDeterminism();
	RunTransport();
	RunCoverage();

	std::cout << "---------------------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << ", failures: " << g_failures << std::endl;
	return g_failures == 0 ? 0 : 1;
}
