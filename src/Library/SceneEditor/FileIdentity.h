//////////////////////////////////////////////////////////////////////
//
//  FileIdentity.h - file-identity fingerprint captured at scene-load.
//
//  Relocated (Model-B P5 Slice 6d) from SourceSpanIndex.h so the
//  surviving CST-save external-modification guard no longer depends on
//  the (now-deleted) legacy byte-splice span indices.  The Job caches a
//  FileIdentity at CST-load time (mCstLoadFileIdentity /
//  RefreshCstLoadFileIdentity); the SaveEngine CST-save path compares
//  it against the current on-disk identity to refuse an in-place save
//  when the file was modified or atomically replaced between load and save.
//
//  See docs/ROUND_TRIP_SAVE_PLAN.md §11.6 for the guard rationale.
//
//////////////////////////////////////////////////////////////////////

#ifndef FileIdentity_
#define FileIdentity_

#include <string>

namespace RISE
{
    /// File-identity fingerprint captured at scene-load time.
    /// Save-time metadata or device/file-id mismatch indicates the file was modified
    /// externally between load and save — an in-place re-serialize
    /// would clobber those external changes.
    struct FileIdentity
    {
        std::string  filePath;
        long long    mtimeSec  = 0;    // POSIX stat.st_mtime
        long long    mtimeNsec = 0;    // POSIX stat.st_mtim.tv_nsec (or 0 if unavailable)
        long long    sizeBytes = 0;    // POSIX stat.st_size
        long long    deviceId  = 0;    // POSIX stat.st_dev (0 where unavailable)
        long long    fileId    = 0;    // POSIX stat.st_ino (replacement detection)
        bool         captured  = false;
    };
}

#endif
