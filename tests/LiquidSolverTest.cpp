// liquid.mac (simulation M7, docs/SIMULATION_SOLVER_CONTRACT.md): contract
// conformance.
//
//   A  description: id, version, outputs, capabilities, estimates, the
//      evidence and its label, effective parameters as a fixed point,
//      refusals, the schema's defaults are the solver's
//   B  frames: the surface is a closed, outward mesh whose volume is the
//      liquid's; fields and particles decode; a box obstacle; a mesh solid
//      from a scene input (identity keyed)
//   C  determinism: twin runs and 1, 3 and 8 threads give identical bytes
//   D  stop / restore in the middle of a segment (in-memory and streamed
//      checkpoints), with marker particles too, equal to an uninterrupted run
//   E  macro steps on the output grid reproduce free stepping byte for byte
//   F  rollback: SaveState / RestoreState repeat a macro step exactly
//   G  rise-sim: bake, stop, resume, resume = the uninterrupted bake, byte for byte
#include "../src/Library/Simulation/Liquid/LiquidSolver.h"
#include "../src/Library/Simulation/Liquid/LiquidParameterSchema.h"
#include "../src/Library/Simulation/SimulationGrid.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationPoints.h"
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
constexpr std::size_t budget = std::size_t(1) << 30;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }
Bytes Encoded(const V& v) { Bytes b; std::string e; RISE::RISECBOR64::Encode(v, b, &e); return b; }
double F32(std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

std::unique_ptr<ISimulationSolver> Make()
{
	std::string error;
	Check(RegisterLiquidSolver(error), "register", error);
	auto s = CreateSolver(LiquidSolverId, error);
	Check(s != nullptr, "create", error);
	return s;
}

V::Members Small()
{
	// A small three-dimensional dam break: 16 x 8 x 4 cells of 2.5 cm.
	return {{"frames", V::Unsigned(6)}, {"domain_min", Vec(0, 0, 0)}, {"domain_size", Vec(0.4, 0.2, 0.1)},
		{"resolution", V::Unsigned(16)},
		{"liquid", V::ArrayValue({V::MapValue({{"type", V::String("box")}, {"min", Vec(-1, -1, -1)}, {"max", Vec(0.15, 0.15, 1)}})})}};
}
V Map(V::Members m) { return V::MapValue(m); }
V With(V::Members m, const char* key, V value)
{
	for (auto& e : m) if (e.first == key) { e.second = value; return V::MapValue(m); }
	m.emplace_back(key, value);
	return V::MapValue(m);
}

std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const IReadOnlyCacheSnapshot& s)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> out;
	for (const auto& e : s.Entries()) out[{e.index, e.output}] = e.digest;
	return out;
}

class DigestSink final : public IFrameSink {
public:
	std::map<std::pair<std::int64_t,std::string>,std::string> digests;
	std::map<std::pair<std::int64_t,std::string>,std::shared_ptr<const Bytes>> payloads;
	bool Publish(std::int64_t i, const std::string& o, double, std::shared_ptr<const Bytes> p, const Bytes&, std::string&) override {
		digests[{i, o}] = RISE::RISECBOR64::SHA256Hex(*p);
		payloads[{i, o}] = p;
		return true;
	}
};

bool RunAll(ISimulationSolver& s, IFrameSink& sink, std::string& error)
{
	AdvanceResult r;
	for (int k = 0; k < 1000; ++k) {
		if (!s.Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error)) return false;
		if (r.status == AdvanceResult::Status::Finished) return true;
	}
	error = "did not finish";
	return false;
}

// Every directed edge of a closed, consistently oriented mesh appears once, its reverse once.
bool ClosedMesh(const MeshFrame& m)
{
	std::map<std::pair<std::uint32_t,std::uint32_t>, int> e;
	for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3)
		for (int k = 0; k < 3; ++k) e[{m.indices[t + std::size_t(k)], m.indices[t + std::size_t((k + 1) % 3)]}] += 1;
	for (const auto& kv : e) {
		const auto rev = e.find({kv.first.second, kv.first.first});
		if (kv.second != 1 || rev == e.end() || rev->second != 1) return false;
	}
	return true;
}
double MeshVolume(const MeshFrame& m)
{
	double v = 0;
	for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
		double p[3][3];
		for (int k = 0; k < 3; ++k) for (int a = 0; a < 3; ++a) p[k][a] = F32(m.positions[3*m.indices[t + std::size_t(k)] + std::size_t(a)]);
		v += (p[0][0]*(p[1][1]*p[2][2] - p[1][2]*p[2][1]) - p[0][1]*(p[1][0]*p[2][2] - p[1][2]*p[2][0]) +
			p[0][2]*(p[1][0]*p[2][1] - p[1][1]*p[2][0]))/6;
	}
	return v;
}

// One scene input: a cube mesh (mesh.v1) of side 0.06 centred at (0.25, 0.03, 0.05).
class CubeScene final : public ISceneInputs {
public:
	std::shared_ptr<const Bytes> mesh;
	CubeScene() {
		const double c[3] = {0.25, 0.03, 0.05}, s = 0.03;
		MeshFrame m;
		for (int k = 0; k < 8; ++k) for (int a = 0; a < 3; ++a) {
			const double x = c[a] + ((k >> a) & 1 ? s : -s);
			const float f = static_cast<float>(x);
			std::uint32_t b;
			std::memcpy(&b, &f, 4);
			m.positions.push_back(b);
		}
		// Outward triangles of the cube (corner = x + 2y + 4z).
		const std::uint32_t q[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
		for (const auto& f : q) m.indices.insert(m.indices.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
		auto bytes = std::make_shared<Bytes>();
		std::string error;
		EncodeMesh(m, budget, *bytes, error);
		mesh = bytes;
	}
	bool Identity(const std::string& name, std::string& hash, std::string& error) const override {
		if (name != "cube") { error = "no such input"; return false; }
		hash = RISE::RISECBOR64::SHA256Hex(*mesh);
		return true;
	}
	bool Sample(const std::string& name, double, std::shared_ptr<const Bytes>& encoded, std::string& codec, std::string& error) const override {
		if (name != "cube") { error = "no such input"; return false; }
		encoded = mesh;
		codec = "mesh.v1";
		return true;
	}
};

// ---------------------------------------------------------------- A

void Description()
{
	std::printf("A: description, evidence, schema\n");
	std::string error;
	auto s = Make();
	SolverDescription d;
	const double t0 = Seconds();
	Check(s->Configure(Map(Small()), EmptySceneInputs(), d, error), "configure", error);
	std::printf("   first Configure (evidence) %.2f s\n", Seconds() - t0);
	Check(d.solverId == LiquidSolverId && !d.version.empty(), "id and version");
	Check(d.cache.outputs.size() == 1 && d.cache.outputs[0].name == "surface" && d.cache.outputs[0].type == "mesh.v2", "surface output only by default");
	Check(d.capabilities.deterministic && d.capabilities.resumable && d.capabilities.externalMacroStep && d.capabilities.rollback &&
		d.capabilities.threadCountInvariant && d.capabilities.backend == "cpu" && d.capabilities.interactive, "capabilities");
	Check(d.capabilities.memoryEstimate > 0 && d.capabilities.costEstimate > 0, "memory and cost estimates");
	const std::string label = EffectiveFidelity(d.evidence);
	for (const auto& c : d.evidence.cases)
		std::printf("   %-26s error %.3e tolerance %.1e %s\n", c.id.c_str(), c.error, c.tolerance, c.passed ? "pass" : "FAIL");
	Check(label == "predictive", "the default numerics earn predictive", label);
	Check(!d.evidence.scope.empty() && ValidEvidenceConditions(d.evidence.conditions), "scope and conditions");
	// Effective parameters are a fixed point.
	SolverDescription d2;
	Check(s->Configure(d.effectiveParameters, EmptySceneInputs(), d2, error) &&
		Encoded(d2.effectiveParameters) == Encoded(d.effectiveParameters), "Configure(effective) reproduces it", error);
	// threads is not keyed.
	SolverDescription d3;
	Check(s->Configure(With(Small(), "threads", V::Unsigned(3)), EmptySceneInputs(), d3, error) &&
		Encoded(d3.effectiveParameters) == Encoded(d.effectiveParameters), "threads is not an effective parameter", error);
	// A non-default scheme is evaluated on its own evidence (first order: preview).
	SolverDescription sl;
	Check(s->Configure(With(Small(), "momentum", V::String("semi_lagrangian")), EmptySceneInputs(), sl, error), "semi-Lagrangian configures", error);
	Check(EffectiveFidelity(sl.evidence) == "preview", "semi-Lagrangian momentum is preview (fails its order case)");
	// Large grids are not interactive.
	SolverDescription big;
	Check(s->Configure(With(Small(), "resolution", V::Unsigned(128)), EmptySceneInputs(), big, error) && !big.capabilities.interactive,
		"128 x 64 x 32 is not interactive", error);
	// Outputs and the port.
	SolverDescription full;
	Check(s->Configure(With(With(Small(), "publish_particles", V::Bool(true)).GetMap(), "publish_fields", V::Bool(true)), EmptySceneInputs(), full, error) &&
		full.cache.outputs.size() == 3 && full.ports.size() == 1 && full.ports[0].output == "fields" && full.ports[0].offers,
		"fields and particles outputs, the fields port", error);
	// Refusals.
	for (const auto& bad : {With(Small(), "bogus", V::Unsigned(1)), With(Small(), "cfl", V::Float(2)),
		With(With(Small(), "particles", V::Bool(false)).GetMap(), "publish_particles", V::Bool(true)), With(Small(), "momentum", V::String("upwind")),
		With(Small(), "domain_size", Vec(0, 1, 1)), With(Small(), "theta_min", V::Float(0.7)), With(Small(), "min_transport_fraction", V::Float(1.5)),
		With(Small(), "liquid", V::ArrayValue({V::MapValue({{"type", V::String("box")}, {"min", Vec(1, 1, 1)}, {"max", Vec(0, 0, 0)}})}))}) {
		SolverDescription x;
		Check(!s->Configure(bad, EmptySceneInputs(), x, error), "refused: " + error);
	}
	// The schema: registered, its defaults are the solver's, every effective parameter is described.
	const ParameterSchema& schema = LiquidParameterSchema();
	Check(FindSolverSchema(LiquidSolverId) == nullptr || FindSolverSchema(LiquidSolverId) == &schema, "schema registry");
	SolverDescription plain;
	Check(s->Configure(V::MapValue({}), EmptySceneInputs(), plain, error), "configure with defaults", error);
	for (const auto& p : schema.parameters) {
		if (!p.keyed) { Check(!plain.effectiveParameters.Find(p.name), "unkeyed " + p.name + " is not effective"); continue; }
		if (p.defaultText.empty()) continue;
		V expected;
		Check(SchemaValueFromText(p, p.defaultText, expected, error), "schema default of " + p.name + " parses", error);
		const V* actual = plain.effectiveParameters.Find(p.name);
		Check(actual && Encoded(*actual) == Encoded(expected), "schema default of " + p.name + " (" + p.defaultText + ") is the solver's");
	}
	for (const auto& m : plain.effectiveParameters.GetMap())
		Check(FindSchemaParameter(schema, m.first) || m.first == "liquid" || m.first == "solids" || m.first == "bodies",
			"effective parameter " + m.first + " is described by the schema");
	for (const auto& p : schema.parameters) {
		if (p.type != SchemaType::Enum) continue;
		for (const auto& v : p.values) {
			SolverDescription x;
			Check(s->Configure(With(Small(), p.name.c_str(), V::String(v)), EmptySceneInputs(), x, error), p.name + " " + v + " configures", error);
		}
	}
	// The chunk translation.
	{
		SchemaFieldValues values{{"liquid", {"box 0 0 0 0.1 0.1 0.1", "fill 0.05"}}, {"solid", {"sphere 0.2 0.05 0.05 0.03",
			"cylinder 0.3 0.05 0.05 0 1 0 0.02 0.1"}}, {"solid_object", {"cube"}}};
		V params;
		std::vector<std::string> inputs;
		Check(TranslateSchemaChunk(schema, values, params, inputs, error) && inputs == std::vector<std::string>{"cube"} &&
			params.Find("liquid") && params.Find("liquid")->GetArray().size() == 2 && params.Find("solids") &&
			params.Find("solids")->GetArray().size() == 3, "chunk fields translate", error);
		Check(!TranslateSchemaChunk(schema, {{"solid", {"cone 1 2 3"}}}, params, inputs, error), "unknown solid refused");
	}
}

// ---------------------------------------------------------------- B

void Frames()
{
	std::printf("B: frames\n");
	std::string error;
	// A box obstacle and a scene-input mesh solid; fields and particles.
	CubeScene scene;
	V::Members m = Small();
	m.emplace_back("solids", V::ArrayValue({V::MapValue({{"type", V::String("box")}, {"min", Vec(0.3, 0, 0.02)}, {"max", Vec(0.35, 0.05, 0.08)}}),
		V::MapValue({{"type", V::String("mesh")}, {"scene_input", V::String("cube")}})}));
	m.emplace_back("publish_particles", V::Bool(true));
	m.emplace_back("publish_fields", V::Bool(true));
	auto s = Make();
	SolverDescription d;
	Check(s->Configure(V::MapValue(m), scene, d, error) && s->Start(error), "configure with solids", error);
	Check(d.inputs == std::vector<std::string>{"cube"}, "the mesh solid is a scene input");
	const V* solids = d.effectiveParameters.Find("solids");
	Check(solids && solids->GetArray().size() == 2 && solids->GetArray()[1].Find("identity"), "its identity is an effective parameter");
	DigestSink sink;
	// The safeguard's count reaches the progress detail (cloth's channel for its solver counts).
	class Detail final : public IProgress {
	public:
		bool Report(double, double, const std::string& detail) override { last = detail; return true; }
		std::string last;
	} detail;
	{
		bool ran = true;
		for (int guard = 0; ran && guard < 1000; ++guard) {
			AdvanceResult r;
			ran = s->Advance(AdvanceRequest{}, sink, nullptr, &detail, r, error);
			if (r.status == AdvanceResult::Status::Finished) break;
		}
		Check(ran, "run", error);
	}
	Check(detail.last.find("clamped faces 0 in 0 steps") != std::string::npos, "the progress detail reports the clamped-face count", detail.last);
	Check(sink.payloads.size() == 18, "6 frames x 3 outputs");
	for (int i = 0; i < 6; ++i) {
		MeshFrame mesh;
		const auto& surf = sink.payloads[{i, "surface"}];
		Check(surf && DecodeMesh(*surf, budget, mesh, error), "surface " + std::to_string(i) + " decodes", error);
		Check(!mesh.indices.empty() && ClosedMesh(mesh), "surface " + std::to_string(i) + " is closed and oriented");
		bool finite = true;
		for (std::uint32_t b : mesh.velocities) finite = finite && std::isfinite(F32(b));
		Check(finite && mesh.velocities.size() == mesh.positions.size() && mesh.normals.size() == mesh.positions.size(), "velocities and normals");
		// The surface volume: the liquid's (0.15 x 0.15 x 0.1) less the rounding of its edges by marching
		// cubes: -5.1% at this resolution (6 cells across), -1.3% and -0.34% at 2x and 4x (second order).
		if (i == 0) Check(std::fabs(MeshVolume(mesh) - 0.15*0.15*0.1) < 0.064*0.15*0.15*0.1, "the initial surface encloses the liquid",
			std::to_string(MeshVolume(mesh)));
		GridFrame f;
		Check(sink.payloads[{i, "fields"}] && DecodeGrid(*sink.payloads[{i, "fields"}], budget, f, error) && f.channels.size() == 3 &&
			f.channels[0].name == "phi" && f.channels[2].components == 3, "fields decode", error);
		PointsFrame p;
		Check(sink.payloads[{i, "particles"}] && DecodePoints(*sink.payloads[{i, "particles"}], budget, p, error) && p.Count() > 0 &&
			p.ids.size() == p.Count(), "particles decode", error);
	}
	// The thin film rides the obstacles without blowing up.
	MeshFrame last;
	DecodeMesh(*sink.payloads[{5, "surface"}], budget, last, error);
	double vmax = 0;
	for (std::uint32_t b : last.velocities) vmax = std::max(vmax, std::fabs(F32(b)));
	Check(vmax < 3*std::sqrt(2*9.81*0.15), "bounded velocities", std::to_string(vmax));
}

// ---------------------------------------------------------------- C, D, E, F

void Determinism()
{
	std::printf("C: determinism and threads\n");
	std::string error;
	std::vector<std::map<std::pair<std::int64_t,std::string>,std::string>> runs;
	for (unsigned threads : {1u, 3u, 8u, 8u}) {
		auto s = Make();
		SolverDescription d;
		DigestSink sink;
		Check(s->Configure(With(Small(), "threads", V::Unsigned(threads)), EmptySceneInputs(), d, error) &&
			s->Start(error) && RunAll(*s, sink, error), "run with " + std::to_string(threads) + " threads", error);
		runs.push_back(sink.digests);
	}
	Check(runs[0].size() == 6 && runs[0] == runs[1] && runs[0] == runs[2], "1, 3 and 8 threads publish identical frames (marker particles on)");
	Check(runs[2] == runs[3], "twin runs publish identical frames");
}

void StopRestore(const V& params, const char* what)
{
	std::printf("D: stop / restore (%s)\n", what);
	std::string error;
	DigestSink reference;
	{
		auto s = Make();
		SolverDescription d;
		Check(s->Configure(params, EmptySceneInputs(), d, error) && s->Start(error) && RunAll(*s, reference, error), "reference", error);
	}
	for (int streamed = 0; streamed < 2; ++streamed) {
		auto a = Make();
		SolverDescription d;
		DigestSink first;
		Check(a->Configure(params, EmptySceneInputs(), d, error) && a->Start(error), "configure", error);
		// Two frames, then a stop in the middle of the next segment.
		AdvanceResult r;
		Check(a->Advance(AdvanceRequest{}, first, nullptr, nullptr, r, error) && a->Advance(AdvanceRequest{}, first, nullptr, nullptr, r, error), "two frames", error);
		class StopAfter final : public IProgress {
		public:
			int calls = 0;
			bool Report(double, double, const std::string&) override { return ++calls < 3; }
		} stop;
		Check(a->Advance(AdvanceRequest{}, first, nullptr, &stop, r, error) && r.status == AdvanceResult::Status::Stopped, "stopped mid-segment", error);
		auto b = Make();
		SolverDescription d2;
		Check(b->Configure(params, EmptySceneInputs(), d2, error), "configure the restore", error);
		if (streamed) {
			MemoryCheckpointSink sink;
			Check(a->CheckpointTo(sink, error), "streamed checkpoint", error);
			MemoryCheckpointSource source(sink.bytes);
			Check(b->RestoreFrom(source, error), "streamed restore", error);
			// A truncated stream is refused.
			Bytes cut(sink.bytes.begin(), sink.bytes.end() - 9);
			MemoryCheckpointSource bad(cut);
			auto c = Make();
			SolverDescription d3;
			Check(c->Configure(params, EmptySceneInputs(), d3, error) && !c->RestoreFrom(bad, error), "truncated stream refused");
		} else {
			Bytes state;
			Check(a->Checkpoint(state, error) && b->Restore(state, error), "checkpoint and restore", error);
		}
		Check(b->Position().time == a->Position().time && b->Position().nextIndex == a->Position().nextIndex, "position restored");
		DigestSink rest;
		Check(RunAll(*b, rest, error), "finish after restore", error);
		auto merged = first.digests;
		for (const auto& e : rest.digests) merged[e.first] = e.second;
		Check(merged == reference.digests, std::string(streamed ? "streamed" : "in-memory") + " stop / restore equals the uninterrupted run");
	}
}

void MacroAndRollback()
{
	std::printf("E, F: macro steps, rollback\n");
	std::string error;
	DigestSink reference;
	auto s = Make();
	SolverDescription d;
	Check(s->Configure(Map(Small()), EmptySceneInputs(), d, error) && s->Start(error) && RunAll(*s, reference, error), "reference", error);
	auto m = Make();
	DigestSink sink;
	Check(m->Configure(Map(Small()), EmptySceneInputs(), d, error) && m->Start(error), "macro configure", error);
	AdvanceResult r;
	for (int k = 0; k < 6; ++k) {
		AdvanceRequest q;
		q.mode = AdvanceRequest::Mode::MacroStep;
		q.macroEnd = k*d.cache.interval;
		Check(m->Advance(q, sink, nullptr, nullptr, r, error), "macro step", error);
	}
	Check(sink.digests == reference.digests, "macro steps on the output grid reproduce free stepping byte for byte");
	// Half-interval macro steps run (their substeps differ: equal to discretisation error only).
	auto h = Make();
	DigestSink half;
	Check(h->Configure(Map(Small()), EmptySceneInputs(), d, error) && h->Start(error), "half configure", error);
	for (int k = 0; k <= 10; ++k) {
		AdvanceRequest q;
		q.mode = AdvanceRequest::Mode::MacroStep;
		q.macroEnd = 0.5*k*d.cache.interval;
		Check(h->Advance(q, half, nullptr, nullptr, r, error), "half macro step", error);
	}
	Check(half.digests.size() == 6, "half steps publish every output");
	// Rollback: save, step, restore, step again: identical.
	auto rb = Make();
	DigestSink one, two;
	Check(rb->Configure(Map(Small()), EmptySceneInputs(), d, error) && rb->Start(error), "rollback configure", error);
	AdvanceRequest q;
	q.mode = AdvanceRequest::Mode::MacroStep;
	q.macroEnd = 0;
	Check(rb->Advance(q, one, nullptr, nullptr, r, error), "first output", error);
	std::shared_ptr<const SolverStateToken> token;
	Check(rb->SaveState(token, error), "save", error);
	q.macroEnd = 2*d.cache.interval;
	Check(rb->Advance(q, one, nullptr, nullptr, r, error), "step", error);
	Check(rb->RestoreState(*token, error), "restore", error);
	Check(rb->Advance(q, two, nullptr, nullptr, r, error), "repeat", error);
	Check(one.digests.at({2, "surface"}) == two.digests.at({2, "surface"}) && two.digests.at({2, "surface"}) == reference.digests.at({2, "surface"}),
		"the repeated macro step is identical");
	auto other = Make();
	SolverDescription d4;
	Check(other->Configure(Map(Small()), EmptySceneInputs(), d4, error) && other->Start(error) && !other->RestoreState(*token, error),
		"a token from another instance is refused");
}

// ---------------------------------------------------------------- H (m7c)

// Viscosity, surface tension and a dynamic body (coupled in the viscous solve).
V::Members Physics()
{
	V::Members m = Small();
	m.emplace_back("viscosity", V::Float(0.5));
	m.emplace_back("surface_tension", V::Float(0.07));
	m.emplace_back("bodies", V::ArrayValue({V::MapValue({{"name", V::String("ball")}, {"shape", V::String("sphere")},
		{"radius", V::Float(0.035)}, {"motion", V::String("dynamic")}, {"density", V::Float(500)}, {"position", Vec(0.3, 0.12, 0.05)}})}));
	return m;
}

void ViscosityAndSurfaceTension()
{
	std::printf("H: viscosity and surface tension (m7c)\n");
	std::string error;
	auto s = Make();
	// Labels: each option adds its cases; surface tension alone is preview.
	SolverDescription v, st, both;
	double t0 = Seconds();
	Check(s->Configure(With(Small(), "viscosity", V::Float(0.5)), EmptySceneInputs(), v, error), "viscosity configures", error);
	std::printf("   first Configure with viscosity %.2f s\n", Seconds() - t0);
	t0 = Seconds();
	Check(s->Configure(With(With(Small(), "viscosity", V::Float(0.5)).GetMap(), "surface_tension", V::Float(0.07)), EmptySceneInputs(), both, error),
		"both configure", error);
	std::printf("   first Configure with both %.2f s\n", Seconds() - t0);
	Check(s->Configure(With(Small(), "surface_tension", V::Float(0.07)), EmptySceneInputs(), st, error), "surface tension configures", error);
	for (const auto& c : both.evidence.cases)
		if (c.id.find("viscous") != std::string::npos || c.id.find("couette") != std::string::npos || c.id.find("drag") != std::string::npos ||
			c.id.find("rotation") != std::string::npos || c.id.find("laplace") != std::string::npos || c.id.find("parasitic") != std::string::npos ||
			c.id.find("capillary") != std::string::npos || c.id.find("oscillating") != std::string::npos)
			std::printf("   %-26s error %.3e tolerance %.1e %s\n", c.id.c_str(), c.error, c.tolerance, c.passed ? "pass" : "FAIL");
	Check(EffectiveFidelity(v.evidence) == "predictive", "viscosity earns predictive", EffectiveFidelity(v.evidence));
	Check(EffectiveFidelity(both.evidence) == "predictive", "viscosity and surface tension earn predictive", EffectiveFidelity(both.evidence));
	Check(EffectiveFidelity(st.evidence) == "preview", "surface tension without viscosity is preview");
	Check(v.evidence.cases.size() == 15 + 6 && both.evidence.cases.size() == 15 + 10 && st.evidence.cases.size() == 15 + 4, "the m7c cases are present when on",
		std::to_string(v.evidence.cases.size()) + " " + std::to_string(both.evidence.cases.size()) + " " + std::to_string(st.evidence.cases.size()));
	auto condition = [](const SolverDescription& d, const char* name) {
		for (const auto& c : d.evidence.conditions) if (c.first == name && !c.second.empty()) return c.second[0];
		return std::string();
	};
	Check(condition(both, "viscosity") == "on" && condition(both, "surface_tension") == "on" && condition(v, "surface_tension") == "off",
		"conditions viscosity and surface_tension");
	SolverDescription off;
	Check(s->Configure(Map(Small()), EmptySceneInputs(), off, error) && condition(off, "viscosity") == "off" && off.evidence.cases.size() == 15,
		"both off: m7b's cases, conditions off", error);
	Check(Encoded(*v.effectiveParameters.Find("viscosity")) == Encoded(V::Float(0.5)) && off.effectiveParameters.Find("surface_tension"),
		"the new parameters are effective (keyed)");
	for (const auto& bad : {With(Small(), "viscosity", V::Float(-1)), With(Small(), "surface_tension", V::Float(-0.1)),
		With(Small(), "viscous_walls", V::String("sticky")), With(Small(), "capillary_cfl", V::Float(0)), With(Small(), "viscosity_tolerance", V::Float(2)),
		With(Small(), "curvature", V::String("height"))}) {
		SolverDescription x;
		Check(!s->Configure(bad, EmptySceneInputs(), x, error), "refused: " + error);
	}
	// Threads, twins, stop / restore and macro steps with both options and a dynamic body.
	const V params = V::MapValue(Physics());
	std::vector<std::map<std::pair<std::int64_t,std::string>,std::string>> runs;
	for (unsigned threads : {1u, 3u, 8u, 8u}) {
		auto r = Make();
		SolverDescription d;
		DigestSink sink;
		Check(r->Configure(With(Physics(), "threads", V::Unsigned(threads)), EmptySceneInputs(), d, error) && r->Start(error) && RunAll(*r, sink, error),
			"physics run with " + std::to_string(threads) + " threads", error);
		runs.push_back(sink.digests);
	}
	Check(runs[0].size() == 12 && runs[0] == runs[1] && runs[0] == runs[2] && runs[2] == runs[3],
		"1, 3 and 8 threads and twins publish identical frames (viscosity, surface tension, a body)");
	StopRestore(params, "viscosity, surface tension and a body");
	auto m = Make();
	SolverDescription d;
	DigestSink sink;
	Check(m->Configure(params, EmptySceneInputs(), d, error) && m->Start(error), "macro configure", error);
	AdvanceResult r;
	for (int k = 0; k < 6; ++k) {
		AdvanceRequest q;
		q.mode = AdvanceRequest::Mode::MacroStep;
		q.macroEnd = k*d.cache.interval;
		Check(m->Advance(q, sink, nullptr, nullptr, r, error), "macro step", error);
	}
	Check(sink.digests == runs[0], "macro steps reproduce free stepping byte for byte (viscosity, surface tension, a body)");
	// Rollback.
	auto rb = Make();
	DigestSink one, two;
	Check(rb->Configure(params, EmptySceneInputs(), d, error) && rb->Start(error), "rollback configure", error);
	AdvanceRequest q;
	q.mode = AdvanceRequest::Mode::MacroStep;
	q.macroEnd = 0;
	Check(rb->Advance(q, one, nullptr, nullptr, r, error), "first output", error);
	std::shared_ptr<const SolverStateToken> token;
	Check(rb->SaveState(token, error), "save", error);
	q.macroEnd = 2*d.cache.interval;
	Check(rb->Advance(q, one, nullptr, nullptr, r, error) && rb->RestoreState(*token, error) && rb->Advance(q, two, nullptr, nullptr, r, error), "repeat", error);
	Check(one.digests.at({2, "surface"}) == two.digests.at({2, "surface"}) && two.digests.at({2, "surface"}) == runs[0].at({2, "surface"}),
		"the repeated macro step is identical (m7c)");
}

// ---------------------------------------------------------------- G

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
		if (name == "manifest.cbor" || (name.size() > 6 && name.compare(name.size() - 6, 6, ".frame") == 0)) files[name] = ReadAll(e.path());
	}
	return files;
}

void WorkerHost(const fs::path& root)
{
	std::printf("G: rise-sim bake, stop, resume\n");
	std::string log, error;
	std::error_code ec;
	fs::create_directories(root, ec);
	const auto params = root/"liquid.json";
	{
		std::ofstream f(params);
		f << "{\"frames\": 8, \"domain_min\": [0, 0, 0], \"domain_size\": [0.4, 0.2, 0.1], \"resolution\": 16, \"publish_particles\": true,"
			" \"liquid\": [{\"type\": \"box\", \"min\": [-1, -1, -1], \"max\": [0.15, 0.15, 1]}],"
			" \"solids\": [{\"type\": \"box\", \"min\": [0.3, 0, 0.02], \"max\": [0.35, 0.05, 0.08]}]}";
	}
	const auto reference = root/"reference", stopped = root/"stopped";
	Check(Worker({"bake", LiquidSolverId, params.string(), reference.string(), "--checkpoint-seconds", "0"}, &log) == WorkerFinished,
		"worker bakes the liquid", log);
	CacheSnapshot disk;
	Check(CacheSnapshot::Open(reference, budget, disk, error) && disk.Complete() && disk.Description().fidelity == "predictive" &&
		disk.Description().producer == LiquidSolverId, "a complete predictive liquid cache", error);
	Check(Worker({"bake", LiquidSolverId, params.string(), stopped.string(), "--stop-after-frames", "3"}, &log) == WorkerStopped,
		"worker stops after three frames", log);
	Check(Worker({"resume", stopped.string(), "--stop-after-frames", "2"}, &log) == WorkerStopped, "resume stops again", log);
	Check(Worker({"resume", stopped.string()}, &log) == WorkerFinished, "resume finishes", log);
	const auto x = Sealed(stopped), y = Sealed(reference);
	Check(x.size() > 1 && x == y, "worker stop and resume equals the uninterrupted bake byte for byte");
	// The in-process host publishes the same frames under the same key.
	InProcessHost host(Make());
	V job;
	Check(RISE::RISECBOR64::DecodeCanonical(ReadAll(reference/"rise-sim.job.cbor"), job, &error) && job.Find("parameters"), "job record", error);
	Check(host.Configure(*job.Find("parameters"), EmptySceneInputs(), error) && host.Start(error) && host.StepToIndex(1 << 20, error) &&
		host.Description().cache.key == disk.Description().key && Digests(*host.Snapshot()) == Digests(disk),
		"in-process and worker hosts publish identical frames", error);
}
} // namespace

int main()
{
	std::string error;
	Check(RegisterLiquidSolver(error) && RegisterLiquidSolver(error), "registration is idempotent", error);
	const double t0 = Seconds();
	Description();
	Frames();
	Determinism();
	StopRestore(With(Small(), "particles", V::Bool(false)), "level set");
	StopRestore(Map(Small()), "with marker particles (the default)");
	MacroAndRollback();
	ViscosityAndSurfaceTension();
	WorkerHost(RISE::TestSupport::ScratchRunDir("rise_liquid_solver_", "worker"));
	std::printf("%s (%d failures, %.1f s)\n", failures ? "FAILED" : "PASSED", failures, Seconds() - t0);
	return failures ? 1 : 0;
}
