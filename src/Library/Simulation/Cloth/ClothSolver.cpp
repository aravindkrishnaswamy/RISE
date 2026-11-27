//////////////////////////////////////////////////////////////////////
//
//  ClothSolver.cpp - "cloth.shell" on the solver contract (simulation M3a)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClothSolver.h"
#include "ClothValidation.h"
#include "ClothContact.h"
#include "../SimulationField.h"
#include "../SimulationMesh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <future>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace RISE { namespace Simulation {

namespace {
using V = RISECBOR64::Value;
using namespace Cloth;

constexpr std::size_t CodecBudget = std::size_t(256) << 20;
// m3a.3: mesh.v2 output (static topology with material UVs), field.v1
// surface_load, held port values per macro step in the state.
// m3a.4: results with mesh colliders, moving pins with barrier contact or
// self-collision change (edge-edge mollifier and exact parallel test, kinematic
// continuation for moving pins and colliders, cloth-cloth Hessian couplings,
// `self_friction`); runs without contact, or with analytic shapes and static
// pins only, give the same frames as m3a.3.
// m3a.5 (M7h): inside a macro step with a partner's steps (liquid.mac's implicit
// sheets) the linear model is corotational (ClothModel.h BeginLinear), and
// surface_elements is offered; every other run gives the frames of m3a.4.
// m3a.6 (M7i): with a partner's steps the cloth continues from the binary32 state it offers and
// fits the corotational rotations to binary32-rounded positions (the partner's copy does the same);
// every other run gives the frames of m3a.5.
// m3a.7 (M7j): with a partner's steps the corotational elements' blocks are balanced (exactly
// invariant to an element's translation; the partner's copy does the same): a free sheet's elastic
// forces sum to zero; every other run gives the frames of m3a.6.
constexpr const char* SolverVersion = "m3a.7";
constexpr const char* StateFormat = "cloth.shell.state.v2";
constexpr const char* LoadPort = "surface_load";
constexpr const char* StatePort = "surface_state";
// M7e (coupling): the linearised response a partner needs to solve the cloth's
// motion implicitly (liquid.mac's coupled sheets): per vertex its lumped mass,
// pinned flag, the non-hydrodynamic force and the Rayleigh coefficients; and
// the projected elastic Hessian as its upper block triangle.  Offered with
// surface_state (never changes results; nothing reads them unless connected).
constexpr const char* MechanicsPort = "surface_mechanics";
constexpr const char* StiffnessPort = "surface_stiffness";
// M7h: the corotational model's elements about the offered state (field.v1 over
// the (element, a, b) blocks with a <= b of each element's local Hessian, element
// order membranes, hinges, rods: the element index, its kind (0 membrane, 1
// hinge, 2 rod), the global vertices of a and b, the 3x3 block of K_e, and on the
// diagonal items (a = b) the element's gradient at vertex a).  A partner's copy
// of the cloth turns each element's response with the element (CorotatedElements
// in ClothLinearAlgebra.h), as the cloth itself does inside the macro step.
constexpr const char* ElementsPort = "surface_elements";
// M7e: the partner's own steps over the held macro step (field.v1 over (step,
// vertex) pairs, step-major: the impulse on each vertex in each step and the
// step's segment ordinal within the macro step).  When it is offered the cloth
// takes the same steps (equal steps per segment, as the partner splits them),
// by backward Euler with each step's impulse as its force: the same discrete
// dynamics as the partner's linearised copy of the cloth (liquid.mac's
// implicit sheets).
constexpr const char* StepsPort = "surface_load_steps";
// M6 (coupling): a mesh collider with motion {type: port, port: <name>} follows
// the mesh.v2 surface accepted on <name>; the contact reaction on every such
// collider is offered on ReactionPort.
constexpr const char* ReactionPort = "collider_reaction";
constexpr const char* DefaultColliderPort = "collider_surface";

bool Fail(std::string& error, const std::string& message) { error = message; return false; }

bool Number(const V& v, double& x)
{
	if (v.GetType() == V::Float64) x = v.GetFloat();
	else if (v.GetType() == V::UnsignedInteger) x = static_cast<double>(v.GetIntegerArgument());
	else if (v.GetType() == V::NegativeInteger) x = -1.0 - static_cast<double>(v.GetIntegerArgument());
	else return false;
	return std::isfinite(x);
}

V Vec(const V3& a) { return V::ArrayValue({V::Float(a[0]), V::Float(a[1]), V::Float(a[2])}); }

double Length(const V3& a) { return std::sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); }

// Strict map reader: unknown keys are refused; read keys are written back
// with canonical types into `out` (unless the caller writes them itself).
class Reader {
public:
	Reader(const V& map, std::string where, std::string& error) : map_(map), where_(std::move(where)), error_(error) {
		if (map.GetType() != V::Map) Bad("must be a map");
	}
	bool Ok() const { return ok_; }
	double Real(const char* key, double fallback, bool effective = true) {
		double x = fallback;
		if (const V* v = Use(key)) if (!Number(*v, x)) Bad(std::string(key) + " must be a finite number");
		if (effective) out.emplace_back(key, V::Float(x));
		return x;
	}
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
	bool Bool(const char* key, bool fallback) {
		bool b = fallback;
		if (const V* v = Use(key)) {
			if (v->GetType() == V::Boolean) b = v->GetBoolean();
			else Bad(std::string(key) + " must be true or false");
		}
		out.emplace_back(key, V::Bool(b));
		return b;
	}
	V3 Vector(const char* key, const V3& fallback, bool effective = true) {
		V3 a = fallback;
		if (const V* v = Use(key)) {
			bool good = v->GetType() == V::Array && v->GetArray().size() == 3;
			for (std::size_t k = 0; good && k < 3; ++k) good = Number(v->GetArray()[k], a[k]);
			if (!good) Bad(std::string(key) + " must be an array of three finite numbers");
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
	bool ok_ = true;
	std::vector<std::string> used_;
};

bool ParseMaterial(const V* v, Material& m, std::string& error)
{
	if (!v) return FindMaterial("cotton-plain-weave", m);
	if (v->GetType() == V::Text) {
		if (!FindMaterial(v->GetText(), m)) return Fail(error, "unknown material " + v->GetText());
		return true;
	}
	Reader r(*v, "material", error);
	Material base;
	const std::string baseName = r.Text("base", "", {}, false);
	const bool haveBase = !baseName.empty();
	if (haveBase && !FindMaterial(baseName, base)) return r.Bad("unknown base material " + baseName);
	const double missing = std::numeric_limits<double>::quiet_NaN();
	m.name = r.Text("name", haveBase ? base.name + " (modified)" : "", {}, false);
	m.source = r.Text("source", haveBase ? base.source : "", {}, false);
	m.density = r.Real("density", haveBase ? base.density : missing, false);
	m.stretchWarp = r.Real("stretch_warp", haveBase ? base.stretchWarp : missing, false);
	m.stretchWeft = r.Real("stretch_weft", haveBase ? base.stretchWeft : missing, false);
	m.shear = r.Real("shear", haveBase ? base.shear : missing, false);
	m.poisson = r.Real("poisson", haveBase ? base.poisson : missing, false);
	m.bendWarp = r.Real("bend_warp", haveBase ? base.bendWarp : missing, false);
	m.bendWeft = r.Real("bend_weft", haveBase ? base.bendWeft : missing, false);
	m.dampingMass = r.Real("damping_mass", haveBase ? base.dampingMass : 0.0, false);
	m.dampingStiffness = r.Real("damping_stiffness", haveBase ? base.dampingStiffness : 0.0, false);
	if (!r.Finish()) return false;
	if (!ValidateMaterial(m, error)) {
		if (!haveBase) error += " (a material map without \"base\" needs name, source and every stiffness)";
		return false;
	}
	return true;
}

// A rigid motion sampled from the scene inputs (ISceneInputs::SampleTransform)
// at any time the model asks for, cached for the last time asked. A sampling
// failure is latched and fails the step that hit it.
class SceneTrack final : public TransformTrack {
public:
	SceneTrack(const ISceneInputs& inputs, std::string name) : inputs_(inputs), name_(std::move(name)) {}
	void At(double t, double rotation[9], double translation[3]) const override {
		std::lock_guard<std::mutex> hold(mutex_);
		if (!cached_ || t != time_) {
			RigidTransform m;
			std::string error;
			if (!inputs_.SampleTransform(name_, t, m, error)) {
				if (error_.empty()) error_ = "scene transform " + name_ + " at t=" + std::to_string(t) + ": " + error;
				m = RigidTransform{};
			}
			transform_ = m;
			time_ = t;
			cached_ = true;
		}
		std::copy(transform_.rotation.begin(), transform_.rotation.end(), rotation);
		std::copy(transform_.translation.begin(), transform_.translation.end(), translation);
	}
	std::string Error() const { std::lock_guard<std::mutex> hold(mutex_); return error_; }
private:
	const ISceneInputs& inputs_;
	const std::string name_;
	mutable std::mutex mutex_;
	mutable bool cached_ = false;
	mutable double time_ = 0;
	mutable RigidTransform transform_;
	mutable std::string error_;
};

// Scene inputs a configuration reads: names in first-use order (the key
// order) and the transform tracks to check after each step.
struct SceneUse {
	const ISceneInputs* inputs = nullptr;
	std::vector<std::string> names;
	std::vector<std::shared_ptr<const SceneTrack>> tracks;
	void Name(const std::string& n) { if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n); }
	std::vector<std::pair<std::string, std::size_t>> ports;   // M6: (accepted port, mesh collider index)
};

bool ParseMotion(const V* v, Motion& motion, V& effective, SceneUse& scene, std::string& error)
{
	if (!v) { motion = Motion{}; effective = V::MapValue({{"type", V::String("fixed")}}); return true; }
	Reader r(*v, "pin motion", error);
	const std::string type = r.Text("type", "fixed", {"fixed", "translate", "rotate", "oscillate", "scene_transform"});
	if (type == "scene_transform") {
		// A named rigid motion from the scene; its identity enters the key
		// through SolverDescription::inputs.
		const std::string input = r.Text("input", "-", {}, false);
		RigidTransform probe;
		std::string hash;
		if (input == "-") return r.Bad("a scene_transform motion names its scene input (input)");
		if (!scene.inputs || !scene.inputs->Identity(input, hash, error) || !scene.inputs->SampleTransform(input, 0.0, probe, error))
			return Fail(error, "motion scene input " + input + ": " + error);
		r.out.emplace_back("input", V::String(input));
		motion.type = Motion::Type::Track;
		auto track = std::make_shared<SceneTrack>(*scene.inputs, input);
		scene.tracks.push_back(track);
		motion.track = track;
		scene.Name(input);
	} else if (type == "translate") { motion.type = Motion::Type::Translate; motion.vector = r.Vector("velocity", {{0, 0, 0}}); }
	else if (type == "oscillate") {
		motion.type = Motion::Type::Oscillate;
		motion.vector = r.Vector("amplitude", {{0, 0, 0}});
		motion.frequency = r.Real("frequency", 1.0);
		if (!(motion.frequency >= 0)) r.Bad("frequency must not be negative");
	} else if (type == "rotate") {
		motion.type = Motion::Type::Rotate;
		motion.center = r.Vector("center", {{0, 0, 0}});
		V3 axis = r.Vector("axis", {{0, 1, 0}}, false);
		const double l = Length(axis);
		if (!(l > 0)) r.Bad("axis must be nonzero");
		else axis = {{axis[0]/l, axis[1]/l, axis[2]/l}};
		motion.axis = axis;
		r.out.emplace_back("axis", Vec(axis));
		motion.rate = r.Real("rate", 0.0);
	} else motion.type = Motion::Type::Fixed;
	if (!r.Finish()) return false;
	effective = V::MapValue(r.out);
	return true;
}

bool ParsePins(const V* v, const SheetSpec& sheet, std::vector<PinGroup>& pins, V& effective, SceneUse& scene,
	std::string& error)
{
	V::Values out;
	pins.clear();
	if (v && v->GetType() != V::Array) return Fail(error, "pins must be an array");
	if (v) for (const V& item : v->GetArray()) {
		Reader r(item, "pin", error);
		const std::string select = r.Text("select", "u0v0", {"u0v0", "u1v0", "u0v1", "u1v1", "u0", "u1", "v0", "v1", "boundary"});
		const std::uint64_t depth = r.Count("depth", 1, 1, 1000);
		V motionValue;
		PinGroup group;
		if (!ParseMotion(r.Raw("motion"), group.motion, motionValue, scene, error)) return false;
		r.out.emplace_back("motion", motionValue);
		if (!r.Finish()) return false;
		const std::uint32_t nu = sheet.verticesU, nv = sheet.verticesV;
		if (depth > nu || depth > nv) return Fail(error, "pin depth exceeds the sheet");
		const std::uint32_t d = static_cast<std::uint32_t>(depth);
		for (std::uint32_t j = 0; j < nv; ++j) for (std::uint32_t i = 0; i < nu; ++i) {
			const bool u0 = i < d, u1 = i + d >= nu, v0 = j < d, v1 = j + d >= nv;
			bool take = false;
			if (select == "u0v0") take = u0 && v0;
			else if (select == "u1v0") take = u1 && v0;
			else if (select == "u0v1") take = u0 && v1;
			else if (select == "u1v1") take = u1 && v1;
			else if (select == "u0") take = u0;
			else if (select == "u1") take = u1;
			else if (select == "v0") take = v0;
			else if (select == "v1") take = v1;
			else take = u0 || u1 || v0 || v1;
			if (take) group.vertices.push_back(i + nu*j);
		}
		pins.push_back(group);
		out.push_back(V::MapValue(r.out));
	}
	// A vertex in two groups follows the first.
	std::vector<char> seen(static_cast<std::size_t>(sheet.verticesU)*sheet.verticesV, 0);
	for (auto& g : pins) {
		std::vector<std::uint32_t> kept;
		for (std::uint32_t q : g.vertices) if (!seen[q]) { seen[q] = 1; kept.push_back(q); }
		g.vertices.swap(kept);
	}
	effective = V::ArrayValue(out);
	return true;
}

double FromF32(std::uint32_t bits);

// Flat array of finite numbers.
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

bool ParseColliders(const V* v, const ISceneInputs& inputs, std::vector<Collider>& colliders,
	std::vector<MeshCollider>& meshes, V& effective, SceneUse& scene, std::string& error)
{
	V::Values out;
	colliders.clear();
	meshes.clear();
	if (v && v->GetType() != V::Array) return Fail(error, "colliders must be an array");
	if (v) for (const V& item : v->GetArray()) {
		Reader r(item, "collider", error);
		const std::string type = r.Text("type", "plane", {"plane", "sphere", "box", "cylinder", "mesh"});
		Collider c;
		if (type == "plane") {
			c.type = Collider::Type::Plane;
			c.a = r.Vector("point", {{0, 0, 0}});
			V3 n = r.Vector("normal", {{0, 1, 0}}, false);
			const double l = Length(n);
			if (!(l > 0)) r.Bad("plane normal must be nonzero");
			else n = {{n[0]/l, n[1]/l, n[2]/l}};
			c.b = n;
			r.out.emplace_back("normal", Vec(n));
		} else if (type == "sphere") {
			c.type = Collider::Type::Sphere;
			c.a = r.Vector("center", {{0, 0, 0}});
			c.radius = r.Real("radius", 0.25);
			if (!(c.radius > 0)) r.Bad("sphere radius must be positive");
		} else if (type == "cylinder") {
			c.type = Collider::Type::Cylinder;
			c.a = r.Vector("point", {{0, 0, 0}});
			V3 n = r.Vector("axis", {{0, 0, 1}}, false);
			const double l = Length(n);
			if (!(l > 0)) r.Bad("cylinder axis must be nonzero");
			else n = {{n[0]/l, n[1]/l, n[2]/l}};
			c.b = n;
			r.out.emplace_back("axis", Vec(n));
			c.radius = r.Real("radius", 0.1);
			if (!(c.radius > 0)) r.Bad("cylinder radius must be positive");
		} else if (type == "box") {
			c.type = Collider::Type::Box;
			c.a = r.Vector("min", {{-0.5, -0.5, -0.5}});
			c.b = r.Vector("max", {{0.5, 0.5, 0.5}});
			if (!(c.a[0] < c.b[0] && c.a[1] < c.b[1] && c.a[2] < c.b[2])) r.Bad("box min must be below max");
		} else {
			// A triangle mesh: inline arrays, or a scene input (mesh.v1 or
			// mesh.v2, its static topology through the inputs) sampled at time 0.
			MeshCollider mesh;
			const std::string input = r.Text("scene_input", "-", {}, false);
			if (input != "-") {
				std::string hash, codec;
				std::shared_ptr<const Bytes> bytes;
				MeshFrame frame;
				if (!inputs.Identity(input, hash, error) || !inputs.Sample(input, 0.0, bytes, codec, error) || !bytes)
					return Fail(error, "collider scene input " + input + ": " + error);
				const StaticPayloadResolver resolve = [&inputs](const std::string& digest, std::size_t budget,
					std::shared_ptr<const Bytes>& fixed, std::string& why) { return inputs.ResolveStatic(digest, budget, fixed, why); };
				if ((codec != "mesh.v1" && codec != "mesh.v2") || !DecodeMesh(*bytes, CodecBudget, resolve, frame, error))
					return Fail(error, "collider scene input " + input + " is not a mesh frame: " + error);
				scene.Name(input);
				mesh.sceneInput = input;
				for (std::uint32_t bits : frame.positions) mesh.vertices.push_back(FromF32(bits));
				mesh.triangles = frame.indices;
				r.out.emplace_back("scene_input", V::String(input));
				r.out.emplace_back("identity", V::String(hash));
				r.Raw("identity"); // written back by Configure; accepted on input
			} else {
				std::vector<double> idx;
				if (!Numbers(r.Raw("vertices"), mesh.vertices) || mesh.vertices.size() % 3 != 0 || mesh.vertices.empty())
					return r.Bad("mesh vertices must be a flat array of 3n finite numbers");
				if (!Numbers(r.Raw("triangles"), idx) || idx.size() % 3 != 0 || idx.empty())
					return r.Bad("mesh triangles must be a flat array of 3m vertex indices");
				for (double k : idx) {
					if (!(k >= 0) || std::floor(k) != k || k >= static_cast<double>(mesh.vertices.size()/3))
						return r.Bad("mesh triangle index out of range");
					mesh.triangles.push_back(static_cast<std::uint32_t>(k));
				}
				V::Values vs, ts;
				for (double x : mesh.vertices) vs.push_back(V::Float(x));
				for (std::uint32_t k : mesh.triangles) ts.push_back(V::Unsigned(k));
				r.out.emplace_back("vertices", V::ArrayValue(vs));
				r.out.emplace_back("triangles", V::ArrayValue(ts));
			}
			V motionValue;
			// M6: {type: port, port: <name>} -- the collider follows a coupled surface.
			const V* mv = r.Raw("motion");
			const V* mt = mv && mv->GetType() == V::Map ? mv->Find("type") : nullptr;
			if (mt && mt->GetType() == V::Text && mt->GetText() == "port") {
				Reader mr(*mv, "collider motion", error);
				mr.Text("type", "port");
				const std::string port = mr.Text("port", DefaultColliderPort);
				if (!mr.Finish()) return false;
				if (port == LoadPort || port == StatePort || port == ReactionPort) return mr.Bad("a collider port needs its own name");
				for (const auto& q : scene.ports) if (q.first == port) return Fail(error, "two colliders follow the port " + port);
				mesh.motion.type = Motion::Type::Track;   // the port track is attached by Configure
				scene.ports.emplace_back(port, meshes.size());
				motionValue = V::MapValue(mr.out);
			} else if (!ParseMotion(mv, mesh.motion, motionValue, scene, error)) return false;
			r.out.emplace_back("motion", motionValue);
			if (!r.Finish()) return false;
			meshes.push_back(std::move(mesh));
			out.push_back(V::MapValue(r.out));
			continue;
		}
		if (!r.Finish()) return false;
		colliders.push_back(c);
		out.push_back(V::MapValue(r.out));
	}
	effective = V::ArrayValue(out);
	return true;
}

std::uint32_t F32(double x)
{
	const float f = static_cast<float>(x);
	std::uint32_t bits;
	std::memcpy(&bits, &f, 4);
	return bits;
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

// ------------------------------------------------------------------ evidence

struct EvidenceSpec {
	const char* id;
	const char* description;
	double tolerance[3]; // by integrator: backward Euler, BDF2, implicit midpoint
};
// Tolerances: 1.25 x the largest error observed at the stated resolution over
// the four built-in records for that integrator, rounded up
// (tests/ClothValidationTest.cpp "Evidence resolutions"; docs/SIMULATION_M3A_RESULTS.md).
// The order cases require an observed order of at least 1.8.
const EvidenceSpec EvidenceSpecs[] = {
	{"catenary-n20", "hanging strip, no bending, 20 segments: max position error / sag vs the elastic catenary",
		{1.7e-3, 1.7e-3, 1.7e-3}},
	{"cantilever-warp-n20", "warp strip cantilever, 20 segments: max position error / tip drop vs the elastica",
		{8.7e-4, 8.7e-4, 8.7e-4}},
	{"cantilever-weft-n20", "weft strip cantilever, 20 segments: max position error / tip drop vs the elastica",
		{8.7e-4, 8.7e-4, 8.7e-4}},
	{"membrane-11-n16", "pretensioned membrane (1,1) mode, 16 cells, 40 steps/period: relative frequency error",
		{1.3e-2, 1.3e-2, 5.5e-3}},
	{"membrane-21-n16", "pretensioned membrane (2,1) mode, 16 cells, 40 steps/period: relative frequency error",
		{1.7e-2, 1.7e-2, 9.6e-3}},
	{"plate-ss-n16", "simply supported square plate fundamental, 16 cells, 40 steps/period: relative frequency error",
		{1.5e-2, 1.4e-2, 6.6e-3}},
	{"energy-n16", "undamped membrane mode, 40 steps/period: |energy change per period - the integrator's linear theory|",
		{3.7e-7, 1.8e-7, 1.2e-9}},
	{"order-cantilever", "cantilever shape error at 10/20/40 segments: 2 - smallest observed order (floor 0)",
		{0.2, 0.2, 0.2}},
	{"order-membrane", "membrane (1,1) frequency error at 8/16 cells: 2 - observed order (floor 0)",
		{0.2, 0.2, 0.2}},
};
// Barrier contact (only when the run has contact; tests/ClothValidationTest.cpp
// "Contact evidence" over the built-in records with the default contact
// settings).  rest, stick and slide are exact up to the Newton tolerance (their
// tolerance is a solver-tolerance level, not 1.25 x observed); the mesh case
// must have no violation; the bounce must not gain energy.
const EvidenceSpec ContactSpecs[] = {
	{"contact-rest-n20", "strip on a plane: max(|contact force / weight - 1|, max |gap - barrier law| / gap)",
		{1e-8, 1e-8, 1e-8}},
	{"contact-cylinder-n40", "strip draped over a cylinder, 40 segments: max centre-line error / half length vs the wrapped elastica",
		{2.0e-3, 2.0e-3, 2.0e-3}},
	{"contact-stick", "sheet on a plane tilted to half the friction angle: |creep speed - smoothed-friction law| / slip speed",
		{1e-5, 1e-5, 1e-5}},
	{"contact-slide", "sheet on a plane tilted 0.2 rad past the friction angle: relative acceleration error vs g (sin - mu cos)",
		{1e-6, 1e-6, 1e-6}},
	{"contact-mesh", "sheet falling over a tilted cube mesh, 0.5 s: steps whose straight path or end state violates the offset",
		{0.5, 0.5, 0.5}},
	{"contact-bounce", "sheet dropped on a frictionless plane, undamped: largest total-energy rise / drop energy",
		{1e-6, 1e-6, 1e-6}},
};

// Self-collision (only when the run has it; tests/ClothValidationTest.cpp
// "Self-collision evidence" over the built-in records and both bake
// integrators with the default contact settings).  The stack's force balance
// is exact up to the Newton tolerance; stick and slide are 1.25 x the largest
// observed (the layered contact's discretisation, which falls under mesh
// refinement); the fold must have no violation.
const EvidenceSpec SelfSpecs[] = {
	{"self-stack-n6", "two layers on a plane: max |force / weight - 1| (plane: both, lower layer: the upper's); the gap within (0, dhat)",
		{1e-8, 1e-8, 1e-8}},
	{"self-stick", "a layer on a pinned layer tilted to half the self-friction angle: |creep speed - smoothed-friction law| / slip speed",
		{1.6e-2, 1.6e-2, 1.6e-2}},
	{"self-slide", "a layer on a pinned layer tilted 0.2 rad past the self-friction angle: relative acceleration error vs g (sin - mu cos)",
		{4.8e-3, 4.8e-3, 4.8e-3}},
	{"self-fold", "strip folded onto itself by moving pins, 1.2 s: steps whose straight path or end state violates an offset (cloth-cloth included)",
		{0.5, 0.5, 0.5}},
};

struct EvidenceKey {
	Material material;
	Triangulation triangulation;
	BendingModel bending;
	Scheme scheme;
	bool contact = false;          // barrier contact cases too
	bool self = false;             // and the self-collision cases
	BarrierSettings barrier;
	double thickness = 0;
};

V EvidenceConfiguration(const EvidenceKey& k, const char* id, int resolution)
{
	V::Members m{{"solver", V::String(ClothSolverId)}, {"version", V::String(SolverVersion)},
		{"case", V::String(id)}, {"resolution", V::Unsigned(static_cast<std::uint64_t>(resolution))},
		{"material", MaterialToValue(k.material)}, {"triangulation", V::String(TriangulationName(k.triangulation))},
		{"bending_model", V::String(BendingModelName(k.bending))}, {"integrator", V::String(SchemeName(k.scheme))}};
	if (k.contact)
		m.emplace_back("contact", V::MapValue({{"barrier_stiffness", V::Float(k.barrier.stiffness)},
			{"contact_activation", V::Float(k.barrier.activation)}, {"friction", V::Float(k.barrier.friction)},
			{"friction_velocity", V::Float(k.barrier.frictionVelocity)}, {"contact_thickness", V::Float(k.thickness)}}));
	if (k.contact && k.self)
		m.emplace_back("self_collision", V::MapValue({{"self_friction", V::Float(k.barrier.selfFriction)}}));
	return V::MapValue(m);
}

// The canonical cases (contact off in k), or only the contact cases.
std::vector<EvidenceCase> RunEvidence(const EvidenceKey& k, bool contactPart)
{
	CaseSettings s;
	s.material = k.material;
	s.triangulation = k.triangulation;
	s.bending = k.bending;
	s.scheme = k.scheme;
	std::vector<EvidenceCase> out;
	auto add = [&](const EvidenceSpec& spec, int resolution, const CaseResult& r, double error) {
		EvidenceCase c;
		c.id = spec.id;
		c.description = spec.description + (r.ok ? std::string() : std::string(" (failed: ") + r.failure + ")");
		c.error = r.ok && std::isfinite(error) ? error : 1.0;
		c.tolerance = spec.tolerance[static_cast<int>(k.scheme)];
		c.passed = r.ok && c.error <= c.tolerance;
		c.digest = EvidenceDigest(c.id, EvidenceConfiguration(k, spec.id, resolution), c.error, c.tolerance, c.passed);
		out.push_back(c);
	};
	// The cases are independent, single-threaded and deterministic (each builds
	// its own system from a copy of the settings), so they run concurrently
	// and are collected in the order they were always added: the evidence,
	// digests included, is the same as when they ran one after another.
	auto launch = [&](int resolution, auto fn) {
		CaseSettings t = s;
		t.resolution = resolution;
		return std::async(std::launch::async, [t, fn]() { return fn(t); });
	};
	if (!contactPart) {
		const int levels[3] = {10, 20, 40};
		auto fCat = launch(20, [](const CaseSettings& t) { return RunCatenary(t); });
		std::future<CaseResult> fCw[3];
		for (int l = 0; l < 3; ++l) fCw[l] = launch(levels[l], [](const CaseSettings& t) { return RunCantilever(t, false); });
		auto fCf = launch(20, [](const CaseSettings& t) { return RunCantilever(t, true); });
		auto fM11 = launch(16, [](const CaseSettings& t) { return RunMembraneMode(t, 1, 1); });
		auto fM21 = launch(16, [](const CaseSettings& t) { return RunMembraneMode(t, 2, 1); });
		auto fPlate = launch(16, [](const CaseSettings& t) { return RunPlate(t, false); });
		auto fEnergy = launch(16, [](const CaseSettings& t) { return RunEnergyDrift(t); });
		auto fM8 = launch(8, [](const CaseSettings& t) { return RunMembraneMode(t, 1, 1); });
		const CaseResult cat = fCat.get();
		add(EvidenceSpecs[0], 20, cat, cat.error);
		CaseResult cw[3];
		for (int l = 0; l < 3; ++l) cw[l] = fCw[l].get();
		const CaseResult cf = fCf.get();
		add(EvidenceSpecs[1], 20, cw[1], cw[1].error);
		add(EvidenceSpecs[2], 20, cf, cf.error);
		const CaseResult m11 = fM11.get(), m21 = fM21.get();
		add(EvidenceSpecs[3], 16, m11, m11.error);
		add(EvidenceSpecs[4], 16, m21, m21.error);
		const CaseResult plate = fPlate.get();
		add(EvidenceSpecs[5], 16, plate, plate.error);
		const CaseResult energy = fEnergy.get();
		add(EvidenceSpecs[6], 16, energy, energy.error);
		CaseResult order = cw[2];
		order.ok = cw[0].ok && cw[1].ok && cw[2].ok;
		if (!order.ok) order.failure = "a cantilever level failed";
		const double pc = std::min(ObservedOrder(cw[0].error, cw[1].error), ObservedOrder(cw[1].error, cw[2].error));
		add(EvidenceSpecs[7], 40, order, std::max(0.0, 2 - pc));
		CaseResult m8 = fM8.get();
		m8.ok = m8.ok && m11.ok;
		const double pm = ObservedOrder(m8.error, m11.error);
		add(EvidenceSpecs[8], 16, m8, std::max(0.0, 2 - pm));
		return out;
	}
	if (k.contact) {
		ContactSettings c;
		c.barrier = k.barrier;
		c.barrier.selfCollision = false;
		c.thickness = k.thickness;
		const double mu = c.barrier.friction;
		ContactSettings sc = c;
		sc.barrier.selfFriction = k.barrier.selfFriction;
		const double ms = sc.barrier.selfFriction;
		auto fRest = launch(20, [c](const CaseSettings& t) { return RunContactRest(t, c); });
		auto fCyl = launch(40, [c](const CaseSettings& t) { return RunContactCylinder(t, c); });
		auto fStick = launch(10, [c, mu](const CaseSettings& t) {
			CaseResult r = RunContactIncline(t, c, 0.5*std::atan(mu));
			if (!(mu > 0)) { r.ok = false; r.failure = "no friction"; }
			return r; });
		auto fSlide = launch(10, [c, mu](const CaseSettings& t) { return RunContactIncline(t, c, std::atan(mu) + 0.2); });
		auto fMesh = launch(10, [c](const CaseSettings& t) { return RunContactMesh(t, c, false, 0.5); });
		auto fBounce = launch(10, [c](const CaseSettings& t) { return RunContactBounce(t, c); });
		std::future<CaseResult> fStack, fSStick, fSSlide, fFold;
		if (k.self) {
			fStack = launch(6, [sc](const CaseSettings& t) { return RunSelfStack(t, sc); });
			fSStick = launch(6, [sc, ms](const CaseSettings& t) {
				CaseResult r = RunSelfIncline(t, sc, 0.5*std::atan(ms));
				if (!(ms > 0)) { r.ok = false; r.failure = "no friction between layers"; }
				return r; });
			fSSlide = launch(6, [sc, ms](const CaseSettings& t) { return RunSelfIncline(t, sc, std::atan(ms) + 0.2); });
			fFold = launch(10, [sc](const CaseSettings& t) { return RunSelfFold(t, sc, 1, 1.2); });
		}
		const CaseResult rest = fRest.get();
		add(ContactSpecs[0], 20, rest, rest.error);
		const CaseResult cyl = fCyl.get();
		add(ContactSpecs[1], 40, cyl, cyl.error);
		const CaseResult stick = fStick.get();
		add(ContactSpecs[2], 10, stick, stick.error);
		const CaseResult slide = fSlide.get();
		add(ContactSpecs[3], 10, slide, slide.error);
		const CaseResult mesh = fMesh.get();
		add(ContactSpecs[4], 10, mesh, mesh.error);
		const CaseResult bounce = fBounce.get();
		add(ContactSpecs[5], 10, bounce, bounce.error);
		if (k.self) {
			const CaseResult stack = fStack.get();
			add(SelfSpecs[0], 6, stack, stack.error);
			const CaseResult sstick = fSStick.get();
			add(SelfSpecs[1], 6, sstick, sstick.error);
			const CaseResult sslide = fSSlide.get();
			add(SelfSpecs[2], 6, sslide, sslide.error);
			const CaseResult fold = fFold.get();
			add(SelfSpecs[3], 10, fold, fold.error);
		}
	}
	return out;
}

// Deterministic, so a process-wide memo is exact.
std::vector<EvidenceCase> Memoised(const EvidenceKey& k, bool contactPart)
{
	static std::mutex mutex;
	static std::map<std::string, std::vector<EvidenceCase>> memo;
	Bytes key;
	std::string ignored;
	RISECBOR64::Encode(EvidenceConfiguration(k, contactPart ? "contact" : "all", 0), key, &ignored);
	const std::string digest = RISECBOR64::SHA256Hex(key);
	{
		std::lock_guard<std::mutex> hold(mutex);
		const auto it = memo.find(digest);
		if (it != memo.end()) return it->second;
	}
	std::vector<EvidenceCase> cases = RunEvidence(k, contactPart);
	std::lock_guard<std::mutex> hold(mutex);
	memo[digest] = cases;
	return cases;
}

// The canonical cases (bound to the configuration without contact), then the
// contact cases when the run has barrier contact.
std::vector<EvidenceCase> Evidence(const EvidenceKey& k)
{
	EvidenceKey base = k;
	base.contact = false;
	std::vector<EvidenceCase> cases = Memoised(base, false);
	if (k.contact) {
		const std::vector<EvidenceCase> more = Memoised(k, true);
		cases.insert(cases.end(), more.begin(), more.end());
	}
	return cases;
}

// ------------------------------------------------------------------ solver

// ------------------------------------------------------------------ M6: colliders driven by a port

// The rigid motion of a port-driven mesh collider: the map from its rest pose
// (its vertices as configured: the surface at time 0) to the pose fitted to
// the accepted surface.  The accepted surface is where the collider is at the
// END of the held step; over the step it moves rigidly from the pose it held
// before (rotation at a constant rate about the relative axis, the rest
// centroid linearly).  At(0) is the rest pose.
class PortTrack final : public TransformTrack {
public:
	static constexpr std::size_t Size = 12 + 12 + 3;   // start pose, end pose, t0, t1, have
	std::array<double,12> start{{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}}, end{{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}};
	double t0 = 0, t1 = 0;
	bool have = false;
	V3 centroid{{0, 0, 0}};                              // of the rest vertices
	void At(double t, double rotation[9], double translation[3]) const override {
		if (t == 0 || !have) {
			const double* src = t == 0 ? Identity() : end.data();
			std::copy(src, src + 9, rotation);
			std::copy(src + 9, src + 12, translation);
			return;
		}
		const double f = t1 > t0 ? (t - t0)/(t1 - t0) : 1.0;
		// Relative rotation D = Re Rs^T as axis-angle, scaled by f.
		double D[9];
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
			double a = 0;
			for (int k = 0; k < 3; ++k) a += end[3*i + k]*start[3*j + k];
			D[3*i + j] = a;
		}
		double axis[3] = {D[7] - D[5], D[2] - D[6], D[3] - D[1]};
		const double sn = 0.5*std::sqrt(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]);
		const double cs = 0.5*(D[0] + D[4] + D[8] - 1);
		const double angle = std::atan2(sn, cs);
		double F[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		if (sn > 0) {
			for (double& a : axis) a /= 2*sn;
			const double th = f*angle, c = std::cos(th), s = std::sin(th), C = 1 - c;
			const double x = axis[0], y = axis[1], z = axis[2];
			const double R[9] = {c + x*x*C, x*y*C - z*s, x*z*C + y*s, y*x*C + z*s, c + y*y*C, y*z*C - x*s,
				z*x*C - y*s, z*y*C + x*s, c + z*z*C};
			std::copy(R, R + 9, F);
		}
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
			double a = 0;
			for (int k = 0; k < 3; ++k) a += F[3*i + k]*start[3*k + j];
			rotation[3*i + j] = a;
		}
		// The rest centroid moves linearly from its start to its end position.
		for (int i = 0; i < 3; ++i) {
			const double ps = start[3*i]*centroid[0] + start[3*i + 1]*centroid[1] + start[3*i + 2]*centroid[2] + start[9 + i];
			const double pe = end[3*i]*centroid[0] + end[3*i + 1]*centroid[1] + end[3*i + 2]*centroid[2] + end[9 + i];
			const double p = ps + f*(pe - ps);
			translation[i] = p - (rotation[3*i]*centroid[0] + rotation[3*i + 1]*centroid[1] + rotation[3*i + 2]*centroid[2]);
		}
	}
	static const double* Identity() { static const double I[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; return I; }
};

// The rigid map (R, c) with p_i ~ R r_i + c in the least-squares sense
// (Horn 1987: the largest eigenvector of the 4x4 quaternion matrix, Jacobi).
std::array<double,12> FitRigid(const std::vector<double>& rest, const std::vector<double>& moved)
{
	const std::size_t n = rest.size()/3;
	double rc[3] = {0, 0, 0}, pc[3] = {0, 0, 0};
	for (std::size_t i = 0; i < n; ++i) for (int a = 0; a < 3; ++a) { rc[a] += rest[3*i + a]; pc[a] += moved[3*i + a]; }
	for (int a = 0; a < 3; ++a) { rc[a] /= static_cast<double>(n); pc[a] /= static_cast<double>(n); }
	double S[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
	for (std::size_t i = 0; i < n; ++i) for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b)
		S[a][b] += (rest[3*i + a] - rc[a])*(moved[3*i + b] - pc[b]);
	double N[4][4] = {
		{S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0]},
		{S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2]},
		{S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1]},
		{S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2]}};
	double Q[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
	for (int sweep = 0; sweep < 50; ++sweep) {
		double off = 0, diag = 0;
		for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) (i == j ? diag : off) += N[i][j]*N[i][j];
		if (!(off > 1e-30*diag)) break;
		for (int p = 0; p < 3; ++p) for (int q = p + 1; q < 4; ++q) {
			if (N[p][q] == 0) continue;
			const double th = 0.5*std::atan2(2*N[p][q], N[q][q] - N[p][p]), c = std::cos(th), s = std::sin(th);
			for (int k = 0; k < 4; ++k) { const double a = N[k][p], b = N[k][q]; N[k][p] = c*a - s*b; N[k][q] = s*a + c*b; }
			for (int k = 0; k < 4; ++k) { const double a = N[p][k], b = N[q][k]; N[p][k] = c*a - s*b; N[q][k] = s*a + c*b; }
			for (int k = 0; k < 4; ++k) { const double a = Q[k][p], b = Q[k][q]; Q[k][p] = c*a - s*b; Q[k][q] = s*a + c*b; }
		}
	}
	int best = 0;
	for (int i = 1; i < 4; ++i) if (N[i][i] > N[best][best]) best = i;
	double w = Q[0][best], x = Q[1][best], y = Q[2][best], z = Q[3][best];
	const double l = std::sqrt(w*w + x*x + y*y + z*z);
	w /= l; x /= l; y /= l; z /= l;
	std::array<double,12> m{{1 - 2*(y*y + z*z), 2*(x*y - w*z), 2*(x*z + w*y), 2*(x*y + w*z), 1 - 2*(x*x + z*z), 2*(y*z - w*x),
		2*(x*z - w*y), 2*(y*z + w*x), 1 - 2*(x*x + y*y), 0, 0, 0}};
	for (int a = 0; a < 3; ++a) m[9 + a] = pc[a] - (m[3*a]*rc[0] + m[3*a + 1]*rc[1] + m[3*a + 2]*rc[2]);
	return m;
}

// One port-driven collider: its track, its rest surface, and the reaction
// impulse accumulated over the held step (about `point`, the collider's rest
// centroid placed at the step's end pose).
struct PortCollider {
	std::string port;
	std::size_t mesh = 0;
	std::shared_ptr<PortTrack> track;
	std::vector<double> rest;
	std::string topology;
	double impulse[3] = {0, 0, 0}, angular[3] = {0, 0, 0}, time = 0;
	V3 point{{0, 0, 0}};
	static constexpr std::size_t Size = PortTrack::Size + 10;
};

// Everything that determines later payloads (checkpoint and rollback).
struct ClothState {
	double t = 0, segStart = 0, segTarget = 0, heldFor = 0, hPrev = 0, time = 0;
	std::uint64_t next = 0, segN = 0, segJ = 0;
	bool finished = false, held = false, havePrev = false;
	std::vector<double> x, v, xPrev, vPrev, pressure, traction;
	std::vector<double> colliders;   // M6: PortCollider::Size per port-driven collider
	std::vector<double> force;       // M7e: the held per-vertex force (empty: none)
	std::vector<double> stepImpulse; // M7e: the held partner steps (3 per vertex per step) and their segments
	std::vector<std::uint32_t> stepSegment;
	std::uint64_t stepCursor = 0, segmentOrdinal = 0;
	std::vector<double> linearX0;    // M7e: the linear model's origin (empty: none)
};

struct ClothToken final : SolverStateToken {
	const void* owner = nullptr;
	ClothState state;
};

class ClothSolver final : public ISimulationSolver {
public:
	bool Configure(const V& parameters, const ISceneInputs&, SolverDescription& d, std::string& error) override;
	bool Start(std::string& error) override;
	bool Restore(const Bytes& state, std::string& error) override;
	bool Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
		AdvanceResult& result, std::string& error) override;
	bool Checkpoint(Bytes& state, std::string& error) override;
	SolverPosition Position() const override { return {t_, next_, finished_}; }
	void RequestStop() override { stop_ = true; }
	bool SaveState(std::shared_ptr<const SolverStateToken>& token, std::string& error) override;
	bool RestoreState(const SolverStateToken& token, std::string& error) override;

private:
	double OutputTime(std::int64_t i) const { return static_cast<double>(i)*interval_; }
	bool Done(AdvanceResult& r, AdvanceResult::Status s) {
		r.status = finished_ ? AdvanceResult::Status::Finished : s;
		r.position = Position();
		return true;
	}
	bool Encode(Bytes& frame, std::shared_ptr<const Bytes>& fixed, std::string& error);
	bool Publish(std::int64_t index, double time, IFrameSink& sink, IPortExchange* ports, std::string& error);
	bool OfferState(double time, IPortExchange& ports, std::string& error);
	bool OfferMechanics(double time, IPortExchange& ports, std::string& error);   // M7e
	bool AcceptSteps(IPortExchange* ports, double time, std::string& error);       // M7e
	std::vector<double> stepImpulse_;          // M7e: the held partner steps (StepsPort)
	bool stepsUnbound_ = false;                // M7f: StepsPort not connected (at the last accept)
	std::vector<std::uint32_t> stepSegment_;
	std::uint64_t stepCursor_ = 0, segmentOrdinal_ = 0;
	bool AcceptLoads(double time, IPortExchange* ports, std::string& error);
	ClothState Capture() const;
	bool Apply(ClothState&& state, std::string& error);
	bool TrackFailure(std::string& error) const;
	// M6: port-driven colliders and their contact reaction.
	bool AcceptColliders(double t0, double t1, IPortExchange* ports, std::string& error);
	bool OfferReaction(double time, IPortExchange& ports, std::string& error);
	void ObserveSteps();
	std::vector<double> SaveColliders() const;
	bool LoadColliders(const std::vector<double>& state, std::string& error);
	std::vector<PortCollider> colliders_;
	std::string reactionDomain_;

	bool configured_ = false, started_ = false, finished_ = false;
	SheetSpec sheet_;
	ModelConfig model_;
	Scheme scheme_ = Scheme::BDF2;
	NewtonOptions newton_;
	std::uint64_t count_ = 0, substeps_ = 1;
	double interval_ = 1.0/30;
	unsigned paceMs_ = 0, threads_ = 1;
	ClothSystem system_;
	std::vector<std::uint32_t> indices_;
	std::vector<std::uint32_t> materialUV_;   // rest (u, v) per vertex, m, binary32
	std::string topology_;                    // topology digest: the surface_load domain
	std::string stiffnessDomain_;             // M7e: the surface_stiffness index space (the Hessian's upper blocks)
	std::string elementsDomain_;              // M7h: the surface_elements index space (the elements' upper blocks)
	std::vector<std::shared_ptr<const SceneTrack>> tracks_;
	double t_ = 0, segStart_ = 0, segTarget_ = 0, heldFor_ = 0;
	std::int64_t next_ = 0;
	std::uint64_t segN_ = 0, segJ_ = 0;
	bool held_ = false;                       // port values read for the macro step ending at heldFor_
	bool staticSent_ = false;                 // topology published to the sink in this run
	bool staticReady_ = false;                // staticRecord_ matches indices_ and materialUV_
	MeshStaticRecord staticRecord_;           // mesh.topology.v1 bytes and digests, encoded once per configuration
	std::shared_ptr<const Bytes> staticPayload_; // the same bytes, shared with sinks and port relays
	StepStats stats_;
	std::atomic<bool> stop_{false};
};

bool ClothSolver::Configure(const V& parameters, const ISceneInputs& inputs, SolverDescription& d, std::string& error)
{
	configured_ = started_ = false;
	Reader p(parameters, "", error);
	const std::string mode = p.Text("mode", "bake", {"bake", "interactive"});
	const bool interactive = mode == "interactive";
	const std::string integrator = p.Text("integrator", interactive ? "backward_euler" : "bdf2",
		{"bdf2", "backward_euler", "implicit_midpoint"});
	if (interactive && integrator != "backward_euler") return Fail(error, "interactive mode integrates with backward_euler");
	SheetSpec sheet;
	sheet.sizeU = p.Real("size_u", 1.0);
	sheet.sizeV = p.Real("size_v", 1.0);
	sheet.verticesU = static_cast<std::uint32_t>(p.Count("vertices_u", 51, 2, 2000));
	sheet.verticesV = static_cast<std::uint32_t>(p.Count("vertices_v", 51, 2, 2000));
	sheet.triangulation = p.Text("triangulation", "alternating", {"alternating", "consistent"}) == "alternating" ?
		Triangulation::Alternating : Triangulation::Consistent;
	sheet.bending = p.Text("bending_model", "lines", {"lines", "hinges"}) == "lines" ? BendingModel::Lines : BendingModel::Hinges;
	sheet.origin = p.Vector("origin", {{-0.5, 1.0, 0.5}});
	V3 au = p.Vector("axis_u", {{1, 0, 0}}, false), av = p.Vector("axis_v", {{0, 0, -1}}, false);
	{
		const double lu = Length(au);
		if (!(lu > 0)) return Fail(error, "axis_u must be nonzero");
		au = {{au[0]/lu, au[1]/lu, au[2]/lu}};
		const double dot = au[0]*av[0] + au[1]*av[1] + au[2]*av[2];
		av = {{av[0] - dot*au[0], av[1] - dot*au[1], av[2] - dot*au[2]}};
		const double lv = Length(av);
		if (!(lv > 1e-9)) return Fail(error, "axis_v must not be parallel to axis_u");
		av = {{av[0]/lv, av[1]/lv, av[2]/lv}};
		p.out.emplace_back("axis_u", Vec(au));
		p.out.emplace_back("axis_v", Vec(av));
	}
	sheet.axisU = au; sheet.axisV = av;
	sheet.stretchU = p.Real("stretch_u", 1.0);
	sheet.stretchV = p.Real("stretch_v", 1.0);
	ModelConfig model;
	if (!ParseMaterial(p.Raw("material"), model.material, error)) return false;
	p.out.emplace_back("material", MaterialToValue(model.material));
	const std::uint64_t frames = p.Count("frames", 121, 1, 1000000);
	const double rate = p.Real("frame_rate", 30.0);
	const std::uint64_t substeps = p.Count("substeps", interactive ? 2 : 8, 1, 100000);
	model.gravity = p.Vector("gravity", {{0, -9.81, 0}});
	V pinsValue, collidersValue;
	SceneUse scene;
	scene.inputs = &inputs;
	if (!ParsePins(p.Raw("pins"), sheet, model.pins, pinsValue, scene, error)) return false;
	p.out.emplace_back("pins", pinsValue);
	if (!ParseColliders(p.Raw("colliders"), inputs, model.colliders, model.meshColliders, collidersValue, scene, error)) return false;
	std::vector<PortCollider> portColliders;   // M6
	for (const auto& q : scene.ports) {
		PortCollider pc;
		pc.port = q.first;
		pc.mesh = q.second;
		pc.track = std::make_shared<PortTrack>();
		MeshCollider& mc = model.meshColliders[q.second];
		mc.motion.track = pc.track;
		pc.rest = mc.vertices;
		pc.topology = MeshTopologyDigest(static_cast<std::uint32_t>(mc.vertices.size()/3), mc.triangles);
		for (std::size_t i = 0; i < mc.vertices.size()/3; ++i) for (int a = 0; a < 3; ++a)
			pc.track->centroid[a] += mc.vertices[3*i + a]/static_cast<double>(mc.vertices.size()/3);
		portColliders.push_back(std::move(pc));
	}
	p.out.emplace_back("colliders", collidersValue);
	const std::string contact = p.Text("contact", interactive ? "penalty" : "barrier", {"barrier", "penalty"});
	model.contact = contact == "barrier" ? ContactModel::Barrier : ContactModel::Penalty;
	model.contactStiffness = p.Real("contact_stiffness", 1e5);
	model.contactThickness = p.Real("contact_thickness", 0.002);
	model.barrier.stiffness = p.Real("barrier_stiffness", 1e5);
	model.barrier.activation = p.Real("contact_activation", 1e-3);
	model.barrier.friction = p.Real("friction", 0.3);
	model.barrier.frictionVelocity = p.Real("friction_velocity", 1e-3);
	model.barrier.selfCollision = p.Bool("self_collision", false);
	model.barrier.selfFriction = p.Real("self_friction", 0.3);
	model.pressure = p.Real("pressure", 0.0);
	model.dragCoefficient = p.Real("drag_coefficient", 0.0);
	model.airDensity = p.Real("air_density", 1.2);
	model.damping = p.Bool("damping", true);
	NewtonOptions newton;
	newton.tolerance = p.Real("newton_tolerance", interactive ? 1e-2 : 1e-3);
	newton.maxIterations = static_cast<int>(p.Count("newton_iterations", interactive ? 3 : 50, 1, 1000));
	newton.cgTolerance = p.Real("cg_tolerance", interactive ? 1e-2 : 1e-6);
	newton.exactHessianFirst = !interactive;
	newton.cgIterations = static_cast<int>(p.Count("cg_iterations", 2000, 1, 1000000));
	const std::string solver = p.Text("linear_solver", "auto", {"auto", "mg_pcg", "pcg", "direct"});
	newton.solver = solver == "auto" ? LinearSolver::Auto : solver == "pcg" ? LinearSolver::ConjugateGradient :
		solver == "mg_pcg" ? LinearSolver::MultigridCG : LinearSolver::Direct;
	newton.requireConvergence = !interactive;
	const unsigned pace = static_cast<unsigned>(p.Count("pace_ms", 0, 0, 10000));
	p.out.pop_back(); // pace never enters the key
	// Results are bitwise the same for any thread count (ClothParallel.h), so
	// the count never enters the key either.
	const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
	const unsigned threads = static_cast<unsigned>(p.Count("threads", std::min(4u, hardware), 1, 256));
	p.out.pop_back();
	SolverDescription candidate;
	if (!p.Finish()) return false;
	candidate.effectiveParameters = V::MapValue(p.out);
	if (!(sheet.sizeU > 0) || !(sheet.sizeV > 0) || !(sheet.stretchU > 0) || !(sheet.stretchV > 0))
		return Fail(error, "sheet size and stretch must be positive");
	if (!(rate > 0)) return Fail(error, "frame_rate must be positive");
	if (!(model.contactStiffness >= 0) || !(model.contactThickness >= 0) || !(model.dragCoefficient >= 0) ||
		!(model.airDensity >= 0)) return Fail(error, "contact and drag parameters must not be negative");
	const bool anyContact = !model.colliders.empty() || !model.meshColliders.empty() ||
		(model.contact == ContactModel::Barrier && model.barrier.selfCollision);
	if (model.contact == ContactModel::Penalty && (!model.meshColliders.empty() || model.barrier.selfCollision))
		return Fail(error, "mesh colliders and self-collision need contact = barrier");
	if (model.contact == ContactModel::Barrier && anyContact && integrator == "implicit_midpoint")
		return Fail(error, "barrier contact integrates with bdf2 or backward_euler");
	if (!(newton.tolerance > 0) || !(newton.cgTolerance > 0) || newton.cgTolerance >= 1)
		return Fail(error, "solver tolerances must be positive (cg_tolerance below 1)");
	Sheet built;
	ClothSystem probe;
	if (!BuildSheet(sheet, model.material, built, error) || !probe.Initialize(built, model, error)) return false;
	sheet_ = sheet; model_ = model; newton_ = newton;
	scheme_ = integrator == "bdf2" ? Scheme::BDF2 : integrator == "backward_euler" ? Scheme::BackwardEuler : Scheme::ImplicitMidpoint;
	count_ = frames; substeps_ = substeps; interval_ = 1/rate; paceMs_ = pace; threads_ = threads;
	indices_.clear();
	for (const auto& t : built.triangles) indices_.insert(indices_.end(), {t.v[0], t.v[1], t.v[2]});
	// Material coordinates: the vertex's rest position on the sheet, metres.
	materialUV_.clear();
	for (std::uint32_t j = 0; j < sheet.verticesV; ++j) for (std::uint32_t i = 0; i < sheet.verticesU; ++i) {
		materialUV_.push_back(F32(sheet.sizeU*static_cast<double>(i)/static_cast<double>(sheet.verticesU - 1)));
		materialUV_.push_back(F32(sheet.sizeV*static_cast<double>(j)/static_cast<double>(sheet.verticesV - 1)));
	}
	topology_ = MeshTopologyDigest(built.Vertices(), indices_);
	{
		// M7e: the stiffness blocks (row <= column) in pattern order name surface_stiffness's index space.
		const BlockMatrix& pat = probe.StiffnessPattern();
		V::Values pairs;
		for (std::uint32_t i = 0; i < pat.Blocks(); ++i)
			for (std::size_t k = pat.RowStart()[i]; k < pat.RowStart()[i + 1]; ++k)
				if (pat.Column()[k] >= i) pairs.push_back(V::ArrayValue({V::Unsigned(i), V::Unsigned(pat.Column()[k])}));
		Bytes encoded;
		RISECBOR64::Encode(V::ArrayValue({V::String("cloth.stiffness.v1"), V::String(topology_), V::ArrayValue(pairs)}), encoded, nullptr);
		stiffnessDomain_ = RISECBOR64::SHA256Hex(encoded);
		// M7h: the elements' blocks (element, kind, row, column) name surface_elements' index space.
		CorotatedElements elems;
		probe.LinearElements(built.initial, elems);
		V::Values items;
		for (std::size_t e = 0; e < elems.Size(); ++e) {
			const CorotatedElements::Element& el = elems.elements[e];
			for (int a = 0; a < el.count; ++a) for (int b = a; b < el.count; ++b)
				items.push_back(V::ArrayValue({V::Unsigned(e), V::Unsigned(el.kind), V::Unsigned(el.v[a]), V::Unsigned(el.v[b])}));
		}
		Bytes encodedElements;
		RISECBOR64::Encode(V::ArrayValue({V::String("cloth.elements.v1"), V::String(topology_), V::ArrayValue(items)}), encodedElements, nullptr);
		elementsDomain_ = RISECBOR64::SHA256Hex(encodedElements);
	}
	staticReady_ = false;   // the static record is rebuilt for this configuration on first use
	tracks_ = scene.tracks;
	colliders_ = std::move(portColliders);
	{
		V::Values names;
		for (const auto& pc : colliders_) names.push_back(V::String(pc.port));
		Bytes encoded;
		RISECBOR64::Encode(V::ArrayValue({V::String("cloth.collider_reaction.v1"), V::ArrayValue(names)}), encoded, nullptr);
		reactionDomain_ = RISECBOR64::SHA256Hex(encoded);
	}
	configured_ = true;

	candidate.solverId = ClothSolverId;
	candidate.version = SolverVersion;
	candidate.cache.firstIndex = 0;
	candidate.cache.count = count_;
	candidate.cache.startTime = 0;
	candidate.cache.interval = interval_;
	candidate.cache.outputs = {{"surface", "mesh.v2", {{"material", "m", "f32x2"}, {"normal", "1", "f32x3"},
		{"position", "m", "f32x3"}, {"velocity", "m/s", "f32x3"}}}};
	candidate.inputs = scene.names;
	Bytes records;
	if (!RISECBOR64::Encode(V::MapValue({{"format", V::String("cloth.material.v1")}, {"material", MaterialToValue(model.material)},
		{"units", MaterialUnits()}, {"damping", V::String("calibrated, not measured")}}), records, &error)) return false;
	candidate.cache.physicalRecords = records;
	candidate.ports = {
		{StatePort, "surface", "m", "mesh.v2", true, "surface",
			{{"normal", "normal", "1", 3}, {"position", "position", "m", 3}, {"velocity", "velocity", "m/s", 3}}, ""},
		{LoadPort, "surface load", "Pa", "field.v1", false, "",
			{{"force", "force", "N", 3}, {"pressure", "pressure", "Pa", 1}, {"traction", "traction", "Pa", 3}}, topology_},
		{MechanicsPort, "surface mechanics", "kg", "field.v1", true, "",
			{{"damping_mass", "damping", "1/s", 1}, {"damping_stiffness", "damping", "s", 1}, {"fixed", "fixed", "1", 1},
			 {"force", "force", "N", 3}, {"mass", "mass", "kg", 1}}, topology_},
		{StiffnessPort, "surface stiffness", "N/m", "field.v1", true, "",
			{{"column", "index", "1", 1}, {"row", "index", "1", 1}, {"stiffness", "stiffness", "N/m", 9}}, stiffnessDomain_},
		{ElementsPort, "surface elements", "N/m", "field.v1", true, "",
			{{"column", "index", "1", 1}, {"element", "index", "1", 1}, {"gradient", "force", "N", 3}, {"kind", "index", "1", 1},
			 {"row", "index", "1", 1}, {"stiffness", "stiffness", "N/m", 9}}, elementsDomain_},
		{StepsPort, "surface load steps", "N s", "field.v1", false, "",
			{{"impulse", "impulse", "N s", 3}, {"segment", "index", "1", 1}}, ""}};
	// M6: each port-driven collider accepts its surface; the reactions on all of them are offered.
	for (const auto& pc : colliders_)
		candidate.ports.push_back({pc.port, "collider surface", "m", "mesh.v2", false, "", {{"position", "position", "m", 3}}, pc.topology});
	if (!colliders_.empty())
		candidate.ports.push_back({ReactionPort, "contact reaction", "N", "field.v1", true, "",
			{{"force", "force", "N", 3}, {"point", "position", "m", 3}, {"torque", "torque", "N m", 3}}, reactionDomain_});
	auto& caps = candidate.capabilities;
	caps.backends = {{"cpu", BackendStatus::Supported, true,
		"portable C++17; `threads` (default min(4, cores)) does not change results; built and tested on macOS "
		"(with and without Metal), Windows and Android builds not yet compiled"}};
	caps.interactive = caps.deterministic = caps.resumable = caps.externalMacroStep = true;
	caps.rollback = true;
	caps.threadCountInvariant = true; // ClothParallel.h: bitwise the same for any thread count
	caps.modes = {"bake", "interactive"};
	caps.backend = "cpu";   // M4: the backend this configuration runs on
	const std::uint64_t n = built.Vertices();
	// M5c, measured (rise-sim peak footprint/RSS, 121..14,641 vertices, both modes, sphere contact):
	// about 10 MiB + 13 KiB per vertex; declared with a 25% margin.  Not keyed.
	caps.memoryEstimate = ((std::uint64_t(10) << 20) + n*13312) * 5 / 4;
	// Wall s per simulated s: per vertex-substep 12 us in bake mode (measured 7-23 us),
	// 3 us interactive (2.5-3.3 us), M1 Max, default threads.  Approximate.  Not keyed.
	caps.costEstimate = (interactive ? 3e-6 : 12e-6) * double(n) * double(substeps) * rate;
	// predictive needs bake mode, no air drag, no motion sampled from the scene,
	// and contact (if any) by barrier; the contact cases (and with
	// self-collision the self-collision cases) then join the evidence.
	const bool barrierContact = model.contact == ContactModel::Barrier && anyContact;
	const bool sceneMotion = !scene.tracks.empty();
	const bool predictiveConditions = !interactive && model.dragCoefficient == 0 && !sceneMotion &&
		(!anyContact || barrierContact);
	candidate.evidence.fidelity = predictiveConditions ? "predictive" : "preview";
	caps.earnableFidelity = predictiveConditions ? std::vector<std::string>{"preview", "predictive"} :
		std::vector<std::string>{"preview"};
	if (!predictiveConditions)
		caps.fidelityLimit = interactive ? "interactive mode accepts truncated Newton iterates" :
			model.dragCoefficient > 0 ? "air drag is explicit and unvalidated" :
			sceneMotion ? "motion sampled from the scene is not covered by the validation cases" :
			"penalty contact has no collision detection";
	// What the label covers (docs/SIMULATION_M3A_RESULTS.md section 5), as
	// conditions and one sentence; both are bound into the evidence digests.
	{
		std::vector<std::string> colliders;
		if (!model.colliders.empty()) colliders.push_back("analytic");
		bool movingMesh = false, staticMesh = false;
		for (const auto& m : model.meshColliders) (m.motion.Moving() ? movingMesh : staticMesh) = true;
		if (movingMesh) colliders.push_back("mesh-rigid-moving");
		if (staticMesh) colliders.push_back("mesh-static");
		if (colliders.empty()) colliders.push_back("none");
		std::sort(colliders.begin(), colliders.end());
		std::vector<std::string> pins{"none"};
		for (const auto& g : model.pins) if (!g.vertices.empty()) pins = {"present"};
		const char* bending = sheet.bending == BendingModel::Lines ? "lines" : "hinges";
		candidate.evidence.conditions = {
			{"bending", {bending}},
			{"colliders", colliders},
			{"contact", {!anyContact ? "none" : model.contact == ContactModel::Barrier ? "barrier" : "penalty"}},
			{"drag", {model.dragCoefficient > 0 ? "quadratic" : "none"}},
			{"integrator", {integrator}},
			{"material", {model.material.name}},
			{"mesh", {"structured-sheet"}},
			{"mode", {mode}},
			{"motion", colliders_.empty() ? std::vector<std::string>{sceneMotion ? "scene-transform" : "prescribed"} :
				std::vector<std::string>{"port", sceneMotion ? "scene-transform" : "prescribed"}},   // M6: "port" < both
			{"self_collision", {model.barrier.selfCollision && barrierContact ? "true" : "false"}},
			{"triangulation", {sheet.triangulation == Triangulation::Alternating ? "alternating" : "consistent"}}};
		std::string scope = std::string("The cloth model (structured sheet, ") + bending + " bending, the material record " +
			model.material.name + ") integrated by " + integrator + " in " + mode + " mode";
		if (barrierContact) scope += ", with barrier contact and lagged smoothed friction against analytic shapes and "
			"static or rigidly moving mesh colliders at contact parameters whose own cases pass";
		if (barrierContact && model.barrier.selfCollision) scope += ", and cloth-cloth barrier contact with lagged smoothed "
			"friction between layers (self_friction)";
		scope += ": certified on canonical problems (force balance, an analytic drape, analytic friction, an "
			"intersection-free tumble, no energy gain";
		if (barrierContact && model.barrier.selfCollision) scope += "; layers' force balance, friction between layers, an "
			"intersection-free fold";
		scope += "); it does not bound the error of a given bake and does not "
			"cover air drag, imported irregular meshes, measured fabric behaviour, deforming "
			"colliders or motion sampled from the scene";
		if (barrierContact && model.barrier.selfCollision) scope += ", nor the shape of a self-contact fold (no analytic "
			"reference; self-convergence only)";
		scope += ".";
		if (!colliders_.empty()) scope += " Colliders that follow a coupling port move rigidly as the pose fitted to the "
			"accepted surface (covered by the coupling's own evidence, not by these cases).";   // M6
		candidate.evidence.scope = scope;
	}
	// Mesh colliders from scene inputs are their geometry at t = 0 moved rigidly
	// (deforming colliders are out of scope).  Chosen: a documented
	// approximation, made explicit: the geometry is sampled again at three later
	// times of the run, and if it departs from the t = 0 geometry moved by the
	// collider's motion the run says so in its notes and is labelled preview.
	std::vector<std::string> deforming;
	for (const auto& mc : model.meshColliders) {
		if (mc.sceneInput.empty() || count_ < 2) continue;
		const double end = static_cast<double>(count_ - 1)*interval_;
		double worst = 0, at = 0;
		bool topology = false;
		for (int k = 1; k <= 3; ++k) {
			const double t = end*k/3.0;
			std::string why, codec;
			std::shared_ptr<const Bytes> bytes;
			MeshFrame frame;
			const StaticPayloadResolver resolve = [&inputs](const std::string& digest, std::size_t budget,
				std::shared_ptr<const Bytes>& fixed, std::string& reason) { return inputs.ResolveStatic(digest, budget, fixed, reason); };
			if (!inputs.Sample(mc.sceneInput, t, bytes, codec, why) || !bytes ||
				(codec != "mesh.v1" && codec != "mesh.v2") || !DecodeMesh(*bytes, CodecBudget, resolve, frame, why))
				return Fail(error, "collider scene input " + mc.sceneInput + " at t = " + std::to_string(t) + ": " + why);
			if (frame.positions.size() != mc.vertices.size() || frame.indices != mc.triangles) { topology = true; at = t; break; }
			for (std::size_t v = 0; v < mc.vertices.size()/3; ++v) {
				const V3 rest{{mc.vertices[3*v], mc.vertices[3*v + 1], mc.vertices[3*v + 2]}};
				const V3 moved = mc.motion.Position(rest, t);
				double d = 0;
				for (int c = 0; c < 3; ++c) { const double e = FromF32(frame.positions[3*v + c]) - moved[c]; d += e*e; }
				if (std::sqrt(d) > worst) { worst = std::sqrt(d); at = t; }
			}
		}
		if (topology || worst > 1e-5) {
			std::ostringstream note;
			note << "mesh collider " << mc.sceneInput << " deforms during the run (";
			if (topology) note << "its topology changes";
			else note << "it departs from its t = 0 geometry moved rigidly by up to " << worst << " m";
			note << ", at t = " << at << " s): the solver follows its t = 0 geometry moved rigidly, an approximation (preview)";
			deforming.push_back(note.str());
		}
	}
	if (!deforming.empty()) {
		candidate.evidence.fidelity = "preview";
		caps.earnableFidelity = {"preview"};
		if (caps.fidelityLimit.empty()) caps.fidelityLimit = "a deforming collider is followed as its t = 0 geometry moved rigidly";
	}
	EvidenceKey key{model.material, sheet.triangulation, sheet.bending, scheme_};
	key.contact = barrierContact;
	key.self = barrierContact && model.barrier.selfCollision;
	key.barrier = model.barrier;
	key.thickness = model.contactThickness;
	candidate.evidence.cases = Evidence(key);
	candidate.notes.push_back("material " + model.material.name + ": " + model.material.source);
	candidate.notes.push_back("Rayleigh damping is calibrated, not measured");
	if (interactive) candidate.notes.push_back("interactive mode: truncated Newton, preview only");
	if (anyContact && model.contact == ContactModel::Penalty)
		candidate.notes.push_back("penalty contact without collision detection: preview only");
	if (barrierContact)
		candidate.notes.push_back("barrier contact with continuous collision detection and lagged smoothed friction");
	if (barrierContact && model.barrier.selfCollision)
		candidate.notes.push_back("cloth-cloth barrier contact with lagged smoothed friction between layers (self_friction)");
	if (model.dragCoefficient > 0) candidate.notes.push_back("air drag is explicit and unvalidated: preview only");
	for (const auto& note : deforming) candidate.notes.push_back(note);
	d = std::move(candidate);
	return true;
}

bool ClothSolver::Start(std::string& error)
{
	if (!configured_) return Fail(error, "configure first");
	Sheet sheet;
	if (!BuildSheet(sheet_, model_.material, sheet, error) || !system_.Initialize(sheet, model_, error)) return false;
	system_.SetThreads(threads_);
	system_.Reset(0);
	for (auto& pc : colliders_) {   // M6: port-driven colliders start at rest
		pc.track->start = pc.track->end = {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}};
		pc.track->t0 = pc.track->t1 = 0;
		pc.track->have = false;
		for (int a = 0; a < 3; ++a) pc.impulse[a] = pc.angular[a] = 0;
		pc.time = 0;
		pc.point = pc.track->centroid;
	}
	ObserveSteps();
	if (!(system_.ContactGap(system_.x, 0) > 0))
		return Fail(error, "the sheet starts within the contact offset of a collider (contact_thickness)");
	system_.portPressure.clear();
	system_.portTraction.clear();
	t_ = 0; next_ = 0; finished_ = count_ == 0; segN_ = segJ_ = 0; segStart_ = segTarget_ = 0;
	held_ = false; heldFor_ = 0;
	staticSent_ = false;
	stats_ = StepStats{};
	started_ = true;
	return TrackFailure(error);
}

bool ClothSolver::TrackFailure(std::string& error) const
{
	for (const auto& t : tracks_) {
		const std::string why = t->Error();
		if (!why.empty()) return Fail(error, why);
	}
	return true;
}

// mesh.v2: positions, velocities and normals per frame; the connectivity and
// the material UVs in one static topology payload.
bool ClothSolver::Encode(Bytes& frame, std::shared_ptr<const Bytes>& fixed, std::string& error)
{
	const std::uint32_t n = system_.Vertices();
	MeshFrame m;
	m.velocityUnits = "m/s";
	std::vector<double> normals;
	system_.VertexNormals(system_.x, normals);
	m.positions.reserve(3*static_cast<std::size_t>(n));
	m.velocities.reserve(3*static_cast<std::size_t>(n));
	m.normals.reserve(3*static_cast<std::size_t>(n));
	for (std::size_t k = 0; k < 3*static_cast<std::size_t>(n); ++k) {
		m.positions.push_back(F32(system_.x[k]));
		m.velocities.push_back(F32(system_.v[k]));
		m.normals.push_back(F32(normals[k]));
	}
	m.indices = indices_;
	m.uvSets.push_back({"material", "m", materialUV_});
	MeshStaticLayout layout;
	layout.topology = true;
	layout.staticUVSets = {"material"};
	if (!staticReady_) {
		if (!PrepareMeshStatic(m, layout, CodecBudget, staticRecord_, error)) return false;
		staticPayload_ = std::make_shared<const Bytes>(staticRecord_.payload);
		staticReady_ = true;
	}
	fixed = staticPayload_;
	return EncodeMeshV2(m, layout, CodecBudget, staticRecord_, frame, error);
}

bool ClothSolver::Publish(std::int64_t index, double time, IFrameSink& sink, IPortExchange* ports, std::string& error)
{
	auto bytes = std::make_shared<Bytes>();
	std::shared_ptr<const Bytes> fixed;
	std::string declared;
	if (!Encode(*bytes, fixed, error)) return false;
	// The static topology goes to the sink once per run, before the first frame.
	if (!staticSent_ && !sink.PublishStatic(MeshTopologyStaticType, fixed, declared, error)) return false;
	staticSent_ = true;
	std::shared_ptr<const Bytes> payload = std::move(bytes);
	if (!sink.Publish(index, "surface", time, payload, {}, error)) return false;
	if (ports && (!ports->OfferStatic(MeshTopologyStaticType, fixed, error) || !ports->Offer(StatePort, time, payload, error)))
		return false;
	if (ports && !OfferMechanics(time, *ports, error)) return false;   // M7e
	if (ports && !colliders_.empty() && !OfferReaction(time, *ports, error)) return false;   // M6
	return true;
}

bool ClothSolver::OfferState(double time, IPortExchange& ports, std::string& error)
{
	// m7i: with a partner's linear copy (its steps taken) the cloth continues from the binary32
	// state it offers, so the copy and the cloth start the next macro step from the same values.
	if (system_.Linear()) system_.RoundState();
	auto bytes = std::make_shared<Bytes>();
	std::shared_ptr<const Bytes> fixed;
	if (!Encode(*bytes, fixed, error)) return false;
	if (!ports.OfferStatic(MeshTopologyStaticType, fixed, error) || !ports.Offer(StatePort, time, std::move(bytes), error)) return false;
	if (!OfferMechanics(time, ports, error)) return false;   // M7e
	return colliders_.empty() || OfferReaction(time, ports, error);   // M6
}

// M7e: surface_mechanics (field.v1 over the vertices) and surface_stiffness
// (field.v1 over the Hessian's upper blocks, row <= column, in pattern order;
// the indices as binary32 integers).
bool ClothSolver::OfferMechanics(double time, IPortExchange& ports, std::string& error)
{
	std::vector<double> force;
	BlockMatrix k;
	system_.Mechanics(force, k);
	const Sheet& sheet = system_.GetSheet();
	const std::uint32_t n = system_.Vertices();
	const Material& m = model_.material;
	const double alpha = model_.damping ? m.dampingMass : 0.0, beta = model_.damping ? m.dampingStiffness : 0.0;
	FieldFrame f;
	f.domain = topology_;
	f.count = n;
	FieldChannel dm{"damping_mass", "1/s", 1, {}}, ds{"damping_stiffness", "s", 1, {}}, fx{"fixed", "1", 1, {}},
		fo{"force", "N", 3, {}}, ma{"mass", "kg", 1, {}};
	for (std::uint32_t q = 0; q < n; ++q) {
		dm.values.push_back(F32(alpha));
		ds.values.push_back(F32(beta));
		fx.values.push_back(F32(system_.Fixed(q) ? 1.0 : 0.0));
		for (int a = 0; a < 3; ++a) fo.values.push_back(F32(force[3*std::size_t(q) + std::size_t(a)]));
		ma.values.push_back(F32(sheet.mass[q]));
	}
	f.channels = {dm, ds, fx, fo, ma};
	auto bytes = std::make_shared<Bytes>();
	if (!EncodeField(f, CodecBudget, *bytes, error) || !ports.Offer(MechanicsPort, time, std::move(bytes), error)) return false;
	FieldFrame s;
	s.domain = stiffnessDomain_;
	FieldChannel col{"column", "1", 1, {}}, row{"row", "1", 1, {}}, st{"stiffness", "N/m", 9, {}};
	for (std::uint32_t i = 0; i < k.Blocks(); ++i)
		for (std::size_t e = k.RowStart()[i]; e < k.RowStart()[i + 1]; ++e) {
			const std::uint32_t j = k.Column()[e];
			if (j < i) continue;
			row.values.push_back(F32(double(i)));
			col.values.push_back(F32(double(j)));
			const double* b = k.Block(e);
			for (int c = 0; c < 9; ++c) st.values.push_back(F32(b[c]));
		}
	s.count = static_cast<std::uint32_t>(row.values.size());
	s.channels = {col, row, st};
	auto sb = std::make_shared<Bytes>();
	if (!EncodeField(s, CodecBudget, *sb, error) || !ports.Offer(StiffnessPort, time, std::move(sb), error)) return false;
	// M7h: the corotational model's elements at x (the values the cloth's own linear model takes).
	CorotatedElements elems;
	system_.LinearElements(system_.x, elems);
	FieldFrame ef;
	ef.domain = elementsDomain_;
	FieldChannel ecol{"column", "1", 1, {}}, eel{"element", "1", 1, {}}, egr{"gradient", "N", 3, {}}, eki{"kind", "1", 1, {}},
		erow{"row", "1", 1, {}}, est{"stiffness", "N/m", 9, {}};
	for (std::size_t e = 0; e < elems.Size(); ++e) {
		const CorotatedElements::Element& el = elems.elements[e];
		const double* ke = &elems.stiffness[144*e];
		for (int a = 0; a < el.count; ++a) for (int b = a; b < el.count; ++b) {
			ecol.values.push_back(F32(double(el.v[b])));
			eel.values.push_back(F32(double(e)));
			eki.values.push_back(F32(double(el.kind)));
			erow.values.push_back(F32(double(el.v[a])));
			for (int i = 0; i < 3; ++i) egr.values.push_back(a == b ? F32(elems.gradient[12*e + std::size_t(3*a + i)]) : F32(0.0));
			for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) est.values.push_back(F32(ke[(3*a + i)*12 + 3*b + j]));
		}
	}
	ef.count = static_cast<std::uint32_t>(erow.values.size());
	ef.channels = {ecol, eel, egr, eki, erow, est};
	auto eb = std::make_shared<Bytes>();
	return EncodeField(ef, CodecBudget, *eb, error) && ports.Offer(ElementsPort, time, std::move(eb), error);
}

// surface_load: field.v1 over the sheet's vertices (domain: the sheet's
// topology digest, count: its vertex count); channel "pressure" (1
// component, Pa, on the front face) and/or "traction" (3 components, Pa,
// world frame). An absent channel is zero.
bool ClothSolver::AcceptLoads(double time, IPortExchange* ports, std::string& error)
{
	if (!ports) return true;
	std::shared_ptr<const Bytes> bytes;
	const auto status = ports->Accept(LoadPort, time, bytes, error);
	if (status == PortStatus::Unbound || status == PortStatus::Unavailable) {
		system_.portPressure.clear();
		system_.portTraction.clear();
		system_.portForce.clear();
		return true;
	}
	if (status != PortStatus::Ready) return false;
	FieldFrame f;
	if (!DecodeField(*bytes, CodecBudget, f, error)) return false;
	const std::uint32_t n = system_.Vertices();
	if (f.domain != topology_ || f.count != n)
		return Fail(error, "surface_load must be a field.v1 over the sheet's vertices (domain: its topology digest)");
	std::vector<double> pressure, traction, force;
	for (const auto& c : f.channels) {
		const bool isPressure = c.name == "pressure" && c.components == 1 && c.units == "Pa";
		const bool isTraction = c.name == "traction" && c.components == 3 && c.units == "Pa";
		const bool isForce = c.name == "force" && c.components == 3 && c.units == "N";   // M7e
		if (!isPressure && !isTraction && !isForce)
			return Fail(error, "surface_load channels are force (3 components, N), pressure (1, Pa) and traction (3, Pa)");
		std::vector<double>& target = isPressure ? pressure : isTraction ? traction : force;
		target.resize(c.values.size());
		for (std::size_t k = 0; k < c.values.size(); ++k) target[k] = FromF32(c.values[k]);
	}
	system_.portPressure.swap(pressure);
	system_.portTraction.swap(traction);
	system_.portForce.swap(force);
	return true;
}

// M7e: StepsPort (unbound or unavailable: none, the held load alone).
bool ClothSolver::AcceptSteps(IPortExchange* ports, double time, std::string& error)
{
	stepImpulse_.clear();
	stepSegment_.clear();
	stepCursor_ = segmentOrdinal_ = 0;
	stepsUnbound_ = false;
	if (!ports) return true;
	std::shared_ptr<const Bytes> bytes;
	const auto status = ports->Accept(StepsPort, time, bytes, error);
	if (status == PortStatus::Unbound || status == PortStatus::Unavailable) { stepsUnbound_ = status == PortStatus::Unbound; error.clear(); return true; }
	if (status != PortStatus::Ready) return false;
	FieldFrame f;
	if (!DecodeField(*bytes, CodecBudget, f, error)) return false;
	const std::uint32_t n = system_.Vertices();
	const FieldChannel *imp = nullptr, *seg = nullptr;
	for (const auto& c : f.channels) {
		if (c.name == "impulse" && c.components == 3 && c.units == "N s") imp = &c;
		else if (c.name == "segment" && c.components == 1 && c.units == "1") seg = &c;
	}
	if (!imp || !seg || n == 0 || f.count % n != 0)
		return Fail(error, std::string(StepsPort) + " must be a field.v1 over (step, vertex) pairs with impulse (3, N s) and segment (1)");
	const std::uint32_t steps = f.count/n;
	stepImpulse_.resize(3*std::size_t(f.count));
	for (std::size_t k = 0; k < stepImpulse_.size(); ++k) stepImpulse_[k] = FromF32(imp->values[k]);
	for (std::uint32_t j = 0; j < steps; ++j) {
		const double o = FromF32(seg->values[std::size_t(j)*n]);
		if (!(o >= 0) || std::floor(o) != o || (j > 0 && o < double(stepSegment_.back())))
			return Fail(error, std::string(StepsPort) + ": invalid segment ordinals");
		stepSegment_.push_back(static_cast<std::uint32_t>(o));
	}
	return true;
}

bool ClothSolver::Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
	AdvanceResult& result, std::string& error)
{
	result = AdvanceResult{};
	if (!started_) return Fail(error, "start or restore first");
	const bool macro = request.mode == AdvanceRequest::Mode::MacroStep;
	if (macro && (!std::isfinite(request.macroEnd) || request.macroEnd < t_)) return Fail(error, "macro end lies in the past");
	const double end = static_cast<double>(count_ ? count_ - 1 : 0)*interval_; // the last output
	if (!macro) held_ = false;
	bool offered = false;
	for (;;) {
		while (!finished_ && t_ == OutputTime(next_)) {
			if (!Publish(next_, t_, sink, ports, error)) return false;
			offered = true;
			++result.framesPublished;
			if (static_cast<std::uint64_t>(++next_) == count_) finished_ = true;
			if (paceMs_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(paceMs_));
			if (!macro) return Done(result, AdvanceResult::Status::Advanced);
		}
		if (finished_) return Done(result, AdvanceResult::Status::Finished);
		if (macro && t_ == request.macroEnd && segN_ == 0) {
			// The end of the macro step: offer the state stamped with its time.
			if (!offered && ports && !OfferState(t_, *ports, error)) return false;
			return Done(result, AdvanceResult::Status::Advanced);
		}
		if (segN_ == 0) {
			segStart_ = t_;
			segTarget_ = macro ? std::min(OutputTime(next_), request.macroEnd) : OutputTime(next_);
			const double nominal = interval_/static_cast<double>(substeps_);
			segN_ = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil((segTarget_ - segStart_)/nominal*(1 - 1e-12))));
			segJ_ = 0;
			// Free-running, each segment is a step; under an imposed macro step the
			// port values are read once, at its start, and held over all of it.
			if (!macro || !held_ || heldFor_ != request.macroEnd) {
				if (!AcceptLoads(segStart_, ports, error)) return false;
				if (!AcceptColliders(segStart_, macro ? request.macroEnd : segTarget_, ports, error)) return false;
				if (!AcceptSteps(macro ? ports : nullptr, segStart_, error)) return false;   // M7e
				// M7f: a `force` channel on surface_load is an implicitly coupled sheet's load (the
				// liquid's step impulses over the macro step).  Without its steps the cloth would
				// apply their mean over its own substeps, the exchange that diverges for light cloth
				// (docs/SIMULATION_M7E_RESULTS.md section 2.1): refused.
				if (macro && stepsUnbound_ && !system_.portForce.empty())
					return Fail(error, std::string("surface_load carries an implicitly coupled sheet's force but ") + StepsPort +
						" is not connected: connect the liquid's sheet load steps port (sheet_load_steps by default) to " + StepsPort +
						" (without the steps the cloth would apply their mean over its own substeps, which diverges for light cloth)");
				// M7e: with the partner's steps, the macro step's elasticity is the partner's linear model.
				if (!stepSegment_.empty()) system_.BeginLinear();
				else system_.EndLinear();
				held_ = macro;
				heldFor_ = request.macroEnd;
			}
			if (macro && !stepSegment_.empty()) {
				// M7e: the partner's steps of this segment (the same split: equal steps).
				std::uint64_t k = 0;
				for (std::size_t j = stepCursor_; j < stepSegment_.size() && stepSegment_[j] == segmentOrdinal_; ++j) ++k;
				if (k == 0) return Fail(error, std::string(StepsPort) + " has no steps for the segment at t = " + std::to_string(segStart_) +
					" (the cloth and its partner must share their output times and macro steps)");
				segN_ = k;
			}
		} else if (macro && segTarget_ > request.macroEnd) return Fail(error, "macro end inside an interrupted segment");
		offered = false;
		while (segJ_ < segN_) {
			if (stop_.load()) {
				stop_ = false; // a stop request is cleared only when consumed
				return Done(result, AdvanceResult::Status::Stopped);
			}
			if (progress) {
				std::ostringstream detail;
				detail << "newton " << stats_.newtonIterations << ", cg " << stats_.cgIterations << ", direct "
					<< stats_.directSolves << ", halvings " << stats_.halvings << ", unconverged " << stats_.unconverged << ", projected " << stats_.projections;
				if (!progress->Report(t_, end, detail.str())) return Done(result, AdvanceResult::Status::Stopped);
			}
			const double tn = segJ_ + 1 == segN_ ? segTarget_ :
				segStart_ + (segTarget_ - segStart_)*static_cast<double>(segJ_ + 1)/static_cast<double>(segN_);
			Scheme scheme = scheme_;
			if (macro && !stepSegment_.empty()) {
				// M7e: the partner's step: its impulse as the step's force, backward Euler.
				const std::uint32_t n = system_.Vertices();
				system_.portForce.assign(stepImpulse_.begin() + std::ptrdiff_t(3*n*stepCursor_), stepImpulse_.begin() + std::ptrdiff_t(3*n*(stepCursor_ + 1)));
				for (double& x : system_.portForce) x /= tn - t_;
				scheme = Scheme::BackwardEuler;
				++stepCursor_;
			}
			if (!system_.Step(tn - t_, scheme, newton_, stats_, error) || !TrackFailure(error)) return false;
			system_.time = tn;
			t_ = tn;
			++segJ_;
		}
		segN_ = segJ_ = 0;
		if (macro && !stepSegment_.empty()) ++segmentOrdinal_;   // M7e
	}
}

// ------------------------------------------------------------------ M6: port-driven colliders

// At the start of a held step [t0, t1]: each port-driven collider accepts the
// surface it must reach by t1 (a mesh.v2 or mesh.v1 of its own topology); it
// moves there from the pose it held.  Unbound or unavailable: it stays.  The
// reaction accumulators restart.
bool ClothSolver::AcceptColliders(double t0, double t1, IPortExchange* ports, std::string& error)
{
	for (auto& pc : colliders_) {
		PortTrack& tr = *pc.track;
		const std::array<double,12> previous = tr.have ? tr.end : std::array<double,12>{{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}};
		std::array<double,12> next = previous;
		if (ports) {
			std::shared_ptr<const Bytes> bytes;
			const auto status = ports->Accept(pc.port, t0, bytes, error);
			if (status == PortStatus::Failed) return false;
			if (status == PortStatus::Ready) {
				MeshFrame m;
				if (!DecodeMesh(*bytes, CodecBudget, ports->Resolver(), m, error)) return Fail(error, pc.port + ": " + error);
				if (m.positionUnits != "m" || m.positions.size() != pc.rest.size() ||
					MeshTopologyDigest(static_cast<std::uint32_t>(pc.rest.size()/3), m.indices) != pc.topology)
					return Fail(error, pc.port + " must carry the collider's own surface (its topology, metres)");
				std::vector<double> moved(m.positions.size());
				for (std::size_t k = 0; k < moved.size(); ++k) moved[k] = FromF32(m.positions[k]);
				next = FitRigid(pc.rest, moved);
			}
		}
		tr.start = previous;
		tr.end = next;
		tr.t0 = t0;
		tr.t1 = t1;
		tr.have = true;
		for (int a = 0; a < 3; ++a) pc.impulse[a] = pc.angular[a] = 0;
		pc.time = 0;
		for (int a = 0; a < 3; ++a)
			pc.point[a] = next[3*a]*tr.centroid[0] + next[3*a + 1]*tr.centroid[1] + next[3*a + 2]*tr.centroid[2] + next[9 + a];
	}
	return true;
}

// After every accepted step: the contact force each port-driven collider
// exerted on the cloth (barrier and friction, ClothContact's MeshForces on the
// step's own frame), integrated over the step, with its moment about `point`.
void ClothSolver::ObserveSteps()
{
	if (colliders_.empty()) { system_.stepObserver = nullptr; return; }
	system_.stepObserver = [this](const ContactFrame& f, const std::vector<double>& x, double h) {
		const BarrierContact* barrier = system_.Barrier();
		if (!barrier) return;
		std::vector<double> force;
		for (auto& pc : colliders_) {
			barrier->MeshForces(f, x, static_cast<std::uint32_t>(pc.mesh), force);
			for (std::size_t i = 0; i < force.size()/3; ++i) {
				const double* fi = &force[3*i];
				if (fi[0] == 0 && fi[1] == 0 && fi[2] == 0) continue;
				const double r[3] = {x[3*i] - pc.point[0], x[3*i + 1] - pc.point[1], x[3*i + 2] - pc.point[2]};
				const double m[3] = {r[1]*fi[2] - r[2]*fi[1], r[2]*fi[0] - r[0]*fi[2], r[0]*fi[1] - r[1]*fi[0]};
				for (int a = 0; a < 3; ++a) { pc.impulse[a] += h*fi[a]; pc.angular[a] += h*m[a]; }
			}
			pc.time += h;
		}
	};
}

// collider_reaction: field.v1 over the port-driven colliders (declaration
// order); per collider the mean force the cloth exerted ON it over the held
// step so far (N), the point the torque is about (m) and that torque (N m).
bool ClothSolver::OfferReaction(double time, IPortExchange& ports, std::string& error)
{
	FieldFrame f;
	f.domain = reactionDomain_;
	f.count = static_cast<std::uint32_t>(colliders_.size());
	FieldChannel force{"force", "N", 3, {}}, point{"point", "m", 3, {}}, torque{"torque", "N m", 3, {}};
	for (const auto& pc : colliders_) {
		const double inv = pc.time > 0 ? 1/pc.time : 0.0;
		for (int a = 0; a < 3; ++a) {
			force.values.push_back(F32(-pc.impulse[a]*inv));
			point.values.push_back(F32(pc.point[a]));
			torque.values.push_back(F32(-pc.angular[a]*inv));
		}
	}
	f.channels = {force, point, torque};
	auto bytes = std::make_shared<Bytes>();
	if (!EncodeField(f, CodecBudget, *bytes, error)) return false;
	return ports.Offer(ReactionPort, time, std::move(bytes), error);
}

std::vector<double> ClothSolver::SaveColliders() const
{
	std::vector<double> out;
	for (const auto& pc : colliders_) {
		out.insert(out.end(), pc.track->start.begin(), pc.track->start.end());
		out.insert(out.end(), pc.track->end.begin(), pc.track->end.end());
		out.insert(out.end(), {pc.track->t0, pc.track->t1, pc.track->have ? 1.0 : 0.0});
		out.insert(out.end(), {pc.impulse[0], pc.impulse[1], pc.impulse[2], pc.angular[0], pc.angular[1], pc.angular[2], pc.time,
			pc.point[0], pc.point[1], pc.point[2]});
	}
	return out;
}

bool ClothSolver::LoadColliders(const std::vector<double>& s, std::string& error)
{
	if (s.size() != PortCollider::Size*colliders_.size()) return Fail(error, "cloth solver state does not match the configured port colliders");
	std::size_t k = 0;
	for (auto& pc : colliders_) {
		for (double& x : pc.track->start) x = s[k++];
		for (double& x : pc.track->end) x = s[k++];
		pc.track->t0 = s[k++]; pc.track->t1 = s[k++]; pc.track->have = s[k++] != 0;
		for (int a = 0; a < 3; ++a) pc.impulse[a] = s[k++];
		for (int a = 0; a < 3; ++a) pc.angular[a] = s[k++];
		pc.time = s[k++];
		for (int a = 0; a < 3; ++a) pc.point[a] = s[k++];
	}
	return true;
}

ClothState ClothSolver::Capture() const
{
	ClothState c;
	c.t = t_; c.segStart = segStart_; c.segTarget = segTarget_; c.heldFor = heldFor_;
	c.hPrev = system_.hPrev; c.time = system_.time;
	c.next = static_cast<std::uint64_t>(next_); c.segN = segN_; c.segJ = segJ_;
	c.finished = finished_; c.held = held_; c.havePrev = system_.havePrev;
	c.x = system_.x; c.v = system_.v; c.xPrev = system_.xPrev; c.vPrev = system_.vPrev;
	c.pressure = system_.portPressure; c.traction = system_.portTraction;
	c.force = system_.portForce;
	c.stepImpulse = stepImpulse_; c.stepSegment = stepSegment_; c.stepCursor = stepCursor_; c.segmentOrdinal = segmentOrdinal_;
	if (system_.Linear()) c.linearX0 = system_.LinearOrigin();
	c.colliders = SaveColliders();
	return c;
}

// A fresh system from the configuration with the captured state, exactly as
// a restored checkpoint (so a rollback repeats an uninterrupted run).
bool ClothSolver::Apply(ClothState&& c, std::string& error)
{
	if (c.next > count_ || c.segJ > c.segN) return Fail(error, "invalid cloth solver state fields");
	Sheet sheet;
	ClothSystem system;
	if (!BuildSheet(sheet_, model_.material, sheet, error) || !system.Initialize(sheet, model_, error)) return false;
	system.SetThreads(threads_);
	const std::size_t dofs = 3*static_cast<std::size_t>(sheet.Vertices());
	if (c.x.size() != dofs || c.v.size() != dofs || (!c.xPrev.empty() && c.xPrev.size() != dofs) ||
		(!c.vPrev.empty() && c.vPrev.size() != dofs) || (!c.pressure.empty() && c.pressure.size() != dofs/3) ||
		(!c.traction.empty() && c.traction.size() != dofs) || (!c.force.empty() && c.force.size() != dofs) ||
		(c.havePrev && (c.xPrev.empty() || c.vPrev.empty())))
		return Fail(error, "cloth solver state does not match the configured sheet");
	system.x.swap(c.x); system.v.swap(c.v); system.xPrev.swap(c.xPrev); system.vPrev.swap(c.vPrev);
	system.hPrev = c.hPrev; system.havePrev = c.havePrev; system.time = c.time;
	system.portPressure.swap(c.pressure); system.portTraction.swap(c.traction);
	system.portForce.swap(c.force);
	if (!c.linearX0.empty()) {
		if (c.linearX0.size() != dofs) return Fail(error, "cloth solver state does not match the configured sheet (linear model)");
		system.BeginLinearAt(c.linearX0);
	}
	system_ = std::move(system);
	if (!LoadColliders(c.colliders, error)) return false;   // M6
	ObserveSteps();
	t_ = c.t; next_ = static_cast<std::int64_t>(c.next); finished_ = c.finished;
	segStart_ = c.segStart; segTarget_ = c.segTarget; segN_ = c.segN; segJ_ = c.segJ;
	held_ = c.held; heldFor_ = c.heldFor;
	stepImpulse_ = std::move(c.stepImpulse); stepSegment_ = std::move(c.stepSegment);
	stepCursor_ = c.stepCursor; segmentOrdinal_ = c.segmentOrdinal;
	if (stepImpulse_.size() != dofs*stepSegment_.size() || stepCursor_ > stepSegment_.size())
		return Fail(error, "cloth solver state does not match the configured sheet (load steps)");
	stats_ = StepStats{};
	started_ = true;
	return true;
}

bool ClothSolver::Checkpoint(Bytes& state, std::string& error)
{
	if (!started_) return Fail(error, "nothing to checkpoint");
	V::Members m{{"format", V::String(StateFormat)}, {"t", V::Float(t_)}, {"next", V::Unsigned(static_cast<std::uint64_t>(next_))},
		{"finished", V::Bool(finished_)}, {"segStart", V::Float(segStart_)}, {"segTarget", V::Float(segTarget_)},
		{"segN", V::Unsigned(segN_)}, {"segJ", V::Unsigned(segJ_)}, {"held", V::Bool(held_)}, {"heldFor", V::Float(heldFor_)},
		{"x", V::BytesValue(PackDoubles(system_.x))}, {"v", V::BytesValue(PackDoubles(system_.v))},
		{"xPrev", V::BytesValue(PackDoubles(system_.xPrev))}, {"vPrev", V::BytesValue(PackDoubles(system_.vPrev))},
		{"hPrev", V::Float(system_.hPrev)}, {"havePrev", V::Bool(system_.havePrev)}, {"time", V::Float(system_.time)},
		{"pressure", V::BytesValue(PackDoubles(system_.portPressure))},
		{"traction", V::BytesValue(PackDoubles(system_.portTraction))}};
	if (!colliders_.empty()) m.emplace_back("port_colliders", V::BytesValue(PackDoubles(SaveColliders())));   // M6
	// M7e: the held per-vertex force, only when one is held (earlier states are unchanged).
	if (!system_.portForce.empty()) m.emplace_back("force", V::BytesValue(PackDoubles(system_.portForce)));
	if (!stepSegment_.empty()) {
		std::vector<double> seg(stepSegment_.begin(), stepSegment_.end());
		m.emplace_back("load_steps", V::BytesValue(PackDoubles(stepImpulse_)));
		m.emplace_back("load_step_segments", V::BytesValue(PackDoubles(seg)));
		m.emplace_back("load_step_cursor", V::Unsigned(stepCursor_));
		m.emplace_back("load_step_segment", V::Unsigned(segmentOrdinal_));
	}
	if (system_.Linear()) m.emplace_back("linear_x0", V::BytesValue(PackDoubles(system_.LinearOrigin())));
	return RISECBOR64::Encode(V::MapValue(m), state, &error);
}

bool ClothSolver::Restore(const Bytes& state, std::string& error)
{
	if (!configured_) return Fail(error, "configure before restore");
	V v;
	if (!CanDecodeCanonicalRecord(state, CodecBudget) || !RISECBOR64::DecodeCanonical(state, v, &error) || v.GetType() != V::Map)
		return Fail(error, "invalid cloth solver state");
	auto real = [&](const char* k, double& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::Float64) return false; x = f->GetFloat(); return true; };
	auto count = [&](const char* k, std::uint64_t& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::UnsignedInteger) return false; x = f->GetIntegerArgument(); return true; };
	auto flag = [&](const char* k, bool& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::Boolean) return false; x = f->GetBoolean(); return true; };
	const V* format = v.Find("format");
	ClothState c;
	if (!format || format->GetType() != V::Text || format->GetText() != StateFormat || !real("t", c.t) || !count("next", c.next) ||
		!flag("finished", c.finished) || !real("segStart", c.segStart) || !real("segTarget", c.segTarget) ||
		!count("segN", c.segN) || !count("segJ", c.segJ) || !flag("held", c.held) || !real("heldFor", c.heldFor) ||
		!real("hPrev", c.hPrev) || !flag("havePrev", c.havePrev) || !real("time", c.time))
		return Fail(error, "invalid cloth solver state fields");
	const std::size_t dofs = 3*static_cast<std::size_t>(sheet_.verticesU)*sheet_.verticesV;
	if (!UnpackDoubles(v.Find("x"), dofs, c.x, false) || !UnpackDoubles(v.Find("v"), dofs, c.v, false) ||
		!UnpackDoubles(v.Find("xPrev"), dofs, c.xPrev, true) || !UnpackDoubles(v.Find("vPrev"), dofs, c.vPrev, true) ||
		!UnpackDoubles(v.Find("pressure"), dofs/3, c.pressure, true) || !UnpackDoubles(v.Find("traction"), dofs, c.traction, true))
		return Fail(error, "cloth solver state does not match the configured sheet");
	if (!colliders_.empty() && !UnpackDoubles(v.Find("port_colliders"), PortCollider::Size*colliders_.size(), c.colliders, false))
		return Fail(error, "cloth solver state does not match the configured port colliders");
	if (v.Find("force") && !UnpackDoubles(v.Find("force"), dofs, c.force, false))
		return Fail(error, "cloth solver state does not match the configured sheet (force)");
	if (v.Find("load_step_segments")) {
		const V* segs = v.Find("load_step_segments");
		std::vector<double> seg;
		if (segs->GetType() != V::ByteString || segs->GetBytes().size() % 8 != 0 || !UnpackDoubles(segs, segs->GetBytes().size()/8, seg, false) ||
			!UnpackDoubles(v.Find("load_steps"), dofs*seg.size(), c.stepImpulse, false) || !count("load_step_cursor", c.stepCursor) ||
			!count("load_step_segment", c.segmentOrdinal))
			return Fail(error, "cloth solver state does not match the configured sheet (load steps)");
		for (double x : seg) c.stepSegment.push_back(static_cast<std::uint32_t>(x));
	}
	if (v.Find("linear_x0") && !UnpackDoubles(v.Find("linear_x0"), dofs, c.linearX0, false))
		return Fail(error, "cloth solver state does not match the configured sheet (linear model)");
	if (!Apply(std::move(c), error)) return false;
	staticSent_ = false;
	return true;
}

bool ClothSolver::SaveState(std::shared_ptr<const SolverStateToken>& token, std::string& error)
{
	if (!started_) return Fail(error, "nothing to save");
	auto t = std::make_shared<ClothToken>();
	t->owner = this;
	t->state = Capture();
	token = std::move(t);
	return true;
}

bool ClothSolver::RestoreState(const SolverStateToken& token, std::string& error)
{
	const auto* t = dynamic_cast<const ClothToken*>(&token);
	if (!t || t->owner != this || !configured_) return Fail(error, "state token belongs to another solver");
	ClothState copy = t->state;
	return Apply(std::move(copy), error);
}
} // namespace

bool RegisterClothSolver(std::string& error)
{
	static std::mutex once;
	static bool registered = false;
	std::lock_guard<std::mutex> hold(once);
	if (registered) return true;
	if (!RegisterSolver(ClothSolverId, []{ return std::unique_ptr<ISimulationSolver>(new ClothSolver()); }, error)) return false;
	registered = true;
	return true;
}

} }
