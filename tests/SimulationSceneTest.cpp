// Simulation M4: simulations as scene elements (docs/SIMULATION_SOLVER_CONTRACT.md).
//
//   P  non-render pose-holder capture: exact Object chains accepted, custom/CSG nodes refused before cloning
//      (`--pose-holder-only` runs only this section)
//   A  the cloth parameter schema's defaults are what cloth.shell's Configure applies
//   B  cloth_simulation parses, describes (missing), and its consumer is an unbound, hidden mesh
//   C  `rise-sim bake <scene>` equals `rise-sim bake cloth.shell <params>` byte for byte
//   D  missing / partial / stale load behaviour; resume from the scene; predictive fails closed
//   E  the cache key changes with every keyed parameter, not with pace/threads
//   F  scene inputs: the key follows a collider object's geometry and animation, not camera,
//      lights or materials; a scene-input bake with an animated collider equals the same motion
//      given parametrically
//   G  a two-simulation chain: staleness propagates downstream; a bake of the downstream bakes
//      the upstream first
//   H  ApplyCstParamEdit on a producer updates the state and starts no bake
//   I  reference legality: unknown simulation, non-mesh output
//   L  editor and agent: the sample scenes round-trip through the CST (save -> load -> the same simulations),
//      the agent schema and the descriptor registry list the producers under category `simulation`
//   J  fire_simulation: parsing, parameter mapping, completion, key prediction and schema validation
//      (no fire solver is created: the scene keys fire without Configure)
#include "../src/Library/Simulation/SimulationWorker.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SceneSimulation.h"
#include "../src/Library/Simulation/SceneInputs.h"
#include "../src/Library/Simulation/SimulationProcess.h"
#include "../src/Library/Simulation/SimulationParameterSchema.h"
#include "../src/Library/Simulation/Cloth/ClothParameterSchema.h"
#include "../src/Library/Simulation/Fire/FireParameterSchema.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Utilities/FireProductionCompute.h"
#include "../src/Library/Geometry/SimulatedMeshGeometry.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Interfaces/ILogPrinter.h"
#include "../src/Library/Job.h"
#include "../src/Library/Agent/SchemaGen.h"
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Interfaces/IAnimator.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "TestTempDir.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace {
using namespace RISE;
using namespace RISE::Simulation;
namespace fs = std::filesystem;
using V = RISECBOR64::Value;
constexpr std::size_t budget = std::size_t(64) << 20;
int failures = 0, passes = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (condition) { ++passes; return; }
	++failures;
	std::cerr << "FAIL: " << what << (detail.empty() ? "" : " -- " + detail) << "\n";
}

int Worker(const std::vector<std::string>& args, std::string* output = nullptr)
{
	std::ostringstream out, err;
	const int code = WorkerMain(args, out, err);
	if (output) *output = out.str() + err.str();
	return code;
}

// The per-user default cache directory of simulation `name` of the scene file `scene`
// (SimulationProcess.h): where a chunk without `cache` bakes.
fs::path DefaultCache(const fs::path& scene, const std::string& name)
{
	std::string error;
	return DefaultSimulationCachePath(scene, name, error);
}

void WriteText(const fs::path& path, const std::string& text)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f << text;
}

RISECBOR64::Bytes ReadAll(const fs::path& path)
{
	std::ifstream f(path, std::ios::binary);
	return RISECBOR64::Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::map<std::string, RISECBOR64::Bytes> Sealed(const fs::path& dir)
{
	std::map<std::string, RISECBOR64::Bytes> files;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(dir, ec)) {
		const auto name = e.path().filename().string();
		if (name == "manifest.cbor" || e.path().extension() == ".frame" || e.path().extension() == ".static")
			files[name] = ReadAll(e.path());
	}
	return files;
}

std::string Encoded(const V& v)
{
	RISECBOR64::Bytes b;
	RISECBOR64::Encode(v, b, nullptr);
	return std::string(b.begin(), b.end());
}

struct Scene {
	IJobPriv* job = nullptr;
	Scene() = default;
	Scene(const Scene&) = delete;
	Scene& operator=(const Scene&) = delete;
	~Scene() { if (job) job->release(); }
	Job* J() const { return dynamic_cast<Job*>(job); }
};

bool Load(const fs::path& path, Scene& scene)
{
	if (!RISE_CreateJobPriv(&scene.job) || !scene.job) return false;
	return scene.job->LoadAsciiSceneViaCst(path.string().c_str());
}

std::vector<SceneSimulationStatus> Statuses(Scene& scene)
{
	std::vector<SceneSimulationStatus> s;
	std::string error;
	if (!scene.J() || !scene.J()->DescribeSimulations(s, error)) Check(false, "DescribeSimulations", error);
	return s;
}

const SceneSimulationStatus* Find(const std::vector<SceneSimulationStatus>& all, const std::string& name)
{
	for (const auto& s : all) if (s.name == name) return &s;
	return nullptr;
}

Implementation::SimulatedMeshGeometry* Mesh(Scene& scene, const char* name)
{
	IGeometryManager* g = scene.job ? scene.job->GetGeometries() : nullptr;
	return g ? dynamic_cast<Implementation::SimulatedMeshGeometry*>(g->GetItem(name)) : nullptr;
}

// ---------------------------------------------------------------- scenes

const char* const Header = "RISE ASCII SCENE 7\n";

std::string Camera(const std::string& location = "0 1 3")
{
	return "pinhole_camera\n{\n\tlocation\t" + location + "\n\tlookat\t0 0.5 0\n\tup\t0 1 0\n\tfov\t40\n}\n\n";
}

// A 9x9 sheet pinned at two corners, 6 frames.
std::string Cloth(const std::string& name, const std::string& extra, const std::string& origin = "-0.5 1 0.5")
{
	return "cloth_simulation\n{\n\tname\t" + name + "\n\tvertices_u\t9\n\tvertices_v\t9\n\tframes\t6\n\tsubsteps\t4\n"
		"\torigin\t" + origin + "\n\tpin\tu0v0\n\tpin\tu1v0\n" + extra + "}\n\n";
}

std::string Consumer(const std::string& geometry, const std::string& source, const std::string& extra = "")
{
	return "simulated_mesh_geometry\n{\n\tname\t" + geometry + "\n\tsource\t" + source + "\n\tdouble_sided\tTRUE\n" +
		extra + "}\n\nstandard_object\n{\n\tname\tobj_" + geometry + "\n\tgeometry\t" + geometry + "\n}\n\n";
}

std::string Ball(const std::string& radius = "0.3", const std::string& position = "0 0.3 0",
	const std::string& material = "")
{
	return "sphere_geometry\n{\n\tname\tgeo_ball\n\tradius\t" + radius + "\n}\n\nstandard_object\n{\n\tname\tball\n"
		"\tgeometry\tgeo_ball\n\tposition\t" + position + "\n" + material + "}\n\n";
}

// ---------------------------------------------------------------- A

void TestSchemaDefaults()
{
	std::printf("A: schema defaults vs Configure\n");
	const ParameterSchema& schema = ClothParameterSchema();
	for (const char* mode : {"bake", "interactive"}) {
		std::string error;
		auto solver = CreateSolver("cloth.shell", error);
		SolverDescription d;
		const bool ok = solver && solver->Configure(V::MapValue({{"mode", V::String(mode)}, {"vertices_u", V::Unsigned(5)},
			{"vertices_v", V::Unsigned(5)}}), EmptySceneInputs(), d, error);
		Check(ok, std::string("cloth configures in ") + mode + " mode", error);
		if (!ok) continue;
		for (const auto& p : schema.parameters) {
			if (!p.keyed || p.defaultText.empty() || p.name == "vertices_u" || p.name == "vertices_v" || p.name == "mode") {
				if (!p.keyed) Check(!d.effectiveParameters.Find(p.name), "unkeyed " + p.name + " is not an effective parameter");
				continue;
			}
			std::string text = p.defaultText;
			const std::string tag = "interactive: ";
			if (std::string(mode) == "interactive" && p.modeDefaults.rfind(tag, 0) == 0) text = p.modeDefaults.substr(tag.size());
			V expected;
			Check(SchemaValueFromText(p, text, expected, error), "schema default of " + p.name + " parses", error);
			const V* actual = d.effectiveParameters.Find(p.name);
			Check(actual && Encoded(*actual) == Encoded(expected),
				std::string(mode) + ": schema default of " + p.name + " (" + text + ") is the solver's");
		}
		// Every effective parameter the solver writes is in the schema (or a chunk field).
		for (const auto& m : d.effectiveParameters.GetMap()) {
			const bool known = FindSchemaParameter(schema, m.first) || m.first == "material" || m.first == "pins" ||
				m.first == "colliders";
			Check(known, "effective parameter " + m.first + " is described by the schema");
		}
	}
	// Enum values are the solver's: each configures, a bogus one does not.
	for (const auto& p : schema.parameters) {
		if (p.type != SchemaType::Enum) continue;
		for (const auto& v : p.values) {
			std::string error;
			auto solver = CreateSolver("cloth.shell", error);
			SolverDescription d;
			V::Members m{{"vertices_u", V::Unsigned(5)}, {"vertices_v", V::Unsigned(5)}, {p.name, V::String(v)}};
			if (p.name == "integrator" && v != "backward_euler") m.emplace_back("mode", V::String("bake"));
			Check(solver->Configure(V::MapValue(m), EmptySceneInputs(), d, error), p.name + " " + v + " configures", error);
		}
		std::string error;
		auto solver = CreateSolver("cloth.shell", error);
		SolverDescription d;
		Check(!solver->Configure(V::MapValue({{p.name, V::String("bogus")}}), EmptySceneInputs(), d, error),
			p.name + " bogus is refused");
	}
	// Ranges: the schema refuses out-of-range text before the solver sees it.
	{
		const SchemaParameter* p = FindSchemaParameter(schema, "vertices_u");
		V v;
		std::string error;
		Check(p && !SchemaValueFromText(*p, "1", v, error) && SchemaValueFromText(*p, "2", v, error), "vertices_u range");
	}
	Check(FindSolverSchema("cloth.shell") == &schema, "the cloth schema is registered beside the factory");
}

// ---------------------------------------------------------------- B, C, D

void TestBakeEquivalence(const fs::path& root)
{
	std::printf("B/C/D: describe, bake equivalence, missing / partial / stale\n");
	const fs::path dir = root/"equivalence";
	fs::create_directories(dir);
	const fs::path scenePath = dir/"flag.RISEscene";
	WriteText(scenePath, std::string(Header) + Camera() + Cloth("flag", "") + Consumer("geo_flag", "flag.surface"));
	{
		Scene scene;
		Check(Load(scenePath, scene), "a scene with a cloth_simulation and no cache loads");
		const auto all = Statuses(scene);
		Check(all.size() == 1 && all[0].name == "flag" && all[0].state == SceneSimulationState::Missing &&
			!all[0].expectedKey.empty() && all[0].framesTotal == 6 && all[0].interactive && all[0].backend == "cpu" &&
			all[0].cacheDir == DefaultCache(scenePath, "flag"),
			"describe: missing, keyed, 6 frames, default cache in the per-user folder",
			all.empty() ? "" : SceneSimulationStateName(all[0].state) + std::string(" ") + all[0].cacheDir.string());
		auto* mesh = Mesh(scene, "geo_flag");
		Check(mesh && !mesh->HasSource() && mesh->Hidden() && !mesh->TopologyBuilt(), "missing cache: the consumer is unbound and hidden");
		std::vector<Simulation::TimelineRange> ranges;
		scene.job->DescribeTimeVaryingAssets(ranges, true);
		Check(ranges.empty(), "an unbound mesh has no timeline range");
	}
	// C: rise-sim bake <scene> == rise-sim bake cloth.shell <params>.
	std::string log;
	Check(Worker({"bake", scenePath.string(), "flag"}, &log) == WorkerFinished, "rise-sim bake <scene> flag", log);
	WriteText(dir/"flag.json", "{\"vertices_u\": 9, \"vertices_v\": 9, \"frames\": 6, \"substeps\": 4, "
		"\"origin\": [-0.5, 1, 0.5], \"pins\": [{\"select\": \"u0v0\"}, {\"select\": \"u1v0\"}]}");
	Check(Worker({"bake", "cloth.shell", (dir/"flag.json").string(), (dir/"plain").string()}, &log) == WorkerFinished,
		"rise-sim bake cloth.shell <params>", log);
	{
		const auto a = Sealed(DefaultCache(scenePath, "flag")), b = Sealed(dir/"plain");
		Check(a.size() > 6 && a == b, "scene bake equals the parametric bake byte for byte (manifest, frames, statics)");
		CacheSnapshot sa, sb;
		std::string error;
		Check(CacheSnapshot::Open(DefaultCache(scenePath, "flag"), budget, sa, error) && CacheSnapshot::Open(dir/"plain", budget, sb, error) &&
			sa.Description().key == sb.Description().key && sa.Description().epoch == sb.Description().epoch,
			"same key and epoch", error);
	}
	Check(Worker({"bake", scenePath.string(), "flag"}, &log) == WorkerFinished && log.find("up to date") != std::string::npos,
		"a second scene bake skips the up-to-date simulation", log);
	Check(Worker({"status", scenePath.string()}, &log) == WorkerFinished && log.find("flag: complete") != std::string::npos,
		"rise-sim status <scene>", log);
	{
		Scene scene;
		Check(Load(scenePath, scene), "the baked scene loads");
		const auto all = Statuses(scene);
		Check(all.size() == 1 && all[0].state == SceneSimulationState::Complete && all[0].framesAvailable == 6 &&
			all[0].cacheKey == all[0].expectedKey, "describe: complete with the current key");
		auto* mesh = Mesh(scene, "geo_flag");
		Check(mesh && mesh->HasSource() && mesh->Source().VertexCount() == 81, "the consumer binds the producer's cache");
	}
	// Stale: another gravity; the old cache still loads and plays, marked.
	const fs::path stalePath = dir/"flag_stale.RISEscene";
	WriteText(stalePath, std::string(Header) + Camera() + Cloth("flag", "\tgravity\t0 -5 0\n\tcache\t" + DefaultCache(scenePath, "flag").string() + "\n") +
		Consumer("geo_flag", "flag.surface"));
	{
		Scene scene;
		Check(Load(stalePath, scene), "a scene whose cache is stale loads");
		const auto all = Statuses(scene);
		Check(all.size() == 1 && all[0].state == SceneSimulationState::Stale && all[0].cacheKey != all[0].expectedKey,
			"describe: stale");
		auto* mesh = Mesh(scene, "geo_flag");
		Check(mesh && mesh->HasSource(), "a stale cache still binds and plays");
	}
	// Partial, then resume from the scene.
	const fs::path partialPath = dir/"flag_partial.RISEscene";
	WriteText(partialPath, std::string(Header) + Camera() + Cloth("flag", "\tcache\tcaches/partial\n") +
		Consumer("geo_flag", "flag.surface"));
	Check(Worker({"bake", partialPath.string(), "flag", "--stop-after-frames", "2"}, &log) == WorkerStopped,
		"a scene bake stops after two frames", log);
	{
		Scene scene;
		Check(Load(partialPath, scene), "a scene whose cache is partial loads");
		const auto all = Statuses(scene);
		Check(all.size() == 1 && all[0].state == SceneSimulationState::Partial && all[0].framesAvailable >= 2 &&
			all[0].framesAvailable < 6, "describe: partial");
		auto* mesh = Mesh(scene, "geo_flag");
		Check(mesh && mesh->HasSource(), "a partial cache binds");
	}
	Check(Worker({"resume", partialPath.string(), "flag"}, &log) == WorkerFinished, "rise-sim resume <scene> flag", log);
	Check(Sealed(dir/"caches"/"partial") == Sealed(DefaultCache(scenePath, "flag")), "stop + resume from the scene equals the uninterrupted bake");
	// Predictive consumer of a missing cache fails closed; of a preview cache too.
	const fs::path predictivePath = dir/"flag_predictive.RISEscene";
	WriteText(predictivePath, std::string(Header) + Camera() + Cloth("flag", "\tcache\tcaches/none\n") +
		Consumer("geo_flag", "flag.surface", "\tfidelity\tpredictive\n"));
	{
		Scene scene;
		Check(!Load(predictivePath, scene), "a predictive consumer of a missing cache fails the load");
	}
	// The worker refuses unknown names and a bake without names.
	Check(Worker({"bake", scenePath.string(), "nonesuch"}, &log) == WorkerUsage, "unknown simulation name", log);
	Check(Worker({"bake", scenePath.string()}, &log) == WorkerUsage, "bake <scene> needs names or --all", log);
}

// ---------------------------------------------------------------- E

class NoInputs final : public ISceneInputProvider {
public:
	bool InputDependencies(const std::string&, std::vector<std::string>& s, std::string&) const override { s.clear(); return true; }
	std::shared_ptr<const ISceneInputs> Capture(const std::vector<std::string>&, const std::map<std::string, std::string>&,
		std::string&) const override { return std::make_shared<NoSceneInputs>(); }
};

std::string KeyOf(SceneSimulationGraph& graph, const V::Members& parameters)
{
	SceneSimulationSpec spec;
	spec.keyword = "cloth_simulation";
	spec.name = "k";
	spec.solverId = "cloth.shell";
	spec.parameters = V::MapValue(parameters);
	spec.cacheDir = "/nonexistent/rise-m4-key";
	std::vector<SceneSimulationPlan> plans;
	std::string error;
	NoInputs provider;
	const std::vector<SceneSimulationSpec> specs{spec};
	if (!graph.Plan(specs, provider, plans, error) || plans.size() != 1 || !plans[0].configured) {
		Check(false, "key computation configures", error + (plans.empty() ? "" : plans[0].error));
		return std::string();
	}
	return plans[0].description.cache.key;
}

void TestKeySensitivity()
{
	std::printf("E: the key changes with every keyed parameter\n");
	SceneSimulationGraph graph;
	const V::Members base{{"vertices_u", V::Unsigned(5)}, {"vertices_v", V::Unsigned(5)}, {"frames", V::Unsigned(3)}};
	const std::string key = KeyOf(graph, base);
	Check(!key.empty() && KeyOf(graph, base) == key, "the key is reproducible");
	const std::uint64_t configures = graph.ConfigureCount();
	KeyOf(graph, base);
	Check(graph.ConfigureCount() == configures, "an unchanged configuration is not configured again (memo)");
	// One different, valid value per keyed parameter (schema text form).
	const std::map<std::string, std::string> changed = {
		{"mode", "interactive"}, {"integrator", "backward_euler"}, {"size_u", "1.5"}, {"size_v", "0.5"},
		{"vertices_u", "6"}, {"vertices_v", "7"}, {"triangulation", "consistent"}, {"bending_model", "hinges"},
		{"origin", "0 1 0"}, {"axis_u", "1 0 0.1"}, {"axis_v", "0 0.1 -1"}, {"stretch_u", "1.1"}, {"stretch_v", "0.9"},
		{"frames", "4"}, {"frame_rate", "24"}, {"substeps", "3"}, {"gravity", "0 -5 0"}, {"contact", "penalty"},
		{"contact_stiffness", "2e5"}, {"contact_thickness", "0.003"}, {"barrier_stiffness", "2e5"},
		{"contact_activation", "0.002"}, {"friction", "0.4"}, {"friction_velocity", "0.002"},
		{"self_collision", "TRUE"}, {"self_friction", "0.4"}, {"pressure", "1"}, {"drag_coefficient", "0.5"}, {"air_density", "1.0"},
		{"damping", "FALSE"}, {"newton_tolerance", "0.002"}, {"newton_iterations", "20"}, {"cg_tolerance", "1e-5"},
		{"cg_iterations", "1000"}, {"linear_solver", "pcg"}};
	const ParameterSchema& schema = ClothParameterSchema();
	std::size_t tested = 0;
	for (const auto& p : schema.parameters) {
		const auto c = changed.find(p.name);
		if (!p.keyed) {
			V v;
			std::string error;
			Check(SchemaValueFromText(p, p.name == "threads" ? "2" : "5", v, error), "unkeyed value parses", error);
			V::Members m = base;
			m.emplace_back(p.name, v);
			Check(KeyOf(graph, m) == key, "unkeyed " + p.name + " leaves the key unchanged");
			continue;
		}
		Check(c != changed.end(), "a test value for keyed parameter " + p.name);
		if (c == changed.end()) continue;
		V v;
		std::string error;
		Check(SchemaValueFromText(p, c->second, v, error), p.name + " test value parses", error);
		V::Members m;
		for (const auto& b : base) if (b.first != p.name) m.push_back(b);
		m.emplace_back(p.name, v);
		const std::string k = KeyOf(graph, m);
		Check(!k.empty() && k != key, "changing " + p.name + " changes the key");
		++tested;
	}
	// Chunk fields: material, pins, colliders.
	for (const auto& extra : std::vector<std::pair<std::string, V>>{
		{"material", V::String("denim-twill")},
		{"pins", V::ArrayValue({V::MapValue({{"select", V::String("u0v0")}})})},
		{"colliders", V::ArrayValue({V::MapValue({{"type", V::String("plane")}, {"point", V::ArrayValue({V::Float(0), V::Float(0), V::Float(0)})}})})}}) {
		V::Members m = base;
		m.push_back(extra);
		Check(KeyOf(graph, m) != key, "changing " + extra.first + " changes the key");
	}
	Check(tested == changed.size(), "every keyed schema parameter was varied");
}

// ---------------------------------------------------------------- F

std::string ColliderScene(const std::string& ball, const std::string& extra = "", const std::string& camera = "0 1 3")
{
	return std::string(Header) + Camera(camera) + ball + extra +
		Cloth("drape", "\tcollider_object\tball\n\tcache\tcaches/drape\n", "-0.5 0.62 0.5") + Consumer("geo_drape", "drape.surface");
}

std::string KeyOfScene(const fs::path& path, const std::string& text, const char* name = "drape")
{
	WriteText(path, text);
	Scene scene;
	if (!Load(path, scene)) { Check(false, "scene loads: " + path.filename().string()); return std::string(); }
	const auto all = Statuses(scene);
	const SceneSimulationStatus* s = Find(all, name);
	if (!s || s->expectedKey.empty()) { Check(false, "scene key: " + path.filename().string(), s ? s->error : ""); return std::string(); }
	return s->expectedKey;
}

void TestSceneInputs(const fs::path& root)
{
	std::printf("F: scene inputs\n");
	const fs::path dir = root/"inputs";
	fs::create_directories(dir);
	const std::string timeline = "timeline\n{\n\telement_type\tobject\n\telement\tball\n\tparam\tposition\n\n"
		"\ttime\t0.0\n\tvalue\t0 0.3 0\n\n\ttime\t1.0\n\tvalue\t0.6 0.3 0\n}\n\n";
	const std::string base = KeyOfScene(dir/"base.RISEscene", ColliderScene(Ball()));
	Check(!base.empty() && base == KeyOfScene(dir/"again.RISEscene", ColliderScene(Ball())), "the scene key is reproducible");
	Check(KeyOfScene(dir/"radius.RISEscene", ColliderScene(Ball("0.25"))) != base, "the collider's geometry enters the key");
	Check(KeyOfScene(dir/"position.RISEscene", ColliderScene(Ball("0.3", "0.1 0.3 0"))) != base, "the collider's transform enters the key");
	const std::string animated = KeyOfScene(dir/"animated.RISEscene", ColliderScene(Ball(), timeline));
	Check(!animated.empty() && animated != base, "the collider's animation enters the key");
	std::string other = timeline;
	other.replace(other.find("0.6 0.3 0"), 9, "0.7 0.3 0");
	Check(KeyOfScene(dir/"animated2.RISEscene", ColliderScene(Ball(), other)) != animated, "a changed keyframe changes the key");
	// Unrelated edits.
	const std::string light = "omni_light\n{\n\tname\tlamp\n\tpower\t10\n\tcolor\t1 1 1\n\tposition\t0 3 0\n}\n\n";
	const std::string material = "uniformcolor_painter\n{\n\tname\tpnt\n\tcolor\t0.5 0.1 0.1\n}\n\nlambertian_material\n"
		"{\n\tname\tred\n\treflectance\tpnt\n}\n\n";
	Check(KeyOfScene(dir/"camera.RISEscene", ColliderScene(Ball(), "", "1 2 4")) == base, "a camera edit leaves the key");
	Check(KeyOfScene(dir/"light.RISEscene", ColliderScene(Ball(), light)) == base, "a light leaves the key");
	Check(KeyOfScene(dir/"material.RISEscene", material + ColliderScene(Ball("0.3", "0 0.3 0", "\tmaterial\tred\n"))) == base,
		"the collider's material leaves the key");
	const std::string bystander = "box_geometry\n{\n\tname\tgeo_box\n\twidth\t1\n\theight\t1\n\tdepth\t1\n}\n\n"
		"standard_object\n{\n\tname\tbox\n\tgeometry\tgeo_box\n\tposition\t3 0 0\n}\n\n";
	Check(KeyOfScene(dir/"bystander.RISEscene", ColliderScene(Ball(), bystander)) == base, "an unrelated object leaves the key");

	// SampleTransform is pure: sampling at other times does not move the scene's object.
	{
		WriteText(dir/"pure.RISEscene", ColliderScene(Ball(), timeline));
		Scene scene;
		Check(Load(dir/"pure.RISEscene", scene), "animated collider scene loads");
		IObjectPriv* ball = scene.job->GetObjects()->GetItem("ball");
		const Matrix4 before = ball->GetFinalTransformMatrix();
		Simulation::SceneObjectInputProvider provider(*scene.job->GetScene(),
			[&](const IGeometry* g, SimulatedGeometryBinding& b) { return scene.J()->SimulatedGeometryBindingOf(g, b); });
		std::string error;
		auto inputs = provider.Capture({"ball"}, {}, error);
		RigidTransform t;
		Check(inputs && inputs->SampleTransform("ball", 0.5, t, error) && std::fabs(t.translation[0] - 0.3) < 1e-12 &&
			std::fabs(t.translation[1] - 0.3) < 1e-12, "SampleTransform evaluates the keyframes (x = 0.3 at t = 0.5)", error);
		const Matrix4 after = ball->GetFinalTransformMatrix();
		Check(std::memcmp(&before, &after, sizeof(Matrix4)) == 0, "sampling does not pose the live scene");
		std::shared_ptr<const RISECBOR64::Bytes> bytes;
		std::string codec;
		MeshFrame frame;
		Check(inputs->Sample("ball", 1.0, bytes, codec, error) && codec == "mesh.v1" && bytes &&
			DecodeMesh(*bytes, budget, frame, error) && !frame.positions.empty(), "Sample returns a world-space mesh.v1", error);
	}

	// A scene-input bake with an animated collider equals the same motion given parametrically.
	{
		// The ball moves +x at 0.6 m/s (linear keyframes); the cloth falls on it.
		const fs::path scenePath = dir/"moving.RISEscene";
		WriteText(scenePath, ColliderScene(Ball("0.3", "0 0.3 0"), timeline));
		std::string log;
		Check(Worker({"bake", scenePath.string(), "drape"}, &log) == WorkerFinished, "bake with an animated scene collider", log);
		Scene scene;
		Check(Load(scenePath, scene), "moving scene loads");
		Simulation::SceneObjectInputProvider provider(*scene.job->GetScene(),
			[&](const IGeometry* g, SimulatedGeometryBinding& b) { return scene.J()->SimulatedGeometryBindingOf(g, b); });
		std::string error;
		auto inputs = provider.Capture({"ball"}, {}, error);
		std::shared_ptr<const RISECBOR64::Bytes> bytes;
		std::string codec;
		MeshFrame mesh;
		Check(inputs && inputs->Sample("ball", 0.0, bytes, codec, error) && DecodeMesh(*bytes, budget, mesh, error),
			"the collider's world mesh at t = 0", error);
		V::Values vs, ts;
		for (std::uint32_t bits : mesh.positions) { float f; std::memcpy(&f, &bits, 4); vs.push_back(V::Float(f)); }
		for (std::uint32_t k : mesh.indices) ts.push_back(V::Unsigned(k));
		V::Members params{{"vertices_u", V::Unsigned(9)}, {"vertices_v", V::Unsigned(9)}, {"frames", V::Unsigned(6)},
			{"substeps", V::Unsigned(4)}, {"origin", V::ArrayValue({V::Float(-0.5), V::Float(0.62), V::Float(0.5)})},
			{"pins", V::ArrayValue({V::MapValue({{"select", V::String("u0v0")}}), V::MapValue({{"select", V::String("u1v0")}})})},
			{"colliders", V::ArrayValue({V::MapValue({{"type", V::String("mesh")}, {"vertices", V::ArrayValue(vs)},
				{"triangles", V::ArrayValue(ts)}, {"motion", V::MapValue({{"type", V::String("translate")},
				{"velocity", V::ArrayValue({V::Float(0.6), V::Float(0), V::Float(0)})}})}})})}};
		RISECBOR64::Bytes cbor;
		RISECBOR64::Encode(V::MapValue(params), cbor, &error);
		std::ofstream(dir/"moving.cbor", std::ios::binary).write(reinterpret_cast<const char*>(cbor.data()),
			static_cast<std::streamsize>(cbor.size()));
		Check(Worker({"bake", "cloth.shell", (dir/"moving.cbor").string(), (dir/"moving_parametric").string()}, &log) ==
			WorkerFinished, "the same motion given parametrically bakes", log);
		CacheSnapshot a, b;
		Check(CacheSnapshot::Open(dir/"caches"/"drape", budget, a, error) && CacheSnapshot::Open(dir/"moving_parametric", budget, b, error),
			"both caches open", error);
		double worst = 0, worstVelocity = 0;
		std::size_t differingFrames = 0;
		bool same = a.Entries().size() == b.Entries().size() && !a.Entries().empty();
		for (std::size_t i = 0; same && i < a.Entries().size(); ++i) {
			std::shared_ptr<const RISECBOR64::Bytes> pa, pb;
			MeshFrame fa, fb;
			same = a.Read(a.Entries()[i].index, "surface", budget, pa, error) == FrameStatus::Ready &&
				b.Read(b.Entries()[i].index, "surface", budget, pb, error) == FrameStatus::Ready &&
				DecodeMesh(*pa, budget, a.Resolver(), fa, error) && DecodeMesh(*pb, budget, b.Resolver(), fb, error) &&
				fa.positions.size() == fb.positions.size();
			for (std::size_t k = 0; same && k < fa.positions.size(); ++k) {
				float x, y;
				std::memcpy(&x, &fa.positions[k], 4);
				std::memcpy(&y, &fb.positions[k], 4);
				worst = std::max(worst, static_cast<double>(std::fabs(x - y)));
			}
			for (std::size_t k = 0; same && k < fa.velocities.size() && k < fb.velocities.size(); ++k) {
				float x, y;
				std::memcpy(&x, &fa.velocities[k], 4);
				std::memcpy(&y, &fb.velocities[k], 4);
				worstVelocity = std::max(worstVelocity, static_cast<double>(std::fabs(x - y)));
			}
			if (same && pa && pb && *pa != *pb) ++differingFrames;
		}
		std::printf("   scene-input vs parametric collider motion: max position difference %.3g m, max velocity "
			"difference %.3g m/s, %zu of %zu payloads differ in bytes\n", worst, worstVelocity, differingFrames, a.Entries().size());
		Check(same && worst <= 1e-5, "scene-input motion equals the parametric motion to solver tolerance (1e-5 m)", error);
		Check(a.Description().key != b.Description().key, "their keys differ (scene input vs inline mesh)");
		// Not vacuous: the cloth touches the ball, so its motion matters.
		const fs::path staticPath = dir/"static.RISEscene";
		WriteText(staticPath, ColliderScene(Ball("0.3", "0 0.3 0")).replace(ColliderScene(Ball()).find("caches/drape"), 12, "caches/still"));
		Check(Worker({"bake", staticPath.string(), "drape"}, &log) == WorkerFinished, "bake with a static ball", log);
		CacheSnapshot still;
		double moved = 0;
		if (CacheSnapshot::Open(dir/"caches"/"still", budget, still, error) && !still.Entries().empty()) {
			std::shared_ptr<const RISECBOR64::Bytes> pa, pb;
			MeshFrame fa, fb;
			const auto& last = a.Entries().back();
			if (a.Read(last.index, "surface", budget, pa, error) == FrameStatus::Ready &&
				still.Read(last.index, "surface", budget, pb, error) == FrameStatus::Ready &&
				DecodeMesh(*pa, budget, a.Resolver(), fa, error) && DecodeMesh(*pb, budget, still.Resolver(), fb, error))
				for (std::size_t k = 0; k < fa.positions.size() && k < fb.positions.size(); ++k) {
					float x, y;
					std::memcpy(&x, &fa.positions[k], 4);
					std::memcpy(&y, &fb.positions[k], 4);
					moved = std::max(moved, static_cast<double>(std::fabs(x - y)));
				}
		}
		std::printf("   moving vs static ball, last frame: max position difference %.3g m\n", moved);
		Check(moved > 1e-4, "the collider's motion changes the cloth (the comparison is not vacuous)");
	}
}

// ---------------------------------------------------------------- G

void TestChain(const fs::path& root)
{
	std::printf("G: a two-simulation chain\n");
	const fs::path dir = root/"chain";
	fs::create_directories(dir);
	auto text = [](const std::string& gravity) {
		return std::string(Header) + Camera() + Cloth("upper", gravity, "-0.5 1 0.5") + Consumer("geo_upper", "upper.surface") +
			Cloth("lower", "\tcollider_object\tobj_geo_upper\n", "-0.5 1.4 0.5") + Consumer("geo_lower", "lower.surface");
	};
	const fs::path scenePath = dir/"chain.RISEscene";
	WriteText(scenePath, text(""));
	{
		Scene scene;
		Check(Load(scenePath, scene), "chain scene loads");
		const auto all = Statuses(scene);
		const auto* up = Find(all, "upper");
		const auto* down = Find(all, "lower");
		Check(up && down && up->state == SceneSimulationState::Missing && down->state == SceneSimulationState::Missing &&
			down->upstreamOutOfDate && down->upstream == std::vector<std::string>{"upper"} && down->expectedKey.empty(),
			"downstream waits for its upstream (missing, upstream out of date, no key yet)", down ? down->error : "");
	}
	std::string log;
	Check(Worker({"bake", scenePath.string(), "lower"}, &log) == WorkerFinished &&
		log.find("upper (cloth.shell") != std::string::npos, "baking the downstream bakes the upstream first", log);
	std::string downKey;
	{
		Scene scene;
		Check(Load(scenePath, scene), "baked chain loads");
		const auto all = Statuses(scene);
		const auto* up = Find(all, "upper");
		const auto* down = Find(all, "lower");
		Check(up && down && up->state == SceneSimulationState::Complete && down->state == SceneSimulationState::Complete &&
			!down->upstreamOutOfDate, "both complete");
		if (down) downKey = down->expectedKey;
	}
	// Change the upstream: it and everything downstream go stale.
	WriteText(scenePath, text("\tgravity\t0 -4 0\n"));
	{
		Scene scene;
		Check(Load(scenePath, scene), "edited chain loads");
		const auto all = Statuses(scene);
		const auto* up = Find(all, "upper");
		const auto* down = Find(all, "lower");
		Check(up && down && up->state == SceneSimulationState::Stale && down->state == SceneSimulationState::Stale &&
			down->upstreamOutOfDate, "staleness propagates downstream", down ? down->error : "");
	}
	Check(Worker({"bake", scenePath.string(), "--all"}, &log) == WorkerFinished, "rebake the chain", log);
	{
		Scene scene;
		Check(Load(scenePath, scene), "rebaked chain loads");
		const auto all = Statuses(scene);
		const auto* down = Find(all, "lower");
		Check(down && down->state == SceneSimulationState::Complete && down->expectedKey != downKey,
			"the downstream key follows the upstream's");
	}
	// A cycle is refused.
	const std::string cycle = std::string(Header) + Camera() +
		Cloth("a", "\tcollider_object\tobj_geo_b\n") + Consumer("geo_a", "a.surface") +
		Cloth("b", "\tcollider_object\tobj_geo_a\n", "-0.5 2 0.5") + Consumer("geo_b", "b.surface");
	WriteText(dir/"cycle.RISEscene", cycle);
	{
		Scene scene;
		Check(Load(dir/"cycle.RISEscene", scene), "a cyclic scene loads (scene inputs resolve after the derive)");
		std::vector<SceneSimulationStatus> s;
		std::string error;
		Check(!scene.J()->DescribeSimulations(s, error) && error.find("cycle") != std::string::npos, "a dependency cycle is refused", error);
	}
}

// ---------------------------------------------------------------- H, I

void TestEditsAndLegality(const fs::path& root)
{
	std::printf("H/I: parameter edits, reference legality\n");
	const fs::path dir = root/"edits";
	fs::create_directories(dir);
	const fs::path scenePath = dir/"edit.RISEscene";
	WriteText(scenePath, std::string(Header) + Camera() + Cloth("flag", "") + Consumer("geo_flag", "flag.surface"));
	std::string log;
	Check(Worker({"bake", scenePath.string(), "flag"}, &log) == WorkerFinished, "bake for the edit test", log);
	Scene scene;
	Check(Load(scenePath, scene), "edit scene loads");
	auto files = [&]() { return Sealed(DefaultCache(scenePath, "flag")); };
	const auto before = files();
	const int code = scene.job->ApplyCstParamEdit("flag", "", "substeps", 0, "5");
	Check(code == 1 || code == 2, "ApplyCstParamEdit on a cloth_simulation applies", std::to_string(code));
	auto all = Statuses(scene);
	Check(all.size() == 1 && all[0].state == SceneSimulationState::Stale, "the edit makes the simulation stale");
	Check(files() == before, "an edit starts no bake");
	Check(scene.job->ApplyCstParamEdit("flag", "", "substeps", 0, "4") != 0, "edit back");
	all = Statuses(scene);
	Check(all.size() == 1 && all[0].state == SceneSimulationState::Complete, "edited back: complete again");
	// Legality.
	auto refused = [&](const std::string& name, const std::string& body) {
		WriteText(dir/(name + ".RISEscene"), std::string(Header) + Camera() + body);
		Scene s;
		return !Load(dir/(name + ".RISEscene"), s);
	};
	Check(refused("unknown", Consumer("geo", "nonesuch.surface")), "a source naming no simulation is refused");
	Check(refused("output", Cloth("flag", "") + Consumer("geo", "flag.volume")), "a source naming no output is refused");
	Check(refused("both", Cloth("flag", "") + "simulated_mesh_geometry\n{\n\tname\tg\n\tsource\tflag.surface\n\tcache\t/tmp/x\n}\n\n"),
		"cache and source together are refused");
	Check(refused("dot", "cloth_simulation\n{\n\tname\ta.b\n}\n\n"), "a simulation name with a dot is refused");
	Check(refused("follow", Cloth("flag", "\tpin\tu0 follow pole\n")), "a pin following an undeclared object is refused");
	Check(refused("unused", Ball() + Cloth("flag", "\tfollow_object\tball\n")), "an unused follow_object is refused");
	// A separate `output` with the bare simulation name binds too.
	WriteText(dir/"bare.RISEscene", std::string(Header) + Camera() + Cloth("flag", "\tcache\t" + DefaultCache(scenePath, "flag").string() + "\n") +
		"simulated_mesh_geometry\n{\n\tname\tg\n\tsource\tflag\n\toutput\tsurface\n}\n\nstandard_object\n{\n\tname\to\n\tgeometry\tg\n}\n\n");
	Scene bare;
	Check(Load(dir/"bare.RISEscene", bare) && Mesh(bare, "g") && Mesh(bare, "g")->HasSource(), "`source flag` + `output surface` binds");
}

// ---------------------------------------------------------------- J

std::string FireScene(const std::string& fields, const std::string& medium = "\tfidelity_mode\tpreview\n")
{
	return std::string(Header) + Camera() +
		"fire_simulation\n{\n\tname\tcampfire\n" + fields + "}\n\n"
		"fire_medium\n{\n\tname\tflame\n" + medium + "\tsource\tcampfire.volume\n\tchannel_carbon\tcarbon\n"
		"\tchannel_temperature\ttemperature\n\tchem_model\tnone\n}\n\n";
}

void TestFire(const fs::path& root)
{
	std::printf("J: fire_simulation\n");
	const fs::path dir = root/"fire";
	fs::create_directories(dir);
	const std::string window = "\tframe_origin_s\t1\n\tend_time_s\t2\n";
	const fs::path path = dir/"fire.RISEscene";
	WriteText(path, FireScene("\tmode\tfast\n\ttier\t6\n\tadvective_cfl\t0.4\n\tinert_plume\tTRUE\n"
		"\tsoot_self_absorption\t2\n" + window));
	Scene scene;
	Check(Load(path, scene), "a fire_simulation and a fire_medium bound to it load without a cache");
	if (!scene.J()) return;
	Check(scene.job->GetMedium("flame") != nullptr, "the unbaked fire's medium exists (empty, with a warning)");
	const auto specs = scene.J()->SimulationSpecs();
	Check(specs.size() == 1 && specs[0].solverId == "fire.methane" && specs[0].keyword == "fire_simulation" &&
		specs[0].outputs.size() == 1 && specs[0].outputs[0].first == "volume", "fire spec: solver, keyword, output");
	if (specs.size() != 1) return;
	const V& p = specs[0].parameters;
	Check(p.GetMap().size() == 7 && p.Find("mode") && p.Find("mode")->GetText() == "fast" && p.Find("tier") &&
		p.Find("tier")->GetIntegerArgument() == 6 && p.Find("advective_cfl") && p.Find("advective_cfl")->GetText() == "0.4" &&
		p.Find("inert_plume") && p.Find("inert_plume")->GetBoolean() && p.Find("soot_self_absorption") &&
		p.Find("soot_self_absorption")->GetIntegerArgument() == 2 && p.Find("end_time_s") && p.Find("end_time_s")->GetFloat() == 2.0,
		"the chunk maps exactly the authored fields, in the solver's types", Encoded(p).size() ? "" : "");
	// Completion: every option explicit (no legacy environment default reaches a scene's fire).
	const ParameterSchema& schema = FireParameterSchema();
	V completed;
	std::string error;
	Check(schema.completeParameters && schema.completeParameters(p, completed, error) &&
		completed.GetMap().size() == schema.parameters.size() - 1 && !completed.Find("backend"),
		"completion states every schema parameter but backend (keyed only as cpu)", error);
	Check(completed.Find("fast_fixed_vcycles") && !completed.Find("fast_fixed_vcycles")->GetBoolean() &&
		completed.Find("source_step_cap_s") && completed.Find("source_step_cap_s")->GetText().empty() &&
		completed.Find("pool_diameter_m") && completed.Find("pool_diameter_m")->GetFloat() == 0.3,
		"completed defaults are the solver's canonical values (false, unset text, 0.30 m)");
	const auto all = Statuses(scene);
	std::string key;
	Check(MakeCacheKey("fire.methane", schema.solverVersion, completed, {}, key, error), "independent key", error);
	Check(all.size() == 1 && all[0].state == SceneSimulationState::Missing && all[0].expectedKey == key &&
		all[0].framesTotal == 17 && !all[0].interactive,
		"fire is keyed by prediction: MakeCacheKey(fire.methane, 1, completed parameters); 17 frames in [1, 2] at 1/16 s",
		all.empty() ? "" : all[0].error + " frames " + std::to_string(all[0].framesTotal));
	auto keyOf = [&](const std::string& fields) {
		WriteText(dir/"variant.RISEscene", FireScene(fields));
		Scene v;
		if (!Load(dir/"variant.RISEscene", v)) return std::string("load failed");
		const auto st = Statuses(v);
		return st.size() == 1 ? st[0].expectedKey : std::string();
	};
	const std::string base = keyOf(window);
	Check(!base.empty() && base == keyOf(window + "\tadvective_cfl\t0.5\n\tmode\tcertified\n\tinert_plume\tFALSE\n"),
		"an option given its default value keys like the option left out");
	// M8d: scene selection of the CPU backend (docs/SIMULATION_SOLVER_CONTRACT.md section 12.4). These
	// keys were recorded with the build before `backend` became a scene parameter (f005c0ac...df27,
	// 5b419d96...712f) and re-pinned at the fire solver version bump 2 -> 3 (section 13: the
	// rim-ignition witness), which changes every fire key: a fire scene keeps its key byte for byte
	// across scene-schema changes, and so does one that states the default.
	const std::string pinnedBase = "4a205582611b41442198b7e53158352c93981f099dbf2ffbf64b0e6703d2bc58", pinnedOptions = "1394dc8b95c2af91b6a9fe3dbbc73ca836b074869a51244da9725d87188b798d";
	std::printf("fire keys: base %s options %s\n", base.c_str(), key.c_str());
	Check(base == pinnedBase && key == pinnedOptions, "existing fire scenes keep their keys (pinned at "
		"fire solver version 3)", base + " " + key);
	Check(keyOf(window + "\tbackend\tmetal\n") == base, "backend metal (the default) keys like backend left out");
	const std::string cpuKey = keyOf(window + "\tbackend\tcpu\n");
	Check(!cpuKey.empty() && cpuKey != base && cpuKey.size() == 64, "backend cpu has its own key", cpuKey);
	for (const std::string& change : {"\tmode\tfast\n", "\ttier\t6\n", "\tinert_plume\tTRUE\n", "\tadvective_cfl\t0.4\n",
		"\tsoot_self_absorption\t1\n", "\tvreman_zone\tbelow:16:0.2857\n\tcompact_viscous_stencil\tTRUE\n"})
		Check(keyOf(window + change) != base, "the key follows " + change.substr(1, change.find('\t', 1) - 1));
	// Without the window the key is unknown until the first bake (not a failure).
	{
		WriteText(dir/"nowindow.RISEscene", FireScene(""));
		Scene v;
		Check(Load(dir/"nowindow.RISEscene", v), "a fire with a solver-derived window loads");
		const auto st = Statuses(v);
		Check(st.size() == 1 && st[0].state == SceneSimulationState::Missing && st[0].expectedKey.empty() &&
			st[0].error.find("key unknown") != std::string::npos, "its key is unknown until the first bake",
			st.empty() ? "" : st[0].error);
	}
	// Schema validation at parse time (the solver's own validators and ranges).
	auto refused = [&](const std::string& name, const std::string& text) {
		WriteText(dir/(name + ".RISEscene"), text);
		Scene v;
		return !Load(dir/(name + ".RISEscene"), v);
	};
	Check(refused("cfl", FireScene(window + "\tadvective_cfl\t2\n")), "advective_cfl 2 is refused (the solver's (0, 1] validator)");
	Check(refused("zone", FireScene(window + "\tvreman_zone\tsideways\n")), "a malformed vreman_zone is refused");
	Check(refused("level", FireScene(window + "\tsoot_self_absorption\t3\n")), "soot_self_absorption 3 is refused (0..2)");
	Check(refused("pool", FireScene(window + "\tpool_diameter_m\t0.4\n")), "a pool other than 0.30 m is refused");
	Check(refused("mode", FireScene(window + "\tmode\tturbo\n")), "an unknown mode is refused");
	Check(refused("backend", FireScene(window + "\tbackend\tgpu\n")), "an unknown backend is refused");
	Check(refused("predictive", FireScene(window, "\tfidelity_mode\tpredictive\n")), "a predictive fire with no cache fails closed");
	// Legality across producers and consumers.
	Check(refused("fire_to_mesh", FireScene(window) + Consumer("geo", "campfire.volume")),
		"simulated_mesh_geometry refuses a grid output");
	Check(refused("mesh_to_fire", std::string(Header) + Camera() + Cloth("flag", "") +
		"fire_medium\n{\n\tname\tflame\n\tfidelity_mode\tpreview\n\tsource\tflag.surface\n\tchannel_carbon\tcarbon\n"
		"\tchannel_temperature\ttemperature\n\tchem_model\tnone\n}\n\n"), "fire_medium refuses a mesh output");
	Check(refused("both", std::string(Header) + Camera() + "fire_simulation\n{\n\tname\tcampfire\n}\n\n"
		"fire_medium\n{\n\tname\tflame\n\tfidelity_mode\tpreview\n\tsource\tcampfire.volume\n\tsequence_manifest\t/x/manifest.cbor\n"
		"\tchannel_carbon\tcarbon\n\tchannel_temperature\ttemperature\n\tchem_model\tnone\n}\n\n"),
		"fire_medium refuses both source and sequence_manifest");
	Check(FindSolverSchema("fire.methane") == &schema, "the fire schema is registered beside the factory");
}

// simulation_cache: a foreign producer; consumers bind it like any producer.
void TestSimulationCache(const fs::path& root)
{
	std::printf("K: simulation_cache\n");
	const fs::path dir = root/"foreign";
	fs::create_directories(dir);
	std::string log;
	WriteText(dir/"flag.json", "{\"vertices_u\": 5, \"vertices_v\": 5, \"frames\": 3}");
	Check(Worker({"bake", "cloth.shell", (dir/"flag.json").string(), (dir/"imported").string()}, &log) == WorkerFinished,
		"a cache baked elsewhere", log);
	WriteText(dir/"foreign.RISEscene", std::string(Header) + Camera() +
		"simulation_cache\n{\n\tname\tfab\n\tcache\timported\n}\n\n" + Consumer("geo", "fab.surface"));
	Scene scene;
	Check(Load(dir/"foreign.RISEscene", scene), "a simulation_cache scene loads");
	auto* mesh = Mesh(scene, "geo");
	Check(mesh && mesh->HasSource() && mesh->Source().VertexCount() == 25, "its consumer binds the imported cache");
	const auto all = Statuses(scene);
	Check(all.size() == 1 && all[0].state == SceneSimulationState::Complete && all[0].solverId.empty() &&
		!all[0].expectedKey.empty() && all[0].expectedKey == all[0].cacheKey, "a foreign cache is complete (never stale)");
	Check(Worker({"bake", (dir/"foreign.RISEscene").string(), "fab"}, &log) == WorkerFinished &&
		log.find("not baked") != std::string::npos, "rise-sim does not bake a simulation_cache", log);
	WriteText(dir/"foreign_grid.RISEscene", std::string(Header) + Camera() +
		"simulation_cache\n{\n\tname\tfab\n\tcache\timported\n}\n\n"
		"fire_medium\n{\n\tname\tflame\n\tfidelity_mode\tpreview\n\tsource\tfab.surface\n\tchannel_carbon\tcarbon\n"
		"\tchannel_temperature\ttemperature\n\tchem_model\tnone\n}\n\n");
	Scene grid;
	Check(!Load(dir/"foreign_grid.RISEscene", grid), "fire_medium refuses a foreign mesh output");
}

// ---------------------------------------------------------------- L

void TestEditorAndAgent(const fs::path& root, const fs::path& repo)
{
	std::printf("L: round trip, agent schema\n");
	const fs::path dir = root/"roundtrip";
	fs::create_directories(dir);
	for (const char* name : {"cloth_flag_scene", "cloth_chain_scene_inputs", "fire_campfire_scene"}) {
		const fs::path source = repo/"scenes"/"Tests"/"Simulation"/(std::string(name) + ".RISEscene");
		if (!fs::exists(source)) { Check(false, std::string("sample scene present: ") + name); continue; }
		const fs::path copy = dir/(std::string(name) + ".RISEscene");
		fs::copy_file(source, copy, fs::copy_options::overwrite_existing);
		Scene a;
		Check(Load(copy, a), std::string("sample loads: ") + name);
		if (!a.J() || !a.J()->GetCstDocument()) continue;
		const std::string saved = RISE::Cst::SerializeCst(*a.J()->GetCstDocument());
		const fs::path again = dir/(std::string(name) + "_saved.RISEscene");
		WriteText(again, saved);
		Scene b;
		Check(Load(again, b), std::string("saved scene loads: ") + name);
		if (!b.J()) continue;
		const auto sa = a.J()->SimulationSpecs(), sb = b.J()->SimulationSpecs();
		bool same = !sa.empty() && sa.size() == sb.size();
		for (std::size_t i = 0; same && i < sa.size(); ++i)
			same = sa[i].name == sb[i].name && sa[i].keyword == sb[i].keyword && sa[i].solverId == sb[i].solverId &&
				Encoded(sa[i].parameters) == Encoded(sb[i].parameters) && sa[i].sceneInputs == sb[i].sceneInputs &&
				sa[i].outputs == sb[i].outputs && sa[i].cacheDir.filename() == sb[i].cacheDir.filename();
		Check(same, std::string("load -> save -> load keeps every simulation: ") + name);
		const auto ka = Statuses(a), kb = Statuses(b);
		Check(ka.size() == kb.size() && !ka.empty() && ka[0].expectedKey == kb[0].expectedKey,
			std::string("and its key: ") + name);
	}
	for (const char* keyword : {"cloth_simulation", "fire_simulation", "simulation_cache"}) {
		const ChunkDescriptor* d = DescriptorForKeyword(String(keyword));
		Check(d && d->category == ChunkCategory::Simulation, std::string(keyword) + " registers under ChunkCategory::Simulation");
		const std::string schema = RISE::Agent::SchemaGenForChunk(keyword);
		Check(schema.find(keyword) != std::string::npos && schema.find("\"simulation\"") != std::string::npos,
			std::string("the agent schema lists ") + keyword, schema.substr(0, 200));
	}
	const std::string category = RISE::Agent::SchemaGenCategory("simulation");
	Check(category.find("cloth_simulation") != std::string::npos && category.find("fire_simulation") != std::string::npos &&
		category.find("simulation_cache") != std::string::npos, "the agent's `simulation` category lists all three");
	// The schema drives the descriptor: every cloth schema parameter is a chunk parameter with its default.
	const ChunkDescriptor* cloth = DescriptorForKeyword(String("cloth_simulation"));
	std::size_t matched = 0;
	for (const auto& p : ClothParameterSchema().parameters)
		if (cloth) for (const auto& q : cloth->parameters) if (q.name == p.name && q.defaultValueHint == p.defaultText) ++matched;
	Check(matched == ClothParameterSchema().parameters.size(), "cloth_simulation's descriptor is generated from the schema");
	const ChunkDescriptor* consumer = DescriptorForKeyword(String("simulated_mesh_geometry"));
	bool reference = false;
	if (consumer) for (const auto& q : consumer->parameters)
		if (q.name == "source") reference = q.kind == ValueKind::Reference && q.referenceCategories == std::vector<ChunkCategory>{ChunkCategory::Simulation};
	Check(reference, "simulated_mesh_geometry.source is a Reference to ChunkCategory::Simulation (the editor draws the connection)");
}


// ---------------------------------------------------------------- M (M5c)

// The estimates a scheduler and the panel read before a bake (not keyed).
void TestEstimates(const fs::path& root)
{
	std::printf("M: estimates\n");
	Capabilities certified, fast, tier6, tier10;
	FireEstimateCapabilities(8, false, {{0, 0, 0}}, certified);
	FireEstimateCapabilities(8, true, {{0, 0, 0}}, fast);
	FireEstimateCapabilities(6, false, {{0, 0, 0}}, tier6);
	FireEstimateCapabilities(10, false, {{0, 0, 0}}, tier10);
	const std::uint64_t table = ((14u + ((std::uint64_t(1) << 24) - 1)*8u) - (14u + 1023u*8u))*4u*sizeof(float);
	// M8: the reserve comes from the Metal owner's working set, so a build without
	// Metal (any non-Apple platform, or macOS with Metal compiled out) reports 0 (unknown).
	RISE::FireProductionComputeCapability compute;
	RISE::QueryFireProductionComputeCapability(compute);
	const bool metalBuilt = compute.deviceDiscovery != RISE::FireProductionDeviceBackendNotBuilt;
	if (!metalBuilt)
		Check(certified.memoryEstimate == 0 && fast.memoryEstimate == 0, "without Metal the fire memory estimate is unknown");
	if (metalBuilt) {
	// The production driver recorded this reserve for a tier-8 certified scene bake
	// (host_memory_admission.v1 next_step_reserve_bytes, 2026-11-23).
	Check(certified.memoryEstimate == 10201038848ull, "fire tier 8 certified: the driver's admission reserve",
		std::to_string(certified.memoryEstimate));
	Check(fast.memoryEstimate == certified.memoryEstimate - table, "fast mode leaves out the 24-level EOS table",
		std::to_string(fast.memoryEstimate));
	Check(tier6.memoryEstimate > 0 && tier6.memoryEstimate < certified.memoryEstimate &&
		tier10.memoryEstimate > certified.memoryEstimate, "the reserve grows with the tier",
		std::to_string(tier6.memoryEstimate) + " " + std::to_string(tier10.memoryEstimate));
	}
	Check(certified.costEstimate == 2324 && fast.costEstimate == 1121 && tier6.costEstimate == 0 &&
		std::fabs(tier10.costEstimate - 1.29*2324) < 1e-9, "fire cost: tier 8 measured, tier 10 scaled, tier 6 unknown");

	const fs::path dir = root/"estimates";
	fs::create_directories(dir);
	WriteText(dir/"fire8.RISEscene", FireScene("\ttier\t8\n\tframe_origin_s\t1\n\tend_time_s\t2\n"));
	Scene fire;
	Check(Load(dir/"fire8.RISEscene", fire), "a tier-8 fire scene loads");
	const auto st = Statuses(fire);
	const ParameterSchema& schema = FireParameterSchema();
	V completed;
	std::string error, key;
	if (fire.J() && !fire.J()->SimulationSpecs().empty())
		Check(schema.completeParameters(fire.J()->SimulationSpecs()[0].parameters, completed, error) &&
			MakeCacheKey("fire.methane", schema.solverVersion, completed, {}, key, error), "independent fire key", error);
	Check(st.size() == 1 && st[0].expectedKey == key && st[0].framesTotal == 17,
		"the estimates leave the predicted key and outputs unchanged");
	if (st.size() == 1) {
		Check(st[0].startTime == 1 && st[0].interval == 0.0625, "the predicted time support: frames from 1 s at 1/16 s");
		if (metalBuilt)
		Check(st[0].memoryEstimate == certified.memoryEstimate && st[0].costEstimate == 2324 &&
			std::fabs(st[0].wallEstimate - 2324*2.0) < 1e-6 &&
			FormatSimulationEstimate(st[0]) == "≈ 77 min, 10.2 GB", "the planned fire carries them: 2 simulated s",
			FormatSimulationEstimate(st[0]));
	}
	// Cloth: from the vertex count and mode (ClothSolver::Configure).
	WriteText(dir/"cloth.RISEscene", std::string(Header) + Camera() + Cloth("flag", ""));
	Scene cloth;
	Check(Load(dir/"cloth.RISEscene", cloth), "a cloth scene loads");
	const auto cs = Statuses(cloth);
	const double n = 81, substeps = 4, rate = 30;
	Check(cs.size() == 1 && cs[0].memoryEstimate == ((std::uint64_t(10) << 20) + 81*13312)*5/4 &&
		std::fabs(cs[0].costEstimate - 12e-6*n*substeps*rate) < 1e-12 &&
		std::fabs(cs[0].wallEstimate - cs[0].costEstimate*5.0/30.0) < 1e-12,
		"cloth: memory 1.25 x (10 MiB + 13 KiB per vertex); cost 12 us per vertex-substep (bake)",
		cs.empty() ? "" : FormatSimulationEstimate(cs[0]));
	SceneSimulationStatus none;
	Check(FormatSimulationEstimate(none).empty(), "nothing known: no estimate text");
}

// ---------------------------------------------------------------- the default cache location

// Records the text of every log message while `On()`.
class LogCapture : public virtual ILogPrinter, public virtual Implementation::Reference {
public:
	void Print(const LogEvent& event) override
	{
		std::lock_guard<std::mutex> hold(mutex_);
		if (on_) text_ += std::string(event.szMessage) + "\n";
	}
	void Flush() override {}
	void Begin() { std::lock_guard<std::mutex> hold(mutex_); on_ = true; text_.clear(); }
	std::string End() { std::lock_guard<std::mutex> hold(mutex_); on_ = false; return text_; }
protected:
	~LogCapture() override {}
private:
	std::mutex mutex_;
	bool on_ = false;
	std::string text_;
};

void SetOverride(const char* value)
{
#ifndef _WIN32
	if (value) ::setenv("RISE_SIMULATION_CACHE_DIR", value, 1); else ::unsetenv("RISE_SIMULATION_CACHE_DIR");
#else
	(void)value;
#endif
}

void TestDefaultCache(const fs::path& root)
{
	std::printf("default cache location: per-user folder, one function for every process\n");
	const fs::path dir = root/"default_cache";
	fs::create_directories(dir);
	const char* previous = std::getenv("RISE_SIMULATION_CACHE_DIR");
	const std::string saved = previous ? previous : "";
	const fs::path base = dir/"user-cache";
	SetOverride(base.string().c_str());
	// Resolution: stable, per scene, per name, under the override.
	std::string error;
	const fs::path sceneA = dir/"a"/"flag.RISEscene", sceneB = dir/"b"/"flag.RISEscene";
	const fs::path a1 = DefaultSimulationCachePath(sceneA, "flag", error), a2 = DefaultSimulationCachePath(sceneA, "flag", error);
	const fs::path b1 = DefaultSimulationCachePath(sceneB, "flag", error);
	Check(!a1.empty() && a1 == a2, "the default is stable across calls", a1.string());
	Check(a1 != b1 && a1.parent_path() != b1.parent_path(), "two scenes of the same file name get different directories");
	Check(a1.filename() == "flag" && DefaultSimulationCachePath(sceneA, "other", error).parent_path() == a1.parent_path(),
		"one directory per scene, one leaf per simulation");
	const std::string scene = a1.parent_path().filename().string();
	Check(scene.size() == std::string("flag-").size() + 16 && scene.compare(0, 5, "flag-") == 0 &&
		scene.find_first_not_of("0123456789abcdef", 5) == std::string::npos, "<scene-stem>-<16 hex>", scene);
	Check(a1.parent_path().parent_path() == DefaultSimulationCacheDirectory() && DefaultSimulationCacheDirectory() == base/"scene-caches",
		"the override replaces the per-user base", DefaultSimulationCacheDirectory().string());
	Check(DefaultSimulationCachePath(sceneA, "a/b", error).empty() && !error.empty(), "a simulation name with a separator cannot name a default");
	SetOverride("relative/ignored");
	Check(DefaultSimulationCacheDirectory().is_absolute() && DefaultSimulationCacheDirectory().filename() == "scene-caches" &&
		DefaultSimulationCacheDirectory().string().find("relative/ignored") == std::string::npos, "a relative override is ignored");
	SetOverride(nullptr);
	Check(DefaultSimulationCacheDirectory().parent_path() == DefaultBakeStateDirectory().parent_path(),
		"without the override the default sits beside the per-user bake state", DefaultSimulationCacheDirectory().string());
	SetOverride(base.string().c_str());
	// The location policy accepts the default with no trusted root.
	ClearTrustedCacheRoots();
	std::string why;
	Check(CheckCacheLocation(a1, true, why), "the default location passes the cache location policy", why);
	Check(fs::is_directory(a1), "and is created");
	// The scene: the Job, the status the worker and the bake manager print, and a bake agree.
	fs::create_directories(sceneA.parent_path());
	WriteText(sceneA, std::string(Header) + Camera() + Cloth("flag", "") + Consumer("geo_flag", "flag.surface"));
	{
		Scene s;
		Check(Load(sceneA, s), "the scene loads");
		const auto all = Statuses(s);
		Check(all.size() == 1 && all[0].cacheDir == a1, "the host resolves the default through the same function", all.empty() ? "" : all[0].cacheDir.string());
	}
	std::string log;
	Check(Worker({"bake", sceneA.string(), "flag"}, &log) == WorkerFinished && log.find(a1.string()) != std::string::npos,
		"rise-sim bakes into the default and says where", log);
	CacheSnapshot snapshot;
	Check(CacheSnapshot::Open(a1, budget, snapshot, error) && snapshot.Complete(), "the cache is at the default", error);
	Check(Worker({"status", sceneA.string()}, &log) == WorkerFinished && log.find("flag: complete") != std::string::npos,
		"a later process finds it again", log);
	// The same scene reached by another spelling of its path finds the same cache; a copy elsewhere does not.
	{
		Scene viaDots;
		Check(Load(dir/"a"/".."/"a"/"flag.RISEscene", viaDots), "the scene loads through a different spelling");
		const auto all = Statuses(viaDots);
		Check(all.size() == 1 && all[0].state == SceneSimulationState::Complete, "the same scene finds its cache again",
			all.empty() ? "" : all[0].cacheDir.string());
		fs::create_directories(sceneB.parent_path());
		WriteText(sceneB, std::string(Header) + Camera() + Cloth("flag", "") + Consumer("geo_flag", "flag.surface"));
		Scene other;
		Check(Load(sceneB, other), "the second scene loads");
		const auto st = Statuses(other);
		Check(st.size() == 1 && st[0].state == SceneSimulationState::Missing && st[0].cacheDir == b1,
			"a second scene with the same names does not share the cache");
	}
	// An explicit `cache` is unchanged: relative to the scene, nothing written to the default.
	{
		const fs::path explicitScene = dir/"c"/"flag.RISEscene";
		fs::create_directories(explicitScene.parent_path());
		WriteText(explicitScene, std::string(Header) + Camera() + Cloth("flag", "\tcache\tmine/flag\n") + Consumer("geo_flag", "flag.surface"));
		Scene s;
		Check(Load(explicitScene, s), "a scene with an explicit cache loads");
		const auto all = Statuses(s);
		Check(all.size() == 1 && all[0].cacheDir == (explicitScene.parent_path()/"mine"/"flag").lexically_normal(),
			"an explicit relative `cache` stays relative to the scene", all.empty() ? "" : all[0].cacheDir.string());
		Check(Worker({"bake", explicitScene.string(), "flag"}, &log) == WorkerFinished &&
			fs::exists(explicitScene.parent_path()/"mine"/"flag"/"manifest.cbor") &&
			!fs::exists(DefaultSimulationCachePath(explicitScene, "flag", error)), "an explicit cache bakes there and not at the default", log);
		// The cache directory is not part of the key: the same configuration elsewhere has the same key.
		CacheSnapshot one, two;
		Check(CacheSnapshot::Open(explicitScene.parent_path()/"mine"/"flag", budget, one, error) && CacheSnapshot::Open(a1, budget, two, error) &&
			one.Description().key == two.Description().key && Sealed(explicitScene.parent_path()/"mine"/"flag") == Sealed(a1),
			"the location enters no key and no frame bytes", error);
	}
	// An old default cache is mentioned once, never read.
	{
		static LogCapture* capture = [] { auto* c = new LogCapture(); GlobalLogPriv()->AddPrinter(c); c->release(); return c; }();
		const fs::path oldScene = dir/"d"/"flag.RISEscene";
		fs::create_directories(oldScene.parent_path());
		WriteText(oldScene, std::string(Header) + Camera() + Cloth("flag", "") + Consumer("geo_flag", "flag.surface"));
		fs::create_directories(oldScene.parent_path()/"caches"/"flag");
		WriteText(oldScene.parent_path()/"caches"/"flag"/"manifest.cbor", "not read");
		capture->Begin();
		{
			Scene s;
			Check(Load(oldScene, s), "a scene with a cache at the old default loads");
			const auto all = Statuses(s);
			Check(all.size() == 1 && all[0].state == SceneSimulationState::Missing, "the old cache is not read");
		}
		const std::string text = capture->End();
		const auto pos = text.find("old default");
		Check(pos != std::string::npos && text.find("old default", pos + 1) == std::string::npos,
			"the old default is mentioned once", text);
		Check(text.find((oldScene.parent_path()/"caches"/"flag").string()) != std::string::npos &&
			text.find("`cache`") != std::string::npos, "naming where it is and how to reuse it", text);
		// No old cache, no line; a populated new default, no line.
		capture->Begin();
		{ Scene s; Load(sceneA, s); }
		Check(capture->End().find("old default") == std::string::npos, "no line when the new default holds the cache");
	}
	SetOverride(saved.empty() ? nullptr : saved.c_str());
}
// Non-render capture-policy regression for every ClonePoseHolder input role.
class CustomInputObject final : public Implementation::Object
{
public:
    using Object::Object;
    void SetIntermediateValue(const IKeyframeParameter& parameter) override
    {
        Object::SetIntermediateValue(parameter);
        SetPosition(Point3(9,0,0));
    }
};
class CaptureCountingSphere final : public Implementation::SphereGeometry
{
public:
    CaptureCountingSphere():SphereGeometry(1) {}
    void addref() const override { ++retains; SphereGeometry::addref(); }
    mutable unsigned int retains=0;
};
void TestConsumedInputPolicy(const bool ancestor)
{
    using namespace Implementation;
    const std::string label=std::string(ancestor ? "consumed ancestor":"shared CSG operand")+" [scene-input-pose-holder-policy] ";
    const auto check=[&](bool ok,const std::string& what) {
        Check(ok,label+what);
        if(ok) std::printf("PASS: %s%s\n",label.c_str(),what.c_str());
    };
    Scene scene;
    check(RISE_CreateJobPriv(&scene.job)&&scene.job,"job fixture");
    if(!scene.job) return;
    auto* geometry=new CaptureCountingSphere;
    auto* operand=new Object(geometry);
    auto* other=new Object(geometry);
    Object* leaf=ancestor ? new Object(geometry):operand;
    auto* manager=scene.job->GetObjects();
    check(manager->AddItem(leaf,"input"),"input is registered");
    if(ancestor) check(manager->AddItem(operand,"parent")&&manager->SetObjectParent("input","parent"),
        "hierarchy link exists before its parent becomes an operand");
    auto* first=new CSGObject(CSG_UNION);
    auto* second=new CSGObject(CSG_UNION);
    operand->SetPosition(Point3(1,0,0));operand->FinalizeTransformations();
    first->SetPosition(Point3(10,0,0));first->FinalizeTransformations();
    second->SetPosition(Point3(20,0,0));second->FinalizeTransformations();
    check(first->AssignObjects(operand,other)&&second->AssignObjects(operand,other),"two composites share the exact Object operand");
    check(manager->AddItem(first,"first")&&manager->AddItem(second,"second"),
        "both transformed composites are registered");
    const Point3 localOrigin=Point3Ops::Transform(operand->GetFinalTransformMatrix(),Point3(0,0,0));
    const Point3 firstWorld=Point3Ops::Transform(first->GetFinalTransformMatrix(),localOrigin);
    const Point3 secondWorld=Point3Ops::Transform(second->GetFinalTransformMatrix(),localOrigin);
    check(operand->IsConsumed()&&localOrigin.x==1&&firstWorld.x==11&&secondWorld.x==21,
        "one composite-local pose has two distinct world placements");
    geometry->retains=0;
    SceneObjectInputProvider provider(*scene.job->GetScene(),{});
    std::string error;
    auto inputs=provider.Capture({"input"},{},error);
    if(inputs) {
        RigidTransform t;
        const bool sampled=inputs->SampleTransform("input",.5,t,error);
        std::printf("  old capture sampled=%d x=%.17g; composite world x=11 and 21\n",sampled,sampled ? t.translation[0]:-1.0e30);
    }
    check(!inputs&&error.find("a CSG operand")!=std::string::npos,"ambiguous world-space input is refused explicitly");
    check(geometry->retains==0,"refusal occurs before any geometry-owning pose holder is cloned");
    check(operand->GetTimeSlot()==-1,"refusal leaves shared operand unslotted");
    inputs.reset();
    operand->release();other->release();first->release();second->release();geometry->release();
    if(ancestor) leaf->release();
}

void TestPoseHolderInputPolicy()
{
    using namespace Implementation;
    std::printf("P: exact Object scene-input pose-holder capture policy (no render)\n");
    TestConsumedInputPolicy(false);
    TestConsumedInputPolicy(true);
    for(const bool ancestor : {false,true}) for(int kind=0;kind<3;++kind) {
        const std::string label=std::string(ancestor ? "ancestor ":"leaf ")+
            (kind==0 ? "built-in":kind==1 ? "custom":"CSG")+" [scene-input-pose-holder-policy] ";
        const auto check=[&](bool ok,const std::string& what) {
            Check(ok,label+what);
            if(ok) std::printf("PASS: %s%s\n",label.c_str(),what.c_str());
        };
        Scene scene;
        check(RISE_CreateJobPriv(&scene.job)&&scene.job,"job fixture");
        if(!scene.job) continue;
        auto* geometry=new CaptureCountingSphere;
        Object* target=kind==0 ? new Object:kind==1 ? static_cast<Object*>(new CustomInputObject):
            static_cast<Object*>(new CSGObject(CSG_UNION));
        Object* leaf=ancestor ? new Object(geometry):target;
        if(!ancestor&&kind!=2) target->AssignGeometry(*geometry);
        auto* manager=scene.job->GetObjects();
        check(manager->AddItem(leaf,"input"),"input fixture registered");
        if(ancestor) check(manager->AddItem(target,"parent")&&manager->SetObjectParent("input","parent"),
            "parent fixture linked");
        auto* animator=scene.job->GetScene()->GetAnimator();
        check(animator->InsertKeyframe(target,"position","0 0 0",0,nullptr,nullptr)&&
            animator->InsertKeyframe(target,"position","2 0 0",1,nullptr,nullptr),"animated fixture");
        const Matrix4 before=leaf->GetFinalTransformMatrix();
        geometry->retains=0;
        SceneObjectInputProvider provider(*scene.job->GetScene(),{});
        std::string error;
        auto inputs=provider.Capture({"input"},{},error);
        if(kind==0) {
            check(inputs!=nullptr,"exact Object chain remains capturable");
            check(geometry->retains>0,"ordinary capture clones geometry-owning pose holders");
            if(inputs) {
                RigidTransform t;
                const bool sampled=inputs->SampleTransform("input",.5,t,error);
                check(sampled&&t.translation[0]==1&&t.translation[1]==0&&t.translation[2]==0,
                    "private pose evaluates exact midpoint translation");
            }
        } else {
            check(!inputs,"unsupported dynamic type is rejected");
            check(error.find("not an exact built-in Object")!=std::string::npos&&
                error.find(ancestor ? "parent `parent`":"scene object `input`")!=std::string::npos,
                "diagnostic identifies unsupported capture role before geometry work");
            check(geometry->retains==0,"rejected capture clones no geometry-owning holder");
        }
        const Matrix4 after=leaf->GetFinalTransformMatrix();
        check(std::memcmp(&before,&after,sizeof(Matrix4))==0,"capture and sampling preserve live transform");
        check(leaf->GetTimeSlot()==-1&&target->GetTimeSlot()==-1,"capture leaves no time slots");
        inputs.reset();
        leaf->release();
        if(ancestor) target->release();
        geometry->release();
    }
}

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("SimulationSceneTest (simulation M4)\n");
	TestPoseHolderInputPolicy();
	if(argc>1&&std::strcmp(argv[1],"--pose-holder-only")==0) {
		std::printf("\n%d passed, %d failed\n",passes,failures);
		return failures ? 1:0;
	}
	std::string error;
	if (!RegisterBuiltinSolvers(error)) std::printf("note: %s\n", error.c_str());
	const fs::path root = RISE::TestSupport::ScratchRunDir("rise_simulation_scene_", "suite");
	std::error_code ec;
	fs::create_directories(root, ec);
	fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ec);
	// Scenes without `cache` bake into the per-user default: keep every run out of the real one
	// (run_all_tests.sh already points it at a per-test directory).
	if (!std::getenv("RISE_SIMULATION_CACHE_DIR")) SetOverride((root/"user-cache").string().c_str());
	TestSchemaDefaults();
	TestBakeEquivalence(root);
	TestKeySensitivity();
	TestSceneInputs(root);
	TestChain(root);
	TestEditsAndLegality(root);
	TestFire(root);
	TestSimulationCache(root);
	TestEditorAndAgent(root, fs::current_path());
	TestEstimates(root);
	TestDefaultCache(root);
	fs::remove_all(RISE::TestSupport::ScratchTestRoot("rise_simulation_scene_"), ec);
	std::printf("\n%d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
