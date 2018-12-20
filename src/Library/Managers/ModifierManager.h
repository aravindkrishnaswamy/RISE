//////////////////////////////////////////////////////////////////////
//
//  ModifierManager.h - Manages modifiers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MODIFIER_MANAGER_
#define MODIFIER_MANAGER_

#include "../Interfaces/IModifierManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class ModifierManager : public virtual IModifierManager, public virtual GenericManager<IRayIntersectionModifier>
		{
		protected:
			virtual ~ModifierManager(){};

		public:
			ModifierManager(){};
		};
	}
}

#endif
