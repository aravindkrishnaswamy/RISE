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
#include "../SimulationField.h"
#include "../SimulationMesh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
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
constexpr const char* SolverVersion = "m3a.3";
constexpr const char* StateFormat = "cloth.shell.state.v2";
constexpr const char* LoadPort = "surface_load";
constexpr const char* StatePort = "surface_state";

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
			if (!ParseMotion(r.Raw("motion"), mesh.motion, motionValue, scene, error)) return false;
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

struct EvidenceKey {
	Material material;
	Triangulation triangulation;
	BendingModel bending;
	Scheme scheme;
	bool contact = false;          // barrier contact cases too
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
	if (!contactPart) {
		s.resolution = 20;
		const CaseResult cat = RunCatenary(s);
		add(EvidenceSpecs[0], 20, cat, cat.error);
		CaseResult cw[3], cf;
		const int levels[3] = {10, 20, 40};
		for (int l = 0; l < 3; ++l) { s.resolution = levels[l]; cw[l] = RunCantilever(s, false); }
		s.resolution = 20;
		cf = RunCantilever(s, true);
		add(EvidenceSpecs[1], 20, cw[1], cw[1].error);
		add(EvidenceSpecs[2], 20, cf, cf.error);
		s.resolution = 16;
		const CaseResult m11 = RunMembraneMode(s, 1, 1), m21 = RunMembraneMode(s, 2, 1);
		add(EvidenceSpecs[3], 16, m11, m11.error);
		add(EvidenceSpecs[4], 16, m21, m21.error);
		const CaseResult plate = RunPlate(s, false);
		add(EvidenceSpecs[5], 16, plate, plate.error);
		const CaseResult energy = RunEnergyDrift(s);
		add(EvidenceSpecs[6], 16, energy, energy.error);
		CaseResult order = cw[2];
		order.ok = cw[0].ok && cw[1].ok && cw[2].ok;
		if (!order.ok) order.failure = "a cantilever level failed";
		const double pc = std::min(ObservedOrder(cw[0].error, cw[1].error), ObservedOrder(cw[1].error, cw[2].error));
		add(EvidenceSpecs[7], 40, order, std::max(0.0, 2 - pc));
		s.resolution = 8;
		CaseResult m8 = RunMembraneMode(s, 1, 1);
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
		s.resolution = 20;
		const CaseResult rest = RunContactRest(s, c);
		add(ContactSpecs[0], 20, rest, rest.error);
		s.resolution = 40;
		const CaseResult cyl = RunContactCylinder(s, c);
		add(ContactSpecs[1], 40, cyl, cyl.error);
		s.resolution = 10;
		const double mu = c.barrier.friction;
		CaseResult stick = RunContactIncline(s, c, 0.5*std::atan(mu));
		if (!(mu > 0)) { stick.ok = false; stick.failure = "no friction"; }
		add(ContactSpecs[2], 10, stick, stick.error);
		const CaseResult slide = RunContactIncline(s, c, std::atan(mu) + 0.2);
		add(ContactSpecs[3], 10, slide, slide.error);
		const CaseResult mesh = RunContactMesh(s, c, false, 0.5);
		add(ContactSpecs[4], 10, mesh, mesh.error);
		const CaseResult bounce = RunContactBounce(s, c);
		add(ContactSpecs[5], 10, bounce, bounce.error);
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

// Everything that determines later payloads (checkpoint and rollback).
struct ClothState {
	double t = 0, segStart = 0, segTarget = 0, heldFor = 0, hPrev = 0, time = 0;
	std::uint64_t next = 0, segN = 0, segJ = 0;
	bool finished = false, held = false, havePrev = false;
	std::vector<double> x, v, xPrev, vPrev, pressure, traction;
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
	bool AcceptLoads(double time, IPortExchange* ports, std::string& error);
	ClothState Capture() const;
	bool Apply(ClothState&& state, std::string& error);
	bool TrackFailure(std::string& error) const;

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
	staticReady_ = false;   // the static record is rebuilt for this configuration on first use
	tracks_ = scene.tracks;
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
			{{"pressure", "pressure", "Pa", 1}, {"traction", "traction", "Pa", 3}}, topology_}};
	auto& caps = candidate.capabilities;
	caps.backends = {{"cpu", BackendStatus::Supported, true,
		"portable C++17; `threads` (default min(4, cores)) does not change results"}};
	caps.interactive = caps.deterministic = caps.resumable = caps.externalMacroStep = true;
	caps.rollback = true;
	caps.threadCountInvariant = true; // ClothParallel.h: bitwise the same for any thread count
	caps.modes = {"bake", "interactive"};
	caps.backend = "cpu";   // M4: the backend this configuration runs on
	const std::uint64_t n = built.Vertices();
	// About a dozen 3-vectors of doubles per vertex (state, history, Newton work)
	// plus three block matrices (pattern, damping, Hessian) of ~13 3x3 blocks per row.
	caps.memoryEstimate = n*(12*24 + 3*13*72) + (std::uint64_t(1) << 20);
	// predictive needs bake mode, no air drag, and contact (if any) by barrier
	// without self-collision; the contact cases then join the evidence.
	const bool barrierContact = model.contact == ContactModel::Barrier && anyContact;
	const bool sceneMotion = !scene.tracks.empty();
	const bool predictiveConditions = !interactive && model.dragCoefficient == 0 && !sceneMotion &&
		(!anyContact || (barrierContact && !model.barrier.selfCollision));
	candidate.evidence.fidelity = predictiveConditions ? "predictive" : "preview";
	caps.earnableFidelity = predictiveConditions ? std::vector<std::string>{"preview", "predictive"} :
		std::vector<std::string>{"preview"};
	if (!predictiveConditions)
		caps.fidelityLimit = interactive ? "interactive mode accepts truncated Newton iterates" :
			model.dragCoefficient > 0 ? "air drag is explicit and unvalidated" :
			sceneMotion ? "motion sampled from the scene is not covered by the validation cases" :
			model.contact == ContactModel::Penalty ? "penalty contact has no collision detection" :
			"self-collision is not validated";
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
			{"motion", {sceneMotion ? "scene-transform" : "prescribed"}},
			{"self_collision", {model.barrier.selfCollision && barrierContact ? "true" : "false"}},
			{"triangulation", {sheet.triangulation == Triangulation::Alternating ? "alternating" : "consistent"}}};
		std::string scope = std::string("The cloth model (structured sheet, ") + bending + " bending, the material record " +
			model.material.name + ") integrated by " + integrator + " in " + mode + " mode";
		if (barrierContact) scope += ", with barrier contact and lagged smoothed friction against analytic shapes and "
			"static or rigidly moving mesh colliders at contact parameters whose own cases pass";
		scope += ": certified on canonical problems (force balance, an analytic drape, analytic friction, an "
			"intersection-free tumble, no energy gain); it does not bound the error of a given bake and does not "
			"cover self-collision, air drag, imported irregular meshes, measured fabric behaviour, deforming "
			"colliders or motion sampled from the scene.";
		candidate.evidence.scope = scope;
	}
	EvidenceKey key{model.material, sheet.triangulation, sheet.bending, scheme_};
	key.contact = barrierContact;
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
		candidate.notes.push_back("self-collision is not validated: preview only");
	if (model.dragCoefficient > 0) candidate.notes.push_back("air drag is explicit and unvalidated: preview only");
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
	return true;
}

bool ClothSolver::OfferState(double time, IPortExchange& ports, std::string& error)
{
	auto bytes = std::make_shared<Bytes>();
	std::shared_ptr<const Bytes> fixed;
	if (!Encode(*bytes, fixed, error)) return false;
	return ports.OfferStatic(MeshTopologyStaticType, fixed, error) && ports.Offer(StatePort, time, std::move(bytes), error);
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
		return true;
	}
	if (status != PortStatus::Ready) return false;
	FieldFrame f;
	if (!DecodeField(*bytes, CodecBudget, f, error)) return false;
	const std::uint32_t n = system_.Vertices();
	if (f.domain != topology_ || f.count != n)
		return Fail(error, "surface_load must be a field.v1 over the sheet's vertices (domain: its topology digest)");
	std::vector<double> pressure, traction;
	for (const auto& c : f.channels) {
		const bool isPressure = c.name == "pressure" && c.components == 1;
		const bool isTraction = c.name == "traction" && c.components == 3;
		if ((!isPressure && !isTraction) || c.units != "Pa")
			return Fail(error, "surface_load channels are pressure (1 component, Pa) and traction (3, Pa)");
		std::vector<double>& target = isPressure ? pressure : traction;
		target.resize(c.values.size());
		for (std::size_t k = 0; k < c.values.size(); ++k) target[k] = FromF32(c.values[k]);
	}
	system_.portPressure.swap(pressure);
	system_.portTraction.swap(traction);
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
				held_ = macro;
				heldFor_ = request.macroEnd;
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
			if (!system_.Step(tn - t_, scheme_, newton_, stats_, error) || !TrackFailure(error)) return false;
			system_.time = tn;
			t_ = tn;
			++segJ_;
		}
		segN_ = segJ_ = 0;
	}
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
		(!c.traction.empty() && c.traction.size() != dofs) || (c.havePrev && (c.xPrev.empty() || c.vPrev.empty())))
		return Fail(error, "cloth solver state does not match the configured sheet");
	system.x.swap(c.x); system.v.swap(c.v); system.xPrev.swap(c.xPrev); system.vPrev.swap(c.vPrev);
	system.hPrev = c.hPrev; system.havePrev = c.havePrev; system.time = c.time;
	system.portPressure.swap(c.pressure); system.portTraction.swap(c.traction);
	system_ = std::move(system);
	t_ = c.t; next_ = static_cast<std::int64_t>(c.next); finished_ = c.finished;
	segStart_ = c.segStart; segTarget_ = c.segTarget; segN_ = c.segN; segJ_ = c.segJ;
	held_ = c.held; heldFor_ = c.heldFor;
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
