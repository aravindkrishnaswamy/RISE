//////////////////////////////////////////////////////////////////////
//
//  Function1DManager.h - Manages 1 dimensional functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FUNCTION1D_MANAGER_
#define FUNCTION1D_MANAGER_

#include "../Interfaces/IFunction1DManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class Function1DManager : public virtual IFunction1DManager, public virtual GenericManager<IFunction1D>
		{
		protected:
			virtual ~Function1DManager(){};

		public:
			Function1DManager(){};
		};
	}
}

#endif
