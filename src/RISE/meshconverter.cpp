//////////////////////////////////////////////////////////////////////
//
//  meshconverter.cpp - Converts an input mesh (.3ds or .ply) into a
//    .risemesh file with a baked BVH cache.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//  Tier A2/A3 sweep (2026-04-27): the legacy bsp/maxpolys/maxdepth CLI
//  arguments are gone — BVH is the sole acceleration structure and its
//  parameters are not user-tunable from this tool.  Today's CLI:
//      meshconverter <in.3ds|in.ply> <out.risemesh> <invert_faces 0|1> <face_normals 0|1>
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include "../Library/RISE_API.h"
#include <string.h>

using namespace RISE;

int main( int argc, char** argv )
{
	if( argc < 5 ) {
		std::cout << "Usage: meshconverter <input mesh> <output .risemesh file> <invert faces [0|1]> <use face normals [0|1]>" << std::endl;
		std::cout << "Example: meshconverter in.3ds out.risemesh 0 0" << std::endl;
		return 1;
	}

	const bool bInvertFaces = !!atoi( argv[3] );
	const bool bFaceNormals = !!atoi( argv[4] );

	// Try and load the input mesh file (assume 3DS mesh for now)
	IMemoryBuffer*				pReadBuffer = 0;
	ITriangleMeshLoaderIndexed*	loader = 0;

	// Figure out what loader to use, depending on the extension of the file
	char* three_letter_extension = &((argv[1])[strlen(argv[1])-4]);
#ifdef strlwr
	if( strcmp( strlwr(three_letter_extension), ".3ds" ) == 0 ) {
#else
	if( strcmp( three_letter_extension, ".3ds" ) == 0 ) {
#endif
		std::cout << "Using 3DS mesh loader" << std::endl;
		RISE_API_CreateMemoryBufferFromFile( &pReadBuffer, argv[1] );
		RISE_API_Create3DSTriangleMeshLoader( &loader, pReadBuffer );
#ifdef strlwr
	} else if( strcmp( strlwr(three_letter_extension), ".ply" ) == 0 ) {
#else
	} else if( strcmp( three_letter_extension, ".ply" ) == 0 ) {
#endif
		std::cout << "Using PLY mesh loader" << std::endl;
		RISE_API_CreatePLYTriangleMeshLoader( &loader, argv[1], bInvertFaces );
	} else {
		std::cout << "Unknown extension: " << three_letter_extension << " on file, must be either .3ds or .ply" << std::endl;
		if( pReadBuffer ) {
			pReadBuffer->release();
		}
		return 1;
	}

	ITriangleMeshGeometryIndexed*		geom = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &geom, /*double_sided=*/false, bFaceNormals );

	loader->LoadTriangleMesh( geom );

	if( pReadBuffer ) {
		pReadBuffer->release();
	}

	// Then dump the geometry out
	IWriteBuffer*		pBuffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &pBuffer, argv[2] );
	geom->Serialize( *pBuffer );
	pBuffer->release();
	geom->release();
	if( loader ) {
		loader->release();
	}
	return 0;
}
