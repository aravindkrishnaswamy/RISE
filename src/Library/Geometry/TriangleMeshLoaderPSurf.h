//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderPSurf.h - Loads a parametric surface as a mesh
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRIANGLE_MESH_LOADER_PSURF_
#define TRIANGLE_MESH_LOADER_PSURF_

#include "../Interfaces/ITriangleMeshLoader.h"
#include "../Interfaces/IParametricSurface.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshLoaderPSurf : public virtual ITriangleMeshLoaderIndexed, public virtual Reference
		{
		protected:
			virtual ~TriangleMeshLoaderPSurf();

			IParametricSurface*		pSurface;		
			unsigned int			nDetailU;
			unsigned int			nDetailV;

		public:
			TriangleMeshLoaderPSurf( IParametricSurface* pSurface_, const unsigned int detail_u, const unsigned int detail_v );

			virtual bool LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom );
		};
	}
}

#endif
