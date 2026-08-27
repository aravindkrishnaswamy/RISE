//////////////////////////////////////////////////////////////////////
//
//  HairGeneratorTest.cpp - Contract test for the painter-driven GROOM
//    GENERATOR and the `hair_geometry` scene chunk (slice D of the
//    hair/fur arc, docs/HAIR_FUR_DESIGN.md section 5.3).
//
//  WHAT THIS TEST OWNS.  The RECIPE -> STRANDS transformation and the
//  chunk that authors it.  It does NOT re-test the curve primitive's
//  intersector or its curve math -- HairGeometryTest.cpp owns those --
//  and it does not test the BCSDF (HairBSDFTest.cpp) or the material
//  chunk (HairMaterialChunkTest.cpp).
//
//  The eight groups, and the specific regression each one buys:
//
//    1. DETERMINISM.  Two grooms built from the same recipe and seed
//       are bit-identical in every control point, width and root UV;
//       changing ONLY the seed changes them.  This is the contract that
//       makes an animated groom stable frame to frame, and it is the
//       one a "just use GlobalRNG" simplification would silently break.
//
//    2. ROOT PLACEMENT.  Roots land ON the tessellated base surface,
//       and they are AREA-weighted rather than triangle-weighted --
//       proved on a two-triangle base whose areas are exactly 9:1,
//       against a binomial tolerance.  A per-triangle-uniform sampler
//       would put 50/50 here and fail by ~50 standard deviations.
//
//    3. DENSITY MASK.  A step-function density painter over the base UV
//       leaves EXACTLY zero roots on its masked half -- not "few".
//
//    4. LENGTH PAINTER + WIDTH LERP.  A 0.5 length painter halves the
//       measured arc length of every strand (checked on the UNBENT
//       configuration so arc length is exactly the authored length),
//       and the reported width at s=0 / s=1 is the authored root / tip.
//
//    5. CLUMPING.  Turning clumping on strictly reduces the mean
//       distance from a strand's tip to its clump centre's tip, at the
//       same seed.  (The test reproduces the DOCUMENTED cell rule --
//       first strand in a cell of side `clump_size` is that cell's
//       centre -- so it also pins that rule, not just "tips moved".)
//
//    6. FRIZZ / CURL.  Non-zero parameters move control points off the
//       straight-growth baseline; zero parameters reproduce the
//       baseline EXACTLY (which is the harder half: it pins that the
//       effects are skipped cleanly rather than applied at zero
//       amplitude through a path that perturbs the random stream).
//
//    7. THE REALIZE CONTRACT.  A deferred groom before Realize() is
//       defined and EMPTY -- no strands, no hits, no crash; after
//       Realize() it has strands and is hit; a second Realize() changes
//       nothing.
//
//    8. THE CHUNK.  A minimal `hair_geometry` chunk parses and
//       registers a geometry; a missing `base_geometry`, an unknown
//       base name, and each out-of-range numeric are all REJECTED with
//       a diagnostic naming the offending parameter.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
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
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Geometry/HairGenerator.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/RandomNumbers.h"
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

//! Takes ownership of the reference a `new` / `RISE_API_Create*` hands
//! back (RISE's Reference base starts at refcount 1), rather than
//! addref-ing on top of it.
template<class T>
class Owned
{
public:
	explicit Owned( T* q = 0 ) : p( q ) {}
	~Owned() { if( p ) p->release(); }
	void reset( T* q ) { if( p ) p->release(); p = q; }
	T*   release_ownership() { T* q = p; p = 0; return q; }
	T*   operator->() const { return p; }
	T&   operator*()  const { return *p; }
	T*   get()        const { return p; }
	explicit operator bool() const { return p != 0; }
private:
	T* p;
	Owned( const Owned& );
	Owned& operator=( const Owned& );
};

//////////////////////////////////////////////////////////////////////
// Test-local painters
//////////////////////////////////////////////////////////////////////

//! Density mask: 0 on the u < 0.5 half of the base UV domain, 1 on the
//! other.  A STEP, deliberately -- "exactly zero roots in the masked
//! half" is a far sharper assertion than "fewer roots".
class UVStepScalarPainter :
	public virtual IScalarPainter,
	public virtual Reference
{
protected:
	virtual ~UVStepScalarPainter() {}
public:
	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
	{
		return ScalarTriple( ri.ptCoord.x < Scalar(0.5) ? Scalar(0) : Scalar(1) );
	}
	bool HasPerChannelVariation() const override { return false; }
};

//! Constant scalar, for the length-multiplier slot.
class ConstScalarPainter :
	public virtual IScalarPainter,
	public virtual Reference
{
protected:
	const Scalar v;
	virtual ~ConstScalarPainter() {}
public:
	explicit ConstScalarPainter( Scalar value ) : v( value ) {}
	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ) const override { return ScalarTriple( v ); }
	bool HasPerChannelVariation() const override { return false; }
};

//////////////////////////////////////////////////////////////////////
// Base-geometry fixtures
//////////////////////////////////////////////////////////////////////

//! The unit square in the z = 0 plane, two triangles, normals +Z, and
//! UV == (x, y).  Making the UV equal the position is what lets the
//! density test assert on ROOT POSITIONS while the painter is really
//! reading UVs.
ITriangleMeshGeometryIndexed* MakeUnitQuad()
{
	ITriangleMeshGeometryIndexed* m = 0;
	if( !RISE_API_CreateTriangleMeshGeometryIndexed( &m, true, false ) || !m ) {
		return 0;
	}
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

//! Two DISJOINT triangles in the z = 0 plane whose areas are exactly
//! 9 : 1 (4.5 and 0.5), separated along x so a root can be classified
//! by position alone.  Normals +Z.
ITriangleMeshGeometryIndexed* MakeNineToOne()
{
	ITriangleMeshGeometryIndexed* m = 0;
	if( !RISE_API_CreateTriangleMeshGeometryIndexed( &m, true, false ) || !m ) {
		return 0;
	}
	m->BeginIndexedTriangles();
	// big: (0,0) (3,0) (0,3) -> area 4.5
	m->AddVertex( Point3(  0, 0, 0 ) );
	m->AddVertex( Point3(  3, 0, 0 ) );
	m->AddVertex( Point3(  0, 3, 0 ) );
	// small: (10,0) (11,0) (10,1) -> area 0.5
	m->AddVertex( Point3( 10, 0, 0 ) );
	m->AddVertex( Point3( 11, 0, 0 ) );
	m->AddVertex( Point3( 10, 1, 0 ) );
	for( int i = 0; i < 6; ++i ) {
		m->AddNormal( Vector3( 0, 0, 1 ) );
		m->AddTexCoord( Point2( 0.75, 0.75 ) );		// unmasked everywhere
	}
	IndexedTriangle t;
	for( int k = 0; k < 3; ++k ) { t.iVertices[k] = k; t.iNormals[k] = k; t.iCoords[k] = k; }
	m->AddIndexedTriangle( t );
	for( int k = 0; k < 3; ++k ) { t.iVertices[k] = 3+k; t.iNormals[k] = 3+k; t.iCoords[k] = 3+k; }
	m->AddIndexedTriangle( t );
	m->DoneIndexedTriangles();
	return m;
}

//////////////////////////////////////////////////////////////////////
// Groom helpers
//////////////////////////////////////////////////////////////////////

//! The plain, unstyled recipe every group starts from: straight quills
//! along the surface normal, no painters, nothing enabled.
HairGroomRecipe PlainRecipe( IGeometry* base, unsigned int count, unsigned int segments = 6 )
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

//! Builds AND realizes a groom.  Returns null if construction was
//! refused (which several negative fixtures below expect).
HairGeometry* BuildAndRealize( const HairGroomRecipe& r )
{
	IGeometry* g = 0;
	if( !RISE_API_CreateHairGeometryGroom( &g, r, "test_groom" ) || !g ) {
		return 0;
	}
	g->Realize();
	// dynamic_cast, not static_cast: Geometry is a VIRTUAL base of
	// HairGeometry, and a downcast through a virtual base is only
	// expressible dynamically.
	HairGeometry* h = dynamic_cast<HairGeometry*>( g );
	if( !h ) { g->release(); }
	return h;
}

//! True iff two grooms are IDENTICAL in every observable: strand count,
//! per-strand control-point count, every control point (compared with
//! `==`, i.e. bitwise on the widened float storage), both widths and
//! the root UV.
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
		if( a.StrandWidthAt( s, 0 ) != b.StrandWidthAt( s, 0 ) ) return false;
		if( a.StrandWidthAt( s, 1 ) != b.StrandWidthAt( s, 1 ) ) return false;
		const Point2 ua = a.StrandRootUV( s );
		const Point2 ub = b.StrandRootUV( s );
		if( !( ua.x == ub.x && ua.y == ub.y ) ) return false;
	}
	return true;
}

Point3 StrandTip( const HairGeometry& g, unsigned int s )
{
	return g.ControlPoint( s, g.numControlPointsOfStrand( s ) - 1 );
}

//////////////////////////////////////////////////////////////////////
// Scene plumbing (HairMaterialChunkTest's pattern, unchanged)
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_hairgen_" + tag + "_" + pid + ".RISEscene";
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
	const std::string capPath = dir + "rise_hairgen_stdout_" + tag + "_" + pidbuf + ".txt";

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
//  1. Determinism
// ============================================================

static void RunDeterminism()
{
	std::cout << "=== 1. Determinism ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	HairGroomRecipe r = PlainRecipe( base.get(), 400 );

	Owned<HairGeometry> a( BuildAndRealize( r ) );
	Owned<HairGeometry> b( BuildAndRealize( r ) );
	if( !a || !b ) { Check( false, "two grooms built from the same recipe" ); return; }

	std::cout << "  strands (seed 7)           : " << a->numStrands() << std::endl;

	Check( a->numStrands() > 0, "the plain recipe actually grows strands" );
	Check( GroomsIdentical( *a, *b ),
	       "MONEY: the same recipe + seed produces BIT-IDENTICAL strands across two independent builds" );

	// A second Realize() on an already-realized groom is a no-op, not a
	// regeneration -- the observable groom is unchanged.
	const unsigned int before = a->numStrands();
	const Point3 tipBefore = StrandTip( *a, 0 );
	a->Realize();
	Check( a->numStrands() == before, "a second Realize() leaves the strand count unchanged" );
	const Point3 tipAfter = StrandTip( *a, 0 );
	Check( tipBefore.x == tipAfter.x && tipBefore.y == tipAfter.y && tipBefore.z == tipAfter.z,
	       "a second Realize() leaves the strands themselves unchanged (idempotent)" );

	r.p.seed = 8;
	Owned<HairGeometry> c( BuildAndRealize( r ) );
	if( !c ) { Check( false, "groom built at a different seed" ); return; }
	Check( !GroomsIdentical( *a, *c ), "changing ONLY the seed changes the groom" );
}

// ============================================================
//  2. Root placement + area weighting
// ============================================================

static void RunRootPlacement()
{
	std::cout << "=== 2. Root placement and area weighting ===" << std::endl;

	// -- roots lie exactly ON the tessellated base (a flat z = 0 quad),
	//    and inside its footprint.
	{
		Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
		if( !base ) { Check( false, "unit-quad base built" ); return; }

		HairGroomRecipe r = PlainRecipe( base.get(), 500 );
		Owned<HairGeometry> g( BuildAndRealize( r ) );
		if( !g ) { Check( false, "quad groom built" ); return; }

		double worstOffPlane = 0;
		int outsideFootprint = 0;
		int badRootUV        = 0;
		for( unsigned int s = 0; s < g->numStrands(); ++s ) {
			const Point3 root = g->ControlPoint( s, 0 );
			worstOffPlane = std::max( worstOffPlane, fabs( (double)root.z ) );
			if( root.x < -1e-9 || root.x > 1.0 + 1e-9 ||
			    root.y < -1e-9 || root.y > 1.0 + 1e-9 ) ++outsideFootprint;
			// UV == position on this fixture, so the stored root UV must
			// agree with the root's own (x, y) -- this is what a
			// hair_material reads as ri.ptCoord1.
			const Point2 uv = g->StrandRootUV( s );
			if( fabs( (double)uv.x - (double)root.x ) > 1e-5 ||
			    fabs( (double)uv.y - (double)root.y ) > 1e-5 ) ++badRootUV;
		}
		std::cout << "  strands                    : " << g->numStrands() << std::endl;
		std::cout << "  worst |z| at a root        : " << worstOffPlane << std::endl;

		Check( g->numStrands() == 500,   "with no density mask every candidate becomes a strand" );
		Check( worstOffPlane < 1e-9,     "every root lies ON the tessellated base surface" );
		Check( outsideFootprint == 0,    "every root lies inside the base's footprint" );
		Check( badRootUV == 0,           "the stored root UV is the base surface's UV at the root" );
	}

	// -- AREA weighting, on a 9:1 pair of triangles.  A per-triangle-
	//    uniform sampler would put ~50/50 here.
	{
		Owned<ITriangleMeshGeometryIndexed> base( MakeNineToOne() );
		if( !base ) { Check( false, "9:1 base built" ); return; }

		const unsigned int N = 4000;
		HairGroomRecipe r = PlainRecipe( base.get(), N );
		Owned<HairGeometry> g( BuildAndRealize( r ) );
		if( !g ) { Check( false, "9:1 groom built" ); return; }

		unsigned int big = 0, small = 0;
		for( unsigned int s = 0; s < g->numStrands(); ++s ) {
			( g->ControlPoint( s, 0 ).x < 5.0 ? big : small )++;
		}
		// Binomial(N, 0.9): mean 3600, sd = sqrt(4000*0.9*0.1) = 19.0.
		// A 5-sigma window is ~95 either side; a 50/50 sampler would
		// land 1600 away, i.e. 84 sigma out.
		const double expected = 0.9 * (double)N;
		const double sd       = sqrt( (double)N * 0.9 * 0.1 );
		std::cout << "  big/small triangle roots   : " << big << " / " << small
		          << "  (expected " << expected << " +/- " << 5.0*sd << ")" << std::endl;

		Check( big + small == N, "every candidate landed on one of the two triangles" );
		Check( fabs( (double)big - expected ) < 5.0 * sd,
		       "MONEY: roots are AREA-weighted (9:1 triangles give a 9:1 split, within 5 sigma)" );
	}
}

// ============================================================
//  3. Density mask
// ============================================================

static void RunDensityMask()
{
	std::cout << "=== 3. Density mask ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	Owned<UVStepScalarPainter> density( new UVStepScalarPainter() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	const unsigned int N = 2000;
	HairGroomRecipe r = PlainRecipe( base.get(), N );
	r.pDensity = density.get();

	Owned<HairGeometry> g( BuildAndRealize( r ) );
	if( !g ) { Check( false, "masked groom built" ); return; }

	int inMaskedHalf = 0;
	for( unsigned int s = 0; s < g->numStrands(); ++s ) {
		if( g->ControlPoint( s, 0 ).x < 0.5 ) ++inMaskedHalf;
	}
	const double sd = sqrt( (double)N * 0.5 * 0.5 );
	std::cout << "  strands surviving          : " << g->numStrands()
	          << "  (expected ~" << N/2 << " +/- " << 5.0*sd << ")" << std::endl;
	std::cout << "  strands in the masked half : " << inMaskedHalf << std::endl;

	Check( inMaskedHalf == 0,
	       "MONEY: a step-function density painter leaves EXACTLY zero roots on its masked half" );
	Check( g->numStrands() > 0, "the unmasked half still grows hair" );
	Check( fabs( (double)g->numStrands() - 0.5*(double)N ) < 5.0*sd,
	       "`count` is a PRE-mask budget: about half the candidates survive a half-masked painter" );
}

// ============================================================
//  4. Length painter and width lerp
// ============================================================

static void RunLengthAndWidth()
{
	std::cout << "=== 4. Length painter and width lerp ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	// The UNBENT configuration: no comb / gravity / curl / frizz, so a
	// strand is exactly a straight segment of the authored length and
	// its arc length is exact rather than shortened by a bend.
	HairGroomRecipe r = PlainRecipe( base.get(), 200 );

	Owned<HairGeometry> plain( BuildAndRealize( r ) );
	if( !plain ) { Check( false, "unbent groom built" ); return; }

	double worstLenErr = 0;
	double worstRootW  = 0;
	double worstTipW   = 0;
	for( unsigned int s = 0; s < plain->numStrands(); ++s ) {
		worstLenErr = std::max( worstLenErr, fabs( (double)plain->StrandArcLength( s ) - r.p.length ) );
		worstRootW  = std::max( worstRootW,  fabs( (double)plain->StrandWidthAt( s, 0 ) - r.p.widthRoot ) );
		worstTipW   = std::max( worstTipW,   fabs( (double)plain->StrandWidthAt( s, 1 ) - r.p.widthTip ) );
	}
	std::cout << "  worst arc-length error     : " << worstLenErr << std::endl;

	Check( worstLenErr < 1e-6, "an unbent strand's arc length is exactly the authored `length`" );
	Check( worstRootW < 1e-9,  "the reported width at s = 0 is `width_root`" );
	Check( worstTipW  < 1e-9,  "the reported width at s = 1 is `width_tip`" );

	// Half-length painter: every strand halves.
	Owned<ConstScalarPainter> half( new ConstScalarPainter( 0.5 ) );
	r.pLengthScale = half.get();
	Owned<HairGeometry> scaled( BuildAndRealize( r ) );
	if( !scaled ) { Check( false, "length-scaled groom built" ); return; }

	double worstScaledErr = 0;
	for( unsigned int s = 0; s < scaled->numStrands(); ++s ) {
		worstScaledErr = std::max( worstScaledErr,
			fabs( (double)scaled->StrandArcLength( s ) - 0.5 * r.p.length ) );
	}
	std::cout << "  worst scaled-length error  : " << worstScaledErr << std::endl;
	Check( worstScaledErr < 1e-6,
	       "MONEY: a 0.5 length painter halves every strand's measured arc length" );
	Check( scaled->numStrands() == plain->numStrands(),
	       "a length painter changes lengths, not which candidates survive" );

	// A ZERO length painter drops every strand rather than emitting
	// degenerate zero-length hair.
	Owned<ConstScalarPainter> zero( new ConstScalarPainter( 0.0 ) );
	r.pLengthScale = zero.get();
	Owned<HairGeometry> none( BuildAndRealize( r ) );
	if( !none ) { Check( false, "zero-length groom built" ); return; }
	Check( none->numStrands() == 0, "a zero length painter drops every strand (no zero-length hair)" );
}

// ============================================================
//  5. Clumping
// ============================================================

static void RunClumping()
{
	std::cout << "=== 5. Clumping ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	const Scalar kCell = 0.25;

	HairGroomRecipe r = PlainRecipe( base.get(), 600, 8 );
	// Frizz, so the strands are NOT all parallel and clumping has
	// something to converge.  Perfectly parallel quills on a flat base
	// all point the same way, so their tips are already as close as the
	// roots are and a clump pull would be a near-no-op -- the test would
	// pass without proving anything.
	r.p.frizz = 0.4;

	Owned<HairGeometry> off( BuildAndRealize( r ) );

	r.p.clump     = 0.9;
	r.p.clumpSize = kCell;
	Owned<HairGeometry> on( BuildAndRealize( r ) );

	if( !off || !on ) { Check( false, "clumped and unclumped grooms built" ); return; }
	Check( off->numStrands() == on->numStrands(), "clumping does not change which candidates survive" );

	// Reproduce the DOCUMENTED cell rule so this measures the contract,
	// not merely "the tips moved": roots are quantised onto a grid of
	// side `clump_size`; each occupied cell's LOWEST-INDEX strand is
	// that cell's centre.  Roots are identical in both grooms (clumping
	// never moves a root), so one assignment serves both.
	std::map<long long, unsigned int> centreOfCell;
	std::vector<unsigned int> centre( off->numStrands(), 0 );
	std::vector<bool> hasCentre( off->numStrands(), false );
	for( unsigned int s = 0; s < off->numStrands(); ++s ) {
		const Point3 root = off->ControlPoint( s, 0 );
		const long long cx = (long long)floor( (double)root.x / (double)kCell );
		const long long cy = (long long)floor( (double)root.y / (double)kCell );
		const long long cz = (long long)floor( (double)root.z / (double)kCell );
		const long long key = ( cx * 73856093LL ) ^ ( cy * 19349663LL ) ^ ( cz * 83492791LL );
		std::map<long long, unsigned int>::const_iterator it = centreOfCell.find( key );
		if( it == centreOfCell.end() ) { centreOfCell[key] = s; }
		else if( it->second != s )     { centre[s] = it->second; hasCentre[s] = true; }
	}

	double sumOff = 0, sumOn = 0;
	unsigned int n = 0;
	for( unsigned int s = 0; s < off->numStrands(); ++s ) {
		if( !hasCentre[s] ) continue;
		sumOff += Point3Ops::Distance( StrandTip( *off, s ), StrandTip( *off, centre[s] ) );
		sumOn  += Point3Ops::Distance( StrandTip( *on,  s ), StrandTip( *on,  centre[s] ) );
		++n;
	}
	Check( n > 20, "the clump-cell partition actually put strands into shared cells" );
	if( n > 0 ) {
		std::cout << "  clumped strands            : " << n << " of " << off->numStrands() << std::endl;
		std::cout << "  mean tip-to-centre, off    : " << sumOff / (double)n << std::endl;
		std::cout << "  mean tip-to-centre, on     : " << sumOn  / (double)n << std::endl;
		Check( sumOn < sumOff,
		       "MONEY: clumping strictly reduces the mean tip-to-clump-centre distance at the same seed" );
	}

	// Roots never move, however hard the clump pulls.
	double worstRootMove = 0;
	for( unsigned int s = 0; s < off->numStrands(); ++s ) {
		worstRootMove = std::max( worstRootMove,
			(double)Point3Ops::Distance( off->ControlPoint( s, 0 ), on->ControlPoint( s, 0 ) ) );
	}
	Check( worstRootMove == 0.0, "clumping never moves a root (the t^2 weight is exactly 0 there)" );

	// clump_size 0 disables clumping entirely, whatever `clump` says.
	r.p.clumpSize = 0.0;
	Owned<HairGeometry> disabled( BuildAndRealize( r ) );
	if( disabled ) {
		Check( GroomsIdentical( *off, *disabled ),
		       "clump_size = 0 disables clumping entirely, even at clump = 0.9" );
	} else {
		Check( false, "clump_size = 0 groom built" );
	}
}

// ============================================================
//  6. Frizz and curl
// ============================================================

static void RunFrizzAndCurl()
{
	std::cout << "=== 6. Frizz and curl ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	const HairGroomRecipe plain = PlainRecipe( base.get(), 300, 10 );
	Owned<HairGeometry> baseline( BuildAndRealize( plain ) );
	if( !baseline ) { Check( false, "baseline groom built" ); return; }

	// The straight-growth baseline really is straight: every control
	// point sits on the surface normal through its own root.
	{
		double worstLateral = 0;
		for( unsigned int s = 0; s < baseline->numStrands(); ++s ) {
			const Point3 root = baseline->ControlPoint( s, 0 );
			for( unsigned int k = 0; k < baseline->numControlPointsOfStrand( s ); ++k ) {
				const Point3 p = baseline->ControlPoint( s, k );
				worstLateral = std::max( worstLateral,
					sqrt( ( p.x - root.x )*( p.x - root.x ) + ( p.y - root.y )*( p.y - root.y ) ) );
			}
		}
		Check( worstLateral < 1e-9, "with every styling knob off, strands grow straight along the normal" );
	}

	// Explicit zeros reproduce the baseline EXACTLY -- the effects are
	// skipped, not applied at zero amplitude through a path that would
	// still perturb the stream.
	{
		HairGroomRecipe r = plain;
		r.p.frizz = 0.0; r.p.curlRadius = 0.0; r.p.curlStep = 0.0;
		r.p.gravity = 0.0; r.p.clump = 0.0; r.p.clumpSize = 0.0;
		Owned<HairGeometry> z( BuildAndRealize( r ) );
		if( !z ) { Check( false, "all-zero groom built" ); return; }
		Check( GroomsIdentical( *baseline, *z ),
		       "MONEY: explicitly-zero frizz / curl / gravity / clump reproduce the baseline BIT-IDENTICALLY" );
	}

	// Frizz moves control points, but never the root.
	{
		HairGroomRecipe r = plain;
		r.p.frizz = 0.5;
		Owned<HairGeometry> f( BuildAndRealize( r ) );
		if( !f ) { Check( false, "frizzed groom built" ); return; }
		Check( !GroomsIdentical( *baseline, *f ), "non-zero frizz moves the strands off the baseline" );
		Check( f->numStrands() == baseline->numStrands(), "frizz does not change the strand count" );
		double worstRootMove = 0;
		for( unsigned int s = 0; s < f->numStrands(); ++s ) {
			worstRootMove = std::max( worstRootMove,
				(double)Point3Ops::Distance( f->ControlPoint( s, 0 ), baseline->ControlPoint( s, 0 ) ) );
		}
		Check( worstRootMove == 0.0, "frizz never moves a root" );
	}

	// Curl moves control points, but never the root, and its lateral
	// excursion is bounded by curl_radius.
	{
		HairGroomRecipe r = plain;
		r.p.curlRadius = 0.05;
		r.p.curlStep   = 0.2;
		Owned<HairGeometry> c( BuildAndRealize( r ) );
		if( !c ) { Check( false, "curled groom built" ); return; }
		Check( !GroomsIdentical( *baseline, *c ), "non-zero curl moves the strands off the baseline" );

		double worstRootMove = 0, worstLateral = 0;
		for( unsigned int s = 0; s < c->numStrands(); ++s ) {
			const Point3 root = c->ControlPoint( s, 0 );
			worstRootMove = std::max( worstRootMove,
				(double)Point3Ops::Distance( root, baseline->ControlPoint( s, 0 ) ) );
			for( unsigned int k = 0; k < c->numControlPointsOfStrand( s ); ++k ) {
				const Point3 p = c->ControlPoint( s, k );
				worstLateral = std::max( worstLateral,
					sqrt( ( p.x - root.x )*( p.x - root.x ) + ( p.y - root.y )*( p.y - root.y ) ) );
			}
		}
		std::cout << "  worst curl lateral offset  : " << worstLateral
		          << "  (curl_radius " << r.p.curlRadius << ")" << std::endl;
		Check( worstRootMove == 0.0, "curl never moves a root (the helix opens out of the follicle)" );
		// The bound is checked to FLOAT precision, not double: control
		// points are stored as float (HairGeometry's documented
		// storage exception), so a coordinate near 1 carries ~1e-7 of
		// quantisation, and both the root and the control point are
		// quantised independently.
		Check( worstLateral > 1e-6 && worstLateral <= r.p.curlRadius + 1e-5,
		       "the curl's lateral excursion is non-zero and bounded by curl_radius" );
	}

	// Gravity pulls tips down in world -Y and nothing else.
	{
		HairGroomRecipe r = plain;
		r.p.gravity = 0.5;
		Owned<HairGeometry> gv( BuildAndRealize( r ) );
		if( !gv ) { Check( false, "gravity groom built" ); return; }
		int wrongWay = 0;
		for( unsigned int s = 0; s < gv->numStrands(); ++s ) {
			const Point3 a = StrandTip( *gv, s );
			const Point3 b = StrandTip( *baseline, s );
			if( !( a.y < b.y - 1e-9 ) ) ++wrongWay;
		}
		Check( wrongWay == 0, "gravity moves every tip DOWN in world -Y" );
	}
}

// ============================================================
//  7. The Realize contract
// ============================================================

static void RunRealizeContract()
{
	std::cout << "=== 7. The Realize contract ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	HairGroomRecipe r = PlainRecipe( base.get(), 300 );
	r.p.widthRoot = 0.05;
	r.p.widthTip  = 0.03;

	IGeometry* raw = 0;
	if( !RISE_API_CreateHairGeometryGroom( &raw, r, "realize_groom" ) || !raw ) {
		Check( false, "deferred groom constructed" );
		return;
	}
	Owned<HairGeometry> g( dynamic_cast<HairGeometry*>( raw ) );

	Check( g->IsValid(),    "a deferred groom with a tessellatable base is valid at construction" );
	Check( !g->IsRealized(), "a deferred groom is NOT realized at construction" );

	// -- DOCUMENTED pre-Realize behaviour: defined and EMPTY.  No
	//    strands, a degenerate box, and every ray misses.  Not a crash,
	//    and not a silent partial groom.
	Check( g->numStrands()  == 0, "a groom before Realize() has no strands" );
	Check( g->numSegments() == 0, "a groom before Realize() has no segments" );

	RandomNumberGenerator rng( 4242u );
	int hitsBefore = 0;
	std::vector<Ray> probes;
	for( int i = 0; i < 400; ++i ) {
		const Point3 o( rng.CanonicalRandom(), rng.CanonicalRandom(), -1.0 );
		const Vector3 d = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) );
		probes.push_back( Ray( o, d ) );
	}
	for( size_t i = 0; i < probes.size(); ++i ) {
		RayIntersectionGeometric ri( probes[i], nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		if( ri.bHit ) ++hitsBefore;
		if( g->IntersectRay_IntersectionOnly( probes[i], RISE_INFINITY, true, true ) ) ++hitsBefore;
	}
	Check( hitsBefore == 0, "MONEY: a groom before Realize() intersects NOTHING (defined and empty, no crash)" );

	g->Realize();
	Check( g->IsRealized(), "Realize() marks the groom realized" );
	Check( g->numStrands() > 0, "Realize() actually generated strands" );

	int hitsAfter = 0;
	for( size_t i = 0; i < probes.size(); ++i ) {
		RayIntersectionGeometric ri( probes[i], nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		if( ri.bHit ) ++hitsAfter;
	}
	std::cout << "  hits before / after        : " << 0 << " / " << hitsAfter << std::endl;
	Check( hitsAfter > 0, "after Realize() the groom is hit by rays that previously missed" );

	// A groom is never an area light and never tessellates, deferred or
	// not -- the composites that would misuse it must refuse at parse.
	Check( !g->CanBeAreaLight(), "a groom refuses to serve as an area light" );
	Check( !g->CanTessellate(),  "a groom refuses to tessellate (so it cannot be a displaced_geometry base)" );
}

// ============================================================
//  8. The chunk
// ============================================================

static void RunChunk()
{
	std::cout << "=== 8. The hair_geometry chunk ===" << std::endl;

	const std::string kBase =
		"sphere_geometry\n{\n\tname\tcc_base\n\tradius\t1.0\n}\n";

	// -- minimal chunk parses and registers a geometry.
	{
		IJobPriv* job = 0;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created (minimal)" ); return; }
		const std::string body = kBase +
			"hair_geometry\n{\n\tname\tcc_groom\n\tbase_geometry\tcc_base\n\tcount\t200\n\tlength\t0.1\n}\n";
		const bool ok = ParseBodyInto( "minimal", body, *job );
		Check( ok, "a minimal hair_geometry chunk parses" );
		IGeometry* geom = job->GetGeometries() ? job->GetGeometries()->GetItem( "cc_groom" ) : 0;
		Check( geom != 0, "the minimal chunk registers a geometry under its own name" );
		if( geom ) {
			HairGeometry* hg = dynamic_cast<HairGeometry*>( geom );
			Check( hg != 0, "the registered geometry really is a HairGeometry" );
			if( hg ) {
				Check( !hg->IsRealized(), "the chunk does NOT generate at parse time (generation is deferred)" );
				hg->Realize();
				Check( hg->numStrands() > 0, "realizing the parsed groom grows strands on the sphere" );
			}
		}
		safe_release( job );
	}

	// -- a fully-styled chunk parses (every optional parameter exercised
	//    at once, so a descriptor/Finalize name mismatch cannot hide).
	{
		IJobPriv* job = 0;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created (styled)" ); return; }
		const std::string body = kBase +
			"scalar_painter\n{\n\tname\tcc_dens\n\tvalue\t0.8\n}\n"
			"scalar_painter\n{\n\tname\tcc_len\n\tvalue\t0.7\n}\n"
			"uniformcolor_painter\n{\n\tname\tcc_comb\n\tcolor\t0.9 0.5 0.5\n}\n"
			"hair_geometry\n{\n"
			"\tname\tcc_groom\n\tbase_geometry\tcc_base\n\tcount\t300\n\tlength\t0.12\n"
			"\tsegments\t10\n\twidth_root\t0.004\n\twidth_tip\t0.001\n\tseed\t3\n\tbase_detail\t16\n"
			"\tdensity\tcc_dens\n\tlength_painter\tcc_len\n\tcomb\tcc_comb\n"
			"\tgravity\t0.2\n\tfrizz\t0.1\n\tclump\t0.5\n\tclump_size\t0.05\n"
			"\tcurl_radius\t0.01\n\tcurl_step\t0.05\n}\n";
		std::string captured;
		const bool ok = ParseBodyCapturing( "styled", body, *job, captured );
		Check( ok, "a fully-styled hair_geometry chunk parses (every optional parameter bound)" );
		IGeometry* geom = job->GetGeometries() ? job->GetGeometries()->GetItem( "cc_groom" ) : 0;
		Check( geom != 0, "the styled chunk registers its geometry" );
		if( geom ) {
			geom->Realize();
			HairGeometry* hg = dynamic_cast<HairGeometry*>( geom );
			Check( hg && hg->numStrands() > 0, "the styled groom grows strands" );
		}
		safe_release( job );
	}

	// -- `segments` and `seed` REACH the generator through the chunk.
	//    Parsing successfully only proves the DESCRIPTOR declares a
	//    parameter; it does not prove Finalize reads it under the same
	//    name (a typo there would silently keep the default forever).
	//    These two are observable in the resulting groom, so they pin
	//    the descriptor-to-Finalize name agreement behaviourally.
	{
		auto GroomWith = []( const char* tag, const std::string& base,
		                     unsigned segments, unsigned seed,
		                     std::vector<Point3>& outTips, unsigned& outCPs ) -> bool {
			IJobPriv* job = 0;
			if( !RISE_CreateJobPriv( &job ) || !job ) { return false; }
			std::ostringstream oss;
			oss << base
			    << "hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n"
			    << "\tcount\t120\n\tlength\t0.1\n\tsegments\t" << segments
			    << "\n\tseed\t" << seed << "\n}\n";
			bool good = false;
			if( ParseBodyInto( tag, oss.str(), *job ) ) {
				IGeometry* geom = job->GetGeometries() ? job->GetGeometries()->GetItem( "g" ) : 0;
				HairGeometry* hg = geom ? dynamic_cast<HairGeometry*>( geom ) : 0;
				if( hg ) {
					hg->Realize();
					outCPs = hg->numStrands() ? hg->numControlPointsOfStrand( 0 ) : 0;
					for( unsigned s = 0; s < hg->numStrands(); ++s ) { outTips.push_back( StrandTip( *hg, s ) ); }
					good = !outTips.empty();
				}
			}
			safe_release( job );
			return good;
		};

		std::vector<Point3> tipsA, tipsB, tipsC;
		unsigned cpsA = 0, cpsB = 0, cpsC = 0;
		const bool a = GroomWith( "seg4_seed1", kBase, 4, 1, tipsA, cpsA );
		const bool b = GroomWith( "seg4_seed2", kBase, 4, 2, tipsB, cpsB );
		const bool c = GroomWith( "seg9_seed1", kBase, 9, 1, tipsC, cpsC );

		Check( a && b && c, "the three seed/segments probe grooms all built" );
		if( a && b && c ) {
			Check( cpsA == 4 && cpsC == 9,
			       "MONEY: `segments` reaches the generator through the chunk (4 and 9 control points per strand)" );
			bool same = ( tipsA.size() == tipsB.size() );
			for( size_t i = 0; same && i < tipsA.size(); ++i ) {
				same = ( tipsA[i].x == tipsB[i].x && tipsA[i].y == tipsB[i].y && tipsA[i].z == tipsB[i].z );
			}
			Check( !same, "MONEY: `seed` reaches the generator through the chunk (two seeds give different strands)" );
		}
	}

	// -- negative fixtures.  Each must be REJECTED, with a diagnostic
	//    naming the offending parameter.
	struct Neg { const char* tag; const char* body; const char* mustSay; };
	const std::string missingBase =
		"hair_geometry\n{\n\tname\tg\n\tcount\t10\n\tlength\t0.1\n}\n";
	const std::string unknownBase =
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tno_such_thing\n\tcount\t10\n\tlength\t0.1\n}\n";
	const std::string missingCount = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tlength\t0.1\n}\n";
	const std::string missingLength = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n}\n";
	const std::string zeroCount = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t0\n\tlength\t0.1\n}\n";
	const std::string hugeCount = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t9000000\n\tlength\t0.1\n}\n";
	const std::string oneSegment = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0.1\n\tsegments\t1\n}\n";
	const std::string zeroLength = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0\n}\n";
	const std::string zeroWidth = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0.1\n\twidth_root\t0\n}\n";
	const std::string curlNoStep = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0.1\n\tcurl_radius\t0.01\n}\n";
	// An infinite plane cannot tessellate, so it cannot carry hair.
	const std::string badBaseKind =
		"infiniteplane_geometry\n{\n\tname\tflat\n}\n"
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tflat\n\tcount\t10\n\tlength\t0.1\n}\n";

	const Neg negs[] = {
		{ "missing_base",   missingBase.c_str(),   "base_geometry" },
		{ "unknown_base",   unknownBase.c_str(),   "no_such_thing" },
		{ "missing_count",  missingCount.c_str(),  "count" },
		{ "missing_length", missingLength.c_str(), "length" },
		{ "zero_count",     zeroCount.c_str(),     "count" },
		{ "huge_count",     hugeCount.c_str(),     "count" },
		{ "one_segment",    oneSegment.c_str(),    "segments" },
		{ "zero_length",    zeroLength.c_str(),    "length" },
		{ "zero_width",     zeroWidth.c_str(),     "width_root" },
		{ "curl_no_step",   curlNoStep.c_str(),    "curl_step" },
		{ "bad_base_kind",  badBaseKind.c_str(),   "tessellat" },
	};

	for( size_t i = 0; i < sizeof(negs)/sizeof(negs[0]); ++i ) {
		IJobPriv* job = 0;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created (negative)" ); return; }
		std::string captured;
		const bool ok = ParseBodyCapturing( negs[i].tag, negs[i].body, *job, captured );
		Check( !ok, std::string( "negative fixture `" ) + negs[i].tag + "` is REJECTED" );
		Check( captured.find( negs[i].mustSay ) != std::string::npos,
		       std::string( "negative fixture `" ) + negs[i].tag +
		       "`: the diagnostic names `" + negs[i].mustSay + "`" );
		safe_release( job );
	}
}

// ============================================================
//  main
// ============================================================

int main()
{
	std::cout << "===== HairGeneratorTest =====" << std::endl << std::endl;

	RunDeterminism();      std::cout << std::endl;
	RunRootPlacement();    std::cout << std::endl;
	RunDensityMask();      std::cout << std::endl;
	RunLengthAndWidth();   std::cout << std::endl;
	RunClumping();         std::cout << std::endl;
	RunFrizzAndCurl();     std::cout << std::endl;
	RunRealizeContract();  std::cout << std::endl;
	RunChunk();            std::cout << std::endl;

	std::cout << "===== Summary =====" << std::endl;
	std::cout << "checks run:    " << g_checks << std::endl;
	std::cout << "checks failed: " << g_failures << std::endl;

	if( g_failures > 0 ) {
		std::cout << std::endl << "HairGeneratorTest FAILED" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All hair generator tests passed!" << std::endl;
	return 0;
}
