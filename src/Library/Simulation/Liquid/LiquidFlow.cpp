//////////////////////////////////////////////////////////////////////
//
//  LiquidFlow.cpp - see LiquidFlow.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidFlow.h"
#include "LiquidLevelSet.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace RISE { namespace Simulation { namespace Liquid {

namespace {
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::size_t Index(const std::array<int,3>& d, int i, int j, int k)
{
	return std::size_t(i) + std::size_t(d[0])*(std::size_t(j) + std::size_t(d[1])*std::size_t(k));
}
std::size_t RowsOf(const std::array<int,3>& d) { return std::size_t(d[1])*std::size_t(d[2]); }

// ------------------------------------------------------------------ semi-Lagrangian helpers

V3 Backtrace(const Grid& g, const MacField& u, const V3& x, double dt)
{
	const V3 v = SampleVelocity(g, u, x);
	const V3 xm{{x[0] - 0.5*dt*v[0], x[1] - 0.5*dt*v[1], x[2] - 0.5*dt*v[2]}};
	const V3 v2 = SampleVelocity(g, u, xm);
	return {{x[0] - dt*v2[0], x[1] - dt*v2[1], x[2] - dt*v2[2]}};
}

// Range of the eight samples the trilinear interpolation of face component a uses at x.
void StencilRange(const Grid& g, int a, const std::vector<double>& f, const V3& x, double& lo, double& hi)
{
	const auto d = g.FaceDims(a);
	int i0[3];
	for (int b = 0; b < 3; ++b) {
		const double s = std::min(std::max((x[b] - g.origin[b])/g.h - (b == a ? 0.0 : 0.5), 0.0), double(d[b] - 1));
		int i = static_cast<int>(std::floor(s));
		if (i >= d[b] - 1) i = std::max(0, d[b] - 2);
		i0[b] = i;
	}
	lo = 1e300; hi = -1e300;
	for (int m = 0; m < 8; ++m) {
		const int i = std::min(i0[0] + (m & 1), d[0] - 1), j = std::min(i0[1] + ((m >> 1) & 1), d[1] - 1),
			k = std::min(i0[2] + ((m >> 2) & 1), d[2] - 1);
		const double v = f[Index(d, i, j, k)];
		lo = std::min(lo, v); hi = std::max(hi, v);
	}
}

// Advecting velocity of component a's faces: the face's own component and
// the average of the four neighbouring faces of each other component.
void FaceAdvectingVelocity(const Grid& g, const MacField& u, int a, std::array<std::vector<double>,3>& c, Pool* pool)
{
	const auto d = g.FaceDims(a);
	const std::size_t count = g.Faces(a);
	for (int b = 0; b < 3; ++b) c[b].resize(count);
	c[a] = u.u[a];
	for (int b = 0; b < 3; ++b) {
		if (b == a) continue;
		const auto db = g.FaceDims(b);
		ForRows(pool, RowsOf(d), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
				for (int i = 0; i < d[0]; ++i) {
					int idx[3] = {i, j, k};
					double s = 0;
					for (int m = 0; m < 4; ++m) {
						int q[3] = {idx[0], idx[1], idx[2]};
						// along a: the two cells sharing the face; along b: the two faces of that cell
						q[a] = std::min(std::max(idx[a] - 1 + (m & 1), 0), g.n[a] - 1);
						q[b] = idx[b] + ((m >> 1) & 1);
						s += u.u[b][Index(db, q[0], q[1], q[2])];
					}
					c[b][Index(d, i, j, k)] = 0.25*s;
				}
			}
		});
	}
}

// ------------------------------------------------------------------ multigrid

struct Level {
	std::array<int,3> n{{1, 1, 1}};
	std::vector<std::uint8_t> active;
	std::vector<double> diag;
	std::array<std::vector<double>,3> w;     // coupling c <-> c + e_a
	std::array<std::vector<double>,3> open;  // open fraction of the face c | c + e_a (for coarsening)
	std::vector<double> top;                 // open fraction of the top boundary face (open top)
	std::vector<double> b, x, r;
	std::vector<int> lo, hi;                 // per row: the first and one past the last active cell
	std::size_t Cells() const { return std::size_t(n[0])*std::size_t(n[1])*std::size_t(n[2]); }
	void Ranges() {
		const std::size_t rows = std::size_t(n[1])*std::size_t(n[2]);
		lo.assign(rows, 0);
		hi.assign(rows, 0);
		for (std::size_t r = 0; r < rows; ++r) {
			const std::size_t base = std::size_t(n[0])*r;
			int a = 0, b2 = n[0];
			while (a < n[0] && !active[base + std::size_t(a)]) ++a;
			while (b2 > a && !active[base + std::size_t(b2 - 1)]) --b2;
			lo[r] = a;
			hi[r] = b2;
		}
	}
};

// f(first, end) over the active span of every row of L, in fixed row chunks.
template <class F>
void ForActive(Pool* pool, const Level& L, F&& f)
{
	const std::size_t n0 = std::size_t(L.n[0]);
	ForRows(pool, L.lo.size(), RowGrain(L.n[0]), [&](std::size_t r0, std::size_t r1) {
		for (std::size_t r = r0; r < r1; ++r) if (L.lo[r] < L.hi[r]) f(n0*r + std::size_t(L.lo[r]), n0*r + std::size_t(L.hi[r]));
	});
}
template <class F>
double SumActive(Pool* pool, const Level& L, F&& f)
{
	const std::size_t n0 = std::size_t(L.n[0]);
	return SumRows(pool, L.lo.size(), RowGrain(L.n[0]), [&](std::size_t r0, std::size_t r1) {
		double s = 0;
		for (std::size_t r = r0; r < r1; ++r) if (L.lo[r] < L.hi[r]) s += f(n0*r + std::size_t(L.lo[r]), n0*r + std::size_t(L.hi[r]));
		return s;
	});
}
template <class F>
double MaxActive(Pool* pool, const Level& L, F&& f)
{
	const std::size_t n0 = std::size_t(L.n[0]);
	return MaxRows(pool, L.lo.size(), RowGrain(L.n[0]), [&](std::size_t r0, std::size_t r1) {
		double m = 0;
		for (std::size_t r = r0; r < r1; ++r) if (L.lo[r] < L.hi[r]) m = std::max(m, f(n0*r + std::size_t(L.lo[r]), n0*r + std::size_t(L.hi[r])));
		return m;
	});
}

// Prolongation weights (cell-centred trilinear) from coarse cells to fine
// index f along one axis; nC coarse cells; `thin` axes are not coarsened.
int CoarseWeights(int f, int nC, bool coarsened, int (&cs)[2], double (&ws)[2])
{
	if (!coarsened) { cs[0] = f; ws[0] = 1; return 1; }
	const int I = f/2;
	const int other = (f & 1) ? I + 1 : I - 1;
	if (other < 0 || other >= nC) { cs[0] = I; ws[0] = 1; return 1; }
	cs[0] = I; ws[0] = 0.75; cs[1] = other; ws[1] = 0.25;
	return 2;
}

class Multigrid {
public:
	std::vector<Level> levels;
	int smoothing = 2;
	Pool* pool = nullptr;

	void Apply(const Level& L, const std::vector<double>& x, std::vector<double>& y) const
	{
		y.resize(L.Cells());
		const auto& n = L.n;
		const std::size_t sx = 1, sy = std::size_t(n[0]), sz = std::size_t(n[0])*std::size_t(n[1]);
		ForRows(PoolFor(L), RowsOf(n), RowGrain(n[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(n[1])), k = int(r/std::size_t(n[1]));
				for (int i = L.lo[r]; i < L.hi[r]; ++i) {
					const std::size_t c = Index(n, i, j, k);
					if (!L.active[c]) { y[c] = 0; continue; }
					double s = L.diag[c]*x[c];
					if (i + 1 < n[0]) s -= L.w[0][c]*x[c + sx];
					if (i > 0) s -= L.w[0][c - sx]*x[c - sx];
					if (j + 1 < n[1]) s -= L.w[1][c]*x[c + sy];
					if (j > 0) s -= L.w[1][c - sy]*x[c - sy];
					if (k + 1 < n[2]) s -= L.w[2][c]*x[c + sz];
					if (k > 0) s -= L.w[2][c - sz]*x[c - sz];
					y[c] = s;
				}
			}
		});
	}

	void HalfSweep(Level& L, int colour) const
	{
		const auto& n = L.n;
		const std::size_t sx = 1, sy = std::size_t(n[0]), sz = std::size_t(n[0])*std::size_t(n[1]);
		ForRows(PoolFor(L), RowsOf(n), RowGrain(n[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(n[1])), k = int(r/std::size_t(n[1]));
				for (int i = L.lo[r] + ((L.lo[r] + j + k + colour) & 1); i < L.hi[r]; i += 2) {
					const std::size_t c = Index(n, i, j, k);
					if (!L.active[c] || !(L.diag[c] > 0)) continue;
					double s = L.b[c];
					if (i + 1 < n[0]) s += L.w[0][c]*L.x[c + sx];
					if (i > 0) s += L.w[0][c - sx]*L.x[c - sx];
					if (j + 1 < n[1]) s += L.w[1][c]*L.x[c + sy];
					if (j > 0) s += L.w[1][c - sy]*L.x[c - sy];
					if (k + 1 < n[2]) s += L.w[2][c]*L.x[c + sz];
					if (k > 0) s += L.w[2][c - sz]*L.x[c - sz];
					L.x[c] = s/L.diag[c];
				}
			}
		});
	}

	// Per-axis prolongation weights P(f, C) as lists for each coarse index C.
	static void FineLists(int nF, int nC, bool coarsened, std::vector<std::vector<std::pair<int,double>>>& lists)
	{
		lists.assign(std::size_t(nC), {});
		for (int f = 0; f < nF; ++f) {
			int cs[2];
			double ws[2];
			const int m = CoarseWeights(f, nC, coarsened, cs, ws);
			for (int q = 0; q < m; ++q) {
				auto& l = lists[std::size_t(cs[q])];
				if (!l.empty() && l.back().first == f) l.back().second += ws[q];
				else l.emplace_back(f, ws[q]);
			}
		}
	}

	// b_C = scale * sum_f P(f, C) r_f.
	void Restrict(const Level& F, Level& C) const
	{
		const bool co[3] = {C.n[0] != F.n[0], C.n[1] != F.n[1], C.n[2] != F.n[2]};
		const int dims = int(co[0]) + int(co[1]) + int(co[2]);
		const double scale = 1.0/std::pow(2.0, double(dims - 2));
		std::vector<std::vector<std::pair<int,double>>> lists[3];
		for (int a = 0; a < 3; ++a) FineLists(F.n[a], C.n[a], co[a], lists[a]);
		C.b.assign(C.Cells(), 0.0);
		ForRows(PoolFor(C), RowsOf(C.n), RowGrain(C.n[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int J = int(r % std::size_t(C.n[1])), K = int(r/std::size_t(C.n[1]));
				for (int I = C.lo[r]; I < C.hi[r]; ++I) {
					const std::size_t cc = Index(C.n, I, J, K);
					if (!C.active[cc]) continue;
					double s = 0;
					for (const auto& z : lists[2][std::size_t(K)]) for (const auto& y : lists[1][std::size_t(J)]) {
						const double wyz = y.second*z.second;
						const std::size_t row = Index(F.n, 0, y.first, z.first);
						for (const auto& x : lists[0][std::size_t(I)]) {
							const std::size_t fc = row + std::size_t(x.first);
							if (F.active[fc]) s += x.second*wyz*F.r[fc];
						}
					}
					C.b[cc] = scale*s;
				}
			}
		});
	}

	// Levels below this many cells run on the caller (the pool's joins cost more).
	Pool* PoolFor(const Level& L) const { return L.Cells() >= 32768 ? pool : nullptr; }

	// x_f += sum_C P(f, C) x_C.
	void Prolong(const Level& C, Level& F) const
	{
		const bool co[3] = {C.n[0] != F.n[0], C.n[1] != F.n[1], C.n[2] != F.n[2]};
		ForRows(PoolFor(F), RowsOf(F.n), RowGrain(F.n[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(F.n[1])), k = int(r/std::size_t(F.n[1]));
				for (int i = F.lo[r]; i < F.hi[r]; ++i) {
					const std::size_t fc = Index(F.n, i, j, k);
					if (!F.active[fc]) continue;
					int cs[3][2];
					double ws[3][2];
					int m[3];
					const int f[3] = {i, j, k};
					for (int a = 0; a < 3; ++a) m[a] = CoarseWeights(f[a], C.n[a], co[a], cs[a], ws[a]);
					double s = 0;
					for (int c = 0; c < m[2]; ++c) for (int b = 0; b < m[1]; ++b) for (int a = 0; a < m[0]; ++a)
						s += ws[0][a]*ws[1][b]*ws[2][c]*C.x[Index(C.n, cs[0][a], cs[1][b], cs[2][c])];
					F.x[fc] += s;
				}
			}
		});
	}

	void Cycle(std::size_t l)
	{
		Level& L = levels[l];
		if (L.x.size() != L.Cells()) L.x.assign(L.Cells(), 0.0);
		else ForActive(PoolFor(L), L, [&](std::size_t a, std::size_t b) { std::fill(L.x.begin() + std::ptrdiff_t(a), L.x.begin() + std::ptrdiff_t(b), 0.0); });
		if (l + 1 == levels.size()) {
			for (int s = 0; s < 30; ++s) { HalfSweep(L, 0); HalfSweep(L, 1); }
			for (int s = 0; s < 30; ++s) { HalfSweep(L, 1); HalfSweep(L, 0); }
			return;
		}
		for (int s = 0; s < smoothing; ++s) { HalfSweep(L, 0); HalfSweep(L, 1); }
		Apply(L, L.x, L.r);
		ForActive(PoolFor(L), L, [&](std::size_t a, std::size_t b) { for (std::size_t c = a; c < b; ++c) L.r[c] = L.b[c] - L.r[c]; });
		Restrict(L, levels[l + 1]);
		Cycle(l + 1);
		Prolong(levels[l + 1], L);
		for (int s = 0; s < smoothing; ++s) { HalfSweep(L, 1); HalfSweep(L, 0); }
	}

	//! z = M^-1 r.
	void Precondition(const std::vector<double>& r, std::vector<double>& z)
	{
		Level& L = levels[0];
		if (L.b.size() != L.Cells()) L.b.assign(L.Cells(), 0.0);
		ForActive(pool, L, [&](std::size_t a, std::size_t b) { std::copy(r.begin() + std::ptrdiff_t(a), r.begin() + std::ptrdiff_t(b), L.b.begin() + std::ptrdiff_t(a)); });
		Cycle(0);
		if (z.size() != L.Cells()) z.assign(L.Cells(), 0.0);
		ForActive(pool, L, [&](std::size_t a, std::size_t b) { std::copy(L.x.begin() + std::ptrdiff_t(a), L.x.begin() + std::ptrdiff_t(b), z.begin() + std::ptrdiff_t(a)); });
	}

	// Coarse level from the finest of `levels` (back()).
	void Coarsen(bool openTop)
	{
		const Level& F = levels.back();
		Level C;
		bool co[3];
		for (int a = 0; a < 3; ++a) { co[a] = F.n[a] > 1; C.n[a] = co[a] ? (F.n[a] + 1)/2 : 1; }
		const std::size_t nc = C.Cells();
		C.active.assign(nc, 0);
		C.diag.assign(nc, 0.0);
		for (int a = 0; a < 3; ++a) { C.w[a].assign(nc, 0.0); C.open[a].assign(nc, 0.0); }
		if (openTop) C.top.assign(nc, 0.0);
		for (int k = 0; k < F.n[2]; ++k) for (int j = 0; j < F.n[1]; ++j) for (int i = 0; i < F.n[0]; ++i)
			if (F.active[Index(F.n, i, j, k)]) C.active[Index(C.n, co[0] ? i/2 : i, co[1] ? j/2 : j, co[2] ? k/2 : k)] = 1;
		for (int K = 0; K < C.n[2]; ++K) for (int J = 0; J < C.n[1]; ++J) for (int I = 0; I < C.n[0]; ++I) {
			const std::size_t cc = Index(C.n, I, J, K);
			const int Cidx[3] = {I, J, K};
			for (int a = 0; a < 3; ++a) {
				if (Cidx[a] + 1 >= C.n[a]) continue;
				// Fine faces on the coarse face: fine index along a = the last child of C.
				const int fa = co[a] ? 2*Cidx[a] + 1 : Cidx[a];
				double s = 0;
				int m = 0;
				const int b = (a + 1) % 3, c = (a + 2) % 3;
				for (int tc = 0; tc < (co[c] ? 2 : 1); ++tc) for (int tb = 0; tb < (co[b] ? 2 : 1); ++tb) {
					int f[3];
					f[a] = fa;
					f[b] = co[b] ? 2*Cidx[b] + tb : Cidx[b];
					f[c] = co[c] ? 2*Cidx[c] + tc : Cidx[c];
					if (f[b] >= F.n[b] || f[c] >= F.n[c] || f[a] + 1 >= F.n[a]) continue;
					s += F.open[a][Index(F.n, f[0], f[1], f[2])];
					++m;
				}
				C.open[a][cc] = m ? s/m : 0.0;
			}
			if (openTop && J == C.n[1] - 1) {
				const int fj = F.n[1] - 1;
				double s = 0;
				int m = 0;
				for (int tk = 0; tk < (co[2] ? 2 : 1); ++tk) for (int ti = 0; ti < (co[0] ? 2 : 1); ++ti) {
					const int fi = co[0] ? 2*I + ti : I, fk = co[2] ? 2*K + tk : K;
					if (fi >= F.n[0] || fk >= F.n[2]) continue;
					s += F.top[Index(F.n, fi, fj, fk)];
					++m;
				}
				C.top[cc] = m ? s/m : 0.0;
			}
		}
		// Rediscretised operator: open fractions between active cells, p = 0 at the
		// centre of an inactive neighbour across an open face, p = 0 on an open top.
		const std::size_t strides[3] = {1, std::size_t(C.n[0]), std::size_t(C.n[0])*std::size_t(C.n[1])};
		for (int K = 0; K < C.n[2]; ++K) for (int J = 0; J < C.n[1]; ++J) for (int I = 0; I < C.n[0]; ++I) {
			const std::size_t cc = Index(C.n, I, J, K);
			const int Cidx[3] = {I, J, K};
			for (int a = 0; a < 3; ++a) {
				if (Cidx[a] + 1 >= C.n[a]) continue;
				const std::size_t nb = cc + strides[a];
				const double A = C.open[a][cc];
				if (!(A > 0)) continue;
				if (C.active[cc] && C.active[nb]) { C.w[a][cc] = A; C.diag[cc] += A; C.diag[nb] += A; }
				else if (C.active[cc]) C.diag[cc] += A;
				else if (C.active[nb]) C.diag[nb] += A;
			}
			if (openTop && J == C.n[1] - 1 && C.active[cc]) C.diag[cc] += 2*C.top[cc];
		}
		C.Ranges();
		levels.push_back(std::move(C));
	}
};

std::size_t FaceRows(const Grid& g, int a) { return RowsOf(g.FaceDims(a)); }
} // namespace

// ------------------------------------------------------------------ extrapolation

void ExtrapolateVelocity(const Grid& g, MacField& u, FaceMask& valid, int layers, int linearLayers, Pool* pool)
{
	for (int a = 0; a < 3; ++a) {
		const auto d = g.FaceDims(a);
		std::vector<std::uint8_t>& known = valid[a];
		std::vector<std::uint8_t> next;
		std::vector<double>& f = u.u[a];
		for (int layer = 0; layer < layers; ++layer) {
			next = known;
			ForRows(pool, FaceRows(g, a), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
				for (std::size_t r = r0; r < r1; ++r) {
					const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
					for (int i = 0; i < d[0]; ++i) {
						const std::size_t c = Index(d, i, j, k);
						if (known[c]) continue;
						const int idx[3] = {i, j, k};
						double s = 0;
						int m = 0;
						for (int b = 0; b < 3; ++b) for (int dir = -1; dir <= 1; dir += 2) {
							int q[3] = {idx[0], idx[1], idx[2]};
							q[b] += dir;
							if (q[b] < 0 || q[b] >= d[b]) continue;
							const std::size_t nb = Index(d, q[0], q[1], q[2]);
							if (!known[nb]) continue;
							// Linear along the axis in the first `linearLayers` layers when the
							// next face is known too; else the neighbour's value.
							q[b] += dir;
							const bool two = layer < linearLayers && q[b] >= 0 && q[b] < d[b] && known[Index(d, q[0], q[1], q[2])];
							s += two ? 2*f[nb] - f[Index(d, q[0], q[1], q[2])] : f[nb];
							++m;
						}
						if (m) { f[c] = s/m; next[c] = 1; }
					}
				}
			});
			known.swap(next);
		}
		for (std::size_t c = 0; c < f.size(); ++c) if (!known[c]) f[c] = 0;
	}
}

// ------------------------------------------------------------------ momentum advection

void VelocityAdvectionRate(const Grid& g, const MacField& u, MacField& rate, Pool* pool)
{
	rate.Resize(g);
	for (int a = 0; a < 3; ++a) {
		if (g.n[a] == 1) continue;
		std::array<std::vector<double>,3> c;
		FaceAdvectingVelocity(g, u, a, c, pool);
		std::array<Extension,3> ext{{Extension::Constant, Extension::Constant, Extension::Constant}};
		ext[a] = Extension::Linear;
		const std::array<const std::vector<double>*,3> cp{{&c[0], &c[1], &c[2]}};
		UpwindAdvectionRhs(g.FaceDims(a), g.h, u.u[a], cp, ext, rate.u[a], pool);
	}
}

void LimitedAdvectionStage(const Grid& g, const MacField& u, double dt, MacField& out, Pool* pool)
{
	out.Resize(g);
	for (int a = 0; a < 3; ++a) {
		if (g.n[a] == 1) continue;
		const auto d = g.FaceDims(a);
		std::array<std::vector<double>,3> c;
		FaceAdvectingVelocity(g, u, a, c, pool);
		std::array<Extension,3> ext{{Extension::Constant, Extension::Constant, Extension::Constant}};
		ext[a] = Extension::Linear;
		const std::array<const std::vector<double>*,3> cp{{&c[0], &c[1], &c[2]}};
		std::vector<double> rate;
		UpwindAdvectionRhs(d, g.h, u.u[a], cp, ext, rate, pool);
		const std::vector<double>& f = u.u[a];
		std::vector<double>& o = out.u[a];
		const std::size_t strides[3] = {1, std::size_t(d[0]), std::size_t(d[0])*std::size_t(d[1])};
		ForRows(pool, FaceRows(g, a), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
				for (int i = 0; i < d[0]; ++i) {
					const std::size_t id = Index(d, i, j, k);
					const int idx[3] = {i, j, k};
					// The upwind cell of the face: one step against the advecting velocity
					// on each axis (none where it is zero or the step leaves the array).
					std::ptrdiff_t step[3] = {0, 0, 0};
					for (int b = 0; b < 3; ++b) {
						const double v = c[b][id];
						const int to = v > 0 ? idx[b] - 1 : v < 0 ? idx[b] + 1 : idx[b];
						if (to >= 0 && to < d[b] && to != idx[b]) step[b] = (to - idx[b])*std::ptrdiff_t(strides[b]);
					}
					double lo = f[id], hi = f[id];
					for (int m = 1; m < 8; ++m) {
						const std::ptrdiff_t off = ((m & 1) ? step[0] : 0) + ((m & 2) ? step[1] : 0) + ((m & 4) ? step[2] : 0);
						const double x = f[std::size_t(std::ptrdiff_t(id) + off)];
						lo = std::min(lo, x);
						hi = std::max(hi, x);
					}
					o[id] = std::min(hi, std::max(lo, f[id] + dt*rate[id]));
				}
			}
		});
	}
}

void AdvectFieldAlong(const Grid& g, const MacField& f, const MacField& v, double dt, MomentumScheme scheme, MacField& out, Pool* pool)
{
	out.Resize(g);
	for (int a = 0; a < 3; ++a) {
		if (g.n[a] == 1) continue;
		const auto d = g.FaceDims(a);
		std::vector<double> forward(g.Faces(a));
		ForRows(pool, FaceRows(g, a), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
				for (int i = 0; i < d[0]; ++i)
					forward[Index(d, i, j, k)] = SampleFace(g, a, f.u[a], Backtrace(g, v, g.FaceCentre(a, i, j, k), dt));
			}
		});
		if (scheme != MomentumScheme::MacCormack) { out.u[a] = std::move(forward); continue; }
		ForRows(pool, FaceRows(g, a), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
				for (int i = 0; i < d[0]; ++i) {
					const std::size_t c = Index(d, i, j, k);
					const V3 x = g.FaceCentre(a, i, j, k);
					// Backward step of the forward result: trace forward and sample it.
					const double back = SampleFace(g, a, forward, Backtrace(g, v, x, -dt));
					const double corrected = forward[c] + 0.5*(f.u[a][c] - back);
					double lo, hi;
					StencilRange(g, a, f.u[a], Backtrace(g, v, x, dt), lo, hi);
					out.u[a][c] = corrected < lo || corrected > hi ? forward[c] : corrected;
				}
			}
		});
	}
}

void AdvectVelocity(const Grid& g, const MacField& u, double dt, MomentumScheme scheme, MacField& out, Pool* pool)
{
	if (scheme != MomentumScheme::Weno5) { AdvectFieldAlong(g, u, u, dt, scheme, out, pool); return; }
	// SSP-RK3 of u_t = -(u . grad) u from limited forward-Euler stages (each
	// stage and so their convex combinations keep every component within
	// the range of its upwind values).
	out.Resize(g);
	MacField s1, s2, e;
	LimitedAdvectionStage(g, u, dt, s1, pool);
	LimitedAdvectionStage(g, s1, dt, e, pool);
	s2.Resize(g);
	for (int a = 0; a < 3; ++a) if (g.n[a] > 1)
		for (std::size_t c = 0; c < s2.u[a].size(); ++c) s2.u[a][c] = 0.75*u.u[a][c] + 0.25*e.u[a][c];
	LimitedAdvectionStage(g, s2, dt, e, pool);
	for (int a = 0; a < 3; ++a) if (g.n[a] > 1)
		for (std::size_t c = 0; c < out.u[a].size(); ++c) out.u[a][c] = u.u[a][c]/3 + 2.0/3.0*e.u[a][c];
}

// ------------------------------------------------------------------ projection

bool Project(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	const MacField* solid, const PressureOptions& options, const std::vector<IPressureCoupling*>& couplings,
	MacField& u, std::vector<double>& q, FaceMask& valid, PressureStats& stats, Pool* pool, std::string& error)
{
	const double start = Now();
	stats = PressureStats();
	const std::size_t n = g.Cells();
	const auto& N = g.n;
	const std::size_t strides[3] = {1, std::size_t(N[0]), std::size_t(N[0])*std::size_t(N[1])};
	auto faceOf = [&](int a, int i, int j, int k, int side) {   // face of cell (i,j,k) on its minus (0) / plus (1) side
		int f[3] = {i, j, k};
		f[a] += side;
		return g.F(a, f[0], f[1], f[2]);
	};
	Level fine;
	fine.n = N;
	fine.active.assign(n, 0);
	fine.diag.assign(n, 0.0);
	for (int a = 0; a < 3; ++a) { fine.w[a].assign(n, 0.0); fine.open[a].assign(n, 0.0); }
	if (options.openTop) fine.top.assign(n, 0.0);
	std::vector<std::uint8_t> dirichlet(n, 0);
	std::vector<double> b(n, 0.0);
	// Active cells: liquid with an open face.
	ForRows(pool, RowsOf(N), RowGrain(N[0]), [&](std::size_t r0, std::size_t r1) {
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(N[1])), k = int(r/std::size_t(N[1]));
			for (int i = 0; i < N[0]; ++i) {
				const std::size_t c = g.C(i, j, k);
				if (!(phi[c] < 0)) continue;
				bool open = false;
				for (int a = 0; a < 3 && !open; ++a)
					for (int s = 0; s < 2; ++s) if (fraction[a][faceOf(a, i, j, k, s)] > 0) { open = true; break; }
				fine.active[c] = open;
			}
		}
	});
	// Couplings, ghost-fluid diagonal terms and the right-hand side.
	ForRows(pool, RowsOf(N), RowGrain(N[0]), [&](std::size_t r0, std::size_t r1) {
		for (std::size_t r = r0; r < r1; ++r) {
			const int j = int(r % std::size_t(N[1])), k = int(r/std::size_t(N[1]));
			for (int i = 0; i < N[0]; ++i) {
				const std::size_t c = g.C(i, j, k);
				const int idx[3] = {i, j, k};
				for (int a = 0; a < 3; ++a) {
					if (idx[a] + 1 < N[a]) fine.open[a][c] = fraction[a][faceOf(a, i, j, k, 1)];
				}
				if (options.openTop && j == N[1] - 1) fine.top[c] = fraction[1][faceOf(1, i, j, k, 1)];
				if (!fine.active[c]) continue;
				double diag = 0, div = 0;
				bool dir = false;
				for (int a = 0; a < 3; ++a) for (int s = 0; s < 2; ++s) {
					const std::size_t f = faceOf(a, i, j, k, s);
					const double A = fraction[a][f];
					const double sign = s ? 1.0 : -1.0;
					const double us = solid ? solid->u[a][f] : 0.0;
					div += sign*(A*u.u[a][f] + (1 - A)*us);
					if (!(A > 0)) continue;
					const int nbIdx = idx[a] + (s ? 1 : -1);
					if (nbIdx < 0 || nbIdx >= N[a]) {
						// Only the open top reaches here (other boundary faces are closed).
						const double theta = std::min(0.5, std::max(options.thetaMin, -phi[c]/g.h));
						diag += A/theta;
						dir = true;
						continue;
					}
					const std::size_t nb = s ? c + strides[a] : c - strides[a];
					if (fine.active[nb]) { diag += A; if (s) fine.w[a][c] = A; continue; }
					// Air neighbour (an inactive cell across an open face has phi >= 0).
					const double theta = std::min(1.0, std::max(options.thetaMin, phi[c]/(phi[c] - phi[nb])));
					diag += A/theta;
					dir = true;
				}
				fine.diag[c] = diag;
				dirichlet[c] = dir;
				b[c] = -div;
			}
		}
	});
	for (const IPressureCoupling* coupling : couplings) coupling->AddRhs(b);
	double mark = Now();
	stats.assemble = mark - start;
	std::size_t unknowns = 0;
	for (std::size_t c = 0; c < n; ++c) unknowns += fine.active[c];
	stats.unknowns = unknowns;
	// Components without a Dirichlet condition: the operator is singular on
	// them (constant null space); b and the preconditioned residual are kept
	// at zero mean there.
	std::vector<std::vector<std::size_t>> singular;
	{
		std::vector<std::int32_t> label(n, -1);
		std::vector<std::size_t> stack, members;
		for (std::size_t c0 = 0; c0 < n; ++c0) {
			if (!fine.active[c0] || label[c0] >= 0) continue;
			bool hasDirichlet = false;
			members.clear();
			stack.assign(1, c0);
			label[c0] = 0;
			while (!stack.empty()) {
				const std::size_t c = stack.back();
				stack.pop_back();
				members.push_back(c);
				hasDirichlet = hasDirichlet || dirichlet[c];
				const int i = int(c % std::size_t(N[0])), j = int((c/std::size_t(N[0])) % std::size_t(N[1]));
				const int k = int(c/(std::size_t(N[0])*std::size_t(N[1])));
				const int idx[3] = {i, j, k};
				for (int a = 0; a < 3; ++a) {
					if (idx[a] + 1 < N[a] && fine.w[a][c] > 0 && label[c + strides[a]] < 0) { label[c + strides[a]] = 0; stack.push_back(c + strides[a]); }
					if (idx[a] > 0 && fine.w[a][c - strides[a]] > 0 && label[c - strides[a]] < 0) { label[c - strides[a]] = 0; stack.push_back(c - strides[a]); }
				}
			}
			if (!hasDirichlet && couplings.empty()) {
				std::sort(members.begin(), members.end());
				singular.push_back(members);
			}
		}
	}
	auto removeMean = [&](std::vector<double>& v) {
		for (const auto& members : singular) {
			double s = 0;
			for (std::size_t c : members) s += v[c];
			const double mean = s/double(members.size());
			for (std::size_t c : members) v[c] -= mean;
		}
	};
	removeMean(b);
	stats.components = Now() - mark;
	mark = Now();
	// Hierarchy.
	Multigrid mg;
	mg.pool = pool;
	mg.smoothing = options.smoothing;
	fine.Ranges();
	mg.levels.push_back(std::move(fine));
	while (mg.levels.back().Cells() > 512 && (mg.levels.back().n[0] > 2 || mg.levels.back().n[1] > 2 || mg.levels.back().n[2] > 2))
		mg.Coarsen(options.openTop);
	stats.levels = int(mg.levels.size());
	stats.hierarchy = Now() - mark;
	mark = Now();
	const Level& L0 = mg.levels[0];
	auto apply = [&](const std::vector<double>& x, std::vector<double>& y) {
		mg.Apply(L0, x, y);
		for (const IPressureCoupling* coupling : couplings) coupling->ApplyAdd(x, y);
	};
	auto normInf = [&](const std::vector<double>& v) {
		return MaxActive(pool, L0, [&](std::size_t a0, std::size_t a1) {
			double m = 0;
			for (std::size_t c = a0; c < a1; ++c) m = std::max(m, std::fabs(v[c]));
			return m;
		});
	};
	auto dot = [&](const std::vector<double>& x, const std::vector<double>& y) {
		return SumActive(pool, L0, [&](std::size_t a0, std::size_t a1) {
			double d = 0;
			for (std::size_t c = a0; c < a1; ++c) d += x[c]*y[c];
			return d;
		});
	};
	const bool warm = options.warmStart && q.size() == n;
	if (!warm) q.assign(n, 0.0);
	else for (std::size_t c = 0; c < n; ++c) if (!mg.levels[0].active[c]) q[c] = 0;
	const double bnorm = normInf(b);
	if (bnorm > 0) {
		std::vector<double> r = b, z(n, 0.0), p(n, 0.0), Ap(n, 0.0);
		if (warm) {
			removeMean(q);
			apply(q, Ap);
			ForActive(pool, L0, [&](std::size_t a0, std::size_t a1) { for (std::size_t c = a0; c < a1; ++c) r[c] = b[c] - Ap[c]; });
		}
		mg.Precondition(r, z);
		removeMean(z);
		ForActive(pool, L0, [&](std::size_t a0, std::size_t a1) { for (std::size_t c = a0; c < a1; ++c) p[c] = z[c]; });
		double rz = dot(r, z);
		const double target = options.tolerance*bnorm;
		double rnorm = warm ? normInf(r) : bnorm;
		int it = 0;
		while (rnorm > target && it < options.maxIterations) {
			apply(p, Ap);
			const double pAp = dot(p, Ap);
			if (!(pAp > 0)) break;
			const double alpha = rz/pAp;
			ForActive(pool, L0, [&](std::size_t a0, std::size_t a1) {
				for (std::size_t c = a0; c < a1; ++c) { q[c] += alpha*p[c]; r[c] -= alpha*Ap[c]; }
			});
			++it;
			rnorm = normInf(r);
			if (rnorm <= target) break;
			mg.Precondition(r, z);
			removeMean(z);
			const double rzNew = dot(r, z);
			const double beta = rzNew/rz;
			rz = rzNew;
			ForActive(pool, L0, [&](std::size_t a0, std::size_t a1) {
				for (std::size_t c = a0; c < a1; ++c) p[c] = z[c] + beta*p[c];
			});
		}
		stats.iterations = it;
		stats.residual = rnorm/bnorm;
		stats.converged = rnorm <= target;
	} else stats.converged = true;
	if (!stats.converged) {
		error = "pressure solve did not converge (" + std::to_string(stats.iterations) + " iterations, residual " +
			std::to_string(stats.residual) + ")";
		return false;
	}
	for (IPressureCoupling* coupling : couplings) coupling->Finish(q);
	stats.solve = Now() - mark;
	mark = Now();
	// Velocity update and validity.
	const std::vector<std::uint8_t>& active = mg.levels[0].active;
	for (int a = 0; a < 3; ++a) {
		const auto d = g.FaceDims(a);
		valid[a].assign(g.Faces(a), 0);
		ForRows(pool, FaceRows(g, a), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
				for (int i = 0; i < d[0]; ++i) {
					const std::size_t f = Index(d, i, j, k);
					const int idx[3] = {i, j, k};
					const double A = fraction[a][f];
					const bool boundary = idx[a] == 0 || idx[a] == N[a];
					if (!(A > 0)) {
						u.u[a][f] = solid ? solid->u[a][f] : 0.0;
						if (boundary) valid[a][f] = 1;
						continue;
					}
					int lo[3] = {idx[0], idx[1], idx[2]};
					--lo[a];
					const bool hasL = idx[a] > 0, hasR = idx[a] < N[a];
					const std::size_t cl = hasL ? g.C(lo[0], lo[1], lo[2]) : 0, cr = hasR ? g.C(idx[0], idx[1], idx[2]) : 0;
					const bool aL = hasL && active[cl], aR = hasR && active[cr];
					const std::uint8_t transported = A >= options.minValidFraction;
					if (aL && aR) { u.u[a][f] -= q[cr] - q[cl]; valid[a][f] = transported; }
					else if (aL) {
						const double theta = hasR ? std::min(1.0, std::max(options.thetaMin, phi[cl]/(phi[cl] - phi[cr])))
							: std::min(0.5, std::max(options.thetaMin, -phi[cl]/g.h));
						u.u[a][f] += q[cl]/theta;
						valid[a][f] = transported;
					} else if (aR) {
						const double theta = std::min(1.0, std::max(options.thetaMin, phi[cr]/(phi[cr] - phi[cl])));
						u.u[a][f] -= q[cr]/theta;
						valid[a][f] = transported;
					}
				}
			}
		});
	}
	stats.update = Now() - mark;
	stats.seconds = Now() - start;
	return true;
}

} } }
