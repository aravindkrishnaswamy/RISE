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
#include "../Utilities/LittleEndian.h"
#include <cstdint>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	bool ReadChunk( IReadBuffer& buffer, unsigned int start, unsigned int limit,
		unsigned short& id, unsigned int& end )
	{
		if( start > limit || limit - start < 6 ||
			start > static_cast<unsigned int>(std::numeric_limits<int>::max()) ||
			!buffer.seek( IBuffer::START, static_cast<int>(start) ) ) return false;
		unsigned char header[6];
		if( !buffer.getBytes( header, sizeof(header) ) ) return false;
		id = DecodeLittleEndian<unsigned short, std::uint16_t>( header );
		const auto length = DecodeLittleEndian<unsigned int, std::uint32_t>( header + 2 );
		if( length < sizeof(header) || length > limit - start ) return false;
		end = start + length;
		return true;
	}
}

TriangleMeshLoader3DS::TriangleMeshLoader3DS( IReadBuffer* pBuffer_ ) :
  nNumObjects( 0 ), pBuffer( pBuffer_ )
{
	if( !pBuffer || pBuffer->Size() == 0 ) {
		pBuffer = nullptr;
		return;
	}
	pBuffer->addref();

	unsigned short id = 0;
	unsigned int mainEnd = 0;
	if( !ReadChunk( *pBuffer, 0, pBuffer->Size(), id, mainEnd ) || id != MAIN3DS ) return;

	constexpr unsigned short kVersionChunk = 0x0002;
	bool supportedVersion = false;
	unsigned int editStart = 0, editEnd = 0;
	for( unsigned int start = 6, end = 0; start < mainEnd; start = end ) {
		if( !ReadChunk( *pBuffer, start, mainEnd, id, end ) ) return;
		if( id == kVersionChunk ) {
			unsigned char version[4];
			if( end - start < 10 || !pBuffer->getBytes( version, sizeof(version) ) ) return;
			supportedVersion = DecodeLittleEndian<unsigned int, std::uint32_t>( version ) >= 3;
		} else if( id == EDIT3DS && editStart == 0 ) {
			editStart = start;
			editEnd = end;
		}
	}
	if( !supportedVersion || editStart == 0 ) return;

	std::vector<unsigned long> objects;
	for( unsigned int start = editStart + 6, end = 0; start < editEnd; start = end ) {
		if( !ReadChunk( *pBuffer, start, editEnd, id, end ) ) return;
		if( id != EDIT_OBJECT ) continue;

		unsigned int child = start + 6;
		bool terminated = false;
		for( ; child < end; ++child ) {
			unsigned char character = 0;
			if( !pBuffer->getBytes( &character, 1 ) ) return;
			if( character == 0 ) {
				terminated = true;
				++child;
				break;
			}
		}
		if( !terminated ) return;
		if( child == start + 7 ) continue;  // dummy object with an empty name

		for( unsigned int childEnd = 0; child < end; child = childEnd ) {
			if( !ReadChunk( *pBuffer, child, end, id, childEnd ) ) return;
			if( id == OBJ_TRIMESH ) {
				objects.push_back( child );
				break;
			}
		}
	}
	objptrs.swap( objects );
	nNumObjects = static_cast<unsigned int>(objptrs.size());
}

TriangleMeshLoader3DS::~TriangleMeshLoader3DS( )
{
	safe_release( pBuffer );
}

bool TriangleMeshLoader3DS::LoadTriangleMesh( ITriangleMeshGeometryIndexed* geometry, int object, bool recenter )
{
	if( !geometry || nNumObjects == 0 || object < -1 ||
		(object >= 0 && static_cast<unsigned int>(object) >= nNumObjects) ) return false;

	struct Mesh {
		VerticesListType vertices;
		NormalsListType normals;
		TexCoordsListType coords;
		IndexTriangleListType triangles;
	};
	const unsigned int first = object < 0 ? 0 : static_cast<unsigned int>(object);
	const unsigned int last = object < 0 ? nNumObjects : first + 1;
	std::vector<Mesh> meshes;
	meshes.reserve( last - first );
	for( auto i = first; i < last; ++i ) {
		auto& mesh = meshes.emplace_back();
		if( !GetVertices( mesh.vertices, i ) || !GetFaces( mesh.triangles, i ) ||
			mesh.vertices.empty() || mesh.triangles.empty() ) return false;
		for( const auto& triangle : mesh.triangles ) {
			for( const auto index : triangle.iVertices ) {
				if( index >= mesh.vertices.size() ) return false;
			}
		}
		if( !CalculateVertexNormals( mesh.triangles, mesh.normals, mesh.vertices ) ) return false;
		if( !GetCoords( mesh.coords, i ) || mesh.coords.size() < mesh.vertices.size() ) {
			mesh.coords.clear();
			GlobalLog()->PrintEasyWarning( "TriangleMeshLoader3DS: No usable texture coordinates in mesh" );
		}
		if( recenter ) CenterObject( mesh.vertices );
	}

	geometry->BeginIndexedTriangles();
	for( auto& mesh : meshes ) {
		const auto vertexOffset = geometry->numPoints();
		const auto normalOffset = geometry->numNormals();
		const auto texcoordOffset = geometry->numCoords();
		const bool hasCoords = !mesh.coords.empty();
		geometry->AddVertices( mesh.vertices );
		geometry->AddNormals( mesh.normals );
		if( hasCoords ) geometry->AddTexCoords( mesh.coords );
		else geometry->AddTexCoord( TexCoord(0, 0) );

		for( auto& triangle : mesh.triangles ) {
			for( unsigned int i = 0; i < 3; ++i ) {
				triangle.iVertices[i] += vertexOffset;
				triangle.iNormals[i] += normalOffset;
				triangle.iCoords[i] = texcoordOffset + (hasCoords ? triangle.iCoords[i] : 0);
			}
			geometry->AddIndexedTriangle( triangle );
		}
	}
	geometry->DoneIndexedTriangles();
	return true;
}

bool TriangleMeshLoader3DS::FindMeshChunk( unsigned int object, unsigned short wanted, unsigned int& payloadSize )
{
	if( object >= objptrs.size() ) return false;
	const auto start = static_cast<unsigned int>(objptrs[object]);
	unsigned short id = 0;
	unsigned int end = 0;
	if( !ReadChunk( *pBuffer, start, pBuffer->Size(), id, end ) || id != OBJ_TRIMESH ) return false;

	for( unsigned int child = start + 6, next = 0; child < end; child = next ) {
		if( !ReadChunk( *pBuffer, child, end, id, next ) ) return false;
		if( id == wanted ) {
			payloadSize = next - child - 6;
			return true;
		}
	}
	return false;
}

bool TriangleMeshLoader3DS::GetVertices( VerticesListType& vec, unsigned int object )
{
	unsigned int size = 0;
	if( !FindMeshChunk( object, TRI_VERTEXL, size ) || size < 2 ) return false;
	unsigned char countBytes[2];
	if( !pBuffer->getBytes( countBytes, sizeof(countBytes) ) ) return false;
	const auto count = DecodeLittleEndian<unsigned short, std::uint16_t>( countBytes );
	if( size - 2 != count * 12 ) return false;
	vec.reserve( count );
	for( unsigned int i = 0; i < count; ++i ) {
		unsigned char record[12];
		if( !pBuffer->getBytes( record, sizeof(record) ) ) return false;
		vec.emplace_back( DecodeLittleEndian<float, std::uint32_t>(record),
			DecodeLittleEndian<float, std::uint32_t>(record + 4),
			DecodeLittleEndian<float, std::uint32_t>(record + 8) );
	}
	return true;
}

bool TriangleMeshLoader3DS::GetCoords( TexCoordsListType& vec, unsigned int object )
{
	unsigned int size = 0;
	if( !FindMeshChunk( object, TRI_MAPPINGCOORS, size ) || size < 2 ) return false;
	unsigned char countBytes[2];
	if( !pBuffer->getBytes( countBytes, sizeof(countBytes) ) ) return false;
	const auto count = DecodeLittleEndian<unsigned short, std::uint16_t>( countBytes );
	if( size - 2 != count * 8 ) return false;
	vec.reserve( count );
	for( unsigned int i = 0; i < count; ++i ) {
		unsigned char record[8];
		if( !pBuffer->getBytes( record, sizeof(record) ) ) return false;
		vec.emplace_back( DecodeLittleEndian<float, std::uint32_t>(record),
			1.0f - DecodeLittleEndian<float, std::uint32_t>(record + 4) );
	}
	return true;
}

bool TriangleMeshLoader3DS::GetFaces( IndexTriangleListType& vec, unsigned int object )
{
	unsigned int size = 0;
	if( !FindMeshChunk( object, TRI_FACEL1, size ) || size < 2 ) return false;
	unsigned char countBytes[2];
	if( !pBuffer->getBytes( countBytes, sizeof(countBytes) ) ) return false;
	const auto count = DecodeLittleEndian<unsigned short, std::uint16_t>( countBytes );
	if( size - 2 < count * 8 ) return false;
	vec.reserve( count );
	for( unsigned int i = 0; i < count; ++i ) {
		unsigned char record[8];
		if( !pBuffer->getBytes( record, sizeof(record) ) ) return false;
		IndexedTriangle tri;
		for( unsigned int vertex = 0; vertex < 3; ++vertex ) {
			const auto index = DecodeLittleEndian<unsigned short, std::uint16_t>(record + vertex*2);
			tri.iVertices[vertex] = tri.iNormals[vertex] = tri.iCoords[vertex] = index;
		}
		vec.push_back( tri );
	}
	return true;
}
