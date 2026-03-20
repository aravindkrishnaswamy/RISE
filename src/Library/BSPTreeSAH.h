//////////////////////////////////////////////////////////////////////
//
//  BSPTreeSAH.h - Definition of a templated SAH based BSP tree class.
//  This class stores arbitrary types as long as a TreeElementProcessor
//  can provide intersection and bounding box helpers for that type.
//
//  Author: OpenAI Codex
//  Date: March 19, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BSP_TREE_SAH_
#define BSP_TREE_SAH_

#include "TreeElementProcessor.h"
#include "Utilities/Reference.h"
#include "Utilities/BoundingBox.h"
#include "Utilities/GeometricUtilities.h"
#include "Intersection/RayPrimitiveIntersections.h"
#include "Intersection/RayIntersection.h"
#include "Interfaces/ILog.h"
#include "Interfaces/ISerializable.h"
#include "Utilities/RTime.h"
#include "Utilities/Profiling.h"
#include "Utilities/stl_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace RISE
{
	static const unsigned char	BSP_SAH_AXIS_X = 0;
	static const unsigned char	BSP_SAH_AXIS_Y = 1;
	static const unsigned char	BSP_SAH_AXIS_Z = 2;
	static const unsigned char	BSP_SAH_AXIS_INVALID = 255;

	static const Scalar			bsp_sah_error_delta_box_size = 0.0001;
	static const Scalar			bsp_sah_traversal_cost = 1.0;
	static const Scalar			bsp_sah_intersection_cost = 80.0;
	static const Scalar			bsp_sah_empty_bonus = 0.2;

	template< class Element >
	class BSPTreeSAH : public Implementation::Reference
	{
	protected:
		typedef std::vector<Element>	ElementListType;

		struct ElementInfo
		{
			Element		elem;
			BoundingBox	bbox;

			ElementInfo( const Element& elem_, const BoundingBox& bbox_ ) :
				elem( elem_ ),
				bbox( bbox_ )
			{}
		};

		typedef std::vector<ElementInfo> ElementInfoListType;

		struct BoundEdge
		{
			enum EDGE_TYPE
			{
				EDGE_END = 0,
				EDGE_START = 1
			};

			Scalar			t;
			unsigned int	primNum;
			EDGE_TYPE		type;

			BoundEdge() :
				t( 0 ),
				primNum( 0 ),
				type( EDGE_START )
			{}

			BoundEdge( const Scalar t_, const unsigned int primNum_, const bool bStart ) :
				t( t_ ),
				primNum( primNum_ ),
				type( bStart ? EDGE_START : EDGE_END )
			{}
		};

		struct BoundEdgeLess
		{
			bool operator()( const BoundEdge& a, const BoundEdge& b ) const
			{
				if( a.t == b.t ) {
					return a.type < b.type;
				}
				return a.t < b.t;
			}
		};

		#include "BSPTreeSAHNode.h"

		BSPTreeNodeSAH			root;
		BoundingBox				bbox;
		unsigned int			maxPerNode;
		const TreeElementProcessor<Element>& ep;

		virtual ~BSPTreeSAH( ){}

	public:
		BSPTreeSAH( const TreeElementProcessor<Element>& ep_, const BoundingBox& bbox_, const unsigned int max_elems_in_one_node ) :
			bbox( bbox_ ),
			maxPerNode( max_elems_in_one_node ),
			ep( ep_ )
		{
			GlobalLog()->PrintEx( eLog_Info, "BSPTreeSAH:: Overall BBox LL(%lf,%lf,%lf) UR(%lf,%lf,%lf)", bbox.ll.x, bbox.ll.y, bbox.ll.z, bbox.ur.x, bbox.ur.y, bbox.ur.z );
		}

		void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces ) const
		{
			ri.bHit = false;
			ri.range = RISE_INFINITY;
			root.IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bbox );
		}

		void IntersectRay( RayIntersection& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
		{
			ri.geometric.bHit = false;
			ri.geometric.range = RISE_INFINITY;
			root.IntersectRay( ep, ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo, bbox );
		}

		bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
		{
			return root.IntersectRay_IntersectionOnly( ep, ray, dHowFar, bHitFrontFaces, bHitBackFaces, bbox );
		}

		void AddElements( const std::vector<Element>& elements, const unsigned char max_recursion_level )
		{
			GlobalLog()->PrintEx( eLog_Info, "BSPTreeSAH:: Generating tree for %d elements, Max Recursion: %d, Max Elements/node: %d", static_cast<int>(elements.size()), max_recursion_level, maxPerNode );
			Timer t;
			t.start();

			ElementInfoListType info;
			info.reserve( elements.size() );
			typename std::vector<Element>::const_iterator i, e;
			for( i=elements.begin(), e=elements.end(); i!=e; i++ ) {
				info.push_back( ElementInfo( *i, ep.GetElementBoundingBox( *i ) ) );
			}

			root.AddElements( ep, info, maxPerNode, bbox, max_recursion_level, 1 );

			t.stop();
			GlobalLog()->PrintEx( eLog_Info, "BSPTreeSAH:: Time to generate BSPTreeSAH %d seconds, %d ms", t.getInterval()/1000, t.getInterval()%1000 );
		}

		void DumpStatistics( const LOG_ENUM e ) const
		{
			GlobalLog()->Print( e, "BSPTreeSAH output: " );
			unsigned int total_elems = 0;
			unsigned int total_nodes = root.DumpStatistics( e, maxPerNode, total_elems, 0 );
			GlobalLog()->PrintEx( e, "Total nodes in tree: %u", total_nodes );
			GlobalLog()->PrintEx( e, "Total elements in all leaf nodes: %u", total_elems );
		}

		BoundingBox GetBBox() const
		{
			return bbox;
		}

		bool GetRootSplit( unsigned char& axis, Scalar& location ) const
		{
			return root.GetSplit( axis, location );
		}

		void Serialize( IWriteBuffer& buffer ) const
		{
			bbox.Serialize( buffer );
			buffer.setUInt( maxPerNode );
			root.Serialize( ep, buffer );
		}

		void Deserialize( IReadBuffer& buffer )
		{
			bbox.Deserialize( buffer );

			GlobalLog()->PrintEx( eLog_Info, "BSPTreeSAH::Deserialize:: New BBox LL(%lf,%lf,%lf) UR(%lf,%lf,%lf)", bbox.ll.x, bbox.ll.y, bbox.ll.z, bbox.ur.x, bbox.ur.y, bbox.ur.z );

			maxPerNode = buffer.getUInt();
			root.Deserialize( ep, buffer );
		}
	};
}

#endif
