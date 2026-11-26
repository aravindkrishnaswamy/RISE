#include "pch.h"
#include "SceneSimulationWorker.h"
#include "SceneSimulation.h"
#include "SimulationGroup.h"
#include "../Job.h"
#include "../Utilities/Reference.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <set>

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace RISE { namespace Simulation {
namespace {
using V = RISECBOR64::Value;

std::string Short(const std::string& key)
{
	return key.size() > 12 ? key.substr(0, 12) : key;
}

void PrintStatus(const SceneSimulationStatus& s, std::ostream& out)
{
	out << s.name << ": " << SceneSimulationStateName(s.state);
	if (s.upstreamOutOfDate) out << " (upstream out of date)";
	out << "\n  producer: " << s.keyword << (s.solverId.empty() ? "" : " (" + s.solverId + ")");
	if (!s.backend.empty()) out << ", backend " << s.backend;
	if (s.interactive) out << ", interactive";
	out << "\n  cache: " << s.cacheDir.string();
	out << "\n  frames: " << s.framesAvailable << " / " << s.framesTotal;
	if (!s.fidelity.empty()) out << ", " << s.fidelity;
	out << "\n  key: " << (s.expectedKey.empty() ? "-" : Short(s.expectedKey));
	if (!s.cacheKey.empty() && s.cacheKey != s.expectedKey) out << " (cache holds " << Short(s.cacheKey) << ")";
	if (!s.inputs.empty()) {
		out << "\n  scene inputs:";
		for (const auto& i : s.inputs) out << " " << i;
	}
	if (!s.upstream.empty()) {
		out << "\n  upstream:";
		for (const auto& u : s.upstream) out << " " << u;
	}
	if (!s.error.empty()) out << "\n  error: " << s.error;
	out << "\n";
}

struct JobHolder {
	IJobPriv* job = nullptr;
	~JobHolder() { if (job) job->release(); }
};

// M6: bakes a coupled run as one job: a simulation_group (every member), or a
// simulation fed one-way through port_source lines (its upstream caches as
// cached members).  Sidecars of the run (rise-sim.group.checkpoint, a status)
// live in `spec.cacheDir` (the group's own directory, or the downstream's
// cache); each live member records its own cache, with a status sidecar and
// the group manifest beside it.  Stops at macro-step boundaries on a
// rise-sim.stop file (in the run's or any member's directory) or after
// --stop-after-frames macro steps, with a checkpoint; resume continues it.
constexpr char groupCheckpointName[] = "rise-sim.group.checkpoint";

int BakeCoupled(const SceneSimulationSpec& spec, const std::vector<SceneSimulationPlan>& plans, const std::string& command,
	const WorkerOptions& options, std::ostream& out, std::ostream& err)
{
	namespace fs = std::filesystem;
	const auto members = SceneGroupLiveMembers(spec);
	auto planOf = [&](const std::string& name) -> const SceneSimulationPlan* {
		for (const auto& p : plans) if (p.spec->name == name) return &p;
		return nullptr;
	};
	bool allComplete = true, allPartial = true;
	for (const auto& m : members) {
		const SceneSimulationPlan* p = planOf(m);
		if (!p) { err << "rise-sim: " << spec.name << ": no member " << m << "\n"; return WorkerFailed; }
		SceneSimulationStatus st;
		InspectSceneSimulation(*p, st);
		allComplete = allComplete && st.state == SceneSimulationState::Complete;
		allPartial = allPartial && (st.state == SceneSimulationState::Partial || st.state == SceneSimulationState::Failed);
	}
	if (command == "bake" && allComplete && !options.force) { out << "rise-sim: " << spec.name << ": up to date\n"; return WorkerFinished; }
	if (command == "resume" && !allPartial) { out << "rise-sim: " << spec.name << ": nothing to resume\n"; return WorkerFinished; }
	std::string error;
	SimulationGroup group;
	if (!BuildSceneGroup(spec, plans, group, error)) { err << "rise-sim: " << spec.name << ": " << error << "\n"; return WorkerFailed; }
	const fs::path dir = spec.cacheDir;
	if (!CheckCacheLocation(dir, true, error)) { err << "rise-sim: " << spec.name << ": " << error << "\n"; return WorkerFailed; }
	Bytes state;
	std::string ignored;
	// M7 prerequisites: a streamed group checkpoint (members' streams behind
	// an index), or the in-memory one written before.
	const bool streamed = allPartial && !options.force && IsStreamedCheckpoint(dir, groupCheckpointName);
	const bool resume = streamed ||
		( allPartial && !options.force && ReadSidecarFile(dir, groupCheckpointName, options.budget, state, ignored) );
	if (command == "resume" && !resume) { err << "rise-sim: " << spec.name << ": no group checkpoint to resume from\n"; return WorkerFailed; }
	for (const auto& m : members)
		if (!group.Record(m, planOf(m)->spec->cacheDir, options.budget, resume, error)) {
			err << "rise-sim: " << spec.name << ": member " << m << ": " << error << "\n"; return WorkerFailed; }
	Bytes header;
	const auto restore = [&](std::string& e) {
		if (!streamed) return group.Restore(state, e);
		return ReadStreamedCheckpoint(dir, groupCheckpointName, options.budget, header,
			[&](ICheckpointSource& source, std::string& e2) { return group.RestoreFrom(source, e2); }, e);
	};
	if (resume ? !restore(error) : !group.Start(error)) {
		err << "rise-sim: " << spec.name << ": " << error << "\n"; return WorkerFailed; }
	out << "rise-sim: " << spec.name << ": " << (resume ? "resuming" : "baking") << " the coupled run of";
	for (const auto& m : members) out << " " << m << " (" << group.MemberFidelity(m) << ")";
	out << ", macro step " << group.MacroStep() << " s\n";
	const auto started = std::chrono::steady_clock::now();
	auto status = [&](const char* st, const std::string& why) {
		for (const auto& m : members) {
			WorkerStatus w;
			w.state = st;
			w.solver = group.Description(m)->solverId;
			w.simulatedTime = group.Time();
			w.endTime = group.EndTime();
			const auto snap = group.Snapshot(m);
			const auto outputs = group.Description(m)->cache.outputs.size();
			w.framesWritten = outputs ? snap->EntryCount()/outputs : 0;
			w.framesTotal = group.Description(m)->cache.count;
			w.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			w.lastError = why;
			w.detail = "coupled run " + spec.name;
			std::string e;
			WriteWorkerStatus(planOf(m)->spec->cacheDir, w, e);
			if (m == members.front() && dir != planOf(m)->spec->cacheDir) WriteWorkerStatus(dir, w, e);
		}
	};
	auto checkpoint = [&]() {
		std::string e;
		const Bytes groupHeader = {0xa0};	// an empty canonical CBOR map: the group stream carries its own index
		return WriteStreamedCheckpoint(dir, groupCheckpointName, groupHeader,
			[&](ICheckpointSink& sink, std::string& e2) { return group.CheckpointTo(sink, e2); }, e);
	};
	auto stopRequested = [&]() {
		std::error_code ec;
		if (fs::exists(dir/kWorkerStopFileName, ec)) return true;
		for (const auto& m : members) if (fs::exists(planOf(m)->spec->cacheDir/kWorkerStopFileName, ec)) return true;
		return false;
	};
	status("running", "");
	auto last = std::chrono::steady_clock::now();
	std::uint64_t steps = 0;
	while (!group.Finished()) {
		if (stopRequested() || (options.stopAfter && steps >= options.stopAfter)) {
			if (!checkpoint()) { status("failed", "cannot write the group checkpoint"); return WorkerFailed; }
			status("stopped", "");
			out << "rise-sim: " << spec.name << ": stopped at t = " << group.Time() << " s; `resume` continues\n";
			return WorkerStopped;
		}
		if (!group.StepMacro(error)) {
			status("failed", error);
			err << "rise-sim: " << spec.name << ": " << error << "\n";
			return WorkerFailed;
		}
		++steps;
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration<double>(now - last).count() >= options.checkpointSeconds) {
			last = now;
			checkpoint();
			status("running", "");
		}
	}
	if (!group.SealRecordings(error)) { status("failed", error); err << "rise-sim: " << spec.name << ": " << error << "\n"; return WorkerFailed; }
	status("finished", "");
	out << "rise-sim: " << spec.name << ": finished, every member sealed\n";
	return WorkerFinished;
}

} // namespace

bool IsSceneFilePath(const std::string& path)
{
	const std::string suffix = ".risescene";
	if (path.size() <= suffix.size()) return false;
	std::string tail = path.substr(path.size() - suffix.size());
	std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return tail == suffix;
}

int SceneWorkerMain(const std::string& command, const std::string& scene, const std::vector<std::string>& names,
	const WorkerOptions& options, std::ostream& out, std::ostream& err)
{
	if (command == "status" && (!names.empty() || options.all || options.force)) {
		err << "rise-sim: status <scene> takes no simulation names\n";
		return WorkerUsage;
	}
	if (command == "bake" && names.empty() && !options.all) {
		err << "rise-sim: bake <scene> needs simulation names or --all\n";
		return WorkerUsage;
	}
	JobHolder holder;
	if (!RISE_CreateJobPriv(&holder.job) || !holder.job) { err << "rise-sim: cannot create a job\n"; return WorkerFailed; }
	if (!holder.job->LoadAsciiSceneViaCst(scene.c_str())) {
		err << "rise-sim: the scene does not load: " << scene << " (see the log)\n";
		return WorkerFailed;
	}
	Job* job = dynamic_cast<Job*>(holder.job);
	if (!job) { err << "rise-sim: unexpected job type\n"; return WorkerFailed; }
	std::string error;
	if (command == "status") {
		std::vector<SceneSimulationStatus> statuses;
		if (!job->DescribeSimulations(statuses, error)) { err << "rise-sim: " << error << "\n"; return WorkerFailed; }
		if (statuses.empty()) out << "the scene declares no simulations\n";
		for (const auto& s : statuses) PrintStatus(s, out);
		return WorkerFinished;
	}
	std::vector<SceneSimulationPlan> plans;
	std::vector<SceneSimulationSpec> specs;
	if (!job->PlanSimulations(plans, specs, error)) { err << "rise-sim: " << error << "\n"; return WorkerFailed; }
	auto planOf = [&](const std::string& name) -> const SceneSimulationPlan* {
		for (const auto& p : plans) if (p.spec->name == name) return &p;
		return nullptr;
	};
	for (const auto& n : names)
		if (!planOf(n)) {
			err << "rise-sim: the scene declares no simulation named " << n << "; it declares:";
			for (const auto& p : plans) err << " " << p.spec->name;
			err << "\n";
			return WorkerUsage;
		}
	// The requested simulations, plus (bake) everything they depend on.
	std::set<std::string> wanted(names.begin(), names.end());
	if (names.empty()) for (const auto& p : plans) wanted.insert(p.spec->name);
	if (command == "bake" && !options.noUpstream) {
		for (auto it = plans.rbegin(); it != plans.rend(); ++it)
			if (wanted.count(it->spec->name)) for (const auto& u : it->upstream) wanted.insert(u);
	}
	// M5: --expect-key names simulations of this scene; each is checked when it
	// is planned for its bake, before its cache is touched.
	for (const auto& e : options.expectKeys)
		if (!planOf(e.first)) {
			err << "rise-sim: --expect-key names " << e.first << ", which the scene does not declare\n";
			return WorkerUsage;
		}
	// Plans are in dependency order: each upstream is final before its consumers.
	// The scene is planned again before each bake: a consumer's scene inputs
	// read its upstream's cache, which the previous bake may just have written.
	std::vector<std::string> order;
	for (const auto& p : plans) order.push_back(p.spec->name);
	std::set<std::string> coupledDone;   // M6: group runs (and their members) already baked
	for (const std::string& name : order) {
		if (!wanted.count(name) || coupledDone.count(name)) continue;
		if (!job->PlanSimulations(plans, specs, error)) { err << "rise-sim: " << error << "\n"; return WorkerFailed; }
		const SceneSimulationPlan* current = planOf(name);
		if (!current) { err << "rise-sim: simulation " << name << " vanished from the plan\n"; return WorkerFailed; }
		const SceneSimulationPlan& plan = *current;
		const SceneSimulationSpec& spec = *plan.spec;
		SceneSimulationStatus state;
		InspectSceneSimulation(plan, state);
		// M6: a group (or a member of one) bakes as one coupled run; so does a port_source simulation.
		const SceneSimulationSpec* group = spec.keyword == kSimulationGroupKeyword ? &spec : SceneGroupOf(specs, name);
		if (group || !spec.portSources.empty()) {
			const SceneSimulationSpec& run = group ? *group : spec;
			coupledDone.insert(run.name);
			for (const auto& m : SceneGroupLiveMembers(run)) coupledDone.insert(m);
			// Upstreams outside the run must be complete.
			for (const auto& p : plans) {
				if (!coupledDone.count(p.spec->name)) continue;
				for (const auto& u : p.upstream) {
					if (coupledDone.count(u)) continue;
					SceneSimulationStatus up;
					InspectSceneSimulation(*planOf(u), up);
					if (up.state != SceneSimulationState::Complete) {
						err << "rise-sim: " << run.name << " reads simulation " << u << ", which is " <<
							SceneSimulationStateName(up.state) << "; bake it first\n";
						return WorkerFailed;
					}
				}
			}
			const int code = BakeCoupled(run, plans, command, options, out, err);
			if (code != WorkerFinished) return code;
			continue;
		}
		if (spec.solverId.empty()) {
			out << "rise-sim: " << spec.name << ": a " << spec.keyword << " has no solver; " <<
				SceneSimulationStateName(state.state) << ", not baked\n";
			continue;
		}
		for (const auto& u : plan.upstream) {
			SceneSimulationStatus up;
			InspectSceneSimulation(*planOf(u), up);
			if (up.state != SceneSimulationState::Complete) {
				err << "rise-sim: " << spec.name << " reads simulation " << u << ", which is " <<
					SceneSimulationStateName(up.state) << "; bake it first (rise-sim bake <scene> " << u << ")\n";
				return WorkerFailed;
			}
		}
		if (!plan.configured && !plan.keyUnknown) {
			err << "rise-sim: " << spec.name << ": " << plan.error << "\n";
			return WorkerFailed;
		}
		if (!plan.inputs) { err << "rise-sim: " << spec.name << ": no scene inputs\n"; return WorkerFailed; }
		// M5: the document that asked for this bake planned it with a key; the
		// file on disk must plan the same one, or the bake would not be what the
		// document shows (unsaved edits, or the file changed since it loaded).
		for (const auto& e : options.expectKeys) {
			if (e.first != name) continue;
			const std::string onDisk = plan.configured ? plan.description.cache.key : std::string();
			if (onDisk != e.second) {
				err << "rise-sim: " << kWorkerSceneMismatch << ": " << name << ": the scene file " << scene <<
					" plans key " << (onDisk.empty() ? std::string("(unknown)") : Short(onDisk)) <<
					" but the loaded document expects " << Short(e.second) <<
					"; save the scene first (the bake runs from the file on disk)\n";
				return WorkerFailed;
			}
		}
		const auto& caps = plan.description.capabilities;
		out << "rise-sim: " << spec.name << " (" << spec.solverId << (caps.backend.empty() ? "" : ", " + caps.backend) <<
			"): " << SceneSimulationStateName(state.state) << ", cache " << spec.cacheDir.string() << "\n";
		int code = WorkerFinished;
		if (command == "resume") {
			if (state.state != SceneSimulationState::Partial && state.state != SceneSimulationState::Failed) {
				out << "rise-sim: " << spec.name << ": nothing to resume\n";
				continue;
			}
			code = WorkerResumeConfiguration(spec.cacheDir, *plan.inputs, options, out, err);
		} else {
			if (state.state == SceneSimulationState::Complete && !options.force) {
				out << "rise-sim: " << spec.name << ": up to date\n";
				continue;
			}
			V parameters;
			if (!SceneSimulationParameters(spec, parameters, error)) { err << "rise-sim: " << spec.name << ": " << error << "\n"; return WorkerFailed; }
			code = WorkerBakeConfiguration(spec.solverId, parameters, *plan.inputs, spec.cacheDir, options,
				/*replace=*/true, out, err);
		}
		if (code != WorkerFinished) return code;
	}
	return WorkerFinished;
}

} }
