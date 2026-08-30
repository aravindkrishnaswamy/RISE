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
#include "../Interfaces/ISurfaceSignalProvider.h"	// occlusion()/thickness() dispatch (design doc Phase 3)
#include "Geometry.h"
#include "MeshSignalBake.h"
#include "../Acceleration/BVH.h"
#include "../Acceleration/AccelerationConfig.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		//! Also an ISurfaceSignalProvider and its own bake occluder: the
		//! `occlusion(r)` / `thickness(r)` expression builtins are answered
		//! for meshes out of a LAZY per-vertex bake this class owns, traced
		//! against its own BVH (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §7).
		//!
		//! The bake is keyed on THIS GEOMETRY -- never on an IObject* --
		//! which is what makes invalidation free: the incremental derive
		//! path drops and recreates a geometry on edit (so a fresh instance
		//! starts with an empty cache), while object chunks are re-pointed
		//! IN PLACE and would have kept a stale bake (§7.2's named trap).
		class TriangleMeshGeometryIndexed :
			public virtual ITriangleMeshGeometryIndexed3,
			public virtual Geometry,
			public virtual TreeElementProcessor<const PointerTriangle*>,
			public ISurfaceSignalProvider,
			public MeshSignalBake::ISelfOccluder,
			public MeshSignalBake::IInputSource
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
			typedef Tangent4ListType					MyTangentsList;

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
			//! Optional per-vertex tangents (v3 interface).  Same indexing
			//! convention as colors: tangent[i] is paired with position[i].
			//! Empty when absent.  Live-only state — not yet persisted in
			//! the .risemesh format (Phase 1 glTF import has no consumer).
			MyTangentsList			pTangents;
			//! Optional secondary UV set (TEXCOORD_1, v3 interface).
			//! Indexed via face's iCoords[k] — same indices as primary UVs.
			//! Empty when absent.  Same persistence note as pTangents.
			MyCoordsList			pTexCoords1;
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

			//! LAZY per-vertex signal bakes (occlusion / thickness), one
			//! table per (kind, radius).  `mutable` + the cache's own RMutex
			//! is the sanctioned ARCHITECTURE.md §Known Exceptions pattern
			//! (SSS point-set precedent); see MeshSignalBake.h for why the
			//! build cannot be eager.
			mutable MeshSignalBakeCache	m_signalBakes;

			//! Per-POSITION normals for the bake, built once alongside the
			//! first table.  NOT the same array as `pNormals`: that one is
			//! independently indexed (a position can carry several normals)
			//! and may be empty on a face-normal mesh, while a per-vertex
			//! bake needs exactly one orientation per POSITION.  Built by
			//! accumulating each incident corner's authored normal, or the
			//! face normal where none is authored.
			mutable std::vector<Vector3>	m_signalVertexNormals;
			mutable bool					m_signalVertexNormalsBuilt;

			//! Builds m_signalVertexNormals if it has not been built.  Called
			//! ONLY from inside the bake cache's find-or-build (i.e. under
			//! its lock), which is what makes touching these two mutable
			//! members safe.
			void EnsureSignalVertexNormals() const;

			//! MeshSignalBake::IInputSource -- assembles the bake input for
			//! `radiusFraction`, called by the cache from inside its lock on
			//! a miss.  Object space throughout; the radius is a FRACTION of
			//! this mesh's own bounding-box diagonal (design doc §9), which
			//! is what makes a shared bake correct for every instance of
			//! this geometry.
			bool MakeBakeInput( const Scalar radiusFraction, MeshSignalBake::Input& out ) const override;

			//! Answers a baked signal at a hit: validate the radius per
			//! §7.1's constant-radius precondition, find-or-build, then
			//! barycentrically interpolate.  The shared body of
			//! ComputeOcclusion / ComputeThickness.
			//! Drops the lazy bakes and the per-position normals they were
			//! built from.  Called wherever the vertex data underneath them
			//! changes -- rebuild (`BeginIndexedTriangles`/
			//! `DoneIndexedTriangles`), vertex-level animation
			//! (`UpdateVertices`) and `ComputeVertexNormals`.
			//!
			//! §14 item 10 asked whether deforming geometry could stale a
			//! bake, on the assumption that RISE animates transforms rather
			//! than vertices.  `UpdateVertices` is the counter-example: the
			//! keyframed-painter DisplacedGeometry path replaces the vertex
			//! array IN PLACE, so one IGeometry* genuinely does change shape
			//! over time.  Free invalidation does not cover it, and this is
			//! the explicit invalidation it needs instead.  Same contract as
			//! UpdateVertices itself: between frames, never concurrent with
			//! rendering.
			void InvalidateSignalBakes();

			bool LookupBakedSignal(
				const MeshSignalBake::Kind kind,
				const SurfaceSignalInfo& hit,
				const Scalar radiusFraction,
				const bool bRadiusIsConstant,
				Scalar& outValue ) const;

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

			//! ISurfaceSignalProvider -- the `occlusion(radius)` builtin on
			//! the mesh family (design doc §7).  Reads the per-vertex AO bake
			//! for `radiusFraction`, building it on this first call if it
			//! does not exist yet, and interpolates it barycentrically over
			//! the hit triangle stamped in `hit`.
			//!
			//! REFUSES (neutral fallback) when the radius was not proven a
			//! compile-time constant, when the hit carries no triangle, or
			//! when the bake could not be built -- never substitutes a
			//! different radius's table (§7.1's mismatch contract: a
			//! wrong-scale mask that looks plausible is worse than a flat one
			//! that is visibly absent).
			bool ComputeOcclusion( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const override;

			//! ISurfaceSignalProvider -- the `thickness(radius)` builtin.
			//! Same bake / interpolate / refuse structure as ComputeOcclusion.
			bool ComputeThickness( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const override;

			//! MeshSignalBake::ISelfOccluder -- boolean any-hit against THIS
			//! mesh's own triangles, through its own BVH.  Used only during a
			//! bake build; sees nothing but this mesh (design doc §8).
			bool AnyHitWithin( const Point3& origin, const Vector3& unitDir, const Scalar maxDist ) const override;

			//! MeshSignalBake::ISelfOccluder -- nearest self-hit distance.
			bool NearestHitWithin( const Point3& origin, const Vector3& unitDir,
				const Scalar maxDist, Scalar& outDist ) const override;

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

			// ITriangleMeshGeometryIndexed3 — tangents + secondary UV set.
			void AddTangent( const Tangent4& tangent ) override;
			void AddTangents( const Tangent4ListType& tangents ) override;
			unsigned int numTangents() const override { return static_cast<unsigned int>(pTangents.size()); }
			Tangent4ListType const& getTangents() const override { return pTangents; }
			void AddTexCoord1( const TexCoord& coord ) override;
			void AddTexCoords1( const TexCoordsListType& coords ) override;
			unsigned int numTexCoords1() const override { return static_cast<unsigned int>(pTexCoords1.size()); }
			TexCoordsListType const& getTexCoords1() const override { return pTexCoords1; }

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
