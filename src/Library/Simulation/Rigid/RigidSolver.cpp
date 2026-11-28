//////////////////////////////////////////////////////////////////////
//
//  RigidSolver.cpp - "rigid.bodies" on the solver contract (simulation M6)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RigidSolver.h"
#include "RigidBody.h"
#include "../SimulationField.h"
#include "../SimulationMesh.h"
#include "../SimulationTransform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>

namespace RISE { namespace Simulation {

namespace {
using V = RISECBOR64::Value;
using namespace Rigid;

constexpr std::size_t CodecBudget = std::size_t(64) << 20;
// m6.2 (M7h): body-body contact (ResolvePairs in RigidBody.h) and the contact_planes port; bakes in
// which no two bodies' bounding spheres meet publish the frames of m6.1.
constexpr const char* SolverVersion = "m6.2";
constexpr const char* StateFormat = "rigid.bodies.state.v1";
constexpr const char* StatePort = "body_state";
constexpr const char* LoadPort = "body_load";
// M7 prerequisites: each body's constant mass properties, for a partner that
// solves the body's motion inside its own step (an owned implicit exchange).
constexpr const char* MassPort = "body_mass_properties";
// M7h: the contact planes and the bodies' contact parameters, for a partner that
// moves its own copies of the bodies inside its step (liquid.mac): field.v1 over
// the planes then the bodies (kind 0 plane: point, normal, restitution,
// friction; kind 1 body, in body_state's order: contact flag, restitution,
// friction).  Offered once, at the start time, with body_mass_properties.
constexpr const char* ContactPort = "body_contact";
constexpr const char* Output = "bodies";
constexpr double Pi = 3.14159265358979323846;

bool Fail(std::string& error, const std::string& message) { error = message; return false; }

bool Number(const V& v, double& x)
{
	if (v.GetType() == V::Float64) x = v.GetFloat();
	else if (v.GetType() == V::UnsignedInteger) x = static_cast<double>(v.GetIntegerArgument());
	else if (v.GetType() == V::NegativeInteger) x = -1.0 - static_cast<double>(v.GetIntegerArgument());
	else return false;
	return std::isfinite(x);
}

template<std::size_t N> V Vec(const std::array<double,N>& a)
{
	V::Values out;
	for (double x : a) out.push_back(V::Float(x));
	return V::ArrayValue(out);
}

// Strict map reader: unknown keys are refused; read keys are written back
// with canonical types into `out`.
class Reader {
public:
	Reader(const V& map, std::string where, std::string& error) : map_(map), where_(std::move(where)), error_(error) {
		if (map.GetType() != V::Map) Bad("must be a map");
	}
	double Real(const char* key, double fallback, bool effective = true) {
		double x = fallback;
		if (const V* v = Use(key)) if (!Number(*v, x)) Bad(std::string(key) + " must be a finite number");
		if (effective) out.emplace_back(key, V::Float(x));
		return x;
	}
	bool Has(const char* key) const { return map_.GetType() == V::Map && map_.Find(key) != nullptr; }
	std::uint64_t Count(const char* key, std::uint64_t fallback, std::uint64_t low, std::uint64_t high) {
		std::uint64_t n = fallback;
		if (const V* v = Use(key)) {
			double x = 0;
			if (v->GetType() == V::UnsignedInteger) n = v->GetIntegerArgument();
			else if (Number(*v, x) && x >= 0 && std::floor(x) == x && x < 9e15) n = static_cast<std::uint64_t>(x);
			else Bad(std::string(key) + " must be a count");
		}
		if (n < low || n > high) Bad(std::string(key) + " out of range");
		out.emplace_back(key, V::Unsigned(n));
		return n;
	}
	std::string Text(const char* key, const std::string& fallback, std::initializer_list<const char*> allowed = {},
		bool effective = true) {
		std::string s = fallback;
		if (const V* v = Use(key)) {
			if (v->GetType() == V::Text && !v->GetText().empty()) s = v->GetText();
			else Bad(std::string(key) + " must be text");
		}
		if (allowed.size() && std::none_of(allowed.begin(), allowed.end(), [&](const char* a) { return s == a; }))
			Bad(std::string(key) + " has an unknown value " + s);
		if (effective) out.emplace_back(key, V::String(s));
		return s;
	}
	template<std::size_t N> std::array<double,N> Vector(const char* key, const std::array<double,N>& fallback,
		bool effective = true) {
		std::array<double,N> a = fallback;
		if (const V* v = Use(key)) {
			bool good = v->GetType() == V::Array && v->GetArray().size() == N;
			for (std::size_t k = 0; good && k < N; ++k) good = Number(v->GetArray()[k], a[k]);
			if (!good) Bad(std::string(key) + " must be an array of " + std::to_string(N) + " finite numbers");
		}
		if (effective) out.emplace_back(key, Vec(a));
		return a;
	}
	const V* Raw(const char* key) { return Use(key); }
	bool Finish() {
		if (!ok_) return false;
		for (const auto& m : map_.GetMap())
			if (std::find(used_.begin(), used_.end(), m.first) == used_.end()) return Bad("unknown parameter " + m.first);
		return true;
	}
	bool Ok() const { return ok_; }
	bool Bad(const std::string& message) {
		if (ok_) error_ = where_.empty() ? message : where_ + ": " + message;
		ok_ = false;
		return false;
	}
	V::Members out;
private:
	const V* Use(const char* key) {
		used_.push_back(key);
		return ok_ && map_.GetType() == V::Map ? map_.Find(key) : nullptr;
	}
	const V& map_;
	std::string where_;
	std::string& error_;
	std::vector<std::string> used_;
	bool ok_ = true;
};

bool Numbers(const V* v, std::vector<double>& out)
{
	out.clear();
	if (!v || v->GetType() != V::Array) return false;
	for (const V& e : v->GetArray()) {
		double x = 0;
		if (!Number(e, x)) return false;
		out.push_back(x);
	}
	return true;
}

double FromF32(std::uint32_t bits)
{
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

Bytes PackDoubles(const std::vector<double>& v)
{
	Bytes out(v.size()*8);
	for (std::size_t i = 0; i < v.size(); ++i) {
		std::uint64_t w;
		std::memcpy(&w, &v[i], 8);
		for (unsigned b = 0; b < 8; ++b) out[8*i + b] = static_cast<unsigned char>(w >> (8*b));
	}
	return out;
}
bool UnpackDoubles(const V* value, std::size_t count, std::vector<double>& v, bool allowEmpty)
{
	if (!value || value->GetType() != V::ByteString) return false;
	const auto& b = value->GetBytes();
	if (allowEmpty && b.empty()) { v.clear(); return true; }
	if (b.size() != count*8) return false;
	v.assign(count, 0);
	for (std::size_t i = 0; i < count; ++i) {
		std::uint64_t w = 0;
		for (unsigned k = 0; k < 8; ++k) w |= std::uint64_t(b[8*i + k]) << (8*k);
		std::memcpy(&v[i], &w, 8);
	}
	return true;
}

// A body as configured: its constants, its initial state, its surface.
struct BodySpec {
	Body initial;
	Shape shape;
	ContactShape contact;                   // M7h: body-body contact (None: a mesh, or `contact` false)
	bool contactOff = false;                // M7h: `contact` false
	std::vector<double> surface;            // body frame
	std::vector<std::uint32_t> triangles;
};

bool ParseBody(const V& item, const ISceneInputs& inputs, std::vector<std::string>& sceneNames, BodySpec& spec,
	V& effective, std::string& error)
{
	Reader r(item, "body", error);
	const std::string name = r.Text("name", "");
	if (name.empty() || name.find('\0') != std::string::npos || !RISECBOR64::IsUTF8NFC(name))
		return r.Bad("a body needs a nonempty NFC name");
	const std::string type = r.Text("shape", "box", {"box", "sphere", "cylinder", "mesh"});
	Shape& s = spec.shape;
	bool sceneMesh = false;
	if (type == "box") { s.type = Shape::Type::Box; s.size = r.Vector<3>("size", {{0.2, 0.2, 0.2}}); }
	else if (type == "sphere") { s.type = Shape::Type::Sphere; s.radius = r.Real("radius", 0.1); }
	else if (type == "cylinder") {
		s.type = Shape::Type::Cylinder; s.radius = r.Real("radius", 0.1); s.height = r.Real("height", 0.2);
	} else {
		s.type = Shape::Type::Mesh;
		const std::string input = r.Text("scene_input", "-", {}, false);
		if (input != "-") {
			// A scene object's geometry, world space at t = 0; the body's pose comes from it.
			std::string hash, codec;
			std::shared_ptr<const Bytes> bytes;
			MeshFrame frame;
			if (!inputs.Identity(input, hash, error) || !inputs.Sample(input, 0.0, bytes, codec, error) || !bytes)
				return Fail(error, "body scene input " + input + ": " + error);
			const StaticPayloadResolver resolve = [&inputs](const std::string& digest, std::size_t budget,
				std::shared_ptr<const Bytes>& fixed, std::string& why) { return inputs.ResolveStatic(digest, budget, fixed, why); };
			if ((codec != "mesh.v1" && codec != "mesh.v2") || !DecodeMesh(*bytes, CodecBudget, resolve, frame, error))
				return Fail(error, "body scene input " + input + " is not a mesh frame: " + error);
			for (std::uint32_t bits : frame.positions) s.vertices.push_back(FromF32(bits));
			s.triangles = frame.indices;
			if (std::find(sceneNames.begin(), sceneNames.end(), input) == sceneNames.end()) sceneNames.push_back(input);
			r.out.emplace_back("scene_input", V::String(input));
			r.out.emplace_back("identity", V::String(hash));
			r.Raw("identity"); // written back by Configure; accepted on input
			sceneMesh = true;
		} else {
			std::vector<double> idx;
			if (!Numbers(r.Raw("vertices"), s.vertices) || s.vertices.size() % 3 != 0 || s.vertices.empty())
				return r.Bad("mesh vertices must be a flat array of 3n finite numbers");
			if (!Numbers(r.Raw("triangles"), idx) || idx.size() % 3 != 0 || idx.empty())
				return r.Bad("mesh triangles must be a flat array of 3m vertex indices");
			for (double k : idx) {
				if (!(k >= 0) || std::floor(k) != k || k >= static_cast<double>(s.vertices.size()/3))
					return r.Bad("mesh triangle index out of range");
				s.triangles.push_back(static_cast<std::uint32_t>(k));
			}
			V::Values vs, ts;
			for (double x : s.vertices) vs.push_back(V::Float(x));
			for (std::uint32_t k : s.triangles) ts.push_back(V::Unsigned(k));
			r.out.emplace_back("vertices", V::ArrayValue(vs));
			r.out.emplace_back("triangles", V::ArrayValue(ts));
		}
	}
	// Density, or a total mass (then the density follows from the volume).
	const bool byMass = r.Has("mass");
	double density = 1000, mass = 0;
	if (byMass) { mass = r.Real("mass", 1.0); if (r.Has("density")) return r.Bad("give density or mass, not both"); }
	else density = r.Real("density", 1000.0);
	V3 position{{0, 0, 0}};
	Quat orientation{{1, 0, 0, 0}};
	if (sceneMesh) {
		if (r.Has("position") || r.Has("orientation")) return r.Bad("a scene_input body takes its pose from the scene");
	} else {
		position = r.Vector<3>("position", position);
		Quat q = r.Vector<4>("orientation", orientation, false);
		if (!CanonicalQuaternion(q)) return r.Bad("orientation must be a nonzero quaternion (w, x, y, z)");
		orientation = q;
		r.out.emplace_back("orientation", Vec(q));
	}
	const V3 velocity = r.Vector<3>("velocity", {{0, 0, 0}});
	const V3 omega = r.Vector<3>("angular_velocity", {{0, 0, 0}});
	// M7h: body-body contact parameters (written back only when given: other bodies keep m6.1's
	// effective parameters).  Defaults are the planes' (restitution 0.5, friction 0.5).
	const double restitution = r.Has("restitution") ? r.Real("restitution", 0.5) : 0.5;
	const double friction = r.Has("friction") ? r.Real("friction", 0.5) : 0.5;
	bool contact = true;
	if (const V* c = r.Raw("contact")) {
		if (c->GetType() != V::Boolean) return r.Bad("contact must be true or false");
		contact = c->GetBoolean();
		r.out.emplace_back("contact", V::Bool(contact));
	}
	if (!(restitution >= 0 && restitution <= 1) || !(friction >= 0)) return r.Bad("restitution must be in [0, 1] and friction nonnegative");
	// M9: the coupling surface of an analytic body from a mesh (a scene object's geometry, `surface_input`, or
	// inline `surface_vertices` / `surface_triangles`), world space at t = 0 with the body at its initial pose:
	// a cloth collider made from the same mesh (`collider_port <object>`) then has this surface's topology, so
	// rigid_surface can drive it while the body keeps its analytic shape for mass, planes and body-body contact.
	// Written back only when given (other bodies keep m6.2's effective parameters and record).
	std::vector<double> surfaceWorld;
	std::vector<std::uint32_t> surfaceTriangles;
	const bool surfaceScene = r.Has("surface_input"), surfaceInline = r.Has("surface_vertices") || r.Has("surface_triangles");
	if ((surfaceScene || surfaceInline) && s.type == Shape::Type::Mesh) return r.Bad("a mesh body's coupling surface is its own mesh");
	if (surfaceScene && surfaceInline) return r.Bad("give surface_input or surface_vertices / surface_triangles, not both");
	if (surfaceScene) {
		const std::string input = r.Text("surface_input", "-", {}, false);
		std::string hash, codec;
		std::shared_ptr<const Bytes> bytes;
		MeshFrame frame;
		if (!inputs.Identity(input, hash, error) || !inputs.Sample(input, 0.0, bytes, codec, error) || !bytes)
			return Fail(error, "body " + name + " surface input " + input + ": " + error);
		const StaticPayloadResolver resolve = [&inputs](const std::string& digest, std::size_t budget,
			std::shared_ptr<const Bytes>& fixed, std::string& why) { return inputs.ResolveStatic(digest, budget, fixed, why); };
		if ((codec != "mesh.v1" && codec != "mesh.v2") || !DecodeMesh(*bytes, CodecBudget, resolve, frame, error))
			return Fail(error, "body " + name + " surface input " + input + " is not a mesh frame: " + error);
		for (std::uint32_t bits : frame.positions) surfaceWorld.push_back(FromF32(bits));
		surfaceTriangles = frame.indices;
		if (std::find(sceneNames.begin(), sceneNames.end(), input) == sceneNames.end()) sceneNames.push_back(input);
		r.out.emplace_back("surface_input", V::String(input));
		r.out.emplace_back("surface_identity", V::String(hash));
		r.Raw("surface_identity");   // written back by Configure; accepted on input
	} else if (surfaceInline) {
		std::vector<double> idx;
		if (!Numbers(r.Raw("surface_vertices"), surfaceWorld) || surfaceWorld.size() % 3 != 0 || surfaceWorld.empty())
			return r.Bad("surface_vertices must be a flat array of 3n finite numbers");
		if (!Numbers(r.Raw("surface_triangles"), idx) || idx.size() % 3 != 0 || idx.empty())
			return r.Bad("surface_triangles must be a flat array of 3m vertex indices");
		for (double k : idx) {
			if (!(k >= 0) || std::floor(k) != k || k >= static_cast<double>(surfaceWorld.size()/3)) return r.Bad("surface triangle index out of range");
			surfaceTriangles.push_back(static_cast<std::uint32_t>(k));
		}
		V::Values vs, ts;
		for (double x : surfaceWorld) vs.push_back(V::Float(x));
		for (std::uint32_t k : surfaceTriangles) ts.push_back(V::Unsigned(k));
		r.out.emplace_back("surface_vertices", V::ArrayValue(vs));
		r.out.emplace_back("surface_triangles", V::ArrayValue(ts));
	}
	if (!r.Finish()) return false;
	MassProperties mp;
	if (!ShapeMass(s, 1.0, mp, error)) return Fail(error, "body " + name + ": " + error);
	const double rho = byMass ? mass/mp.volume : density;
	if (!(rho > 0) || !std::isfinite(rho)) return Fail(error, "body " + name + ": density and mass must be positive");
	for (double& c : mp.inertia) c *= rho;
	mp.mass = rho*mp.volume;
	// Body frame: the centroid, the principal axes (analytic shapes keep their axes).
	V3 moments{{mp.inertia[0], mp.inertia[4], mp.inertia[8]}};
	M3 axes{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
	if (s.type == Shape::Type::Mesh) PrincipalAxes(mp.inertia, moments, axes);
	if (!(moments[0] > 0) || !(moments[1] > 0) || !(moments[2] > 0)) return Fail(error, "body " + name + " has a degenerate inertia");
	Body& b = spec.initial;
	b.name = name;
	b.mass = mp.mass;
	b.moments = moments;
	const M3 Ra = QMatrix(orientation);
	const M3 Rb = Multiply(Ra, axes);
	b.q = MatrixQuaternion(Rb);
	b.x = Add(position, Apply(Ra, mp.centroid));
	b.v = velocity;
	const V3 wb = ApplyT(QMatrix(b.q), omega);
	b.pi = {{moments[0]*wb[0], moments[1]*wb[1], moments[2]*wb[2]}};
	b.sphere = s.type == Shape::Type::Sphere;
	b.radius = s.type == Shape::Type::Sphere ? s.radius : 0;
	// Surface and support points in the body frame: P^T (y - c).
	auto toBody = [&](std::vector<double>& pts) {
		for (std::size_t k = 0; k + 2 < pts.size(); k += 3) {
			const V3 y = ApplyT(axes, Sub(V3{{pts[k], pts[k + 1], pts[k + 2]}}, mp.centroid));
			pts[k] = y[0]; pts[k + 1] = y[1]; pts[k + 2] = y[2];
		}
	};
	ShapeSurface(s, spec.surface, spec.triangles);
	toBody(spec.surface);
	if (!surfaceWorld.empty()) {
		// M9: the given surface in the body frame, R_b^T (y - x_b) at the initial pose.
		const M3 Rw = QMatrix(b.q);
		spec.surface.clear();
		for (std::size_t k = 0; k + 2 < surfaceWorld.size(); k += 3) {
			const V3 y = ApplyT(Rw, Sub(V3{{surfaceWorld[k], surfaceWorld[k + 1], surfaceWorld[k + 2]}}, b.x));
			spec.surface.insert(spec.surface.end(), {y[0], y[1], y[2]});
		}
		spec.triangles = surfaceTriangles;
	}
	ShapeSupportPoints(s, b.support);
	toBody(b.support);
	spec.contact = MakeContactShape(s, restitution, friction);
	if (!contact) spec.contact.type = ContactShape::Type::None;
	spec.contactOff = !contact;
	effective = V::MapValue(r.out);
	return true;
}

// ------------------------------------------------------------------ evidence

V3 Rotate(const V3& axisAngle, const V3& p)
{
	const double a = Norm(axisAngle);
	if (a == 0) return p;
	const V3 k = Mul(axisAngle, 1/a);
	return Add(Add(Mul(p, std::cos(a)), Mul(Cross(k, p), std::sin(a))), Mul(k, Dot(k, p)*(1 - std::cos(a))));
}
M3 RotationMatrix(const V3& axisAngle)
{
	M3 m{};
	for (int c = 0; c < 3; ++c) {
		V3 e{{0, 0, 0}};
		e[c] = 1;
		const V3 col = Rotate(axisAngle, e);
		for (int r = 0; r < 3; ++r) m[3*r + c] = col[r];
	}
	return m;
}
double Frobenius(const M3& a, const M3& b)
{
	double s = 0;
	for (int k = 0; k < 9; ++k) s += (a[k] - b[k])*(a[k] - b[k]);
	return std::sqrt(s);
}

// Torque-free symmetric top (I1 = I2): R(t) = exp(t L^/I1) R0 exp(t mu e3^),
// mu = Pi3 (1/I3 - 1/I1), exactly (the two parts of H commute).
double SymmetricTopError(double h)
{
	Body b;
	b.moments = {{2, 2, 1}};
	b.pi = {{4, 0, 6}};    // omega = (2, 0, 6) rad/s
	const V3 L = b.pi;     // R0 = 1
	const double mu = b.pi[2]*(1/b.moments[2] - 1/b.moments[0]);
	const std::uint64_t steps = static_cast<std::uint64_t>(std::ceil(5.0/h));
	double worst = 0;
	for (std::uint64_t k = 1; k <= steps; ++k) {
		FreeRotation(b, h);
		const double t = static_cast<double>(k)*h;
		const M3 exact = Multiply(RotationMatrix(Mul(L, t/b.moments[0])), RotationMatrix(V3{{0, 0, mu*t}}));
		worst = std::max(worst, Frobenius(QMatrix(b.q), exact));
	}
	return worst;
}

// Asymmetric body spun about its intermediate axis with a small perturbation
// (Dzhanibekov): energy and |L| relative to their start, worst over the run;
// the run must show at least four flips of the spin axis.
double TennisRacketError(double h, int& flips)
{
	Body b;
	b.moments = {{1, 2, 3}};
	b.pi = {{0.01, 2*5.0, 0.01}};
	const double e0 = b.KineticEnergy();
	const V3 l0 = b.AngularMomentum();
	const std::uint64_t steps = static_cast<std::uint64_t>(std::ceil(60.0/h));
	double worst = 0, last = b.pi[1];
	flips = 0;
	for (std::uint64_t k = 0; k < steps; ++k) {
		FreeRotation(b, h);
		if ((b.pi[1] > 0) != (last > 0)) ++flips;
		last = b.pi[1];
		const V3 l = b.AngularMomentum();
		worst = std::max({worst, std::fabs(b.KineticEnergy() - e0)/e0, Norm(Sub(l, l0))/Norm(l0)});
	}
	return worst;
}

// A spinning box thrown under gravity: the centre of mass follows the
// parabola exactly (the scheme is exact for constant forces).
double ProjectileError(double h)
{
	Body b;
	b.mass = 2;
	b.moments = {{0.1, 0.2, 0.3}};
	b.v = {{3, 5, -1}};
	b.pi = {{0.2, 0.5, -0.3}};
	const V3 g{{0, -9.81, 0}};
	const std::uint64_t steps = static_cast<std::uint64_t>(std::ceil(1.0/h));
	double worst = 0;
	for (std::uint64_t k = 1; k <= steps; ++k) {
		StepBody(b, Load(), g, h);
		const double t = static_cast<double>(k)*h;
		const V3 exact = Add(Mul(V3{{3, 5, -1}}, t), Mul(g, 0.5*t*t));
		worst = std::max(worst, Norm(Sub(b.x, exact)));
	}
	const double range = Norm(Add(Mul(V3{{3, 5, -1}}, 1.0), Mul(g, 0.5)));
	return worst/range;
}

// A physical pendulum (a 1 m rod, 0.05 m square section) held at one end by
// a stiff, lightly damped external spring to a fixed pivot, released from
// 0.5 rad: the measured period against the exact one, 4 sqrt(Ip / (m g d))
// K(sin(theta0 / 2)).  The spring force enters as a load at the rod's end,
// evaluated at every kick.
double PendulumError(double h)
{
	MassProperties mp;
	Shape rod;
	rod.size = {{0.05, 1.0, 0.05}};
	std::string ignored;
	ShapeMass(rod, 1000, mp, ignored);
	Body b;
	b.mass = mp.mass;
	b.moments = {{mp.inertia[0], mp.inertia[4], mp.inertia[8]}};
	const double theta0 = 0.5, d = 0.5, g = 9.81;
	const V3 pivot{{0, 0, 0}}, attach{{0, 0.5, 0}};
	b.q = QAxis(2, theta0);
	b.x = Apply(QMatrix(b.q), V3{{0, -0.5, 0}});
	const double ws = std::min(0.4/h, 400.0), k = b.mass*ws*ws, c = 2*0.2*b.mass*ws;
	auto load = [&]() {
		const M3 R = QMatrix(b.q);
		const V3 r = Apply(R, attach);
		const V3 p = Add(b.x, r);
		const V3 vp = Add(b.v, Cross(b.AngularVelocity(), r));
		Load l;
		l.force = Sub(Mul(Sub(p, pivot), -k), Mul(vp, c));
		l.point = p;
		l.atCentre = false;
		return l;
	};
	const V3 gravity{{0, -g, 0}};
	const double ip = b.moments[2] + b.mass*d*d;
	// K(m) by the arithmetic-geometric mean.
	const double kk = std::sin(theta0/2);
	double a0 = 1, g0 = std::sqrt(1 - kk*kk);
	for (int i = 0; i < 30; ++i) { const double an = 0.5*(a0 + g0); g0 = std::sqrt(a0*g0); a0 = an; }
	const double K = Pi/(2*a0);
	const double exact = 4*std::sqrt(ip/(b.mass*g*d))*K;
	// Crossings of the centre of mass through x = 0 going in the -x direction.
	std::vector<double> crossings;
	double t = 0, lastX = b.x[0];
	const std::uint64_t steps = static_cast<std::uint64_t>(std::ceil(3.5*exact/h));
	for (std::uint64_t s = 0; s < steps; ++s) {
		KickBody(b, load(), gravity, 0.5*h);
		DriftBody(b, h);
		KickBody(b, load(), gravity, 0.5*h);
		t += h;
		if (lastX > 0 && b.x[0] <= 0) crossings.push_back(t - h*b.x[0]/(b.x[0] - lastX));
		lastX = b.x[0];
	}
	if (crossings.size() < 3) return 1.0;
	const double measured = (crossings.back() - crossings.front())/static_cast<double>(crossings.size() - 1);
	return std::fabs(measured - exact)/exact;
}

struct CaseSpec { const char* id; const char* description; double tolerance; };
// Tolerances: about twice the error observed at the default substep (30 Hz x
// 8 = 1/240 s), rounded up (docs/SIMULATION_M6_RESULTS.md); a coarser
// substep can fail them and then publishes preview.
const CaseSpec Cases[] = {
	{"symmetric-top", "torque-free symmetric top, 5 s: max |R - R_exact|_F against the closed form", 7e-4},
	{"tennis-racket", "asymmetric body near its intermediate axis, 60 s (>= 4 flips): max relative change of energy and |L|", 7e-6},
	{"projectile", "spinning box under gravity, 1 s: max position error / range against the parabola", 1e-12},
	{"pendulum", "physical pendulum on a stiff external spring, 0.5 rad: relative period error against the exact period", 1.6e-3}};

std::vector<EvidenceCase> ComputeEvidence(double h)
{
	static std::mutex mutex;
	static std::map<double, std::vector<EvidenceCase>> memo;
	{
		std::lock_guard<std::mutex> hold(mutex);
		const auto it = memo.find(h);
		if (it != memo.end()) return it->second;
	}
	std::vector<EvidenceCase> cases;
	for (const CaseSpec& spec : Cases) {
		EvidenceCase c;
		c.id = spec.id;
		c.description = spec.description;
		c.tolerance = spec.tolerance;
		const std::string id = spec.id;
		int flips = 0;
		c.error = id == "symmetric-top" ? SymmetricTopError(h) : id == "tennis-racket" ? TennisRacketError(h, flips) :
			id == "projectile" ? ProjectileError(h) : PendulumError(h);
		if (id == "tennis-racket" && flips < 4) c.error = 1;
		c.passed = std::isfinite(c.error) && c.error <= c.tolerance;
		const V configuration = V::MapValue({{"solver", V::String(RigidSolverId)}, {"version", V::String(SolverVersion)},
			{"substep", V::Float(h)}, {"integrator", V::String("kick-drift-kick, principal-axis splitting")}});
		c.digest = EvidenceDigest(c.id, configuration, c.error, c.tolerance, c.passed);
		cases.push_back(c);
	}
	std::lock_guard<std::mutex> hold(mutex);
	memo[h] = cases;
	return cases;
}

// ------------------------------------------------------------------ solver

struct RigidState {
	double t = 0, segStart = 0, segTarget = 0, heldFor = 0;
	std::uint64_t next = 0, segN = 0, segJ = 0;
	bool finished = false, held = false;
	std::vector<Body> bodies;
	std::vector<Load> loads;
};
struct RigidToken final : SolverStateToken {
	const void* owner = nullptr;
	RigidState state;
};

class RigidSolver final : public ISimulationSolver {
public:
	bool Configure(const V& parameters, const ISceneInputs& inputs, SolverDescription& d, std::string& error) override;
	bool Start(std::string& error) override;
	bool Restore(const Bytes& state, std::string& error) override;
	bool Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
		AdvanceResult& result, std::string& error) override;
	bool Checkpoint(Bytes& state, std::string& error) override;
	SolverPosition Position() const override { return {s_.t, static_cast<std::int64_t>(s_.next), s_.finished}; }
	void RequestStop() override { stop_ = true; }
	bool SaveState(std::shared_ptr<const SolverStateToken>& token, std::string& error) override;
	bool RestoreState(const SolverStateToken& token, std::string& error) override;

private:
	double OutputTime(std::uint64_t i) const { return static_cast<double>(i)*interval_; }
	bool Done(AdvanceResult& r, AdvanceResult::Status st) {
		r.status = s_.finished ? AdvanceResult::Status::Finished : st;
		r.position = Position();
		return true;
	}
	bool Encode(double time, std::shared_ptr<const Bytes>& payload, std::string& error) const;
	bool OfferMassProperties(IPortExchange* ports, std::string& error) const;   // M7 prerequisites
	bool OfferContact(IPortExchange* ports, std::string& error) const;          // M7h
	std::string contactDomain_;
	bool AcceptLoads(double time, IPortExchange* ports, std::string& error);
	void Substep(double h);

	bool configured_ = false, started_ = false;
	std::vector<BodySpec> specs_;
	std::vector<Plane> planes_;
	std::vector<ContactShape> shapes_;          // M7h: per body, its body-body contact geometry
	bool pairs_ = false;                        // M7h: two or more bodies take body-body contact
	V3 gravity_{{0, -9.81, 0}};
	std::uint64_t count_ = 121, substeps_ = 8;
	double interval_ = 1.0/30;
	std::string domain_;
	RigidState s_;
	std::atomic<bool> stop_{false};
};

bool RigidSolver::Configure(const V& parameters, const ISceneInputs& inputs, SolverDescription& d, std::string& error)
{
	configured_ = started_ = false;
	Reader p(parameters, "", error);
	const std::uint64_t frames = p.Count("frames", 121, 1, 1000000);
	const double rate = p.Real("frame_rate", 30.0);
	const std::uint64_t substeps = p.Count("substeps", 8, 1, 100000);
	const V3 gravity = p.Vector<3>("gravity", {{0, -9.81, 0}});
	std::vector<std::string> sceneNames;
	std::vector<BodySpec> specs;
	V::Values bodiesOut;
	const V* bodies = p.Raw("bodies");
	if (!bodies || bodies->GetType() != V::Array || bodies->GetArray().empty())
		return p.Bad("bodies must be a nonempty array");
	for (const V& item : bodies->GetArray()) {
		BodySpec spec;
		V eff;
		if (!ParseBody(item, inputs, sceneNames, spec, eff, error)) return false;
		specs.push_back(std::move(spec));
		bodiesOut.push_back(eff);
	}
	// Bodies in name order (the transform.v1 order); names unique.
	std::vector<std::size_t> order(specs.size());
	for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return specs[a].initial.name < specs[b].initial.name; });
	std::vector<BodySpec> sorted;
	V::Values sortedOut;
	for (std::size_t i : order) { sorted.push_back(specs[i]); sortedOut.push_back(bodiesOut[i]); }
	for (std::size_t i = 1; i < sorted.size(); ++i)
		if (sorted[i].initial.name == sorted[i - 1].initial.name) return Fail(error, "body names must be unique: " + sorted[i].initial.name);
	p.out.emplace_back("bodies", V::ArrayValue(sortedOut));
	std::vector<Plane> planes;
	V::Values planesOut;
	if (const V* pl = p.Raw("planes")) {
		if (pl->GetType() != V::Array) return p.Bad("planes must be an array");
		for (const V& item : pl->GetArray()) {
			Reader r(item, "plane", error);
			Plane plane;
			plane.point = r.Vector<3>("point", {{0, 0, 0}});
			V3 n = r.Vector<3>("normal", {{0, 1, 0}}, false);
			const double l = Norm(n);
			if (!(l > 0)) return r.Bad("plane normal must be nonzero");
			plane.normal = Mul(n, 1/l);
			r.out.emplace_back("normal", Vec(plane.normal));
			plane.restitution = r.Real("restitution", 0.5);
			plane.friction = r.Real("friction", 0.5);
			if (!(plane.restitution >= 0 && plane.restitution <= 1) || !(plane.friction >= 0))
				return r.Bad("restitution must be in [0, 1] and friction nonnegative");
			if (!r.Finish()) return false;
			planes.push_back(plane);
			planesOut.push_back(V::MapValue(r.out));
		}
	}
	p.out.emplace_back("planes", V::ArrayValue(planesOut));
	SolverDescription candidate;
	if (!p.Finish()) return false;
	if (!(rate > 0)) return Fail(error, "frame_rate must be positive");
	candidate.effectiveParameters = V::MapValue(p.out);
	// M7h: body-body contact for the analytic shapes.  A mesh body takes none: there is no
	// mesh distance in this solver, so a mesh body among others is refused unless it says `contact false`.
	std::size_t contactBodies = 0;
	for (const BodySpec& b : sorted) if (b.contact.type != ContactShape::Type::None) ++contactBodies;
	for (const BodySpec& b : sorted)
		if (b.shape.type == Shape::Type::Mesh && sorted.size() > 1 && !b.contactOff) {
			return Fail(error, "body " + b.initial.name + " is a mesh: body-body contact covers spheres, boxes and cylinders only (no mesh "
				"distance in rigid.bodies); give the mesh body `contact false` (it then passes through the other bodies) or use an analytic shape");
		}
	specs_ = std::move(sorted);
	planes_ = planes;
	shapes_.clear();
	for (const BodySpec& b : specs_) shapes_.push_back(b.contact);
	pairs_ = contactBodies > 1;
	gravity_ = gravity;
	count_ = frames; substeps_ = substeps; interval_ = 1/rate;
	std::vector<std::string> names;
	for (const auto& s : specs_) names.push_back(s.initial.name);
	domain_ = TransformBodyDomain(names);
	{
		Bytes encoded;
		RISECBOR64::Encode(V::ArrayValue({V::String("rigid.body_contact.v1"), V::String(domain_), V::Unsigned(planes_.size())}), encoded, nullptr);
		contactDomain_ = RISECBOR64::SHA256Hex(encoded);
	}
	configured_ = true;

	candidate.solverId = RigidSolverId;
	candidate.version = SolverVersion;
	candidate.cache.firstIndex = 0;
	candidate.cache.count = count_;
	candidate.cache.startTime = 0;
	candidate.cache.interval = interval_;
	candidate.cache.outputs = {{Output, TransformCodec, {{"angular_velocity", "rad/s", "f64x3"},
		{"orientation", "1", "f64x4"}, {"position", "m", "f64x3"}, {"velocity", "m/s", "f64x3"}}}};
	candidate.inputs = sceneNames;
	// Per body: mass, moments and the surface in the body frame.
	{
		V::Values records;
		for (const auto& s : specs_) {
			V::Values vs, ts;
			for (double x : s.surface) vs.push_back(V::Float(x));
			for (std::uint32_t k : s.triangles) ts.push_back(V::Unsigned(k));
			records.push_back(V::MapValue({{"name", V::String(s.initial.name)}, {"shape", V::String(ShapeName(s.shape.type))},
				{"mass", V::Float(s.initial.mass)}, {"moments", Vec(s.initial.moments)},
				{"surface_vertices", V::ArrayValue(vs)}, {"surface_triangles", V::ArrayValue(ts)}}));
		}
		Bytes records2;
		if (!RISECBOR64::Encode(V::MapValue({{"format", V::String(RigidBodiesRecordFormat)}, {"bodies", V::ArrayValue(records)},
			{"units", V::MapValue({{"mass", V::String("kg")}, {"moments", V::String("kg m^2")}, {"surface", V::String("m")}})}}),
			records2, &error)) return false;
		candidate.cache.physicalRecords = records2;
	}
	candidate.ports = {
		{StatePort, "rigid body state", "m", TransformCodec, true, Output,
			{{"angular_velocity", "angular velocity", "rad/s", 3}, {"orientation", "orientation", "1", 4},
			 {"position", "position", "m", 3}, {"velocity", "velocity", "m/s", 3}}, domain_},
		{LoadPort, "body load", "N", "field.v1", false, "",
			{{"force", "force", "N", 3}, {"point", "position", "m", 3}, {"torque", "torque", "N m", 3}}, domain_},
		// M7 prerequisites (additive): offered once, at the start time; constant.
		{MassPort, "rigid body mass properties", "kg", "field.v1", true, "",
			{{"center_of_mass", "position", "m", 3}, {"external_force", "force", "N", 3},
			 {"inertia", "moment of inertia", "kg m^2", 9}, {"mass", "mass", "kg", 1}}, domain_},
		// M7h (additive): offered once, at the start time; constant.
		{ContactPort, "body contact", "1", "field.v1", true, "",
			{{"contact", "flag", "1", 1}, {"friction", "friction", "1", 1}, {"kind", "index", "1", 1}, {"normal", "normal", "1", 3},
			 {"point", "position", "m", 3}, {"restitution", "restitution", "1", 1}}, contactDomain_}};
	auto& caps = candidate.capabilities;
	caps.backends = {{"cpu", BackendStatus::Supported, true, "portable C++17, single-threaded; built and tested on macOS (with and without Metal), "
		"Windows and Android builds not yet compiled"}};
	caps.interactive = caps.deterministic = caps.resumable = caps.externalMacroStep = caps.rollback = true;
	caps.threadCountInvariant = true;
	caps.modes = {"bake"};
	caps.backend = "cpu";
	caps.memoryEstimate = (std::uint64_t(1) << 20) + specs_.size()*4096;
	caps.costEstimate = 1e-4*static_cast<double>(specs_.size())*static_cast<double>(substeps_)*rate;
	const bool contact = !planes_.empty() || pairs_;
	candidate.evidence.fidelity = contact ? "preview" : "predictive";
	caps.earnableFidelity = contact ? std::vector<std::string>{"preview"} : std::vector<std::string>{"preview", "predictive"};
	if (contact) caps.fidelityLimit = pairs_ ? "plane and body-body contact are impulse models with restitution and Coulomb friction, not validated" :
		"plane contact is an impulse model with restitution and Coulomb friction, not validated";
	{
		std::vector<std::string> shapes;
		for (const auto& s : specs_) shapes.push_back(ShapeName(s.shape.type));
		std::sort(shapes.begin(), shapes.end());
		shapes.erase(std::unique(shapes.begin(), shapes.end()), shapes.end());
		std::vector<std::string> kinds;
		if (!planes_.empty()) kinds.push_back("plane-impulse");
		if (pairs_) kinds.push_back("body-impulse");
		if (kinds.empty()) kinds.push_back("none");
		std::sort(kinds.begin(), kinds.end());
		candidate.evidence.conditions = {{"contact", kinds},
			{"integrator", {"kick-drift-kick-splitting"}}, {"shape", shapes}};
		candidate.evidence.scope = "Free rigid bodies (no joints, no body-body contact) integrated by kick-drift-kick with "
			"the symplectic principal-axis splitting at the configured substep, under gravity and loads held over each "
			"macro step: certified on canonical problems (the closed-form symmetric top, the invariants of an asymmetric "
			"body over many flips, projectile motion, a physical pendulum); it does not bound the error of a given bake "
			"and does not cover plane contact, joints or body-body contact.";
	}
	candidate.evidence.cases = ComputeEvidence(interval_/static_cast<double>(substeps_));
	if (!planes_.empty()) candidate.notes.push_back("plane contact: sequential impulses with restitution and Coulomb friction (preview)");
	if (pairs_) candidate.notes.push_back("body-body contact (spheres, boxes, cylinders; m6.2): sequential impulses at sampled points (box corners and "
		"edge midpoints, cylinder rims, sphere centres), restitution min(e_a, e_b), friction sqrt(mu_a mu_b) (preview; edge-edge contact between "
		"the samples is not detected)");
	d = std::move(candidate);
	return true;
}

bool RigidSolver::Start(std::string& error)
{
	if (!configured_) return Fail(error, "configure first");
	s_ = RigidState();
	for (const auto& spec : specs_) s_.bodies.push_back(spec.initial);
	s_.finished = count_ == 0;
	started_ = true;
	return true;
}

// body_mass_properties: field.v1 over the body set; per body the centre of
// mass in the body frame (the body frame's origin: zero), the constant
// external force this solver applies besides body_load (m g; M7b), the inertia
// tensor in the body frame (the principal axes: diagonal, row-major 3x3) and
// the mass.
bool RigidSolver::OfferMassProperties(IPortExchange* ports, std::string& error) const
{
	FieldFrame f;
	f.domain = domain_;
	f.count = static_cast<std::uint32_t>(s_.bodies.size());
	FieldChannel com{"center_of_mass", "m", 3, {}}, external{"external_force", "N", 3, {}}, inertia{"inertia", "kg m^2", 9, {}},
		mass{"mass", "kg", 1, {}};
	const auto bits = [](double x) { const float v = static_cast<float>(x); std::uint32_t b; std::memcpy(&b, &v, 4); return b; };
	for (const Body& b : s_.bodies) {
		for (int k = 0; k < 3; ++k) com.values.push_back(bits(0));
		// M7b (additive): the constant load this solver applies besides body_load, m g.
		for (int k = 0; k < 3; ++k) external.values.push_back(bits(b.mass*gravity_[std::size_t(k)]));
		for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) inertia.values.push_back(bits(r == c ? b.moments[r] : 0.0));
		mass.values.push_back(bits(b.mass));
	}
	f.channels = {com, external, inertia, mass};
	auto bytes = std::make_shared<Bytes>();
	if (!EncodeField(f, CodecBudget, *bytes, error)) return false;
	return ports->Offer(MassPort, 0, bytes, error) && OfferContact(ports, error);
}

bool RigidSolver::OfferContact(IPortExchange* ports, std::string& error) const
{
	FieldFrame f;
	f.domain = contactDomain_;
	f.count = static_cast<std::uint32_t>(planes_.size() + s_.bodies.size());
	FieldChannel on{"contact", "1", 1, {}}, mu{"friction", "1", 1, {}}, kind{"kind", "1", 1, {}}, normal{"normal", "1", 3, {}},
		point{"point", "m", 3, {}}, e{"restitution", "1", 1, {}};
	const auto bits = [](double x) { const float v = static_cast<float>(x); std::uint32_t b; std::memcpy(&b, &v, 4); return b; };
	for (const Plane& p : planes_) {
		on.values.push_back(bits(1));
		mu.values.push_back(bits(p.friction));
		kind.values.push_back(bits(0));
		for (int k = 0; k < 3; ++k) { normal.values.push_back(bits(p.normal[std::size_t(k)])); point.values.push_back(bits(p.point[std::size_t(k)])); }
		e.values.push_back(bits(p.restitution));
	}
	for (std::size_t i = 0; i < s_.bodies.size(); ++i) {
		const ContactShape& c = shapes_[i];
		on.values.push_back(bits(c.type == ContactShape::Type::None ? 0 : 1));
		mu.values.push_back(bits(c.friction));
		kind.values.push_back(bits(1));
		for (int k = 0; k < 3; ++k) { normal.values.push_back(bits(0)); point.values.push_back(bits(0)); }
		e.values.push_back(bits(c.restitution));
	}
	f.channels = {on, mu, kind, normal, point, e};
	auto bytes = std::make_shared<Bytes>();
	if (!EncodeField(f, CodecBudget, *bytes, error)) return false;
	return ports->Offer(ContactPort, 0, bytes, error);
}

bool RigidSolver::Encode(double time, std::shared_ptr<const Bytes>& payload, std::string& error) const
{
	TransformFrame f;
	f.time = time;
	for (const Body& b : s_.bodies) {
		TransformBody tb;
		tb.name = b.name;
		tb.position = b.x;
		Quat q = b.q;
		CanonicalQuaternion(q);
		tb.orientation = q;
		tb.velocity = b.v;
		tb.angularVelocity = b.AngularVelocity();
		f.bodies.push_back(tb);
	}
	auto bytes = std::make_shared<Bytes>();
	if (!EncodeTransforms(f, CodecBudget, *bytes, error)) return false;
	payload = std::move(bytes);
	return true;
}

// body_load: field.v1 over the body set; channels force (N, 3), point (m,
// 3), torque (N m, 3), each optional; absent: zero (no point: the centre).
bool RigidSolver::AcceptLoads(double time, IPortExchange* ports, std::string& error)
{
	s_.loads.assign(s_.bodies.size(), Load());
	if (!ports) return true;
	std::shared_ptr<const Bytes> bytes;
	const auto status = ports->Accept(LoadPort, time, bytes, error);
	if (status == PortStatus::Unbound || status == PortStatus::Unavailable) return true;
	if (status != PortStatus::Ready) return false;
	FieldFrame f;
	if (!DecodeField(*bytes, CodecBudget, f, error)) return false;
	if (f.domain != domain_ || f.count != s_.bodies.size())
		return Fail(error, "body_load must be a field.v1 over the body set (domain: TransformBodyDomain of the names)");
	for (const auto& c : f.channels) {
		const bool force = c.name == "force" && c.units == "N", point = c.name == "point" && c.units == "m",
			torque = c.name == "torque" && c.units == "N m";
		if ((!force && !point && !torque) || c.components != 3)
			return Fail(error, "body_load channels are force (N), point (m) and torque (N m), 3 components each");
		for (std::size_t i = 0; i < s_.bodies.size(); ++i) {
			const V3 x{{FromF32(c.values[3*i]), FromF32(c.values[3*i + 1]), FromF32(c.values[3*i + 2])}};
			if (force) s_.loads[i].force = x;
			else if (torque) s_.loads[i].torque = x;
			else { s_.loads[i].point = x; s_.loads[i].atCentre = false; }
		}
	}
	return true;
}

void RigidSolver::Substep(double h)
{
	for (std::size_t i = 0; i < s_.bodies.size(); ++i) {
		const Load load = i < s_.loads.size() ? s_.loads[i] : Load();
		StepBody(s_.bodies[i], load, gravity_, h);
		if (!planes_.empty()) ResolvePlanes(s_.bodies[i], planes_, gravity_, h);
	}
	// M7h: body-body contact (nothing happens, bit for bit, while no two bounding spheres overlap).
	if (pairs_ && PairsNear(s_.bodies, shapes_)) ResolvePairs(s_.bodies, shapes_, planes_, gravity_, h);
}

bool RigidSolver::Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
	AdvanceResult& result, std::string& error)
{
	result = AdvanceResult{};
	if (!started_) return Fail(error, "start or restore first");
	const bool macro = request.mode == AdvanceRequest::Mode::MacroStep;
	if (macro && (!std::isfinite(request.macroEnd) || request.macroEnd < s_.t)) return Fail(error, "macro end lies in the past");
	const double end = static_cast<double>(count_ ? count_ - 1 : 0)*interval_;
	if (!macro) s_.held = false;
	bool offered = false;
	for (;;) {
		while (!s_.finished && s_.t == OutputTime(s_.next)) {
			std::shared_ptr<const Bytes> payload;
			if (!Encode(s_.t, payload, error) || !sink.Publish(static_cast<std::int64_t>(s_.next), Output, s_.t, payload, {}, error))
				return false;
			if (ports && !ports->Offer(StatePort, s_.t, payload, error)) return false;
			if (ports && s_.t == 0 && !OfferMassProperties(ports, error)) return false;
			offered = true;
			++result.framesPublished;
			if (++s_.next == count_) s_.finished = true;
			if (!macro) return Done(result, AdvanceResult::Status::Advanced);
		}
		if (s_.finished) return Done(result, AdvanceResult::Status::Finished);
		if (macro && s_.t == request.macroEnd && s_.segN == 0) {
			if (!offered && ports) {
				std::shared_ptr<const Bytes> payload;
				if (!Encode(s_.t, payload, error) || !ports->Offer(StatePort, s_.t, payload, error)) return false;
				if (s_.t == 0 && !OfferMassProperties(ports, error)) return false;
			}
			return Done(result, AdvanceResult::Status::Advanced);
		}
		if (s_.segN == 0) {
			s_.segStart = s_.t;
			s_.segTarget = macro ? std::min(OutputTime(s_.next), request.macroEnd) : OutputTime(s_.next);
			const double nominal = interval_/static_cast<double>(substeps_);
			s_.segN = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil((s_.segTarget - s_.segStart)/nominal*(1 - 1e-12))));
			s_.segJ = 0;
			if (!macro || !s_.held || s_.heldFor != request.macroEnd) {
				if (!AcceptLoads(s_.segStart, ports, error)) return false;
				s_.held = macro;
				s_.heldFor = request.macroEnd;
			}
		} else if (macro && s_.segTarget > request.macroEnd) return Fail(error, "macro end inside an interrupted segment");
		offered = false;
		while (s_.segJ < s_.segN) {
			if (stop_.load()) {
				stop_ = false;
				return Done(result, AdvanceResult::Status::Stopped);
			}
			if (progress && !progress->Report(s_.t, end, std::string())) return Done(result, AdvanceResult::Status::Stopped);
			const double tn = s_.segJ + 1 == s_.segN ? s_.segTarget :
				s_.segStart + (s_.segTarget - s_.segStart)*static_cast<double>(s_.segJ + 1)/static_cast<double>(s_.segN);
			Substep(tn - s_.t);
			s_.t = tn;
			++s_.segJ;
		}
		s_.segN = s_.segJ = 0;
	}
}

std::vector<double> PackBodies(const std::vector<Body>& bodies)
{
	std::vector<double> v;
	for (const Body& b : bodies) {
		v.insert(v.end(), b.x.begin(), b.x.end());
		v.insert(v.end(), b.q.begin(), b.q.end());
		v.insert(v.end(), b.v.begin(), b.v.end());
		v.insert(v.end(), b.pi.begin(), b.pi.end());
	}
	return v;
}
std::vector<double> PackLoads(const std::vector<Load>& loads)
{
	std::vector<double> v;
	for (const Load& l : loads) {
		v.insert(v.end(), l.force.begin(), l.force.end());
		v.insert(v.end(), l.point.begin(), l.point.end());
		v.insert(v.end(), l.torque.begin(), l.torque.end());
		v.push_back(l.atCentre ? 1 : 0);
	}
	return v;
}

bool RigidSolver::Checkpoint(Bytes& state, std::string& error)
{
	if (!started_) return Fail(error, "nothing to checkpoint");
	V::Members m{{"format", V::String(StateFormat)}, {"t", V::Float(s_.t)}, {"next", V::Unsigned(s_.next)},
		{"finished", V::Bool(s_.finished)}, {"segStart", V::Float(s_.segStart)}, {"segTarget", V::Float(s_.segTarget)},
		{"segN", V::Unsigned(s_.segN)}, {"segJ", V::Unsigned(s_.segJ)}, {"held", V::Bool(s_.held)},
		{"heldFor", V::Float(s_.heldFor)}, {"bodies", V::BytesValue(PackDoubles(PackBodies(s_.bodies)))},
		{"loads", V::BytesValue(PackDoubles(PackLoads(s_.loads)))}};
	return RISECBOR64::Encode(V::MapValue(m), state, &error);
}

bool RigidSolver::Restore(const Bytes& state, std::string& error)
{
	if (!configured_) return Fail(error, "configure before restore");
	V v;
	if (!CanDecodeCanonicalRecord(state, CodecBudget) || !RISECBOR64::DecodeCanonical(state, v, &error) || v.GetType() != V::Map)
		return Fail(error, "invalid rigid solver state");
	auto real = [&](const char* k, double& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::Float64) return false; x = f->GetFloat(); return true; };
	auto count = [&](const char* k, std::uint64_t& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::UnsignedInteger) return false; x = f->GetIntegerArgument(); return true; };
	auto flag = [&](const char* k, bool& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::Boolean) return false; x = f->GetBoolean(); return true; };
	const V* format = v.Find("format");
	RigidState c;
	if (!format || format->GetType() != V::Text || format->GetText() != StateFormat || !real("t", c.t) || !count("next", c.next) ||
		!flag("finished", c.finished) || !real("segStart", c.segStart) || !real("segTarget", c.segTarget) ||
		!count("segN", c.segN) || !count("segJ", c.segJ) || !flag("held", c.held) || !real("heldFor", c.heldFor) ||
		c.next > count_ || c.segJ > c.segN)
		return Fail(error, "invalid rigid solver state fields");
	std::vector<double> bodies, loads;
	const std::size_t n = specs_.size();
	if (!UnpackDoubles(v.Find("bodies"), 13*n, bodies, false) || !UnpackDoubles(v.Find("loads"), 10*n, loads, true))
		return Fail(error, "rigid solver state does not match the configured bodies");
	for (std::size_t i = 0; i < n; ++i) {
		Body b = specs_[i].initial;
		const double* p = &bodies[13*i];
		b.x = {{p[0], p[1], p[2]}}; b.q = {{p[3], p[4], p[5], p[6]}}; b.v = {{p[7], p[8], p[9]}}; b.pi = {{p[10], p[11], p[12]}};
		c.bodies.push_back(b);
		if (!loads.empty()) {
			const double* l = &loads[10*i];
			Load ld;
			ld.force = {{l[0], l[1], l[2]}}; ld.point = {{l[3], l[4], l[5]}}; ld.torque = {{l[6], l[7], l[8]}}; ld.atCentre = l[9] != 0;
			c.loads.push_back(ld);
		}
	}
	s_ = std::move(c);
	started_ = true;
	return true;
}

bool RigidSolver::SaveState(std::shared_ptr<const SolverStateToken>& token, std::string& error)
{
	if (!started_) return Fail(error, "nothing to save");
	auto t = std::make_shared<RigidToken>();
	t->owner = this;
	t->state = s_;
	token = std::move(t);
	return true;
}

bool RigidSolver::RestoreState(const SolverStateToken& token, std::string& error)
{
	const auto* t = dynamic_cast<const RigidToken*>(&token);
	if (!t || t->owner != this || !configured_) return Fail(error, "state token belongs to another solver");
	s_ = t->state;
	started_ = true;
	return true;
}
} // namespace

bool RegisterRigidSolver(std::string& error)
{
	static std::mutex once;
	static bool registered = false;
	std::lock_guard<std::mutex> hold(once);
	if (registered) return true;
	if (!RegisterSolver(RigidSolverId, []{ return std::unique_ptr<ISimulationSolver>(new RigidSolver()); }, error)) return false;
	registered = true;
	return true;
}

bool RigidBodySurface(const Bytes& records, const std::string& body, std::vector<double>& vertices,
	std::vector<std::uint32_t>& triangles, std::string& error)
{
	V v;
	if (records.empty() || !RISECBOR64::DecodeCanonical(records, v, &error) || v.GetType() != V::Map)
		return Fail(error, "no rigid body record");
	const V* format = v.Find("format");
	const V* list = v.Find("bodies");
	if (!format || format->GetType() != V::Text || format->GetText() != RigidBodiesRecordFormat || !list ||
		list->GetType() != V::Array) return Fail(error, "not a rigid.bodies.v1 record");
	for (const V& b : list->GetArray()) {
		const V* name = b.Find("name");
		if (!name || name->GetType() != V::Text || name->GetText() != body) continue;
		std::vector<double> idx;
		if (!Numbers(b.Find("surface_vertices"), vertices) || !Numbers(b.Find("surface_triangles"), idx))
			return Fail(error, "malformed rigid body surface record");
		triangles.clear();
		for (double k : idx) triangles.push_back(static_cast<std::uint32_t>(k));
		return true;
	}
	return Fail(error, "no body named " + body + " in the rigid body record");
}

} }
