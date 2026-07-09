//////////////////////////////////////////////////////////////////////
//
//  SaveEngine.h - the scene-save side of the Model-B CST cutover.
//
//  Since Slice 6c-3a every scene loads CST-only (Job retains a
//  canonical CST Document), so Save() SERIALIZEs that retained Document
//  directly (Cst::SerializeCst — byte-exact on an unedited round-trip,
//  minimal-diff on edits) and writes it atomically.
//
//  An external-modification guard refuses an IN-PLACE save when the
//  loaded file changed on disk after load (mtime/size mismatch) —
//  overwriting it with the in-memory scene would silently clobber those
//  external edits (docs/ROUND_TRIP_SAVE_PLAN.md §11.6).  The guard reads
//  the CST-load FileIdentity via IJobPriv::GetCstLoadFileIdentity().
//
//  (The legacy two-mode byte-splice save — Mode A in-place line rewrite
//  + Mode B managed override block, driven by SourceSpanIndex /
//  OverrideSpanIndex — was deleted in Model-B P5 Slice 6d: it only ran
//  for a non-CST Job, which no longer exists.)
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
    };

    /// True iff the status indicates a successful (file-state-coherent)
    /// outcome.  Saved and NoOp both qualify; Refused and Failed do not.
    inline bool Succeeded( SaveResult::Status s )
    {
        return s == SaveResult::Status::Saved
            || s == SaveResult::Status::NoOp;
    }

    /// CST-Document save engine.  Serializes the Job's retained CST
    /// Document; refuses an in-place save when the loaded file changed
    /// externally.
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
            DirtyTracker&                          dirty,
            std::unordered_set<std::string>&       scaleFromAnchorSet );

        SaveResult Save( const std::string& filePath );

    private:
        IJobPriv&                              mJob;
        DirtyTracker&                          mDirty;
        std::unordered_set<std::string>&       mScaleFromAnchorSet;
    };
}

#endif
