//////////////////////////////////////////////////////////////////////
//
//  IBezierPatchGeometry.h - Interface to triangle mesh geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IBEZIER_PATCH_GEOMETRY_
#define IBEZIER_PATCH_GEOMETRY_

#include "IGeometry.h"
#include "../Polygon.h"

namespace RISE
{
	//! A geometry class made up of bezier patches
	/// \sa IGeometry
	class IBezierPatchGeometry : 
		public virtual IGeometry
	{
	protected:
		IBezierPatchGeometry(){};
		virtual ~IBezierPatchGeometry(){};

	public:
		// Functions special to this type of class

		//
		// Adds a new patch to the existing list of patches
		//

		//! Adds a new patch to the list
		virtual void AddPatch( const BezierPatch& patch ) = 0;

		//! Instructs that addition of new patches is complete and that
		//! we can prepare for rendering
		virtual void Prepare() = 0;
	};
}

#endif

