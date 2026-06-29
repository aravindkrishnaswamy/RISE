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
#include "../Interfaces/ITransformable.h"   // TransformState (F6 gizmo drag-start capture)
#include "SaveEngine.h"
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

		//! Photoshop-style tool palette: tools are grouped into category
		//! "slots", and each slot remembers its last-used sub-tool so a
		//! single click on the slot re-activates that sub-tool rather
		//! than the category default.  Long-press / right-click opens
		//! a flyout with all sub-tools in the category.
		//!
		//! Numeric values are part of the C-API surface (bridges pass
		//! ints), so don't reorder.  `ScrubTimeline` is intentionally
		//! NOT in any category — the timeline scrub lives in the
		//! bottom timeline bar, not the main toolbar.
		enum class ToolCategory : int
		{
			Select          = 0,   ///< { Select }              (single sub-tool, no flyout)
			Camera          = 1,   ///< { Orbit, Pan, Zoom, Roll }
			ObjectTransform = 2    ///< { Translate, Rotate, Scale } — needs gizmos
		};

		static constexpr int kNumToolCategories = 3;

		//! Which category does `t` belong to?  ScrubTimeline returns
		//! Select as a fallback (it's not in the main toolbar but
		//! callers expect SOMETHING).  This invariant must hold:
		//! every Tool value maps to exactly one ToolCategory.
		static ToolCategory CategoryForTool( Tool t );

		//! Return the per-category default sub-tool — the one the
		//! toolbar slot shows when the user hasn't picked anything
		//! yet.  Used by the platform UI to seed the slot's initial
		//! icon and by `GetLastSubToolForCategory` as a fallback.
		static Tool DefaultSubToolForCategory( ToolCategory cat );

		//! Photoshop "last-used" memory: returns the sub-tool the
		//! user most recently picked from this category's flyout
		//! (or the category default if the user hasn't activated
		//! anything in this category yet).  A single click on the
		//! slot uses this; the flyout always offers the full set.
		Tool GetLastSubToolForCategory( ToolCategory cat ) const;

		//! Screen-space gizmo handle for the platform overlay to draw
		//! and the controller's pointer dispatch to hit-test.  Positions
		//! are in the camera's CURRENT image-pixel space — UI code that
		//! converts to widget-space must apply the same `fullW`/`fullH`
		//! normalisation it uses for pointer events (see
		//! `GetCameraDimensions`).  Layout convention is "world-axis
		//! only" (per the locked design): handles align to world X/Y/Z,
		//! not to the object's local basis.  Refreshed on demand via
		//! `RefreshGizmoHandles`; values stay valid until the next
		//! refresh (or controller mutation that invalidates the array).
		struct GizmoHandle
		{
			//! What kind of UI gesture this handle accepts.  Numeric
			//! values are C-API surface — don't reorder.
			enum Kind : int
			{
				AxisArrow        = 0,  ///< Translate: drag along world axis
				AxisPlane        = 1,  ///< Translate: drag in plane perpendicular to axis
				ScreenCenter     = 2,  ///< Translate: drag in screen plane (axis == -1)
				AxisRing         = 3,  ///< Rotate: drag tangent to ring around world axis
				ScreenRing       = 4,  ///< Rotate: drag tangent to view-aligned ring (axis == -1)
				AxisScaleHandle  = 5,  ///< Scale: drag along world axis (cube glyph at tip)
				UniformScaleCube = 6   ///< Scale: drag uniformly (axis == -1)
			};
			int    kind;          ///< `Kind` cast to int (C-API surface)
			int    axis;          ///< 0=X, 1=Y, 2=Z; -1 for screen-aligned handles
			double screenX;       ///< Image-pixel-space X (camera's current dims)
			double screenY;       ///< Image-pixel-space Y
			double screenRadius;  ///< Hit-test radius in pixels (drawn icon size hint)
		};

		//! Recompute the gizmo handle array for the current selection +
		//! tool + camera.  Sets the count to 0 (no handles drawn) when:
		//!   - the active tool isn't in `ToolCategory::ObjectTransform`
		//!   - no Object is selected
		//!   - the camera's projection is degenerate (singular matrix
		//!     or pivot behind the eye)
		//! Called by the platform UI before reading the handle array
		//! (typically once per preview frame).
		void RefreshGizmoHandles();

		unsigned int GizmoHandleCount() const;
		int          GizmoHandleKind( unsigned int idx ) const;
		int          GizmoHandleAxis( unsigned int idx ) const;
		double       GizmoHandleScreenX( unsigned int idx ) const;
		double       GizmoHandleScreenY( unsigned int idx ) const;
		double       GizmoHandleScreenRadius( unsigned int idx ) const;

		//! Test/debug hook: project a world-space point to the camera's
		//! current image-pixel space.  Returns false if the point is
		//! behind the camera, the projection is degenerate, or no
		//! camera is attached.
		bool ForTest_ProjectWorldToScreen( double wx, double wy, double wz,
		                                   double& outSx, double& outSy ) const;

		//! Test/debug hook: returns the world-space pivot used by the
		//! gizmo system for the current Object selection.  Reads the
		//! object's `FinalTransformMatrix` translation column (the
		//! world-space origin of the object's local frame).  False if
		//! no Object is selected or the object's transform is
		//! unresolvable.
		bool ForTest_GetSelectionPivotWorld( double& wx, double& wy, double& wz ) const;

		//! Hit-test the current gizmo handle array against an image-
		//! pixel-space pointer position.  Returns the index of the
		//! closest handle whose screen-space proximity is within its
		//! `screenRadius`, or -1 on miss.  Front-to-back priority
		//! follows the handle array order (center / planes / rings
		//! come BEFORE axis arrows so the central glyphs aren't
		//! occluded by longer arrow shafts during hit-test).
		//!
		//! The pointer dispatch (`OnPointerDown`) uses this to switch
		//! from the legacy "drag-anywhere translates" math to a
		//! handle-constrained drag for the duration of the gesture;
		//! exposed publicly so the platform UI can render hover state
		//! (e.g. highlight the handle whose hit-test would catch the
		//! cursor's current position).
		int  GizmoHandleAt( const Point2& px ) const;

		//! True iff a gizmo handle was hit on the most recent
		//! `OnPointerDown` and the drag is still active (not yet
		//! followed by `OnPointerUp`).  The platform UI uses this to
		//! switch the cursor / draw the "active handle" highlight.
		bool IsGizmoDragActive() const;

		//! Active drag handle kind (`GizmoHandle::Kind` cast to int),
		//! or -1 when no gizmo drag is in progress.  Exposed for the
		//! platform overlay so it can highlight the active glyph
		//! between PointerDown and PointerUp.
		int  ActiveGizmoKind() const;
		int  ActiveGizmoAxis() const;

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
			Medium     = 7,   ///< Participating media section (Homogeneous editable;
			                  ///< Heterogeneous read-only because the majorant grid is
			                  ///< baked at construction).
			Animation  = 8,   ///< Named animation paths — picking one makes it the
			                  ///< active animation (like picking a camera); no editable
			                  ///< properties, selection just activates it.
			SceneVariant = 9  ///< scene_variant overlays; picking one RE-DERIVES the scene with that variant active.
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

		// Transactional rollback (feature/gui-snapshot-prototype) ----
		//
		// A *transaction* brackets a sequence of edits that may need to
		// be atomically rejected — an AI L1 (low-confidence) staging
		// reject, an external-client conflict that loses the merge, or
		// a UI "cancel this gesture" affordance.  It is INDEPENDENT of
		// the SceneEditor composite (which collapses a drag into one
		// undo entry); a transaction can wrap one composite, several
		// edits, or none.
		//
		// The shipping interactive drag is ALREADY atomic-on-commit:
		// each OnPointerMove Apply mutates the live scene and records
		// history; OnPointerUp's EndComposite only pushes a marker (no
		// re-apply, no double-apply).  These methods do NOT change that
		// flow — they ADD a clean rollback primitive on top of it.
		//
		// ROLLBACK MECHANISM (re-based 2026: inverse-edit, NOT snapshot).
		// RollbackTransaction reverts by APPLYING THE INVERSE EDITS down
		// to the BeginTransaction undo depth — i.e. it drives
		// SceneEditor::Undo until the undo stack is back at the
		// transaction baseline, reverting live state ON THE SAME object /
		// light / camera / material instances the forward edits touched,
		// then clears the redo stack so the rolled-back gesture is NOT
		// redoable.  It does NOT call Scene::RestoreFromSnapshot (the
		// deep-clone snapshot/restore path has unresolved P1 defects —
		// multi-camera loss, lost identity/sharing, no absence/failure
		// representation; see §13a and the EXPERIMENTAL note on
		// Scene::CreateSnapshot / RestoreFromSnapshot).  No baseline
		// snapshot is captured.  This is identity-safe and clone-free:
		// the only edit types a transaction may contain are the inverse-
		// undoable ones (every SceneEdit op the SceneEditor records —
		// object transform + material/shader/shadow/geometry/interior-
		// medium binding, camera, light, material-slot, medium, scene
		// time).  Edit kinds that BYPASS the EditHistory (film via
		// Job::SetFilm, rasterizer params, animation frame count) are NOT
		// recorded and therefore NOT reverted by a rollback — they leave
		// no undo entry to invert.  Callers must not rely on rollback to
		// undo those; see the per-method notes.
		//
		// Concurrency: BeginTransaction only records a counter (no scene
		// touch).  RollbackTransaction MUTATES the live scene (the
		// inverse-edit applies), so it cancel-and-parks exactly like Undo
		// / SetProperty (trip the rasterizer cancel flag, wait for the
		// in-flight pass to drain under mMutex, revert with the lock held,
		// then KickRender).

		//! Open a rollbackable transaction by recording the current undo
		//! depth, so a later RollbackTransaction can revert exactly the
		//! edits made within the transaction (by applying their inverses
		//! down to this baseline depth).  Captures NO snapshot — rollback
		//! is inverse-edit based.
		//!
		//! Returns true on success.  Calling it while a transaction is
		//! already open REPLACES the baseline (the new call wins) —
		//! nesting is not supported, matching the single-gesture model.
		//!
		//! Returns FALSE (refuses) when a SceneEditor composite is OPEN
		//! (BeginComposite without EndComposite): the baseline would land
		//! inside the group and rollback's composite Undo would undershoot
		//! it, corrupting the surrounding history (re-review finding A).
		//! (Unlike the prior snapshot-based version, this no longer fails
		//! on an out-of-tree IScenePriv: inverse-edit rollback works
		//! through the SceneEditor for any scene the editor can mutate.)
		bool BeginTransaction();

		//! True iff a rollbackable transaction is currently open.
		bool IsTransactionOpen() const;

		//! Roll the transaction's edits back by applying their INVERSES:
		//! drive SceneEditor::Undo until the undo stack returns to the
		//! BeginTransaction baseline depth (reverting live state on the
		//! same instances the forward edits mutated — which, for light
		//! edits and emissive-material rebinds, bumps the scene's light-
		//! topology generation so a reused RayCaster rebuilds its
		//! LightSampler), then clear the redo stack (a rolled-back
		//! gesture must NOT be redoable) and neutralize any open
		//! composite.  Triggers a re-render so the viewport reflects the
		//! reverted state.  Does NOT call Scene::RestoreFromSnapshot.
		//!
		//! Returns false if no transaction is open, or true on a
		//! completed revert.  Returns false (and still closes the
		//! transaction) if the inverse-apply could not fully reach the
		//! baseline depth — e.g. a target entity was removed out from
		//! under an edit, or the gesture exceeded the EditHistory bound
		//! and older records were trimmed away — so the caller learns the
		//! rollback was only partial rather than silently believing the
		//! scene is back at baseline.
		//!
		//! NOTE (honest scope): edits that bypass the EditHistory (film /
		//! rasterizer params / animation frame count — see the mechanism
		//! comment above) leave no inverse to apply and are NOT reverted.
		bool RollbackTransaction();

		//! Commit the transaction: the live edits stay (they were already
		//! applied + recorded during the transaction) and the transaction
		//! is simply closed.  Record-only — it does NOT re-apply or revert
		//! anything (the redo stack is left intact, so a subsequent Undo /
		//! Redo of the committed edits works normally).  No-op (returns
		//! false) if no transaction is open.
		bool EndTransaction();

		//! H1 (de-brittling, P-STATE): the COMPLETE transactional editor-state
		//! baseline -- ONE owned struct captured at BeginTransaction and restored
		//! on RollbackTransaction.  Adding new transactional state is a single
		//! edit here + in Capture/RestoreEditorState (see
		//! docs/gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md).
		struct EditorStateSnapshot {
			unsigned long long          historyMarker;     //!< EditHistory::NextSeq() at capture
			SceneEditor::DirtySnapshot  dirty;             //!< ALL dirty sources (tracker + scale-from-anchor)
			Category                    selectionCategory;
			String                      selectionName;
			// H1 (B-gap close): own the FULL selection state, not just the
			// primary tuple -- a cross-category re-pick inside a transaction
			// must revert wholesale.  std::vector (not [kNumCategories]) so the
			// struct needn't see kNumCategories, which is declared further down.
			std::vector<String>         selectionByCategory;   //!< per-category selection memory
			std::vector<bool>           sectionExpanded;       //!< per-category panel-section expand state
		};
		EditorStateSnapshot CaptureEditorState() const;
		//! restoreDirtyAndHistory: on a FULL rollback restore dirty + the
		//! pre-transaction redo stack; on a PARTIAL rollback pass false so the
		//! residual-dirty state + history are left intact (P1-#1/#3).  Selection
		//! is always restored.
		void                RestoreEditorState( const EditorStateSnapshot& s, bool restoreDirty = true );

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

		// (Named animations are a first-class accordion Category —
		// Category::Animation; the generic CategoryEntityCount/Name,
		// CategoryActiveName and SetSelection surface lists + activates
		// them, so no bespoke per-feature accessors are needed here.
		// GetAnimationOptions above already follows the active animation.)

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

		// Phase 6.5 (docs/ROUND_TRIP_SAVE_PLAN.md §9.9): save the
		// dirty edits + retained overrides back to a `.RISEscene`
		// file using the two-mode round-trip-save engine.  Follows
		// the lock-free disk-IO sequence:
		//   1. Acquire mMutex, cancel in-flight render, wait for
		//      mRendering=false, set mSaving=true, release mMutex.
		//   2. Run SaveEngine::Save outside the lock (file IO is slow).
		//   3. Reacquire mMutex, clear mSaving, surface any error,
		//      notify the render loop.
		// `filePath` is the target .RISEscene to write — typically
		// the originally-loaded path, but the caller can redirect for
		// Save-As.  Returns the SaveResult so the UI can show the
		// outcome (status + counters + error / warning messages).
		SaveResult RequestSave( const std::string& filePath );

		//! True iff a save is currently in flight on disk.  The render
		//! loop's wake condition consults this so a new render pass
		//! doesn't start mid-save (we don't want concurrent file
		//! access AND we want the save's frame-store reads to see a
		//! stable state).  Mirrors mRendering but in the opposite
		//! direction.
		bool IsSaving() const { return mSaving.load(); }

		//! Diagnostic message from the most recent save attempt.
		//! Empty after a successful Saved or NoOp; populated on
		//! Refused or Failed with the engine's errorMessage.  Returned
		//! BY VALUE so a diagnostic logger that caches the string
		//! across a subsequent RequestSave (which mutates
		//! mLastSaveError) doesn't get a torn read of the underlying
		//! std::string buffer.  The write-under-lock + read-by-value
		//! pattern relies on the caller invoking LastSaveError from
		//! the same thread that calls RequestSave (the UI thread in
		//! all platform shells).
		std::string LastSaveError() const;

		//! Phase 6.5 UI hook: true iff there's at least one edit
		//! since the last load / save that the SaveEngine would
		//! write to disk (i.e., not just NoOp).  Drives the GUI's
		//! "Save Scene" button enable state on both platform shells.
		//! Cheap O(1) — just checks the SceneEditor's dirty trackers.
		bool HasUnsavedChanges() const { return mEditor.HasUnsavedChanges(); }

		//! Phase 6.5 UI hook: install a listener that fires when
		//! `HasUnsavedChanges()` flips (clean→dirty or dirty→clean).
		//! The listener runs on the thread that drove the transition
		//! (typically the UI thread for Apply/Undo/Redo edits, or
		//! the calling thread for RequestSave on the clean→ transition
		//! after a successful save).  Platform bridges should marshal
		//! into their UI dispatch queue inside the listener body if
		//! they need main-thread semantics.  Fires ONCE per transition
		//! — a stream of N edits that all leave the scene dirty
		//! produces one callback, not N.  Pass an empty/null `std::function`
		//! to detach.
		using DirtyChangedFn = SceneEditor::DirtyChangedFn;
		void SetDirtyChangedListener( DirtyChangedFn fn )
		{
			mEditor.SetDirtyChangedListener( std::move( fn ) );
		}

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
		// P1: clear the selection if its named entity no longer resolves; called
		// UNCONDITIONALLY after any Undo/Redo so a stale selection never survives,
		// even an atomic no-op composite undo (didWork == false).
		void DropStaleSelection_();

		IJobPriv&                   mJob;
		IRasterizer*                mInteractiveRasterizer;  // borrowed
		// Cached downcast of mInteractiveRasterizer for the polish-pass
		// path (SetSampleCount).  Null if the rasterizer isn't an
		// InteractivePelRasterizer (e.g. test mode with no rasterizer).
		Implementation::InteractivePelRasterizer* mInteractiveImpl;
		SceneEditor                 mEditor;
		Tool                        mTool;
		//! Photoshop-style per-category "last-used" sub-tool memory.
		//! Updated by every `SetTool` call (the tool's category slot
		//! remembers it).  Indexed by `ToolCategory` int values.
		Tool                        mLastSubToolPerCategory[ kNumToolCategories ];
		//! Gizmo handle cache — refreshed by `RefreshGizmoHandles` and
		//! read by the platform overlay + pointer dispatch.  Empty
		//! when the active tool isn't an Object-transform tool or no
		//! Object is selected.
		std::vector<GizmoHandle>    mGizmoHandles;

		//! Active gizmo drag state.  Captured at OnPointerDown when
		//! the pointer hits a handle; consumed by OnPointerMove to
		//! drive constrained drag math; cleared at OnPointerUp.
		//!
		//! `axisDir[a]` is the screen-space direction (in pixels per
		//! world unit, NOT normalised) of world axis `a` at the
		//! pivot, captured at drag-start.  Holding these constant for
		//! the whole drag means a 1-px pointer move produces a
		//! consistent world delta even if the camera shifts mid-drag
		//! (in practice the camera doesn't, but the invariant makes
		//! the math predictable for tests).
		struct GizmoDragState
		{
			bool    active;
			int     kind;             ///< `GizmoHandle::Kind` cast to int
			int     axis;             ///< 0=X, 1=Y, 2=Z; -1 for screen-aligned
			Point3  pivotWorld;       ///< pivot at drag-start
			double  pivotScreenX;     ///< pivot's screen projection at drag-start
			double  pivotScreenY;
			double  anchorPxX;        ///< pointer position at drag-start (for cumulative drags)
			double  anchorPxY;
			double  axisDirX[3];      ///< pixels per world unit, x component
			double  axisDirY[3];      ///< pixels per world unit, y component
			bool    axisOk[3];        ///< false if axis colinear with view at drag-start
			Vector3 prevOrient;       ///< object Euler at drag-start (for Rotate)
			Matrix4 dragStartMatrix;  ///< object's `GetFinalTransformMatrix()` at drag-start.
			                          ///< Used as the anchor for `ScaleObjectFromAnchor` —
			                          ///< Apply restores this then pushes a Stretch on top,
			                          ///< so the factor composes correctly with whatever
			                          ///< transform-stack state the object had (matrix
			                          ///< import / quaternion / earlier SetObjectScale).
			TransformState dragStartState;  ///< F6: component-decomposed transform at
			                                ///< drag-start, so undo of a ScaleObjectFromAnchor
			                                ///< restores COMPONENTS (not a stack-collapsed matrix)
			                                ///< -> a later absolute setter composes correctly.
			bool    dragStartStateValid;    ///< F6: dragStartState captured this drag.
			double  prevAngle;        ///< pointer angle around pivot (for Ring drags)
		};
		GizmoDragState              mGizmoDrag;
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
		static constexpr int        kNumCategories = 10;  // None..SceneVariant
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
		// P1: true iff THIS pointer gesture opened an editor composite on
		// pointer-down.  OnPointerUp closes based on this, NOT the current tool/
		// selection -- a tool/selection change mid-gesture must not strand it.
		bool                        mGestureOpenedComposite;
		// P1: true iff THIS time-scrub opened an editor composite (OnTimeScrubBegin).
		// A missing End / repeated Begin must not strand it -- mirrors the pointer guard.
		bool                        mScrubOpenedComposite;

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
		// Phase 6.5: signals the render loop NOT to start a new pass
		// while a save is in flight (mirror of mRendering for the
		// "saving" direction).  Set inside the locked section of
		// RequestSave; cleared after the engine returns.
		std::atomic<bool>           mSaving;
		std::string                 mLastSaveError;
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

		// Transactional-rollback state.  Appended at the end of the member
		// list so the addition is layout-additive (no field before it
		// shifts).  Re-based on inverse-edit rollback (NOT snapshot): no
		// SceneSnapshot is held.  `mTxnOpen` is true exactly when a
		// transaction is open.  `mTxnBaseline.historyMarker` records EditHistory::NextSeq() at
		// BeginTransaction so RollbackTransaction undoes while the top edit's
		// seq >= that marker (trim-immune; survives the 1024 history cap).  Both
		// are touched only on the UI thread (Begin/Rollback/End are
		// UI-thread calls), so they need no synchronization beyond the
		// cancel-and-park RollbackTransaction already takes for the scene
		// mutation itself.
		bool                                 mTxnOpen;
		EditorStateSnapshot                  mTxnBaseline;        // H1: one owned baseline (history marker + dirty + selection)

		// Disable copy / move
		SceneEditController( const SceneEditController& );
		SceneEditController& operator=( const SceneEditController& );
	};
}

#endif
