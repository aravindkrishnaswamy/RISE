// Simulation M9 (docs/SIMULATION_M9_RESULTS.md, task 3): analytic rigid bodies two-way coupled to cloth that
// also collide with each other.  A rigid.bodies sphere or box takes its coupling surface from a mesh
// (`surface_vertices` / `surface_triangles`, or a scene object's geometry with `surface_input`; world space at
// t = 0 with the body at its initial pose); the cloth's port collider made from the same mesh then has that
// surface's topology, so rigid_surface drives it, while the body keeps its analytic shape for mass, planes and
// body-body contact (mesh bodies take none).
//
//   A  zero gravity, a free cotton sheet: a box moving up strikes a sphere at rest below the sheet (body-body
//      contact), the sphere is thrown into the sheet (cloth contact, the reaction returned to it): the total
//      linear momentum of the sheet and both bodies is conserved (implicit Gauss-Seidel coupling); the sphere
//      gains the box's momentum, then the sheet does
//   B  a trampoline: a sphere and, above it, a box dropped on a sheet pinned at its corners: the box lands on the
//      sphere (body-body), both are caught by the sheet (neither passes it); the frames of a run do not depend on
//      the surface's origin (a tessellation unlike the shape's own record surface)
//   C  refusals and the record: a mesh body with a surface is refused; without one the rigid record and the
//      effective parameters are unchanged; the record carries the given surface in the body frame
#include "../src/Library/Simulation/SimulationGroup.h"
#include "../src/Library/Simulation/SimulationPortAdapters.h"
#include "../src/Library/Simulation/BuiltinSolvers.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Simulation/SimulationTransform.h"
#include "../src/Library/Simulation/Rigid/RigidSolver.h"
#include "../src/Library/Simulation/Cloth/ClothSolver.h"
#include "../src/Library/Simulation/Coupling/ClothRigidCoupling.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {
using namespace RISE::Simulation;
using V = RISE::RISECBOR64::Value;
constexpr std::size_t Budget = std::size_t(256) << 20;
constexpr double Pi = 3.14159265358979323846;
int failures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = {})
{
	if (!condition) { ++failures; std::cerr << "FAIL: " << what << " " << detail << "\n"; }
}
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
V Vec(double a, double b, double c) { return V::ArrayValue({V::Float(a), V::Float(b), V::Float(c)}); }
double F32(std::uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

// A latitude-longitude sphere (not the rigid record's icosphere) and a box of 8 corners, world space.
struct Mesh { std::vector<double> v; std::vector<std::uint32_t> t; };
Mesh UVSphere(const std::array<double,3>& c, double r, int rings = 6, int segments = 10)
{
	Mesh m;
	m.v.insert(m.v.end(), {c[0], c[1] + r, c[2]});
	for (int i = 1; i < rings; ++i) {
		const double th = Pi*i/rings;
		for (int j = 0; j < segments; ++j) {
			const double ph = 2*Pi*j/segments;
			m.v.insert(m.v.end(), {c[0] + r*std::sin(th)*std::cos(ph), c[1] + r*std::cos(th), c[2] - r*std::sin(th)*std::sin(ph)});
		}
	}
	m.v.insert(m.v.end(), {c[0], c[1] - r, c[2]});
	const std::uint32_t south = std::uint32_t(m.v.size()/3 - 1);
	auto ring = [&](int i, int j) { return std::uint32_t(1 + (i - 1)*segments + (j % segments)); };
	for (int j = 0; j < segments; ++j) m.t.insert(m.t.end(), {0u, ring(1, j), ring(1, j + 1)});
	for (int i = 1; i + 1 < rings; ++i)
		for (int j = 0; j < segments; ++j) {
			m.t.insert(m.t.end(), {ring(i, j), ring(i + 1, j), ring(i + 1, j + 1)});
			m.t.insert(m.t.end(), {ring(i, j), ring(i + 1, j + 1), ring(i, j + 1)});
		}
	for (int j = 0; j < segments; ++j) m.t.insert(m.t.end(), {south, ring(rings - 1, j + 1), ring(rings - 1, j)});
	return m;
}
Mesh Box(const std::array<double,3>& c, double s)
{
	Mesh m;
	const double h = s/2;
	for (int k = 0; k < 8; ++k) m.v.insert(m.v.end(), {c[0] + (k & 1 ? h : -h), c[1] + (k & 2 ? h : -h), c[2] + (k & 4 ? h : -h)});
	const std::uint32_t f[12][3] = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
		{2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
	for (const auto& tri : f) m.t.insert(m.t.end(), {tri[0], tri[1], tri[2]});
	return m;
}
V Floats(const std::vector<double>& x) { V::Values out; for (double a : x) out.push_back(V::Float(a)); return V::ArrayValue(out); }
V Counts(const std::vector<std::uint32_t>& x) { V::Values out; for (auto a : x) out.push_back(V::Unsigned(a)); return V::ArrayValue(out); }

struct Body { std::string name; std::string shape; double size = 0, mass = 0; std::array<double,3> at{{0, 0, 0}}, velocity{{0, 0, 0}}; Mesh mesh; };

struct Scenario {
	std::vector<Body> bodies;
	bool gravity = true, pins = true;
	double sheetSize = 0.8, sheetY = 0;
	std::uint32_t vertices = 15;
	std::uint64_t frames = 31, substeps = 8;
	double rate = 30;
};

V RigidParameters(const Scenario& sc, bool withSurfaces)
{
	V::Values bodies;
	for (const Body& b : sc.bodies) {
		V::Members m{{"name", V::String(b.name)}, {"shape", V::String(b.shape)}, {"mass", V::Float(b.mass)},
			{"position", Vec(b.at[0], b.at[1], b.at[2])}, {"velocity", Vec(b.velocity[0], b.velocity[1], b.velocity[2])},
			{"restitution", V::Float(0.2)}, {"friction", V::Float(0.4)}};
		if (b.shape == "sphere") m.emplace_back("radius", V::Float(b.size));
		else m.emplace_back("size", Vec(b.size, b.size, b.size));
		if (withSurfaces) { m.emplace_back("surface_vertices", Floats(b.mesh.v)); m.emplace_back("surface_triangles", Counts(b.mesh.t)); }
		bodies.push_back(V::MapValue(m));
	}
	V::Members p{{"bodies", V::ArrayValue(bodies)}, {"frames", V::Unsigned(sc.frames)}, {"frame_rate", V::Float(sc.rate)},
		{"substeps", V::Unsigned(sc.substeps)}};
	if (!sc.gravity) p.emplace_back("gravity", Vec(0, 0, 0));
	return V::MapValue(p);
}

V ClothParameters(const Scenario& sc)
{
	V::Values colliders;
	for (const Body& b : sc.bodies)
		colliders.push_back(V::MapValue({{"type", V::String("mesh")}, {"vertices", Floats(b.mesh.v)}, {"triangles", Counts(b.mesh.t)},
			{"motion", V::MapValue({{"type", V::String("port")}, {"port", V::String("collider_" + b.name)}})}}));
	V::Members m{{"mode", V::String("bake")}, {"vertices_u", V::Unsigned(sc.vertices)}, {"vertices_v", V::Unsigned(sc.vertices)},
		{"size_u", V::Float(sc.sheetSize)}, {"size_v", V::Float(sc.sheetSize)}, {"origin", Vec(-0.5*sc.sheetSize, sc.sheetY, 0.5*sc.sheetSize)},
		{"frames", V::Unsigned(sc.frames)}, {"frame_rate", V::Float(sc.rate)}, {"substeps", V::Unsigned(sc.substeps)},
		{"material", V::String("cotton-plain-weave")}, {"colliders", V::ArrayValue(colliders)}};
	if (sc.pins) {
		V::Values pins;
		for (const char* corner : {"u0v0", "u1v0", "u0v1", "u1v1"}) pins.push_back(V::MapValue({{"select", V::String(corner)}}));
		m.emplace_back("pins", V::ArrayValue(pins));
	}
	if (!sc.gravity) {
		m.emplace_back("gravity", Vec(0, 0, 0));
		m.emplace_back("integrator", V::String("backward_euler"));
		m.emplace_back("damping", V::Bool(false));
	}
	return V::MapValue(m);
}

bool Build(SimulationGroup& g, const Scenario& sc, std::string& error)
{
	bool ok = g.AddMember("body", CreateSolver(RigidSolverId, error), RigidParameters(sc, true), error) &&
		g.AddMember("sheet", CreateSolver(ClothSolverId, error), ClothParameters(sc), error);
	V::Values targets;
	for (const Body& b : sc.bodies) {
		PortConnection c;
		c.fromMember = "body"; c.fromPort = "body_state"; c.toMember = "sheet"; c.toPort = "collider_" + b.name;
		c.adapter = RigidSurfaceAdapterId;
		c.adapterParameters = V::MapValue({{"body", V::String(b.name)}});
		ok = ok && g.Connect(c, error);
		targets.push_back(V::String(b.name));
	}
	PortConnection r;
	r.fromMember = "sheet"; r.fromPort = "collider_reaction"; r.toMember = "body"; r.toPort = "body_load";
	r.adapter = ContactReactionAdapterId;
	r.adapterParameters = V::MapValue({{"targets", V::ArrayValue(targets)}});
	ok = ok && g.Connect(r, error);
	GroupPolicy p;
	p.order = ExchangeOrder::GaussSeidel;
	p.memberOrder = {"body", "sheet"};
	p.subIterations = 30;
	p.convergenceTolerance = 1e-6;
	p.absoluteTolerance = 1e-4;
	p.aitken = true;
	p.onNonConvergence = GroupPolicy::NonConvergence::Halve;   // as conjurer: a resting contact converges on a shorter step
	p.minimumMacroStep = 0.002;
	return ok && g.Configure(p, EmptySceneInputs(), error);
}

struct Frame { std::array<double,3> sheetMomentum{{0, 0, 0}}; double sheetLowest = 1e9; std::vector<TransformBody> bodies; };

bool Measure(SimulationGroup& g, const Scenario& sc, std::int64_t i, Frame& f, std::string& error)
{
	std::shared_ptr<const Bytes> bytes;
	MeshFrame m;
	auto sheet = g.Snapshot("sheet");
	if (sheet->Read(i, "surface", Budget, bytes, error) != FrameStatus::Ready || !DecodeMesh(*bytes, Budget, sheet->Resolver(), m, error)) return false;
	const std::uint32_t n = sc.vertices;
	const double h = sc.sheetSize/(n - 1);
	f = Frame();
	for (std::uint32_t j = 0; j < n; ++j) for (std::uint32_t k = 0; k < n; ++k) {
		const std::size_t q = std::size_t(k + n*j);
		const double mass = 0.15*((k == 0 || k == n - 1) ? 0.5*h : h)*((j == 0 || j == n - 1) ? 0.5*h : h);
		for (int a = 0; a < 3; ++a) f.sheetMomentum[std::size_t(a)] += mass*F32(m.velocities[3*q + std::size_t(a)]);
		f.sheetLowest = std::min(f.sheetLowest, F32(m.positions[3*q + 1]));
	}
	auto body = g.Snapshot("body");
	TransformFrame t;
	if (body->Read(i, "bodies", Budget, bytes, error) != FrameStatus::Ready || !DecodeTransforms(*bytes, Budget, t, error)) return false;
	f.bodies = t.bodies;
	return true;
}

double MassOf(const Scenario& sc, const std::string& name) { for (const Body& b : sc.bodies) if (b.name == name) return b.mass; return 0; }

void ZeroGravity()
{
	std::printf("A: zero gravity, a free sheet: box -> sphere -> sheet\n");
	std::string error;
	Scenario sc;
	sc.gravity = false;
	sc.pins = false;
	sc.sheetSize = 0.6;
	sc.vertices = 13;
	sc.sheetY = 0.16;
	sc.rate = 60;
	sc.frames = 31;
	Body ball{"ball", "sphere", 0.05, 0.1, {{0, 0.05, 0}}, {{0, 0, 0}}, {}};
	ball.mesh = UVSphere(ball.at, ball.size);
	Body box{"box", "box", 0.08, 0.15, {{0.0, -0.2, 0}}, {{0, 1.2, 0}}, {}};
	box.mesh = Box(box.at, box.size);
	sc.bodies = {ball, box};
	SimulationGroup g;
	const double t0 = Seconds();
	if (!Build(g, sc, error) || !g.Start(error) || !g.Run(error)) { Check(false, "zero-g run", error); return; }
	const double P0 = 0.15*1.2;
	double worst = 0, ballMax = 0, sheetMax = 0, boxMin = 1e9;
	for (std::int64_t i = 0; i < std::int64_t(sc.frames); ++i) {
		Frame f;
		if (!Measure(g, sc, i, f, error)) { Check(false, "zero-g measure", error); return; }
		std::array<double,3> P = f.sheetMomentum;
		for (const TransformBody& b : f.bodies) {
			for (int a = 0; a < 3; ++a) P[std::size_t(a)] += MassOf(sc, b.name)*b.velocity[std::size_t(a)];
			if (b.name == "ball") ballMax = std::max(ballMax, b.velocity[1]);
			if (b.name == "box") boxMin = std::min(boxMin, b.velocity[1]);
		}
		sheetMax = std::max(sheetMax, f.sheetMomentum[1]);
		const double d = std::sqrt((P[0])*(P[0]) + (P[1] - P0)*(P[1] - P0) + P[2]*P[2]);
		worst = std::max(worst, d/P0);
	}
	std::printf("  total momentum drift up to %.2e of the box's (0.18 N s); the ball's largest upward speed %.3f m/s (the box's lowest %.3f m/s); "
		"the sheet's largest momentum %.4f N s; %.1f s\n", worst, ballMax, boxMin, sheetMax, Seconds() - t0);
	Check(worst < 1e-3, "the total linear momentum of sheet and bodies is conserved (1e-3)");
	Check(ballMax > 0.5 && boxMin < 1.0, "the box strikes the sphere (body-body contact): the sphere is thrown up, the box slowed");
	Check(sheetMax > 0.02, "the sphere is caught by the sheet: the sheet takes up its momentum");
}

void Trampoline()
{
	std::printf("B: a trampoline: a box dropped onto a sphere on a pinned sheet\n");
	std::string error;
	Scenario sc;
	sc.frames = 19;   // 0.6 s
	sc.vertices = 11;
	Body ball{"ball", "sphere", 0.08, 0.3, {{0, 0.1, 0}}, {{0, 0, 0}}, {}};
	ball.mesh = UVSphere(ball.at, ball.size);
	Body box{"box", "box", 0.1, 0.2, {{0.02, 0.26, 0.01}}, {{0, 0, 0}}, {}};
	box.mesh = Box(box.at, box.size);
	sc.bodies = {ball, box};
	SimulationGroup g;
	const double t0 = Seconds();
	if (!Build(g, sc, error) || !g.Start(error) || !g.Run(error)) { Check(false, "trampoline run", error); return; }
	double closest = 1e9, passed = -1e9;
	Frame last;
	for (std::int64_t i = 0; i < std::int64_t(sc.frames); ++i) {
		Frame f;
		if (!Measure(g, sc, i, f, error)) { Check(false, "trampoline measure", error); return; }
		const TransformBody* b = nullptr;
		const TransformBody* c = nullptr;
		for (const TransformBody& x : f.bodies) { if (x.name == "ball") b = &x; if (x.name == "box") c = &x; }
		if (!b || !c) { Check(false, "bodies in the frame"); return; }
		const double dx = c->position[0] - b->position[0], dy = c->position[1] - b->position[1], dz = c->position[2] - b->position[2];
		closest = std::min(closest, std::sqrt(dx*dx + dy*dy + dz*dz));
		// Neither body's centre goes below the sheet's lowest point (a body through the sheet would).
		passed = std::max(passed, std::max(f.sheetLowest - b->position[1], f.sheetLowest - c->position[1]));
		last = f;
	}
	std::printf("  closest centres %.4f m (touching: 0.08 + 0.05 = 0.13 m and more); the bodies' centres at least %.4f m above the sheet's "
		"lowest point; sheet lowest %.4f m at 0.6 s; %.1f s\n", closest, -passed, last.sheetLowest, Seconds() - t0);
	Check(closest > 0.1 && closest < 0.16, "the box lands on the sphere and does not pass through it (body-body contact)");
	Check(passed < 0, "neither body passes through the sheet");
	Check(last.sheetLowest < -0.02, "the sheet carries both bodies");
}

void RecordAndRefusals()
{
	std::printf("C: the record and refusals\n");
	std::string error;
	Scenario sc;
	Body ball{"ball", "sphere", 0.08, 0.3, {{0, 0.12, 0}}, {{0, 0, 0}}, {}};
	ball.mesh = UVSphere(ball.at, ball.size);
	sc.bodies = {ball};
	auto s = CreateSolver(RigidSolverId, error);
	SolverDescription with, without;
	Check(s && s->Configure(RigidParameters(sc, true), EmptySceneInputs(), with, error), "configure with a surface", error);
	Check(s && s->Configure(RigidParameters(sc, false), EmptySceneInputs(), without, error), "configure without", error);
	std::vector<double> v;
	std::vector<std::uint32_t> t;
	Check(RigidBodySurface(with.cache.physicalRecords, "ball", v, t, error) && t == ball.mesh.t && v.size() == ball.mesh.v.size(), "the record carries the given surface", error);
	double worst = 0;
	for (std::size_t k = 0; k + 2 < v.size(); k += 3) worst = std::max(worst, std::fabs(std::sqrt(v[k]*v[k] + v[k + 1]*v[k + 1] + v[k + 2]*v[k + 2]) - 0.08));
	Check(worst < 1e-12, "the surface is in the body frame (about the centre of mass)");
	Check(RigidBodySurface(without.cache.physicalRecords, "ball", v, t, error) && v.size() == 3*42, "without a surface the record keeps the icosphere");
	Bytes a, b;
	RISE::RISECBOR64::Encode(without.effectiveParameters, a, nullptr);
	RISE::RISECBOR64::Encode(with.effectiveParameters, b, nullptr);
	Check(a != b && without.version == "m6.2" && with.version == "m6.2", "the version is unchanged (m6.2); the surface is keyed when given");
	V::Members bad{{"bodies", V::ArrayValue({V::MapValue({{"name", V::String("m")}, {"shape", V::String("mesh")},
		{"vertices", Floats(ball.mesh.v)}, {"triangles", Counts(ball.mesh.t)}, {"surface_vertices", Floats(ball.mesh.v)},
		{"surface_triangles", Counts(ball.mesh.t)}})})}};
	std::string e;
	Check(!s->Configure(V::MapValue(bad), EmptySceneInputs(), with, e), "a mesh body with a coupling surface is refused");
}
}

int main()
{
	std::string error;
	if (!RegisterBuiltinSolvers(error)) { std::cerr << "register: " << error << "\n"; return 1; }
	const double t0 = Seconds();
	ZeroGravity();
	Trampoline();
	RecordAndRefusals();
	std::printf("%s (%.1f s)\n", failures ? "FAILED" : "all passed", Seconds() - t0);
	return failures ? 1 : 0;
}
