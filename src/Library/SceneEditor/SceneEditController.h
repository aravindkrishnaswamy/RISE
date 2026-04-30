//////////////////////////////////////////////////////////////////////
//
//  SceneEditController.h - The cross-platform brain of the interactive
//    scene editor.  Owns the SceneEditor (mutation), the interactive
//    rasterizer (live preview), the toolbar state machine, and the
//    render thread that cancel-restarts on every edit.
//
//  Each platform UI becomes a thin sink: a viewport that subscribes
//  to the preview output, a toolbar that calls SetTool(...), and
//  pointer event forwarding to OnPointerDown/Move/Up.  The
//  reinterpretation of pointer drag (orbit camera vs translate
//  object vs scrub) is the controller's job — putting it in three
//  platform UIs would guarantee behavioural drift.
//
//  See docs/INTERACTIVE_EDITOR_PLAN.md §4.6.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SCENEEDITCONTROLLER_
#define RISE_SCENEEDITCONTROLLER_

#include "SceneEditor.h"
#include "CancellableProgressCallback.h"
#include "CameraIntrospection.h"
#include "../Interfaces/IJobPriv.h"
#include "../Interfaces/IRasterizer.h"
#include "../Interfaces/IRasterizerOutput.h"
#include "../Interfaces/IProgressCallback.h"
#include "../Interfaces/ILogPrinter.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace RISE
{
	namespace Implementation { class InteractivePelRasterizer; }

	class SceneEditController
	{
	public:
		//! Toolbar mode — drives how OnPointerMove is interpreted.
		//! Numeric values are part of the C-API surface (the platform
		//! bridges pass tool selections through as ints), so don't
		//! reorder.
		enum class Tool
		{
			Select          = 0,   ///< pointer-down picks the object under cursor
			TranslateObject = 1,   ///< drag translates the selected object
			RotateObject    = 2,   ///< drag rotates the selected object
			ScaleObject     = 3,   ///< drag scales the selected object
			OrbitCamera     = 4,   ///< drag orbits the camera (mutates target_orientation)
			PanCamera       = 5,   ///< drag pans the camera (translates pos + lookAt)
			ZoomCamera      = 6,   ///< drag zooms the camera (dolly along forward)
			ScrubTimeline   = 7,   ///< drag scrubs the timeline
			RollCamera      = 8    ///< drag rolls the camera around the forward axis
		};

		//! @param job                     borrowed; caller keeps alive.
		//!                                Must be IJobPriv (which IJob
		//!                                always is in practice — Job
		//!                                inherits IJobPriv).
		//! @param interactiveRasterizer   borrowed; caller keeps alive.
		//!                                May be NULL — the controller
		//!                                degrades to "queue edits, no
		//!                                rendering" mode used by the
		//!                                Phase-2 unit tests.
		SceneEditController( IJobPriv& job, IRasterizer* interactiveRasterizer );
		virtual ~SceneEditController();

		// Lifecycle ---------------------------------------------------

		//! Spawn the render thread.  Idempotent.
		void Start();

		//! Set the running flag false, trip the cancel flag, signal the
		//! condvar, and join the render thread.  Idempotent.
		void Stop();

		bool IsRunning() const;

		// Sinks -------------------------------------------------------
		// Set once before Start() — we don't synchronize sink writes
		// against the render thread because the contract is that
		// platform UIs install sinks at construction time.

		void SetPreviewSink( IRasterizerOutput* sink );
		void SetProgressSink( IProgressCallback* sink );
		void SetLogSink( ILogPrinter* sink );

		// Tool state machine -----------------------------------------

		void SetTool( Tool t );
		Tool CurrentTool() const;

		// Pointer events (called from UI thread) ---------------------
		// Coordinates are platform-defined screen pixels in the
		// preview surface coordinate system.  Conversion from the
		// platform's native event space (which may be window points,
		// HiDPI-backed pixels, etc.) is the bridge layer's job.

		void OnPointerDown( const Point2& px );
		void OnPointerMove( const Point2& px );
		void OnPointerUp( const Point2& px );

		// Direct controls (UI thread) --------------------------------

		//! Bracket a time-scrub interaction.  All OnTimeScrub calls
		//! between Begin and End collapse to one undo entry.
		void OnTimeScrubBegin();
		void OnTimeScrub( Scalar t );
		void OnTimeScrubEnd();

		//! Bracket a property-panel scrub gesture (a click-and-drag
		//! on a value's chevron handle).  Without this signal, the
		//! controller has no way to distinguish a stream of rapid
		//! SetProperty edits from one-off keyboard commits, so the
		//! preview-scale state machine never bumps the divisor and
		//! the user sees only the centre tiles update — every kick
		//! cancels the in-flight render before the outer tiles get
		//! a chance.  Begin bumps the divisor to kPreviewScaleMotionStart
		//! and arms the during-motion adaptive loop; End restores
		//! full resolution and queues one final pass so the polish
		//! frame appears.
		void BeginPropertyScrub();
		void EndPropertyScrub();

		void Undo();
		void Redo();

		//! Canonical scene time tracked by the editor's edit history.
		//! Updated by every OnTimeScrub call AND by Undo / Redo of a
		//! SetSceneTime edit — the SceneEditor's mLastSetTime is the
		//! single source of truth for "where the scene currently is in
		//! time".  Platform UIs should query this just before
		//! handing off to a production rasterizer (e.g. through
		//! IScene::SetSceneTime) instead of trusting their own
		//! timeline-widget state, which goes stale on undo/redo.
		Scalar LastSceneTime() const;

		//! Stop the interactive thread, run the production rasterizer
		//! (whatever the scene declared) on the in-memory mutated
		//! scene, and restart the interactive thread.  Blocks until
		//! the production render completes.
		bool RequestProductionRender();

		// Selection accessors ----------------------------------------

		String SelectedObjectName() const;

		//! Stable full-resolution camera dimensions for pointer-event
		//! coord conversion in the platform bridges.  The controller
		//! temporarily swaps the camera's frame dims to a smaller
		//! preview size during a fast drag (see kPreviewScale), so
		//! ICamera::GetWidth/Height are NOT a stable reference: their
		//! values flicker between full-res and subsampled depending
		//! on whether the swap-restore window is currently inside a
		//! render pass.  Bridges that convert window-space mouse
		//! coords to image-pixel space MUST use this getter, not the
		//! camera's own width/height — otherwise the comparison
		//! between mLastPx (captured at one scale level) and the
		//! incoming px (in another) produces deltas that are wrong
		//! by the scale ratio, manifesting as 4×–32× pan/orbit jumps
		//! whenever the preview-scale state machine steps.
		//!
		//! Returns false if the controller has no camera attached.
		bool GetCameraDimensions( unsigned int& w, unsigned int& h ) const;

		//! Reads the scene's animation options — start time, end time,
		//! number of frames — for sizing the timeline scrubber's
		//! range.  Defaults are (0, 1, 30) when no `animation_options`
		//! chunk was declared in the .RISEscene file.  Returns false
		//! if the underlying job is unavailable.
		bool GetAnimationOptions( double& timeStart, double& timeEnd,
		                          unsigned int& numFrames ) const;

		// Test hooks (Phase 2) ---------------------------------------
		// These let tests bypass picking and observe internal counters.
		// They live in non-RISE_TEST_HOOKS builds too — the surface
		// is small and harmless, and we'd rather not gate parts of
		// the public API behind a build flag.

		void ForTest_SetSelected( const String& name );

		//! Increments each time RequestCancel actually trips an
		//! in-flight render.  Reads can race with the render thread;
		//! callers should join via Stop() before sampling.
		unsigned int ForTest_GetCancelCount() const;

		//! Increments at the start of each render-loop iteration
		//! that actually fires a render pass.
		unsigned int ForTest_GetRenderCount() const;

		//! Block until the render thread has run at least the given
		//! number of completed render passes since Start().  Returns
		//! false on timeout.  Used by the cancel-restart test to wait
		//! for settling without sleep-polling.
		bool ForTest_WaitForRenders( unsigned int count, unsigned int timeoutMs );

		const SceneEditor& Editor() const { return mEditor; }
		SceneEditor&       Editor()       { return mEditor; }

		//! Lets the platform's preview sink check whether the current
		//! pass was cancelled mid-render before dispatching to the UI.
		//! End-of-pass FlushToOutputs fires unconditionally inside the
		//! rasterizer, so without this check a cancelled pass would
		//! overwrite the previous (good) frame with a partially-filled
		//! one.  Reset() at the start of each render-loop iteration
		//! clears the flag, so the value at end-of-pass tells the sink
		//! "was THIS pass cancelled?".
		bool IsCancelRequested() const { return mCancelProgress.IsCancelRequested(); }

		// Properties panel — what the right-side panel should show
		// depends on the active tool and selection:
		//
		//   - Camera tools (Orbit / Pan / Zoom) selected → camera
		//     properties (descriptor-driven, editable).
		//   - Select tool with a picked object → object info
		//     (name + position from final transform; read-only for
		//     now — full editing surface is future work).
		//   - Anything else (Select with no pick, transform tools
		//     w/o selection, etc.) → empty panel.
		//
		// PanelMode is the discriminator the platform UI checks to
		// decide whether to show the panel header / contents at all.
		enum class PanelMode : int { None = 0, Camera = 1, Object = 2 };

		PanelMode CurrentPanelMode() const;

		//! Title string for the panel — "Camera", "Object: <name>",
		//! or empty.  Platforms can render this above the property
		//! list.
		String CurrentPanelHeader() const;

		// Returns an opaque pointer to a snapshot the caller copies
		// out via PropertyCount / PropertyAt.  The snapshot is owned
		// by the controller and invalidated by the next call.

		unsigned int PropertyCount() const;
		String PropertyName( unsigned int idx ) const;
		String PropertyValue( unsigned int idx ) const;
		String PropertyDescription( unsigned int idx ) const;
		int  PropertyKind( unsigned int idx ) const;       // ValueKind cast to int
		bool PropertyEditable( unsigned int idx ) const;

		//! Quick-pick preset accessors for the editor combo box.
		//! Empty for parameters whose descriptor declares no presets.
		unsigned int PropertyPresetCount( unsigned int idx ) const;
		String PropertyPresetLabel( unsigned int idx, unsigned int presetIdx ) const;
		String PropertyPresetValue( unsigned int idx, unsigned int presetIdx ) const;

		//! Short unit suffix to display next to the editor field —
		//! e.g. "mm" for camera sensor / focal / shift, "°" for
		//! angles, "scene units" for focus_distance.  Empty for
		//! dimensionless / unlabelled parameters.  Pure presentation
		//! hint; the parser ignores it.
		String PropertyUnitLabel( unsigned int idx ) const;

		//! Refresh the property snapshot from the live entity.  Called
		//! by the platform UI before reading PropertyN getters.
		//! Picks camera vs object vs empty based on CurrentPanelMode.
		void RefreshProperties();

		//! Apply an edit to a named property.  Triggers a re-render via
		//! the existing edit-pending machinery.  Returns false if the
		//! parse fails or the property is read-only.  Currently routes
		//! to camera properties only (object editing is future work).
		bool SetProperty( const String& name, const String& valueStr );

	protected:
		//! Test override point.  Production override calls
		//! mInteractiveRasterizer->RasterizeScene with the current
		//! scene, our cancellable progress callback installed, and
		//! our preview sink registered as a rasterizer output.
		//!
		//! Mock implementations in tests can simulate cancellable
		//! work without needing a real scene + caster + film.
		virtual void DoOneRenderPass();

	private:
		void RenderLoop();
		void KickRender();

		//! Cast a ray through pixel `px` (image-pixel space) and set
		//! `mSelected` to the hit object's name (or empty if no hit).
		//! Called from OnPointerDown when the Select tool is active.
		void PickAt( const Point2& px );

		IJobPriv&                   mJob;
		IRasterizer*                mInteractiveRasterizer;  // borrowed
		// Cached downcast of mInteractiveRasterizer for the polish-pass
		// path (SetSampleCount).  Null if the rasterizer isn't an
		// InteractivePelRasterizer (e.g. test mode with no rasterizer).
		Implementation::InteractivePelRasterizer* mInteractiveImpl;
		SceneEditor                 mEditor;
		Tool                        mTool;
		String                      mSelected;
		Point2                      mLastPx;
		std::atomic<bool>           mPointerDown;

		// Property-panel chevron scrub is in progress.  Tracked
		// SEPARATELY from mPointerDown so a panel scrub doesn't
		// stomp on an active viewport drag (panel scrub bracket
		// flipping mPointerDown=false would silently break a
		// concurrent orbit / pan / zoom that's still mouse-down).
		// Adaptive-scaling reads OR these two flags so the same
		// preview-scale machinery fires for either gesture.  The
		// render thread also watchdogs this flag — if no edits
		// arrive for kScrubWatchdogMs, the flag self-clears so a
		// missed EndPropertyScrub (e.g. SwiftUI gesture interrupted
		// by parent re-render, Compose pointerInput torn down
		// mid-drag) doesn't leave the preview stuck at low quality
		// indefinitely.
		std::atomic<bool>           mScrubInProgress;

		IRasterizerOutput*          mPreviewSink;
		IProgressCallback*          mProgressSink;
		ILogPrinter*                mLogSink;

		CancellableProgressCallback mCancelProgress;

		// Render-thread machinery -----------------------------------

		std::thread                 mRenderThread;
		mutable std::mutex          mMutex;
		std::condition_variable     mCV;
		std::atomic<bool>           mRunning;
		std::atomic<bool>           mEditPending;
		std::atomic<bool>           mRendering;
		std::atomic<unsigned int>   mCancelCount;
		std::atomic<unsigned int>   mRenderCount;

		// Adaptive preview-resolution divisor.  1 = full camera res;
		// 2 = half each axis = 1/4 the pixel work; 4 = 1/16; 8 = 1/64;
		// 16 = 1/256; 32 = 1/1024.  Six levels.  Starts at 1 (idle);
		// OnPointerDown for motion tools bumps it to kMotionStart.
		// Three feedback loops shape the scale over time:
		//
		//   1. During-motion adaptation: each pass measures wall-clock
		//      and steps the divisor toward a 30Hz budget.  ×2 step
		//      when mildly slow, ×4 jump when very slow, /2 step when
		//      consistently fast.  Lets the system ramp up quickly on
		//      heavy scenes (scale 4 → 16 in two slow frames) and
		//      drift back down as the user slows.
		//
		//   2. Resume-after-pause snap: OnPointerMove detects a gap
		//      longer than kRefineIdleMs and snaps scale back up to
		//      kMotionStart so the first frame after a pause doesn't
		//      stall the viewport at scale=1.
		//
		//   3. Idle refinement: when the pointer is held but no edits
		//      arrive for kRefineIdleMs, the render thread wakes
		//      itself every kRefineWakeMs and steps scale toward 1.
		//      Each refinement pass is rendered without re-running the
		//      during-motion adaptation, otherwise the heavy pass at
		//      the new lower scale would yo-yo it back up.  Result:
		//      after the user stops moving, the image refines itself
		//      from coarse to full-resolution over ~half a second.
		// Stable full-resolution camera dimensions, captured at the
		// start of each DoOneRenderPass BEFORE the preview-scale dim
		// swap.  Bridges read these via GetCameraDimensions to
		// convert pointer events into a coord space that doesn't
		// flicker with the subsample state.  Atomic because the
		// render thread writes them and the UI thread reads them.
		std::atomic<unsigned int>   mFullResW;
		std::atomic<unsigned int>   mFullResH;

		std::atomic<unsigned int>   mPreviewScale;
		static constexpr unsigned int kPreviewScaleMin = 1;
		static constexpr unsigned int kPreviewScaleMax = 32;
		static constexpr unsigned int kPreviewScaleMotionStart = 4;
		// Render-time bands.  Above kTargetMs we downsample more,
		// above kSlowMs we jump 2 levels, below kFastMs we upsample
		// one level.  The gap between bands prevents oscillation.
		static constexpr int        kTargetMs = 33;
		static constexpr int        kSlowMs   = 100;
		static constexpr int        kFastMs   = 16;
		// Idle refinement timing.  After the user pauses for this
		// long while pointer-down, the render thread starts walking
		// the scale toward 1, one level per wake interval.
		static constexpr int        kRefineIdleMs = 150;
		static constexpr int        kRefineWakeMs = 100;

		// Property-scrub watchdog: if no edits land within this
		// window after BeginPropertyScrub, the render thread
		// presumes the End event was lost and clears the scrub
		// flag.  Long enough to never trigger during an active
		// scrub (humans pause for a few hundred ms between drag
		// micro-corrections), short enough that a missed End
		// recovers within a noticeable beat.
		static constexpr int        kScrubWatchdogMs = 1500;

		// Time of the most recent KickRender (ms since steady-clock
		// epoch).  Read by the render thread to decide whether the
		// pointer has been idle long enough to refine.  Read by
		// OnPointerMove to decide whether to snap scale back up after
		// a pause.
		std::atomic<long long>      mLastEditTimeMs;

		// Set by RenderLoop before DoOneRenderPass when the upcoming
		// pass was triggered by an idle-refinement timeout (not by a
		// user edit).  DoOneRenderPass reads this to skip the
		// during-motion adaptation: the refinement loop is already
		// the authority on scale during refinement.
		bool                        mInRefinementPass;

		// Polish-pass state machine.  After OnPointerUp, we run the
		// regular 1-SPP scale=1 final pass, then chain a 4-SPP polish
		// pass at scale=1 that uses the elevated-recursion polish ray
		// caster (one bounce of glossy / refl / refr).  Any new user
		// edit (KickRender) cancels the chain.
		//
		//   None                — no polish in flight
		//   FinalRegularRunning — OnPointerUp queued the 1-SPP pass;
		//                         the post-pass logic transitions to
		//                         PolishQueued and triggers another
		//                         pass at 4 SPP.
		//   PolishQueued        — the upcoming pass is the polish.
		//                         DoOneRenderPass reads this and
		//                         calls InteractivePelRasterizer::
		//                         SetSampleCount(4) before the pass,
		//                         SetSampleCount(1) after.
		enum class PolishState : int { None = 0, FinalRegularRunning = 1, PolishQueued = 2 };
		std::atomic<int>            mPolishState;
		static constexpr unsigned int kPolishSampleCount = 4;

		// Properties-panel snapshot (rebuilt on RefreshProperties).
		std::vector<CameraProperty> mProperties;

		// Disable copy / move
		SceneEditController( const SceneEditController& );
		SceneEditController& operator=( const SceneEditController& );
	};
}

#endif
