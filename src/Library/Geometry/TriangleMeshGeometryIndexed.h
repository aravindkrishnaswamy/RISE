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
#include "../Acceleration/BVH.h"
#include "../Acceleration/AccelerationConfig.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshGeometryIndexed :
			public virtual ITriangleMeshGeometryIndexed2,
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
			typedef VertexColorsListType				MyColorsList;

			typedef std::vector<Scalar>					TriangleAreasList;

		protected:
			IndexedTriangleList		indexedtris;		// List of indexed polygons
			MyPointsList			pPoints;			// The list of points
			MyNormalsList			pNormals;			// The list of normals
			MyCoordsList			pCoords;			// The list of coords
			//! Optional per-vertex colors.  Indices into this array match
			//! position indices (i.e. a face's `iVertices[k]` selects the
			//! colour at `pColors[iVertices[k]]`).  Empty when the source
			//! mesh has no color data — the vertex-color painter falls
			//! back to its configured default in that case.
			MyColorsList			pColors;
			MyPointerTriangleList	ptr_polygons;		// The list of pointer triangles

			bool					bDoubleSided;		// Are the polygons all double sided?
			bool					bUseFaceNormals;	// Are we going to use computed face normals rather than interpolated vertex normals?

			// BVH is the sole active acceleration structure (Tier A2 cleanup,
			// 2026-04-27).  Legacy BSP/octree members were dropped along with
			// the on-disk v4 format that stops emitting BSP/octree bytes.
			// Pre-A2 v1/v2/v3 .risemesh files still load — the legacy bytes
			// are read into Deserialize-local temporaries, validated, and
			// discarded, never reaching this class as state.
			BVH<const PointerTriangle*>*		pPtrBVH;

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
				const bool bDoubleSided_,
				const bool bUseFaceNormals
				);

			// From ISerializable interface
			void Serialize( IWriteBuffer& buffer ) const override;
			void Deserialize( IReadBuffer& buffer ) override;

			// Pass-through tessellation: emits the stored indexed triangles unchanged.
			// The `detail` parameter is ignored — an indexed mesh already IS a mesh.
			// Index references are offset by the caller's existing vertex count.
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
			// Adds indexed triangle lists
			void BeginIndexedTriangles( ) override;				// I'm going to feed you a bunch of indexed triangles
			void AddVertex( const Vertex& point ) override;		// Adds a point
			void AddNormal( const Normal& normal ) override;	// Adds a normal
			void AddTexCoord( const TexCoord& coord ) override;	// Adds a texture co-ordinate
			void AddVertices( const VerticesListType& points ) override;
			void AddNormals( const NormalsListType& normals ) override;
			void AddTexCoords( const TexCoordsListType& coords ) override;
			void AddIndexedTriangle( const IndexedTriangle& tri ) override;
			void AddIndexedTriangles( const IndexTriangleListType& tris ) override;
			unsigned int numPoints( ) const	override	{ return static_cast<unsigned int>(pPoints.size()); }
			unsigned int numNormals( ) const override	{ return static_cast<unsigned int>(pNormals.size()); }
			unsigned int numCoords( ) const	override	{ return static_cast<unsigned int>(pCoords.size()); }
			MyPointsList const& getVertices() const { return pPoints; }
			MyNormalsList const& getNormals() const { return pNormals; }
			MyCoordsList const& getCoords() const { return pCoords; }
			MyPointerTriangleList const& getFaces() const { return ptr_polygons; }

			// ITriangleMeshGeometryIndexed2 — per-vertex color support.
			void AddColor( const VertexColor& color ) override;
			void AddColors( const VertexColorsListType& colors ) override;
			unsigned int numColors() const override { return static_cast<unsigned int>(pColors.size()); }
			VertexColorsListType const& getColors() const override { return pColors; }

			void DoneIndexedTriangles( ) override;				// I'm done feeding you a bunch of indexed triangles

			//! Tier 1 §3 animation support.  Replace the vertex and
			//! normal arrays in place (count must match the existing
			//! arrays — topology is preserved), then refit the BVH
			//! bottom-up rather than rebuild from scratch.
			//!
			//! Returns the BVH refit duration in milliseconds for
			//! caller bench reporting.  Returns 0 if there is no
			//! current BVH (caller should call DoneIndexedTriangles
			//! first) or on size mismatch.
			//!
			//! Thread-safety: must be called between frames, never
			//! concurrent with intersection.  See docs/BVH_ACCELERATION_PLAN.md
			//! §4.6 for the full design.
			unsigned int UpdateVertices( const VerticesListType& newVertices,
			                             const NormalsListType&  newNormals ) override;

			void ComputeVertexNormals() override;

			// From TreeElementProcessor
			typedef const PointerTriangle*	MYOBJ;
				void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
				void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
				bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
				BoundingBox GetElementBoundingBox( const MYOBJ elem ) const override;
				bool ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const override;
				char WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const override;

			void SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const override;
			void DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const override;

			//! Phase 2 BVH float-filter: extract per-vertex float positions.
			bool GetFloatTriangleVertices( const MYOBJ elem, float v0[3], float v1[3], float v2[3] ) const override;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override { return 0; };
			void SetIntermediateValue( const IKeyframeParameter& val ) override {};
			void RegenerateData( ) override {};
		};
	}
}

#endif
