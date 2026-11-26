// Liquid solver with viscosity and surface tension: validation (simulation
// M7c, cases 10 and 11 of docs/SIMULATION_M7_DESIGN.md;
// docs/SIMULATION_M7C_RESULTS.md section 4).
//
//   10a viscous Taylor-Green vortex (free slip): exp(-2 nu t), exact
//   10b viscous damping of a deep standing wave (free slip): Lamb's linear
//       root of (s + 2 nu k^2)^2 + g k = 4 nu^2 k^3 m; the inviscid twin's
//       numerical damping subtracted ("net"); no-slip walls reported
//   10c circular Couette flow between a spinning cut-cell cylinder and a
//       static one: the steady profile and torque (exact), the start-up
//       profile against a fine 1-D radial solve
//   10d Stokes drag on a cylinder translating in a concentric container
//       (exact, Happel and Brenner)
//   10e a free disk in rigid rotation: one viscous step leaves it unchanged
//   10f a sphere settling in a cylindrical container (case 10): the terminal
//       velocity against Stokes with the Haberman-Sayre wall factor; light
//       spheres rising (the implicit body coupling)
//   11a static drop: Laplace pressure, parasitic currents (Laplace number
//       12000); inviscid and cell-curvature variants reported
//   11b oscillating drop against Lamb's frequency (2-D; 3-D extended)
//   11c capillary-gravity standing wave
//
// Fast tier about 45 s on 8 threads; --extended (or RISE_TEST_EXTENDED=1)
// adds the finer levels, 3-D drops and the capillary time-step sweep.
// Tolerances are 1.25 x the error observed at that level with the default
// numerics, rounded up.
#include "../src/Library/Simulation/Liquid/LiquidValidation.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace RISE::Simulation::Liquid;
int failures = 0;
constexpr double Pi = 3.14159265358979323846;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double Order(double coarse, double fine, double ratio = 2) { return std::log(std::fabs(coarse)/std::fabs(fine))/std::log(ratio); }

void Row(const char* id, const char* reference, int n, double error, double tolerance, double seconds, const std::string& note = {})
{
	const bool gated = tolerance > 0;
	const bool pass = !gated || (std::isfinite(error) && std::fabs(error) <= tolerance);
	char tol[16] = "-";
	if (gated) std::snprintf(tol, sizeof tol, "%.2e", tolerance);
	std::printf("| %-34s | %-26s | %4d | %+11.3e | %9s | %-6s | %6.2f s | %s\n", id, reference, n, error,
		tol, gated ? (pass ? "pass" : "FAIL") : "report", seconds, note.c_str());
	if (!pass) Check(false, id, "error " + std::to_string(error) + " above " + std::to_string(tolerance));
}
void OrderRow(const char* id, double coarse, double fine, double minimum, double ratio = 2)
{
	const double p = Order(coarse, fine, ratio);
	if (minimum > 0) {
		std::printf("|   order %-26s | %.2f (minimum %.1f) %s\n", id, p, minimum, p >= minimum ? "pass" : "FAIL");
		if (!(p >= minimum)) Check(false, std::string("order of ") + id, std::to_string(p));
	} else std::printf("|   order %-26s | %.2f (reported)\n", id, p);
}
double Tol(const std::map<int,double>& t, int n) { const auto it = t.find(n); return it == t.end() ? 0.0 : it->second; }

// ------------------------------------------------------------------ 10a

void TaylorGreen(bool extended, Pool* pool)
{
	std::printf("\n10a. Viscous Taylor-Green vortex, nu = 0.05 m^2/s, free slip, T = 2 (exact decay exp(-2 nu t))\n");
	const SolverOptions o;
	std::vector<int> levels{16, 32, 64};
	if (extended) levels.push_back(128);
	const std::map<int,double> tol{{16, 7.0e-4}, {32, 6.6e-4}, {64, 3.4e-4}, {128, 1.7e-4}};
	std::vector<double> e;
	for (int n : levels) {
		const auto r = RunViscousTaylorGreen(n, 0.05, 2.0, o, pool);
		char note[96];
		std::snprintf(note, sizeof note, "max face error %.2e; %d steps, viscous CG <= %d", r.maxError, r.steps, r.iterations);
		Row("amplitude / exp(-2 nu T) - 1", "exact", n, r.amplitudeError, Tol(tol, n), r.seconds, note);
		e.push_back(r.amplitudeError);
	}
	for (std::size_t i = 1; i < e.size(); ++i) OrderRow("amplitude (first order in time)", e[i - 1], e[i], 0);
}

// ------------------------------------------------------------------ 10b

void WaveDamping(bool extended, Pool* pool)
{
	std::printf("\n10b. Viscous damping of a standing wave, kd = pi, ka = 0.01, nu = 1e-3 m^2/s, 4 periods\n");
	const SolverOptions o;
	double decay = 0, omega = 0;
	ViscousWaveRoot(9.81, 2*Pi, 1e-3, 0, decay, omega);
	std::printf("     Lamb's root: decay %.5f 1/s (2 nu k^2 = %.5f), omega %.5f rad/s; nu k^2 / omega = %.2e, boundary layer sqrt(2 nu / omega) = %.4f m\n",
		decay, 2*1e-3*4*Pi*Pi, omega, 1e-3*4*Pi*Pi/omega, std::sqrt(2e-3/omega));
	std::vector<int> levels{16, 32};
	if (extended) levels.push_back(64);
	const std::map<int,double> tolNet{{16, 2.1e-1}, {32, 8.4e-2}, {64, 3.6e-2}};
	std::vector<double> nets;
	for (int n : levels) {
		const auto r = RunViscousWave(n, Pi, 0.01, 4, 1e-3, false, true, o, pool);
		char note[160];
		std::snprintf(note, sizeof note, "measured %.4e 1/s, inviscid twin %.2e (%.0fx below); total error %+.2e; omega %.4f", r.decay, r.inviscidDecay,
			r.decayExact/std::max(1e-300, r.inviscidDecay), r.error, r.omega);
		Row("(decay - inviscid) / Lamb - 1", "Lamb deep water", n, r.errorNet, Tol(tolNet, n), r.seconds, note);
		nets.push_back(r.errorNet);
	}
	for (std::size_t i = 1; i < nets.size(); ++i) OrderRow("net decay", nets[i - 1], nets[i], 0);
	const auto ns = RunViscousWave(32, Pi, 0.01, 4, 1e-3, true, false, o, pool);
	char note[96];
	std::snprintf(note, sizeof note, "no-slip walls and bottom: decay %.4e 1/s (wall boundary layers add)", ns.decay);
	Row("no slip: decay / Lamb (free slip) - 1", "reported", 32, ns.error, 0, ns.seconds, note);
}

// ------------------------------------------------------------------ 10c, 10d, 10e

void Couette(bool extended, Pool* pool)
{
	std::printf("\n10c. Circular Couette flow: cylinder r 0.15 spinning in a static cut-cell cylinder r 0.4, nu = 0.01, no slip, 6 s\n");
	const SolverOptions o;
	std::vector<int> levels{32, 64};
	if (extended) levels.push_back(128);
	const std::map<int,double> tolT{{32, 8.2e-3}, {64, 8.1e-4}, {128, 5.3e-3}}, tolP{{32, 2.6e-2}, {64, 1.2e-2}, {128, 5.1e-3}}, tolS{{32, 3.4e-2}, {64, 2.4e-2}, {128, 6.5e-3}};
	std::vector<double> tq, pr, su;
	for (int n : levels) {
		const auto r = RunCouette(n, 0.15, 0.4, 0.1, 0.01, 6.0, 0.37, 0.5, o, pool);
		char note[96];
		std::snprintf(note, sizeof note, "omega 0.1 rad/s, Re 0.375; torque %.4e N, exact %.4e; %d steps", r.torque, r.torqueExact, r.steps);
		Row("torque / exact - 1", "exact", n, r.torqueError, Tol(tolT, n), r.seconds, note);
		Row("steady profile / (omega R1)", "A r + B / r", n, r.profileError, Tol(tolP, n), 0);
		Row("start-up profile at 0.5 s", "1-D radial, 4000 cells", n, r.startupError, Tol(tolS, n), 0);
		tq.push_back(r.torqueError); pr.push_back(r.profileError); su.push_back(r.startupError);
	}
	for (std::size_t i = 1; i < tq.size(); ++i) { OrderRow("torque", tq[i - 1], tq[i], 0); OrderRow("profile", pr[i - 1], pr[i], 0); OrderRow("start-up", su[i - 1], su[i], 0); }
	for (int n : {32, 64}) {
		const auto r = RunCouette(n, 0.15, 0.4, 1.0, 0.01, 6.0, 0.37, 0, o, pool);
		Row("Re 3.75: torque / exact - 1", "reported", n, r.torqueError, 0, r.seconds, "the near-wall advection bias (section 4)");
	}
}

void Drag(bool extended, Pool* pool)
{
	std::printf("\n10d. Stokes drag on a cylinder (r 0.15) translating at 0.01 m/s in a concentric container (r 0.4), nu = 1 m^2/s, 0.3 s\n");
	const SolverOptions o;
	std::vector<int> levels{32, 64};
	if (extended) levels.push_back(128);
	const std::map<int,double> tol{{32, 3.2e-2}, {64, 1.3e-2}, {128, 2.4e-3}};
	std::vector<double> e;
	for (int n : levels) {
		const auto r = RunCylinderDrag(n, 0.15, 0.4, 0.01, 1.0, 0.3, 0.37, o, pool);
		char note[96];
		std::snprintf(note, sizeof note, "F %.4e N/m, exact %.4e; viscous share %.3f; %d steps", r.force, r.exact, r.viscousShare, r.steps);
		Row("F / exact - 1", "Happel and Brenner", n, r.error, Tol(tol, n), r.seconds, note);
		e.push_back(r.error);
	}
	for (std::size_t i = 1; i < e.size(); ++i) OrderRow("drag", e[i - 1], e[i], 0);
	// The viscous time-step limit (nu dt / h^2 <= viscous_cfl): the split steps' convergence.
	for (double c : {16.0, 0.0}) {
		SolverOptions v = o;
		v.viscousCfl = c;
		const auto r = RunCylinderDrag(32, 0.15, 0.4, 0.01, 1.0, 0.3, 0.37, v, pool);
		char note[64];
		std::snprintf(note, sizeof note, "viscous_cfl %g, %d steps", c, r.steps);
		Row("F / exact - 1 (viscous_cfl)", "reported", 32, r.error, 0, r.seconds, note);
	}
}

void Rotation(bool extended, Pool* pool)
{
	std::printf("\n10e. Free disk (r 0.3) in rigid rotation, zero gravity: zero strain, zero viscous traction at the surface\n");
	const SolverOptions o;
	std::vector<int> levels{32};
	if (extended) levels.push_back(64);
	for (int n : levels) {
		const auto r = RunRotatingDisk(n, 0.01, 1.0, o, pool);
		Row("one viscous step: max |du| / (omega R)", "0", n, r.stepChange, 1e-9, r.seconds);
		char note[96];
		std::snprintf(note, sizeof note, "inviscid run's own change %+.2e over 1 rad", r.inviscidLoss);
		Row("angular momentum change, viscous - inviscid", "0", n, r.momentLoss, 0, 0, note);
	}
}

// ------------------------------------------------------------------ 10f

void Settling(bool extended, Pool* pool)
{
	std::printf("\n10f. Sphere settling on the axis of a cylinder (a 0.012, R 0.04, lambda 0.3), nu = 0.01 m^2/s, density ratio 2\n");
	std::printf("     reference: Stokes 2 (ratio - 1) g a^2 / (9 nu) x Haberman-Sayre %.5f (Bohlin's series %.5f)\n", HabermanSayre(0.3), BohlinFactor(0.3));
	const SolverOptions o;
	std::vector<int> levels{24};
	if (extended) { levels.push_back(32); levels.push_back(48); }
	const std::map<int,double> tol{{24, 9.6e-2}, {32, 7.1e-2}, {48, 5.6e-2}};
	std::vector<double> e;
	for (int n : levels) {
		const auto r = RunSettlingSphere(n, 0.012, 0.04, 0.16, 2.0, 0.01, 0.12, o, pool);
		char note[128];
		std::snprintf(note, sizeof note, "U %.4e m/s, reference %.4e, Re %.3f, sphere %.1f cells; %d steps, CG <= %d", r.velocity, r.corrected, r.reynolds,
			0.012/(0.08/n), r.steps, r.maxIterations);
		Check(r.finite, "settling run is finite");
		Row("U / U_Stokes K(lambda) - 1", "Haberman-Sayre", n, r.error, Tol(tol, n), r.seconds, note);
		e.push_back(r.error);
	}
	for (std::size_t i = 1; i < e.size(); ++i) OrderRow("terminal velocity", e[i - 1], e[i], 0, i == 1 ? 32.0/24 : 1.5);
	if (extended)
		for (double ratio : {0.5, 0.1}) {
			const auto r = RunSettlingSphere(24, 0.012, 0.04, 0.16, ratio, 0.01, 0.08, o, pool);
			char note[96];
			std::snprintf(note, sizeof note, "density ratio %.2f (rising): U %.4e m/s; stable", ratio, r.velocity);
			Check(r.finite && std::fabs(r.error) < 0.5, "a light sphere rises stably (implicit viscous coupling)", std::to_string(r.error));
			Row("light: U / reference - 1", "Haberman-Sayre", 24, r.error, 0, r.seconds, note);
		}
}

// ------------------------------------------------------------------ 11

void StaticDrop(bool extended, Pool* pool)
{
	std::printf("\n11a. Static drop, R = 0.25, rho = sigma = 1, zero gravity, 10 capillary times (Laplace number 12000: mu = 6.45e-3)\n");
	const SolverOptions o;
	const double mu = std::sqrt(0.5/12000);
	std::vector<int> levels{32};
	if (extended) levels.push_back(64);
	const std::map<int,double> tolP{{32, 2.7e-3}, {64, 6.4e-4}}, tolC{{32, 5.5e-5}, {64, 5.9e-5}};
	for (int n : levels) {
		const auto r = RunStaticDrop(n, false, 10, mu, o, pool);
		char note[128];
		std::snprintf(note, sizeof note, "max U %.2e U_s (U_s = sqrt(sigma / rho R)), final %.2e; curvature at t = 0 %.1e; %d steps", r.capillary,
			r.finalSpeed, r.curvatureError, r.steps);
		Row("2-D: pressure / (sigma / R) - 1", "Laplace", n, r.pressureError, Tol(tolP, n), r.seconds, note);
		Row("2-D: parasitic Ca = mu U / sigma", "0", n, r.capillaryNumber, Tol(tolC, n), 0);
	}
	{
		SolverOptions c = o;
		c.interpolateCurvature = false;
		const auto r = RunStaticDrop(32, false, 10, mu, c, pool);
		char note[96];
		std::snprintf(note, sizeof note, "curvature of the liquid cell: pressure %+.2e, curvature at t = 0 %.1e", r.pressureError, r.curvatureError);
		Row("cell curvature: parasitic Ca", "reported", 32, r.capillaryNumber, 0, r.seconds, note);
		const auto inv = RunStaticDrop(32, false, 10, 0, o, pool);
		std::snprintf(note, sizeof note, "inviscid: max U %.2e U_s, final %.2e U_s (not bounded: preview)", inv.capillary, inv.finalSpeed);
		Row("inviscid: pressure / (sigma / R) - 1", "reported", 32, inv.pressureError, 0, inv.seconds, note);
	}
	if (extended) {
		const auto r = RunStaticDrop(32, true, 4, mu, o, pool);
		char note[128];
		std::snprintf(note, sizeof note, "3-D, 4 capillary times: Ca %.2e, max U %.2e U_s; curvature at t = 0 %.1e", r.capillaryNumber, r.capillary, r.curvatureError);
		Row("3-D: pressure / (2 sigma / R) - 1", "Laplace", 32, r.pressureError, 0, r.seconds, note);
		// The capillary time-step limit.
		for (double c : {1.0, 1.5, 2.0}) {
			SolverOptions v = o;
			v.capillaryCfl = c;
			const auto q = RunStaticDrop(32, false, 10, mu, v, pool);
			char nt[64];
			std::snprintf(nt, sizeof nt, "capillary_cfl %g: max U %.2e U_s", c, q.capillary);
			Row("capillary_cfl: parasitic Ca", "reported", 32, q.capillaryNumber, 0, q.seconds, nt);
		}
	}
}

void Oscillation(bool extended, Pool* pool)
{
	std::printf("\n11b. Oscillating drop, mode 2, amplitude 0.05 R, R = 0.25, rho = sigma = 1, zero gravity, inviscid, 2 periods\n");
	const SolverOptions o;
	std::vector<int> levels{16, 32};
	if (extended) { levels.push_back(64); levels.push_back(128); }
	const std::map<int,double> tol{{16, 5.6e-2}, {32, 2.2e-2}, {64, 5.4e-3}, {128, 0}};
	std::vector<double> e;
	for (int n : levels) {
		const auto r = RunOscillatingDrop(n, false, 0.05, 2, 0, o, pool);
		char note[96];
		std::snprintf(note, sizeof note, "omega %.4f, Lamb %.4f; decay %.1e per radian; %d steps", r.omega, r.omegaExact, r.decay, r.steps);
		Row("2-D: omega / Lamb - 1", "n (n^2 - 1) sigma / rho R^3", n, r.error, Tol(tol, n), r.seconds, note);
		e.push_back(r.error);
	}
	for (std::size_t i = 1; i < e.size(); ++i) OrderRow("2-D frequency", e[i - 1], e[i], 0);
	if (extended) {
		const auto r = RunOscillatingDrop(32, true, 0.05, 1.5, 0, o, pool);
		char note[96];
		std::snprintf(note, sizeof note, "omega %.4f, Lamb %.4f; %d steps", r.omega, r.omegaExact, r.steps);
		Row("3-D: omega / Lamb - 1", "n (n-1)(n+2) sigma / rho R^3", 32, r.error, 0, r.seconds, note);
		const auto v = RunOscillatingDrop(32, false, 0.05, 2, std::sqrt(0.5/12000), o, pool);
		std::snprintf(note, sizeof note, "Laplace 12000: decay %.3f 1/s against 4 nu / R^2 = %.3f; omega error %+.2e", v.decay, v.decayExact, v.error);
		Row("2-D viscous: decay / irrotational - 1", "reported", 32, v.decayError, 0, v.seconds, note);
	}
}

void CapillaryWave(bool extended, Pool* pool)
{
	std::printf("\n11c. Capillary-gravity standing wave, Bond number 1 (sigma k^2 / rho = g), kd = pi, ka = 0.01, 3 periods\n");
	const SolverOptions o;
	std::vector<int> levels{16, 32};
	if (extended) levels.push_back(64);
	const std::map<int,double> tol{{16, 7.1e-2}, {32, 3.3e-2}, {64, 1.6e-2}};
	std::vector<double> e;
	const double sr = 9.81/(4*Pi*Pi);
	for (int n : levels) {
		const auto r = RunCapillaryWave(n, Pi, 0.01, sr, 3, o, pool);
		char note[96];
		std::snprintf(note, sizeof note, "omega %.4f, exact %.4f; decay %.1e per radian; %d steps", r.omega, r.omegaExact, r.decay, r.steps);
		Row("omega / exact - 1", "(gk + sigma k^3/rho) tanh kd", n, r.frequencyError, Tol(tol, n), r.seconds, note);
		e.push_back(r.frequencyError);
	}
	for (std::size_t i = 1; i < e.size(); ++i) OrderRow("capillary wave (first order: constant interface extrapolation)", e[i - 1], e[i], 0.9);
}
}

int main(int argc, char** argv)
{
	bool extended = false;
	if (const char* e = std::getenv("RISE_TEST_EXTENDED")) if (std::strcmp(e, "1") == 0) extended = true;
	for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--extended") == 0) extended = true;
	Pool pool(8);
	const double t0 = Seconds();
	TaylorGreen(extended, &pool);
	WaveDamping(extended, &pool);
	Couette(extended, &pool);
	Drag(extended, &pool);
	Rotation(extended, &pool);
	Settling(extended, &pool);
	StaticDrop(extended, &pool);
	Oscillation(extended, &pool);
	CapillaryWave(extended, &pool);
	std::printf("\nLiquidPhysicsValidationTest%s: %.1f s\n", extended ? " (extended)" : "", Seconds() - t0);
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("LiquidPhysicsValidationTest: all checks passed\n");
	return 0;
}
