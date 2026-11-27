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
#include <tuple>

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
			for (std::uint32_t t = 0; 3*std::size_t(t) < sheets[s].triangles.size(); ++t)
				if (sheets[s].open.empty() || !sheets[s].open[t]) work.emplace_back(std::uint32_t(s), t);
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
			// m7f: in a grid one cell thick along an axis (2-D) the liquid has no gradient along
			// it, so its force on the sheet has no component along it either: the normal is
			// projected onto the grid's plane.  Without it a strip whose two vertex rows sagged
			// unequally was pushed across the slab by the hydrostatic load (a tenth of it at a
			// 0.1 rad twist, against a cloth's weight of a few pascals), left the cell-centre line
			// and closed no face at all (docs/SIMULATION_M7F_RESULTS.md).  Only for sheets coupled
			// implicitly (the bytes of other sheets are m7e.1's).
			if (sh.implicit) {
				bool thin = false;
				for (int m = 0; m < 3; ++m) if (g.n[m] == 1 && n[std::size_t(m)] != 0) { n[std::size_t(m)] = 0; thin = true; }
				if (thin) {
					const double l2 = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
					if (!(l2 > 1e-6)) continue;   // the sheet lies in the grid's plane: it separates nothing there
					for (double& c : n) c /= l2;
				}
			}
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
					for (int m = 0; m < 3; ++m) x.normal[m] = n[std::size_t(m)];
					x.s = s;
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
	SheetCover(g, sheets, out, pool);
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
		const auto it = std::lower_bound(faces.begin(), faces.end(), SheetCrossing{axis, face}, [](const SheetCrossing& p, const SheetCrossing& q) {
			return p.axis != q.axis ? p.axis < q.axis : p.face < q.face; });
		return it != faces.end() && it->axis == axis && it->face == face && it->Barrier();   // m7i: a face mostly open passes
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

// ------------------------------------------------------------------ m7i: free-edge cover

namespace {
// The area of triangle p (2-D) inside the rectangle [x0, x1] x [y0, y1] (Sutherland-Hodgman).
double ClippedArea(const double p[3][2], const double lo[2], const double hi[2])
{
	double a[12][2], b[12][2];
	int na = 3;
	for (int i = 0; i < 3; ++i) { a[i][0] = p[i][0]; a[i][1] = p[i][1]; }
	for (int e = 0; e < 4 && na > 0; ++e) {
		const int axis = e/2;
		const bool lower = e % 2 == 0;
		const double lim = lower ? lo[axis] : hi[axis];
		int nb = 0;
		for (int i = 0; i < na; ++i) {
			const double* P = a[i];
			const double* Q = a[(i + 1) % na];
			const bool ip = lower ? P[axis] >= lim : P[axis] <= lim, iq = lower ? Q[axis] >= lim : Q[axis] <= lim;
			if (ip) { b[nb][0] = P[0]; b[nb][1] = P[1]; ++nb; }
			if (ip != iq) {
				const double t = (lim - P[axis])/(Q[axis] - P[axis]);
				b[nb][0] = P[0] + t*(Q[0] - P[0]);
				b[nb][1] = P[1] + t*(Q[1] - P[1]);
				b[nb][axis] = lim;
				++nb;
			}
		}
		na = nb;
		for (int i = 0; i < na; ++i) { a[i][0] = b[i][0]; a[i][1] = b[i][1]; }
	}
	double area = 0;
	for (int i = 0; i < na; ++i) area += a[i][0]*a[(i + 1) % na][1] - a[(i + 1) % na][0]*a[i][1];
	return 0.5*std::fabs(area);
}

// Whether the segment p q (2-D) meets the closed rectangle [lo, hi] (Liang-Barsky).
bool SegmentMeets(const double p[2], const double q[2], const double lo[2], const double hi[2])
{
	double t0 = 0, t1 = 1;
	for (int m = 0; m < 2; ++m) {
		const double d = q[m] - p[m];
		if (d == 0) { if (p[m] < lo[m] || p[m] > hi[m]) return false; continue; }
		double ta = (lo[m] - p[m])/d, tb = (hi[m] - p[m])/d;
		if (ta > tb) std::swap(ta, tb);
		t0 = std::max(t0, ta);
		t1 = std::min(t1, tb);
		if (t0 > t1) return false;
	}
	return true;
}

// Per sheet: its free edges (boundary edges not pinned at both ends) and the triangles at each vertex.
struct FreeEdges {
	std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
	std::vector<std::uint32_t> start, triangles;   // per vertex: [start[v], start[v + 1]) in triangles
};

void BuildFreeEdges(const LiquidSheet& sh, FreeEdges& f)
{
	std::vector<std::pair<std::uint32_t, std::uint32_t>> all;
	all.reserve(sh.triangles.size());
	for (std::size_t t = 0; t + 2 < sh.triangles.size(); t += 3)
		for (int a = 0; a < 3; ++a) {
			const std::uint32_t p = sh.triangles[t + std::size_t(a)], q = sh.triangles[t + std::size_t((a + 1) % 3)];
			all.emplace_back(std::min(p, q), std::max(p, q));
		}
	std::sort(all.begin(), all.end());
	for (std::size_t k = 0; k < all.size();) {
		std::size_t e = k + 1;
		while (e < all.size() && all[e] == all[k]) ++e;
		const auto& x = all[k];
		const bool pinned = x.first < sh.dyn.fixed.size() && x.second < sh.dyn.fixed.size() && sh.dyn.fixed[x.first] && sh.dyn.fixed[x.second];
		if (e - k == 1 && !pinned && x.first != x.second) f.edges.push_back(x);
		k = e;
	}
	if (f.edges.empty()) return;
	const std::size_t n = sh.Vertices();
	f.start.assign(n + 1, 0);
	for (std::uint32_t v : sh.triangles) ++f.start[std::size_t(v) + 1];
	for (std::size_t v = 0; v < n; ++v) f.start[v + 1] += f.start[v];
	f.triangles.assign(sh.triangles.size(), 0);
	std::vector<std::uint32_t> at(f.start.begin(), f.start.end() - 1);
	for (std::size_t t = 0; t + 2 < sh.triangles.size(); t += 3)
		for (int a = 0; a < 3; ++a) f.triangles[at[sh.triangles[t + std::size_t(a)]]++] = std::uint32_t(t/3);
}
} // namespace

namespace {
// The area of the sheet's projection along `axis` inside the face square [lo, hi] over h^2: the
// triangles reached from `t0` through shared vertices whose projection meets the square.
double PatchCover(const LiquidSheet& sh, const FreeEdges& f, int axis, std::uint32_t t0, const double lo[2], const double hi[2],
	double area, std::vector<std::uint32_t>& queue, std::vector<std::uint32_t>& seen)
{
	constexpr std::size_t Limit = 512;
	const int b = (axis + 1) % 3, e = (axis + 2) % 3;
	queue.assign(1, t0);
	seen.assign(1, t0);
	double covered = 0;
	for (std::size_t head = 0; head < queue.size() && queue.size() < Limit; ++head) {
		const std::uint32_t t = queue[head];
		double p[3][2];
		double blo[2] = {1e300, 1e300}, bhi[2] = {-1e300, -1e300};
		for (int m = 0; m < 3; ++m) {
			const std::size_t v = sh.triangles[3*std::size_t(t) + std::size_t(m)];
			p[m][0] = sh.x[3*v + std::size_t(b)];
			p[m][1] = sh.x[3*v + std::size_t(e)];
			for (int r = 0; r < 2; ++r) { blo[r] = std::min(blo[r], p[m][r]); bhi[r] = std::max(bhi[r], p[m][r]); }
		}
		if (blo[0] > hi[0] || bhi[0] < lo[0] || blo[1] > hi[1] || bhi[1] < lo[1]) continue;
		covered += ClippedArea(p, lo, hi);
		for (int m = 0; m < 3; ++m) {
			const std::uint32_t v = sh.triangles[3*std::size_t(t) + std::size_t(m)];
			for (std::uint32_t i = f.start[v]; i < f.start[std::size_t(v) + 1]; ++i) {
				const std::uint32_t n = f.triangles[i];
				if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
				seen.push_back(n);
				queue.push_back(n);
			}
		}
	}
	const double c = covered/area;
	return c >= 1 - 1e-9 ? 1.0 : std::max(0.0, c);
}

// FindSheetCrossings' unit normal of a triangle (projected onto the grid's plane in 2-D for an
// implicit sheet); false when it has none.
bool TriangleNormal(const Grid& g, const LiquidSheet& sh, std::uint32_t t, V3& n)
{
	const V3 p0 = At(sh.x, sh.triangles[3*std::size_t(t)]), p1 = At(sh.x, sh.triangles[3*std::size_t(t) + 1]), p2 = At(sh.x, sh.triangles[3*std::size_t(t) + 2]);
	n = CrossP(Minus(p1, p0), Minus(p2, p0));
	const double len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
	if (!(len > 0)) return false;
	for (double& c : n) c /= len;
	if (sh.implicit) {
		bool thin = false;
		for (int m = 0; m < 3; ++m) if (g.n[m] == 1 && n[std::size_t(m)] != 0) { n[std::size_t(m)] = 0; thin = true; }
		if (thin) {
			const double l2 = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
			if (!(l2 > 1e-6)) return false;
			for (double& c : n) c /= l2;
		}
	}
	return true;
}
} // namespace

void SheetCover(const Grid& g, const std::vector<LiquidSheet>& sheets, std::vector<SheetCrossing>& faces, Pool* pool)
{
	std::vector<FreeEdges> free(sheets.size());
	bool any = false;
	for (std::size_t s = 0; s < sheets.size(); ++s)
		if (sheets[s].Coupled() && !sheets[s].x.empty()) { BuildFreeEdges(sheets[s], free[s]); any = any || !free[s].edges.empty(); }
	if (!any) return;
	const double h = g.h, area = h*h;
	auto square = [&](int a, const int idx[3], double lo[2], double hi[2]) {
		const int b = (a + 1) % 3, e = (a + 2) % 3;
		lo[0] = g.origin[std::size_t(b)] + idx[b]*h;
		lo[1] = g.origin[std::size_t(e)] + idx[e]*h;
		hi[0] = lo[0] + h;
		hi[1] = lo[1] + h;
	};
	// The crossed faces whose square a free edge reaches.
	constexpr std::size_t Grain = 256;
	Cloth::ParallelFor(pool, Cloth::Chunks(faces.size(), Grain), [&](std::size_t ch) {
		std::vector<std::uint32_t> queue, seen;
		for (std::size_t k = ch*Grain; k < std::min(faces.size(), (ch + 1)*Grain); ++k) {
			SheetCrossing& c = faces[k];
			const FreeEdges& f = free[c.sheet];
			if (f.edges.empty()) continue;
			const LiquidSheet& sh = sheets[c.sheet];
			const int a = c.axis, b = (a + 1) % 3, e = (a + 2) % 3;
			const auto d = g.FaceDims(a);
			const int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
				int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
			double lo[2], hi[2];
			square(a, idx, lo, hi);
			bool near = false;
			for (const auto& x : f.edges) {
				const double p[2] = {sh.x[3*std::size_t(x.first) + std::size_t(b)], sh.x[3*std::size_t(x.first) + std::size_t(e)]};
				const double q[2] = {sh.x[3*std::size_t(x.second) + std::size_t(b)], sh.x[3*std::size_t(x.second) + std::size_t(e)]};
				if (SegmentMeets(p, q, lo, hi)) { near = true; break; }
			}
			if (near) c.cover = PatchCover(sh, f, a, c.triangle, lo, hi, area, queue, seen);
		}
	});
	// The faces beside a free edge whose cell-centre segment the sheet does not cross but whose
	// square it covers in part: the face of each lattice line a free edge's projection meets that
	// holds the edge's point nearest the line (the line's crossing, had the sheet reached it).
	// Their cover (below 1/2 for a straight edge) keeps the load continuous as an edge passes a
	// cell centre (from 0 to 1, where the crossing alone jumped from 0 to a whole face).
	// A line the sheet crosses anywhere has its crossing: only lines it does not cross get one.
	auto lineOf = [&](int a, std::size_t face) {
		const auto d = g.FaceDims(a);
		int idx[3] = {int(face % std::size_t(d[0])), int((face/std::size_t(d[0])) % std::size_t(d[1])), int(face/(std::size_t(d[0])*std::size_t(d[1])))};
		idx[a] = 0;
		return g.F(a, idx[0], idx[1], idx[2]);
	};
	std::vector<std::tuple<std::uint32_t, int, std::size_t>> lines;   // sheet, axis, line
	for (const SheetCrossing& c : faces) if (!free[c.sheet].edges.empty()) lines.emplace_back(c.sheet, c.axis, lineOf(c.axis, c.face));
	std::sort(lines.begin(), lines.end());
	std::vector<SheetCrossing> extra;
	std::vector<double> reach;   // per extra: the squared projected distance of its edge point from the line
	std::vector<std::uint32_t> queue, seen;
	for (std::size_t si = 0; si < sheets.size(); ++si) {
		const FreeEdges& f = free[si];
		const LiquidSheet& sh = sheets[si];
		for (const auto& x : f.edges) {
			// The edge's triangle (the one triangle holding both ends).
			std::uint32_t t = 0;
			bool found = false;
			for (std::uint32_t i = f.start[x.first]; i < f.start[std::size_t(x.first) + 1] && !found; ++i) {
				const std::uint32_t c = f.triangles[i];
				for (int m = 0; m < 3; ++m) if (sh.triangles[3*std::size_t(c) + std::size_t(m)] == x.second) { t = c; found = true; }
			}
			V3 n;
			if (!found || !TriangleNormal(g, sh, t, n)) continue;
			const V3 P = At(sh.x, x.first), Q = At(sh.x, x.second);
			for (int a = 0; a < 3; ++a) {
				if (g.n[a] < 2) continue;
				const int b = (a + 1) % 3, e = (a + 2) % 3;
				auto cells = [&](int m, int& first, int& last) {
					const double l = std::min(P[std::size_t(m)], Q[std::size_t(m)]), u = std::max(P[std::size_t(m)], Q[std::size_t(m)]);
					first = std::max(0, int(std::floor((l - g.origin[std::size_t(m)])/h)));
					last = std::min(g.n[m] - 1, int(std::floor((u - g.origin[std::size_t(m)])/h)));
				};
				int j0, j1, k0, k1;
				cells(b, j0, j1);
				cells(e, k0, k1);
				const double p2[2] = {P[std::size_t(b)], P[std::size_t(e)]}, q2[2] = {Q[std::size_t(b)], Q[std::size_t(e)]};
				for (int k = k0; k <= k1; ++k) for (int j = j0; j <= j1; ++j) {
					int idx[3];
					idx[a] = 0; idx[b] = j; idx[e] = k;
					double lo[2], hi[2];
					square(a, idx, lo, hi);
					if (!SegmentMeets(p2, q2, lo, hi)) continue;
					// The edge's point nearest the line (in the projection).
					const double cx = 0.5*(lo[0] + hi[0]), cy = 0.5*(lo[1] + hi[1]);
					const double dx = q2[0] - p2[0], dy = q2[1] - p2[1], len2 = dx*dx + dy*dy;
					const double tt = len2 > 0 ? std::min(1.0, std::max(0.0, ((cx - p2[0])*dx + (cy - p2[1])*dy)/len2)) : 0.0;
					const double sa = P[std::size_t(a)] + tt*(Q[std::size_t(a)] - P[std::size_t(a)]);
					const int i = int(std::floor((sa - g.origin[std::size_t(a)])/h - 0.5));
					if (i < 0 || i + 1 > g.n[a] - 1) continue;
					idx[a] = i + 1;
					SheetCrossing c;
					c.axis = a;
					c.face = g.F(a, idx[0], idx[1], idx[2]);
					if (std::binary_search(lines.begin(), lines.end(), std::make_tuple(std::uint32_t(si), a, lineOf(a, c.face)))) continue;
					c.cover = PatchCover(sh, f, a, t, lo, hi, area, queue, seen);
					if (!(c.cover > 1e-6)) continue;
					c.sheet = std::uint32_t(si);
					c.triangle = t;
					for (int m = 0; m < 3; ++m) {
						const std::uint32_t v = sh.triangles[3*std::size_t(t) + std::size_t(m)];
						c.w[m] = v == x.first ? 1 - tt : v == x.second ? tt : 0.0;
					}
					c.distance = std::fabs(sa - (g.origin[std::size_t(a)] + (i + 1)*h));
					double vn = 0;
					for (int m = 0; m < 3; ++m) {
						const V3 vm = At(sh.v, sh.triangles[3*std::size_t(t) + std::size_t(m)]);
						for (int r = 0; r < 3; ++r) vn += c.w[m]*vm[std::size_t(r)]*n[std::size_t(r)];
					}
					c.velocity = vn*n[std::size_t(a)];
					for (int m = 0; m < 3; ++m) c.normal[m] = n[std::size_t(m)];
					c.s = sa;
					c.crossed = false;
					const double ex = p2[0] + tt*dx - cx, ey = p2[1] + tt*dy - cy;
					extra.push_back(c);
					reach.push_back(ex*ex + ey*ey);
				}
			}
		}
	}
	if (extra.empty()) return;
	// One face per line: the edge point nearest the line (several edges reach a line their projection runs along).
	{
		std::vector<std::size_t> order(extra.size());
		for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
		auto key = [&](std::size_t k) { return std::make_tuple(extra[k].sheet, extra[k].axis, lineOf(extra[k].axis, extra[k].face)); };
		std::sort(order.begin(), order.end(), [&](std::size_t p, std::size_t q) {
			const auto kp = key(p), kq = key(q);
			if (kp != kq) return kp < kq;
			if (reach[p] != reach[q]) return reach[p] < reach[q];
			if (extra[p].face != extra[q].face) return extra[p].face < extra[q].face;
			return extra[p].triangle < extra[q].triangle;
		});
		std::vector<SheetCrossing> kept;
		for (std::size_t k = 0; k < order.size(); ++k)
			if (k == 0 || key(order[k]) != key(order[k - 1])) kept.push_back(extra[order[k]]);
		extra.swap(kept);
	}
	faces.insert(faces.end(), extra.begin(), extra.end());
	std::sort(faces.begin(), faces.end(), [](const SheetCrossing& p, const SheetCrossing& q) {
		if (p.axis != q.axis) return p.axis < q.axis;
		if (p.face != q.face) return p.face < q.face;
		if (p.crossed != q.crossed) return p.crossed;
		if (p.distance != q.distance) return p.distance < q.distance;
		if (p.sheet != q.sheet) return p.sheet < q.sheet;
		return p.triangle < q.triangle;
	});
	faces.erase(std::unique(faces.begin(), faces.end(), [](const SheetCrossing& p, const SheetCrossing& q) {
		return p.axis == q.axis && p.face == q.face; }), faces.end());
}

// ------------------------------------------------------------------ m7e: implicit coupling

namespace {
// A banded ordering of the stiffness pattern: reverse Cuthill-McKee from a
// vertex of least degree (ties by index), every component in turn;
// deterministic.
std::vector<std::uint32_t> BandOrder(const Cloth::BlockMatrix& a)
{
	const std::uint32_t n = a.Blocks();
	std::vector<std::uint32_t> degree(n, 0), order;
	for (std::uint32_t i = 0; i < n; ++i) degree[i] = std::uint32_t(a.RowStart()[i + 1] - a.RowStart()[i]);
	std::vector<char> seen(n, 0);
	order.reserve(n);
	for (;;) {
		std::uint32_t start = n;
		for (std::uint32_t i = 0; i < n; ++i) if (!seen[i] && (start == n || degree[i] < degree[start])) start = i;
		if (start == n) break;
		std::size_t head = order.size();
		order.push_back(start);
		seen[start] = 1;
		while (head < order.size()) {
			const std::uint32_t i = order[head++];
			std::vector<std::uint32_t> next;
			for (std::size_t k = a.RowStart()[i]; k < a.RowStart()[i + 1]; ++k) {
				const std::uint32_t j = a.Column()[k];
				if (!seen[j]) { seen[j] = 1; next.push_back(j); }
			}
			std::sort(next.begin(), next.end(), [&](std::uint32_t p, std::uint32_t q) {
				return degree[p] != degree[q] ? degree[p] < degree[q] : p < q; });
			order.insert(order.end(), next.begin(), next.end());
		}
	}
	std::reverse(order.begin(), order.end());
	return order;
}
} // namespace

bool SheetDynamics::Prepare(double dt, const std::vector<double>& v, const std::vector<SheetBlock>& extra,
	const std::vector<std::uint32_t>& triangles, std::string& error)
{
	const std::uint32_t n = std::uint32_t(mass.size());
	const bool withK = WithStiffness();
	if (matrix.Blocks() != n) {
		// The pattern: the stiffness's and every triangle's vertex pairs (the face mass couples a triangle's vertices).
		std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
		if (withK)
			for (std::uint32_t i = 0; i < n; ++i)
				for (std::size_t k = stiffness.RowStart()[i]; k < stiffness.RowStart()[i + 1]; ++k)
					if (stiffness.Column()[k] > i) pairs.emplace_back(i, stiffness.Column()[k]);
		for (std::size_t t = 0; t + 2 < triangles.size(); t += 3)
			for (int a = 0; a < 3; ++a) for (int c = a + 1; c < 3; ++c) {
				const std::uint32_t p = triangles[t + std::size_t(a)], q = triangles[t + std::size_t(c)];
				if (p != q) pairs.emplace_back(std::min(p, q), std::max(p, q));
			}
		std::sort(pairs.begin(), pairs.end());
		pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
		matrix.Build(n, pairs);
		order.clear();
	}
	Cloth::BlockMatrix& s = matrix;
	s.Zero();
	const double ks = dt*dampingStiffness + dt*dt;
	const Cloth::BlockMatrix& K = Stiffness();
	if (withK)
		for (std::uint32_t i = 0; i < n; ++i)
			for (std::size_t k = K.RowStart()[i]; k < K.RowStart()[i + 1]; ++k) {
				double* b = s.Block(s.Slot(i, K.Column()[k]));
				const double* kb = K.Block(k);
				for (int r = 0; r < 9; ++r) b[r] += ks*kb[r];
			}
	for (std::uint32_t q = 0; q < n; ++q) {
		double* b = s.Block(s.DiagonalSlot(q));
		const double m = mass[q]*(1 + dt*dampingMass);
		b[0] += m; b[4] += m; b[8] += m;
	}
	for (const SheetBlock& e : extra) {
		double* b = s.Block(s.Slot(e.i, e.j));
		for (int r = 0; r < 9; ++r) b[r] += e.b[r];
		if (e.i != e.j) {
			double* t = s.Block(s.Slot(e.j, e.i));
			for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) t[3*c + r] += e.b[3*r + c];
		}
	}
	// Pins: their velocity is given; (S + F)_fp V_p moves to the free rows' right-hand side,
	// and their rows and columns become the identity.
	pinned.assign(3*std::size_t(n), 0.0);
	for (std::uint32_t i = 0; i < n; ++i) {
		if (fixed[i]) continue;
		for (std::size_t k = s.RowStart()[i]; k < s.RowStart()[i + 1]; ++k) {
			const std::uint32_t j = s.Column()[k];
			if (!fixed[j]) continue;
			const double* b = s.Block(k);
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c) pinned[3*std::size_t(i) + std::size_t(r)] += b[3*r + c]*v[3*std::size_t(j) + std::size_t(c)];
		}
	}
	for (std::uint32_t i = 0; i < n; ++i)
		for (std::size_t k = s.RowStart()[i]; k < s.RowStart()[i + 1]; ++k) {
			const std::uint32_t j = s.Column()[k];
			if (!fixed[i] && !fixed[j]) continue;
			double* b = s.Block(k);
			for (int r = 0; r < 9; ++r) b[r] = 0;
			if (i == j) b[0] = b[4] = b[8] = 1;
		}
	if (order.size() != n) order = BandOrder(s);
	if (!factor.Factor(s, order)) {
		error = "the coupled sheet's step matrix is not positive definite (mass, damping and stiffness of the partner)";
		return false;
	}
	return true;
}

void SheetDynamics::Solve(const std::vector<double>& r, std::vector<double>& out) const
{
	std::vector<double> b = r;
	for (std::size_t q = 0; q < fixed.size(); ++q) if (fixed[q]) b[3*q] = b[3*q + 1] = b[3*q + 2] = 0;
	factor.Solve(b, out);
	for (std::size_t q = 0; q < fixed.size(); ++q) if (fixed[q]) out[3*q] = out[3*q + 1] = out[3*q + 2] = 0;
}

void SheetDynamics::Rotate(double dt, const std::vector<double>& v)
{
	if (!Corotated()) return;
	std::vector<double> p(xe.size());
	for (std::size_t k = 0; k < p.size(); ++k) p[k] = xe[k] + dt*v[k];
	elements.Rotate(x0, p);
	if (rotated.Blocks() != stiffness.Blocks()) rotated = stiffness;
	elements.Assemble(rotated);
}

void SheetDynamics::ResetElements()
{
	if (elements.Empty()) { origin.clear(); return; }
	for (std::size_t e = 0; e < elements.Size(); ++e) {
		double* r = &elements.rotation[9*e];
		for (int k = 0; k < 9; ++k) r[k] = k % 4 == 0 ? 1.0 : 0.0;
	}
	elements.Gradient(x0, x0, origin);
	rotated = stiffness;
	elements.Assemble(rotated);
}

void SheetDynamics::Rhs(double dt, const std::vector<double>& v, std::vector<double>& out) const
{
	const std::size_t n = mass.size();
	std::vector<double> d(3*n), kd;
	if (Corotated()) {
		// m7h: the corotational model's elastic gradient at xe less its value at x0 (force holds -origin).
		elements.Gradient(x0, xe, kd);
		for (std::size_t k = 0; k < 3*n; ++k) kd[k] -= origin[k];
	} else {
		for (std::size_t k = 0; k < 3*n; ++k) d[k] = xe[k] - x0[k];
		if (WithStiffness()) stiffness.Multiply(d, kd);
	}
	out.assign(3*n, 0.0);
	for (std::size_t q = 0; q < n; ++q)
		for (int a = 0; a < 3; ++a) {
			const std::size_t k = 3*q + std::size_t(a);
			out[k] = mass[q]*v[k] + dt*(force[k] - (kd.empty() ? 0.0 : kd[k]));
		}
}

void SheetDynamics::Predict(double dt, const std::vector<double>& v, const std::vector<double>& extra, std::vector<double>& out) const
{
	const std::size_t n = mass.size();
	std::vector<double> b;
	Rhs(dt, v, b);
	for (std::size_t q = 0; q < n; ++q)
		for (int a = 0; a < 3; ++a) {
			const std::size_t k = 3*q + std::size_t(a);
			b[k] = fixed[q] ? v[k] : b[k] + (extra.empty() ? 0.0 : extra[k]) - pinned[k];
		}
	factor.Solve(b, out);
	for (std::size_t q = 0; q < n; ++q) if (fixed[q]) for (int a = 0; a < 3; ++a) out[3*q + std::size_t(a)] = v[3*q + std::size_t(a)];
}

void SheetDynamics::ApplyS(double dt, const std::vector<double>& V, std::vector<double>& out) const
{
	const std::size_t n = mass.size();
	std::vector<double> kv;
	if (WithStiffness()) Stiffness().Multiply(V, kv);
	out.assign(3*n, 0.0);
	const double ks = dt*dampingStiffness + dt*dt;
	for (std::size_t q = 0; q < n; ++q)
		for (int a = 0; a < 3; ++a) {
			const std::size_t k = 3*q + std::size_t(a);
			out[k] = mass[q]*(1 + dt*dampingMass)*V[k] + (kv.empty() ? 0.0 : ks*kv[k]);
		}
}

SheetCoupling::SheetCoupling(const Grid& g, const std::vector<SheetCrossing>& faces, const std::vector<LiquidSheet>& sheets, double density,
	const std::vector<std::uint8_t>* skip)
	: g_(g), crossings_(faces), sheets_(sheets), skip_(skip && !skip->empty() ? skip : nullptr), scale_(density*g.h*g.h*g.h)
{
	impulse_.resize(sheets.size());
	r_.resize(sheets.size());
	z_.resize(sheets.size());
	for (std::size_t i = 0; i < sheets.size(); ++i)
		if (sheets[i].Coupled()) {
			impulse_[i].assign(3*sheets[i].dyn.Vertices(), 0.0);
			r_[i].assign(impulse_[i].size(), 0.0);
			z_[i].assign(impulse_[i].size(), 0.0);
		}
}

void SheetCoupling::Begin(const std::vector<std::uint8_t>& active)
{
	faces_.clear();
	for (std::size_t k = 0; k < crossings_.size(); ++k) {
		const SheetCrossing& c = crossings_[k];
		const LiquidSheet& sh = sheets_[c.sheet];
		if (!sh.Coupled() || (skip_ && (*skip_)[k])) continue;
		const auto d = g_.FaceDims(c.axis);
		int idx[3] = {int(c.face % std::size_t(d[0])), int((c.face/std::size_t(d[0])) % std::size_t(d[1])),
			int(c.face/(std::size_t(d[0])*std::size_t(d[1])))};
		Face f;
		f.R = g_.C(idx[0], idx[1], idx[2]);
		--idx[c.axis];
		f.L = g_.C(idx[0], idx[1], idx[2]);
		f.aL = active[f.L] != 0;
		f.aR = active[f.R] != 0;
		if (!f.aL && !f.aR) continue;
		f.sheet = c.sheet;
		for (int m = 0; m < 3; ++m) {
			f.v[m] = sh.triangles[3*std::size_t(c.triangle) + std::size_t(m)];
			for (int k = 0; k < 3; ++k) f.c[m][k] = c.w[m]*c.normal[c.axis]*c.normal[k]*c.cover;   // m7i: the closed part c
		}
		faces_.push_back(f);
	}
}

void SheetCoupling::Measure(const std::vector<double>& q, std::vector<std::vector<double>>& out) const
{
	out.resize(sheets_.size());
	for (std::size_t i = 0; i < sheets_.size(); ++i) out[i].assign(sheets_[i].Coupled() ? 3*sheets_[i].dyn.Vertices() : 0, 0.0);
	for (const Face& f : faces_) {
		const double jump = scale_*((f.aL ? q[f.L] : 0.0) - (f.aR ? q[f.R] : 0.0));
		std::vector<double>& o = out[f.sheet];
		for (int m = 0; m < 3; ++m) for (int k = 0; k < 3; ++k) o[3*std::size_t(f.v[m]) + std::size_t(k)] += f.c[m][k]*jump;
	}
}

void SheetCoupling::ApplyAdd(const std::vector<double>& q, std::vector<double>& out) const
{
	if (faces_.empty()) return;
	for (std::size_t i = 0; i < sheets_.size(); ++i) std::fill(r_[i].begin(), r_[i].end(), 0.0);
	for (const Face& f : faces_) {
		const double jump = (f.aL ? q[f.L] : 0.0) - (f.aR ? q[f.R] : 0.0);
		std::vector<double>& r = r_[f.sheet];
		for (int m = 0; m < 3; ++m) for (int k = 0; k < 3; ++k) r[3*std::size_t(f.v[m]) + std::size_t(k)] += f.c[m][k]*jump;
	}
	for (std::size_t i = 0; i < sheets_.size(); ++i) if (!r_[i].empty()) sheets_[i].dyn.Solve(r_[i], z_[i]);
	for (const Face& f : faces_) {
		const std::vector<double>& z = z_[f.sheet];
		double gz = 0;
		for (int m = 0; m < 3; ++m) for (int k = 0; k < 3; ++k) gz += f.c[m][k]*z[3*std::size_t(f.v[m]) + std::size_t(k)];
		if (f.aL) out[f.L] += scale_*gz;
		if (f.aR) out[f.R] -= scale_*gz;
	}
}

void SheetCoupling::Finish(const std::vector<double>& q)
{
	Measure(q, impulse_);
}

void SheetCrossingVelocities(const std::vector<LiquidSheet>& sheets, const std::vector<std::vector<double>>& v,
	std::vector<SheetCrossing>& faces)
{
	for (SheetCrossing& c : faces) {
		if (!sheets[c.sheet].Coupled() || c.sheet >= v.size() || v[c.sheet].empty()) continue;
		const LiquidSheet& sh = sheets[c.sheet];
		// FindSheetCrossings' arithmetic, so that faces closed again from a restored state carry the same bits.
		const V3 v0 = At(v[c.sheet], sh.triangles[3*std::size_t(c.triangle)]), v1 = At(v[c.sheet], sh.triangles[3*std::size_t(c.triangle) + 1]),
			v2 = At(v[c.sheet], sh.triangles[3*std::size_t(c.triangle) + 2]);
		double vn = 0;
		for (int m = 0; m < 3; ++m)
			vn += (c.w[0]*v0[std::size_t(m)] + c.w[1]*v1[std::size_t(m)] + c.w[2]*v2[std::size_t(m)])*c.normal[m];
		c.velocity = vn*c.normal[c.axis];
	}
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
