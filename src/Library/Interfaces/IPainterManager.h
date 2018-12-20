//////////////////////////////////////////////////////////////////////
//
//  IPainterManager.h - Interface for the painter manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPAINTERMANAGER_
#define IPAINTERMANAGER_

#include "IPainter.h"
#include "IManager.h"

namespace RISE
{
	class IPainterManager : public virtual IManager<IPainter>
	{
	protected:
		IPainterManager(){};
		virtual ~IPainterManager(){};

	public:
		//
		// Painter specific
		//
	};
}

#endif
