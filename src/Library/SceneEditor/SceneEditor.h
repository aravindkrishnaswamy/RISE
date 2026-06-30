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
#include <functional>
#include <unordered_set>

namespace RISE
{
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
		void SetJob( IJob* job ) { mJob = job; }

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

		//! F7: snapshot / restore the DirtyTracker channels so a transactional
		//! rollback can return the dirty state to its pre-transaction baseline
		//! (Undo re-marks dirty; created entities are never un-marked).
		//! F7: snapshot / restore ALL dirty state for transactional rollback.
		//! Captures the four DirtyTracker channels PLUS the fifth set
		//! mScaleFromAnchorSet (BUG-2: it lives outside DirtyTracker, so a
		//! plain State copy missed it -> a rolled-back scale-gizmo gesture
		//! still reported unsaved changes).  Restore also fires the dirty-
		//! changed listener (BUG-1: a silent dirty->clean left the Save button
		//! stale).
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

		//! For round-trip save (Phase 6 / Phase A).
		const EditHistory& History() const { return mHistory; }
		EditHistory&       History()       { return mHistory; }

		//! Phase 6.3 (docs/ROUND_TRIP_SAVE_PLAN.md §7.1): per-object
		//! dirty-since-load set.  Populated by Apply / Undo / Redo
		//! whenever a transform-shaped SceneEdit touches an object;
		//! read by the save engine (Phase 6.4) to decide which
		//! objects to compare against the loaded snapshot.  V1 only
		//! tracks OBJECT transform ops (material / shader / etc. do
		//! not mark dirty — out of V1 scope per §7.6).
		const DirtyTracker& Dirty() const { return mDirtyTracker; }
		DirtyTracker&       Dirty()       { return mDirtyTracker; }

		//! Object names that received a `ScaleObjectFromAnchor` op
		//! since the last load.  Phase 6.4 §9.2 forceMatrixOverride
		//! gate consults this — SFA-touched objects always serialize
		//! as `matrix` overrides (pinned 2.8) even when their
		//! current transform happens to be decomposable.
		const std::unordered_set<std::string>& ScaleFromAnchorSet() const { return mScaleFromAnchorSet; }
		void ClearDirtyState()
		{
			mDirtyTracker.Clear();
			mScaleFromAnchorSet.clear();
			FireDirtyChangedIfTransitioned();
		}

		//! True iff any edit since the last load / save would produce
		//! a non-NoOp SaveEngine pass.  Drives the GUI's "Save" button
		//! enable state on both platform shells.  Cheap O(1).
		//! Consults BOTH dirty channels — object transforms (Phase 6)
		//! AND property-shaped edits on objects / cameras / lights /
		//! materials / media (Phase B).
		bool HasUnsavedChanges() const
		{
			return mDirtyTracker.HasAnyDirty()
			    || !mScaleFromAnchorSet.empty();
		}

		//! Notification channel for the platform GUI to update its
		//! Save-button enable state.  Fires only on TRANSITIONS of
		//! `HasUnsavedChanges()` (clean→dirty or dirty→clean), so
		//! a stream of N edits that all leave the scene dirty
		//! produces ONE callback.  Fired from Apply / Undo / Redo /
		//! ClearDirtyState.  Listener runs on the calling thread
		//! (typically the UI thread); bridges should marshal to
		//! their UI dispatch queue if needed.
		//!
		//! Set once before the editor starts receiving edits;
		//! re-setting clears the previous listener.  Pass `nullptr`
		//! to detach.
		using DirtyChangedFn = std::function<void(bool hasUnsavedChanges)>;
		void SetDirtyChangedListener( DirtyChangedFn fn )
		{
			mDirtyChangedListener = std::move( fn );
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

		// Phase 6.3: tracks which OBJECTS have been mutated by
		// transform-shaped ops since the last load / save.  Read by
		// the save engine to decide which objects to compare.  Cleared
		// by ClearDirtyState() after a successful save.
		DirtyTracker  mDirtyTracker;

		// Phase 6.3 (pinned 2.8 / R2 §7): names that have received any
		// ScaleObjectFromAnchor op since the last load.  Save engine's
		// forceMatrixOverride gate (§9.2) consults this — SFA-touched
		// objects always serialize as matrix-form overrides regardless
		// of whether `TryDecompose(Mfinal).ok` succeeds.
		std::unordered_set<std::string> mScaleFromAnchorSet;

		// P5 Slice 3 expansion (object transform): objects whose live transform changed since the last
		// CommitPendingCstObjectTransforms.  Populated by transform ops on a CST scene; flushed to the `matrix`
		// param by the controller at a parked boundary.  UI-thread-only.
		std::unordered_set<std::string> mPendingCstObjMatrix;

		//! Phase 6.5 UI hook: GUI-installed listener fired on
		//! `HasUnsavedChanges()` TRANSITIONS only.  Empty by default
		//! (no callbacks fire until SetDirtyChangedListener is called).
		DirtyChangedFn mDirtyChangedListener;

		//! Memoized last-fired value of `HasUnsavedChanges()` so the
		//! listener only fires on transitions.  Starts at false
		//! (matches the initial clean state on construction).
		bool mPrevHasUnsavedChanges = false;

	public:
		//! Re-evaluate `HasUnsavedChanges()` and fire the listener iff
		//! the value changed since the last fire.  Called from every
		//! mutation entry point (Apply success, Undo, Redo,
		//! ClearDirtyState).  No-op when no listener is installed.
		//! `public` because a file-scope RAII helper in SceneEditor.cpp
		//! invokes it from Apply / Undo / Redo's scope-exit; no
		//! preconditions — safe to call from anywhere.
		void FireDirtyChangedIfTransitioned();

		//! P5 Slice 3 expansion (object transform): the CONTROLLER calls these at a render-thread-PARKED boundary
		//! (drag-end / panel transform edit / undo / redo) to commit object transforms accumulated by the
		//! per-frame edits as the authoritative standard_object `matrix` param.  Committing re-derives (a variant
		//! scene ClearAll's), so it MUST run parked -- hence it is the controller's job, not auto-flushed here.
		bool HasPendingCstObjectTransforms() const { return !mPendingCstObjMatrix.empty(); }
		bool CommitPendingCstObjectTransforms();

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

		//! P5 Slice 3 expansion (object): route a SetObjectShadowFlags edit to the standard_object
		//! casts_shadows / receives_shadows bool params (bit0 = casts, bit1 = receives).  Two CST re-derives.
		bool RouteObjectShadowFlagsToCst_( const String& objectName, int flags );

		//! P5 Slice 3 expansion (object transform): note an object whose transform changed for a deferred
		//! `matrix`-param commit; route ONE object's net transform (rebinds on a D2).
		void NoteCstObjectTransform_( const String& name );
		bool ApplyCstObjectMatrix_( const std::string& name, const std::string& matrix16 );

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
		//! save engine's property pass diffs current-vs-loaded
		//! introspection, so a marked-but-unchanged entity contributes
		//! nothing (it nets to a NoOp).
		void MarkEditEntityDirty( const SceneEdit& edit );

		//! Compute whether the loaded scene has any populated photon map.
		bool ComputeScenePhotonsExist() const;
	};
}

#endif
