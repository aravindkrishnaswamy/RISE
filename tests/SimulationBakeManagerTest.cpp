// Simulation M5: the bake manager and the agent's simulation verbs
// (docs/SIMULATION_M5A_RESULTS.md).  CPU only: cloth.shell and the reference
// solvers.  Every bake is a real separate worker process: this program
// re-runs itself as rise-sim (`--rise-sim-worker`, the library WorkerMain
// with the built-in and reference solvers registered), started by the
// manager's own spawn path.  Every worker this test sees is killed and reaped
// at the end, also when a check fails.
//
//   L  worker location: override, search, a missing override is an error
//   Q  queue order on a two-simulation chain (upstream first; the downstream waits for it)
//   G  GPU lock: two worker processes of a fake-GPU solver contend for one (private) lock;
//      a second manager adopts both, one waits ("waiting-gpu"), then runs; a stop while
//      waiting ends it stopped; this manager runs one GPU-class job at a time
//   S  stop -> stopped, resumable -> resume -> the sealed cache equals an uninterrupted bake;
//      change notifications fire as frames land
//   A  adoption: the manager object is destroyed and recreated while the worker keeps running
//   P  a stale pid (another process, a dead process, a live process without the cache lock)
//      is never adopted: the bake is reported interrupted and resumable
//   F  the save rule: no file; unsaved edits that change the key (the worker refuses, "save
//      first", the cache untouched); unsaved edits with a key not known yet (refused at once)
//   V  agent verbs end to end over AgentRpc: sim_list, sim_bake, sim_wait, sim_status,
//      sim_cancel mid-bake, resume, live verbs headless, autonomy
//   R  a live controller through the C API: bake from the document, the job rows, the
//      change counter, and RefreshSimulationFrames binding an unbound consumer as frames
//      land and rebinding it after a rebake (a new epoch)
#include "../src/Library/Simulation/SimulationBakeManager.h"
#include "../src/Library/Simulation/SimulationProcess.h"
#include "../src/Library/Simulation/SimulationWorker.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SceneSimulation.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Job.h"
#include "../src/Library/Agent/AgentSimulation.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Geometry/SimulatedMeshGeometry.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/RISE_API.h"
#include "SimulationReferenceSolvers.h"
#include "TestTempDir.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace {
using namespace RISE;
using namespace RISE::Simulation;
namespace fs = std::filesystem;
using V = RISECBOR64::Value;
using Clock = std::chrono::steady_clock;
int failures = 0, passes = 0;
fs::path g_self;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (condition) { ++passes; return; }
	++failures;
	std::cerr << "FAIL: " << what << (detail.empty() ? "" : " -- " + detail) << "\n";
}

// ---------------------------------------------------------------- the fake GPU solver
// test.advection_diffusion, declaring a backend that needs the GPU.
class FakeGpuSolver final : public ISimulationSolver {
public:
	static constexpr const char* Id = "test.fake_gpu";
	bool Configure(const V& parameters, const ISceneInputs& inputs, SolverDescription& d, std::string& error) override {
		if (!inner_.Configure(parameters, inputs, d, error)) return false;
		d.solverId = Id;
		d.capabilities.backends.push_back({"test-gpu", BackendStatus::Supported, true, "a fake GPU backend (tests)"});
		d.capabilities.backend = "test-gpu";
		return true;
	}
	bool Start(std::string& error) override { return inner_.Start(error); }
	bool Restore(const Bytes& state, std::string& error) override { return inner_.Restore(state, error); }
	bool Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
		AdvanceResult& result, std::string& error) override {
		return inner_.Advance(request, sink, ports, progress, result, error);
	}
	bool Checkpoint(Bytes& state, std::string& error) override { return inner_.Checkpoint(state, error); }
	SolverPosition Position() const override { return inner_.Position(); }
	void RequestStop() override { inner_.RequestStop(); }
private:
	Reference::AdvectionDiffusionSolver inner_;
};

void RegisterTestSolvers()
{
	std::string error;
	RegisterBuiltinSolvers(error);
	Reference::RegisterReferenceSolvers();
	RegisterSolver(FakeGpuSolver::Id, []{ return std::unique_ptr<ISimulationSolver>(new FakeGpuSolver); }, error);
}

// ---------------------------------------------------------------- processes

std::set<std::uint64_t> g_pids;

void TrackJobs(const SimulationBakeManager& m)
{
	for (const auto& j : m.Jobs()) if (j.pid) g_pids.insert(j.pid);
}

void KillAll()
{
#ifndef _WIN32
	for (const auto pid : g_pids) {
		if (::kill(static_cast<pid_t>(pid), SIGKILL) == 0 || errno == ESRCH) {
			int status = 0;
			::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
			for (int i = 0; i < 100 && ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) == 0; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
#endif
	g_pids.clear();
}

bool WaitFor(const std::function<bool()>& condition, double seconds)
{
	const auto end = Clock::now() + std::chrono::duration<double>(seconds);
	while (Clock::now() < end) {
		if (condition()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return condition();
}

// ---------------------------------------------------------------- files and scenes

void WriteText(const fs::path& path, const std::string& text)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f << text;
}

Bytes ReadAll(const fs::path& path)
{
	std::ifstream f(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::map<std::string, Bytes> Sealed(const fs::path& dir)
{
	std::map<std::string, Bytes> files;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(dir, ec)) {
		const auto name = e.path().filename().string();
		if (name == "manifest.cbor" || e.path().extension() == ".frame" || e.path().extension() == ".static")
			files[name] = ReadAll(e.path());
	}
	return files;
}

const char* const Header = "RISE ASCII SCENE 7\n";

std::string Camera()
{
	return "pinhole_camera\n{\n\tlocation\t0 1 3\n\tlookat\t0 0.5 0\n\tup\t0 1 0\n\tfov\t40\n}\n\n";
}

// A 9x9 sheet pinned at two corners; `extra` adds chunk lines.
std::string Cloth(const std::string& name, unsigned frames, const std::string& extra,
	const std::string& origin = "-0.5 1 0.5")
{
	return "cloth_simulation\n{\n\tname\t" + name + "\n\tvertices_u\t9\n\tvertices_v\t9\n\tframes\t" +
		std::to_string(frames) + "\n\tsubsteps\t4\n\torigin\t" + origin + "\n\tpin\tu0v0\n\tpin\tu1v0\n" + extra + "}\n\n";
}

std::string Consumer(const std::string& geometry, const std::string& source)
{
	return "simulated_mesh_geometry\n{\n\tname\t" + geometry + "\n\tsource\t" + source + "\n\tdouble_sided\tTRUE\n"
		"}\n\nstandard_object\n{\n\tname\tobj_" + geometry + "\n\tgeometry\t" + geometry + "\n}\n\n";
}

std::string Pace(unsigned ms) { return "\tpace_ms\t" + std::to_string(ms) + "\n"; }

struct Scene {
	IJobPriv* job = nullptr;
	Scene() = default;
	Scene(const Scene&) = delete;
	Scene& operator=(const Scene&) = delete;
	~Scene() { if (job) job->release(); }
};

bool Load(const fs::path& path, Scene& scene)
{
	if (!RISE_CreateJobPriv(&scene.job) || !scene.job) return false;
	return scene.job->LoadAsciiSceneViaCst(path.string().c_str());
}

// The headless agent path's snapshot (the serialized Document against the file).
BakeSceneSnapshot Snapshot(const fs::path& scenePath)
{
	BakeSceneSnapshot s;
	Scene scene;
	std::string error;
	if (!Load(scenePath, scene) || !Agent::AgentSimulationSnapshot(*scene.job, nullptr, s, error))
		Check(false, "snapshot of " + scenePath.string(), error);
	return s;
}

const SceneSimulationStatus* Find(const BakeSceneSnapshot& s, const std::string& name)
{
	for (const auto& x : s.simulations) if (x.name == name) return &x;
	return nullptr;
}

BakeManagerSettings Settings(const fs::path& root)
{
	BakeManagerSettings s;
	s.workerCommand = {g_self.string(), "--rise-sim-worker"};
	s.gpuLock = root/"gpu.lock";
	s.stateDirectory = root/"state";
	s.pollMilliseconds = 40;
	s.checkpointSeconds = 0;   // after every output index
	s.maxCpuBakes = 2;
	return s;
}

fs::path PrivateDir(const fs::path& root, const std::string& leaf)
{
	const fs::path dir = root/leaf;
	std::error_code ec;
	fs::create_directories(dir, ec);
	fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
	return dir;
}

BakeJob WaitJob(SimulationBakeManager& m, std::uint64_t id, double seconds)
{
	BakeJob j;
	m.Wait(id, static_cast<unsigned>(seconds * 1000), j);
	TrackJobs(m);
	return j;
}

std::string Describe(const BakeJob& j)
{
	return std::string(BakeJobStateName(j.state)) + " frames " + std::to_string(j.framesWritten) + "/" +
		std::to_string(j.framesTotal) + (j.lastError.empty() ? "" : " error: " + j.lastError) +
		(j.waitingFor.empty() ? "" : " waiting for " + j.waitingFor) + (j.logFile.empty() ? "" : " log " + j.logFile.string());
}

// ---------------------------------------------------------------- L

void TestLocate(const fs::path& root)
{
	std::printf("L: worker location\n");
	fs::path found;
	std::vector<fs::path> searched;
	Check(!SimulationBakeManager::LocateWorker((root/"no-such-rise-sim").string(), found, &searched) && searched.size() == 1,
		"a missing override is an error, not a fall back");
	Check(SimulationBakeManager::LocateWorker(g_self.string(), found) && found == fs::weakly_canonical(g_self),
		"an executable override is used");
	searched.clear();
	const bool any = SimulationBakeManager::LocateWorker("", found, &searched);
	bool besideTools = false;
	for (const auto& s : searched) besideTools = besideTools || s.string().find("tools/rise_sim") != std::string::npos;
	Check(besideTools, "the search looks in the make tree's bin/tools beside the running program");
	std::printf("   search from %s: %s\n", g_self.parent_path().string().c_str(), any ? found.string().c_str() : "(not built)");
	SimulationBakeManager m(Settings(root));
	std::vector<std::string> command;
	std::string error;
	Check(m.ResolveWorker(command, error) && command.size() == 2 && command[1] == "--rise-sim-worker",
		"the configured worker command wins", error);
}

// ---------------------------------------------------------------- Q

void TestQueueOrder(const fs::path& root)
{
	std::printf("Q: queue order on a two-simulation chain\n");
	const fs::path dir = PrivateDir(root, "chain");
	const fs::path scene = dir/"chain.RISEscene";
	WriteText(scene, std::string(Header) + Camera() + Cloth("upper", 6, Pace(60)) + Consumer("geo_upper", "upper.surface") +
		Cloth("lower", 4, "\tcollider_object\tobj_geo_upper\n", "-0.5 1.4 0.5") + Consumer("geo_lower", "lower.surface"));
	SimulationBakeManager m(Settings(root));
	std::vector<std::string> order;   // "<simulation> <state>" as observed by the listener
	std::mutex orderMutex;
	const auto listener = m.AddListener([&](const BakeEvent& e) {
		if (e.kind != BakeEvent::Kind::JobChanged) return;
		std::lock_guard<std::mutex> lk(orderMutex);
		const std::string s = e.job.simulation + " " + BakeJobStateName(e.job.state);
		if (order.empty() || order.back() != s) order.push_back(s);
	});
	const BakeSceneSnapshot snap = Snapshot(scene);
	BakeRequest request;
	request.simulations = {"lower"};
	std::vector<std::uint64_t> ids;
	std::string error;
	Check(m.RequestBake(snap, request, ids, error) && ids.size() == 2, "a bake of the downstream queues two jobs", error);
	if (ids.size() != 2) { m.RemoveListener(listener); return; }
	BakeJob up, down;
	m.Job(ids[0], up);
	m.Job(ids[1], down);
	Check(up.simulation == "upper" && down.simulation == "lower" && down.dependsOn == std::vector<std::uint64_t>{ids[0]},
		"upstream first; the downstream depends on it");
	Check(WaitFor([&] { m.Job(ids[1], down); return !down.waitingFor.empty(); }, 5) &&
		down.waitingFor.find("upstream simulation upper") != std::string::npos && down.state == BakeJobState::Queued,
		"while the upstream bakes, the downstream is queued waiting for it", Describe(down));
	up = WaitJob(m, ids[0], 30);
	down = WaitJob(m, ids[1], 30);
	Check(up.state == BakeJobState::Finished && down.state == BakeJobState::Finished, "both finish",
		Describe(up) + " | " + Describe(down));
	m.RemoveListener(listener);
	std::size_t upFinished = order.size(), downRunning = order.size();
	for (std::size_t i = 0; i < order.size(); ++i) {
		if (order[i] == "upper finished" && upFinished == order.size()) upFinished = i;
		if (order[i] == "lower running" && downRunning == order.size()) downRunning = i;
	}
	std::string seen;
	for (const auto& o : order) seen += o + "; ";
	Check(upFinished < downRunning && downRunning < order.size(), "the downstream starts only after the upstream finished", seen);
	const BakeSceneSnapshot after = Snapshot(scene);
	const auto* u = Find(after, "upper");
	const auto* l = Find(after, "lower");
	Check(u && l && u->state == SceneSimulationState::Complete && l->state == SceneSimulationState::Complete &&
		!l->upstreamOutOfDate, "both caches complete with their current keys");
	// Bake again: nothing to do.
	Check(m.RequestBake(after, request, ids, error) && ids.size() == 1, "an up-to-date request makes one job", error);
	if (!ids.empty()) {
		BakeJob j;
		m.Job(ids[0], j);
		Check(j.state == BakeJobState::Finished && j.detail == "up to date" && j.pid == 0, "an up-to-date simulation starts no worker");
	}
	TrackJobs(m);
}

// ---------------------------------------------------------------- G

std::vector<std::string> WorkerArgs(const std::vector<std::string>& rest)
{
	std::vector<std::string> a = {g_self.string(), "--rise-sim-worker"};
	a.insert(a.end(), rest.begin(), rest.end());
	return a;
}

void TestGpuLock(const fs::path& root)
{
	std::printf("G: GPU lock across processes, adoption of a waiting worker, GPU-class queue\n");
	const fs::path dir = PrivateDir(root, "gpu");
	const fs::path lock = dir/"private-gpu.lock";
	WriteText(dir/"params.json", "{\"frames\": 8, \"cells\": 8, \"pace_ms\": 120}");
	const fs::path c1 = dir/"one", c2 = dir/"two", c3 = dir/"three";
	std::uint64_t p1 = 0, p2 = 0, p3 = 0;
	std::string error;
	Check(SpawnDetachedProcess(WorkerArgs({"bake", FakeGpuSolver::Id, (dir/"params.json").string(), c1.string(),
		"--gpu-lock", lock.string(), "--checkpoint-seconds", "0"}), dir/"one.log", p1, error), "start worker one", error);
	g_pids.insert(p1);
	// Two waits on the lock, so they both start after one holds it.
	Check(WaitFor([&] { WorkerStatus s; std::string e; return ReadWorkerStatus(c1, s, e) && s.state == "running"; }, 10),
		"worker one runs (it holds the GPU lock)");
	Check(SpawnDetachedProcess(WorkerArgs({"bake", FakeGpuSolver::Id, (dir/"params.json").string(), c2.string(),
		"--gpu-lock", lock.string(), "--checkpoint-seconds", "0"}), dir/"two.log", p2, error), "start worker two", error);
	Check(SpawnDetachedProcess(WorkerArgs({"bake", FakeGpuSolver::Id, (dir/"params.json").string(), c3.string(),
		"--gpu-lock", lock.string(), "--checkpoint-seconds", "0"}), dir/"three.log", p3, error), "start worker three", error);
	g_pids.insert(p2);
	g_pids.insert(p3);
	WorkerStatus s2;
	Check(WaitFor([&] { std::string e; return ReadWorkerStatus(c2, s2, e) && s2.state == "waiting-gpu"; }, 10) &&
		s2.detail.find("held by pid " + std::to_string(p1)) != std::string::npos,
		"worker two waits for the GPU, naming the holder", s2.state + " / " + s2.detail);
	// A second manager adopts the three bakes from their caches (as an app would at load).
	SimulationBakeManager m(Settings(root));
	BakeSceneSnapshot snap;
	snap.scene = dir/"gpu.RISEscene";   // adoption reads caches only
	for (const auto& c : {std::make_pair("one", c1), std::make_pair("two", c2), std::make_pair("three", c3)}) {
		SceneSimulationStatus s;
		s.name = c.first;
		s.solverId = FakeGpuSolver::Id;
		s.backend = "test-gpu";
		s.cacheDir = c.second;
		s.state = SceneSimulationState::Baking;
		snap.simulations.push_back(s);
	}
	Check(m.Attach(snap, error), "attach", error);
	const std::string key = SimulationBakeManager::SceneKey(snap.scene);
	BakeJob j1, j2, j3;
	Check(m.LatestJob(key, "one", j1) && m.LatestJob(key, "two", j2) && m.LatestJob(key, "three", j3) &&
		j1.adopted && j2.adopted && j1.pid == p1 && j2.pid == p2 && j1.state == BakeJobState::Running &&
		j2.state == BakeJobState::WaitingForGpu && j3.state == BakeJobState::WaitingForGpu,
		"adopted: one running, two and three waiting for the GPU", Describe(j1) + " | " + Describe(j2) + " | " + Describe(j3));
	// Stop three while it waits: it ends stopped (nothing ran, so nothing to checkpoint).
	Check(m.Stop(j3.id, false, error), "stop a waiting worker", error);
	j3 = WaitJob(m, j3.id, 10);
	Check(j3.state == BakeJobState::Stopped && !j3.interrupted, "a worker stopped while waiting for the GPU ends stopped",
		Describe(j3));
	// Exclusivity: two is never running while one runs.
	bool overlap = false;
	j1 = j2 = BakeJob();
	WaitFor([&] {
		m.Job(j1.id ? j1.id : 0, j1);
		BakeJob a, b;
		m.LatestJob(key, "one", a);
		m.LatestJob(key, "two", b);
		if (a.state == BakeJobState::Running && b.state == BakeJobState::Running) overlap = true;
		j1 = a; j2 = b;
		return BakeJobTerminal(a.state) && BakeJobTerminal(b.state);
	}, 30);
	Check(!overlap && j1.state == BakeJobState::Finished && j2.state == BakeJobState::Finished,
		"one finishes, then two runs and finishes; never both at once", Describe(j1) + " | " + Describe(j2));
	{
		WorkerStatus a, b;
		std::string e;
		Check(ReadWorkerStatus(c1, a, e) && ReadWorkerStatus(c2, b, e) && a.state == "finished" && b.state == "finished" &&
			b.wallSeconds > 0, "both caches report finished");
		std::string why;
		Check(InspectCache(c1, std::size_t(64) << 20, why) == CacheState::Complete &&
			InspectCache(c2, std::size_t(64) << 20, why) == CacheState::Complete, "both caches sealed");
	}

	// This manager runs one GPU-class job at a time (two cloth sims, here declared GPU).
	const fs::path sceneDir = PrivateDir(root, "gpuqueue");
	const fs::path scene = sceneDir/"pair.RISEscene";
	WriteText(scene, std::string(Header) + Camera() + Cloth("left", 5, Pace(50)) + Consumer("geo_left", "left.surface") +
		Cloth("right", 5, Pace(50), "0.6 1 0.5") + Consumer("geo_right", "right.surface"));
	BakeSceneSnapshot pair = Snapshot(scene);
	for (auto& s : pair.simulations) s.backend = "test-gpu";
	BakeRequest all;
	all.all = true;
	std::vector<std::uint64_t> ids;
	Check(m.RequestBake(pair, all, ids, error) && ids.size() == 2, "bake both", error);
	if (ids.size() == 2) {
		BakeJob second;
		Check(WaitFor([&] { m.Job(ids[1], second); return second.waitingFor.find("GPU slot") != std::string::npos; }, 5),
			"the second GPU-class job waits for this manager's GPU slot", Describe(second));
		const BakeJob a = WaitJob(m, ids[0], 30), b = WaitJob(m, ids[1], 30);
		Check(a.state == BakeJobState::Finished && b.state == BakeJobState::Finished, "both GPU-class jobs finish",
			Describe(a) + " | " + Describe(b));
	}
	TrackJobs(m);
}

// ---------------------------------------------------------------- S

void TestStopResume(const fs::path& root)
{
	std::printf("S: stop -> resumable -> resume equals an uninterrupted bake; change notification\n");
	const std::string text = std::string(Header) + Camera() + Cloth("flag", 12, Pace(70)) + Consumer("geo_flag", "flag.surface");
	const fs::path a = PrivateDir(root, "stop"), b = PrivateDir(root, "straight");
	WriteText(a/"flag.RISEscene", text);
	WriteText(b/"flag.RISEscene", text);
	SimulationBakeManager m(Settings(root));
	std::atomic<int> landed{0};
	std::atomic<std::uint64_t> landedFrames{0};
	const auto listener = m.AddListener([&](const BakeEvent& e) {
		if (e.kind == BakeEvent::Kind::FramesLanded && e.job.simulation == "flag") {
			++landed;
			landedFrames = e.job.framesWritten;
		}
	});
	const std::uint64_t counter0 = m.ChangeCounter();
	const std::string keyA = SimulationBakeManager::SceneKey(a/"flag.RISEscene");
	const std::uint64_t generation0 = m.FramesGeneration(keyA);
	BakeRequest request;
	request.simulations = {"flag"};
	std::vector<std::uint64_t> ids;
	std::string error;
	Check(m.RequestBake(Snapshot(a/"flag.RISEscene"), request, ids, error) && ids.size() == 1, "bake flag", error);
	if (ids.empty()) { m.RemoveListener(listener); return; }
	BakeJob j;
	Check(WaitFor([&] { m.Job(ids[0], j); return j.state == BakeJobState::Running && j.framesWritten >= 3; }, 20),
		"the bake runs and writes frames", Describe(j));
	Check(landed.load() >= 1 && landedFrames.load() >= 1 && m.FramesGeneration(keyA) > generation0 && m.ChangeCounter() > counter0,
		"frames-landed notifications fire while frames land (listener, frames generation, change counter)");
	Check(j.pid != 0 && j.etaSeconds >= 0 && j.endTime > 0, "the job reports its pid, an ETA and the end time", Describe(j));
	Check(m.Stop(ids[0], false, error), "stop", error);
	m.Job(ids[0], j);
	Check(j.state == BakeJobState::Stopping || j.state == BakeJobState::Stopped, "stopping", Describe(j));
	j = WaitJob(m, ids[0], 20);
	Check(j.state == BakeJobState::Stopped && j.resumable && !j.interrupted && j.exitCode == WorkerStopped &&
		j.framesWritten > 0 && j.framesWritten < 12, "stopped at a checkpoint, resumable", Describe(j));
	{
		const BakeSceneSnapshot snap = Snapshot(a/"flag.RISEscene");
		const auto* s = Find(snap, "flag");
		Check(s && s->state == SceneSimulationState::Partial, "the cache is partial with the current key");
		std::uint64_t resumed = 0;
		Check(m.Resume(snap, "flag", resumed, error), "resume", error);
		const BakeJob r = WaitJob(m, resumed, 30);
		Check(r.state == BakeJobState::Finished && r.resume && r.framesWritten == 12, "the resume finishes", Describe(r));
	}
	m.RemoveListener(listener);
	// The uninterrupted bake of the same scene in its own directory.
	Check(m.RequestBake(Snapshot(b/"flag.RISEscene"), request, ids, error) && ids.size() == 1, "uninterrupted bake", error);
	if (!ids.empty()) {
		const BakeJob u = WaitJob(m, ids[0], 30);
		Check(u.state == BakeJobState::Finished, "the uninterrupted bake finishes", Describe(u));
	}
	const auto sa = Sealed(a/"caches"/"flag"), sb = Sealed(b/"caches"/"flag");
	Check(sa.size() > 12 && sa == sb, "stopped and resumed equals uninterrupted, byte for byte (manifest, frames, statics)",
		std::to_string(sa.size()) + " vs " + std::to_string(sb.size()) + " files");
	TrackJobs(m);
}

// ---------------------------------------------------------------- A

void TestAdoption(const fs::path& root)
{
	std::printf("A: adoption after the manager is destroyed and recreated\n");
	const fs::path dir = PrivateDir(root, "adopt");
	const fs::path scene = dir/"flag.RISEscene";
	WriteText(scene, std::string(Header) + Camera() + Cloth("flag", 10, Pace(90)) + Consumer("geo_flag", "flag.surface"));
	std::uint64_t pid = 0;
	{
		std::unique_ptr<SimulationBakeManager> first(new SimulationBakeManager(Settings(root)));
		BakeRequest request;
		request.simulations = {"flag"};
		std::vector<std::uint64_t> ids;
		std::string error;
		Check(first->RequestBake(Snapshot(scene), request, ids, error) && ids.size() == 1, "bake", error);
		BakeJob j;
		Check(!ids.empty() && WaitFor([&] { first->Job(ids[0], j); return j.framesWritten >= 2; }, 20), "it runs", Describe(j));
		pid = j.pid;
		TrackJobs(*first);
	}   // the manager goes; the worker does not
	Check(pid != 0 && ProcessAlive(pid, 0), "the worker outlives its manager");
	SimulationBakeManager second(Settings(root));
	std::string error;
	Check(second.Attach(Snapshot(scene), error), "a new manager attaches the scene", error);
	BakeJob j;
	const std::string key = SimulationBakeManager::SceneKey(scene);
	Check(second.LatestJob(key, "flag", j) && j.adopted && j.pid == pid &&
		(j.state == BakeJobState::Running || j.state == BakeJobState::Finished), "the running bake is adopted with its pid",
		Describe(j));
	// Attach again: no second row.
	second.Attach(Snapshot(scene), error);
	Check(second.Jobs(key).size() == 1, "attaching again adds no job");
	j = WaitJob(second, j.id, 30);
	Check(j.state == BakeJobState::Finished && j.framesWritten == 10, "the adopted bake is followed to the end", Describe(j));
	std::string why;
	Check(InspectCache(dir/"caches"/"flag", std::size_t(64) << 20, why) == CacheState::Complete, "sealed");
}

// ---------------------------------------------------------------- P

bool WriteStatusRecord(const fs::path& cache, const std::string& state, std::uint64_t pid, std::uint64_t start,
	const std::string& host)
{
	Bytes bytes;
	std::string error;
	return RISECBOR64::Encode(V::MapValue({{"format", V::String("rise.sim.status.v1")}, {"state", V::String(state)},
		{"solver", V::String("cloth.shell")}, {"simulated", V::Float(0.1)}, {"end", V::Float(0.3)},
		{"frames", V::Unsigned(2)}, {"total", V::Unsigned(8)}, {"wall", V::Float(1.0)}, {"error", V::String("")},
		{"detail", V::String("")}, {"pid", V::Unsigned(pid)}, {"pid_start", V::Unsigned(start)},
		{"host", V::String(host)}}), bytes, &error) && ReplaceFileDurably(cache, "rise-sim.status.cbor", bytes, error);
}

void TestStalePid(const fs::path& root)
{
	std::printf("P: a stale pid is never adopted\n");
	const fs::path dir = PrivateDir(root, "stale");
	const fs::path scene = dir/"flag.RISEscene";
	WriteText(scene, std::string(Header) + Camera() + Cloth("flag", 8, Pace(60)) + Consumer("geo_flag", "flag.surface"));
	const fs::path cache = dir/"caches"/"flag";
	{
		// A partial cache: bake and stop.
		SimulationBakeManager m(Settings(root));
		BakeRequest request;
		request.simulations = {"flag"};
		std::vector<std::uint64_t> ids;
		std::string error;
		Check(m.RequestBake(Snapshot(scene), request, ids, error) && ids.size() == 1, "bake", error);
		BakeJob j;
		if (!ids.empty()) {
			WaitFor([&] { m.Job(ids[0], j); return j.framesWritten >= 2; }, 20);
			m.Stop(ids[0], false, error);
			j = WaitJob(m, ids[0], 20);
		}
		Check(j.state == BakeJobState::Stopped, "a partial cache to test with", Describe(j));
		TrackJobs(m);
	}
	const std::string host = CurrentHostName();
	const std::uint64_t self = CurrentProcessId(), selfStart = ProcessStartStamp(self);
	// A dead process with its real start stamp.
	std::uint64_t dead = 0, deadStart = 0;
	{
		std::string error;
		Check(SpawnDetachedProcess(WorkerArgs({"status", cache.string()}), dir/"dead.log", dead, error), "a short process", error);
		deadStart = ProcessStartStamp(dead);
		int code = 0;
		bool notChild = false;
		WaitFor([&] { return ReapChildProcess(dead, code, notChild); }, 10);
	}
	struct Case { const char* what; std::uint64_t pid, start; };
	const Case cases[] = {
		{"a live process whose start stamp is not the record's (a recycled pid)", self, selfStart + 1},
		{"a process that is gone", dead, deadStart},
		{"a live process with the right stamp that does not hold the cache lock", self, selfStart},
	};
	for (const Case& c : cases) {
		Check(WriteStatusRecord(cache, "running", c.pid, c.start, host), "write a stale status record");
		SimulationBakeManager m(Settings(root));
		std::string error;
		m.Attach(Snapshot(scene), error);
		BakeJob j;
		const bool found = m.LatestJob(SimulationBakeManager::SceneKey(scene), "flag", j);
		Check(found && j.state == BakeJobState::Stopped && j.interrupted && j.resumable && j.pid == 0,
			std::string("not adopted, reported interrupted and resumable: ") + c.what, found ? Describe(j) : "no job");
		// Stop has nothing to signal.
		Check(!m.Stop(j.id, false, error), "an interrupted job cannot be stopped (nothing runs)");
	}
	// Resume the interrupted bake.
	SimulationBakeManager m(Settings(root));
	std::uint64_t resumed = 0;
	std::string error;
	Check(m.Resume(Snapshot(scene), "flag", resumed, error), "resume the interrupted bake", error);
	const BakeJob r = WaitJob(m, resumed, 30);
	Check(r.state == BakeJobState::Finished && r.framesWritten == 8, "it finishes", Describe(r));
}

// ---------------------------------------------------------------- F

void TestSaveRule(const fs::path& root)
{
	std::printf("F: the save rule\n");
	const fs::path dir = PrivateDir(root, "save");
	SimulationBakeManager m(Settings(root));
	BakeRequest request;
	request.simulations = {"flag"};
	std::vector<std::uint64_t> ids;
	std::string error;
	{
		BakeSceneSnapshot none;
		Check(!m.RequestBake(none, request, ids, error) && error.find("save the scene first") != std::string::npos,
			"a document without a file cannot bake", error);
	}
	const fs::path scene = dir/"flag.RISEscene";
	WriteText(scene, std::string(Header) + Camera() + Cloth("flag", 4, "") + Consumer("geo_flag", "flag.surface"));
	{
		// An unsaved edit that changes the key: the worker refuses before touching the cache.
		Scene s;
		Check(Load(scene, s), "load");
		Check(s.job->ApplyCstParamEdit("flag", "", "substeps", 0, "5") != 0 || true, "edit in memory");
		BakeSceneSnapshot snap;
		Check(Agent::AgentSimulationSnapshot(*s.job, nullptr, snap, error) && snap.unsavedChanges, "the snapshot sees unsaved changes",
			error);
		const auto* flag = Find(snap, "flag");
		const BakeSceneSnapshot disk = Snapshot(scene);
		const auto* onDisk = Find(disk, "flag");
		Check(flag && onDisk && !flag->expectedKey.empty() && flag->expectedKey != onDisk->expectedKey,
			"the edit changes the key the document expects");
		Check(m.RequestBake(snap, request, ids, error) && ids.size() == 1, "the request is queued (its key is known)", error);
		if (!ids.empty()) {
			const BakeJob j = WaitJob(m, ids[0], 20);
			Check(j.state == BakeJobState::Failed && j.lastError.rfind("save first", 0) == 0 &&
				j.lastError.find(kWorkerSceneMismatch) != std::string::npos, "the worker refuses: save first", Describe(j));
		}
		Check(!fs::exists(dir/"caches"/"flag"), "the cache was not touched");
	}
	{
		// A clean document bakes.
		Check(m.RequestBake(Snapshot(scene), request, ids, error) && ids.size() == 1, "clean: queued", error);
		if (!ids.empty()) {
			const BakeJob j = WaitJob(m, ids[0], 20);
			Check(j.state == BakeJobState::Finished, "clean: baked", Describe(j));
		}
	}
	{
		// Unsaved changes and a key not known yet (the upstream is not baked): refused at once.
		const fs::path chain = dir/"chain.RISEscene";
		WriteText(chain, std::string(Header) + Camera() + Cloth("upper", 4, "") + Consumer("geo_upper", "upper.surface") +
			Cloth("lower", 4, "\tcollider_object\tobj_geo_upper\n", "-0.5 1.4 0.5") + Consumer("geo_lower", "lower.surface"));
		Scene s;
		Check(Load(chain, s), "load chain");
		s.job->ApplyCstParamEdit("lower", "", "substeps", 0, "5");
		BakeSceneSnapshot snap;
		Agent::AgentSimulationSnapshot(*s.job, nullptr, snap, error);
		BakeRequest lower;
		lower.simulations = {"lower"};
		Check(snap.unsavedChanges && !m.RequestBake(snap, lower, ids, error) &&
			error.find("save the scene first") != std::string::npos && error.find("lower") != std::string::npos,
			"unsaved changes with a key not known yet: save first, at once", error);
		Check(!fs::exists(dir/"caches"/"upper"), "nothing was started");
	}
	TrackJobs(m);
}

// ---------------------------------------------------------------- V

using Agent::JsonValue;

JsonValue Call(Agent::AgentRpcDispatcher& rpc, const std::string& method, const JsonValue& params, bool* rpcError = nullptr)
{
	static double id = 1000;
	JsonValue req = JsonValue::MakeObject();
	req.set("jsonrpc", JsonValue::MakeString("2.0"));
	req.set("id", JsonValue::MakeNumber(++id));
	req.set("method", JsonValue::MakeString(method));
	req.set("params", params);
	JsonValue env;
	std::string error;
	Agent::JsonParse(rpc.HandleLine(Agent::JsonSerialize(req)), env, error);
	if (rpcError) *rpcError = env.has("error");
	return env.has("error") ? env.get("error") : env.get("result");
}

JsonValue Obj(std::initializer_list<std::pair<const char*, JsonValue>> members)
{
	JsonValue o = JsonValue::MakeObject();
	for (const auto& m : members) o.set(m.first, m.second);
	return o;
}

std::string State(const JsonValue& list, const std::string& name)
{
	const JsonValue& sims = list.get("simulations");
	for (std::size_t i = 0; i < sims.size(); ++i)
		if (sims.at(i).get("name").asString() == name) return sims.at(i).get("state").asString();
	return "?";
}

void TestAgentVerbs(const fs::path& root)
{
	std::printf("V: agent verbs end to end\n");
	SimulationBakeManager::Instance().SetSettings(Settings(root));
	const fs::path dir = PrivateDir(root, "agent");
	const fs::path scene = dir/"flag.RISEscene";
	WriteText(scene, std::string(Header) + Camera() + Cloth("flag", 6, "") + Consumer("geo_flag", "flag.surface") +
		Cloth("slow", 12, Pace(80), "0.6 1 0.5") + Consumer("geo_slow", "slow.surface"));
	std::unique_ptr<Agent::AgentSession> session = Agent::AgentSession::LoadFromFile(scene.string());
	Check(session != nullptr, "the agent session loads the scene");
	if (!session) return;
	Agent::AgentRpcDispatcher rpc(std::move(session), Agent::AgentAutonomy::Commit);
	using J = JsonValue;
	JsonValue list = Call(rpc, "sim_list", J::MakeObject());
	Check(list.get("ok").asBool() && list.get("simulations").size() == 2 && State(list, "flag") == "missing" &&
		!list.get("liveSupported").asBool() && !list.get("unsavedChanges").asBool(), "sim_list: two missing simulations, headless",
		Agent::JsonSerialize(list));
	JsonValue bake = Call(rpc, "sim_bake", Obj({{"simulation", J::MakeString("flag")}}));
	Check(bake.get("ok").asBool() && bake.get("jobId").asNumber() > 0, "sim_bake returns a job id", Agent::JsonSerialize(bake));
	const double flagJob = bake.get("jobId").asNumber();
	JsonValue wait;
	for (int i = 0; i < 6; ++i) {
		wait = Call(rpc, "sim_wait", Obj({{"jobId", J::MakeNumber(flagJob)}, {"timeoutMs", J::MakeNumber(10000)}}));
		if (wait.get("completed").asBool()) break;
	}
	Check(wait.get("completed").asBool() && wait.get("job").get("state").asString() == "finished" &&
		wait.get("job").get("framesWritten").asNumber() == 6, "sim_wait: finished, 6 frames", Agent::JsonSerialize(wait));
	JsonValue status = Call(rpc, "sim_status", Obj({{"jobId", J::MakeNumber(flagJob)}}));
	Check(status.get("found").asBool() && status.get("job").get("state").asString() == "finished" &&
		status.get("job").get("progress").asNumber() == 1.0, "sim_status: finished", Agent::JsonSerialize(status));
	list = Call(rpc, "sim_list", J::MakeObject());
	{
		const JsonValue& sims = list.get("simulations");
		bool ok = false;
		for (std::size_t i = 0; i < sims.size(); ++i)
			if (sims.at(i).get("name").asString() == "flag")
				ok = sims.at(i).get("state").asString() == "complete" && sims.at(i).get("keyStatus").asString() == "current" &&
					sims.at(i).get("job").get("state").asString() == "finished";
		Check(ok, "sim_list: flag complete, key current, its job finished", Agent::JsonSerialize(list));
	}
	// Cancel mid-bake, then resume through sim_bake.
	bake = Call(rpc, "sim_bake", Obj({{"simulation", J::MakeString("slow")}}));
	const double slowJob = bake.get("jobId").asNumber();
	Check(bake.get("ok").asBool() && slowJob > 0, "sim_bake slow", Agent::JsonSerialize(bake));
	Check(WaitFor([&] {
		status = Call(rpc, "sim_status", Obj({{"jobId", J::MakeNumber(slowJob)}}));
		return status.get("job").get("framesWritten").asNumber() >= 2;
	}, 20), "the slow bake writes frames", Agent::JsonSerialize(status));
	{
		wait = Call(rpc, "sim_wait", Obj({{"jobId", J::MakeNumber(slowJob)}, {"timeoutMs", J::MakeNumber(-5)}}));
		Check(!wait.get("completed").asBool() && wait.get("found").asBool(), "sim_wait timeoutMs clamps to 0 (a poll)");
	}
	JsonValue cancel = Call(rpc, "sim_cancel", Obj({{"jobId", J::MakeNumber(slowJob)}}));
	Check(cancel.get("cancelled").asBool(), "sim_cancel accepted", Agent::JsonSerialize(cancel));
	for (int i = 0; i < 3; ++i) {
		wait = Call(rpc, "sim_wait", Obj({{"jobId", J::MakeNumber(slowJob)}, {"timeoutMs", J::MakeNumber(10000)}}));
		if (wait.get("completed").asBool()) break;
	}
	Check(wait.get("job").get("state").asString() == "stopped" && wait.get("job").get("resumable").asBool() &&
		wait.get("job").get("framesWritten").asNumber() < 12, "cancelled mid-bake: stopped, resumable", Agent::JsonSerialize(wait));
	list = Call(rpc, "sim_list", J::MakeObject());
	Check(State(list, "slow") == "partial", "sim_list: slow is partial");
	bake = Call(rpc, "sim_bake", Obj({{"simulation", J::MakeString("slow")}}));
	const double resumeJob = bake.get("jobId").asNumber();
	Check(bake.get("ok").asBool() && resumeJob > slowJob, "sim_bake continues the partial cache", Agent::JsonSerialize(bake));
	for (int i = 0; i < 6; ++i) {
		wait = Call(rpc, "sim_wait", Obj({{"jobId", J::MakeNumber(resumeJob)}, {"timeoutMs", J::MakeNumber(10000)}}));
		if (wait.get("completed").asBool()) break;
	}
	Check(wait.get("job").get("state").asString() == "finished" && wait.get("job").get("framesWritten").asNumber() == 12,
		"the resumed bake finishes", Agent::JsonSerialize(wait));
	list = Call(rpc, "sim_list", J::MakeObject());
	Check(State(list, "slow") == "complete", "sim_list: slow complete");
	// Params, live verbs headless, unknown names.
	bool rpcError = false;
	Call(rpc, "sim_bake", J::MakeObject(), &rpcError);
	Check(rpcError, "sim_bake without simulation or all is an invalid-params error");
	Call(rpc, "sim_wait", J::MakeObject(), &rpcError);
	Check(rpcError, "sim_wait needs a jobId");
	JsonValue unknown = Call(rpc, "sim_bake", Obj({{"simulation", J::MakeString("nope")}}));
	Check(!unknown.get("ok").asBool() && unknown.get("message").asString().find("no simulation named") != std::string::npos,
		"an unknown simulation is an ok:false refusal", Agent::JsonSerialize(unknown));
	JsonValue live = Call(rpc, "sim_live_start", Obj({{"simulation", J::MakeString("flag")}}));
	Check(!live.get("ok").asBool() && live.has("supported") && !live.get("supported").asBool(),
		"sim_live_start headless: supported false", Agent::JsonSerialize(live));
	JsonValue seek = Call(rpc, "sim_seek", Obj({{"time", J::MakeNumber(0.1)}}));
	Check(!seek.get("supported").asBool(), "sim_seek headless: supported false");
	status = Call(rpc, "sim_status", J::MakeObject());
	Check(status.get("jobs").size() >= 3, "sim_status without a jobId lists the scene's jobs");
	// Read autonomy: the reads work, a bake is refused.
	{
		std::unique_ptr<Agent::AgentSession> readOnly = Agent::AgentSession::LoadFromFile(scene.string());
		Agent::AgentRpcDispatcher reader(std::move(readOnly), Agent::AgentAutonomy::Read);
		JsonValue r = Call(reader, "sim_list", J::MakeObject(), &rpcError);
		Check(!rpcError && r.get("ok").asBool(), "sim_list works under read autonomy");
		r = Call(reader, "sim_bake", Obj({{"simulation", J::MakeString("flag")}}), &rpcError);
		Check(rpcError && r.get("code").asNumber() == -32011, "sim_bake is refused under read autonomy", Agent::JsonSerialize(r));
	}
	TrackJobs(SimulationBakeManager::Instance());
}

// ---------------------------------------------------------------- R

void TestControllerRefresh(const fs::path& root)
{
	std::printf("R: a live controller: C API bake, job rows, RefreshSimulationFrames\n");
	SimulationBakeManager::Instance().SetSettings(Settings(root));
	const fs::path dir = PrivateDir(root, "controller");
	const fs::path path = dir/"view.RISEscene";
	WriteText(path, std::string(Header) + "film\n{\n\twidth 160\n\theight 90\n}\n\n" + Camera() +
		Cloth("flag", 10, Pace(80)) + Consumer("geo_flag", "flag.surface"));
	IJobPriv* job = nullptr;
	IRasterizer* rasterizer = nullptr;
	IRayCaster* preview = nullptr;
	IRayCaster* polish = nullptr;
	if (!RISE_CreateJobPriv(&job) || !job || !job->LoadAsciiSceneViaCst(path.string().c_str()) ||
		!Implementation::CreateInteractiveMaterialPreviewPipeline(&rasterizer, &preview, &polish)) {
		Check(false, "a live controller over the scene");
		if (job) job->release();
		return;
	}
	SceneEditController* ctrl = new SceneEditController(*job, rasterizer);
	ctrl->Start();
	auto* mesh = dynamic_cast<Implementation::SimulatedMeshGeometry*>(job->GetGeometries()->GetItem("geo_flag"));
	Check(mesh && !mesh->HasSource(), "no cache: the consumer is unbound");
	char error[512] = {};
	bool refreshed = true;
	Check(RISE_API_SceneEditController_RefreshSimulationFrames(ctrl, &refreshed, error, sizeof error) && !refreshed,
		"nothing landed: the refresh does nothing");
	const unsigned long long counter0 = RISE_API_SimulationBake_ChangeCounter();
	unsigned long long jobId = 0;
	Check(RISE_API_SceneEditController_SimulationBake(ctrl, "flag", false, &jobId, error, sizeof error) && jobId != 0,
		"C API bake", error);
	auto row = [&](int& state, unsigned long long& frames) {
		unsigned int count = 0;
		state = -1;
		frames = 0;
		if (!RISE_API_SceneEditController_GetSimulationBakeJobCount(ctrl, &count)) return false;
		for (unsigned int i = 0; i < count; ++i) {
			unsigned long long id = 0, pid = 0;
			char name[64] = {};
			if (RISE_API_SceneEditController_GetSimulationBakeJob(ctrl, i, &id, name, sizeof name, &state, nullptr, nullptr,
				nullptr, nullptr, nullptr, &frames, nullptr, nullptr, nullptr, &pid, nullptr, 0, nullptr, 0, nullptr, 0,
				nullptr, 0) && id == jobId) {
				if (pid) g_pids.insert(pid);
				return true;
			}
		}
		return false;
	};
	int state = -1;
	unsigned long long frames = 0;
	Check(WaitFor([&] { return row(state, frames) && frames >= 2; }, 20), "the job row shows frames landing",
		std::to_string(state) + " " + std::to_string(frames));
	Check(RISE_API_SimulationBake_ChangeCounter() > counter0, "the change counter moved");
	Check(WaitFor([&] {
		RISE_API_SceneEditController_RefreshSimulationFrames(ctrl, &refreshed, error, sizeof error);
		return refreshed;
	}, 5), "frames landed: the refresh runs", error);
	Check(mesh->HasSource() && !mesh->Hidden(), "the unbound consumer is bound to the partial cache and shown");
	Check(RISE_API_SceneEditController_RefreshSimulationFrames(ctrl, &refreshed, error, sizeof error) && !refreshed,
		"no new frames since: the refresh does nothing");
	BakeJob done = WaitJob(SimulationBakeManager::Instance(), jobId, 30);
	Check(done.state == BakeJobState::Finished, "the controller's bake finishes", Describe(done));
	Check(row(state, frames) && state == 5 && frames == 10, "the row reads finished (5), 10 frames");
	// A rebake is a new epoch: the bound consumer is rebound to it.
	const std::string epoch0 = mesh->Source().Description().epoch;
	Check(RISE_API_SceneEditController_SimulationBake(ctrl, "flag", true, &jobId, error, sizeof error), "force rebake", error);
	done = WaitJob(SimulationBakeManager::Instance(), jobId, 30);
	Check(done.state == BakeJobState::Finished, "the rebake finishes", Describe(done));
	RISE_API_SceneEditController_RefreshSimulationFrames(ctrl, &refreshed, error, sizeof error);
	CacheSnapshot disk;
	std::string why;
	Check(refreshed && CacheSnapshot::Open(dir/"caches"/"flag", std::size_t(64) << 20, disk, why) &&
		mesh->HasSource() && mesh->Source().Description().epoch == disk.Description().epoch &&
		disk.Description().epoch != epoch0, "after a rebake the consumer is rebound to the new epoch", error);
	// Stopping and resuming through the C API.
	Check(RISE_API_SceneEditController_SimulationBake(ctrl, "flag", true, &jobId, error, sizeof error), "rebake again", error);
	Check(WaitFor([&] { return row(state, frames) && frames >= 2; }, 20), "it runs");
	Check(RISE_API_SceneEditController_SimulationBakeStop(ctrl, "flag", false, error, sizeof error), "C API stop", error);
	done = WaitJob(SimulationBakeManager::Instance(), jobId, 20);
	Check(done.state == BakeJobState::Stopped && done.resumable, "stopped, resumable", Describe(done));
	Check(RISE_API_SceneEditController_SimulationBakeResume(ctrl, "flag", &jobId, error, sizeof error), "C API resume", error);
	done = WaitJob(SimulationBakeManager::Instance(), jobId, 30);
	Check(done.state == BakeJobState::Finished && done.resume, "the resume finishes", Describe(done));
	char worker[1024] = {};
	Check(RISE_API_SimulationBake_GetWorkerPath(worker, sizeof worker, error, sizeof error) &&
		std::string(worker).find("--rise-sim-worker") != std::string::npos, "C API worker path", worker);
	ctrl->Stop();
	delete ctrl;
	rasterizer->release();
	preview->release();
	polish->release();
	job->release();
	TrackJobs(SimulationBakeManager::Instance());
}

} // namespace

int main(int argc, char** argv)
{
	if (argc >= 2 && std::string(argv[1]) == "--rise-sim-worker") {
		RegisterTestSolvers();
		return WorkerMain(argc - 1, argv + 1);
	}
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("SimulationBakeManagerTest (simulation M5)\n");
	g_self = CurrentExecutablePath();
	if (g_self.empty()) g_self = fs::absolute(argv[0]);
	RegisterTestSolvers();
	const fs::path root = RISE::TestSupport::ScratchRunDir("rise_bake_manager_", "suite");
	std::error_code ec;
	fs::create_directories(root, ec);
	fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ec);
	struct Teardown { ~Teardown() { KillAll(); } } teardown;
	std::atexit([] { KillAll(); });
	const auto began = Clock::now();
	TestLocate(root);
	TestQueueOrder(root);
	TestGpuLock(root);
	TestStopResume(root);
	TestAdoption(root);
	TestStalePid(root);
	TestSaveRule(root);
	TestAgentVerbs(root);
	TestControllerRefresh(root);
	KillAll();
	fs::remove_all(RISE::TestSupport::ScratchTestRoot("rise_bake_manager_"), ec);
	std::printf("\n%d passed, %d failed (%.1f s)\n", passes, failures,
		std::chrono::duration<double>(Clock::now() - began).count());
	return failures ? 1 : 0;
}
