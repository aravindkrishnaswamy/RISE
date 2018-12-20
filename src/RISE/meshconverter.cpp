//////////////////////////////////////////////////////////////////////
//
//  meshconverter.cpp - This a problem that converts a given input
//    mesh into a TriangleMeshGeometry object with a nice octree
//    already built in
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include "../Library/RISE_API.h"
#include <string.h>

using namespace RISE;

int main( int argc, char** argv )
{
	if( argc < 8 ) {
		std::cout << "Usage: meshconverter <input mesh> <output .risemesh file> <use bsp [0|1]> <max polys/node> <max node levels> <invert faces [0|1]> <use face normals [0|1]" << std::endl;
		std::cout << "Example: meshconverter in.3ds out.risemesh 1 10 24 0 0" << std::endl;
		return 1;
	}

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
		RISE_API_CreatePLYTriangleMeshLoader( &loader, argv[1], !!atoi(argv[6]) );
	} else {
		std::cout << "Unknown extension: " << three_letter_extension << " on file, must be either .3ds or .ply" << std::endl;
		pReadBuffer->release();
		return 1;
	}
	
	ITriangleMeshGeometryIndexed*		geom = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &geom, atoi(argv[4]), atoi(argv[5]), false, !!atoi(argv[3]), !!atoi(argv[7]) );

	loader->LoadTriangleMesh( geom );

	if( pReadBuffer ) {
		pReadBuffer->release();
	}

	// Then dump the geometry out
	IWriteBuffer*		pBuffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &pBuffer, argv[2] );
	geom->Serialize( *pBuffer );
	pBuffer->release();
}
