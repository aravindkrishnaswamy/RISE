//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshGeometry.h - Definition of a geoemtry class that is
//  made up entirely of triangle meshes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRIANGLE_MESH_GEOMETRY_
#define TRIANGLE_MESH_GEOMETRY_

#include "../Interfaces/ITriangleMeshGeometry.h"
#include "Geometry.h"
#include "../Octree.h"
#include "../BSPTree.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshGeometry : 
			public virtual ITriangleMeshGeometry, 
			public virtual Geometry,
			public virtual TreeElementProcessor<const Triangle*>
		{
		protected:
			virtual ~TriangleMeshGeometry();


		public:
			// This structure describes each triangle...
			// Note that some elements are as an optimiztion
			typedef TriangleListType					MyTriangleList;

			typedef std::vector<Scalar>					TriangleAreasList;

		protected:
			MyTriangleList			polygons;			// The list of polygons

			unsigned int			nMaxPerOctantNode;	// Maximum number of polygons per octant node
			unsigned char			nMaxRecursionLevel;	// Maximum recursion level when generating the tree

			bool					bDoubleSided;		// Are the polygons all double sided?
			bool					bUseBSP;			// Are we using BSP trees ?
			
			Octree<const Triangle*>*			pPolygonsOctree;
			BSPTree<const Triangle*>*			pPolygonsBSPtree;

			TriangleAreasList		areas;				// Areas of the triangles
			TriangleAreasList		areasCDF;			// Cumulative density function of the triangle areas
			Scalar					totalArea;			// Total area

			//! Computes the triangle areas and the CDF
			void ComputeAreas();

		public:
			TriangleMeshGeometry( 
				const unsigned int max_polys_per_node, 
				const unsigned char max_recursion_level, 
				const bool bDoubleSided_,
				const bool bUseBSP
				);

			// From ISerializable interface
			void Serialize( IWriteBuffer& buffer ) const;
			void Deserialize( IReadBuffer& buffer );

			void GenerateMesh( );
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const;
			BoundingBox GenerateBoundingBox() const;
			bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea() const;

			// Functions special to this class

			// Adds a triangle to the existing list of triangles
			void BeginTriangles( );						// I'm going to feed you a bunch of triangles
			void AddTriangle( const Triangle& tri );
			void DoneTriangles( );						// I'm done feeding you a bunch of triangles
			
			const MyTriangleList	getTriangles() const{ return polygons; }

			// From TreeElementProcessor
			typedef const Triangle*	MYOBJ;
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

