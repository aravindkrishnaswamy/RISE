// Worker host (rise-sim) through the library WorkerMain with the reference
// solvers registered: bake/resume/status for grid and mesh solvers, frames
// identical to the in-process host, stop/resume byte-identical to an
// uninterrupted bake, SIGINT stop at a safe point, SIGKILL and a crash cut
// between payload and manifest leaving the previous generation readable;
// static payloads written once; status end time = the last output's time.
// `--checkpoint-memory [MB]` (not run by default) measures the worker's peak
// memory around a checkpoint of a large opaque solver state.
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
#include <sys/resource.h>
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
		if (name == "manifest.cbor" || (name.size() > 6 && name.compare(name.size()-6, 6, ".frame") == 0) ||
			(name.size() > 7 && name.compare(name.size()-7, 7, ".static") == 0))
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
	const auto& dd = disk.Description();
	Check(ReadWorkerStatus(reference, status, error) && status.state == "finished" &&
		status.framesWritten == dd.count && status.framesTotal == status.framesWritten &&
		status.endTime == dd.startTime + static_cast<double>(dd.count - 1)*dd.interval &&
		status.simulatedTime == status.endTime, "status record reports finished at the last output's time", error);
	std::size_t statics = 0;
	for (const auto& e : fs::directory_iterator(reference)) if (e.path().extension() == ".static") ++statics;
	Check(statics == disk.Statics().size() && (std::string(id) != "test.membrane" || statics == 1),
		"a fixed-topology bake stores its topology once as a static payload");
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

// M7 prerequisites: streamed checkpoints.  The worker writes version 2 (the
// solver's CheckpointTo behind a header, digest in the trailer), refuses a
// corrupted or truncated one, and still resumes a version-1 file; through the
// in-process host the reference solvers' streams resume exactly as their
// in-memory checkpoints do.
void StreamedCheckpoints(const fs::path& root, const fs::path& reference)
{
	const auto params = root/"test.membrane.json";
	std::string log, error;
	// Version 2 on disk; integrity.
	const auto dir = root/"streamed";
	Check(Worker({"bake", "test.membrane", params.string(), dir.string(), "--stop-after-frames", "5"}, &log) == WorkerStopped,
		"streamed: partial bake", log);
	Bytes file;
	{
		std::ifstream f(dir/"rise-sim.checkpoint", std::ios::binary);
		file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}
	Check(file.size() > 100 && std::memcmp(file.data(), "RISESIMC", 8) == 0 && file[8] == 2 &&
		std::memcmp(file.data() + file.size() - 8, "RISESIME", 8) == 0, "the worker writes a streamed (version 2) checkpoint");
	const auto rewrite = [&](const Bytes& b) {
		std::ofstream f(dir/"rise-sim.checkpoint", std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
	};
	Bytes corrupt = file;
	corrupt[file.size()/2] ^= 0x40;
	rewrite(corrupt);
	Check(Worker({"resume", dir.string()}, &log) == WorkerFailed && log.find("digest mismatch") != std::string::npos,
		"a corrupted stream is refused before the solver reads it", log);
	Bytes truncated(file.begin(), file.end() - 40);
	rewrite(truncated);
	Check(Worker({"resume", dir.string()}, &log) == WorkerFailed, "a truncated stream is refused", log);
	rewrite(file);
	Check(Worker({"resume", dir.string()}, &log) == WorkerFinished && Identical(dir, reference),
		"the streamed checkpoint resumes to the uninterrupted bake byte for byte", log);

	// A version-1 checkpoint (what workers wrote before) still resumes.
	const auto old = root/"version1";
	Check(Worker({"bake", "test.membrane", params.string(), old.string(), "--stop-after-frames", "5"}, &log) == WorkerStopped,
		"version 1: partial bake", log);
	{
		InProcessHost host(CreateSolver("test.membrane", error));
		Bytes json;
		V parameters;
		{
			std::ifstream f(params, std::ios::binary);
			json.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		}
		InProcessHost::Checkpoint c;
		Check(ParseSolverParameters(json, budget, parameters, error) && host.Configure(parameters, EmptySceneInputs(), error) &&
			host.Start(error) && host.StepToIndex(4, error) && host.SaveCheckpoint(c, error), "in-memory checkpoint at frame 5", error);
		const SolverDescription& d = host.Description();
		Bytes header;
		Check(RISE::RISECBOR64::Encode(V::MapValue({{"key", V::String(d.cache.key)}, {"solver", V::String(d.solverId)},
			{"version", V::String(d.version)}, {"time", V::Float(c.position.time)}, {"next", V::Signed(c.position.nextIndex)},
			{"finished", V::Bool(c.position.finished)}, {"size", V::Unsigned(c.state.size())},
			{"digest", V::String(RISE::RISECBOR64::SHA256Hex(c.state))}}), header, &error), "version 1 header", error);
		Bytes v1{'R', 'I', 'S', 'E', 'S', 'I', 'M', 'C', 1, 0, 0, 0};
		for (unsigned k = 0; k < 8; ++k) v1.push_back(static_cast<std::uint8_t>(std::uint64_t(header.size()) >> (8*k)));
		v1.insert(v1.end(), header.begin(), header.end());
		v1.insert(v1.end(), c.state.begin(), c.state.end());
		std::ofstream f(old/"rise-sim.checkpoint", std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(v1.data()), static_cast<std::streamsize>(v1.size()));
	}
	Check(Worker({"resume", old.string()}, &log) == WorkerFinished && Identical(old, reference),
		"a version 1 checkpoint resumes to the uninterrupted bake byte for byte", log);

	// In process: both reference solvers, in-memory and streamed checkpoints
	// after six frames resume identically; the stream is a different
	// byte layout (the solvers' own CheckpointTo, not the default).
	for (const char* id : {"test.advection_diffusion", "test.membrane"}) {
		const V p = V::MapValue({});
		auto run = [&](InProcessHost& h) { return h.Configure(p, EmptySceneInputs(), error); };
		InProcessHost full(CreateSolver(id, error));
		Check(run(full) && full.Start(error) && full.StepToIndex(1000, error), "uninterrupted in-process run", error);
		InProcessHost first(CreateSolver(id, error));
		InProcessHost::Checkpoint memory;
		MemoryCheckpointSink sink;
		std::string key;
		SolverPosition position;
		Check(run(first) && first.Start(error) && first.StepToIndex(6, error) && first.SaveCheckpoint(memory, error) &&
			first.SaveCheckpointTo(sink, key, position, error), "both checkpoints", error);
		Check(sink.bytes != memory.state && sink.bytes.size() > 20 && std::memcmp(sink.bytes.data(), "RISEREFS", 8) == 0,
			"the stream is the solver's own layout");
		InProcessHost a(CreateSolver(id, error)), b(CreateSolver(id, error));
		MemoryCheckpointSource source(sink.bytes);
		Check(run(a) && a.RestoreCheckpoint(memory, error) && a.StepToIndex(1000, error), "in-memory resume", error);
		Check(run(b) && b.RestoreCheckpointFrom(source, key, position, error) && b.StepToIndex(1000, error), "streamed resume", error);
		// The resumed hosts hold the frames from the checkpoint on.
		auto ref = Digests(*full.Snapshot());
		for (auto it = ref.begin(); it != ref.end(); ) it = it->first.first < position.nextIndex ? ref.erase(it) : std::next(it);
		Check(!ref.empty() && Digests(*a.Snapshot()) == ref && Digests(*b.Snapshot()) == ref,
			std::string("in-memory and streamed resumes equal the uninterrupted run: ") + id);
		// A stream cut short is refused.
		Bytes cut(sink.bytes.begin(), sink.bytes.end() - 8);
		MemoryCheckpointSource shortSource(cut);
		InProcessHost c(CreateSolver(id, error));
		Check(run(c) && !c.RestoreCheckpointFrom(shortSource, key, position, error), "a truncated stream is refused", error);
	}
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

// ---------------------------------------------------------------- checkpoint memory (opt-in)

// An opaque state of `megabytes`, two outputs; Checkpoint hands over one copy.
class BallastSolver final : public ISimulationSolver {
public:
	bool Configure(const V& parameters, const ISceneInputs&, SolverDescription& d, std::string& error) override {
		const V* mb = parameters.Find("megabytes");
		if (!mb || mb->GetType() != V::UnsignedInteger) return Fail(error, "megabytes");
		size_ = static_cast<std::size_t>(mb->GetIntegerArgument()) << 20;
		const V* stream = parameters.Find("stream");
		stream_ = stream && stream->GetType() == V::Boolean && stream->GetBoolean();
		d = SolverDescription{};
		d.solverId = "test.ballast"; d.version = "1";
		d.effectiveParameters = parameters;
		d.cache.count = 2; d.cache.interval = 1;
		d.cache.outputs = {{"field", "grid.v1", {{"x", "1", "f32"}}}};
		d.capabilities.deterministic = d.capabilities.resumable = true;
		return true;
	}
	bool Start(std::string&) override {
		state_.assign(size_, 0);
		for (std::size_t i = 0; i < state_.size(); i += 4096) state_[i] = static_cast<std::uint8_t>(i >> 12);
		next_ = 0;
		return true;
	}
	bool Restore(const Bytes& b, std::string&) override { state_ = b; next_ = 1; return true; }
	bool Advance(const AdvanceRequest&, IFrameSink& sink, IPortExchange*, IProgress*, AdvanceResult& r, std::string& error) override {
		if (!sink.Publish(next_, "field", static_cast<double>(next_), std::make_shared<const Bytes>(Bytes{1}), {}, error)) return false;
		++next_;
		r.status = next_ == 2 ? AdvanceResult::Status::Finished : AdvanceResult::Status::Advanced;
		r.position = Position();
		return true;
	}
	bool Checkpoint(Bytes& state, std::string&) override { state = state_; return true; }
	// M7 prerequisites: with `stream`, the state goes to and comes from the
	// checkpoint stream in 8 MiB chunks, never copied whole.
	bool CheckpointTo(ICheckpointSink& sink, std::string& error) override {
		if (!stream_) return ISimulationSolver::CheckpointTo(sink, error);
		for (std::size_t i = 0; i < state_.size(); i += std::size_t(8) << 20)
			if (!sink.Write(state_.data() + i, std::min(state_.size() - i, std::size_t(8) << 20), error)) return false;
		return true;
	}
	bool RestoreFrom(ICheckpointSource& source, std::string& error) override {
		if (!stream_) return ISimulationSolver::RestoreFrom(source, error);
		state_.resize(static_cast<std::size_t>(source.Remaining()));
		for (std::size_t i = 0; i < state_.size(); i += std::size_t(8) << 20)
			if (!source.Read(state_.data() + i, std::min(state_.size() - i, std::size_t(8) << 20), error)) return false;
		next_ = 1;
		return true;
	}
	SolverPosition Position() const override { return {static_cast<double>(next_), next_, next_ == 2}; }
	void RequestStop() override {}
private:
	bool stream_ = false;
	std::size_t size_ = 0;
	Bytes state_;
	std::int64_t next_ = 0;
};

#ifndef _WIN32
// Peak resident set of a child running the worker (macOS: bytes; Linux: KiB).
double ChildPeakMB(const std::vector<std::string>& args)
{
	std::cout.flush();
	const pid_t child = ::fork();
	if (child == 0) {
		std::ostringstream out, err;
		_exit(WorkerMain(args, out, err));
	}
	int status = 0;
	struct rusage usage{};
	if (::wait4(child, &status, 0, &usage) != child) return -1;
#ifdef __APPLE__
	return static_cast<double>(usage.ru_maxrss)/(1 << 20);
#else
	return static_cast<double>(usage.ru_maxrss)/1024;
#endif
}

void CheckpointMemory(const fs::path& root, unsigned megabytes)
{
	std::string error;
	RegisterSolver("test.ballast", []{ return std::unique_ptr<ISimulationSolver>(new BallastSolver); }, error);
	const auto params = root/"ballast.json";
	WriteText(params, "{\"megabytes\": " + std::to_string(megabytes) + "}");
	const double plain = ChildPeakMB({"bake", "test.ballast", params.string(), (root/"ballast-plain").string(),
		"--checkpoint-seconds", "1000000", "--budget-mb", "4096"});
	const double checkpointed = ChildPeakMB({"bake", "test.ballast", params.string(), (root/"ballast-checkpoint").string(),
		"--stop-after-frames", "1", "--budget-mb", "4096"});
	std::error_code ec;
	const auto file = fs::file_size(root/"ballast-checkpoint"/"rise-sim.checkpoint", ec);
	std::cout << "checkpoint memory: state " << megabytes << " MB; peak RSS without a checkpoint " << plain
		<< " MB, with one " << checkpointed << " MB (difference " << checkpointed - plain << " MB; the solver's own "
		"checkpoint copy is " << megabytes << " MB); checkpoint file " << static_cast<double>(file)/(1 << 20) << " MB\n";
	Check(plain > 0 && checkpointed > 0 && checkpointed - plain < 1.25*megabytes,
		"the worker's checkpoint write path holds no extra full copy of the state");
	const double resumed = ChildPeakMB({"resume", (root/"ballast-checkpoint").string(), "--budget-mb", "4096"});
	// M7 prerequisites: the same solver streaming its state (CheckpointTo /
	// RestoreFrom): the write and the resume hold the state once.
	const auto streamParams = root/"ballast-stream.json";
	WriteText(streamParams, "{\"megabytes\": " + std::to_string(megabytes) + ", \"stream\": true}");
	const double streamPlain = ChildPeakMB({"bake", "test.ballast", streamParams.string(), (root/"ballast-stream-plain").string(),
		"--checkpoint-seconds", "1000000", "--budget-mb", "4096"});
	const double streamed = ChildPeakMB({"bake", "test.ballast", streamParams.string(), (root/"ballast-stream").string(),
		"--stop-after-frames", "1", "--budget-mb", "4096"});
	const double streamResumed = ChildPeakMB({"resume", (root/"ballast-stream").string(), "--budget-mb", "4096"});
	std::cout << "streamed checkpoint memory: state " << megabytes << " MB; peak RSS without a checkpoint " << streamPlain
		<< " MB, writing one " << streamed << " MB, resuming from it " << streamResumed << " MB (in-memory path: write "
		<< checkpointed << " MB, resume " << resumed << " MB)\n";
	Check(streamPlain > 0 && streamed > 0 && streamed - streamPlain < 0.1*megabytes + 64,
		"a streamed checkpoint write holds the state once (peak about the state size)");
	Check(streamResumed > 0 && streamResumed < 1.1*megabytes + 64, "a streamed resume holds the state once");
	std::error_code exists;
	Check(fs::exists(root/"ballast-stream"/"manifest.cbor", exists), "the streamed resume finished the bake");
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

int main(int argc, char** argv)
{
	RegisterReferenceSolvers();
	const fs::path root = RISE::TestSupport::ScratchRunDir("rise_sim_worker_", "suite");
	try {
		fs::create_directories(root);
		if (!DiskPublicationAvailable(root)) { fs::remove_all(root); return 0; }
#ifndef _WIN32
		if (argc > 1 && std::string(argv[1]) == "--checkpoint-memory") {
			CheckpointMemory(root, argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 110u);
			std::error_code ignored;
			fs::remove_all(root, ignored);
			return failures ? 1 : 0;
		}
#else
		(void)argc; (void)argv;
#endif
		Conformance(root, "test.advection_diffusion", "{\"frames\": 12, \"cells\": 24}", root/"grid-reference");
		Conformance(root, "test.membrane", "{}", root/"mesh-reference");
		StopResume(root, root/"mesh-reference");
		StreamedCheckpoints(root, root/"mesh-reference");
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
