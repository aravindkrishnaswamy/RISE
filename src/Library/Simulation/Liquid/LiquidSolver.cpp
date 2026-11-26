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
#include "../SimulationGrid.h"
#include "../SimulationMesh.h"
#include "../SimulationPoints.h"

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
constexpr const char* SolverVersion = "m7a.1";
constexpr const char* StateMagic = "RISELIQ1";
constexpr const char* StateFormat = "liquid.mac.state.v1";
constexpr const char* SurfaceOutput = "surface";
constexpr const char* FieldsOutput = "fields";
constexpr const char* ParticlesOutput = "particles";
constexpr const char* FieldsPort = "fields";
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
		{"min_face_fraction", V::Float(o.minFaceFraction)}, {"speed_limit", V::Float(o.speedLimit)},
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
	{"taylor-green-n32", "Taylor-Green vortex, closed box, 32 x 32, T = 2: rms face velocity error / max speed", 3.7e-4},
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
};

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
		o.minFaceFraction = p.Real("min_face_fraction", 0.5);
		o.speedLimit = p.Real("speed_limit", 3.0);
		o.extrapolationLayers = int(p.Count("extrapolation_layers", 6, 1, 64));
		o.interfaceExtrapolationLayers = int(p.Count("interface_extrapolation_layers", 4, 1, 64));
		o.linearExtrapolationLayers = int(p.Count("linear_extrapolation_layers", 1, 0, 8));
		o.pressure.openTop = setup.openTop;
		if (!(o.cfl > 0 && o.cfl <= 1) || !(o.waveCfl > 0 && o.waveCfl <= 2)) return p.Bad("cfl must be in (0, 1] and wave_cfl in (0, 2]");
		if (!(o.pressure.tolerance > 0 && o.pressure.tolerance < 1)) return p.Bad("pressure_tolerance must be in (0, 1)");
		if (!(o.pressure.thetaMin > 0 && o.pressure.thetaMin < 0.5)) return p.Bad("theta_min must be in (0, 1/2)");
		if (!(o.reinitThreshold >= 0)) return p.Bad("reinit_threshold must not be negative");
		if (!(o.minFaceFraction >= 0 && o.minFaceFraction < 1)) return p.Bad("min_face_fraction must be in [0, 1)");
		if (!(o.speedLimit >= 0)) return p.Bad("speed_limit must not be negative");
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
	if (setup.grid.Cells() > (std::size_t(1) << 31)) return Fail(error, "the grid is too large");
	candidate.effectiveParameters = V::MapValue(p.out);
	setup_ = setup;
	options_ = o;
	threads_ = threads;
	count_ = frames;
	interval_ = 1/rate;
	publishFields_ = fields;
	publishParticles_ = particlesOut;
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
	candidate.inputs = sceneNames;
	{
		Bytes records;
		if (!RISECBOR64::Encode(V::MapValue({{"format", V::String("liquid.mac.v1")}, {"density", V::Float(setup.density)},
			{"model", V::String("single-phase inviscid incompressible liquid, free surface p = 0, no surface tension")},
			{"units", V::MapValue({{"density", V::String("kg/m^3")}, {"length", V::String("m")}})}}), records, &error)) return false;
		candidate.cache.physicalRecords = records;
	}
	// One-way coupling: the fields output as a cached-output port.
	if (fields)
		candidate.ports = {{FieldsPort, "liquid fields", "m", "grid.v1", true, FieldsOutput,
			{{"phi", "signed distance", "m", 1}, {"pressure", "pressure", "Pa", 1}, {"velocity", "velocity", "m/s", 3}}, ""}};
	auto& caps = candidate.capabilities;
	caps.backends = {{"cpu", BackendStatus::Supported, true, "portable C++17; `threads` does not change results"}};
	const std::size_t cells = setup.grid.Cells();
	caps.interactive = cells <= InteractiveCells;
	caps.deterministic = caps.resumable = caps.externalMacroStep = caps.rollback = true;
	caps.threadCountInvariant = true;
	caps.modes = {"bake"};
	caps.backend = "cpu";
	// Measured (docs/SIMULATION_M7A_RESULTS.md section 8): peak RSS 356 bytes per cell at
	// 128^3 (a dam break; 565 at 64^3 with the fixed part), particles about 140 bytes
	// per cell at 16 per band cell, the fields output 48; declared with a 25% margin.
	caps.memoryEstimate = (std::uint64_t(64) << 20) + std::uint64_t(cells)*450 +
		(o.particles ? std::uint64_t(cells)*std::uint64_t(o.particle.perCell)*9 : 0) + (fields ? std::uint64_t(cells)*60 : 0);
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
	}
	const bool solids = !setup.solids.empty();
	candidate.evidence.fidelity = "predictive";
	caps.earnableFidelity = {"preview", "predictive"};
	candidate.evidence.conditions = {{"interface", {o.particles ? "particle-level-set" : "level-set"}},
		{"momentum", {MomentumName(o.momentum)}}, {"solids", {solids ? "static" : "none"}}, {"top", {setup.openTop ? "open" : "wall"}},
		{"volume_correction", {o.volumeCorrection ? "on" : "off"}}};
	candidate.evidence.scope = "Single-phase inviscid incompressible liquid with a free surface (p = 0, no surface tension) "
		"in a box with free-slip walls and static solids, on a uniform staggered grid: certified on canonical problems "
		"(interface transport, hydrostatic rest with a tilted surface and an immersed solid, linear standing waves at finite and "
		"infinite depth, rectangular-tank sloshing, the early-time collapse of a column, a Taylor-Green vortex, volume "
		"correction), second order on the smooth ones; it does not bound the error of a given bake and does not cover "
		"spray, thin sheets or any feature below about two cells, moving solids, viscosity, surface tension, or the accuracy "
		"of large-deformation flows (the interface is advanced with the step-start velocity, first order in time for "
		"nonlinear transport).";
	candidate.evidence.cases = ComputeEvidence(o, threads);
	if (!solids) candidate.notes.push_back("no solids: the domain walls (free slip) only");
	d = std::move(candidate);
	return true;
}

bool LiquidSolver::Start(std::string& error)
{
	if (!configured_) return Fail(error, "configure first");
	if (!model_.Initialize(setup_, options_, threads_, error)) return false;
	s_ = Segment();
	s_.finished = count_ == 0;
	started_ = true;
	return true;
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
	for (;;) {
		while (!s_.finished && s_.t == OutputTime(s_.next)) {
			if (!Publish(sink, ports, result, error)) return false;
			offered = true;
			if (++s_.next == count_) s_.finished = true;
			if (!macro) return Done(result, AdvanceResult::Status::Advanced);
		}
		if (s_.finished) return Done(result, AdvanceResult::Status::Finished);
		if (macro && s_.t == request.macroEnd && s_.segN == 0) {
			if (!offered && !OfferFields(ports, error)) return false;
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
			if (progress && !progress->Report(s_.t, end, std::string())) return Done(result, AdvanceResult::Status::Stopped);
			const double tn = s_.segJ + 1 == s_.segN ? s_.segTarget :
				s_.segStart + (s_.segTarget - s_.segStart)*static_cast<double>(s_.segJ + 1)/static_cast<double>(s_.segN);
			StepStats stats;
			if (!model_.Step(tn - s_.t, stats, error)) return false;
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
