#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "../src/Library/BSPTreeSAH.h"
#include "../src/Library/Intersection/RayPrimitiveIntersections.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	struct TestPrimitive
	{
		unsigned int	id;
		BoundingBox		bbox;

		TestPrimitive() :
			id( 0 ),
			bbox( Point3(0,0,0), Point3(0,0,0) )
		{}

		TestPrimitive( const unsigned int id_, const BoundingBox& bbox_ ) :
			id( id_ ),
			bbox( bbox_ )
		{}
	};

	class TestProcessor :
		public virtual TreeElementProcessor<TestPrimitive>,
		public virtual Reference
	{
	public:
		virtual ~TestProcessor(){}
		void RayElementIntersection( RayIntersectionGeometric& ri, const TestPrimitive elem, const bool, const bool ) const
		{
			BOX_HIT h;
			RayBoxIntersection( ri.ray, h, elem.bbox.ll, elem.bbox.ur );

			if( h.bHit && h.dRange >= NEARZERO ) {
				ri.bHit = true;
				ri.range = h.dRange;
				ri.ptIntersection = ri.ray.PointAtLength( h.dRange );
				ri.ptCoord = Point2( static_cast<Scalar>(elem.id), 0 );
			}
		}

		void RayElementIntersection( RayIntersection& ri, const TestPrimitive elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool ) const
		{
			RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
		}

		bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const TestPrimitive elem, const bool, const bool ) const
		{
			BOX_HIT h;
			RayBoxIntersection( ray, h, elem.bbox.ll, elem.bbox.ur );
			return h.bHit && h.dRange >= NEARZERO && h.dRange <= dHowFar;
		}

		BoundingBox GetElementBoundingBox( const TestPrimitive elem ) const
		{
			return elem.bbox;
		}

		bool ElementBoxIntersection( const TestPrimitive elem, const BoundingBox& bbox ) const
		{
			return elem.bbox.DoIntersect( bbox );
		}

		char WhichSideofPlaneIsElement( const TestPrimitive elem, const Plane& plane ) const
		{
			return GeometricUtilities::WhichSideOfPlane( plane, elem.bbox );
		}

		void SerializeElement( IWriteBuffer& buffer, const TestPrimitive elem ) const
		{
			buffer.setUInt( elem.id );
			elem.bbox.Serialize( buffer );
		}

		void DeserializeElement( IReadBuffer& buffer, TestPrimitive& ret ) const
		{
			ret.id = buffer.getUInt();
			ret.bbox.Deserialize( buffer );
		}
	};

	bool IsClose( const Scalar a, const Scalar b, const Scalar epsilon = 1e-6 )
	{
		return std::fabs( a - b ) < epsilon;
	}

	TestPrimitive MakePrimitive(
		const unsigned int id,
		const Scalar minx,
		const Scalar maxx,
		const Scalar miny = 0,
		const Scalar maxy = 1,
		const Scalar minz = 0,
		const Scalar maxz = 1
		)
	{
		return TestPrimitive( id, BoundingBox( Point3(minx, miny, minz), Point3(maxx, maxy, maxz) ) );
	}

	BoundingBox ComputeOverallBBox( const std::vector<TestPrimitive>& primitives )
	{
		BoundingBox overall( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );
		std::vector<TestPrimitive>::const_iterator i, e;
		for( i=primitives.begin(), e=primitives.end(); i!=e; i++ ) {
			overall.Include( i->bbox );
		}
		return overall;
	}

	BSPTreeSAH<TestPrimitive>* BuildTree( const TestProcessor& processor, const std::vector<TestPrimitive>& primitives, const unsigned int maxPerNode = 1, const unsigned char maxDepth = 16 )
	{
		BSPTreeSAH<TestPrimitive>* pTree = new BSPTreeSAH<TestPrimitive>( processor, ComputeOverallBBox( primitives ), maxPerNode );
		pTree->AddElements( primitives, maxDepth );
		return pTree;
	}

	RayIntersectionGeometric TraceNaive( const TestProcessor& processor, const std::vector<TestPrimitive>& primitives, const Ray& ray )
	{
		RayIntersectionGeometric ri( ray, nullRasterizerState );
		std::vector<TestPrimitive>::const_iterator i, e;
		for( i=primitives.begin(), e=primitives.end(); i!=e; i++ ) {
			RayIntersectionGeometric cur( ray, nullRasterizerState );
			processor.RayElementIntersection( cur, *i, true, true );
			if( cur.bHit && cur.range < ri.range ) {
				ri = cur;
			}
		}
		return ri;
	}

	void AssertTreeMatchesNaive( const TestProcessor& processor, const std::vector<TestPrimitive>& primitives, BSPTreeSAH<TestPrimitive>& tree, const Ray& ray )
	{
		RayIntersectionGeometric expected = TraceNaive( processor, primitives, ray );
		RayIntersectionGeometric actual( ray, nullRasterizerState );
		tree.IntersectRay( actual, true, true );

		assert( actual.bHit == expected.bHit );
		if( actual.bHit ) {
			assert( IsClose( actual.range, expected.range ) );
			assert( IsClose( actual.ptCoord.x, expected.ptCoord.x ) );
		}

		RayIntersection full( ray, nullRasterizerState );
		tree.IntersectRay( full, true, true, false );
		assert( full.geometric.bHit == expected.bHit );
		if( full.geometric.bHit ) {
			assert( IsClose( full.geometric.range, expected.range ) );
			assert( IsClose( full.geometric.ptCoord.x, expected.ptCoord.x ) );
		}

		const bool expectedShadow = expected.bHit && expected.range <= 100.0;
		assert( tree.IntersectRay_IntersectionOnly( ray, 100.0, true, true ) == expectedShadow );
	}

	void TestRootSplitUsesSAH()
	{
		std::cout << "Testing BSPTreeSAH root split selection..." << std::endl;

		TestProcessor processor;
		std::vector<TestPrimitive> primitives;
		primitives.push_back( MakePrimitive( 1, 0.0, 1.0 ) );
		primitives.push_back( MakePrimitive( 2, 1.1, 2.0 ) );
		primitives.push_back( MakePrimitive( 3, 2.1, 3.0 ) );
		primitives.push_back( MakePrimitive( 4, 9.0, 10.0 ) );

		BSPTreeSAH<TestPrimitive>* pTree = BuildTree( processor, primitives );

		unsigned char axis = BSP_SAH_AXIS_INVALID;
		Scalar location = -1;
		assert( pTree->GetRootSplit( axis, location ) );
		assert( axis == BSP_SAH_AXIS_X );
		assert( IsClose( location, 3.0 ) );

		safe_release( pTree );

		std::cout << "BSPTreeSAH root split selection Passed!" << std::endl;
	}

	void TestFullyOverlappingBoxesStayLeaf()
	{
		std::cout << "Testing BSPTreeSAH leaf fallback..." << std::endl;

		TestProcessor processor;
		std::vector<TestPrimitive> primitives;
		primitives.push_back( MakePrimitive( 1, 0.0, 4.0 ) );
		primitives.push_back( MakePrimitive( 2, 0.0, 4.0 ) );

		BSPTreeSAH<TestPrimitive>* pTree = BuildTree( processor, primitives, 1, 8 );

		unsigned char axis = BSP_SAH_AXIS_INVALID;
		Scalar location = -1;
		assert( !pTree->GetRootSplit( axis, location ) );

		safe_release( pTree );

		std::cout << "BSPTreeSAH leaf fallback Passed!" << std::endl;
	}

	void TestIntersectionsMatchNaive()
	{
		std::cout << "Testing BSPTreeSAH intersections against naive traversal..." << std::endl;

		TestProcessor processor;
		std::vector<TestPrimitive> primitives;
		primitives.push_back( MakePrimitive( 1, 0.0, 1.0 ) );
		primitives.push_back( MakePrimitive( 2, 2.0, 4.0 ) );
		primitives.push_back( MakePrimitive( 3, 4.5, 6.5 ) );
		primitives.push_back( MakePrimitive( 4, 8.0, 9.0 ) );

		BSPTreeSAH<TestPrimitive>* pTree = BuildTree( processor, primitives );

		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(-1.0, 0.5, 0.5), Vector3(1,0,0) ) );
		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(1.5, 0.5, 0.5), Vector3(1,0,0) ) );
		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(4.0, 0.5, 0.5), Vector3(1,0,0) ) );
		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(7.0, 0.5, 0.5), Vector3(1,0,0) ) );
		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(10.0, 0.5, 0.5), Vector3(-1,0,0) ) );
		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(7.0, 2.0, 0.5), Vector3(1,0,0) ) );

		safe_release( pTree );

		std::cout << "BSPTreeSAH intersections against naive traversal Passed!" << std::endl;
	}

	void TestBinnedBuildMatchesNaive()
	{
		std::cout << "Testing BSPTreeSAH binned build path..." << std::endl;

		TestProcessor processor;
		std::vector<TestPrimitive> primitives;
		for( unsigned int i=0; i<200; i++ ) {
			const Scalar minx = Scalar(i) * 2.0;
			primitives.push_back( MakePrimitive( i+1, minx, minx+1.0 ) );
		}

		BSPTreeSAH<TestPrimitive>* pTree = BuildTree( processor, primitives, 1, 32 );

		unsigned char axis = BSP_SAH_AXIS_INVALID;
		Scalar location = -1;
		assert( pTree->GetRootSplit( axis, location ) );
		assert( axis == BSP_SAH_AXIS_X );

		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(0.5, 0.5, -1.0), Vector3(0, 0, 1) ) );
		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(120.5, 0.5, -1.0), Vector3(0, 0, 1) ) );
		AssertTreeMatchesNaive( processor, primitives, *pTree, Ray( Point3(398.5, 0.5, -1.0), Vector3(0, 0, 1) ) );

		safe_release( pTree );

		std::cout << "BSPTreeSAH binned build path Passed!" << std::endl;
	}

	void TestSerializationRoundTrip()
	{
		std::cout << "Testing BSPTreeSAH serialization..." << std::endl;

		TestProcessor processor;
		std::vector<TestPrimitive> primitives;
		primitives.push_back( MakePrimitive( 1, 0.0, 1.0 ) );
		primitives.push_back( MakePrimitive( 2, 1.1, 2.0 ) );
		primitives.push_back( MakePrimitive( 3, 2.1, 3.0 ) );
		primitives.push_back( MakePrimitive( 4, 9.0, 10.0 ) );

		BSPTreeSAH<TestPrimitive>* pOriginal = BuildTree( processor, primitives );
		MemoryBuffer* pBuffer = new MemoryBuffer( 256 );
		pOriginal->Serialize( *pBuffer );
		pBuffer->seek( IBuffer::START, 0 );

		BSPTreeSAH<TestPrimitive>* pReloaded = new BSPTreeSAH<TestPrimitive>( processor, BoundingBox( Point3(0,0,0), Point3(0,0,0) ), 1 );
		pReloaded->Deserialize( *pBuffer );

		unsigned char originalAxis = BSP_SAH_AXIS_INVALID;
		unsigned char reloadedAxis = BSP_SAH_AXIS_INVALID;
		Scalar originalLocation = -1;
		Scalar reloadedLocation = -1;

		assert( pOriginal->GetRootSplit( originalAxis, originalLocation ) );
		assert( pReloaded->GetRootSplit( reloadedAxis, reloadedLocation ) );
		assert( originalAxis == reloadedAxis );
		assert( IsClose( originalLocation, reloadedLocation ) );

		AssertTreeMatchesNaive( processor, primitives, *pReloaded, Ray( Point3(-1.0, 0.5, 0.5), Vector3(1,0,0) ) );
		AssertTreeMatchesNaive( processor, primitives, *pReloaded, Ray( Point3(8.5, 0.5, 0.5), Vector3(-1,0,0) ) );

		safe_release( pReloaded );
		safe_release( pBuffer );
		safe_release( pOriginal );

		std::cout << "BSPTreeSAH serialization Passed!" << std::endl;
	}
}

int main()
{
	TestRootSplitUsesSAH();
	TestFullyOverlappingBoxesStayLeaf();
	TestIntersectionsMatchNaive();
	TestBinnedBuildMatchesNaive();
	TestSerializationRoundTrip();

	std::cout << "All BSPTreeSAH tests passed!" << std::endl;
	return 0;
}
