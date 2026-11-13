//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderRAW.cpp - Implementation of the RAW mesh loader
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
#include "TriangleMeshLoaderRAW.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MediaPathLocator.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "../Utilities/FiniteMath.h"

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoaderRAW::TriangleMeshLoaderRAW( const char * szFile )
{
	const String filename = GlobalMediaPathLocator().Find( szFile );
	if( std::strlen( filename.c_str() ) < sizeof(szFilename) ) {
		std::strcpy( szFilename, filename.c_str() );
	} else {
		szFilename[0] = '\0';
		GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW:: File name is too long" );
	}
}

TriangleMeshLoaderRAW::~TriangleMeshLoaderRAW( )
{
}

bool TriangleMeshLoaderRAW::LoadTriangleMesh( ITriangleMeshGeometry* pGeom )
{
	std::ifstream inputFile( szFilename );

	if( !inputFile || !pGeom ) {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderRAW:: Failed to open file or bad geometry object" );
		return false;
	}

	std::vector<Triangle> triangles;
	std::string line;

	while( std::getline( inputFile, line ) )
	{
		std::istringstream fields( line );
		std::string kind;
		fields >> kind;
		if( kind != "tri" && kind != "quad" ) {
			continue;
		}

		double vertices[12];
		const int coordinateCount = kind == "tri" ? 9 : 12;
		for( int coordinate = 0; coordinate < coordinateCount; ++coordinate ) {
			std::string token;
			fields >> token;
			char* end = nullptr;
			vertices[coordinate] = std::strtod( token.c_str(), &end );
			if( token.empty() || end != token.data() + token.size() || !IsFiniteDouble( vertices[coordinate] ) ) {
				GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW:: Invalid polygon coordinates" );
				return false;
			}
		}
		std::string trailing;
		if( (fields >> trailing) && trailing[0] != '#' ) {
			GlobalLog()->PrintEasyError( "TriangleMeshLoaderRAW:: Extra polygon coordinates" );
			return false;
		}

		// What kind of polygon do we have ?
		if( kind == "tri" )
		{
			// Compute the normals ourselves
			// Only use face normals
			Triangle	tri;
			tri.vertices[0] = Point3( vertices[0], vertices[1], vertices[2] );
			tri.vertices[1] = Point3( vertices[3], vertices[4], vertices[5] );
			tri.vertices[2] = Point3( vertices[6], vertices[7], vertices[8] );

			tri.normals[0] = Vector3Ops::Normalize(Vector3Ops::Cross( 
				Vector3Ops::mkVector3(tri.vertices[1], tri.vertices[0]),
				Vector3Ops::mkVector3(tri.vertices[2], tri.vertices[0]) ));
			tri.normals[1] = tri.normals[2] = tri.normals[0];
			tri.coords[0] = Point2( 0,0 );
			tri.coords[1] = Point2( 0,1 );
			tri.coords[2] = Point2( 1,1 );

			triangles.push_back( tri );
		}
		else
		{
			// Compute the normals ourselves and add two triangles
			Triangle	triA;
			triA.vertices[0] = Point3( vertices[0], vertices[1], vertices[2] );
			triA.vertices[1] = Point3( vertices[3], vertices[4], vertices[5] );
			triA.vertices[2] = Point3( vertices[6], vertices[7], vertices[8] );

			Triangle	triB;
			triB.vertices[0] = triA.vertices[0];
			triB.vertices[1] = triA.vertices[2];
			triB.vertices[2] = Point3( vertices[9], vertices[10], vertices[11] );

			Vector3 vNormalA = Vector3Ops::Normalize(Vector3Ops::Cross( 
				Vector3Ops::mkVector3(triA.vertices[1], triA.vertices[0]),
				Vector3Ops::mkVector3(triA.vertices[2], triA.vertices[0]) ));

			Vector3 vNormalB = Vector3Ops::Normalize(Vector3Ops::Cross( 
				Vector3Ops::mkVector3(triB.vertices[1], triB.vertices[0]),
				Vector3Ops::mkVector3(triB.vertices[2], triB.vertices[0]) ));

			triA.normals[0] = triA.normals[1] = triA.normals[2] = vNormalA;
			triB.normals[0] = triB.normals[1] = triB.normals[2] = vNormalB;

			// Texture co-ordinates
			triA.coords[0] = Point2( 1.0, 0.0 );
			triA.coords[1] = Point2( 0.0, 0.0 );
			triA.coords[2] = Point2( 0.0, 1.0 );

			triB.coords[0] = Point2( 1.0, 0.0 );
			triB.coords[1] = Point2( 0.0, 1.0 );
			triB.coords[2] = Point2( 1.0, 1.0 );

			triangles.push_back( triA );
			triangles.push_back( triB );
		}
	}

	if( inputFile.bad() ) {
		return false;
	}

	pGeom->BeginTriangles();
	for( const Triangle& triangle : triangles ) {
		pGeom->AddTriangle( triangle );
	}
	std::vector<Triangle>().swap( triangles );
	pGeom->DoneTriangles();
	
	return true;
}
