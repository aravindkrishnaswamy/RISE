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
#include "../Acceleration/BVH.h"
#include "../Acceleration/AccelerationConfig.h"
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

			bool					bDoubleSided;		// Are the polygons all double sided?

			// BVH is the sole active acceleration structure (Tier A2 cleanup,
			// 2026-04-27).  Legacy BSP/octree members were dropped along with
			// the on-disk v4 format that stops emitting BSP/octree bytes.
			// Pre-A2 v2/v3 .risemesh files still load — the legacy bytes are
			// read into Deserialize-local temporaries, validated, and
			// discarded, never reaching this class as state.
			BVH<const Triangle*>*				pPolygonsBVH;

			TriangleAreasList		areas;				// Areas of the triangles
			TriangleAreasList		areasCDF;			// Cumulative density function of the triangle areas
			Scalar					totalArea;			// Total area

			//! Computes the triangle areas and the CDF
			void ComputeAreas();

		public:
			TriangleMeshGeometry(
				const bool bDoubleSided_
				);

			// From ISerializable interface
			void Serialize( IWriteBuffer& buffer ) const override;
			void Deserialize( IReadBuffer& buffer ) override;

			// Pass-through tessellation: emits the existing triangles (de-indexed per-vertex).
			// The `detail` parameter is ignored — a triangle-mesh source already IS a mesh.
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const override;

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;
			bool DoPreHitTest( ) const override { return true; };

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea() const override;

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const override;

			// Functions special to this class

			// Adds a triangle to the existing list of triangles
			void BeginTriangles( ) override;					// I'm going to feed you a bunch of triangles
			void AddTriangle( const Triangle& tri ) override;
			void DoneTriangles( ) override;						// I'm done feeding you a bunch of triangles

			const MyTriangleList	getTriangles() const{ return polygons; }

			// From TreeElementProcessor
			typedef const Triangle*	MYOBJ;
				void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
				void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
				bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
				BoundingBox GetElementBoundingBox( const MYOBJ elem ) const override;
				bool ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const override;
				char WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const override;

			void SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const override;
			void DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const override;

			//! Tier 1 §4 BVH float-filter: extract per-vertex float positions.
			bool GetFloatTriangleVertices( const MYOBJ elem, float v0[3], float v1[3], float v2[3] ) const override;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override { return 0; };
			void SetIntermediateValue( const IKeyframeParameter& val ) override {};
			void RegenerateData( ) override {};
		};
	}
}

#endif
