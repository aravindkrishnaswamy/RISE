//////////////////////////////////////////////////////////////////////
//
//  LiquidBodies.cpp - see LiquidBodies.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidBodies.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace RISE { namespace Simulation { namespace Liquid {

namespace {
constexpr double Pi = 3.14159265358979323846;
double Clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
double Norm3(const V3& a) { return std::sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]); }

// Body-frame coordinates of a world point.
V3 ToBody(const Rigid::M3& R, const V3& X, const V3& p)
{
	const V3 d{{p[0] - X[0], p[1] - X[1], p[2] - X[2]}};
	return Rigid::ApplyT(R, d);
}
} // namespace

// ------------------------------------------------------------------ shapes

const char* BodyShapeName(BodyShape::Type t)
{
	return t == BodyShape::Type::Sphere ? "sphere" : t == BodyShape::Type::Box ? "box" : t == BodyShape::Type::Cylinder ? "cylinder" : "mesh";
}

// ------------------------------------------------------------------ mesh bodies (m7d)

namespace {
V3 Sub3(const V3& a, const V3& b) { return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}}; }
double Dot3(const V3& a, const V3& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

// The point of triangle abc nearest p (Ericson, Real-Time Collision Detection, 5.1.5).
V3 ClosestPoint(const V3& p, const V3& a, const V3& b, const V3& c)
{
	const V3 ab = Sub3(b, a), ac = Sub3(c, a), ap = Sub3(p, a);
	const double d1 = Dot3(ab, ap), d2 = Dot3(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	const V3 bp = Sub3(p, b);
	const double d3 = Dot3(ab, bp), d4 = Dot3(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	const double vc = d1*d4 - d3*d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) { const double v = d1/(d1 - d3); return {{a[0] + v*ab[0], a[1] + v*ab[1], a[2] + v*ab[2]}}; }
	const V3 cp = Sub3(p, c);
	const double d5 = Dot3(ab, cp), d6 = Dot3(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	const double vb = d5*d2 - d1*d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) { const double w = d2/(d2 - d6); return {{a[0] + w*ac[0], a[1] + w*ac[1], a[2] + w*ac[2]}}; }
	const double va = d3*d6 - d5*d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		const double w = (d4 - d3)/((d4 - d3) + (d5 - d6));
		return {{b[0] + w*(c[0] - b[0]), b[1] + w*(c[1] - b[1]), b[2] + w*(c[2] - b[2])}};
	}
	const double denom = 1/(va + vb + vc), v = vb*denom, w = vc*denom;
	return {{a[0] + ab[0]*v + ac[0]*w, a[1] + ab[1]*v + ac[1]*w, a[2] + ab[2]*v + ac[2]*w}};
}

// A bounding-volume tree over triangles for nearest-point queries (median
// splits on the longest axis, ties by triangle index: deterministic).
class TriangleTree {
public:
	TriangleTree(const std::vector<double>& v, const std::vector<std::uint32_t>& t) : v_(v), t_(t) {
		std::vector<std::uint32_t> ids;
		for (std::uint32_t k = 0; 3*std::size_t(k) < t.size(); ++k) {
			const V3 a = P(t[3*k]), b = P(t[3*k + 1]), c = P(t[3*k + 2]);
			const V3 n{{(b[1] - a[1])*(c[2] - a[2]) - (b[2] - a[2])*(c[1] - a[1]), (b[2] - a[2])*(c[0] - a[0]) - (b[0] - a[0])*(c[2] - a[2]),
				(b[0] - a[0])*(c[1] - a[1]) - (b[1] - a[1])*(c[0] - a[0])}};
			if (Dot3(n, n) > 0) ids.push_back(k);   // a collapsed triangle has no nearest point of its own
		}
		if (!ids.empty()) { nodes_.emplace_back(); Build(0, ids, 0, ids.size()); }
		order_ = ids;
	}
	double Nearest(const V3& p) const {
		if (nodes_.empty()) return 1e300;
		double best = 1e300;
		std::vector<std::uint32_t> stack{0};
		while (!stack.empty()) {
			const Node& n = nodes_[stack.back()];
			stack.pop_back();
			if (BoxDistance2(n, p) >= best) continue;
			if (n.count) {
				for (std::uint32_t m = 0; m < n.count; ++m) {
					const std::uint32_t k = order_[n.first + m];
					const V3 q = ClosestPoint(p, P(t_[3*k]), P(t_[3*k + 1]), P(t_[3*k + 2]));
					const V3 d = Sub3(p, q);
					best = std::min(best, Dot3(d, d));
				}
				continue;
			}
			const double l = BoxDistance2(nodes_[n.first], p), r = BoxDistance2(nodes_[n.first + 1], p);
			if (l < r) { stack.push_back(n.first + 1); stack.push_back(n.first); }
			else { stack.push_back(n.first); stack.push_back(n.first + 1); }
		}
		return std::sqrt(best);
	}
private:
	struct Node { V3 lo, hi; std::uint32_t first = 0, count = 0; };   // leaf: triangles [first, first + count) of order_; else children first, first + 1
	V3 P(std::uint32_t k) const { return {{v_[3*std::size_t(k)], v_[3*std::size_t(k) + 1], v_[3*std::size_t(k) + 2]}}; }
	static double BoxDistance2(const Node& n, const V3& p) {
		double s = 0;
		for (int a = 0; a < 3; ++a) {
			const double e = p[a] < n.lo[a] ? n.lo[a] - p[a] : p[a] > n.hi[a] ? p[a] - n.hi[a] : 0.0;
			s += e*e;
		}
		return s;
	}
	void Build(std::uint32_t at, std::vector<std::uint32_t>& ids, std::size_t b, std::size_t e) {
		Node n;
		n.lo = {{1e300, 1e300, 1e300}};
		n.hi = {{-1e300, -1e300, -1e300}};
		for (std::size_t i = b; i < e; ++i)
			for (int m = 0; m < 3; ++m) {
				const V3 q = P(t_[3*std::size_t(ids[i]) + std::size_t(m)]);
				for (int a = 0; a < 3; ++a) { n.lo[a] = std::min(n.lo[a], q[a]); n.hi[a] = std::max(n.hi[a], q[a]); }
			}
		if (e - b <= 4) { n.first = std::uint32_t(b); n.count = std::uint32_t(e - b); nodes_[at] = n; return; }
		int axis = 0;
		for (int a = 1; a < 3; ++a) if (n.hi[a] - n.lo[a] > n.hi[axis] - n.lo[axis]) axis = a;
		auto centre = [&](std::uint32_t k) {
			return P(t_[3*std::size_t(k)])[axis] + P(t_[3*std::size_t(k) + 1])[axis] + P(t_[3*std::size_t(k) + 2])[axis];
		};
		std::sort(ids.begin() + std::ptrdiff_t(b), ids.begin() + std::ptrdiff_t(e), [&](std::uint32_t x, std::uint32_t y) {
			const double cx = centre(x), cy = centre(y);
			return cx != cy ? cx < cy : x < y;
		});
		const std::size_t mid = b + (e - b)/2;
		// The two children are allocated next to each other.
		n.first = std::uint32_t(nodes_.size());
		nodes_.emplace_back();
		nodes_.emplace_back();
		nodes_[at] = n;
		Build(n.first, ids, b, mid);
		Build(n.first + 1, ids, mid, e);
	}
	const std::vector<double>& v_;
	const std::vector<std::uint32_t>& t_;
	std::vector<Node> nodes_;
	std::vector<std::uint32_t> order_;
};
} // namespace

double MeshDistance::Sample(const V3& y) const
{
	double f[3], out = 0;
	int i[3];
	for (int a = 0; a < 3; ++a) {
		const double x = (y[a] - origin[a])/spacing;
		const double top = double(nodes[a] - 1);
		if (x < 0) out += x*x;
		else if (x > top) out += (x - top)*(x - top);
		const double c = Clamp(x, 0.0, top);
		i[a] = std::min(nodes[a] - 2, int(std::floor(c)));
		f[a] = c - i[a];
	}
	if (out > 0) return pad + spacing*std::sqrt(out);
	auto at = [&](int di, int dj, int dk) {
		return d[std::size_t(i[0] + di) + std::size_t(nodes[0])*(std::size_t(i[1] + dj) + std::size_t(nodes[1])*std::size_t(i[2] + dk))];
	};
	const double c00 = at(0, 0, 0) + f[0]*(at(1, 0, 0) - at(0, 0, 0)), c10 = at(0, 1, 0) + f[0]*(at(1, 1, 0) - at(0, 1, 0));
	const double c01 = at(0, 0, 1) + f[0]*(at(1, 0, 1) - at(0, 0, 1)), c11 = at(0, 1, 1) + f[0]*(at(1, 1, 1) - at(0, 1, 1));
	const double c0 = c00 + f[1]*(c10 - c00), c1 = c01 + f[1]*(c11 - c01);
	return c0 + f[2]*(c1 - c0);
}

bool MakeMeshShape(const std::vector<double>& vertices, const std::vector<std::uint32_t>& triangles, double spacing, double pad,
	BodyShape& shape, V3& centroid, Rigid::M3& axes, std::string& error)
{
	if (!(spacing > 0) || !(pad >= 0)) { error = "mesh body: invalid distance lattice"; return false; }
	Rigid::Shape rs;
	rs.type = Rigid::Shape::Type::Mesh;
	rs.vertices = vertices;
	rs.triangles = triangles;
	Rigid::MassProperties mp;
	if (!Rigid::ShapeMass(rs, 1.0, mp, error)) { error = "mesh body: " + error; return false; }
	V3 moments;
	Rigid::PrincipalAxes(mp.inertia, moments, axes);
	if (!(moments[0] > 0) || !(moments[1] > 0) || !(moments[2] > 0)) { error = "mesh body has a degenerate inertia"; return false; }
	centroid = mp.centroid;
	auto m = std::make_shared<MeshDistance>();
	m->volume = mp.volume;
	m->unitMoments = moments;
	m->triangles = triangles;
	m->vertices.resize(vertices.size());
	V3 lo{{1e300, 1e300, 1e300}}, hi{{-1e300, -1e300, -1e300}};
	for (std::size_t k = 0; k + 2 < vertices.size(); k += 3) {
		const V3 y = Rigid::ApplyT(axes, V3{{vertices[k] - centroid[0], vertices[k + 1] - centroid[1], vertices[k + 2] - centroid[2]}});
		for (int a = 0; a < 3; ++a) {
			m->vertices[k + std::size_t(a)] = y[a];
			lo[a] = std::min(lo[a], y[a]);
			hi[a] = std::max(hi[a], y[a]);
		}
		m->boundingRadius = std::max(m->boundingRadius, Norm3(y));
	}
	m->smallestExtent = std::min(hi[0] - lo[0], std::min(hi[1] - lo[1], hi[2] - lo[2]));
	m->spacing = spacing;
	m->pad = pad;
	std::size_t total = 1;
	for (int a = 0; a < 3; ++a) {
		m->origin[a] = lo[a] - pad;
		m->nodes[a] = std::max(2, int(std::ceil((hi[a] - lo[a] + 2*pad)/spacing)) + 1);
		total *= std::size_t(m->nodes[a]);
	}
	if (total > (std::size_t(1) << 28)) { error = "mesh body: the distance lattice is too large (more than 2^28 nodes)"; return false; }
	m->d.assign(total, 0.0);
	const TriangleTree tree(m->vertices, m->triangles);
	const auto& vb = m->vertices;
	auto vert = [&](std::uint32_t k) { return V3{{vb[3*std::size_t(k)], vb[3*std::size_t(k) + 1], vb[3*std::size_t(k) + 2]}}; };
	// The triangles each x row of nodes may cross (by their yz bounds), for the sign.
	const int ny = m->nodes[1], nz = m->nodes[2], nx = m->nodes[0];
	std::vector<std::vector<std::uint32_t>> rows(std::size_t(ny)*std::size_t(nz));
	for (std::uint32_t t = 0; 3*std::size_t(t) < triangles.size(); ++t) {
		double l[3] = {1e300, 1e300, 1e300}, u[3] = {-1e300, -1e300, -1e300};
		for (int c = 0; c < 3; ++c) {
			const V3 p = vert(triangles[3*std::size_t(t) + std::size_t(c)]);
			for (int a = 0; a < 3; ++a) { l[a] = std::min(l[a], p[a]); u[a] = std::max(u[a], p[a]); }
		}
		const int j0 = std::max(0, int(std::floor((l[1] - m->origin[1])/spacing)) - 1), j1 = std::min(ny - 1, int(std::ceil((u[1] - m->origin[1])/spacing)) + 1);
		const int k0 = std::max(0, int(std::floor((l[2] - m->origin[2])/spacing)) - 1), k1 = std::min(nz - 1, int(std::ceil((u[2] - m->origin[2])/spacing)) + 1);
		for (int k = k0; k <= k1; ++k) for (int j = j0; j <= j1; ++j) rows[std::size_t(j) + std::size_t(ny)*std::size_t(k)].push_back(t);
	}
	const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
	Pool pool(std::min(16u, hw));
	const double oy = 0.0001234567*spacing, oz = 0.0000987654*spacing;   // off the lattice so the line misses edges
	ForRows(&pool, rows.size(), 16, [&](std::size_t r0, std::size_t r1) {
		std::vector<double> xs;
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(ny)), k = int(r/std::size_t(ny));
			const double y = m->origin[1] + j*spacing + oy, z = m->origin[2] + k*spacing + oz;
			xs.clear();
			for (std::uint32_t t : rows[r]) {
				const V3 a = vert(triangles[3*std::size_t(t)]), b = vert(triangles[3*std::size_t(t) + 1]), c = vert(triangles[3*std::size_t(t) + 2]);
				const double det = (b[1] - a[1])*(c[2] - a[2]) - (c[1] - a[1])*(b[2] - a[2]);
				if (det == 0) continue;
				const double u = ((y - a[1])*(c[2] - a[2]) - (c[1] - a[1])*(z - a[2]))/det;
				const double v = ((b[1] - a[1])*(z - a[2]) - (y - a[1])*(b[2] - a[2]))/det;
				if (u < 0 || v < 0 || u + v > 1) continue;
				xs.push_back(a[0] + u*(b[0] - a[0]) + v*(c[0] - a[0]));
			}
			std::sort(xs.begin(), xs.end());
			std::size_t q = 0;
			for (int i = 0; i < nx; ++i) {
				const V3 p{{m->origin[0] + i*spacing, m->origin[1] + j*spacing, m->origin[2] + k*spacing}};
				while (q < xs.size() && xs[q] <= p[0]) ++q;
				const bool inside = ((xs.size() - q) & 1) != 0;
				const double dist = tree.Nearest(p);
				m->d[std::size_t(i) + std::size_t(nx)*r] = inside ? -dist : dist;
			}
		}
	});
	shape = BodyShape();
	shape.type = BodyShape::Type::Mesh;
	shape.mesh = std::move(m);
	return true;
}

double BodyShape::Distance(const V3& y) const
{
	switch (type) {
	case Type::Mesh: return mesh ? mesh->Sample(y) : 1e300;
	case Type::Sphere: return Norm3(y) - radius;
	case Type::Box: {
		double outside = 0, inside = -1e300;
		for (int a = 0; a < 3; ++a) {
			const double q = std::fabs(y[a]) - 0.5*size[a];
			outside += std::max(q, 0.0)*std::max(q, 0.0);
			inside = std::max(inside, q);
		}
		return std::sqrt(outside) + std::min(inside, 0.0);
	}
	case Type::Cylinder: {
		const double qr = std::sqrt(y[0]*y[0] + y[1]*y[1]) - radius, qa = std::fabs(y[2]) - 0.5*height;
		const double out = std::sqrt(std::max(qr, 0.0)*std::max(qr, 0.0) + std::max(qa, 0.0)*std::max(qa, 0.0));
		return out + std::min(std::max(qr, qa), 0.0);
	}
	}
	return 1e300;
}

double BodyShape::BoundingRadius() const
{
	switch (type) {
	case Type::Sphere: return radius;
	case Type::Box: return 0.5*Norm3(size);
	case Type::Cylinder: return std::sqrt(radius*radius + 0.25*height*height);
	case Type::Mesh: return mesh ? mesh->boundingRadius : 0;
	}
	return 0;
}

double BodyShape::Volume() const
{
	switch (type) {
	case Type::Sphere: return 4.0/3.0*Pi*radius*radius*radius;
	case Type::Box: return size[0]*size[1]*size[2];
	case Type::Cylinder: return Pi*radius*radius*height;
	case Type::Mesh: return mesh ? mesh->volume : 0;
	}
	return 0;
}

V3 BodyShape::Moments(double m) const
{
	switch (type) {
	case Type::Sphere: { const double I = 0.4*m*radius*radius; return {{I, I, I}}; }
	case Type::Box: {
		const double x = size[0]*size[0], y = size[1]*size[1], z = size[2]*size[2];
		return {{m*(y + z)/12, m*(x + z)/12, m*(x + y)/12}};
	}
	case Type::Cylinder: {
		const double r = m*(3*radius*radius + height*height)/12;
		return {{r, r, 0.5*m*radius*radius}};
	}
	case Type::Mesh: {
		if (!mesh || !(mesh->volume > 0)) return {{0, 0, 0}};
		const double rho = m/mesh->volume;
		return {{rho*mesh->unitMoments[0], rho*mesh->unitMoments[1], rho*mesh->unitMoments[2]}};
	}
	}
	return {{0, 0, 0}};
}

double BodyShape::SmallestExtent() const
{
	switch (type) {
	case Type::Sphere: return 2*radius;
	case Type::Box: return std::min(size[0], std::min(size[1], size[2]));
	case Type::Cylinder: return std::min(2*radius, height);
	case Type::Mesh: return mesh ? mesh->smallestExtent : 0;
	}
	return 0;
}

void LiquidBody::PrescribedAt(double t, V3& x, Quat& q, V3& v, V3& omega) const
{
	const double w = 2*Pi*frequency, s = std::sin(w*t + phase), c = std::cos(w*t + phase);
	for (int a = 0; a < 3; ++a) {
		x[a] = x0[a] + v0[a]*t + amplitude[a]*s;
		v[a] = v0[a] + amplitude[a]*w*c;
	}
	omega = spin;
	const double rate = Norm3(spin);
	if (rate > 0) {
		const double half = 0.5*rate*t, k = std::sin(half)/rate;
		const Quat d{{std::cos(half), spin[0]*k, spin[1]*k, spin[2]*k}};
		q = Rigid::QMul(d, q0);
		Rigid::QNormalize(q);
	} else q = q0;
}

double LiquidBody::SurfaceSpeed() const
{
	return Norm3(state.v) + Norm3(Omega())*shape.BoundingRadius();
}

void WorldInertia(const Rigid::Body& b, std::array<double,9>& inertia, std::array<double,9>& inverse)
{
	const Rigid::M3 R = Rigid::QMatrix(b.q);
	for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
		double s = 0, t = 0;
		for (int k = 0; k < 3; ++k) {
			s += R[3*r + k]*b.moments[k]*R[3*c + k];
			t += R[3*r + k]/b.moments[k]*R[3*c + k];
		}
		inertia[3*r + c] = s;
		inverse[3*r + c] = t;
	}
}

// ------------------------------------------------------------------ geometry

void BodyGeometry::Initialize(const Grid& g, const std::vector<double>& staticNodes,
	const std::array<std::vector<double>,3>& staticFractions, const std::vector<double>& staticOpen, bool openTop, double band)
{
	g_ = g;
	openTop_ = openTop;
	band_ = band;
	staticNodes_ = staticNodes;
	staticFractions_ = staticFractions;
	staticOpen_ = staticOpen;
	last_.clear();
	faces_.clear();
	enclosed_.clear();
	for (auto& t : touched_) t.clear();
}

BodyGeometry::Box BodyGeometry::NodeBox(const LiquidBody& b) const
{
	Box box;
	const double r = b.shape.BoundingRadius() + band_ + g_.h;
	box.empty = false;
	for (int a = 0; a < 3; ++a) {
		const double lo = (b.state.x[a] - r - g_.origin[a])/g_.h, hi = (b.state.x[a] + r - g_.origin[a])/g_.h;
		box.lo[a] = int(std::max(0.0, std::floor(lo)));
		box.hi[a] = int(std::min(double(g_.n[a]), std::ceil(hi)));
		if (!(lo <= double(g_.n[a]) && hi >= 0) || box.lo[a] > box.hi[a]) box.empty = true;
	}
	return box;
}

void BodyGeometry::Update(const std::vector<LiquidBody>& bodies, std::vector<double>& nodes,
	std::array<std::vector<double>,3>& fraction, std::vector<double>& open, Pool* pool)
{
	const Grid& g = g_;
	const std::size_t nb = bodies.size();
	std::vector<Box> now(nb), region(nb);
	std::vector<Rigid::M3> R(nb);
	for (std::size_t i = 0; i < nb; ++i) {
		now[i] = NodeBox(bodies[i]);
		R[i] = Rigid::QMatrix(bodies[i].state.q);
		region[i] = now[i];
		if (i < last_.size() && !last_[i].empty) {
			if (region[i].empty) region[i] = last_[i];
			else for (int a = 0; a < 3; ++a) {
				region[i].lo[a] = std::min(region[i].lo[a], last_[i].lo[a]);
				region[i].hi[a] = std::max(region[i].hi[a], last_[i].hi[a]);
			}
		}
	}
	auto inside = [](const Box& b, int i, int j, int k) {
		return !b.empty && i >= b.lo[0] && i <= b.hi[0] && j >= b.lo[1] && j <= b.hi[1] && k >= b.lo[2] && k <= b.hi[2];
	};
	// The distance of body i at a world point, clamped to the band.
	auto bodyDistance = [&](std::size_t i, const V3& p) {
		return Clamp(bodies[i].shape.Distance(ToBody(R[i], bodies[i].state.x, p)), -band_, band_);
	};
	// The open fraction of a cell outside the bodies: 1 or 0 away from their
	// surfaces; in a cell their surface may cut, the mean over 4^3 sub-cells of
	// clamp(1/2 + d / h_s) with d the bodies' exact distance at the sub-cell
	// centre (h_s = h / 4).  It is continuous in the bodies' poses: the
	// volume correction reads it at every step, and the coarse estimate from
	// the eight nodes (static solids) changed by jumps that pumped energy into
	// a bobbing body (docs/SIMULATION_SOLVER_CONTRACT.md section 3).
	auto bodyMin = [&](const V3& p, int ci, int cj, int ck) {
		double d = 1e300;
		for (std::size_t q = 0; q < nb; ++q) {
			if (!inside(now[q], ci, cj, ck) && !inside(now[q], ci + 1, cj + 1, ck + 1)) continue;
			d = std::min(d, bodies[q].shape.Distance(ToBody(R[q], bodies[q].state.x, p)));
		}
		return d;
	};
	auto BodyOpen = [&](int i, int j, int k) {
		const V3 c = g.CellCentre(i, j, k);
		const double d = bodyMin(c, i, j, k);
		if (d >= 0.8660254037844386*g.h + 1e-12*g.h) return 1.0;
		if (d <= -0.8660254037844386*g.h - 1e-12*g.h) return 0.0;
		const int S = 4;
		const double hs = g.h/S;
		double sum = 0;
		for (int c3 = 0; c3 < S; ++c3) for (int b3 = 0; b3 < S; ++b3) for (int a3 = 0; a3 < S; ++a3) {
			const V3 p{{c[0] + (a3 + 0.5 - 0.5*S)*hs, c[1] + (b3 + 0.5 - 0.5*S)*hs, c[2] + (c3 + 0.5 - 0.5*S)*hs}};
			sum += Clamp(0.5 + bodyMin(p, i, j, k)/hs, 0.0, 1.0);
		}
		return sum/double(S*S*S);
	};
	// The open part of a face's control volume (the cell of side h centred on the
	// face; across a one-cell axis the slab): 4^3 sub-cells as in BodyOpen.
	auto FaceVolumeOpen = [&](int a, int i, int j, int k) {
		const V3 c = g.FaceCentre(a, i, j, k);
		int ci = i, cj = j, ck = k;
		if (a == 0) ci = std::max(0, i - 1); else if (a == 1) cj = std::max(0, j - 1); else ck = std::max(0, k - 1);
		const double d = bodyMin(c, ci, cj, ck);
		if (d >= 0.8660254037844386*g.h + 1e-12*g.h) return 1.0;
		if (d <= -0.8660254037844386*g.h - 1e-12*g.h) return 0.0;
		const int S = 4;
		const double hs = g.h/S;
		double sum = 0;
		for (int c3 = 0; c3 < S; ++c3) for (int b3 = 0; b3 < S; ++b3) for (int a3 = 0; a3 < S; ++a3) {
			const V3 p{{c[0] + (a3 + 0.5 - 0.5*S)*hs, c[1] + (b3 + 0.5 - 0.5*S)*hs, c[2] + (c3 + 0.5 - 0.5*S)*hs}};
			sum += Clamp(0.5 + bodyMin(p, ci, cj, ck)/hs, 0.0, 1.0);
		}
		return sum/double(S*S*S);
	};
	// 1. Nodes in each region: the static value and every body whose box holds the node.
	for (std::size_t r = 0; r < nb; ++r) {
		const Box& b = region[r];
		if (b.empty) continue;
		const int nx = b.hi[0] - b.lo[0] + 1, ny = b.hi[1] - b.lo[1] + 1, nz = b.hi[2] - b.lo[2] + 1;
		ForRows(pool, std::size_t(ny)*std::size_t(nz), RowGrain(nx), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t row = r0; row < r1; ++row) {
				const int j = b.lo[1] + int(row % std::size_t(ny)), k = b.lo[2] + int(row/std::size_t(ny));
				for (int i = b.lo[0]; i <= b.hi[0]; ++i) {
					const std::size_t id = g.N(i, j, k);
					double v = staticNodes_[id];
					for (std::size_t q = 0; q < nb; ++q) if (inside(now[q], i, j, k)) v = std::min(v, bodyDistance(q, g.Node(i, j, k)));
					nodes[id] = v;
				}
			}
		});
	}
	// 2. Faces and cells with a corner in a region.
	for (std::size_t r = 0; r < nb; ++r) {
		const Box& b = region[r];
		if (b.empty) continue;
		for (int a = 0; a < 3; ++a) {
			const auto d = g.FaceDims(a);
			int lo[3], hi[3];
			for (int c = 0; c < 3; ++c) {
				if (c == a) { lo[c] = b.lo[c]; hi[c] = std::min(b.hi[c], d[c] - 1); }
				else { lo[c] = std::max(0, b.lo[c] - 1); hi[c] = std::min(d[c] - 1, b.hi[c]); }
			}
			if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) continue;
			const int ny = hi[1] - lo[1] + 1, nz = hi[2] - lo[2] + 1;
			ForRows(pool, std::size_t(ny)*std::size_t(nz), RowGrain(hi[0] - lo[0] + 1), [&](std::size_t r0, std::size_t r1) {
				for (std::size_t row = r0; row < r1; ++row) {
					const int j = lo[1] + int(row % std::size_t(ny)), k = lo[2] + int(row/std::size_t(ny));
					for (int i = lo[0]; i <= hi[0]; ++i) {
						const std::size_t f = g.F(a, i, j, k);
						// The open fraction of a face beside a body: the open part of the face's
						// control volume (Batty, Bertails and Bridson 2007), not of its area.  A
						// face parallel to a flat body side is either open or closed by area, so
						// a flat bottom moving across a face plane changed the buoyancy by jumps
						// of rho g h per unit area (a staircase in the body's position); the
						// control volume's fraction varies continuously.  Static fractions hold
						// any `min_face_fraction` closing.
						const bool boundary = (a == 0 ? i : a == 1 ? j : k) == 0 || (a == 0 ? i : a == 1 ? j : k) == g.n[a];
						const double open = boundary ? staticFractions_[a][f] : FaceVolumeOpen(a, i, j, k);
						fraction[a][f] = std::min(open, staticFractions_[a][f]);
					}
				}
			});
		}
		{
			const int lo[3] = {std::max(0, b.lo[0] - 1), std::max(0, b.lo[1] - 1), std::max(0, b.lo[2] - 1)};
			const int hi[3] = {std::min(g.n[0] - 1, b.hi[0]), std::min(g.n[1] - 1, b.hi[1]), std::min(g.n[2] - 1, b.hi[2])};
			if (lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2]) {
				const int ny = hi[1] - lo[1] + 1, nz = hi[2] - lo[2] + 1;
				ForRows(pool, std::size_t(ny)*std::size_t(nz), RowGrain(hi[0] - lo[0] + 1), [&](std::size_t r0, std::size_t r1) {
					for (std::size_t row = r0; row < r1; ++row) {
						const int j = lo[1] + int(row % std::size_t(ny)), k = lo[2] + int(row/std::size_t(ny));
						for (int i = lo[0]; i <= hi[0]; ++i) {
							const std::size_t c = g.C(i, j, k);
							open[c] = std::min(staticOpen_[c], BodyOpen(i, j, k));
						}
					}
				});
			}
		}
	}
	// 3. Body faces: closed faces inside a body's current box whose nearest solid at the face centre is that body.
	faces_.assign(nb, {});
	for (std::size_t i = 0; i < nb; ++i) {
		const Box& b = now[i];
		if (b.empty) continue;
		for (int a = 0; a < 3; ++a) {
			const auto d = g.FaceDims(a);
			int lo[3], hi[3];
			for (int c = 0; c < 3; ++c) {
				if (c == a) { lo[c] = std::max(1, b.lo[c]); hi[c] = std::min(b.hi[c], g.n[c] - 1); }   // interior faces only
				else { lo[c] = std::max(0, b.lo[c] - 1); hi[c] = std::min(d[c] - 1, b.hi[c]); }
			}
			if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) continue;
			const int ny = hi[1] - lo[1] + 1, nz = hi[2] - lo[2] + 1;
			const std::size_t rows = std::size_t(ny)*std::size_t(nz), grain = RowGrain(hi[0] - lo[0] + 1);
			const std::size_t chunks = Cloth::Chunks(rows, grain);
			std::vector<std::vector<BodyFace>> part(chunks);
			Cloth::ParallelFor(pool, chunks, [&](std::size_t ch) {
				for (std::size_t row = ch*grain; row < std::min(rows, (ch + 1)*grain); ++row) {
					const int j = lo[1] + int(row % std::size_t(ny)), k = lo[2] + int(row/std::size_t(ny));
					for (int ii = lo[0]; ii <= hi[0]; ++ii) {
						const std::size_t f = g.F(a, ii, j, k);
						const double A = fraction[a][f];
						if (!(A < 1)) continue;
						const V3 xf = g.FaceCentre(a, ii, j, k);
						const double mine = bodies[i].shape.Distance(ToBody(R[i], bodies[i].state.x, xf));
						if (!(mine < band_)) continue;
						// The static distance at the face centre: the mean of its corners.
						const int bb = (a + 1) % 3, cc = (a + 2) % 3;
						double st = 0;
						for (int m = 0; m < 4; ++m) {
							int q[3] = {ii, j, k};
							if (m == 1 || m == 2) ++q[bb];
							if (m == 2 || m == 3) ++q[cc];
							st += staticNodes_[g.N(q[0], q[1], q[2])];
						}
						st *= 0.25;
						if (!(mine < st)) continue;
						bool nearest = true;
						for (std::size_t q = 0; q < nb && nearest; ++q) {
							if (q == i || !inside(now[q], ii, j, k)) continue;
							const double other = bodies[q].shape.Distance(ToBody(R[q], bodies[q].state.x, xf));
							if (other < mine || (other == mine && q < i)) nearest = false;
						}
						if (!nearest) continue;
						BodyFace bf;
						bf.axis = a;
						bf.face = f;
						bf.closed = 1 - A;
						const V3 r{{xf[0] - bodies[i].state.x[0], xf[1] - bodies[i].state.x[1], xf[2] - bodies[i].state.x[2]}};
						V3 e{{0, 0, 0}};
						e[a] = 1;
						const V3 rx = Rigid::Cross(r, e);
						bf.g = {{e[0], e[1], e[2], rx[0], rx[1], rx[2]}};
						part[ch].push_back(bf);
					}
				}
			});
			for (auto& p : part) faces_[i].insert(faces_[i].end(), p.begin(), p.end());
		}
	}
	// 4. Cells inside a body with every face closed (their level set is extrapolated).
	enclosed_.clear();
	for (std::size_t i = 0; i < nb; ++i) {
		const Box& b = now[i];
		if (b.empty) continue;
		for (int k = b.lo[2]; k < std::min(b.hi[2], g.n[2]); ++k)
			for (int j = b.lo[1]; j < std::min(b.hi[1], g.n[1]); ++j)
				for (int ii = b.lo[0]; ii < std::min(b.hi[0], g.n[0]); ++ii) {
					bool closed = true;
					for (int a = 0; a < 3 && closed; ++a) {
						int q[3] = {ii, j, k};
						if (fraction[a][g.F(a, q[0], q[1], q[2])] > 0) closed = false;
						++q[a];
						if (fraction[a][g.F(a, q[0], q[1], q[2])] > 0) closed = false;
					}
					if (!closed) continue;
					if (bodies[i].shape.Distance(ToBody(R[i], bodies[i].state.x, g.CellCentre(ii, j, k))) < 0) enclosed_.push_back(g.C(ii, j, k));
				}
	}
	std::sort(enclosed_.begin(), enclosed_.end());
	enclosed_.erase(std::unique(enclosed_.begin(), enclosed_.end()), enclosed_.end());
	last_ = now;
}

void BodyGeometry::SolidVelocity(const std::vector<Vec6>& w, MacField& solid) const
{
	for (int a = 0; a < 3; ++a) {
		if (solid.u[a].size() != g_.Faces(a)) solid.u[a].assign(g_.Faces(a), 0.0);
		for (std::size_t f : touched_[a]) solid.u[a][f] = 0;
	}
	auto& touched = touched_;
	for (auto& t : touched) t.clear();
	for (std::size_t i = 0; i < faces_.size() && i < w.size(); ++i)
		for (const BodyFace& f : faces_[i]) {
			double s = 0;
			for (int k = 0; k < 6; ++k) s += f.g[std::size_t(k)]*w[i][std::size_t(k)];
			solid.u[f.axis][f.face] = s;
			touched[f.axis].push_back(f.face);
		}
}

// ------------------------------------------------------------------ coupling

BodyCoupling::BodyCoupling(const Grid& g, const std::vector<BodyFace>& faces, const LiquidBody& body, double density, bool coupled)
	: g_(g), faces_(faces), scale_(density*g.h*g.h*g.h), free_(body.free), coupled_(coupled)
{
	invMass_ = body.state.mass > 0 ? 1/body.state.mass : 0;
	std::array<double,9> inertia;
	WorldInertia(body.state, inertia, invInertia_);
}

void BodyCoupling::Begin(const std::vector<std::uint8_t>& active)
{
	// (cell, order, contribution): a face adds s (1 - A) g to the cell on its minus side
	// (s = +1, the face is that cell's plus face) and to the cell on its plus side (s = -1).
	struct Item { std::size_t cell, order; Vec6 v; };
	std::vector<Item> items;
	items.reserve(2*faces_.size());
	std::size_t order = 0;
	for (const BodyFace& f : faces_) {
		const auto d = g_.FaceDims(f.axis);
		const std::size_t i = f.face % std::size_t(d[0]), j = (f.face/std::size_t(d[0])) % std::size_t(d[1]),
			k = f.face/(std::size_t(d[0])*std::size_t(d[1]));
		int idx[3] = {int(i), int(j), int(k)};
		for (int side = 0; side < 2; ++side) {
			int c[3] = {idx[0], idx[1], idx[2]};
			if (side == 0) --c[f.axis];
			if (c[f.axis] < 0 || c[f.axis] >= g_.n[f.axis]) continue;
			const std::size_t cell = g_.C(c[0], c[1], c[2]);
			if (!active[cell]) continue;
			const double s = (side == 0 ? 1.0 : -1.0)*f.closed;
			Item it{cell, order++, {}};
			for (int m = 0; m < 6; ++m) it.v[std::size_t(m)] = s*f.g[std::size_t(m)];
			items.push_back(it);
		}
	}
	std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.cell != b.cell ? a.cell < b.cell : a.order < b.order; });
	entries_.clear();
	for (const Item& it : items) {
		if (entries_.empty() || entries_.back().first != it.cell) entries_.push_back({it.cell, Vec6{{0, 0, 0, 0, 0, 0}}});
		for (int m = 0; m < 6; ++m) entries_.back().second[std::size_t(m)] += it.v[std::size_t(m)];
	}
}

Vec6 BodyCoupling::InverseMass(const Vec6& v) const
{
	Vec6 out{{0, 0, 0, 0, 0, 0}};
	for (int a = 0; a < 3; ++a) if (free_[std::size_t(a)]) out[std::size_t(a)] = invMass_*v[std::size_t(a)];
	if (free_[3])
		for (int r = 0; r < 3; ++r) out[std::size_t(3 + r)] = invInertia_[std::size_t(3*r)]*v[3] + invInertia_[std::size_t(3*r + 1)]*v[4] +
			invInertia_[std::size_t(3*r + 2)]*v[5];
	return out;
}

Vec6 BodyCoupling::Measure(const std::vector<double>& q) const
{
	Vec6 s{{0, 0, 0, 0, 0, 0}};
	for (const auto& e : entries_) for (int m = 0; m < 6; ++m) s[std::size_t(m)] += e.second[std::size_t(m)]*q[e.first];
	for (double& x : s) x *= scale_;
	return s;
}

void BodyCoupling::ApplyAdd(const std::vector<double>& q, std::vector<double>& out) const
{
	if (!coupled_) return;
	// out_c += J_c . (rho h^3 M^-1 J q): Measure carries the one factor rho h^3.
	const Vec6 y = InverseMass(Measure(q));
	for (const auto& e : entries_) {
		double s = 0;
		for (int m = 0; m < 6; ++m) s += e.second[std::size_t(m)]*y[std::size_t(m)];
		out[e.first] += s;
	}
}

void BodyCoupling::Finish(const std::vector<double>& q)
{
	impulse_ = Measure(q);
}

} } }
