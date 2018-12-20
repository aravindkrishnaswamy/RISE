//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoader3DS.cpp - Implements the 3DS triangle mesh
//  loader
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2002
//  Tabs: 4
//  Comments: MAJOR CAVEAT!!!!  I only read vertices and faces, I don't
//  pay attention to anything else, not even the transformation matrices
//  thus if you are not really careful, you may not get what you expect
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshLoader3DS.h"
#include "3DS_Defines.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include <stdio.h>
#include "../Utilities/DiskFileReadBuffer.h"

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoader3DS::TriangleMeshLoader3DS( IReadBuffer* pBuffer_ ) :
  nNumObjects( 0 ), pBuffer( pBuffer_ )
{
	if( pBuffer->Size() > 0 ) {
		pBuffer->addref();
	} else {
		pBuffer = 0;
	}

	if( pBuffer )
	{
		//
		// Now do some preprocessing count the number of
		// valid objects as well storing pointers to those 
		// valid objects...
		//
		unsigned short id = pBuffer->getUWord( );
		if( id != MAIN3DS )					// bad id...
			return;

		// Now lets check the version number...
		// Seek to byte 28...
		pBuffer->seek( IBuffer::START, 28 );

		unsigned char version = pBuffer->getUChar();

		if( version < 3) {					// we only support version 3 or greater
			return;
		}

		pBuffer->seek( IBuffer::START, 2 );
		
		// Now we have to get to the begining of the EDIT3DS chunk
		unsigned int off = 0L, next = 0L;
		id = 0;
		while( id != EDIT3DS ) {
			id = pBuffer->getUWord();
		}

		unsigned int chunkend = pBuffer->getUInt() + pBuffer->getCurPos() - 6;			// read the offset to the end of the chunk

		// Now we sit tight and check the id's of all the subchunks
		// Now we *should* be at the first chunk...
		while( pBuffer->getCurPos() < chunkend )
		{
			id = pBuffer->getUWord();
			off = pBuffer->getUInt() - 6;	// offset to the next object
			next = pBuffer->getCurPos() + off;	
			if( id == EDIT_OBJECT )
			{
				unsigned int objchunkend = pBuffer->getCurPos() + off;
				//oooh... an object, first check the object's name to make sure it ain't a dummy object
				if( ReadName( ) != -1 )
				{
					// Lets check if we are already there ( at OBJ_TRIMESH )
					id = pBuffer->getUWord();
					if( id != OBJ_TRIMESH )
					{
						off = pBuffer->getUInt();
						pBuffer->seek( IBuffer::CUR, off-6 );
						// Now we seach for OBJ_TRIMESH, but make sure we don't go past this object's end
						while( pBuffer->getCurPos() < objchunkend )
						{
							id = pBuffer->getUWord();
							if( id == OBJ_TRIMESH )
							{
								// ooohh... we found a mesh... update some stuff...
								// First comes the object pointer...
								objptrs.push_back( pBuffer->getCurPos() - 2 );
								nNumObjects++;
								break;	// we are done, so break...
							}
							else
							{
								off = pBuffer->getUInt();
								pBuffer->seek( IBuffer::CUR, off-6 );
							}
						}
					}
					else
					{
						// ooohh... we found a mesh... update some stuff...
						// First comes the object pointer...
						objptrs.push_back( pBuffer->getCurPos()-2 );
						nNumObjects++;
					}
				}
			}
			pBuffer->seek( IBuffer::START, next );
		}
	} else {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoader3DS:: Failed to load file" );
	}
}

TriangleMeshLoader3DS::~TriangleMeshLoader3DS( )
{
	safe_release( pBuffer );
}

bool TriangleMeshLoader3DS::LoadTriangleMesh_GoodObjNum( ITriangleMeshGeometryIndexed* pGeom, int obj, bool bRecenter )
{
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;
	IndexTriangleListType tris;

	bool	bValidCoords = false;

	if( !GetVertices( vertices, obj ) ) {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoader3DS:: Failed to read vertices" );
		return false;
	}

	if( !GetFaces( tris, obj ) ) {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoader3DS:: Failed to read polygons" );
		return false;
	}

	if( !CalculateVertexNormals( tris, normals, vertices ) ) {
		return false;
	}

	bValidCoords = GetCoords( coords, obj );	

	if( !bValidCoords ) {
		GlobalLog()->Print( eLog_Warning, "TriangleMeshLoader3DS:: No texture co-ordinates in mesh" );
	}


	// Recenter the object if we need to
	if( bRecenter ) {
		CenterObject( vertices );
	}

	// Insert points
	unsigned int nPtStart = pGeom->numPoints();
	unsigned int nNormStart = pGeom->numNormals();
	unsigned int nCoordStart = pGeom->numCoords();

	pGeom->AddVertices( vertices );
	pGeom->AddNormals( normals );

	if( bValidCoords ) {
		pGeom->AddTexCoords( coords );
	} else {
		pGeom->AddTexCoord( TexCoord( 0, 0 ) );		// dummy co-ordinate
	}

	IndexTriangleListType::iterator		i, e;

	for( i=tris.begin(), e=tris.end(); i!=e; i++ )
	{
		const IndexedTriangle&	thisTri = *i;
		IndexedTriangle	mytri;

		for( int j=0; j<3; j++ )
		{
			mytri.iVertices[j] = nPtStart + thisTri.iVertices[j];
			mytri.iNormals[j] = nNormStart + thisTri.iNormals[j];

			if( bValidCoords ) {
				mytri.iCoords[j] = nCoordStart + thisTri.iCoords[j];
			} else {
				mytri.iCoords[j] = nCoordStart;
			}
		}

		pGeom->AddIndexedTriangle( mytri );
	}

	return true;
}

bool TriangleMeshLoader3DS::LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom, int obj, bool bRecenter )
{
	// If obj is -1, then we load all the objects
	if( obj == -1 )
	{
		pGeom->BeginIndexedTriangles();
		for( unsigned int i=0; i<nNumObjects; i++ ) {
			if( !LoadTriangleMesh_GoodObjNum( pGeom, i, bRecenter ) ) {
				return false;
			}
		}
		pGeom->DoneIndexedTriangles();
		return true;
	}
	else
	{
		pGeom->BeginIndexedTriangles();
		bool bRet = LoadTriangleMesh_GoodObjNum( pGeom, obj, bRecenter );
		pGeom->DoneIndexedTriangles();

		return bRet;
	}
}

int TriangleMeshLoader3DS::ReadName( )
{
	char temp_name [100];
	unsigned int teller = 0;
	unsigned char letter;

	strcpy( temp_name,"Default name" );

	letter = pBuffer->getUChar();
	if ( letter == 0 ) {
		return( -1 ); // dummy object
	}

	temp_name[teller] = letter;
	teller++;

	do {
		letter = pBuffer->getUChar();
		temp_name[teller] = letter;
		teller++;
	} while ( ( letter != 0 ) && ( teller < 12 ) );

	temp_name[teller-1] = 0;
	return ( 0 );
}

unsigned int TriangleMeshLoader3DS::ReadVertices( VerticesListType& vec )
{
	// Read the vertices and toss them into the vector. 
	// First read the number of vertices...
	unsigned int numVerts = pBuffer->getUWord();
	Vertex v;

	for( unsigned int i = 0; i < numVerts; i++ ) {
		v.x = pBuffer->getFloat();
		v.y = pBuffer->getFloat();
		v.z = pBuffer->getFloat();
		vec.push_back( v );
	}
	return (12 * numVerts + 2);
}

bool TriangleMeshLoader3DS::GetVertices( VerticesListType& vec, unsigned int n )
{
	if( n >= nNumObjects ) {
		return false;
	}

	if (n >= objptrs.size()) {
		return false;
	}

	pBuffer->seek( IBuffer::START, objptrs[n] );		// seek to the object's position

	unsigned int id = pBuffer->getUWord();
	
	// We should be at OBJ_TRIMESH
	// So lets look for TRI_VERTEXL
	unsigned int off = pBuffer->getUInt() - 6;
	unsigned int thischunkend;
	unsigned int chunkend = pBuffer->getCurPos() + off;

	// loop until we find the vertex list
	while( pBuffer->getCurPos() < chunkend )
	{
		id = pBuffer->getUWord();
		off = pBuffer->getUInt() - 6;		// move current position to next chunk
		thischunkend = pBuffer->getCurPos() + off;
		if( id == TRI_VERTEXL )
		{
			if( ReadVertices( vec ) != off ) {	// make sure we read the required number of verts...
				return false;
			} else {
				return true;
			}
		} else {								// unknown chunk, so skip it!
			pBuffer->seek( IBuffer::START, thischunkend );
		}
	}
	return false;
}

unsigned int TriangleMeshLoader3DS::ReadCoords( TexCoordsListType& vec )
{
	// Read the number of vertices here...
	unsigned short numCoords = pBuffer->getUWord();
	Point2 v;

	for( unsigned int i = 0; i < numCoords; i++ ) {
		v.x = pBuffer->getFloat();
		v.y = 1.0f - pBuffer->getFloat();
		vec.push_back( v );
	}
	return (8 * numCoords + 2 );
}

bool TriangleMeshLoader3DS::GetCoords( TexCoordsListType& vec, unsigned int n )
{
	if( n >= nNumObjects ) {
		return false;
	}

	pBuffer->seek( IBuffer::START, objptrs[n] );		// seek to the object's position

	unsigned short id = pBuffer->getUWord();
	
	// We should be at OBJ_TRIMESH
	// So lets look for TRI_MAPPINGCOORS
	unsigned int off = pBuffer->getUInt() - 6;
	unsigned int thischunkend;
	unsigned int chunkend = pBuffer->getCurPos() + off;

	// loop until we find the vertex list
	while( pBuffer->getCurPos() < chunkend )
	{
		id = pBuffer->getUWord();
		off = pBuffer->getUInt() - 6;		// move current position to next chunk
		thischunkend = pBuffer->getCurPos() + off;
		if( id == TRI_MAPPINGCOORS ) {
			if( ReadCoords( vec ) != off ) {
				return false;
			} else {
				return true;
			}
		} else {							// unknown chunk, so skip it!
			pBuffer->seek( IBuffer::START, thischunkend );
		}
	}
	return false;
}

unsigned int TriangleMeshLoader3DS::ReadFaces( IndexTriangleListType& vec )
{
	// Read the number of faces..
	unsigned short numFaces = pBuffer->getUWord();
	unsigned int temp = 0;
	IndexedTriangle c;

	for( unsigned int i = 0; i < numFaces; i++ )
	{
		c.iVertices[0] = pBuffer->getUWord();
		c.iVertices[1] = pBuffer->getUWord();
		c.iVertices[2] = pBuffer->getUWord();
		// We are just going to skip over it for now (normal calculation help)
		pBuffer->seek( IBuffer::CUR, 2 );
		// Set the normal and tmap indices to the same as the vertex...
		c.iCoords[0] = c.iNormals[0] = c.iVertices[0];
		c.iCoords[1] = c.iNormals[1] = c.iVertices[1];
		c.iCoords[2] = c.iNormals[2] = c.iVertices[2];

		vec.push_back( c );
	}

	// Check for smoothing
	if( pBuffer->getUWord() == TRI_SMOOTH ) {
		// We have smoothing info, which we will just ignore...
		unsigned int ptr = pBuffer->getUInt();
		temp += ptr;
		pBuffer->seek( IBuffer::START, ptr-6 );
	} else {
		pBuffer->seek( IBuffer::CUR, -2 );			// go back
	}

	// Check for material info
	if( pBuffer->getUWord() == TRI_MATERIAL ) {
		// We have material info, which we will just ignore...
		unsigned int ptr = pBuffer->getUInt();
		temp += ptr;
		pBuffer->seek( IBuffer::START, ptr-6 );
	} else {
		pBuffer->seek( IBuffer::CUR, -2 );			// go back
	}
	
	return (8 * numFaces + 2 + temp);
}

bool TriangleMeshLoader3DS::GetFaces( IndexTriangleListType& vec, unsigned int n )
{
	if( n >= nNumObjects ) {
		return false;
	}

	pBuffer->seek( IBuffer::START, objptrs[n] );		// seek to the object's position

	unsigned short id = pBuffer->getUWord();
	
	// We should be at OBJ_TRIMESH
	// So lets look for TRI_FACEL1
	unsigned int off = pBuffer->getUInt() - 6;
	unsigned int thischunkend;
	unsigned int chunkend = pBuffer->getCurPos() + off;

	// loop until we find the face list
	while( pBuffer->getCurPos() < chunkend )
	{
		id = pBuffer->getUWord();
		off = pBuffer->getUInt() - 6;		// move current position to next chunk
		thischunkend = pBuffer->getCurPos() + off;
		if( id == TRI_FACEL1 ) {
			ReadFaces( vec );
			return true;
		} else {						// unknown chunk, so skip it!
			pBuffer->seek( IBuffer::START, thischunkend );
		}
	}
	return false;
}
