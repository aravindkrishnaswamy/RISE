//////////////////////////////////////////////////////////////////////
//
//  BezierPatchGeometry.h - Bezier patch geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 17, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BEZIERPATCH_GEOMETRY_
#define BEZIERPATCH_GEOMETRY_

#include "Geometry.h"
#include "../Interfaces/IBezierPatchGeometry.h"
#include "../Octree.h"
#include "../BSPTree.h"
#include "BezierValueGenerator.h"

namespace RISE
{
	namespace Implementation
	{
		class BezierPatchGeometry : 
			public virtual IBezierPatchGeometry,
			public virtual Geometry,
			public virtual TreeElementProcessor<MYBEZIERPATCH>
		{
		public:	

			typedef std::vector<BezierPatch>				BezierPatchList;
			typedef std::vector<MYBEZIERPATCH>			BezierPatchPtrList;

		protected:
			// The BSP tree of bezier patches
			BSPTree<MYBEZIERPATCH>*		pBSPTree;

			// The Octree tree of bezier patches
			Octree<MYBEZIERPATCH>*		pOctree;

			BezierPatchList			patches;			// List of bezier patches
			unsigned int			nMaxPerOctantNode;	// Maximum number of polygons per octant node
			unsigned char			nMaxRecursionLevel;	// Maximum recursion level when generating the tree
			bool					bUseBSP;			// Are we using BSP trees ?
			bool					bAnalytic;			// Are we going to analytically render the bezier patches ?

			// The value generator generates tesselated patches
			BezierValueGenerator generator;

			// The MRU cache 
			mutable MRUCache<MYBEZIERPATCH, ITriangleMeshGeometryIndexed> cache;

			virtual ~BezierPatchGeometry( );

		public:
			BezierPatchGeometry(
				const unsigned int max_patch_per_node, 
				const unsigned char max_recursion_level,
				const bool bUseBSP,
				const bool bAnalytic,
				const unsigned int cache_size,
				const unsigned int max_polys_per_node, 
				const unsigned char max_poly_recursion_level, 
				const bool bDoubleSided,
				const bool bPolyUseBSP,
				const bool bUseFaceNormals,
				const unsigned int detail,
				const IFunction2D* displacement,
				const Scalar disp_scale
				);

			// Adds a new patch to the list
			void AddPatch( const BezierPatch& patch );

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
			typedef MYBEZIERPATCH		MYOBJ;
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
