//////////////////////////////////////////////////////////////////////
//
//  LiquidSheets.cpp - see LiquidSheets.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidSheets.h"

#include <algorithm>
#include <cmath>

namespace RISE { namespace Simulation { namespace Liquid {

namespace {
constexpr double Pi = 3.14159265358979323846;
V3 At(const std::vector<double>& p, std::uint32_t k) { return {{p[3*std::size_t(k)], p[3*std::size_t(k) + 1], p[3*std::size_t(k) + 2]}}; }
V3 Minus(const V3& a, const V3& b) { return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}}; }
V3 CrossP(const V3& a, const V3& b) { return {{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}}; }
} // namespace

void LiquidSheet::PrescribedAt(double t)
{
	V3 X, V, w;
	Quat q;
	path.PrescribedAt(t, X, q, V, w);
	const Rigid::M3 R = Rigid::QMatrix(q);
	const std::size_t n = local.size()/3;
	x.resize(3*n);
	v.resize(3*n);
	for (std::size_t k = 0; k < n; ++k) {
		const V3 r = Rigid::Apply(R, V3{{local[3*k], local[3*k + 1], local[3*k + 2]}});
		const V3 wr = Rigid::Cross(w, r);
		for (int a = 0; a < 3; ++a) {
			x[3*k + std::size_t(a)] = X[std::size_t(a)] + r[std::size_t(a)];
			v[3*k + std::size_t(a)] = V[std::size_t(a)] + wr[std::size_t(a)];
		}
	}
}

double LiquidSheet::SpeedBound() const
{
	if (motion == Motion::Prescribed) {
		double v0 = 0, a = 0, w = 0, r = 0;
		for (int k = 0; k < 3; ++k) {
			v0 += path.v0[std::size_t(k)]*path.v0[std::size_t(k)];
			a += path.amplitude[std::size_t(k)]*path.amplitude[std::size_t(k)];
			w += path.spin[std::size_t(k)]*path.spin[std::size_t(k)];
		}
		for (std::size_t k = 0; k + 2 < local.size(); k += 3)
			r = std::max(r, local[k]*local[k] + local[k + 1]*local[k + 1] + local[k + 2]*local[k + 2]);
		return std::sqrt(v0) + std::sqrt(a)*2*Pi*path.frequency + std::sqrt(w)*std::sqrt(r);
	}
	double s = 0;
	for (std::size_t k = 0; k + 2 < v.size(); k += 3) s = std::max(s, v[k]*v[k] + v[k + 1]*v[k + 1] + v[k + 2]*v[k + 2]);
	return std::sqrt(s);
}

void FindSheetCrossings(const Grid& g, const std::vector<LiquidSheet>& sheets, std::vector<SheetCrossing>& out, Pool* pool)
{
	out.clear();
	std::vector<std::pair<std::uint32_t, std::uint32_t>> work;   // (sheet, triangle)
	for (std::size_t s = 0; s < sheets.size(); ++s)
		if (!sheets[s].x.empty())
			for (std::uint32_t t = 0; 3*std::size_t(t) < sheets[s].triangles.size(); ++t) work.emplace_back(std::uint32_t(s), t);
	if (work.empty()) return;
	constexpr std::size_t Grain = 256;
	const std::size_t chunks = Cloth::Chunks(work.size(), Grain);
	std::vector<std::vector<SheetCrossing>> part(chunks);
	constexpr double Eps = 1e-10;
	Cloth::ParallelFor(pool, chunks, [&](std::size_t ch) {
		std::vector<SheetCrossing>& mine = part[ch];
		for (std::size_t wi = ch*Grain; wi < std::min(work.size(), (ch + 1)*Grain); ++wi) {
			const LiquidSheet& sh = sheets[work[wi].first];
			const std::uint32_t t = work[wi].second;
			const std::uint32_t i0 = sh.triangles[3*std::size_t(t)], i1 = sh.triangles[3*std::size_t(t) + 1], i2 = sh.triangles[3*std::size_t(t) + 2];
			const V3 p0 = At(sh.x, i0), p1 = At(sh.x, i1), p2 = At(sh.x, i2);
			const V3 e1 = Minus(p1, p0), e2 = Minus(p2, p0);
			V3 n = CrossP(e1, e2);
			const double len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
			if (!(len > 0)) continue;
			for (double& c : n) c /= len;
			const V3 v0 = At(sh.v, i0), v1 = At(sh.v, i1), v2 = At(sh.v, i2);
			for (int a = 0; a < 3; ++a) {
				if (g.n[a] < 2) continue;   // no interior faces along a one-cell axis
				const int b = (a + 1) % 3, c = (a + 2) % 3;
				const double det = e1[std::size_t(b)]*e2[std::size_t(c)] - e2[std::size_t(b)]*e1[std::size_t(c)];
				if (det == 0) continue;     // parallel to axis a: the other axes' lines cross it
				double lo[3], hi[3];
				for (int m = 0; m < 3; ++m) {
					lo[m] = std::min(p0[std::size_t(m)], std::min(p1[std::size_t(m)], p2[std::size_t(m)]));
					hi[m] = std::max(p0[std::size_t(m)], std::max(p1[std::size_t(m)], p2[std::size_t(m)]));
				}
				// The cell-centre lines in the triangle's (b, c) bounds (a little beyond: the inclusive test decides).
				auto range = [&](int m, int& first, int& last) {
					first = std::max(0, int(std::ceil((lo[m] - g.origin[std::size_t(m)])/g.h - 0.5 - 1e-9)));
					last = std::min(g.n[m] - 1, int(std::floor((hi[m] - g.origin[std::size_t(m)])/g.h - 0.5 + 1e-9)));
				};
				int j0, j1, k0, k1;
				range(b, j0, j1);
				range(c, k0, k1);
				for (int k = k0; k <= k1; ++k) for (int j = j0; j <= j1; ++j) {
					const double y = g.origin[std::size_t(b)] + (j + 0.5)*g.h, z = g.origin[std::size_t(c)] + (k + 0.5)*g.h;
					const double db = y - p0[std::size_t(b)], dc = z - p0[std::size_t(c)];
					double u = (db*e2[std::size_t(c)] - e2[std::size_t(b)]*dc)/det;
					double w = (e1[std::size_t(b)]*dc - db*e1[std::size_t(c)])/det;
					if (u < -Eps || w < -Eps || u + w > 1 + Eps) continue;
					u = std::max(0.0, u);
					w = std::max(0.0, w);
					if (u + w > 1) { const double s = 1/(u + w); u *= s; w *= s; }
					const double s = p0[std::size_t(a)] + u*e1[std::size_t(a)] + w*e2[std::size_t(a)];
					const int i = int(std::floor((s - g.origin[std::size_t(a)])/g.h - 0.5));
					if (i < 0 || i + 1 > g.n[a] - 1) continue;   // beyond the outermost cell centres: no interior face
					SheetCrossing x;
					x.axis = a;
					int idx[3];
					idx[a] = i + 1; idx[b] = j; idx[c] = k;
					x.face = g.F(a, idx[0], idx[1], idx[2]);
					x.sheet = work[wi].first;
					x.triangle = t;
					x.w[0] = 1 - u - w; x.w[1] = u; x.w[2] = w;
					x.distance = std::fabs(s - (g.origin[std::size_t(a)] + (i + 1)*g.h));
					double vn = 0;
					for (int m = 0; m < 3; ++m)
						vn += (x.w[0]*v0[std::size_t(m)] + x.w[1]*v1[std::size_t(m)] + x.w[2]*v2[std::size_t(m)])*n[std::size_t(m)];
					x.velocity = vn*n[std::size_t(a)];
					mine.push_back(x);
				}
			}
		}
	});
	for (auto& p : part) out.insert(out.end(), p.begin(), p.end());
	std::sort(out.begin(), out.end(), [](const SheetCrossing& p, const SheetCrossing& q) {
		if (p.axis != q.axis) return p.axis < q.axis;
		if (p.face != q.face) return p.face < q.face;
		if (p.distance != q.distance) return p.distance < q.distance;
		if (p.sheet != q.sheet) return p.sheet < q.sheet;
		return p.triangle < q.triangle;
	});
	out.erase(std::unique(out.begin(), out.end(), [](const SheetCrossing& p, const SheetCrossing& q) {
		return p.axis == q.axis && p.face == q.face; }), out.end());
}

namespace {
// Whether cell c has a neighbour across an open face that was liquid before the step.
bool Fed(const Grid& g, std::size_t c, const std::array<std::vector<double>,3>& fraction, const std::vector<double>& old)
{
	const int idx[3] = {int(c % std::size_t(g.n[0])), int((c/std::size_t(g.n[0])) % std::size_t(g.n[1])),
		int(c/(std::size_t(g.n[0])*std::size_t(g.n[1])))};
	for (int a = 0; a < 3; ++a) for (int side = 0; side < 2; ++side) {
		int f[3] = {idx[0], idx[1], idx[2]}, nb[3] = {idx[0], idx[1], idx[2]};
		f[a] += side;
		nb[a] += side ? 1 : -1;
		if (nb[a] < 0 || nb[a] >= g.n[a]) continue;
		if (!(fraction[std::size_t(a)][g.F(a, f[0], f[1], f[2])] > 0)) continue;
		if (old[g.C(nb[0], nb[1], nb[2])] < 0) return true;
	}
	return false;
}
} // namespace

std::size_t BlockSheetLeaks(const Grid& g, const std::vector<SheetCrossing>& faces, const std::array<std::vector<double>,3>& fraction,
	const std::vector<double>& old, std::vector<double>& phi, Pool* pool)
{
	std::vector<std::size_t> cells;
	cells.reserve(2*faces.size());
	for (const SheetCrossing& x : faces) {
		const auto d = g.FaceDims(x.axis);
		int idx[3] = {int(x.face % std::size_t(d[0])), int((x.face/std::size_t(d[0])) % std::size_t(d[1])),
			int(x.face/(std::size_t(d[0])*std::size_t(d[1])))};
		cells.push_back(g.C(idx[0], idx[1], idx[2]));
		--idx[x.axis];
		cells.push_back(g.C(idx[0], idx[1], idx[2]));
	}
	std::sort(cells.begin(), cells.end());
	cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
	std::size_t kept = 0;
	for (std::size_t c : cells) {
		if (!(old[c] >= 0) || !(phi[c] < old[c])) continue;
		if (!Fed(g, c, fraction, old)) { phi[c] = old[c]; ++kept; }
	}
	// Anywhere: an air cell that turned liquid without a wet open neighbour keeps
	// its old value (the step's extrapolated velocity copies the wet side's flow
	// across a sheet to the dry side, which carried the false interface's level
	// set, near zero, away from the sheet, where the volume correction and the
	// particles' corrections turned it negative).
	const std::size_t rows = std::size_t(g.n[1])*std::size_t(g.n[2]);
	const std::size_t grain = RowGrain(g.n[0]);
	std::vector<std::size_t> part(Cloth::Chunks(rows, grain), 0);
	Cloth::ParallelFor(pool, part.size(), [&](std::size_t ch) {
		for (std::size_t r = ch*grain; r < std::min(rows, (ch + 1)*grain); ++r)
			for (int i = 0; i < g.n[0]; ++i) {
				const std::size_t c = std::size_t(i) + std::size_t(g.n[0])*r;
				if (old[c] >= 0 && phi[c] < 0 && !Fed(g, c, fraction, old)) { phi[c] = old[c]; ++part[ch]; }
			}
	});
	for (std::size_t k : part) kept += k;
	return kept;
}

bool CrossesSheets(const Grid& g, const std::vector<SheetCrossing>& faces, const V3& from, const V3& to)
{
	int a[3], b[3];
	for (int m = 0; m < 3; ++m) {
		a[m] = std::min(g.n[m] - 1, std::max(0, int(std::floor((from[std::size_t(m)] - g.origin[std::size_t(m)])/g.h))));
		b[m] = std::min(g.n[m] - 1, std::max(0, int(std::floor((to[std::size_t(m)] - g.origin[std::size_t(m)])/g.h))));
	}
	if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) return false;
	auto closed = [&](int axis, const int c[3], int step) {
		// The face between cell c and its neighbour along axis (step +-1).
		int f[3] = {c[0], c[1], c[2]};
		if (step > 0) ++f[axis];
		const std::size_t face = g.F(axis, f[0], f[1], f[2]);
		return std::binary_search(faces.begin(), faces.end(), SheetCrossing{axis, face}, [](const SheetCrossing& p, const SheetCrossing& q) {
			return p.axis != q.axis ? p.axis < q.axis : p.face < q.face; });
	};
	// Every order of the axes that differ: a path is open if none of its steps is closed.
	int axes[3], na = 0;
	for (int m = 0; m < 3; ++m) if (a[m] != b[m]) axes[na++] = m;
	std::sort(axes, axes + na);
	do {
		int c[3] = {a[0], a[1], a[2]};
		bool open = true;
		for (int k = 0; k < na && open; ++k) {
			const int m = axes[k];
			const int step = b[m] > c[m] ? 1 : -1;
			while (c[m] != b[m] && open) {
				if (closed(m, c, step)) open = false;
				c[m] += step;
			}
		}
		if (open) return false;
	} while (std::next_permutation(axes, axes + na));
	return true;
}

void SheetPressure(const LiquidSheet& sheet, const std::vector<double>& impulse, double span, std::vector<double>& pressure)
{
	const std::size_t n = sheet.Vertices();
	std::vector<double> area(3*n, 0.0);
	for (std::size_t t = 0; 3*t + 2 < sheet.triangles.size(); ++t) {
		const std::uint32_t i0 = sheet.triangles[3*t], i1 = sheet.triangles[3*t + 1], i2 = sheet.triangles[3*t + 2];
		const V3 p0 = At(sheet.x, i0);
		const V3 c = CrossP(Minus(At(sheet.x, i1), p0), Minus(At(sheet.x, i2), p0));
		for (std::uint32_t k : {i0, i1, i2}) for (int a = 0; a < 3; ++a) area[3*std::size_t(k) + std::size_t(a)] += c[std::size_t(a)]/6;
	}
	pressure.assign(n, 0.0);
	if (!(span > 0)) return;
	for (std::size_t k = 0; k < n; ++k) {
		double fa = 0, aa = 0;
		for (int a = 0; a < 3; ++a) {
			fa += impulse[3*k + std::size_t(a)]/span*area[3*k + std::size_t(a)];
			aa += area[3*k + std::size_t(a)]*area[3*k + std::size_t(a)];
		}
		pressure[k] = aa > 0 ? -fa/aa : 0.0;
	}
}

} } }
