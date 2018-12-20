//////////////////////////////////////////////////////////////////////
//
//  ITriangleMeshLoader.h - Defines an interface to a triangle
//  mesh loader, which loads triangle meshes into a given
//  TriangleMeshObject
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_TRIANGLE_MESH_LOADER_
#define I_TRIANGLE_MESH_LOADER_

#include "IReference.h"
#include "ITriangleMeshGeometry.h"

namespace RISE
{
	//! A class that is able to load a triangle mesh geometry class with triangles
	/// \sa ITriangleMeshGeometry
	class ITriangleMeshLoader : public virtual IReference
	{
	protected:
		ITriangleMeshLoader(){};
		virtual ~ITriangleMeshLoader(){};

	public:
		//! Loads a triangle mesh
		virtual bool LoadTriangleMesh( 
			ITriangleMeshGeometry* pGeom			///< [in] The triangle mesh geometry object to load to
			) = 0;
	};

	//! A class that is able to load a triangle mesh geometry class with indexed triangles
	/// \sa ITriangleMeshGeometryIndexed
	class ITriangleMeshLoaderIndexed : public virtual IReference
	{
	protected:
		ITriangleMeshLoaderIndexed(){};
		virtual ~ITriangleMeshLoaderIndexed(){};

	public:
		//! Loads an indexed triangle mesh
		virtual bool LoadTriangleMesh( 
			ITriangleMeshGeometryIndexed* pGeom		///< [in] The triangle mesh geometry object to load to
			) = 0;
	};
}

#endif
