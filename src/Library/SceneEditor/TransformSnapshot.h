//////////////////////////////////////////////////////////////////////
//
//  TransformSnapshot.h - Phase 6.1 helper: per-name Matrix4 store.
//
//  Used twice by Job:
//    - mBaseTransforms: captured inside StandardObjectAsciiChunkParser::
//      Finalize, AFTER AddObject* succeeds but BEFORE any subsequent
//      chunk has had a chance to mutate the object.  Records the
//      "what the source chunks alone produced" state.
//    - mLoadedTransforms: captured at end-of-parse, AFTER all
//      override_object chunks (if any) have applied.  Records the
//      runtime state the user starts editing from.
//
//  Per R2 finding §3 (P1): both snapshots are needed so the save
//  engine can distinguish "user reverted to loaded state" from "user
//  reverted PAST the loaded state back to the chunk-only base."  The
//  former is a no-op rewrite (managed-block override stays); the
//  latter erases the managed-block entry.
//
//  Spec: docs/ROUND_TRIP_SAVE_PLAN.md §7.4.
//
//////////////////////////////////////////////////////////////////////

#ifndef TransformSnapshot_
#define TransformSnapshot_

#include "../Utilities/Math3D/Math3D.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace RISE
{
    class TransformSnapshot
    {
    public:
        void Add(const std::string& name, const Matrix4& m);
        const Matrix4* Find(const std::string& name) const;
        bool Contains(const std::string& name) const;
        std::size_t Count() const;
        void Clear();

        /// Sorted list of all names in the snapshot.  Used by the save
        /// engine for deterministic iteration order and by the
        /// end-of-parse loaded-snapshot population pass to know which
        /// names to query from the object manager.
        std::vector<std::string> Names() const;

    private:
        std::unordered_map<std::string, Matrix4> mEntries;
    };
}

#endif
