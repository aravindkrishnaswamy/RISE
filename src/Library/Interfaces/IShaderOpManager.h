//////////////////////////////////////////////////////////////////////
//
//  IShaderOpManager.h - Interface for the shader op manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 30, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISHADEROPMANAGER_
#define ISHADEROPMANAGER_

#include "IShaderOp.h"
#include "IManager.h"

namespace RISE
{
	class IShaderOpManager : public virtual IManager<IShaderOp>
	{
	protected:
		IShaderOpManager(){};
		virtual ~IShaderOpManager(){};

	public:
		//
		// ShaderOp specific
		//
	};
}

#endif
