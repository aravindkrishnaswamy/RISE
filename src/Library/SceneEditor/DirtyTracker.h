//////////////////////////////////////////////////////////////////////
//
//  DirtyTracker.h - Phase 6.3 of the round-trip-save pipeline.
//
//  Records which OBJECTS have been mutated by transform-shaped
//  SceneEdit ops since the last load or save.  The save engine
//  (Phase 6.4) iterates DirtyTracker.Snapshot() to know which
//  entities to check against the loaded transform snapshot.
//
//  Coarse-grained by design (per object, not per field).  The save
//  engine's matrix-equality comparison (§9.4) resolves the
//  "drag → undo → no-op" case, so per-field bookkeeping would just
//  duplicate work.  See docs/ROUND_TRIP_SAVE_PLAN.md §7.1 + §7.5.
//
//  V1 scope: only OBJECT transform ops mark dirty.  Material /
//  shader / shadow / medium / camera / light edits do NOT touch
//  DirtyTracker — they're out-of-scope for V1's save engine
//  (Phase B will add parallel trackers per §7.6).
//
//////////////////////////////////////////////////////////////////////

#ifndef DirtyTracker_
#define DirtyTracker_

#include <string>
#include <unordered_set>
#include <vector>

namespace RISE
{
    class DirtyTracker
    {
    public:
        void MarkDirty( const std::string& objectName );
        void Clear();

        bool IsDirty() const { return !mNames.empty(); }
        bool Contains( const std::string& objectName ) const;

        /// Deterministic sorted list of dirty object names.  The save
        /// engine iterates this for ordered processing.
        std::vector<std::string> Snapshot() const;

        std::size_t Count() const { return mNames.size(); }

    private:
        std::unordered_set<std::string> mNames;
    };
}

#endif
