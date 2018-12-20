//////////////////////////////////////////////////////////////////////
//
//  IFunction2DManager.h - Interface for the painter manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IFUNCTION2DMANAGER_
#define IFUNCTION2DMANAGER_

#include "IFunction2D.h"
#include "IManager.h"

namespace RISE
{
	class IFunction2DManager : public virtual IManager<IFunction2D>
	{
	protected:
		IFunction2DManager(){};
		virtual ~IFunction2DManager(){};

	public:
		//
		// Function2D specific
		//
	};
}

#endif
