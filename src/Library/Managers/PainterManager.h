//////////////////////////////////////////////////////////////////////
//
//  PainterManger.h - Manages painters
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PAINTER_MANAGER_
#define PAINTER_MANAGER_

#include "../Interfaces/IPainterManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class PainterManager : public virtual IPainterManager, public virtual GenericManager<IPainter>
		{
		protected:
			virtual ~PainterManager(){};

		public:
			PainterManager(){};
		};
	}
}

#endif
