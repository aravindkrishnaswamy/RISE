//////////////////////////////////////////////////////////////////////
//
//  PointSetOctree.h - The point set octree, which we use
//    in two translucence shaders for acceleration
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 19, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POINTSET_OCTREENODE_H_
#define POINTSET_OCTREENODE_H_

#include "../../Interfaces/ISubSurfaceExtinctionFunction.h"
#include "../../Interfaces/IBSDF.h"
#include "../../Utilities/GeometricUtilities.h"

namespace RISE
{
	namespace Implementation
	{
		class PointSetOctree
		{
		public:
			struct SamplePoint
			{
				Point3			ptPosition;			// Location of the sample point
				RISEPel			irrad;				// irradiance of the sample point
			};
			typedef std::vector<SamplePoint> PointSet;

		protected:
			class PointSetOctreeNode
			{
			protected:
				PointSetOctreeNode** pChildren;			// Children of this node
				PointSet* pElements;					// Elements of this node
				RISEPel irrad;							// Average irradiance of this node
				
				Scalar HowFarIsPointFromYou(
					Vector3& dir,
					const Point3& point,
					const BoundingBox bbox,
					const char which_child
					) const;


				void MyBBFromParent( 
					const BoundingBox& bbox,
					char which_child, 
					BoundingBox& my_bb 
					) const;

				const RISEPel& AverageIrradiance(
					) const;

			public:
				PointSetOctreeNode();
				virtual ~PointSetOctreeNode();

				bool AddElements( 
					const PointSet& points,
					const unsigned int maxElements,
					const BoundingBox& bbox,
					const char which_child,
					const unsigned char max_recursion_level
					);

				void Evaluate(
					RISEPel& c,
					const BoundingBox& bbox,
					const char which_child,
					const Point3& point, 
					const ISubSurfaceExtinctionFunction& pFunc,
					const Scalar maxDistance,
					const IBSDF* pBSDF,
					const RayIntersectionGeometric& rig
					) const;
			};

			PointSetOctreeNode root;
			BoundingBox		bbox;			// Bounding box
			unsigned int	maxPerNode;


		public:
			PointSetOctree(
				const BoundingBox& bbox_, 
				const unsigned int max_elems_in_one_node
				) : 
				bbox( bbox_ ),
				maxPerNode( max_elems_in_one_node )
			{
			};

			virtual ~PointSetOctree()
			{
			}

			bool AddElements( 
				PointSet& points, 
				const unsigned char max_recursion_level )
			{
				return root.AddElements( points, maxPerNode, bbox, 99, max_recursion_level );
			}

			void Evaluate( 
				RISEPel& c,
				const Point3& point, 
				const ISubSurfaceExtinctionFunction& pFunc,
				const Scalar error,
				const IBSDF* pBSDF,
				const RayIntersectionGeometric& rig
				)
			{
				return root.Evaluate( c, bbox, 99, point, pFunc, pFunc.GetMaximumDistanceForError(error), pBSDF, rig );
			};
		};
	}
}

#endif

