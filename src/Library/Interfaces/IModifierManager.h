//////////////////////////////////////////////////////////////////////
//
//  IModifierManager.h - Interface for the painter manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IModifierMANAGER_
#define IModifierMANAGER_

#include "IRayIntersectionModifier.h"
#include "IManager.h"

namespace RISE
{
	class IModifierManager : public virtual IManager<IRayIntersectionModifier>
	{
	protected:
		IModifierManager(){};
		virtual ~IModifierManager(){};

	public:
		//
		// Modifier specific
		//
	};
}

#endif
