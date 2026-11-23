// Solver contract and in-process host: conformance of both reference solvers,
// stop/checkpoint/restore equal to an uninterrupted run, sticky stop requests,
// rollback equal to a re-run, immutable snapshots while the worker thread
// steps, snapshot sharing (a long run's publish cost stays flat), restore
// discarding later frames, static payloads in memory, external macro steps
// (accept at the start, offer at the end), one-way port feed from a cached
// output equal to the live feed, port compatibility, side outputs in
// process, and evidence-driven fidelity with conditions bound into digests.
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulatedMeshSource.h"
#include "../src/Library/Simulation/SimulationTimeline.h"
#include "SimulationReferenceSolvers.h"
#include "TestTempDir.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <map>

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

V Map(V::Members m) { return V::MapValue(m); }

std::unique_ptr<ISimulationSolver> Make(const char* id)
{
	std::string error;
	auto s = CreateSolver(id, error);
	Check(s != nullptr, "registry creates solver", error);
	return s;
}

// (index, output) -> payload digest, from any snapshot.
std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const IReadOnlyCacheSnapshot& s)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> out;
	for (const auto& e : s.Entries()) out[{e.index, e.output}] = e.digest;
	return out;
}

std::map<std::pair<std::int64_t,std::string>,std::string> RunToEnd(const char* id, const V& params,
	SolverDescription* description = nullptr)
{
	InProcessHost host(Make(id));
	std::string error;
	NoSceneInputs inputs;
	Check(host.Configure(params, inputs, error) && host.Start(error), std::string("configure and start ") + id, error);
	Check(host.StepToIndex((std::numeric_limits<std::int64_t>::max)(), error) && host.Position().finished,
		std::string("caller-thread run finishes ") + id, error);
	if (description) *description = host.Description();
	return Digests(*host.Snapshot());
}

class CountingSink final : public IFrameSink {
public:
	std::vector<std::pair<std::int64_t,std::shared_ptr<const Bytes>>> frames;
	std::map<std::string,std::shared_ptr<const Bytes>> statics;
	bool Publish(std::int64_t index, const std::string&, double, std::shared_ptr<const Bytes> payload,
		const Bytes&, std::string&) override { frames.emplace_back(index, std::move(payload)); return true; }
	bool PublishStatic(const std::string&, std::shared_ptr<const Bytes> payload, std::string& digest,
		std::string&) override {
		digest = RISE::RISECBOR64::SHA256Hex(*payload);
		statics[digest] = std::move(payload);
		return true;
	}
	StaticPayloadResolver Resolver() const {
		return [this](const std::string& d, std::size_t, std::shared_ptr<const Bytes>& b, std::string& e) {
			const auto it = statics.find(d);
			if (it == statics.end()) { e = "unpublished static"; return false; }
			b = it->second;
			return true;
		};
	}
};

class StopAfter final : public IProgress {
public:
	explicit StopAfter(int n) : left_(n) {}
	bool Report(double, double, const std::string&) override { return left_-- > 0; }
private:
	int left_;
};

// The contract, exercised directly: description, registry, effective
// parameters, evidence, frames, and a stop in the middle of a segment.
void Conformance(const char* id, const V& params)
{
	const std::string name(id);
	std::string error;
	NoSceneInputs inputs;
	SolverDescription d;
	auto solver = Make(id);
	if (!solver) return;
	Check(solver->Configure(params, inputs, d, error), name + " configures", error);
	Check(d.solverId == name && !d.version.empty() && !d.cache.outputs.empty() &&
		IsNativePayloadType(d.cache.outputs[0].type), name + " describes native outputs");
	int offered = 0, accepted = 0;
	for (const auto& p : d.ports) (p.offers ? offered : accepted)++;
	Check(offered == 1 && accepted == 1, name + " declares one offered and one accepted port");
	Check(d.capabilities.deterministic && d.capabilities.resumable && d.capabilities.externalMacroStep &&
		d.capabilities.interactive && d.capabilities.backends.size() == 1 &&
		d.capabilities.backends[0].status == BackendStatus::Supported, name + " capabilities");
	Check(d.evidence.cases.size() == 1 && d.evidence.cases[0].passed &&
		d.evidence.cases[0].error <= d.evidence.cases[0].tolerance && d.evidence.cases[0].error > 0 &&
		EffectiveFidelity(d.evidence) == "predictive", name + " computes passing evidence",
		std::to_string(d.evidence.cases[0].error));
	std::cout << name << ": evidence " << d.evidence.cases[0].id << " error " << d.evidence.cases[0].error
		<< " tolerance " << d.evidence.cases[0].tolerance << "\n";

	// Effective parameters reproduce themselves and the key; pace does not enter it.
	SolverDescription again, paced;
	auto other = Make(id);
	V::Members withPace = params.GetMap();
	withPace.emplace_back("pace_ms", V::Unsigned(1));
	Check(PrepareCacheDescription(d, inputs, "", error) && other->Configure(d.effectiveParameters, inputs, again, error) &&
		PrepareCacheDescription(again, inputs, "", error) && again.cache.key == d.cache.key &&
		other->Configure(V::MapValue(withPace), inputs, paced, error) && PrepareCacheDescription(paced, inputs, "", error) &&
		paced.cache.key == d.cache.key, name + " effective parameters are a fixed point; pace is not keyed", error);
	Check(!other->Configure(Map({{"no_such_parameter", V::Unsigned(1)}}), inputs, again, error),
		name + " refuses unknown parameters");

	// Uninterrupted run.
	CountingSink whole;
	AdvanceResult r;
	Check(solver->Start(error), name + " starts", error);
	while (!solver->Position().finished)
		if (!solver->Advance(AdvanceRequest{}, whole, nullptr, nullptr, r, error)) { Check(false, name + " advances", error); return; }
	Check(whole.frames.size() == d.cache.count && r.status == AdvanceResult::Status::Finished,
		name + " publishes one frame per output index");
	AdvanceResult after;
	Check(solver->Advance(AdvanceRequest{}, whole, nullptr, nullptr, after, error) &&
		after.status == AdvanceResult::Status::Finished && after.framesPublished == 0, name + " stays finished");

	// Stop inside a segment, checkpoint, restore in a new instance, continue.
	auto first = Make(id), second = Make(id);
	SolverDescription d1, d2;
	CountingSink head, tail;
	Check(first->Configure(params, inputs, d1, error) && first->Start(error), name + " second run starts", error);
	Check(first->Advance(AdvanceRequest{}, head, nullptr, nullptr, r, error) &&
		first->Advance(AdvanceRequest{}, head, nullptr, nullptr, r, error), name + " two outputs", error);
	StopAfter stopper(3);
	Check(first->Advance(AdvanceRequest{}, head, nullptr, &stopper, r, error) &&
		r.status == AdvanceResult::Status::Stopped && head.frames.size() == 2, name + " stops mid-segment via progress");
	first->RequestStop();
	Check(first->Advance(AdvanceRequest{}, head, nullptr, nullptr, r, error) && r.status == AdvanceResult::Status::Stopped &&
		head.frames.size() == 2, name + " a stop request made between calls is honoured by the next Advance (sticky)");
	Check(first->Advance(AdvanceRequest{}, head, nullptr, nullptr, r, error) && r.status == AdvanceResult::Status::Advanced &&
		head.frames.size() == 3, name + " the consumed stop request is cleared");
	StopAfter stopper2(5);
	Check(first->Advance(AdvanceRequest{}, head, nullptr, &stopper2, r, error) &&
		r.status == AdvanceResult::Status::Stopped, name + " stops again mid-segment");
	Bytes state;
	Check(first->Checkpoint(state, error) && second->Configure(params, inputs, d2, error) &&
		second->Restore(state, error) && second->Position().time == first->Position().time &&
		second->Position().nextIndex == 3, name + " restores a mid-segment checkpoint", error);
	while (!second->Position().finished)
		if (!second->Advance(AdvanceRequest{}, tail, nullptr, nullptr, r, error)) { Check(false, name + " resumed advance", error); return; }
	bool same = head.frames.size() + tail.frames.size() == whole.frames.size();
	for (std::size_t i = 0; same && i < head.frames.size(); ++i) same = *head.frames[i].second == *whole.frames[i].second;
	for (std::size_t i = 0; same && i < tail.frames.size(); ++i)
		same = *tail.frames[i].second == *whole.frames[head.frames.size()+i].second;
	Check(same, name + " stop and restore reproduces the uninterrupted frames byte for byte");
	Check(!second->Restore(Bytes{1, 2, 3}, error), name + " refuses a malformed checkpoint");
}

void GridAccuracy()
{
	// Every published grid frame stays within the evidence tolerance of the exact solution.
	auto solver = std::make_unique<AdvectionDiffusionSolver>();
	auto* raw = solver.get();
	std::string error;
	NoSceneInputs inputs;
	SolverDescription d;
	CountingSink sink;
	AdvanceResult r;
	Check(raw->Configure(Map({}), inputs, d, error) && raw->Start(error), "grid solver starts", error);
	double worst = 0;
	while (!raw->Position().finished) {
		if (!raw->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error)) break;
		GridFrame g;
		Check(DecodeGrid(*sink.frames.back().second, budget, g, error), "grid frame decodes", error);
		const double t = d.cache.startTime + static_cast<double>(sink.frames.back().first)*d.cache.interval, h = raw->Cell();
		const auto& c = g.channels[0];
		for (std::uint32_t j = 0; j < c.dimensions[1]; ++j) for (std::uint32_t i = 0; i < c.dimensions[0]; ++i)
			worst = std::max(worst, std::fabs(FromF32(c.blocks[0].values[i + c.dimensions[0]*j]) -
				raw->Exact((i + .5)*h, (j + .5)*h, t)));
	}
	Check(worst <= d.evidence.cases[0].tolerance, "grid frames track the exact solution", std::to_string(worst));
}

void MeshFrames()
{
	auto solver = Make(MembraneSolver::Id);
	std::string error;
	NoSceneInputs inputs;
	SolverDescription d;
	CountingSink sink;
	AdvanceResult r;
	Check(solver->Configure(Map({}), inputs, d, error) && solver->Start(error), "membrane starts", error);
	while (!solver->Position().finished && solver->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error)) {}
	std::string digest;
	bool fixed = true, moving = false;
	for (const auto& f : sink.frames) {
		MeshFrame m;
		MeshTopology t;
		if (!DecodeMesh(*f.second, budget, sink.Resolver(), m, error) || !PeekMeshTopology(*f.second, t, error)) { fixed = false; break; }
		if (digest.empty()) digest = t.digest;
		fixed = fixed && t.digest == digest && !t.reference.empty() && !m.velocities.empty() && !m.normals.empty() &&
			m.uvSets.size() == 1 && m.uvSets[0].name == "material" && m.attributes.size() == 1;
		for (std::size_t q = 2; q < m.velocities.size(); q += 3) moving = moving || FromF32(m.velocities[q]) != 0;
	}
	Check(fixed && moving && sink.frames.size() == d.cache.count && sink.statics.size() == 1,
		"membrane publishes a mesh.v2 stream over one static topology, with velocities, UVs and an attribute", error);
}

void MacroSteps()
{
	// Half-interval macro steps plus an off-grid stop: outputs land at their
	// own times and agree with output-to-output stepping to roundoff.
	std::string error;
	NoSceneInputs inputs;
	InProcessHost host(Make(MembraneSolver::Id)), reference(Make(MembraneSolver::Id));
	Check(host.Configure(Map({}), inputs, error) && host.Start(error) &&
		reference.Configure(Map({}), inputs, error) && reference.Start(error), "macro hosts start", error);
	const auto& c = host.Description().cache;
	AdvanceRequest odd{AdvanceRequest::Mode::MacroStep, c.interval*0.37};
	AdvanceResult r;
	Check(host.Step(odd, r, error) && r.position.time == odd.macroEnd && r.position.nextIndex == 1 &&
		r.framesPublished == 1, "macro step to an off-grid time publishes only index 0", error);
	Check(!host.Step(AdvanceRequest{AdvanceRequest::Mode::MacroStep, 0}, r, error), "macro end in the past refused");
	for (std::uint64_t j = 1; j <= 2*(c.count-1); ++j) {
		AdvanceRequest half{AdvanceRequest::Mode::MacroStep, c.startTime + static_cast<double>(j)*(c.interval/2)};
		if (!host.Step(half, r, error) || r.position.time != half.macroEnd) { Check(false, "half-interval macro step", error); break; }
	}
	Check(host.Position().finished && host.Snapshot()->Complete(), "macro stepping publishes every output");
	Check(reference.StepToIndex(1000, error), "reference run", error);
	const auto a = host.Snapshot(), b = reference.Snapshot();
	double worst = 0;
	for (std::size_t i = 0; i < a->Entries().size(); ++i) {
		std::shared_ptr<const Bytes> x, y;
		MeshFrame mx, my;
		if (a->Entries()[i].time != b->Entries()[i].time ||
			a->Read(a->Entries()[i].index, "surface", budget, x, error) != FrameStatus::Ready ||
			b->Read(b->Entries()[i].index, "surface", budget, y, error) != FrameStatus::Ready ||
			!DecodeMesh(*x, budget, a->Resolver(), mx, error) || !DecodeMesh(*y, budget, b->Resolver(), my, error)) { worst = 1; break; }
		for (std::size_t q = 2; q < mx.positions.size(); q += 3)
			worst = std::max(worst, std::fabs(FromF32(mx.positions[q]) - FromF32(my.positions[q])));
	}
	std::cout << "macro-stepped vs output-stepped max displacement difference " << worst << " m\n";
	Check(worst < 1e-5, "macro-stepped frames match output-stepped frames to discretization accuracy", std::to_string(worst));
}

void WorkerThreadAndSnapshots()
{
	const V params = Map({{"pace_ms", V::Unsigned(3)}});
	const auto reference = RunToEnd(MembraneSolver::Id, Map({}));
	std::string error;
	NoSceneInputs inputs;
	InProcessHost host(Make(MembraneSolver::Id));
	Check(host.Configure(params, inputs, error) && host.Start(error), "threaded host starts", error);
	Check(host.Play(error, 4) && host.WaitIdle(error) && host.Position().nextIndex == 5 && !host.Busy(),
		"play to an index pauses after publishing it", error);
	AdvanceResult r;
	Check(host.Play(error) && !host.Step(AdvanceRequest{}, r, error), "caller stepping refused while playing");
	// Hold snapshots taken while the worker publishes; none may change later.
	struct Held { std::shared_ptr<const IReadOnlyCacheSnapshot> s; std::size_t n; std::string digest; std::vector<Bytes> payloads; };
	std::vector<Held> held;
	auto collect = [&](std::size_t limit) {
		while (host.Busy() && held.size() < limit) {
			Held h{host.Snapshot(), 0, {}, {}};
			h.n = h.s->Entries().size();
			h.digest = h.s->Digest();
			for (const auto& e : h.s->Entries()) {
				std::shared_ptr<const Bytes> b;
				if (h.s->Read(e.index, e.output, budget, b, error) == FrameStatus::Ready) h.payloads.push_back(*b);
			}
			held.push_back(std::move(h));
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	};
	collect(6);
	host.Pause();
	Check(host.WaitIdle(error), "pause while playing", error);
	const auto pausedAt = host.Position();
	Check(!host.Busy() && !pausedAt.finished && pausedAt.nextIndex > 5, "pause stops the worker before the end",
		std::to_string(pausedAt.nextIndex));
	Check(host.Play(error), "play again", error);
	collect(400);
	Check(host.WaitIdle(error) && host.Position().finished, "play resumes to the end", error);
	bool consistent = held.size() > 2, partial = false;
	for (const auto& h : held) {
		consistent = consistent && h.s->Entries().size() == h.n && h.s->Digest() == h.digest && h.payloads.size() == h.n;
		partial = partial || (h.n > 0 && h.n < 24 && !h.s->Complete());
		for (std::size_t i = 0; consistent && i < h.n; ++i) {
			std::shared_ptr<const Bytes> b;
			consistent = h.s->Read(h.s->Entries()[i].index, "surface", budget, b, error) == FrameStatus::Ready &&
				*b == h.payloads[i] && RISE::RISECBOR64::SHA256Hex(*b) == h.s->Entries()[i].digest;
		}
	}
	Check(consistent && partial, "readers keep consistent partial snapshots while the worker steps",
		std::to_string(held.size()));
	std::shared_ptr<const Bytes> none;
	Check(held.front().s->Read(23, "surface", budget, none, error) == FrameStatus::Unavailable,
		"an early snapshot reports later frames unavailable");
	Check(Digests(*host.Snapshot()) == reference && host.Snapshot()->Complete(),
		"threaded run with pause equals the uninterrupted run");
}

void InMemoryCheckpoint()
{
	std::string error;
	NoSceneInputs inputs;
	const V params = Map({{"frames", V::Unsigned(12)}});
	const auto reference = RunToEnd(AdvectionDiffusionSolver::Id, params);
	InProcessHost a(Make(AdvectionDiffusionSolver::Id)), b(Make(AdvectionDiffusionSolver::Id));
	InProcessHost::Checkpoint checkpoint;
	Check(a.Configure(params, inputs, error) && a.Start(error) && a.StepToIndex(5, error) &&
		a.SaveCheckpoint(checkpoint, error) && checkpoint.position.nextIndex == 6, "checkpoint in memory", error);
	Check(a.StepToIndex(100, error) && Digests(*a.Snapshot()) == reference, "run continues after checkpoint", error);
	const auto finalDigest = a.Snapshot()->Digest();
	Check(a.RestoreCheckpoint(checkpoint, error) && a.Position().nextIndex == 6 && !a.Position().finished &&
		a.StepToIndex(100, error) && Digests(*a.Snapshot()) == reference && a.Snapshot()->Digest() != finalDigest,
		"scrub back to a checkpoint and replay republishes identical frames", error);
	Check(b.Configure(params, inputs, error) && b.RestoreCheckpoint(checkpoint, error) && b.StepToIndex(100, error),
		"restore into a fresh host", error);
	auto tail = Digests(*b.Snapshot());
	bool same = tail.size() == 6;
	for (const auto& t : tail) same = same && t.first.first >= 6 && reference.at(t.first) == t.second;
	Check(same, "fresh host restored from checkpoint reproduces the tail");
	auto foreign = checkpoint;
	foreign.key = std::string(64, '0');
	Check(!b.RestoreCheckpoint(foreign, error), "checkpoint for another configuration refused");
}

void Recording(const fs::path& root)
{
	std::string error;
	NoSceneInputs inputs;
	InProcessHost host(Make(AdvectionDiffusionSolver::Id));
	Check(host.Configure(Map({}), inputs, error) && host.Record(root/"recorded", budget, error) && host.Start(error) &&
		host.Play(error) && host.WaitIdle(error), "record while playing", error);
	CacheSnapshot disk;
	Check(CacheSnapshot::Open(root/"recorded", budget, disk, error) && disk.Complete() &&
		Digests(disk) == Digests(*host.Snapshot()) && disk.Description().fidelity == "predictive" &&
		disk.Description().key == host.Description().cache.key && disk.Description().evidence.size() == 1,
		"recording seals the same frames, key and evidence", error);
	InProcessHost again(Make(AdvectionDiffusionSolver::Id));
	Check(again.Configure(Map({}), inputs, error) && again.Record(root/"recorded", budget, error) &&
		again.Description().cache.epoch != disk.Description().epoch, "re-recording starts a new epoch", error);
}

void Fidelity(const fs::path& root)
{
	std::string error;
	NoSceneInputs inputs;
	InProcessHost strict(Make(MembraneSolver::Id)), modest(Make(AdvectionDiffusionSolver::Id));
	Check(strict.Configure(Map({{"tolerance", V::Float(1e-9)}}), inputs, error), "strict configure", error);
	const auto& d = strict.Description();
	Check(!d.evidence.cases[0].passed && d.evidence.fidelity == "predictive" && d.cache.fidelity == "preview",
		"a failing analytic check publishes preview despite requesting predictive");
	Check(strict.Record(root/"failed-evidence", budget, error) && strict.Start(error) && strict.StepToIndex(100, error),
		"bake with failed evidence", error);
	CacheSnapshot disk;
	Check(CacheSnapshot::Open(root/"failed-evidence", budget, disk, error) && disk.Description().fidelity == "preview",
		"cache manifest carries the preview label", error);
	Check(modest.Configure(Map({{"fidelity", V::String("preview")}}), inputs, error) &&
		modest.Description().evidence.cases[0].passed && modest.Description().cache.fidelity == "preview",
		"a passing solver that requests preview stays preview", error);

	Evidence forged;
	forged.fidelity = "predictive";
	forged.scope = "a test scope";
	EvidenceCase c{"x", "", std::string(64, 'a'), 0.5, 0.1, true};
	forged.cases = {c};
	Check(EffectiveFidelity(forged) == "preview", "a case asserting pass with error above tolerance is preview");
	forged.cases[0].error = 0.05;
	Check(EffectiveFidelity(forged) == "predictive", "consistent passing evidence is predictive");
	forged.scope.clear();
	Check(EffectiveFidelity(forged) == "preview", "predictive needs a stated scope");
	forged.scope = "a test scope";
	forged.conditions = {{"mode", {"bake"}}, {"contact", {"none"}}};
	Check(EffectiveFidelity(forged) == "preview", "unsorted evidence conditions are invalid and preview");
	forged.conditions = {{"contact", {"none"}}, {"mode", {"bake"}}};
	Check(EffectiveFidelity(forged) == "predictive", "valid conditions keep predictive");
	forged.cases[0].digest = "nope";
	Check(EffectiveFidelity(forged) == "preview", "evidence without a digest is preview");
	forged.cases.clear();
	Check(EffectiveFidelity(forged) == "preview", "no evidence is preview");
}

void PortFeeds(const fs::path& root)
{
	std::string error;
	NoSceneInputs inputs;
	const V grid = Map({{"quantity", V::String("pressure")}, {"units", V::String("Pa")}, {"amplitude", V::Float(0.5)},
		{"cells", V::Unsigned(16)}, {"frames", V::Unsigned(24)}, {"interval", V::Float(0.01)}});
	InProcessHost liveGrid(Make(AdvectionDiffusionSolver::Id)), liveMembrane(Make(MembraneSolver::Id));
	Check(liveGrid.Configure(grid, inputs, error) && liveMembrane.Configure(Map({}), inputs, error),
		"coupled solvers configure", error);
	const PortDeclaration* offer = nullptr; const PortDeclaration* accept = nullptr;
	for (const auto& p : liveGrid.Description().ports) if (p.offers) offer = &p;
	for (const auto& p : liveMembrane.Description().ports) if (!p.offers) accept = &p;
	Check(offer && accept && PortsCompatible(*offer, *accept, error), "grid field port feeds the membrane load port", error);
	PortDeclaration wrong = *offer;
	wrong.units = "kPa";
	Check(!PortsCompatible(wrong, *accept, error), "unit mismatch refused");
	wrong = *offer;
	wrong.channels[0].components = 3;
	Check(!PortsCompatible(wrong, *accept, error), "channel component mismatch refused (an adapter's job)");
	wrong = *offer;
	wrong.channels[0].units = "kPa";
	Check(!PortsCompatible(wrong, *accept, error), "channel unit mismatch refused");
	wrong = *offer;
	PortDeclaration domained = *accept;
	wrong.domain = std::string(64, 'a');
	domained.domain = std::string(64, 'b');
	Check(!PortsCompatible(wrong, domained, error), "different index-space domains refused");
	domained.domain.clear();
	Check(PortsCompatible(wrong, domained, error), "an unstated domain is decided per payload", error);
	Check(FindPortAdapter(wrong, *accept) == nullptr, "no port adapter is registered for an unknown pair");

	// Live, one way: the grid steps first, the membrane follows with macro ends at the grid's output times.
	PortRelay relay;
	relay.Connect("load", "field");
	liveGrid.SetPorts(&relay);
	liveMembrane.SetPorts(&relay);
	Check(liveGrid.Start(error) && liveMembrane.Start(error), "coupled start", error);
	for (std::int64_t k = 0; k < 24; ++k) {
		AdvanceResult r;
		if (!liveGrid.StepToIndex(k, error) ||
			!liveMembrane.Step({AdvanceRequest::Mode::MacroStep, liveGrid.Snapshot()->Entries().back().time}, r, error)) {
			Check(false, "live coupled step", error); break;
		}
	}
	// Cached: bake the grid, then feed the membrane from the cache.
	InProcessHost bake(Make(AdvectionDiffusionSolver::Id)), cachedMembrane(Make(MembraneSolver::Id)),
		uncoupled(Make(MembraneSolver::Id));
	Check(bake.Configure(grid, inputs, error) && bake.Record(root/"pressure", budget, error) && bake.Start(error) &&
		bake.StepToIndex(100, error), "bake the pressure field", error);
	auto disk = std::make_shared<CacheSnapshot>();
	Check(CacheSnapshot::Open(root/"pressure", budget, *disk, error) && disk->Complete(), "open baked field", error);
	CachedPortFeed feed(disk, budget);
	feed.Bind("load", offer->output);
	cachedMembrane.SetPorts(&feed);
	Check(cachedMembrane.Configure(Map({}), inputs, error) && cachedMembrane.Start(error), "cached-feed membrane", error);
	for (const auto& e : disk->Entries()) {
		AdvanceResult r;
		if (!cachedMembrane.Step({AdvanceRequest::Mode::MacroStep, e.time}, r, error)) { Check(false, "cached coupled step", error); break; }
	}
	Check(uncoupled.Configure(Map({}), inputs, error) && uncoupled.Start(error) && uncoupled.StepToIndex(100, error),
		"uncoupled membrane", error);
	const auto live = Digests(*liveMembrane.Snapshot()), cached = Digests(*cachedMembrane.Snapshot());
	Check(live.size() == 24 && live == cached, "one-way feed from a cached output equals the live feed byte for byte");
	Check(live != Digests(*uncoupled.Snapshot()), "the load actually changes the membrane");
	Check(Digests(*disk) == Digests(*liveGrid.Snapshot()), "offering solver's live frames equal its cached frames");
}


// An exchange that feeds one payload to every accepted port and records the
// times of accepts and offers.
class RecordingFeed final : public IPortExchange {
public:
	std::shared_ptr<const Bytes> payload;
	std::vector<double> accepts;
	std::vector<std::pair<std::string,double>> offers;
	std::map<std::string,std::shared_ptr<const Bytes>> statics;
	bool Offer(const std::string& port, double time, std::shared_ptr<const Bytes>, std::string&) override {
		offers.emplace_back(port, time);
		return true;
	}
	PortStatus Accept(const std::string&, double time, std::shared_ptr<const Bytes>& encoded, std::string&) override {
		accepts.push_back(time);
		if (!payload) return PortStatus::Unavailable;
		encoded = payload;
		return PortStatus::Ready;
	}
	bool OfferStatic(const std::string&, std::shared_ptr<const Bytes> bytes, std::string&) override {
		statics[RISE::RISECBOR64::SHA256Hex(*bytes)] = std::move(bytes);
		return true;
	}
};

// A constant scalar grid of n x n cells (cell size 1/(n-1)... any lattice the
// reference solvers accept): a pressure or source value.
std::shared_ptr<const Bytes> ConstantGrid(std::uint32_t n, float value, double cell)
{
	GridFrame g;
	GridChannel c;
	c.name = "pressure"; c.units = "Pa";
	c.dimensions = {n, n, 1};
	c.indexToWorld = {cell,0,0,0, 0,cell,0,0, 0,0,cell,0, 0,0,0,1};
	GridBlock b;
	b.dimensions = c.dimensions;
	b.active.assign((n*n+7)/8, 0xff);
	if ((n*n)%8) b.active.back() = static_cast<std::uint8_t>((1u << ((n*n)%8)) - 1);
	b.values.assign(n*n, F32(value));
	c.blocks.push_back(b);
	g.channels.push_back(c);
	auto bytes = std::make_shared<Bytes>();
	std::string error;
	EncodeGrid(g, budget, *bytes, error);
	return bytes;
}

// Accept at the start of each macro step (held over it), offer at its end.
void PortTiming()
{
	std::string error;
	auto solver = Make(MembraneSolver::Id);
	SolverDescription d;
	Check(solver->Configure(Map({}), EmptySceneInputs(), d, error) && solver->Start(error), "timing membrane", error);
	RecordingFeed feed;
	feed.payload = ConstantGrid(17, 0.5f, 1.0/16);
	CountingSink sink;
	AdvanceResult r;
	const double dt = d.cache.interval;
	// Macro steps of 1.5 output intervals: each spans an output time.
	std::vector<double> ends;
	for (int k = 1; k <= 4; ++k) ends.push_back(1.5*dt*k);
	double begin = 0;
	bool ok = true;
	for (double e : ends) {
		const auto before = feed.accepts.size();
		ok = ok && solver->Advance({AdvanceRequest::Mode::MacroStep, e}, sink, &feed, nullptr, r, error);
		ok = ok && feed.accepts.size() == before + 1 && feed.accepts.back() == begin;
		ok = ok && !feed.offers.empty() && feed.offers.back().second == e;
		begin = e;
	}
	Check(ok, "one accept per macro step at its start; the last offer of each step is stamped with its end", error);
	std::size_t outputOffers = 0;
	for (const auto& o : feed.offers) {
		const double k = o.second/dt;
		if (std::fabs(k - std::round(k)) < 1e-9) ++outputOffers;
	}
	Check(outputOffers == sink.frames.size() && feed.statics.size() == 1,
		"offers also go out at every output time, with the static topology offered first");
	// Free-running: every segment is a step and reads its own value.
	auto free = Make(MembraneSolver::Id);
	RecordingFeed freeFeed;
	CountingSink freeSink;
	Check(free->Configure(Map({}), EmptySceneInputs(), d, error) && free->Start(error) &&
		free->Advance(AdvanceRequest{}, freeSink, &freeFeed, nullptr, r, error) &&
		free->Advance(AdvanceRequest{}, freeSink, &freeFeed, nullptr, r, error) &&
		freeFeed.accepts == std::vector<double>{0.0}, "free-running reads once per output interval", error);
}

// Rollback: a repeated macro step equals the first attempt, and with new port
// values equals a re-run of that step from a checkpoint.
void Rollback(const char* id, const V& params, std::shared_ptr<const Bytes> a, std::shared_ptr<const Bytes> b)
{
	const std::string name(id);
	std::string error;
	auto solver = Make(id), rerun = Make(id);
	SolverDescription d, d2;
	CountingSink sink;
	AdvanceResult r;
	RecordingFeed feed;
	feed.payload = a;
	Check(solver->Configure(params, EmptySceneInputs(), d, error) && d.capabilities.rollback && solver->Start(error),
		name + " rollback solver", error);
	const double dt = d.cache.interval;
	bool ok = solver->Advance({AdvanceRequest::Mode::MacroStep, 1.5*dt}, sink, &feed, nullptr, r, error);
	std::shared_ptr<const SolverStateToken> token;
	Bytes checkpoint;
	ok = ok && solver->SaveState(token, error) && solver->Checkpoint(checkpoint, error);
	const auto at = solver->Position();
	CountingSink first, second, third, reference;
	ok = ok && solver->Advance({AdvanceRequest::Mode::MacroStep, 3.5*dt}, first, &feed, nullptr, r, error);
	const auto after = solver->Position();
	ok = ok && solver->RestoreState(*token, error) && solver->Position().time == at.time &&
		solver->Position().nextIndex == at.nextIndex;
	ok = ok && solver->Advance({AdvanceRequest::Mode::MacroStep, 3.5*dt}, second, &feed, nullptr, r, error);
	Check(ok && solver->Position().time == after.time && first.frames.size() == 2 && second.frames.size() == 2 &&
		*first.frames[0].second == *second.frames[0].second && *first.frames[1].second == *second.frames[1].second,
		name + " a rolled-back macro step repeats byte for byte", error);
	// New port values for the repeated step: equal to a re-run from the checkpoint with those values.
	feed.payload = b;
	RecordingFeed other;
	other.payload = b;
	ok = solver->RestoreState(*token, error) &&
		solver->Advance({AdvanceRequest::Mode::MacroStep, 3.5*dt}, third, &feed, nullptr, r, error) &&
		rerun->Configure(params, EmptySceneInputs(), d2, error) && rerun->Restore(checkpoint, error) &&
		rerun->Advance({AdvanceRequest::Mode::MacroStep, 3.5*dt}, reference, &other, nullptr, r, error);
	bool same = ok && third.frames.size() == reference.frames.size() && !third.frames.empty();
	for (std::size_t i = 0; same && i < third.frames.size(); ++i) same = *third.frames[i].second == *reference.frames[i].second;
	Check(same && *third.frames[0].second != *first.frames[0].second,
		name + " a repeated step re-reads its ports and equals a re-run with the new values", error);
	auto stranger = Make(id);
	SolverDescription d3;
	Check(stranger->Configure(params, EmptySceneInputs(), d3, error) && !stranger->RestoreState(*token, error),
		name + " a token from another instance is refused");
}

// Pause/Play cycles: a stop that misses the step in progress is consumed
// by the host, never by a later caller step.
void PauseCycles()
{
	std::string error;
	const auto reference = RunToEnd(MembraneSolver::Id, Map({}));
	InProcessHost host(Make(MembraneSolver::Id));
	Check(host.Configure(Map({{"pace_ms", V::Unsigned(1)}}), EmptySceneInputs(), error) && host.Start(error), "cycling host", error);
	bool ok = true;
	for (int k = 0; k < 12 && ok && !host.Position().finished; ++k) {
		ok = host.Play(error);
		std::this_thread::sleep_for(std::chrono::microseconds(300 + 200*k));
		host.Pause();
		ok = ok && host.WaitIdle(error);
		AdvanceResult r;
		if (ok && !host.Position().finished) ok = host.Step(AdvanceRequest{}, r, error) && r.status != AdvanceResult::Status::Stopped;
	}
	Check(ok && host.StepToIndex(1000, error) && Digests(*host.Snapshot()) == reference,
		"pause/play/step cycles never leak a stop into a later step and keep the frames", error);
}

// Restoring a checkpoint discards the frames published after it.
void RestoreDiscards()
{
	std::string error;
	const V params = Map({{"frames", V::Unsigned(12)}});
	const auto reference = RunToEnd(MembraneSolver::Id, params);
	InProcessHost host(Make(MembraneSolver::Id));
	InProcessHost::Checkpoint checkpoint;
	Check(host.Configure(params, EmptySceneInputs(), error) && host.Start(error) && host.StepToIndex(4, error) &&
		host.SaveCheckpoint(checkpoint, error) && host.StepToIndex(9, error), "run past a checkpoint", error);
	const auto before = host.Snapshot();
	Check(host.RestoreCheckpoint(checkpoint, error), "restore", error);
	const auto after = host.Snapshot();
	std::shared_ptr<const Bytes> b;
	Check(before->Entries().size() == 10 && after->Entries().size() == 5 &&
		after->Read(7, "surface", budget, b, error) == FrameStatus::Unavailable &&
		after->Read(4, "surface", budget, b, error) == FrameStatus::Ready && after->Statics().size() == 1,
		"frames after the checkpoint leave later snapshots; earlier ones and the statics stay");
	Check(before->Entries().size() == 10, "a snapshot taken before the restore is unchanged");
	Check(host.StepToIndex(100, error) && Digests(*host.Snapshot()) == reference, "the replay republishes the same frames", error);
}

// The live viewport's per-frame reads (published count, latest contiguous frame, timeline range) follow a
// 20,000-frame run through the snapshot's counting accessors: their cost does not grow with the run.
void LiveReaderCost()
{
	std::string error;
	const std::int64_t n = 20000, window = 2000;
	const V params = Map({{"frames", V::Unsigned(n)}, {"vertices", V::Unsigned(3)}, {"substeps", V::Unsigned(1)}});
	InProcessHost host(Make(MembraneSolver::Id));
	Check(host.Configure(params, EmptySceneInputs(), error) && host.Start(error) && host.StepToIndex(0, error),
		"configure a long live run and publish its first frame", error);
	std::unique_ptr<MeshCacheSource> source;
	MeshSourceOptions options;
	Check(MeshCacheSource::OpenLive([&host]() { return host.Snapshot(); }, options, source, error),
		"bind the viewport reader to the live publication", error);
	if (!source) return;
	double early = 0, late = 0;
	bool exact = true;
	for (std::int64_t i = 1; i < n && exact; ++i) {
		exact = host.StepToIndex(i, error);
		const auto t0 = std::chrono::steady_clock::now();
		std::int64_t latest = -2;
		TimelineRange range;
		const auto snapshot = host.Snapshot();
		const OutputSpan span = snapshot->Span("surface");
		exact = exact && LatestContiguousMeshFrame(*source, latest, error) &&
			DescribeMeshSource(*source, true, range, error);
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (i < window) early += s;
		if (i >= n - window) late += s;
		exact = exact && latest == i && range.framesAvailable == static_cast<std::uint64_t>(i + 1) &&
			span.count == static_cast<std::uint64_t>(i + 1) && span.first == 0 && span.last == i &&
			snapshot->EntryCount() == static_cast<std::size_t>(i + 1) &&
			range.availableEnd == MeshFrameSceneTime(*source, i);
	}
	std::cout << "live reader cost: first " << window << " frames " << early*1e6/window << " us/frame, last " << window
		<< " frames " << late*1e6/window << " us/frame at " << n << " published frames\n";
	Check(exact, "every reader answer follows the live run exactly", error);
	Check(late < 4*early + 2e-6*window, "a long live run's per-frame reader cost stays flat");
	// A gap in the run (a frame republished out of order) falls back to the walk and still answers exactly.
	MemoryPublisher gaps;
	CacheDescription d;
	d.key = std::string(64, '3'); d.epoch = std::string(64, '4'); d.producer = "gaps"; d.producerVersion = "1";
	d.count = 10;
	d.outputs = {{"field", "grid.v1", {{"x", "1", "f32"}}}, {"other", "grid.v1", {{"x", "1", "f32"}}}};
	gaps.Reset(d);
	auto payload = std::make_shared<const Bytes>(Bytes{1});
	Check(gaps.Publish(0, "field", 0, payload, {}, error) && gaps.Publish(1, "field", 1, payload, {}, error) &&
		gaps.Publish(3, "field", 3, payload, {}, error) && gaps.Publish(2, "other", 2, payload, {}, error), "publish with a gap", error);
	const auto g = gaps.Current();
	const OutputSpan field = g->Span("field"), other = g->Span("other"), none = g->Span("absent");
	Check(field.count == 3 && field.first == 0 && field.last == 3 && other.count == 1 && other.first == 2 && other.last == 2 &&
		none.count == 0 && g->EntryCount() == 4, "spans count per output and expose the gap");
	gaps.DiscardFrom(3);
	Check(gaps.Current()->Span("field").count == 2 && gaps.Current()->Span("field").last == 1 &&
		gaps.Current()->Span("other").count == 1 && g->Span("field").count == 3, "discarding recomputes the spans of later generations only");
}

// A long interactive run: publishing stays O(log n), snapshots stay immutable.
void SnapshotSharing()
{
	CacheDescription d;
	d.key = std::string(64, '7'); d.epoch = std::string(64, '8'); d.producer = "sharing"; d.producerVersion = "1";
	const std::int64_t n = 60000;
	d.count = n + 10;
	d.outputs = {{"field", "grid.v1", {{"x", "1", "f32"}}}};
	MemoryPublisher publisher;
	publisher.Reset(d);
	auto payload = std::make_shared<const Bytes>(Bytes{1, 2, 3});
	std::string error;
	const std::int64_t window = 3000;
	double early = 0, late = 0;
	std::shared_ptr<const MemoryCacheSnapshot> held;
	bool ok = true;
	for (std::int64_t i = 0; i < n && ok; ++i) {
		const auto t0 = std::chrono::steady_clock::now();
		ok = publisher.Publish(i, "field", static_cast<double>(i), payload, {}, error);
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (i < window) early += s;
		if (i >= n - window) late += s;
		if (i == n/2) held = publisher.Current();
	}
	std::cout << "publish cost: first " << window << " frames " << early*1e6/window << " us/frame, last " << window
		<< " frames " << late*1e6/window << " us/frame at " << n << " entries\n";
	Check(ok && late < 4*early + 2e-6*window, "a long run's publish cost stays flat (shared snapshot storage)", error);
	const auto current = publisher.Current();
	CacheEntry e;
	std::shared_ptr<const Bytes> b;
	Check(held->Count() == static_cast<std::size_t>(n/2 + 1) && current->Count() == static_cast<std::size_t>(n) &&
		held->Read(n/2 + 5, "field", budget, b, error) == FrameStatus::Unavailable &&
		current->FindEntry(n - 1, "field", e) && e.time == static_cast<double>(n - 1) &&
		held->Entries().size() == static_cast<std::size_t>(n/2 + 1), "held snapshots keep their own entries");
	// Replacement and an out-of-order insert leave earlier generations alone.
	auto other = std::make_shared<const Bytes>(Bytes{9});
	Check(publisher.Publish(100, "field", 100, other, {}, error) && publisher.Current()->Read(100, "field", budget, b, error) ==
		FrameStatus::Ready && *b == *other && current->Read(100, "field", budget, b, error) == FrameStatus::Ready &&
		*b == *payload, "a republished frame replaces it in later generations only", error);
	publisher.DiscardFrom(n - 5);
	Check(publisher.Current()->Count() == static_cast<std::size_t>(n - 5) && current->Count() == static_cast<std::size_t>(n),
		"discarding a tail leaves earlier generations alone");
	Check(publisher.Publish(n + 2, "field", 1, payload, {}, error) && publisher.Publish(n - 3, "field", 1, payload, {}, error) &&
		publisher.Current()->FindEntry(n + 2, "field", e) && publisher.Current()->FindEntry(n - 3, "field", e) &&
		publisher.Current()->Entries().size() == static_cast<std::size_t>(n - 3), "out-of-order publication keeps the order", error);
}

// Static payloads in memory: required before a referencing frame.
void StaticsInMemory()
{
	std::string error;
	InProcessHost host(Make(MembraneSolver::Id));
	Check(host.Configure(Map({{"frames", V::Unsigned(3)}}), EmptySceneInputs(), error) && host.Start(error) &&
		host.StepToIndex(10, error), "membrane in memory", error);
	const auto s = host.Snapshot();
	std::shared_ptr<const Bytes> frame;
	MeshFrame m;
	Check(s->Statics().size() == 1 && s->Read(2, "surface", budget, frame, error) == FrameStatus::Ready &&
		DecodeMesh(*frame, budget, s->Resolver(), m, error) && m.uvSets.size() == 1, "in-memory frames resolve their statics", error);
	MemoryPublisher bare;
	bare.Reset(host.Description().cache);
	Check(!bare.Publish(0, "surface", 0, frame, {}, error) && error.find("unpublished static") != std::string::npos,
		"a frame referencing an unpublished static payload is refused");
}

// Evidence conditions and scope: recorded and bound into the evidence digests.
void EvidenceScope(const fs::path& root)
{
	std::string error;
	auto solver = Make(MembraneSolver::Id);
	SolverDescription d;
	Check(solver->Configure(Map({}), EmptySceneInputs(), d, error) && PrepareCacheDescription(d, EmptySceneInputs(), "", error),
		"membrane description", error);
	const auto& c = d.cache;
	Check(c.evidenceConditions == d.evidence.conditions && c.evidenceScope == d.evidence.scope && c.evidence.size() == 1 &&
		c.evidence[0] == ScopedEvidenceDigest(d.evidence.cases[0].digest, c.evidenceConditions, c.evidenceScope) &&
		c.evidence[0] != d.evidence.cases[0].digest && c.fidelity == "predictive",
		"the cache records conditions and scope and binds them into the evidence digest");
	auto other = d.evidence.conditions;
	other[0].second = {"free"};
	Check(ScopedEvidenceDigest(d.evidence.cases[0].digest, other, c.evidenceScope) != c.evidence[0] &&
		ScopedEvidenceDigest(d.evidence.cases[0].digest, c.evidenceConditions, "another scope") != c.evidence[0] &&
		ScopedEvidenceDigest(d.evidence.cases[0].digest, {}, "") == d.evidence.cases[0].digest,
		"changing the conditions or the scope changes the digest");
	InProcessHost host(Make(MembraneSolver::Id));
	CacheSnapshot disk;
	Check(host.Configure(Map({{"frames", V::Unsigned(2)}}), EmptySceneInputs(), error) && host.Record(root/"scoped", budget, error) &&
		host.Start(error) && host.StepToIndex(10, error) && CacheSnapshot::Open(root/"scoped", budget, disk, error) &&
		disk.Description().evidenceConditions == host.Description().evidence.conditions &&
		disk.Description().evidenceScope == host.Description().evidence.scope && disk.Statics().size() == 1,
		"the manifest carries the evidence scope and the static topology", error);
}

// A minimal solver that needs a side-output directory.
class SideOutputSolver final : public ISimulationSolver {
public:
	bool Configure(const V&, const ISceneInputs&, SolverDescription& d, std::string&) override {
		d = SolverDescription{};
		d.solverId = "test.side_output"; d.version = "1";
		d.effectiveParameters = V::MapValue({});
		d.cache.count = 2; d.cache.interval = 1;
		d.cache.outputs = {{"field", "grid.v1", {{"x", "1", "f32"}}}};
		d.sideOutput = "exports";
		return true;
	}
	bool SetSideOutputDirectory(const fs::path& directory, std::string&) override { dir_ = directory; return true; }
	bool Start(std::string& error) override {
		if (dir_.empty()) return Fail(error, "needs a side output directory");
		next_ = 0;
		return true;
	}
	bool Restore(const Bytes&, std::string& error) override { return Fail(error, "no"); }
	bool Advance(const AdvanceRequest&, IFrameSink& sink, IPortExchange*, IProgress*, AdvanceResult& r, std::string& error) override {
		std::ofstream(dir_/("frame" + std::to_string(next_))) << next_;
		if (!sink.Publish(next_, "field", static_cast<double>(next_), std::make_shared<const Bytes>(Bytes{1}), {}, error)) return false;
		++next_;
		r.status = next_ == 2 ? AdvanceResult::Status::Finished : AdvanceResult::Status::Advanced;
		r.position = Position();
		return true;
	}
	bool Checkpoint(Bytes&, std::string& error) override { return Fail(error, "no"); }
	SolverPosition Position() const override { return {static_cast<double>(next_), next_, next_ == 2}; }
	void RequestStop() override {}
private:
	fs::path dir_;
	std::int64_t next_ = 0;
};

void SideOutputs(const fs::path& root)
{
	std::string error;
	InProcessHost none(std::unique_ptr<ISimulationSolver>(new SideOutputSolver));
	Check(none.Configure(Map({}), EmptySceneInputs(), error) && !none.Start(error), "a side-output solver refuses to start without one");
	InProcessHost chosen(std::unique_ptr<ISimulationSolver>(new SideOutputSolver));
	const auto dir = root/"side"/"chosen";
	Check(chosen.Configure(Map({}), EmptySceneInputs(), error) && chosen.SetSideOutputDirectory(dir, error) &&
		chosen.Start(error) && chosen.StepToIndex(5, error) && fs::exists(dir/"frame1"), "an explicit side-output directory", error);
#ifndef _WIN32
	Check((fs::status(dir).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
		"the side-output directory is owner-only");
#endif
	InProcessHost recorded(std::unique_ptr<ISimulationSolver>(new SideOutputSolver));
	Check(recorded.Configure(Map({}), EmptySceneInputs(), error) && recorded.Record(root/"side"/"cache", budget, error) &&
		recorded.Start(error) && recorded.StepToIndex(5, error) && fs::exists(root/"side"/"cache"/"exports"/"frame0"),
		"a recording host gives the solver <cache>/<sideOutput>", error);
	InProcessHost plain(Make(MembraneSolver::Id));
	Check(plain.Configure(Map({}), EmptySceneInputs(), error) && !plain.SetSideOutputDirectory(dir, error),
		"a solver without a side output refuses one");
}

void Registry()
{
	std::string error;
	Check(!RegisterSolver(MembraneSolver::Id, []{ return std::unique_ptr<ISimulationSolver>(new MembraneSolver); }, error),
		"duplicate registration refused");
	Check(!CreateSolver("no.such.solver", error) && !error.empty(), "unknown solver reported");
	const auto ids = RegisteredSolvers();
	Check(std::find(ids.begin(), ids.end(), AdvectionDiffusionSolver::Id) != ids.end() &&
		std::find(ids.begin(), ids.end(), MembraneSolver::Id) != ids.end(), "registry lists reference solvers");
}
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
	const fs::path root = RISE::TestSupport::ScratchRunDir("rise_sim_host_", "suite");
	try {
		Registry();
		Conformance(AdvectionDiffusionSolver::Id, Map({}));
		Conformance(MembraneSolver::Id, Map({}));
		GridAccuracy();
		MeshFrames();
		MacroSteps();
		WorkerThreadAndSnapshots();
		InMemoryCheckpoint();
		PortTiming();
		Rollback(MembraneSolver::Id, Map({}), ConstantGrid(17, 0.5f, 1.0/16), ConstantGrid(17, -2.0f, 1.0/16));
		{
			auto source = [](float v) {
				GridFrame g;
				GridChannel c;
				c.name = "concentration rate"; c.units = "1/s";
				c.dimensions = {32, 32, 1};
				GridBlock b;
				b.dimensions = c.dimensions;
				b.active.assign(128, 0xff);
				b.values.assign(1024, F32(v));
				c.blocks.push_back(b);
				g.channels.push_back(c);
				auto bytes = std::make_shared<Bytes>();
				std::string error;
				EncodeGrid(g, budget, *bytes, error);
				return std::shared_ptr<const Bytes>(std::move(bytes));
			};
			Rollback(AdvectionDiffusionSolver::Id, Map({}), source(0.25f), source(-1.0f));
		}
		PauseCycles();
		RestoreDiscards();
		SnapshotSharing();
		LiveReaderCost();
		StaticsInMemory();
		if (DiskPublicationAvailable(root)) {
			Recording(root);
			Fidelity(root);
			PortFeeds(root);
			EvidenceScope(root);
			SideOutputs(root);
		}
	} catch (const std::exception& e) {
		++failures;
		std::cerr << "exception: " << e.what() << "\n";
	}
	std::error_code ignored;
	fs::remove_all(root, ignored);
	if (failures) { std::cerr << failures << " simulation host check(s) failed\n"; return 1; }
	std::cout << "Simulation solver contract and in-process host checks passed\n";
	return 0;
}
