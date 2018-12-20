//////////////////////////////////////////////////////////////////////
//
//  GeometryUtilities.h - Declaration of a bunch of utility
//  functions for dealing with geometric objects
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../Polygon.h"
#include "../Interfaces/IFunction2D.h"

namespace RISE
{
	extern bool CalculateVertexNormals( IndexTriangleListType& vFaces, NormalsListType& vNormals, VerticesListType& vVertices );

	// Generates a grid, with independentably settable width and height
	extern bool GenerateGrid(
		const int nWidthDetail, 
		const int nHeightDetail, 
		const Scalar left, 
		const Scalar top, 
		const Scalar right, 
		const Scalar bottom,
		VerticesListType& vVertices, 
		NormalsListType& vNormals,
		TexCoordsListType& vCoords, 
		IndexTriangleListType& vFaces
		);

	// Re-map texture co-ords
	// Remaps the texture co-ors so that instead of going from [0..1] it goes from [1..0..1]
	extern void RemapTextureCoords(
		TexCoordsListType& vCoords 
		);

	// Inverts a given object
	extern bool InvertObject( 
		IndexTriangleListType& vFaces, 
		NormalsListType& vNormals, 
		TexCoordsListType& vCoords 
		);


	//
	// Given a bunch of points, we compute the center of the points and move all the points so that they're
	// center is at the origin (0,0,0)
	extern bool CenterObject(
		VerticesListType& vVertices 
		);

	//
	// Given a bunch of polygons with their points, we will detect shared points and combine them
	// We assume the vertex, texture mapping co-ordinates and normal indices are all the same in 
	// a polygon
	extern bool CombineSharedVertices( 
		IndexTriangleListType& vFaces, 
		VerticesListType& vVertices 
		);

	//
	// Given a bunch of polygons and their points, and assuming that the polygons are basically a list
	// of grids with same detail, we try to detect shared points and combine them.
	extern void CombineSharedVerticesFromGrids(
		IndexTriangleListType& vFaces, 
		VerticesListType& vVertices,
		const unsigned int numgrids,
		const unsigned int nWidthDetail,
		const unsigned int nHeightDetail
		);

	// Applies a displacement map to the given Object
	extern void ApplyDisplacementMapToObject( 
		IndexTriangleListType& vFaces, 
		VerticesListType& vVertices, 
		NormalsListType& vNormals, 
		TexCoordsListType& vCoords,
		const IFunction2D& displacement,
		const Scalar scale
		);

}
