//////////////////////////////////////////////////////////////////////
//
//  RigidBody.cpp - see RigidBody.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RigidBody.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace RISE { namespace Simulation { namespace Rigid {

namespace {
constexpr double Pi = 3.14159265358979323846;
}

V3 Add(const V3& a, const V3& b) { return {{a[0] + b[0], a[1] + b[1], a[2] + b[2]}}; }
V3 Sub(const V3& a, const V3& b) { return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}}; }
V3 Mul(const V3& a, double s) { return {{a[0]*s, a[1]*s, a[2]*s}}; }
V3 Cross(const V3& a, const V3& b) { return {{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}}; }
double Dot(const V3& a, const V3& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
double Norm(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Apply(const M3& m, const V3& a)
{
	return {{m[0]*a[0] + m[1]*a[1] + m[2]*a[2], m[3]*a[0] + m[4]*a[1] + m[5]*a[2], m[6]*a[0] + m[7]*a[1] + m[8]*a[2]}};
}
V3 ApplyT(const M3& m, const V3& a)
{
	return {{m[0]*a[0] + m[3]*a[1] + m[6]*a[2], m[1]*a[0] + m[4]*a[1] + m[7]*a[2], m[2]*a[0] + m[5]*a[1] + m[8]*a[2]}};
}
M3 Multiply(const M3& a, const M3& b)
{
	M3 c{};
	for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
		double s = 0;
		for (int k = 0; k < 3; ++k) s += a[3*i + k]*b[3*k + j];
		c[3*i + j] = s;
	}
	return c;
}
Quat QMul(const Quat& a, const Quat& b)
{
	return {{a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3],
		a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2],
		a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1],
		a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0]}};
}
Quat QAxis(int axis, double angle)
{
	Quat q{{std::cos(0.5*angle), 0, 0, 0}};
	q[1 + axis] = std::sin(0.5*angle);
	return q;
}
M3 QMatrix(const Quat& q)
{
	const double w = q[0], x = q[1], y = q[2], z = q[3];
	return {{1 - 2*(y*y + z*z), 2*(x*y - w*z), 2*(x*z + w*y),
		2*(x*y + w*z), 1 - 2*(x*x + z*z), 2*(y*z - w*x),
		2*(x*z - w*y), 2*(y*z + w*x), 1 - 2*(x*x + y*y)}};
}
void QNormalize(Quat& q)
{
	const double n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	for (double& c : q) c /= n;
}

const char* ShapeName(Shape::Type t)
{
	switch (t) {
	case Shape::Type::Box: return "box";
	case Shape::Type::Sphere: return "sphere";
	case Shape::Type::Cylinder: return "cylinder";
	case Shape::Type::Mesh: break;
	}
	return "mesh";
}

bool ShapeMass(const Shape& s, double density, MassProperties& out, std::string& error)
{
	out = MassProperties();
	if (!(density > 0) || !std::isfinite(density)) { error = "density must be positive"; return false; }
	M3 I{};
	switch (s.type) {
	case Shape::Type::Box: {
		const double a = s.size[0], b = s.size[1], c = s.size[2];
		if (!(a > 0) || !(b > 0) || !(c > 0)) { error = "box size must be positive"; return false; }
		out.volume = a*b*c;
		out.mass = density*out.volume;
		I[0] = out.mass*(b*b + c*c)/12; I[4] = out.mass*(a*a + c*c)/12; I[8] = out.mass*(a*a + b*b)/12;
		break;
	}
	case Shape::Type::Sphere: {
		const double r = s.radius;
		if (!(r > 0)) { error = "sphere radius must be positive"; return false; }
		out.volume = 4.0/3.0*Pi*r*r*r;
		out.mass = density*out.volume;
		I[0] = I[4] = I[8] = 0.4*out.mass*r*r;
		break;
	}
	case Shape::Type::Cylinder: {
		const double r = s.radius, h = s.height;
		if (!(r > 0) || !(h > 0)) { error = "cylinder radius and height must be positive"; return false; }
		out.volume = Pi*r*r*h;
		out.mass = density*out.volume;
		I[0] = I[4] = out.mass*(3*r*r + h*h)/12; I[8] = 0.5*out.mass*r*r;
		break;
	}
	case Shape::Type::Mesh: {
		const auto& v = s.vertices;
		const auto& t = s.triangles;
		if (v.size() < 12 || v.size() % 3 || t.size() < 12 || t.size() % 3) { error = "a mesh body needs a closed triangle mesh"; return false; }
		for (double c : v) if (!std::isfinite(c)) { error = "mesh body vertices must be finite"; return false; }
		// Closed: every edge used exactly twice, once in each direction -- on the
		// vertices welded by exact position (a tessellation may split vertices
		// along seams; the divergence theorem needs only a closed surface).
		// Welded on a lattice of 1e-6 of the extent (binary32 seams agree to that).
		double lo[3] = {v[0], v[1], v[2]}, hi[3] = {v[0], v[1], v[2]};
		for (std::size_t i = 0; i < v.size(); ++i) { lo[i%3] = std::min(lo[i%3], v[i]); hi[i%3] = std::max(hi[i%3], v[i]); }
		const double q = 1e-6*std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-300});
		std::map<std::array<long long,3>, std::uint32_t> weld;
		std::vector<std::uint32_t> id(v.size()/3);
		for (std::size_t i = 0; i < id.size(); ++i)
			id[i] = weld.emplace(std::array<long long,3>{{std::llround((v[3*i] - lo[0])/q), std::llround((v[3*i + 1] - lo[1])/q),
				std::llround((v[3*i + 2] - lo[2])/q)}}, static_cast<std::uint32_t>(weld.size())).first->second;
		std::map<std::pair<std::uint32_t,std::uint32_t>, int> edges;
		for (std::size_t k = 0; k < t.size(); k += 3) {
			for (int e = 0; e < 3; ++e) if (t[k + e]*3ull >= v.size()) { error = "mesh body triangle index out of range"; return false; }
			// A triangle collapsed onto an edge or point (a tessellation's pole) encloses nothing.
			if (id[t[k]] == id[t[k + 1]] || id[t[k + 1]] == id[t[k + 2]] || id[t[k]] == id[t[k + 2]]) continue;
			for (int e = 0; e < 3; ++e) {
			const std::uint32_t a = id[t[k + e]], b = id[t[k + (e + 1)%3]];
			if (++edges[{a, b}] > 1) { error = "mesh body is not a closed, consistently oriented surface"; return false; }
			}
		}
		for (const auto& e : edges)
			if (edges.find({e.first.second, e.first.first}) == edges.end()) {
				error = "mesh body is not closed"; return false;
			}
		double vol = 0;
		V3 first{{0, 0, 0}};
		M3 second{};
		for (std::size_t k = 0; k < t.size(); k += 3) {
			const V3 a{{v[3*t[k]], v[3*t[k] + 1], v[3*t[k] + 2]}};
			const V3 b{{v[3*t[k+1]], v[3*t[k+1] + 1], v[3*t[k+1] + 2]}};
			const V3 c{{v[3*t[k+2]], v[3*t[k+2] + 1], v[3*t[k+2] + 2]}};
			const double det = Dot(a, Cross(b, c));
			vol += det/6;
			const V3 sum = Add(Add(a, b), c);
			first = Add(first, Mul(sum, det/24));
			// Integral of x x^T over the tetrahedron (0, a, b, c).
			for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
				second[3*i + j] += det/120*(sum[i]*sum[j] + a[i]*a[j] + b[i]*b[j] + c[i]*c[j]);
		}
		if (!(vol > 0)) { error = "mesh body has no positive volume (open or inward-facing)"; return false; }
		out.volume = vol;
		out.mass = density*vol;
		out.centroid = Mul(first, 1/vol);
		// Covariance about the centroid, then I = tr(C) 1 - C.
		M3 C{};
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
			C[3*i + j] = density*(second[3*i + j] - vol*out.centroid[i]*out.centroid[j]);
		const double tr = C[0] + C[4] + C[8];
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) I[3*i + j] = (i == j ? tr : 0) - C[3*i + j];
		break;
	}
	}
	out.inertia = I;
	return true;
}

void PrincipalAxes(const M3& inertia, V3& moments, M3& axes)
{
	double a[3][3], p[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) a[i][j] = 0.5*(inertia[3*i + j] + inertia[3*j + i]);
	for (int sweep = 0; sweep < 64; ++sweep) {
		const double off = a[0][1]*a[0][1] + a[0][2]*a[0][2] + a[1][2]*a[1][2];
		const double diag = a[0][0]*a[0][0] + a[1][1]*a[1][1] + a[2][2]*a[2][2];
		if (!(off > 1e-30*diag)) break;
		for (int r = 0; r < 2; ++r) for (int c = r + 1; c < 3; ++c) {
			if (a[r][c] == 0) continue;
			const double theta = 0.5*std::atan2(2*a[r][c], a[c][c] - a[r][r]);
			const double cs = std::cos(theta), sn = std::sin(theta);
			for (int k = 0; k < 3; ++k) {
				const double akr = a[k][r], akc = a[k][c];
				a[k][r] = cs*akr - sn*akc; a[k][c] = sn*akr + cs*akc;
			}
			for (int k = 0; k < 3; ++k) {
				const double ark = a[r][k], ack = a[c][k];
				a[r][k] = cs*ark - sn*ack; a[c][k] = sn*ark + cs*ack;
			}
			for (int k = 0; k < 3; ++k) {
				const double pkr = p[k][r], pkc = p[k][c];
				p[k][r] = cs*pkr - sn*pkc; p[k][c] = sn*pkr + cs*pkc;
			}
		}
	}
	int order[3] = {0, 1, 2};
	std::sort(order, order + 3, [&](int i, int j) { return a[i][i] < a[j][j] || (a[i][i] == a[j][j] && i < j); });
	for (int c = 0; c < 3; ++c) {
		moments[c] = a[order[c]][order[c]];
		// Canonical column sign: the largest component positive.
		int big = 0;
		for (int k = 1; k < 3; ++k) if (std::fabs(p[k][order[c]]) > std::fabs(p[big][order[c]])) big = k;
		const double s = p[big][order[c]] < 0 ? -1 : 1;
		for (int k = 0; k < 3; ++k) axes[3*k + c] = s*p[k][order[c]];
	}
	// Proper rotation: flip the last axis when the frame is left-handed.
	const V3 c0{{axes[0], axes[3], axes[6]}}, c1{{axes[1], axes[4], axes[7]}}, c2{{axes[2], axes[5], axes[8]}};
	if (Dot(Cross(c0, c1), c2) < 0) for (int k = 0; k < 3; ++k) axes[3*k + 2] = -axes[3*k + 2];
}

void ShapeSurface(const Shape& s, std::vector<double>& v, std::vector<std::uint32_t>& t)
{
	v.clear(); t.clear();
	switch (s.type) {
	case Shape::Type::Box: {
		const double hx = s.size[0]/2, hy = s.size[1]/2, hz = s.size[2]/2;
		for (int k = 0; k < 8; ++k) v.insert(v.end(), {k & 1 ? hx : -hx, k & 2 ? hy : -hy, k & 4 ? hz : -hz});
		// Faces (outward, counter-clockwise from outside).
		const std::uint32_t f[12][3] = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
			{2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
		for (const auto& tri : f) t.insert(t.end(), {tri[0], tri[1], tri[2]});
		break;
	}
	case Shape::Type::Sphere: {
		// Icosahedron subdivided once, projected to the sphere: 42 vertices, 80 triangles.
		const double g = (1 + std::sqrt(5.0))/2;
		std::vector<V3> p = {{{-1, g, 0}}, {{1, g, 0}}, {{-1, -g, 0}}, {{1, -g, 0}}, {{0, -1, g}}, {{0, 1, g}},
			{{0, -1, -g}}, {{0, 1, -g}}, {{g, 0, -1}}, {{g, 0, 1}}, {{-g, 0, -1}}, {{-g, 0, 1}}};
		const std::uint32_t f[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
			{11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5},
			{2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
		std::map<std::pair<std::uint32_t,std::uint32_t>, std::uint32_t> mid;
		auto middle = [&](std::uint32_t a, std::uint32_t b) {
			const auto key = std::make_pair(std::min(a, b), std::max(a, b));
			const auto it = mid.find(key);
			if (it != mid.end()) return it->second;
			p.push_back(Mul(Add(p[a], p[b]), 0.5));
			const std::uint32_t id = static_cast<std::uint32_t>(p.size() - 1);
			mid[key] = id;
			return id;
		};
		for (const auto& tri : f) {
			const std::uint32_t a = middle(tri[0], tri[1]), b = middle(tri[1], tri[2]), c = middle(tri[2], tri[0]);
			t.insert(t.end(), {tri[0], a, c, tri[1], b, a, tri[2], c, b, a, b, c});
		}
		for (const auto& q : p) {
			const V3 u = Mul(q, s.radius/Norm(q));
			v.insert(v.end(), {u[0], u[1], u[2]});
		}
		break;
	}
	case Shape::Type::Cylinder: {
		const std::uint32_t n = 32;
		const double h = s.height/2;
		for (std::uint32_t k = 0; k < n; ++k) {
			const double a = 2*Pi*static_cast<double>(k)/n;
			v.insert(v.end(), {s.radius*std::cos(a), s.radius*std::sin(a), -h});
			v.insert(v.end(), {s.radius*std::cos(a), s.radius*std::sin(a), h});
		}
		v.insert(v.end(), {0, 0, -h, 0, 0, h});
		const std::uint32_t bottom = 2*n, top = 2*n + 1;
		for (std::uint32_t k = 0; k < n; ++k) {
			const std::uint32_t a0 = 2*k, a1 = 2*k + 1, b0 = 2*((k + 1)%n), b1 = 2*((k + 1)%n) + 1;
			t.insert(t.end(), {a0, b0, b1, a0, b1, a1, bottom, b0, a0, top, a1, b1});
		}
		break;
	}
	case Shape::Type::Mesh:
		v = s.vertices; t = s.triangles;
		break;
	}
}

void ShapeSupportPoints(const Shape& s, std::vector<double>& points)
{
	std::vector<std::uint32_t> ignored;
	if (s.type == Shape::Type::Sphere) { points.clear(); return; }
	ShapeSurface(s, points, ignored);
	if (s.type == Shape::Type::Cylinder) points.resize(points.size() - 6); // the cap centres never touch first
}

V3 Body::AngularVelocity() const
{
	const V3 w{{pi[0]/moments[0], pi[1]/moments[1], pi[2]/moments[2]}};
	return Apply(QMatrix(q), w);
}

double Body::KineticEnergy() const
{
	return 0.5*mass*Dot(v, v) + 0.5*(pi[0]*pi[0]/moments[0] + pi[1]*pi[1]/moments[1] + pi[2]*pi[2]/moments[2]);
}

V3 Body::AngularMomentum() const
{
	return Add(Mul(Cross(x, v), mass), Apply(QMatrix(q), pi));
}

namespace {
// The exact flow of H_i = pi_i^2 / (2 I_i) for time h: a rotation by
// theta = h pi_i / I_i about body axis i (R <- R exp(theta e_i), pi <- exp(-theta e_i) pi).
void AxisFlow(Body& b, int i, double h)
{
	const double theta = h*b.pi[i]/b.moments[i];
	if (theta == 0) return;
	const double c = std::cos(theta), s = std::sin(theta);
	const int j = (i + 1)%3, k = (i + 2)%3;
	const double pj = b.pi[j], pk = b.pi[k];
	b.pi[j] = c*pj + s*pk;
	b.pi[k] = -s*pj + c*pk;
	b.q = QMul(b.q, QAxis(i, theta));
}

void Kick(Body& b, const Load& load, const V3& gravity, double h)
{
	b.v = Add(b.v, Mul(Add(Mul(load.force, 1/b.mass), gravity), h));
	V3 tau = load.torque;
	if (!load.atCentre) tau = Add(tau, Cross(Sub(load.point, b.x), load.force));
	if (tau[0] != 0 || tau[1] != 0 || tau[2] != 0) b.pi = Add(b.pi, Mul(ApplyT(QMatrix(b.q), tau), h));
}
}

void FreeRotation(Body& b, double h)
{
	AxisFlow(b, 0, 0.5*h);
	AxisFlow(b, 1, 0.5*h);
	AxisFlow(b, 2, h);
	AxisFlow(b, 1, 0.5*h);
	AxisFlow(b, 0, 0.5*h);
	QNormalize(b.q);
}

void KickBody(Body& b, const Load& load, const V3& gravity, double h) { Kick(b, load, gravity, h); }

void DriftBody(Body& b, double h)
{
	b.x = Add(b.x, Mul(b.v, h));
	FreeRotation(b, h);
}

void StepBody(Body& b, const Load& load, const V3& gravity, double h)
{
	Kick(b, load, gravity, 0.5*h);
	DriftBody(b, h);
	Kick(b, load, gravity, 0.5*h);
}

namespace {
V3 InverseInertia(const Body& b, const M3& R, const V3& a)
{
	const V3 l = ApplyT(R, a);
	return Apply(R, V3{{l[0]/b.moments[0], l[1]/b.moments[1], l[2]/b.moments[2]}});
}
} // namespace

// M7h: sequential impulses with accumulated, clamped impulses over a set of contact points
// between body a and body b (null: a static plane), normals pointing the way a must move.
// Normal: the relative normal velocity reaches -e vn0 (the restitution of the approach speed
// vn0 before the solve, 0 below the resting speed) and the accumulated impulse stays >= 0;
// friction along two tangents, the accumulated pair clamped to mu times the normal impulse.
// Fixed passes in point order: deterministic.
namespace {
// One contact row of a set solved together: bodies a and b (b < 0: a static plane).
struct Row {
	int a = 0, b = -1;
	V3 ra{{0, 0, 0}}, rb{{0, 0, 0}}, n{{0, 0, 0}}, t1{{0, 0, 0}}, t2{{0, 0, 0}};
	double kn = 0, kt1 = 0, kt2 = 0, bias = 0, ln = 0, lt1 = 0, lt2 = 0, e = 0, mu = 0;
};
// Sequential impulses with accumulated, clamped impulses over the rows (point order, fixed passes).
int SolveRows(std::vector<Body*>& bodies, const std::vector<std::uint8_t>& moves, std::vector<Row>& rows, double resting, int passes)
{
	auto R = [&](int i) { return QMatrix(bodies[std::size_t(i)]->q); };
	auto relative = [&](const Row& r) {
		const Body& a = *bodies[std::size_t(r.a)];
		V3 v = Add(a.v, Cross(a.AngularVelocity(), r.ra));
		if (r.b >= 0) { const Body& b = *bodies[std::size_t(r.b)]; v = Sub(v, Add(b.v, Cross(b.AngularVelocity(), r.rb))); }
		return v;
	};
	auto mass = [&](const Row& r, const V3& d) {
		double k = 0;
		if (moves[std::size_t(r.a)]) {
			const Body& a = *bodies[std::size_t(r.a)];
			k += 1/a.mass + Dot(d, Cross(InverseInertia(a, R(r.a), Cross(r.ra, d)), r.ra));
		}
		if (r.b >= 0 && moves[std::size_t(r.b)]) {
			const Body& b = *bodies[std::size_t(r.b)];
			k += 1/b.mass + Dot(d, Cross(InverseInertia(b, R(r.b), Cross(r.rb, d)), r.rb));
		}
		return k;
	};
	auto apply = [&](const Row& r, const V3& J) {   // +J on a, -J on b at the point
		if (moves[std::size_t(r.a)]) { Body& a = *bodies[std::size_t(r.a)]; a.v = Add(a.v, Mul(J, 1/a.mass)); a.pi = Add(a.pi, ApplyT(R(r.a), Cross(r.ra, J))); }
		if (r.b >= 0 && moves[std::size_t(r.b)]) {
			Body& b = *bodies[std::size_t(r.b)];
			b.v = Sub(b.v, Mul(J, 1/b.mass)); b.pi = Sub(b.pi, ApplyT(R(r.b), Cross(r.rb, J)));
		}
	};
	for (Row& r : rows) {
		int k = 0;
		for (int m = 1; m < 3; ++m) if (std::fabs(r.n[std::size_t(m)]) < std::fabs(r.n[std::size_t(k)])) k = m;
		V3 axis{{0, 0, 0}};
		axis[std::size_t(k)] = 1;
		r.t1 = Sub(axis, Mul(r.n, Dot(axis, r.n)));
		r.t1 = Mul(r.t1, 1/Norm(r.t1));
		r.t2 = Cross(r.n, r.t1);
		r.kn = mass(r, r.n); r.kt1 = mass(r, r.t1); r.kt2 = mass(r, r.t2);
		const double vn0 = Dot(relative(r), r.n);
		r.bias = -vn0 > resting ? -r.e*vn0 : 0.0;
	}
	int impulses = 0;
	for (int pass = 0; pass < passes; ++pass)
		for (Row& r : rows) {
			if (!(r.kn > 0)) continue;
			const double vn = Dot(relative(r), r.n);
			const double ln = std::max(0.0, r.ln + (r.bias - vn)/r.kn);
			const double dn = ln - r.ln;
			r.ln = ln;
			if (dn != 0) { apply(r, Mul(r.n, dn)); if (pass == 0) ++impulses; }
			if (r.mu > 0 && r.kt1 > 0 && r.kt2 > 0) {
				const V3 vr = relative(r);
				double l1 = r.lt1 - Dot(vr, r.t1)/r.kt1, l2 = r.lt2 - Dot(vr, r.t2)/r.kt2;
				const double cap = r.mu*r.ln, l = std::sqrt(l1*l1 + l2*l2);
				if (l > cap) { l1 *= cap/l; l2 *= cap/l; }
				apply(r, Add(Mul(r.t1, l1 - r.lt1), Mul(r.t2, l2 - r.lt2)));
				r.lt1 = l1; r.lt2 = l2;
			}
		}
	return impulses;
}
// A plane's support points of body b within the slop (none unless one lies below the plane).
bool PlanePoints(const Body& b, const Plane& pl, double slop, std::vector<ContactPoint>& out, double& depth)
{
	out.clear();
	depth = 0;
	bool below = false;
	if (b.sphere) {
		const double d = Dot(Sub(b.x, pl.point), pl.normal) - b.radius;
		if (d < slop) { out.push_back({Sub(b.x, Mul(pl.normal, b.radius)), pl.normal, d}); depth = std::min(depth, d); below = d < 0; }
		return below;
	}
	const M3 R = QMatrix(b.q);
	for (std::size_t k = 0; k + 2 < b.support.size(); k += 3) {
		const V3 p = Add(b.x, Apply(R, V3{{b.support[k], b.support[k + 1], b.support[k + 2]}}));
		const double d = Dot(Sub(p, pl.point), pl.normal);
		if (d < 0) below = true;
		if (d < slop) { out.push_back({p, pl.normal, d}); depth = std::min(depth, d); }
	}
	return below;
}
} // namespace

int SolveContacts(Body& a, Body* b, bool moveA, bool moveB, const std::vector<ContactPoint>& points, double e, double mu,
	double resting, int passes)
{
	std::vector<Body*> bodies{&a};
	std::vector<std::uint8_t> moves{std::uint8_t(moveA ? 1 : 0)};
	if (b) { bodies.push_back(b); moves.push_back(moveB ? 1 : 0); }
	std::vector<Row> rows;
	for (const ContactPoint& c : points) {
		Row r;
		r.a = 0; r.b = b ? 1 : -1;
		r.ra = Sub(c.point, a.x);
		if (b) r.rb = Sub(c.point, b->x);
		r.n = c.normal; r.e = e; r.mu = mu;
		rows.push_back(r);
	}
	return SolveRows(bodies, moves, rows, resting, passes);
}

int ResolvePlanes(Body& b, const std::vector<Plane>& planes, const V3& gravity, double h)
{
	int impulses = 0;
	const double resting = 2*Norm(gravity)*h;
	for (const Plane& pl : planes) {
		if (!b.sphere) {
			// m6.2 (M7h): every support point below the plane in one set of sequential impulses
			// (m6.1 took the deepest point alone, eight times: a box resting flat on a plane took
			// its whole support at one corner each substep, tilted 0.2 rad and walked 1 cm in 5 s).
			std::vector<ContactPoint> points;
			double depth = 0;
			// Points within the slop (ResolvePairs) take part; a body with no point below is left as m6.1 left it.
			if (!PlanePoints(b, pl, resting*h, points, depth)) continue;
			impulses += SolveContacts(b, nullptr, true, false, points, pl.restitution, pl.friction, resting, ContactPasses);
			b.x = Add(b.x, Mul(pl.normal, -depth));
			continue;
		}
		for (int iteration = 0; iteration < 8; ++iteration) {
			const M3 R = QMatrix(b.q);
			// The deepest support point below the plane.
			double depth = 0;
			V3 contact{{0, 0, 0}};
			if (b.sphere) {
				const double d = Dot(Sub(b.x, pl.point), pl.normal) - b.radius;
				if (d < depth) { depth = d; contact = Sub(b.x, Mul(pl.normal, b.radius)); }
			}
			for (std::size_t k = 0; k + 2 < b.support.size(); k += 3) {
				const V3 p = Add(b.x, Apply(R, V3{{b.support[k], b.support[k + 1], b.support[k + 2]}}));
				const double d = Dot(Sub(p, pl.point), pl.normal);
				if (d < depth) { depth = d; contact = p; }
			}
			if (!(depth < 0)) break;
			const V3 r = Sub(contact, b.x);
			auto invInertia = [&](const V3& a) {
				const V3 l = ApplyT(R, a);
				return Apply(R, V3{{l[0]/b.moments[0], l[1]/b.moments[1], l[2]/b.moments[2]}});
			};
			auto pointVelocity = [&]() { return Add(b.v, Cross(b.AngularVelocity(), r)); };
			auto applyImpulse = [&](const V3& J) {
				b.v = Add(b.v, Mul(J, 1/b.mass));
				b.pi = Add(b.pi, ApplyT(R, Cross(r, J)));
			};
			const V3 n = pl.normal;
			const double vn = Dot(pointVelocity(), n);
			if (vn < 0) {
				const double kn = 1/b.mass + Dot(n, Cross(invInertia(Cross(r, n)), r));
				const double e = -vn > resting ? pl.restitution : 0.0;
				const double jn = -(1 + e)*vn/kn;
				applyImpulse(Mul(n, jn));
				++impulses;
				const V3 vp = pointVelocity();
				const V3 vt = Sub(vp, Mul(n, Dot(vp, n)));
				const double speed = Norm(vt);
				if (speed > 0 && pl.friction > 0) {
					const V3 tdir = Mul(vt, 1/speed);
					const double kt = 1/b.mass + Dot(tdir, Cross(invInertia(Cross(r, tdir)), r));
					const double jt = std::min(speed/kt, pl.friction*jn);
					applyImpulse(Mul(tdir, -jt));
				}
			}
			// Out of the plane along its normal.
			b.x = Add(b.x, Mul(n, -depth));
		}
	}
	return impulses;
}

// ------------------------------------------------------------------ M7h: body-body contact

ContactShape MakeContactShape(const Shape& shape, double restitution, double friction)
{
	ContactShape c;
	c.restitution = restitution;
	c.friction = friction;
	switch (shape.type) {
	case Shape::Type::Sphere:
		c.type = ContactShape::Type::Sphere;
		c.half = {{shape.radius, shape.radius, shape.radius}};
		c.bound = shape.radius;
		break;
	case Shape::Type::Box: {
		c.type = ContactShape::Type::Box;
		c.half = Mul(shape.size, 0.5);
		c.bound = Norm(c.half);
		const double h[3] = {c.half[0], c.half[1], c.half[2]};
		for (int k = 0; k < 8; ++k) c.points.insert(c.points.end(), {k & 1 ? h[0] : -h[0], k & 2 ? h[1] : -h[1], k & 4 ? h[2] : -h[2]});
		// Edge midpoints: an edge resting across another body's face between its corners.
		for (int a = 0; a < 3; ++a)
			for (int k = 0; k < 4; ++k) {
				double q[3];
				q[a] = 0;
				q[(a + 1) % 3] = k & 1 ? h[(a + 1) % 3] : -h[(a + 1) % 3];
				q[(a + 2) % 3] = k & 2 ? h[(a + 2) % 3] : -h[(a + 2) % 3];
				c.points.insert(c.points.end(), {q[0], q[1], q[2]});
			}
		break;
	}
	case Shape::Type::Cylinder: {
		c.type = ContactShape::Type::Cylinder;
		c.half = {{shape.radius, shape.radius, shape.height/2}};
		c.bound = std::sqrt(shape.radius*shape.radius + shape.height*shape.height/4);
		std::vector<std::uint32_t> ignored;
		ShapeSurface(shape, c.points, ignored);
		c.points.resize(c.points.size() - 6);   // the rims (the cap centres touch only after a rim)
		break;
	}
	case Shape::Type::Mesh:
		c.type = ContactShape::Type::None;
		break;
	}
	return c;
}

double ShapeDistance(const ContactShape& s, const V3& p, V3& normal, const V3* hint)
{
	// Faces within this of the deepest (a point on an edge or corner of the shape, as a box's
	// corner on a box stacked flush) are a tie, broken by the hint: the face whose normal points
	// most along the direction from this body to the other.
	const double tie = 1e-9*(s.half[0] + s.half[1] + s.half[2]);
	normal = {{0, 1, 0}};
	switch (s.type) {
	case ContactShape::Type::Sphere: {
		const double l = Norm(p);
		if (l > 0) normal = Mul(p, 1/l);
		return l - s.half[0];
	}
	case ContactShape::Type::Box: {
		const V3 q{{std::fabs(p[0]) - s.half[0], std::fabs(p[1]) - s.half[1], std::fabs(p[2]) - s.half[2]}};
		const V3 out{{std::max(q[0], 0.0), std::max(q[1], 0.0), std::max(q[2], 0.0)}};
		const double lo = Norm(out);
		if (lo > 0) {
			for (int a = 0; a < 3; ++a) normal[std::size_t(a)] = (p[std::size_t(a)] < 0 ? -1.0 : 1.0)*out[std::size_t(a)]/lo;
			return lo;
		}
		int a = 0;
		for (int k = 1; k < 3; ++k) if (q[std::size_t(k)] > q[std::size_t(a)]) a = k;
		if (hint) {
			int best = a;
			double align = -1;
			for (int k = 0; k < 3; ++k) {
				if (q[std::size_t(k)] < q[std::size_t(a)] - tie) continue;
				const double w = std::fabs((*hint)[std::size_t(k)])/s.half[std::size_t(k)];
				if (w > align) { align = w; best = k; }
			}
			a = best;
		}
		normal = {{0, 0, 0}};
		normal[std::size_t(a)] = p[std::size_t(a)] < 0 ? -1.0 : 1.0;
		return q[std::size_t(a)];
	}
	case ContactShape::Type::Cylinder: {
		const double rho = std::sqrt(p[0]*p[0] + p[1]*p[1]);
		const double dr = rho - s.half[0], dz = std::fabs(p[2]) - s.half[2];
		const V3 radial = rho > 0 ? V3{{p[0]/rho, p[1]/rho, 0}} : V3{{1, 0, 0}};
		const V3 axial{{0, 0, p[2] < 0 ? -1.0 : 1.0}};
		if (dr > 0 && dz > 0) {
			const double l = std::sqrt(dr*dr + dz*dz);
			normal = Add(Mul(radial, dr/l), Mul(axial, dz/l));
			return l;
		}
		bool side = dr >= dz;
		if (hint && std::fabs(dr - dz) <= tie) {
			const double hr = std::sqrt((*hint)[0]*(*hint)[0] + (*hint)[1]*(*hint)[1])/s.half[0], hz = std::fabs((*hint)[2])/s.half[2];
			side = hr >= hz;
		}
		if (side) { normal = radial; return dr; }
		normal = axial;
		return dz;
	}
	case ContactShape::Type::None: break;
	}
	return std::numeric_limits<double>::infinity();
}

bool PairsNear(const std::vector<Body>& bodies, const std::vector<ContactShape>& shapes)
{
	for (std::size_t i = 0; i < bodies.size(); ++i) {
		if (shapes[i].type == ContactShape::Type::None) continue;
		for (std::size_t j = i + 1; j < bodies.size(); ++j) {
			if (shapes[j].type == ContactShape::Type::None) continue;
			if (Norm(Sub(bodies[j].x, bodies[i].x)) < shapes[i].bound + shapes[j].bound) return true;
		}
	}
	return false;
}

namespace {
// The penetrating points of body a and body b (a's sample points in b, b's in a, a sphere's centre).
void FindPairContacts(const Body& a, const ContactShape& sa, const Body& b, const ContactShape& sb, std::vector<ContactPoint>& out, double slop)
{
	out.clear();
	const M3 Ra = QMatrix(a.q), Rb = QMatrix(b.q);
	const V3 towardA = ApplyT(Rb, Sub(a.x, b.x)), towardB = ApplyT(Ra, Sub(b.x, a.x));   // body frames
	auto inB = [&](const V3& w, double radius, ContactPoint& c) {
		V3 n;
		const double d = ShapeDistance(sb, ApplyT(Rb, Sub(w, b.x)), n, &towardA) - radius;
		c.normal = Apply(Rb, n);
		c.depth = d;
		c.point = Sub(w, Mul(c.normal, radius));
		return d < slop;
	};
	auto inA = [&](const V3& w, double radius, ContactPoint& c) {
		V3 n;
		const double d = ShapeDistance(sa, ApplyT(Ra, Sub(w, a.x)), n, &towardB) - radius;
		const V3 nw = Apply(Ra, n);
		c.normal = Mul(nw, -1);
		c.depth = d;
		c.point = Sub(w, Mul(nw, radius));
		return d < slop;
	};
	ContactPoint c;
	if (sa.type == ContactShape::Type::Sphere && sb.type == ContactShape::Type::Sphere) {
		const V3 d = Sub(a.x, b.x);
		const double l = Norm(d);
		c.normal = l > 0 ? Mul(d, 1/l) : V3{{0, 1, 0}};
		c.depth = l - sa.half[0] - sb.half[0];
		c.point = Add(b.x, Mul(c.normal, sb.half[0] + 0.5*c.depth));
		if (c.depth < slop) out.push_back(c);
		return;
	}
	if (sa.type == ContactShape::Type::Box && sb.type == ContactShape::Type::Box) {
		// Two boxes: one normal for the pair, the axis of least overlap among the 15 separating
		// axes (face axes preferred unless an edge axis overlaps clearly less), so that the points
		// of a box stacked flush on another (on the other's side planes too) are pushed apart
		// along the stacking axis, not sideways.  Each penetrating sample point's depth is
		// measured along that normal.
		const V3 d = Sub(a.x, b.x);
		V3 axesA[3], axesB[3];
		for (int k = 0; k < 3; ++k) {
			axesA[k] = {{Ra[std::size_t(k)], Ra[std::size_t(3 + k)], Ra[std::size_t(6 + k)]}};
			axesB[k] = {{Rb[std::size_t(k)], Rb[std::size_t(3 + k)], Rb[std::size_t(6 + k)]}};
		}
		auto extent = [](const V3* axes, const V3& half, const V3& l) {
			return half[0]*std::fabs(Dot(l, axes[0])) + half[1]*std::fabs(Dot(l, axes[1])) + half[2]*std::fabs(Dot(l, axes[2]));
		};
		double least = std::numeric_limits<double>::infinity();
		V3 n{{0, 1, 0}};
		auto test = [&](V3 l, double prefer) {
			const double len = Norm(l);
			if (!(len > 1e-9)) return true;
			l = Mul(l, 1/len);
			const double sep = Dot(l, d);
			const double o = extent(axesA, sa.half, l) + extent(axesB, sb.half, l) - std::fabs(sep);
			if (o < -slop) return false;   // separated along l
			if (o*prefer < least) { least = o*prefer; n = sep < 0 ? Mul(l, -1) : l; }
			return true;
		};
		for (int k = 0; k < 3; ++k) if (!test(axesA[k], 1) || !test(axesB[k], 1)) return;
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) if (!test(Cross(axesA[i], axesB[j]), 1/0.95)) return;
		const double hA = extent(axesA, sa.half, n), hB = extent(axesB, sb.half, n);
		V3 ignored;
		for (std::size_t k = 0; k + 2 < sa.points.size(); k += 3) {
			const V3 w = Add(a.x, Apply(Ra, V3{{sa.points[k], sa.points[k + 1], sa.points[k + 2]}}));
			if (!(ShapeDistance(sb, ApplyT(Rb, Sub(w, b.x)), ignored) < slop)) continue;
			c.point = w; c.normal = n; c.depth = Dot(n, Sub(w, b.x)) - hB;
			if (c.depth < slop) out.push_back(c);
		}
		for (std::size_t k = 0; k + 2 < sb.points.size(); k += 3) {
			const V3 w = Add(b.x, Apply(Rb, V3{{sb.points[k], sb.points[k + 1], sb.points[k + 2]}}));
			if (!(ShapeDistance(sa, ApplyT(Ra, Sub(w, a.x)), ignored) < slop)) continue;
			c.point = w; c.normal = n; c.depth = -Dot(n, Sub(w, a.x)) - hA;
			if (c.depth < slop) out.push_back(c);
		}
		return;
	}
	if (sa.type == ContactShape::Type::Sphere) { if (inB(a.x, sa.half[0], c)) out.push_back(c); }
	else for (std::size_t k = 0; k + 2 < sa.points.size(); k += 3)
		if (inB(Add(a.x, Apply(Ra, V3{{sa.points[k], sa.points[k + 1], sa.points[k + 2]}})), 0.0, c)) out.push_back(c);
	if (sb.type == ContactShape::Type::Sphere) { if (inA(b.x, sb.half[0], c)) out.push_back(c); }
	else for (std::size_t k = 0; k + 2 < sb.points.size(); k += 3)
		if (inA(Add(b.x, Apply(Rb, V3{{sb.points[k], sb.points[k + 1], sb.points[k + 2]}})), 0.0, c)) out.push_back(c);
}
} // namespace

int ResolvePairs(std::vector<Body>& bodies, const std::vector<ContactShape>& shapes, const std::vector<Plane>& planes,
	const V3& gravity, double h, int iterations, const std::vector<std::uint8_t>* active)
{
	int impulses = 0;
	const double resting = 2*Norm(gravity)*h;
	// Points within the slop (the fall of a resting step, 2 |g| h^2) count as touching, so a body
	// resting on another is held at the velocity level without first sinking into it.
	const double slop = resting*h;
	std::vector<Body*> ptrs;
	for (Body& b : bodies) ptrs.push_back(&b);
	std::vector<std::uint8_t> moves(bodies.size(), 1);
	if (active) moves = *active;
	std::vector<ContactPoint> contacts;
	struct Correction { int a = 0, b = -1; V3 n{{0, 0, 0}}; double depth = 0; };
	for (int it = 0; it < iterations; ++it) {
		// Every touching pair and, for the bodies in them, every touching plane: one set of rows.
		std::vector<Row> rows;
		std::vector<Correction> corrections;
		std::vector<std::uint8_t> involved(bodies.size(), 0);
		for (std::size_t i = 0; i < bodies.size(); ++i) {
			if (shapes[i].type == ContactShape::Type::None) continue;
			for (std::size_t j = i + 1; j < bodies.size(); ++j) {
				if (shapes[j].type == ContactShape::Type::None || (!moves[i] && !moves[j])) continue;
				const Body& a = bodies[i];
				const Body& b = bodies[j];
				if (!(Norm(Sub(b.x, a.x)) < shapes[i].bound + shapes[j].bound)) continue;
				FindPairContacts(a, shapes[i], b, shapes[j], contacts, slop);
				if (contacts.empty()) continue;
				involved[i] = involved[j] = 1;
				Correction c{int(i), int(j), {{0, 0, 0}}, 0};
				for (const ContactPoint& p : contacts) {
					Row r;
					r.a = int(i); r.b = int(j);
					r.ra = Sub(p.point, a.x); r.rb = Sub(p.point, b.x);
					r.n = p.normal;
					r.e = std::min(shapes[i].restitution, shapes[j].restitution);
					r.mu = std::sqrt(shapes[i].friction*shapes[j].friction);
					rows.push_back(r);
					if (p.depth < c.depth) { c.depth = p.depth; c.n = p.normal; }
				}
				if (c.depth < 0) corrections.push_back(c);
			}
		}
		if (rows.empty()) break;
		impulses += SolveRows(ptrs, moves, rows, resting, ContactPasses);
		// Apart along each pair's deepest normal in inverse proportion to the masses; out of the planes.
		for (const Correction& c : corrections) {
			Body& a = bodies[std::size_t(c.a)];
			const double ima = moves[std::size_t(c.a)] ? 1/a.mass : 0.0;
			if (c.b < 0) { if (ima > 0) a.x = Add(a.x, Mul(c.n, -c.depth)); continue; }
			Body& b = bodies[std::size_t(c.b)];
			const double imb = moves[std::size_t(c.b)] ? 1/b.mass : 0.0, share = ima + imb;
			if (!(share > 0)) continue;
			a.x = Add(a.x, Mul(c.n, -c.depth*ima/share));
			b.x = Sub(b.x, Mul(c.n, -c.depth*imb/share));
		}
		// A pair's push can drive a body into a plane: the planes again for the bodies in a pair
		// (solved after the pairs, which converged better than one set of both: measured on a stack).
		if (!planes.empty())
			for (std::size_t i = 0; i < bodies.size(); ++i)
				if (involved[i] && moves[i]) impulses += ResolvePlanes(bodies[i], planes, gravity, h);
	}
	return impulses;
}

} } }
