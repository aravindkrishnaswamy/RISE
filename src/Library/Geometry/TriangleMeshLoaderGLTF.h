//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderGLTF.h - Defines a triangle mesh loader that
//  reads a single primitive of a single mesh from a .gltf or .glb
//  file using cgltf.  See docs/GLTF_IMPORT.md for the full design
//  plan; this loader implements Phase 1.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRIANGLE_MESH_LOADER_GLTF_
#define TRIANGLE_MESH_LOADER_GLTF_

#include "../Interfaces/ITriangleMeshLoader.h"
#include "../Utilities/Reference.h"
#include <string>

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshLoaderGLTF : public virtual ITriangleMeshLoaderIndexed, public virtual Reference
		{
		protected:
			virtual ~TriangleMeshLoaderGLTF();

			std::string		szFilename;			// Resolved on-disk path
			unsigned int	meshIndex;			// Index into data->meshes
			unsigned int	primitiveIndex;		// Index into data->meshes[meshIndex].primitives
			bool			bFlipV;				// Flip TEXCOORD V at load time (glTF UV origin is top-left)

		public:
			TriangleMeshLoaderGLTF(
				const char*		szFile,			///< [in] Source .gltf or .glb file
				unsigned int	meshIdx,		///< [in] Which mesh in the file (0-based)
				unsigned int	primIdx,		///< [in] Which primitive within the mesh (0-based)
				bool			flipV			///< [in] Flip the V coordinate of TEXCOORD attributes
				);

			virtual bool LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom );
		};
	}
}

#endif
