#include "pch.h"
#include "SceneSimulation.h"
#include "SimulationHost.h"
#include "SimulationWorker.h"
#include "SimulationParameterSchema.h"

#include <algorithm>
#include <functional>
#include <set>

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;
namespace fs = std::filesystem;
constexpr std::size_t manifestBudget = std::size_t(256) << 20;

bool Fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}
} // namespace

const char* SceneSimulationStateName(SceneSimulationState s)
{
	switch (s) {
	case SceneSimulationState::Missing: return "missing";
	case SceneSimulationState::Baking: return "baking";
	case SceneSimulationState::Partial: return "partial";
	case SceneSimulationState::Complete: return "complete";
	case SceneSimulationState::Stale: return "stale";
	case SceneSimulationState::Failed: break;
	}
	return "failed";
}

bool SceneSimulationParameters(const SceneSimulationSpec& spec, V& parameters, std::string& error)
{
	const ParameterSchema* schema = FindSolverSchema(spec.solverId);
	if (schema && schema->completeParameters) return schema->completeParameters(spec.parameters, parameters, error);
	parameters = spec.parameters;
	return true;
}

bool SceneSimulationGraph::Configure(const SceneSimulationSpec& spec, const ISceneInputs& inputs,
	SolverDescription& d, bool& keyUnknown, std::string& error)
{
	keyUnknown = false;
	const ParameterSchema* schema = FindSolverSchema(spec.solverId);
	V parameters;
	if (!SceneSimulationParameters(spec, parameters, error)) return false;
	// A solver keyed by prediction (no in-process Configure) also depends on
	// what its last bake derived.
	V baked;
	bool haveBaked = false;
	if (schema && schema->predictEffectiveParameters) {
		std::string solver, version, key, ignored;
		haveBaked = ReadWorkerJobRecord(spec.cacheDir, solver, version, baked, key, ignored) &&
			solver == spec.solverId && version == schema->solverVersion;
	}
	// Memo key: solver, the parameters, the identity of every scene input the
	// spec names (which is everything the solver can read), and a prediction's
	// baked values.
	Bytes params;
	if (!RISECBOR64::Encode(parameters, params, &error)) return false;
	V::Values identities;
	for (const auto& name : spec.sceneInputs) {
		std::string hash;
		if (!inputs.Identity(name, hash, error)) return Fail(error, "scene input `" + name + "`: " + error);
		identities.push_back(V::ArrayValue({V::String(name), V::String(hash)}));
	}
	Bytes memo, bakedBytes;
	if (haveBaked && !RISECBOR64::Encode(baked, bakedBytes, &error)) return false;
	if (!RISECBOR64::Encode(V::ArrayValue({V::String(spec.solverId), V::BytesValue(params),
		V::ArrayValue(identities), V::BytesValue(bakedBytes)}), memo, &error)) return false;
	const std::string memoKey = RISECBOR64::SHA256Hex(memo);
	{
		std::lock_guard<std::mutex> hold(mutex_);
		const auto it = memo_.find(memoKey);
		if (it != memo_.end()) { d = it->second; return true; }
	}
	SolverDescription candidate;
	if (schema && schema->predictEffectiveParameters) {
		// Keyed without Configure (the schema's prediction); the worker's
		// Configure remains authoritative, so a wrong prediction reads as stale.
		std::uint64_t frames = 0;
		if (!schema->predictEffectiveParameters(parameters, haveBaked ? &baked : nullptr, candidate.effectiveParameters,
			frames, error)) {
			keyUnknown = !haveBaked;
			return false;
		}
		candidate.solverId = spec.solverId;
		candidate.version = schema->solverVersion;
		candidate.cache.count = frames;
		for (const auto& o : schema->outputs) candidate.cache.outputs.push_back({o.name, o.codec, {}});
		if (!MakeCacheKey(candidate.solverId, candidate.version, candidate.effectiveParameters, {}, candidate.cache.key, error))
			return false;
	} else {
		auto solver = CreateSolver(spec.solverId, error);
		if (!solver) return false;
		if (!solver->Configure(parameters, inputs, candidate, error)) return false;
		if (!PrepareCacheDescription(candidate, inputs, "", error)) return false;
	}
	std::lock_guard<std::mutex> hold(mutex_);
	++configures_;
	memo_[memoKey] = candidate;
	d = std::move(candidate);
	return true;
}

std::size_t SceneSimulationGraph::MemoSize() const
{
	std::lock_guard<std::mutex> hold(mutex_);
	return memo_.size();
}

std::uint64_t SceneSimulationGraph::ConfigureCount() const
{
	std::lock_guard<std::mutex> hold(mutex_);
	return configures_;
}

void SceneSimulationGraph::ClearMemo()
{
	std::lock_guard<std::mutex> hold(mutex_);
	memo_.clear();
}

bool SceneSimulationGraph::Plan(const std::vector<SceneSimulationSpec>& specs, const ISceneInputProvider& provider,
	std::vector<SceneSimulationPlan>& plans, std::string& error)
{
	plans.clear();
	std::map<std::string, std::size_t> byName;
	for (std::size_t i = 0; i < specs.size(); ++i) {
		if (!byName.emplace(specs[i].name, i).second) return Fail(error, "two simulations are named " + specs[i].name);
	}
	// Direct dependencies of each simulation, through its scene inputs.
	std::vector<std::vector<std::string>> upstream(specs.size());
	for (std::size_t i = 0; i < specs.size(); ++i) {
		for (const auto& input : specs[i].sceneInputs) {
			std::vector<std::string> deps;
			if (!provider.InputDependencies(input, deps, error))
				return Fail(error, "simulation " + specs[i].name + ", scene input `" + input + "`: " + error);
			for (const auto& d : deps) {
				if (!byName.count(d)) return Fail(error, "simulation " + specs[i].name + " reads `" + input +
					"`, which depends on an unknown simulation " + d);
				if (std::find(upstream[i].begin(), upstream[i].end(), d) == upstream[i].end()) upstream[i].push_back(d);
			}
		}
	}
	// Depth-first topological order (upstream first), refusing cycles.
	std::vector<int> mark(specs.size(), 0);   // 0 new, 1 on the stack, 2 done
	std::vector<std::size_t> order;
	std::function<bool(std::size_t, std::vector<std::string>&)> visit =
		[&](std::size_t i, std::vector<std::string>& path) -> bool {
		if (mark[i] == 2) return true;
		path.push_back(specs[i].name);
		if (mark[i] == 1) {
			std::string cycle;
			for (const auto& p : path) cycle += (cycle.empty() ? "" : " -> ") + p;
			return Fail(error, "simulation dependency cycle: " + cycle);
		}
		mark[i] = 1;
		for (const auto& d : upstream[i]) if (!visit(byName[d], path)) return false;
		mark[i] = 2;
		path.pop_back();
		order.push_back(i);
		return true;
	};
	for (std::size_t i = 0; i < specs.size(); ++i) {
		std::vector<std::string> path;
		if (!visit(i, path)) return false;
	}
	std::map<std::string, std::string> keys;   // expected key by simulation
	for (const std::size_t i : order) {
		const SceneSimulationSpec& spec = specs[i];
		SceneSimulationPlan plan;
		plan.spec = &spec;
		plan.upstream = upstream[i];
		if (spec.solverId.empty()) {
			// A foreign producer: its key is whatever its cache says.
			CacheSnapshot snapshot;
			std::string ignored;
			if (CacheSnapshot::Open(spec.cacheDir, manifestBudget, snapshot, ignored))
				keys[spec.name] = snapshot.Description().key;
			plan.configured = true;
			plans.push_back(std::move(plan));
			continue;
		}
		std::map<std::string, std::string> upstreamKeys;
		bool upstreamOk = true;
		for (const auto& d : plan.upstream) {
			const auto k = keys.find(d);
			const SceneSimulationPlan* up = nullptr;
			for (const auto& p : plans) if (p.spec->name == d) up = &p;
			SceneSimulationStatus upState;
			if (up) InspectSceneSimulation(*up, upState);
			if (k == keys.end() || k->second.empty() || !up || upState.state != SceneSimulationState::Complete) {
				upstreamOk = false;
				plan.waitingForUpstream = true;
				plan.error = "waits for upstream simulation " + d + " (" +
					(up ? SceneSimulationStateName(upState.state) : "unplanned") + ")";
				break;
			}
			upstreamKeys[d] = k->second;
		}
		if (upstreamOk) {
			std::string why;
			plan.inputs = provider.Capture(spec.sceneInputs, upstreamKeys, why);
			if (!plan.inputs) plan.error = "scene inputs: " + why;
			else if (!Configure(spec, *plan.inputs, plan.description, plan.keyUnknown, why))
				plan.error = (plan.keyUnknown ? "key unknown until the first bake: " : "configure: ") + why;
			else {
				plan.configured = true;
				keys[spec.name] = plan.description.cache.key;
			}
		}
		plans.push_back(std::move(plan));
	}
	return true;
}

void InspectSceneSimulation(const SceneSimulationPlan& plan, SceneSimulationStatus& s)
{
	const SceneSimulationSpec& spec = *plan.spec;
	s = SceneSimulationStatus();
	s.name = spec.name;
	s.keyword = spec.keyword;
	s.solverId = spec.solverId;
	s.cacheDir = spec.cacheDir;
	s.upstream = plan.upstream;
	s.inputs = spec.sceneInputs;
	if (plan.configured && !spec.solverId.empty()) {
		const auto& d = plan.description;
		s.expectedKey = d.cache.key;
		s.interactive = d.capabilities.interactive;
		s.backend = d.capabilities.backend;
		s.framesTotal = d.cache.count;
		s.startTime = d.cache.startTime;
		s.interval = d.cache.interval;
	}
	std::string error;
	const CacheState disk = InspectCache(spec.cacheDir, manifestBudget, error);
	if (disk == CacheState::Corrupt) {
		s.state = SceneSimulationState::Failed;
		s.error = "cache " + spec.cacheDir.string() + " is corrupt: " + error;
		return;
	}
	CacheSnapshot snapshot;
	bool open = false;
	if (disk != CacheState::Missing) {
		std::string why;
		open = CacheSnapshot::Open(spec.cacheDir, manifestBudget, snapshot, why);
		if (!open && disk != CacheState::Baking) {
			s.state = SceneSimulationState::Failed;
			s.error = "cache " + spec.cacheDir.string() + ": " + why;
			return;
		}
	}
	if (open) {
		const auto& d = snapshot.Description();
		s.cacheKey = d.key;
		s.fidelity = d.fidelity;
		const auto outputs = d.outputs.size();
		s.framesAvailable = outputs ? snapshot.Entries().size() / outputs : 0;
		if (spec.solverId.empty()) {
			s.expectedKey = d.key;
			s.framesTotal = d.count;
			s.startTime = d.startTime;
			s.interval = d.interval;
		}
	}
	if (plan.keyUnknown) {
		s.state = disk == CacheState::Missing ? SceneSimulationState::Missing :
			disk == CacheState::Baking ? SceneSimulationState::Baking : SceneSimulationState::Stale;
		s.error = plan.error;
		return;
	}
	if (plan.waitingForUpstream) {
		s.state = disk == CacheState::Missing ? SceneSimulationState::Missing :
			disk == CacheState::Baking ? SceneSimulationState::Baking : SceneSimulationState::Stale;
		s.upstreamOutOfDate = true;
		s.error = plan.error;
		return;
	}
	if (!plan.configured) {
		s.state = SceneSimulationState::Failed;
		s.error = plan.error;
		return;
	}
	if (disk == CacheState::Missing) {
		s.state = SceneSimulationState::Missing;
		return;
	}
	if (disk == CacheState::Baking) { s.state = SceneSimulationState::Baking; return; }
	if (!spec.solverId.empty() && s.cacheKey != s.expectedKey) { s.state = SceneSimulationState::Stale; return; }
	if (disk == CacheState::Complete) { s.state = SceneSimulationState::Complete; return; }
	// Partial: a bake that failed (and was not resumed) says so in its status sidecar.
	WorkerStatus worker;
	std::string ignored;
	if (ReadWorkerStatus(spec.cacheDir, worker, ignored) && worker.state == "failed") {
		s.state = SceneSimulationState::Failed;
		s.error = "the last bake failed: " + worker.lastError;
		return;
	}
	s.state = SceneSimulationState::Partial;
}

bool SceneSimulationGraph::Describe(const std::vector<SceneSimulationSpec>& specs,
	const ISceneInputProvider& provider, std::vector<SceneSimulationStatus>& statuses, std::string& error)
{
	statuses.clear();
	std::vector<SceneSimulationPlan> plans;
	if (!Plan(specs, provider, plans, error)) return false;
	std::vector<SceneSimulationStatus> ordered;
	ordered.reserve(plans.size());
	for (const auto& plan : plans) {
		SceneSimulationStatus s;
		InspectSceneSimulation(plan, s);
		ordered.push_back(std::move(s));
	}
	// Propagation: plans are in dependency order, so each upstream is final.
	for (std::size_t i = 0; i < ordered.size(); ++i) {
		for (const auto& u : ordered[i].upstream) {
			for (std::size_t j = 0; j < i; ++j) {
				if (ordered[j].name != u) continue;
				if (ordered[j].state != SceneSimulationState::Complete || ordered[j].upstreamOutOfDate)
					ordered[i].upstreamOutOfDate = true;
			}
		}
	}
	// Report in declaration order.
	for (const auto& spec : specs)
		for (auto& s : ordered) if (s.name == spec.name) statuses.push_back(s);
	return true;
}

} }
