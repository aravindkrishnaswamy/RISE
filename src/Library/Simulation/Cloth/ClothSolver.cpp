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
#include "../SimulationGrid.h"
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
constexpr const char* SolverVersion = "m3a.1";
constexpr const char* StateFormat = "cloth.shell.state.v1";
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

bool ParseMotion(const V* v, Motion& motion, V& effective, std::string& error)
{
	if (!v) { motion = Motion{}; effective = V::MapValue({{"type", V::String("fixed")}}); return true; }
	Reader r(*v, "pin motion", error);
	const std::string type = r.Text("type", "fixed", {"fixed", "translate", "rotate", "oscillate"});
	if (type == "translate") { motion.type = Motion::Type::Translate; motion.vector = r.Vector("velocity", {{0, 0, 0}}); }
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

bool ParsePins(const V* v, const SheetSpec& sheet, std::vector<PinGroup>& pins, V& effective, std::string& error)
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
		if (!ParseMotion(r.Raw("motion"), group.motion, motionValue, error)) return false;
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

bool ParseColliders(const V* v, std::vector<Collider>& colliders, V& effective, std::string& error)
{
	V::Values out;
	colliders.clear();
	if (v && v->GetType() != V::Array) return Fail(error, "colliders must be an array");
	if (v) for (const V& item : v->GetArray()) {
		Reader r(item, "collider", error);
		const std::string type = r.Text("type", "plane", {"plane", "sphere", "box"});
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
		} else {
			c.type = Collider::Type::Box;
			c.a = r.Vector("min", {{-0.5, -0.5, -0.5}});
			c.b = r.Vector("max", {{0.5, 0.5, 0.5}});
			if (!(c.a[0] < c.b[0] && c.a[1] < c.b[1] && c.a[2] < c.b[2])) r.Bad("box min must be below max");
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

struct EvidenceKey {
	Material material;
	Triangulation triangulation;
	BendingModel bending;
	Scheme scheme;
};

V EvidenceConfiguration(const EvidenceKey& k, const char* id, int resolution)
{
	return V::MapValue({{"solver", V::String(ClothSolverId)}, {"version", V::String(SolverVersion)},
		{"case", V::String(id)}, {"resolution", V::Unsigned(static_cast<std::uint64_t>(resolution))},
		{"material", MaterialToValue(k.material)}, {"triangulation", V::String(TriangulationName(k.triangulation))},
		{"bending_model", V::String(BendingModelName(k.bending))}, {"integrator", V::String(SchemeName(k.scheme))}});
}

std::vector<EvidenceCase> RunEvidence(const EvidenceKey& k)
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

// Deterministic, so a process-wide memo is exact.
std::vector<EvidenceCase> Evidence(const EvidenceKey& k)
{
	static std::mutex mutex;
	static std::map<std::string, std::vector<EvidenceCase>> memo;
	Bytes key;
	std::string ignored;
	RISECBOR64::Encode(EvidenceConfiguration(k, "all", 0), key, &ignored);
	const std::string digest = RISECBOR64::SHA256Hex(key);
	{
		std::lock_guard<std::mutex> hold(mutex);
		const auto it = memo.find(digest);
		if (it != memo.end()) return it->second;
	}
	std::vector<EvidenceCase> cases = RunEvidence(k);
	std::lock_guard<std::mutex> hold(mutex);
	memo[digest] = cases;
	return cases;
}

// ------------------------------------------------------------------ solver

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

private:
	double OutputTime(std::int64_t i) const { return static_cast<double>(i)*interval_; }
	bool Done(AdvanceResult& r, AdvanceResult::Status s) {
		r.status = finished_ ? AdvanceResult::Status::Finished : s;
		r.position = Position();
		return true;
	}
	bool Publish(std::int64_t index, double time, IFrameSink& sink, IPortExchange* ports, std::string& error);
	bool AcceptLoads(double time, IPortExchange* ports, std::string& error);

	bool configured_ = false, started_ = false, finished_ = false;
	SheetSpec sheet_;
	ModelConfig model_;
	Scheme scheme_ = Scheme::BDF2;
	NewtonOptions newton_;
	std::uint64_t count_ = 0, substeps_ = 1;
	double interval_ = 1.0/30;
	unsigned paceMs_ = 0;
	ClothSystem system_;
	std::vector<std::uint32_t> indices_;
	double t_ = 0, segStart_ = 0, segTarget_ = 0;
	std::int64_t next_ = 0;
	std::uint64_t segN_ = 0, segJ_ = 0;
	StepStats stats_;
	std::atomic<bool> stop_{false};
};

bool ClothSolver::Configure(const V& parameters, const ISceneInputs&, SolverDescription& d, std::string& error)
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
	if (!ParsePins(p.Raw("pins"), sheet, model.pins, pinsValue, error)) return false;
	p.out.emplace_back("pins", pinsValue);
	if (!ParseColliders(p.Raw("colliders"), model.colliders, collidersValue, error)) return false;
	p.out.emplace_back("colliders", collidersValue);
	model.contactStiffness = p.Real("contact_stiffness", 1e5);
	model.contactThickness = p.Real("contact_thickness", 0.002);
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
	const std::string solver = p.Text("linear_solver", "auto", {"auto", "pcg", "direct"});
	newton.solver = solver == "auto" ? LinearSolver::Auto : solver == "pcg" ? LinearSolver::ConjugateGradient : LinearSolver::Direct;
	newton.requireConvergence = !interactive;
	const unsigned pace = static_cast<unsigned>(p.Count("pace_ms", 0, 0, 10000));
	p.out.pop_back(); // pace never enters the key
	SolverDescription candidate;
	if (!p.Finish()) return false;
	candidate.effectiveParameters = V::MapValue(p.out);
	if (!(sheet.sizeU > 0) || !(sheet.sizeV > 0) || !(sheet.stretchU > 0) || !(sheet.stretchV > 0))
		return Fail(error, "sheet size and stretch must be positive");
	if (!(rate > 0)) return Fail(error, "frame_rate must be positive");
	if (!(model.contactStiffness >= 0) || !(model.contactThickness >= 0) || !(model.dragCoefficient >= 0) ||
		!(model.airDensity >= 0)) return Fail(error, "contact and drag parameters must not be negative");
	if (!(newton.tolerance > 0) || !(newton.cgTolerance > 0) || newton.cgTolerance >= 1)
		return Fail(error, "solver tolerances must be positive (cg_tolerance below 1)");
	Sheet built;
	ClothSystem probe;
	if (!BuildSheet(sheet, model.material, built, error) || !probe.Initialize(built, model, error)) return false;
	sheet_ = sheet; model_ = model; newton_ = newton;
	scheme_ = integrator == "bdf2" ? Scheme::BDF2 : integrator == "backward_euler" ? Scheme::BackwardEuler : Scheme::ImplicitMidpoint;
	count_ = frames; substeps_ = substeps; interval_ = 1/rate; paceMs_ = pace;
	indices_.clear();
	for (const auto& t : built.triangles) indices_.insert(indices_.end(), {t.v[0], t.v[1], t.v[2]});
	configured_ = true;

	candidate.solverId = ClothSolverId;
	candidate.version = SolverVersion;
	candidate.cache.firstIndex = 0;
	candidate.cache.count = count_;
	candidate.cache.startTime = 0;
	candidate.cache.interval = interval_;
	candidate.cache.outputs = {{"surface", "mesh.v1", {{"normal", "1", "f32x3"}, {"position", "m", "f32x3"}, {"velocity", "m/s", "f32x3"}}}};
	Bytes records;
	if (!RISECBOR64::Encode(V::MapValue({{"format", V::String("cloth.material.v1")}, {"material", MaterialToValue(model.material)},
		{"units", MaterialUnits()}, {"damping", V::String("calibrated, not measured")}}), records, &error)) return false;
	candidate.cache.physicalRecords = records;
	candidate.ports = {
		{StatePort, "surface", "m", "mesh.v1", true, "surface"},
		{LoadPort, "surface load", "Pa", "grid.v1", false, ""}};
	auto& caps = candidate.capabilities;
	caps.backends = {{"cpu", BackendStatus::Supported, true, "portable C++17, single thread"}};
	caps.interactive = caps.deterministic = caps.resumable = caps.externalMacroStep = true;
	caps.fidelityModes = {"preview", "predictive"};
	const std::uint64_t n = built.Vertices();
	// About a dozen 3-vectors of doubles per vertex (state, history, Newton work)
	// plus three block matrices (pattern, damping, Hessian) of ~13 3x3 blocks per row.
	caps.memoryEstimate = n*(12*24 + 3*13*72) + (std::uint64_t(1) << 20);
	const bool predictiveConditions = !interactive && model.colliders.empty() && model.dragCoefficient == 0;
	candidate.evidence.fidelity = predictiveConditions ? "predictive" : "preview";
	candidate.evidence.cases = Evidence({model.material, sheet.triangulation, sheet.bending, scheme_});
	candidate.notes.push_back("material " + model.material.name + ": " + model.material.source);
	candidate.notes.push_back("Rayleigh damping is calibrated, not measured");
	if (interactive) candidate.notes.push_back("interactive mode: truncated Newton, preview only");
	if (!model.colliders.empty()) candidate.notes.push_back("penalty contact without collision detection: preview only");
	if (model.dragCoefficient > 0) candidate.notes.push_back("air drag is explicit and unvalidated: preview only");
	d = std::move(candidate);
	return true;
}

bool ClothSolver::Start(std::string& error)
{
	if (!configured_) return Fail(error, "configure first");
	Sheet sheet;
	if (!BuildSheet(sheet_, model_.material, sheet, error) || !system_.Initialize(sheet, model_, error)) return false;
	system_.Reset(0);
	system_.portPressure.clear();
	system_.portTraction.clear();
	t_ = 0; next_ = 0; finished_ = count_ == 0; segN_ = segJ_ = 0; segStart_ = segTarget_ = 0;
	stats_ = StepStats{};
	started_ = true;
	return true;
}

bool ClothSolver::Publish(std::int64_t index, double time, IFrameSink& sink, IPortExchange* ports, std::string& error)
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
	auto bytes = std::make_shared<Bytes>();
	if (!EncodeMesh(m, CodecBudget, *bytes, error)) return false;
	std::shared_ptr<const Bytes> payload = std::move(bytes);
	if (!sink.Publish(index, "surface", time, payload, {}, error)) return false;
	if (ports && !ports->Offer(StatePort, time, payload, error)) return false;
	return true;
}

// surface_load: grid.v1 on the vertex lattice (dimensions nu x nv x 1, origin
// 0); channel "pressure" (1 component, Pa, on the front face) and/or
// "traction" (3 components, Pa, world frame).  Cells without a block take
// the channel background.
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
	GridFrame g;
	if (!DecodeGrid(*bytes, CodecBudget, g, error)) return false;
	const std::uint32_t nu = sheet_.verticesU, nv = sheet_.verticesV;
	std::vector<double> pressure, traction;
	for (const auto& c : g.channels) {
		const bool isPressure = c.name == "pressure" && c.components == 1;
		const bool isTraction = c.name == "traction" && c.components == 3;
		if (!isPressure && !isTraction) return Fail(error, "surface_load channels are pressure (1 component) and traction (3)");
		if (c.dimensions[0] != nu || c.dimensions[1] != nv || c.dimensions[2] != 1 || c.origin[0] != 0 || c.origin[1] != 0 ||
			c.origin[2] != 0 || c.units != "Pa")
			return Fail(error, "surface_load must cover the sheet's vertex lattice in Pa");
		std::vector<double>& target = isPressure ? pressure : traction;
		const unsigned comps = c.components;
		target.assign(static_cast<std::size_t>(nu)*nv*comps, 0.0);
		for (std::uint32_t j = 0; j < nv; ++j) for (std::uint32_t i = 0; i < nu; ++i) {
			const std::size_t q = i + static_cast<std::size_t>(nu)*j;
			for (unsigned k = 0; k < comps; ++k) target[comps*q + k] = FromF32(c.background[k]);
			for (const auto& b : c.blocks) {
				const std::int64_t li = static_cast<std::int64_t>(i) - b.origin[0], lj = static_cast<std::int64_t>(j) - b.origin[1];
				if (li < 0 || lj < 0 || b.origin[2] > 0 || li >= b.dimensions[0] || lj >= b.dimensions[1] ||
					-b.origin[2] >= static_cast<std::int64_t>(b.dimensions[2])) continue;
				const std::size_t cell = b.constant ? 0 :
					static_cast<std::size_t>(li + b.dimensions[0]*(lj + static_cast<std::int64_t>(b.dimensions[1])*(-b.origin[2])));
				for (unsigned k = 0; k < comps; ++k) target[comps*q + k] = FromF32(b.values[comps*cell + k]);
			}
		}
	}
	system_.portPressure.swap(pressure);
	system_.portTraction.swap(traction);
	return true;
}

bool ClothSolver::Advance(const AdvanceRequest& request, IFrameSink& sink, IPortExchange* ports, IProgress* progress,
	AdvanceResult& result, std::string& error)
{
	stop_ = false;
	result = AdvanceResult{};
	if (!started_) return Fail(error, "start or restore first");
	const bool macro = request.mode == AdvanceRequest::Mode::MacroStep;
	if (macro && (!std::isfinite(request.macroEnd) || request.macroEnd < t_)) return Fail(error, "macro end lies in the past");
	const double end = static_cast<double>(count_)*interval_; // as the reference solvers report it
	for (;;) {
		while (!finished_ && t_ == OutputTime(next_)) {
			if (!Publish(next_, t_, sink, ports, error)) return false;
			++result.framesPublished;
			if (static_cast<std::uint64_t>(++next_) == count_) finished_ = true;
			if (paceMs_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(paceMs_));
			if (!macro) return Done(result, AdvanceResult::Status::Advanced);
		}
		if (finished_) return Done(result, AdvanceResult::Status::Finished);
		if (macro && t_ == request.macroEnd && segN_ == 0) return Done(result, AdvanceResult::Status::Advanced);
		if (segN_ == 0) {
			segStart_ = t_;
			segTarget_ = macro ? std::min(OutputTime(next_), request.macroEnd) : OutputTime(next_);
			const double nominal = interval_/static_cast<double>(substeps_);
			segN_ = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil((segTarget_ - segStart_)/nominal*(1 - 1e-12))));
			segJ_ = 0;
			if (!AcceptLoads(segStart_, ports, error)) return false;
		} else if (macro && segTarget_ > request.macroEnd) return Fail(error, "macro end inside an interrupted segment");
		while (segJ_ < segN_) {
			if (stop_.load()) return Done(result, AdvanceResult::Status::Stopped);
			if (progress) {
				std::ostringstream detail;
				detail << "newton " << stats_.newtonIterations << ", cg " << stats_.cgIterations << ", direct "
					<< stats_.directSolves << ", halvings " << stats_.halvings << ", unconverged " << stats_.unconverged << ", projected " << stats_.projections;
				if (!progress->Report(t_, end, detail.str())) return Done(result, AdvanceResult::Status::Stopped);
			}
			const double tn = segJ_ + 1 == segN_ ? segTarget_ :
				segStart_ + (segTarget_ - segStart_)*static_cast<double>(segJ_ + 1)/static_cast<double>(segN_);
			if (!system_.Step(tn - t_, scheme_, newton_, stats_, error)) return false;
			system_.time = tn;
			t_ = tn;
			++segJ_;
		}
		segN_ = segJ_ = 0;
	}
}

bool ClothSolver::Checkpoint(Bytes& state, std::string& error)
{
	if (!started_) return Fail(error, "nothing to checkpoint");
	V::Members m{{"format", V::String(StateFormat)}, {"t", V::Float(t_)}, {"next", V::Unsigned(static_cast<std::uint64_t>(next_))},
		{"finished", V::Bool(finished_)}, {"segStart", V::Float(segStart_)}, {"segTarget", V::Float(segTarget_)},
		{"segN", V::Unsigned(segN_)}, {"segJ", V::Unsigned(segJ_)},
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
	double t = 0, s0 = 0, s1 = 0, hPrev = 0, time = 0;
	std::uint64_t next = 0, n = 0, j = 0;
	bool fin = false, havePrev = false;
	if (!format || format->GetType() != V::Text || format->GetText() != StateFormat || !real("t", t) || !count("next", next) ||
		!flag("finished", fin) || !real("segStart", s0) || !real("segTarget", s1) || !count("segN", n) || !count("segJ", j) ||
		!real("hPrev", hPrev) || !flag("havePrev", havePrev) || !real("time", time) || next > count_ || j > n)
		return Fail(error, "invalid cloth solver state fields");
	Sheet sheet;
	ClothSystem system;
	if (!BuildSheet(sheet_, model_.material, sheet, error) || !system.Initialize(sheet, model_, error)) return false;
	const std::size_t dofs = 3*static_cast<std::size_t>(sheet.Vertices());
	std::vector<double> x, vel, xp, vp, pressure, traction;
	if (!UnpackDoubles(v.Find("x"), dofs, x, false) || !UnpackDoubles(v.Find("v"), dofs, vel, false) ||
		!UnpackDoubles(v.Find("xPrev"), dofs, xp, true) || !UnpackDoubles(v.Find("vPrev"), dofs, vp, true) ||
		!UnpackDoubles(v.Find("pressure"), dofs/3, pressure, true) || !UnpackDoubles(v.Find("traction"), dofs, traction, true) ||
		(havePrev && (xp.empty() || vp.empty())))
		return Fail(error, "cloth solver state does not match the configured sheet");
	system.x.swap(x); system.v.swap(vel); system.xPrev.swap(xp); system.vPrev.swap(vp);
	system.hPrev = hPrev; system.havePrev = havePrev; system.time = time;
	system.portPressure.swap(pressure); system.portTraction.swap(traction);
	system_ = std::move(system);
	t_ = t; next_ = static_cast<std::int64_t>(next); finished_ = fin;
	segStart_ = s0; segTarget_ = s1; segN_ = n; segJ_ = j;
	stats_ = StepStats{};
	started_ = true;
	return true;
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
