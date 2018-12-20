//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderPSurf.cpp - Implements the surface loader
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshLoaderPSurf.h"
#include "GeometryUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoaderPSurf::TriangleMeshLoaderPSurf( IParametricSurface* pSurface_, const unsigned int detail_u, const unsigned int detail_v ) : 
  pSurface( pSurface_ ),
  nDetailU( detail_u ),
  nDetailV( detail_v )
{
	if( pSurface ) {
		pSurface->addref();
	}
}

TriangleMeshLoaderPSurf::~TriangleMeshLoaderPSurf()
{
	safe_release( pSurface );
}

bool TriangleMeshLoaderPSurf::LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom )
{
	if( !pSurface ) {
		return false;
	}

	VerticesListType		vertices;
	NormalsListType			normals;
	TexCoordsListType		coords;
	IndexTriangleListType	triangles;

	Scalar u_start = 0, u_end = 0, v_start = 0, v_end = 0;

	pSurface->GetRange( u_start, u_end, v_start, v_end );

	GenerateGrid( nDetailU, nDetailV, u_start, v_start, u_end, v_end, vertices, normals, coords, triangles );

	for( unsigned int i = 0; i < nDetailV + 1; i++ ) {
		for( unsigned int j = 0; j < nDetailU + 1; j++ ) {
			const Scalar u = vertices[i*(nDetailU+1)+j].x;
			const Scalar v = vertices[i*(nDetailU+1)+j].y;

			pSurface->Evaluate( vertices[i*(nDetailU+1)+j], u, v );
		}
	}

	normals.clear();
	CalculateVertexNormals( triangles, normals, vertices );

	// Now pump everything to the triangle mesh geometry
	pGeom->BeginIndexedTriangles();
	pGeom->AddVertices( vertices );
	pGeom->AddNormals( normals );
	pGeom->AddTexCoords( coords );
	pGeom->AddIndexedTriangles( triangles );
	pGeom->DoneIndexedTriangles();

	return true;
}