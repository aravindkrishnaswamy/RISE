//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderPLY.cpp - Implementation of the PLY mesh loader
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshLoaderPLY.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MediaPathLocator.h"
#include <stdio.h>

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoaderPLY::TriangleMeshLoaderPLY( const char * szFile, const bool bInvertFaces_ ) : 
  bInvertFaces( bInvertFaces_ )
{
	strncpy( szFilename, GlobalMediaPathLocator().Find(szFile).c_str(), 256 );
}

TriangleMeshLoaderPLY::~TriangleMeshLoaderPLY( )
{
}

bool TriangleMeshLoaderPLY::LoadAscii( ITriangleMeshGeometryIndexed* pGeom, FILE* inputFile, const unsigned int numVerts, const unsigned int numPolygons )
{
	char line[4096] = {0};

	// Read all the vertices
	unsigned int i=0;
	for( i=0; i<numVerts; i++ ) {
		if( fgets( (char*)&line, 4096, inputFile ) == NULL ) {
			GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: Failed to read line in the middle of reading the verticies" );
			return false;
		}

		Scalar x, y, z;
		if( sscanf( line, "%lf %lf %lf", &x, &y, &z ) != 3 ) {
			return false;
		}

		pGeom->AddVertex( Vertex( x, y, z ) );
	}

	// Read all the polygons
	for( i=0; i<numPolygons; i++ ) {
		if( fgets( (char*)&line, 4096, inputFile ) == NULL ) {
			GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: Failed to read line in the middle of reading the polygons" );
			return false;
		}

		unsigned int cnt=0;
		sscanf( line, "%u", &cnt );

		if( cnt == 3 ) {
			unsigned int a, b, c;
			sscanf( line, "%u %u %u %u", &cnt, &a, &b, &c );

			IndexedTriangle tri;
			if( bInvertFaces ) {
				tri.iVertices[0] = tri.iNormals[0] = a;
				tri.iVertices[1] = tri.iNormals[1] = c;
				tri.iVertices[2] = tri.iNormals[2] = b;
			} else {
				tri.iVertices[0] = tri.iNormals[0] = a;
				tri.iVertices[1] = tri.iNormals[1] = b;
				tri.iVertices[2] = tri.iNormals[2] = c;
			}
			tri.iCoords[0] = tri.iCoords[1] = tri.iCoords[2] = 0;

			pGeom->AddIndexedTriangle( tri );

		} else if( cnt == 4 ) {

			unsigned int a, b, c, d;
			if( sscanf( line, "%u %u %u %u %u", &cnt, &a, &b, &c, &d ) != 4 ) {
				return false;
			}

			IndexedTriangle triA;
			if( bInvertFaces ) {
				triA.iVertices[0] = triA.iNormals[0] = a;
				triA.iVertices[1] = triA.iNormals[1] = c;
				triA.iVertices[2] = triA.iNormals[2] = b;
			} else {
				triA.iVertices[0] = triA.iNormals[0] = a;
				triA.iVertices[1] = triA.iNormals[1] = b;
				triA.iVertices[2] = triA.iNormals[2] = c;
			}
			triA.iCoords[0] = triA.iCoords[1] = triA.iCoords[2] = 0;

			IndexedTriangle triB;
			if( bInvertFaces ) {
				triB.iVertices[0] = triB.iNormals[0] = a;
				triB.iVertices[1] = triB.iNormals[1] = d;
				triB.iVertices[2] = triB.iNormals[2] = c;
			} else {
				triB.iVertices[0] = triB.iNormals[0] = a;
				triB.iVertices[1] = triB.iNormals[1] = c;
				triB.iVertices[2] = triB.iNormals[2] = d;
			}
			triB.iCoords[0] = triB.iCoords[1] = triB.iCoords[2] = 0;

			pGeom->AddIndexedTriangle( triA );
			pGeom->AddIndexedTriangle( triB );

		} else {
			GlobalLog()->PrintEx( eLog_Error, "TriangleMeshLoaderPLY:: We only read triangles or quads, this file has a polygon with '%u' vertices", cnt );
			return false;
		}		
	}

	return true;
}

bool TriangleMeshLoaderPLY::LoadBinary( ITriangleMeshGeometryIndexed* pGeom, FILE* inputFile, const unsigned int numVerts, const unsigned int numPolygons, const bool bFlipEndianess )
{
	// Read all the vertices
	unsigned int i=0;
	for( i=0; i<numVerts; i++ ) {
		float x, y, z;

		if( fread( &x, sizeof(float), 1, inputFile ) != 1 ) return false;
		if( fread( &y, sizeof(float), 1, inputFile ) != 1 ) return false;
		if( fread( &z, sizeof(float), 1, inputFile ) != 1 ) return false;

		if( bFlipEndianess ) {
			FlipFloat( &x );
			FlipFloat( &y );
			FlipFloat( &z );
		}

		pGeom->AddVertex( Vertex( x, y, z ) );
	}

	// Read all the polygons
	for( i=0; i<numPolygons; i++ ) {
		unsigned char cnt=0;
		if( fread( &cnt, sizeof( unsigned char ), 1, inputFile ) != 1 ) return false;

		if( cnt == 3 ) {
			unsigned int a, b, c;
			if( fread( &a, sizeof( unsigned int ), 1, inputFile ) != 1 ) return false;
			if( fread( &b, sizeof( unsigned int ), 1, inputFile ) != 1 ) return false;
			if( fread( &c, sizeof( unsigned int ), 1, inputFile ) != 1 ) return false;

			if( bFlipEndianess ) {
				FlipUInt( &a );
				FlipUInt( &b );
				FlipUInt( &c );
			}

			IndexedTriangle tri;
			if( bInvertFaces ) {
				tri.iVertices[0] = tri.iNormals[0] = a;
				tri.iVertices[1] = tri.iNormals[1] = c;
				tri.iVertices[2] = tri.iNormals[2] = b;
			} else {
				tri.iVertices[0] = tri.iNormals[0] = a;
				tri.iVertices[1] = tri.iNormals[1] = b;
				tri.iVertices[2] = tri.iNormals[2] = c;
			}
			tri.iCoords[0] = tri.iCoords[1] = tri.iCoords[2] = 0;

			pGeom->AddIndexedTriangle( tri );

		} else if( cnt == 4 ) {

			unsigned int a, b, c, d;
			if( fread( &a, sizeof( unsigned int ), 1, inputFile ) != 1 ) return false;
			if( fread( &b, sizeof( unsigned int ), 1, inputFile ) != 1 ) return false;
			if( fread( &c, sizeof( unsigned int ), 1, inputFile ) != 1 ) return false;
			if( fread( &d, sizeof( unsigned int ), 1, inputFile ) != 1 ) return false;

			if( bFlipEndianess ) {
				FlipUInt( &a );
				FlipUInt( &b );
				FlipUInt( &c );
				FlipUInt( &d );
			}

			IndexedTriangle triA;
			if( bInvertFaces ) {
				triA.iVertices[0] = triA.iNormals[0] = a;
				triA.iVertices[1] = triA.iNormals[1] = c;
				triA.iVertices[2] = triA.iNormals[2] = b;
			} else {
				triA.iVertices[0] = triA.iNormals[0] = a;
				triA.iVertices[1] = triA.iNormals[1] = b;
				triA.iVertices[2] = triA.iNormals[2] = c;
			}
			triA.iCoords[0] = triA.iCoords[1] = triA.iCoords[2] = 0;

			IndexedTriangle triB;
			if( bInvertFaces ) {
				triB.iVertices[0] = triB.iNormals[0] = a;
				triB.iVertices[1] = triB.iNormals[1] = d;
				triB.iVertices[2] = triB.iNormals[2] = c;
			} else {
				triB.iVertices[0] = triB.iNormals[0] = a;
				triB.iVertices[1] = triB.iNormals[1] = c;
				triB.iVertices[2] = triB.iNormals[2] = d;
			}
			triB.iCoords[0] = triB.iCoords[1] = triB.iCoords[2] = 0;

			pGeom->AddIndexedTriangle( triA );
			pGeom->AddIndexedTriangle( triB );

		} else {
			GlobalLog()->PrintEx( eLog_Error, "TriangleMeshLoaderPLY:: We only read triangles or quads, this file has a polygon with '%u' vertices", cnt );
			return false;
		}		
	}

	return true;
}

bool TriangleMeshLoaderPLY::LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom )
{
	FILE* inputFile = fopen( szFilename, "rb" );

	if( !inputFile || !pGeom ) {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderPLY:: Failed to open file or bad geometry object" );
		return false;
	}

	// Lets surf through the basic stuff in the begining
	char line[4096] = {0};
	char temp[32] = {0};
	char temp2[32] = {0};

	unsigned int numVerts=0;
	unsigned int numPolygons=0;

	if( fgets( (char*)&line, 4096, inputFile ) == NULL ) {
		fclose( inputFile );
		return false;
	}

	// Check for the ply header
	if( strncmp( line, "ply", 3 ) != 0 ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: Failed to read ply header" );
		fclose( inputFile );
		return false;
	}

	bool bAscii = true;
	bool bBigEndian = false;

	while( fgets( (char*)&line, 4096, inputFile ) != NULL )
	{
		// Continue until we get to the list of data
		sscanf( line, "%s", temp );
		if( strcmp( temp, "element" ) == 0 ) {
			// Find out what kind of element
			sscanf( line, "%s %s", temp, temp2 );

			if( strcmp( temp2, "vertex" ) == 0 ) {
				// Get the vertex count
				sscanf( line, "%s %s %u", temp, temp2, &numVerts );
			} else if( strcmp( temp2, "face" ) == 0 ) {
				// Get the face count
				sscanf( line, "%s %s %u", temp, temp2, &numPolygons );
			} else {
				GlobalLog()->PrintEx( eLog_Warning, "TriangleMeshLoaderPLY:: Found unknown element type '%s'.  Don't be surprised if this fails", temp2 );
			}
		} if( strcmp( temp, "format" ) == 0 ) {
			// Find out what kind of format
			sscanf( line, "%s %s", temp, temp2 );

			if( strcmp( temp2, "ascii" ) == 0 ) {
				bAscii = true;
			} else if( strcmp( temp2, "binary_big_endian" ) == 0 ) {
				bAscii = false;
				bBigEndian = true;
			} else if( strcmp( temp2, "binary_little_endian" ) == 0 ) {
				bAscii = false;
				bBigEndian = false;
			}
		} else if( strcmp( temp, "end_header" ) == 0 ) {
			// Header is done get out of here
			break;
		}
	}

	if( !numVerts || !numPolygons ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: Failed to read vertices and face counts" );
		fclose( inputFile );
		return false;
	}

	pGeom->BeginIndexedTriangles();

	// There are no texture co-ordinates, so fake it
	pGeom->AddTexCoord( TexCoord( 0, 0 ) );

	bool bSuccess = false;

	if( bAscii ) {
		bSuccess = LoadAscii( pGeom, inputFile, numVerts, numPolygons );
	} else {
#ifdef RISE_BIG_ENDIAN
		bSuccess = LoadBinary( pGeom, inputFile, numVerts, numPolygons, !bBigEndian );
#else 
		bSuccess = LoadBinary( pGeom, inputFile, numVerts, numPolygons, bBigEndian );
#endif
	}

	fclose( inputFile );

	if( bSuccess ) {
		// We do the work of computing vertex normals
		pGeom->ComputeVertexNormals();
		pGeom->DoneIndexedTriangles();
		
		return true;
	}

	GlobalLog()->PrintEasyError( "TriangleMeshLoaderPLY:: Something went wrong while trying to read the data" );
	return false;
}

