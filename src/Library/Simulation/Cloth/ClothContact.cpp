//////////////////////////////////////////////////////////////////////
//
//  ClothContact.cpp - Barrier contact, CCD and friction for the cloth
//  solver (formulas: ClothContact.h)
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClothContact.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace RISE { namespace Simulation { namespace Cloth {

namespace {
constexpr double Infinity = std::numeric_limits<double>::infinity();

inline V3 Add(const V3& a, const V3& b) { return {{a[0]+b[0], a[1]+b[1], a[2]+b[2]}}; }
inline V3 Sub(const V3& a, const V3& b) { return {{a[0]-b[0], a[1]-b[1], a[2]-b[2]}}; }
inline V3 Mul(const V3& a, double s) { return {{a[0]*s, a[1]*s, a[2]*s}}; }
inline double Dot(const V3& a, const V3& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
inline V3 Cross(const V3& a, const V3& b)
{
	return {{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}};
}
inline double Norm(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 At(const std::vector<double>& x, std::uint32_t i)
{
	const std::size_t k = 3*static_cast<std::size_t>(i);
	return {{x[k], x[k+1], x[k+2]}};
}

// ------------------------------------------------------------------ distance pieces

// A piece of a primitive distance: r = sum_i c_i(w) x_{idx_i} with c affine
// in the closest-point parameters w (m of them, at their minimising values).
struct Piece {
	int n = 0, m = 0;
	int idx[4] = {0, 0, 0, 0};
	double c0[4] = {0, 0, 0, 0};
	double cw[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
	double w[2] = {0, 0};
	double d2 = Infinity;
};

Piece PointPoint(const V3* xs, int a, int b)
{
	Piece p;
	p.n = 2; p.m = 0;
	p.idx[0] = a; p.idx[1] = b;
	p.c0[0] = 1; p.c0[1] = -1;
	const V3 r = Sub(xs[a], xs[b]);
	p.d2 = Dot(r, r);
	return p;
}

// Point a against segment (b, c): interior piece or the nearer endpoint.
Piece PointEdge(const V3* xs, int a, int b, int c)
{
	const V3 e = Sub(xs[c], xs[b]);
	const double ee = Dot(e, e);
	const double s = ee > 0 ? Dot(Sub(xs[a], xs[b]), e)/ee : 0;
	if (!(s > 0)) return PointPoint(xs, a, b);
	if (!(s < 1)) return PointPoint(xs, a, c);
	Piece p;
	p.n = 3; p.m = 1;
	p.idx[0] = a; p.idx[1] = b; p.idx[2] = c;
	p.c0[0] = 1; p.c0[1] = -1; p.c0[2] = 0;
	p.cw[1][0] = 1; p.cw[2][0] = -1;
	p.w[0] = s;
	const V3 r = Sub(Sub(xs[a], xs[b]), Mul(e, s));
	p.d2 = Dot(r, r);
	return p;
}

// Point 0 against triangle (1, 2, 3).
Piece PointTriangle(const V3* xs)
{
	const V3 e1 = Sub(xs[2], xs[1]), e2 = Sub(xs[3], xs[1]), q = Sub(xs[0], xs[1]);
	const double a = Dot(e1, e1), b = Dot(e1, e2), c = Dot(e2, e2), d = Dot(e1, q), e = Dot(e2, q);
	const double det = a*c - b*b;
	if (det > 1e-14*a*c) {
		const double u = (c*d - b*e)/det, v = (a*e - b*d)/det;
		if (u > 0 && v > 0 && u + v < 1) {
			Piece p;
			p.n = 4; p.m = 2;
			for (int k = 0; k < 4; ++k) p.idx[k] = k;
			p.c0[0] = 1; p.c0[1] = -1; p.c0[2] = 0; p.c0[3] = 0;
			p.cw[1][0] = 1; p.cw[1][1] = 1; p.cw[2][0] = -1; p.cw[3][1] = -1;
			p.w[0] = u; p.w[1] = v;
			const V3 r = Sub(Sub(q, Mul(e1, u)), Mul(e2, v));
			p.d2 = Dot(r, r);
			return p;
		}
	}
	Piece best = PointEdge(xs, 0, 1, 2);
	const Piece p2 = PointEdge(xs, 0, 2, 3), p3 = PointEdge(xs, 0, 3, 1);
	if (p2.d2 < best.d2) best = p2;
	if (p3.d2 < best.d2) best = p3;
	return best;
}

// Segments (0, 1) and (2, 3).  Nearly parallel segments (sin^2 of the angle
// below `parallel`) use the point-edge pieces: their interior piece's Hessian
// grows like 1/sin^2.  The distance is then overestimated by at most about
// L sin/2 (1e-3 L with the default), far below the thickness offset.
Piece EdgeEdge(const V3* xs, double parallel = 1e-6)
{
	const V3 da = Sub(xs[1], xs[0]), db = Sub(xs[3], xs[2]), r0 = Sub(xs[0], xs[2]);
	const double a = Dot(da, da), b = Dot(da, db), c = Dot(db, db), d = Dot(da, r0), e = Dot(db, r0);
	const double det = a*c - b*b;
	Piece best;
	if (det > parallel*a*c) {
		const double s = (b*e - c*d)/det, t = (a*e - b*d)/det;
		if (s > 0 && s < 1 && t > 0 && t < 1) {
			Piece p;
			p.n = 4; p.m = 2;
			for (int k = 0; k < 4; ++k) p.idx[k] = k;
			p.c0[0] = 1; p.c0[1] = 0; p.c0[2] = -1; p.c0[3] = 0;
			p.cw[0][0] = -1; p.cw[1][0] = 1; p.cw[2][1] = 1; p.cw[3][1] = -1;
			p.w[0] = s; p.w[1] = t;
			const V3 r = Sub(Add(r0, Mul(da, s)), Mul(db, t));
			p.d2 = Dot(r, r);
			best = p;
		}
	}
	const Piece cand[4] = {PointEdge(xs, 0, 2, 3), PointEdge(xs, 1, 2, 3), PointEdge(xs, 2, 0, 1), PointEdge(xs, 3, 0, 1)};
	for (const Piece& p : cand) if (p.d2 < best.d2) best = p;
	return best;
}

// Gradient (3N) and Hessian (3N x 3N) of the piece's squared distance over the
// primitive's N points: the joint Hessian in (x, w) reduced by its Schur
// complement over w (the minimiser).
void PieceDerivatives(const Piece& p, const V3* xs, int N, double* g, double* h)
{
	double c[4];
	V3 r{{0, 0, 0}}, R[2] = {{{0, 0, 0}}, {{0, 0, 0}}};
	for (int i = 0; i < p.n; ++i) {
		c[i] = p.c0[i];
		for (int k = 0; k < p.m; ++k) c[i] += p.cw[i][k]*p.w[k];
		r = Add(r, Mul(xs[p.idx[i]], c[i]));
		for (int k = 0; k < p.m; ++k) R[k] = Add(R[k], Mul(xs[p.idx[i]], p.cw[i][k]));
	}
	const int D = 3*N;
	if (g) {
		std::fill(g, g + D, 0.0);
		for (int i = 0; i < p.n; ++i) for (int a = 0; a < 3; ++a) g[3*p.idx[i] + a] += 2*c[i]*r[a];
	}
	if (!h) return;
	std::fill(h, h + D*D, 0.0);
	// Hxw(i,k) = 2 (c_i R_k + cw_ik r); Hww(k,l) = 2 R_k.R_l.
	double hxw[4][2][3];
	for (int i = 0; i < p.n; ++i) for (int k = 0; k < p.m; ++k) for (int a = 0; a < 3; ++a)
		hxw[i][k][a] = 2*(c[i]*R[k][a] + p.cw[i][k]*r[a]);
	double winv[2][2] = {{0, 0}, {0, 0}};
	bool reduce = p.m > 0;
	if (p.m == 1) {
		const double hww = 2*Dot(R[0], R[0]);
		if (hww > 0) winv[0][0] = 1/hww; else reduce = false;
	} else if (p.m == 2) {
		const double h00 = 2*Dot(R[0], R[0]), h01 = 2*Dot(R[0], R[1]), h11 = 2*Dot(R[1], R[1]);
		const double det = h00*h11 - h01*h01;
		if (det > 1e-14*h00*h11 && det > 0) {
			winv[0][0] = h11/det; winv[0][1] = -h01/det; winv[1][0] = -h01/det; winv[1][1] = h00/det;
		} else reduce = false;
	}
	for (int i = 0; i < p.n; ++i) for (int j = 0; j < p.n; ++j) for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
		double v = a == b ? 2*c[i]*c[j] : 0.0;
		if (reduce)
			for (int k = 0; k < p.m; ++k) for (int l = 0; l < p.m; ++l) v -= hxw[i][k][a]*winv[k][l]*hxw[j][l][b];
		h[(3*p.idx[i] + a)*D + 3*p.idx[j] + b] += v;
	}
}

// Symmetric n x n (row-major) replaced by its positive semidefinite part
// (cyclic Jacobi, fixed sweep order).
void ProjectPSD(double* a, int n)
{
	double v[144], d[12];
	for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) v[i*n + j] = i == j ? 1.0 : 0.0;
	double scale = 0;
	for (int i = 0; i < n*n; ++i) scale = std::max(scale, std::fabs(a[i]));
	if (!(scale > 0)) return;
	for (int sweep = 0; sweep < 50; ++sweep) {
		double off = 0;
		for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j) off += a[i*n + j]*a[i*n + j];
		if (off <= 1e-30*scale*scale) break;
		for (int p = 0; p < n; ++p) for (int q = p + 1; q < n; ++q) {
			const double apq = a[p*n + q];
			if (std::fabs(apq) <= 1e-300) continue;
			const double theta = (a[q*n + q] - a[p*n + p])/(2*apq);
			const double t = (theta >= 0 ? 1.0 : -1.0)/(std::fabs(theta) + std::sqrt(theta*theta + 1));
			const double cs = 1/std::sqrt(t*t + 1), sn = t*cs;
			for (int k = 0; k < n; ++k) {
				const double akp = a[k*n + p], akq = a[k*n + q];
				a[k*n + p] = cs*akp - sn*akq;
				a[k*n + q] = sn*akp + cs*akq;
			}
			for (int k = 0; k < n; ++k) {
				const double apk = a[p*n + k], aqk = a[q*n + k];
				a[p*n + k] = cs*apk - sn*aqk;
				a[q*n + k] = sn*apk + cs*aqk;
			}
			for (int k = 0; k < n; ++k) {
				const double vkp = v[k*n + p], vkq = v[k*n + q];
				v[k*n + p] = cs*vkp - sn*vkq;
				v[k*n + q] = sn*vkp + cs*vkq;
			}
		}
	}
	bool negative = false;
	for (int i = 0; i < n; ++i) { d[i] = a[i*n + i]; if (d[i] < 0) { negative = true; d[i] = 0; } }
	(void)negative;
	for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) {
		double s = 0;
		for (int k = 0; k < n; ++k) s += v[i*n + k]*d[k]*v[j*n + k];
		a[i*n + j] = s;
	}
}

double PairDistance2(int kind, const V3* xs)
{
	return (kind == 0 ? PointTriangle(xs) : EdgeEdge(xs)).d2;
}

// The exact segment distance (interior piece down to round-off parallelism), for the crossing test.
double ExactDistance2(int kind, const V3* xs)
{
	return (kind == 0 ? PointTriangle(xs) : EdgeEdge(xs, 1e-14)).d2;
}

// Smoothed friction: f1 and its antiderivative F0 (F0(0) = eps/3) at speed y.
void FrictionCurve(double y, double eps, double& f0, double& f1, double& df1)
{
	if (y < eps) {
		f1 = 2*y/eps - y*y/(eps*eps);
		df1 = 2/eps - 2*y/(eps*eps);
		f0 = y*y/eps - y*y*y/(3*eps*eps) + eps/3;
	} else { f1 = 1; df1 = 0; f0 = y; }
}

inline std::uint64_t CellKey(std::int64_t i, std::int64_t j, std::int64_t k)
{
	const std::uint64_t m = (std::uint64_t(1) << 21) - 1;
	return ((static_cast<std::uint64_t>(i) & m) << 42) | ((static_cast<std::uint64_t>(j) & m) << 21) | (static_cast<std::uint64_t>(k) & m);
}

struct Box {
	V3 lo{{Infinity, Infinity, Infinity}}, hi{{-Infinity, -Infinity, -Infinity}};
	void Grow(const V3& p) { for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); } }
	void Inflate(double r) { for (int k = 0; k < 3; ++k) { lo[k] -= r; hi[k] += r; } }
	bool Overlaps(const double* o) const {
		for (int k = 0; k < 3; ++k) if (hi[k] < o[k] || lo[k] > o[3 + k]) return false;
		return true;
	}
};

// Cell index ranges of a box; false when it covers more than 4096 cells (or is not finite).
bool CellRange(const Box& b, double cell, std::int64_t* lo, std::int64_t* hi)
{
	double count = 1;
	for (int k = 0; k < 3; ++k) {
		if (!std::isfinite(b.lo[k]) || !std::isfinite(b.hi[k])) return false;
		const double l = std::floor(b.lo[k]/cell), h = std::floor(b.hi[k]/cell);
		count *= h - l + 1;
		if (!(count <= 4096)) return false;
		lo[k] = static_cast<std::int64_t>(l); hi[k] = static_cast<std::int64_t>(h);
	}
	return true;
}

// Calls f(id) for every id stored in the cells the box covers and in the
// oversize list (ids may repeat); false (and no calls) when the box covers too
// many cells, so the caller scans everything.
template <class F>
bool ForCells(const HashGrid& grid, double cell, const Box& b, F&& f)
{
	std::int64_t lo[3], hi[3];
	if (!CellRange(b, cell, lo, hi)) return false;
	for (std::uint32_t id : grid.oversize) f(id);
	for (std::int64_t i = lo[0]; i <= hi[0]; ++i) for (std::int64_t j = lo[1]; j <= hi[1]; ++j) for (std::int64_t k = lo[2]; k <= hi[2]; ++k) {
		const auto it = grid.cells.find(CellKey(i, j, k));
		if (it == grid.cells.end()) continue;
		for (std::uint32_t id : it->second) f(id);
	}
	return true;
}

void Insert(HashGrid& grid, double cell, const Box& b, std::uint32_t id)
{
	std::int64_t lo[3], hi[3];
	if (!CellRange(b, cell, lo, hi)) { grid.oversize.push_back(id); return; }
	for (std::int64_t i = lo[0]; i <= hi[0]; ++i) for (std::int64_t j = lo[1]; j <= hi[1]; ++j) for (std::int64_t k = lo[2]; k <= hi[2]; ++k)
		grid.cells[CellKey(i, j, k)].push_back(id);
}

// Conservative advancement of a point against a 1-Lipschitz signed distance.
double ShapeCCD(const Collider& c, const V3& x, const V3& dx, double xi, double s)
{
	const double lp = Norm(dx);
	if (lp == 0) return 1;
	V3 grad;
	const double gap0 = c.Distance(x, grad) - xi;
	if (!(gap0 > 0)) return 0;
	const double g = s*gap0;
	double t = 0, tl = (1 - s)*gap0/lp;
	for (int it = 0; it < 100000; ++it) {
		const double gap = c.Distance(Add(x, Mul(dx, t + tl)), grad) - xi;
		if (t > 0 && gap < g) return t;
		t += tl;
		if (t > 1) return 1;
		tl = 0.9*gap/lp;
	}
	return t;
}

// Coefficients of the triple product (b(t) x c(t)) . q(t) for b, c, q linear in t.
void TripleCubic(const V3& b0, const V3& b1, const V3& c0, const V3& c1, const V3& q0, const V3& q1, double* k)
{
	k[0] = Dot(Cross(b0, c0), q0);
	k[1] = Dot(Cross(b1, c0), q0) + Dot(Cross(b0, c1), q0) + Dot(Cross(b0, c0), q1);
	k[2] = Dot(Cross(b1, c1), q0) + Dot(Cross(b1, c0), q1) + Dot(Cross(b0, c1), q1);
	k[3] = Dot(Cross(b1, c1), q1);
}

// Real roots in [0, 1] of k0 + k1 t + k2 t^2 + k3 t^3: bisection on the
// monotone pieces between the derivative's roots.
int CubicRoots(const double* k, double* roots)
{
	auto f = [&](double t) { return ((k[3]*t + k[2])*t + k[1])*t + k[0]; };
	double cuts[4] = {0, 0, 0, 1};
	int nc = 1;
	// f' = k1 + 2 k2 t + 3 k3 t^2.
	const double A = 3*k[3], B = 2*k[2], C = k[1];
	double crit[2];
	int ncrit = 0;
	if (std::fabs(A) > 1e-300) {
		const double disc = B*B - 4*A*C;
		if (disc >= 0) {
			const double sq = std::sqrt(disc);
			const double q = -0.5*(B + (B >= 0 ? sq : -sq));
			const double r1 = q/A, r2 = q != 0 ? C/q : r1;
			crit[ncrit++] = std::min(r1, r2);
			crit[ncrit++] = std::max(r1, r2);
		}
	} else if (std::fabs(B) > 1e-300) crit[ncrit++] = -C/B;
	for (int i = 0; i < ncrit; ++i) if (crit[i] > 0 && crit[i] < 1) cuts[nc++] = crit[i];
	cuts[nc++] = 1;
	int n = 0;
	for (int i = 0; i + 1 < nc; ++i) {
		double lo = cuts[i], hi = cuts[i+1];
		double flo = f(lo), fhi = f(hi);
		if (flo == 0) { roots[n++] = lo; continue; }
		if ((flo > 0) == (fhi > 0) && fhi != 0) continue;
		for (int it = 0; it < 80; ++it) {
			const double mid = 0.5*(lo + hi), fm = f(mid);
			if ((fm > 0) == (flo > 0) && fm != 0) { lo = mid; flo = fm; } else hi = mid;
		}
		roots[n++] = hi;
	}
	return n;
}
} // namespace

// ------------------------------------------------------------------ public kernels

double Barrier(double z, double dhat, double* first, double* second)
{
	if (!(z < dhat)) { if (first) *first = 0; if (second) *second = 0; return 0; }
	const double l = std::log(z/dhat), u = z - dhat;
	if (first) *first = -2*u*l - u*u/z;
	if (second) *second = -2*l - 4*u/z + u*u/(z*z);
	return -u*u*l;
}

double PointTriangleDistance2(const V3& p, const V3& t0, const V3& t1, const V3& t2, double* g, double* h)
{
	const V3 xs[4] = {p, t0, t1, t2};
	const Piece piece = PointTriangle(xs);
	if (g || h) PieceDerivatives(piece, xs, 4, g, h);
	return piece.d2;
}

double EdgeEdgeDistance2(const V3& a0, const V3& a1, const V3& b0, const V3& b1, double* g, double* h)
{
	const V3 xs[4] = {a0, a1, b0, b1};
	const Piece piece = EdgeEdge(xs);
	if (g || h) PieceDerivatives(piece, xs, 4, g, h);
	return piece.d2;
}

double AdditiveCCD(int kind, const V3* x, const V3* dx, double xi, double s)
{
	V3 p[4], xs[4];
	V3 mean{{0, 0, 0}};
	for (int k = 0; k < 4; ++k) mean = Add(mean, dx[k]);
	mean = Mul(mean, 0.25);
	for (int k = 0; k < 4; ++k) { p[k] = Sub(dx[k], mean); xs[k] = x[k]; }
	const double lp = kind == 0 ?
		Norm(p[0]) + std::max(Norm(p[1]), std::max(Norm(p[2]), Norm(p[3]))) :
		std::max(Norm(p[0]), Norm(p[1])) + std::max(Norm(p[2]), Norm(p[3]));
	if (lp == 0) return 1;
	const double gap0 = std::sqrt(PairDistance2(kind, xs)) - xi;
	if (!(gap0 > 0)) return 0;
	const double g = s*gap0;
	double t = 0, tl = (1 - s)*gap0/lp;
	for (int it = 0; it < 100000; ++it) {
		for (int k = 0; k < 4; ++k) xs[k] = Add(x[k], Mul(p[k], t + tl));
		const double gap = std::sqrt(PairDistance2(kind, xs)) - xi;
		if (t > 0 && gap < g) return t;
		t += tl;
		if (t > 1) return 1;
		tl = 0.9*gap/lp;
	}
	return t;
}

bool CrossesDuringStep(int kind, const V3* x0, const V3* x1)
{
	V3 d[4];
	double scale = 0;
	for (int k = 0; k < 4; ++k) {
		d[k] = Sub(x1[k], x0[k]);
		for (int c = 0; c < 3; ++c) scale = std::max(scale, std::max(std::fabs(x0[k][c]), std::fabs(x1[k][c])));
	}
	const double tol = 1e-9*std::max(scale, 1e-3);
	auto at = [&](double t, V3* xs) { for (int k = 0; k < 4; ++k) xs[k] = Add(x0[k], Mul(d[k], t)); };
	auto touching = [&](double t) {
		V3 xs[4];
		at(t, xs);
		return std::sqrt(ExactDistance2(kind, xs)) <= tol;
	};
	if (touching(0) || touching(1)) return true;
	double k[4];
	if (kind == 0) // (t1 - t0) x (t2 - t0) . (p - t0)
		TripleCubic(Sub(x0[2], x0[1]), Sub(d[2], d[1]), Sub(x0[3], x0[1]), Sub(d[3], d[1]), Sub(x0[0], x0[1]), Sub(d[0], d[1]), k);
	else           // (a1 - a0) x (b1 - b0) . (b0 - a0)
		TripleCubic(Sub(x0[1], x0[0]), Sub(d[1], d[0]), Sub(x0[3], x0[2]), Sub(d[3], d[2]), Sub(x0[2], x0[0]), Sub(d[2], d[0]), k);
	double mag = 0;
	for (double c : k) mag = std::max(mag, std::fabs(c));
	if (!(mag > 1e-300)) { // coplanar throughout: sample
		for (int i = 1; i < 256; ++i) if (touching(i/256.0)) return true;
		return false;
	}
	double roots[4];
	const int n = CubicRoots(k, roots);
	for (int i = 0; i < n; ++i) if (touching(roots[i])) return true;
	// A grazing touch is a double root without a sign change: check the cubic's critical points.
	const double A = 3*k[3], B = 2*k[2], C = k[1];
	if (std::fabs(A) > 1e-300) {
		const double disc = B*B - 4*A*C;
		if (disc >= 0) {
			const double sq = std::sqrt(disc);
			for (double r : {(-B - sq)/(2*A), (-B + sq)/(2*A)}) if (r > 0 && r < 1 && touching(r)) return true;
		}
	} else if (std::fabs(B) > 1e-300) {
		const double r = -C/B;
		if (r > 0 && r < 1 && touching(r)) return true;
	}
	return false;
}

// ------------------------------------------------------------------ BarrierContact

bool BarrierContact::Initialize(const Sheet& sheet, const ModelConfig& config, std::string& error)
{
	*this = BarrierContact();
	settings_ = config.barrier;
	xi_ = config.contactThickness;
	shapes_ = config.colliders;
	vertices_ = sheet.Vertices();
	if (!(settings_.stiffness > 0) || !(settings_.activation > 0) || !(xi_ >= 0) || !(settings_.friction >= 0) ||
		!(settings_.frictionVelocity > 0)) {
		error = "barrier contact needs positive stiffness, activation and slip speed, and nonnegative thickness and friction";
		return false;
	}
	vertexWeight_ = sheet.area;
	std::map<std::pair<std::uint32_t,std::uint32_t>, double> edgeArea;
	for (const auto& t : sheet.triangles) {
		triangles_.insert(triangles_.end(), {t.v[0], t.v[1], t.v[2]});
		triangleWeight_.push_back(t.area/3);
		for (int k = 0; k < 3; ++k) {
			const std::uint32_t a = t.v[k], b = t.v[(k + 1) % 3];
			edgeArea[{std::min(a, b), std::max(a, b)}] += t.area/3;
		}
	}
	for (const auto& e : edgeArea) { edges_.push_back(e.first.first); edges_.push_back(e.first.second); edgeWeight_.push_back(e.second); }
	double edgeSum = 0;
	std::size_t edgeCount = 0;
	for (std::size_t m = 0; m < config.meshColliders.size(); ++m) {
		const MeshCollider& mc = config.meshColliders[m];
		if (mc.vertices.size() % 3 != 0 || mc.triangles.size() % 3 != 0 || mc.triangles.empty()) {
			error = "mesh collider needs 3 coordinates per vertex and 3 indices per triangle";
			return false;
		}
		const std::uint32_t base = static_cast<std::uint32_t>(colliderV0_.size()/3);
		const std::uint32_t count = static_cast<std::uint32_t>(mc.vertices.size()/3);
		for (double c : mc.vertices) if (!std::isfinite(c)) { error = "mesh collider vertices must be finite"; return false; }
		colliderV0_.insert(colliderV0_.end(), mc.vertices.begin(), mc.vertices.end());
		colliderMesh_.insert(colliderMesh_.end(), count, static_cast<std::uint32_t>(m));
		std::map<std::pair<std::uint32_t,std::uint32_t>, int> edges;
		for (std::size_t t = 0; t < mc.triangles.size(); t += 3) {
			for (int k = 0; k < 3; ++k) if (mc.triangles[t + k] >= count) { error = "mesh collider index out of range"; return false; }
			for (int k = 0; k < 3; ++k) colliderTriangles_.push_back(base + mc.triangles[t + k]);
			for (int k = 0; k < 3; ++k) {
				const std::uint32_t a = base + mc.triangles[t + k], b = base + mc.triangles[t + (k + 1) % 3];
				edges[{std::min(a, b), std::max(a, b)}] = 1;
			}
		}
		for (const auto& e : edges) {
			colliderEdges_.push_back(e.first.first); colliderEdges_.push_back(e.first.second);
			edgeSum += Norm(Sub(At(colliderV0_, e.first.first), At(colliderV0_, e.first.second)));
			++edgeCount;
		}
		motions_.push_back(mc.motion);
		moving_ = moving_ || mc.motion.Moving();
	}
	meanEdge_ = edgeCount ? edgeSum/static_cast<double>(edgeCount) : 0;
	// Non-adjacent primitives of the flat sheet are h/sqrt(2) apart (a vertex and
	// the far diagonal of its cell); the self-contact reach must stay below that.
	if (settings_.selfCollision && !(xi_ + settings_.activation < 0.5*std::min(sheet.hu, sheet.hv))) {
		error = "self-collision needs contact_thickness + contact_activation below half the cell size";
		return false;
	}
	active_ = !shapes_.empty() || !colliderV0_.empty() || settings_.selfCollision;
	return true;
}

void BarrierContact::Place(double t, ContactFrame& f, double t0) const
{
	f.time = t;
	const std::size_t nv = colliderV0_.size()/3;
	f.colliderX.resize(colliderV0_.size());
	f.colliderV.resize(colliderV0_.size());
	for (std::size_t i = 0; i < nv; ++i) {
		const Motion& m = motions_[colliderMesh_[i]];
		const V3 rest = At(colliderV0_, static_cast<std::uint32_t>(i));
		const V3 p = m.Position(rest, t);
		const V3 v = t0 < t ? Mul(Sub(p, m.Position(rest, t0)), 1/(t - t0)) : m.Velocity(rest, t);
		for (int k = 0; k < 3; ++k) { f.colliderX[3*i + k] = p[k]; f.colliderV[3*i + k] = v[k]; }
	}
	f.cell = std::max(meanEdge_, 2*(xi_ + settings_.activation));
	f.gridTriangles.Clear(); f.gridEdges.Clear(); f.gridVertices.Clear();
	const std::size_t nt = colliderTriangles_.size()/3, ne = colliderEdges_.size()/2;
	f.triangleBox.assign(6*nt, 0.0);
	f.edgeBox.assign(6*ne, 0.0);
	for (std::size_t t3 = 0; t3 < nt; ++t3) {
		Box b;
		for (int k = 0; k < 3; ++k) b.Grow(At(f.colliderX, colliderTriangles_[3*t3 + k]));
		for (int k = 0; k < 3; ++k) { f.triangleBox[6*t3 + k] = b.lo[k]; f.triangleBox[6*t3 + 3 + k] = b.hi[k]; }
		Insert(f.gridTriangles, f.cell, b, static_cast<std::uint32_t>(t3));
	}
	for (std::size_t e = 0; e < ne; ++e) {
		Box b;
		for (int k = 0; k < 2; ++k) b.Grow(At(f.colliderX, colliderEdges_[2*e + k]));
		for (int k = 0; k < 3; ++k) { f.edgeBox[6*e + k] = b.lo[k]; f.edgeBox[6*e + 3 + k] = b.hi[k]; }
		Insert(f.gridEdges, f.cell, b, static_cast<std::uint32_t>(e));
	}
	for (std::size_t i = 0; i < nv; ++i) {
		Box b;
		b.Grow(At(f.colliderX, static_cast<std::uint32_t>(i)));
		Insert(f.gridVertices, f.cell, b, static_cast<std::uint32_t>(i));
	}
}

void BarrierContact::Candidates(ContactFrame& f, const std::vector<double>& x, const std::vector<double>* dx) const
{
	std::vector<ContactPair>& out = f.candidates;
	out.clear();
	const double reach = xi_ + settings_.activation;
	auto point = [&](std::uint32_t i, Box& b) {
		const V3 p = At(x, i);
		b.Grow(p);
		if (dx) b.Grow(Add(p, At(*dx, i)));
	};
	// Analytic shapes: the signed distance is 1-Lipschitz.
	for (std::uint32_t i = 0; i < vertices_; ++i) {
		const V3 p = At(x, i);
		const double motion = dx ? Norm(At(*dx, i)) : 0.0;
		for (std::size_t c = 0; c < shapes_.size(); ++c) {
			V3 grad;
			if (shapes_[c].Distance(p, grad) - motion < reach) out.push_back({ContactPair::Shape, i, static_cast<std::uint32_t>(c)});
		}
	}
	const std::size_t nct = colliderTriangles_.size()/3, nce = colliderEdges_.size()/2, ncv = colliderV0_.size()/3;
	if (ncv > 0) {
		// Brute force when a box covers too many cells.
		auto query = [&](const HashGrid& grid, const Box& b, std::size_t count, auto&& test) {
			if (!ForCells(grid, f.cell, b, test)) for (std::size_t id = 0; id < count; ++id) test(static_cast<std::uint32_t>(id));
		};
		for (std::uint32_t i = 0; i < vertices_; ++i) {
			Box b;
			point(i, b);
			b.Inflate(reach);
			query(f.gridTriangles, b, nct, [&](std::uint32_t t) {
				if (b.Overlaps(&f.triangleBox[6*static_cast<std::size_t>(t)])) out.push_back({ContactPair::VertexTriangle, i, t});
			});
		}
		for (std::uint32_t e = 0; e < edges_.size()/2; ++e) {
			Box b;
			point(edges_[2*e], b); point(edges_[2*e + 1], b);
			b.Inflate(reach);
			query(f.gridEdges, b, nce, [&](std::uint32_t ce) {
				if (b.Overlaps(&f.edgeBox[6*static_cast<std::size_t>(ce)])) out.push_back({ContactPair::EdgeEdge, e, ce});
			});
		}
		for (std::uint32_t t = 0; t < triangles_.size()/3; ++t) {
			Box b;
			for (int k = 0; k < 3; ++k) point(triangles_[3*t + k], b);
			b.Inflate(reach);
			query(f.gridVertices, b, ncv, [&](std::uint32_t v) {
				const V3 p = At(f.colliderX, v);
				bool inside = true;
				for (int k = 0; k < 3; ++k) inside = inside && p[k] >= b.lo[k] && p[k] <= b.hi[k];
				if (inside) out.push_back({ContactPair::TriangleVertex, t, v});
			});
		}
	}
	if (settings_.selfCollision) {
		// A grid over the cloth triangles' and edges' (swept, inflated) boxes.
		HashGrid gt, ge;
		std::vector<double> tbox(6*(triangles_.size()/3)), ebox(6*(edges_.size()/2));
		double extent = 0;
		for (std::uint32_t e = 0; e < edges_.size()/2; ++e)
			extent += Norm(Sub(At(x, edges_[2*e]), At(x, edges_[2*e + 1])));
		const double cell = std::max(edges_.empty() ? 0.0 : extent/static_cast<double>(edges_.size()/2), 2*reach);
		for (std::uint32_t t = 0; t < triangles_.size()/3; ++t) {
			Box b;
			for (int k = 0; k < 3; ++k) point(triangles_[3*t + k], b);
			b.Inflate(reach);
			for (int k = 0; k < 3; ++k) { tbox[6*t + k] = b.lo[k]; tbox[6*t + 3 + k] = b.hi[k]; }
			Insert(gt, cell, b, t);
		}
		for (std::uint32_t e = 0; e < edges_.size()/2; ++e) {
			Box b;
			point(edges_[2*e], b); point(edges_[2*e + 1], b);
			b.Inflate(reach);
			for (int k = 0; k < 3; ++k) { ebox[6*e + k] = b.lo[k]; ebox[6*e + 3 + k] = b.hi[k]; }
			Insert(ge, cell, b, e);
		}
		for (std::uint32_t i = 0; i < vertices_; ++i) {
			Box b;
			point(i, b);
			auto test = [&](std::uint32_t t) {
				const std::uint32_t* tv = &triangles_[3*static_cast<std::size_t>(t)];
				if (tv[0] == i || tv[1] == i || tv[2] == i) return;
				if (b.Overlaps(&tbox[6*static_cast<std::size_t>(t)])) out.push_back({ContactPair::SelfVertexTriangle, i, t});
			};
			if (!ForCells(gt, cell, b, test)) for (std::uint32_t t = 0; t < triangles_.size()/3; ++t) test(t);
		}
		for (std::uint32_t e = 0; e < edges_.size()/2; ++e) {
			Box b;
			point(edges_[2*e], b); point(edges_[2*e + 1], b);
			auto test = [&](std::uint32_t o) {
				if (o <= e) return;
				const std::uint32_t a0 = edges_[2*e], a1 = edges_[2*e + 1], b0 = edges_[2*static_cast<std::size_t>(o)], b1 = edges_[2*static_cast<std::size_t>(o) + 1];
				if (a0 == b0 || a0 == b1 || a1 == b0 || a1 == b1) return;
				if (b.Overlaps(&ebox[6*static_cast<std::size_t>(o)])) out.push_back({ContactPair::SelfEdgeEdge, e, o});
			};
			if (!ForCells(ge, cell, b, test)) for (std::uint32_t o = 0; o < edges_.size()/2; ++o) test(o);
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
}

double BarrierContact::Weight(const ContactPair& p) const
{
	switch (p.kind) {
	case ContactPair::Shape:
	case ContactPair::VertexTriangle:
	case ContactPair::SelfVertexTriangle: return vertexWeight_[p.a];
	case ContactPair::EdgeEdge:
	case ContactPair::SelfEdgeEdge: return edgeWeight_[p.a];
	case ContactPair::TriangleVertex: break;
	}
	return triangleWeight_[p.a];
}

// The pair's four points (kind 0: point, triangle; kind 1: edge, edge), their
// ids and whether each is a cloth vertex.
bool BarrierContact::Points(const ContactFrame& f, const std::vector<double>& x, const ContactPair& p, V3* xs,
	std::uint32_t* ids, bool* cloth, int& kind) const
{
	auto cl = [&](int k, std::uint32_t v) { ids[k] = v; cloth[k] = true; xs[k] = At(x, v); };
	auto co = [&](int k, std::uint32_t v) { ids[k] = v; cloth[k] = false; xs[k] = At(f.colliderX, v); };
	switch (p.kind) {
	case ContactPair::Shape: return false;
	case ContactPair::VertexTriangle:
		kind = 0; cl(0, p.a);
		for (int k = 0; k < 3; ++k) co(1 + k, colliderTriangles_[3*static_cast<std::size_t>(p.b) + k]);
		return true;
	case ContactPair::TriangleVertex:
		kind = 0; co(0, p.b);
		for (int k = 0; k < 3; ++k) cl(1 + k, triangles_[3*static_cast<std::size_t>(p.a) + k]);
		return true;
	case ContactPair::EdgeEdge:
		kind = 1;
		cl(0, edges_[2*static_cast<std::size_t>(p.a)]); cl(1, edges_[2*static_cast<std::size_t>(p.a) + 1]);
		co(2, colliderEdges_[2*static_cast<std::size_t>(p.b)]); co(3, colliderEdges_[2*static_cast<std::size_t>(p.b) + 1]);
		return true;
	case ContactPair::SelfVertexTriangle:
		kind = 0; cl(0, p.a);
		for (int k = 0; k < 3; ++k) cl(1 + k, triangles_[3*static_cast<std::size_t>(p.b) + k]);
		return true;
	case ContactPair::SelfEdgeEdge:
		kind = 1;
		cl(0, edges_[2*static_cast<std::size_t>(p.a)]); cl(1, edges_[2*static_cast<std::size_t>(p.a) + 1]);
		cl(2, edges_[2*static_cast<std::size_t>(p.b)]); cl(3, edges_[2*static_cast<std::size_t>(p.b) + 1]);
		return true;
	}
	return false;
}

namespace {
// Adds a dense n-point (3n x 3n) contribution to the gradient and Hessian on
// the free cloth points; `self`: only the diagonal blocks enter the Hessian.
void Scatter(const double* g, const double* h, int n, const std::uint32_t* ids, const bool* cloth,
	const std::vector<char>& fixed, bool project, bool self, std::vector<double>* gradient, BlockMatrix* hessian)
{
	bool use[4];
	for (int k = 0; k < n; ++k) use[k] = cloth[k] && !fixed[ids[k]];
	if (gradient)
		for (int k = 0; k < n; ++k) if (use[k])
			for (int c = 0; c < 3; ++c) (*gradient)[3*static_cast<std::size_t>(ids[k]) + c] += g[3*k + c];
	if (!hessian) return;
	int map[4], m = 0;
	for (int k = 0; k < n; ++k) if (use[k]) map[m++] = k;
	if (m == 0) return;
	double sub[144];
	const int D = 3*n, S = 3*m;
	for (int a = 0; a < m; ++a) for (int b = 0; b < m; ++b)
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
			sub[(3*a + i)*S + 3*b + j] = (self && a != b) ? 0.0 : h[(3*map[a] + i)*D + 3*map[b] + j];
	if (project) {
		if (self) {
			for (int a = 0; a < m; ++a) {
				double blk[9];
				for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) blk[3*i + j] = sub[(3*a + i)*S + 3*a + j];
				ProjectPSD(blk, 3);
				for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) sub[(3*a + i)*S + 3*a + j] = blk[3*i + j];
			}
		} else ProjectPSD(sub, S);
	}
	for (int a = 0; a < m; ++a) for (int b = 0; b < m; ++b) {
		if (self && a != b) continue;
		double* blk = hessian->Block(hessian->Slot(ids[map[a]], ids[map[b]]));
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) blk[3*i + j] += sub[(3*a + i)*S + 3*b + j];
	}
}

void TangentBasis(const V3& n, double* t)
{
	int k = 0;
	for (int c = 1; c < 3; ++c) if (std::fabs(n[c]) < std::fabs(n[k])) k = c;
	V3 e{{0, 0, 0}};
	e[k] = 1;
	V3 t1 = Cross(n, e);
	t1 = Mul(t1, 1/Norm(t1));
	const V3 t2 = Cross(n, t1);
	for (int r = 0; r < 3; ++r) { t[2*r] = t1[r]; t[2*r + 1] = t2[r]; }
}
}

void BarrierContact::LagFriction(ContactFrame& f, const std::vector<double>& x, double scale, const std::vector<double>& xTilde) const
{
	f.friction.clear();
	f.frictionScale = scale;
	f.xTilde = xTilde;
	if (!(settings_.friction > 0) || !(scale > 0)) return;
	const double dhat = settings_.activation, kappa = settings_.stiffness;
	for (const ContactPair& p : f.candidates) {
		FrictionRecord r;
		V3 normal;
		double z;
		if (p.kind == ContactPair::Shape) {
			const V3 q = At(x, p.a);
			z = shapes_[p.b].Distance(q, normal) - xi_;
			r.count = 1; r.point[0] = p.a; r.cloth[0] = true; r.weight[0] = 1;
		} else {
			V3 xs[4];
			std::uint32_t ids[4];
			bool cloth[4];
			int kind = 0;
			Points(f, x, p, xs, ids, cloth, kind);
			const Piece piece = kind == 0 ? PointTriangle(xs) : EdgeEdge(xs);
			z = std::sqrt(piece.d2) - xi_;
			V3 rv{{0, 0, 0}};
			r.count = 4;
			for (int k = 0; k < 4; ++k) { r.point[k] = ids[k]; r.cloth[k] = cloth[k]; r.weight[k] = 0; }
			for (int i = 0; i < piece.n; ++i) {
				double c = piece.c0[i];
				for (int k = 0; k < piece.m; ++k) c += piece.cw[i][k]*piece.w[k];
				r.weight[piece.idx[i]] += c;
				rv = Add(rv, Mul(xs[piece.idx[i]], c));
			}
			const double l = Norm(rv);
			if (!(l > 0)) continue;
			normal = Mul(rv, 1/l);
		}
		if (!(z > 0) || !(z < dhat)) continue;
		double b1 = 0;
		Barrier(z, dhat, &b1, nullptr);
		r.normalForce = kappa*Weight(p)*std::fabs(b1);
		TangentBasis(normal, r.tangent);
		f.friction.push_back(r);
	}
}

double BarrierContact::Evaluate(const ContactFrame& f, const std::vector<double>& x, std::vector<double>* gradient,
	BlockMatrix* hessian, bool project, const std::vector<char>& fixed) const
{
	const double dhat = settings_.activation, kappa = settings_.stiffness;
	double energy = 0;
	const bool derivatives = gradient || hessian;
	for (const ContactPair& p : f.candidates) {
		if (p.kind == ContactPair::Shape) {
			if (fixed[p.a]) continue;
			V3 n;
			double hd[9];
			const double z = shapes_[p.b].Distance(At(x, p.a), n, hessian ? hd : nullptr) - xi_;
			if (!(z > 0)) return Infinity; // also a NaN gap
			if (!(z < dhat)) continue;
			double b1, b2;
			const double w = kappa*Weight(p);
			energy += w*Barrier(z, dhat, &b1, &b2);
			if (!derivatives) continue;
			double g[3], h[9];
			for (int i = 0; i < 3; ++i) g[i] = w*b1*n[i];
			if (hessian) for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) h[3*i + j] = w*(b2*n[i]*n[j] + b1*hd[3*i + j]);
			const std::uint32_t ids[1] = {p.a};
			const bool cloth[1] = {true};
			Scatter(g, hessian ? h : nullptr, 1, ids, cloth, fixed, project, false, gradient, hessian);
			continue;
		}
		V3 xs[4];
		std::uint32_t ids[4];
		bool cloth[4];
		int kind = 0;
		Points(f, x, p, xs, ids, cloth, kind);
		bool any = false;
		for (int k = 0; k < 4; ++k) any = any || (cloth[k] && !fixed[ids[k]]);
		const Piece piece = kind == 0 ? PointTriangle(xs) : EdgeEdge(xs);
		const double d = std::sqrt(piece.d2), z = d - xi_;
		if (!(z > 0)) return Infinity;
		if (!(z < dhat)) continue;
		double b1, b2;
		const double w = kappa*Weight(p);
		energy += w*Barrier(z, dhat, &b1, &b2);
		if (!derivatives || !any) continue;
		double g2[12], h2[144], g[12], h[144];
		PieceDerivatives(piece, xs, 4, g2, hessian ? h2 : nullptr);
		double gd[12];
		for (int k = 0; k < 12; ++k) { gd[k] = g2[k]/(2*d); g[k] = w*b1*gd[k]; }
		if (hessian)
			for (int i = 0; i < 12; ++i) for (int j = 0; j < 12; ++j) {
				const double hd = (h2[12*i + j] - 2*gd[i]*gd[j])/(2*d);
				h[12*i + j] = w*(b2*gd[i]*gd[j] + b1*hd);
			}
		const bool self = p.kind == ContactPair::SelfVertexTriangle || p.kind == ContactPair::SelfEdgeEdge;
		Scatter(g, hessian ? h : nullptr, 4, ids, cloth, fixed, project, self, gradient, hessian);
	}
	// Lagged friction: (mu lambda / c) F0(|T^T v_rel|), v = c (x - xtilde).
	const double mu = settings_.friction, eps = settings_.frictionVelocity, c = f.frictionScale;
	for (const FrictionRecord& r : f.friction) {
		V3 v{{0, 0, 0}};
		for (int k = 0; k < r.count; ++k) {
			if (r.weight[k] == 0) continue;
			const std::size_t q = 3*static_cast<std::size_t>(r.point[k]);
			const V3 vk = r.cloth[k] ? V3{{c*(x[q] - f.xTilde[q]), c*(x[q+1] - f.xTilde[q+1]), c*(x[q+2] - f.xTilde[q+2])}} :
				V3{{f.colliderV[q], f.colliderV[q+1], f.colliderV[q+2]}};
			v = Add(v, Mul(vk, r.weight[k]));
		}
		const double u0 = r.tangent[0]*v[0] + r.tangent[2]*v[1] + r.tangent[4]*v[2];
		const double u1 = r.tangent[1]*v[0] + r.tangent[3]*v[1] + r.tangent[5]*v[2];
		const double y = std::sqrt(u0*u0 + u1*u1);
		double f0, f1, df1;
		FrictionCurve(y, eps, f0, f1, df1);
		const double ml = mu*r.normalForce;
		energy += ml/c*f0;
		if (!derivatives) continue;
		// Force direction T u-hat and the 2x2 tangent stiffness.
		double uh[2] = {0, 0}, m2[4];
		if (y > 1e-12*eps) {
			uh[0] = u0/y; uh[1] = u1/y;
			const double ratio = f1/y;
			m2[0] = df1*uh[0]*uh[0] + ratio*(1 - uh[0]*uh[0]);
			m2[1] = m2[2] = df1*uh[0]*uh[1] - ratio*uh[0]*uh[1];
			m2[3] = df1*uh[1]*uh[1] + ratio*(1 - uh[1]*uh[1]);
		} else { m2[0] = m2[3] = 2/eps; m2[1] = m2[2] = 0; }
		V3 dir;
		for (int i = 0; i < 3; ++i) dir[i] = r.tangent[2*i]*uh[0] + r.tangent[2*i + 1]*uh[1];
		double tm[9]; // T m2 T^T
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
			double sacc = 0;
			for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) sacc += r.tangent[2*i + a]*m2[2*a + b]*r.tangent[2*j + b];
			tm[3*i + j] = sacc;
		}
		double g[12], h[144];
		const int n = r.count;
		for (int k = 0; k < n; ++k) for (int i = 0; i < 3; ++i) g[3*k + i] = ml*f1*r.weight[k]*dir[i];
		if (hessian)
			for (int a = 0; a < n; ++a) for (int b = 0; b < n; ++b) for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
				h[(3*a + i)*3*n + 3*b + j] = ml*c*r.weight[a]*r.weight[b]*tm[3*i + j];
		bool self = true;
		for (int k = 0; k < n; ++k) self = self && r.cloth[k];
		Scatter(g, hessian ? h : nullptr, n, r.point, r.cloth, fixed, false, n > 1 && self, gradient, hessian);
	}
	return energy;
}

double BarrierContact::MaxStep(const ContactFrame& f, const std::vector<double>& x, const std::vector<double>& dx,
	ContactStats* stats) const
{
	double alpha = 1;
	for (const ContactPair& p : f.candidates) {
		double t = 1;
		if (p.kind == ContactPair::Shape) t = ShapeCCD(shapes_[p.b], At(x, p.a), At(dx, p.a), xi_, 0.1);
		else {
			V3 xs[4], ds[4];
			std::uint32_t ids[4];
			bool cloth[4];
			int kind = 0;
			Points(f, x, p, xs, ids, cloth, kind);
			bool moving = false;
			for (int k = 0; k < 4; ++k) {
				ds[k] = cloth[k] ? At(dx, ids[k]) : V3{{0, 0, 0}};
				moving = moving || ds[k][0] != 0 || ds[k][1] != 0 || ds[k][2] != 0;
			}
			if (moving) t = AdditiveCCD(kind, xs, ds, xi_, 0.1);
		}
		alpha = std::min(alpha, t);
	}
	if (stats) {
		++stats->ccdCalls;
		if (alpha < 1) ++stats->ccdLimited;
		stats->smallestAlpha = std::min(stats->smallestAlpha, alpha);
	}
	return alpha;
}

double BarrierContact::SmallestGap(const ContactFrame& f, const std::vector<double>& x) const
{
	double gap = Infinity;
	for (const ContactPair& p : f.candidates) {
		if (p.kind == ContactPair::Shape) {
			V3 n;
			gap = std::min(gap, shapes_[p.b].Distance(At(x, p.a), n) - xi_);
			continue;
		}
		V3 xs[4];
		std::uint32_t ids[4];
		bool cloth[4];
		int kind = 0;
		Points(f, x, p, xs, ids, cloth, kind);
		gap = std::min(gap, std::sqrt(PairDistance2(kind, xs)) - xi_);
	}
	return gap;
}

V3 BarrierContact::TotalForce(const ContactFrame& f, const std::vector<double>& x) const
{
	std::vector<double> g(x.size(), 0.0);
	ContactFrame plain = f;
	plain.friction.clear();
	const std::vector<char> none(vertices_, 0);
	Evaluate(plain, x, &g, nullptr, false, none);
	V3 total{{0, 0, 0}};
	for (std::uint32_t i = 0; i < vertices_; ++i) total = Sub(total, At(g, i));
	return total;
}

bool BarrierContact::IntersectionFree(ContactFrame& f, const std::vector<double>& x0, const std::vector<double>& x1,
	std::string& detail, bool skipMoving) const
{
	std::vector<double> dx(x0.size());
	for (std::size_t k = 0; k < dx.size(); ++k) dx[k] = x1[k] - x0[k];
	Candidates(f, x0, &dx);
	for (const ContactPair& p : f.candidates) {
		if (p.kind == ContactPair::Shape) {
			V3 n;
			const V3 a = At(x0, p.a), b = At(x1, p.a);
			// Crossing the surface itself (xi = 0), by conservative advancement to a tiny fraction of the gap.
			if (!(shapes_[p.b].Distance(a, n) > 0) || ShapeCCD(shapes_[p.b], a, Sub(b, a), 0.0, 1e-6) < 1) {
				detail = "vertex " + std::to_string(p.a) + " reaches analytic collider " + std::to_string(p.b);
				return false;
			}
			continue;
		}
		V3 a[4], b[4];
		std::uint32_t ids[4];
		bool cloth[4];
		int kind = 0;
		Points(f, x0, p, a, ids, cloth, kind);
		Points(f, x1, p, b, ids, cloth, kind);
		bool moving = false;
		for (int k = 0; k < 4; ++k) moving = moving || (!cloth[k] && motions_[colliderMesh_[ids[k]]].Moving());
		if (skipMoving && moving) continue;
		if (CrossesDuringStep(kind, a, b)) {
			detail = std::string(kind == 0 ? "point-triangle" : "edge-edge") + " pair (" + std::to_string(p.a) + ", " +
				std::to_string(p.b) + ") of kind " + std::to_string(static_cast<int>(p.kind)) + " touches";
			return false;
		}
	}
	return true;
}

bool BarrierContact::Carry(const std::vector<double>& x, double t0, double t1, std::vector<double>& out) const
{
	out = x;
	if (!moving_) return false;
	ContactFrame f0, f1;
	Place(t0, f0);
	Place(t1, f1);
	// Per moving mesh: its largest displacement over the step (vertices, and the
	// farthest point of its box under the motion, which bounds carried cloth).
	const std::size_t meshes = motions_.size();
	std::vector<double> travel(meshes, 0.0);
	std::vector<Box> boxes(meshes);
	for (std::size_t i = 0; i < colliderV0_.size()/3; ++i) {
		const std::uint32_t m = colliderMesh_[i];
		const V3 p0 = At(f0.colliderX, static_cast<std::uint32_t>(i));
		boxes[m].Grow(p0);
		travel[m] = std::max(travel[m], Norm(Sub(At(f1.colliderX, static_cast<std::uint32_t>(i)), p0)));
	}
	double longest = 0;
	for (std::size_t e = 0; e < edges_.size()/2; ++e)
		longest = std::max(longest, Norm(Sub(At(x, edges_[2*e]), At(x, edges_[2*e + 1]))));
	// A vertex within R of a moving mesh at t0 moves with it (the nearest one
	// when several qualify).  R is wide enough that an element with a vertex
	// beyond it stays out of reach however its carried vertices move: three
	// times the travel, twice the reach and twice the longest cloth edge.
	std::vector<double> nearest(vertices_, Infinity);
	std::vector<int> owner(vertices_, -1);
	const double reach = xi_ + settings_.activation;
	for (std::size_t m = 0; m < meshes; ++m) {
		if (!motions_[m].Moving()) continue;
		const double R = 3*travel[m] + 2*reach + 2*longest;
		Box b = boxes[m];
		b.Inflate(R);
		for (std::uint32_t i = 0; i < vertices_; ++i) {
			const V3 p = At(x, i);
			bool inside = true;
			for (int k = 0; k < 3; ++k) inside = inside && p[k] >= b.lo[k] && p[k] <= b.hi[k];
			if (!inside) continue;
			Box q;
			q.Grow(p);
			q.Inflate(R);
			double best = Infinity;
			auto test = [&](std::uint32_t t) {
				if (colliderMesh_[colliderTriangles_[3*static_cast<std::size_t>(t)]] != m) return;
				const V3 xs[4] = {p, At(f0.colliderX, colliderTriangles_[3*static_cast<std::size_t>(t)]),
					At(f0.colliderX, colliderTriangles_[3*static_cast<std::size_t>(t) + 1]),
					At(f0.colliderX, colliderTriangles_[3*static_cast<std::size_t>(t) + 2])};
				best = std::min(best, std::sqrt(PointTriangle(xs).d2));
			};
			if (!ForCells(f0.gridTriangles, f0.cell, q, test))
				for (std::uint32_t t = 0; t < colliderTriangles_.size()/3; ++t) test(t);
			if (best < R && best < nearest[i]) { nearest[i] = best; owner[i] = static_cast<int>(m); }
		}
	}
	bool any = false;
	for (std::uint32_t i = 0; i < vertices_; ++i) {
		if (owner[i] < 0) continue;
		const V3 q = motions_[static_cast<std::size_t>(owner[i])].Advance(At(x, i), t0, t1);
		for (int k = 0; k < 3; ++k) out[3*static_cast<std::size_t>(i) + k] = q[k];
		any = true;
	}
	return any;
}

} } }
