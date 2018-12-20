//////////////////////////////////////////////////////////////////////
//
//  ShaderOpManager.h - Manages materials
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 30, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHADEROP_MANAGER_
#define SHADEROP_MANAGER_

#include "../Interfaces/IShaderOpManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class ShaderOpManager : 
			public virtual IShaderOpManager, 
			public virtual GenericManager<IShaderOp>
		{
		protected:
			virtual ~ShaderOpManager(){};

		public:
			ShaderOpManager(){};
		};
	}
}

#endif
