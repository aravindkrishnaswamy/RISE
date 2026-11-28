// liquid.mac closed-mesh rigid bodies (simulation M7d part 2,
// docs/SIMULATION_SOLVER_CONTRACT.md section 3).
//
//   A  the mesh shape: volume, centroid and moments by the divergence theorem
//      against the analytic sphere (a tessellation's deficit), the distance
//      lattice against the sphere's exact distance, the body frame
//   B  refusals: an open mesh, an inward (inverted) mesh, through the shape
//      builder and through Configure, with a clear error
//   C  validation: a tessellated sphere reproduces the analytic sphere's added
//      mass (impulsive start in a concentric container) and Archimedes
//      equilibrium at density ratios 0.1, 0.5 and 0.9 within the shape's
//      discretisation (1.25 x the observed difference at 32 cells; the
//      difference shrinks at 48 cells under --extended)
//   D  the solver: an inline mesh body configures and round-trips its
//      effective parameters; the body's centre of mass is position + R c;
//      1 and 8 threads publish identical bytes; the chunk field body_object
//      translates to a scene-input mesh body
//   E  a liquid port body following rigid.bodies' mesh body (the same mesh):
//      the two solvers agree on the body frame (the initial pose check
//      passes) and the owned exchange runs
#include "../src/Library/Simulation/Liquid/LiquidSolver.h"
#include "../src/Library/Simulation/Liquid/LiquidParameterSchema.h"
#include "../src/Library/Simulation/Liquid/LiquidValidation.h"
#include "../src/Library/Simulation/Rigid/RigidSolver.h"
#include "../src/Library/Simulation/SimulationGroup.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationParameterSchema.h"
#include "../src/Library/Simulation/SimulationTransform.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

namespace {
using namespace RISE::Simulation;
using V = RISE::RISECBOR64::Value;
constexpr double Pi = 3.14159265358979323846;
int failures = 0;
bool extended = false;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }

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

// An icosphere of radius R (vertices on the sphere), `sub` subdivisions, outward.
void Icosphere(int sub, double R, std::vector<double>& v, std::vector<std::uint32_t>& t)
{
	const double p = (1 + std::sqrt(5.0))/2;
	const double base[12][3] = {{-1, p, 0}, {1, p, 0}, {-1, -p, 0}, {1, -p, 0}, {0, -1, p}, {0, 1, p}, {0, -1, -p}, {0, 1, -p},
		{p, 0, -1}, {p, 0, 1}, {-p, 0, -1}, {-p, 0, 1}};
	std::vector<std::array<double,3>> P;
	for (const auto& b : base) { const double l = std::sqrt(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]); P.push_back({{b[0]/l, b[1]/l, b[2]/l}}); }
	std::vector<std::array<std::uint32_t,3>> F = {{{0, 11, 5}}, {{0, 5, 1}}, {{0, 1, 7}}, {{0, 7, 10}}, {{0, 10, 11}}, {{1, 5, 9}}, {{5, 11, 4}},
		{{11, 10, 2}}, {{10, 7, 6}}, {{7, 1, 8}}, {{3, 9, 4}}, {{3, 4, 2}}, {{3, 2, 6}}, {{3, 6, 8}}, {{3, 8, 9}}, {{4, 9, 5}}, {{2, 4, 11}},
		{{6, 2, 10}}, {{8, 6, 7}}, {{9, 8, 1}}};
	for (int s = 0; s < sub; ++s) {
		std::map<std::pair<std::uint32_t,std::uint32_t>, std::uint32_t> mid;
		auto M = [&](std::uint32_t a, std::uint32_t b) {
			const auto key = std::make_pair(std::min(a, b), std::max(a, b));
			const auto it = mid.find(key);
			if (it != mid.end()) return it->second;
			std::array<double,3> m{{P[a][0] + P[b][0], P[a][1] + P[b][1], P[a][2] + P[b][2]}};
			const double l = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
			for (double& c : m) c /= l;
			P.push_back(m);
			return mid[key] = std::uint32_t(P.size() - 1);
		};
		std::vector<std::array<std::uint32_t,3>> G;
		for (const auto& f : F) {
			const auto a = M(f[0], f[1]), b = M(f[1], f[2]), c = M(f[2], f[0]);
			G.push_back({{f[0], a, c}}); G.push_back({{f[1], b, a}}); G.push_back({{f[2], c, b}}); G.push_back({{a, b, c}});
		}
		F = G;
	}
	v.clear();
	t.clear();
	for (const auto& q : P) for (double c : q) v.push_back(R*c);
	for (const auto& f : F) for (auto k : f) t.push_back(k);
}

V MeshValue(const std::vector<double>& v, const std::vector<std::uint32_t>& t)
{
	V::Values vs, ts;
	for (double x : v) vs.push_back(V::Float(x));
	for (auto k : t) ts.push_back(V::Unsigned(k));
	return V::MapValue({{"vertices", V::ArrayValue(vs)}, {"triangles", V::ArrayValue(ts)}});
}

// ---------------------------------------------------------------- A, B

void Shape()
{
	using namespace Liquid;
	std::printf("A, B: shape and refusals\n");
	std::vector<double> v;
	std::vector<std::uint32_t> t;
	Icosphere(3, 0.15, v, t);
	BodyShape s;
	V3 c;
	Rigid::M3 axes;
	std::string error;
	const int n = 32;
	Check(MakeMeshShape(v, t, 0.5/n, 6.0/n, s, c, axes, error), "a closed icosphere builds", error);
	const double Vs = 4.0/3*Pi*0.15*0.15*0.15;
	// The inscribed polyhedron's volume deficit (exact by the divergence theorem): 0.86% for 1280 triangles.
	Check(s.Volume() < Vs && s.Volume()/Vs - 1 > -1e-2, "the mesh volume is the polyhedron's (below the sphere's by < 1%)", std::to_string(s.Volume()/Vs - 1));
	Check(std::sqrt(c[0]*c[0] + c[1]*c[1] + c[2]*c[2]) < 1e-12, "the centroid of the icosphere is its centre");
	const V3 I = s.Moments(1.0);
	for (int a = 0; a < 3; ++a) Check(std::fabs(I[std::size_t(a)]/(0.4*0.15*0.15) - 1) < 2e-2, "the moments are a sphere's within 2%", std::to_string(I[std::size_t(a)]/(0.4*0.0225) - 1));
	Check(std::fabs(s.SmallestExtent() - 0.3) < 1e-3 && std::fabs(s.BoundingRadius() - 0.15) < 1e-12, "extent and bounding radius");
	// The distance lattice near the surface against the sphere's (sagitta + trilinear error: 0.049 h observed).
	double worst = 0;
	for (int k = 0; k < 4000; ++k) {
		const V3 y{{(k % 13)/13.0*0.4 - 0.2, ((k*7) % 17)/17.0*0.4 - 0.2, ((k*11) % 19)/19.0*0.4 - 0.2}};
		const double r = std::sqrt(y[0]*y[0] + y[1]*y[1] + y[2]*y[2]);
		if (std::fabs(r - 0.15) < 0.1) worst = std::max(worst, std::fabs(s.Distance(y) - (r - 0.15)));
	}
	Check(worst < 0.0625/n, "the mesh distance is within 0.0625 h of the sphere's near the surface", std::to_string(worst*n) + " h");
	const double far = s.Distance({{1, 0, 0}});
	Check(far <= 0.85 + 1e-12 && far > 0.8, "beyond the lattice the distance is a lower bound of the true one (0.85)", std::to_string(far));
	// Refusals: open (one triangle removed), inverted.
	{
		std::vector<std::uint32_t> open(t.begin(), t.end() - 3);
		BodyShape o;
		Check(!MakeMeshShape(v, open, 0.5/n, 6.0/n, o, c, axes, error) && error.find("closed") != std::string::npos, "an open mesh is refused", error);
		std::vector<std::uint32_t> inv = t;
		for (std::size_t k = 0; k < inv.size(); k += 3) std::swap(inv[k + 1], inv[k + 2]);
		Check(!MakeMeshShape(v, inv, 0.5/n, 6.0/n, o, c, axes, error) && error.find("inward") != std::string::npos, "an inward mesh is refused", error);
		auto liquid = Make(LiquidSolverId);
		SolverDescription d;
		V::Members body{{"name", V::String("rock")}, {"shape", V::String("mesh")}};
		const V meshValue = MeshValue(v, open);
		for (const auto& m : meshValue.GetMap()) body.push_back(m);
		const V params = V::MapValue({{"frames", V::Unsigned(2)}, {"resolution", V::Unsigned(24)},
			{"bodies", V::ArrayValue({V::MapValue(body)})}});
		Check(!liquid->Configure(params, EmptySceneInputs(), d, error) && error.find("closed") != std::string::npos &&
			error.find("rock") != std::string::npos, "Configure refuses an open mesh body naming it", error);
	}
}

// ---------------------------------------------------------------- C

void Validation()
{
	using namespace Liquid;
	std::printf("C: tessellated sphere against the sphere\n");
	std::vector<double> v;
	std::vector<std::uint32_t> t;
	Icosphere(3, 0.15, v, t);
	Pool pool(8);
	SolverOptions o;
	struct Row { int n; double addedTol; double archTol[3]; };
	std::vector<Row> rows{{32, 9.0e-3, {5.1e-3, 4.6e-3, 1.04e-2}}};
	if (extended) rows.push_back({48, 5.5e-3, {3.5e-3, 2.1e-3, 3.0e-3}});
	for (const Row& row : rows) {
		const int n = row.n;
		BodyShape mesh;
		V3 c;
		Rigid::M3 axes;
		std::string error;
		Check(MakeMeshShape(v, t, 0.5/n, 6.0/n, mesh, c, axes, error), "mesh", error);
		const auto a = RunAddedMassImpulse(n, true, 0.15, 0.4, 0.37, o, &pool);
		const auto b = RunAddedMassImpulse(n, true, 0.15, 0.4, 0.37, o, &pool, &mesh);
		// Both against the potential-flow added mass of their own volume.
		const double diff = b.error - a.error;
		std::printf("  n %d: added mass error sphere %.3e mesh %.3e (difference %.3e, tolerance %.1e)\n", n, a.error, b.error, diff, row.addedTol);
		Check(std::fabs(diff) <= row.addedTol, "the mesh sphere's added mass is the sphere's within the tolerance", std::to_string(diff));
		const double ratios[3] = {0.1, 0.5, 0.9};
		for (int k = 0; k < 3; ++k) {
			const auto fa = RunFloatingStatics(n, true, ratios[k], 0.37, o, &pool);
			const auto fb = RunFloatingStatics(n, true, ratios[k], 0.37, o, &pool, &mesh);
			const double d = fb.offset - fa.offset;
			std::printf("  n %d ratio %.1f: Archimedes offset / R sphere %.3e mesh %.3e (difference %.3e, tolerance %.1e)\n", n, ratios[k], fa.offset,
				fb.offset, d, row.archTol[k]);
			Check(std::fabs(d) <= row.archTol[k], "the mesh sphere floats where the sphere floats within the tolerance", std::to_string(d));
		}
	}
}

// ---------------------------------------------------------------- D

V::Members Tank(int n, std::uint64_t frames)
{
	return {{"frames", V::Unsigned(frames)}, {"domain_min", Vec(0, 0, 0)}, {"domain_size", Vec(1, 1, 1)},
		{"resolution", V::Unsigned(std::uint64_t(n))},
		{"liquid", V::ArrayValue({V::MapValue({{"type", V::String("fill")}, {"height", V::Float(0.5)}})})}};
}

void Solver()
{
	std::printf("D: the solver\n");
	std::vector<double> v;
	std::vector<std::uint32_t> t;
	Icosphere(2, 0.12, v, t);
	for (std::size_t k = 0; k < v.size(); k += 3) v[k] += 0.05;   // the authoring origin off the centroid
	std::string error;
	V::Members body{{"name", V::String("buoy")}, {"shape", V::String("mesh")}, {"density", V::Float(500)}, {"position", Vec(0.4, 0.55, 0.5)},
		{"orientation", V::ArrayValue({V::Float(std::cos(0.2)), V::Float(0), V::Float(0), V::Float(std::sin(0.2))})}};
	const V meshValue = MeshValue(v, t);
	for (const auto& m : meshValue.GetMap()) body.push_back(m);
	V::Members p = Tank(20, 4);
	p.emplace_back("bodies", V::ArrayValue({V::MapValue(body)}));
	DigestSink reference;
	for (int threads : {1, 8}) {
		auto s = Make(LiquidSolverId);
		SolverDescription d, again;
		DigestSink sink;
		V::Members q = p;
		q.emplace_back("threads", V::Unsigned(std::uint64_t(threads)));
		Check(s->Configure(V::MapValue(q), EmptySceneInputs(), d, error), "a mesh body configures", error);
		Bytes e1, e2;
		Check(s->Configure(d.effectiveParameters, EmptySceneInputs(), again, error) && RISE::RISECBOR64::Encode(d.effectiveParameters, e1, &error) &&
			RISE::RISECBOR64::Encode(again.effectiveParameters, e2, &error) && e1 == e2, "the effective parameters reproduce themselves", error);
		Check(s->Configure(V::MapValue(q), EmptySceneInputs(), d, error) && s->Start(error) && RunAll(*s, sink, error), "a mesh body runs", error);
		if (threads == 1) reference = sink;
		else Check(sink.digests == reference.digests, "8 threads publish the bytes of one");
	}
	TransformFrame f;
	Check(DecodeTransforms(*reference.payloads.at({0, "bodies"}), std::size_t(1) << 30, f, error) && f.bodies.size() == 1, "bodies output", error);
	if (f.bodies.size() == 1) {
		// The centre of mass: position + R c, c = (0.05, 0, 0) in the authoring frame, R a rotation by 0.4 rad about z.
		const double x = 0.4 + 0.05*std::cos(0.4), y = 0.55 + 0.05*std::sin(0.4);
		Check(std::fabs(f.bodies[0].position[0] - x) < 1e-9 && std::fabs(f.bodies[0].position[1] - y) < 1e-9 && std::fabs(f.bodies[0].position[2] - 0.5) < 1e-9,
			"the mesh body's centre of mass is position + R c");
	}
	// The chunk field body_object: a scene-input mesh body named after the object.
	{
		const ParameterSchema& schema = LiquidParameterSchema();
		V params;
		std::vector<std::string> inputs;
		Check(TranslateSchemaChunk(schema, {{"body_object", {"rock density 2500 motion dynamic lock xz"}}}, params, inputs, error) &&
			params.Find("bodies") && inputs == std::vector<std::string>{"rock"}, "body_object translates", error);
		if (params.Find("bodies")) {
			const V& b = params.Find("bodies")->GetArray()[0];
			Check(b.Find("shape") && b.Find("shape")->GetText() == "mesh" && b.Find("scene_input") && b.Find("scene_input")->GetText() == "rock" &&
				b.Find("name")->GetText() == "rock", "body_object is a scene-input mesh named after the object");
		}
		Check(!TranslateSchemaChunk(schema, {{"body_object", {""}}}, params, inputs, error), "body_object needs an object");
	}
}

// ---------------------------------------------------------------- E

void Following()
{
	std::printf("E: a port body following rigid.bodies' mesh body\n");
	std::vector<double> v;
	std::vector<std::uint32_t> t;
	Icosphere(2, 0.1, v, t);
	// An off-centre, rotated authoring frame: the body frame is the centroid and principal axes in both solvers.
	for (std::size_t k = 0; k < v.size(); k += 3) { v[k] *= 1.4; v[k + 1] *= 0.8; v[k] += 0.03; }
	const V mesh = MeshValue(v, t);
	const int n = 24;
	const auto frames = std::uint64_t(4);
	V::Members liquidBody{{"name", V::String("egg")}, {"shape", V::String("mesh")}, {"motion", V::String("port")}, {"position", Vec(0.5, 0.6, 0.5)}};
	V::Members rigidBody{{"name", V::String("egg")}, {"shape", V::String("mesh")}, {"density", V::Float(600)}, {"position", Vec(0.5, 0.6, 0.5)}};
	for (const auto& m : mesh.GetMap()) { liquidBody.push_back(m); rigidBody.push_back(m); }
	V::Members liquid = Tank(n, frames);
	liquid.emplace_back("bodies", V::ArrayValue({V::MapValue(liquidBody)}));
	const V rigid = V::MapValue({{"frames", V::Unsigned(frames)}, {"substeps", V::Unsigned(4)}, {"bodies", V::ArrayValue({V::MapValue(rigidBody)})}});
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.memberOrder = {"pool", "drop"};
	p.ownedExchanges = {{"pool", {"drop"}}};
	SimulationGroup G;
	std::string error;
	bool ok = G.AddMember("pool", Make(LiquidSolverId), V::MapValue(liquid), error) && G.AddMember("drop", Make(RigidSolverId), rigid, error) &&
		G.Connect({"drop", "body_state", "pool", "body_state"}, error) && G.Connect({"drop", "body_mass_properties", "pool", "body_mass_properties"}, error) &&
		G.Connect({"pool", "body_load", "drop", "body_load"}, error) && G.Configure(p, EmptySceneInputs(), error) && G.Start(error);
	Check(ok, "the group configures", error);
	while (ok && !G.Finished()) { ok = G.StepMacro(error); Check(ok, "the owned exchange runs (the initial poses agree)", error); }
}
}

int main(int argc, char** argv)
{
	if (const char* e = std::getenv("RISE_TEST_EXTENDED")) if (std::strcmp(e, "1") == 0) extended = true;
	for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--extended") == 0) extended = true;
	std::string error;
	Check(RegisterLiquidSolver(error), "register liquid.mac", error);
	Check(RegisterRigidSolver(error), "register rigid.bodies", error);
	const double t0 = Seconds();
	Shape();
	Validation();
	Solver();
	Following();
	std::printf("LiquidMeshBodyTest: %.1f s\n", Seconds() - t0);
	if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	std::printf("LiquidMeshBodyTest: all checks passed\n");
	return 0;
}
