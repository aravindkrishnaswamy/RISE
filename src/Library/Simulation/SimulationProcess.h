// Simulation M5: the few process and file-lock primitives the bake manager
// and the worker share (docs/SIMULATION_SOLVER_CONTRACT.md).  POSIX is the
// implemented and tested path; the Windows branches (CreateProcessW,
// LockFileEx, GetProcessTimes, TerminateProcess) are written to the same
// contract but are NOT compiled or tested here.
//
// Nothing here is solver- or cache-specific.
#ifndef RISE_SIMULATION_SIMULATION_PROCESS_H
#define RISE_SIMULATION_SIMULATION_PROCESS_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

//! The running executable (absolute), or empty when the platform cannot say.
std::filesystem::path CurrentExecutablePath();
std::uint64_t CurrentProcessId();
//! This machine's name ("" when unknown).
std::string CurrentHostName();

//! An opaque, per-boot-stable stamp of when process `pid` started (macOS:
//! proc_pidinfo start time in microseconds; Linux: /proc/<pid>/stat start
//! ticks; Windows: creation FILETIME).  0 when the process does not exist or
//! the platform cannot say.  A pid together with this stamp names exactly
//! one process: a recycled pid has another stamp.
std::uint64_t ProcessStartStamp(std::uint64_t pid);

//! Starts `argv` (argv[0] is the executable path) detached from this process
//! (its own session on POSIX, so a terminal's ^C or this process exiting does
//! not stop it), stdin from the null device, stdout and stderr appended to
//! `logFile` (created 0600).  No other descriptor is inherited where the
//! platform allows it (macOS: POSIX_SPAWN_CLOEXEC_DEFAULT).
bool SpawnDetachedProcess(const std::vector<std::string>& argv, const std::filesystem::path& logFile,
	std::uint64_t& pid, std::string& error);

//! For a child of this process: true once it has exited (reaped here), with
//! `exitCode` its exit status, or 128 + signal when a signal ended it.  False
//! while it runs, and false with `notChild` set when `pid` is not (or no
//! longer) a child of this process.
bool ReapChildProcess(std::uint64_t pid, int& exitCode, bool& notChild);

//! True when a process `pid` exists whose start stamp is `startStamp` (any
//! stamp when `startStamp` is 0).  A zombie child counts as gone.
bool ProcessAlive(std::uint64_t pid, std::uint64_t startStamp);

//! Asks process `pid` (verified by `startStamp`) to stop at a safe point:
//! SIGINT on POSIX.  Windows has no signal to send: callers use the stop
//! file (rise-sim honours it on every platform).  False when the process is
//! not the one named.
bool InterruptProcess(std::uint64_t pid, std::uint64_t startStamp, std::string& error);
//! Ends it at once (SIGKILL / TerminateProcess): no checkpoint is written.
bool KillProcess(std::uint64_t pid, std::uint64_t startStamp, std::string& error);

//! An exclusive advisory lock on a file (flock / LockFileEx), released when
//! the object is destroyed or the process exits.  The file is created 0600
//! with missing parent directories 0700; a symlink is refused.
class AdvisoryFileLock {
public:
	AdvisoryFileLock();
	~AdvisoryFileLock();
	AdvisoryFileLock(const AdvisoryFileLock&) = delete;
	AdvisoryFileLock& operator=(const AdvisoryFileLock&) = delete;
	//! True when this object now holds the lock; false with `busy` set when
	//! another holder has it, or false with `error` on any other failure.
	bool TryLock(const std::filesystem::path& file, bool& busy, std::string& error);
	bool Held() const;
	void Unlock();
	//! While held: replaces the file's content (the holder records its pid).
	bool WriteContent(const std::string& text, std::string& error);
private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
//! The text a holder recorded in a lock file (empty when unreadable).
std::string ReadLockFileContent(const std::filesystem::path& file);

//! Creates every missing directory of `directory` owner-only (0700).
bool CreatePrivateDirectories(const std::filesystem::path& directory, std::string& error);

//! Per-user directories: the default GPU lock file shared by every RISE
//! process of this user (`RISE_SIM_GPU_LOCK` overrides it) and the bake
//! manager's state directory (job logs).
//!   macOS   ~/Library/Caches/RISE/rise-sim.gpu.lock, ~/Library/Caches/RISE/bakes
//!   Linux   $XDG_CACHE_HOME (or ~/.cache)/rise/...
//!   Windows %LOCALAPPDATA%\RISE\...
std::filesystem::path DefaultGpuLockPath();
std::filesystem::path DefaultBakeStateDirectory();

//! The default simulation cache location (docs/SIMULATION_SOLVER_CONTRACT.md,
//! "Cache location policy"): used when a scene's simulation chunk names no
//! `cache`.  It is disposable application data in the per-user cache folder,
//! next to the two directories above:
//!   `<base>/simulation`, base as above, or `$RISE_SIMULATION_CACHE_DIR` (an
//!   absolute path; relative values are ignored) when set -- for a machine
//!   whose home volume is small.  Without a usable home directory (Android),
//!   the system temporary directory.
std::filesystem::path DefaultSimulationCacheDirectory();
//! The cache directory of the simulation `name` of the scene file `sceneFile`:
//!   `<DefaultSimulationCacheDirectory()>/<scene-stem>-<16 hex of SHA-256 of
//!   the scene file's canonical absolute path>/<name>`
//! Every process (the in-process host, `rise-sim`, the bake manager, the GUI,
//! render-time playback) resolves the default through this function, so they
//! agree.  Two scenes never share a directory; the same scene finds its
//! cache again.  Empty (and `error` set) when `name` is not a single path
//! component.
std::filesystem::path DefaultSimulationCachePath(const std::filesystem::path& sceneFile, const std::string& name,
	std::string& error);

//! The solver's stated backend needs the machine's one GPU slot: stated,
//! and not "cpu".  (Capabilities::backend, contract Appendix C.)
bool IsGpuBackend(const std::string& backend);

} }
#endif
