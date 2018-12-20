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
#include <stdio.h>

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoaderRAW::TriangleMeshLoaderRAW( const char * szFile )
{
	strncpy( szFilename, GlobalMediaPathLocator().Find(szFile).c_str(), 256 );
}

TriangleMeshLoaderRAW::~TriangleMeshLoaderRAW( )
{
}

bool TriangleMeshLoaderRAW::LoadTriangleMesh( ITriangleMeshGeometry* pGeom )
{
	FILE* inputFile = fopen( szFilename, "r" );

	if( !inputFile || !pGeom ) {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderRAW:: Failed to open file or bad geometry object" );
		return false;
	}

	pGeom->BeginTriangles();

	char line[4096];
	char temp[32];

	while( fgets( (char*)&line, 4096, inputFile ) != NULL )
	{
		sscanf( line, "%s", temp );

		// What kind of polygon do we have ?
		if( strcmp( temp, "tri" ) == 0 )
		{
			// We have a triangle, so read the three vertices!
			double vAx, vAy, vAz, vBx, vBy, vBz, vCx, vCy, vCz;
			sscanf( line, "%s %lf %lf %lf %lf %lf %lf %lf %lf %lf", temp, &vAx, &vAy, &vAz, &vBx, &vBy, &vBz, &vCx, &vCy, &vCz );

			// Compute the normals ourselves
			// Only use face normals
			Triangle	tri;
			tri.vertices[0] = Point3( vAx, vAy, vAz );
			tri.vertices[1] = Point3( vBx, vBy, vBz );
			tri.vertices[2] = Point3( vCx, vCy, vCz );

			tri.normals[0] = Vector3Ops::Normalize(Vector3Ops::Cross( 
				Vector3Ops::mkVector3(tri.vertices[1], tri.vertices[0]),
				Vector3Ops::mkVector3(tri.vertices[2], tri.vertices[0]) ));
			tri.normals[1] = tri.normals[2] = tri.normals[0];
			tri.coords[0] = Point2( 0,0 );
			tri.coords[1] = Point2( 0,1 );
			tri.coords[2] = Point2( 1,1 );

			pGeom->AddTriangle( tri );
		}
		else if( strcmp( temp, "quad" ) == 0 )
		{
			// We have a quad, so read the four vertices
			double vAx, vAy, vAz, vBx, vBy, vBz, vCx, vCy, vCz, vDx, vDy, vDz;
			sscanf( line, "%s %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", temp, &vAx, &vAy, &vAz, &vBx, &vBy, &vBz, &vCx, &vCy, &vCz, &vDx, &vDy, &vDz );

			// Compute the normals ourselves and add two triangles
			Triangle	triA;
			triA.vertices[0] = Point3( vAx, vAy, vAz );
			triA.vertices[1] = Point3( vBx, vBy, vBz );
			triA.vertices[2] = Point3( vCx, vCy, vCz );

			Triangle	triB;
			triB.vertices[0] = Point3( vAx, vAy, vAz );
			triB.vertices[1] = Point3( vCx, vCy, vCz );
			triB.vertices[2] = Point3( vDx, vDy, vDz );

			Vector3 vNormal = Vector3Ops::Normalize(Vector3Ops::Cross( 
				Vector3Ops::mkVector3(triA.vertices[1], triA.vertices[0]),
				Vector3Ops::mkVector3(triA.vertices[2], triA.vertices[0]) ));

			triA.normals[0] = triA.normals[1] = triA.normals[2] = triB.normals[0] = triB.normals[1] = triB.normals[2] = vNormal;

			// Texture co-ordinates
			triA.coords[0] = Point2( 1.0, 0.0 );
			triA.coords[1] = Point2( 0.0, 0.0 );
			triA.coords[2] = Point2( 0.0, 1.0 );

			triB.coords[0] = Point2( 1.0, 0.0 );
			triB.coords[1] = Point2( 0.0, 1.0 );
			triB.coords[2] = Point2( 1.0, 1.0 );

			pGeom->AddTriangle( triA );
			pGeom->AddTriangle( triB );
		}
	}

	pGeom->DoneTriangles();

	fclose( inputFile );
	
	return true;
}

