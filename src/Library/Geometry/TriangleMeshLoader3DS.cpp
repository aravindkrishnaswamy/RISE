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
#include <algorithm>
#include "../Utilities/LittleEndian.h"
#include <cstdint>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	bool ReadChunk( IReadBuffer& buffer, unsigned int start, unsigned int limit,
		unsigned short& id, unsigned int& end, bool allowTruncatedEdit = false )
	{
		if( start > limit || limit - start < 6 ||
			!BufferSeek( buffer, IBuffer::START, start ) ) return false;
		unsigned char header[6];
		if( !buffer.getBytes( header, sizeof(header) ) ) return false;
		id = DecodeLittleEndian<unsigned short, std::uint16_t>( header );
		const auto length = DecodeLittleEndian<unsigned int, std::uint32_t>( header + 2 );
		if( length < sizeof(header) ) return false;
		if( length > limit - start && !(allowTruncatedEdit && id == EDIT3DS) ) return false;
		end = start + std::min( length, limit - start );
		return true;
	}
}

TriangleMeshLoader3DS::TriangleMeshLoader3DS( IReadBuffer* pBuffer_ ) :
  nNumObjects( 0 ), pBuffer( pBuffer_ )
{
	if( !pBuffer || BufferSize(*pBuffer) == 0 ) {
		pBuffer = nullptr;
		return;
	}
	pBuffer->addref();
	if( BufferSize(*pBuffer) > std::numeric_limits<unsigned int>::max() ) {
		GlobalLog()->PrintSourceError( "TriangleMeshLoader3DS: Input exceeds the 32-bit 3DS file-size limit", __FILE__, __LINE__ );
		return;
	}

	unsigned char header[6];
	if( !pBuffer->seek( IBuffer::START, 0 ) || !pBuffer->getBytes( header, sizeof(header) ) ) return;
	unsigned short id = DecodeLittleEndian<unsigned short, std::uint16_t>( header );
	const auto declaredEnd = DecodeLittleEndian<unsigned int, std::uint32_t>( header + 2 );
	if( id != MAIN3DS || declaredEnd < sizeof(header) ) return;
	// MAIN3DS and EDIT3DS may include truncated metadata tails. Actual mesh
	// chunks must fit their containers and the available bytes.
	const unsigned int mainEnd = std::min( declaredEnd, pBuffer->Size() );
	unsigned int editStart = 0, editEnd = 0;
	for( unsigned int start = 6, end = 0; start < mainEnd; start = end ) {
		if( !ReadChunk( *pBuffer, start, mainEnd, id, end, true ) ) return;
		if( id == EDIT3DS ) {
			editStart = start;
			editEnd = end;
			break;
		}
	}
	if( editStart == 0 ) return;

	// Publish each discovered mesh immediately, so malformed trailing metadata
	// cannot discard earlier objects. LoadTriangleMesh still validates all mesh data.
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
				objptrs.push_back( child );
				++nNumObjects;
				break;
			}
		}
	}
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
		if( !GetVertices( mesh.vertices, i ) ) return false;
		if( mesh.vertices.empty() ) {
			meshes.pop_back();
			continue;
		}
		if( !GetFaces( mesh.triangles, i ) ) return false;
		if( mesh.triangles.empty() ) {
			meshes.pop_back();
			continue;
		}
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
