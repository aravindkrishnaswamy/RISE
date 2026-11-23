// The production fire behind the generic solver contract (simulation M2,
// track C): "fire.methane" through the rise-sim worker on a short tier-6
// window (frame origin 0.0625 s, end 0.13 s: about 80 accepted steps, two
// frames).
//
//   configuration  every result-affecting option changes the cache key,
//                  RISE_FIRE_WORKERS does not, legacy environment values are
//                  defaults that explicit parameters override, malformed
//                  values and a changed environment at Start are refused,
//                  one fire solver per process
//   bake           sealed native cache; each native frame is byte-identical
//                  to importing the sealed legacy sequence the bake also
//                  wrote, decodes to the same voxels, and fire_medium's
//                  reader plays the cache directly; the fire checkpoint codec
//                  writes the same bytes in memory as on disk
//   stop/resume    --stop-after-frames, then SIGINT between frames, then
//                  resume: sealed cache, legacy frames and final fire
//                  checkpoint identical to the uninterrupted bake
//   extended       (RISE_TEST_EXTENDED=1 or --extended) legacy frames
//                  byte-identical to the pre-existing capstone path for the
//                  same window; fast mode bakes and resumes
//
// Every fire run and every configuration runs in a child process: the fire
// library keeps process-wide state (one fire solver per process).
#include "../src/Library/Simulation/SimulationWorker.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationGrid.h"
#include "../src/Library/Simulation/Fire/FireSolver.h"
#include "../src/Library/Utilities/FireProductionCompute.h"
#include "../src/Library/Utilities/FireSequence.h"
#include "../src/Library/Utilities/FireSequenceCache.h"
#if defined(RISE_ENABLE_OPENVDB)
#include "../src/Library/Simulation/Fire/FireRun.h"
#endif
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
using namespace RISE;
using namespace RISE::Simulation;
namespace fs = std::filesystem;
using V = RISECBOR64::Value;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << (detail.empty() ? "" : ": ") << detail << "\n"; }
	else std::cout << "ok: " << what << "\n";
}

Bytes ReadAll(const fs::path& path)
{
	std::ifstream f(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string ReadText(const fs::path& path)
{
	const auto b = ReadAll(path);
	return std::string(b.begin(), b.end());
}

void WriteBytes(const fs::path& path, const Bytes& bytes)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteText(const fs::path& path, const std::string& text)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f << text;
}

// ---------------------------------------------------------------- children

// Child entry: the worker with the fire solver registered.
int WorkerChild(int argc, char** argv)
{
	std::string error;
	if (!RegisterFireSolver(error)) { std::cerr << error << "\n"; return 1; }
	std::vector<std::string> args(argv, argv + argc);
	return WorkerMain(args, std::cout, std::cerr);
}

// Child entry: configure once from a canonical CBOR parameter file and print
// the cache key, the effective parameters (hex) and the notes.
int ConfigureChild(const char* parameterFile)
{
	std::string error;
	V parameters;
	if (!RegisterFireSolver(error) || !RISECBOR64::DecodeCanonical(ReadAll(parameterFile), parameters, &error)) {
		std::cout << "ERROR " << error << "\n"; return 1; }
	auto solver = CreateSolver("fire.methane", error);
	SolverDescription d;
	NoSceneInputs inputs;
	if (!solver || !solver->Configure(parameters, inputs, d, error) || !PrepareCacheDescription(d, inputs, "", error)) {
		std::cout << "ERROR " << error << "\n"; return 1; }
	Bytes effective;
	RISECBOR64::Encode(d.effectiveParameters, effective, &error);
	std::cout << "KEY " << d.cache.key << "\nEFFECTIVE ";
	for (unsigned char c : effective) { char hex[3]; std::snprintf(hex, sizeof hex, "%02x", c); std::cout << hex; }
	std::cout << "\nFRAMES " << d.cache.count << "\nDETERMINISTIC " << d.capabilities.deterministic << "\n";
	for (const auto& note : d.notes) std::cout << "NOTE " << note << "\n";
	for (const auto& b : d.capabilities.backends) std::cout << "BACKEND " << b.name << " " << BackendStatusName(b.status)
		<< " " << b.available << "\n";
	return 0;
}

// Child entry: the environment changes between Configure and Start.
int StartMismatchChild(const char* parameterFile, const char* side)
{
	std::string error;
	V parameters;
	if (!RegisterFireSolver(error) || !RISECBOR64::DecodeCanonical(ReadAll(parameterFile), parameters, &error)) return 1;
	auto solver = CreateSolver("fire.methane", error);
	SolverDescription d;
	NoSceneInputs inputs;
	if (!solver || !solver->Configure(parameters, inputs, d, error)) { std::cout << "ERROR " << error << "\n"; return 1; }
#if defined(_WIN32)
	_putenv_s("RISE_FIRE_METHANE_SOOT_YIELD", "0.4");
#else
	setenv("RISE_FIRE_METHANE_SOOT_YIELD", "0.4", 1);
#endif
	if (!solver->SetSideOutputDirectory(side, error)) return 1;
	if (solver->Start(error)) { std::cout << "STARTED\n"; return 1; }
	std::cout << "START_REFUSED " << error << "\n";
	return 0;
}

// Child entry: a second live fire solver in one process is refused.
int SecondInstanceChild(const char* parameterFile)
{
	std::string error;
	V parameters;
	if (!RegisterFireSolver(error) || !RISECBOR64::DecodeCanonical(ReadAll(parameterFile), parameters, &error)) return 1;
	auto first = CreateSolver("fire.methane", error), second = CreateSolver("fire.methane", error);
	SolverDescription d;
	NoSceneInputs inputs;
	if (!first || !second || second->Configure(parameters, inputs, d, error)) return 1;
	std::cout << "SECOND_REFUSED " << error << "\n";
	return 0;
}

#if defined(RISE_ENABLE_OPENVDB)
// Child entry: the pre-existing capstone path for the same window.
int CapstoneChild(const char* directory)
{
	FireRun::FireCapstoneOptions o;
	o.resolutionTier = 6.0; o.frameCadenceS = 0.0625; o.outputDirectory = directory;
	o.windowS = 0.13; o.hasTemporalFrameOrigin = true; o.temporalFrameOriginS = 0.0625;
	o.sealPartialWindow = true;
	return FireRun::RunProductionTemporalCapstone(o);
}
#endif

#if !defined(_WIN32) && defined(RISE_ENABLE_OPENVDB)
constexpr std::size_t budget = std::size_t(512) << 20;
fs::path self;

// Spawns this program with arguments; the child gets the parent's environment
// without any RISE_FIRE_* / RISE_R27* variable, plus `extra`; output to `log`.
pid_t Spawn(const std::vector<std::string>& args, const std::vector<std::string>& extra, const fs::path& log)
{
	std::vector<std::string> env;
	for (char** e = environ; *e; ++e) {
		const std::string s = *e;
		if (s.rfind("RISE_FIRE_", 0) == 0 || s.rfind("RISE_R27", 0) == 0) continue;
		env.push_back(s);
	}
	for (const auto& e : extra) env.push_back(e);
	std::vector<std::string> argv{self.string()};
	argv.insert(argv.end(), args.begin(), args.end());
	std::vector<char*> a, p;
	for (auto& s : argv) a.push_back(&s[0]);
	a.push_back(nullptr);
	for (auto& s : env) p.push_back(&s[0]);
	p.push_back(nullptr);
	std::cout.flush(); std::cerr.flush();
	const pid_t child = ::fork();
	if (child == 0) {
		const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) { ::dup2(fd, 1); ::dup2(fd, 2); }
		::execve(a[0], a.data(), p.data());
		::_exit(127);
	}
	return child;
}

int Wait(pid_t child)
{
	int status = 0;
	while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return -1;
}

int Run(const std::vector<std::string>& args, const std::vector<std::string>& extra, const fs::path& log)
{
	const pid_t child = Spawn(args, extra, log);
	return child > 0 ? Wait(child) : -1;
}

// ---------------------------------------------------------------- configuration

struct Configured { int code = -1; std::string key, effective, output; };

Configured Configure(const fs::path& root, const V::Members& parameters, const std::vector<std::string>& env = {})
{
	static int serial = 0;
	const auto file = root / ("parameters_" + std::to_string(serial) + ".cbor");
	const auto log = root / ("configure_" + std::to_string(serial++) + ".log");
	Bytes bytes;
	std::string error;
	RISECBOR64::Encode(V::MapValue(parameters), bytes, &error);
	WriteBytes(file, bytes);
	Configured c;
	c.code = Run({"--configure-child", file.string()}, env, log);
	c.output = ReadText(log);
	std::istringstream lines(c.output);
	std::string line;
	while (std::getline(lines, line)) {
		if (line.rfind("KEY ", 0) == 0) c.key = line.substr(4);
		if (line.rfind("EFFECTIVE ", 0) == 0) c.effective = line.substr(10);
	}
	return c;
}

V::Members Window(V::Members extra = {})
{
	V::Members m{{"end_time_s", V::Float(0.13)}, {"frame_origin_s", V::Float(0.0625)}, {"tier", V::Unsigned(6)}};
	for (auto& e : extra) m.push_back(e);
	return m;
}

void Configuration(const fs::path& root, bool metal)
{
	const auto start = std::chrono::steady_clock::now();
	const auto base = Configure(root, Window());
	Check(base.code == 0 && base.key.size() == 64 && base.output.find("FRAMES 2") != std::string::npos &&
		base.output.find("DETERMINISTIC 1") != std::string::npos &&
		base.output.find("BACKEND cpu unsupported 0") != std::string::npos &&
		base.output.find(std::string("BACKEND metal supported ") + (metal ? "1" : "0")) != std::string::npos,
		"configure: tier-6 window declares two frames, certified mode deterministic, metal supported, cpu unsupported",
		base.output);

	// Every result-affecting option changes the key (options that need
	// another one are compared against a base that has it).
	const auto compact = Configure(root, Window({{"compact_viscous_stencil", V::Bool(true)}}));
	const auto fast = Configure(root, Window({{"mode", V::String("fast")}}));
	Check(fast.output.find("DETERMINISTIC 0") != std::string::npos, "fast mode is declared nondeterministic", fast.output);
	struct Change { const char* name; V value; const Configured* against; };
	const std::vector<Change> changes{
		{"source_step_cap_s", V::String("0.001"), &base}, {"floor_emissivity", V::String("1"), &base},
		{"inert_plume", V::Bool(true), &base}, {"inert_seed", V::String("0.01"), &base},
		{"advective_cfl", V::String("0.4"), &base}, {"fds_mixing_time", V::Bool(true), &base},
		{"methane_soot_yield", V::String("0.3"), &base}, {"soot_radiative_fraction_cap", V::String("0.45"), &base},
		{"soot_sheet_oxidation", V::Bool(true), &base}, {"soot_self_absorption", V::Unsigned(2), &base},
		{"scalar_vreman_cv", V::String("0.1"), &base}, {"fct_alpha", V::String("1"), &base},
		{"scalar_slope", V::String("central"), &base}, {"wall_slope", V::String("linear"), &base},
		{"limited_momentum_carrier", V::Bool(true), &base}, {"compact_viscous_stencil", V::Bool(true), &base},
		{"mode", V::String("fast"), &base},
		{"vreman_zone", V::String("below:16:0.2857"), &compact}, {"near_wall_wale", V::Bool(true), &compact},
		{"hyper_vreman", V::String("0.5"), &compact}, {"fast_fixed_vcycles", V::Bool(true), &fast}};
	const auto names = FireSolverEnvironmentOptions();
	std::set<std::string> covered;
	for (const auto& c : changes) {
		V::Members m = Window({{c.name, c.value}});
		if (c.against == &compact && std::string(c.name) != "compact_viscous_stencil") m.emplace_back("compact_viscous_stencil", V::Bool(true));
		if (c.against == &fast && std::string(c.name) != "mode") m.emplace_back("mode", V::String("fast"));
		const auto changed = Configure(root, m);
		Check(changed.code == 0 && changed.key.size() == 64 && changed.key != c.against->key &&
			changed.key != base.key, std::string("cache key changes with ") + c.name, changed.output);
		covered.insert(c.name);
	}
	bool all = true;
	for (const auto& n : names) all = all && covered.count(n.first);
	Check(all && names.size() == changes.size(), "every result-affecting option is covered");

	// Environment: workers do not enter the key; legacy values are defaults.
	const auto workers = Configure(root, Window(), {"RISE_FIRE_WORKERS=3"});
	Check(workers.code == 0 && workers.key == base.key, "RISE_FIRE_WORKERS does not change the cache key", workers.output);
	const auto explicitYield = Configure(root, Window({{"methane_soot_yield", V::String("0.3")}}));
	const auto fromEnvironment = Configure(root, Window(), {"RISE_FIRE_METHANE_SOOT_YIELD=0.3"});
	Check(fromEnvironment.code == 0 && fromEnvironment.key == explicitYield.key &&
		fromEnvironment.effective == explicitYield.effective &&
		fromEnvironment.output.find("methane_soot_yield from legacy RISE_FIRE_METHANE_SOOT_YIELD=0.3") != std::string::npos,
		"a legacy environment value becomes the effective parameter and is reported", fromEnvironment.output);
	const auto overridden = Configure(root, Window({{"methane_soot_yield", V::String("0.3")}}),
		{"RISE_FIRE_METHANE_SOOT_YIELD=0.4"});
	Check(overridden.code == 0 && overridden.key == explicitYield.key, "explicit parameters override the environment",
		overridden.output);
	const auto number = Configure(root, Window({{"methane_soot_yield", V::Float(0.3)}}));
	Check(number.code == 0 && number.key == explicitYield.key, "a JSON number maps to the same option text", number.output);
	const auto fastEnv = Configure(root, Window(), {"RISE_FIRE_FAST_MODE=1"});
	Check(fastEnv.code == 0 && fastEnv.key == fast.key, "RISE_FIRE_FAST_MODE=1 selects fast mode", fastEnv.output);

	// Refusals.
	const auto badValue = Configure(root, Window({{"methane_soot_yield", V::String("0.9")}}));
	const auto unknown = Configure(root, Window({{"colour", V::String("red")}}));
	const auto needsCompact = Configure(root, Window({{"vreman_zone", V::String("below:16:0.2857")}}));
	const auto badEnv = Configure(root, Window(), {"RISE_FIRE_INERT_PLUME=yes"});
	const auto badTier = Configure(root, Window({{"tier", V::Unsigned(7)}}));
	Check(badValue.code != 0 && unknown.code != 0 && needsCompact.code != 0 && badEnv.code != 0 && badTier.code != 0 &&
		unknown.output.find("unknown parameter") != std::string::npos &&
		badEnv.output.find("RISE_FIRE_INERT_PLUME") != std::string::npos,
		"malformed values, unknown parameters, missing prerequisites and malformed legacy variables are refused",
		badValue.output + unknown.output + needsCompact.output + badEnv.output + badTier.output);

	// The environment changed after Configure: Start refuses.
	Bytes bytes;
	std::string error;
	RISECBOR64::Encode(V::MapValue(Window()), bytes, &error);
	WriteBytes(root/"window.cbor", bytes);
	fs::create_directories(root/"mismatch-side");
	const int mismatch = Run({"--start-mismatch-child", (root/"window.cbor").string(), (root/"mismatch-side").string()},
		{}, root/"mismatch.log");
	const auto mismatchLog = ReadText(root/"mismatch.log");
	Check(mismatch == 0 && (metal ? mismatchLog.find("RISE_FIRE_METHANE_SOOT_YIELD=0.4 differs") != std::string::npos :
		mismatchLog.find("Metal") != std::string::npos),
		"Start refuses an environment that differs from the effective options", mismatchLog);
	const int second = Run({"--second-instance-child", (root/"window.cbor").string()}, {}, root/"second.log");
	Check(second == 0 && ReadText(root/"second.log").find("only one fire solver") != std::string::npos,
		"a second live fire solver in one process is refused", ReadText(root/"second.log"));
	std::cout << "configuration checks: " << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()
		<< " s\n";
}

// ---------------------------------------------------------------- bakes

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

// Legacy outputs that must not depend on how the run was hosted: the frames
// (working and sealed copies), their time records, frame_times.csv and the
// sealed sequence manifest.
std::map<std::string, Bytes> LegacyFrames(const fs::path& legacy)
{
	std::map<std::string, Bytes> files;
	for (const char* sub : {"frames", "checkpoints"}) {
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(legacy/sub, ec)) {
			const auto ext = e.path().extension().string();
			if (ext == ".vdb" || ext == ".time") files[std::string(sub) + "/" + e.path().filename().string()] = ReadAll(e.path());
		}
	}
	for (const char* name : {"frame_times.csv", "sequence_manifest.rise-fire.cbor"}) files[name] = ReadAll(legacy/name);
	return files;
}

Bytes NormalizedCheckpoint(const fs::path& path)
{
	Bytes b = ReadAll(path);
	if (b.size() < 200) return {};
	std::fill(b.begin()+40, b.begin()+48, 0);
	std::fill(b.begin()+128, b.begin()+192, 0);
	std::fill(b.end()-8, b.end(), 0);
	return b;
}

int Bake(const fs::path& parameters, const fs::path& cache, const fs::path& log, const std::vector<std::string>& extra = {},
	const std::vector<std::string>& env = {})
{
	std::vector<std::string> args{"--worker", "bake", "fire.methane", parameters.string(), cache.string()};
	args.insert(args.end(), extra.begin(), extra.end());
	return Run(args, env, log);
}

std::size_t Lines(const fs::path& file)
{
	std::ifstream f(file);
	return static_cast<std::size_t>(std::count(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>(), '\n'));
}

// Stops a resume with SIGINT once it has accepted `steps` more steps.
int ResumeWithInterrupt(const fs::path& cache, std::size_t steps, const fs::path& log, std::size_t& stoppedAt,
	const std::vector<std::string>& env = {})
{
	const auto trajectory = cache/"legacy"/"budgets"/"maximum_velocity_trajectory.csv";
	const std::size_t before = Lines(trajectory);
	const pid_t child = Spawn({"--worker", "resume", cache.string()}, env, log);
	if (child <= 0) return -1;
	stoppedAt = 0;
	for (int i = 0; i < 6000; ++i) {
		int status = 0;
		if (::waitpid(child, &status, WNOHANG) == child)
			return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
		const auto now = Lines(trajectory);
		if (now >= before + steps) { stoppedAt = now - 1; ::kill(child, SIGINT); break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return Wait(child);
}

bool CheckFrame(const Implementation::FireSequenceManifest& original, const Implementation::FireSequenceManifest& native,
	std::int64_t index, std::string& error)
{
	Implementation::FireSequencePreparedFrame a, b;
	if (!original.LoadFrame(index, a, error) || !native.LoadFrame(index, b, error)) return false;
	if (!b.available || a.channels.size() != b.channels.size() || a.frameIndex != b.frameIndex ||
		a.wholeFileSha256 != b.wholeFileSha256) { error = "frame identity mismatch"; return false; }
	for (const auto& item : a.channels) {
		const auto found = b.channels.find(item.first);
		if (found == b.channels.end()) { error = "missing channel " + item.first; return false; }
		const auto& x = item.second; const auto& y = found->second;
		if (x.vectorValues != y.vectorValues || x.dimensions != y.dimensions || x.minimum != y.minimum ||
			x.maximum != y.maximum || x.values.size() != y.values.size() ||
			std::memcmp(x.values.data(), y.values.data(), x.values.size()*sizeof(double)) != 0) {
			error = "channel bit mismatch " + item.first; return false; }
	}
	return true;
}

// The bake's native cache against its own legacy export.
void CheckBake(const fs::path& root, const fs::path& cache)
{
	const auto legacy = cache/"legacy";
	CacheSnapshot snap;
	std::string error;
	Check(CacheSnapshot::Open(cache, budget, snap, error) && snap.Complete() && snap.Description().producer == "fire.methane" &&
		snap.Description().count == 2 && snap.Entries().size() == 2 && snap.Description().fidelity == "preview",
		"rise-sim seals a two-frame preview cache produced by fire.methane", error);
	Check(!fs::exists(cache/"rise-sim.checkpoint") && fs::is_regular_file(legacy/"sequence_manifest.rise-fire.cbor") &&
		fs::is_regular_file(legacy/"frames"/"frame_00001.vdb") && fs::is_regular_file(legacy/"production_final.checkpoint"),
		"the legacy run directory is sealed beside the cache with frames and a final checkpoint");

	// Native frames: byte-identical to importing the sealed legacy sequence.
	const auto imported = root/"import";
	fs::remove_all(imported);
	CacheSnapshot import;
	const bool importedOK = Implementation::ImportFireSequenceCache((legacy/"sequence_manifest.rise-fire.cbor").string(),
		imported.string(), error) && CacheSnapshot::Open(imported, budget, import, error);
	bool same = importedOK && import.Entries().size() == snap.Entries().size();
	for (std::size_t i = 0; same && i < snap.Entries().size(); ++i) {
		std::shared_ptr<const Bytes> a, b;
		same = snap.Read(snap.Entries()[i].index, "volume", budget, a, error) == FrameStatus::Ready &&
			import.Read(snap.Entries()[i].index, "volume", budget, b, error) == FrameStatus::Ready && *a == *b &&
			snap.Entries()[i].time == import.Entries()[i].time;
	}
	Check(same, "each native frame is byte-identical to the legacy import of the same VDB frame", error);

	// fire_medium's reader plays the rise-sim cache directly.
	Implementation::FireSequenceManifest original, native;
	const auto envelope = ReadAll(legacy/"sequence_manifest.rise-fire.cbor");
	bool playable = original.LoadCanonicalEnvelope(envelope, legacy.string(), error) &&
		native.LoadPlaybackFile((cache/"manifest.cbor").string(), error) && native.IsValid() && native.IsComplete() &&
		native.IsNativeCache() && native.CaseRecordId() == original.CaseRecordId() &&
		native.OpticalRecord() == original.OpticalRecord() && native.Channels().size() == original.Channels().size() &&
		native.SourceQualification() == "preview_only" && native.PreflightAllFrames(error);
	for (const auto& f : original.Frames()) playable = playable && CheckFrame(original, native, f.index, error);
	Check(playable, "fire_medium loads the native cache; every voxel equals the legacy frame bit for bit", error);

#if defined(RISE_ENABLE_OPENVDB)
	// The in-memory checkpoint codec writes the bytes the file writer wrote.
	FireRun::MethaneRunCheckpoint state;
	Bytes memory;
	Check(FireRun::LoadMethaneRunCheckpoint(legacy/"production_final.checkpoint", state, error) &&
		FireRun::SerializeMethaneRunCheckpoint(state, memory, error) && memory == ReadAll(legacy/"production_final.checkpoint"),
		"SerializeMethaneRunCheckpoint equals the checkpoint file byte for byte", error);
#endif
}

void BakeAndResume(const fs::path& root, bool extended)
{
	const auto parameters = root/"window.json";
	WriteText(parameters, "{\"tier\": 6, \"frame_origin_s\": 0.0625, \"end_time_s\": 0.13}");
	const auto reference = root/"uninterrupted";
	auto t0 = std::chrono::steady_clock::now();
	const int baked = Bake(parameters, reference, root/"bake.log", {"--checkpoint-seconds", "0"});
	const double bakeS = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
	Check(baked == WorkerFinished, "rise-sim bakes the tier-6 window", ReadText(root/"bake.log"));
	std::cout << "uninterrupted bake: " << bakeS << " s\n";
	if (baked != WorkerFinished) return;
	CheckBake(root, reference);

	// Stop after the first frame, resume and stop between frames, resume to the end.
	const auto resumed = root/"resumed";
	t0 = std::chrono::steady_clock::now();
	std::string inspect;
	Check(Bake(parameters, resumed, root/"stop1.log", {"--stop-after-frames", "1"}) == WorkerStopped &&
		fs::exists(resumed/"rise-sim.checkpoint") && InspectCache(resumed, budget, inspect) == CacheState::Partial,
		"--stop-after-frames 1 stops with a checkpoint and a partial cache", ReadText(root/"stop1.log"));
	std::size_t stoppedAt = 0;
	const int interrupted = ResumeWithInterrupt(resumed, 15, root/"stop2.log", stoppedAt);
	WorkerStatus status;
	std::string error;
	Check(interrupted == WorkerStopped && ReadWorkerStatus(resumed, status, error) && status.state == "stopped" &&
		status.framesWritten == 1, "SIGINT stops the resumed run between frames (after accepted step " +
		std::to_string(stoppedAt) + ")", ReadText(root/"stop2.log"));
	Check(Run({"--worker", "resume", resumed.string()}, {}, root/"resume.log") == WorkerFinished,
		"resume finishes the window", ReadText(root/"resume.log"));
	std::cout << "stopped and resumed bake: " << std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count()
		<< " s\n";
	Check(Sealed(resumed) == Sealed(reference) && Sealed(reference).size() == 3,
		"stop and resume seal a native cache byte-identical to the uninterrupted bake");
	Check(LegacyFrames(resumed/"legacy") == LegacyFrames(reference/"legacy"),
		"stop and resume write legacy frames, frame times and manifest byte-identical to the uninterrupted bake");
	const auto a = NormalizedCheckpoint(reference/"legacy"/"production_final.checkpoint");
	const auto b = NormalizedCheckpoint(resumed/"legacy"/"production_final.checkpoint");
	Check(!a.empty() && a == b, "the final fire checkpoint equals the uninterrupted run's (producer id normalized)");

	if (!extended) return;
#if defined(RISE_ENABLE_OPENVDB)
	// The pre-existing capstone path for the same window writes the same frames.
	const auto capstone = root/"capstone";
	t0 = std::chrono::steady_clock::now();
	const int capstoneCode = Run({"--capstone-child", capstone.string()}, {}, root/"capstone.log");
	std::cout << "capstone run: " << std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count() << " s\n";
	auto legacy = LegacyFrames(reference/"legacy"), plain = LegacyFrames(capstone);
	const auto capstoneLog = ReadText(root/"capstone.log");
	Check(capstoneCode == 0 && legacy == plain && legacy.size() == 8,
		"rise-sim legacy frames, frame times and sealed manifest are byte-identical to the capstone path's",
		"exit " + std::to_string(capstoneCode) + ", " + std::to_string(legacy.size()) + " legacy files; " +
		capstoneLog.substr(capstoneLog.size() > 2000 ? capstoneLog.size() - 2000 : 0));
#endif

	// Fast mode through the same path.
	const auto fastParameters = root/"fast.json";
	WriteText(fastParameters, "{\"tier\": 6, \"frame_origin_s\": 0.0625, \"end_time_s\": 0.13, \"mode\": \"fast\"}");
	const auto fastA = root/"fast-uninterrupted", fastB = root/"fast-resumed";
	Check(Bake(fastParameters, fastA, root/"fast_bake.log") == WorkerFinished, "fast mode bakes", ReadText(root/"fast_bake.log"));
	Check(Bake(fastParameters, fastB, root/"fast_stop.log", {"--stop-after-frames", "1"}) == WorkerStopped &&
		Run({"--worker", "resume", fastB.string()}, {}, root/"fast_resume.log") == WorkerFinished,
		"fast mode stops and resumes", ReadText(root/"fast_stop.log") + ReadText(root/"fast_resume.log"));
	CacheSnapshot x, y;
	double worst = 0;
	bool comparable = CacheSnapshot::Open(fastA, budget, x, error) && CacheSnapshot::Open(fastB, budget, y, error) &&
		x.Complete() && y.Complete() && x.Entries().size() == y.Entries().size();
	for (std::size_t i = 0; comparable && i < x.Entries().size(); ++i) {
		std::shared_ptr<const Bytes> p, q;
		GridFrame g, h;
		comparable = x.Read(x.Entries()[i].index, "volume", budget, p, error) == FrameStatus::Ready &&
			y.Read(y.Entries()[i].index, "volume", budget, q, error) == FrameStatus::Ready &&
			DecodeGrid(*p, budget, g, error) && DecodeGrid(*q, budget, h, error) && g.channels.size() == h.channels.size();
		for (std::size_t c = 0; comparable && c < g.channels.size(); ++c) {
			const auto& u = g.channels[c]; const auto& w = h.channels[c];
			comparable = u.name == w.name && u.blocks.size() == w.blocks.size();
			for (std::size_t k = 0; comparable && k < u.blocks.size(); ++k) {
				comparable = u.blocks[k].values.size() == w.blocks[k].values.size();
				for (std::size_t n = 0; comparable && n < u.blocks[k].values.size(); ++n) {
					float f, s;
					std::memcpy(&f, &u.blocks[k].values[n], 4); std::memcpy(&s, &w.blocks[k].values[n], 4);
					const double scale = std::max({1.0, std::fabs(double(f)), std::fabs(double(s))});
					worst = std::max(worst, std::fabs(double(f) - double(s))/scale);
				}
			}
		}
	}
	const bool identical = comparable && Sealed(fastA) == Sealed(fastB);
	std::cout << "fast mode stop/resume: " << (identical ? "byte-identical to the uninterrupted fast bake" :
		"differs from the uninterrupted fast bake") << ", largest relative field difference " << worst << "\n";
	Check(comparable, "fast-mode resumed frames are comparable with the uninterrupted fast bake", error);
}
#endif
} // namespace

int main(int argc, char** argv)
{
	if (argc >= 2 && std::strcmp(argv[1], "--worker") == 0) return WorkerChild(argc - 2, argv + 2);
	if (argc == 3 && std::strcmp(argv[1], "--configure-child") == 0) return ConfigureChild(argv[2]);
	if (argc == 4 && std::strcmp(argv[1], "--start-mismatch-child") == 0) return StartMismatchChild(argv[2], argv[3]);
	if (argc == 3 && std::strcmp(argv[1], "--second-instance-child") == 0) return SecondInstanceChild(argv[2]);
#if defined(RISE_ENABLE_OPENVDB)
	if (argc == 3 && std::strcmp(argv[1], "--capstone-child") == 0) return CapstoneChild(argv[2]);
#endif
#if defined(_WIN32) || !defined(RISE_ENABLE_OPENVDB)
	std::string error;
	Check(RegisterFireSolver(error), "the fire solver registers on every build", error);
	auto solver = CreateSolver("fire.methane", error);
	SolverDescription d;
	NoSceneInputs inputs;
	Check(solver && !solver->Configure(V::MapValue({}), inputs, d, error) && error.find("OpenVDB") != std::string::npos,
		"without OpenVDB the fire solver refuses with a clear error", error);
	std::cout << "SKIP: fire runs need macOS with OpenVDB and Metal\n";
	return failures ? 1 : 0;
#else
	self = fs::absolute(argv[0]);
	const bool extended = (argc == 2 && std::strcmp(argv[1], "--extended") == 0) ||
		(std::getenv("RISE_TEST_EXTENDED") && std::strcmp(std::getenv("RISE_TEST_EXTENDED"), "1") == 0);
	FireProductionComputeCapability capability;
	QueryFireProductionComputeCapability(capability);
	const fs::path root = TestSupport::ScratchRunDir("rise_fire_solver_", "suite");
	try {
		fs::create_directories(root);
		Configuration(root, capability.available);
		if (capability.available) BakeAndResume(root, extended);
		else std::cout << "SKIP: fire bakes need a Metal device (" << capability.structuredError << ")\n";
	} catch (const std::exception& e) {
		++failures;
		std::cerr << "exception: " << e.what() << "\n";
	}
	std::error_code ignored;
	if (!failures) fs::remove_all(root, ignored);
	else std::cerr << "kept " << root << "\n";
	if (failures) { std::cerr << failures << " fire solver check(s) failed\n"; return 1; }
	std::cout << "Fire solver checks passed\n";
	return 0;
#endif
}
