//////////////////////////////////////////////////////////////////////
//
//  SaveEngine.cpp - the scene-save side of the Model-B CST cutover.
//
//  Serializes an immutable snapshot of the Job's retained CST Document
//  (the complete source of truth since Slice 6c-3a routed every GUI edit
//  into it) and writes it atomically.  An external-modification guard
//  refuses an IN-PLACE save when the loaded file changed on disk after
//  load.
//
//  See docs/ROUND_TRIP_SAVE_PLAN.md §9 (algorithm history) and §11.6
//  (the external-modification guard's rationale).
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SaveEngine.h"
#include "../Cst/Cst.h"   // P5 Slice 4: SerializeCst the retained CST Document

#include "FileIdentity.h"

#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <sys/stat.h>

// POSIX-only headers + helpers used by the optional fsync hardening
// path in AtomicWrite.  On Windows we fall back to std::ofstream +
// std::filesystem::rename without the fsync step — the rename is
// still atomic on NTFS via ReplaceFile semantics (std::filesystem
// uses MoveFileEx with MOVEFILE_REPLACE_EXISTING under the hood).
// `<process.h>` is the Windows header that declares `_getpid`; on
// POSIX `getpid` is in `<unistd.h>`.
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace RISE
{

namespace {

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

// Atomic write: tmp + (POSIX-only fsync) + rename + (POSIX-only
// dir-fsync) (§9.8).  R-final P1 #1 fix: portable across POSIX and
// Windows.  Core flow uses std::ofstream + std::filesystem::rename —
// both atomic on supported filesystems (POSIX rename is atomic on
// same-FS; std::filesystem::rename calls MoveFileEx with
// MOVEFILE_REPLACE_EXISTING on Windows, also atomic on NTFS).  The
// fsync hardening (which protects against power-loss between rename
// and journal-flush) is POSIX-only; on Windows we rely on NTFS
// journaling and skip the explicit flush.
bool AtomicWrite( const std::string& filePath, const std::string& bytes,
                  std::string& outError )
{
    // Build a per-process / per-invocation tmp path.  pid + epoch
    // seconds is unique enough for single-threaded save sequences;
    // the rename is what enforces atomicity, not the tmp uniqueness.
#if defined(_WIN32)
    const long long pidValue = static_cast<long long>( ::_getpid() );
#else
    const long long pidValue = static_cast<long long>( ::getpid() );
#endif
    const std::string tmpPath = filePath + ".tmp." +
        std::to_string( pidValue ) + "." +
        std::to_string( static_cast<long long>( std::time( nullptr ) ) );

    // Write bytes to tmp via std::ofstream (binary mode, no text
    // translation — line endings are managed explicitly upstream).
    {
        std::ofstream ofs( tmpPath.c_str(),
                           std::ios::out | std::ios::binary | std::ios::trunc );
        if( !ofs.is_open() ) {
            outError = "could not open temp file for write: " + tmpPath;
            return false;
        }
        ofs.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
        if( !ofs.good() ) {
            outError = "write to temp file failed: " + tmpPath;
            ofs.close();
            std::error_code rmEc;
            std::filesystem::remove( tmpPath, rmEc );
            return false;
        }
        ofs.flush();
        ofs.close();
        // POSIX-only: fsync the tmp file so the bytes hit storage
        // before the rename.  On Windows NTFS journaling does this
        // implicitly; std::ofstream::close already flushed the
        // userspace buffer.
#if !defined(_WIN32)
        int fd = ::open( tmpPath.c_str(), O_RDONLY );
        if( fd >= 0 ) {
            if( ::fsync( fd ) != 0 ) {
                outError = std::string( "fsync(tmp) failed: " ) + std::strerror( errno );
                ::close( fd );
                std::error_code rmEc;
                std::filesystem::remove( tmpPath, rmEc );
                return false;
            }
            ::close( fd );
        }
#endif
    }

    // Atomic rename — same FS guarantees on POSIX; MoveFileEx with
    // MOVEFILE_REPLACE_EXISTING on Windows (also atomic on NTFS).
    std::error_code renameEc;
    std::filesystem::rename( tmpPath, filePath, renameEc );
    if( renameEc ) {
        outError = std::string( "rename failed: " ) + renameEc.message();
        std::error_code rmEc;
        std::filesystem::remove( tmpPath, rmEc );
        return false;
    }

    // Best-effort directory fsync to harden the rename against power
    // loss.  POSIX-only; failure here is not fatal (file IS already
    // in place).  Windows skips: NTFS journaling covers it.
#if !defined(_WIN32)
    const std::size_t slash = filePath.find_last_of( "/\\" );
    const std::string dirPath = slash != std::string::npos
        ? filePath.substr( 0, slash )
        : std::string( "." );
    int dirfd = ::open( dirPath.c_str(), O_RDONLY );
    if( dirfd >= 0 ) {
        ::fsync( dirfd );
        ::close( dirfd );
    }
#endif
    return true;
}

// Read file bytes into a string.
bool ReadFile( const std::string& filePath, std::string& outBytes,
               std::string& outError )
{
    std::ifstream in( filePath.c_str(), std::ios::in | std::ios::binary );
    if( !in.is_open() ) {
        outError = "could not open file for reading";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    outBytes = ss.str();
    return true;
}

}  // anonymous namespace

// ----------------------------------------------------------------------
// SaveEngine
// ----------------------------------------------------------------------

SaveEngine::SaveEngine(
    const Cst::Document& document,
    const FileIdentity& loadedFileIdentity )
    : mDocument( document )
    , mLoadedFileIdentity( loadedFileIdentity )
{
}

SaveResult SaveEngine::Save( const std::string& filePath )
{
    SaveResult result;
    result.filePath = filePath;

    // ---- CST-Document save (the Model-B cutover's save side) -----------
    // The controller captured this immutable persistent-Document snapshot
    // under its mutation mutex.  File IO can therefore run unlocked without
    // borrowing the Job's replaceable pCstDocument or touching dirty state.
    // A concurrent edit lands on a newer head; RequestSave's publish step
    // detects that version change and leaves the editor dirty.

    // External-modification guard, IN-PLACE save only: if the loaded file
    // was edited on disk after load, an in-place re-serialize would clobber
    // those external edits with the in-memory CST -- refuse (§11.6).  A
    // Save-As writes a DIFFERENT file from the in-memory doc, so the loaded
    // file's on-disk state is irrelevant; only guard when writing back to
    // the loaded path.  The FileIdentity is captured at CST-load time
    // (Job::RefreshCstLoadFileIdentity) and read via GetCstLoadFileIdentity.
    {
        const FileIdentity& cstLoadIdent = mLoadedFileIdentity;
        if( cstLoadIdent.captured && cstLoadIdent.filePath == filePath ) {
            struct ::stat cur = {};
            if( ::stat( filePath.c_str(), &cur ) == 0 ) {
                const long long curSize  = static_cast<long long>( cur.st_size );
                const long long curMtime = static_cast<long long>( cur.st_mtime );
                long long curMtimeNsec = 0;
#if defined(__APPLE__)
                curMtimeNsec = static_cast<long long>( cur.st_mtimespec.tv_nsec );
#elif defined(__linux__) || defined(__unix__)
                curMtimeNsec = static_cast<long long>( cur.st_mtim.tv_nsec );
#endif
                if( curSize != cstLoadIdent.sizeBytes || curMtime != cstLoadIdent.mtimeSec
                    || curMtimeNsec != cstLoadIdent.mtimeNsec ) {
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage =
                        "scene file '" + filePath + "' was modified externally between load and save "
                        "(mtime/size mismatch).  Saving would overwrite those external changes with the "
                        "in-memory scene.  Reload the file before saving.";
                    return result;
                }
            }
        }
    }

    const std::string text = RISE::Cst::SerializeCst( mDocument );

    // NoOp when the target already holds exactly these bytes (e.g. a save
    // with no pending edits).
    std::string existing, rerr;
    if( ReadFile( filePath, existing, rerr ) && existing == text ) {
        result.status = SaveResult::Status::NoOp;
        return result;
    }

    std::string werr;
    if( !AtomicWrite( filePath, text, werr ) ) {
        result.status = SaveResult::Status::Failed;
        result.errorMessage = werr;
        return result;
    }

    result.status = SaveResult::Status::Saved;
    return result;
}

}  // namespace RISE
