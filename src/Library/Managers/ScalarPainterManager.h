//////////////////////////////////////////////////////////////////////
//
//  ScalarPainterManager.h - Manages named IScalarPainter instances.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCALARPAINTERMANAGER_
#define SCALARPAINTERMANAGER_

#include "../Interfaces/IScalarPainterManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class ScalarPainterManager :
			public virtual IScalarPainterManager,
			public virtual GenericManager<IScalarPainter>
		{
		protected:
			virtual ~ScalarPainterManager() {}

		public:
			ScalarPainterManager() {}
		};
	}
}

#endif
