//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoader3DS.h - Defines a triangle loader that loads
//  from a 3DS file
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRIANGLE_MESH_LOADER_3DS_
#define TRIANGLE_MESH_LOADER_3DS_

#include "../Interfaces/ITriangleMeshLoader.h"
#include "../Interfaces/IReadBuffer.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshLoader3DS : public virtual ITriangleMeshLoaderIndexed, public virtual Reference
		{
		protected:
			virtual ~TriangleMeshLoader3DS();

			unsigned int				nNumObjects;	///< Number of objects in file
			std::vector<unsigned long>	objptrs;		///< Offsets to specific objects
			IReadBuffer*				pBuffer;		///< buffer that contains the file

			int ReadName( );

			// Helpful parsing functions
			bool GetVertices( VerticesListType& vec, unsigned int n );
			bool GetCoords( TexCoordsListType& vec, unsigned int n );
			bool GetFaces( IndexTriangleListType& vec, unsigned int n );

			unsigned int ReadVertices( VerticesListType& vec );
			unsigned int ReadCoords( TexCoordsListType& vec );
			unsigned int ReadFaces( IndexTriangleListType& vec );

			bool LoadTriangleMesh_GoodObjNum( ITriangleMeshGeometryIndexed* pGeom, int obj, bool bRecenter );

		public:
			TriangleMeshLoader3DS( IReadBuffer* pBuffer_ );

			virtual bool LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom ){ return LoadTriangleMesh( pGeom, -1 ); }
			virtual bool LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom, int obj, bool bRecenter = false );
		};
	}
}

#endif
