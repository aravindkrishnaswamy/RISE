// rigid.bodies (simulation M6): mass properties, the evidence cases and
// their labels, determinism, checkpoint / restore and rollback equal to an
// uninterrupted run, macro steps, the body_load / body_state ports with exact
// impulse and angular-impulse balance, plane contact (preview), a scene-input
// body, and parameter refusals.
#include "../src/Library/Simulation/Rigid/RigidBody.h"
#include "../src/Library/Simulation/Rigid/RigidSolver.h"
#include "../src/Library/Simulation/SimulationField.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationTransform.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>

namespace {
using namespace RISE::Simulation;
using namespace RISE::Simulation::Rigid;
using V = RISE::RISECBOR64::Value;
constexpr std::size_t budget = std::size_t(64) << 20;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol*std::max(1.0, std::fabs(b)); }
V Vec3(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }
std::uint32_t Bits(double x) { const float f = static_cast<float>(x); std::uint32_t b; std::memcpy(&b, &f, 4); return b; }

Bytes Encoded(const V& v) { Bytes b; std::string e; RISE::RISECBOR64::Encode(v, b, &e); return b; }

std::unique_ptr<ISimulationSolver> Make()
{
	std::string error;
	Check(RegisterRigidSolver(error), "register", error);
	auto s = CreateSolver(RigidSolverId, error);
	Check(s != nullptr, "create", error);
	return s;
}

V Bodies(V::Values bodies, V::Members extra = {})
{
	V::Members m{{"bodies", V::ArrayValue(bodies)}};
	for (auto& e : extra) m.push_back(e);
	return V::MapValue(m);
}

std::map<std::pair<std::int64_t,std::string>,std::string> Digests(const IReadOnlyCacheSnapshot& s)
{
	std::map<std::pair<std::int64_t,std::string>,std::string> out;
	for (const auto& e : s.Entries()) out[{e.index, e.output}] = e.digest;
	return out;
}

TransformFrame Last(const IReadOnlyCacheSnapshot& s)
{
	TransformFrame f;
	std::string error;
	std::int64_t last = -1;
	for (const auto& e : s.Entries()) last = std::max(last, e.index);
	std::shared_ptr<const Bytes> bytes;
	if (s.Read(last, "bodies", budget, bytes, error) == FrameStatus::Ready) DecodeTransforms(*bytes, budget, f, error);
	return f;
}

// A scene with one mesh input: a unit-density box mesh, translated and rotated.
class BoxScene final : public ISceneInputs {
public:
	std::shared_ptr<const Bytes> mesh;
	BoxScene() {
		Shape box;
		box.size = {{0.4, 0.2, 0.1}};
		std::vector<double> v;
		std::vector<std::uint32_t> t;
		ShapeSurface(box, v, t);
		const M3 R = QMatrix(QAxis(2, 0.3));
		MeshFrame m;
		for (std::size_t k = 0; k < v.size(); k += 3) {
			const V3 p = Add(Apply(R, V3{{v[k], v[k+1], v[k+2]}}), V3{{1, 2, 3}});
			for (double c : p) m.positions.push_back(Bits(c));
		}
		m.indices = t;
		auto bytes = std::make_shared<Bytes>();
		std::string error;
		EncodeMesh(m, budget, *bytes, error);
		mesh = bytes;
	}
	bool Identity(const std::string& name, std::string& hash, std::string& error) const override {
		if (name != "crate") { error = "no input " + name; return false; }
		hash = RISE::RISECBOR64::SHA256Hex(*mesh);
		return true;
	}
	bool Sample(const std::string& name, double, std::shared_ptr<const Bytes>& encoded, std::string& codec,
		std::string& error) const override {
		if (name != "crate") { error = "no input " + name; return false; }
		encoded = mesh; codec = "mesh.v1";
		return true;
	}
};
}

int main(int argc, char** argv)
{
	(void)argc; (void)argv;
	std::string error;
	// ---------------------------------------------------------------- A. mass properties
	{
		MassProperties box, mesh, sphere, cyl;
		Shape s;
		s.size = {{0.4, 0.2, 0.1}};
		Check(ShapeMass(s, 500, box, error), "box mass", error);
		Check(Near(box.mass, 500*0.008, 1e-14) && Near(box.inertia[0], box.mass*(0.04 + 0.01)/12, 1e-14) &&
			Near(box.inertia[8], box.mass*(0.16 + 0.04)/12, 1e-14), "box mass and inertia");
		Shape m;
		m.type = Shape::Type::Mesh;
		ShapeSurface(s, m.vertices, m.triangles);
		Check(ShapeMass(m, 500, mesh, error), "box-mesh mass", error);
		double worst = std::fabs(mesh.mass - box.mass)/box.mass;
		for (int k = 0; k < 9; ++k) worst = std::max(worst, std::fabs(mesh.inertia[k] - box.inertia[k])/box.inertia[0]);
		Check(worst < 1e-12, "polyhedral mass properties of the box mesh equal the box's", std::to_string(worst));
		// A unit right tetrahedron: V = 1/6, centroid (1/4, 1/4, 1/4), I_xx about the centroid = 1/80 (rho = 1)... check by formula.
		Shape tet;
		tet.type = Shape::Type::Mesh;
		tet.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
		tet.triangles = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
		MassProperties tp;
		Check(ShapeMass(tet, 1, tp, error), "tetrahedron", error);
		// Known: about the origin I_xx = 1/60 (integral of y^2 + z^2 = 2/120), products 1/120; shift to the centroid.
		const double ixx = 2.0/60 - (1.0/6)*(2.0/16), ixy = -(1.0/120) + (1.0/6)/16;
		Check(Near(tp.volume, 1.0/6, 1e-15) && Near(tp.centroid[0], 0.25, 1e-15) && Near(tp.inertia[0], ixx, 1e-14) &&
			Near(tp.inertia[1], ixy, 1e-14), "tetrahedron volume, centroid and inertia");
		Shape inward = tet;
		std::swap(inward.triangles[1], inward.triangles[2]);
		std::swap(inward.triangles[4], inward.triangles[5]);
		std::swap(inward.triangles[7], inward.triangles[8]);
		std::swap(inward.triangles[10], inward.triangles[11]);
		Check(!ShapeMass(inward, 1, tp, error), "an inward-facing mesh is refused");
		Shape open = tet;
		open.triangles.resize(9);
		Check(!ShapeMass(open, 1, tp, error), "an open mesh is refused");
		s.type = Shape::Type::Sphere; s.radius = 0.1;
		Check(ShapeMass(s, 1000, sphere, error) && Near(sphere.inertia[0], 0.4*sphere.mass*0.01, 1e-14), "sphere");
		s.type = Shape::Type::Cylinder; s.radius = 0.1; s.height = 0.3;
		Check(ShapeMass(s, 1000, cyl, error) && Near(cyl.inertia[8], 0.5*cyl.mass*0.01, 1e-14) &&
			Near(cyl.inertia[0], cyl.mass*(0.03 + 0.09)/12, 1e-14), "cylinder");
		// Icosphere surface encloses about the sphere's volume (it is inscribed).
		Shape ico;
		ico.type = Shape::Type::Mesh;
		Shape sp; sp.type = Shape::Type::Sphere; sp.radius = 1;
		ShapeSurface(sp, ico.vertices, ico.triangles);
		MassProperties ip;
		Check(ShapeMass(ico, 1, ip, error) && ico.triangles.size() == 240 && ip.volume < 4.0/3*3.14159265 &&
			ip.volume > 0.8*4.0/3*3.14159265, "the sphere surface is a closed inscribed icosphere", error);
		// Principal axes of a rotated box's inertia.
		const M3 R = QMatrix(QAxis(1, 0.7));
		M3 rotated{};
		const M3 Ibox = box.inertia;
		const M3 tmp = Multiply(R, Ibox);
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
			double acc = 0;
			for (int k = 0; k < 3; ++k) acc += tmp[3*i + k]*R[3*j + k];
			rotated[3*i + j] = acc;
		}
		V3 moments; M3 axes;
		PrincipalAxes(rotated, moments, axes);
		Check(Near(moments[0], box.inertia[0], 1e-12) && Near(moments[2], box.inertia[8], 1e-12),
			"principal moments of a rotated inertia");
	}
	// ---------------------------------------------------------------- B. evidence and labels
	{
		auto solver = Make();
		SolverDescription d;
		const V params = Bodies({V::MapValue({{"name", V::String("ball")}, {"shape", V::String("sphere")}, {"radius", V::Float(0.1)}})});
		Check(solver->Configure(params, EmptySceneInputs(), d, error), "configure", error);
		for (const auto& c : d.evidence.cases)
			std::cout << "  evidence " << c.id << ": error " << c.error << " (tolerance " << c.tolerance << ") "
				<< (c.passed ? "pass" : "FAIL") << "\n";
		Check(d.evidence.cases.size() == 4, "four evidence cases");
		for (const auto& c : d.evidence.cases) Check(c.passed, "evidence case passes at the default substep: " + c.id);
		Check(PrepareCacheDescription(d, EmptySceneInputs(), "", error) && d.cache.fidelity == "predictive",
			"free motion earns predictive", error);
		auto again = Make();
		SolverDescription d2;
		Check(again->Configure(d.effectiveParameters, EmptySceneInputs(), d2, error) &&
			Encoded(d2.effectiveParameters) == Encoded(d.effectiveParameters), "effective parameters are a fixed point", error);
		// Second order: the symmetric-top and invariant errors fall by about 4 when the substep halves.
		SolverDescription fine;
		Check(Make()->Configure(Bodies({V::MapValue({{"name", V::String("ball")}})}, {{"substeps", V::Unsigned(16)}}),
			EmptySceneInputs(), fine, error), "configure at half the substep", error);
		for (std::size_t k = 0; k < 2; ++k) {
			const double ratio = d.evidence.cases[k].error/fine.evidence.cases[k].error;
			std::cout << "  " << d.evidence.cases[k].id << " error ratio for h -> h/2: " << ratio << " (order " << std::log2(ratio) << ")\n";
			Check(ratio > 3.5 && ratio < 4.5, "second-order convergence of " + d.evidence.cases[k].id, std::to_string(ratio));
		}
		// With a plane: preview.
		SolverDescription dp;
		Check(Make()->Configure(Bodies({V::MapValue({{"name", V::String("ball")}, {"shape", V::String("sphere")}})},
			{{"planes", V::ArrayValue({V::MapValue({{"point", Vec3(0, 0, 0)}})})}}), EmptySceneInputs(), dp, error) &&
			PrepareCacheDescription(dp, EmptySceneInputs(), "", error) && dp.cache.fidelity == "preview" &&
			!dp.capabilities.fidelityLimit.empty(), "plane contact publishes preview", error);
		// A coarse substep fails its cases and publishes preview.
		SolverDescription dc;
		Check(Make()->Configure(Bodies({V::MapValue({{"name", V::String("ball")}})}, {{"substeps", V::Unsigned(1)},
			{"frame_rate", V::Float(5)}}), EmptySceneInputs(), dc, error) &&
			PrepareCacheDescription(dc, EmptySceneInputs(), "", error) && dc.cache.fidelity == "preview",
			"a 0.2 s substep fails its evidence: preview", error);
		Check(d.capabilities.externalMacroStep && d.capabilities.rollback && d.capabilities.deterministic &&
			d.capabilities.resumable && d.capabilities.interactive && d.capabilities.backend == "cpu", "capabilities");
		std::vector<double> sv; std::vector<std::uint32_t> st;
		Check(RigidBodySurface(d.cache.physicalRecords, "ball", sv, st, error) && st.size() == 240, "the body surface record", error);
	}
	// ---------------------------------------------------------------- C. determinism, checkpoint, rollback, macro steps
	const V tumble = Bodies({
		V::MapValue({{"name", V::String("crate")}, {"shape", V::String("box")}, {"size", Vec3(0.4, 0.2, 0.1)},
			{"density", V::Float(500)}, {"position", Vec3(0, 1, 0)}, {"velocity", Vec3(1, 3, 0)},
			{"angular_velocity", Vec3(0.2, 6, 0.1)}}),
		V::MapValue({{"name", V::String("can")}, {"shape", V::String("cylinder")}, {"radius", V::Float(0.05)},
			{"height", V::Float(0.2)}, {"mass", V::Float(0.3)}, {"orientation", V::ArrayValue({V::Float(1), V::Float(0.2), V::Float(0), V::Float(0)})},
			{"angular_velocity", Vec3(3, 0, 1)}})}, {{"frames", V::Unsigned(20)}});
	std::map<std::pair<std::int64_t,std::string>,std::string> reference;
	{
		InProcessHost host(Make());
		Check(host.Configure(tumble, EmptySceneInputs(), error) && host.Start(error) &&
			host.StepToIndex(1000, error) && host.Position().finished, "free run", error);
		reference = Digests(*host.Snapshot());
		Check(reference.size() == 20, "20 frames");
		InProcessHost twin(Make());
		Check(twin.Configure(tumble, EmptySceneInputs(), error) && twin.Start(error) && twin.StepToIndex(1000, error), "twin", error);
		Check(Digests(*twin.Snapshot()) == reference, "deterministic: identical payload bytes");
		// Stop in the middle of a segment, checkpoint, restore in a fresh solver.
		InProcessHost a(Make());
		Check(a.Configure(tumble, EmptySceneInputs(), error) && a.Start(error) && a.StepToIndex(6, error), "partial", error);
		InProcessHost::Checkpoint cp;
		Check(a.SaveCheckpoint(cp, error), "checkpoint", error);
		InProcessHost b(Make());
		Check(b.Configure(tumble, EmptySceneInputs(), error) && b.RestoreCheckpoint(cp, error) && b.StepToIndex(1000, error),
			"restore and finish", error);
		auto merged = Digests(*a.Snapshot());
		for (const auto& e : Digests(*b.Snapshot())) merged[e.first] = e.second;
		Check(merged == reference, "stop / restore equals the uninterrupted run");
		// Macro steps on the output grid, then at half intervals, reproduce free stepping.
		auto solver = Make();
		SolverDescription d;
		Check(solver->Configure(tumble, EmptySceneInputs(), d, error) && solver->Start(error), "macro configure", error);
		class Sink final : public IFrameSink {
		public:
			std::map<std::pair<std::int64_t,std::string>,std::string> digests;
			bool Publish(std::int64_t i, const std::string& o, double, std::shared_ptr<const Bytes> p, const Bytes&, std::string&) override {
				digests[{i, o}] = RISE::RISECBOR64::SHA256Hex(*p); return true; }
		} sink;
		AdvanceResult r;
		const double dt = d.cache.interval;
		for (int k = 0; k <= 19; ++k) {
			AdvanceRequest q; q.mode = AdvanceRequest::Mode::MacroStep; q.macroEnd = k*dt;
			Check(solver->Advance(q, sink, nullptr, nullptr, r, error), "macro step", error);
		}
		Check(sink.digests == reference, "macro steps on the output grid reproduce free stepping byte for byte");
		// Rollback: save, step, restore, step again -> the same bytes.
		auto roll = Make();
		SolverDescription dr;
		Check(roll->Configure(tumble, EmptySceneInputs(), dr, error) && roll->Start(error), "rollback configure", error);
		Sink s1, s2;
		AdvanceRequest q; q.mode = AdvanceRequest::Mode::MacroStep; q.macroEnd = 3*dt;
		Check(roll->Advance(q, s1, nullptr, nullptr, r, error), "advance", error);
		std::shared_ptr<const SolverStateToken> token;
		Check(roll->SaveState(token, error), "save", error);
		q.macroEnd = 5*dt;
		Check(roll->Advance(q, s1, nullptr, nullptr, r, error) && roll->RestoreState(*token, error) &&
			roll->Advance(q, s2, nullptr, nullptr, r, error), "advance, restore, repeat", error);
		Check(s2.digests.size() == 2 && s2.digests.at({4, "bodies"}) == s1.digests.at({4, "bodies"}) &&
			s2.digests.at({5, "bodies"}) == s1.digests.at({5, "bodies"}), "a rolled-back repeat equals the first pass");
		Check(!Make()->RestoreState(*token, error), "a token from another instance is refused");
	}
	// ---------------------------------------------------------------- D. ports: exact impulse and angular impulse
	{
		const V one = Bodies({V::MapValue({{"name", V::String("crate")}, {"shape", V::String("box")}, {"size", Vec3(0.4, 0.2, 0.1)},
			{"density", V::Float(500)}, {"angular_velocity", Vec3(0.3, 2, 0.1)}})}, {{"gravity", Vec3(0, 0, 0)}, {"frames", V::Unsigned(12)}});
		auto solver = Make();
		SolverDescription d;
		Check(solver->Configure(one, EmptySceneInputs(), d, error) && solver->Start(error), "load configure", error);
		const PortDeclaration* load = nullptr;
		const PortDeclaration* state = nullptr;
		for (const auto& p : d.ports) { if (p.name == "body_load") load = &p; if (p.name == "body_state") state = &p; }
		Check(load && state && !load->offers && state->offers && load->domain == TransformBodyDomain({"crate"}) &&
			state->codec == TransformCodec && state->output == "bodies", "port declarations");
		// A force at a point plus a torque, offered on the relay at t = 0 and held.
		FieldFrame f;
		f.domain = load->domain; f.count = 1;
		f.channels = {{"force", "N", 3, {Bits(2), Bits(-1), Bits(0.5)}}, {"point", "m", 3, {Bits(0.1), Bits(0.05), Bits(-0.2)}},
			{"torque", "N m", 3, {Bits(0.01), Bits(0), Bits(0.02)}}};
		auto bytes = std::make_shared<Bytes>();
		Check(EncodeField(f, budget, *bytes, error), "encode load", error);
		PortRelay relay;
		relay.Connect("body_load", "load");
		Check(relay.Offer("load", 0, bytes, error), "offer", error);
		// Angular momentum about the origin before and after: dL = T (tau + p x F) exactly; dP = T F.
		class Sink final : public IFrameSink {
		public:
			std::vector<std::shared_ptr<const Bytes>> frames;
			bool Publish(std::int64_t, const std::string&, double, std::shared_ptr<const Bytes> p, const Bytes&, std::string&) override {
				frames.push_back(p); return true; }
		} sink;
		AdvanceResult r;
		const double T = 10*d.cache.interval;
		AdvanceRequest q; q.mode = AdvanceRequest::Mode::MacroStep; q.macroEnd = T;
		Check(solver->Advance(q, sink, &relay, nullptr, r, error) && sink.frames.size() == 11, "advance under the held load", error);
		TransformFrame first, last;
		DecodeTransforms(*sink.frames.front(), budget, first, error);
		DecodeTransforms(*sink.frames.back(), budget, last, error);
		const double mass = 500*0.008;
		const V3 F{{static_cast<float>(2), static_cast<float>(-1), static_cast<float>(0.5)}};
		const V3 P{{static_cast<float>(0.1), static_cast<float>(0.05), static_cast<float>(-0.2)}};
		const V3 tau{{static_cast<float>(0.01), 0, static_cast<float>(0.02)}};
		double dp = 0;
		for (int k = 0; k < 3; ++k) dp = std::max(dp, std::fabs(mass*(last.bodies[0].velocity[k] - first.bodies[0].velocity[k]) - T*F[k]));
		Check(dp < 1e-12, "linear impulse T F exactly", std::to_string(dp));
		// Angular momentum about the origin from the published state.
		auto angular = [&](const TransformBody& b) {
			MassProperties mp; Shape s; s.size = {{0.4, 0.2, 0.1}};
			ShapeMass(s, 500, mp, error);
			const M3 R = QuaternionMatrix(b.orientation);
			const V3 wb = ApplyT(R, b.angularVelocity);
			const V3 pib{{mp.inertia[0]*wb[0], mp.inertia[4]*wb[1], mp.inertia[8]*wb[2]}};
			return Add(Mul(Cross(b.position, b.velocity), mass), Apply(R, pib));
		};
		const V3 dL = Sub(angular(last.bodies[0]), angular(first.bodies[0]));
		const V3 expected = Mul(Add(tau, Cross(P, F)), T);
		double dl = 0;
		for (int k = 0; k < 3; ++k) dl = std::max(dl, std::fabs(dL[k] - expected[k]));
		Check(dl < 1e-12, "angular impulse T (tau + p x F) about the origin exactly", std::to_string(dl));
		// The state port offers transform.v1 at the macro end.
		std::shared_ptr<const Bytes> offered;
		PortRelay states;
		AdvanceRequest q2; q2.mode = AdvanceRequest::Mode::MacroStep; q2.macroEnd = T;
		Check(solver->Advance(q2, sink, &states, nullptr, r, error), "zero-length macro step", error);
		Check(states.Accept("body_state", T, offered, error) == PortStatus::Ready, "body_state offered at the macro end", error);
		// A load on the wrong domain is refused.
		FieldFrame wrong = f;
		wrong.domain = TransformBodyDomain({"other"});
		auto wb = std::make_shared<Bytes>();
		EncodeField(wrong, budget, *wb, error);
		PortRelay bad;
		bad.Connect("body_load", "load");
		bad.Offer("load", 0, wb, error);
		auto s2 = Make();
		SolverDescription d2;
		s2->Configure(one, EmptySceneInputs(), d2, error);
		s2->Start(error);
		q.macroEnd = d.cache.interval;
		Check(!s2->Advance(q, sink, &bad, nullptr, r, error), "a body_load on another domain is refused");
	}
	// ---------------------------------------------------------------- E. free fall and plane contact (preview)
	{
		const V drop = Bodies({V::MapValue({{"name", V::String("ball")}, {"shape", V::String("sphere")}, {"radius", V::Float(0.1)},
			{"position", Vec3(0, 1.1, 0)}})}, {{"frames", V::Unsigned(61)},
			{"planes", V::ArrayValue({V::MapValue({{"point", Vec3(0, 0, 0)}, {"restitution", V::Float(0.5)}, {"friction", V::Float(0.3)}})})}});
		InProcessHost host(Make());
		Check(host.Configure(drop, EmptySceneInputs(), error) && host.Start(error) && host.StepToIndex(1000, error), "drop", error);
		double lowest = 1e9, apex = 0;
		bool bounced = false;
		double prevVy = 0;
		for (std::int64_t i = 0; i < 61; ++i) {
			std::shared_ptr<const Bytes> b;
			TransformFrame f;
			if (host.Snapshot()->Read(i, "bodies", budget, b, error) != FrameStatus::Ready || !DecodeTransforms(*b, budget, f, error)) continue;
			const double y = f.bodies[0].position[1];
			lowest = std::min(lowest, y);
			if (f.bodies[0].velocity[1] > 0) bounced = true;
			if (bounced && prevVy > 0 && f.bodies[0].velocity[1] <= 0) apex = std::max(apex, y);
			prevVy = f.bodies[0].velocity[1];
		}
		std::cout << "  plane contact: lowest centre " << lowest << " m, first apex " << apex << " m (e^2 drop = 0.25 m above contact)\n";
		Check(bounced && lowest > 0.09, "the ball bounces off the plane without sinking through", std::to_string(lowest));
		Check(apex > 0.1 + 0.15 && apex < 0.1 + 0.35, "the bounce reaches about e^2 of the drop (preview model)", std::to_string(apex));
	}
	// ---------------------------------------------------------------- F. a scene-input body
	{
		BoxScene scene;
		auto solver = Make();
		SolverDescription d;
		const V params = Bodies({V::MapValue({{"name", V::String("crate")}, {"shape", V::String("mesh")}, {"scene_input", V::String("crate")},
			{"density", V::Float(500)}})}, {{"frames", V::Unsigned(2)}, {"gravity", Vec3(0, 0, 0)}});
		Check(solver->Configure(params, scene, d, error), "scene-input body", error);
		Check(d.inputs == std::vector<std::string>{"crate"}, "the scene input is declared for the key");
		Check(PrepareCacheDescription(d, scene, "", error), "prepare", error);
		InProcessHost host(Make());
		Check(host.Configure(params, scene, error) && host.Start(error) && host.StepToIndex(10, error), "run", error);
		const TransformFrame f = Last(*host.Snapshot());
		Check(f.bodies.size() == 1 && Near(f.bodies[0].position[0], 1, 1e-6) && Near(f.bodies[0].position[1], 2, 1e-6) &&
			Near(f.bodies[0].position[2], 3, 1e-6), "the body starts at the mesh's centroid");
	}
	// ---------------------------------------------------------------- H. body-body contact (m6.2, preview)
	{
		auto frames = [&](InProcessHost& host, std::int64_t n) {
			std::vector<TransformFrame> out;
			for (std::int64_t i = 0; i < n; ++i) {
				std::shared_ptr<const Bytes> b;
				TransformFrame f;
				if (host.Snapshot()->Read(i, "bodies", budget, b, error) == FrameStatus::Ready && DecodeTransforms(*b, budget, f, error)) out.push_back(f);
			}
			return out;
		};
		auto sphere = [](const char* name, double r, double density, V3 x, V3 v, double e, double mu) {
			return V::MapValue({{"name", V::String(name)}, {"shape", V::String("sphere")}, {"radius", V::Float(r)}, {"density", V::Float(density)},
				{"position", Vec3(x[0], x[1], x[2])}, {"velocity", Vec3(v[0], v[1], v[2])}, {"restitution", V::Float(e)}, {"friction", V::Float(mu)}});
		};
		// H1: two spheres, elastic (e = 1, no friction), no gravity: head-on against the 1-D formulas, and oblique.
		for (int oblique = 0; oblique < 2; ++oblique) {
			const double ra = 0.1, rb = 0.07, rhoA = 800, rhoB = 2000;
			const V3 va{{1.2, 0, 0}}, vb{{-0.6, 0, 0}};
			const V params = Bodies({sphere("a", ra, rhoA, {{-0.4, 0, 0}}, va, 1, 0), sphere("b", rb, rhoB, {{0.4, oblique ? 0.09 : 0.0, 0}}, vb, 1, 0)},
				{{"frames", V::Unsigned(31)}, {"gravity", Vec3(0, 0, 0)}});
			InProcessHost host(Make());
			Check(host.Configure(params, EmptySceneInputs(), error) && host.Start(error) && host.StepToIndex(1000, error), "two spheres", error);
			const auto fs = frames(host, 31);
			Check(fs.size() == 31, "31 frames");
			if (fs.size() != 31) continue;
			const double ma = rhoA*4.0/3*3.14159265358979323846*ra*ra*ra, mb = rhoB*4.0/3*3.14159265358979323846*rb*rb*rb;
			auto momentum = [&](const TransformFrame& f, int k) { return ma*f.bodies[0].velocity[std::size_t(k)] + mb*f.bodies[1].velocity[std::size_t(k)]; };
			auto energy = [&](const TransformFrame& f) {
				double e = 0;
				for (int k = 0; k < 3; ++k) e += 0.5*ma*f.bodies[0].velocity[std::size_t(k)]*f.bodies[0].velocity[std::size_t(k)] +
					0.5*mb*f.bodies[1].velocity[std::size_t(k)]*f.bodies[1].velocity[std::size_t(k)];
				return e;
			};
			double dp = 0;
			for (int k = 0; k < 3; ++k) dp = std::max(dp, std::fabs(momentum(fs.back(), k) - momentum(fs.front(), k)));
			const double p0 = ma*1.2, e0 = energy(fs.front()), de = std::fabs(energy(fs.back()) - e0)/e0;
			double gap = 1e9;
			for (const auto& f : fs) gap = std::min(gap, Norm(Sub(f.bodies[1].position, f.bodies[0].position)) - ra - rb);
			std::cout << "  two spheres " << (oblique ? "oblique" : "head-on") << ": |dp| " << dp/p0 << " of p0, |dE| / E " << de
				<< ", least gap " << gap << " m, after: va " << fs.back().bodies[0].velocity[0] << " vb " << fs.back().bodies[1].velocity[0] << "\n";
			Check(dp < 1e-12*p0 && de < 1e-12, "an elastic sphere collision keeps momentum and energy", std::to_string(dp) + " " + std::to_string(de));
			Check(fs.back().bodies[1].position[0] - fs.back().bodies[0].position[0] > ra + rb && gap > -0.02, "the spheres separate");
			if (!oblique) {
				const double ua = ((ma - mb)*va[0] + 2*mb*vb[0])/(ma + mb), ub = ((mb - ma)*vb[0] + 2*ma*va[0])/(ma + mb);
				Check(Near(fs.back().bodies[0].velocity[0], ua, 1e-12) && Near(fs.back().bodies[1].velocity[0], ub, 1e-12),
					"head-on: the 1-D elastic velocities", std::to_string(fs.back().bodies[0].velocity[0]) + " vs " + std::to_string(ua));
			}
		}
		// H2: a stack of three boxes on a floor, at rest for 5 s: no drift, no jitter.
		{
			auto box = [](const char* name, double y) {
				return V::MapValue({{"name", V::String(name)}, {"shape", V::String("box")}, {"size", Vec3(0.2, 0.2, 0.2)}, {"density", V::Float(500)},
					{"position", Vec3(0, y, 0)}});
			};
			for (int nb = 1; nb <= 3; ++nb) {
			V::Values list{box("b1", 0.1)};
			if (nb > 1) list.push_back(box("b2", 0.3));
			if (nb > 2) list.push_back(box("b3", 0.5));
			const V params = Bodies(list, {{"frames", V::Unsigned(151)},
				{"planes", V::ArrayValue({V::MapValue({{"point", Vec3(0, 0, 0)}, {"restitution", V::Float(0.5)}, {"friction", V::Float(0.5)}})})}});
			InProcessHost host(Make());
			Check(host.Configure(params, EmptySceneInputs(), error) && host.Start(error) && host.StepToIndex(1000, error), "box stack", error);
			const auto fs = frames(host, 151);
			// Over the whole run: drift and tilt; after the first second (settled): speeds and the height range.
			double drift = 0, speed = 0, tilt = 0, sink = 0, lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
			for (std::size_t k = 0; k < fs.size(); ++k)
				for (std::size_t i = 0; i < std::size_t(nb); ++i) {
					const auto& b = fs[k].bodies[i];
					drift = std::max(drift, std::sqrt(b.position[0]*b.position[0] + b.position[2]*b.position[2]));
					tilt = std::max(tilt, 2*std::acos(std::min(1.0, std::fabs(b.orientation[0]))));
					sink = std::max(sink, std::fabs(b.position[1] - (0.1 + 0.2*double(i))));
					if (k < 30) continue;
					speed = std::max(speed, Norm(b.velocity) + 0.1*Norm(b.angularVelocity));
					lo[i] = std::min(lo[i], b.position[1]); hi[i] = std::max(hi[i], b.position[1]);
				}
			double range = 0;
			for (int i = 0; i < nb; ++i) range = std::max(range, hi[i] - lo[i]);
			std::cout << "  box stack of " << nb << ", 5 s: horizontal drift " << drift << " m, tilt " << tilt << " rad, height change " << sink
				<< " m; after 1 s: largest speed " << speed << " m/s, height range " << range << " m\n";
			Check(fs.size() == 151 && drift < 1e-5 && tilt < 1e-4 && sink < 1e-3 && speed < 1e-3 && range < 1e-4,
				"a box stack rests without drift or jitter", std::to_string(drift) + " " + std::to_string(tilt) + " " + std::to_string(sink) + " " +
				std::to_string(speed) + " " + std::to_string(range));
			}
		}
		// H2b: a stack and a sphere dropped onto it: stop / restore and a twin equal the uninterrupted run.
		{
			auto box = [](const char* name, double y) {
				return V::MapValue({{"name", V::String(name)}, {"shape", V::String("box")}, {"size", Vec3(0.2, 0.2, 0.2)}, {"density", V::Float(500)},
					{"position", Vec3(0, y, 0)}});
			};
			const V params = Bodies({box("b1", 0.1), box("b2", 0.3), sphere("s", 0.06, 2000, {{0.05, 0.8, 0.02}}, {{0, 0, 0}}, 0.5, 0.5),
				V::MapValue({{"name", V::String("c")}, {"shape", V::String("cylinder")}, {"radius", V::Float(0.05)}, {"height", V::Float(0.15)},
					{"position", Vec3(-0.04, 1.1, 0)}, {"orientation", V::ArrayValue({V::Float(1), V::Float(0.3), V::Float(0), V::Float(0)})}})},
				{{"frames", V::Unsigned(40)},
				{"planes", V::ArrayValue({V::MapValue({{"point", Vec3(0, 0, 0)}, {"restitution", V::Float(0.5)}, {"friction", V::Float(0.5)}})})}});
			InProcessHost ref(Make()), twin(Make()), a(Make()), b(Make());
			Check(ref.Configure(params, EmptySceneInputs(), error) && ref.Start(error) && ref.StepToIndex(1000, error), "contact run", error);
			Check(twin.Configure(params, EmptySceneInputs(), error) && twin.Start(error) && twin.StepToIndex(1000, error), "contact twin", error);
			Check(a.Configure(params, EmptySceneInputs(), error) && a.Start(error) && a.StepToIndex(17, error), "contact partial", error);
			InProcessHost::Checkpoint cp;
			Check(a.SaveCheckpoint(cp, error) && b.Configure(params, EmptySceneInputs(), error) && b.RestoreCheckpoint(cp, error) &&
				b.StepToIndex(1000, error), "contact restore", error);
			auto merged = Digests(*a.Snapshot());
			for (const auto& e : Digests(*b.Snapshot())) merged[e.first] = e.second;
			const auto fs = frames(ref, 40);
			// The deepest the sphere's surface goes into either box (bodies in name order: b1, b2, c, s).
			Shape bs;
			bs.size = {{0.2, 0.2, 0.2}};
			const ContactShape cb = MakeContactShape(bs, 0.5, 0.5);
			double least = 1e9;
			for (const auto& f : fs)
				for (int k = 0; k < 2; ++k) {
					const auto& b = f.bodies[std::size_t(k)];
					const Quat q{{b.orientation[0], b.orientation[1], b.orientation[2], b.orientation[3]}};
					V3 n;
					least = std::min(least, ShapeDistance(cb, ApplyT(QMatrix(q), Sub(f.bodies[3].position, b.position)), n) - 0.06);
				}
			std::cout << "  sphere and cylinder onto a stack: the sphere's least clearance to the boxes " << least << " m\n";
			Check(Digests(*twin.Snapshot()) == Digests(*ref.Snapshot()) && merged == Digests(*ref.Snapshot()),
				"with body-body contact: deterministic, and stop / restore equals the uninterrupted run");
			Check(least > -0.005, "the sphere does not pass into the stack", std::to_string(least));
		}
		// H3: bounding spheres apart: the bytes are those without body-body contact (each body alone).
		{
			const V both = Bodies({sphere("a", 0.1, 800, {{-0.5, 1, 0}}, {{0.3, 0, 0}}, 0.5, 0.5), sphere("b", 0.1, 800, {{0.5, 1, 0}}, {{-0.3, 0, 0}}, 0.5, 0.5)},
				{{"frames", V::Unsigned(10)}});
			InProcessHost host(Make());
			Check(host.Configure(both, EmptySceneInputs(), error) && host.Start(error) && host.StepToIndex(1000, error), "apart", error);
			const auto fs = frames(host, 10);
			double least = 1e9;
			for (const auto& f : fs) least = std::min(least, Norm(Sub(f.bodies[1].position, f.bodies[0].position)));
			Check(fs.size() == 10 && least > 0.2 && Near(fs.back().bodies[0].velocity[0], 0.3, 1e-15), "bodies that never meet are not touched by the contact");
		}
		// H4: a mesh body among others needs `contact false`.
		SolverDescription d;
		Shape cube;
		cube.size = {{0.2, 0.2, 0.2}};
		std::vector<double> cv;
		std::vector<std::uint32_t> ct;
		ShapeSurface(cube, cv, ct);
		V::Values vs, ts;
		for (double x : cv) vs.push_back(V::Float(x));
		for (std::uint32_t k : ct) ts.push_back(V::Unsigned(k));
		V::Members mesh{{"name", V::String("m")}, {"shape", V::String("mesh")}, {"vertices", V::ArrayValue(vs)}, {"triangles", V::ArrayValue(ts)},
			{"position", Vec3(0, 2, 0)}};
		Check(!Make()->Configure(Bodies({V::MapValue(mesh), sphere("s", 0.1, 800, {{0, 0, 0}}, {{0, 0, 0}}, 0.5, 0.5)}), EmptySceneInputs(), d, error) &&
			error.find("contact false") != std::string::npos, "a mesh body among others without contact false is refused", error);
		mesh.emplace_back("contact", V::Bool(false));
		Check(Make()->Configure(Bodies({V::MapValue(mesh), sphere("s", 0.1, 800, {{0, 0, 0}}, {{0, 0, 0}}, 0.5, 0.5)}), EmptySceneInputs(), d, error),
			"a mesh body with contact false configures", error);
	}
	// ---------------------------------------------------------------- G. refusals
	{
		SolverDescription d;
		auto bad = [&](const V& p, const char* what) { Check(!Make()->Configure(p, EmptySceneInputs(), d, error), what); };
		bad(V::MapValue({}), "no bodies refused");
		bad(Bodies({V::MapValue({{"name", V::String("a")}, {"colour", V::String("red")}})}), "unknown body key refused");
		bad(Bodies({V::MapValue({{"name", V::String("a")}}), V::MapValue({{"name", V::String("a")}})}), "duplicate names refused");
		bad(Bodies({V::MapValue({{"name", V::String("a")}, {"mass", V::Float(1)}, {"density", V::Float(2)}})}), "mass and density refused");
		bad(Bodies({V::MapValue({{"name", V::String("a")}, {"size", Vec3(0, 1, 1)}})}), "degenerate box refused");
		bad(Bodies({V::MapValue({{"name", V::String("a")}})}, {{"wind", V::Float(1)}}), "unknown top-level key refused");
		bad(Bodies({V::MapValue({{"name", V::String("a")}, {"shape", V::String("mesh")}, {"vertices", V::ArrayValue({V::Float(0), V::Float(0), V::Float(0)})},
			{"triangles", V::ArrayValue({V::Unsigned(0), V::Unsigned(0), V::Unsigned(0)})}})}), "a degenerate mesh refused");
	}
	if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
	std::cout << "RigidSolverTest: all checks passed\n";
	return 0;
}
