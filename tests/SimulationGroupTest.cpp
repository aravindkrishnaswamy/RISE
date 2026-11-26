// Simulation M6 group host (SimulationGroup.h) with test solvers: explicit
// staggered exchange (Jacobi and Gauss-Seidel) at first order in the macro
// step, implicit sub-iteration converging to a second-order coupled scheme,
// under-relaxation with Aitken, failure and step halving on non-convergence,
// checkpoint / restore byte identity, held-back frames, keys through the
// coupling (one-way upstream keeps its standalone key), labels, an automatic
// unit adapter, a cached member equal to a live upstream, and the reference
// solvers in a group equal to a CachedPortFeed run.
#include "../src/Library/Simulation/SimulationGroup.h"
#include "../src/Library/Simulation/SimulationPortAdapters.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "SimulationCouplingSolvers.h"
#include "SimulationReferenceSolvers.h"

#include <cmath>
#include <iostream>
#include <map>

namespace {
using namespace RISE::Simulation;
using namespace RISE::Simulation::Reference;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}

std::unique_ptr<ISimulationSolver> Make(const char* id)
{
	std::string error;
	auto s = CreateSolver(id, error);
	Check(s != nullptr, "create " + std::string(id), error);
	return s;
}

// Problem: ma = 1, ka = 4, mb = 2, kb = 3, kc = 2; ua(0) = 0.1, ub(0) = 0, va = 0, vb = 0.5.
const double MA = 1, KA = 4, MB = 2, KB = 3, KC = 2, UA = 0.1, VB = 0.5;

V Oscillator(double mass, double k, double kc, double u, double v, double interval, std::uint64_t frames,
	std::uint64_t substeps, const char* units = "m")
{
	return V::MapValue({{"mass", V::Float(mass)}, {"stiffness", V::Float(k)}, {"coupling", V::Float(kc)},
		{"displacement", V::Float(u)}, {"velocity", V::Float(v)}, {"interval", V::Float(interval)},
		{"frames", V::Unsigned(frames)}, {"substeps", V::Unsigned(substeps)}, {"units", V::String(units)}});
}

struct Built {
	std::unique_ptr<SimulationGroup> group;
	bool ok = false;
};

Built Pair(const GroupPolicy& policy, double interval, std::uint64_t frames, std::uint64_t substeps, double kc = KC,
	const char* unitsB = "m")
{
	Built b;
	b.group = std::make_unique<SimulationGroup>();
	std::string error;
	auto& g = *b.group;
	b.ok = g.AddMember("a", Make(OscillatorSolver::Id), Oscillator(MA, KA, kc, UA, 0, interval, frames, substeps), error) &&
		g.AddMember("b", Make(OscillatorSolver::Id), Oscillator(MB, KB, kc, 0, VB, interval, frames, substeps, unitsB), error) &&
		g.Connect({"a", "state", "b", "partner"}, error) && g.Connect({"b", "state", "a", "partner"}, error) &&
		g.Configure(policy, EmptySceneInputs(), error) && g.Start(error);
	Check(b.ok, "build the oscillator pair", error);
	return b;
}

double LastDisplacement(const SimulationGroup& g, const std::string& member, double scale = 1)
{
	const auto s = g.Snapshot(member);
	std::int64_t last = -1;
	for (const auto& e : s->Entries()) last = std::max(last, e.index);
	std::shared_ptr<const Bytes> bytes;
	std::string error;
	FieldFrame f;
	if (s->Read(last, "state", CodecBudget, bytes, error) != FrameStatus::Ready || !DecodeField(*bytes, CodecBudget, f, error)) return NAN;
	return FromF32(f.channels[0].values[0])/scale;
}

// Error at T = 2 s against the exact solution (both masses), for a macro step H = interval.
double PairError(GroupPolicy policy, double H, unsigned* iterations = nullptr)
{
	const std::uint64_t frames = static_cast<std::uint64_t>(std::llround(2.0/H)) + 1;
	auto built = Pair(policy, H, frames, 40);
	std::string error;
	unsigned total = 0;
	while (built.ok && !built.group->Finished()) {
		if (!built.group->StepMacro(error)) { Check(false, "step", error); return NAN; }
		total += built.group->LastStep().totalIterations;
	}
	if (iterations) *iterations = total;
	double ea, eb;
	TwoOscillatorExact(MA, KA, MB, KB, KC, UA, 0, 0, VB, 2.0, ea, eb);
	return std::max(std::fabs(LastDisplacement(*built.group, "a") - ea), std::fabs(LastDisplacement(*built.group, "b") - eb));
}

std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const IReadOnlyCacheSnapshot& s)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> out;
	for (const auto& e : s.Entries()) out[{e.index, e.output}] = e.digest;
	return out;
}
}

int main()
{
	RegisterReferenceSolvers();
	RegisterCouplingTestSolvers();
	std::string error;
	Check(RegisterBuiltinPortAdapters(error), "register adapters", error);
	// ---------------------------------------------------------------- orders of the exchange schemes
	{
		GroupPolicy jacobi; jacobi.order = ExchangeOrder::Jacobi;
		GroupPolicy gs; gs.order = ExchangeOrder::GaussSeidel;
		GroupPolicy implicitGS; implicitGS.order = ExchangeOrder::GaussSeidel; implicitGS.subIterations = 50;
		implicitGS.convergenceTolerance = 1e-7;   // the exchanged field.v1 samples are binary32
		GroupPolicy implicitJ = implicitGS; implicitJ.order = ExchangeOrder::Jacobi;
		const double H = 0.04;
		struct Row { const char* name; GroupPolicy p; double e1, e2; unsigned it; };
		Row rows[] = {{"staggered Jacobi", jacobi, 0, 0, 0}, {"staggered Gauss-Seidel", gs, 0, 0, 0},
			{"implicit Gauss-Seidel", implicitGS, 0, 0, 0}, {"implicit Jacobi", implicitJ, 0, 0, 0}};
		for (auto& r : rows) {
			r.e1 = PairError(r.p, H, &r.it);
			r.e2 = PairError(r.p, H/2);
			std::cout << "  " << r.name << ": error " << r.e1 << " (H = " << H << "), " << r.e2 << " (H/2), order "
				<< std::log2(r.e1/r.e2) << ", " << r.it << " passes\n";
		}
		Check(std::log2(rows[0].e1/rows[0].e2) > 0.8 && std::log2(rows[0].e1/rows[0].e2) < 1.3, "staggered Jacobi is first order");
		Check(std::log2(rows[1].e1/rows[1].e2) > 0.8 && std::log2(rows[1].e1/rows[1].e2) < 1.3, "staggered Gauss-Seidel is first order");
		Check(std::log2(rows[2].e1/rows[2].e2) > 1.8, "implicit Gauss-Seidel is second order");
		Check(std::log2(rows[3].e1/rows[3].e2) > 1.8, "implicit Jacobi is second order");
		Check(std::fabs(rows[2].e1 - rows[3].e1) < 1e-6, "both implicit orders converge to the same coupled solution");
		Check(rows[2].e1 < 0.1*rows[1].e1, "implicit is far more accurate than staggered at the same step");
		Check(rows[2].it < rows[3].it, "Gauss-Seidel sub-iteration converges in fewer passes than Jacobi");
		// One pass of "implicit" is the explicit scheme.
		GroupPolicy once = gs; once.subIterations = 1;
		Check(PairError(once, H) == rows[1].e1, "subIterations 1 equals the explicit scheme");
	}
	// ---------------------------------------------------------------- relaxation, Aitken, non-convergence, halving
	{
		// Strong coupling: kc = 4000 at H = 0.05 makes the plain fixed point diverge.
		GroupPolicy plain; plain.order = ExchangeOrder::GaussSeidel; plain.subIterations = 60; plain.convergenceTolerance = 1e-6;
		auto a = Pair(plain, 0.05, 11, 40, 4000);
		Check(a.ok && !a.group->StepMacro(error) && error.find("did not converge") != std::string::npos,
			"non-convergence fails the step with a diagnostic", error);
		std::cout << "  diagnostic: " << error.substr(0, 160) << "...\n";
		GroupPolicy aitken = plain; aitken.relaxation = 0.5; aitken.aitken = true;
		auto b = Pair(aitken, 0.05, 11, 40, 4000);
		Check(b.ok && b.group->StepMacro(error), "Aitken relaxation converges where the plain iteration diverges", error);
		std::cout << "  Aitken: " << b.group->LastStep().iterations << " passes, residual " << b.group->LastStep().residual << "\n";
		GroupPolicy halve = plain; halve.subIterations = 12; halve.onNonConvergence = GroupPolicy::NonConvergence::Halve;
		halve.minimumMacroStep = 0.05/64;
		auto c = Pair(halve, 0.05, 11, 40, 4000);
		Check(c.ok && c.group->StepMacro(error) && c.group->LastStep().halvings > 0, "halving the step converges", error);
		std::cout << "  halving: " << c.group->LastStep().halvings << " halvings, " << c.group->LastStep().totalIterations << " passes\n";
		Check(c.group->Time() == 0.05 && c.group->Snapshot("a")->EntryCount() == 2, "the halved step ends on the macro grid with one new frame");
		GroupPolicy floor = halve; floor.minimumMacroStep = 0.04;
		auto d = Pair(floor, 0.05, 11, 40, 4000);
		Check(d.ok && !d.group->StepMacro(error), "halving stops at the floor and fails", error);
	}
	// ---------------------------------------------------------------- checkpoint / restore, held frames
	{
		GroupPolicy p; p.subIterations = 20; p.convergenceTolerance = 1e-6; p.relaxation = 0.7; p.aitken = true;
		auto full = Pair(p, 0.05, 21, 10);
		Check(full.group->Run(error) && full.group->Finished(), "uninterrupted implicit run", error);
		const auto ra = Digests(*full.group->Snapshot("a")), rb = Digests(*full.group->Snapshot("b"));
		Check(ra.size() == 21 && rb.size() == 21, "every frame published once (held frames committed on acceptance)");
		auto first = Pair(p, 0.05, 21, 10);
		for (int k = 0; k < 7; ++k) first.group->StepMacro(error);
		Bytes state;
		Check(first.group->Checkpoint(state, error), "group checkpoint", error);
		SimulationGroup second;
		Check(second.AddMember("a", Make(OscillatorSolver::Id), Oscillator(MA, KA, KC, UA, 0, 0.05, 21, 10), error) &&
			second.AddMember("b", Make(OscillatorSolver::Id), Oscillator(MB, KB, KC, 0, VB, 0.05, 21, 10), error) &&
			second.Connect({"a", "state", "b", "partner"}, error) && second.Connect({"b", "state", "a", "partner"}, error) &&
			second.Configure(p, EmptySceneInputs(), error) && second.Restore(state, error) && second.Run(error), "restore and finish", error);
		auto ma = Digests(*first.group->Snapshot("a")), mb = Digests(*first.group->Snapshot("b"));
		for (const auto& e : Digests(*second.Snapshot("a"))) ma[e.first] = e.second;
		for (const auto& e : Digests(*second.Snapshot("b"))) mb[e.first] = e.second;
		Check(ma == ra && mb == rb, "stop / resume of the whole group is byte-identical");
		// M7 prerequisites: the group streamed (index + member streams) resumes identically.
		MemoryCheckpointSink sink;
		Check(first.group->CheckpointTo(sink, error), "streamed group checkpoint", error);
		SimulationGroup third;
		MemoryCheckpointSource source(sink.bytes);
		Check(third.AddMember("a", Make(OscillatorSolver::Id), Oscillator(MA, KA, KC, UA, 0, 0.05, 21, 10), error) &&
			third.AddMember("b", Make(OscillatorSolver::Id), Oscillator(MB, KB, KC, 0, VB, 0.05, 21, 10), error) &&
			third.Connect({"a", "state", "b", "partner"}, error) && third.Connect({"b", "state", "a", "partner"}, error) &&
			third.Configure(p, EmptySceneInputs(), error) && third.RestoreFrom(source, error) && third.Run(error),
			"restore the streamed group and finish", error);
		auto sa = Digests(*first.group->Snapshot("a")), sb = Digests(*first.group->Snapshot("b"));
		for (const auto& e : Digests(*third.Snapshot("a"))) sa[e.first] = e.second;
		for (const auto& e : Digests(*third.Snapshot("b"))) sb[e.first] = e.second;
		Check(sa == ra && sb == rb, "stop / resume of the group through its stream is byte-identical");
		Bytes cut(sink.bytes.begin(), sink.bytes.end() - 3);
		MemoryCheckpointSource shortSource(cut);
		SimulationGroup fourth;
		Check(fourth.AddMember("a", Make(OscillatorSolver::Id), Oscillator(MA, KA, KC, UA, 0, 0.05, 21, 10), error) &&
			fourth.AddMember("b", Make(OscillatorSolver::Id), Oscillator(MB, KB, KC, 0, VB, 0.05, 21, 10), error) &&
			fourth.Connect({"a", "state", "b", "partner"}, error) && fourth.Connect({"b", "state", "a", "partner"}, error) &&
			fourth.Configure(p, EmptySceneInputs(), error) && !fourth.RestoreFrom(shortSource, error), "a truncated group stream is refused");
		// A state of another configuration is refused.
		SimulationGroup other;
		GroupPolicy q = p; q.convergenceTolerance = 1e-5;
		Check(other.AddMember("a", Make(OscillatorSolver::Id), Oscillator(MA, KA, KC, UA, 0, 0.05, 21, 10), error) &&
			other.AddMember("b", Make(OscillatorSolver::Id), Oscillator(MB, KB, KC, 0, VB, 0.05, 21, 10), error) &&
			other.Connect({"a", "state", "b", "partner"}, error) && other.Connect({"b", "state", "a", "partner"}, error) &&
			other.Configure(q, EmptySceneInputs(), error) && !other.Restore(state, error), "a foreign group state is refused");
	}
	// ---------------------------------------------------------------- keys and labels
	{
		GroupPolicy gs;
		SolverDescription alone;
		Check(Make(OscillatorSolver::Id)->Configure(Oscillator(MA, KA, KC, UA, 0, 0.05, 21, 10), EmptySceneInputs(), alone, error) &&
			PrepareCacheDescription(alone, EmptySceneInputs(), "", error), "standalone", error);
		// One-way a -> b: a keeps its standalone key, b's key covers a.
		auto oneWay = [&](double kcA, double kbB, const GroupPolicy& p, std::string& keyA, std::string& keyB) {
			SimulationGroup g;
			bool ok = g.AddMember("a", Make(OscillatorSolver::Id), Oscillator(MA, KA, kcA, UA, 0, 0.05, 21, 10), error) &&
				g.AddMember("b", Make(OscillatorSolver::Id), Oscillator(MB, kbB, KC, 0, VB, 0.05, 21, 10), error) &&
				g.Connect({"a", "state", "b", "partner"}, error) && g.Configure(p, EmptySceneInputs(), error);
			keyA = ok ? g.Description("a")->cache.key : ""; keyB = ok ? g.Description("b")->cache.key : "";
			return ok;
		};
		std::string a1, b1, a2, b2, a3, b3, a4, b4;
		Check(oneWay(KC, KB, gs, a1, b1), "one-way group", error);
		Check(a1 == alone.cache.key, "the upstream of a one-way coupling keeps its standalone key");
		Check(b1 != a1 && b1 != alone.cache.key, "the downstream key covers the coupling");
		Check(oneWay(KC*1.5, KB, gs, a2, b2) && b2 != b1, "an upstream change makes the downstream stale");
		Check(oneWay(KC, KB*1.5, gs, a3, b3) && a3 == a1 && b3 != b1, "a downstream change leaves the upstream key alone");
		GroupPolicy jac; jac.order = ExchangeOrder::Jacobi;
		Check(oneWay(KC, KB, jac, a4, b4) && a4 == a1 && b4 != b1, "the policy is part of the coupled key");
		// Two-way: both keys change with either member.
		auto pair1 = Pair(gs, 0.05, 21, 10);
		auto pair2 = Pair(gs, 0.05, 21, 10, KC*1.5);
		Check(pair1.group->Description("a")->cache.key != pair2.group->Description("a")->cache.key &&
			pair1.group->Description("b")->cache.key != pair2.group->Description("b")->cache.key, "two-way: both keys follow both members");
		// Labels: staggered is preview; implicit with registered passing evidence is predictive.
		Check(pair1.group->MemberFidelity("a") == "preview" && !pair1.group->CouplingLimit("a").empty(), "staggered coupling is preview");
		Check(RegisterCouplingEvidence("direct", [](Evidence& e, std::string&) {
			EvidenceCase c; c.id = "test-coupling"; c.error = 0; c.tolerance = 1; c.passed = true;
			c.digest = EvidenceDigest(c.id, V::MapValue({}), 0, 1, true);
			e.fidelity = "predictive"; e.cases = {c}; e.conditions = {{"problem", {"oscillators"}}}; e.scope = "test coupling";
			return true; }, error), "register test coupling evidence", error);
		GroupPolicy imp; imp.subIterations = 20; imp.convergenceTolerance = 1e-6;
		auto pair3 = Pair(imp, 0.05, 21, 10);
		Check(pair3.group->MemberFidelity("a") == "predictive" && pair3.group->MemberFidelity("b") == "predictive",
			"implicit coupling with passing evidence is predictive", pair3.group->CouplingLimit("a"));
		const auto& cond = pair3.group->Description("a")->cache.evidenceConditions;
		bool hasCoupling = false;
		for (const auto& c : cond) hasCoupling = hasCoupling || (c.first == "coupling" && c.second == std::vector<std::string>{"implicit-gauss-seidel"});
		Check(hasCoupling, "the coupling condition joins the member's evidence");
		GroupPolicy loose = imp; loose.convergenceTolerance = 1e-4;
		auto pair4 = Pair(loose, 0.05, 21, 10);
		Check(pair4.group->MemberFidelity("a") == "preview", "a loose implicit tolerance is preview");
	}
	// ---------------------------------------------------------------- automatic unit adapter
	{
		GroupPolicy gs; gs.subIterations = 30; gs.convergenceTolerance = 1e-6;
		auto metres = Pair(gs, 0.05, 21, 10);
		auto mixed = Pair(gs, 0.05, 21, 10, KC, "cm");
		Check(metres.ok && mixed.ok && metres.group->Run(error) && mixed.group->Run(error), "runs", error);
		const double da = std::fabs(LastDisplacement(*metres.group, "a") - LastDisplacement(*mixed.group, "a"));
		const double db = std::fabs(LastDisplacement(*metres.group, "b") - LastDisplacement(*mixed.group, "b", 100));
		Check(da < 1e-6 && db < 1e-6, "a cm partner through the automatic unit_scale adapter matches the m run", std::to_string(std::max(da, db)));
	}
	// ---------------------------------------------------------------- a cached member equals a live upstream
	{
		GroupPolicy gs;
		SimulationGroup live;
		Check(live.AddMember("a", Make(OscillatorSolver::Id), Oscillator(MA, KA, KC, UA, 0, 0.05, 21, 10), error) &&
			live.AddMember("b", Make(OscillatorSolver::Id), Oscillator(MB, KB, KC, 0, VB, 0.05, 21, 10), error) &&
			live.Connect({"a", "state", "b", "partner"}, error) && live.Configure(gs, EmptySceneInputs(), error) &&
			live.Start(error) && live.Run(error), "live one-way", error);
		SimulationGroup cached;
		Check(cached.AddCachedMember("a", live.Snapshot("a"), error) &&
			cached.AddMember("b", Make(OscillatorSolver::Id), Oscillator(MB, KB, KC, 0, VB, 0.05, 21, 10), error) &&
			cached.Connect({"a", "state", "b", "partner"}, error) && cached.Configure(gs, EmptySceneInputs(), error) &&
			cached.Start(error) && cached.Run(error), "cached one-way", error);
		Check(Digests(*cached.Snapshot("b")) == Digests(*live.Snapshot("b")), "fed from a cache = fed live (Gauss-Seidel), byte for byte");
		Check(cached.Description("b")->cache.key != live.Description("b")->cache.key, "and keyed by the cache's key, not the live member");
	}
	// ---------------------------------------------------------------- reference solvers: group = CachedPortFeed
	{
		const V upstream = V::MapValue({{"quantity", V::String("dye rate")}, {"units", V::String("1/s")}, {"frames", V::Unsigned(8)}});
		const V downstream = V::MapValue({{"quantity", V::String("dye")}, {"units", V::String("1")}, {"frames", V::Unsigned(8)}});
		InProcessHost a(Make(AdvectionDiffusionSolver::Id));
		Check(a.Configure(upstream, EmptySceneInputs(), error) && a.Start(error) && a.StepToIndex(100, error), "upstream bake", error);
		CachedPortFeed feed(a.Snapshot(), CodecBudget);
		feed.Bind("source", "field");
		InProcessHost b(Make(AdvectionDiffusionSolver::Id));
		b.SetPorts(&feed);
		Check(b.Configure(downstream, EmptySceneInputs(), error) && b.Start(error) && b.StepToIndex(100, error), "fed bake", error);
		SimulationGroup g;
		GroupPolicy jac; jac.order = ExchangeOrder::Jacobi;
		Check(g.AddCachedMember("up", a.Snapshot(), error) &&
			g.AddMember("down", Make(AdvectionDiffusionSolver::Id), downstream, error) &&
			g.Connect({"up", "field", "down", "source"}, error) && g.Configure(jac, EmptySceneInputs(), error) &&
			g.Start(error) && g.Run(error), "reference solvers in a group", error);
		Check(Digests(*g.Snapshot("down")) == Digests(*b.Snapshot()), "a Jacobi group over a cache equals CachedPortFeed byte for byte");
		Check(g.Description("down")->cache.key != b.Description().cache.key, "the group keys the port feed (the M4 gap)");
		// Refusals.
		SimulationGroup bad;
		Check(bad.AddMember("m", Make(MembraneSolver::Id), V::MapValue({}), error) &&
			bad.AddMember("d", Make(AdvectionDiffusionSolver::Id), downstream, error) &&
			bad.Connect({"m", "surface", "d", "source"}, error) && !bad.Configure(GroupPolicy(), EmptySceneInputs(), error),
			"incompatible ports without an adapter are refused", error);
		SimulationGroup twice;
		Check(twice.AddMember("x", Make(OscillatorSolver::Id), V::MapValue({}), error) && !twice.AddMember("x", Make(OscillatorSolver::Id), V::MapValue({}), error),
			"a duplicate member is refused");
	}
	if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
	std::cout << "SimulationGroupTest: all checks passed\n";
	return 0;
}
