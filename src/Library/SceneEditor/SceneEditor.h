//////////////////////////////////////////////////////////////////////
//
//  SceneEditor.h - The only sanctioned mutator of a loaded scene.
//    Owns three responsibilities:
//      1. Enforces the post-mutation invariant chain (FinalizeTrans-
//         formations -> ResetRuntimeData -> InvalidateSpatialStruct-
//         ure) on every transform edit.
//      2. Records every mutation as a value-typed SceneEdit into an
//         EditHistory for undo/redo.
//      3. Tags a "dirty scope" enum consulted by SceneEditController
//         to decide between cheap re-render and expensive photon regen.
//
//  See docs/INTERACTIVE_EDITOR_PLAN.md §4.3 and
//  docs/gui/ADR_DOCUMENT_FIRST_EDITOR.md.
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
#include "../Interfaces/IJobPriv.h"   // RISE::CstBatchUndoOp
#include "DirtyTracker.h"
#include "../Interfaces/IScenePriv.h"
#include "../Cst/Cst.h"               // CstHeadVersion (derived dirty)
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
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

	//! Single source of truth for `standard_object` surface-binding params
	//! disallowed on container nodes (pure transforms with no geometry).
	//! Shared between SceneEditor and SceneEditController's agent-commit gate.
	bool IsObjectSurfaceBindingParamName( const String& param );

	class SceneEditor
	{
	public:
		//! Coarse change classification consulted via LastDirtyScope().
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

		//! Optional manager hooks installed by SceneEditController; null managers
		//! cause dependent property edits to fail at Apply time.
		void SetMaterialManager( class IMaterialManager* mgr ) { mMaterialManager = mgr; }
		void SetShaderManager( class IShaderManager* mgr ) { mShaderManager = mgr; }
		void SetPainterManager( class IPainterManager* mgr ) { mPainterManager = mgr; }
		void SetScalarPainterManager( class IScalarPainterManager* mgr ) { mScalarPainterManager = mgr; }

		//! Optional Job pointer for medium resolution and CST routing.
		//! Initial bind captures the clean head baseline; same-Job rebind
		//! (after a full re-derive) preserves the saved baseline and refreshes derived dirty.
		void SetJob( IJob* job )
		{
			const bool initialBinding = ( mJob != job );
			mJob = job;
			if( initialBinding ) {
				CaptureSavedHeadVersion_();
			} else {
				RecomputeHeadDiffers_();
				RecomputeHasUnsavedChangesCached_();
			}
		}

		//! Re-point at the Job's current scene after a whole-scene rebuild; call
		//! alongside Set*Manager setters so cached pointers do not dangle.
		void RebindScene( IScenePriv& scene ) { mScene = &scene; mScenePhotonsExist = ComputeScenePhotonsExist(); mSceneScale = 0; }

		//! Apply an edit, run the invariant chain, push onto history, and update
		//! LastDirtyScope(). Returns false (scene unmodified) if invalid or target missing.
		bool Apply( const SceneEdit& edit );

		//! Pop the most recent edit and apply its inverse. Returns false if stack
		//! is empty, refused, or target is gone.
		bool Undo();

		//! Re-apply a previously undone edit.
		bool Redo();

		//! Bracket multiple Apply() calls into a single undo entry.
		void BeginComposite( const char* label );
		void EndComposite();

		//! Reset composite depth to 0 without pushing an orphan CompositeEnd
		//! (used during mid-gesture transactional rollback).
		void ForceCompositeDepthZero() { if( mCompositeDepth > 0 ) mCompositeDepth = 0; }

		//! True while a composite is open. BeginTransaction refuses mid-composite
		//! so rollback cannot walk past the baseline into an enclosing group.
		bool IsCompositeOpen() const { return mCompositeDepth > 0; }

		//! Snapshot/restore all dirty state (DirtyTracker + mScaleFromAnchorSet)
		//! for transactional rollback. Restore OR-merges the CST-head boolean
		//! (see DirtyTracker.h) and fires the dirty-changed notification.
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

		//! True if the scene had any populated photon map at construction/rebind.
		bool ScenePhotonsExist() const { return mScenePhotonsExist; }

		//! Most recently applied scene time (atomic acquire; defaults to 0).
		Scalar LastSceneTime() const
		{
			return mLastSetTime.load( std::memory_order_acquire );
		}

		//! Lazily computed and cached union-bbox diagonal for scaling camera pan/zoom rates.
		Scalar SceneScale() const;

		//! Canonical orbit/pan/zoom/roll math shared with per-pane override cameras.
		static void ApplyCameraOpToCamera(
			Implementation::CameraCommon& cam,
			const SceneEdit& e,
			Scalar sceneScale );

		const EditHistory& History() const { return mHistory; }
		EditHistory&       History()       { return mHistory; }

		//! Dirty-since-load channels feeding HasUnsavedChanges() (docs/gui/ADR_DOCUMENT_FIRST_EDITOR.md §7.1).
		const DirtyTracker& Dirty() const { return mDirtyTracker; }
		DirtyTracker&       Dirty()       { return mDirtyTracker; }

		//! Objects touched by ScaleObjectFromAnchor since load/save; feeds HasUnsavedChanges() and DirtySnapshot.
		const std::unordered_set<std::string>& ScaleFromAnchorSet() const { return mScaleFromAnchorSet; }
		void ClearDirtyState()
		{
			mDirtyTracker.Clear();
			mScaleFromAnchorSet.clear();
			CaptureSavedHeadVersion_();
			FireDirtyChangedIfTransitioned();
		}

		//! Mark dirty for an agent commit that mutated the retained CST directly.
		//! Known kinds route to per-category channels; empty names or unknown kinds
		//! unconditionally set the CST-head boolean channel. Fires dirty transition.
		void MarkCstHeadDirty( const char* entityName, const char* entityKind )
		{
			const std::string name = entityName ? entityName : std::string();
			const std::string kind = entityKind ? entityKind : std::string();

			EntityCategory category;
			const bool isKnown = ClassifyCstEntityKind( kind, category );

			// Empty name would no-op in MarkEntityDirty; set the unconditional boolean channel instead.
			if( name.empty() ) {
				mDirtyTracker.MarkCstHeadDirty();
			} else if( isKnown ) {
				mDirtyTracker.MarkEntityDirty( category, name );
			} else {
				mDirtyTracker.MarkCstHeadDirty();
			}
			FireDirtyChangedIfTransitioned();
		}

		//! Bump Scene light-topology generation for an agent CST param edit:
		//! checks emissivity for material kinds, bumps conservatively for empty/unknown
		//! kinds, and no-ops for known non-material kinds.
		void BumpSceneLightGenerationForAgentParamEdit(
			const char* entityName, const char* entityKind );

		//! Record an already-applied agent param commit onto EditHistory (clears redo).
		//! Does not mark dirty or update mLastScope (caller does). `occ`/`occAddressed`
		//! arm the repeatable-param undo/redo drift guard.
		void PushAgentCstParamEdit(
			const String& entityName, const String& entityKind, const String& param,
			const String& newValue, const String& prevValue, bool prevValueWasAbsent,
			int occ = 0, bool occAddressed = false,
			const std::string& ownerPreText = std::string(), const std::string& ownerPostText = std::string() );

		//! Record an already-applied agent chunk insert or remove onto EditHistory
		//! (clears redo; caller marks dirty). `chunkBytes` is exact text (insert input
		//! or pre-erase dropped bytes); `docIndex` is the top-level splice index;
		//! `wasRasterizer` applies to removes only.
		void PushAgentChunkCrudEdit(
			bool isInsert, const String& chunkName, const String& chunkKind, const String& chunkBytes,
			int docIndex, bool wasRasterizer );

		//! Record an already-applied atomic batch chunk remove as one EditHistory entry
		//! (clears redo; caller marks dirty). `priorDocText` is the Undo payload;
		//! `redoTargetLines` (`kind\tname` per line) is the Redo payload.
		void PushAgentRemoveChunksEdit(
			const String& displayTargets, const String& priorDocText, const String& redoTargetLines,
			bool anyWasRasterizer );

		//! Record an already-applied whole-document geometry replacement as one
		//! EditHistory entry using byte-exact `priorDocText`/`postDocText` (caller marks dirty).
		void PushAgentReplaceGeometryEdit(
			const String& objectName, const String& priorDocText, const String& postDocText );

		//! Record an already-applied positioned Duplicate-node fork as one EditHistory
		//! entry using byte-exact `priorDocText`/`postDocText` (docs/gui/NODE_GRAPH_CANVAS.md §6;
		//! caller marks dirty).
		void PushAgentDuplicateNodeEdit(
			const String& copyName, const String& copyKeyword,
			const String& priorDocText, const String& postDocText );

		//! Record an already-applied agent edit batch as one EditHistory entry holding
		//! operation-level inverses in landing order (caller marks dirty).
		void PushAgentEditBatchEdit(
			const String& label, const std::vector<RISE::CstBatchUndoOp>& ops, bool landedRasterizerInsert );

		//! Return and clear any user-facing notice from a refused Undo/Redo ("" if none).
		std::string ConsumeHistoryNotice();
		void ClearHistoryNotice();

		//! O(1) lock-free dirty state combining the CST revision-diff floor
		//! (docs/gui/ADR_DOCUMENT_FIRST_EDITOR.md §3.3), DirtyTracker, and
		//! mScaleFromAnchorSet. Conservative (may be true when Save would NoOp).
		bool HasUnsavedChanges() const
		{
			return mHasUnsavedChangesCached.load( std::memory_order_acquire );
		}

		//! Coalesced dirty-transition listener delivered outside controller locks.
		//! Key on the reported bool value, never on call count; bridges should
		//! marshal to their UI queue. Pass `nullptr` to detach.
		using DirtyChangedFn = std::function<void(bool hasUnsavedChanges)>;
		using DirtyChangedPtr = std::shared_ptr<DirtyChangedFn>;

	private:
		friend class SceneEditController;
		//! Atomically swap listener and wait for any in-flight callback (except on
		//! the callback thread itself) to quiesce; returns retired owner so arbitrary
		//! capture destructors never run under the notification mutex.
		DirtyChangedPtr ExchangeDirtyChangedListener( DirtyChangedFn fn )
		{
			const std::shared_ptr<DirtyNotificationState> state = mDirtyNotificationState;
			DirtyChangedPtr incoming;
			if( fn ) incoming = std::make_shared<DirtyChangedFn>( std::move( fn ) );
			DirtyChangedPtr retired;
			{
				std::unique_lock<std::mutex> lk( state->mutex );
				if( !state->registrationClosed )
				{
					retired.swap( state->listener );
					state->listener.swap( incoming );
				}
				else
				{
					retired.swap( incoming );
				}
				if( state->callbacksInFlight != 0
				 && state->callbackThread != std::this_thread::get_id() ) {
					state->callbackCV.wait( lk, [&] {
						return state->callbacksInFlight == 0;
					} );
				}
			}
			return retired;
		}

		//! Terminal teardown detach: closes registration and removes the listener
		//! under one mutex hold, quiesces in-flight callbacks, and returns the retired owner.
		DirtyChangedPtr CloseDirtyChangedListener()
		{
			const std::shared_ptr<DirtyNotificationState> state = mDirtyNotificationState;
			DirtyChangedPtr retired;
			{
				std::unique_lock<std::mutex> lk( state->mutex );
				state->registrationClosed = true;
				retired.swap( state->listener );
				if( state->callbacksInFlight != 0
				 && state->callbackThread != std::this_thread::get_id() ) {
					state->callbackCV.wait( lk, [&] {
						return state->callbacksInFlight == 0;
					} );
				}
			}
			return retired;
		}

	public:
		//! Replace/detach listener; target retirement runs under dirty-callback lifecycle TLS.
		void SetDirtyChangedListener( DirtyChangedFn fn );

	private:
		//! Advance Scene light-topology generation so reused RayCasters rebuild LightSampler.
		void BumpSceneLightGeneration();
		//! Bump light-topology generation iff either `prevMat` or `newMat` is emissive (null-safe).
		void BumpSceneLightGenerationIfEmitterSetChanged(
			const class IMaterial* prevMat, const class IMaterial* newMat );
		//! Bump light-topology generation iff `mat` is emissive (for area/position/exitance edits).
		void BumpSceneLightGenerationIfMaterialEmits( const class IMaterial* mat );

		//! Shared CST chunk-kind -> EntityCategory classifier (returns false for empty/unknown kinds).
		static bool ClassifyCstEntityKind( const std::string& kind, EntityCategory& outCategory );

		IScenePriv*  mScene;                                  // borrowed; re-pointable via RebindScene
		class IMaterialManager*       mMaterialManager;       // borrowed; nullable
		class IShaderManager*         mShaderManager;         // borrowed; nullable
		class IPainterManager*        mPainterManager;        // borrowed; nullable
		class IScalarPainterManager*  mScalarPainterManager;  // borrowed; nullable
		IJob*                         mJob;                   // borrowed; nullable
		std::string                   mHistoryNotice;
		void NoteHistoryRefusal_( const SceneEdit& edit, bool forward, const std::string& why, bool fixable );
		EditHistory  mHistory;
		DirtyScope   mLastScope;
		int          mCompositeDepth;
		bool         mScenePhotonsExist;
		std::atomic<Scalar> mLastSetTime;                     // atomic snapshot for production-render handoff
		mutable Scalar mSceneScale;                           // lazy bbox-union diagonal (0 = uncomputed)

		DirtyTracker  mDirtyTracker;
		std::unordered_set<std::string> mScaleFromAnchorSet;

		// UI-thread-only deferred CST transform/camera commit state (flushed while render thread is parked).
		std::unordered_set<std::string> mPendingCstObjMatrix;
		std::unordered_set<std::string> mPendingCstObjForward;

		// Verbatim pre/post chunk text per object for exact-byte transform Undo restoration,
		// verified against `prePose` (FormatMatrix16) before swapping.
		struct CstTransformSpelling_ { std::string pre; std::string post; std::string prePose; };
		std::unordered_map< std::string, std::vector<CstTransformSpelling_> > mCstTransformSpellings;
		static constexpr std::size_t kCstTransformSpellingCap = 32;
		static constexpr std::size_t kCstTransformSpellingObjectCap = 256;
		std::unordered_map< std::string, std::string > mPendingCstObjPrePose;

		std::string mPendingCstCameraName;
		std::string mLastNonRoutableTransformObj;             // dedupes csg_object transform refusal logs
		std::set<std::string> mWorldOpRefusalWarned;          // per-instance dedup for singular parent-chain refusals

		// True if the last Apply's CST re-derive mutated the live scene (raw code 1/2/3),
		// even when code 3 caused Apply() to return false; reset at Apply() entry.
		bool mCstLiveSceneChanged = false;

		//! Outlives SceneEditor via shared_ptr so concurrent teardown can quiesce safely.
		struct DirtyNotificationState
		{
			DirtyChangedPtr  listener;
			bool             registrationClosed = false;
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

		//! State-only drain (never dereferences SceneEditor); blocks concurrent teardown via callbacksInFlight.
		static bool DrainDirtyNotificationState_(
			const std::shared_ptr<DirtyNotificationState>& state );

		std::atomic<bool> mHeadDiffersCached{ false };        // cached derived-dirty floor (updated under mutation lock)
		std::atomic<bool> mHasUnsavedChangesCached{ false };  // lock-free combined dirty answer
		RISE::Cst::CstHeadVersion mSavedHeadVersion;          // clean baseline at bind / last save

		void CaptureSavedHeadVersion_();
		bool HeadDiffersFromSaved_() const { return mHeadDiffersCached.load( std::memory_order_acquire ); }
		//! Recompute cached revision floor; requires stable Document (caller holds mutation lock).
		void RecomputeHeadDiffers_();
		void RecomputeHasUnsavedChangesCached_();

	public:
		//! RAII scope deferring dirty-listener delivery across composite controller calls;
		//! outermost destructor performs the state-only drain.
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

		//! Record a potential dirty transition without invoking the listener (safe under any lock).
		//! Controller calls DrainDirtyNotification() after releasing locks.
		void FireDirtyChangedIfTransitioned();

		//! Deliver coalesced dirty transition if state changed. MUST be called with NO
		//! controller locks held; thread-safe, contains listener exceptions. Returns false
		//! if owner teardown marked the editor dead (caller must stop touching members).
		bool DrainDirtyNotification();

		//! True while this thread is executing or retiring this editor's dirty listener.
		bool IsInDirtyChangedCallbackOnThisThread() const;

		//! Recompute cached derived-dirty floor (call only while Document is stable).
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

		//! Flush deferred object/camera CST commits; controller MUST call at a render-parked boundary.
		bool HasPendingCstObjectTransforms() const { return !mPendingCstObjMatrix.empty(); }
		bool CommitPendingCstObjectTransforms();
		bool HasPendingCstCameraPose() const { return !mPendingCstCameraName.empty(); }
		bool CommitPendingCstCameraPose();

		//! True if the last Apply()'s CST re-derive modified the live scene (raw code 1/2/3).
		bool CstLiveSceneChangedInLastApply() const { return mCstLiveSceneChanged; }

	private:
		IObjectPriv* FindObject( const String& name ) const;
		//! Apply forward transform/binding op (`isReplay` suppresses container creation gates).
		bool ApplyObjectOpForward( IObjectPriv& obj, const SceneEdit& edit, bool isReplay );
		void RestoreObjectTransform( IObjectPriv& obj, const SceneEdit& edit );
		//! Resolve edited camera by recorded name (falls back to active camera if unnamed).
		class ICamera* ResolveEditedCamera( const SceneEdit& e );
		//! Entity registration serial for remove+re-add identity detection (0 = untracked).
		unsigned long long ResolveTargetSerial( const SceneEdit& e ) const;
		bool ApplyMaterialSlotByName( const SceneEdit& e, const String& painterName );
		//! Re-point cached scene/managers after a D2 full re-derive (result 2 or 3).
		void RebindToJob_();
		//! Route property edit through retained CST; self-rebinds on D2, returns false on code 0 or 3.
		bool RouteCstParamEdit_( const char* entityName, const char* entityKind, const char* role, const char* value );

		//! Determine whether a light `color` edit needs an atomic `colorspace Rec709RGB_Linear` +
		//! `color` composite write (preventing double gamma-decode on non-linear light chunks) and
		//! capture raw prior texts for verbatim Undo. Returns false on non-`color` roles, already-linear
		//! chunks, or duplicate `color`/`colorspace` occurrences; sets `outRefuse` if the composite
		//! is required but original text capture failed (callers must refuse the edit).
		bool LightColorCompositeState_( const char* lightName, const String& propertyName,
		                                String& outPrevColorText, String& outPrevColorSpaceText,
		                                bool& outRefuse ) const;
		//! Atomic `colorspace` + `color` write via `Job::ApplyCstParamEdits`.
		bool RouteCstLightColorComposite_( const char* lightName, const char* colorValue, const char* colorSpaceValue );

		//! Mutation-keyed (`r >= 1`) CST routes for agent Undo/Redo; `outDiagnosed` flags code-3 re-derives.
		bool RouteCstParamRemove_( const char* entityName, const char* entityKind, const char* role, int occ, bool* outDiagnosed = nullptr );
		bool RouteCstParamEditChecked_( const char* entityName, const char* entityKind, const char* role, const char* value, int occ, bool* outDiagnosed = nullptr );
		//! Verbatim owner-chunk swap for SetAgentCstParam Undo/Redo: -1 = N/A, 0 = failed derive, 1 = landed.
		int RouteCstParamOwnerRestore_( const SceneEdit& edit, bool forward, bool* outDiagnosed = nullptr );

		//! Drift guard for occurrence-addressed edits: verifies `edit.cstParamOcc` still exists
		//! and matches `expectedValue` (whitespace-normalized). Always true if `!cstParamOccAddressed`.
		bool OccurrenceEditStillAddressable_( const SceneEdit& edit, const char* expectedValue, const char* direction ) const;

		//! Mutation-keyed (`r >= 1`) chunk-CRUD, batch-remove, and document-swap routes for agent Undo/Redo.
		bool RouteAgentChunkCrud_( const SceneEdit& edit, bool forInsertOp, bool forward, bool* outDiagnosed = nullptr );
		bool RouteAgentRemoveChunksBatch_( const SceneEdit& edit, bool forward, bool* outDiagnosed = nullptr );
		bool RouteAgentDocumentSwap_( const SceneEdit& edit, bool forward, bool* outDiagnosed = nullptr );

		bool RouteObjectShadowFlagsToCst_( const String& objectName, int flags );
		void NoteCstObjectTransform_( const String& name, bool revert = false );
		//! Swap undone object chunk back to its pre-transform text if verified against `matrix16`.
		bool RestoreCstObjectSpelling_( const std::string& name, const std::string& matrix16, bool& routed );
		void NoteCstCameraDrag_( const String& camName );
		bool ApplyCstObjectMatrix_( const std::string& name, const std::string& matrix16 );
		bool ApplyCstObjectComponents_( const std::string& name, const std::string& position, const std::string& orientation );

		//! Single per-op forward/revert dispatchers shared by single and composite paths.
		//! `isReplay` is intentionally non-defaulted: true suppresses container creation gates on replay.
		bool ApplyForwardMutation( const SceneEdit& edit, bool isReplay );
		bool ApplyRevertMutation( const SceneEdit& edit );
		//! Validate target and capture prev* state before ApplyForwardMutation (no mutation on false).
		bool CaptureForApply( SceneEdit& edit );
		DirtyScope AggregateCompositeScope( bool sawObjectOp, bool sawCameraOp, bool sawTimeOp, bool sawPropertyOp ) const;
		void RunObjectInvariantChain( IObjectPriv& obj );
		//! Route property-shaped edits to DirtyTracker. Must be called only after mutation succeeds.
		void MarkEditEntityDirty( const SceneEdit& edit );
		bool ComputeScenePhotonsExist() const;
	};
}

#endif
