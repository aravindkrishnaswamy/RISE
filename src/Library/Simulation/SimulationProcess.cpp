#include "pch.h"
#include "SimulationProcess.h"
#include "../Utilities/RISECBOR64.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
// NOT COMPILED OR TESTED in this tree (docs/SIMULATION_SOLVER_CONTRACT.md, "Windows").
#include <windows.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/proc_info.h>
#endif
extern char** environ;
#endif

namespace RISE { namespace Simulation {
namespace fs = std::filesystem;

namespace {
bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

fs::path HomeDirectory()
{
#ifdef _WIN32
	if (const char* p = std::getenv("USERPROFILE")) return fs::path(p);
	return fs::path();
#else
	if (const char* h = std::getenv("HOME")) if (*h) return fs::path(h);
	if (const passwd* pw = ::getpwuid(::getuid())) if (pw->pw_dir) return fs::path(pw->pw_dir);
	return fs::path();
#endif
}

fs::path PerUserRoot()
{
#ifdef _WIN32
	if (const char* p = std::getenv("LOCALAPPDATA")) if (*p) return fs::path(p) / "RISE";
	return HomeDirectory() / "AppData" / "Local" / "RISE";
#elif defined(__APPLE__)
	return HomeDirectory() / "Library" / "Caches" / "RISE";
#else
	if (const char* x = std::getenv("XDG_CACHE_HOME")) if (*x == '/') return fs::path(x) / "rise";
	return HomeDirectory() / ".cache" / "rise";
#endif
}

// Missing directories are created owner-only.
bool MakePrivateDirectories(const fs::path& directory, std::string& error)
{
	std::error_code ec;
	if (fs::is_directory(directory, ec)) return true;
	fs::path existing = directory;
	std::vector<fs::path> missing;
	while (!existing.empty() && !fs::exists(existing, ec)) {
		missing.push_back(existing);
		if (existing == existing.parent_path()) break;
		existing = existing.parent_path();
	}
	for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
		fs::create_directory(*it, ec);
		if (ec && !fs::is_directory(*it)) return Fail(error, "cannot create " + it->string() + ": " + ec.message());
		fs::permissions(*it, fs::perms::owner_all, fs::perm_options::replace, ec);
	}
	return fs::is_directory(directory, ec) || Fail(error, "not a directory: " + directory.string());
}
} // namespace

bool CreatePrivateDirectories(const fs::path& directory, std::string& error)
{
	return MakePrivateDirectories(directory, error);
}

bool IsGpuBackend(const std::string& backend)
{
	return !backend.empty() && backend != "cpu";
}

fs::path DefaultGpuLockPath()
{
	if (const char* p = std::getenv("RISE_SIM_GPU_LOCK")) if (*p) return fs::path(p);
	return PerUserRoot() / "rise-sim.gpu.lock";
}

fs::path DefaultBakeStateDirectory()
{
	return PerUserRoot() / "bakes";
}

fs::path DefaultSimulationCacheDirectory()
{
	fs::path base;
	if (const char* o = std::getenv("RISE_SIMULATION_CACHE_DIR")) if (*o && fs::path(o).is_absolute()) base = fs::path(o);
	if (base.empty()) base = PerUserRoot();
	if (!base.is_absolute()) {   // no home directory (Android): the system temporary directory
		std::error_code ec;
		const fs::path temp = fs::temp_directory_path(ec);
		base = (ec ? fs::path("/tmp") : temp) / "RISE";
	}
	return base / "scene-caches";
}

fs::path DefaultSimulationCachePath(const fs::path& sceneFile, const std::string& name, std::string& error)
{
	if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) {
		error = "the simulation name `" + name + "` cannot name a default cache directory (use `cache` in the scene)";
		return fs::path();
	}
	std::error_code ec;
	fs::path absolute = fs::absolute(sceneFile, ec);
	if (ec) { error = "cannot resolve the scene file path " + sceneFile.string(); return fs::path(); }
	fs::path canonical = fs::weakly_canonical(absolute, ec);
	if (ec) canonical = absolute.lexically_normal();
	const std::string text = canonical.generic_string();
	const std::string digest = RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char*>(text.data()), text.size());
	std::string stem = sceneFile.stem().string();
	for (char& c : stem)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) c = '_';
	if (stem.size() > 48) stem.resize(48);
	if (stem.empty() || stem == "." || stem == "..") stem = "scene";
	return DefaultSimulationCacheDirectory() / (stem + "-" + digest.substr(0, 16)) / name;
}

std::string ReadLockFileContent(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	std::string text;
	if (in) std::getline(in, text);
	return text;
}

#ifndef _WIN32
// ------------------------------------------------------------------ POSIX

fs::path CurrentExecutablePath()
{
	std::error_code ec;
#ifdef __APPLE__
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buffer(size + 1, '\0');
	if (_NSGetExecutablePath(buffer.data(), &size) != 0) return fs::path();
	buffer.resize(std::strlen(buffer.c_str()));
	const fs::path p = fs::weakly_canonical(fs::path(buffer), ec);
	return ec ? fs::path(buffer) : p;
#else
	const fs::path p = fs::read_symlink("/proc/self/exe", ec);
	return ec ? fs::path() : p;
#endif
}

std::uint64_t CurrentProcessId() { return static_cast<std::uint64_t>(::getpid()); }

std::string CurrentHostName()
{
	char name[256] = {};
	if (::gethostname(name, sizeof(name) - 1) != 0) return std::string();
	return std::string(name);
}

namespace {
// Start stamp and whether the process is a zombie.
bool ProcessInfo(std::uint64_t pid, std::uint64_t& stamp, bool& zombie)
{
	stamp = 0;
	zombie = false;
	if (pid == 0 || pid > static_cast<std::uint64_t>(0x7fffffff)) return false;
#ifdef __APPLE__
	proc_bsdinfo info;
	std::memset(&info, 0, sizeof(info));
	const int got = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, PROC_PIDTBSDINFO_SIZE);
	if (got != PROC_PIDTBSDINFO_SIZE) return false;
	stamp = static_cast<std::uint64_t>(info.pbi_start_tvsec) * 1000000u + static_cast<std::uint64_t>(info.pbi_start_tvusec);
	zombie = info.pbi_status == 5;   // SZOMB
	return true;
#else
	std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
	std::string text;
	if (!in || !std::getline(in, text)) return false;
	const auto close = text.rfind(')');
	if (close == std::string::npos) return false;
	std::istringstream fields(text.substr(close + 1));
	std::string state;
	fields >> state;   // field 3
	zombie = state == "Z" || state == "X";
	std::string field;
	for (int i = 4; i <= 22 && (fields >> field); ++i)
		if (i == 22) stamp = std::strtoull(field.c_str(), nullptr, 10);
	return stamp != 0;
#endif
}
} // namespace

std::uint64_t ProcessStartStamp(std::uint64_t pid)
{
	std::uint64_t stamp = 0;
	bool zombie = false;
	return ProcessInfo(pid, stamp, zombie) ? stamp : 0;
}

bool ProcessAlive(std::uint64_t pid, std::uint64_t startStamp)
{
	std::uint64_t stamp = 0;
	bool zombie = false;
	if (!ProcessInfo(pid, stamp, zombie) || zombie) return false;
	return startStamp == 0 || stamp == startStamp;
}

bool SpawnDetachedProcess(const std::vector<std::string>& argv, const fs::path& logFile, std::uint64_t& pid,
	std::string& error)
{
	if (argv.empty()) return Fail(error, "no worker command");
	std::vector<char*> args;
	for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
	args.push_back(nullptr);
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	if (posix_spawn_file_actions_init(&actions) != 0) return Fail(error, "posix_spawn_file_actions_init failed");
	if (posix_spawnattr_init(&attr) != 0) {
		posix_spawn_file_actions_destroy(&actions);
		return Fail(error, "posix_spawnattr_init failed");
	}
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 1, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
	posix_spawn_file_actions_adddup2(&actions, 1, 2);
	short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
	flags |= POSIX_SPAWN_SETSID;
#else
	flags |= POSIX_SPAWN_SETPGROUP;
	posix_spawnattr_setpgroup(&attr, 0);
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
	flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
	posix_spawnattr_setflags(&attr, flags);
	sigset_t none, defaults;
	sigemptyset(&none);
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGINT);
	sigaddset(&defaults, SIGTERM);
	sigaddset(&defaults, SIGPIPE);
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setsigdefault(&attr, &defaults);
	pid_t child = 0;
	const int rc = posix_spawn(&child, args[0], &actions, &attr, args.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	if (rc != 0) return Fail(error, "cannot start " + argv[0] + ": " + std::strerror(rc));
	pid = static_cast<std::uint64_t>(child);
	return true;
}

bool ReapChildProcess(std::uint64_t pid, int& exitCode, bool& notChild)
{
	notChild = false;
	if (pid == 0) { notChild = true; return false; }
	int status = 0;
	pid_t rc;
	do { rc = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG); } while (rc < 0 && errno == EINTR);
	if (rc == 0) return false;
	if (rc < 0) { notChild = true; return false; }
	exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
	return true;
}

namespace {
bool SignalProcess(std::uint64_t pid, std::uint64_t startStamp, int signal, std::string& error)
{
	if (!ProcessAlive(pid, startStamp)) return Fail(error, "process " + std::to_string(pid) + " is not the worker it was");
	if (::kill(static_cast<pid_t>(pid), signal) != 0) return Fail(error, std::string("kill: ") + std::strerror(errno));
	return true;
}
} // namespace

bool InterruptProcess(std::uint64_t pid, std::uint64_t startStamp, std::string& error)
{
	return SignalProcess(pid, startStamp, SIGINT, error);
}

bool KillProcess(std::uint64_t pid, std::uint64_t startStamp, std::string& error)
{
	return SignalProcess(pid, startStamp, SIGKILL, error);
}

struct AdvisoryFileLock::Impl { int fd = -1; };

AdvisoryFileLock::AdvisoryFileLock() : impl_(new Impl) {}
AdvisoryFileLock::~AdvisoryFileLock() { Unlock(); }
bool AdvisoryFileLock::Held() const { return impl_->fd >= 0; }

void AdvisoryFileLock::Unlock()
{
	if (impl_->fd >= 0) {
		::flock(impl_->fd, LOCK_UN);
		::close(impl_->fd);
		impl_->fd = -1;
	}
}

bool AdvisoryFileLock::TryLock(const fs::path& file, bool& busy, std::string& error)
{
	busy = false;
	if (Held()) return true;
	if (!MakePrivateDirectories(file.parent_path(), error)) return false;
	const int fd = ::open(file.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) return Fail(error, "cannot open lock " + file.string() + ": " + std::strerror(errno));
	int rc;
	do { rc = ::flock(fd, LOCK_EX | LOCK_NB); } while (rc != 0 && errno == EINTR);
	if (rc != 0) {
		const int why = errno;
		::close(fd);
		if (why == EWOULDBLOCK) { busy = true; return false; }
		return Fail(error, "cannot lock " + file.string() + ": " + std::strerror(why));
	}
	impl_->fd = fd;
	return true;
}

bool AdvisoryFileLock::WriteContent(const std::string& text, std::string& error)
{
	if (!Held()) return Fail(error, "lock not held");
	if (::ftruncate(impl_->fd, 0) != 0 || ::pwrite(impl_->fd, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size()))
		return Fail(error, std::string("cannot write lock file: ") + std::strerror(errno));
	return true;
}

#else
// ------------------------------------------------------------------ Windows
// NOT COMPILED OR TESTED here.  Same contract as the POSIX branch: stop is the
// stop file (rise-sim.stop), there is no SIGINT; KillProcess terminates.

fs::path CurrentExecutablePath()
{
	std::wstring buffer(32768, L'\0');
	const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (n == 0 || n >= buffer.size()) return fs::path();
	buffer.resize(n);
	return fs::path(buffer);
}

std::uint64_t CurrentProcessId() { return static_cast<std::uint64_t>(GetCurrentProcessId()); }

std::string CurrentHostName()
{
	char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
	DWORD size = sizeof(name);
	return GetComputerNameA(name, &size) ? std::string(name, size) : std::string();
}

std::uint64_t ProcessStartStamp(std::uint64_t pid)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (!h) return 0;
	FILETIME created, exited, kernel, user;
	std::uint64_t stamp = 0;
	DWORD code = 0;
	if (GetProcessTimes(h, &created, &exited, &kernel, &user) && GetExitCodeProcess(h, &code) && code == STILL_ACTIVE)
		stamp = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
	CloseHandle(h);
	return stamp;
}

bool ProcessAlive(std::uint64_t pid, std::uint64_t startStamp)
{
	const std::uint64_t stamp = ProcessStartStamp(pid);
	return stamp != 0 && (startStamp == 0 || stamp == startStamp);
}

bool SpawnDetachedProcess(const std::vector<std::string>& argv, const fs::path& logFile, std::uint64_t& pid,
	std::string& error)
{
	if (argv.empty()) return Fail(error, "no worker command");
	std::wstring command;
	for (const auto& a : argv) {
		if (!command.empty()) command += L' ';
		command += L'"' + fs::path(a).wstring() + L'"';
	}
	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE log = CreateFileW(logFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	if (log == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE) {
		if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
		if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
		return Fail(error, "cannot open the worker log");
	}
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nul;
	si.hStdOutput = log;
	si.hStdError = log;
	PROCESS_INFORMATION pi{};
	const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
		CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(log);
	CloseHandle(nul);
	if (!ok) return Fail(error, "CreateProcess failed (" + std::to_string(GetLastError()) + ")");
	pid = pi.dwProcessId;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

bool ReapChildProcess(std::uint64_t pid, int& exitCode, bool& notChild)
{
	notChild = false;
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (!h) { notChild = true; return false; }
	DWORD code = 0;
	const bool done = GetExitCodeProcess(h, &code) && code != STILL_ACTIVE;
	CloseHandle(h);
	if (done) exitCode = static_cast<int>(code);
	return done;
}

bool InterruptProcess(std::uint64_t, std::uint64_t, std::string& error)
{
	return Fail(error, "no signal on Windows: use the stop file");
}

bool KillProcess(std::uint64_t pid, std::uint64_t startStamp, std::string& error)
{
	if (!ProcessAlive(pid, startStamp)) return Fail(error, "process is not the worker it was");
	HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
	if (!h) return Fail(error, "cannot open the worker process");
	const BOOL ok = TerminateProcess(h, 137);
	CloseHandle(h);
	return ok ? true : Fail(error, "TerminateProcess failed");
}

struct AdvisoryFileLock::Impl { HANDLE h = INVALID_HANDLE_VALUE; };

AdvisoryFileLock::AdvisoryFileLock() : impl_(new Impl) {}
AdvisoryFileLock::~AdvisoryFileLock() { Unlock(); }
bool AdvisoryFileLock::Held() const { return impl_->h != INVALID_HANDLE_VALUE; }

void AdvisoryFileLock::Unlock()
{
	if (Held()) {
		OVERLAPPED range{};
		UnlockFileEx(impl_->h, 0, 1, 0, &range);
		CloseHandle(impl_->h);
		impl_->h = INVALID_HANDLE_VALUE;
	}
}

bool AdvisoryFileLock::TryLock(const fs::path& file, bool& busy, std::string& error)
{
	busy = false;
	if (Held()) return true;
	if (!MakePrivateDirectories(file.parent_path(), error)) return false;
	HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return Fail(error, "cannot open lock " + file.string());
	OVERLAPPED range{};
	if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range)) {
		const DWORD why = GetLastError();
		CloseHandle(h);
		if (why == ERROR_LOCK_VIOLATION) { busy = true; return false; }
		return Fail(error, "cannot lock " + file.string());
	}
	impl_->h = h;
	return true;
}

bool AdvisoryFileLock::WriteContent(const std::string& text, std::string& error)
{
	if (!Held()) return Fail(error, "lock not held");
	// The locked byte range is byte 0; the text goes after it is unlocked
	// for readers on Windows only through FILE_SHARE_READ.
	SetFilePointer(impl_->h, 0, nullptr, FILE_BEGIN);
	SetEndOfFile(impl_->h);
	DWORD written = 0;
	return WriteFile(impl_->h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size() ?
		true : Fail(error, "cannot write lock file");
}
#endif

} }
