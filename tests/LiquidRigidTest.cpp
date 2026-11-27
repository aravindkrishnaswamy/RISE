// liquid.mac with rigid bodies (simulation M7b, docs/SIMULATION_M7B_RESULTS.md):
// the coupled pressure solve and its group integration.
//
//   A  the coupled operator A + rho h^3 J^T M^-1 J is symmetric and positive
//      definite (dense, 2-D and 3-D), and its body term is the J the coupling
//      assembles (rank <= 6)
//   B  description: bodies output, ports and ownedImplicitPorts, the body
//      evidence and conditions, the label (preview for a body below four
//      cells and for one within two cells of a wall), refusals, the chunk
//      translation of `body`
//   C  determinism: 1, 3 and 8 threads give identical bytes (dynamic and
//      prescribed bodies, marker particles on)
//   D  stop / restore mid-segment (in-memory and streamed) and rollback with
//      bodies equal the uninterrupted run
//   E  group: the liquid owning rigid.bodies' exchange (implicit, predictive)
//      at density ratios 0.1 and 0.5 is stable and follows the liquid's own
//      dynamic body; the staggered exchange at 0.5 diverges (preview); the
//      momentum audit: the rigid body's momentum change equals the liquid's
//      offered impulse plus its external force; the group checkpoint resumes
//      byte for byte; a mismatched initial pose and a port body outside a
//      group are refused
#include "../src/Library/Simulation/Liquid/LiquidSolver.h"
#include "../src/Library/Simulation/Liquid/LiquidParameterSchema.h"
#include "../src/Library/Simulation/Liquid/LiquidValidation.h"
#include "../src/Library/Simulation/Rigid/RigidSolver.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationGroup.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationTransform.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>

namespace {
using namespace RISE::Simulation;
using V = RISE::RISECBOR64::Value;
constexpr std::size_t budget = std::size_t(1) << 30;
constexpr double Pi = 3.14159265358979323846;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }
double F32(std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

std::unique_ptr<ISimulationSolver> Make(const char* id)
{
	std::string error;
	auto s = CreateSolver(id, error);
	Check(s != nullptr, std::string("create ") + id, error);
	return s;
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

// A 2-D tank [0, 1]^2 one cell thick, filled to 0.5, with a horizontal
// cylinder (radius 0.15, axis z, through the slab) at y.
V::Members Tank(int n, std::uint64_t frames)
{
	const double h = 1.0/n;
	return {{"frames", V::Unsigned(frames)}, {"domain_min", Vec(0, 0, -0.5*h)}, {"domain_size", Vec(1, 1, h)},
		{"resolution", V::Unsigned(std::uint64_t(n))},
		{"liquid", V::ArrayValue({V::MapValue({{"type", V::String("fill")}, {"height", V::Float(0.5)}})})}};
}
V Cylinder(const std::string& name, int n, double y, V::Members extra)
{
	V::Members m{{"name", V::String(name)}, {"shape", V::String("cylinder")}, {"radius", V::Float(0.15)},
		{"height", V::Float(4.0/n)}, {"position", Vec(0.5, y, 0)}};
	for (auto& e : extra) m.push_back(e);
	return V::MapValue(m);
}
V With(V::Members m, const char* key, V value)
{
	for (auto& e : m) if (e.first == key) { e.second = value; return V::MapValue(m); }
	m.emplace_back(key, value);
	return V::MapValue(m);
}
double FloatY(double ratio) { return 0.5 + 0.15 - Liquid::FloatingDepth(false, 0.15, ratio); }

// ---------------------------------------------------------------- A

// Dense operator over the active cells; returns false if not symmetric positive definite.
void OperatorCase(bool threeD)
{
	using namespace Liquid;
	const int n = threeD ? 10 : 16;
	LiquidSetup s;
	s.grid.n = {{n, n, threeD ? n : 1}};
	s.grid.h = 1.0/n;
	s.grid.origin = {{0, 0, threeD ? 0.0 : -0.5/n}};
	Shape fill;
	fill.type = Shape::Type::Plane;
	fill.point = {{0, 0.5, 0}};
	fill.axis = {{0, 1, 0}};
	s.liquid.push_back(fill);
	BodyShape shape;
	shape.type = threeD ? BodyShape::Type::Sphere : BodyShape::Type::Cylinder;
	shape.radius = 0.23;
	shape.height = 4.0/n;
	const double R = 0.23, V = threeD ? 4.0/3.0*Pi*R*R*R : Pi*R*R/n;
	LiquidBody body = MakeBody(shape, BodyMotion::Dynamic, 0.1*1000*V, {{0.47, 0.55, threeD ? 0.52 : 0.0}}, s.gravity);
	body.state.q = {{std::cos(0.2), 0.3*std::sin(0.2), 0.5*std::sin(0.2), std::sqrt(1 - 0.34)*std::sin(0.2)}};
	s.bodies.push_back(body);
	SolverOptions o;
	o.particles = false;
	LiquidModel m;
	std::string error;
	Check(m.Initialize(s, o, 1, error), "operator model", error);
	const Grid& g = m.GridSpec();
	BodyCoupling c(g, m.Geometry().Faces()[0], m.bodies[0], s.density, true);
	std::vector<IPressureCoupling*> cs{&c};
	std::vector<std::uint8_t> active;
	std::vector<double> x(g.Cells(), 0.0), y;
	ApplyPressureOperator(g, m.phi, m.fraction, o.pressure, cs, x, y, active, nullptr);
	std::vector<std::size_t> cells;
	for (std::size_t k = 0; k < g.Cells(); ++k) if (active[k]) cells.push_back(k);
	const std::size_t N = cells.size();
	std::vector<double> A(N*N), G(N*N);
	for (std::size_t j = 0; j < N; ++j) {
		std::fill(x.begin(), x.end(), 0.0);
		x[cells[j]] = 1;
		ApplyPressureOperator(g, m.phi, m.fraction, o.pressure, cs, x, y, active, nullptr);
		for (std::size_t i = 0; i < N; ++i) A[i*N + j] = y[cells[i]];
		ApplyPressureOperator(g, m.phi, m.fraction, o.pressure, {}, x, y, active, nullptr);
		for (std::size_t i = 0; i < N; ++i) G[i*N + j] = y[cells[i]];
	}
	double scale = 0, asym = 0, termError = 0;
	for (double v : A) scale = std::max(scale, std::fabs(v));
	for (std::size_t i = 0; i < N; ++i) for (std::size_t j = 0; j < N; ++j) asym = std::max(asym, std::fabs(A[i*N + j] - A[j*N + i]));
	// The body term rebuilt from the coupling's entries: rho h^3 J^T M^-1 J.
	std::map<std::size_t, std::size_t> row;
	for (std::size_t i = 0; i < N; ++i) row[cells[i]] = i;
	const auto& entries = c.Entries();
	const double sc = s.density*g.h*g.h*g.h;
	for (const auto& ei : entries) for (const auto& ej : entries) {
		Vec6 v = ej.second;
		const Vec6 mv = c.InverseMass(v);
		double t = 0;
		for (int k = 0; k < 6; ++k) t += ei.second[std::size_t(k)]*mv[std::size_t(k)];
		t *= sc;
		const std::size_t i = row.at(ei.first), j = row.at(ej.first);
		termError = std::max(termError, std::fabs(A[i*N + j] - G[i*N + j] - t));
	}
	// Cholesky: positive definite iff every pivot is positive.
	std::vector<double> L = A;
	double minPivot = 1e300;
	bool spd = true;
	for (std::size_t k = 0; k < N && spd; ++k) {
		double d = L[k*N + k];
		for (std::size_t p = 0; p < k; ++p) d -= L[k*N + p]*L[k*N + p];
		if (!(d > 0)) { spd = false; break; }
		minPivot = std::min(minPivot, d);
		const double r = std::sqrt(d);
		L[k*N + k] = r;
		for (std::size_t i = k + 1; i < N; ++i) {
			double v = L[i*N + k];
			for (std::size_t p = 0; p < k; ++p) v -= L[i*N + p]*L[k*N + p];
			L[i*N + k] = v/r;
		}
	}
	std::printf("A (%s): %zu unknowns, %zu cells in J, max |A - A^T| / max |A| = %.1e, body term error %.1e, Cholesky %s (min pivot %.3e)\n",
		threeD ? "3-D" : "2-D", N, entries.size(), asym/scale, termError/scale, spd ? "succeeds" : "FAILS", minPivot);
	Check(asym <= 1e-14*scale, "the coupled operator is symmetric");
	Check(termError <= 1e-12*scale, "the body term is rho h^3 J^T M^-1 J of the assembled J");
	Check(spd, "the coupled operator is positive definite");
	Check(entries.size() > 6, "J reaches the body's cut cells");
}

// ---------------------------------------------------------------- B

void Description()
{
	std::printf("B: description, label, refusals, chunk\n");
	std::string error;
	auto s = Make(LiquidSolverId);
	SolverDescription d;
	V::Members p = Tank(32, 4);
	p.emplace_back("bodies", V::ArrayValue({Cylinder("float", 32, FloatY(0.5), {{"density", V::Float(500)}, {"free", V::ArrayValue({V::Bool(false), V::Bool(true), V::Bool(false), V::Bool(false)})}})}));
	const double t0 = Seconds();
	Check(s->Configure(V::MapValue(p), EmptySceneInputs(), d, error), "configure with a body", error);
	std::printf("  configure with bodies (evidence) %.2f s; version %s\n", Seconds() - t0, d.version.c_str());
	Check(d.version == "m7l.1", "version m7l.1");
	bool bodiesOut = false;
	for (const auto& o : d.cache.outputs) if (o.name == "bodies" && o.type == TransformCodec) bodiesOut = true;
	Check(bodiesOut, "the bodies output (transform.v1)");
	Check(d.ports.empty() && d.capabilities.ownedImplicitPorts.empty(), "a dynamic body declares no port");
	int bodyCases = 0;
	for (const auto& c : d.evidence.cases) {
		if (c.id.find("added-mass") == 0 || c.id.find("archimedes") == 0 || c.id == "neutral-rest-n32" || c.id == "light-float-n32" ||
			c.id == "momentum-audit-n32" || c.id == "body-impulse-n32") {
			++bodyCases;
			std::printf("  %-28s %.3e (tolerance %.1e) %s\n", c.id.c_str(), c.error, c.tolerance, c.passed ? "pass" : "FAIL");
			Check(c.passed, "body evidence " + c.id);
		}
	}
	Check(bodyCases == 10, "ten body evidence cases");
	bool condition = false;
	for (const auto& c : d.evidence.conditions) if (c.first == "bodies" && c.second == std::vector<std::string>{"dynamic"}) condition = true;
	Check(condition, "condition bodies: dynamic");
	Check(EffectiveFidelity(d.evidence) == "predictive", "a resolved floating body is predictive", d.capabilities.fidelityLimit);
	// Preview: a body below four cells; a body within two cells of a wall.
	{
		V::Members q = Tank(32, 4);
		q.emplace_back("bodies", V::ArrayValue({V::MapValue({{"name", V::String("pebble")}, {"shape", V::String("sphere")},
			{"radius", V::Float(0.05)}, {"position", Vec(0.5, 0.3, 0)}})}));
		SolverDescription x;
		Check(s->Configure(V::MapValue(q), EmptySceneInputs(), x, error) && EffectiveFidelity(x.evidence) == "preview" &&
			x.capabilities.fidelityLimit.find("smaller than four cells") != std::string::npos, "a body below four cells is preview", x.capabilities.fidelityLimit);
		V::Members w = Tank(32, 4);
		w.emplace_back("bodies", V::ArrayValue({Cylinder("close", 32, 0.18, {})}));
		Check(s->Configure(V::MapValue(w), EmptySceneInputs(), x, error) && EffectiveFidelity(x.evidence) == "preview" &&
			x.capabilities.fidelityLimit.find("within two cells") != std::string::npos, "a body within two cells of a wall is preview", x.capabilities.fidelityLimit);
	}
	// Port bodies: ports; implicit declares the owned ports, staggered does not.
	{
		V::Members q = Tank(32, 4);
		q.emplace_back("bodies", V::ArrayValue({Cylinder("float", 32, FloatY(0.5), {{"motion", V::String("port")}})}));
		SolverDescription x;
		Check(s->Configure(V::MapValue(q), EmptySceneInputs(), x, error), "a port body configures", error);
		// m7h: and the optional body_contact (rigid.bodies' planes and contact parameters).
		Check(x.ports.size() == 4 && x.capabilities.ownedImplicitPorts == std::vector<std::string>{"body_contact", "body_load", "body_mass_properties", "body_state"},
			"a port body declares body_state, body_mass_properties, body_load and body_contact as owned implicit ports");
		V::Members st = Tank(32, 4);
		st.emplace_back("bodies", V::ArrayValue({Cylinder("float", 32, FloatY(0.5), {{"motion", V::String("port")}, {"coupling", V::String("staggered")}})}));
		Check(s->Configure(V::MapValue(st), EmptySceneInputs(), x, error) && x.ports.size() == 4 && x.capabilities.ownedImplicitPorts.empty(),
			"a staggered port body declares no owned port");
		// Outside a group a port body cannot run.
		Check(s->Start(error), "start the staggered configuration", error);
		DigestSink sink;
		AdvanceResult r;
		Check(!s->Advance(AdvanceRequest{}, sink, nullptr, nullptr, r, error) && error.find("simulation_group") != std::string::npos,
			"a port body outside a group is refused", error);
	}
	// Refusals.
	for (const V& bad : {V::MapValue({{"name", V::String("a")}, {"shape", V::String("cone")}}),
		V::MapValue({{"name", V::String("a")}, {"radius", V::Float(-1)}}),
		V::MapValue({{"name", V::String("a")}, {"density", V::Float(500)}, {"mass", V::Float(1)}}),
		V::MapValue({{"name", V::String("")}}),
		V::MapValue({{"name", V::String("a")}, {"orientation", V::ArrayValue({V::Float(0), V::Float(0), V::Float(0), V::Float(0)})}}),
		V::MapValue({{"name", V::String("a")}, {"motion", V::String("prescribed")}, {"frequency", V::Float(-1)}}),
		V::MapValue({{"name", V::String("a")}, {"motion", V::String("port")}, {"coupling", V::String("loose")}}),
		V::MapValue({{"name", V::String("a")}, {"free", V::ArrayValue({V::Bool(true)})}}),
		V::MapValue({{"name", V::String("a")}, {"wings", V::Float(2)}})}) {
		V::Members q = Tank(16, 2);
		q.emplace_back("bodies", V::ArrayValue({bad}));
		SolverDescription x;
		Check(!s->Configure(V::MapValue(q), EmptySceneInputs(), x, error), "refused body: " + error);
	}
	{
		V::Members q = Tank(16, 2);
		q.emplace_back("bodies", V::ArrayValue({Cylinder("twin", 16, 0.6, {}), Cylinder("twin", 16, 0.3, {})}));
		SolverDescription x;
		Check(!s->Configure(V::MapValue(q), EmptySceneInputs(), x, error) && error.find("unique") != std::string::npos, "duplicate body names refused", error);
	}
	// The chunk translation.
	{
		const ParameterSchema& schema = LiquidParameterSchema();
		SchemaFieldValues values{{"body", {"ball sphere 0.1 density 300 at 0 0.4 0 lock xzr", "crate box 0.2 0.2 0.2 motion port coupling staggered at 0.3 0.4 0",
			"stirrer cylinder 0.05 0.3 motion prescribed amplitude 0.02 0 0 frequency 2 phase 0.5 spin 0 0 1 orientation 1 0 0 0 velocity 0 0 0"}}};
		V params;
		std::vector<std::string> inputs;
		Check(TranslateSchemaChunk(schema, values, params, inputs, error) && params.Find("bodies") && params.Find("bodies")->GetArray().size() == 3,
			"body lines translate", error);
		const V& ball = params.Find("bodies")->GetArray()[0];
		const V* free = ball.Find("free");
		Check(free && free->GetArray().size() == 4 && !free->GetArray()[0].GetBoolean() && free->GetArray()[1].GetBoolean() &&
			!free->GetArray()[2].GetBoolean() && !free->GetArray()[3].GetBoolean(), "lock xzr frees y only");
		V::Members q = Tank(32, 2);
		q.emplace_back("bodies", *params.Find("bodies"));
		SolverDescription x;
		Check(s->Configure(V::MapValue(q), EmptySceneInputs(), x, error), "the translated bodies configure", error);
		Check(!TranslateSchemaChunk(schema, {{"body", {"ball sphere 0.1 colour red"}}}, params, inputs, error), "an unknown body option is refused");
	}
}

// ---------------------------------------------------------------- C, D

V::Members Mixed(int n)
{
	V::Members p = Tank(n, 5);
	p.emplace_back("bodies", V::ArrayValue({
		Cylinder("float", n, FloatY(0.3) + 0.02, {{"density", V::Float(300)}}),
		V::MapValue({{"name", V::String("stirrer")}, {"shape", V::String("box")}, {"size", Vec(0.12, 0.12, 4.0/n)},
			{"motion", V::String("prescribed")}, {"position", Vec(0.2, 0.25, 0)}, {"amplitude", Vec(0.03, 0, 0)}, {"frequency", V::Float(1.5)},
			{"angular_velocity", Vec(0, 0, 1)}})}));
	return p;
}

void Determinism()
{
	std::printf("C: threads\n");
	std::string error;
	DigestSink reference;
	for (int threads : {1, 3, 8}) {
		auto s = Make(LiquidSolverId);
		SolverDescription d;
		DigestSink sink;
		Check(s->Configure(With(Mixed(24), "threads", V::Unsigned(std::uint64_t(threads))), EmptySceneInputs(), d, error) && s->Start(error) &&
			RunAll(*s, sink, error), "run with threads", error);
		if (threads == 1) reference = sink;
		else Check(sink.digests == reference.digests, std::to_string(threads) + " threads publish the bytes of one thread");
	}
	Check(reference.digests.count({4, "bodies"}) == 1, "the bodies output is published");
	TransformFrame f;
	Check(DecodeTransforms(*reference.payloads.at({4, "bodies"}), budget, f, error) && f.bodies.size() == 2 && f.bodies[0].name == "float",
		"bodies frames decode (sorted by name)", error);
	if (f.bodies.size() == 2) {
		const double x = 0.2 + 0.03*std::sin(2*Pi*1.5*f.time);
		Check(std::fabs(f.bodies[1].position[0] - x) < 1e-12 && std::fabs(f.bodies[1].angularVelocity[2] - 1) < 1e-12,
			"the prescribed body follows its motion", std::to_string(f.bodies[1].position[0] - x));
	}
}

void StopRestore()
{
	std::printf("D: stop / restore, rollback\n");
	std::string error;
	const V params = V::MapValue(Mixed(24));
	DigestSink reference;
	{
		auto s = Make(LiquidSolverId);
		SolverDescription d;
		Check(s->Configure(params, EmptySceneInputs(), d, error) && s->Start(error) && RunAll(*s, reference, error), "reference", error);
	}
	for (int streamed = 0; streamed < 2; ++streamed) {
		auto a = Make(LiquidSolverId);
		SolverDescription d;
		DigestSink first;
		Check(a->Configure(params, EmptySceneInputs(), d, error) && a->Start(error), "configure", error);
		AdvanceResult r;
		Check(a->Advance(AdvanceRequest{}, first, nullptr, nullptr, r, error) && a->Advance(AdvanceRequest{}, first, nullptr, nullptr, r, error), "two frames", error);
		class StopAfter final : public IProgress {
		public:
			int calls = 0;
			bool Report(double, double, const std::string&) override { return ++calls < 3; }
		} stop;
		Check(a->Advance(AdvanceRequest{}, first, nullptr, &stop, r, error) && r.status == AdvanceResult::Status::Stopped, "stopped mid-segment", error);
		auto b = Make(LiquidSolverId);
		SolverDescription d2;
		Check(b->Configure(params, EmptySceneInputs(), d2, error), "configure the restore", error);
		if (streamed) {
			MemoryCheckpointSink sink;
			Check(a->CheckpointTo(sink, error), "streamed checkpoint", error);
			MemoryCheckpointSource source(sink.bytes);
			Check(b->RestoreFrom(source, error), "streamed restore", error);
		} else {
			Bytes state;
			Check(a->Checkpoint(state, error) && b->Restore(state, error), "checkpoint and restore", error);
		}
		DigestSink rest;
		Check(RunAll(*b, rest, error), "finish after restore", error);
		auto merged = first.digests;
		for (const auto& e : rest.digests) merged[e.first] = e.second;
		Check(merged == reference.digests, std::string(streamed ? "streamed" : "in-memory") + " stop / restore with bodies equals the uninterrupted run");
	}
	// A checkpoint without bodies is refused by a configuration with them.
	{
		auto plain = Make(LiquidSolverId);
		SolverDescription d;
		Bytes state;
		Check(plain->Configure(V::MapValue(Tank(24, 5)), EmptySceneInputs(), d, error) && plain->Start(error) && plain->Checkpoint(state, error), "plain checkpoint", error);
		auto b = Make(LiquidSolverId);
		Check(b->Configure(params, EmptySceneInputs(), d, error) && !b->Restore(state, error), "a checkpoint without the bodies is refused");
	}
	// Rollback.
	auto rb = Make(LiquidSolverId);
	SolverDescription d;
	DigestSink one, two;
	Check(rb->Configure(params, EmptySceneInputs(), d, error) && rb->Start(error), "rollback configure", error);
	AdvanceRequest q;
	q.mode = AdvanceRequest::Mode::MacroStep;
	q.macroEnd = d.cache.interval;
	AdvanceResult r;
	Check(rb->Advance(q, one, nullptr, nullptr, r, error), "first", error);
	std::shared_ptr<const SolverStateToken> token;
	Check(rb->SaveState(token, error), "save", error);
	q.macroEnd = 3*d.cache.interval;
	Check(rb->Advance(q, one, nullptr, nullptr, r, error), "step", error);
	Check(rb->RestoreState(*token, error), "restore", error);
	Check(rb->Advance(q, two, nullptr, nullptr, r, error), "repeat", error);
	Check(one.digests.at({3, "surface"}) == two.digests.at({3, "surface"}) && one.digests.at({3, "bodies"}) == two.digests.at({3, "bodies"}) &&
		two.digests.at({3, "bodies"}) == reference.digests.at({3, "bodies"}), "the repeated macro step is identical (surface and bodies)");
}

// ---------------------------------------------------------------- E

struct GroupRun {
	std::unique_ptr<SimulationGroup> group;
	bool ok = false;
	std::string error;
	std::vector<double> t, y, vy;
	double maxSpeed = 0, auditWorst = 0;
};

V::Members RigidParams(int n, double ratio, double y, std::uint64_t frames)
{
	const double h = 1.0/n;
	return {{"frames", V::Unsigned(frames)}, {"substeps", V::Unsigned(8)},
		{"bodies", V::ArrayValue({V::MapValue({{"name", V::String("float")}, {"shape", V::String("cylinder")}, {"radius", V::Float(0.15)},
			{"height", V::Float(4*h)}, {"mass", V::Float(ratio*1000*Pi*0.15*0.15*h)}, {"position", Vec(0.5, y, 0)}})})}};
}

GroupRun Group(int n, double ratio, double T, const char* coupling, double rigidY = -1, bool run = true, bool start = true)
{
	GroupRun g;
	const double y0 = FloatY(ratio) + 0.015;
	const auto frames = std::uint64_t(std::llround(T*30)) + 1;
	V::Members liquid = Tank(n, frames);
	liquid.emplace_back("bodies", V::ArrayValue({Cylinder("float", n, y0, {{"motion", V::String("port")}, {"coupling", V::String(coupling)}})}));
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.memberOrder = {"pool", "drop"};
	if (std::string(coupling) == "implicit") p.ownedExchanges = {{"pool", {"drop"}}};
	g.group = std::make_unique<SimulationGroup>();
	auto& G = *g.group;
	g.ok = G.AddMember("pool", Make(LiquidSolverId), V::MapValue(liquid), g.error) &&
		G.AddMember("drop", Make(RigidSolverId), V::MapValue(RigidParams(n, ratio, rigidY >= 0 ? rigidY : y0, frames)), g.error) &&
		G.Connect({"drop", "body_state", "pool", "body_state"}, g.error) &&
		G.Connect({"drop", "body_mass_properties", "pool", "body_mass_properties"}, g.error) &&
		G.Connect({"pool", "body_load", "drop", "body_load"}, g.error) && G.Configure(p, EmptySceneInputs(), g.error) && (!start || G.Start(g.error));
	if (!g.ok || !run) return g;
	const double mass = ratio*1000*Pi*0.15*0.15/n, H = 1.0/30;
	double lastV = 0;
	std::size_t loadLink = 2;
	G.SetStepObserver([&](double t, const SimulationGroup& gr) {
		const auto s = gr.Snapshot("drop");
		std::shared_ptr<const Bytes> bytes;
		std::string e;
		CacheEntry entry;
		const auto index = static_cast<std::int64_t>(std::llround(t*30));
		if (!s->FindEntry(index, "bodies", entry) || s->Read(index, "bodies", budget, bytes, e) != FrameStatus::Ready) return;
		TransformFrame f;
		if (!DecodeTransforms(*bytes, budget, f, e)) return;
		const double v = f.bodies[0].velocity[1];
		g.t.push_back(t);
		g.y.push_back(f.bodies[0].position[1]);
		g.vy.push_back(v);
		g.maxSpeed = std::max(g.maxSpeed, std::fabs(v));
		// The audit: m (v1 - v0) = H (F_offered + m g) over the step (binary32 load).
		if (const auto load = gr.ServedValue(loadLink)) {
			FieldFrame lf;
			if (DecodeField(*load, budget, lf, e)) {
				double F = 0;
				for (const auto& c : lf.channels) if (c.name == "force") F = F32(c.values[1]);
				const double residual = std::fabs(mass*(v - lastV) - H*(F - mass*9.81));
				g.auditWorst = std::max(g.auditWorst, residual/(mass*9.81*H));
			}
		}
		lastV = v;
	});
	while (!G.Finished())
		if (!G.StepMacro(g.error)) { g.ok = false; return g; }
	return g;
}

void Groups()
{
	std::printf("E: group (rigid.bodies owned by the liquid)\n");
	std::string error;
	for (double ratio : {0.1, 0.5}) {
		const double t0 = Seconds();
		GroupRun g = Group(32, ratio, 1.5, "implicit");
		Check(g.ok, "owned group runs", g.error);
		if (!g.ok) continue;
		Check(g.group->MemberFidelity("pool") == "predictive" && g.group->MemberFidelity("drop") == "predictive", "owned implicit group is predictive",
			g.group->CouplingLimit("drop"));
		const double y0 = FloatY(ratio) + 0.015;
		double worst = 0;
		for (double y : g.y) worst = std::max(worst, std::fabs(y - FloatY(ratio)));
		// The liquid's own dynamic body, all degrees of freedom free: the same motion.
		V::Members solo = Tank(32, 46);
		solo.emplace_back("bodies", V::ArrayValue({Cylinder("float", 32, y0, {{"mass", V::Float(ratio*1000*Pi*0.15*0.15/32)}})}));
		auto s = Make(LiquidSolverId);
		SolverDescription d;
		DigestSink sink;
		Check(s->Configure(V::MapValue(solo), EmptySceneInputs(), d, error) && s->Start(error) && RunAll(*s, sink, error), "solo run", error);
		double diff = 0;
		for (std::size_t k = 0; k < g.t.size(); ++k) {
			const auto index = static_cast<std::int64_t>(std::llround(g.t[k]*30));
			TransformFrame f;
			if (!sink.payloads.count({index, "bodies"}) || !DecodeTransforms(*sink.payloads.at({index, "bodies"}), budget, f, error)) continue;
			diff = std::max(diff, std::fabs(f.bodies[0].position[1] - g.y[k]));
		}
		std::printf("  ratio %.1f implicit: max |y - y_eq| %.4f m (start %.4f), max |v| %.3f m/s, |group - liquid alone| %.2e m, "
			"audit %.1e, %.1f s\n", ratio, worst, std::fabs(y0 - FloatY(ratio)), g.maxSpeed, diff, g.auditWorst, Seconds() - t0);
		Check(worst < 1.25*std::fabs(y0 - FloatY(ratio)), "the owned exchange is stable at density ratio " + std::to_string(ratio));
		Check(diff < 0.02*0.15, "the group follows the liquid's own coupled body (within 2% of R)", std::to_string(diff));
		Check(g.auditWorst < 1e-5, "the rigid body's momentum change is the liquid's impulse plus m g (binary32 load)", std::to_string(g.auditWorst));
	}
	{
		const double t0 = Seconds();
		GroupRun g = Group(32, 0.5, 1.0, "staggered");
		const double implicitMax = 0.1;   // the implicit run's velocities stay below this (above: ~0.07)
		std::printf("  ratio 0.5 staggered: %s, max |v| %.3g m/s, labels %s / %s, %.1f s\n", g.ok ? "ran" : ("failed: " + g.error).c_str(),
			g.maxSpeed, g.group ? g.group->MemberFidelity("pool").c_str() : "-", g.group ? g.group->MemberFidelity("drop").c_str() : "-", Seconds() - t0);
		Check(g.group && g.group->MemberFidelity("drop") == "preview" && g.group->CouplingLimit("drop").find("staggered") != std::string::npos,
			"the staggered exchange is preview");
		Check(!g.ok || g.maxSpeed > 10*implicitMax, "the staggered exchange at density ratio 0.5 diverges (the added-mass instability)");
	}
	// A rigid body that does not start where the liquid has it is refused at the first exchange.
	{
		GroupRun g = Group(16, 0.5, 0.2, "implicit", 0.3);
		Check(!g.ok && g.error.find("does not start") != std::string::npos, "a mismatched initial pose is refused", g.error);
	}
	// The group checkpoint (members' streams) resumes byte for byte.
	{
		GroupRun a = Group(24, 0.5, 0.3, "implicit", -1, false), ref = Group(24, 0.5, 0.3, "implicit", -1, false);
		Check(a.ok && ref.ok, "checkpoint groups", a.error + ref.error);
		std::string e;
		while (ref.ok && !ref.group->Finished()) Check(ref.group->StepMacro(e), "reference macro", e);
		for (int k = 0; k < 4 && a.ok; ++k) Check(a.group->StepMacro(e), "first macro steps", e);
		MemoryCheckpointSink sink;
		Check(a.group->CheckpointTo(sink, e), "group checkpoint", e);
		GroupRun b = Group(24, 0.5, 0.3, "implicit", -1, false, false);
		MemoryCheckpointSource source(sink.bytes);
		Check(b.ok && b.group->RestoreFrom(source, e), "group restore", e);
		while (b.ok && !b.group->Finished()) Check(b.group->StepMacro(e), "resumed macro", e);
		for (const char* member : {"pool", "drop"}) {
			std::map<std::pair<std::int64_t,std::string>,std::string> x, y;
			for (const auto& en : ref.group->Snapshot(member)->Entries()) x[{en.index, en.output}] = en.digest;
			for (const auto& en : b.group->Snapshot(member)->Entries()) if (en.index >= 5) y[{en.index, en.output}] = en.digest;
			for (auto it = x.begin(); it != x.end();) it = it->first.first < 5 ? x.erase(it) : std::next(it);
			Check(!x.empty() && x == y, std::string("the resumed group publishes ") + member + "'s frames byte for byte");
		}
	}
}
}

// ---------------------------------------------------------------- m7g probe: a dense sphere settling on the floor
// --settle [cells n] [T s] [density kg/m^3] [restitution e]: a 3-D tank 0.32 m on a side, water to
// 0.24 m, a sphere of radius 0.04 m released at rest at y = 0.15 m, rigid.bodies owned by the liquid
// (implicit), the floor a rigid.bodies plane.  Per macro step: the sphere's height and velocity,
// the liquid's offered force F, and the momentum residual m (v1 - v0) - H (F - m g): what
// rigid.bodies' contact gave.  At rest the liquid should carry the buoyancy rho V g and the floor
// the rest, (m - rho V) g.
void SettleProbe(int argc, char** argv)
{
	int n = 40;
	double T = 2, rho = 2500, e = 0, friction = 0.3;
	for (int i = 2; i + 1 < argc; i += 2) {
		const std::string k = argv[i], v = argv[i + 1];
		if (k == "cells") n = std::atoi(v.c_str());
		else if (k == "T") T = std::atof(v.c_str());
		else if (k == "density") rho = std::atof(v.c_str());
		else if (k == "restitution") e = std::atof(v.c_str());
	}
	const double R = 0.04, L = 0.32, H = 1.0/30, Vs = 4.0/3.0*Pi*R*R*R, m = rho*Vs, g = 9.81;
	const auto frames = std::uint64_t(std::llround(T*30)) + 1;
	V::Members liquid{{"frames", V::Unsigned(frames)}, {"domain_min", Vec(0, 0, 0)}, {"domain_size", Vec(L, L, L)},
		{"resolution", V::Unsigned(std::uint64_t(n))},
		{"liquid", V::ArrayValue({V::MapValue({{"type", V::String("fill")}, {"height", V::Float(0.24)}})})},
		{"bodies", V::ArrayValue({V::MapValue({{"name", V::String("ball")}, {"shape", V::String("sphere")}, {"radius", V::Float(R)},
			{"position", Vec(0.16, 0.15, 0.16)}, {"motion", V::String("port")}, {"coupling", V::String("implicit")}})})}};
	V::Members rigid{{"frames", V::Unsigned(frames)}, {"substeps", V::Unsigned(8)},
		{"bodies", V::ArrayValue({V::MapValue({{"name", V::String("ball")}, {"shape", V::String("sphere")}, {"radius", V::Float(R)},
			{"mass", V::Float(m)}, {"position", Vec(0.16, 0.15, 0.16)}})})},
		{"planes", V::ArrayValue({V::MapValue({{"point", Vec(0, 0, 0)}, {"normal", Vec(0, 1, 0)}, {"restitution", V::Float(e)}, {"friction", V::Float(friction)}})})}};
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.memberOrder = {"pool", "drop"};
	p.ownedExchanges = {{"pool", {"drop"}}};
	SimulationGroup G;
	std::string error;
	bool ok = G.AddMember("pool", Make(LiquidSolverId), V::MapValue(liquid), error) &&
		G.AddMember("drop", Make(RigidSolverId), V::MapValue(rigid), error) &&
		G.Connect({"drop", "body_state", "pool", "body_state"}, error) &&
		G.Connect({"drop", "body_mass_properties", "pool", "body_mass_properties"}, error) &&
		G.Connect({"pool", "body_load", "drop", "body_load"}, error) && G.Configure(p, EmptySceneInputs(), error) && G.Start(error);
	if (!ok) { std::printf("failed: %s\n", error.c_str()); return; }
	std::vector<double> ts, ys, vys, Fs, Js;
	double lastV = 0;
	G.SetStepObserver([&](double t, const SimulationGroup& gr) {
		const auto s = gr.Snapshot("drop");
		std::shared_ptr<const Bytes> bytes;
		std::string w;
		CacheEntry entry;
		const auto index = static_cast<std::int64_t>(std::llround(t*30));
		if (!s->FindEntry(index, "bodies", entry) || s->Read(index, "bodies", budget, bytes, w) != FrameStatus::Ready) return;
		TransformFrame f;
		if (!DecodeTransforms(*bytes, budget, f, w)) return;
		double F = 0;
		if (const auto load = gr.ServedValue(2)) {
			FieldFrame lf;
			if (DecodeField(*load, budget, lf, w)) for (const auto& c : lf.channels) if (c.name == "force") F = F32(c.values[1]);
		}
		const double v = f.bodies[0].velocity[1];
		ts.push_back(t); ys.push_back(f.bodies[0].position[1]); vys.push_back(v); Fs.push_back(F);
		Js.push_back(m*(v - lastV) - H*(F - m*g));
		lastV = v;
	});
	const double w0 = Seconds();
	while (!G.Finished()) if (!G.StepMacro(error)) { std::printf("failed at t %.3f: %s\n", G.Time(), error.c_str()); break; }
	const double wall = Seconds() - w0;
	for (std::size_t k = 0; k < ts.size(); ++k)
		if (k % 3 == 0 || k + 1 == ts.size())
			std::printf("  t %.3f y %.5f (gap %+.2e m) vy %+.4f F %.3f N (rho V g %.3f) floor impulse %.3e N s (%.3f N)\n", ts[k], ys[k], ys[k] - R, vys[k], Fs[k],
				1000*Vs*g, Js[k], Js[k]/H);
	// Rest: the last third.
	double my = 0, mF = 0, mJ = 0, sv = 0, cnt = 0, ymin = 1e9, ymax = -1e9;
	for (std::size_t k = 0; k < ts.size(); ++k) if (ts[k] > 2*T/3) { my += ys[k]; mF += Fs[k]; mJ += Js[k]/H; sv += vys[k]*vys[k]; ++cnt; ymin = std::min(ymin, ys[k]); ymax = std::max(ymax, ys[k]); }
	if (cnt > 0)
		std::printf("settle cells %d density %g e %g T %g: rest (last third) y - R %+.2e m (range %.2e), rms vy %.2e m/s, liquid force %.3f N (buoyancy %.3f), floor force %.3f N (expected %.3f), "
			"sum %.3f N (weight %.3f); %.1f s\n", n, rho, e, T, my/cnt - R, ymax - ymin, std::sqrt(sv/cnt), mF/cnt, 1000*Vs*g, mJ/cnt, (m - 1000*Vs)*g, mF/cnt + mJ/cnt, m*g, wall);
}

// ---------------------------------------------------------------- m7h: two bodies colliding under water
// Tank 0.32 m, water to 0.28 m, two spheres (R = 0.035 m, 1500 kg/m^3) at mid depth moving towards
// each other at 0.4 m/s each, rigid.bodies owned by the liquid (implicit) with a floor plane and,
// with `contact`, rigid.bodies' body_contact connected (its planes and body-body contact for the
// liquid's own copies too).  Measured: the least centre distance against 2R, the largest body speed,
// and the momentum account along x per macro step: sum m dv - H sum F (F the liquid's offered
// force): the impulse of everything else, which along x is the body-body contact alone (equal and
// opposite: zero for the pair) and the floor's friction (the account is taken while both are
// clear of the floor).
struct CollideResult { bool ok = false; std::string error; double least = 1e9, speed = 0, residual = 0, impulse = 0, wall = 0; int steps = 0; };
CollideResult Collide(bool contact, int n, double T)
{
	CollideResult out;
	const double R = 0.035, L = 0.32, H = 1.0/30, Vs = 4.0/3.0*Pi*R*R*R, rho = 1500, m = rho*Vs;
	const auto frames = std::uint64_t(std::llround(T*30)) + 1;
	auto liquidBody = [&](const char* name, double x, double vx) {
		return V::MapValue({{"name", V::String(name)}, {"shape", V::String("sphere")}, {"radius", V::Float(R)}, {"position", Vec(x, 0.15, 0.16)},
			{"velocity", Vec(vx, 0, 0)}, {"motion", V::String("port")}, {"coupling", V::String("implicit")}});
	};
	auto rigidBody = [&](const char* name, double x, double vx) {
		return V::MapValue({{"name", V::String(name)}, {"shape", V::String("sphere")}, {"radius", V::Float(R)}, {"mass", V::Float(m)},
			{"position", Vec(x, 0.15, 0.16)}, {"velocity", Vec(vx, 0, 0)}, {"restitution", V::Float(0.5)}, {"friction", V::Float(0.3)}});
	};
	V::Members liquid{{"frames", V::Unsigned(frames)}, {"domain_min", Vec(0, 0, 0)}, {"domain_size", Vec(L, L, L)},
		{"resolution", V::Unsigned(std::uint64_t(n))},
		{"liquid", V::ArrayValue({V::MapValue({{"type", V::String("fill")}, {"height", V::Float(0.28)}})})},
		{"bodies", V::ArrayValue({liquidBody("a", 0.1, 0.4), liquidBody("b", 0.22, -0.4)})}};
	V::Members rigid{{"frames", V::Unsigned(frames)}, {"substeps", V::Unsigned(8)},
		{"bodies", V::ArrayValue({rigidBody("a", 0.1, 0.4), rigidBody("b", 0.22, -0.4)})},
		{"planes", V::ArrayValue({V::MapValue({{"point", Vec(0, 0, 0)}, {"normal", Vec(0, 1, 0)}, {"restitution", V::Float(0.3)}, {"friction", V::Float(0.3)}})})}};
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.memberOrder = {"pool", "drop"};
	p.ownedExchanges = {{"pool", {"drop"}}};
	SimulationGroup G;
	std::string& error = out.error;
	bool ok = G.AddMember("pool", Make(LiquidSolverId), V::MapValue(liquid), error) &&
		G.AddMember("drop", Make(RigidSolverId), V::MapValue(rigid), error) &&
		G.Connect({"drop", "body_state", "pool", "body_state"}, error) &&
		G.Connect({"drop", "body_mass_properties", "pool", "body_mass_properties"}, error) &&
		G.Connect({"pool", "body_load", "drop", "body_load"}, error) &&
		(!contact || G.Connect({"drop", "body_contact", "pool", "body_contact"}, error)) && G.Configure(p, EmptySceneInputs(), error) && G.Start(error);
	if (!ok) return out;
	double lastV[2] = {0.4, -0.4};
	G.SetStepObserver([&](double t, const SimulationGroup& gr) {
		const auto s = gr.Snapshot("drop");
		std::shared_ptr<const Bytes> bytes;
		std::string w;
		const auto index = static_cast<std::int64_t>(std::llround(t*30));
		if (s->Read(index, "bodies", budget, bytes, w) != FrameStatus::Ready) return;
		TransformFrame f;
		if (!DecodeTransforms(*bytes, budget, f, w) || f.bodies.size() != 2) return;
		double F[2] = {0, 0};
		if (const auto load = gr.ServedValue(2)) {
			FieldFrame lf;
			if (DecodeField(*load, budget, lf, w)) for (const auto& c : lf.channels) if (c.name == "force") { F[0] = F32(c.values[0]); F[1] = F32(c.values[3]); }
		}
		const double d = Rigid::Norm(Rigid::Sub(f.bodies[1].position, f.bodies[0].position));
		out.least = std::min(out.least, d);
		for (int i = 0; i < 2; ++i) out.speed = std::max(out.speed, Rigid::Norm(f.bodies[std::size_t(i)].velocity));
		// The x account: m (dv_a + dv_b) - H (F_a + F_b).
		const double r = m*(f.bodies[0].velocity[0] - lastV[0] + f.bodies[1].velocity[0] - lastV[1]) - H*(F[0] + F[1]);
		const double j = std::fabs(m*(f.bodies[0].velocity[0] - lastV[0]) - H*F[0]);
		// Only while both are clear of the floor (its friction is the one other x impulse).
		if (f.bodies[0].position[1] - R > 1e-3 && f.bodies[1].position[1] - R > 1e-3) {
			out.residual = std::max(out.residual, std::fabs(r));
			out.impulse = std::max(out.impulse, j);
		}
		lastV[0] = f.bodies[0].velocity[0]; lastV[1] = f.bodies[1].velocity[0];
		++out.steps;
	});
	const double w0 = Seconds();
	while (!G.Finished()) if (!G.StepMacro(error)) return out;
	out.wall = Seconds() - w0;
	out.ok = true;
	return out;
}

void Collision()
{
	std::printf("m7h: two spheres colliding under water (24 cells, 0.6 s)\n");
	const double R = 0.035;
	const CollideResult with = Collide(true, 24, 0.6), without = Collide(false, 24, 0.6);
	std::printf("  body_contact connected: %s, least centre distance %.4f m (2R %.3f), max speed %.3f m/s, x account residual %.2e N s "
		"(largest body x impulse beyond the liquid's %.2e N s), %.1f s\n", with.ok ? "ran" : with.error.c_str(), with.least, 2*R, with.speed,
		with.residual, with.impulse, with.wall);
	std::printf("  not connected (m7g's wall stop): %s, least centre distance %.4f m, max speed %.3f m/s\n", without.ok ? "ran" : without.error.c_str(),
		without.least, without.speed);
	Check(with.ok && with.least > 2*R - 0.004 && with.speed < 1.0, "two submerged bodies collide without passing into each other, stably", with.error);
	Check(with.ok && with.impulse > 1e-3 && with.residual < 1e-12, "the collision exchanges equal and opposite impulses (x account)",
		std::to_string(with.residual));
}

int main(int argc, char** argv)
{
	std::string error;
	Check(RegisterLiquidSolver(error), "register liquid.mac", error);
	Check(RegisterRigidSolver(error), "register rigid.bodies", error);
	if (argc > 1 && std::string(argv[1]) == "--settle") { SettleProbe(argc, argv); return 0; }
	if (argc > 2 && std::string(argv[1]) == "--collide") {
		const CollideResult c = Collide(std::string(argv[2]) == "1", argc > 3 ? std::atoi(argv[3]) : 40, argc > 4 ? std::atof(argv[4]) : 1.0);
		std::printf("collide contact %s: %s least %.4f speed %.3f residual %.2e impulse %.2e steps %d (%.1f s)\n", argv[2], c.ok ? "ran" : c.error.c_str(),
			c.least, c.speed, c.residual, c.impulse, c.steps, c.wall);
		return 0;
	}
	const double t0 = Seconds();
	OperatorCase(false);
	OperatorCase(true);
	Description();
	Determinism();
	StopRestore();
	Groups();
	Collision();
	std::printf("LiquidRigidTest: %.1f s\n", Seconds() - t0);
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("LiquidRigidTest: all checks passed\n");
	return 0;
}
