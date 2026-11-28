// Simulation M6: see ClothRigidCoupling.h.
#include "pch.h"
#include "ClothRigidCoupling.h"
#include "../SimulationField.h"
#include "../SimulationMesh.h"
#include "../SimulationPortAdapters.h"
#include "../SimulationTransform.h"
#include "../Rigid/RigidSolver.h"
#include "../Cloth/ClothSolver.h"

#include <cmath>
#include <cstring>
#include <mutex>

namespace RISE { namespace Simulation {

namespace {
using V = RISECBOR64::Value;
constexpr std::size_t Budget = std::size_t(256) << 20;
constexpr double ClothDensity = 0.15;   // the cotton-plain-weave record, kg/m^2

bool Fail(std::string& error, const std::string& message) { error = message; return false; }
double Float(std::uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }

std::vector<double> SheetMasses(double size, std::uint32_t n)
{
	const double h = size/static_cast<double>(n - 1);
	std::vector<double> m;
	for (std::uint32_t j = 0; j < n; ++j) for (std::uint32_t i = 0; i < n; ++i) {
		const double wu = (i == 0 || i == n - 1) ? 0.5*h : h, wv = (j == 0 || j == n - 1) ? 0.5*h : h;
		m.push_back(ClothDensity*wu*wv);
	}
	return m;
}

// The body's surface in world space at t = 0 (its record surface at its initial pose).
bool BodyMesh(const V& rigid, const std::string& body, ClothRigidScenario& out, V& collider, std::string& error)
{
	auto solver = CreateSolver(RigidSolverId, error);
	SolverDescription d;
	if (!solver || !solver->Configure(rigid, EmptySceneInputs(), d, error)) return false;
	std::vector<double> local;
	std::vector<std::uint32_t> tris;
	if (!RigidBodySurface(d.cache.physicalRecords, body, local, tris, error) || !solver->Start(error)) return false;
	class Sink final : public IFrameSink {
	public:
		std::shared_ptr<const Bytes> first;
		bool Publish(std::int64_t, const std::string&, double, std::shared_ptr<const Bytes> p, const Bytes&, std::string&) override {
			if (!first) first = p;
			return true;
		}
	} sink;
	AdvanceRequest r;
	r.mode = AdvanceRequest::Mode::MacroStep;
	r.macroEnd = 0;
	AdvanceResult result;
	TransformFrame f;
	if (!solver->Advance(r, sink, nullptr, nullptr, result, error) || !sink.first || !DecodeTransforms(*sink.first, Budget, f, error))
		return false;
	const TransformBody* b = nullptr;
	for (const auto& x : f.bodies) if (x.name == body) b = &x;
	if (!b) return Fail(error, "no body " + body);
	const auto R = QuaternionMatrix(b->orientation);
	V::Values vs, ts;
	for (std::size_t i = 0; i < local.size(); i += 3)
		for (int a = 0; a < 3; ++a)
			vs.push_back(V::Float(b->position[a] + R[3*a]*local[i] + R[3*a + 1]*local[i + 1] + R[3*a + 2]*local[i + 2]));
	for (auto k : tris) ts.push_back(V::Unsigned(k));
	collider = V::MapValue({{"type", V::String("mesh")}, {"vertices", V::ArrayValue(vs)}, {"triangles", V::ArrayValue(ts)},
		{"motion", V::MapValue({{"type", V::String("port")}, {"port", V::String("collider_surface")}})}});
	// Mass and moments from the record.
	V rec;
	if (!RISECBOR64::DecodeCanonical(d.cache.physicalRecords, rec, &error)) return false;
	for (const V& e : rec.Find("bodies")->GetArray()) {
		if (e.Find("name")->GetText() != body) continue;
		out.mass = e.Find("mass")->GetFloat();
		for (int a = 0; a < 3; ++a) out.moments[a] = e.Find("moments")->GetArray()[a].GetFloat();
	}
	out.body = body;
	out.rigidVersion = d.version;
	return true;
}

V Members(V::Members m) { return V::MapValue(m); }
}

bool ZeroGravitySpinScenario(std::uint64_t substeps, std::uint64_t frames, ClothRigidScenario& out, std::string& error,
	std::uint32_t vertices)
{
	out = ClothRigidScenario();
	const double rate = 240.0/static_cast<double>(substeps);
	out.rigid = Members({{"bodies", V::ArrayValue({Members({{"name", V::String("box")}, {"shape", V::String("box")},
		{"size", Vec(0.3, 0.3, 0.3)}, {"density", V::Float(30)}, {"velocity", Vec(0, 0.5, 0)},
		{"angular_velocity", Vec(0, 2, 0)}})})}, {"gravity", Vec(0, 0, 0)}, {"frames", V::Unsigned(frames)},
		{"frame_rate", V::Float(rate)}, {"substeps", V::Unsigned(substeps)}});
	V collider;
	if (!BodyMesh(out.rigid, "box", out, collider, error)) return false;
	out.cloth = Members({{"mode", V::String("bake")}, {"integrator", V::String("backward_euler")}, {"damping", V::Bool(false)},
		{"gravity", Vec(0, 0, 0)}, {"vertices_u", V::Unsigned(vertices)}, {"vertices_v", V::Unsigned(vertices)},
		{"size_u", V::Float(0.6)}, {"size_v", V::Float(0.6)}, {"origin", Vec(-0.3, 0.16, 0.3)},
		{"frames", V::Unsigned(frames)}, {"frame_rate", V::Float(rate)}, {"substeps", V::Unsigned(substeps)},
		{"material", V::String("cotton-plain-weave")}, {"colliders", V::ArrayValue({collider})}});
	out.clothMasses = SheetMasses(0.6, vertices);
	out.duration = static_cast<double>(frames - 1)/rate;
	return true;
}

bool FloatingBoxScenario(double mass, std::uint64_t frames, ClothRigidScenario& out, std::string& error)
{
	out = ClothRigidScenario();
	out.rigid = Members({{"bodies", V::ArrayValue({Members({{"name", V::String("box")}, {"shape", V::String("box")},
		{"size", Vec(0.3, 0.3, 0.3)}, {"mass", V::Float(mass)}})})}, {"gravity", Vec(0, 0, 0)},
		{"frames", V::Unsigned(frames)}, {"frame_rate", V::Float(30)}, {"substeps", V::Unsigned(8)}});
	V collider;
	if (!BodyMesh(out.rigid, "box", out, collider, error)) return false;
	out.cloth = Members({{"mode", V::String("bake")}, {"vertices_u", V::Unsigned(15)}, {"vertices_v", V::Unsigned(15)},
		{"size_u", V::Float(0.7)}, {"size_v", V::Float(0.7)}, {"origin", Vec(-0.35, 0.18, 0.35)},
		{"frames", V::Unsigned(frames)}, {"frame_rate", V::Float(30)}, {"substeps", V::Unsigned(8)},
		{"material", V::String("cotton-plain-weave")}, {"colliders", V::ArrayValue({collider})}});
	out.clothMasses = SheetMasses(0.7, 15);
	out.duration = static_cast<double>(frames - 1)/30;
	return true;
}

bool TrampolineScenario(double mass, std::uint64_t frames, ClothRigidScenario& out, std::string& error)
{
	out = ClothRigidScenario();
	out.rigid = Members({{"bodies", V::ArrayValue({Members({{"name", V::String("ball")}, {"shape", V::String("sphere")},
		{"radius", V::Float(0.1)}, {"mass", V::Float(mass)}, {"position", Vec(0, 0.36, 0)}})})},
		{"frames", V::Unsigned(frames)}, {"frame_rate", V::Float(30)}, {"substeps", V::Unsigned(8)}});
	V collider;
	if (!BodyMesh(out.rigid, "ball", out, collider, error)) return false;
	V::Values pins;
	for (const char* corner : {"u0v0", "u1v0", "u0v1", "u1v1"}) pins.push_back(Members({{"select", V::String(corner)}}));
	out.cloth = Members({{"mode", V::String("bake")}, {"vertices_u", V::Unsigned(15)}, {"vertices_v", V::Unsigned(15)},
		{"size_u", V::Float(0.8)}, {"size_v", V::Float(0.8)}, {"origin", Vec(-0.4, 0, 0.4)},
		{"frames", V::Unsigned(frames)}, {"frame_rate", V::Float(30)}, {"substeps", V::Unsigned(8)},
		{"material", V::String("cotton-plain-weave")}, {"pins", V::ArrayValue(pins)}, {"colliders", V::ArrayValue({collider})}});
	out.clothMasses = SheetMasses(0.8, 15);
	out.duration = static_cast<double>(frames - 1)/30;
	return true;
}

namespace {
void Connections(SimulationGroup& g, const ClothRigidScenario& sc, bool live, std::string& error, bool& ok)
{
	PortConnection toSheet;
	toSheet.fromMember = "body"; toSheet.fromPort = live ? "body_state" : "bodies";
	toSheet.toMember = "sheet"; toSheet.toPort = "collider_surface";
	toSheet.adapter = RigidSurfaceAdapterId;
	toSheet.adapterParameters = V::MapValue({{"body", V::String(sc.body)}});
	ok = ok && g.Connect(toSheet, error);
	if (!live) return;
	PortConnection toBody;
	toBody.fromMember = "sheet"; toBody.fromPort = "collider_reaction";
	toBody.toMember = "body"; toBody.toPort = "body_load";
	toBody.adapter = ContactReactionAdapterId;
	toBody.adapterParameters = V::MapValue({{"targets", V::ArrayValue({V::String(sc.body)})}});
	ok = ok && g.Connect(toBody, error);
}
}

bool BuildClothRigidGroup(SimulationGroup& g, const ClothRigidScenario& sc, const GroupPolicy& policy, std::string& error)
{
	bool ok = g.AddMember("body", CreateSolver(RigidSolverId, error), sc.rigid, error) &&
		g.AddMember("sheet", CreateSolver(ClothSolverId, error), sc.cloth, error);
	Connections(g, sc, true, error, ok);
	return ok && g.Configure(policy, EmptySceneInputs(), error);
}

bool BuildClothFromCachedRigid(SimulationGroup& g, const ClothRigidScenario& sc, std::shared_ptr<const IReadOnlyCacheSnapshot> rigid,
	const GroupPolicy& policy, std::string& error)
{
	bool ok = g.AddCachedMember("body", std::move(rigid), error) && g.AddMember("sheet", CreateSolver(ClothSolverId, error), sc.cloth, error);
	Connections(g, sc, false, error, ok);
	return ok && g.Configure(policy, EmptySceneInputs(), error);
}

bool MeasureClothRigid(const IReadOnlyCacheSnapshot& sheet, const IReadOnlyCacheSnapshot& body, std::int64_t index,
	const ClothRigidScenario& sc, ClothRigidTotals& t, std::string& error)
{
	t = ClothRigidTotals();
	std::shared_ptr<const Bytes> bytes;
	MeshFrame m;
	if (sheet.Read(index, "surface", Budget, bytes, error) != FrameStatus::Ready || !DecodeMesh(*bytes, Budget, sheet.Resolver(), m, error))
		return false;
	if (m.positions.size() != 3*sc.clothMasses.size()) return Fail(error, "the sheet does not match the scenario");
	for (std::size_t q = 0; q < sc.clothMasses.size(); ++q) {
		const double mass = sc.clothMasses[q];
		double x[3], v[3];
		for (int a = 0; a < 3; ++a) { x[a] = Float(m.positions[3*q + a]); v[a] = Float(m.velocities[3*q + a]); }
		t.momentum[0] += mass*v[0]; t.momentum[1] += mass*v[1]; t.momentum[2] += mass*v[2];
		t.angular[0] += mass*(x[1]*v[2] - x[2]*v[1]); t.angular[1] += mass*(x[2]*v[0] - x[0]*v[2]); t.angular[2] += mass*(x[0]*v[1] - x[1]*v[0]);
		t.kinetic += 0.5*mass*(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
	}
	TransformFrame f;
	if (body.Read(index, "bodies", Budget, bytes, error) != FrameStatus::Ready || !DecodeTransforms(*bytes, Budget, f, error)) return false;
	const TransformBody* b = nullptr;
	for (const auto& x : f.bodies) if (x.name == sc.body) b = &x;
	if (!b) return Fail(error, "no body " + sc.body);
	const auto R = QuaternionMatrix(b->orientation);
	double wb[3], pib[3], spin[3];
	for (int a = 0; a < 3; ++a) wb[a] = R[a]*b->angularVelocity[0] + R[3 + a]*b->angularVelocity[1] + R[6 + a]*b->angularVelocity[2];
	for (int a = 0; a < 3; ++a) pib[a] = sc.moments[a]*wb[a];
	for (int a = 0; a < 3; ++a) spin[a] = R[3*a]*pib[0] + R[3*a + 1]*pib[1] + R[3*a + 2]*pib[2];
	const auto& x = b->position;
	const auto& v = b->velocity;
	for (int a = 0; a < 3; ++a) t.momentum[a] += sc.mass*v[a];
	t.angular[0] += sc.mass*(x[1]*v[2] - x[2]*v[1]) + spin[0];
	t.angular[1] += sc.mass*(x[2]*v[0] - x[0]*v[2]) + spin[1];
	t.angular[2] += sc.mass*(x[0]*v[1] - x[1]*v[0]) + spin[2];
	t.kinetic += 0.5*sc.mass*(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) + 0.5*(pib[0]*wb[0] + pib[1]*wb[1] + pib[2]*wb[2]);
	t.bodyPosition = {{x[0], x[1], x[2]}};
	return true;
}

namespace {
struct CaseSpec { const char* id; const char* description; double tolerance; };
// Tolerances: about five times the drift observed at the canonical
// resolution (docs/SIMULATION_SOLVER_CONTRACT.md "Coupling evidence").
const CaseSpec Specs[] = {
	{"zero-g-linear-momentum", "box into a free sheet, zero gravity, implicit Gauss-Seidel: max |P(t) - P(0)| / |P(0)|", 5e-7},
	{"zero-g-angular-momentum", "the same: max |L(t) - L(0)| / |L(0)| about the origin (includes backward Euler's own rotational damping)", 5e-4},
	{"zero-g-energy", "the same: largest rise of the kinetic energy over its start, / KE(0)", 1e-6}};

bool ComputeEvidence(Evidence& e, std::string& error)
{
	ClothRigidScenario sc;
	if (!ZeroGravitySpinScenario(8, 16, sc, error, 9)) return false;
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.memberOrder = {"body", "sheet"};
	p.subIterations = 30;
	p.convergenceTolerance = 1e-6;
	p.aitken = true;
	// A2: the result is a function of the scenario, the policy, both solvers' versions and the build.
	const V storeKey = V::MapValue({{"kind", V::String(ClothRigidCouplingKind)}, {"problem", V::String("zero-g-spin-9")},
		{"policy", V::String("implicit-gauss-seidel-1e-6-aitken-30")}, {"rigid", sc.rigid}, {"cloth", sc.cloth},
		{"rigid_version", V::String(sc.rigidVersion)}, {"cloth_version", V::String(ClothSolverVersion())}});
	if (LoadPersistedEvidence("coupling." + std::string(ClothRigidCouplingKind), storeKey, e)) return true;
	SimulationGroup g;
	if (!BuildClothRigidGroup(g, sc, p, error) || !g.Start(error) || !g.Run(error)) return Fail(error, "coupling evidence run: " + error);
	ClothRigidTotals t0, t;
	if (!MeasureClothRigid(*g.Snapshot("sheet"), *g.Snapshot("body"), 0, sc, t0, error)) return false;
	double dp = 0, dl = 0, ke = 0;
	const double p0 = std::sqrt(t0.momentum[0]*t0.momentum[0] + t0.momentum[1]*t0.momentum[1] + t0.momentum[2]*t0.momentum[2]);
	const double l0 = std::sqrt(t0.angular[0]*t0.angular[0] + t0.angular[1]*t0.angular[1] + t0.angular[2]*t0.angular[2]);
	for (std::int64_t i = 1; i < 16; ++i) {
		if (!MeasureClothRigid(*g.Snapshot("sheet"), *g.Snapshot("body"), i, sc, t, error)) return false;
		double a = 0, b = 0;
		for (int k = 0; k < 3; ++k) { a += std::pow(t.momentum[k] - t0.momentum[k], 2); b += std::pow(t.angular[k] - t0.angular[k], 2); }
		dp = std::max(dp, std::sqrt(a)/p0);
		dl = std::max(dl, std::sqrt(b)/l0);
		ke = std::max(ke, (t.kinetic - t0.kinetic)/t0.kinetic);
	}
	const double errors[3] = {dp, dl, std::max(0.0, ke)};
	const V configuration = V::MapValue({{"kind", V::String(ClothRigidCouplingKind)}, {"problem", V::String("zero-g-spin-9")},
		{"policy", V::String("implicit-gauss-seidel-1e-6")}});
	for (int k = 0; k < 3; ++k) {
		EvidenceCase c;
		c.id = Specs[k].id;
		c.description = Specs[k].description;
		c.error = errors[k];
		c.tolerance = Specs[k].tolerance;
		c.passed = std::isfinite(c.error) && c.error <= c.tolerance;
		c.digest = EvidenceDigest(c.id, configuration, c.error, c.tolerance, c.passed);
		e.cases.push_back(c);
	}
	e.fidelity = "predictive";
	e.conditions = {{"exchange", {"impulse"}}, {"surface", {"rigid-fit"}}};
	e.scope = "the contact impulse and its moment crossing the port as the step's own barrier and friction impulse, the "
		"collider following the body's fitted pose: momentum balance and no energy gain on a canonical zero-gravity "
		"impact; it does not bound the trajectory error of a given coupled run";
	PersistEvidence("coupling." + std::string(ClothRigidCouplingKind), storeKey, e);
	return true;
}
}

bool RegisterClothRigidCouplingEvidence(std::string& error)
{
	static std::mutex once;
	static bool done = false;
	std::lock_guard<std::mutex> hold(once);
	if (done) return true;
	if (!RegisterCouplingEvidence(ClothRigidCouplingKind, ComputeEvidence, error)) return false;
	done = true;
	return true;
}

} }
