//////////////////////////////////////////////////////////////////////
//
//  SourceSpanIndex.h - Phase 6.1 of the round-trip-save pipeline.
//
//  Per-entity source-file metadata captured at scene load.  Lets the
//  save engine (Phase 6.4) decide Mode A (in-place line rewrite) vs
//  Mode B (managed override block) per parameter, and route every
//  dirty target's placement-offset / cross-file checks (R5 + R7 fixes).
//
//  Mirrors the data structure pinned in
//  docs/ROUND_TRIP_SAVE_PLAN.md §6.3.
//
//  Ownership: held by Job via unique_ptr; exposed read-only via
//  IJobPriv::GetSourceSpanIndex().  Populated by AsciiSceneParser
//  after each standard_object chunk's Finalize succeeds.
//
//////////////////////////////////////////////////////////////////////

#ifndef SourceSpanIndex_
#define SourceSpanIndex_

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RISE
{
    /// One byte-range edit + its length delta.  After a successful
    /// save the engine compiles its `EditScript` into this form and
    /// calls `SourceSpanIndex::ApplyOffsetDeltas` /
    /// `OverrideSpanIndex::ApplyOffsetDeltas` so subsequent in-session
    /// saves can splice at the correct (post-edit) byte positions.
    /// R-final P1 #1: without this, the second save uses load-time
    /// offsets that point at unrelated bytes after the first save's
    /// length-changing splices.  R-final-3 P1: the OverrideSpanIndex
    /// shares the same delta type so its unmanaged records track the
    /// same byte shifts.
    struct OffsetDelta
    {
        std::size_t threshold;   // bytes at this offset and ABOVE shift
        long long   delta;       // signed byte delta (positive = inserted, negative = removed)
    };

    /// Per-token byte range + macro-substitution sentinel.  Public
    /// data shape; populated from Phase 0's RawTokenCapture.
    struct RawTokenSpan
    {
        std::size_t byteBegin = 0;
        std::size_t byteEnd   = 0;
        bool        isSymbolic = false;
    };

    /// Per-parameter source location.  A parameter line like
    /// `position 0 0 0  # important value` produces:
    ///   keyword: "position" (covers the leading keyword)
    ///   value range: byte range of "0 0 0"
    ///   comment range: byte range of "# important value"
    /// Mode A's splice writes into [valueBegin, valueEnd); the
    /// trailing `# comment ...\n` is byte-preserved because it
    /// lives at [commentBegin, lineEndOffset).
    struct ParameterSpan
    {
        std::size_t lineBeginOffset = 0;   // first byte of the line (incl indent)
        std::size_t lineEndOffset   = 0;   // one past last content byte (excludes EOL)
        std::size_t valueBegin      = 0;   // first byte of the first value token
        std::size_t valueEnd        = 0;   // one past last byte of last value token
        std::size_t commentBegin    = 0;   // offset of '#' if present, else == lineEndOffset
        bool        isSymbolic      = false;  // OR of all value-token isSymbolic flags
    };

    /// Which transform path the source used.  Mirrors `standard_object`'s
    /// `matrix > quaternion > orientation` precedence (the parser's
    /// runtime decision at [AsciiSceneParser.cpp:5500]).
    enum class AuthorMode : unsigned char
    {
        Euler,       // source has `orientation` (or only position/scale, or nothing)
        Quaternion,  // source has `quaternion`
        Matrix       // source has `matrix`
    };

    /// Source location for a parsed entity.  Owned by IJob via the
    /// SourceSpanIndex map.  See §6.3.
    struct SourceSpan
    {
        std::string filePath;
        std::size_t chunkBeginOffset      = 0;   // first byte of the chunk-keyword token
        std::size_t chunkEndOffset        = 0;   // one past the closing `}`
        std::size_t bodyOpenBraceOffset   = 0;   // offset of the `{` byte
        std::size_t bodyCloseBraceOffset  = 0;   // offset of the `}` byte
        std::size_t bodyCloseBraceLineBegin = 0; // first byte of the line containing `}`
                                                  // (Phase 6.4 uses this for "insert before }" splice)
        AuthorMode  authorMode = AuthorMode::Euler;
        bool        chunkRevisited = false;      // set TRUE when the parser re-enters
                                                  // this chunk's byte range (FOR-body iteration)
        std::unordered_map<std::string, ParameterSpan> parameterSpans;
    };

    /// Per-entity source data registry.  See §6.3 and §6.4 for
    /// population rules (first-visit gets a SourceSpan; FOR-body
    /// 2..N entities don't get one, but every runtime entity DOES
    /// get a CreationLocation entry via RecordCreationLocation).
    /// File-identity fingerprint captured at scene-load time.
    /// Save-time mtime/size mismatch indicates the file was modified
    /// externally between load and save — applying load-time byte
    /// offsets to those changed bytes would corrupt unrelated content.
    /// See docs/ROUND_TRIP_SAVE_PLAN.md §11.6.
    struct FileIdentity
    {
        std::string  filePath;
        long long    mtimeSec  = 0;    // POSIX stat.st_mtime
        long long    mtimeNsec = 0;    // POSIX stat.st_mtim.tv_nsec (or 0 if unavailable)
        long long    sizeBytes = 0;    // POSIX stat.st_size
        bool         captured  = false;
    };

    class SourceSpanIndex
    {
    public:
        SourceSpanIndex();
        ~SourceSpanIndex();

        void Add(const std::string& name, SourceSpan&& span);
        const SourceSpan* Find(const std::string& name) const;
        std::size_t Count() const;

        /// Mutable access used by AsciiSceneParser to flip
        /// `chunkRevisited` when FOR loops re-enter a chunk.
        /// Returns null for unknown names.
        SourceSpan* FindMutable(const std::string& name);

        // R5 §1 / pinned 2.20: every successful runtime-entity creation
        // records its enclosing chunk's source-file path + end offset
        // here.  Stable across FOR-body iterations (siblings share the
        // same value).  R7 §1 / pinned 2.25: the filePath is recorded so
        // the save engine can refuse cross-file dirty targets.
        void RecordCreationLocation(const std::string& name,
                                    std::string filePath,
                                    std::size_t chunkEndOffset);
        std::size_t GetCreationOffsetEnd(const std::string& name) const;
        const std::string& GetCreationFilePath(const std::string& name) const;
        bool HasCreationLocation(const std::string& name) const;

        /// Sentinel returned by GetCreationOffsetEnd for unknown names.
        static constexpr std::size_t kNoCreationOffset = static_cast<std::size_t>(-1);

        /// Clears all state.  Called by AsciiSceneParser at the top of
        /// each parse.
        void Clear();

        /// Capture the top-level file's identity (path + mtime + size)
        /// at scene-load time.  Save engine compares against the
        /// current on-disk identity in step 0; mismatch returns
        /// Status::Refused with the external-modification diagnostic.
        void SetFileIdentity( FileIdentity id );
        const FileIdentity& GetFileIdentity() const { return mFileIdentity; }

        /// Walk every stored offset (per-span + per-parameter +
        /// per-creation-location) and apply the cumulative delta from
        /// `deltas`.  Each `OffsetDelta.threshold` is the END byte of
        /// the edit that produced it; offsets `>= threshold` shift by
        /// the delta.  Deltas are summed in sorted-threshold order.
        ///
        /// Mode A splices: an EditOp `{begin, end, replacement}`
        /// produces `{threshold = end, delta = replacement.size() -
        /// (end - begin)}`.  The parameter's `valueBegin` (== begin)
        /// stays put; `valueEnd` shifts by delta — putting the
        /// parameter's value range at [begin, begin + new_length),
        /// which is exactly where the spliced bytes now live.
        ///
        /// Pure inserts (begin == end): threshold == begin, delta ==
        /// replacement.size().  All offsets `>= begin` shift.
        void ApplyOffsetDeltas( const std::vector<OffsetDelta>& deltas );

        // Test/diagnostic helpers.
        const std::unordered_map<std::string, SourceSpan>& Entries() const { return mEntries; }

    private:
        struct CreationLocation
        {
            std::string filePath;
            std::size_t chunkEndOffset = 0;
        };
        std::unordered_map<std::string, SourceSpan>      mEntries;
        std::unordered_map<std::string, CreationLocation> mCreationLocation;
        std::string                                       mEmptyPath;  // for GetCreationFilePath fallback
        FileIdentity                                      mFileIdentity;
    };
}

#endif
