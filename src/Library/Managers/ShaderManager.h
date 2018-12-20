//////////////////////////////////////////////////////////////////////
//
//  MaterialManger.h - Manages materials
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 24, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHADER_MANAGER_
#define SHADER_MANAGER_

#include "../Interfaces/IShaderManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class ShaderManager : public virtual IShaderManager, public virtual GenericManager<IShader>
		{
		protected:
			virtual ~ShaderManager(){};

		public:
			ShaderManager(){};
		};
	}
}

#endif
