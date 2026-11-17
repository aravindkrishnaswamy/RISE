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
        long long    mtimeSec  = 0;    // POSIX stat.st_mtime; Windows: FILETIME seconds (1601 epoch)
        long long    mtimeNsec = 0;    // POSIX stat.st_mtim.tv_nsec; Windows: FILETIME sub-second (100ns units as ns)
        long long    sizeBytes = 0;    // file size in bytes
        long long    deviceId  = 0;    // POSIX stat.st_dev; Windows: volume serial number
        long long    fileId    = 0;    // POSIX stat.st_ino; Windows: 64-bit NTFS file index (replacement detection)
        bool         captured  = false;
    };

    /// Capture `path`'s on-disk identity (captured == false on any failure).
    /// The ONLY sanctioned way to fill a FileIdentity: both capture sites
    /// (load baseline, save-time comparison) must go through one
    /// implementation or the fields silently diverge in meaning.  On POSIX
    /// this is stat() (st_dev / st_ino + nanosecond mtime).  On Windows the
    /// CRT stat is unusable for replacement detection -- st_ino is
    /// documented as always 0 and st_dev is a drive ordinal -- so the
    /// identity comes from GetFileInformationByHandle (volume serial +
    /// 64-bit file index) with the 100ns last-write time (CRT stat
    /// truncates mtime to whole seconds).  The two platforms' field
    /// encodings differ, which is fine: the guard only ever compares two
    /// identities captured by this same function on the same machine.
    /// Defined in SaveEngine.cpp (its sole library consumer besides
    /// Job::RefreshCstLoadFileIdentity; no dedicated .cpp TU).
    FileIdentity CaptureFileIdentity( const char* path );
}

#endif
