// Solver contract and in-process host: conformance of both reference solvers,
// stop/checkpoint/restore equal to an uninterrupted run, immutable snapshots
// while the worker thread steps, external macro steps, one-way port feed from
// a cached output equal to the live feed, and evidence-driven fidelity.
#include "../src/Library/Simulation/SimulationHost.h"
#include "SimulationReferenceSolvers.h"
#include "TestTempDir.h"

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
	bool Publish(std::int64_t index, const std::string&, double, std::shared_ptr<const Bytes> payload,
		const Bytes&, std::string&) override { frames.emplace_back(index, std::move(payload)); return true; }
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
	Check(first->Advance(AdvanceRequest{}, head, nullptr, nullptr, r, error) && head.frames.size() == 3,
		name + " a stop request made between calls is cleared by the next Advance");
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
		if (!DecodeMesh(*f.second, budget, m, error) || !PeekMeshTopology(*f.second, t, error)) { fixed = false; break; }
		if (digest.empty()) digest = t.digest;
		fixed = fixed && t.digest == digest && !m.velocities.empty() && !m.normals.empty();
		for (std::size_t q = 2; q < m.velocities.size(); q += 3) moving = moving || FromF32(m.velocities[q]) != 0;
	}
	Check(fixed && moving && sink.frames.size() == d.cache.count, "membrane publishes a fixed-topology stream with velocities");
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
			!DecodeMesh(*x, budget, mx, error) || !DecodeMesh(*y, budget, my, error)) { worst = 1; break; }
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
	EvidenceCase c{"x", "", std::string(64, 'a'), 0.5, 0.1, true};
	forged.cases = {c};
	Check(EffectiveFidelity(forged) == "preview", "a case asserting pass with error above tolerance is preview");
	forged.cases[0].error = 0.05;
	Check(EffectiveFidelity(forged) == "predictive", "consistent passing evidence is predictive");
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
		if (DiskPublicationAvailable(root)) {
			Recording(root);
			Fidelity(root);
			PortFeeds(root);
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
