#include "pch.h"
#include "SceneSimulationWorker.h"
#include "SceneSimulation.h"
#include "../Job.h"
#include "../Utilities/Reference.h"

#include <algorithm>
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
	if (command == "bake") {
		for (auto it = plans.rbegin(); it != plans.rend(); ++it)
			if (wanted.count(it->spec->name)) for (const auto& u : it->upstream) wanted.insert(u);
	}
	// Plans are in dependency order: each upstream is final before its consumers.
	// The scene is planned again before each bake: a consumer's scene inputs
	// read its upstream's cache, which the previous bake may just have written.
	std::vector<std::string> order;
	for (const auto& p : plans) order.push_back(p.spec->name);
	for (const std::string& name : order) {
		if (!wanted.count(name)) continue;
		if (!job->PlanSimulations(plans, specs, error)) { err << "rise-sim: " << error << "\n"; return WorkerFailed; }
		const SceneSimulationPlan* current = planOf(name);
		if (!current) { err << "rise-sim: simulation " << name << " vanished from the plan\n"; return WorkerFailed; }
		const SceneSimulationPlan& plan = *current;
		const SceneSimulationSpec& spec = *plan.spec;
		SceneSimulationStatus state;
		InspectSceneSimulation(plan, state);
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
