// GLTFLoaderTest.cpp
//
// Smoke-and-regression test for TriangleMeshLoaderGLTF.  Loads the
// Khronos Sample-Assets "Box" -- the canonical minimal glTF asset --
// and verifies the geometry came through with the expected counts,
// extents, and attributes.  Box.glb ships in the repo at
// scenes/Tests/Geometry/assets/Box.glb (1.6 KB, CC-BY).
//
// Coverage:
//   - Box.glb loads without errors via the v3 indexed mesh interface
//   - Position count > 0 and bounding box matches the canonical -0.5..+0.5
//     extents specified in the source asset
//   - NORMAL is read into pNormals (Box.glb declares NORMAL)
//   - No COLOR_0 in source => pColors stays empty
//   - No TANGENT in source => pTangents stays empty (v3 storage hooks)
//   - Faces produced (12 triangles for the 6-face box)
//   - Out-of-range mesh_index returns false (does not crash)
//   - Missing file returns false (does not crash)
//
// Phase 1 of glTF import (see docs/GLTF_IMPORT.md).  Tangent /
// TEXCOORD_1 storage is exercised at the interface level here -- a
// dedicated tangent-loading test will land alongside the
// NormalTangentTest sample asset in Phase 2 once the consumer
// (normal_map_modifier) exists to verify the data round-trips
// correctly through a render.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "../src/Library/Geometry/TriangleMeshLoaderGLTF.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	bool IsClose( const Scalar a, const Scalar b, const Scalar eps = 1e-4 )
	{
		return std::fabs( a - b ) <= eps;
	}

	// Repo-relative path to the committed Box.glb.  Tests are launched
	// from the repo root by run_all_tests.{sh,ps1}; if you run a single
	// test executable from elsewhere, set RISE_MEDIA_PATH to the repo
	// root or cd there first.
	const char* kBoxAssetPath = "scenes/Tests/Geometry/assets/Box.glb";

	void TestBoxLoadsCleanly()
	{
		std::cout << "Testing Box.glb loads with expected geometry..." << std::endl;

		TriangleMeshGeometryIndexed* pMesh =
			new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();

		TriangleMeshLoaderGLTF* pLoader =
			new TriangleMeshLoaderGLTF( kBoxAssetPath, 0, 0, false );
		pLoader->addref();

		const bool ok = pLoader->LoadTriangleMesh( pMesh );
		assert( ok && "Box.glb failed to load -- is the asset committed at "
		              "scenes/Tests/Geometry/assets/Box.glb and is the "
		              "test running from the repo root?" );

		// Box.glb has 24 vertices (6 faces × 4 corners; each face's
		// corners get their own normals so vertices are not shared
		// between faces) and 12 triangles.  Asserting on these exact
		// numbers makes the test brittle if Khronos re-exports the
		// asset; assert on extents instead.
		const auto& pts = pMesh->getVertices();
		assert( pts.size() > 0 && "Box.glb produced zero vertices" );

		// Bounding box: Box.glb's canonical extent is -0.5..+0.5 along
		// each axis (the asset's accessor min/max declares this).
		Scalar minx = pts[0].x, miny = pts[0].y, minz = pts[0].z;
		Scalar maxx = pts[0].x, maxy = pts[0].y, maxz = pts[0].z;
		for( size_t i = 1; i < pts.size(); ++i ) {
			if( pts[i].x < minx ) minx = pts[i].x;
			if( pts[i].y < miny ) miny = pts[i].y;
			if( pts[i].z < minz ) minz = pts[i].z;
			if( pts[i].x > maxx ) maxx = pts[i].x;
			if( pts[i].y > maxy ) maxy = pts[i].y;
			if( pts[i].z > maxz ) maxz = pts[i].z;
		}
		assert( IsClose( minx, -0.5 ) && "Box.glb min.x != -0.5" );
		assert( IsClose( miny, -0.5 ) && "Box.glb min.y != -0.5" );
		assert( IsClose( minz, -0.5 ) && "Box.glb min.z != -0.5" );
		assert( IsClose( maxx,  0.5 ) && "Box.glb max.x != +0.5" );
		assert( IsClose( maxy,  0.5 ) && "Box.glb max.y != +0.5" );
		assert( IsClose( maxz,  0.5 ) && "Box.glb max.z != +0.5" );

		// 6 faces × 2 triangles each = 12 triangles.
		const auto& faces = pMesh->getFaces();
		assert( faces.size() == 12 && "Box.glb should yield 12 triangles" );

		// NORMAL is declared in Box.glb.
		assert( pMesh->numNormals() > 0 && "Box.glb NORMAL attribute was not read" );

		// COLOR_0 is NOT in Box.glb -- pColors must stay empty (otherwise
		// the v2 dynamic_cast or the attribute matcher is misbehaving).
		assert( pMesh->getColors().empty() && "unexpected COLOR_0 read from Box.glb" );

		// TANGENT is NOT in Box.glb -- v3 tangent storage must be empty.
		assert( pMesh->numTangents() == 0 && "unexpected TANGENT read from Box.glb" );

		// TEXCOORD_1 is NOT in Box.glb -- v3 secondary-UV storage must be empty.
		assert( pMesh->numTexCoords1() == 0 && "unexpected TEXCOORD_1 read from Box.glb" );

		pLoader->release();
		pMesh->release();
		std::cout << "  Passed!" << std::endl;
	}

	void TestMissingFileFailsCleanly()
	{
		std::cout << "Testing missing .glb file returns false without crashing..." << std::endl;

		TriangleMeshGeometryIndexed* pMesh =
			new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();

		TriangleMeshLoaderGLTF* pLoader =
			new TriangleMeshLoaderGLTF(
				"scenes/Tests/Geometry/assets/this_file_does_not_exist.glb",
				0, 0, false );
		pLoader->addref();

		const bool ok = pLoader->LoadTriangleMesh( pMesh );
		assert( !ok && "Missing file should have returned false" );

		pLoader->release();
		pMesh->release();
		std::cout << "  Passed!" << std::endl;
	}

	void TestOutOfRangeMeshIndexFailsCleanly()
	{
		std::cout << "Testing out-of-range mesh_index returns false..." << std::endl;

		TriangleMeshGeometryIndexed* pMesh =
			new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();

		// Box.glb has exactly 1 mesh.  Asking for mesh_index 99 must
		// fail cleanly with a clear log line.
		TriangleMeshLoaderGLTF* pLoader =
			new TriangleMeshLoaderGLTF( kBoxAssetPath, 99, 0, false );
		pLoader->addref();

		const bool ok = pLoader->LoadTriangleMesh( pMesh );
		assert( !ok && "Out-of-range mesh_index should have returned false" );

		pLoader->release();
		pMesh->release();
		std::cout << "  Passed!" << std::endl;
	}

	void TestOutOfRangePrimitiveIndexFailsCleanly()
	{
		std::cout << "Testing out-of-range primitive returns false..." << std::endl;

		TriangleMeshGeometryIndexed* pMesh =
			new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();

		// Box.glb has 1 mesh with 1 primitive.  primitive=99 must fail
		// cleanly.
		TriangleMeshLoaderGLTF* pLoader =
			new TriangleMeshLoaderGLTF( kBoxAssetPath, 0, 99, false );
		pLoader->addref();

		const bool ok = pLoader->LoadTriangleMesh( pMesh );
		assert( !ok && "Out-of-range primitive should have returned false" );

		pLoader->release();
		pMesh->release();
		std::cout << "  Passed!" << std::endl;
	}

	void TestFlipVActuallyFlips()
	{
		std::cout << "Testing flip_v inverts TEXCOORD_0 V values..." << std::endl;

		// Load Box twice: once unflipped, once flipped.  TEXCOORD_0
		// values for Box.glb are all in [0, 1]; with flip_v on the V
		// component should equal 1 - original_v.
		TriangleMeshGeometryIndexed* pMeshA = new TriangleMeshGeometryIndexed( false, false );
		TriangleMeshGeometryIndexed* pMeshB = new TriangleMeshGeometryIndexed( false, false );
		pMeshA->addref();
		pMeshB->addref();

		TriangleMeshLoaderGLTF* pLoaderA = new TriangleMeshLoaderGLTF( kBoxAssetPath, 0, 0, false );
		TriangleMeshLoaderGLTF* pLoaderB = new TriangleMeshLoaderGLTF( kBoxAssetPath, 0, 0, true  );
		pLoaderA->addref();
		pLoaderB->addref();

		assert( pLoaderA->LoadTriangleMesh( pMeshA ) );
		assert( pLoaderB->LoadTriangleMesh( pMeshB ) );

		const auto& uvA = pMeshA->getCoords();
		const auto& uvB = pMeshB->getCoords();
		assert( uvA.size() == uvB.size() && "UV count should match between flipped and unflipped" );
		assert( uvA.size() > 0 && "Box.glb should have TEXCOORD_0" );

		for( size_t i = 0; i < uvA.size(); ++i ) {
			// U unchanged; V = 1 - V on the flipped load.
			assert( IsClose( uvA[i].x,             uvB[i].x ) && "U coordinate must not change with flip_v" );
			assert( IsClose( uvA[i].y, 1.0 - uvB[i].y         ) && "V coordinate must equal 1 - flipped V" );
		}

		pLoaderA->release();
		pLoaderB->release();
		pMeshA->release();
		pMeshB->release();
		std::cout << "  Passed!" << std::endl;
	}
}

int main()
{
	std::cout << "Running GLTFLoaderTest..." << std::endl;
	TestBoxLoadsCleanly();
	TestMissingFileFailsCleanly();
	TestOutOfRangeMeshIndexFailsCleanly();
	TestOutOfRangePrimitiveIndexFailsCleanly();
	TestFlipVActuallyFlips();
	std::cout << "All GLTFLoaderTest cases passed." << std::endl;
	return 0;
}
