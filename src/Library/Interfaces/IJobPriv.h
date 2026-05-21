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
#include "ICameraManager.h"
#include "IPainterManager.h"
#include "IScalarPainterManager.h"
#include "IMaterialManager.h"
#include "IModifierManager.h"
#include "IFunction1DManager.h"
#include "IFunction2DManager.h"
#include "IShaderManager.h"
#include "IShaderOpManager.h"
#include "IRasterizer.h"

namespace RISE
{
	// Phase 6.1 + 6.2 forward declarations — these types live in
	// `src/Library/SceneEditor/` so we keep the include light.  The
	// getters return raw pointers so this header doesn't have to
	// pull in unordered_map / Matrix4 / etc.
	class SourceSpanIndex;
	class TransformSnapshot;
	class OverrideSpanIndex;

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
		virtual ICameraManager*				GetCameras() = 0;
		virtual IPainterManager*			GetPainters() = 0;
		virtual IScalarPainterManager*		GetScalarPainters() = 0;
		virtual IFunction1DManager*			GetFunction1Ds() = 0;
		virtual IFunction2DManager*			GetFunction2Ds() = 0;
		virtual IMaterialManager*			GetMaterials() = 0;
		virtual IModifierManager*			GetModifiers() = 0;
		virtual IObjectManager*				GetObjects() = 0;
		virtual ILightManager*				GetLights() = 0;
		virtual IRasterizer*				GetRasterizer() = 0;
		virtual IShaderManager*				GetShaders() = 0;
		virtual IShaderOpManager*			GetShaderOps() = 0;

		// Phase 6.1 (docs/ROUND_TRIP_SAVE_PLAN.md §6.3 + §7.4).
		// Per-entity source-file metadata + transform snapshots captured
		// at scene-load time.  Read-only after parse completes; mutable
		// access is only via AsciiSceneParser during the load pass.
		// Returns non-null for any Job constructed via the standard
		// factories (all three are owned by Job and live for its lifetime);
		// null is reserved for future "lightweight" job types.
		virtual SourceSpanIndex*			GetSourceSpanIndexMutable() = 0;
		virtual const SourceSpanIndex*		GetSourceSpanIndex() const = 0;
		virtual TransformSnapshot*			GetBaseTransformSnapshotMutable() = 0;
		virtual const TransformSnapshot*	GetBaseTransformSnapshot() const = 0;
		virtual TransformSnapshot*			GetLoadedTransformSnapshotMutable() = 0;
		virtual const TransformSnapshot*	GetLoadedTransformSnapshot() const = 0;

		// Phase 6.2 (docs/ROUND_TRIP_SAVE_PLAN.md §6.8 + pinned 2.16).
		// Catalog of every `override_object` chunk parsed from the
		// scene file, with managed/unmanaged classification.
		virtual OverrideSpanIndex*			GetOverrideSpanIndexMutable() = 0;
		virtual const OverrideSpanIndex*	GetOverrideSpanIndex() const = 0;
	};


	//! Creates a new empty job
	bool RISE_CreateJobPriv( 
			IJobPriv** ppi										///< [out] Pointer to recieve the job
			);
}

#endif
