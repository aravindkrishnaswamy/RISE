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

        // ---- Aggregate ----------------------------------------------

        /// True iff EITHER channel has anything — drives the GUI's
        /// "unsaved changes" / Save-button enable state.
        bool HasAnyDirty() const
        {
            return !mNames.empty() || !mEntityDirty.empty();
        }

        /// Clear BOTH channels.  Called after a successful save.
        void Clear();

    private:
        std::unordered_set<std::string> mNames;        ///< object transform dirty
        std::set<DirtyEntity>           mEntityDirty;  ///< property-shaped dirty (sorted)
    };
}

#endif
