//////////////////////////////////////////////////////////////////////
//
//  MaterialManger.h - Manages materials
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MATERIAL_MANAGER_
#define MATERIAL_MANAGER_

#include "../Interfaces/IMaterialManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class MaterialManager : public virtual IMaterialManager, public virtual GenericManager<IMaterial>
		{
		protected:
			virtual ~MaterialManager(){};

		public:
			MaterialManager(){};
		};
	}
}

#endif
