//////////////////////////////////////////////////////////////////////
//
//  SceneEditController.h - Cross-platform interactive scene editor
//    controller. Owns SceneEditor (mutation), interactive preview
//    rasterizer, toolbar state machine, and cancel-restart render
//    thread (docs/INTERACTIVE_EDITOR_PLAN.md §4.6).
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
#include "../Interfaces/ITransformable.h"
#include "SaveEngine.h"
#include "CancellableProgressCallback.h"
#include "CameraIntrospection.h"
#include "ReferenceGraph.h"
#include "ConnectionLegality.h"
#include "OwnershipClosure.h"
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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class PlaybackClock;
		class LiveSimulation;
		struct PlaybackStatus;
		struct LiveSimulationStatus;
	}
	namespace Simulation { class MeshCacheSource; struct BakeSceneSnapshot; }

	//! Shells display real render pixels opaque even at coverage alpha 0;
	//! negative alpha is reserved for the Last Render empty-placeholder sentinel.
	inline unsigned char ViewportShellDisplayAlpha8( const RISEColor& c )
	{
		return c.a < 0.0 ? 0 : 255;
	}

	namespace Implementation { class InteractivePelRasterizer; }
	namespace Implementation { class FrameStore; }
	class IRasterImage;
	//! Forward declaration (docs/gui/RENDER_MODES.md §5; underlying type int).
	namespace Implementation { enum class ViewportRenderMode; }

	class SceneEditController
	{
	public:
		//! Toolbar mode driving OnPointerMove. Numeric values are C-API ABI -- do not reorder.
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

		//! Toolbar category slots with per-slot last-used sub-tool memory.
		//! C-API ABI -- do not reorder. ScrubTimeline lives in the timeline bar, not here.
		enum class ToolCategory : int
		{
			Select          = 0,   ///< { Select }              (single sub-tool, no flyout)
			Camera          = 1,   ///< { Orbit, Pan, Zoom, Roll }
			ObjectTransform = 2    ///< { Translate, Rotate, Scale } — needs gizmos
		};

		static constexpr int kNumToolCategories = 3;

		//! Maps every Tool to a ToolCategory (ScrubTimeline falls back to Select).
		static ToolCategory CategoryForTool( Tool t );

		//! Default sub-tool for a category slot before any user pick.
		static Tool DefaultSubToolForCategory( ToolCategory cat );

		//! Most recently picked sub-tool for `cat` (or its default).
		Tool GetLastSubToolForCategory( ToolCategory cat ) const;

		//! Screen-space world-axis gizmo handle in current full-res camera pixel space.
		//! Refreshed on demand via RefreshGizmoHandles().
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

		//! Recompute gizmo handles for current selection/tool/camera (0 handles if
		//! non-transform tool, no Object selected, or degenerate camera projection).
		void RefreshGizmoHandles();

		unsigned int GizmoHandleCount() const;
		int          GizmoHandleKind( unsigned int idx ) const;
		int          GizmoHandleAxis( unsigned int idx ) const;
		double       GizmoHandleScreenX( unsigned int idx ) const;
		double       GizmoHandleScreenY( unsigned int idx ) const;
		double       GizmoHandleScreenRadius( unsigned int idx ) const;

		//! Test hook: project world point to image-pixel space (false if behind camera / degenerate).
		bool ForTest_ProjectWorldToScreen( double wx, double wy, double wz,
		                                   double& outSx, double& outSy ) const;

		//! Test hook: world-space gizmo pivot (selected object's FinalTransformMatrix translation).
		bool ForTest_GetSelectionPivotWorld( double& wx, double& wy, double& wz ) const;

		//! Test hook: apply world-space translate op (conjugated into parent frame) to selected Object.
		bool ForTest_TranslateSelectedObjectWorld( double dx, double dy, double dz );

		//! Hit-test gizmo handles in image-pixel space (-1 on miss).
		//! Array order defines priority (center/planes/rings before arrows).
		int  GizmoHandleAt( const Point2& px ) const;

		//! True while a gizmo-handle drag is active between OnPointerDown and OnPointerUp.
		bool IsGizmoDragActive() const;

		//! Active drag handle kind/axis (`GizmoHandle::Kind` / 0..2), or -1 when inactive.
		int  ActiveGizmoKind() const;
		int  ActiveGizmoAxis() const;

		//! Navigation axis-ball nubs (+X/-X/+Y/-Y/+Z/-Z; docs/gui/CAMERAS_AND_VIEWS.md §4).
		//! Clicking a nub snaps the transient view non-destructively (SnapViewToAxis).
		struct NavGizmoNub
		{
			int    axis         = 0;      ///< 0=X, 1=Y, 2=Z (C-API surface)
			bool   negative     = false;  ///< false=+axis, true=−axis
			double screenX      = 0.0;    ///< nub center, in the caller's ball-geometry space
			double screenY      = 0.0;
			double screenRadius = 0.0;    ///< hit-test / draw radius (== nubRadius arg)
			bool   facing       = true;   ///< true=toward viewer (bright), false=away (dim)
		};

		//! Recompute 6 axis-ball nubs in caller widget space (0 if no pinhole camera or non-positive radii).
		bool RefreshNavGizmo( double centerX, double centerY,
		                      double ballRadius, double nubRadius );

		unsigned int NavGizmoNubCount() const;   ///< 0 or 6
		//! Read nub `idx`; false (outputs untouched) when out of range.
		bool NavGizmoNubInfo( unsigned int idx, int& outAxis, bool& outNegative,
		                      double& outScreenX, double& outScreenY,
		                      double& outScreenRadius, bool& outFacing ) const;

		//! Hit-test nubs in caller widget space (front-facing nubs win ties; -1 on miss).
		int NavGizmoNubAt( double px, double py ) const;

		//! Accordion / selection category discriminator. C-API ABI -- do not reorder;
		//! values 0..7 match PanelMode.
		enum class Category : int
		{
			None       = 0,   ///< no selection — accordion fully collapsed
			Camera     = 1,   ///< Cameras section, picking activates SetActiveCamera
			Rasterizer = 2,   ///< Rasterizer section, picking activates SetActiveRasterizer
			Object     = 3,   ///< Objects section, picking from list or viewport
			Light      = 4,   ///< Lights section
			Film       = 5,   ///< Output Settings section (single Film per scene)
			Material   = 6,   ///< Materials section
			Medium     = 7,   ///< Participating media (Homogeneous editable; Heterogeneous read-only)
			Animation  = 8,   ///< Named animation paths (picking activates)
			SceneVariant = 9, ///< scene_variant overlays; picking one RE-DERIVES the scene with that variant active.
			Painter    = 10,  ///< Painters (IPainter + IScalarPainter union; PanelMode::None)
			Geometry   = 11,  ///< Geometry (IGeometryManager; CST introspection; PanelMode::None)
			Simulation = 12   ///< Scene simulations (CST introspection; PanelMode::None)
		};

		//! Category array bound (None..Simulation).
		static constexpr int kNumCategories = 13;

		//! Monotonic render job identity (0 = kInvalidRenderJobId).
		typedef std::uint64_t RenderJobId;
		static constexpr RenderJobId kInvalidRenderJobId = 0;

		//! Coordinator job ids are EVEN (start 2, stride 2); AgentSession session-local
		//! ids are ODD (start 1, stride 2), keeping both disjoint within exact JSON double range.
		static constexpr RenderJobId kControllerRenderJobIdStride = 2;

		//! Render job class. All classes are mutually exclusive on Rasterize() via the single-slot worker.
		enum class RenderClass
		{
			Interactive  = 0,
			AgentPreview = 1,
			Production   = 2
		};

		//! Cause of a coordinated-render refusal (reported via `outRefusal` on
		//! RunPreviewRenderParked / SubmitAgentRenderAsync / SubmitAgentRenderSync).
		//! Retriability is part of each enumerator's contract.
		enum class RenderRefusal
		{
			//! No refusal (also left on throw out of `fn`).
			None = 0,

			//! Terminal Stop()/teardown in progress. NOT retriable.
			ControllerStopped,

			//! Transaction, pointer gesture, scrub, save, or composite open. Retriable.
			EditorBusy,

			//! Another non-pinned coordinated render owns the gate/slot, or fairness wait timed out. Retriable.
			CoordinatedRenderBusy,

			//! Transient FinalizeOpenInteractions failure (open gesture/scrub/composite). Retriable.
			InteractionFinalizeFailed,

			//! Sticky mInteractionPersistenceFailed is latched (a prior pending CST commit failed).
			//! NOT retriable and never clears for the life of this controller.
			InteractionFinalizeLatched,

			//! Submit paths only: single render slot is occupied by a PINNED job. Retriable once done.
			PinnedRenderBusy
		};

		//! Snapshot of current render job (read under mJobStatusMutex, never mMutex).
		//! `active == true` covers all render classes; fields are stale/informational when inactive.
		//! Published as a whole record at every mint site. Do NOT use `pinned` to infer refusal cause.
		struct RenderJobStatus
		{
			RenderJobId  id          = kInvalidRenderJobId;
			RenderClass  renderClass = RenderClass::Interactive;
			bool         active      = false;
			String       clientLabel;
			bool         pinned      = false;
		};

		//! Result of a controller-routed agent CST commit:
		//!   rawCode 1/2 -> applied=true,  status="applied"   (incremental / full re-derive)
		//!   rawCode 3   -> applied=false, status="diagnosed" (mutated + managers replaced, with diagnostics)
		//!   rawCode 0   -> applied=false, status="rejected"  (head untouched; `retriable` true iff open txn)
		//!   conflict    -> applied=false, status="conflict"  (baseVersion mismatch; head untouched)
		struct AgentCommitResult
		{
			bool                       applied = false;
			bool                       conflict = false;
			bool                       retriable = false; //!< true = transient refusal (open txn)
			int                        rawCode = 0;      //!< 0 reject/conflict / 1 incremental / 2 D2 / 3 replaced-but-diagnosed
			String                     status;           //!< "applied" / "rejected" / "diagnosed" / "conflict"
			RISE::Cst::CstHeadVersion  headVersion;      //!< the head-version AFTER the call
			String                     message;
			//! Echoed chunk keyword/name for chunk-CRUD verbs (empty for param edits).
			String                     chunkKeyword;
			String                     chunkName;
			//! ApplyAgentEditBatch: isolation budget exceeded (committed nothing; retry sequentially).
			bool                       needsSequential = false;
			//! True iff refusal was a would-not-derive failure (Job code 0), not a policy/syntax gate.
			bool                       wouldNotDerive = false;
		};

		//! Thread-safe CST param edit via Job::ApplyCstParamEditChecked under cancel-and-park mMutex.
		//! Refused (`retriable=true`, non-mutating) while mTxnOpen; conflicts on non-null baseVersion
		//! mismatch. Rebinds on D2 (codes 2/3), marks dirty, records undo, and kicks render.
		//! `occ`/`occAddressed` address repeatable params (arming the undo/redo drift guard).
		AgentCommitResult ApplyAgentParamEdit(
			const String& entityName,
			const String& entityKind,
			const String& param,
			const String& value,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull,
			int occ = 0, bool occAddressed = false );

		//! Thread-safe CST chunk insert (single `keyword { ... }` chunk) via Job::ApplyCstInsertChunk.
		//! Same transaction/conflict/cancel-and-park/rebind/dirty/undo/kick contract as ApplyAgentParamEdit.
		AgentCommitResult ApplyAgentInsertChunk(
			const String& chunkText,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Thread-safe trivia-preserving CST chunk remove (`DocEraseChunkTidy`).
		//! Still-referenced targets fail dry-run and are rejected with the head byte-identical.
		AgentCommitResult ApplyAgentRemoveChunk(
			const String& target,
			const String& kind,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Atomic batch chunk remove via Job::ApplyCstRemoveChunks (all-or-nothing, one head bump,
		//! one EditHistory entry). `chunkName`/`chunkKeyword` echo comma/'\n'-joined targets/kinds.
		AgentCommitResult ApplyAgentRemoveChunks(
			const std::vector<String>& targets,
			const std::vector<String>& kinds,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Batched agent inserts/param edits under one mMutex hold via Job::ApplyCstEditBatch
		//! (one dry-run + one full re-derive, one head bump, one EditHistory entry; no baseVersion).
		//! Exceeding dry-run isolation budget commits nothing and marks admitted elements `needsSequential`.
		//! Code-3 diagnostics are attributed per element where possible.
		std::vector<AgentCommitResult> ApplyAgentEditBatch( const std::vector<CstBatchEdit>& edits );

		//! Atomic whole-document swap via Job::ApplyCstReplaceDocumentText (one head bump, one
		//! EditHistory entry). Callers computing candidates outside mMutex MUST pass `baseVersionOrNull`.
		//! Pass `entityKind=""` when `objectName` is a verb label rather than a real entity name.
		AgentCommitResult ApplyAgentReplaceGeometry(
			const String& objectName,
			const String& candidateDocText,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull,
			const char* verbLabel = "replace_geometry_scaffold",
			const char* entityKind = "standard_object" );

		//! Verb kind replayed when an AgentProposal is approved.
		enum class AgentProposalKind
		{
			ParamEdit    = 0,   //!< replays via ApplyAgentParamEdit (entity/kind/param/value)
			InsertChunk  = 1,   //!< replays via ApplyAgentInsertChunk (chunkText)
			RemoveChunk  = 2,   //!< replays via ApplyAgentRemoveChunk (target/kind)
			//! Replays via ApplyAgentRemoveChunks (one atomic proposal for the whole batch).
			RemoveChunks = 3
		};

		//! Inert proposal staged by an External-authority AgentSession for Owner approval.
		//! Replayed via ApplyAgent* on approval if baseVersion still matches.
		struct AgentProposal
		{
			std::uint64_t       id = 0;         //!< monotonic, unique within this controller's lifetime; never 0 (0 = "not found" sentinel)
			AgentProposalKind   kind = AgentProposalKind::ParamEdit;
			String              target;
			String              entityKind;
			String              param;
			String              value;
			String              chunkText;
			//! RemoveChunk uses target/entityKind; RemoveChunks packs '\n'-separated lists into both.
			RISE::Cst::CstHeadVersion baseVersion;   //!< the head this proposal was staged against
			//! If false, StageProposal stamps `baseVersion` atomically from the live head; if true, keeps caller's.
			bool                hasExplicitBaseVersion = false;
			//! True if staging session's part-plan gate was armed; ResolveProposal re-checks
			//! geometry-introducing delta on ParamEdit against the current head.
			bool                buildPlanGateArmedAtStage = false;
			String              sessionLabel;        //!< diagnostic session label
			String              status;              //!< "pending" / "applied" / "rejected" / "conflict"
		};

		//! Fail-closed cap on simultaneously pending proposals.
		static const std::size_t kMaxPendingProposals = 32;

		//! Total proposal storage cap (pending + resolved; oldest resolved evicted when full).
		static const std::size_t kMaxProposalHistory = 8 * kMaxPendingProposals;

		//! Stage an inert proposal (no Document mutation, render kick, or history record).
		//! Stamps `baseVersion` under render admission when `!hasExplicitBaseVersion` and writes
		//! it to `outStagedVersion`. Returns nonzero id, or 0 if pending queue is full or tearing down.
		//! Thread-safe without waiting on render-duration mMutex holds.
		std::uint64_t StageProposal( const AgentProposal& proposal,
		                             RISE::Cst::CstHeadVersion* outStagedVersion = nullptr );

		//! Snapshot of all queued proposals (pending + resolved) under mProposalMutex.
		std::vector<AgentProposal> ListProposals() const;

		//! Approve or reject proposal `id`. Caller (AgentSession) enforces Owner-only authority.
		//! Reject is unconditional (fills `outResult` with current head). Approve checks
		//! `baseVersion` atomically under mMutex (marks "conflict" on mismatch) and replays via
		//! ApplyAgent*; a `retriable` refusal leaves the proposal pending. Returns false on unknown/
		//! already-resolved id or render-admission refusal (distinguishable via `outResult`).
		bool ResolveProposal( std::uint64_t id, bool approve, AgentCommitResult* outResult = nullptr );

		//! Run `fn` synchronously on caller thread with interactive render parked and non-recursive
		//! mMutex held (`fn` must not call mMutex-taking controller methods; CurrentRenderJob() is safe).
		//! Refuses (returns false, `fn` not run) if txn/gesture/save open or another coordinated render owns gate.
		bool RunPreviewRenderParked( const std::function<void()>& fn );

		//! Same as RunPreviewRenderParked above, plus RenderJobId minting and RAII active-flag tracking
		//! (exception-safe: throws out of `fn` propagate; `*outJobId` is written whenever `fn` runs).
		//! `*outRefusal` is seeded to `None` on entry and overwritten on refusal (leaves `None` iff `fn` ran).
		bool RunPreviewRenderParked(
			const std::function<void()>& fn,
			RenderClass                  renderClass,
			const String&                clientLabel,
			RenderJobId*                 outJobId,
			RenderRefusal*               outRefusal = nullptr );

		//! Non-blocking snapshot of current render job under mJobStatusMutex (safe inside render closures).
		//! `active` includes interactive passes; use `outRefusal` to diagnose coordinated-render gates.
		RenderJobStatus CurrentRenderJob() const;

		//! Submit `fn` to the dedicated single-slot agent-render worker and return immediately.
		//! Worker parks interactive render under mMutex and runs `fn` (must be thread-agnostic and
		//! must not re-enter mMutex). Refuses (false, `outRefusal` set) if Stop() called, slot busy,
		//! sync fairness waiters queued, txn/gesture/save open, or occupant is `pinned`.
		//! `pinned` blocks silent supersession by new submissions, NOT CancelAgentRender_/Stop().
		bool SubmitAgentRenderAsync(
			std::function<void()> fn,
			const String&         clientLabel,
			RenderJobId*          outJobId,
			bool                  pinned = false,
			RenderClass           renderClass = RenderClass::AgentPreview,
			RenderRefusal*        outRefusal = nullptr );

		//! Synchronous single-slot render submission with FIFO fairness ticket queue (prevents
		//! starvation against async submitters). `timeoutMs` bounds the queue wait only; once
		//! accepted, blocks until worker completes and rethrows any exception from `fn`.
		//! Waits behind any occupant (including `pinned`); on timeout `outRefusal` distinguishes
		//! `PinnedRenderBusy` vs `CoordinatedRenderBusy` under the slot lock.
		bool SubmitAgentRenderSync(
			std::function<void()> fn,
			const String&         clientLabel,
			RenderJobId*          outJobId,
			unsigned int          timeoutMs = 30000,
			bool                  pinned = false,
			RenderClass           renderClass = RenderClass::AgentPreview,
			RenderRefusal*        outRefusal = nullptr );

		//! Route a production render through the single-slot worker tagged RenderClass::Production
		//! (same fairness/refusal/rethrow contract as SubmitAgentRenderSync; interactive ROI preserved).
		bool SubmitProductionRenderSync(
			std::function<void()> fn,
			const String&         clientLabel,
			RenderJobId*          outJobId,
			unsigned int          queueTimeoutMs = 30000 );

		//! Shared shell helper routing `doRasterize` through SubmitProductionRenderSync (or direct
		//! call if `controller == nullptr`). Composes `guiProgress` as `mCancelProgress` inner so
		//! either UI cancel or coordinator Stop/Cancel aborts the render; captures prior Job progress
		//! inside the slot and RAII-restores both Job progress and inner pointer under mMutex.
		static bool RunProductionRenderComposed(
			IJobPriv&                     job,
			SceneEditController*          controller,
			const String&                clientLabel,
			IProgressCallback*            guiProgress,
			const std::function<bool()>& doRasterize,
			unsigned int                  queueTimeoutMs = 30000 );

		//! Single-record status lookup for coordinator (EVEN) job ids (no historical ring).
		//! ODD or mismatched ids return `found = false`.
		struct RenderJobLookup
		{
			bool            found = false;
			RenderJobStatus status;
		};
		RenderJobLookup GetRenderJobStatus( RenderJobId id ) const;

		//! Wait up to `timeoutMs` (0 = non-blocking poll) for coordinator job `id` to complete.
		//! Returns false on timeout or unknown/ODD id.
		bool WaitForRenderJob( RenderJobId id, unsigned int timeoutMs ) const;

		//! Non-blocking cancel signal for an in-flight/queued agent render (retained across submit-
		//! to-worker handoff). Effective when the render installs `AgentRenderProgress()` on the Job.
		void CancelAgentRender_();

		//! Controller-owned progress callback (`&mCancelProgress`, never null) for agent renders.
		IProgressCallback* AgentRenderProgress() { return &mCancelProgress; }

		//! `job` and `interactiveRasterizer` are borrowed (caller keeps alive; null rasterizer = headless mode).
		SceneEditController( IJobPriv& job, IRasterizer* interactiveRasterizer );
		virtual ~SceneEditController();

		// Lifecycle ---------------------------------------------------

		//! Spawn interactive render thread (idempotent). `suppressInitialRender` (one-shot) skips
		//! the startup pass so a finished production image survives until the next edit/gesture.
		void Start( bool suppressInitialRender = false );

		//! Stop and join ONLY the interactive render thread (idempotent, restartable via Start()).
		//! Keeps the agent-render worker alive so production/agent renders remain serviceable.
		void StopInteractive();

		//! Commit pending object/camera CST deltas and close controller-owned composites for any
		//! open interaction. Called before coordinated render admission.
		bool FinalizeOpenInteractions();

		//! Terminal Job-alive teardown boundary for C-ABI owners: detaches callbacks, persists open
		//! gestures, and closes mutation admission. Returns false on persistence failure or synchronous
		//! re-entry from a Last Render / dirty callback. Owner must stop external callers first.
		bool PrepareForDestruction();

		//! Single-owner terminal delete claim for the C destroy shim (returns false if already claimed).
		bool PrepareForDestructionForDelete();

		//! True once ~SceneEditController has started.
		bool IsRawDestructionInProgress() const
		{
			return mInDestructorTeardown.load( std::memory_order_acquire );
		}

		//! True while this thread is inside a Last Render sink or dirty-changed callback/finalizer
		//! (synchronous Destroy is rejected in that state).
		bool IsInLastRenderSinkCallbackOnThisThread() const;
		bool IsInDirtyChangedCallbackOnThisThread() const;

		// Refinement pause & status -----------------------------------

		//! Pause interactive refinement via StopInteractive() (edits still mutate; idempotent).
		void PauseRefinement();

		//! Resume after PauseRefinement() (no-op if not paused; any Start() also clears paused).
		void ResumeRefinement();

		bool IsRefinementPaused() const;

		//! Interactive refinement phase (best-effort poll for status chrome).
		enum class RefinementPhase : int
		{
			Idle      = 0,   //!< converged: full res, nothing in flight
			Rendering = 1,   //!< full-resolution pass in flight
			Refining  = 2,   //!< walking the resolution ladder (divisor > 1)
			Polishing = 3,   //!< denoised polish pass queued / in flight
			Paused    = 4,   //!< PauseRefinement() is in effect
		};

		//! @param outScaleDivisor  Receives the current preview-scale
		//!        divisor (1..32, powers of two; 1 = full resolution).
		RefinementPhase GetRefinementStatus( unsigned int& outScaleDivisor ) const;

		// Interactive region-of-interest (docs/GUI_ROADMAP.md A4) -----
		// Inclusive full-res film coords restricting full-res interactive passes only
		// (coarse ladder passes render full-frame; production renders ignore it unless explicit).
		// Degenerate boxes are refused; out-of-film boxes render full-frame. Thread-safe.

		void SetInteractiveRegion( unsigned int left, unsigned int top,
		                           unsigned int right, unsigned int bottom );
		void ClearInteractiveRegion();

		//! TRUE (+ coords) when a region is active.
		bool GetInteractiveRegion( unsigned int& left, unsigned int& top,
		                           unsigned int& right, unsigned int& bottom ) const;

		//! True if interactive rasterizer honors render regions (IRasterizer::HonorsRegion).
		bool InteractiveRasterizerHonorsRegion() const;

		//! Full terminal teardown (idempotent; rejected synchronously from Last Render sink callbacks).
		//! LOAD-BEARING ORDER: retires and joins the agent-render worker FIRST (setting mAgentRenderStop
		//! before any slot can free), then calls StopInteractive(). Use StopInteractive() to pause only.
		void Stop();

		bool IsRunning() const;

		// Sinks (install once before Start()) -------------------------

		void SetPreviewSink( IRasterizerOutput* sink );
		void SetProgressSink( IProgressCallback* sink );
		void SetLogSink( ILogPrinter* sink );

		// Tool state machine -----------------------------------------

		void SetTool( Tool t );
		Tool CurrentTool() const;

		// Pointer events (UI thread; preview-surface pixel coords) ----

		void OnPointerDown( const Point2& px );
		void OnPointerMove( const Point2& px );
		void OnPointerUp( const Point2& px );

		// Direct controls (UI thread) --------------------------------

		//! Bracket time-scrub into one undo entry (false if render admission blocked mutation).
		bool OnTimeScrubBegin();
		bool OnTimeScrub( Scalar t );
		bool OnTimeScrubEnd();

		//! Bracket property-panel chevron scrub so adaptive preview scaling activates during rapid edits.
		void BeginPropertyScrub();
		void EndPropertyScrub();

		//! Apply Undo/Redo (false if empty/refused/blocked; refused batch sets ConsumeHistoryNotice()).
		bool Undo();
		bool Redo();

		//! Return and clear any notice from the last refused Undo/Redo ("" if none).
		std::string ConsumeHistoryNotice();

		//! Labels for next Undo/Redo step (empty if stack empty; mMutex-serialized).
		String UndoLabel() const;
		String RedoLabel() const;

		//! Authoritative scene time tracked by SceneEditor (updated by scrub/Undo/Redo).
		Scalar LastSceneTime() const;

		//! Stop interactive thread, run production rasterizer synchronously, and restart interactive.
		bool RequestProductionRender();

		// Transactional rollback (inverse-edit based, not snapshot) ---
		// Reverts recorded EditHistory ops back to the BeginTransaction depth on the live
		// instances and clears redo. Independent of SceneEditor composites; non-history edits
		// (film, rasterizer params, animation frame count) are NOT reverted.

		//! Open a rollbackable transaction (replaces any open baseline; no nesting).
		//! Refuses (returns false) while a SceneEditor composite is open.
		bool BeginTransaction();

		//! True iff a rollbackable transaction is currently open.
		bool IsTransactionOpen() const;

		//! Undo recorded edits back to baseline under cancel-and-park mMutex, clear redo,
		//! neutralize any open composite, and re-render. Returns false if none open or partial.
		bool RollbackTransaction();

		//! Close transaction keeping live edits and redo stack intact (false if none open).
		bool EndTransaction();

		//! Complete transactional editor-state baseline (docs/gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md).
		struct EditorStateSnapshot {
			unsigned long long          historyMarker;     //!< EditHistory::NextSeq() at capture
			SceneEditor::DirtySnapshot  dirty;             //!< ALL dirty sources (tracker + scale-from-anchor)
			Category                    selectionCategory;
			String                      selectionName;
			std::vector<String>         selectionByCategory;   //!< per-category selection memory
			std::vector<bool>           sectionExpanded;       //!< per-category panel-section expand state
		};
		EditorStateSnapshot CaptureEditorState() const;
		//! Pass `restoreDirty=false` on partial rollback to retain residual dirty state and history.
		void                RestoreEditorState( const EditorStateSnapshot& s, bool restoreDirty = true );

		// Selection accessors ----------------------------------------
		// Camera/Rasterizer selection activates the entity under cancel-and-park; others are UI-only.

		Category GetSelectionCategory() const;
		String   GetSelectionName() const;

		//! Per-category selection name (empty if none picked in `cat`).
		String GetSelectionNameForCategory( Category cat ) const;

		//! True if `cat`'s accordion section is expanded (tracked independently of selection).
		bool IsSectionExpanded( Category cat ) const;

		//! Collapse `cat`'s section, clearing its selection and falling back primary if needed.
		void CollapseSection( Category cat );

		//! Set `(cat, entityName)` selection (empty name expands without picking; false on unknown camera/rasterizer).
		bool SetSelection( Category cat, const String& entityName );

		//! Legacy helper: returns selection name iff primary category is Object.
		String SelectedObjectName() const;

		// Accordion entity lists (polled on SceneEpoch() changes) -----

		unsigned int CategoryEntityCount( Category cat ) const;
		String       CategoryEntityName( Category cat, unsigned int idx ) const;

		//! Scene-level active entity name (Camera/Rasterizer/Film="default"; empty for others).
		String       CategoryActiveName( Category cat ) const;

		// Authored-graph node tree ------------------------------------
		// Generic per-category tree surface (flat categories = N childless roots; Object folds
		// synthesized instancing entries into their authoring chunk via GetObjectProvenance).
		// Prefer `ReadTree` for transactional multi-node walks. Per-node getters use
		// generation-tagged handles valid only within the current published snapshot (never
		// hold handles across turns; key UI state on name/path). TreeNodeCount/TreeRootCount
		// refresh and republish only on structural/serial/rebuildCount change; shells re-read
		// on SceneEpoch() (any future reparent path MUST bump mSceneEpoch).

		//! Opaque 64-bit generation-tagged node handle (high 32 = generation, low 32 = index).
		typedef unsigned long long TreeNodeHandle;

		//! Invalid/stale handle sentinel (also root parent).
		static constexpr TreeNodeHandle kInvalidTreeNode = 0xFFFFFFFFFFFFFFFFull;

		//! Raw node-index sentinel inside AuthoredTree (distinct from TreeNodeHandle space).
		static constexpr unsigned int kInvalidNodeIndex = 0xFFFFFFFFu;

		//! Total nodes in `cat`'s tree.  REFRESHES the snapshot.
		unsigned int TreeNodeCount( Category cat ) const;
		//! Number of ROOT nodes in `cat`'s tree.  REFRESHES the snapshot.
		unsigned int TreeRootCount( Category cat ) const;
		//! Published tree generation (does not refresh; 0 = never refreshed; process-global counter).
		unsigned long long TreeGeneration( Category cat ) const;

		// Note: incremental edits re-register non-Object entities (bumping serial & generation),
		// whereas Objects re-point in place. Use ReadTree rather than parking handles across edits.

		//! Handle of the `rootIdx`-th root, or kInvalidTreeNode.
		TreeNodeHandle TreeRootNode( Category cat, unsigned int rootIdx ) const;
		//! Child count of `node` (0 for unknown/stale handle).
		unsigned int TreeChildCountByHandle( Category cat, TreeNodeHandle node ) const;
		//! Handle of `node`'s `childIdx`-th child, or kInvalidTreeNode.
		TreeNodeHandle TreeChildNode( Category cat, TreeNodeHandle node, unsigned int childIdx ) const;
		//! Parent handle of `node` (kInvalidTreeNode for root/unknown/stale handle).
		TreeNodeHandle TreeNodeParent( Category cat, TreeNodeHandle node ) const;
		//! Entity name of `node` (empty for unknown/stale handle).
		String       TreeNodeNameByHandle( Category cat, TreeNodeHandle node ) const;

		//! One node in a published AuthoredTree (`parent` and `childIndices` index into `nodes`).
		struct TreeNodeRow
		{
			String       name;         //!< entity name (the selection identity)
			unsigned int parent;       //!< kInvalidNodeIndex for a root
			unsigned int firstChild;   //!< offset into AuthoredTree::childIndices
			unsigned int childCount;   //!< length of that slice
			//! `IManager::GetItemSerial` for same-name replacement detection (0 if store has no serial).
			//! Comparable per-row only within one store instance (see AuthoredTree::rebuildCount).
			unsigned long long serial = 0;
		};

		//! Published tree snapshot. MUST be read and assigned as one struct under `mUiSnapshotMutex`
		//! so `nodes`, `childIndices`, `roots`, and `generation` stay mutually consistent.
		struct AuthoredTree
		{
			std::vector<TreeNodeRow>  nodes;
			std::vector<unsigned int> childIndices;  //!< flattened child lists
			std::vector<unsigned int> roots;         //!< indices into `nodes`
			//! Published generation (0 = unpublished); handles carry this tag.
			unsigned long long        generation = 0;
			//! `IJobPriv::GetContainerRebuildCount` (bumped on InitializeContainers / SetPrimaryAcceleration);
			//! detects full store rebuilds where per-manager serials restart from 0.
			unsigned long long        rebuildCount = 0;
		};

		//! Transactional bulk tree read: refreshes once and copies published tree under `mUiSnapshotMutex`.
		//! Returned indices are raw indices into `out.nodes`; use `HandleFor` to mint handles.
		void ReadTree( Category cat, AuthoredTree& out ) const;

		//! Mint a generation-tagged handle for `t.nodes[index]` (pure/static; returns kInvalidTreeNode
		//! if out-of-range or unpublished `t.generation == 0`). Bound to the originating controller.
		static TreeNodeHandle HandleFor( const AuthoredTree& t, unsigned int index );

		//! Presentation-only fold from a live viewport-picked name (e.g. instanced `I[1,0]`) to
		//! its published outliner row name via `IObjectManager::GetObjectProvenance` (never name
		//! parsing; cycle-proof). Does not alter selection or refresh the tree (except cold gen==0);
		//! returns `entityName` unchanged if unresolved or contended.
		String ResolveTreeRowName( Category cat, const String& entityName ) const;

		//! `ResolveTreeRowName( GetSelectionCategory(), GetSelectionName() )` for outliner highlight.
		String SelectionRowName() const;

		//! Pure input seed for BuildAuthoredTree.
		struct TreeNodeSeed
		{
			String             name;
			String             parent;      //!< "" -- or a name that is not itself a seed -- means ROOT
			unsigned long long order  = 0;  //!< sibling display order key
			//! Registration serial (`IManager::GetItemSerial`), or 0 for non-IManager categories
			//! (same-store replacement undetectable there; full rebuilds still caught by rebuildCount).
			unsigned long long serial = 0;
		};

		//! Pure iterative tree assembler: preserves every seed, promotes dangling parents to roots,
		//! breaks cycles, sorts deterministically by `(order, name)`, leaves `generation == 0`.
		static AuthoredTree BuildAuthoredTree( const std::vector<TreeNodeSeed>& seeds );

		// =====================================================================
		// Painter/Material DAG (docs/gui/NODE_GRAPH_CANVAS.md §1.1/§1.2/§6)
		// Document-seeded from SceneReferenceGraph (Painter, Material, plus
		// edge-reached Function chunks; `def` stages stay internal as defCount).
		// Node identity is `Cst::NodeId` (so same-name colour/scalar painters
		// stay distinct). Same compare-then-publish discipline as AuthoredTree.
		// =====================================================================

		//! Pure DAG node seed; nonzero `id` (`Cst::NodeId`) is unique node identity.
		struct GraphNodeSeed
		{
			Cst::NodeId   id = 0;
			String        name;
			String        chunkKeyword;
			ParameterPipe outputPipe = ParameterPipe::Other;
			ChunkCategory category   = ChunkCategory::Painter;
			unsigned long long order  = 0;   //!< sibling/display order key, same role as TreeNodeSeed::order
			unsigned long long serial = 0;   //!< registration identity, same role as TreeNodeSeed::serial
			int           defCount   = 0;
			//! Object Graph `count_u * count_v` repeat count (0 = non-repeat / Painter-Material).
			int           repeatCount = 0;
		};

		//! Pure DAG edge seed matched by `fromId`/`toId` (`toId == 0` => dangling/out-of-scope port).
		struct GraphEdgeSeed
		{
			Cst::NodeId   fromId = 0;
			ChunkCategory fromCategory = ChunkCategory::Painter;
			String        fromName;
			String        paramName;
			int           occurrence   = 0;
			std::vector<ChunkCategory> portCategories;
			ParameterPipe portPipe = ParameterPipe::Other;
			bool portColorAlsoAcceptsScalar = false;
			Cst::NodeId   toId = 0;
			ChunkCategory toCategory   = ChunkCategory::Painter;
			String        toName;        //!< may name no seed at all -- see BuildPainterMaterialGraph
		};

		//! Node port entry (shared by `outEdges` and `inEdges`).
		struct GraphPort
		{
			String paramName;                          //!< the reference-bearing param's role
			int    occurrence      = 0;                 //!< 0-based occurrence among same-role siblings
			//! Target/source index into `nodes`, or `kInvalidNodeIndex` for dangling/out-of-scope ports.
			unsigned int otherNode = kInvalidNodeIndex;
			String otherName;                            //!< the other end's name, even when otherNode is invalid
			//! Declared accepted target categories (ParameterDescriptor::referenceCategories).
			std::vector<ChunkCategory> portCategories;
			ParameterPipe portPipe = ParameterPipe::Other;
			bool portColorAlsoAcceptsScalar = false;
		};

		//! Opaque 64-bit generation-tagged graph node handle (`(generation << 32) | index`).
		//! Prevents aliasing across full re-derives where raw `Cst::NodeId` values reset.
		typedef unsigned long long GraphNodeHandle;
		//! Invalid/stale graph handle sentinel (separate space from kInvalidTreeNode).
		static constexpr GraphNodeHandle kInvalidGraphNode = 0xFFFFFFFFFFFFFFFFull;

		//! Graph node; shared nodes appear once with multiple `inEdges` (links, not copies).
		struct GraphNode
		{
			//! Stamped at publish time from `PainterMaterialGraph::generation` (excluded from equality).
			GraphNodeHandle handle = kInvalidGraphNode;
			String        name;
			String        chunkKeyword;                  //!< e.g. "ramp_painter", "ggx_material"
			ParameterPipe outputPipe = ParameterPipe::Other;
			ChunkCategory category    = ChunkCategory::Painter;
			unsigned long long order  = 0;                //!< presentation-order key, same role as TreeNodeSeed::order
			unsigned long long serial = 0;                //!< registration identity, same role as TreeNodeSeed::serial
			//! `ExpressionProgram::DefCount()` for expression painters (0 otherwise).
			int defCount = 0;
			//! Object Graph `count_u * count_v` when `count_u` is authored (0 = non-repeat; >= 1 = repeated).
			int repeatCount = 0;
			std::vector<GraphPort> outEdges;              //!< this node's OWN reference params -> other nodes (or dangling)
			std::vector<GraphPort> inEdges;                //!< other nodes' reference params -> this node (its "parents")
		};

		//! Published DAG snapshot (must be read/assigned as one struct under `mUiSnapshotMutex`).
		struct PainterMaterialGraph
		{
			//! Per-read freshness; false when contention/render ownership served the cached graph.
			//! Not graph content, so excluded from snapshot equivalence.
			bool snapshotFresh = true;
			std::vector<GraphNode> nodes;
			//! Published generation from global counter (0 = unpublished); stamps `GraphNode::handle`.
			unsigned long long generation   = 0;
			//! Store rebuild count (`IJobPriv::GetContainerRebuildCount`).
			unsigned long long rebuildCount = 0;
		};

		//! Neutral alias for the shared graph model used by the Object Graph.
		using SceneGraphModel = PainterMaterialGraph;

		//! Pure DAG assembler keyed by `GraphNodeSeed::id` (duplicate ids keep first in `(order, name)`
		//! order; edges with unknown `fromId` are dropped; unknown/0 `toId` becomes a node-less port;
		//! cycles and self-references are represented directly without walking).
		static PainterMaterialGraph BuildPainterMaterialGraph(
			const std::vector<GraphNodeSeed>& nodeSeeds,
			const std::vector<GraphEdgeSeed>& edgeSeeds );

		//! Pure dangling-port seeding from cached node identities (excludes expression records).
		static std::vector<GraphEdgeSeed> BuildDanglingGraphEdgeSeeds(
			const std::vector<GraphNodeSeed>& nodes,
			const std::vector<Cst::UnresolvedReference>& unresolved,
			size_t* sourceLookups = nullptr );

		//! Transactional bulk graph read: refreshes once and copies under `mUiSnapshotMutex`.
		void ReadPainterMaterialGraph( PainterMaterialGraph& out ) const;

		//! Pure handle resolver: returns index in `g.nodes`, or `kInvalidNodeIndex` if invalid/stale/out-of-range.
		static unsigned int ResolveGraphNodeHandle( const PainterMaterialGraph& g, GraphNodeHandle handle );

		//! Pure unique `(category, name)` lookup in `g` (-1 if absent or ambiguous; `*outMatches` gives count).
		static int ResolveUniqueGraphNodeIndex( const PainterMaterialGraph& g, ChunkCategory category,
		                                         const String& name, int* outMatches = nullptr );

		// Connection legality passthrough (ConnectionLegality.h, name-addressed)

		//! `ConnectionLegality::CheckConnectionByName` against the controller's current Document.
		ConnectionVerdict CheckConnection(
			ChunkCategory targetCategory, const String& targetName,
			const String& paramName,
			ChunkCategory candidateCategory, const String& candidateName ) const;

		//! `ConnectionLegality::WouldCycle` by `(category, name)` (false on unresolved/ambiguous; check legality first).
		bool WouldCycle(
			ChunkCategory fromCategory, const String& fromName,
			ChunkCategory toCategory, const String& toName ) const;

		//! Pure fixpoint BFS promoting Function chunks reachable from Painter/Material referrers
		//! (updates `promotedFunctionIds` and `functionFrontier` in place; cycle-safe via visited set).
		static void ExpandFunctionPromotionFrontier(
			const std::multimap<Cst::NodeId, const ReferenceEdge*>& edgesByReferrer,
			std::set<Cst::NodeId>& promotedFunctionIds,
			std::vector<std::pair<Cst::NodeId, String> >& functionFrontier );

		// =====================================================================
		// Laid-out graph snapshot, sidecar write, and C-ABI flat accessors
		// (GraphLayout.h + GraphLayoutSidecar.h; docs/gui/NODE_GRAPH_CANVAS.md §6).
		// Scene path comes from `mJob.GetCstLoadFileIdentity().filePath` (empty if unsaved).
		// =====================================================================

		//! Laid-out 2D position parallel to `graph.nodes` (local type avoids circular include with GraphLayout.h).
		struct GraphNodePosition
		{
			double x = 0.0;
			double y = 0.0;
		};

		//! Composed graph + per-node positions (edges live on node `outEdges`/`inEdges`).
		struct PainterMaterialGraphLaidOut
		{
			PainterMaterialGraph graph;
			//! Parallel to graph.nodes; see GraphNodePosition's own comment.
			std::vector<GraphNodePosition> positions;
		};

		//! Read graph, load sidecar positions fresh from disk, and auto-layout missing nodes via GraphLayout.
		void ReadPainterMaterialGraphLaidOut( PainterMaterialGraphLaidOut& out ) const;

		//! Name-keyed position update for WriteGraphLayoutPositions (names survive save/reload).
		struct GraphNodePositionUpdate
		{
			String name;
			double x = 0.0;
			double y = 0.0;
		};

		//! Merge `updates` into sidecar and atomically write via `GraphLayoutSidecar::WriteSidecar`.
		//! Acquires blocking `mMutex` to read scene path + live node names consistently (refuses with
		//! error while a render owns the scene; no-ops true for unsaved scenes or unchanged bytes).
		bool WriteGraphLayoutPositions( const std::vector<GraphNodePositionUpdate>& updates,
		                                std::string& outError ) const;

		// Flat C-ABI graph accessors: `PainterGraphNodeCount()` refreshes; all other accessors
		// read the published snapshot under `mUiSnapshotMutex` (prefer bulk `ReadPainterMaterialGraph`
		// in C++ bridges). Port `otherNode` resolves to `GraphNodeHandle` (`kInvalidGraphNode` if node-less).

		//! Node count (refreshes snapshot).
		unsigned int PainterGraphNodeCount() const;

		//! Currently published generation (does not refresh; 0 if unpublished).
		unsigned long long PainterGraphGeneration() const;

		//! Handle of `idx`-th node (`kInvalidGraphNode` if out of range).
		GraphNodeHandle PainterGraphNodeHandleAt( unsigned int idx ) const;

		//! Node name / keyword (empty on invalid/stale handle).
		String PainterGraphNodeName( GraphNodeHandle node ) const;
		String PainterGraphNodeKeyword( GraphNodeHandle node ) const;

		//! `ChunkCategory` cast to int (-1 on invalid/stale handle; append-only ABI).
		int PainterGraphNodeCategory( GraphNodeHandle node ) const;
		int PainterGraphNodeOutputPipe( GraphNodeHandle node ) const;
		bool PainterGraphNodePortPipe( GraphNodeHandle node, unsigned int portIdx,
			bool incoming, int& outPipe, bool& outColorAlsoAcceptsScalar ) const;

		//! Expression `def` count (0 for non-expression; -1 on invalid/stale handle).
		int PainterGraphNodeDefCount( GraphNodeHandle node ) const;

		//! Single-node laid-out position (computes sidecar+layout per call; use bulk read for walks).
		bool PainterGraphNodePosition( GraphNodeHandle node, double& outX, double& outY ) const;

		//! Port counts (0 on invalid/stale handle).
		unsigned int PainterGraphNodeOutEdgeCount( GraphNodeHandle node ) const;
		unsigned int PainterGraphNodeInEdgeCount( GraphNodeHandle node ) const;

		//! Port at `portIdx` (`outOtherNode == kInvalidGraphNode` for dangling/out-of-scope; false on bad handle/idx).
		bool PainterGraphNodeOutEdge( GraphNodeHandle node, unsigned int portIdx,
		                              GraphNodeHandle& outOtherNode, String& outParamName,
		                              int& outOccurrence, String& outOtherName ) const;
		bool PainterGraphNodeInEdge( GraphNodeHandle node, unsigned int portIdx,
		                             GraphNodeHandle& outOtherNode, String& outParamName,
		                             int& outOccurrence, String& outOtherName ) const;

		// Node-graph spotlight & focused-view queries -----------------

		//! `(category, name)` node identity returned by `AppearanceClosureForObject`.
		struct AppearanceClosureEntry
		{
			ChunkCategory category = ChunkCategory::Painter;
			String        name;
		};

		//! Material + transitive Painter/Function/Material BFS closure for live Object `objectName`
		//! (instancing-correct via live bound IMaterial). Empty if unknown, unbound, ambiguous in
		//! graph, or contended. `*outDegraded` (when non-null) is true ONLY on lock/render contention.
		std::vector<AppearanceClosureEntry> AppearanceClosureForObject(
			const String& objectName, bool* outDegraded = nullptr ) const;

		//! Focused subgraph rooted at `(cat, name)` (Object -> appearance closure; Painter/Function/
		//! Material -> upstream `outEdges` closure), laid out fresh with transient auto-layout (never
		//! reads/writes `.risegraph.json`). `*outDegraded` is true on render ownership or lock contention.
		void ReadPainterMaterialGraphLaidOutFocused( ChunkCategory cat, const String& name,
		                                              PainterMaterialGraphLaidOut& out,
		                                              bool* outDegraded = nullptr ) const;

		// =====================================================================
		// Object Graph (`SceneGraphModel`, same compare-then-publish discipline)
		// Nodes: Object (excluding `override_object`), Geometry (incl. `gltf_import`),
		// and `rect_light`/`shape_light` (re-categorized as Object).
		// Edges: hierarchy (`parent`, `source`, `obja`, `objb`) unflipped (child->parent);
		// geometry (`geometry`, `base_geometry`) flipped (geometry->consumer) so L->R
		// GraphLayout places roots at rank 0, children right, shared geometry rightmost.
		// =====================================================================
		void ReadObjectGraph( SceneGraphModel& out ) const;

		//! Laid-out Object Graph snapshot (`positions` parallel to `graph.nodes`).
		struct ObjectGraphLaidOut
		{
			SceneGraphModel graph;
			//! Parallel to graph.nodes; see GraphNodePosition's own comment.
			std::vector<GraphNodePosition> positions;
		};

		//! `ReadObjectGraph` + fresh transient `GraphLayout::LayoutGraph` (no sidecar; `.risegraph.json`
		//! is bare-name-keyed for Painter/Material and not partitioned by graph kind).
		void ReadObjectGraphLaidOut( ObjectGraphLaidOut& out ) const;

		//! Focused Object Graph for Object `name` (transient layout): union of separate UP (ancestors +
		//! owning CSG composites) and DOWN (descendants + CSG operands) walks (no sibling leaks), full
		//! GEO chain (`geometry`/`base_geometry`), and 1-hop `source` target. `*outDegraded` on contention.
		void ReadObjectGraphLaidOutFocused( const String& name, ObjectGraphLaidOut& out,
		                                     bool* outDegraded = nullptr ) const;

		//! Monotonic scene-structure epoch (global seed at construction; bumped on structural
		//! mutations, camera clones, chunk CRUD, rollbacks, selection changes) for shell list caching.
		unsigned int SceneEpoch() const;

		//! Stable full-resolution camera dimensions (do NOT read ICamera::GetWidth/Height directly,
		//! as preview scaling swaps film dims per pass). False if no camera attached.
		bool GetCameraDimensions( unsigned int& w, unsigned int& h ) const;

		//! Scene animation range `(timeStart, timeEnd, numFrames)` (default 0, 1, 30).
		//! Non-blocking: returns false (outputs untouched) while render owns scene or mMutex contended.
		bool GetAnimationOptions( double& timeStart, double& timeEnd,
		                          unsigned int& numFrames ) const;

		//! True if scene has keyframes or simulation time-varying assets (non-blocking; false if busy).
		bool GetHasAnimation( bool& hasAnimation ) const;

		// Simulation playback, live runs, and baking (docs/SIMULATION_SOLVER_CONTRACT.md)

		//! Play scene time on a controller thread (opens one scrub composite; tracks live solvers).
		//! `fps <= 0` uses real-time timeline grid for simulation scenes, else `kFrameSteppingPlaybackFps`.
		bool PlaybackPlay( double fps, bool loop );
		//! `timeScale`: scene seconds per wall second (PlaybackClock::Play).
		bool PlaybackPlayRange( double start, double end, double fps, bool loop, double timeScale = 1.0 );
		static constexpr double kFrameSteppingPlaybackFps = 30.0;
		bool PlaybackPause();				//!< also pauses live solvers
		//! Move the playhead: while playing, the clock jumps there; while
		//! paused, one scrub (clamped to the latest live frame).
		bool PlaybackSeek( double t );
		bool GetPlaybackStatus( Implementation::PlaybackStatus& status ) const;
		//! Every time-varying asset's frames and how much is baked (reads a
		//! partial cache's manifest).  Same non-blocking contract as
		//! GetAnimationOptions.
		bool GetTimeVaryingAssets( std::vector<Simulation::TimelineRange>& ranges ) const;

		//! Attach an interactive solver to `simulated_mesh_geometry` `geometry` (topology must match).
		bool LiveSimulationAttach( const std::string& geometry, const std::string& solverId,
			const std::vector<unsigned char>& parameters, std::string& error );
		//! Stop the solver (joined) and rebind the geometry's baked cache.
		bool LiveSimulationDetach( const std::string& geometry, std::string& error );
		//! Restart from the initial state (published frames are dropped) and
		//! show its first frame.
		bool LiveSimulationReset( const std::string& geometry, std::string& error );
		//! New parameters: a new cache key; restarts.
		bool LiveSimulationSetParameters( const std::string& geometry,
			const std::vector<unsigned char>& parameters, std::string& error );
		//! Restart, also recording every frame to a new epoch of the disk
		//! cache at `cacheDirectory` (sealed when the run finishes).
		bool LiveSimulationRecord( const std::string& geometry, const std::string& cacheDirectory,
			std::string& error );
		//! False when no live simulation is attached to `geometry`, or while
		//! a scrub holds the live state (non-blocking; poll again).
		bool GetLiveSimulationStatus( const std::string& geometry,
			Implementation::LiveSimulationStatus& status ) const;
		unsigned int LiveSimulationCount() const;
		//! Run declared scene simulation `simulation` live (inputs captured detached under render park).
		bool LiveSimulationAttachSimulation( const std::string& simulation, std::string& error );
		//! Restart live scene simulations whose configuration or consumer changed after an edit.
		bool LiveSimulationResync( std::string& error );
		//! Describe all declared simulations (non-blocking; false while scene busy).
		bool GetSimulations( std::vector<Simulation::SceneSimulationStatus>& statuses, std::string& error ) const;

		//! Snapshot scene file identity, unsaved status, and simulations for SimulationBakeManager.
		//! `blocking=false` returns false if busy; `checkUnsaved=false` skips full-file byte compare.
		bool GetSimulationBakeSnapshot( Simulation::BakeSceneSnapshot& snapshot, bool blocking,
			std::string& error, bool checkUnsaved = true ) const;
		//! Rebind `source`-backed simulated meshes and re-derive rigid-body transform keyframes when
		//! bake manager `FramesGeneration` advances (poll on `RISE_API_SimulationBake_ChangeCounter`).
		bool RefreshSimulationFrames( bool& refreshed, std::string& error );
		//! Current row-major 4x4 world transform of `object` (false if missing or scene busy).
		bool GetObjectWorldTransform( const std::string& object, double matrix[16], std::string& error ) const;
		//! Detach the live run of scene simulation `simulation`.
		bool LiveSimulationDetachSimulation( const std::string& simulation, std::string& error );
		//! Bake-manager scene key for current scene file (non-blocking; `attachNeeded` set on first key).
		std::string SimulationBakeSceneKey( bool* attachNeeded = nullptr ) const;
		//! Interactive passes finished without cancellation (the clock's
		//! coalescing signal).
		std::uint64_t GetCompletedPassCount() const;

		//! Headless RGBA8 preview (`w*h*4`) of painter output (`defIndex < 0`) or expression `def` stage
		//! (see PainterPreview.h). Non-blocking try_lock; returns false on missing/invalid/busy.
		bool GetPainterPreview( const String& painterName, int defIndex,
		                        unsigned int w, unsigned int h,
		                        std::vector<unsigned char>& outRGBA,
		                        bool* outWasScalar = nullptr,
		                        double* outRangeMin = nullptr,
		                        double* outRangeMax = nullptr ) const;

		//! Horizontal gradient strip preview for a `ramp_painter` (same non-blocking contract).
		bool GetRampStripPreview( const String& painterName,
		                          unsigned int w, unsigned int h,
		                          std::vector<unsigned char>& outRGBA ) const;

		//! Headless 3D shaderball RGBA8 preview of a material (docs/gui/MATERIAL_EDITOR.md §4; non-blocking).
		bool GetMaterialPreview( const String& materialName,
		                         unsigned int w, unsigned int h,
		                         std::vector<unsigned char>& outRGBA ) const;

		//! Thin-film 0..89 deg reflectance strip and single-angle eval for `ggx_material` (SPECTRAL_DIFFERENTIATORS.md §5).
		bool GetThinFilmAngleStrip( const String& materialName,
		                            unsigned int w, unsigned int h,
		                            std::vector<unsigned char>& outRGBA,
		                            double* outThicknessNm = nullptr,
		                            double* outFilmIor = nullptr ) const;

		bool EvaluateThinFilmAtAngle( const String& materialName,
		                              double angleDeg,
		                              unsigned char outRGBA[4],
		                              double* outThicknessNm = nullptr,
		                              double* outFilmIor = nullptr ) const;

		//! Thread-safe coherent copy of live `mInteractiveFrameStore` pixels without triggering a render
		//! (tile shared_locks via DumpImage; `*outSourcePane` captured atomically under frame-store mutex).
		bool CopyInteractiveFrame( std::vector<RISEColor>& outPixels,
		                           unsigned int& outWidth,
		                           unsigned int& outHeight,
		                           unsigned int* outSourcePane = nullptr ) const;

		// Test hooks --------------------------------------------------

		//! Test hook: set `(cat, name)` selection directly without pointer events.
		void ForTest_SetSelection( Category cat, const String& name );

		//! Increments each time RequestCancel actually trips an
		//! in-flight render.  Reads can race with the render thread;
		//! callers should join via Stop() before sampling.
		unsigned int ForTest_GetCancelCount() const;

		//! Test hook: count of CancelAgentRender_() invocations (distinct from UI mCancelCount).
		unsigned int ForTest_GetAgentCancelRequestCount() const;

		//! Increments at the start of each render-loop iteration
		//! that actually fires a render pass.
		unsigned int ForTest_GetRenderCount() const;

		//! Test hook: block until at least `count` render passes complete (false on timeout).
		bool ForTest_WaitForRenders( unsigned int count, unsigned int timeoutMs );

		//! Test hook: current `mRenderOwnsScene` state.
		bool ForTest_RenderOwnsScene() const { return mRenderOwnsScene.load( std::memory_order_acquire ); }

		//! Pane whose pixels `mInteractiveFrameStore` currently holds (for multi-pane read_viewport).
		unsigned int CurrentRenderPane() const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			return mCurrentPane;
		}

		//! Test hook: trigger KickRender().
		void ForTest_KickRender() { KickRender(); }

		//! Test hook: currently loaded scheduler pane (under mMutex).
		unsigned int ForTest_CurrentPane() const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			return mCurrentPane;
		}

		//! Test hook: true iff `pane` holds FinalRegularRunning (live or saved).
		bool ForTest_PaneHasFinalRegularPolish( unsigned int pane ) const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			if( pane >= kViewportPaneCount ) return false;
			const int state = ( pane == mCurrentPane )
				? mPolishState.load( std::memory_order_acquire )
				: mPaneRender[pane].polishSaved;
			return state == static_cast<int>( PolishState::FinalRegularRunning );
		}

		//! Test hook: override pointer-gesture watchdog timeout (ms).
		void ForTest_SetPointerWatchdogMs( int ms )
		{
			mPointerWatchdogMs.store( ms, std::memory_order_release );
		}
		bool ForTest_VariantAdmissionPendingForPane( unsigned int pane ) const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			return mVariantInteractionAdmissionPending
			    && mVariantInteractionAdmissionPane == pane;
		}

		//! Test hook: true if pointer watchdog marked the active gesture stale.
		bool ForTest_PointerGestureStale() const
		{
			return mPointerGestureStale.load( std::memory_order_acquire );
		}

		//! Test hook: raw mPointerDown flag (stays true while stale so late PointerUp finalizes).
		bool ForTest_PointerDown() const
		{
			return mPointerDown.load( std::memory_order_acquire );
		}

		bool ForTest_ScrubInProgress() const
		{
			return mScrubInProgress.load( std::memory_order_acquire );
		}

		bool ForTest_DestructionClaimed() const
		{
			return mDestructionState.load( std::memory_order_acquire )
			    != DestructionOpen;
		}

		//! Test hook: latch sticky `mInteractionPersistenceFailed` (irreversible, matching production).
		void ForTest_TripInteractionPersistenceFailure()
		{
			mInteractionPersistenceFailed.store( true, std::memory_order_release );
		}
		//! Test hook: force next FinalizeOpenInteractions() to return false transiently (one-shot).
		void ForTest_FailNextFinalizeOpenInteractions()
		{
			mForTestFinalizeFailOnce.store( true, std::memory_order_release );
		}
		//! Test hook: arm current pane's FinalRegularRunning polish state.
		void ForTest_ArmFinalRegularPolish()
		{
			std::lock_guard<std::mutex> lk( mMutex );
			mPolishState.store(
				static_cast<int>( PolishState::FinalRegularRunning ),
				std::memory_order_release );
		}

		const SceneEditor& Editor() const { return mEditor; }
		SceneEditor&       Editor()       { return mEditor; }

		//! Atomic whole-Document save via SaveEngine::Save (docs/gui/ADR_DOCUMENT_FIRST_EDITOR.md §9.9).
		//! Snapshots immutable Document under parked mMutex, sets mSaving, writes disk I/O lock-free,
		//! then clears dirty only if live head still matches the saved snapshot.
		SaveResult RequestSave( const std::string& filePath );

		//! True while disk I/O of RequestSave is in flight (blocks new interactive passes).
		bool IsSaving() const { return mSaving.load(); }

		//! Error message from last RequestSave (empty on Saved/NoOp; returned by value).
		std::string LastSaveError() const;

		//! O(1) conservative dirty flag driving the Save button (may be true when Save would NoOp).
		bool HasUnsavedChanges() const { return mEditor.HasUnsavedChanges(); }

		// Scene-file live sync & source traceability ------------------

		//! Current CST serialization (empty if none; takes mMutex -- do not poll during a render).
		String SerializedSceneText() const;

		//! Current retained CST head `(uuid, revision)` (both 0 if none; takes mMutex).
		void GetSceneTextVersion( std::uint64_t& outUuid,
		                          std::uint64_t& outRevision ) const;

		//! Atomic `{hasDocument, document, headVersion}` snapshot under blocking `mMutex`.
		//! AGENT RPC THREAD ONLY (blocks during async renders; never call from UI frame callbacks
		//! or render closures). Returns empty/{0,0} without touching mJob once teardown starts.
		void ReadAgentSceneSnapshot( bool& outHasDocument,
		                             std::string& outDocument,
		                             RISE::Cst::CstHeadVersion& outVersion ) const;

		//! Head-version-only twin of ReadAgentSceneSnapshot (same lock/thread/teardown contract).
		RISE::Cst::CstHeadVersion ReadAgentHeadVersion() const;

		//! Persistent-structure Document copy under ReadAgentSceneSnapshot's lock/lifecycle contract.
		void CopyAgentHeadDocument( bool& outHasDocument,
		                            RISE::Cst::Document& outDocument,
		                            RISE::Cst::CstHeadVersion& outVersion ) const;

		//! Apply one CstBatchEdit to caller's `doc` under mMutex (reads live Job state; mutates only `doc`).
		int ApplyAgentBatchElementToDocument( RISE::Cst::Document& doc, const CstBatchEdit& edit,
		                                      CstBatchOutcome& outcome ) const;

		//! Resolve `(cat, name)` to top-level chunk byte offset and 1-based line in SerializedSceneText().
		//! False for Rasterizer/Film/None or unresolvable/ambiguous names. Takes mMutex (O(doc bytes) line count).
		bool EntitySourceLocation( Category cat, const String& name,
		                          std::uint64_t& outByteOffset, std::uint32_t& outLine ) const;

		//! Resolve `(cat, name, param, occ)` to scene-file byte span + 1-based line/col (covers named
		//! categories, unnamed active Camera, and singleton Film/Rasterizer). Empty `param` -> whole chunk.
		struct SourceSpan
		{
			bool          present    = false;  //!< a scene-file span was resolved
			std::uint64_t byteOffset = 0;      //!< absolute byte offset in SerializeCst(doc)
			std::uint64_t byteLength = 0;      //!< param span width; 0 for a whole-chunk reveal
			std::uint32_t line       = 1;      //!< 1-based line of byteOffset
			std::uint32_t column     = 1;      //!< 1-based column of byteOffset
		};
		bool ResolveSourceSpan( Category cat, const String& name, const String& param,
		                        int occ, SourceSpan& out ) const;

		//! Reverse byte-offset -> `(outCat, outName, outParam, outOccurrence)` lookup (`outName` is chunk
		//! keyword for unnamed Rasterizer chunks). Returns false outside addressable chunks.
		bool SourceRefAtByteOffset( std::uint64_t offset, Category& outCat,
		                            String& outName, String& outParam,
		                            int* outOccurrence = nullptr ) const;

		//! Install coalesced dirty-transition listener (pass empty fn to detach).
		//! Fires on draining thread with NO controller/notification lock held. Synchronous re-entry
		//! is supported, but Destroy/Prepare/delete from inside the callback is forbidden.
		using DirtyChangedFn = SceneEditor::DirtyChangedFn;
		void SetDirtyChangedListener( DirtyChangedFn fn )
		{
			mEditor.SetDirtyChangedListener( std::move( fn ) );
		}

		//! True if cancellation was requested on `mCancelProgress`.
		//! Viewport sinks deliberately publish useful partial frames and do
		//! not use this state as a presentation filter.
		bool IsCancelRequested() const { return mCancelProgress.IsCancelRequested(); }

		// Properties panel (numeric values 0..7 match Category ABI) ---
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

		//! Panel header string (e.g. "Camera", "Object: <name>", or empty).
		String CurrentPanelHeader() const;

		// Primary-selection property snapshot accessors (rebuilt by RefreshProperties()).

		unsigned int PropertyCount() const;
		String PropertyName( unsigned int idx ) const;
		String PropertyValue( unsigned int idx ) const;
		String PropertyDescription( unsigned int idx ) const;
		int  PropertyKind( unsigned int idx ) const;       // ValueKind cast to int
		bool PropertyEditable( unsigned int idx ) const;

		//! Per-category property snapshot accessors for multi-section panels.
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
		bool         PropertyHasRangeFor( Category cat, unsigned int idx ) const;
		double       PropertyRangeMinFor( Category cat, unsigned int idx ) const;
		double       PropertyRangeMaxFor( Category cat, unsigned int idx ) const;
		double       PropertyRangeStepFor( Category cat, unsigned int idx ) const;
		bool         PropertyTargetsPainterFor( Category cat, unsigned int idx ) const;
		int          PropertyInlinePainterKindFor( Category cat, unsigned int idx ) const;
		String       PropertyInlinePainterValueFor( Category cat, unsigned int idx ) const;
		unsigned int PropertyInlinePainterRefCountFor( Category cat, unsigned int idx ) const;

		//! Quick-pick preset accessors (empty if descriptor declares none).
		unsigned int PropertyPresetCount( unsigned int idx ) const;
		String PropertyPresetLabel( unsigned int idx, unsigned int presetIdx ) const;
		String PropertyPresetValue( unsigned int idx, unsigned int presetIdx ) const;

		//! Display unit suffix (e.g. "mm", "°"; empty if dimensionless; presentation hint only).
		String PropertyUnitLabel( unsigned int idx ) const;

		//! Authored numeric slider range hint (currently expression painter `param[i]`; step 0 = continuous).
		//! Presentation hint only; `PropertyHasRange` is true iff both bounds are declared.
		bool   PropertyHasRange( unsigned int idx ) const;
		double PropertyRangeMin( unsigned int idx ) const;
		double PropertyRangeMax( unsigned int idx ) const;
		double PropertyRangeStep( unsigned int idx ) const;

		//! Inline constant-painter editing & swatch metadata for painter Reference rows.
		bool         PropertyTargetsPainter( unsigned int idx ) const;
		int          PropertyInlinePainterKind( unsigned int idx ) const;
		String       PropertyInlinePainterValue( unsigned int idx ) const;
		unsigned int PropertyInlinePainterRefCount( unsigned int idx ) const;

		//! Resolve a Reference row's jump-to-definition `(outCat, outName)` against live managers
		//! (first declared referenceCategory wins; false on non-Reference, empty, or dangling).
		bool PropertyJumpTarget( unsigned int idx, Category& outCat, String& outName ) const;
		bool PropertyJumpTargetFor( Category cat, unsigned int idx, Category& outCat, String& outName ) const;

		//! Rebuild property snapshots from live entities under non-blocking try_lock.
		void RefreshProperties();

		//! Apply property edit on primary selection and re-render. Returns false on parse/read-only
		//! error or if live Object edit succeeded but CST transform commit failed (still re-renders).
		bool SetProperty( const String& name, const String& valueStr );

		//! Apply property edit routed through `cat`'s per-category selection.
		bool SetPropertyForCategory( Category cat, const String& name, const String& valueStr );

		//! Edit `color`/`value` on the constant painter bound to `slotParam` via checked CST path.
		bool SetReferenceSlotPainterValue( Category cat, const String& slotParam, const String& valueStr );

		//! Fork painter bound to `slotParam` immediately after original and rebind `slotParam` (1 Undo step).
		bool ForkReferenceSlotPainter( Category cat, const String& slotParam, String* outNewPainterName = nullptr );

		//! Apply spectral preset (SPECTRAL_DIFFERENTIATORS.md §3/§4) as one atomic undo batch:
		//! ggx_material metals (Au/Ag/Cu/Al/Cr/Ti), dielectric/scalar Sellmeier glasses, blackbody K.
		bool ApplySpectralPreset( Category cat, const String& presetId );

		//! Clone active camera under canonicalized unique name, promote to active, persist as CST
		//! chunk (undoable), and bump SceneEpoch. False if no active/clonable camera or `outLen == 0`.
		bool CloneActiveCamera( const String& proposedName,
		                        char* outName, unsigned int outLen );

		//! Promote current transient free-fly ViewportPose into a new named scene camera (undoable).
		//! False if free-fly inactive, no camera manager, buffer too small, or edit refused.
		bool StampViewToNewCamera( const String& proposedName,
		                           char* outName, unsigned int outLen,
		                           unsigned int pane = kViewportNavPrimary );

		// Free-fly viewport pose (docs/gui/CAMERAS_AND_VIEWS.md §5.3-5.5)
		// Transient per-pane CameraSnapshot override for non-destructive navigation (no SceneEdit/
		// undo/dirty; production render unaffected). Only Stamp/Promote writes a scene camera.

		//! Sentinel targeting `mPrimaryPane` under lock (explicit `0` targets pane 0 directly).
		static constexpr unsigned int kViewportNavPrimary = 0xFFFFFFFFu;

		//! Seed transient pose from active camera and realize override (cancel-and-parks).
		bool EnterFreeFlyFromActiveCamera( unsigned int pane = kViewportNavPrimary );

		//! Realize explicit transient `pose` override (cancel-and-parks).
		bool SetViewportPose( const CameraSnapshot& pose, unsigned int pane = kViewportNavPrimary );

		//! Drop transient override and return to Scene::pActiveCamera (false if inactive; cancel-and-parks).
		bool ExitFreeFly( unsigned int pane = kViewportNavPrimary );

		//! Whether a transient viewport pose is currently overriding the
		//! interactive render camera.
		bool IsFreeFlyActive( unsigned int pane = kViewportNavPrimary ) const;

		//! Copy the current transient pose into `out`.  Returns false when
		//! free-fly isn't active (out untouched).
		bool GetViewportPose( CameraSnapshot& out, unsigned int pane = kViewportNavPrimary ) const;

		// Axis snaps & Home (non-destructive transient ViewportPose; docs/gui/CAMERAS_AND_VIEWS.md §4.2)

		//! Snap view down world axis (`axis`: 0=X/1=Y/2=Z, `negative`: -axis), preserving distance & optics.
		bool SnapViewToAxis( int axis, bool negative, unsigned int pane = kViewportNavPrimary );

		//! Capture current view into session Home slot (false if no capturable camera).
		bool SetHomeView( unsigned int pane = kViewportNavPrimary );

		//! Restore Home slot into transient pose (false if unset).
		bool GoToHomeView( unsigned int pane = kViewportNavPrimary );

		//! Whether a home view has been captured this session.
		bool HasHomeView() const;

		// Named Views (docs/gui/CAMERAS_AND_VIEWS.md §3): session bookmarks (non-destructive;
		// only PromoteNamedViewToCamera mutates scene). Guarded by `mNamedViewsMutex`.
		struct NamedView
		{
			String         name;
			CameraSnapshot pose;
		};

		//! Capture current view as a new named view (false if no camera or `name` > 255 bytes).
		bool CaptureNamedView( const String& name );

		unsigned int NamedViewCount() const;
		//! Copy view `idx`'s name into `out` (NUL-terminated).  False on a bad
		//! index or a too-small buffer.
		bool NamedViewName( unsigned int idx, char* out, unsigned int outLen ) const;

		//! Restore view `idx` into transient ViewportPose (non-destructive).
		bool RestoreNamedView( unsigned int idx );

		//! Re-capture the CURRENT view into slot `idx` (SketchUp "Update
		//! Scene").  False on a bad index or no capturable camera.
		bool UpdateNamedView( unsigned int idx );

		//! Remove view `idx`.  False on a bad index.
		bool DeleteNamedView( unsigned int idx );

		//! Promote view `idx` into a new active named scene camera (canonicalized, deduped, undoable).
		bool PromoteNamedViewToCamera( unsigned int idx, const String& proposedName,
		                               char* outName, unsigned int outLen );

		//! Thread-safe lookup of named view pose under `mNamedViewsMutex` (docs/gui/RENDER_MODES.md §8).
		bool FindNamedViewPose( const String& name, CameraSnapshot& outPose ) const;

		//! Registered Add-Entity template count for `cat` (see EntityTemplates.h).
		unsigned int EntityTemplateCount( Category cat ) const;

		//! Display label for the template at `idx` within `cat` (e.g.
		//! "Sphere", "Omni Light"), or empty for an out-of-range idx.
		String EntityTemplateLabel( Category cat, unsigned int idx ) const;

		//! Instantiate template `idx` in `cat` via sequential ApplyAgentInsertChunk calls (picks a
		//! doc-wide collision-free base + sub-chunk name set; bootstraps default material if needed).
		//! Multi-chunk templates undo as N steps and do not auto-rollback on mid-sequence failure.
		AgentCommitResult InstantiateEntityTemplate( Category cat, unsigned int idx, String* outName );

		//! Duplicate named entity chunk in `cat` with a fresh deduped name via ApplyAgentInsertChunk.
		AgentCommitResult DuplicateEntity( Category cat, const String& name, String* outName );

		// Node-graph chunk creation (docs/gui/NODE_GRAPH_CANVAS.md §6)
		// Derives minimal body via EntityTemplates::BuildNodeChunkText and commits through
		// ApplyAgentInsertChunk (inherits full D2 rebind, dirty, undo, epoch, and light rebuild).

		//! Creation argument (`param` declared on descriptor; `name` is refused).
		struct ChunkNodeArg
		{
			String param;
			String value;
		};

		//! Required caller-supplied argument for `keyword` (`referenceCategories` empty = unrestricted).
		struct ChunkNodeRequirement
		{
			String param;
			String description;
			bool   isReference = false;
			std::vector<ChunkCategory> referenceCategories;
		};

		//! Pure descriptor query returning required creation args for `keyword` (no locking).
		std::vector<ChunkNodeRequirement> ChunkNodeRequirements( const String& keyword ) const;

		//! Pure descriptor query: keywords for `category` sorted modern -> deprecated -> frozen.
		std::vector<String> PaletteKeywords( ChunkCategory category ) const;

		//! Pure descriptor query: deprecation replacement hint for `keyword` (empty if current).
		std::string PaletteKeywordDeprecation( const String& keyword ) const;

		//! Pure descriptor query: true if `keyword` is a frozen legacy chunk type.
		bool PaletteKeywordIsFrozen( const String& keyword ) const;

		//! Create one painter/material chunk of `keyword` with canonicalized doc-wide unique name
		//! (truncated to 255 bytes with suffix reserved; `*outName` filled whenever Document mutated).
		//! Rejected results leave head byte-identical; diagnosed results mutated + rebuilt managers.
		AgentCommitResult CreateChunkNode( const String& keyword,
		                                   const String& baseName,
		                                   const std::vector<ChunkNodeArg>& args,
		                                   String* outName );

		// Ownership-closure rewire (OwnershipClosure.h; docs/gui/MATERIAL_EDITOR.md §3.7a)

		//! Rewire commit outcome + closure diagnostics (populated on both clean and refusal outcomes).
		struct RewireResult
		{
			//! Underlying commit result (every refusal is `status == "rejected"`, head byte-identical).
			AgentCommitResult commit;

			//! Closure classification (`Clean` if past closure gate; check `commit.applied` for success).
			ClosureClassification closure = ClosureClassification::Clean;

			//! True if refused by ConnectionLegality (`commit.message` holds parser diagnostic).
			bool legalityRefused = false;
			//! True if Color->Scalar adapter insertion is legal and admitted by all gates.
			bool canInsertScalarAdapter = false;

			//! True when the refusal came from the forward-reachability cycle
			//! check (`ConnectionLegality::WouldCycle`).
			bool cycleRefused = false;

			//! Shared chunk(s), out-of-closure referrers, and owning roots (copied from OwnershipClosureResult).
			std::vector<String> sharedChunks;
			std::vector<String> outOfClosureReferrers;
			std::vector<String> owners;

			//! Chunks orphaned by this rewire (not auto-deleted; cleared whenever `!commit.applied`).
			std::vector<String> nowUnreferenced;
		};

		//! Re-point `(targetCategory, targetName).param[occurrence]` to `(newRefCategory, newRefName)`.
		//! Load-bearing gate order (steps 1-4 leave Document byte-identical on refusal):
		//!   1. Render-locked / no-document pre-flight   2. Ambiguity-refusing endpoint resolution
		//!   3. ConnectionLegality + WouldCycle          4. OwnershipClosure::Compute (§3.7a)
		//!   5. Commit via ApplyAgentParamEditInner_ (txn/conflict/undo/rebind/dirty/epoch/kick).
		//! Refuses out-of-range occurrence, tuple-reference params, and non-chunk detach (`none`).
		RewireResult RewireConnection(
			ChunkCategory targetCategory, const String& targetName,
			const String& param, int occurrence,
			ChunkCategory newRefCategory, const String& newRefName,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Insert an R-channel scalar_painter after the source and bind the
		//! target slot, as one operation-level undo batch. Raster painters
		//! use `texture`; other Color-capable painters use `painter`.
		RewireResult InsertScalarAdapterAndRewire(
			ChunkCategory targetCategory, const String& targetName,
			const String& param, int occurrence,
			ChunkCategory sourceCategory, const String& sourceName,
			String* outAdapterName = nullptr,
			const Cst::CstHeadVersion* baseVersionOrNull = nullptr );

		// Reference-safe graph delete & positioned fork (docs/gui/NODE_GRAPH_CANVAS.md §6,
		// docs/gui/ENTITY_CREATION.md §5). ChunkCategory-addressed siblings of RewireConnection
		// (leaving UI-Category RemoveEntity / DuplicateEntity unchanged for the outliner).

		//! Downstream handling for `DeleteGraphNode`.
		enum class GraphDeleteMode
		{
			//! Delete target only; newly orphaned downstream chunks remain in the document.
			TargetOnly = 0,
			//! Delete target + solely-owned downstream closure in one atomic undoable batch (never shared).
			Cascade    = 1
		};

		//! Reference-safe delete outcome + blocking referrers / removed chunk list.
		struct DeleteResult
		{
			//! Underlying commit (every refusal is `status == "rejected"`, head byte-identical).
			AgentCommitResult commit;

			//! `AmbiguousTargetName`/`UnresolvedTarget` on lookup failure; `Clean` past resolution.
			ClosureClassification closure = ClosureClassification::Clean;

			//! True when the refusal was "still referenced" -- so a bridge can
			//! offer "rewire those away" without parsing prose.
			bool referenceRefused = false;

			//! True when a CASCADE was refused because its sweep would have
			//! reached a chunk another graph owns.
			bool cascadeRefused = false;

			//! Blocking referrers as `chunk`.`param` (`<keyword>`.`param` if unnamed); empty on success.
			std::vector<String> referrers;

			//! Removed chunks in document order (always empty on any refusal).
			std::vector<String> removed;
		};

		//! Reference-safe delete for any editor-addressable ChunkCategory (docs/gui/ENTITY_CREATION.md §5).
		//! Load-bearing gate order (steps 1-4 leave Document byte-identical on refusal):
		//!   1. Render-locked/teardown/no-doc  2. Ambiguity-refusing ResolveChunk
		//!   3. FindReferencesTo (any referrer refuses in both modes)
		//!   4. Cascade: cross-check every member via OwnersOf + addressability
		//!   5. Commit via ApplyAgentRemoveChunk (TargetOnly) or ApplyAgentRemoveChunks (Cascade, 1 Undo).
		DeleteResult DeleteGraphNode(
			ChunkCategory category, const String& name,
			GraphDeleteMode mode,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! `DuplicateGraphNode`'s answer.
		struct DuplicateResult
		{
			AgentCommitResult commit;
			ClosureClassification closure = ClosureClassification::Clean;
			//! Deduped copy name (empty unless Document mutated).
			String newName;
			//! Original chunk's document index (-1 if nothing landed).
			int    originalIndex = -1;
		};

		//! Shallow-fork `(category, name)` (Painter/Function/Material only) into an owned copy
		//! spliced immediately after the original in declaration order (docs/gui/MATERIAL_EDITOR.md §3.7a).
		//! Copy keeps links to downstream leaves. Non-mutating refusal on unknown/ambiguous/unnamed target.
		DuplicateResult DuplicateGraphNode(
			ChunkCategory category, const String& name,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Remove named entity in `cat` via ApplyAgentRemoveChunk (narrowed by role-kind suffix).
		AgentCommitResult RemoveEntity( Category cat, const String& name );

		// Viewport render modes (docs/gui/RENDER_MODES.md §4-§5)
		// Parks render under mMutex to swap caster/variant; resets to "preview" on every scene load.

		//! Switch viewport mode (`preview`/`normals`/`depth`/`facets`/`wireframe`; not `objectmap`).
		//! No-op true if already active; false on unknown/non-viewport mode, busy render, or headless.
		bool SetViewportRenderMode( const char* name );

		//! Active viewport render-mode wire name (never null; "preview" by default/reset).
		const char* GetViewportRenderMode() const;

		// X-ray axis (docs/gui/RENDER_MODES.md): straight-line continuation through transmissive
		// surfaces across all modes (default off; reset on RebindEditorToJob).

		//! Toggle x-ray view across all casters under parked mMutex (false if render busy or headless).
		bool SetViewportXray( bool xray );

		//! The CURRENT x-ray flag (false by default and after every
		//! RebindEditorToJob reset -- see the axis doc above).
		bool GetViewportXray() const;

		// N-up multi-viewport pane model (docs/gui/RENDER_MODES.md §7)
		// 4 persistent pane slots (hidden panes retain config); pane 0 aliases single-viewport state.
		// Setters fail-closed on hidden panes or while a render owns the scene.

		enum class ViewportLayout : int
		{
			Single     = 0,   //!< pane 0 only (today's viewport)
			TwoH       = 1,   //!< panes 0 | 1 side-by-side
			OnePlusTwo = 2,   //!< pane 0 big + panes 1,2 stacked right
			Quad       = 3,   //!< panes 0-3 in a 2x2 grid
		};

		enum class PaneVantageKind : int
		{
			SceneCamera = 0,  //!< track the live active scene camera
			FreeFly     = 1,  //!< per-pane free-fly pose (never mutates the scene camera)
			NamedView   = 2,  //!< re-resolved by name each pass; falls back to the snapshot if deleted
			SceneCameraNamed = 3, //!< track one manager-registered scene camera by name
		};

		//! T4: top-level pane content source.  Stable C-ABI wire values;
		//! LastRender is deliberately orthogonal to the render-mode registry.
		enum class PaneContentSource : int
		{
			Interactive = 0,
			LastRender  = 1,
		};

		static constexpr unsigned int kViewportPaneCount = 4;
		//! Frame marker used for persistent Last Render sink delivery.  Shipping
		//! GUI sinks use it to move full-resolution conversion off the UI caller.
		static constexpr unsigned int kLastRenderSinkFrame = ~0u;

		//! Atomic snapshot of all 4 panes captured with CopyInteractiveFrame.
		struct PaneSetSnapshot
		{
			int          layout  = 0;
			unsigned int primary = 0;
			struct Pane
			{
				bool                               visible = false;
				PaneContentSource                  contentSource = PaneContentSource::Interactive;
				Implementation::ViewportRenderMode mode = {};   // value-init == 0 == Preview (enum not fully defined here)
				PaneVantageKind                    vantageKind = PaneVantageKind::SceneCamera;
				String                             namedView;
			} panes[kViewportPaneCount];
		};

		//! Capture pane set into `out`. REQUIRES non-recursive `mMutex` already held (parked read).
		void SnapshotPaneSetForParkedRead( PaneSetSnapshot& out ) const;

		//! Panes a layout makes visible: Single=1, TwoH=2, OnePlusTwo=3, Quad=4.
		static unsigned int PaneCountForLayout( ViewportLayout layout );

		//! Switch layout (primary falls back to pane 0 if hidden; false while render owns scene).
		bool SetViewportLayout( ViewportLayout layout );
		ViewportLayout GetViewportLayout() const;

		//! Make `pane` primary (owner of editing / picking / gizmos).  The
		//! pane must be visible in the current layout.  False on an invalid
		//! or hidden pane, or while a render owns the scene.
		bool SetPrimaryPane( unsigned int pane );
		unsigned int GetPrimaryPane() const;

		//! Set visible pane's render mode (pane 0 forwards to SetViewportRenderMode; hidden panes refuse).
		bool SetPaneRenderMode( unsigned int pane, const char* name );
		const char* GetPaneRenderMode( unsigned int pane ) const;

		//! Select Interactive or LastRender for a visible pane (pane 0 refuses LastRender in Single layout).
		bool SetPaneContentSource( unsigned int pane, PaneContentSource source );
		PaneContentSource GetPaneContentSource( unsigned int pane ) const;

		//! Deep-copy and publish completed render image to LastRender panes (caller owns parked render slot).
		bool PublishAgentRenderImageParked( const IRasterImage& image );
		//! Ownership-transfer sibling of PublishAgentRenderImageParked (nulls `image` on success).
		bool AdoptAgentRenderImageParked( IRasterImage*& image );

		//! Set visible pane's surface pixel dims (`0/0` resets to film dims; marks pane dirty).
		bool SetPaneSurfaceDims( unsigned int pane, unsigned int w, unsigned int h );

		//! Set per-pane preview sink (controller addrefs; null clears; pane 0 falls back to legacy sink).
		bool SetPaneSink( unsigned int pane, IRasterizerOutput* pSink );

		//! Pane-indexed pointer input: promotes clicked pane to primary, parks/switches before arming
		//! gesture pin; secondary `SceneCamera` panes convert to FreeFly on camera drag, while
		//! `SceneCameraNamed` edits its bound manager camera via Undo. False if hidden/invalid/render busy.
		bool OnPanePointerDown( unsigned int pane, const Point2& px );
		bool OnPanePointerMove( unsigned int pane, const Point2& px );
		bool OnPanePointerUp( unsigned int pane, const Point2& px );

		//! Per-pane free-fly entry/exit (pane 0 aliases EnterFreeFlyFromActiveCamera/ExitFreeFly).
		bool PaneEnterFreeFly( unsigned int pane );
		bool PaneExitFreeFly( unsigned int pane );

		//! Per-pane refinement phase/divisor (reads live registers for current pane, saved slot otherwise).
		bool GetPaneRefinementStatus( unsigned int pane, int& outPhase,
		                              unsigned int& outScaleDivisor ) const;

		bool SetPaneVantageSceneCamera( unsigned int pane );
		bool SetPaneVantageSceneCameraNamed( unsigned int pane, const char* name );
		bool SetPaneVantageNamedView( unsigned int pane, const char* name );
		//! Introspection: current kind (+ referenced name for NamedView /
		//! SceneCameraNamed).
		bool GetPaneVantage( unsigned int pane, PaneVantageKind& outKind,
		                     String& outNamedView ) const;

		// Environment / IBL singleton (docs/gui/APPROACHABILITY_FOUNDATION.md)
		// Wraps the active rasterizer's `radiance_*` params + bound HDR/EXR painter.
		struct EnvironmentInfo
		{
			bool   hasEnvironment = false;  //!< a radiance_map painter is bound (name != "none")
			bool   proceduralSky  = false;  //!< a procedural sky / non-painter map is installed (read-only in v1)
			String painterName;             //!< bound painter name ("" if none)
			String file;                    //!< resolved HDRI file path ("" if unresolved / procedural)
			double scale = 1.0;             //!< intensity multiplier
			double orientDeg[3] = { 0.0, 0.0, 0.0 };  //!< Euler rotation in DEGREES (converted from stored radians)
			bool   background = true;       //!< map visible behind geometry (primary-ray visibility)
			bool   editable = false;        //!< false when the active rasterizer takes no radiance map (MLT) or none exists
		};

		//! Read active rasterizer's environment binding + CST painter file (false if no scene/rasterizer).
		bool GetEnvironment( EnvironmentInfo& out ) const;

		//! Update live radiance params + CST mirror (`Job::ApplyCstEnvironmentEdit`; false if uneditable).
		bool SetEnvironmentScale( double scale );
		bool SetEnvironmentBackground( bool background );
		bool SetEnvironmentOrient( double xDeg, double yDeg, double zDeg );

		//! Swap bound environment painter's HDRI file via full CST re-derive (false if unbound).
		bool SetEnvironmentFile( const String& absPath );

		//! Insert `hdr_painter`/`exr_painter` chunk and bind `radiance_map` on active rasterizer.
		AgentCommitResult AddEnvironment( const String& hdriPath, String* outPainterName );

		//! Remove the environment: unbinds radiance_map on the active
		//! rasterizer (live + CST) so no global radiance map is installed.
		//! The painter chunk is left in the Document (harmless, reusable).
		bool RemoveEnvironment();

	protected:
		//! Overridable single interactive render pass (mocked in unit tests).
		virtual void DoOneRenderPass();

		//! Test seam: called unlocked before RenderLoop re-acquires mMutex to mint an interactive pass.
		virtual void ForTest_OnAboutToMintInteractivePass() {}

		//! Test seam: called unlocked after interactive pass mint, before pass body.
		virtual void ForTest_OnInteractivePassMinted() {}
		//! Test seam: called after BeautyVariant live/final quality configuration.
		virtual void ForTest_OnBeautyVariantPassConfigured(
			IRasterizer&, Implementation::ViewportRenderMode, bool ) {}
		//! Test seam: called after mRendering=false, before rasterizer detaches output refs.
		virtual void ForTest_OnInteractivePassBeforeOutputDetach() {}
		//! Test seam: called after pass body and post-pass transitions retire.
		virtual void ForTest_OnInteractivePassRetired() {}

		//! Test seam: called in OnPointerUp after final-render/polish state and wake publish under mMutex.
		virtual void ForTest_OnPointerUpAfterFinalRenderArmed() {}

		//! Test seam: called after PointerDown publishes gesture, before opening tool composite.
		virtual void ForTest_OnPointerDownAdmitted() {}

		//! Test seam: called after TimeScrubBegin opens composite while still holding admission.
		virtual void ForTest_OnTimeScrubBeginAdmitted() {}

		//! Test seam: called under mMutex after OnPointerMove is counted in-flight.
		virtual void ForTest_OnPointerMoveAdmitted() {}

		//! Test seam: called after Last Render drain registers lifetime use, before delivery barrier.
		virtual void ForTest_OnLastRenderDrainRegistered() {}
		//! Test seam: called after teardown closes Last Render drain registration, before quiesce wait.
		virtual void ForTest_OnLastRenderQuiesceStarted() {}
		//! Test seam: called before pane source transition attempts Last Render delivery barrier.
		virtual void ForTest_OnLastRenderTransitionAttempt( unsigned int ) {}

		//! Test seam: called under mMutex only when SetViewportLayout shrink parks an active/gestured
		//! pane (`mRendering == false`, new layout authoritative). Covers pass-body state only, not
		//! unlocked post-pass tails.
		virtual void ForTest_OnViewportShrinkParked() {}

		//! Test seam: called unlocked in AgentRenderWorkerLoop_ after slot release, before CancelAndParkRender_.
		virtual void ForTest_OnAgentWorkerAboutToParkRender() {}
		//! Test seam: called before coordinated submission attempts mRenderAdmissionMutex.
		virtual void ForTest_OnCoordinatedRenderAdmissionAttempt() {}

		//! Test seam: called after RunPreviewRenderParked registers lifetime with Stop(), before admission.
		virtual void ForTest_OnDirectRenderBeforeAdmission() {}

		//! Test seam: called unlocked in RequestSave after mSaving=true & lease, before SaveEngine::Save.
		virtual void ForTest_OnSaveEngineAboutToRun() {}

	private:
		RewireResult RewireConnectionInner_(
			ChunkCategory targetCategory, const String& targetName,
			const String& param, int occurrence,
			ChunkCategory newRefCategory, const String& newRefName,
			const Cst::CstHeadVersion* baseVersionOrNull,
			bool insertScalarAdapter, String* outAdapterName );

		// Node-graph spotlight / focused-view helpers -----------------

		//! Non-blocking object -> live bound material name lookup (sets `*outDegraded` on contention).
		String ResolveObjectMaterialNameLocked_( const String& objectName, bool* outDegraded = nullptr ) const;

		//! Pure `outEdges`-only BFS from `startIdx` in discovery order.
		static std::vector<unsigned int> BFSGraphClosure( const PainterMaterialGraph& g, unsigned int startIdx );

		//! Pure subgraph filter to node indices `keep` (skips out-of-range entries without shifting remaps).
		static PainterMaterialGraph FilterPainterMaterialGraph(
			const PainterMaterialGraph& g, const std::vector<unsigned int>& keep );

		bool PrepareForDestructionClaimed_();
		void RenderLoop();
		void KickRender();

		//! Capture current view (free-fly pose if active, else active scene camera under park).
		bool CaptureCurrentView( CameraSnapshot& out );

		//! Long-lived single-slot agent/production render worker (started in ctor, joined in Stop()).
		//! Parks interactive thread under mMutex, runs closure with RAII active-flag guard, captures
		//! exceptions into `mAgentRenderException`, releases mMutex, then notifies `mAgentRenderDoneCV`.
		void AgentRenderWorkerLoop_();

		//! Map `FinalizeOpenInteractions() == false` to `InteractionFinalizeLatched` (if sticky
		//! `mInteractionPersistenceFailed` is set; never clears it) or retriable `InteractionFinalizeFailed`.
		RenderRefusal ClassifyFinalizeFailure_() const;

		//! Shared slot-claim core (caller holds `mAgentRenderSlotMutex` via `slotLk`; caller seeds `outRefusal`).
		bool SubmitAgentRenderAsync_Locked(
			std::unique_lock<std::mutex>& slotLk,
			std::function<void()>         fn,
			const String&                 clientLabel,
			RenderJobId*                  outJobId,
			bool                          bypassFairQueueCheck,
			bool                          pinned = false,
			RenderClass                   renderClass = RenderClass::AgentPreview,
			RenderRefusal*                outRefusal = nullptr );

		//! Ground-truth check (under `mAgentRenderSlotMutex`) that worker is not inside `id`'s closure.
		bool AgentRenderSlotIdleFor_( RenderJobId id ) const;

		//! Trip cancel flag if rendering and wait on `mCV` until `!mRendering` (caller holds `mMutex` via `lk`).
		void CancelAndParkRender_( std::unique_lock<std::mutex>& lk );

		//! Capture prior `param[occ]` value/absence under `mMutex` immediately before ApplyCstParamEditChecked.
		bool CaptureAgentPriorParamValue_(
			const String& entityName, const String& entityKind, const String& param,
			String& outPrevValue, bool& outWasAbsent, int occ = 0 );

		//! Capture chunk bytes, index, and rasterizer flag under `mMutex` before ApplyCstRemoveChunk.
		bool CaptureAgentChunkForRemoveUndo_(
			const String& target, const String& kind,
			String& outBytes, int& outIndex, bool& outWasRasterizer );

		//! True iff target resolves in retained Document to a geometry-less `standard_object` container.
		bool AgentTargetIsContainerObject_( const String& entityName, const String& entityKind );

		//! Critical sections for agent chunk CRUD / batch remove / edit batch (caller holds `mRenderAdmissionMutex`).
		AgentCommitResult ApplyAgentRemoveChunksCrud_(
			const std::vector<String>& targets,
			const std::vector<String>& kinds,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		std::vector<AgentCommitResult> ApplyAgentEditBatchCrud_( const std::vector<CstBatchEdit>& edits );

		//! Shared whole-document swap critical section for ReplaceGeometry and DuplicateGraphNode
		//! (caller holds `mRenderAdmissionMutex`).
		AgentCommitResult ApplyAgentReplaceGeometryCrud_(
			const String& objectName,
			const String& candidateDocText,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull,
			const char* verbLabel,
			const char* entityKind = "standard_object",
			const char* noun = "geometry replacement",
			const char* appliedPhrase = "geometry replaced",
			bool duplicateNodeOp = false );

		AgentCommitResult ApplyAgentChunkCrud_(
			bool isInsert,
			const String& a,
			const String& b,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Apply live rasterizer radiance param + CST mirror under park (empty `radiance_map` unbinds;
		//! refuses in open txn or MLT; `*outPersisted` reports whether CST mirror succeeded).
		bool SetEnvironmentRadianceParam_( const char* paramName, const std::string& value,
			bool* outPersisted = nullptr );

		//! Re-point mEditor at Job's current scene/managers after any whole-scene re-derive.
		void RebindEditorToJob();

		//! Ray-pick at image pixel `px` and update object selection.
		void PickAt( const Point2& px );

		//! Resize/reuse `mInteractiveFrameStore` and bind to `activeRast` (no-op if null).
		void EnsureInteractiveFrameStore_( unsigned int width, unsigned int height, IRasterizer* activeRast );

		//! Re-sync secondary Material/Medium selection from pinned Object after Undo/Redo.
		void ResyncObjectBoundSections_();
		//! Clear selection if its entity no longer resolves (called unconditionally after Undo/Redo).
		void DropStaleSelection_();

		//! Build and install view-mode caster under parked `mMutex` (preserves current x-ray flag).
		bool InstallViewModeCaster_( Implementation::ViewportRenderMode mode );

		IJobPriv&                   mJob;
		IRasterizer*                mInteractiveRasterizer;  // borrowed
		// Downcast cache of mInteractiveRasterizer (null in headless/test mode).
		Implementation::InteractivePelRasterizer* mInteractiveImpl;
		// Owned BeautyVariant rasterizer (deep_reflect/direct; mutated under parked mMutex; reset on reload).
		IRasterizer*                mVariantRasterizer;
		// Active viewport render mode (under mMutex; reset to Preview on load).
		Implementation::ViewportRenderMode mViewportRenderMode;
		// Active x-ray flag (under mMutex; default off, reset on load).
		bool                                mViewportXray;
		SceneEditor                 mEditor;
		Tool                        mTool;
		//! Per-category last-used sub-tool memory.
		Tool                        mLastSubToolPerCategory[ kNumToolCategories ];
		//! Cached screen-space gizmo handles.
		std::vector<GizmoHandle>    mGizmoHandles;
		//! Film pixels per primary-pane surface pixel for stable visual gizmo sizing.
		double                      mGizmoPixelScale = 1.0;

		//! Cached navigation axis-ball nubs (0 or 6).
		std::vector<NavGizmoNub>    mNavGizmoNubs;

		//! Active gizmo drag state captured at OnPointerDown (constant screen-space `axisDir` per drag).
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
			double  pixelScale = 1.0; ///< film pixels per displayed pixel at drag-start
			double  axisDirX[3];      ///< pixels per world unit, x component
			double  axisDirY[3];      ///< pixels per world unit, y component
			bool    axisOk[3];        ///< false if axis colinear with view at drag-start
			Vector3 prevOrient;       ///< object Euler at drag-start (for Rotate)
			Matrix4 dragStartMatrix;  ///< local-matrix anchor for scale drag
			TransformStateV2 dragStartState;  ///< exact components, stack entries, and
			                                ///< authoritative-matrix metadata at drag-start.
			bool    dragStartStateValid;    ///< F6: dragStartState captured this drag.
			double  prevAngle;        ///< pointer angle around pivot (for Ring drags)
		};
		GizmoDragState              mGizmoDrag;
		// UI-thread selection state: per-category picks + primary tuple.
		String                      mSelectionByCategory[ kNumCategories ];
		//! Per-category accordion expand flags (tracked independently of `mSelectionByCategory`).
		bool                        mSectionExpanded[ kNumCategories ];
		Category                    mSelectionCategory;
		String                      mSelectionName;
		// Structural mutation epoch polled by UI shells.
		std::atomic<unsigned int>   mSceneEpoch;
		Point2                      mLastPx;
		std::atomic<bool>           mPointerDown;
		// Tracks whether THIS pointer gesture opened an editor composite.
		bool                        mGestureOpenedComposite;
		// Tracks whether THIS time scrub opened an editor composite.
		bool                        mScrubOpenedComposite;
		bool                        mTimelineScrubWatchdogRecovered = false;
		//! Guarded by mMutex: keeps BeautyVariant cancellation replacement at live quality during admission.
		bool                        mVariantInteractionAdmissionPending = false;
		unsigned int                mVariantInteractionAdmissionPane = 0;

		// Property-panel scrub flag (separate from mPointerDown; auto-cleared by kScrubWatchdogMs).
		std::atomic<bool>           mScrubInProgress;

		IRasterizerOutput*          mPreviewSink;
		IProgressCallback*          mProgressSink;
		ILogPrinter*                mLogSink;

		// Owned per-pass interactive FrameStore reused across same-dim passes.
		mutable RISE::Implementation::FrameStore* mInteractiveFrameStore;
		//! Pane owning `mInteractiveFrameStore` (guarded by `mInteractiveFrameStoreMutex`).
		mutable unsigned int              mInteractiveFrameStorePane = 0;

		// Leaf mutex guarding ONLY `mInteractiveFrameStore` pointer/pane swaps (never nests with other locks).
		mutable std::mutex mInteractiveFrameStoreMutex;

		CancellableProgressCallback mCancelProgress;

		// Render-thread machinery -----------------------------------

		std::thread                 mRenderThread;
		mutable std::mutex          mMutex;
		std::condition_variable     mCV;
		std::atomic<bool>           mRunning;
		std::atomic<bool>           mEditPending;

		//! Leaf mutex guarding proposal queue (bounded by kMaxPendingProposals & kMaxProposalHistory;
		//! oldest resolved entry evicted at history cap; pending entries never evicted).
		mutable std::mutex          mProposalMutex;
		std::vector<AgentProposal> mProposals;
		//! Monotonic proposal id counter (starts at 1; guarded by mProposalMutex).
		std::uint64_t               mNextProposalId = 1;
		//! One-shot flag consumed by RenderLoop to skip startup pass after production render.
		std::atomic<bool>           mSuppressInitialRender;
		std::atomic<bool>           mRendering;
		// True during lock-free save I/O; checked at both RenderLoop wake and in-lock mint gate.
		std::atomic<bool>           mSaving;
		// Recursive admission handshake between renders and UI ops (never held across render body).
		// Lock order: mAgentRenderSlotMutex -> mRenderAdmissionMutex -> mMutex.
		// `mAgentRenderBlocksInteractive` is checked at RenderLoop wake AND re-checked under mMutex at mint.
		mutable std::recursive_mutex mRenderAdmissionMutex;
		std::atomic<bool>           mAgentRenderBlocksInteractive;
		//! Leaf state tracking active direct `RunPreviewRenderParked` callers so Stop() drains them.
		mutable std::mutex          mDirectRenderStateMutex;
		std::condition_variable     mDirectRenderDoneCV;
		//! Guarded by `mDirectRenderStateMutex`; `mDirectRenderStopping` is terminal.
		unsigned int                mDirectRenderCallCount;
		bool                        mDirectRenderStopping;
		//! Preserves Stop/cancel arriving before direct render resets shared `mCancelProgress`.
		std::atomic<bool>           mDirectRenderCancelRequested;
		std::string                 mLastSaveError;
		std::atomic<unsigned int>   mCancelCount;
		std::atomic<unsigned int>   mAgentCancelRequestCount;   // see ForTest_GetAgentCancelRequestCount
		std::atomic<unsigned int>   mRenderCount;

		// Render-identity status record guarded by narrow `mJobStatusMutex` (never held across a render).
		// `mNextRenderJobId` starts at 2 and increments by `kControllerRenderJobIdStride` (even ids).
		mutable std::mutex mJobStatusMutex;
		RenderJobId     mNextRenderJobId;
		RenderJobStatus mCurrentRenderJob;

		// Dedicated long-lived agent/production render worker (started in ctor, joined in Stop()).
		// Lock ordering (outermost -> innermost; never reversed concurrently):
		//   AgentSession::mAsyncCacheMutex -> mAgentRenderSlotMutex -> mRenderAdmissionMutex -> mMutex
		//   mAgentRenderSlotMutex / mMutex -> mJobStatusMutex (leaf; never held across renders)
		// Worker holds `mMutex` for the render body; `mAgentRenderSlotMutex` guards only microsecond slot state.
		std::thread                 mAgentRenderThread;
		mutable std::mutex          mAgentRenderSlotMutex;
		std::condition_variable     mAgentRenderCV;       // worker waits on this for {pending | stop} -- guarded by mAgentRenderSlotMutex
		// mutable: WaitForRenderJob is logically const (a read-only status
		// poll) but must block on this CV.
		mutable std::condition_variable mAgentRenderDoneCV;   // submitter(s) wait on this for "the slot freed up" -- guarded by mAgentRenderSlotMutex
		std::atomic<bool>           mAgentRenderStop;      // set by Stop(); wakes the worker to exit
		bool                        mAgentRenderPending;   // a submission is queued or currently running -- guarded by mAgentRenderSlotMutex
		std::function<void()>       mAgentRenderFn;        // the pending/running submission -- guarded by mAgentRenderSlotMutex
		//! Atomic per-occupant cancel latch (written under slot lock, re-checked after taking mMutex).
		std::atomic<bool>           mAgentRenderCancelRequested;
		//! Lock-free flag set while a coordinated/direct render closure holds `mMutex`.
		//! Guarded UI methods fast-path no-op/default instead of blocking behind long renders.
		std::atomic<bool>           mRenderOwnsScene;
		RenderJobId                 mAgentRenderJobId;     // the id assigned to the CURRENT slot occupant -- guarded by mAgentRenderSlotMutex
		//! True iff current slot occupant was submitted pinned (guarded by `mAgentRenderSlotMutex`).
		bool                        mAgentRenderPinned;
		//! Captured worker exception rethrown by SubmitAgentRenderSync (cleared on new submit; slot lock).
		std::exception_ptr          mAgentRenderException;

		//! FIFO fairness ticket state preventing sync-caller starvation under async contention
		//! (guarded by `mAgentRenderSlotMutex`; async submits refuse while WaitingSyncCount > 0).
		unsigned long long          mAgentRenderNextTicket;
		unsigned long long          mAgentRenderServingTicket;
		unsigned int                mAgentRenderWaitingSyncCount;

		// Full-res camera dims captured before per-pass preview-scale swap (atomic for UI GetCameraDimensions).
		std::atomic<unsigned int>   mFullResW;
		std::atomic<unsigned int>   mFullResH;
		//! Guards CameraCommon frame/matrix reads against DoOneRenderPass's
		//! temporary ResizeFilm swap.  UI overlays take this with try_lock so
		//! they defer rather than block during an interactive pass.
		std::mutex                  mCameraFrameMutex;

		std::atomic<unsigned int>   mPreviewScale;
		// True while a BeautyVariant mode pins `mPreviewScale` (mutated under mMutex).
		std::atomic<bool>           mPreviewScalePinned;
		//! Single choke point storing `mPreviewScale` only when `!mPreviewScalePinned`.
		void SetPreviewScaleIfUnpinned_( unsigned int v );
		static constexpr unsigned int kPreviewScaleMin = 1;
		static constexpr unsigned int kPreviewScaleMax = 32;
		static constexpr unsigned int kPreviewScaleMotionStart = 4;
		// Adaptive preview-resolution ladder (1..32) and 30 Hz render-time hysteresis bands.
		static constexpr int        kTargetMs = 33;
		static constexpr int        kSlowMs   = 100;
		static constexpr int        kFastMs   = 16;
		// Idle refinement timing while pointer is held still.
		static constexpr int        kRefineIdleMs = 150;
		static constexpr int        kRefineWakeMs = 100;

		// Auto-clear property scrub flag if EndPropertyScrub is lost.
		static constexpr int        kScrubWatchdogMs = 1500;

		// Pointer-gesture watchdog: marks idle gesture stale so sibling panes resume rotating;
		// later Move reclaims gesture pane, and late PointerUp still finalizes.
		static constexpr int        kPointerWatchdogMs = 10000;

		//! Mutable watchdog threshold (overrideable by tests via ForTest_SetPointerWatchdogMs).
		std::atomic<int>            mPointerWatchdogMs { kPointerWatchdogMs };

		// Steady-clock timestamp (ms) of last KickRender for idle refinement & pause-resume snap.
		std::atomic<long long>      mLastEditTimeMs;

		// Set by pointer watchdog while keeping `mPointerDown` true so late PointerUp still finalizes.
		std::atomic<bool>           mPointerGestureStale{ false };
		//! Sticky truth latch set on pending-CST commit failure and NEVER cleared.
		//! Forces FinalizeOpenInteractions() -> false (`RenderRefusal::InteractionFinalizeLatched`).
		std::atomic<bool>           mInteractionPersistenceFailed { false };
		//! One-shot test flag forcing a transient FinalizeOpenInteractions failure.
		std::atomic<bool>           mForTestFinalizeFailOnce { false };

		//! Stamped at gesture start and top of OnPointerMove (before wait) for pointer watchdog.
		std::atomic<long long>      mLastPointerActivityMs { 0 };

		//! Active OnPointerMove count (watchdog stands down while > 0).
		std::atomic<int>            mPointerMovesInFlight { 0 };
		//! Timeline counterpart to mPointerMovesInFlight.  Prevents the lost-End
		//! watchdog from closing/reclassifying a scrub callback while that callback
		//! is parked behind an obsolete render quantum.
		std::atomic<int>            mTimeScrubsInFlight { 0 };

		// Set by ~SceneEditController before Stop() so orphan cleanup skips mJob CST commits.
		std::atomic<bool>           mInDestructorTeardown{ false };
		enum DestructionState
		{
			DestructionOpen = 0,
			DestructionPreparing,
			DestructionPrepared,
			DestructionDeletePreparing,
			DestructionDestroying
		};
		std::atomic<int>            mDestructionState{ DestructionOpen };
		std::atomic<bool>           mPreparationSucceeded{ true };

		// Set by RenderLoop on idle-refinement passes to suppress during-motion scale adaptation.
		bool                        mInRefinementPass;
		// Render-thread register populated alongside the per-pass denoise
		// configuration and consumed by fire-fidelity preflight.
		bool                        mCurrentPassUsesOidn{ false };

		// Post-gesture polish state machine: 1-SPP scale=1 final pass -> 4-SPP polish pass (cancelled by KickRender).
		enum class PolishState : int { None = 0, FinalRegularRunning = 1, PolishQueued = 2 };
		std::atomic<int>            mPolishState;
		static constexpr unsigned int kPolishSampleCount = 4;

		// Serializes Start()/StopInteractive() thread spawn/join (never acquired by render thread).
		std::mutex                  mLifecycleMutex;

		// True between PauseRefinement() and ResumeRefinement()/Start().
		std::atomic<bool>           mRefinementPaused{ false };

		// Packed 16-bit inclusive ROI coords (`left<<48 | top<<32 | right<<16 | bottom`).
		std::atomic<bool>           mInteractiveRegionActive{ false };
		std::atomic<std::uint64_t>  mInteractiveRegionPacked{ 0 };

		// Transient free-fly pose and owned standalone ICamera override (swapped under parked mMutex).
		CameraSnapshot              mViewportPose;
		bool                        mViewportPoseActive = false;
		ICamera*                    mViewportOverrideCamera = nullptr;

		// Session Home view bookmark (non-destructive).
		CameraSnapshot              mHomeView;
		bool                        mHasHomeView = false;

		// Session Named Views guarded by leaf `mNamedViewsMutex` (read by agent worker via FindNamedViewPose).
		mutable std::mutex          mNamedViewsMutex;
		std::vector<NamedView>      mNamedViews;

		// N-up desired pane configs (guarded by mMutex; pane 0 aliases single-viewport mode/vantage).
		struct PaneConfig
		{
			Implementation::ViewportRenderMode mode {};
			PaneContentSource                  contentSource = PaneContentSource::Interactive;
			PaneVantageKind                    vantageKind = PaneVantageKind::SceneCamera;
			CameraSnapshot                     pose {};      // named-target deletion fallback / FreeFly
			String                             namedViewRef; // valid when vantageKind==NamedView
			String                             sceneCameraRef; // valid when vantageKind==SceneCameraNamed
			//! P3a slice 3: per-pane render surface in pixels (the GUI's
			//! pane rect).  0/0 = "use the film's rest dims" -- pane 0's
			//! default, byte-identical single-viewport behaviour.
			unsigned int                       surfaceW = 0;
			unsigned int                       surfaceH = 0;
		};
		PaneConfig                  mPaneConfigs[kViewportPaneCount];  // [0] contentSource only; mode/vantage alias
		//! Lock-free secondary-pane mode snapshots for UI polling (published under mMutex).
		std::atomic<int>            mPaneModeSnapshots[kViewportPaneCount] {};
		ViewportLayout              mViewportLayout = ViewportLayout::Single;
		unsigned int                mPrimaryPane    = 0;   // always visible in layout
		//! Lock-free layout/primary snapshots for non-blocking UI paint/timer polls.
		std::atomic<int>            mViewportLayoutSnapshot { 0 };
		std::atomic<unsigned int>   mPrimaryPaneSnapshot { 0 };

		// Context-switch scheduler state (docs/gui/RENDER_MODES.md §7.3): scalar members are
		// working registers; `mPaneRender[i]` holds saved state for unscheduled panes (swapped under mMutex).
		struct PaneRenderState
		{
			bool                         dirty = true;    // needs a pass (scene edit / config change)
			uint64_t                     dirtyGeneration = 1;
			int                          polishSaved = 0; // PolishState as int (None) -- saved register
			unsigned int                 previewScaleSaved  = 1;
			bool                         previewPinnedSaved = false;
			ICamera*                     overrideCamera = nullptr;  // realized vantage (owned addref; null => scene camera)
			bool                         poseActive = false;        // saved mViewportPoseActive register
			CameraSnapshot               poseSaved {};              // saved mViewportPose register
			Implementation::FrameStore*  frameStore = nullptr;      // saved store (owned addref; null => allocate lazily)
			IRasterizer*                 variantRasterizer = nullptr;  // saved variant pipeline (owned)
			//! Owned per-pane view-mode caster (persists across switches to preserve depth auto-window).
			IRayCaster*                  viewModeCaster = nullptr;
			//! Mode last realized for this slot (compared against config at switch time).
			Implementation::ViewportRenderMode realizedMode {};
			//! Owned per-pane sink (null => legacy `mPreviewSink` for pane 0).
			IRasterizerOutput*           paneSink = nullptr;
			//! Set by panes 1-3 config setters; reconciled by SwitchToPaneLocked_.
			bool                         configDirty = false;
		};
		PaneRenderState             mPaneRender[kViewportPaneCount];
		unsigned int                mCurrentPane = 0;   // render-thread-owned; UI reads under mMutex

		//! Rotation continuation wake flag (distinct from `mEditPending` so edit timestamps aren't faked).
		std::atomic<bool>           mPanePassPending { false };

		struct SharedDirectTarget
		{
			unsigned int pane = kViewportPaneCount;
			unsigned int width = 0;
			unsigned int height = 0;
			uint64_t dirtyGeneration = 0;
		};
		//! Render-thread working snapshot, populated under mMutex at mint and
		//! consumed by DoOneRenderPass before the quantum retires.
		std::vector<SharedDirectTarget> mSharedDirectTargets;
		struct PendingSharedDirectPublish
		{
			unsigned int pane = kViewportPaneCount;
			uint64_t dirtyGeneration = 0;
			Scalar cameraExposureEV = 0;
			IRasterImage* image = nullptr;       //!< owning reference
			IRasterImage* rawImage = nullptr;    //!< owning reference; non-null iff denoised
		};
		//! Built by DoOneRenderPass, drained only after ActiveFlipGuard has
		//! published mRendering=false so arbitrary sink re-entry cannot deadlock.
		std::vector<PendingSharedDirectPublish> mPendingSharedDirectPublishes;

		//! T4: immutable deep copy of the most recent successful full
		//! production/agent render.  Guarded by mMutex, independent of the
		//! interactive FrameStores, retained until replacement/teardown.
		IRasterImage*               mLastRenderImage = nullptr;

		//! Deferred Last Render sink delivery (recorded under mMutex, drained outside all controller locks).
		struct PendingLastRenderPublish
		{
			IRasterizerOutput* sink  = nullptr;   //!< owning addref
			IRasterImage*      image = nullptr;   //!< owning addref (the retained frame or a placeholder)
			unsigned int       pane  = 0;
			unsigned long long sourceGeneration = 0;
			unsigned long long publishGeneration = 0;
		};
		//! Incremented per content-source transition to discard stale deferred Last Render batches.
		std::atomic<unsigned long long> mPaneContentGeneration[kViewportPaneCount] {};
		//! Per-pane latest queued image generation (invalidates superseded detached batches).
		std::atomic<unsigned long long> mPaneLastRenderPublishGeneration[kViewportPaneCount] {};
		//! Leaf mutex guarding pending/retired Last Render queues (lock order: mMutex -> this).
		mutable std::mutex          mLastRenderPublishMutex;
		std::vector<PendingLastRenderPublish> mPendingLastRenderPublishes;
		//! Superseded/evicted records whose owning refs must be released only
		//! after controller and queue locks are dropped.  Normal setter/render
		//! tails drain this immediately; teardown drains any residual entries.
		std::vector<PendingLastRenderPublish> mRetiredLastRenderPublishes;
		//! Global recursive delivery barrier serializing arbitrary sink callbacks (prevents AB/BA).
		std::recursive_mutex        mLastRenderDeliveryMutex;

		//! Tracks active Last Render drains so teardown quiesces callbacks before destruction.
		mutable std::mutex          mLastRenderDrainStateMutex;
		std::condition_variable     mLastRenderDrainDoneCV;
		unsigned int                mLastRenderDrainCalls = 0;
		bool                        mLastRenderDrainsStopping = false;
		bool         BeginLastRenderDrain_();
		void         EndLastRenderDrain_();
		void         QuiesceLastRenderDrains_();

		//! Queue coalesced per-pane Last Render delivery (requires `mMutex` held).
		void         QueueLastRenderPublishLocked_( unsigned int pane );
		//! Deliver queued Last Render frames and release retired refs. MUST be called with NO
		//! controller lock held (including `mRenderAdmissionMutex`).
		void         DrainLastRenderPublishes_( bool blocking = true );

		//! Working registers for current pane surface dims (`0` = film rest dims).
		unsigned int                mCurrentPaneSurfaceW = 0;
		unsigned int                mCurrentPaneSurfaceH = 0;

		//! Target pane of active pointer gesture (written under parked `mMutex`).
		unsigned int                mGesturePane = 0;
		//! One-shot flag preserving `mGesturePane` across OnPanePointerDown -> OnPointerDown.
		bool                        mGesturePaneArmed = false;

		//! Apply camera-motion op to current pane's override camera (parks internally).
		bool ApplyPaneFlyOp_( const SceneEdit& edit );

		//! Scheduler helpers -- ALL require mMutex held (and the render
		//! parked where they touch registers); see each impl's doc.
		void         SwitchToPaneLocked_( unsigned int pane );
		//! Build BeautyVariant rasterizer for `mode` (requires `mMutex` held).
		bool         BuildVariantRasterizer_( Implementation::ViewportRenderMode mode,
		                                      IRasterizer** out );
		//! Preempt BeautyVariant pass for interaction under `mMutex` (pins live quality, suppresses OIDN, parks).
		void         PreemptVariantForInteractionLocked_(
			unsigned int pane, std::unique_lock<std::mutex>& lk );
		unsigned int PickNextVisiblePaneLocked_() const;
		bool         AnyVisiblePaneHasWorkLocked_() const;
		void         MarkAllVisiblePanesDirtyLocked_();
		bool         IsInteractivePaneLocked_( unsigned int pane ) const;
		Implementation::ViewportRenderMode PaneModeLocked_( unsigned int pane ) const;
		bool         PanesShareTransportViewLocked_( unsigned int a, unsigned int b ) const;
		bool         PaneBaseDimensionsLocked_( unsigned int pane,
			unsigned int& width, unsigned int& height ) const;
		void         MarkPaneDirtyLocked_( unsigned int pane );
		void         DrainSharedDirectPublishes_();
		bool         CaptureLastRenderImageLocked_( const IRasterImage& image );
		bool         AdoptLastRenderImageLocked_( IRasterImage*& image );
		//! Mark panes (including hidden) bound to camera `name` for reconcile (requires `mMutex`).
		void         InvalidatePanesBoundToSceneCameraLocked_( const String& name );
		//! Conservatively reconcile all SceneCameraNamed panes after Undo/Redo (requires `mMutex`).
		void         InvalidateAllSceneCameraNamedPanesLocked_();
		//! Mark panes bound to named view `name` for reconcile (takes `mMutex`; caller must NOT hold `mNamedViewsMutex`).
		void         InvalidatePanesBoundToNamedView_( const String& name );

		//! Effective camera for picking/gizmos (active override camera if set, else scene camera).
		const ICamera* EffectiveViewportCamera_( const IScene* scene ) const;

		//! Camera-dimension fallback for callers already holding `mMutex`.
		bool GetCameraDimensionsLocked_( unsigned int& w, unsigned int& h ) const;

		//! Jump-to-definition shared body (see PropertyJumpTarget) --
		//! resolves one row's Reference value against the live managers.
		bool ResolveRowJumpTarget_( const CameraProperty& row, Category& outCat, String& outName ) const;

		//! Locked mutation bodies (public wrappers call these then drain dirty notification unlocked).
		bool SetPropertyInner_( Category targetCategory, const String& targetName,
		                       const String& name, const String& valueStr );
		//! Locked capture/restore helpers (caller holds `mMutex` and drains dirty afterward).
		EditorStateSnapshot CaptureEditorStateLocked_() const;
		void RestoreEditorStateLocked_( const EditorStateSnapshot& s, bool restoreDirty );
		bool SetSelectionInner_( Category cat, const String& entityName );
		bool UndoInner_();
		bool RedoInner_();
		//! Inner param edit with repeatable `occ`/`occAddressed` (caller bounds `occ`).
		AgentCommitResult ApplyAgentParamEditInner_(
			const String& entityName, const String& entityKind, const String& param,
			const String& value, const RISE::Cst::CstHeadVersion* baseVersionOrNull,
			int occ = 0, bool occAddressed = false );

		//! Unlocked enumeration bodies (require `mMutex` held or single-threaded context).
		unsigned int CategoryEntityCountLocked_( Category cat ) const;
		String       CategoryEntityNameLocked_( Category cat, unsigned int idx ) const;
		String       CategoryActiveNameLocked_( Category cat ) const;

		//! O(N) bulk entity-name enumeration for `cat` (requires `mMutex` held).
		void CategoryEntityNamesLocked_( Category cat, std::vector<String>& out ) const;

		//! Registration serial of `(cat, name)` (0 for non-IManager categories and Painter union; requires `mMutex`).
		unsigned long long CategoryEntitySerialLocked_( Category cat, const String& name ) const;

		//! Non-blocking try_lock snapshot refresh (serves stale snapshot on contention/render-owns-scene).
		void RefreshEnumSnapshot_( Category cat ) const;

		//! Non-blocking try_lock tree refresh; compares before publishing so unchanged trees keep generation.
		void RefreshTreeSnapshot_( Category cat ) const;

		//! Build `cat`'s tree from the live managers.  REQUIRES mMutex held.
		AuthoredTree BuildCategoryTreeLocked_( Category cat ) const;

		//! Fold synthesized instancing entries into authoring chunks via `IObjectManager::GetObjectProvenance`
		//! (never parse name syntax; requires `mMutex`).
		void BuildObjectTreeSeedsLocked_( std::vector<TreeNodeSeed>& outSeeds ) const;

		//! Non-blocking compare-then-publish refresh for `mUi.painterMaterialGraph`.
		bool RefreshPainterMaterialGraphSnapshot_() const;

		//! Single-scan (`AllChunks` + `EdgesAndDangling`) DAG seed gathering under `mMutex`.
		void BuildPainterMaterialGraphSeedsLocked_(
			std::vector<GraphNodeSeed>& outNodes, std::vector<GraphEdgeSeed>& outEdges ) const;

		//! Non-blocking compare-then-publish refresh for `mUi.objectGraph`.
		bool RefreshObjectGraphSnapshot_() const;

		//! Single-scan Object Graph seed gathering under `mMutex`.
		void BuildObjectGraphSeedsLocked_(
			std::vector<GraphNodeSeed>& outNodes, std::vector<GraphEdgeSeed>& outEdges ) const;

		//! Pure 4-component Object Graph focus closure (separate UP/DOWN worklists + GEO + 1-hop SOURCE).
		static std::vector<unsigned int> ObjectGraphFocusedClosure(
			const SceneGraphModel& g, unsigned int startIdx );

		//! Read `mJob.GetCstLoadFileIdentity().filePath` (clears `outPath` first).
		//! `blocking=false` try-locks (poll path); `blocking=true` waits for `mMutex` (refuses if render owns scene).
		bool GetCurrentScenePath_( std::string& outPath, bool blocking = false ) const;

		//! Check published tree membership under `mUiSnapshotMutex` without refreshing.
		bool TreeContainsName_( Category cat, const std::string& name ) const;

		//! Unified UI read snapshot served under leaf `mUiSnapshotMutex` (ADR rule 4; no ad-hoc live reads).
		struct EditorUiSnapshot
		{
			std::vector<CameraProperty> properties;                          // primary (panel-mode routed)
			std::vector<CameraProperty> propertiesByCategory[kNumCategories];
			std::vector<String>         entityNames[kNumCategories];
			String                      activeNames[kNumCategories];
			AuthoredTree                trees[kNumCategories];
			PainterMaterialGraph        painterMaterialGraph;
			SceneGraphModel             objectGraph;
		};
		mutable std::mutex        mUiSnapshotMutex;   // leaf: never held while acquiring any other lock
		mutable EditorUiSnapshot  mUi;

		//! Reject GUI Reference edit iff `value` names a runtime-only entity with no CST chunk.
		bool WouldPersistDanglingReference_( Category cat, const String& entityName,
			const String& paramName, const String& value );

		//! Effective camera of `pane` (reads slot for non-current panes; requires `mMutex`).
		const ICamera* PaneEffectiveCameraLocked_( unsigned int pane, const IScene* scene ) const;

		//! Gizmo handle computation requiring `mMutex` AND `mCameraFrameMutex` already held.
		void RefreshGizmoHandlesLocked_();

		// Transactional-rollback state (layout-additive tail; inverse-edit based, seq-marker trim-immune).
		// Atomic `mTxnOpen` allows race-free checks; transitions serialized under `mMutex`.
		std::atomic<bool>                    mTxnOpen;
		EditorStateSnapshot                  mTxnBaseline;        // H1: one owned baseline (history marker + dirty + selection)

		// Simulation state (lock order: mLiveOpMutex -> mLiveMutex -> mRenderAdmissionMutex -> mMutex).
		std::atomic<std::uint64_t>           mCompletedPasses{ 0 };
		std::mutex                           mLiveOpMutex;        // serializes attach / detach / restart
		mutable std::mutex                   mLiveMutex;          // guards mLiveSims
		std::vector<std::unique_ptr<Implementation::LiveSimulation> > mLiveSims;
		std::unique_ptr<Implementation::PlaybackClock> mPlayback;
		double ClampToLiveFrames_( double t ) const;	// caller holds mLiveMutex
		bool ShowTimeNow_( double t );
		bool TimelineRangeLocked_( double& start, double& end, unsigned int& frames ) const;
		bool RebindParked_( Implementation::LiveSimulation& live,
			std::unique_ptr<Simulation::MeshCacheSource>& source, double installTime,
			bool force, std::string& error );
		//! The transform twin of RebindParked_ (a live run with no mesh).
		bool DriveTransformsParked_( Implementation::LiveSimulation& live, bool attach, double installTime,
			bool force, std::string& error );
		bool RestartLive_( const std::string& geometry, const std::vector<unsigned char>* parameters,
			const std::string* record, std::string& error );
		// M4: a scene simulation's live run, built off the scene locks from what
		// is captured under mMutex; and the shared tail of every attach.
		bool PrepareSceneLive_( const std::string& simulation,
			std::unique_ptr<Implementation::LiveSimulation>& live, std::string& error );
		bool AttachPrepared_( std::unique_ptr<Implementation::LiveSimulation> live, std::string& error );
		void ShutdownSimulation_();
		// M5 (appended): the bake manager's frames generation last refreshed.
		std::mutex                           mBakeRefreshMutex;
		std::uint64_t                        mBakeFramesSeen = 0;
		mutable std::mutex                   mBakeKeyMutex;
		mutable std::string                  mBakeSceneKey, mBakeAttachedKey;

		// Disable copy / move
		SceneEditController( const SceneEditController& );
		SceneEditController& operator=( const SceneEditController& );
	};
}

#endif
