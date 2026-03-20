#include <cassert>
#include <iostream>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	void WritePoint( MemoryBuffer& buffer, const Scalar x, const Scalar y, const Scalar z )
	{
		buffer.setDouble( x );
		buffer.setDouble( y );
		buffer.setDouble( z );
	}

	void WriteTexCoord( MemoryBuffer& buffer, const Scalar x, const Scalar y )
	{
		buffer.setDouble( x );
		buffer.setDouble( y );
	}

	void TestLegacyIndexedMeshBSPRebuild()
	{
		std::cout << "Testing legacy RISE mesh BSP compatibility..." << std::endl;

		MemoryBuffer* pBuffer = new MemoryBuffer();
		pBuffer->setBytes( "RISETMGI", 8 );
		pBuffer->setUInt( 1 );
		pBuffer->setUInt( 1 );
		pBuffer->setChar( 8 );
		pBuffer->setChar( 0 );

		pBuffer->setUInt( 3 );
		WritePoint( *pBuffer, 0.0, 0.0, 0.0 );
		WritePoint( *pBuffer, 1.0, 0.0, 0.0 );
		WritePoint( *pBuffer, 0.0, 1.0, 0.0 );

		pBuffer->setUInt( 3 );
		WritePoint( *pBuffer, 0.0, 0.0, 1.0 );
		WritePoint( *pBuffer, 0.0, 0.0, 1.0 );
		WritePoint( *pBuffer, 0.0, 0.0, 1.0 );

		pBuffer->setUInt( 3 );
		WriteTexCoord( *pBuffer, 0.0, 0.0 );
		WriteTexCoord( *pBuffer, 1.0, 0.0 );
		WriteTexCoord( *pBuffer, 0.0, 1.0 );

		pBuffer->setUInt( 1 );
		pBuffer->setUInt( 0 ); pBuffer->setUInt( 0 ); pBuffer->setUInt( 0 );
		pBuffer->setUInt( 1 ); pBuffer->setUInt( 1 ); pBuffer->setUInt( 1 );
		pBuffer->setUInt( 2 ); pBuffer->setUInt( 2 ); pBuffer->setUInt( 2 );

		pBuffer->setChar( 0 );
		pBuffer->setChar( 1 );
		pBuffer->setChar( 1 );

		// Legacy BSP bytes are intentionally left unread by the compatibility path.
		pBuffer->setUInt( 0xDEADBEEF );

		pBuffer->seek( IBuffer::START, 0 );

		TriangleMeshGeometryIndexed* pGeom = new TriangleMeshGeometryIndexed( 1, 8, false, true, false );
		pGeom->Deserialize( *pBuffer );

		assert( pGeom->numPoints() == 3 );

		RayIntersectionGeometric ri( Ray( Point3(0.25, 0.25, 1.0), Vector3(0, 0, -1) ), nullRasterizerState );
		pGeom->IntersectRay( ri, true, true, false );
		assert( ri.bHit );

		safe_release( pGeom );
		safe_release( pBuffer );

		std::cout << "Legacy RISE mesh BSP compatibility Passed!" << std::endl;
	}
}

int main()
{
	TestLegacyIndexedMeshBSPRebuild();
	return 0;
}
