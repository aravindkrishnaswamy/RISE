// Liquid solver validation (simulation M7, docs/SIMULATION_M7A_RESULTS.md section 4).
//
//   1  interface transport: Zalesak's slotted disk (one revolution) and the
//      single vortex (T = 2), 50 / 100 / 200 cells, without and with marker
//      particles: area change, L1 interface error, observed orders
//   1r reinitialisation schemes applied every step to a resting circle and
//      slotted disk: volume drift after 10 / 100 / 1000 calls (the measured
//      choice of the default)
//   2  hydrostatic rest: flat, tilted (gravity rotated), with an immersed
//      static sphere (cut cells), 2-D and 3-D: max |u| / sqrt(g h), pressure
//      against rho g depth
//   2m momentum advection: the Taylor-Green vortex for the three schemes,
//      one and two stages (the measured choice of the default)
//   3  standing gravity wave, finite depth and deep water: frequency against
//      omega^2 = g k tanh(k d), observed orders, numerical decay
//   4  sloshing: the first two modes of a rectangular tank
//   5  early-time collapse of a column against the potential-flow series
//   5s the same collapse against two cut-cell solid planes rotated off the
//      grid (the solid boundary's convergence)
//   6  volume: raw loss without correction, correction shifts with it
//   7  observed orders of 2-5 (within each case)
//   8  dam break against Martin and Moyce 1952 and Lobovsky et al. 2014 (reported, not gating)
//   mc marching cubes: closed, outward, second-order area and volume
//
// Fast tier: the coarse levels (about a minute on 8 threads).  --extended (or
// RISE_TEST_EXTENDED=1) adds the finer levels, the long vortex, the particle
// runs at 200 cells and the finer dam break.  Tolerances are 1.25 x the
// error observed at that level with the default numerics, rounded up;
// second-order cases must reach order 1.8 (1.7 where a singularity or a
// sharp corner is in the measured region: stated per row).
#include "../src/Library/Simulation/Liquid/LiquidValidation.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
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
double Order(double coarse, double fine) { return std::log(std::fabs(coarse)/std::fabs(fine))/std::log(2.0); }

// One table row; `tolerance` 0 = reported only.
void Row(const char* id, const char* reference, int n, double error, double tolerance, double seconds, const std::string& note = {})
{
	const bool gated = tolerance > 0;
	const bool pass = !gated || (std::isfinite(error) && std::fabs(error) <= tolerance);
	char tol[16] = "-";
	if (gated) std::snprintf(tol, sizeof tol, "%.2e", tolerance);
	std::printf("| %-30s | %-26s | %4d | %+11.3e | %9s | %-6s | %6.2f s | %s\n", id, reference, n, error,
		tol, gated ? (pass ? "pass" : "FAIL") : "report", seconds, note.c_str());
	if (!pass) Check(false, id, "error " + std::to_string(error) + " above " + std::to_string(tolerance));
}
void OrderRow(const char* id, double coarse, double fine, double minimum)
{
	const double p = Order(coarse, fine);
	std::printf("|   order %-22s | %.2f (minimum %.1f) %s\n", id, p, minimum, p >= minimum ? "pass" : "FAIL");
	if (!(p >= minimum)) Check(false, std::string("order of ") + id, std::to_string(p));
}

// ------------------------------------------------------------------ 1

void Transport(bool extended, Pool* pool)
{
	std::printf("\n1. Interface transport (L1 = symmetric-difference area / perimeter, m; dA = relative area change)\n");
	struct Level { int n; double zl1, zda, vl1, vda; };
	// Tolerances: 1.25 x observed (default numerics), per level.
	const std::map<int, std::array<double,4>> tol{{50, {{8.4e-3, 0.16, 4.5e-3, 0.056}}}, {100, {{2.0e-3, 4.1e-3, 9.5e-4, 0.012}}},
		{200, {{5.6e-4, 8.0e-4, 2.7e-4, 2.8e-3}}}};
	for (int particles = 0; particles < 2; ++particles) {
		TransportOptions o;
		o.particles = particles != 0;
		std::vector<int> levels{50, 100};
		if (!particles || extended) levels.push_back(200);
		std::vector<TransportResult> z, v;
		for (int n : levels) {
			const TransportResult a = RunZalesak(n, o, pool), b = RunVortex(n, 2.0, o, pool);
			z.push_back(a); v.push_back(b);
			const auto t = tol.at(n);
			char note[160];
			std::snprintf(note, sizeof note, "area change %+.2e, %zu particles, %d reinit calls", a.areaChange, a.particles, a.reinitCalls);
			// With particles (the default): 1.25 x observed as well.
			const std::map<int, std::array<double,2>> pt{{50, {{4.3e-3, 3.2e-3}}}, {100, {{9.2e-4, 7.1e-4}}}, {200, {{2.9e-4, 2.6e-4}}}};
			Row(particles ? "zalesak L1 (particles)" : "zalesak L1", "exact slotted disk", n, a.l1, particles ? pt.at(n)[0] : t[0], a.seconds, note);
			std::snprintf(note, sizeof note, "area change %+.2e, %zu particles, %d reinit calls", b.areaChange, b.particles, b.reinitCalls);
			Row(particles ? "vortex T=2 L1 (particles)" : "vortex T=2 L1", "initial circle (reversal)", n, b.l1, particles ? pt.at(n)[1] : t[2], b.seconds, note);
			if (!particles) {
				Row("zalesak |dA|", "0", n, std::fabs(a.areaChange), t[1], 0);
				Row("vortex |dA|", "0", n, std::fabs(b.areaChange), t[3], 0);
			}
		}
		if (!particles) {
			// The slot's corners limit the slotted disk (minimum 1.7); the vortex is smooth (1.8).
			for (std::size_t k = 1; k < z.size(); ++k) OrderRow("zalesak L1", z[k - 1].l1, z[k].l1, 1.7);
			for (std::size_t k = 1; k < v.size(); ++k) OrderRow("vortex L1", v[k - 1].l1, v[k].l1, 1.8);
		} else {
			// The particle correction is not a smooth error term: better at every level, but
			// its order at the finest level falls to 1.5-1.7 (reported, not gated).
			for (std::size_t k = 1; k < z.size(); ++k)
				std::printf("|   order with particles: zalesak %.2f, vortex %.2f (reported)\n", Order(z[k - 1].l1, z[k].l1), Order(v[k - 1].l1, v[k].l1));
		}
	}
	if (extended) {
		// The demanding case: T = 8 (a long thin filament at the turning point).
		for (int particles = 0; particles < 2; ++particles) {
			TransportOptions o;
			o.particles = particles != 0;
			for (int n : {100, 200}) {
				const TransportResult r = RunVortex(n, 8.0, o, pool);
				char note[96];
				std::snprintf(note, sizeof note, "area change %+.2e", r.areaChange);
				Row(particles ? "vortex T=8 L1 (particles)" : "vortex T=8 L1", "initial circle", n, r.l1, 0, r.seconds, note);
			}
		}
	}
}

void ReinitSchemes(Pool* pool)
{
	std::printf("\n1r. Reinitialisation applied every step to a resting interface (relative volume change)\n");
	const std::pair<ReinitScheme, const char*> schemes[] = {{ReinitScheme::Sussman, "sussman"}, {ReinitScheme::RussoSmereka, "russo_smereka"},
		{ReinitScheme::RussoSmerekaCentral, "russo_smereka_central"}, {ReinitScheme::Subcell, "subcell"}, {ReinitScheme::Frozen, "frozen"}};
	for (int shape = 0; shape < 2; ++shape) for (const auto& s : schemes) for (int n : {50, 100}) {
		Grid g;
		g.n = {{n, n, 1}};
		g.h = 1.0/n;
		g.origin = {{0, 0, -0.5*g.h}};
		std::vector<double> phi(g.Cells());
		Shape slot;
		slot.type = Shape::Type::Box;
		slot.lo = {{0.475, 0, -1}};
		slot.hi = {{0.525, 0.85, 1}};
		for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
			const V3 x = g.CellCentre(i, j, 0);
			const double disk = std::sqrt((x[0] - 0.5)*(x[0] - 0.5) + (x[1] - 0.75)*(x[1] - 0.75)) - 0.15;
			phi[g.C(i, j, 0)] = shape ? std::max(disk, -slot.Distance(x)) : disk;
		}
		Reinitialize(g, phi, ReinitScheme::Sussman, 3*n/2, 0.3, pool);
		const double v0 = LiquidVolume(g, phi, nullptr, pool);
		double d[3] = {0, 0, 0};
		for (int call = 1; call <= 1000; ++call) {
			Reinitialize(g, phi, s.first, 1, 0.3, pool);
			if (call == 10) d[0] = LiquidVolume(g, phi, nullptr, pool)/v0 - 1;
			if (call == 100) d[1] = LiquidVolume(g, phi, nullptr, pool)/v0 - 1;
			if (call == 1000) d[2] = LiquidVolume(g, phi, nullptr, pool)/v0 - 1;
		}
		std::printf("|   %-8s %-22s n=%3d: after 10 %+.2e, 100 %+.2e, 1000 %+.2e\n", shape ? "slotted" : "circle", s.second, n, d[0], d[1], d[2]);
	}
	for (int n : {32, 64}) for (const auto& s : schemes) {
		const ReinitDrift r = MeasureReinitDrift(n, s.first, 100, 1.0, pool);
		std::printf("|   distorted sphere n=%d %-22s: crossing drift mean %.2e max %.2e (/h), |grad| error %.2e\n", n, s.second,
			r.meanDrift, r.maxDrift, r.gradError);
	}
}

// ------------------------------------------------------------------ 2

void Hydrostatic(Pool* pool)
{
	std::printf("\n2. Hydrostatic rest (max |u| over the run / sqrt(g h); pressure error / (rho g H))\n");
	const SolverOptions o;
	struct C { const char* id; bool three; double tilt; bool sphere; double y; double T; };
	const C cases[] = {{"flat 2-D", false, 0, false, 0, 1}, {"tilted 2-D", false, 0.3, false, 0, 1},
		{"tilted + sphere 2-D", false, 0.3, true, 0.3, 1}, {"sphere 1.6 cells deep 2-D", false, 0, true, 0.3, 1},
		{"tilted + sphere 3-D", true, 0.3, true, 0.3, 0.25}};
	for (const C& c : cases) {
		const HydrostaticResult r = RunHydrostatic(32, c.three, c.tilt, c.sphere, c.y, c.T, o, pool);
		char note[128];
		std::snprintf(note, sizeof note, "%d steps, <= %d PCG iterations, volume drift %.1e", r.steps, r.iterations, r.volumeDrift);
		// The discretisation is exact for a linear pressure (the variational
		// weights and the ghost fluid with exact theta): tolerance = the solver's level.
		Row((std::string(c.id) + " |u|").c_str(), "0", 32, r.maxVelocity, 1e-7, r.seconds, note);
		Row((std::string(c.id) + " p").c_str(), "rho g depth", 32, r.pressureError, 1e-7, 0);
	}
}

// ------------------------------------------------------------------ 2m

void Momentum(bool extended, Pool* pool)
{
	std::printf("\n2m. Taylor-Green vortex, T = 2 (rms face error / max speed; 1 - kinetic energy ratio)\n");
	const std::pair<MomentumScheme, const char*> schemes[] = {{MomentumScheme::Weno5, "weno5"}, {MomentumScheme::MacCormack, "maccormack"},
		{MomentumScheme::SemiLagrangian, "semi_lagrangian"}};
	const std::map<int, double> tol{{16, 1.8e-3}, {32, 3.8e-4}, {64, 8.2e-5}, {128, 1.8e-5}};   // m7a.2 (bounded stages)
	std::vector<int> levels{16, 32, 64};
	if (extended) levels.push_back(128);
	for (int stages = 2; stages >= 1; --stages) for (const auto& s : schemes) {
		SolverOptions o;
		o.momentum = s.first;
		o.momentumStages = stages;
		std::vector<double> e;
		for (int n : levels) {
			const TaylorGreenResult r = RunTaylorGreen(n, 2.0, o, pool);
			e.push_back(r.l2Error);
			char id[64], note[96];
			std::snprintf(id, sizeof id, "%s, %d stage%s", s.second, stages, stages == 2 ? "s" : "");
			std::snprintf(note, sizeof note, "max %.2e, 1 - KE %.2e, %d steps", r.maxError, 1 - r.energyRatio, r.steps);
			const bool gated = s.first == MomentumScheme::Weno5 && stages == 2;
			Row(id, "steady exact solution", n, r.l2Error, gated ? tol.at(n) : 0, r.seconds, note);
		}
		for (std::size_t k = 1; k < e.size(); ++k) {
			if (s.first == MomentumScheme::Weno5 && stages == 2) OrderRow("weno5 2 stages", e[k - 1], e[k], 1.8);
			else std::printf("|   order %-22s | %.2f (reported)\n", s.second, Order(e[k - 1], e[k]));
		}
	}
}

// ------------------------------------------------------------------ 3, 4, 5

void Waves(bool extended, Pool* pool)
{
	std::printf("\n3. Standing gravity wave, ka = 0.01, 3 periods (relative frequency error; decay = gamma / omega)\n");
	const SolverOptions o;
	std::vector<int> levels{16, 32, 64};
	if (extended) levels.push_back(128);
	const double kdFinite = 3*Pi/8;
	const std::map<int, std::array<double,2>> tol{{16, {{1.9e-2, 1.6e-2}}}, {32, {{4.8e-3, 4.0e-3}}}, {64, {{1.2e-3, 9.9e-4}}},
		{128, {{2.8e-4, 2.5e-4}}}};
	for (int deep = 0; deep < 2; ++deep) {
		std::vector<double> e;
		for (int n : levels) {
			const WaveResult r = RunStandingWave(n, deep ? Pi : kdFinite, 0.01, 3, o, pool);
			e.push_back(r.frequencyError);
			char note[160];
			std::snprintf(note, sizeof note, "omega %.5f vs %.5f, decay %+.2e /rad, %d steps, volume shifts %.1e a", r.omega, r.omegaExact,
				r.decay, r.steps, r.volumeShift);
			Row(deep ? "deep (kd = pi)" : "finite (kd = 3 pi / 8)", "w^2 = g k tanh(k d)", n, r.frequencyError, tol.at(n)[deep], r.seconds, note);
		}
		for (std::size_t k = 1; k < e.size(); ++k) OrderRow(deep ? "deep" : "finite", e[k - 1], e[k], 1.8);
	}
	std::printf("\n4. Sloshing, 1 x 0.6 tank, depth 0.5, two periods of mode (1,0)\n");
	{
		std::vector<int> sl{20};
		if (extended) sl.push_back(40);
		const std::map<int, std::array<double,2>> st{{20, {{2.4e-3, 6.9e-3}}}, {40, {{5.8e-4, 5.0e-4}}}};
		std::vector<SloshResult> r;
		for (int n : sl) {
			r.push_back(RunSloshing(n, 2, o, pool));
			Row("mode (1,0)", "linear theory", n, r.back().error10, st.at(n)[0], r.back().seconds);
			Row("mode (0,1)", "linear theory", n, r.back().error01, st.at(n)[1], 0);
		}
		if (r.size() == 2) { OrderRow("sloshing (1,0)", r[0].error10, r[1].error10, 1.8); OrderRow("sloshing (0,1)", r[0].error01, r[1].error01, 1.8); }
	}
	std::printf("\n5. Early-time collapse of a column (acceleration / g against the series)\n");
	{
		std::vector<int> cl{16, 32, 64};
		if (extended) cl.push_back(128);
		const std::map<int, std::array<double,2>> ct{{16, {{6.1e-3, 2.5e-3}}}, {32, {{1.6e-3, 1.2e-3}}}, {64, {{4.5e-4, 5.9e-4}}},
			{128, {{1.2e-4, 2.9e-4}}}};
		std::vector<CollapseResult> r;
		for (int n : cl) {
			r.push_back(RunColumnCollapse(n, 1.0, o, pool));
			Row("max beyond H/4 of the corner", "potential-flow series", n, r.back().maxFar, ct.at(n)[0], r.back().seconds);
			Row("L2 over the column", "potential-flow series", n, r.back().l2, ct.at(n)[1], 0);
		}
		// The bottom corner of the dam face is a log singularity of the
		// acceleration: the far field converges at second order (minimum 1.7:
		// the H/4 region still feels it), the L2 norm at first.
		for (std::size_t k = 1; k < r.size(); ++k) { OrderRow("collapse far field", r[k - 1].maxFar, r[k].maxFar, 1.7); OrderRow("collapse L2", r[k - 1].l2, r[k].l2, 0.9); }
		double ax, ay;
		CollapseAcceleration(1.0, 1.0 - 1e-9, 1.0, 200000, ax, ay);
		Row("series: free fall at the top corner", "a_y = -g", 0, ay + 1, 1e-4, 0, "the reference itself: a_x = " + std::to_string(ax));
	}
}

// ------------------------------------------------------------------ 5s

void SolidBoundary(bool extended, Pool* pool)
{
	std::printf("\n5s. Early-time collapse against cut-cell solids: wall and bed rotated 0.35 rad, pivot off the grid (acceleration / g; pressure / (rho g H))\n");
	const SolverOptions o;
	std::vector<int> levels{16, 32, 64};
	if (extended) levels.push_back(128);
	// {velocity max beyond H/4 of the corner, pressure max}: 1.25 x observed (m7a.2).
	const std::map<int, std::array<double,2>> tol{{16, {{3.7e-2, 6.8e-4}}}, {32, {{1.8e-2, 2.0e-4}}}, {64, {{1.1e-2, 4.5e-5}}},
		{128, {{6.4e-3, 1.6e-5}}}};
	const V3 pivot{{0.8123, 0.1789, 0}};
	std::vector<SolidCollapseResult> r;
	for (int n : levels) {
		r.push_back(RunSolidCollapse(n, 0.35, pivot, o, pool));
		const SolidCollapseResult& c = r.back();
		char note[160];
		std::snprintf(note, sizeof note, "within 2h of a solid %.2e (%zu faces), farther %.2e; L2 %.2e; smallest open fraction %.3f", c.maxNearSolid,
			c.nearFaces, c.maxInterior, c.l2, c.minFraction);
		Row("velocity, max beyond H/4", "potential-flow series", n, c.maxFar, tol.at(n)[0], c.seconds, note);
		Row("pressure, max beyond H/4", "potential-flow series", n, c.pressure, tol.at(n)[1], 0);
	}
	// The pressure converges at about second order; the velocity on the faces
	// next to the cut cells (where the max is) at about first order: the
	// face-fraction (Ng, Min and Gibou 2009) operator's gradient is one order
	// below its solution at the boundary.  Minimums below the observed orders.
	for (std::size_t k = 1; k < r.size(); ++k) {
		OrderRow("cut-cell velocity", r[k - 1].maxFar, r[k].maxFar, 0.6);
		OrderRow("cut-cell pressure", r[k - 1].pressure, r[k].pressure, 1.4);
	}
}

// ------------------------------------------------------------------ 6

void Volume(bool extended, Pool* pool)
{
	std::printf("\n6. Volume over 10 periods of a ka = 0.1 standing wave (finite depth)\n");
	const SolverOptions o;
	std::vector<int> levels{32, 64};
	if (extended) levels.push_back(128);
	const std::map<int, double> tol{{32, 1.5e-4}, {64, 7.4e-5}, {128, 4.3e-5}};
	std::vector<double> raw, shift;
	for (int n : levels) {
		const VolumeResult off = RunVolumeConservation(n, 10, false, o, pool), on = RunVolumeConservation(n, 10, true, o, pool);
		raw.push_back(off.drift);
		shift.push_back(on.meanShift);
		Row("raw loss, correction off", "0", n, off.drift, 0, off.seconds, "per period " + std::to_string(off.driftPerPeriod));
		char note[96];
		std::snprintf(note, sizeof note, "max shift %.2e h, residual drift %.1e", on.maxShift, on.drift);
		Row("mean shift / h, correction on", "0", n, on.meanShift, tol.at(n), on.seconds, note);
		Row("drift, correction on", "0", n, on.drift, 1e-6, 0);
	}
	for (std::size_t k = 1; k < raw.size(); ++k)
		std::printf("|   order raw loss %.2f, order mean shift %.2f (reported)\n", Order(raw[k - 1], raw[k]), Order(shift[k - 1], shift[k]));
}

// ------------------------------------------------------------------ 8

void DamBreak(bool extended, Pool* pool)
{
	std::printf("\n8. Dam break, square column (reported against Martin and Moyce 1952, table 1, n^2 = 1; not gating)\n");
	const SolverOptions o;
	std::vector<int> levels{32};
	if (extended) levels.push_back(64);
	for (int n : levels) {
		const DamBreakResult r = RunDamBreak(n, 1.1, o, pool);
		// Martin and Moyce give no true zero time (their T is set to 0.80 at Z = 1.44): align there.
		double offset = 0, speed = 0;
		for (std::size_t q = 0; q < r.z.size(); ++q) if (std::fabs(r.z[q] - 1.44) < 1e-9 && r.t[q] >= 0) offset = 0.80 - r.t[q];
		std::printf("|   %d cells per column width, %d steps, %.1f s, volume drift %.1e, time offset %+.3f\n", n, r.steps, r.seconds,
			r.volumeDrift, offset);
		double sumSq = 0;
		int m = 0;
		for (std::size_t q = 0; q < r.z.size(); ++q) {
			if (r.t[q] < 0) continue;
			const double tt = r.t[q] + offset, mm = MartinMoyceSquare[q][1];
			std::printf("|     Z %.2f: T %.3f (M&M %.2f, %+.3f)\n", r.z[q], tt, mm, tt - mm);
			sumSq += (tt - mm)*(tt - mm);
			++m;
		}
		// Front speed dZ/dT between Z = 1.44 and the last value reached (Ritter: 2).
		for (std::size_t q = r.z.size(); q-- > 0;) if (r.t[q] >= 0) {
			for (std::size_t p = 0; p < r.z.size(); ++p) if (std::fabs(r.z[p] - 1.44) < 1e-9) speed = (r.z[q] - r.z[p])/(r.t[q] - r.t[p]);
			break;
		}
		std::printf("|     rms time difference %.3f over %d points; front speed %.2f sqrt(g a) (Martin and Moyce %.2f; Ritter 2)\n",
			std::sqrt(sumSq/std::max(1, m)), m, speed, (4.89 - 1.44)/(3.29 - 0.80));
		std::printf("|     second reference, Lobovsky et al. 2014 (100 runs per depth; average front speed for t sqrt(g/H) > 1, v / sqrt(g H)): "
			"H = 600 mm (square column, ours) %.2f, H = 300 mm (reservoir 2 H wide) %.2f; their reduction of Martin and Moyce %.2f\n",
			LobovskyFrontSpeed600, LobovskyFrontSpeed300, LobovskyReductionMartinMoyce57);
		Check(r.steps > 0 && std::fabs(r.volumeDrift) < 1e-4 && speed > 1 && speed < 2.2, "dam break runs, conserves volume, front speed sane",
			std::to_string(speed));
		// Wall impact in Lobovsky's tank (the square column, H = 600 mm): reported against their sensor statistics.
		const DamBreakImpactResult im = RunDamBreakImpact(n, 1.4, o, pool);
		std::printf("|     impact wall at 1.683 a from the gate, %d cells per a, %d steps, %.1f s; sensor-1 peak at T = %.2f\n",
			n, im.steps, im.seconds, im.peakTime);
		for (int q = 0; q < 4; ++q) {
			const double rg = 9.81*600e-3*1000/100.0;   // rho g H in mbar (H = 0.6 m)
			const double* e = LobovskyImpact600[q];
			std::printf("|       sensor %d (%2.0f mm): P* %.2f (wall cell, includes hydrostatic); experiment median %.2f, 2.5-97.5 %% %.2f-%.2f\n",
				q + 1, e[0], im.peak[q], e[1]/rg, e[2]/rg, e[3]/rg);
		}
		Check(im.finite && im.peak[0] > 0, "dam break impact run finite", std::to_string(im.peak[0]));
	}
}

// ------------------------------------------------------------------ mc

void Surface(Pool* pool)
{
	std::printf("\nmc. Marching cubes on a sphere's distance (R = 0.3) and on random fields\n");
	std::vector<double> ae, ve;
	for (int n : {16, 32, 64}) {
		const std::array<int,3> d{{n, n, n}};
		const double h = 1.0/(n - 1), R = 0.3;
		std::vector<double> f(std::size_t(n)*n*n);
		for (int k = 0; k < n; ++k) for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i)
			f[std::size_t(i) + std::size_t(n)*(std::size_t(j) + std::size_t(n)*std::size_t(k))] =
				std::sqrt((i*h - 0.5)*(i*h - 0.5) + (j*h - 0.5)*(j*h - 0.5) + (k*h - 0.5)*(k*h - 0.5)) - R;
		IsoSurface s;
		MarchingCubes(d, f, {{0, 0, 0}}, h, s, pool);
		double area = 0, vol = 0;
		std::size_t outward = 0;
		std::map<std::pair<std::uint32_t,std::uint32_t>, int> edges;
		for (std::size_t t = 0; t < s.triangles.size(); t += 3) {
			const double* a = &s.positions[3*s.triangles[t]];
			const double* b = &s.positions[3*s.triangles[t + 1]];
			const double* c = &s.positions[3*s.triangles[t + 2]];
			const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
			const double nn[3] = {u[1]*v[2] - u[2]*v[1], u[2]*v[0] - u[0]*v[2], u[0]*v[1] - u[1]*v[0]};
			area += 0.5*std::sqrt(nn[0]*nn[0] + nn[1]*nn[1] + nn[2]*nn[2]);
			vol += (a[0]*(b[1]*c[2] - b[2]*c[1]) - a[1]*(b[0]*c[2] - b[2]*c[0]) + a[2]*(b[0]*c[1] - b[1]*c[0]))/6;
			const double cx = (a[0] + b[0] + c[0])/3 - 0.5, cy = (a[1] + b[1] + c[1])/3 - 0.5, cz = (a[2] + b[2] + c[2])/3 - 0.5;
			if (nn[0]*cx + nn[1]*cy + nn[2]*cz > 0) ++outward;
			for (int m = 0; m < 3; ++m) edges[{s.triangles[t + std::size_t(m)], s.triangles[t + std::size_t((m + 1) % 3)]}] += 1;
		}
		bool closed = true;
		for (const auto& kv : edges) { const auto r = edges.find({kv.first.second, kv.first.first}); closed = closed && kv.second == 1 && r != edges.end() && r->second == 1; }
		Check(closed && outward == s.triangles.size()/3, "sphere surface closed and outward at n = " + std::to_string(n));
		ae.push_back(area/(4*Pi*R*R) - 1);
		ve.push_back(vol/(4.0/3*Pi*R*R*R) - 1);
		std::printf("|   n=%2d: %zu triangles, area error %+.2e, volume error %+.2e\n", n, s.triangles.size()/3, ae.back(), ve.back());
	}
	OrderRow("sphere area", ae[1], ae[2], 1.8);
	OrderRow("sphere volume", ve[1], ve[2], 1.8);
	std::mt19937_64 rng(7);
	std::uniform_real_distribution<double> U(-1, 1);
	int open = 0;
	for (int trial = 0; trial < 300; ++trial) {
		const int n = 8;
		std::vector<double> f(std::size_t(n)*n*n);
		for (int k = 0; k < n; ++k) for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i)
			f[std::size_t(i + n*(j + n*k))] = (i == 0 || j == 0 || k == 0 || i == n - 1 || j == n - 1 || k == n - 1) ? 1.0 : U(rng);
		IsoSurface s;
		MarchingCubes({{n, n, n}}, f, {{0, 0, 0}}, 1.0, s, pool);
		std::map<std::pair<std::uint32_t,std::uint32_t>, int> edges;
		for (std::size_t t = 0; t < s.triangles.size(); t += 3)
			for (int m = 0; m < 3; ++m) edges[{s.triangles[t + std::size_t(m)], s.triangles[t + std::size_t((m + 1) % 3)]}] += 1;
		bool closed = true;
		for (const auto& kv : edges) { const auto r = edges.find({kv.first.second, kv.first.first}); closed = closed && kv.second == 1 && r != edges.end() && r->second == 1; }
		if (!closed) ++open;
	}
	std::printf("|   300 random fields (ambiguous faces included): %d not closed\n", open);
	Check(open == 0, "random fields give closed, consistently oriented surfaces");
}
} // namespace

int main(int argc, char** argv)
{
	bool extended = false;
	for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--extended") == 0) extended = true;
	if (const char* e = std::getenv("RISE_TEST_EXTENDED")) if (std::strcmp(e, "1") == 0) extended = true;
	Pool pool(8);
	const double t0 = Seconds();
	std::printf("Liquid validation (%s; 8 threads; results do not depend on the count)\n", extended ? "extended" : "fast tier");
	Surface(&pool);
	Transport(extended, &pool);
	ReinitSchemes(&pool);
	Hydrostatic(&pool);
	Momentum(extended, &pool);
	Waves(extended, &pool);
	SolidBoundary(extended, &pool);
	Volume(extended, &pool);
	DamBreak(extended, &pool);
	std::printf("\n%s (%d failures, %.1f s)\n", failures ? "FAILED" : "PASSED", failures, Seconds() - t0);
	return failures ? 1 : 0;
}
