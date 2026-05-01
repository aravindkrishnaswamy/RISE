//////////////////////////////////////////////////////////////////////
//
//  CameraManager.h - Concrete camera manager.  All behaviour
//    (AddItem / RemoveItem / GetItem / refcount / unique-name
//    enforcement) is provided by GenericManager<ICamera>.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAMERA_MANAGER_
#define CAMERA_MANAGER_

#include "../Interfaces/ICameraManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class CameraManager : public virtual ICameraManager, public virtual GenericManager<ICamera>
		{
		protected:
			virtual ~CameraManager(){};

		public:
			CameraManager(){};
		};
	}
}

#endif
