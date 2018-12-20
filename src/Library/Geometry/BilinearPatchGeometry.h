//////////////////////////////////////////////////////////////////////
//
//  BilinearPatchGeometry.h - Bilinear patch geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 18, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BILINEARPATCH_GEOMETRY_
#define BILINEARPATCH_GEOMETRY_

#include "Geometry.h"
#include "../Interfaces/IBilinearPatchGeometry.h"
#include "../Octree.h"
#include "../BSPTree.h"

namespace RISE
{
	namespace Implementation
	{
		class BilinearPatchGeometry : 
			public virtual IBilinearPatchGeometry,
			public virtual Geometry,
			public virtual TreeElementProcessor<const BilinearPatch*>
		{
		public:
			typedef std::vector<BilinearPatch>			BilinearPatchList;
			typedef std::vector<const BilinearPatch*>	BilinearPatchPtrList;


		protected:
			// The BSP tree of bezier patches
			BSPTree<const BilinearPatch*>*	pBSPTree;

			// The Octree tree of bezier patches
			Octree<const BilinearPatch*>*		pOctree;

			BilinearPatchList		patches;			// List of bezier patches
			unsigned int			nMaxPerOctantNode;	// Maximum number of patches per octant node
			unsigned char			nMaxRecursionLevel;	// Maximum recursion level when generating the tree
			bool					bUseBSP;			// Are we using BSP trees ?

			virtual ~BilinearPatchGeometry( );

		public:
			BilinearPatchGeometry(
				const unsigned int max_polys_per_node, 
				const unsigned char max_recursion_level,
				const bool bUseBSP
				);

			// Adds a new patch to the list
			void AddPatch( const BilinearPatch& patch );

			// Instructs that addition of new patches is complete and that
			// we can prepare for rendering
			void Prepare();

			void GenerateMesh( );
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const; 
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea( ) const;

			// From TreeElementProcessor
			typedef const BilinearPatch*		MYOBJ;
			void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
			void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
			bool ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const;
			char WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const;

			void SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const;
			void DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0; };
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif
