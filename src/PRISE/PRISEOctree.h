//////////////////////////////////////////////////////////////////////
//
//  PRISEOctree.h - A special octree for PRISE, basically each
//    octant node contains CPU information along with regular
//    octant node information
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 13, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PRISE_OCTREE_
#define PRISE_OCTREE_

#include <deque>
#include <algorithm>
#include "../Utilities/Reference.h"
#include "../Utilities/MemoryBuffer.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/ISerializable.h"
#include "../Utilities/Time.h"
#include "../Octree.h"

// These three functions need to specialized for EVERY type we want to use
// this octree with!
template< class T >
static void RayElementIntersection( RayIntersectionGeometric& ri, const T elem, const bool bHitFrontFaces, const bool bHitBackFaces );

template< class T >
static bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const T elem, const bool bHitFrontFaces, const bool bHitBackFaces );

template< class T >
static bool ElementBoxIntersection( const T elem, const Vector3D vLowerLeft, const Vector3D vUpperRight );

template< class Element >
class PRISEOctree : public Implementation::Reference
{
protected:

	#include "PRISEOctreeNode.h"

	PRISEOctreeNode	root;			// Root node of the tree
	Point3D			ll;
	Point3D			ur;
	unsigned int	maxPerNode;
	IMemoryBuffer*	traversalBuf;	// A buffer that describes how the tree was traversed

	virtual ~PRISEOctree( )
	{
		if( traversalBuf ) {
			traversalBuf->RemoveRef();
			traversalBuf = 0;
		}
	}

public:
	// Must define the tree size, in full 3D space upon creation, I'll write a cooler
	// octree than resize itself and everything later, for now this is more practical
	PRISEOctree( Vector3D vLowerLeft, Vector3D vUpperRight, const unsigned int max_elems_in_one_node ) :
	  ll( Point3D( vLowerLeft.x, vLowerLeft.y, vLowerLeft.z ) ),
	  ur( Point3D( vUpperRight.x, vUpperRight.y, vUpperRight.z ) ),
	  maxPerNode( max_elems_in_one_node ), 
	  traversalBuf( 0 )
	{
		GlobalLog()->PrintEx( eLog_Info, TYPICAL_PRIORITY, "PRISEOctree:: Overall BBox LL(%Lf,%Lf,%Lf) UR(%Lf,%Lf,%Lf)", vLowerLeft.x, vLowerLeft.y, vLowerLeft.z, vUpperRight.x, vUpperRight.y, vUpperRight.z );
		traversalBuf = new Implementation::MemoryBuffer( 1024 );
		GlobalLog()->PrintNew( traversalBuf, __FILE__, __LINE__, "traversal buffer" );
	};

	void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		traversalBuf->seek( IBuffer::START, 0 );
		// Pass the request to the root
		BOX_HIT	h;
		root.IntersectRayBB( ll, ur, 99, ri.ray, h );
		ri.custom = 0;

		if( h.bHit ) {
			traversalBuf->setChar( 1 );
			root.IntersectRay( ri, bHitFrontFaces, bHitBackFaces, ll, ur, 99, traversalBuf, 0 );
		} else {
			traversalBuf->setChar( 0 );
		}
	}

	void IntersectRayImcomplete( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, IMemoryBuffer* mb ) const
	{
		traversalBuf->seek( IBuffer::START, 0 );
		// Pass the request to the root
		BOX_HIT	h;
		ri.custom = 0;
		mb->seek( IBuffer::START, 0 );

		if( mb->getChar() == 1 ) {		// otherwise what are we doing here ?
			traversalBuf->setChar( 1 );
			root.IntersectRayImcomplete( ri, bHitFrontFaces, bHitBackFaces, ll, ur, 99, traversalBuf, 0, mb );
		}
	}

	bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
	{
		// Pass the request to the root
		BOX_HIT	h;
		root.IntersectRayBB( ll, ur, 99, ray, h );

		if( h.bHit && h.dRange < dHowFar  ) {
			return root.IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces, ll, ur, 99 );
		}

		return false;
	}

	// All elements must be added at the same time.  Again a more robust Octree system 
	// could and probably should be written, but thats later
	bool AddElements( const std::vector<Element>& elements, const unsigned char max_recursion_level, const unsigned int maxPerNodeLevel2, const unsigned char maxRecursionLevel2 )
	{
		GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, "PRISEOctree:: Generating tree for %d elements, Max Recursion: %d, Max Elements/node: %d", elements.size(), max_recursion_level, maxPerNode );
		Timer	t;
		t.start();
		bool	bRet = root.AddElements( elements, maxPerNode, ll, ur, 99, max_recursion_level, maxPerNodeLevel2, maxRecursionLevel2 );
		t.stop();
		GlobalLog()->PrintEx( eLog_Info, TYPICAL_PRIORITY, "PRISEOctree:: Time to generate Octree %d seconds, %d ms", t.getInterval()/1000, t.getInterval()%1000 );
		return bRet;
	}

	void DumpStatistics( const LOG_ENUM e, const unsigned int p ) const
	{
		// Dumps to the global log
		GlobalLog()->Print( e, p, "PRISEOctree output: " );
		
		// Call the root node to dump, do this recursively
		unsigned int total_nodes = root.DumpStatistics( e, p, maxPerNode );
		GlobalLog()->PrintEx( e, p, "Total nodes in tree: %u", total_nodes );
	}

	void GetBBox( Point3D& ll_, Point3D& ur_ ) const
	{
		ll_ = ll;
		ur_ = ur;
	}

	void Serialize( IWriteBuffer& buffer ) const
	{
		// write the ll, ur and max per node
		buffer.ResizeForMore( sizeof( Point3D ) * 2 + sizeof( unsigned int ) );
		buffer.setDouble( ll.x );
		buffer.setDouble( ll.y );
		buffer.setDouble( ll.z );
		buffer.setDouble( ur.x );
		buffer.setDouble( ur.y );
		buffer.setDouble( ur.z );

		buffer.setUInt( maxPerNode );

		// Serialize the root node, should cascade from there
		root.Serialize( buffer );
	}

	void SerializeForCPU( IWriteBuffer& buffer, int cpu ) const
	{
		// write the ll, ur and max per node
		buffer.ResizeForMore( sizeof( Point3D ) * 2 + sizeof( unsigned int ) );
		buffer.setDouble( ll.x );
		buffer.setDouble( ll.y );
		buffer.setDouble( ll.z );
		buffer.setDouble( ur.x );
		buffer.setDouble( ur.y );
		buffer.setDouble( ur.z );

		buffer.setUInt( maxPerNode );

		// Serialize the root node, should cascade from there
		root.SerializeForCPU( buffer, cpu );
	}

	int CPUFromCallStack( unsigned int nCallStack ) const
	{
		std::deque<char>	call_stack;
		while( nCallStack > 0 ) {
			call_stack.push_front( nCallStack % 10 );
			nCallStack /= 10;
		}

		return root.CPUFromCallStack( call_stack );
	}

	void Deserialize( IReadBuffer& buffer) 
	{
		// read the ll, ur and max per node
		ll.x = buffer.getDouble();
		ll.y = buffer.getDouble();
		ll.z = buffer.getDouble();
		ur.x = buffer.getDouble();
		ur.y = buffer.getDouble();
		ur.z = buffer.getDouble();

		maxPerNode = buffer.getUInt();

		// Deserialize the root node, should cascade from there
		root.Deserialize( buffer );
	}

	void SegmentForCPUS( const unsigned int num_cpus )
	{
		// We need to go through and "color" the octree 
		// so that nodes are marked as to what CPU they belong to
		unsigned int *polygons_to_cpu = new unsigned int[num_cpus];
		for( int i=0; i<num_cpus; i++ ) {
			polygons_to_cpu[i] = 0;
		}

		root.PrecomputePolyCount();
		root.SimpleSegmentForCPUS( num_cpus, polygons_to_cpu );

		delete [] polygons_to_cpu;
	}
};

#endif

