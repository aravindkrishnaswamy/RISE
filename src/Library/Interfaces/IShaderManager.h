//////////////////////////////////////////////////////////////////////
//
//  IShaderManager.h - Interface for the shader manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 24, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISHADERMANAGER_
#define ISHADERMANAGER_

#include "IShader.h"
#include "IManager.h"

namespace RISE
{
	class IShaderManager : public virtual IManager<IShader>
	{
	protected:
		IShaderManager(){};
		virtual ~IShaderManager(){};

	public:
		//
		// Shader specific
		//
	};
}

#endif
