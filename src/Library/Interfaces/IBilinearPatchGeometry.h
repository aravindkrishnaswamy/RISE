//////////////////////////////////////////////////////////////////////
//
//  IBilinearPatchGeometry.h - Interface to triangle mesh geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IBILINEAR_PATCH_GEOMETRY_
#define IBILINEAR_PATCH_GEOMETRY_

#include "IGeometry.h"
#include "../Polygon.h"

namespace RISE
{
	//! A geometry class made up of bilinear patches
	/// \sa IGeometry
	class IBilinearPatchGeometry : 
		public virtual IGeometry
	{
	protected:
		IBilinearPatchGeometry(){};
		virtual ~IBilinearPatchGeometry(){};

	public:
		// Functions special to this type of class

		//
		// Adds a new patch to the existing list of patches
		//

		//! Adds a new patch to the list
		virtual void AddPatch( const BilinearPatch& patch ) = 0;

		//! Instructs that addition of new patches is complete and that
		//! we can prepare for rendering
		virtual void Prepare() = 0;
	};
}


#endif

