//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshGeometryIndexed.h - Definition of a geoemtry class that is
//  made up entirely of indexed triangle meshes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 2, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRIANGLE_MESH_GEOMETRY_INDEXED_
#define TRIANGLE_MESH_GEOMETRY_INDEXED_

#include "../Interfaces/ITriangleMeshGeometry.h"
#include "Geometry.h"
#include "../Octree.h"
#include "../BSPTreeSAH.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshGeometryIndexed : 
			public virtual ITriangleMeshGeometryIndexed, 
			public virtual Geometry,
			public virtual TreeElementProcessor<const PointerTriangle*>
		{
		protected:
			virtual ~TriangleMeshGeometryIndexed();


		public:
			// This structure describes each triangle...
			// Note that some elements are as an optimiztion
			typedef IndexTriangleListType				IndexedTriangleList;
			typedef PointerTriangleListType				MyPointerTriangleList;
			typedef VerticesListType					MyPointsList;
			typedef NormalsListType						MyNormalsList;
			typedef TexCoordsListType					MyCoordsList;

			typedef std::vector<Scalar>					TriangleAreasList;

		protected:
			IndexedTriangleList		indexedtris;		// List of indexed polygons
			MyPointsList			pPoints;			// The list of points
			MyNormalsList			pNormals;			// The list of normals
			MyCoordsList			pCoords;			// The list of coords
			MyPointerTriangleList	ptr_polygons;		// The list of pointer triangles

			unsigned int			nMaxPerOctantNode;	// Maximum number of polygons per octant node
			unsigned char			nMaxRecursionLevel;	// Maximum recursion level when generating the tree

			bool					bDoubleSided;		// Are the polygons all double sided?
			bool					bUseBSP;			// Are we using BSP trees ?
			
			bool					bUseFaceNormals;	// Are we going to use computed face normals rather than interpolated vertex normals?

			Octree<const PointerTriangle*>*		pPtrOctree;
			BSPTreeSAH<const PointerTriangle*>*	pPtrBSPtree;

			TriangleAreasList		areas;				// Areas of the triangles
			TriangleAreasList		areasCDF;			// Cumulative density function of the triangle areas
			Scalar					totalArea;			// Total area

			// Mailboxing: unique ID for this geometry, used with thread_local
			// storage to avoid redundant intersection tests when the same
			// triangle appears in multiple BSP leaves.
#ifdef RISE_ENABLE_MAILBOXING
			const unsigned int					geometryId;
			size_t numTriangles() const { return ptr_polygons.size(); }
#endif

			//! Computes the triangle areas and the CDF
			void ComputeAreas();

		public:
			TriangleMeshGeometryIndexed( 
				const unsigned int max_polys_per_node, 
				const unsigned char max_recursion_level, 
				const bool bDoubleSided_,
				const bool bUseBSP,
				const bool bUseFaceNormals
				);

			// From ISerializable interface
			void Serialize( IWriteBuffer& buffer ) const;
			void Deserialize( IReadBuffer& buffer );

			// Pass-through tessellation: emits the stored indexed triangles unchanged.
			// The `detail` parameter is ignored — an indexed mesh already IS a mesh.
			// Index references are offset by the caller's existing vertex count.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const;
			BoundingBox GenerateBoundingBox() const;
			bool DoPreHitTest( ) const { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea() const;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const;

			// Functions special to this class
			// Adds indexed triangle lists
			void BeginIndexedTriangles( );				// I'm going to feed you a bunch of indexed triangles
			void AddVertex( const Vertex& point );		// Adds a point
			void AddNormal( const Normal& normal );		// Adds a normal
			void AddTexCoord( const TexCoord& coord );	// Adds a texture co-ordinate
			void AddVertices( const VerticesListType& points );
			void AddNormals( const NormalsListType& normals );
			void AddTexCoords( const TexCoordsListType& coords );
			void AddIndexedTriangle( const IndexedTriangle& tri );
			void AddIndexedTriangles( const IndexTriangleListType& tris );
			unsigned int numPoints( ) const		{ return static_cast<unsigned int>(pPoints.size()); }
			unsigned int numNormals( ) const	{ return static_cast<unsigned int>(pNormals.size()); }
			unsigned int numCoords( ) const		{ return static_cast<unsigned int>(pCoords.size()); }
			MyPointsList const& getVertices() const { return pPoints; }
			MyNormalsList const& getNormals() const { return pNormals; }
			MyCoordsList const& getCoords() const { return pCoords; }
			MyPointerTriangleList const& getFaces() const { return ptr_polygons; }
			void DoneIndexedTriangles( );				// I'm done feeding you a bunch of indexed triangles

			void ComputeVertexNormals();

			// From TreeElementProcessor
			typedef const PointerTriangle*	MYOBJ;
				void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
				void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
				bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
				BoundingBox GetElementBoundingBox( const MYOBJ elem ) const;
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
