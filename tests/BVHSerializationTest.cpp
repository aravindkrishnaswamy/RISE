//////////////////////////////////////////////////////////////////////
//
//  BVHSerializationTest.cpp - Round-trip BVH serialize / deserialize
//  test, verifying intersection equivalence.
//
//  This is the regression gate for the v3+ `.risemesh` BVH cache
//  format.  It catches:
//  1. Format-byte mismatches between Serialize() and Deserialize()
//     (header / version / node-count / prim-index ordering).
//  2. Drift between BVH2 cache content and the post-load
//     BuildFastFilter() / BuildBVH4() rederivation — both are run
//     by Deserialize, so a stale post-load hook would fail the
//     intersection-equivalence check.
//  3. Silent corruption of overallBox or per-node bbox bytes during
//     write/read.
//
//  Pattern follows tests/BVHBuilderTest.cpp's TestPrim/TestProc
//  fixture so the assertions are decoupled from the production mesh
//  classes.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

#include "../src/Library/Acceleration/BVH.h"
#include "../src/Library/Acceleration/AccelerationConfig.h"
#include "../src/Library/Intersection/RayPrimitiveIntersections.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	struct TestPrim
	{
		unsigned int  id;
		BoundingBox   bbox;
		TestPrim() : id(0), bbox(Point3(0,0,0), Point3(0,0,0)) {}
		TestPrim( unsigned int id_, const BoundingBox& b ) : id(id_), bbox(b) {}
	};

	class TestProc :
		public virtual TreeElementProcessor<TestPrim>,
		public virtual Reference
	{
	public:
		virtual ~TestProc() {}

		void RayElementIntersection( RayIntersectionGeometric& ri, const TestPrim elem, const bool, const bool ) const
		{
			BOX_HIT h;
			RayBoxIntersection( ri.ray, h, elem.bbox.ll, elem.bbox.ur );
			if( h.bHit && h.dRange >= NEARZERO && h.dRange < ri.range ) {
				ri.bHit          = true;
				ri.range         = h.dRange;
				ri.ptIntersection = ri.ray.PointAtLength( h.dRange );
				ri.ptCoord       = Point2( static_cast<Scalar>(elem.id), 0 );
			}
		}

		void RayElementIntersection( RayIntersection& ri, const TestPrim elem, const bool a, const bool b, const bool ) const
		{
			RayElementIntersection( ri.geometric, elem, a, b );
		}

		bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const TestPrim elem, const bool, const bool ) const
		{
			BOX_HIT h;
			RayBoxIntersection( ray, h, elem.bbox.ll, elem.bbox.ur );
			return h.bHit && h.dRange >= NEARZERO && h.dRange <= dHowFar;
		}

		BoundingBox GetElementBoundingBox( const TestPrim elem ) const
		{
			return elem.bbox;
		}

		bool ElementBoxIntersection( const TestPrim elem, const BoundingBox& bbox ) const
		{
			return elem.bbox.DoIntersect( bbox );
		}

		char WhichSideofPlaneIsElement( const TestPrim elem, const Plane& plane ) const
		{
			return GeometricUtilities::WhichSideOfPlane( plane, elem.bbox );
		}

		void SerializeElement( IWriteBuffer&, const TestPrim ) const {}
		void DeserializeElement( IReadBuffer&, TestPrim& ) const {}
	};

	int  failures   = 0;
	int  totalChecks = 0;

	#define EXPECT(cond, msg) do {                                          \
		++totalChecks;                                                      \
		if( !(cond) ) { ++failures;                                         \
			std::cerr << "FAIL " << __FILE__ << ":" << __LINE__             \
			          << "  " << msg << "\n"; }                             \
	} while(0)

	AccelerationConfig MkCfg( unsigned int leafSize )
	{
		AccelerationConfig c;
		c.maxLeafSize         = leafSize;
		c.binCount            = 32;
		c.sahTraversalCost    = 1.0;
		c.sahIntersectionCost = 1.0;
		c.doubleSided         = false;
		return c;
	}

	BoundingBox WorldBox( const std::vector<TestPrim>& prims )
	{
		BoundingBox b( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
		               Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY ) );
		for( const auto& p : prims ) {
			b.Include( p.bbox.ll );
			b.Include( p.bbox.ur );
		}
		return b;
	}

	std::vector<TestPrim> MakeRandomPrims( unsigned int N, unsigned int seed )
	{
		std::mt19937                          rng( seed );
		std::uniform_real_distribution<float> u( -1.0f, 1.0f );
		std::vector<TestPrim> prims;
		prims.reserve( N );
		for( unsigned int i = 0; i < N; ++i ) {
			Scalar cx = u(rng), cy = u(rng), cz = 5.0 + u(rng) * 0.5;
			Scalar h  = 0.05;
			BoundingBox b( Point3( cx-h, cy-h, cz-h ), Point3( cx+h, cy+h, cz+h ) );
			prims.push_back( TestPrim( i, b ) );
		}
		return prims;
	}

	//
	// Test 1: empty-BVH round trip.  Edge case — a BVH built from zero
	// prims serialises as a header + zero nodes + zero prims, and must
	// deserialise back to the same empty state.  Production catches
	// this when an empty mesh is loaded; without test coverage, header
	// drift could go unnoticed.
	//
	void TestEmptyRoundTrip()
	{
		std::cerr << "TestEmptyRoundTrip...\n";

		std::vector<TestPrim> prims;
		TestProc* proc = new TestProc(); proc->addref();
		BoundingBox b( Point3(0,0,0), Point3(1,1,1) );

		BVH<TestPrim>* bvhA = new BVH<TestPrim>( *proc, prims, b, MkCfg(4) );
		EXPECT( bvhA->numNodes() == 0, "empty bvh has zero nodes" );

		MemoryBuffer* buf = new MemoryBuffer();
		bvhA->Serialize( *buf, []( const TestPrim& p ) { return p.id; } );
		buf->seek( IBuffer::START, 0 );

		// Round-trip: build empty shell, deserialize.
		std::vector<TestPrim> empty;
		BVH<TestPrim>* bvhB = new BVH<TestPrim>( *proc, empty, b, MkCfg(4) );
		const bool ok = bvhB->Deserialize( *buf, 0,
			[]( unsigned int ) -> TestPrim { return TestPrim(); } );
		EXPECT( ok, "Deserialize on empty stream succeeds" );
		EXPECT( bvhB->numNodes() == 0, "deserialized empty bvh has zero nodes" );
		EXPECT( bvhB->numPrims() == 0, "deserialized empty bvh has zero prims" );

		bvhA->release();
		bvhB->release();
		safe_release( buf );
		proc->release();
	}

	//
	// Test 2: single-prim round trip.  Smallest non-trivial input — a
	// single-leaf root tree.  Validates the serialise / deserialise
	// header path on a tree shape that's distinct from the typical
	// recursive-internals case.
	//
	void TestSinglePrimRoundTrip()
	{
		std::cerr << "TestSinglePrimRoundTrip...\n";

		// id == position-in-vector: that's the contract Serialize / Deserialize
		// expect.  primIdxFn returns the prim's index in the caller's
		// authoritative vector; primAt resolves that index back to a prim.
		// We mirror the production pattern from TriangleMeshGeometryIndexed,
		// where indices are pointer-arithmetic against ptr_polygons[0].
		std::vector<TestPrim> prims = {
			TestPrim( 0, BoundingBox( Point3(-1,-1,5), Point3(1,1,6) ) )
		};
		TestProc* proc = new TestProc(); proc->addref();

		BVH<TestPrim>* bvhA = new BVH<TestPrim>( *proc, prims, WorldBox(prims), MkCfg(4) );
		EXPECT( bvhA->numNodes() == 1, "single-prim bvh has 1 node" );
		EXPECT( bvhA->numPrims() == 1, "single-prim bvh has 1 prim" );

		MemoryBuffer* buf = new MemoryBuffer();
		bvhA->Serialize( *buf, []( const TestPrim& p ) { return p.id; } );
		buf->seek( IBuffer::START, 0 );

		std::vector<TestPrim> empty;
		BVH<TestPrim>* bvhB = new BVH<TestPrim>( *proc, empty,
			BoundingBox( Point3(0,0,0), Point3(1,1,1) ), MkCfg(4) );
		const bool ok = bvhB->Deserialize( *buf, (uint32_t)prims.size(),
			[&prims]( unsigned int idx ) -> TestPrim {
				return idx < prims.size() ? prims[idx] : TestPrim();
			} );
		EXPECT( ok, "Deserialize on single-prim stream succeeds" );
		EXPECT( bvhB->numNodes() == 1, "deserialized has 1 node" );
		EXPECT( bvhB->numPrims() == 1, "deserialized has 1 prim" );

		// Intersection equivalence: same ray should produce same result.
		Ray ray; ray.origin = Point3(0,0,0); ray.SetDir( Vector3(0,0,1) );
		const bool hitA = bvhA->IntersectRay_IntersectionOnly( ray, 100.0, true, true );
		const bool hitB = bvhB->IntersectRay_IntersectionOnly( ray, 100.0, true, true );
		EXPECT( hitA && hitB, "both BVHs hit the prim" );

		bvhA->release();
		bvhB->release();
		safe_release( buf );
		proc->release();
	}

	//
	// Test 3: random-N round trip with intersection equivalence.  The
	// real regression gate — drives random rays through both the
	// in-memory and the round-tripped BVH and checks every (id, range)
	// pair matches.  If Serialize / Deserialize drifts (e.g., a node-
	// layout change, version bump, or post-load hook bug), this fails
	// loudly.
	//
	void TestRandomRoundTripIntersectionEquivalence( unsigned int N, unsigned int R, unsigned int leafSize )
	{
		std::cerr << "TestRandomRoundTripIntersectionEquivalence N=" << N << " R=" << R << " leaf=" << leafSize << "...\n";

		std::vector<TestPrim> prims = MakeRandomPrims( N, 7777 + N );
		TestProc* proc = new TestProc(); proc->addref();

		BVH<TestPrim>* bvhA = new BVH<TestPrim>( *proc, prims, WorldBox(prims), MkCfg(leafSize) );

		MemoryBuffer* buf = new MemoryBuffer();
		bvhA->Serialize( *buf, []( const TestPrim& p ) { return p.id; } );
		buf->seek( IBuffer::START, 0 );

		std::vector<TestPrim> empty;
		BVH<TestPrim>* bvhB = new BVH<TestPrim>( *proc, empty,
			BoundingBox( Point3(0,0,0), Point3(1,1,1) ), MkCfg(leafSize) );
		const bool ok = bvhB->Deserialize( *buf, (uint32_t)prims.size(),
			[&prims]( unsigned int idx ) -> TestPrim {
				return idx < prims.size() ? prims[idx] : TestPrim();
			} );
		EXPECT( ok, "Deserialize on random-N stream succeeds" );
		EXPECT( bvhA->numNodes() == bvhB->numNodes(), "node count matches across round-trip" );
		EXPECT( bvhA->numPrims() == bvhB->numPrims(), "prim count matches across round-trip" );

		std::mt19937                          rng( 9999 );
		std::uniform_real_distribution<float> u( -1.0f, 1.0f );

		unsigned int matched = 0, divergent = 0;
		for( unsigned int r = 0; r < R; ++r ) {
			Ray ray;
			ray.origin = Point3( u(rng) * 0.5, u(rng) * 0.5, 0.0 );
			Vector3 d( u(rng) * 0.5, u(rng) * 0.5, 1.0 );
			d = Vector3Ops::Normalize( d );
			ray.SetDir( d );

			RayIntersectionGeometric riA( ray, nullRasterizerState );
			bvhA->IntersectRay( riA, true, true );
			RayIntersectionGeometric riB( ray, nullRasterizerState );
			bvhB->IntersectRay( riB, true, true );

			if( riA.bHit != riB.bHit ) {
				++divergent;
				continue;
			}
			if( !riA.bHit ) {
				++matched;
				continue;
			}
			// Both hit — same id, same range (within 1e-9 for fp64 noise).
			if( riA.ptCoord.x == riB.ptCoord.x &&
			    std::fabs( riA.range - riB.range ) < 1e-9 ) {
				++matched;
			} else {
				++divergent;
			}
		}

		std::cerr << "  matched=" << matched << " divergent=" << divergent
		          << "  (nodes=" << bvhA->numNodes() << ")\n";
		EXPECT( divergent == 0, "round-tripped BVH must produce identical hits to original" );

		bvhA->release();
		bvhB->release();
		safe_release( buf );
		proc->release();
	}

	//
	// Test 4: format header guard.  If somebody bumps the on-disk
	// version number without a backward-compat path, Deserialize must
	// return false (not silently produce a corrupted tree).
	//
	void TestVersionMismatchRejected()
	{
		std::cerr << "TestVersionMismatchRejected...\n";

		std::vector<TestPrim> prims = MakeRandomPrims( 32, 1234 );
		TestProc* proc = new TestProc(); proc->addref();
		BVH<TestPrim>* bvhA = new BVH<TestPrim>( *proc, prims, WorldBox(prims), MkCfg(4) );

		MemoryBuffer* buf = new MemoryBuffer();
		bvhA->Serialize( *buf, []( const TestPrim& p ) { return p.id; } );

		// Hand-corrupt the version byte to force a failure path.
		// Layout: "BVH2"(4) + version(4) + ... ; flip a bit in version.
		char* raw = buf->Pointer();
		EXPECT( raw != nullptr, "MemoryBuffer exposes raw pointer" );
		raw[4] = (char)0xFF;   // version = 0xFFFFFFFF — will reject
		raw[5] = (char)0xFF;
		raw[6] = (char)0xFF;
		raw[7] = (char)0xFF;

		buf->seek( IBuffer::START, 0 );
		std::vector<TestPrim> empty;
		BVH<TestPrim>* bvhB = new BVH<TestPrim>( *proc, empty,
			BoundingBox( Point3(0,0,0), Point3(1,1,1) ), MkCfg(4) );
		const bool ok = bvhB->Deserialize( *buf, (uint32_t)prims.size(),
			[&prims]( unsigned int idx ) -> TestPrim {
				return idx < prims.size() ? prims[idx] : TestPrim();
			} );
		EXPECT( !ok, "Deserialize rejects a bogus version" );

		bvhA->release();
		bvhB->release();
		safe_release( buf );
		proc->release();
	}

	//
	// Test 5: magic-bytes guard.  Wrong magic must be rejected so a
	// stream from a different acceleration structure (legacy BSP, etc.)
	// can't be silently consumed.
	//
	void TestBadMagicRejected()
	{
		std::cerr << "TestBadMagicRejected...\n";

		MemoryBuffer* buf = new MemoryBuffer();
		buf->setBytes( "XXXX", 4 );
		buf->setUInt( 1u );
		buf->setUInt( 0u );
		buf->setUInt( 0u );
		buf->setDouble( 0.0 ); buf->setDouble( 0.0 ); buf->setDouble( 0.0 );
		buf->setDouble( 1.0 ); buf->setDouble( 1.0 ); buf->setDouble( 1.0 );
		buf->seek( IBuffer::START, 0 );

		TestProc* proc = new TestProc(); proc->addref();
		std::vector<TestPrim> empty;
		BVH<TestPrim>* bvh = new BVH<TestPrim>( *proc, empty,
			BoundingBox( Point3(0,0,0), Point3(1,1,1) ), MkCfg(4) );
		const bool ok = bvh->Deserialize( *buf, 0u,
			[]( unsigned int ) -> TestPrim { return TestPrim(); } );
		EXPECT( !ok, "Deserialize rejects bogus magic bytes" );

		bvh->release();
		safe_release( buf );
		proc->release();
	}
}

int main()
{
	TestEmptyRoundTrip();
	TestSinglePrimRoundTrip();
	TestRandomRoundTripIntersectionEquivalence(  100,  100, 4 );
	TestRandomRoundTripIntersectionEquivalence( 1000, 1000, 4 );
	TestRandomRoundTripIntersectionEquivalence( 1000, 1000, 10 );
	TestVersionMismatchRejected();
	TestBadMagicRejected();

	std::cerr << "\nBVHSerializationTest: " << (totalChecks - failures) << "/"
	          << totalChecks << " checks passed, " << failures << " failures.\n";
	return failures == 0 ? 0 : 1;
}
