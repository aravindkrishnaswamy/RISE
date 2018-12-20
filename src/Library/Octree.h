//////////////////////////////////////////////////////////////////////
//
//  Octree.h - Definition of a templated octree class.  This class
//  allows arbritary types to be stored in the octree as long
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

#ifndef OCTREE_
#define OCTREE_

#include "TreeElementProcessor.h"
#include "Utilities/Reference.h"
#include "Utilities/BoundingBox.h"
#include "Utilities/GeometricUtilities.h"
#include "Intersection/RayPrimitiveIntersections.h"
#include "Intersection/RayIntersection.h"
#include "Interfaces/ILog.h"
#include "Interfaces/ISerializable.h"
#include "Utilities/RTime.h"
#include <vector>
#include <algorithm>

namespace RISE
{
	static const Scalar		error_delta_box_size = 0.0001;

	template< class Element >
	class Octree : public Implementation::Reference
	{
	protected:

		#include "OctreeNode.h"

		OctreeNode		root;			// Root node of the tree
		BoundingBox		bbox;			// Bounding box
		unsigned int	maxPerNode;
		const TreeElementProcessor<Element>& ep;

		virtual ~Octree( ){}

	public:
		// Must define the tree size, in full 3D space upon creation, I'll write a cooler
		// octree than resize itself and everything later, for now this is more practical
		Octree( const TreeElementProcessor<Element>& ep_, const BoundingBox& bbox_, const unsigned int max_elems_in_one_node ) :
		bbox( bbox_ ),
		maxPerNode( max_elems_in_one_node ),
		ep( ep_ )
		{
			GlobalLog()->PrintEx( eLog_Info, "Octree:: Overall BBox LL(%lf,%lf,%lf) UR(%lf,%lf,%lf)", bbox.ll.x, bbox.ll.y, bbox.ll.z, bbox.ur.x, bbox.ur.y, bbox.ur.z );
		};

		void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces ) const
		{
			// Pass the request to the root
			BOX_HIT	h;
			root.IntersectRayBB( bbox, 99, ri.ray, h );

			if( h.bHit ) {
				root.IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbox, 99 );
			}
		}

		void IntersectRay( RayIntersection& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
		{
			// Pass the request to the root
			BOX_HIT	h;
			root.IntersectRayBB( bbox, 99, ri.geometric.ray, h );

			if( h.bHit ) {
				root.IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbox, 99 );
			}
		}

		bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
		{
			// Pass the request to the root
			BOX_HIT	h;
			root.IntersectRayBB( bbox, 99, ray, h );

			if( h.bHit ) {
				if( (h.dRange < dHowFar) ||
					GeometricUtilities::IsPointInsideBox( ray.origin, bbox.ll, bbox.ur )
					)
				{
					return root.IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbox, 99 );
				}
			}

			return false;
		}

		// All elements must be added at the same time.  Again a more robust Octree system
		// could and probably should be written, but thats later
		bool AddElements( const std::vector<Element>& elements, const unsigned char max_recursion_level )
		{
			GlobalLog()->PrintEx( eLog_Info, "Octree:: Generating tree for %d elements, Max Recursion: %d, Max Elements/node: %d", elements.size(), max_recursion_level, maxPerNode );
			Timer	t;
			t.start();
			bool	bRet = root.AddElements( ep, elements, maxPerNode, bbox, 99, max_recursion_level );
			t.stop();
			GlobalLog()->PrintEx( eLog_Info, "Octree:: Time to generate Octree %d seconds, %d ms", t.getInterval()/1000, t.getInterval()%1000 );
			return bRet;
		}

		void DumpStatistics( const LOG_ENUM e ) const
		{
			// Dumps to the global log
			GlobalLog()->Print( e, "Octree output: " );

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

			GlobalLog()->PrintEx( eLog_Info, "Octree::Deserialize:: New BBox LL(%lf,%lf,%lf) UR(%lf,%lf,%lf)", bbox.ll.x, bbox.ll.y, bbox.ll.z, bbox.ur.x, bbox.ur.y, bbox.ur.z );

			maxPerNode = buffer.getUInt();

			// Deserialize the root node, should cascade from there
			root.Deserialize( ep, buffer );
		}
	};
}

#endif
