//////////////////////////////////////////////////////////////////////
//
//  SaveEngine.h - the scene-save side of the Model-B CST cutover.
//
//  Since Slice 6c-3a every scene loads CST-only (Job retains a
//  canonical CST Document).  The controller snapshots that persistent
//  Document under its mutation mutex; Save() serializes the immutable
//  snapshot (Cst::SerializeCst — byte-exact on an unedited round-trip,
//  minimal-diff on edits) and writes it atomically.
//
//  An external-modification guard refuses an IN-PLACE save when the
//  loaded file changed on disk after load (metadata/file-id mismatch) —
//  overwriting it with the in-memory scene would silently clobber those
//  external edits (docs/ROUND_TRIP_SAVE_PLAN.md §11.6).  The controller
//  snapshots the CST-load FileIdentity alongside the Document.
//
//  (The legacy two-mode byte-splice save — Mode A in-place line rewrite
//  + Mode B managed override block, driven by SourceSpanIndex /
//  OverrideSpanIndex — was deleted in Model-B P5 Slice 6d: it only ran
//  for a non-CST Job, which no longer exists.)
//
//  Inputs are borrowed immutable references; outputs are a SaveResult
//  value.  Caller is responsible for capturing the snapshots and
//  publishing dirty/file-identity state after the unlocked IO.
//
//////////////////////////////////////////////////////////////////////

#ifndef SaveEngine_
#define SaveEngine_

#include <memory>
#include <string>

namespace RISE
{
    struct FileIdentity;
    struct SaveLease;
    namespace Cst { struct Document; }

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

    /// CST-Document save engine.  Serializes an immutable Document
    /// snapshot; refuses an in-place save when the loaded file changed
    /// externally.
    ///
    /// Concurrency: each engine reads only immutable snapshots and takes a
    /// process-wide per-canonical-path lease around identity validation
    /// through atomic publication.  Different target files remain
    /// independent; overlapping saves to one target cannot both succeed.
    class SaveEngine
    {
    public:
        /// Borrowed immutable snapshots.  Lifetime: caller guarantees
        /// they outlive the SaveEngine.  SaveEngine deliberately has no
        /// access to the live Job or editor dirty state: the controller
        /// captures these under its mutation mutex, performs file IO
        /// unlocked, then publishes save/dirty state under the mutex.
        SaveEngine(
            const Cst::Document& document,
            const FileIdentity& loadedFileIdentity,
            const std::string& filePath );
        ~SaveEngine();

        SaveResult Save();

    private:
        const Cst::Document& mDocument;
        const FileIdentity&  mLoadedFileIdentity;
        std::string          mFilePath;
        std::unique_ptr<SaveLease> mLease;
    };
}

#endif
