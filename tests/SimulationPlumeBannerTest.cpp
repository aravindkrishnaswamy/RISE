// Simulation M6: one-way coupling from a fire CACHE to a cloth banner (no fire
// solver runs).  The banner above the plume is loaded by the aerodynamic_drag
// adapter from the cache's velocity (cm/s) and its temperature (density by the
// ideal gas law at ambient pressure, molar mass of air) through the group's
// cached member (CachedPortFeed).  Preview (the drag model is not validated).
// Also: the banner's key covers the fire cache's key and the adapter's
// identity (the M4 gap), and the flow lifts the banner against a no-flow run.
//
// The cache: RISE_M6_FIRE_CACHE, else $HOME/.cache/rise-sim-m2/t8_accept (the
// M2 acceptance bake), opened read-only.  Without one the test says so and passes.
#include "../src/Library/Simulation/SimulationGroup.h"
#include "../src/Library/Simulation/SimulationPortAdapters.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Simulation/SimulationGrid.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/Cloth/ClothSolver.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
using namespace RISE::Simulation;
using V = RISE::RISECBOR64::Value;
constexpr std::size_t Budget = std::size_t(1) << 30;
int failures = 0;
void Check(bool c, const std::string& what, const std::string& detail = {})
{
	if (!c) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Float(std::uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }

double CentreHeight(const IReadOnlyCacheSnapshot& s, std::int64_t index)
{
	std::shared_ptr<const Bytes> bytes;
	std::string error;
	MeshFrame m;
	if (s.Read(index, "surface", Budget, bytes, error) != FrameStatus::Ready || !DecodeMesh(*bytes, Budget, s.Resolver(), m, error)) return NAN;
	// The centre vertex of the 11 x 11 banner.
	return m.positions.size() == 3*121 ? Float(m.positions[3*60 + 1]) : NAN;
}
}

int main()
{
	std::string error;
	Check(RegisterBuiltinSolvers(error), "register", error);
	const char* env = std::getenv("RISE_M6_FIRE_CACHE");
	const char* home = std::getenv("HOME");
	const std::filesystem::path root = env ? env : std::string(home ? home : "") + "/.cache/rise-sim-m2/t8_accept";
	auto fire = std::make_shared<CacheSnapshot>();
	if (!CacheSnapshot::Open(root, std::size_t(256) << 20, *fire, error)) {
		std::cout << "SimulationPlumeBannerTest: no fire cache at " << root << " (" << error << "): skipped\n";
		return 0;
	}
	const auto& d = fire->Description();
	std::cout << "  fire cache " << root << ": producer " << d.producer << ", " << fire->EntryCount() << " frames, start " << d.startTime
		<< " s, interval " << d.interval << " s, fidelity " << d.fidelity << "\n";
	// The plume: the grid's box and the column of the fastest updraft in the first frame.
	std::int64_t first = -1;
	double firstTime = 0;
	for (const auto& e : fire->Entries()) if (e.output == "volume" && (first < 0 || e.index < first)) { first = e.index; firstTime = e.time; }
	std::shared_ptr<const Bytes> bytes;
	GridFrame g;
	Check(first >= 0 && fire->Read(first, "volume", Budget, bytes, error) == FrameStatus::Ready && DecodeGrid(*bytes, Budget, g, error),
		"read a fire frame", error);
	const GridChannel* vel = nullptr;
	for (const auto& c : g.channels) {
		std::cout << "    channel " << c.name << " [" << c.units << "] x" << int(c.components) << ", dims " << c.dimensions[0] << " x "
			<< c.dimensions[1] << " x " << c.dimensions[2] << ", " << c.blocks.size() << " blocks\n";
		if (c.name == "velocity") vel = &c;
	}
	Check(vel && vel->components == 3, "a velocity channel");
	if (!vel) return 1;
	const auto& m = vel->indexToWorld;
	double best = -1, bx = 0, bz = 0, ylo = 1e30, yhi = -1e30, sw = 0, sx = 0, sz = 0;
	for (const auto& b : vel->blocks) for (std::uint32_t k = 0; k < b.dimensions[2]; ++k) for (std::uint32_t j = 0; j < b.dimensions[1]; ++j) {
		const double I[3] = {double(b.origin[0]), double(b.origin[1] + int(j)), double(b.origin[2] + int(k))};
		const double y = m[4]*I[0] + m[5]*I[1] + m[6]*I[2] + m[7];
		ylo = std::min(ylo, y); yhi = std::max(yhi, y);
	}
	// The banner's height: 55% of the grid; the plume axis: the fastest updraft within a cell of that height.
	const double spacing = std::fabs(m[5]) + std::fabs(m[4]) + std::fabs(m[6]);
	const double y = ylo + 0.55*(yhi - ylo), size = 0.4;
	for (const auto& b : vel->blocks) {
		for (std::uint32_t k = 0; k < b.dimensions[2]; ++k) for (std::uint32_t j = 0; j < b.dimensions[1]; ++j) for (std::uint32_t i = 0; i < b.dimensions[0]; ++i) {
			const std::size_t cell = b.constant ? 0 : i + b.dimensions[0]*(j + std::size_t(b.dimensions[1])*k);
			const double vy = Float(b.values[3*cell + 1]);
			const double I[3] = {double(b.origin[0] + int(i)), double(b.origin[1] + int(j)), double(b.origin[2] + int(k))};
			const double yc = m[4]*I[0] + m[5]*I[1] + m[6]*I[2] + m[7];
			if (std::fabs(yc - y) > spacing) continue;
			if (vy > best) best = vy;
			if (vy > 0) {   // the plume axis: the centroid of the updraft's momentum flux at that height
				const double w = vy*vy;
				sw += w; sx += w*(m[0]*I[0] + m[1]*I[1] + m[2]*I[2] + m[3]); sz += w*(m[8]*I[0] + m[9]*I[1] + m[10]*I[2] + m[11]);
			}
		}
	}
	if (sw > 0) { bx = sx/sw; bz = sz/sw; }
	std::cout << "    banner height " << y << " m: fastest updraft there " << best << " " << vel->units << ", updraft centroid x " << bx << ", z " << bz
		<< "; grid height " << ylo << " .. " << yhi << " m\n";
	auto banner = [&](bool) {
		return V::MapValue({{"mode", V::String("bake")}, {"vertices_u", V::Unsigned(11)}, {"vertices_v", V::Unsigned(11)},
			{"size_u", V::Float(size)}, {"size_v", V::Float(size)}, {"origin", Vec(bx - size/2, y, bz + size/2)},
			{"frames", V::Unsigned(13)}, {"material", V::String("silk-lightweight")},
			{"pins", V::ArrayValue({V::MapValue({{"select", V::String("u0")}}), V::MapValue({{"select", V::String("u1")}})})}});
	};
	GroupPolicy p;   // explicit: the cache is fixed, nothing to iterate
	auto build = [&](SimulationGroup& group, const V& drag) {
		PortConnection c;
		c.fromMember = "fire"; c.fromPort = "volume"; c.toMember = "banner"; c.toPort = "surface_load";
		c.adapter = AerodynamicDragAdapterId;
		c.adapterParameters = drag;
		c.auxiliary = {{"banner", "surface_state"}};
		return group.AddCachedMember("fire", fire, true, error) && group.SetCachedTimeOffset("fire", firstTime, error) &&
			group.AddMember("banner", CreateSolver(ClothSolverId, error), banner(true), error) && group.Connect(c, error) &&
			group.Configure(p, EmptySceneInputs(), error);
	};
	const V drag = V::MapValue({{"temperature_channel", V::String("temperature")}});
	SimulationGroup run;
	double upward = 0;   // the served vertical traction, averaged over vertices and steps
	int steps = 0;
	run.SetStepObserver([&](double, const SimulationGroup& g) {
		FieldFrame f;
		std::string e;
		if (!g.ServedValue(0) || !DecodeField(*g.ServedValue(0), Budget, f, e)) return;
		double ty = 0;
		for (std::size_t k = 1; k < f.channels[0].values.size(); k += 3) ty += Float(f.channels[0].values[k]);
		upward += ty/static_cast<double>(f.channels[0].values.size()/3);
		++steps;
	});
	const auto start = std::chrono::steady_clock::now();
	Check(build(run, drag) && run.Start(error) && run.Run(error), "the banner over the cached plume", error);
	upward /= std::max(1, steps);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	Check(run.MemberFidelity("banner") == "preview" && !run.CouplingLimit("banner").empty(),
		"a drag model through a port is preview", run.CouplingLimit("banner"));
	{
		// Implicit would not make it predictive either: the adapter is a model.
		SimulationGroup imp;
		GroupPolicy q; q.subIterations = 4; q.convergenceTolerance = 1e-6;
		PortConnection c;
		c.fromMember = "fire"; c.fromPort = "volume"; c.toMember = "banner"; c.toPort = "surface_load";
		c.adapter = AerodynamicDragAdapterId; c.adapterParameters = drag; c.auxiliary = {{"banner", "surface_state"}};
		Check(imp.AddCachedMember("fire", fire, true, error) && imp.AddMember("banner", CreateSolver(ClothSolverId, error), banner(true), error) &&
			imp.Connect(c, error) && imp.Configure(q, EmptySceneInputs(), error) && imp.MemberFidelity("banner") == "preview" &&
			imp.CouplingLimit("banner").find("model") != std::string::npos, "the drag model keeps an implicit coupling preview", imp.CouplingLimit("banner"));
	}
	// Keys: the drag coefficients and the fire cache enter the banner's key; the standalone banner differs.
	SimulationGroup other;
	Check(build(other, V::MapValue({{"temperature_channel", V::String("temperature")}, {"normal_coefficient", V::Float(2)}})),
		"configure with another coefficient", error);
	SolverDescription alone;
	Check(CreateSolver(ClothSolverId, error)->Configure(banner(false), EmptySceneInputs(), alone, error) &&
		PrepareCacheDescription(alone, EmptySceneInputs(), "", error), "standalone banner", error);
	Check(run.Description("banner")->cache.key != other.Description("banner")->cache.key &&
		run.Description("banner")->cache.key != alone.cache.key, "the cache feed and the adapter's identity enter the banner's key");
	// The flow lifts the banner against the same banner in still air.
	InProcessHost still(CreateSolver(ClothSolverId, error));
	Check(still.Configure(banner(false), EmptySceneInputs(), error) && still.Start(error) && still.StepToIndex(100, error), "still air", error);
	SimulationGroup ten;
	Check(build(ten, V::MapValue({{"temperature_channel", V::String("temperature")}, {"scale", V::Float(10)}})) && ten.Start(error) &&
		ten.Run(error), "ten times the drag", error);
	const double lifted = CentreHeight(*run.Snapshot("banner"), 12), sagged = CentreHeight(*still.Snapshot(), 12),
		strong = CentreHeight(*ten.Snapshot("banner"), 12);
	std::cout << "  mean vertical drag traction over the run " << upward << " Pa (silk weight 0.69 Pa); banner centre after 0.4 s: "
		"still air " << sagged << " m, plume " << lifted << " m, plume x10 " << strong << " m (" << seconds << " s)\n";
	Check(upward > 0, "the plume's drag on the banner points up on average");
	Check(std::isfinite(lifted) && lifted != sagged, "the cached plume changes the banner");
	Check(strong > sagged, "a ten-times drag lifts the banner's centre above its still-air sag");
	if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
	std::cout << "SimulationPlumeBannerTest: all checks passed\n";
	return 0;
}
