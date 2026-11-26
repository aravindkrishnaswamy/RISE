// The fire.methane CPU backend (simulation M8, part A).
//
//   default      CPU only (no Metal needed): a coarse tier-4 case stepped through the
//                solver interface in child processes (in-process host): the same
//                accepted states at 1, 3 and 8 workers, a checkpoint/restore
//                continuation equal to the uninterrupted run, finite physical values
//                that move the right way (fuel enters at the burner, the gas heats
//                and rises), the solver description (backend "cpu", experimental,
//                its own cache key) and the refusals.  Each child runs a few steps.
//   --agreement  (also in the fast tier, skipped where Metal is absent) Metal fast mode
//                and CPU fast mode from the same start for 8 steps of the same case:
//                the per-step relative differences, with a tolerance derived from the
//                one-step burner reference (docs/SIMULATION_M8_RESULTS.md section 10).
//   --cost TIER STEPS  timing only: CPU and Metal seconds per step at that tier.
//
// Every fire run is a child process: the fire library keeps process-wide state.
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationSolver.h"
#include "../src/Library/Simulation/Fire/FireSolver.h"
#include "../src/Library/Utilities/FireProductionCompute.h"
#if defined(RISE_ENABLE_OPENVDB)
#include "../src/Library/Simulation/Fire/FireRun.h"
#include "../src/Library/Simulation/Fire/FireRunCheckpoint.h"
#endif
#include "TestTempDir.h"

#include <algorithm>
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
void CPUOnly(const fs::path& root)
{
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
	std::cout << "cpu-only checks: " << std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count() << " s\n";
}

#endif

// Metal fast mode against CPU fast mode from the same start, compared after every step.
// Step 1 agrees to binary32 rounding once both run the same V-cycle schedule (results
// section 10.3; Metal fast mode's adaptive V-cycle stop leaves ~2e-4 of the face speed);
// from step 2 a few burner-surface cells differ by ~0.015 K and the difference peaks at
// steps 2-4, then decays (measured to step 24). Bounds, per step, of the field scale:
// species and densities 2e-4 of the maximum gas density, temperature 3e-4 of its maximum,
// energy 1e-2 of max |E| (small near ambient), face velocity 1e-2 of the maximum speed
// (about 1.5 times the owner's 1e-3 sqrt(g D*) endpoint velocity tolerance at this case).
constexpr int kAgreementSteps = 5;
constexpr double kSpeciesTolerance = 2.0e-4, kTemperatureTolerance = 3.0e-4, kEnergyTolerance = 1.0e-2,
	kVelocityTolerance = 1.0e-2;
void Agreement(const fs::path& root, int steps, bool fast = true, const std::vector<std::string>& metalEnv = {})
{
	std::string m, c;
	const int metal = SolverRun(root/"metal", kTier, false, fast, kEnd, steps, -1, "-", metalEnv, m);
	const int cpu = SolverRun(root/"cpu", kTier, true, fast, kEnd, steps, -1, "-", {}, c);
	Check(metal == 0 && cpu == 0, "Metal fast mode and CPU fast mode run the coarse case", m + c);
	if (metal != 0 || cpu != 0) return;
	const char* names[] = {"Z", "CH4", "O2", "N2", "CO2", "H2O", "CO", "C(s)", "E", "T", "u"};
	bool within = true;
	std::ostringstream curve;
	for (int k = 1; k <= steps; ++k) {
		const State a = Load(root/"metal"/("state_" + std::to_string(k) + ".txt"));
		const State b = Load(root/"cpu"/("state_" + std::to_string(k) + ".txt"));
		const auto rel = Relative(a, b);
		std::cout << "AGREEMENT step " << k << " time " << std::setprecision(9) << a.time << std::setprecision(3);
		double scalar = 0;
		for (std::size_t i = 0; i < rel.size(); ++i) {
			std::cout << " " << names[i] << "=" << rel[i];
			if (i != 7 && i != 10) scalar = std::max(scalar, rel[i]);
		}
		std::cout << "\n";
		double species = 0;
		for (std::size_t i = 0; i < 8; ++i) species = std::max(species, rel[i]);
		scalar = species;
		const bool ok = a.ok && b.ok && a.time == b.time && species <= kSpeciesTolerance &&
			rel[9] <= kTemperatureTolerance && rel[8] <= kEnergyTolerance && rel[10] <= kVelocityTolerance;
		if (!ok) curve << "step " << k << ": species " << scalar << " E " << rel[8] << " T " << rel[9] << " velocity "
			<< rel[10] << "; ";
		within = within && ok;
	}
	Check(within, "CPU and Metal fast mode agree after each of " + std::to_string(steps) + " steps within 2e-4 "
		"(species), 3e-4 (temperature), 1e-2 (energy, velocity) of the field scale", curve.str());
}
#endif
} // namespace

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
#endif
	if (argc == 3 && std::strcmp(argv[1], "--configure-child") == 0) return ConfigureChildMain(std::strcmp(argv[2], "1") == 0);
	if (argc == 2 && std::strcmp(argv[1], "--refusal-child") == 0) return RefusalChildMain();
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
