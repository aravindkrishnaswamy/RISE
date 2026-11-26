//////////////////////////////////////////////////////////////////////
//
//  IScalarPainterManager.h - Manager interface for scalar painters.
//
//  Mirrors IPainterManager.  Kept as a thin alias so future
//  scalar-painter-specific bookkeeping (e.g. type registration for
//  the scene language) has a place to live.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISCALARPAINTERMANAGER_
#define ISCALARPAINTERMANAGER_

#include "IScalarPainter.h"
#include "IManager.h"

namespace RISE
{
	class IScalarPainterManager : public virtual IManager<IScalarPainter>
	{
	protected:
		IScalarPainterManager() {}
		virtual ~IScalarPainterManager() {}
	};
}

#endif
