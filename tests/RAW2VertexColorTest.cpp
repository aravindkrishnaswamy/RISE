// RAW2VertexColorTest.cpp
//
// Verifies the optional per-vertex color field appended to RAW2 vertex
// lines (Phase 3 of the vertex-color rollout, 2026-04-28).  The legacy
// 9-field vertex line continues to load color-free; the new 12-field
// vertex line populates pColors.  Mixed-schema files drop colors with
// a warning rather than emit a misaligned color array.
//
// WHY THIS FILE USES A FAILURE TALLY AND NOT `assert` (2026-08-17 counted-
// assertion conversion pass -- see CSGObjectIdentityTest.cpp for the full
// rationale).  build/cmake/rise-tests/CMakeLists.txt never overrides
// CMAKE_CXX_FLAGS_RELEASE, MSVC's Release default carries `/DNDEBUG`, and
// run_all_tests.ps1 defaults to `-Config Release` -- so on that path every
// `assert` compiled to nothing.  Worse, `LoadInto(...)` -- the call that
// actually reads the fixture file into the mesh -- used to sit INSIDE
// `assert(...)`, so under NDEBUG the mesh was never loaded at all and every
// following assertion silently checked an empty mesh.  Every side-effecting
// call is now made on its own line, with only its RESULT checked.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "../src/Library/Geometry/TriangleMeshLoaderRAW2.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

namespace
{
	std::string TempPath( const char* suffix )
	{
		static std::atomic<unsigned> counter{ 0 };
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		std::string name = "rise_raw2_test_";
		name += std::to_string( static_cast<unsigned long long>( stamp ) );
		name += "_";
		name += std::to_string( counter.fetch_add( 1 ) );
		name += suffix;
		return ( std::filesystem::temp_directory_path() / name ).string();
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
		Check( f != nullptr, "legacy: the fixture file opens for writing" );
		std::fprintf( f, "3 1\n" );
		std::fprintf( f, "v 0 0 0  0 0 1  0 0\n" );
		std::fprintf( f, "v 1 0 0  0 0 1  1 0\n" );
		std::fprintf( f, "v 0 1 0  0 0 1  0 1\n" );
		std::fprintf( f, "t 0 1 2\n" );
		std::fclose( f );

		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();
		// SIDE EFFECT ON ITS OWN LINE -- this is the call that loads the mesh the
		// assertions below read.  Inside an `assert` it vanished under NDEBUG.
		const bool loaded = LoadInto( *pMesh, path );
		Check( loaded, "legacy: the 9-field fixture loads" );
		Check( pMesh->numPoints() == 3, "legacy: 3 points loaded" );
		Check( pMesh->getColors().empty(), "legacy: no colors in legacy file" );	// no colors in legacy file
		pMesh->release();
		RemoveQuiet( path );
	}

	void TestExtended12FieldVertexLines()
	{
		std::cout << "Testing RAW2 with new 12-field colored vertex lines..." << std::endl;
		const std::string path = TempPath( "_rise_raw2_color.raw2" );

		FILE* f = std::fopen( path.c_str(), "w" );
		Check( f != nullptr, "extended: the fixture file opens for writing" );
		std::fprintf( f, "3 1\n" );
		std::fprintf( f, "v 0 0 0  0 0 1  0 0  1.0 0.0 0.0\n" );
		std::fprintf( f, "v 1 0 0  0 0 1  1 0  0.0 1.0 0.0\n" );
		std::fprintf( f, "v 0 1 0  0 0 1  0 1  0.0 0.0 1.0\n" );
		std::fprintf( f, "t 0 1 2\n" );
		std::fclose( f );

		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();
		const bool loaded = LoadInto( *pMesh, path );
		Check( loaded, "extended: the 12-field fixture loads" );
		Check( pMesh->numPoints() == 3, "extended: 3 points loaded" );

		const auto& cols = pMesh->getColors();
		Check( cols.size() == 3, "extended: 3 colors loaded" );
		// sRGB(1,0,0) -> ROMM RGB -> red dominant; same for green / blue.
		Check( cols[0].r > cols[0].g && cols[0].r > cols[0].b, "extended: vertex 0 color is red-dominant" );
		Check( cols[1].g > cols[1].r && cols[1].g > cols[1].b, "extended: vertex 1 color is green-dominant" );
		Check( cols[2].b > cols[2].r && cols[2].b > cols[2].g, "extended: vertex 2 color is blue-dominant" );

		pMesh->release();
		RemoveQuiet( path );
	}

	void TestMixedSchemaDropsColors()
	{
		std::cout << "Testing RAW2 mixed-schema files drop colors..." << std::endl;
		const std::string path = TempPath( "_rise_raw2_mixed.raw2" );

		// First two vertices have colors; third does not -- should drop ALL colors.
		FILE* f = std::fopen( path.c_str(), "w" );
		Check( f != nullptr, "mixed: the fixture file opens for writing" );
		std::fprintf( f, "3 1\n" );
		std::fprintf( f, "v 0 0 0  0 0 1  0 0  1.0 0.0 0.0\n" );
		std::fprintf( f, "v 1 0 0  0 0 1  1 0  0.0 1.0 0.0\n" );
		std::fprintf( f, "v 0 1 0  0 0 1  0 1\n" );
		std::fprintf( f, "t 0 1 2\n" );
		std::fclose( f );

		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();
		const bool loaded = LoadInto( *pMesh, path );
		Check( loaded, "mixed: the mixed-schema fixture loads" );
		Check( pMesh->numPoints() == 3, "mixed: 3 points loaded" );
		Check( pMesh->getColors().empty(), "mixed: colors dropped for the whole mesh" );
		pMesh->release();
		RemoveQuiet( path );
	}
}

int main()
{
	std::cout << "Running RAW2VertexColorTest..." << std::endl;
	TestLegacy9FieldVertexLines();
	TestExtended12FieldVertexLines();
	TestMixedSchemaDropsColors();
	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
