// Simulation M4: simulations declared by a scene -- the dependency graph,
// the cache key of each simulation against the scene, and its state against
// its cache on disk (docs/SIMULATION_SOLVER_CONTRACT.md).
//
// A simulation's cache key is MakeCacheKey(solver, version, effective
// parameters, [(input, Identity(input))...]) exactly as every host computes
// it (PrepareCacheDescription); the scene's part is the identities of the
// scene elements the simulation reads.  An element whose content is the
// output of another simulation (an object whose geometry is a
// simulated_mesh_geometry bound with `source`) has an identity that embeds
// that simulation's expected key, so a change upstream changes every key
// downstream: staleness propagates through the keys themselves.
//
// Nothing here runs a solver beyond Configure, and nothing here writes a
// cache.  Nothing here is solver-specific.
#ifndef RISE_SIMULATION_SCENE_SIMULATION_H
#define RISE_SIMULATION_SCENE_SIMULATION_H

#include "SimulationSolver.h"
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

//! One producer chunk, resolved against the scene file.
struct SceneSimulationSpec {
	std::string keyword, name;
	std::string solverId;                  // empty: simulation_cache (a foreign producer, no solver)
	RISECBOR64::Value parameters;          // the solver parameter map (as authored, before Configure)
	std::filesystem::path cacheDir;        // absolute
	std::vector<std::string> sceneInputs;  // scene elements the parameters read
	std::vector<std::pair<std::string, std::string>> outputs; // declared (name, codec); empty for simulation_cache
	//! M6: one-way couplings declared on this simulation, as authored:
	//! "<accepted port> from <simulation>.<output> [via <adapter> [key=value ...]] [aux <member>.<port>]".
	std::vector<std::string> portSources;
};
//! M6: the keyword of a co-stepping group (solverId empty, parameters: the
//! group's chunk fields as a map, cacheDir: the group's own sidecar directory).
extern const char* const kSimulationGroupKeyword;   // "simulation_group"

enum class SceneSimulationState {
	Missing,   // no cache
	Baking,    // a writer holds the cache lock
	Partial,   // current key, not sealed
	Complete,  // current key, sealed
	Stale,     // the cache holds another configuration (its key differs); it still plays, marked
	Failed     // configuration refused, cache corrupt, or the last bake failed and left no complete cache
};
const char* SceneSimulationStateName(SceneSimulationState state);

struct SceneSimulationStatus {
	std::string name, keyword, solverId;
	SceneSimulationState state = SceneSimulationState::Missing;
	std::string expectedKey;               // the key the scene asks for (empty when it cannot be computed)
	std::string cacheKey;                  // the key on disk (empty when missing)
	std::filesystem::path cacheDir;
	std::vector<std::string> upstream;     // simulations this one's inputs depend on
	std::vector<std::string> inputs;       // scene elements it reads
	//! Some upstream simulation is not complete with its current key, so a
	//! bake of this one would read out-of-date or missing data.
	bool upstreamOutOfDate = false;
	bool interactive = false;              // capabilities.interactive of this configuration
	std::string backend;                   // capabilities.backend ("" = not stated)
	std::uint64_t memoryEstimate = 0;      // M5: capabilities.memoryEstimate (peak bytes; 0 = unknown)
	double costEstimate = 0;               // M5: capabilities.costEstimate (s per simulated s; 0 = unknown)
	std::string fidelity;                  // of the cache on disk ("" when missing)
	std::uint64_t framesAvailable = 0, framesTotal = 0;
	double startTime = 0, interval = 0;    // the configuration's time support
	std::string error;                     // why Failed (or a configuration problem)
	//! M5: the simulated_mesh_geometry elements bound to it by `source` (by
	//! name; a live run drives the first), filled in by Job::DescribeSimulations;
	//! for a transform producer (rigid.bodies) the objects that follow its bodies.
	std::vector<std::string> meshConsumers;
	//! M5c: why a Stale simulation is stale, from the cache's job record
	//! against this configuration: "parameters changed (<members>)" naming the
	//! effective-parameter members that differ (a moved or resized collider
	//! shows as cloth's `colliders`), "scene input changed
	//! (<inputs>)", "upstream out of date", "solver version changed",
	//! "configuration changed" (no job record to compare), for a group member
	//! "member <other>: ..." or "coupling changed (...)", or the prediction
	//! error when the key is unknown.  Empty unless state is Stale.
	std::string staleReason;
	//! M5c: estimated wall time of a whole bake from the start, seconds:
	//! costEstimate x the simulated span (startTime + interval x (framesTotal-1)).
	//! 0 = unknown.  Approximate by construction; see the solver's estimate.
	double wallEstimate = 0;
	//! GUI status pass: what stands behind the label.  `evidenceScope` is the
	//! sentence the cache's fidelity label covers (recorded in its manifest;
	//! "" when the cache has none or there is no cache) and `evidenceCases` the
	//! number of validation digests bound to it.  `fidelityLimit` is why this
	//! configuration cannot earn `predictive` (the solver's own statement; ""
	//! when it can, or when the solver is not configured here).  A group
	//! reports the member that limits its label.
	std::string evidenceScope;
	std::uint32_t evidenceCases = 0;
	std::string fidelityLimit;
	//! GUI status pass: the couplings declared on this simulation -- a
	//! group's `connect` lines and ownership (`owned a b`), and any
	//! `port_source` lines -- as authored.  Empty for an uncoupled simulation.
	std::vector<std::string> connections;
};

//! M5c: "≈ 5 h, 4.0 GB" (either part may be missing; "" when both are
//! unknown).  What the simulations panel and `sim_list` show before a bake.
std::string FormatSimulationEstimate(const SceneSimulationStatus& status);

//! What the scene supplies: scene inputs for a simulation.  Implemented by
//! the Job (SceneInputs.h).
class ISceneInputProvider {
public:
	virtual ~ISceneInputProvider() = default;
	//! The simulations whose outputs the named scene element's content
	//! depends on (empty for an ordinary element).
	virtual bool InputDependencies(const std::string& input, std::vector<std::string>& simulations,
		std::string& error) const = 0;
	//! A detached view of the named elements: after it returns it never reads
	//! the live scene again, so a solver may sample it from any thread while
	//! the scene is edited or rendered.  `upstreamKeys` holds the expected
	//! key of every simulation an input depends on.
	virtual std::shared_ptr<const ISceneInputs> Capture(const std::vector<std::string>& inputs,
		const std::map<std::string, std::string>& upstreamKeys, std::string& error) const = 0;
};

//! One simulation configured against the scene.
struct SceneSimulationPlan {
	const SceneSimulationSpec* spec = nullptr;
	std::vector<std::string> upstream;
	std::shared_ptr<const ISceneInputs> inputs;  // null for simulation_cache
	SolverDescription description;               // after PrepareCacheDescription (parent "")
	bool configured = false;
	//! An upstream simulation is not complete with its current key, so this
	//! one's scene inputs (which read the upstream cache) cannot be sampled
	//! and its key is unknown: it is missing (no cache) or stale (a cache
	//! baked against other upstream data) until the upstream is baked.
	bool waitingForUpstream = false;
	//! The solver is keyed by prediction (no in-process Configure) and the
	//! prediction needs a value only a bake derives: the key is unknown until
	//! the first bake.  Not a refusal: the worker configures and bakes it.
	bool keyUnknown = false;
	std::string error;
};

//! The graph.  Thread-safe; it memoizes the description of each
//! (solver, parameters, input identities), so re-planning an unchanged scene
//! configures nothing.
class SceneSimulationGraph {
public:
	//! Plans every simulation in dependency order (upstream first).  False
	//! only for a structural error (a dependency cycle, an unknown upstream);
	//! a simulation that does not configure gets a plan with its error.
	bool Plan(const std::vector<SceneSimulationSpec>& specs, const ISceneInputProvider& provider,
		std::vector<SceneSimulationPlan>& plans, std::string& error);
	//! Plan, then inspect every cache on disk, with propagation.
	bool Describe(const std::vector<SceneSimulationSpec>& specs, const ISceneInputProvider& provider,
		std::vector<SceneSimulationStatus>& statuses, std::string& error);
	//! The configured descriptions remembered (for tests: how many Configure calls were avoided).
	std::size_t MemoSize() const;
	std::uint64_t ConfigureCount() const;
	void ClearMemo();
private:
	bool Configure(const SceneSimulationSpec& spec, const ISceneInputs& inputs, SolverDescription& d,
		bool& keyUnknown, std::string& error);
	mutable std::mutex mutex_;
	std::map<std::string, SolverDescription> memo_;   // by solver | parameters | identities
	std::uint64_t configures_ = 0;
};

//! The state of one planned simulation against its cache directory.
void InspectSceneSimulation(const SceneSimulationPlan& plan, SceneSimulationStatus& status);

//! The parameters a scene passes to its solver's Configure: the authored map,
//! completed with the schema's explicit defaults when the solver's schema
//! asks for that (SimulationParameterSchema.h: fire).
bool SceneSimulationParameters(const SceneSimulationSpec& spec, RISECBOR64::Value& parameters, std::string& error);

class SimulationGroup;
//! M6: the coupled run a scene declares for `spec` -- a simulation_group, or
//! a simulation with port_source lines (its upstreams then stand in as cached
//! members) -- built over the plans (Configure done, nothing started).
//! Members whose plans are not configured, or upstreams not complete, refuse.
bool BuildSceneGroup(const SceneSimulationSpec& spec, const std::vector<SceneSimulationPlan>& plans,
	SimulationGroup& group, std::string& error);
//! M6: the simulations a coupled spec runs live (a group's members; the
//! downstream of port_source lines), and the group spec (if any) a simulation belongs to.
std::vector<std::string> SceneGroupLiveMembers(const SceneSimulationSpec& spec);
const SceneSimulationSpec* SceneGroupOf(const std::vector<SceneSimulationSpec>& specs, const std::string& simulation);

} }
#endif
