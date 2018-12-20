//////////////////////////////////////////////////////////////////////
//
//  Polygon.h - A templated class which is supposed to represent
//				a polygon
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POLYGON_
#define POLYGON_

#include <vector>
#include "Utilities/Math3D/Math3D.h"

namespace RISE
{

	////////////////////////////////////
	//
	// A polygon class
	//
	////////////////////////////////////
	template< int N >
	class Polygon_Template
	{
	public:
		//
		// A standard polygon has vertices, normals and
		// 2 sets of texture mapping co-ordinates
		//
		Point3			vertices[N];
		Vector3			normals[N];
		Point2			coords[N];

	public:
		Polygon_Template( )
		{
		};

		virtual ~Polygon_Template( )
		{
		};
	};

	////////////////////////////////////
	//
	// An indexed polygon
	//
	////////////////////////////////////
	template< int N >
	class IndexedPolygon_Template
	{
	public:
		//
		// Indexed polygons have their vertices, normals and such 
		// in other lists
		//
		unsigned int	iVertices[N];
		unsigned int	iNormals[N];
		unsigned int	iCoords[N];

	public:
		IndexedPolygon_Template( )
		{
		};

		virtual ~IndexedPolygon_Template( )
		{
		};
	};

	////////////////////////////////////
	//
	// An index polygon that uses a
	// pointer rather than an index
	//
	////////////////////////////////////
	template< int N >
	class PointerPolygon_Template
	{
	public:
		//
		// Indexed polygons have their vertices, normals and such 
		// in other lists
		//
		Point3*			pVertices[N];
		Vector3*		pNormals[N];
		Point2*			pCoords[N];

	public:
		PointerPolygon_Template( )
		{
	#ifdef _DEBUG
			for( int i=0; i<N; i++ )
			{
				pVertices[i] = 0;
				pNormals[i] = 0;
				pCoords[i] = 0;
			}
	#endif
		};

		virtual ~PointerPolygon_Template( )
		{
		};
	};

	////////////////////////////////////
	//
	// A bezier curve and bezier patch
	// definitions
	//
	////////////////////////////////////

	struct BezierCurve
	{
		Point3 pts[4];
	};

	struct BezierPatch
	{
		BezierCurve	c[4];
	};

	struct BilinearPatch
	{
		Point3 pts[4];
	};


	typedef Polygon_Template<3>			Triangle;
	typedef Polygon_Template<4>			Quad;
	typedef IndexedPolygon_Template<3>	IndexedTriangle;
	typedef IndexedPolygon_Template<4>	IndexedQuad;
	typedef PointerPolygon_Template<3>	PointerTriangle;
	typedef PointerPolygon_Template<4>	PointerQuad;

	typedef std::vector<Triangle>			TriangleListType;
	typedef std::vector<Quad>				QuadListType;
	typedef std::vector<IndexedTriangle>	IndexTriangleListType;
	typedef std::vector<IndexedQuad>		IndexQuadListType;
	typedef std::vector<PointerTriangle>	PointerTriangleListType;
	typedef std::vector<PointerQuad>		PointerQuadType;

	typedef Point3							Vertex;
	typedef Vector3							Normal;
	typedef Point2							TexCoord;

	typedef std::vector<Vertex>				VerticesListType;
	typedef std::vector<Normal>				NormalsListType;
	typedef std::vector<TexCoord>			TexCoordsListType;

	typedef std::vector<BezierPatch>		BezierPatchesListType;
}
#endif

