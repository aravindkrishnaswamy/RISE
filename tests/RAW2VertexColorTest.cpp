// RAW2VertexColorTest.cpp
//
// Verifies the optional per-vertex color field appended to RAW2 vertex
// lines (Phase 3 of the vertex-color rollout, 2026-04-28).  The legacy
// 9-field vertex line continues to load color-free; the new 12-field
// vertex line populates pColors.  Mixed-schema files drop colors with
// a warning rather than emit a misaligned color array.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "../src/Library/Geometry/TriangleMeshLoaderRAW2.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	std::string TempPath( const char* suffix )
	{
		char buf[L_tmpnam];
		const char* p = std::tmpnam( buf );
		std::string path = p ? p : "rise_raw2_test";
		path += suffix;
		return path;
	}

	void RemoveQuiet( const std::string& p ) { std::remove( p.c_str() ); }

	bool LoadInto( TriangleMeshGeometryIndexed& mesh, const std::string& path )
	{
		TriangleMeshLoaderRAW2* pLoader = new TriangleMeshLoaderRAW2( path.c_str() );
		pLoader->addref();
		const bool ok = pLoader->LoadTriangleMesh( &mesh );
		pLoader->release();
		return ok;
	}

	void TestLegacy9FieldVertexLines()
	{
		std::cout << "Testing RAW2 with legacy 9-field vertex lines..." << std::endl;
		const std::string path = TempPath( "_rise_raw2_legacy.raw2" );

		FILE* f = std::fopen( path.c_str(), "w" );
		assert( f );
		std::fprintf( f, "3 1\n" );
		std::fprintf( f, "v 0 0 0  0 0 1  0 0\n" );
		std::fprintf( f, "v 1 0 0  0 0 1  1 0\n" );
		std::fprintf( f, "v 0 1 0  0 0 1  0 1\n" );
		std::fprintf( f, "t 0 1 2\n" );
		std::fclose( f );

		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();
		assert( LoadInto( *pMesh, path ) );
		assert( pMesh->numPoints() == 3 );
		assert( pMesh->getColors().empty() );	// no colors in legacy file
		pMesh->release();
		RemoveQuiet( path );
		std::cout << "  Passed!" << std::endl;
	}

	void TestExtended12FieldVertexLines()
	{
		std::cout << "Testing RAW2 with new 12-field colored vertex lines..." << std::endl;
		const std::string path = TempPath( "_rise_raw2_color.raw2" );

		FILE* f = std::fopen( path.c_str(), "w" );
		assert( f );
		std::fprintf( f, "3 1\n" );
		std::fprintf( f, "v 0 0 0  0 0 1  0 0  1.0 0.0 0.0\n" );
		std::fprintf( f, "v 1 0 0  0 0 1  1 0  0.0 1.0 0.0\n" );
		std::fprintf( f, "v 0 1 0  0 0 1  0 1  0.0 0.0 1.0\n" );
		std::fprintf( f, "t 0 1 2\n" );
		std::fclose( f );

		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();
		assert( LoadInto( *pMesh, path ) );
		assert( pMesh->numPoints() == 3 );

		const auto& cols = pMesh->getColors();
		assert( cols.size() == 3 );
		// sRGB(1,0,0) → ROMM RGB → red dominant; same for green / blue.
		assert( cols[0].r > cols[0].g && cols[0].r > cols[0].b );
		assert( cols[1].g > cols[1].r && cols[1].g > cols[1].b );
		assert( cols[2].b > cols[2].r && cols[2].b > cols[2].g );

		pMesh->release();
		RemoveQuiet( path );
		std::cout << "  Passed!" << std::endl;
	}

	void TestMixedSchemaDropsColors()
	{
		std::cout << "Testing RAW2 mixed-schema files drop colors..." << std::endl;
		const std::string path = TempPath( "_rise_raw2_mixed.raw2" );

		// First two vertices have colors; third does not — should drop ALL colors.
		FILE* f = std::fopen( path.c_str(), "w" );
		assert( f );
		std::fprintf( f, "3 1\n" );
		std::fprintf( f, "v 0 0 0  0 0 1  0 0  1.0 0.0 0.0\n" );
		std::fprintf( f, "v 1 0 0  0 0 1  1 0  0.0 1.0 0.0\n" );
		std::fprintf( f, "v 0 1 0  0 0 1  0 1\n" );
		std::fprintf( f, "t 0 1 2\n" );
		std::fclose( f );

		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();
		assert( LoadInto( *pMesh, path ) );
		assert( pMesh->numPoints() == 3 );
		assert( pMesh->getColors().empty() );
		pMesh->release();
		RemoveQuiet( path );
		std::cout << "  Passed!" << std::endl;
	}
}

int main()
{
	std::cout << "Running RAW2VertexColorTest..." << std::endl;
	TestLegacy9FieldVertexLines();
	TestExtended12FieldVertexLines();
	TestMixedSchemaDropsColors();
	std::cout << "All RAW2VertexColorTest cases passed." << std::endl;
	return 0;
}
