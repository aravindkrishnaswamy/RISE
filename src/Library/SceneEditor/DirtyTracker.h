//////////////////////////////////////////////////////////////////////
//
//  DirtyTracker.h - Phase 6.3 + Phase B of the round-trip-save
//    pipeline.
//
//  Records which entities have been mutated by the interactive
//  editor since the last load or save.  Historically (Phase 6/B/C)
//  the byte-splice save engine iterated the tracker to decide which
//  entities to compare against their loaded baselines; the
//  CST-cutover Slice 6d deleted that mechanism.  Today
//  SaveEngine::Save is an unconditional whole-Document SerializeCst
//  (NoOp on byte-equality) whose ONLY tracker interaction is
//  Clear() after a successful save — the channels exist to feed
//  HasAnyDirty(), which drives SceneEditor::HasUnsavedChanges() and
//  thus the GUI's Save-button / unsaved-changes state.
//
//  Two original channels:
//    - mNames        — OBJECT TRANSFORM dirty set (Phase 6).
//                      Coarse per-object.
//    - mEntityDirty  — Phase B per-(category,name) dirty set for
//                      property-shaped edits: object material /
//                      shader / shadow / interior-medium bindings,
//                      camera transform + properties, light
//                      properties, material slots, medium
//                      properties.  Also coarse per entity.
//
//  Later phases added further channels, documented at their
//  sections below: the Phase C created-entity channels and the
//  Model-B F5 CST-head boolean channel.
//
//  See docs/ROUND_TRIP_SAVE_PLAN.md §7.1 + §7.5 + §7.6.
//
//////////////////////////////////////////////////////////////////////

#ifndef DirtyTracker_
#define DirtyTracker_

#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RISE
{
    /// Which family of scene entity an edit touched.  Phase 6 only
    /// needed objects; Phase B's property-shaped save path needs to
    /// disambiguate a camera "glass" from a material "glass" — entity
    /// names are unique only WITHIN a manager, not across managers.
    enum class EntityCategory : unsigned char
    {
        Object,    ///< property edits on a standard_object (material / shader / shadow / interior-medium)
        Camera,    ///< camera transform + property edits
        Light,     ///< light property edits
        Material,  ///< material slot edits
        Medium     ///< participating-medium property edits
    };

    /// One dirty entity: (category, manager-registered name).
    using DirtyEntity = std::pair<EntityCategory, std::string>;

    class DirtyTracker
    {
    public:
        // ---- Object transform channel (Phase 6) ---------------------

        void MarkDirty( const std::string& objectName );

        bool IsDirty() const { return !mNames.empty(); }
        bool Contains( const std::string& objectName ) const;

        /// Deterministic sorted list of object-transform-dirty names.
        /// No production consumer since the Slice-6d byte-splice
        /// deletion (the save engine's transform pass that iterated
        /// it is gone); retained for tests/diagnostics.
        std::vector<std::string> Snapshot() const;

        std::size_t Count() const { return mNames.size(); }

        // ---- Property channel (Phase B) -----------------------------

        /// Mark an entity dirty for property-shaped save routing.
        /// Empty `name` is ignored.
        void MarkEntityDirty( EntityCategory category, const std::string& name );

        /// Deterministic sorted list of (category,name) entity-dirty
        /// pairs.  No production consumer since the Slice-6d
        /// byte-splice deletion (the save engine's property pass that
        /// iterated it is gone); retained for tests/diagnostics.
        std::vector<DirtyEntity> EntitySnapshot() const;

        std::size_t EntityCount() const { return mEntityDirty.size(); }

        // ---- Created-entity channel (Phase C) -----------------------
        // Entities the editor CREATED this session (e.g. an AddCamera
        // clone).  Under the deleted byte-splice save these had no
        // source span, so the save engine emitted a fresh chunk for
        // them inside its managed block; that machinery is gone with
        // Slice 6d, and today the channel only feeds dirty state.
        //
        // Two sub-channels with opposite lifetimes:
        //   - `mCreatedPending`  — "created since the last save".
        //     Transient: cleared by Clear() so the Save button greys
        //     after a save.  Feeds HasAnyDirty().
        //   - `mSessionCreated`  — "created at any point this session".
        //     PERSISTENT across Clear() (a byte-splice-era rule: the
        //     emitted chunk lived in the wholesale-re-rendered managed
        //     block, so every subsequent save had to re-emit it).
        //     Reset only when the whole tracker is reconstructed
        //     (new scene → new editor).

        /// Mark an entity as newly created.  Writes BOTH sub-channels.
        void MarkEntityCreated( EntityCategory category, const std::string& name );

        /// Sorted (category,name) list of entities created this
        /// SESSION.  No production consumer since the Slice-6d
        /// byte-splice deletion (the save engine no longer re-emits
        /// per-entity chunks); retained for tests/diagnostics.
        std::vector<DirtyEntity> SessionCreatedSnapshot() const;

        std::size_t SessionCreatedCount() const { return mSessionCreated.size(); }

        /// True iff (category,name) was created this session.  No
        /// production consumer since the Slice-6d byte-splice
        /// deletion (the save engine's property pass that consulted
        /// it is gone); retained for tests/diagnostics.
        bool IsSessionCreated( EntityCategory category,
                               const std::string& name ) const
        {
            return mSessionCreated.find( std::make_pair( category, name ) )
                != mSessionCreated.end();
        }

        // ---- CST-head channel (Model-B F5) --------------------------
        // Boolean "the retained CST Document head was mutated" flag
        // for edits that map onto NONE of the per-entity categories
        // above — agent commits on painters / functions / rasterizer
        // params / shaders, or a commit whose entity name is empty.
        // SaveEngine::Save has NO dirty gate — it serializes the
        // whole Document unconditionally and NoOps on byte-equality;
        // the aggregate dirty bit (HasAnyDirty()) gates only the GUI
        // Save-button enable, so a single coarse boolean is exactly
        // the granularity that consumer needs — parking such names in
        // the object-transform set (the pre-A1 hack) was a semantic
        // overload.  Transient: cleared by Clear() so a successful
        // save greys the Save button.

        /// Set the CST-head-dirty flag.  UNCONDITIONAL — there is no
        /// name to validate — so the mark can never silently no-op
        /// (data-loss guard for the agent commit path).
        void MarkCstHeadDirty() { mCstHeadDirty = true; }

        /// True iff the CST head was marked dirty since the last
        /// load / save.
        bool CstHeadDirty() const { return mCstHeadDirty; }

        // ---- Aggregate ----------------------------------------------

        /// True iff there is anything to save SINCE THE LAST SAVE —
        /// drives the GUI's "unsaved changes" / Save-button state.
        /// Deliberately excludes `mSessionCreated` (persistent): a
        /// session-created entity that was already saved is not an
        /// unsaved change.
        bool HasAnyDirty() const
        {
            return !mNames.empty()
                || !mEntityDirty.empty()
                || !mCreatedPending.empty()
                || mCstHeadDirty;
        }

        /// Clear the TRANSIENT channels (object transform, property,
        /// created-pending, CST-head).  Called after a successful
        /// save.  The persistent `mSessionCreated` channel is
        /// intentionally kept.
        void Clear();

// ---- Transaction snapshot (F7) ------------------------------
// The editor's transactional rollback restores the dirty channels
// to their pre-transaction state, so a fully reverted document does
// not keep showing unsaved changes (undo RE-MARKS dirty, and created
// entities are never un-marked).  The four SETS restore by plain
// value copy.  The CST-head BOOLEAN is deliberately ASYMMETRIC:
// RestoreState OR-merges it (a set flag survives ANY restore),
// because its only writer is the agent-commit path
// (SceneEditController::ApplyAgentParamEdit), whose Document
// mutation has no EditHistory record — the rollback's Undo loop can
// never revert it.  A plain value copy would let a mid-transaction
// agent commit be wiped by the rollback's restore of the clean
// pre-transaction baseline while the mutated Document survives,
// silently losing the edit on a close-without-prompt.  (A flag set
// BEFORE the transaction began is captured in the snapshot and is
// preserved either way.)
struct State {
	std::unordered_set<std::string> names;
	std::set<DirtyEntity>           entityDirty;
	std::set<DirtyEntity>           createdPending;
	std::set<DirtyEntity>           sessionCreated;
	bool                            cstHeadDirty = false;
};
State CaptureState() const { return State{ mNames, mEntityDirty, mCreatedPending, mSessionCreated, mCstHeadDirty }; }
void  RestoreState( const State& st ) { mNames = st.names; mEntityDirty = st.entityDirty; mCreatedPending = st.createdPending; mSessionCreated = st.sessionCreated; mCstHeadDirty = st.cstHeadDirty || mCstHeadDirty; }

    private:
        std::unordered_set<std::string> mNames;          ///< object transform dirty (transient)
        std::set<DirtyEntity>           mEntityDirty;    ///< property-shaped dirty (transient)
        std::set<DirtyEntity>           mCreatedPending; ///< created since last save (transient)
        std::set<DirtyEntity>           mSessionCreated; ///< created this session (persistent)
        bool                            mCstHeadDirty = false; ///< CST head mutated, uncategorized (transient)
    };
}

#endif
