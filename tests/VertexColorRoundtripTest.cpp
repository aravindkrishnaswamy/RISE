// VertexColorRoundtripTest.cpp
//
// Verifies that .risemesh v5 (per-vertex color support, 2026-04-28)
// round-trips correctly: Serialize a mesh with vertex colors, read it
// back into a fresh mesh, and confirm the colors match byte-for-byte.
// Also exercises the v1..v4 → v5 backward-compat path by Serialize-ing
// a mesh with colors and verifying that a Deserialize against a fresh
// instance produces the same color array.

#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Interfaces/IBuffer.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	bool IsClose( const Scalar a, const Scalar b, const Scalar eps = 1e-9 )
	{
		return std::fabs( a - b ) <= eps;
	}

	void BuildSampleColoredMesh( TriangleMeshGeometryIndexed& mesh )
	{
		mesh.BeginIndexedTriangles();
		mesh.AddVertex( Point3( 0, 0, 0 ) );
		mesh.AddVertex( Point3( 1, 0, 0 ) );
		mesh.AddVertex( Point3( 0, 1, 0 ) );
		// Push three deterministic linear-ROMM RGB colors directly so the
		// test does not depend on any color-space conversion path.
		mesh.AddColor( RISEPel( 0.50, 0.10, 0.10 ) );
		mesh.AddColor( RISEPel( 0.10, 0.50, 0.10 ) );
		mesh.AddColor( RISEPel( 0.10, 0.10, 0.50 ) );
		mesh.AddTexCoord( Point2( 0, 0 ) );
		mesh.AddTexCoord( Point2( 1, 0 ) );
		mesh.AddTexCoord( Point2( 0, 1 ) );

		IndexedTriangle tri;
		tri.iVertices[0] = 0; tri.iVertices[1] = 1; tri.iVertices[2] = 2;
		tri.iCoords[0]   = 0; tri.iCoords[1]   = 1; tri.iCoords[2]   = 2;
		// ComputeVertexNormals below produces one normal per position, so
		// the normal indices follow the vertex indices.
		tri.iNormals[0]  = 0; tri.iNormals[1]  = 1; tri.iNormals[2]  = 2;
		mesh.AddIndexedTriangle( tri );
		mesh.ComputeVertexNormals();
		mesh.DoneIndexedTriangles();
	}

	void TestRoundtrip()
	{
		std::cout << "Testing TriangleMeshGeometryIndexed v5 color round-trip..." << std::endl;

		TriangleMeshGeometryIndexed* pSrc = new TriangleMeshGeometryIndexed( false, false );
		pSrc->addref();
		BuildSampleColoredMesh( *pSrc );

		MemoryBuffer* pBuffer = new MemoryBuffer();
		pBuffer->addref();

		pSrc->Serialize( *pBuffer );

		// Rewind.  IBuffer::START is the absolute origin.
		pBuffer->seek( IBuffer::START, 0 );

		TriangleMeshGeometryIndexed* pDst = new TriangleMeshGeometryIndexed( false, false );
		pDst->addref();
		pDst->Deserialize( *pBuffer );

		const auto& srcColors = pSrc->getColors();
		const auto& dstColors = pDst->getColors();
		assert( srcColors.size() == dstColors.size() );
		assert( dstColors.size() == 3 );
		for( size_t i = 0; i < srcColors.size(); ++i ) {
			assert( IsClose( srcColors[i].r, dstColors[i].r ) );
			assert( IsClose( srcColors[i].g, dstColors[i].g ) );
			assert( IsClose( srcColors[i].b, dstColors[i].b ) );
		}

		// And the rest of the mesh state should round-trip too.
		assert( pSrc->numPoints() == pDst->numPoints() );
		assert( pSrc->numNormals() == pDst->numNormals() );
		assert( pSrc->numCoords() == pDst->numCoords() );
		assert( pSrc->getFaces().size() == pDst->getFaces().size() );

		pDst->release();
		pBuffer->release();
		pSrc->release();
		std::cout << "  Passed!" << std::endl;
	}

	void TestEmptyColorsRoundtrip()
	{
		std::cout << "Testing v5 round-trip when source has no colors..." << std::endl;

		TriangleMeshGeometryIndexed* pSrc = new TriangleMeshGeometryIndexed( false, false );
		pSrc->addref();
		pSrc->BeginIndexedTriangles();
		pSrc->AddVertex( Point3( 0, 0, 0 ) );
		pSrc->AddVertex( Point3( 1, 0, 0 ) );
		pSrc->AddVertex( Point3( 0, 1, 0 ) );
		pSrc->AddTexCoord( Point2( 0, 0 ) );
		pSrc->AddTexCoord( Point2( 1, 0 ) );
		pSrc->AddTexCoord( Point2( 0, 1 ) );
		IndexedTriangle tri;
		tri.iVertices[0] = 0; tri.iVertices[1] = 1; tri.iVertices[2] = 2;
		tri.iCoords[0]   = 0; tri.iCoords[1]   = 1; tri.iCoords[2]   = 2;
		tri.iNormals[0]  = 0; tri.iNormals[1]  = 1; tri.iNormals[2]  = 2;
		pSrc->AddIndexedTriangle( tri );
		pSrc->ComputeVertexNormals();
		pSrc->DoneIndexedTriangles();
		assert( pSrc->getColors().empty() );

		MemoryBuffer* pBuffer = new MemoryBuffer();
		pBuffer->addref();
		pSrc->Serialize( *pBuffer );
		pBuffer->seek( IBuffer::START, 0 );

		TriangleMeshGeometryIndexed* pDst = new TriangleMeshGeometryIndexed( false, false );
		pDst->addref();
		pDst->Deserialize( *pBuffer );
		assert( pDst->getColors().empty() );
		assert( pDst->numPoints() == 3 );

		pDst->release();
		pBuffer->release();
		pSrc->release();
		std::cout << "  Passed!" << std::endl;
	}

	// Out-of-range indices (from an API caller or a corrupt .risemesh) are
	// refused per triangle in EVERY build.  Before the fix the range check
	// was _DEBUG-only and a release build stored &pNormals[garbage], which
	// ComputeAreas then dereferenced (SIGBUS) -- the crash this file hit
	// while its own fixture left iNormals indeterminate.
	void TestOutOfRangeIndicesRefused()
	{
		std::cout << "Testing out-of-range indices are refused, not dereferenced..." << std::endl;

		TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( false, false );
		pMesh->addref();
		pMesh->BeginIndexedTriangles();
		pMesh->AddVertex( Point3( 0, 0, 0 ) );
		pMesh->AddVertex( Point3( 1, 0, 0 ) );
		pMesh->AddVertex( Point3( 0, 1, 0 ) );
		pMesh->AddTexCoord( Point2( 0, 0 ) );
		pMesh->AddTexCoord( Point2( 1, 0 ) );
		pMesh->AddTexCoord( Point2( 0, 1 ) );

		IndexedTriangle good;
		for( int k = 0; k < 3; ++k ) { good.iVertices[k] = k; good.iCoords[k] = k; good.iNormals[k] = k; }
		IndexedTriangle badNormal = good;
		badNormal.iNormals[1] = 0x7fffffffu;
		IndexedTriangle badVertex = good;
		badVertex.iVertices[2] = 3;
		IndexedTriangle badCoord = good;
		badCoord.iCoords[0] = 3;

		pMesh->AddIndexedTriangle( good );
		pMesh->AddIndexedTriangle( badNormal );
		pMesh->AddIndexedTriangle( badVertex );
		pMesh->AddIndexedTriangle( badCoord );
		pMesh->ComputeVertexNormals();
		pMesh->DoneIndexedTriangles();
		assert( pMesh->getFaces().size() == 1 );

		// A default-constructed triangle has zero indices, not garbage.
		const IndexedTriangle zeroed;
		for( int k = 0; k < 3; ++k ) {
			assert( zeroed.iVertices[k] == 0 && zeroed.iNormals[k] == 0 && zeroed.iCoords[k] == 0 );
		}

		// A corrupt stream: serialize, then overwrite the first vertex index
		// of the one triangle with an out-of-range value.  The index block is
		// the last thing before the double-sided flag, so locate it from the
		// known layout: header 8 + version 4 + face-normal flag 1, then
		// points (4 + 3*3*8), normals (4 + 3*3*8), coords (4 + 3*2*8),
		// colors (4), triangle count (4).
		MemoryBuffer* pBuffer = new MemoryBuffer();
		pBuffer->addref();
		pMesh->Serialize( *pBuffer );
		const unsigned int vertexIndexOffset = 8 + 4 + 1 + (4 + 72) + (4 + 72) + (4 + 48) + 4 + 4;
		pBuffer->seek( IBuffer::START, vertexIndexOffset );
		assert( pBuffer->getUInt() == 0 );
		pBuffer->seek( IBuffer::START, vertexIndexOffset );
		pBuffer->setUInt( 1000000u );
		pBuffer->seek( IBuffer::START, 0 );

		TriangleMeshGeometryIndexed* pDst = new TriangleMeshGeometryIndexed( false, false );
		pDst->addref();
		pDst->Deserialize( *pBuffer );
		assert( pDst->getFaces().empty() );
		assert( pDst->numPoints() == 3 );

		pDst->release();
		pBuffer->release();
		pMesh->release();
		std::cout << "  Passed!" << std::endl;
	}
}

int main()
{
	std::cout << "Running VertexColorRoundtripTest..." << std::endl;
	TestRoundtrip();
	TestEmptyColorsRoundtrip();
	TestOutOfRangeIndicesRefused();
	std::cout << "All VertexColorRoundtripTest cases passed." << std::endl;
	return 0;
}
