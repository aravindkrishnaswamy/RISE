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

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <sys/stat.h>
#include <unordered_map>

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

// One in-process save lease per canonical target path.  The operation mutex
// covers external-identity validation through the atomic rename, closing the
// check-then-write race between two independently loaded controllers.  The
// owning SaveEngine reserves this state before its pre-IO seam; a competing
// engine's try-lock is therefore an immediate, deterministic refusal.
struct PathSaveState
{
    std::mutex operationMutex;
};

std::string CanonicalSavePath( const std::string& filePath )
{
    std::error_code ec;
    std::filesystem::path path =
        std::filesystem::weakly_canonical( std::filesystem::path( filePath ), ec );
    if( ec ) {
        ec.clear();
        path = std::filesystem::absolute( std::filesystem::path( filePath ), ec );
        if( ec ) path = std::filesystem::path( filePath );
        path = path.lexically_normal();
    }
    std::string key = path.string();
#if defined(_WIN32)
    // Windows path lookup is case-insensitive; make the in-process lease
    // key obey the same rule so spelling aliases cannot bypass it.
    for( char& c : key ) {
        if( c >= 'A' && c <= 'Z' ) c = static_cast<char>( c - 'A' + 'a' );
    }
#endif
    return key;
}

std::shared_ptr<PathSaveState> SaveStateForPath( const std::string& canonicalPath )
{
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<PathSaveState>> registry;

    std::lock_guard<std::mutex> lk( registryMutex );
    std::shared_ptr<PathSaveState> state = registry[canonicalPath].lock();
    if( !state ) {
        state.reset( new PathSaveState() );
        registry[canonicalPath] = state;
    }
    return state;
}

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
    // Build a per-process / per-invocation tmp path. The monotonic
    // counter prevents two controllers in the same process from sharing
    // a temp file even when they save the same target concurrently.
    static std::atomic<unsigned long long> nextTmpId{ 1 };
#if defined(_WIN32)
    const long long pidValue = static_cast<long long>( ::_getpid() );
#else
    const long long pidValue = static_cast<long long>( ::getpid() );
#endif
    const unsigned long long tmpId =
        nextTmpId.fetch_add( 1, std::memory_order_relaxed );
    const std::string tmpPath = filePath + ".tmp." +
        std::to_string( pidValue ) + "." +
        std::to_string( tmpId );

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

struct SaveLease
{
    explicit SaveLease( const std::string& canonicalPath )
        : state( SaveStateForPath( canonicalPath ) )
        , lock( state->operationMutex, std::try_to_lock )
    {
    }

    std::shared_ptr<PathSaveState> state;
    std::unique_lock<std::mutex>   lock;
};

SaveEngine::SaveEngine(
    const Cst::Document& document,
    const FileIdentity& loadedFileIdentity,
    const std::string& filePath )
    : mDocument( document )
    , mLoadedFileIdentity( loadedFileIdentity )
    , mFilePath( filePath )
    , mResolvedFilePath( CanonicalSavePath( filePath ) )
    , mLease( new SaveLease( mResolvedFilePath ) )
{
}

SaveEngine::~SaveEngine() = default;

SaveResult SaveEngine::Save()
{
    SaveResult result;
    result.filePath = mFilePath;

    // The lease is reserved in the constructor, before the controller's
    // pre-IO test hook and before this method starts.  A second controller
    // therefore cannot slip through the same path while the first is paused
    // anywhere between snapshot capture and atomic publication.
    if( !mLease || !mLease->lock.owns_lock() ) {
        result.status = SaveResult::Status::Refused;
        result.errorMessage =
            "save refused: another save to '" + mFilePath +
            "' is already in progress";
        return result;
    }
    const std::string& canonicalTarget = mResolvedFilePath;

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
        if( cstLoadIdent.captured
            && CanonicalSavePath( cstLoadIdent.filePath ) == canonicalTarget ) {
            struct ::stat cur = {};
            if( ::stat( mResolvedFilePath.c_str(), &cur ) == 0 ) {
                const long long curSize  = static_cast<long long>( cur.st_size );
                const long long curMtime = static_cast<long long>( cur.st_mtime );
                const long long curDevice = static_cast<long long>( cur.st_dev );
                const long long curFileId = static_cast<long long>( cur.st_ino );
                long long curMtimeNsec = 0;
#if defined(__APPLE__)
                curMtimeNsec = static_cast<long long>( cur.st_mtimespec.tv_nsec );
#elif defined(__linux__) || defined(__unix__)
                curMtimeNsec = static_cast<long long>( cur.st_mtim.tv_nsec );
#endif
                if( curSize != cstLoadIdent.sizeBytes || curMtime != cstLoadIdent.mtimeSec
                    || curMtimeNsec != cstLoadIdent.mtimeNsec
                    || curDevice != cstLoadIdent.deviceId
                    || curFileId != cstLoadIdent.fileId ) {
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage =
                        "scene file '" + mFilePath + "' was modified externally between load and save "
                        "(metadata/file-id mismatch).  Saving would overwrite those external changes with the "
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
    if( ReadFile( mResolvedFilePath, existing, rerr ) && existing == text ) {
        result.status = SaveResult::Status::NoOp;
        return result;
    }

    std::string werr;
    if( !AtomicWrite( mResolvedFilePath, text, werr ) ) {
        result.status = SaveResult::Status::Failed;
        result.errorMessage = werr;
        return result;
    }

    result.status = SaveResult::Status::Saved;
    return result;
}

}  // namespace RISE
