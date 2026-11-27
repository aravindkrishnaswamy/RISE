//////////////////////////////////////////////////////////////////////
//
//  LiquidLevelSet.cpp - see LiquidLevelSet.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidLevelSet.h"

#include <algorithm>
#include <cmath>

namespace RISE { namespace Simulation { namespace Liquid {

namespace {
constexpr double Pi = 3.14159265358979323846;
inline double Sq(double x) { return x*x; }

std::size_t Rows(const std::array<int,3>& d) { return std::size_t(d[1])*std::size_t(d[2]); }

// splitmix64: a reproducible hash of (cell, step, index) for seeding.
std::uint64_t Mix(std::uint64_t x)
{
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30))*0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27))*0x94d049bb133111ebull;
	return x ^ (x >> 31);
}
double Uniform(std::uint64_t& state)
{
	state = Mix(state);
	return double(state >> 11)*(1.0/9007199254740992.0);
}

// Smoothed Heaviside and delta, epsilon = 1.5 h.
double Heaviside(double x, double eps)
{
	if (x < -eps) return 0;
	if (x > eps) return 1;
	return 0.5*(1 + x/eps + std::sin(Pi*x/eps)/Pi);
}
double Delta(double x, double eps)
{
	if (std::fabs(x) > eps) return 0;
	return 0.5*(1 + std::cos(Pi*x/eps))/eps;
}

// |grad phi| by central differences (one-sided at the ends; 0 along thin axes).
// m7f: with a barrier, one-sided at a closed face too (0 between two).
double GradientNorm(const Grid& g, const std::vector<double>& phi, int i, int j, int k, const LevelSetBarrier* barrier = nullptr)
{
	const int idx[3] = {i, j, k};
	double s = 0;
	for (int a = 0; a < 3; ++a) {
		if (g.n[a] == 1) continue;
		int lo[3] = {i, j, k}, hi[3] = {i, j, k};
		double span = 2;
		if (barrier && !barrier->closed[std::size_t(a)].empty()) {
			lo[a] = barrier->Reach(g, idx, a, -1);
			hi[a] = barrier->Reach(g, idx, a, 1);
			span = hi[a] - lo[a];
			if (span == 0) continue;
		}
		else if (idx[a] == 0) { ++hi[a]; span = 1; }
		else if (idx[a] == g.n[a] - 1) { --lo[a]; span = 1; }
		else { --lo[a]; ++hi[a]; }
		s += Sq((phi[g.C(hi[0], hi[1], hi[2])] - phi[g.C(lo[0], lo[1], lo[2])])/(span*g.h));
	}
	return std::sqrt(s);
}

V3 Gradient(const Grid& g, const std::vector<double>& phi, const V3& x)
{
	V3 n{{0, 0, 0}};
	for (int a = 0; a < 3; ++a) {
		if (g.n[a] == 1) continue;
		V3 p = x, m = x;
		p[a] += 0.5*g.h; m[a] -= 0.5*g.h;
		n[a] = (SampleCells(g, phi, p) - SampleCells(g, phi, m))/g.h;
	}
	return n;
}

bool InDomain(const Grid& g, const V3& x)
{
	for (int a = 0; a < 3; ++a)
		if (!(x[a] >= g.origin[a] && x[a] <= g.origin[a] + g.h*g.n[a])) return false;
	return true;
}
} // namespace

double Weno5(double v1, double v2, double v3, double v4, double v5)
{
	const double s1 = 13.0/12.0*Sq(v1 - 2*v2 + v3) + 0.25*Sq(v1 - 4*v2 + 3*v3);
	const double s2 = 13.0/12.0*Sq(v2 - 2*v3 + v4) + 0.25*Sq(v2 - v4);
	const double s3 = 13.0/12.0*Sq(v3 - 2*v4 + v5) + 0.25*Sq(3*v3 - 4*v4 + v5);
	const double eps = 1e-6*std::max(std::max(std::max(Sq(v1), Sq(v2)), std::max(Sq(v3), Sq(v4))), Sq(v5)) + 1e-99;
	const double a1 = 0.1/Sq(s1 + eps), a2 = 0.6/Sq(s2 + eps), a3 = 0.3/Sq(s3 + eps);
	const double p1 = v1/3 - 7*v2/6 + 11*v3/6;
	const double p2 = -v2/6 + 5*v3/6 + v4/3;
	const double p3 = v3/3 + 5*v4/6 - v5/6;
	return (a1*p1 + a2*p2 + a3*p3)/(a1 + a2 + a3);
}

void LevelSetBarrier::Clear()
{
	for (int a = 0; a < 3; ++a) { closed[std::size_t(a)].clear(); cells[std::size_t(a)].clear(); samples[std::size_t(a)].clear(); }
	fixed.clear();
}

int LevelSetBarrier::Reach(const Grid& g, const int idx[3], int axis, int offset) const
{
	const std::vector<std::uint8_t>& cl = closed[std::size_t(axis)];
	int f[3] = {idx[0], idx[1], idx[2]};
	int t = idx[axis];
	const int dir = offset > 0 ? 1 : -1;
	for (int step = 0; step < std::abs(offset); ++step) {
		const int nt = t + dir;
		if (nt < 0 || nt >= g.n[axis]) break;
		f[axis] = std::max(t, nt);
		if (!cl.empty() && cl[g.F(axis, f[0], f[1], f[2])] == 1) break;
		t = nt;
	}
	return t;
}

void LevelSetBarrier::Build(const Grid& g, const std::array<std::vector<std::pair<std::size_t, std::uint8_t>>,3>& faces)
{
	Clear();
	for (int a = 0; a < 3; ++a) {
		if (faces[std::size_t(a)].empty() || g.n[a] < 2) continue;
		closed[std::size_t(a)].assign(g.Faces(a), 0);
		for (const auto& f : faces[std::size_t(a)]) closed[std::size_t(a)][f.first] = f.second;
	}
	for (int a = 0; a < 3; ++a) {
		if (closed[std::size_t(a)].empty()) continue;
		const auto d = g.FaceDims(a);
		std::vector<std::size_t>& list = cells[std::size_t(a)];
		for (const auto& fe : faces[std::size_t(a)]) {
			const std::size_t f = fe.first;
			int idx[3] = {int(f % std::size_t(d[0])), int((f/std::size_t(d[0])) % std::size_t(d[1])), int(f/(std::size_t(d[0])*std::size_t(d[1])))};
			const int fa = idx[a];   // the face between cells fa - 1 and fa
			for (int t = fa - 3; t <= fa + 2; ++t) {
				if (t < 0 || t >= g.n[a]) continue;
				idx[a] = t;
				list.push_back(g.C(idx[0], idx[1], idx[2]));
			}
		}
		std::sort(list.begin(), list.end());
		list.erase(std::unique(list.begin(), list.end()), list.end());
		std::vector<Sample>& out = samples[std::size_t(a)];
		out.resize(7*list.size());
		const std::vector<std::uint8_t>& cl = closed[std::size_t(a)];
		for (std::size_t e = 0; e < list.size(); ++e) {
			const std::size_t c = list[e];
			const int idx[3] = {int(c % std::size_t(g.n[0])), int((c/std::size_t(g.n[0])) % std::size_t(g.n[1])),
				int(c/(std::size_t(g.n[0])*std::size_t(g.n[1])))};
			auto cellAt = [&](int t) { int q[3] = {idx[0], idx[1], idx[2]}; q[a] = t; return g.C(q[0], q[1], q[2]); };
			auto faceKind = [&](int t0, int t1) { int q[3] = {idx[0], idx[1], idx[2]}; q[a] = std::max(t0, t1); return cl[g.F(a, q[0], q[1], q[2])]; };
			for (int m = 0; m < 7; ++m) {
				const int o = m - 3;
				Sample& S = out[7*e + std::size_t(m)];
				int t = idx[a];
				S.i1 = S.i2 = c; S.s = 0;
				if (o == 0) continue;
				const int dir = o > 0 ? 1 : -1;
				bool done = false;
				for (int step = 0; step < std::abs(o) && !done; ++step) {
					const int nt = t + dir;
					if (nt < 0 || nt >= g.n[a]) {
						// Beyond the domain: the level set's linear extension from the edge (constant when
						// the edge's inner neighbour lies across a closed face).
						const int inner = t - dir;
						S.i1 = cellAt(t);
						S.outside = true;
						if (inner >= 0 && inner < g.n[a] && faceKind(inner, t) != 1) { S.i2 = cellAt(inner); S.s = double(std::abs(idx[a] + o - t)); }
						else S.i2 = S.i1;
						done = true;
					} else if (faceKind(t, nt) == 1) { S.i1 = S.i2 = cellAt(t); done = true; }
					else if (faceKind(t, nt) == 2) {
						// A boundary face: the cell across, then the line through it and the last cell
						// on this side: f[t] + k (f[nt] - f[t]) for the k-th sample beyond t.
						S.i1 = cellAt(t); S.i2 = cellAt(nt); S.s = -double(std::abs(idx[a] + o - t));
						done = true;
					}
					else t = nt;
				}
				if (!done) S.i1 = S.i2 = cellAt(t);
			}
		}
	}
}

void OneSidedDerivatives(const std::array<int,3>& d, double h, const std::vector<double>& f, int axis,
	const std::array<Extension,3>& ext, Pool* pool,
	const std::function<void(std::size_t row, const double* dm, const double* dp)>& emit,
	const LevelSetBarrier* barrier)
{
	const int n0 = d[0];
	const double inv = 1/h;
	const int na = d[axis];
	const bool linear = ext[axis] == Extension::Linear && na > 1;
	const bool patched = barrier && !barrier->cells[std::size_t(axis)].empty();
	ForRows(pool, Rows(d), RowGrain(n0), [&](std::size_t r0, std::size_t r1) {
		std::vector<double> line(axis == 0 ? std::size_t(n0) + 6 : 0), ghost(axis == 0 ? 0 : 7*std::size_t(n0));
		std::vector<double> dm(static_cast<std::size_t>(n0)), dp(static_cast<std::size_t>(n0));
		const double* P[7];
		for (std::size_t r = r0; r < r1; ++r) {
			const std::size_t base = std::size_t(n0)*r;
			const double* row = &f[base];
			if (axis == 0) {
				for (int i = 0; i < n0; ++i) line[std::size_t(i) + 3] = row[i];
				for (int s = 1; s <= 3; ++s) {
					line[std::size_t(3 - s)] = linear ? row[0] + s*(row[0] - row[1]) : row[0];
					line[std::size_t(n0 + 2 + s)] = linear ? row[n0 - 1] + s*(row[n0 - 1] - row[n0 - 2]) : row[n0 - 1];
				}
				for (int m = 0; m < 7; ++m) P[m] = line.data() + m;
			} else {
				const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
				const int t = axis == 1 ? j : k;
				const std::ptrdiff_t stride = axis == 1 ? std::ptrdiff_t(n0) : std::ptrdiff_t(n0)*std::ptrdiff_t(d[1]);
				for (int m = 0; m < 7; ++m) {
					const int o = m - 3, tt = t + o;
					if (tt >= 0 && tt < na) { P[m] = row + o*stride; continue; }
					double* gr = &ghost[std::size_t(m)*std::size_t(n0)];
					const int edge = tt < 0 ? 0 : na - 1;
					const double* fe = row + (edge - t)*stride;
					if (!linear) for (int i = 0; i < n0; ++i) gr[i] = fe[i];
					else {
						const double* fi = row + ((tt < 0 ? 1 : na - 2) - t)*stride;
						const double s = tt < 0 ? -tt : tt - (na - 1);
						for (int i = 0; i < n0; ++i) gr[i] = fe[i] + s*(fe[i] - fi[i]);
					}
					P[m] = gr;
				}
			}
			for (int i = 0; i < n0; ++i) {
				const double q0 = (P[1][i] - P[0][i])*inv, q1 = (P[2][i] - P[1][i])*inv, q2 = (P[3][i] - P[2][i])*inv;
				const double q3 = (P[4][i] - P[3][i])*inv, q4 = (P[5][i] - P[4][i])*inv, q5 = (P[6][i] - P[5][i])*inv;
				dm[std::size_t(i)] = Weno5(q0, q1, q2, q3, q4);
				dp[std::size_t(i)] = Weno5(q5, q4, q3, q2, q1);
			}
			if (patched) {
				// m7f: the cells whose stencil reaches past a closed face, with their sided samples.
				const std::vector<std::size_t>& cl = barrier->cells[std::size_t(axis)];
				for (auto it = std::lower_bound(cl.begin(), cl.end(), base); it != cl.end() && *it < base + std::size_t(n0); ++it) {
					const LevelSetBarrier::Sample* S = &barrier->samples[std::size_t(axis)][7*std::size_t(it - cl.begin())];
					double v[7];
					for (int m = 0; m < 7; ++m) v[m] = !linear && S[m].outside ? f[S[m].i1] : f[S[m].i1] + S[m].s*(f[S[m].i1] - f[S[m].i2]);
					const double q0 = (v[1] - v[0])*inv, q1 = (v[2] - v[1])*inv, q2 = (v[3] - v[2])*inv;
					const double q3 = (v[4] - v[3])*inv, q4 = (v[5] - v[4])*inv, q5 = (v[6] - v[5])*inv;
					const std::size_t i = *it - base;
					dm[i] = Weno5(q0, q1, q2, q3, q4);
					dp[i] = Weno5(q5, q4, q3, q2, q1);
				}
			}
			emit(base, dm.data(), dp.data());
		}
	});
}

void UpwindAdvectionRhs(const std::array<int,3>& d, double h, const std::vector<double>& f,
	const std::array<const std::vector<double>*,3>& c, const std::array<Extension,3>& ext,
	std::vector<double>& rhs, Pool* pool, const LevelSetBarrier* barrier)
{
	rhs.assign(f.size(), 0.0);
	const int n0 = d[0];
	for (int axis = 0; axis < 3; ++axis) {
		if (d[axis] == 1) continue;   // thin axis: no variation
		const std::vector<double>& s = *c[axis];
		OneSidedDerivatives(d, h, f, axis, ext, pool, [&](std::size_t base, const double* dm, const double* dp) {
			for (int i = 0; i < n0; ++i) {
				const double v = s[base + std::size_t(i)];
				rhs[base + std::size_t(i)] -= v*(v > 0 ? dm[i] : dp[i]);
			}
		}, barrier);
	}
}

void AdvectLevelSet(const Grid& g, std::vector<double>& phi, const CellVelocityAt& velocity, double t, double dt, Pool* pool,
	const LevelSetBarrier* barrier)
{
	if (barrier && barrier->Empty()) barrier = nullptr;
	const std::array<Extension,3> ext{{Extension::Linear, Extension::Linear, Extension::Linear}};
	std::array<std::vector<double>,3> c;
	std::vector<double> rhs, p1(phi.size()), p2(phi.size());
	const std::size_t n = phi.size();
	const std::array<const std::vector<double>*,3> cp{{&c[0], &c[1], &c[2]}};
	velocity(t, c);
	UpwindAdvectionRhs(g.n, g.h, phi, cp, ext, rhs, pool, barrier);
	ForRows(pool, n, 4096, [&](std::size_t a, std::size_t b) { for (std::size_t i = a; i < b; ++i) p1[i] = phi[i] + dt*rhs[i]; });
	velocity(t + dt, c);
	UpwindAdvectionRhs(g.n, g.h, p1, cp, ext, rhs, pool, barrier);
	ForRows(pool, n, 4096, [&](std::size_t a, std::size_t b) {
		for (std::size_t i = a; i < b; ++i) p2[i] = 0.75*phi[i] + 0.25*(p1[i] + dt*rhs[i]);
	});
	velocity(t + 0.5*dt, c);
	UpwindAdvectionRhs(g.n, g.h, p2, cp, ext, rhs, pool, barrier);
	ForRows(pool, n, 4096, [&](std::size_t a, std::size_t b) {
		for (std::size_t i = a; i < b; ++i) phi[i] = phi[i]/3 + 2.0/3.0*(p2[i] + dt*rhs[i]);
	});
}

void Reinitialize(const Grid& g, std::vector<double>& phi, ReinitScheme scheme, int iterations, double cfl, Pool* pool,
	const LevelSetBarrier* barrier, Extension wall)
{
	if (barrier && barrier->Empty()) barrier = nullptr;
	if (scheme == ReinitScheme::None || iterations <= 0) return;
	const std::size_t n = phi.size();
	const std::vector<double> phi0 = phi;
	const double h = g.h;
	std::vector<double> sgn(n), dtau(n, cfl*h);
	// Cells with a sign change to a face neighbour: their index into `nearData`.
	struct Near {
		double dist = 0;        // Russo-Smereka distance estimate
		double dm[3], dp[3];    // Subcell: distance to the crossing on the minus / plus side (0: none)
	};
	std::vector<std::int32_t> nearId(n, -1);
	std::vector<Near> nearData;
	for (int k = 0; k < g.n[2]; ++k) for (int j = 0; j < g.n[1]; ++j) for (int i = 0; i < g.n[0]; ++i) {
		const std::size_t c = g.C(i, j, k);
		const double p = phi0[c];
		sgn[c] = p/std::sqrt(p*p + h*h);
		if (scheme == ReinitScheme::Sussman) continue;
		const int idx[3] = {i, j, k};
		Near d;
		bool crossing = false;
		double grad2 = 0, central2 = 0, closest = h;
		for (int a = 0; a < 3; ++a) {
			d.dm[a] = d.dp[a] = 0;
			if (g.n[a] == 1) continue;
			double fm = 0, fp = 0;
			// m7f: a neighbour across a closed face is absent.
			const bool hm = barrier ? barrier->Reach(g, idx, a, -1) != idx[a] : idx[a] > 0;
			const bool hp = barrier ? barrier->Reach(g, idx, a, 1) != idx[a] : idx[a] < g.n[a] - 1;
			if (hm) { int q[3] = {i, j, k}; --q[a]; fm = phi0[g.C(q[0], q[1], q[2])]; }
			if (hp) { int q[3] = {i, j, k}; ++q[a]; fp = phi0[g.C(q[0], q[1], q[2])]; }
			const bool cm = hm && (fm < 0) != (p < 0), cp = hp && (fp < 0) != (p < 0);
			crossing = crossing || cm || cp;
			// Crossings of the quadratic ENO interpolant (Min and Gibou 2007): the
			// second difference of smaller magnitude of the two stencils that
			// contain the cell pair; kept at least 1e-3 h from the centre.
			auto at = [&](int o) {
				int q[3] = {i, j, k};
				q[a] = barrier ? barrier->Reach(g, idx, a, o) : std::max(0, std::min(g.n[a] - 1, idx[a] + o));
				return phi0[g.C(q[0], q[1], q[2])];
			};
			auto crossingTo = [&](int dir) {
				const double f0 = p, f1 = at(dir);
				const double d2a = at(-dir) - 2*f0 + f1, d2b = f0 - 2*f1 + at(2*dir);
				const double c2 = 0.5*((d2a*d2b <= 0) ? 0.0 : (std::fabs(d2a) < std::fabs(d2b) ? d2a : d2b));
				double t = f0/(f0 - f1);   // linear root, s in [0, 1]
				if (std::fabs(c2) > 1e-14*(std::fabs(f0) + std::fabs(f1))) {
					// f(s) = f0 + (f1 - f0 - c2) s + c2 s^2 on s in [0, 1] (s = distance / h).
					const double b = f1 - f0 - c2, disc = b*b - 4*c2*f0;
					if (disc >= 0) {
						const double sq = std::sqrt(disc);
						const double r1 = (-b - sq)/(2*c2), r2 = (-b + sq)/(2*c2);
						if (r1 >= 0 && r1 <= 1) t = r1;
						else if (r2 >= 0 && r2 <= 1) t = r2;
					}
				}
				return std::max(1e-3*h, h*t);
			};
			if (cm) { d.dm[a] = crossingTo(-1); closest = std::min(closest, d.dm[a]); }
			if (cp) { d.dp[a] = crossingTo(+1); closest = std::min(closest, d.dp[a]); }
			// Russo and Smereka: the largest of the one-sided and central differences;
			// the central (one-sided at a wall) difference for RussoSmerekaCentral.
			double slope = 1e-12;
			if (hm) slope = std::max(slope, std::fabs(p - fm)/h);
			if (hp) slope = std::max(slope, std::fabs(fp - p)/h);
			if (hm && hp) slope = std::max(slope, std::fabs(fp - fm)/(2*h));
			grad2 += slope*slope;
			const double central = hm && hp ? (fp - fm)/(2*h) : hm ? (p - fm)/h : hp ? (fp - p)/h : 0.0;
			central2 += central*central;
		}
		if (!crossing) continue;
		// The central estimate unless it collapses (a kink or an extremum across
		// the cell): then Russo and Smereka's maximum.
		d.dist = scheme == ReinitScheme::RussoSmerekaCentral && central2 >= 0.25*grad2 ? p/std::sqrt(central2) : p/std::sqrt(grad2);
		nearId[c] = std::int32_t(nearData.size());
		nearData.push_back(d);
		// Subcell: the interface is a boundary at distance `closest`; the local
		// pseudo-time step keeps the update stable (dtau / d <= 1/2).
		if (scheme == ReinitScheme::Subcell) dtau[c] = std::min(cfl*h, 0.5*closest);
		// Frozen: the interface cells keep their values (the zero contour cannot move).
		if (scheme == ReinitScheme::Frozen) dtau[c] = 0;
	}
	// m7f: boundary values beside a sheet (LevelSetBarrier::fixed) are kept.
	if (barrier && !barrier->fixed.empty())
		for (std::size_t c = 0; c < n; ++c) if (barrier->fixed[c]) dtau[c] = 0;
	const std::array<Extension,3> ext{{wall, wall, wall}};
	std::vector<double> g2(n), rhs(n), p1(n), p2(n);
	const bool subcell = scheme == ReinitScheme::Subcell, frozen = scheme == ReinitScheme::RussoSmereka || scheme == ReinitScheme::RussoSmerekaCentral;
	auto L = [&](const std::vector<double>& f) {
		std::fill(g2.begin(), g2.end(), 0.0);
		const int n0 = g.n[0];
		for (int axis = 0; axis < 3; ++axis) {
			if (g.n[axis] == 1) continue;
			OneSidedDerivatives(g.n, h, f, axis, ext, pool, [&](std::size_t base, const double* dm, const double* dp) {
				for (int i = 0; i < n0; ++i) {
					const std::size_t c = base + std::size_t(i);
					double a = dm[i], b = dp[i];
					if (subcell && nearId[c] >= 0) {
						// One-sided differences to the interface (phi = 0 there).
						const Near& d = nearData[std::size_t(nearId[c])];
						if (d.dm[axis] > 0) a = f[c]/d.dm[axis];
						if (d.dp[axis] > 0) b = -f[c]/d.dp[axis];
					}
					g2[c] += sgn[c] > 0 ? std::max(Sq(std::max(a, 0.0)), Sq(std::min(b, 0.0)))
						: std::max(Sq(std::min(a, 0.0)), Sq(std::max(b, 0.0)));
				}
			}, barrier);
		}
		ForRows(pool, n, 4096, [&](std::size_t a, std::size_t b) {
			for (std::size_t c = a; c < b; ++c) {
				if (frozen && nearId[c] >= 0)
					rhs[c] = -((phi0[c] < 0 ? -std::fabs(f[c]) : std::fabs(f[c])) - nearData[std::size_t(nearId[c])].dist)/h;
				else rhs[c] = -sgn[c]*(std::sqrt(g2[c]) - 1);
			}
		});
	};
	for (int it = 0; it < iterations; ++it) {
		L(phi);
		ForRows(pool, n, 4096, [&](std::size_t a, std::size_t b) { for (std::size_t c = a; c < b; ++c) p1[c] = phi[c] + dtau[c]*rhs[c]; });
		L(p1);
		ForRows(pool, n, 4096, [&](std::size_t a, std::size_t b) {
			for (std::size_t c = a; c < b; ++c) p2[c] = 0.75*phi[c] + 0.25*(p1[c] + dtau[c]*rhs[c]);
		});
		L(p2);
		ForRows(pool, n, 4096, [&](std::size_t a, std::size_t b) {
			for (std::size_t c = a; c < b; ++c) phi[c] = phi[c]/3 + 2.0/3.0*(p2[c] + dtau[c]*rhs[c]);
		});
	}
}

double GradientDeviation(const Grid& g, const std::vector<double>& phi, double band, Pool* pool, const LevelSetBarrier* barrier)
{
	if (barrier && barrier->Empty()) barrier = nullptr;
	const std::size_t rows = Rows(g.n);
	return MaxRows(pool, rows, RowGrain(g.n[0]), [&](std::size_t r0, std::size_t r1) {
		double m = 0;
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(g.n[1])), k = int(r/std::size_t(g.n[1]));
			for (int i = 0; i < g.n[0]; ++i)
				if (std::fabs(phi[g.C(i, j, k)]) < band*g.h) m = std::max(m, std::fabs(GradientNorm(g, phi, i, j, k, barrier) - 1));
		}
		return m;
	});
}

double LiquidVolume(const Grid& g, const std::vector<double>& phi, const std::vector<double>* open, Pool* pool)
{
	const double eps = 1.5*g.h, cell = g.h*g.h*g.h;
	return cell*SumRows(pool, phi.size(), 4096, [&](std::size_t a, std::size_t b) {
		double s = 0;
		for (std::size_t c = a; c < b; ++c) s += (1 - Heaviside(phi[c], eps))*(open ? (*open)[c] : 1.0);
		return s;
	});
}

double InterfaceArea(const Grid& g, const std::vector<double>& phi, const std::vector<double>* open, Pool* pool,
	const std::vector<std::uint8_t>* exclude)
{
	const double eps = 1.5*g.h, cell = g.h*g.h*g.h;
	const std::size_t rows = Rows(g.n);
	return cell*SumRows(pool, rows, RowGrain(g.n[0]), [&](std::size_t r0, std::size_t r1) {
		double s = 0;
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(g.n[1])), k = int(r/std::size_t(g.n[1]));
			for (int i = 0; i < g.n[0]; ++i) {
				const std::size_t c = g.C(i, j, k);
				if (exclude && (*exclude)[c]) continue;
				const double d = Delta(phi[c], eps);
				if (d == 0) continue;
				s += d*GradientNorm(g, phi, i, j, k)*(open ? (*open)[c] : 1.0);
			}
		}
		return s;
	});
}

double CorrectVolume(const Grid& g, std::vector<double>& phi, double target, const std::vector<double>* open, Pool* pool)
{
	const double v = LiquidVolume(g, phi, open, pool), area = InterfaceArea(g, phi, open, pool);
	if (!(area > 0)) return 0;
	const double shift = (v - target)/area;
	ForRows(pool, phi.size(), 4096, [&](std::size_t a, std::size_t b) { for (std::size_t c = a; c < b; ++c) phi[c] += shift; });
	return shift;
}

void Curvature(const Grid& g, const std::vector<double>& phi, std::vector<double>& kappa, Pool* pool)
{
	const auto& n = g.n;
	kappa.assign(g.Cells(), 0.0);
	int dims = 0;
	for (int a = 0; a < 3; ++a) dims += n[a] > 1;
	const double limit = double(std::max(1, dims - 1))/g.h;
	const double h = g.h, ih2 = 1/(h*h);
	ForRows(pool, Rows(n), RowGrain(n[0]), [&](std::size_t r0, std::size_t r1) {
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(n[1])), k = int(r/std::size_t(n[1]));
			for (int i = 0; i < n[0]; ++i) {
				// phi at offsets (-1, 0, +1) along each axis with clamped (mirrored) indices.
				auto at = [&](int di, int dj, int dk) {
					const int q[3] = {std::min(std::max(i + di, 0), n[0] - 1), std::min(std::max(j + dj, 0), n[1] - 1),
						std::min(std::max(k + dk, 0), n[2] - 1)};
					return phi[g.C(q[0], q[1], q[2])];
				};
				const double c = phi[g.C(i, j, k)];
				double d1[3], d2[3], dm[3];   // first, second, mixed (xy, xz, yz)
				const int e[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
				for (int a = 0; a < 3; ++a) {
					if (n[a] == 1) { d1[a] = d2[a] = 0; continue; }
					const double p = at(e[a][0], e[a][1], e[a][2]), m = at(-e[a][0], -e[a][1], -e[a][2]);
					d1[a] = (p - m)/(2*h);
					d2[a] = (p - 2*c + m)*ih2;
				}
				const int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
				for (int q = 0; q < 3; ++q) {
					const int a = pairs[q][0], b = pairs[q][1];
					if (n[a] == 1 || n[b] == 1) { dm[q] = 0; continue; }
					int pp[3] = {0, 0, 0}, pm[3] = {0, 0, 0}, mp[3] = {0, 0, 0}, mm[3] = {0, 0, 0};
					pp[a] = 1; pp[b] = 1; pm[a] = 1; pm[b] = -1; mp[a] = -1; mp[b] = 1; mm[a] = -1; mm[b] = -1;
					dm[q] = (at(pp[0], pp[1], pp[2]) - at(pm[0], pm[1], pm[2]) - at(mp[0], mp[1], mp[2]) + at(mm[0], mm[1], mm[2]))*0.25*ih2;
				}
				const double gx = d1[0], gy = d1[1], gz = d1[2];
				const double g2 = gx*gx + gy*gy + gz*gz;
				const double gn = std::sqrt(g2);
				if (!(gn >= 1e-6)) continue;
				const double num = gx*gx*(d2[1] + d2[2]) + gy*gy*(d2[0] + d2[2]) + gz*gz*(d2[0] + d2[1])
					- 2*gx*gy*dm[0] - 2*gx*gz*dm[1] - 2*gy*gz*dm[2];
				const double kv = num/(g2*gn);
				kappa[g.C(i, j, k)] = std::min(limit, std::max(-limit, kv));
			}
		}
	});
}

// ------------------------------------------------------------------ marker particles

void MarkerParticles::SeedCells(const Grid& g, const std::vector<double>& phi, const std::vector<std::size_t>& cells,
	const std::vector<int>& counts, std::uint64_t step, std::vector<Particle>& out) const
{
	const double h = g.h, bmin = options_.rmin*h, bmax = options_.band*h;
	for (std::size_t q = 0; q < cells.size(); ++q) {
		const std::size_t c = cells[q];
		const int i = int(c % std::size_t(g.n[0])), j = int((c/std::size_t(g.n[0])) % std::size_t(g.n[1]));
		const int k = int(c/(std::size_t(g.n[0])*std::size_t(g.n[1])));
		for (int m = 0; m < counts[q]; ++m) {
			std::uint64_t state = Mix(Mix(std::uint64_t(c)) ^ Mix(step*0x51ed27ull + std::uint64_t(m)));
			V3 x = g.Node(i, j, k);
			for (int a = 0; a < 3; ++a) x[a] += (g.n[a] == 1 ? 0.5 : Uniform(state))*h;
			const double sign = SampleCells(g, phi, x) < 0 ? -1.0 : 1.0;
			const double goal = sign*(bmin + Uniform(state)*(bmax - bmin));
			bool ok = false;
			double lambda = 1;
			for (int it = 0; it < 15; ++it) {
				const double f = SampleCells(g, phi, x);
				if (sign*f >= bmin && sign*f <= bmax && InDomain(g, x)) { ok = true; break; }
				V3 n = Gradient(g, phi, x);
				const double len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
				if (!(len > 0)) break;
				for (int a = 0; a < 3; ++a) x[a] += lambda*(goal - f)*n[a]/len;
				lambda *= 0.8;
			}
			if (!ok) continue;
			const double f = SampleCells(g, phi, x);
			Particle p;
			p.x = x;
			p.sign = sign;
			p.radius = std::min(std::max(sign*f, options_.rmin*h), options_.rmax*h);
			p.id = 0;
			out.push_back(p);
		}
	}
}

void MarkerParticles::Seed(const Grid& g, const std::vector<double>& phi, const ParticleOptions& options, std::uint64_t step, Pool* pool)
{
	(void)pool;
	options_ = options;
	particles_.clear();
	nextId_ = 1;
	std::vector<std::size_t> cells;
	for (std::size_t c = 0; c < phi.size(); ++c) if (std::fabs(phi[c]) < options.band*g.h) cells.push_back(c);
	std::vector<int> counts(cells.size(), options.perCell);
	std::vector<Particle> seeded;
	SeedCells(g, phi, cells, counts, step, seeded);
	for (auto& p : seeded) { p.id = nextId_++; particles_.push_back(p); }
}

void MarkerParticles::Advect(const Grid& g, const std::function<const MacField&(double)>& velocityAt, double t, double dt, Pool* pool)
{
	const std::size_t n = particles_.size();
	std::vector<V3> x1(n), x2(n);
	{
		const MacField& u = velocityAt(t);
		ForRows(pool, n, 1024, [&](std::size_t a, std::size_t b) {
			for (std::size_t p = a; p < b; ++p) {
				const V3 v = SampleVelocity(g, u, particles_[p].x);
				for (int d = 0; d < 3; ++d) x1[p][d] = particles_[p].x[d] + dt*v[d];
			}
		});
	}
	{
		const MacField& u = velocityAt(t + dt);
		ForRows(pool, n, 1024, [&](std::size_t a, std::size_t b) {
			for (std::size_t p = a; p < b; ++p) {
				const V3 v = SampleVelocity(g, u, x1[p]);
				for (int d = 0; d < 3; ++d) x2[p][d] = 0.75*particles_[p].x[d] + 0.25*(x1[p][d] + dt*v[d]);
			}
		});
	}
	{
		const MacField& u = velocityAt(t + 0.5*dt);
		ForRows(pool, n, 1024, [&](std::size_t a, std::size_t b) {
			for (std::size_t p = a; p < b; ++p) {
				const V3 v = SampleVelocity(g, u, x2[p]);
				for (int d = 0; d < 3; ++d) particles_[p].x[d] = particles_[p].x[d]/3 + 2.0/3.0*(x2[p][d] + dt*v[d]);
			}
		});
	}
}

std::size_t MarkerParticles::Correct(const Grid& g, std::vector<double>& phi, Pool* pool) const
{
	const std::size_t n = particles_.size();
	std::vector<std::uint8_t> escaped(n, 0);
	ForRows(pool, n, 1024, [&](std::size_t a, std::size_t b) {
		for (std::size_t p = a; p < b; ++p) {
			const Particle& q = particles_[p];
			escaped[p] = q.sign*SampleCells(g, phi, q.x) < -q.radius;
		}
	});
	std::vector<double> plus, minus;
	std::vector<std::size_t> touched;
	std::size_t count = 0;
	for (std::size_t p = 0; p < n; ++p) {
		if (!escaped[p]) continue;
		if (plus.empty()) { plus = phi; minus = phi; }
		++count;
		const Particle& q = particles_[p];
		int i0[3];
		for (int a = 0; a < 3; ++a) {
			const double s = (q.x[a] - g.origin[a])/g.h - 0.5;
			i0[a] = std::max(0, std::min(g.n[a] - 1, static_cast<int>(std::floor(s))));
		}
		for (int m = 0; m < 8; ++m) {
			int c3[3] = {i0[0] + (m & 1), i0[1] + ((m >> 1) & 1), i0[2] + ((m >> 2) & 1)};
			bool inside = true;
			for (int a = 0; a < 3; ++a) if (c3[a] >= g.n[a]) inside = false;
			if (!inside) continue;
			const V3 xc = g.CellCentre(c3[0], c3[1], c3[2]);
			const double d = std::sqrt(Sq(xc[0] - q.x[0]) + Sq(xc[1] - q.x[1]) + Sq(xc[2] - q.x[2]));
			const double phip = q.sign*(q.radius - d);
			const std::size_t c = g.C(c3[0], c3[1], c3[2]);
			if (q.sign > 0) { if (phip > plus[c]) plus[c] = phip; }
			else if (phip < minus[c]) minus[c] = phip;
			touched.push_back(c);
		}
	}
	for (std::size_t c : touched) phi[c] = std::fabs(plus[c]) <= std::fabs(minus[c]) ? plus[c] : minus[c];
	return count;
}

void MarkerParticles::AdjustRadii(const Grid& g, const std::vector<double>& phi, Pool* pool)
{
	const std::size_t n = particles_.size();
	std::vector<std::uint8_t> keep(n, 1);
	const double rmin = options_.rmin*g.h, rmax = options_.rmax*g.h;
	ForRows(pool, n, 1024, [&](std::size_t a, std::size_t b) {
		for (std::size_t p = a; p < b; ++p) {
			Particle& q = particles_[p];
			if (!InDomain(g, q.x)) { keep[p] = 0; continue; }
			const double s = q.sign*SampleCells(g, phi, q.x);
			if (s < -1.5*q.radius) { keep[p] = 0; continue; }
			q.radius = std::min(std::max(s, rmin), rmax);
		}
	});
	std::size_t w = 0;
	for (std::size_t p = 0; p < n; ++p) if (keep[p]) particles_[w++] = particles_[p];
	particles_.resize(w);
}

void MarkerParticles::Reseed(const Grid& g, const std::vector<double>& phi, std::uint64_t step, Pool* pool)
{
	(void)pool;
	const double band = options_.band*g.h;
	std::vector<int> count(phi.size(), 0);
	std::vector<std::uint8_t> keep(particles_.size(), 1);
	for (std::size_t p = 0; p < particles_.size(); ++p) {
		const Particle& q = particles_[p];
		int c3[3];
		for (int a = 0; a < 3; ++a)
			c3[a] = std::max(0, std::min(g.n[a] - 1, static_cast<int>(std::floor((q.x[a] - g.origin[a])/g.h))));
		const std::size_t c = g.C(c3[0], c3[1], c3[2]);
		const double f = SampleCells(g, phi, q.x);
		const bool escapedNow = q.sign*f < -q.radius;
		if (!escapedNow && std::fabs(phi[c]) > band + g.h) { keep[p] = 0; continue; }
		++count[c];
	}
	std::size_t w = 0;
	for (std::size_t p = 0; p < particles_.size(); ++p) if (keep[p]) particles_[w++] = particles_[p];
	particles_.resize(w);
	std::vector<std::size_t> cells;
	std::vector<int> add;
	for (std::size_t c = 0; c < phi.size(); ++c)
		if (std::fabs(phi[c]) < band && count[c] < options_.perCell/2) { cells.push_back(c); add.push_back(options_.perCell - count[c]); }
	std::vector<Particle> seeded;
	SeedCells(g, phi, cells, add, step, seeded);
	for (auto& p : seeded) { p.id = nextId_++; particles_.push_back(p); }
}

} } }
