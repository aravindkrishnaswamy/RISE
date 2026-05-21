//////////////////////////////////////////////////////////////////////
//
//  SaveEngine.h - Phase 6.4 of the round-trip-save pipeline.
//
//  Implements the two-mode save algorithm pinned in
//  docs/ROUND_TRIP_SAVE_PLAN.md §9:
//
//    Mode A — in-place line rewrite for direct, Euler-authored,
//      single-visit, non-shadowed, decomposable parameter lines.
//      Splices the new value into the source file's bytes.
//
//    Mode B — managed override_object block for entities the source
//      file can't represent in-place (FOR-generated, matrix/quaternion
//      authored, shadowed by unmanaged overrides, ScaleObjectFromAnchor-
//      touched, etc.).  The block lives at a sentinel-bracketed
//      offset; placement is determined by the BARRIER `>` command
//      classification (§9.6.x) and refuses cross-file / barrier-
//      conflict / destructive-target cases.
//
//  Inputs are borrowed references; outputs are a SaveResult value.
//  Caller is responsible for the cancel-and-park dance against the
//  render thread (§9.9 — Phase 6.5).
//
//////////////////////////////////////////////////////////////////////

#ifndef SaveEngine_
#define SaveEngine_

#include <string>
#include <unordered_set>
#include <vector>

namespace RISE
{
    class IJobPriv;
    class SourceSpanIndex;
    class OverrideSpanIndex;
    class TransformSnapshot;
    class DirtyTracker;

    /// Outcome of a Save() call.  Status discriminates UI messaging.
    struct SaveResult
    {
        /// Pinned 2.22 / R5 §6: four-state status.
        enum class Status : unsigned char
        {
            Saved,    ///< bytes written to disk
            NoOp,     ///< no edits to write; file unchanged
            Refused,  ///< engine declined; original file untouched
            Failed    ///< IO error; original file untouched
        };

        Status      status = Status::Failed;
        std::string filePath;
        std::string errorMessage;          ///< populated on Refused / Failed

        // Diagnostic counters (§9.2 step 4 / pinned 2.23).  Count work
        // attempted INSIDE the DirtyTracker.Snapshot() loop — they are
        // NOT totals of all objects in the file.  On a zero-edits save
        // all four stay zero; the correctness signal is byte-identity,
        // not a counter value.
        unsigned int noOpCount            = 0;  ///< dirty but Mfinal == Mloaded
        unsigned int directRewriteCount   = 0;  ///< Mode A lines written
        unsigned int overrideRewriteCount = 0;  ///< Mode B per-field entries
        unsigned int matrixFallbackCount  = 0;  ///< force-matrix-override entries
    };

    /// True iff the status indicates a successful (file-state-coherent)
    /// outcome.  Saved and NoOp both qualify; Refused and Failed do not.
    inline bool Succeeded( SaveResult::Status s )
    {
        return s == SaveResult::Status::Saved
            || s == SaveResult::Status::NoOp;
    }

    /// Two-mode save engine.  See §9 for the full algorithm.
    ///
    /// Concurrency: the engine itself takes no locks and does no
    /// thread coordination.  The CALLER is responsible for parking
    /// the render thread (cancel + wait for !mRendering) before
    /// calling Save and resuming after — see §9.9 / Phase 6.5's
    /// `SceneEditController::RequestSave`.
    class SaveEngine
    {
    public:
        /// Borrowed references.  Lifetime: caller guarantees they
        /// outlive the SaveEngine.  `dirty` is non-const because
        /// Save() clears it on a successful (Saved or NoOp) outcome.
        SaveEngine(
            IJobPriv&                              job,
            const SourceSpanIndex&                 spans,
            const OverrideSpanIndex&               overrideSpans,
            const TransformSnapshot&               base,
            const TransformSnapshot&               loaded,
            DirtyTracker&                          dirty,
            std::unordered_set<std::string>&       scaleFromAnchorSet );

        SaveResult Save( const std::string& filePath );

    private:
        IJobPriv&                              mJob;
        const SourceSpanIndex&                 mSpans;
        const OverrideSpanIndex&               mOverrideSpans;
        const TransformSnapshot&               mBase;
        const TransformSnapshot&               mLoaded;
        DirtyTracker&                          mDirty;
        std::unordered_set<std::string>&       mScaleFromAnchorSet;
    };
}

#endif
