// The fire.methane CPU backend (simulation M8, part A; M8c follow-up).
//
//   default      CPU only (no Metal needed): the sliced content hash equals the serial
//                one; a coarse tier-4 case stepped through the solver interface in child
//                processes (in-process host): the same accepted states at 1 and 8
//                workers, a checkpoint/restore continuation equal to the uninterrupted
//                run, finite physical values that move the right way (fuel enters at
//                the burner, the gas heats and rises), the solver description (backend
//                "cpu", experimental, its own cache key) and the refusals.
//   --agreement N (FireCPUMetalAgreementTest runs N = 8; skipped where Metal is absent)
//                Metal fast mode and CPU fast mode from the same start for N steps:
//                every step to accumulated binary32 rounding (solver version 3: no
//                pre-pilot rim ignition from rounding residue), and one step from
//                identical bytes (Metal's steps 2 and 5) to rounding
//                (docs/SIMULATION_M8_RESULTS.md sections 10-13).
//   --agreement-certified N, --agreement-fixed-vcycles N   the same in other modes.
//   --cost TIER STEPS  timing only: CPU and Metal seconds per step at that tier.
//   Diagnostics (results section 11): --splice-checkpoint SOURCE TARGET OUT,
//   --long-run-child ..., --hash-check.
//
// Every fire run is a child process: the fire library keeps process-wide state.
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationSolver.h"
#include "../src/Library/Simulation/Fire/FireSolver.h"
#include "../src/Library/Simulation/Fire/FireParameterSchema.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Utilities/FireProductionCompute.h"
#include "../src/Library/Utilities/FireProductionAdvection.h"
#if defined(RISE_ENABLE_OPENVDB)
#include "../src/Library/Simulation/Fire/FireRun.h"
#include "../src/Library/Simulation/Fire/FireRunCheckpoint.h"
#endif
#include "TestTempDir.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
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
std::string ReadText(const fs::path& path) { const auto b = ReadAll(path); return std::string(b.begin(), b.end()); }
void WriteBytes(const fs::path& path, const Bytes& bytes)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// SOURCE's fire state and trailer under
// TARGET's configuration digest, so one backend's accepted state can be restored on the
// other and both take the next step from identical bytes (results section 11).
bool SpliceCheckpoint(const fs::path& source, const fs::path& target, const fs::path& out)
{
	const Bytes a = ReadAll(source), b = ReadAll(target);
	auto trailerOf = [](const Bytes& c, std::size_t& fire, V& t) {
		if (c.size() < 16) return false;
		std::uint64_t length = 0;
		for (unsigned i = 0; i < 8; ++i) length |= std::uint64_t(c[c.size()-16+i]) << (8*i);
		if (length > c.size()-16) return false;
		fire = c.size() - 16 - static_cast<std::size_t>(length);
		std::string why;
		return RISECBOR64::DecodeCanonical(Bytes(c.begin()+static_cast<std::ptrdiff_t>(fire), c.end()-16), t, &why) &&
			t.GetType() == V::Map;
	};
	std::size_t fireA = 0, fireB = 0;
	V ta, tb;
	if (!trailerOf(a, fireA, ta) || !trailerOf(b, fireB, tb) || !tb.Find("configuration")) return false;
	V::Members members;
	for (const auto& m : ta.GetMap()) members.emplace_back(m.first, m.first == "configuration" ? *tb.Find("configuration") : m.second);
	Bytes trailer;
	std::string why;
	if (!RISECBOR64::Encode(V::MapValue(members), trailer, &why)) return false;
	Bytes spliced(a.begin(), a.begin()+static_cast<std::ptrdiff_t>(fireA));
	spliced.insert(spliced.end(), trailer.begin(), trailer.end());
	const std::uint64_t length = trailer.size();
	for (unsigned i = 0; i < 8; ++i) spliced.push_back(static_cast<unsigned char>(length >> (8*i)));
	spliced.insert(spliced.end(), a.end()-8, a.end());
	WriteBytes(out, spliced);
	return true;
}

using NoInputs = NoSceneInputs;

struct NullSink final : IFrameSink {
	bool Publish(std::int64_t, const std::string&, double, std::shared_ptr<const Bytes>, const Bytes&, std::string&) override
		{ return true; }
};

V Parameters(double tier, bool cpu, bool fast, double endS)
{
	V::Members m{{"tier", V::Unsigned(static_cast<std::uint64_t>(tier))}, {"frame_origin_s", V::Float(0.0)},
		{"end_time_s", V::Float(endS)}};
	if (cpu) m.emplace_back("backend", V::String("cpu"));
	if (fast) m.emplace_back("mode", V::String("fast"));
	// Diagnostic: Metal fast mode with the certified fixed V-cycle count (agreement curve).
	if (fast && std::getenv("RISE_M8B_FIXED_VCYCLES")) m.emplace_back("fast_fixed_vcycles", V::Bool(true));
	return V::MapValue(m);
}

#if defined(RISE_ENABLE_OPENVDB)
// Fire state of a solver checkpoint: the bytes before the adapter's trailer.
bool FireState(const Bytes& checkpoint, const fs::path& scratch, FireRun::MethaneRunCheckpoint& state, std::string& error)
{
	if (checkpoint.size() < 16) { error = "short checkpoint"; return false; }
	std::uint64_t length = 0;
	for (unsigned i = 0; i < 8; ++i) length |= std::uint64_t(checkpoint[checkpoint.size()-16+i]) << (8*i);
	if (length > checkpoint.size()-16) { error = "bad trailer"; return false; }
	const std::size_t fire = checkpoint.size() - 16 - static_cast<std::size_t>(length);
	WriteBytes(scratch, Bytes(checkpoint.begin(), checkpoint.begin() + static_cast<std::ptrdiff_t>(fire)));
	return FireRun::LoadMethaneRunCheckpoint(scratch, state, error);
}

// One line per accepted state: time, then every cell's 9 binary32 conservative words and
// every face velocity, as hex, so parents can compare runs bit for bit or numerically.
void DumpState(const FireRun::MethaneRunCheckpoint& s, const fs::path& out)
{
	std::ofstream o(out, std::ios::trunc);
	o << std::setprecision(17) << "time " << s.simulationTimeS << " steps " << s.acceptedSteps << " dims "
		<< s.dimensions[0] << " " << s.dimensions[1] << " " << s.dimensions[2] << "\n";
	for (const auto& cell : s.states) {
		const auto q = FireSim::ToConservativeVector(cell);
		for (std::size_t c = 0; c < 9; ++c) o << q[c] << ' ';
		o << cell.temperatureK << '\n';
	}
	for (unsigned axis = 0; axis < 3; ++axis) {
		o << "velocity " << axis << "\n";
		for (const double v : s.velocity.component[axis]) o << v << '\n';
	}
}

// Child: Configure, Start or Restore, accept `steps` steps (each Advance with a stop request
// parks after the next step), write the checkpoint after `checkpointAt` steps and the state
// after the last.  Prints one STEP line per accepted step with its wall time.
int SolverRunChild(int argc, char** argv)
{
	// args: tier cpu fast end dir steps checkpointAt restoreFile|-
	if (argc != 8) return 2;
	const double tier = std::atof(argv[0]);
	const bool cpu = std::strcmp(argv[1], "1") == 0, fast = std::strcmp(argv[2], "1") == 0;
	const double end = std::atof(argv[3]);
	const fs::path dir = argv[4];
	const int steps = std::atoi(argv[5]), checkpointAt = std::atoi(argv[6]);
	const std::string restore = argv[7];
	std::string error;
	if (!RegisterFireSolver(error)) { std::cout << "ERROR " << error << "\n"; return 1; }
	auto solver = CreateSolver("fire.methane", error);
	SolverDescription d;
	NoInputs inputs;
	if (!solver || !solver->Configure(Parameters(tier, cpu, fast, end), inputs, d, error) ||
		!PrepareCacheDescription(d, inputs, "", error)) { std::cout << "ERROR configure " << error << "\n"; return 1; }
	std::cout << "KEY " << d.cache.key << "\nBACKEND " << d.capabilities.backend << "\nDETERMINISTIC "
		<< d.capabilities.deterministic << "\n";
	for (const auto& b : d.capabilities.backends)
		std::cout << "BACKENDS " << b.name << " " << BackendStatusName(b.status) << " " << b.available << "\n";
	const fs::path side = dir/"legacy";
	fs::create_directories(side);
	if (!solver->SetSideOutputDirectory(side, error)) { std::cout << "ERROR side " << error << "\n"; return 1; }
	if (restore == "-") { if (!solver->Start(error)) { std::cout << "ERROR start " << error << "\n"; return 1; } }
	else if (!solver->Restore(ReadAll(restore), error)) { std::cout << "ERROR restore " << error << "\n"; return 1; }
	NullSink sink;
	int accepted = 0;
	const auto runStart = std::chrono::steady_clock::now();
	while (accepted < steps) {
		AdvanceRequest request;
		AdvanceResult result;
		solver->RequestStop();
		const auto t0 = std::chrono::steady_clock::now();
		if (!solver->Advance(request, sink, nullptr, nullptr, result, error)) { std::cout << "ERROR advance " << error << "\n"; return 1; }
		if (result.status == AdvanceResult::Status::Finished) { std::cout << "FINISHED\n"; break; }
		if (result.status != AdvanceResult::Status::Stopped) continue;   // a frame was published
		++accepted;
		std::cout << "STEP " << accepted << " time " << std::setprecision(17) << result.position.time << " wall_s "
			<< std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count() << "\n";
		if (checkpointAt < 0) {   // every step's state, for the agreement curve
			Bytes state;
			FireRun::MethaneRunCheckpoint fire;
			if (!solver->Checkpoint(state, error) || !FireState(state, dir/"fire.checkpoint", fire, error)) {
				std::cout << "ERROR step checkpoint " << error << "\n"; return 1; }
			DumpState(fire, dir/("state_" + std::to_string(accepted) + ".txt"));
			WriteBytes(dir/("checkpoint_" + std::to_string(accepted) + ".bin"), state);
		}
		if (accepted == checkpointAt) {
			Bytes state;
			if (!solver->Checkpoint(state, error)) { std::cout << "ERROR checkpoint " << error << "\n"; return 1; }
			WriteBytes(dir/"checkpoint.bin", state);
		}
	}
	Bytes state;
	FireRun::MethaneRunCheckpoint fire;
	if (!solver->Checkpoint(state, error) || !FireState(state, dir/"fire.checkpoint", fire, error)) {
		std::cout << "ERROR final checkpoint " << error << "\n"; return 1; }
	DumpState(fire, dir/"state.txt");
	std::cout << "RUN_WALL_S " << std::chrono::duration<double>(std::chrono::steady_clock::now()-runStart).count()
		<< "\nDONE " << fire.acceptedSteps << "\n";
	std::cout.flush();
	// The driver thread is parked; leave without unwinding it (the process ends here).
	std::_Exit(0);
}
#endif

#if defined(RISE_ENABLE_OPENVDB)
// Diagnostic (--long-run-child TIER CPU FAST END DIR STEPS STATS_EVERY DUMP_EVERY; results
// section 11): a long run printing domain and centreline statistics every STATS_EVERY steps
// and dumping the accepted state as binary32 (9 conservative words and T per cell, then the
// face velocities) every DUMP_EVERY steps, for Metal/CPU trajectory comparisons.
int LongRunChild(int argc, char** argv)
{
	if (argc != 8) return 2;
	const double tier = std::atof(argv[0]);
	const bool cpu = std::strcmp(argv[1], "1") == 0, fast = std::strcmp(argv[2], "1") == 0;
	const double end = std::atof(argv[3]);
	const fs::path dir = argv[4];
	const int steps = std::atoi(argv[5]), statsEvery = std::atoi(argv[6]), dumpEvery = std::atoi(argv[7]);
	std::string error;
	if (!RegisterFireSolver(error)) { std::cout << "ERROR " << error << "\n"; return 1; }
	auto solver = CreateSolver("fire.methane", error);
	SolverDescription d;
	NoInputs inputs;
	if (!solver || !solver->Configure(Parameters(tier, cpu, fast, end), inputs, d, error) ||
		!PrepareCacheDescription(d, inputs, "", error)) { std::cout << "ERROR configure " << error << "\n"; return 1; }
	fs::create_directories(dir/"legacy");
	if (!solver->SetSideOutputDirectory(dir/"legacy", error) || !solver->Start(error)) {
		std::cout << "ERROR start " << error << "\n"; return 1; }
	NullSink sink;
	int accepted = 0;
	const auto runStart = std::chrono::steady_clock::now();
	std::cout << std::setprecision(9);
	while (accepted < steps) {
		AdvanceRequest request;
		AdvanceResult result;
		solver->RequestStop();
		if (!solver->Advance(request, sink, nullptr, nullptr, result, error)) { std::cout << "ERROR advance " << error << "\n"; return 1; }
		if (result.status == AdvanceResult::Status::Finished) { std::cout << "FINISHED\n"; break; }
		if (result.status != AdvanceResult::Status::Stopped) continue;
		++accepted;
		const bool stats = accepted % statsEvery == 0, dump = dumpEvery > 0 && accepted % dumpEvery == 0;
		if (!stats && !dump) continue;
		Bytes state;
		FireRun::MethaneRunCheckpoint s;
		if (!solver->Checkpoint(state, error) || !FireState(state, dir/"fire.checkpoint", s, error)) {
			std::cout << "ERROR step checkpoint " << error << "\n"; return 1; }
		const std::size_t nx = s.dimensions[0], ny = s.dimensions[1], nz = s.dimensions[2];
		if (stats) {
			const double volume = s.cellWidthM*s.cellWidthM*s.cellWidthM;
			double sumT = 0, maxT = 0, gas = 0, fuel = 0, products = 0, soot = 0, energy = 0, hot = 0;
			for (const auto& c : s.states) {
				sumT += c.temperatureK; maxT = std::max(maxT, c.temperatureK);
				gas += c.GasDensity()*volume; fuel += c.constituent[0]*volume;
				products += (c.constituent[3] + c.constituent[4])*volume; soot += c.constituent[6]*volume;
				energy += c.sensibleEnergyJPerM3*volume; if (c.temperatureK > 500) hot += volume;
			}
			double ke = 0, maxSpeed = 0, maxUp = 0;
			for (unsigned axis = 0; axis < 3; ++axis) for (const double v : s.velocity.component[axis]) {
				maxSpeed = std::max(maxSpeed, std::fabs(v)); if (axis == 2) maxUp = std::max(maxUp, v);
				ke += 0.5*1.1*v*v*volume; }   // per-face, ambient density: a trend measure only
			double hrr = 0;
			for (const float q : s.values.reaction) hrr += static_cast<double>(q)*volume;
			std::cout << "STAT step " << accepted << " time " << s.simulationTimeS << " meanT " << sumT/s.states.size()
				<< " maxT " << maxT << " gas_kg " << gas << " fuel_kg " << fuel << " products_kg " << products
				<< " soot_kg " << soot << " energy_J " << energy << " hot_m3 " << hot << " ke " << ke << " max_speed "
				<< maxSpeed << " max_w " << maxUp << " frame_hrr_W " << hrr << " clT";
			// Centreline: the 2x2 column around the axis (nx, ny odd: the centre cell and its +x, +y neighbours).
			const std::size_t cx = nx/2, cy = ny/2;
			for (std::size_t z = 0; z < nz; z += 4) {
				double t = 0; for (std::size_t j = 0; j < 2; ++j) for (std::size_t i = 0; i < 2; ++i)
					t += s.states[(z*ny + cy + j)*nx + cx + i].temperatureK;
				std::cout << " " << t/4; }
			std::cout << " clW";
			for (std::size_t z = 0; z <= nz; z += 4) {
				double w = 0; for (std::size_t j = 0; j < 2; ++j) for (std::size_t i = 0; i < 2; ++i)
					w += s.velocity.component[2][(z*ny + cy + j)*nx + cx + i];
				std::cout << " " << w/4; }
			std::cout << " wall_s " << std::chrono::duration<double>(std::chrono::steady_clock::now()-runStart).count()
				<< std::endl;
		}
		if (dump) {
			std::vector<float> words;
			words.reserve(s.states.size()*10);
			for (const auto& cell : s.states) {
				const auto q = FireSim::ToConservativeVector(cell);
				for (std::size_t c = 0; c < 9; ++c) words.push_back(static_cast<float>(q[c]));
				words.push_back(static_cast<float>(cell.temperatureK));
			}
			for (unsigned axis = 0; axis < 3; ++axis) for (const double v : s.velocity.component[axis])
				words.push_back(static_cast<float>(v));
			std::ofstream o(dir/("dump_" + std::to_string(accepted) + ".bin"), std::ios::binary | std::ios::trunc);
			o.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size()*sizeof(float)));
		}
	}
	std::cout << "RUN_WALL_S " << std::chrono::duration<double>(std::chrono::steady_clock::now()-runStart).count()
		<< "\nDONE " << accepted << std::endl;
	std::_Exit(0);
}
#endif

#if !defined(_WIN32) && defined(RISE_ENABLE_OPENVDB)
fs::path self;

int Run(const std::vector<std::string>& args, const std::vector<std::string>& extra, const fs::path& log)
{
	std::vector<std::string> env;
	for (char** e = environ; *e; ++e) {
		const std::string s = *e;
		if (s.rfind("RISE_FIRE_", 0) == 0) continue;
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
	if (child < 0) return -1;
	int status = 0;
	while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

struct State {
	double time = 0; std::size_t steps = 0;
	std::vector<std::vector<double>> cells;   // 9 conservative + temperature
	std::array<std::vector<double>, 3> velocity;
	bool ok = false;
};

State Load(const fs::path& file)
{
	State s;
	std::ifstream in(file);
	std::string word;
	std::size_t nx = 0, ny = 0, nz = 0;
	if (!(in >> word >> s.time >> word >> s.steps >> word >> nx >> ny >> nz)) return s;
	s.cells.assign(nx*ny*nz, std::vector<double>(10));
	for (auto& c : s.cells) for (double& v : c) if (!(in >> v)) return s;
	for (unsigned axis = 0; axis < 3; ++axis) {
		unsigned a = 0;
		if (!(in >> word >> a) || a != axis) return s;
		const std::size_t faces = axis == 0 ? (nx+1)*ny*nz : axis == 1 ? nx*(ny+1)*nz : nx*ny*(nz+1);
		s.velocity[axis].resize(faces);
		for (double& v : s.velocity[axis]) if (!(in >> v)) return s;
	}
	s.ok = true;
	return s;
}

[[maybe_unused]] bool SameBits(const State& a, const State& b)
{
	return a.ok && b.ok && a.time == b.time && a.steps == b.steps && a.cells == b.cells && a.velocity == b.velocity;
}

// Max |a-b| per component: the nine conservative rows over the maximum gas density
// (species) or the maximum |E| (energy), temperature over the maximum temperature, velocity
// over the maximum face speed.  Species are scaled by the gas density, not their own
// maximum: early in a run the products are traces dominated by cancellation.
std::vector<double> Relative(const State& a, const State& b)
{
	std::vector<double> out(11, 0.0);
	if (!a.ok || !b.ok || a.cells.size() != b.cells.size()) return std::vector<double>(11, INFINITY);
	double gas = 0, energy = 0, temperature = 0;
	for (const auto& c : a.cells) {
		double g = 0; for (std::size_t k = 1; k <= 6; ++k) g += c[k];
		gas = std::max(gas, g); energy = std::max(energy, std::fabs(c[8])); temperature = std::max(temperature, c[9]);
	}
	for (std::size_t c = 0; c < 10; ++c) {
		double diff = 0;
		for (std::size_t i = 0; i < a.cells.size(); ++i) diff = std::max(diff, std::fabs(a.cells[i][c]-b.cells[i][c]));
		out[c] = diff/(c < 8 ? gas : c == 8 ? energy : temperature);
	}
	double diff = 0, scale = 0;
	for (unsigned axis = 0; axis < 3; ++axis) for (std::size_t f = 0; f < a.velocity[axis].size(); ++f) {
		diff = std::max(diff, std::fabs(a.velocity[axis][f]-b.velocity[axis][f]));
		scale = std::max(scale, std::fabs(a.velocity[axis][f])); }
	out[10] = scale > 0 ? diff/scale : diff;
	return out;
}

[[maybe_unused]] std::string Field(const std::string& log, const std::string& key)
{
	const std::string text = "\n" + log;
	const auto at = text.find("\n" + key + " ");
	if (at == std::string::npos) return {};
	const auto start = at + key.size() + 2;
	return text.substr(start, text.find('\n', start) - start);
}

int SolverRun(const fs::path& dir, double tier, bool cpu, bool fast, double end, int steps, int checkpointAt,
	const std::string& restore, const std::vector<std::string>& env, std::string& log)
{
	fs::create_directories(dir);
	const int code = Run({"--solver-run-child", std::to_string(tier), cpu ? "1" : "0", fast ? "1" : "0",
		std::to_string(end), dir.string(), std::to_string(steps), std::to_string(checkpointAt), restore}, env, dir/"run.log");
	log = ReadText(dir/"run.log");
	return code;
}

constexpr double kTier = 4;       // the coarse case: 35 x 35 x 53 cells
constexpr double kEnd = 0.0625;   // one frame at t=0; the steps stop long before the second

#if !defined(RISE_FIRE_CPU_AGREEMENT_ONLY)
// M8c: the sliced FNV-1a (content hashes on the CPU backend's workers) equals the serial
// byte loop for every word layout the owner hashes, at 1, 3 and 8 workers.
void SlicedHashMatchesSerial()
{
	std::vector<std::uint64_t> words(300007);
	std::uint64_t x = 0x9e3779b97f4a7c15ull;
	for (auto& w : words) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; w = x; }
	bool same = true;
	// {data bytes, trailing zero bytes, leading zero bytes}
	const unsigned layouts[][3] = {{8, 0, 0}, {4, 4, 0}, {4, 0, 0}, {1, 0, 0}, {3, 5, 0}, {5, 0, 3}, {2, 1, 4}};
	for (const auto& layout : layouts) {
		std::uint64_t serial = 0xcbf29ce484222325ull;
		for (std::uint64_t w : words) {
			for (unsigned b = 0; b < layout[2]; ++b) serial *= 1099511628211ull;
			for (unsigned b = 0; b < layout[0]; ++b) { serial ^= (w >> (8*b)) & 0xFF; serial *= 1099511628211ull; }
			for (unsigned b = 0; b < layout[1]; ++b) serial *= 1099511628211ull;
		}
		for (unsigned workers : {1u, 3u, 8u}) {
			FireProductionCPUParallelScope scope(workers);
			const std::uint64_t sliced = FireProductionFNV1aWords(0xcbf29ce484222325ull, words.data(), words.size(),
				layout[0], layout[1], layout[2]);
			same = same && sliced == serial;
		}
	}
	Check(same, "the sliced FNV-1a content hash equals the serial byte loop (7 word layouts, leading and trailing "
		"zero bytes, 1/3/8 workers)");
}

// M8d (results section 12.4): `backend cpu` chosen in a fire scene.  rise-sim bakes it on the
// CPU backend (the launch receipt names the CPU step) under the scene's predicted key, and the
// same completed parameters run through the in-process host with that key.
int RunTool(const fs::path& tool, const std::vector<std::string>& args, const std::vector<std::string>& extra,
	const fs::path& log)
{
	std::vector<std::string> env;
	for (char** e = environ; *e; ++e) { const std::string s = *e; if (s.rfind("RISE_FIRE_", 0) != 0) env.push_back(s); }
	for (const auto& e : extra) env.push_back(e);
	std::vector<std::string> argv{tool.string()};
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
	if (child < 0) return -1;
	int status = 0;
	while (::waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

void SceneSelection(const fs::path& root)
{
	const fs::path dir = root/"scene";
	fs::create_directories(dir);
	{
		std::ofstream f(dir/"cpu.RISEscene");
		f << "RISE ASCII SCENE 7\npinhole_camera\n{\n\tlocation\t0 1 3\n\tlookat\t0 0.5 0\n\tup\t0 1 0\n\tfov\t40\n}\n\n"
			"fire_simulation\n{\n\tname\tcampfire\n\tcache\tcache_cpu\n\tbackend\tcpu\n\tmode\tfast\n\ttier\t6\n"
			"\tframe_origin_s\t0\n\tend_time_s\t0.001\n}\n\n";
	}
	const fs::path tool = self.parent_path().parent_path()/"tools"/"rise_sim";
	const std::vector<std::string> trust{"RISE_SIMULATION_TRUSTED_ROOTS=" + root.string()};
	const int baked = RunTool(tool, {"bake", (dir/"cpu.RISEscene").string(), "campfire"}, trust, dir/"bake.log");
	const int status = RunTool(tool, {"status", (dir/"cpu.RISEscene").string()}, trust, dir/"status.log");
	const std::string receipt = ReadText(dir/"cache_cpu"/"legacy"/"window_launch.v1"), st = ReadText(dir/"status.log");
	Check(baked == 0 && status == 0 && st.find("campfire: complete") != std::string::npos &&
		receipt.find("cpu-m8-v1") != std::string::npos,
		"rise-sim bakes a fire scene with backend cpu on the CPU backend (receipt cpu-m8-v1), complete under its key",
		ReadText(dir/"bake.log") + st);
	std::string in;
	const int ran = Run({"--in-process-scene-child", (dir/"inprocess").string()}, {}, dir/"inprocess.log");
	in = ReadText(dir/"inprocess.log");
	const std::string key = Field(in, "KEY");
	const std::string shown = st.substr(st.find("key: ") == std::string::npos ? 0 : st.find("key: ") + 5, 12);
	Check(ran == 0 && key.size() == 64 && key == Field(in, "PREDICTED") && key.compare(0, 12, shown) == 0 &&
		Field(in, "BACKEND") == "cpu" && Field(in, "FINISHED") == "1",
		"the same scene parameters run on the CPU backend through the in-process host, under the scene's predicted key",
		in + " status key " + shown);
}

void CPUOnly(const fs::path& root)
{
	SlicedHashMatchesSerial();
	const auto t0 = std::chrono::steady_clock::now();
	std::string a, b, c, d, e;
	// Fast mode: the same strict binary32 C++ as certified (the CPU backend has no fast
	// arithmetic; fast only drops per-step seals), about 1.5x cheaper.
	const int steps = 2;
	const int one = SolverRun(root/"w1", kTier, true, true, kEnd, steps, 1, "-", {"RISE_FIRE_WORKERS=1"}, a);
	Check(one == 0 && a.find("DONE " + std::to_string(steps)) != std::string::npos,
		"the CPU backend runs the coarse case through the solver interface (1 worker)", a);
	Check(Field(a, "BACKEND") == "cpu" && a.find("BACKENDS cpu experimental 1") != std::string::npos &&
		a.find("BACKENDS metal supported") != std::string::npos && Field(a, "DETERMINISTIC") == "1",   // fast mode too
		"the description states backend cpu, experimental and available, deterministic", a);
	const int eight = SolverRun(root/"w8", kTier, true, true, kEnd, steps, 0, "-", {"RISE_FIRE_WORKERS=8"}, c);
	b = c;
	const State s1 = Load(root/"w1"/"state.txt"), s8 = Load(root/"w8"/"state.txt");
	Check(eight == 0 && SameBits(s1, s8), "1 and 8 workers accept bit-identical states", c);
	Check(Field(a, "KEY") == Field(b, "KEY"), "the worker count does not change the cache key");
	// Restore from the checkpoint after step 2 and accept one more step, in a copy of the
	// run's legacy directory (a resume continues the same cache, as rise-sim does).
	fs::create_directories(root/"resume");
	fs::copy(root/"w1"/"legacy", root/"resume"/"legacy", fs::copy_options::recursive);
	const int resumed = SolverRun(root/"resume", kTier, true, true, kEnd, steps - 1, 0,
		(root/"w1"/"checkpoint.bin").string(), {"RISE_FIRE_WORKERS=4"}, d);
	const State sr = Load(root/"resume"/"state.txt");
	Check(resumed == 0 && SameBits(s1, sr), "restoring the step-1 checkpoint and taking one step reproduces step 2 "
		"bit for bit (4 workers after 1)", d);
	// Physical sanity: finite, positive densities, ambient far field, fuel entering at the burner,
	// the gas warming near it and moving up.
	bool finite = s1.ok, positive = s1.ok;
	double maxT = 0, maxFuel = 0, maxUp = 0;
	for (const auto& cell : s1.cells) {
		for (double v : cell) finite = finite && std::isfinite(v);
		double gas = 0; for (std::size_t k = 1; k <= 6; ++k) gas += cell[k];
		positive = positive && gas > 0 && cell[9] > 0;
		maxT = std::max(maxT, cell[9]); maxFuel = std::max(maxFuel, cell[1]);
	}
	for (double w : s1.velocity[2]) { finite = finite && std::isfinite(w); maxUp = std::max(maxUp, w); }
	std::ostringstream sanity;
	sanity << "time " << s1.time << " max_T " << maxT << " max_CH4 " << maxFuel << " max_w " << maxUp;
	std::cout << "sanity: " << sanity.str() << "\n";
	Check(finite && positive && s1.time > 0 && maxFuel > 0 && maxT >= 300 && maxT < 2300 && maxUp > 0 && maxUp < 5,
		"accepted states are finite with positive gas density; methane enters, temperatures stay in [300, 2300) K, "
		"the gas moves up", sanity.str());
	// The Metal default keys differently, and the solver refuses what the CPU owner lacks.
	std::string metalKey;
	{
		const int code = Run({"--configure-child", "0"}, {}, root/"metal-configure.log");
		metalKey = Field(ReadText(root/"metal-configure.log"), "KEY");
		Check(code == 0 && metalKey.size() == 64 && metalKey != Field(a, "KEY"),
			"backend cpu has its own cache key (the Metal default's is unchanged)", ReadText(root/"metal-configure.log"));
	}
	const int refused = Run({"--refusal-child"}, {}, root/"refusal.log");
	Check(refused == 0, "an invalid backend and a Metal-only operator on the CPU backend are refused",
		ReadText(root/"refusal.log"));
	SceneSelection(root);
	std::cout << "cpu-only checks: " << std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count() << " s\n";
}

#endif

// Metal fast mode against CPU fast mode (results sections 10.3, 11, 12 and 13).
//  - Trajectories from the same start, compared after every step.  Until solver version 3 a
//    burner-rim cell could become reaction-eligible before the pilot was established as soon as
//    its CO2+H2O exceeded the ambient zero, and after step 1 those products are signed rounding
//    residue (|rho_p| < 3e-11 kg/m3), different on the two backends, so the trajectories split at
//    step 2 or 3 by up to 1e-4 of the temperature scale (the "rim-ignition event").  Version 3
//    (section 13) asks for a product fraction of 1e-6 (FireCase::RimIgnitionProductWitness),
//    which no residue reaches, so nothing ignites before the pilot does (about step 75) and the
//    whole trajectory is held to accumulated binary32 rounding at every step: species 1e-6,
//    temperature 3e-7, energy 3e-6, velocity 2e-5 of the field scale (measured maxima over 10 steps
//    in section 13; the rounding grows slowly with the step).
//  - One step from identical bytes: Metal's accepted step-2 and step-5 states restored on
//    the CPU backend; each step must equal Metal's next step to rounding.  Step 5 -> 6 is the
//    step the CPU's record-range EOS bracket once moved (section 12.1).
constexpr int kAgreementSteps = 8;
constexpr double kSpeciesTolerance = 1.0e-6, kTemperatureTolerance = 3.0e-7, kEnergyTolerance = 3.0e-6,
	kVelocityTolerance = 2.0e-5;
constexpr double kRoundingSpeciesTolerance = 1.0e-6, kRoundingTemperatureTolerance = 1.0e-7,
	kRoundingEnergyTolerance = 1.0e-6, kRoundingVelocityTolerance = 1.0e-5;
bool WithinRounding(const std::vector<double>& rel)
{
	double species = 0;
	for (std::size_t i = 0; i < 8; ++i) species = std::max(species, rel[i]);
	return species <= kRoundingSpeciesTolerance && rel[9] <= kRoundingTemperatureTolerance &&
		rel[8] <= kRoundingEnergyTolerance && rel[10] <= kRoundingVelocityTolerance;
}
void PrintAgreement(const char* label, int k, double time, const std::vector<double>& rel)
{
	const char* names[] = {"Z", "CH4", "O2", "N2", "CO2", "H2O", "CO", "C(s)", "E", "T", "u"};
	std::cout << label << " step " << k << " time " << std::setprecision(9) << time << std::setprecision(3);
	for (std::size_t i = 0; i < rel.size(); ++i) std::cout << " " << names[i] << "=" << rel[i];
	std::cout << "\n";
}
void Agreement(const fs::path& root, int steps, bool fast = true, const std::vector<std::string>& env = {},
	bool identical = true)
{
	// `env` (both backends): RISE_M8B_FIXED_VCYCLES=1 for the fixed V-cycle diagnostic curve.
	std::string m, c;
	const int metal = SolverRun(root/"metal", kTier, false, fast, kEnd, steps, -1, "-", env, m);
	const int cpu = SolverRun(root/"cpu", kTier, true, fast, kEnd, steps, -1, "-", env, c);
	Check(metal == 0 && cpu == 0, "Metal fast mode and CPU fast mode run the coarse case", m + c);
	if (metal != 0 || cpu != 0) return;
	bool within = true, first = true;
	std::ostringstream curve;
	for (int k = 1; k <= steps; ++k) {
		const State a = Load(root/"metal"/("state_" + std::to_string(k) + ".txt"));
		const State b = Load(root/"cpu"/("state_" + std::to_string(k) + ".txt"));
		const auto rel = Relative(a, b);
		PrintAgreement("AGREEMENT", k, a.time, rel);
		double species = 0;
		for (std::size_t i = 0; i < 8; ++i) species = std::max(species, rel[i]);
		if (k == 1) first = a.ok && b.ok && a.time == b.time && WithinRounding(rel);
		const bool ok = a.ok && b.ok && a.time == b.time && species <= kSpeciesTolerance &&
			rel[9] <= kTemperatureTolerance && rel[8] <= kEnergyTolerance && rel[10] <= kVelocityTolerance;
		if (!ok || (k == 1 && !first)) curve << "step " << k << ": species " << species << " E " << rel[8] << " T "
			<< rel[9] << " velocity " << rel[10] << "; ";
		within = within && ok;
	}
	Check(first, "step 1 agrees to rounding: species 1e-6, temperature 1e-7, energy 1e-6, velocity 1e-5 of the "
		"field scale", curve.str());
	Check(within, "CPU and Metal fast mode agree to accumulated rounding after each of " + std::to_string(steps) +
		" steps (no pre-pilot rim ignition): species 1e-6, temperature 3e-7, energy 3e-6, velocity 2e-5 of the "
		"field scale", curve.str());
	if (!identical) return;
	// The CPU backend from Metal's accepted step-k bytes, one step (k = 2 and 5).
	for (const int k : {2, 5}) {
		if (steps < k + 1) continue;
		std::string r;
		const fs::path dir = root/("identical_" + std::to_string(k));
		fs::create_directories(dir);
		fs::copy(root/"metal"/"legacy", dir/"legacy", fs::copy_options::recursive);
		const bool spliced = SpliceCheckpoint(root/"metal"/("checkpoint_" + std::to_string(k) + ".bin"),
			root/"cpu"/("checkpoint_" + std::to_string(k) + ".bin"), dir/"input.bin");
		const int one = spliced ? SolverRun(dir, kTier, true, fast, kEnd, 1, 0, (dir/"input.bin").string(), env, r) : -1;
		const State a = Load(root/"metal"/("state_" + std::to_string(k + 1) + ".txt")), b = Load(dir/"state.txt");
		const auto rel = Relative(a, b);
		PrintAgreement("IDENTICAL_START", k + 1, a.time, rel);
		Check(one == 0 && a.ok && b.ok && a.time == b.time && WithinRounding(rel),
			"from Metal's accepted step-" + std::to_string(k) + " state the CPU backend's step " + std::to_string(k + 1) +
			" equals Metal's to rounding (species 1e-6, temperature 1e-7, energy 1e-6, velocity 1e-5 of the field scale)", r);
	}
}
#endif
} // namespace

// M8d (results section 12.4): the scene's parameters for a CPU fire, completed by the fire
// schema exactly as a scene keys them.  Tier 6 (the smallest scene tier), one step.
V SceneParameters()
{
	return V::MapValue({{"backend", V::String("cpu")}, {"mode", V::String("fast")}, {"tier", V::Unsigned(6)},
		{"frame_origin_s", V::Float(0.0)}, {"end_time_s", V::Float(0.001)}});
}

// Child: the completed scene parameters through the in-process host, run to the end.
int InProcessSceneChild(const fs::path& dir)
{
	std::string error;
	if (!RegisterFireSolver(error)) { std::cout << "ERROR " << error << "\n"; return 1; }
	V completed;
	const ParameterSchema& schema = FireParameterSchema();
	if (!schema.completeParameters(SceneParameters(), completed, error)) { std::cout << "ERROR complete " << error << "\n"; return 1; }
	std::string predicted;
	if (!MakeCacheKey("fire.methane", schema.solverVersion, completed, {}, predicted, error)) {
		std::cout << "ERROR key " << error << "\n"; return 1; }
	auto solver = CreateSolver("fire.methane", error);
	if (!solver) { std::cout << "ERROR create " << error << "\n"; return 1; }
	InProcessHost host(std::move(solver));
	NoInputs inputs;
	fs::create_directories(dir/"legacy");
	if (!host.Configure(completed, inputs, error) || !host.SetSideOutputDirectory(dir/"legacy", error) ||
		!host.Start(error)) { std::cout << "ERROR host " << error << "\n"; return 1; }
	std::cout << "PREDICTED " << predicted << "\nKEY " << host.Description().cache.key << "\nBACKEND "
		<< host.Description().capabilities.backend << "\n";
	if (!host.StepToIndex(0, error)) { std::cout << "ERROR step " << error << "\n"; return 1; }
	AdvanceResult result;
	for (int i = 0; i < 8 && result.status != AdvanceResult::Status::Finished; ++i)
		if (!host.Step(AdvanceRequest(), result, error)) { std::cout << "ERROR advance " << error << "\n"; return 1; }
	std::cout << "POSITION " << std::setprecision(17) << host.Position().time << "\nFINISHED "
		<< (result.status == AdvanceResult::Status::Finished) << "\n";
	std::cout.flush();
	std::_Exit(0);
}

int ConfigureChildMain(bool cpu)
{
	std::string error;
	if (!RegisterFireSolver(error)) { std::cout << "ERROR " << error << "\n"; return 1; }
	auto solver = CreateSolver("fire.methane", error);
	SolverDescription d;
	NoInputs inputs;
	if (!solver || !solver->Configure(Parameters(4, cpu, false, 0.0625), inputs, d, error) ||
		!PrepareCacheDescription(d, inputs, "", error)) { std::cout << "ERROR " << error << "\n"; return 1; }
	std::cout << "KEY " << d.cache.key << "\n";
	return 0;
}

int RefusalChildMain()
{
	std::string error;
	if (!RegisterFireSolver(error)) return 1;
	{
		auto solver = CreateSolver("fire.methane", error);
		SolverDescription d;
		NoInputs inputs;
		V::Members m{{"tier", V::Unsigned(4)}, {"backend", V::String("gpu")}};
		if (solver->Configure(V::MapValue(m), inputs, d, error) || error.find("backend") == std::string::npos) return 1;
		std::cout << "REFUSED " << error << "\n";
	}
	auto solver = CreateSolver("fire.methane", error);
	SolverDescription d;
	NoInputs inputs;
	V::Members m{{"tier", V::Unsigned(4)}, {"backend", V::String("cpu")}, {"compact_viscous_stencil", V::Bool(true)},
		{"frame_origin_s", V::Float(0.0)}, {"end_time_s", V::Float(0.0625)}};
	// The launch plan's case probe already refuses the operator (Configure fails).
	if (!solver->Configure(V::MapValue(m), inputs, d, error)) {
		if (error.find("compact viscous stencil") == std::string::npos) { std::cout << "configure " << error << "\n"; return 1; }
		std::cout << "REFUSED " << error << "\n";
		return 0;
	}
	bool unavailable = false;
	for (const auto& b : d.capabilities.backends) unavailable = unavailable || (b.name == "cpu" && !b.available);
	const fs::path side = TestSupport::ScratchRunDir("rise_fire_cpu_refusal_", "side");
	fs::create_directories(side);
	solver->SetSideOutputDirectory(side, error);
	const bool started = solver->Start(error);
	std::error_code ignored;
	fs::remove_all(side, ignored);
	if (started || !unavailable || error.find("compact viscous stencil") == std::string::npos) {
		std::cout << "NOT REFUSED " << error << "\n"; return 1; }
	std::cout << "REFUSED " << error << "\n";
	return 0;
}

int main(int argc, char** argv)
{
#if defined(RISE_ENABLE_OPENVDB)
	if (argc >= 2 && std::strcmp(argv[1], "--solver-run-child") == 0) return SolverRunChild(argc - 2, argv + 2);
	if (argc >= 2 && std::strcmp(argv[1], "--long-run-child") == 0) return LongRunChild(argc - 2, argv + 2);
#endif
	if (argc == 3 && std::strcmp(argv[1], "--configure-child") == 0) return ConfigureChildMain(std::strcmp(argv[2], "1") == 0);
	if (argc == 2 && std::strcmp(argv[1], "--refusal-child") == 0) return RefusalChildMain();
#if defined(RISE_ENABLE_OPENVDB)
	if (argc == 3 && std::strcmp(argv[1], "--in-process-scene-child") == 0) return InProcessSceneChild(argv[2]);
#endif
	if (argc == 5 && std::strcmp(argv[1], "--splice-checkpoint") == 0) return SpliceCheckpoint(argv[2], argv[3], argv[4]) ? 0 : 1;
#if !defined(_WIN32) && defined(RISE_ENABLE_OPENVDB) && !defined(RISE_FIRE_CPU_AGREEMENT_ONLY)
	if (argc == 2 && std::strcmp(argv[1], "--hash-check") == 0) {
		SlicedHashMatchesSerial();
		// Throughput of one owner-sized digest (the tier-8 state: 9 x 504,666 words, four bytes and four zeros).
		std::vector<std::uint64_t> words(9u*504666u, 0x3f8000003f800000ull);
		for (unsigned workers : {1u, 8u}) {
			FireProductionCPUParallelScope scope(workers);
			const auto t0 = std::chrono::steady_clock::now();
			std::uint64_t h = 0;
			for (int r = 0; r < 5; ++r) h ^= FireProductionFNV1aWords(0xcbf29ce484222325ull, words.data(), words.size(), 4u, 4u);
			std::cout << "hash timing: " << workers << " worker(s) " << std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now()-t0).count()/5 << " ms per tier-8 state digest (" << (h & 1) << ")\n";
		}
		return failures ? 1 : 0;
	}
#endif
#if defined(_WIN32) || !defined(RISE_ENABLE_OPENVDB)
	std::cout << "SKIP: fire runs need OpenVDB (the legacy frame export) and a POSIX host for the child runs\n";
	return 0;
#else
	self = fs::absolute(argv[0]);
	FireProductionComputeCapability capability;
	QueryFireProductionComputeCapability(capability);
	const fs::path root = TestSupport::ScratchRunDir("rise_fire_cpu_backend_", "suite");
	try {
		fs::create_directories(root);
		if (argc == 4 && std::strcmp(argv[1], "--cost") == 0) {
			const double tier = std::atof(argv[2]);
			const int steps = std::atoi(argv[3]);
			for (const bool cpu : {true, false}) {
				if (!cpu && !capability.available) continue;
				std::string log;
				const int code = SolverRun(root/(cpu ? "cost-cpu" : "cost-metal"), tier, cpu, true, 0.0625, steps, 0, "-",
					{}, log);
				std::cout << (cpu ? "CPU" : "METAL") << " tier " << tier << " exit " << code << "\n" << log << "\n";
			}
		} else {
			const bool curve = argc == 3 && std::strcmp(argv[1], "--agreement") == 0;
			const bool certifiedCurve = argc == 3 && std::strcmp(argv[1], "--agreement-certified") == 0;
			const bool fixedCurve = argc == 3 && std::strcmp(argv[1], "--agreement-fixed-vcycles") == 0;
#if defined(RISE_FIRE_CPU_AGREEMENT_ONLY)
			const bool agreement = true;
#else
			const bool agreement = curve || certifiedCurve || fixedCurve;
			if (!agreement) CPUOnly(root);
#endif
			if (!agreement) {}
			else if (!capability.available)
				std::cout << "SKIP: Metal/CPU agreement needs a Metal device (" << capability.structuredError << ")\n";
			else if (certifiedCurve) Agreement(root, std::atoi(argv[2]), false);
			else if (fixedCurve) Agreement(root, std::atoi(argv[2]), true, {"RISE_M8B_FIXED_VCYCLES=1"});
			else Agreement(root, curve ? std::atoi(argv[2]) : kAgreementSteps);
		}
	} catch (const std::exception& e) {
		++failures;
		std::cerr << "exception: " << e.what() << "\n";
	}
	std::error_code ignored;
	if (!failures) fs::remove_all(root, ignored);
	else std::cerr << "kept " << root << "\n";
	if (failures) { std::cerr << failures << " fire CPU backend check(s) failed\n"; return 1; }
	std::cout << "Fire CPU backend checks passed\n";
	return 0;
#endif
}
