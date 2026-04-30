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
			bool			bFlipV;				// Flip TEXCOORD V at load.  glTF stores V increasing upward (V=0 = bottom of texture, OpenGL convention), RISE's TexturePainter samples V increasing downward (row 0 = top of stored image, DirectX convention) -- so this must be TRUE for typical glTF assets.  The `gltfmesh_geometry` chunk defaults it TRUE for that reason; the loader itself stays neutral so callers via Job/RISE_API can override.

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
