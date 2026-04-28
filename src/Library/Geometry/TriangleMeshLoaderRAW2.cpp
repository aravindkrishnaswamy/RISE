//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderRAW2.cpp - Implementation of the RAW mesh loader
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 1, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshLoaderRAW2.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/ITriangleMeshGeometry.h"
#include "../Utilities/MediaPathLocator.h"
#include <stdio.h>

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoaderRAW2::TriangleMeshLoaderRAW2( const char * szFile )
{
	strncpy( szFilename, GlobalMediaPathLocator().Find(szFile).c_str(), 256 );
}

TriangleMeshLoaderRAW2::~TriangleMeshLoaderRAW2( )
{
}

bool TriangleMeshLoaderRAW2::LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom )
{
	FILE* inputFile = fopen( szFilename, "r" );

	if( !inputFile || !pGeom ) {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderRAW2:: Failed to open file or bad geometry object" );
		return false;
	}

	pGeom->BeginIndexedTriangles();

	// RAW2 vertex line is "v vX vY vZ nX nY nZ cX cY [r g b]".
	// The trailing R G B triple is optional and added 2026-04-28.
	// Older files (no colors) parse with 9 sscanf fields; new files
	// parse with 12.  Colors are buffered locally and attached to the
	// geometry after the loop iff every vertex supplied them — partial
	// coverage gets dropped with a warning to keep the color array
	// aligned with pPoints.
	ITriangleMeshGeometryIndexed2* pGeom2 = dynamic_cast<ITriangleMeshGeometryIndexed2*>( pGeom );
	std::vector<RISEPel> pendingColors;

	char line[4096];

	// First read how many vertices we have and how many polygons we have
	fgets( (char*)&line, 4096, inputFile );

	unsigned int numVerts, numTris;
	sscanf( line, "%u %u", &numVerts, &numTris );

	unsigned int i=0;
	// Read through all the vertices
	for( i=0; i<numVerts; i++ ) {
		if( !fgets( (char*)&line, 4096, inputFile ) ) {
			GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderRAW2:: Failed to read a line from the file" );
			return false;
		}

		char type;
		double vX, vY, vZ, nX, nY, nZ, cX, cY;
		double r = 0, g = 0, b = 0;
		const int n = sscanf( line, "%c %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
			&type, &vX, &vY, &vZ, &nX, &nY, &nZ, &cX, &cY, &r, &g, &b );

		if( type != 'v' || (n != 9 && n != 12) ) {
			GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderRAW2:: Expected a vertex but didn't get one" );
			return false;
		}

		pGeom->AddVertex( Vertex( vX, vY, vZ ) );
		pGeom->AddNormal( Normal( nX, nY, nZ ) );
		pGeom->AddTexCoord( TexCoord( cX, cY ) );

		if( n == 12 ) {
			// RAW2 colors are conventionally sRGB-encoded floats in
			// [0, 1] (matches the PLY uchar interpretation).
			pendingColors.push_back( RISEPel( sRGBPel( r, g, b ) ) );
		}
	}

	if( !pendingColors.empty() ) {
		if( pendingColors.size() == numVerts && pGeom2 ) {
			pGeom2->AddColors( pendingColors );
		} else if( pendingColors.size() != numVerts ) {
			GlobalLog()->PrintEasyWarning(
				"TriangleMeshLoaderRAW2:: file mixes colored and uncolored vertex lines — colors dropped" );
		} else if( !pGeom2 ) {
			GlobalLog()->PrintEasyWarning(
				"TriangleMeshLoaderRAW2:: target geometry does not implement ITriangleMeshGeometryIndexed2 — colors dropped" );
		}
	}

	// Read the triangles
	for( i=0; i<numTris; i++ ){
		if( !fgets( (char*)&line, 4096, inputFile ) ) {
			GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderRAW2:: Failed to read a line from the file" );
			return false;
		}

		char type;
		unsigned int a, b, c;
		sscanf( line, "%c %u %u %u", &type, &a, &b, &c );

		if( type != 't' ) {
			GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderRAW2:: Expected a triangle but didn't get one" );
			return false;
		}

		IndexedTriangle tri;
		tri.iVertices[0] = tri.iNormals[0] = tri.iCoords[0] = a;
		tri.iVertices[1] = tri.iNormals[1] = tri.iCoords[1] = b;
		tri.iVertices[2] = tri.iNormals[2] = tri.iCoords[2] = c;

		pGeom->AddIndexedTriangle( tri );
	}

	pGeom->DoneIndexedTriangles();

	fclose( inputFile );

	return true;
}

