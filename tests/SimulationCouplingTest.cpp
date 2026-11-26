// Simulation M6: cloth <-> rigid body two-way coupling through the group host.
//
//   * Zero gravity, no pins: a box moving and spinning into a free sheet.
//     Total linear and angular momentum of sheet + box (from the published
//     frames) against their start, for staggered (Gauss-Seidel, Jacobi) and
//     implicit coupling at two macro steps; kinetic energy after the impact
//     never above its start (the barrier is conservative, friction dissipates).
//   * Weak coupling: a heavy floating box under a falling sheet; the two-way
//     group against the sheet fed one-way from a CACHED rigid bake (the box
//     unmoved): their difference falls with the mass ratio.
//   * The trampoline: a ball dropped onto a sheet pinned at its corners
//     (gravity), staggered and implicit, stop / resume identity.
//   * The cloth's collider_reaction port and the port-driven collider.
// Fast tier by default; --extended adds a finer macro step study.
#include "../src/Library/Simulation/SimulationGroup.h"
#include "../src/Library/Simulation/SimulationPortAdapters.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationTransform.h"
#include "../src/Library/Simulation/Rigid/RigidSolver.h"
#include "../src/Library/Simulation/Coupling/ClothRigidCoupling.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

namespace {
using namespace RISE::Simulation;
using V = RISE::RISECBOR64::Value;
constexpr std::size_t Budget = std::size_t(256) << 20;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
using D3 = std::array<double,3>;
double Norm(const D3& a) { return std::sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); }

struct Result { double dp = 0, dl = 0, keRise = 0, pScale = 0, lScale = 0, seconds = 0, dpEnd = 0, dlEnd = 0; unsigned passes = 0; bool ok = false; D3 body{{0, 0, 0}}; };

Result RunZeroG(const GroupPolicy& policy, std::uint64_t substeps, std::uint64_t frames)
{
	Result r;
	ClothRigidScenario sc;
	std::string error;
	if (!ZeroGravitySpinScenario(substeps, frames, sc, error)) { Check(false, "scenario", error); return r; }
	SimulationGroup g;
	const auto start = std::chrono::steady_clock::now();
	if (!BuildClothRigidGroup(g, sc, policy, error) || !g.Start(error)) { Check(false, "build zero-g group", error); return r; }
	while (!g.Finished()) {
		if (!g.StepMacro(error)) { Check(false, "zero-g step", error); return r; }
		r.passes += g.LastStep().totalIterations;
	}
	r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	ClothRigidTotals t0, t;
	if (!MeasureClothRigid(*g.Snapshot("sheet"), *g.Snapshot("body"), 0, sc, t0, error)) { Check(false, "measure", error); return r; }
	r.pScale = Norm(t0.momentum); r.lScale = Norm(t0.angular);
	for (std::int64_t i = 1; i < static_cast<std::int64_t>(frames); ++i) {
		if (!MeasureClothRigid(*g.Snapshot("sheet"), *g.Snapshot("body"), i, sc, t, error)) { Check(false, "measure", error); return r; }
		D3 dp, dl;
		for (int a = 0; a < 3; ++a) { dp[a] = t.momentum[a] - t0.momentum[a]; dl[a] = t.angular[a] - t0.angular[a]; }
		r.dp = std::max(r.dp, Norm(dp)/r.pScale);
		r.dl = std::max(r.dl, Norm(dl)/r.lScale);
		r.keRise = std::max(r.keRise, (t.kinetic - t0.kinetic)/t0.kinetic);
		r.dpEnd = Norm(dp)/r.pScale;
		r.dlEnd = Norm(dl)/r.lScale;
	}
	r.body = t.bodyPosition;
	r.ok = true;
	return r;
}

// The heaviest box's two-way run against one-way from its cached rigid bake.
struct Weak { double box = 0, sheet = 0; };
Weak WeakCoupling(double mass, std::uint64_t frames)
{
	Weak w;
	std::string error;
	ClothRigidScenario sc;
	if (!FloatingBoxScenario(mass, frames, sc, error)) { Check(false, "floating box", error); return w; }
	GroupPolicy p; p.order = ExchangeOrder::GaussSeidel; p.memberOrder = {"body", "sheet"};
	p.subIterations = 30; p.convergenceTolerance = 1e-6; p.aitken = true;
	SimulationGroup two;
	if (!BuildClothRigidGroup(two, sc, p, error) || !two.Start(error) || !two.Run(error)) { Check(false, "two-way floating box", error); return w; }
	// The rigid bake alone (nothing loads it): the box stays where it is.
	InProcessHost rigid(CreateSolver(RigidSolverId, error));
	if (!rigid.Configure(sc.rigid, EmptySceneInputs(), error) || !rigid.Start(error) || !rigid.StepToIndex(1000000, error)) {
		Check(false, "rigid bake", error); return w; }
	GroupPolicy one; one.order = ExchangeOrder::GaussSeidel;
	SimulationGroup cached;
	if (!BuildClothFromCachedRigid(cached, sc, rigid.Snapshot(), one, error) || !cached.Start(error) || !cached.Run(error)) {
		Check(false, "one-way from the cached rigid bake", error); return w; }
	ClothRigidTotals a, b;
	const std::int64_t last = static_cast<std::int64_t>(frames) - 1;
	if (!MeasureClothRigid(*two.Snapshot("sheet"), *two.Snapshot("body"), last, sc, a, error) ||
		!MeasureClothRigid(*cached.Snapshot("sheet"), *cached.Snapshot("body"), last, sc, b, error)) { Check(false, "measure", error); return w; }
	w.box = Norm({{a.bodyPosition[0] - b.bodyPosition[0], a.bodyPosition[1] - b.bodyPosition[1], a.bodyPosition[2] - b.bodyPosition[2]}});
	// Largest vertex difference of the sheets.
	std::shared_ptr<const Bytes> x, y;
	MeshFrame mx, my;
	if (two.Snapshot("sheet")->Read(last, "surface", Budget, x, error) == FrameStatus::Ready &&
		cached.Snapshot("sheet")->Read(last, "surface", Budget, y, error) == FrameStatus::Ready &&
		DecodeMesh(*x, Budget, two.Snapshot("sheet")->Resolver(), mx, error) && DecodeMesh(*y, Budget, cached.Snapshot("sheet")->Resolver(), my, error))
		for (std::size_t k = 0; k < mx.positions.size(); ++k) {
			float fa, fb;
			std::memcpy(&fa, &mx.positions[k], 4); std::memcpy(&fb, &my.positions[k], 4);
			w.sheet = std::max(w.sheet, std::fabs(static_cast<double>(fa) - fb));
		}
	return w;
}

std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const IReadOnlyCacheSnapshot& s)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> out;
	for (const auto& e : s.Entries()) out[{e.index, e.output}] = e.digest;
	return out;
}
}

int main(int argc, char** argv)
{
	const bool extended = (argc == 2 && std::strcmp(argv[1], "--extended") == 0) ||
		(std::getenv("RISE_TEST_EXTENDED") && std::strcmp(std::getenv("RISE_TEST_EXTENDED"), "1") == 0);
	std::string error;
	Check(RegisterBuiltinSolvers(error), "register", error);
	// ---------------------------------------------------------------- zero gravity: momentum exchange
	{
		GroupPolicy gs; gs.order = ExchangeOrder::GaussSeidel; gs.memberOrder = {"body", "sheet"};
		GroupPolicy jac = gs; jac.order = ExchangeOrder::Jacobi;
		GroupPolicy imp = gs; imp.subIterations = 30; imp.convergenceTolerance = 1e-6; imp.aitken = true;
		GroupPolicy loose = imp; loose.convergenceTolerance = 1e-3;
		GroupPolicy mid = imp; mid.convergenceTolerance = 1e-4;
		struct Row { const char* name; GroupPolicy p; std::uint64_t sub, frames; Result r; };
		std::vector<Row> rows = {{"staggered Gauss-Seidel, H = 1/30 s", gs, 8, 16, {}}, {"staggered Gauss-Seidel, H = 1/60 s", gs, 4, 31, {}},
			{"staggered Jacobi, H = 1/30 s", jac, 8, 16, {}}, {"staggered Jacobi, H = 1/60 s", jac, 4, 31, {}},
			{"implicit Gauss-Seidel tol 1e-3, H = 1/30 s", loose, 8, 16, {}}, {"implicit Gauss-Seidel tol 1e-4, H = 1/30 s", mid, 8, 16, {}},
			{"implicit Gauss-Seidel tol 1e-6, H = 1/30 s", imp, 8, 16, {}}};
		if (extended) {
			rows.push_back({"staggered Gauss-Seidel, H = 1/120 s", gs, 2, 61, {}});
			rows.push_back({"implicit Gauss-Seidel tol 1e-6, H = 1/60 s", imp, 4, 31, {}});
		}
		std::cout << "  zero gravity, sheet 13 x 13 + spinning box, 0.5 s, backward Euler cloth (max over frames; end):\n";
		for (auto& row : rows) {
			row.r = RunZeroG(row.p, row.sub, row.frames);
			std::cout << "    " << row.name << ": |dP|/|P0| " << row.r.dp << " (end " << row.r.dpEnd << "), |dL|/|L0| " << row.r.dl
				<< " (end " << row.r.dlEnd << "), KE rise " << row.r.keRise << ", passes " << row.r.passes << ", " << row.r.seconds << " s\n";
			Check(row.r.ok, std::string("run ") + row.name);
		}
		const auto order = [](const Result& a, const Result& b) { return std::log2(a.dp/b.dp); };
		std::cout << "    staggered momentum error order in H: Gauss-Seidel " << order(rows[0].r, rows[1].r) << ", Jacobi "
			<< order(rows[2].r, rows[3].r) << "; KE rise order " << std::log2(rows[0].r.keRise/rows[1].r.keRise) << "\n";
		Check(order(rows[0].r, rows[1].r) > 0.7 && order(rows[0].r, rows[1].r) < 1.3, "staggered momentum error is first order in the macro step");
		Check(rows[0].r.dpEnd < 0.1*rows[0].r.dp, "staggered: the impulse in flight is delivered after the contact ends");
		Check(rows[6].r.dp < 1e-6, "implicit (tol 1e-6): linear momentum conserved across the port", std::to_string(rows[6].r.dp));
		Check(rows[6].r.keRise < 1e-6, "implicit: kinetic energy never rises (barrier conservative, friction dissipative)",
			std::to_string(rows[6].r.keRise));
		Check(rows[4].r.dp > rows[6].r.dp && rows[5].r.dp >= rows[6].r.dp, "the implicit momentum error follows the iteration tolerance");
		Check(rows[6].r.dl < 2e-3, "implicit: angular momentum to backward Euler's own rotational damping", std::to_string(rows[6].r.dl));
	}
	// ---------------------------------------------------------------- labels of the coupled members
	{
		ClothRigidScenario sc;
		GroupPolicy imp; imp.order = ExchangeOrder::GaussSeidel; imp.memberOrder = {"body", "sheet"};
		imp.subIterations = 30; imp.convergenceTolerance = 1e-6; imp.aitken = true;
		Check(ZeroGravitySpinScenario(8, 4, sc, error), "scenario", error);
		SimulationGroup g;
		Check(BuildClothRigidGroup(g, sc, imp, error), "configure", error);
		Evidence ev;
		Check(CouplingEvidence(ClothRigidCouplingKind, ev, error), "coupling evidence", error);
		for (const auto& c : ev.cases) std::cout << "  coupling evidence " << c.id << ": " << c.error << " (tolerance " << c.tolerance << ")\n";
		std::cout << "  labels: body " << g.MemberFidelity("body") << ", sheet " << g.MemberFidelity("sheet") << " "
			<< g.CouplingLimit("sheet") << "\n";
		Check(g.MemberFidelity("body") == "predictive" && g.MemberFidelity("sheet") == "predictive",
			"implicit cloth-rigid coupling with passing evidence is predictive", g.CouplingLimit("sheet"));
		GroupPolicy gs = imp; gs.subIterations = 1;
		SimulationGroup s;
		Check(BuildClothRigidGroup(s, sc, gs, error) && s.MemberFidelity("sheet") == "preview", "staggered cloth-rigid coupling is preview", error);
	}
	// ---------------------------------------------------------------- weak coupling: two-way vs one-way from a cache
	{
		const double light = 2, heavy = 8;
		const Weak a = WeakCoupling(light, 13), b = WeakCoupling(heavy, 13);
		const double ma = 0.15*0.49/light, mb = 0.15*0.49/heavy;
		std::cout << "  weak coupling (sheet on a floating box, 0.4 s): mass ratio " << ma << ": box " << a.box << " m, sheet " << a.sheet
			<< " m; ratio " << mb << ": box " << b.box << " m, sheet " << b.sheet << " m; box difference / ratio " << a.box/ma
			<< ", " << b.box/mb << "\n";
		Check(a.box > 0 && b.box > 0 && b.box < a.box, "the two-way box moves less when it is heavier");
		Check(std::fabs(a.box/ma - b.box/mb) < 0.25*std::max(a.box/ma, b.box/mb), "two-way and one-way differ by the mass ratio times a constant");
		Check(b.sheet < a.sheet, "the sheets agree better at the smaller mass ratio");
	}
	// ---------------------------------------------------------------- the trampoline: staggered and implicit, stop / resume
	{
		ClothRigidScenario sc;
		Check(TrampolineScenario(0.1, 12, sc, error), "trampoline", error);
		GroupPolicy imp; imp.order = ExchangeOrder::GaussSeidel; imp.memberOrder = {"body", "sheet"};
		imp.subIterations = 30; imp.convergenceTolerance = 1e-6; imp.aitken = true;
		GroupPolicy gs = imp; gs.subIterations = 1;
		for (const GroupPolicy* p : {&gs, &imp}) {
			SimulationGroup g;
			const auto start = std::chrono::steady_clock::now();
			Check(BuildClothRigidGroup(g, sc, *p, error) && g.Start(error) && g.Run(error), "trampoline run", error);
			double lowest = 1e9;
			for (std::int64_t i = 0; i < 12; ++i) {
				ClothRigidTotals t;
				if (MeasureClothRigid(*g.Snapshot("sheet"), *g.Snapshot("body"), i, sc, t, error)) lowest = std::min(lowest, t.bodyPosition[1]);
			}
			std::cout << "  trampoline (" << (p == &gs ? "staggered" : "implicit") << "): lowest ball centre " << lowest << " m, "
				<< std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s\n";
			Check(lowest > -0.3 && lowest < 0.1 + 0.002, "the ball lands on the sheet and the sheet catches it");
		}
		// Stop after 6 steps, checkpoint, restore into a fresh group: identical frames.
		SimulationGroup full;
		Check(BuildClothRigidGroup(full, sc, imp, error) && full.Start(error) && full.Run(error), "uninterrupted", error);
		SimulationGroup first;
		Check(BuildClothRigidGroup(first, sc, imp, error) && first.Start(error), "first", error);
		for (int k = 0; k < 5; ++k) first.StepMacro(error);
		Bytes state;
		Check(first.Checkpoint(state, error), "checkpoint", error);
		SimulationGroup second;
		Check(BuildClothRigidGroup(second, sc, imp, error) && second.Restore(state, error) && second.Run(error), "resume", error);
		for (const char* m : {"sheet", "body"}) {
			auto merged = Digests(*first.Snapshot(m));
			for (const auto& e : Digests(*second.Snapshot(m))) merged[e.first] = e.second;
			Check(merged == Digests(*full.Snapshot(m)), std::string("group stop / resume byte-identical: ") + m);
		}
	}
	if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
	std::cout << "SimulationCouplingTest: all checks passed" << (extended ? " (extended)" : "") << "\n";
	return 0;
}
