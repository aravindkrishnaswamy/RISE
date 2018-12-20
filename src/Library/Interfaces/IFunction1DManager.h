//////////////////////////////////////////////////////////////////////
//
//  IFunction1DManager.h - Interface for the painter manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IFUNCTION1DMANAGER_
#define IFUNCTION1DMANAGER_

#include "IFunction1D.h"
#include "IManager.h"

namespace RISE
{
	class IFunction1DManager : public virtual IManager<IFunction1D>
	{
	protected:
		IFunction1DManager(){};
		virtual ~IFunction1DManager(){};

	public:
		//
		// Function1D specific
		//
	};
}

#endif
