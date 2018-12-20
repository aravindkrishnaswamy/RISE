//////////////////////////////////////////////////////////////////////
//
//  BSPTree.h - Definition of a templated BSPTree class.  This class
//  allows arbritary types to be stored in the BSPTree as long
//  as there is an intersection function for that type with boxes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 22, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BSPTree_
#define BSPTree_

#include "TreeElementProcessor.h"
#include "Utilities/Reference.h"
#include "Utilities/BoundingBox.h"
#include "Utilities/GeometricUtilities.h"
#include "Intersection/RayPrimitiveIntersections.h"
#include "Intersection/RayIntersection.h"
#include "Interfaces/ILog.h"
#include "Interfaces/ISerializable.h"
#include "Utilities/RTime.h"
#include "Utilities/stl_utils.h"
#include <vector>
#include <algorithm>

namespace RISE
{
	static const char BASE_TYPE_X = 2;
	static const char BASE_TYPE_Y = 4;
	static const char BASE_TYPE_Z = 8;

	static const char next_base_type[3] = {BASE_TYPE_Y, BASE_TYPE_Z, BASE_TYPE_X};

	static const Scalar		bsp_error_delta_box_size = 0.0001;

	template< class Element >
	class BSPTree : public Implementation::Reference
	{
	protected:

		typedef std::vector<Element>	ElementListType;

		#include "BSPTreeNode.h"

		BSPTreeNode		root;			// Root node of the tree
		BoundingBox		bbox;			// Bounding box of the tree
		unsigned int	maxPerNode;
		const TreeElementProcessor<Element>& ep;

		virtual ~BSPTree( ){}

	public:
		BSPTree( const TreeElementProcessor<Element>& ep_, const BoundingBox& bbox_, const unsigned int max_elems_in_one_node ) :
		bbox( bbox_ ),
		maxPerNode( max_elems_in_one_node ),
		ep( ep_ )
		{
			GlobalLog()->PrintEx( eLog_Info, "BSPTree:: Overall BBox LL(%lf,%lf,%lf) UR(%lf,%lf,%lf)", bbox.ll.x, bbox.ll.y, bbox.ll.z, bbox.ur.x, bbox.ur.y, bbox.ur.z );
		};

		void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces ) const
		{
			ri.bHit = false;
			ri.range = INFINITY;
			root.IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbox, 10 );
		}

		void IntersectRay( RayIntersection& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
		{
			ri.geometric.bHit = false;
			ri.geometric.range = INFINITY;
			root.IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbox, 10 );
		}

		bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
		{
			return root.IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbox, 10 );
		}

		// All elements must be added at the same time.  Again a more robust BSPTree system
		// could and probably should be written, but thats later
		void AddElements( const std::vector<Element>& elements, const unsigned char max_recursion_level )
		{
			GlobalLog()->PrintEx( eLog_Info, "BSPTree:: Generating tree for %d elements, Max Recursion: %d, Max Elements/node: %d", elements.size(), max_recursion_level, maxPerNode );
			Timer	t;
			t.start();

			std::vector<Element> empty;
			root.AddElements( ep, elements, empty, maxPerNode, bbox, 10, max_recursion_level );

			t.stop();
			GlobalLog()->PrintEx( eLog_Info, "BSPTree:: Time to generate BSPTree %d seconds, %d ms", t.getInterval()/1000, t.getInterval()%1000 );
		}

		void DumpStatistics( const LOG_ENUM e ) const
		{
			// Dumps to the global log
			GlobalLog()->Print( e, "BSPTree output: " );

			// Call the root node to dump, do this recursively
			unsigned int total_elems = 0;
			unsigned int total_nodes = root.DumpStatistics( e, maxPerNode, total_elems );
			GlobalLog()->PrintEx( e, "Total nodes in tree: %u", total_nodes );
			GlobalLog()->PrintEx( e, "Total elements in all leaf nodes: %u", total_elems );
		}

		BoundingBox GetBBox() const
		{
			return bbox;
		}

		void Serialize( IWriteBuffer& buffer ) const
		{
			// write the ll, ur and max per node
			bbox.Serialize( buffer );

			buffer.setUInt( maxPerNode );

			// Serialize the root node, should cascade from there
			root.Serialize( ep, buffer );
		}

		void Deserialize( IReadBuffer& buffer )
		{
			// read the ll, ur and max per node
			bbox.Deserialize( buffer );

			GlobalLog()->PrintEx( eLog_Info, "BSPTree::Deserialize:: New BBox LL(%lf,%lf,%lf) UR(%lf,%lf,%lf)", bbox.ll.x, bbox.ll.y, bbox.ll.z, bbox.ur.x, bbox.ur.y, bbox.ur.z );

			maxPerNode = buffer.getUInt();

			// Deserialize the root node, should cascade from there
			root.Deserialize( ep, buffer );
		}
	};
}

#endif
