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

#include "../Cst/Cst.h"   // Facet 5 slice 1a: CstHeadVersion (a trivial POD returned by value below; Cst.h is std-lib-only weight)

namespace RISE
{
	// Forward declarations — these types live in
	// `src/Library/SceneEditor/` so we keep the include light.  The
	// getters return raw pointers so this header doesn't have to
	// pull in unordered_map / Matrix4 / etc.
	class TransformSnapshot;
	struct FileIdentity;   // defined in SceneEditor/FileIdentity.h; CST-save external-mod guard reads it via GetCstLoadFileIdentity()

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

		// Two transform snapshots the legacy streaming loader captured at
		// scene-load time (loader deleted, Model-B P5 Slice 6c).  The
		// CST-only load path never populates them — Job allocates them
		// empty so the getters return stable non-null pointers.  Currently
		// unused (no callers); retained pending deletion — the virtual
		// layout here is append-only.  (The related SourceSpanIndex /
		// OverrideSpanIndex byte-splice metadata was deleted in Slice 6d.)
		virtual TransformSnapshot*			GetBaseTransformSnapshotMutable() = 0;
		virtual const TransformSnapshot*	GetBaseTransformSnapshot() const = 0;
		virtual TransformSnapshot*			GetLoadedTransformSnapshotMutable() = 0;
		virtual const TransformSnapshot*	GetLoadedTransformSnapshot() const = 0;

		// Model-B P5 Slice 6a: the CST-load file identity (path + mtime + size) captured by
		// Job::RefreshCstLoadFileIdentity.  The CST-save external-modification guard reads THIS
		// so the guard has no dependency on any legacy load-time span index.  Job is the sole implementer.
		virtual const FileIdentity&			GetCstLoadFileIdentity() const = 0;

		// Facet 5 (live co-edit) slice 1a: the optimistic-concurrency identity of the retained CST
		// head -- (uuid, revision), see RISE::Cst::CstHeadVersion.  `uuid` is minted fresh on EVERY
		// load (incl. a reload of the same file) and is 0 when no CST head is retained; `revision`
		// bumps IFF the retained Document content changes.  The AgentSession baseHeadVersion conflict
		// precondition reads this to reject a stale patch.  Job is the sole implementer.
		virtual RISE::Cst::CstHeadVersion	GetCstHeadVersion() const = 0;

		// Model-B F2 slice S2a fix round 2 (P2-A): read-only accessor for the progress callback
		// installed via SetProgress -- null when none is installed.  Originally test-only (so a test
		// can assert the Job's progress hook is genuinely restored after a throwing render); since
		// the 2026-07-12 slot-ownership hardening it also has production callers (the macOS bridge's
		// setProgressBlock: refusal check reads it; the coordinator render paths use the atomic
		// ExchangeProgress instead for their capture).  Job is the sole implementer.
		// APPENDED AT THE TRUE END OF THE VIRTUAL TAIL (append-only ABI convention -- do NOT insert mid-tail).
		virtual IProgressCallback*			GetProgress() const = 0;
	};


	//! Creates a new empty job
	bool RISE_CreateJobPriv( 
			IJobPriv** ppi										///< [out] Pointer to recieve the job
			);
}

#endif
