//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderBezier.cpp - Implementation of the bezier mesh
//  loader
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 7, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshLoaderBezier.h"
#include "BezierTesselation.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MediaPathLocator.h"
#include <stdio.h>

using namespace RISE;
using namespace RISE::Implementation;

TriangleMeshLoaderBezier::TriangleMeshLoaderBezier(
	const char * szFile, 
	const unsigned int detail,
	const bool bCombineSharedVertices_,
	const bool bCenterObject_,
	const IFunction2D* displacement_,
	Scalar disp_scale_
	) : 
  nDetail( detail ),
  bCombineSharedVertices( bCombineSharedVertices_ ),
  bCenterObject( bCenterObject_ ),
  displacement( displacement_ ),
  disp_scale( disp_scale_ )
{
	strncpy( szFilename, GlobalMediaPathLocator().Find(szFile).c_str(), 256 );
}

TriangleMeshLoaderBezier::~TriangleMeshLoaderBezier( )
{
}

bool TriangleMeshLoaderBezier::LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom )
{
	FILE* inputFile = fopen( szFilename, "r" );

	if( !inputFile || !pGeom ) {
		GlobalLog()->Print( eLog_Error, "TriangleMeshLoaderBezier:: Failed to open file or bad geometry object" );
		return false;
	}

	pGeom->BeginIndexedTriangles();

	char line[4096];
	
	if( fgets( (char*)&line, 4096, inputFile ) != NULL )
	{
		// Read that first line, it tells us how many
		// patches are dealing with here
		unsigned int	numPatches = 0;
		sscanf( line, "%u", &numPatches );

		BezierPatchesListType	patches;

		for( unsigned int i=0; i<numPatches; i++ )
		{
			// We assume every 16 lines gives us a patch
			BezierPatch		patch;

			for( int j=0; j<4; j++ ) {
				for( int k=0; k<4; k++ ) {
					double x, y, z;
					if( fscanf( inputFile, "%lf %lf %lf", &x, &y, &z ) == EOF ) {
						GlobalLog()->PrintSourceError( "TriangleMeshLoaderBezier:: Fatal error while reading file.  Nothing will be loaded", __FILE__, __LINE__ );
						return false;
					}

					patch.c[j].pts[k] = Point3( x, y, z );
				}
			}

			patches.push_back( patch );
		}


		GlobalLog()->PrintEx( eLog_Event, "TriangleMeshLoaderBezier:: Tesselating %u bezier patches...", numPatches );

		// Now tesselate all the patches together and then add them to 
		// the geometry object
		IndexTriangleListType			indtris;
		VerticesListType				vertices;
		NormalsListType					normals;
		TexCoordsListType				coords;

		GeneratePolygonsFromBezierPatches( indtris, vertices, normals, coords, patches, nDetail );
		if( bCombineSharedVertices ) {
			GlobalLog()->PrintEx( eLog_Event, "TriangleMeshLoaderBezier:: Attempting to combine shared vertices..." );			
			CombineSharedVerticesFromGrids( indtris, vertices, numPatches, nDetail, nDetail );
		}

		CalculateVertexNormals( indtris, normals, vertices );

		if( bCenterObject ) {
			CenterObject( vertices );
		}

		if( displacement ) {
			RemapTextureCoords( coords );
			ApplyDisplacementMapToObject( indtris, vertices, normals, coords, *displacement, disp_scale );

			// After applying displacement, recalculate the vertex normals
			normals.clear();
			CalculateVertexNormals( indtris, normals, vertices );
		}

		pGeom->AddVertices( vertices );
		pGeom->AddNormals( normals );
		pGeom->AddTexCoords( coords );
		pGeom->AddIndexedTriangles( indtris );

		GlobalLog()->PrintEx( eLog_Event, "TriangleMeshGeometryIndexed:: Constructing acceleration structures for %u triangles", indtris.size() );
	}	

	pGeom->DoneIndexedTriangles();

	fclose( inputFile );

	return true;
}

