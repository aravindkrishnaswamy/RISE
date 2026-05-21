//////////////////////////////////////////////////////////////////////
//
//  OverrideSpanIndex.h - Phase 6.2 catalog of `override_object`
//    chunks parsed from a scene file.
//
//  Spec: docs/ROUND_TRIP_SAVE_PLAN.md §6.8 + pinned 2.16.
//
//  Every `override_object` chunk in the loaded file gets one
//  OverrideRecord here, regardless of whether it lives INSIDE our
//  managed sentinel-bracketed block (`managed == true`) or was
//  hand-authored outside it (`managed == false`).  The save engine
//  (Phase 6.4) reads this to:
//    - Seed `accumulator` from MANAGED records (FindManaged).
//    - Refuse Mode A in-place rewrites when an UNMANAGED chunk
//      targeting the same name exists (HasUnmanagedFor).
//
//  Unmanaged records are detection-only: the save engine never
//  reads their field values into the accumulator (R4 §1).
//
//////////////////////////////////////////////////////////////////////

#ifndef OverrideSpanIndex_
#define OverrideSpanIndex_

#include "../Utilities/Math3D/Math3D.h"
#include "SourceSpanIndex.h"  // for OffsetDelta (shared cross-index type)
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RISE
{
    // Canonical sentinel comments bracketing the managed override
    // block.  Shared between the parser's `mInsideManagedOverrideBlock`
    // classifier (AsciiSceneParser.cpp) and the save engine's
    // `LocateManagedBlock` (SaveEngine.cpp).  Both sides match the
    // line content EXACTLY (after stripping leading whitespace + CR);
    // substring matches are too loose — a user-written comment like
    // `# TODO: RISE editor overrides go here` would otherwise flip
    // the parser's classifier without the locator agreeing.
    inline constexpr const char* kManagedBlockSentinelOpen =
        "# === RISE editor overrides (managed by interactive editor) ===";
    inline constexpr const char* kManagedBlockSentinelClose =
        "# === end RISE editor overrides ===";

    /// Per-chunk record for an `override_object` block in the source.
    struct OverrideRecord
    {
        std::string targetName;           // the `name` field of the chunk
        std::string filePath;             // source file the chunk lives in
        std::size_t chunkBeginOffset = 0; // first byte of the chunk keyword
        std::size_t chunkEndOffset   = 0; // one past the closing `}` line
        bool        managed = false;      // true iff inside the sentinel block

        // Which fields were present in the chunk.  Used by save-engine
        // value-comparison; only consulted for MANAGED records.
        bool        hasPosition    = false;
        bool        hasOrientation = false;
        bool        hasQuaternion  = false;
        bool        hasMatrix      = false;
        bool        hasScale       = false;

        // The chunk's applied values.  Only consulted for MANAGED
        // records (R4 §1 — unmanaged records are detection-only).
        // RISE has no Vector4 type; quaternion is a plain double[4]
        // (xyzw, glTF convention) — matches how `bag.GetVec4` returns
        // quaternion values to the chunk parser.
        Vector3 position;        // default-constructed = (0,0,0)
        Vector3 orientation;     // DEGREES, matching scene-language convention
        double  quaternion[4];   // xyzw, glTF convention; identity = (0,0,0,1)
        Matrix4 matrix;          // default-constructed = identity
        Vector3 scale;           // per-axis stretch

        OverrideRecord()
            : position(0, 0, 0)
            , orientation(0, 0, 0)
            , scale(1, 1, 1)
        {
            quaternion[0] = 0;
            quaternion[1] = 0;
            quaternion[2] = 0;
            quaternion[3] = 1;
        }
    };

    /// Catalog of every override_object chunk in the loaded file.
    /// Append-only; iteration order matches scene-file order.
    ///
    /// API duty separation (R4 §1, §2):
    ///   - MANAGED records: FindManaged returns at-most-one per name
    ///     (canonical layout guarantees, pinned 2.7).  Value
    ///     fields consulted by the save engine for the accumulator
    ///     seed.
    ///   - UNMANAGED records: HasUnmanagedFor is the only API that
    ///     consults them.  Their value fields are NEVER read into
    ///     the accumulator — they survive verbatim in the file
    ///     because we never touch their bytes.
    class OverrideSpanIndex
    {
    public:
        OverrideSpanIndex();
        ~OverrideSpanIndex();

        void Add( OverrideRecord&& rec );

        /// Return the at-most-one managed record for `name`, or
        /// nullptr.  If multiple managed records exist (malformed
        /// save), returns the LAST one in scene-file order — the
        /// engine's overall behaviour is best-effort; the malformed-
        /// sentinel case is caught loudly elsewhere.
        const OverrideRecord* FindManaged( const std::string& name ) const;

        /// True iff ANY record for `name` has `managed == false`.
        /// Drives the save engine's Mode A refusal gate.
        bool HasUnmanagedFor( const std::string& name ) const;

        /// All records for a name (managed + unmanaged), in scene-file
        /// order.  Used by the save engine's existing-block erase pass
        /// (which only operates on entries with managed == true).
        std::vector<const OverrideRecord*> FindAll( const std::string& name ) const;

        std::size_t Count() const;
        void        Clear();

        /// R-final-3 P1: post-save offset adjustment for UNMANAGED
        /// records.  Mode A length-changing splices shift every byte
        /// position >= the edit's end offset; without this, unmanaged
        /// override chunkBeginOffset / chunkEndOffset stay at their
        /// load-time values and subsequent same-session saves consult
        /// stale byte positions for placement / refusal decisions.
        /// Managed records' offsets are also walked here for parity
        /// (they're typically wiped by `RemoveAllManaged` before the
        /// next save, but applying deltas first keeps the invariant
        /// "every offset in this index points at the right byte"
        /// intact through the entire post-save sequence).
        void ApplyOffsetDeltas( const std::vector<OffsetDelta>& deltas );

        /// R-final-3 P1: wipe every record with `managed == true`,
        /// preserving UNMANAGED records (their bytes were never
        /// touched by the save).  Used by the save engine right
        /// before re-seeding the managed catalog from the just-
        /// emitted accumulator — without this, a same-session
        /// second save would see the old (now-stale) managed entries
        /// plus the new ones it's about to add.
        void RemoveAllManaged();

        /// Phase 6.5 Save-As re-anchor: rewrite every record whose
        /// filePath matches `oldPath` to `newPath`.  Both managed
        /// and unmanaged records are remapped, because Save-As
        /// copies the source's bytes (including unmanaged overrides)
        /// into the target — so the unmanaged chunks now physically
        /// live in the target file.  Records from a different file
        /// (`> load` chains) are left alone.
        void RemapFilePath( const std::string& oldPath,
                            const std::string& newPath );

        // Direct access for tests / save engine iteration.
        const std::vector<OverrideRecord>& Entries() const { return mEntries; }

    private:
        std::vector<OverrideRecord>                          mEntries;
        std::unordered_multimap<std::string, std::size_t>    mByName;
    };
}

#endif
