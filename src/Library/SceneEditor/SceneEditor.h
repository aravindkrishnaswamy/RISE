//////////////////////////////////////////////////////////////////////
//
//  SceneEditor.h - The only sanctioned mutator of a loaded scene.
//    Owns three responsibilities:
//      1. Enforces the post-mutation invariant chain (FinalizeTrans-
//         formations -> ResetRuntimeData -> InvalidateSpatialStruct-
//         ure) on every transform edit so platform code can never
//         forget a step.
//      2. Records every mutation as a value-typed SceneEdit into an
//         EditHistory for undo/redo.
//      3. Tags a "dirty scope" enum that the render orchestrator
//         (SceneEditController) consults to decide between cheap
//         re-render and expensive photon regen.
//
//  This is the SceneEditor + SceneEdit value-record design from
//  docs/INTERACTIVE_EDITOR_PLAN.md §4.3.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SCENEEDITOR_
#define RISE_SCENEEDITOR_

#include "SceneEdit.h"
#include "EditHistory.h"
#include "DirtyTracker.h"
#include "../Interfaces/IScenePriv.h"
#include "../Cst/Cst.h"   // Document-first ADR phase 1: CstHeadVersion (derived dirty)
#include <functional>
#include <unordered_set>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <utility>

namespace RISE
{
	namespace Implementation { class CameraCommon; }
	class IObjectPriv;
	class IJob;

	class SceneEditor
	{
	public:
		//! Coarse "what kind of change happened" enum.  The render
		//! orchestrator consults LastDirtyScope() to decide how much
		//! work the next render needs to redo.
		enum DirtyScope
		{
			Dirty_None,
			Dirty_ObjectTransform,   ///< spatial structure invalidated
			Dirty_Camera,            ///< no spatial invalidation
			Dirty_Time,              ///< photons MAY need regen on production
			Dirty_TimeAndPhotons     ///< photons definitely need regen
		};

		SceneEditor( IScenePriv& scene );
		~SceneEditor();

		//! Optional manager hooks installed by `SceneEditController` so
		//! Phase-3 object property edits (`SetObjectMaterial` /
		//! `SetObjectShader`) can resolve names through the right
		//! manager.  Null managers cause those ops to fail at Apply
		//! time — the editor degrades to "transform / camera ops only"
		//! mode used by Phase-1/2 unit tests that pass IScenePriv only.
		void SetMaterialManager( class IMaterialManager* mgr ) { mMaterialManager = mgr; }
		void SetShaderManager( class IShaderManager* mgr ) { mShaderManager = mgr; }

		//! Painter manager hooks for `SceneEdit::SetMaterialProperty`.
		//! Apply uses these to resolve the painter-name string the
		//! panel sends into the actual IPainter*/IScalarPainter*
		//! to bind on the material.  Null managers cause material
		//! property edits to fail at Apply (matching the material /
		//! shader degradation pattern).
		void SetPainterManager( class IPainterManager* mgr ) { mPainterManager = mgr; }
		void SetScalarPainterManager( class IScalarPainterManager* mgr ) { mScalarPainterManager = mgr; }

		//! Optional Job pointer installed by SceneEditController so
		//! Phase-1-unlock object property edits (currently
		//! `SetObjectInteriorMedium`) can resolve medium names through
		//! `IJob::GetMedium` and recover prev-state via
		//! `IJob::EnumerateMediumNames`.  Null Job causes those ops to
		//! fail at Apply time — the editor stays usable for transform /
		//! material / shader / shadow / camera / light ops without it.
		void SetJob( IJob* job )
		{
			const bool initialBinding = ( mJob != job );
			mJob = job;
			if( initialBinding ) {
				// Only initial attachment establishes a clean baseline. A D2
				// re-derive rebinds the SAME Job after committing a new head;
				// treating that as a fresh bind would baseline an unsaved edit.
				CaptureSavedHeadVersion_();
			} else {
				// Same-Job rebind: retain the saved baseline and refresh the
				// derived floor against the new live head.
				RecomputeHeadDiffers_();
				RecomputeHasUnsavedChangesCached_();
			}
		}

		//! Re-point the editor at the Job's CURRENT scene after a whole-scene rebuild (e.g. a scene_variant
		//! re-derive ClearAll's + recreates the Scene + managers); call ALONGSIDE the SetXManager setters,
		//! otherwise mScene + the cached managers dangle into freed storage.
		void RebindScene( IScenePriv& scene ) { mScene = &scene; mScenePhotonsExist = ComputeScenePhotonsExist(); mSceneScale = 0; }   // also refresh the scene-derived caches

		//! Apply an edit.  On success, runs the invariant chain,
		//! pushes the edit (with its prevTransform/prevTime captured
		//! before the mutation) onto the history, and updates
		//! LastDirtyScope().  Returns false if the edit references
		//! an unknown object or is otherwise invalid; the scene is
		//! not modified in that case.
		bool Apply( const SceneEdit& edit );

		//! Pop the most recent edit and apply its inverse.  Returns
		//! false if the undo stack is empty or the edit's target
		//! object has gone away.
		bool Undo();

		//! Re-apply an edit that was previously undone.
		bool Redo();

		//! Open a composite group.  Subsequent Apply() calls between
		//! BeginComposite/EndComposite collapse to one undo entry.
		void BeginComposite( const char* label );
		void EndComposite();

		//! Force the open-composite depth back to zero WITHOUT pushing a
		//! CompositeEnd marker.  Used by the controller's transactional
		//! rollback (feature/gui-snapshot-prototype #2b(b)): a rollback
		//! can fire mid-gesture, after BeginComposite opened a group and
		//! the gesture's history (including the unmatched CompositeBegin)
		//! has just been reverted by SceneEditor::Undo's seq-walk + ClearRedo.  If the
		//! tool's later EndComposite still saw depth>0 it would push an
		//! ORPHAN CompositeEnd against the discarded history, corrupting
		//! the next Undo's composite walk.  Resetting the depth makes
		//! that EndComposite a safe no-op.  No-op when already at zero.
		void ForceCompositeDepthZero() { if( mCompositeDepth > 0 ) mCompositeDepth = 0; }

		//! True while a BeginComposite group is open (mCompositeDepth > 0).
		//! The controller's BeginTransaction refuses to open a transaction
		//! mid-composite: the baseline depth would land INSIDE the group
		//! and a single composite Undo() during rollback would walk PAST
		//! it, consuming the pre-baseline CompositeBegin and corrupting
		//! the surrounding undo history (re-review finding A).
		bool IsCompositeOpen() const { return mCompositeDepth > 0; }

		//! F7: snapshot / restore ALL dirty state so a transactional
		//! rollback can return it to its pre-transaction baseline (Undo
		//! re-marks dirty; created entities are never un-marked).
		//! Captures the DirtyTracker channels (four sets + the CST-head
		//! boolean) PLUS the additional set
		//! mScaleFromAnchorSet (BUG-2: it lives outside DirtyTracker, so a
		//! plain State copy missed it -> a rolled-back scale-gizmo gesture
		//! still reported unsaved changes).  Restore also fires the dirty-
		//! changed listener (BUG-1: a silent dirty->clean left the Save button
		//! stale).  Note the CST-head boolean is NOT restored by plain
		//! value: DirtyTracker::RestoreState OR-merges it -- but that
		//! belt-and-braces covers ONLY the UNCATEGORIZED/empty-kind
		//! agent-mark channel (KNOWN kinds mark the per-entity sets,
		//! which restore by plain copy); the real mid-transaction guard
		//! is ApplyAgentParamEdit's mTxnOpen refusal, which rejects
		//! agent commits while a transaction is open (see the F7 doc
		//! in DirtyTracker.h).
		struct DirtySnapshot {
			DirtyTracker::State              tracker;
			std::unordered_set<std::string> scaleFromAnchor;
		};
		DirtySnapshot CaptureDirtyState() const {
			DirtySnapshot s; s.tracker = mDirtyTracker.CaptureState(); s.scaleFromAnchor = mScaleFromAnchorSet; return s;
		}
		void RestoreDirtyState( const DirtySnapshot& s ) {
			mDirtyTracker.RestoreState( s.tracker );
			mScaleFromAnchorSet = s.scaleFromAnchor;
			FireDirtyChangedIfTransitioned();
		}

		DirtyScope LastDirtyScope() const { return mLastScope; }

		//! True if the scene had at least one populated photon map
		//! at construction.  The controller uses this to decide
		//! whether SetSceneTime needs to run before a production
		//! render after a scrub.
		bool ScenePhotonsExist() const { return mScenePhotonsExist; }

		//! Most recently applied scene time via SetSceneTime ops.
		//! Defaults to 0 (the implicit scene time before any scrub).
		//! The controller reads this so it can run a full
		//! `IScene::SetSceneTime(t)` (with photon regen) before a
		//! production render — the preview path uses
		//! SetSceneTimeForPreview which deliberately skips photon
		//! regen and would leave caustics from the pre-scrub time.
		Scalar LastSceneTime() const { return mLastSetTime; }

		//! Characteristic length of the scene — the diagonal of the
		//! axis-aligned union of all object bounding boxes (or a
		//! sensible fallback if the scene is empty / unbounded).
		//! Used by the camera-control math to scale pan / zoom rates
		//! by overall scene size, so a small scene gets small absolute
		//! changes per pixel and a large scene gets large ones,
		//! independent of where the camera is positioned.  Lazily
		//! computed on first use; cached thereafter.
		Scalar SceneScale() const;

		//! P3a slice 3 (N-up per-pane fly): the CANONICAL camera-op math --
		//! orbit / pan / zoom / roll pixel-deltas onto a CameraCommon --
		//! exposed so SceneEditController can apply the SAME transform to a
		//! pane's realized standalone override camera that Apply() uses for
		//! the scene camera.  Exposing the one canonical implementation
		//! beats re-deriving the math (drift class); the definition stays
		//! in SceneEditor.cpp.
		static void ApplyCameraOpToCamera(
			Implementation::CameraCommon& cam,
			const SceneEdit& e,
			Scalar sceneScale );

		//! The undo/redo record.  Historically an input to round-trip
		//! save (Phase 6 / Phase A byte-splice); since the Slice-6d
		//! deletion save no longer reads it — consumers are undo/redo
		//! and the controller's transactional-rollback machinery.
		const EditHistory& History() const { return mHistory; }
		EditHistory&       History()       { return mHistory; }

		//! Dirty-since-load tracker (origin: Phase 6.3,
		//! docs/ROUND_TRIP_SAVE_PLAN.md §7.1).  Populated by Apply /
		//! Undo / Redo (and the agent path via MarkCstHeadDirty).
		//! Historically the Phase 6.4 byte-splice save engine read it
		//! to decide which objects to compare against the loaded
		//! snapshot; since the Slice-6d byte-splice deletion the
		//! channels only feed HasUnsavedChanges() / the GUI
		//! Save-button state — the save engine's sole tracker
		//! interaction is Clear() after a successful save.
		const DirtyTracker& Dirty() const { return mDirtyTracker; }
		DirtyTracker&       Dirty()       { return mDirtyTracker; }

		//! Object names that received a `ScaleObjectFromAnchor` op
		//! since the last load.  Historically the Phase 6.4 §9.2
		//! forceMatrixOverride gate consulted this so SFA-touched
		//! objects always serialized as `matrix` overrides (pinned
		//! 2.8); since the Slice-6d byte-splice deletion it feeds
		//! HasUnsavedChanges() and the F7 CaptureDirtyState /
		//! RestoreDirtyState transaction snapshot.  The production
		//! save path (SceneEditController::RequestSave) hands the
		//! engine a local COPY of this set; the member itself is
		//! cleared by the controller's mEditor.ClearDirtyState()
		//! call on a successful save.
		const std::unordered_set<std::string>& ScaleFromAnchorSet() const { return mScaleFromAnchorSet; }
		void ClearDirtyState()
		{
			mDirtyTracker.Clear();
			mScaleFromAnchorSet.clear();
			// Document-first ADR phase 1: a successful save makes the CURRENT
			// head the clean baseline for the derived-dirty comparison.
			CaptureSavedHeadVersion_();
			FireDirtyChangedIfTransitioned();
		}

		//! F5 slice 1b: mark the editor dirty for an AGENT commit that
		//! mutated the retained CST head DIRECTLY (via
		//! Job::ApplyCstParamEdit), bypassing SceneEditor::Apply.  The
		//! GUI path marks dirty inside Apply (MarkEditEntityDirty); the
		//! agent path does not, so without this call a live agent edit
		//! leaves HasUnsavedChanges() FALSE — the Save button stays
		//! disabled and a close-without-prompt path can silently LOSE
		//! the edit.
		//!
		//! `entityKind` is the CST chunk kind of the edited entity
		//! (e.g. "standard_object", "material", "*_light", "camera",
		//! "*_medium").  KNOWN kinds route to their per-category dirty
		//! channel (mirroring MarkEditEntityDirty), so the mark matches
		//! the GUI's channel for the common cases.  UNKNOWN kinds (the
		//! agent can edit painters / functions / rasterizer params /
		//! shaders — kinds the tracker's categories don't cover) set
		//! the tracker's first-class CST-head boolean channel
		//! (DirtyTracker::MarkCstHeadDirty) so HasAnyDirty() still
		//! flips true without parking uncategorized names in the
		//! object-transform set.  Either mark is SAFE: SaveEngine::Save
		//! has NO dirty gate — it serializes the whole Document
		//! unconditionally (NoOp on byte-equality) and never
		//! byte-splices per entity; the aggregate dirty bit gates only
		//! the GUI Save-button enable — and both channels are cleared
		//! by DirtyTracker::Clear() on a successful save.
		//!
		//! Fires FireDirtyChangedIfTransitioned() so the Save-button
		//! listener runs on the clean→dirty transition.  The listener
		//! runs on the CALLING thread (a future background-transport
		//! agent must marshal it to the UI dispatch queue — a 1c
		//! concern, not this fix).  An empty `entityName` is tolerated:
		//! it sets the same CST-head boolean channel, whose set is
		//! UNCONDITIONAL (no name to validate), so the mark can NEVER
		//! silently no-op — this is a data-loss guard.
		void MarkCstHeadDirty( const char* entityName, const char* entityKind )
		{
			const std::string name = entityName ? entityName : std::string();
			const std::string kind = entityKind ? entityKind : std::string();

			// Map KNOWN CST chunk kinds to their per-category channel so
			// the agent mark mirrors the GUI's MarkEditEntityDirty for
			// the common cases.  ClassifyCstEntityKind (below) is the
			// SINGLE shared classifier -- see its own doc for why this
			// used to be an independent copy of the same vocabulary.
			EntityCategory category;
			const bool isKnown = ClassifyCstEntityKind( kind, category );

			// An empty name would make MarkEntityDirty a SILENT NO-OP
			// (DirtyTracker ignores empty names), so set the CST-head
			// boolean channel BEFORE the category dispatch regardless of
			// kind -- the boolean set is unconditional, so the mark
			// always flips HasAnyDirty() by construction (data-loss
			// guard).  Empty names are a LIVE case, not just defence:
			// the agent chunk-CRUD path (SceneEditController::
			// ApplyAgentChunkCrud_) legitimately passes an empty name for
			// an UNNAMED inserted chunk (film / rasterizer / camera-class),
			// and any future caller must not re-open the hole either.
			if( name.empty() ) {
				mDirtyTracker.MarkCstHeadDirty();
			} else if( isKnown ) {
				mDirtyTracker.MarkEntityDirty( category, name );
			} else {
				// UNKNOWN / uncategorized kind (painter, function,
				// rasterizer, shader): set the first-class CST-head
				// boolean channel -- a coarse "the retained Document
				// changed" mark that keeps uncategorized names OUT of
				// the object-transform set (the pre-A1 semantic
				// overload parked them in mNames).
				mDirtyTracker.MarkCstHeadDirty();
			}
			FireDirtyChangedIfTransitioned();
		}

		//! Shared-undo follow-up (agent emissive-material light-gen bump):
		//! resolve whether an agent-originated CST param edit touched a
		//! MATERIAL, and if so, bump the light-topology generation iff that
		//! material is emissive (BumpSceneLightGenerationIfMaterialEmits).
		//! This closes the same gap MarkEditEntityDirty's SetMaterialProperty
		//! arm covers for the GUI property panel (SceneEditor.cpp ~910-924),
		//! but for the agent path -- which never routes through that switch
		//! (its forward commit lands via SceneEditController::
		//! ApplyAgentParamEdit, its Undo/Redo arms via ApplyRevertMutation /
		//! ApplyForwardMutation's dedicated SetAgentCstParam branches).
		//! Without this bump, editing an emissive material's exitance/scale
		//! through the agent (or undoing/redoing that edit) never advances
		//! Scene::mLightTopologyGeneration, so a reused RayCaster's
		//! LightSampler alias table keeps the STALE selection weight from
		//! before the edit -- light SELECTION stays biased even though
		//! per-sample Le is still read live (the estimator itself is
		//! unbiased; only convergence rate suffers).  Bites ONLY on a
		//! code-1 incremental apply -- a D2 full re-derive (codes 2/3)
		//! rebuilds the Scene from scratch and gets a fresh alias table via
		//! RebindEditorToJob, so bumping there too is harmless, just moot.
		//!
		//! `entityKind` uses the SAME kind-string vocabulary as
		//! MarkCstHeadDirty ("material" / "*_material" for materials; other
		//! known CST kinds -- "standard_object", "*_light", "*_camera",
		//! camera, "*_medium" -- for everything else the agent can target).
		//! Resolution rule, by design NARROWER than MarkCstHeadDirty's full
		//! dispatch (this call only needs "is this a material"):
		//!   - kind == "material" or kind ends with "_material": look the
		//!     entity up by name in mMaterialManager and bump iff it's
		//!     emissive (GetEmitter() != null).  A rename/retarget mid-edit
		//!     that fails to resolve is treated as non-emissive (no bump) --
		//!     symmetric with BumpSceneLightGenerationIfMaterialEmits's own
		//!     null-tolerant contract.
		//!   - kind is EMPTY or any other UNRECOGNIZED string (the agent can
		//!     target painters, functions, rasterizer params, shaders --
		//!     kinds outside the GUI's Category taxonomy, and also passes
		//!     empty kind on some call shapes): bump CONSERVATIVELY.  A
		//!     spurious bump costs one alias-table rebuild on the next
		//!     render (cheap); a MISSED bump on an actually-emissive target
		//!     is a rendering-correctness bug (stale light selection) --
		//!     the tradeoff is deliberately asymmetric in favour of
		//!     correctness.
		//!   - kind is a KNOWN NON-material CST kind ("standard_object",
		//!     "*_light", "*_camera"/camera, "*_medium"): no bump here.  A
		//!     spatial edit on an emissive OBJECT is covered by
		//!     BumpSceneLightGenerationIfEmitterSetChanged /
		//!     BumpSceneLightGenerationIfMaterialEmits at the transform-op
		//!     call sites (MarkEditEntityDirty's own dispatch); a light
		//!     PROPERTY edit is covered by the SetLightProperty Undo arm's
		//!     existing BumpSceneLightGeneration() call.  Those are
		//!     DIFFERENT SceneEdit ops (SetObjectGeometry/SetLightProperty,
		//!     not SetAgentCstParam) with their own bump call sites already
		//!     in place -- this helper only needs to cover the
		//!     SetAgentCstParam / ApplyAgentParamEdit surface.
		//! Defined out-of-line in SceneEditor.cpp: the body calls
		//! mMaterialManager->GetItem(), which needs IMaterialManager's full
		//! definition -- this header only forward-declares that type (see
		//! `class IMaterialManager* mMaterialManager` below), matching how
		//! BumpSceneLightGenerationIfMaterialEmits is declared here / defined
		//! in the .cpp.
		void BumpSceneLightGenerationForAgentParamEdit(
			const char* entityName, const char* entityKind );

		//! Shared-undo U1: push an EditHistory record for an agent PARAM commit
		//! that has ALREADY been applied to the retained Document (via
		//! Job::ApplyCstParamEditChecked -- see
		//! SceneEditController::ApplyAgentParamEdit).  Unlike every other
		//! SceneEdit op, the FORWARD mutation for this first apply happens
		//! BEFORE this call, not inside it -- this method only records the
		//! (entityName, entityKind, param, newValue, prevValue-or-absent)
		//! tuple so a later Undo/Redo can revert/redo it through the ordinary
		//! ApplyRevertMutation/ApplyForwardMutation machinery (RouteCstParamEdit_ /
		//! RouteCstParamRemove_ — the SAME routing the GUI property panel uses).
		//!
		//! Does NOT call MarkEditEntityDirty or FireDirtyChangedIfTransitioned
		//! or touch mLastScope -- the caller (ApplyAgentParamEdit) already calls
		//! MarkCstHeadDirty for the dirty side effect (which fires the listener
		//! itself), and mLastScope has no agent-path reader.  Does NOT run
		//! CaptureForApply (there is nothing to capture — the caller already
		//! captured `prevValue`/`prevValueWasAbsent` from the Document under the
		//! same lock as the apply) and does NOT call ApplyForwardMutation (the
		//! mutation already happened).
		//!
		//! `entityKind` may be empty (agents can target CST kinds outside the
		//! GUI's Category taxonomy).  Clears the redo stack (ordinary new-edit
		//! semantics — Push does this).
		void PushAgentCstParamEdit(
			const String& entityName, const String& entityKind, const String& param,
			const String& newValue, const String& prevValue, bool prevValueWasAbsent );

		//! Shared-undo U2: push an EditHistory record for an agent chunk-CRUD commit (insert OR remove) that
		//! has ALREADY been applied to the retained Document (via Job::ApplyCstInsertChunk /
		//! Job::ApplyCstRemoveChunk -- see SceneEditController::ApplyAgentChunkCrud_).  Same shape as
		//! PushAgentCstParamEdit: the forward mutation already happened, so this only records what a later
		//! Undo/Redo needs to revert/redo it.
		//!
		//! `isInsert` selects AgentInsertChunk / AgentRemoveChunk.  `chunkName`/`chunkKind` are the chunk's
		//! resolved name/keyword (echoed by the Job call).  `chunkBytes` is: for an insert, the EXACT chunk
		//! text the agent supplied (Redo replays it through Job::ApplyCstInsertChunk again); for a remove, the
		//! EXACT verbatim bytes the CALLER captured from the Document immediately BEFORE the erase (the
		//! concatenation of every top-level item Cst::DocEraseChunkTidy actually dropped) -- Undo splices this
		//! back at `docIndex` via Job::ApplyCstRestoreChunkAt.  `docIndex` is meaningful for BOTH ops: for an
		//! insert, the top-level index ApplyCstInsertChunk's [leadSep][chunk][trailSep] triple landed at
		//! (captured by the CALLER immediately AFTER the insert) -- Undo removes exactly those 3 items at that
		//! index via Job::ApplyCstRemoveItemsAt; for a remove, the index the chunk occupied at capture time
		//! (BEFORE the erase) -- Undo restores at that index via Job::ApplyCstRestoreChunkAt.  `wasRasterizer`
		//! is meaningful ONLY for a remove (ignored for an insert): whether the removed chunk was itself a
		//! `*_rasterizer` chunk (see AgentRemoveChunk's doc).
		//!
		//! Does NOT call MarkEditEntityDirty / FireDirtyChangedIfTransitioned or touch mLastScope -- the caller
		//! already calls MarkCstHeadDirty for the dirty side effect, matching PushAgentCstParamEdit.  Clears the
		//! redo stack (ordinary new-edit semantics -- Push does this).
		void PushAgentChunkCrudEdit(
			bool isInsert, const String& chunkName, const String& chunkKind, const String& chunkBytes,
			int docIndex, bool wasRasterizer );

		//! True when anything MAY need saving since the last load /
		//! save.  Conservative: it can be true when a Save would NoOp
		//! (e.g. edit→undo re-marks dirty; Save then NoOps on
		//! byte-equality of the serialized Document).  Drives the
		//! GUI's "Save" button enable state on both platform shells.
		//! Cheap O(1).
		//! Mutation sites publish every TRANSIENT dirty channel via
		//! DirtyTracker::HasAnyDirty() — object transforms (Phase 6),
		//! property-shaped edits on objects / cameras / lights /
		//! materials / media (Phase B), created-pending entities
		//! (Phase C), and the CST-head boolean (Model-B F5) — plus the
		//! scale-from-anchor set.
		//! Document-first ADR phase 1: the REVISION-DIFF term is the derived
		//! floor -- a Document commit after the last save ALWAYS reads dirty,
		//! regardless of tracker bookkeeping (kills the false-clean class
		//! permanently).  The tracker + scale-anchor terms persist as safe
		//! (false-dirty-only) supplements for live-only pending state and
		//! legacy scenes.  Known asymmetry: undo-back-to-saved still reads
		//! dirty (revision moved) -- a safe false-positive (ADR SS3.3).
		bool HasUnsavedChanges() const
		{
			return mHasUnsavedChangesCached.load( std::memory_order_acquire );
		}

		//! Notification channel for the platform GUI to update its
		//! Save-button enable state.  COALESCED transition semantics
		//! (document-first phase 1 + round-5): the listener reports the
		//! CURRENT `HasUnsavedChanges()` whenever it DIFFERS from the last
		//! report, delivered from the post-unlock drain with NO lock held.
		//! A clean->dirty->clean burst between drains nets to NO callback --
		//! the final state was already correct; consumers must key on the
		//! reported VALUE, never count calls.  Listener runs on whichever
		//! thread drained; bridges should marshal to their UI queue.
		//!
		//! Set once before the editor starts receiving edits;
		//! re-setting clears the previous listener.  Pass `nullptr`
		//! to detach.
		using DirtyChangedFn = std::function<void(bool hasUnsavedChanges)>;
		void SetDirtyChangedListener( DirtyChangedFn fn )
		{
			const std::shared_ptr<DirtyNotificationState> state = mDirtyNotificationState;
			std::unique_lock<std::mutex> lk( state->mutex );
			state->listener = std::move( fn );
			// Replacing/detaching is a quiescence barrier for the PREVIOUS
			// callback.  Platform listeners capture raw bridge userData, so
			// returning while another thread still invokes its copied
			// std::function would let bridge teardown free that pointer first.
			// A callback may itself destroy the controller/editor; that same
			// callback thread must not wait for its own return.
			if( state->callbacksInFlight != 0
			 && state->callbackThread != std::this_thread::get_id() ) {
				state->callbackCV.wait( lk, [&] {
					return state->callbacksInFlight == 0;
				} );
			}
			// Don't fire on attach — the bridge already knows the
			// initial state (it can call HasUnsavedChanges()).
		}

	private:
		//! Advance the live Scene's light/structure generation so a render
		//! reusing a cached RayCaster rebuilds its LightSampler /
		//! LuminaryManager / environment sampler instead of taking the
		//! same-Scene-pointer fast path (feature/gui-snapshot-prototype
		//! #2b(a)).  Call after any in-place LIGHT mutation (energy / colour
		//! / direction / cone).  No-op if mScene isn't a concrete
		//! RISE::Implementation::Scene (out-of-tree IScenePriv).  Keeps the
		//! Scene generation in lockstep with editor light edits — the
		//! engine-side counterpart of Scene::RestoreFromSnapshot's own bump.
		void BumpSceneLightGeneration();

		//! Bump the live Scene's light-topology generation iff a material-
		//! BINDING change affects the emitter set — i.e. either the old or
		//! the new material is emissive (`IMaterial::GetEmitter() != null`).
		//! A `SetObjectMaterial` edit that binds an object to/from (or
		//! between) emissive materials changes which objects the
		//! LuminaryManager treats as area lights, so a reused RayCaster
		//! must rebuild its LightSampler — otherwise a cached luminary
		//! pointing at a now-non-emissive material would later deref a NULL
		//! emitter (the in-place light-edit path already bumps; this is the
		//! object-material-binding counterpart).  No-op when neither
		//! material is emissive (a plain reflectance-only swap leaves the
		//! emitter set unchanged).  Either pointer may be null (a missing /
		//! unresolved material is treated as non-emissive).
		void BumpSceneLightGenerationIfEmitterSetChanged(
			const class IMaterial* prevMat, const class IMaterial* newMat );

		//! Bump the light-topology generation iff `mat` is emissive
		//! (`GetEmitter() != null`).  For edits that change an existing
		//! luminaire's cached sampler state WITHOUT changing the emitter SET:
		//! a SPATIAL edit on an emissive object (its area / world position
		//! feed the LightSampler alias-table weight + representative point,
		//! baked at Prepare()) and a material-SLOT edit on an emissive
		//! material (its exitance feeds the same weight).  Stale state biases
		//! light SELECTION only -- the estimator stays unbiased (per-sample
		//! area / Le are read live) -- but a reused RayCaster must rebuild to
		//! converge (re-review finding B).  No-op for null / non-emissive mat.
		void BumpSceneLightGenerationIfMaterialEmits( const class IMaterial* mat );

		//! Model-B F2 S3 fix round (P3-a): the SINGLE source of truth for
		//! "which EntityCategory does this CST chunk-kind string belong
		//! to" -- shared by MarkCstHeadDirty (above, in the public section)
		//! and BumpSceneLightGenerationForAgentParamEdit (SceneEditor.cpp),
		//! which used to carry two independently-maintained copies of the
		//! same suffix-matching `endsWith` vocabulary ("standard_object" /
		//! "camera" or "*_camera" / "*_light" / "material" or "*_material" /
		//! "*_medium").  Drift risk: a future new CST kind (or a renamed
		//! suffix) added to one classifier and not the other would silently
		//! desync the agent's dirty-marking from its light-regen bump.  ONE
		//! helper, called from both, makes that structurally impossible.
		//!
		//! Returns true and fills `outCategory` when `kind` matches a KNOWN
		//! CST entity kind; returns false (outCategory untouched) for an
		//! empty or unrecognized kind (painter / function / rasterizer /
		//! shader / any future uncategorized chunk) -- callers each apply
		//! their OWN policy for the unknown case (MarkCstHeadDirty falls
		//! back to the coarse CST-head boolean channel; the light-regen
		//! bump conservatively bumps).
		static bool ClassifyCstEntityKind( const std::string& kind, EntityCategory& outCategory );

		IScenePriv*  mScene;   // borrowed; re-pointable via RebindScene after a whole-scene rebuild
		class IMaterialManager*       mMaterialManager;       // borrowed; nullable
		class IShaderManager*         mShaderManager;         // borrowed; nullable
		class IPainterManager*        mPainterManager;        // borrowed; nullable (Phase 4)
		class IScalarPainterManager*  mScalarPainterManager;  // borrowed; nullable (Phase 4)
		IJob*                         mJob;                   // borrowed; nullable
		EditHistory  mHistory;
		DirtyScope   mLastScope;
		int          mCompositeDepth;
		bool         mScenePhotonsExist;
		// IScene exposes SetSceneTime but no GetSceneTime, so we
		// track the most-recently-applied time locally.  Used to
		// capture prevTime for undo on SetSceneTime ops.
		Scalar       mLastSetTime;
		// Cached scene scale (bbox-union diagonal).  0 = uncomputed;
		// `SceneScale()` lazy-computes on first call.  `mutable`
		// because `SceneScale` is `const`-qualified so camera-op
		// code can call it from forward / inverse paths.
		mutable Scalar mSceneScale;

		// Phase 6.3: dirty-since-load channels.  Historically the
		// byte-splice save engine read them to decide which entities
		// to compare against their loaded baselines; that mechanism
		// went with the Slice-6d deletion.  Today the channels only
		// feed HasUnsavedChanges() / the GUI Save-button state (save
		// is a whole-Document SerializeCst), plus the F7
		// CaptureDirtyState / RestoreDirtyState transactional
		// snapshot.  Cleared by ClearDirtyState() after a
		// successful save.
		DirtyTracker  mDirtyTracker;

		// Phase 6.3 (pinned 2.8 / R2 §7): names that have received any
		// ScaleObjectFromAnchor op since the last load.  Historically
		// the byte-splice save engine's forceMatrixOverride gate (§9.2)
		// consulted this so SFA-touched objects always serialized as
		// matrix-form overrides; that gate went with the Slice-6d
		// deletion.  Today it only feeds HasUnsavedChanges() (plus the
		// F7 dirty-state capture/restore); cleared by ClearDirtyState()
		// after a successful save.
		std::unordered_set<std::string> mScaleFromAnchorSet;

		// P5 Slice 3 expansion (object transform): objects whose live transform changed since the last
		// CommitPendingCstObjectTransforms.  Populated by transform ops on a CST scene; flushed to the `matrix`
		// param by the controller at a parked boundary.  UI-thread-only.
		std::unordered_set<std::string> mPendingCstObjMatrix;

		// P5 Slice 3 expansion (camera drag): the active camera's name if a camera-drag op changed its pose since
		// the last CommitPendingCstCameraPose (empty = none).  Flushed to the camera chunk's pose params by the
		// controller at a parked boundary.  UI-thread-only.  Intentionally a SINGLE name, not a set (unlike
		// mPendingCstObjMatrix): a drag gesture targets the ONE active camera, so a composite is single-camera.
		// If a future path ever drags multiple cameras in one composite, convert this to an unordered_set + a
		// snapshot-before-route pass (mirroring the object commit).
		std::string mPendingCstCameraName;

		// P5 Slice 3 expansion (object transform): the last object whose transform edit was REFUSED on a CST scene
		// for lacking a `matrix` param (csg_object).  Used to log the refusal once per distinct object instead of
		// per gizmo drag-frame.  UI-thread-only.
		std::string mLastNonRoutableTransformObj;

		// Model-B (code-3 re-render fix): "did a CST re-derive during the LAST Apply CHANGE the live scene?", i.e.
		// did any route helper see Job::DeriveEditedCstDocument_ return a mutating code (1 incremental / 2 replaced-
		// clean / 3 replaced-DIAGNOSED)?  This is DECOUPLED from Apply's success bool: a code-3 re-derive REPLACED
		// the Scene+managers (managers rebuilt from the mutated Document, best-effort) but reports FAILURE (the route
		// helpers return false) -- yet the viewport MUST re-render to show the replaced scene, else the user sees
		// STALE pre-edit pixels.  Reset to false ONLY at Apply's top; the route helpers OR-in true on code != 0.  The
		// controller reads it after Apply (CstLiveSceneChangedInLastApply) and kicks the render on `ok || changed`.
		// UI-thread-only, same discipline as mPendingCst*.  (Standalone CommitPendingCst* callers -- OnPointerUp,
		// Undo/Redo -- kick unconditionally and don't consult this; it is meaningful only right after Apply.)
		bool mCstLiveSceneChanged = false;

		//! Deferred-notification state lives independently of SceneEditor's
		//! storage so a C callback may destroy its controller safely. Drain
		//! keeps a shared reference, and SceneEditor::~SceneEditor marks the
		//! owner dead before the callback returns to the drain; the drain then
		//! touches only this surviving state and exits without dereferencing
		//! the destroyed editor.
		struct DirtyNotificationState
		{
			DirtyChangedFn   listener;
			bool             prevHasUnsavedChanges = false;
			std::atomic<bool> currentHasUnsavedChanges{ false };
			std::atomic<bool> pending{ false };
			std::atomic<bool> delivering{ false };
			std::atomic<bool> ownerAlive{ true };
			std::atomic<unsigned int> deferDepth{ 0 };
			std::mutex        mutex;
			std::condition_variable callbackCV;
			unsigned int      callbacksInFlight = 0;
			std::thread::id   callbackThread;
		};
		std::shared_ptr<DirtyNotificationState> mDirtyNotificationState;

		//! State-only drain used by the public drain and the deferral
		//! token. It never dereferences SceneEditor, so a listener may
		//! destroy the controller/editor and the active drain still returns
		//! safely.
		static bool DrainDirtyNotificationState_(
			const std::shared_ptr<DirtyNotificationState>& state );

		//! Round-5 review P1: the derived-dirty floor is served from this CACHED
		//! atomic, recomputed at every mutation's Fire (under the mutator's own
		//! lock -> the Job head read is race-free) and by the controller's
		//! per-frame RefreshDerivedDirty catch-all.  The lock-free C-API dirty
		//! query then never reads the non-atomic head/saved versions directly
		//! (the round-4 wiring did, a torn-read UB).
		std::atomic<bool> mHeadDiffersCached{ false };

		//! Complete lock-free dirty answer. DirtyTracker and
		//! mScaleFromAnchorSet are mutable containers written under the
		//! controller mutex, so public/ABI readers must never inspect them
		//! directly. Mutation sites publish their combined value here.
		std::atomic<bool> mHasUnsavedChangesCached{ false };

		//! Document-first ADR phase 1: the head version at bind / last
		//! successful save -- the clean baseline for derived dirty.
		RISE::Cst::CstHeadVersion mSavedHeadVersion;

		//! Capture the CURRENT head version as the clean baseline (bind +
		//! save success).
		void CaptureSavedHeadVersion_();

		//! Derived-dirty floor: serves mHeadDiffersCached (lock-free).
		bool HeadDiffersFromSaved_() const { return mHeadDiffersCached.load( std::memory_order_acquire ); }

		//! Recompute the cached floor from the live head + saved baseline.
		//! REQUIRES a stable Document (the caller's mutation lock, or a
		//! single-threaded context) -- see mHeadDiffersCached's doc.
		void RecomputeHeadDiffers_();

		//! Publish the complete dirty answer from the stable tracker,
		//! scale-anchor set, and cached revision floor.
		void RecomputeHasUnsavedChangesCached_();

	public:
		//! Defers dirty delivery across a composite public controller call.
		//! Nested mutation helpers may each attempt to drain; while this
		//! token lives those drains only leave the notification pending.
		//! The last token's destructor performs the state-only drain after
		//! all controller work has completed.
		class DirtyNotificationDeferral
		{
		public:
			DirtyNotificationDeferral( DirtyNotificationDeferral&& other ) noexcept
				: mState( std::move( other.mState ) ) {}
			~DirtyNotificationDeferral();

			DirtyNotificationDeferral( const DirtyNotificationDeferral& ) = delete;
			DirtyNotificationDeferral& operator=( const DirtyNotificationDeferral& ) = delete;
			DirtyNotificationDeferral& operator=( DirtyNotificationDeferral&& ) = delete;

		private:
			friend class SceneEditor;
			explicit DirtyNotificationDeferral(
				const std::shared_ptr<DirtyNotificationState>& state )
				: mState( state ) {}
			std::shared_ptr<DirtyNotificationState> mState;
		};

		DirtyNotificationDeferral DeferDirtyNotifications();

		//! Document-first ADR phase 1 (fire -> pending + drain): mutation
		//! entry points (Apply success, Undo, Redo, ClearDirtyState, the
		//! marks) call THIS, which only records "dirty may have changed" --
		//! it never invokes the listener, so it is safe under any lock.  The
		//! CONTROLLER drains after releasing its mutex
		//! (DrainDirtyNotification), so the listener always runs OUTSIDE
		//! controller locks; a missed drain site degrades to a one-frame
		//! delay (RefreshProperties drains as a per-frame catch-all), never
		//! a deadlock or a missed transition.  Kept public for the RAII
		//! helper in SceneEditor.cpp; safe to call from anywhere.
		void FireDirtyChangedIfTransitioned();

		//! Drain the pending dirty notification: if a mutation flagged a
		//! possible change, read the atomically-published complete dirty
		//! value and fire the listener iff it transitioned since the last
		//! fire. MUST be called with no controller locks held. Thread-safe
		//! (leaf mutex serializes concurrent drains); synchronous re-entry is
		//! coalesced into the active deliverer's loop.
		//! Returns false when the listener destroyed this editor/controller.
		//! Callers that have more member work after a drain must return
		//! immediately on false.
		bool DrainDirtyNotification();

		//! Round-5 review P1: recompute the cached derived-dirty floor.  Call
		//! ONLY while the Document is stable (the controller calls it inside
		//! its RefreshProperties mMutex try_lock region as a per-frame
		//! self-heal for any mutation path that missed its Fire).
		void RefreshDerivedDirty()
		{
			RecomputeHeadDiffers_();
			RecomputeHasUnsavedChangesCached_();
			const std::shared_ptr<DirtyNotificationState> state =
				mDirtyNotificationState;
			state->currentHasUnsavedChanges.store(
				mHasUnsavedChangesCached.load( std::memory_order_acquire ),
				std::memory_order_release );
			state->pending.store( true, std::memory_order_release );
		}

		//! P5 Slice 3 expansion (object transform): the CONTROLLER calls these at a render-thread-PARKED boundary
		//! (drag-end / panel transform edit / undo / redo) to commit object transforms accumulated by the
		//! per-frame edits as the authoritative standard_object `matrix` param.  Committing re-derives (a variant
		//! scene ClearAll's), so it MUST run parked -- hence it is the controller's job, not auto-flushed here.
		bool HasPendingCstObjectTransforms() const { return !mPendingCstObjMatrix.empty(); }
		bool CommitPendingCstObjectTransforms();
		bool HasPendingCstCameraPose() const { return !mPendingCstCameraName.empty(); }
		bool CommitPendingCstCameraPose();

		//! Model-B (code-3 re-render fix): did the LAST Apply's CST re-derive CHANGE the live scene (raw code
		//! 1/2/3), decoupled from Apply's success bool?  A code-3 re-derive REPLACED the Scene+managers (so the
		//! viewport must re-render) but Apply returns false (the edit is diagnosed/failed).  The controller kicks
		//! the render on `ApplyResult || CstLiveSceneChangedInLastApply()` so a diagnosed edit still repaints the
		//! replaced scene instead of leaving stale pixels.  Valid only immediately after Apply (reset at Apply top).
		bool CstLiveSceneChangedInLastApply() const { return mCstLiveSceneChanged; }

	private:
		//! Look up an object by name on the live ObjectManager and
		//! return its IObjectPriv*.  Returns null if the object is
		//! not found or does not implement IObjectPriv (which would
		//! indicate a non-mutable Object subclass — in practice
		//! every concrete Object inherits IObjectPriv).
		IObjectPriv* FindObject( const String& name ) const;

		//! Apply a forward transform op to an object.  Caller has
		//! already captured prevTransform.
		bool ApplyObjectOpForward( IObjectPriv& obj, const SceneEdit& edit );   ///< P1: false if a binding op's forward target no longer resolves

		//! Restore an object's transform from a captured matrix.
		void RestoreObjectTransform( IObjectPriv& obj, const SceneEdit& edit );

		//! F4: resolve the camera an edit TARGETS (by recorded name) so
		//! Undo/Redo restore the edited camera, not the active-later one.
		//! Falls back to the active camera for legacy edits with no name.
		class ICamera* ResolveEditedCamera( const SceneEdit& e );
		// P1: serial of the entity whose STATE the op restores (object/camera/material/
		// light), for remove+re-add identity detection.  0 = op tracks no identity.
		unsigned long long ResolveTargetSerial( const SceneEdit& e ) const;

		//! F1: rebind a material slot to a named painter -- shared by the
		//! SetMaterialProperty single-edit AND composite-walk undo/redo arms.
		bool ApplyMaterialSlotByName( const SceneEdit& e, const String& painterName );

		//! P5 Slice 3: re-point this editor's cached scene + managers at the Job's CURRENT ones after a CST
		//! D2 full re-derive (Job::ApplyCstParamEdit result 2 or 3 ClearAll'd the Job, freeing the old ones).
		void RebindToJob_();

		//! P5 Slice 3 expansion: route a property edit through the retained CST (Job::ApplyCstParamEdit) instead
		//! of the direct manager mutation, so the Document stays complete (a later material D2 can't lose it).
		//! Self-rebinds on a D2 full re-derive (result >=2).  Returns false (caller fails the edit) on a reject
		//! (0) or a diagnosed re-derive (3).  Shared by the material/light/... CST branches.
		bool RouteCstParamEdit_( const char* entityName, const char* entityKind, const char* role, const char* value );

		//! Shared-undo U1: inverse of an agent param edit that INSERTED a previously-absent param -- removes it
		//! instead of re-setting a nonexistent prior value.  See SceneEditor.cpp for the full rationale.
		//! P1-2 fix (round 1): `occ` selects which occurrence to remove (0 = first).
		//! P1-3 fix (round 1): the bool return is keyed on MUTATION (r>=1), not cleanliness (r==1||2) -- a
		//! code-3 diagnosed re-derive still mutated + rebound the Document, so it must read as "the revert
		//! happened" for the caller's history-stack bookkeeping.  `outDiagnosed` (may be null) reports the
		//! code-3 case for callers that need to log it. See SceneEditor.cpp for the full rationale.
		bool RouteCstParamRemove_( const char* entityName, const char* entityKind, const char* role, int occ, bool* outDiagnosed = nullptr );

		//! Shared-undo U1: full-derivability-gated twin of RouteCstParamEdit_, used ONLY by SetAgentCstParam's
		//! Undo/Redo -- see SceneEditor.cpp for why the agent op cannot safely share the ungated GUI-property route.
		//! P1-3 fix (round 1): same mutation-keyed return + `outDiagnosed` as RouteCstParamRemove_ above.
		bool RouteCstParamEditChecked_( const char* entityName, const char* entityKind, const char* role, const char* value, bool* outDiagnosed = nullptr );

		//! Shared-undo U2: route a chunk-CRUD Undo/Redo through Job's chunk-CRUD primitives.  `forInsertOp`
		//! selects which op's inverse/redo table applies (AgentInsertChunk vs AgentRemoveChunk); `forward`
		//! selects Redo-shape (re-apply the original verb) vs Undo-shape (apply its inverse).  Same
		//! mutation-keyed return + `outDiagnosed` convention as RouteCstParamRemove_/RouteCstParamEditChecked_
		//! above (r>=1 -- including a diagnosed code-3 -- reads as "the mutation landed" for the history-stack
		//! bookkeeping).  See SceneEditor.cpp for the per-shape dispatch (insert-Undo = remove by name;
		//! insert-Redo = re-insert captured bytes; remove-Undo = ApplyCstRestoreChunkAt at the captured index;
		//! remove-Redo = re-remove by name).
		bool RouteAgentChunkCrud_( const SceneEdit& edit, bool forInsertOp, bool forward, bool* outDiagnosed = nullptr );

		//! P5 Slice 3 expansion (object): route a SetObjectShadowFlags edit to the standard_object
		//! casts_shadows / receives_shadows bool params (bit0 = casts, bit1 = receives).  Two CST re-derives.
		bool RouteObjectShadowFlagsToCst_( const String& objectName, int flags );

		//! P5 Slice 3 expansion (object transform): note an object whose transform changed for a deferred
		//! `matrix`-param commit; route ONE object's net transform (rebinds on a D2).
		void NoteCstObjectTransform_( const String& name );
		void NoteCstCameraDrag_( const String& camName );
		bool ApplyCstObjectMatrix_( const std::string& name, const std::string& matrix16 );
		bool ApplyCstObjectComponents_( const std::string& name, const std::string& position, const std::string& orientation );

		//! H2 (P-WALK): the SINGLE per-op forward + revert dispatchers.  Each
		//! IS the single-edit body; the composite walk-loops call them per
		//! inner edit so the single and composite paths can NEVER drift (the
		//! F1/F4/F5 bug class lived in that drift).  Both set mLastScope per
		//! op; composite callers override it with the aggregate scope after
		//! the loop.  Forward uses propertyValue / the new transform; revert
		//! restores the captured prev* state.  Return false only on a
		//! resolve/validation miss (target gone), matching the prior bodies.
		bool ApplyForwardMutation( const SceneEdit& edit );   // Redo direction
		bool ApplyRevertMutation( const SceneEdit& edit );    // Undo direction

		//! H2 Stage 3 (finishes P-WALK): the capture/validate HALF of a
		//! first Apply -- resolve the target, run every rejection gate, and
		//! record the prev* state into `edit`.  Returns false (reject, no
		//! mutation, no history) on a validation miss.  Apply = this +
		//! ApplyForwardMutation + push, so the forward MUTATION now lives in
		//! exactly one place (ApplyForwardMutation), shared by Apply/Redo.
		bool CaptureForApply( SceneEdit& edit );

		//! H2 (P-WALK finish): fold the composite walk's per-op-category saw-
		//! flags into one scope -- shared by composite Undo AND Redo so the
		//! aggregation lives in one place.  NOTE the deliberate asymmetry vs the
		//! single-edit dispatchers: a SINGLE object *property* op (material /
		//! shader) scopes to Dirty_ObjectTransform (every IsObjectOp does), but
		//! the SAME op inside a composite lands in sawPropertyOp -> here ->
		//! Dirty_Camera.  Both force a re-render, and OpNeedsSpatialRebuild (not
		//! the scope) gates the actual BVH rebuild, so the divergence is
		//! conservative and harmless.
		DirtyScope AggregateCompositeScope( bool sawObjectOp, bool sawCameraOp, bool sawTimeOp, bool sawPropertyOp ) const;

		//! Run the post-mutation invariant chain on a single object
		//! and on the manager.
		void RunObjectInvariantChain( IObjectPriv& obj );

		//! Phase B: route a property-shaped edit (camera / light /
		//! material / medium property, or object material / shader /
		//! shadow / interior-medium binding) into the DirtyTracker's
		//! per-category channel.  Called from Apply / Undo / Redo.
		//! Transform ops are NOT handled here — they mark the object-
		//! transform channel inline.  Over-marking is harmless: the
		//! channels only feed HasUnsavedChanges(), and save is an
		//! unconditional whole-Document SerializeCst that NoOps on
		//! byte-equality (the byte-splice engine's property pass that
		//! diffed marked entities went with Slice 6d), so a
		//! marked-but-unchanged entity at worst lights the Save button.
		void MarkEditEntityDirty( const SceneEdit& edit );

		//! Compute whether the loaded scene has any populated photon map.
		bool ComputeScenePhotonsExist() const;
	};
}

#endif
