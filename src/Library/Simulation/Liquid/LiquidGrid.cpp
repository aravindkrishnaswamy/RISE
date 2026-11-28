//////////////////////////////////////////////////////////////////////
//
//  LiquidGrid.cpp - Staggered MAC grid sampling, analytic/mesh SDFs,
//  cut-cell face/cell open fractions, and initial liquid rasterization.
//  Conventions: uniform cell spacing h; cell centres at origin+(i+0.5)h;
//  axis-a faces at origin+i*h along a; SDF < 0 inside solid/liquid.
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidGrid.h"

#include <algorithm>
#include <cmath>

namespace RISE { namespace Simulation { namespace Liquid {

namespace {
double Clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
V3 Sub(const V3& a, const V3& b) { return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}}; }
V3 Add(const V3& a, const V3& b) { return {{a[0] + b[0], a[1] + b[1], a[2] + b[2]}}; }
V3 Mul(const V3& a, double s) { return {{a[0]*s, a[1]*s, a[2]*s}}; }
double DotV(const V3& a, const V3& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
double NormV(const V3& a) { return std::sqrt(DotV(a, a)); }

// Trilinear interpolation on an array of dims d at continuous index s.
double Trilinear(const std::vector<double>& f, const std::array<int,3>& d, const V3& s)
{
	int i0[3];
	double t[3];
	for (int a = 0; a < 3; ++a) {
		const double c = Clamp(s[a], 0.0, double(d[a] - 1));
		int i = static_cast<int>(std::floor(c));
		if (i >= d[a] - 1) i = std::max(0, d[a] - 2);
		i0[a] = i;
		t[a] = d[a] == 1 ? 0.0 : c - i;
	}
	auto at = [&](int di, int dj, int dk) {
		const int i = std::min(i0[0] + di, d[0] - 1), j = std::min(i0[1] + dj, d[1] - 1), k = std::min(i0[2] + dk, d[2] - 1);
		return f[std::size_t(i) + std::size_t(d[0])*(std::size_t(j) + std::size_t(d[1])*std::size_t(k))];
	};
	const double c00 = at(0, 0, 0)*(1 - t[0]) + at(1, 0, 0)*t[0];
	const double c10 = at(0, 1, 0)*(1 - t[0]) + at(1, 1, 0)*t[0];
	const double c01 = at(0, 0, 1)*(1 - t[0]) + at(1, 0, 1)*t[0];
	const double c11 = at(0, 1, 1)*(1 - t[0]) + at(1, 1, 1)*t[0];
	const double c0 = c00*(1 - t[1]) + c10*t[1];
	const double c1 = c01*(1 - t[1]) + c11*t[1];
	return c0*(1 - t[2]) + c1*t[2];
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection, 5.1.5).
V3 ClosestOnTriangle(const V3& p, const V3& a, const V3& b, const V3& c)
{
	const V3 ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
	const double d1 = DotV(ab, ap), d2 = DotV(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	const V3 bp = Sub(p, b);
	const double d3 = DotV(ab, bp), d4 = DotV(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	const double vc = d1*d4 - d3*d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return Add(a, Mul(ab, d1/(d1 - d3)));
	const V3 cp = Sub(p, c);
	const double d5 = DotV(ab, cp), d6 = DotV(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	const double vb = d5*d2 - d1*d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return Add(a, Mul(ac, d2/(d2 - d6)));
	const double va = d3*d6 - d5*d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return Add(b, Mul(Sub(c, b), (d4 - d3)/((d4 - d3) + (d5 - d6))));
	const double denom = 1/(va + vb + vc);
	return Add(a, Add(Mul(ab, vb*denom), Mul(ac, vc*denom)));
}

// Signed distance of a closed mesh at every node: exact within `band`, the
// sign from the parity of crossings of a line along +x (offset by an
// irrational fraction of a cell in y and z so that it misses edges).
void MeshNodes(const Grid& g, const Shape& s, double band, std::vector<double>& out, Pool* pool)
{
	const auto nd = g.NodeDims();
	const std::size_t ntri = s.triangles.size()/3;
	auto vert = [&](std::uint32_t k) { return V3{{s.vertices[3*k], s.vertices[3*k + 1], s.vertices[3*k + 2]}}; };
	std::vector<std::array<int,6>> range(ntri);   // node index boxes (band-inflated)
	for (std::size_t t = 0; t < ntri; ++t) {
		V3 lo = vert(s.triangles[3*t]), hi = lo;
		for (int m = 1; m < 3; ++m) {
			const V3 p = vert(s.triangles[3*t + m]);
			for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
		}
		for (int a = 0; a < 3; ++a) {
			range[t][a] = std::max(0, static_cast<int>(std::floor((lo[a] - band - g.origin[a])/g.h)));
			range[t][3 + a] = std::min(nd[a] - 1, static_cast<int>(std::ceil((hi[a] + band - g.origin[a])/g.h)));
		}
	}
	std::vector<double> dist(g.Nodes(), band);
	// Unsigned distance, by node slab (each chunk owns its slab).
	ForRows(pool, std::size_t(nd[2]), 1, [&](std::size_t k0, std::size_t k1) {
		for (std::size_t t = 0; t < ntri; ++t) {
			const auto& r = range[t];
			if (r[2] > int(k1) - 1 || r[5] < int(k0)) continue;
			const V3 a = vert(s.triangles[3*t]), b = vert(s.triangles[3*t + 1]), c = vert(s.triangles[3*t + 2]);
			for (int k = std::max(r[2], int(k0)); k <= std::min(r[5], int(k1) - 1); ++k)
				for (int j = r[1]; j <= r[4]; ++j)
					for (int i = r[0]; i <= r[3]; ++i) {
						const V3 p = g.Node(i, j, k);
						const double d = NormV(Sub(p, ClosestOnTriangle(p, a, b, c)));
						double& slot = dist[g.N(i, j, k)];
						if (d < slot) slot = d;
					}
		}
	});
	// Parity along x lines through the nodes.
	const double oy = 0.0001234567*g.h, oz = 0.0000987654*g.h;
	std::vector<std::vector<std::uint32_t>> rows(std::size_t(nd[1])*std::size_t(nd[2]));
	for (std::size_t t = 0; t < ntri; ++t) {
		V3 lo = vert(s.triangles[3*t]), hi = lo;
		for (int m = 1; m < 3; ++m) {
			const V3 p = vert(s.triangles[3*t + m]);
			for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
		}
		const int j0 = std::max(0, static_cast<int>(std::floor((lo[1] - g.origin[1])/g.h)) - 1);
		const int j1 = std::min(nd[1] - 1, static_cast<int>(std::ceil((hi[1] - g.origin[1])/g.h)) + 1);
		const int k0 = std::max(0, static_cast<int>(std::floor((lo[2] - g.origin[2])/g.h)) - 1);
		const int k1 = std::min(nd[2] - 1, static_cast<int>(std::ceil((hi[2] - g.origin[2])/g.h)) + 1);
		for (int k = k0; k <= k1; ++k) for (int j = j0; j <= j1; ++j) rows[std::size_t(j) + std::size_t(nd[1])*std::size_t(k)].push_back(std::uint32_t(t));
	}
	ForRows(pool, rows.size(), 64, [&](std::size_t r0, std::size_t r1) {
		std::vector<double> xs;
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(nd[1])), k = int(r/std::size_t(nd[1]));
			const double y = g.origin[1] + j*g.h + oy, z = g.origin[2] + k*g.h + oz;
			xs.clear();
			for (std::uint32_t t : rows[r]) {
				const V3 a = vert(s.triangles[3*t]), b = vert(s.triangles[3*t + 1]), c = vert(s.triangles[3*t + 2]);
				// Barycentric coordinates of (y, z) in the triangle's yz projection.
				const double det = (b[1] - a[1])*(c[2] - a[2]) - (c[1] - a[1])*(b[2] - a[2]);
				if (det == 0) continue;
				const double u = ((y - a[1])*(c[2] - a[2]) - (c[1] - a[1])*(z - a[2]))/det;
				const double v = ((b[1] - a[1])*(z - a[2]) - (y - a[1])*(b[2] - a[2]))/det;
				if (u < 0 || v < 0 || u + v > 1) continue;
				xs.push_back(a[0] + u*(b[0] - a[0]) + v*(c[0] - a[0]));
			}
			std::sort(xs.begin(), xs.end());
			std::size_t m = 0;
			for (int i = 0; i < nd[0]; ++i) {
				const double x = g.origin[0] + i*g.h;
				while (m < xs.size() && xs[m] <= x) ++m;
				const bool inside = ((xs.size() - m) & 1) != 0;
				const std::size_t id = g.N(i, j, k);
				out[id] = inside ? -dist[id] : dist[id];
			}
		}
	});
}
} // namespace

// ------------------------------------------------------------------ MAC sampling & reductions

double Dot(Pool* pool, const std::vector<double>& a, const std::vector<double>& b)
{
	const std::size_t n = a.size();
	return SumRows(pool, n, 4096, [&](std::size_t i0, std::size_t i1) {
		double s = 0;
		for (std::size_t i = i0; i < i1; ++i) s += a[i]*b[i];
		return s;
	});
}

// Trilinear interpolation of a cell-centred field (half-cell offset on all axes).
double SampleCells(const Grid& g, const std::vector<double>& f, const V3& x)
{
	V3 s;
	for (int a = 0; a < 3; ++a) s[a] = (x[a] - g.origin[a])/g.h - 0.5;
	return Trilinear(f, g.n, s);
}

// Trilinear interpolation of an axis-a MAC face field (node-aligned on a, centred on b, c).
double SampleFace(const Grid& g, int a, const std::vector<double>& f, const V3& x)
{
	V3 s;
	for (int b = 0; b < 3; ++b) s[b] = (x[b] - g.origin[b])/g.h - (b == a ? 0.0 : 0.5);
	return Trilinear(f, g.FaceDims(a), s);
}

V3 SampleVelocity(const Grid& g, const MacField& u, const V3& x)
{
	return {{SampleFace(g, 0, u.u[0], x), SampleFace(g, 1, u.u[1], x), SampleFace(g, 2, u.u[2], x)}};
}

// Averages opposite MAC faces to produce collocated cell-centred velocities.
void CellVelocity(const Grid& g, const MacField& u, std::array<std::vector<double>,3>& c, Pool* pool)
{
	for (int a = 0; a < 3; ++a) c[a].resize(g.Cells());
	const std::size_t rows = std::size_t(g.n[1])*std::size_t(g.n[2]);
	ForRows(pool, rows, RowGrain(g.n[0]), [&](std::size_t r0, std::size_t r1) {
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(g.n[1])), k = int(r/std::size_t(g.n[1]));
			for (int i = 0; i < g.n[0]; ++i) {
				const std::size_t id = g.C(i, j, k);
				c[0][id] = 0.5*(u.u[0][g.F(0, i, j, k)] + u.u[0][g.F(0, i + 1, j, k)]);
				c[1][id] = 0.5*(u.u[1][g.F(1, i, j, k)] + u.u[1][g.F(1, i, j + 1, k)]);
				c[2][id] = 0.5*(u.u[2][g.F(2, i, j, k)] + u.u[2][g.F(2, i, j, k + 1)]);
			}
		}
	});
}

// ------------------------------------------------------------------ shapes & cut-cell fractions

// Exact Euclidean signed distance (< 0 inside) for analytic shapes; `invert` flips interior/exterior.
double Shape::Distance(const V3& x) const
{
	if (invert) {
		Shape s = *this;
		s.invert = false;
		return -s.Distance(x);
	}
	switch (type) {
	case Type::Plane: return DotV(Sub(x, point), axis);
	case Type::Sphere: return NormV(Sub(x, point)) - radius;
	case Type::Box: {
		V3 q;
		double outside = 0, inside = -1e300;
		for (int a = 0; a < 3; ++a) {
			const double c = 0.5*(lo[a] + hi[a]), e = 0.5*(hi[a] - lo[a]);
			q[a] = std::fabs(x[a] - c) - e;
			outside += std::max(q[a], 0.0)*std::max(q[a], 0.0);
			inside = std::max(inside, q[a]);
		}
		return std::sqrt(outside) + std::min(inside, 0.0);
	}
	case Type::Cylinder: {
		const V3 d = Sub(x, point);
		const double along = DotV(d, axis);
		const double radial = NormV(Sub(d, Mul(axis, along)));
		const double qr = radial - radius, qa = std::fabs(along) - 0.5*height;
		const double out = std::sqrt(std::max(qr, 0.0)*std::max(qr, 0.0) + std::max(qa, 0.0)*std::max(qa, 0.0));
		return out + std::min(std::max(qr, qa), 0.0);
	}
	case Type::Mesh: break;
	}
	return 1e300;
}

// Evaluates the union (min) of solid shape SDFs at grid nodes, clamped to [-band, band].
void SolidNodes(const Grid& g, const std::vector<Shape>& shapes, double band, std::vector<double>& nodes, Pool* pool)
{
	nodes.assign(g.Nodes(), band);
	const auto nd = g.NodeDims();
	std::vector<double> mesh;
	for (const Shape& s : shapes) {
		if (s.type == Shape::Type::Mesh) {
			mesh.assign(g.Nodes(), band);
			MeshNodes(g, s, band, mesh, pool);
		}
		ForRows(pool, std::size_t(nd[1])*std::size_t(nd[2]), RowGrain(nd[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(nd[1])), k = int(r/std::size_t(nd[1]));
				for (int i = 0; i < nd[0]; ++i) {
					const std::size_t id = g.N(i, j, k);
					const double d = s.type == Shape::Type::Mesh ? mesh[id] : Clamp(s.Distance(g.Node(i, j, k)), -band, band);
					nodes[id] = std::min(nodes[id], d);
				}
			}
		});
	}
}

// Exact area fraction where a linear triangle with vertex SDFs (a, b, c) is positive (open).
double TrianglePositiveFraction(double a, double b, double c)
{
	const bool pa = a > 0, pb = b > 0, pc = c > 0;
	const int positives = int(pa) + int(pb) + int(pc);
	if (positives == 3) return 1;
	if (positives == 0) return 0;
	if (positives == 1) {
		const double p = pa ? a : pb ? b : c;
		const double n1 = pa ? b : a, n2 = pc ? b : c;
		return p*p/((p - n1)*(p - n2));
	}
	const double n = !pa ? a : !pb ? b : c;
	const double p1 = !pa ? b : a, p2 = !pc ? b : c;
	return 1 - n*n/((n - p1)*(n - p2));
}

// Computes cut-cell open face fraction in [0, 1] by subdividing the face quad into four
// centre-fan triangles; domain walls are closed except an optional openTop boundary.
double FaceFraction(const Grid& g, const std::vector<double>& nodes, int a, int i, int j, int k, bool openTop, int openAxis)
{
	const int b = (a + 1) % 3, c = (a + 2) % 3;
	const int idx[3] = {i, j, k};
	const int along = idx[a];
	if (along == 0 || along == g.n[a]) {
		if (!(openTop && a == openAxis && along == g.n[a])) return 0.0;
	}
	double v[4];
	for (int m = 0; m < 4; ++m) {
		int q[3] = {idx[0], idx[1], idx[2]};
		if (m == 1 || m == 2) ++q[b];
		if (m == 2 || m == 3) ++q[c];
		v[m] = nodes[g.N(q[0], q[1], q[2])];
		// A solid surface through a node (a box face on a grid plane) is
		// exact there: round-off of either sign must not open a sliver.
		if (std::fabs(v[m]) < 1e-9*g.h) v[m] = 0;
	}
	// Thin (one-cell) dimensions: the face spans the whole extent.
	const double centre = 0.25*(v[0] + v[1] + v[2] + v[3]);
	double f = 0;
	for (int m = 0; m < 4; ++m) f += TrianglePositiveFraction(centre, v[m], v[(m + 1) % 4]);
	return 0.25*f;
}

void FaceFractions(const Grid& g, const std::vector<double>& nodes, bool openTop, int openAxis,
	std::array<std::vector<double>,3>& fraction, Pool* pool)
{
	for (int a = 0; a < 3; ++a) {
		const auto d = g.FaceDims(a);
		fraction[a].assign(g.Faces(a), 0.0);
		ForRows(pool, std::size_t(d[1])*std::size_t(d[2]), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
				for (int i = 0; i < d[0]; ++i) fraction[a][g.F(a, i, j, k)] = FaceFraction(g, nodes, a, i, j, k, openTop, openAxis);
			}
		});
	}
}

// Linear open-volume estimate of cell (i, j, k) from the mean of its 8 corner solid SDFs.
double CellOpenFraction(const Grid& g, const std::vector<double>& nodes, int i, int j, int k)
{
	double s = 0;
	for (int m = 0; m < 8; ++m) s += nodes[g.N(i + (m & 1), j + ((m >> 1) & 1), k + ((m >> 2) & 1))];
	return Clamp(0.5 + 0.125*s/g.h, 0.0, 1.0);
}

void CellOpenFractions(const Grid& g, const std::vector<double>& nodes, std::vector<double>& open, Pool* pool)
{
	open.assign(g.Cells(), 1.0);
	ForRows(pool, std::size_t(g.n[1])*std::size_t(g.n[2]), RowGrain(g.n[0]), [&](std::size_t r0, std::size_t r1) {
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(g.n[1])), k = int(r/std::size_t(g.n[1]));
			for (int i = 0; i < g.n[0]; ++i) open[g.C(i, j, k)] = CellOpenFraction(g, nodes, i, j, k);
		}
	});
}

// Initializes cell-centred liquid level set phi as the union of initial liquid shapes.
void LiquidCells(const Grid& g, const std::vector<Shape>& regions, std::vector<double>& phi, Pool* pool)
{
	phi.assign(g.Cells(), 1e300);
	ForRows(pool, std::size_t(g.n[1])*std::size_t(g.n[2]), RowGrain(g.n[0]), [&](std::size_t r0, std::size_t r1) {
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(g.n[1])), k = int(r/std::size_t(g.n[1]));
			for (int i = 0; i < g.n[0]; ++i) {
				double d = 1e300;
				for (const Shape& s : regions) d = std::min(d, s.Distance(g.CellCentre(i, j, k)));
				phi[g.C(i, j, k)] = d;
			}
		}
	});
	// No region: all air, a positive distance bounded by the domain.
	const double farDistance = g.h*double(g.n[0] + g.n[1] + g.n[2]);  // not `far`: an MSVC <windows.h> macro
	for (double& x : phi) x = Clamp(x, -farDistance, farDistance);
}

} } }
