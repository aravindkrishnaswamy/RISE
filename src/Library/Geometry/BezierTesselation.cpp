//////////////////////////////////////////////////////////////////////
//
//  BezierTeseslation.cpp - Implements bezier tesselation functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BezierTesselation.h"
#include "GeometryUtilities.h"

namespace RISE
{

	void GeneratePointsFromBezierCurve( VerticesListType& dest, const BezierCurve& curve, const unsigned int nDetail )
	{
		const Point3&	p1 = curve.pts[0];
		const Point3&	p2 = curve.pts[1];
		const Point3&	p3 = curve.pts[2];
		const Point3&	p4 = curve.pts[3];

		const Scalar deltastep = 1.0 / (Scalar)nDetail;
		const Scalar deltastep2 = deltastep * deltastep;
		const Scalar deltastep3 = deltastep2 * deltastep;

		const Vector3 vA = Vector3Ops::mkVector3( p4, p1 ) + 3.0 * Vector3Ops::mkVector3( p2, p3 );
		const Vector3 vB = (Vector3Ops::mkVector3( p1, p2 ) + Vector3Ops::mkVector3( p3, p2 )) * 3.0;
		const Vector3 vC = Vector3Ops::mkVector3( p2, p1 ) * 3.0;

		Vector3 vDelta = (vA*deltastep3) + (vB*deltastep2) + (vC*deltastep);
		Vector3 vDelta2 = (vA*(deltastep3*6.0)) + (vB*(deltastep2*2.0));
		Vector3 vDelta3 = vA*(deltastep3*6.0);

		Point3 pt1 = p1;
		
		// Insert the first control point into the vector
		dest.push_back( pt1 );
		
		for( unsigned int i=1; i<nDetail+1; i++ )
		{
			Point3 pt2 = Point3Ops::mkPoint3(pt1, vDelta);
			vDelta = vDelta + vDelta2;
			vDelta2 = vDelta2 + vDelta3;

			dest.push_back( pt2 );
			
			pt1 = pt2;
		}
	}

	void GeneratePointsFromBezierPatch( VerticesListType& dest, const BezierPatch& patch, const unsigned int nDetail )
	{
		VerticesListType	tempA, tempB, tempC, tempD;

		GeneratePointsFromBezierCurve( tempA, patch.c[0], nDetail );
		GeneratePointsFromBezierCurve( tempB, patch.c[1], nDetail );
		GeneratePointsFromBezierCurve( tempC, patch.c[2], nDetail );
		GeneratePointsFromBezierCurve( tempD, patch.c[3], nDetail );

		for( unsigned int i=0; i<nDetail+1; i++ )
		{
			BezierCurve	tempCurve;
			tempCurve.pts[0] = tempA[i];
			tempCurve.pts[1] = tempB[i];
			tempCurve.pts[2] = tempC[i];
			tempCurve.pts[3] = tempD[i];
			GeneratePointsFromBezierCurve( dest, tempCurve, nDetail );
		}
	}

	void GeneratePolygonsFromBezierPatch( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const BezierPatch& patch, const unsigned int nDetail )
	{
		unsigned int i, j;

		// First generate all the points
		GeneratePointsFromBezierPatch( vertices, patch, nDetail );

		// Generate a nice set of texture co-ordinates
		for( i = 0; i < nDetail+1; i++ ) {
			for( j = 0; j < nDetail+1; j++ ) {
				coords.push_back( Point2( (1.0-1.0/Scalar(nDetail)*Scalar(i) ),  (1.0/Scalar(nDetail)*Scalar(j)) ) );
			}
		}

		// Generate the list of indexed triangles
		for( i = 0; i < nDetail; i++ ) {
			for( j = 0; j < nDetail; j++ ) {
				// First triangle
				IndexedTriangle	face;

				face.iVertices[2] = ( i * (nDetail+1) + j );
				face.iVertices[1] = ( (i+1) * (nDetail+1) + j );
				face.iVertices[0] = ( i * (nDetail+1) + j+1 );
				face.iCoords[0] = face.iNormals[0] = face.iVertices[0];
				face.iCoords[1] = face.iNormals[1] = face.iVertices[1];
				face.iCoords[2] = face.iNormals[2] = face.iVertices[2];
				tris.push_back( face );

				face.iVertices[2] = ( (i+1) * (nDetail+1) + j );
				face.iVertices[1] = ( (i+1) * (nDetail+1) + j+1 );
				face.iVertices[0] = ( i * (nDetail+1) + j+1 );
				face.iCoords[0] = face.iNormals[0] = face.iVertices[0];
				face.iCoords[1] = face.iNormals[1] = face.iVertices[1];
				face.iCoords[2] = face.iNormals[2] = face.iVertices[2];
				tris.push_back( face );
			}
		}
	}

	void GeneratePolygonsFromBezierPatches( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, BezierPatchesListType& patches, const unsigned int nDetail )
	{
		// First generate all the points from all the patches
		unsigned int		nNumPatches = patches.size();
		unsigned int		m, i, j;

		for( m = 0; m < nNumPatches; m++ )
		{
			int		nVertexOffset = (nDetail+1)*(nDetail+1)*m;

			GeneratePointsFromBezierPatch( vertices, patches[m], nDetail );

			for( i = 0; i < nDetail+1; i++ ) {
				for( j = 0; j < nDetail+1; j++ ) {
					coords.push_back( Point2( (1.0-1.0/Scalar(nDetail)*Scalar(i) ),  (1.0/Scalar(nDetail)*Scalar(j)) ) );
				}
			}

			
			for( i = 0; i < nDetail; i++ ) {
				for( j = 0; j < nDetail; j++ ) {
					// First triangle
					IndexedTriangle	face;

					face.iVertices[2] = ( i * (nDetail+1) + j ) + nVertexOffset;
					face.iVertices[1] = ( (i+1) * (nDetail+1) + j ) + nVertexOffset;
					face.iVertices[0] = ( i * (nDetail+1) + j+1 ) + nVertexOffset;
					face.iCoords[0] = face.iNormals[0] = face.iVertices[0];
					face.iCoords[1] = face.iNormals[1] = face.iVertices[1];
					face.iCoords[2] = face.iNormals[2] = face.iVertices[2];
					tris.push_back( face );

					face.iVertices[2] = ( (i+1) * (nDetail+1) + j ) + nVertexOffset;
					face.iVertices[1] = ( (i+1) * (nDetail+1) + j+1 ) + nVertexOffset;
					face.iVertices[0] = ( i * (nDetail+1) + j+1 ) + nVertexOffset;
					face.iCoords[0] = face.iNormals[0] = face.iVertices[0];
					face.iCoords[1] = face.iNormals[1] = face.iVertices[1];
					face.iCoords[2] = face.iNormals[2] = face.iVertices[2];
					tris.push_back( face );
				}
			}
		}
	}

	void GeneratePolygonsFromBezierPatch( TriangleListType& tris, const BezierPatch& patch, const unsigned int nDetail )
	{
		// Compute the indexed triangles, then break them up
		IndexTriangleListType			indtris;
		VerticesListType				vertices;
		NormalsListType					normals;
		TexCoordsListType				coords;

		GeneratePolygonsFromBezierPatch( indtris, vertices, normals, coords, patch, nDetail );

		// Now break it up!
		IndexTriangleListType::const_iterator	i, e;
		for( i=indtris.begin(), e=indtris.end(); i!=e; i++ )
		{
			const IndexedTriangle& itri = (*i);
			Triangle	tri;

			for( int j=0; j<3; j++ ) {
				tri.vertices[j] = vertices[ itri.iVertices[j] ];
				tri.normals[j] = normals[ itri.iNormals[j] ];
				tri.coords[j] = coords[ itri.iCoords[j] ];
			}

			tris.push_back( tri );
		}
	}
}

