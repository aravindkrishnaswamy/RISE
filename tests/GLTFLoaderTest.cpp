// GLTFLoaderTest.cpp
//
// Adversarial regression test for TriangleMeshLoaderGLTF.  Exercises
// the full Phase 1 attribute matrix (POSITION, NORMAL, TANGENT,
// TEXCOORD_0, TEXCOORD_1, COLOR_0, indices) against the curated
// Khronos Sample-Assets corpus committed at
// scenes/Tests/Geometry/assets/.  Each sample is included in the
// matrix because it stresses a specific code path that no other
// asset in the corpus stresses.
//
// Asset matrix (see docs/GLTF_IMPORT.md §12 for licenses + sources):
//
//   Box.glb ............ POSITION + NORMAL + indices
//                        baseline -- minimal triangle mesh
//   BoxTextured.glb .... + TEXCOORD_0
//                        first asset that exercises pCoords
//   Duck.glb ........... larger real-world mesh (2399 verts)
//                        catches "doesn't crash on bigger data"
//   Avocado.glb ........ + TANGENT
//                        first asset that populates pTangents (v3)
//   NormalTangentTest.glb ........ name lies; source has NO TANGENT
//                                   exercises the "TANGENT absent" path
//   NormalTangentMirrorTest.glb .. has TANGENT with mirrored UVs
//                                   bitangent sign must contain BOTH +1 and -1
//   VertexColorTest.glb .. has 2 meshes; mesh[0] no COLOR, mesh[1] has COLOR_0
//                          exercises the "v2 dynamic_cast + AddColor" path
//   MultiUVTest.glb .... + TEXCOORD_1
//                        first asset that populates pTexCoords1 (v3)
//   OrientationTest.glb .. 13 meshes -- exercises mesh_index up to the boundary
//   AlphaBlendModeTest.glb .. 9 meshes -- v1 ignores alphaMode metadata,
//                              should load every mesh without crashing
//
// Plus the four negative paths from the original test:
//   - Missing file returns false without crashing
//   - Out-of-range mesh_index returns false
//   - Out-of-range primitive_index returns false
//   - flip_v inverts V coords (reload-and-compare round-trip)
//
// Phase 1 of glTF import (see docs/GLTF_IMPORT.md §7).

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

	// ---------- BoxTextured: first TEXCOORD_0 ----------
	void TestBoxTextured()
	{
		std::cout << "Testing BoxTextured.glb (TEXCOORD_0)..." << std::endl;
		TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
		TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
			"scenes/Tests/Geometry/assets/BoxTextured.glb", 0, 0, false ); l->addref();
		assert( l->LoadTriangleMesh( m ) );
		assert( m->getVertices().size() == 24 && "BoxTextured should have 24 verts" );
		assert( m->getFaces().size()    == 12 && "BoxTextured should have 12 tris" );
		assert( m->numNormals() > 0          && "BoxTextured should have NORMAL" );
		// First asset that actually populates pCoords (Box has none).
		assert( m->numCoords()  == 24        && "BoxTextured should have TEXCOORD_0 with 24 entries" );
		assert( m->numTangents() == 0        && "BoxTextured does not have TANGENT" );
		assert( m->numTexCoords1() == 0      && "BoxTextured does not have TEXCOORD_1" );
		assert( m->getColors().empty()       && "BoxTextured does not have COLOR_0" );
		l->release(); m->release();
		std::cout << "  Passed!" << std::endl;
	}

	// ---------- Duck: bigger real-world mesh ----------
	void TestDuck()
	{
		std::cout << "Testing Duck.glb (larger real-world mesh)..." << std::endl;
		TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
		TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
			"scenes/Tests/Geometry/assets/Duck.glb", 0, 0, false ); l->addref();
		assert( l->LoadTriangleMesh( m ) );
		// Per the source accessor metadata: 2399 positions, 12636 indices = 4212 tris.
		assert( m->getVertices().size() == 2399 && "Duck should have 2399 verts" );
		assert( m->getFaces().size()    == 12636 / 3 && "Duck should have 4212 tris" );
		assert( m->numNormals() > 0   && "Duck should have NORMAL" );
		assert( m->numCoords()  == 2399 && "Duck should have TEXCOORD_0" );
		l->release(); m->release();
		std::cout << "  Passed!" << std::endl;
	}

	// ---------- Avocado: first asset with TANGENT ----------
	void TestAvocado()
	{
		std::cout << "Testing Avocado.glb (first TANGENT asset)..." << std::endl;
		TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
		TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
			"scenes/Tests/Geometry/assets/Avocado.glb", 0, 0, false ); l->addref();
		assert( l->LoadTriangleMesh( m ) );
		assert( m->getVertices().size() == 406 && "Avocado should have 406 verts" );
		assert( m->getFaces().size()    == 2046 / 3 && "Avocado should have 682 tris" );
		assert( m->numNormals()  > 0   && "Avocado should have NORMAL" );
		assert( m->numCoords()   == 406 && "Avocado should have TEXCOORD_0" );
		// THE KEY ASSERTION for v3 tangent path -- this is the first asset
		// in the matrix that populates pTangents.  The path runs through
		// dynamic_cast<ITriangleMeshGeometryIndexed3*> in the loader.
		assert( m->numTangents() == 406 && "Avocado should have 406 tangents (v3 storage)" );
		// Bitangent sign must be exactly +1 or -1 per glTF 2.0 spec.
		const auto& tans = m->getTangents();
		for( size_t i = 0; i < tans.size(); ++i ) {
			const Scalar w = tans[i].bitangentSign;
			assert( (IsClose( w, 1.0, 1e-3 ) || IsClose( w, -1.0, 1e-3 ))
				&& "Avocado tangent bitangent sign must be +/-1" );
		}
		l->release(); m->release();
		std::cout << "  Passed!" << std::endl;
	}

	// ---------- NormalTangentTest: name lies; source has NO TANGENT ----------
	void TestNormalTangentTest()
	{
		std::cout << "Testing NormalTangentTest.glb (TANGENT absent path)..." << std::endl;
		TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
		TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
			"scenes/Tests/Geometry/assets/NormalTangentTest.glb", 0, 0, false ); l->addref();
		assert( l->LoadTriangleMesh( m ) );
		assert( m->getVertices().size() == 3983 && "NormalTangentTest should have 3983 verts" );
		assert( m->numNormals() > 0   && "NormalTangentTest should have NORMAL" );
		assert( m->numCoords()  == 3983 && "NormalTangentTest should have TEXCOORD_0" );
		// CRITICAL: the asset's NAME suggests TANGENT, but the actual source
		// has only NORMAL+POSITION+TEXCOORD_0 -- the renderer is expected to
		// derive tangents from positions+normals+UVs.  Phase 1 must not
		// fabricate tangents; numTangents() must be exactly 0 here.
		assert( m->numTangents() == 0 && "NormalTangentTest source has no TANGENT; loader must not fabricate" );
		l->release(); m->release();
		std::cout << "  Passed!" << std::endl;
	}

	// ---------- NormalTangentMirrorTest: TANGENT with mirrored UVs ----------
	void TestNormalTangentMirrorTest()
	{
		std::cout << "Testing NormalTangentMirrorTest.glb (TANGENT with bitangent sign +1 AND -1)..." << std::endl;
		TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
		TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
			"scenes/Tests/Geometry/assets/NormalTangentMirrorTest.glb", 0, 0, false ); l->addref();
		assert( l->LoadTriangleMesh( m ) );
		assert( m->getVertices().size() == 2770 && "NormalTangentMirrorTest should have 2770 verts" );
		assert( m->numTangents() == 2770       && "NormalTangentMirrorTest should have 2770 tangents" );

		// CRITICAL: this is THE asset that catches a bitangent-sign bug.
		// Mirrored UVs require some vertices to have w=+1 and others w=-1.
		// If we silently dropped the w component or normalised it, this
		// assertion fails and we'd see broken normal-mapping in Phase 2.
		const auto& tans = m->getTangents();
		bool sawPos = false, sawNeg = false;
		for( size_t i = 0; i < tans.size(); ++i ) {
			const Scalar w = tans[i].bitangentSign;
			assert( (IsClose( w, 1.0, 1e-3 ) || IsClose( w, -1.0, 1e-3 ))
				&& "MirrorTest tangent bitangent sign must be +/-1" );
			if( w > 0 ) sawPos = true;
			else        sawNeg = true;
		}
		assert( sawPos && "MirrorTest should contain at least one tangent with +1 bitangent sign" );
		assert( sawNeg && "MirrorTest should contain at least one tangent with -1 bitangent sign" );
		l->release(); m->release();
		std::cout << "  Passed!" << std::endl;
	}

	// ---------- VertexColorTest: COLOR_0 round-trip (mesh[1] has it, mesh[0] does not) ----------
	void TestVertexColorTest()
	{
		std::cout << "Testing VertexColorTest.glb (COLOR_0 v2 dynamic_cast path)..." << std::endl;

		// Mesh 0 (LabelMesh): NORMAL+POSITION+TANGENT+TEXCOORD_0, NO COLOR_0.
		// Verifies that absence is correctly observed.
		{
			TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
			TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
				"scenes/Tests/Geometry/assets/VertexColorTest.glb", 0, 0, false ); l->addref();
			assert( l->LoadTriangleMesh( m ) );
			assert( m->getColors().empty() && "VertexColorTest mesh[0] (LabelMesh) has no COLOR_0" );
			assert( m->numTangents() == 24 && "VertexColorTest mesh[0] should have TANGENT" );
			l->release(); m->release();
		}

		// Mesh 1 (VertexColorTestMesh): NORMAL+POSITION+TANGENT+TEXCOORD_0+COLOR_0.
		// Verifies COLOR_0 routes through the v2 dynamic_cast and the
		// linear-Rec709 -> RISEPel conversion.
		{
			TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
			TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
				"scenes/Tests/Geometry/assets/VertexColorTest.glb", 1, 0, false ); l->addref();
			assert( l->LoadTriangleMesh( m ) );
			assert( m->numColors() == 48 && "VertexColorTest mesh[1] should have 48 COLOR_0 entries" );
			// Verify at least one color is non-default (i.e. not pure white,
			// which would indicate the loader wrote uninitialized values).
			const auto& cols = m->getColors();
			bool sawColored = false;
			for( size_t i = 0; i < cols.size(); ++i ) {
				if( cols[i].r < 0.99 || cols[i].g < 0.99 || cols[i].b < 0.99 ) {
					sawColored = true;
					break;
				}
			}
			assert( sawColored && "VertexColorTest mesh[1] should have at least one non-white color" );
			l->release(); m->release();
		}
		std::cout << "  Passed!" << std::endl;
	}

	// ---------- MultiUVTest: TEXCOORD_1 (v3 secondary-UV path) ----------
	void TestMultiUVTest()
	{
		std::cout << "Testing MultiUVTest.glb (TEXCOORD_1 v3 path)..." << std::endl;
		TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
		TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
			"scenes/Tests/Geometry/assets/MultiUVTest.glb", 0, 0, false ); l->addref();
		assert( l->LoadTriangleMesh( m ) );
		assert( m->getVertices().size() == 24 );
		// THE KEY ASSERTION for v3 secondary-UV path -- this is the first
		// (and currently only) asset in the matrix that populates pTexCoords1.
		assert( m->numCoords()     == 24 && "MultiUVTest should have 24 TEXCOORD_0" );
		assert( m->numTexCoords1() == 24 && "MultiUVTest should have 24 TEXCOORD_1 (v3 storage)" );
		// TEXCOORD_1 is the lightmap UV; it should differ from TEXCOORD_0
		// for at least some vertices, otherwise why would the asset declare it?
		const auto& uv0 = m->getCoords();
		const auto& uv1 = m->getTexCoords1();
		bool sawDifferent = false;
		for( size_t i = 0; i < uv0.size(); ++i ) {
			if( !IsClose( uv0[i].x, uv1[i].x, 1e-4 ) || !IsClose( uv0[i].y, uv1[i].y, 1e-4 ) ) {
				sawDifferent = true;
				break;
			}
		}
		assert( sawDifferent && "MultiUVTest TEXCOORD_1 should differ from TEXCOORD_0 for at least one vertex" );
		l->release(); m->release();
		std::cout << "  Passed!" << std::endl;
	}

	// ---------- OrientationTest: 13 meshes; exercise mesh_index near boundary ----------
	void TestOrientationTest()
	{
		std::cout << "Testing OrientationTest.glb (13-mesh asset, boundary mesh_index)..." << std::endl;

		// First mesh loads.
		{
			TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
			TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
				"scenes/Tests/Geometry/assets/OrientationTest.glb", 0, 0, false ); l->addref();
			assert( l->LoadTriangleMesh( m ) );
			assert( m->getVertices().size() > 0 );
			l->release(); m->release();
		}

		// Last mesh (index 12) loads.
		{
			TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
			TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
				"scenes/Tests/Geometry/assets/OrientationTest.glb", 12, 0, false ); l->addref();
			assert( l->LoadTriangleMesh( m ) && "OrientationTest mesh_index 12 (last) should load" );
			assert( m->getVertices().size() > 0 );
			l->release(); m->release();
		}

		// One past the last (index 13) fails cleanly.
		{
			TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
			TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
				"scenes/Tests/Geometry/assets/OrientationTest.glb", 13, 0, false ); l->addref();
			assert( !l->LoadTriangleMesh( m ) && "OrientationTest mesh_index 13 (out of range) must fail" );
			l->release(); m->release();
		}
		std::cout << "  Passed!" << std::endl;
	}

	// ---------- AlphaBlendModeTest: 9 meshes load (we ignore alphaMode metadata) ----------
	void TestAlphaBlendModeTest()
	{
		std::cout << "Testing AlphaBlendModeTest.glb (9 meshes, alpha modes ignored in v1)..." << std::endl;
		// Phase 1 ignores alphaMode metadata; the loader must not crash on
		// any of the 9 meshes (Cutoff25, Cutoff75, Bed, Blend, GreenArrows,
		// DecalOpaque, Opaque, DecalBlend, CutoffDefault).
		for( unsigned int mi = 0; mi < 9; ++mi ) {
			TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false ); m->addref();
			TriangleMeshLoaderGLTF* l = new TriangleMeshLoaderGLTF(
				"scenes/Tests/Geometry/assets/AlphaBlendModeTest.glb", mi, 0, false ); l->addref();
			const bool ok = l->LoadTriangleMesh( m );
			assert( ok && "AlphaBlendModeTest mesh should load (alphaMode is ignored in Phase 1)" );
			assert( m->getVertices().size() > 0 );
			l->release(); m->release();
		}
		std::cout << "  Passed (9 meshes loaded, alphaMode metadata ignored)!" << std::endl;
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

		// Use BoxTextured (which DOES carry TEXCOORD_0) for the flip
		// round-trip; Box.glb itself has no UVs and would only exercise
		// the "no-UV placeholder" path, defeating the test.
		const char* asset = "scenes/Tests/Geometry/assets/BoxTextured.glb";

		TriangleMeshGeometryIndexed* pMeshA = new TriangleMeshGeometryIndexed( false, false );
		TriangleMeshGeometryIndexed* pMeshB = new TriangleMeshGeometryIndexed( false, false );
		pMeshA->addref();
		pMeshB->addref();

		TriangleMeshLoaderGLTF* pLoaderA = new TriangleMeshLoaderGLTF( asset, 0, 0, false );
		TriangleMeshLoaderGLTF* pLoaderB = new TriangleMeshLoaderGLTF( asset, 0, 0, true  );
		pLoaderA->addref();
		pLoaderB->addref();

		assert( pLoaderA->LoadTriangleMesh( pMeshA ) );
		assert( pLoaderB->LoadTriangleMesh( pMeshB ) );

		const auto& uvA = pMeshA->getCoords();
		const auto& uvB = pMeshB->getCoords();
		assert( uvA.size() == uvB.size() && "UV count should match between flipped and unflipped" );
		assert( uvA.size() == 24 && "BoxTextured should have 24 TEXCOORD_0 entries" );

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

	// Positive-path asset coverage (each adds one new code path
	// over the previous; see file header for the matrix rationale).
	TestBoxLoadsCleanly();
	TestBoxTextured();
	TestDuck();
	TestAvocado();
	TestNormalTangentTest();
	TestNormalTangentMirrorTest();
	TestVertexColorTest();
	TestMultiUVTest();
	TestOrientationTest();
	TestAlphaBlendModeTest();

	// Negative paths.
	TestMissingFileFailsCleanly();
	TestOutOfRangeMeshIndexFailsCleanly();
	TestOutOfRangePrimitiveIndexFailsCleanly();
	TestFlipVActuallyFlips();

	std::cout << "All GLTFLoaderTest cases passed." << std::endl;
	return 0;
}
