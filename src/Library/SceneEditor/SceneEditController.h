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
#include <cstdint>
#include <exception>
#include <functional>
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

		//! Model-B F2 slice S1: render IDENTITY.  A monotonic id assigned to
		//! every render this controller (or a headless AgentSession wrapping
		//! a plain Job) kicks off, plus which CLASS of render it is.  This
		//! slice is bookkeeping ONLY -- no scheduling, threading, or
		//! cancellation semantics change; it exists so later slices (async
		//! render worker, pinned-vs-preview, gate retirement) have a stable
		//! seam to hang off.  0 is the reserved "invalid/none" id -- the
		//! counter starts at 2 (see kControllerRenderJobIdStride below).
		typedef std::uint64_t RenderJobId;
		static constexpr RenderJobId kInvalidRenderJobId = 0;

		//! Pre-S2 hardening: this controller's coordinator-minted ids and
		//! AgentSession's session-local ids (AgentSession.h
		//! mNextSessionLocalRenderJobId) are TWO INDEPENDENT counters that
		//! both start small -- without a disjointness rule the same numeric
		//! id can name two different renders in one process, which a future
		//! Status(jobId)/Wait(jobId) (S2) would alias onto the wrong job.
		//! Fix: the two spaces are disjoint by PARITY -- coordinator
		//! (controller-minted) ids are EVEN, starting at 2 and incrementing
		//! by this stride; session-local ids are ODD, starting at 1 and
		//! incrementing by the same stride (see AgentSession.h).  A tagged-
		//! high-bit scheme (id | (1ULL<<63)) was considered and REJECTED:
		//! the wire path serializes ids through a double
		//! (JsonValue::MakeNumber -> Json.cpp SerializeNumber), whose
		//! exact-integer fast path requires fabs(d) < 9.0e15 -- (1ULL<<63)
		//! is ~9.22e18, so it both (a) falls through to the %.17g
		//! scientific-notation branch and (b) cannot even round-trip
		//! through a double exactly (2^63 exceeds the 53-bit mantissa),
		//! corrupting the id on the wire.  The parity scheme keeps every
		//! id, for the lifetime of any realistic session, comfortably
		//! inside the exact-double-integer range.
		static constexpr RenderJobId kControllerRenderJobIdStride = 2;

		//! What KIND of render a RenderJobId names.  Named for what EXISTS
		//! TODAY only:
		//!   Interactive  -- the controller's own RenderLoop pass (the
		//!                   cancel-restart preview loop driving the
		//!                   viewport).
		//!   AgentPreview -- a caller-supplied lambda run synchronously
		//!                   under RunPreviewRenderParked (today: the Agent
		//!                   surface's transient film/camera-override
		//!                   render).
		//! Production-render tracking is explicitly OUT of scope for this
		//! slice (see slice S4).
		enum class RenderClass
		{
			Interactive  = 0,
			AgentPreview = 1
		};

		//! Snapshot of the CURRENT render job, read under mMutex.  `active`
		//! is false when no render is presently in flight; `id`/`renderClass`
		//! then reflect the MOST RECENTLY assigned job (stale, informational
		//! only).
		//!
		//! Fix-round-1 P3-c: `clientLabel` echoes the diagnostic tag a
		//! caller passed to SubmitAgentRenderAsync / SubmitAgentRenderSync
		//! (empty for an Interactive-class job, which has no client label
		//! concept).  Was write-only dead state (mAgentRenderClientLabel
		//! was recorded but never read by anything) -- surfaced here so a
		//! Status() consumer can tell WHICH agent submitted an in-flight
		//! render, useful for S2b's gate predicate and for diagnosing which
		//! caller is occupying the single slot.
		struct RenderJobStatus
		{
			RenderJobId  id          = kInvalidRenderJobId;
			RenderClass  renderClass = RenderClass::Interactive;
			bool         active      = false;
			String       clientLabel;
		};

		//! Facet 5 slice 1b: the structured result of a CONTROLLER-ROUTED
		//! agent commit (ApplyAgentParamEdit).  Mirrors the Agent surface's
		//! AgentPatchResult 1:1 (folding Job::ApplyCstParamEdit's 0/1/2/3
		//! return the SAME way), but lives HERE so the SceneEditor library
		//! layer does not depend on the Agent layer (the dependency runs
		//! Agent -> SceneEditor, never the reverse).  AgentSession maps this
		//! into its own AgentPatchResult when it routes ProposePatch through
		//! an attached controller.
		//!
		//!   * rawCode 1/2 -> applied=true,  status="applied": the Document
		//!            was mutated and the live Job re-derived CLEANLY
		//!            (1 incremental / 2 full re-derive).  The only
		//!            clean-success codes.
		//!   * rawCode 3   -> applied=false, status="diagnosed": the Document
		//!            WAS mutated and the managers WERE replaced, BUT the full
		//!            re-derive emitted diagnostics -- the source contract
		//!            treats 3 as a FAILURE, so applied is false.
		//!   * rawCode 0   -> applied=false, status="rejected": edit refused;
		//!            the head is byte-identical (nothing changed).  The
		//!            pre-flight guards (no Document / empty field / open
		//!            editor transaction) map here.
		//!   * conflict    -> applied=false, status="conflict", rawCode=0: a
		//!            supplied baseVersion did NOT equal the Job's current
		//!            head -- the patch was rejected WITHOUT mutating (a stale
		//!            patch must never touch the Document); re-read and retry.
		//! `headVersion` is the Job's head AFTER the call (post-commit on a
		//! clean apply; the current/unchanged head otherwise).  `conflict` is
		//! a convenience bool == (status == "conflict").
		//! `retriable` disambiguates the "rejected" bucket for a machine
		//! client: true means the refusal is TRANSIENT -- the identical
		//! commit can succeed later with NO change to the patch.  The ONLY
		//! transient reject today is the open-editor-transaction refusal
		//! (retry after the gesture completes); the permanent rejects (no
		//! Document / empty field / unknown entity / bad value) keep the
		//! default false -- retrying them verbatim can never succeed.  A
		//! version CONFLICT does NOT set the flag: it has its own
		//! status="conflict" and is retriable-by-protocol via re-read
		//! (re-read the head, rebase, re-propose) rather than by verbatim
		//! resubmission.
		struct AgentCommitResult
		{
			bool                       applied = false;
			bool                       conflict = false;
			bool                       retriable = false; //!< always present; meaningful for status="rejected" only: true = transient refusal (open editor transaction) -- retry the SAME commit later; false = permanent
			int                        rawCode = 0;      //!< 0 reject/conflict / 1 incremental / 2 D2 / 3 replaced-but-diagnosed
			String                     status;           //!< "applied" / "rejected" / "diagnosed" / "conflict"
			RISE::Cst::CstHeadVersion  headVersion;      //!< the head-version AFTER the call
			String                     message;
			//! Model-B F5 slice S2 (chunk-CRUD verbs only; empty for ApplyAgentParamEdit):
			//! the affected chunk's KEYWORD (kind) and `name` param, echoed from Job's
			//! parse/resolution so the Agent surface reports what was inserted/removed.
			String                     chunkKeyword;
			String                     chunkName;
		};

		//! Facet 5 slice 1b: route an agent param-value commit through the
		//! render-thread-SAFE edit path.  This is the SAME (entity,param) edit
		//! the Agent surface's ProposePatch makes -- routed through
		//! Job::ApplyCstParamEditChecked (round-2 P1-A: the FULL-DERIVABILITY
		//! gated variant, so an agent retarget can never commit a head that no
		//! longer derives in document order; the GUI panel/gizmo path keeps the
		//! ungated ApplyCstParamEdit) -- but wrapped in the controller's
		//! cancel-and-park critical section so it is safe against the live
		//! render thread, and it calls RebindEditorToJob on a D2 full re-derive
		//! (codes 2/3) so the editor's cached scene/manager pointers do not
		//! dangle.  Callable from ANY thread PROVIDED the transaction API is
		//! unused (transactions are main-thread-only and the mTxnOpen
		//! pre-flight read below is UNSYNCHRONIZED -- mMutex does not cover
		//! the flag); within that contract it takes mMutex +
		//! cancel-and-parks itself, and the whole commit + rebind +
		//! version-bump runs UNDER mMutex so no render-thread reader can
		//! observe the transient {0,0} head-version (D2 ClearAll) or a
		//! half-rebuilt Scene.
		//!
		//! @param baseVersionOrNull  OPTIONAL optimistic-concurrency
		//!        precondition (slice 1a).  When non-null and it does NOT
		//!        equal the Job's current head, the commit is REJECTED with a
		//!        CONFLICT (WITHOUT mutating) so a stale patch never clobbers a
		//!        newer head.  Null -> unconditional (back-compat).
		//!
		//! Returns an AgentCommitResult; on a clean apply it sets
		//! mEditPending + kicks the render so the viewport re-renders.
		//!
		//! REFUSED (status="rejected", retriable=true, non-mutating) while
		//! an editor transaction is open: an agent commit has no
		//! EditHistory record, so RollbackTransaction could never revert
		//! it -- the agent should retry after the gesture completes
		//! (retriable=true marks this as the transient reject a wire
		//! client may resubmit verbatim).  That mTxnOpen check is the
		//! unsynchronized read the headline's proviso refers to: it relies
		//! on the main-thread contract (mMutex does not cover the flag;
		//! see its member doc), so a future async transport must marshal
		//! commits to the main thread.
		AgentCommitResult ApplyAgentParamEdit(
			const String& entityName,
			const String& entityKind,
			const String& param,
			const String& value,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Model-B F5 slice S2: route an agent CHUNK INSERT through the SAME
		//! render-thread-SAFE critical section as ApplyAgentParamEdit (mTxnOpen
		//! refusal FIRST -> cancel-and-park under mMutex -> conflict gate ->
		//! Job::ApplyCstInsertChunk -> rebind (an insert is ALWAYS D2-class:
		//! codes 2/3 replace the Scene + managers) -> MarkCstHeadDirty ->
		//! re-render kick -> post-commit head read).  `chunkText` must be ONE
		//! complete `keyword { ... }` chunk (contract on the IJob virtual).
		//! The result's chunkKeyword/chunkName echo the parsed identity; Job's
		//! negative refusal codes (-1 malformed / -2 duplicate) are normalized
		//! to rawCode=0 / status="rejected" with a specific message.
		AgentCommitResult ApplyAgentInsertChunk(
			const String& chunkText,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Model-B F5 slice S2: route an agent CHUNK REMOVE through the same
		//! critical section (see ApplyAgentInsertChunk).  `target` is the bare
		//! chunk name; `kind` (may be empty) narrows a cross-category clash
		//! with the SAME resolution rules as ApplyAgentParamEdit.  The erase is
		//! the TRIVIA-PRESERVING Cst::DocEraseChunkTidy (safe for file-authored
		//! chunks); a still-referenced target fails Job's dry-run and is
		//! rejected with the first diagnostic, head byte-identical.
		AgentCommitResult ApplyAgentRemoveChunk(
			const String& target,
			const String& kind,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Facet 5 (preview-render safety): run `fn` with the render thread
		//! CANCEL-AND-PARKED under mMutex -- for a caller that needs to
		//! transiently mutate LIVE, non-Document state that the interactive
		//! render loop ALSO touches unsynchronized (DoOneRenderPass's
		//! per-pass Film-dims / camera-frame swap runs with no lock against
		//! anything outside this controller).  This is NOT a Document edit:
		//! it does not go through ApplyCstParamEdit, does not bump the
		//! head-version, and does not mark anything dirty -- it exists so
		//! the Agent surface's preview `render` (transient film-dims /
		//! camera-pose override, capture-set-render-restore) cannot race
		//! DoOneRenderPass's swap of the SAME shared Film/cameras.  `fn` is
		//! invoked exactly once, synchronously, on the calling thread, with
		//! the render thread parked and mMutex HELD; `fn` must not re-enter
		//! the controller -- in particular it must not call
		//! CurrentRenderJob() or any other mMutex-taking method on this
		//! object.  mMutex is a plain (non-recursive) std::mutex, so
		//! re-entering it from `fn` is an immediate self-deadlock, not a
		//! stall-and-retry.  Refused (returns false, `fn` NOT invoked) while
		//! an editor transaction is open -- parking here would stall the
		//! gesture (same rule as ApplyAgentParamEdit's mTxnOpen refusal).
		//! Returns true iff `fn` ran.
		bool RunPreviewRenderParked( const std::function<void()>& fn );

		//! Model-B F2 slice S1: SAME as RunPreviewRenderParked above
		//! (including the non-recursive-mMutex / no-reentrancy contract on
		//! `fn`), plus render-identity bookkeeping -- assigns a fresh
		//! monotonic RenderJobId, records {id, renderClass, active=true}
		//! under the SAME mMutex hold this method already takes for the
		//! cancel-and-park (zero new synchronization), runs `fn`, then
		//! marks the job record inactive before returning -- via an RAII
		//! guard, so `active` is flipped false on EVERY exit, including an
		//! exception unwinding out of `fn` (a real throw site: `fn` is
		//! typically AgentSession's doRenderWork, which calls
		//! mJob->Rasterize(), and OIDN denoise is a documented real throw
		//! source -- see AgentSession.h's Render(AgentRenderParams) doc).
		//! `clientLabel` is an optional free-form diagnostic tag (e.g. the
		//! agent transport's session id) -- not interpreted, purely for
		//! later observability.  `outJobId` (when non-null) receives the
		//! assigned id on the path that actually runs `fn` -- INCLUDING
		//! when `fn` throws (the job existed and ran; an id names a call
		//! that ran, not a call that succeeded -- mirrors
		//! AgentRenderResult::renderJobId's field doc that a FAILED render
		//! still carries its renderJobId).  On refusal (mTxnOpen open, `fn`
		//! never invoked) `*outJobId` is left UNTOUCHED and the counter
		//! does not advance.  Returns true iff `fn` ran (identical refusal
		//! contract to the base overload) -- note a throw out of `fn`
		//! propagates PAST this call (it returns true only on the ordinary
		//! path; an exception unwinds through instead of returning at all).
		bool RunPreviewRenderParked(
			const std::function<void()>& fn,
			RenderClass                  renderClass,
			const String&                clientLabel,
			RenderJobId*                 outJobId );

		//! Model-B F2 slice S1: snapshot of the current render job under
		//! mMutex.  See RenderJobStatus's doc for the "stale when inactive"
		//! semantics.
		RenderJobStatus CurrentRenderJob() const;

		//! Model-B F2 slice S2a: submit `fn` to run OFF the calling thread on
		//! this controller's DEDICATED, long-lived agent-render WORKER, and
		//! return immediately (the mint + handoff happens under a brief
		//! mMutex hold; the calling thread does not block for the render's
		//! duration).  The WORKER -- not the submitter -- takes mMutex,
		//! cancel-and-parks the interactive thread, runs `fn`, and releases,
		//! exactly mirroring RunPreviewRenderParked's critical section
		//! (same lock, same CancelAndParkRender_, same ActiveFlipGuard-
		//! style exception safety) -- just executed on a different thread.
		//! This is what closes the pre-existing race documented on
		//! AgentSession::Render: a plain no-override agent render used to
		//! call mJob->Rasterize() DIRECTLY with no park at all; routing it
		//! through here (or through the synchronous SubmitAgentRenderSync
		//! wrapper below) means EVERY controller-attached agent render is
		//! now serialized against DoOneRenderPass.
		//!
		//! SINGLE-SLOT: only one agent render may be queued/in-flight at a
		//! time.  A submit while a prior one is still queued OR running is
		//! REJECTED (returns false, `fn` is NEVER invoked, the counter does
		//! not advance, `*outJobId` is left untouched) -- this is not a
		//! depth-N queue.  Also refused (identical to RunPreviewRenderParked)
		//! while an editor transaction is open.
		//!
		//! Fix-round-1 P1-1: also refused, honestly and immediately, once
		//! Stop() has been (or is being) called -- mAgentRenderStop is
		//! checked under the SAME mAgentRenderSlotMutex hold as the
		//! single-slot check, so a submission racing Stop() either lands
		//! cleanly BEFORE the stop flag is visible (and the worker will run
		//! it -- Stop() joins the worker only AFTER it drains) or is
		//! refused outright ("controller stopped") -- there is no window
		//! where a submission is accepted into a slot the worker will never
		//! service again.
		//!
		//! Fix-round-1 P1-2: also refused when one or more SYNCHRONOUS
		//! callers (SubmitAgentRenderSync) are already WAITING for a fair
		//! turn at the slot ("queued waiters exist") -- an async submitter
		//! must never jump a waiting sync ticket.  See SubmitAgentRenderSync
		//! for the fairness scheme this refusal protects.
		//!
		//! `fn` runs on the WORKER thread, not the caller's -- it must be
		//! self-contained (no thread-affinity assumptions) and, like
		//! RunPreviewRenderParked's `fn`, must not re-enter this controller
		//! (mMutex is non-recursive; the worker already holds it for the
		//! duration of `fn`).
		//!
		//! `outJobId` (when non-null) receives the assigned id the moment
		//! the submission is ACCEPTED (before the worker necessarily starts
		//! running `fn` -- but the job is already recorded {active=true} at
		//! that point, so a concurrent GetRenderJobStatus/WaitForRenderJob
		//! call can observe it immediately).  Returns true iff the
		//! submission was accepted (the worker WILL run `fn`); false on
		//! refusal.
		bool SubmitAgentRenderAsync(
			std::function<void()> fn,
			const String&         clientLabel,
			RenderJobId*          outJobId );

		//! Model-B F2 slice S2a: the SYNCHRONOUS convenience wrapper --
		//! submits `fn` exactly like SubmitAgentRenderAsync, then blocks the
		//! calling thread until the worker finishes running it (or the
		//! submission was refused, in which case this returns false
		//! immediately with no wait).  Used by AgentSession's Render() to
		//! preserve today's "blocks until the render is done" contract while
		//! still routing through the dedicated worker (closing the
		//! no-override race) instead of calling mJob->Rasterize() on the
		//! calling thread directly.  A throw out of `fn` on the worker is
		//! caught there and RE-THROWN on the CALLING thread once the worker
		//! signals completion, so this method's throw contract matches a
		//! direct synchronous call of `fn` -- callers that wrap this in
		//! try/catch see the exact same exception a direct call would have
		//! produced.
		//!
		//! Fix-round-1 P1-2 (fair reservation): under async-submission
		//! contention, a naive "submit-or-reject" sync caller starves --
		//! an async-spam loop wins the single-slot race almost every time
		//! (measured ~0-2/200 sync successes).  Fix: a FIFO ticket queue,
		//! ~40 lines, entirely under the existing mAgentRenderSlotMutex (no
		//! new lock).  A sync call takes the NEXT ticket
		//! (mAgentRenderNextTicket++) then waits until BOTH (a) the slot is
		//! free (!mAgentRenderPending) AND (b) its ticket is the one
		//! currently being served (ticket == mAgentRenderServingTicket) --
		//! so sync waiters are served in the order they arrived, and an
		//! async submitter that shows up while ANY sync ticket is
		//! outstanding is refused outright by SubmitAgentRenderAsync
		//! ("queued waiters exist") rather than allowed to jump the queue.
		//! Async submitters themselves never take a ticket and never wait --
		//! they keep the existing reject-if-busy semantics when no sync
		//! waiter is queued.  The ticket is released (mAgentRenderServingTicket
		//! advanced, waiters notified) on EVERY exit from the wait --
		//! success, refusal, or a `timeoutMs` timeout -- so a timed-out
		//! waiter never strands the queue for whoever is behind it.
		//!
		//! Round-2 P2-C (the gap the round-1 fix left open, now CLOSED):
		//! round-1's own comment at the old call site claimed "release the
		//! ticket, THEN call the public SubmitAgentRenderAsync -- this is
		//! all single-threaded from here, no window for another thread's
		//! SubmitAgentRenderAsync to run between these two lines" -- that
		//! reasoning was wrong for a genuinely CONCURRENT caller: releasing
		//! mAgentRenderSlotMutex between the ticket release and the
		//! round-trip back into SubmitAgentRenderAsync gave a real async
		//! submitter on ANOTHER thread a window to observe
		//! mAgentRenderWaitingSyncCount drop to 0 and win the slot ahead of
		//! the sync waiter whose fair turn it just was.  Fixed by inlining
		//! the mint-and-claim (the private SubmitAgentRenderAsync_Locked
		//! helper, shared with SubmitAgentRenderAsync) so this call NEVER
		//! releases mAgentRenderSlotMutex between "it is now genuinely our
		//! turn" and "the slot is now ours" -- the ticket release and the
		//! slot claim happen under one continuous lock hold.  Verified by
		//! AgentRenderAsyncTest.cpp's RunFairSlotReservationTest, tightened
		//! from the round-1 ">90% of 200" threshold to "all but at most one
		//! of 200" (the one allowance covers a legitimate fairness-wait
		//! timeout under adversarial scheduling, not a lost race) --
		//! red-proved by reverting to the round-1 round-trip call, which
		//! regresses the measured success rate back toward the old bound.
		//!
		//! `timeoutMs` bounds the FAIRNESS WAIT ONLY (the queue-position
		//! wait before this call is even allowed to submit) -- it does NOT
		//! bound the render itself, which this call still waits for
		//! unconditionally once submitted (matching the pre-fix contract:
		//! a caller that reaches the front of the queue always gets its
		//! render's result, however long the render takes).  Default is
		//! generous (30000ms) since a caller that actually wants a tight
		//! queueing deadline should pass one explicitly.  Returns false
		//! (fn NEVER invoked) on a fairness-wait timeout, or on the same
		//! refusal causes as SubmitAgentRenderAsync (open transaction,
		//! Stop() called) discovered once this caller reaches the front.
		bool SubmitAgentRenderSync(
			std::function<void()> fn,
			const String&         clientLabel,
			RenderJobId*          outJobId,
			unsigned int          timeoutMs = 30000 );

		//! Model-B F2 slice S2a: status surface for a render job id.  For a
		//! COORDINATOR (controller-minted, EVEN) id that matches the
		//! CURRENT or MOST-RECENTLY-COMPLETED job on this controller,
		//! returns that job's status (mirrors CurrentRenderJob's "stale
		//! when inactive" semantics -- `active` is only meaningful for the
		//! CURRENT job; an older completed id reports {id, class,
		//! active=false} using whatever the LAST record happens to be, so a
		//! caller should treat a mismatched id defensively -- see below).
		//! ODD (session-local, AgentSession-minted) ids and any id that does
		//! not match the last-known record are reported NOT FOUND: `found`
		//! is false and `status` is a default-constructed RenderJobStatus.
		//! This is a single-record lookup (no historical ring) -- it can
		//! only answer "is THIS the job I currently know about", not "give
		//! me the history of job N". That is sufficient for S2a's Status/
		//! Wait surface: a caller submits, gets an id back, and immediately
		//! polls/waits on that SAME id before anything else runs on this
		//! controller.
		struct RenderJobLookup
		{
			bool            found = false;
			RenderJobStatus status;
		};
		RenderJobLookup GetRenderJobStatus( RenderJobId id ) const;

		//! Model-B F2 slice S2a: block the calling thread until the render
		//! job named by `id` completes, or `timeoutMs` elapses.  Returns
		//! true iff the job was observed to complete (or was ALREADY
		//! complete) within the timeout; false on timeout OR when `id` is
		//! an ODD (session-local) id or otherwise unrecognized (mirrors
		//! GetRenderJobStatus's "not found" contract -- there is nothing on
		//! this controller to wait for).  A `timeoutMs` of 0 polls once
		//! (no wait) -- use this for a non-blocking "is it done yet" check
		//! that still validates the id.
		bool WaitForRenderJob( RenderJobId id, unsigned int timeoutMs ) const;

		//! Fix-round-1 P2-C: trip the cancel signal for an IN-FLIGHT agent
		//! render, WITHOUT blocking.  Reuses the EXACT SAME
		//! CancellableProgressCallback (mCancelProgress) the interactive
		//! loop already uses -- safe to share because the two render
		//! classes are MUTUALLY EXCLUSIVE in time (the agent worker holds
		//! mMutex, via CancelAndParkRender_, for its render's whole
		//! duration, so RenderLoop cannot be mid-pass -- and cannot call
		//! mCancelProgress.Reset() -- while an agent render is in flight;
		//! see RenderLoop's per-pass Reset() site, which only runs under a
		//! BRIEF mMutex hold at pass-start, never contended against the
		//! worker's render-duration hold).  For this to actually ABORT the
		//! render (not just mark it cancelled), the render's progress
		//! callback must be this controller's mCancelProgress -- see
		//! AgentRenderProgress() below, which AgentSession's doRenderWork
		//! installs on the Job before calling Rasterize() when a controller
		//! is attached, exactly mirroring how the interactive rasterizer
		//! gets it.  Safe to call whether or not a render is actually in
		//! flight (a no-op cancel on an idle controller).  Called by
		//! Stop() (so a slow agent render does not stall teardown
		//! unboundedly) and by AgentSession's DrainAsyncRender_ (so a
		//! session teardown mid-render completes promptly instead of
		//! waiting for a possibly-long render to run to completion).
		void CancelAgentRender_();

		//! Fix-round-1 P2-C: the progress callback an agent render's
		//! doRenderWork must install on the Job (via IJob::SetProgress)
		//! before calling Rasterize(), and restore afterward, so
		//! CancelAgentRender_ / Stop() can actually abort an in-flight
		//! agent render instead of merely marking mCancelProgress cancelled
		//! while the rasterizer's block-fetch loop keeps consuming tiles.
		//! Returns mCancelProgress by address -- valid for the
		//! controller's whole lifetime, so AgentSession (which borrows the
		//! controller and never outlives it while attached) can hold the
		//! pointer for the duration of one render without a dangling-ref
		//! concern.  Null-safe by construction: this never returns null.
		IProgressCallback* AgentRenderProgress() { return &mCancelProgress; }

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
		//!
		//! @param suppressInitialRender  When true, the render thread
		//!        skips the one-shot "show something on Start" pass it
		//!        normally runs at startup.  The GUI sets this when it
		//!        restarts the interactive viewport right after a
		//!        production render: the production result is already on
		//!        screen, and an initial preview pass would immediately
		//!        overwrite it (the user-visible "the finished render
		//!        flashes then flips back to the live preview" bug).
		//!        The render thread stays parked until the first real
		//!        edit / gesture, so the production image survives until
		//!        the user actually interacts.  One-shot — consumed on
		//!        the next Start(); a subsequent Start() (e.g. a fresh
		//!        scene load) renders normally.
		void Start( bool suppressInitialRender = false );

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

		// Phase 6.5 (docs/ROUND_TRIP_SAVE_PLAN.md §9.9): save the scene
		// by serializing the Job's retained CST Document whole
		// (SaveEngine::Save -> Cst::SerializeCst) to a `.RISEscene`
		// file: an external-modification guard refuses an in-place
		// save when the loaded file changed on disk after load, the
		// write is atomic (temp + rename), and the engine NoOps when
		// the serialized bytes equal the on-disk file.  Dirty state
		// does not select WHAT is written (the whole Document always
		// is) -- it only gates the GUI Save button; a successful
		// Saved/NoOp Clear()s the DirtyTracker.  Follows
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

		//! Phase 6.5 UI hook: true when anything MAY need saving since
		//! the last load / save.  Conservative: it can be true when a
		//! Save would NoOp (e.g. edit→undo re-marks dirty; Save then
		//! NoOps on byte-equality of the serialized Document).  Drives
		//! the GUI's "Save Scene" button enable state on both platform
		//! shells.  Cheap O(1) — just checks the SceneEditor's dirty
		//! trackers.
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
		//! parse fails, the property is read-only, or (Object edits,
		//! A2) the live edit applied but the CST transform-commit
		//! follow-through failed -- in that case the scene DID change
		//! and re-renders even though this returns false (logged;
		//! false does not always mean "nothing happened").  Routes through
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
		//! Persistence: the clone is ALSO recorded as a faithful camera
		//! chunk in the retained canonical CST Document, so it survives a
		//! D2 full re-derive AND a save->reload (the SaveEngine serializes
		//! the Document).  Undo removes that chunk; redo re-inserts it.
		//! (Historically, on a legacy non-CST scene the clone lived in the
		//! in-memory Scene/Job only and a reload dropped it; post-Slice-6c
		//! every production load is CST-only, so that case no longer
		//! occurs.)
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

		//! Fix-round-4 P2 RED-PROVE test hook.  Called by RenderLoop on
		//! every iteration, unlocked, immediately BEFORE it re-acquires
		//! mMutex to (re-check the agent gate and, if still clear) mint
		//! this pass's mCurrentRenderJob record -- i.e. exactly the seam
		//! a test needs to deterministically land an agent-render mint
		//! (SubmitAgentRenderAsync_Locked) in the window the P2 fix
		//! closes.  No-op in production (empty base implementation);
		//! test overrides can block here until released.
		virtual void ForTest_OnAboutToMintInteractivePass() {}

		//! Fix-round-4 P2 RED-PROVE test hook, the WORKER-side twin of
		//! ForTest_OnAboutToMintInteractivePass above.  Called by
		//! AgentRenderWorkerLoop_ once per occupant, unlocked, immediately
		//! AFTER it releases mAgentRenderSlotMutex (the submission has
		//! already been minted and pulled out of the slot) but BEFORE it
		//! acquires mMutex via CancelAndParkRender_ -- i.e. the narrow real
		//! window between SubmitAgentRenderAsync_Locked's flag-set and the
		//! worker's own mMutex acquisition, which is what RenderLoop's mint
		//! block can otherwise race to grab first.  A test can hold the
		//! worker open here to give RenderLoop's mint attempt a clean,
		//! deterministic shot at that race instead of relying on raw
		//! scheduler timing.  No-op in production.
		virtual void ForTest_OnAgentWorkerAboutToParkRender() {}

	private:
		void RenderLoop();
		void KickRender();

		//! Model-B F2 slice S2a: the dedicated agent-render worker's loop.
		//! Started in the ctor, joined in Stop() (which the dtor calls
		//! unconditionally) -- LONG-LIVED (never
		//! spawn-per-render), so any one-time-per-thread init a future
		//! `fn` might rely on happens exactly once.  Waits on
		//! mAgentRenderCV for {a submission pending | shutdown}; on a
		//! submission, takes mMutex, cancel-and-parks the interactive
		//! thread (CancelAndParkRender_ -- identical critical section to
		//! RunPreviewRenderParked, just run from this thread instead of
		//! the submitter's), runs the pending `fn` under an RAII guard
		//! that flips the job record inactive on every exit (including a
		//! throw), captures any exception into mAgentRenderException
		//! (rethrown to a SubmitAgentRenderSync caller; silently observed
		//! by SubmitAgentRenderAsync callers via GetRenderJobStatus/
		//! WaitForRenderJob only -- they must inspect their own result
		//! plumbing for failure, matching AgentRenderResult's existing
		//! "ok=false" convention), then releases mMutex and notifies
		//! mAgentRenderDoneCV.
		void AgentRenderWorkerLoop_();

		//! Round-2 P2-C: the mint-and-claim core SHARED by SubmitAgentRenderAsync
		//! and SubmitAgentRenderSync -- everything from the post-mTxnOpen
		//! Stop()/single-slot/fair-queue checks through setting
		//! mAgentRenderPending=true, assuming the caller ALREADY HOLDS
		//! mAgentRenderSlotMutex via `slotLk` (this method neither locks nor
		//! unlocks it).  Factored out so SubmitAgentRenderSync can inline the
		//! claim WITHOUT releasing mAgentRenderSlotMutex between releasing its
		//! fairness ticket and claiming the slot -- see SubmitAgentRenderSync's
		//! doc for the exact cross-thread window this closes (a concurrent
		//! SubmitAgentRenderAsync call on ANOTHER thread could otherwise see
		//! mAgentRenderWaitingSyncCount drop to 0 and win the slot in the gap).
		//! Returns true iff the submission was accepted (mints into `*outJobId`);
		//! false on any of the same refusal causes SubmitAgentRenderAsync
		//! documents (Stop() called, slot occupied, `bypassFairQueueCheck` is
		//! false and a fair-queue waiter is registered).  `bypassFairQueueCheck`
		//! is true ONLY for SubmitAgentRenderSync's own call (it holds the lock
		//! continuously from ticket-release through this claim, so ITS OWN
		//! still-registered-until-a-moment-ago ticket must not self-refuse);
		//! SubmitAgentRenderAsync passes false (the normal external-caller rule).
		bool SubmitAgentRenderAsync_Locked(
			std::unique_lock<std::mutex>& slotLk,
			std::function<void()>         fn,
			const String&                 clientLabel,
			RenderJobId*                  outJobId,
			bool                          bypassFairQueueCheck );

		//! Fix-round-3 (churn UAF): GROUND-TRUTH predicate for "the agent-
		//! render worker is not (and will never be, without a fresh
		//! submission) inside a closure for job `id`".  mCurrentRenderJob's
		//! `active` flag is a STATUS RECORD that a completing INTERACTIVE
		//! pass can clear out from under an in-flight AGENT job's record
		//! (see RenderLoop's completion-site comment for the exact
		//! clobber this closes) -- ownership-checked writes (fix-round-3's
		//! other half) close the clobber AT THE SOURCE, but this predicate
		//! exists so a DRAIN (WaitForRenderJob) never has to trust the
		//! status record alone: it cross-checks the slot bookkeeping
		//! (mAgentRenderPending / mAgentRenderJobId), which is the ONLY
		//! state the worker actually mutates to signal "I am done running
		//! this closure" (mAgentRenderPending is cleared, under
		//! mAgentRenderSlotMutex, strictly AFTER fn() has returned -- see
		//! AgentRenderWorkerLoop_'s tail).  Returns true (idle) whenever
		//! `id` is NOT the current agent-slot occupant -- in particular:
		//!   * the slot has never been touched by this id (an
		//!     Interactive-class id, or an id this controller never
		//!     minted) -- vacuously idle, nothing to cross-check;
		//!   * the slot occupant moved on to a DIFFERENT id (this id's
		//!     closure fully returned before the next submission could be
		//!     accepted -- single-slot policy guarantees that ordering);
		//!   * the slot is simply unoccupied.
		//! Returns false ONLY while `id` is the exact occupant CURRENTLY
		//! recorded pending -- i.e. the worker is (or is about to be)
		//! inside that job's closure.
		bool AgentRenderSlotIdleFor_( RenderJobId id ) const;

		//! Cancel-and-park the render thread: trip the rasterizer cancel
		//! flag if a pass is in flight (bumping mCancelCount), then wait on
		//! mCV until mRendering is false -- i.e. the in-flight pass has
		//! drained and released the Scene.  The caller MUST already hold
		//! mMutex via `lk` (this waits on it); on return the render thread
		//! is parked and the caller may safely mutate the Scene / managers
		//! before releasing the lock.  Factored out of the ~dozen inline
		//! copies of this idiom (SetProperty branches, CloneActiveCamera,
		//! the variant switch, Undo/Redo/Rollback) so a new caller (the
		//! Facet-5 agent commit) reuses the SAME park logic rather than
		//! risk a subtly-different re-implementation.
		void CancelAndParkRender_( std::unique_lock<std::mutex>& lk );

		//! Shared-undo U1: capture the CURRENT value (or absence) of `param` on
		//! the entity resolved by (entityName, entityKind) from the retained
		//! CST Document, BEFORE ApplyAgentParamEdit's coming
		//! ApplyCstParamEditChecked call mutates it.  Caller must hold mMutex
		//! (same hold as the coming apply -- no TOCTOU).  Returns false (no
		//! output written) if there is no retained Document or the entity does
		//! not resolve.  See SceneEditController.cpp for the full contract.
		bool CaptureAgentPriorParamValue_(
			const String& entityName, const String& entityKind, const String& param,
			String& outPrevValue, bool& outWasAbsent );

		//! Shared-undo U2: capture the EXACT verbatim bytes + top-level document index of the chunk
		//! `ApplyAgentRemoveChunk` is about to erase, BEFORE the coming `Job::ApplyCstRemoveChunk` call runs --
		//! caller must hold mMutex (same hold as the coming apply -- no TOCTOU).  Resolves via the SAME
		//! `DocFindByNameAnyRole` call + camera-unique-fallback rule Job's remove uses, so the captured chunk is
		//! guaranteed to be the SAME one the remove is about to act on.  `outBytes` is the concatenation, in
		//! document order, of the chunk's own text plus (if present) its immediately-following top-level item --
		//! capturing BOTH unconditionally is deliberately conservative: `Job::ApplyCstRestoreChunkAt`'s caller
		//! (SceneEditor's Undo arm) reinserts exactly whatever was captured here, so over-capturing a separator
		//! that DocEraseChunkTidy will NOT end up tidying away would restore a byte-for-byte WRONG (extra-
		//! separator) Document -- see SceneEditController.cpp for how the post-remove item-count diff trims
		//! `outBytes` down to only the items ACTUALLY dropped.  `outWasRasterizer` records whether the resolved
		//! chunk is itself a `*_rasterizer` chunk (mirrors ApplyCstInsertChunk's P1-B activation rule for Undo).
		//! Returns false (no output written) if there is no retained Document or the target does not resolve.
		bool CaptureAgentChunkForRemoveUndo_(
			const String& target, const String& kind,
			String& outBytes, int& outIndex, bool& outWasRasterizer );

		//! Model-B F5 slice S2: the SHARED body of ApplyAgentInsertChunk /
		//! ApplyAgentRemoveChunk -- the two verbs differ ONLY in which Job
		//! primitive runs inside the parked critical section and in their
		//! rejection wording, so one core carries the whole reviewed commit
		//! pattern (mTxnOpen refusal FIRST -> park under mMutex -> pre-flight
		//! refusals -> conflict gate -> apply -> rebind on 2/3 -> code fold ->
		//! MarkCstHeadDirty + kick on a mutated head).  `isInsert` selects the
		//! primitive; `a` = chunkText (insert) or target name (remove); `b` =
		//! unused (insert) or kind (remove).
		AgentCommitResult ApplyAgentChunkCrud_(
			bool isInsert,
			const String& a,
			const String& b,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Re-point mEditor at the Job's CURRENT scene + managers.  Called at construction AND after any
		//! whole-scene re-derive (a scene_variant switch ClearAll's + recreates the Scene + managers); without
		//! the re-bind the editor's cached scene/manager pointers dangle into freed storage (use-after-free on
		//! the next edit/gizmo/undo).
		void RebindEditorToJob();

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
		//! One-shot, set by Start( true ) before the render thread is
		//! spawned and consumed by RenderLoop on entry.  When set, the
		//! loop skips its initial "show something on Start" pass so the
		//! current on-screen image (the just-finished production render)
		//! is preserved until the first user edit.  See the
		//! suppressInitialRender parameter on Start() for the full
		//! rationale.
		std::atomic<bool>           mSuppressInitialRender;
		std::atomic<bool>           mRendering;
		// Phase 6.5: signals the render loop NOT to start a new pass
		// while a save is in flight (mirror of mRendering for the
		// "saving" direction).  Set inside the locked section of
		// RequestSave; cleared after the engine returns.
		std::atomic<bool>           mSaving;
		// Model-B F2 slice S2a: mirrors mSaving's pattern for the agent-
		// render worker's window.  Without this, there is a real race:
		// SubmitAgentRenderAsync mints {agentJobId, active=true} into
		// mCurrentRenderJob and hands the closure to the worker, but the
		// worker has not yet reached CancelAndParkRender_ (which is what
		// actually sets mRendering / blocks a NEW interactive pass) --
		// during that gap, RenderLoop's own per-pass mint block (which
		// runs unconditionally at the top of every iteration) can win the
		// race for mMutex first and STOMP mCurrentRenderJob with its OWN
		// interactive id, silently losing the agent job's record (caught
		// by this slice's own flaky test run: GetRenderJobStatus stopped
		// finding the freshly-submitted id because RenderLoop had already
		// overwritten it).  Set true (under mMutex, alongside the
		// mCurrentRenderJob write) by SubmitAgentRenderAsync for the
		// FULL duration from mint through worker completion; cleared
		// (under mMutex) by the worker right before it clears the slot.
		//
		// Fix-round-4 P3-2: the paragraph below used to claim RenderLoop's
		// per-pass "mint gate" (not just its wake predicate) already
		// required this flag false, which was NOT what the code did at the
		// time -- the mint block took mMutex and minted unconditionally,
		// with no re-check between the wake predicate/line-4473 snapshot
		// and the mint a good ~60 unlocked lines later.  That gap was a
		// real, reachable clobber (round-4 P2's RED-PROVE test forces it).
		// P2's fix closes it by re-checking this flag a SECOND time, INSIDE
		// the same mMutex hold the mint block itself takes, immediately
		// before minting -- so as of that fix, the claim below is actually
		// true: RenderLoop consults this flag twice (the wake predicate
		// AND the 4473 post-wake snapshot, both before doing any
		// refinement/polish bookkeeping; then again, authoritatively,
		// right before the mint under mMutex) and skips minting outright
		// if it's set at that final check, so the two "job openers" cannot
		// land overlapping mints on mCurrentRenderJob.  The remaining
		// exposure this does NOT need to close: an agent mint that lands
		// AFTER RenderLoop's final in-lock check has already passed (i.e.
		// RenderLoop's mint runs first) is fine as-is -- both mint sites'
		// own completion writes are ownership-checked (fix-round-3, churn
		// UAF) against the id they themselves minted, so whichever pass's
		// mint loses the race still gets its own clean completion later;
		// only an UNGUARDED unconditional mint clobbering an ALREADY-
		// LANDED record was ever the bug.
		std::atomic<bool>           mAgentRenderBlocksInteractive;
		std::string                 mLastSaveError;
		std::atomic<unsigned int>   mCancelCount;
		std::atomic<unsigned int>   mRenderCount;

		// Model-B F2 slice S1: render-identity bookkeeping.  The counter
		// starts at 2 and increments by kControllerRenderJobIdStride (EVEN
		// ids only; 0 = kInvalidRenderJobId / "none assigned yet"; see that
		// constant's doc for why -- disjoint from AgentSession's ODD
		// session-local ids) and is SHARED across both RenderClass values
		// (RunPreviewRenderParked's agent-preview path and RenderLoop's
		// interactive pass both draw from it), so ids are globally ordered
		// across classes on one controller.
		//
		// Model-B F2 slice S2a fix: BOTH fields moved from "guarded by
		// mMutex" to their OWN dedicated mJobStatusMutex.  Reason: mMutex
		// is held by BOTH RunPreviewRenderParked AND the S2a worker for
		// the RENDER'S WHOLE DURATION (that hold is what gives
		// CancelAndParkRender_ its exclusivity) -- so a STATUS READER
		// (GetRenderJobStatus / CurrentRenderJob / WaitForRenderJob) that
		// also locked mMutex would BLOCK for the entire render before it
		// could even READ the status, making "observe the job ACTIVE
		// while it runs" impossible (caught by this slice's own flaky
		// test: every poll during a 200ms render blocked until the
		// render finished, then read `active=false` because by then it
		// HAD finished -- looked like a missed update, was actually lock
		// contention).  mJobStatusMutex is NEVER held across a render --
		// every writer (RenderLoop's two sites, SubmitAgentRenderAsync's
		// mint, both ActiveFlipGuards) and every reader (CurrentRenderJob,
		// GetRenderJobStatus, WaitForRenderJob's poll) takes it only for
		// the few instructions needed to read or write this record, so a
		// status poll is NEVER blocked behind an in-flight render.
		mutable std::mutex mJobStatusMutex;
		RenderJobId     mNextRenderJobId;
		RenderJobStatus mCurrentRenderJob;

		// Model-B F2 slice S2a: the dedicated, long-lived agent-render
		// worker.  Started next to mRenderThread in the ctor, joined next
		// to it in Stop() -- mirrors mRenderThread's lifecycle exactly so
		// teardown order stays correct (both threads must be joined
		// BEFORE anything they touch -- mJob, mEditor, the sinks -- is
		// destroyed; Stop() joins both before ~SceneEditController runs
		// the rest of its body).
		//
		// THREE locks total on this controller now (mMutex + two below),
		// each deliberately narrow and single-purpose -- the split is the
		// fix for TWO real bugs this slice's own concurrency test caught:
		//
		//   1. mMutex is held by the worker for the render's WHOLE
		//      DURATION (via CancelAndParkRender_, exactly like
		//      RunPreviewRenderParked).  A submitter that needed mMutex to
		//      check/set the single-slot flag would BLOCK for the render's
		//      entire duration before it could even LEARN the slot was
		//      occupied, defeating "refuse immediately" and "returns
		//      quickly" alike.  mAgentRenderSlotMutex (below) is a
		//      SEPARATE, NARROW lock guarding ONLY the slot bookkeeping
		//      (mAgentRenderPending / mAgentRenderFn / the id/label/
		//      exception/generation fields) -- held for microseconds at a
		//      time (check-and-set on submit; pull-the-closure-out at the
		//      start of a worker iteration; clear-and-notify at the end)
		//      and NEVER held across the render itself.
		//
		//   2. mCurrentRenderJob/mNextRenderJobId (declared just above,
		//      with mJobStatusMutex) hit the SAME problem one level down:
		//      even after (1)'s fix, GetRenderJobStatus/CurrentRenderJob/
		//      WaitForRenderJob still used mMutex to READ the job record --
		//      so a caller polling "is it done yet?" during an in-flight
		//      render blocked for the WHOLE render before it could read
		//      anything, making "observe active while running" impossible
		//      (every poll saw the render already finished).
		//      mJobStatusMutex is the fix: a THIRD narrow lock, held only
		//      long enough to read/write the one small record, taken by
		//      every writer (RenderLoop's two sites, SubmitAgentRenderAsync,
		//      both ActiveFlipGuards) and every reader, NEVER held across a
		//      render by anyone.
		//
		// Nesting order is the ONLY order used anywhere, so none of the
		// three locks can deadlock against each other: mAgentRenderSlotMutex
		// may be held OUTSIDE a brief, nested mMutex or mJobStatusMutex
		// acquisition (SubmitAgentRenderAsync does both, in that order);
		// mMutex may be held OUTSIDE a brief, nested mJobStatusMutex
		// acquisition (RenderLoop, the worker, RunPreviewRenderParked); no
		// path ever acquires mJobStatusMutex or mMutex and THEN tries to
		// acquire mAgentRenderSlotMutex while still holding it (the worker
		// and WaitForRenderJob both release one before acquiring the
		// other).
		//
		// Round-2 P1-2 ADDS a fourth lock to this table, on the CALLER side
		// (AgentSession::mAsyncCacheMutex -- a different class, but the two
		// nest, so the ordering belongs in the same table): AgentSession::
		// RenderAsync holds mAsyncCacheMutex across its ENTIRE call into
		// SubmitAgentRenderAsync (which takes mAgentRenderSlotMutex
		// internally, nesting mMutex/mJobStatusMutex under it per the rule
		// above) plus the mAsyncOutstandingJobId publish that follows --
		// see RenderAsync's own comment for why.  The SUBMIT-TIME order,
		// OUTERMOST to INNERMOST:
		//
		//   AgentSession::mAsyncCacheMutex
		//     -> SceneEditController::mAgentRenderSlotMutex
		//          -> SceneEditController::mMutex           (brief, mAgentRenderBlocksInteractive)
		//          -> SceneEditController::mJobStatusMutex
		//
		// (mMutex and mJobStatusMutex are siblings under mAgentRenderSlotMutex,
		// never nested under each other -- see the three-lock note above.)
		//
		// A SEPARATE, LATER call chain nests mMutex and mAsyncCacheMutex in
		// the OPPOSITE order -- the worker thread, mid-render, holds mMutex
		// (via CancelAndParkRender_, for the render's whole duration) and
		// its `fn()` -- RenderCore_'s cache-population tail, at the very
		// end of a SUCCESSFUL render -- takes mAsyncCacheMutex while that
		// mMutex hold is still live.  This is NOT a deadlock risk despite
		// being the reverse order, because the two acquisitions can never
		// be SIMULTANEOUS CONTENDERS for the same pair: the single-slot
		// check inside SubmitAgentRenderAsync_Locked (mAgentRenderPending
		// must be false to proceed) is only satisfied once the worker has
		// already RELEASED mMutex for the render it just finished (the
		// worker releases mMutex, at line ~3566, strictly BEFORE it clears
		// mAgentRenderPending a few lines later under a fresh
		// mAgentRenderSlotMutex acquisition) -- so by the time a NEW
		// RenderAsync call's brief, nested mMutex acquisition can even be
		// reached, no worker is holding mMutex from a PRIOR render, and the
		// worker servicing THIS NEW submission cannot yet exist (it hasn't
		// been woken). The two orderings therefore apply to disjoint
		// instants in time, not to the same lock pair racing itself.
		// DrainAsyncRender_ takes AT MOST one lock at a time (mAsyncCacheMutex
		// to read/clear the id; separately, unlocked, calls into the
		// controller's own CancelAgentRender_/WaitForRenderJob, which take
		// their own internal locks with no AgentSession lock held) -- no
		// ordering concern there.
		std::thread                 mAgentRenderThread;
		mutable std::mutex          mAgentRenderSlotMutex;
		std::condition_variable     mAgentRenderCV;       // worker waits on this for {pending | stop} -- guarded by mAgentRenderSlotMutex
		// mutable: WaitForRenderJob is logically const (a read-only status
		// poll) but must block on this CV.
		mutable std::condition_variable mAgentRenderDoneCV;   // submitter(s) wait on this for "the slot freed up" -- guarded by mAgentRenderSlotMutex
		std::atomic<bool>           mAgentRenderStop;      // set by Stop(); wakes the worker to exit
		bool                        mAgentRenderPending;   // a submission is queued or currently running -- guarded by mAgentRenderSlotMutex
		std::function<void()>       mAgentRenderFn;        // the pending/running submission -- guarded by mAgentRenderSlotMutex
		// Fix-round-1 P3-c: mAgentRenderClass / mAgentRenderClientLabel
		// (a second, slot-scoped copy of this bookkeeping) were DELETED --
		// both were write-only dead state (set on every submit, never read
		// by anything).  mCurrentRenderJob.renderClass / .clientLabel
		// (guarded by mJobStatusMutex, populated at the SAME submit sites)
		// already carry the identical information to the surface that
		// actually reads it (GetRenderJobStatus / CurrentRenderJob).
		RenderJobId                 mAgentRenderJobId;     // the id assigned to the CURRENT slot occupant -- guarded by mAgentRenderSlotMutex
		//! Set by the worker just before it signals completion on
		//! mAgentRenderDoneCV: true iff mAgentRenderFn threw.  Consumed
		//! (and cleared) by SubmitAgentRenderSync, which rethrows via
		//! std::rethrow_exception so its own caller sees the identical
		//! exception a direct synchronous call of `fn` would have
		//! produced.  A SubmitAgentRenderAsync caller does not consume
		//! this -- it has no synchronous point to rethrow into -- so it
		//! is cleared unconditionally at the START of every new
		//! submission (never allowed to linger past the slot it
		//! belonged to).  Guarded by mAgentRenderSlotMutex.
		std::exception_ptr          mAgentRenderException;

		//! Fix-round-1 P1-2: FIFO fairness ticket scheme guarding the
		//! single slot against systematic sync-caller starvation under
		//! async-submission contention.  All THREE fields guarded by
		//! mAgentRenderSlotMutex (same lock as the rest of the slot
		//! bookkeeping -- no new lock).
		//!
		//!   mAgentRenderNextTicket    -- monotonic counter; a sync waiter
		//!                                claims ticket = mAgentRenderNextTicket++
		//!                                on arrival.
		//!   mAgentRenderServingTicket -- the ticket currently allowed to
		//!                                submit.  A sync waiter blocks
		//!                                until BOTH the slot is free AND
		//!                                its ticket == this value; on
		//!                                taking its turn (successfully OR
		//!                                on refusal/timeout -- see
		//!                                SubmitAgentRenderSync) it
		//!                                advances this by 1 and notifies,
		//!                                releasing the NEXT queued waiter.
		//!   mAgentRenderWaitingSyncCount -- how many sync callers are
		//!                                CURRENTLY queued (claimed a
		//!                                ticket, not yet released it).
		//!                                SubmitAgentRenderAsync refuses
		//!                                outright ("queued waiters exist")
		//!                                whenever this is nonzero, so an
		//!                                async submitter can never jump a
		//!                                waiting sync ticket.
		//!
		//! Replaces the former mAgentRenderCompletedGeneration (write-only
		//! dead state -- bumped by the worker, never read by anything):
		//! mAgentRenderServingTicket already serves as a monotonic
		//! generation counter for the slot's occupancy history, so a
		//! second counter added nothing.
		unsigned long long          mAgentRenderNextTicket;
		unsigned long long          mAgentRenderServingTicket;
		unsigned int                mAgentRenderWaitingSyncCount;

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
