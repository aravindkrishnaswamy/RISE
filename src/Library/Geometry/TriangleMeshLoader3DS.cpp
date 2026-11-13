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
#include <limits>
#include "../Utilities/DiskFileReadBuffer.h"

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoader3DS::TriangleMeshLoader3DS( IReadBuffer* pBuffer_ ) :
  nNumObjects( 0 ), pBuffer( pBuffer_ )
{
	if( pBuffer && pBuffer->Size() > 0 ) {
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
		const unsigned int fileSize = pBuffer->Size();
		id = 0;
		while( id != EDIT3DS && pBuffer->getCurPos() <= fileSize &&
			fileSize - pBuffer->getCurPos() >= 2 ) {
			id = pBuffer->getUWord();
		}
		if( id != EDIT3DS || pBuffer->getCurPos() > fileSize || fileSize - pBuffer->getCurPos() < 4 ) return;

		const unsigned int editStart = pBuffer->getCurPos() - 2;
		const unsigned int editLength = pBuffer->getUInt();
		if( editLength < 6 || editLength > fileSize - editStart ) return;
		const unsigned int editEnd = editStart + editLength;
		const auto seekTo = [this]( unsigned int position ) {
			return position <= static_cast<unsigned int>(std::numeric_limits<int>::max()) &&
				pBuffer->seek( IBuffer::START, static_cast<int>(position) );
		};
		std::vector<unsigned long> objects;
		for( unsigned int start = pBuffer->getCurPos(); start < editEnd; start = pBuffer->getCurPos() ) {
			if( editEnd - start < 6 ) return;
			id = pBuffer->getUWord();
			const unsigned int length = pBuffer->getUInt();
			if( length < 6 || length > editEnd - start ) return;
			const unsigned int end = start + length;
			if( id == EDIT_OBJECT ) {
				bool terminated = false;
				const unsigned int nameStart = pBuffer->getCurPos();
				while( pBuffer->getCurPos() < end ) {
					if( pBuffer->getChar() == '\0' ) {
						terminated = true;
						break;
					}
				}
				if( !terminated ) return;
				for( unsigned int child = pBuffer->getCurPos();
					child - nameStart > 1 && child < end; child = pBuffer->getCurPos() ) {
					if( end - child < 6 ) return;
					const auto childId = pBuffer->getUWord();
					const unsigned int childLength = pBuffer->getUInt();
					if( childLength < 6 || childLength > end - child ) return;
					if( childId == OBJ_TRIMESH ) {
						objects.push_back( child );
						break;
					}
					if( !seekTo( child + childLength ) ) return;
				}
			}
			if( !seekTo( end ) ) return;
		}
		objptrs.swap( objects );
		nNumObjects = static_cast<unsigned int>(objptrs.size());
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

	for( const auto& triangle : tris ) {
		for( const auto index : triangle.iVertices ) {
			if( index >= vertices.size() ) return false;
		}
	}

	if( !CalculateVertexNormals( tris, normals, vertices ) ) {
		return false;
	}

	bValidCoords = GetCoords( coords, obj ) && coords.size() >= vertices.size();

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
		if( nNumObjects == 0 ) return false;
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

bool TriangleMeshLoader3DS::FindMeshChunk( unsigned int object, unsigned short wanted, unsigned int& payloadSize )
{
	if( object >= objptrs.size() ||
		!pBuffer->seek( IBuffer::START, static_cast<int>(objptrs[object]) ) ) return false;

	const unsigned int start = pBuffer->getCurPos();
	if( pBuffer->getUWord() != OBJ_TRIMESH ) return false;
	const unsigned int length = pBuffer->getUInt();
	if( length < 6 || length > pBuffer->Size() - start ) return false;
	const unsigned int end = start + length;

	for( unsigned int child = pBuffer->getCurPos(); child < end; child = pBuffer->getCurPos() ) {
		if( end - child < 6 ) return false;
		const auto id = pBuffer->getUWord();
		const unsigned int childLength = pBuffer->getUInt();
		if( childLength < 6 || childLength > end - child ) return false;
		if( id == wanted ) {
			payloadSize = childLength - 6;
			return true;
		}
		const auto next = child + childLength;
		if( next > static_cast<unsigned int>(std::numeric_limits<int>::max()) ||
			!pBuffer->seek( IBuffer::START, static_cast<int>(next) ) ) return false;
	}
	return false;
}

bool TriangleMeshLoader3DS::GetVertices( VerticesListType& vec, unsigned int object )
{
	unsigned int size = 0;
	if( !FindMeshChunk( object, TRI_VERTEXL, size ) || size < 2 ) return false;
	const unsigned int count = pBuffer->getUWord();
	if( size - 2 != count * 12 ) return false;
	vec.reserve( count );
	for( unsigned int i = 0; i < count; ++i ) {
		Vertex v;
		v.x = pBuffer->getFloat();
		v.y = pBuffer->getFloat();
		v.z = pBuffer->getFloat();
		vec.push_back( v );
	}
	return true;
}

bool TriangleMeshLoader3DS::GetCoords( TexCoordsListType& vec, unsigned int object )
{
	unsigned int size = 0;
	if( !FindMeshChunk( object, TRI_MAPPINGCOORS, size ) || size < 2 ) return false;
	const unsigned int count = pBuffer->getUWord();
	if( size - 2 != count * 8 ) return false;
	vec.reserve( count );
	for( unsigned int i = 0; i < count; ++i ) {
		Point2 v;
		v.x = pBuffer->getFloat();
		v.y = 1.0f - pBuffer->getFloat();
		vec.push_back( v );
	}
	return true;
}

bool TriangleMeshLoader3DS::GetFaces( IndexTriangleListType& vec, unsigned int object )
{
	unsigned int size = 0;
	if( !FindMeshChunk( object, TRI_FACEL1, size ) || size < 2 ) return false;
	const unsigned int count = pBuffer->getUWord();
	if( size - 2 < count * 8 ) return false;
	vec.reserve( count );
	for( unsigned int i = 0; i < count; ++i ) {
		IndexedTriangle tri;
		for( unsigned int vertex = 0; vertex < 3; ++vertex ) {
			const auto index = pBuffer->getUWord();
			tri.iVertices[vertex] = tri.iNormals[vertex] = tri.iCoords[vertex] = index;
		}
		pBuffer->getUWord();  // face flags
		vec.push_back( tri );
	}
	return true;
}
