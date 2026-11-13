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
#include "../Utilities/FiniteMath.h"
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoaderRAW2::TriangleMeshLoaderRAW2( const char * szFile )
{
	const String filename = GlobalMediaPathLocator().Find( szFile );
	if( std::strlen( filename.c_str() ) < sizeof(szFilename) ) {
		std::strcpy( szFilename, filename.c_str() );
	} else {
		szFilename[0] = '\0';
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW2:: File name is too long" );
	}
}

TriangleMeshLoaderRAW2::~TriangleMeshLoaderRAW2( )
{
}

bool TriangleMeshLoaderRAW2::LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom )
{
	std::ifstream input( szFilename );
	if( !input || !pGeom ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW2:: Failed to open file or bad geometry object" );
		return false;
	}

	const auto readCount = []( std::istringstream& row, unsigned int& count ) {
		std::string token;
		if( !(row >> token) ) return false;
		const char* begin = token.data();
		const char* end = begin + token.size();
		if( *begin == '+' ) ++begin;
		const auto parsed = std::from_chars( begin, end, count );
		return parsed.ec == std::errc{} && parsed.ptr == end;
	};
	const auto finishedRow = []( std::istringstream& row ) {
		std::string extra;
		return !(row >> extra) || extra[0] == '#';
	};
	std::string line;
	if( !std::getline( input, line ) ) {
		return false;
	}
	std::istringstream header( line );
	unsigned int numVerts, numTris;
	if( !readCount( header, numVerts ) || !readCount( header, numTris ) || !finishedRow( header ) ) {
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW2:: Invalid vertex and triangle counts" );
		return false;
	}

	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coordinates;
	std::vector<RISEPel> colors;
	IndexTriangleListType triangles;

	for( unsigned int i=0; i<numVerts; ++i ) {
		if( !std::getline( input, line ) ) return false;
		std::istringstream row( line );
		std::string kind, token;
		double values[11];
		size_t count = 0;
		if( !(row >> kind) || kind != "v" ) return false;
		while( row >> token && token[0] != '#' ) {
			if( count == 11 ) return false;
			char* end = nullptr;
			const double value = std::strtod( token.c_str(), &end );
			if( end != token.data() + token.size() || !IsFiniteDouble( value ) ) return false;
			values[count++] = value;
		}
		if( count != 8 && count != 11 ) {
			GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW2:: Invalid vertex record" );
			return false;
		}
		vertices.emplace_back( values[0], values[1], values[2] );
		normals.emplace_back( values[3], values[4], values[5] );
		coordinates.emplace_back( values[6], values[7] );
		if( count == 11 ) {
			colors.emplace_back( sRGBPel( values[8], values[9], values[10] ) );
		}
	}

	for( unsigned int i=0; i<numTris; ++i ) {
		if( !std::getline( input, line ) ) return false;
		std::istringstream row( line );
		std::string kind;
		IndexedTriangle triangle;
		if( !(row >> kind) || kind != "t" ) return false;
		for( unsigned int corner=0; corner<3; ++corner ) {
			unsigned int index;
			if( !readCount( row, index ) || index >= numVerts ) {
				GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW2:: Invalid triangle index" );
				return false;
			}
			triangle.iVertices[corner] = triangle.iNormals[corner] = triangle.iCoords[corner] = index;
		}
		if( !finishedRow( row ) ) return false;
		triangles.push_back( triangle );
	}

	while( std::getline( input, line ) ) {
		const auto first = line.find_first_not_of( " \t\r\f\v" );
		if( first != std::string::npos && line[first] != '#' ) {
			GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW2:: Extra records after declared triangles" );
			return false;
		}
	}
	if( input.bad() ) return false;

	pGeom->BeginIndexedTriangles();
	pGeom->AddVertices( vertices );
	pGeom->AddNormals( normals );
	pGeom->AddTexCoords( coordinates );
	if( !colors.empty() ) {
		auto* colored = dynamic_cast<ITriangleMeshGeometryIndexed2*>( pGeom );
		if( colors.size() != numVerts ) {
			GlobalLog()->PrintEasyWarning( "TriangleMeshLoaderRAW2:: file mixes colored and uncolored vertex lines — colors dropped" );
		} else if( colored ) {
			colored->AddColors( colors );
		} else {
			GlobalLog()->PrintEasyWarning( "TriangleMeshLoaderRAW2:: target geometry does not implement ITriangleMeshGeometryIndexed2 — colors dropped" );
		}
	}
	pGeom->AddIndexedTriangles( triangles );
	VerticesListType().swap( vertices );
	NormalsListType().swap( normals );
	TexCoordsListType().swap( coordinates );
	std::vector<RISEPel>().swap( colors );
	IndexTriangleListType().swap( triangles );
	pGeom->DoneIndexedTriangles();
	return true;
}
