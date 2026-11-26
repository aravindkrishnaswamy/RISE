//////////////////////////////////////////////////////////////////////
//
//  ICameraManager.h - Interface for the camera manager.  Mirrors the
//    other named-thing managers (painters, materials, lights), so a
//    Scene can hold many cameras keyed by name with one designated
//    active (see IScene::GetActiveCameraName / SetActiveCamera).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ICAMERAMANAGER_
#define ICAMERAMANAGER_

#include "ICamera.h"
#include "IManager.h"

namespace RISE
{
	class ICameraManager : public virtual IManager<ICamera>
	{
	protected:
		ICameraManager(){};
		virtual ~ICameraManager(){};
	};
}

#endif
