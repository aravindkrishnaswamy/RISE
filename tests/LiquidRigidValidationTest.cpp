// Liquid solver with rigid bodies: validation (simulation M7b, case 9 of
// docs/SIMULATION_SOLVER_CONTRACT.md; docs/SIMULATION_SOLVER_CONTRACT.md section 4).
//
//   9a added mass of an impulsive start in concentric containers (2-D
//      cylinder, 3-D sphere): exact potential flow; the error over six offsets
//      of the body from the grid (rms), observed orders
//   9b the cylinder in prescribed oscillation (moving cut cells): the force's
//      sine coefficient against m_a X w^2; the in-phase-with-velocity part
//   9c heavy bodies released at rest in the same containers (coupled):
//      initial acceleration against (m - rho V) g / (m + m_a)
//   9d Archimedes: the discrete equilibrium of a floating sphere (3-D) and
//      cylinder (2-D) at density ratios 0.1, 0.5, 0.9 (rms over offsets)
//   9e a discretely neutral body at rest in still liquid: zero to the solver
//      tolerance; with the exact mass rho V: the buoyancy error's drift
//   9f floating dynamics at ratios 0.1, 0.5, 0.9 (coupled): no growth, the
//      mean position against Archimedes, the momentum audits
//   9g bobbing: a 2-D box spanning the tank between two gaps, against the
//      linear potential-flow frequency computed by an independent
//      finite-difference eigen-solve (and the manometer model, reported)
//
// Fast tier: the coarse levels (about 40 s on 8 threads).  --extended (or
// RISE_TEST_EXTENDED=1) adds the finer levels and the 3-D dynamics.
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
	std::printf("| %-34s | %-30s | %4d | %+11.3e | %9s | %-6s | %6.2f s | %s\n", id, reference, n, error,
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

const double Shifts[] = {0.0, 0.13, 0.37, 0.5, 0.71, 0.89};

// ------------------------------------------------------------------ 9a

void AddedMass(bool extended, Pool* pool)
{
	std::printf("\n9a. Added mass of an impulsive start (concentric containers, a = 0.15, b = 0.4; error over 6 grid offsets)\n");
	const SolverOptions o;
	for (int threeD = 0; threeD < 2; ++threeD) {
		std::vector<int> levels = threeD ? std::vector<int>{24, 32, 48} : std::vector<int>{32, 64, 128, 256};
		if (extended) levels.push_back(threeD ? 64 : 512);
		if (extended && threeD) levels.push_back(96);
		const std::map<int,double> tol = threeD ? std::map<int,double>{{24, 5.6e-2}, {32, 3.2e-2}, {48, 1.4e-2}, {64, 7.2e-3}, {96, 2.9e-3}} :
			std::map<int,double>{{32, 1.6e-2}, {64, 3.3e-3}, {128, 3.0e-4}, {256, 1.4e-4}, {512, 1.4e-4}};
		std::vector<double> e;
		for (int n : levels) {
			double rms = 0, worst = 0, secs = 0, side = 0;
			for (double s : Shifts) {
				const auto r = RunAddedMassImpulse(n, threeD, 0.15, 0.4, s, o, pool);
				rms += r.error*r.error;
				worst = std::max(worst, std::fabs(r.error));
				side = std::max(side, r.sideForce);
				secs += r.seconds;
			}
			rms = std::sqrt(rms/6);
			e.push_back(rms);
			char note[96];
			std::snprintf(note, sizeof note, "max %.2e; |F_y| / |F_x| %.1e", worst, side);
			Row(threeD ? "sphere m_a, rms" : "cylinder m_a, rms", threeD ? "(rho V/2)(b3+2a3)/(b3-a3)" : "rho pi a2 (b2+a2)/(b2-a2)",
				n, rms, Tol(tol, n), secs, note);
		}
		for (std::size_t k = 1; k < e.size(); ++k)
			OrderRow(threeD ? "sphere m_a" : "cylinder m_a", e[k - 1], e[k], threeD ? 1.8 : 0.0, double(levels[k])/levels[k - 1]);
	}
}

// ------------------------------------------------------------------ 9b, 9c

void Oscillation(bool extended, Pool* pool)
{
	std::printf("\n9b. Prescribed oscillation of the cylinder (X = 0.01, 2 Hz, 2 periods; moving cut cells)\n");
	const SolverOptions o;
	std::vector<int> levels{32, 64, 128};
	if (extended) levels.push_back(256);
	// 256: the reference's floor (the force's O((X / b)^2) nonlinear terms, about 6e-4; 9a's 1e-4 floor).
	const std::map<int,double> tol{{32, 1.5e-2}, {64, 3.4e-3}, {128, 4.1e-4}, {256, 6.8e-4}};
	std::vector<double> e;
	for (int n : levels) {
		const auto r = RunAddedMassOscillation(n, 0.15, 0.4, 0.01, 2.0, 2, o, pool);
		e.push_back(r.error);
		char note[96];
		std::snprintf(note, sizeof note, "cosine / sine coefficient %.1e (the part in phase with v)", r.sideForce);
		Row("sine coefficient / (m_a X w^2) - 1", "m_a of 9a", n, r.error, Tol(tol, n), r.seconds, note);
	}
	for (std::size_t k = 1; k < e.size(); ++k) OrderRow("oscillation", e[k - 1], e[k], 0.0);

	std::printf("\n9c. Heavy bodies released at rest in the containers (coupled; heave only)\n");
	struct Case { bool threeD; double ratio; std::vector<int> levels; std::map<int,double> tol; };
	std::vector<Case> cases{{false, 2.0, {32, 64, 128}, {{32, 7.2e-3}, {64, 1.5e-3}, {128, 5.1e-5}}},
		{false, 5.0, {32, 64, 128}, {{32, 4.0e-3}, {64, 7.6e-4}, {128, 4.1e-5}}},
		{true, 2.0, {24, 32}, {{24, 1.3e-2}, {32, 6.8e-3}, {48, 2.5e-3}}}};
	if (extended) cases[2].levels.push_back(48);
	for (const Case& c : cases) {
		for (int n : c.levels) {
			const auto r = RunHeavySinking(n, c.threeD, c.ratio, 0.1, o, pool);
			char note[128];
			std::snprintf(note, sizeof note, "a0 %.4f g against %.4f g; drop in 0.1 s / constant-m_a model - 1: %+.2e; %d PCG it",
				r.acceleration/9.81, r.exact/9.81, r.drop/r.dropModel - 1, r.maxIterations);
			Row(c.threeD ? (c.ratio == 2 ? "sphere 2.0: initial acceleration" : "sphere") : (c.ratio == 2 ? "cylinder 2.0: initial acceleration" :
				"cylinder 5.0: initial acceleration"), "(m - rho V) g / (m + m_a)", n, r.error, Tol(c.tol, n), r.seconds, note);
		}
	}
}

// ------------------------------------------------------------------ 9d, 9e

void Archimedes(bool extended, Pool* pool)
{
	std::printf("\n9d. Archimedes: discrete equilibrium height - exact, / R (R = 0.15, tank filled to 0.5; rms over 3 grid offsets)\n");
	const SolverOptions o;
	const double shifts[] = {0.0, 0.37, 0.71};
	for (int threeD = 1; threeD >= 0; --threeD) {
		for (double ratio : {0.1, 0.5, 0.9}) {
			std::vector<int> levels = threeD ? std::vector<int>{24, 32, 48} : std::vector<int>{32, 64, 128};
			if (extended) levels.push_back(threeD ? 64 : 256);
			std::map<int,double> tol;
			if (threeD) tol = ratio == 0.1 ? std::map<int,double>{{24, 2.6e-2}, {32, 1.4e-2}, {48, 6.1e-3}, {64, 3.4e-3}} :
				ratio == 0.5 ? std::map<int,double>{{24, 1.1e-3}, {32, 7.4e-4}, {48, 2.5e-4}, {64, 1.6e-4}} :
				std::map<int,double>{{24, 2.3e-2}, {32, 1.2e-2}, {48, 5.3e-3}, {64, 3.1e-3}};
			else tol = ratio == 0.1 ? std::map<int,double>{{32, 9.7e-3}, {64, 2.4e-3}, {128, 6.0e-4}, {256, 1.5e-4}} :
				ratio == 0.5 ? std::map<int,double>{{32, 5.0e-4}, {64, 2.4e-4}, {128, 3.6e-5}, {256, 2.5e-5}} :
				std::map<int,double>{{32, 9.1e-3}, {64, 2.5e-3}, {128, 5.5e-4}, {256, 1.3e-4}};
			std::vector<double> e;
			for (int n : levels) {
				double rms = 0, secs = 0;
				for (double s : shifts) { const auto r = RunFloatingStatics(n, threeD, ratio, s, o, pool); rms += r.offset*r.offset; secs += r.seconds; }
				rms = std::sqrt(rms/3);
				e.push_back(rms);
				char id[64];
				std::snprintf(id, sizeof id, "%s, ratio %.1f", threeD ? "sphere" : "cylinder", ratio);
				Row(id, "Archimedes depth", n, rms, Tol(tol, n), secs);
			}
			if (ratio != 0.5)
				for (std::size_t k = 1; k < e.size(); ++k) OrderRow(threeD ? "sphere" : "cylinder", e[k - 1], e[k], 1.7, double(levels[k])/levels[k - 1]);
		}
	}

	std::printf("\n9e. Neutral body at rest (free in all degrees of freedom, still liquid)\n");
	for (int threeD = 0; threeD < 2; ++threeD) {
		const int n = threeD ? 24 : 32;
		const auto r = RunNeutralBody(n, threeD, threeD ? 0.5 : 1.0, o, pool);
		char note[128];
		std::snprintf(note, sizeof note, "liquid %.1e; with the mass rho V (V_y / V - 1 = %+.2e): drift %+.2e g", r.liquidSpeed, r.volumeError,
			r.exactMassAcceleration);
		Row(threeD ? "sphere, mass rho V_y: |v| / sqrt(gh)" : "cylinder, mass rho V_y: |v| / sqrt(gh)", "0 (rest)", n,
			std::max(r.bodySpeed, r.liquidSpeed), 1e-7, r.seconds, note);
	}
}

// ------------------------------------------------------------------ 9f

void Dynamics(bool extended, Pool* pool)
{
	std::printf("\n9f. Floating dynamics: released 0.1 R above equilibrium (heave only, coupled in the pressure solve), 3 s\n");
	const SolverOptions o;
	for (double ratio : {0.1, 0.5, 0.9}) {
		std::vector<double> mean;
		for (int n : {32, 64}) {
			const auto r = RunFloatingDynamics(n, false, ratio, 0.1, 3.0, false, o, pool);
			Check(r.finite, "floating dynamics finite");
			char note[160];
			std::snprintf(note, sizeof note, "mean %+.2e R; growth %.3f; period %.4f s; total audit / (m g T) %.1e; %d PCG it", r.meanOffset,
				r.growth, r.period, r.totalAuditBody, r.maxIterations);
			char id[64];
			std::snprintf(id, sizeof id, "cylinder %.1f: max excursion / start", ratio);
			// Stability: the largest excursion from the mean never exceeds the release offset (no growth;
			// a strict bound, not an error tolerance); its second half is reported.  Audits: 1.25 x observed.
			const std::map<int,double> audit = ratio == 0.1 ? std::map<int,double>{{32, 2.2e-6}, {64, 1.9e-5}} :
				ratio == 0.5 ? std::map<int,double>{{32, 2.7e-5}, {64, 1.3e-5}} : std::map<int,double>{{32, 8.6e-6}, {64, 5.0e-6}};
			Row(id, "<= 1 (no growth)", n, r.growth, 1.0 + 1e-12, r.seconds, note);
			Row("  second half: max / start", "reported", n, r.lateGrowth, 0, 0);
			Row("  body impulse audit", "0 (round-off)", n, r.bodyAudit, 1e-12, 0);
			Row("  liquid + body momentum audit", "0 (/ (M + m) g T)", n, r.totalAudit, Tol(audit, n), 0);
			mean.push_back(r.meanOffset);
		}
		OrderRow("mean position", mean[0], mean[1], 0.0);
	}
	if (extended) {
		const auto r = RunFloatingDynamics(32, true, 0.5, 0.1, 1.5, false, o, pool);
		char note[160];
		std::snprintf(note, sizeof note, "mean %+.2e R; growth %.3f; period %.4f s; audits %.0e / %.1e; %d PCG it", r.meanOffset, r.growth, r.period,
			r.bodyAudit, r.totalAudit, r.maxIterations);
		Row("sphere 0.5, 3-D: max / start", "< 1.25 (no growth)", 32, r.growth, 1.25, r.seconds, note);
	}
}

// ------------------------------------------------------------------ 9g

void Bobbing(bool extended, Pool* pool)
{
	std::printf("\n9g. Bobbing: 2-D box w = 0.6, draft 0.2 (density 0.5), tank 1 m, depth 0.5 (gaps 0.2), released 1 cm low\n");
	const double L = 1, w = 0.6, D = 0.5, d = 0.2;
	std::vector<int> fd{80, 160};
	if (extended) fd.push_back(320);
	std::vector<double> om;
	for (int c : fd) {
		const double t0 = Seconds();
		om.push_back(BoxHeaveFrequency(L, w, D, d, c));
		Row("reference: FD eigen-solve", "linear potential flow", c, om.back(), 0, Seconds() - t0, "rad/s (cells across the half tank)");
	}
	// Richardson with the observed order of the reference (re-entrant corners: about 4/3).
	double ref = om.back();
	if (om.size() >= 3) {
		const double p = Order(om[om.size() - 2] - om[om.size() - 3], om.back() - om[om.size() - 2]);
		ref = om.back() + (om.back() - om[om.size() - 2])/(std::pow(2.0, p) - 1);
		std::printf("|   reference order %.2f, extrapolated %.5f rad/s\n", p, ref);
	} else {
		ref = om.back() + (om.back() - om.front())/(std::pow(2.0, 4.0/3.0) - 1);
		std::printf("|   reference extrapolated with order 4/3: %.5f rad/s (the --extended run measures the order)\n", ref);
	}
	Row("manometer (long-wave) model", "the reference", 0, BoxHeaveManometer(L, w, D, d)/ref - 1, 0, 0, "an order-of-magnitude model");
	const SolverOptions o;
	std::vector<int> levels{32, 64};
	if (extended) levels.push_back(128);
	// 1 cm (5% of the draft): at 2 cm the frequency is lower by about 0.25% (nonlinear), at 0.5 cm as at 1 cm.
	const std::map<int,double> tol{{32, 1.3e-2}, {64, 4.9e-3}, {128, 1.4e-4}};
	for (int n : levels) {
		const auto r = RunBoxBobbing(n, L, w, D, d, 0.01, 5, o, pool);
		char note[96];
		std::snprintf(note, sizeof note, "omega %.4f rad/s; amplitude ratio over 4 periods %.3f", r.omega, r.decay);
		Row("frequency / reference - 1", "FD potential flow", n, r.omega/ref - 1, Tol(tol, n), r.seconds, note);
	}
}
}

int main(int argc, char** argv)
{
	bool extended = false;
	if (const char* e = std::getenv("RISE_TEST_EXTENDED")) if (std::strcmp(e, "1") == 0) extended = true;
	for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--extended") == 0) extended = true;
	Pool pool(8);
	const double t0 = Seconds();
	AddedMass(extended, &pool);
	Oscillation(extended, &pool);
	Archimedes(extended, &pool);
	Dynamics(extended, &pool);
	Bobbing(extended, &pool);
	std::printf("\nLiquidRigidValidationTest%s: %.1f s\n", extended ? " (extended)" : "", Seconds() - t0);
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("LiquidRigidValidationTest: all checks passed\n");
	return 0;
}
