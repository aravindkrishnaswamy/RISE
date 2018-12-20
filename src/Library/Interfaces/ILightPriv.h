//////////////////////////////////////////////////////////////////////
//
//  ILightPriv.h - Privileged interface, allows you to change stuff
//
//  Note that Ambient light is also a subclass
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 23, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ILIGHT_PRIV_
#define ILIGHT_PRIV_

#include "ITransformable.h"
#include "ILight.h"

namespace RISE
{
	//! Priviledged light interface, allows placement and translation
	class ILightPriv : public virtual ILight, public virtual ITranslatable, public virtual IPositionable
	{
	protected:
		virtual ~ILightPriv(){};
		ILightPriv(){};
	};
}

#endif
