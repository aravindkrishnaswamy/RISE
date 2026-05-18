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
	namespace Implementation { class FrameStore; }

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

		//! Discriminator for the right-side accordion sections.
		//! Selection is a (Category, entityName) tuple — see
		//! `mSelectionCategory` / `mSelectionName`.  Numeric values are
		//! part of the C-API surface (the platform bridges pass these
		//! through as ints), so don't reorder.  Each value also doubles
		//! as the PanelMode discriminator below (the same int comes back
		//! from CurrentPanelMode), which means PanelMode and Category
		//! share their first three numeric values for back-compat with
		//! the Phase-2 panel API: None=0, Camera=1, Object=3 retained;
		//! Rasterizer=2 and Light=4 are new.
		enum class Category : int
		{
			None       = 0,   ///< no selection — accordion fully collapsed
			Camera     = 1,   ///< Cameras section, picking activates SetActiveCamera
			Rasterizer = 2,   ///< Rasterizer section, picking activates SetActiveRasterizer
			Object     = 3,   ///< Objects section, picking from list or viewport
			Light      = 4,   ///< Lights section
			Film       = 5,   ///< Output Settings section (single Film per scene)
			Material   = 6,   ///< Materials section
			Medium     = 7    ///< Participating media section (Homogeneous editable;
			                  ///< Heterogeneous read-only because the majorant grid is
			                  ///< baked at construction).
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
		// Selection is the (Category, entityName) tuple that drives both
		// the accordion's expanded section and the property panel's
		// content.  Single selection across the whole panel: picking
		// anything clears whatever was picked before.
		//
		// Side effects of SetSelection differ by category:
		//   Camera     → calls SetActiveCamera (viewport re-renders).
		//   Rasterizer → calls SetActiveRasterizer (next render uses it).
		//   Object     → UI state only.
		//   Light      → UI state only.
		//   Film       → UI state only (single Film per scene; selection
		//                just opens the Output Settings panel).
		// Both Camera and Rasterizer flows go through the cancel-and-park
		// machinery so the swap can't race a mid-flight render pass.

		Category GetSelectionCategory() const;
		String   GetSelectionName() const;

		//! Per-category selection accessor (Phase 4b).  Returns the
		//! entity picked in `cat`'s section, or empty when nothing
		//! is picked in that section.  Distinct from
		//! `GetSelectionName()` which returns only the primary
		//! (most-recently-set) selection: this accessor lets the
		//! panel render multiple sections expanded simultaneously
		//! (e.g. picking an Object expands BOTH the Object and the
		//! Material section, where Material auto-tracks the
		//! object's bound material).
		String GetSelectionNameForCategory( Category cat ) const;

		//! True if `cat`'s accordion section is expanded — tracked
		//! separately from the per-category selection so a user can
		//! click a section header to expand it without yet picking
		//! an entity in that section (the dropdown shows the active-
		//! fallback name; the property list renders the active entity
		//! for sections that have one — Camera, Rasterizer, Film —
		//! or stays empty for Object/Light/Material until a pick).
		bool IsSectionExpanded( Category cat ) const;

		//! Collapse `cat`'s section: clears both the expanded flag
		//! AND the per-category selection.  If this was the primary
		//! category, the primary tuple falls back to any other
		//! expanded section with a non-empty selection, or to
		//! Category::None if none remains.
		void CollapseSection( Category cat );

		//! Apply a (category, entityName) selection.  Empty entityName
		//! is allowed for Camera / Rasterizer / Object / Light: it
		//! means "expand this section, clear the picked entity".  For
		//! Category::None the entityName is ignored.  Returns false on
		//! a category-specific failure (e.g. unknown camera/rasterizer
		//! name); UI-only categories always return true.
		bool SetSelection( Category cat, const String& entityName );

		//! Returns the legacy "selected object name" — empty unless the
		//! current selection's category is Object.  Kept around for the
		//! pointer-event handlers that already used it as a "do I have
		//! an object to translate/rotate/scale?" guard.  New callers
		//! should query GetSelectionCategory + GetSelectionName.
		String SelectedObjectName() const;

		// Accordion entity lists -------------------------------------
		// CategoryEntityCount returns the number of selectable entries
		// in a category; CategoryEntityName returns the display name
		// for a given index.  The platform UIs poll these on each
		// scene-epoch change to rebuild their list views.

		unsigned int CategoryEntityCount( Category cat ) const;
		String       CategoryEntityName( Category cat, unsigned int idx ) const;

		//! Scene-level active entity for a category, independent of the
		//! UI selection.  Camera → IScene::GetActiveCameraName; Rasterizer
		//! → IJob::GetActiveRasterizerName; Film → "default" (a scene has
		//! exactly one Film by construction); Object/Light/None → empty
		//! (no scene-level "active" concept for those).  The accordion
		//! dropdowns display this on first scene load so the user sees
		//! the active camera / rasterizer / film rather than "(pick one)".
		String       CategoryActiveName( Category cat ) const;

		//! Monotonic counter — set ONCE at controller construction from
		//! a process-global atomic that increments per `SceneEditController`
		//! instance.  Each fresh controller therefore has a unique
		//! epoch, which platform UIs cache against `(epoch, category)
		//! → entity-name list` to detect scene reload (the GUI tears
		//! down + recreates the bridge, which builds a new controller).
		//!
		//! NOT bumped on mid-session structural mutations (Add/Remove
		//! camera/object/light, rasterizer-registry add) — Phase 2
		//! doesn't surface those mutations through the GUI, and the
		//! rasterizer list is the static-catalogue union which doesn't
		//! change with registry adds.  Phase 3 will instrument the
		//! relevant `IJob::Add*` paths to advance a Job-side counter
		//! the controller can poll if/when those mutations become
		//! reachable from the interactive UI.
		unsigned int SceneEpoch() const;

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

		//! Sets selection directly without going through pointer events.
		//! Replaces the legacy single-string ForTest_SetSelected hook —
		//! the new contract takes a (category, entityName) tuple.  For
		//! Category::Object it is equivalent to the old Phase-2 hook.
		void ForTest_SetSelection( Category cat, const String& name );

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

		// Properties panel — what the right-side panel should show is
		// purely a function of the current selection (category +
		// entity).  The accordion UI on each platform expands the
		// section corresponding to PanelMode and shows the per-entity
		// property rows below it.
		//
		// PanelMode values are kept in numeric lockstep with Category
		// so the C-API can return either as the same int.
		enum class PanelMode : int {
			None       = 0,
			Camera     = 1,
			Rasterizer = 2,
			Object     = 3,
			Light      = 4,
			Film       = 5,   ///< Output Settings panel for the scene's IFilm
			Material   = 6,   ///< Materials panel
			Medium     = 7    ///< Participating media panel
		};

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

		//! Per-category property snapshot accessors (Phase 4b).
		//! `RefreshProperties()` populates per-category snapshots
		//! for every category with a non-empty selection; these
		//! accessors let the panel render each expanded section's
		//! rows independently.  The single-arg `PropertyCount()` /
		//! `PropertyName(idx)` / ... accessors above continue to
		//! return the PRIMARY category's rows for back-compat.
		unsigned int PropertyCountFor( Category cat ) const;
		String       PropertyNameFor( Category cat, unsigned int idx ) const;
		String       PropertyValueFor( Category cat, unsigned int idx ) const;
		String       PropertyDescriptionFor( Category cat, unsigned int idx ) const;
		int          PropertyKindFor( Category cat, unsigned int idx ) const;
		bool         PropertyEditableFor( Category cat, unsigned int idx ) const;
		unsigned int PropertyPresetCountFor( Category cat, unsigned int idx ) const;
		String       PropertyPresetLabelFor( Category cat, unsigned int idx, unsigned int presetIdx ) const;
		String       PropertyPresetValueFor( Category cat, unsigned int idx, unsigned int presetIdx ) const;
		String       PropertyUnitLabelFor( Category cat, unsigned int idx ) const;

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
		//! parse fails or the property is read-only.  Routes through
		//! the PRIMARY selection — for the multi-section editing path
		//! (per-section edits when both Object and Material sections
		//! are expanded), use `SetPropertyForCategory` so the edit
		//! routes to the right per-category selection.
		bool SetProperty( const String& name, const String& valueStr );

		//! Same as SetProperty but routes through `cat`'s per-
		//! category selection (Phase 4b multi-section panel).  When
		//! `cat` matches the primary selection's category, this is
		//! equivalent to `SetProperty(name, valueStr)`.  When the
		//! Materials section is expanded as a secondary because
		//! primary is Object, an edit in that section routes here
		//! with `cat = Material` and the controller resolves the
		//! material name via the Object's bound material.
		bool SetPropertyForCategory( Category cat, const String& name, const String& valueStr );

		//! Clone the currently-active camera under a new name and
		//! promote the clone to active.  `proposedName` is the user's
		//! choice; on duplicate the controller appends a numeric
		//! dedup suffix so the call always succeeds when there IS an
		//! active camera to clone.  The chosen name is written into
		//! `outName` (NUL-terminated; caller-owned buffer of
		//! `outLen` bytes).  Returns false on no-active-camera, an
		//! unsupported camera type, or `outLen == 0`.  Bumps
		//! `SceneEpoch` so platform UIs auto-rebuild the camera list.
		//!
		//! Persistence caveat: the clone lives in the in-memory
		//! Scene/Job only.  Reloading the .RISEscene file via
		//! `LoadAsciiScene` drops it — the SceneEditor's scene-text
		//! round-trip (Phase 6 serializer) is still pending.
		bool CloneActiveCamera( const String& proposedName,
		                        char* outName, unsigned int outLen );

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

		//! L6e-3 — Ensure `mInteractiveFrameStore` matches the given
		//! dimensions and push it to the interactive rasterizer via
		//! `SetFrameStore`.  Called from `DoOneRenderPass` AFTER the
		//! per-pass camera-dim swap so the FrameStore tracks the
		//! current preview-scale dims.  Same-dim short-circuit avoids
		//! reallocation thrash across passes that don't change scale.
		//! No-op when `mInteractiveRasterizer` is null (test/skeleton
		//! mode).  See impl in SceneEditController.cpp.
		void EnsureInteractiveFrameStore_( unsigned int width, unsigned int height );

		//! Re-derive the auto-synced Material / Medium section
		//! selection names from the currently-pinned Object's bound
		//! material and interior medium.  Called after Undo / Redo
		//! to keep the per-category panel state coherent with the
		//! restored scene state.  No-op if no Object is pinned.
		void ResyncObjectBoundSections_();

		IJobPriv&                   mJob;
		IRasterizer*                mInteractiveRasterizer;  // borrowed
		// Cached downcast of mInteractiveRasterizer for the polish-pass
		// path (SetSampleCount).  Null if the rasterizer isn't an
		// InteractivePelRasterizer (e.g. test mode with no rasterizer).
		Implementation::InteractivePelRasterizer* mInteractiveImpl;
		SceneEditor                 mEditor;
		Tool                        mTool;
		// Selection state — Phase 4b moved from a single tuple to
		// a per-category model so the panel can show multiple
		// sections expanded simultaneously (Object pick auto-
		// expands the Material section bound to that object's
		// material, etc.).  `mSelectionByCategory[i]` is the picked
		// entity name for Category(i), empty when nothing is picked
		// in that section.  `mSelectionCategory` + `mSelectionName`
		// stay as the "primary" — the most recently set non-empty
		// pick, used for the panel header / single-tuple callers.
		// All writes happen on the UI thread; render thread doesn't
		// touch these.
		static constexpr int        kNumCategories = 8;   // None..Medium
		String                      mSelectionByCategory[ kNumCategories ];
		//! Per-category "is the accordion section expanded?" flag,
		//! tracked SEPARATELY from `mSelectionByCategory` so a user
		//! who clicks a section HEADER (to open the section with no
		//! entity picked yet) gets an expanded-but-empty section.
		//! Without this split, my Phase 4b panel collapsed every
		//! section whose per-cat selection was empty — including
		//! the "just-opened with no pick yet" state.  SetSelection
		//! sets the flag; CollapseSection clears it.
		bool                        mSectionExpanded[ kNumCategories ];
		Category                    mSelectionCategory;
		String                      mSelectionName;
		// Bumped on any structural mutation (scene load, camera add,
		// rasterizer register, etc.) so platform UIs can detect when to
		// re-pull entity lists.  Atomic because Job-side writes can
		// happen from any thread that mutates the scene; reads are
		// from the UI thread polling on each preview frame.
		std::atomic<unsigned int>   mSceneEpoch;
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

		// L6e-3 — Per-pass FrameStore for the interactive rasterizer.
		// Allocated/reused in `DoOneRenderPass` to track the current
		// preview-scale dims (which the camera-dim swap mutates each
		// pass between full-res and 1/scale-res).  Pushed to
		// `mInteractiveRasterizer` via `SetFrameStore` so per-pixel
		// writes during `RasterizeScene` land in this store, AND the
		// `OnRasterizerFrameStoreChanged` notification fires on the
		// preview sink — `ViewportPreviewSink::OnRasterizerFrameStoreChanged`
		// (Mac bridge) forwards to the interactive VFS's
		// `BindFrameStore` so direct FrameStore observers track the
		// current per-pass canonical buffer.
		//
		// Pre-L6e-3: the interactive VFS stayed in internal-managed
		// mode (legacy IRasterizerOutput chain → FrameSink copy →
		// VFS-internal store via `ViewportPreviewSink::OutputImage`'s
		// fan-out to `mFanoutVFS->OutputImage`).  L5a's dormant cache
		// amortized the per-scale reallocation in VFS-internal mode.
		//
		// Post-L6e-3: bound mode for interactive.  The dormant-cache
		// equivalent lives here in SceneEditController — we keep the
		// FrameStore around across passes when dims match, reallocate
		// only when scale changes shrink/grow the active dims.
		// `Reference`-counted; we own one addref.
		mutable RISE::Implementation::FrameStore* mInteractiveFrameStore;

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
		// `mProperties` is the PRIMARY-selection snapshot (kept for
		// back-compat with the single-tuple PropertyXxx accessors).
		// `mPropertiesByCategory[i]` is the per-section snapshot —
		// populated for every category with a non-empty selection
		// in `RefreshProperties`.  Phase 4b's multi-section panel
		// reads the per-category arrays so each expanded section
		// renders its own rows independently.
		std::vector<CameraProperty>                          mProperties;
		std::vector<CameraProperty>                          mPropertiesByCategory[ kNumCategories ];

		// Disable copy / move
		SceneEditController( const SceneEditController& );
		SceneEditController& operator=( const SceneEditController& );
	};
}

#endif
