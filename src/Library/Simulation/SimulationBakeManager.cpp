#include "pch.h"
#include "SimulationBakeManager.h"
#include "SimulationCache.h"
#include "SimulationProcess.h"
#include "SimulationWorker.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

namespace RISE { namespace Simulation {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
std::atomic<std::uint64_t> g_nextJobId{1};
constexpr std::size_t kManifestBudget = std::size_t(64) << 20;
constexpr std::size_t kMaxTerminalJobs = 256;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

bool Live(BakeJobState s)
{
	return s == BakeJobState::WaitingForGpu || s == BakeJobState::Running || s == BakeJobState::Stopping;
}

bool IsExecutable(const fs::path& p)
{
	std::error_code ec;
	if (!fs::is_regular_file(p, ec)) return false;
#ifdef _WIN32
	return true;
#else
	return ::access(p.c_str(), X_OK) == 0;
#endif
}

std::uint64_t PhysicalMemory()
{
#ifdef _WIN32
	MEMORYSTATUSEX m;
	m.dwLength = sizeof(m);
	return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
#elif defined(__APPLE__)
	std::uint64_t bytes = 0;
	std::size_t size = sizeof(bytes);
	return ::sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
#else
	const long pages = ::sysconf(_SC_PHYS_PAGES), page = ::sysconf(_SC_PAGE_SIZE);
	return pages > 0 && page > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page) : 0;
#endif
}

std::string MiB(std::uint64_t bytes)
{
	return std::to_string((bytes + (1u << 20) - 1) >> 20) + " MiB";
}

// The last "rise-sim: ..." line of a worker log (its error, for a failed job).
std::string LastWorkerMessage(const fs::path& log)
{
	std::ifstream in(log, std::ios::binary);
	if (!in) return std::string();
	in.seekg(0, std::ios::end);
	const std::streamoff size = in.tellg();
	const std::streamoff from = size > 8192 ? size - 8192 : 0;
	in.seekg(from);
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::istringstream lines(text);
	std::string line, last, lastAny;
	while (std::getline(lines, line)) {
		if (line.empty()) continue;
		lastAny = line;
		if (line.rfind("rise-sim: ", 0) == 0 && line.rfind("rise-sim: note:", 0) != 0) last = line.substr(10);
	}
	return last.empty() ? lastAny : last;
}

const SceneSimulationStatus* Find(const BakeSceneSnapshot& scene, const std::string& name)
{
	for (const auto& s : scene.simulations) if (s.name == name) return &s;
	return nullptr;
}

// Not complete with the current key, or reading an upstream that is not.
bool OutOfDate(const SceneSimulationStatus& s)
{
	return s.state != SceneSimulationState::Complete || s.upstreamOutOfDate;
}

// The worker the status record names is alive and is the one baking this
// cache: same host, the pid's start stamp matches, the cache writer lock is held.
bool VerifiedLiveWorker(const fs::path& cache, const WorkerStatus& status)
{
	if (status.pid == 0 || status.pidStart == 0) return false;
	if (!status.host.empty() && status.host != CurrentHostName()) return false;
	if (!ProcessAlive(status.pid, status.pidStart)) return false;
	std::string ignored;
	return InspectCache(cache, kManifestBudget, ignored) == CacheState::Baking;
}
} // namespace

const char* BakeJobStateName(BakeJobState state)
{
	switch (state) {
	case BakeJobState::Queued: return "queued";
	case BakeJobState::WaitingForGpu: return "waiting-gpu";
	case BakeJobState::Running: return "running";
	case BakeJobState::Stopping: return "stopping";
	case BakeJobState::Stopped: return "stopped";
	case BakeJobState::Finished: return "finished";
	case BakeJobState::Failed: return "failed";
	case BakeJobState::Cancelled: return "cancelled";
	}
	return "failed";
}

bool BakeJobTerminal(BakeJobState state)
{
	return state == BakeJobState::Stopped || state == BakeJobState::Finished || state == BakeJobState::Failed ||
		state == BakeJobState::Cancelled;
}

struct SimulationBakeManager::Entry {
	BakeJob job;
	std::uint64_t startStamp = 0;     // ProcessStartStamp of job.pid
	bool child = false;               // started by this manager object (waitpid gives its exit code)
	bool interruptSent = false;
	std::vector<std::pair<std::string, std::string>> expect;
	double obsWall = -1, obsSim = 0;  // first progress observation of this process (ETA)
	std::vector<std::string> upstreamNames;
};

std::string SimulationBakeManager::SceneKey(const fs::path& scene)
{
	if (scene.empty()) return std::string();
	std::error_code ec;
	fs::path p = fs::weakly_canonical(fs::absolute(scene, ec), ec);
	if (ec) p = scene;
	return p.string();
}

SimulationBakeManager& SimulationBakeManager::Instance()
{
	static SimulationBakeManager manager;
	return manager;
}

SimulationBakeManager::SimulationBakeManager() {}

SimulationBakeManager::SimulationBakeManager(const BakeManagerSettings& settings) : settings_(settings) {}

SimulationBakeManager::~SimulationBakeManager()
{
	{
		Lock lock(mutex_);
		quit_ = true;
	}
	wake_.notify_all();
	if (monitor_.joinable()) monitor_.join();
}

BakeManagerSettings SimulationBakeManager::Settings() const
{
	Lock lock(mutex_);
	return settings_;
}

void SimulationBakeManager::SetSettings(const BakeManagerSettings& settings)
{
	{
		Lock lock(mutex_);
		settings_ = settings;
	}
	wake_.notify_all();
}

void SimulationBakeManager::SetWorkerOverride(const std::string& path)
{
	Lock lock(mutex_);
	workerOverride_ = path;
}

bool SimulationBakeManager::LocateWorker(const std::string& override, fs::path& found, std::vector<fs::path>* searched)
{
#ifdef _WIN32
	const std::string ext = ".exe";
	const char separator = ';';
#else
	const std::string ext;
	const char separator = ':';
#endif
	std::vector<fs::path> candidates;
	if (!override.empty()) candidates.push_back(override);
	if (const char* env = std::getenv("RISE_SIM_WORKER")) if (*env) candidates.push_back(env);
	const fs::path exe = CurrentExecutablePath();
	if (!exe.empty()) {
		const fs::path d = exe.parent_path();
		for (const char* rel : {"rise-sim", "rise_sim", "tools/rise_sim", "tools/rise-sim", "../tools/rise_sim",
			"../Helpers/rise-sim", "../Resources/rise-sim"})
			candidates.push_back((d / rel).string() + ext);
	}
	if (const char* path = std::getenv("PATH")) {
		std::string all(path);
		std::size_t start = 0;
		while (start <= all.size()) {
			const std::size_t end = all.find(separator, start);
			const std::string dir = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!dir.empty()) {
				candidates.push_back((fs::path(dir) / "rise-sim").string() + ext);
				candidates.push_back((fs::path(dir) / "rise_sim").string() + ext);
			}
			if (end == std::string::npos) break;
			start = end + 1;
		}
	}
	for (std::size_t i = 0; i < candidates.size(); ++i) {
		if (searched) searched->push_back(candidates[i]);
		if (IsExecutable(candidates[i])) {
			std::error_code ec;
			const fs::path canonical = fs::weakly_canonical(candidates[i], ec);
			found = ec ? candidates[i] : canonical;
			return true;
		}
		// An explicit override that is not there is an error, not a reason to
		// fall back to another worker.
		if (i == 0 && !override.empty()) return false;
	}
	return false;
}

bool SimulationBakeManager::ResolveWorker(std::vector<std::string>& command, std::string& error) const
{
	BakeManagerSettings settings;
	std::string override;
	{
		Lock lock(mutex_);
		settings = settings_;
		override = workerOverride_;
	}
	if (!settings.workerCommand.empty()) {
		command = settings.workerCommand;
		return true;
	}
	fs::path found;
	std::vector<fs::path> searched;
	if (!LocateWorker(override, found, &searched)) {
		std::string list;
		for (const auto& s : searched) list += (list.empty() ? "" : ", ") + s.string();
#if defined(_WIN32) || defined(__ANDROID__)
		// M8: the VS2022 and Android builds have no rise-sim target and this
		// platform's process host has never been compiled or run.
		const std::string platform = " (out-of-process bakes are unsupported on this platform: no rise-sim worker is built for it)";
#else
		const std::string platform;
#endif
		return Fail(error, override.empty() ?
			"rise-sim was not found (looked in: " + list + "); set the worker path (RISE_API_SimulationBake_SetWorkerPath or RISE_SIM_WORKER)" + platform :
			"the configured rise-sim worker " + override + " is not an executable file");
	}
	command = {found.string()};
	return true;
}

std::uint64_t SimulationBakeManager::MemoryBudget_() const
{
	if (settings_.memoryBudget) return settings_.memoryBudget;
	const std::uint64_t physical = PhysicalMemory();
	return physical ? physical / 2 : ~std::uint64_t(0);
}

std::uint64_t SimulationBakeManager::FramesGeneration(const std::string& scene) const
{
	Lock lock(mutex_);
	const auto it = framesGeneration_.find(scene);
	return it == framesGeneration_.end() ? 0 : it->second;
}

std::uint64_t SimulationBakeManager::AddListener(BakeListener listener)
{
	std::lock_guard<std::mutex> lock(listenerMutex_);
	const std::uint64_t id = nextListener_++;
	listeners_[id] = std::move(listener);
	return id;
}

void SimulationBakeManager::RemoveListener(std::uint64_t id)
{
	{
		std::lock_guard<std::mutex> lock(listenerMutex_);
		listeners_.erase(id);
	}
	// Wait for a dispatch in flight (recursive: from inside a listener this
	// returns at once, and the caller is that dispatch).
	std::lock_guard<std::recursive_mutex> wait(dispatchMutex_);
}

void SimulationBakeManager::Dispatch_(std::vector<BakeEvent>& events)
{
	if (events.empty()) return;
	std::lock_guard<std::recursive_mutex> dispatching(dispatchMutex_);
	std::vector<std::pair<std::uint64_t, BakeListener>> listeners;
	{
		std::lock_guard<std::mutex> lock(listenerMutex_);
		listeners.assign(listeners_.begin(), listeners_.end());
	}
	for (const auto& e : events) {
		for (const auto& l : listeners) {
			{
				// A listener removed by an earlier one in this dispatch is skipped.
				std::lock_guard<std::mutex> lock(listenerMutex_);
				if (!listeners_.count(l.first)) continue;
			}
			try { l.second(e); } catch (...) {}
		}
	}
	events.clear();
}

void SimulationBakeManager::Update_(Entry& e, std::vector<BakeEvent>& events, bool framesLanded)
{
	const std::uint64_t c = ++counter_;
	e.job.changeCounter = c;
	if (framesLanded) ++framesGeneration_[e.job.scene];
	BakeEvent ev;
	ev.kind = BakeEvent::Kind::JobChanged;
	ev.job = e.job;
	ev.changeCounter = c;
	events.push_back(ev);
	if (framesLanded) {
		ev.kind = BakeEvent::Kind::FramesLanded;
		events.push_back(ev);
	}
	changed_.notify_all();
}

SimulationBakeManager::Entry* SimulationBakeManager::Active_(const std::string& scene, const std::string& simulation)
{
	for (auto& j : jobs_)
		if (j.second->job.scene == scene && j.second->job.simulation == simulation && !BakeJobTerminal(j.second->job.state))
			return j.second.get();
	return nullptr;
}

SimulationBakeManager::Entry& SimulationBakeManager::NewEntry_(const std::string& scene, const SceneSimulationStatus& s)
{
	std::unique_ptr<Entry> e(new Entry);
	e->job.id = g_nextJobId++;
	e->job.scene = scene;
	e->job.simulation = s.name;
	e->job.solverId = s.solverId;
	e->job.backend = s.backend;
	e->job.cacheDir = s.cacheDir;
	e->job.framesTotal = s.framesTotal;
	e->job.memoryEstimate = s.memoryEstimate;
	if (s.framesTotal) e->job.endTime = s.startTime + static_cast<double>(s.framesTotal - 1) * s.interval;
	e->upstreamNames = s.upstream;
	Entry& ref = *e;
	jobs_[ref.job.id] = std::move(e);
	// Bounded history: drop the oldest terminal jobs.
	std::size_t terminal = 0;
	for (const auto& j : jobs_) if (BakeJobTerminal(j.second->job.state)) ++terminal;
	for (auto it = jobs_.begin(); terminal > kMaxTerminalJobs && it != jobs_.end();) {
		if (BakeJobTerminal(it->second->job.state)) { it = jobs_.erase(it); --terminal; }
		else ++it;
	}
	return ref;
}

void SimulationBakeManager::EnsureMonitor_(Lock&)
{
	if (!monitor_.joinable()) monitor_ = std::thread([this] { MonitorLoop_(); });
}

void SimulationBakeManager::MonitorLoop_()
{
	Lock lock(mutex_);
	while (!quit_) {
		std::vector<BakeEvent> events;
		Tick_(lock, events);
		if (!events.empty()) {
			lock.unlock();
			Dispatch_(events);
			lock.lock();
			if (quit_) break;
		}
		const unsigned ms = std::max(20u, settings_.pollMilliseconds);
		wake_.wait_for(lock, std::chrono::milliseconds(ms));
	}
}

void SimulationBakeManager::PollNow()
{
	std::vector<BakeEvent> events;
	{
		Lock lock(mutex_);
		Tick_(lock, events);
	}
	Dispatch_(events);
}

// ------------------------------------------------------------------ progress

void SimulationBakeManager::ReadProgress_(Entry& e, std::vector<BakeEvent>& events)
{
	WorkerStatus s;
	std::string ignored;
	if (!ReadWorkerStatus(e.job.cacheDir, s, ignored)) return;
	// A record another process wrote (the previous bake's, before this worker
	// wrote its first) is not this job's.  An adopted worker of an older
	// rise-sim records no pid: its record is the only one there is.
	if (e.job.pid != 0 && s.pid != e.job.pid) return;
	bool changed = false, frames = false;
	BakeJobState state = e.job.state;
	if (e.job.state != BakeJobState::Stopping) {
		if (s.state == "waiting-gpu") state = BakeJobState::WaitingForGpu;
		else if (s.state == "running") state = BakeJobState::Running;
	}
	if (state != e.job.state) { e.job.state = state; changed = true; }
	if (s.framesWritten != e.job.framesWritten) { frames = s.framesWritten > e.job.framesWritten; changed = true; }
	if (s.simulatedTime != e.job.simulatedTime || s.endTime != e.job.endTime || s.framesTotal != e.job.framesTotal ||
		s.detail != e.job.detail || s.wallSeconds != e.job.wallSeconds) changed = true;
	e.job.simulatedTime = s.simulatedTime;
	e.job.endTime = s.endTime;
	e.job.framesWritten = s.framesWritten;
	e.job.framesTotal = s.framesTotal;
	e.job.wallSeconds = s.wallSeconds;
	e.job.detail = s.detail;
	if (s.state == "running") {
		if (e.obsWall < 0) { e.obsWall = s.wallSeconds; e.obsSim = s.simulatedTime; }
		const double dw = s.wallSeconds - e.obsWall, ds = s.simulatedTime - e.obsSim;
		const double eta = dw > 0 && ds > 0 ? std::max(0.0, (s.endTime - s.simulatedTime) * dw / ds) : -1;
		if (eta != e.job.etaSeconds) { e.job.etaSeconds = eta; changed = true; }
	}
	if (changed) Update_(e, events, frames);
}

void SimulationBakeManager::Finalize_(Entry& e, bool haveExit, int exitCode, std::vector<BakeEvent>& events)
{
	WorkerStatus s;
	std::string ignored;
	const bool status = ReadWorkerStatus(e.job.cacheDir, s, ignored) && (e.job.pid == 0 || s.pid == e.job.pid);
	const std::uint64_t before = e.job.framesWritten;
	if (status) {
		e.job.simulatedTime = s.simulatedTime;
		e.job.endTime = s.endTime;
		e.job.framesWritten = s.framesWritten;
		e.job.framesTotal = s.framesTotal;
		e.job.wallSeconds = s.wallSeconds;
	}
	e.job.detail.clear();
	e.job.exitCode = haveExit ? exitCode : -1;
	BakeJobState state;
	if (haveExit) {
		if (exitCode == WorkerFinished) state = BakeJobState::Finished;
		else if (exitCode == WorkerStopped) state = BakeJobState::Stopped;
		else if (exitCode >= 128) { state = BakeJobState::Stopped; e.job.interrupted = true; }
		else state = BakeJobState::Failed;
	} else if (status && s.state == "finished") state = BakeJobState::Finished;
	else if (status && s.state == "stopped") state = BakeJobState::Stopped;
	else if (status && s.state == "failed") state = BakeJobState::Failed;
	else { state = BakeJobState::Stopped; e.job.interrupted = true; }
	if (state == BakeJobState::Failed) {
		e.job.lastError = status && !s.lastError.empty() ? s.lastError :
			e.job.logFile.empty() ? std::string("the worker failed") : LastWorkerMessage(e.job.logFile);
		if (e.job.lastError.find(kWorkerSceneMismatch) != std::string::npos)
			e.job.lastError = "save first: " + e.job.lastError;
	} else if (e.job.interrupted && e.job.lastError.empty()) {
		e.job.lastError = haveExit ? "the worker was ended by signal " + std::to_string(exitCode - 128) +
			" (no checkpoint at the stop; a resume continues from the last periodic checkpoint)" :
			"the worker ended without a clean stop";
	}
	if (state == BakeJobState::Finished) {
		e.job.etaSeconds = 0;
		e.job.framesWritten = std::max(e.job.framesWritten, e.job.framesTotal);
	} else e.job.etaSeconds = -1;
	e.job.state = state;
	std::string why;
	e.job.resumable = state != BakeJobState::Finished && InspectCache(e.job.cacheDir, kManifestBudget, why) == CacheState::Partial;
	std::error_code ec;
	fs::remove(e.job.cacheDir / kWorkerStopFileName, ec);
	Update_(e, events, e.job.framesWritten > before || state == BakeJobState::Finished);
}

// ------------------------------------------------------------------ scheduling

bool SimulationBakeManager::StartJob_(Entry& e, std::string& error)
{
	std::vector<std::string> argv;
	if (!settings_.workerCommand.empty()) argv = settings_.workerCommand;
	else {
		fs::path found;
		std::vector<fs::path> searched;
		if (!LocateWorker(workerOverride_, found, &searched)) {
			std::string list;
			for (const auto& s : searched) list += (list.empty() ? "" : ", ") + s.string();
			return Fail(error, "rise-sim was not found (looked in: " + list + ")");
		}
		argv.push_back(found.string());
	}
	argv.push_back(e.job.resume ? "resume" : "bake");
	argv.push_back(e.job.scene);
	argv.push_back(e.job.simulation);
	if (!e.job.resume) argv.push_back("--no-upstream");
	if (e.job.force && !e.job.resume) argv.push_back("--force");
	if (!settings_.gpuLock.empty()) { argv.push_back("--gpu-lock"); argv.push_back(settings_.gpuLock.string()); }
	if (settings_.checkpointSeconds >= 0) {
		std::ostringstream s;
		s << settings_.checkpointSeconds;
		argv.push_back("--checkpoint-seconds");
		argv.push_back(s.str());
	}
	for (const auto& x : e.expect) { argv.push_back("--expect-key"); argv.push_back(x.first + "=" + x.second); }
	const fs::path dir = (settings_.stateDirectory.empty() ? DefaultBakeStateDirectory() : settings_.stateDirectory) / "logs";
	if (!CreatePrivateDirectories(dir, error)) return false;
	std::string leaf;
	for (char c : e.job.simulation) leaf += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
	e.job.logFile = dir / (std::to_string(static_cast<long long>(std::time(nullptr))) + "-" + std::to_string(CurrentProcessId()) +
		"-" + std::to_string(e.job.id) + "-" + leaf + ".log");
	{
		std::ofstream log(e.job.logFile, std::ios::app);
		log << "# rise-sim job " << e.job.id << ":";
		for (const auto& a : argv) log << " " << a;
		log << "\n";
	}
	std::uint64_t pid = 0;
	if (!SpawnDetachedProcess(argv, e.job.logFile, pid, error)) return false;
	e.job.pid = pid;
	e.startStamp = ProcessStartStamp(pid);
	e.child = true;
	e.interruptSent = false;
	e.obsWall = -1;
	e.job.state = BakeJobState::Running;
	e.job.waitingFor.clear();
	e.job.lastError.clear();
	e.job.interrupted = false;
	e.job.resumable = false;
	return true;
}

unsigned SimulationBakeManager::VerifyCachesLocked_(std::vector<BakeEvent>& events)
{
	// The newest job of each cache directory decides; a newer active job (a
	// bake writing it) means the cache is being made, not gone.
	std::map<fs::path, Entry*> latest;
	for (auto& j : jobs_) latest[j.second->job.cacheDir] = j.second.get();
	unsigned marked = 0;
	for (auto& l : latest) {
		Entry& e = *l.second;
		const BakeJob& b = e.job;
		if (b.cacheDir.empty() || b.cacheMissing) continue;
		// A simulation_group job has no solver and no cache of its own (its directory holds only the worker's
		// status file); its members' jobs carry the caches and are checked on their own.
		if (b.solverId.empty()) continue;
		const bool hadCache = b.state == BakeJobState::Finished || (b.state == BakeJobState::Stopped && b.framesWritten > 0);
		if (!hadCache) continue;
		std::error_code ec;
		const auto manifest = fs::symlink_status(b.cacheDir / "manifest.cbor", ec);
		if (manifest.type() != fs::file_type::not_found) continue;
		std::string ignored;
		if (InspectCache(b.cacheDir, kManifestBudget, ignored) != CacheState::Missing) continue;   // a writer holds it, or it is odd: not ours to call
		e.job.cacheMissing = true;
		e.job.resumable = false;
		e.job.lastError = "the simulation cache " + b.cacheDir.string() + " was deleted from disk; bake the simulation again";
		e.job.detail = "cache deleted from disk";
		Update_(e, events, /*framesLanded=*/true);   // consumers drop the frames they still show
		++marked;
	}
	return marked;
}

unsigned SimulationBakeManager::VerifyCaches()
{
	std::vector<BakeEvent> events;
	unsigned marked = 0;
	{
		Lock lock(mutex_);
		marked = VerifyCachesLocked_(events);
		lastVerify_ = std::chrono::steady_clock::now();
	}
	Dispatch_(events);
	return marked;
}

void SimulationBakeManager::Tick_(Lock&, std::vector<BakeEvent>& events)
{
	// 0. Caches deleted behind our back (a stat per finished job, once a second).
	if (std::chrono::steady_clock::now() - lastVerify_ >= std::chrono::seconds(1)) {
		lastVerify_ = std::chrono::steady_clock::now();
		VerifyCachesLocked_(events);
	}
	// 1. Live workers: exit, progress, stop requests.
	for (auto& j : jobs_) {
		Entry& e = *j.second;
		if (!Live(e.job.state)) continue;
		if (e.job.pid != 0) {
			int code = 0;
			bool notChild = false;
			if (ReapChildProcess(e.job.pid, code, notChild)) { Finalize_(e, true, code, events); continue; }
			if (notChild && !ProcessAlive(e.job.pid, e.startStamp)) { Finalize_(e, false, 0, events); continue; }
		} else {
			// An older worker without a pid: it is alive while it holds the cache lock.
			std::string ignored;
			if (InspectCache(e.job.cacheDir, kManifestBudget, ignored) != CacheState::Baking) {
				Finalize_(e, false, 0, events);
				continue;
			}
		}
		if (e.job.state == BakeJobState::Stopping) {
			// Re-assert the stop file: a worker that was still starting removes
			// a stale one before it begins.
			std::error_code ec;
			if (!fs::exists(e.job.cacheDir / kWorkerStopFileName, ec)) {
				std::string ignored;
				ReplaceFileDurably(e.job.cacheDir, kWorkerStopFileName, Bytes{'s', 't', 'o', 'p', '\n'}, ignored);
			}
		}
		ReadProgress_(e, events);
	}

	// 2. The queue, in id order.
	unsigned cpuRunning = 0;
	bool gpuRunning = false;
	std::uint64_t memoryRunning = 0;
	for (const auto& j : jobs_) {
		const BakeJob& b = j.second->job;
		if (!Live(b.state)) continue;
		if (IsGpuBackend(b.backend) || b.backend.empty()) gpuRunning = true; else ++cpuRunning;
		memoryRunning += b.memoryEstimate;
	}
	const std::uint64_t budget = MemoryBudget_();
	const unsigned maxCpu = std::max(1u, settings_.maxCpuBakes);
	for (auto& j : jobs_) {
		Entry& e = *j.second;
		if (e.job.state != BakeJobState::Queued) continue;
		std::string waiting;
		bool cancel = false;
		for (const std::uint64_t d : e.job.dependsOn) {
			const auto it = jobs_.find(d);
			if (it == jobs_.end()) continue;
			const BakeJob& up = it->second->job;
			if (up.state == BakeJobState::Finished) continue;
			if (BakeJobTerminal(up.state)) {
				cancel = true;
				e.job.lastError = "upstream simulation " + up.simulation + " did not finish (" + BakeJobStateName(up.state) + ")";
				break;
			}
			waiting = "upstream simulation " + up.simulation;
			break;
		}
		if (cancel) { e.job.state = BakeJobState::Cancelled; e.job.waitingFor.clear(); Update_(e, events); continue; }
		const bool gpu = IsGpuBackend(e.job.backend) || e.job.backend.empty();
		if (waiting.empty()) {
			if (gpu && gpuRunning) waiting = "the GPU slot (another GPU or unstated-backend bake of this app runs)";
			else if (!gpu && cpuRunning >= maxCpu) waiting = "a CPU bake slot (" + std::to_string(cpuRunning) + " of " +
				std::to_string(maxCpu) + " running)";
			else if (e.job.memoryEstimate > budget) {
				e.job.state = BakeJobState::Failed;
				e.job.lastError = "declares " + MiB(e.job.memoryEstimate) + ", over the bake memory budget of " + MiB(budget);
				e.job.waitingFor.clear();
				Update_(e, events);
				continue;
			} else if (e.job.memoryEstimate && memoryRunning + e.job.memoryEstimate > budget)
				waiting = "memory (" + MiB(memoryRunning) + " of " + MiB(budget) + " in use by running bakes)";
		}
		if (!waiting.empty()) {
			if (waiting != e.job.waitingFor) { e.job.waitingFor = waiting; Update_(e, events); }
			continue;
		}
		std::string error;
		if (!StartJob_(e, error)) {
			e.job.state = BakeJobState::Failed;
			e.job.lastError = error;
			e.job.waitingFor.clear();
			Update_(e, events);
			continue;
		}
		if (gpu) gpuRunning = true; else ++cpuRunning;
		memoryRunning += e.job.memoryEstimate;
		Update_(e, events);
	}
}

// ------------------------------------------------------------------ adoption

bool SimulationBakeManager::AttachLocked_(const BakeSceneSnapshot& scene, std::vector<BakeEvent>& events)
{
	const std::string key = SceneKey(scene.scene);
	if (key.empty()) return true;
	bool any = false;
	for (const auto& s : scene.simulations) {
		if (s.solverId.empty() || s.cacheDir.empty()) continue;
		// The newest job for this cache.
		Entry* latest = nullptr;
		for (auto& j : jobs_) if (j.second->job.cacheDir == s.cacheDir) latest = j.second.get();
		if (latest && !BakeJobTerminal(latest->job.state)) continue;
		WorkerStatus w;
		std::string ignored;
		if (!ReadWorkerStatus(s.cacheDir, w, ignored)) continue;
		const bool liveState = w.state == "running" || w.state == "waiting-gpu";
		if (latest) {
			// Only a worker started elsewhere after our last job is news.
			if (!liveState || w.pid == 0 || w.pid == latest->job.pid || !VerifiedLiveWorker(s.cacheDir, w)) continue;
		}
		const CacheState disk = InspectCache(s.cacheDir, kManifestBudget, ignored);
		const bool current = s.state == SceneSimulationState::Partial || s.state == SceneSimulationState::Failed ||
			s.state == SceneSimulationState::Baking;
		if (liveState && VerifiedLiveWorker(s.cacheDir, w)) {
			Entry& e = NewEntry_(key, s);
			e.job.adopted = true;
			e.job.pid = w.pid;
			e.startStamp = w.pidStart;
			e.job.state = w.state == "waiting-gpu" ? BakeJobState::WaitingForGpu : BakeJobState::Running;
			ReadProgress_(e, events);
			Update_(e, events);
			any = true;
		} else if (liveState && w.pid == 0 && disk == CacheState::Baking) {
			// An older rise-sim (no pid in its record) holds the cache lock.
			Entry& e = NewEntry_(key, s);
			e.job.adopted = true;
			e.job.stoppable = false;
			e.job.state = BakeJobState::Running;
			ReadProgress_(e, events);
			e.job.detail = "baked by a rise-sim that does not record its pid; stop it where it was started";
			Update_(e, events);
			any = true;
		} else if (liveState || w.state == "stopped") {
			if (disk != CacheState::Partial) continue;
			// Interrupted (the record says running, nothing runs) or cleanly stopped.
			Entry& e = NewEntry_(key, s);
			e.job.adopted = true;
			e.job.pid = 0;
			e.job.state = BakeJobState::Stopped;
			e.job.interrupted = liveState;
			e.job.resumable = current;
			e.job.simulatedTime = w.simulatedTime;
			e.job.endTime = w.endTime;
			e.job.framesWritten = w.framesWritten;
			e.job.framesTotal = w.framesTotal;
			e.job.wallSeconds = w.wallSeconds;
			e.job.lastError = liveState ? "interrupted: the worker (pid " + std::to_string(w.pid) +
				") is gone without a clean stop" : std::string();
			if (!current) e.job.detail = "the cache holds another configuration (stale); bake to replace it";
			Update_(e, events);
			any = true;
		}
	}
	return any;
}

bool SimulationBakeManager::Attach(const BakeSceneSnapshot& scene, std::string&)
{
	std::vector<BakeEvent> events;
	{
		Lock lock(mutex_);
		if (AttachLocked_(scene, events)) EnsureMonitor_(lock);
		VerifyCachesLocked_(events);
		lastVerify_ = std::chrono::steady_clock::now();
	}
	wake_.notify_all();
	Dispatch_(events);
	return true;
}

// ------------------------------------------------------------------ requests

bool SimulationBakeManager::RequestBake(const BakeSceneSnapshot& scene, const BakeRequest& request,
	std::vector<std::uint64_t>& jobs, std::string& error)
{
	jobs.clear();
	if (scene.scene.empty())
		return Fail(error, "save the scene first: a bake runs rise-sim on the scene file, and this document has no file");
	const std::string key = SceneKey(scene.scene);
	std::vector<BakeEvent> events;
	{
		Lock lock(mutex_);
		AttachLocked_(scene, events);
		// M6: a simulation_group bakes as one job (one worker running the group
		// host); its members are never jobs of their own.
		auto groupOf = [&](const std::string& n) -> const SceneSimulationStatus* {
			for (const auto& g : scene.simulations)
				if (g.keyword == kSimulationGroupKeyword && std::find(g.upstream.begin(), g.upstream.end(), n) != g.upstream.end()) return &g;
			return nullptr;
		};
		auto bakeable = [&](const SceneSimulationStatus& x) { return !x.solverId.empty() || x.keyword == kSimulationGroupKeyword; };
		// The requested set.
		std::set<std::string> named;
		if (request.all) {
			for (const auto& s : scene.simulations)
				if (bakeable(s) && !groupOf(s.name) && (!request.staleOnly || OutOfDate(s))) named.insert(s.name);
		} else {
			for (const auto& n : request.simulations) {
				const SceneSimulationStatus* s = Find(scene, n);
				if (!s) {
					std::string list;
					for (const auto& x : scene.simulations) list += (list.empty() ? "" : ", ") + x.name;
					return Fail(error, "the scene declares no simulation named `" + n + "` (it declares: " + list + ")");
				}
				if (!bakeable(*s)) return Fail(error, "`" + n + "` is a " + s->keyword + ": it has no solver to bake");
				const SceneSimulationStatus* g = groupOf(n);
				named.insert(g ? g->name : n);
			}
			if (named.empty()) return Fail(error, "name a simulation to bake, or ask for all");
		}
		// Plus every upstream that is out of date (transitively).
		std::set<std::string> wanted = named;
		for (auto it = scene.simulations.rbegin(); it != scene.simulations.rend(); ++it) {
			if (!wanted.count(it->name)) continue;
			std::vector<std::string> ups = it->upstream;
			if (it->keyword == kSimulationGroupKeyword) {   // M6: the members' own upstreams outside the group
				ups.clear();
				for (const auto& m : it->upstream)
					if (const SceneSimulationStatus* ms = Find(scene, m))
						for (const auto& u : ms->upstream)
							if (std::find(it->upstream.begin(), it->upstream.end(), u) == it->upstream.end()) ups.push_back(u);
			}
			for (const auto& u0 : ups) {
				const SceneSimulationStatus* g = groupOf(u0);
				const std::string u = g ? g->name : u0;
				const SceneSimulationStatus* up = Find(scene, u);
				if (up && bakeable(*up) && (OutOfDate(*up) || Active_(key, u))) wanted.insert(u);
			}
		}
		// Save rule (see the header).
		if (scene.unsavedChanges) {
			for (const auto& s : scene.simulations) {
				if (!wanted.count(s.name) || !s.expectedKey.empty() || Active_(key, s.name)) continue;
				return Fail(error, "save the scene first: the document has unsaved changes, and the key of `" + s.name +
					"` cannot be checked against the file yet (" + (s.error.empty() ? std::string("not known before a bake") : s.error) +
					"); the bake runs from the file on disk");
			}
		}
		for (const auto& s : scene.simulations) {
			if (!wanted.count(s.name)) continue;
			if (Entry* active = Active_(key, s.name)) {
				jobs.push_back(active->job.id);
				continue;
			}
			const bool force = request.force && named.count(s.name);
			Entry& e = NewEntry_(key, s);
			e.job.force = force;
			if (!s.expectedKey.empty()) e.expect.emplace_back(s.name, s.expectedKey);
			for (const auto& u : s.upstream) if (Entry* up = Active_(key, u)) if (up != &e) e.job.dependsOn.push_back(up->job.id);
			if (!force && !OutOfDate(s) && e.job.dependsOn.empty()) {
				// Up to date: nothing to run.
				e.job.state = BakeJobState::Finished;
				e.job.framesWritten = s.framesAvailable;
				e.job.etaSeconds = 0;
				e.job.detail = "up to date";
			}
			jobs.push_back(e.job.id);
			Update_(e, events);
		}
		EnsureMonitor_(lock);
	}
	wake_.notify_all();
	Dispatch_(events);
	return true;
}

bool SimulationBakeManager::Resume(const BakeSceneSnapshot& scene, const std::string& simulation, std::uint64_t& job,
	std::string& error)
{
	if (scene.scene.empty()) return Fail(error, "save the scene first: a bake runs rise-sim on the scene file");
	const std::string key = SceneKey(scene.scene);
	const SceneSimulationStatus* s = Find(scene, simulation);
	if (!s) return Fail(error, "the scene declares no simulation named `" + simulation + "`");
	if (s->solverId.empty()) return Fail(error, "`" + simulation + "` has no solver");
	std::vector<BakeEvent> events;
	{
		Lock lock(mutex_);
		AttachLocked_(scene, events);
		if (Entry* active = Active_(key, simulation)) {
			job = active->job.id;
			lock.unlock();
			Dispatch_(events);
			return Fail(error, "`" + simulation + "` is already " + BakeJobStateName(active->job.state));
		}
		if (s->state != SceneSimulationState::Partial && s->state != SceneSimulationState::Failed)
			return Fail(error, "nothing to resume: `" + simulation + "` is " + SceneSimulationStateName(s->state) +
				(s->state == SceneSimulationState::Stale ? " (bake it to replace the cache)" : ""));
		if (scene.unsavedChanges && s->expectedKey.empty())
			return Fail(error, "save the scene first: the document has unsaved changes and the key of `" + simulation +
				"` cannot be checked against the file");
		Entry& e = NewEntry_(key, *s);
		e.job.resume = true;
		if (!s->expectedKey.empty()) e.expect.emplace_back(s->name, s->expectedKey);
		job = e.job.id;
		Update_(e, events);
		EnsureMonitor_(lock);
	}
	wake_.notify_all();
	Dispatch_(events);
	return true;
}

bool SimulationBakeManager::StopLocked_(Entry& e, bool kill, std::vector<BakeEvent>& events, std::string& error)
{
	if (e.job.state == BakeJobState::Queued) {
		e.job.state = BakeJobState::Cancelled;
		e.job.waitingFor.clear();
		Update_(e, events);
		return true;
	}
	if (BakeJobTerminal(e.job.state)) return Fail(error, "job " + std::to_string(e.job.id) + " is not running (" +
		BakeJobStateName(e.job.state) + ")");
	if (!e.job.stoppable || e.job.pid == 0)
		return Fail(error, "this bake runs in a process that does not record its pid; stop it where it was started");
	if (kill) {
		if (!KillProcess(e.job.pid, e.startStamp, error)) return false;
		if (e.job.state != BakeJobState::Stopping) { e.job.state = BakeJobState::Stopping; Update_(e, events); }
		return true;
	}
	// The stop file works everywhere (and is the only stop on Windows); SIGINT
	// makes a POSIX worker notice within its 20 ms watcher tick.
	if (!ReplaceFileDurably(e.job.cacheDir, kWorkerStopFileName, Bytes{'s', 't', 'o', 'p', '\n'}, error)) return false;
#ifndef _WIN32
	if (!e.interruptSent) {
		std::string ignored;
		if (InterruptProcess(e.job.pid, e.startStamp, ignored)) e.interruptSent = true;
	}
#endif
	if (e.job.state != BakeJobState::Stopping) {
		e.job.state = BakeJobState::Stopping;
		e.job.detail = "stopping at the next safe point (checkpoint)";
		Update_(e, events);
	}
	return true;
}

bool SimulationBakeManager::Stop(std::uint64_t id, bool kill, std::string& error)
{
	std::vector<BakeEvent> events;
	bool ok = false;
	{
		Lock lock(mutex_);
		const auto it = jobs_.find(id);
		if (it == jobs_.end()) return Fail(error, "no bake job " + std::to_string(id));
		ok = StopLocked_(*it->second, kill, events, error);
	}
	wake_.notify_all();
	Dispatch_(events);
	return ok;
}

bool SimulationBakeManager::StopSimulation(const std::string& scene, const std::string& simulation, bool kill,
	std::uint64_t* job, std::string& error)
{
	std::vector<BakeEvent> events;
	bool ok = false;
	{
		Lock lock(mutex_);
		Entry* e = Active_(scene, simulation);
		if (!e) return Fail(error, "`" + simulation + "` has no queued or running bake");
		if (job) *job = e->job.id;
		ok = StopLocked_(*e, kill, events, error);
	}
	wake_.notify_all();
	Dispatch_(events);
	return ok;
}

unsigned SimulationBakeManager::CancelQueued(const std::string& scene)
{
	std::vector<BakeEvent> events;
	unsigned n = 0;
	{
		Lock lock(mutex_);
		for (auto& j : jobs_) {
			Entry& e = *j.second;
			if (e.job.state != BakeJobState::Queued || (!scene.empty() && e.job.scene != scene)) continue;
			e.job.state = BakeJobState::Cancelled;
			e.job.waitingFor.clear();
			Update_(e, events);
			++n;
		}
	}
	wake_.notify_all();
	Dispatch_(events);
	return n;
}

// ------------------------------------------------------------------ queries

std::vector<BakeJob> SimulationBakeManager::Jobs(const std::string& scene) const
{
	Lock lock(mutex_);
	std::vector<BakeJob> out;
	for (const auto& j : jobs_) if (scene.empty() || j.second->job.scene == scene) out.push_back(j.second->job);
	return out;
}

bool SimulationBakeManager::Job(std::uint64_t id, BakeJob& job) const
{
	Lock lock(mutex_);
	const auto it = jobs_.find(id);
	if (it == jobs_.end()) return false;
	job = it->second->job;
	return true;
}

bool SimulationBakeManager::LatestJob(const std::string& scene, const std::string& simulation, BakeJob& job) const
{
	Lock lock(mutex_);
	const BakeJob* found = nullptr;
	for (const auto& j : jobs_)
		if (j.second->job.scene == scene && j.second->job.simulation == simulation) found = &j.second->job;
	if (!found) return false;
	job = *found;
	return true;
}

bool SimulationBakeManager::Wait(std::uint64_t id, unsigned timeoutMs, BakeJob& job) const
{
	Lock lock(mutex_);
	const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		const auto it = jobs_.find(id);
		if (it == jobs_.end()) return false;
		job = it->second->job;
		if (BakeJobTerminal(job.state)) return true;
		if (changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
			const auto again = jobs_.find(id);
			if (again == jobs_.end()) return false;
			job = again->second->job;
			return BakeJobTerminal(job.state);
		}
	}
}

} }
