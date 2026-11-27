// cloth.shell on the solver contract (simulation M3a): description,
// capabilities, ports, computed evidence and labels; effective parameters a
// fixed point; frames (fixed topology, velocities, normals, pins, contact);
// stop/checkpoint/restore byte-identical in-process and through the worker;
// in-process and worker hosts publish identical frames; imposed macro steps on
// the output grid equal free stepping; a constant pressure through the
// surface_load port (field.v1 on the sheet's vertices) equals the same
// pressure as a parameter; mesh.v2 output with a static topology and
// material UVs; sticky stops; rollback equal to a re-run; pins and colliders
// driven by scene transforms; evidence conditions and scope.
#include "../src/Library/Simulation/Cloth/ClothSolver.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationWorker.h"
#include "TestTempDir.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace {
using namespace RISE::Simulation;
using V = RISE::RISECBOR64::Value;
namespace fs = std::filesystem;
constexpr std::size_t budget = std::size_t(64) << 20;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}

double Seconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

V Map(V::Members m) { return V::MapValue(m); }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }

// A small hanging sheet: 9 x 9 vertices pinned at two corners, 10 frames at 30 Hz.
V::Members Small()
{
	return {{"vertices_u", V::Unsigned(9)}, {"vertices_v", V::Unsigned(9)}, {"size_u", V::Float(0.4)}, {"size_v", V::Float(0.4)},
		{"frames", V::Unsigned(10)}, {"substeps", V::Unsigned(4)},
		{"pins", V::ArrayValue({Map({{"select", V::String("u0v0")}}), Map({{"select", V::String("u1v0")}})})}};
}
V With(V::Members m, const std::string& key, const V& value)
{
	for (auto& p : m) if (p.first == key) { p.second = value; return V::MapValue(m); }
	m.emplace_back(key, value);
	return V::MapValue(m);
}

std::unique_ptr<ISimulationSolver> Make()
{
	std::string error;
	auto s = CreateSolver(ClothSolverId, error);
	Check(s != nullptr, "registry creates cloth.shell", error);
	return s;
}

std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const IReadOnlyCacheSnapshot& s)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> out;
	for (const auto& e : s.Entries()) out[{e.index, e.output}] = e.digest;
	return out;
}

class CountingSink final : public IFrameSink {
public:
	std::vector<std::pair<std::int64_t,std::shared_ptr<const Bytes>>> frames;
	std::map<std::string,std::shared_ptr<const Bytes>> statics;
	bool Publish(std::int64_t index, const std::string&, double, std::shared_ptr<const Bytes> payload,
		const Bytes&, std::string&) override { frames.emplace_back(index, std::move(payload)); return true; }
	bool PublishStatic(const std::string&, std::shared_ptr<const Bytes> payload, std::string& digest, std::string&) override {
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

double F(std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

// A cube of side 0.2 m under the small sheet's fall (x -0.4..-0.2, y 0.7..0.9,
// z 0.2..0.4) as a mesh collider, optionally rotating about its vertical axis.
V CubeCollider(bool rotating)
{
	V::Values vs, ts;
	for (int k = 0; k < 8; ++k) {
		vs.push_back(V::Float((k & 1) ? -0.2 : -0.4));
		vs.push_back(V::Float((k & 2) ? 0.9 : 0.7));
		vs.push_back(V::Float((k & 4) ? 0.4 : 0.2));
	}
	const unsigned faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
		{2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
	for (const auto& f : faces) for (unsigned k : f) ts.push_back(V::Unsigned(k));
	V::Members m{{"type", V::String("mesh")}, {"vertices", V::ArrayValue(vs)}, {"triangles", V::ArrayValue(ts)}};
	if (rotating) m.emplace_back("motion", Map({{"type", V::String("rotate")}, {"center", Vec(-0.3, 0.8, 0.3)},
		{"axis", Vec(0, 1, 0)}, {"rate", V::Float(2.0)}}));
	return V::ArrayValue({V::MapValue(m)});
}

// Scene inputs offering one mesh.v1 frame named "cube".
class CubeInput final : public ISceneInputs {
public:
	bool Identity(const std::string& name, std::string& hash, std::string& error) const override {
		if (name != "cube") { error = "no such input"; return false; }
		hash = "cube-identity";
		return true;
	}
	bool Sample(const std::string& name, double, std::shared_ptr<const Bytes>& encoded, std::string& codec,
		std::string& error) const override {
		if (name != "cube") { error = "no such input"; return false; }
		MeshFrame m;
		for (int k = 0; k < 8; ++k) for (double c : {(k & 1) ? -0.2 : -0.4, (k & 2) ? 0.9 : 0.7, (k & 4) ? 0.4 : 0.2}) {
			const float f = static_cast<float>(c);
			std::uint32_t bits;
			std::memcpy(&bits, &f, 4);
			m.positions.push_back(bits);
		}
		m.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
		auto bytes = std::make_shared<Bytes>();
		if (!EncodeMesh(m, budget, *bytes, error)) return false;
		encoded = std::move(bytes);
		codec = "mesh.v1";
		return true;
	}
};

// A scene input whose cube grows by 10% per second about its centre: a deforming collider.
class GrowingCube final : public ISceneInputs {
public:
	bool Identity(const std::string& name, std::string& hash, std::string& error) const override {
		if (name != "cube") { error = "no such input"; return false; }
		hash = "growing-cube";
		return true;
	}
	bool Sample(const std::string& name, double t, std::shared_ptr<const Bytes>& encoded, std::string& codec,
		std::string& error) const override {
		if (name != "cube") { error = "no such input"; return false; }
		MeshFrame m;
		const double c[3] = {-0.3, 0.8, 0.3}, s = 1 + 0.1*t;
		for (int k = 0; k < 8; ++k) {
			const double p[3] = {(k & 1) ? -0.2 : -0.4, (k & 2) ? 0.9 : 0.7, (k & 4) ? 0.4 : 0.2};
			for (int d = 0; d < 3; ++d) {
				const float f = static_cast<float>(c[d] + s*(p[d] - c[d]));
				std::uint32_t bits;
				std::memcpy(&bits, &f, 4);
				m.positions.push_back(bits);
			}
		}
		m.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
		auto bytes = std::make_shared<Bytes>();
		if (!EncodeMesh(m, budget, *bytes, error)) return false;
		encoded = std::move(bytes);
		codec = "mesh.v1";
		return true;
	}
};

void Description()
{
	std::string error;
	NoSceneInputs inputs;
	SolverDescription d;
	auto solver = Make();
	const double t0 = Seconds();
	Check(solver->Configure(Map(Small()), inputs, d, error), "cloth configures", error);
	const double first = Seconds() - t0;
	Check(d.solverId == ClothSolverId && d.version == "m3a.5" && d.cache.outputs.size() == 1 &&
		d.cache.outputs[0].name == "surface" && d.cache.outputs[0].type == "mesh.v2" && d.cache.count == 10,
		"cloth describes one mesh.v2 output (version m3a.5)");
	const PortDeclaration* offer = nullptr; const PortDeclaration* accept = nullptr;
	for (const auto& p : d.ports) {
		if (p.name == "surface_state") offer = &p;
		if (p.name == "surface_load") accept = &p;
	}
	std::vector<std::uint32_t> indices;
	// M7e: surface_load gained a per-vertex force channel, and the cloth offers its
	// mechanics and stiffness and accepts a partner's steps (five ports); M7h: and its
	// corotational model's elements (six).
	Check(d.ports.size() == 6 && offer && accept && offer->offers && offer->output == "surface" &&
		offer->codec == "mesh.v2" && !accept->offers && accept->codec == "field.v1" && accept->units == "Pa" &&
		accept->channels.size() == 3 && accept->channels[0].name == "force" && accept->channels[0].units == "N" &&
		accept->channels[1].name == "pressure" && accept->channels[2].components == 3 &&
		accept->domain.size() == 64 && accept->output.empty(),
		"cloth offers surface_state and accepts a live-only field.v1 surface_load on its own vertices");
	const auto& c = d.capabilities;
	Check(c.deterministic && c.resumable && c.externalMacroStep && c.interactive && c.rollback && c.threadCountInvariant &&
		c.backends.size() == 1 && c.backends[0].name == "cpu" && c.backends[0].status == BackendStatus::Supported &&
		c.backends[0].available && c.modes == std::vector<std::string>{"bake", "interactive"} &&
		c.earnableFidelity == std::vector<std::string>{"preview", "predictive"} && c.fidelityLimit.empty(),
		"cloth capabilities");
	bool bakeCondition = false;
	for (const auto& k : d.evidence.conditions) bakeCondition = bakeCondition || (k.first == "mode" && k.second == std::vector<std::string>{"bake"});
	Check(bakeCondition && !d.evidence.scope.empty() && ValidEvidenceConditions(d.evidence.conditions),
		"cloth states the conditions and scope of its label");
	std::printf("evidence (bake, cotton, alternating, lines, bdf2), first Configure %.2f s:\n", first);
	bool all = !d.evidence.cases.empty();
	for (const auto& e : d.evidence.cases) {
		std::printf("  %-20s error %.3e tolerance %.2e %s\n", e.id.c_str(), e.error, e.tolerance, e.passed ? "pass" : "FAIL");
		all = all && e.passed && e.error <= e.tolerance && e.digest.size() == 64;
	}
	Check(all && d.evidence.fidelity == "predictive" && EffectiveFidelity(d.evidence) == "predictive",
		"bake mode earns predictive from computed evidence");
	// Effective parameters are a fixed point and reproduce the key; pace is not keyed.
	SolverDescription again, paced;
	auto other = Make();
	Check(PrepareCacheDescription(d, inputs, "", error) && other->Configure(d.effectiveParameters, inputs, again, error) &&
		PrepareCacheDescription(again, inputs, "", error) && again.cache.key == d.cache.key &&
		other->Configure(With(Small(), "pace_ms", V::Unsigned(2)), inputs, paced, error) &&
		PrepareCacheDescription(paced, inputs, "", error) && paced.cache.key == d.cache.key,
		"effective parameters are a fixed point; pace is not keyed", error);
	Bytes a, b;
	RISE::RISECBOR64::Encode(d.effectiveParameters, a, &error);
	RISE::RISECBOR64::Encode(again.effectiveParameters, b, &error);
	Check(a == b && again.evidence.cases.size() == d.evidence.cases.size(), "reconfigured description matches");
	Check(!other->Configure(With(Small(), "no_such_parameter", V::Unsigned(1)), inputs, again, error) &&
		error.find("unknown parameter") != std::string::npos, "unknown parameters refused", error);
	Check(!other->Configure(With(Small(), "material", V::String("unobtainium")), inputs, again, error), "unknown material refused");
	Check(!other->Configure(With(With(Small(), "mode", V::String("interactive")).GetMap(), "integrator", V::String("bdf2")),
		inputs, again, error), "interactive mode refuses a non-backward-Euler integrator");
	// Labels: interactive, penalty contact and the hinge bending model are preview;
	// bake-mode barrier contact earns predictive from its own evidence cases, with
	// self-collision from its self-collision cases too (m3a.4).
	SolverDescription interactive, contact, barrier, self, interactiveContact, hinges, midpoint;
	Check(other->Configure(With(Small(), "mode", V::String("interactive")), inputs, interactive, error) &&
		interactive.evidence.fidelity == "preview" && EffectiveFidelity(interactive.evidence) == "preview" &&
		interactive.capabilities.earnableFidelity == std::vector<std::string>{"preview"} &&
		!interactive.capabilities.fidelityLimit.empty(), "interactive mode is preview and says why", error);
	const V sphere = V::ArrayValue({Map({{"type", V::String("sphere")}, {"center", Vec(0, 0, 0)}, {"radius", V::Float(0.1)}})});
	Check(other->Configure(With(With(Small(), "colliders", sphere).GetMap(), "contact", V::String("penalty")), inputs, contact, error) &&
		EffectiveFidelity(contact.evidence) == "preview", "penalty contact is preview", error);
	const double tb = Seconds();
	Check(other->Configure(With(Small(), "colliders", sphere), inputs, barrier, error) && barrier.evidence.fidelity == "predictive" &&
		EffectiveFidelity(barrier.evidence) == "predictive" && barrier.evidence.cases.size() == d.evidence.cases.size() + 6,
		"bake-mode barrier contact earns predictive from its contact cases", error);
	std::printf("contact evidence (barrier defaults), first Configure %.2f s:\n", Seconds() - tb);
	for (const auto& e : barrier.evidence.cases)
		if (e.id.compare(0, 8, "contact-") == 0)
			std::printf("  %-22s error %.3e tolerance %.2e %s\n", e.id.c_str(), e.error, e.tolerance, e.passed ? "pass" : "FAIL");
	const double ts = Seconds();
	Check(other->Configure(With(With(Small(), "colliders", sphere).GetMap(), "self_collision", V::Bool(true)), inputs, self, error) &&
		self.evidence.fidelity == "predictive" && EffectiveFidelity(self.evidence) == "predictive" &&
		self.evidence.cases.size() == d.evidence.cases.size() + 10, "bake-mode self-collision earns predictive from its self-collision cases", error);
	std::printf("self-collision evidence (defaults), first Configure %.2f s:\n", Seconds() - ts);
	for (const auto& e : self.evidence.cases)
		if (e.id.compare(0, 5, "self-") == 0)
			std::printf("  %-22s error %.3e tolerance %.2e %s\n", e.id.c_str(), e.error, e.tolerance, e.passed ? "pass" : "FAIL");
	SolverDescription selfFriction;
	Check(other->Configure(With(With(With(Small(), "colliders", sphere).GetMap(), "self_collision", V::Bool(true)).GetMap(),
		"self_friction", V::Float(0)), inputs, selfFriction, error) && EffectiveFidelity(selfFriction.evidence) == "preview",
		"self-collision without friction between layers fails its stick case and is preview", error);
	Check(other->Configure(With(With(Small(), "colliders", sphere).GetMap(), "mode", V::String("interactive")), inputs,
		interactiveContact, error) && EffectiveFidelity(interactiveContact.evidence) == "preview", "interactive contact is preview", error);
	Check(!other->Configure(With(With(Small(), "colliders", sphere).GetMap(), "integrator", V::String("implicit_midpoint")), inputs,
		again, error), "barrier contact refuses implicit midpoint");
	Check(!other->Configure(With(With(Small(), "colliders", CubeCollider(false)).GetMap(), "contact", V::String("penalty")), inputs,
		again, error), "mesh colliders refuse penalty contact");
	// A mesh collider: effective parameters are a fixed point; a scene-input mesh enters by name and identity.
	SolverDescription mesh, meshAgain, scene;
	Check(other->Configure(With(Small(), "colliders", CubeCollider(true)), inputs, mesh, error) &&
		other->Configure(mesh.effectiveParameters, inputs, meshAgain, error), "mesh collider configures", error);
	Bytes ma, mb;
	RISE::RISECBOR64::Encode(mesh.effectiveParameters, ma, &error);
	RISE::RISECBOR64::Encode(meshAgain.effectiveParameters, mb, &error);
	Check(ma == mb && EffectiveFidelity(mesh.evidence) == "predictive", "mesh collider parameters are a fixed point and predictive");
	CubeInput cube;
	Check(other->Configure(With(Small(), "colliders", V::ArrayValue({Map({{"type", V::String("mesh")},
		{"scene_input", V::String("cube")}})})), cube, scene, error), "scene-input mesh collider configures", error);
	const V* list = scene.effectiveParameters.Find("colliders");
	Check(list && list->GetArray().size() == 1 && list->GetArray()[0].Find("identity") &&
		list->GetArray()[0].Find("identity")->GetText() == "cube-identity" && scene.inputs == std::vector<std::string>{"cube"},
		"the scene input's identity enters the parameters and the input is listed");
	Check(EffectiveFidelity(scene.evidence) == "predictive", "a static scene-input mesh collider is predictive");
	// A deforming collider is followed as its t = 0 geometry moved rigidly: an
	// approximation the description states, labelled preview.
	GrowingCube growing;
	SolverDescription grows;
	Check(other->Configure(With(Small(), "colliders", V::ArrayValue({Map({{"type", V::String("mesh")},
		{"scene_input", V::String("cube")}})})), growing, grows, error) && grows.evidence.fidelity == "preview" &&
		EffectiveFidelity(grows.evidence) == "preview", "a deforming scene-input collider is preview", error);
	bool stated = false;
	for (const auto& n : grows.notes) stated = stated || (n.find("deforms during the run") != std::string::npos &&
		n.find("t = 0 geometry moved rigidly") != std::string::npos);
	Check(stated, "the description states the deforming-collider approximation");
	Check(other->Configure(With(Small(), "bending_model", V::String("hinges")), inputs, hinges, error) &&
		hinges.evidence.fidelity == "predictive" && EffectiveFidelity(hinges.evidence) == "preview",
		"the hinge bending model fails its evidence and is preview", error);
	for (const auto& e : hinges.evidence.cases)
		if (!e.passed) std::printf("  hinges: %-20s error %.3e tolerance %.2e FAIL (as documented)\n", e.id.c_str(), e.error, e.tolerance);
	Check(other->Configure(With(Small(), "integrator", V::String("implicit_midpoint")), inputs, midpoint, error) &&
		EffectiveFidelity(midpoint.evidence) == "predictive", "implicit midpoint bake earns predictive", error);
	// Every built-in record earns its evidence.
	for (const char* name : {"denim-twill", "silk-lightweight", "polyester-knit"}) {
		SolverDescription m;
		Check(other->Configure(With(Small(), "material", V::String(name)), inputs, m, error) &&
			EffectiveFidelity(m.evidence) == "predictive", std::string("built-in material ") + name + " earns predictive", error);
	}
}

void Frames()
{
	std::string error;
	NoSceneInputs inputs;
	auto solver = Make();
	SolverDescription d;
	CountingSink sink;
	AdvanceResult r;
	Check(solver->Configure(With(Small(), "colliders", V::ArrayValue({Map({{"type", V::String("sphere")},
		{"center", Vec(-0.3, 0.75, 0.3)}, {"radius", V::Float(0.1)}})})), inputs, d, error) && solver->Start(error),
		"sheet over a sphere starts", error);
	while (!solver->Position().finished && solver->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error)) {}
	Check(error.empty() && sink.frames.size() == 10, "publishes one frame per output index", error);
	std::string digest;
	bool fixed = true, moving = false, pinned = true, clear = true;
	double nearest = 1e9;
	MeshFrame first;
	for (std::size_t k = 0; k < sink.frames.size(); ++k) {
		MeshFrame m;
		MeshTopology t;
		if (!DecodeMesh(*sink.frames[k].second, budget, sink.Resolver(), m, error) ||
			!PeekMeshTopology(*sink.frames[k].second, t, error)) { fixed = false; break; }
		if (k == 0) { digest = t.digest; first = m; }
		fixed = fixed && t.digest == digest && t.vertices == 81 && t.triangles == 128 && m.velocities.size() == 243 && m.normals.size() == 243;
		for (std::size_t q = 0; q < m.velocities.size(); ++q) moving = moving || F(m.velocities[q]) != 0;
		for (std::uint32_t corner : {0u, 8u}) for (int c = 0; c < 3; ++c)
			pinned = pinned && m.positions[3*corner + c] == first.positions[3*corner + c];
		for (std::size_t q = 0; q < 81; ++q) {
			const double dx = F(m.positions[3*q]) + 0.3, dy = F(m.positions[3*q+1]) - 0.75, dz = F(m.positions[3*q+2]) - 0.3;
			const double d = std::sqrt(dx*dx + dy*dy + dz*dz);
			nearest = std::min(nearest, d);
			clear = clear && d > 0.1 + 0.002; // barrier: outside the 2 mm offset
		}
	}
	Check(fixed && moving && sink.statics.size() == 1, "fixed-topology stream with velocities and normals over one static topology");
	Check(first.uvSets.size() == 1 && first.uvSets[0].name == "material" && first.uvSets[0].units == "m" &&
		F(first.uvSets[0].values[0]) == 0 && F(first.uvSets[0].values[1]) == 0 &&
		std::fabs(F(first.uvSets[0].values[2*80]) - 0.4) < 1e-7 && std::fabs(F(first.uvSets[0].values[2*80 + 1]) - 0.4) < 1e-7,
		"the material coordinates are published as the UV set `material` in metres");
	Check(pinned, "pinned corners stay put");
	Check(clear && nearest < 0.1 + 0.002 + 0.001, "barrier contact keeps the sheet outside the sphere's offset (and touches it)",
		std::to_string(nearest));
	// Prescribed motion: a translating pin moves at its velocity.
	SolverDescription dm;
	CountingSink moved;
	auto mover = Make();
	V::Members p = Small();
	p.erase(std::remove_if(p.begin(), p.end(), [](const auto& m){ return m.first == "pins"; }), p.end());
	p.emplace_back("pins", V::ArrayValue({Map({{"select", V::String("v0")}, {"motion", Map({{"type", V::String("translate")},
		{"velocity", Vec(0.3, 0, 0)}})}})}));
	Check(mover->Configure(V::MapValue(p), inputs, dm, error) && mover->Start(error), "moving pins start", error);
	while (!mover->Position().finished && mover->Advance(AdvanceRequest{}, moved, nullptr, nullptr, r, error)) {}
	MeshFrame m0, m9;
	Check(DecodeMesh(*moved.frames.front().second, budget, moved.Resolver(), m0, error) &&
		DecodeMesh(*moved.frames.back().second, budget, moved.Resolver(), m9, error), "decode moving frames", error);
	const double shift = F(m9.positions[0]) - F(m0.positions[0]);
	Check(std::fabs(shift - 0.3*9/30.0) < 1e-6 && std::fabs(F(m9.velocities[0]) - 0.3) < 1e-6, "translating pins follow their motion",
		std::to_string(shift));
}

// Stop inside a segment, checkpoint, restore in a new instance, continue: byte-identical.
void StopRestore(const V& params, const std::string& what)
{
	std::string error;
	NoSceneInputs inputs;
	auto whole = Make(), first = Make(), second = Make();
	SolverDescription d0, d1, d2;
	CountingSink all, head, tail;
	AdvanceResult r;
	Check(whole->Configure(params, inputs, d0, error) && whole->Start(error), "reference run", error);
	while (!whole->Position().finished && whole->Advance(AdvanceRequest{}, all, nullptr, nullptr, r, error)) {}
	Check(first->Configure(params, inputs, d1, error) && first->Start(error) &&
		first->Advance(AdvanceRequest{}, head, nullptr, nullptr, r, error) &&
		first->Advance(AdvanceRequest{}, head, nullptr, nullptr, r, error), "two outputs", error);
	StopAfter stopper(3);
	Check(first->Advance(AdvanceRequest{}, head, nullptr, &stopper, r, error) && r.status == AdvanceResult::Status::Stopped &&
		head.frames.size() == 2, "stops mid-segment via progress");
	Bytes state;
	Check(first->Checkpoint(state, error) && second->Configure(params, inputs, d2, error) && second->Restore(state, error) &&
		second->Position().time == first->Position().time && second->Position().nextIndex == 2, "restores mid-segment", error);
	while (!second->Position().finished && second->Advance(AdvanceRequest{}, tail, nullptr, nullptr, r, error)) {}
	bool same = head.frames.size() + tail.frames.size() == all.frames.size();
	for (std::size_t i = 0; same && i < head.frames.size(); ++i) same = *head.frames[i].second == *all.frames[i].second;
	for (std::size_t i = 0; same && i < tail.frames.size(); ++i) same = *tail.frames[i].second == *all.frames[head.frames.size()+i].second;
	Check(same, what + ": stop and restore reproduces the uninterrupted frames byte for byte");
	Check(!second->Restore(Bytes{1, 2, 3}, error), "malformed checkpoint refused");
}

// The same frames, byte for byte, for any thread count (barrier contact with
// friction against a sphere and a rotating mesh).
void Threads()
{
	std::string error;
	NoSceneInputs inputs;
	V::Members base = Small();
	base.emplace_back("colliders", V::ArrayValue({Map({{"type", V::String("sphere")}, {"center", Vec(-0.1, 0.85, 0.25)},
		{"radius", V::Float(0.04)}}), CubeCollider(true).GetArray()[0]}));
	base.emplace_back("self_collision", V::Bool(true)); // cloth-cloth couplings in the matrix extension
	std::vector<std::vector<Bytes>> runs;
	for (unsigned threads : {1u, 3u, 4u}) {
		auto solver = Make();
		SolverDescription d;
		CountingSink sink;
		AdvanceResult r;
		Check(solver->Configure(With(base, "threads", V::Unsigned(threads)), inputs, d, error) && solver->Start(error),
			"threaded run " + std::to_string(threads), error);
		while (!solver->Position().finished && solver->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error)) {}
		Check(error.empty() && sink.frames.size() == 10, "threaded run finishes", error);
		std::vector<Bytes> frames;
		for (const auto& f : sink.frames) frames.push_back(*f.second);
		runs.push_back(frames);
		if (threads == 1) {
			Bytes a, b;
			SolverDescription d4;
			Check(solver->Configure(With(base, "threads", V::Unsigned(4)), inputs, d4, error), "reconfigure with threads", error);
			RISE::RISECBOR64::Encode(d.effectiveParameters, a, &error);
			RISE::RISECBOR64::Encode(d4.effectiveParameters, b, &error);
			Check(a == b, "the thread count does not enter the effective parameters");
		}
	}
	Check(runs[0] == runs[1] && runs[0] == runs[2], "1, 3 and 4 threads publish identical frames");
}

// A mesh ball (24 x 24 UV sphere, as sphere_geometry tessellates it) moving
// laterally under a pinned 25 x 25 sheet at the default 8 substeps: m3a.3
// failed Newton here at t = 0.33 s.  And a ball rising into a pinned edge: the
// run fails with the specific diagnostic.
V UVBall(double r, double cx, double cy, double cz, double vx, double vy)
{
	const double pi = 3.14159265358979323846;
	const int n = 24;
	V::Values vs, ts;
	std::vector<std::vector<unsigned>> rows(n + 1);
	unsigned north = 0, south = 0, count = 0;
	auto add = [&](double x, double y, double z) {
		vs.push_back(V::Float(cx + r*x)); vs.push_back(V::Float(cy + r*y)); vs.push_back(V::Float(cz + r*z));
		return count++;
	};
	for (int j = 0; j <= n; ++j) {
		const double phi = pi*j/n;
		if (j == 0 || j == n) { (j == 0 ? north : south) = add(-std::sin(phi), std::cos(phi), 0); continue; }
		for (int i = 0; i <= n; ++i) {
			const double th = 2*pi*i/n;
			rows[j].push_back(add(-std::sin(phi)*std::cos(th), std::cos(phi), std::sin(phi)*std::sin(th)));
		}
	}
	auto I = [&](int j, int i) { return j == 0 ? north : j == n ? south : rows[j][i]; };
	for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
		const unsigned a = I(j, i), b = I(j, i + 1), c = I(j + 1, i), d = I(j + 1, i + 1);
		if (j != 0) for (unsigned k : {a, c, b}) ts.push_back(V::Unsigned(k));
		if (j + 1 != n) for (unsigned k : {b, c, d}) ts.push_back(V::Unsigned(k));
	}
	return Map({{"type", V::String("mesh")}, {"vertices", V::ArrayValue(vs)}, {"triangles", V::ArrayValue(ts)},
		{"motion", Map({{"type", V::String("translate")}, {"velocity", Vec(vx, vy, 0)}})}});
}

void MovingColliders()
{
	std::string error;
	NoSceneInputs inputs;
	const double t0 = Seconds();
	{
		auto solver = Make();
		SolverDescription d;
		CountingSink sink;
		AdvanceResult r;
		const V params = Map({{"vertices_u", V::Unsigned(25)}, {"vertices_v", V::Unsigned(25)}, {"frames", V::Unsigned(16)},
			{"origin", Vec(-0.5, 0.6, 0.5)}, {"pins", V::ArrayValue({Map({{"select", V::String("u0v0")}}), Map({{"select", V::String("u1v0")}})})},
			{"colliders", V::ArrayValue({UVBall(0.25, 0, 0.25, 0, 0.15, 0)})}});
		Check(solver->Configure(params, inputs, d, error) && solver->Start(error), "lateral ball configures", error);
		while (!solver->Position().finished && solver->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error)) {}
		Check(error.empty() && sink.frames.size() == 16 && EffectiveFidelity(d.evidence) == "predictive",
			"a mesh ball moving laterally under a pinned 25 x 25 sheet bakes at 8 substeps (predictive)", error);
		std::printf("lateral UV ball, 25 x 25, 16 frames at 8 substeps: %zu frames, %.2f s\n", sink.frames.size(), Seconds() - t0);
	}
	{
		auto solver = Make();
		SolverDescription d;
		CountingSink sink;
		AdvanceResult r;
		const V params = Map({{"vertices_u", V::Unsigned(15)}, {"vertices_v", V::Unsigned(15)}, {"frames", V::Unsigned(10)},
			{"origin", Vec(-0.5, 0.6, 0.5)}, {"pins", V::ArrayValue({Map({{"select", V::String("v0")}})})},
			{"colliders", V::ArrayValue({UVBall(0.1, 0, 0.45, 0.5, 0, 0.3)})}});
		Check(solver->Configure(params, inputs, d, error) && solver->Start(error), "pinch configures", error);
		error.clear();
		bool failed = false;
		while (!solver->Position().finished) if (!solver->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error)) { failed = true; break; }
		Check(failed && error.find("infeasible at t = ") != std::string::npos &&
			error.find("pinched between mesh collider 0 and pin group 0") != std::string::npos,
			"a ball pressing into a pinned edge fails with the pinch diagnostic", error);
		std::printf("pinch through the contract: %s\n", error.c_str());
	}
}

// Macro steps on the output grid equal free stepping exactly; half-interval
// macro steps keep the substep length and agree to round-off.
void MacroSteps()
{
	std::string error;
	NoSceneInputs inputs;
	InProcessHost free(Make()), aligned(Make()), half(Make());
	const V params = Map(Small());
	Check(free.Configure(params, inputs, error) && free.Start(error) && free.StepToIndex(1000, error), "free run", error);
	Check(aligned.Configure(params, inputs, error) && aligned.Start(error) && half.Configure(params, inputs, error) && half.Start(error),
		"macro hosts", error);
	const auto& c = aligned.Description().cache;
	AdvanceResult r;
	for (std::uint64_t k = 0; k < c.count; ++k)
		if (!aligned.Step({AdvanceRequest::Mode::MacroStep, c.startTime + static_cast<double>(k)*c.interval}, r, error)) { Check(false, "aligned macro step", error); break; }
	Check(aligned.Position().finished && Digests(*aligned.Snapshot()) == Digests(*free.Snapshot()),
		"macro steps on the output grid equal free stepping byte for byte");
	for (std::uint64_t j = 0; j <= 2*(c.count - 1); ++j)
		if (!half.Step({AdvanceRequest::Mode::MacroStep, c.startTime + static_cast<double>(j)*(c.interval/2)}, r, error)) { Check(false, "half macro step", error); break; }
	const auto a = half.Snapshot(), b = free.Snapshot();
	double worst = 0;
	for (std::size_t i = 0; i < a->Entries().size() && i < b->Entries().size(); ++i) {
		std::shared_ptr<const Bytes> x, y;
		MeshFrame mx, my;
		if (a->Read(a->Entries()[i].index, "surface", budget, x, error) != FrameStatus::Ready ||
			b->Read(b->Entries()[i].index, "surface", budget, y, error) != FrameStatus::Ready ||
			!DecodeMesh(*x, budget, a->Resolver(), mx, error) || !DecodeMesh(*y, budget, b->Resolver(), my, error)) { worst = 1; break; }
		for (std::size_t q = 0; q < mx.positions.size(); ++q) worst = std::max(worst, std::fabs(F(mx.positions[q]) - F(my.positions[q])));
	}
	std::printf("half-interval macro steps vs free stepping: max position difference %.3e m\n", worst);
	Check(half.Position().finished && worst < 1e-6, "half-interval macro steps agree with free stepping", std::to_string(worst));
}

// One payload for every Accept: a constant pressure on the sheet's vertices
// (field.v1 over the topology digest of a nu x nv sheet).
class ConstantLoad final : public IPortExchange {
public:
	ConstantLoad(const std::string& domain, std::uint32_t count, float pressure) { Set(domain, count, pressure); }
	void Set(const std::string& domain, std::uint32_t count, float pressure) {
		FieldFrame f;
		f.domain = domain;
		f.count = count;
		std::uint32_t bits;
		std::memcpy(&bits, &pressure, 4);
		f.channels.push_back({"pressure", "Pa", 1, std::vector<std::uint32_t>(count, bits)});
		auto bytes = std::make_shared<Bytes>();
		std::string error;
		ok = EncodeField(f, budget, *bytes, error);
		payload = std::move(bytes);
	}
	bool Offer(const std::string& port, double, std::shared_ptr<const Bytes> encoded, std::string&) override {
		if (port == "surface_state") offers.push_back(std::move(encoded));
		return true;
	}
	PortStatus Accept(const std::string& port, double, std::shared_ptr<const Bytes>& encoded, std::string&) override {
		if (port != "surface_load") return PortStatus::Unbound;
		++accepts;
		encoded = payload;
		return PortStatus::Ready;
	}
	bool OfferStatic(const std::string&, std::shared_ptr<const Bytes> bytes, std::string&) override {
		statics[RISE::RISECBOR64::SHA256Hex(*bytes)] = std::move(bytes);
		return true;
	}
	bool ok = false;
	int accepts = 0;
	std::shared_ptr<const Bytes> payload;
	std::vector<std::shared_ptr<const Bytes>> offers;
	std::map<std::string,std::shared_ptr<const Bytes>> statics;
};

std::string LoadDomain(const V& params)
{
	auto solver = Make();
	SolverDescription d;
	std::string error;
	solver->Configure(params, EmptySceneInputs(), d, error);
	for (const auto& p : d.ports) if (!p.offers) return p.domain;
	return {};
}

void PortFeed()
{
	std::string error;
	NoSceneInputs inputs;
	InProcessHost viaPort(Make()), viaParameter(Make()), none(Make());
	const std::string domain = LoadDomain(Map(Small()));
	ConstantLoad load(domain, 81, 2.5f);
	Check(load.ok && domain.size() == 64, "encode the load field");
	viaPort.SetPorts(&load);
	Check(viaPort.Configure(Map(Small()), inputs, error) && viaPort.Start(error) && viaPort.StepToIndex(100, error),
		"pressure through the port", error);
	Check(viaParameter.Configure(With(Small(), "pressure", V::Float(2.5)), inputs, error) && viaParameter.Start(error) &&
		viaParameter.StepToIndex(100, error), "pressure as a parameter", error);
	Check(none.Configure(Map(Small()), inputs, error) && none.Start(error) && none.StepToIndex(100, error), "no pressure", error);
	Check(load.accepts > 0 && Digests(*viaPort.Snapshot()) == Digests(*viaParameter.Snapshot()),
		"a constant pressure via surface_load equals the same pressure as a parameter, byte for byte");
	Check(Digests(*viaPort.Snapshot()) != Digests(*none.Snapshot()), "the pressure changes the motion");
	bool offered = load.offers.size() == 10;
	const auto snapshot = viaPort.Snapshot();
	for (std::size_t i = 0; offered && i < load.offers.size(); ++i) {
		std::shared_ptr<const Bytes> b;
		offered = snapshot->Read(static_cast<std::int64_t>(i), "surface", budget, b, error) == FrameStatus::Ready && *b == *load.offers[i];
	}
	Check(offered && load.statics.size() == 1 && snapshot->Statics().size() == 1 &&
		load.statics.begin()->first == snapshot->Statics()[0].digest,
		"surface_state offers the published surface payloads and their static topology");
	// A load over another index space is refused.
	InProcessHost wrong(Make()), shorter(Make());
	ConstantLoad bad(std::string(64, 'a'), 81, 1.0f), few(domain, 25, 1.0f);
	wrong.SetPorts(&bad);
	shorter.SetPorts(&few);
	Check(wrong.Configure(Map(Small()), inputs, error) && wrong.Start(error) && !wrong.StepToIndex(100, error) &&
		error.find("domain") != std::string::npos, "a load on another domain is refused", error);
	Check(shorter.Configure(Map(Small()), inputs, error) && shorter.Start(error) && !shorter.StepToIndex(100, error),
		"a load with the wrong element count is refused");
}


// A stop requested between Advance calls stops the next one, then clears.
void StickyStop()
{
	std::string error;
	auto solver = Make();
	SolverDescription d;
	CountingSink sink;
	AdvanceResult r;
	Check(solver->Configure(Map(Small()), EmptySceneInputs(), d, error) && solver->Start(error) &&
		solver->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error), "sticky-stop sheet", error);
	solver->RequestStop();
	Check(solver->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error) && r.status == AdvanceResult::Status::Stopped &&
		sink.frames.size() == 1, "a stop requested between calls stops the next Advance");
	Check(solver->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error) && r.status == AdvanceResult::Status::Advanced &&
		sink.frames.size() == 2, "the consumed request is cleared");
}

// Rollback: a repeated macro step equals the first; with a new load it equals
// a re-run of the step from a checkpoint with that load.
void Rollback()
{
	std::string error;
	const V params = Map(Small());
	const std::string domain = LoadDomain(params);
	auto solver = Make(), rerun = Make();
	SolverDescription d, d2;
	ConstantLoad load(domain, 81, 1.0f), other(domain, 81, 6.0f);
	CountingSink sink, first, second, third, reference;
	AdvanceResult r;
	Check(solver->Configure(params, EmptySceneInputs(), d, error) && solver->Start(error), "rollback sheet", error);
	const double dt = d.cache.interval;
	std::shared_ptr<const SolverStateToken> token;
	Bytes checkpoint;
	bool ok = solver->Advance({AdvanceRequest::Mode::MacroStep, 1.5*dt}, sink, &load, nullptr, r, error) &&
		solver->SaveState(token, error) && solver->Checkpoint(checkpoint, error) &&
		solver->Advance({AdvanceRequest::Mode::MacroStep, 3.5*dt}, first, &load, nullptr, r, error) &&
		solver->RestoreState(*token, error) &&
		solver->Advance({AdvanceRequest::Mode::MacroStep, 3.5*dt}, second, &load, nullptr, r, error);
	bool same = ok && first.frames.size() == 2 && second.frames.size() == 2;
	for (std::size_t i = 0; same && i < 2; ++i) same = *first.frames[i].second == *second.frames[i].second;
	Check(same, "a rolled-back cloth macro step repeats byte for byte", error);
	ok = solver->RestoreState(*token, error) &&
		solver->Advance({AdvanceRequest::Mode::MacroStep, 3.5*dt}, third, &other, nullptr, r, error) &&
		rerun->Configure(params, EmptySceneInputs(), d2, error) && rerun->Restore(checkpoint, error) &&
		rerun->Advance({AdvanceRequest::Mode::MacroStep, 3.5*dt}, reference, &other, nullptr, r, error);
	same = ok && third.frames.size() == 2 && reference.frames.size() == 2;
	for (std::size_t i = 0; same && i < 2; ++i) same = *third.frames[i].second == *reference.frames[i].second;
	Check(same && *third.frames[1].second != *first.frames[1].second,
		"a repeated step with a new load equals a re-run from the checkpoint with that load", error);
}

// Scene inputs with one rigid motion "rig": a translation at `velocity`,
// optionally failing after `failAfter` seconds.
class RigInput final : public ISceneInputs {
public:
	explicit RigInput(double velocity, const std::string& identity = "rig-identity", double failAfter = 1e9)
		: velocity_(velocity), identity_(identity), failAfter_(failAfter) {}
	bool Identity(const std::string& name, std::string& hash, std::string& error) const override {
		if (name != "rig") { error = "no such input"; return false; }
		hash = RISE::RISECBOR64::SHA256Hex(reinterpret_cast<const unsigned char*>(identity_.data()), identity_.size());
		return true;
	}
	bool Sample(const std::string&, double, std::shared_ptr<const Bytes>&, std::string&, std::string& error) const override {
		error = "no payload inputs";
		return false;
	}
	bool SampleTransform(const std::string& name, double time, RigidTransform& m, std::string& error) const override {
		if (name != "rig") { error = "no such transform"; return false; }
		if (time > failAfter_) { error = "rig track ends"; return false; }
		m = RigidTransform{};
		m.translation = {{velocity_*time, 0, 0}};
		return true;
	}
private:
	double velocity_;
	std::string identity_;
	double failAfter_;
};

// Pins driven by a scene transform follow it like the same parametric motion.
void SceneTransforms()
{
	std::string error;
	V::Members base = Small();
	base.erase(std::remove_if(base.begin(), base.end(), [](const auto& m){ return m.first == "pins"; }), base.end());
	V::Members tracked = base, parametric = base;
	tracked.emplace_back("pins", V::ArrayValue({Map({{"select", V::String("v0")}, {"motion", Map({{"type", V::String("scene_transform")},
		{"input", V::String("rig")}})}})}));
	parametric.emplace_back("pins", V::ArrayValue({Map({{"select", V::String("v0")}, {"motion", Map({{"type", V::String("translate")},
		{"velocity", Vec(0.3, 0, 0)}})}})}));
	RigInput rig(0.3);
	auto a = Make(), b = Make();
	SolverDescription da, db;
	CountingSink sa, sb;
	AdvanceResult r;
	Check(a->Configure(V::MapValue(tracked), rig, da, error) && a->Start(error), "scene-transform pins configure", error);
	Check(b->Configure(V::MapValue(parametric), EmptySceneInputs(), db, error) && b->Start(error), "parametric pins", error);
	while (!a->Position().finished && a->Advance(AdvanceRequest{}, sa, nullptr, nullptr, r, error)) {}
	while (!b->Position().finished && b->Advance(AdvanceRequest{}, sb, nullptr, nullptr, r, error)) {}
	double worst = 0;
	for (std::size_t k = 0; k < sa.frames.size() && k < sb.frames.size(); ++k) {
		MeshFrame x, y;
		if (!DecodeMesh(*sa.frames[k].second, budget, sa.Resolver(), x, error) ||
			!DecodeMesh(*sb.frames[k].second, budget, sb.Resolver(), y, error)) { worst = 1; break; }
		for (std::size_t q = 0; q < x.positions.size(); ++q) worst = std::max(worst, std::fabs(F(x.positions[q]) - F(y.positions[q])));
	}
	std::printf("scene-transform pins vs the same parametric motion: max position difference %.3e m\n", worst);
	Check(sa.frames.size() == 10 && worst < 1e-5, "pins follow a scene transform like the same parametric motion");
	Check(da.inputs == std::vector<std::string>{"rig"} && EffectiveFidelity(da.evidence) == "preview" &&
		!da.capabilities.fidelityLimit.empty(), "a scene-transform input is listed, keyed and preview");
	SolverDescription other;
	auto c = Make();
	RigInput moved(0.3, "another-identity");
	Check(PrepareCacheDescription(da, rig, "", error) && c->Configure(V::MapValue(tracked), moved, other, error) &&
		PrepareCacheDescription(other, moved, "", error) && other.cache.key != da.cache.key,
		"the transform input's identity enters the cache key", error);
	auto failing = Make();
	RigInput broken(0.3, "rig-identity", 0.1);
	SolverDescription df;
	CountingSink sf;
	bool failed = false;
	Check(failing->Configure(V::MapValue(tracked), broken, df, error) && failing->Start(error), "failing track configures", error);
	while (!failing->Position().finished) {
		if (!failing->Advance(AdvanceRequest{}, sf, nullptr, nullptr, r, error)) { failed = true; break; }
	}
	Check(failed && error.find("rig track ends") != std::string::npos, "a transform sampling failure fails the step", error);
	// A mesh collider moved by a scene transform configures (rigid motion).
	V::Members collider = Small();
	V cube = CubeCollider(false);
	V::Members m = cube.GetArray()[0].GetMap();
	m.emplace_back("motion", Map({{"type", V::String("scene_transform")}, {"input", V::String("rig")}}));
	collider.emplace_back("colliders", V::ArrayValue({V::MapValue(m)}));
	SolverDescription dc;
	auto e = Make();
	Check(e->Configure(V::MapValue(collider), rig, dc, error) && dc.inputs == std::vector<std::string>{"rig"},
		"a mesh collider follows a scene transform", error);
	Check(!e->Configure(V::MapValue(collider), EmptySceneInputs(), dc, error), "a missing transform input is refused");
}

void InteractiveMode()
{
	std::string error;
	NoSceneInputs inputs;
	InProcessHost host(Make());
	const double t0 = Seconds();
	Check(host.Configure(With(Small(), "mode", V::String("interactive")), inputs, error) && host.Start(error) &&
		host.Play(error) && host.WaitIdle(error) && host.Position().finished && host.Snapshot()->Complete(),
		"interactive mode plays on the host's thread", error);
	std::printf("interactive 9 x 9 sheet, 10 frames: %.3f s including Configure\n", Seconds() - t0);
}

// ---------------------------------------------------------------- worker

int Worker(const std::vector<std::string>& args, std::string* output = nullptr)
{
	std::ostringstream out, err;
	const int code = WorkerMain(args, out, err);
	if (output) *output = out.str() + err.str();
	return code;
}

Bytes ReadAll(const fs::path& path)
{
	std::ifstream f(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::map<std::string, Bytes> Sealed(const fs::path& dir)
{
	std::map<std::string, Bytes> files;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(dir, ec)) {
		const auto name = e.path().filename().string();
		if (name == "manifest.cbor" || (name.size() > 6 && name.compare(name.size()-6, 6, ".frame") == 0) ||
			(name.size() > 7 && name.compare(name.size()-7, 7, ".static") == 0)) files[name] = ReadAll(e.path());
	}
	return files;
}

void WorkerHost(const fs::path& root)
{
	std::string log, error;
	const auto params = root/"cloth.json";
	{
		std::ofstream f(params);
		f << "{\"vertices_u\": 9, \"vertices_v\": 9, \"size_u\": 0.4, \"size_v\": 0.4, \"frames\": 10, \"substeps\": 4,"
			" \"pins\": [{\"select\": \"u0v0\"}, {\"select\": \"u1v0\"}]}";
	}
	const auto reference = root/"reference", stopped = root/"stopped";
	Check(Worker({"bake", ClothSolverId, params.string(), reference.string(), "--checkpoint-seconds", "0"}, &log) == WorkerFinished,
		"worker bakes cloth", log);
	CacheSnapshot disk;
	Check(CacheSnapshot::Open(reference, budget, disk, error) && disk.Complete() && disk.Description().fidelity == "predictive" &&
		disk.Description().producer == ClothSolverId && !disk.Description().physicalRecords.empty(),
		"worker seals a predictive cloth cache with the material record", error);
	// The in-process host publishes the same frames under the same key.
	InProcessHost host(Make());
	NoSceneInputs inputs;
	V job;
	Check(RISE::RISECBOR64::DecodeCanonical(ReadAll(reference/"rise-sim.job.cbor"), job, &error) && job.Find("parameters"), "job record", error);
	Check(host.Configure(*job.Find("parameters"), inputs, error) && host.Start(error) && host.StepToIndex(1 << 20, error) &&
		host.Description().cache.key == disk.Description().key && Digests(*host.Snapshot()) == Digests(disk),
		"in-process and worker hosts publish identical cloth frames", error);
	// Stop, resume, resume: byte-identical to the uninterrupted bake.
	Check(Worker({"bake", ClothSolverId, params.string(), stopped.string(), "--stop-after-frames", "3"}, &log) == WorkerStopped,
		"worker stops after three frames", log);
	Check(Worker({"resume", stopped.string(), "--stop-after-frames", "4"}, &log) == WorkerStopped, "resume stops again", log);
	Check(Worker({"resume", stopped.string()}, &log) == WorkerFinished, "resume finishes", log);
	const auto x = Sealed(stopped), y = Sealed(reference);
	Check(x.size() > 1 && x == y, "worker stop and resume equals the uninterrupted bake byte for byte");
}

bool DiskPublicationAvailable(const fs::path& root)
{
#ifdef _WIN32
	CacheDescription d;
	d.key = std::string(64, '1'); d.epoch = std::string(64, '2'); d.producer = "probe"; d.producerVersion = "1";
	d.outputs = {{"fields", "grid.v1", {{"density", "1", "f32"}}}};
	CacheWriter probe;
	std::string error;
	if (!probe.Begin(root/"durability-probe", d, "", false, budget, error)) {
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
	std::string error;
	Check(RegisterClothSolver(error) && RegisterClothSolver(error), "cloth registration is idempotent", error);
	const fs::path root = RISE::TestSupport::ScratchRunDir("rise_cloth_solver_", "suite");
	const double t0 = Seconds();
	try {
		Description();
		Frames();
		StopRestore(Map(Small()), "free sheet");
		{
			V::Members contact = Small();
			contact.emplace_back("colliders", V::ArrayValue({Map({{"type", V::String("sphere")}, {"center", Vec(-0.1, 0.85, 0.25)},
				{"radius", V::Float(0.04)}}), CubeCollider(true).GetArray()[0]}));
			StopRestore(V::MapValue(contact), "barrier contact with friction and a rotating mesh");
			// Kinematic stages (moving pins and a translating mesh) and self-collision.
			V::Members moving = Small();
			moving[6].second = V::ArrayValue({Map({{"select", V::String("u0v0")}, {"motion", Map({{"type", V::String("translate")},
				{"velocity", Vec(0.2, 0, 0)}})}}), Map({{"select", V::String("u1v0")}})});
			V cube = CubeCollider(false).GetArray()[0];
			V::Members cm = cube.GetMap();
			cm.emplace_back("motion", Map({{"type", V::String("translate")}, {"velocity", Vec(0, 0.3, 0)}}));
			moving.emplace_back("colliders", V::ArrayValue({V::MapValue(cm)}));
			moving.emplace_back("self_collision", V::Bool(true));
			StopRestore(V::MapValue(moving), "moving pins, a rising mesh and self-collision");
		}
		Threads();
		MovingColliders();
		MacroSteps();
		PortFeed();
		StickyStop();
		Rollback();
		SceneTransforms();
		InteractiveMode();
		fs::create_directories(root);
		if (DiskPublicationAvailable(root)) WorkerHost(root);
	} catch (const std::exception& e) {
		++failures;
		std::cerr << "exception: " << e.what() << "\n";
	}
	std::error_code ignored;
	fs::remove_all(root, ignored);
	std::printf("total %.1f s\n", Seconds() - t0);
	if (failures) { std::cerr << failures << " cloth solver check(s) failed\n"; return 1; }
	std::cout << "Cloth solver contract checks passed\n";
	return 0;
}
