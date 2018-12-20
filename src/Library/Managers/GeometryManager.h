//////////////////////////////////////////////////////////////////////
//
//  GeometryManager.h - Manages geometry
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 15, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GEOMETRY_MANAGER_
#define GEOMETRY_MANAGER_

#include "../Interfaces/IGeometryManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class GeometryManager : public virtual IGeometryManager, public virtual GenericManager<IGeometry>
		{
		protected:
			virtual ~GeometryManager(){};

		public:
			GeometryManager(){};
		};
	}
}

#endif
