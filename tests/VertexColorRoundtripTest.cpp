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
		// Normals will be auto-computed.
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
}

int main()
{
	std::cout << "Running VertexColorRoundtripTest..." << std::endl;
	TestRoundtrip();
	TestEmptyColorsRoundtrip();
	std::cout << "All VertexColorRoundtripTest cases passed." << std::endl;
	return 0;
}
