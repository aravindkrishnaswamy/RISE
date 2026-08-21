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
#include "ReferenceGraph.h"                 // doc-88 Phase 3 S11 round 2 P2-b: ReferenceEdge, for the public ExpandFunctionPromotionFrontier helper's signature
#include "ConnectionLegality.h"             // doc-88 Phase 3 S17: ConnectionVerdict, for CheckConnection/WouldCycle below
#include "OwnershipClosure.h"               // doc-88 Phase 3 S19: ClosureClassification, for RewireConnection's RewireResult below
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
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace RISE
{
	//! Desktop viewport sinks intentionally display ordinary render output as
	//! opaque, even when a no-hit pixel carries coverage alpha 0.  A negative
	//! alpha is reserved solely for the Last Render "no completed image yet"
	//! clear sentinel; shells map that sentinel to transparent and every real
	//! render pixel to opaque.  Keeping the rule here gives Mac, Windows, and
	//! the regression test one source of truth.
	inline unsigned char ViewportShellDisplayAlpha8( const RISEColor& c )
	{
		return c.a < 0.0 ? 0 : 255;
	}

	namespace Implementation { class InteractivePelRasterizer; }
	namespace Implementation { class FrameStore; }
	class IRasterImage;
	//! GUI render modes P1 (docs/gui/RENDER_MODES.md §5).  Opaque-enum
	//! forward declaration (implicit `int` underlying type, matching the
	//! full definition in InteractivePelRasterizer.h) -- gives this header
	//! a complete-enough type for the `mViewportRenderMode` data member
	//! below without pulling in the full interactive-rasterizer header.
	namespace Implementation { enum class ViewportRenderMode; }

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

		//! Test/debug hook: apply the WORLD-space translate op the gizmo's
		//! translate drag produces, to the current Object selection.  Exists so
		//! the world-delta path -- which under 87 conjugates the op into the
		//! object's PARENT frame -- can be regression-tested without
		//! synthesising a pointer gesture and a camera projection.  Returns
		//! false if no Object is selected or the edit was refused.
		bool ForTest_TranslateSelectedObjectWorld( double dx, double dy, double dz );

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

		//! -------- Navigation axis-ball gizmo (Tier 2 / Direction B §4) --------
		//!
		//! Six nubs (+X/−X/+Y/−Y/+Z/−Z) laid out around a ball the platform
		//! draws in a viewport corner; clicking a nub snaps the VIEW down that
		//! world axis (SnapViewToAxis, non-destructive).  Mirrors the object
		//! gizmo's shared-math / thin-platform-draw split: C++ projects the
		//! world axes through the CURRENT interactive camera (the free-fly
		//! viewport pose when active, else the scene's active camera), places
		//! the nubs, and hit-tests; the platform only draws + routes the click.
		//! See docs/gui/CAMERAS_AND_VIEWS.md §4.
		struct NavGizmoNub
		{
			int    axis         = 0;      ///< 0=X, 1=Y, 2=Z (C-API surface)
			bool   negative     = false;  ///< false=+axis, true=−axis
			double screenX      = 0.0;    ///< nub center, in the caller's ball-geometry space
			double screenY      = 0.0;
			double screenRadius = 0.0;    ///< hit-test / draw radius (== nubRadius arg)
			bool   facing       = true;   ///< true=toward viewer (bright), false=away (dim)
		};

		//! Recompute the six nubs for a ball centered at (centerX, centerY)
		//! with `ballRadius`, each nub `nubRadius`, all in the caller's widget
		//! space (whatever consistent space it also feeds NavGizmoHitTest).
		//! Sets the count to 0 (nothing to draw) when there is no supported
		//! (pinhole) interactive camera or the geometry args are non-positive.
		//! Reads camera state only — no scene mutation, zero render cost.
		bool RefreshNavGizmo( double centerX, double centerY,
		                      double ballRadius, double nubRadius );

		unsigned int NavGizmoNubCount() const;   ///< 0 or 6
		//! Read nub `idx`; false (outputs untouched) when out of range.
		bool NavGizmoNubInfo( unsigned int idx, int& outAxis, bool& outNegative,
		                      double& outScreenX, double& outScreenY,
		                      double& outScreenRadius, bool& outFacing ) const;

		//! Hit-test a pointer position (same space as RefreshNavGizmo's ball
		//! geometry) against the nubs.  Front-facing nubs win ties (they draw
		//! on top of the ones pointing away).  Returns the nub index or -1.
		int NavGizmoNubAt( double px, double py ) const;

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
			SceneVariant = 9, ///< scene_variant overlays; picking one RE-DERIVES the scene with that variant active.
			Painter    = 10,  ///< Painters section (union of the IPainter + IScalarPainter
			                  ///< managers).  CurrentPanelMode returns PanelMode::None for
			                  ///< this category this slice (no dedicated PanelMode value) —
			                  ///< property rows are read via PropertyCountFor/PropertyNameFor
			                  ///< (indexed directly by Category, not by the current panel).
			Geometry   = 11   ///< Geometry section (IGeometryManager; every "*_geometry"
			                  ///< chunk).  GUI redesign 2026-07-22: enumerated via
			                  ///< IJob::EnumerateGeometryNames; property rows are the
			                  ///< generic descriptor+CST surface (CstIntrospection); edits
			                  ///< route through ApplyAgentParamEdit (entityKind
			                  ///< "geometry").  Same PanelMode::None convention as Painter.
		};

		//! Category array bound (None..Geometry).  PUBLIC so free helpers
		//! (PropsForCat) and shells never mirror it as a stale literal.
		static constexpr int kNumCategories = 12;

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
		//!                   under RunPreviewRenderParked, OR submitted via
		//!                   SubmitAgentRenderAsync/Sync (today: the Agent
		//!                   surface's transient film/camera-override
		//!                   render AND its plain no-override render).
		//!   Production   -- Model-B F2 slice S4: a platform-shell
		//!                   "Render" action (the GUI's full-quality,
		//!                   possibly-multi-minute render), submitted via
		//!                   SubmitProductionRenderSync.  Routed through the
		//!                   SAME single-slot worker as AgentPreview so a
		//!                   production render, an agent render, and the
		//!                   interactive loop can never occupy Rasterize()
		//!                   at the same time.
		enum class RenderClass
		{
			Interactive  = 0,
			AgentPreview = 1,
			Production   = 2
		};

		//! Fix-round-8 P1: WHY a coordinated-render entry point refused.
		//! Reported through the optional `outRefusal` out-param of
		//! RunPreviewRenderParked, SubmitAgentRenderAsync and
		//! SubmitAgentRenderSync.
		//!
		//! Those methods refuse at many distinct gates (RunPreviewRenderParked
		//! at seven) and used to return a bare `false`, so the caller had to
		//! reverse-engineer the cause.  AgentSession did that by reading
		//! CurrentRenderJob() -- which is NOT a proxy for any gate here:
		//! RenderLoop mints an `active == true` RenderClass::Interactive job
		//! for EVERY ordinary viewport pass, so in the normal steady state
		//! (viewport drawing) an mTxnOpen refusal was reported to the model
		//! as "render queued or in progress", pointing it at something that
		//! completes constantly while the transaction that actually blocks
		//! stays open.  The callee already knows which gate fired; it now
		//! says so, which closes the whole class (races,
		//! gate-claimed-but-not-yet-minted windows, future causes) rather
		//! than narrowing one case.
		//!
		//! ROUND-10: renamed from `RenderRefusal`.  The submit paths
		//! now report through this same enum (round-10 finding 2b: they were
		//! INFERRING their refusal cause from CurrentRenderJob().pinned, the
		//! exact defect round 8 removed from RunPreviewRenderParked's
		//! callers), and a Production-class submission refusal is not a
		//! "preview" refusal.  Two enumerators were added at the same time:
		//! PinnedRenderBusy (submit paths only) and
		//! InteractionFinalizeLatched (round-10 finding 3).
		//!
		//! RETRIABILITY is part of each enumerator's contract and is stated
		//! per-value below.  It is load-bearing: these values become
		//! model-facing strings, and telling a model to retry something that
		//! can never succeed is the infinite-retry failure this branch
		//! exists to remove.
		enum class RenderRefusal
		{
			//! No refusal.  The value left in `*outRefusal` whenever the
			//! call returns true, and also on the throw path (an exception
			//! out of `fn` propagates past the refusal sites entirely).
			None = 0,

			//! Terminal Stop()/teardown is in progress -- mDirectRenderStopping
			//! (registration closed) or mAgentRenderStop (controller stopped).
			//! NOT retriable: the controller is going away.
			ControllerStopped,

			//! An editor transaction, pointer gesture, time scrub, save, or a
			//! direct SceneEditor composite is open (mTxnOpen / mSaving /
			//! mPointerDown / mScrubInProgress / IsCompositeOpen).
			//! Retriable once the gesture completes.
			//! Round-10 P2: RunPreviewRenderParked reports this from THREE
			//! sites, not the two the previous wording claimed -- the
			//! pre-admission mTxnOpen check, the post-admission
			//! txn/save/composite check, and the post-park revalidation.
			//! The submit paths report it from four sites BETWEEN them, but
			//! NOT symmetrically (round-12 P2 -- the previous wording credited
			//! both with a pre-flight check): only SubmitAgentRenderAsync has
			//! its own pre-flight mTxnOpen check; SubmitAgentRenderSync goes
			//! straight to the fairness ticket and can only reach EditorBusy
			//! through the three nested mTxnOpen / mSaving / IsCompositeOpen
			//! checks in the shared SubmitAgentRenderAsync_Locked.
			EditorBusy,

			//! Another coordinated (agent/production) render owns the
			//! admission gate -- queued or running
			//! (mAgentRenderBlocksInteractive), or, from the submit paths,
			//! the single agent-render slot is already occupied by a
			//! NON-pinned job / a fair-queue waiter owns the next turn / the
			//! synchronous fairness wait timed out still waiting for it.
			//! Retriable once that render completes.
			//! NOTE this gate is claimed BEFORE the job record is minted, by
			//! both SubmitAgentRenderAsync_Locked and RunPreviewRenderParked,
			//! so a CurrentRenderJob()-based reconstruction cannot see it
			//! during that window -- another reason the cause is reported
			//! directly.
			CoordinatedRenderBusy,

			//! An open platform interaction could not be finalized
			//! (FinalizeOpenInteractions returned false) for a TRANSIENT
			//! reason: a pointer gesture / property scrub / timeline scrub /
			//! controller-opened composite was still open when the finalize
			//! attempt ended, or the admission gate was taken underneath it.
			//! Retriable -- the next attempt can succeed.
			InteractionFinalizeFailed,

			//! ROUND-10 finding 3.  FinalizeOpenInteractions returned false
			//! because mInteractionPersistenceFailed is LATCHED -- a pending
			//! CST object-transform / camera-pose commit failed (a CST route
			//! failed, or a dragged camera/object vanished from the scene
			//! mid-gesture).  That flag is a deliberate STICKY truth flag: it
			//! is set at five production sites (plus the
			//! ForTest_TripInteractionPersistenceFailure seam) and NEVER
			//! cleared, because once a live
			//! gesture's delta failed to reach the Document the controller can
			//! no longer claim the Document matches what the user did.
			//!
			//! **NOT retriable, and it will NOT clear on its own.**  Every
			//! subsequent preview render and every read_viewport refuses for
			//! the life of the controller.  This value exists precisely so
			//! that fact is REPORTED rather than dressed up as the retriable
			//! InteractionFinalizeFailed above -- reporting it as retriable is
			//! what produced the infinite-retry loop this branch removes.
			//! The discrimination is exact, not heuristic: whenever the flag
			//! is set, FinalizeOpenInteractions returns false unconditionally,
			//! so "flag set at the refusal site" == "this refusal is
			//! permanent" and "flag clear" == "one of the transient causes".
			InteractionFinalizeLatched,

			//! ROUND-10 finding 2b.  SUBMIT PATHS ONLY (never produced by
			//! RunPreviewRenderParked, which has no slot concept): the single
			//! agent-render slot is occupied by a PINNED job, which refuses
			//! every new submission rather than being superseded.  Retriable
			//! once that job completes.  Previously AgentSession INFERRED
			//! this by reading CurrentRenderJob().pinned after the refusal --
			//! a stale-field read that claimed a pinned render was in flight
			//! for the rest of the session once any pinned render had ever
			//! completed.
			PinnedRenderBusy
		};

		//! Snapshot of the CURRENT render job, read under mJobStatusMutex
		//! (NOT mMutex -- see CurrentRenderJob()'s doc for why that
		//! distinction is load-bearing).  `active` is false when no render
		//! is presently in flight; `id`/`renderClass` then reflect the MOST
		//! RECENTLY assigned job (stale, informational only).  `active ==
		//! true` covers EVERY render class, interactive passes included --
		//! it is not an agent-render-gate indicator.
		//!
		//! Fix-round-1 P3-c: `clientLabel` echoes the diagnostic tag a
		//! caller passed to SubmitAgentRenderAsync / SubmitAgentRenderSync
		//! (empty for an Interactive-class job, which has no client label
		//! concept).  Was write-only dead state (mAgentRenderClientLabel
		//! was recorded but never read by anything) -- surfaced here so a
		//! Status() consumer can tell WHICH agent submitted an in-flight
		//! render, useful for S2b's gate predicate and for diagnosing which
		//! caller is occupying the single slot.
		//! Model-B F2 slice S3 ADDITIVE field: `pinned` echoes whether the
		//! CURRENT agent-slot occupant was submitted as a PINNED render
		//! (see SubmitAgentRenderAsync / SubmitAgentRenderSync's `pinned`
		//! parameter).  Always false for an Interactive-class job (the
		//! interactive loop has no pinned concept) and for a
		//! RunPreviewRenderParked job (that path takes no `pinned`
		//! argument); meaningful for a submitted job for as long as
		//! `active` is true (stale once the job completes, same
		//! "informational only" caveat as `id`/`renderClass` above).
		//!
		//! ROUND-10 finding 2a: `pinned` used to be written at ONE of the
		//! three mint sites (SubmitAgentRenderAsync_Locked).  The other
		//! two -- RunPreviewRenderParked and RenderLoop's per-pass
		//! Interactive mint -- overwrote id/renderClass/active/clientLabel
		//! and left `pinned` STALE, so once ANY pinned render had
		//! completed the field read `true` for the rest of the session.
		//! All three sites now publish a WHOLE, freshly default-
		//! constructed RenderJobStatus, so a record always fully describes
		//! ITS OWN job and a field added later cannot repeat this.
		//! Regression-pinned by AgentRenderAsyncTest's
		//! RunPinnedFieldNotStaleAfterCompletionTest.
		//!
		//! DO NOT use this field to explain WHY a submission was refused.
		//! That was the round-10 bug: the refusal messages read it and
		//! announced "a pinned render is in flight" when none was.  The
		//! submit paths now report RenderRefusal::PinnedRenderBusy through
		//! their own `outRefusal` out-param -- decided by the slot state
		//! under the slot lock at the moment of refusal, which no
		//! after-the-fact status read can reconstruct.
		struct RenderJobStatus
		{
			RenderJobId  id          = kInvalidRenderJobId;
			RenderClass  renderClass = RenderClass::Interactive;
			bool         active      = false;
			String       clientLabel;
			bool         pinned      = false;
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
		//! dangle. Callable from any thread: the transaction-open gate is
		//! rechecked under mMutex before the path cancel-and-parks, and the
		//! whole commit + rebind +
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
		//! client may resubmit verbatim).
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

		//! R1a (2026-08-09, batched remove_chunks): route an ATOMIC agent BATCH chunk remove through the SAME
		//! critical section as ApplyAgentRemoveChunk (mTxnOpen refusal -> cancel-and-park under mMutex ->
		//! conflict gate -> the Job primitive -> rebind -> MarkCstHeadDirty -> ONE history push -> re-render
		//! kick -> post-commit head read).  `targets[i]` / `kinds[i]` (kinds may be shorter, or carry empty
		//! entries, meaning "no kind narrowing") are resolved with the SAME rules the singular verb uses.
		//!
		//! ALL-OR-NOTHING and ONE head bump: Job::ApplyCstRemoveChunks resolves every target against the
		//! unmutated Document, refuses the WHOLE batch if any target fails, and otherwise erases them all in
		//! ONE Document mutation realized by ONE dry-run-guarded re-derive.  Intra-batch references therefore
		//! resolve regardless of the order the caller listed them in, and a target still referenced from
		//! OUTSIDE the batch refuses the batch as a whole with the head byte-identical.
		//!
		//! HISTORY: exactly ONE EditHistory record (SceneEdit::AgentRemoveChunks) is pushed, so a single Cmd-Z
		//! restores every removed chunk -- not N records the user has to undo one at a time.  The record's
		//! Undo payload is the byte-exact pre-batch Document text captured under this same lock hold.
		//!
		//! The result's `chunkName` is the comma-joined target list and `chunkKeyword` is the '\n'-joined
		//! resolved keyword list (one entry per INPUT target, input order) -- the batch analogue of the
		//! singular verb's single-chunk echo.
		AgentCommitResult ApplyAgentRemoveChunks(
			const std::vector<String>& targets,
			const std::vector<String>& kinds,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! R2 (2026-08-10, replace_geometry_scaffold): install `candidateDocText` -- a WHOLE candidate
		//! document the caller computed off a head snapshot -- through the SAME critical section as
		//! ApplyAgentRemoveChunks (mTxnOpen refusal -> cancel-and-park under mMutex -> conflict gate -> the
		//! Job primitive -> rebind -> MarkCstHeadDirty -> ONE history push -> re-render kick -> post-commit
		//! head read).  `objectName` is the `standard_object` whose `geometry` slot the candidate rebinds --
		//! used for the history record, the dirty mark, and the result echo, never for resolution (the
		//! candidate text already IS the resolved outcome).
		//!
		//! ATOMIC and ONE head bump: Job::ApplyCstReplaceDocumentText re-parses the text and hands it to the
		//! SAME dry-run-guarded re-derive every chunk-CRUD verb ends in, so a candidate that would not derive
		//! leaves Document + live scene byte-identical.
		//!
		//! `baseVersionOrNull` is NOT optional in practice for this verb even though the parameter is
		//! nullable: the candidate was computed OUTSIDE this lock, so committing it without proving the head
		//! has not moved would silently CLOBBER a concurrent co-editor's edit (a whole-document swap
		//! overwrites everything, unlike a targeted param edit).  AgentSession::ReplaceGeometryScaffold
		//! therefore always passes the head version its own snapshot read, and the conflict gate below is
		//! what makes the snapshot-then-commit sequence safe.  A null base is still honoured (no gate) for
		//! the benefit of a direct in-process caller that has externally serialized itself.
		//!
		//! HISTORY: exactly ONE EditHistory record (SceneEdit::AgentReplaceGeometry) is pushed, carrying the
		//! byte-exact PRE text (captured under this same lock hold) and the byte-exact POST text, so one
		//! Cmd-Z restores the pre-call document and one Cmd-Shift-Z reinstalls the post-call one.
		//! `verbLabel` is the DIAGNOSTIC CONTEXT string handed to
		//! Job::ApplyCstReplaceDocumentText -- it names, in the log, which
		//! agent verb composed the candidate.  Defaulted so every pre-existing
		//! call site and its wording are untouched; 88 step 2 passes
		//! "collapse_to_instances", the second verb whose commit is one
		//! whole-document swap (see AgentSession::CollapseToInstances).  It
		//! does NOT change the result messages -- those stay this verb's, and
		//! a caller with different wording overwrites them in its own result,
		//! which is exactly what CollapseToInstances does.
		AgentCommitResult ApplyAgentReplaceGeometry(
			const String& objectName,
			const String& candidateDocText,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull,
			const char* verbLabel = "replace_geometry_scaffold" );

		//! Secure-MCP slice 5a: which verb-kind a staged AgentProposal replays
		//! on approval.  Mirrors the three existing agent commit entry points
		//! 1:1 -- there is no fourth kind because those are the only three
		//! mutating verbs the Agent surface exposes.
		enum class AgentProposalKind
		{
			ParamEdit    = 0,   //!< replays via ApplyAgentParamEdit (entity/kind/param/value)
			InsertChunk  = 1,   //!< replays via ApplyAgentInsertChunk (chunkText)
			RemoveChunk  = 2,   //!< replays via ApplyAgentRemoveChunk (target/kind)
			//! R1a (2026-08-09): replays via ApplyAgentRemoveChunks -- ONE proposal for a WHOLE batch remove,
			//! so an Owner approves (or rejects) the batch as the single atomic edit it is, rather than N
			//! separate cards that could be partially approved into a state the agent never asked for.
			RemoveChunks = 3
		};

		//! Secure-MCP slice 5a: ONE staged (inert) proposal from an
		//! External-authority AgentSession.  Carries EXACTLY the args the
		//! corresponding ApplyAgent* verb needs to replay the edit unchanged
		//! at approval time, plus the bookkeeping ResolveProposal needs: the
		//! head-version the proposal was staged AGAINST (re-checked at
		//! approval -- the hard optimistic-concurrency invariant, see
		//! ResolveProposal's doc) and a human-readable label naming which
		//! session staged it (diagnostic only; not used for any gating
		//! decision here -- the Owner-only-may-resolve gate is enforced by
		//! AgentSession, which knows its own authority, not by the
		//! controller, which does not track per-proposal ownership).
		//! Status as of slice 5c: `sessionLabel` is plumbed end-to-end (this
		//! struct, StageProposal, AgentSession::AgentProposalEntry) and is
		//! now POPULATED whenever the staging AgentSession had
		//! SetSessionLabel() called on it -- the GUI-hosted loopback
		//! transport labels its External session at server-start time (e.g.
		//! "external-http"), so a proposal staged by a real remote MCP
		//! client carries a human-readable "who proposed this" string here.
		//! Every pre-5c construction site (headless CLI transports,
		//! LoadFromFile/WrapJob callers that never call SetSessionLabel)
		//! still stages "" -- unchanged.
		struct AgentProposal
		{
			std::uint64_t       id = 0;         //!< monotonic, unique within this controller's lifetime; never 0 (0 = "not found" sentinel)
			AgentProposalKind   kind = AgentProposalKind::ParamEdit;
			//! ParamEdit fields (kind==ParamEdit only).
			String              target;
			String              entityKind;
			String              param;
			String              value;
			//! InsertChunk fields (kind==InsertChunk only).
			String              chunkText;
			//! RemoveChunk fields (kind==RemoveChunk only); `target`/`entityKind`
			//! above double as RemoveChunk's (target,kind) -- no separate fields.
			//! R1a: RemoveChunkS (kind==RemoveChunks) reuses the SAME two carriers rather than adding
			//! batch-only members -- `target` holds the '\n'-separated TARGET NAMES and `entityKind` the
			//! '\n'-separated KINDS, one entry per target, same order, an empty entry meaning "no kind
			//! narrowing".  ResolveProposal splits them back apart; ListProposals surfaces them verbatim so a
			//! proposal card can render the N targets it will delete.
			RISE::Cst::CstHeadVersion baseVersion;   //!< the head this proposal was staged against
			//! S5a hardening: when false (the common case -- the proposing
			//! session did not pin an explicit baseHeadVersion), `baseVersion`
			//! above is IGNORED by the caller and StageProposal stamps it
			//! itself under render admission (and mMutex when no render owns
			//! the immutable scene) from the controller's OWN current head --
			//! this is what closes the unlocked-read race (see StageProposal's
			//! doc).  When true, the caller already supplied a real
			//! caller-pinned baseVersion (an explicit baseHeadVersion argument
			//! to one of the 6 mutating verbs -- propose_patch/propose_patches/
			//! insert_chunk/insert_chunks/remove_chunk/remove_chunks) and
			//! StageProposal passes it through untouched.
			bool                hasExplicitBaseVersion = false;
			//! G2 fix-round (2026-08-10): was the staging session's PART-PLAN
			//! GATE still ARMED at stage time (no plan filed, not given up, not
			//! disabled)?  Carried on the proposal because ResolveProposal has
			//! no handle on the AgentSession that staged it and the gate's state
			//! is session-scoped -- unlike E1's and R1c's re-checks, which are
			//! stateless policy the controller can evaluate on its own.
			//!
			//! Used for ONE narrow re-check: a ParamEdit staged while the gate
			//! was armed was, by construction, NOT geometry-introducing against
			//! the head AS IT STOOD THEN (AgentSession::ProposePatch's G2 arm
			//! refuses those before they can stage).  But a param edit's effect
			//! is head-dependent -- a target that did not resolve at stage time
			//! can resolve later -- so the same value can become
			//! geometry-introducing while the proposal sits on the queue.
			//! ResolveProposal therefore re-runs the stateless delta
			//! (RISE::Agent::DescribeBuildPlanGeometryDeltaForPatch) against the
			//! CURRENT head and refuses, exactly as it already does for E1 and
			//! R1c.  FALSE for every proposal staged with the gate disarmed --
			//! including every pre-G2 proposal shape and every default-
			//! constructed AgentProposal -- so the re-check simply does not run
			//! for them, which is the correct answer: no plan was ever required.
			//! The gate's disarm flags are permanent-once-set, so this can only
			//! be conservatively STALE (armed at stage, plan filed before
			//! resolve); the refusal names reissue as the remedy and the
			//! reissued patch stages cleanly.
			bool                buildPlanGateArmedAtStage = false;
			String              sessionLabel;        //!< diagnostic: which session staged it (caller-supplied via AgentSession::SetSessionLabel); "" when the staging session never set one -- see struct doc above
			String              status;              //!< "pending" / "applied" / "rejected" / "conflict"
		};

		//! Secure-MCP slice 6: the PENDING-queue depth cap StageProposal
		//! enforces -- see mProposals's doc for the full rationale. 32 is
		//! generously above any realistic legitimate backlog (a human Owner
		//! reviewing proposals one at a time; even a batch-authoring agent
		//! staging a scene's worth of edits tops out at a handful before an
		//! Owner would reasonably resolve some), so this cap exists purely
		//! as a fail-closed backstop against a runaway/hostile External
		//! client staging proposals faster than any Owner could plausibly
		//! resolve them, not as a limit a well-behaved client should expect
		//! to bump into.
		static const std::size_t kMaxPendingProposals = 32;

		//! Secure-MCP slice 6: the TOTAL storage cap (pending + resolved)
		//! StageProposal enforces -- see mProposals's doc. 8x
		//! kMaxPendingProposals: generous enough that a normal session's
		//! full resolved-audit history is very unlikely to be evicted while
		//! still relevant, while guaranteeing this vector can never grow
		//! past a fixed, small bound no matter how long the controller
		//! (and an attached external session) stays alive.
		static const std::size_t kMaxProposalHistory = 8 * kMaxPendingProposals;

		//! Secure-MCP slice 5a: STAGE one proposal (INERT -- no Document
		//! mutation, no render kick, no EditHistory record).
		//!
		//! `baseVersion` is the head this proposal is checked against at
		//! approval time (ResolveProposal).  S5a hardening round: the CALLER
		//! (AgentSession) no longer pre-reads the controller's head-version
		//! itself to populate this field -- that read raced this same
		//! controller's render thread / a concurrent commit against the
		//! non-atomic 16-byte CstHeadVersion, outside any lock.  Instead:
		//! when `proposal.hasExplicitBaseVersion` is false, StageProposal
		//! stamps `baseVersion` from `mJob.GetCstHeadVersion()` ITSELF, under
		//! render admission (plus mMutex when the render gate is clear) and
		//! enqueues under the proposal leaf mutex -- so the
		//! captured baseVersion is atomically "the head at the instant this
		//! proposal joined the queue", with no window for a torn read or a
		//! stale value.  When `hasExplicitBaseVersion` is true (the caller
		//! pinned a real baseHeadVersion argument), `proposal.baseVersion` is
		//! passed through untouched -- that value did not come from an
		//! unlocked read here; it is whatever the caller's own (separately
		//! guarded) provenance was.
		//!
		//! Returns the freshly-minted, never-0 proposal id -- OR 0 (Secure-
		//! MCP slice 6) if the PENDING queue is already at kMaxPendingProposals:
		//! a REFUSAL, not a silent drop -- nothing is enqueued, mNextProposalId
		//! is not consumed, and `outStagedVersion` (if provided) is left
		//! UNTOUCHED. The caller (AgentSession::ProposePatch/InsertChunk/
		//! RemoveChunk) maps a 0 return to a distinct queue-full result
		//! rather than treating it as a normal stage. Thread-safe without
		//! waiting behind a render-duration mMutex hold: staging touches only
		//! the proposal queue, and render admission makes the retained
		//! Document head immutable while a direct/coordinated render owns the
		//! scene. Use `outStagedVersion` to read
		//! back the exact baseVersion that was stamped/kept -- this is the
		//! value the caller should surface as the "staged" response's
		//! headVersion, again with no separate unlocked read.
		//! Returns 0 without borrowing the Job or touching the queue after
		//! lifecycle preparation has claimed the controller.
		std::uint64_t StageProposal( const AgentProposal& proposal,
		                             RISE::Cst::CstHeadVersion* outStagedVersion = nullptr );

		//! Secure-MCP slice 5a: a snapshot of every proposal currently on
		//! the queue (pending AND resolved -- resolved proposals stay
		//! visible for audit rather than being purged; see the class doc).
		//! Thread-safe under the proposal leaf mutex; never waits behind a
		//! render-duration scene lock.
		std::vector<AgentProposal> ListProposals() const;

		//! Secure-MCP slice 5a: resolve proposal `id` by APPROVING (`approve
		//! = true`) or REJECTING (`approve = false`) it.
		//!
		//! REJECT is unconditional: status -> "rejected", no mutation, no
		//! re-check.
		//!
		//! APPROVE re-checks `proposal.baseVersion` against the controller's
		//! CURRENT head-version, UNDER THE SAME mMutex hold used for the
		//! whole resolve -- THE HARD INVARIANT this slice exists for: a
		//! proposal staged against a head that has since moved (an
		//! intervening edit bumped the revision, OR the Job itself was torn
		//! down and replaced -- a fresh uuid) is marked "conflict" and is
		//! NOT applied, no matter how long it has sat on the queue.  On a
		//! version match, the proposal is replayed through the EXACT SAME
		//! ApplyAgent{ParamEdit,InsertChunk,RemoveChunk} entry point a
		//! direct (non-staged) commit would use -- so an approved proposal
		//! is indistinguishable, from EditHistory's point of view, from an
		//! Owner committing the same edit directly: it gets a normal undo
		//! record, a normal dirty mark, a normal re-render kick.
		//!
		//! Returns false (no such id, or already resolved -- resolving an
		//! already-applied/rejected/conflict proposal is refused rather
		//! than silently re-run) with the queue left untouched; true
		//! otherwise, with `outResult` filled from the replay.  The
		//! render-admission gate ALSO returns false without consulting the
		//! queue, so `false` alone does not mean "unknown id": that path
		//! fills `outResult` with a refusal whose `retriable` says whether
		//! resolving again later can work (true while a render is queued
		//! or running, false once the controller is being destroyed).  A
		//! caller that passes `outResult == nullptr` cannot tell the two
		//! apart.  A REJECT
		//! never calls ApplyAgent* at all, but still fills `outResult`
		//! with status="rejected" plus the REAL current head (Secure-MCP
		//! slice 5b fix round P2-2 -- leaving it default-constructed put
		//! the {0,0} "no head to report" sentinel on the wire).
		//!
		//! TRANSIENT-REFUSAL EXCEPTION (round-2 FIX-2 review, C5): when
		//! the approve replay comes back `retriable` -- ApplyAgent*
		//! refused on an open editor transaction/gesture BEFORE touching
		//! the Document -- the proposal is deliberately LEFT PENDING and
		//! this still returns true with `outResult` carrying that
		//! refusal.  Folding it to "rejected" would permanently burn a
		//! staged proposal because an Owner clicked Approve mid-gesture;
		//! nothing was applied and the head is byte-identical, so simply
		//! approving again a moment later is a true retry.
		//!
		//! CALLER CONTRACT (enforced ONE LAYER UP, in AgentSession, not
		//! here): the controller has no notion of "authority" -- it cannot
		//! tell an Owner-authority caller from an External one.  The
		//! Owner-only-may-resolve gate is AgentSession::ResolveProposal's
		//! job (it refuses outright for an External-authority session
		//! before ever reaching this method).  This method is therefore
		//! callable by anything holding a controller pointer; treat it as a
		//! privileged operation at the layer that DOES know who is calling.
		bool ResolveProposal( std::uint64_t id, bool approve, AgentCommitResult* outResult = nullptr );

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
		//! the controller by calling any mMutex-taking method on this
		//! object.  mMutex is a plain (non-recursive) std::mutex, so
		//! re-entering it from `fn` is an immediate self-deadlock, not a
		//! stall-and-retry.  (Fix-round-8 P1: CurrentRenderJob() used to be
		//! named here as an example of a forbidden call -- it is NOT one.
		//! It takes mJobStatusMutex, never mMutex, precisely so a status
		//! read cannot block behind a render-duration hold; calling it from
		//! `fn` is safe.  The prohibition applies to the mMutex-taking
		//! methods, not to this one.)  Refused (returns false, `fn` NOT invoked) while
		//! an editor transaction/gesture or save is open, or while another
		//! direct/coordinated render owns the admission gate. Direct renders
		//! publish that same gate before waiting for mMutex, so UI callbacks
		//! refuse promptly instead of queuing behind the render-duration hold.
		//! Returns true iff `fn` ran.
		bool RunPreviewRenderParked( const std::function<void()>& fn );

		//! Model-B F2 slice S1: SAME as RunPreviewRenderParked above
		//! (including the non-recursive-mMutex / no-reentrancy contract on
		//! `fn`), plus render-identity bookkeeping -- assigns a fresh
		//! monotonic RenderJobId, records {id, renderClass, active=true}
		//! under mJobStatusMutex after a short admission handshake and the
		//! cancel-and-park mMutex hold, runs `fn`, then
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
		//!
		//! Fix-round-8 P1: `outRefusal` (optional) receives WHY the call
		//! refused -- see RenderRefusal.  ROUND-10 CORRECTION to this
		//! paragraph: the implementation writes `None` UNCONDITIONALLY as its
		//! very first statement and then overwrites it at whichever of the
		//! seven refusal paths fires, so a caller's pre-seeded value never
		//! survives -- the earlier wording ("it is NOT written when an
		//! exception unwinds ... the value the caller initialized it with
		//! survives") was false.  The property callers actually rely on is
		//! unchanged and IS true: once this call returns or throws,
		//! `*outRefusal` holds `None` iff `fn` ran -- a throw out of `fn`
		//! unwinds past every refusal site, so that leading `None` write is
		//! the last one that happened -- and the specific cause otherwise.
		//! Initializing the local to `None` anyway is still recommended: it
		//! keeps the variable from ever being read indeterminate if this
		//! contract is weakened, and it documents the intended default.
		//! `outRefusal` exists because the caller CANNOT reconstruct the
		//! cause: CurrentRenderJob() reports the interactive loop's own
		//! per-pass job as active, and two of the gates here are claimed
		//! BEFORE any job record is minted.
		bool RunPreviewRenderParked(
			const std::function<void()>& fn,
			RenderClass                  renderClass,
			const String&                clientLabel,
			RenderJobId*                 outJobId,
			RenderRefusal*               outRefusal = nullptr );

		//! Model-B F2 slice S1: snapshot of the current render job, taken
		//! under mJobStatusMutex.  See RenderJobStatus's doc for the "stale
		//! when inactive" semantics.
		//!
		//! Fix-round-8 P1: this doc used to say "under mMutex", contradicting
		//! the implementation.  mJobStatusMutex is DELIBERATE and load-bearing
		//! (slice S2a) -- reading via mMutex would block this call for an
		//! entire in-flight render's duration, which is exactly what a status
		//! read must never do.  Consequences that follow from the real lock,
		//! and that the stale doc hid: this method is safe to call from
		//! inside a RunPreviewRenderParked / SubmitAgentRender* closure (it
		//! does NOT self-deadlock on the non-recursive mMutex those hold),
		//! and it is NOT a proxy for any admission gate -- RenderLoop mints
		//! an `active == true` Interactive job for every ordinary viewport
		//! pass, so `active` says "some render is in flight", never "the
		//! agent-render gate is claimed".  Use RunPreviewRenderParked's
		//! `outRefusal` for the latter.
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
		//! Model-B F2 slice S3 (pinned-vs-preview): `pinned` (default
		//! false = today's PREVIEW semantics, unchanged) marks this
		//! submission as a PINNED render.  With one render slot, the
		//! DELIBERATE MINIMAL policy is REJECT, NOT QUEUE:
		//!   * a PINNED job in flight (mAgentRenderPinned true for the
		//!     current occupant) causes ANY new submission -- async or
		//!     sync, pinned or not -- to be refused with a pinned-
		//!     specific message ("a pinned render is in flight"), on top
		//!     of (not instead of) the existing single-slot / Stop() /
		//!     fair-queue refusal causes.
		//!   * a PREVIEW job in flight keeps today's exact reject-if-busy
		//!     behaviour -- this parameter does not change what happens
		//!     when the occupant is a preview job.
		//!   * once accepted, a pinned job runs to completion exactly
		//!     like a preview job -- there is no "pinned jobs can't be
		//!     cancelled" protection.  Pinned protects against SILENT
		//!     SUPERSESSION by a later submission, NOT against an
		//!     explicit CancelAgentRender_ / render_cancel or a Stop()/
		//!     teardown drain -- both cancel a pinned render exactly as
		//!     they cancel a preview one (CancelAgentRender_ and Stop()
		//!     are unconditional; they do not consult mAgentRenderPinned
		//!     at all).
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
		//! Model-B F2 slice S4: `renderClass` (default AgentPreview, so
		//! every pre-existing call site is byte-for-byte unaffected) tags
		//! the slot's job-status record instead of always minting
		//! AgentPreview -- the ONLY thing a Production submission
		//! (SubmitProductionRenderSync below) does differently from an
		//! ordinary agent submission is this tag; every fair-queue /
		//! pinned / Stop() refusal rule is IDENTICAL and shared via
		//! SubmitAgentRenderAsync_Locked.
		//! ROUND-10 finding 2b: `outRefusal` (optional) receives WHY this
		//! call refused, exactly as RunPreviewRenderParked's out-param
		//! does -- see RenderRefusal.  Same write contract: it is seeded
		//! with `None` unconditionally on entry and overwritten at
		//! whichever refusal site fires, so `None` on return means
		//! "accepted".  This exists because AgentSession was inferring the
		//! cause from CurrentRenderJob().pinned AFTER the refusal -- a read
		//! that is both racy (the slot lock is released by then) and, until
		//! the round-10 whole-record fix, STALE (the field survived its
		//! job's completion), so it announced a phantom pinned render to
		//! the model on the commonest refusal path there is.  A pinned
		//! occupant now reports RenderRefusal::PinnedRenderBusy, decided
		//! under the slot lock at the moment of refusal.
		bool SubmitAgentRenderAsync(
			std::function<void()> fn,
			const String&         clientLabel,
			RenderJobId*          outJobId,
			bool                  pinned = false,
			RenderClass           renderClass = RenderClass::AgentPreview,
			RenderRefusal*        outRefusal = nullptr );

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
		//!
		//! Model-B F2 S3 fix round (P2 -- corrects a prior version of this
		//! doc): a PINNED occupant does NOT give this call a distinct
		//! "pinned refusal" outcome.  The fairness wait's predicate is
		//! `!mAgentRenderPending && myTicket == mAgentRenderServingTicket`
		//! -- i.e. this call is only ever woken to attempt the inline
		//! submit once the slot is ALREADY free -- so
		//! SubmitAgentRenderAsync_Locked's `if( mAgentRenderPinned )`
		//! branch is UNREACHABLE from this path (mAgentRenderPending is
		//! guaranteed false, under the same continuously-held
		//! mAgentRenderSlotMutex, by the time that check would run).  The
		//! coherent, live-verified semantic: a pinned occupant makes a
		//! sync caller WAIT for the fairness window (like any other
		//! occupant), not refuse it outright -- pinned guards against
		//! SILENT SUPERSESSION by a later submission stealing the slot out
		//! from under an in-flight render, not against fair queuing behind
		//! it.  A sync caller with a `timeoutMs` shorter than the pinned
		//! render's remaining duration simply times out (the ordinary
		//! fairness-wait timeout above): the OUTCOME is the same bool
		//! `false` a non-pinned occupant would produce -- pinned does not
		//! give this path a distinct refusal RULE.  ROUND-10 amendment:
		//! this paragraph used to add "there is no separate pinned-specific
		//! rejection MESSAGE on this path".  There now is one, and it is
		//! not a policy change: `outRefusal` classifies the timeout by
		//! reading the occupant's pinned-ness under the SAME slot-lock hold
		//! the wait woke under, so a caller can tell the model WHY its wait
		//! ran out.  The admission rule is unchanged (wait, do not refuse);
		//! only the honesty of the reported reason improved.  Contrast
		//! SubmitAgentRenderAsync, which REFUSES a pinned-occupied slot
		//! immediately, since it never waits.  A sync caller with a
		//! GENEROUS `timeoutMs` instead waits out the pinned render's
		//! remaining duration and then proceeds normally once the slot
		//! frees.
		//!
		//! `pinned` (default false, same semantics as
		//! SubmitAgentRenderAsync's parameter) marks THIS submission as
		//! pinned once it is accepted.
		//! Model-B F2 slice S4: `renderClass` (default AgentPreview) is
		//! the same additive tag documented on the async overload above --
		//! it changes nothing about the fairness/refusal semantics this
		//! method's own doc describes, only what CurrentRenderJob /
		//! GetRenderJobStatus report while this submission occupies the
		//! slot.
		//! ROUND-10 finding 2b: `outRefusal` (optional) receives WHY this
		//! call refused -- see RenderRefusal and the identical param on
		//! SubmitAgentRenderAsync.  On THIS path the fairness-wait timeout
		//! is also classified: the occupant's pinned-ness is read under
		//! the SAME mAgentRenderSlotMutex hold the wait just woke under,
		//! so a timeout caused by a pinned occupant reports
		//! PinnedRenderBusy and one caused by an ordinary occupant reports
		//! CoordinatedRenderBusy.  That is a genuine, non-stale read --
		//! unlike the CurrentRenderJob().pinned inference it replaces,
		//! which ran after the lock was released and (before round 10's
		//! whole-record mint fix) reported a LONG-COMPLETED pinned job.
		//! Note the S3-P2 paragraph above still holds: this path never
		//! reaches SubmitAgentRenderAsync_Locked's pinned branch, because
		//! the fairness predicate only wakes it once the slot is free.
		//! The timeout classification is where a pinned occupant becomes
		//! visible to a sync caller.
		bool SubmitAgentRenderSync(
			std::function<void()> fn,
			const String&         clientLabel,
			RenderJobId*          outJobId,
			unsigned int          timeoutMs = 30000,
			bool                  pinned = false,
			RenderClass           renderClass = RenderClass::AgentPreview,
			RenderRefusal*        outRefusal = nullptr );

		//! Model-B F2 slice S4: route a PRODUCTION render (a platform
		//! shell's "Render" / "Render Animation" / "Render Region" action)
		//! through the EXACT SAME single-slot coordinator machinery as
		//! SubmitAgentRenderSync -- fairness ticket, cancel-and-park,
		//! pinned-occupant rules, Stop()-honesty -- just tagged
		//! RenderClass::Production in the job-status record instead of
		//! AgentPreview.  This is the fix for the pre-F2-S4 gap: platform
		//! shells (macOS RISEBridge.mm's `-rasterize`, Windows
		//! RenderEngine.cpp) used to call Job::Rasterize() DIRECTLY,
		//! wholly unserialized against the interactive loop AND the
		//! agent-render worker -- two threads could be inside Rasterize()
		//! on the SAME Scene at once, and Job::SetProgress's single slot
		//! meant whichever render's teardown ran last silently owned the
		//! other's cancel/progress hook.  Routing through here closes
		//! both: with one render slot, production/agent/interactive are
		//! mutually exclusive BY CONSTRUCTION, and the ONLY progress hook
		//! ever installed on the Job while a render is in flight is this
		//! controller's own mCancelProgress (see AgentRenderProgress()) --
		//! a caller composes the platform's own progress/cancel UI as
		//! mCancelProgress's `inner` (CancellableProgressCallback::SetInner)
		//! for the duration of `fn`, so the platform UI keeps working
		//! exactly as before while the coordinator's own cancel (Stop() /
		//! CancelAgentRender_) ALSO aborts a production render, since
		//! CancellableProgressCallback::Progress refuses (returns false)
		//! the instant EITHER the outer cancel trips OR the composed
		//! inner callback returns false.
		//!
		//! `fn` runs on the SAME dedicated worker thread that runs agent
		//! renders (uniform with SubmitAgentRenderSync, NOT a caller-runs-
		//! fn-under-park shape) -- exactly one thread is ever inside
		//! Rasterize() at a time, on any render class.  This call BLOCKS
		//! the calling thread (mirroring the platform shells' existing
		//! "rasterize is synchronous, call it from a background thread"
		//! contract) until `fn` completes or the submission is refused.
		//! `queueTimeoutMs` bounds ONLY the fairness wait for a turn at the
		//! slot (same meaning as SubmitAgentRenderSync's `timeoutMs`) --
		//! once accepted, this waits out the render unconditionally,
		//! however long it takes.  Returns false (fn never invoked) on
		//! the same refusal causes as SubmitAgentRenderSync: an open
		//! editor transaction, Stop() called/in-progress, a fairness-wait
		//! timeout, or a PINNED occupant's fairness wait outlasting
		//! `queueTimeoutMs`.  A thrown exception out of `fn` propagates to
		//! the caller exactly as SubmitAgentRenderSync documents.
		//!
		//! Interactive-region state is deliberately preserved across this
		//! submission. Ordinary production callbacks render full-frame by
		//! calling Job::Rasterize / RasterizeAnimationUsingOptions and never
		//! consult that state; an explicit "Render Active Region" command
		//! captures the coordinates before submission and calls
		//! Job::RasterizeRegion from `fn`.
		bool SubmitProductionRenderSync(
			std::function<void()> fn,
			const String&         clientLabel,
			RenderJobId*          outJobId,
			unsigned int          queueTimeoutMs = 30000 );

		//! Fix-round S4-1 (throw-path UAF): the shared, tested composition a
		//! platform shell's production-render entry point (macOS
		//! RISEBridge.mm's `-rasterize`/`-rasterizeAnimation`/
		//! `-rasterizeRegion...`, Windows RenderEngine.cpp's
		//! startRender/startAnimationRender) uses to route `doRasterize`
		//! through `controller`'s SubmitProductionRenderSync -- see that
		//! method's doc for the coordinator rationale.  Pulled out of the two
		//! platform files (which had it duplicated byte-for-byte apart from
		//! BOOL vs bool) into ONE place so the throw-path fix below only has
		//! to be written, reviewed, and tested once.  Pure IProgressCallback
		//! plumbing -- no AppKit/Qt dependency, so it is exercised directly by
		//! tests/AgentRenderAsyncTest.cpp instead of only transitively via a
		//! platform shell.
		//!
		//! NULL-safe: with `controller` == nullptr this just calls
		//! `doRasterize` directly (the pre-S4 / headless behaviour).
		//!
		//! Fix-round 2 (Windows cancel regression): `guiProgress` is now an
		//! EXPLICIT parameter -- the platform's own progress/cancel sink to
		//! compose in as mCancelProgress's `inner` for this render's
		//! duration.  This replaces the prior implicit assumption that
		//! `job.GetProgress()` (read at composition time, on the render
		//! worker thread) IS the platform's callback.  That assumption held
		//! on macOS (RISEBridge.mm installs its BlockProgressCallback
		//! PERSISTENTLY on the Job, so GetProgress() always returns it) but
		//! NOT on Windows: RenderEngine.cpp's startRender/
		//! startAnimationRender deliberately SKIP installing their
		//! per-render ProgressCallbackAdapter on the Job when a controller
		//! is attached (see startRender's own comment) -- so GetProgress()
		//! read nullptr for the render's whole duration, `inner` was never
		//! the platform's adapter, ProgressCallbackAdapter::Progress was
		//! never called, and the Windows Cancel button + progress/ETA UI
		//! went dead for every coordinator-routed production render.
		//!
		//! Two SEPARATE values are now used, on purpose:
		//!   - `guiProgress` (this parameter) is composed in as `inner` --
		//!     the thing that actually receives forwarded ticks and whose
		//!     own cancel-by-return-false is honoured.  Callers pass their
		//!     real progress sink here directly; there is no more guessing
		//!     via GetProgress().
		//!   - `prior = job.GetProgress()`, captured INSIDE the submitted
		//!     closure -- i.e. inside the coordinator slot, immediately
		//!     before this render's install -- is what gets RESTORED to the
		//!     Job's progress slot on every exit.  (Slot-ownership
		//!     hardening, 2026-07-12: the capture used to happen ONCE at
		//!     entry to this function, on the SUBMITTING thread, before the
		//!     fairness wait -- which could observe a TRANSIENT occupant's
		//!     callback (an agent render's coordProgress, installed from
		//!     the coordinator worker) and then permanently re-install it
		//!     on exit; a Job outliving its controller then carried a
		//!     dangling pointer into freed controller storage.  Inside the
		//!     slot the read is serialized against every other slot writer,
		//!     so it observes the honest steady-state value.)  `prior` is
		//!     deliberately NOT assumed to equal `guiProgress`: it is
		//!     "whatever the Job's progress slot honestly held right before
		//!     this render claimed it", which is the only thing a restore
		//!     can correctly promise on EITHER platform.  On macOS this is
		//!     the persistent BlockProgressCallback (equal to `guiProgress`
		//!     there, since that IS what's installed).  On Windows this is
		//!     typically nullptr (startRender skips the direct install when
		//!     a controller is attached; the completion handler's own
		//!     conditional clear then finds nothing of its adapter to
		//!     remove) -- so the restore is a harmless no-op there, and the
		//!     contract stays correct even if a future Windows change
		//!     starts installing something on that slot.
		//!
		//! This controller's own mCancelProgress (AgentRenderProgress()) is
		//! installed on the Job for the render's duration with `guiProgress`
		//! composed in as its `inner` (CancellableProgressCallback::SetInner),
		//! so the platform's progress UI / cancel button keeps working
		//! exactly as before while the coordinator's own cancel (Stop() /
		//! CancelAgentRender_) ALSO aborts a production render --
		//! CancellableProgressCallback::Progress refuses the instant EITHER
		//! source trips.
		//!
		//! Throw-path fix (Fix-round S4-1): BOTH the Job-slot restore (via
		//! ProgressRestoreGuard, restoring `prior`) AND the mCancelProgress
		//! `inner` reset (via InnerResetGuard) are RAII, armed immediately
		//! after the composed callback is installed, so a throw out of
		//! `doRasterize` (OIDN denoise is a documented real throw site) can
		//! never skip either teardown step.  Before that fix,
		//! `coordProgress->SetInner(nullptr)` was a plain statement AFTER the
		//! `doRasterize()` call with no guard covering it -- a throw left
		//! mCancelProgress.mInner dangling at whatever the platform shell's
		//! now-destroyed progress object was, and the interactive loop
		//! installs &mCancelProgress on the Job every pass (see RenderLoop's
		//! per-pass mCancelProgress.Reset(), which only clears the cancel
		//! flag, NOT mInner), so the very next interactive pass -- or a later
		//! agent render sharing the same mCancelProgress -- would forward
		//! ticks through the dangling pointer.
		//!
		//! Guard-destruction-order proof: the two RAII guards below fire back
		//! to back on ONE thread with no lock of their own, but that is safe
		//! REGARDLESS of which one is declared (and therefore destroyed)
		//! first, because both destructors run INSIDE the caller's mMutex
		//! hold. `fn` here is always invoked as the closure
		//! SubmitProductionRenderSync/SubmitAgentRenderSync hands to
		//! AgentRenderWorkerLoop_, which calls it at
		//! `try { if( fn ) fn(); } catch( ... ) { caught = ...; }` with that
		//! whole try/catch nested INSIDE `std::unique_lock<std::mutex>
		//! renderLk( mMutex )` (acquired via CancelAndParkRender_ and held for
		//! the render's WHOLE duration, released only after the try/catch
		//! block exits) -- so stack unwinding out of `doRasterize()` runs
		//! this function's local guard destructors, and the try/catch's own
		//! catch-and-continue, all while renderLk is still held. The ONLY
		//! other code that touches mCancelProgress is RenderLoop's per-pass
		//! `mCancelProgress.Reset()`, which itself takes the SAME mMutex
		//! before touching it. Mutual exclusion on mMutex therefore closes
		//! the window completely: an interactive pass cannot install a fresh
		//! inner or reset the cancel flag while this function's guards are
		//! unwinding, no matter which guard is torn down first. (Order is
		//! still chosen deliberately below: the Job-slot restore runs first,
		//! pointing the Job away from coordProgress, then the inner clear
		//! makes coordProgress itself inert -- belt-and-suspenders, not load-
		//! bearing for correctness.)
		//! `queueTimeoutMs` (Fix-round-3): forwarded verbatim to the
		//! underlying SubmitProductionRenderSync call -- bounds ONLY the
		//! fairness wait for a turn at the slot, same meaning as that
		//! method's own parameter of the same name. Default (30000ms)
		//! matches SubmitProductionRenderSync's default, so every existing
		//! caller (both platform shells) is unaffected; tests pass a short
		//! value to exercise the refusal path deterministically without a
		//! 30s wait.
		static bool RunProductionRenderComposed(
			IJobPriv&                     job,
			SceneEditController*          controller,
			const String&                clientLabel,
			IProgressCallback*            guiProgress,
			const std::function<bool()>& doRasterize,
			unsigned int                  queueTimeoutMs = 30000 );

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
		//! gets it. A cancel received after async submission but before the
		//! worker begins is retained for that occupant, so the worker cannot
		//! erase it with its per-render Reset(). Safe to call whether or not a
		//! render is actually in flight (a no-op cancel on an idle controller).
		//! Called by
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

		//! Halt ONLY the interactive render loop (mRenderThread): set the
		//! running flag false, trip the cancel flag, signal the condvar,
		//! and join the thread.  Idempotent.  Does NOT touch the agent-
		//! render worker (mAgentRenderThread) or mAgentRenderStop -- a
		//! production/agent render submitted immediately afterward via
		//! SubmitAgentRenderSync / SubmitProductionRenderSync /
		//! RunProductionRenderComposed is still served normally.
		//!
		//! Model-B F2 slice S4 fix round 4: this is the piece both
		//! platform shells actually want when they pause the interactive
		//! viewport before a production render (RenderViewModel.swift's
		//! startRender/startAnimationRender on macOS;
		//! MainWindow::onRender/onRenderAnimation on Windows) -- the goal
		//! there is "stop the interactive loop from racing the production
		//! rasterizer for the scene", not "permanently retire this
		//! controller's agent-render worker".  Before this split, both
		//! call sites called the monolithic Stop() below, which ALSO set
		//! mAgentRenderStop = true and joined mAgentRenderThread -- a
		//! ONE-SHOT, UNRESTARTABLE teardown (the worker is spawned only
		//! once, in the constructor; nothing ever resets the flag or
		//! respawns the thread) -- so the production render submitted a
		//! few lines later was refused with "controller stopped", and
		//! EVERY subsequent render on that controller (interactive restart
		//! notwithstanding) was refused the same way for the rest of the
		//! controller's lifetime.  Re-entrant with Start(): calling Start()
		//! after StopInteractive() respawns mRenderThread exactly as after
		//! a full Stop(), since Start() only ever looks at mRunning.
		void StopInteractive();

		//! Finalize every controller-owned UI interaction that may have lost
		//! its platform End event.  Pending object/camera deltas are committed
		//! to the retained Document and only controller-owned composites are
		//! closed.  Coordinated render admission calls this before claiming the
		//! scene, so a render requested mid-drag cannot strand the gesture.
		bool FinalizeOpenInteractions();

		//! Job-alive destruction boundary.  Raw C++ destruction cannot safely
		//! touch mJob because some direct owners destroy the Job first; C-ABI
		//! owners promise the borrowed Job outlives this controller.  Their
		//! destroy shim calls this before delete so callbacks are detached and an
		//! open gesture is persisted.  This is a terminal preparation call.
		//! Returns false if persistence failed, or if invoked synchronously from
		//! a Last Render sink callback/controller-owned final release or dirty-
		//! changed callback/copied-target finalizer (those callouts may be inside
		//! their own drain or a controller mutation).  Success permanently closes
		//! mutation admission; only idempotent Prepare and Destroy remain valid.
		//! The owner must stop/join ordinary external callers before entering this
		//! lifetime boundary; lifecycle arbitration covers internal workers,
		//! callbacks, and competing lifecycle calls, not arbitrary concurrent use
		//! of an object whose storage the owner is about to release.
		bool PrepareForDestruction();

		//! C destroy shim's single-owner terminal claim.  Unlike the public
		//! idempotent Prepare call, this atomically reserves the eventual delete
		//! before any callback/drain quiescence wait.  Returns false when another
		//! Prepare/Destroy/raw destructor already owns the lifecycle transition.
		bool PrepareForDestructionForDelete();

		//! True after a raw C++ destructor has begun.  The C destroy shim uses
		//! this to reject a concurrent/re-entrant second delete while the raw
		//! destructor is quiescing an in-flight dirty callback.
		bool IsRawDestructionInProgress() const
		{
			return mInDestructorTeardown.load( std::memory_order_acquire );
		}

		//! C-ABI teardown helper: true while this thread is executing one of this
		//! controller's Last Render sink callbacks or releasing a controller-owned
		//! sink reference that may invoke its final destructor.  Synchronous
		//! Destroy is rejected in that state; the owner must retry after the
		//! callout returns.
		bool IsInLastRenderSinkCallbackOnThisThread() const;
		bool IsInDirtyChangedCallbackOnThisThread() const;

		// Refinement pause + status (UI redesign, design brief A2) ------
		// "The viewport is the renderer": explicit user-facing Pause /
		// Resume of progressive refinement, plus an HONEST status
		// readout.  There is no sample-accumulation "pass N of M" in the
		// interactive loop — it refines by walking a 6-level resolution
		// ladder and then runs a denoised polish pass (see mPreviewScale
		// / mPolishState) — so the status surfaces exactly that.

		//! Pause progressive refinement: joins the interactive render
		//! thread via StopInteractive(), so the on-screen image survives
		//! and CPU goes quiet.  Edits made while paused mutate the scene
		//! normally and appear on Resume.  Idempotent.
		void PauseRefinement();

		//! Resume after PauseRefinement(): respawns the render loop with
		//! a normal initial pass, after which the idle-refinement ladder
		//! walks back to full quality.  No-op when not paused.  Note any
		//! Start() — including the post-production-render restart the
		//! platform shells perform — also clears the paused flag; after
		//! that restart the viewport genuinely is live again, so paused
		//! state would be a lie.
		void ResumeRefinement();

		bool IsRefinementPaused() const;

		//! Interactive-refinement phase for status chrome.  Values race
		//! benignly with the render thread — this is a poll-for-display
		//! API, not a synchronization point.
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

		// Interactive region-of-interest (UI redesign, design brief A4) -
		// A user-drawn box (full-resolution film pixel coordinates,
		// INCLUSIVE on all four edges) that restricts FULL-RES
		// interactive passes to the box, so iteration inside it is
		// dramatically cheaper.  Coarse preview-ladder passes
		// (divisor > 1) still render the whole frame — otherwise pixels
		// outside the box would go stale at mismatched scales during
		// navigation.  The region applies ONLY to the interactive
		// viewport. Production coordination preserves this state. Ordinary
		// production renders are still always full-frame because their
		// callbacks never read it; only the separately named Render Active
		// Region action captures and forwards these bounds. See
		// docs/gui/DESIGN_BRIEF.md A4.
		// right/bottom are clamped to the film at use; a box whose
		// left/top lies outside the film is ignored for that pass
		// (full-frame render) while still reported by
		// GetInteractiveRegion.  A degenerate box is refused at set
		// time.  Thread-safe; both mutators kick a render pass.

		void SetInteractiveRegion( unsigned int left, unsigned int top,
		                           unsigned int right, unsigned int bottom );
		void ClearInteractiveRegion();

		//! TRUE (+ coords) when a region is active.
		bool GetInteractiveRegion( unsigned int& left, unsigned int& top,
		                           unsigned int& right, unsigned int& bottom ) const;

		//! TRUE when the interactive rasterizer honors a render region —
		//! see IRasterizer::HonorsRegion.  (The stock interactive
		//! rasterizer always does; this exists so the region UI never
		//! has to assume.)
		bool InteractiveRasterizerHonorsRegion() const;

		//! Set the running flag false, trip the cancel flag, signal the
		//! condvar, and join the render thread -- AND permanently retire
		//! the agent-render worker (mAgentRenderThread), refusing every
		//! later agent/production submission with "controller stopped".
		//! Idempotent.  This is the FULL teardown; it's what the
		//! destructor calls (via RISE_API_DestroySceneEditController) and
		//! is correct there because the controller itself is going away.
		//! Rejected synchronously from a Last Render sink callback or final
		//! controller-owned sink release; that callout may be inside a counted
		//! direct call, the agent worker, a setter, or teardown itself, so lifecycle
		//! teardown must be retried by the owner after the callout returns.
		//! It is very likely NOT what a platform shell wants to call
		//! merely to pause the interactive viewport ahead of a production
		//! render -- see StopInteractive() above for that case.
		//!
		//! ORDER IS LOAD-BEARING: retires the agent worker (stop flag ->
		//! notifies -> cancel -> join) FIRST, and only THEN calls
		//! StopInteractive() for the interactive-loop tail.  A queued
		//! SubmitAgentRenderSync caller's wait predicate checks both
		//! "slot free" and "my turn" -- if the interactive cancel ran
		//! first, it could free the occupying render's slot (shared
		//! mCancelProgress) before mAgentRenderStop is observably set,
		//! letting the queued waiter's predicate go true and run a full
		//! render during teardown instead of refusing.  Retiring the
		//! agent worker first guarantees the stop flag is set (and
		//! waiters woken to re-check it) before the interactive teardown
		//! can free anyone's slot.  See
		//! RunQueuedSyncWaiterUnblocksOnStopTest (test (h) in
		//! tests/AgentRenderAsyncTest.cpp), which fails deterministically
		//! under the reversed order.
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
		//! Fallible because a coordinated/direct render can acquire
		//! admission between a shell's enabled-state snapshot and the
		//! callback. FALSE means no scrub state/time mutation occurred.
		bool OnTimeScrubBegin();
		bool OnTimeScrub( Scalar t );
		bool OnTimeScrubEnd();

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

		//! Human-readable labels for the next Undo()/Redo() step
		//! ("Translate", "Agent Edit", …) for the platform shells'
		//! Edit-menu items — thin forwards of EditHistory::LabelForUndo/
		//! Redo.  Empty when the corresponding stack is empty.  Safe to poll
		//! while an agent thread commits edits; the history peek is serialized
		//! with the mutation path by mMutex.
		String UndoLabel() const;
		String RedoLabel() const;

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

		// Authored-graph node tree (87 §5 step 4a) ---------------------
		//
		// THE GENERIC NODE-CHILDREN SURFACE that replaces the per-category
		// flat lists above.  Both shells consume it: 4b builds a SwiftUI
		// `OutlineGroup` from it, 4c a `QAbstractItemModel`.  Neither knows
		// anything about Objects specifically -- a category whose entities
		// have no hierarchy (Camera, Material, Painter, ...) is modelled as
		// N ROOTS WITH NO CHILDREN, which is why a shell needs no per-
		// category branch and why the flat categories keep working through
		// the identical code path.
		//
		// The tree is over the AUTHORED graph, not over the flat render
		// list.  For Category::Object that means the manager's entries with
		// the SYNTHESIZED ones (87 step 3's instancing expansions) FOLDED
		// INTO the chunk that produced them -- see BuildObjectTreeSeedsLocked_
		// for the rule and for why nobody may reconstruct it from the
		// spelling of a name.
		//
		// `ReadTree` IS THE SANCTIONED MULTI-NODE SURFACE.  Decided in round 4
		// of this step's review, and it changes what the per-node getters are
		// FOR, so read this before building anything on them.
		//
		// A node is addressed by an opaque handle that carries the SNAPSHOT
		// GENERATION it was minted in, and every per-node getter refuses a
		// handle whose generation is not the currently published one.  That
		// tag remains worth having: it turns "silently re-resolves onto some
		// other node" into a visible failure -- remove one object and a bare
		// index that named `BBB` names `CCC`, with a valid-looking name
		// coming back and no way for the caller to tell (measured).
		//
		// But a handle is NOT A DURABLE IDENTITY and must not be treated as
		// one.  Its guarantee is bounded: VALID WITHIN ONE WALK, against the
		// generation you observed.  Two things bound it, and neither is going
		// away:
		//   - A REBUILD of the stores (ClearAll + re-derive) is only
		//     detectable because `AuthoredTree::rebuildCount` is folded into
		//     the equivalence.  Take that away and the tag cannot see it,
		//     because the serials it is built on restart from 1.
		//   - An INCREMENTAL edit invalidates a whole category's handles even
		//     when nothing the shell drew has moved -- see the
		//     "HANDLE CHURN" note on the getters below.
		// So nothing should hold a handle ACROSS AN EVENT-LOOP TURN.  Nothing
		// in tree does: 87 §5 step 4 keys expand state on the tree PATH, this
		// controller's selection is by NAME (`SetSelection( Category, const
		// String& )`), and `ViewportBridge::categoryTree` reads `ReadTree`
		// only.  4b and 4c must be written the same way.
		//
		// The per-node getters therefore stay as a CONVENIENCE (a single-node
		// probe, the C-ABI's only node surface) and as a SAFETY NET -- a
		// detectable-when-it-can-be failure beats a silent one.  They are not
		// deleted and neither is the generation check.
		//
		// PREFER `ReadTree` for anything that walks more than one node.  It
		// is the only TRANSACTIONAL entry point: one refresh, one locked
		// pass, the whole tree copied out.  A multi-call walk is not
		// transactional even without a mutation -- each getter takes the
		// leaf lock on its own, so another thread's count call can land
		// between two of them.  DO NOT "simplify" the handle back to a raw
		// index either: within a walk the tag is what makes the failure
		// visible.
		//
		// REFRESH CADENCE, and how it differs from the flat getters
		// deliberately: `TreeNodeCount` and `TreeRootCount` refresh the
		// snapshot; the per-node accessors serve the PUBLISHED snapshot
		// without refreshing.  The flat getters refresh on every indexed
		// call, which makes a full walk of N entries O(N^2) (each
		// CategoryEntityName rebuilds the whole list).  A tree walk touches
		// every node several times, so repeating that here would be O(N^2)
		// with a much larger constant.  Entering through a count getter is
		// not a burden the shells have to remember: a tree walk BEGINS at
		// the roots.
		//
		// A refresh REPUBLISHES ONLY WHEN THE TREE ACTUALLY CHANGED, and the
		// generation is bumped only then.  That is load-bearing rather than
		// an optimisation: bumping on every refresh would invalidate every
		// outstanding handle on every count call, which makes the handle API
		// unusable.  Compare-then-publish costs one O(n) structural compare
		// on top of an O(n) build.
		//
		// EPOCH CONTRACT, and a TRAP for 4b/4c.  A shell re-reads when
		// `SceneEpoch()` advances.  That is sufficient TODAY only because
		// `parent` is a READ-ONLY property (ObjectIntrospection.cpp) and every
		// re-parent therefore goes through a document chunk edit, which bumps
		// the epoch.  When 4b/4c add DRAG-TO-REPARENT, that path MUST bump
		// `mSceneEpoch` itself: a re-parent changes the TREE without changing
		// any category's entity LIST, so nothing else in the pipeline would
		// notice, and the outliner would keep drawing the old hierarchy.  The
		// generation tag does not cover this -- it makes a stale handle fail,
		// it does not tell a shell that the shape it drew is out of date.

		//! An opaque, GENERATION-TAGGED node handle.  64 bits because it is
		//! two fields: the high 32 are the snapshot generation the handle was
		//! minted in, the low 32 the index into that snapshot's node table.
		//! Callers must treat it as opaque -- the split is an implementation
		//! detail of Encode/DecodeTreeHandle in the .cpp.  32 bits would have
		//! forced a narrow generation field that wraps within one GUI session
		//! and starts making stale handles look valid again, which is the
		//! exact failure the tag exists to remove.
		typedef unsigned long long TreeNodeHandle;

		//! Handle value meaning "no such node" -- returned for an
		//! out-of-range index, for a handle from an older generation, and
		//! reported as the parent of a root.
		static constexpr TreeNodeHandle kInvalidTreeNode = 0xFFFFFFFFFFFFFFFFull;

		//! Sentinel for a RAW node index inside an AuthoredTree
		//! (TreeNodeRow::parent).  Distinct from kInvalidTreeNode, which is a
		//! HANDLE: the two live in different value spaces and conflating them
		//! is how a root's parent would decode to node 0xFFFFFFFF.
		static constexpr unsigned int kInvalidNodeIndex = 0xFFFFFFFFu;

		//! Total nodes in `cat`'s tree.  REFRESHES the snapshot.
		unsigned int TreeNodeCount( Category cat ) const;
		//! Number of ROOT nodes in `cat`'s tree.  REFRESHES the snapshot.
		unsigned int TreeRootCount( Category cat ) const;
		//! The generation of `cat`'s currently published tree.  Does NOT
		//! refresh.  A caller that wants to know whether a walk it just did
		//! spanned a republish reads this before and after and compares.
		//!
		//! 0 means NO REFRESH HAS RUN for this category yet -- nothing is
		//! published, so there are no nodes and no handle can resolve.  Note
		//! that a legitimately EMPTY category does NOT stay at 0: since round
		//! 4 the tree carries `rebuildCount`, which differs from the
		//! default-constructed 0 on the very first refresh, so an empty
		//! category publishes an empty tree at generation >= 1.  (An earlier
		//! version of this doc said the opposite, and it was true then.)
		//!
		//! Generations are minted from a PROCESS-GLOBAL counter shared by
		//! every controller and every category, so no two published trees
		//! anywhere in the process share one -- see the round-4 P3-1 note in
		//! the .cpp.
		unsigned long long TreeGeneration( Category cat ) const;

		// HANDLE CHURN, and an ASYMMETRY a 4b/4c author must not be
		// surprised by (round-4 P2).  An INCREMENTAL CST param edit
		// (`Cst.cpp`'s re-Finalize loop) DROPS and RE-ADDS the affected
		// Material / Geometry / Light / Modifier entity in the same manager
		// under the same name, which mints a fresh registration serial --
		// while OBJECTS are re-pointed IN PLACE, precisely so the raw
		// addresses the TLAS stores stay valid, and keep their serial.
		//
		// So dragging a sphere's `radius` bumps the Geometry tree's
		// generation on EVERY tick and invalidates every outstanding
		// Geometry handle, while the same gesture on an Object's transform
		// invalidates nothing.  That is CORRECT under the identity model --
		// the geometry really is a different instance -- and it is harmless
		// exactly because `ReadTree` is the sanctioned surface and nothing
		// holds a handle across a turn.  It is documented here so that a
		// shell which finds its handles dying under a drag reaches for
		// `ReadTree` instead of "fixing" the equivalence by dropping the
		// serial, which would re-open round-4 case S.

		//! Handle of the `rootIdx`-th root, or kInvalidTreeNode.
		TreeNodeHandle TreeRootNode( Category cat, unsigned int rootIdx ) const;
		//! How many children `node` has.  0 for an unknown or STALE handle.
		//!
		//! `ByHandle` for the same reason the C-ABI mirror carries it: round
		//! 3 widened the node parameter from `unsigned int` to
		//! `TreeNodeHandle` and changed NOTHING ELSE in the signature, so a
		//! caller written against the pre-handle API keeps compiling and
		//! degrades to "every node is a childless row" -- indistinguishable
		//! from a legitimately flat category.  A rename is the only thing
		//! that breaks it loudly.  Round 4 applied it on this side too, which
		//! also gives the C++ and C names parity.  (The other three getters
		//! kept their names: each ALSO changed its return type, so a caller
		//! storing the result in an `unsigned int` at least gets a narrowing
		//! the compiler can warn about, and the C ABI's out-pointer widening
		//! is a hard error outright.)
		unsigned int TreeChildCountByHandle( Category cat, TreeNodeHandle node ) const;
		//! Handle of `node`'s `childIdx`-th child, or kInvalidTreeNode.
		TreeNodeHandle TreeChildNode( Category cat, TreeNodeHandle node, unsigned int childIdx ) const;
		//! `node`'s parent handle, or kInvalidTreeNode for a root / unknown /
		//! STALE handle.  Walking this up to a root is how a shell builds the
		//! tree PATH that 87 §5 step 4 keys expand-state on -- but do that
		//! WITHIN one walk, or better over a `ReadTree` copy, not by parking
		//! a handle between turns.
		TreeNodeHandle TreeNodeParent( Category cat, TreeNodeHandle node ) const;
		//! `node`'s entity name -- the identity a shell passes to
		//! `SetSelection` and renders as the row label.  Empty for an unknown
		//! or STALE handle.  `ByHandle` for the reason on
		//! `TreeChildCountByHandle`.
		String       TreeNodeNameByHandle( Category cat, TreeNodeHandle node ) const;

		//! One tree node, as published in the snapshot.  `parent` and the
		//! `childIndices` slice are indices into the SAME snapshot's node
		//! table -- see AuthoredTree for why that identity is load-bearing.
		struct TreeNodeRow
		{
			String       name;         //!< entity name (the selection identity)
			unsigned int parent;       //!< kInvalidNodeIndex for a root
			unsigned int firstChild;   //!< offset into AuthoredTree::childIndices
			unsigned int childCount;   //!< length of that slice
			//! REGISTRATION IDENTITY -- `IManager::GetItemSerial` for the
			//! entity behind this row, 0 when the backing store has no
			//! serial (see TreeNodeSeed::serial).  Carried so that
			//! TreesEquivalent can tell a REPLACEMENT from a no-op: a
			//! remove + re-add under the SAME name is a different instance
			//! with the same name, same parent and the same sort position,
			//! so every other member of this row compares equal and the
			//! generation would stand -- leaving an outstanding handle
			//! resolving onto an object the manager no longer holds.  This
			//! is the identity model `SceneEditor.cpp`'s capture/apply
			//! serial gate already enforces one layer down; a name-only
			//! equivalence here would contradict it.
			//!
			//! MEANINGFUL PER INDEX, NOT CATEGORY-WIDE.  A serial is unique
			//! only within one manager instance, and `Category::Painter` is
			//! a UNION of two managers with independent counters -- so two
			//! Painter rows denoting two genuinely different entities can
			//! carry the same number.  `TreesEquivalent` compares row `i`
			//! against row `i`, which is exactly the comparison the value
			//! supports; do not build a set or a map out of these.
			//!
			//! AND IT SAYS NOTHING ACROSS A REBUILD.  See
			//! AuthoredTree::rebuildCount -- the counter behind every serial
			//! restarts when the manager is reconstructed, so the serial is
			//! an identity WITHIN one instance of the stores only.
			//!
			//! NOT part of the public handle surface and deliberately not
			//! exposed by a getter -- it is an equivalence input, not a row
			//! property a shell should key on.  (A shell keys on the NAME,
			//! which is what selection takes.)
			unsigned long long serial = 0;
		};

		//! ONE STRUCT, READ AND PUBLISHED UNDER ONE LOCK HOLD, and it must
		//! stay that way.
		//!
		//! The flat per-category surface is a single `std::vector<String>`:
		//! there is nothing to keep consistent with anything else.  A TREE is
		//! a node table PLUS index arrays that address INTO that table, so a
		//! reader that saw a NEW `childIndices` against an OLD `nodes` would
		//! read OUT OF BOUNDS, not merely answer stale.
		//!
		//! WHAT ACTUALLY PREVENTS THAT is `mUiSnapshotMutex`: RefreshTreeSnapshot_
		//! assigns this struct while holding it, and EVERY reader holds the
		//! same lock for the WHOLE of its read.  It is NOT the assignment
		//! being one statement -- the publish is the compiler-generated
		//! `AuthoredTree::operator=( AuthoredTree&& )`, which is FOUR
		//! sequential member assignments (three vector moves and the
		//! generation), and a reader that took no lock could land between any
		//! two of them.  An earlier version of this comment claimed the
		//! single assignment was itself the mechanism; it is not, and reading
		//! it that way would invite a lock-free reader that is unsound.
		//!
		//! Keeping the vectors in one struct is still worth doing -- it is
		//! what makes "publish them all or none" the obvious shape and stops
		//! a future edit from publishing one member outside the lock -- so DO
		//! NOT split these back out into separate snapshot members.
		struct AuthoredTree
		{
			std::vector<TreeNodeRow>  nodes;
			std::vector<unsigned int> childIndices;  //!< flattened child lists
			std::vector<unsigned int> roots;         //!< indices into `nodes`
			//! The generation this tree was published at; 0 = never
			//! published.  Handles minted from this tree carry it, and the
			//! per-node getters refuse any handle that does not match.
			//! Travels INSIDE the struct so it cannot be published
			//! separately from the table it describes.
			unsigned long long        generation = 0;
			//! WHICH INSTANCE OF THE STORES this tree was read out of --
			//! `IJobPriv::GetContainerRebuildCount`, stamped by
			//! `BuildCategoryTreeLocked_`.  Compared by `TreesEquivalent`,
			//! which is what makes a whole-scene rebuild republish.
			//!
			//! It is here because A SERIAL IS NOT A CROSS-REBUILD IDENTITY,
			//! and the rest of this API's identity story is built on serials.
			//! `GenericManager::m_nNextSerial` is per-manager-INSTANCE and
			//! starts at 0 on construction; `Job::ClearAll` destroys and
			//! recreates every manager; a re-derive re-registers the same
			//! document in the same order.  So a full re-derive reproduces
			//! BYTE-IDENTICAL serials for genuinely new instances, every row
			//! compares equal, the generation stands, and a handle minted
			//! before the rebuild still resolves -- now onto an entity that
			//! did not exist when it was minted.  (Measured; case U.)
			//!
			//! THE HOLE THIS CLOSES BELONGED TO EVERY CATEGORY, not to the
			//! serial-less ones.  An earlier version of this header
			//! enumerated Medium / Rasterizer / Film / Animation /
			//! SceneVariant and implied the gap was theirs.  Their gap is
			//! real and separate (a same-name REPLACEMENT inside one store
			//! is invisible there, because there is no serial to move -- see
			//! TreeNodeSeed::serial).  The rebuild gap was UNIVERSAL: it
			//! applied to Object, Material, Geometry, Light and Painter
			//! exactly as much, because it defeats the serial rather than
			//! being defeated by its absence.
			//!
			//! Reachable on the mainline, not exotic: `ApplyCstParamEdit`
			//! returning 2 or 3 -- the full-re-derive fallback taken by every
			//! category the incremental path refuses, which is all Camera and
			//! all Painter edits, `let` edits, unnamed chunks and composed
			//! materials -- plus `RederiveCstWithVariant` and reopening a
			//! document into a reused Job.
			//!
			//! Bumped at TWO sites in Job, not one: `InitializeContainers` and
			//! `SetPrimaryAcceleration`, which replaces the ObjectManager on its
			//! own.  Round 5 found the second one missing here; no GUI can reach
			//! it today (its callers are the CLI console, the Blender bridge at
			//! job-build time, and 3DSMax, none of which holds a controller), but
			//! the fresh manager restarts its serials at 0 just the same, so it
			//! would have reopened this exact hole for whoever wired it up next.
			unsigned long long        rebuildCount = 0;
		};

		//! THE TRANSACTIONAL READ, and what every multi-node consumer should
		//! use.  Refreshes once, then copies the whole published tree out
		//! under a single hold of the leaf lock, so the result cannot mix two
		//! trees the way a sequence of per-node getters can.  The returned
		//! indices (`roots`, `TreeNodeRow::parent`, `childIndices`) are RAW
		//! indices into `out.nodes` -- not handles -- because the caller owns
		//! the copy and nothing can republish underneath it.  `HandleFor`
		//! below is the way back from one of those indices to a handle the
		//! per-node getters accept.
		//!
		//! This is an API PROPERTY, not an accident of how a given shell
		//! happens to be written: ViewportBridge::categoryTree and 4b's
		//! SwiftUI model both depend on it, and neither should have to
		//! rediscover it by reasoning about the getters.
		void ReadTree( Category cat, AuthoredTree& out ) const;

		//! THE WAY BACK from a `ReadTree` row to the per-node getters.
		//!
		//! `ReadTree` is what every multi-node consumer is told to use, and it
		//! yields RAW INDICES -- while `TreeNodeNameByHandle` / `TreeChildCountByHandle` /
		//! `TreeNodeParent` take generation-tagged HANDLES, and the encoding is
		//! deliberately private.  Without this a shell that follows the advice
		//! has only two ways out of its own model row: re-walk with the
		//! per-node getters (surrendering the transactional property it took
		//! ReadTree for), or hand-roll the `(generation << 32) | index` layout
		//! in shell code (breaking the "two places know the layout" invariant
		//! that makes the tag changeable at all).  So: one function, on this
		//! side of the wall.
		//!
		//! VALIDITY.  The handle carries `t.generation`, and every getter
		//! compares that against the CURRENTLY PUBLISHED generation for the
		//! category -- so a handle minted from a copy resolves for exactly as
		//! long as that copy is still the published tree, and FAILS (rather
		//! than naming some other node) once a real change republishes.  An
		//! idle refresh does not republish, so a handle minted from a copy
		//! survives a UI poll.
		//!
		//! Returns kInvalidTreeNode for an out-of-range index, and for a tree
		//! that was never published (`generation == 0` -- e.g. one straight
		//! out of `BuildAuthoredTree`), because no such handle could ever
		//! resolve.
		//!
		//! PURE and static: it reads only the tree it is handed, so it takes
		//! no lock.
		//!
		//! IT DOES CARE WHICH CONTROLLER THE COPY CAME FROM, and an earlier
		//! version of this line said the opposite and sold that as a feature.
		//! A handle is only ever resolved AGAINST a particular controller's
		//! published tree, so one minted from controller A's copy and handed
		//! to controller B is a bug, not a portability affordance.  Since
		//! round 4 generations come from a process-global counter, so such a
		//! handle FAILS to decode rather than naming B's node at the same
		//! index (which is what it used to do -- measured).
		static TreeNodeHandle HandleFor( const AuthoredTree& t, unsigned int index );

		//! THE WAY BACK FROM A LIVE ENTRY TO THE ROW THAT REPRESENTS IT.
		//!
		//! The outliner draws the AUTHORED graph, so a SYNTHESIZED entry
		//! (87 step 3's instancing expansions) is not a row: `I[1,0]` folds
		//! into `I`.  Selection, though, is by NAME and comes from wherever
		//! the user clicked -- and a VIEWPORT pick necessarily names the LIVE
		//! entry it hit, because that is the only thing a ray can return.  So
		//! `GetSelectionName()` can legitimately hold a name no row answers
		//! to, and a shell matching it against row names highlights nothing.
		//! That was the state 87 step 4b left behind, deliberately.
		//!
		//! This resolves that name to the row.  Answers `entityName` itself
		//! whenever that IS a row -- which covers every ordinary object, the
		//! step-3a COLLAPSE case (entry name == chunk name), and the PASS-1b
		//! UNFOLD case where a same-name collision made the fold refuse.
		//!
		//! THE AUTHORITY IS TREE MEMBERSHIP, NOT A REIMPLEMENTED FOLD RULE.
		//! `BuildObjectTreeSeedsLocked_` decides what folds, through several
		//! interacting cases (collapse, unfold-on-collision, synthesized
		//! chunk nodes).  Restating any of that here would be a second copy
		//! free to drift, and a resolver that names a row the tree does not
		//! contain is worse than no resolver -- the shell would highlight
		//! nothing while reporting success.  So the only question asked of
		//! the fold data is "what chunk did this entry come from?"
		//! (`IObjectManager::GetObjectProvenance`, the ONE sanctioned route
		//! -- see its header: a synthesized name is an opaque token and
		//! nobody may split it on `.` or probe for a `[`), and the only
		//! question asked of the tree is "is this name a row?".  Every
		//! discrimination falls out of the second question.
		//!
		//! ITERATED and CYCLE-PROOF: the chain is walked with a visited set
		//! rather than a hop budget, so a future expansion that folds one
		//! synthesized entry into another resolves, and a fold CYCLE
		//! terminates instead of spinning.  One hop is all any shape reaches
		//! today.
		//!
		//! Returns `entityName` UNCHANGED when nothing resolves -- an empty
		//! name, a name that has since been deleted, a non-Object category
		//! (only Objects have provenance), or a contended read.  The caller
		//! then highlights nothing, which is exactly the pre-fix behaviour:
		//! this can restore a highlight, never move one onto a wrong row.
		//!
		//! IT DOES NOT CHANGE WHAT IS SELECTED, and that is the point.  The
		//! selection stays the LIVE ENTRY the user clicked, so the gizmo
		//! still lands on THAT copy, the properties panel still inspects
		//! THAT copy, and the viewport chrome still names it.  Folding
		//! `mSelectionName` itself would have moved all three onto the array
		//! -- and for a COUNTED chunk, whose row has no live object at all,
		//! `IObjectManager::GetItem` would return null and the gizmo would
		//! disappear rather than move.  This is a PRESENTATION fold, applied
		//! at the one place that presents.
		//!
		//! IT DOES NOT REFRESH THE TREE, unlike `ReadTree` /
		//! `TreeNodeCount` -- it answers against whatever is PUBLISHED,
		//! which is what the shell has drawn.  Both shells re-read the
		//! selection on every refresh and the Qt outliner's refresh rides
		//! `imageUpdated` (once per preview frame), so refreshing here would
		//! defeat the epoch gate they use to keep the O(n) rebuild off that
		//! path.  The sole exception is a category nothing has published yet
		//! (`TreeGeneration( cat ) == 0`), which is a COLD controller rather
		//! than a stale one and publishes once, so this stays usable on its
		//! own.  See the fuller note at the definition.
		String ResolveTreeRowName( Category cat, const String& entityName ) const;

		//! The row a shell should highlight for the CURRENT selection:
		//! `ResolveTreeRowName( GetSelectionCategory(), GetSelectionName() )`.
		//! This is what an outliner compares against its row names; the
		//! unfolded `GetSelectionName()` remains what the property panel,
		//! the gizmo and the viewport chrome use.
		String SelectionRowName() const;

		//! The input to the pure tree assembler: one record per node that is
		//! to APPEAR in the tree, already resolved out of whatever the
		//! category's backing store is.
		struct TreeNodeSeed
		{
			String             name;
			String             parent;      //!< "" -- or a name that is not itself a seed -- means ROOT
			unsigned long long order  = 0;  //!< sibling display order key
			//! Registration identity of the entity behind this seed, or 0
			//! when its backing store does not provide one.  DISTINCT from
			//! `order`, which is only a display key: for the flat categories
			//! `order` is the entity's position in the enumeration, while
			//! this is `IManager::GetItemSerial`.  They coincide for
			//! Category::Object, where the serial IS the declaration order.
			//!
			//! 0 means "this store cannot tell a replacement from the
			//! original" -- Medium, Rasterizer, Film, Animation and
			//! SceneVariant, none of which is reached through an IManager
			//! from here.  For those categories a remove + re-add under the
			//! same name WITHIN one instance of the stores does NOT bump the
			//! tree generation, and an outstanding handle keeps resolving.
			//! Said out loud rather than left for a reader to infer from a
			//! silent 0.
			//!
			//! That is the ONLY gap left, and it is narrower than it reads:
			//! a whole-scene REBUILD is caught for every category, serial or
			//! not, by AuthoredTree::rebuildCount.
			//!
			//! Category::Painter does not come through here at all -- its
			//! serial is positional, from the manager the row was enumerated
			//! out of.  See BuildCategoryTreeLocked_.
			unsigned long long serial = 0;
		};

		//! Assemble an AuthoredTree from seeds.  PURE and static: it touches
		//! no controller state and no manager, which is what lets its two
		//! hostile inputs -- a DANGLING parent and a CYCLE -- be driven
		//! directly by a test.  Neither is reachable through
		//! `IObjectManager::SetObjectParent` today (it refuses an unknown
		//! parent and walks the ancestor chain to refuse a cycle), so a
		//! scene-level test cannot produce either and this seam is the only
		//! honest way to pin the guards.
		//!
		//! Contract:
		//!  - Every seed becomes exactly one node.  Nothing is ever dropped.
		//!  - A parent that names no seed is treated as ROOT (the same rule
		//!    `ComposeWorldTransforms` applies to a dangling link).
		//!  - A cycle is BROKEN, not followed: one link on each cycle is cut
		//!    and the node it belonged to becomes a root, so the walk
		//!    terminates and every node stays visible.  The assembly is
		//!    iterative throughout -- no recursion, so no stack overflow on
		//!    a deep chain either.
		//!  - Roots and each child list are ordered by `order`, ties broken
		//!    by name, so the result is deterministic.
		//!  - `generation` is left 0: only RefreshTreeSnapshot_ stamps it,
		//!    at publish time.
		static AuthoredTree BuildAuthoredTree( const std::vector<TreeNodeSeed>& seeds );

		// =====================================================================
		// doc-88 Phase 3 S11 -- the multi-parent Painter/Material DAG.
		//
		// Generalizes BuildAuthoredTree (immediately above) from a single-
		// parent TREE (Category::Object) to a multi-parent DAG (Category::
		// Painter + Category::Material, plus any ChunkCategory::Function node
		// an edge from one of those actually reaches -- docs/gui/
		// NODE_GRAPH_CANVAS.md §1.1's two-level model: a `def` stage stays
		// internal to its owning painter node, NEVER a top-level graph node
		// here). Same three-layer split as the object tree:
		//   1. BuildPainterMaterialGraph        -- PURE.  Seeds+edge-seeds ->
		//                                          PainterMaterialGraph.  The
		//                                          only layer a test may hand
		//                                          a hostile input to.
		//   2. BuildPainterMaterialGraphSeedsLocked_ -- seeds nodes from the
		//                                          retained CST Document's
		//                                          own Painter/Material
		//                                          chunks (SceneReferenceGraph
		//                                          ::AllChunks), edges from
		//                                          SceneReferenceGraph::
		//                                          EdgesAndDangling over the
		//                                          SAME scan, both under
		//                                          mMutex; the live Painter/
		//                                          Material managers are
		//                                          consulted ONLY for
		//                                          enrichment (serial,
		//                                          def-count), never as the
		//                                          node SOURCE -- see the
		//                                          .cpp for why (doc-88 S11
		//                                          review round 1 P1-2).
		//   3. RefreshPainterMaterialGraphSnapshot_  -- the SAME stale-fallback
		//                                          publish discipline
		//                                          RefreshTreeSnapshot_ uses.
		//
		// UNLIKE BuildAuthoredTree, this assembler never WALKS the structure
		// it builds -- a DAG's adjacency is exactly the edge-seed list,
		// classified once against an id->index map (GraphNodeSeed::id, the
		// chunk's own Cst::NodeId -- see review round 1 P1-2), so a crafted
		// cycle (or a self-reference) needs no break-and-root logic: it is
		// simply two ordinary adjacency rows, never a hang.  See
		// BuildPainterMaterialGraph's own comment.
		// =====================================================================

		//! The input to the pure DAG assembler: one record per NODE that is
		//! to appear in the graph, seeded from a `SceneReferenceGraph::
		//! DocumentChunk` (identity/keyword/category/name) plus enrichment
		//! read out of the live Painter/Material managers (order/serial/
		//! def-count) -- see BuildPainterMaterialGraphSeedsLocked_.
		//!
		//! `id` is the chunk's OWN `Cst::NodeId` and is the assembler's node
		//! IDENTITY (doc-88 S11 review round 1 P1-2/P2-1/P2-2) -- NOT
		//! (category,name): two chunks of the SAME category can legally
		//! share a name (a colour painter and a `scalar_painter` both named
		//! "P"), and each gets its OWN node here. `id` must be nonzero and
		//! unique per seed (0 is the reserved "no such chunk" sentinel
		//! `Cst::NodeId` uses throughout this codebase -- `DocParamId`,
		//! `ResolveChunk`, ... -- so a real document chunk never has it).
		struct GraphNodeSeed
		{
			Cst::NodeId   id = 0;
			String        name;
			String        chunkKeyword;
			ChunkCategory category   = ChunkCategory::Painter;
			unsigned long long order  = 0;   //!< sibling/display order key, same role as TreeNodeSeed::order
			unsigned long long serial = 0;   //!< registration identity, same role as TreeNodeSeed::serial
			int           defCount   = 0;
		};

		//! The input to the pure DAG assembler: one record per RESOLVED
		//! reference edge, already resolved out of the retained Document
		//! (see SceneReferenceGraph::Edges) -- deliberately the SAME shape
		//! ReferenceEdge carries (referrer side + target side), so the
		//! seeds-gathering step is a straight field copy, not a re-derivation.
		//!
		//! `fromId`/`toId` are the referrer's/target's `Cst::NodeId` (see
		//! GraphNodeSeed::id) -- the assembler matches edges to nodes by
		//! THESE, not by (category,name), for the same ambiguity reason.
		//! `toId == 0` means the reference is dangling or points outside
		//! this graph's modeled categories (`toName` still carries the
		//! target's name for display either way) -- `fromId` is always
		//! nonzero (an edge is only ever discovered by walking a real
		//! chunk's own params).
		struct GraphEdgeSeed
		{
			Cst::NodeId   fromId = 0;
			ChunkCategory fromCategory = ChunkCategory::Painter;
			String        fromName;
			String        paramName;
			int           occurrence   = 0;
			std::vector<ChunkCategory> portCategories;
			Cst::NodeId   toId = 0;
			ChunkCategory toCategory   = ChunkCategory::Painter;
			String        toName;        //!< may name no seed at all -- see BuildPainterMaterialGraph
		};

		//! One port on a graph node: a single (paramName, occurrence) slot
		//! that either REFERENCES another node (an outEdges row) or IS
		//! referenced BY another node (an inEdges row) -- the SAME struct
		//! serves both directions, mirroring ReferenceEdge's shape.
		struct GraphPort
		{
			String paramName;                          //!< the Reference-kind param's role
			int    occurrence      = 0;                 //!< 0-based occurrence among same-role siblings
			//! Index into PainterMaterialGraph::nodes of the OTHER end, or
			//! kInvalidNodeIndex when this port is a NODE-LESS PORT: the
			//! reference is dangling (points at a name nothing declares) or
			//! points OUT OF SCOPE (a category this graph does not model,
			//! e.g. a `standard_object.geometry` reference would never reach
			//! this graph at all, but a Painter->Object edge if one ever
			//! existed would land here). Never dropped silently -- see
			//! BuildPainterMaterialGraph.
			unsigned int otherNode = kInvalidNodeIndex;
			String otherName;                            //!< the other end's name, even when otherNode is invalid
			//! The port's declared TYPE (ParameterDescriptor::referenceCategories)
			//! -- what kind of chunk this slot accepts.
			std::vector<ChunkCategory> portCategories;
		};

		//! An opaque, GENERATION-TAGGED node handle for a PUBLISHED
		//! PainterMaterialGraph -- THE SAME `(generation << 32) | index`
		//! layout as `TreeNodeHandle` (see its own comment for the 32/32
		//! split rationale), minted with the SAME `EncodeTreeHandle` the
		//! object tree uses (doc-88 S11 review round 2 P1).
		//!
		//! WHY THIS EXISTS, and why `GraphNode` used to publish a raw
		//! `Cst::NodeId` instead: a chunk's `Cst::NodeId` is a PER-PARSE
		//! identity (`Cst.h`'s own NodeId lineage caveat) -- a full re-derive
		//! (any of the `ApplyCstParamEdit` fallback paths, `RederiveCstWith
		//! Variant`, reopening a document into a reused Job) mints a FRESH
		//! Document with FRESH NodeIds, and nothing stops a later chunk from
		//! being handed the SAME integer an earlier, now-gone chunk used to
		//! own. A shell that cached a raw `id` across such a reload and used
		//! it to, say, jump back to "the node the user last selected" could
		//! silently land on an unrelated chunk instead of failing loudly --
		//! exactly the aliasing hole `TreeNodeHandle` was built to close for
		//! the object tree, and the S11 spec (`docs/gui/NODE_GRAPH_CANVAS.md`
		//! §6 S11) requires the SAME discipline here.
		//!
		//! Callers must treat it as opaque; see `ResolveGraphNodeHandle`.
		typedef unsigned long long GraphNodeHandle;
		//! Handle value meaning "no such node" -- default `GraphNode::handle`
		//! before publish, and the refusal value `ResolveGraphNodeHandle`
		//! returns for an unresolvable handle. Same bit pattern as
		//! `kInvalidTreeNode`, kept as a SEPARATE named constant (rather than
		//! reusing `kInvalidTreeNode` directly) because the two live in
		//! different handle spaces -- see `kInvalidTreeNode`'s own comment on
		//! why conflating sentinel spaces is a hazard even when the bits match.
		static constexpr GraphNodeHandle kInvalidGraphNode = 0xFFFFFFFFFFFFFFFFull;

		//! One node: a single Painter/Material (or edge-reached Function)
		//! chunk. `inEdges` is the "parents is a LIST" half of the doc's
		//! §1.2 links-not-copies rule -- a painter shared by two materials
		//! is ONE node with TWO inEdges rows, never two nodes.
		struct GraphNode
		{
			//! Opaque, generation-tagged identity -- see `GraphNodeHandle`.
			//! `kInvalidGraphNode` here means "not yet stamped": true only
			//! for a `GraphNode` fresh out of the PURE `BuildPainterMaterial
			//! Graph` assembler, which has no generation to mint against (the
			//! same reason `AuthoredTree::TreeNodeRow` carries no handle at
			//! all). Every node this controller actually PUBLISHES (i.e. a
			//! `GraphNode` reachable via `ReadPainterMaterialGraph`) carries a
			//! real handle, stamped in `RefreshPainterMaterialGraphSnapshot_`
			//! right after `generation` is finalized -- see that function's
			//! own comment for why the stamp cannot happen any earlier.
			//! Deliberately NOT compared by `GraphNodesEqual`: it is a
			//! DERIVED field, not content, and the compare-then-publish step
			//! that decides whether to bump `generation` runs BEFORE a new
			//! generation exists to stamp with -- comparing it would either
			//! always disagree (spurious republish every refresh) or require
			//! computing the new generation before knowing whether one is
			//! needed. Replaces the raw `Cst::NodeId` this struct used to
			//! publish -- see `GraphNodeHandle`'s own comment for why.
			GraphNodeHandle handle = kInvalidGraphNode;
			String        name;
			String        chunkKeyword;                  //!< e.g. "ramp_painter", "ggx_material"
			ChunkCategory category    = ChunkCategory::Painter;
			unsigned long long order  = 0;                //!< presentation-order key, same role as TreeNodeSeed::order
			unsigned long long serial = 0;                //!< registration identity, same role as TreeNodeSeed::serial
			//! S10's ExpressionProgram::DefCount() for an expression-family
			//! painter (expression_painter / scalar_painter{expression}); 0
			//! for every other chunk kind. The §1.1 two-level model's hook:
			//! `def` stages are a COUNT here, never nodes of their own.
			int defCount = 0;
			std::vector<GraphPort> outEdges;              //!< this node's OWN reference params -> other nodes (or dangling)
			std::vector<GraphPort> inEdges;                //!< other nodes' reference params -> this node (its "parents")
		};

		//! ONE STRUCT, published under mUiSnapshotMutex exactly like
		//! AuthoredTree -- see AuthoredTree's own comment for why the publish
		//! must stay one assignment under one lock hold rather than several
		//! fields split apart.
		struct PainterMaterialGraph
		{
			std::vector<GraphNode> nodes;
			//! Generation this graph was published at; 0 = never published.
			//! Same process-global counter as AuthoredTree::generation
			//! (NextTreeGeneration()) -- so a Painter/Material graph
			//! generation and an AuthoredTree generation are drawn from the
			//! same space and never collide. Every `GraphNode::handle` in
			//! `nodes` is stamped from THIS field at publish time (doc-88 S11
			//! review round 2 P1) -- see `GraphNode::handle`'s own comment.
			unsigned long long generation   = 0;
			//! Which INSTANCE of the stores this graph was read out of --
			//! same role as AuthoredTree::rebuildCount; see its comment for
			//! why a serial alone cannot detect a ClearAll + re-derive.
			unsigned long long rebuildCount = 0;
		};

		//! The pure assembly step, seeds+edge-seeds -> PainterMaterialGraph.
		//! PURE and static, like BuildAuthoredTree: touches no controller
		//! state and no manager.
		//!
		//! Node identity is `GraphNodeSeed::id` (the backing chunk's own
		//! `Cst::NodeId`) -- NOT (category, name) (doc-88 S11 review round 1
		//! P1-2/P2-1/P2-2: a Painter and a Function chunk may share a name,
		//! the same coarseness Cst::BuildReferenceGraph's own "painter
		//! alias" already lives with, but so can TWO chunks of the SAME
		//! category, e.g. a colour painter and a `scalar_painter` both named
		//! "P" -- (category,name) cannot key those as separate nodes, `id`
		//! always can). A DUPLICATE `id` seed keeps the FIRST occurrence in
		//! presentation order (order, then name tie-break, exactly like
		//! BuildAuthoredTree) and DROPS every later seed sharing that id
		//! from `out.nodes` too -- not just from the lookup map. (An earlier
		//! draft of this assembler deduped ONLY the lookup map, leaving a
		//! second same-key seed in `out.nodes` as a permanently-unreachable
		//! orphan node no edge could ever address -- round-1 review caught
		//! it with a synthetic 2-duplicate-seed input producing 3 nodes
		//! instead of 1.) This is the caller's contract, same "least
		//! surprising degradation" posture BuildAuthoredTree's own
		//! duplicate-name handling documents.
		//!
		//! An edge seed whose `fromId` does not match any node seed's `id`
		//! is DROPPED (the caller's contract: every edge must be seeded
		//! FROM a node this graph actually contains). An edge seed whose
		//! `toId` is 0 or does not match any node seed's `id` becomes a
		//! NODE-LESS PORT on the referrer's outEdges (otherNode ==
		//! kInvalidNodeIndex, otherName preserved) -- covers both a genuinely
		//! DANGLING reference and a reference that resolves out of this
		//! graph's modeled categories; never a crash, never a silently
		//! dropped edge.
		//!
		//! A self-reference (fromName==toName, fromCategory==toCategory) is
		//! NOT special-cased: the node simply gets one row in its own
		//! outEdges AND one row in its own inEdges. A CYCLE among edge seeds
		//! needs no guard at all -- unlike BuildAuthoredTree this function
		//! never walks the structure it produces, so there is nothing for a
		//! cycle to make loop.
		static PainterMaterialGraph BuildPainterMaterialGraph(
			const std::vector<GraphNodeSeed>& nodeSeeds,
			const std::vector<GraphEdgeSeed>& edgeSeeds );

		//! THE TRANSACTIONAL READ -- mirrors ReadTree exactly: refreshes
		//! once, then copies the whole published graph out under a single
		//! hold of the leaf lock.
		void ReadPainterMaterialGraph( PainterMaterialGraph& out ) const;

		//! THE WAY BACK from a `GraphNode::handle` a caller squirreled away
		//! (e.g. "the node the user last selected") to that node's index in
		//! a LATER `PainterMaterialGraph` copy -- mirrors `HandleFor`'s
		//! reverse direction for the object tree, adapted to this type's own
		//! shape: `GraphNode` already carries its handle inline (unlike
		//! `AuthoredTree::TreeNodeRow`, which carries none), so there is
		//! nothing to MINT here, only to RESOLVE.
		//!
		//! Returns the resolved node's index into `g.nodes` on success.
		//! Returns `kInvalidNodeIndex` -- never a crash, never a silent
		//! alias onto whatever node happens to sit at the decoded index in
		//! `g` -- when `handle` is `kInvalidGraphNode`, was minted from a
		//! DIFFERENT generation than `g.generation` (the exact case a raw
		//! `Cst::NodeId` could not detect, see `GraphNodeHandle`'s own
		//! comment), or decodes to an out-of-range index.
		//!
		//! PURE and static, same testability posture as
		//! `BuildPainterMaterialGraph`: a test can hand it a hostile or
		//! cross-generation handle directly, with no controller, no lock, no
		//! live document.
		static unsigned int ResolveGraphNodeHandle( const PainterMaterialGraph& g, GraphNodeHandle handle );

		//! Resolve the UNIQUE node index in `g` for `(category, name)`, or
		//! -1 when zero or MORE THAN ONE node matches -- refuse rather than
		//! guess, the same posture `SceneReferenceGraph::ResolveChunk` uses
		//! for the identical hazard (a graph seeded straight from the
		//! Document, per `BuildPainterMaterialGraph`'s own comment, CAN
		//! legally contain two same-category chunks sharing a name -- node
		//! identity here is `GraphNodeSeed::id`, not `(category,name)`).
		//! `outMatches`, when non-null, receives the raw match count so a
		//! caller (or a test) can distinguish "not found" (0) from
		//! "ambiguous, refused" (>1) -- an index of -1 alone cannot tell
		//! those apart.
		//!
		//! PURE and static, same testability posture as
		//! `BuildPainterMaterialGraph`/`ResolveGraphNodeHandle`: a test can
		//! hand this a synthetic graph (built via `BuildPainterMaterialGraph`
		//! from hand-authored seeds, exactly like this class's own PART-2-
		//! style unit tests do) containing a deliberate same-category
		//! same-name duplicate, with no controller, no lock, no live
		//! document, no Job that would need to successfully DERIVE two
		//! same-named chunks (which a real scene load cannot do -- the live
		//! manager `AddItem` refuses a duplicate name and fails the whole
		//! derive; the DOCUMENT-seeded graph this resolves against has no
		//! such restriction, which is exactly the gap this method closes).
		//!
		//! Used by `AppearanceClosureForObject` to resolve the object's
		//! bound material name into a graph node; exposed on this class's
		//! public PURE-helper surface (not file-local) so callers/tests
		//! needing the identical "unique (category,name) or refuse" answer
		//! do not have to re-derive it.
		static int ResolveUniqueGraphNodeIndex( const PainterMaterialGraph& g, ChunkCategory category,
		                                         const String& name, int* outMatches = nullptr );

		// =====================================================================
		// doc-88 Phase 3 S17 -- connection-legality passthrough
		// (ConnectionLegality.h/.cpp; docs/gui/NODE_GRAPH_CANVAS.md sect. 6
		// S17). Thin, controller-scoped wrappers over the standalone
		// (Document + NodeId)-only `ConnectionLegality` module -- the
		// FUTURE S21 drag-drop canvas's pre-commit checks. Addressed by
		// (category, name) rather than `GraphNodeHandle`, matching
		// `SceneReferenceGraph::ResolveChunk`'s own addressing (a
		// `GraphNode` published to the ABI carries no raw `Cst::NodeId` by
		// design -- see `GraphNodeHandle`'s comment above -- so a
		// name-based resolve is this passthrough's natural seam; S21 can
		// read `chunkKeyword`/`category`/`name` directly off the
		// `GraphNode` it already has cached from `ReadPainterMaterialGraph`).
		// =====================================================================

		//! `ConnectionLegality::CheckConnectionByName` over this
		//! controller's CURRENT document (`mJob.GetCstDocument()`).
		//! Illegal (with a diagnostic) when either name fails to resolve,
		//! `paramName` is not a declared Reference-kind parameter on the
		//! target's descriptor, or the candidate's category/pipe is
		//! rejected -- see ConnectionLegality.h for the full contract.
		ConnectionVerdict CheckConnection(
			ChunkCategory targetCategory, const String& targetName,
			const String& paramName,
			ChunkCategory candidateCategory, const String& candidateName ) const;

		//! `ConnectionLegality::WouldCycle` over this controller's CURRENT
		//! document, with `from`/`to` resolved by (category, name) via
		//! `SceneReferenceGraph::ResolveChunk`. Returns false (never a
		//! crash) when either name fails to resolve or is ambiguous --
		//! the caller should treat an unresolved name as "cannot commit
		//! this wire at all" via `CheckConnection` first, not infer
		//! safety from a false `WouldCycle` alone.
		bool WouldCycle(
			ChunkCategory fromCategory, const String& fromName,
			ChunkCategory toCategory, const String& toName ) const;

		//! ONE FULL PASS of the transitive Function-node promotion BFS that
		//! `BuildPainterMaterialGraphSeedsLocked_` runs to find every
		//! Function-category chunk reachable from an in-scope Painter/
		//! Material referrer (doc-88 S11 review round 1 P2-3's fixpoint
		//! walk). Factored out of that method (doc-88 S11 review round 2
		//! P2-b) because the walk's own visited-set guard -- the thing that
		//! makes a crafted Function->Function CYCLE terminate rather than
		//! loop forever -- was previously reachable only through a REAL
		//! document's chunk descriptors, which today never form such a
		//! cycle post-derive, leaving the guard itself untested by anything
		//! but a comment's say-so.
		//!
		//! `edgesByReferrer` is the SAME adjacency index the production call
		//! site already builds once from its full edge scan (an in-memory
		//! multimap, `referrerId -> ReferenceEdge*`, built once and reused --
		//! see `BuildPainterMaterialGraphSeedsLocked_`'s own comment for why
		//! that reuse matters on a large scene). `promotedFunctionIds` is the
		//! visited set (a chunk id enters it at most once, which is what
		//! bounds this to the graph's actual size no matter how the edges
		//! are wired) and `functionFrontier` is the BFS queue-so-far, in
		//! discovery order; BOTH are read AND written in place, exactly as
		//! the inline loop this replaces did, so the caller's post-loop use
		//! of either (the node-seed emission loop, the edge-seed in-scope
		//! check) sees the identical end state.
		//!
		//! The loop bound is `functionFrontier.size()`, RE-READ every
		//! iteration -- that is what lets an entry THIS CALL appends get
		//! walked in the same pass, to a fixpoint, rather than needing the
		//! caller to invoke this repeatedly.
		//!
		//! PURE over its explicit parameters (no Document, no manager, no
		//! controller lock) -- a synthetic `edgesByReferrer`, including one
		//! encoding a Function->Function cycle, can drive this EXACT
		//! function in a test and prove it terminates, rather than a test
		//! re-deriving the walk against its own copy of the logic. The
		//! production call site (`BuildPainterMaterialGraphSeedsLocked_`)
		//! calls this with the SAME three pieces of state it threaded
		//! through the loop before this refactor -- behavior unchanged.
		static void ExpandFunctionPromotionFrontier(
			const std::multimap<Cst::NodeId, const ReferenceEdge*>& edgesByReferrer,
			std::set<Cst::NodeId>& promotedFunctionIds,
			std::vector<std::pair<Cst::NodeId, String> >& functionFrontier );

		// =====================================================================
		// doc-88 Phase 3 S14 -- the `{nodes, edges, positions}` snapshot,
		// composed from S11 (this graph) + S12 (GraphLayout::LayoutGraph,
		// GraphLayout.h) + S13 (GraphLayoutSidecar, GraphLayoutSidecar.h) --
		// and the layout-position WRITE counterpart, S13's sidecar write's
		// first real (non-test) caller.  docs/gui/NODE_GRAPH_CANVAS.md sect.
		// 6 S14.  Plus a FLAT indexed-accessor surface mirroring
		// TreeNodeCount/TreeRootNode/TreeChildNode/TreeNodeParent/
		// TreeNodeNameByHandle's own split (a REFRESHING count getter, then
		// non-refreshing per-index/per-handle getters reading the snapshot
		// the count call just published) -- the shape `RISE_API_
		// SceneEditController_PainterGraph*` (RISE_API.h) forwards 1:1, the
		// SAME two-surface pattern S11's own AuthoredTree-crossing-the-ABI
		// precedent established: a bulk C++ read (`ReadTree` /
		// `ReadPainterMaterialGraph`) for a shell that can see the nested
		// C++ type (both platform bridges, exactly like `-categoryTree:`
		// already does for AuthoredTree -- see RISEViewportBridge.h's own
		// "one `-categoryTree:` call rather than walked node by node"
		// comment for why a per-node ABI walk is the WRONG shape for a
		// widget's bulk read), plus these flat accessors for a caller that
		// cannot name the nested type at all (the C ABI; see the "NO
		// `ReadTree` AND NO `HandleFor` ON THIS SURFACE" note by the Tree
		// ABI block in RISE_API.h for why).
		//
		// WHERE THE SCENE PATH COMES FROM (the sidecar's own contract needs
		// one): the controller does NOT own a dedicated "current scene path"
		// field.  It reads `mJob.GetCstLoadFileIdentity().filePath` instead
		// -- the SAME FileIdentity the CST save-guard already tracks
		// (FileIdentity.h), refreshed at CST-load (`Job::
		// LoadAsciiSceneViaCst` calls `RefreshCstLoadFileIdentity(filename)`
		// right after a successful load) AND at the end of every successful
		// `RequestSave` (so a Save-As re-anchors it to the new path).  This
		// is exactly "the currently open scene's file path" a sidecar path
		// needs, already threaded through the one place (`Job`) both load
		// and save funnel through -- no new field, no new invalidation
		// surface to keep in sync.  Empty (`FileIdentity::filePath == ""`)
		// on a brand-new, never-loaded-or-saved scene -- GraphLayoutSidecar
		// ::SidecarPathForScene's/WriteSidecar's own "unsaved scene" case,
		// which both ReadPainterMaterialGraphLaidOut (degrades to
		// full-auto-layout, no sidecar) and WriteGraphLayoutPositions
		// (no-ops, returns true) already handle by construction -- see
		// GetCurrentScenePath_'s own comment (SceneEditController.cpp) for
		// the full non-blocking-poll-vs-blocking-write split.
		// =====================================================================

		//! One node's laid-out 2D position -- parallel-indexed to
		//! `PainterMaterialGraphLaidOut::graph.nodes` (positions[i] is
		//! nodes[i]'s position), the SAME parallel-array convention
		//! `GraphLayout::ComputeRanks` already established for this graph
		//! type ("ranks[i] is nodes[i]'s rank" -- GraphLayout.h).
		//!
		//! Deliberately a LOCAL type here, not a reuse of `GraphLayout::
		//! GraphLayoutPoint` -- GraphLayout.h itself `#include`s THIS header
		//! (it takes a `PainterMaterialGraph` by value), so this header
		//! including GraphLayout.h back for its point type would be
		//! circular. Same (x, y) shape; SceneEditController.cpp (which DOES
		//! include GraphLayout.h) converts between the two with a trivial
		//! field copy at the composition boundary.
		struct GraphNodePosition
		{
			double x = 0.0;
			double y = 0.0;
		};

		//! The composed snapshot: S11's PainterMaterialGraph plus a position
		//! for EVERY node -- S13's sidecar filling in whatever it has saved,
		//! S12's LayoutGraph filling in everything else -- exactly the
		//! "{nodes, edges, positions}" shape NODE_GRAPH_CANVAS.md sect. 6
		//! S14 asks for.  No separate edge array: an edge is reachable
		//! through either endpoint's own `outEdges`/`inEdges` (`GraphPort`
		//! already carries both endpoints -- `otherNode`/`otherName` -- plus
		//! the `paramName` label), which is everything S15's canvas needs to
		//! draw a wire (both endpoints + the param label it connects).
		struct PainterMaterialGraphLaidOut
		{
			PainterMaterialGraph graph;
			//! Parallel to graph.nodes; see GraphNodePosition's own comment.
			std::vector<GraphNodePosition> positions;
		};

		//! THE S14 COMPOSITION POINT.  Three steps, in order:
		//!   1. `ReadPainterMaterialGraph` (S11) -- the current graph.
		//!   2. `GraphLayoutSidecar::ReadSidecar` (S13) on the CURRENT
		//!      scene's path (see this block's own "WHERE THE SCENE PATH
		//!      COMES FROM" note) -- whatever positions were saved.  Read
		//!      FRESH from disk on EVERY call, deliberately not cached: the
		//!      sidecar is a tiny (per-node, two-double) JSON file, so a
		//!      per-call read costs nothing worth caching for, and caching
		//!      it would add a THIRD invalidation cadence (beside the
		//!      graph's own compare-then-publish and the scene path's own
		//!      Save-As-can-move-without-a-graph-change hazard --
		//!      GetCurrentScenePath_'s own comment) for no measured benefit.
		//!      A caller that walks many nodes per frame should use the
		//!      bulk read below ONCE per frame (exactly what both platform
		//!      bridges do), not re-call this per node.
		//!   3. `GraphLayout::LayoutGraph` (S12) over the result -- echoes
		//!      every saved position back verbatim and auto-lays-out every
		//!      node the sidecar did not have one for.
		//! `out.positions` is populated parallel to `out.graph.nodes`; a
		//! node with an empty name (should not occur, see `GraphLayout::
		//! LayoutGraph`'s own comment) is left at the default (0, 0).
		void ReadPainterMaterialGraphLaidOut( PainterMaterialGraphLaidOut& out ) const;

		//! One position UPDATE for WriteGraphLayoutPositions -- keyed by
		//! node NAME, not `GraphNodeHandle` (see GraphLayout.h's "NAME-KEYED,
		//! NOT HANDLE-KEYED" note: a handle is generation-tagged and cannot
		//! survive the save/reload a sidecar entry must).
		struct GraphNodePositionUpdate
		{
			String name;
			double x = 0.0;
			double y = 0.0;
		};

		//! THE WRITE COUNTERPART -- the future canvas drag's commit path
		//! (one node moved -> one update here), and S13's `GraphLayoutSidecar
		//! ::WriteSidecar`'s first REAL (non-test) caller.  Merges `updates`
		//! onto whatever the sidecar currently has saved (so moving one node
		//! does not clobber every other node's saved position), then writes
		//! through `WriteSidecar`, which itself:
		//!   - orphan-prunes any name not among the CURRENT graph's own node
		//!     names. This function supplies that live-name set itself, from
		//!     `BuildPainterMaterialGraphSeedsLocked_` called under the SAME
		//!     blocking `mMutex` hold used to read the scene path (doc-88
		//!     S14 review round P2(1)) -- deliberately NOT via
		//!     `ReadPainterMaterialGraph` / `RefreshPainterMaterialGraphSnapshot_`,
		//!     whose non-blocking `try_lock` may serve a STALE published
		//!     graph under contention. Reading the path and the live-name
		//!     set from two SEPARATE critical sections (the prior shape)
		//!     left a window where a node added by a concurrent edit is
		//!     reflected in neither, or in the path but not (yet, or at all,
		//!     under sustained contention) the stale-servable graph read --
		//!     either way this call's own `updates` for that node would get
		//!     silently orphan-pruned by `WriteSidecar` moments after the
		//!     node was created. An explicit user-initiated write cannot
		//!     accept that degradation the way a polled read may;
		//!   - REFUSES (no-ops, returns true, touches no file) when the
		//!     current scene has never been saved -- see this block's own
		//!     "WHERE THE SCENE PATH COMES FROM" note; this function passes
		//!     the path it read straight through rather than special-casing
		//!     empty itself, so the ONE refusal rule lives in ONE place
		//!     (`WriteSidecar`);
		//!   - skips the write entirely when the serialized result is
		//!     byte-identical to the file's current content ("only when
		//!     positions actually changed").
		//! An `updates` entry with an empty name is skipped (nothing to key
		//! a sidecar entry by).  Returns false with `outError` set on an
		//! actual I/O failure, OR when the controller cannot determine the
		//! current scene path right now (a render owns the scene -- same
		//! `mRenderOwnsScene` refusal `GetCurrentScenePath_`'s blocking
		//! branch itself implements; this is the ONE case this function
		//! refuses itself, because an explicit user-initiated write must not
		//! silently no-op the way a POLLED read may).
		bool WriteGraphLayoutPositions( const std::vector<GraphNodePositionUpdate>& updates,
		                                std::string& outError ) const;

		// ---- Flat indexed accessors -----------------------------------
		// Mirror TreeNodeCount/TreeRootNode/TreeChildCountByHandle/
		// TreeChildNode/TreeNodeParent/TreeNodeNameByHandle's own split
		// EXACTLY: PainterGraphNodeCount is the ONLY one that refreshes
		// (`RefreshPainterMaterialGraphSnapshot_`); every other accessor
		// below reads the ALREADY-PUBLISHED `mUi.painterMaterialGraph`
		// under the leaf `mUiSnapshotMutex` ONLY, no refresh -- a caller
		// walking many nodes/ports must call PainterGraphNodeCount ONCE
		// first (exactly the existing Tree-walk contract). Calling it once
		// does NOT, by itself, PREVENT a republish from landing mid-walk --
		// a concurrent thread can call PainterGraphNodeCount (or any other
		// refresh) at any time, and a raw-INDEX accessor like
		// `PainterGraphNodeHandleAt` has no generation of its own to check
		// against, so an index obtained before such a republish can name a
		// different node, or run past a now-shorter array, after one lands.
		// The two guarantees this surface actually has are narrower:
		// (a) every per-HANDLE accessor below (as opposed to per-index)
		// resolves through `ResolveGraphNodeHandle`, which refuses a handle
		// whose encoded generation does not match the graph it is being
		// read against -- so once a caller holds a `GraphNodeHandle`, a
		// later republish makes further per-handle accessor calls for it
		// fail cleanly (`kInvalidGraphNode`-shaped refusal) rather than read
		// a wrong or stale node; and (b) every PRODUCTION caller of this
		// graph (both platform bridges) reads it through the one-shot bulk
		// C++ snapshot (`ReadPainterMaterialGraph` / `ReadPainterMaterialGraphLaidOut`)
		// instead of walking this flat surface index-by-index, so the
		// mid-walk-republish hazard described above does not arise for
		// them at all. This flat indexed surface exists for the C ABI
		// (`RISE_API.cpp`) and is, in practice, exercised only by it and by
		// `tests/ReferenceGraphTest.cpp` -- it is SMOKE-TEST-SCOPED, not a
		// surface any shipped bridge walks this way. `otherNode` on a port accessor is
		// resolved to the OTHER node's real `GraphNodeHandle` (not the raw
		// `GraphPort::otherNode` index) IN THE SAME LOCKED PASS -- one
		// consistent identity space (`GraphNodeHandle`, `kInvalidGraphNode`
		// for "no such node", including a node-less/dangling port) for
		// every accessor below, rather than a second index space a caller
		// would have to know how to dereference.  `portCategories` is
		// DELIBERATELY NOT exposed here -- nothing on this surface needs it
		// yet (S15's canvas draws a wire from endpoint + param label alone;
		// see PainterMaterialGraphLaidOut's own comment) -- a future
		// connection-legality slice (S17) adds an accessor for it WHEN
		// something crossing the ABI actually reads it, not before.

		//! Total nodes in the CURRENT Painter/Material graph.  Refreshes.
		unsigned int PainterGraphNodeCount() const;

		//! The generation of the currently published graph.  Does NOT
		//! refresh (see the block comment).  0 if nothing has ever been
		//! published.
		unsigned long long PainterGraphGeneration() const;

		//! Handle of the `idx`-th node (presentation order, same order
		//! `ReadPainterMaterialGraph` would hand back).  `kInvalidGraphNode`
		//! on an out-of-range index.
		GraphNodeHandle PainterGraphNodeHandleAt( unsigned int idx ) const;

		//! `node`'s name / chunk keyword.  Empty String on an unknown or
		//! stale handle.
		String PainterGraphNodeName( GraphNodeHandle node ) const;
		String PainterGraphNodeKeyword( GraphNodeHandle node ) const;

		//! `node`'s `ChunkCategory`, cast to int -- the SAME "just append,
		//! never reorder" ABI-stability contract the parser's `ValueKind`
		//! already crosses the ABI under (RISEViewportProperty.kind's own
		//! "the parser's ValueKind enum cast to int" comment, RISEViewport
		//! Bridge.h) -- not a bespoke per-value NS_ENUM/QT-enum mirror: a
		//! graph node's category is, in practice, always Painter/Material/
		//! Function today (`BuildPainterMaterialGraphSeedsLocked_`'s own
		//! scope), and a shell that wants a friendly label already has
		//! `chunkKeyword` (e.g. "ramp_painter") without needing to switch on
		//! this value at all.  -1 on an unknown or stale handle (never a
		//! valid `ChunkCategory` ordinal).
		int PainterGraphNodeCategory( GraphNodeHandle node ) const;

		//! `node`'s expression-family def-stage count (S10's `ExpressionProgram
		//! ::DefCount()`; 0 for every non-expression chunk).  -1 on an
		//! unknown or stale handle (never a legitimate defCount).
		int PainterGraphNodeDefCount( GraphNodeHandle node ) const;

		//! `node`'s laid-out position (see ReadPainterMaterialGraphLaidOut).
		//! UNLIKE every other accessor on this surface, this one DOES
		//! perform the S14 composition (sidecar read + LayoutGraph fill-in)
		//! on every call -- position is not part of the compare-then-publish
		//! `PainterMaterialGraph` snapshot the other accessors read (see
		//! this block's own "WHERE THE SCENE PATH COMES FROM" note for why
		//! position/path cannot be folded into that same struct). Acceptable
		//! for an occasional/smoke-test caller; a widget walking every
		//! node's position should call `ReadPainterMaterialGraphLaidOut`
		//! ONCE instead (exactly as RISESceneTreeNode's own comment steers a
		//! bulk walk away from a per-node handle call). Returns false --
		//! `outX`/`outY` untouched -- on an unknown or stale handle.
		bool PainterGraphNodePosition( GraphNodeHandle node, double& outX, double& outY ) const;

		//! Port counts + indexed port accessors for `node`'s two port lists
		//! (S11's `GraphNode::outEdges`/`inEdges` -- "direction" is WHICH of
		//! these two you call, not a separate flag field: mirrors GraphNode's
		//! own out/in split rather than inventing a second encoding of the
		//! same information). 0 / false on an unknown or stale handle, same
		//! degradation as every other accessor here.
		unsigned int PainterGraphNodeOutEdgeCount( GraphNodeHandle node ) const;
		unsigned int PainterGraphNodeInEdgeCount( GraphNodeHandle node ) const;

		//! One port at `portIdx` (0-based, presentation order = `GraphPort`'s
		//! own vector order). `outOtherNode` is `kInvalidGraphNode` for a
		//! node-less port (dangling reference, or a reference out of this
		//! graph's modeled categories -- `GraphPort::otherNode`'s own
		//! comment) -- `outOtherName` still carries the target's name either
		//! way. Returns false -- no out-param touched -- on an unknown/stale
		//! handle or an out-of-range `portIdx`.
		bool PainterGraphNodeOutEdge( GraphNodeHandle node, unsigned int portIdx,
		                              GraphNodeHandle& outOtherNode, String& outParamName,
		                              int& outOccurrence, String& outOtherName ) const;
		bool PainterGraphNodeInEdge( GraphNodeHandle node, unsigned int portIdx,
		                             GraphNodeHandle& outOtherNode, String& outParamName,
		                             int& outOccurrence, String& outOtherName ) const;

		// =====================================================================
		// Node-graph "spotlight" query -- viewport/outliner object pick ->
		// canvas highlight.  A thin composition over TWO already-shipped,
		// already-correct mechanisms (deliberately not a third): the live
		// object->material resolution `SetSelection`'s Object-pick auto-fill
		// already uses (`FindObjectMaterialName`, SceneEditController.cpp,
		// instancing-correct by construction -- see its own comment), and the
		// S11 `PainterMaterialGraph` this class already publishes
		// (`ReadPainterMaterialGraph`) -- the SAME edges the node-graph canvas
		// itself draws from.  "Never disagree with what the canvas shows" only
		// holds WITHIN one call: the two reads below are two SEPARATE snapshot
		// acquisitions (the live-manager name resolution, then a possibly-later
		// `ReadPainterMaterialGraph` refresh), so a concurrent structural edit
		// landing in the gap between them (a rename, or a duplicate-name chunk
		// being added/removed) can, in principle, make this call's answer stale
		// by one edit relative to what the canvas renders a moment later --
		// review-round P1 fix note: this is an eventual-consistency window, not
		// a "never" guarantee.
		// =====================================================================

		//! One entry in an `AppearanceClosureForObject` result: a node
		//! IDENTITY, not just a display string -- (category, name), the SAME
		//! two fields `SceneReferenceGraph::ResolveChunk`/`GraphNode` already
		//! use to address a node, because a bare name is NOT unique here.
		//! Two DIFFERENT-category chunks (a Painter and a Material) may
		//! legally share a name; a caller matching by name alone against a
		//! mixed-category node list (exactly what `PainterMaterialGraph::nodes`
		//! is) can silently spotlight the wrong node (review-round P1 fix).
		//! `category` is `RISE::ChunkCategory` cast to int -- the SAME ordinal
		//! `GraphNode::category`/`RISEGraphNode.category` already use (Painter
		//! 0, Function 1, Material 2 -- the only three this graph models), so
		//! a caller matches an entry against an already-fetched
		//! `PainterMaterialGraph`/`RISEPainterMaterialGraph` node by
		//! `(category, name)` directly, no cast or remapping needed.
		struct AppearanceClosureEntry
		{
			ChunkCategory category = ChunkCategory::Painter;
			String        name;
		};

		//! For `objectName` (an Object-category chunk name -- a
		//! `standard_object`/`csg_object`, addressed the same way
		//! `selectionRowName` resolves an instancing pick back to its own
		//! chunk, NOT a synthesized per-repetition/per-subtree-member name
		//! like `I[1,0]`/`I.child`, neither of which is ever an addressable
		//! chunk), returns the chunk (category, name) identities to highlight
		//! on the Painter/Material node-graph canvas: the object's bound
		//! material first (index 0), then every node transitively reachable
		//! from it in the published `PainterMaterialGraph` -- Painter,
		//! Function, AND Material nodes alike (so a `composite_material`'s
		//! `top`/`bottom` sub-materials, and their own painter chains, are
		//! included, not just the primary material's own direct painter
		//! references) -- in BFS discovery order.
		//!
		//! Empty when: `objectName` does not name a live, registered Object
		//! (unknown name, or a Painter/Material/other-category name passed by
		//! mistake); the object has no material bound (`material none`/unset,
		//! or a container node with no `geometry` to bind one to at all); the
		//! resolved material name does not appear in the current graph at
		//! all; the resolved material name is AMBIGUOUS in the current graph
		//! -- more than one `ChunkCategory::Material` node shares it (the CST
		//! Document tolerates two same-name Material chunks even though the
		//! live `IMaterialManager` a real derive registers into cannot -- see
		//! `BuildPainterMaterialGraphSeedsLocked_`'s own "two same-category
		//! chunks... each gets its OWN node" comment; this method REFUSES
		//! rather than guess which one `FindObjectMaterialName`'s live answer
		//! actually meant, matching `SceneReferenceGraph::ResolveChunk`'s own
		//! established "ambiguous name -> refuse" convention, review-round P1
		//! fix); or the controller could not get a non-blocking hold of the
		//! commit lock right now (a render owns the scene, or another editor
		//! operation is already inside it) -- see this method's own `.cpp`
		//! comment for the locking discipline, mirrored from
		//! `ResolveTreeRowName`/`SelectionRowName`'s established
		//! try-lock-and-degrade-to-empty pattern; a polling caller (both
		//! platform canvases already poll on every selection-observing pass)
		//! retries on the next call rather than blocking the UI thread behind
		//! a render.
		//!
		//! `outDegraded` (later external review round): an empty `result` is
		//! OVERLOADED -- it means EITHER a genuinely resolved answer (unknown
		//! object, no material bound, ambiguous material) OR that this call
		//! could not even ATTEMPT a real answer because the commit lock was
		//! contended. A caller that polls (both platform canvases do) needs
		//! to tell these apart: a genuine empty answer should be accepted and
		//! NOT retried; a degraded one should be retried once the lock is
		//! likely free again. When non-null, `*outDegraded` is set to `true`
		//! ONLY on the `mRenderOwnsScene`/`try_to_lock` refusals above, and
		//! `false` on every other return path -- INCLUDING a successful walk
		//! that finds nothing reachable. Defaults to `nullptr` (every
		//! existing caller, including every test written before this
		//! parameter existed, is unaffected).
		//!
		//! `objectName` naming an INSTANCING chunk (a `standard_object` with
		//! its own `source X` and no `material` of its own) resolves to the
		//! SAME material `X` itself would: this query reads the LIVE
		//! post-derive `IObject`'s bound `IMaterial` (`FindObjectMaterialName`),
		//! never the instancing chunk's own CST text -- which, absent an
		//! explicit `material` override, never has a `material` line at all
		//! (`Cst::DeriveToJob` PASS-2 merges the source's bindings into the
		//! Job-facing bag at DERIVE time, never into the retained Document;
		//! see `standard_object`'s own descriptor comment on `source`).
		std::vector<AppearanceClosureEntry> AppearanceClosureForObject(
			const String& objectName, bool* outDegraded = nullptr ) const;

		// =====================================================================
		// Node-graph FOCUSED-VIEW read -- user-requested slice: the canvas
		// toggles between "show all nodes" (ReadPainterMaterialGraphLaidOut,
		// above) and "show only the selection's subgraph". Subgraph
		// selection AND layout live here in C++ (this class's own C4
		// convention: logic in the shared core, shells stay thin), not
		// duplicated per platform.
		// =====================================================================

		//! Focused variant of `ReadPainterMaterialGraphLaidOut`: `out.graph`
		//! contains ONLY the subgraph rooted at `(cat, name)`, laid out
		//! fresh. Subgraph definition:
		//!   - `cat == ChunkCategory::Object`: the object's APPEARANCE
		//!     CLOSURE -- exactly `AppearanceClosureForObject`'s own
		//!     resolution (live bound material, instancing-correct) and BFS
		//!     (shared code, not a second walk) -- the object's bound
		//!     material plus its full transitive Painter/Function/Material
		//!     closure.
		//!   - `cat == Painter/Function/Material` (a canvas node): that
		//!     node plus its own transitive closure in the SAME direction
		//!     (`BFSGraphClosure`'s outEdges walk -- a node's "inputs",
		//!     the identical direction the Object case walks from a
		//!     material). Nodes that only REFERENCE the selected node
		//!     (downstream referrers, reachable via inEdges, never
		//!     followed) are EXCLUDED from the subgraph -- this is a
		//!     narrower view than `AppearanceClosureForObject` would ever
		//!     produce for the same node, by design (an "upstream/inputs
		//!     only" focus, not "everything touching this node").
		//! `name` unknown, or (for the Object case) the object's bound
		//! material AMBIGUOUS/unresolved, or (for the non-Object case)
		//! `(cat, name)` itself ambiguous/not-found -- empty `out.graph`,
		//! same refusal convention as `ResolveUniqueGraphNodeIndex`.
		//!
		//! `outDegraded`: set true ONLY when the Object-category case's
		//! live object->material resolution could not get a non-blocking
		//! hold of the commit lock (a render owns the scene) -- see
		//! `AppearanceClosureForObject`'s own `outDegraded` comment for the
		//! full contract; identical semantics here. The non-Object case
		//! never degrades this way (`ReadPainterMaterialGraph` itself never
		//! blocks; under contention it serves a stale published snapshot,
		//! not an empty/failed read), so `*outDegraded` is always `false`
		//! for a Painter/Function/Material `cat`.
		//!
		//! LAYOUT IS TRANSIENT (design decision, NOT an oversight): unlike
		//! `ReadPainterMaterialGraphLaidOut`, this NEVER reads or writes the
		//! `.risegraph.json` sidecar -- `GraphLayout::LayoutGraph` runs
		//! against an always-empty saved-positions map, fresh on every
		//! call, so a focused subgraph's layout can never leak into, or be
		//! polluted by, the persisted all-view layout. Toggling back to the
		//! all-view reads the sidecar exactly as it stood before any
		//! focused excursion.
		void ReadPainterMaterialGraphLaidOutFocused( ChunkCategory cat, const String& name,
		                                              PainterMaterialGraphLaidOut& out,
		                                              bool* outDegraded = nullptr ) const;

		//! Monotonic counter — set ONCE at controller construction from
		//! a process-global atomic that increments per `SceneEditController`
		//! instance.  Each fresh controller therefore has a unique
		//! epoch, which platform UIs cache against `(epoch, category)
		//! → entity-name list` to detect scene reload (the GUI tears
		//! down + recreates the bridge, which builds a new controller).
		//!
		//! ALSO bumped on mid-session structural mutations that change
		//! the entity set: CloneActiveCamera, gizmo/transform commits,
		//! transaction rollback, selection changes, and — since the
		//! entity-creation slice — every landed agent chunk CRUD
		//! (ApplyAgentChunkCrud_, i.e. InstantiateEntityTemplate /
		//! DuplicateEntity / RemoveEntity and agent insert_chunk/
		//! remove_chunk).  So a GUI outliner that caches `(epoch,
		//! category) → entity-name list` and re-enumerates on an epoch
		//! change picks up adds/removes/duplicates live, on either
		//! platform, without a bespoke per-op refresh signal.
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
		//! chunk was declared in the .RISEscene file.  Returns false without
		//! touching the outputs while a render owns the scene or another editor
		//! operation holds the commit mutex; polling callers retain their last
		//! successful tuple and retry.
		bool GetAnimationOptions( double& timeStart, double& timeEnd,
		                          unsigned int& numFrames ) const;

		//! Read whether the live scene currently contains any keyframed
		//! elements.  Agent edits can add or remove timelines after the GUI
		//! has loaded, so platform shells poll this alongside the animation
		//! options instead of treating the load-time answer as permanent.
		//! Returns false without touching `hasAnimation` while a render owns
		//! the scene or another editor operation holds the commit mutex; the
		//! caller should retain its last successful snapshot and try again.
		bool GetHasAnimation( bool& hasAnimation ) const;

		//! doc 88 S10 (Tier-2 panel affordances): headless RGBA8 preview of
		//! a named painter -- see PainterPreview.h for the full domain /
		//! display-encode / scalar-normalization contract, which this
		//! method does not repeat.  `defIndex < 0` previews the painter's
		//! own output (colour or scalar pipe, whichever it resolves in);
		//! `defIndex >= 0` previews that expression `def` slot's
		//! intermediate stage (refused for a non-expression painter or an
		//! out-of-range index).  Fills `outRGBA` (resized to exactly
		//! `w*h*4` bytes on success, row-major top-to-bottom) and, when the
		//! previewed stage is SCALAR-typed, the auto-range actually applied
		//! (`outWasScalar`/`outRangeMin`/`outRangeMax` -- all optional,
		//! pass null to skip; untouched for a colour-typed stage). Returns
		//! false (outRGBA cleared) on an unresolved name, w/h outside
		//! (0, PainterPreview::kMaxDim], an out-of-range/inapplicable
		//! defIndex, or the same render-owns-scene / contended-lock
		//! refusal GetAnimationOptions above documents (non-blocking
		//! try_lock -- PainterPreview.h's CONCURRENCY note).
		bool GetPainterPreview( const String& painterName, int defIndex,
		                        unsigned int w, unsigned int h,
		                        std::vector<unsigned char>& outRGBA,
		                        bool* outWasScalar = nullptr,
		                        double* outRangeMin = nullptr,
		                        double* outRangeMax = nullptr ) const;

		//! doc 88 S10: a ramp_painter's own colour interpolation over its
		//! authored stop domain, as a horizontal gradient strip -- see
		//! PainterPreview::RenderRampStripPreview's doc comment.  Same
		//! fill/refusal contract as GetPainterPreview above; additionally
		//! refuses when `painterName` does not resolve to a ramp_painter.
		bool GetRampStripPreview( const String& painterName,
		                          unsigned int w, unsigned int h,
		                          std::vector<unsigned char>& outRGBA ) const;

		// (Named animations are a first-class accordion Category —
		// Category::Animation; the generic CategoryEntityCount/Name,
		// CategoryActiveName and SetSelection surface lists + activates
		// them, so no bespoke per-feature accessors are needed here.
		// GetAnimationOptions above already follows the active animation.)

		//! Toolkit slice 1 (read_viewport): copy the CURRENT live interactive
		//! viewport pixels out of `mInteractiveFrameStore` -- the exact frame
		//! the user is looking at right now -- WITHOUT triggering a render.
		//! Fills `outPixels` (row-major, linear radiance, width*height) and
		//! `outWidth`/`outHeight`; returns true iff a frame was available.
		//!
		//! Thread-safe and coherent: the pointer is snapshot-and-addref'd
		//! under `mInteractiveFrameStoreMutex` (so a concurrent
		//! `EnsureInteractiveFrameStore_` reallocation can't free it
		//! mid-read), then the pixels are copied via
		//! `AsBeautyRasterImage().DumpImage()` -- which acquires EVERY tile's
		//! shared_lock up front, the SAME complete-coherent-snapshot mechanism
		//! `ViewportFrameStore::SaveAs` uses -- so an in-flight interactive
		//! render pass writing tiles cannot tear the copy.  Returns false
		//! (leaving outputs empty/zero) when no interactive frame store exists
		//! yet (no render has run) or it has zero dims.  Callable from any
		//! thread (e.g. the agent RPC thread) concurrently with the render
		//! thread.
		//! `outSourcePane` (optional) receives, ATOMICALLY with the frame,
		//! which pane's pixels the copy holds -- captured under the same
		//! frame-store leaf lock as the store snapshot (user-review P1#3),
		//! so the returned image and its pane label always agree even if
		//! the scheduler advances immediately after.
		bool CopyInteractiveFrame( std::vector<RISEColor>& outPixels,
		                           unsigned int& outWidth,
		                           unsigned int& outHeight,
		                           unsigned int* outSourcePane = nullptr ) const;

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

		//! Increments on every CancelAgentRender_() invocation -- the
		//! agent-teardown/drain cancel path, which deliberately does NOT
		//! bump mCancelCount (that counter belongs to the UI-mutation
		//! CancelAndParkRender_ idiom).  Added for AgentRenderAsyncTest's
		//! no-stale-id red-prove: a stale mAsyncOutstandingJobId makes
		//! ~AgentSession's drain call CancelAgentRender_() against an
		//! unrelated render, and THIS counter is the only signal that
		//! observes that call directly.
		unsigned int ForTest_GetAgentCancelRequestCount() const;

		//! Increments at the start of each render-loop iteration
		//! that actually fires a render pass.
		unsigned int ForTest_GetRenderCount() const;

		//! Block until the render thread has run at least the given
		//! number of completed render passes since Start().  Returns
		//! false on timeout.  Used by the cancel-restart test to wait
		//! for settling without sleep-polling.
		bool ForTest_WaitForRenders( unsigned int count, unsigned int timeoutMs );

		//! Test-only: the render-owns-scene guard flag (true while a
		//! production/agent render holds mMutex across its closure).  Lets a
		//! test PROVE a guarded UI method no-ops (not wedges) mid-render and
		//! that the flag clears afterward.
		bool ForTest_RenderOwnsScene() const { return mRenderOwnsScene.load( std::memory_order_acquire ); }

		//! P3c: the pane whose content the interactive frame store
		//! currently holds -- i.e. which pane a CopyInteractiveFrame /
		//! agent read_viewport actually returns.  Public and honest (not a
		//! test seam): the agent surface reports it so a multi-pane
		//! viewport read is attributable.
		unsigned int CurrentRenderPane() const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			return mCurrentPane;
		}

		//! P3a slice 2 test seam: drive the render loop the way an edit
		//! does (KickRender is private; scheduler tests need the edit
		//! trigger without performing a real scene mutation).
		void ForTest_KickRender() { KickRender(); }

		//! P3a slice 2 test seam: which pane's registers are currently
		//! loaded (the scheduler's context).  Read under mMutex for a
		//! settled answer; test-only, like every ForTest_* accessor.
		unsigned int ForTest_CurrentPane() const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			return mCurrentPane;
		}

		//! T0 polish-order oracle: whether `pane` owns the
		//! FinalRegularRunning marker in either the live register or its
		//! saved slot.  Lets the forced release-interleaving test verify
		//! ownership, not merely a scheduler-dependent eventual pass count.
		bool ForTest_PaneHasFinalRegularPolish( unsigned int pane ) const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			if( pane >= kViewportPaneCount ) return false;
			const int state = ( pane == mCurrentPane )
				? mPolishState.load( std::memory_order_acquire )
				: mPaneRender[pane].polishSaved;
			return state == static_cast<int>( PolishState::FinalRegularRunning );
		}

		//! Round-8 test seam: shorten the pointer-gesture watchdog so the
		//! regression test does not have to wait kPointerWatchdogMs.
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

		//! Round-8 test oracle: has the render thread declared the live
		//! pointer gesture abandoned (see mPointerGestureStale)?
		bool ForTest_PointerGestureStale() const
		{
			return mPointerGestureStale.load( std::memory_order_acquire );
		}

		//! Round-8 test oracle: the raw pointer-gesture flag.  The watchdog
		//! must leave this SET while marking the gesture stale, so a late
		//! pointer-up still finalizes instead of early-returning.
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

		//! ROUND-10 finding 3 test seam.  Trip mInteractionPersistenceFailed
		//! -- the STICKY flag a failed pending-CST commit sets -- so a test
		//! can exercise the PERMANENT FinalizeOpenInteractions refusal
		//! (RenderRefusal::InteractionFinalizeLatched) without having to
		//! stage a CST route failure or a vanished dragged camera.  There is
		//! deliberately NO un-trip: the production flag is never cleared
		//! either, and a test that needs a clean controller builds one.
		void ForTest_TripInteractionPersistenceFailure()
		{
			mInteractionPersistenceFailed.store( true, std::memory_order_release );
		}
		//! ROUND-10 finding 3 test seam, the TRANSIENT twin of the above.
		//! Makes the NEXT FinalizeOpenInteractions() call return false
		//! WITHOUT setting the sticky flag, i.e. exactly the retriable
		//! RenderRefusal::InteractionFinalizeFailed shape, then clears
		//! itself.  Needed because the production transient causes (an
		//! OnTimeScrubEnd failure, a scrub/composite that survives its own
		//! finalize) have no deterministic black-box trigger, and an
		//! untested arm of a model-facing retriability mapping is a claim,
		//! not a fact.  Consulted at exactly one point, immediately before
		//! FinalizeOpenInteractions' final return, so it cannot skip any of
		//! the real finalization work.
		void ForTest_FailNextFinalizeOpenInteractions()
		{
			mForTestFinalizeFailOnce.store( true, std::memory_order_release );
		}
		//! T0 pause-state oracle setup: arm the current pane's normal
		//! final-to-polish transition without synthesizing a pointer gesture.
		void ForTest_ArmFinalRegularPolish()
		{
			std::lock_guard<std::mutex> lk( mMutex );
			mPolishState.store(
				static_cast<int>( PolishState::FinalRegularRunning ),
				std::memory_order_release );
		}

		const SceneEditor& Editor() const { return mEditor; }
		SceneEditor&       Editor()       { return mEditor; }

		// Phase 6.5 (docs/ROUND_TRIP_SAVE_PLAN.md §9.9): save the scene
		// by serializing the Job's retained CST Document whole
		// (SaveEngine::Save -> Cst::SerializeCst) to a `.RISEscene`
		// file: an external-modification guard refuses an in-place
		// save when the loaded file changed on disk after load, the
		// write is atomic (temp + rename), and the engine NoOps when
		// the serialized bytes equal the on-disk file. Dirty state does
		// not select WHAT is written (the whole Document snapshot always
		// is); it only gates the GUI Save button. Follows the lock-free
		// disk-IO sequence:
		//   1. Acquire mMutex, cancel in-flight render, wait for
		//      mRendering=false, snapshot Document/file identity/head,
		//      set mSaving=true, release mMutex.
		//   2. Run SaveEngine::Save on the immutable snapshots outside
		//      the lock (file IO is slow).
		//   3. Reacquire mMutex, clear mSaving, surface any error,
		//      clear dirty only if the live head still equals the
		//      serialized snapshot, and notify the render loop.
		// `filePath` is the target .RISEscene to write — typically
		// the originally-loaded path, but the caller can redirect for
		// Save-As.  Returns the SaveResult so the UI can show the
		// outcome (status + counters + error / warning messages).
		SaveResult RequestSave( const std::string& filePath );

		//! True iff a save is currently in flight on disk.  The render
		//! loop's wake condition consults this so a new render pass
		//! doesn't start mid-save. The save reads immutable Document
		//! state, but retaining the gate avoids competing IO/render work
		//! and preserves viewport scheduling. Mirrors mRendering in the
		//! opposite direction.
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

		// Editor live-sync (UI refinement item 1) -----------------------
		// The Scene-file editor mirrors the live CST: every committed
		// mutation (GUI edit, agent edit, undo/redo) is visible as text
		// without a manual sync step.

		//! The retained CST Document's serialization — the exact bytes
		//! RequestSave would write.  Empty when no document is retained.
		//! Takes mMutex (a commit on another thread must not be observed
		//! half-applied), so CALLERS MUST NOT POLL DURING A RENDER — the
		//! render worker holds mMutex for the render's whole duration
		//! and this would wedge the caller (the shells gate their poll
		//! on the scene-editable predicate, same as the proposals poll).
		String SerializedSceneText() const;

		//! Cheap change detector for the editor's poll: the retained CST
		//! head version (uuid fresh per load; revision bumps iff content
		//! changed).  Same mMutex caveat as SerializedSceneText — the
		//! 16-byte version must not be read torn.  Both 0 when no
		//! document is retained.
		void GetSceneTextVersion( std::uint64_t& outUuid,
		                          std::uint64_t& outRevision ) const;

		//! The AGENT surface's read of the head: an ATOMIC
		//! {hasDocument, document, headVersion} capture under ONE mMutex hold.
		//!
		//! read_document and validate's head form report byte offsets INTO a
		//! document ALONGSIDE the headVersion those offsets are supposed to
		//! describe.  Reading the three separately -- as three independent
		//! unlocked AgentSession accessor calls -- lets a commit on another
		//! thread land between them, so the answer pairs one revision's
		//! offsets with another revision's stamp, and the serialization
		//! itself races the mutation.  One lock, one snapshot, no window.
		//!
		//! Unlike SerializedSceneText / GetSceneTextVersion above (UI POLLS:
		//! render-owns-scene guard + try-lock, silently serving nothing when
		//! busy) this BLOCKS on mMutex.  A poll that skips a beat is
		//! harmless; an agent read that answers "" / {0,0} while a head
		//! exists is a lie the model cannot detect.  So: AGENT RPC THREAD
		//! ONLY -- never from a UI frame callback, and never from inside a
		//! render closure (mMutex is non-recursive -- see
		//! RunPreviewRenderParked's `fn` contract).
		//!
		//! DISCLOSED LIVENESS COST of choosing to block.  The interactive
		//! render loop takes only brief per-pass mMutex holds and a
		//! cancel-and-park commit is bounded, so the ordinary wait is short.
		//! The long one is an ASYNC AGENT RENDER (`render {async:true}` ->
		//! AgentSession::RenderAsync), whose worker holds mMutex for the
		//! render's whole duration: a read verb issued from ANOTHER thread
		//! while one is in flight waits for it to finish.  That is a
		//! deliberate trade -- a slow honest answer over a fast wrong one --
		//! but it means a driver on the UI thread should not dispatch read
		//! verbs while it has an async render outstanding (poll render_status
		//! / render_wait, which take mJobStatusMutex and never block on
		//! mMutex).  mMutex is also a BARGING lock: under a sustained commit
		//! storm a reader can wait through many commits before winning it.
		//!
		//! TERMINAL LIFECYCLE.  Refuses BEFORE borrowing mJob (reporting the
		//! "no head" answer: false / "" / {0,0}) once ~SceneEditController or
		//! PrepareForDestruction has claimed the controller -- the same test
		//! StageProposal and the agent-edit funnels' destroying arm make, and
		//! for the same reason: past that point the owner may already have
		//! released the Job.  Checked on the atomics alone, so it NARROWS the
		//! window rather than closing it; owner quiescence is still the real
		//! guarantee, exactly as the gate-publication sites say.
		void ReadAgentSceneSnapshot( bool& outHasDocument,
		                             std::string& outDocument,
		                             RISE::Cst::CstHeadVersion& outVersion ) const;

		//! The head-version half of ReadAgentSceneSnapshot without paying
		//! for the serialization -- same lock, same thread contract.  For
		//! the result-stamping sites that need only the version.
		RISE::Cst::CstHeadVersion ReadAgentHeadVersion() const;

		//! "Reveal in scene file" (the design comp's ⌗ affordance): resolve
		//! ENTITY (cat, name) to WHERE it sits in SerializedSceneText() --
		//! its top-level chunk's byte offset and 1-based line number.
		//! Resolution mirrors the SAME DocFindByNameAnyRole call + role-
		//! kind-suffix / unique-fallback convention CaptureAgentPriorParamValue_
		//! and Job::ApplyCstParamEditImpl_ use for entity addressing (see the
		//! RoleKindSuffixForCategory table at the .cpp definition) -- so a
		//! resolvable name here is guaranteed to be the SAME chunk an agent
		//! edit against (cat, name) would land on.  Categories with no CST
		//! chunk-name addressing scheme (Rasterizer/Film -- their
		//! CategoryEntityName/CategoryActiveName values are registry TYPE
		//! names or a synthetic preset label, not a chunk `name` param) and
		//! Category::None always return false.
		//! Takes mMutex -- SAME caveat as SerializedSceneText: CALLERS MUST
		//! NOT POLL DURING A RENDER (the render worker holds mMutex for the
		//! render's whole duration; gate on the scene-editable predicate).
		//! Returns false (outputs unchanged) when: no retained Document, the
		//! name doesn't resolve (absent or ambiguous under DocFindByNameAnyRole),
		//! or the resolved chunk is not (anymore) a top-level item.
		//! COST: O(log N) to resolve the chunk's position (DocFindByNameAnyRole
		//! is an O(N) name scan today -- see its own doc comment -- followed
		//! by O(log N) DocIndexOfNodeId + DocByteOffsetOfItem), plus an O(doc
		//! bytes) newline count over the serialized PREFIX to turn the byte
		//! offset into a line number (SerializeCst has no partial/prefix-only
		//! form).  Fine at click/selection cadence (this is not a per-frame
		//! or per-keystroke path); not something to poll in a loop.
		bool EntitySourceLocation( Category cat, const String& name,
		                          std::uint64_t& outByteOffset, std::uint32_t& outLine ) const;

		//! -------- Source traceability: any UI element -> its scene-file span -----
		//!
		//! Generalizes EntitySourceLocation from "a named entity's chunk" to "any
		//! scene-derived UI element", at three granularities, keyed by the SAME
		//! (Category, name, param) address the CST EDIT path uses -- so what a UI
		//! element can REVEAL is exactly what it can EDIT (they resolve through one
		//! CST-node path and cannot drift).  A byte RANGE (offset+length) lets the
		//! editor highlight the exact span; line/column are 1-based.
		//!
		//! Resolution of (cat, name) -> chunk covers the named categories (Object /
		//! Light / Material / Medium / Painter / Animation / SceneVariant), the
		//! unnamed-active Camera (unique-fallback), AND the unnamed unique-in-kind
		//! singletons Film and Rasterizer (which EntitySourceLocation /
		//! RoleKindSuffixForCategory reject) -- so the Environment section reveals
		//! its radiance_* rows via (Rasterizer, "", "radiance_scale"...) and its
		//! HDRI file via (Painter, <bound painter>, "file").  Reference-following is
		//! the WIDGET's job: it already knows the referenced entity's name, so it
		//! builds the ref for the referenced chunk directly (no indirection here).
		//!
		//! `param` empty = whole-chunk span (byteLength 0; highlight the line);
		//! non-empty = the `occ`-th (0-based) matching param's tight `role value…`
		//! run.  Returns false (out.present stays false) on no retained CST, an
		//! unresolvable ref (removed entity / bad param), or a session-only element
		//! whose widget shouldn't have called this.  Same mMutex / no-poll-during-
		//! render caveat + O(doc bytes) line-count cost as EntitySourceLocation.
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

		//! Reverse: the UI element whose scene-file source contains byte `offset`
		//! (a text-editor cursor / selection) -> fills (outCat, outName, outParam)
		//! so the caller can select + highlight that element.  outParam is empty
		//! when the offset is inside a chunk but not on a specific param;
		//! `*outOccurrence` (if non-null) is the 0-based occurrence index of that
		//! param among its same-role siblings (0 for a non-repeated param / no
		//! param), so a click inside the k-th repeat round-trips to
		//! ResolveSourceSpan(...,occ=k).  For the Rasterizer category outName is the
		//! rasterizer KIND (its keyword) -- rasterizer chunks are unnamed and
		//! addressed by kind -- so a reverse ref into a non-active rasterizer chunk
		//! forward-resolves to that SAME chunk.  Returns false when there is no
		//! retained CST or the offset isn't inside an addressable chunk (inter-chunk
		//! trivia, a non-entity chunk kind, EOF).  Completes the round-trip (text ->
		//! UI) using the CST's byte->node map (DocParamAtByteOffset /
		//! DocItemAtByteOffset).
		bool SourceRefAtByteOffset( std::uint64_t offset, Category& outCat,
		                            String& outName, String& outParam,
		                            int* outOccurrence = nullptr ) const;

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
		//! THREADING CONTRACT (document-first phase 1): the listener fires on
		//! whichever thread drains the mutation, with NO controller or
		//! notification lock held. Synchronous non-lifecycle re-entry is
		//! supported.  Raw `delete` and C-API Destroy/Prepare from the callback
		//! (or a copied callback-target destructor) are NOT supported: C lifecycle
		//! calls fail closed, and the owner must delete after the callback returns.
		//! Platform bridges should still marshal UI work to their event queue for
		//! thread affinity. Transitions are coalesced: consume the reported value
		//! rather than counting calls.
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
		bool         PropertyHasRangeFor( Category cat, unsigned int idx ) const;
		double       PropertyRangeMinFor( Category cat, unsigned int idx ) const;
		double       PropertyRangeMaxFor( Category cat, unsigned int idx ) const;
		double       PropertyRangeStepFor( Category cat, unsigned int idx ) const;

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

		//! doc 88 S4b (Tier-1 param sliders): the row's authored numeric
		//! RANGE, if it declared one.  `PropertyHasRange` is true only
		//! when BOTH bounds are known -- a shell must not draw a slider
		//! otherwise, and must keep its text field either way (the
		//! range is a presentation hint, not a validation rule: the
		//! value still round-trips as text through SetProperty, and the
		//! scene language accepts values outside the hint).  Step is 0
		//! when the author declared none ("continuous").  The min/max/
		//! step readers return 0 for a row with no range, which is why
		//! `HasRange` is a separate question and not an in-band 0/0.
		//!
		//! Populated today only for an expression painter's `param[i]`
		//! rows, from the `min` / `max` / `step` metadata on the scene
		//! text's `param` line (ExpressionParamSpec).
		bool   PropertyHasRange( unsigned int idx ) const;
		double PropertyRangeMin( unsigned int idx ) const;
		double PropertyRangeMax( unsigned int idx ) const;
		double PropertyRangeStep( unsigned int idx ) const;

		//! Jump-to-definition (GUI redesign, 2026-07-22): for a
		//! ValueKind::Reference row whose value names another element,
		//! resolve WHICH UI category that element lives in so the shell
		//! can SetSelection(outCat, outName) -- the "right-click a
		//! reference, jump to its definition" affordance.  Resolution
		//! probes the row's descriptor-declared referenceCategories
		//! against the LIVE managers first-wins (the same order Cst.cpp's
		//! ComputeChunkRefs resolves reference edges).  Returns false for
		//! a non-Reference row, an empty/unset value, or a value that
		//! doesn't currently name an element in any declared category
		//! (dangling reference -- the shell greys the menu item).
		//! Indexes the PRIMARY snapshot; the For twin indexes the
		//! per-category snapshot (same convention as every accessor pair
		//! above).
		bool PropertyJumpTarget( unsigned int idx, Category& outCat, String& outName ) const;
		bool PropertyJumpTargetFor( Category cat, unsigned int idx, Category& outCat, String& outName ) const;

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
		//! promote the clone to active. `proposedName` is canonicalized to a
		//! CST-safe identifier before a numeric dedup suffix is appended, so
		//! the call always succeeds when there IS an active camera to clone.
		//! The chosen name is written into
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

		//! B3 fly-then-stamp (Tier 2 §5.3): promote the CURRENT transient
		//! free-fly ViewportPose into a NEW named scene camera.  The single
		//! B3 action that mutates the scene — an AddCamera transaction seeded
		//! from the pose (which IS a CameraSnapshot) instead of the active
		//! camera; it persists via the retained CST Document exactly like
		//! CloneActiveCamera, and is undoable. `proposedName` is canonicalized
		//! to a CST-safe identifier before deduplication; `outName` receives the
		//! chosen name. Returns false — outName cleared — when
		//! free-fly isn't active (nothing transient to stamp), there's no
		//! camera manager, the buffer is too small, or the edit is refused.
		//! Non-destructive navigation (fly/snap/Home) stays pose-only; THIS is
		//! the deliberate bridge to a durable camera.
		bool StampViewToNewCamera( const String& proposedName,
		                           char* outName, unsigned int outLen,
		                           unsigned int pane = kViewportNavPrimary );

		//! -------- Free-fly viewport pose (Tier 2 / Direction B §5.3-5.5) --------
		//!
		//! A transient, viewport-private "pose" the interactive preview renders
		//! THROUGH (via a render-camera override, §5.5) so navigating / snapping /
		//! restoring a named view never mutates Scene::pActiveCamera and never
		//! touches production render.  The pose payload IS a CameraSnapshot (pose +
		//! full optics + kind), the same value the clone/named-view paths use.
		//! Entering free-fly seeds the pose from the current active camera; while
		//! active, the interactive pass renders through the override; exiting
		//! reverts to rendering through the scene's active camera.  NONE of these
		//! is a scene mutation: no SceneEdit, no revision bump, no undo entry
		//! (identical cost/side-effect profile to today's active-camera navigation,
		//! but non-destructive).  Stamp/promote (a later slice) is the only action
		//! that writes a scene camera.  See docs/gui/CAMERAS_AND_VIEWS.md.

		//! user-review P1#6 (round 2): the free-fly funnel takes an explicit
		//! target pane so the two callers with DIFFERENT intents stay separated:
		//! the nav overlay (drawn on the primary pane) targets the PRIMARY pane
		//! -- signalled by the default `kViewportNavPrimary` sentinel, resolved
		//! to `mPrimaryPane` under the lock -- while the pane-0 alias forwarders
		//! (PaneEnterFreeFly(0), Set/GetPaneVantage*(0)) pass an EXPLICIT 0 so
		//! they keep operating on pane 0 regardless of which pane is primary.
		//! In single-viewport mode mPrimaryPane==0, so the default path is
		//! byte-for-byte the classic behavior.
		static constexpr unsigned int kViewportNavPrimary = 0xFFFFFFFFu;

		//! Enter free-fly: capture the active camera into the transient pose and
		//! realize the override.  Returns false when there is no active camera or
		//! its kind isn't realizable (same clonability gate as CloneActiveCamera).
		//! Cancel-and-parks (swaps the render-thread-read override under the lock).
		bool EnterFreeFlyFromActiveCamera( unsigned int pane = kViewportNavPrimary );

		//! Set the transient pose explicitly (e.g. a named-view restore or an
		//! axis snap computes the target pose).  Realizes a fresh override camera
		//! from `pose`.  Returns false when the pose kind isn't realizable / no
		//! film.  Cancel-and-parks.
		bool SetViewportPose( const CameraSnapshot& pose, unsigned int pane = kViewportNavPrimary );

		//! Exit free-fly: drop the transient pose + release the override so the
		//! interactive pass renders through Scene::pActiveCamera again.  No-op
		//! (returns false) when free-fly isn't active.  Cancel-and-parks.
		bool ExitFreeFly( unsigned int pane = kViewportNavPrimary );

		//! Whether a transient viewport pose is currently overriding the
		//! interactive render camera.
		bool IsFreeFlyActive( unsigned int pane = kViewportNavPrimary ) const;

		//! Copy the current transient pose into `out`.  Returns false when
		//! free-fly isn't active (out untouched).
		bool GetViewportPose( CameraSnapshot& out, unsigned int pane = kViewportNavPrimary ) const;

		//! -------- Axis snaps + Home (Tier 2 / Direction B §4.2) --------
		//!
		//! Non-destructive VIEW navigation: re-pose the transient ViewportPose
		//! (auto-entering free-fly from the active camera first if needed) to look
		//! straight down a world axis at the current pivot, PRESERVING distance and
		//! the camera's optics — the same pose-only math applied to the view, never
		//! to a scene camera (no SceneEdit / revision bump / undo entry, §4.2).

		//! Enumerates the six axis nubs.  `axis`: 0=X, 1=Y, 2=Z; `negative` picks
		//! the −axis side (Blender: click again = opposite).  Snap positions the
		//! camera on the chosen side of the pivot looking back down the axis, with
		//! a sensible up (world-Y for X/Z axes; ∓Z for the Y/top-bottom axis so up
		//! isn't parallel to the view).  Returns false on a bad axis, no seedable
		//! camera, or a realization failure.
		bool SnapViewToAxis( int axis, bool negative, unsigned int pane = kViewportNavPrimary );

		//! Capture the CURRENT view (the transient pose if in free-fly, else the
		//! active camera) into the single reserved "home" slot.  Returns false when
		//! there is no camera to capture.
		bool SetHomeView( unsigned int pane = kViewportNavPrimary );

		//! Restore the home slot into the transient pose (a degenerate named-view
		//! restore, §4.2).  Returns false when no home has been set.
		bool GoToHomeView( unsigned int pane = kViewportNavPrimary );

		//! Whether a home view has been captured this session.
		bool HasHomeView() const;

		//! -------- Named Views (Tier 2 / Direction B §3) --------
		//!
		//! Session/UI bookmarks of a view — the same pose+full-optics payload
		//! (a CameraSnapshot) the free-fly ViewportPose and Home slot carry.
		//! NOT scene state: capture/restore/update/delete never write the
		//! scene; restore lands the payload in the transient ViewportPose
		//! (non-destructive, exactly like an axis snap or Home).  The ONLY
		//! scene write is PromoteNamedViewToCamera (§3.4).  In-memory this
		//! slice; sidecar persistence + thumbnails are follow-ups.  Named
		//! views are guarded by mNamedViewsMutex -- originally UI-thread-
		//! only, but FindNamedViewPose (agent render{view:}) added a
		//! cross-thread READER; name-MUTATING calls remain single-writer
		//! UI-thread-only (UpdateNamedView's tamper-witness relies on it).
		struct NamedView
		{
			String         name;
			CameraSnapshot pose;
		};

		//! Capture the CURRENT view (the free-fly pose if active, else the
		//! active scene camera) as a NEW named view.  Returns false when there
		//! is no capturable camera or `name` exceeds 255 payload bytes (the
		//! platform bridge enumeration ABI reserves one byte for its
		//! trailing NUL).  `name` is otherwise used as-is (caller dedups if it
		//! wants unique labels).
		bool CaptureNamedView( const String& name );

		unsigned int NamedViewCount() const;
		//! Copy view `idx`'s name into `out` (NUL-terminated).  False on a bad
		//! index or a too-small buffer.
		bool NamedViewName( unsigned int idx, char* out, unsigned int outLen ) const;

		//! Restore view `idx` into the transient ViewportPose (non-destructive
		//! — no scene mutation, no undo entry; a degenerate named-view restore
		//! per §3.2).  False on a bad index or realization failure.
		bool RestoreNamedView( unsigned int idx );

		//! Re-capture the CURRENT view into slot `idx` (SketchUp "Update
		//! Scene").  False on a bad index or no capturable camera.
		bool UpdateNamedView( unsigned int idx );

		//! Remove view `idx`.  False on a bad index.
		bool DeleteNamedView( unsigned int idx );

		//! Promote view `idx` into a NEW named scene camera (§3.4) — the same
		//! AddCamera-from-a-pose write B3 stamp uses, sourced from the stored
		//! view instead of the live pose. `proposedName` is canonicalized to a
		//! CST-safe identifier before deduplication; `outName` receives the
		//! chosen name and the new camera becomes active. False on a
		//! bad index, no camera manager, a too-small buffer, or a refused edit.
		bool PromoteNamedViewToCamera( unsigned int idx, const String& proposedName,
		                               char* outName, unsigned int outLen );

		//! GUI render modes P2a `render{view:}` surface (docs/gui/RENDER_MODES.md
		//! §8): find a captured named view by NAME and copy its pose out.
		//! UNLIKE the rest of the Named Views API above (documented UI-thread-
		//! only, no lock -- see the section comment in the .cpp), this
		//! accessor IS reachable from the agent-render worker thread (a
		//! `render{view:"..."}` call can run on SubmitAgentRenderAsync's
		//! dedicated worker, not the UI thread that owns Capture/Update/
		//! Delete), so it -- and every other mNamedViews touch -- is guarded
		//! by mNamedViewsMutex.  Returns false if no view named `name` exists.
		bool FindNamedViewPose( const String& name, CameraSnapshot& outPose ) const;

		//! Entity-creation slice: number of "Add Entity" templates
		//! registered for `cat` (see EntityTemplates.h).  0 for
		//! categories with none (Camera/Rasterizer/Film/Animation/
		//! SceneVariant/None).
		unsigned int EntityTemplateCount( Category cat ) const;

		//! Display label for the template at `idx` within `cat` (e.g.
		//! "Sphere", "Omni Light"), or empty for an out-of-range idx.
		String EntityTemplateLabel( Category cat, unsigned int idx ) const;

		//! Instantiate the template at `idx` within `cat`: expands its
		//! chunk-text sequence (substituting @NAME@ / @MATERIAL@ /
		//! @TEXTURE@ as the template requires — see EntityTemplates.h)
		//! and inserts each chunk in order via ApplyAgentInsertChunk,
		//! roots first.  Object templates that need a material and find
		//! none in the scene bootstrap a bundled default
		//! uniformcolor_painter + lambertian_material first.  On
		//! success `*outName` (if non-null) receives the deduped
		//! instance name chosen for the new entity (base name suffixed
		//! "_2", "_3", ... on collision, mirroring CloneActiveCamera's
		//! UniqueCameraName suffix scheme).  The chosen name is picked so
		//! its ENTIRE name set is free before any insert — the top-level
		//! name (in its own category) AND every derived sub-chunk name a
		//! multi-chunk template bakes in (e.g. an Object's `<name>_geo`
		//! geometry, a GGX material's `<name>_rd`/`<name>_rs` painters),
		//! checked doc-wide — so a leftover orphan sub-chunk can't make an
		//! Add fail mid-sequence with a name the user never chose.
		//!
		//! Multi-chunk templates are SEQUENTIAL, independently-atomic
		//! ApplyAgentInsertChunk calls, NOT one composite undo step —
		//! BeginTransaction/EndTransaction and the agent-commit surface
		//! are mutually exclusive by design (see ApplyAgentInsertChunk's
		//! own mTxnOpen refusal), and SceneEditor's composite grouping
		//! (BeginComposite/EndComposite) is never wired to the agent
		//! entry points.  A multi-chunk Add therefore undoes as N
		//! separate steps (one Cmd-Z per chunk, innermost/last-inserted
		//! first) — an honest, documented limitation of this slice, not
		//! a bug.
		//!
		//! On any chunk's insert failing partway through a sequence,
		//! returns that chunk's AgentCommitResult verbatim (its
		//! `message` names the failure) and does NOT roll back chunks
		//! already inserted — same "no atomic multi-chunk rollback"
		//! caveat as above.  Callers that want a byte-identical bailout
		//! must Undo the partially-applied steps themselves.
		AgentCommitResult InstantiateEntityTemplate( Category cat, unsigned int idx, String* outName );

		//! Duplicate the named entity in `cat`: serializes its current
		//! chunk out of the retained CST Document, substitutes a fresh
		//! deduped name for its `name` parameter, and inserts the copy
		//! via ApplyAgentInsertChunk.  On success `*outName` (if
		//! non-null) receives the new name.  Refuses (with a non-empty
		//! message) when `cat` has no chunk-name addressing scheme, the
		//! entity isn't found, or the insert itself is rejected (e.g. a
		//! dangling reference the copy would introduce).
		AgentCommitResult DuplicateEntity( Category cat, const String& name, String* outName );

		//! -------- S18: node-graph canvas chunk creation --------
		//! (docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S18; the Phase-B
		//! prerequisite S19/S20/S21 build on.)
		//!
		//! WHY THIS EXISTS ALONGSIDE THE TWO CREATION PATHS ABOVE.  There
		//! are exactly three chunk-creation shapes in the tree and this is
		//! deliberately NOT a fourth:
		//!   * Job::ApplyCstInsertCameraChunk -- DOCUMENT-ONLY, no
		//!     re-derive, because CloneActiveCamera already put the clone
		//!     in the LIVE scene and a re-derive would throw it away.  A
		//!     live-first special case, not a general creator.
		//!   * Job::ApplyCstInsertChunk (via ApplyAgentInsertChunk) -- the
		//!     GENERAL creator: caller-supplied chunk TEXT, tier-positioned
		//!     splice, dry-run-guarded full re-derive, EditHistory record,
		//!     dirty, epoch, kick.  Everything below routes through it.
		//!   * InstantiateEntityTemplate -- a fixed PICKER over
		//!     hand-authored recipes, keyed by (category, index).
		//! The canvas needs the picker's ergonomics (pick a name, hand back
		//! a live entity) over an OPEN keyword set (a search palette of
		//! every painter/material chunk the registry knows), which no
		//! fixed table can supply.  So this method composes the two: it
		//! DERIVES the minimal body from the ChunkDescriptorRegistry
		//! (EntityTemplates::BuildNodeChunkText) and then inserts it
		//! through ApplyAgentInsertChunk -- inheriting the whole commit
		//! discipline (admission lock, mTxnOpen refusal, cancel-and-park,
		//! optimistic-concurrency conflict gate, dry-run-guarded derive so
		//! a refusal leaves the Document BYTE-IDENTICAL, rebind on D2,
		//! MarkCstHeadDirty, the U2 EditHistory record so one Cmd-Z removes
		//! the node again, the scene-epoch bump the canvas re-enumerates
		//! on, and the render kick) rather than re-deriving any of it.
		//!
		//! NOT EXPOSED TO THE AGENT SURFACE, deliberately: an agent already
		//! writes chunk text, and insert_chunk accepts it with strictly
		//! more expressive power than a keyword + arg list.  Adding a verb
		//! that can only do less would widen the MCP surface for no
		//! capability.  This is a controller + C ABI surface for the S21
		//! canvas.
		//!
		//! LIGHT GENERATION: no explicit bump is needed (nor correct) here.
		//! An insert is always D2-class -- the Scene and every manager are
		//! rebuilt from the re-derived Document, so the luminary list is
		//! reconstructed wholesale.  The BumpSceneLightGeneration* calls
		//! exist for the INCREMENTAL (code-1) param-edit path, which
		//! mutates a live material in place; there is no such path here.
		//! A created emissive material therefore lights the scene on the
		//! very next pass, and creating a non-emissive one costs nothing.

		//! One creation argument: `param` must be a parameter the
		//! keyword's descriptor declares, `value` its literal text.
		//! `name` is refused -- this verb picks the name.
		struct ChunkNodeArg
		{
			String param;
			String value;
		};

		//! One argument the caller MUST supply for a given keyword (a
		//! `required` descriptor parameter with no static default -- in
		//! practice the required REFERENCE slots, e.g. `ramp_painter`'s
		//! `input`, which can only name another chunk in THIS scene).
		//! `isReference` tells the canvas to resolve it from the drag
		//! context / a candidate picker rather than a text field; use
		//! ConnectionLegality (S17) for the legal candidate set.
		struct ChunkNodeRequirement
		{
			String param;
			String description;
			bool   isReference = false;
		};

		//! The caller-supplied-argument contract for `keyword`.  Empty for
		//! a keyword that needs nothing, for a non-painter/material
		//! keyword, and for an unknown keyword.  Pure descriptor read: no
		//! scene state, no locking, callable from any thread at any time.
		std::vector<ChunkNodeRequirement> ChunkNodeRequirements( const String& keyword ) const;

		//! doc-88 Phase 3 S21 -- every registered keyword whose descriptor
		//! category is @a category, sorted lexicographically
		//! (`ChunkDescriptorRegistry::AllKeywordsForCategory` passthrough).
		//! Backs the canvas's "add node" search palette: `Painter`,
		//! `Function`, and `Material` are the three categories the palette
		//! offers (the same three `PainterMaterialGraph` models -- see
		//! `BuildPainterMaterialGraphSeedsLocked_`'s own comment). Pure
		//! descriptor read, same "no scene state, no locking" posture as
		//! `ChunkNodeRequirements` above.
		std::vector<String> PaletteKeywords( ChunkCategory category ) const;

		//! Create ONE new painter/material chunk of type `keyword`, named
		//! from `baseName` (deduped `_2`, `_3`, ... exactly as
		//! InstantiateEntityTemplate does, and checked doc-wide so a
		//! leftover chunk of another kind cannot collide), with the
		//! minimal derivable body plus `args`.  `*outName` (if non-null)
		//! receives the name that actually landed -- read from the primitive's
		//! own parsed-back-out chunkName, not the locally-composed base --
		//! whenever the Document was mutated (a clean apply OR a diagnosed-
		//! but-mutated commit, see below), never only on a clean apply.
		//!
		//! REFUSES on a REJECTED result (non-mutating, head byte-identical,
		//! `message` naming the cause, `retriable` set like
		//! ApplyAgentInsertChunk's own render-locked/teardown refusals) when:
		//! the keyword is unknown or is not a painter / material; an arg
		//! names an undeclared parameter or carries an empty value; a
		//! required arg is missing; an arg value is not a single line; or
		//! the composed chunk would not derive in context (the dry-run's own
		//! diagnostic is appended).  Also carries every refusal
		//! ApplyAgentInsertChunk itself can produce -- render-locked
		//! (retriable), open-editor-transaction (retriable), controller
		//! teardown.  A DIAGNOSED result is DIFFERENT and NOT byte-identical:
		//! the chunk was spliced in and the live managers were rebuilt, but
		//! the full re-derive also emitted diagnostics -- `applied` is still
		//! false, the mutation is real (and undoable), and `*outName` IS
		//! filled.
		//!
		//! `baseName` is a HINT, canonicalized through the same
		//! CanonicalCameraName choke point CloneActiveCamera's name pick
		//! uses (whitespace/braces/etc. become `_`): only a genuinely empty
		//! base (before or after canonicalization) falls back to the keyword
		//! itself (`ramp_painter` -> `ramp_painter`, `ramp_painter_2`, ...) --
		//! a 1-char base is used as-is.  Any base is suffixed on collision
		//! (reserving suffix bytes before truncating to the 255-byte payload
		//! the 256-byte `outName` C ABI buffer allows, same discipline as
		//! UniqueCameraName), so the caller must read `*outName` rather than
		//! assume it got what it asked for.
		AgentCommitResult CreateChunkNode( const String& keyword,
		                                   const String& baseName,
		                                   const std::vector<ChunkNodeArg>& args,
		                                   String* outName );

		// =====================================================================
		// doc-88 Phase 3 S19 -- the ownership-closure rewrite + REFUSE path
		// (OwnershipClosure.h/.cpp; docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S19,
		// implementing docs/gui/MATERIAL_EDITOR.md sect. 3.7a).  The canvas's
		// REWIRE verb: re-point one reference slot at a different chunk.
		// =====================================================================

		//! `RewireConnection`'s answer: the ordinary commit result, plus the
		//! sect. 3.7a closure report the canvas needs to badge / explain /
		//! offer the escape hatch.  The closure fields are populated on BOTH
		//! outcomes where they are known (a clean rewire reports the owner it
		//! resolved and anything the edit orphaned; a refusal reports what
		//! blocked it), never only on failure.
		struct RewireResult
		{
			//! The underlying commit -- `applied` / `status` / `message` /
			//! `headVersion` / `conflict` / `retriable` all carry the SAME
			//! meanings `AgentCommitResult` documents, because on the accept
			//! path this IS ApplyAgentParamEdit's result verbatim.  Every
			//! refusal below is a `status == "rejected"`, non-mutating,
			//! head-BYTE-IDENTICAL result.
			AgentCommitResult commit;

			//! Why the closure step refused, machine-readable so a bridge can
			//! branch (offer Duplicate-node only for `SharedTarget`) without
			//! parsing prose.  `Clean` on every outcome that got PAST the
			//! closure step -- including a later legality/conflict refusal --
			//! so read `commit.applied` for "did it land", not this.
			ClosureClassification closure = ClosureClassification::Clean;

			//! True when the refusal came from `ConnectionLegality`
			//! (S17) rather than from the closure: the proposed binding is
			//! not one the real parser would accept.  `commit.message` then
			//! carries the parser's OWN diagnostic verbatim, so a
			//! canvas-rejected wire reads identically to what a hand-edited
			//! scene file fails on (MATERIAL_EDITOR.md:145).
			bool legalityRefused = false;

			//! True when the refusal came from the forward-reachability cycle
			//! check (`ConnectionLegality::WouldCycle`).
			bool cycleRefused = false;

			//! sect. 3.7a's (a)/(b)/(c): the shared chunk(s), the referrers
			//! outside the closure, and the owning roots.  See
			//! OwnershipClosureResult's own field docs -- these are copied
			//! from it verbatim.
			std::vector<String> sharedChunks;
			std::vector<String> outOfClosureReferrers;
			std::vector<String> owners;

			//! Chunks that lose their LAST reference if/when this rewire
			//! commits -- for the canvas to badge as newly orphaned.
			//!
			//! SCOPING, stated rather than left to be discovered: this slice
			//! does NOT auto-delete them.  Reference-safe delete (block-or-
			//! cascade, ENTITY_CREATION.md sect. 5) is S20's slice; S19 rewires
			//! the slot and REPORTS what that orphaned, leaving the orphan in
			//! the document where an undo can still restore the wiring.
			//!
			//! APPLIED GUARD: empty whenever `commit.applied` is false --
			//! including a refusal that lands AFTER the closure step already
			//! computed a non-empty report (a later addressing-seam mismatch,
			//! a mid-transaction refusal, a stale-baseVersion conflict).
			//! `RewireConnection` clears it on every refusal path so this
			//! field never reports an orphan from an edit that never
			//! committed (S19 review round 1 P2-1).
			std::vector<String> nowUnreferenced;
		};

		//! Re-point `targetName`'s `param` (occurrence `occurrence`) at
		//! `newRefName`, as ONE undoable commit through the existing
		//! param-edit machinery.  The canvas's drag-a-wire-to-a-different-
		//! node verb.
		//!
		//! GATE ORDER, which is load-bearing:
		//!   1. Render-locked / no-document pre-flight.
		//!   2. RESOLUTION of both endpoints.  An AMBIGUOUS name (two
		//!      same-category chunks share it -- the colour/`scalar_painter`
		//!      pair `SceneReferenceGraph::ResolveChunk` documents) REFUSES;
		//!      it never guesses which one it meant.
		//!   3. LEGALITY (`ConnectionLegality::CheckConnection`) and the
		//!      CYCLE check (`WouldCycle`) -- FIRST, before ownership,
		//!      because an illegal or cyclic wire is refused whether or not
		//!      the edit owns what it touches, and its diagnostic is the
		//!      parser's own.
		//!   4. OWNERSHIP CLOSURE (`OwnershipClosure::Compute`) -- sect. 3.7a.
		//!   5. The commit, through `ApplyAgentParamEditInner_` (so it
		//!      inherits the mid-transaction refusal, the optimistic-
		//!      concurrency conflict gate, the U1 undo record + prior-value
		//!      capture at the SAME occurrence, the D2 rebind, the dirty
		//!      mark, the scene-epoch bump the canvas re-enumerates on, and
		//!      the render kick -- none of it re-derived here).
		//!
		//! Every refusal in steps 1-4 leaves the retained Document
		//! BYTE-IDENTICAL: nothing before step 5 mutates anything.
		//!
		//! `newRefName` must name a REAL chunk.  A DETACH (unbind the slot)
		//! is NOT commitable in this slice -- NOT because a reference slot
		//! has no valid empty literal (`none` parses and commits into most
		//! reference slots fine when written directly), but because
		//! `ConnectionLegality::CheckConnection` (step 3 above) is
		//! `Cst::NodeId`-addressed on both sides, and `none` is a runtime
		//! default with no `NodeId` -- there is no candidate to run the
		//! legality gate against, so a detach's per-slot legality can't be
		//! checked.  Closing that (a NodeId-free legality path for `none`)
		//! is S20's scope, alongside the reference-safe delete/unbind
		//! policy it already owns.  `OwnershipClosure::Compute`
		//! DOES model `TopologyEditKind::Detach`, so a canvas can pre-flight
		//! "would detaching this orphan anything?" today.
		//!
		//! `occurrence` selects WHICH occurrence of a repeatable reference
		//! param to rewrite, and is validated against the chunk's actual
		//! occurrence count (out of range REFUSES rather than silently
		//! no-op'ing, which is what an unchecked occurrence does inside
		//! `Cst::DocSetOrAddParamValue`).  NOTE (pinned by
		//! RewireConnectionTest's registry sweep): NO chunk kind addressable
		//! through this verb declares a repeatable pure-`ValueKind::Reference`
		//! parameter today -- the one that exists at all,
		//! `standard_shader.shaderop`, lives on a Shader-category chunk this
		//! controller has no (category, name) addressing scheme for -- so
		//! `occurrence` is 0 in every reachable case, and the sweep fails
		//! loudly the day that stops being true.  Reference params that are
		//! repeatable via a TUPLE (`voronoi_painter.gen`'s `<x> <y>
		//! <painter>`) are refused: rewriting one would have to re-emit the
		//! whole tuple, which this verb does not do.
		//!
		//! `baseVersionOrNull` is the same optimistic-concurrency
		//! precondition every other agent commit takes: non-null and not
		//! equal to the current head -> `status == "conflict"`, non-mutating.
		RewireResult RewireConnection(
			ChunkCategory targetCategory, const String& targetName,
			const String& param, int occurrence,
			ChunkCategory newRefCategory, const String& newRefName,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		// =====================================================================
		// doc-88 Phase 3 S20 -- cycle / orphan / copy-vs-link enforcement
		// (docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S20; the reference-safe
		// delete policy of docs/gui/ENTITY_CREATION.md sect. 5, and sect. 3.7a's
		// Duplicate-node escape hatch that S19 could only NAME).
		//
		// WHY THESE ARE NEW VERBS AND NOT `RemoveEntity` / `DuplicateEntity`
		// GROWN A POLICY (the placement question, answered rather than left
		// implicit -- both shipped verbs were read first):
		//
		//   * ADDRESSING.  `RemoveEntity`/`DuplicateEntity` are addressed by
		//     the UI `Category` and resolve through
		//     `Cst::DocFindByNameAnyRole` + `RoleKindSuffixForCategory` -- the
		//     UI UNION in which "painter" deliberately accepts BOTH
		//     ChunkCategory::Painter and ChunkCategory::Function, and in which
		//     a camera may resolve POSITIONALLY.  A graph node's identity is
		//     (declared descriptor category, name) and must NOT merge those
		//     two (see SceneReferenceGraph::ResolveChunk's own note), which is
		//     exactly why `RewireConnection` is `ChunkCategory`-addressed.
		//     These two are its siblings, so they are too.
		//   * BLAST RADIUS.  `RemoveEntity` is the shipped OUTLINER delete for
		//     EVERY category.  ENTITY_CREATION.md sect. 5.2 deletes an Object,
		//     a Camera and a Light FREELY -- teaching `RemoveEntity` to refuse
		//     on referrers would change behaviour for every one of those, and
		//     for every shipped caller/bridge/test of it, to serve a
		//     painter/material policy.
		//   * RESULT SHAPE.  A canvas delete has to report WHICH referrers
		//     blocked it and WHAT a cascade swept; `AgentCommitResult` has
		//     nowhere to carry either.  `RewireResult` set the precedent of a
		//     verb-specific result wrapping the commit.
		// `RemoveEntity` and `DuplicateEntity` are therefore UNCHANGED and
		// still the right calls for the outliner; these are the graph's.

		//! What a `DeleteGraphNode` should do about the chunks BELOW the
		//! target that nothing else uses once it is gone.
		enum class GraphDeleteMode
		{
			//! Delete ONLY the target.  Chunks it solely owned stay in the
			//! document as orphans (which is also what a rewire leaves behind
			//! -- see `RewireResult::nowUnreferenced`).
			TargetOnly = 0,
			//! Delete the target AND its solely-owned, unreferenced-after
			//! closure, as ONE undoable composite.  A chunk shared with any
			//! other graph is NEVER swept -- see `kDeleteCascadeSharedFmt`.
			Cascade    = 1
		};

		//! `DeleteGraphNode`'s answer: the ordinary commit result plus the
		//! reference-safety report the canvas needs to explain a refusal or
		//! show what a cascade took.
		struct DeleteResult
		{
			//! The underlying commit.  On the accept path this IS
			//! `ApplyAgentRemoveChunk`'s (TargetOnly) or
			//! `ApplyAgentRemoveChunks`'s (Cascade) result verbatim; every
			//! refusal below is a `status == "rejected"`, non-mutating,
			//! head-BYTE-IDENTICAL result.
			AgentCommitResult commit;

			//! `AmbiguousTargetName` / `UnresolvedTarget` when the name did
			//! not resolve to exactly one chunk; `Clean` on every outcome that
			//! got past resolution -- INCLUDING a later reference/cascade
			//! refusal, which is not a CLOSURE verdict.  Read
			//! `commit.applied` for "did it land", not this.
			ClosureClassification closure = ClosureClassification::Clean;

			//! True when the refusal was "still referenced" -- so a bridge can
			//! offer "rewire those away" without parsing prose.
			bool referenceRefused = false;

			//! True when a CASCADE was refused because its sweep would have
			//! reached a chunk another graph owns.
			bool cascadeRefused = false;

			//! Every referrer that blocks the delete, as `chunk`.`param`
			//! (an unnamed referrer contributes `<keyword>`.`param`, the same
			//! angle-bracket display convention `OwnershipClosureResult::owners`
			//! uses).  Populated on the `referenceRefused` path; EMPTY on a
			//! clean delete, which by definition had none.
			std::vector<String> referrers;

			//! In DOCUMENT ORDER: the chunks this call removed (target first
			//! only if it happens to be first in the document -- this is the
			//! document's order, not a priority order).  EMPTY ON EVERY REFUSAL
			//! (corrected -- S20 review round 1 P2-2: an earlier draft of this
			//! comment claimed a cascade-shared refusal leaves a "preview" of
			//! the sweep here, but that can never happen -- this field's only
			//! writer is the addressability loop, which itself only runs while
			//! the internal refusal accumulator is still empty, and the
			//! shared-cascade guard always sets that accumulator BEFORE the
			//! addressability loop would run.  So a `cascadeRefused` outcome
			//! reaches this field exactly like every other refusal: empty).
			//! APPLIED GUARD: on a refusal `commit.applied` is false and
			//! nothing was removed -- read this as a record of a successful
			//! mutation only, never present on any refusal path.
			std::vector<String> removed;
		};

		//! Delete the graph node `(category, name)`, reference-safely.
		//!
		//! CATEGORY SCOPE -- DELIBERATELY WIDER THAN `DuplicateGraphNode`
		//! (S20 review round 1 P3, documenting the asymmetry rather than
		//! collapsing it): `DuplicateGraphNode` below refuses every category
		//! except Painter / Function / Material, because ITS specific
		//! contract -- "splice the copy immediately after the original in
		//! declaration order" -- is only meaningful for an interior graph
		//! node or a material sitting inside a reference DAG where "after"
		//! has a consumer-legality meaning.  This verb has no such
		//! constraint: it implements ENTITY_CREATION.md sect. 5's
		//! reference-safe delete policy, which is scoped to EVERY
		//! introspectable entity family the doc's dependency-graph table
		//! covers -- Object, Camera, Light, Material, Medium, and (stage 2)
		//! Painter -- not to the canvas's graph-node subset.  `DeleteGraphNodeTest`
		//! 1f/1g exercise this directly: a `ChunkCategory::Shader` delete
		//! reaches the SAME addressability refusal a Painter/Material delete
		//! would (refused for lacking a `(UI Category, name)` edit path in
		//! THIS editor, per `UiCategoryForChunkCategory`/
		//! `RoleKindSuffixForCategory` -- not for being the "wrong kind" the
		//! way `DuplicateGraphNode`'s up-front kind check refuses).  So the
		//! practical scope ends up self-limiting to whatever
		//! `RoleKindSuffixForCategory` already addresses (Painter, Material,
		//! Geometry, Medium, Object, Camera, Light) without this verb having
		//! to enumerate or gate categories itself -- widening that set (a
		//! future Shader/Modifier addressing surface) widens what this verb
		//! can delete for free, with no change here.
		//!
		//! GATE ORDER, load-bearing exactly as `RewireConnection`'s is:
		//!   1. Render-locked / teardown / no-document pre-flight.
		//!   2. RESOLUTION, ambiguity-aware (`SceneReferenceGraph::ResolveChunk`).
		//!      An AMBIGUOUS name REFUSES -- never delete on a guess.
		//!   3. REFERRERS (`SceneReferenceGraph::FindReferencesTo`).  Any
		//!      referrer at all REFUSES, in BOTH modes, naming every one of
		//!      them plus the two escapes (`kDeleteReferencedFmt`).
		//!   4. `Cascade` only: build the sweep, then CROSS-CHECK every member's
		//!      ownership through `OwnershipClosure::OwnersOf` -- a member owned
		//!      by anything but the target REFUSES the whole cascade
		//!      (`kDeleteCascadeSharedFmt`), and so does a member the editor
		//!      cannot address (`kDeleteUnaddressableFmt`).
		//!   5. The commit: `ApplyAgentRemoveChunk` (TargetOnly) or
		//!      `ApplyAgentRemoveChunks` (Cascade -- ONE atomic all-or-nothing
		//!      erase, ONE EditHistory record, so one Cmd-Z restores the whole
		//!      composite).
		//!
		//! Every refusal in steps 1-4 leaves the retained Document
		//! BYTE-IDENTICAL: nothing before step 5 mutates anything.
		//!
		//! WHY `ApplyAgentRemoveChunks` AND NOT `BeginTransaction`/
		//! `EndTransaction` FOR THE CASCADE.  The two are MUTUALLY EXCLUSIVE by
		//! construction: every agent-commit entry point (`ApplyAgentChunkCrud_`,
		//! `ApplyAgentRemoveChunksCrud_`, `ApplyAgentParamEditInner_`) refuses
		//! outright while `mTxnOpen` is set, so bracketing agent removes in an
		//! editor transaction produces N refusals, not a composite -- the same
		//! constraint `InstantiateEntityTemplate` already documents.
		//! `ApplyAgentRemoveChunks` IS the composite primitive: it resolves and
		//! erases every target against the SAME pre-erase Document, runs ONE
		//! dry-run-guarded re-derive (so an intra-batch reference can never
		//! cause a spurious refusal), and records ONE `AgentRemoveChunks` U2
		//! step whose undo payload is the BYTE-EXACT pre-batch document -- a
		//! stronger inverse than N per-chunk splices, which for ADJACENT
		//! targets are not even well defined (`DocEraseChunkTidy`'s
		//! trailing-separator collapse for chunk i depends on whether chunk
		//! i+1 is still there).
		//!
		//! `baseVersionOrNull` is the usual optimistic-concurrency precondition.
		DeleteResult DeleteGraphNode(
			ChunkCategory category, const String& name,
			GraphDeleteMode mode,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! `DuplicateGraphNode`'s answer.
		struct DuplicateResult
		{
			//! The underlying commit (the whole-document composite swap).
			AgentCommitResult commit;
			//! `AmbiguousTargetName` / `UnresolvedTarget` when the name did not
			//! resolve to exactly one chunk; `Clean` otherwise.
			ClosureClassification closure = ClosureClassification::Clean;
			//! The deduped name the copy actually landed under -- READ THIS
			//! rather than assuming the requested base survived. Empty unless
			//! the Document was mutated.
			String newName;
			//! The top-level document index of the ORIGINAL at the moment of
			//! the copy, for a canvas that wants to place the new node beside
			//! it. -1 when nothing landed.
			int    originalIndex = -1;
		};

		//! Fork `(category, name)` into an owned copy positioned IMMEDIATELY
		//! AFTER the original in declaration order -- sect. 3.7a's Duplicate-node
		//! escape hatch, which S19 could name in a diagnostic but not deliver.
		//!
		//! WHY THIS IS A NEW VERB AND NOT A FIX INSIDE `DuplicateEntity`.
		//! `DuplicateEntity`'s copy is placed by `Job::ApplyCstInsertChunk`'s
		//! TIER heuristic -- a painter goes "before the first Material/Geometry/
		//! Shader/... chunk".  In a scene whose FIRST chunk is a `standard_shader`
		//! (tier 1), that target index is 0, ahead of every painter the copy
		//! itself references, so the positioned dry-run fails and the insert
		//! FALLS BACK to append-at-end -- which is the S19 handoff's pinned gap
		//! (`RewireConnectionTest` case 1c'), reproduced and confirmed by probe.
		//! Making `ApplyCstInsertChunk` position by DEPENDENCY instead would
		//! change where every agent `insert_chunk` lands, in every scene, to
		//! serve one canvas verb; and no pure function of (document, chunk text)
		//! can express "immediately after THAT chunk", because the text of a
		//! copy does not say which chunk it is a copy OF.  So position is the
		//! CALLER's knowledge here, and this verb splices with it.  A rename is
		//! deliberately NOT attempted: `DuplicateEntity` remains the outliner's
		//! duplicate, unchanged.
		//!
		//! SHALLOW, BY DESIGN AND BY DEFINITION.  The copy shares every chunk
		//! the original referenced -- that is the POINT of the hatch: it unshares
		//! exactly ONE level, so the requesting consumer can be re-pointed at a
		//! node it now solely owns while the leaves below stay links (sect. 3.8's
		//! "shared painters are links, not copies").  A DEEP copy would fork the
		//! whole subgraph and is deliberately out of scope for this slice; it is
		//! also why the copy is guaranteed to derive at its new position:
		//! identical references, one slot later than an original that already
		//! derived where it sits.
		//!
		//! Refuses (non-mutating, head byte-identical) on an ambiguous or
		//! unresolved name, on a chunk with no `name` parameter to substitute,
		//! on a name collision the dedup could not clear, and on every refusal
		//! the commit layer itself can produce.
		DuplicateResult DuplicateGraphNode(
			ChunkCategory category, const String& name,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! Remove the named entity in `cat` via ApplyAgentRemoveChunk,
		//! narrowed by `cat`'s CST role-kind suffix (RoleKindSuffixForCategory)
		//! so e.g. removing a Material named the same as an unrelated
		//! Light doesn't clash.  On failure surfaces a non-empty refusal
		//! message (ApplyAgentChunkCrud_'s English wrapper naming the
		//! likely still-referenced / non-deriving cause, with Job's
		//! diagnostic appended) — e.g. removing a material a
		//! standard_object still references is rejected, not silently
		//! skipped.
		AgentCommitResult RemoveEntity( Category cat, const String& name );

		//! -------- Viewport render modes (P1, docs/gui/RENDER_MODES.md §5) --------
		//!
		//! Mode switch = caster swap on the interactive rasterizer, coordinated
		//! the SAME way SetViewportPose (above) swaps mViewportOverrideCamera:
		//! check the render-owns-scene guard first (no-op while a production/
		//! agent render is in flight), take mMutex, CancelAndParkRender_ so the
		//! swap never races DoOneRenderPass's in-flight RasterizeScene call,
		//! mutate, then release the lock and kick a repaint.  See
		//! InteractivePelRasterizer::SetViewModeCaster for the caster-swap
		//! mechanics (which preview/polish invariants it preserves) and
		//! RebindEditorToJob for the "every scene load/reload resets to
		//! preview" rule (a scene silently opening in depth mode would read
		//! as a broken render).

		//! Switch the interactive viewport to render-mode `name` (a registry
		//! wire name -- "preview", "normals", "depth", "facets", "wireframe";
		//! NOT "objectmap", which is `viewportSelectable=false`: it has its
		//! own palette-lifecycle pipeline, not a plain caster swap -- see
		//! docs/gui/RENDER_MODES.md §4).  No-op (returns true, nothing
		//! mutated) when `name` already names the active mode.  Returns false
		//! for an unknown name, a non-viewport-selectable mode, while a
		//! production/agent render owns the scene, or in skeleton mode (no
		//! interactive rasterizer wired up -- see the constructor's
		//! test-harness note).
		bool SetViewportRenderMode( const char* name );

		//! The registry wire name of the CURRENTLY active viewport render
		//! mode ("preview" by default and after every reset).  Never null.
		const char* GetViewportRenderMode() const;

		//! -------- X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis") --------
		//!
		//! An orthogonal boolean axis that applies to EVERY viewport render
		//! mode, INCLUDING Preview (the studio material-preview pipeline):
		//! when on, the resolved primary hit is walked THROUGH transmissive
		//! (glass-like) surfaces to the first OPAQUE hit -- a straight-line
		//! continuation of the original ray, deliberately with NO refraction
		//! bending (an x-ray, not an optics simulation).  Resolution lives in
		//! the CASTER layer (RayCaster::ResolveXrayView_ / SetXrayViewResolve),
		//! so no per-mode caster rebuild is needed: SetViewportXray just
		//! stamps InteractivePelRasterizer::SetXrayView on the flag change,
		//! which propagates to every caster the rasterizer currently holds
		//! (active, polish, and both saved-caster slots) -- see that method's
		//! doc.  DEFAULT OFF: the viewport shows the first transmissive
		//! surface normally. X-ray is an explicit opt-in for seeing opaque
		//! geometry behind it.

		//! Set the x-ray flag.  Applies immediately regardless of which mode
		//! is active (including Preview).  Same lock/park discipline as
		//! SetViewportRenderMode above, but with NO caster rebuild -- just a
		//! flag stamp.  No-op (returns true) when `xray` already matches the
		//! current flag.  Returns false while a production/agent render owns
		//! the scene, or in skeleton mode (no interactive rasterizer).
		bool SetViewportXray( bool xray );

		//! The CURRENT x-ray flag (false by default and after every
		//! RebindEditorToJob reset -- see the axis doc above).
		bool GetViewportXray() const;

		//! -------- N-up multi-viewport pane model (RENDER_MODES.md §7, P3a) ----
		//!
		//! Four ALWAYS-PRESENT pane slots; the layout selects the visible
		//! subset (hidden panes keep their configuration so toggling
		//! 2x2 -> 1 -> 2x2 never loses a setup).  Pane 0 is an ALIAS VIEW of
		//! the existing single-viewport state (its mode IS
		//! mViewportRenderMode; its vantage IS the free-fly ViewportPose
		//! state), so single-viewport behaviour is unchanged and every
		//! existing setter/getter keeps working unmodified.  Panes 1-3 hold
		//! validated configuration that the §7.3 context-switch scheduler
		//! (SwitchToPaneLocked_ and friends, below) realizes lazily at each
		//! rotation switch -- all visible panes RENDER.
		//!
		//! All setters follow the house discipline: render-owns-scene guard
		//! first, then mMutex (+ CancelAndParkRender_ where render-thread-read
		//! state is touched), fail-closed on any invalid input.

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

		//! user-review P1-3: an atomic snapshot of the whole pane set, captured
		//! WITH the interactive frame (not read back through the individual
		//! locking getters after rendering resumed, which could describe a later
		//! state than the returned PNG).  Filled by SnapshotPaneSetForParkedRead.
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

		//! Capture the whole pane set into @a out.  REQUIRES mMutex to ALREADY
		//! be held -- call ONLY from inside a RunPreviewRenderParked closure (the
		//! same contract CopyInteractiveFrame follows), so the snapshot is atomic
		//! with the frame copied there and the render is parked.  Re-taking
		//! mMutex here would deadlock (it is a plain non-recursive std::mutex).
		void SnapshotPaneSetForParkedRead( PaneSetSnapshot& out ) const;

		//! Panes a layout makes visible: Single=1, TwoH=2, OnePlusTwo=3, Quad=4.
		static unsigned int PaneCountForLayout( ViewportLayout layout );

		//! Switch the pane layout.  Newly-visible panes keep whatever
		//! configuration they already hold (defaults: preview / SceneCamera).
		//! When the shrink hides the current primary, primary falls back to
		//! pane 0 (§7.2).  False while a production/agent render owns the
		//! scene.  No-op true when `layout` is already active.
		bool SetViewportLayout( ViewportLayout layout );
		ViewportLayout GetViewportLayout() const;

		//! Make `pane` primary (owner of editing / picking / gizmos).  The
		//! pane must be visible in the current layout.  False on an invalid
		//! or hidden pane, or while a render owns the scene.
		bool SetPrimaryPane( unsigned int pane );
		unsigned int GetPrimaryPane() const;

		//! Set pane `pane`'s render mode by registry wire name.  Pane 0
		//! FORWARDS to SetViewportRenderMode (the alias contract above);
		//! panes 1-3 validate the name against the registry (viewport-
		//! selectable modes only) and store it.  Hidden panes REFUSE (§7.4
		//! fail-closed contract); getters work on any valid index.
		bool SetPaneRenderMode( unsigned int pane, const char* name );
		const char* GetPaneRenderMode( unsigned int pane ) const;

		//! Select Interactive or LastRender content for a visible pane.
		//! LastRender parks an in-flight quantum, publishes the retained full
		//! render (or an explicit empty placeholder), and is never scheduled.
		//! Pane 0 refuses LastRender in Single layout so the legacy surface
		//! remains unchanged.
		bool SetPaneContentSource( unsigned int pane, PaneContentSource source );
		PaneContentSource GetPaneContentSource( unsigned int pane ) const;

		//! AgentSession completion hook.  The caller MUST already own this
		//! controller's coordinated render slot (mMutex held, preview parked).
		//! The image is deep-copied before return and pushed once to each
		//! LastRender pane sink.
		bool PublishAgentRenderImageParked( const IRasterImage& image );
		//! Ownership-transfer sibling used by AgentSession after its
		//! InMemoryRasterizerOutput has already created the required owning
		//! full-resolution copy.  On success, `image` is adopted and nulled;
		//! on refusal ownership remains with the caller.
		bool AdoptAgentRenderImageParked( IRasterImage*& image );

		//! P3a slice 3: set pane `pane`'s displayed-surface pixel dims (the
		//! GUI's aspect-fitted pane rect). N-up passes render at
		//! surface/previewScale; Single pane 0 retains the legacy
		//! scaleFilmToFit resolution policy and uses these dimensions only for
		//! display-space gizmo sizing. 0/0 resets to film dimensions. Applies
		//! to VISIBLE panes only (§7.4 fail-closed); marks the pane dirty.
		bool SetPaneSurfaceDims( unsigned int pane, unsigned int w, unsigned int h );

		//! P3a slice 3: per-pane preview sink.  The render pass attaches
		//! CURRENT pane's sink for its quantum.  Pane 0 falls back to the
		//! legacy single sink when it has no override; panes 1--3 with no
		//! sink publish nowhere.  Pass null to clear.  The controller addrefs.
		//! A subsequent legacy SetPreviewSink call replaces a retained pane-0
		//! override and restores the legacy sink as pane 0's source.
		bool SetPaneSink( unsigned int pane, IRasterizerOutput* pSink );

		//! Vantage setters.  Pane 0 forwards the existing SceneCamera /
		//! NamedView flows, but REFUSES SceneCameraNamed: pane 0 is the
		//! active-camera editing surface.  Panes 1-3 store configuration.
		//! NamedView and SceneCameraNamed require their target to exist NOW
		//! and snapshot its pose as the deletion fallback; live re-resolve by
		//! name happens at render-time reconcile.
		//! -------- P3a slice 3: pane-indexed pointer input ----------------
		//!
		//! The P3b shells hit-test their pane rects and forward events with
		//! the pane index.  Down: (a) refuses while a render owns the scene
		//! (the r2/r4 contract); (b) CLICK PROMOTES PRIMARY (§7.8 ratified
		//! decision 1); (c) parks + context-switches to the pane BEFORE the
		//! gesture pin arms; (d) for a camera-motion tool on a SECONDARY
		//! `SceneCamera` pane, converts the pane to per-pane FreeFly seeded
		//! from the active camera.  `SceneCameraNamed` is the exception:
		//! it stays kind 3 and routes orbit/pan/zoom/roll through the
		//! edit/undo path at its bound manager-camera name, never through
		//! SetActiveCamera; a deleted target refuses rather than falling
		//! back.  NamedView/FreeFly keep private-pose navigation, while
		//! pane 0 keeps the classic active-camera-edit semantics (§7.2).
		//! Move/Up forward to the un-indexed handlers -- the
		//! pane context was established at Down and the gesture pin holds
		//! it.  The un-indexed handlers remain byte-identical pane-0
		//! behaviour.
		//! \return False when the pane is hidden/invalid or a render owns
		//! the scene (the shell should drop the gesture).
		bool OnPanePointerDown( unsigned int pane, const Point2& px );
		bool OnPanePointerMove( unsigned int pane, const Point2& px );
		bool OnPanePointerUp( unsigned int pane, const Point2& px );

		//! P3a slice 3: per-pane free-fly twins (§7.4).  Pane 0 forwards to
		//! the classic EnterFreeFlyFromActiveCamera / ExitFreeFly; panes
		//! 1-3 set the pane's vantage config (enter = seed from the scene
		//! camera; exit = back to SceneCamera tracking).
		bool PaneEnterFreeFly( unsigned int pane );
		bool PaneExitFreeFly( unsigned int pane );

		//! P3a slice 3: per-pane refinement status.  For the CURRENT pane
		//! reads the live registers (exactly what GetRefinementStatus
		//! reports); for an unscheduled pane reads its saved slot state.
		//! Phase values match GetRefinementStatus's contract.
		bool GetPaneRefinementStatus( unsigned int pane, int& outPhase,
		                              unsigned int& outScaleDivisor ) const;

		bool SetPaneVantageSceneCamera( unsigned int pane );
		bool SetPaneVantageSceneCameraNamed( unsigned int pane, const char* name );
		bool SetPaneVantageNamedView( unsigned int pane, const char* name );
		//! Introspection: current kind (+ referenced name for NamedView /
		//! SceneCameraNamed).
		bool GetPaneVantage( unsigned int pane, PaneVantageKind& outKind,
		                     String& outNamedView ) const;

		//! -------- Environment / IBL section (GUI Environment panel) --------
		//!
		//! The image-based-lighting environment is a scene-level singleton
		//! (NOT an ILight): an `hdr_painter`/`exr_painter` supplies the image
		//! and four `radiance_*` params on the ACTIVE rasterizer chunk bind it
		//! as the global radiance map.  These accessors surface + edit that
		//! singleton without the GUI needing to know the two-chunk shape.  See
		//! docs/gui/ENVIRONMENT_SECTION.md.
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

		//! Read the current environment binding from the active rasterizer's
		//! live snapshot (+ resolve the bound painter's `file` from the CST).
		//! Returns false only when there is no scene / no active rasterizer;
		//! otherwise fills `out` (with hasEnvironment=false when unbound).
		bool GetEnvironment( EnvironmentInfo& out ) const;

		//! Set the environment intensity / background-visibility / rotation.
		//! Each applies BOTH a live rasterizer rebuild (viewport re-renders)
		//! and a CST mirror (Job::ApplyCstEnvironmentEdit) so it persists on
		//! save.  Return false when there is no editable bound environment.
		bool SetEnvironmentScale( double scale );
		bool SetEnvironmentBackground( bool background );
		bool SetEnvironmentOrient( double xDeg, double yDeg, double zDeg );

		//! Swap the HDRI file of the currently-bound environment painter.
		//! Routes through the Painter CST-param path (a full re-derive that
		//! reloads the texture) so it persists.  `absPath` should be an
		//! existing file (the GUI file picker is the guard).  Returns false
		//! when no environment is bound.
		bool SetEnvironmentFile( const String& absPath );

		//! Create an environment from an HDRI file when none exists: inserts a
		//! `hdr_painter`/`exr_painter` chunk (kind chosen from the extension)
		//! and binds it via radiance_map on the active rasterizer.  On success
		//! `*outPainterName` (if non-null) receives the new painter name.
		AgentCommitResult AddEnvironment( const String& hdriPath, String* outPainterName );

		//! Remove the environment: unbinds radiance_map on the active
		//! rasterizer (live + CST) so no global radiance map is installed.
		//! The painter chunk is left in the Document (harmless, reusable).
		bool RemoveEnvironment();

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

		//! T0 RED-PROVE seam: called after the scheduler has selected and
		//! minted an interactive pass, with mMutex released but before the
		//! pass body runs.  Tests use it to force observation of the first
		//! post-gesture context switch.
		virtual void ForTest_OnInteractivePassMinted() {}
		//! Called after a BeautyVariant pass's live/final quality policy was
		//! applied and the mint lock was released, while the pass rasterizer
		//! is retained.  Test subclasses inspect the effective sample/denoise
		//! state and the interaction classification that production will run.
		virtual void ForTest_OnBeautyVariantPassConfigured(
			IRasterizer&, Implementation::ViewportRenderMode, bool ) {}
		//! Called after mRendering=false is published for a completed/cancelled
		//! quantum but before the rasterizer drops its retained output refs.
		//! Tests use it to force controller sink detachment while the rasterizer
		//! remains the final sink owner.
		virtual void ForTest_OnInteractivePassBeforeOutputDetach() {}
		//! Called after a pass body and every base RenderLoop post-pass transition
		//! (cancellation requeue or polish/rotation arm) have fully retired.
		virtual void ForTest_OnInteractivePassRetired() {}

		//! T0 RED-PROVE seam: called by OnPointerUp after the gesture pane's
		//! final-render/polish state and edit wake have been published as one
		//! mMutex-serialized transition.  A test can wait here until the render
		//! thread has selected its first post-release pane, proving that the
		//! polish marker was installed before (and on the correct side of) that
		//! context switch.  No-op in production.
		virtual void ForTest_OnPointerUpAfterFinalRenderArmed() {}

		//! Called after PointerDown publishes the live gesture but before it
		//! opens the tool composite.  The full Down transition must still own
		//! render admission here; tests hold this point to prove a render cannot
		//! finalize a half-constructed gesture.
		virtual void ForTest_OnPointerDownAdmitted() {}

		//! Called after TimeScrubBegin opens its composite but before returning.
		//! The begin transition must still own admission here so a render cannot
		//! finalize the scrub halfway through its construction.
		virtual void ForTest_OnTimeScrubBeginAdmitted() {}

		//! Called while mMutex protects the definitive move/watchdog handoff,
		//! after the move is counted in-flight.  Test subclasses can hold this
		//! point to prove the watchdog cannot stale an admitted move.
		virtual void ForTest_OnPointerMoveAdmitted() {}

		//! Called after a Last Render drain has registered its lifetime use but
		//! before it acquires the callback/start barrier.  Tests hold this seam
		//! to prove teardown closes registration without deadlocking a registered
		//! drain that still needs the barrier.
		virtual void ForTest_OnLastRenderDrainRegistered() {}
		//! Called after teardown has closed new Last Render drain registration,
		//! immediately before it waits for existing registrations to retire.
		virtual void ForTest_OnLastRenderQuiesceStarted() {}
		//! Called immediately before a pane source transition attempts the global
		//! Last Render callback/start barrier.
		virtual void ForTest_OnLastRenderTransitionAttempt( unsigned int ) {}

		//! Called from SetViewportLayout's shrink branch -- but ONLY on the
		//! sub-branch that actually PARKS, i.e. when the shrink hides the
		//! currently-scheduled pane or pins an active pointer gesture to a
		//! now-invisible one (`currentHidden || gestureHidden`).  A shrink
		//! that hides neither never parks and therefore never stamps: there
		//! is no in-flight pass to make provably idle, so the epoch this seam
		//! exists to publish would not be true.  A TEST that relies on the
		//! stamp must construct a shrink that hides the scheduled or the
		//! gesture pane, and must treat a missing stamp as a FATAL setup
		//! failure -- a non-fatal precondition check silently degrades the
		//! assertion it guards back to an unsynchronised whole-sequence scan.
		//! At the instant it does fire, the new layout is authoritative and
		//! the render thread is provably idle:
		//! mMutex is held, the park wait has already returned (mRendering is
		//! false), and the scheduler has not yet been relocated to a visible
		//! pane.  No interactive pass can be running or minting here -- a mint
		//! needs this same mMutex, and a pass BODY (DoOneRenderPass) only runs
		//! with mRendering true -- so a test subclass can stamp a STABLE
		//! "everything after this point was scheduled by the post-shrink
		//! layout" epoch.  Without such an epoch a test that clears its
		//! recorder and then calls SetViewportLayout has an unsynchronised gap
		//! in which a legitimate pre-shrink pass (an idle-refinement tick on
		//! the still-visible, gesture-pinned pane) can land and masquerade as a
		//! post-shrink scheduling violation.
		//!
		//! SCOPE of the stability guarantee: it covers state written ONLY from
		//! inside the pass body, under the mRendering gate.  It does NOT extend
		//! to state written from the UNLOCKED pass tail that runs after
		//! DoOneRenderPass returns and after ActiveFlipGuard publishes
		//! mRendering=false -- DrainSharedDirectPublishes_, the output detach /
		//! ForTest_OnInteractivePassBeforeOutputDetach seam, the Last Render
		//! drain.  A recorder driven from one of THOSE seams can still be
		//! running concurrently with this branch's continuation, so it gets no
		//! epoch guarantee here.  No-op in production.
		virtual void ForTest_OnViewportShrinkParked() {}

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
		//! Called by coordinated submission immediately before it attempts the
		//! render-admission mutex used by interaction begin/finalize transitions.
		virtual void ForTest_OnCoordinatedRenderAdmissionAttempt() {}

		//! Direct-render teardown RED-PROVE hook. Called after a
		//! RunPreviewRenderParked caller has registered its lifetime with
		//! Stop(), but before it tries to acquire render admission. A test
		//! can hold a pre-existing caller here and prove terminal Stop()
		//! drains it even though it has not yet become the render occupant.
		//! No-op in production.
		virtual void ForTest_OnDirectRenderBeforeAdmission() {}

		//! Fix-round-6 (save-vs-render race) RED-PROVE test hook.  Called
		//! by RequestSave, unlocked, immediately AFTER its step-1 lock
		//! scope has set mSaving=true and released mMutex and AFTER it has
		//! reserved the per-target save lease, but BEFORE SaveEngine::Save
		//! runs.  A test can hold RequestSave open here
		//! to deterministically widen the real (normally sub-millisecond)
		//! window during which mSaving is true and RenderLoop's mint-site
		//! re-check must bounce rather than mint -- without this seam,
		//! proving the race requires racing real file IO, which is not
		//! deterministic.  No-op in production.
		virtual void ForTest_OnSaveEngineAboutToRun() {}

	private:
		// ---- Node-graph spotlight / focused-view shared helpers --------
		// See AppearanceClosureForObject and ReadPainterMaterialGraphLaidOutFocused
		// (both public, above) for the two consumers of these three.

		//! Object -> live bound material name, locking-safe (try_to_lock,
		//! degrade rather than block behind a render). See the .cpp
		//! definition's own comment for the full rationale; shared by
		//! AppearanceClosureForObject and
		//! ReadPainterMaterialGraphLaidOutFocused's Object-category case.
		String ResolveObjectMaterialNameLocked_( const String& objectName, bool* outDegraded = nullptr ) const;

		//! BFS from `startIdx` over `g` via outEdges only (the "downstream
		//! reference" / "this node's own inputs" direction). Returns
		//! visited indices in discovery order, `startIdx` first. PURE and
		//! static -- see the .cpp definition's own comment.
		static std::vector<unsigned int> BFSGraphClosure( const PainterMaterialGraph& g, unsigned int startIdx );

		//! Filter `g` down to exactly the nodes named by `keep` (indices
		//! into `g.nodes`, caller's own order), remapping every port's
		//! `otherNode` into the new index space or to `kInvalidNodeIndex`
		//! when the target is not itself kept. PURE and static -- see the
		//! .cpp definition's own comment.
		//!
		//! Contract (review-round P3-1 fix on 0562b9c4): tolerates an
		//! out-of-range entry anywhere in `keep` -- it is silently skipped
		//! and every OTHER entry's remap is still exactly correct (not
		//! shifted by the skip count, the bug this fix closed). `keep` is
		//! documented as a SET of node indices; a caller that repeats one
		//! gets an unspecified but non-crashing remap for that index's
		//! incoming edges. BFSGraphClosure's own output already satisfies
		//! both (clean, no duplicates) -- this contract exists for future
		//! callers of this now-shared helper, not the one caller today.
		static PainterMaterialGraph FilterPainterMaterialGraph(
			const PainterMaterialGraph& g, const std::vector<unsigned int>& keep );

		bool PrepareForDestructionClaimed_();
		void RenderLoop();
		void KickRender();

		//! Named Views / Home helper: capture the CURRENT view (the free-fly
		//! pose if active, else the active scene camera under cancel-and-park)
		//! into `out`.  False when there is no capturable camera.
		bool CaptureCurrentView( CameraSnapshot& out );

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
		//! Model-B F2 slice S3: `pinned` marks this submission (once
		//! accepted) as a PINNED render in the slot bookkeeping
		//! (mAgentRenderPinned + mCurrentRenderJob.pinned) -- see
		//! SubmitAgentRenderAsync's doc for the refusal policy this
		//! enables (a pinned occupant refuses ANY new submission).
		//! Model-B F2 slice S4: `renderClass` (default AgentPreview) tags
		//! the minted mCurrentRenderJob.renderClass -- see
		//! SubmitAgentRenderAsync's overload doc.
		//! ROUND-10 finding 3: map a FinalizeOpenInteractions() == false
		//! into the right RenderRefusal.  FinalizeOpenInteractions returns
		//! false for two structurally different reasons, and only one of
		//! them can ever succeed on a retry:
		//!   * mInteractionPersistenceFailed is LATCHED -- a pending CST
		//!     transform/camera commit failed earlier.  That flag is set at
		//!     five production sites plus the test seam, and never cleared
		//!     (see its member doc), and it
		//!     short-circuits FinalizeOpenInteractions unconditionally, so
		//!     every later render and viewport read refuses FOREVER.
		//!     -> InteractionFinalizeLatched (NOT retriable).
		//!   * anything else (a gesture/scrub/composite still open, the
		//!     admission gate taken underneath it) -> InteractionFinalize-
		//!     Failed (retriable).
		//! The discrimination is EXACT rather than a guess: the latched
		//! flag forces the false return, so "set at the refusal site"
		//! is precisely "this refusal cannot clear".
		//!
		//! This deliberately does NOT clear the sticky flag.  The flag's
		//! purpose is that once a live gesture's delta failed to reach the
		//! Document, the controller can no longer claim the Document
		//! matches what the user did -- rendering or handing out viewport
		//! pixels from that state would publish a scene the user never
		//! authored.  The round-10 fix is to stop LYING about it, not to
		//! weaken it.
		RenderRefusal ClassifyFinalizeFailure_() const;

		//! ROUND-10 finding 2b: `outRefusal` (optional) receives the
		//! RenderRefusal cause on every `return false` here.  Unlike the
		//! public entry points this helper does NOT seed it -- the caller
		//! that owns the out-param seeds `None` before its own pre-flight
		//! checks, and this helper only ever overwrites on refusal.
		bool SubmitAgentRenderAsync_Locked(
			std::unique_lock<std::mutex>& slotLk,
			std::function<void()>         fn,
			const String&                 clientLabel,
			RenderJobId*                  outJobId,
			bool                          bypassFairQueueCheck,
			bool                          pinned = false,
			RenderClass                   renderClass = RenderClass::AgentPreview,
			RenderRefusal*                outRefusal = nullptr );

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
		//! doc 88 S4b: `occ` selects WHICH occurrence to capture (0 = first) and
		//! MUST equal the occurrence the coming write addresses -- capture and
		//! write reading different lines is exactly how an Undo comes to restore
		//! the wrong one.  Defaults to 0, the pre-S4b convention.
		bool CaptureAgentPriorParamValue_(
			const String& entityName, const String& entityKind, const String& param,
			String& outPrevValue, bool& outWasAbsent, int occ = 0 );

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

		//! 87: is the agent's target a CONTAINER node -- a `standard_object`
		//! that names no `geometry`?  Read from the DOCUMENT (which is what the
		//! agent path mutates) using CaptureAgentPriorParamValue_'s verbatim
		//! resolution, so the two cannot drift apart.  FALSE for an
		//! unresolvable target, for any other chunk role, and when there is no
		//! retained Document.
		bool AgentTargetIsContainerObject_( const String& entityName, const String& entityKind );


		//! Model-B F5 slice S2: the SHARED body of ApplyAgentInsertChunk /
		//! ApplyAgentRemoveChunk -- the two verbs differ ONLY in which Job
		//! primitive runs inside the parked critical section and in their
		//! rejection wording, so one core carries the whole reviewed commit
		//! pattern (mTxnOpen refusal FIRST -> park under mMutex -> pre-flight
		//! refusals -> conflict gate -> apply -> rebind on 2/3 -> code fold ->
		//! MarkCstHeadDirty + kick on a mutated head).  `isInsert` selects the
		//! primitive; `a` = chunkText (insert) or target name (remove); `b` =
		//! unused (insert) or kind (remove).
		//! R1a (2026-08-09, batched remove_chunks): the batch sibling of ApplyAgentChunkCrud_ -- the whole
		//! critical section for ApplyAgentRemoveChunks (see that method's doc).  Kept SEPARATE from
		//! ApplyAgentChunkCrud_ deliberately: the batch takes a LIST, captures a WHOLE-DOCUMENT undo payload
		//! rather than a per-chunk bytes+index capture, and pushes a DIFFERENT history op.  Everything the two
		//! genuinely share lives one level down, in Job::ApplyCstRemoveChunks.  Caller holds
		//! mRenderAdmissionMutex and has already cleared the agent-render gate; this takes mMutex itself.
		AgentCommitResult ApplyAgentRemoveChunksCrud_(
			const std::vector<String>& targets,
			const std::vector<String>& kinds,
			const RISE::Cst::CstHeadVersion* baseVersionOrNull );

		//! R2 (2026-08-10, replace_geometry_scaffold): the whole critical section for
		//! ApplyAgentReplaceGeometry (see that method's doc).  Kept SEPARATE from ApplyAgentRemoveChunksCrud_
		//! for the same reason that one is separate from ApplyAgentChunkCrud_: it takes a whole-document text
		//! rather than a target list, calls a different Job primitive, and pushes a different history op.
		//! Caller holds mRenderAdmissionMutex and has already cleared the agent-render gate; this takes
		//! mMutex itself.
		//!
		//! doc-88 Phase 3 S20: GENERALIZED (not copied) so the canvas's
		//! positioned Duplicate-node rides the SAME commit block.  A positioned
		//! insert needs exactly this undo shape and no other: its inverse cannot
		//! be `AgentInsertChunk`'s, because that op's REDO replays through
		//! `Job::ApplyCstInsertChunk`, whose TIER heuristic would put the copy
		//! somewhere else than the forward commit did.  A byte-exact prior/post
		//! text pair is position-agnostic in both directions.  The `entityKind` /
		//! `noun` / `appliedPhrase` / `duplicateNodeOp` parameters are the ONLY differences between
		//! the two verbs -- deliberately parameters rather than a second copy of
		//! the mTxnOpen refusal + cancel-and-park + conflict gate + rebind +
		//! history-push + epoch-bump + kick sequence, which is precisely the
		//! "two policies free to drift" shape this file keeps warning about.
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

		//! Shared core for the environment radiance-param setters (scale /
		//! background / orient / radiance_map bind+unbind): under the
		//! cancel-and-park critical section, apply the edit LIVE to the active
		//! rasterizer (SetRasterizerParameter -> rebuild) then MIRROR it into
		//! the retained CST (Job::ApplyCstEnvironmentEdit) so it survives a
		//! save.  Refuses (false) inside an open transaction (not undoable),
		//! when there is no active rasterizer, or when the active rasterizer
		//! takes no radiance map (MLT).  An empty `value` for `radiance_map`
		//! UNBINDS (live: no map; CST: erase all four radiance_* params).
		//! `*outPersisted` (if non-null) reports whether the CST MIRROR recorded
		//! the edit (true) or no-oped because the active rasterizer has no unique
		//! chunk in the Document (false -> the live edit stands but a save would
		//! drop it) -- callers surface that so a bind is never falsely "applied".
		bool SetEnvironmentRadianceParam_( const char* paramName, const std::string& value,
			bool* outPersisted = nullptr );

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
		//! `activeRast` (GUI render modes P2a addition): the rasterizer THIS
		//! pass is about to drive -- mVariantRasterizer while a BeautyVariant
		//! mode is active, else mInteractiveRasterizer -- so the FrameStore
		//! wiring follows whichever rasterizer actually renders the pass
		//! instead of always targeting mInteractiveRasterizer.
		void EnsureInteractiveFrameStore_( unsigned int width, unsigned int height, IRasterizer* activeRast );

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

		// Builds `mode`'s view-mode caster and installs it via
		// InteractivePelRasterizer::SetViewModeCaster.  `mode` must be a
		// casterFactory mode (Normals/Depth/Facets/Wireframe); callers
		// already hold mMutex with the render parked.  Returns false
		// (nothing installed) on the "shouldn't happen" caster-factory
		// failure -- same fail-closed contract SetViewportRenderMode had
		// inline before this was factored out.  No x-ray argument -- the
		// built caster starts at the factory default (false) and
		// SetViewModeCaster immediately re-stamps it with the rasterizer's
		// CURRENT x-ray flag (InteractivePelRasterizer::SetXrayView /
		// ApplyXrayViewToCaster_), so this stays correct with no plumbing
		// here.
		bool InstallViewModeCaster_( Implementation::ViewportRenderMode mode );

		IJobPriv&                   mJob;
		IRasterizer*                mInteractiveRasterizer;  // borrowed
		// Cached downcast of mInteractiveRasterizer for the polish-pass
		// path (SetSampleCount).  Null if the rasterizer isn't an
		// InteractivePelRasterizer (e.g. test mode with no rasterizer).
		Implementation::InteractivePelRasterizer* mInteractiveImpl;
		// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): the ephemeral,
		// CONTROLLER-OWNED BeautyVariant pipeline (deep_reflect / direct) --
		// a separate IRasterizer the render loop DRIVES while a variant mode
		// is active, NOT a caster swap on mInteractiveRasterizer.  Owned
		// (refcounted) reference; null when no variant mode is active.
		// Mutated ONLY under mMutex with the render parked (SetViewportRenderMode),
		// released in the destructor and by RebindEditorToJob's every-scene-
		// load reset.  Never touched from ~SceneEditController-adjacent mJob
		// access (this member is entirely rasterizer/caster state, no Job
		// reference held).
		IRasterizer*                mVariantRasterizer;
		// GUI render modes P1 (docs/gui/RENDER_MODES.md §5): the currently
		// active viewport render mode.  Mutated ONLY under mMutex (by
		// SetViewportRenderMode and by RebindEditorToJob's every-scene-load
		// reset-to-Preview); Preview at construction and after every reset.
		Implementation::ViewportRenderMode mViewportRenderMode;
		// X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): the currently
		// active x-ray flag.  Mutated ONLY under mMutex (by SetViewportXray
		// and by RebindEditorToJob's every-scene-load reset-to-false).
		// DEFAULT OFF: false at construction and after every reset -- the
		// viewport shows transmissive surfaces normally. Mirrored
		// onto the interactive rasterizer via
		// InteractivePelRasterizer::SetXrayView at both of those sites.
		bool                                mViewportXray;
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
		//! Full-resolution film pixels per current primary-pane surface
		//! pixel. Handle layout and ring hit tolerance are multiplied by
		//! this value so their visible/clickable size stays stable when a
		//! high-resolution film is shown in a smaller viewport.
		double                      mGizmoPixelScale = 1.0;

		//! Navigation axis-ball nubs, recomputed by RefreshNavGizmo (Tier 2
		//! §4).  0 or 6 entries; read out through the count + per-index getter.
		std::vector<NavGizmoNub>    mNavGizmoNubs;

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
			double  pixelScale = 1.0; ///< film pixels per displayed pixel at drag-start
			double  axisDirX[3];      ///< pixels per world unit, x component
			double  axisDirY[3];      ///< pixels per world unit, y component
			bool    axisOk[3];        ///< false if axis colinear with view at drag-start
			Vector3 prevOrient;       ///< object Euler at drag-start (for Rotate)
			Matrix4 dragStartMatrix;  ///< authoritative LOCAL-matrix anchor for scale drag (87: the
			                          ///< scale is applied along the node's own axes, so a world
			                          ///< anchor would bake a parent's transform into a child).
			TransformStateV2 dragStartState;  ///< exact components, stack entries, and
			                                ///< authoritative-matrix metadata at drag-start.
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
		// (kNumCategories moved PUBLIC next to the Category enum -- GUI
		//  redesign 2026-07-22: the free-function PropsForCat mirrored it as
		//  a literal that went stale TWICE; public visibility removes the
		//  mirroring hazard for good.)
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
		bool                        mTimelineScrubWatchdogRecovered = false;
		//! Guarded by mMutex.  Bridges the interval between a BeautyVariant
		//! interaction's admission and publication of its normal pointer/scrub
		//! flag.  If cancellation retirement re-mints in that interval, the
		//! replacement is still configured at live quality, never full OIDN.
		bool                        mVariantInteractionAdmissionPending = false;
		unsigned int                mVariantInteractionAdmissionPane = 0;

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
		//! user-review P1#3: which pane's pixels mInteractiveFrameStore
		//! currently holds.  Written under mInteractiveFrameStoreMutex
		//! wherever the store register is (re)assigned, so a reader under
		//! that same lock gets a consistent (frame, pane) pair.
		mutable unsigned int              mInteractiveFrameStorePane = 0;

		// Toolkit slice 1 (read_viewport): a LEAF mutex guarding ONLY the
		// `mInteractiveFrameStore` POINTER swap -- nothing else.  The store's
		// pixel CONTENT is guarded independently by FrameStore's per-tile
		// shared_mutex (which a reader acquires transitively via
		// `DumpImage`); this mutex exists solely so the cross-thread reader
		// `CopyInteractiveFrame` can snapshot-and-addref the pointer without
		// racing `EnsureInteractiveFrameStore_`'s release-old/reassign in the
		// dims-changed branch (which, pre-slice-1, ran with ZERO
		// synchronization -- safe only while the render thread was the SOLE
		// accessor).  LEAF: `EnsureInteractiveFrameStore_` runs entirely
		// OUTSIDE `mMutex` (DoOneRenderPass is called by RenderLoop AFTER it
		// releases mMutex), so this lock never nests with mMutex /
		// mJobStatusMutex / mAsyncCacheMutex / mAgentRenderSlotMutex -- it is
		// deliberately NOT part of the lock-order table.  Only the
		// dims-changed reassignment takes it; the same-dims short-circuit
		// reads the pointer on the render thread itself (sequenced after that
		// thread's own prior write) and needs no lock.
		mutable std::mutex mInteractiveFrameStoreMutex;

		CancellableProgressCallback mCancelProgress;

		// Render-thread machinery -----------------------------------

		std::thread                 mRenderThread;
		mutable std::mutex          mMutex;
		std::condition_variable     mCV;
		std::atomic<bool>           mRunning;
		std::atomic<bool>           mEditPending;

		//! Secure-MCP slice 5a: proposal bookkeeping has its own leaf mutex
		//! so an External proposal/list request never waits behind the
		//! render-duration scene lock. ResolveProposal still holds render
		//! admission across snapshot -> optimistic ApplyAgent* -> status
		//! publication, so no second resolver/commit can interleave; the
		//! ApplyAgent* base-version gate remains the authoritative atomic
		//! check-and-apply.
		//!
		//! A resolved (applied/rejected/conflict) proposal is KEPT
		//! on the queue for audit rather than purged at resolve time -- but
		//! see Secure-MCP slice 6 below, which bounds how far that audit
		//! trail can grow: this controller's lifetime is one Job/one
		//! scene-load (LoadAsciiSceneViaCst refuses a second load on the
		//! same Job, so "reload" in practice means a brand-new Job + brand-
		//! new controller, which starts with an empty mProposals by
		//! construction), but a single long-running session hosting an
		//! attached external MCP client for hours could otherwise grow this
		//! vector without limit.
		//!
		//! Secure-MCP slice 6 (limits hardening): TWO independent caps, both
		//! enforced in StageProposal --
		//!   * kMaxPendingProposals bounds how many entries may be
		//!     status=="pending" AT ONCE.  StageProposal REFUSES (returns 0,
		//!     never enqueuing) a new proposal once the pending count is
		//!     already at the cap -- fail-closed: it does NOT silently drop
		//!     or overwrite anyone's existing pending proposal, it just
		//!     declines to add a new one until the Owner resolves (approves
		//!     or rejects) enough of the existing backlog. This is what
		//!     stops an unbounded backlog of NEVER-RESOLVED proposals from
		//!     accumulating (a resolve requires an Owner-authority session
		//!     to actively act; nothing else drains "pending").
		//!   * kMaxProposalHistory bounds the TOTAL size of this vector
		//!     (pending + resolved).  Once at that cap, StageProposal evicts
		//!     the SINGLE OLDEST resolved (non-pending) entry before
		//!     appending the new one -- so the audit trail is a bounded
		//!     sliding window of the most recent history rather than
		//!     unbounded, while a pending entry is NEVER evicted to make
		//!     room (only resolved entries are eviction candidates). Given
		//!     kMaxPendingProposals <= kMaxProposalHistory, whenever the
		//!     total is at the history cap there is ALWAYS at least one
		//!     resolved entry to evict (pending count is bounded strictly
		//!     below the history cap by the sibling gate above), so this
		//!     eviction can never stall or need to fall back to dropping a
		//!     pending proposal.
		mutable std::mutex          mProposalMutex;
		std::vector<AgentProposal> mProposals;
		//! Monotonic proposal-id counter; starts at 1 (0 is the "not found"
		//! sentinel returned by StageProposal only if ever called before
		//! construction — never in practice, since the counter is a plain
		//! member initialized at construction). Guarded by mProposalMutex,
		//! same as mProposals.
		std::uint64_t               mNextProposalId = 1;
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
		// RequestSave; cleared after the engine returns.  Fix-round-6:
		// consulted at the SAME two sites as mAgentRenderBlocksInteractive
		// below (the wake predicate's snapshot AND the mint block's own
		// in-lock re-check immediately before minting) for the identical
		// reason -- RequestSave's mSaving=true store happens under mMutex
		// but can land anywhere in the ~60 unlocked lines between the
		// snapshot and the mint, so only the in-lock re-check is
		// authoritative.
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
		// Serializes the short admission handshake between a long
		// coordinated/direct render and UI operations that must acquire
		// mMutex without ever queuing behind that render. Lock order:
		// mAgentRenderSlotMutex (when present) -> this mutex -> mMutex.
		// It is never held for the render itself; only until the gate below
		// is published or a UI mutation/gesture/save is complete. Recursive
		// because several public UI operations deliberately compose other
		// public operations (axis snap -> enter free-fly -> set pose; pane-0
		// aliases -> legacy setters) on the same UI thread. Cross-thread
		// exclusion semantics remain identical to a plain mutex.
		mutable std::recursive_mutex mRenderAdmissionMutex;
		std::atomic<bool>           mAgentRenderBlocksInteractive;
		//! Direct RunPreviewRenderParked calls execute on their caller's
		//! thread, outside the dedicated agent worker's joinable lifetime.
		//! This leaf state lets terminal Stop() close registration and drain
		//! every caller that entered before closure, including callers still
		//! waiting to acquire mRenderAdmissionMutex.
		mutable std::mutex          mDirectRenderStateMutex;
		std::condition_variable     mDirectRenderDoneCV;
		//! Both guarded by mDirectRenderStateMutex. The count is incremented
		//! at method entry, before admission, and decremented as the caller's
		//! final controller access. mDirectRenderStopping is terminal:
		//! Stop() sets it before teardown so no later caller can register
		//! behind the zero-count predicate.
		unsigned int                mDirectRenderCallCount;
		bool                        mDirectRenderStopping;
		//! Per-direct-occupant cancellation latch. The direct path must reset
		//! the shared progress token after draining an interactive pass; this
		//! bit preserves an explicit Stop/render_cancel that arrives before
		//! that reset, exactly as mAgentRenderCancelRequested does for the
		//! dedicated worker.
		std::atomic<bool>           mDirectRenderCancelRequested;
		std::string                 mLastSaveError;
		std::atomic<unsigned int>   mCancelCount;
		std::atomic<unsigned int>   mAgentCancelRequestCount;   // see ForTest_GetAgentCancelRequestCount
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
		// The controller's render-coordination locks are deliberately narrow
		// and single-purpose -- the split is the
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
		// Nesting order is the ONLY order used anywhere, so these locks
		// cannot deadlock against each other: mAgentRenderSlotMutex may be
		// held OUTSIDE a brief, nested mRenderAdmissionMutex + mMutex or
		// mJobStatusMutex
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
		//          -> SceneEditController::mRenderAdmissionMutex
		//               -> SceneEditController::mMutex      (brief, mAgentRenderBlocksInteractive)
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
		//! True iff CancelAgentRender_ observed the current slot occupant.
		//! Written while mAgentRenderSlotMutex is held, but atomic because the
		//! worker re-checks it after taking mMutex (the two locks deliberately
		//! never nest in that direction). Cleared for every fresh submission
		//! and again when the occupant releases the slot, preventing a cancel
		//! for one job from leaking into its successor.
		std::atomic<bool>           mAgentRenderCancelRequested;
		//! Lock-free "a production/agent render owns the scene" flag: true while
		//! the render CLOSURE runs under the mMutex render-hold.  Set by a RAII
		//! scope around BOTH fn() sites -- the async AgentRenderWorkerLoop_ and the
		//! sync RunPreviewRenderParked (RenderClass::AgentPreview; despite the name
		//! it runs an AGENT render, not a brief interactive preview).  UI-callable
		//! mMutex methods check this FIRST and early-return a no-op (mutators) /
		//! default (getters) WITHOUT taking mMutex, so a main-thread call during a
		//! render no-ops instead of blocking.  It SHRINKS the wedge window from the
		//! whole render duration to the render-SETUP interval only: the flag is set
		//! just AFTER mMutex is acquired, so the park+jobId-mint prologue runs under
		//! mMutex with the flag still false -- a UI call landing in that ~microsecond
		//! window still wedges once, then self-heals (the next call sees the flag).
		//! Scoped to fn() (not the whole mMutex hold) on purpose: CancelAndParkRender_
		//! RELEASES mMutex in its mCV.wait, so a wider scope would false-no-op edits
		//! while mMutex is momentarily free.  NOT set for the interactive RenderLoop
		//! (brief per-pass mMutex holds) nor the no-mMutex UI-thread RasterizeScene
		//! path.  Render-lifecycle methods (Stop*, WaitForRenderJob, worker/loop) are
		//! deliberately NOT guarded.
		std::atomic<bool>           mRenderOwnsScene;
		// Fix-round-1 P3-c: mAgentRenderClass / mAgentRenderClientLabel
		// (a second, slot-scoped copy of this bookkeeping) were DELETED --
		// both were write-only dead state (set on every submit, never read
		// by anything).  mCurrentRenderJob.renderClass / .clientLabel
		// (guarded by mJobStatusMutex, populated at the SAME submit sites)
		// already carry the identical information to the surface that
		// actually reads it (GetRenderJobStatus / CurrentRenderJob).
		RenderJobId                 mAgentRenderJobId;     // the id assigned to the CURRENT slot occupant -- guarded by mAgentRenderSlotMutex
		//! Model-B F2 slice S3: true iff the CURRENT slot occupant
		//! (mAgentRenderJobId) was submitted PINNED.  Guarded by
		//! mAgentRenderSlotMutex, same as the rest of the slot bookkeeping
		//! -- SubmitAgentRenderAsync_Locked's single-slot check reads this
		//! to decide whether a new submission gets the pinned-specific
		//! refusal or the ordinary busy one.  A second, independent copy
		//! of the SAME fact also lives in mCurrentRenderJob.pinned
		//! (guarded by mJobStatusMutex) for the STATUS-READ side
		//! (GetRenderJobStatus / CurrentRenderJob) -- mirrors the existing
		//! split between mAgentRenderJobId and mCurrentRenderJob.id (one
		//! lock guards "can a NEW submission proceed", the other guards
		//! "what does a STATUS POLL see", and neither may block behind
		//! the other -- see the three-lock note above).
		bool                        mAgentRenderPinned;
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
		//! Guards CameraCommon frame/matrix reads against DoOneRenderPass's
		//! temporary ResizeFilm swap.  UI overlays take this with try_lock so
		//! they defer rather than block during an interactive pass.
		std::mutex                  mCameraFrameMutex;

		std::atomic<unsigned int>   mPreviewScale;
		// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): true while a
		// BeautyVariant mode (deep_reflect/direct) is active.  Every site
		// that would otherwise mutate mPreviewScale (the idle-refinement
		// walk-down, the during-motion adaptation ladder, and every
		// gesture-driven reset -- OnPointerDown/Move/Up, OnTimeScrubBegin/End,
		// Begin/EndPropertyScrub) checks this flag first and no-ops when set,
		// so a variant mode's fixed resolution divisor (pinned into
		// mPreviewScale itself by SetViewportRenderMode) can never drift.
		// Mutated ONLY under mMutex by SetViewportRenderMode / RebindEditorToJob.
		std::atomic<bool>           mPreviewScalePinned;
		//! Store `v` into mPreviewScale UNLESS a variant mode has it pinned
		//! (see mPreviewScalePinned's doc) -- the single choke point every
		//! gesture-driven reset site uses instead of a bare
		//! `mPreviewScale.store(...)`, so the pin can never be bypassed by a
		//! future call site that forgets to check it.
		void SetPreviewScaleIfUnpinned_( unsigned int v );
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

		// Pointer-gesture watchdog (round-8 review P2 fix).  T0 made a
		// completed quantum stop self-arming the rotation while a gesture
		// pins the scheduler, which is what killed the pinned-pane spin --
		// but it also means a gesture flag that is never cleared freezes
		// every sibling pane for good.  That is reachable in a live
		// session: a shell can DROP the pointer-up (both viewports guard
		// their pointer handlers on an `interactionEnabled` flag that a
		// chat/agent render request flips, while the core-side render is
		// separately REFUSED because a gesture is open -- so no teardown
		// path runs either).  StopInteractive covers teardown and
		// kScrubWatchdogMs covers a lost property-End; this covers the
		// remaining live-session pointer case.  MUCH longer than the scrub
		// window: a held-still drag is legitimate and common (the user is
		// looking, not dragging), and the recovery ends that gesture, so
		// the threshold must be well past any plausible deliberate pause.
		// The stale state releases sibling rotation while the pointer is idle.
		// A later Move atomically reclaims mGesturePane and resumes the same
		// composite, so a legitimate long hold is not made inert.  A render
		// request instead calls FinalizeOpenInteractions before admission,
		// closing the composite and clearing raw mPointerDown.
		static constexpr int        kPointerWatchdogMs = 10000;

		//! Live threshold for the arm above.  A member (not the constant
		//! directly) purely so the regression test can shorten it -- a test
		//! that really waited 10 s would be a 10 s test.  Production never
		//! writes it.
		std::atomic<int>            mPointerWatchdogMs { kPointerWatchdogMs };

		// Time of the most recent KickRender (ms since steady-clock
		// epoch).  Read by the render thread to decide whether the
		// pointer has been idle long enough to refine.  Read by
		// OnPointerMove to decide whether to snap scale back up after
		// a pause.
		std::atomic<long long>      mLastEditTimeMs;

		// Round-8: set by the render thread's kPointerWatchdogMs arm when a
		// pointer gesture looks abandoned (see that constant).  A STALE
		// gesture stops pinning the scheduler and stops suppressing the
		// end-of-quantum rotation arm, so frozen siblings recover -- but
		// mPointerDown itself stays TRUE on purpose, so a late-arriving
		// OnPointerUp still takes its normal finalization path (composite
		// close + pending-CST commit).  Clearing mPointerDown here instead
		// would make that pointer-up early-return and strand the open
		// composite, which blocks agent D2 renders indefinitely.
		// OnPointerMove parks any sibling pass, reloads mGesturePane, and then
		// clears stale before applying the resumed delta.  Cleared by every
		// gesture-boundary path: pointer down/up on either entry point,
		// StopInteractive's orphan cleanup, and the layout-shrink clear.
		std::atomic<bool>           mPointerGestureStale{ false };
		//! Sticky truth flag for a gesture-finalization route failure.  Commit
		//! helpers consume their pending sets even on failure, so admitting a
		//! later render as though persistence succeeded would silently lose the
		//! live edit on re-derive.  Finalize/Prepare report false once tripped.
		//!
		//! Set at five PRODUCTION sites, plus the
		//! ForTest_TripInteractionPersistenceFailure test seam (a sixth
		//! store(true) in this same header); NEVER cleared, deliberately -- see
		//! ClassifyFinalizeFailure_ for why that is the right safety
		//! behaviour and what round 10 changed instead.  The CONSEQUENCE a
		//! reader must know: once tripped, FinalizeOpenInteractions returns
		//! false unconditionally, so EVERY later coordinated render and
		//! EVERY read_viewport refuses for the life of this controller.
		//! That refusal is reported as RenderRefusal::Interaction-
		//! FinalizeLatched and surfaced to the model as explicitly NOT
		//! retriable; do not re-describe it anywhere as "retry shortly".
		std::atomic<bool>           mInteractionPersistenceFailed { false };
		//! ROUND-10 finding 3: one-shot TEST-ONLY forcing of a TRANSIENT
		//! FinalizeOpenInteractions failure -- see
		//! ForTest_FailNextFinalizeOpenInteractions.  Always false in
		//! production; the single read is an already-cheap acquire on a path
		//! that runs once per render admission, not per sample.
		std::atomic<bool>           mForTestFinalizeFailOnce { false };

		//! Round-8 review P1 fix: POINTER-ACTIVITY clock for the watchdog
		//! above -- deliberately NOT mLastEditTimeMs.  That timestamp is
		//! stamped only AFTER OnPointerMove's render-completion wait
		//! (`mCV.wait(!mRendering)`) and a successful Apply, so during a slow
		//! pass it does not advance even while the user is actively dragging.
		//! On exactly the panes this feature targets -- BeautyVariant -- an
		//! older full-quality quantum could outlive the next gesture (the live
		//! policy now cancels it and atomically suppresses its OIDN tail), so a
		//! single pass can exceed the watchdog window, and gating on mLastEditTimeMs
		//! would mark a genuinely-live drag stale and silently freeze it.
		//! Stamped at gesture start and at the TOP of OnPointerMove, before
		//! any wait.
		std::atomic<long long>      mLastPointerActivityMs { 0 };

		//! Count of OnPointerMove calls currently
		//! executing (including one parked in the render-completion wait).
		//! The activity stamp alone is not sufficient: while a long pass runs,
		//! the UI thread is BLOCKED inside OnPointerMove, so no new stamp can
		//! arrive, and the loop's watchdog check can win the wake race against
		//! the unblocked move's own stamp.  A move in flight IS pointer
		//! activity by definition, so the watchdog stands down while this is
		//! non-zero.
		std::atomic<int>            mPointerMovesInFlight { 0 };
		//! Timeline counterpart to mPointerMovesInFlight.  Prevents the lost-End
		//! watchdog from closing/reclassifying a scrub callback while that callback
		//! is parked behind an obsolete render quantum.
		std::atomic<int>            mTimeScrubsInFlight { 0 };

		// Set by ~SceneEditController BEFORE Stop(),
		// so StopInteractive's orphaned-gesture cleanup skips its
		// CommitPendingCst* calls on the destructor path.  Those route
		// through mJob (CstObjectTransformKind -> a full D2 re-derive), and
		// this dtor CANNOT touch mJob -- several owners legitimately destroy
		// the Job first (the `controller.Stop(); pJob->release();` pattern
		// with a stack controller destroyed afterwards), which is exactly the
		// use-after-free the dtor's own NOTE documents.  C-ABI destruction
		// first calls PrepareForDestruction while its Job-outlives-controller
		// contract still holds; only raw fallback destruction skips the flush.
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

		// Review-round-1 P1: Start()/StopInteractive() used to synchronize
		// only via the mRunning CAS -- the mRenderThread OBJECT (spawn in
		// Start, join in StopInteractive) had no lock.  With
		// PauseRefinement/ResumeRefinement these become independently
		// fireable user actions (any thread via the C API), and a Resume's
		// Start() landing between a Pause's CAS and its join() would
		// move-assign onto a still-joinable std::thread ->
		// std::terminate().  mLifecycleMutex serializes the whole
		// CAS+spawn / CAS+join sequences.  Never taken by the render
		// thread itself, so holding it across the join cannot deadlock.
		std::mutex                  mLifecycleMutex;

		// User-facing refinement pause (PauseRefinement/ResumeRefinement).
		// TRUE between Pause and Resume; cleared by any Start().  In-class
		// init — the ctor predates this member and doesn't list it.
		std::atomic<bool>           mRefinementPaused{ false };

		// Interactive region-of-interest (SetInteractiveRegion).  The four
		// INCLUSIVE full-res film coords are packed 16 bits each into one
		// atomic (left<<48 | top<<32 | right<<16 | bottom) so the render
		// thread reads a coherent tuple without a lock;
		// mInteractiveRegionActive gates use.  Coords beyond 65535 are
		// clamped at set time (no real film is that large).
		std::atomic<bool>           mInteractiveRegionActive{ false };
		std::atomic<std::uint64_t>  mInteractiveRegionPacked{ 0 };

		// Free-fly viewport pose (Tier 2 §5.3-5.5).  `mViewportPoseActive` gates
		// the render-camera override in DoOneRenderPass; when set,
		// `mViewportOverrideCamera` is a viewport-private, addref'd standalone
		// ICamera (realized from `mViewportPose` via RealizeStandaloneCamera) that
		// the interactive pass renders through IN PLACE OF Scene::pActiveCamera.
		// These are swapped ONLY under the cancel-and-park critical section (the
		// render thread reads mViewportOverrideCamera at the top of a pass), so a
		// plain bool + raw pointer are safe (no atomics needed — set while parked).
		// The override is released on exit / re-realize / destruction.  `mViewportPose`
		// keeps the source snapshot so a film-dim change can re-sync the override.
		CameraSnapshot              mViewportPose;
		bool                        mViewportPoseActive = false;
		ICamera*                    mViewportOverrideCamera = nullptr;

		// Home view (Tier 2 §4.2): a single reserved pose slot, session UI state
		// (never a scene write).  Captured by SetHomeView, restored by GoToHomeView.
		CameraSnapshot              mHomeView;
		bool                        mHasHomeView = false;

		// Named Views (Tier 2 §3): session/UI bookmarks.  Originally UI-
		// thread-only (never read by the render thread), so the vector
		// needed no lock -- GUI render modes P2a's `render{view:}` surface
		// (FindNamedViewPose) added the FIRST reader reachable from a
		// non-UI thread (the agent-render worker), so this mutex now guards
		// every touch (see the "Named Views" section comment in the .cpp).
		mutable std::mutex          mNamedViewsMutex;
		std::vector<NamedView>      mNamedViews;

		// -------- N-up pane model storage: DESIRED state (configs) --------
		// Pane 0's mode/vantage are NOT stored here -- pane 0 is an alias
		// view of mViewportRenderMode + the free-fly ViewportPose state (see
		// the public API doc above).  Slots 1-3 hold validated config that
		// SwitchToPaneLocked_'s reconcile consumes at the mint boundary.
		// Guarded by mMutex (setters lock; the render thread reads ONLY
		// inside the mint lock, so no park is needed in the setters).
		// Default-member-init (house pattern -- see mSectionExpanded's ctor
		// note) so no init-list entries are needed and -Wreorder can't bite.
		struct PaneConfig
		{
			// Value-init {} == enumerator 0 == Preview (the enum is only
			// forward-declared here -- see the line-55 note -- so the
			// enumerator NAME is unavailable; InteractivePelRasterizer.h
			// defines Preview first and a static_assert in the .cpp pins it).
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
		//! Lock-free copy of each secondary pane's desired mode for UI
		//! polling.  A coordinated render can hold mMutex for its duration,
		//! so GetPaneRenderMode must not wait for that lock.  Value-init 0 is
		//! Preview, matching PaneConfig::mode; writers publish under mMutex.
		std::atomic<int>            mPaneModeSnapshots[kViewportPaneCount] {};
		ViewportLayout              mViewportLayout = ViewportLayout::Single;
		unsigned int                mPrimaryPane    = 0;   // always visible in layout
		//! Lock-free UI-read snapshots.  Agent/production renders deliberately
		//! hold mMutex for their whole duration, while platform paint/timer paths
		//! still need the last coherent layout and primary without blocking.
		//! Writers update both under mMutex before publishing the layout snapshot.
		std::atomic<int>            mViewportLayoutSnapshot { 0 };
		std::atomic<unsigned int>   mPrimaryPaneSnapshot { 0 };

		// -------- P3a slice 2: context-switch scheduler state --------
		//
		// THE MODEL (RENDER_MODES.md §7.3 "context-switch model"): the
		// existing scalar members (mPreviewScale, mPolishState,
		// mViewportOverrideCamera/Pose/Active, mVariantRasterizer,
		// mInteractiveFrameStore, the caster installed on mInteractiveImpl)
		// remain the render loop's WORKING REGISTERS, untouched by this
		// slice; each pane slot below is the SAVED copy for a pane that is
		// not currently scheduled.  SwitchToPaneLocked_ saves registers ->
		// slot and loads slot -> registers, ONLY inside the mint lock's
		// mMutex hold (or a caller that has parked, same discipline as every
		// setter).  The refinement LADDER never needs saving: scale > min
		// exists only during a gesture (gesture-end snaps to min -- see
		// OnPointerUp), and gestures pin the scheduler to the gestured pane
		// (PickNext returns the current pane while a gesture is active), so
		// a switch can only happen with the ladder at rest.
		struct PaneRenderState
		{
			bool                         dirty = true;    // needs a pass (scene edit / config change)
			uint64_t                     dirtyGeneration = 1;
			int                          polishSaved = 0; // PolishState as int (None) -- saved register
			//! review-r1 P1: the resolution ladder + variant pin are part of
			//! the register set.  Defaults: full-res (1), unpinned -- what a
			//! fresh single-viewport starts at.
			unsigned int                 previewScaleSaved  = 1;
			bool                         previewPinnedSaved = false;
			ICamera*                     overrideCamera = nullptr;  // realized vantage (owned addref; null => scene camera)
			bool                         poseActive = false;        // saved mViewportPoseActive register
			CameraSnapshot               poseSaved {};              // saved mViewportPose register
			Implementation::FrameStore*  frameStore = nullptr;      // saved store (owned addref; null => allocate lazily)
			IRasterizer*                 variantRasterizer = nullptr;  // saved variant pipeline (owned)
			//! Per-pane view-mode caster instance (owned addref).  PERSISTS
			//! across switches deliberately: depth's auto-window state lives
			//! in the caster, and rebuilding per rotation would reset the
			//! calibration on every quantum.  Null for Preview / variant
			//! modes and until first realized.
			IRayCaster*                  viewModeCaster = nullptr;
			//! What mode the slot's caster/variant were last built for.
			//! Compared against the CONFIGURED mode at switch time; a
			//! mismatch (config edited while unscheduled) triggers the
			//! reconcile rebuild.  Value-init 0 == Preview (static_assert
			//! in the .cpp).
			Implementation::ViewportRenderMode realizedMode {};
			//! P3a slice 3: per-pane sink (owned addref; null => fall back
			//! to the legacy mPreviewSink, which stays pane 0's default).
			IRasterizerOutput*           paneSink = nullptr;
			//! Set by the pane-config setters (mode/vantage) for panes 1-3;
			//! consumed by SwitchToPaneLocked_'s reconcile.  Setters mutate
			//! ONLY desired-state fields under mMutex -- never registers --
			//! so they need no park; ALL register mutation stays on the
			//! render thread inside the mint lock (plus the pane-0 setters,
			//! which park and force-switch to pane 0 first).
			bool                         configDirty = false;
		};
		PaneRenderState             mPaneRender[kViewportPaneCount];
		unsigned int                mCurrentPane = 0;   // render-thread-owned; UI reads under mMutex

		//! One new wake flag for the rotation: set when a quantum completes
		//! and OTHER visible panes still have work.  Deliberately NOT
		//! mEditPending -- that flag's consumption drives isExplicitEdit
		//! bookkeeping (refinement-tick reset, mLastEditTimeMs semantics)
		//! that a rotation continuation must not fake.  Added to the CV wait
		//! predicate alongside mEditPending with the same acquire/consume
		//! shape.
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

		//! Round-8 review P2 fix: deferred last-render sink delivery.  The
		//! publish used to call `sink->OutputImage` with mMutex HELD, which
		//! (a) violates ADR rule 5 ("notifications fire outside locks") and
		//! is a latent deadlock for any future sink that calls back into the
		//! controller, and (b) ran a full per-pixel conversion on the UI
		//! thread inside the lock -- a visible hitch at production
		//! resolutions, blocking every other mMutex caller meanwhile.
		//! Same shape as the editor's DrainDirtyNotification: RECORD under
		//! the lock, DRAIN after releasing it, with a per-frame catch-all in
		//! RefreshProperties so a missed drain site degrades to a one-frame
		//! delay rather than a lost image.
		struct PendingLastRenderPublish
		{
			IRasterizerOutput* sink  = nullptr;   //!< owning addref
			IRasterImage*      image = nullptr;   //!< owning addref (the retained frame or a placeholder)
			unsigned int       pane  = 0;
			unsigned long long sourceGeneration = 0;
			unsigned long long publishGeneration = 0;
		};
		//! Incremented on every content-source transition.  Deferred records
		//! whose captured generation no longer matches are discarded before
		//! they can overwrite a pane that has returned to Interactive output.
		std::atomic<unsigned long long> mPaneContentGeneration[kViewportPaneCount] {};
		//! Latest queued image identity per pane.  A newer production frame or
		//! sink replacement invalidates an older detached batch even when the
		//! content source itself has not changed.
		std::atomic<unsigned long long> mPaneLastRenderPublishGeneration[kViewportPaneCount] {};
		//! LEAF mutex: guards the queue only.  Acquired while holding mMutex
		//! (record side) and alone (drain side) -- never the reverse, and no
		//! other lock is ever taken while it is held.
		mutable std::mutex          mLastRenderPublishMutex;
		std::vector<PendingLastRenderPublish> mPendingLastRenderPublishes;
		//! Superseded/evicted records whose owning refs must be released only
		//! after controller and queue locks are dropped.  Normal setter/render
		//! tails drain this immediately; teardown drains any residual entries.
		std::vector<PendingLastRenderPublish> mRetiredLastRenderPublishes;
		//! One callback/start barrier for every pane.  A global recursive barrier
		//! deliberately serializes arbitrary sink callbacks: per-pane locks allow
		//! two callbacks to re-enter setters for each other's panes and AB/BA
		//! deadlock.  Recursive is required for same-thread controller re-entry;
		//! nested drains themselves are deferred until the outer callback returns.
		std::recursive_mutex        mLastRenderDeliveryMutex;

		//! Every DrainLastRenderPublishes_ call registers before touching queue,
		//! barrier, or callback state.  Job-live teardown closes registration and
		//! waits for the active count to reach zero, preventing deletion while a
		//! callback or detached batch still uses controller members.
		mutable std::mutex          mLastRenderDrainStateMutex;
		std::condition_variable     mLastRenderDrainDoneCV;
		unsigned int                mLastRenderDrainCalls = 0;
		bool                        mLastRenderDrainsStopping = false;
		bool         BeginLastRenderDrain_();
		void         EndLastRenderDrain_();
		void         QuiesceLastRenderDrains_();

		//! Record a pane's last-render delivery.  REQUIRES mMutex held.
		//! Coalesces per pane (only the newest frame matters), which also
		//! bounds the queue at kViewportPaneCount entries.
		void         QueueLastRenderPublishLocked_( unsigned int pane );
		//! Deliver every queued frame and retire displaced records.  MUST be called with NO controller lock
		//! held -- including mRenderAdmissionMutex, which the pane setters
		//! take (holding it across a sink callback is the same ADR rule-5
		//! hazard as holding mMutex).  Safe to call when the queue is empty
		//! (the common case).
		//! `blocking` controls whether the global callback/start barrier may wait;
		//! stale detached batches are generation-skipped.
		void         DrainLastRenderPublishes_( bool blocking = true );

		//! P3a slice 3: the CURRENT pane's surface dims, stamped at the
		//! mint (inside the same lock hold as the context switch) and read
		//! by DoOneRenderPass -- registers, like every per-quantum input.
		//! 0 = use film rest dims.
		unsigned int                mCurrentPaneSurfaceW = 0;
		unsigned int                mCurrentPaneSurfaceH = 0;

		//! P3a slice 3: which pane the ACTIVE gesture targets.  Written by
		//! OnPanePointerDown (under mMutex, parked) before the gesture pin
		//! arms; read by the un-indexed OnPointerDown's force-switch block
		//! (which generalizes from "switch to 0" to "switch to the gesture
		//! pane").  Legacy un-indexed input leaves it 0 -- byte-identical
		//! single-viewport behaviour.
		unsigned int                mGesturePane = 0;
		//! One-shot: set by OnPanePointerDown (under mMutex) so the
		//! forwarded un-indexed Down keeps the armed pane instead of
		//! resetting to 0; consumed exactly once.
		bool                        mGesturePaneArmed = false;

		//! P3a slice 3: apply a camera-op edit to the CURRENT pane's
		//! realized override camera (per-pane fly) -- the canonical
		//! SceneEditor::ApplyCameraOpToCamera math, then re-snapshot the
		//! pose and kick.  Requires a camera-motion edit and
		//! mViewportPoseActive; parks internally.
		bool ApplyPaneFlyOp_( const SceneEdit& edit );

		//! Scheduler helpers -- ALL require mMutex held (and the render
		//! parked where they touch registers); see each impl's doc.
		void         SwitchToPaneLocked_( unsigned int pane );
		//! Builds a BeautyVariant pipeline for `mode` (production-default-
		//! shader recovery included) -- the factory block extracted from
		//! SetViewportRenderMode so the scheduler's per-pane lazy build and
		//! the pane-0 setter share ONE implementation.  Requires mMutex held.
		//! False (and *out untouched) on factory failure.
		bool         BuildVariantRasterizer_( Implementation::ViewportRenderMode mode,
		                                      IRasterizer** out );
		//! Begins a BeautyVariant interaction while mMutex is held: pins any
		//! cancellation replacement to `pane`, suppresses obsolete OIDN, and
		//! parks.  Shared by pointer, property, and timeline admission so their
		//! preemption semantics cannot drift.
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
		//! Mark panes bound to one manager-registered camera for desired-state
		//! reconcile.  REQUIRES mMutex held; includes hidden panes so a later
		//! reveal cannot resurrect a stale realized camera.
		void         InvalidatePanesBoundToSceneCameraLocked_( const String& name );
		//! Undo/redo may not expose the affected edit kind here; conservatively
		//! reconcile every SceneCameraNamed pane.  REQUIRES mMutex held.
		void         InvalidateAllSceneCameraNamedPanesLocked_();
		//! review-r2 P1: named-view propagation -- marks every pane bound
		//! to `name` for reconcile.  Takes mMutex; caller must NOT hold
		//! mNamedViewsMutex (never nested, either order).
		void         InvalidatePanesBoundToNamedView_( const String& name );

		//! user-review P1#2: the camera picking + gizmo projection must use
		//! -- the pane the user is INTERACTING with (its free-fly / named-
		//! view override when active), NOT always the scene camera.  During
		//! a gesture the register (mViewportOverrideCamera) reflects the
		//! gesture pane (OnPanePointerDown force-switches to it), so this is
		//! the correct camera for the click that is happening.  Falls back
		//! to the scene camera when no override is active (pane 0 / scene-
		//! camera panes).  RefreshNavGizmo already used this exact idiom
		//! inline; this centralizes it so pick + every gizmo site agree.
		const ICamera* EffectiveViewportCamera_( const IScene* scene ) const;

		//! Camera-dimension fallback for call sites that already hold mMutex.
		//! The public getter try-locks around this body when its atomic cache
		//! is not primed, keeping UI polling non-blocking without making
		//! locked gizmo paths self-contend.
		bool GetCameraDimensionsLocked_( unsigned int& w, unsigned int& h ) const;

		//! Jump-to-definition shared body (see PropertyJumpTarget) --
		//! resolves one row's Reference value against the live managers.
		bool ResolveRowJumpTarget_( const CameraProperty& row, Category& outCat, String& outName ) const;

		//! Document-first ADR phase 1: the mutation bodies.  The PUBLIC twins
		//! call these then drain the deferred dirty notification OUTSIDE any
		//! lock (SceneEditor::DrainDirtyNotification) -- so listener work never
		//! runs under mMutex and a re-entrant listener cannot deadlock.
		bool SetPropertyInner_( Category targetCategory, const String& targetName,
		                       const String& name, const String& valueStr );
		//! Drain-free body of RestoreEditorState -- for callers that HOLD mMutex
		//! (RollbackTransaction), whose own post-unlock drain delivers the
		//! notification.  The public RestoreEditorState wraps this + drains.
		EditorStateSnapshot CaptureEditorStateLocked_() const;
		void RestoreEditorStateLocked_( const EditorStateSnapshot& s, bool restoreDirty );
		bool SetSelectionInner_( Category cat, const String& entityName );
		void UndoInner_();
		void RedoInner_();
		//! doc 88 S4b: `occ` is WHICH occurrence of `param` to write (0 = first),
		//! and `occAddressed` says the caller reached this param through an
		//! occurrence-addressed row (`stop[2]`) rather than by naming a param the
		//! chunk spells once.  The pair travels together all the way into the
		//! history record (SceneEdit::cstParamOcc / cstParamOccAddressed), where
		//! `occAddressed` arms the undo/redo drift guard.  Both default to the
		//! pre-S4b meaning, so the agent surface (propose_patch) and every other
		//! caller stay occurrence-0 with no behaviour change.  The caller is
		//! responsible for bounding `occ` against the chunk's actual occurrence
		//! count; an out-of-range `occ` here just makes the Document edit a no-op
		//! and reports a plain rejection.
		AgentCommitResult ApplyAgentParamEditInner_(
			const String& entityName, const String& entityKind, const String& param,
			const String& value, const RISE::Cst::CstHeadVersion* baseVersionOrNull,
			int occ = 0, bool occAddressed = false );

		//! External-review round 3 (2026-07-22): the UNLOCKED bodies of the
		//! category-enumeration getters.  REQUIRE a stable manager set: either
		//! mMutex held (SourceRefAtByteOffset's addressability probe, the
		//! InstantiateEntityTemplate name-pick scope) or a single-threaded
		//! context.  The PUBLIC getters wrap these with the mRenderOwnsScene
		//! guard + a blocking mMutex hold (safe: GUI edits run on the UI thread
		//! itself, so a UI-thread poll only ever contends brief background
		//! holds -- see the SetDirtyChangedListener threading contract).
		unsigned int CategoryEntityCountLocked_( Category cat ) const;
		String       CategoryEntityNameLocked_( Category cat, unsigned int idx ) const;
		String       CategoryActiveNameLocked_( Category cat ) const;

		//! ALL of `cat`'s entity names in ONE pass -- the O(N) bulk twin of
		//! the O(N) - per - call `CategoryEntityNameLocked_`.  Same list, same
		//! order, by construction: the manager-backed categories share the
		//! single EnumerateItemNames call here, and the handful that are
		//! genuinely index-addressed (Rasterizer, Film, Animation,
		//! SceneVariant) fall through to the count+index loop, which is O(N)
		//! for them anyway.
		//!
		//! Exists because the per-index getter RE-ENUMERATES THE WHOLE
		//! MANAGER on every call, so building a list of N names through it is
		//! O(N^2).  RefreshEnumSnapshot_ has always paid that; the tree API
		//! is meant to REPLACE the flat one and must not inherit its worst
		//! property, so both go through this.  REQUIRES mMutex held.
		void CategoryEntityNamesLocked_( Category cat, std::vector<String>& out ) const;

		//! Registration serial of `name` in `cat`'s backing store, or 0 when
		//! that store is not an IManager reachable from here (Medium,
		//! Rasterizer, Film, Animation, SceneVariant) -- see
		//! TreeNodeSeed::serial for what a 0 costs.  mMutex must be held.
		//!
		//! ALSO 0 FOR Category::Painter, which is not a lookup failure but a
		//! statement that the question is ill-posed: Painter is the union of
		//! two managers with independent counters, a name may live in both,
		//! and a row denotes ONE of them by position.  BuildCategoryTreeLocked_
		//! builds Painter seeds from `CollectPainterUnionEntries` instead and
		//! never calls this for that category.
		unsigned long long CategoryEntitySerialLocked_( Category cat, const String& name ) const;

		//! Round-4 structural fix (the "UI reads live managers" P1 class): the
		//! PUBLIC enumeration getters serve a per-category SNAPSHOT with a
		//! stale-fallback refresh, never a blocking mMutex hold.  Refresh
		//! policy: try_lock mMutex -- on success rebuild this category's
		//! snapshot from the *Locked_ bodies; on contention (or render-owns-
		//! scene) serve the PRIOR snapshot unchanged.  Properties: never
		//! blocks (no UI freeze), never deadlocks (a dirty-listener that
		//! re-enters on the mutating thread fails the try_lock and gets the
		//! stale list), never races a manager swap (live reads only under the
		//! lock), and never flickers empty (stale beats empty).  mEnumSnapshotMutex
		//! guards ONLY the snapshot arrays; it is always leaf-level -- nothing
		//! acquires mMutex (or anything else) while holding it.
		void RefreshEnumSnapshot_( Category cat ) const;

		//! 87 step 4a: the same refresh policy, for the node tree.  A
		//! SEPARATE pass from RefreshEnumSnapshot_ rather than a second half
		//! of it, deliberately: the flat getters refresh on EVERY indexed
		//! call, so folding the tree build into them would rebuild the whole
		//! tree once per `CategoryEntityName` -- N tree builds to enumerate N
		//! names.  Identical discipline otherwise: bail on render-owns-scene,
		//! bail on a contended mMutex (serving the prior tree in both cases),
		//! build into a LOCAL under mMutex, publish under the leaf
		//! mUiSnapshotMutex -- but COMPARE FIRST and publish only when the
		//! tree actually changed, stamping a fresh generation when it does.
		//! An unconditional publish would bump the generation on every count
		//! call and invalidate every outstanding handle, which would make the
		//! handle API unusable for the shells it exists for.
		//!
		//! Note on the `std::try_to_lock` above: it doubles as a re-entrancy
		//! guard (a dirty-changed listener that calls back in on the mutating
		//! thread fails it and gets the stale tree).  By the standard,
		//! try_lock on a non-recursive mutex the calling thread already owns
		//! is UNDEFINED; in practice pthreads and Win32 both return false,
		//! which is the behaviour relied on.  This is PRE-EXISTING -- the
		//! whole refresh discipline is copied from RefreshEnumSnapshot_,
		//! which has always done it -- and is recorded here rather than
		//! changed, because changing it is a change to the deadlock-avoidance
		//! contract of every snapshot getter, not a local edit.
		void RefreshTreeSnapshot_( Category cat ) const;

		//! Build `cat`'s tree from the live managers.  REQUIRES mMutex held.
		AuthoredTree BuildCategoryTreeLocked_( Category cat ) const;

		//! Category::Object's half of that: turn the manager's FLAT entry set
		//! into the AUTHORED graph's nodes.  REQUIRES mMutex held.
		//!
		//! The flat set is the render list, and under 87 step 3 instancing it
		//! contains SYNTHESIZED entries that have no chunk of their own --
		//! `I.X` for a subtree clone, `I[i,j]` for a repetition, `I[i,j].X`
		//! for both.  Showing them would flood the outliner (an 8x8 grid is
		//! 64+ rows) and offer edits on rows no author can address.  So each
		//! synthesized entry is FOLDED INTO ITS INSTANCING CHUNK, and the
		//! discriminator is `IObjectManager::GetObjectProvenance` -- NEVER the
		//! spelling of the name.  Splitting on `.` or probing for `[` is
		//! explicitly forbidden by that interface's contract (an author may
		//! legitimately write `name my.object`), and the map is the only
		//! sanctioned route.
		void BuildObjectTreeSeedsLocked_( std::vector<TreeNodeSeed>& outSeeds ) const;

		//! doc-88 Phase 3 S11's refresh cadence for the Painter/Material DAG --
		//! the SAME compare-then-publish discipline as RefreshTreeSnapshot_
		//! (a serve-stale on contention/render-owns-scene, republish only on
		//! an actual structural change, generation bumped only then).
		void RefreshPainterMaterialGraphSnapshot_() const;

		//! Gather BOTH halves of the DAG assembler's input under mMutex, from
		//! ONE `SceneReferenceGraph::AllChunks` document scan (doc-88 S11
		//! review round 1 P1-1/P1-2 -- see the .cpp for the full design
		//! note): node seeds are every Painter/Material-category chunk that
		//! scan finds (identity/keyword/category/name are correct BY
		//! CONSTRUCTION, no re-resolution), enriched with order/serial/
		//! defCount read from the live Painter/Material managers as a
		//! SECONDARY lookup, never as the node source; edge seeds (plus
		//! Function-node promotion and dangling ports) come from
		//! `SceneReferenceGraph::EdgesAndDangling` over the SAME retained
		//! Document (`mJob.GetCstDocument()`), given the SAME chunk scan and
		//! called ONCE -- calling `ResolveChunk` per node (the prior design)
		//! or `Edges`/`DanglingReferences` as two separate passes would each
		//! re-pay the O(N log N) document walk (measured 24s under mMutex on
		//! a 7442-chunk scene for the ResolveChunk-per-node version).
		//! REQUIRES mMutex held.
		void BuildPainterMaterialGraphSeedsLocked_(
			std::vector<GraphNodeSeed>& outNodes, std::vector<GraphEdgeSeed>& outEdges ) const;

		//! doc-88 Phase 3 S14: "the current scene's file path", read from
		//! `mJob.GetCstLoadFileIdentity().filePath` -- see the block comment
		//! by `ReadPainterMaterialGraphLaidOut`'s declaration for why the
		//! controller reads THIS field rather than owning a dedicated path
		//! of its own. `outPath` is cleared first and left empty (not
		//! untouched) on every return, including a `false` return -- a
		//! caller that ignores the bool and reads `outPath` anyway sees ""
		//! (the "no sidecar" / "unsaved scene" degradation), never a stale
		//! leftover from a previous call.
		//!
		//! `blocking=false` (the default -- used by the POLLED read path,
		//! `ReadPainterMaterialGraphLaidOut`) follows the same non-blocking
		//! "serve stale on contention" convention `RefreshTreeSnapshot_`/
		//! `GetAnimationOptions` already use: `try_to_lock`, returns false
		//! without touching `outPath` on contention OR while a render owns
		//! the scene -- a polling caller degrades to "no sidecar this call"
		//! (full auto-layout) rather than blocking a UI thread, and
		//! self-heals on the next poll.
		//!
		//! `blocking=true` (used by the explicit, user-initiated
		//! `WriteGraphLayoutPositions`) waits for `mMutex` like
		//! `RequestSave`'s own snapshot step does -- an explicit write must
		//! not silently no-op just because a poll happened to be
		//! mid-refresh. It still refuses (returns false) while a render owns
		//! the scene, same as `RequestSave` itself refuses a save in that
		//! state -- `WriteGraphLayoutPositions` surfaces that as an error
		//! rather than a silent no-op, unlike the polled read path, because
		//! it is an explicit user action.
		bool GetCurrentScenePath_( std::string& outPath, bool blocking = false ) const;

		//! Does `cat`'s CURRENTLY PUBLISHED tree contain a row named `name`?
		//! Takes only the leaf snapshot lock and does NOT refresh -- it asks
		//! about the tree a shell has drawn, which is the question
		//! `ResolveTreeRowName` needs answered.
		bool TreeContainsName_( Category cat, const std::string& name ) const;

		//! Document-first ADR phase 2: THE unified UI read surface.  Every
		//! public read accessor (properties, enumeration, jump rows) serves
		//! this one struct under the one leaf mUiSnapshotMutex; the writers
		//! (RefreshProperties, RefreshEnumSnapshot_) build into locals under
		//! the mMutex try_lock and publish with a brief leaf hold.  A new
		//! UI-visible datum belongs IN THIS STRUCT, refreshed on one of the
		//! existing cadences -- never an ad-hoc live read (ADR rule 4).
		struct EditorUiSnapshot
		{
			std::vector<CameraProperty> properties;                          // primary (panel-mode routed)
			std::vector<CameraProperty> propertiesByCategory[kNumCategories];
			std::vector<String>         entityNames[kNumCategories];
			String                      activeNames[kNumCategories];
			//! 87 step 4a.  ONE AuthoredTree per category -- see AuthoredTree's
			//! own comment for why its vectors may not be hoisted out into
			//! parallel arrays here.
			AuthoredTree                trees[kNumCategories];
			//! doc-88 Phase 3 S11.  ONE graph, not per-category (unlike
			//! `trees`) -- it already spans exactly Category::Painter +
			//! Category::Material (plus any edge-reached Function node), so a
			//! per-category slot would be either empty or a duplicate of this
			//! one for every other category.
			PainterMaterialGraph        painterMaterialGraph;
		};
		mutable std::mutex        mUiSnapshotMutex;   // leaf: never held while acquiring any other lock
		mutable EditorUiSnapshot  mUi;

		// NO PER-CONTROLLER GENERATION COUNTER.  Round-4 P3-1 moved it to a
		// process-global atomic (`NextTreeGeneration()` in the .cpp): a
		// per-controller counter seeded at 1 meant a handle minted on
		// controller A decoded cleanly against controller B and named B's node
		// at the same index.  Global also keeps the cross-CATEGORY property the
		// old member had (one counter serves every category, so no two
		// published trees share a generation) without depending on the two
		// being in the same object.

		//! External-review P1 (2026-07-22): general reference-target guard for
		//! GUI property edits.  Returns true (== "reject this edit") ONLY when
		//! @a value would persist a DANGLING reference: @a paramName is a
		//! Reference-kind param on @a entityName's chunk (@a cat's role suffix),
		//! and @a value names a RUNTIME-only registered entity of an allowed
		//! referenceCategory that has NO CST chunk -- so it renders now but fails
		//! on save/reload.  Inline literals (e.g. `ior 1.7`), CST-backed names,
		//! and the `none` sentinel all return false (allowed).  Conservative by
		//! construction: only the precisely-diagnosable runtime-only case is
		//! rejected, so it never blocks a legitimate edit.
		bool WouldPersistDanglingReference_( Category cat, const String& entityName,
			const String& paramName, const String& value );

		//! user-review P1-5 / P2-1: the effective camera of a SPECIFIC pane
		//! (its realized free-fly / named-view override, else the scene camera),
		//! reading the SLOT for a non-current pane.  REQUIRES mMutex held: the
		//! render thread's SwitchToPaneLocked_ (which swaps + ->release()s these)
		//! also runs under mMutex, so the returned pointer stays valid for the
		//! caller's use while the lock is held.  Used by the gizmo + nav-ball
		//! refreshers to project through the PRIMARY pane (what the overlay is
		//! drawn on), not whatever pane the scheduler last rotated in.
		const ICamera* PaneEffectiveCameraLocked_( unsigned int pane, const IScene* scene ) const;

		//! user-review P2-A: the gizmo-handle computation, assuming mMutex AND
		//! mCameraFrameMutex are ALREADY held.  The public RefreshGizmoHandles()
		//! wraps this in TRY-locks (best-effort, never blocks the paint thread);
		//! the gesture path (OnPointerDown) instead PARKS the render and holds
		//! both locks so the grab's hit-test is reliable even mid-refinement --
		//! the try-lock path would otherwise silently drop the handles (and the
		//! click) whenever a preview pass held mCameraFrameMutex.
		void RefreshGizmoHandlesLocked_();

		// Properties-panel snapshot (rebuilt on RefreshProperties).
		// `mProperties` is the PRIMARY-selection snapshot (kept for
		// back-compat with the single-tuple PropertyXxx accessors).
		// `mPropertiesByCategory[i]` is the per-section snapshot —
		// populated for every category with a non-empty selection
		// in `RefreshProperties`.  Phase 4b's multi-section panel
		// reads the per-category arrays so each expanded section
		// renders its own rows independently.
		// (mProperties / mPropertiesByCategory moved into EditorUiSnapshot --
		// Document-first ADR phase 2.)

		// Transactional-rollback state.  Appended at the end of the member
		// list so the addition is layout-additive (no field before it
		// shifts).  Re-based on inverse-edit rollback (NOT snapshot): no
		// SceneSnapshot is held.  `mTxnOpen` is true exactly when a
		// transaction is open.  `mTxnBaseline.historyMarker` records EditHistory::NextSeq() at
		// BeginTransaction so RollbackTransaction undoes while the top edit's
		// seq >= that marker (trim-immune; survives the 1024 history cap).
		// Begin/Rollback/End are UI entry points, but any-thread agent
		// commits and render submissions consult the flag. Atomic loads make
		// status checks race-free; transitions and commit rechecks are
		// serialized under mMutex.
		std::atomic<bool>                    mTxnOpen;
		EditorStateSnapshot                  mTxnBaseline;        // H1: one owned baseline (history marker + dirty + selection)

		// Disable copy / move
		SceneEditController( const SceneEditController& );
		SceneEditController& operator=( const SceneEditController& );
	};
}

#endif
