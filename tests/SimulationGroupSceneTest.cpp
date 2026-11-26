// Simulation M6, scene side: `rigid_simulation` (generated from the rigid
// schema), `simulation_group`, `port_source` and the cloth's `collider_port`;
// planning (coupled keys and labels, group state), `rise-sim bake <scene>
// <group>` equal to the same group driven programmatically, staleness through
// a coupling and through a cached port feed (the M4 gap), stop / resume of a
// group bake, round trip of the sample scene.
#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Simulation/SceneSimulation.h"
#include "../src/Library/Simulation/SimulationGroup.h"
#include "../src/Library/Simulation/SimulationWorker.h"
#include "../src/Library/Simulation/SceneSimulationWorker.h"
#include "TestTempDir.h"

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace {
using namespace RISE;
using namespace RISE::Simulation;
namespace fs = std::filesystem;
using V = RISECBOR64::Value;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << (detail.empty() ? "" : " -- " + detail) << "\n"; }
}
void WriteText(const fs::path& path, const std::string& text) { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << text; }
int Worker(const std::vector<std::string>& args, std::string* output = nullptr)
{
	std::ostringstream out, err;
	const int code = WorkerMain(args, out, err);
	if (output) *output = out.str() + err.str();
	return code;
}
struct Scene {
	IJobPriv* job = nullptr;
	~Scene() { if (job) job->release(); }
	Job* J() const { return dynamic_cast<Job*>(job); }
};
bool Load(const fs::path& path, Scene& s) { return RISE_CreateJobPriv(&s.job) && s.job && s.job->LoadAsciiSceneViaCst(path.string().c_str()); }
std::vector<SceneSimulationStatus> Statuses(Scene& s)
{
	std::vector<SceneSimulationStatus> out;
	std::string error;
	if (!s.J() || !s.J()->DescribeSimulations(out, error)) Check(false, "describe", error);
	return out;
}
const SceneSimulationStatus* Find(const std::vector<SceneSimulationStatus>& all, const std::string& n)
{
	for (const auto& s : all) if (s.name == n) return &s;
	return nullptr;
}
std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const IReadOnlyCacheSnapshot& s)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> out;
	for (const auto& e : s.Entries()) out[{e.index, e.output}] = e.digest;
	return out;
}
std::map<std::pair<std::int64_t,std::string>,std::string> DiskDigests(const fs::path& dir)
{
	CacheSnapshot s;
	std::string error;
	if (!CacheSnapshot::Open(dir, std::size_t(64) << 20, s, error)) return {};
	return Digests(s);
}

std::string SceneText(const std::string& mass, const std::string& group, const std::string& sheetExtra = "")
{
	return "RISE ASCII SCENE 7\n"
		"pinhole_camera\n{\n\tlocation\t1.2 0.8 1.6\n\tlookat\t0 0 0\n\tup\t0 1 0\n\tfov\t40\n}\n"
		"sphere_geometry\n{\n\tname\tgeo_ball\n\tradius\t0.1\n}\n"
		"standard_object\n{\n\tname\tball\n\tgeometry\tgeo_ball\n\tposition\t0 0.15 0\n}\n"
		"rigid_simulation\n{\n\tname\tdrop\n\tframes\t7\n\tbody_object\tball mass " + mass + "\n}\n"
		"cloth_simulation\n{\n\tname\tsheet\n\tsize_u\t0.6\n\tsize_v\t0.6\n\tvertices_u\t9\n\tvertices_v\t9\n\tframes\t7\n"
		"\torigin\t-0.3 0 0.3\n\tpin\tu0v0\n\tpin\tu1v0\n\tpin\tu0v1\n\tpin\tu1v1\n\tcollider_port\tball\n" + sheetExtra + "}\n" + group;
}
const char* Group =
	"simulation_group\n{\n\tname\ttramp\n\tmembers\tdrop\n\tmembers\tsheet\n"
	"\tconnect\tdrop.body_state -> sheet.collider_surface via rigid_surface body=ball\n"
	"\tconnect\tsheet.collider_reaction -> drop.body_load via contact_reaction targets=ball\n"
	"\tpolicy\timplicit\n\tmember_order\tdrop sheet\n\ttolerance\t1e-6\n\tabsolute_tolerance\t1e-4\n}\n";
}

int main()
{
	std::string error;
	Check(RegisterBuiltinSolvers(error), "register", error);
	const fs::path root = RISE::TestSupport::ScratchRunDir("rise_sim_group_scene_", "suite");
	std::error_code ec;
	fs::create_directories(root, ec);
	fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ec);
	// ---------------------------------------------------------------- plan
	const fs::path scene = root/"tramp.RISEscene";
	WriteText(scene, SceneText("0.05", Group));
	std::string groupKey, sheetKey, dropKey;
	{
		Scene s;
		Check(Load(scene, s), "the group scene loads");
		const auto st = Statuses(s);
		const auto *drop = Find(st, "drop"), *sheet = Find(st, "sheet"), *tramp = Find(st, "tramp");
		Check(drop && sheet && tramp, "three simulations");
		if (!drop || !sheet || !tramp) return 1;
		Check(drop->keyword == "rigid_simulation" && drop->solverId == "rigid.bodies", "rigid_simulation chunk", drop->error);
		Check(tramp->keyword == "simulation_group" && tramp->state == SceneSimulationState::Missing, "the group is missing", tramp->error);
		Check(tramp->upstream == std::vector<std::string>({"drop", "sheet"}), "the group depends on its members");
		Check(!drop->expectedKey.empty() && !sheet->expectedKey.empty(), "coupled keys planned", sheet->error);
		dropKey = drop->expectedKey; sheetKey = sheet->expectedKey;
		// The coupled key differs from the standalone one.
		WriteText(root/"alone.RISEscene", SceneText("0.05", ""));
		Scene a;
		Check(Load(root/"alone.RISEscene", a), "standalone scene");
		const auto sa = Statuses(a);
		Check(Find(sa, "drop") && Find(sa, "drop")->expectedKey != dropKey, "the group's coupling enters the member keys");
		// The descriptor: generated from the schema.
		std::vector<SceneSimulationSpec> specs = s.J()->SimulationSpecs();
		Check(specs.size() == 3 && specs[2].parameters.Find("connect") && specs[2].parameters.Find("connect")->GetArray().size() == 2,
			"the group's fields are recorded");
	}
	// ---------------------------------------------------------------- bake = programmatic group
	{
		std::string out;
		const int code = Worker({"bake", scene.string(), "tramp"}, &out);
		Check(code == WorkerFinished, "rise-sim bake <scene> <group>", out);
		Scene s;
		Check(Load(scene, s), "reload");
		const auto st = Statuses(s);
		Check(Find(st, "tramp") && Find(st, "tramp")->state == SceneSimulationState::Complete, "the group is complete");
		Check(Find(st, "sheet") && Find(st, "sheet")->state == SceneSimulationState::Complete && Find(st, "sheet")->expectedKey == sheetKey,
			"members complete with their coupled keys");
		Check(fs::exists(root/"caches"/"sheet"/kGroupManifestName) && fs::exists(root/"caches"/"drop"/kGroupManifestName),
			"each member cache carries the group manifest");
		// The same group, driven programmatically from the plans, in memory.
		std::vector<SceneSimulationPlan> plans;
		std::vector<SceneSimulationSpec> specs;
		Check(s.J()->PlanSimulations(plans, specs, error), "plan", error);
		const SceneSimulationSpec* g = nullptr;
		for (const auto& x : specs) if (x.name == "tramp") g = &x;
		SimulationGroup group;
		Check(g && BuildSceneGroup(*g, plans, group, error) && group.Start(error) && group.Run(error), "programmatic group", error);
		Check(DiskDigests(root/"caches"/"sheet").size() == 7 && DiskDigests(root/"caches"/"drop").size() == 7, "seven frames each");
		Check(Digests(*group.Snapshot("sheet")) == DiskDigests(root/"caches"/"sheet") &&
			Digests(*group.Snapshot("drop")) == DiskDigests(root/"caches"/"drop"), "the scene bake equals the programmatic group byte for byte");
		Check(Worker({"bake", scene.string(), "sheet"}, &out) == WorkerFinished && out.find("up to date") != std::string::npos,
			"naming a member bakes (here: skips) its group", out);
	}
	// ---------------------------------------------------------------- staleness through the coupling
	{
		WriteText(scene, SceneText("0.08", Group));
		Scene s;
		Check(Load(scene, s), "edited scene");
		const auto st = Statuses(s);
		Check(Find(st, "drop")->state == SceneSimulationState::Stale && Find(st, "sheet")->state == SceneSimulationState::Stale &&
			Find(st, "tramp")->state == SceneSimulationState::Stale, "editing one member makes both members and the group stale");
		// Stop after two macro steps, then resume: equal to an uninterrupted bake.
		std::string out;
		Check(Worker({"bake", scene.string(), "tramp", "--stop-after-frames", "2"}, &out) == WorkerStopped, "stopped bake", out);
		Check(fs::exists(root/"caches"/"tramp"/"rise-sim.group.checkpoint"), "a group checkpoint");
		Check(Worker({"resume", scene.string(), "tramp"}, &out) == WorkerFinished, "resume the group", out);
		const auto resumed = DiskDigests(root/"caches"/"sheet");
		Check(resumed.size() == 7, "the resumed bake has every frame");
		Check(Worker({"bake", scene.string(), "tramp", "--force"}, &out) == WorkerFinished, "uninterrupted rebake", out);
		Check(resumed == DiskDigests(root/"caches"/"sheet"), "stop / resume of a scene group bake equals the uninterrupted bake");
	}
	// ---------------------------------------------------------------- port_source from a cache (the M4 gap)
	{
		const fs::path one = root/"oneway";
		fs::create_directories(one, ec);
		const std::string feed = "\tport_source\tcollider_surface from drop.bodies via rigid_surface body=ball\n";
		WriteText(one/"s.RISEscene", SceneText("0.05", "", feed));
		std::string out;
		Check(Worker({"bake", (one/"s.RISEscene").string(), "sheet"}, &out) == WorkerFinished, "bake a port_source sheet (drop first)", out);
		Scene s;
		Check(Load(one/"s.RISEscene", s), "load");
		auto st = Statuses(s);
		Check(Find(st, "sheet")->state == SceneSimulationState::Complete && Find(st, "sheet")->upstream == std::vector<std::string>({"drop"}) ,
			"complete; the port source is an upstream", Find(st, "sheet")->error);
		const std::string before = Find(st, "sheet")->expectedKey;
		WriteText(one/"s.RISEscene", SceneText("0.08", "", feed));
		Check(Worker({"bake", (one/"s.RISEscene").string(), "drop"}, &out) == WorkerFinished, "rebake the upstream", out);
		Scene t;
		Check(Load(one/"s.RISEscene", t), "load");
		st = Statuses(t);
		Check(Find(st, "sheet")->state == SceneSimulationState::Stale && Find(st, "sheet")->expectedKey != before,
			"a cache-fed port enters the downstream key: an upstream change makes it stale");
	}
	// ---------------------------------------------------------------- the sample scene round-trips
	{
		const fs::path sample = fs::current_path()/"scenes"/"Tests"/"Simulation"/"rigid_cloth_trampoline.RISEscene";
		const fs::path copy = root/"sample.RISEscene";
		fs::copy_file(sample, copy, fs::copy_options::overwrite_existing, ec);
		Scene a;
		Check(Load(copy, a) && a.J()->GetCstDocument(), "the sample scene loads");
		if (a.J() && a.J()->GetCstDocument()) {
			WriteText(root/"sample_saved.RISEscene", RISE::Cst::SerializeCst(*a.J()->GetCstDocument()));
			Scene b;
			Check(Load(root/"sample_saved.RISEscene", b), "the saved sample loads");
			const auto sa = a.J()->SimulationSpecs(), sb = b.J()->SimulationSpecs();
			bool same = sa.size() == 3 && sa.size() == sb.size();
			for (std::size_t i = 0; same && i < sa.size(); ++i) {
				RISECBOR64::Bytes x, y;
				RISECBOR64::Encode(sa[i].parameters, x, nullptr);
				RISECBOR64::Encode(sb[i].parameters, y, nullptr);
				same = sa[i].name == sb[i].name && sa[i].keyword == sb[i].keyword && x == y && sa[i].portSources == sb[i].portSources;
			}
			Check(same, "load -> save -> load keeps the rigid simulation, the cloth and the group");
		}
	}
	fs::remove_all(RISE::TestSupport::ScratchTestRoot("rise_sim_group_scene_"), ec);
	if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
	std::cout << "SimulationGroupSceneTest: all checks passed\n";
	return 0;
}
