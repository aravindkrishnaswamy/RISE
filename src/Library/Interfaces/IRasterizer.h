//////////////////////////////////////////////////////////////////////
//
//  IRasterizer.h - Defines an interface to a rasterizer.  This is what 
//    converts a 3D scene into a raster image which is basically something
//    viewable
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRASTERIZER_
#define IRASTERIZER_

#include "IReference.h"
#include "IRasterizerOutput.h"
#include "IRasterizeSequence.h"
#include "IScene.h"
#include "ISampling2D.h"
#include "IEnumCallback.h"
#include "IProgressCallback.h"

namespace RISE
{
	namespace Implementation { class FrameStore; }

	class IRasterizer : public virtual IReference
	{
	protected:
		IRasterizer(){};
		virtual ~IRasterizer(){};

	public:
		//! Adds a new rasterizer output
		virtual void AddRasterizerOutput(
			IRasterizerOutput* ro							///< [in] The rasterizer output to add
			) = 0;

		//! Frees all rasterizer outputs
		virtual void FreeRasterizerOutputs() = 0;

		//! Enumerates all the rasterizer outputs
		virtual void EnumerateRasterizerOutputs(
			IEnumCallback<IRasterizerOutput>& pFunc			///< [in] Callback functor to call with each rasterizer output
			) const = 0;

		//! Attaches the a particular scene
		virtual void AttachToScene( 
			const IScene* pScene							///< [in] Scene to attach to
			) = 0;

		//! Detaches from a scene
		virtual void DetachFromScene(
			const IScene* pScene							///< [in] Scene to detach from
			) = 0;

		//! Predicts the amount of time it will take to rasterize
		/// \return The predicted time to rasterize the entire scene in milliseconds
		virtual unsigned int PredictTimeToRasterizeScene( 
			const IScene& pScene,							///< [in] Scene to perform prediction on
			const ISampling2D& pSampling,					///< [in] Sampling kernel to use for prediction
			unsigned int* pActualTime						///< [out] Actual amount of time it will to fully rasterize the scene
			) const = 0;

		//! Rasterizes a scene
		virtual void RasterizeScene(
			const IScene& pScene,							///< [in] Scene to rasterize
			const Rect* pRect,								///< [in] Region in the scene to rasterize, if its NULL, rasterizes the entire scene
			IRasterizeSequence* pRasterSequence				///< [in] The sequence in which to rasterize the scene
			) const = 0;

		//! Rasterizes a scene using the scene animator for keyframes
		virtual void RasterizeSceneAnimation(
			const IScene& pScene,							///< [in] Scene to rasterize
			const Scalar time_start,						///< [in] Scene time to start rasterizing at
			const Scalar time_end,							///< [in] Scene time to finish rasterizing
			const unsigned int num_frames,					///< [in] Number of frames to rasterize
			const bool do_fields,							///< [in] Should the rasterizer render to fields ?
			const bool invert_fields,						///< [in] Should the fields be temporally inverted?
			const Rect* pRect,								///< [in] Region in the scene to rasterize, if its NULL, rasterizes the entire scene
			const unsigned int* specificFrame,				///< [in] Asks to render only a specific frame
			IRasterizeSequence* pRasterSequence				///< [in] Raster sequence (only used when high temporal sampling is enabled)
			) const = 0;

		//! Sets the progress callback
		virtual void SetProgressCallback(
			IProgressCallback* pFunc						///< [in] Callback functor to call to report progress
			) = 0;

		//! L6 Phase 2 — canonical FrameStore the rasterizer writes
		//! into.  In L6a (foundation) the rasterizer holds the
		//! pointer but doesn't use it — internal `mPersistentImage`
		//! (RISERasterImage) is still the write target.  L6b moves
		//! the per-tile splat path through the FrameStore's beauty
		//! channel.  May return nullptr if the caller (typically
		//! `Job`) hasn't supplied one to the factory; rasterizer
		//! consumers MUST check before dereferencing.  Non-pure
		//! with a null default so existing out-of-tree IRasterizer
		//! subclasses (mocks / test stubs) don't break — the
		//! in-tree `Rasterizer` base class overrides.
		//!
		//! Lifetime contract — IMPORTANT for L6b consumers:
		//! the returned raw pointer is owned by the rasterizer,
		//! NOT addref'd before return.  Callers that need to
		//! retain the pointer past the rasterizer's lifetime MUST
		//! `addref()` it themselves and `release()` when done.  A
		//! caller that just dereferences the pointer for an
		//! immediate read/write MUST hold a counted reference to
		//! the rasterizer for the duration of the dereference,
		//! otherwise a concurrent rasterizer destruction (Job
		//! swap, scene reload) drops the FrameStore underneath.
		//! Mirror the `ViewportFrameStore::SnapshotFrameStore`
		//! addref-snapshot pattern (Rendering/ViewportFrameStore.cpp)
		//! when handing the FrameStore to a thread that may outlive
		//! the rasterizer.
		virtual Implementation::FrameStore* GetFrameStore() const { return nullptr; }

		//! Auto-dispatcher introspection — for the auto_rasterizer + the "Auto"
		//! UI surfacing.  A plain rasterizer is not a dispatcher; AutoRasterizer
		//! overrides these to report the concrete integrator it resolved to
		//! (valid after the first render) and the one-line reason.  The cross-UI
		//! query surface: every C++ bridge reaches it via IJobPriv::GetRasterizer().
		virtual bool IsAutoDispatcher() const { return false; }
		virtual const char* ResolvedIntegratorName() const { return ""; }
		virtual const char* ResolveReason() const { return ""; }
	};
}


#endif
