// Simulation M6: see SimulationPortAdapters.h.
#include "pch.h"
#include "SimulationPortAdapters.h"
#include "SimulationField.h"
#include "SimulationGrid.h"
#include "SimulationMesh.h"
#include "SimulationTransform.h"
#include "Rigid/RigidSolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace RISE { namespace Simulation {

namespace {
using V = RISECBOR64::Value;
constexpr std::size_t CodecBudget = std::size_t(1) << 30;

bool Fail(std::string& error, const std::string& message) { error = message; return false; }

double FromF32(std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
std::uint32_t F32(double x) { const float f = static_cast<float>(x); std::uint32_t b; std::memcpy(&b, &f, 4); return b; }

bool Number(const V& v, double& x)
{
	if (v.GetType() == V::Float64) x = v.GetFloat();
	else if (v.GetType() == V::UnsignedInteger) x = static_cast<double>(v.GetIntegerArgument());
	else if (v.GetType() == V::NegativeInteger) x = -1.0 - static_cast<double>(v.GetIntegerArgument());
	else return false;
	return std::isfinite(x);
}

struct FactoryRegistry {
	std::mutex mutex;
	std::vector<std::pair<std::string,PortAdapterFactory>> factories;
};
FactoryRegistry& Factories()
{
	static FactoryRegistry registry;
	return registry;
}

// Strict parameter map reader for adapters: unknown keys are refused, every
// key read is written back canonically (the identity's parameters).
class Params {
public:
	Params(const V& map, const char* where, std::string& error) : map_(map), where_(where), error_(error) {
		if (map.GetType() != V::Map) Bad("parameters must be a map");
	}
	double Real(const char* key, double fallback) {
		double x = fallback;
		if (const V* v = Use(key)) if (!Number(*v, x)) Bad(std::string(key) + " must be a finite number");
		out.emplace_back(key, V::Float(x));
		return x;
	}
	std::string Text(const char* key, const std::string& fallback) {
		std::string s = fallback;
		if (const V* v = Use(key)) {
			if (v->GetType() == V::Text) s = v->GetText();
			else Bad(std::string(key) + " must be text");
		}
		out.emplace_back(key, V::String(s));
		return s;
	}
	const V* Raw(const char* key) { return Use(key); }
	bool Finish() {
		if (!ok_) return false;
		if (map_.GetType() == V::Map)
			for (const auto& m : map_.GetMap())
				if (std::find(used_.begin(), used_.end(), m.first) == used_.end()) return Bad("unknown parameter " + m.first);
		return true;
	}
	bool Bad(const std::string& message) {
		if (ok_) error_ = std::string(where_) + ": " + message;
		ok_ = false;
		return false;
	}
	V Effective() const { return V::MapValue(out); }
	V::Members out;
private:
	const V* Use(const char* key) {
		used_.push_back(key);
		return ok_ && map_.GetType() == V::Map ? map_.Find(key) : nullptr;
	}
	const V& map_;
	const char* where_;
	std::string& error_;
	std::vector<std::string> used_;
	bool ok_ = true;
};

// ------------------------------------------------------------------ units

struct UnitRow { const char* quantity; const char* unit; double scale; }; // value in the quantity's base unit
const UnitRow Units[] = {
	{"length", "m", 1}, {"length", "cm", 1e-2}, {"length", "mm", 1e-3}, {"length", "km", 1e3},
	{"length", "in", 0.0254}, {"length", "ft", 0.3048},
	{"velocity", "m/s", 1}, {"velocity", "cm/s", 1e-2}, {"velocity", "mm/s", 1e-3}, {"velocity", "km/h", 1/3.6},
	{"mass", "kg", 1}, {"mass", "g", 1e-3},
	{"density", "kg/m^3", 1}, {"density", "kg/m3", 1}, {"density", "g/m^3", 1e-3}, {"density", "g/m3", 1e-3},
	{"density", "g/cm^3", 1e3}, {"density", "g/cm3", 1e3},
	{"force", "N", 1}, {"force", "kN", 1e3}, {"force", "mN", 1e-3},
	{"torque", "N m", 1}, {"torque", "N mm", 1e-3},
	{"pressure", "Pa", 1}, {"pressure", "kPa", 1e3}, {"pressure", "MPa", 1e6}, {"pressure", "bar", 1e5},
	{"temperature", "K", 1},
	{"angular velocity", "rad/s", 1},
	{"dimensionless", "1", 1}};
}

bool UnitFactor(const std::string& from, const std::string& to, double& factor)
{
	if (from == to) { factor = 1; return true; }
	const UnitRow *a = nullptr, *b = nullptr;
	for (const auto& r : Units) { if (from == r.unit) a = &r; if (to == r.unit) b = &r; }
	if (!a || !b || std::string(a->quantity) != b->quantity) return false;
	factor = a->scale/b->scale;
	return true;
}

std::string PortAdapterIdentity(const std::string& id, const std::string& version, const V& parameters)
{
	Bytes encoded;
	if (!RISECBOR64::Encode(V::ArrayValue({V::String("rise.simulation.adapter.v1"), V::String(id), V::String(version), parameters}),
		encoded, nullptr)) return std::string();
	return RISECBOR64::SHA256Hex(encoded);
}

namespace {

// ------------------------------------------------------------------ unit_scale

bool ScaleableCodec(const std::string& c) { return c == "field.v1" || c == "transform.v1" || c == "grid.v1"; }

class UnitScaleAdapter final : public IPortAdapter {
public:
	bool Handles(const PortDeclaration& offer, const PortDeclaration& accept) const override {
		if (!offer.offers || accept.offers || offer.codec != accept.codec || !ScaleableCodec(offer.codec)) return false;
		if (offer.quantity != accept.quantity || offer.channels.size() != accept.channels.size()) return false;
		if (!offer.domain.empty() && !accept.domain.empty() && offer.domain != accept.domain) return false;
		bool differs = offer.units != accept.units;
		double f = 1;
		if (!UnitFactor(offer.units, accept.units, f)) return false;
		for (std::size_t i = 0; i < offer.channels.size(); ++i) {
			const auto& a = offer.channels[i];
			const auto& b = accept.channels[i];
			if (a.name != b.name || a.quantity != b.quantity || a.components != b.components || !UnitFactor(a.units, b.units, f))
				return false;
			differs = differs || a.units != b.units;
		}
		return differs;
	}
	bool Convert(const PortDeclaration& offer, const PortDeclaration& accept, const Bytes& offered,
		const StaticPayloadResolver&, std::shared_ptr<const Bytes>& accepted, std::string& error) const override {
		auto out = std::make_shared<Bytes>();
		auto target = [&](const std::string& channel, const std::string& units, std::string& to, double& f) {
			for (const auto& c : accept.channels) if (c.name == channel) { to = c.units; return UnitFactor(units, to, f); }
			return false;
		};
		if (offer.codec == "field.v1") {
			FieldFrame f;
			if (!DecodeField(offered, CodecBudget, f, error)) return false;
			for (auto& c : f.channels) {
				std::string to;
				double k = 1;
				if (!target(c.name, c.units, to, k)) return Fail(error, "unit_scale: no conversion for channel " + c.name + " [" + c.units + "]");
				if (k != 1) for (auto& w : c.values) w = F32(FromF32(w)*k);
				c.units = to;
			}
			if (!EncodeField(f, CodecBudget, *out, error)) return false;
		} else if (offer.codec == "transform.v1") {
			TransformFrame f;
			if (!DecodeTransforms(offered, CodecBudget, f, error)) return false;
			double k = 1;
			if (!UnitFactor(f.lengthUnits, accept.units, k)) return Fail(error, "unit_scale: no conversion from " + f.lengthUnits);
			for (auto& b : f.bodies) {
				for (double& x : b.position) x *= k;
				for (double& x : b.velocity) x *= k;
			}
			f.lengthUnits = accept.units;
			if (!EncodeTransforms(f, CodecBudget, *out, error)) return false;
		} else {
			GridFrame g;
			if (!DecodeGrid(offered, CodecBudget, g, error)) return false;
			for (auto& c : g.channels) {
				std::string to;
				double k = 1;
				if (!target(c.name, c.units, to, k)) return Fail(error, "unit_scale: no conversion for channel " + c.name + " [" + c.units + "]");
				if (k != 1) {
					for (auto& w : c.background) w = F32(FromF32(w)*k);
					for (auto& b : c.blocks) for (auto& w : b.values) w = F32(FromF32(w)*k);
				}
				c.units = to;
			}
			if (!EncodeGrid(g, CodecBudget, *out, error)) return false;
		}
		accepted = std::move(out);
		return true;
	}
	std::string Identity() const override { return PortAdapterIdentity(UnitScaleAdapterId, "1", V::MapValue({})); }
	std::string Fidelity() const override { return "predictive"; }
	std::string Scope() const override {
		return "a scalar unit factor per channel from a fixed table; exact up to binary32 rounding of the samples";
	}
};

// ------------------------------------------------------------------ field_relabel

class FieldRelabelAdapter final : public IPortAdapter {
public:
	FieldRelabelAdapter() = default;
	explicit FieldRelabelAdapter(std::vector<std::pair<std::string,std::string>> map, V effective)
		: map_(std::move(map)), effective_(std::move(effective)) {}
	bool Handles(const PortDeclaration& offer, const PortDeclaration& accept) const override {
		if (!offer.offers || accept.offers || offer.codec != "field.v1" || accept.codec != "field.v1") return false;
		if (offer.channels.size() != accept.channels.size() || offer.units != accept.units) return false;
		if (!offer.domain.empty() && !accept.domain.empty() && offer.domain != accept.domain) return false;
		bool differs = offer.quantity != accept.quantity;
		for (std::size_t i = 0; i < offer.channels.size(); ++i) {
			const auto& a = offer.channels[i];
			const auto& b = accept.channels[i];
			const std::string target = Target(a.name, offer, accept);
			const PortChannel* t = nullptr;
			for (const auto& c : accept.channels) if (c.name == target) t = &c;
			if (!t || t->units != a.units || t->components != a.components) return false;
			differs = differs || a.name != b.name || a.quantity != b.quantity;
		}
		return differs;
	}
	bool Convert(const PortDeclaration& offer, const PortDeclaration& accept, const Bytes& offered,
		const StaticPayloadResolver&, std::shared_ptr<const Bytes>& accepted, std::string& error) const override {
		FieldFrame f;
		if (!DecodeField(offered, CodecBudget, f, error)) return false;
		if (!accept.domain.empty() && f.domain != accept.domain) return Fail(error, "field_relabel: the payload's domain is not the accepted one");
		for (auto& c : f.channels) {
			c.name = Target(c.name, offer, accept);
			bool known = false;
			for (const auto& a : accept.channels) known = known || (a.name == c.name && a.units == c.units && a.components == c.components);
			if (!known) return Fail(error, "field_relabel: channel " + c.name + " is not accepted");
		}
		std::sort(f.channels.begin(), f.channels.end(), [](const FieldChannel& a, const FieldChannel& b) { return a.name < b.name; });
		auto out = std::make_shared<Bytes>();
		if (!EncodeField(f, CodecBudget, *out, error)) return false;
		accepted = std::move(out);
		return true;
	}
	std::string Identity() const override { return PortAdapterIdentity(FieldRelabelAdapterId, "1", effective_); }
	std::string Fidelity() const override { return "predictive"; }
	std::string Scope() const override { return "channels renamed on the identical index space; values copied bit for bit"; }
private:
	// Explicit map, else positional (the i-th offered channel becomes the i-th accepted one).
	std::string Target(const std::string& name, const PortDeclaration& offer, const PortDeclaration& accept) const {
		for (const auto& m : map_) if (m.first == name) return m.second;
		if (!map_.empty()) return name;
		for (std::size_t i = 0; i < offer.channels.size() && i < accept.channels.size(); ++i)
			if (offer.channels[i].name == name) return accept.channels[i].name;
		return name;
	}
	std::vector<std::pair<std::string,std::string>> map_;
	V effective_ = V::MapValue({});
};

// ------------------------------------------------------------------ rigid_surface

class RigidSurfaceAdapter final : public IPortAdapter {
public:
	RigidSurfaceAdapter(std::string body, std::vector<double> vertices, std::vector<std::uint32_t> triangles, V effective)
		: body_(std::move(body)), vertices_(std::move(vertices)), triangles_(std::move(triangles)), effective_(std::move(effective)) {
		topology_ = MeshTopologyDigest(static_cast<std::uint32_t>(vertices_.size()/3), triangles_);
	}
	bool Handles(const PortDeclaration& offer, const PortDeclaration& accept) const override {
		return offer.offers && !accept.offers && offer.codec == TransformCodec && (accept.codec == "mesh.v2" || accept.codec == "mesh.v1") &&
			(accept.domain.empty() || accept.domain == topology_);
	}
	bool Convert(const PortDeclaration&, const PortDeclaration& accept, const Bytes& offered, const StaticPayloadResolver&,
		std::shared_ptr<const Bytes>& accepted, std::string& error) const override {
		TransformFrame f;
		if (!DecodeTransforms(offered, CodecBudget, f, error)) return false;
		const TransformBody* b = nullptr;
		for (const auto& x : f.bodies) if (x.name == body_) b = &x;
		if (!b) return Fail(error, "rigid_surface: no body named " + body_ + " in the offered transforms");
		double k = 1;
		if (!UnitFactor(f.lengthUnits, "m", k)) return Fail(error, "rigid_surface: transforms in unknown units " + f.lengthUnits);
		const auto R = QuaternionMatrix(b->orientation);
		const std::size_t n = vertices_.size()/3;
		MeshFrame m;
		m.positionUnits = "m";
		m.velocityUnits = "m/s";
		std::vector<double> world(3*n);
		for (std::size_t i = 0; i < n; ++i) {
			const double* p = &vertices_[3*i];
			double r[3];
			for (int a = 0; a < 3; ++a) r[a] = R[3*a]*p[0] + R[3*a + 1]*p[1] + R[3*a + 2]*p[2];
			const auto& w = b->angularVelocity;
			const double wr[3] = {w[1]*r[2] - w[2]*r[1], w[2]*r[0] - w[0]*r[2], w[0]*r[1] - w[1]*r[0]};
			for (int a = 0; a < 3; ++a) {
				world[3*i + a] = (b->position[a] + r[a])*k;
				m.positions.push_back(F32(world[3*i + a]));
			}
			for (int a = 0; a < 3; ++a) m.velocities.push_back(F32((b->velocity[a] + wr[a])*k));
		}
		// Area-weighted unit vertex normals.
		std::vector<double> nrm(3*n, 0.0);
		for (std::size_t t = 0; t + 2 < triangles_.size(); t += 3) {
			const double* a = &world[3*triangles_[t]];
			const double* c1 = &world[3*triangles_[t + 1]];
			const double* c2 = &world[3*triangles_[t + 2]];
			const double e1[3] = {c1[0] - a[0], c1[1] - a[1], c1[2] - a[2]}, e2[3] = {c2[0] - a[0], c2[1] - a[1], c2[2] - a[2]};
			const double cr[3] = {e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
			for (int v = 0; v < 3; ++v) for (int a = 0; a < 3; ++a) nrm[3*triangles_[t + v] + a] += cr[a];
		}
		for (std::size_t i = 0; i < n; ++i) {
			const double l = std::sqrt(nrm[3*i]*nrm[3*i] + nrm[3*i + 1]*nrm[3*i + 1] + nrm[3*i + 2]*nrm[3*i + 2]);
			for (int a = 0; a < 3; ++a) m.normals.push_back(F32(l > 0 ? nrm[3*i + a]/l : 0.0));
		}
		m.indices = triangles_;
		auto out = std::make_shared<Bytes>();
		if (accept.codec == "mesh.v1") {
			if (!EncodeMesh(m, CodecBudget, *out, error)) return false;
		} else {
			Bytes fixed;
			std::string digest;
			if (!EncodeMeshV2(m, MeshStaticLayout(), CodecBudget, *out, fixed, digest, error)) return false;
		}
		accepted = std::move(out);
		return true;
	}
	std::string Identity() const override { return PortAdapterIdentity(RigidSurfaceAdapterId, "1", effective_); }
	std::string Fidelity() const override { return "predictive"; }
	std::string Scope() const override {
		return "the body's surface placed rigidly by its transform, with rigid-motion vertex velocities; exact up to "
			"binary32 rounding of the positions";
	}
private:
	std::string body_;
	std::vector<double> vertices_;
	std::vector<std::uint32_t> triangles_;
	V effective_;
	std::string topology_;
};

// ------------------------------------------------------------------ grid sampling

// Trilinear sampling of one grid channel at world points (sample centres at
// integer indices; outside every block: the background).
class GridSampler {
public:
	bool Bind(const GridChannel& c, std::string& error) {
		c_ = &c;
		const auto& m = c.indexToWorld;
		const double a[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
		const double det = a[0]*(a[4]*a[8] - a[5]*a[7]) - a[1]*(a[3]*a[8] - a[5]*a[6]) + a[2]*(a[3]*a[7] - a[4]*a[6]);
		if (!(std::fabs(det) > 0)) return Fail(error, "grid index transform is singular");
		inv_[0] = (a[4]*a[8] - a[5]*a[7])/det; inv_[1] = (a[2]*a[7] - a[1]*a[8])/det; inv_[2] = (a[1]*a[5] - a[2]*a[4])/det;
		inv_[3] = (a[5]*a[6] - a[3]*a[8])/det; inv_[4] = (a[0]*a[8] - a[2]*a[6])/det; inv_[5] = (a[2]*a[3] - a[0]*a[5])/det;
		inv_[6] = (a[3]*a[7] - a[4]*a[6])/det; inv_[7] = (a[1]*a[6] - a[0]*a[7])/det; inv_[8] = (a[0]*a[4] - a[1]*a[3])/det;
		t_[0] = m[3]; t_[1] = m[7]; t_[2] = m[11];
		// A block index when the blocks tile a regular lattice (the usual case); else a linear scan.
		tiled_ = !c.blocks.empty();
		if (tiled_) tile_ = c.blocks.front().dimensions;
		for (std::size_t i = 0; tiled_ && i < c.blocks.size(); ++i) {
			const auto& b = c.blocks[i];
			if (b.dimensions != tile_) { tiled_ = false; break; }
			for (int k = 0; k < 3; ++k) if (Mod(b.origin[k], tile_[k]) != 0) tiled_ = false;
			index_[Key(b.origin[0]/static_cast<std::int64_t>(tile_[0]), b.origin[1]/static_cast<std::int64_t>(tile_[1]),
				b.origin[2]/static_cast<std::int64_t>(tile_[2]))] = i;
		}
		return true;
	}
	void Sample(const double x[3], double* out) const {
		const unsigned n = c_->components;
		double p[3];
		for (int r = 0; r < 3; ++r) {
			const double d[3] = {x[0] - t_[0], x[1] - t_[1], x[2] - t_[2]};
			p[r] = inv_[3*r]*d[0] + inv_[3*r + 1]*d[1] + inv_[3*r + 2]*d[2];
		}
		std::int64_t i0[3];
		double f[3];
		for (int r = 0; r < 3; ++r) { const double fl = std::floor(p[r]); i0[r] = static_cast<std::int64_t>(fl); f[r] = p[r] - fl; }
		for (unsigned k = 0; k < n; ++k) out[k] = 0;
		double v[16];
		for (int corner = 0; corner < 8; ++corner) {
			const int dx = corner & 1, dy = (corner >> 1) & 1, dz = (corner >> 2) & 1;
			const double w = (dx ? f[0] : 1 - f[0])*(dy ? f[1] : 1 - f[1])*(dz ? f[2] : 1 - f[2]);
			if (w == 0) continue;
			Value(i0[0] + dx, i0[1] + dy, i0[2] + dz, v);
			for (unsigned k = 0; k < n; ++k) out[k] += w*v[k];
		}
	}
private:
	static std::int64_t Mod(std::int64_t a, std::uint32_t b) { const std::int64_t m = a % static_cast<std::int64_t>(b); return m < 0 ? m + b : m; }
	static std::int64_t FloorDiv(std::int64_t a, std::uint32_t b) { return (a - Mod(a, b))/static_cast<std::int64_t>(b); }
	static std::uint64_t Key(std::int64_t a, std::int64_t b, std::int64_t c) {
		return (static_cast<std::uint64_t>(a + (1 << 20)) << 42) ^ (static_cast<std::uint64_t>(b + (1 << 20)) << 21) ^
			static_cast<std::uint64_t>(c + (1 << 20));
	}
	void Value(std::int64_t i, std::int64_t j, std::int64_t k, double* v) const {
		const unsigned n = c_->components;
		const GridBlock* hit = nullptr;
		if (tiled_) {
			const auto it = index_.find(Key(FloorDiv(i, tile_[0]), FloorDiv(j, tile_[1]), FloorDiv(k, tile_[2])));
			if (it != index_.end()) hit = &c_->blocks[it->second];
		} else {
			for (const auto& b : c_->blocks)
				if (i >= b.origin[0] && j >= b.origin[1] && k >= b.origin[2] && i < b.origin[0] + b.dimensions[0] &&
					j < b.origin[1] + b.dimensions[1] && k < b.origin[2] + b.dimensions[2]) { hit = &b; break; }
		}
		if (!hit) { for (unsigned q = 0; q < n; ++q) v[q] = FromF32(c_->background[q < 3 ? q : 0]); return; }
		if (hit->constant) { for (unsigned q = 0; q < n; ++q) v[q] = FromF32(hit->values[q]); return; }
		const std::size_t cell = static_cast<std::size_t>((i - hit->origin[0]) + hit->dimensions[0]*((j - hit->origin[1]) +
			static_cast<std::int64_t>(hit->dimensions[1])*(k - hit->origin[2])));
		for (unsigned q = 0; q < n; ++q) v[q] = FromF32(hit->values[n*cell + q]);
	}
	const GridChannel* c_ = nullptr;
	double inv_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, t_[3] = {0, 0, 0};
	bool tiled_ = false;
	std::array<std::uint32_t,3> tile_{};
	std::map<std::uint64_t, std::size_t> index_;
};

// ------------------------------------------------------------------ aerodynamic_drag

struct DragSettings {
	double normal = 1.2, tangential = 0.02, density = 1.2, pressure = 101325, molarMass = 0.028964, scale = 1;
	std::string velocity = "velocity", densityChannel, temperature;
};

class AerodynamicDragAdapter final : public IPortAdapter {
public:
	AerodynamicDragAdapter(DragSettings s, V effective) : s_(std::move(s)), effective_(std::move(effective)) {}
	bool Handles(const PortDeclaration& offer, const PortDeclaration& accept) const override {
		return offer.offers && !accept.offers && offer.codec == "grid.v1" && accept.codec == "field.v1";
	}
	bool Convert(const PortDeclaration& offer, const PortDeclaration& accept, const Bytes& offered, const StaticPayloadResolver& resolve,
		std::shared_ptr<const Bytes>& accepted, std::string& error) const override {
		return ConvertWithAuxiliary(offer, accept, offered, {}, resolve, accepted, error);
	}
	bool ConvertWithAuxiliary(const PortDeclaration&, const PortDeclaration& accept, const Bytes& offered,
		const std::vector<std::shared_ptr<const Bytes>>& auxiliary, const StaticPayloadResolver& resolve,
		std::shared_ptr<const Bytes>& accepted, std::string& error) const override {
		if (auxiliary.empty() || !auxiliary[0])
			return Fail(error, "aerodynamic_drag needs the loaded surface (its surface_state) as the connection's auxiliary port");
		MeshFrame mesh;
		if (!DecodeMesh(*auxiliary[0], CodecBudget, resolve, mesh, error)) return Fail(error, "aerodynamic_drag: surface: " + error);
		if (mesh.velocities.empty() || mesh.normals.empty()) return Fail(error, "aerodynamic_drag: the surface needs velocities and normals");
		GridFrame g;
		if (!DecodeGrid(offered, CodecBudget, g, error)) return false;
		const GridChannel *vel = nullptr, *rho = nullptr, *temp = nullptr;
		for (const auto& c : g.channels) {
			if (c.name == s_.velocity) vel = &c;
			if (!s_.densityChannel.empty() && c.name == s_.densityChannel) rho = &c;
			if (!s_.temperature.empty() && c.name == s_.temperature) temp = &c;
		}
		if (!vel || vel->components != 3) return Fail(error, "aerodynamic_drag: no 3-component velocity channel " + s_.velocity);
		double kv = 1, kr = 1;
		if (!UnitFactor(vel->units, "m/s", kv)) return Fail(error, "aerodynamic_drag: velocity in unknown units " + vel->units);
		if (!s_.densityChannel.empty() && (!rho || rho->components != 1 || !UnitFactor(rho->units, "kg/m^3", kr)))
			return Fail(error, "aerodynamic_drag: no scalar density channel " + s_.densityChannel + " in kg/m^3 (or a listed unit)");
		if (!s_.temperature.empty() && (!temp || temp->components != 1 || temp->units != "K"))
			return Fail(error, "aerodynamic_drag: no scalar temperature channel " + s_.temperature + " in K");
		GridSampler sv, sr, st;
		if (!sv.Bind(*vel, error) || (rho && !sr.Bind(*rho, error)) || (temp && !st.Bind(*temp, error))) return false;
		double kx = 1;
		if (!UnitFactor(mesh.positionUnits, "m", kx)) return Fail(error, "aerodynamic_drag: surface positions in unknown units");
		const std::size_t n = mesh.positions.size()/3;
		const double R = 8.314462618;
		FieldChannel traction{"traction", "Pa", 3, {}};
		traction.values.reserve(3*n);
		for (std::size_t i = 0; i < n; ++i) {
			double x[3], u[3], vc[3], nm[3];
			for (int a = 0; a < 3; ++a) {
				x[a] = FromF32(mesh.positions[3*i + a])*kx;
				vc[a] = FromF32(mesh.velocities[3*i + a])*kx;
				nm[a] = FromF32(mesh.normals[3*i + a]);
			}
			sv.Sample(x, u);
			double density = s_.density;
			if (rho) { sr.Sample(x, &density); density *= kr; }
			else if (temp) {
				double T = 0;
				st.Sample(x, &T);
				density = T > 1 ? s_.pressure*s_.molarMass/(R*T) : s_.density;
			}
			if (!(density >= 0) || !std::isfinite(density)) density = 0;
			double rel[3];
			for (int a = 0; a < 3; ++a) rel[a] = u[a]*kv - vc[a];
			const double un = rel[0]*nm[0] + rel[1]*nm[1] + rel[2]*nm[2];
			double ut[3];
			for (int a = 0; a < 3; ++a) ut[a] = rel[a] - un*nm[a];
			const double utn = std::sqrt(ut[0]*ut[0] + ut[1]*ut[1] + ut[2]*ut[2]);
			const double q = 0.5*density*s_.scale;
			for (int a = 0; a < 3; ++a) {
				const double t = q*(s_.normal*std::fabs(un)*un*nm[a] + s_.tangential*utn*ut[a]);
				traction.values.push_back(F32(std::isfinite(t) ? t : 0.0));
			}
		}
		FieldFrame out;
		out.domain = MeshTopologyDigest(static_cast<std::uint32_t>(n), mesh.indices);
		if (!accept.domain.empty() && out.domain != accept.domain) return Fail(error, "aerodynamic_drag: the surface is not the accepting sheet");
		out.count = static_cast<std::uint32_t>(n);
		out.channels.push_back(std::move(traction));
		auto bytes = std::make_shared<Bytes>();
		if (!EncodeField(out, CodecBudget, *bytes, error)) return false;
		accepted = std::move(bytes);
		return true;
	}
	std::string Identity() const override { return PortAdapterIdentity(AerodynamicDragAdapterId, "1", effective_); }
	std::string Fidelity() const override { return "preview"; }
	std::string Scope() const override {
		return "a quadratic drag MODEL (normal and tangential coefficients stated, gas density from a channel, the ideal gas "
			"law or a constant), sampled trilinearly at the surface vertices; unvalidated";
	}
private:
	DragSettings s_;
	V effective_;
};

// ------------------------------------------------------------------ contact_reaction

class ContactReactionAdapter final : public IPortAdapter {
public:
	ContactReactionAdapter(std::vector<std::string> targets, std::vector<std::string> bodies, V effective)
		: targets_(std::move(targets)), bodies_(std::move(bodies)), effective_(std::move(effective)) {
		domain_ = TransformBodyDomain(bodies_);
	}
	bool Handles(const PortDeclaration& offer, const PortDeclaration& accept) const override {
		return offer.offers && !accept.offers && offer.codec == "field.v1" && accept.codec == "field.v1" &&
			(accept.domain.empty() || accept.domain == domain_);
	}
	bool Convert(const PortDeclaration&, const PortDeclaration&, const Bytes& offered, const StaticPayloadResolver&,
		std::shared_ptr<const Bytes>& accepted, std::string& error) const override {
		FieldFrame f;
		if (!DecodeField(offered, CodecBudget, f, error)) return false;
		if (f.count != targets_.size()) return Fail(error, "contact_reaction: the reaction has " + std::to_string(f.count) +
			" colliders, the adapter maps " + std::to_string(targets_.size()));
		const FieldChannel *force = nullptr, *point = nullptr, *torque = nullptr;
		for (const auto& c : f.channels) {
			if (c.components != 3) return Fail(error, "contact_reaction: channels have 3 components");
			if (c.name == "force" && c.units == "N") force = &c;
			else if (c.name == "point" && c.units == "m") point = &c;
			else if (c.name == "torque" && c.units == "N m") torque = &c;
			else return Fail(error, "contact_reaction: unexpected channel " + c.name + " [" + c.units + "]");
		}
		if (!force) return Fail(error, "contact_reaction: no force channel");
		const std::size_t nb = bodies_.size();
		std::vector<double> F(3*nb, 0.0), P(3*nb, 0.0), T(3*nb, 0.0);
		std::vector<bool> seen(nb, false);
		for (std::size_t k = 0; k < targets_.size(); ++k) {
			const std::size_t j = static_cast<std::size_t>(std::find(bodies_.begin(), bodies_.end(), targets_[k]) - bodies_.begin());
			double fk[3], pk[3] = {0, 0, 0}, tk[3] = {0, 0, 0};
			for (int a = 0; a < 3; ++a) {
				fk[a] = FromF32(force->values[3*k + a]);
				if (point) pk[a] = FromF32(point->values[3*k + a]);
				if (torque) tk[a] = FromF32(torque->values[3*k + a]);
			}
			if (!seen[j]) { for (int a = 0; a < 3; ++a) P[3*j + a] = pk[a]; seen[j] = true; }
			// Moved to the body's first point: tau + (p_k - p_j) x F_k.
			const double d[3] = {pk[0] - P[3*j], pk[1] - P[3*j + 1], pk[2] - P[3*j + 2]};
			const double c[3] = {d[1]*fk[2] - d[2]*fk[1], d[2]*fk[0] - d[0]*fk[2], d[0]*fk[1] - d[1]*fk[0]};
			for (int a = 0; a < 3; ++a) { F[3*j + a] += fk[a]; T[3*j + a] += tk[a] + c[a]; }
		}
		FieldFrame out;
		out.domain = domain_;
		out.count = static_cast<std::uint32_t>(nb);
		FieldChannel cf{"force", "N", 3, {}}, cp{"point", "m", 3, {}}, ct{"torque", "N m", 3, {}};
		for (std::size_t i = 0; i < 3*nb; ++i) { cf.values.push_back(F32(F[i])); cp.values.push_back(F32(P[i])); ct.values.push_back(F32(T[i])); }
		out.channels = {cf, cp, ct};
		auto bytes = std::make_shared<Bytes>();
		if (!EncodeField(out, CodecBudget, *bytes, error)) return false;
		accepted = std::move(bytes);
		return true;
	}
	std::string Identity() const override { return PortAdapterIdentity(ContactReactionAdapterId, "1", effective_); }
	std::string Fidelity() const override { return "predictive"; }
	std::string Scope() const override {
		return "per-collider contact reactions summed onto their bodies, torques moved to a common point; exact up to "
			"binary32 rounding";
	}
private:
	std::vector<std::string> targets_, bodies_;
	V effective_;
	std::string domain_;
};

bool TextList(const V* v, std::vector<std::string>& out)
{
	out.clear();
	if (!v || v->GetType() != V::Array) return false;
	for (const V& e : v->GetArray()) {
		if (e.GetType() != V::Text || e.GetText().empty()) return false;
		out.push_back(e.GetText());
	}
	return true;
}

V TextArray(const std::vector<std::string>& list)
{
	V::Values out;
	for (const auto& s : list) out.push_back(V::String(s));
	return V::ArrayValue(out);
}

bool RecordBodyNames(const CacheDescription* d, std::vector<std::string>& names)
{
	names.clear();
	if (!d || d->physicalRecords.empty()) return false;
	V v;
	std::string error;
	if (!RISECBOR64::DecodeCanonical(d->physicalRecords, v, &error) || v.GetType() != V::Map) return false;
	const V* list = v.Find("bodies");
	if (!list || list->GetType() != V::Array) return false;
	for (const V& b : list->GetArray()) {
		const V* n = b.Find("name");
		if (!n || n->GetType() != V::Text) return false;
		names.push_back(n->GetText());
	}
	return true;
}

std::shared_ptr<const IPortAdapter> MakeUnitScale(const V& p, const PortAdapterContext&, std::string& error)
{
	Params r(p, "unit_scale", error);
	if (!r.Finish()) return nullptr;
	return std::make_shared<UnitScaleAdapter>();
}

std::shared_ptr<const IPortAdapter> MakeRelabel(const V& p, const PortAdapterContext&, std::string& error)
{
	Params r(p, "field_relabel", error);
	std::vector<std::pair<std::string,std::string>> map;
	if (const V* m = r.Raw("channels")) {
		if (m->GetType() != V::Array) { r.Bad("channels must be an array of [offered, accepted] name pairs"); return nullptr; }
		V::Values out;
		for (const V& e : m->GetArray()) {
			std::vector<std::string> pair;
			if (!TextList(&e, pair) || pair.size() != 2) { r.Bad("channels must be an array of [offered, accepted] name pairs"); return nullptr; }
			map.emplace_back(pair[0], pair[1]);
			out.push_back(TextArray(pair));
		}
		r.out.emplace_back("channels", V::ArrayValue(out));
	}
	if (!r.Finish()) return nullptr;
	return std::make_shared<FieldRelabelAdapter>(std::move(map), r.Effective());
}

std::shared_ptr<const IPortAdapter> MakeRigidSurface(const V& p, const PortAdapterContext& context, std::string& error)
{
	Params r(p, "rigid_surface", error);
	const std::string body = r.Text("body", "");
	std::vector<double> vertices;
	std::vector<std::uint32_t> triangles;
	const V* vs = r.Raw("vertices");
	const V* ts = r.Raw("triangles");
	if (!r.Finish()) return nullptr;
	if (body.empty()) { error = "rigid_surface needs the body's name"; return nullptr; }
	if (vs || ts) {
		if (!vs || !ts || vs->GetType() != V::Array || ts->GetType() != V::Array) { error = "rigid_surface: vertices and triangles go together"; return nullptr; }
		for (const V& e : vs->GetArray()) { double x = 0; if (!Number(e, x)) { error = "rigid_surface: vertices must be finite numbers"; return nullptr; } vertices.push_back(x); }
		for (const V& e : ts->GetArray()) {
			if (e.GetType() != V::UnsignedInteger || e.GetIntegerArgument() >= vertices.size()/3) { error = "rigid_surface: triangle index out of range"; return nullptr; }
			triangles.push_back(static_cast<std::uint32_t>(e.GetIntegerArgument()));
		}
		if (vertices.empty() || vertices.size() % 3 || triangles.empty() || triangles.size() % 3) { error = "rigid_surface: malformed surface"; return nullptr; }
	} else if (!context.offering || !RigidBodySurface(context.offering->physicalRecords, body, vertices, triangles, error)) {
		error = "rigid_surface: the body's surface is neither given nor in the offering member's record (" + error + ")";
		return nullptr;
	}
	// The identity carries the surface itself (a digest of it), whichever way it came.
	Bytes surface;
	V::Values vv, tv;
	for (double x : vertices) vv.push_back(V::Float(x));
	for (std::uint32_t k : triangles) tv.push_back(V::Unsigned(k));
	RISECBOR64::Encode(V::ArrayValue({V::ArrayValue(vv), V::ArrayValue(tv)}), surface, nullptr);
	const V effective = V::MapValue({{"body", V::String(body)}, {"surface", V::String(RISECBOR64::SHA256Hex(surface))}});
	auto adapter = std::make_shared<RigidSurfaceAdapter>(body, std::move(vertices), std::move(triangles), effective);
	if (context.offer && context.accept && !adapter->Handles(*context.offer, *context.accept)) {
		error = "rigid_surface: the accepted port is not a mesh on this body's surface (topology)";
		return nullptr;
	}
	return adapter;
}

std::shared_ptr<const IPortAdapter> MakeDrag(const V& p, const PortAdapterContext&, std::string& error)
{
	Params r(p, "aerodynamic_drag", error);
	DragSettings s;
	s.normal = r.Real("normal_coefficient", 1.2);
	s.tangential = r.Real("tangential_coefficient", 0.02);
	s.velocity = r.Text("velocity_channel", "velocity");
	s.densityChannel = r.Text("density_channel", "");
	s.temperature = r.Text("temperature_channel", "");
	s.density = r.Real("density", 1.2);
	s.pressure = r.Real("ambient_pressure", 101325);
	s.molarMass = r.Real("molar_mass", 0.028964);
	s.scale = r.Real("scale", 1);
	if (!r.Finish()) return nullptr;
	if (!(s.normal >= 0) || !(s.tangential >= 0) || !(s.density >= 0) || !(s.pressure > 0) || !(s.molarMass > 0) || !(s.scale >= 0)) {
		error = "aerodynamic_drag: coefficients, density and scale must not be negative; pressure and molar mass positive";
		return nullptr;
	}
	return std::make_shared<AerodynamicDragAdapter>(s, r.Effective());
}

std::shared_ptr<const IPortAdapter> MakeContactReaction(const V& p, const PortAdapterContext& context, std::string& error)
{
	Params r(p, "contact_reaction", error);
	std::vector<std::string> targets, bodies;
	const V* tv = r.Raw("targets");
	if (tv && tv->GetType() == V::Text && !tv->GetText().empty()) targets = {tv->GetText()};   // one collider
	else if (!TextList(tv, targets) || targets.empty()) { r.Bad("targets must list the body each collider belongs to"); return nullptr; }
	r.out.emplace_back("targets", TextArray(targets));
	if (const V* b = r.Raw("bodies")) {
		if (!TextList(b, bodies)) { r.Bad("bodies must be the sorted body names of the accepting set"); return nullptr; }
	} else if (!RecordBodyNames(context.accepting, bodies)) {
		r.Bad("bodies not given and the accepting member has no rigid.bodies record");
		return nullptr;
	}
	r.out.emplace_back("bodies", TextArray(bodies));
	if (!r.Finish()) return nullptr;
	if (!std::is_sorted(bodies.begin(), bodies.end()) || std::adjacent_find(bodies.begin(), bodies.end()) != bodies.end()) {
		error = "contact_reaction: bodies must be sorted and unique"; return nullptr;
	}
	for (const auto& t : targets)
		if (std::find(bodies.begin(), bodies.end(), t) == bodies.end()) { error = "contact_reaction: no body named " + t; return nullptr; }
	auto adapter = std::make_shared<ContactReactionAdapter>(targets, bodies, r.Effective());
	if (context.offer && context.accept && !adapter->Handles(*context.offer, *context.accept)) {
		error = "contact_reaction: the accepted port is not a field over this body set";
		return nullptr;
	}
	return adapter;
}
} // namespace

bool RegisterPortAdapterFactory(const std::string& id, PortAdapterFactory factory, std::string& error)
{
	if (id.empty() || !RISECBOR64::IsUTF8NFC(id) || !factory) return Fail(error, "invalid port adapter factory id or factory");
	auto& r = Factories();
	std::lock_guard<std::mutex> hold(r.mutex);
	for (const auto& f : r.factories) if (f.first == id) return Fail(error, "port adapter factory already registered: " + id);
	r.factories.emplace_back(id, std::move(factory));
	return true;
}

std::shared_ptr<const IPortAdapter> CreatePortAdapter(const std::string& id, const V& parameters,
	const PortAdapterContext& context, std::string& error)
{
	PortAdapterFactory factory;
	{
		auto& r = Factories();
		std::lock_guard<std::mutex> hold(r.mutex);
		for (const auto& f : r.factories) if (f.first == id) factory = f.second;
	}
	if (!factory) { error = "unknown port adapter: " + id; return nullptr; }
	auto adapter = factory(parameters, context, error);
	if (adapter && context.offer && context.accept && !adapter->Handles(*context.offer, *context.accept)) {
		error = "port adapter " + id + " does not convert " + context.offer->name + " [" + context.offer->codec + "] to " +
			context.accept->name + " [" + context.accept->codec + "]";
		return nullptr;
	}
	return adapter;
}

std::vector<std::string> RegisteredPortAdapterFactories()
{
	auto& r = Factories();
	std::lock_guard<std::mutex> hold(r.mutex);
	std::vector<std::string> ids;
	for (const auto& f : r.factories) ids.push_back(f.first);
	std::sort(ids.begin(), ids.end());
	return ids;
}

bool RegisterBuiltinPortAdapters(std::string& error)
{
	static std::mutex once;
	static bool done = false;
	std::lock_guard<std::mutex> hold(once);
	if (done) return true;
	if (!RegisterPortAdapterFactory(UnitScaleAdapterId, MakeUnitScale, error) ||
		!RegisterPortAdapterFactory(FieldRelabelAdapterId, MakeRelabel, error) ||
		!RegisterPortAdapterFactory(RigidSurfaceAdapterId, MakeRigidSurface, error) ||
		!RegisterPortAdapterFactory(AerodynamicDragAdapterId, MakeDrag, error) ||
		!RegisterPortAdapterFactory(ContactReactionAdapterId, MakeContactReaction, error)) return false;
	// The parameterless exact ones are also found automatically (FindPortAdapter).
	if (!RegisterPortAdapter(UnitScaleAdapterId, std::make_shared<UnitScaleAdapter>(), error) ||
		!RegisterPortAdapter(FieldRelabelAdapterId, std::make_shared<FieldRelabelAdapter>(), error)) return false;
	done = true;
	return true;
}

} }
