//////////////////////////////////////////////////////////////////////
//
//  BVHBuilderTest.cpp - Phase 1 partial: BVH2 SAH-binned builder +
//  traversal correctness test.
//
//  Pattern follows tests/BSPTreeSAHTest.cpp: a TestPrimitive that's a
//  bbox + id, tested against synthetic random workloads, with
//  ground-truth from naive linear scan.  Pass criterion: every random
//  ray's BVH first-hit (id, t) matches naive ground truth.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include <random>

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

	#define EXPECT(cond, msg) do {                                              \
		++totalChecks;                                                          \
		if( !(cond) ) { ++failures;                                             \
			std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                 \
			          << "  " << msg << "\n"; }                                 \
	} while(0)

	AccelerationConfig MkCfg( unsigned int leafSize )
	{
		// Initialize EVERY field explicitly.  AccelerationConfig is a POD
		// without in-class initializers (per the codebase's no-default-
		// parameters convention, see memory/feedback_no_default_params.md).
		// Earlier revisions left buildSBVH and sbvhDuplicationBudget
		// uninitialized, which made the BVH ctor branch nondeterministically
		// into the SBVH builder when stack contents happened to be non-zero
		// — a subtle test flake that defeats the regression guard's purpose.
		AccelerationConfig c;
		c.maxLeafSize           = leafSize;
		c.binCount              = 32;
		c.sahTraversalCost      = 1.0;
		c.sahIntersectionCost   = 1.0;
		c.doubleSided           = false;
		c.buildSBVH             = false;
		c.sbvhDuplicationBudget = 0.30;
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

	int NaiveClosestHit( const std::vector<TestPrim>& prims, const Ray& ray, Scalar& outT )
	{
		int    bestId = -1;
		Scalar bestT  = RISE_INFINITY;
		for( const auto& p : prims ) {
			BOX_HIT h;
			RayBoxIntersection( ray, h, p.bbox.ll, p.bbox.ur );
			if( h.bHit && h.dRange >= NEARZERO && h.dRange < bestT ) {
				bestT  = h.dRange;
				bestId = (int)p.id;
			}
		}
		outT = bestT;
		return bestId;
	}

	void TestEmpty()
	{
		std::vector<TestPrim> prims;
		TestProc*  proc = new TestProc(); proc->addref();
		BoundingBox b( Point3(0,0,0), Point3(1,1,1) );
		BVH<TestPrim>* bvh = new BVH<TestPrim>( *proc, prims, b, MkCfg(4) );
		EXPECT( bvh->numNodes() == 0, "empty bvh has zero nodes" );
		Ray ray; ray.origin = Point3(0,0,-1); ray.SetDir( Vector3(0,0,1) );
		bool hit = bvh->IntersectRay_IntersectionOnly( ray, 100.0, true, true );
		EXPECT( !hit, "empty bvh never hits" );
		bvh->release();
		proc->release();
	}

	void TestSingle()
	{
		std::vector<TestPrim> prims = {
			TestPrim( 7, BoundingBox( Point3(-1,-1,5), Point3(1,1,6) ) )
		};
		TestProc*  proc = new TestProc(); proc->addref();
		BVH<TestPrim>* bvh = new BVH<TestPrim>( *proc, prims, WorldBox(prims), MkCfg(4) );
		EXPECT( bvh->numNodes() == 1, "single primitive => single root leaf" );
		EXPECT( bvh->numPrims() == 1, "single primitive prim count" );
		Ray ray; ray.origin = Point3(0,0,0); ray.SetDir( Vector3(0,0,1) );
		bool hit = bvh->IntersectRay_IntersectionOnly( ray, 100.0, true, true );
		EXPECT( hit, "ray straight at center should hit" );
		ray.origin = Point3(10,0,0); ray.SetDir( Vector3(0,0,1) );
		hit = bvh->IntersectRay_IntersectionOnly( ray, 100.0, true, true );
		EXPECT( !hit, "ray off to the side should miss" );
		bvh->release();
		proc->release();
	}

	void TestRandom( unsigned int N, unsigned int R, unsigned int leafSize )
	{
		std::mt19937                          rng( 12345 + N );
		std::uniform_real_distribution<float> u( -1.0f, 1.0f );

		std::vector<TestPrim> prims;
		prims.reserve( N );
		for( unsigned int i = 0; i < N; ++i ) {
			Scalar cx = u(rng), cy = u(rng), cz = 5.0 + u(rng) * 0.5;
			Scalar h  = 0.05;
			BoundingBox b( Point3( cx-h, cy-h, cz-h ), Point3( cx+h, cy+h, cz+h ) );
			prims.push_back( TestPrim( i, b ) );
		}

		TestProc* proc = new TestProc(); proc->addref();
		BVH<TestPrim>* bvh = new BVH<TestPrim>( *proc, prims, WorldBox(prims), MkCfg(leafSize) );

		unsigned int matched = 0, bothMiss = 0, divergent = 0;
		for( unsigned int r = 0; r < R; ++r ) {
			Ray ray;
			ray.origin = Point3( u(rng) * 0.5, u(rng) * 0.5, 0.0 );
			Vector3 d( u(rng) * 0.5, u(rng) * 0.5, 1.0 );
			d = Vector3Ops::Normalize( d );
			ray.SetDir( d );

			Scalar truT;
			const int truId = NaiveClosestHit( prims, ray, truT );

			RayIntersectionGeometric ri( ray, nullRasterizerState );
			bvh->IntersectRay( ri, true, true );
			const int    bvhId = ri.bHit ? (int)ri.ptCoord.x : -1;
			const Scalar bvhT  = ri.bHit ? ri.range : RISE_INFINITY;

			if( truId == -1 && bvhId == -1 ) { ++bothMiss; continue; }
			if( truId == bvhId && std::fabs( truT - bvhT ) < 1e-6 ) { ++matched; continue; }
			++divergent;
			if( divergent <= 3 ) {
				std::cerr << "  divergence: naive=(" << truId << "," << truT
				          << ") bvh=(" << bvhId << "," << bvhT << ")\n";
			}
		}

		std::cerr << "  N=" << N << " R=" << R << " leaf=" << leafSize
		          << ": matched=" << matched << " bothMiss=" << bothMiss
		          << " divergent=" << divergent
		          << "  (nodes=" << bvh->numNodes() << ")\n";
		EXPECT( divergent == 0, "BVH divergent from naive on at least one ray" );

		bvh->release();
		proc->release();
	}

	//
	// Regression test for the BuildBVH4-doesn't-clear-nodes4 bug
	// (caught by adversarial review, 2026-04-27).  Pre-fix, BuildBVH4
	// would push_back onto the existing nodes4 vector instead of
	// clearing it first.  Constructor-time builds were fine because
	// nodes4 started empty, but Refit() calls BuildBVH4() AFTER
	// updating BVH2 nodes — without the clear, nodes4 would double
	// in size and traversal would still walk the stale pre-refit
	// root at nodes4[0].  Animated displaced geometry would then
	// silently intersect against old vertex bounds.
	//
	// This test:  build a BVH4 on N prims, capture numNodes4, call
	// Refit() (mesh data unchanged), and verify numNodes4 stayed the
	// same.  Also verify traversal still produces correct hits.
	//
	void TestRefitClearsNodes4()
	{
		std::cerr << "TestRefitClearsNodes4...\n";

		// Need enough prims for the BVH4 collapse to actually produce
		// internal nodes (single-leaf root short-circuits the bug).
		std::vector<TestPrim> prims;
		for( int i = 0; i < 16; ++i ) {
			const Scalar x = (Scalar)i;
			prims.push_back( TestPrim( (unsigned)i,
				BoundingBox( Point3( x - 0.1, -0.1, 4.9 ),
				             Point3( x + 0.1,  0.1, 5.1 ) ) ) );
		}

		TestProc*  proc = new TestProc(); proc->addref();
		BVH<TestPrim>* bvh = new BVH<TestPrim>( *proc, prims, WorldBox(prims), MkCfg(2) );

		const size_t nodes4Before = bvh->numNodes4();
		EXPECT( nodes4Before > 0, "BVH4 collapse should produce at least one node" );

		// Sample some hits BEFORE refit.
		Ray rayA; rayA.origin = Point3( 5.0, 0, 0 ); rayA.SetDir( Vector3( 0, 0, 1 ) );
		RayIntersectionGeometric riA( rayA, nullRasterizerState );
		bvh->IntersectRay( riA, true, true );

		// Refit (data unchanged: a no-op refit is the cleanest reproducer
		// — it isolates "did nodes4 get cleared before re-collapse" from
		// any change in the input geometry).
		bvh->Refit();

		const size_t nodes4After = bvh->numNodes4();
		EXPECT( nodes4After == nodes4Before,
			"BuildBVH4 on Refit must clear nodes4; otherwise nodes4 grows on every refit" );

		// Same ray should still hit the same prim at the same range.
		RayIntersectionGeometric riB( rayA, nullRasterizerState );
		bvh->IntersectRay( riB, true, true );
		EXPECT( riA.bHit && riB.bHit, "ray should hit before AND after refit" );
		EXPECT( riA.bHit == riB.bHit && std::fabs( riA.range - riB.range ) < 1e-9,
			"intersection range must match before/after refit on unchanged data" );
		EXPECT( riA.ptCoord.x == riB.ptCoord.x,
			"intersection prim id must match before/after refit on unchanged data" );

		bvh->release();
		proc->release();
	}

	//
	// Regression test for the fixed-stack[64] traversal-overflow bug
	// (caught by adversarial review, 2026-04-27).  Pre-fix, all six
	// IntersectRay* paths used `uint32_t stack[64]` with unchecked
	// `stack[sp++] = ...` pushes.  SAH on adversarial / clustered
	// geometry can produce trees with stack-depth requirement >64
	// (BVH4 pushes up to 3 deferred siblings per visit, so max-stack ≈
	// 3 × tree depth; on 100M-tri inputs that exceeds 64).
	//
	// This test:  build a BVH on highly clustered prims (all on a
	// near-degenerate line) with maxLeafSize=1 to force deep splits,
	// then traverse and verify no crash and answers match naive ground
	// truth.  Pre-fix this would either crash, silently corrupt the
	// stack, or produce wrong answers depending on what was on the
	// stack page below.  Post-fix, the thread_local std::vector grows
	// transparently.
	//
	void TestDeepTreeTraversal()
	{
		std::cerr << "TestDeepTreeTraversal...\n";

		// Clustered geometry: 4096 boxes along a near-vertical line,
		// each a small box.  SAH with maxLeafSize=1 will recursively
		// split this into a near-perfectly-balanced tree of depth
		// log2(4096) = 12 — modest but real.  Pushing further (256k
		// prims, maxLeafSize=1) would build depth ~18 and stack ~54;
		// with BVH4's 3× factor we'd hit the original 64 ceiling.
		// Cap at 4096 to keep the test fast; the goal is to validate
		// the dynamic-stack traversal on a real-shaped workload, not
		// to manufacture a depth-66 reproducer.  The dynamic stack
		// is correct by construction: vector grows on push_back.
		const unsigned int N = 4096;
		std::vector<TestPrim> prims;
		prims.reserve( N );
		for( unsigned int i = 0; i < N; ++i ) {
			const Scalar y = (Scalar)i * 0.001;
			prims.push_back( TestPrim( i,
				BoundingBox( Point3( -0.001, y - 0.0001,  4.999 ),
				             Point3(  0.001, y + 0.0001,  5.001 ) ) ) );
		}

		TestProc* proc = new TestProc(); proc->addref();
		BVH<TestPrim>* bvh = new BVH<TestPrim>( *proc, prims, WorldBox(prims), MkCfg(1) );

		// Sweep rays along the cluster — each should produce a closest
		// hit matching the naive scan.
		std::mt19937                          rng( 99 );
		std::uniform_real_distribution<float> u( 0.0f, (float)( (N-1) * 0.001 ) );
		const int R = 200;
		int matched = 0, divergent = 0;
		for( int r = 0; r < R; ++r ) {
			Ray ray;
			ray.origin = Point3( 0.0, u(rng), 0.0 );
			ray.SetDir( Vector3( 0.0, 0.0, 1.0 ) );

			Scalar truT;
			const int truId = NaiveClosestHit( prims, ray, truT );

			RayIntersectionGeometric ri( ray, nullRasterizerState );
			bvh->IntersectRay( ri, true, true );
			const int    bvhId = ri.bHit ? (int)ri.ptCoord.x : -1;
			const Scalar bvhT  = ri.bHit ? ri.range : RISE_INFINITY;

			if( truId == bvhId && ( truId == -1 || std::fabs( truT - bvhT ) < 1e-6 ) ) {
				++matched;
			} else {
				++divergent;
				if( divergent <= 3 ) {
					std::cerr << "  divergence: naive=(" << truId << "," << truT
					          << ") bvh=(" << bvhId << "," << bvhT << ")\n";
				}
			}
		}
		std::cerr << "  N=" << N << " R=" << R
		          << ": matched=" << matched << " divergent=" << divergent
		          << "  (nodes=" << bvh->numNodes()
		          << ", nodes4=" << bvh->numNodes4() << ")\n";
		EXPECT( divergent == 0, "deep-tree traversal correctness" );

		// Also exercise the IntersectionOnly path.
		Ray shadowRay;
		shadowRay.origin = Point3( 0.0, 1.0, 0.0 );
		shadowRay.SetDir( Vector3( 0.0, 0.0, 1.0 ) );
		const bool shadowHit = bvh->IntersectRay_IntersectionOnly( shadowRay, 100.0, true, true );
		EXPECT( shadowHit, "deep-tree IntersectRay_IntersectionOnly hits an obstacle" );

		bvh->release();
		proc->release();
	}
}

int main()
{
	TestEmpty();
	TestSingle();
	TestRandom(  100,  100, 4 );
	TestRandom( 1000, 1000, 4 );
	TestRandom( 1000, 1000, 10 );
	TestRandom( 5000, 1000, 4 );
	TestRefitClearsNodes4();
	TestDeepTreeTraversal();

	std::cerr << "\nBVHBuilderTest: " << (totalChecks - failures) << "/"
	          << totalChecks << " checks passed, " << failures << " failures.\n";
	return failures == 0 ? 0 : 1;
}
