//////////////////////////////////////////////////////////////////////
//
//  ITriangleMeshGeometry.h - Interface to triangle mesh geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ITRIANGLE_MESH_GEOMETRY_
#define ITRIANGLE_MESH_GEOMETRY_

#include "IGeometry.h"
#include "ISerializable.h"
#include "../Polygon.h"

namespace RISE
{
	//! A geometry class made up of pure triangles
	/// \sa IGeometry
	class ITriangleMeshGeometry : public virtual IGeometry, public virtual ISerializable
	{
	protected:
		ITriangleMeshGeometry(){};
		virtual ~ITriangleMeshGeometry(){};

	public:
		// Functions special to this type of class

		//
		// Adds a triangle to the existing list of triangles
		//

		//! Begins adding triangles
		virtual void BeginTriangles( ) = 0;

		//! Add a single triangle
		virtual void AddTriangle(
			const Triangle& tri														///< [in] Triangle to add
			) = 0;

		//! Done adding a bunch of triangles
		virtual void DoneTriangles( ) = 0;
	};

	//! A geometry class made up of indexed triangles
	/// \sa IGeometry
	class ITriangleMeshGeometryIndexed : public virtual IGeometry, public virtual ISerializable
	{
	protected:
		ITriangleMeshGeometryIndexed(){};
		virtual ~ITriangleMeshGeometryIndexed(){};

	public:
		// Functions special to this type of class
		
		//
		// Adds indexed triangle lists
		//

		//! Beings adding indexed triangles
		virtual void BeginIndexedTriangles() = 0;

		//! Adds a point
		virtual void AddVertex( const Vertex& point ) = 0;

		//! Adds a normal
		virtual void AddNormal( const Normal& normal ) = 0;

		//! Adds a texture co-ordinate
		virtual void AddTexCoord( const TexCoord& coord ) = 0;

		//! Adds a list of points
		virtual void AddVertices( const VerticesListType& points ) = 0;

		//! Adds a list of normals
		virtual void AddNormals( const NormalsListType& normals ) = 0;

		//! Adds a list of texture co-ordinates
		virtual void AddTexCoords( const TexCoordsListType& coords ) = 0;

		//! Adds a single indexed triangle
		virtual void AddIndexedTriangle( const IndexedTriangle& tri ) = 0;

		//! Adds a list of indexed triangles
		virtual void AddIndexedTriangles( const IndexTriangleListType& tris ) = 0;

		/// \return The number of points
		virtual unsigned int numPoints() const = 0;

		/// \return The number of normals
		virtual unsigned int numNormals() const = 0;

		/// \return The number of texture co-ordinates
		virtual unsigned int numCoords() const = 0;

		//! Done adding a bunch of indexed triangles
		virtual void DoneIndexedTriangles() = 0;
		
		//! Computes vertex normals
		//! Requires: You must be at the end of doing IndexedTriangles
		//!           You must have already specified all the vertices
		//!			  You must have added all the IndexedTriangles
		//!	          You MUST not have called DoneIndexedTriangles yet!
		virtual void ComputeVertexNormals() = 0;

		//! Tier 1 §3 animation refit support.  Replace vertex / normal
		//! arrays in place (count must match the existing arrays —
		//! topology preserved) and refit the BVH bottom-up.  Returns
		//! the BVH refit duration in ms; 0 on size mismatch.
		//!
		//! Caller must ensure this is invoked between frames, never
		//! concurrent with intersection.  See
		//! docs/BVH_ACCELERATION_PLAN.md §4.6.
		virtual unsigned int UpdateVertices(
			const VerticesListType& newVertices,
			const NormalsListType&  newNormals ) = 0;
	};

	//! Sub-interface that adds optional per-vertex color storage to
	//! ITriangleMeshGeometryIndexed.  Color indices are tied to vertex
	//! position indices (every position index `i` maps to the same color
	//! index `i`) — the convention every common exporter (PLY, 3DS, RAW2,
	//! FBX, glTF COLOR_0) actually produces, so adding a fourth `iColors`
	//! per triangle would just bloat `PointerTriangle` (the BVH leaf
	//! payload) for no exporter-driven benefit.
	//!
	//! Loaders that want to attach colors `dynamic_cast` to this
	//! interface; if the cast fails the loader should drop colors with a
	//! warning rather than fail the load.  Adding it as a separate
	//! interface (instead of new pure virtuals on the v1 interface)
	//! preserves the v1 vtable layout for any out-of-tree subclass.
	class ITriangleMeshGeometryIndexed2 : public virtual ITriangleMeshGeometryIndexed
	{
	protected:
		ITriangleMeshGeometryIndexed2(){};
		virtual ~ITriangleMeshGeometryIndexed2(){};

	public:
		//! Adds a single per-vertex color (linear ROMM RGB; see RISEPel).
		virtual void AddColor( const VertexColor& color ) = 0;

		//! Adds a list of per-vertex colors.
		virtual void AddColors( const VertexColorsListType& colors ) = 0;

		/// \return The number of stored vertex colors (0 if not present)
		virtual unsigned int numColors() const = 0;

		/// \return The vertex-color array; may be empty.
		virtual VertexColorsListType const& getColors() const = 0;
	};
}

#endif

