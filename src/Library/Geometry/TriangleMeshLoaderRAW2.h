//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderRAW2.h - Defines a RAW mesh loader, capable
//  of loading from a really simple .rawMesh file, which is a custom
//  format.  The format of the file is really simple, it is 
//  a geometric file only, it is in ascii and just contains either
//  a bunch of quads or a bunch of triangles, or a mixture of both
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 26, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRIANGLE_MESH_LOADER_RAW2_
#define TRIANGLE_MESH_LOADER_RAW2_

#include "../Interfaces/ITriangleMeshLoader.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshLoaderRAW2 : public virtual ITriangleMeshLoaderIndexed, public virtual Reference
		{
		protected:
			virtual ~TriangleMeshLoaderRAW2();

			char				szFilename[256];	// The name of the file we are to read from

		public:
			TriangleMeshLoaderRAW2( const char* szFile );

			virtual bool LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom );
		};
	}
}

#endif
