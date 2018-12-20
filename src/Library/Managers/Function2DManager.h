//////////////////////////////////////////////////////////////////////
//
//  Function2DManager.h - Manages 1 dimensional functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FUNCTION2D_MANAGER_
#define FUNCTION2D_MANAGER_

#include "../Interfaces/IFunction2DManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class Function2DManager : public virtual IFunction2DManager, public virtual GenericManager<IFunction2D>
		{
		protected:
			virtual ~Function2DManager(){};

		public:
			Function2DManager(){};
		};
	}
}

#endif
