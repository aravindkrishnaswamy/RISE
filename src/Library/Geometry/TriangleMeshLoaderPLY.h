//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderPLY.h - Defines a PLY mesh loader, capable
//  of loading the PLY file format, also known as the Stanford
//  triangle format. 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRIANGLE_MESH_LOADER_PLY_
#define TRIANGLE_MESH_LOADER_PLY_

#include "../Interfaces/ITriangleMeshLoader.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshLoaderPLY : public virtual ITriangleMeshLoaderIndexed, public virtual Reference
		{
		protected:
			virtual ~TriangleMeshLoaderPLY( );

			char				szFilename[256];	// The name of the file we are to read from
			const bool			bInvertFaces;		// Should we flip the order of the faces
													// The reason this is here is because I've noticed that some PLY models have flipped around faces

			// Flipping endian-ness on a 4-byte unsigned integer
			inline void FlipUInt( unsigned int *n )
			{
				*n=(((*n>>24)&0xff) |
					((*n&0xff)<<24) |
					((*n>>8)&0xff00) |
					((*n&0xff00)<<8));   
			}

			// Flipping endian-ness on a 4-byte float
			inline void FlipFloat( float* f )
			{
				unsigned int* n = (unsigned int*)f;
				FlipUInt(n);

			}

			// Reads Ascii format
			virtual bool LoadAscii( ITriangleMeshGeometryIndexed* pGeom, FILE* inputFile, const unsigned int numVerts, const unsigned int numPolygons );

			// Reads binary, in the local machine format (BE on BE machine, LE on LE machine)
			virtual bool LoadBinary( ITriangleMeshGeometryIndexed* pGeom, FILE* inputFile, const unsigned int numVerts, const unsigned int numPolygons, const bool bFlipEndianess );

		public:
			TriangleMeshLoaderPLY( const char* szFile, const bool bInvertFaces );

			virtual bool LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom );
		};
	}
}

#endif
