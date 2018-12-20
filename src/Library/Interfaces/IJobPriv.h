//////////////////////////////////////////////////////////////////////
//
//  IJobPriv.h - Priviledged interface which has getters
// 
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IJOBPRIV_
#define IJOBPRIV_

#include "IJob.h"
#include "IScenePriv.h"
#include "IGeometryManager.h"
#include "IObjectManager.h"
#include "ILightManager.h"
#include "IPainterManager.h"
#include "IMaterialManager.h"
#include "IModifierManager.h"
#include "IFunction1DManager.h"
#include "IFunction2DManager.h"
#include "IShaderManager.h"
#include "IShaderOpManager.h"
#include "IRasterizer.h"

namespace RISE
{
	//! IJobPriv - Priviledged interface with getters
	class IJobPriv : public virtual IJob
	{
	protected:
		IJobPriv(){};
		virtual ~IJobPriv(){};

	public:

		//
		// Getters
		//
		virtual IScenePriv*					GetScene() = 0;
		virtual IGeometryManager*			GetGeometries() = 0;
		virtual IPainterManager*			GetPainters() = 0;
		virtual IFunction1DManager*			GetFunction1Ds() = 0;
		virtual IFunction2DManager*			GetFunction2Ds() = 0;
		virtual IMaterialManager*			GetMaterials() = 0;
		virtual IModifierManager*			GetModifiers() = 0;
		virtual IObjectManager*				GetObjects() = 0;
		virtual ILightManager*				GetLights() = 0;
		virtual IRasterizer*				GetRasterizer() = 0;
		virtual IShaderManager*				GetShaders() = 0;
		virtual IShaderOpManager*			GetShaderOps() = 0;
	};


	//! Creates a new empty job
	bool RISE_CreateJobPriv( 
			IJobPriv** ppi										///< [out] Pointer to recieve the job
			);
}

#endif
