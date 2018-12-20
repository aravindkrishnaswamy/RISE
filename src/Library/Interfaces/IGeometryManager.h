//////////////////////////////////////////////////////////////////////
//
//  IGeometryManager.h - Interface for the geometry manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 15, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IGEOMETRYMANAGER_
#define IGEOMETRYMANAGER_

#include "IGeometry.h"
#include "IManager.h"

namespace RISE
{
	class IGeometryManager : public virtual IManager<IGeometry>
	{
	protected:
		IGeometryManager(){};
		virtual ~IGeometryManager(){};

	public:
		//
		// Geometry specific
		//
	};
}

#endif
