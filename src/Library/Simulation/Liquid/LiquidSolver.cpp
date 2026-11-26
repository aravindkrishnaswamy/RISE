//////////////////////////////////////////////////////////////////////
//
//  LiquidSolver.cpp - "liquid.mac" on the solver contract (simulation M7)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidSolver.h"
#include "LiquidModel.h"
#include "LiquidValidation.h"
#include "../SimulationField.h"
#include "../SimulationGrid.h"
#include "../SimulationMesh.h"
#include "../SimulationPoints.h"
#include "../SimulationTransform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <thread>

namespace RISE { namespace Simulation {

namespace {
using V = RISECBOR64::Value;
using namespace Liquid;

constexpr std::size_t CodecBudget = std::size_t(16) << 30;
constexpr std::size_t InputBudget = std::size_t(256) << 20;
constexpr const char* SolverVersion = "m7c.1";
// M7c: the per-step cost with viscosity relative to without (measured, section 8 of the results).
constexpr double ViscousCostFactor = 2.8;
constexpr const char* StateMagic = "RISELIQ1";
constexpr const char* StateFormat = "liquid.mac.state.v1";
constexpr const char* SurfaceOutput = "surface";
constexpr const char* FieldsOutput = "fields";
constexpr const char* ParticlesOutput = "particles";
constexpr const char* FieldsPort = "fields";
// M7b: rigid bodies.
constexpr const char* BodiesOutput = "bodies";
constexpr const char* StatePort = "body_state";
constexpr const char* MassPort = "body_mass_properties";
constexpr const char* LoadPort = "body_load";
constexpr std::size_t InteractiveCells = 32768;   // 32^3: interactive below this size

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

double FromF32(std::uint32_t bits)
{
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

// Strict map reader: unknown keys are refused; read keys are written back
// with canonical types into `out` (as the rigid solver's).
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
	bool Bool(const char* key, bool fallback) {
		bool b = fallback;
		if (const V* v = Use(key)) {
			if (v->GetType() == V::Boolean) b = v->GetBoolean();
			else Bad(std::string(key) + " must be a boolean");
		}
		out.emplace_back(key, V::Bool(b));
		return b;
	}
	std::uint64_t Count(const char* key, std::uint64_t fallback, std::uint64_t low, std::uint64_t high, bool effective = true) {
		std::uint64_t n = fallback;
		if (const V* v = Use(key)) {
			double x = 0;
			if (v->GetType() == V::UnsignedInteger) n = v->GetIntegerArgument();
			else if (Number(*v, x) && x >= 0 && std::floor(x) == x && x < 9e15) n = static_cast<std::uint64_t>(x);
			else Bad(std::string(key) + " must be a count");
		}
		if (n < low || n > high) Bad(std::string(key) + " out of range");
		if (effective) out.emplace_back(key, V::Unsigned(n));
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
	V3 Vector(const char* key, const V3& fallback, bool effective = true) {
		V3 a = fallback;
		if (const V* v = Use(key)) {
			bool good = v->GetType() == V::Array && v->GetArray().size() == 3;
			for (std::size_t k = 0; good && k < 3; ++k) good = Number(v->GetArray()[k], a[k]);
			if (!good) Bad(std::string(key) + " must be an array of 3 finite numbers");
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

V3 Normalised(const V3& a)
{
	const double l = std::sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
	return l > 0 ? V3{{a[0]/l, a[1]/l, a[2]/l}} : a;
}

// A liquid region: {type: box, min, max} | {type: sphere, center, radius} | {type: fill, height} (y below height).
bool ParseLiquid(const V& item, Shape& s, V& effective, std::string& error)
{
	Reader r(item, "liquid", error);
	const std::string type = r.Text("type", "box", {"box", "sphere", "fill"});
	if (type == "box") {
		s.type = Shape::Type::Box;
		s.lo = r.Vector("min", {{0, 0, 0}});
		s.hi = r.Vector("max", {{0.1, 0.1, 0.1}});
		if (!(s.hi[0] > s.lo[0] && s.hi[1] > s.lo[1] && s.hi[2] > s.lo[2])) return r.Bad("a box needs max > min on every axis");
	} else if (type == "sphere") {
		s.type = Shape::Type::Sphere;
		s.point = r.Vector("center", {{0, 0, 0}});
		s.radius = r.Real("radius", 0.1);
		if (!(s.radius > 0)) return r.Bad("radius must be positive");
	} else {
		s.type = Shape::Type::Plane;
		s.point = {{0, r.Real("height", 0.1), 0}};
		s.axis = {{0, 1, 0}};
	}
	if (!r.Finish()) return false;
	effective = V::MapValue(r.out);
	return true;
}

// A static solid: plane | sphere | box | cylinder | mesh (inline or a scene input sampled at t = 0).
bool ParseSolid(const V& item, const ISceneInputs& inputs, std::vector<std::string>& sceneNames, Shape& s, V& effective,
	std::string& error)
{
	Reader r(item, "solid", error);
	const std::string type = r.Text("type", "box", {"plane", "sphere", "box", "cylinder", "mesh"});
	if (type == "plane") {
		s.type = Shape::Type::Plane;
		s.point = r.Vector("point", {{0, 0, 0}});
		const V3 n = r.Vector("normal", {{0, 1, 0}}, false);
		if (!(n[0]*n[0] + n[1]*n[1] + n[2]*n[2] > 0)) return r.Bad("plane normal must be nonzero");
		s.axis = Normalised(n);
		r.out.emplace_back("normal", Vec(s.axis));
	} else if (type == "sphere") {
		s.type = Shape::Type::Sphere;
		s.point = r.Vector("center", {{0, 0, 0}});
		s.radius = r.Real("radius", 0.1);
		if (!(s.radius > 0)) return r.Bad("radius must be positive");
	} else if (type == "box") {
		s.type = Shape::Type::Box;
		s.lo = r.Vector("min", {{0, 0, 0}});
		s.hi = r.Vector("max", {{0.1, 0.1, 0.1}});
		if (!(s.hi[0] > s.lo[0] && s.hi[1] > s.lo[1] && s.hi[2] > s.lo[2])) return r.Bad("a box needs max > min on every axis");
	} else if (type == "cylinder") {
		s.type = Shape::Type::Cylinder;
		s.point = r.Vector("center", {{0, 0, 0}});
		const V3 a = r.Vector("axis", {{0, 1, 0}}, false);
		if (!(a[0]*a[0] + a[1]*a[1] + a[2]*a[2] > 0)) return r.Bad("cylinder axis must be nonzero");
		s.axis = Normalised(a);
		r.out.emplace_back("axis", Vec(s.axis));
		s.radius = r.Real("radius", 0.1);
		s.height = r.Real("height", 0.2);
		if (!(s.radius > 0) || !(s.height > 0)) return r.Bad("cylinder radius and height must be positive");
	} else {
		s.type = Shape::Type::Mesh;
		const std::string input = r.Text("scene_input", "-", {}, false);
		if (input != "-") {
			std::string hash, codec;
			std::shared_ptr<const Bytes> bytes;
			MeshFrame frame;
			if (!inputs.Identity(input, hash, error) || !inputs.Sample(input, 0.0, bytes, codec, error) || !bytes)
				return Fail(error, "solid scene input " + input + ": " + error);
			const StaticPayloadResolver resolve = [&inputs](const std::string& digest, std::size_t budget,
				std::shared_ptr<const Bytes>& fixed, std::string& why) { return inputs.ResolveStatic(digest, budget, fixed, why); };
			if ((codec != "mesh.v1" && codec != "mesh.v2") || !DecodeMesh(*bytes, InputBudget, resolve, frame, error))
				return Fail(error, "solid scene input " + input + " is not a mesh frame: " + error);
			for (std::uint32_t bits : frame.positions) s.vertices.push_back(FromF32(bits));
			s.triangles = frame.indices;
			if (std::find(sceneNames.begin(), sceneNames.end(), input) == sceneNames.end()) sceneNames.push_back(input);
			r.out.emplace_back("scene_input", V::String(input));
			r.out.emplace_back("identity", V::String(hash));
			r.Raw("identity");
		} else {
			std::vector<double> idx;
			if (!Numbers(r.Raw("vertices"), s.vertices) || s.vertices.size() % 3 != 0 || s.vertices.empty())
				return r.Bad("mesh vertices must be a flat array of 3n finite numbers");
			if (!Numbers(r.Raw("triangles"), idx) || idx.size() % 3 != 0 || idx.empty())
				return r.Bad("mesh triangles must be a flat array of 3m vertex indices");
			for (double k : idx) {
				if (!(k >= 0) || std::floor(k) != k || k >= static_cast<double>(s.vertices.size()/3)) return r.Bad("mesh triangle index out of range");
				s.triangles.push_back(static_cast<std::uint32_t>(k));
			}
			V::Values vs, ts;
			for (double x : s.vertices) vs.push_back(V::Float(x));
			for (std::uint32_t k : s.triangles) ts.push_back(V::Unsigned(k));
			r.out.emplace_back("vertices", V::ArrayValue(vs));
			r.out.emplace_back("triangles", V::ArrayValue(ts));
		}
	}
	if (!r.Finish()) return false;
	effective = V::MapValue(r.out);
	return true;
}

// A rigid body (m7b; LiquidBodies.h): {name, shape: sphere | box | cylinder
// (body frame, centred at the centre of mass; cylinder axis z), motion:
// dynamic | prescribed | port, position, orientation (w, x, y, z), velocity,
// angular_velocity; dynamic: density or mass, free [x, y, z, rotation];
// prescribed: amplitude, frequency, phase (x(t) = position + velocity t +
// amplitude sin(2 pi frequency t + phase), a constant angular velocity);
// port: coupling implicit | staggered (the pose and twist from
// rigid.bodies' body_state; the initial state must equal rigid.bodies')}.
bool ParseBody(const V& item, const V3& gravity, LiquidBody& b, V& effective, std::string& error)
{
	Reader r(item, "body", error);
	b.name = r.Text("name", "");
	if (b.name.empty() || b.name.find('\0') != std::string::npos || !RISECBOR64::IsUTF8NFC(b.name))
		return r.Bad("a body needs a nonempty NFC name");
	const std::string shape = r.Text("shape", "sphere", {"sphere", "box", "cylinder"});
	if (shape == "sphere") {
		b.shape.type = BodyShape::Type::Sphere;
		b.shape.radius = r.Real("radius", 0.1);
		if (!(b.shape.radius > 0)) return r.Bad("radius must be positive");
	} else if (shape == "box") {
		b.shape.type = BodyShape::Type::Box;
		b.shape.size = r.Vector("size", {{0.2, 0.2, 0.2}});
		if (!(b.shape.size[0] > 0 && b.shape.size[1] > 0 && b.shape.size[2] > 0)) return r.Bad("a box size must be positive on every axis");
	} else {
		b.shape.type = BodyShape::Type::Cylinder;
		b.shape.radius = r.Real("radius", 0.1);
		b.shape.height = r.Real("height", 0.2);
		if (!(b.shape.radius > 0) || !(b.shape.height > 0)) return r.Bad("cylinder radius and height must be positive");
	}
	const std::string motion = r.Text("motion", "dynamic", {"dynamic", "prescribed", "port"});
	b.motion = motion == "dynamic" ? BodyMotion::Dynamic : motion == "prescribed" ? BodyMotion::Prescribed : BodyMotion::Port;
	const V3 position = r.Vector("position", {{0, 0, 0}});
	Quat q{{1, 0, 0, 0}};
	if (const V* v = r.Raw("orientation")) {
		std::vector<double> xs;
		if (!Numbers(v, xs) || xs.size() != 4) return r.Bad("orientation must be an array of 4 finite numbers (w, x, y, z)");
		for (int k = 0; k < 4; ++k) q[std::size_t(k)] = xs[std::size_t(k)];
	}
	if (!CanonicalQuaternion(q)) return r.Bad("orientation must be a nonzero quaternion (w, x, y, z)");
	r.out.emplace_back("orientation", V::ArrayValue({V::Float(q[0]), V::Float(q[1]), V::Float(q[2]), V::Float(q[3])}));
	const V3 velocity = r.Vector("velocity", {{0, 0, 0}});
	const V3 omega = r.Vector("angular_velocity", {{0, 0, 0}});
	double mass = 1;
	if (b.motion == BodyMotion::Dynamic) {
		if (r.Has("mass")) {
			if (r.Has("density")) return r.Bad("give density or mass, not both");
			mass = r.Real("mass", 1.0);
		} else mass = r.Real("density", 1000.0)*b.shape.Volume();
		if (!(mass > 0)) return r.Bad("density and mass must be positive");
		bool all = true;
		if (const V* v = r.Raw("free")) {
			if (v->GetType() != V::Array || v->GetArray().size() != 4) return r.Bad("free must be 4 booleans (x, y, z, rotation)");
			for (std::size_t k = 0; k < 4; ++k) {
				const V& e = v->GetArray()[k];
				if (e.GetType() != V::Boolean) return r.Bad("free must be 4 booleans (x, y, z, rotation)");
				b.free[k] = e.GetBoolean();
				all = all && b.free[k];
			}
		}
		r.out.emplace_back("free", V::ArrayValue({V::Bool(b.free[0]), V::Bool(b.free[1]), V::Bool(b.free[2]), V::Bool(b.free[3])}));
		(void)all;
	} else if (b.motion == BodyMotion::Prescribed) {
		b.amplitude = r.Vector("amplitude", {{0, 0, 0}});
		b.frequency = r.Real("frequency", 0.0);
		b.phase = r.Real("phase", 0.0);
		if (!(b.frequency >= 0)) return r.Bad("frequency must not be negative");
	} else {
		b.staggered = r.Text("coupling", "implicit", {"implicit", "staggered"}) == "staggered";
	}
	if (!r.Finish()) return false;
	b.state.mass = mass;
	b.state.moments = b.shape.Moments(mass);
	b.state.x = position;
	b.state.q = q;
	b.state.v = velocity;
	for (int a = 0; a < 3; ++a) if (b.motion == BodyMotion::Dynamic && !b.free[std::size_t(a)]) b.state.v[std::size_t(a)] = 0;
	const V3 wb = Rigid::ApplyT(Rigid::QMatrix(q), omega);
	for (int a = 0; a < 3; ++a) b.state.pi[std::size_t(a)] = (b.motion == BodyMotion::Dynamic && !b.free[3]) ? 0.0 :
		b.state.moments[std::size_t(a)]*wb[std::size_t(a)];
	b.x0 = position;
	b.v0 = velocity;
	b.q0 = q;
	b.spin = omega;
	for (int a = 0; a < 3; ++a) b.externalForce[std::size_t(a)] = b.motion == BodyMotion::Dynamic ? mass*gravity[std::size_t(a)] : 0.0;
	effective = V::MapValue(r.out);
	return true;
}

const char* MotionName(const LiquidBody& b)
{
	return b.motion == BodyMotion::Dynamic ? "dynamic" : b.motion == BodyMotion::Prescribed ? "prescribed" :
		b.staggered ? "port-staggered" : "port-implicit";
}

const char* MomentumName(MomentumScheme m)
{
	return m == MomentumScheme::Weno5 ? "weno5" : m == MomentumScheme::MacCormack ? "maccormack" : "semi_lagrangian";
}
const char* ReinitName(ReinitScheme r)
{
	switch (r) {
	case ReinitScheme::None: return "none";
	case ReinitScheme::Sussman: return "sussman";
	case ReinitScheme::RussoSmereka: return "russo_smereka";
	case ReinitScheme::RussoSmerekaCentral: return "russo_smereka_central";
	case ReinitScheme::Subcell: return "subcell";
	case ReinitScheme::Frozen: return "frozen";
	}
	return "sussman";
}

// ------------------------------------------------------------------ evidence

// The numerics that the evidence certifies (bound into every case digest).
V NumericsValue(const SolverOptions& o)
{
	return V::MapValue({{"cfl", V::Float(o.cfl)}, {"extrapolation_layers", V::Unsigned(std::uint64_t(o.extrapolationLayers))},
		{"interface_extrapolation_layers", V::Unsigned(std::uint64_t(o.interfaceExtrapolationLayers))},
		{"linear_extrapolation_layers", V::Unsigned(std::uint64_t(o.linearExtrapolationLayers))},
		{"min_face_fraction", V::Float(o.minFaceFraction)}, {"min_transport_fraction", V::Float(o.minTransportFraction)}, {"speed_limit", V::Float(o.speedLimit)},
		{"momentum", V::String(MomentumName(o.momentum))}, {"momentum_stages", V::Unsigned(std::uint64_t(o.momentumStages))},
		{"particles", V::Bool(o.particles)}, {"particles_per_cell", V::Unsigned(std::uint64_t(o.particle.perCell))},
		{"pressure_tolerance", V::Float(o.pressure.tolerance)}, {"reinit", V::String(ReinitName(o.reinit))},
		{"reinit_cfl", V::Float(o.reinitCfl)}, {"reinit_iterations", V::Unsigned(std::uint64_t(o.reinitIterations))},
		{"reinit_threshold", V::Float(o.reinitThreshold)}, {"theta_min", V::Float(o.pressure.thetaMin)},
		{"volume_correction", V::Bool(o.volumeCorrection)}, {"wave_cfl", V::Float(o.waveCfl)}});
}

struct CaseSpec { const char* id; const char* description; double tolerance; double withParticles = 0; };
// Tolerances: 1.25 x the error observed with the default numerics at the
// evidence resolution, rounded up (docs/SIMULATION_M7A_RESULTS.md section 5);
// orders: error = max(0, 1.8 - observed order) against 0.2; the rest at the
// pressure solver's tolerance level.
const CaseSpec Cases[] = {
	{"transport-zalesak-n50", "Zalesak slotted disk, one revolution, 50 x 50: symmetric-difference area / perimeter (m)", 8.4e-3, 4.3e-3},
	{"transport-vortex-n50", "single vortex, T = 2, 50 x 50: symmetric-difference area / perimeter (m)", 4.5e-3, 3.2e-3},
	{"hydrostatic-n32", "tilted tank at rest with an immersed sphere, 32 x 32, 0.25 s: max |u| / sqrt(g h)", 1e-7},
	{"hydrostatic-pressure-n32", "same: max |p - rho g depth| / (rho g H)", 1e-7},
	{"wave-finite-n32", "standing wave kd = 1.178, ka = 0.01, 32 cells per wavelength, 3 periods: |omega error| / omega", 4.8e-3},
	{"wave-deep-n32", "standing wave kd = pi, ka = 0.01, 32 cells per wavelength, 3 periods: |omega error| / omega", 4.0e-3},
	{"order-wave", "standing wave kd = 1.178, 16 -> 32 cells: max(0, 1.8 - observed order)", 0.2},
	{"slosh-n20", "3-D rectangular tank 1 x 0.6, depth 0.5, 20 cells across, 2 periods: max |omega error| / omega of modes (1,0), (0,1)", 6.9e-3},
	{"collapse-n32", "early-time column collapse, 32 cells per height: max |a - a_series| / g at least H/4 from the corner", 1.6e-3},
	{"order-collapse", "early-time column collapse, 16 -> 32: max(0, 1.8 - observed order)", 0.2},
	{"solid-collapse-n32", "early-time collapse against two cut-cell solid planes rotated 0.35 rad off the grid, 32 cells per height: max |a - a_series| / g at least H/4 from the corner", 1.8e-2},
	{"solid-pressure-n32", "same: max |p - p_series| / (rho g H) in the column", 2.0e-4},
	{"taylor-green-n32", "Taylor-Green vortex, closed box, 32 x 32, T = 2: rms face velocity error / max speed", 3.8e-4},
	{"order-taylor-green", "Taylor-Green, 16 -> 32: max(0, 1.8 - observed order)", 0.2},
	{"volume-correction-n32", "sloshing ka = 0.1, 32 cells, 10 periods: mean |volume-correction shift| / h", 1.5e-4}};

std::vector<EvidenceCase> ComputeEvidence(const SolverOptions& options, unsigned threads)
{
	static std::mutex mutex;
	static std::map<std::string, std::vector<EvidenceCase>> memo;
	Bytes key;
	std::string ignored;
	RISECBOR64::Encode(NumericsValue(options), key, &ignored);
	const std::string digest = RISECBOR64::SHA256Hex(key);
	{
		std::lock_guard<std::mutex> hold(mutex);
		const auto it = memo.find(digest);
		if (it != memo.end()) return it->second;
	}
	Pool pool(threads);
	std::map<std::string, double> error;
	{
		TransportOptions t;
		t.reinit = options.reinit;
		t.reinitIterations = options.reinitIterations;
		t.reinitThreshold = options.reinitThreshold;
		t.reinitCfl = options.reinitCfl;
		t.particles = options.particles;
		t.particle = options.particle;
		t.cfl = options.cfl;
		error["transport-zalesak-n50"] = RunZalesak(50, t, &pool).l1;
		error["transport-vortex-n50"] = RunVortex(50, 2.0, t, &pool).l1;
	}
	{
		const HydrostaticResult h = RunHydrostatic(32, false, 0.3, true, 0.3, 0.25, options, &pool);
		error["hydrostatic-n32"] = h.maxVelocity;
		error["hydrostatic-pressure-n32"] = h.pressureError;
	}
	const double kdFinite = 3*3.14159265358979323846/8;   // 3 cells of depth per 16 across: interface on faces at every level
	{
		const WaveResult w16 = RunStandingWave(16, kdFinite, 0.01, 3, options, &pool);
		const WaveResult w32 = RunStandingWave(32, kdFinite, 0.01, 3, options, &pool);
		error["wave-finite-n32"] = std::fabs(w32.frequencyError);
		const double order = std::log(std::fabs(w16.frequencyError)/std::fabs(w32.frequencyError))/std::log(2.0);
		error["order-wave"] = std::isfinite(order) ? std::max(0.0, 1.8 - order) : 1.0;
		error["wave-deep-n32"] = std::fabs(RunStandingWave(32, 3.14159265358979323846, 0.01, 3, options, &pool).frequencyError);
	}
	{
		const SloshResult s = RunSloshing(20, 2, options, &pool);
		error["slosh-n20"] = std::max(std::fabs(s.error10), std::fabs(s.error01));
	}
	{
		const CollapseResult c16 = RunColumnCollapse(16, 1.0, options, &pool), c32 = RunColumnCollapse(32, 1.0, options, &pool);
		error["collapse-n32"] = c32.maxFar;
		const double order = std::log(c16.maxFar/c32.maxFar)/std::log(2.0);
		error["order-collapse"] = std::isfinite(order) ? std::max(0.0, 1.8 - order) : 1.0;
	}
	{
		const SolidCollapseResult c = RunSolidCollapse(32, 0.35, {{0.8123, 0.1789, 0}}, options, &pool);
		error["solid-collapse-n32"] = c.maxFar;
		error["solid-pressure-n32"] = c.pressure;
	}
	{
		const TaylorGreenResult t16 = RunTaylorGreen(16, 2.0, options, &pool), t32 = RunTaylorGreen(32, 2.0, options, &pool);
		error["taylor-green-n32"] = t32.l2Error;
		const double order = std::log(t16.l2Error/t32.l2Error)/std::log(2.0);
		error["order-taylor-green"] = std::isfinite(order) ? std::max(0.0, 1.8 - order) : 1.0;
	}
	error["volume-correction-n32"] = options.volumeCorrection ? RunVolumeConservation(32, 10, true, options, &pool).meanShift : 1.0;
	std::vector<EvidenceCase> cases;
	for (const CaseSpec& spec : Cases) {
		EvidenceCase c;
		c.id = spec.id;
		c.description = spec.description;
		c.tolerance = options.particles && spec.withParticles > 0 ? spec.withParticles : spec.tolerance;
		c.error = error[spec.id];
		c.passed = std::isfinite(c.error) && c.error <= c.tolerance;
		const V configuration = V::MapValue({{"solver", V::String(LiquidSolverId)}, {"version", V::String(SolverVersion)},
			{"numerics", NumericsValue(options)}});
		c.digest = EvidenceDigest(c.id, configuration, c.error, c.tolerance, c.passed);
		cases.push_back(c);
	}
	std::lock_guard<std::mutex> hold(mutex);
	memo[digest] = cases;
	return cases;
}

// M7b: the rigid-body cases (computed only when the configuration has bodies).
// Tolerances: 1.25 x the error observed with the default numerics, rounded
// up (docs/SIMULATION_M7B_RESULTS.md section 5); the neutral body at the
// solver's tolerance level; the impulse audit at round-off.
const CaseSpec BodyCases[] = {
	{"added-mass-cylinder-n64", "impulsive start of a circular cylinder (radius 0.15) in a concentric cut-cell container (radius 0.4), 2-D, 64 cells: |m_a / (rho pi a^2 (b^2 + a^2) / (b^2 - a^2)) - 1|", 2.6e-3},
	{"added-mass-sphere-n32", "impulsive start of a sphere (radius 0.15) in a concentric spherical container (radius 0.4), 32^3: |m_a / ((rho V / 2) (b^3 + 2 a^3) / (b^3 - a^3)) - 1|", 2.8e-2},
	{"added-mass-oscillation-n32", "the cylinder in prescribed oscillation (amplitude 0.01, 2 Hz, 2 periods; moving cut cells), 2-D, 32 cells: |sine coefficient of the force / (m_a X w^2) - 1|", 1.5e-2},
	{"archimedes-0.1-n32", "floating sphere, density ratio 0.1, 32^3: |discrete equilibrium height - Archimedes| / R", 1.4e-2},
	{"archimedes-0.5-n32", "floating sphere, density ratio 0.5, 32^3: |discrete equilibrium height - Archimedes| / R", 5.0e-4},
	{"archimedes-0.9-n32", "floating sphere, density ratio 0.9, 32^3: |discrete equilibrium height - Archimedes| / R", 1.2e-2},
	{"neutral-rest-n32", "a discretely neutral cylinder (mass rho V_y) at rest in still liquid, 2-D, 32 cells, 1 s, free in all degrees of freedom: max body and liquid speed / sqrt(g h)", 1e-7},
	{"light-float-n32", "a floating cylinder of density ratio 0.1 released 0.1 R above equilibrium, coupled in the pressure solve, 2-D, 32 cells, 3 s: max |y - mean| over the second half / the initial offset from the mean (no growth)", 0.76},
	{"momentum-audit-n32", "the same run: |change of liquid + body vertical momentum - gravity's and the walls' impulses| / (gravity's impulse on liquid and body)", 2.2e-6},
	{"body-impulse-n32", "the same run: |m dv - m g T - the liquid's impulses on the body| / (m g T)", 1e-12}};

std::vector<EvidenceCase> ComputeBodyEvidence(const SolverOptions& options, unsigned threads)
{
	static std::mutex mutex;
	static std::map<std::string, std::vector<EvidenceCase>> memo;
	Bytes key;
	std::string ignored;
	RISECBOR64::Encode(NumericsValue(options), key, &ignored);
	const std::string digest = RISECBOR64::SHA256Hex(key);
	{
		std::lock_guard<std::mutex> hold(mutex);
		const auto it = memo.find(digest);
		if (it != memo.end()) return it->second;
	}
	Pool pool(threads);
	std::map<std::string, double> error;
	error["added-mass-cylinder-n64"] = std::fabs(RunAddedMassImpulse(64, false, 0.15, 0.4, 0.37, options, &pool).error);
	error["added-mass-sphere-n32"] = std::fabs(RunAddedMassImpulse(32, true, 0.15, 0.4, 0.37, options, &pool).error);
	error["added-mass-oscillation-n32"] = std::fabs(RunAddedMassOscillation(32, 0.15, 0.4, 0.01, 2.0, 2, options, &pool).error);
	error["archimedes-0.1-n32"] = std::fabs(RunFloatingStatics(32, true, 0.1, 0.37, options, &pool).offset);
	error["archimedes-0.5-n32"] = std::fabs(RunFloatingStatics(32, true, 0.5, 0.37, options, &pool).offset);
	error["archimedes-0.9-n32"] = std::fabs(RunFloatingStatics(32, true, 0.9, 0.37, options, &pool).offset);
	{
		const NeutralResult n = RunNeutralBody(32, false, 1.0, options, &pool);
		error["neutral-rest-n32"] = std::max(n.bodySpeed, n.liquidSpeed);
	}
	{
		const FloatDynamics f = RunFloatingDynamics(32, false, 0.1, 0.1, 3.0, false, options, &pool);
		error["light-float-n32"] = f.finite ? f.lateGrowth : 1e300;
		error["momentum-audit-n32"] = f.finite ? f.totalAudit : 1e300;
		error["body-impulse-n32"] = f.finite ? f.bodyAudit : 1e300;
	}
	std::vector<EvidenceCase> cases;
	for (const CaseSpec& spec : BodyCases) {
		EvidenceCase c;
		c.id = spec.id;
		c.description = spec.description;
		c.tolerance = spec.tolerance;
		c.error = error[spec.id];
		c.passed = std::isfinite(c.error) && c.error <= c.tolerance;
		const V configuration = V::MapValue({{"solver", V::String(LiquidSolverId)}, {"version", V::String(SolverVersion)},
			{"numerics", NumericsValue(options)}, {"bodies", V::String("coupled pressure solve (m7b)")}});
		c.digest = EvidenceDigest(c.id, configuration, c.error, c.tolerance, c.passed);
		cases.push_back(c);
	}
	std::lock_guard<std::mutex> hold(mutex);
	memo[digest] = cases;
	return cases;
}

// M7c: the viscosity and surface-tension cases (computed only when the option is on).
// Tolerances: 1.25 x the error observed with the default numerics, rounded up
// (docs/SIMULATION_M7C_RESULTS.md section 5); the rigid rotation at the solver's level.
V PhysicsNumericsValue(const SolverOptions& o)
{
	return V::MapValue({{"base", NumericsValue(o)}, {"capillary_cfl", V::Float(o.capillaryCfl)},
		{"curvature", V::String(o.interpolateCurvature ? "interpolated" : "cell")}, {"viscosity_tolerance", V::Float(o.viscosityTolerance)},
		{"viscous_cfl", V::Float(o.viscousCfl)}, {"viscous_samples", V::Unsigned(std::uint64_t(o.viscousSamples))},
		{"viscous_theta_min", V::Float(o.viscousThetaMin)}});
}

const CaseSpec ViscousCases[] = {
	{"viscous-taylor-green-n32", "Taylor-Green vortex, nu = 0.05 m^2/s, free-slip walls, 32 x 32, T = 2: |amplitude / exp(-2 nu T) - 1|", 6.6e-4},
	{"viscous-wave-n32", "standing wave kd = pi, ka = 0.01, nu = 1e-3 m^2/s, free slip, 32 cells per wavelength, 4 periods: |decay rate / Lamb's deep-water root - 1| (the numerical damping included)", 3.9e-2},
	{"couette-torque-n32", "circular Couette, a spinning cut-cell cylinder (r 0.15, 0.1 rad/s) in a static one (r 0.4), nu = 0.01, no slip, 32 cells, steady: |torque / exact - 1|", 9.2e-3},
	{"couette-profile-n32", "same: max |u - u_exact| / (omega R1) between the walls", 2.3e-2},
	{"cylinder-drag-n32", "Stokes drag on a cylinder (r 0.15) translating in a concentric container (r 0.4), nu = 1, no slip, 32 cells: |F / exact - 1|", 3.2e-2},
	{"rigid-rotation-n32", "a free liquid disk in rigid rotation, one viscous step: max |du| / (omega R) (zero strain: no change)", 1e-9}};

const CaseSpec CapillaryCases[] = {
	{"laplace-pressure-n32", "static drop R = 0.25 (2-D), sigma = rho = 1, Laplace number 12000, 32 cells, 2 capillary times: |mean inside pressure / (sigma / R) - 1|", 2.6e-3},
	{"parasitic-n32", "same: largest parasitic speed over the run as a capillary number mu U / sigma", 3.0e-5},
	{"capillary-wave-n32", "capillary-gravity standing wave, Bond number 1, kd = pi, ka = 0.01, 32 cells per wavelength, 3 periods: |omega error| / omega", 3.3e-2},
	{"oscillating-drop-n32", "2-D drop R = 0.25, mode 2, amplitude 0.05 R, zero gravity, 32 cells, 2 periods: |omega / Lamb's - 1|", 2.2e-2}};

std::vector<EvidenceCase> ComputePhysicsEvidence(const SolverOptions& options, unsigned threads, bool viscosity, bool capillary)
{
	static std::mutex mutex;
	static std::map<std::string, std::vector<EvidenceCase>> memo;
	Bytes key;
	std::string ignored;
	RISECBOR64::Encode(V::MapValue({{"numerics", PhysicsNumericsValue(options)}, {"viscosity", V::Bool(viscosity)},
		{"capillary", V::Bool(capillary)}}), key, &ignored);
	const std::string digest = RISECBOR64::SHA256Hex(key);
	{
		std::lock_guard<std::mutex> hold(mutex);
		const auto it = memo.find(digest);
		if (it != memo.end()) return it->second;
	}
	Pool pool(threads);
	std::map<std::string, double> error;
	std::vector<const CaseSpec*> specs;
	if (viscosity) {
		error["viscous-taylor-green-n32"] = std::fabs(RunViscousTaylorGreen(32, 0.05, 2.0, options, &pool).amplitudeError);
		error["viscous-wave-n32"] = std::fabs(RunViscousWave(32, 3.14159265358979323846, 0.01, 4, 1e-3, false, false, options, &pool).error);
		const CouetteResult c = RunCouette(32, 0.15, 0.4, 0.1, 0.01, 6.0, 0.37, 0, options, &pool);
		error["couette-torque-n32"] = std::fabs(c.torqueError);
		error["couette-profile-n32"] = c.profileError;
		error["cylinder-drag-n32"] = std::fabs(RunCylinderDrag(32, 0.15, 0.4, 0.01, 1.0, 0.3, 0.37, options, &pool).error);
		error["rigid-rotation-n32"] = RunRotatingDisk(32, 0.01, 0, options, &pool).stepChange;
		for (const CaseSpec& c2 : ViscousCases) specs.push_back(&c2);
	}
	if (capillary) {
		const DropResult d = RunStaticDrop(32, false, 2, std::sqrt(0.5/12000), options, &pool);
		error["laplace-pressure-n32"] = std::fabs(d.pressureError);
		error["parasitic-n32"] = d.capillaryNumber;
		error["capillary-wave-n32"] = std::fabs(RunCapillaryWave(32, 3.14159265358979323846, 0.01, 9.81/(4*3.14159265358979323846*3.14159265358979323846), 3, options, &pool).frequencyError);
		error["oscillating-drop-n32"] = std::fabs(RunOscillatingDrop(32, false, 0.05, 2, 0, options, &pool).error);
		for (const CaseSpec& c2 : CapillaryCases) specs.push_back(&c2);
	}
	std::vector<EvidenceCase> cases;
	for (const CaseSpec* spec : specs) {
		EvidenceCase c;
		c.id = spec->id;
		c.description = spec->description;
		c.tolerance = spec->tolerance;
		c.error = error[spec->id];
		c.passed = std::isfinite(c.error) && c.error <= c.tolerance;
		const V configuration = V::MapValue({{"solver", V::String(LiquidSolverId)}, {"version", V::String(SolverVersion)},
			{"numerics", PhysicsNumericsValue(options)}, {"physics", V::String("viscosity and surface tension (m7c)")}});
		c.digest = EvidenceDigest(c.id, configuration, c.error, c.tolerance, c.passed);
		cases.push_back(c);
	}
	std::lock_guard<std::mutex> hold(mutex);
	memo[digest] = cases;
	return cases;
}

// ------------------------------------------------------------------ checkpoint streams

class StreamWriter {
public:
	explicit StreamWriter(ICheckpointSink& sink) : sink_(sink) {}
	bool Raw(const void* data, std::size_t size, std::string& error) {
		const auto* p = static_cast<const std::uint8_t*>(data);
		while (size) {
			const std::size_t n = std::min(size, std::size_t(8) << 20);
			if (!sink_.Write(p, n, error)) return false;
			p += n; size -= n;
		}
		return true;
	}
	bool U64(std::uint64_t x, std::string& error) {
		std::uint8_t b[8];
		for (int k = 0; k < 8; ++k) b[k] = std::uint8_t(x >> (8*k));
		return Raw(b, 8, error);
	}
	bool Doubles(const std::vector<double>& v, std::string& error) {
		std::vector<std::uint8_t> buf;
		std::size_t i = 0;
		while (i < v.size()) {
			const std::size_t n = std::min(v.size() - i, std::size_t(1) << 20);
			buf.resize(8*n);
			for (std::size_t k = 0; k < n; ++k) {
				std::uint64_t w;
				std::memcpy(&w, &v[i + k], 8);
				for (int b = 0; b < 8; ++b) buf[8*k + std::size_t(b)] = std::uint8_t(w >> (8*b));
			}
			if (!Raw(buf.data(), buf.size(), error)) return false;
			i += n;
		}
		return true;
	}
private:
	ICheckpointSink& sink_;
};

class StreamReader {
public:
	explicit StreamReader(ICheckpointSource& source) : source_(source) {}
	bool Raw(void* data, std::size_t size, std::string& error) {
		auto* p = static_cast<std::uint8_t*>(data);
		while (size) {
			const std::size_t n = std::min(size, std::size_t(8) << 20);
			if (source_.Remaining() < n) return Fail(error, "liquid checkpoint stream is truncated");
			if (!source_.Read(p, n, error)) return false;
			p += n; size -= n;
		}
		return true;
	}
	bool U64(std::uint64_t& x, std::string& error) {
		std::uint8_t b[8];
		if (!Raw(b, 8, error)) return false;
		x = 0;
		for (int k = 0; k < 8; ++k) x |= std::uint64_t(b[k]) << (8*k);
		return true;
	}
	bool Doubles(std::vector<double>& v, std::size_t count, std::string& error) {
		if (source_.Remaining() < 8*std::uint64_t(count)) return Fail(error, "liquid checkpoint stream is truncated");
		v.resize(count);
		std::vector<std::uint8_t> buf;
		std::size_t i = 0;
		while (i < count) {
			const std::size_t n = std::min(count - i, std::size_t(1) << 20);
			buf.resize(8*n);
			if (!Raw(buf.data(), buf.size(), error)) return false;
			for (std::size_t k = 0; k < n; ++k) {
				std::uint64_t w = 0;
				for (int b = 0; b < 8; ++b) w |= std::uint64_t(buf[8*k + std::size_t(b)]) << (8*b);
				std::memcpy(&v[i + k], &w, 8);
			}
			i += n;
		}
		return true;
	}
	std::uint64_t Remaining() const { return source_.Remaining(); }
private:
	ICheckpointSource& source_;
};

// ------------------------------------------------------------------ solver

struct Segment {
	double t = 0, segStart = 0, segTarget = 0;
	std::uint64_t next = 0, segN = 0, segJ = 0;
	bool finished = false;
};

struct LiquidToken final : SolverStateToken {
	const void* owner = nullptr;
	Segment seg;
	std::vector<double> phi, q;
	MacField u;
	FaceMask valid;
	double lastDt = 0, targetVolume = 0;
	std::uint64_t steps = 0, nextId = 0;
	std::vector<MarkerParticles::Particle> particles;
	// M7b: bodies and the open port exchange.
	std::vector<LiquidBody> bodies;
	std::vector<Vec6> accumulated, last, lastViscous;   // the last step's impulse and its viscous part (m7c: the bodies' prediction)
	bool macroOpen = false;
	double macroStart = 0;
	std::vector<std::string> portNames;
	std::uint64_t contacts = 0;
};

// The pack of a body's state in checkpoints: x (3), q (4), v (3), pi (3), mass,
// moments (3), external force (3), the accumulated impulse (6).
constexpr std::size_t BodyDoubles = 26;
void PackBody(const LiquidBody& b, const Vec6& acc, std::vector<double>& out)
{
	const Rigid::Body& s = b.state;
	out.insert(out.end(), s.x.begin(), s.x.end());
	out.insert(out.end(), s.q.begin(), s.q.end());
	out.insert(out.end(), s.v.begin(), s.v.end());
	out.insert(out.end(), s.pi.begin(), s.pi.end());
	out.push_back(s.mass);
	out.insert(out.end(), s.moments.begin(), s.moments.end());
	out.insert(out.end(), b.externalForce.begin(), b.externalForce.end());
	out.insert(out.end(), acc.begin(), acc.end());
}
void UnpackBody(const double* p, LiquidBody& b, Vec6& acc)
{
	Rigid::Body& s = b.state;
	for (int k = 0; k < 3; ++k) s.x[std::size_t(k)] = p[k];
	for (int k = 0; k < 4; ++k) s.q[std::size_t(k)] = p[3 + k];
	for (int k = 0; k < 3; ++k) s.v[std::size_t(k)] = p[7 + k];
	for (int k = 0; k < 3; ++k) s.pi[std::size_t(k)] = p[10 + k];
	s.mass = p[13];
	for (int k = 0; k < 3; ++k) s.moments[std::size_t(k)] = p[14 + k];
	for (int k = 0; k < 3; ++k) b.externalForce[std::size_t(k)] = p[17 + k];
	for (int k = 0; k < 6; ++k) acc[std::size_t(k)] = p[20 + k];
}
Bytes DoublesBytes(const std::vector<double>& v)
{
	Bytes out(8*v.size());
	for (std::size_t i = 0; i < v.size(); ++i) {
		std::uint64_t w;
		std::memcpy(&w, &v[i], 8);
		for (int b = 0; b < 8; ++b) out[8*i + std::size_t(b)] = std::uint8_t(w >> (8*b));
	}
	return out;
}
bool BytesDoubles(const V* value, std::size_t count, std::vector<double>& v)
{
	if (!value || value->GetType() != V::ByteString || value->GetBytes().size() != 8*count) return false;
	const auto& b = value->GetBytes();
	v.assign(count, 0);
	for (std::size_t i = 0; i < count; ++i) {
		std::uint64_t w = 0;
		for (int k = 0; k < 8; ++k) w |= std::uint64_t(b[8*i + std::size_t(k)]) << (8*k);
		std::memcpy(&v[i], &w, 8);
	}
	return true;
}

class LiquidSolver final : public ISimulationSolver {
public:
	bool Configure(const V& parameters, const ISceneInputs& inputs, SolverDescription& d, std::string& error) override;
	bool Start(std::string& error) override;
	bool Restore(const Bytes& state, std::string& error) override;
	bool Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
		AdvanceResult& result, std::string& error) override;
	bool Checkpoint(Bytes& state, std::string& error) override;
	bool CheckpointTo(ICheckpointSink& sink, std::string& error) override;
	bool RestoreFrom(ICheckpointSource& source, std::string& error) override;
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
	bool Publish(IFrameSink& sink, IPortExchange* ports, AdvanceResult& result, std::string& error);
	bool OfferFields(IPortExchange* ports, std::string& error);
	// M7b: port bodies (pose and twist from rigid.bodies at each macro start;
	// the hydrodynamic impulse over the step offered as a held load).
	bool AcceptBodies(IPortExchange* ports, std::string& error);
	bool OfferLoads(IPortExchange* ports, double time, double span, std::string& error);
	bool EncodeBodies(double time, std::shared_ptr<const Bytes>& payload, std::string& error) const;
	//! Inelastic contact of the bodies the liquid integrates (dynamic and coupled port
	//! bodies) with the domain walls (preview; not offered: rigid.bodies owns contact).
	void WallContact(double dt);

	bool configured_ = false, started_ = false;
	LiquidSetup setup_;
	SolverOptions options_;
	unsigned threads_ = 1;
	std::uint64_t count_ = 121;
	double interval_ = 1.0/30;
	bool publishFields_ = false, publishParticles_ = false;
	LiquidModel model_;
	Segment s_;
	std::atomic<bool> stop_{false};
	bool portBodies_ = false, implicitPorts_ = false;
	bool macroOpen_ = false;
	double macroStart_ = 0;
	std::vector<std::string> portNames_;
	std::uint64_t contacts_ = 0;
};

bool LiquidSolver::Configure(const V& parameters, const ISceneInputs& inputs, SolverDescription& d, std::string& error)
{
	configured_ = started_ = false;
	Reader p(parameters, "", error);
	const std::uint64_t frames = p.Count("frames", 121, 1, 1000000);
	const double rate = p.Real("frame_rate", 30.0);
	const V3 origin = p.Vector("domain_min", {{-0.5, 0, -0.25}});
	V3 size = p.Vector("domain_size", {{1, 0.5, 0.5}}, false);
	const std::uint64_t resolution = p.Count("resolution", 64, 1, 4096);
	if (!(size[0] > 0 && size[1] > 0 && size[2] > 0)) return p.Bad("domain_size must be positive on every axis");
	LiquidSetup setup;
	setup.grid.h = std::max(size[0], std::max(size[1], size[2]))/double(resolution);
	for (int a = 0; a < 3; ++a) {
		setup.grid.n[a] = std::max(1, static_cast<int>(std::lround(size[a]/setup.grid.h)));
		size[a] = setup.grid.n[a]*setup.grid.h;   // the effective (whole-cell) extent
	}
	setup.grid.origin = origin;
	p.out.emplace_back("domain_size", Vec(size));
	setup.gravity = p.Vector("gravity", {{0, -9.81, 0}});
	setup.density = p.Real("density", 1000.0);
	setup.openTop = p.Text("top", "wall", {"wall", "open"}) == "open";
	setup.initialVelocity = p.Vector("initial_velocity", {{0, 0, 0}});
	// m7c: viscosity and surface tension (0: off).
	setup.viscosity = p.Real("viscosity", 0.0);
	setup.noSlip = p.Text("viscous_walls", "no_slip", {"no_slip", "free_slip"}) == "no_slip";
	setup.surfaceTension = p.Real("surface_tension", 0.0);
	{
		const V* list = p.Raw("liquid");
		V::Values out;
		if (!list) {
			Shape s;
			s.type = Shape::Type::Box;
			s.lo = {{-0.5, 0, -0.25}};
			s.hi = {{-0.2, 0.3, 0.25}};
			setup.liquid.push_back(s);
			out.push_back(V::MapValue({{"type", V::String("box")}, {"min", Vec(s.lo)}, {"max", Vec(s.hi)}}));
		} else {
			if (list->GetType() != V::Array) return p.Bad("liquid must be an array of regions");
			for (const V& item : list->GetArray()) {
				Shape s;
				V eff;
				if (!ParseLiquid(item, s, eff, error)) return false;
				setup.liquid.push_back(s);
				out.push_back(eff);
			}
		}
		p.out.emplace_back("liquid", V::ArrayValue(out));
	}
	std::vector<std::string> sceneNames;
	{
		V::Values out;
		if (const V* list = p.Raw("solids")) {
			if (list->GetType() != V::Array) return p.Bad("solids must be an array");
			for (const V& item : list->GetArray()) {
				Shape s;
				V eff;
				if (!ParseSolid(item, inputs, sceneNames, s, eff, error)) return false;
				setup.solids.push_back(std::move(s));
				out.push_back(eff);
			}
		}
		p.out.emplace_back("solids", V::ArrayValue(out));
	}
	{
		V::Values out;
		if (const V* list = p.Raw("bodies")) {
			if (list->GetType() != V::Array) return p.Bad("bodies must be an array");
			for (const V& item : list->GetArray()) {
				LiquidBody b;
				V eff;
				if (!ParseBody(item, setup.gravity, b, eff, error)) return false;
				setup.bodies.push_back(std::move(b));
				out.push_back(eff);
			}
		}
		p.out.emplace_back("bodies", V::ArrayValue(out));
	}
	SolverOptions o;
	{
		const std::string m = p.Text("momentum", "weno5", {"weno5", "maccormack", "semi_lagrangian"});
		o.momentum = m == "weno5" ? MomentumScheme::Weno5 : m == "maccormack" ? MomentumScheme::MacCormack : MomentumScheme::SemiLagrangian;
		o.momentumStages = int(p.Count("momentum_stages", 2, 1, 2));
		const std::string r = p.Text("reinit", "sussman", {"none", "sussman", "russo_smereka", "russo_smereka_central", "subcell", "frozen"});
		o.reinit = r == "none" ? ReinitScheme::None : r == "sussman" ? ReinitScheme::Sussman : r == "russo_smereka" ? ReinitScheme::RussoSmereka :
			r == "russo_smereka_central" ? ReinitScheme::RussoSmerekaCentral : r == "subcell" ? ReinitScheme::Subcell : ReinitScheme::Frozen;
		o.reinitIterations = int(p.Count("reinit_iterations", 1, 1, 100));
		o.reinitThreshold = p.Real("reinit_threshold", 0.1);
		o.particles = p.Bool("particles", true);
		o.particle.perCell = int(p.Count("particles_per_cell", 16, 1, 256));
		o.volumeCorrection = p.Bool("volume_correction", true);
		o.cfl = p.Real("cfl", 0.5);
		o.waveCfl = p.Real("wave_cfl", 0.5);
		o.pressure.tolerance = p.Real("pressure_tolerance", 1e-8);
		o.pressure.thetaMin = p.Real("theta_min", 1e-3);
		o.minFaceFraction = p.Real("min_face_fraction", 0.0);
		o.minTransportFraction = p.Real("min_transport_fraction", 0.25);
		o.speedLimit = p.Real("speed_limit", 3.0);
		o.viscosityTolerance = p.Real("viscosity_tolerance", 1e-8);
		o.viscousCfl = p.Real("viscous_cfl", 4.0);
		o.capillaryCfl = p.Real("capillary_cfl", 0.5);
		o.interpolateCurvature = p.Text("curvature", "interpolated", {"interpolated", "cell"}) == "interpolated";
		o.extrapolationLayers = int(p.Count("extrapolation_layers", 6, 1, 64));
		o.interfaceExtrapolationLayers = int(p.Count("interface_extrapolation_layers", 4, 1, 64));
		o.linearExtrapolationLayers = int(p.Count("linear_extrapolation_layers", 1, 0, 8));
		o.pressure.openTop = setup.openTop;
		if (!(o.cfl > 0 && o.cfl <= 1) || !(o.waveCfl > 0 && o.waveCfl <= 2)) return p.Bad("cfl must be in (0, 1] and wave_cfl in (0, 2]");
		if (!(o.pressure.tolerance > 0 && o.pressure.tolerance < 1)) return p.Bad("pressure_tolerance must be in (0, 1)");
		if (!(o.pressure.thetaMin > 0 && o.pressure.thetaMin < 0.5)) return p.Bad("theta_min must be in (0, 1/2)");
		if (!(o.reinitThreshold >= 0)) return p.Bad("reinit_threshold must not be negative");
		if (!(o.minFaceFraction >= 0 && o.minFaceFraction < 1)) return p.Bad("min_face_fraction must be in [0, 1)");
		if (!(o.minTransportFraction >= 0 && o.minTransportFraction <= 1)) return p.Bad("min_transport_fraction must be in [0, 1]");
		if (!(o.speedLimit >= 0)) return p.Bad("speed_limit must not be negative");
		if (!(o.viscosityTolerance > 0 && o.viscosityTolerance < 1)) return p.Bad("viscosity_tolerance must be in (0, 1)");
		if (!(o.viscousCfl >= 0)) return p.Bad("viscous_cfl must not be negative");
		if (!(o.capillaryCfl > 0 && o.capillaryCfl <= 2)) return p.Bad("capillary_cfl must be in (0, 2]");
	}
	const bool fields = p.Bool("publish_fields", false);
	const bool particlesOut = p.Bool("publish_particles", false);
	if (particlesOut && !o.particles) return p.Bad("publish_particles needs particles");
	// Results are bitwise the same for any thread count (LiquidGrid.h): unkeyed.
	const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
	const unsigned threads = static_cast<unsigned>(p.Count("threads", std::min(16u, hardware), 1, 256, false));
	SolverDescription candidate;
	if (!p.Finish()) return false;
	if (!(rate > 0)) return Fail(error, "frame_rate must be positive");
	if (!(setup.density > 0)) return Fail(error, "density must be positive");
	if (!(setup.viscosity >= 0) || !std::isfinite(setup.viscosity)) return Fail(error, "viscosity must not be negative");
	if (!(setup.surfaceTension >= 0) || !std::isfinite(setup.surfaceTension)) return Fail(error, "surface_tension must not be negative");
	if (setup.grid.Cells() > (std::size_t(1) << 31)) return Fail(error, "the grid is too large");
	for (std::size_t i = 0; i < setup.bodies.size(); ++i)
		for (std::size_t j = 0; j < i; ++j)
			if (setup.bodies[i].name == setup.bodies[j].name) return Fail(error, "body names must be unique: " + setup.bodies[i].name);
	candidate.effectiveParameters = V::MapValue(p.out);
	setup_ = setup;
	options_ = o;
	threads_ = threads;
	count_ = frames;
	interval_ = 1/rate;
	publishFields_ = fields;
	publishParticles_ = particlesOut;
	portBodies_ = implicitPorts_ = false;
	for (const LiquidBody& b : setup.bodies) if (b.motion == BodyMotion::Port) portBodies_ = true;
	implicitPorts_ = portBodies_;
	for (const LiquidBody& b : setup.bodies) if (b.motion == BodyMotion::Port && b.staggered) implicitPorts_ = false;
	configured_ = true;

	candidate.solverId = LiquidSolverId;
	candidate.version = SolverVersion;
	candidate.cache.firstIndex = 0;
	candidate.cache.count = count_;
	candidate.cache.startTime = 0;
	candidate.cache.interval = interval_;
	candidate.cache.outputs = {{SurfaceOutput, "mesh.v2", {{"normal", "1", "f32x3"}, {"position", "m", "f32x3"}, {"velocity", "m/s", "f32x3"}}}};
	if (fields) candidate.cache.outputs.push_back({FieldsOutput, "grid.v1", {{"phi", "m", "f32"}, {"pressure", "Pa", "f32"},
		{"velocity", "m/s", "f32x3"}}});
	if (particlesOut) candidate.cache.outputs.push_back({ParticlesOutput, "points.v1", {{"position", "m", "f32x3"},
		{"radius", "m", "f32"}, {"sign", "1", "f32"}, {"velocity", "m/s", "f32x3"}}});
	if (!setup.bodies.empty()) candidate.cache.outputs.push_back({BodiesOutput, TransformCodec, {{"angular_velocity", "rad/s", "f64x3"},
		{"orientation", "1", "f64x4"}, {"position", "m", "f64x3"}, {"velocity", "m/s", "f64x3"}}});
	candidate.inputs = sceneNames;
	{
		Bytes records;
		V::Members rec{{"format", V::String("liquid.mac.v1")}, {"density", V::Float(setup.density)},
			{"model", V::String("single-phase inviscid incompressible liquid, free surface p = 0, no surface tension")},
			{"units", V::MapValue({{"density", V::String("kg/m^3")}, {"length", V::String("m")}})}};
		if (setup.viscosity > 0 || setup.surfaceTension > 0) {
			// m7c (the m7b record is unchanged when both are off).
			rec[2].second = V::String(std::string("single-phase incompressible liquid") + (setup.viscosity > 0 ? ", constant viscosity" : ", inviscid") +
				", free surface" + (setup.surfaceTension > 0 ? " with surface tension" : " p = 0, no surface tension"));
			rec.emplace_back("surface_tension", V::Float(setup.surfaceTension));
			rec.emplace_back("viscosity", V::Float(setup.viscosity));
			rec[3].second = V::MapValue({{"density", V::String("kg/m^3")}, {"length", V::String("m")}, {"surface_tension", V::String("N/m")},
				{"viscosity", V::String("Pa s")}});
		}
		if (!RISECBOR64::Encode(V::MapValue(rec), records, &error)) return false;
		candidate.cache.physicalRecords = records;
	}
	// One-way coupling: the fields output as a cached-output port.
	if (fields)
		candidate.ports = {{FieldsPort, "liquid fields", "m", "grid.v1", true, FieldsOutput,
			{{"phi", "signed distance", "m", 1}, {"pressure", "pressure", "Pa", 1}, {"velocity", "velocity", "m/s", 3}}, ""}};
	// M7b: bodies that follow rigid.bodies (their declarations match its ports).
	if (portBodies_) {
		candidate.ports.push_back({StatePort, "rigid body state", "m", TransformCodec, false, "",
			{{"angular_velocity", "angular velocity", "rad/s", 3}, {"orientation", "orientation", "1", 4},
			 {"position", "position", "m", 3}, {"velocity", "velocity", "m/s", 3}}, ""});
		candidate.ports.push_back({MassPort, "rigid body mass properties", "kg", "field.v1", false, "",
			{{"center_of_mass", "position", "m", 3}, {"external_force", "force", "N", 3},
			 {"inertia", "moment of inertia", "kg m^2", 9}, {"mass", "mass", "kg", 1}}, ""});
		candidate.ports.push_back({LoadPort, "body load", "N", "field.v1", true, "",
			{{"force", "force", "N", 3}, {"point", "position", "m", 3}, {"torque", "torque", "N m", 3}}, ""});
	}
	auto& caps = candidate.capabilities;
	caps.backends = {{"cpu", BackendStatus::Supported, true, "portable C++17; `threads` does not change results"}};
	const std::size_t cells = setup.grid.Cells();
	caps.interactive = cells <= InteractiveCells;
	caps.deterministic = caps.resumable = caps.externalMacroStep = caps.rollback = true;
	caps.threadCountInvariant = true;
	caps.modes = {"bake"};
	caps.backend = "cpu";
	// M7b: the coupled pressure solve is the implicit exchange for port bodies.
	if (implicitPorts_) caps.ownedImplicitPorts = {LoadPort, MassPort, StatePort};
	// Measured (docs/SIMULATION_M7A_RESULTS.md section 8): peak RSS 356 bytes per cell at
	// 128^3 (a dam break; 565 at 64^3 with the fixed part), particles about 140 bytes
	// per cell at 16 per band cell, the fields output 48; declared with a 25% margin.
	caps.memoryEstimate = (std::uint64_t(64) << 20) + std::uint64_t(cells)*450 +
		(o.particles ? std::uint64_t(cells)*std::uint64_t(o.particle.perCell)*9 : 0) + (fields ? std::uint64_t(cells)*60 : 0) +
		// m7c: the viscous solve's arrays (about 30 doubles per cell) and the curvature.
		(setup.viscosity > 0 ? std::uint64_t(cells)*300 : 0) + (setup.surfaceTension > 0 ? std::uint64_t(cells)*8 : 0);
	// Wall s per simulated s: the measured 1.2e-7 s per cell and step (128^3, 16
	// threads, particles +5%) times the steps a second needs at the wave limit or
	// at the dam-break speed sqrt(2 g D), whichever is more.
	{
		const double gn = std::sqrt(setup.gravity[0]*setup.gravity[0] + setup.gravity[1]*setup.gravity[1] + setup.gravity[2]*setup.gravity[2]);
		double depth = 0;
		for (int a = 0; a < 3; ++a) depth += gn > 0 ? std::fabs(setup.gravity[a])/gn*size[a] : 0;
		const double speed = std::max(std::sqrt(2*gn*depth), 1e-3);
		const double steps = std::max(speed/(o.cfl*setup.grid.h), gn > 0 ? std::sqrt(gn*depth)/(o.waveCfl*setup.grid.h) : 0.0);
		caps.costEstimate = 1.2e-7*double(cells)*steps*(o.particles ? 1.05 : 1.0);
		// M7b: measured at 128^3, 16 threads (docs/SIMULATION_M7B_RESULTS.md section 10): one coupled
		// body +35%, four +55%, eight +77% per step (more at 64^3).
		if (!setup.bodies.empty()) caps.costEstimate *= 1.3 + 0.06*double(setup.bodies.size());
		// M7c (docs/SIMULATION_M7C_RESULTS.md section 8): the viscous solve and the capillary limit.
		if (setup.viscosity > 0) caps.costEstimate *= ViscousCostFactor;
		if (setup.surfaceTension > 0) {
			const double capDt = o.capillaryCfl*std::sqrt(setup.density*setup.grid.h*setup.grid.h*setup.grid.h/(2*3.14159265358979323846*setup.surfaceTension));
			const double capSteps = 1/capDt;
			if (capSteps > steps) caps.costEstimate *= capSteps/std::max(1.0, steps);
		}
	}
	const bool solids = !setup.solids.empty();
	candidate.evidence.fidelity = "predictive";
	caps.earnableFidelity = {"preview", "predictive"};
	candidate.evidence.conditions = {{"interface", {o.particles ? "particle-level-set" : "level-set"}},
		{"momentum", {MomentumName(o.momentum)}}, {"solids", {solids ? "static" : "none"}}, {"top", {setup.openTop ? "open" : "wall"}},
		{"volume_correction", {o.volumeCorrection ? "on" : "off"}}, {"viscosity", {setup.viscosity > 0 ? "on" : "off"}},
		{"surface_tension", {setup.surfaceTension > 0 ? "on" : "off"}}};
	std::sort(candidate.evidence.conditions.begin(), candidate.evidence.conditions.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });
	candidate.evidence.scope = "Single-phase inviscid incompressible liquid with a free surface (p = 0, no surface tension) "
		"in a box with free-slip walls and static solids, on a uniform staggered grid: certified on canonical problems "
		"(interface transport, hydrostatic rest with a tilted surface and an immersed solid, linear standing waves at finite and "
		"infinite depth, rectangular-tank sloshing, the early-time collapse of a column against the box walls and against "
		"cut-cell solid planes rotated off the grid, a Taylor-Green vortex, volume correction), second order on the smooth "
		"ones except next to cut-cell solids (pressure about second order, velocity about first order on the faces beside "
		"the cut cells); it does not bound the error of a given bake and does not cover "
		"spray, thin sheets or any feature below about two cells, moving solids, viscosity, surface tension, or the accuracy "
		"of large-deformation flows (the interface is advanced with the step-start velocity, first order in time for "
		"nonlinear transport).";
	candidate.evidence.cases = ComputeEvidence(o, threads);
	if (!solids && setup.bodies.empty())
		candidate.notes.push_back(setup.viscosity > 0 && setup.noSlip ? "no solids: the domain walls (no slip for the viscosity) only" : "no solids: the domain walls (free slip) only");
	if (!setup.bodies.empty()) {
		// M7b: rigid bodies in the pressure solve.
		std::vector<std::string> motions;
		for (const LiquidBody& b : setup.bodies) motions.push_back(MotionName(b));
		std::sort(motions.begin(), motions.end());
		motions.erase(std::unique(motions.begin(), motions.end()), motions.end());
		candidate.evidence.conditions.push_back({"bodies", motions});
		std::sort(candidate.evidence.conditions.begin(), candidate.evidence.conditions.end(),
			[](const auto& a, const auto& b) { return a.first < b.first; });
		candidate.evidence.scope = "Single-phase inviscid incompressible liquid with a free surface (p = 0, no surface tension) "
			"in a box with free-slip walls, static solids and rigid bodies (sphere, box, cylinder) whose six degrees of freedom are "
			"solved with the liquid in one symmetric pressure system (dynamic or rigid.bodies-driven bodies) or that move on a "
			"prescribed path, on a uniform staggered grid: certified on canonical problems (those of the liquid alone, and the "
			"added mass of an impulsively started and of an oscillating cylinder and an impulsively started sphere in concentric "
			"containers, Archimedes equilibrium of floating spheres at density ratios 0.1, 0.5 and 0.9, a neutral body at rest, "
			"a light floating body's stability and its momentum audit); it does not bound the error of a given bake and does not "
			"cover contact of a body with another body or with a wall inside the liquid, gaps between a body and a wall, a "
			"static solid or another body below about two cells, bodies smaller than about four cells, spray, thin sheets or any "
			"feature below about two cells, viscosity, surface tension, or the accuracy of large-deformation flows (first order "
			"in time for nonlinear transport; velocities beside cut cells, moving or static, about first order).";
		const auto body = ComputeBodyEvidence(o, threads);
		candidate.evidence.cases.insert(candidate.evidence.cases.end(), body.begin(), body.end());
		// Configurations outside the scope: preview.
		const Grid& g = setup.grid;
		std::string limit;
		for (std::size_t i = 0; i < setup.bodies.size() && limit.empty(); ++i) {
			const LiquidBody& b = setup.bodies[i];
			if (b.shape.SmallestExtent() < 4*g.h)
				limit = "body " + b.name + " is smaller than four cells (" + std::to_string(b.shape.SmallestExtent()/g.h) + ")";
		}
		for (std::size_t i = 0; i < setup.bodies.size() && limit.empty(); ++i) {
			// The clearance at the start: the smallest body distance at nodes on a wall (not
			// across a one-cell axis, not the open top), inside a static analytic solid or
			// inside another body, near the body.
			const LiquidBody& b = setup.bodies[i];
			const Rigid::M3 Rb = Rigid::QMatrix(b.state.q);
			auto dist = [&](const LiquidBody& c, const Rigid::M3& R, const V3& x) {
				const V3 d{{x[0] - c.state.x[0], x[1] - c.state.x[1], x[2] - c.state.x[2]}};
				return c.shape.Distance(Rigid::ApplyT(R, d));
			};
			const double reach = b.shape.BoundingRadius() + 3*g.h;
			int lo[3], hi[3];
			for (int a = 0; a < 3; ++a) {
				lo[a] = std::max(0, int(std::floor((b.state.x[std::size_t(a)] - reach - g.origin[std::size_t(a)])/g.h)));
				hi[a] = std::min(g.n[a], int(std::ceil((b.state.x[std::size_t(a)] + reach - g.origin[std::size_t(a)])/g.h)));
			}
			double clearance = 1e300;
			for (int k = lo[2]; k <= hi[2]; ++k) for (int j = lo[1]; j <= hi[1]; ++j) for (int ii = lo[0]; ii <= hi[0]; ++ii) {
				const int idx[3] = {ii, j, k};
				const V3 x = g.Node(ii, j, k);
				bool obstacle = false;
				for (int a = 0; a < 3 && !obstacle; ++a)
					if (g.n[a] > 1 && (idx[a] == 0 || idx[a] == g.n[a]) && !(a == 1 && idx[a] == g.n[a] && setup.openTop)) obstacle = true;
				for (const Shape& sh : setup.solids) if (!obstacle && sh.type != Shape::Type::Mesh && sh.Distance(x) <= 0) obstacle = true;
				for (std::size_t q = 0; q < setup.bodies.size() && !obstacle; ++q)
					if (q != i && dist(setup.bodies[q], Rigid::QMatrix(setup.bodies[q].state.q), x) <= 0) obstacle = true;
				if (obstacle) clearance = std::min(clearance, dist(b, Rb, x));
			}
			if (clearance < 2*g.h)
				limit = "body " + b.name + " starts within two cells of a wall, a solid or another body (" + std::to_string(std::max(0.0, clearance)/g.h) + " cells)";
		}
		if (!limit.empty()) {
			candidate.evidence.fidelity = "preview";
			caps.fidelityLimit = limit;
			candidate.notes.push_back("bodies: " + limit + " (outside the certified scope)");
		}
		candidate.notes.push_back("rigid bodies: contact with walls or other bodies inside the liquid is not covered (preview); "
			"bodies the liquid integrates stop inelastically at the domain walls");
	}
	if (setup.viscosity > 0 || setup.surfaceTension > 0) {
		// M7c: viscosity and surface tension, each certified by its own cases.
		const auto physics = ComputePhysicsEvidence(o, threads, setup.viscosity > 0, setup.surfaceTension > 0);
		candidate.evidence.cases.insert(candidate.evidence.cases.end(), physics.begin(), physics.end());
		std::string add = " M7c adds";
		if (setup.viscosity > 0)
			add += " constant viscosity (implicit, the variational strain-rate form, zero viscous traction at the free surface, " +
				std::string(setup.noSlip ? "no slip" : "free slip") + " at walls, solids and bodies; certified on a viscous Taylor-Green "
				"vortex, the viscous damping of a standing wave against Lamb's root, circular Couette flow between a spinning cut-cell "
				"cylinder and a static one, Stokes drag on a cylinder in a concentric container and a rigidly rotating free disk; first "
				"order in time; bodies the liquid integrates are unknowns of the viscous solve; not covering variable viscosity or viscous flow in gaps "
				"below about two cells)";
		if (setup.surfaceTension > 0)
			add += std::string(setup.viscosity > 0 ? " and" : "") + " surface tension (the ghost-fluid Laplace jump with the level set's "
				"curvature at the sub-cell interface, 90 degree contact angle at the domain walls in the curvature only; certified on a "
				"viscous static drop's Laplace pressure and parasitic currents, a capillary-gravity standing wave and an oscillating drop, "
				"first order (the interface velocity is extrapolated by a constant with surface tension); "
				"not covering contact-line dynamics at solids, or inviscid surface tension over long times, whose parasitic currents grow)";
		candidate.evidence.scope += add + ".";
		std::string limit;
		if (setup.surfaceTension > 0 && !(setup.viscosity > 0))
			limit = "surface tension without viscosity: a static drop's parasitic currents are not bounded over long times (they grow to "
				"the capillary speed in about 5-10 capillary times at 32 cells)";
		else if (setup.surfaceTension > 0 && (!setup.solids.empty() || !setup.bodies.empty()))
			limit = "surface tension with solids or bodies: the contact angle and contact-line motion at solids are not validated";
		if (!limit.empty() && caps.fidelityLimit.empty()) {
			candidate.evidence.fidelity = "preview";
			caps.fidelityLimit = limit;
		}
		if (!limit.empty()) candidate.notes.push_back(limit + " (outside the certified scope)");
		if (setup.viscosity > 0 && !setup.bodies.empty())
			candidate.notes.push_back("viscosity with bodies: case 10 (a settling sphere) is in the validation test, not in this evidence "
				"(too slow for Configure; docs/SIMULATION_M7C_RESULTS.md)");
	}
	d = std::move(candidate);
	return true;
}

bool LiquidSolver::Start(std::string& error)
{
	if (!configured_) return Fail(error, "configure first");
	if (!model_.Initialize(setup_, options_, threads_, error)) return false;
	s_ = Segment();
	s_.finished = count_ == 0;
	macroOpen_ = false;
	macroStart_ = 0;
	portNames_.clear();
	contacts_ = 0;
	started_ = true;
	return true;
}

bool LiquidSolver::EncodeBodies(double time, std::shared_ptr<const Bytes>& payload, std::string& error) const
{
	TransformFrame f;
	f.time = time;
	for (const LiquidBody& b : model_.bodies) {
		TransformBody tb;
		tb.name = b.name;
		tb.position = b.state.x;
		Quat q = b.state.q;
		CanonicalQuaternion(q);
		tb.orientation = q;
		tb.velocity = b.state.v;
		tb.angularVelocity = b.Omega();
		f.bodies.push_back(tb);
	}
	std::sort(f.bodies.begin(), f.bodies.end(), [](const TransformBody& a, const TransformBody& b) { return a.name < b.name; });
	auto bytes = std::make_shared<Bytes>();
	if (!EncodeTransforms(f, CodecBudget, *bytes, error)) return false;
	payload = std::move(bytes);
	return true;
}

bool LiquidSolver::AcceptBodies(IPortExchange* ports, std::string& error)
{
	std::shared_ptr<const Bytes> bytes;
	if (ports->Accept(StatePort, s_.t, bytes, error) != PortStatus::Ready)
		return Fail(error, "a port body needs rigid.bodies' body_state connected to " + std::string(StatePort) + ": " + error);
	TransformFrame f;
	if (!DecodeTransforms(*bytes, CodecBudget, f, error)) return false;
	portNames_.clear();
	for (const auto& tb : f.bodies) portNames_.push_back(tb.name);
	FieldFrame mf;
	const bool haveMass = [&]() {
		std::shared_ptr<const Bytes> mb;
		std::string why;
		const PortStatus st = ports->Accept(MassPort, s_.t, mb, why);
		return st == PortStatus::Ready && DecodeField(*mb, CodecBudget, mf, why);
	}();
	if (implicitPorts_ && !haveMass) return Fail(error, "an implicitly coupled port body needs rigid.bodies' body_mass_properties");
	if (haveMass && (mf.domain != TransformBodyDomain(portNames_) || mf.count != portNames_.size()))
		return Fail(error, "body_mass_properties is not over body_state's bodies");
	const FieldChannel *mass = nullptr, *inertia = nullptr, *external = nullptr;
	if (haveMass)
		for (const auto& c : mf.channels) {
			if (c.name == "mass" && c.components == 1) mass = &c;
			else if (c.name == "inertia" && c.components == 9) inertia = &c;
			else if (c.name == "external_force" && c.components == 3) external = &c;
		}
	if (implicitPorts_ && (!mass || !inertia || !external))
		return Fail(error, "body_mass_properties needs mass, inertia and external_force (rigid.bodies m6.1 with the m7b channel)");
	for (LiquidBody& b : model_.bodies) {
		if (b.motion != BodyMotion::Port) continue;
		const auto it = std::find(portNames_.begin(), portNames_.end(), b.name);
		if (it == portNames_.end()) return Fail(error, "body_state has no body named " + b.name);
		const std::size_t k = std::size_t(it - portNames_.begin());
		const TransformBody& tb = f.bodies[k];
		if (s_.t == 0) {
			// The liquid's initial state of the body must be rigid.bodies'.
			double dx = 0, dv = 0;
			for (int a = 0; a < 3; ++a) {
				dx = std::max(dx, std::fabs(tb.position[std::size_t(a)] - b.state.x[std::size_t(a)]));
				dv = std::max(dv, std::fabs(tb.velocity[std::size_t(a)] - b.state.v[std::size_t(a)]));
			}
			Quat q = b.state.q;
			CanonicalQuaternion(q);
			double dq = 0;
			for (int a = 0; a < 4; ++a) dq = std::max(dq, std::fabs(tb.orientation[std::size_t(a)] - q[std::size_t(a)]));
			if (dx > 1e-6 || dv > 1e-6 || dq > 1e-6)
				return Fail(error, "liquid body " + b.name + " does not start where rigid.bodies has it (position, orientation and velocity must match)");
		}
		if (mass) {
			b.state.mass = FromF32(mass->values[k]);
			for (int a = 0; a < 3; ++a) b.state.moments[std::size_t(a)] = FromF32(inertia->values[9*k + 4*std::size_t(a)]);
			if (external) for (int a = 0; a < 3; ++a) b.externalForce[std::size_t(a)] = FromF32(external->values[3*k + std::size_t(a)]);
			if (!(b.state.mass > 0) || !(b.state.moments[0] > 0 && b.state.moments[1] > 0 && b.state.moments[2] > 0))
				return Fail(error, "body " + b.name + " has an invalid mass or inertia");
		}
		b.state.x = tb.position;
		b.state.q = tb.orientation;
		b.state.v = tb.velocity;
		const V3 wb = Rigid::ApplyT(Rigid::QMatrix(b.state.q), tb.angularVelocity);
		for (int a = 0; a < 3; ++a) b.state.pi[std::size_t(a)] = b.state.moments[std::size_t(a)]*wb[std::size_t(a)];
	}
	model_.UpdateBodyGeometry(true);
	return true;
}

// body_load over body_state's bodies: the liquid's mean force and torque (about
// the centre of mass; no point channel) over the macro step, which, held by
// rigid.bodies, gives the body the impulse the coupled solve gave it.
bool LiquidSolver::OfferLoads(IPortExchange* ports, double time, double span, std::string& error)
{
	FieldFrame f;
	f.domain = TransformBodyDomain(portNames_);
	f.count = static_cast<std::uint32_t>(portNames_.size());
	FieldChannel force{"force", "N", 3, {}}, torque{"torque", "N m", 3, {}};
	const auto bits = [](double x) { const float v = static_cast<float>(x); std::uint32_t b; std::memcpy(&b, &v, 4); return b; };
	for (const std::string& name : portNames_) {
		Vec6 load{{0, 0, 0, 0, 0, 0}};
		for (std::size_t i = 0; i < model_.bodies.size(); ++i)
			if (model_.bodies[i].name == name && model_.bodies[i].motion == BodyMotion::Port && span > 0)
				for (int m = 0; m < 6; ++m) load[std::size_t(m)] = model_.bodyAccumulated[i][std::size_t(m)]/span;
		for (int a = 0; a < 3; ++a) { force.values.push_back(bits(load[std::size_t(a)])); torque.values.push_back(bits(load[std::size_t(3 + a)])); }
	}
	f.channels = {force, torque};
	auto bytes = std::make_shared<Bytes>();
	if (!EncodeField(f, CodecBudget, *bytes, error)) return false;
	return ports->Offer(LoadPort, time, bytes, error);
}

void LiquidSolver::WallContact(double dt)
{
	// Bodies the liquid integrates stop at the domain walls: sequential
	// impulses (restitution 0, friction 0.3) at their support points
	// (rigid.bodies' plane contact), counted; not part of the validated model.
	const Grid& g = model_.GridSpec();
	std::vector<Rigid::Plane> planes;
	for (int a = 0; a < 3; ++a) {
		if (g.n[a] == 1) continue;
		for (int side = 0; side < 2; ++side) {
			if (a == 1 && side == 1 && setup_.openTop) continue;
			Rigid::Plane p;
			p.point = g.origin;
			p.point[std::size_t(a)] += side ? g.n[a]*g.h : 0.0;
			p.normal = {{0, 0, 0}};
			p.normal[std::size_t(a)] = side ? -1.0 : 1.0;
			p.restitution = 0;
			p.friction = 0.3;
			planes.push_back(p);
		}
	}
	for (LiquidBody& b : model_.bodies) {
		// Coupled port bodies too: rigid.bodies owns their contact, but inside the macro
		// step the liquid's own copy must not pass through a wall (it squeezed the liquid
		// between them and returned a spurious impulse that the contact then doubled).
		if (!b.Coupled()) continue;
		if (b.state.support.empty() && !b.state.sphere) {
			Rigid::Shape rs;
			rs.type = b.shape.type == BodyShape::Type::Sphere ? Rigid::Shape::Type::Sphere :
				b.shape.type == BodyShape::Type::Box ? Rigid::Shape::Type::Box : Rigid::Shape::Type::Cylinder;
			rs.size = b.shape.size;
			rs.radius = b.shape.radius;
			rs.height = b.shape.height;
			Rigid::ShapeSupportPoints(rs, b.state.support);
			b.state.sphere = rs.type == Rigid::Shape::Type::Sphere;
			b.state.radius = b.shape.radius;
		}
		const Rigid::V3 v0 = b.state.v;
		const int n = Rigid::ResolvePlanes(b.state, planes, setup_.gravity, dt);
		if (n) {
			contacts_ += std::uint64_t(n);
			for (int a = 0; a < 3; ++a) if (!b.free[std::size_t(a)]) b.state.v[std::size_t(a)] = v0[std::size_t(a)];
			if (!b.free[3]) b.state.pi = {{0, 0, 0}};
		}
	}
}

bool LiquidSolver::OfferFields(IPortExchange* ports, std::string& error)
{
	if (!ports || !publishFields_) return true;
	GridFrame f;
	auto bytes = std::make_shared<Bytes>();
	if (!model_.Fields(f, error) || !EncodeGrid(f, CodecBudget, *bytes, error)) return false;
	return ports->Offer(FieldsPort, s_.t, bytes, error);
}

bool LiquidSolver::Publish(IFrameSink& sink, IPortExchange* ports, AdvanceResult& result, std::string& error)
{
	const std::int64_t index = static_cast<std::int64_t>(s_.next);
	{
		MeshFrame m;
		auto bytes = std::make_shared<Bytes>();
		Bytes staticPayload;
		std::string staticDigest;
		if (!model_.Surface(m, error) || !EncodeMeshV2(m, MeshStaticLayout(), CodecBudget, *bytes, staticPayload, staticDigest, error))
			return false;
		if (!sink.Publish(index, SurfaceOutput, s_.t, bytes, {}, error)) return false;
		++result.framesPublished;
	}
	if (publishFields_) {
		GridFrame f;
		auto bytes = std::make_shared<Bytes>();
		if (!model_.Fields(f, error) || !EncodeGrid(f, CodecBudget, *bytes, error)) return false;
		if (!sink.Publish(index, FieldsOutput, s_.t, bytes, {}, error)) return false;
		if (ports && !ports->Offer(FieldsPort, s_.t, bytes, error)) return false;
		++result.framesPublished;
	}
	if (publishParticles_) {
		PointsFrame f;
		auto bytes = std::make_shared<Bytes>();
		if (!model_.Points(f, error) || !EncodePoints(f, CodecBudget, *bytes, error)) return false;
		if (!sink.Publish(index, ParticlesOutput, s_.t, bytes, {}, error)) return false;
		++result.framesPublished;
	}
	if (!model_.bodies.empty()) {
		std::shared_ptr<const Bytes> bytes;
		if (!EncodeBodies(s_.t, bytes, error) || !sink.Publish(index, BodiesOutput, s_.t, bytes, {}, error)) return false;
		++result.framesPublished;
	}
	return true;
}

bool LiquidSolver::Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
	AdvanceResult& result, std::string& error)
{
	result = AdvanceResult{};
	if (!started_) return Fail(error, "start or restore first");
	const bool macro = request.mode == AdvanceRequest::Mode::MacroStep;
	if (macro && (!std::isfinite(request.macroEnd) || request.macroEnd < s_.t)) return Fail(error, "macro end lies in the past");
	const double end = static_cast<double>(count_ ? count_ - 1 : 0)*interval_;
	bool offered = false;
	// M7b: port bodies follow rigid.bodies through a group: their state at each
	// macro step's start, the liquid's impulse on them offered at its end.
	if (portBodies_ && (!macro || !ports))
		return Fail(error, "the liquid's port bodies follow rigid.bodies: run it in a simulation_group (the liquid owning the exchange)");
	if (portBodies_ && !macroOpen_ && s_.t < request.macroEnd && !s_.finished) {
		if (!AcceptBodies(ports, error)) return false;
		model_.ResetBodyImpulses();
		macroOpen_ = true;
		macroStart_ = s_.t;
	}
	auto closeMacro = [&]() {
		if (!portBodies_ || s_.t != request.macroEnd) return true;
		const double span = macroOpen_ ? s_.t - macroStart_ : 0.0;
		macroOpen_ = false;
		if (portNames_.empty()) return true;   // nothing accepted yet (the start): no load
		return OfferLoads(ports, s_.t, span, error);
	};
	for (;;) {
		while (!s_.finished && s_.t == OutputTime(s_.next)) {
			if (!Publish(sink, ports, result, error)) return false;
			offered = true;
			if (++s_.next == count_) s_.finished = true;
			if (!macro) return Done(result, AdvanceResult::Status::Advanced);
		}
		if (s_.finished) {
			if (!closeMacro()) return false;
			return Done(result, AdvanceResult::Status::Finished);
		}
		if (macro && s_.t == request.macroEnd && s_.segN == 0) {
			if (!offered && !OfferFields(ports, error)) return false;
			if (!closeMacro()) return false;
			return Done(result, AdvanceResult::Status::Advanced);
		}
		if (s_.segN == 0) {
			s_.segStart = s_.t;
			s_.segTarget = macro ? std::min(OutputTime(s_.next), request.macroEnd) : OutputTime(s_.next);
			const double len = s_.segTarget - s_.segStart;
			const double dtmax = model_.MaxStep(len);
			s_.segN = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(len/dtmax*(1 - 1e-12))));
			s_.segJ = 0;
		} else if (macro && s_.segTarget > request.macroEnd) return Fail(error, "macro end inside an interrupted segment");
		offered = false;
		while (s_.segJ < s_.segN) {
			if (stop_.load()) {
				stop_ = false;
				return Done(result, AdvanceResult::Status::Stopped);
			}
			// The safeguard's count (StepStats::clamped totals of this run; zero in the
			// robustness sweep since m7a.2) in the progress detail, as cloth reports its solver counts.
			if (progress && !progress->Report(s_.t, end, "steps " + std::to_string(model_.steps) + ", clamped faces " +
				std::to_string(model_.clampedFaces) + " in " + std::to_string(model_.clampedSteps) + " steps" +
				(model_.bodies.empty() ? std::string() : ", body wall contacts " + std::to_string(contacts_))))
				return Done(result, AdvanceResult::Status::Stopped);
			const double tn = s_.segJ + 1 == s_.segN ? s_.segTarget :
				s_.segStart + (s_.segTarget - s_.segStart)*static_cast<double>(s_.segJ + 1)/static_cast<double>(s_.segN);
			StepStats stats;
			model_.SetTime(s_.t);
			if (!model_.Step(tn - s_.t, stats, error)) return false;
			if (!model_.bodies.empty()) WallContact(tn - s_.t);
			s_.t = tn;
			++s_.segJ;
		}
		s_.segN = s_.segJ = 0;
	}
}

// Stream: magic, u64 header length, canonical CBOR header, then phi, q, the
// three face components (binary64), the three validity masks (bytes), and
// the particles (x, y, z, radius, sign as binary64; id as u64), little endian.
bool LiquidSolver::CheckpointTo(ICheckpointSink& sink, std::string& error)
{
	if (!started_) return Fail(error, "nothing to checkpoint");
	const Grid& g = model_.GridSpec();
	const auto& ps = model_.particles.Particles();
	V::Members m{{"format", V::String(StateFormat)}, {"t", V::Float(s_.t)}, {"next", V::Unsigned(s_.next)},
		{"finished", V::Bool(s_.finished)}, {"segStart", V::Float(s_.segStart)}, {"segTarget", V::Float(s_.segTarget)},
		{"segN", V::Unsigned(s_.segN)}, {"segJ", V::Unsigned(s_.segJ)}, {"steps", V::Unsigned(model_.steps)},
		{"lastDt", V::Float(model_.lastDt_)}, {"targetVolume", V::Float(model_.targetVolume_)},
		{"cells", V::ArrayValue({V::Unsigned(std::uint64_t(g.n[0])), V::Unsigned(std::uint64_t(g.n[1])), V::Unsigned(std::uint64_t(g.n[2]))})},
		{"particles", V::Unsigned(ps.size())}, {"nextId", V::Unsigned(model_.particles.NextId())}};
	if (!model_.bodies.empty()) {
		// M7b: the bodies' state and the open port exchange (only when there are bodies).
		std::vector<double> packed;
		for (std::size_t i = 0; i < model_.bodies.size(); ++i) PackBody(model_.bodies[i], model_.bodyAccumulated[i], packed);
		V::Values names;
		for (const auto& n : portNames_) names.push_back(V::String(n));
		m.emplace_back("bodies", V::BytesValue(DoublesBytes(packed)));
		m.emplace_back("macroOpen", V::Bool(macroOpen_));
		m.emplace_back("macroStart", V::Float(macroStart_));
		m.emplace_back("portNames", V::ArrayValue(names));
		m.emplace_back("contacts", V::Unsigned(contacts_));
		if (setup_.viscosity > 0) {
			// m7c: the last step's impulse on each body (the viscous solve predicts the body's
			// wall velocity with it; only with viscosity, so m7b streams are unchanged).
			std::vector<double> last;
			for (const Vec6& I : model_.bodyImpulse) last.insert(last.end(), I.begin(), I.end());
			for (std::size_t i = 0; i < model_.bodies.size(); ++i) {
				const Vec6 I = i < model_.bodyViscousImpulse.size() ? model_.bodyViscousImpulse[i] : Vec6{{0, 0, 0, 0, 0, 0}};
				last.insert(last.end(), I.begin(), I.end());
			}
			m.emplace_back("bodyImpulse", V::BytesValue(DoublesBytes(last)));
		}
	}
	Bytes header;
	if (!RISECBOR64::Encode(V::MapValue(m), header, &error)) return false;
	StreamWriter w(sink);
	if (!w.Raw(StateMagic, 8, error) || !w.U64(header.size(), error) || !w.Raw(header.data(), header.size(), error) ||
		!w.Doubles(model_.phi, error) || !w.Doubles(model_.q_, error)) return false;
	for (int a = 0; a < 3; ++a) if (!w.Doubles(model_.u.u[a], error)) return false;
	for (int a = 0; a < 3; ++a) if (!w.Raw(model_.valid[a].data(), model_.valid[a].size(), error)) return false;
	std::vector<double> pd;
	pd.reserve(5*ps.size());
	for (const auto& q : ps) { pd.push_back(q.x[0]); pd.push_back(q.x[1]); pd.push_back(q.x[2]); pd.push_back(q.radius); pd.push_back(q.sign); }
	if (!w.Doubles(pd, error)) return false;
	for (const auto& q : ps) if (!w.U64(q.id, error)) return false;
	return true;
}

bool LiquidSolver::RestoreFrom(ICheckpointSource& source, std::string& error)
{
	if (!configured_) return Fail(error, "configure before restore");
	StreamReader r(source);
	char magic[8];
	std::uint64_t length = 0;
	if (!r.Raw(magic, 8, error) || std::memcmp(magic, StateMagic, 8) != 0) return Fail(error, "not a liquid.mac checkpoint");
	if (!r.U64(length, error) || length > (std::uint64_t(1) << 20) || length > r.Remaining()) return Fail(error, "invalid liquid checkpoint header");
	Bytes header(static_cast<std::size_t>(length));
	if (!r.Raw(header.data(), header.size(), error)) return false;
	V v;
	if (!CanDecodeCanonicalRecord(header, std::size_t(1) << 20) || !RISECBOR64::DecodeCanonical(header, v, &error) || v.GetType() != V::Map)
		return Fail(error, "invalid liquid checkpoint header");
	auto real = [&](const char* k, double& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::Float64) return false; x = f->GetFloat(); return true; };
	auto count = [&](const char* k, std::uint64_t& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::UnsignedInteger) return false; x = f->GetIntegerArgument(); return true; };
	auto flag = [&](const char* k, bool& x) { const V* f = v.Find(k); if (!f || f->GetType() != V::Boolean) return false; x = f->GetBoolean(); return true; };
	const V* format = v.Find("format");
	const V* cells = v.Find("cells");
	Segment seg;
	std::uint64_t steps = 0, particles = 0, nextId = 0;
	double lastDt = 0, target = 0;
	if (!format || format->GetType() != V::Text || format->GetText() != StateFormat || !real("t", seg.t) || !count("next", seg.next) ||
		!flag("finished", seg.finished) || !real("segStart", seg.segStart) || !real("segTarget", seg.segTarget) ||
		!count("segN", seg.segN) || !count("segJ", seg.segJ) || !count("steps", steps) || !real("lastDt", lastDt) ||
		!real("targetVolume", target) || !count("particles", particles) || !count("nextId", nextId) ||
		seg.next > count_ || seg.segJ > seg.segN || !cells || cells->GetType() != V::Array || cells->GetArray().size() != 3)
		return Fail(error, "invalid liquid checkpoint fields");
	for (int a = 0; a < 3; ++a) {
		const V& c = cells->GetArray()[std::size_t(a)];
		if (c.GetType() != V::UnsignedInteger || c.GetIntegerArgument() != std::uint64_t(setup_.grid.n[a]))
			return Fail(error, "the liquid checkpoint is for another grid");
	}
	std::vector<double> packed;
	bool macroOpen = false;
	double macroStart = 0;
	std::vector<std::string> portNames;
	std::uint64_t contacts = 0;
	std::vector<double> lastImpulse;
	if (!setup_.bodies.empty() && setup_.viscosity > 0 && !BytesDoubles(v.Find("bodyImpulse"), 12*setup_.bodies.size(), lastImpulse))
		return Fail(error, "the liquid checkpoint does not hold the bodies' last impulses (viscosity)");
	if (!setup_.bodies.empty()) {
		const V* names = v.Find("portNames");
		if (!BytesDoubles(v.Find("bodies"), BodyDoubles*setup_.bodies.size(), packed) || !flag("macroOpen", macroOpen) ||
			!real("macroStart", macroStart) || !count("contacts", contacts) || !names || names->GetType() != V::Array)
			return Fail(error, "the liquid checkpoint does not hold the configured bodies");
		for (const V& n : names->GetArray()) {
			if (n.GetType() != V::Text) return Fail(error, "invalid liquid checkpoint fields");
			portNames.push_back(n.GetText());
		}
	} else if (v.Find("bodies")) return Fail(error, "the liquid checkpoint holds bodies the configuration does not have");
	// The static data (solid fractions) come from the configuration.
	if (!model_.Initialize(setup_, options_, threads_, error)) return false;
	const Grid& g = model_.GridSpec();
	std::vector<double> phi, q;
	MacField u;
	FaceMask valid;
	if (!r.Doubles(phi, g.Cells(), error) || !r.Doubles(q, g.Cells(), error)) return false;
	for (int a = 0; a < 3; ++a) if (!r.Doubles(u.u[a], g.Faces(a), error)) return false;
	for (int a = 0; a < 3; ++a) {
		valid[a].resize(g.Faces(a));
		if (!r.Raw(valid[a].data(), valid[a].size(), error)) return false;
	}
	if (particles > r.Remaining()/48) return Fail(error, "liquid checkpoint stream is truncated");
	std::vector<double> pd;
	if (!r.Doubles(pd, 5*std::size_t(particles), error)) return false;
	std::vector<MarkerParticles::Particle> ps(static_cast<std::size_t>(particles));
	for (std::size_t i = 0; i < ps.size(); ++i) {
		ps[i].x = {{pd[5*i], pd[5*i + 1], pd[5*i + 2]}};
		ps[i].radius = pd[5*i + 3];
		ps[i].sign = pd[5*i + 4];
		if (!r.U64(ps[i].id, error)) return false;
	}
	if (r.Remaining() != 0) return Fail(error, "trailing bytes after the liquid checkpoint");
	model_.phi = std::move(phi);
	model_.q_ = std::move(q);
	model_.u = std::move(u);
	model_.valid = std::move(valid);
	model_.lastDt_ = lastDt;
	model_.targetVolume_ = target;
	model_.steps = steps;
	model_.particles.SetState(std::move(ps), nextId, options_.particle);
	if (!setup_.bodies.empty()) {
		for (std::size_t i = 0; i < model_.bodies.size(); ++i) UnpackBody(&packed[BodyDoubles*i], model_.bodies[i], model_.bodyAccumulated[i]);
		if (!lastImpulse.empty()) {
			const std::size_t nb = model_.bodies.size();
			model_.bodyViscousImpulse.assign(nb, Vec6{{0, 0, 0, 0, 0, 0}});
			for (std::size_t i = 0; i < nb; ++i) for (int k = 0; k < 6; ++k) {
				model_.bodyImpulse[i][std::size_t(k)] = lastImpulse[6*i + std::size_t(k)];
				model_.bodyViscousImpulse[i][std::size_t(k)] = lastImpulse[6*(nb + i) + std::size_t(k)];
			}
		}
		model_.UpdateBodyGeometry(false);
		model_.SetTime(seg.t);
	}
	macroOpen_ = macroOpen;
	macroStart_ = macroStart;
	portNames_ = std::move(portNames);
	contacts_ = contacts;
	s_ = seg;
	started_ = true;
	return true;
}

bool LiquidSolver::Checkpoint(Bytes& state, std::string& error)
{
	MemoryCheckpointSink sink;
	if (!CheckpointTo(sink, error)) return false;
	state = std::move(sink.bytes);
	return true;
}

bool LiquidSolver::Restore(const Bytes& state, std::string& error)
{
	MemoryCheckpointSource source(state);
	return RestoreFrom(source, error);
}

bool LiquidSolver::SaveState(std::shared_ptr<const SolverStateToken>& token, std::string& error)
{
	if (!started_) return Fail(error, "nothing to save");
	auto t = std::make_shared<LiquidToken>();
	t->owner = this;
	t->seg = s_;
	t->phi = model_.phi;
	t->q = model_.q_;
	t->u = model_.u;
	t->valid = model_.valid;
	t->lastDt = model_.lastDt_;
	t->targetVolume = model_.targetVolume_;
	t->steps = model_.steps;
	t->nextId = model_.particles.NextId();
	t->particles = model_.particles.Particles();
	t->bodies = model_.bodies;
	t->accumulated = model_.bodyAccumulated;
	t->last = model_.bodyImpulse;
	t->lastViscous = model_.bodyViscousImpulse;
	t->macroOpen = macroOpen_;
	t->macroStart = macroStart_;
	t->portNames = portNames_;
	t->contacts = contacts_;
	token = std::move(t);
	return true;
}

bool LiquidSolver::RestoreState(const SolverStateToken& token, std::string& error)
{
	const auto* t = dynamic_cast<const LiquidToken*>(&token);
	if (!t || t->owner != this || !configured_ || !started_) return Fail(error, "state token belongs to another solver");
	s_ = t->seg;
	model_.phi = t->phi;
	model_.q_ = t->q;
	model_.u = t->u;
	model_.valid = t->valid;
	model_.lastDt_ = t->lastDt;
	model_.targetVolume_ = t->targetVolume;
	model_.steps = t->steps;
	model_.particles.SetState(t->particles, t->nextId, options_.particle);
	if (!t->bodies.empty()) {
		model_.bodies = t->bodies;
		model_.bodyAccumulated = t->accumulated;
		model_.bodyImpulse = t->last;
		model_.bodyViscousImpulse = t->lastViscous;
		model_.UpdateBodyGeometry(false);
		model_.SetTime(t->seg.t);
	}
	macroOpen_ = t->macroOpen;
	macroStart_ = t->macroStart;
	portNames_ = t->portNames;
	contacts_ = t->contacts;
	return true;
}
} // namespace

bool RegisterLiquidSolver(std::string& error)
{
	static std::mutex once;
	static bool registered = false;
	std::lock_guard<std::mutex> hold(once);
	if (registered) return true;
	if (!RegisterSolver(LiquidSolverId, []{ return std::unique_ptr<ISimulationSolver>(new LiquidSolver()); }, error)) return false;
	registered = true;
	return true;
}

} }
