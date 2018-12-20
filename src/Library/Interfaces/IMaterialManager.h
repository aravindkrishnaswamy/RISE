//////////////////////////////////////////////////////////////////////
//
//  IMaterialManager.h - Interface for the painter manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IMATERALMANAGER_
#define IMATERALMANAGER_

#include "IMaterial.h"
#include "IManager.h"

namespace RISE
{
	class IMaterialManager : public virtual IManager<IMaterial>
	{
	protected:
		IMaterialManager(){};
		virtual ~IMaterialManager(){};

	public:
		//
		// Material specific
		//
	};
}

#endif
