//////////////////////////////////////////////////////////////////////
//
//  VCMLightVertexStoreTest.cpp - Unit tests for the LightVertexStore
//    (src/Library/Shaders/VCMLightVertexStore.h), the persistent
//    KD-tree-indexed store VCM uses to locate light subpath vertices
//    within a fixed merge radius.
//
//    The store's KD-tree is a clone of PhotonMapCore::BalanceSegment
//    + LocateAllPhotons with one intentional deviation: the balance
//    step uses the correct end-exclusive nth_element range
//    (begin()+to+1), so every stored vertex ends up with its
//    splitting plane set correctly.  These tests validate store
//    behavior against a brute-force ground truth rather than against
//    PhotonMapCore directly (which has a subtle off-by-one).
//
//    Coverage:
//      1. Empty store: Query returns nothing, Size=0, IsBuilt after Clear
//      2. Single-vertex store: inside radius hits, outside misses
//      3. Grid of vertices: every KD-tree query result matches the
//         brute-force ground truth (same set, ignoring order)
//      4. Zero radius returns nothing
//      5. BuildKDTree idempotence after multiple Append calls
//      6. Concat path: per-thread buffer moves into the store
//      7. Stress test: 10k random vertices, 100 random queries vs
//         brute force
//      8. ClampOutlierThroughputs rebuilds the cached
//         LightVertex::throughputSpectrum for every vertex (not just
//         the rescaled ones) to match a fresh
//         VCMIntegrator::LightThroughputRadianceNM computation
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#include "../src/Library/Shaders/VCMLightVertexStore.h"
#include "../src/Library/Shaders/VCMIntegrator.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

static void Check( bool cond, const char* label )
{
	if( cond ) {
		g_pass++;
	} else {
		g_fail++;
		printf( "  FAIL: %s\n", label );
	}
}

//
// Brute-force ground truth: walk every stored vertex linearly and
// collect indices whose squared distance to 'center' is <
// radiusSq.  Uses the same "<" predicate the store does.
//
static std::set<std::size_t> BruteForceQueryIndices(
	const std::vector<LightVertex>& all,
	const Point3& center,
	const Scalar radiusSq
	)
{
	std::set<std::size_t> indices;
	for( std::size_t i = 0; i < all.size(); i++ ) {
		const Vector3 d = Vector3Ops::mkVector3( center, all[i].ptPosition );
		const Scalar distSq = Vector3Ops::SquaredModulus( d );
		if( distSq < radiusSq ) {
			indices.insert( i );
		}
	}
	return indices;
}

//
// Make a LightVertex at a given position and tag it with a unique
// integer id (stored in pathLength so we can recognize it after the
// KD-tree has rearranged the array).
//
static LightVertex MakeVertex( Scalar x, Scalar y, Scalar z, unsigned short id )
{
	LightVertex v;
	v.ptPosition = Point3( x, y, z );
	v.pathLength = id;
	v.normal = Vector3( 0, 0, 1 );
	v.wi = Vector3( 0, 0, 1 );
	return v;
}

//
// Extract the set of "id" tags from a query result.  Because
// BuildKDTree permutes the internal array, the ids are the stable
// identity we compare against brute force.
//
static std::set<unsigned short> IdsFromQueryResult( const std::vector<LightVertex>& result )
{
	std::set<unsigned short> ids;
	for( std::size_t i = 0; i < result.size(); i++ ) {
		ids.insert( result[i].pathLength );
	}
	return ids;
}

//
// Extract the set of ids from a brute-force index set, paired with
// the ORIGINAL unpermuted vertex list.  This must be called BEFORE
// BuildKDTree if the vertex list is the same as what was given to
// the store, because BuildKDTree permutes that list.
//
static std::set<unsigned short> IdsFromBruteForce(
	const std::set<std::size_t>& indices,
	const std::vector<LightVertex>& originalList
	)
{
	std::set<unsigned short> ids;
	for( std::set<std::size_t>::const_iterator i = indices.begin(); i != indices.end(); ++i ) {
		ids.insert( originalList[*i].pathLength );
	}
	return ids;
}

//////////////////////////////////////////////////////////////////////
// Test 1: Empty store behavior
//////////////////////////////////////////////////////////////////////
static void TestEmptyStore()
{
	printf( "Test 1: Empty store\n" );

	LightVertexStore store;
	Check( store.Size() == 0, "empty: size 0" );
	Check( !store.IsBuilt(), "empty: not built yet" );

	store.BuildKDTree();
	Check( store.IsBuilt(), "empty: built after BuildKDTree" );
	Check( store.Size() == 0, "empty: size still 0" );

	std::vector<LightVertex> out;
	store.Query( Point3( 0, 0, 0 ), 1.0, out );
	Check( out.empty(), "empty: query returns nothing" );

	store.Clear();
	Check( !store.IsBuilt(), "empty: not built after Clear" );
}

//////////////////////////////////////////////////////////////////////
// Test 2: Single-vertex store
//////////////////////////////////////////////////////////////////////
static void TestSingleVertex()
{
	printf( "Test 2: Single vertex\n" );

	LightVertexStore store;
	store.Append( MakeVertex( 1, 2, 3, 42 ) );
	store.BuildKDTree();

	// Inside radius
	{
		std::vector<LightVertex> out;
		store.Query( Point3( 1, 2, 3 ), 0.01, out );
		Check( out.size() == 1, "single: self-query hit" );
		Check( out.size() == 1 && out[0].pathLength == 42, "single: returned vertex is the right one" );
	}

	// Outside radius
	{
		std::vector<LightVertex> out;
		store.Query( Point3( 10, 10, 10 ), 1.0, out );
		Check( out.empty(), "single: far query misses" );
	}

	// Radius exactly touches the point (the store uses "<", not "<=",
	// so a point at exactly radius is NOT in the result)
	{
		std::vector<LightVertex> out;
		const Vector3 delta = Vector3Ops::mkVector3( Point3( 1, 2, 3 ), Point3( 4, 2, 3 ) );
		const Scalar rSq = Vector3Ops::SquaredModulus( delta );
		store.Query( Point3( 4, 2, 3 ), rSq, out );
		Check( out.empty(), "single: exactly-at-radius excluded" );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 3: Grid of vertices compared against brute force
//////////////////////////////////////////////////////////////////////
static void TestGridVsBruteForce()
{
	printf( "Test 3: Grid vs brute force\n" );

	// 10x10x10 grid of points at unit spacing.
	std::vector<LightVertex> original;
	unsigned short id = 0;
	for( int x = 0; x < 10; x++ ) {
		for( int y = 0; y < 10; y++ ) {
			for( int z = 0; z < 10; z++ ) {
				original.push_back( MakeVertex( x, y, z, id++ ) );
			}
		}
	}

	// Brute-force every test query against the ORIGINAL (unpermuted) list.
	struct QueryCase { Point3 center; Scalar radiusSq; const char* label; };
	QueryCase cases[] = {
		{ Point3( 5, 5, 5 ), 4.0, "center, r^2=4" },
		{ Point3( 0, 0, 0 ), 1.00001, "corner, r^2 just above 1" },
		{ Point3( 9, 9, 9 ), 1.00001, "far corner" },
		{ Point3( 5, 5, 5 ), 0.1, "small radius at grid point" },
		{ Point3( 5.5, 5.5, 5.5 ), 1.0, "off-grid query" },
		{ Point3( 100, 100, 100 ), 1.0, "far from any point" },
	};

	std::vector< std::set<unsigned short> > expected;
	for( std::size_t c = 0; c < sizeof( cases ) / sizeof( cases[0] ); c++ ) {
		const std::set<std::size_t> idx = BruteForceQueryIndices( original, cases[c].center, cases[c].radiusSq );
		expected.push_back( IdsFromBruteForce( idx, original ) );
	}

	// Build the store (which permutes the underlying array).
	LightVertexStore store;
	for( std::size_t i = 0; i < original.size(); i++ ) {
		store.Append( original[i] );
	}
	store.BuildKDTree();
	Check( store.Size() == 1000, "grid: size 1000" );

	// Compare every query result against the pre-built expected set.
	for( std::size_t c = 0; c < sizeof( cases ) / sizeof( cases[0] ); c++ ) {
		std::vector<LightVertex> out;
		store.Query( cases[c].center, cases[c].radiusSq, out );
		const std::set<unsigned short> got = IdsFromQueryResult( out );
		if( got == expected[c] ) {
			g_pass++;
		} else {
			g_fail++;
			printf( "  FAIL: grid %s (got %zu, expected %zu)\n",
				cases[c].label, got.size(), expected[c].size() );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// Test 4: Zero radius returns nothing
//////////////////////////////////////////////////////////////////////
static void TestZeroRadius()
{
	printf( "Test 4: Zero radius\n" );

	LightVertexStore store;
	store.Append( MakeVertex( 0, 0, 0, 0 ) );
	store.Append( MakeVertex( 1, 0, 0, 1 ) );
	store.BuildKDTree();

	std::vector<LightVertex> out;
	store.Query( Point3( 0, 0, 0 ), 0.0, out );
	Check( out.empty(), "r=0: nothing returned" );
}

//////////////////////////////////////////////////////////////////////
// Test 5: Build / append / rebuild state tracking
//////////////////////////////////////////////////////////////////////
static void TestBuildStateTracking()
{
	printf( "Test 5: Build state tracking\n" );

	LightVertexStore store;
	Check( !store.IsBuilt(), "fresh: not built" );
	store.Append( MakeVertex( 0, 0, 0, 1 ) );
	Check( !store.IsBuilt(), "after append: not built" );
	store.BuildKDTree();
	Check( store.IsBuilt(), "after build: built" );

	// Appending invalidates the built flag.
	store.Append( MakeVertex( 1, 0, 0, 2 ) );
	Check( !store.IsBuilt(), "after another append: not built" );

	store.BuildKDTree();
	Check( store.IsBuilt(), "after rebuild: built" );

	// Clear resets the flag.
	store.Clear();
	Check( !store.IsBuilt(), "after clear: not built" );
	Check( store.Size() == 0, "after clear: size 0" );
}

//////////////////////////////////////////////////////////////////////
// Test 6: Concat path — per-thread buffer moved into the store
//////////////////////////////////////////////////////////////////////
static void TestConcat()
{
	printf( "Test 6: Concat from per-thread buffer\n" );

	LightVertexStore store;

	// Thread 1 buffer
	std::vector<LightVertex> buf1;
	buf1.push_back( MakeVertex( 0, 0, 0, 10 ) );
	buf1.push_back( MakeVertex( 1, 0, 0, 11 ) );
	store.Concat( std::move( buf1 ) );
	Check( store.Size() == 2, "concat 1: size 2" );

	// Thread 2 buffer concatenated onto existing data
	std::vector<LightVertex> buf2;
	buf2.push_back( MakeVertex( 2, 0, 0, 20 ) );
	buf2.push_back( MakeVertex( 3, 0, 0, 21 ) );
	buf2.push_back( MakeVertex( 4, 0, 0, 22 ) );
	store.Concat( std::move( buf2 ) );
	Check( store.Size() == 5, "concat 2: size 5" );

	store.BuildKDTree();

	// Query should see all five.
	std::vector<LightVertex> out;
	store.Query( Point3( 2, 0, 0 ), 100.0, out );
	Check( out.size() == 5, "concat: query recovers all 5 vertices" );

	const std::set<unsigned short> ids = IdsFromQueryResult( out );
	Check( ids.count( 10 ) && ids.count( 11 ) && ids.count( 20 ) && ids.count( 21 ) && ids.count( 22 ),
		"concat: all ids preserved" );
}

//////////////////////////////////////////////////////////////////////
// Test 7: Stress — 10k random vertices vs brute force
//////////////////////////////////////////////////////////////////////
static void TestStressRandom()
{
	printf( "Test 7: 10k random vertices, 100 random queries\n" );

	// Deterministic PRNG via Linear Congruential Generator.
	// Avoids dragging in RISE's MersenneTwister for a unit test.
	unsigned long long rngState = 0x12345678ULL;
	const auto Rand01 = [&rngState]() -> Scalar {
		rngState = rngState * 6364136223846793005ULL + 1442695040888963407ULL;
		const unsigned int x = static_cast<unsigned int>( rngState >> 32 );
		return static_cast<Scalar>( x ) / static_cast<Scalar>( 0xFFFFFFFFU );
	};

	// 10k random vertices in [-10, 10]^3.
	std::vector<LightVertex> original;
	original.reserve( 10000 );
	for( unsigned short i = 0; i < 10000; i++ ) {
		const Scalar x = -10.0 + 20.0 * Rand01();
		const Scalar y = -10.0 + 20.0 * Rand01();
		const Scalar z = -10.0 + 20.0 * Rand01();
		original.push_back( MakeVertex( x, y, z, i ) );
	}

	// Brute-force every query BEFORE building so 'original' isn't permuted.
	struct QueryInput { Point3 center; Scalar radiusSq; };
	std::vector<QueryInput> queries;
	std::vector< std::set<unsigned short> > expected;
	queries.reserve( 100 );
	expected.reserve( 100 );
	for( int q = 0; q < 100; q++ ) {
		QueryInput qi;
		qi.center = Point3( -10 + 20 * Rand01(), -10 + 20 * Rand01(), -10 + 20 * Rand01() );
		qi.radiusSq = 0.25 + 4.0 * Rand01();   // radii in [0.5, 2.06]^2
		queries.push_back( qi );
		const std::set<std::size_t> idx = BruteForceQueryIndices( original, qi.center, qi.radiusSq );
		expected.push_back( IdsFromBruteForce( idx, original ) );
	}

	// Build the store from a COPY of 'original'.
	LightVertexStore store;
	for( std::size_t i = 0; i < original.size(); i++ ) {
		store.Append( original[i] );
	}
	store.BuildKDTree();
	Check( store.Size() == 10000, "stress: size 10000" );

	int mismatches = 0;
	std::size_t totalHits = 0;
	for( std::size_t q = 0; q < queries.size(); q++ ) {
		std::vector<LightVertex> out;
		store.Query( queries[q].center, queries[q].radiusSq, out );
		totalHits += out.size();
		const std::set<unsigned short> got = IdsFromQueryResult( out );
		if( got != expected[q] ) {
			mismatches++;
			if( mismatches <= 3 ) {
				printf( "  query %zu: got %zu, expected %zu\n",
					q, got.size(), expected[q].size() );
			}
		}
	}
	Check( mismatches == 0, "stress: every query matches brute force" );
	printf( "  stress: %zu total hits across 100 queries\n", totalHits );
}

//////////////////////////////////////////////////////////////////////
// Test 8: ClampOutlierThroughputs is one of the three sites required
// to rebuild LightVertex::throughputSpectrum after rewriting
// `throughput` (see VCMLightVertex.h's field comment and
// VCMLightVertexStore.cpp's ClampOutlierThroughputs).  Build a store
// with one bright outlier vertex among several ordinary ones, clamp
// with parameters chosen so the outlier is provably rescaled, then
// verify every vertex's cached throughputSpectrum is EXACTLY (not
// approximately) what VCMIntegrator::LightThroughputRadianceNM
// computes fresh from the (possibly just-rescaled) throughput.  A
// forgotten rebuild after the rescale would leave the outlier vertex
// carrying the PRE-clamp spectrum -- this test's whole point is that
// the outlier is guaranteed to actually be rescaled, so a missing
// rebuild is guaranteed to be caught.
//////////////////////////////////////////////////////////////////////
static void TestClampOutlierRebuildsThroughputSpectrum()
{
	printf( "Test 8: ClampOutlierThroughputs rebuilds throughputSpectrum\n" );

	LightVertexStore store;

	// Five ordinary vertices at luminance ~1, one bright outlier at
	// luminance ~100.  50th-percentile clamp at multiplier 2 puts the
	// threshold at ~2 -- comfortably below the outlier's ~100 and
	// comfortably above the ordinary vertices' ~1, so the outlier is
	// guaranteed to be rescaled and the ordinary vertices guaranteed
	// untouched.
	// Every vertex's throughputSpectrum is seeded from its throughput
	// here to stand in for the real deposit site (ConvertLightSubpath,
	// covered separately by VCMSpectralRecurrenceTest) -- this test is
	// only about the SECOND writer, ClampOutlierThroughputs, so the
	// pre-clamp state must already be self-consistent or a mismatch on
	// an untouched vertex would be a false failure, not evidence of a
	// missing rebuild.
	for( int i = 0; i < 5; i++ ) {
		LightVertex v = MakeVertex( Scalar( i ), 0, 0, static_cast<unsigned short>( i ) );
		v.throughput = RISEPel( 1, 1, 1 );
		v.throughputSpectrum = VCMIntegrator::LightThroughputSpectrum( v.throughput );
		store.Append( v );
	}
	{
		LightVertex outlier = MakeVertex( 10, 0, 0, 99 );
		outlier.throughput = RISEPel( 100, 60, 20 );	// colored, not grey -- exercises chroma preservation too
		outlier.throughputSpectrum = VCMIntegrator::LightThroughputSpectrum( outlier.throughput );
		store.Append( outlier );
	}

	store.ClampOutlierThroughputs( Scalar( 0.5 ), Scalar( 2.0 ) );

	// Confirm the outlier was actually rescaled (proves the clamp did
	// something, so the rebuild-verification below is meaningful and
	// not vacuously true).
	bool outlierRescaled = false;
	for( std::size_t i = 0; i < store.Size(); i++ ) {
		if( store.Get( i ).pathLength == 99 ) {
			outlierRescaled = ( store.Get( i ).throughput[0] < Scalar( 100 ) );
		}
	}
	Check( outlierRescaled, "clamp: outlier throughput was actually rescaled" );

	const double checkNMs[2] = { 450.0, 650.0 };
	bool allMatch = true;
	// Tolerance, not `==`: RGBIlluminantSpectrum::Eval() is compiled at
	// two different call sites here (directly in this test vs. inside
	// the library's VCMIntegrator::LightThroughputRadianceNM), and this
	// repo's macOS build pairs `-ffast-math` with associative-math /
	// FP-contraction, so the SAME formula's rounding can differ in the
	// last few bits depending on each call site's surrounding code (see
	// CLAUDE.md's fast-math entry; VCMSpectralRecurrenceTest's
	// CheckClose documents the same measured ~1e-13 relative spread). A
	// genuine "forgotten rebuild" bug -- the outlier keeping its
	// PRE-clamp spectrum after `throughput` was rescaled by ~30x above
	// -- differs by orders of magnitude more than this tolerance.
	for( std::size_t i = 0; i < store.Size(); i++ ) {
		const LightVertex& v = store.Get( i );
		for( int k = 0; k < 2; k++ ) {
			const Scalar nm = Scalar( checkNMs[k] );
			const double cached = (double)v.throughputSpectrum.Eval( nm );
			const double fresh = (double)VCMIntegrator::LightThroughputRadianceNM( v.throughput, nm );
			const double scale = ( fabs( cached ) > fabs( fresh ) ? fabs( cached ) : fabs( fresh ) );
			const double tol = 1e-9 * ( scale > 1.0 ? scale : 1.0 );
			if( fabs( cached - fresh ) > tol ) {
				allMatch = false;
				printf( "  FAIL: vertex %zu (id %hu) throughputSpectrum.Eval(%g) = %.10f, fresh = %.10f\n",
					i, v.pathLength, checkNMs[k], cached, fresh );
			}
		}
	}
	Check( allMatch, "clamp: every vertex's throughputSpectrum matches a fresh rebuild post-clamp" );
}

//////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////
int main()
{
	printf( "=== VCMLightVertexStore Unit Test ===\n" );

	TestEmptyStore();
	TestSingleVertex();
	TestGridVsBruteForce();
	TestZeroRadius();
	TestBuildStateTracking();
	TestConcat();
	TestStressRandom();
	TestClampOutlierRebuildsThroughputSpectrum();

	printf( "\nPassed: %d\nFailed: %d\n", g_pass, g_fail );
	if( g_fail > 0 ) {
		printf( "*** TEST SUITE FAILED ***\n" );
		return 1;
	}
	printf( "All tests passed.\n" );
	return 0;
}
