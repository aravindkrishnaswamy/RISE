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
	};
}

#endif

