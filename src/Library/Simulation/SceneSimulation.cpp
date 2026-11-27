#include "pch.h"
#include "SceneSimulation.h"
#include "SimulationHost.h"
#include "SimulationWorker.h"
#include "SimulationParameterSchema.h"
#include "SimulationGroup.h"

#include <cstdlib>
#include <sstream>

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

// ------------------------------------------------------------------ M6: coupling syntax

std::vector<std::string> Tokens(const std::string& text)
{
	std::istringstream in(text);
	std::vector<std::string> out;
	std::string t;
	while (in >> t) out.push_back(t);
	return out;
}

bool SplitDotted(const std::string& s, std::string& a, std::string& b)
{
	const auto dot = s.find('.');
	if (dot == std::string::npos || dot == 0 || dot + 1 == s.size()) return false;
	a = s.substr(0, dot);
	b = s.substr(dot + 1);
	return true;
}

// `key=value` adapter parameters: a number, TRUE/FALSE, a comma list of
// names, or text.
V ParameterValue(const std::string& v)
{
	char* end = nullptr;
	const double x = std::strtod(v.c_str(), &end);
	if (!v.empty() && end && *end == '\0') return V::Float(x);
	if (v == "TRUE" || v == "true") return V::Bool(true);
	if (v == "FALSE" || v == "false") return V::Bool(false);
	if (v.find(',') != std::string::npos) {
		V::Values list;
		std::string item;
		std::istringstream in(v);
		while (std::getline(in, item, ',')) if (!item.empty()) list.push_back(V::String(item));
		return V::ArrayValue(list);
	}
	return V::String(v);
}

// The tail of a connection: [via <adapter> [key=value ...]] [aux <member>.<port> ...]
// [time_offset <s>] (port_source: the upstream cache is read at its time = this one's + s).
bool ParseVia(const std::vector<std::string>& t, std::size_t i, PortConnection& c, const std::string& where, std::string& error)
{
	V::Members params;
	while (i < t.size()) {
		if (t[i] == "via") {
			if (i + 1 >= t.size() || !c.adapter.empty()) return Fail(error, where + ": `via` names one adapter");
			c.adapter = t[i + 1];
			i += 2;
			while (i < t.size() && t[i] != "aux" && t[i] != "via" && t[i] != "time_offset") {
				const auto eq = t[i].find('=');
				if (eq == std::string::npos || eq == 0) return Fail(error, where + ": adapter parameters are key=value, not `" + t[i] + "`");
				params.emplace_back(t[i].substr(0, eq), ParameterValue(t[i].substr(eq + 1)));
				++i;
			}
		} else if (t[i] == "time_offset") {
			char* end = nullptr;
			if (i + 1 >= t.size()) return Fail(error, where + ": `time_offset` takes seconds");
			(void)std::strtod(t[i + 1].c_str(), &end);
			if (!end || *end != '\0') return Fail(error, where + ": `time_offset` takes seconds");
			i += 2;   // read by TimeOffsetOf
		} else if (t[i] == "aux") {
			std::string m, port;
			if (i + 1 >= t.size() || !SplitDotted(t[i + 1], m, port)) return Fail(error, where + ": `aux` names <member>.<port>");
			c.auxiliary.emplace_back(m, port);
			i += 2;
		} else return Fail(error, where + ": unexpected `" + t[i] + "`");
	}
	c.adapterParameters = V::MapValue(params);
	return true;
}

double TimeOffsetOf(const std::string& text)
{
	const auto t = Tokens(text);
	for (std::size_t i = 0; i + 1 < t.size(); ++i) if (t[i] == "time_offset") return std::strtod(t[i + 1].c_str(), nullptr);
	return 0;
}

// `connect <a>.<port> -> <b>.<port> [via ...] [aux ...]`
bool ParseConnect(const std::string& text, PortConnection& c, std::string& error)
{
	const auto t = Tokens(text);
	const std::string where = "connect `" + text + "`";
	if (t.size() < 3 || t[1] != "->" || !SplitDotted(t[0], c.fromMember, c.fromPort) || !SplitDotted(t[2], c.toMember, c.toPort))
		return Fail(error, where + ": expected `<member>.<port> -> <member>.<port> [via <adapter> [key=value ...]] [aux <member>.<port>]`");
	return ParseVia(t, 3, c, where, error);
}

// `port_source <port> from <simulation>.<output> [via ...] [aux ...]` on `downstream`.
bool ParsePortSource(const std::string& text, const std::string& downstream, PortConnection& c, std::string& error)
{
	const auto t = Tokens(text);
	const std::string where = "port_source `" + text + "`";
	if (t.size() < 3 || t[1] != "from" || !SplitDotted(t[2], c.fromMember, c.fromPort))
		return Fail(error, where + ": expected `<port> from <simulation>.<output> [via <adapter> [key=value ...]] [aux <member>.<port>]`");
	c.toMember = downstream;
	c.toPort = t[0];
	if (!ParseVia(t, 3, c, where, error)) return false;
	// The drag model reads the loaded surface: the downstream's own state unless stated.
	if (c.adapter == "aerodynamic_drag" && c.auxiliary.empty()) c.auxiliary.emplace_back(downstream, "surface_state");
	return true;
}

std::vector<std::string> TextList(const V* v)
{
	std::vector<std::string> out;
	if (v && v->GetType() == V::Array) for (const V& e : v->GetArray()) if (e.GetType() == V::Text) out.push_back(e.GetText());
	return out;
}
double RealOf(const V& m, const char* key, double fallback)
{
	const V* v = m.Find(key);
	return v && v->GetType() == V::Float64 ? v->GetFloat() : v && v->GetType() == V::UnsignedInteger ?
		static_cast<double>(v->GetIntegerArgument()) : fallback;
}
std::string TextOf(const V& m, const char* key, const std::string& fallback)
{
	const V* v = m.Find(key);
	return v && v->GetType() == V::Text ? v->GetText() : fallback;
}

bool GroupPolicyOf(const V& m, GroupPolicy& p, std::string& error)
{
	const std::string kind = TextOf(m, "policy", "staggered"), order = TextOf(m, "order", "gauss-seidel");
	if (kind != "staggered" && kind != "implicit") return Fail(error, "policy is staggered or implicit");
	if (order != "gauss-seidel" && order != "jacobi") return Fail(error, "order is gauss-seidel or jacobi");
	p.order = order == "jacobi" ? ExchangeOrder::Jacobi : ExchangeOrder::GaussSeidel;
	p.macroStep = RealOf(m, "macro_step", 0);
	p.memberOrder = Tokens(TextOf(m, "member_order", ""));
	// M7b: `owned <owner> <partner> [<partner> ...]` (GroupPolicy::ownedExchanges).
	for (const std::string& line : TextList(m.Find("owned"))) {
		const auto t = Tokens(line);
		if (t.size() < 2) return Fail(error, "owned names an owner and at least one partner: `" + line + "`");
		GroupPolicy::OwnedExchange e;
		e.owner = t[0];
		e.partners.assign(t.begin() + 1, t.end());
		p.ownedExchanges.push_back(e);
	}
	if (kind == "implicit") {
		p.subIterations = static_cast<unsigned>(RealOf(m, "sub_iterations", 30));
		p.convergenceTolerance = RealOf(m, "tolerance", 1e-6);
		p.relaxation = RealOf(m, "relaxation", 1);
		const V* a = m.Find("aitken");
		p.aitken = a && a->GetType() == V::Boolean ? a->GetBoolean() : true;
		p.onNonConvergence = TextOf(m, "on_non_convergence", "fail") == "halve" ? GroupPolicy::NonConvergence::Halve :
			GroupPolicy::NonConvergence::Fail;
		p.minimumMacroStep = RealOf(m, "minimum_macro_step", 0);
		p.absoluteTolerance = RealOf(m, "absolute_tolerance", 0);
		if (p.subIterations < 2) return Fail(error, "an implicit group needs sub_iterations of at least 2");
	}
	return true;
}
} // namespace

const char* const kSimulationGroupKeyword = "simulation_group";

std::vector<std::string> SceneGroupLiveMembers(const SceneSimulationSpec& spec)
{
	if (spec.keyword == kSimulationGroupKeyword) return TextList(spec.parameters.Find("members"));
	if (!spec.portSources.empty()) return {spec.name};
	return {};
}

const SceneSimulationSpec* SceneGroupOf(const std::vector<SceneSimulationSpec>& specs, const std::string& simulation)
{
	for (const auto& s : specs) {
		if (s.keyword != kSimulationGroupKeyword) continue;
		const auto members = SceneGroupLiveMembers(s);
		if (std::find(members.begin(), members.end(), simulation) != members.end()) return &s;
	}
	return nullptr;
}

bool BuildSceneGroup(const SceneSimulationSpec& spec, const std::vector<SceneSimulationPlan>& plans, SimulationGroup& group,
	std::string& error)
{
	auto planOf = [&](const std::string& name) -> const SceneSimulationPlan* {
		for (const auto& p : plans) if (p.spec && p.spec->name == name) return &p;
		return nullptr;
	};
	auto live = [&](const std::string& name) -> bool {
		const SceneSimulationPlan* p = planOf(name);
		if (!p) return Fail(error, "no simulation named " + name);
		if (p->spec->solverId.empty()) return Fail(error, name + " has no solver to run");
		if (!p->configured || !p->inputs) return Fail(error, name + " is not configured: " + p->error);
		V parameters;
		if (!SceneSimulationParameters(*p->spec, parameters, error)) return false;
		auto solver = CreateSolver(p->spec->solverId, error);
		return solver && group.AddMember(name, std::move(solver), parameters, error) && group.SetMemberInputs(name, *p->inputs, error);
	};
	GroupPolicy policy;
	std::vector<PortConnection> connections;
	if (spec.keyword == kSimulationGroupKeyword) {
		const auto members = SceneGroupLiveMembers(spec);
		if (members.empty()) return Fail(error, "simulation_group " + spec.name + " names no members");
		for (const auto& m : members) if (!live(m)) return Fail(error, "simulation_group " + spec.name + ": " + error);
		for (const auto& line : TextList(spec.parameters.Find("connect"))) {
			PortConnection c;
			if (!ParseConnect(line, c, error)) return false;
			connections.push_back(c);
		}
		if (!GroupPolicyOf(spec.parameters, policy, error)) return Fail(error, "simulation_group " + spec.name + ": " + error);
	} else {
		if (spec.portSources.empty()) return Fail(error, spec.name + " is not coupled");
		if (!live(spec.name)) return false;
		std::set<std::string> cached;
		for (const auto& line : spec.portSources) {
			PortConnection c;
			if (!ParsePortSource(line, spec.name, c, error)) return Fail(error, spec.name + ": " + error);
			if (!cached.count(c.fromMember)) {
				const SceneSimulationPlan* up = planOf(c.fromMember);
				if (!up) return Fail(error, spec.name + ": port_source reads an unknown simulation " + c.fromMember);
				auto snapshot = std::make_shared<CacheSnapshot>();
				if (!CacheSnapshot::Open(up->spec->cacheDir, manifestBudget, *snapshot, error) || !snapshot->Complete())
					return Fail(error, spec.name + ": the cache of " + c.fromMember + " is not complete (" + error + ")");
				if (!group.AddCachedMember(c.fromMember, snapshot, up->spec->solverId.empty(), error) ||
					!group.SetCachedTimeOffset(c.fromMember, TimeOffsetOf(line), error)) return false;
				cached.insert(c.fromMember);
			}
			connections.push_back(c);
		}
	}
	for (const auto& c : connections) if (!group.Connect(c, error)) return false;
	return group.Configure(policy, EmptySceneInputs(), error);
}

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
		if (schema->describeUnkeyed) schema->describeUnkeyed(candidate.effectiveParameters, candidate);   // M5c
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
	// M6: a group depends on its members; a port_source on the simulation it reads.
	for (std::size_t i = 0; i < specs.size(); ++i) {
		std::vector<std::string> deps;
		if (specs[i].keyword == kSimulationGroupKeyword) deps = SceneGroupLiveMembers(specs[i]);
		for (const auto& line : specs[i].portSources) {
			PortConnection c;
			if (!ParsePortSource(line, specs[i].name, c, error)) return Fail(error, "simulation " + specs[i].name + ": " + error);
			deps.push_back(c.fromMember);
		}
		for (const auto& d : deps) {
			if (!byName.count(d)) return Fail(error, "simulation " + specs[i].name + " couples with an unknown simulation " + d);
			if (std::find(upstream[i].begin(), upstream[i].end(), d) == upstream[i].end()) upstream[i].push_back(d);
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
		if (spec.keyword == kSimulationGroupKeyword) {
			// M6: the members' keys and labels are the group's.
			SimulationGroup group;
			std::string why;
			if (!BuildSceneGroup(spec, plans, group, why)) plan.error = why;
			else {
				for (auto& p : plans) {
					const SolverDescription* d = p.spec ? group.Description(p.spec->name) : nullptr;
					if (!d) continue;
					p.description = *d;
					keys[p.spec->name] = d->cache.key;
				}
				plan.configured = true;
				plan.description.solverId = kSimulationGroupKeyword;
				plan.description.cache.key = group.GroupIdentity();
				keys[spec.name] = group.GroupIdentity();
			}
			plans.push_back(std::move(plan));
			continue;
		}
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
		if (plan.configured && !spec.portSources.empty()) {
			// M6: fed one-way from caches: the key covers each upstream cache and adapter.
			std::vector<SceneSimulationPlan> view;
			for (const auto& p : plans) {
				SceneSimulationPlan q;
				q.spec = p.spec; q.configured = p.configured; q.inputs = p.inputs; q.error = p.error;
				view.push_back(q);
			}
			SceneSimulationPlan self;
			self.spec = plan.spec; self.configured = true; self.inputs = plan.inputs;
			view.push_back(self);
			SimulationGroup group;
			std::string why;
			if (!BuildSceneGroup(spec, view, group, why)) { plan.configured = false; plan.error = "coupling: " + why; keys.erase(spec.name); }
			else {
				plan.description = *group.Description(spec.name);
				keys[spec.name] = plan.description.cache.key;
			}
		}
		plans.push_back(std::move(plan));
	}
	return true;
}

namespace {
// "parameters changed (<members>)" naming the top-level effective-parameter
// members that differ (a collider shows as the solver's derived member, e.g.
// cloth's `colliders`); "" when equal; "configuration changed" when they
// cannot be compared.
std::string ParameterChange(const V& baked, const V& now)
{
	std::string ignored;
	Bytes a, b;
	if (!RISECBOR64::Encode(baked, a, &ignored) || !RISECBOR64::Encode(now, b, &ignored)) return "configuration changed";
	if (a == b) return std::string();
	std::string names;
	if (baked.GetType() == V::Map && now.GetType() == V::Map) {
		std::set<std::string> keys;
		for (const auto& m : baked.GetMap()) keys.insert(m.first);
		for (const auto& m : now.GetMap()) keys.insert(m.first);
		for (const auto& k : keys) {
			const V* x = baked.Find(k);
			const V* y = now.Find(k);
			Bytes ex, ey;
			if (x) RISECBOR64::Encode(*x, ex, &ignored);
			if (y) RISECBOR64::Encode(*y, ey, &ignored);
			if (!x || !y || ex != ey) names += (names.empty() ? "" : ", ") + k;
		}
	}
	return names.empty() ? "parameters changed" : "parameters changed (" + names + ")";
}

// What a group's member recorded when the group baked (the manifest each
// recorded member's directory holds, SimulationGroup::WriteGroupManifests).
struct GroupMemberRecord {
	std::string solver, version;
	V parameters;
	bool haveParameters = false;
	std::string key;
};
struct GroupRecord {
	std::string identity;
	std::map<std::string, GroupMemberRecord> members;
};
bool ReadGroupRecord(const fs::path& directory, GroupRecord& record)
{
	Bytes bytes;
	std::string error;
	V v;
	if (!ReadSidecarFile(directory, kGroupManifestName, std::size_t(16) << 20, bytes, error) ||
		!RISECBOR64::DecodeCanonical(bytes, v, &error) || v.GetType() != V::Map) return false;
	const V* identity = v.Find("identity");
	const V* members = v.Find("members");
	if (!identity || identity->GetType() != V::Text || !members || members->GetType() != V::Array) return false;
	record.identity = identity->GetText();
	for (const auto& m : members->GetArray()) {
		const V* name = m.Find("name");
		if (m.GetType() != V::Map || !name || name->GetType() != V::Text) continue;
		GroupMemberRecord r;
		if (const V* x = m.Find("solver")) if (x->GetType() == V::Text) r.solver = x->GetText();
		if (const V* x = m.Find("version")) if (x->GetType() == V::Text) r.version = x->GetText();
		if (const V* x = m.Find("key")) if (x->GetType() == V::Text) r.key = x->GetText();
		if (const V* x = m.Find("parameters")) { r.parameters = *x; r.haveParameters = true; }
		record.members[name->GetText()] = std::move(r);
	}
	return true;
}

// Why one member's own configuration differs from what the group baked: ""
// when its solver, version and effective parameters are the same.
std::string MemberOwnChange(const GroupMemberRecord& baked, const SceneSimulationPlan& plan)
{
	if (baked.solver != plan.description.solverId || baked.version != plan.description.version) return "solver version changed";
	if (!baked.haveParameters) return "configuration changed";
	return ParameterChange(baked.parameters, plan.description.effectiveParameters);
}

// The specific reason a member of a group is stale (the job record a single
// simulation has does not exist for it): its own solver, version or
// parameters; another member's; the coupling (connections, policy, adapters);
// its scene inputs.  "configuration changed" when the cache predates the
// record of parameters.
std::string GroupMemberStaleReason(const SceneSimulationPlan& plan, const SceneSimulationSpec& group,
	const std::vector<SceneSimulationPlan>& plans)
{
	GroupRecord record;
	if (!ReadGroupRecord(plan.spec->cacheDir, record)) return "configuration changed";
	const auto self = record.members.find(plan.spec->name);
	if (self == record.members.end()) return "configuration changed";
	if (self->second.solver.empty() && !self->second.haveParameters) return "configuration changed";   // baked before parameters were recorded
	const std::string own = MemberOwnChange(self->second, plan);
	if (!own.empty()) return own;
	for (const auto& other : record.members) {
		if (other.first == plan.spec->name) continue;
		for (const auto& p : plans) {
			if (!p.spec || p.spec->name != other.first || !p.configured) continue;
			const std::string why = MemberOwnChange(other.second, p);
			if (!why.empty() && why != "configuration changed") return "member " + other.first + ": " + why;
		}
	}
	for (const auto& p : plans) {
		if (p.spec != &group) continue;
		if (p.configured && p.description.cache.key != record.identity) return "coupling changed (connections, policy or adapters)";
	}
	if (!plan.spec->sceneInputs.empty()) {
		std::string names;
		for (const auto& n : plan.spec->sceneInputs) names += (names.empty() ? "" : ", ") + n;
		return "scene input changed (" + names + ")";
	}
	return "configuration changed";
}
} // namespace

namespace {
// M5c: why the cache's key differs from the configuration's, from the job
// record the worker wrote beside the cache (solver, version, effective
// parameters).  The key covers exactly those plus the scene inputs'
// identities, so equal parameters mean an input changed.
std::string StaleReason(const SceneSimulationSpec& spec, const SceneSimulationPlan& plan)
{
	std::string solver, version, key, ignored;
	V baked;
	if (!ReadWorkerJobRecord(spec.cacheDir, solver, version, baked, key, ignored)) return "configuration changed";
	if (solver != plan.description.solverId || version != plan.description.version) return "solver version changed";
	const std::string changed = ParameterChange(baked, plan.description.effectiveParameters);
	if (!changed.empty()) return changed;
	if (spec.sceneInputs.empty()) return "configuration changed";
	std::string names;
	for (const auto& n : spec.sceneInputs) names += (names.empty() ? "" : ", ") + n;
	return "scene input changed (" + names + ")";
}
} // namespace

std::string FormatSimulationEstimate(const SceneSimulationStatus& status)
{
	std::string out;
	if (status.wallEstimate > 0) {
		const double t = status.wallEstimate;
		char buf[64];
		if (t < 90) std::snprintf(buf, sizeof(buf), "%.0f s", std::max(1.0, t));
		else if (t < 5400) std::snprintf(buf, sizeof(buf), "%.0f min", t / 60);
		else if (t < 48 * 3600) std::snprintf(buf, sizeof(buf), "%.0f h", t / 3600);
		else std::snprintf(buf, sizeof(buf), "%.0f d", t / 86400);
		out = std::string("\u2248 ") + buf;
	}
	if (status.memoryEstimate > 0) {
		const double m = static_cast<double>(status.memoryEstimate);
		char buf[64];
		if (m >= 1e9) std::snprintf(buf, sizeof(buf), "%.1f GB", m / 1e9);
		else std::snprintf(buf, sizeof(buf), "%.0f MB", std::max(1.0, m / 1e6));
		out += (out.empty() ? std::string("\u2248 ") : std::string(", ")) + buf;
	}
	return out;
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
	// GUI status pass: the couplings as authored (a group's connect lines and
	// ownership, then any port_source lines).
	if (spec.keyword == kSimulationGroupKeyword) {
		for (const auto& line : TextList(spec.parameters.Find("connect"))) s.connections.push_back(line);
		for (const auto& line : TextList(spec.parameters.Find("owned"))) s.connections.push_back("owned " + line);
	}
	for (const auto& line : spec.portSources) s.connections.push_back("port_source " + line);
	if (plan.configured && !spec.solverId.empty()) {
		const auto& d = plan.description;
		s.fidelityLimit = d.capabilities.fidelityLimit;
		s.expectedKey = d.cache.key;
		s.interactive = d.capabilities.interactive;
		s.backend = d.capabilities.backend;
		s.memoryEstimate = d.capabilities.memoryEstimate;
		s.costEstimate = d.capabilities.costEstimate;
		s.framesTotal = d.cache.count;
		s.startTime = d.cache.startTime;
		s.interval = d.cache.interval;
		if (s.costEstimate > 0 && s.framesTotal > 0)   // M5c: a bake simulates from 0 to the last frame
			s.wallEstimate = s.costEstimate * (std::max(0.0, s.startTime) +
				s.interval * static_cast<double>(s.framesTotal - 1));
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
		s.evidenceScope = d.evidenceScope;
		s.evidenceCases = static_cast<std::uint32_t>(d.evidence.size());
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
		if (s.state == SceneSimulationState::Stale) s.staleReason = plan.error;
		return;
	}
	if (plan.waitingForUpstream) {
		s.state = disk == CacheState::Missing ? SceneSimulationState::Missing :
			disk == CacheState::Baking ? SceneSimulationState::Baking : SceneSimulationState::Stale;
		s.upstreamOutOfDate = true;
		s.error = plan.error;
		if (s.state == SceneSimulationState::Stale) s.staleReason = "upstream out of date";
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
	if (!spec.solverId.empty() && s.cacheKey != s.expectedKey) {
		s.state = SceneSimulationState::Stale;
		s.staleReason = StaleReason(spec, plan);
		return;
	}
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
	// A member of a simulation_group has no job record (the group baked it), so
	// its reason comes from the group manifest beside its cache.
	for (std::size_t i = 0; i < ordered.size(); ++i) {
		if (ordered[i].state != SceneSimulationState::Stale || ordered[i].staleReason != "configuration changed") continue;
		if (plans[i].spec->keyword == kSimulationGroupKeyword) continue;
		const SceneSimulationSpec* group = SceneGroupOf(specs, plans[i].spec->name);
		if (group) ordered[i].staleReason = GroupMemberStaleReason(plans[i], *group, plans);
	}
	// Propagation: plans are in dependency order, so each upstream is final.
	for (std::size_t i = 0; i < ordered.size(); ++i) {
		for (const auto& u : ordered[i].upstream) {
			for (std::size_t j = 0; j < i; ++j) {
				if (ordered[j].name != u) continue;
				if (ordered[j].state != SceneSimulationState::Complete || ordered[j].upstreamOutOfDate) {
					ordered[i].upstreamOutOfDate = true;
					// M5c: an input that changed because its upstream did is the upstream's doing.
					if (ordered[i].staleReason.rfind("scene input changed", 0) == 0)
						ordered[i].staleReason = "upstream out of date";
				}
			}
		}
	}
	// M6: a group's state is its members' (complete only when all are).
	for (auto& g : ordered) {
		if (g.keyword != kSimulationGroupKeyword) continue;
		if (g.state == SceneSimulationState::Failed && !g.error.empty()) continue;
		bool all = true, any = false, stale = false, baking = false, failed = false;
		for (const auto& m : g.upstream)
			for (const auto& s : ordered) if (s.name == m) {
				all = all && s.state == SceneSimulationState::Complete;
				any = any || s.state != SceneSimulationState::Missing;
				stale = stale || s.state == SceneSimulationState::Stale;
				baking = baking || s.state == SceneSimulationState::Baking;
				failed = failed || s.state == SceneSimulationState::Failed;
				g.framesAvailable = std::max(g.framesAvailable, s.framesAvailable);
				g.framesTotal = std::max(g.framesTotal, s.framesTotal);
				if (g.fidelity.empty() || s.fidelity == "preview") {
					g.fidelity = s.fidelity;
					g.evidenceScope = s.evidenceScope;
					g.evidenceCases = s.evidenceCases;
					g.fidelityLimit = s.fidelityLimit;
				}
			}
		if (stale) {
			// The group's own reason: each stale member's, by name.
			g.staleReason.clear();
			for (const auto& m : g.upstream)
				for (const auto& s : ordered)
					if (s.name == m && s.state == SceneSimulationState::Stale)
						g.staleReason += (g.staleReason.empty() ? "" : "; ") + m + ": " + s.staleReason;
		}
		g.state = all ? SceneSimulationState::Complete : failed ? SceneSimulationState::Failed : baking ? SceneSimulationState::Baking :
			stale ? SceneSimulationState::Stale : any ? SceneSimulationState::Partial : SceneSimulationState::Missing;
		g.upstreamOutOfDate = false;
	}
	// Report in declaration order.
	for (const auto& spec : specs)
		for (auto& s : ordered) if (s.name == spec.name) statuses.push_back(s);
	return true;
}

} }
