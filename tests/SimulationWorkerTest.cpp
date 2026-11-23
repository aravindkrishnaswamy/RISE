// Worker host (rise-sim) through the library WorkerMain with the reference
// solvers registered: bake/resume/status for grid and mesh solvers, frames
// identical to the in-process host, stop/resume byte-identical to an
// uninterrupted bake, SIGINT stop at a safe point, SIGKILL and a crash cut
// between payload and manifest leaving the previous generation readable.
#include "../src/Library/Simulation/SimulationWorker.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "SimulationReferenceSolvers.h"
#include "TestTempDir.h"

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace RISE::Simulation;
using namespace RISE::Simulation::Reference;
namespace fs = std::filesystem;
constexpr std::size_t budget = std::size_t(64) << 20;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}

int Worker(const std::vector<std::string>& args, std::string* output = nullptr)
{
	std::ostringstream out, err;
	const int code = WorkerMain(args, out, err);
	if (output) *output = out.str() + err.str();
	return code;
}

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

// The sealed result: manifest bytes plus every payload file.
std::map<std::string, Bytes> Sealed(const fs::path& dir)
{
	std::map<std::string, Bytes> files;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(dir, ec)) {
		const auto name = e.path().filename().string();
		if (name == "manifest.cbor" || (name.size() > 6 && name.compare(name.size()-6, 6, ".frame") == 0))
			files[name] = ReadAll(e.path());
	}
	return files;
}

bool Identical(const fs::path& a, const fs::path& b)
{
	const auto x = Sealed(a), y = Sealed(b);
	return x.size() > 1 && x == y;
}

std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const IReadOnlyCacheSnapshot& s)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> out;
	for (const auto& e : s.Entries()) out[{e.index, e.output}] = e.digest;
	return out;
}

// Both solvers through the worker: sealed, predictive, frames identical to the in-process host.
void Conformance(const fs::path& root, const char* id, const std::string& json, const fs::path& reference)
{
	const auto params = root/(std::string(id) + ".json");
	WriteText(params, json);
	std::string log;
	Check(Worker({"bake", id, params.string(), reference.string(), "--checkpoint-seconds", "0"}, &log) == WorkerFinished,
		std::string("worker bakes ") + id, log);
	CacheSnapshot disk;
	std::string error;
	Check(CacheSnapshot::Open(reference, budget, disk, error) && disk.Complete() &&
		disk.Description().fidelity == "predictive" && disk.Description().producer == id,
		std::string("worker seals a predictive cache for ") + id, error);
	Check(!fs::exists(reference/"rise-sim.checkpoint"), "finished bake removes its checkpoint");
	WorkerStatus status;
	Check(ReadWorkerStatus(reference, status, error) && status.state == "finished" &&
		status.framesWritten == disk.Description().count && status.framesTotal == status.framesWritten,
		"status record reports finished", error);
	Check(Worker({"status", reference.string()}, &log) == WorkerFinished && log.find("cache: complete") != std::string::npos &&
		log.find("worker: finished") != std::string::npos, "status command", log);

	// In-process host with the same parameters publishes byte-identical frames.
	InProcessHost host(CreateSolver(id, error));
	NoSceneInputs inputs;
	V job;
	Check(RISE::RISECBOR64::DecodeCanonical(ReadAll(reference/"rise-sim.job.cbor"), job, &error) && job.Find("parameters"), "job record decodes", error);
	Check(host.Configure(*job.Find("parameters"), inputs, error) && host.Start(error) && host.StepToIndex(1 << 20, error) &&
		host.Description().cache.key == disk.Description().key && Digests(*host.Snapshot()) == Digests(disk),
		std::string("in-process and worker hosts publish identical frames for ") + id, error);
	Check(Worker({"bake", id, params.string(), reference.string()}, &log) == WorkerFinished &&
		log.find("already complete") != std::string::npos, "rebaking a complete matching cache is a no-op", log);
}

void StopResume(const fs::path& root, const fs::path& reference)
{
	const auto params = root/"test.membrane.json", dir = root/"stopped";
	std::string log, error;
	Check(Worker({"bake", "test.membrane", params.string(), dir.string(), "--stop-after-frames", "5"}, &log) == WorkerStopped,
		"worker stops after five frames", log);
	WorkerStatus status;
	Check(ReadWorkerStatus(dir, status, error) && status.state == "stopped" && status.framesWritten == 5 &&
		fs::exists(dir/"rise-sim.checkpoint") && InspectCache(dir, budget, error) == CacheState::Partial,
		"stopped bake leaves a checkpoint and a partial cache", error);
	Check(Worker({"bake", "test.membrane", params.string(), dir.string()}, &log) == WorkerFailed &&
		log.find("use resume") != std::string::npos, "bake refuses to clobber a partial cache", log);
	Check(Worker({"resume", dir.string(), "--stop-after-frames", "7"}, &log) == WorkerStopped &&
		ReadWorkerStatus(dir, status, error) && status.framesWritten == 12, "resume stops again", log);
	Check(Worker({"resume", dir.string()}, &log) == WorkerFinished, "resume finishes", log);
	Check(Identical(dir, reference), "stop and resume equals the uninterrupted bake byte for byte");
	Check(Worker({"resume", dir.string()}, &log) == WorkerFinished && log.find("already complete") != std::string::npos,
		"resuming a sealed cache is a no-op", log);

	// Canonical CBOR parameters give the same cache as JSON.
	const auto cbor = root/"membrane.cbor", viaCbor = root/"via-cbor";
	Bytes encoded;
	Check(RISE::RISECBOR64::Encode(V::MapValue({}), encoded, &error), "encode CBOR parameters", error);
	{ std::ofstream f(cbor, std::ios::binary); f.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size())); }
	Check(Worker({"bake", "test.membrane", cbor.string(), viaCbor.string()}, &log) == WorkerFinished &&
		Identical(viaCbor, reference), "canonical CBOR parameters bake the same cache", log);

	// --force rebakes as a new epoch with the same frames.
	CacheSnapshot before, after;
	Check(CacheSnapshot::Open(viaCbor, budget, before, error) &&
		Worker({"bake", "test.membrane", cbor.string(), viaCbor.string(), "--force"}, &log) == WorkerFinished &&
		CacheSnapshot::Open(viaCbor, budget, after, error) && after.Description().epoch != before.Description().epoch &&
		Digests(after) == Digests(before), "forced rebake starts a new epoch", log);
}

// Crash after a payload was published but before the manifest pointed at it.
void PayloadManifestCut(const fs::path& root, const fs::path& reference)
{
	const auto params = root/"test.membrane.json", dir = root/"cut";
	std::string log, error;
	Check(Worker({"bake", "test.membrane", params.string(), dir.string(), "--stop-after-frames", "4"}, &log) == WorkerStopped,
		"partial bake for the cut", log);
	CacheSnapshot full, before;
	Check(CacheSnapshot::Open(reference, budget, full, error) && CacheSnapshot::Open(dir, budget, before, error),
		"open caches", error);
	std::shared_ptr<const Bytes> next;
	Check(full.Read(4, "surface", budget, next, error) == FrameStatus::Ready, "next payload", error);
	{
		std::ofstream orphan(dir/(full.Entries()[4].digest + ".frame"), std::ios::binary);
		orphan.write(reinterpret_cast<const char*>(next->data()), static_cast<std::streamsize>(next->size()));
		std::ofstream torn(dir/".cache.pending", std::ios::binary);
		torn << "\xa4";
	}
	CacheSnapshot after;
	bool readable = CacheSnapshot::Open(dir, budget, after, error) && after.Digest() == before.Digest() &&
		after.Entries().size() == 4;
	for (const auto& e : after.Entries()) {
		std::shared_ptr<const Bytes> b;
		readable = readable && after.Read(e.index, e.output, budget, b, error) == FrameStatus::Ready;
	}
	std::shared_ptr<const Bytes> unpublished;
	Check(readable && after.Read(4, "surface", budget, unpublished, error) == FrameStatus::Unavailable,
		"previous generation stays readable and the orphan stays invisible", error);
	Check(Worker({"resume", dir.string()}, &log) == WorkerFinished && Identical(dir, reference),
		"resume over an orphaned payload and torn manifest seals the same cache", log);
}

void Refusals(const fs::path& root)
{
	std::string log;
	const auto params = root/"test.advection_diffusion.json";
	Check(Worker({}, &log) == WorkerUsage && Worker({"bake", "x"}, &log) == WorkerUsage &&
		Worker({"status", "a", "--bogus"}, &log) == WorkerUsage, "usage errors exit 2");
	Check(Worker({"bake", "no.such.solver", params.string(), (root/"u").string()}, &log) == WorkerFailed &&
		log.find("unknown solver") != std::string::npos, "unknown solver fails", log);
	const auto bad = root/"bad.json";
	WriteText(bad, "{\"frames\": 4, \"colour\": \"red\"}");
	Check(Worker({"bake", "test.advection_diffusion", bad.string(), (root/"bad").string()}, &log) == WorkerFailed &&
		log.find("unknown parameter") != std::string::npos, "unknown parameter fails", log);
	WriteText(bad, "[1,2]");
	Check(Worker({"bake", "test.advection_diffusion", bad.string(), (root/"bad").string()}, &log) == WorkerFailed,
		"non-map parameter record fails", log);
	Check(Worker({"resume", (root/"nothing-here").string()}, &log) == WorkerFailed, "resume without a job fails", log);

	// A job whose parameters were edited no longer reproduces the cache key.
	const auto dir = root/"tampered";
	Check(Worker({"bake", "test.advection_diffusion", params.string(), dir.string(), "--stop-after-frames", "2"}, &log) ==
		WorkerStopped, "partial grid bake", log);
	V job;
	std::string error;
	Check(RISE::RISECBOR64::DecodeCanonical(ReadAll(dir/"rise-sim.job.cbor"), job, &error), "read job", error);
	V::Members members = job.GetMap(), parameters = job.Find("parameters")->GetMap();
	for (auto& p : parameters) if (p.first == "diffusivity") p.second = V::Float(0.02);
	for (auto& m : members) if (m.first == "parameters") m.second = V::MapValue(parameters);
	Bytes tampered;
	Check(RISE::RISECBOR64::Encode(V::MapValue(members), tampered, &error) &&
		ReplaceFileDurably(dir, "rise-sim.job.cbor", tampered, error), "tamper with job", error);
	Check(Worker({"resume", dir.string()}, &log) == WorkerFailed && log.find("cache key") != std::string::npos,
		"resume verifies the cache key", log);
}

#ifndef _WIN32
pid_t Spawn(const std::vector<std::string>& args)
{
	std::cout.flush();
	const pid_t child = ::fork();
	if (child == 0) {
		std::ostringstream out, err;
		_exit(WorkerMain(args, out, err));
	}
	return child;
}

bool WaitFrames(const fs::path& dir, std::uint64_t frames)
{
	for (int i = 0; i < 2000; ++i) {
		WorkerStatus s;
		std::string ignored;
		if (ReadWorkerStatus(dir, s, ignored) && s.framesWritten >= frames) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

int ExitCode(pid_t child)
{
	int status = 0;
	if (::waitpid(child, &status, 0) != child) return -1;
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return -1;
}

// pace_ms slows the bake down without entering the cache key.
void Signals(const fs::path& root, const fs::path& reference)
{
	const auto paced = root/"paced.json";
	WriteText(paced, "{\"pace_ms\": 25}");
	std::string log, error;

	const auto interrupted = root/"sigint";
	pid_t child = Spawn({"bake", "test.membrane", paced.string(), interrupted.string()});
	Check(child > 0 && WaitFrames(interrupted, 3), "paced bake makes progress");
	::kill(child, SIGINT);
	Check(ExitCode(child) == WorkerStopped, "SIGINT stops the worker at a safe point with the stopped exit code");
	WorkerStatus status;
	Check(ReadWorkerStatus(interrupted, status, error) && status.state == "stopped" && status.framesWritten < 24 &&
		fs::exists(interrupted/"rise-sim.checkpoint"), "SIGINT writes a checkpoint and status", error);
	Check(Worker({"resume", interrupted.string()}, &log) == WorkerFinished && Identical(interrupted, reference),
		"resume after SIGINT equals the uninterrupted bake", log);

	const auto killed = root/"sigkill";
	child = Spawn({"bake", "test.membrane", paced.string(), killed.string(), "--checkpoint-seconds", "0.06"});
	Check(child > 0 && WaitFrames(killed, 5), "second paced bake makes progress");
	::kill(child, SIGKILL);
	Check(ExitCode(child) == 128 + SIGKILL, "worker killed");
	CacheSnapshot left;
	bool readable = CacheSnapshot::Open(killed, budget, left, error) && !left.Complete() && left.Entries().size() >= 5;
	for (const auto& e : left.Entries()) {
		std::shared_ptr<const Bytes> b;
		readable = readable && left.Read(e.index, e.output, budget, b, error) == FrameStatus::Ready;
	}
	Check(readable && InspectCache(killed, budget, error) == CacheState::Partial,
		"after SIGKILL the last committed generation is readable and the lock is free", error);
	Check(Worker({"status", killed.string()}, &log) == WorkerFinished && log.find("worker: interrupted") != std::string::npos,
		"status reports an interrupted worker", log);
	Check(Worker({"resume", killed.string()}, &log) == WorkerFinished && Identical(killed, reference),
		"resume after SIGKILL replays from the last checkpoint and equals the uninterrupted bake", log);
}
#endif
// Same probe as SimulationCacheTest: some Windows volumes cannot flush a
// directory, and the cache writer then refuses durable publication.
bool DiskPublicationAvailable(const fs::path& root)
{
#ifdef _WIN32
	CacheDescription d;
	d.key = std::string(64, '1'); d.epoch = std::string(64, '2'); d.producer = "probe"; d.producerVersion = "1";
	d.outputs = {{"fields", "grid.v1", {{"density", "1", "f32"}}}};
	CacheWriter probe;
	std::string error;
	if (!probe.Begin(root/"durability-probe", d, "", false, budget, error)) {
		Check(error == "durable cache publication unavailable: directory flush", "unexpected writer probe failure", error);
		std::cout << "SKIP: disk publication unavailable on this volume\n";
		return false;
	}
#endif
	std::error_code ec;
	return fs::create_directories(root, ec) || fs::is_directory(root, ec);
}
} // namespace

int main()
{
	RegisterReferenceSolvers();
	const fs::path root = RISE::TestSupport::ScratchRunDir("rise_sim_worker_", "suite");
	try {
		fs::create_directories(root);
		if (!DiskPublicationAvailable(root)) { fs::remove_all(root); return 0; }
		Conformance(root, "test.advection_diffusion", "{\"frames\": 12, \"cells\": 24}", root/"grid-reference");
		Conformance(root, "test.membrane", "{}", root/"mesh-reference");
		StopResume(root, root/"mesh-reference");
		PayloadManifestCut(root, root/"mesh-reference");
		Refusals(root);
#ifndef _WIN32
		Signals(root, root/"mesh-reference");
#endif
	} catch (const std::exception& e) {
		++failures;
		std::cerr << "exception: " << e.what() << "\n";
	}
	std::error_code ignored;
	fs::remove_all(root, ignored);
	if (failures) { std::cerr << failures << " worker check(s) failed\n"; return 1; }
	std::cout << "Simulation worker host checks passed\n";
	return 0;
}
