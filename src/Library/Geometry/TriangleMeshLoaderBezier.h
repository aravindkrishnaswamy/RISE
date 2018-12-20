//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshLoaderBezier.h - Loads a raw file that is full 
//  of bezier patches
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 7, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRIANGLE_MESH_LOADER_BEZIER_
#define TRIANGLE_MESH_LOADER_BEZIER_

#include "../Interfaces/ITriangleMeshLoader.h"
#include "../Interfaces/IFunction2D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TriangleMeshLoaderBezier : public virtual ITriangleMeshLoaderIndexed, public virtual Reference
		{
		protected:
			virtual ~TriangleMeshLoaderBezier();

			char				szFilename[256];	// The name of the file we are to read from
			unsigned int		nDetail;
			bool				bCombineSharedVertices;
			bool				bCenterObject;

			const IFunction2D*		displacement;		// Displacement function
			Scalar					disp_scale;			// Scale for displacement

		public:
			TriangleMeshLoaderBezier( 
				const char* szFile, 
				const unsigned int detail,
				const bool bCombineSharedVertices_,
				const bool bCenterObject_,
				const IFunction2D* displacement_,
				Scalar disp_scale_
				);

			virtual bool LoadTriangleMesh( ITriangleMeshGeometryIndexed* pGeom );
		};
	}
}

#endif
