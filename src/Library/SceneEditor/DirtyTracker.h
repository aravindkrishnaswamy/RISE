//////////////////////////////////////////////////////////////////////
//
//  DirtyTracker.h - Phase 6.3 + Phase B of the round-trip-save
//    pipeline.
//
//  Records which entities have been mutated by the interactive
//  editor since the last load or save.  The save engine iterates
//  the tracker to know which entities to compare against their
//  loaded baseline (transform snapshot for objects; property
//  snapshot for everything else).
//
//  Two channels:
//    - mNames        — OBJECT TRANSFORM dirty set (Phase 6).
//                      Coarse per-object; the save engine's
//                      matrix-equality check (§9.4) resolves the
//                      "drag → undo → no-op" case.
//    - mEntityDirty  — Phase B per-(category,name) dirty set for
//                      property-shaped edits: object material /
//                      shader / shadow / interior-medium bindings,
//                      camera transform + properties, light
//                      properties, material slots, medium
//                      properties.  Also coarse per entity; the
//                      save engine diffs a parse-time PropertySnapshot
//                      against current introspection to find the
//                      changed parameters.
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
        /// The save engine's transform pass iterates this.
        std::vector<std::string> Snapshot() const;

        std::size_t Count() const { return mNames.size(); }

        // ---- Property channel (Phase B) -----------------------------

        /// Mark an entity dirty for property-shaped save routing.
        /// Empty `name` is ignored.
        void MarkEntityDirty( EntityCategory category, const std::string& name );

        /// Deterministic sorted list of (category,name) entity-dirty
        /// pairs.  The save engine's property pass iterates this.
        std::vector<DirtyEntity> EntitySnapshot() const;

        std::size_t EntityCount() const { return mEntityDirty.size(); }

        // ---- Created-entity channel (Phase C) -----------------------
        // Entities the editor CREATED this session (e.g. an AddCamera
        // clone).  These have no source span — the save engine emits
        // a fresh chunk for them inside the managed block.
        //
        // Two sub-channels because the two consumers have opposite
        // lifetimes:
        //   - `mCreatedPending`  — "created since the last save".
        //     Transient: cleared by Clear() so the Save button greys
        //     after a save.  Feeds HasAnyDirty().
        //   - `mSessionCreated`  — "created at any point this session".
        //     PERSISTENT across Clear(): the emitted chunk lives in
        //     the wholesale-re-rendered managed block, so every
        //     subsequent save must re-emit it.  Reset only when the
        //     whole tracker is reconstructed (new scene → new editor).

        /// Mark an entity as newly created.  Writes BOTH sub-channels.
        void MarkEntityCreated( EntityCategory category, const std::string& name );

        /// Sorted (category,name) list of entities created this
        /// SESSION — the save engine re-emits a fresh chunk for each.
        std::vector<DirtyEntity> SessionCreatedSnapshot() const;

        std::size_t SessionCreatedCount() const { return mSessionCreated.size(); }

        /// True iff (category,name) was created this session.  The
        /// save engine's property pass consults this: a property edit
        /// on a session-created entity is NOT a refusal (no source
        /// span) — the created-entity pass re-emits the whole chunk.
        bool IsSessionCreated( EntityCategory category,
                               const std::string& name ) const
        {
            return mSessionCreated.find( std::make_pair( category, name ) )
                != mSessionCreated.end();
        }

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
                || !mCreatedPending.empty();
        }

        /// Clear the TRANSIENT channels (object transform, property,
        /// created-pending).  Called after a successful save.  The
        /// persistent `mSessionCreated` channel is intentionally kept.
        void Clear();

// ---- Transaction snapshot (F7) ------------------------------
// The editor's transactional rollback restores the dirty channels
// to their pre-transaction state, so a fully reverted document does
// not keep showing unsaved changes (undo RE-MARKS dirty, and created
// entities are never un-marked).  A plain value copy of the four
// sets is sufficient.
struct State {
	std::unordered_set<std::string> names;
	std::set<DirtyEntity>           entityDirty;
	std::set<DirtyEntity>           createdPending;
	std::set<DirtyEntity>           sessionCreated;
};
State CaptureState() const { return State{ mNames, mEntityDirty, mCreatedPending, mSessionCreated }; }
void  RestoreState( const State& st ) { mNames = st.names; mEntityDirty = st.entityDirty; mCreatedPending = st.createdPending; mSessionCreated = st.sessionCreated; }

    private:
        std::unordered_set<std::string> mNames;          ///< object transform dirty (transient)
        std::set<DirtyEntity>           mEntityDirty;    ///< property-shaped dirty (transient)
        std::set<DirtyEntity>           mCreatedPending; ///< created since last save (transient)
        std::set<DirtyEntity>           mSessionCreated; ///< created this session (persistent)
    };
}

#endif
